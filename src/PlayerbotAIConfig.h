/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTAICONFIG_H
#define PLAYERBOTS_PLAYERBOTAICONFIG_H

#include "BotAttributes.h"
#include "DBCEnums.h"
#include "NaturalReaction.h"
#include "SharedDefines.h"
#include "TacticGate.h"
#include "Tactics.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

enum class BotCheatMask : uint32
{
    none = 0,
    taxi = 1,
    gold = 2,
    health = 4,
    mana = 8,
    power = 16,
    raid = 32,
    food = 64,
    maxMask = 128
};

enum class HealingManaEfficiency : uint8
{
    VERY_LOW = 1,
    LOW = 2,
    MEDIUM = 4,
    HIGH = 8,
    VERY_HIGH = 16,
    SUPERIOR = 32
};

enum class ShowHideCosmetic : uint8
{
    ALWAYS_HIDE = 0,
    ALWAYS_SHOW = 1,
    RANDOMIZE = 2
};

enum class AutoPartyBuffMode : uint8
{
    DISABLED = 0,
    RAID_ONLY = 1,
    GROUP_OR_RAID = 2
};

enum ArenaRatingMode : uint32
{
    ARENA_RATING_MODE_RANDOM = 0,
    ARENA_RATING_MODE_EARNED = 1,
};

// Playerbots.Ladder.Role
enum LadderRole : uint32
{
    LADDER_ROLE_OFF = 0,
    LADDER_ROLE_MAIN = 1,
    LADDER_ROLE_ARENA = 2,
};

enum NewRpgStatus : int
{
    //Initial Status
    RPG_IDLE = 0,
    RPG_GO_GRIND = 1,
    RPG_GO_CAMP = 2,
    // Exploring nearby
    RPG_WANDER_RANDOM = 3,
    RPG_WANDER_NPC = 4,
    // Do Quest (based on quest status)
    RPG_DO_QUEST = 5,
    // Travel

    RPG_TRAVEL_FLIGHT = 6,
    // Taking a break
    RPG_REST = 7,
    RPG_OUTDOOR_PVP = 8,
    // Roam the current zone harvesting herb/ore nodes
    RPG_DO_GATHER = 9,
    RPG_STATUS_END = 10
};

#define MAX_SPECNO 20

// One level range/bucket used by the random bot level brackets sub-feature (see RandomBotLevelMgr).
struct LevelBracketConfig
{
    uint8 Lower = 1;
    uint8 Upper = 80;
    uint8 Pct = 0;
};

class PlayerbotAIConfig
{
public:
    static PlayerbotAIConfig& Instance()
    {
        static PlayerbotAIConfig instance;

        return instance;
    }

    bool Initialize();
    void LoadRandomBotLevelConfig();
    bool IsInRandomAccountList(uint32 id);
    bool IsInRandomQuestItemList(uint32 id);
    bool IsPvpProhibited(uint32 zoneId, uint32 areaId);
    bool IsInPvpProhibitedZone(uint32 id);
    bool IsInPvpProhibitedArea(uint32 id);

    bool Enabled;
    bool DisabledWithoutRealPlayer;
    bool EnableICCBuffs;
    bool AllowAccountBots, AllowGuildBots, AllowTrustedAccountBots;
    bool RandomBotGuildNearby, RandomBotInvitePlayer, InviteChat;
    uint32 GlobalCoolDown, ReactDelay, MaxWaitForMove, DisableMoveSplinePath, MaxMovementSearchTime, ExpireActionTime,
        DispelAuraDuration, PassiveDelay, RepeatDelay, ErrorDelay, RpgDelay, SitDelay, ReturnDelay, LootDelay;
    bool DynamicReactDelay;
    float SightDistance, SpellDistance, ReactDistance, GrindDistance, LootDistance, ShootDistance, FleeDistance,
        TooCloseDistance, MeleeDistance, FollowDistance, WhisperDistance, ContactDistance, AoeRadius, RpgDistance,
        TargetPosRecalcDistance, FarDistance, HealDistance, AggroDistance;
    // Each bot's own sight range on world maps (core Player::SetSightRangeCap), set at login; 0 = off
    float BotSightRangeCap;
    uint32 CriticalHealth, LowHealth, MediumHealth, AlmostFullHealth;
    uint32 LowMana, MediumMana, HighMana;
    bool AutoSaveMana;
    uint32 SaveManaThreshold;
    AutoPartyBuffMode AutoGreaterBlessings;
    AutoPartyBuffMode AutoPartyBuffs;
    bool TellWhenMissingBuffReagents;
    uint32 MissingBuffReagentMessageCooldown;
    bool ForceRebuffOnReadyCheck;
    uint32 ForceRebuffMarginSecs;
    bool AutoAvoidAoe;
    float MaxAoeAvoidRadius;
    std::set<uint32> AoeAvoidSpellWhitelist;
    bool TellWhenAvoidAoe;
    std::set<uint32> DisallowedGameObjects;
    std::set<uint32> AttunementQuests;
    std::set<uint32> UnobtainableItems;

