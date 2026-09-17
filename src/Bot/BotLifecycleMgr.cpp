/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotLifecycleMgr.h"

#include "AccountMgr.h"
#include "ArrivalSlots.h"
#include "CharacterCache.h"
#include "Config.h"
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
#include <ctime>
#include <map>
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
constexpr uint32 PLAN_GRACE = WEEK_CHECK_INTERVAL + 5 * MINUTE;  // a week planned this soon after it began is planned whole
constexpr uint32 ARRIVAL_STATS_INTERVAL = 30 * MINUTE;
constexpr uint32 ARRIVAL_RESCAN_INTERVAL = 2 * MINUTE;  // rows inserted by other tools start within this
constexpr uint32 ARRIVAL_INSERT_CHUNK = 200;

using ArrivalSlots::HourMass;
using ArrivalSlots::HourWeightAt;

uint32 StochasticRound(float value)
{
    if (value <= 0.0f)
        return 0;
    uint32 whole = uint32(value);
    if (frand(0.0f, 1.0f) < value - float(whole))
        ++whole;
    return whole;
}

// "Mon 2026-09-14 00:00" in the population clock's local time
std::string LocalTimeStr(uint32 time)
{
    time_t const local = time_t(int64(time) + RandomPlayerbotMgr::GetPopulationUtcOffset(time));
    std::tm tm{};
    gmtime_r(&local, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%a %Y-%m-%d %H:%M", &tm);
    return buffer;
}
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

    // Who created a row: 'plan' (weekly plan), 'admin' (.astro arrivals add) or anything another tool sets.
    // Only 'plan' rows count towards and are re-planned with the weekly budget. Older builds insert without
    // the column and get 'plan', which is what they are.
    if (!PlayerbotsDatabase.Query("SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                                  "AND TABLE_NAME = 'astro_bot_arrivals' AND COLUMN_NAME = 'source'"))
    {
        PlayerbotsDatabase.DirectExecute(
            "ALTER TABLE `astro_bot_arrivals` ADD COLUMN `source` VARCHAR(16) NOT NULL DEFAULT 'plan' AFTER `status`");
        LOG_INFO("server.loading", ">> Arrivals: added astro_bot_arrivals.source");
    }

    PlayerbotsDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS `astro_bot_arrival_weeks` ("
        "`week_start` INT UNSIGNED NOT NULL,"
        "`planned_at` INT UNSIGNED NOT NULL,"
        "`plan` VARCHAR(16) NOT NULL,"
        "`share` FLOAT NOT NULL,"
        "`week_budget` FLOAT NOT NULL,"
        "`budget` INT UNSIGNED NOT NULL,"
        "`existing` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`added` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`total` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`per_day` VARCHAR(64) NOT NULL DEFAULT '',"
        "`first_at` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`last_at` INT UNSIGNED NOT NULL DEFAULT 0,"
        "PRIMARY KEY (`week_start`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Astro Realm: one row per planned arrivals week'");
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
        ReloadPendingArrivals();
        SpreadArrivalBacklog(now);

        // Arrivals of the last hour count towards MaxPerHour after a restart
        recentArrivalStarts.clear();
        if (QueryResult result = PlayerbotsDatabase.Query(
                "SELECT created_at FROM astro_bot_arrivals WHERE status = 'done' AND created_at > {} ORDER BY created_at",
                now > HOUR ? now - HOUR : 0))
        {
            do
            {
                recentArrivalStarts.push_back(result->Fetch()[0].Get<uint32>());
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

float BotLifecycleMgr::SampleArrivalHour(float minHour)
{
    auto const& points = sPlayerbotAIConfig.arrivalsHourPoints;
    minHour = std::clamp(minHour, 0.0f, 24.0f);
    float const span = 24.0f - minHour;
    if (span <= 0.0f)
        return minHour;

    // Largest weight inside [minHour, 24): the curve is piecewise linear, so an end or a point
    float maxWeight = std::max(HourWeightAt(points, minHour), HourWeightAt(points, 24.0f));
    for (auto const& point : points)
        if (point.first >= minHour)
            maxWeight = std::max(maxWeight, point.second);

    // No weight left in the window (or none at all): uniform over the window
    if (maxWeight <= 0.0f || HourMass(points, minHour, 24.0f) <= 1e-4f)
        return minHour + frand(0.0f, span);

    for (uint32 attempt = 0; attempt < 200; ++attempt)
    {
        float hour = minHour + frand(0.0f, span);
        if (frand(0.0f, maxWeight) <= HourWeightAt(points, hour))
            return std::min(hour, 24.0f);
    }
    return minHour + frand(0.0f, span);
}

float BotLifecycleMgr::RemainingWeekShare(uint32 weekStart, uint32 from) const
{
    if (from <= weekStart)
        return 1.0f;
    if (from >= weekStart + 7 * DAY)
        return 0.0f;

    auto const& points = sPlayerbotAIConfig.arrivalsHourPoints;
    uint32 const today = (from - weekStart) / DAY;
    float const hourNow = float(from - weekStart - today * DAY) / HOUR;
    float const dayMass = HourMass(points, 0.0f, 24.0f);
    float const todayShare = dayMass > 0.0f ? HourMass(points, hourNow, 24.0f) / dayMass : (24.0f - hourNow) / 24.0f;

    float ahead = 0.0f, total = 0.0f;
    for (uint32 d = 0; d < 7; ++d)
    {
        float const weight = sPlayerbotAIConfig.arrivalsWeekdayWeights[d];
        total += weight;
        if (d > today)
            ahead += weight;
        else if (d == today)
            ahead += weight * todayShare;
    }
    if (total <= 0.0f)
        return float(weekStart + 7 * DAY - from) / float(7 * DAY);
    return std::clamp(ahead / total, 0.0f, 1.0f);
}

std::vector<uint32> BotLifecycleMgr::DrawWeekSlots(uint32 weekStart, uint32 from, uint32 now, uint32 count,
                                                   std::array<uint32, 7>& perDay)
{
    perDay.fill(0);
    std::vector<uint32> times;
    if (!count)
        return times;

    uint32 const weekEnd = weekStart + 7 * DAY;
    from = std::clamp(from, weekStart, weekEnd - 1);
    uint32 const earliest = std::max(from, now);
    uint32 const today = (from - weekStart) / DAY;
    float const hourNow = float(from - weekStart - today * DAY) / HOUR;

    // Fully past days get no weight; today only the hour weight still ahead
    auto const& points = sPlayerbotAIConfig.arrivalsHourPoints;
    float const dayMass = HourMass(points, 0.0f, 24.0f);
    float const todayShare = dayMass > 0.0f ? HourMass(points, hourNow, 24.0f) / dayMass : (24.0f - hourNow) / 24.0f;

    std::gamma_distribution<float> gamma(sPlayerbotAIConfig.arrivalsDayShape, 1.0f);
    std::array<float, 7> dayWeights{};
    float total = 0.0f;
    for (uint32 d = 0; d < 7; ++d)
    {
        float const g = gamma(rng);
        if (d < today)
            continue;
        dayWeights[d] = sPlayerbotAIConfig.arrivalsWeekdayWeights[d] * g * (d == today ? todayShare : 1.0f);
        total += dayWeights[d];
    }

    times.reserve(count);
    for (uint32 i = 0; i < count; ++i)
    {
        uint32 day = today;
        uint32 at;
        if (total > 0.0f)
        {
            float roll = frand(0.0f, total);
            day = 6;
            for (uint32 d = today; d < 7; ++d)
            {
                if (dayWeights[d] <= 0.0f)
                    continue;
                day = d;
                roll -= dayWeights[d];
                if (roll <= 0.0f)
                    break;
            }
            float const hour = SampleArrivalHour(day == today ? hourNow : 0.0f);
            uint32 const dayStart = weekStart + day * DAY;
            at = std::min(dayStart + uint32(hour * HOUR), dayStart + DAY - 1);
        }
        else
        {
            // No weight left anywhere this week: uniform over the rest of it
            at = from + uint32(frand(0.0f, 1.0f) * float(weekEnd - 1 - from));
            day = (at - weekStart) / DAY;
        }

        // Never dropped: a slot that would be in the past happens now instead
        times.push_back(std::max(at, earliest));
        ++perDay[std::min<uint32>(day, 6)];
    }
    std::sort(times.begin(), times.end());
    return times;
}

// Plans a week's arrivals once, recorded in astro_bot_arrival_weeks. A full week gets a budget of
// PerYear * 7 / 365 (+-15%) spread over the days with weights WeekdayWeights[d] * Gamma(DayShape), each at a
// sign-up hour drawn from HourWeights (local time). A week that already started (first start mid-week, or
// after downtime) only plans the time still ahead: past days get weight 0, today's hours are truncated to
// [now, midnight) and, with ScaleMidWeek, the budget is scaled to the share of the week still ahead. A week
// planned by an older build (rows without a week record) is topped up to that budget once.
void BotLifecycleMgr::EnsureWeekPlan(uint32 now)
{
    uint32 const weekStart = GetWeekStart(now);
    if (weekStart == plannedWeekStart)
        return;

    if (PlayerbotsDatabase.Query("SELECT 1 FROM astro_bot_arrival_weeks WHERE week_start = {}", weekStart))
    {
        plannedWeekStart = weekStart;
        return;
    }

    uint32 rowsAll = 0, rowsReal = 0, rowsAhead = 0;
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT COUNT(*), CAST(COALESCE(SUM(status <> 'empty'), 0) AS UNSIGNED), "
            "CAST(COALESCE(SUM(status IN ('pending', 'creating')), 0) AS UNSIGNED) "
            "FROM astro_bot_arrivals WHERE week_start = {} AND source = 'plan'", weekStart))
    {
        Field* f = result->Fetch();
        rowsAll = uint32(f[0].Get<uint64>());
        rowsReal = uint32(f[1].Get<uint64>());
        rowsAhead = uint32(f[2].Get<uint64>());
    }
    bool const legacy = rowsAll > 0;

    // Planned right after the week began (the check runs every WEEK_CHECK_INTERVAL): the whole week
    uint32 const from = !legacy && now - weekStart <= PLAN_GRACE ? weekStart : now;
    float const share = sPlayerbotAIConfig.arrivalsScaleMidWeek ? RemainingWeekShare(weekStart, from) : 1.0f;
    float const weekBudget = sPlayerbotAIConfig.arrivalsPerYear * 7.0f / 365.25f * frand(0.85f, 1.15f);
    uint32 const budget = StochasticRound(weekBudget * share);
    uint32 const existing = !legacy ? 0 : sPlayerbotAIConfig.arrivalsScaleMidWeek ? rowsAhead : rowsReal;
    uint32 const add = budget > existing ? budget - existing : 0;

    std::array<uint32, 7> addedPerDay{};
    std::vector<uint32> const times = DrawWeekSlots(weekStart, from, now, add, addedPerDay);

    if (!times.empty())
    {
        std::ostringstream sql;
        sql << "INSERT INTO astro_bot_arrivals (week_start, scheduled_at, status, source) VALUES ";
        for (size_t i = 0; i < times.size(); ++i)
            sql << (i ? "," : "") << "(" << weekStart << "," << times[i] << ",'pending','plan')";
        PlayerbotsDatabase.DirectExecute(sql.str());
    }
    else if (!rowsAll)
    {
        // Marker so older builds do not plan the week again
        PlayerbotsDatabase.DirectExecute(
            "INSERT INTO astro_bot_arrivals (week_start, scheduled_at, status) VALUES ({}, {}, 'empty')", weekStart,
            weekStart);
    }

    // Summary of the whole week as it now stands (earlier rows included)
    std::array<uint32, 7> perDay{};
    uint32 rows = 0, first = 0, last = 0;
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT scheduled_at FROM astro_bot_arrivals WHERE week_start = {} AND status <> 'empty' AND source = 'plan' "
            "ORDER BY scheduled_at",
            weekStart))
    {
        do
        {
            uint32 const at = result->Fetch()[0].Get<uint32>();
            ++perDay[std::min<uint32>(at > weekStart ? (at - weekStart) / DAY : 0, 6)];
            first = rows++ ? first : at;
            last = at;
        } while (result->NextRow());
    }

    char const* plan = legacy ? "topped_up" : from == weekStart ? "full" : "mid_week";
    std::ostringstream days;
    for (uint32 d = 0; d < 7; ++d)
        days << (d ? " " : "") << perDay[d];

    PlayerbotsDatabase.DirectExecute(
        "INSERT IGNORE INTO astro_bot_arrival_weeks (week_start, planned_at, plan, share, week_budget, budget, existing, "
        "added, total, per_day, first_at, last_at) VALUES ({}, {}, '{}', {:.4f}, {:.2f}, {}, {}, {}, {}, '{}', {}, {})",
        weekStart, now, plan, share, weekBudget, budget, existing, times.size(), rows, days.str(), first, last);

    ReloadPendingArrivals();
    plannedWeekStart = weekStart;

    LOG_INFO("server.worldserver",
             "Arrivals: week of {} planned ({}): {:.1f}% of the week ahead, budget {} (full week {:.1f}), {} existing, "
             "{} added; Mon..Sun {} = {} total, first {}, last {}; {} pending",
             LocalTimeStr(weekStart), plan, share * 100.0f, budget, weekBudget, existing, times.size(), days.str(), rows,
             rows ? LocalTimeStr(first) : "-", rows ? LocalTimeStr(last) : "-", pendingArrivals.size());
}

void BotLifecycleMgr::ReloadPendingArrivals()
{
    pendingArrivals.clear();
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT id, scheduled_at FROM astro_bot_arrivals WHERE status = 'pending' ORDER BY scheduled_at, id"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const id = fields[0].Get<uint32>();
            maxKnownArrivalId = std::max(maxKnownArrivalId, id);
            if (hasActiveArrival && id == activeArrival.rowId)
                continue;  // its 'creating' update may still be queued
            pendingArrivals.emplace_back(id, fields[1].Get<uint32>());
        } while (result->NextRow());
    }
}

