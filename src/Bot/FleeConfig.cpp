/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FleeConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using FleeRow = ConfigRow<FleeConfig>;

constexpr std::array<FleeRow, 3> FleeRows = {{
    {"EscapeYards", &FleeConfig::EscapeYards, 1, 2000},
    {"CorneredSeconds", &FleeConfig::CorneredSeconds, 1, 600},
    {"BoundSeconds", &FleeConfig::BoundSeconds, 1, 600},
}};
}  // namespace

ConfigFamily<FleeConfig> const FleeFamily{"Playerbots.Flee.", FleeRows};
