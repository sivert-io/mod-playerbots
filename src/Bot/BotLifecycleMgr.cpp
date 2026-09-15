/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotLifecycleMgr.h"

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>

namespace
{
uint32 Now() { return static_cast<uint32>(GameTime::GetGameTime().count()); }

constexpr uint32 ARRIVAL_STEP_DELAY = 3;
constexpr uint32 ARRIVAL_SPACING = 20;          // minimum seconds between two arrivals
constexpr uint32 MAINTENANCE_INTERVAL = 60;
constexpr uint32 WEEK_CHECK_INTERVAL = 10 * MINUTE;
constexpr uint32 MIN_OFFLINE_GAP = 45 * MINUTE;
}  // namespace

char const* BotLifecycleMgr::ChronotypeName(BotChronotype type)
{
    switch (type)
    {
        case BotChronotype::Daytime: return "daytime";
        case BotChronotype::Evening: return "evening";
        case BotChronotype::NightOwl: return "night_owl";
        default: return "any";
    }
}

char const* BotLifecycleMgr::PlaystyleName(uint8 style)
{
    switch (style)
    {
        case BOT_STYLE_QUESTER: return "quester";
        case BOT_STYLE_GRINDER: return "grinder";
        case BOT_STYLE_CRAFTER: return "crafter";
        case BOT_STYLE_EXPLORER: return "explorer";
        default: return "social";
    }
}

char const* BotLifecycleMgr::StatusName(BotLifeStatus status)
{
    switch (status)
    {
        case BotLifeStatus::Break: return "break";
        case BotLifeStatus::Quit: return "quit";
        default: return "active";
    }
}

void BotLifecycleMgr::CreateTables()
{
    // Also shipped as data/sql/playerbots/updates/2026_09_15_00_astro_bot_lifecycle.sql; created here as
    // well so the feature works when Playerbots.Updates.EnableDatabases is off.
    PlayerbotsDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS `astro_bot_persona` ("
        "`guid` INT UNSIGNED NOT NULL,"
        "`account_id` INT UNSIGNED NOT NULL,"
        "`arrived_at` INT UNSIGNED NOT NULL,"
        "`race` TINYINT UNSIGNED NOT NULL,"
        "`class` TINYINT UNSIGNED NOT NULL,"
        "`chronotype` VARCHAR(16) NOT NULL,"
        "`sessions_per_week` TINYINT UNSIGNED NOT NULL,"
        "`session_minutes` SMALLINT UNSIGNED NOT NULL,"
        "`playstyle` VARCHAR(16) NOT NULL,"
        "`style_quester` TINYINT UNSIGNED NOT NULL,"
        "`style_grinder` TINYINT UNSIGNED NOT NULL,"
        "`style_crafter` TINYINT UNSIGNED NOT NULL,"
        "`style_explorer` TINYINT UNSIGNED NOT NULL,"
        "`style_social` TINYINT UNSIGNED NOT NULL,"
        "`break_factor` FLOAT NOT NULL DEFAULT 1,"
        "`status` VARCHAR(16) NOT NULL DEFAULT 'active',"
        "`break_until` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`quit_at` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`next_login_at` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`last_login_at` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`last_logout_at` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`sessions_played` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`breaks_taken` SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
        "PRIMARY KEY (`guid`),"
        "UNIQUE KEY `idx_account` (`account_id`),"
        "KEY `idx_status` (`status`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Astro Realm: persona and lifecycle of each random bot'");

    PlayerbotsDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS `astro_bot_arrivals` ("
        "`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,"
        "`week_start` INT UNSIGNED NOT NULL,"
        "`scheduled_at` INT UNSIGNED NOT NULL,"
        "`status` VARCHAR(16) NOT NULL DEFAULT 'pending',"
        "`account_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`guid` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`created_at` INT UNSIGNED NOT NULL DEFAULT 0,"
        "PRIMARY KEY (`id`),"
        "KEY `idx_status_time` (`status`, `scheduled_at`),"
        "KEY `idx_week` (`week_start`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Astro Realm: planned and completed bot arrivals'");
}

