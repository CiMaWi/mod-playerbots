/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GEARWORTHRULES_H
#define _PLAYERBOT_GEARWORTHRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

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

// The band's play hours per level below the cap, as night lab5-20261125-175728 measured them (report.txt
// "leveling"); 0 at the cap, which has its own horizon. 70-79's value is beyond the cap's horizon by itself
inline float HoursPerLevel(std::uint32_t level)
{
    if (level >= LEVEL_CAP)
        return 0.0f;
    if (level >= 70)
        return 353.0f;
    if (level >= 60)
        return 11.86f;
    if (level >= 50)
        return 6.97f;
    if (level >= 40)
        return 5.34f;
    if (level >= 30)
        return 4.30f;
    if (level >= 20)
        return 3.34f;
    return 1.44f;
}

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

// Which rule decides a vendor item's purchase (gear WG5, PLAN 3.2 "At a vendor")
enum class Route : std::uint8_t
{
    Old,      // the buy action's own budget and score comparison
    ByWorth,  // a candidate of the worth rule
    Never     // gear the upgrade rule does not wear, or that costs more than gold: not bought
};

struct RouteInput
{
    bool keeper{false};      // a vendor visit of a bot that keeps its possessions
    bool gear{false};        // a weapon or armour item
    bool equipUsage{false};  // "item usage" is one of the equip usages
    bool upgrade{false};     // "item upgrade" is EQUIP or REPLACE (R4)
    bool goldPrice{false};   // no extended cost
};

inline Route RouteOf(RouteInput const& in)
{
    if (!in.keeper || !in.gear || !in.equipUsage)
        return Route::Old;
    return in.upgrade && in.goldPrice ? Route::ByWorth : Route::Never;
}

// A vendor item the upgrade rule would wear, with its worth to the bot
struct Offer
{
    std::uint32_t item{0};   // entry
    std::uint8_t slot{0};    // the equipment slot it replaces
    std::uint32_t slots{0};  // one bit per equipment slot the purchase fills
    std::uint32_t worth{0};  // copper, as are the price and the ceiling
    std::uint32_t price{0};
    std::uint32_t ceiling{0};
};

constexpr std::size_t NO_OFFER = std::numeric_limits<std::size_t>::max();

// What the offer is worth above its price; below 0 for one dearer than its worth
inline std::int64_t Surplus(Offer const& offer)
{
    return static_cast<std::int64_t>(offer.worth) - static_cast<std::int64_t>(offer.price);
}

// The offer to buy next: of those whose slots are unfilled, within their ceiling and the spendable gold, the one
// worth most above its price (the first of equals); NO_OFFER when there is none
inline std::size_t BestBuy(std::vector<Offer> const& offers, std::uint32_t filled, std::uint64_t spendable)
{
    std::size_t best = NO_OFFER;
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        Offer const& offer = offers[i];
        if ((offer.slots & filled) || offer.price > offer.ceiling || offer.price > spendable)
            continue;
        if (best == NO_OFFER || Surplus(offer) > Surplus(offers[best]))
            best = i;
    }
    return best;
}

struct Refusal
{
    std::size_t offer{NO_OFFER};
    bool noGold{false};  // within its ceiling, so only the gold is missing; else dearer than the ceiling
};

// Once BestBuy finds none: one refusal per replaced slot that is neither filled nor refused yet (a bit per slot). It
// names the best offer within its ceiling (no gold), else the best offer (over its worth)
inline std::vector<Refusal> Refusals(std::vector<Offer> const& offers, std::uint32_t filled, std::uint32_t refused)
{
    std::vector<Refusal> out;
    for (std::size_t i = 0; i < offers.size(); ++i)
    {
        Offer const& offer = offers[i];
        if ((offer.slots & filled) || (refused & (1u << offer.slot)))
            continue;

        bool const noGold = offer.price <= offer.ceiling;
        auto const same = std::find_if(
            out.begin(), out.end(), [&](Refusal const& refusal) { return offers[refusal.offer].slot == offer.slot; });
        if (same == out.end())
            out.push_back({i, noGold});
        else if (noGold != same->noGold ? noGold : Surplus(offer) > Surplus(offers[same->offer]))
            *same = {i, noGold};
    }
    return out;
}

// Gear lines at the auctioneer (gear WG7, PLAN 3.3): per replaced slot the offer the bot takes. `wears` (an offer's
// index -> bool: the upgrade rule, R1) is asked in each slot's order until its first yes: an offer within its ceiling
// and the spendable gold before any other (a dear offer hides no buyable one), then the largest surplus, then the
// lower price; worth 0 never. A slot none of whose offers fits keeps its best one, whose line writes the skip row.
// The picks by surplus, the largest first (then the cheaper, then the first)
template <typename Wears>
std::vector<std::size_t> PickPerSlot(std::vector<Offer> const& offers, std::uint64_t spendable, Wears&& wears)
{
    auto const fits = [&](Offer const& offer) { return offer.price <= offer.ceiling && offer.price <= spendable; };
    auto const bySurplus = [&](std::size_t a, std::size_t b)
    {
        if (Surplus(offers[a]) != Surplus(offers[b]))
            return Surplus(offers[a]) > Surplus(offers[b]);
        if (offers[a].price != offers[b].price)
            return offers[a].price < offers[b].price;
        return a < b;
    };

    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < offers.size(); ++i)
        if (offers[i].worth)
            order.push_back(i);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
              { return fits(offers[a]) != fits(offers[b]) ? fits(offers[a]) : bySurplus(a, b); });

    std::vector<std::size_t> picks;
    std::uint32_t picked = 0;  // a bit per slot with its pick
    for (std::size_t const i : order)
    {
        std::uint32_t const bit = 1u << offers[i].slot;
        if ((picked & bit) || !wears(i))
            continue;
        picked |= bit;
        picks.push_back(i);
    }
    std::sort(picks.begin(), picks.end(), bySurplus);
    return picks;
}

// The equipment slots as the core numbers them (EQUIPMENT_SLOT_*)
constexpr std::uint8_t EQUIPMENT_SLOTS = 19;
constexpr std::uint8_t SLOT_SHIRT = 3;
constexpr std::uint8_t SLOT_OFF_HAND = 16;
constexpr std::uint8_t SLOT_TABARD = 18;
constexpr std::uint8_t NO_SLOT = 0xFF;

struct WornSlot
{
    bool worn{false};
    std::uint32_t itemLevel{0};
};

// The slot an auction visit's `no_offer` row names when the house has no line for the bot (PLAN 3.3 point 4: demand
// without supply): the first empty slot, else the one whose worn item has the lowest item level (the first of
// equals). Never the shirt, the tabard or a `blocked` slot (a bit per slot: the off hand under a two-hander without
// titan grip, a slot an open bid fills); NO_SLOT when none is left
inline std::uint8_t NoOfferSlot(std::array<WornSlot, EQUIPMENT_SLOTS> const& worn, std::uint32_t blocked)
{
    blocked |= (1u << SLOT_SHIRT) | (1u << SLOT_TABARD);
    std::uint8_t lowest = NO_SLOT;
    for (std::uint8_t slot = 0; slot < EQUIPMENT_SLOTS; ++slot)
    {
        if (blocked & (1u << slot))
            continue;
        if (!worn[slot].worn)
            return slot;
        if (lowest == NO_SLOT || worn[slot].itemLevel < worn[lowest].itemLevel)
            lowest = slot;
    }
    return lowest;
}
}  // namespace GearWorth

#endif
