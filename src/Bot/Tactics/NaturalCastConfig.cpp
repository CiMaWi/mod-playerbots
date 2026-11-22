/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaturalCastConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using FakeRow = ConfigRow<FakeConfig>;

// The caps of the old reads (a larger value is capped, now with a warning)
constexpr uint32 MaxMs = 120000;
constexpr uint32 MaxPct = 100;
constexpr uint32 MaxPerMinute = 600;  // fakes a minute
constexpr uint32 MaxFullSkill = 100;  // 100: no bot below full ability
constexpr uint32 MaxSwitch = 1;
constexpr uint32 MaxYards = 100;

constexpr std::array<FakeRow, 20> FakeRows = {{
    {"KnownFromSkill", &FakeConfig::KnownFromSkill, 0, MaxFullSkill},
    {"MinCastMs", &FakeConfig::MinCastMs, 0, MaxMs},
    {"CancelPctAtPatience0", &FakeConfig::CancelPctAtPatience0, 0, MaxPct},
    {"CancelPctAtPatience100", &FakeConfig::CancelPctAtPatience100, 0, MaxPct},
    {"CancelSigmaPct", &FakeConfig::CancelSigmaPct, 0, MaxPct},
    {"CancelMinMs", &FakeConfig::CancelMinMs, 0, MaxMs},
    {"CancelMinLeftMs", &FakeConfig::CancelMinLeftMs, 0, MaxMs},
    {"RepeatMsAtPatience0", &FakeConfig::RepeatMsAtPatience0, 0, MaxMs},
    {"RepeatMsAtPatience100", &FakeConfig::RepeatMsAtPatience100, 0, MaxMs},
    {"RepeatJitterPct", &FakeConfig::RepeatJitterPct, 0, MaxPct},
    {"MaxPerMinute", &FakeConfig::MaxPerMinute, 0, MaxPerMinute},
    {"KickerRangeYards", &FakeConfig::KickerRangeYards, 0, MaxYards},
    {"KickerRangeSlackYards", &FakeConfig::KickerRangeSlackYards, 0, MaxYards},
    {"KickerNeedFacing", &FakeConfig::KickerNeedFacing, 0, MaxSwitch},
    {"KickSeenMs", &FakeConfig::KickSeenMs, 0, MaxMs},
    {"KickMemoryFromSkill", &FakeConfig::KickMemoryFromSkill, 0, MaxFullSkill},
    {"ChancePctAtSkill70", &FakeConfig::ChancePctAtSkill70, 0, MaxPct},
    {"ChancePctAtSkill99", &FakeConfig::ChancePctAtSkill99, 0, MaxPct},
    {"BaitedWindowMs", &FakeConfig::BaitedWindowMs, 0, MaxMs},
    {"HealGuardPct", &FakeConfig::HealGuardPct, 0, MaxPct},
}};
}  // namespace

ConfigFamily<FakeConfig> const FakeFamily{"Playerbots.Tactics.Fake.", FakeRows};

FakeConfig& FakeConfigStorage()
{
    static FakeConfig values;
    return values;
}