void BotLifecycleMgr::Init()
{
    if (!sPlayerbotAIConfig.lifecycleEnabled)
    {
        if (sPlayerbotAIConfig.arrivalsEnabled)
            LOG_ERROR("playerbots", "AiPlayerbot.Arrivals.Enable needs AiPlayerbot.Lifecycle.Enable = 1, arrivals are off");
        return;
    }

    uint32 const now = Now();
    CreateTables();
    LoadPersonas(now);

    // Next free account number for the random bot prefix
    std::string const prefix = sPlayerbotAIConfig.randomBotAccountPrefix;
    if (QueryResult result = LoginDatabase.Query("SELECT username FROM account WHERE username LIKE '{}%%'", prefix))
    {
        do
        {
            std::string name = result->Fetch()[0].Get<std::string>();
            std::string suffix = name.substr(std::min(name.size(), prefix.size()));
            if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), ::isdigit) && suffix.size() < 9)
                nextAccountIndex = std::max<uint32>(nextAccountIndex, std::stoul(suffix) + 1);
        } while (result->NextRow());
    }

    if (sPlayerbotAIConfig.arrivalsEnabled)
    {
        // Accounts are appended at runtime while bot AI threads read the list: reserve up front so
        // push_back never reallocates.
        sPlayerbotAIConfig.randomBotAccounts.reserve(sPlayerbotAIConfig.randomBotAccounts.size() +
                                                     sPlayerbotAIConfig.arrivalsCap + 256);

        // Arrivals that were being created when the server stopped are retried
        PlayerbotsDatabase.DirectExecute("UPDATE astro_bot_arrivals SET status = 'pending' WHERE status = 'creating'");
        if (QueryResult result = PlayerbotsDatabase.Query(
                "SELECT id, scheduled_at FROM astro_bot_arrivals WHERE status = 'pending' ORDER BY scheduled_at"))
        {
            do
            {
                Field* fields = result->Fetch();
                pendingArrivals.emplace_back(fields[0].Get<uint32>(), fields[1].Get<uint32>());
            } while (result->NextRow());
        }
    }

    initialized = true;

    uint32 withoutPersona = 0;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM characters c WHERE c.account IN (SELECT id FROM {}.account WHERE username LIKE '{}%%')",
            LoginDatabase.GetConnectionInfo()->database, prefix))
        withoutPersona = static_cast<uint32>(result->Fetch()[0].Get<uint64>());
    withoutPersona = withoutPersona > personas.size() ? withoutPersona - personas.size() : 0;

    LOG_INFO("server.loading", ">> Bot lifecycle: {} personas loaded, {} pending arrivals, next account {}{}", personas.size(),
             pendingArrivals.size(), prefix, nextAccountIndex);
    if (withoutPersona)
        LOG_WARN("playerbots", "Bot lifecycle: {} random bot characters have no persona and will not log in "
                               "(run the bot wipe or disable AiPlayerbot.Lifecycle.Enable)", withoutPersona);
}

void BotLifecycleMgr::LoadPersonas(uint32 now)
{
    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT guid, account_id, arrived_at, race, class, chronotype, sessions_per_week, session_minutes, "
        "style_quester, style_grinder, style_crafter, style_explorer, style_social, break_factor, status, "
        "break_until, quit_at, next_login_at, last_login_at, last_logout_at, sessions_played, breaks_taken "
        "FROM astro_bot_persona");
    if (!result)
        return;

    std::vector<BotPersona*> changed;
    std::unique_lock lock(personaLock);
    do
    {
        Field* f = result->Fetch();
        BotPersona p;
        p.guid = f[0].Get<uint32>();
        p.accountId = f[1].Get<uint32>();
        p.arrivedAt = f[2].Get<uint32>();
        p.race = f[3].Get<uint8>();
        p.cls = f[4].Get<uint8>();
        std::string const chronotype = f[5].Get<std::string>();
        p.chronotype = chronotype == "daytime"     ? BotChronotype::Daytime
                       : chronotype == "evening"   ? BotChronotype::Evening
                       : chronotype == "night_owl" ? BotChronotype::NightOwl
                                                   : BotChronotype::Any;
        p.sessionsPerWeek = std::max<uint8>(1, f[6].Get<uint8>());
        p.sessionMinutes = std::max<uint16>(10, f[7].Get<uint16>());
        for (uint8 i = 0; i < BOT_STYLE_COUNT; ++i)
            p.styles[i] = f[8 + i].Get<uint8>();
        p.breakFactor = f[13].Get<float>();
        std::string const status = f[14].Get<std::string>();
        p.status = status == "quit" ? BotLifeStatus::Quit : status == "break" ? BotLifeStatus::Break : BotLifeStatus::Active;
        p.breakUntil = f[15].Get<uint32>();
        p.quitAt = f[16].Get<uint32>();
        p.nextLoginAt = f[17].Get<uint32>();
        p.lastLoginAt = f[18].Get<uint32>();
        p.lastLogoutAt = f[19].Get<uint32>();
        p.sessionsPlayed = f[20].Get<uint32>();
        p.breaksTaken = f[21].Get<uint16>();
        personas[p.guid] = p;
    } while (result->NextRow());

    // Fix up schedules after a restart
    for (auto& [guid, p] : personas)
    {
        if (p.status == BotLifeStatus::Quit)
            continue;

        uint32 const cycle = 7 * DAY / std::max<uint32>(1, p.sessionsPerWeek);
        if (p.lastLoginAt > p.lastLogoutAt || !p.nextLoginAt)
        {
            // Was online when the server stopped: reconnects shortly, like a real player would
            p.lastLogoutAt = now;
            if (p.status == BotLifeStatus::Active)
                p.nextLoginAt = now + urand(MINUTE, 10 * MINUTE);
            changed.push_back(&p);
        }
        else if (p.status == BotLifeStatus::Active && p.nextLoginAt + 2 * HOUR < now)
        {
            // Missed its login while the server was down: pick a new time within one play cycle
            p.nextLoginAt = AlignToChronotype(p, now + uint32(frand(0.0f, 1.0f) * cycle), now + 5 * MINUTE,
                                              std::min<uint32>(12 * HOUR, cycle / 3));
            changed.push_back(&p);
        }
    }

    for (BotPersona* p : changed)
        SavePersona(*p);
}

