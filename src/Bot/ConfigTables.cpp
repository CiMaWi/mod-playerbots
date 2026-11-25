/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ConfigTables.h"

#include <array>
#include <functional>
#include <set>
#include <vector>

#include "AcquaintanceConfig.h"
#include "ArenaCcConfig.h"
#include "ArenaCoverConfig.h"
#include "ArenaDefensiveConfig.h"
#include "ArenaFocusConfig.h"
#include "ArenaInterruptConfig.h"
#include "ArenaKiteConfig.h"
#include "ArenaOnlyConfig.h"
#include "ArenaRecruitConfig.h"
#include "ArenaTeamConfig.h"
#include "AuctionConfig.h"
#include "BankMailConfig.h"
#include "BotAttributeTypes.h"
#include "Chat.h"
#include "Config.h"
#include "ConfigTable.h"
#include "FleeConfig.h"
#include "ItemUseConfig.h"
#include "LadderConfig.h"
#include "Log.h"
#include "LoneKeys.h"
#include "MatchLogConfig.h"
#include "NaturalCastConfig.h"
#include "NaturalMoveConfig.h"
#include "NaturalPathConfig.h"
#include "NaturalReactionConfig.h"
#include "NaturalTraceConfig.h"
#include "NaturalTurnConfig.h"
#include "PlayerbotAIConfig.h"
#include "Population.h"
#include "PopulationConfig.h"
#include "ProfessionConfig.h"
#include "PvpGearConfig.h"
#include "PvpTestConfig.h"
#include "QuestingConfig.h"
#include "ScriptMgr.h"
#include "SkillConfig.h"
#include "SkillGrowthConfig.h"
#include "TacticGateConfig.h"
#include "TacticIds.h"
#include "TestPlayerConfig.h"
#include "WorldLog.h"
#include "WorldLogConfig.h"
#include "WorldPvp.h"
#include "WorldPvpConfig.h"
#include "WorldTravelConfig.h"

