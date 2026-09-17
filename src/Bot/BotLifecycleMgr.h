/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTLIFECYCLEMGR_H
#define PLAYERBOTS_BOTLIFECYCLEMGR_H

#include "Common.h"
#include "PlayerbotAIConfig.h"
#include <array>
#include <deque>
#include <random>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

enum class BotChronotype : uint8
{
    Daytime = 0,
    Evening = 1,
    NightOwl = 2,
    Any = 3
};

enum BotPlaystyle : uint8
{
    BOT_STYLE_QUESTER = 0,
    BOT_STYLE_GRINDER = 1,
    BOT_STYLE_CRAFTER = 2,
    BOT_STYLE_EXPLORER = 3,
    BOT_STYLE_SOCIAL = 4,
    BOT_STYLE_COUNT = 5
};

enum class BotLifeStatus : uint8
{
    Active = 0,
    Break = 1,
    Quit = 2
};

// Stable per-bot persona, stored in acore_playerbots.astro_bot_persona (one row per bot character)
struct BotPersona
{
    uint32 guid = 0;
    uint32 accountId = 0;
    uint32 arrivedAt = 0;
    uint8 race = 0;
    uint8 cls = 0;
    BotChronotype chronotype = BotChronotype::Any;
    uint8 sessionsPerWeek = 4;
    uint16 sessionMinutes = 90;
    std::array<uint8, BOT_STYLE_COUNT> styles{{20, 20, 20, 20, 20}};
    float breakFactor = 1.0f;
    BotLifeStatus status = BotLifeStatus::Active;
    uint32 breakUntil = 0;
    uint32 quitAt = 0;
    uint32 nextLoginAt = 0;  // 0 while in a session or after quitting
    uint32 lastLoginAt = 0;
    uint32 lastLogoutAt = 0;
    uint32 sessionsPlayed = 0;
    uint16 breaksTaken = 0;
};

struct BotLoginCandidate
{
    uint32 guid;
    uint32 accountId;
    uint8 race;
    uint8 cls;
};

// Treats random bots as real players (AiPlayerbot.Lifecycle.* and AiPlayerbot.Arrivals.*):
// - arrivals: a weekly budget of new level 1 bots (account + one character) spread unevenly over
//   the days of the week and over realistic sign-up hours, created at runtime;
// - personas: chronotype, sessions per week, session length, playstyle weights, break proneness;
// - schedules: each bot logs in when its own next login time comes, plays a session, takes short
//   or long breaks and rarely quits for good. The online population emerges from these schedules.
// All state is owned by the world thread; persona reads from bot AI threads are guarded.
class BotLifecycleMgr
{
public:
    static BotLifecycleMgr& instance()
    {
        static BotLifecycleMgr instance;
        return instance;
    }

    void Init();
    // Personas and sessions; runs on the random bot manager's AI tick
    void Update();
    // Week plan, arrivals, DB re-scan and stats; runs every world tick (world thread), at most once a second
    void UpdateArrivals();
    // After ".reload config" applied new values (world thread). Re-plans the rest of the week when the plan
    // keys changed; Cap and MaxPerHour act on the next arrival by themselves.
    void OnConfigReloaded(bool arrivalPlanChanged, uint32 oldArrivalsPerYear);

    // ".astro arrivals ..." (world thread). Each returns the lines to show; every change is logged.
    std::vector<std::string> AdminStatus();
    std::vector<std::string> AdminAddArrivals(uint32 count, uint32 hours, uint32 burstPerHour, std::string const& by);
    std::vector<std::string> AdminSetRate(uint32 perHour, std::string const& by);
    std::vector<std::string> AdminPause(bool pause, std::string const& by);

    bool IsActive() const { return initialized && sPlayerbotAIConfig.lifecycleEnabled; }
    bool HasPersona(uint32 guid) const;

    // Bots whose next login time has come (world thread)
    std::vector<BotLoginCandidate> GetDueCandidates(uint32 now) const;
    // Returns the length in seconds of the session that starts now
    uint32 StartSession(uint32 guid, uint32 now);
    void EndSession(uint32 guid, uint32 now);
    void OnLoginFailed(uint32 guid, uint32 now);

