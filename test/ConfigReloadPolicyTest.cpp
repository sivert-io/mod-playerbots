// Standalone test for src/Bot/ConfigReloadPolicy.h (not part of the module build). Run from the module root:
//   c++ -std=c++17 -I src/Bot test/ConfigReloadPolicyTest.cpp -o /tmp/reload-test && /tmp/reload-test

#include "ConfigReloadPolicy.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

using namespace ConfigReloadPolicy;

static int failures = 0;

static void Expect(bool ok, std::string const& name)
{
    if (!ok)
        ++failures;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", name.c_str());
}

static void Classification()
{
    Expect(Classify("AiPlayerbot.BotActiveAlone") == KeyClass::Live, "BotActiveAlone is live");
    Expect(Classify("AiPlayerbot.botActiveAloneSmartScaleDiffLimitfloor") == KeyClass::Live, "smart scale keys are live");
    Expect(Classify("AiPlayerbot.RandomBotTalk") == KeyClass::Live, "RandomBotTalk is live");
    Expect(Classify("AiPlayerbot.BroadcastChanceKillElite") == KeyClass::Live, "broadcast chances are live");
    Expect(Classify("AiPlayerbot.AllowGuildBots") == KeyClass::Live, "AllowGuildBots is live");
    Expect(Classify("AiPlayerbot.RandomBotPlayerControl") == KeyClass::Live, "RandomBotPlayerControl is live");
    Expect(Classify("AiPlayerbot.Arrivals.MaxPerHour") == KeyClass::Live, "Arrivals.MaxPerHour is live");
    Expect(Classify("AiPlayerbot.Lifecycle.SessionsPerWeek") == KeyClass::Live, "Lifecycle.SessionsPerWeek is live");

    Expect(Classify("AiPlayerbot.Arrivals.Enable") == KeyClass::Restart, "Arrivals.Enable needs a restart");
    Expect(Classify("AiPlayerbot.Lifecycle.Enable") == KeyClass::Restart, "Lifecycle.Enable needs a restart");
    Expect(Classify("AiPlayerbot.MaxRandomBots") == KeyClass::Restart, "MaxRandomBots is guarded");
    Expect(Classify("AiPlayerbot.RandomBotAccountCount") == KeyClass::Restart, "RandomBotAccountCount is guarded");
    Expect(Classify("AiPlayerbot.DeleteRandomBotAccounts") == KeyClass::Restart, "DeleteRandomBotAccounts is guarded");
    Expect(Classify("AiPlayerbot.RandomBotMaps") == KeyClass::Restart, "RandomBotMaps needs a restart");
    Expect(Classify("PlayerbotsDatabaseInfo") == KeyClass::Restart, "database info needs a restart");
    Expect(Classify("AiPlayerbot.SomethingNew") == KeyClass::Restart, "unknown keys default to restart");
    Expect(RestartReason("AiPlayerbot.SomethingNew") == "only read at startup", "default restart reason");
}

