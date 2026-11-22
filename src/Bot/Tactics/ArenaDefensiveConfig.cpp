/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaDefensiveConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using DefensiveRow = ConfigRow<DefensiveConfig>;

// The caps of the old reads (a larger value is capped, now with a warning)
constexpr uint32 MaxMs = 120000;
constexpr uint32 MaxPct = 100;
constexpr uint32 MaxFullSkill = 100;  // 100: no bot below full ability
constexpr uint32 MaxSwitch = 1;
constexpr uint32 MaxEvents = 20;
constexpr uint32 MaxYards = 100;
constexpr uint32 MaxEnemies = 8;  // the enemy team's size cap

constexpr std::array<DefensiveRow, 30> DefensiveRows = {{
    {"BurstWindowMs", &DefensiveConfig::BurstWindowMs, 0, MaxMs},
    {"BurstLossPctAtSkill1", &DefensiveConfig::BurstLossPctAtSkill1, 0, MaxPct},
    {"BurstLossPctAtSkill99", &DefensiveConfig::BurstLossPctAtSkill99, 0, MaxPct},
    {"SampleMs", &DefensiveConfig::SampleMs, 0, MaxMs},
    {"FocusedMinEnemies", &DefensiveConfig::FocusedMinEnemies, 0, MaxEnemies},
    {"FocusedMinTeam", &DefensiveConfig::FocusedMinTeam, 0, MaxEnemies},
    {"HpPctAtSkill1", &DefensiveConfig::HpPctAtSkill1, 0, MaxPct},
    {"HpPctAtSkill99", &DefensiveConfig::HpPctAtSkill99, 0, MaxPct},
    {"FocusedHpBonusPct", &DefensiveConfig::FocusedHpBonusPct, 0, MaxPct},
    {"CriticalHpPct", &DefensiveConfig::CriticalHpPct, 0, MaxPct},
    {"OpenerPctAtSkill1", &DefensiveConfig::OpenerPctAtSkill1, 0, MaxPct},
    {"OpenerPctAtSkill99", &DefensiveConfig::OpenerPctAtSkill99, 0, MaxPct},
    {"LateMsAtEffort0", &DefensiveConfig::LateMsAtEffort0, 0, MaxMs},
    {"LateMsAtEffort100", &DefensiveConfig::LateMsAtEffort100, 0, MaxMs},
    {"CautionShiftPct", &DefensiveConfig::CautionShiftPct, 0, MaxPct},
    {"MinGapMs", &DefensiveConfig::MinGapMs, 0, MaxMs},
    {"PotionEnable", &DefensiveConfig::PotionEnable, 0, MaxSwitch},
    {"PotionHpPct", &DefensiveConfig::PotionHpPct, 0, MaxPct},
    {"Protect.HpPctAtSkill1", &DefensiveConfig::ProtectHpPctAtSkill1, 0, MaxPct},
    {"Protect.HpPctAtSkill99", &DefensiveConfig::ProtectHpPctAtSkill99, 0, MaxPct},
    {"Protect.AttackedWithinMs", &DefensiveConfig::ProtectAttackedWithinMs, 0, MaxMs},
    {"Protect.DistanceYards", &DefensiveConfig::ProtectDistanceYards, 0, MaxYards},
    {"Protect.MinGapMs", &DefensiveConfig::ProtectMinGapMs, 0, MaxMs},
    {"Opponent.Enable", &DefensiveConfig::OpponentEnable, 0, MaxSwitch},
    {"Opponent.LeadPctAtForesight99", &DefensiveConfig::OpponentLeadPctAtForesight99, 0, MaxPct},
    {"Opponent.MinEvents", &DefensiveConfig::OpponentMinEvents, 0, MaxEvents},
    {"Opponent.FromSkill", &DefensiveConfig::OpponentFromSkill, 0, MaxFullSkill},
    {"Opponent.ExpireMs", &DefensiveConfig::OpponentExpireMs, 0, MaxMs},
    {"BurstHoldMaxMs", &DefensiveConfig::BurstHoldMaxMs, 0, MaxMs},
    {"BurstHoldFloorPct", &DefensiveConfig::BurstHoldFloorPct, 0, MaxPct},
}};
}  // namespace

ConfigFamily<DefensiveConfig> const DefensiveFamily{"Playerbots.Tactics.Defensive.", DefensiveRows};

DefensiveConfig& DefensiveConfigStorage()
{
    static DefensiveConfig values;
    return values;
}