    uint32 OpenGoSpell;
    bool RandomBotAutologin;
    bool BotAutologin;
    std::string RandomBotMapsAsString;
    float ProbTeleToBankers;
    bool EnableWeightTeleToCityBankers;
    int WeightTeleToStormwind;
    int WeightTeleToIronforge;
    int WeightTeleToDarnassus;
    int WeightTeleToExodar;
    int WeightTeleToOrgrimmar;
    int WeightTeleToUndercity;
    int WeightTeleToThunderBluff;
    int WeightTeleToSilvermoonCity;
    int WeightTeleToShattrathCity;
    int WeightTeleToDalaran;
    std::vector<uint32> RandomBotMaps;
    std::vector<uint32> RandomBotQuestItems;
    std::vector<uint32> RandomBotAccounts;
    std::vector<uint32> RandomBotSpellIds;
    std::vector<uint32> RandomBotQuestIds;
    uint32 RandomBotTeleportDistance;
    float RandomGearLoweringChance;
    int32 RandomGearQualityLimit;
    int32 RandomGearScoreLimit;
    bool PreferClassArmorType;
    bool PreferredSpecWeapons;
    float RandomBotMinLevelChance, RandomBotMaxLevelChance;
    float RandomBotRpgChance;
    uint32 MinRandomBots, MaxRandomBots;
    uint32 RandomBotUpdateInterval, RandomBotCountChangeMinInterval, RandomBotCountChangeMaxInterval;
    uint32 MinRandomBotInWorldTime, MaxRandomBotInWorldTime;
    uint32 MinRandomBotRandomizeTime, MaxRandomBotRandomizeTime;
    uint32 MinRandomBotChangeStrategyTime, MaxRandomBotChangeStrategyTime;
    uint32 MinRandomBotReviveTime, MaxRandomBotReviveTime;
    uint32 MinRandomBotTeleportInterval, MaxRandomBotTeleportInterval;
    uint32 PermanentlyInWorldTime;
    uint32 MinRandomBotPvpTime, MaxRandomBotPvpTime;
    uint32 RandomBotsPerInterval;
    uint32 RandomBotPrintStatsInterval;
    uint32 MinRandomBotsPriceChangeInterval, MaxRandomBotsPriceChangeInterval;
    uint32 DisabledWithoutRealPlayerLoginDelay, DisabledWithoutRealPlayerLogoutDelay;
    bool RandomBotJoinLfg;
    bool RandomBotJoinLfgOutleveledExpansion;

    // Professions
    bool EnableFishingWithMaster;
    uint32 ClassMatchingProfessionChance;
    float FishingDistanceFromMaster, FishingDistance, EndFishingWithMaster;

    // chat
    bool RandomBotTalk;
    bool RandomBotEmote;
    bool RandomBotSuggestDungeons;
    bool EnableBroadcasts;
    bool EnableGreet;
    bool RandomBotSayWithoutMaster;
    bool AnnounceConsumableUse;

    uint32 BroadcastChanceMaxValue;