// Arrivals that came due while the server was down are spread in order over the next BacklogSpreadMinutes
// instead of all signing up at once
void BotLifecycleMgr::SpreadArrivalBacklog(uint32 now)
{
    size_t overdue = 0;
    while (overdue < pendingArrivals.size() && pendingArrivals[overdue].second < now)
        ++overdue;
    if (!overdue || !sPlayerbotAIConfig.arrivalsBacklogSpreadMinutes)
        return;

    uint32 const oldest = pendingArrivals.front().second;
    float const step = float(sPlayerbotAIConfig.arrivalsBacklogSpreadMinutes * MINUTE) / float(overdue);
    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();
    for (size_t i = 0; i < overdue; ++i)
    {
        // One slot per overdue arrival, each in its own sub-window, so the order is kept
        uint32 const at = now + uint32((float(i) + frand(0.0f, 0.999f)) * step);
        pendingArrivals[i].second = at;
        trans->Append("UPDATE astro_bot_arrivals SET scheduled_at = {} WHERE id = {}", at, pendingArrivals[i].first);
    }
    PlayerbotsDatabase.DirectCommitTransaction(trans);
    std::stable_sort(pendingArrivals.begin(), pendingArrivals.end(),
                     [](auto const& a, auto const& b) { return a.second < b.second; });

    LOG_INFO("server.loading", ">> Arrivals: {} overdue arrivals (oldest due {}) spread over the next {} minutes", overdue,
             LocalTimeStr(oldest), sPlayerbotAIConfig.arrivalsBacklogSpreadMinutes);
}

