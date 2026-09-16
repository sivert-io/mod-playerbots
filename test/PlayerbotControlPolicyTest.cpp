// Standalone test for src/Mgr/Security/PlayerbotControlPolicy.h (not part of the module build).
//   c++ -std=c++17 -I src/Mgr/Security test/PlayerbotControlPolicyTest.cpp -o /tmp/policy-test && /tmp/policy-test

#include "PlayerbotControlPolicy.h"

#include <cstdio>

using PlayerbotControlPolicy::AltBotRequest;
using PlayerbotControlPolicy::CanAddAltBot;
using PlayerbotControlPolicy::RandomBotObeysMaster;

static int failures = 0;

static void Expect(bool actual, bool expected, char const* name)
{
    if (actual != expected)
    {
        ++failures;
        std::printf("FAIL %s: expected %d got %d\n", name, expected, actual);
    }
    else
        std::printf("ok   %s\n", name);
}

// Astro Realm live settings after the fix
static AltBotRequest Astro()
{
    AltBotRequest r;
    r.allowAccountBots = true;
    r.allowGuildBots = false;
    r.allowTrustedAccountBots = false;
    r.randomBotPlayerControl = false;
    return r;
}

// Upstream defaults (what cs2 ran before the fix)
static AltBotRequest Upstream()
{
    AltBotRequest r;
    r.allowAccountBots = true;
    r.allowGuildBots = true;
    r.allowTrustedAccountBots = true;
    r.randomBotPlayerControl = true;
    return r;
}

int main()
{
    AltBotRequest r;

    r = Astro(); r.sameAccount = true;
    Expect(CanAddAltBot(r), true, "own account alt can be added");

    r = Astro(); r.sameGuild = true;
    Expect(CanAddAltBot(r), false, "guild mate's character cannot be added");

    r = Upstream(); r.sameGuild = true;
    Expect(CanAddAltBot(r), true, "before fix: guild mate's character could be added");

    r = Astro(); r.linkedAccount = true;
    Expect(CanAddAltBot(r), false, "linked account character cannot be added");

    r = Astro();
    Expect(CanAddAltBot(r), false, "unrelated account character cannot be added");

    r = Upstream();
    Expect(CanAddAltBot(r), false, "unrelated account character never could be added");

    r = Astro(); r.allowGuildBots = true; r.sameGuild = true; r.targetIsRandomBotAccount = true;
    Expect(CanAddAltBot(r), false, "persona bot in own guild cannot be added even with guild bots on");

    r = Astro(); r.sameGuild = true; r.allowGuildBots = true; r.targetIsRandomBotAccount = true;
    r.masterIsGameMaster = true;
    Expect(CanAddAltBot(r), true, "GM keeps guild path to persona bots");

    r = Astro(); r.targetIsRandomBotAccount = true; r.addClassBot = true;
    Expect(CanAddAltBot(r), true, "addclass bots stay addable");

    r = Astro(); r.randomBotManager = true; r.targetIsRandomBotAccount = true;
    Expect(CanAddAltBot(r), true, "random bot manager logs persona bots in");

    Expect(RandomBotObeysMaster(false, false), false, "player master cannot command persona bot");
    Expect(RandomBotObeysMaster(true, false), true, "GM can command persona bot");
    Expect(RandomBotObeysMaster(false, true), true, "before fix: player master could command random bot");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