    uint32 BroadcastToGuildGlobalChance;
    uint32 BroadcastToWorldGlobalChance;
    uint32 BroadcastToGeneralGlobalChance;
    uint32 BroadcastToTradeGlobalChance;
    uint32 BroadcastToLFGGlobalChance;
    uint32 BroadcastToLocalDefenseGlobalChance;
    uint32 BroadcastToWorldDefenseGlobalChance;
    uint32 BroadcastToGuildRecruitmentGlobalChance;

    uint32 BroadcastChanceLootingItemPoor;
    uint32 BroadcastChanceLootingItemNormal;
    uint32 BroadcastChanceLootingItemUncommon;
    uint32 BroadcastChanceLootingItemRare;
    uint32 BroadcastChanceLootingItemEpic;
    uint32 BroadcastChanceLootingItemLegendary;
    uint32 BroadcastChanceLootingItemArtifact;

    uint32 BroadcastChanceQuestAccepted;
    uint32 BroadcastChanceQuestUpdateObjectiveCompleted;
    uint32 BroadcastChanceQuestUpdateObjectiveProgress;
    uint32 BroadcastChanceQuestUpdateFailedTimer;
    uint32 BroadcastChanceQuestUpdateComplete;
    uint32 BroadcastChanceQuestTurnedIn;

    uint32 BroadcastChanceKillNormal;
    uint32 BroadcastChanceKillElite;
    uint32 BroadcastChanceKillRareelite;
    uint32 BroadcastChanceKillWorldboss;
    uint32 BroadcastChanceKillRare;
    uint32 BroadcastChanceKillUnknown;
    uint32 BroadcastChanceKillPet;
    uint32 BroadcastChanceKillPlayer;

    uint32 BroadcastChanceLevelupGeneric;
    uint32 BroadcastChanceLevelupTenX;
    uint32 BroadcastChanceLevelupMaxLevel;

    uint32 BroadcastChanceSuggestInstance;
    uint32 BroadcastChanceSuggestQuest;
    uint32 BroadcastChanceSuggestGrindMaterials;
    uint32 BroadcastChanceSuggestGrindReputation;
    uint32 BroadcastChanceSuggestSell;
    uint32 BroadcastChanceSuggestSomething;

    uint32 BroadcastChanceSuggestSomethingToxic;

    uint32 BroadcastChanceSuggestToxicLinks;
    std::string ToxicLinksPrefix;
    uint32 ToxicLinksRepliesChance;

    uint32 BroadcastChanceSuggestThunderfury;
    uint32 ThunderfuryRepliesChance;

    uint32 BroadcastChanceGuildManagement;

    uint32 GuildRepliesRate;

    bool RandomBotJoinBG;
    bool RandomBotAutoJoinBG;

    std::string RandomBotAutoJoinICBrackets;
    std::string RandomBotAutoJoinEYBrackets;
    std::string RandomBotAutoJoinAVBrackets;
    std::string RandomBotAutoJoinABBrackets;
    std::string RandomBotAutoJoinWSBrackets;

    uint32 RandomBotAutoJoinBGICCount;
    uint32 RandomBotAutoJoinBGEYCount;
    uint32 RandomBotAutoJoinBGAVCount;
    uint32 RandomBotAutoJoinBGABCount;
    uint32 RandomBotAutoJoinBGWSCount;

    uint32 RandomBotAutoJoinArenaBracket;

    uint32 RandomBotAutoJoinBGRatedArena2v2Count;
    uint32 RandomBotAutoJoinBGRatedArena3v3Count;
    uint32 RandomBotAutoJoinBGRatedArena5v5Count;
    uint32 RandomBotAutoJoinRatedArenaQueueSlots;
    // Playerbots.RandomBotAutoJoinRatedArenaUncapped: every free bot team queues, no slot or count limit
    bool RandomBotAutoJoinRatedArenaUncapped;
    bool RandomBotArenaGroupOnWorldThread;