void BotLifecycleMgr::InsertPersona(BotPersona const& p) const
{
    uint8 primary = 0;
    for (uint8 i = 1; i < BOT_STYLE_COUNT; ++i)
        if (p.styles[i] > p.styles[primary])
            primary = i;

    PlayerbotsDatabase.Execute(
        "INSERT INTO astro_bot_persona (guid, account_id, arrived_at, race, class, chronotype, sessions_per_week, "
        "session_minutes, playstyle, style_quester, style_grinder, style_crafter, style_explorer, style_social, "
        "break_factor, status, break_until, quit_at, next_login_at, last_login_at, last_logout_at, sessions_played, "
        "breaks_taken) VALUES ({}, {}, {}, {}, {}, '{}', {}, {}, '{}', {}, {}, {}, {}, {}, {}, '{}', {}, {}, {}, {}, {}, {}, {})",
        p.guid, p.accountId, p.arrivedAt, p.race, p.cls, ChronotypeName(p.chronotype), p.sessionsPerWeek,
        p.sessionMinutes, PlaystyleName(primary), p.styles[0], p.styles[1], p.styles[2], p.styles[3], p.styles[4],
        p.breakFactor, StatusName(p.status), p.breakUntil, p.quitAt, p.nextLoginAt, p.lastLoginAt, p.lastLogoutAt,
        p.sessionsPlayed, p.breaksTaken);
}

void BotLifecycleMgr::SavePersona(BotPersona const& p) const
{
    PlayerbotsDatabase.Execute(
        "UPDATE astro_bot_persona SET status = '{}', break_until = {}, quit_at = {}, next_login_at = {}, "
        "last_login_at = {}, last_logout_at = {}, sessions_played = {}, breaks_taken = {} WHERE guid = {}",
        StatusName(p.status), p.breakUntil, p.quitAt, p.nextLoginAt, p.lastLoginAt, p.lastLogoutAt, p.sessionsPlayed,
        p.breaksTaken, p.guid);
}

