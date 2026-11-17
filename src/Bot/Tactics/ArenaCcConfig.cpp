/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaCcConfig.h"

#include <array>
#include <cstdint>

#include "ConfigTable.h"

namespace
{
using Row = ConfigRow<CcConfig>;

// The caps of the old reads (a larger value is capped, now with a warning)
constexpr uint32 MaxMs = 120000;
constexpr uint32 MaxPct = 100;
constexpr uint32 MaxSkill = 100;  // 100: no bot below full ability
constexpr uint32 MaxSwitch = 1;
constexpr uint32 MaxEvents = 20;
constexpr uint32 MaxEnsureTrinket = 2;
constexpr uint32 MaxMarginYards = 20;
constexpr uint32 MaxClassMask = UINT32_MAX;       // read whole, then Finish drops the bits of no class
constexpr uint32 ClassMaskAll = (1u << 11) - 1u;  // the 11 classes, bit class - 1

// G6d-a: a class mask keeps the classes' bits only, as the old read did
void MaskExemptClasses(CcConfig& out)
{
    out.AvoidAoeGuardExemptClasses &= ClassMaskAll;
    out.AvoidDotGuardExemptClasses &= ClassMaskAll;
}

constexpr std::array<Row, 15> CcRows = {{
    {"DrAwareFromSkill", &CcConfig::CcDrAwareFromSkill, 0, MaxSkill},
    {"ChainsFromSkill", &CcConfig::CcChainsFromSkill, 0, MaxSkill},
    {"MinExpectedMs", &CcConfig::CcMinExpectedMs, 0, MaxMs},
    {"DrMissPctAtSkill1", &CcConfig::CcDrMissPctAtSkill1, 0, MaxPct},
    {"DrMissPctAtSkill99", &CcConfig::CcDrMissPctAtSkill99, 0, MaxPct},
    {"ChainLeadMsAtSkill1", &CcConfig::CcChainLeadMsAtSkill1, 0, MaxMs},
    {"ChainLeadMsAtSkill99", &CcConfig::CcChainLeadMsAtSkill99, 0, MaxMs},
    {"WrongTargetPctAtSkill1", &CcConfig::CcWrongTargetPctAtSkill1, 0, MaxPct},
    {"WrongTargetPctAtSkill99", &CcConfig::CcWrongTargetPctAtSkill99, 0, MaxPct},
    {"PatientHoldMs", &CcConfig::CcPatientHoldMs, 0, MaxMs},
    {"BehindStopPct", &CcConfig::CcBehindStopPct, 0, MaxPct},
    {"ChainEnable", &CcConfig::CcChainEnable, 0, MaxSwitch},
    {"OpponentMemoryEnable", &CcConfig::CcOpponentMemoryEnable, 0, MaxSwitch},
    {"OpponentMinEvents", &CcConfig::CcOpponentMinEvents, 0, MaxEvents},
    {"DispelMarginMs", &CcConfig::CcDispelMarginMs, 0, MaxMs},
}};

constexpr std::array<Row, 18> AvoidRows = {{
    {"OwnBreakPctAtSkill1", &CcConfig::AvoidOwnBreakPctAtSkill1, 0, MaxPct},
    {"OwnBreakPctAtSkill99", &CcConfig::AvoidOwnBreakPctAtSkill99, 0, MaxPct},
    {"OwnBreakNeverFromSkill", &CcConfig::AvoidOwnBreakNeverFromSkill, 0, MaxSkill},
    {"WastedBreakPctAtSkill1", &CcConfig::AvoidWastedBreakPctAtSkill1, 0, MaxPct},
    {"WastedBreakPctAtSkill99", &CcConfig::AvoidWastedBreakPctAtSkill99, 0, MaxPct},
    {"IgnoreRootsFromSkill", &CcConfig::AvoidIgnoreRootsFromSkill, 0, MaxSkill},
    {"MinLeftMs", &CcConfig::AvoidMinLeftMs, 0, MaxMs},
    {"LowHealthPct", &CcConfig::AvoidLowHealthPct, 0, MaxPct},
    {"WastedBreakNeverFromSkill", &CcConfig::AvoidWastedBreakNeverFromSkill, 0, MaxSkill},
    {"CautiousLowHealthPct", &CcConfig::AvoidCautiousLowHealthPct, 0, MaxPct},
    {"EnsureTrinket", &CcConfig::AvoidEnsureTrinket, 0, MaxEnsureTrinket},
    {"AoeGuardEnable", &CcConfig::AvoidAoeGuardEnable, 0, MaxSwitch},
    {"AoeGuardMarginYards", &CcConfig::AvoidAoeGuardMarginYards, 0, MaxMarginYards},
    {"DotGuardEnable", &CcConfig::AvoidDotGuardEnable, 0, MaxSwitch},
    {"AoeGuardExemptClasses", &CcConfig::AvoidAoeGuardExemptClasses, 0, MaxClassMask},
    {"DotGuardExemptClasses", &CcConfig::AvoidDotGuardExemptClasses, 0, MaxClassMask},
    {"PetCallOffEnable", &CcConfig::AvoidPetCallOffEnable, 0, MaxSwitch},
    {"ExcludeFightEnemies", &CcConfig::AvoidExcludeFightEnemies, 0, MaxSwitch},
}};
}  // namespace

ConfigFamily<CcConfig> const CcFamily{"Playerbots.Tactics.Cc.", CcRows};
ConfigFamily<CcConfig> const AvoidFamily{"Playerbots.Tactics.Avoid.", AvoidRows, &MaskExemptClasses};
