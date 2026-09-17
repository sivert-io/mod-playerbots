/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Astro Realm admin commands (GM, also from the console and SOAP):
//   .astro arrivals status
//   .astro arrivals add <count> [over <hours>] [rate <perHour>]
//   .astro arrivals rate <perHour>
//   .astro arrivals pause | resume

#include "BotLifecycleMgr.h"
#include "Chat.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include <algorithm>
#include <cctype>
#include <sstream>

using namespace Acore::ChatCommands;

namespace
{
std::string Actor(ChatHandler* handler)
{
    WorldSession* session = handler->GetSession();
    if (!session)
        return "console/SOAP";
    Player* player = session->GetPlayer();
    return (player ? player->GetName() : std::string("?")) + " (account " + std::to_string(session->GetAccountId()) + ")";
}

void Print(ChatHandler* handler, std::vector<std::string> const& lines)
{
    for (std::string const& line : lines)
        handler->SendSysMessage(line);
}

// "36", "36h" -> 36; false for anything else
bool ParseNumber(std::string token, uint32& value)
{
    if (!token.empty() && (token.back() == 'h' || token.back() == 'H'))
        token.pop_back();
    if (token.empty() || token.size() > 9 || !std::all_of(token.begin(), token.end(), ::isdigit))
        return false;
    value = uint32(std::stoul(token));
    return true;
}
}  // namespace

class astro_commandscript : public CommandScript
{
public:
    astro_commandscript() : CommandScript("astro_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable arrivalsTable = {
            {"status", HandleStatus, SEC_GAMEMASTER, Console::Yes},
            {"add", HandleAdd, SEC_GAMEMASTER, Console::Yes},
            {"rate", HandleRate, SEC_GAMEMASTER, Console::Yes},
            {"pause", HandlePause, SEC_GAMEMASTER, Console::Yes},
            {"resume", HandleResume, SEC_GAMEMASTER, Console::Yes},
        };
        static ChatCommandTable astroTable = {
            {"arrivals", arrivalsTable},
        };
        static ChatCommandTable commandTable = {
            {"astro", astroTable},
        };
        return commandTable;
    }

    static bool HandleStatus(ChatHandler* handler, char const* /*args*/)
    {
        Print(handler, sBotLifecycleMgr.AdminStatus());
        return true;
    }

    static bool HandleAdd(ChatHandler* handler, char const* args)
    {
        std::istringstream in(args ? args : "");
        std::vector<std::string> tokens;
        for (std::string token; in >> token;)
            tokens.push_back(token);

        uint32 count = 0, hours = 24, rate = 0;
        bool ok = !tokens.empty() && ParseNumber(tokens[0], count) && tokens[0].back() != 'h';
        for (size_t i = 1; ok && i < tokens.size(); i += 2)
        {
            if (i + 1 >= tokens.size())
                ok = false;
            else if (tokens[i] == "over")
                ok = ParseNumber(tokens[i + 1], hours);
            else if (tokens[i] == "rate" || tokens[i] == "cap")
                ok = ParseNumber(tokens[i + 1], rate) && tokens[i + 1].back() != 'h';
            else
                ok = false;
        }
        if (!ok)
        {
            handler->SendSysMessage("Usage: .astro arrivals add <count> [over <hours>] [rate <perHour>]  (default over 24h)");
            return false;
        }

        Print(handler, sBotLifecycleMgr.AdminAddArrivals(count, hours, rate, Actor(handler)));
        return true;
    }

    static bool HandleRate(ChatHandler* handler, char const* args)
    {
        uint32 rate = 0;
        std::string token;
        std::istringstream(args ? args : "") >> token;
        if (!ParseNumber(token, rate) || token.back() == 'h')
        {
            handler->SendSysMessage("Usage: .astro arrivals rate <perHour>  (0 = no hourly cap, until the next .reload config)");
            return false;
        }

        Print(handler, sBotLifecycleMgr.AdminSetRate(rate, Actor(handler)));
        return true;
    }

    static bool HandlePause(ChatHandler* handler, char const* /*args*/)
    {
        Print(handler, sBotLifecycleMgr.AdminPause(true, Actor(handler)));
        return true;
    }

    static bool HandleResume(ChatHandler* handler, char const* /*args*/)
    {
        Print(handler, sBotLifecycleMgr.AdminPause(false, Actor(handler)));
        return true;
    }
};

void AddAstroCommandScripts() { new astro_commandscript(); }