BotPersona BotLifecycleMgr::GeneratePersona(uint32 guid, uint32 accountId, uint8 race, uint8 cls, uint32 now)
{
    BotPersona p;
    p.guid = guid;
    p.accountId = accountId;
    p.arrivedAt = now;
    p.race = race;
    p.cls = cls;

    uint32 roll = urand(0, 99);
    p.chronotype = roll < 20 ? BotChronotype::Daytime
                   : roll < 65 ? BotChronotype::Evening
                   : roll < 85 ? BotChronotype::NightOwl
                               : BotChronotype::Any;

    // Sessions per week from the configured weighted ranges
    auto const& ranges = sPlayerbotAIConfig.lifecycleSessionsPerWeek;
    float total = 0.0f;
    for (auto const& r : ranges)
        total += r.second;
    float pick = frand(0.0f, total);
    auto chosen = ranges.back().first;
    for (auto const& r : ranges)
    {
        pick -= r.second;
        if (pick <= 0.0f)
        {
            chosen = r.first;
            break;
        }
    }
    p.sessionsPerWeek = uint8(std::clamp<uint32>(urand(chosen.first, chosen.second), 1, 28));

    // Typical session length: players who play more often also tend to play longer
    std::lognormal_distribution<float> sessionNoise(0.0f, 0.35f);
    float minutes = sPlayerbotAIConfig.lifecycleSessionMinutesMedian * (0.7f + 0.06f * p.sessionsPerWeek) *
                    sessionNoise(rng);
    p.sessionMinutes = uint16(std::clamp(minutes, 20.0f, 360.0f));

    // Playstyle weights (percent): one dominant style, the rest split randomly
    static constexpr std::array<uint32, BOT_STYLE_COUNT> primaryWeights{{40, 15, 10, 15, 20}};
    uint32 styleRoll = urand(1, 100);
    uint8 primary = BOT_STYLE_SOCIAL;
    for (uint8 i = 0; i < BOT_STYLE_COUNT; ++i)
    {
        if (styleRoll <= primaryWeights[i])
        {
            primary = i;
            break;
        }
        styleRoll -= primaryWeights[i];
    }
    uint32 primaryShare = urand(35, 60);
    std::array<float, BOT_STYLE_COUNT> others{};
    float othersSum = 0.0f;
    for (uint8 i = 0; i < BOT_STYLE_COUNT; ++i)
    {
        others[i] = i == primary ? 0.0f : frand(0.1f, 1.0f);
        othersSum += others[i];
    }
    uint32 assigned = 0;
    for (uint8 i = 0; i < BOT_STYLE_COUNT; ++i)
    {
        if (i == primary)
            continue;
        p.styles[i] = uint8(std::floor(others[i] / othersSum * (100 - primaryShare)));
        assigned += p.styles[i];
    }
    p.styles[primary] = uint8(100 - assigned);

    std::lognormal_distribution<float> breakNoise(0.0f, 0.5f);
    p.breakFactor = std::clamp(breakNoise(rng), 0.25f, 3.0f);

    p.status = BotLifeStatus::Active;
    return p;
}

// Moves `target` to a local hour drawn from the bot's chronotype, by at most `maxShift` seconds,
// and never before `earliest`.
uint32 BotLifecycleMgr::AlignToChronotype(BotPersona const& p, uint32 target, uint32 earliest, uint32 maxShift)
{
    if (p.chronotype == BotChronotype::Any)
        return std::max(target, earliest);

    float peak = p.chronotype == BotChronotype::Daytime ? 11.0f : p.chronotype == BotChronotype::Evening ? 20.0f : 23.5f;
    std::normal_distribution<float> hourDist(peak, 3.0f);
    float hour = std::fmod(hourDist(rng) + 48.0f, 24.0f);

    float localHour = RandomPlayerbotMgr::GetPopulationLocalHour(target);
    float delta = std::fmod(hour - localHour + 48.0f, 24.0f);
    if (delta > 12.0f)
        delta -= 24.0f;

    int64 shift = int64(delta * HOUR);
    shift = std::clamp<int64>(shift, -int64(maxShift), int64(maxShift));
    int64 result = int64(target) + shift;
    while (result < int64(earliest))
        result += DAY;
    return uint32(result);
}

uint32 BotLifecycleMgr::ScheduleNextLogin(BotPersona const& p, uint32 from, uint32 sessionSeconds)
{
    uint32 const cycle = 7 * DAY / std::max<uint32>(1, p.sessionsPerWeek);
    uint32 gap = uint32(cycle * frand(0.5f, 1.5f));
    gap = gap > sessionSeconds + MIN_OFFLINE_GAP ? gap - sessionSeconds : MIN_OFFLINE_GAP;
    return AlignToChronotype(p, from + gap, from + MIN_OFFLINE_GAP, std::min<uint32>(12 * HOUR, cycle / 3));
}

bool BotLifecycleMgr::HasPersona(uint32 guid) const
{
    std::shared_lock lock(personaLock);
    return personas.contains(guid);
}

std::vector<BotLoginCandidate> BotLifecycleMgr::GetDueCandidates(uint32 now) const
{
    std::vector<BotLoginCandidate> due;
    std::shared_lock lock(personaLock);
    for (auto const& [guid, p] : personas)
    {
        if (p.status == BotLifeStatus::Active && p.nextLoginAt && p.nextLoginAt <= now)
            due.push_back({guid, p.accountId, p.race, p.cls});
    }

    // Longest waiting first, so a full server (MaxOnline) lets bots in fairly
    std::sort(due.begin(), due.end(), [this](BotLoginCandidate const& a, BotLoginCandidate const& b)
              { return personas.at(a.guid).nextLoginAt < personas.at(b.guid).nextLoginAt; });
    return due;
}

