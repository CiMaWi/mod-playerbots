/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaKiteConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using KiteRow = ConfigRow<KiteConfig>;
using DrinkRow = ConfigRow<DrinkConfig>;
using JumpRow = ConfigRow<JumpConfig>;

// The caps of the old reads (a larger value is capped, now with a warning)
constexpr uint32 MaxMs = 120000;
constexpr uint32 MaxPct = 100;
constexpr uint32 MaxSkill = 99;  // a skill, 0-99
constexpr uint32 MaxSwitch = 1;
constexpr uint32 MaxCount = 1000;  // rays, relocations, yards

constexpr std::array<KiteRow, 15> KiteRows = {{
    {"KnownFromSkill", &KiteConfig::KnownFromSkill, 0, MaxSkill},
    {"EffortMin", &KiteConfig::EffortMin, 0, MaxPct},
    {"MeleeYards", &KiteConfig::MeleeYards, 0, MaxCount},
    {"StepYards", &KiteConfig::StepYards, 0, MaxCount},
    {"StepIntervalMs", &KiteConfig::StepIntervalMs, 0, MaxMs},
    {"EpisodeMs", &KiteConfig::EpisodeMs, 0, MaxMs},
    {"GiveUpMsAtEffort0", &KiteConfig::GiveUpMsAtEffort0, 0, MaxMs},
    {"GiveUpMsAtEffort100", &KiteConfig::GiveUpMsAtEffort100, 0, MaxMs},
    {"SnareFirst", &KiteConfig::SnareFirst, 0, MaxSwitch},
    {"PillarMaxYards", &KiteConfig::PillarMaxYards, 0, MaxCount},
    {"PartnerMinYards", &KiteConfig::PartnerMinYards, 0, MaxCount},
    {"JumpMinSavedYards", &KiteConfig::JumpMinSavedYards, 0, MaxCount},
    {"MemoryFromSkill", &KiteConfig::MemoryFromSkill, 0, MaxSkill},
    {"MemoryWindowMs", &KiteConfig::MemoryWindowMs, 0, MaxMs},
    {"PanicBelowSkill", &KiteConfig::PanicBelowSkill, 0, MaxSkill},
}};

constexpr std::array<DrinkRow, 12> DrinkRows = {{
    {"KnownFromSkill", &DrinkConfig::KnownFromSkill, 0, MaxSkill},
    {"EffortMin", &DrinkConfig::EffortMin, 0, MaxPct},
    {"PrepEffortMin", &DrinkConfig::PrepEffortMin, 0, MaxPct},
    {"PrepBelowPct", &DrinkConfig::PrepBelowPct, 0, MaxPct},
    {"PrepMinLeftMs", &DrinkConfig::PrepMinLeftMs, 0, MaxMs},
    {"SafeBelowPct", &DrinkConfig::SafeBelowPct, 0, MaxPct},
    {"SafeBelowPctHealer", &DrinkConfig::SafeBelowPctHealer, 0, MaxPct},
    {"EnemyYards", &DrinkConfig::EnemyYards, 0, MaxCount},
    {"QuietMs", &DrinkConfig::QuietMs, 0, MaxMs},
    {"PartnerMinHpPct", &DrinkConfig::PartnerMinHpPct, 0, MaxPct},
    {"BreakEnemyYards", &DrinkConfig::BreakEnemyYards, 0, MaxCount},
    {"NoDrinkLogMs", &DrinkConfig::NoDrinkLogMs, 0, MaxMs},
}};

constexpr std::array<JumpRow, 10> JumpRows = {{
    {"KnownFromSkill", &JumpConfig::KnownFromSkill, 0, MaxSkill},
    {"ChaseFromAggression", &JumpConfig::ChaseFromAggression, 0, MaxSkill},
    {"EscapeFromCaution", &JumpConfig::EscapeFromCaution, 0, MaxSkill},
    {"MinSavedYards", &JumpConfig::MinSavedYards, 0, MaxCount},
    {"LandingTolX10", &JumpConfig::LandingTolX10, 0, MaxCount},
    {"TestJumps", &JumpConfig::TestJumps, 0, MaxCount},
    {"TestWalkTimeoutMs", &JumpConfig::TestWalkTimeoutMs, 0, MaxMs},
    {"TestAirTimeoutMs", &JumpConfig::TestAirTimeoutMs, 0, MaxMs},
    {"TestSettleMs", &JumpConfig::TestSettleMs, 0, MaxMs},
    {"TestMaxStuckMs", &JumpConfig::TestMaxStuckMs, 0, MaxMs},
}};
}  // namespace

ConfigFamily<KiteConfig> const KiteFamily{"Playerbots.Tactics.Kite.", KiteRows};
ConfigFamily<DrinkConfig> const DrinkFamily{"Playerbots.Tactics.Drink.", DrinkRows};
ConfigFamily<JumpConfig> const JumpFamily{"Playerbots.Tactics.Jump.", JumpRows};

KiteConfig& KiteConfigStorage()
{
    static KiteConfig values;
    return values;
}

DrinkConfig& DrinkConfigStorage()
{
    static DrinkConfig values;
    return values;
}

JumpConfig& JumpConfigStorage()
{
    static JumpConfig values;
    return values;
}
