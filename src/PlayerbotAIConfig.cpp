/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotAIConfig.h"
#include "BisListMgr.h"
#include "Config.h"
#include "LadderMgr.h"
#include "NewRpgInfo.h"
#include "PlayerbotDungeonRepository.h"
#include "PlayerbotFactory.h"
#include "PlayerbotGuildMgr.h"
#include "Playerbots.h"
#include "RandomItemMgr.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "Talentspec.h"
#include "ThreadCheck.h"
#include "TravelMgr.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <sstream>

template <class T>
void LoadList(std::string const value, T& list)
{
    std::vector<std::string> ids = split(value, ',');
    for (std::vector<std::string>::iterator i = ids.begin(); i != ids.end(); i++)
    {
        uint32 id = atoi((*i).c_str());
        // if (!id)
        //     continue;
        list.push_back(id);
    }
}

template <class T>
void LoadSet(std::string const value, T& set)
{
    std::vector<std::string> ids = split(value, ',');
    for (std::vector<std::string>::iterator i = ids.begin(); i != ids.end(); i++)
    {
        uint32 id = atoi((*i).c_str());
        // if (!id)
        //     continue;
        set.insert(id);
    }
}

template <class T>
void LoadListString(std::string const value, T& list)
{
    std::vector<std::string> strings = split(value, ',');
    for (std::vector<std::string>::iterator i = strings.begin(); i != strings.end(); i++)
    {
        std::string const string = *i;
        if (string.empty())
            continue;

        list.push_back(string);
    }
}

// Parses a comma-separated, whitespace-tolerant bot name list (as used by both
// Playerbots.LevelBrackets.ExcludeNames and Playerbots.ResetBotLevel.ExcludeNames) into out.
static void ParseLevelMgrExcludeNames(std::string const& csv, std::vector<std::string>& out)
{
    out.clear();
    std::istringstream f(csv);
    std::string s;
    while (getline(f, s, ','))
    {
        s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
        if (!s.empty())
            out.push_back(s);
    }
}

