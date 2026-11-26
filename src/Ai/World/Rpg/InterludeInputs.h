/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INTERLUDEINPUTS_H
#define PLAYERBOTS_INTERLUDEINPUTS_H

#include <algorithm>
#include <cstdint>
#include <vector>

#include "FailedAttemptRules.h"
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

// An inn whose bind would save nothing: the bot's bind is not far from its spot (BindIsFar, NE3), whether the bot
// bound at this innkeeper, a few yards off his spot, or at another inn of the town
inline bool UsualInn(Place const& bind, Place const& spot)
{
    return !WorldTravel::BindIsFar(bind.map == spot.map, std::hypot(bind.x - spot.x, bind.y - spot.y));
}

// The live entry of the record of failed attempts that refuses an inn's spot: any place entry within PLACE_YARDS (an
// inn trip's not_bound, no_route, unreachable_poi, trip_deaths, inn_no_walk end, or another goal's place there).
// nullptr: none
inline FailedAttempts::Entry const* InnRefusal(std::vector<FailedAttempts::Entry> const& live, Place const& spot)
{
    WorldTravel::SightPoint const point{spot.map, spot.x, spot.y, spot.z};
    auto const it = std::find_if(live.begin(), live.end(),
                                 [&point](FailedAttempts::Entry const& entry)
                                 {
                                     return entry.key.kind == FailedAttempts::Kind::Place &&
                                            FailedAttempts::Rules::Near(entry.key.from, point);
                                 });
    return it == live.end() ? nullptr : &*it;
}

// NR.5: an inn the bot is near (within INN_BIND_FAR_YARDS, as UsualInn reads it) and since when, without a break
struct InnStay
{
    std::uint32_t entry{0};
    Place place;
    std::uint32_t sinceMs{0};
    bool told{false};  // its reaching the dwell was told to the planner (StayRipened)
};

// A stay ends when the bot is farther from the inn, or on another map
inline void DropLeftStays(std::vector<InnStay>& stays, Place const& at)
{
    std::erase_if(stays, [&at](InnStay const& stay) { return !UsualInn(at, stay.place); });
}

inline InnStay const* FindStay(std::vector<InnStay> const& stays, std::uint32_t entry)
{
    auto const it =
        std::find_if(stays.begin(), stays.end(), [entry](InnStay const& stay) { return stay.entry == entry; });
    return it == stays.end() ? nullptr : &*it;
}

// The stays at the bot's place now: the left ones end, a seen inn that came near starts one
inline void NoteInnStays(std::vector<InnStay>& stays, std::vector<InnSeen> const& seen, Place const& at,
                         std::uint32_t nowMs)
{
    DropLeftStays(stays, at);
    for (InnSeen const& inn : seen)
        if (UsualInn(at, inn.place) && !FindStay(stays, inn.entry))
            stays.push_back({inn.entry, inn.place, nowMs});
}

// Marks the inns the bot has not stayed near for the dwell (0: no dwell, none is marked)
inline void MarkUnstayed(std::vector<Inn>& inns, std::vector<InnStay> const& stays, std::uint32_t nowMs,
                         std::uint32_t dwellMs)
{
    for (Inn& inn : inns)
    {
        InnStay const* stay = FindStay(stays, inn.id);
        inn.unstayed = dwellMs && (!stay || nowMs - stay->sinceMs < dwellMs);
    }
}

// True when a stay reached the dwell since the last look: once per stay
inline bool StayRipened(std::vector<InnStay>& stays, std::uint32_t nowMs, std::uint32_t dwellMs)
{
    bool ripened = false;
    for (InnStay& stay : stays)
        if (dwellMs && !stay.told && nowMs - stay.sinceMs >= dwellMs)
            ripened = stay.told = true;
    return ripened;
}

// The inns the bind weighs (the planner's AddHearth, the bind entry's pick): each seen inn but one an entry of the
// record of failed attempts refuses (InnRefusal; live is a plain copy of its live entries), marked usual by UsualInn
inline std::vector<Inn> InnInputs(std::vector<InnSeen> const& seen, Place const& bind,
                                  std::vector<FailedAttempts::Entry> const& live)
{
    std::vector<Inn> inns;
    inns.reserve(seen.size());
    for (InnSeen const& inn : seen)
        if (!InnRefusal(live, inn.place))
            inns.push_back({inn.entry, inn.place, inn.yards, inn.fits, UsualInn(bind, inn.place)});
    return inns;
}

// The bind entry's inn (planning 3.19 (3.3)): the order's innkeeper when it names one, else the nearest inn; either
// fitting the level within maxYards, never a usual one. -1: none
inline int PickBindInn(std::vector<Inn> const& inns, std::uint32_t innkeeper, float maxYards)
{
    std::vector<WorldTravel::InnCandidate> candidates;
    candidates.reserve(inns.size());
    for (Inn const& inn : inns)
        candidates.push_back({inn.yards, !inn.usual && inn.fits && (!innkeeper || inn.id == innkeeper)});
    return WorldTravel::PickInn(candidates, maxYards);
}
}  // namespace Planner::Goals

#endif