uint32 BotLifecycleMgr::StartSession(uint32 guid, uint32 now)
{
    std::unique_lock lock(personaLock);
    auto it = personas.find(guid);
    if (it == personas.end())
        return HOUR;

    BotPersona& p = it->second;
    p.lastLoginAt = now;
    p.nextLoginAt = 0;
    SavePersona(p);

    std::lognormal_distribution<float> noise(0.0f, 0.35f);
    float seconds = p.sessionMinutes * MINUTE * noise(rng);
    return uint32(std::clamp(seconds, float(15 * MINUTE), float(8 * HOUR)));
}

void BotLifecycleMgr::EndSession(uint32 guid, uint32 now)
{
    std::unique_lock lock(personaLock);
    auto it = personas.find(guid);
    if (it == personas.end())
        return;

    BotPersona& p = it->second;
    if (p.status == BotLifeStatus::Quit)
        return;

    uint32 const sessionSeconds =
        p.lastLoginAt && p.lastLoginAt <= now ? now - p.lastLoginAt : uint32(p.sessionMinutes) * MINUTE;
    p.lastLogoutAt = now;
    ++p.sessionsPlayed;

    float const sessionsPerYear = std::max(1.0f, p.sessionsPerWeek * 52.0f);

    // Rare permanent quit: annual rate turned into a per-session probability
    float annualQuit = std::min(0.95f, sPlayerbotAIConfig.lifecycleQuitPercentPerYear / 100.0f *
                                           std::min(3.0f, std::sqrt(p.breakFactor)));
    float quitChance = 1.0f - std::pow(1.0f - annualQuit, 1.0f / sessionsPerYear);
    if (annualQuit > 0.0f && frand(0.0f, 1.0f) < quitChance)
    {
        p.status = BotLifeStatus::Quit;
        p.quitAt = now;
        p.nextLoginAt = 0;
        SavePersona(p);
        LOG_INFO("playerbots", "Bot lifecycle: bot #{} quit after {} sessions", guid, p.sessionsPlayed);
        return;
    }

    // Breaks, longest first: each configured kind happens `perYear * breakFactor` times per year on average
    auto breaks = sPlayerbotAIConfig.lifecycleBreaks;
    std::sort(breaks.begin(), breaks.end(), [](auto const& a, auto const& b) { return a.first.second > b.first.second; });
    for (auto const& [days, perYear] : breaks)
    {
        if (perYear <= 0.0f || days.second == 0)
            continue;

        if (frand(0.0f, 1.0f) < perYear * p.breakFactor / sessionsPerYear)
        {
            uint32 duration = urand(days.first, days.second) * DAY + urand(0, DAY);
            p.status = BotLifeStatus::Break;
            p.breakUntil = now + duration;
            ++p.breaksTaken;
            uint32 const cycle = 7 * DAY / std::max<uint32>(1, p.sessionsPerWeek);
            p.nextLoginAt = AlignToChronotype(p, p.breakUntil, p.breakUntil, std::min<uint32>(12 * HOUR, cycle / 3));
            SavePersona(p);
            LOG_DEBUG("playerbots", "Bot lifecycle: bot #{} takes a {} day break", guid, duration / DAY);
            return;
        }
    }

    p.nextLoginAt = ScheduleNextLogin(p, now, sessionSeconds);
    SavePersona(p);
}

void BotLifecycleMgr::OnLoginFailed(uint32 guid, uint32 now)
{
    std::unique_lock lock(personaLock);
    auto it = personas.find(guid);
    if (it == personas.end() || it->second.status == BotLifeStatus::Quit)
        return;

    it->second.nextLoginAt = now + 15 * MINUTE;
    SavePersona(it->second);
}

