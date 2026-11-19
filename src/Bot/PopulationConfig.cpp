/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PopulationConfig.h"

#include <array>
#include <limits>

#include "ConfigTable.h"

namespace
{
using PopulationRow = ConfigRow<PopulationOptions>;

constexpr double Unbounded = std::numeric_limits<double>::infinity();

constexpr std::array<PopulationRow, 6> RotationRows = {{
    {"RosterSize", &PopulationOptions::RosterSize, 0, Unbounded, ConfigKind::TuningListed},
    {"IntervalSeconds", &PopulationOptions::IntervalSeconds, 10, Unbounded, ConfigKind::TuningListed},
    {"MeanSessionMinutes", &PopulationOptions::MeanSessionMinutes, 1, Unbounded, ConfigKind::TuningListed},
    {"MinSessionMinutes", &PopulationOptions::MinSessionMinutes, 0, Unbounded, ConfigKind::TuningListed},
    {"AcquaintedSharePct", &PopulationOptions::AcquaintedSharePct, 0, 100},
    {"AcquaintedMinScore", &PopulationOptions::AcquaintedMinScore, 1, Unbounded},
}};

constexpr std::array<PopulationRow, 4> RetireRows = {{
    {"IntervalSeconds", &PopulationOptions::RetireIntervalSeconds, 10, Unbounded, ConfigKind::TuningListed},
    {"MaxPerPass", &PopulationOptions::RetireMaxPerPass, 0, Unbounded, ConfigKind::TuningListed},
    {"MaxPerHour", &PopulationOptions::RetireMaxPerHour, 0, Unbounded, ConfigKind::TuningListed},
    {"TestTarget", &PopulationOptions::RetireTestTarget, 0, Unbounded, ConfigKind::Test},
}};

// One family for the one key: the prefix is the whole key and the row's key is empty
constexpr std::array<PopulationRow, 1> FollowRows = {{
    {"", &PopulationOptions::FollowTargets, 0, 0, ConfigKind::Role},
}};
}  // namespace

ConfigFamily<PopulationOptions> const PopulationRotationFamily{"Playerbots.Population.Rotation.", RotationRows};
ConfigFamily<PopulationOptions> const PopulationRetireFamily{"Playerbots.Population.Retire.", RetireRows};
ConfigFamily<PopulationOptions> const PopulationFollowFamily{"Playerbots.Population.FollowTargets", FollowRows};