    uint32 RandomBotTeleLowerLevel, RandomBotTeleHigherLevel;
    std::map<uint32, std::pair<uint32, uint32>> ZoneBrackets;
    bool LogInGroupOnly, LogValuesPerTick;
    bool SummonAtInnkeepersEnabled;
    std::string CombatStrategies, NonCombatStrategies;
    std::string RandomBotCombatStrategies, RandomBotNonCombatStrategies;
    std::string ReactStrategies, RandomBotReactStrategies;
    bool ApplyInstanceStrategies;
    uint32 RandomBotMinLevel, RandomBotMaxLevel;
    float RandomChangeMultiplier;

    // std::string premadeLevelSpec[MAX_CLASSES][10][91]; //lvl 10 - 100
    // ClassSpecs classSpecs[MAX_CLASSES];

    std::string PremadeSpecName[MAX_CLASSES][MAX_SPECNO];
    std::string PremadeSpecGlyph[MAX_CLASSES][MAX_SPECNO];
    std::vector<uint32> ParsedSpecGlyph[MAX_CLASSES][MAX_SPECNO];
    std::string PremadeSpecLink[MAX_CLASSES][MAX_SPECNO][MAX_LEVEL];
    std::string PremadeHunterPetLink[3][21];
    std::vector<std::vector<uint32>> ParsedSpecLinkOrder[MAX_CLASSES][MAX_SPECNO][MAX_LEVEL];
    std::vector<std::vector<uint32>> ParsedHunterPetLinkOrder[3][21];
    uint32 RandomClassSpecProb[MAX_CLASSES][MAX_SPECNO];
    uint32 RandomClassSpecIndex[MAX_CLASSES][MAX_SPECNO];

    std::string CommandPrefix, CommandSeparator;
    std::string RandomBotAccountPrefix;
    uint32 RandomBotAccountCount;
    bool RandomBotRandomPassword;
    bool DeleteRandomBotAccounts;
    uint32 RandomBotGuildCount, RandomBotGuildSizeMax;
    bool DeleteRandomBotGuilds;
    std::vector<uint32> PvpProhibitedZoneIds;
    std::vector<uint32> PvpProhibitedAreaIds;
    bool FastReactInBG;

    bool RandomBotsWalkingRPG;
    bool RandomBotsWalkingRPGInDoors;
    uint32 MinEnchantingBotLevel;
    uint32 LimitEnchantExpansion;
    uint32 LimitGearExpansion;
    uint32 RandomBotStartingLevel;
    bool EnablePeriodicOnlineOffline;
    float PeriodicOnlineOfflineRatio;
    bool GearScoreCheck;
    bool RandomBotPreQuests;
    bool BotSendMailEnabled;

    bool GuildTaskEnabled;
    uint32 MinGuildTaskChangeTime, MaxGuildTaskChangeTime;
    uint32 MinGuildTaskAdvertisementTime, MaxGuildTaskAdvertisementTime;
    uint32 MinGuildTaskRewardTime, MaxGuildTaskRewardTime;
    uint32 GuildTaskAdvertCleanupTime;
    uint32 GuildTaskKillTaskDistance;

    uint32 IterationsPerTick;

    std::mutex LogMutex;
    bool EnableAutoTradeOnItemMention;
    std::vector<std::string> TradeActionExcludedPrefixes;
    std::vector<std::string> AllowedLogFiles;
    std::unordered_map<std::string, std::pair<FILE*, bool>> LogFiles;

    std::vector<std::string> BotCheats;
    uint32 BotCheatMask = 0;

    struct WorldBuff
    {
        uint32 SpellId;
        uint32 FactionId;
        uint32 ClassId;
        uint32 SpecId;
        uint32 MinLevel;
        uint32 MaxLevel;
    };

    std::vector<WorldBuff> WorldBuffs;

