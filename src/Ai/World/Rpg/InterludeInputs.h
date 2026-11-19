/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INTERLUDEINPUTS_H
#define PLAYERBOTS_INTERLUDEINPUTS_H

#include <cstdint>
#include <vector>

#include "GoalModel.h"

// The live Situation's errand inputs (planning WT63c): the nearest vendor and repairer with their walk, and the bag
// offers, from what the bot's map thread found in the planner's indexes (PlannerService SituationOf). Pure
namespace Planner::Goals
{
// A vendor that trades with the bot, nearest first
struct VendorSeen
{
    std::uint32_t entry{0};
    float yards{0.0f};
    bool repairs{false};
};

// A bag on a bag vendor's list that trades with the bot
struct BagSeen
{
    std::uint32_t item{0};
    std::uint32_t vendor{0};
    float yards{0.0f};
    std::uint32_t price{0};
    std::uint32_t slots{0};
};

// The first vendor and the first that repairs; each bag larger than the smallest the bot carries (0: a bag slot is
// empty) within the errand walk limit, as an offer that replaces that bag
inline void FillErrandInputs(Situation& situation, std::vector<VendorSeen> const& vendors,
                             std::vector<BagSeen> const& bags, float runSpeed, std::uint32_t smallestBag)
{
    for (VendorSeen const& vendor : vendors)
    {
        float const minutes = WorldTravel::WalkMinutes(vendor.yards, runSpeed);
        if (situation.bags.vendor.walkMinutes < 0.0f)
            situation.bags.vendor = {vendor.entry, minutes, vendor.repairs};
        if (vendor.repairs && situation.bags.repairer.walkMinutes < 0.0f)
            situation.bags.repairer = {vendor.entry, minutes, true};
    }
    for (BagSeen const& bag : bags)
    {
        float const minutes = WorldTravel::WalkMinutes(bag.yards, runSpeed);
        if (bag.slots > smallestBag && minutes <= WorldTravel::ERRAND_MAX_WALK_MINUTES)
            situation.bagOffers.push_back({bag.item, bag.vendor, minutes, bag.price, bag.slots, smallestBag});
    }
}
}  // namespace Planner::Goals

#endif
