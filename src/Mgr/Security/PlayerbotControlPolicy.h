/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTCONTROLPOLICY_H
#define PLAYERBOTS_PLAYERBOTCONTROLPOLICY_H

#include <cctype>
#include <string>

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

    // Does a character parse chat as bot commands at all? A persona bot that takes no orders must not
    // react to "who", "wts", "invite", "leave" or anything else a person might type to it: every one of
    // those answers (stat lines, price lists, "Invite me to your group first") gives the bot away.
    // Its replies come from the chat module like anyone else's.
    inline bool AcceptsChatCommands(bool isRandomBotAccount, bool fromIsGameMaster, bool fromIsSelf,
                                    bool randomBotPlayerControl)
    {
        if (!isRandomBotAccount || fromIsSelf)
            return true;
        return fromIsGameMaster || randomBotPlayerControl;
    }

    // Does a refused request get a canned explanation whispered back ("You are too low level: 5/60",
    // "I have a master already")? Never from a persona bot: people just ignore requests.
    inline bool ExplainsDenials(bool isRandomBotAccount, bool fromIsGameMaster, bool randomBotPlayerControl)
    {
        if (!isRandomBotAccount)
            return true;
        return fromIsGameMaster || randomBotPlayerControl;
    }

    // Would this character name give the bot away? "Botrag" and "Kaidzubot" come from the stock name
    // pool; a person reading "bot" in a name assumes the obvious.
    inline bool NameRevealsBot(std::string const& name)
    {
        std::string lower;
        lower.reserve(name.size());
        for (char c : name)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return lower.find("bot") != std::string::npos;
    }
}  // namespace PlayerbotControlPolicy

#endif