    uint32 CommandServerPort;
    bool PerfMonEnabled;
    bool PerfMonWorldThread;
    bool DebugThreadChecks;
    bool SkipIdleTriggerChecks;
    bool SummonWhenGroup;
    ShowHideCosmetic RandomBotShowHelmet;
    ShowHideCosmetic RandomBotShowCloak;
    bool RandomBotFixedLevel;
    bool DisableRandomLevels;
    float RandomBotXPRate;
    uint32 RandomBotAllianceRatio;
    uint32 RandomBotHordeRatio;
    bool DisableDeathKnightLogin;
    bool LimitTalentsExpansion;
    uint32 BotActiveAlone;
    uint32 BotActiveAloneDurationSeconds;
    uint32 BotActiveAloneForceWhenInRadius;
    bool BotActiveAloneForceWhenInZone;
    bool BotActiveAloneForceWhenInMap;
    bool BotActiveAloneForceWhenIsFriend;
    bool BotActiveAloneForceWhenInGuild;
    bool TestPlayerEnable;
    std::string TestPlayerAccount;
    bool PvpTestEnable;
    uint32 PvpTestTimeoutSeconds;
    // Where pvptest duel and brawl fight: map id and position
    uint32 PvpTestFightMap;
    float PvpTestFightX;
    float PvpTestFightY;
    float PvpTestFightZ;
    float PvpTestFightO;
    bool CensusEnable;
    uint32 CensusIntervalSeconds;
    bool AcquaintanceEnable;
    uint32 AcquaintanceFlushSeconds;
    uint32 AcquaintanceDirectCooldownSeconds;
    uint32 AcquaintanceGroupJoinCooldownSeconds;
    uint32 AcquaintancePointsDirect;
    uint32 AcquaintancePointsGroupJoin;
    uint32 AcquaintancePointsGuildJoin;
    uint32 AcquaintancePointsPvpKill;
    uint32 AcquaintancePointsPerGroupMinute;
    uint32 AcquaintancePointsPerGuildHour;
    uint32 AcquaintancePointsPerNearbyMinute;
    uint32 AcquaintanceMaxScore;
    uint32 AcquaintanceGroupSampleSeconds;
    uint32 AcquaintanceGuildSampleSeconds;
    uint32 AcquaintanceNearbyIntervalSeconds;
    float AcquaintanceNearbyRadius;
    bool LadderEnable;
    uint32 LadderRole;
    std::string LadderServerId;
    bool MatchLogEnable;
    uint32 MatchLogTraceEvery;
    uint32 MatchLogTraceMaxEvents;
    uint32 MatchLogTraceMaxBytes;
    bool NaturalTraceEnable;
    uint32 NaturalTraceMaxUnits;
    uint32 NaturalTraceSampleEvery;
    bool NaturalTraceOrganic;
    std::string NaturalTraceDir;
    uint32 NaturalTraceIntervalMs;
    uint32 NaturalTraceMaxRowsPerUnit;
    // Playerbots.Natural.Trace.Format (G0i): false = jsonl (one file per day), true = binary (one file per match)
    bool NaturalTraceBinary;
    uint32 LadderDepartTimeoutSeconds;
    uint32 LadderContactMinutes;
    uint32 LadderPoolTeams;
    bool LadderMatchmakerEnable;
    uint32 LadderMatchmakerDelayMinSeconds;
    uint32 LadderMatchmakerDelayMaxSeconds;
    uint32 LadderMatchmakerMmrWidth;
    uint32 LadderMatchmakerWindow;
    uint32 LadderMatchmakerWindowGrowth;
    uint32 LadderMatchmakerRecentOpponents;
    uint32 LadderMatchmakerTimeoutSeconds;
    uint32 LadderMatchmakerMaxPicks;
    bool LadderMatchmakerSkirmish;
    bool LadderMatchmakerPoolOnly;
    bool PvpGearEnable;
    std::string PvpGearTierOrder;
    std::string PvpGearBaseTier;
    bool PvpGearWear;
    bool PvpGearWearBetweenMatches;
    bool PvpGearBuyEnable;
    std::string PvpGearBuySlotPriority;
    bool PvpGearBuySaveTowardBetter;
    uint32 PvpGearBuyCheckIntervalMinutes;
    uint32 PvpGearBuyBatchPerTick;
    bool SkillEnable;
    float SkillReactFactorLow;
    float SkillReactFactorHigh;
    bool SkillFullAbilityInPvE;
    bool SkillReactActive;
    uint32 SkillNoticeMissPercentLow;
    uint32 SkillNoticeMissPercentHigh;
    uint32 SkillNoticeDelayMsLow;
    uint32 SkillNoticeDelayMsHigh;
    uint32 SkillNoticeWindowMs;
    bool SkillNoticeActive;
    // Playerbots.Skill.Distribution and its parameters: a bot's fixed percentile maps to its skill through it
    bool SkillDistributionUniform;
    float SkillMean;
    float SkillSd;
    float SkillMin;
    float SkillMax;
    // Playerbots.Skill.ExemptRealMasterBots: bots with a game-client master play at full ability (1, phase D) or keep
    // their skill (0, DT.2)
    bool SkillExemptRealMasterBots;
    // Playerbots.Skill.CurveExponent, KnowledgeSpread, Effort*, StyleSpread: the tactic gate's skill parameters
    TacticGateParams SkillGate;
    // Playerbots.Skill.Attributes.*: loadings, spreads and the correlation matrix (factorised at load) of the attribute
    // model (BotAttributes.h); the defaults make every ability the skill and every trait neutral
    BotAttributeParams SkillAttributes;
    // Playerbots.Natural.Reaction.*: human reaction times in the skill multiplier (NaturalReaction.h). Active needs
    // Playerbots.Skill.Enable; Contexts is a tactic scope mask (default the PvP contexts)
    bool NaturalReactionEnable;
    bool NaturalReactionActive;
    uint8 NaturalReactionContexts;
    ReactionParams NaturalReaction;
    // Playerbots.Natural.Cadence.*: a faster AI check and a smaller stagger for bots in its Contexts, in combat in a
    // started match (NaturalReaction.h). NearRealPlayerYards is read but inert: no cached proximity flag exists yet.
    bool NaturalCadenceEnable;
    uint32 NaturalCadenceCheckMs;
    uint32 NaturalCadenceStaggerMs;
    uint8 NaturalCadenceContexts;
    float NaturalCadenceNearRealPlayerYards;
    // Playerbots.ArenaOnly.*: random bots outside an arena only wait for their next match, spread over 4 continents
    bool ArenaOnlyEnable;
    bool ArenaOnlySpreadContinents;
    // Playerbots.RandomBotTestNamePrefix: new random bots are named prefix + a letters-only counter (empty = pool)
    std::string RandomBotTestNamePrefix;
    // Playerbots.Tactics.*: the system switch, each tactic's Enable and Scope (indexed by TacticId), and the mask of
    // the tactics enabled in config (0 while the system is off)
    bool TacticsEnable;
    std::array<TacticSettings, TACTIC_COUNT> TacticConfig;
    uint32 TacticsConfigMask;
    bool BotActiveAloneSmartScale;
    uint32 BotActiveAloneSmartScaleDiffLimitFloor;
    uint32 BotActiveAloneSmartScaleDiffLimitCeiling;
    uint32 BotActiveAloneSmartScaleWhenMinLevel;
    uint32 BotActiveAloneSmartScaleWhenMaxLevel;
    uint32 BotActiveAloneSmartScalePercentile;
    // Situation budgets (ActivityBudget): per-situation smart scale floor and ceiling, indexed by BotSituation
    // (open world, dungeon, boss, PvP); 0 = the global value. SituationForce: -1 = detect, else a BotSituation.
    bool SituationEnable;
    uint32 SituationIntervalMs;
    int32 SituationForce;
    uint32 SituationDiffLimitFloor[4];
    uint32 SituationDiffLimitCeiling[4];

