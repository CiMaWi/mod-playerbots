/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaInterruptConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using Row = ConfigRow<InterruptConfig>;

// The caps of the old reads (a larger value is capped, now with a warning)
constexpr uint32 MaxPct = 100;
constexpr uint32 MaxSwitch = 1;
constexpr uint32 MaxCasts = 20;
constexpr uint32 MaxCacheMs = 5000;
constexpr uint32 MaxHoldMs = 60000;
constexpr uint32 MaxHoldCastMs = 10000;
constexpr uint32 MaxHoldMarginMs = 2000;

constexpr std::array<Row, 38> InterruptRows = {{
    {"HealSharePctAtSkill1", &InterruptConfig::HealSharePctAtSkill1, 0, MaxPct},
    {"HealSharePctAtSkill99", &InterruptConfig::HealSharePctAtSkill99, 0, MaxPct},
    {"DamageSharePct", &InterruptConfig::DamageSharePct, 0, MaxPct},
    {"CcSharePct", &InterruptConfig::CcSharePct, 0, MaxPct},
    {"MaxSharePct", &InterruptConfig::MaxSharePct, 0, MaxPct},
    {"SigmaPctAtSkill1", &InterruptConfig::SigmaPctAtSkill1, 0, MaxPct},
    {"SigmaPctAtSkill99", &InterruptConfig::SigmaPctAtSkill99, 0, MaxPct},
    {"UrgentHpPct", &InterruptConfig::UrgentHpPct, 0, MaxPct},
    {"CancelSharePct", &InterruptConfig::CancelSharePct, 0, MaxPct},
    {"CancelMinCasts", &InterruptConfig::CancelMinCasts, 0, MaxCasts},
    {"PatienceSharePct", &InterruptConfig::PatienceSharePct, 0, MaxPct},
    {"HoldForHealer", &InterruptConfig::HoldForHealer, 0, MaxSwitch},
    {"HealerCacheMs", &InterruptConfig::HealerCacheMs, 0, MaxCacheMs},
    {"ArmsPummel", &InterruptConfig::ArmsPummel, 0, MaxSwitch},
    {"SeeCancelLag", &InterruptConfig::SeeCancelLag, 0, MaxSwitch},
    {"UsePctAtSkill1", &InterruptConfig::UsePctAtSkill1, 0, MaxPct},
    {"UsePctAtSkill99", &InterruptConfig::UsePctAtSkill99, 0, MaxPct},
    {"AttentionMsAtSkill1", &InterruptConfig::AttentionMsAtSkill1, 0, MaxHoldMs},
    {"AttentionMsAtSkill99", &InterruptConfig::AttentionMsAtSkill99, 0, MaxHoldMs},
    {"HoldPolicy", &InterruptConfig::HoldPolicy, 0, MaxSwitch},
    {"HoldKnowPctAtKnowledge1", &InterruptConfig::HoldKnowPctAtKnowledge1, 0, MaxPct},
    {"HoldKnowPctAtKnowledge99", &InterruptConfig::HoldKnowPctAtKnowledge99, 0, MaxPct},
    {"HoldAssumedCastMs", &InterruptConfig::HoldAssumedCastMs, 0, MaxHoldCastMs},
    {"HoldInstantSeenMs", &InterruptConfig::HoldInstantSeenMs, 0, MaxHoldMs},
    {"HoldHpHighPct", &InterruptConfig::HoldHpHighPct, 0, MaxPct},
    {"HoldHpLowPct", &InterruptConfig::HoldHpLowPct, 0, MaxPct},
    {"HoldRecentCastHealMs", &InterruptConfig::HoldRecentCastHealMs, 0, MaxHoldMs},
    {"HoldRecentHpShiftPct", &InterruptConfig::HoldRecentHpShiftPct, 0, MaxPct},
    {"HoldInstantCoverPct", &InterruptConfig::HoldInstantCoverPct, 0, MaxPct},
    {"HoldWindowMarginMs", &InterruptConfig::HoldWindowMarginMs, 0, MaxHoldMarginMs},
    {"HoldMotivationPctAtKnowledge1", &InterruptConfig::HoldMotivationPctAtKnowledge1, 0, MaxPct},
    {"HoldMotivationPctAtKnowledge99", &InterruptConfig::HoldMotivationPctAtKnowledge99, 0, MaxPct},
    {"HoldMsAtPatience0", &InterruptConfig::HoldMsAtPatience0, 0, MaxHoldMs},
    {"HoldMsAtPatience100", &InterruptConfig::HoldMsAtPatience100, 0, MaxHoldMs},
    {"HoldResetMs", &InterruptConfig::HoldResetMs, 0, MaxHoldMs},
    {"HoldTrendHorizonMs", &InterruptConfig::HoldTrendHorizonMs, 0, MaxHoldMs},
    {"HoldTrendPctAtKnowledge1", &InterruptConfig::HoldTrendPctAtKnowledge1, 0, MaxPct},
    {"HoldTrendPctAtKnowledge99", &InterruptConfig::HoldTrendPctAtKnowledge99, 0, MaxPct},
}};
}  // namespace

ConfigFamily<InterruptConfig> const InterruptFamily{"Playerbots.Tactics.Interrupt.", InterruptRows};
