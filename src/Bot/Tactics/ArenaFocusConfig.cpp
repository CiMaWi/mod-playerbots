/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaFocusConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using Row = ConfigRow<FocusConfig>;

// The caps of the old reads (a larger value is capped, now with a warning)
constexpr uint32 MaxMs = 120000;
constexpr uint32 MaxPct = 100;
constexpr uint32 MaxMargin = 255;
constexpr uint32 MaxSeconds = 600;
constexpr uint32 MaxSkill = 99;
constexpr uint32 MaxMoonMode = 2;  // ArenaFocus.h FOCUS_MOON_*

constexpr std::array<Row, 13> HoldRows = {{
    {"HoldMsAtSkill1", &FocusConfig::HoldMsAtSkill1, 0, MaxMs},
    {"HoldMsAtSkill99", &FocusConfig::HoldMsAtSkill99, 0, MaxMs},
    {"HysteresisPctAtSkill1", &FocusConfig::HoldHysteresisPctAtSkill1, 0, MaxPct},
    {"HysteresisPctAtSkill99", &FocusConfig::HoldHysteresisPctAtSkill99, 0, MaxPct},
    {"ExecPctAtSkill1", &FocusConfig::HoldExecPctAtSkill1, 0, MaxPct},
    {"ExecPctAtSkill99", &FocusConfig::HoldExecPctAtSkill99, 0, MaxPct},
    {"ExecChanceAtSkill1", &FocusConfig::HoldExecChanceAtSkill1, 0, MaxPct},
    {"ExecChanceAtSkill99", &FocusConfig::HoldExecChanceAtSkill99, 0, MaxPct},
    {"FollowChanceAtSkill1", &FocusConfig::HoldFollowChanceAtSkill1, 0, MaxPct},
    {"FollowChanceAtSkill99", &FocusConfig::HoldFollowChanceAtSkill99, 0, MaxPct},
    {"FollowDelayMsAtSkill1", &FocusConfig::HoldFollowDelayMsAtSkill1, 0, MaxMs},
    {"FollowDelayMsAtSkill99", &FocusConfig::HoldFollowDelayMsAtSkill99, 0, MaxMs},
    {"LosLostMs", &FocusConfig::HoldLosLostMs, 0, MaxMs},
}};

constexpr std::array<Row, 22> FocusRows = {{
    {"PlanIntervalMsAtSkill1", &FocusConfig::FocusPlanIntervalMsAtSkill1, 0, MaxMs},
    {"PlanIntervalMsAtSkill99", &FocusConfig::FocusPlanIntervalMsAtSkill99, 0, MaxMs},
    {"HoldMsAtSkill1", &FocusConfig::FocusHoldMsAtSkill1, 0, MaxMs},
    {"HoldMsAtSkill99", &FocusConfig::FocusHoldMsAtSkill99, 0, MaxMs},
    {"HysteresisPctAtSkill1", &FocusConfig::FocusHysteresisPctAtSkill1, 0, MaxPct},
    {"HysteresisPctAtSkill99", &FocusConfig::FocusHysteresisPctAtSkill99, 0, MaxPct},
    {"ExecPctAtSkill1", &FocusConfig::FocusExecPctAtSkill1, 0, MaxPct},
    {"ExecPctAtSkill99", &FocusConfig::FocusExecPctAtSkill99, 0, MaxPct},
    {"ExecChanceAtSkill1", &FocusConfig::FocusExecChanceAtSkill1, 0, MaxPct},
    {"ExecChanceAtSkill99", &FocusConfig::FocusExecChanceAtSkill99, 0, MaxPct},
    {"RemarkDelayMsAtSkill1", &FocusConfig::FocusRemarkDelayMsAtSkill1, 0, MaxMs},
    {"RemarkDelayMsAtSkill99", &FocusConfig::FocusRemarkDelayMsAtSkill99, 0, MaxMs},
    {"StrongMarginAtSkill1", &FocusConfig::FocusStrongMarginAtSkill1, 0, MaxMargin},
    {"StrongMarginAtSkill99", &FocusConfig::FocusStrongMarginAtSkill99, 0, MaxMargin},
    {"MinRemarkSeconds", &FocusConfig::FocusMinRemarkSeconds, 0, MaxSeconds},
    {"HumanMarkRespectSeconds", &FocusConfig::FocusHumanMarkRespectSeconds, 0, MaxSeconds},
    {"LosLostMs", &FocusConfig::FocusLosLostMs, 0, MaxMs},
    {"DefensiveSwapPct", &FocusConfig::FocusDefensiveSwapPct, 0, MaxPct},
    {"AddonKnowledgeFromSkill", &FocusConfig::FocusAddonKnowledgeFromSkill, 0, MaxSkill},
    {"MoonMode", &FocusConfig::FocusMoonMode, 0, MaxMoonMode},
    {"MoonGraceMs", &FocusConfig::FocusMoonGraceMs, 0, MaxMs},
    {"MoonRetryMs", &FocusConfig::FocusMoonRetryMs, 0, MaxMs},
}};

constexpr std::array<Row, 3> FocusTargetRows = {{
    {"MoonPctAtTeamwork1", &FocusConfig::FocusTargetMoonPctAtTeamwork1, 0, MaxPct},
    {"MoonPctAtTeamwork99", &FocusConfig::FocusTargetMoonPctAtTeamwork99, 0, MaxPct},
    {"KickBonus", &FocusConfig::FocusTargetKickBonus, 0, MaxMargin},
}};
}  // namespace

ConfigFamily<FocusConfig> const HoldFamily{"Playerbots.Tactics.Hold.", HoldRows};
ConfigFamily<FocusConfig> const FocusFamily{"Playerbots.Tactics.Focus.", FocusRows};
ConfigFamily<FocusConfig> const FocusTargetFamily{"Playerbots.Tactics.FocusTarget.", FocusTargetRows};

FocusConfig& FocusConfigStorage()
{
    static FocusConfig values;
    return values;
}