uint32 BotLifecycleMgr::GetRpgStatusWeight(uint32 guid, NewRpgStatus status, uint32 baseWeight) const
{
    if (!baseWeight || !IsActive() || sPlayerbotAIConfig.lifecyclePlaystyleInfluence <= 0.0f)
        return baseWeight;

    float share;
    {
        std::shared_lock lock(personaLock);
        auto it = personas.find(guid);
        if (it == personas.end())
            return baseWeight;

        auto const& s = it->second.styles;
        switch (status)
        {
            case RPG_DO_QUEST: share = s[BOT_STYLE_QUESTER]; break;
            case RPG_GO_GRIND:
            case RPG_GO_CAMP: share = s[BOT_STYLE_GRINDER]; break;
            case RPG_WANDER_RANDOM:
            case RPG_TRAVEL_FLIGHT: share = s[BOT_STYLE_EXPLORER]; break;
            case RPG_WANDER_NPC: share = (s[BOT_STYLE_CRAFTER] + s[BOT_STYLE_SOCIAL]) / 2.0f; break;
            case RPG_REST:
            case RPG_SOCIAL_AFK: share = s[BOT_STYLE_SOCIAL]; break;
            default: return baseWeight;
        }
    }

    // An even split (20% each) keeps the configured weight; a 60% style weighs 2x, a 5% style 0.6x
    float multiplier = 0.5f + 2.5f * (share / 100.0f);
    multiplier = 1.0f + sPlayerbotAIConfig.lifecyclePlaystyleInfluence * (multiplier - 1.0f);
    return std::max<uint32>(1, uint32(std::lround(baseWeight * multiplier)));
}

void BotLifecycleMgr::MaintainPersonas(uint32 now)
{
    std::unique_lock lock(personaLock);
    for (auto& [guid, p] : personas)
    {
        if (p.status == BotLifeStatus::Break && now >= p.breakUntil)
        {
            p.status = BotLifeStatus::Active;
            if (!p.nextLoginAt)
                p.nextLoginAt = now + urand(MINUTE, HOUR);
            SavePersona(p);
        }
    }
}

std::string BotLifecycleMgr::GetStatsLine(uint32 now) const
{
    uint32 active = 0, onBreak = 0, quit = 0, due = 0, inSession = 0;
    {
        std::shared_lock lock(personaLock);
        for (auto const& [guid, p] : personas)
        {
            switch (p.status)
            {
                case BotLifeStatus::Active:
                    ++active;
                    if (!p.nextLoginAt)
                        ++inSession;
                    else if (p.nextLoginAt <= now)
                        ++due;
                    break;
                case BotLifeStatus::Break: ++onBreak; break;
                case BotLifeStatus::Quit: ++quit; break;
            }
        }
    }

    std::ostringstream out;
    out << "personas " << (active + onBreak + quit) << " (active " << active << ", in session " << inSession
        << ", waiting to log in " << due << ", on break " << onBreak << ", quit " << quit << "), pending arrivals "
        << pendingArrivals.size();
    return out.str();
}

uint32 BotLifecycleMgr::GetWeekStart(uint32 now) const
{
    int64 const offset = RandomPlayerbotMgr::GetPopulationUtcOffset(now);
    int64 const local = int64(now) + offset;
    int64 const days = local / DAY;
    int64 const weekday = (days + 3) % 7;  // 1970-01-01 was a Thursday; Monday = 0
    return uint32((days - weekday) * DAY - offset);
}

float BotLifecycleMgr::SampleArrivalHour()
{
    auto const& points = sPlayerbotAIConfig.arrivalsHourPoints;
    float maxWeight = 0.0f;
    for (auto const& point : points)
        maxWeight = std::max(maxWeight, point.second);
    if (maxWeight <= 0.0f)
        return frand(0.0f, 24.0f);

    auto weightAt = [&points](float hour)
    {
        std::pair<float, float> prev = points.back();
        prev.first -= 24.0f;
        std::pair<float, float> next = points.front();
        next.first += 24.0f;
        for (auto const& point : points)
        {
            if (point.first <= hour)
                prev = point;
            else
            {
                next = point;
                break;
            }
        }
        float weight = prev.second;
        if (next.first > prev.first)
            weight += (next.second - prev.second) * (hour - prev.first) / (next.first - prev.first);
        return weight;
    };

    for (uint32 attempt = 0; attempt < 100; ++attempt)
    {
        float hour = frand(0.0f, 24.0f);
        if (frand(0.0f, maxWeight) <= weightAt(hour))
            return hour;
    }
    return frand(0.0f, 24.0f);
}