void BotLifecycleMgr::LogArrivalStats(uint32 now)
{
    uint32 done = 0, failed = 0, capped = 0;
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT status, COUNT(*) FROM astro_bot_arrivals WHERE week_start = {} GROUP BY status", plannedWeekStart))
    {
        do
        {
            Field* f = result->Fetch();
            std::string const status = f[0].Get<std::string>();
            uint32 const count = uint32(f[1].Get<uint64>());
            if (status == "done")
                done = count;
            else if (status == "failed")
                failed = count;
            else if (status == "capped")
                capped = count;
        } while (result->NextRow());
    }

    size_t due = 0;
    for (auto const& pending : pendingArrivals)
    {
        if (pending.second > now)
            break;
        ++due;
    }

    while (!recentArrivalStarts.empty() && recentArrivalStarts.front() + HOUR <= now)
        recentArrivalStarts.pop_front();

    LOG_INFO("server.worldserver",
             "Arrivals: week of {}: {} done, {} failed, {} capped; {} pending ({} due now), next {}; {} started in the "
             "last hour (MaxPerHour {}), {} waits for MaxPerHour since the last report",
             LocalTimeStr(plannedWeekStart), done, failed, capped, pendingArrivals.size() + (hasActiveArrival ? 1 : 0),
             due, pendingArrivals.empty() ? "-" : LocalTimeStr(pendingArrivals.front().second),
             recentArrivalStarts.size(), sPlayerbotAIConfig.arrivalsMaxPerHour, arrivalsDeferred);
    arrivalsDeferred = 0;
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
    if (now < nextArrivalStep || (arrivalsPaused && !hasActiveArrival))
        return;

    if (!hasActiveArrival)
    {
        if (pendingArrivals.empty() || pendingArrivals.front().second > now)
            return;

        // Rolling hourly cap: the arrival stays pending and starts once the oldest of the last hour ages out
        while (!recentArrivalStarts.empty() && recentArrivalStarts.front() + HOUR <= now)
            recentArrivalStarts.pop_front();
        if (sPlayerbotAIConfig.arrivalsMaxPerHour && recentArrivalStarts.size() >= sPlayerbotAIConfig.arrivalsMaxPerHour)
        {
            ++arrivalsDeferred;
            nextArrivalStep = recentArrivalStarts.front() + HOUR;
            return;
        }

        // Another tool may have cancelled or taken the row since it was loaded
        uint32 const rowId = pendingArrivals.front().first;
        pendingArrivals.pop_front();
        QueryResult row = PlayerbotsDatabase.Query("SELECT status FROM astro_bot_arrivals WHERE id = {}", rowId);
        if (!row || row->Fetch()[0].Get<std::string>() != "pending")
        {
            LOG_INFO("playerbots", "Arrivals: row {} is no longer pending, skipped", rowId);
            return;
        }

        activeArrival = PendingArrival();
        activeArrival.rowId = rowId;
        hasActiveArrival = true;

        size_t total;
        {
            std::shared_lock lock(personaLock);
            total = personas.size();
        }
        // No headroom left means the account list would reallocate under readers: stop like the cap does
        if (total >= sPlayerbotAIConfig.arrivalsCap || !AccountHeadroom())
        {
            FinishArrival("capped");
            return;
        }

        recentArrivalStarts.push_back(now);
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

}

void BotLifecycleMgr::UpdateArrivals()
{
    if (!IsActive() || !sPlayerbotAIConfig.arrivalsEnabled)
        return;

    uint32 const now = Now();
    if (now == lastArrivalTick)
        return;
    lastArrivalTick = now;

    if (now >= nextWeekCheck)
    {
        nextWeekCheck = now + WEEK_CHECK_INTERVAL;
        EnsureWeekPlan(now);
    }

    if (now >= nextArrivalRescan)
    {
        nextArrivalRescan = now + ARRIVAL_RESCAN_INTERVAL;
        RescanNewArrivals();
    }

    ProcessArrivals(now);

    if (now >= nextArrivalStats && plannedWeekStart)
    {
        nextArrivalStats = now + ARRIVAL_STATS_INTERVAL;
        LogArrivalStats(now);
    }
}

size_t BotLifecycleMgr::AccountHeadroom() const
{
    auto const& accounts = sPlayerbotAIConfig.randomBotAccounts;
    return accounts.capacity() > accounts.size() ? accounts.capacity() - accounts.size() : 0;
}

void BotLifecycleMgr::RescanNewArrivals()
{
    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT id, scheduled_at, source FROM astro_bot_arrivals WHERE status = 'pending' AND id > {} ORDER BY id",
        maxKnownArrivalId);
    if (!result)
        return;

    uint32 added = 0;
    std::map<std::string, uint32> sources;
    do
    {
        Field* f = result->Fetch();
        uint32 const id = f[0].Get<uint32>();
        uint32 const at = f[1].Get<uint32>();
        maxKnownArrivalId = std::max(maxKnownArrivalId, id);
        if ((hasActiveArrival && id == activeArrival.rowId) ||
            std::any_of(pendingArrivals.begin(), pendingArrivals.end(), [id](auto const& p) { return p.first == id; }))
            continue;

        auto const pos = std::upper_bound(pendingArrivals.begin(), pendingArrivals.end(), at,
                                          [](uint32 value, auto const& p) { return value < p.second; });
        pendingArrivals.insert(pos, {id, at});
        ++sources[f[2].Get<std::string>()];
        ++added;
    } while (result->NextRow());

    if (!added)
        return;

    std::ostringstream by;
    for (auto const& [source, count] : sources)
        by << (by.tellp() > 0 ? ", " : "") << source << " " << count;
    LOG_INFO("playerbots", "Arrivals: picked up {} new pending rows from the database ({}), {} pending, next {}", added,
             by.str(), PendingCount(), LocalTimeStr(pendingArrivals.front().second));
}

