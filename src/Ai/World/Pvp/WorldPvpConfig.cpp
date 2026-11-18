/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldPvpConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using WorldPvpRow = ConfigRow<WorldPvpConfig>;
using EnthusiasmRow = ConfigRow<WorldPvpEnthusiasmConfig>;

constexpr uint32_t MaxRadiusYards = 2000;
constexpr uint32_t DayInSeconds = 86400;
constexpr float MaxPoints = 1000.0f;
constexpr float MaxWeight = 100.0f;

constexpr std::array<WorldPvpRow, 5> WorldPvpRows = {{
    {"ScanYards", &WorldPvpConfig::ScanYards, 1, 200},
    {"GraveyardYards", &WorldPvpConfig::GraveyardYards, 0, MaxRadiusYards},
    {"GhostEnemyYards", &WorldPvpConfig::GhostEnemyYards, 0, MaxRadiusYards},
    {"GhostRecheckSeconds", &WorldPvpConfig::GhostRecheckSeconds, 1, 600, ConfigKind::TuningListed},
    {"GhostSpiritHealerSeconds", &WorldPvpConfig::GhostSpiritHealerSeconds, 0, DayInSeconds, ConfigKind::TuningListed},
}};

// A float value out of range keeps the default (KeepDefault, as the hand-written read did); a whole number clamps
constexpr std::array<EnthusiasmRow, 34> EnthusiasmRows = {{
    {"Max", &WorldPvpEnthusiasmConfig::Max, 1.0f, MaxPoints, ConfigKind::Tuning, ConfigOutOfRange::KeepDefault},
    {"RecoveryPerHour", &WorldPvpEnthusiasmConfig::RecoveryPerHour, 0.0f, MaxPoints, ConfigKind::TuningListed,
     ConfigOutOfRange::KeepDefault},
    {"DepletionPerMinute", &WorldPvpEnthusiasmConfig::DepletionPerMinute, 0.1f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"DeathEdit", &WorldPvpEnthusiasmConfig::DeathEdit, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"DeathEditConfident", &WorldPvpEnthusiasmConfig::DeathEditConfident, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"DeathEditUnconfident", &WorldPvpEnthusiasmConfig::DeathEditUnconfident, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"KillEdit", &WorldPvpEnthusiasmConfig::KillEdit, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"GroupDeathYards", &WorldPvpEnthusiasmConfig::GroupDeathYards, 0, MaxRadiusYards},
    {"GroupDeathShare", &WorldPvpEnthusiasmConfig::GroupDeathShare, 0.0f, 1.0f, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"StartCost", &WorldPvpEnthusiasmConfig::StartCost, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"StartCostShare", &WorldPvpEnthusiasmConfig::StartCostShare, 0, 100, ConfigKind::TuningListed},
    {"JoinBase", &WorldPvpEnthusiasmConfig::JoinBase, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"JoinAggression", &WorldPvpEnthusiasmConfig::JoinAggression, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"AggressionJoinFactor", &WorldPvpEnthusiasmConfig::AggressionJoinFactor, 0.0f, 1.0f, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"JoinGreen", &WorldPvpEnthusiasmConfig::JoinGreen, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"JoinFactionAttacked", &WorldPvpEnthusiasmConfig::JoinFactionAttacked, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"JoinFactionHelpfulness", &WorldPvpEnthusiasmConfig::JoinFactionHelpfulness, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"JoinGroupAttacked", &WorldPvpEnthusiasmConfig::JoinGroupAttacked, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"JoinGraveyard", &WorldPvpEnthusiasmConfig::JoinGraveyard, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"JoinBalanceMax", &WorldPvpEnthusiasmConfig::JoinBalanceMax, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"FleeBase", &WorldPvpEnthusiasmConfig::FleeBase, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"FleeCaution", &WorldPvpEnthusiasmConfig::FleeCaution, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"FleeGroupHold", &WorldPvpEnthusiasmConfig::FleeGroupHold, 0.0f, MaxPoints, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"GroupHoldYards", &WorldPvpEnthusiasmConfig::GroupHoldYards, 0, MaxRadiusYards},
    {"BalanceYards", &WorldPvpEnthusiasmConfig::BalanceYards, 1, MaxRadiusYards},
    {"Weight.Grey", &WorldPvpEnthusiasmConfig::WeightGrey, 0.0f, MaxWeight, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"Weight.Green", &WorldPvpEnthusiasmConfig::WeightGreen, 0.0f, MaxWeight, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"Weight.Yellow", &WorldPvpEnthusiasmConfig::WeightYellow, 0.0f, MaxWeight, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"Weight.Orange", &WorldPvpEnthusiasmConfig::WeightOrange, 0.0f, MaxWeight, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"Weight.Red", &WorldPvpEnthusiasmConfig::WeightRed, 0.0f, MaxWeight, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"DepletionFactorLead", &WorldPvpEnthusiasmConfig::DepletionFactorLead, 0.01f, 1.0f, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"DepletionFactorBehind", &WorldPvpEnthusiasmConfig::DepletionFactorBehind, 1.0f, MaxWeight, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"BalanceLead", &WorldPvpEnthusiasmConfig::BalanceLead, 0.51f, 1.0f, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
    {"BalanceBehind", &WorldPvpEnthusiasmConfig::BalanceBehind, 0.0f, 0.49f, ConfigKind::Tuning,
     ConfigOutOfRange::KeepDefault},
}};
}  // namespace

ConfigFamily<WorldPvpConfig> const WorldPvpFamily{"Playerbots.WorldPvp.", WorldPvpRows};
ConfigFamily<WorldPvpEnthusiasmConfig> const WorldPvpEnthusiasmFamily{"Playerbots.WorldPvp.Enthusiasm.",
                                                                      EnthusiasmRows};
