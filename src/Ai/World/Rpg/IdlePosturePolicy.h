/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_IDLEPOSTUREPOLICY_H
#define PLAYERBOTS_IDLEPOSTUREPOLICY_H

#include <cstdint>

// When does a bot that is hanging around sit down, stand up, or dance a little? Pure functions with no
// core dependencies so the rules can be tested without a server (see test/IdlePosturePolicyTest.cpp).
//
// People do not sit the moment they stop: they stand around for a while first, how long depends on the
// person, and they do not sit at all in the middle of a grind spot unless they have been waiting a long
// time. They stand up before they walk off.
namespace IdlePosturePolicy
{
    enum class Activity : uint8_t
    {
        Other,       // questing, grinding, travelling: sit only after a long wait
        Resting,     // taking a break (rpg rest)
        SocialAfk,   // hanging around an inn or a bank
        Errands,     // wandering between vendors and trainers
    };

    struct Settings
    {
        bool enabled = true;
        uint32_t minIdleMs = 20000;
        uint32_t maxIdleMs = 40000;
        uint32_t openWorldFactor = 2;   // outside towns and breaks, wait this many times longer
        uint32_t sitChancePct = 75;     // per idle spell: some people just stand
        uint32_t danceChancePct = 2;    // per idle check in town once settled
        uint32_t danceMinMs = 8000;
        uint32_t danceMaxMs = 25000;
        uint32_t danceCooldownMs = 600000;
    };

    struct Facts
    {
        bool alive = true;
        bool inCombat = false;
        bool moving = false;
        bool mounted = false;
        bool casting = false;
        bool swimming = false;
        bool inFlight = false;
        bool followingPlayer = false;   // grouped with a real player: posture follows theirs
        bool inRestArea = false;        // city or inn
        bool sitting = false;
        Activity activity = Activity::Other;
        uint32_t idleMs = 0;            // time since the bot last moved or did something
    };

    enum class Posture : uint8_t
    {
        Stand,   // stand up now
        Sit,     // sit down, or stay seated
        Keep,    // leave it as it is
    };

    // Each bot waits a different time before sitting: a stable per-bot base in [min, max] from its id,
    // nudged per idle spell by `spellRoll` (0-99), stretched outside towns and breaks.
    inline uint32_t SitDelayMs(uint32_t botId, uint32_t spellRoll, Facts const& f, Settings const& s)
    {
        uint32_t const span = s.maxIdleMs > s.minIdleMs ? s.maxIdleMs - s.minIdleMs : 0;
        uint32_t const hash = (botId * 2654435761u) >> 8;
        uint32_t base = s.minIdleMs + (span ? hash % (span + 1) : 0);

        // +-15% for this particular idle spell, still inside the configured window
        int64_t const jitter = (static_cast<int64_t>(spellRoll % 100) - 50) * static_cast<int64_t>(base) * 15 / 5000;
        int64_t delay = static_cast<int64_t>(base) + jitter;
        if (delay < s.minIdleMs)
            delay = s.minIdleMs;
        if (delay > s.maxIdleMs)
            delay = s.maxIdleMs;

        bool const relaxed = f.inRestArea || f.activity == Activity::Resting || f.activity == Activity::SocialAfk;
        if (!relaxed)
            delay *= s.openWorldFactor ? s.openWorldFactor : 1;

        return static_cast<uint32_t>(delay);
    }

    inline bool Busy(Facts const& f)
    {
        return !f.alive || f.inCombat || f.moving || f.mounted || f.casting || f.swimming || f.inFlight;
    }

    inline Posture Decide(Facts const& f, uint32_t botId, uint32_t spellRoll, Settings const& s)
    {
        if (!s.enabled)
            return f.sitting && !f.followingPlayer ? Posture::Stand : Posture::Keep;

        if (f.followingPlayer)
            return Posture::Keep;

        if (Busy(f))
            return f.sitting ? Posture::Stand : Posture::Keep;

        if (f.sitting)
            return Posture::Sit;

        if (f.idleMs < SitDelayMs(botId, spellRoll, f, s))
            return Posture::Keep;

        // Some idle spells a person just stands around. The roll is fixed per spell, so this is decided
        // once rather than retried every tick until it comes up "sit".
        return (spellRoll % 100) < s.sitChancePct ? Posture::Sit : Posture::Keep;
    }

    // A short dance in town now and then, only standing, settled, and not too often.
    inline bool ShouldStartDance(Facts const& f, uint32_t botId, uint32_t spellRoll, uint32_t tickRoll,
                                 uint32_t msSinceLastDance, Settings const& s)
    {
        if (!s.enabled || !s.danceChancePct || f.followingPlayer || Busy(f) || f.sitting || !f.inRestArea)
            return false;
        if (msSinceLastDance < s.danceCooldownMs)
            return false;
        if (f.idleMs < SitDelayMs(botId, spellRoll, f, s))
            return false;
        return (tickRoll % 100) < s.danceChancePct;
    }

    inline bool ShouldStopDance(Facts const& f, uint32_t danceElapsedMs, uint32_t danceDurationMs)
    {
        return Busy(f) || f.sitting || danceElapsedMs >= danceDurationMs;
    }
}  // namespace IdlePosturePolicy

#endif