// Plans this week's arrivals once: a budget of PerYear * 7 / 365 (+-15%), spread over the days with
// weights WeekdayWeights[d] * Gamma(DayShape) so some days get many and others none, each at a
// sign-up hour drawn from HourWeights (local time). Slots already in the past are dropped.
void BotLifecycleMgr::EnsureWeekPlan(uint32 now)
{
    uint32 const weekStart = GetWeekStart(now);
    if (weekStart == plannedWeekStart)
        return;

    if (QueryResult result = PlayerbotsDatabase.Query("SELECT COUNT(*) FROM astro_bot_arrivals WHERE week_start = {}", weekStart))
    {
        if (result->Fetch()[0].Get<uint64>() > 0)
        {
            plannedWeekStart = weekStart;
            return;
        }
    }

    float const mean = sPlayerbotAIConfig.arrivalsPerYear * 7.0f / 365.25f;
    float const noisy = mean * frand(0.85f, 1.15f);
    uint32 budget = uint32(noisy);
    if (frand(0.0f, 1.0f) < noisy - float(budget))
        ++budget;

    std::gamma_distribution<float> gamma(sPlayerbotAIConfig.arrivalsDayShape, 1.0f);
    std::array<float, 7> dayWeights{};
    float total = 0.0f;
    for (uint32 d = 0; d < 7; ++d)
    {
        dayWeights[d] = sPlayerbotAIConfig.arrivalsWeekdayWeights[d] * gamma(rng);
        total += dayWeights[d];
    }

    std::array<uint32, 7> perDay{};
    std::vector<uint32> times;
    for (uint32 i = 0; i < budget && total > 0.0f; ++i)
    {
        float roll = frand(0.0f, total);
        uint32 day = 6;
        for (uint32 d = 0; d < 7; ++d)
        {
            roll -= dayWeights[d];
            if (roll <= 0.0f)
            {
                day = d;
                break;
            }
        }
        ++perDay[day];

        uint32 const at = weekStart + day * DAY + uint32(SampleArrivalHour() * HOUR);
        if (at >= now)
            times.push_back(at);
    }

    std::ostringstream sql;
    if (!times.empty())
    {
        std::sort(times.begin(), times.end());
        sql << "INSERT INTO astro_bot_arrivals (week_start, scheduled_at, status) VALUES ";
        for (size_t i = 0; i < times.size(); ++i)
            sql << (i ? "," : "") << "(" << weekStart << "," << times[i] << ",'pending')";
    }
    else
    {
        // Marker so the week is not planned again after a restart
        sql << "INSERT INTO astro_bot_arrivals (week_start, scheduled_at, status) VALUES (" << weekStart << ","
            << weekStart << ",'empty')";
    }
    PlayerbotsDatabase.DirectExecute(sql.str());

    pendingArrivals.clear();
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT id, scheduled_at FROM astro_bot_arrivals WHERE status = 'pending' ORDER BY scheduled_at"))
    {
        do
        {
            Field* fields = result->Fetch();
            pendingArrivals.emplace_back(fields[0].Get<uint32>(), fields[1].Get<uint32>());
        } while (result->NextRow());
    }

    plannedWeekStart = weekStart;
    LOG_INFO("playerbots", "Arrivals: week plan {} bots (Mon..Sun {} {} {} {} {} {} {}), {} still ahead", budget,
             perDay[0], perDay[1], perDay[2], perDay[3], perDay[4], perDay[5], perDay[6], times.size());
}

void BotLifecycleMgr::FinishArrival(char const* status)
{
    PlayerbotsDatabase.Execute(
        "UPDATE astro_bot_arrivals SET status = '{}', account_id = {}, guid = {}, created_at = {} WHERE id = {}", status,
        activeArrival.accountId, activeArrival.guid, Now(), activeArrival.rowId);
    hasActiveArrival = false;
    activeArrival = PendingArrival();
    nextArrivalStep = Now() + ARRIVAL_SPACING;
}