void BotLifecycleMgr::OnConfigReloaded(bool arrivalPlanChanged, uint32 oldArrivalsPerYear)
{
    if (!IsActive() || !sPlayerbotAIConfig.arrivalsEnabled)
        return;

    uint32 const now = Now();

    // Personas can only grow as far as this run reserved account slots for (Init)
    size_t personaCount;
    {
        std::shared_lock lock(personaLock);
        personaCount = personas.size();
    }
    size_t const reachable = personaCount + AccountHeadroom() + (hasActiveArrival ? 1 : 0);
    if (sPlayerbotAIConfig.arrivalsCap > reachable)
    {
        LOG_WARN("playerbots", "Arrivals: Cap {} is above the {} bots this run reserved room for; using {} until a restart",
                 sPlayerbotAIConfig.arrivalsCap, reachable, reachable);
        sPlayerbotAIConfig.arrivalsCap = uint32(reachable);
    }

    // A wait for the old MaxPerHour may no longer apply
    if (!hasActiveArrival)
        nextArrivalStep = std::min(nextArrivalStep, now);

    if (arrivalPlanChanged)
        ReplanCurrentWeek(now, oldArrivalsPerYear);
}

// The rest of the current week with the reloaded PerYear, DayShape, WeekdayWeights, HourWeights and
// ScaleMidWeek. Idempotent: the pending 'plan' rows of the week get new times, then rows are added or marked
// 'dropped' to reach the new remaining budget. Done, failed, capped and creating rows and rows from other
// sources are never touched, so reloading twice with the same values keeps the same number of rows.
void BotLifecycleMgr::ReplanCurrentWeek(uint32 now, uint32 oldArrivalsPerYear)
{
    uint32 const weekStart = GetWeekStart(now);
    QueryResult week = PlayerbotsDatabase.Query(
        "SELECT week_budget FROM astro_bot_arrival_weeks WHERE week_start = {}", weekStart);
    if (!week)
        return;  // not planned yet: the next week check plans it with the new values

    // Keep the week's own +-15% draw: its budget relative to the PerYear it was planned with
    float const oldFullWeek = oldArrivalsPerYear * 7.0f / 365.25f;
    float const draw = oldFullWeek > 0.0f ? std::clamp(week->Fetch()[0].Get<float>() / oldFullWeek, 0.85f, 1.15f) : 1.0f;
    float const weekBudget = sPlayerbotAIConfig.arrivalsPerYear * 7.0f / 365.25f * draw;

    std::vector<uint32> pendingIds;
    uint32 started = 0;
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT id, status FROM astro_bot_arrivals WHERE week_start = {} AND source = 'plan' "
            "AND status NOT IN ('empty', 'dropped')", weekStart))
    {
        do
        {
            Field* f = result->Fetch();
            uint32 const id = f[0].Get<uint32>();
            if (f[1].Get<std::string>() == "pending" && !(hasActiveArrival && id == activeArrival.rowId))
                pendingIds.push_back(id);
            else
                ++started;
        } while (result->NextRow());
    }

    float share = 1.0f;
    uint32 target;
    if (sPlayerbotAIConfig.arrivalsScaleMidWeek)
    {
        share = RemainingWeekShare(weekStart, now);
        target = uint32(std::lround(weekBudget * share));
    }
    else
    {
        uint32 const whole = uint32(std::lround(weekBudget));
        target = whole > started ? whole - started : 0;
    }

    ArrivalSlots::Replan const delta = ArrivalSlots::PlanDelta(uint32(pendingIds.size()), target);
    std::array<uint32, 7> perDay{};
    std::vector<uint32> const times = DrawWeekSlots(weekStart, now, now, target, perDay);
    std::shuffle(pendingIds.begin(), pendingIds.end(), rng);

    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();
    for (uint32 i = 0; i < delta.reschedule; ++i)
        trans->Append("UPDATE astro_bot_arrivals SET scheduled_at = {} WHERE id = {} AND status = 'pending'", times[i],
                      pendingIds[i]);
    for (uint32 i = delta.reschedule; i < delta.reschedule + delta.drop; ++i)
        trans->Append("UPDATE astro_bot_arrivals SET status = 'dropped' WHERE id = {} AND status = 'pending'", pendingIds[i]);
    for (uint32 i = delta.reschedule; i < times.size(); i += ARRIVAL_INSERT_CHUNK)
    {
        std::ostringstream sql;
        sql << "INSERT INTO astro_bot_arrivals (week_start, scheduled_at, status, source) VALUES ";
        for (uint32 j = i; j < std::min<uint32>(uint32(times.size()), i + ARRIVAL_INSERT_CHUNK); ++j)
            sql << (j > i ? "," : "") << "(" << weekStart << "," << times[j] << ",'pending','plan')";
        trans->Append(sql.str().c_str());
    }
    trans->Append("UPDATE astro_bot_arrival_weeks SET plan = 'replanned', week_budget = {:.2f}, share = {:.4f}, "
                  "budget = {} WHERE week_start = {}", weekBudget, share, target, weekStart);
    PlayerbotsDatabase.DirectCommitTransaction(trans);

    ReloadPendingArrivals();
    if (!hasActiveArrival)
        nextArrivalStep = std::min(nextArrivalStep, now);

    std::ostringstream days;
    for (uint32 d = 0; d < 7; ++d)
        days << (d ? " " : "") << perDay[d];
    LOG_INFO("playerbots",
             "Arrivals: week of {} re-planned after config reload: PerYear {} -> {}, {:.1f}% of the week ahead, "
             "remaining budget {} (was {} pending): {} moved, {} added, {} dropped; Mon..Sun {}; {} pending",
             LocalTimeStr(weekStart), oldArrivalsPerYear, sPlayerbotAIConfig.arrivalsPerYear, share * 100.0f, target,
             pendingIds.size(), delta.reschedule, delta.insert, delta.drop, days.str(), PendingCount());
}

