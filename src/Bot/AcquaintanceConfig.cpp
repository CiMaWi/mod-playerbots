/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcquaintanceConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using AcquaintanceRow = ConfigRow<AcquaintanceConfig>;

constexpr std::array<AcquaintanceRow, 15> AcquaintanceRows = {{
    {"FlushSeconds", &AcquaintanceConfig::FlushSeconds, 5, 3600},
    {"DirectCooldownSeconds", &AcquaintanceConfig::DirectCooldownSeconds, 0, 86400},
    {"GroupJoinCooldownSeconds", &AcquaintanceConfig::GroupJoinCooldownSeconds, 0, 86400},
    {"PointsDirect", &AcquaintanceConfig::PointsDirect, 0, 10000},
    {"PointsGroupJoin", &AcquaintanceConfig::PointsGroupJoin, 0, 10000},
    {"PointsGuildJoin", &AcquaintanceConfig::PointsGuildJoin, 0, 10000},
    {"PointsPvpKill", &AcquaintanceConfig::PointsPvpKill, 0, 10000},
    {"PointsPerGroupMinute", &AcquaintanceConfig::PointsPerGroupMinute, 0, 10000},
    {"PointsPerGuildHour", &AcquaintanceConfig::PointsPerGuildHour, 0, 10000},
    {"PointsPerNearbyMinute", &AcquaintanceConfig::PointsPerNearbyMinute, 0, 10000},
    {"MaxScore", &AcquaintanceConfig::MaxScore, 1, 2000000000},
    {"GroupSampleSeconds", &AcquaintanceConfig::GroupSampleSeconds, 1, 3600},
    {"GuildSampleSeconds", &AcquaintanceConfig::GuildSampleSeconds, 5, 3600, ConfigKind::TuningListed},
    {"NearbyIntervalSeconds", &AcquaintanceConfig::NearbyIntervalSeconds, 0, 3600},
    {"NearbyRadius", &AcquaintanceConfig::NearbyRadius, 1.0, 100.0},
}};

}  // namespace

ConfigFamily<AcquaintanceConfig> const AcquaintanceFamily{"Playerbots.Acquaintance.", AcquaintanceRows};
