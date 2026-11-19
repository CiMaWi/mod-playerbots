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
#include "WorldTravelRules.h"

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

// An innkeeper the bot can use, as its map thread found it in the planner's index (PlannerService InnsNear), nearest
// first (planning WT63d)
struct InnSeen
{
    std::uint32_t entry{0};  // the innkeeper's creature entry: the hearth goal's target
    Place place;             // the bind spot in front of him (WorldTravel::INN_FRONT_YARDS): where the trip walks
    float yards{0.0f};
    bool fits{false};  // its zone's level bracket fits the bot's level
    std::uint32_t zoneId{0};
};

// The inns the bind weighs (the planner's AddHearth, the bind entry's pick): each seen inn but a spot where a bind of
// the bot found no usable innkeeper lately (WorldTravel::InnAvoided); the usual inn is the one the bot is bound at
// (its bind lies within INN_REACH_YARDS of the spot: the bind trip binds that close)
inline std::vector<Inn> InnInputs(std::vector<InnSeen> const& seen, Place const& bind,
                                  std::vector<WorldTravel::AvoidedSpot> const& avoid, std::uint32_t nowMs)
{
    std::vector<Inn> inns;
    inns.reserve(seen.size());
    for (InnSeen const& inn : seen)
    {
        WorldTravel::SightPoint const spot{inn.place.map, inn.place.x, inn.place.y, inn.place.z};
        if (!avoid.empty() && WorldTravel::InnAvoided(avoid, nowMs, spot))
            continue;
        bool const usual = bind.map == inn.place.map &&
                           std::hypot(bind.x - inn.place.x, bind.y - inn.place.y) <= WorldTravel::INN_REACH_YARDS;
        inns.push_back({inn.entry, inn.place, inn.yards, inn.fits, false, usual});
    }
    return inns;
}

// The bind entry's inn (planning 3.19 (3.3)): the order's innkeeper when it names one, else the nearest inn; either
// fitting the level within maxYards. -1: none
inline int PickBindInn(std::vector<Inn> const& inns, std::uint32_t innkeeper, float maxYards)
{
    std::vector<WorldTravel::InnCandidate> candidates;
    candidates.reserve(inns.size());
    for (Inn const& inn : inns)
        candidates.push_back({inn.yards, inn.fits && (!innkeeper || inn.id == innkeeper)});
    return WorldTravel::PickInn(candidates, maxYards);
}
}  // namespace Planner::Goals

#endif
