/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONLINETYPES_H
#define _PLAYERBOT_AUCTIONLINETYPES_H

#include <cstdint>

// The plain types of a buy line (AuctionBuyList.h) and its ceiling (AuctionPricing.h), the part an auctioneer visit's
// state holds (BankMailState.h AuctionVisitData::gearLines), so that PlayerbotAI.h does not see the rules.
namespace AuctionRules
{
// Worth and getting in the planner's value units (MaterialRules.h: Cost::value = copper x factor); the ceiling is
// the lesser as copper at the bot's need factor, per unit. No use: 0; no way of getting it: the worth alone
struct CeilingInput
{
    double worthValue{0.0};     // what a unit is worth to the bot; 0 no use
    double gettingValue{-1.0};  // a vendor's price x factor, or its own getting cost; < 0 no way
    float factor{0.0f};         // Need.factor
};

// Who wants a line; the list is funded in this order. Path (the skill path's materials and recipes, WA7) and Gear
// (gear WG7: upgrades listed at the house) are declared for their later owners
enum class LineOwner : std::uint8_t
{
    Use,  // item use's demand (ItemDemandRules.h DemandLine: buyEntry, buyUnits, WorthEachOf)
    Bag,  // a bag by its worth (ItemWorthRules.h BagWorth)
    Path,
    Gear
};

// One need line: units of an item, worth and getting per unit in the planner's value units (copper x factor)
struct BuyLine
{
    std::uint32_t item{0};
    LineOwner owner{LineOwner::Use};
    std::uint32_t units{0};
    CeilingInput ceiling;
    std::uint64_t excessUnitWorth{0};  // copper: a unit beyond the line, its vendor payout or keep worth
    std::uint32_t auctionId{0};        // the one auction the line takes; 0 = any offer of the entry
};
}  // namespace AuctionRules

#endif
