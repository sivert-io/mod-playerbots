/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTCONTROLPOLICY_H
#define PLAYERBOTS_PLAYERBOTCONTROLPOLICY_H

// Who may take control of which character. Pure functions with no core dependencies so the rules
// can be tested without a server (see test/PlayerbotControlPolicyTest.cpp).
namespace PlayerbotControlPolicy
{
    struct AltBotRequest
    {
        // Settings
        bool allowAccountBots = false;
        bool allowGuildBots = false;
        bool allowTrustedAccountBots = false;
        bool randomBotPlayerControl = false;

        // Facts about the request
        bool randomBotManager = false;    // the random bot manager logs the bot in (no human master)
        bool masterIsGameMaster = false;
        bool sameAccount = false;
        bool sameGuild = false;
        bool linkedAccount = false;
        bool addClassBot = false;
        bool targetIsRandomBotAccount = false;
    };

    // May the character be logged in as a bot controlled by the requesting master?
    inline bool CanAddAltBot(AltBotRequest const& r)
    {
        if (r.randomBotManager)
            return true;

        // Random bot (persona) characters are never handed to a normal player as an alt bot, whatever
        // guild or link they share, unless player control of random bots is switched on.
        if (r.targetIsRandomBotAccount && !r.addClassBot && !r.randomBotPlayerControl && !r.masterIsGameMaster)
            return false;

        return (r.allowAccountBots && r.sameAccount) || (r.allowGuildBots && r.sameGuild) ||
               (r.allowTrustedAccountBots && r.linkedAccount) || r.addClassBot;
    }

    // Does a random bot take orders ("give", "trade", "destroy", "summon", ...) from a player it treats as master?
    inline bool RandomBotObeysMaster(bool fromIsGameMaster, bool randomBotPlayerControl)
    {
        return fromIsGameMaster || randomBotPlayerControl;
    }
}  // namespace PlayerbotControlPolicy

#endif