    bool FreeMethodLoot;
    int32 LootNeedRollLevel;
    bool LootGreedRollLevel;
    bool LootRollRecipe;
    bool LootRollDisenchant;
    std::string AutoPickReward;
    bool AutoEquipUpgradeLoot;
    float EquipUpgradeThreshold;
    bool TwoRoundsGearInit;
    bool SyncQuestWithPlayer;
    bool SyncQuestForPlayer;
    bool DropObsoleteQuests;
    bool AllowLearnTrainerSpells;
    bool AutoPickTalents;
    bool AutoUpgradeEquip;
    int32 HunterWolfPet;
    int32 DefaultPetStance;
    int32 PetChatCommandDebug;
    bool AutoLearnTrainerSpells;
    bool AutoDoQuests;
    bool EnableNewRpgStrategy;
    std::unordered_map<NewRpgStatus, uint32> RpgStatusProbWeight;
    bool SyncLevelWithPlayers;
    bool RandomBotConcentrateInPlayerZone;
    bool AutoLearnQuestSpells;
    bool AutoTeleportForLevel;
    bool RandomBotGroupNearby;
    int32 EnableRandomBotTrading;
    uint32 TweakValue;  // Debugging config

    uint32 RandomBotArenaTeamMaxRating;
    uint32 RandomBotArenaTeamMinRating;
    uint32 RandomBotArenaTeamRatingMode;
    uint32 RandomBotArenaTeam2v2Count;
    uint32 RandomBotArenaTeam3v3Count;
    uint32 RandomBotArenaTeam5v5Count;
    bool DeleteRandomBotArenaTeams;
    // Playerbots.ArenaTeam.Seed.*: a new bot team's rating and MMR from its members' skill percentiles plus noise
    bool ArenaTeamSeedEnable;
    float ArenaTeamSeedSpread;
    float ArenaTeamSeedNoiseSd;
    // Playerbots.ArenaTeam.*: team formation by partner score (earned rating mode only)
    bool ArenaTeamFormationEnable;
    uint32 ArenaTeamUpdateSeconds;
    uint32 ArenaTeamMaxRoster2v2;
    uint32 ArenaTeamMaxRoster3v3;
    uint32 ArenaTeamMaxRoster5v5;
    uint32 ArenaTeamMinMembers;
    bool ArenaTeamRandomPair;
    bool ArenaTeamRepickEachQueue;
    uint32 ArenaTeamLossStreakToConsider;
    uint32 ArenaTeamLeaveChanceBottom;
    uint32 ArenaTeamLeaveChanceTop;
    uint32 ArenaTeamMinTeamStaySeconds;
    uint32 ArenaTeamToleranceBottom;
    uint32 ArenaTeamToleranceTop;
    float ArenaTeamSynergyWeight;
    uint32 ArenaTeamRecruitsPerPass;
    uint32 ArenaTeamRecruitAboveMinChance;
    // Playerbots.ArenaRecruit.*: the hidden rule for a real player's arena team invite to a randombot
    bool ArenaRecruitEnable;
    uint32 ArenaRecruitRatingAtSkill1;
    uint32 ArenaRecruitRatingAtSkill99;
    uint32 ArenaRecruitDeltaBase;
    float ArenaRecruitDeltaPerAcquaintancePoint;
    uint32 ArenaRecruitDeltaMax;
    uint32 ArenaRecruitLoyaltyMinGames;
    uint32 ArenaRecruitLoyaltyMinWinPercent;

