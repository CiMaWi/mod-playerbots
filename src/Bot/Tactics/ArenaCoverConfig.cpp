/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaCoverConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using LosRow = ConfigRow<LosConfig>;
using PillarRow = ConfigRow<PillarConfig>;

// The caps of the old reads (a larger value is capped, now with a warning)
constexpr uint32 MaxMs = 120000;
constexpr uint32 MaxPct = 100;
constexpr uint32 MaxSkill = 99;    // a skill, 0-99
constexpr uint32 MaxCount = 1000;  // rays, relocations, yards

constexpr std::array<LosRow, 14> LosRows = {{
    {"KnownFromSkill", &LosConfig::KnownFromSkill, 0, MaxSkill},
    {"LateMsAtSkill1", &LosConfig::LateMsAtSkill1, 0, MaxMs},
    {"LateMsAtSkill99", &LosConfig::LateMsAtSkill99, 0, MaxMs},
    {"IntoOpenPctAtSkill1", &LosConfig::IntoOpenPctAtSkill1, 0, MaxPct},
    {"IntoOpenPctAtSkill99", &LosConfig::IntoOpenPctAtSkill99, 0, MaxPct},
    {"HidesFromCasterFromSkill", &LosConfig::HidesFromCasterFromSkill, 0, MaxSkill},
    {"EffortMin", &LosConfig::EffortMin, 0, MaxPct},
    {"HoldMs", &LosConfig::HoldMs, 0, MaxMs},
    {"MaxRelocs", &LosConfig::MaxRelocs, 0, MaxCount},
    {"RelocWindowMs", &LosConfig::RelocWindowMs, 0, MaxMs},
    {"SearchIntervalMs", &LosConfig::SearchIntervalMs, 0, MaxMs},
    {"MaxRaysPerSearch", &LosConfig::MaxRaysPerSearch, 0, MaxCount},
    {"RaysPerSecond", &LosConfig::RaysPerSecond, 0, MaxCount},
    {"TimeoutMs", &LosConfig::TimeoutMs, 0, MaxMs},
}};

constexpr std::array<PillarRow, 7> PillarRows = {{
    {"KnownFromSkill", &PillarConfig::KnownFromSkill, 0, MaxSkill},
    {"HardCastMs", &PillarConfig::HardCastMs, 0, MaxMs},
    {"StepYards", &PillarConfig::StepYards, 0, MaxCount},
    {"RandomHidePctAtSkill1", &PillarConfig::RandomHidePctAtSkill1, 0, MaxPct},
    {"RandomHidePctAtSkill59", &PillarConfig::RandomHidePctAtSkill59, 0, MaxPct},
    {"CautiousEarlyMs", &PillarConfig::CautiousEarlyMs, 0, MaxMs},
    {"RvIdleMs", &PillarConfig::RvIdleMs, 0, MaxMs},
}};
}  // namespace

ConfigFamily<LosConfig> const LosFamily{"Playerbots.Tactics.Los.", LosRows};
ConfigFamily<PillarConfig> const PillarFamily{"Playerbots.Tactics.Pillar.", PillarRows};
