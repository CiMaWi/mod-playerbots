/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaTeamConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using ArenaTeamRow = ConfigRow<ArenaTeamConfig>;

constexpr std::array<ArenaTeamRow, 28> ArenaTeamRows = {{
    {"Seed.Enable", &ArenaTeamConfig::SeedEnable, 0, 0, ConfigKind::Test},
    {"Seed.Spread", &ArenaTeamConfig::SeedSpread, 0, 3000, ConfigKind::Test},
    {"Seed.NoiseSd", &ArenaTeamConfig::SeedNoiseSd, 0, 1000, ConfigKind::Test},
    {"Formation.Enable", &ArenaTeamConfig::FormationEnable, 0, 0, ConfigKind::Test},
    {"UpdateSeconds", &ArenaTeamConfig::UpdateSeconds, 5, 3600, ConfigKind::TuningListed},
    {"MaxRoster2v2", &ArenaTeamConfig::MaxRoster2v2, 2, 4},
    {"MaxRoster3v3", &ArenaTeamConfig::MaxRoster3v3, 3, 6},
    {"MaxRoster5v5", &ArenaTeamConfig::MaxRoster5v5, 5, 10},
    {"MinMembers", &ArenaTeamConfig::MinMembers, 2, 10},
    {"RandomPair", &ArenaTeamConfig::RandomPair, 0, 0},
    {"RepickEachQueue", &ArenaTeamConfig::RepickEachQueue, 0, 0},
    {"LossStreakToConsider", &ArenaTeamConfig::LossStreakToConsider, 1, 50, ConfigKind::TuningListed},
    {"LeaveChanceBottom", &ArenaTeamConfig::LeaveChanceBottom, 0, 100, ConfigKind::TuningListed},
    {"LeaveChanceTop", &ArenaTeamConfig::LeaveChanceTop, 0, 100, ConfigKind::TuningListed},
    {"MinTeamStaySeconds", &ArenaTeamConfig::MinTeamStaySeconds, 0, 86400, ConfigKind::TuningListed},
    {"ToleranceBottom", &ArenaTeamConfig::ToleranceBottom, 1, 98},
    {"ToleranceTop", &ArenaTeamConfig::ToleranceTop, 1, 98},
    {"SynergyWeight", &ArenaTeamConfig::SynergyWeight, 0, 1},
    {"RecruitsPerPass", &ArenaTeamConfig::RecruitsPerPass, 0, 100},
    {"RecruitAboveMinChance", &ArenaTeamConfig::RecruitAboveMinChance, 0, 100, ConfigKind::TuningListed},
    {"CompLearning.Enable", &ArenaTeamConfig::CompLearningEnable, 0, 0, ConfigKind::TuningListed},
    {"CompLearning.File", &ArenaTeamConfig::CompLearningFile, 0, 0},
    {"CompLearning.WeightBottom", &ArenaTeamConfig::CompLearningWeightBottom, 0, 1},
    {"CompLearning.WeightTop", &ArenaTeamConfig::CompLearningWeightTop, 0, 1},
    {"CompLearning.ExploreBottom", &ArenaTeamConfig::CompLearningExploreBottom, 0, 2},
    {"CompLearning.ExploreTop", &ArenaTeamConfig::CompLearningExploreTop, 0, 2},
    {"CompLearning.TemperatureBottom", &ArenaTeamConfig::CompLearningTemperatureBottom, 0.01, 10},
    {"CompLearning.TemperatureTop", &ArenaTeamConfig::CompLearningTemperatureTop, 0.01, 10},
}};

}  // namespace

ConfigFamily<ArenaTeamConfig> const ArenaTeamFamily{"Playerbots.ArenaTeam.", ArenaTeamRows};

ArenaTeamConfig& ArenaTeamConfigStorage()
{
    static ArenaTeamConfig values;
    return values;
}