    uint32 SelfBotLevel;
    bool DowngradeMaxLevelBot;
    bool EquipAndSpecPersistence;
    int32 EquipAndSpecPersistenceLevel;
    int32 GroupInvitationPermission;
    bool AllowBattlegroundCommands;
    bool KeepAltsInGroup = false;
    bool AllowSummonInCombat;
    bool AllowSummonWhenMasterIsDead;
    bool AllowSummonWhenBotIsDead;
    int ReviveBotWhenSummoned;
    bool BotRepairWhenSummon;
    bool AutoInitOnly;
    bool ResetInstanceIdForAltBots;
    float AutoInitEquipLevelLimitRatio;
    int32 MaxAddedBots;
    int32 AddClassCommand;
    int32 AddClassAccountPoolSize;
    bool AddClassRandomCharacter;
    int32 MaintenanceCommand;
    bool AltMaintenanceAttunementQs,
            AltMaintenanceBags,
            AltMaintenanceAmmo,
            AltMaintenanceFood,
            AltMaintenanceReagents,
            AltMaintenanceConsumables,
            AltMaintenancePotions,
            AltMaintenanceTalentTree,
            AltMaintenancePet,
            AltMaintenancePetTalents,
            AltMaintenanceClassSpells,
            AltMaintenanceAvailableSpells,
            AltMaintenanceSkills,
            AltMaintenanceReputation,
            AltMaintenanceSpecialSpells,
            AltMaintenanceMounts,
            AltMaintenanceGlyphs,
            AltMaintenanceKeyring,
            AltMaintenanceGemsEnchants;
    int32 AutoGearCommand, AutoGearCommandAltBots, AutoGearQualityLimit, AutoGearScoreLimit;
    int32 AutoGearBisCommand;

