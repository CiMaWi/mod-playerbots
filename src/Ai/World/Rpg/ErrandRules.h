/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ERRANDRULES_H
#define _PLAYERBOT_ERRANDRULES_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "ItemWorthRules.h"

// Trips and errands (travel plan WT15, PLAN 3.11, WT.6): providers say what an errand needs (a service), where it can
// be done and what it is worth now; PlanTrip puts every errand on a stop of the trip and opens a detour only when the
// errands done there are worth more than the detour costs. Player-free: the server fills the ErrandSnapshot on the
// bot's map thread, tests/errand-trips-selftest.cpp runs the same code.
namespace WorldTravel
{
// Append-only: later plans add training (WU), mail and the bank (WB), the auction house (WA) and town visits (WC)
enum class ErrandKind : uint8_t
{
    TurnIn,
    Sell,
    Repair,
    BuyBag,
    BindHearth,
    LearnFlight
};
constexpr uint8_t ERRAND_KIND_COUNT = 6;

inline char const* ErrandKindName(ErrandKind kind)
{
    switch (kind)
    {
        case ErrandKind::TurnIn:
            return "turn_in";
        case ErrandKind::Sell:
            return "sell";
        case ErrandKind::Repair:
            return "repair";
        case ErrandKind::BuyBag:
            return "buy_bag";
        case ErrandKind::BindHearth:
            return "bind_hearth";
        case ErrandKind::LearnFlight:
            return "learn_flight";
    }
    return "other";
}

// What a stop offers (bits, append-only)
constexpr uint32_t SERVICE_QUEST_ENDER = 1u << 0;
constexpr uint32_t SERVICE_VENDOR = 1u << 1;
constexpr uint32_t SERVICE_REPAIR = 1u << 2;
constexpr uint32_t SERVICE_INNKEEPER = 1u << 3;
constexpr uint32_t SERVICE_FLIGHT_MASTER = 1u << 4;

// An errand place this close to a stop is done at that stop (a town square; quest camps cluster at 25 to 70 yd)
constexpr float STOP_JOIN_YARDS = 40.0f;
// A trip opens at most this many detours
constexpr std::size_t MAX_DETOUR_STOPS = 3;
// Repairing (the goal model's rule, WT14): worth the minutes a bot below this durability would lose to broken gear
constexpr uint8_t REPAIR_PERCENT = 20;
constexpr float REPAIR_MINUTES_SAVED = 10.0f;

struct ErrandPlace
{
    uint32_t MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

inline float Yards(ErrandPlace const& a, ErrandPlace const& b)
{
    if (a.MapId != b.MapId)
        return std::numeric_limits<float>::infinity();

    float const dx = a.X - b.X;
    float const dy = a.Y - b.Y;
    float const dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A provider's answer: what the errand needs, where it can be done and what it is worth right now (copper)
struct ErrandOffer
{
    ErrandKind Kind = ErrandKind::Sell;
    uint32_t Service = 0;
    bool Placed = false;  // done at Place only (a quest's ender, a vendor's bag); else at any stop with the service
    ErrandPlace Place;
    float WorthCopper = 0.0f;
    uint32_t Id = 0;      // the quest, the bag item, the inn or node; 0 for selling and repairing
    uint32_t Target = 0;  // the creature or object entry the errand talks to at Place; 0 = any with the service
    bool TargetIsObject = false;
};

// A place near the bot that offers services: a vendor (that may repair), an innkeeper, a flight master
struct ServiceStop
{
    uint32_t Id = 0;  // the creature entry
    ErrandPlace Place;
    uint32_t Services = 0;
};

struct CompletedQuest
{
    uint32_t QuestId = 0;
    uint32_t Ender = 0;  // creature or object entry
    bool EnderIsObject = false;
    ErrandPlace Place;  // the ender's nearest spawn
    float WorthCopper = 0.0f;
};

struct BagOfferAt
{
    uint32_t Item = 0;
    uint32_t Vendor = 0;
    ErrandPlace Place;
    uint32_t Price = 0;
    uint32_t Slots = 0;
    uint32_t ReplacedSlots = 0;  // the bag it replaces; 0 for an empty bag slot
};

struct PlaceOffer
{
    uint32_t Id = 0;
    uint32_t Target = 0;
    ErrandPlace Place;
    float WorthCopper = 0.0f;
};

// What the providers read: filled on the bot's map thread, never a Player
struct ErrandSnapshot
{
    uint32_t Level = 1;
    uint64_t Money = 0;
    SlotRates Rates;
    uint32_t BagHorizonHours = 0;
    uint32_t FreeSlots = 0;
    uint32_t SellableSlots = 0;
    uint32_t DurabilityPercent = PERCENT;
    float MinutesToNextStop = 0.0f;  // until the bot passes a vendor anyway
    uint64_t Reserve = 0;            // ShouldBuyBag's reserve
    uint32_t ThriftPercent = PERCENT;
    uint32_t BagBudgetPercent = PERCENT;
    std::vector<ServiceStop> Stops;
    std::vector<CompletedQuest> Completed;
    std::vector<BagOfferAt> Bags;
    std::vector<PlaceOffer> Inns;         // an inn worth binding at
    std::vector<PlaceOffer> FlightNodes;  // an undiscovered node worth learning
};

using ErrandProvider = void (*)(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers);

struct ProviderRow
{
    ErrandKind Kind;
    char const* Name;
    ErrandProvider Provide;
};

// Selling frees the slots for the time until the next natural stop (WT3's TripWorthIt counts the same worth)
inline float SaleWorth(SlotRates const& rates, uint32_t freeSlots, uint32_t sellableSlots, float minutesToNextStop)
{
    return SlotsWorthPerHour(rates, freeSlots, sellableSlots) * minutesToNextStop / MINUTES_PER_HOUR;
}

// The broken share of a bot below REPAIR_PERCENT durability, valued at its earning rate
inline float RepairWorth(uint32_t durabilityPercent, float earnCopperPerMinute)
{
    if (durabilityPercent >= REPAIR_PERCENT)
        return 0.0f;

    float const broken = static_cast<float>(REPAIR_PERCENT - durabilityPercent) / REPAIR_PERCENT;
    return broken * REPAIR_MINUTES_SAVED * earnCopperPerMinute;
}

inline void ProvideTurnIns(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    for (CompletedQuest const& quest : snapshot.Completed)
        offers.push_back({ErrandKind::TurnIn, SERVICE_QUEST_ENDER, true, quest.Place, quest.WorthCopper, quest.QuestId,
                          quest.Ender, quest.EnderIsObject});
}

inline void ProvideSale(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    if (!snapshot.SellableSlots)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Sell;
    offer.Service = SERVICE_VENDOR;
    offer.WorthCopper =
        SaleWorth(snapshot.Rates, snapshot.FreeSlots, snapshot.SellableSlots, snapshot.MinutesToNextStop);
    offers.push_back(offer);
}

// Paid repairs at a vendor that repairs; free repairs are the upkeep plan's (WU)
inline void ProvideRepair(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    float const worth = RepairWorth(snapshot.DurabilityPercent, snapshot.Rates.EarnCopperPerMinute);
    if (worth <= 0.0f)
        return;

    ErrandOffer offer;
    offer.Kind = ErrandKind::Repair;
    offer.Service = SERVICE_REPAIR;
    offer.WorthCopper = worth;
    offers.push_back(offer);
}

// A bag ShouldBuyBag takes; buying it now gains its slots until the next vendor stop (as the goal model's bag offer)
inline void ProvideBags(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    for (BagOfferAt const& bag : snapshot.Bags)
    {
        if (bag.Slots <= bag.ReplacedSlots)
            continue;

        BagBuyInput in;
        in.Price = bag.Price;
        in.Worth = BagWorth(snapshot.Rates, snapshot.FreeSlots, bag.Slots, bag.ReplacedSlots, snapshot.BagHorizonHours);
        in.Gold = snapshot.Money;
        in.Reserve = snapshot.Reserve;
        in.ThriftPercent = snapshot.ThriftPercent;
        in.BudgetPercent = snapshot.BagBudgetPercent;
        if (!ShouldBuyBag(in))
            continue;

        float const worth =
            SaleWorth(snapshot.Rates, snapshot.FreeSlots, bag.Slots - bag.ReplacedSlots, snapshot.MinutesToNextStop);
        offers.push_back({ErrandKind::BuyBag, SERVICE_VENDOR, true, bag.Place, worth, bag.Item, bag.Vendor, false});
    }
}

inline void ProvideHearth(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    for (PlaceOffer const& inn : snapshot.Inns)
        offers.push_back(
            {ErrandKind::BindHearth, SERVICE_INNKEEPER, true, inn.Place, inn.WorthCopper, inn.Id, inn.Target, false});
}

inline void ProvideFlightNodes(ErrandSnapshot const& snapshot, std::vector<ErrandOffer>& offers)
{
    for (PlaceOffer const& node : snapshot.FlightNodes)
        offers.push_back({ErrandKind::LearnFlight, SERVICE_FLIGHT_MASTER, true, node.Place, node.WorthCopper, node.Id,
                          node.Target, false});
}

// The travel plan's providers; a later plan adds its row to its own copy of this table, PlanTrip stays as it is
inline std::vector<ProviderRow> const& TravelProviders()
{
    static std::vector<ProviderRow> const rows = {
        {ErrandKind::TurnIn, "turn_in", &ProvideTurnIns},
        {ErrandKind::Sell, "sell", &ProvideSale},
        {ErrandKind::Repair, "repair", &ProvideRepair},
        {ErrandKind::BuyBag, "buy_bag", &ProvideBags},
        {ErrandKind::BindHearth, "bind_hearth", &ProvideHearth},
        {ErrandKind::LearnFlight, "learn_flight", &ProvideFlightNodes},
    };
    return rows;
}

inline std::vector<ErrandOffer> CollectOffers(ErrandSnapshot const& snapshot, std::vector<ProviderRow> const& rows)
{
    std::vector<ErrandOffer> offers;
    for (ProviderRow const& row : rows)
        row.Provide(snapshot, offers);
    return offers;
}

struct TripStop
{
    ErrandPlace Place;
    uint32_t Services = 0;
    uint32_t Id = 0;       // the service stop's entry; 0 for a placed errand's own place
    bool OnRoute = false;  // a stop the bot makes anyway
    float DetourMinutes = 0.0f;
    std::vector<ErrandOffer> Errands;
};

struct TripRequest
{
    ErrandPlace Start;
    std::vector<TripStop> Route;  // the stops the bot makes anyway, in order; may be empty
    float YardsPerSecond = 0.0f;
    SlotRates Rates;
};

struct Refusal
{
    ErrandOffer Offer;
    char const* Reason = "";  // detour_not_worth, too_far, no_stop
    float CostCopper = 0.0f;  // the cheapest detour it would need (detour_not_worth)
    float WalkMinutes = 0.0f;
};

struct TripPlan
{
    std::vector<TripStop> Stops;
    std::vector<Refusal> Refused;
};

inline bool StopTakes(TripStop const& stop, ErrandOffer const& offer)
{
    if (offer.Placed)
        return Yards(stop.Place, offer.Place) <= STOP_JOIN_YARDS;

    return offer.Service && (stop.Services & offer.Service) == offer.Service;
}

// The yards that passing `place` adds at the best point of the route, and that point. After the last stop the walk
// there counts once, standing for the way back (as TripWorthIt)
inline float InsertYards(ErrandPlace const& start, std::vector<TripStop> const& stops, ErrandPlace const& place,
                         std::size_t& index)
{
    index = stops.size();
    float best = Yards(stops.empty() ? start : stops.back().Place, place);
    for (std::size_t i = 0; i < stops.size(); ++i)
    {
        ErrandPlace const& from = i ? stops[i - 1].Place : start;
        float const extra = Yards(from, place) + Yards(place, stops[i].Place) - Yards(from, stops[i].Place);
        if (extra < best)
        {
            best = extra;
            index = i;
        }
    }
    return best;
}

// A detour costs a usual trip's minutes (t) plus the walk it adds, at the earning rate: TripWorthIt's cost
inline float DetourCost(SlotRates const& rates, float walkMinutes)
{
    return (rates.TripMinutes + walkMinutes) * rates.EarnCopperPerMinute;
}

// The detour rule: an errand joins a stop already on the trip when one can take it; a new stop opens only when the
// pending errands it takes are worth more than its detour, the best gain first, until no detour pays or the trip has
// MAX_DETOUR_STOPS detours. Deterministic: candidates in the order given, strict comparisons.
inline TripPlan PlanTrip(TripRequest const& request, std::vector<ServiceStop> const& known,
                         std::vector<ErrandOffer> const& offers)
{
    TripPlan plan;
    plan.Stops = request.Route;
    for (TripStop& stop : plan.Stops)
        stop.OnRoute = true;

    std::vector<bool> done(offers.size(), false);
    auto const join = [&plan, &offers, &done]()
    {
        for (std::size_t i = 0; i < offers.size(); ++i)
            for (TripStop& stop : plan.Stops)
                if (!done[i] && StopTakes(stop, offers[i]))
                {
                    stop.Errands.push_back(offers[i]);
                    done[i] = true;
                }
    };
    // The candidate places: the known service stops, then each errand's own place
    std::vector<TripStop> candidates;
    for (ServiceStop const& service : known)
        candidates.push_back({service.Place, service.Services, service.Id, false, 0.0f, {}});
    for (ErrandOffer const& offer : offers)
        if (offer.Placed)
            candidates.push_back({offer.Place, offer.Service, offer.Target, false, 0.0f, {}});

    join();
    for (std::size_t detours = 0; detours < MAX_DETOUR_STOPS; ++detours)
    {
        bool found = false;
        float bestGain = 0.0f;
        std::size_t bestCandidate = 0;
        std::size_t bestIndex = 0;
        float bestMinutes = 0.0f;
        for (std::size_t c = 0; c < candidates.size(); ++c)
        {
            float worth = 0.0f;
            bool takes = false;
            for (std::size_t i = 0; i < offers.size(); ++i)
                if (!done[i] && StopTakes(candidates[c], offers[i]))
                {
                    worth += offers[i].WorthCopper;
                    takes = true;
                }
            if (!takes)
                continue;

            std::size_t index = 0;
            float const minutes =
                WalkMinutes(InsertYards(request.Start, plan.Stops, candidates[c].Place, index), request.YardsPerSecond);
            if (!(minutes <= ERRAND_MAX_WALK_MINUTES))
                continue;

            float const cost = DetourCost(request.Rates, minutes);
            if (!(worth > cost) || (found && !(worth - cost > bestGain)))
                continue;

            found = true;
            bestGain = worth - cost;
            bestCandidate = c;
            bestIndex = index;
            bestMinutes = minutes;
        }
        if (!found)
            break;

        TripStop stop = candidates[bestCandidate];
        stop.DetourMinutes = bestMinutes;
        plan.Stops.insert(plan.Stops.begin() + static_cast<std::ptrdiff_t>(bestIndex), std::move(stop));
        join();
    }

    // What is left: no place takes it, every place is beyond the walk limit, or no detour pays
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        if (done[i])
            continue;

        Refusal refusal;
        refusal.Offer = offers[i];
        refusal.Reason = "no_stop";
        bool reachable = false;
        for (TripStop const& candidate : candidates)
        {
            if (!StopTakes(candidate, offers[i]))
                continue;

            std::size_t index = 0;
            float const minutes =
                WalkMinutes(InsertYards(request.Start, plan.Stops, candidate.Place, index), request.YardsPerSecond);
            if (!(minutes <= ERRAND_MAX_WALK_MINUTES))
            {
                if (!reachable)
                    refusal.Reason = "too_far";
                continue;
            }
            float const cost = DetourCost(request.Rates, minutes);
            if (!reachable || cost < refusal.CostCopper)
            {
                refusal.CostCopper = cost;
                refusal.WalkMinutes = minutes;
            }
            reachable = true;
            refusal.Reason = "detour_not_worth";
        }
        plan.Refused.push_back(refusal);
    }
    return plan;
}
}  // namespace WorldTravel

#endif