std::vector<std::string> BotLifecycleMgr::AdminStatus()
{
    std::vector<std::string> lines;
    if (!IsActive() || !sPlayerbotAIConfig.arrivalsEnabled)
    {
        lines.push_back("Arrivals are off (AiPlayerbot.Lifecycle.Enable and AiPlayerbot.Arrivals.Enable, restart)");
        return lines;
    }

    uint32 const now = Now();
    std::ostringstream sources;
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT source, COUNT(*) FROM astro_bot_arrivals WHERE status = 'pending' GROUP BY source ORDER BY source"))
    {
        do
        {
            Field* f = result->Fetch();
            sources << (sources.tellp() > 0 ? ", " : "") << f[0].Get<std::string>() << " " << f[1].Get<uint64>();
        } while (result->NextRow());
    }

    size_t due = 0;
    for (auto const& pending : pendingArrivals)
        due += pending.second <= now;
    std::string next = "-";
    if (hasActiveArrival)
        next = "being created now";
    else if (!pendingArrivals.empty())
    {
        uint32 const at = std::max(pendingArrivals.front().second, nextArrivalStep);
        next = LocalTimeStr(at) + (at > now ? " (in " + std::to_string((at - now + 59) / 60) + " min)" : " (due)");
    }
    if (arrivalsPaused)
        lines.push_back("PAUSED: no new arrivals start until .astro arrivals resume (or a restart)");
    lines.push_back("Pending " + std::to_string(PendingCount()) + " (" + (sources.tellp() > 0 ? sources.str() : "none in DB") +
                    "), due now " + std::to_string(due) + ", next " + next);

    std::map<std::string, uint64> week;
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT status, COUNT(*) FROM astro_bot_arrivals WHERE week_start = {} GROUP BY status", GetWeekStart(now)))
    {
        do
        {
            Field* f = result->Fetch();
            week[f[0].Get<std::string>()] = f[1].Get<uint64>();
        } while (result->NextRow());
    }
    uint64 allDone = 0;
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT COUNT(*) FROM astro_bot_arrivals WHERE status = 'done'"))
        allDone = result->Fetch()[0].Get<uint64>();
    lines.push_back("Week of " + LocalTimeStr(GetWeekStart(now)) + ": done " + std::to_string(week["done"]) + ", failed " +
                    std::to_string(week["failed"]) + ", capped " + std::to_string(week["capped"]) + ", dropped " +
                    std::to_string(week["dropped"]) + "; done all time " + std::to_string(allDone));

    while (!recentArrivalStarts.empty() && recentArrivalStarts.front() + HOUR <= now)
        recentArrivalStarts.pop_front();
    uint32 const configured = uint32(std::max(0, sConfigMgr->GetOption<int32>("AiPlayerbot.Arrivals.MaxPerHour", 8, false)));
    uint32 const perHour = sPlayerbotAIConfig.arrivalsMaxPerHour;
    lines.push_back("Rate: " + std::to_string(recentArrivalStarts.size()) + " started in the last hour, MaxPerHour " +
                    (perHour ? std::to_string(perHour) : std::string("off")) +
                    (perHour != configured ? " (set live, config " + std::to_string(configured) + ")" : "") +
                    ", at most about " + std::to_string(HOUR / (ARRIVAL_SPACING + 2 * ARRIVAL_STEP_DELAY + 2)) +
                    " per hour with the arrival spacing");

    size_t personaCount;
    {
        std::shared_lock lock(personaLock);
        personaCount = personas.size();
    }
    lines.push_back("Personas " + std::to_string(personaCount) + ", Cap " + std::to_string(sPlayerbotAIConfig.arrivalsCap) +
                    ", room for " + std::to_string(AccountHeadroom()) + " more accounts this run, PerYear " +
                    std::to_string(sPlayerbotAIConfig.arrivalsPerYear));
    lines.push_back("Lifecycle: " + GetStatsLine(now));
    return lines;
}

