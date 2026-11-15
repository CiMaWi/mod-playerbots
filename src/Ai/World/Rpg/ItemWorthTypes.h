/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMWORTHTYPES_H
#define _PLAYERBOT_ITEMWORTHTYPES_H

#include <array>
#include <cstdint>

// The level bands' slot worth defaults (ItemWorthRules.h), the part WorldTravelConfig.h holds.
namespace WorldTravel
{
constexpr uint32_t LEVEL_BAND_COUNT = 5;

// The defaults of one level band, used until a bot has measured its own numbers
struct SlotBand
{
    uint32_t LootSlotsPerHour = 0;     // r: looted items per hour that need a new slot
    uint32_t TripMinutes = 0;          // t: minutes per inventory trip
    uint32_t EarnCopperPerMinute = 0;  // e: what a minute of the bot's time is worth
    uint32_t BagHorizonHours = 0;      // the hours before the bot would replace a bag it buys now
};

using SlotBandTable = std::array<SlotBand, LEVEL_BAND_COUNT>;

constexpr SlotBandTable DEFAULT_SLOT_BANDS = {{
    {15, 10, 30, 20},
    {20, 10, 100, 30},
    {25, 8, 250, 40},
    {25, 8, 500, 40},
    {30, 6, 1000, 100},
}};
}  // namespace WorldTravel

#endif
