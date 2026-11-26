/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GEARWORTHRULES_H
#define _PLAYERBOT_GEARWORTHRULES_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "AuctionBuyList.h"
#include "ItemWorthRules.h"

// What an upgrade is worth to a bot (gear plan WG4, PLAN 3.2, WG.4): the time it saves, at what the bot earns in that
// time, worth = gain x fight share x earn rate x hours worn, in copper at the neutral exchange; the same family of
// rule as bags (WorldTravel::BagWorth) and riding ranks (UpkeepNeeds). One worth for the vendor and the auctioneer
// (R3); what the bot pays at most follows from it by the auction house's ceiling (CeilingOf). Pure and std only:
// tests/gear-worth-selftest.cpp checks this code
namespace GearWorth
{
// The share of played time in which gear strength matters: quest, grind, camp and gather time on night
// lab5-20261125-175728 (54 % + 3 %; do_quest alone 58 %). Strict combat time is about 12 %
constexpr float FIGHT_SHARE = 0.6f;
// An item is outgrown once its item level is this many levels below the bot's: the same night's upgrades replaced
// items a median 2 to 4 levels below the bot (bands 10-19 to 50-59)
constexpr std::int64_t OUTGROWN_LEVELS = 3;
// At the level cap, and at most below it, an item is worn this many hours (UpkeepNeeds::MAX_LEVEL_HORIZON_HOURS)
constexpr double CAP_HORIZON_HOURS = 100.0;
constexpr std::uint32_t LEVEL_CAP = 80;

// Scores are StatsWeightCalculator's after SetUpForEquipCompare, every item with its suffix (R2)
struct WorthInput
{
    float itemScore{0.0f};
    float wornScore{0.0f};            // the item or items it replaces; <= 0 an empty slot
    float summedScore{0.0f};          // the positive scores of everything the bot would then wear, the item included
    std::uint32_t level{0};           // the bot's
    std::uint32_t itemLevel{0};       // ItemTemplate::ItemLevel
    float earnCopperPerMinute{0.0f};  // the band's e (WorldTravel::SlotRates)
    float hoursPerLevel{0.0f};        // the band's play hours per level
};

inline bool Usable(float value) { return std::isfinite(value) && value > 0.0f; }

// The share by which the item raises the bot's gear strength, in [0, 1]; a worn item scoring below 0 counts as none
inline double Gain(WorthInput const& in)
{
    if (!std::isfinite(in.itemScore) || !std::isfinite(in.wornScore) || !Usable(in.summedScore))
        return 0.0;

    double const added = static_cast<double>(in.itemScore) - std::max(static_cast<double>(in.wornScore), 0.0);
    if (!(added > 0.0))
        return 0.0;
    return std::min(added / static_cast<double>(in.summedScore), 1.0);
}

// The hours until the bot outgrows the item: the levels until its item level is OUTGROWN_LEVELS below the bot's,
// times the band's hours per level, at most CAP_HORIZON_HOURS; CAP_HORIZON_HOURS at the level cap
inline double HoursWorn(WorthInput const& in)
{
    if (in.level >= LEVEL_CAP)
        return CAP_HORIZON_HOURS;
    if (!Usable(in.hoursPerLevel))
        return 0.0;

    std::int64_t const levels = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(in.itemLevel) + OUTGROWN_LEVELS - static_cast<std::int64_t>(in.level));
    return std::min(static_cast<double>(levels) * static_cast<double>(in.hoursPerLevel), CAP_HORIZON_HOURS);
}

// The upgrade's worth in copper, below WorldTravel::WORTH_PRICELESS; 0 without a gain, an earn rate or hours, and
// for any input that is not a finite number
inline std::uint32_t Worth(WorthInput const& in)
{
    if (!Usable(in.earnCopperPerMinute) || !std::isfinite(in.hoursPerLevel))
        return 0;

    double const worth = Gain(in) * static_cast<double>(FIGHT_SHARE) * static_cast<double>(in.earnCopperPerMinute) *
                         static_cast<double>(WorldTravel::MINUTES_PER_HOUR) * HoursWorn(in);
    if (!(worth > 0.0))
        return 0;
    return static_cast<std::uint32_t>(
        std::min(std::floor(worth), static_cast<double>(WorldTravel::WORTH_PRICELESS - 1)));
}

// An upgrade's buy line, priced as a bag's (AH.5): the worth over the bot's share, at most the price of a vendor that
// stocks it (vendorCopper 0: none does)
inline AuctionRules::BuyLine GearLine(std::uint32_t item, std::uint32_t worthCopper, std::uint32_t vendorCopper,
                                      AuctionRules::CopperWeight weight)
{
    float const share = AuctionRules::PriceShare(weight);
    return AuctionRules::LineOf(AuctionRules::LineOwner::Gear, item, 1, worthCopper,
                                vendorCopper ? static_cast<double>(vendorCopper) * share : -1.0, share, 0);
}

// What the bot pays at most for the upgrade, at the vendor and the auctioneer alike: its gear line's ceiling
inline std::uint32_t CeilingOf(std::uint32_t worthCopper, std::uint32_t vendorCopper, AuctionRules::CopperWeight weight)
{
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(AuctionRules::CeilingOf(GearLine(0, worthCopper, vendorCopper, weight)),
                                std::numeric_limits<std::uint32_t>::max()));
}
}  // namespace GearWorth

#endif
