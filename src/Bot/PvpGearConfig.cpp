/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpGearConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using PvpGearRow = ConfigRow<PvpGearConfig>;

constexpr std::array<PvpGearRow, 8> PvpGearRows = {{
    {"TierOrder", &PvpGearConfig::TierOrder, 0, 0},
    {"BaseTier", &PvpGearConfig::BaseTier, 0, 0},
    {"WearBetweenMatches", &PvpGearConfig::WearBetweenMatches, 0, 0, ConfigKind::TuningListed},
    {"Buy.Enable", &PvpGearConfig::BuyEnable, 0, 0, ConfigKind::Test},
    {"Buy.SlotPriority", &PvpGearConfig::BuySlotPriority, 0, 0},
    {"Buy.SaveTowardBetter", &PvpGearConfig::BuySaveTowardBetter, 0, 0},
    {"Buy.CheckIntervalMinutes", &PvpGearConfig::BuyCheckIntervalMinutes, 1, 1440, ConfigKind::TuningListed},
    {"Buy.BatchPerTick", &PvpGearConfig::BuyBatchPerTick, 1, 100, ConfigKind::TuningListed},
}};

}  // namespace

ConfigFamily<PvpGearConfig> const PvpGearFamily{"Playerbots.PvpGear.", PvpGearRows};
