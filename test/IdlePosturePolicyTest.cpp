// Standalone test for src/Ai/World/Rpg/IdlePosturePolicy.h (not part of the module build).
//   c++ -std=c++17 -I src/Ai/World/Rpg test/IdlePosturePolicyTest.cpp -o /tmp/idle-test && /tmp/idle-test

#include "IdlePosturePolicy.h"

#include <cstdio>

using namespace IdlePosturePolicy;

static int failures = 0;

static void Expect(bool ok, char const* name)
{
    if (!ok)
    {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
    else
        std::printf("ok   %s\n", name);
}

static Facts InTown(uint32_t idleMs)
{
    Facts f;
    f.inRestArea = true;
    f.activity = Activity::SocialAfk;
    f.idleMs = idleMs;
    return f;
}

int main()
{
    Settings s;
    uint32_t const bot = 4711;
    uint32_t const sitRoll = 10;    // this idle spell ends in sitting (10 < 75)
    uint32_t const standRoll = 90;  // this one does not

    // Timing
    for (uint32_t id = 1; id < 2000; id += 37)
        for (uint32_t roll = 0; roll < 100; roll += 7)
        {
            uint32_t const d = SitDelayMs(id, roll, InTown(0), s);
            if (d < s.minIdleMs || d > s.maxIdleMs)
            {
                Expect(false, "town sit delay stays within 20-40 s");
                id = 2000;
                break;
            }
        }
    Expect(SitDelayMs(1, 50, InTown(0), s) != SitDelayMs(2, 50, InTown(0), s) ||
               SitDelayMs(3, 50, InTown(0), s) != SitDelayMs(4, 50, InTown(0), s),
           "different bots wait different times");

    Facts grind;
    grind.activity = Activity::Other;
    Expect(SitDelayMs(bot, 50, grind, s) >= 2 * s.minIdleMs, "open world waits at least twice as long");

    // Sitting down
    Expect(Decide(InTown(5000), bot, sitRoll, s) == Posture::Keep, "just stopped: keeps standing");
    Expect(Decide(InTown(19999), bot, sitRoll, s) == Posture::Keep, "under 20 s: keeps standing");
    Expect(Decide(InTown(41000), bot, sitRoll, s) == Posture::Sit, "idle 41 s in town: sits");
    Expect(Decide(InTown(41000), bot, standRoll, s) == Posture::Keep, "some idle spells stay standing");

    Facts grinding = grind;
    grinding.idleMs = 35000;
    Expect(Decide(grinding, bot, sitRoll, s) == Posture::Keep, "35 s pause mid-grind: no sitting");
    grinding.idleMs = 81000;
    Expect(Decide(grinding, bot, sitRoll, s) == Posture::Sit, "81 s waiting for a respawn: sits");

    // Standing up
    Facts f = InTown(60000);
    f.sitting = true;
    Expect(Decide(f, bot, sitRoll, s) == Posture::Sit, "seated and idle: stays seated (eating keeps going)");
    f.moving = true;
    Expect(Decide(f, bot, sitRoll, s) == Posture::Stand, "stands before moving");
    f.moving = false;
    f.inCombat = true;
    Expect(Decide(f, bot, sitRoll, s) == Posture::Stand, "stands in combat");
    f.inCombat = false;
    f.mounted = true;
    Expect(Decide(f, bot, sitRoll, s) == Posture::Stand, "stands when mounting");
    f.mounted = false;

    Facts busy = InTown(90000);
    busy.casting = true;
    Expect(Decide(busy, bot, sitRoll, s) == Posture::Keep, "not sitting down mid-cast");

    Facts follow = InTown(90000);
    follow.followingPlayer = true;
    follow.sitting = true;
    Expect(Decide(follow, bot, sitRoll, s) == Posture::Keep, "grouped bot mirrors the player instead");

    // Disabled: the old behaviour (stand a sitting bot up)
    Settings off = s;
    off.enabled = false;
    Expect(Decide(f, bot, sitRoll, off) == Posture::Stand, "disabled: sitting bot stands up as before");
    Expect(Decide(InTown(90000), bot, sitRoll, off) == Posture::Keep, "disabled: never sits on its own");

    // Dancing
    Expect(ShouldStartDance(InTown(60000), bot, sitRoll, 1, 3600000, s), "settled in town, lucky roll: dances");
    Expect(!ShouldStartDance(InTown(60000), bot, sitRoll, 50, 3600000, s), "unlucky roll: no dance");
    Expect(!ShouldStartDance(InTown(60000), bot, sitRoll, 1, 60000, s), "danced a minute ago: no dance");
    Expect(!ShouldStartDance(InTown(5000), bot, sitRoll, 1, 3600000, s), "just arrived: no dance");
    Facts field = grind;
    field.idleMs = 600000;
    Expect(!ShouldStartDance(field, bot, sitRoll, 1, 3600000, s), "no dancing out in the field");
    Facts seated = InTown(60000);
    seated.sitting = true;
    Expect(!ShouldStartDance(seated, bot, sitRoll, 1, 3600000, s), "no dancing while seated");
    Expect(ShouldStopDance(InTown(60000), 26000, 25000), "dance ends after its duration");
    Facts walking = InTown(0);
    walking.moving = true;
    Expect(ShouldStopDance(walking, 1000, 25000), "dance stops when walking off");
    Expect(!ShouldStopDance(InTown(60000), 1000, 25000), "dance goes on");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