namespace
{
// A family with the struct it fills, type-erased; one line in Registry() per family
struct ConfigTableEntry
{
    std::string Prefix;
    std::function<void()> Load;
    // does the family (a row or a code tail) know the key
    std::function<bool(std::string const&)> Knows;
    // the lines of the rows whose full key starts with the filter
    std::function<void(std::string const&, std::vector<std::string>&)> Print;
};

void WarnConfigRow(std::string const& text) { LOG_WARN("server.loading", "{}", text); }

// The keys under Playerbots.Skill. that PlayerbotAIConfig still reads by hand: the attribute correlation pairs
constexpr std::array<char const*, 1> SkillCodeTails = {"Attributes.Corr."};

// codeTails: the keys under the family's prefix that code outside the table reads (not unknown)
template <typename Struct>
ConfigTableEntry MakeEntry(ConfigFamily<Struct> const& family, Struct& target,
                           std::span<char const* const> codeTails = {})
{
    ConfigTableEntry entry;
    entry.Prefix = family.Prefix;
    entry.Load = [&family, &target]()
    {
        LoadConfigFamily(
            family, target, [](std::string const& key, auto fallback, bool warnIfMissing)
            { return sConfigMgr->GetOption<decltype(fallback)>(key, fallback, warnIfMissing); }, WarnConfigRow);
    };
    entry.Knows = [&family, codeTails](std::string const& key) { return KnowsConfigKey(family, key, codeTails); };
    entry.Print = [&family, &target](std::string const& filter, std::vector<std::string>& lines)
    {
        for (ConfigRow<Struct> const& row : family.Rows)
        {
            if (row.Kind == ConfigKind::Retired)
                continue;
            if ((std::string(family.Prefix) + row.Key).rfind(filter, 0) == 0)
                lines.push_back(ConfigRowLine(family, row, target));
        }
    };
    return entry;
}

// Removed switches: one family per full key with one empty-key row, so exactly that key is known and a line that
// still sets it is ignored without a warning (the parent prefixes hold live keys read elsewhere)
struct RetiredSwitches
{
    bool Acquaintance = false;
    bool SkipIdle = false;
    bool Goals = false;
    bool Persistent = false;
    bool RotationEnable = false;
    bool RetireEnable = false;
    bool RetireDryRun = false;
    bool RetireTestRange = false;
    bool RetireMaxScore = false;
    bool RetireCandidates = false;
    bool RetireCreateAttempts = false;
    bool RetireSummaryMinutes = false;
    bool NaturalChoice = false;
    bool NaturalLoad = false;
    bool NaturalEase = false;
    bool NaturalAnticipate = false;
    bool NaturalBusySkip = false;
    bool NaturalMovement = false;
    bool NaturalThink = false;
    bool NaturalQueue = false;
    bool NaturalMemory = false;
    bool NaturalReaction = false;
    bool NaturalEndToEnd = false;
    bool NaturalCadence = false;
    bool MatchLogEnable = false;
    bool PvpGearEnable = false;
    bool PvpGearWear = false;
    bool SkillEnable = false;
    bool ArenaGroupOnWorldThread = false;
    bool ArenaRatingMode = false;
    bool ArenaTeamMinRating = false;
    bool ArenaTeamMaxRating = false;
    bool LadderEnable = false;
    bool NearRealPlayerYards = false;
    bool MatchmakerEnable = false;
};

RetiredSwitches RetiredSwitchValues;

constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredAcquaintanceRows = {{
    {"", &RetiredSwitches::Acquaintance, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredAcquaintanceFamily{"Playerbots.Acquaintance.Enable",
                                                              RetiredAcquaintanceRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredSkipIdleRows = {{
    {"", &RetiredSwitches::SkipIdle, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredSkipIdleFamily{"Playerbots.SkipIdleTriggerChecks", RetiredSkipIdleRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredGoalsRows = {{
    {"", &RetiredSwitches::Goals, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredGoalsFamily{"Playerbots.WorldTravel.Planner.Goals", RetiredGoalsRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredPersistentRows = {{
    {"", &RetiredSwitches::Persistent, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredPersistentFamily{"Playerbots.Population.Persistent", RetiredPersistentRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredRotationEnableRows = {{
    {"", &RetiredSwitches::RotationEnable, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredRotationEnableFamily{"Playerbots.Population.Rotation.Enable",
                                                                RetiredRotationEnableRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredRetireEnableRows = {{
    {"", &RetiredSwitches::RetireEnable, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredRetireEnableFamily{"Playerbots.Population.Retire.Enable",
                                                              RetiredRetireEnableRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredRetireDryRunRows = {{
    {"", &RetiredSwitches::RetireDryRun, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredRetireDryRunFamily{"Playerbots.Population.Retire.DryRun",
                                                              RetiredRetireDryRunRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredRetireTestRangeRows = {{
    {"", &RetiredSwitches::RetireTestRange, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredRetireTestRangeFamily{"Playerbots.Population.Retire.TestRange",
                                                                 RetiredRetireTestRangeRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredRetireMaxScoreRows = {{
    {"", &RetiredSwitches::RetireMaxScore, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredRetireMaxScoreFamily{"Playerbots.Population.Retire.MaxScore",
                                                                RetiredRetireMaxScoreRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredRetireCandidatesRows = {{
    {"", &RetiredSwitches::RetireCandidates, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredRetireCandidatesFamily{"Playerbots.Population.Retire.Candidates",
                                                                  RetiredRetireCandidatesRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredRetireCreateAttemptsRows = {{
    {"", &RetiredSwitches::RetireCreateAttempts, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredRetireCreateAttemptsFamily{"Playerbots.Population.Retire.CreateAttempts",
                                                                      RetiredRetireCreateAttemptsRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredRetireSummaryMinutesRows = {{
    {"", &RetiredSwitches::RetireSummaryMinutes, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredRetireSummaryMinutesFamily{"Playerbots.Population.Retire.SummaryMinutes",
                                                                      RetiredRetireSummaryMinutesRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalChoiceRows = {{
    {"", &RetiredSwitches::NaturalChoice, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalChoiceFamily{"Playerbots.Natural.Reaction.Choice.Enable",
                                                               RetiredNaturalChoiceRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalLoadRows = {{
    {"", &RetiredSwitches::NaturalLoad, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalLoadFamily{"Playerbots.Natural.Reaction.Load.Enable",
                                                             RetiredNaturalLoadRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalEaseRows = {{
    {"", &RetiredSwitches::NaturalEase, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalEaseFamily{"Playerbots.Natural.Reaction.Ease.Enable",
                                                             RetiredNaturalEaseRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalAnticipateRows = {{
    {"", &RetiredSwitches::NaturalAnticipate, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalAnticipateFamily{"Playerbots.Natural.Reaction.Anticipate.Enable",
                                                                   RetiredNaturalAnticipateRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalBusySkipRows = {{
    {"", &RetiredSwitches::NaturalBusySkip, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalBusySkipFamily{"Playerbots.Natural.Reaction.Reeval.BusySkip",
                                                                 RetiredNaturalBusySkipRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalMovementRows = {{
    {"", &RetiredSwitches::NaturalMovement, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalMovementFamily{"Playerbots.Natural.Reaction.Movement.Enable",
                                                                 RetiredNaturalMovementRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalThinkRows = {{
    {"", &RetiredSwitches::NaturalThink, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalThinkFamily{"Playerbots.Natural.Reaction.Think.Enable",
                                                              RetiredNaturalThinkRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalQueueRows = {{
    {"", &RetiredSwitches::NaturalQueue, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalQueueFamily{"Playerbots.Natural.Reaction.Queue.Enable",
                                                              RetiredNaturalQueueRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalMemoryRows = {{
    {"", &RetiredSwitches::NaturalMemory, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalMemoryFamily{"Playerbots.Natural.Reaction.Memory.Enable",
                                                               RetiredNaturalMemoryRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalReactionRows = {{
    {"", &RetiredSwitches::NaturalReaction, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalReactionFamily{"Playerbots.Natural.Reaction.Enable",
                                                                 RetiredNaturalReactionRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalEndToEndRows = {{
    {"", &RetiredSwitches::NaturalEndToEnd, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalEndToEndFamily{"Playerbots.Natural.Reaction.EndToEnd",
                                                                 RetiredNaturalEndToEndRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNaturalCadenceRows = {{
    {"", &RetiredSwitches::NaturalCadence, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNaturalCadenceFamily{"Playerbots.Natural.Cadence.Enable",
                                                                RetiredNaturalCadenceRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredMatchLogEnableRows = {{
    {"", &RetiredSwitches::MatchLogEnable, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredMatchLogEnableFamily{"Playerbots.MatchLog.Enable",
                                                                RetiredMatchLogEnableRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredPvpGearEnableRows = {{
    {"", &RetiredSwitches::PvpGearEnable, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredPvpGearEnableFamily{"Playerbots.PvpGear.Enable", RetiredPvpGearEnableRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredPvpGearWearRows = {{
    {"", &RetiredSwitches::PvpGearWear, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredPvpGearWearFamily{"Playerbots.PvpGear.Wear", RetiredPvpGearWearRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredSkillEnableRows = {{
    {"", &RetiredSwitches::SkillEnable, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredSkillEnableFamily{"Playerbots.Skill.Enable", RetiredSkillEnableRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredArenaGroupOnWorldThreadRows = {{
    {"", &RetiredSwitches::ArenaGroupOnWorldThread, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredArenaGroupOnWorldThreadFamily{"Playerbots.RandomBotArenaGroupOnWorldThread",
                                                                         RetiredArenaGroupOnWorldThreadRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredArenaRatingModeRows = {{
    {"", &RetiredSwitches::ArenaRatingMode, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredArenaRatingModeFamily{"Playerbots.RandomBotArenaTeamRatingMode",
                                                                 RetiredArenaRatingModeRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredArenaTeamMinRatingRows = {{
    {"", &RetiredSwitches::ArenaTeamMinRating, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredArenaTeamMinRatingFamily{"Playerbots.RandomBotArenaTeamMinRating",
                                                                    RetiredArenaTeamMinRatingRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredArenaTeamMaxRatingRows = {{
    {"", &RetiredSwitches::ArenaTeamMaxRating, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredArenaTeamMaxRatingFamily{"Playerbots.RandomBotArenaTeamMaxRating",
                                                                    RetiredArenaTeamMaxRatingRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredLadderEnableRows = {{
    {"", &RetiredSwitches::LadderEnable, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredLadderEnableFamily{"Playerbots.Ladder.Enable", RetiredLadderEnableRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredNearRealPlayerYardsRows = {{
    {"", &RetiredSwitches::NearRealPlayerYards, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredNearRealPlayerYardsFamily{"Playerbots.Natural.Cadence.NearRealPlayerYards",
                                                                     RetiredNearRealPlayerYardsRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredMatchmakerEnableRows = {{
    {"", &RetiredSwitches::MatchmakerEnable, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredMatchmakerEnableFamily{"Playerbots.Ladder.Matchmaker.Enable",
                                                                  RetiredMatchmakerEnableRows};

// CS22c-j add one line per family
std::vector<ConfigTableEntry> BuildRegistry()
{
    std::vector<ConfigTableEntry> entries = {
        MakeEntry(BankMailFamily, sPlayerbotAIConfig.BankMail),
        MakeEntry(ProfessionFamily, sPlayerbotAIConfig.Professions),
        MakeEntry(AuctionFamily, sPlayerbotAIConfig.Auction),
        MakeEntry(QuestingFamily, sPlayerbotAIConfig.Questing),
        MakeEntry(HoldFamily, sPlayerbotAIConfig.TacticFocus),
        MakeEntry(FocusFamily, sPlayerbotAIConfig.TacticFocus),
        MakeEntry(FocusTargetFamily, sPlayerbotAIConfig.TacticFocus),
        MakeEntry(CcFamily, sPlayerbotAIConfig.TacticCc),
        MakeEntry(AvoidFamily, sPlayerbotAIConfig.TacticCc),
        MakeEntry(InterruptFamily, sPlayerbotAIConfig.TacticInterrupt),
        MakeEntry(DefensiveFamily, sPlayerbotAIConfig.TacticDefensive),
        MakeEntry(FakeFamily, sPlayerbotAIConfig.TacticFake),
        MakeEntry(LosFamily, sPlayerbotAIConfig.TacticLos),
        MakeEntry(PillarFamily, sPlayerbotAIConfig.TacticPillar),
        MakeEntry(KiteFamily, sPlayerbotAIConfig.TacticKite),
        MakeEntry(DrinkFamily, sPlayerbotAIConfig.TacticDrink),
        MakeEntry(JumpFamily, sPlayerbotAIConfig.TacticJump),
        MakeEntry(SkillFamily, sPlayerbotAIConfig.Skill, SkillCodeTails),
        MakeEntry(SkillGateFamily, sPlayerbotAIConfig.SkillGate),
        MakeEntry(SkillGrowthFamily, sPlayerbotAIConfig.SkillGrowth),
        MakeEntry(SkillAttributeLoadingFamily, sPlayerbotAIConfig.SkillAttributes),
        MakeEntry(SkillAttributeSpreadFamily, sPlayerbotAIConfig.SkillAttributes),
        MakeEntry(ReactionFamily, sPlayerbotAIConfig.NaturalReaction),
        MakeEntry(ReactionChoiceFamily, sPlayerbotAIConfig.NaturalReaction.Choice),
        MakeEntry(ReactionLoadFamily, sPlayerbotAIConfig.NaturalReaction.Load),
        MakeEntry(ReactionEaseFamily, sPlayerbotAIConfig.NaturalReaction.Ease),
        MakeEntry(ReactionThinkFamily, sPlayerbotAIConfig.NaturalReaction.Think),
        MakeEntry(ReactionAnticipateFamily, sPlayerbotAIConfig.NaturalReaction.Anticipate),
        MakeEntry(ReactionQueueFamily, sPlayerbotAIConfig.NaturalReaction.Queue),
        MakeEntry(ReactionReevalFamily, sPlayerbotAIConfig.NaturalReaction.Reeval),
        MakeEntry(ReactionMemoryFamily, sPlayerbotAIConfig.NaturalReaction.Memory),
        MakeEntry(CadenceFamily, sPlayerbotAIConfig.NaturalCadence),
        MakeEntry(NaturalFacingFamily, sPlayerbotAIConfig.NaturalFacing),
        MakeEntry(NaturalTrackFamily, sPlayerbotAIConfig.NaturalTrack),
        MakeEntry(NaturalPathFamily, sPlayerbotAIConfig.NaturalPath),
        MakeEntry(NaturalTraceFamily, sPlayerbotAIConfig.NaturalTraceConfig),
        MakeEntry(FleeFamily, sPlayerbotAIConfig.Flee),
        MakeEntry(WorldTravelFamily, sPlayerbotAIConfig.WorldTravel),
        MakeEntry(LadderFamily, sPlayerbotAIConfig.Ladder),
        MakeEntry(MatchmakerFamily, sPlayerbotAIConfig.Ladder.Matchmaker),
        MakeEntry(ArenaTeamFamily, sPlayerbotAIConfig.ArenaTeamOptions),
        MakeEntry(ArenaRecruitFamily, sPlayerbotAIConfig.ArenaRecruitOptions),
        MakeEntry(PvpGearFamily, sPlayerbotAIConfig.PvpGear),
        MakeEntry(AcquaintanceFamily, sPlayerbotAIConfig.Acquaintance),
        MakeEntry(MatchLogFamily, sPlayerbotAIConfig.MatchLog),
        MakeEntry(PvpTestFamily, sPlayerbotAIConfig.PvpTest),
        MakeEntry(TestPlayerFamily, sPlayerbotAIConfig.TestPlayer),
        MakeEntry(ArenaOnlyFamily, sPlayerbotAIConfig.ArenaOnly),
        MakeEntry(ItemUseFamily, sPlayerbotAIConfig.ItemUse),
        MakeEntry(SightRangeCapFamily, sPlayerbotAIConfig.Lone),
        MakeEntry(RatedQueueSlotsFamily, sPlayerbotAIConfig.Lone),
        MakeEntry(RatedQueueUncappedFamily, sPlayerbotAIConfig.Lone),
        MakeEntry(TestNamePrefixFamily, sPlayerbotAIConfig.Lone),
        MakeEntry(DebugThreadChecksFamily, sPlayerbotAIConfig.Lone),
        MakeEntry(TestMissLogFamily, sPlayerbotAIConfig.Lone),
        MakeEntry(WorldLogFamily, WorldLog::Options()),
        MakeEntry(PopulationRotationFamily, Population::Options()),
        MakeEntry(PopulationRetireFamily, Population::Options()),
        MakeEntry(PopulationFollowFamily, Population::Options()),
        MakeEntry(WorldPvpFamily, sPlayerbotAIConfig.WorldPvp),
        MakeEntry(WorldPvpEnthusiasmFamily, sPlayerbotAIConfig.WorldPvp.Enthusiasm),
        MakeEntry(RetiredAcquaintanceFamily, RetiredSwitchValues),
        MakeEntry(RetiredSkipIdleFamily, RetiredSwitchValues),
        MakeEntry(RetiredGoalsFamily, RetiredSwitchValues),
        MakeEntry(RetiredPersistentFamily, RetiredSwitchValues),
        MakeEntry(RetiredRotationEnableFamily, RetiredSwitchValues),
        MakeEntry(RetiredRetireEnableFamily, RetiredSwitchValues),
        MakeEntry(RetiredRetireDryRunFamily, RetiredSwitchValues),
        MakeEntry(RetiredRetireTestRangeFamily, RetiredSwitchValues),
        MakeEntry(RetiredRetireMaxScoreFamily, RetiredSwitchValues),
        MakeEntry(RetiredRetireCandidatesFamily, RetiredSwitchValues),
        MakeEntry(RetiredRetireCreateAttemptsFamily, RetiredSwitchValues),
        MakeEntry(RetiredRetireSummaryMinutesFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalChoiceFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalLoadFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalEaseFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalAnticipateFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalBusySkipFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalMovementFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalThinkFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalQueueFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalMemoryFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalReactionFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalEndToEndFamily, RetiredSwitchValues),
        MakeEntry(RetiredNaturalCadenceFamily, RetiredSwitchValues),
        MakeEntry(RetiredMatchLogEnableFamily, RetiredSwitchValues),
        MakeEntry(RetiredPvpGearEnableFamily, RetiredSwitchValues),
        MakeEntry(RetiredPvpGearWearFamily, RetiredSwitchValues),
        MakeEntry(RetiredSkillEnableFamily, RetiredSwitchValues),
        MakeEntry(RetiredArenaGroupOnWorldThreadFamily, RetiredSwitchValues),
        MakeEntry(RetiredArenaRatingModeFamily, RetiredSwitchValues),
        MakeEntry(RetiredArenaTeamMinRatingFamily, RetiredSwitchValues),
        MakeEntry(RetiredArenaTeamMaxRatingFamily, RetiredSwitchValues),
        MakeEntry(RetiredLadderEnableFamily, RetiredSwitchValues),
        MakeEntry(RetiredNearRealPlayerYardsFamily, RetiredSwitchValues),
        MakeEntry(RetiredMatchmakerEnableFamily, RetiredSwitchValues),
    };
    // One family per tactic (Playerbots.Tactics.<ConfigName>.), then the system switch: its Finish reads them
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
        entries.push_back(MakeEntry(TacticSettingsFamily(index), sPlayerbotAIConfig.Tactics.Settings[index]));
    entries.push_back(MakeEntry(TacticsEnableFamily, sPlayerbotAIConfig.Tactics));
    return entries;
}

std::vector<ConfigTableEntry> const& Registry()
{
    static std::vector<ConfigTableEntry> const entries = BuildRegistry();
    return entries;
}

class ConfigTablesWorldScript : public WorldScript
{
public:
    ConfigTablesWorldScript() : WorldScript("ConfigTablesWorldScript", {WORLDHOOK_ON_AFTER_CONFIG_LOAD}) {}

    // The first load is covered by PlayerbotAIConfig::Initialize; `.reload config` does not call it
    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
        {
            LoadConfigTables();
            // The walk set the role-derived values (formation, recruiting, PvP gear buying, the seed) to their keys'
            // again
            sPlayerbotAIConfig.ApplyLadderRole();
            WorldLog::LoadConfig(true);
            Population::LoadConfig(true);
        }
    }
};
}  // namespace

// A key is unknown when it lies under a family's prefix (equals a full-key family's prefix) and no family knows it:
// several families share a prefix (Playerbots.Skill. holds the skill, gate, growth and attribute rows)
void LoadConfigTables()
{
    std::set<std::string> keys;
    for (ConfigTableEntry const& entry : Registry())
    {
        entry.Load();
        bool const fullKey = entry.Prefix.back() != '.';
        for (std::string const& key : sConfigMgr->GetKeysByString(entry.Prefix))
            if (!fullKey || key == entry.Prefix)
                keys.insert(key);
    }

    for (std::string const& key : keys)
    {
        bool known = false;
        std::string owner;
        for (ConfigTableEntry const& entry : Registry())
        {
            known = known || entry.Knows(key);
            if (entry.Prefix.back() == '.' && key.rfind(entry.Prefix, 0) == 0 && entry.Prefix.size() > owner.size())
                owner = entry.Prefix;
        }
        if (!known)
            WarnConfigRow(key + " is set but not a known key of " + owner + "*, ignored");
    }

    // The world PvP values built from the rows (the enthusiasm and ghost values, once per load, WorldPvp.h)
    ::WorldPvp::PublishConfigValues(sPlayerbotAIConfig.WorldPvp);
}

void PrintConfigTables(ChatHandler* handler, std::string const& prefix)
{
    std::vector<std::string> lines;
    for (ConfigTableEntry const& entry : Registry())
        if (entry.Prefix.rfind(prefix, 0) == 0 || prefix.rfind(entry.Prefix, 0) == 0)
            entry.Print(prefix, lines);

    if (lines.empty())
    {
        handler->PSendSysMessage("CONFIG 0 rows: no config rows under '{}'", prefix);
        return;
    }
    handler->PSendSysMessage("CONFIG {} rows (* = differs from the default)", lines.size());
    for (std::string const& line : lines)
        handler->PSendSysMessage("{}", line);
}

void AddSC_config_tables() { new ConfigTablesWorldScript(); }