bool PlayerbotAIConfig::Initialize()
{
    LOG_INFO("server.loading", "Initializing mod-playerbots, based on AI Playerbots by ike3 and the original Playerbots by blueboy");

    Enabled = sConfigMgr->GetOption<bool>("Playerbots.Enabled", true);
    if (!Enabled)
    {
        LOG_INFO("server.loading", "Playerbots Module is disabled in playerbots.conf");
        return false;
    }

    GlobalCoolDown = sConfigMgr->GetOption<int32>("Playerbots.GlobalCooldown", 500);
    MaxWaitForMove = sConfigMgr->GetOption<int32>("Playerbots.MaxWaitForMove", 5000);
    DisableMoveSplinePath = sConfigMgr->GetOption<int32>("Playerbots.DisableMoveSplinePath", 0);
    MaxMovementSearchTime = sConfigMgr->GetOption<int32>("Playerbots.MaxMovementSearchTime", 3);
    ExpireActionTime = sConfigMgr->GetOption<int32>("Playerbots.ExpireActionTime", 5000);
    DispelAuraDuration = sConfigMgr->GetOption<int32>("Playerbots.DispelAuraDuration", 700);
    ReactDelay = sConfigMgr->GetOption<int32>("Playerbots.ReactDelay", 100);
    DynamicReactDelay = sConfigMgr->GetOption<bool>("Playerbots.DynamicReactDelay", true);
    PassiveDelay = sConfigMgr->GetOption<int32>("Playerbots.PassiveDelay", 10000);
    RepeatDelay = sConfigMgr->GetOption<int32>("Playerbots.RepeatDelay", 2000);
    ErrorDelay = sConfigMgr->GetOption<int32>("Playerbots.ErrorDelay", 100);
    RpgDelay = sConfigMgr->GetOption<int32>("Playerbots.RpgDelay", 10000);
    SitDelay = sConfigMgr->GetOption<int32>("Playerbots.SitDelay", 20000);
    ReturnDelay = sConfigMgr->GetOption<int32>("Playerbots.ReturnDelay", 2000);
    LootDelay = sConfigMgr->GetOption<int32>("Playerbots.LootDelay", 1000);
    DisabledWithoutRealPlayerLoginDelay = sConfigMgr->GetOption<int32>("Playerbots.DisabledWithoutRealPlayerLoginDelay", 30);
    DisabledWithoutRealPlayerLogoutDelay = sConfigMgr->GetOption<int32>("Playerbots.DisabledWithoutRealPlayerLogoutDelay", 300);

    FarDistance = sConfigMgr->GetOption<float>("Playerbots.FarDistance", 20.0f);
    SightDistance = sConfigMgr->GetOption<float>("Playerbots.SightDistance", 100.0f);
    BotSightRangeCap = std::max(0.0f, sConfigMgr->GetOption<float>("Playerbots.BotSightRangeCap", 0.0f));
    SpellDistance = sConfigMgr->GetOption<float>("Playerbots.SpellDistance", 28.5f);
    ShootDistance = sConfigMgr->GetOption<float>("Playerbots.ShootDistance", 5.0f);
    HealDistance = sConfigMgr->GetOption<float>("Playerbots.HealDistance", 38.5f);
    LootDistance = sConfigMgr->GetOption<float>("Playerbots.LootDistance", 15.0f);
    FleeDistance = sConfigMgr->GetOption<float>("Playerbots.FleeDistance", 5.0f);
    AggroDistance = sConfigMgr->GetOption<float>("Playerbots.AggroDistance", 22.0f);
    TooCloseDistance = sConfigMgr->GetOption<float>("Playerbots.TooCloseDistance", 5.0f);
    MeleeDistance = sConfigMgr->GetOption<float>("Playerbots.MeleeDistance", 0.75f);
    FollowDistance = sConfigMgr->GetOption<float>("Playerbots.FollowDistance", 1.5f);
    WhisperDistance = sConfigMgr->GetOption<float>("Playerbots.WhisperDistance", 6000.0f);
    ContactDistance = sConfigMgr->GetOption<float>("Playerbots.ContactDistance", 0.45f);
    AoeRadius = sConfigMgr->GetOption<float>("Playerbots.AoeRadius", 10.0f);
    RpgDistance = sConfigMgr->GetOption<float>("Playerbots.RpgDistance", 200.0f);
    GrindDistance = sConfigMgr->GetOption<float>("Playerbots.GrindDistance", 75.0f);
    ReactDistance = sConfigMgr->GetOption<float>("Playerbots.ReactDistance", 150.0f);

    CriticalHealth = sConfigMgr->GetOption<int32>("Playerbots.CriticalHealth", 25);
    LowHealth = sConfigMgr->GetOption<int32>("Playerbots.LowHealth", 45);
    MediumHealth = sConfigMgr->GetOption<int32>("Playerbots.MediumHealth", 65);
    AlmostFullHealth = sConfigMgr->GetOption<int32>("Playerbots.AlmostFullHealth", 85);
    LowMana = sConfigMgr->GetOption<int32>("Playerbots.LowMana", 15);
    MediumMana = sConfigMgr->GetOption<int32>("Playerbots.MediumMana", 40);
    HighMana = sConfigMgr->GetOption<int32>("Playerbots.HighMana", 65);
    AutoSaveMana = sConfigMgr->GetOption<bool>("Playerbots.AutoSaveMana", true);
    SaveManaThreshold = sConfigMgr->GetOption<int32>("Playerbots.SaveManaThreshold", 60);
    switch (sConfigMgr->GetOption<uint32>("Playerbots.AutoGreaterBlessings", 1))
    {
        case 0:
            AutoGreaterBlessings = AutoPartyBuffMode::DISABLED;
            break;
        case 2:
            AutoGreaterBlessings = AutoPartyBuffMode::GROUP_OR_RAID;
            break;
        case 1:
        default:
            AutoGreaterBlessings = AutoPartyBuffMode::RAID_ONLY;
            break;
    }
    switch (sConfigMgr->GetOption<uint32>("Playerbots.AutoPartyBuffs", 2))
    {
        case 0:
            AutoPartyBuffs = AutoPartyBuffMode::DISABLED;
            break;
        case 1:
            AutoPartyBuffs = AutoPartyBuffMode::RAID_ONLY;
            break;
        case 2:
        default:
            AutoPartyBuffs = AutoPartyBuffMode::GROUP_OR_RAID;
            break;
    }
    TellWhenMissingBuffReagents = sConfigMgr->GetOption<bool>("Playerbots.TellWhenMissingBuffReagents", true);
    MissingBuffReagentMessageCooldown = sConfigMgr->GetOption<uint32>(
        "Playerbots.MissingBuffReagentMessageCooldown", 300);
    ForceRebuffOnReadyCheck = sConfigMgr->GetOption<bool>("Playerbots.ForceRebuffOnReadyCheck", false);
    ForceRebuffMarginSecs = std::min(sConfigMgr->GetOption<uint32>("Playerbots.ForceRebuffMarginSecs", 60), 3600u);
    AutoAvoidAoe = sConfigMgr->GetOption<bool>("Playerbots.AutoAvoidAoe", true);
    MaxAoeAvoidRadius = sConfigMgr->GetOption<float>("Playerbots.MaxAoeAvoidRadius", 15.0f);
    LoadSet<std::set<uint32>>(sConfigMgr->GetOption<std::string>("Playerbots.AoeAvoidSpellWhitelist", "50759,57491,13810,29946"),
                              AoeAvoidSpellWhitelist);
    TellWhenAvoidAoe = sConfigMgr->GetOption<bool>("Playerbots.TellWhenAvoidAoe", false);

    RandomGearLoweringChance = sConfigMgr->GetOption<float>("Playerbots.RandomGearLoweringChance", 0.0f);
    RandomGearQualityLimit = sConfigMgr->GetOption<int32>("Playerbots.RandomGearQualityLimit", 3);
    RandomGearScoreLimit = sConfigMgr->GetOption<int32>("Playerbots.RandomGearScoreLimit", 0);
    PreferClassArmorType  = sConfigMgr->GetOption<bool>("Playerbots.PreferClassArmorType", false);
    PreferredSpecWeapons  = sConfigMgr->GetOption<bool>("Playerbots.PreferredSpecWeapons", false);

    RandomBotMinLevelChance = sConfigMgr->GetOption<float>("Playerbots.RandomBotMinLevelChance", 0.1f);
    RandomBotMaxLevelChance = sConfigMgr->GetOption<float>("Playerbots.RandomBotMaxLevelChance", 0.1f);
    RandomBotRpgChance = sConfigMgr->GetOption<float>("Playerbots.RandomBotRpgChance", 0.20f);

    IterationsPerTick = sConfigMgr->GetOption<int32>("Playerbots.IterationsPerTick", 10);

    AllowAccountBots = sConfigMgr->GetOption<bool>("Playerbots.AllowAccountBots", true);
    AllowGuildBots = sConfigMgr->GetOption<bool>("Playerbots.AllowGuildBots", true);
    AllowTrustedAccountBots = sConfigMgr->GetOption<bool>("Playerbots.AllowTrustedAccountBots", true);
    DisabledWithoutRealPlayer = sConfigMgr->GetOption<bool>("Playerbots.DisabledWithoutRealPlayer", false);
    RandomBotGuildNearby = sConfigMgr->GetOption<bool>("Playerbots.RandomBotGuildNearby", false);
    RandomBotInvitePlayer = sConfigMgr->GetOption<bool>("Playerbots.RandomBotInvitePlayer", false);
    InviteChat = sConfigMgr->GetOption<bool>("Playerbots.InviteChat", false);

    RandomBotMapsAsString = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotMaps", "0,1,530,571");
    LoadList<std::vector<uint32>>(RandomBotMapsAsString, RandomBotMaps);
    ProbTeleToBankers = sConfigMgr->GetOption<float>("Playerbots.ProbTeleToBankers", 0.25f);
    EnableWeightTeleToCityBankers = sConfigMgr->GetOption<bool>("Playerbots.EnableWeightTeleToCityBankers", false);
    WeightTeleToStormwind = sConfigMgr->GetOption<int>("Playerbots.TeleToStormwindWeight", 2);
    WeightTeleToIronforge = sConfigMgr->GetOption<int>("Playerbots.TeleToIronforgeWeight", 1);
    WeightTeleToDarnassus = sConfigMgr->GetOption<int>("Playerbots.TeleToDarnassusWeight", 1);
    WeightTeleToExodar = sConfigMgr->GetOption<int>("Playerbots.TeleToExodarWeight", 1);
    WeightTeleToOrgrimmar = sConfigMgr->GetOption<int>("Playerbots.TeleToOrgrimmarWeight", 2);
    WeightTeleToUndercity = sConfigMgr->GetOption<int>("Playerbots.TeleToUndercityWeight", 1);
    WeightTeleToThunderBluff = sConfigMgr->GetOption<int>("Playerbots.TeleToThunderBluffWeight", 1);
    WeightTeleToSilvermoonCity = sConfigMgr->GetOption<int>("Playerbots.TeleToSilvermoonCityWeight", 1);
    WeightTeleToShattrathCity = sConfigMgr->GetOption<int>("Playerbots.TeleToShattrathCityWeight", 1);
    WeightTeleToDalaran = sConfigMgr->GetOption<int>("Playerbots.TeleToDalaranWeight", 1);
    LoadList<std::vector<uint32>>(
        sConfigMgr->GetOption<std::string>("Playerbots.RandomBotQuestItems",
                                           "5175,5176,5177,5178,6948,11000,12382,13704,16309"),
        RandomBotQuestItems);
    LoadList<std::vector<uint32>>(sConfigMgr->GetOption<std::string>("Playerbots.RandomBotSpellIds", "54197"),
                                  RandomBotSpellIds);
    LoadList<std::vector<uint32>>(
        sConfigMgr->GetOption<std::string>("Playerbots.PvpProhibitedZoneIds",
                                           "2255,656,2361,2362,2363,976,35,2268,3425,392,541,1446,3828,3712,3738,3565,"
                                           "3539,3623,4152,3988,4658,4284,4418,4436,4275,4323,4395,3703,4298,3951"),
        PvpProhibitedZoneIds);
    LoadList<std::vector<uint32>>(
        sConfigMgr->GetOption<std::string>("Playerbots.PvpProhibitedAreaIds",
                                           "976,35,392,2268,4161,4010,4317,4312,3649,3887,3958,3724,4080,3938,3754,3786,"
                                           "3973,4085,4086,4087,4088,251"),
        PvpProhibitedAreaIds);
    FastReactInBG = sConfigMgr->GetOption<bool>("Playerbots.FastReactInBG", true);
    LoadList<std::vector<uint32>>(
        sConfigMgr->GetOption<std::string>("Playerbots.RandomBotQuestIds", "3802,5505,6502,7761,7848,10277,10285,11492,"
                                           "13188,13189,24499,24511,24710,24712"),
        RandomBotQuestIds);

    LoadSet<std::set<uint32>>(
        sConfigMgr->GetOption<std::string>("Playerbots.DisallowedGameObjects",
                                           "176213,17155,2656,74448,19020,3719,3658,3705,3706,105579,75293,2857,"
                                           "179490,141596,160836,160845,179516,176224,181085,176112,128308,128403,"
                                           "165739,165738,175245,175970,176325,176327,123329,2560"),
        DisallowedGameObjects);
    LoadSet<std::set<uint32>>(
        sConfigMgr->GetOption<std::string>("Playerbots.AttunementQuests", "10279,10277,10282,10283,10284,10285,10296,"
                                           "10297,10298,11481,11482,11488,11490,11492,10901,10888,10445,10985"),
        AttunementQuests);

    LoadSet<std::set<uint32>>(
        sConfigMgr->GetOption<std::string>("Playerbots.UnobtainableItems", "12468,44869,44870,46978"),
        UnobtainableItems);

    BotAutologin = sConfigMgr->GetOption<bool>("Playerbots.BotAutologin", false);
    RandomBotAutologin = sConfigMgr->GetOption<bool>("Playerbots.RandomBotAutologin", true);
    MinRandomBots = sConfigMgr->GetOption<int32>("Playerbots.MinRandomBots", 500);
    MaxRandomBots = sConfigMgr->GetOption<int32>("Playerbots.MaxRandomBots", 500);
    RandomBotUpdateInterval = sConfigMgr->GetOption<int32>("Playerbots.RandomBotUpdateInterval", 20);
    RandomBotCountChangeMinInterval =
        sConfigMgr->GetOption<int32>("Playerbots.RandomBotCountChangeMinInterval", 30 * MINUTE);
    RandomBotCountChangeMaxInterval =
        sConfigMgr->GetOption<int32>("Playerbots.RandomBotCountChangeMaxInterval", 2 * HOUR);
    MinRandomBotInWorldTime = sConfigMgr->GetOption<int32>("Playerbots.MinRandomBotInWorldTime", 2 * HOUR);
    MaxRandomBotInWorldTime = sConfigMgr->GetOption<int32>("Playerbots.MaxRandomBotInWorldTime", 14 * 24 * HOUR);
    MinRandomBotRandomizeTime = sConfigMgr->GetOption<int32>("Playerbots.MinRandomBotRandomizeTime", 2 * HOUR);
    MaxRandomBotRandomizeTime = sConfigMgr->GetOption<int32>("Playerbots.MaxRandomBotRandomizeTime", 14 * 24 * HOUR);
    MinRandomBotChangeStrategyTime =
        sConfigMgr->GetOption<int32>("Playerbots.MinRandomBotChangeStrategyTime", 30 * MINUTE);
    MaxRandomBotChangeStrategyTime =
        sConfigMgr->GetOption<int32>("Playerbots.MaxRandomBotChangeStrategyTime", 2 * HOUR);
    MinRandomBotReviveTime = sConfigMgr->GetOption<int32>("Playerbots.MinRandomBotReviveTime", MINUTE);
    MaxRandomBotReviveTime = sConfigMgr->GetOption<int32>("Playerbots.MaxRandomBotReviveTime", 5 * MINUTE);
    MinRandomBotTeleportInterval = sConfigMgr->GetOption<int32>("Playerbots.MinRandomBotTeleportInterval", 1 * HOUR);
    MaxRandomBotTeleportInterval = sConfigMgr->GetOption<int32>("Playerbots.MaxRandomBotTeleportInterval", 5 * HOUR);
    PermanentlyInWorldTime =
        sConfigMgr->GetOption<int32>("Playerbots.PermanentlyInWorldTime", 1 * YEAR);
    RandomBotTeleportDistance = sConfigMgr->GetOption<int32>("Playerbots.RandomBotTeleportDistance", 100);
    RandomBotsPerInterval = sConfigMgr->GetOption<int32>("Playerbots.RandomBotsPerInterval", 60);
    RandomBotPrintStatsInterval = sConfigMgr->GetOption<int32>("Playerbots.RandomBotPrintStatsInterval", 300);
    MinRandomBotsPriceChangeInterval =
        sConfigMgr->GetOption<int32>("Playerbots.MinRandomBotsPriceChangeInterval", 2 * HOUR);
    MaxRandomBotsPriceChangeInterval =
        sConfigMgr->GetOption<int32>("Playerbots.MaxRandomBotsPriceChangeInterval", 48 * HOUR);
    RandomBotJoinLfg = sConfigMgr->GetOption<bool>("Playerbots.RandomBotJoinLfg", true);
    RandomBotJoinLfgOutleveledExpansion =
        sConfigMgr->GetOption<bool>("Playerbots.RandomBotJoinLfgOutleveledExpansion", true);

    RestrictHealerDPS = sConfigMgr->GetOption<bool>("Playerbots.HealerDPSMapRestriction", false);
    LoadList<std::vector<uint32>>(
        sConfigMgr->GetOption<std::string>("Playerbots.RestrictedHealerDPSMaps",
                                             "33,34,36,43,47,48,70,90,109,129,209,229,230,329,349,389,429,1001,1004,"
                                             "1007,269,540,542,543,545,546,547,552,553,554,555,556,557,558,560,585,574,"
                                             "575,576,578,595,599,600,601,602,604,608,619,632,650,658,668,409,469,509,"
                                             "531,532,534,544,548,550,564,565,580,249,533,603,615,616,624,631,649,724"),
        RestrictedHealerDPSMaps);

    //////////////////////////// ICC

    EnableICCBuffs = sConfigMgr->GetOption<bool>("Playerbots.EnableICCBuffs", true);

    //////////////////////////// Professions
    ClassMatchingProfessionChance =
        std::min<uint32>(100, sConfigMgr->GetOption<uint32>("Playerbots.ClassMatchingProfessionChance", 30));
    FishingDistanceFromMaster = sConfigMgr->GetOption<float>("Playerbots.FishingDistanceFromMaster", 10.0f);
    EndFishingWithMaster = sConfigMgr->GetOption<float>("Playerbots.EndFishingWithMaster", 30.0f);
    FishingDistance = sConfigMgr->GetOption<float>("Playerbots.FishingDistance", 40.0f);
    EnableFishingWithMaster = sConfigMgr->GetOption<bool>("Playerbots.EnableFishingWithMaster", true);
    //////////////////////////// CHAT
    EnableBroadcasts = sConfigMgr->GetOption<bool>("Playerbots.EnableBroadcasts", true);
    RandomBotTalk = sConfigMgr->GetOption<bool>("Playerbots.RandomBotTalk", false);
    RandomBotEmote = sConfigMgr->GetOption<bool>("Playerbots.RandomBotEmote", false);
    RandomBotSuggestDungeons = sConfigMgr->GetOption<bool>("Playerbots.RandomBotSuggestDungeons", true);
    RandomBotSayWithoutMaster = sConfigMgr->GetOption<bool>("Playerbots.RandomBotSayWithoutMaster", false);
    AnnounceConsumableUse = sConfigMgr->GetOption<bool>("Playerbots.AnnounceConsumableUse", true);

    // broadcastChanceMaxValue is used in urand(1, broadcastChanceMaxValue) for broadcasts,
    // lowering it will increase the chance, setting it to 0 will disable broadcasts
    // for internal use, not intended to be change by the user
    BroadcastChanceMaxValue = EnableBroadcasts ? 30000 : 0;

    // all broadcast chances should be in range 1-broadcastChanceMaxValue, value of 0 will disable this particular
    // broadcast setting value to max does not guarantee the broadcast, as there are some internal randoms as well
    BroadcastToGuildGlobalChance = sConfigMgr->GetOption<int32>("Playerbots.BroadcastToGuildGlobalChance", 30000);
    BroadcastToWorldGlobalChance = sConfigMgr->GetOption<int32>("Playerbots.BroadcastToWorldGlobalChance", 30000);
    BroadcastToGeneralGlobalChance = sConfigMgr->GetOption<int32>("Playerbots.BroadcastToGeneralGlobalChance", 30000);
    BroadcastToTradeGlobalChance = sConfigMgr->GetOption<int32>("Playerbots.BroadcastToTradeGlobalChance", 30000);
    BroadcastToLFGGlobalChance = sConfigMgr->GetOption<int32>("Playerbots.BroadcastToLFGGlobalChance", 30000);
    BroadcastToLocalDefenseGlobalChance =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastToLocalDefenseGlobalChance", 30000);
    BroadcastToWorldDefenseGlobalChance =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastToWorldDefenseGlobalChance", 30000);
    BroadcastToGuildRecruitmentGlobalChance =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastToGuildRecruitmentGlobalChance", 30000);

    BroadcastChanceLootingItemPoor = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceLootingItemPoor", 30);
    BroadcastChanceLootingItemNormal =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceLootingItemNormal", 300);
    BroadcastChanceLootingItemUncommon =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceLootingItemUncommon", 10000);
    BroadcastChanceLootingItemRare = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceLootingItemRare", 20000);
    BroadcastChanceLootingItemEpic = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceLootingItemEpic", 30000);
    BroadcastChanceLootingItemLegendary =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceLootingItemLegendary", 30000);
    BroadcastChanceLootingItemArtifact =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceLootingItemArtifact", 30000);

    BroadcastChanceQuestAccepted = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceQuestAccepted", 6000);
    BroadcastChanceQuestUpdateObjectiveCompleted =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceQuestUpdateObjectiveCompleted", 300);
    BroadcastChanceQuestUpdateObjectiveProgress =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceQuestUpdateObjectiveProgress", 300);
    BroadcastChanceQuestUpdateFailedTimer =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceQuestUpdateFailedTimer", 300);
    BroadcastChanceQuestUpdateComplete =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceQuestUpdateComplete", 1000);
    BroadcastChanceQuestTurnedIn = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceQuestTurnedIn", 10000);

    BroadcastChanceKillNormal = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceKillNormal", 30);
    BroadcastChanceKillElite = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceKillElite", 300);
    BroadcastChanceKillRareelite = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceKillRareelite", 3000);
    BroadcastChanceKillWorldboss = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceKillWorldboss", 20000);
    BroadcastChanceKillRare = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceKillRare", 10000);
    BroadcastChanceKillUnknown = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceKillUnknown", 100);
    BroadcastChanceKillPet = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceKillPet", 10);
    BroadcastChanceKillPlayer = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceKillPlayer", 30);

    BroadcastChanceLevelupGeneric = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceLevelupGeneric", 20000);
    BroadcastChanceLevelupTenX = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceLevelupTenX", 30000);
    BroadcastChanceLevelupMaxLevel = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceLevelupMaxLevel", 30000);

    BroadcastChanceSuggestInstance = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceSuggestInstance", 5000);
    BroadcastChanceSuggestQuest = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceSuggestQuest", 10000);
    BroadcastChanceSuggestGrindMaterials =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceSuggestGrindMaterials", 5000);
    BroadcastChanceSuggestGrindReputation =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceSuggestGrindReputation", 5000);
    BroadcastChanceSuggestSell = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceSuggestSell", 300);
    BroadcastChanceSuggestSomething =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceSuggestSomething", 30000);

    BroadcastChanceSuggestSomethingToxic =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceSuggestSomethingToxic", 0);

    BroadcastChanceSuggestToxicLinks = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceSuggestToxicLinks", 0);
    ToxicLinksPrefix = sConfigMgr->GetOption<std::string>("Playerbots.ToxicLinksPrefix", "gnomes");

    BroadcastChanceSuggestThunderfury =
        sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceSuggestThunderfury", 1);

    // does not depend on global chance
    BroadcastChanceGuildManagement = sConfigMgr->GetOption<int32>("Playerbots.BroadcastChanceGuildManagement", 30000);

    ToxicLinksRepliesChance = sConfigMgr->GetOption<int32>("Playerbots.ToxicLinksRepliesChance", 30);    // 0-100
    ThunderfuryRepliesChance = sConfigMgr->GetOption<int32>("Playerbots.ThunderfuryRepliesChance", 40);  // 0-100
    GuildRepliesRate = sConfigMgr->GetOption<int32>("Playerbots.GuildRepliesRate", 100);                 // 0-100

    RandomBotJoinBG = sConfigMgr->GetOption<bool>("Playerbots.RandomBotJoinBG", true);
    RandomBotAutoJoinBG = sConfigMgr->GetOption<bool>("Playerbots.RandomBotAutoJoinBG", false);

    RandomBotAutoJoinArenaBracket = sConfigMgr->GetOption<int32>("Playerbots.RandomBotAutoJoinArenaBracket", 14);

    RandomBotAutoJoinWSBrackets = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotAutoJoinWSBrackets", "7");
    RandomBotAutoJoinABBrackets = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotAutoJoinABBrackets", "6");
    RandomBotAutoJoinAVBrackets = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotAutoJoinAVBrackets", "3");
    RandomBotAutoJoinEYBrackets = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotAutoJoinEYBrackets", "2");
    RandomBotAutoJoinICBrackets = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotAutoJoinICBrackets", "1");

    RandomBotAutoJoinBGWSCount = sConfigMgr->GetOption<int32>("Playerbots.RandomBotAutoJoinBGWSCount", 1);
    RandomBotAutoJoinBGABCount = sConfigMgr->GetOption<int32>("Playerbots.RandomBotAutoJoinBGABCount", 1);
    RandomBotAutoJoinBGAVCount = sConfigMgr->GetOption<int32>("Playerbots.RandomBotAutoJoinBGAVCount", 0);
    RandomBotAutoJoinBGEYCount = sConfigMgr->GetOption<int32>("Playerbots.RandomBotAutoJoinBGEYCount", 1);
    RandomBotAutoJoinBGICCount = sConfigMgr->GetOption<int32>("Playerbots.RandomBotAutoJoinBGICCount", 0);

    RandomBotAutoJoinBGRatedArena2v2Count =
        sConfigMgr->GetOption<int32>("Playerbots.RandomBotAutoJoinBGRatedArena2v2Count", 0);
    RandomBotAutoJoinBGRatedArena3v3Count =
        sConfigMgr->GetOption<int32>("Playerbots.RandomBotAutoJoinBGRatedArena3v3Count", 0);
    RandomBotAutoJoinBGRatedArena5v5Count =
        sConfigMgr->GetOption<int32>("Playerbots.RandomBotAutoJoinBGRatedArena5v5Count", 0);
    RandomBotAutoJoinRatedArenaQueueSlots =
        std::max(1, sConfigMgr->GetOption<int32>("Playerbots.RandomBotAutoJoinRatedArenaQueueSlots", 1));
    RandomBotAutoJoinRatedArenaUncapped =
        sConfigMgr->GetOption<bool>("Playerbots.RandomBotAutoJoinRatedArenaUncapped", false);
    RandomBotArenaGroupOnWorldThread =
        sConfigMgr->GetOption<bool>("Playerbots.RandomBotArenaGroupOnWorldThread", false);
    LogInGroupOnly = sConfigMgr->GetOption<bool>("Playerbots.LogInGroupOnly", true);
    LogValuesPerTick = sConfigMgr->GetOption<bool>("Playerbots.LogValuesPerTick", false);
    SummonAtInnkeepersEnabled = sConfigMgr->GetOption<bool>("Playerbots.SummonAtInnkeepersEnabled", true);
    RandomBotMinLevel = sConfigMgr->GetOption<int32>("Playerbots.RandomBotMinLevel", 1);
    RandomBotMaxLevel = sConfigMgr->GetOption<int32>("Playerbots.RandomBotMaxLevel", 80);
    if (RandomBotMaxLevel > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        RandomBotMaxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    // Bracket defaults (below) derive from randomBotMaxLevel, so this must run after it is read.
    LoadRandomBotLevelConfig();

    RandomBotTeleLowerLevel = sConfigMgr->GetOption<int32>("Playerbots.RandomBotTeleLowerLevel", 1);
    RandomBotTeleHigherLevel = sConfigMgr->GetOption<int32>("Playerbots.RandomBotTeleHigherLevel", 3);
    OpenGoSpell = sConfigMgr->GetOption<int32>("Playerbots.OpenGoSpell", 6477);

    // Zones for NewRpgStrategy teleportation brackets
    std::vector<uint32> zoneIds = {
        // Classic WoW - Low-level zones
        1, 12, 14, 85, 141, 215, 3430, 3524,
        // Classic WoW - Mid-level zones
        17, 38, 40, 130, 148, 3433, 3525,
        // Classic WoW - High-level zones
        10, 11, 44, 267, 331, 400, 406,
        // Classic WoW - Higher-level zones
        3, 8, 15, 16, 33, 45, 47, 51, 357, 405, 440,
        // Classic WoW - Top-level zones
        4, 28, 46, 139, 361, 490, 618, 1377,
        // The Burning Crusade - Zones
        3483, 3518, 3519, 3520, 3521, 3522, 3523, 4080,
        // Wrath of the Lich King - Zones
        65, 66, 67, 210, 394, 495, 2817, 3537, 3711, 4197
    };

    for (uint32 zoneId : zoneIds)
    {
        std::string setting = "Playerbots.ZoneBracket." + std::to_string(zoneId);
        std::string value = sConfigMgr->GetOption<std::string>(setting, "");

        if (!value.empty())
        {
            size_t commaPos = value.find(',');
            if (commaPos != std::string::npos)
            {
                uint32 minLevel = atoi(value.substr(0, commaPos).c_str());
                uint32 maxLevel = atoi(value.substr(commaPos + 1).c_str());
                ZoneBrackets[zoneId] = std::make_pair(minLevel, maxLevel);
            }
        }
    }

    RandomChangeMultiplier = sConfigMgr->GetOption<float>("Playerbots.RandomChangeMultiplier", 1.0);

    RandomBotCombatStrategies = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotCombatStrategies", "");
    RandomBotNonCombatStrategies = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotNonCombatStrategies", "");
    CombatStrategies = sConfigMgr->GetOption<std::string>("Playerbots.CombatStrategies", "");
    NonCombatStrategies = sConfigMgr->GetOption<std::string>("Playerbots.NonCombatStrategies", "");
    ReactStrategies = sConfigMgr->GetOption<std::string>("Playerbots.ReactStrategies", "");
    RandomBotReactStrategies = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotReactStrategies", "");
    ApplyInstanceStrategies = sConfigMgr->GetOption<bool>("Playerbots.ApplyInstanceStrategies", true);

    CommandPrefix = sConfigMgr->GetOption<std::string>("Playerbots.CommandPrefix", "");
    CommandSeparator = sConfigMgr->GetOption<std::string>("Playerbots.CommandSeparator", "\\\\");

    CommandServerPort = sConfigMgr->GetOption<int32>("Playerbots.CommandServerPort", 8888);
    PerfMonEnabled = sConfigMgr->GetOption<bool>("Playerbots.PerfMonEnabled", false);
    PerfMonWorldThread = sConfigMgr->GetOption<bool>("Playerbots.PerfMonWorldThread", false);
    DebugThreadChecks = sConfigMgr->GetOption<bool>("Playerbots.DebugThreadChecks", false);
    PlayerbotThreadCheck::enabled = DebugThreadChecks;
    SkipIdleTriggerChecks = sConfigMgr->GetOption<bool>("Playerbots.SkipIdleTriggerChecks", false);

    UseGroundMountAtMinLevel = sConfigMgr->GetOption<int32>("Playerbots.UseGroundMountAtMinLevel", 20);
    UseFastGroundMountAtMinLevel = sConfigMgr->GetOption<int32>("Playerbots.UseFastGroundMountAtMinLevel", 40);
    UseFlyMountAtMinLevel = sConfigMgr->GetOption<int32>("Playerbots.UseFlyMountAtMinLevel", 60);
    UseFastFlyMountAtMinLevel = sConfigMgr->GetOption<int32>("Playerbots.UseFastFlyMountAtMinLevel", 70);

    // stagger bot flightpath takeoff
    BotTaxiDelayMin = sConfigMgr->GetOption<uint32>("Playerbots.BotTaxiDelayMinMs", 350);
    BotTaxiDelayMax = sConfigMgr->GetOption<uint32>("Playerbots.BotTaxiDelayMaxMs", 5000);
    BotTaxiGapMs = sConfigMgr->GetOption<uint32>("Playerbots.BotTaxiGapMs", 200);
    BotTaxiGapJitterMs = sConfigMgr->GetOption<uint32>("Playerbots.BotTaxiGapJitterMs", 100);

    LOG_INFO("server.loading", "Loading TalentSpecs...");

    for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
    {
        if (cls == 10)
        {
            continue;
        }
        for (uint32 spec = 0; spec < MAX_SPECNO; ++spec)
        {
            std::ostringstream os;
            os << "Playerbots.PremadeSpecName." << cls << "." << spec;
            PremadeSpecName[cls][spec] = sConfigMgr->GetOption<std::string>(os.str().c_str(), "", false);
            os.str("");
            os.clear();
            os << "Playerbots.PremadeSpecGlyph." << cls << "." << spec;
            PremadeSpecGlyph[cls][spec] = sConfigMgr->GetOption<std::string>(os.str().c_str(), "", false);
            std::vector<std::string> splitSpecGlyph = split(PremadeSpecGlyph[cls][spec], ',');
            for (std::string& split : splitSpecGlyph)
            {
                if (split.size() != 0)
                {
                    ParsedSpecGlyph[cls][spec].push_back(atoi(split.c_str()));
                }
            }
            for (uint32 level = 0; level < MAX_LEVEL; ++level)
            {
                std::ostringstream os;
                os << "Playerbots.PremadeSpecLink." << cls << "." << spec << "." << level;
                PremadeSpecLink[cls][spec][level] = sConfigMgr->GetOption<std::string>(os.str().c_str(), "", false);
                ParsedSpecLinkOrder[cls][spec][level] = ParseTempTalentsOrder(cls, PremadeSpecLink[cls][spec][level]);
            }
        }
        for (uint32 spec = 0; spec < 3; ++spec)
        {
            for (uint32 points = 0; points < 21; ++points)
            {
                std::ostringstream os;
                os << "Playerbots.PremadeHunterPetLink." << spec << "." << points;
                PremadeHunterPetLink[spec][points] = sConfigMgr->GetOption<std::string>(os.str().c_str(), "", false);
                ParsedHunterPetLinkOrder[spec][points] =
                    ParseTempPetTalentsOrder(spec, PremadeHunterPetLink[spec][points]);
            }
        }
        for (uint32 spec = 0; spec < MAX_SPECNO; ++spec)
        {
            std::ostringstream os;
            os << "Playerbots.RandomClassSpecProb." << cls << "." << spec;
            uint32 def;
            if (spec <= 1)
                def = 33;
            else if (spec == 2)
                def = 34;
            else
                def = 0;
            RandomClassSpecProb[cls][spec] = sConfigMgr->GetOption<uint32>(os.str().c_str(), def, false);
            os.str("");
            os.clear();
            os << "Playerbots.RandomClassSpecIndex." << cls << "." << spec;
            RandomClassSpecIndex[cls][spec] = sConfigMgr->GetOption<uint32>(os.str().c_str(), spec, false);
        }
    }

    BotCheats.clear();
    LoadListString<std::vector<std::string>>(sConfigMgr->GetOption<std::string>("Playerbots.BotCheats", "food,taxi,raid"),
                                             BotCheats);

    BotCheatMask = 0;

    if (std::find(BotCheats.begin(), BotCheats.end(), "food") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::food;
    if (std::find(BotCheats.begin(), BotCheats.end(), "taxi") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::taxi;
    if (std::find(BotCheats.begin(), BotCheats.end(), "gold") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::gold;
    if (std::find(BotCheats.begin(), BotCheats.end(), "health") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::health;
    if (std::find(BotCheats.begin(), BotCheats.end(), "mana") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::mana;
    if (std::find(BotCheats.begin(), BotCheats.end(), "power") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::power;
    if (std::find(BotCheats.begin(), BotCheats.end(), "raid") != BotCheats.end())
        BotCheatMask |= (uint32)BotCheatMask::raid;

    LoadListString<std::vector<std::string>>(sConfigMgr->GetOption<std::string>("Playerbots.AllowedLogFiles", ""),
                                             AllowedLogFiles);
    EnableAutoTradeOnItemMention = sConfigMgr->GetOption<bool>("Playerbots.EnableAutoTradeOnItemMention", true);
    LoadListString<std::vector<std::string>>(sConfigMgr->GetOption<std::string>("Playerbots.TradeActionExcludedPrefixes", ""),
                                             TradeActionExcludedPrefixes);

    WorldBuffs.clear();
    LoadWorldBuff();
    LOG_INFO("playerbots", "Loading World Buff Feature...");

    RandomBotAccountPrefix = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotAccountPrefix", "rndbot");
    RandomBotAccountCount = sConfigMgr->GetOption<int32>("Playerbots.RandomBotAccountCount", 0);
    DeleteRandomBotAccounts = sConfigMgr->GetOption<bool>("Playerbots.DeleteRandomBotAccounts", false);
    RandomBotGuildCount = sConfigMgr->GetOption<int32>("Playerbots.RandomBotGuildCount", 20);
    RandomBotGuildSizeMax = sConfigMgr->GetOption<int32>("Playerbots.RandomBotGuildSizeMax", 15);
    DeleteRandomBotGuilds = sConfigMgr->GetOption<bool>("Playerbots.DeleteRandomBotGuilds", false);

    BotSendMailEnabled = sConfigMgr->GetOption<bool>("Playerbots.BotSendMailEnabled", true);

    GuildTaskEnabled = sConfigMgr->GetOption<bool>("Playerbots.EnableGuildTasks", false);
    MinGuildTaskChangeTime = sConfigMgr->GetOption<int32>("Playerbots.MinGuildTaskChangeTime", 3 * 24 * 3600);
    MaxGuildTaskChangeTime = sConfigMgr->GetOption<int32>("Playerbots.MaxGuildTaskChangeTime", 4 * 24 * 3600);
    MinGuildTaskAdvertisementTime = sConfigMgr->GetOption<int32>("Playerbots.MinGuildTaskAdvertisementTime", 300);
    MaxGuildTaskAdvertisementTime = sConfigMgr->GetOption<int32>("Playerbots.MaxGuildTaskAdvertisementTime", 12 * 3600);
    MinGuildTaskRewardTime = sConfigMgr->GetOption<int32>("Playerbots.MinGuildTaskRewardTime", 300);
    MaxGuildTaskRewardTime = sConfigMgr->GetOption<int32>("Playerbots.MaxGuildTaskRewardTime", 3600);
    GuildTaskAdvertCleanupTime = sConfigMgr->GetOption<int32>("Playerbots.GuildTaskAdvertCleanupTime", 300);
    GuildTaskKillTaskDistance = sConfigMgr->GetOption<int32>("Playerbots.GuildTaskKillTaskDistance", 2000);
    TargetPosRecalcDistance = sConfigMgr->GetOption<float>("Playerbots.TargetPosRecalcDistance", 0.1f);

    //cosmetics
    switch (sConfigMgr->GetOption<int32>("Playerbots.RandomBotShowHelmet", 1))
    {
        case 0:
            RandomBotShowHelmet = ShowHideCosmetic::ALWAYS_HIDE;
            break;
        case 2:
            RandomBotShowHelmet = ShowHideCosmetic::RANDOMIZE;
            break;
        case 1:
        default:
            RandomBotShowHelmet = ShowHideCosmetic::ALWAYS_SHOW;
            break;
    }
    switch (sConfigMgr->GetOption<int32>("Playerbots.RandomBotShowCloak", 1))
    {
        case 0:
            RandomBotShowCloak = ShowHideCosmetic::ALWAYS_HIDE;
            break;
        case 2:
            RandomBotShowCloak = ShowHideCosmetic::RANDOMIZE;
            break;
        case 1:
        default:
            RandomBotShowCloak = ShowHideCosmetic::ALWAYS_SHOW;
            break;
    }

    // SPP switches
    EnableGreet = sConfigMgr->GetOption<bool>("Playerbots.EnableGreet", true);
    SummonWhenGroup = sConfigMgr->GetOption<bool>("Playerbots.SummonWhenGroup", true);
    RandomBotFixedLevel = sConfigMgr->GetOption<bool>("Playerbots.RandomBotFixedLevel", false);
    DisableRandomLevels = sConfigMgr->GetOption<bool>("Playerbots.DisableRandomLevels", false);
    RandomBotRandomPassword = sConfigMgr->GetOption<bool>("Playerbots.RandomBotRandomPassword", true);
    DowngradeMaxLevelBot = sConfigMgr->GetOption<bool>("Playerbots.DowngradeMaxLevelBot", true);
    EquipAndSpecPersistence = sConfigMgr->GetOption<bool>("Playerbots.EquipAndSpecPersistence", true);
    EquipAndSpecPersistenceLevel = sConfigMgr->GetOption<int32>("Playerbots.EquipAndSpecPersistenceLevel", 1);
    GroupInvitationPermission = sConfigMgr->GetOption<int32>("Playerbots.GroupInvitationPermission", 1);
    AllowBattlegroundCommands = sConfigMgr->GetOption<bool>("Playerbots.AllowBattlegroundCommands", false);
    KeepAltsInGroup = sConfigMgr->GetOption<bool>("Playerbots.KeepAltsInGroup", false);
    AllowSummonInCombat = sConfigMgr->GetOption<bool>("Playerbots.AllowSummonInCombat", true);
    AllowSummonWhenMasterIsDead = sConfigMgr->GetOption<bool>("Playerbots.AllowSummonWhenMasterIsDead", true);
    AllowSummonWhenBotIsDead = sConfigMgr->GetOption<bool>("Playerbots.AllowSummonWhenBotIsDead", true);
    ReviveBotWhenSummoned = sConfigMgr->GetOption<int32>("Playerbots.ReviveBotWhenSummoned", 1);
    BotRepairWhenSummon = sConfigMgr->GetOption<bool>("Playerbots.BotRepairWhenSummon", true);
    AutoInitOnly = sConfigMgr->GetOption<bool>("Playerbots.AutoInitOnly", false);
    ResetInstanceIdForAltBots = sConfigMgr->GetOption<bool>("Playerbots.ResetInstanceIdForAltBots", false);
    AutoInitEquipLevelLimitRatio = sConfigMgr->GetOption<float>("Playerbots.AutoInitEquipLevelLimitRatio", 1.0);

    MaxAddedBots = sConfigMgr->GetOption<int32>("Playerbots.MaxAddedBots", 40);
    AddClassCommand = sConfigMgr->GetOption<int32>("Playerbots.AddClassCommand", 1);
    AddClassAccountPoolSize = sConfigMgr->GetOption<int32>("Playerbots.AddClassAccountPoolSize", 50);
    AddClassRandomCharacter = sConfigMgr->GetOption<bool>("Playerbots.AddClassRandomCharacter", false);
    MaintenanceCommand = sConfigMgr->GetOption<int32>("Playerbots.MaintenanceCommand", 1);

    AltMaintenanceAttunementQs = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceAttunementQuests", true);
    AltMaintenanceBags = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceBags", true);
    AltMaintenanceAmmo = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceAmmo", true);
    AltMaintenanceFood = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceFood", true);
    AltMaintenanceReagents = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceReagents", true);
    AltMaintenanceConsumables = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceConsumables", true);
    AltMaintenancePotions = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenancePotions", true);
    AltMaintenanceTalentTree = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceTalentTree", true);
    AltMaintenancePet = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenancePet", true);
    AltMaintenancePetTalents = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenancePetTalents", true);
    AltMaintenanceClassSpells = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceClassSpells", true);
    AltMaintenanceAvailableSpells = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceAvailableSpells", true);
    AltMaintenanceSkills = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceSkills", true);
    AltMaintenanceReputation = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceReputation", true);
    AltMaintenanceSpecialSpells = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceSpecialSpells", true);
    AltMaintenanceMounts = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceMounts", true);
    AltMaintenanceGlyphs = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceGlyphs", true);
    AltMaintenanceKeyring = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceKeyring", true);
    AltMaintenanceGemsEnchants = sConfigMgr->GetOption<bool>("Playerbots.AltMaintenanceGemsEnchants", true);

    AutoGearCommand = sConfigMgr->GetOption<int32>("Playerbots.AutoGearCommand", 1);
    AutoGearCommandAltBots = sConfigMgr->GetOption<int32>("Playerbots.AutoGearCommandAltBots", 1);
    AutoGearBisCommand = sConfigMgr->GetOption<int32>("Playerbots.AutoGearBisCommand", 0);
    AutoGearQualityLimit = sConfigMgr->GetOption<int32>("Playerbots.AutoGearQualityLimit", 3);
    AutoGearScoreLimit = sConfigMgr->GetOption<int32>("Playerbots.AutoGearScoreLimit", 0);

    RandomBotXPRate = sConfigMgr->GetOption<float>("Playerbots.RandomBotXPRate", 1.0);
    RandomBotAllianceRatio = sConfigMgr->GetOption<int32>("Playerbots.RandomBotAllianceRatio", 50);
    RandomBotHordeRatio = sConfigMgr->GetOption<int32>("Playerbots.RandomBotHordeRatio", 50);
    DisableDeathKnightLogin = sConfigMgr->GetOption<bool>("Playerbots.DisableDeathKnightLogin", 0);
    LimitTalentsExpansion = sConfigMgr->GetOption<bool>("Playerbots.LimitTalentsExpansion", 0);
    BotActiveAlone = sConfigMgr->GetOption<int32>("Playerbots.BotActiveAlone", 10);
    BotActiveAloneDurationSeconds = sConfigMgr->GetOption<int32>("Playerbots.BotActiveAloneDurationSeconds", 30);
    BotActiveAloneForceWhenInRadius = sConfigMgr->GetOption<uint32>("Playerbots.BotActiveAloneForceWhenInRadius", 150);
    BotActiveAloneForceWhenInZone = sConfigMgr->GetOption<bool>("Playerbots.BotActiveAloneForceWhenInZone", 1);
    BotActiveAloneForceWhenInMap = sConfigMgr->GetOption<bool>("Playerbots.BotActiveAloneForceWhenInMap", 0);
    BotActiveAloneForceWhenIsFriend = sConfigMgr->GetOption<bool>("Playerbots.BotActiveAloneForceWhenIsFriend", 0);
    BotActiveAloneForceWhenInGuild = sConfigMgr->GetOption<bool>("Playerbots.BotActiveAloneForceWhenInGuild", 1);
    TestPlayerEnable = sConfigMgr->GetOption<bool>("Playerbots.TestPlayer.Enable", false);
    TestPlayerAccount = sConfigMgr->GetOption<std::string>("Playerbots.TestPlayer.Account", "WBTEST");
    PvpTestEnable = sConfigMgr->GetOption<bool>("Playerbots.PvpTest.Enable", false);
    PvpTestTimeoutSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.PvpTest.TimeoutSeconds", 900), 60, 3600);
    // "<map> <x> <y> <z> <o>"; the default is the Hillsbrad Foothills spot (a contested zone, duels allowed)
    PvpTestFightMap = 0;
    PvpTestFightX = -436.657f;
    PvpTestFightY = -581.254f;
    PvpTestFightZ = 53.5944f;
    PvpTestFightO = 1.25917f;
    std::string const fightLocation = sConfigMgr->GetOption<std::string>("Playerbots.PvpTest.FightLocation", "");
    if (!fightLocation.empty())
    {
        uint32 map = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float o = 0.0f;
        if (std::sscanf(fightLocation.c_str(), "%u %f %f %f %f", &map, &x, &y, &z, &o) == 5)
        {
            PvpTestFightMap = map;
            PvpTestFightX = x;
            PvpTestFightY = y;
            PvpTestFightZ = z;
            PvpTestFightO = o;
        }
        else
            LOG_ERROR("playerbots",
                      "Playerbots.PvpTest.FightLocation must be \"<map> <x> <y> <z> <o>\"; using the default spot");
    }
    CensusEnable = sConfigMgr->GetOption<bool>("Playerbots.Census.Enable", false);
    CensusIntervalSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Census.IntervalSeconds", 30), 5, 3600);
    AcquaintanceEnable = sConfigMgr->GetOption<bool>("Playerbots.Acquaintance.Enable", false);
    AcquaintanceFlushSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.FlushSeconds", 30), 5, 3600);
    AcquaintanceDirectCooldownSeconds = std::clamp<uint32>(
        sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.DirectCooldownSeconds", 60), 0, 86400);
    AcquaintanceGroupJoinCooldownSeconds = std::clamp<uint32>(
        sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.GroupJoinCooldownSeconds", 600), 0, 86400);
    AcquaintancePointsDirect =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.PointsDirect", 10), 0, 10000);
    AcquaintancePointsGroupJoin =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.PointsGroupJoin", 20), 0, 10000);
    AcquaintancePointsGuildJoin =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.PointsGuildJoin", 15), 0, 10000);
    AcquaintancePointsPvpKill =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.PointsPvpKill", 5), 0, 10000);
    AcquaintancePointsPerGroupMinute =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.PointsPerGroupMinute", 5), 0, 10000);
    AcquaintancePointsPerGuildHour =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.PointsPerGuildHour", 6), 0, 10000);
    AcquaintancePointsPerNearbyMinute =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.PointsPerNearbyMinute", 1), 0, 10000);
    AcquaintanceMaxScore =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.MaxScore", 1000000), 1, 2000000000);
    AcquaintanceGroupSampleSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.GroupSampleSeconds", 10), 1, 3600);
    AcquaintanceGuildSampleSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.GuildSampleSeconds", 60), 5, 3600);
    AcquaintanceNearbyIntervalSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Acquaintance.NearbyIntervalSeconds", 10), 0, 3600);
    AcquaintanceNearbyRadius =
        std::clamp(sConfigMgr->GetOption<float>("Playerbots.Acquaintance.NearbyRadius", 30.0f), 1.0f, 100.0f);
    LadderEnable = sConfigMgr->GetOption<bool>("Playerbots.Ladder.Enable", false);
    LadderRole = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.Role", LADDER_ROLE_OFF),
                                    LADDER_ROLE_OFF, LADDER_ROLE_ARENA);
    LadderServerId = sConfigMgr->GetOption<std::string>("Playerbots.Ladder.ServerId", "");
    MatchLogEnable = sConfigMgr->GetOption<bool>("Playerbots.MatchLog.Enable", false);
    MatchLogTraceEvery = sConfigMgr->GetOption<uint32>("Playerbots.MatchLog.TraceEvery", 0);
    MatchLogTraceMaxEvents =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.MatchLog.TraceMaxEvents", 20000), 16, 1000000);
    MatchLogTraceMaxBytes =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.MatchLog.TraceMaxBytes", 4194304), 4096, 67108864);
    NaturalTraceEnable = sConfigMgr->GetOption<bool>("Playerbots.Natural.Trace.Enable", false);
    NaturalTraceMaxUnits =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Natural.Trace.MaxUnits", 8), 1, 1000);
    NaturalTraceSampleEvery =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Natural.Trace.SampleEvery", 1), 0, 1000000);
    NaturalTraceOrganic = sConfigMgr->GetOption<bool>("Playerbots.Natural.Trace.Organic", false);
    NaturalTraceDir = sConfigMgr->GetOption<std::string>("Playerbots.Natural.Trace.Dir", "");
    NaturalTraceIntervalMs =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Natural.Trace.IntervalMs", 100), 50, 1000);
    NaturalTraceMaxRowsPerUnit = std::clamp<uint32>(
        sConfigMgr->GetOption<uint32>("Playerbots.Natural.Trace.MaxRowsPerUnit", 200000), 100, 10000000);
    {
        std::string format = sConfigMgr->GetOption<std::string>("Playerbots.Natural.Trace.Format", "jsonl");
        std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return std::tolower(c); });
        NaturalTraceBinary = format == "binary";
        if (!NaturalTraceBinary && format != "jsonl")
            LOG_ERROR("server.loading", "Playerbots.Natural.Trace.Format = {}: unknown, jsonl is used", format);
    }
    LadderDepartTimeoutSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.DepartTimeoutSeconds", 600), 5, 86400);
    LadderContactMinutes =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.ContactMinutes", 15), 0, 1440);
    LadderPoolTeams = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.PoolTeams", 0), 0, 5000);
    LadderMatchmakerEnable = sConfigMgr->GetOption<bool>("Playerbots.Ladder.Matchmaker.Enable", false);
    LadderMatchmakerDelayMinSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.Matchmaker.DelayMinSeconds", 3), 0, 600);
    LadderMatchmakerDelayMaxSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.Matchmaker.DelayMaxSeconds", 8),
                           LadderMatchmakerDelayMinSeconds, 600);
    LadderMatchmakerMmrWidth =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.Matchmaker.MmrWidth", 150), 1, 5000);
    LadderMatchmakerWindow =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.Matchmaker.Window", 200), 1, 5000);
    LadderMatchmakerWindowGrowth =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.Matchmaker.WindowGrowth", 5), 0, 1000);
    LadderMatchmakerRecentOpponents =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.Matchmaker.RecentOpponents", 3), 0, 20);
    LadderMatchmakerTimeoutSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.Matchmaker.TimeoutSeconds", 60), 5, 3600);
    LadderMatchmakerMaxPicks =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Ladder.Matchmaker.MaxPicks", 3), 1, 100);
    LadderMatchmakerSkirmish = sConfigMgr->GetOption<bool>("Playerbots.Ladder.Matchmaker.Skirmish", true);
    LadderMatchmakerPoolOnly = sConfigMgr->GetOption<bool>("Playerbots.Ladder.Matchmaker.PoolOnly", true);
    PvpGearEnable = sConfigMgr->GetOption<bool>("Playerbots.PvpGear.Enable", false);
    // Parsed and checked by PvpGearMgr::Load (unknown names are logged there)
    PvpGearTierOrder = sConfigMgr->GetOption<std::string>("Playerbots.PvpGear.TierOrder",
                                                          "Hateful/Savage,Deadly,Furious,Relentless,Wrathful");
    PvpGearBaseTier = sConfigMgr->GetOption<std::string>("Playerbots.PvpGear.BaseTier", "");
    PvpGearWear = sConfigMgr->GetOption<bool>("Playerbots.PvpGear.Wear", false);
    PvpGearWearBetweenMatches = sConfigMgr->GetOption<bool>("Playerbots.PvpGear.WearBetweenMatches", false);
    PvpGearBuyEnable = sConfigMgr->GetOption<bool>("Playerbots.PvpGear.Buy.Enable", false);
    // Parsed and checked by PvpGearMgr::Load (unknown names are logged there)
    PvpGearBuySlotPriority = sConfigMgr->GetOption<std::string>(
        "Playerbots.PvpGear.Buy.SlotPriority",
        "weapon,chest,legs,head,shoulders,hands,feet,waist,wrists,back,neck,rings,trinkets,ranged");
    PvpGearBuySaveTowardBetter = sConfigMgr->GetOption<bool>("Playerbots.PvpGear.Buy.SaveTowardBetter", true);
    PvpGearBuyCheckIntervalMinutes =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.PvpGear.Buy.CheckIntervalMinutes", 60), 1, 1440);
    PvpGearBuyBatchPerTick =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.PvpGear.Buy.BatchPerTick", 5), 1, 100);
    SkillEnable = sConfigMgr->GetOption<bool>("Playerbots.Skill.Enable", false);
    SkillReactFactorLow =
        std::clamp(sConfigMgr->GetOption<float>("Playerbots.Skill.ReactFactorAtSkill1", 1.0f), 0.1f, 10.0f);
    SkillReactFactorHigh =
        std::clamp(sConfigMgr->GetOption<float>("Playerbots.Skill.ReactFactorAtSkill99", 1.0f), 0.1f, 10.0f);
    SkillFullAbilityInPvE = sConfigMgr->GetOption<bool>("Playerbots.Skill.FullAbilityInPvE", false);
    SkillReactActive = SkillEnable && (SkillReactFactorLow != 1.0f || SkillReactFactorHigh != 1.0f);
    SkillNoticeMissPercentLow =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Skill.NoticeMissPercentAtSkill1", 0), 0, 95);
    SkillNoticeMissPercentHigh =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Skill.NoticeMissPercentAtSkill99", 0), 0, 95);
    SkillNoticeDelayMsLow =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Skill.NoticeDelayMsAtSkill1", 0), 0, 10000);
    SkillNoticeDelayMsHigh =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Skill.NoticeDelayMsAtSkill99", 0), 0, 10000);
    SkillNoticeWindowMs =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Skill.NoticeWindowMs", 1500), 100, 60000);
    SkillNoticeActive = SkillEnable && (SkillNoticeMissPercentLow != 0 || SkillNoticeMissPercentHigh != 0 ||
                                        SkillNoticeDelayMsLow != 0 || SkillNoticeDelayMsHigh != 0);
    {
        std::string distribution = sConfigMgr->GetOption<std::string>("Playerbots.Skill.Distribution", "normal");
        std::transform(distribution.begin(), distribution.end(), distribution.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (distribution != "normal" && distribution != "uniform")
            LOG_ERROR("server.loading", "Playerbots.Skill.Distribution = {}: unknown, normal is used", distribution);

        SkillDistributionUniform = distribution == "uniform";
        SkillMean = std::clamp(sConfigMgr->GetOption<float>("Playerbots.Skill.Mean", 50.0f), 1.0f, 99.0f);
        SkillSd = std::clamp(sConfigMgr->GetOption<float>("Playerbots.Skill.Sd", 15.0f), 0.1f, 100.0f);
        SkillMin = std::clamp(sConfigMgr->GetOption<float>("Playerbots.Skill.Min", 1.0f), 1.0f, 99.0f);
        SkillMax = std::clamp(sConfigMgr->GetOption<float>("Playerbots.Skill.Max", 99.0f), SkillMin, 99.0f);
    }
    SkillExemptRealMasterBots = sConfigMgr->GetOption<bool>("Playerbots.Skill.ExemptRealMasterBots", true);
    {
        auto const percent = [](std::string const& key, uint32 fallback, uint32 high)
        { return static_cast<uint8>(std::clamp<uint32>(sConfigMgr->GetOption<uint32>(key, fallback), 0, high)); };
        SkillGate.CurveExponent =
            std::clamp(sConfigMgr->GetOption<float>("Playerbots.Skill.CurveExponent", 1.0f), 0.1f, 10.0f);
        SkillGate.KnowledgeSpread = percent("Playerbots.Skill.KnowledgeSpread", 15, 50);
        SkillGate.EffortAtSkill1 = percent("Playerbots.Skill.EffortAtSkill1", 100, 100);
        SkillGate.EffortAtSkill99 = percent("Playerbots.Skill.EffortAtSkill99", 100, 100);
        SkillGate.EffortSpread = percent("Playerbots.Skill.EffortSpread", 0, 50);
        SkillGate.StyleSpread = percent("Playerbots.Skill.StyleSpread", 0, 50);
    }
    {
        // The attribute model (BotAttributes.h); factorised here, so also at .playerbots rndbot reload
        std::string const prefix = "Playerbots.Skill.Attributes.";
        BotAttributeParams& attributes = SkillAttributes;
        attributes = BotAttributeParams();
        for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
        {
            std::string const name = BOT_ATTRIBUTE_DEFINITIONS[index].Name;
            float const loading = IsBotPersonality(index) ? 0.0f : 1.0f;
            attributes.Loading[index] =
                std::clamp(sConfigMgr->GetOption<float>(prefix + "Loading." + name, loading, false), 0.0f, 1.0f);
            attributes.Spread[index] =
                std::clamp(sConfigMgr->GetOption<float>(prefix + "Spread." + name, 0.0f, false), 0.0f, 3.0f);
        }

        // Correlations of the own parts as named pairs, either order: Playerbots.Skill.Attributes.Corr.Wisdom-Teamwork
        auto const lower = [](std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        auto const find = [&lower](std::string const& name)
        {
            for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
            {
                if (lower(BOT_ATTRIBUTE_DEFINITIONS[index].Name) == lower(name))
                    return static_cast<int32>(index);
            }

            return -1;
        };
        std::string const corrPrefix = prefix + "Corr.";
        for (std::string const& key : sConfigMgr->GetKeysByString(corrPrefix))
        {
            std::string const pair = key.substr(corrPrefix.size());
            size_t const dash = pair.find('-');
            int32 const first = dash == std::string::npos ? -1 : find(pair.substr(0, dash));
            int32 const second = dash == std::string::npos ? -1 : find(pair.substr(dash + 1));
            if (first < 0 || second < 0 || first == second)
            {
                LOG_ERROR("server.loading", "{}: not a pair of two attribute names, ignored", key);
                continue;
            }

            double const value = std::clamp(sConfigMgr->GetOption<float>(key, 0.0f, false), -0.99f, 0.99f);
            attributes.Correlation[first][second] = value;
            attributes.Correlation[second][first] = value;
        }

        if (!FactorAttributeMatrix(attributes.Correlation, attributes.Factor))
        {
            LOG_ERROR("server.loading",
                      "Playerbots.Skill.Attributes.Corr.*: the correlation matrix is not positive definite; the "
                      "attributes' own parts are drawn uncorrelated (identity matrix)");
            attributes.Correlation = IdentityAttributeMatrix();
        }
    }
    {
        NaturalReactionEnable = sConfigMgr->GetOption<bool>("Playerbots.Natural.Reaction.Enable", false);
        NaturalReactionActive = SkillEnable && NaturalReactionEnable;
        auto const ms = [](std::string const& key, uint32 fallback)
        { return std::clamp<uint32>(sConfigMgr->GetOption<uint32>(key, fallback), 0, 10000); };
        auto const real = [](std::string const& key, float fallback, float low, float high)
        { return std::clamp(sConfigMgr->GetOption<float>(key, fallback), low, high); };
        ReactionParams& reaction = NaturalReaction;
        reaction.MedianMsAtSkill1 = real("Playerbots.Natural.Reaction.MedianMsAtSkill1", 690.0f, 50.0f, 10000.0f);
        reaction.MedianMsAtSkill99 = real("Playerbots.Natural.Reaction.MedianMsAtSkill99", 220.0f, 50.0f, 10000.0f);
        reaction.SigmaAtSkill1 = real("Playerbots.Natural.Reaction.SigmaAtSkill1", 0.45f, 0.0f, 2.0f);
        reaction.SigmaAtSkill99 = real("Playerbots.Natural.Reaction.SigmaAtSkill99", 0.25f, 0.0f, 2.0f);
        reaction.MinMs = ms("Playerbots.Natural.Reaction.MinMs", 150);
        reaction.BehindMinMs = ms("Playerbots.Natural.Reaction.BehindPenaltyMinMs", 150);
        reaction.BehindMaxMs =
            std::max(reaction.BehindMinMs, ms("Playerbots.Natural.Reaction.BehindPenaltyMaxMs", 400));
        reaction.SwitchMinMs = ms("Playerbots.Natural.Reaction.SwitchCostMinMs", 100);
        reaction.SwitchMaxMs = std::max(reaction.SwitchMinMs, ms("Playerbots.Natural.Reaction.SwitchCostMaxMs", 300));
        reaction.ConeDegrees = real("Playerbots.Natural.Reaction.ConeDegrees", 200.0f, 10.0f, 360.0f);
        reaction.FocusTimeoutMs = ms("Playerbots.Natural.Reaction.FocusTimeoutMs", 2000);
        reaction.EndToEnd = sConfigMgr->GetOption<bool>("Playerbots.Natural.Reaction.EndToEnd", false);
        reaction.PingMs = ms("Playerbots.Natural.Reaction.PingMs", 60);
        reaction.DisplayMs = ms("Playerbots.Natural.Reaction.DisplayMs", 30);
        reaction.ServerTickMs =
            std::clamp(sConfigMgr->GetOption<int32>("Playerbots.Natural.Reaction.ServerTickMs", -1), -1, 1000);
        // G0h3's parts (NaturalReaction.h), each off by default
        auto const flag = [](std::string const& key) { return sConfigMgr->GetOption<bool>(key, false); };
        auto const count = [](std::string const& key, uint32 fallback, uint32 low, uint32 high)
        { return std::clamp<uint32>(sConfigMgr->GetOption<uint32>(key, fallback), low, high); };
        std::string const part = "Playerbots.Natural.Reaction.";
        ReactionChoiceParams& choice = reaction.Choice;
        choice.Enable = flag(part + "Choice.Enable");
        choice.MsPerDoublingAt1 = real(part + "Choice.MsPerDoublingAtSkill1", 120.0f, 0.0f, 2000.0f);
        choice.MsPerDoublingAt99 = real(part + "Choice.MsPerDoublingAtSkill99", 50.0f, 0.0f, 2000.0f);
        choice.MaxOptions = count(part + "Choice.MaxOptions", 8, 1, 64);
        ReactionLoadParams& load = reaction.Load;
        load.Enable = flag(part + "Load.Enable");
        load.MsPerEventAt1 = ms(part + "Load.MsPerEventAtSkill1", 200);
        load.MsPerEventAt99 = ms(part + "Load.MsPerEventAtSkill99", 80);
        load.MissPercentPerEventAt1 = count(part + "Load.MissPercentPerEventAtSkill1", 10, 0, 100);
        load.MissPercentPerEventAt99 = count(part + "Load.MissPercentPerEventAtSkill99", 2, 0, 100);
        load.TopProtectAt1 = real(part + "Load.TopProtectAtSkill1", 0.0f, 0.0f, 1.0f);
        load.TopProtectAt99 = real(part + "Load.TopProtectAtSkill99", 0.8f, 0.0f, 1.0f);
        load.MaxEvents = count(part + "Load.MaxEvents", 3, 1, 5);
        ReactionEaseParams& ease = reaction.Ease;
        ease.Enable = flag(part + "Ease.Enable");
        static char const* const easeNames[REACTION_EASE_COUNT] = {"Common", "Rare", "Focus", "NewTarget"};
        ReactionEaseParams const easeDefaults;
        for (uint32 index = 0; index < REACTION_EASE_COUNT; ++index)
        {
            std::string const prefix = part + "Ease." + easeNames[index] + "MsAtSkill";
            ease.MsAt1[index] = ms(prefix + "1", easeDefaults.MsAt1[index]);
            ease.MsAt99[index] = ms(prefix + "99", easeDefaults.MsAt99[index]);
            // Strictly rising at both ends, so at every Dexterity
            if (index == 0)
                continue;

            ease.MsAt1[index] = std::max(ease.MsAt1[index], ease.MsAt1[index - 1] + 1);
            ease.MsAt99[index] = std::max(ease.MsAt99[index], ease.MsAt99[index - 1] + 1);
        }

        ease.FocusDexterity = static_cast<uint8>(count(part + "Ease.FocusDexterity", 40, 1, 99));
        ReactionThinkParams& think = reaction.Think;
        think.Enable = flag(part + "Think.Enable");
        think.ShortcutMsAt1 = ms(part + "Think.ShortcutMsAtSkill1", 40);
        think.ShortcutMsAt99 = ms(part + "Think.ShortcutMsAtSkill99", 20);
        think.DeepMsAt1 = ms(part + "Think.DeepMsAtSkill1", 400);
        think.DeepMsAt99 = ms(part + "Think.DeepMsAtSkill99", 150);
        think.DeepPercentAt1 = count(part + "Think.DeepPercentAtSkill1", 0, 0, 100);
        think.DeepPercentAt99 = count(part + "Think.DeepPercentAtSkill99", 100, 0, 100);
        think.PatienceWaitMsAt0 = ms(part + "Think.PatienceWaitMsAt0", 0);
        think.PatienceWaitMsAt100 = ms(part + "Think.PatienceWaitMsAt100", 1500);
        ReactionAnticipateParams& anticipate = reaction.Anticipate;
        anticipate.Enable = flag(part + "Anticipate.Enable");
        anticipate.ShareAt1 = real(part + "Anticipate.ShareAtSkill1", 0.1f, 0.0f, 0.95f);
        anticipate.ShareAt99 = real(part + "Anticipate.ShareAtSkill99", 0.5f, 0.0f, 0.95f);
        ReactionQueueParams& queue = reaction.Queue;
        queue.Enable = flag(part + "Queue.Enable");
        queue.MinMsAt1 = ms(part + "Queue.MinMsAtSkill1", 60);
        queue.MaxMsAt1 = std::max(queue.MinMsAt1, ms(part + "Queue.MaxMsAtSkill1", 250));
        queue.MinMsAt99 = ms(part + "Queue.MinMsAtSkill99", 10);
        queue.MaxMsAt99 = std::max(queue.MinMsAt99, ms(part + "Queue.MaxMsAtSkill99", 80));
        ReactionReevalParams& reeval = reaction.Reeval;
        reeval.Enable = flag(part + "Reeval.Enable");
        reeval.ImportantCastMs = ms(part + "Reeval.ImportantCastMs", 1500);
        reeval.MinOwnRemainingMs = ms(part + "Reeval.MinOwnRemainingMs", 300);
        reeval.BusySkip = flag(part + "Reeval.BusySkip");
        reeval.BusyMarginMs = ms(part + "Reeval.BusyMarginMs", 100);
        ReactionMemoryParams& memory = reaction.Memory;
        memory.Enable = flag(part + "Memory.Enable");
        memory.MinCasts = count(part + "Memory.MinCasts", 3, 1, 100);
        memory.EndToleranceMs = ms(part + "Memory.EndToleranceMs", 150);
        reaction.MovementEnable = flag(part + "Movement.Enable");
        // Arena and battleground only by default: the model's arm cost measurably more per tick (G0h2-b)
        uint8 const reactionMatches =
            static_cast<uint8>(TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground));
        std::string const contexts =
            sConfigMgr->GetOption<std::string>("Playerbots.Natural.Reaction.Contexts", "Arena,Battleground");
        std::string error;
        NaturalReactionContexts = reactionMatches;
        if (!ParseTacticScope(contexts, NaturalReactionContexts, error))
        {
            LOG_ERROR("server.loading", "Playerbots.Natural.Reaction.Contexts: {}; Arena,Battleground is used", error);
            NaturalReactionContexts = reactionMatches;
        }

        NaturalCadenceEnable = sConfigMgr->GetOption<bool>("Playerbots.Natural.Cadence.Enable", false);
        NaturalCadenceCheckMs =
            std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Natural.Cadence.CheckMs", 100), 20, 1000);
        NaturalCadenceStaggerMs = std::clamp<uint32>(
            sConfigMgr->GetOption<uint32>("Playerbots.Natural.Cadence.StaggerMs", 20), 0, UPSTREAM_CHECK_STAGGER_MS);
        NaturalCadenceNearRealPlayerYards = std::clamp(
            sConfigMgr->GetOption<float>("Playerbots.Natural.Cadence.NearRealPlayerYards", 0.0f), 0.0f, 500.0f);
        uint8 const matches =
            static_cast<uint8>(TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground));
        std::string const cadenceContexts =
            sConfigMgr->GetOption<std::string>("Playerbots.Natural.Cadence.Contexts", "Arena,Battleground");
        std::string cadenceError;
        NaturalCadenceContexts = matches;
        if (!ParseTacticScope(cadenceContexts, NaturalCadenceContexts, cadenceError))
        {
            LOG_ERROR("server.loading", "Playerbots.Natural.Cadence.Contexts: {}; Arena,Battleground is used",
                      cadenceError);
            NaturalCadenceContexts = matches;
        }
    }
    TacticsEnable = sConfigMgr->GetOption<bool>("Playerbots.Tactics.Enable", false);
    TacticsConfigMask = 0;
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
    {
        TacticDefinition const& definition = TACTIC_DEFINITIONS[index];
        std::string const prefix = std::string("Playerbots.Tactics.") + definition.ConfigName;
        TacticSettings& settings = TacticConfig[index];
        // The per-tactic keys are optional (commented in playerbots.conf.dist): no missing-key warnings
        settings.Enable = sConfigMgr->GetOption<bool>(prefix + ".Enable", false, false);
        settings.Scope = definition.DefaultScope;
        std::string const scope = sConfigMgr->GetOption<std::string>(prefix + ".Scope", "", false);
        std::string error;
        if (!scope.empty() && !ParseTacticScope(scope, settings.Scope, error))
        {
            LOG_WARN("playerbots", "{}.Scope: {}; using {}", prefix, error, FormatTacticScope(definition.DefaultScope));
            settings.Scope = definition.DefaultScope;
        }

        auto const percent = [&prefix](char const* key, uint32 fallback)
        {
            return static_cast<uint8>(
                std::clamp<uint32>(sConfigMgr->GetOption<uint32>(prefix + key, fallback, false), 0, 100));
        };
        settings.KnownFromSkill = percent(".KnownFromSkill", 0);
        settings.UseChanceAtSkill1 = percent(".UseChanceAtSkill1", 100);
        settings.UseChanceAtSkill99 = percent(".UseChanceAtSkill99", 100);

        if (TacticsEnable && settings.Enable)
            TacticsConfigMask |= TacticBit(static_cast<TacticId>(index));
    }

    // Playerbots.Tactics.Hold.* and .Focus.* (optional): the target hold's and the team plan's numbers, the defaults in
    // FocusConfig; read by the pvp hold and arena focus strategies (FocusStrategy.cpp)
    TacticFocus = FocusConfig{};
    constexpr uint32 MaxFocusMs = 120000;
    constexpr uint32 MaxFocusPct = 100;
    constexpr uint32 MaxFocusMargin = 255;
    constexpr uint32 MaxFocusSeconds = 600;
    constexpr uint32 MaxFocusSkill = 99;
    auto const focusNumber = [](char const* key, uint32& value, uint32 maximum)
    {
        value = std::min<uint32>(sConfigMgr->GetOption<uint32>(std::string("Playerbots.Tactics.") + key, value, false),
                                 maximum);
    };
    focusNumber("Hold.HoldMsAtSkill1", TacticFocus.HoldMsAtSkill1, MaxFocusMs);
    focusNumber("Hold.HoldMsAtSkill99", TacticFocus.HoldMsAtSkill99, MaxFocusMs);
    focusNumber("Hold.HysteresisPctAtSkill1", TacticFocus.HoldHysteresisPctAtSkill1, MaxFocusPct);
    focusNumber("Hold.HysteresisPctAtSkill99", TacticFocus.HoldHysteresisPctAtSkill99, MaxFocusPct);
    focusNumber("Hold.ExecPctAtSkill1", TacticFocus.HoldExecPctAtSkill1, MaxFocusPct);
    focusNumber("Hold.ExecPctAtSkill99", TacticFocus.HoldExecPctAtSkill99, MaxFocusPct);
    focusNumber("Hold.ExecChanceAtSkill1", TacticFocus.HoldExecChanceAtSkill1, MaxFocusPct);
    focusNumber("Hold.ExecChanceAtSkill99", TacticFocus.HoldExecChanceAtSkill99, MaxFocusPct);
    focusNumber("Hold.FollowChanceAtSkill1", TacticFocus.HoldFollowChanceAtSkill1, MaxFocusPct);
    focusNumber("Hold.FollowChanceAtSkill99", TacticFocus.HoldFollowChanceAtSkill99, MaxFocusPct);
    focusNumber("Hold.FollowDelayMsAtSkill1", TacticFocus.HoldFollowDelayMsAtSkill1, MaxFocusMs);
    focusNumber("Hold.FollowDelayMsAtSkill99", TacticFocus.HoldFollowDelayMsAtSkill99, MaxFocusMs);
    focusNumber("Hold.LosLostMs", TacticFocus.HoldLosLostMs, MaxFocusMs);
    focusNumber("Focus.PlanIntervalMsAtSkill1", TacticFocus.FocusPlanIntervalMsAtSkill1, MaxFocusMs);
    focusNumber("Focus.PlanIntervalMsAtSkill99", TacticFocus.FocusPlanIntervalMsAtSkill99, MaxFocusMs);
    focusNumber("Focus.HoldMsAtSkill1", TacticFocus.FocusHoldMsAtSkill1, MaxFocusMs);
    focusNumber("Focus.HoldMsAtSkill99", TacticFocus.FocusHoldMsAtSkill99, MaxFocusMs);
    focusNumber("Focus.HysteresisPctAtSkill1", TacticFocus.FocusHysteresisPctAtSkill1, MaxFocusPct);
    focusNumber("Focus.HysteresisPctAtSkill99", TacticFocus.FocusHysteresisPctAtSkill99, MaxFocusPct);
    focusNumber("Focus.ExecPctAtSkill1", TacticFocus.FocusExecPctAtSkill1, MaxFocusPct);
    focusNumber("Focus.ExecPctAtSkill99", TacticFocus.FocusExecPctAtSkill99, MaxFocusPct);
    focusNumber("Focus.ExecChanceAtSkill1", TacticFocus.FocusExecChanceAtSkill1, MaxFocusPct);
    focusNumber("Focus.ExecChanceAtSkill99", TacticFocus.FocusExecChanceAtSkill99, MaxFocusPct);
    focusNumber("Focus.RemarkDelayMsAtSkill1", TacticFocus.FocusRemarkDelayMsAtSkill1, MaxFocusMs);
    focusNumber("Focus.RemarkDelayMsAtSkill99", TacticFocus.FocusRemarkDelayMsAtSkill99, MaxFocusMs);
    focusNumber("Focus.StrongMarginAtSkill1", TacticFocus.FocusStrongMarginAtSkill1, MaxFocusMargin);
    focusNumber("Focus.StrongMarginAtSkill99", TacticFocus.FocusStrongMarginAtSkill99, MaxFocusMargin);
    focusNumber("Focus.MinRemarkSeconds", TacticFocus.FocusMinRemarkSeconds, MaxFocusSeconds);
    focusNumber("Focus.HumanMarkRespectSeconds", TacticFocus.FocusHumanMarkRespectSeconds, MaxFocusSeconds);
    focusNumber("Focus.LosLostMs", TacticFocus.FocusLosLostMs, MaxFocusMs);
    focusNumber("Focus.DefensiveSwapPct", TacticFocus.FocusDefensiveSwapPct, MaxFocusPct);
    focusNumber("Focus.AddonKnowledgeFromSkill", TacticFocus.FocusAddonKnowledgeFromSkill, MaxFocusSkill);
    // G1b-c: 0 no moon, 1 G1b's moon, 2 the moon only while the team's CC lands (ArenaFocus.h FOCUS_MOON_*)
    constexpr uint32 MaxMoonMode = 2;
    focusNumber("Focus.MoonMode", TacticFocus.FocusMoonMode, MaxMoonMode);
    focusNumber("Focus.MoonGraceMs", TacticFocus.FocusMoonGraceMs, MaxFocusMs);
    focusNumber("Focus.MoonRetryMs", TacticFocus.FocusMoonRetryMs, MaxFocusMs);

    // Playerbots.Tactics.Cc.* and .Avoid.* (optional): crowd control's and its avoidance's numbers, the defaults in
    // CcConfig; read by the Cc, OwnCc and Trinket tactics (G4b, G5a; inert until then)
    TacticCc = CcConfig{};
    constexpr uint32 MaxCcSkill = 100;
    focusNumber("Cc.DrAwareFromSkill", TacticCc.CcDrAwareFromSkill, MaxCcSkill);
    focusNumber("Cc.ChainsFromSkill", TacticCc.CcChainsFromSkill, MaxCcSkill);
    focusNumber("Cc.MinExpectedMs", TacticCc.CcMinExpectedMs, MaxFocusMs);
    focusNumber("Cc.DrMissPctAtSkill1", TacticCc.CcDrMissPctAtSkill1, MaxFocusPct);
    focusNumber("Cc.DrMissPctAtSkill99", TacticCc.CcDrMissPctAtSkill99, MaxFocusPct);
    focusNumber("Cc.ChainLeadMsAtSkill1", TacticCc.CcChainLeadMsAtSkill1, MaxFocusMs);
    focusNumber("Cc.ChainLeadMsAtSkill99", TacticCc.CcChainLeadMsAtSkill99, MaxFocusMs);
    focusNumber("Cc.WrongTargetPctAtSkill1", TacticCc.CcWrongTargetPctAtSkill1, MaxFocusPct);
    focusNumber("Cc.WrongTargetPctAtSkill99", TacticCc.CcWrongTargetPctAtSkill99, MaxFocusPct);
    focusNumber("Cc.PatientHoldMs", TacticCc.CcPatientHoldMs, MaxFocusMs);
    focusNumber("Cc.BehindStopPct", TacticCc.CcBehindStopPct, MaxFocusPct);
    // G4c: chains and the opponent memory; 0/1 switches
    constexpr uint32 MaxCcSwitch = 1;
    constexpr uint32 MaxCcEvents = 20;
    focusNumber("Cc.ChainEnable", TacticCc.CcChainEnable, MaxCcSwitch);
    focusNumber("Cc.OpponentMemoryEnable", TacticCc.CcOpponentMemoryEnable, MaxCcSwitch);
    focusNumber("Cc.OpponentMinEvents", TacticCc.CcOpponentMinEvents, MaxCcEvents);
    focusNumber("Cc.DispelMarginMs", TacticCc.CcDispelMarginMs, MaxFocusMs);
    focusNumber("Avoid.OwnBreakPctAtSkill1", TacticCc.AvoidOwnBreakPctAtSkill1, MaxFocusPct);
    focusNumber("Avoid.OwnBreakPctAtSkill99", TacticCc.AvoidOwnBreakPctAtSkill99, MaxFocusPct);
    focusNumber("Avoid.OwnBreakNeverFromSkill", TacticCc.AvoidOwnBreakNeverFromSkill, MaxCcSkill);
    focusNumber("Avoid.WastedBreakPctAtSkill1", TacticCc.AvoidWastedBreakPctAtSkill1, MaxFocusPct);
    focusNumber("Avoid.WastedBreakPctAtSkill99", TacticCc.AvoidWastedBreakPctAtSkill99, MaxFocusPct);
    focusNumber("Avoid.IgnoreRootsFromSkill", TacticCc.AvoidIgnoreRootsFromSkill, MaxCcSkill);
    focusNumber("Avoid.MinLeftMs", TacticCc.AvoidMinLeftMs, MaxFocusMs);
    focusNumber("Avoid.LowHealthPct", TacticCc.AvoidLowHealthPct, MaxFocusPct);
    // G5a: the Trinket and Dispel tactics' numbers
    constexpr uint32 MaxEnsureTrinket = 2;
    focusNumber("Avoid.WastedBreakNeverFromSkill", TacticCc.AvoidWastedBreakNeverFromSkill, MaxCcSkill);
    focusNumber("Avoid.CautiousLowHealthPct", TacticCc.AvoidCautiousLowHealthPct, MaxFocusPct);
    focusNumber("Avoid.EnsureTrinket", TacticCc.AvoidEnsureTrinket, MaxEnsureTrinket);
    // G4c-b: OwnCc's own-break guards
    constexpr uint32 MaxCcMarginYards = 20;
    focusNumber("Avoid.AoeGuardEnable", TacticCc.AvoidAoeGuardEnable, MaxCcSwitch);
    focusNumber("Avoid.AoeGuardMarginYards", TacticCc.AvoidAoeGuardMarginYards, MaxCcMarginYards);
    focusNumber("Avoid.DotGuardEnable", TacticCc.AvoidDotGuardEnable, MaxCcSwitch);
    focusNumber("Avoid.PetCallOffEnable", TacticCc.AvoidPetCallOffEnable, MaxCcSwitch);
    focusNumber("Avoid.ExcludeFightEnemies", TacticCc.AvoidExcludeFightEnemies, MaxCcSwitch);

    // Playerbots.Tactics.Interrupt.* (optional, G5b): the Interrupt tactic's numbers, the defaults in InterruptConfig;
    // read by the interrupt triggers and "enemy healer target" while Playerbots.Tactics.Interrupt.Enable is 1
    TacticInterrupt = InterruptConfig{};
    constexpr uint32 MaxInterruptCasts = 20;
    constexpr uint32 MaxInterruptCacheMs = 5000;
    focusNumber("Interrupt.HealSharePctAtSkill1", TacticInterrupt.HealSharePctAtSkill1, MaxFocusPct);
    focusNumber("Interrupt.HealSharePctAtSkill99", TacticInterrupt.HealSharePctAtSkill99, MaxFocusPct);
    focusNumber("Interrupt.DamageSharePct", TacticInterrupt.DamageSharePct, MaxFocusPct);
    focusNumber("Interrupt.CcSharePct", TacticInterrupt.CcSharePct, MaxFocusPct);
    focusNumber("Interrupt.MaxSharePct", TacticInterrupt.MaxSharePct, MaxFocusPct);
    focusNumber("Interrupt.SigmaPctAtSkill1", TacticInterrupt.SigmaPctAtSkill1, MaxFocusPct);
    focusNumber("Interrupt.SigmaPctAtSkill99", TacticInterrupt.SigmaPctAtSkill99, MaxFocusPct);
    focusNumber("Interrupt.UrgentHpPct", TacticInterrupt.UrgentHpPct, MaxFocusPct);
    focusNumber("Interrupt.CancelSharePct", TacticInterrupt.CancelSharePct, MaxFocusPct);
    focusNumber("Interrupt.CancelMinCasts", TacticInterrupt.CancelMinCasts, MaxInterruptCasts);
    focusNumber("Interrupt.PatienceSharePct", TacticInterrupt.PatienceSharePct, MaxFocusPct);
    focusNumber("Interrupt.HoldForHealer", TacticInterrupt.HoldForHealer, MaxCcSwitch);
    focusNumber("Interrupt.HealerCacheMs", TacticInterrupt.HealerCacheMs, MaxInterruptCacheMs);

    BotActiveAloneSmartScale = sConfigMgr->GetOption<bool>("Playerbots.botActiveAloneSmartScale", 1);
    BotActiveAloneSmartScaleDiffLimitFloor = sConfigMgr->GetOption<uint32>("Playerbots.botActiveAloneSmartScaleDiffLimitfloor", 50);
    BotActiveAloneSmartScaleDiffLimitCeiling = sConfigMgr->GetOption<uint32>("Playerbots.botActiveAloneSmartScaleDiffLimitCeiling", 200);
    BotActiveAloneSmartScaleWhenMinLevel = sConfigMgr->GetOption<uint32>("Playerbots.botActiveAloneSmartScaleWhenMinLevel", 1);
    BotActiveAloneSmartScaleWhenMaxLevel = sConfigMgr->GetOption<uint32>("Playerbots.botActiveAloneSmartScaleWhenMaxLevel", 80);
    BotActiveAloneSmartScalePercentile = sConfigMgr->GetOption<uint32>("Playerbots.botActiveAloneSmartScalePercentile", 0);
    if (BotActiveAloneSmartScalePercentile > 100)
    {
        LOG_WARN("playerbots", "Playerbots.botActiveAloneSmartScalePercentile {} is above 100, using 100",
                 BotActiveAloneSmartScalePercentile);
        BotActiveAloneSmartScalePercentile = 100;
    }

    SituationEnable = sConfigMgr->GetOption<bool>("Playerbots.Situation.Enable", false);
    SituationIntervalMs =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.Situation.IntervalMs", 1000), 100, 60000);
    // In BotSituation order (ActivityBudget.h): open world, dungeon, boss, PvP
    static char const* const situationKeys[] = {"OpenWorld", "Dungeon", "Boss", "Pvp"};
    static char const* const situationNames[] = {"open", "dungeon", "boss", "pvp"};
    for (size_t i = 0; i < std::size(situationKeys); ++i)
    {
        std::string const prefix = std::string("Playerbots.Situation.") + situationKeys[i];
        SituationDiffLimitFloor[i] = sConfigMgr->GetOption<uint32>(prefix + ".DiffLimitFloor", 0);
        SituationDiffLimitCeiling[i] = sConfigMgr->GetOption<uint32>(prefix + ".DiffLimitCeiling", 0);
    }

    std::string situationForce = sConfigMgr->GetOption<std::string>("Playerbots.Situation.Force", "");
    std::transform(situationForce.begin(), situationForce.end(), situationForce.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    SituationForce = -1;
    if (!situationForce.empty())
    {
        for (size_t i = 0; i < std::size(situationNames); ++i)
        {
            if (situationForce == situationNames[i])
                SituationForce = static_cast<int32>(i);
        }

        if (SituationForce < 0)
            LOG_WARN("playerbots", "Playerbots.Situation.Force '{}' is not pvp, boss, dungeon or open: detecting",
                     situationForce);
    }

    RandomBotsWalkingRPG = sConfigMgr->GetOption<bool>("Playerbots.RandombotsWalkingRPG", false);
    RandomBotsWalkingRPGInDoors = sConfigMgr->GetOption<bool>("Playerbots.RandombotsWalkingRPG.InDoors", false);
    MinEnchantingBotLevel = sConfigMgr->GetOption<int32>("Playerbots.MinEnchantingBotLevel", 60);
    LimitEnchantExpansion = sConfigMgr->GetOption<int32>("Playerbots.LimitEnchantExpansion", 1);
    LimitGearExpansion = sConfigMgr->GetOption<int32>("Playerbots.LimitGearExpansion", 1);
    RandomBotStartingLevel = sConfigMgr->GetOption<int32>("Playerbots.RandombotStartingLevel", 1);
    EnablePeriodicOnlineOffline = sConfigMgr->GetOption<bool>("Playerbots.EnablePeriodicOnlineOffline", false);
    EnableRandomBotTrading = sConfigMgr->GetOption<int32>("Playerbots.EnableRandomBotTrading", 1);
    PeriodicOnlineOfflineRatio = sConfigMgr->GetOption<float>("Playerbots.PeriodicOnlineOfflineRatio", 2.0);
    GearScoreCheck = sConfigMgr->GetOption<bool>("Playerbots.GearScoreCheck", false);
    RandomBotPreQuests = sConfigMgr->GetOption<bool>("Playerbots.PreQuests", false);

    // SPP automation
    FreeMethodLoot = sConfigMgr->GetOption<bool>("Playerbots.FreeMethodLoot", false);
    LootNeedRollLevel = sConfigMgr->GetOption<int32>("Playerbots.LootNeedRollLevel", 1);
    LootRollRecipe = sConfigMgr->GetOption<bool>("Playerbots.LootRollRecipe", false);
    LootRollDisenchant = sConfigMgr->GetOption<bool>("Playerbots.LootRollDisenchant", false);
    LootGreedRollLevel = sConfigMgr->GetOption<bool>("Playerbots.LootGreedRollLevel", false);
    AutoPickReward = sConfigMgr->GetOption<std::string>("Playerbots.AutoPickReward", "yes");
    AutoEquipUpgradeLoot = sConfigMgr->GetOption<bool>("Playerbots.AutoEquipUpgradeLoot", true);
    EquipUpgradeThreshold = sConfigMgr->GetOption<float>("Playerbots.EquipUpgradeThreshold", 1.1f);
    TwoRoundsGearInit = sConfigMgr->GetOption<bool>("Playerbots.TwoRoundsGearInit", false);
    SyncQuestWithPlayer = sConfigMgr->GetOption<bool>("Playerbots.SyncQuestWithPlayer", true);
    SyncQuestForPlayer = sConfigMgr->GetOption<bool>("Playerbots.SyncQuestForPlayer", false);
    DropObsoleteQuests = sConfigMgr->GetOption<bool>("Playerbots.DropObsoleteQuests", true);
    AllowLearnTrainerSpells = sConfigMgr->GetOption<bool>("Playerbots.AllowLearnTrainerSpells", true);
    AutoPickTalents = sConfigMgr->GetOption<bool>("Playerbots.AutoPickTalents", true);
    AutoUpgradeEquip = sConfigMgr->GetOption<bool>("Playerbots.AutoUpgradeEquip", true);
    HunterWolfPet = sConfigMgr->GetOption<int32>("Playerbots.HunterWolfPet", 0);
    DefaultPetStance = sConfigMgr->GetOption<int32>("Playerbots.DefaultPetStance", 1);
    PetChatCommandDebug = sConfigMgr->GetOption<bool>("Playerbots.PetChatCommandDebug", 0);
    AutoLearnTrainerSpells = sConfigMgr->GetOption<bool>("Playerbots.AutoLearnTrainerSpells", true);
    AutoLearnQuestSpells = sConfigMgr->GetOption<bool>("Playerbots.AutoLearnQuestSpells", true);
    AutoTeleportForLevel = sConfigMgr->GetOption<bool>("Playerbots.AutoTeleportForLevel", false);
    AutoDoQuests = sConfigMgr->GetOption<bool>("Playerbots.AutoDoQuests", true);
    EnableNewRpgStrategy = sConfigMgr->GetOption<bool>("Playerbots.EnableNewRpgStrategy", true);

    RpgStatusProbWeight[RPG_WANDER_RANDOM] = sConfigMgr->GetOption<int32>("Playerbots.RpgStatusProbWeight.WanderRandom", 15);
    RpgStatusProbWeight[RPG_WANDER_NPC] = sConfigMgr->GetOption<int32>("Playerbots.RpgStatusProbWeight.WanderNpc", 20);
    RpgStatusProbWeight[RPG_GO_GRIND] = sConfigMgr->GetOption<int32>("Playerbots.RpgStatusProbWeight.GoGrind", 15);
    RpgStatusProbWeight[RPG_GO_CAMP] = sConfigMgr->GetOption<int32>("Playerbots.RpgStatusProbWeight.GoCamp", 10);
    RpgStatusProbWeight[RPG_DO_QUEST] = sConfigMgr->GetOption<int32>("Playerbots.RpgStatusProbWeight.DoQuest", 60);
    RpgStatusProbWeight[RPG_TRAVEL_FLIGHT] = sConfigMgr->GetOption<int32>("Playerbots.RpgStatusProbWeight.TravelFlight", 15);
    RpgStatusProbWeight[RPG_REST] = sConfigMgr->GetOption<int32>("Playerbots.RpgStatusProbWeight.Rest", 5);
    RpgStatusProbWeight[RPG_OUTDOOR_PVP] = sConfigMgr->GetOption<int32>("Playerbots.RpgStatusProbWeight.OutdoorPvp", 10);
    RpgStatusProbWeight[RPG_DO_GATHER] = sConfigMgr->GetOption<int32>("Playerbots.RpgStatusProbWeight.DoGather", 15);

    SyncLevelWithPlayers = sConfigMgr->GetOption<bool>("Playerbots.SyncLevelWithPlayers", false);
    RandomBotConcentrateInPlayerZone =
        sConfigMgr->GetOption<bool>("Playerbots.RandomBotConcentrateInPlayerZone", false);
    RandomBotGroupNearby = sConfigMgr->GetOption<bool>("Playerbots.RandomBotGroupNearby", false);

    // arena
    RandomBotArenaTeam2v2Count = sConfigMgr->GetOption<int32>("Playerbots.RandomBotArenaTeam2v2Count", 10);
    RandomBotArenaTeam3v3Count = sConfigMgr->GetOption<int32>("Playerbots.RandomBotArenaTeam3v3Count", 10);
    RandomBotArenaTeam5v5Count = sConfigMgr->GetOption<int32>("Playerbots.RandomBotArenaTeam5v5Count", 5);
    DeleteRandomBotArenaTeams = sConfigMgr->GetOption<bool>("Playerbots.DeleteRandomBotArenaTeams", false);
    ArenaTeamSeedEnable = sConfigMgr->GetOption<bool>("Playerbots.ArenaTeam.Seed.Enable", false);
    ArenaTeamSeedSpread =
        std::clamp(sConfigMgr->GetOption<float>("Playerbots.ArenaTeam.Seed.Spread", 800.0f), 0.0f, 3000.0f);
    ArenaTeamSeedNoiseSd =
        std::clamp(sConfigMgr->GetOption<float>("Playerbots.ArenaTeam.Seed.NoiseSd", 100.0f), 0.0f, 1000.0f);
    ArenaOnlyEnable = sConfigMgr->GetOption<bool>("Playerbots.ArenaOnly.Enable", false);
    ArenaOnlySpreadContinents = sConfigMgr->GetOption<bool>("Playerbots.ArenaOnly.SpreadContinents", true);
    RandomBotTestNamePrefix = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotTestNamePrefix", "");
    if (!RandomBotTestNamePrefix.empty())
    {
        // The core's name check takes letters only (no digits) and a name of at most 12: room for 3 counter letters
        bool const letters = std::all_of(RandomBotTestNamePrefix.begin(), RandomBotTestNamePrefix.end(),
                                         [](unsigned char c) { return std::isalpha(c) != 0; });
        if (!letters || RandomBotTestNamePrefix.size() < 2 || RandomBotTestNamePrefix.size() > 9)
        {
            LOG_ERROR("server.loading",
                      "Playerbots.RandomBotTestNamePrefix = {}: 2-9 letters only; the name pool is used",
                      RandomBotTestNamePrefix);
            RandomBotTestNamePrefix.clear();
        }
        else
        {
            std::transform(RandomBotTestNamePrefix.begin(), RandomBotTestNamePrefix.end(),
                           RandomBotTestNamePrefix.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            RandomBotTestNamePrefix[0] = static_cast<char>(std::toupper(RandomBotTestNamePrefix[0]));
        }
    }
    RandomBotArenaTeamMaxRating = sConfigMgr->GetOption<int32>("Playerbots.RandomBotArenaTeamMaxRating", 2000);
    RandomBotArenaTeamMinRating = sConfigMgr->GetOption<int32>("Playerbots.RandomBotArenaTeamMinRating", 1000);
    RandomBotArenaTeamRatingMode =
        sConfigMgr->GetOption<int32>("Playerbots.RandomBotArenaTeamRatingMode", 0) == ARENA_RATING_MODE_EARNED
            ? ARENA_RATING_MODE_EARNED
            : ARENA_RATING_MODE_RANDOM;
    ArenaTeamFormationEnable = sConfigMgr->GetOption<bool>("Playerbots.ArenaTeam.Formation.Enable", false);
    ArenaTeamUpdateSeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.UpdateSeconds", 60), 5, 3600);
    ArenaTeamMaxRoster2v2 =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.MaxRoster2v2", 4), 2, 4);
    ArenaTeamMaxRoster3v3 =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.MaxRoster3v3", 6), 3, 6);
    ArenaTeamMaxRoster5v5 =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.MaxRoster5v5", 10), 5, 10);
    ArenaTeamMinMembers =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.MinMembers", 2), 2, 10);
    ArenaTeamRandomPair = sConfigMgr->GetOption<bool>("Playerbots.ArenaTeam.RandomPair", true);
    ArenaTeamRepickEachQueue = sConfigMgr->GetOption<bool>("Playerbots.ArenaTeam.RepickEachQueue", true);
    ArenaTeamLossStreakToConsider =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.LossStreakToConsider", 3), 1, 50);
    ArenaTeamLeaveChanceBottom =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.LeaveChanceBottom", 20), 0, 100);
    ArenaTeamLeaveChanceTop =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.LeaveChanceTop", 50), 0, 100);
    ArenaTeamMinTeamStaySeconds =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.MinTeamStaySeconds", 600), 0, 86400);
    ArenaTeamToleranceBottom =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.ToleranceBottom", 40), 1, 98);
    ArenaTeamToleranceTop =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.ToleranceTop", 8), 1, 98);
    ArenaTeamSynergyWeight =
        std::clamp(sConfigMgr->GetOption<float>("Playerbots.ArenaTeam.SynergyWeight", 0.4f), 0.0f, 1.0f);
    ArenaTeamRecruitsPerPass =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.RecruitsPerPass", 4), 0, 100);
    ArenaTeamRecruitAboveMinChance =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaTeam.RecruitAboveMinChance", 0), 0, 100);
    ArenaRecruitEnable = sConfigMgr->GetOption<bool>("Playerbots.ArenaRecruit.Enable", false);
    ArenaRecruitRatingAtSkill1 =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaRecruit.RatingAtSkill1", 0), 0, 10000);
    ArenaRecruitRatingAtSkill99 =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaRecruit.RatingAtSkill99", 2000), 0, 10000);
    ArenaRecruitDeltaBase =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaRecruit.DeltaBase", 300), 0, 10000);
    ArenaRecruitDeltaPerAcquaintancePoint = std::clamp(
        sConfigMgr->GetOption<float>("Playerbots.ArenaRecruit.DeltaPerAcquaintancePoint", 4.0f), 0.0f, 1000.0f);
    ArenaRecruitDeltaMax =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaRecruit.DeltaMax", 1500), 0, 10000);
    ArenaRecruitLoyaltyMinGames =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaRecruit.LoyaltyMinGames", 5), 1, 1000);
    ArenaRecruitLoyaltyMinWinPercent =
        std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.ArenaRecruit.LoyaltyMinWinPercent", 55), 0, 100);
    // Ladder roles: an arena instance runs only the teams its roster hosts; main gets arena matches only from the
    // ladder's matchmaker
    if (LadderRole == LADDER_ROLE_ARENA)
    {
        if (RandomBotArenaTeam2v2Count || RandomBotArenaTeam3v3Count || RandomBotArenaTeam5v5Count ||
            DeleteRandomBotArenaTeams || ArenaTeamFormationEnable || ArenaRecruitEnable)
            LOG_INFO("server.loading",
                     "Playerbots.Ladder.Role = 2: bot arena team creation, deletion, formation and recruiting are off");

        RandomBotArenaTeam2v2Count = 0;
        RandomBotArenaTeam3v3Count = 0;
        RandomBotArenaTeam5v5Count = 0;
        DeleteRandomBotArenaTeams = false;
        ArenaTeamFormationEnable = false;
        ArenaRecruitEnable = false;
        if (LadderMatchmakerEnable)
            LOG_INFO("server.loading", "Playerbots.Ladder.Role = 2: the matchmaker is off");

        LadderMatchmakerEnable = false;
    }
    else if (LadderRole == LADDER_ROLE_MAIN)
    {
        if (RandomBotAutoJoinBGRatedArena2v2Count || RandomBotAutoJoinBGRatedArena3v3Count ||
            RandomBotAutoJoinBGRatedArena5v5Count)
            LOG_INFO("server.loading", "Playerbots.Ladder.Role = 1: bot rated arena auto-join is off");

        RandomBotAutoJoinBGRatedArena2v2Count = 0;
        RandomBotAutoJoinBGRatedArena3v3Count = 0;
        RandomBotAutoJoinBGRatedArena5v5Count = 0;
        // Test-run options: never on the server the players play on
        if (ArenaTeamSeedEnable || ArenaOnlyEnable || RandomBotAutoJoinRatedArenaUncapped)
            LOG_INFO("server.loading",
                     "Playerbots.Ladder.Role = 1: ArenaTeam.Seed, ArenaOnly and the uncapped rated queue are off");

        ArenaTeamSeedEnable = false;
        ArenaOnlyEnable = false;
        RandomBotAutoJoinRatedArenaUncapped = false;
    }

    if (ArenaTeamFormationEnable && RandomBotArenaTeamRatingMode != ARENA_RATING_MODE_EARNED)
        LOG_INFO("server.loading",
                 "Playerbots.ArenaTeam.Formation.Enable is ignored: it needs "
                 "Playerbots.RandomBotArenaTeamRatingMode = 1");

    SelfBotLevel = sConfigMgr->GetOption<int32>("Playerbots.SelfBotLevel", 1);

    RandomPlayerbotFactory::LoadLocalizedNameColumns();
    RandomPlayerbotFactory::CreateRandomBots();
    if (World::IsStopped())
    {
        return true;
    }

    // Assign account types after accounts are created
    sRandomPlayerbotMgr.AssignAccountTypes();

    if (sPlayerbotAIConfig.Enabled)
    {
        sRandomPlayerbotMgr.Init();
    }

    PlayerbotGuildMgr::instance().Init();
    LadderMgr::Instance().LoadRoster();  // Ladder role: drops the teams not hosted here before the registry is built
    sRandomPlayerbotMgr.InitArenaTeams();
    sRandomItemMgr.Init();
    sRandomItemMgr.InitAfterAhBot();
    sBisListMgr->LoadAll();
    PlayerbotTextMgr::instance().LoadBotTexts();
    PlayerbotTextMgr::instance().LoadBotTextChance();
    PlayerbotFactory::Init();

    AiObjectContext::BuildAllSharedContexts();

    if (sPlayerbotAIConfig.RandomBotSuggestDungeons)
    {
        PlayerbotDungeonRepository::instance().LoadDungeonSuggestions();
    }
    sTravelMgr.Init();

    ExcludedHunterPetFamilies.clear();
    LoadList<std::vector<uint32>>(sConfigMgr->GetOption<std::string>("Playerbots.ExcludedHunterPetFamilies", ""), ExcludedHunterPetFamilies);

    LOG_INFO("server.loading", "---------------------------------------");
    LOG_INFO("server.loading", "       mod-playerbots initialized      ");
    LOG_INFO("server.loading", "---------------------------------------");

    return true;
}

// Loads Playerbots.LevelBrackets.* and Playerbots.ResetBotLevel.* (see RandomBotLevelMgr). Also
// re-run on ".reload config" via RandomBotLevelWorldScript::OnAfterConfigLoad. Bracket
// bounds/percentages are only the as-configured values here; RandomBotLevelMgr::LoadConfig()
// copies them into its own working state, since dynamic distribution and clamp/rebalance mutate
// percentages at runtime.
void PlayerbotAIConfig::LoadRandomBotLevelConfig()
{
    // ---- Level brackets ----
    LevelBracketsEnabled = sConfigMgr->GetOption<bool>("Playerbots.LevelBrackets.Enabled", false);
    LevelBracketsIgnoreGuildWithRealPlayers =
        sConfigMgr->GetOption<bool>("Playerbots.LevelBrackets.IgnoreGuildBotsWithRealPlayers", true);
    LevelBracketsIgnoreArenaTeamBots =
        sConfigMgr->GetOption<bool>("Playerbots.LevelBrackets.IgnoreArenaTeamBots", true);

    LevelBracketsCheckFrequency = sConfigMgr->GetOption<uint32>("Playerbots.LevelBrackets.CheckFrequency", 300);
    LevelBracketsFlaggedCheckFrequency =
        sConfigMgr->GetOption<uint32>("Playerbots.LevelBrackets.CheckFlaggedFrequency", 15);
    LevelBracketsDynamicDistribution =
        sConfigMgr->GetOption<bool>("Playerbots.LevelBrackets.Dynamic.UseDynamicDistribution", false);
    LevelBracketsRealPlayerWeight =
        sConfigMgr->GetOption<float>("Playerbots.LevelBrackets.Dynamic.RealPlayerWeight", 1.0f);
    LevelBracketsSyncFactions = sConfigMgr->GetOption<bool>("Playerbots.LevelBrackets.Dynamic.SyncFactions", false);
    LevelBracketsIgnoreFriendListed = sConfigMgr->GetOption<bool>("Playerbots.LevelBrackets.IgnoreFriendListed", true);
    LevelBracketsFlaggedProcessLimit =
        sConfigMgr->GetOption<uint32>("Playerbots.LevelBrackets.FlaggedProcessLimit", 5);

    ParseLevelMgrExcludeNames(sConfigMgr->GetOption<std::string>("Playerbots.LevelBrackets.ExcludeNames", ""),
        LevelBracketsExcludeNames);

    LevelBracketsNumRanges =
        static_cast<uint8>(sConfigMgr->GetOption<uint32>("Playerbots.LevelBrackets.NumRanges", 9));
    LevelBracketsAlliance.resize(LevelBracketsNumRanges);
    LevelBracketsHorde.resize(LevelBracketsNumRanges);

    for (uint8 i = 0; i < LevelBracketsNumRanges; ++i)
    {
        std::string idx = std::to_string(i + 1);
        uint32 defaultLower = (i == 0 ? 1 : i * 10);
        uint32 defaultUpper = (i < LevelBracketsNumRanges - 1 ? i * 10 + 9 : RandomBotMaxLevel);
        LevelBracketsAlliance[i].Lower = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Playerbots.LevelBrackets.Alliance.Range" + idx + ".Lower", defaultLower));
        LevelBracketsAlliance[i].Upper = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Playerbots.LevelBrackets.Alliance.Range" + idx + ".Upper", defaultUpper));
        LevelBracketsAlliance[i].Pct = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Playerbots.LevelBrackets.Alliance.Range" + idx + ".Pct", 11));
    }

    for (uint8 i = 0; i < LevelBracketsNumRanges; ++i)
    {
        std::string idx = std::to_string(i + 1);
        uint32 defaultLower = (i == 0 ? 1 : i * 10);
        uint32 defaultUpper = (i < LevelBracketsNumRanges - 1 ? i * 10 + 9 : RandomBotMaxLevel);
        LevelBracketsHorde[i].Lower = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Playerbots.LevelBrackets.Horde.Range" + idx + ".Lower", defaultLower));
        LevelBracketsHorde[i].Upper = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Playerbots.LevelBrackets.Horde.Range" + idx + ".Upper", defaultUpper));
        LevelBracketsHorde[i].Pct = static_cast<uint8>(
            sConfigMgr->GetOption<uint32>("Playerbots.LevelBrackets.Horde.Range" + idx + ".Pct", 11));
    }

    // A mismatch forcibly disables SyncFactions and logs an error; it never brings the server down.
    if (LevelBracketsSyncFactions)
    {
        for (uint8 i = 0; i < LevelBracketsNumRanges; ++i)
        {
            if (LevelBracketsAlliance[i].Lower != LevelBracketsHorde[i].Lower ||
                LevelBracketsAlliance[i].Upper != LevelBracketsHorde[i].Upper)
            {
                LOG_ERROR("server.loading",
                    "[RandomBotLevelMgr] Bracket mismatch detected between factions at index {}. Alliance: {}-{}, "
                    "Horde: {}-{}. SyncFactions requires both bracket count and min/max levels to match exactly; "
                    "forcibly disabling SyncFactions for this session. Check your configuration.",
                    i, LevelBracketsAlliance[i].Lower, LevelBracketsAlliance[i].Upper,
                    LevelBracketsHorde[i].Lower, LevelBracketsHorde[i].Upper);
                LevelBracketsSyncFactions = false;
                break;
            }
        }
    }

    // ---- Level reset ----
    ResetBotLevelEnabled = sConfigMgr->GetOption<bool>("Playerbots.ResetBotLevel.Enabled", false);

    ResetBotLevelMaxLevel =
        static_cast<uint8>(sConfigMgr->GetOption<uint32>("Playerbots.ResetBotLevel.MaxLevel", 80));
    if ((ResetBotLevelMaxLevel < 2 || ResetBotLevelMaxLevel > 80) && ResetBotLevelMaxLevel != 0)
    {
        LOG_ERROR("server.loading",
            "[RandomBotLevelMgr] Invalid Playerbots.ResetBotLevel.MaxLevel value: {}. Using default value 80.",
            ResetBotLevelMaxLevel);
        ResetBotLevelMaxLevel = 80;
    }

    ResetBotLevelResetTo =
        static_cast<uint8>(sConfigMgr->GetOption<uint32>("Playerbots.ResetBotLevel.ResetToLevel", 1));
    if (ResetBotLevelResetTo < 1 || (ResetBotLevelMaxLevel > 0 && ResetBotLevelResetTo >= ResetBotLevelMaxLevel))
    {
        LOG_ERROR("server.loading",
            "[RandomBotLevelMgr] Invalid Playerbots.ResetBotLevel.ResetToLevel value: {}. Using default value 1.",
            ResetBotLevelResetTo);
        ResetBotLevelResetTo = 1;
    }

    ResetBotLevelSkipFrom =
        static_cast<uint8>(sConfigMgr->GetOption<uint32>("Playerbots.ResetBotLevel.SkipFromLevel", 0));
    if (ResetBotLevelSkipFrom > 80 || (ResetBotLevelMaxLevel > 0 && ResetBotLevelSkipFrom >= ResetBotLevelMaxLevel))
    {
        LOG_ERROR("server.loading",
            "[RandomBotLevelMgr] Invalid Playerbots.ResetBotLevel.SkipFromLevel value: {}. Using default value 0 "
            "(disabled).",
            ResetBotLevelSkipFrom);
        ResetBotLevelSkipFrom = 0;
    }

    ResetBotLevelSkipTo = static_cast<uint8>(sConfigMgr->GetOption<uint32>("Playerbots.ResetBotLevel.SkipToLevel", 1));
    if (ResetBotLevelSkipTo < 1 || ResetBotLevelSkipTo > 80 ||
        (ResetBotLevelMaxLevel > 0 && ResetBotLevelSkipTo > ResetBotLevelMaxLevel))
    {
        LOG_ERROR("server.loading",
            "[RandomBotLevelMgr] Invalid Playerbots.ResetBotLevel.SkipToLevel value: {}. Using default value 1.",
            ResetBotLevelSkipTo);
        ResetBotLevelSkipTo = 1;
    }

    ResetBotLevelChance =
        static_cast<uint8>(sConfigMgr->GetOption<uint32>("Playerbots.ResetBotLevel.ResetChance", 100));
    if (ResetBotLevelChance > 100)
    {
        LOG_ERROR("server.loading",
            "[RandomBotLevelMgr] Invalid Playerbots.ResetBotLevel.ResetChance value: {}. Using default value 100.",
            ResetBotLevelChance);
        ResetBotLevelChance = 100;
    }

    ResetBotLevelScaledChance = sConfigMgr->GetOption<bool>("Playerbots.ResetBotLevel.ScaledChance", false);

    ResetBotLevelRestrictTimePlayed =
        sConfigMgr->GetOption<bool>("Playerbots.ResetBotLevel.RestrictTimePlayed", false);
    ResetBotLevelMinTimePlayed = sConfigMgr->GetOption<uint32>("Playerbots.ResetBotLevel.MinTimePlayed", 86400);
    ResetBotLevelPlayedTimeCheckFrequency =
        sConfigMgr->GetOption<uint32>("Playerbots.ResetBotLevel.PlayedTimeCheckFrequency", 864);

    ResetBotLevelIgnoreGuildWithRealPlayers =
        sConfigMgr->GetOption<bool>("Playerbots.ResetBotLevel.IgnoreGuildBotsWithRealPlayers", false);

    ParseLevelMgrExcludeNames(sConfigMgr->GetOption<std::string>("Playerbots.ResetBotLevel.ExcludeNames", ""),
        ResetBotLevelExcludeNames);
}

bool PlayerbotAIConfig::IsInRandomAccountList(uint32 id)
{
    return find(RandomBotAccounts.begin(), RandomBotAccounts.end(), id) != RandomBotAccounts.end();
}

bool PlayerbotAIConfig::IsInRandomQuestItemList(uint32 id)
{
    return find(RandomBotQuestItems.begin(), RandomBotQuestItems.end(), id) != RandomBotQuestItems.end();
}

bool PlayerbotAIConfig::IsPvpProhibited(uint32 zoneId, uint32 areaId)
{
    return IsInPvpProhibitedZone(zoneId) || IsInPvpProhibitedArea(areaId) || IsInPvpProhibitedZone(areaId);
}

bool PlayerbotAIConfig::IsInPvpProhibitedZone(uint32 id)
{
    return find(PvpProhibitedZoneIds.begin(), PvpProhibitedZoneIds.end(), id) != PvpProhibitedZoneIds.end();
}

bool PlayerbotAIConfig::IsInPvpProhibitedArea(uint32 id)
{
    return find(PvpProhibitedAreaIds.begin(), PvpProhibitedAreaIds.end(), id) != PvpProhibitedAreaIds.end();
}

bool PlayerbotAIConfig::IsRestrictedHealerDPSMap(uint32 mapId) const
{
    return RestrictHealerDPS &&
            std::find(RestrictedHealerDPSMaps.begin(), RestrictedHealerDPSMaps.end(), mapId) != RestrictedHealerDPSMaps.end();
}

std::string const PlayerbotAIConfig::GetTimestampStr()
{
    time_t t = time(nullptr);
    tm* aTm = localtime(&t);
    //       YYYY   year
    //       MM     month (2 digits 01-12)
    //       DD     day (2 digits 01-31)
    //       HH     hour (2 digits 00-23)
    //       MM     minutes (2 digits 00-59)
    //       SS     seconds (2 digits 00-59)
    // Sized for the widest output snprintf can produce for these int conversions, so the
    // result is never truncated.
    char buf[128];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d-%02d-%02d", aTm->tm_year + 1900, aTm->tm_mon + 1, aTm->tm_mday, aTm->tm_hour,
             aTm->tm_min, aTm->tm_sec);
    return std::string(buf);
}

bool PlayerbotAIConfig::OpenLog(std::string const fileName, char const* mode)
{
    if (!HasLog(fileName))
        return false;

    auto logFileIt = LogFiles.find(fileName);
    if (logFileIt == LogFiles.end())
    {
        LogFiles.insert(std::make_pair(fileName, std::make_pair(nullptr, false)));
        logFileIt = LogFiles.find(fileName);
    }

    FILE* file = logFileIt->second.first;
    bool fileOpen = logFileIt->second.second;

    if (fileOpen)  // close log file
        fclose(file);

    std::string m_logsDir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!m_logsDir.empty())
    {
        if ((m_logsDir.at(m_logsDir.length() - 1) != '/') && (m_logsDir.at(m_logsDir.length() - 1) != '\\'))
            m_logsDir.append("/");
    }

    file = fopen((m_logsDir + fileName).c_str(), mode);
    fileOpen = true;

    logFileIt->second.first = file;
    logFileIt->second.second = fileOpen;

    return true;
}

void PlayerbotAIConfig::Log(std::string const fileName, char const* str, ...)
{
    if (!str)
        return;

    std::lock_guard<std::mutex> guard(LogMutex);

    if (!IsLogOpen(fileName) && !OpenLog(fileName, "a"))
        return;

    FILE* file = LogFiles.find(fileName)->second.first;

    va_list ap;
    va_start(ap, str);
    vfprintf(file, str, ap);
    fprintf(file, "\n");
    va_end(ap);
    fflush(file);

    fflush(stdout);
}

void PlayerbotAIConfig::LoadWorldBuff()
{
    std::string matrix = sConfigMgr->GetOption<std::string>("Playerbots.WorldBuffMatrix", "", true);
    if (matrix.empty())
        return;

    std::istringstream entryStream(matrix);
    std::string entry;

    while (std::getline(entryStream, entry, ';'))
    {

        entry.erase(0, entry.find_first_not_of(" \t\r\n"));
        entry.erase(entry.find_last_not_of(" \t\r\n") + 1);

        size_t firstColon = entry.find(':');
        size_t secondColon = entry.find(':', firstColon + 1);

        if (firstColon == std::string::npos || secondColon == std::string::npos)
        {
            LOG_ERROR("playerbots", "Malformed entry: [{}]", entry);
            continue;
        }

        std::string metaPart = entry.substr(firstColon + 1, secondColon - firstColon - 1);
        std::string spellPart = entry.substr(secondColon + 1);

        std::vector<uint32> ids;
        std::istringstream metaStream(metaPart);
        std::string token;
        while (std::getline(metaStream, token, ','))
        {
            try {
                ids.push_back(static_cast<uint32>(std::stoi(token)));
            } catch (...) {
                LOG_ERROR("playerbots", "Invalid meta token in [{}]", entry);
                break;
            }
        }

        if (ids.size() != 5)
        {
            LOG_ERROR("playerbots", "Entry [{}] has incomplete meta block", entry);
            continue;
        }

        std::istringstream spellStream(spellPart);
        while (std::getline(spellStream, token, ','))
        {
            try {
                uint32 spellId = static_cast<uint32>(std::stoi(token));
                WorldBuff wb = { spellId, ids[0], ids[1], ids[2], ids[3], ids[4] };
                WorldBuffs.push_back(wb);
            } catch (...) {
                LOG_ERROR("playerbots", "Invalid spell ID in [{}]", entry);
            }
        }
    }
}

static std::vector<std::string> Split(std::string const& str, std::string const& pattern)
{
    std::vector<std::string> res;
    if (str == "")
        return res;
    // Also add separators to string connections to facilitate intercepting the last paragraph.
    std::string strs = str + pattern;
    size_t pos = strs.find(pattern);

    while (pos != strs.npos)
    {
        std::string temp = strs.substr(0, pos);
        res.push_back(temp);
        // Remove the split string and split the remaining string
        strs = strs.substr(pos + 1, strs.size());
        pos = strs.find(pattern);
    }

    return res;
}

std::vector<std::vector<uint32>> PlayerbotAIConfig::ParseTempTalentsOrder(uint32 cls, std::string tab_link)
{
    // check bad link
    uint32 classMask = 1 << (cls - 1);
    std::vector<std::vector<uint32>> res;
    std::vector<std::string> tab_links = Split(tab_link, "-");
    std::map<uint32, std::vector<TalentEntry const*>> spells;
    std::vector<std::vector<std::vector<uint32>>> orders(3);
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
        if (!talentInfo)
            continue;

        TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
        if (!talentTabInfo)
            continue;

        if ((classMask & talentTabInfo->ClassMask) == 0)
            continue;

        spells[talentTabInfo->tabpage].push_back(talentInfo);
    }
    for (int tab = 0; tab < 3; tab++)
    {
        if (tab_links.size() <= (size_t)tab)
        {
            break;
        }
        std::sort(spells[tab].begin(), spells[tab].end(),
                  [&](TalentEntry const* lhs, TalentEntry const* rhs)
                  { return lhs->Row != rhs->Row ? lhs->Row < rhs->Row : lhs->Col < rhs->Col; });
        for (uint32 i = 0; i < tab_links[tab].size(); i++)
        {
            if (i >= spells[tab].size())
            {
                break;
            }
            int lvl = tab_links[tab][i] - '0';
            if (lvl == 0)
                continue;
            orders[tab].push_back({(uint32)tab, spells[tab][i]->Row, spells[tab][i]->Col, (uint32)lvl});
        }
    }
    // sort by talent tab size
    std::sort(orders.begin(), orders.end(), [&](auto& lhs, auto& rhs) { return lhs.size() > rhs.size(); });
    for (auto& order : orders)
    {
        res.insert(res.end(), order.begin(), order.end());
    }
    return res;
}

std::vector<std::vector<uint32>> PlayerbotAIConfig::ParseTempPetTalentsOrder(uint32 spec, std::string tab_link)
{
    // check bad link
    // uint32 classMask = 1 << (cls - 1);
    std::vector<TalentEntry const*> spells;
    std::vector<std::vector<uint32>> orders;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
        if (!talentInfo)
            continue;

        TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
        if (!talentTabInfo)
            continue;

        if (!((1 << spec) & talentTabInfo->petTalentMask))
            continue;
        // skip some duplicate spells like dash/dive
        if (talentInfo->TalentID == 2201 || talentInfo->TalentID == 2208 || talentInfo->TalentID == 2219 ||
            talentInfo->TalentID == 2203)
            continue;

        spells.push_back(talentInfo);
    }
    std::sort(spells.begin(), spells.end(),
              [&](TalentEntry const* lhs, TalentEntry const* rhs)
              { return lhs->Row != rhs->Row ? lhs->Row < rhs->Row : lhs->Col < rhs->Col; });
    for (uint32 i = 0; i < tab_link.size(); i++)
    {
        if (i >= spells.size())
        {
            break;
        }
        int lvl = tab_link[i] - '0';
        if (lvl == 0)
            continue;
        orders.push_back({spells[i]->Row, spells[i]->Col, (uint32)lvl});
    }
    // sort by talent tab size
    std::sort(orders.begin(), orders.end(), [&](auto& lhs, auto& rhs) { return lhs.size() > rhs.size(); });

    return orders;
}