    uint32 UseGroundMountAtMinLevel;
    uint32 UseFastGroundMountAtMinLevel;
    uint32 UseFlyMountAtMinLevel;
    uint32 UseFastFlyMountAtMinLevel;

    // stagger flightpath takeoff
    uint32 BotTaxiDelayMin;
    uint32 BotTaxiDelayMax;
    uint32 BotTaxiGapMs;
    uint32 BotTaxiGapJitterMs;

    std::string const GetTimestampStr();
    bool HasLog(std::string const fileName)
    {
        return std::find(AllowedLogFiles.begin(), AllowedLogFiles.end(), fileName) != AllowedLogFiles.end();
    };
    bool OpenLog(std::string const fileName, char const* mode = "a");
    bool IsLogOpen(std::string const fileName)
    {
        auto it = LogFiles.find(fileName);
        return it != LogFiles.end() && it->second.second;
    }
    void Log(std::string const fileName, char const* str, ...);

    void LoadWorldBuff();

    static std::vector<std::vector<uint32>> ParseTempTalentsOrder(uint32 cls, std::string temp_talents_order);
    static std::vector<std::vector<uint32>> ParseTempPetTalentsOrder(uint32 spec, std::string temp_talents_order);

    bool RestrictHealerDPS = false;
    std::vector<uint32> RestrictedHealerDPSMaps;
    bool IsRestrictedHealerDPSMap(uint32 mapId) const;

    std::vector<uint32> ExcludedHunterPetFamilies;

    // Random bot level brackets (periodic redistribution across per-faction level ranges). See
    // RandomBotLevelMgr; percentages here are the as-configured values, not the runtime working copy.
    bool LevelBracketsEnabled;
    uint32 LevelBracketsCheckFrequency;
    uint32 LevelBracketsFlaggedCheckFrequency;
    uint32 LevelBracketsFlaggedProcessLimit;
    bool LevelBracketsIgnoreGuildWithRealPlayers;
    bool LevelBracketsIgnoreArenaTeamBots;
    bool LevelBracketsIgnoreFriendListed;
    std::vector<std::string> LevelBracketsExcludeNames;
    uint8 LevelBracketsNumRanges;
    std::vector<LevelBracketConfig> LevelBracketsAlliance;
    std::vector<LevelBracketConfig> LevelBracketsHorde;
    bool LevelBracketsDynamicDistribution;
    float LevelBracketsRealPlayerWeight;
    bool LevelBracketsSyncFactions;

    // Random bot level reset (reset random bots reaching max level). See RandomBotLevelMgr.
    bool ResetBotLevelEnabled;
    uint8 ResetBotLevelMaxLevel;
    uint8 ResetBotLevelResetTo;
    uint8 ResetBotLevelSkipFrom;
    uint8 ResetBotLevelSkipTo;
    uint8 ResetBotLevelChance;
    bool ResetBotLevelScaledChance;
    bool ResetBotLevelRestrictTimePlayed;
    uint32 ResetBotLevelMinTimePlayed;
    uint32 ResetBotLevelPlayedTimeCheckFrequency;
    bool ResetBotLevelIgnoreGuildWithRealPlayers;
    std::vector<std::string> ResetBotLevelExcludeNames;

private:
    PlayerbotAIConfig() = default;
    ~PlayerbotAIConfig() = default;

    PlayerbotAIConfig(PlayerbotAIConfig const&) = delete;
    PlayerbotAIConfig& operator=(PlayerbotAIConfig const&) = delete;

    PlayerbotAIConfig(PlayerbotAIConfig&&) = delete;
    PlayerbotAIConfig& operator=(PlayerbotAIConfig&&) = delete;
};

#define sPlayerbotAIConfig PlayerbotAIConfig::Instance()

#endif