std::vector<std::string> BotLifecycleMgr::AdminAddArrivals(uint32 count, uint32 hours, uint32 burstPerHour,
                                                           std::string const& by)
{
    std::vector<std::string> lines;
    if (!IsActive() || !sPlayerbotAIConfig.arrivalsEnabled)
    {
        lines.push_back("Arrivals are off (AiPlayerbot.Lifecycle.Enable and AiPlayerbot.Arrivals.Enable, restart)");
        return lines;
    }
    if (!count || count > 5000 || !hours || hours > 14 * 24)
    {
        lines.push_back("Count must be 1..5000 and hours 1..336");
        return lines;
    }

    size_t personaCount;
    {
        std::shared_lock lock(personaLock);
        personaCount = personas.size();
    }
    uint32 const pending = PendingCount();
    if (personaCount + pending + count > sPlayerbotAIConfig.arrivalsCap)
    {
        lines.push_back("Not added: " + std::to_string(personaCount) + " personas + " + std::to_string(pending) +
                        " pending + " + std::to_string(count) + " would pass AiPlayerbot.Arrivals.Cap " +
                        std::to_string(sPlayerbotAIConfig.arrivalsCap) + " (raise it and .reload config)");
        return lines;
    }
    if (pending + count > AccountHeadroom())
    {
        lines.push_back("Not added: this run reserved room for " + std::to_string(AccountHeadroom()) +
                        " more bot accounts and " + std::to_string(pending) + " are already pending (needs a restart)");
        return lines;
    }

    uint32 const now = Now();
    uint32 const start = now + MINUTE;
    uint32 const end = now + hours * HOUR;
    std::vector<uint32> const times = ArrivalSlots::DrawWindow(
        start, end, count, sPlayerbotAIConfig.arrivalsHourPoints,
        [](uint32 t) { return RandomPlayerbotMgr::GetPopulationLocalHour(time_t(t)); }, rng);

    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();
    for (size_t i = 0; i < times.size(); i += ARRIVAL_INSERT_CHUNK)
    {
        std::ostringstream sql;
        sql << "INSERT INTO astro_bot_arrivals (week_start, scheduled_at, status, source) VALUES ";
        for (size_t j = i; j < std::min(times.size(), i + ARRIVAL_INSERT_CHUNK); ++j)
            sql << (j > i ? "," : "") << "(" << GetWeekStart(times[j]) << "," << times[j] << ",'pending','admin')";
        trans->Append(sql.str().c_str());
    }
    PlayerbotsDatabase.DirectCommitTransaction(trans);
    ReloadPendingArrivals();

    if (burstPerHour)
        sPlayerbotAIConfig.arrivalsMaxPerHour = burstPerHour;
    if (!hasActiveArrival)
        nextArrivalStep = std::min(nextArrivalStep, now);

    std::vector<uint32> window;
    for (auto const& p : pendingArrivals)
        if (p.second < end)
            window.push_back(p.second);
    uint32 const peakAdded = ArrivalSlots::PeakPerHour(times);
    uint32 const peakAll = ArrivalSlots::PeakPerHour(window);

    lines.push_back("Added " + std::to_string(count) + " arrivals between " + LocalTimeStr(times.front()) + " and " +
                    LocalTimeStr(times.back()) + ", busiest hour " + std::to_string(peakAdded) + " (" +
                    std::to_string(peakAll) + " with the other pending rows); " + std::to_string(PendingCount()) +
                    " pending");
    uint32 const perHour = sPlayerbotAIConfig.arrivalsMaxPerHour;
    if (burstPerHour)
        lines.push_back("MaxPerHour set to " + std::to_string(burstPerHour) + " until the next .reload config");
    if (perHour && peakAll > perHour)
        lines.push_back("MaxPerHour " + std::to_string(perHour) + " is below the busiest hour (" + std::to_string(peakAll) +
                        "): arrivals will queue and run late. Raise it with .astro arrivals rate <n> or "
                        "AiPlayerbot.Arrivals.MaxPerHour and .reload config");

    LOG_INFO("playerbots", "Arrivals admin: {} added {} arrivals over {} h ({} to {}), busiest hour {}, MaxPerHour {}{}",
             by, count, hours, LocalTimeStr(times.front()), LocalTimeStr(times.back()), peakAll, perHour,
             burstPerHour ? " (set by this command)" : "");
    return lines;
}

