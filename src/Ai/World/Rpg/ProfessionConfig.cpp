/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ProfessionConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using Row = ConfigRow<ProfessionConfig>;

constexpr std::array<Row, 4> ProfessionRows = {{
    {"Learn.GathererLevel", &ProfessionConfig::GathererLevel, 1, 80},
    {"Craft.VisitDeadlineSeconds", &ProfessionConfig::CraftVisitDeadlineSeconds, 60, 3600},
    {"Fish.TripDeadlineSeconds", &ProfessionConfig::FishTripDeadlineSeconds, 60, 3600},
    {"VisitStepSeconds", &ProfessionConfig::VisitStepSeconds, 1, 60},
}};
}  // namespace

ConfigFamily<ProfessionConfig> const ProfessionFamily{"Playerbots.Professions.", ProfessionRows};
