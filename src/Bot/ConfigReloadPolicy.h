/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CONFIGRELOADPOLICY_H
#define PLAYERBOTS_CONFIGRELOADPOLICY_H

// Which playerbots.conf keys ".reload config" applies to a running server, and the diff/summary logic
// around it. Plain C++ with no AzerothCore includes so test/ConfigReloadPolicyTest.cpp builds it standalone.

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ConfigReloadPolicy
{
enum class KeyClass
{
    Live,     // applied on reload
    Restart,  // only read at startup, the change waits for the next restart
};

struct Rule
{
    char const* pattern;  // exact key, or a prefix when it ends with '*'
    KeyClass cls;
    char const* reason;
};

// First match wins. The Restart rules with a reason come first so a Live prefix never swallows them.
// Every key PlayerbotAIConfig::LoadLiveOptions() reads must match a Live rule and every exact Live rule
// must be read there; test/ConfigReloadPolicyTest.cpp checks both against the source.
inline std::vector<Rule> const& Rules()
{
    static std::vector<Rule> const rules = {
        // Guarded: never applied live
        {"AiPlayerbot.Enabled", KeyClass::Restart, "turns the whole module on or off"},
        {"AiPlayerbot.Lifecycle.Enable", KeyClass::Restart, "the lifecycle loads personas and tables at startup"},
        {"AiPlayerbot.Arrivals.Enable", KeyClass::Restart, "arrivals reserve the bot account list at startup"},
        {"AiPlayerbot.MinRandomBots", KeyClass::Restart, "bot pool size, used at startup (guarded: could mass create or log out bots)"},
        {"AiPlayerbot.MaxRandomBots", KeyClass::Restart, "bot pool size, used at startup (guarded: could mass create or log out bots)"},
        {"AiPlayerbot.RandomBotAccountCount", KeyClass::Restart, "accounts are created at startup"},
        {"AiPlayerbot.RandomBotAccountPrefix", KeyClass::Restart, "accounts are matched by prefix at startup"},
        {"AiPlayerbot.DeleteRandomBot*", KeyClass::Restart, "deletion only runs at startup"},
        {"AiPlayerbot.RandomBotMaps", KeyClass::Restart, "teleport locations are precomputed at startup"},
        {"AiPlayerbot.ZoneBracket.*", KeyClass::Restart, "teleport locations are precomputed at startup"},
        {"AiPlayerbot.PopulationCurve.UtcOffsetMinutes", KeyClass::Restart, "moves the arrivals week boundary"},
        {"AiPlayerbot.PopulationCurve.EuropeanDst", KeyClass::Restart, "moves the arrivals week boundary"},
        {"AiPlayerbot.RandomBotSuggestDungeons", KeyClass::Restart, "dungeon suggestions are loaded at startup"},
        {"PlayerbotsDatabaseInfo", KeyClass::Restart, "database connections are opened at startup"},
        {"Playerbots*", KeyClass::Restart, "database settings are used at startup"},

        // Live: timings and movement
        {"AiPlayerbot.GlobalCooldown", KeyClass::Live, ""},
        {"AiPlayerbot.MaxWaitForMove", KeyClass::Live, ""},
        {"AiPlayerbot.DisableMoveSplinePath", KeyClass::Live, ""},
        {"AiPlayerbot.MaxMovementSearchTime", KeyClass::Live, ""},
        {"AiPlayerbot.RealisticTurning.*", KeyClass::Live, ""},
        {"AiPlayerbot.ExpireActionTime", KeyClass::Live, ""},
        {"AiPlayerbot.DispelAuraDuration", KeyClass::Live, ""},
        {"AiPlayerbot.ReactDelay", KeyClass::Live, ""},
        {"AiPlayerbot.DynamicReactDelay", KeyClass::Live, ""},
        {"AiPlayerbot.PassiveDelay", KeyClass::Live, ""},
        {"AiPlayerbot.RepeatDelay", KeyClass::Live, ""},
        {"AiPlayerbot.ErrorDelay", KeyClass::Live, ""},
        {"AiPlayerbot.RpgDelay", KeyClass::Live, ""},
        {"AiPlayerbot.SitDelay", KeyClass::Live, ""},
        {"AiPlayerbot.ReturnDelay", KeyClass::Live, ""},
        {"AiPlayerbot.LootDelay", KeyClass::Live, ""},
        {"AiPlayerbot.DisabledWithoutRealPlayer*", KeyClass::Live, ""},
        {"AiPlayerbot.FarDistance", KeyClass::Live, ""},
        {"AiPlayerbot.SightDistance", KeyClass::Live, ""},
        {"AiPlayerbot.SpellDistance", KeyClass::Live, ""},
        {"AiPlayerbot.ShootDistance", KeyClass::Live, ""},
        {"AiPlayerbot.HealDistance", KeyClass::Live, ""},
        {"AiPlayerbot.LootDistance", KeyClass::Live, ""},
        {"AiPlayerbot.FleeDistance", KeyClass::Live, ""},
        {"AiPlayerbot.AggroDistance", KeyClass::Live, ""},
        {"AiPlayerbot.TooCloseDistance", KeyClass::Live, ""},
        {"AiPlayerbot.MeleeDistance", KeyClass::Live, ""},
        {"AiPlayerbot.FollowDistance", KeyClass::Live, ""},
        {"AiPlayerbot.WhisperDistance", KeyClass::Live, ""},
        {"AiPlayerbot.ContactDistance", KeyClass::Live, ""},
        {"AiPlayerbot.AoeRadius", KeyClass::Live, ""},
        {"AiPlayerbot.RpgDistance", KeyClass::Live, ""},
        {"AiPlayerbot.GrindDistance", KeyClass::Live, ""},
        {"AiPlayerbot.ReactDistance", KeyClass::Live, ""},
        {"AiPlayerbot.RandombotsWalkingRPG*", KeyClass::Live, ""},
        {"AiPlayerbot.UseGroundMountAtMinLevel", KeyClass::Live, ""},
        {"AiPlayerbot.UseFastGroundMountAtMinLevel", KeyClass::Live, ""},
        {"AiPlayerbot.UseFlyMountAtMinLevel", KeyClass::Live, ""},
        {"AiPlayerbot.UseFastFlyMountAtMinLevel", KeyClass::Live, ""},
        {"AiPlayerbot.BotTaxi*", KeyClass::Live, ""},

        // Live: combat and buffs
        {"AiPlayerbot.CriticalHealth", KeyClass::Live, ""},
        {"AiPlayerbot.LowHealth", KeyClass::Live, ""},
        {"AiPlayerbot.MediumHealth", KeyClass::Live, ""},
        {"AiPlayerbot.AlmostFullHealth", KeyClass::Live, ""},
        {"AiPlayerbot.LowMana", KeyClass::Live, ""},
        {"AiPlayerbot.MediumMana", KeyClass::Live, ""},
        {"AiPlayerbot.HighMana", KeyClass::Live, ""},
        {"AiPlayerbot.AutoSaveMana", KeyClass::Live, ""},
        {"AiPlayerbot.SaveManaThreshold", KeyClass::Live, ""},
        {"AiPlayerbot.AutoGreaterBlessings", KeyClass::Live, ""},
        {"AiPlayerbot.AutoPartyBuffs", KeyClass::Live, ""},
        {"AiPlayerbot.TellWhenMissingBuffReagents", KeyClass::Live, ""},
        {"AiPlayerbot.MissingBuffReagentMessageCooldown", KeyClass::Live, ""},
        {"AiPlayerbot.ForceRebuffOnReadyCheck", KeyClass::Live, ""},
        {"AiPlayerbot.ForceRebuffMarginSecs", KeyClass::Live, ""},
        {"AiPlayerbot.AutoAvoidAoe", KeyClass::Live, ""},
        {"AiPlayerbot.MaxAoeAvoidRadius", KeyClass::Live, ""},
        {"AiPlayerbot.AoeAvoidSpellWhitelist", KeyClass::Live, ""},
        {"AiPlayerbot.TellWhenAvoidAoe", KeyClass::Live, ""},
        {"AiPlayerbot.BotCheats", KeyClass::Live, ""},

        // Live: gear, loot, quests, progression of future actions
        {"AiPlayerbot.RandomGearLoweringChance", KeyClass::Live, ""},
        {"AiPlayerbot.RandomGearQualityLimit", KeyClass::Live, ""},
        {"AiPlayerbot.RandomGearScoreLimit", KeyClass::Live, ""},
        {"AiPlayerbot.PreferClassArmorType", KeyClass::Live, ""},
        {"AiPlayerbot.PreferredSpecWeapons", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotMinLevelChance", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotMaxLevelChance", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotRpgChance", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotXPRate", KeyClass::Live, ""},
        {"AiPlayerbot.EnableRandomBotTrading", KeyClass::Live, ""},
        {"AiPlayerbot.GearScoreCheck", KeyClass::Live, ""},
        {"AiPlayerbot.PreQuests", KeyClass::Live, ""},
        {"AiPlayerbot.FreeMethodLoot", KeyClass::Live, ""},
        {"AiPlayerbot.LootNeedRollLevel", KeyClass::Live, ""},
        {"AiPlayerbot.LootRollRecipe", KeyClass::Live, ""},
        {"AiPlayerbot.LootRollDisenchant", KeyClass::Live, ""},
        {"AiPlayerbot.LootGreedRollLevel", KeyClass::Live, ""},
        {"AiPlayerbot.AutoPickReward", KeyClass::Live, ""},
        {"AiPlayerbot.AutoEquipUpgradeLoot", KeyClass::Live, ""},
        {"AiPlayerbot.EquipUpgradeThreshold", KeyClass::Live, ""},
        {"AiPlayerbot.TwoRoundsGearInit", KeyClass::Live, ""},
        {"AiPlayerbot.SyncQuestWithPlayer", KeyClass::Live, ""},
        {"AiPlayerbot.SyncQuestForPlayer", KeyClass::Live, ""},
        {"AiPlayerbot.DropObsoleteQuests", KeyClass::Live, ""},
        {"AiPlayerbot.AllowLearnTrainerSpells", KeyClass::Live, ""},
        {"AiPlayerbot.AutoPickTalents", KeyClass::Live, ""},
        {"AiPlayerbot.AutoUpgradeEquip", KeyClass::Live, ""},
        {"AiPlayerbot.HunterWolfPet", KeyClass::Live, ""},
        {"AiPlayerbot.DefaultPetStance", KeyClass::Live, ""},
        {"AiPlayerbot.PetChatCommandDebug", KeyClass::Live, ""},
        {"AiPlayerbot.AutoLearnTrainerSpells", KeyClass::Live, ""},
        {"AiPlayerbot.AutoLearnQuestSpells", KeyClass::Live, ""},
        {"AiPlayerbot.AutoTeleportForLevel", KeyClass::Live, ""},
        {"AiPlayerbot.AutoDoQuests", KeyClass::Live, ""},
        {"AiPlayerbot.RpgStatusProbWeight.*", KeyClass::Live, ""},
        {"AiPlayerbot.RpgSocialAfk.*", KeyClass::Live, ""},

        // Live: security and grouping
        {"AiPlayerbot.AllowAccountBots", KeyClass::Live, ""},
        {"AiPlayerbot.AllowGuildBots", KeyClass::Live, ""},
        {"AiPlayerbot.AllowTrustedAccountBots", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotPlayerControl", KeyClass::Live, ""},
        {"AiPlayerbot.GroupInvitationPermission", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotGuildNearby", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotInvitePlayer", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotGroupNearby", KeyClass::Live, ""},
        {"AiPlayerbot.InviteChat", KeyClass::Live, ""},
        {"AiPlayerbot.SummonWhenGroup", KeyClass::Live, ""},
        {"AiPlayerbot.KeepAltsInGroup", KeyClass::Live, ""},
        {"AiPlayerbot.AllowSummonInCombat", KeyClass::Live, ""},
        {"AiPlayerbot.AllowSummonWhenMasterIsDead", KeyClass::Live, ""},
        {"AiPlayerbot.AllowSummonWhenBotIsDead", KeyClass::Live, ""},
        {"AiPlayerbot.ReviveBotWhenSummoned", KeyClass::Live, ""},
        {"AiPlayerbot.BotRepairWhenSummon", KeyClass::Live, ""},

        // Live: chat, emotes, broadcasts
        {"AiPlayerbot.EnableBroadcasts", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotTalk", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotEmote", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotSayWithoutMaster", KeyClass::Live, ""},
        {"AiPlayerbot.EnableGreet", KeyClass::Live, ""},
        {"AiPlayerbot.BroadcastTo*", KeyClass::Live, ""},
        {"AiPlayerbot.BroadcastChance*", KeyClass::Live, ""},
        {"AiPlayerbot.ToxicLinksPrefix", KeyClass::Live, ""},
        {"AiPlayerbot.ToxicLinksRepliesChance", KeyClass::Live, ""},
        {"AiPlayerbot.ThunderfuryRepliesChance", KeyClass::Live, ""},
        {"AiPlayerbot.GuildRepliesRate", KeyClass::Live, ""},

        // Live: activity scaling
        {"AiPlayerbot.BotActiveAlone*", KeyClass::Live, ""},
        {"AiPlayerbot.botActiveAloneSmartScale*", KeyClass::Live, ""},

        // Live: random bot manager timers
        {"AiPlayerbot.RandomBotUpdateInterval", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotCountChangeMinInterval", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotCountChangeMaxInterval", KeyClass::Live, ""},
        {"AiPlayerbot.MinRandomBotInWorldTime", KeyClass::Live, ""},
        {"AiPlayerbot.MaxRandomBotInWorldTime", KeyClass::Live, ""},
        {"AiPlayerbot.MinRandomBotRandomizeTime", KeyClass::Live, ""},
        {"AiPlayerbot.MaxRandomBotRandomizeTime", KeyClass::Live, ""},
        {"AiPlayerbot.MinRandomBotChangeStrategyTime", KeyClass::Live, ""},
        {"AiPlayerbot.MaxRandomBotChangeStrategyTime", KeyClass::Live, ""},
        {"AiPlayerbot.MinRandomBotReviveTime", KeyClass::Live, ""},
        {"AiPlayerbot.MaxRandomBotReviveTime", KeyClass::Live, ""},
        {"AiPlayerbot.MinRandomBotTeleportInterval", KeyClass::Live, ""},
        {"AiPlayerbot.MaxRandomBotTeleportInterval", KeyClass::Live, ""},
        {"AiPlayerbot.PermanentlyInWorldTime", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotTeleportDistance", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotsPerInterval", KeyClass::Live, ""},
        {"AiPlayerbot.RandomBotPrintStatsInterval", KeyClass::Live, ""},

        // Live: lifecycle and arrivals (Enable keys are guarded above)
        {"AiPlayerbot.Lifecycle.*", KeyClass::Live, ""},
        {"AiPlayerbot.Arrivals.*", KeyClass::Live, ""},

        // Live: already re-read by RandomBotLevelWorldScript
        {"AiPlayerbot.LevelBrackets.*", KeyClass::Live, ""},
        {"AiPlayerbot.ResetBotLevel.*", KeyClass::Live, ""},
    };
    return rules;
}

inline bool Matches(std::string const& pattern, std::string const& key)
{
    if (!pattern.empty() && pattern.back() == '*')
        return key.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
    return key == pattern;
}

inline Rule const* FindRule(std::string const& key)
{
    for (Rule const& rule : Rules())
        if (Matches(rule.pattern, key))
            return &rule;
    return nullptr;
}

inline KeyClass Classify(std::string const& key)
{
    Rule const* rule = FindRule(key);
    return rule ? rule->cls : KeyClass::Restart;
}

inline std::string RestartReason(std::string const& key)
{
    Rule const* rule = FindRule(key);
    return rule && rule->cls == KeyClass::Restart ? rule->reason : "only read at startup";
}

// Values of these keys never go to the log
inline bool IsSecret(std::string const& key)
{
    return key.find("DatabaseInfo") != std::string::npos || key.find("Password") != std::string::npos ||
           key.find("Secret") != std::string::npos;
}

using Snapshot = std::map<std::string, std::string>;

struct Change
{
    std::string key;
    std::string before;  // "<unset>" when the key was not in the config
    std::string after;
    KeyClass cls;
};

constexpr char const* UNSET = "<unset>";

inline std::vector<Change> Diff(Snapshot const& before, Snapshot const& after)
{
    std::vector<Change> changes;
    auto b = before.begin();
    auto a = after.begin();
    while (b != before.end() || a != after.end())
    {
        if (a == after.end() || (b != before.end() && b->first < a->first))
        {
            changes.push_back({b->first, b->second, UNSET, Classify(b->first)});
            ++b;
        }
        else if (b == before.end() || a->first < b->first)
        {
            changes.push_back({a->first, UNSET, a->second, Classify(a->first)});
            ++a;
        }
        else
        {
            if (a->second != b->second)
                changes.push_back({a->first, b->second, a->second, Classify(a->first)});
            ++a;
            ++b;
        }
    }
    return changes;
}

// The snapshot to compare the next reload against: live changes are taken, restart-only changes keep their
// old value so every later reload still reports them as waiting for a restart.
inline Snapshot NextSnapshot(Snapshot const& before, Snapshot const& after)
{
    Snapshot next = after;
    for (Change const& change : Diff(before, after))
    {
        if (change.cls == KeyClass::Live)
            continue;
        if (change.before == UNSET)
            next.erase(change.key);
        else
            next[change.key] = change.before;
    }
    return next;
}

inline size_t CountLive(std::vector<Change> const& changes)
{
    size_t live = 0;
    for (Change const& change : changes)
        live += change.cls == KeyClass::Live;
    return live;
}

// Keys whose change re-plans the rest of the current arrivals week (Cap and MaxPerHour act on the next
// arrival without a re-plan)
inline bool AffectsArrivalPlan(std::vector<Change> const& changes)
{
    for (Change const& change : changes)
    {
        if (change.cls != KeyClass::Live)
            continue;
        if (change.key == "AiPlayerbot.Arrivals.PerYear" || change.key == "AiPlayerbot.Arrivals.DayShape" ||
            change.key == "AiPlayerbot.Arrivals.WeekdayWeights" || change.key == "AiPlayerbot.Arrivals.HourWeights" ||
            change.key == "AiPlayerbot.Arrivals.ScaleMidWeek")
            return true;
    }
    return false;
}

inline std::string Shorten(std::string const& value, size_t max = 48)
{
    return value.size() <= max ? value : value.substr(0, max - 3) + "...";
}

// "playerbots config reloaded: 3 keys changed: applied live: A 1 -> 0, B 5 -> 8; needs a restart: C (reason)"
inline std::string Summary(std::vector<Change> const& changes)
{
    std::string live, restart;
    for (Change const& change : changes)
    {
        std::string const before = IsSecret(change.key) ? "***" : Shorten(change.before);
        std::string const after = IsSecret(change.key) ? "***" : Shorten(change.after);
        if (change.cls == KeyClass::Live)
            live += (live.empty() ? "" : ", ") + change.key + " " + before + " -> " + after;
        else
            restart += (restart.empty() ? "" : ", ") + change.key + " " + before + " -> " + after + " (" +
                       RestartReason(change.key) + ")";
    }

    std::string out = "playerbots config reloaded: " + std::to_string(changes.size()) +
                      (changes.size() == 1 ? " key changed" : " keys changed");
    if (!changes.empty())
        out += ":";
    if (!live.empty())
        out += " applied live: " + live;
    if (!restart.empty())
        out += std::string(live.empty() ? "" : ";") + " needs a restart: " + restart;
    return out;
}
}  // namespace ConfigReloadPolicy

#endif