static void Diffing()
{
    Snapshot before = {{"AiPlayerbot.BotActiveAlone", "100"},
                       {"AiPlayerbot.MaxRandomBots", "5000"},
                       {"AiPlayerbot.RandomBotTalk", "1"},
                       {"AiPlayerbot.Removed", "x"},
                       {"PlayerbotsDatabaseInfo", "host;3306;user;hunter2;db"}};
    Snapshot after = {{"AiPlayerbot.Arrivals.PerYear", "8000"},
                      {"AiPlayerbot.BotActiveAlone", "60"},
                      {"AiPlayerbot.MaxRandomBots", "100"},
                      {"AiPlayerbot.RandomBotTalk", "1"},
                      {"PlayerbotsDatabaseInfo", "host;3306;user;swordfish;db"}};

    std::vector<Change> const changes = Diff(before, after);
    Expect(changes.size() == 5, "diff finds added, removed and changed keys, not unchanged ones");
    Expect(CountLive(changes) == 2, "two of them apply live");
    Expect(AffectsArrivalPlan(changes), "PerYear change re-plans arrivals");

    bool addedOk = false, removedOk = false;
    for (Change const& c : changes)
    {
        addedOk |= c.key == "AiPlayerbot.Arrivals.PerYear" && c.before == UNSET && c.after == "8000";
        removedOk |= c.key == "AiPlayerbot.Removed" && c.after == UNSET && c.cls == KeyClass::Restart;
    }
    Expect(addedOk, "added key reported as <unset> -> value");
    Expect(removedOk, "removed key reported as value -> <unset>");

    std::string const summary = Summary(changes);
    Expect(summary.rfind("playerbots config reloaded: 5 keys changed:", 0) == 0, "summary starts with the count");
    Expect(summary.find("AiPlayerbot.BotActiveAlone 100 -> 60") != std::string::npos, "summary shows live change");
    Expect(summary.find("needs a restart: ") != std::string::npos, "summary lists restart keys");
    Expect(summary.find("hunter2") == std::string::npos && summary.find("swordfish") == std::string::npos,
           "summary never prints database credentials");
    Expect(Summary({}) == "playerbots config reloaded: 0 keys changed", "empty summary");

    // Restart-only changes stay pending across reloads, live ones are taken
    Snapshot const next = NextSnapshot(before, after);
    Expect(next.at("AiPlayerbot.BotActiveAlone") == "60", "next snapshot takes live values");
    Expect(next.at("AiPlayerbot.MaxRandomBots") == "5000", "next snapshot keeps the old guarded value");
    Expect(next.at("AiPlayerbot.Removed") == "x", "next snapshot keeps a removed restart-only key");
    Expect(next.at("AiPlayerbot.Arrivals.PerYear") == "8000", "next snapshot takes an added live key");
    std::vector<Change> const again = Diff(next, after);
    Expect(again.size() == 3 && CountLive(again) == 0, "a second identical reload applies nothing and still lists the 3 restart keys");

    Snapshot const onlyRate = {{"AiPlayerbot.Arrivals.MaxPerHour", "60"}};
    Expect(!AffectsArrivalPlan(Diff({{"AiPlayerbot.Arrivals.MaxPerHour", "8"}}, onlyRate)),
           "MaxPerHour alone does not re-plan the week");
    Expect(!AffectsArrivalPlan(Diff({}, {{"AiPlayerbot.Arrivals.Enable", "0"}})), "restart-only Enable never re-plans");
}

// Every key read in PlayerbotAIConfig::LoadLiveOptions() must be live, and every exact live rule must be read
// there (or by LoadRandomBotLevelConfig for the level manager keys), so the policy and the code cannot drift.
static void MatchesSource()
{
    std::ifstream file("src/PlayerbotAIConfig.cpp");
    if (!file)
    {
        Expect(false, "src/PlayerbotAIConfig.cpp readable (run from the module root)");
        return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string const source = buffer.str();

    size_t const begin = source.find("void PlayerbotAIConfig::LoadLiveOptions()");
    size_t const end = source.find("\n}\n", begin);
    Expect(begin != std::string::npos && end != std::string::npos, "LoadLiveOptions found");
    if (begin == std::string::npos || end == std::string::npos)
        return;
    std::string const body = source.substr(begin, end - begin);

    std::set<std::string> read;
    std::regex const keyRe("\"(AiPlayerbot\\.[A-Za-z0-9_.]+)\"");
    for (std::sregex_iterator it(body.begin(), body.end(), keyRe), stop; it != stop; ++it)
        read.insert((*it)[1]);
    Expect(read.size() > 150, "LoadLiveOptions reads the expected number of keys (" + std::to_string(read.size()) + ")");

    for (std::string const& key : read)
        if (Classify(key) != KeyClass::Live)
            Expect(false, "read live but classified restart: " + key);

    for (Rule const& rule : Rules())
    {
        std::string const pattern = rule.pattern;
        if (rule.cls != KeyClass::Live || pattern.rfind("AiPlayerbot.LevelBrackets", 0) == 0 ||
            pattern.rfind("AiPlayerbot.ResetBotLevel", 0) == 0)
            continue;
        bool found = false;
        for (std::string const& key : read)
            found |= Matches(pattern, key);
        if (!found)
            Expect(false, "live rule matches nothing LoadLiveOptions reads: " + pattern);
    }

    for (char const* guarded : {"AiPlayerbot.Arrivals.Enable", "AiPlayerbot.Lifecycle.Enable", "AiPlayerbot.MaxRandomBots",
                                "AiPlayerbot.MinRandomBots", "AiPlayerbot.RandomBotAccountCount"})
        Expect(!read.count(guarded), std::string("LoadLiveOptions does not read ") + guarded);
    Expect(true, "source and policy agree");
}

int main()
{
    Classification();
    Diffing();
    MatchesSource();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