std::vector<std::string> BotLifecycleMgr::AdminSetRate(uint32 perHour, std::string const& by)
{
    std::vector<std::string> lines;
    if (!IsActive() || !sPlayerbotAIConfig.arrivalsEnabled)
    {
        lines.push_back("Arrivals are off (AiPlayerbot.Lifecycle.Enable and AiPlayerbot.Arrivals.Enable, restart)");
        return lines;
    }

    uint32 const old = sPlayerbotAIConfig.arrivalsMaxPerHour;
    sPlayerbotAIConfig.arrivalsMaxPerHour = perHour;
    if (!hasActiveArrival)
        nextArrivalStep = std::min(nextArrivalStep, Now());

    lines.push_back("MaxPerHour " + (old ? std::to_string(old) : std::string("off")) + " -> " +
                    (perHour ? std::to_string(perHour) : std::string("off")) +
                    " until the next .reload config (set AiPlayerbot.Arrivals.MaxPerHour to keep it)");
    LOG_INFO("playerbots", "Arrivals admin: {} set MaxPerHour {} -> {}", by, old, perHour);
    return lines;
}

std::vector<std::string> BotLifecycleMgr::AdminPause(bool pause, std::string const& by)
{
    std::vector<std::string> lines;
    bool const was = arrivalsPaused;
    arrivalsPaused = pause;
    if (!pause && !hasActiveArrival)
        nextArrivalStep = std::min(nextArrivalStep, Now());
    lines.push_back(pause ? (was ? "Arrivals were already paused" : "Arrivals paused (the one being created finishes)")
                          : (was ? "Arrivals resumed, overdue rows start one by one within MaxPerHour"
                                 : "Arrivals were not paused"));
    LOG_INFO("playerbots", "Arrivals admin: {} {} arrivals ({} pending)", by, pause ? "paused" : "resumed", PendingCount());
    return lines;
}
