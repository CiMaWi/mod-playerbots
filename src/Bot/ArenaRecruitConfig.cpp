/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaRecruitConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using ArenaRecruitRow = ConfigRow<ArenaRecruitConfig>;

constexpr std::array<ArenaRecruitRow, 8> ArenaRecruitRows = {{
    {"Enable", &ArenaRecruitConfig::Enable, 0, 0, ConfigKind::Test},
    {"RatingAtSkill1", &ArenaRecruitConfig::RatingAtSkill1, 0, 10000},
    {"RatingAtSkill99", &ArenaRecruitConfig::RatingAtSkill99, 0, 10000},
    {"DeltaBase", &ArenaRecruitConfig::DeltaBase, 0, 10000},
    {"DeltaPerAcquaintancePoint", &ArenaRecruitConfig::DeltaPerAcquaintancePoint, 0, 1000},
    {"DeltaMax", &ArenaRecruitConfig::DeltaMax, 0, 10000},
    {"LoyaltyMinGames", &ArenaRecruitConfig::LoyaltyMinGames, 1, 1000},
    {"LoyaltyMinWinPercent", &ArenaRecruitConfig::LoyaltyMinWinPercent, 0, 100},
}};

}  // namespace

ConfigFamily<ArenaRecruitConfig> const ArenaRecruitFamily{"Playerbots.ArenaRecruit.", ArenaRecruitRows};