    // Scales an RPG status weight by the bot's playstyle weights
    uint32 GetRpgStatusWeight(uint32 guid, NewRpgStatus status, uint32 baseWeight) const;

    std::string GetStatsLine(uint32 now) const;

    static char const* ChronotypeName(BotChronotype type);
    static char const* PlaystyleName(uint8 style);
    static char const* StatusName(BotLifeStatus status);

private:
    BotLifecycleMgr() = default;

    struct PendingArrival
    {
        uint32 rowId = 0;
        uint8 stage = 0;
        uint32 nextStepAt = 0;
        uint32 attempts = 0;
        std::string username;
        uint32 accountId = 0;
        uint32 guid = 0;
        uint8 race = 0;
        uint8 cls = 0;
    };

    void CreateTables();
    void LoadPersonas(uint32 now);
    void SavePersona(BotPersona const& persona) const;
    void InsertPersona(BotPersona const& persona) const;

    BotPersona GeneratePersona(uint32 guid, uint32 accountId, uint8 race, uint8 cls, uint32 now);
    uint32 ScheduleNextLogin(BotPersona const& persona, uint32 from, uint32 sessionSeconds);
    uint32 AlignToChronotype(BotPersona const& persona, uint32 target, uint32 earliest, uint32 maxShift);
    // Sign-up hour drawn from HourWeights, truncated to [minHour, 24)
    float SampleArrivalHour(float minHour = 0.0f);

    void MaintainPersonas(uint32 now);
    uint32 GetWeekStart(uint32 now) const;
    void EnsureWeekPlan(uint32 now);
    // Expected share [0, 1] of a week's arrivals at or after `from` (weekday and hour weights)
    float RemainingWeekShare(uint32 weekStart, uint32 from) const;
    // `count` arrival times in [max(from, now), weekStart + 7 days), spread like a weekly plan; perDay gets the day counts
    std::vector<uint32> DrawWeekSlots(uint32 weekStart, uint32 from, uint32 now, uint32 count, std::array<uint32, 7>& perDay);
    void ReloadPendingArrivals();
    // Picks up pending rows other tools inserted (ids above the highest one already known)
    void RescanNewArrivals();
    void ReplanCurrentWeek(uint32 now, uint32 oldArrivalsPerYear);
    // Accounts that can still be appended to randomBotAccounts without reallocating it (see Init)
    size_t AccountHeadroom() const;
    uint32 PendingCount() const { return uint32(pendingArrivals.size()) + (hasActiveArrival ? 1 : 0); }
    void SpreadArrivalBacklog(uint32 now);
    void ProcessArrivals(uint32 now);
    void FinishArrival(char const* status);
    void LogArrivalStats(uint32 now);

    bool initialized = false;
    mutable std::shared_mutex personaLock;
    std::unordered_map<uint32, BotPersona> personas;

    std::deque<std::pair<uint32, uint32>> pendingArrivals;  // (row id, scheduled at), sorted
    PendingArrival activeArrival;
    bool hasActiveArrival = false;
    uint32 nextAccountIndex = 0;
    uint32 plannedWeekStart = 0;
    std::deque<uint32> recentArrivalStarts;  // start times of arrivals in the last hour (MaxPerHour)
    uint32 arrivalsDeferred = 0;             // times an arrival waited for MaxPerHour since the last stats line

    uint32 nextMaintenance = 0;
    uint32 nextWeekCheck = 0;
    uint32 nextArrivalStep = 0;
    uint32 nextArrivalStats = 0;
    uint32 nextArrivalRescan = 0;
    uint32 lastArrivalTick = 0;
    uint32 maxKnownArrivalId = 0;
    bool arrivalsPaused = false;  // .astro arrivals pause, in memory until resume or restart

    std::mt19937 rng{std::random_device{}()};
};

#define sBotLifecycleMgr BotLifecycleMgr::instance()

#endif