void BotLifecycleMgr::ProcessArrivals(uint32 now)
{
    if (now < nextArrivalStep)
        return;

    if (!hasActiveArrival)
    {
        if (pendingArrivals.empty() || pendingArrivals.front().second > now)
            return;

        activeArrival = PendingArrival();
        activeArrival.rowId = pendingArrivals.front().first;
        pendingArrivals.pop_front();
        hasActiveArrival = true;

        size_t total;
        {
            std::shared_lock lock(personaLock);
            total = personas.size();
        }
        if (total >= sPlayerbotAIConfig.arrivalsCap)
        {
            FinishArrival("capped");
            return;
        }

        PlayerbotsDatabase.Execute("UPDATE astro_bot_arrivals SET status = 'creating' WHERE id = {}", activeArrival.rowId);
    }

    PendingArrival& a = activeArrival;
    switch (a.stage)
    {
        case 0:  // account
        {
            std::string const username = sPlayerbotAIConfig.randomBotAccountPrefix + std::to_string(nextAccountIndex++);
            if (AccountMgr::GetId(username))
                return;  // taken, try the next number on the next step

            static constexpr char const* chars = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
            std::string password;
            for (uint32 i = 0; i < 16; ++i)
                password += chars[urand(0, 55)];

            if (sAccountMgr->CreateAccount(username, password) != AOR_OK)
            {
                if (++a.attempts >= 5)
                    FinishArrival("failed");
                return;
            }

            a.username = username;
            a.stage = 1;
            a.attempts = 0;
            nextArrivalStep = now + ARRIVAL_STEP_DELAY;
            return;
        }
        case 1:  // wait for the account row
        {
            a.accountId = AccountMgr::GetId(a.username);
            if (!a.accountId)
            {
                if (++a.attempts >= 40)
                    FinishArrival("failed");
                nextArrivalStep = now + ARRIVAL_STEP_DELAY;
                return;
            }
            a.stage = 2;
            a.attempts = 0;
            return;
        }
        case 2:  // character
        {
            if (!RandomPlayerbotFactory::PickArrivalRaceClass(a.race, a.cls))
            {
                LOG_ERROR("playerbots", "Arrivals: no valid race/class combination, check AiPlayerbot.Arrivals.*Weights");
                FinishArrival("failed");
                return;
            }
            uint8 const gender = urand(1, 100) <= sPlayerbotAIConfig.arrivalsFemaleChance ? GENDER_FEMALE : GENDER_MALE;

            WorldSession* session = new WorldSession(a.accountId, "", 0x0, nullptr, SEC_PLAYER,
                                                     EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), LOCALE_enUS, 0,
                                                     false, false, 0, true);
            RandomPlayerbotFactory factory;
            std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> noNameCache;
            Player* player = factory.CreateBot(session, a.race, a.cls, gender, noNameCache);
            if (!player)
            {
                delete session;
                if (++a.attempts >= 5)
                    FinishArrival("failed");
                nextArrivalStep = now + ARRIVAL_STEP_DELAY;
                return;
            }

            player->SaveToDB(true, false);
            sCharacterCache->AddCharacterCacheEntry(player->GetGUID(), a.accountId, player->GetName(),
                                                    player->getGender(), player->getRace(), player->getClass(),
                                                    player->GetLevel());
            a.guid = player->GetGUID().GetCounter();
            LOG_INFO("playerbots", "Arrivals: {} signed up ({} race {} class {}) on account {}", player->GetName(),
                     IsAlliance(a.race) ? "Alliance" : "Horde", a.race, a.cls, a.username);
            player->CleanupsBeforeDelete();
            delete player;
            delete session;

            a.stage = 3;
            a.attempts = 0;
            nextArrivalStep = now + ARRIVAL_STEP_DELAY;
            return;
        }
        case 3:  // wait for the character row, then register the bot
        {
            if (!CharacterDatabase.Query("SELECT 1 FROM characters WHERE guid = {}", a.guid))
            {
                if (++a.attempts >= 40)
                    FinishArrival("failed");
                nextArrivalStep = now + ARRIVAL_STEP_DELAY;
                return;
            }

            PlayerbotsDatabase.Execute(
                "INSERT INTO playerbots_account_type (account_id, account_type, assignment_date) VALUES ({}, 1, NOW()) "
                "ON DUPLICATE KEY UPDATE account_type = 1",
                a.accountId);
            sRandomPlayerbotMgr.RegisterArrivalAccount(a.accountId);

            BotPersona persona = GeneratePersona(a.guid, a.accountId, a.race, a.cls, now);
            persona.nextLoginAt = now + urand(3 * MINUTE, 20 * MINUTE);  // a new player usually starts right away
            InsertPersona(persona);
            {
                std::unique_lock lock(personaLock);
                personas[persona.guid] = persona;
            }

            FinishArrival("done");
            return;
        }
    }
}

void BotLifecycleMgr::Update()
{
    if (!IsActive())
        return;

    uint32 const now = Now();

    if (now >= nextMaintenance)
    {
        nextMaintenance = now + MAINTENANCE_INTERVAL;
        MaintainPersonas(now);
    }

    if (!sPlayerbotAIConfig.arrivalsEnabled)
        return;

    if (now >= nextWeekCheck)
    {
        nextWeekCheck = now + WEEK_CHECK_INTERVAL;
        EnsureWeekPlan(now);
    }

    ProcessArrivals(now);
}
