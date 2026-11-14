/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMWORTHRULES_H
#define _PLAYERBOT_ITEMWORTHRULES_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

// The Player-free rules of item worth, slot worth, destroying for space, buying bags and inventory errands for
// persistent bots. All worth is in copper. Header-only so tests/item-worth-selftest.cpp checks the same code the server
// runs.
namespace WorldTravel
{
constexpr uint32_t LEVEL_BAND_COUNT = 5;
// The highest level of each band: 1-19, 20-39, 40-59, 60-69, 70-80
constexpr std::array<uint32_t, LEVEL_BAND_COUNT> LEVEL_BAND_TOP = {19, 39, 59, 69, 80};
// The worth of an item that is never destroyed for space
constexpr uint32_t WORTH_PRICELESS = std::numeric_limits<uint32_t>::max();
// What the bot saves by having an item it uses, when the item has no buy price: vendors sell at about four times
// what they pay
constexpr uint32_t BUY_PER_SELL_PRICE = 4;
constexpr float MINUTES_PER_HOUR = 60.0f;
constexpr uint32_t MS_PER_HOUR = 3600000;
// A bot's own loot and loss rates are measured over windows of this length
constexpr uint32_t MEASURE_WINDOW_MS = MS_PER_HOUR;
constexpr uint32_t PERCENT = 100;

inline uint32_t LevelBand(uint32_t level)
{
    for (uint32_t band = 0; band < LEVEL_BAND_COUNT; ++band)
        if (level <= LEVEL_BAND_TOP[band])
            return band;

    return LEVEL_BAND_COUNT - 1;
}

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

// How an item counts when space runs short
enum class SpaceClass : uint8_t
{
    Sell,      // worth what a vendor pays now (later the auction or disenchant value)
    Use,       // an upgrade, consumable, reagent or ammo the bot uses: worth what it saves the bot
    Kept,      // gear the bank rule keeps for a later level (BankRules.h): worth its later worth
    Priceless  // quest, guild task and kept items, upgrades, the hearthstone: never destroyed
};

struct ItemFacts
{
    bool Quest = false;  // a quest item, a quest starter or a guild task item
    bool Kept = false;   // kept on purpose (exact quest amount, stock to keep), or banked with its later worth unknown
    bool Hearthstone = false;
    bool Upgrade = false;         // an equipment upgrade
    bool Used = false;            // a consumable, reagent, tradeskill item or ammo the bot uses
    uint32_t LaterWorthEach = 0;  // kept by the bank rule for a later level: one item's later worth; 0 = not
};

inline SpaceClass ClassifyForSpace(ItemFacts const& facts)
{
    if (facts.Quest || facts.Kept || facts.Hearthstone || facts.Upgrade)
        return SpaceClass::Priceless;
    if (facts.LaterWorthEach)
        return SpaceClass::Kept;

    return facts.Used ? SpaceClass::Use : SpaceClass::Sell;
}

// sellMultiplier: what a vendor pays this bot per copper of the item's sell price; laterWorthEach: one Kept item's
// later worth
inline uint32_t ItemWorth(SpaceClass spaceClass, uint32_t sellPrice, uint32_t buyPrice, uint32_t count,
                          float sellMultiplier, uint32_t laterWorthEach = 0)
{
    if (spaceClass == SpaceClass::Priceless)
        return WORTH_PRICELESS;

    double each = sellPrice * static_cast<double>(std::max(sellMultiplier, 0.0f));
    if (spaceClass == SpaceClass::Use)
        each = std::max<double>(buyPrice, static_cast<double>(sellPrice) * BUY_PER_SELL_PRICE);
    else if (spaceClass == SpaceClass::Kept)
        each = laterWorthEach;

    double const total = each * std::max<uint32_t>(count, 1);
    return static_cast<uint32_t>(std::min(total, static_cast<double>(WORTH_PRICELESS - 1)));
}

struct SpaceCandidate
{
    uint32_t Worth = 0;
    bool Protected = false;  // priceless, a worn PvP item or a slot that cannot take the loot
};

constexpr int32_t NO_VICTIM = -1;

// The index of the lowest-worth item that may be destroyed (the first of equal ones), or NO_VICTIM
inline int32_t LowestWorthVictim(std::vector<SpaceCandidate> const& candidates)
{
    int32_t victim = NO_VICTIM;
    for (size_t index = 0; index < candidates.size(); ++index)
    {
        SpaceCandidate const& candidate = candidates[index];
        if (candidate.Protected || candidate.Worth == WORTH_PRICELESS)
            continue;

        if (victim == NO_VICTIM || candidate.Worth < candidates[victim].Worth)
            victim = static_cast<int32_t>(index);
    }

    return victim;
}

struct DestroyForSpaceInput
{
    bool InInstance = false;   // no outlet in a dungeon, raid or battleground
    bool TripWorthIt = false;  // in the world, an inventory trip is worth more than destroying (WT3 decides)
    uint32_t LootWorth = 0;
    uint32_t VictimWorth = 0;
    uint32_t MarginPercent = 0;  // in the world the loot must beat the victim by this much
};

// A full bot destroys its lowest-worth item for better loot when no outlet is near: always in an instance, in the
// world only when no trip is worth it and the loot beats the victim by the margin
inline bool ShouldDestroyFor(DestroyForSpaceInput const& in)
{
    if (in.VictimWorth == WORTH_PRICELESS || in.LootWorth == 0)
        return false;

    if (in.InInstance)
        return in.LootWorth > in.VictimWorth;

    if (in.TripWorthIt)
        return false;

    uint64_t const needed = static_cast<uint64_t>(in.VictimWorth) * (PERCENT + in.MarginPercent);
    return static_cast<uint64_t>(in.LootWorth) * PERCENT > needed;
}

// The bot's travel shares (upkeep plan WU14, §3.8): the minutes per hour of play on route legs that a riding rank
// shortens. A walk leg counts whole, mounted or not (a rank saves its ratio of the time at the bot's best speed now);
// transport and taxi legs count nothing (no mount shortens them). The flight shares count where a flying mount would
// fly: fly legs, and walk legs in an open flying area, ridden on the ground until the bot flies there
enum TravelShare : uint8_t
{
    TRAVEL_WALK,       // every walk leg: the ground ranks shorten it
    TRAVEL_FLIGHT,     // fly legs; walk legs in an open flying area outside Northrend (inside it with cold weather)
    TRAVEL_NORTHREND,  // fly and walk legs in Northrend's open flying areas: cold weather flying flies the walked ones
    TRAVEL_SHARES
};
constexpr uint32_t TRAVEL_MAP_NORTHREND = 571;

enum class LegKind : uint8_t
{
    Other,  // a transport, a taxi or a no-op leg
    Walk,
    Fly
};

// The shares a done or failed leg counts towards, as bits (1 << TravelShare). openFlying: the leg ended in an area
// where flying mounts fly (FlightMap's open areas); coldWeather: the bot knows cold weather flying
constexpr uint8_t LegShares(LegKind kind, uint32_t mapId, bool openFlying, bool coldWeather)
{
    constexpr uint32_t walk = 1u << TRAVEL_WALK;
    constexpr uint32_t flight = 1u << TRAVEL_FLIGHT;
    constexpr uint32_t northrend = 1u << TRAVEL_NORTHREND;
    bool const inNorthrend = mapId == TRAVEL_MAP_NORTHREND;
    uint32_t shares = 0;
    if (kind == LegKind::Fly)
        shares = inNorthrend ? flight | northrend : flight;
    else if (kind == LegKind::Walk && !openFlying)
        shares = walk;
    else if (kind == LegKind::Walk && !inNorthrend)
        shares = walk | flight;
    else if (kind == LegKind::Walk)
        shares = coldWeather ? walk | flight | northrend : walk | northrend;
    return static_cast<uint8_t>(shares);
}

// A bot's rolling measurements (in memory, per login). Until the first window closes the band defaults apply.
struct SlotMeasure
{
    bool WindowStarted = false;
    uint32_t WindowStartMs = 0;
    float WindowSlots = 0.0f;
    float WindowLostCopper = 0.0f;
    bool HasOwn = false;
    float LootSlotsPerHour = 0.0f;   // r
    float LostCopperPerHour = 0.0f;  // d: destroyed or left behind for lack of space
    bool HasTrip = false;
    float TripMinutes = 0.0f;  // t: from the start of an errand to the end of its vendor visit
    // The last errand check (a start, an end or a refusal) and the vendor it found, for TripWorthIt while looting
    bool ErrandChecked = false;
    uint32_t ErrandCheckMs = 0;
    bool HasVendor = false;
    uint32_t VendorMapId = 0;
    float VendorX = 0.0f;
    float VendorY = 0.0f;
    float VendorZ = 0.0f;
    // Consumable use per upkeep stock kind (upkeep plan WU6, UpkeepNeeds::SampleUse): the stock the last vendor visit
    // ended with, what it was counted by (the pet's diet, the ammo type and grade; 0 = unknown) and the use per hour
    // measured from its falls since; 0 = not measured yet (the default applies)
    static constexpr std::size_t USE_KINDS = 7;  // Planner::UPKEEP_STOCK_COUNT
    bool UseNoted = false;
    uint32_t UseNotedMs = 0;
    std::array<uint32_t, USE_KINDS> UseStock{};
    std::array<uint32_t, USE_KINDS> UseBasis{};
    std::array<float, USE_KINDS> UsePerHour{};
    // The travel shares (NoteTravelLeg): the leg milliseconds of the open window per share and the measured minutes
    // per hour; until the first window closes the needs read the default walk share and no flight
    bool TravelStarted = false;
    uint32_t TravelStartMs = 0;
    std::array<float, TRAVEL_SHARES> TravelWindowMs{};
    bool HasTravel = false;
    std::array<float, TRAVEL_SHARES> TravelMinutesPerHour{};
};

// Closes a window that is at least MEASURE_WINDOW_MS old; the new rates are the average of the old ones and the
// window's (nowMs from a wrapping millisecond clock)
inline void RollWindow(SlotMeasure& measure, uint32_t nowMs)
{
    if (!measure.WindowStarted)
    {
        measure.WindowStarted = true;
        measure.WindowStartMs = nowMs;
        return;
    }

    uint32_t const elapsed = nowMs - measure.WindowStartMs;
    if (elapsed < MEASURE_WINDOW_MS)
        return;

    float const hours = static_cast<float>(elapsed) / MS_PER_HOUR;
    float const slots = measure.WindowSlots / hours;
    float const lost = measure.WindowLostCopper / hours;
    if (measure.HasOwn)
    {
        measure.LootSlotsPerHour = (measure.LootSlotsPerHour + slots) / 2.0f;
        measure.LostCopperPerHour = (measure.LostCopperPerHour + lost) / 2.0f;
    }
    else
    {
        measure.HasOwn = true;
        measure.LootSlotsPerHour = slots;
        measure.LostCopperPerHour = lost;
    }

    measure.WindowStartMs = nowMs;
    measure.WindowSlots = 0.0f;
    measure.WindowLostCopper = 0.0f;
}

inline void AddLootSlots(SlotMeasure& measure, uint32_t nowMs, float slots)
{
    RollWindow(measure, nowMs);
    measure.WindowSlots += slots;
}

inline void AddLostWorth(SlotMeasure& measure, uint32_t nowMs, uint32_t copper)
{
    RollWindow(measure, nowMs);
    if (copper != WORTH_PRICELESS)
        measure.WindowLostCopper += static_cast<float>(copper);
}

// A done or failed leg's time (shares: LegShares' bits). The first window starts where the first leg started, so
// that leg lies inside it; a leg ending at least MEASURE_WINDOW_MS after the window's start closes it, and the new
// shares are the average of the old ones and the window's (at most the whole hour; nowMs from a wrapping clock)
inline void NoteTravelLeg(SlotMeasure& measure, uint8_t shares, uint32_t legMs, uint32_t nowMs)
{
    if (!measure.TravelStarted)
    {
        measure.TravelStarted = true;
        measure.TravelStartMs = nowMs - legMs;
    }
    for (std::size_t share = 0; share < TRAVEL_SHARES; ++share)
        if (shares & (1u << share))
            measure.TravelWindowMs[share] += static_cast<float>(legMs);

    uint32_t const elapsed = nowMs - measure.TravelStartMs;
    if (elapsed < MEASURE_WINDOW_MS)
        return;

    for (std::size_t share = 0; share < TRAVEL_SHARES; ++share)
    {
        float const minutes =
            std::min(measure.TravelWindowMs[share] / static_cast<float>(elapsed) * MINUTES_PER_HOUR, MINUTES_PER_HOUR);
        float& rate = measure.TravelMinutesPerHour[share];
        rate = measure.HasTravel ? (rate + minutes) / 2.0f : minutes;
        measure.TravelWindowMs[share] = 0.0f;
    }
    measure.HasTravel = true;
    measure.TravelStartMs = nowMs;
}

struct SlotRates
{
    float LootSlotsPerHour = 0.0f;     // r
    float TripMinutes = 0.0f;          // t
    float LostCopperPerHour = 0.0f;    // d
    float EarnCopperPerMinute = 0.0f;  // e
};

// The minutes of a finished errand; t is the average of the old value and the new trip
inline void AddTripMinutes(SlotMeasure& measure, float minutes)
{
    measure.TripMinutes = measure.HasTrip ? (measure.TripMinutes + minutes) / 2.0f : minutes;
    measure.HasTrip = true;
}

// The bot's own r and d once measured and its own t after its first errand, else the band's; e stays the band's
// until the earning rate is measured (WT15)
inline SlotRates CurrentRates(SlotMeasure const& measure, SlotBand const& band)
{
    SlotRates rates;
    rates.LootSlotsPerHour = measure.HasOwn ? measure.LootSlotsPerHour : static_cast<float>(band.LootSlotsPerHour);
    rates.TripMinutes = measure.HasTrip ? measure.TripMinutes : static_cast<float>(band.TripMinutes);
    rates.LostCopperPerHour = measure.HasOwn ? measure.LostCopperPerHour : 0.0f;
    rates.EarnCopperPerMinute = static_cast<float>(band.EarnCopperPerMinute);
    return rates;
}

// With F free slots after an emptying trip the bot makes about r / F trips of t minutes per hour, at most the hour
inline float TripMinutesPerHour(SlotRates const& rates, uint32_t freeSlots)
{
    float const slots = static_cast<float>(std::max<uint32_t>(freeSlots, 1));
    return std::min(MINUTES_PER_HOUR, rates.TripMinutes * rates.LootSlotsPerHour / slots);
}

// What `added` more slots are worth per hour: the trip minutes they save times e, plus the share added / (F + added)
// of what the bot loses for lack of space
inline float SlotsWorthPerHour(SlotRates const& rates, uint32_t freeSlots, uint32_t added)
{
    if (!added)
        return 0.0f;

    uint32_t const free = std::max<uint32_t>(freeSlots, 1);  // a full bot counts as one free slot
    float const saved = TripMinutesPerHour(rates, free) - TripMinutesPerHour(rates, free + added);
    float const share = static_cast<float>(added) / static_cast<float>(free + added);
    return saved * rates.EarnCopperPerMinute + rates.LostCopperPerHour * share;
}

// What one more free slot is worth to the bot per hour
inline float SlotWorthPerHour(SlotRates const& rates, uint32_t freeSlots)
{
    return SlotsWorthPerHour(rates, freeSlots, 1);
}

// A bag is worth the slots it adds over the bag it replaces (0 for an empty bag slot) over the horizon
inline uint32_t BagWorth(SlotRates const& rates, uint32_t freeSlots, uint32_t bagSlots, uint32_t replacedSlots,
                         uint32_t horizonHours)
{
    if (bagSlots <= replacedSlots)
        return 0;

    double const worth =
        static_cast<double>(SlotsWorthPerHour(rates, freeSlots, bagSlots - replacedSlots)) * horizonHours;
    return static_cast<uint32_t>(std::min(worth, static_cast<double>(WORTH_PRICELESS - 1)));
}

struct BagBuyInput
{
    uint32_t Price = 0;
    uint32_t Worth = 0;
    uint64_t Gold = 0;
    uint64_t Reserve = 0;          // what the bot soon needs: repairs, training, travel, ammo, guild
    uint32_t ThriftPercent = 100;  // scales the reserve
    uint32_t BudgetPercent = 100;  // the share of the money above the reserve one bag may cost
};

// A bag is bought when its price is at most its worth and fits the budget left after the reserve
inline bool ShouldBuyBag(BagBuyInput const& in)
{
    if (!in.Worth || in.Price > in.Worth)
        return false;

    uint64_t const reserve = in.Reserve * in.ThriftPercent / PERCENT;
    if (in.Gold <= reserve)
        return false;

    return static_cast<uint64_t>(in.Price) * PERCENT <= (in.Gold - reserve) * in.BudgetPercent;
}

// Inventory errands: a trip to a vendor to sell, repair and maybe buy a bag. A bot whose general bags are this full
// checks whether a trip is worth it
constexpr uint32_t ERRAND_PRESSURE_PERCENT = 80;
// After an errand check (a start, an end or a refusal) the next waits this long, so that bags full of things no vendor
// takes do not send the bot on trip after trip
constexpr uint32_t ERRAND_RETRY_MS = 5 * 60 * 1000;
// A vendor farther than this walk is never chosen; the deadline leaves room for the walk, fights and the visit
constexpr float ERRAND_MAX_WALK_MINUTES = 10.0f;
constexpr uint32_t ERRAND_DEADLINE_MS = 20 * 60 * 1000;
constexpr float SECONDS_PER_MINUTE = 60.0f;

inline void StampErrandCheck(SlotMeasure& measure, uint32_t nowMs)
{
    measure.ErrandChecked = true;
    measure.ErrandCheckMs = nowMs;
}

// No errand check (a start, an end or a refusal) within the retry time; the repair errand shares it
inline bool ErrandRetryOver(SlotMeasure const& measure, uint32_t nowMs)
{
    return !measure.ErrandChecked || nowMs - measure.ErrandCheckMs >= ERRAND_RETRY_MS;
}

// The cheap part of the errand rule, for the trigger: bags this full and no errand check within the retry time
inline bool ErrandDue(SlotMeasure const& measure, uint32_t usedPercent, uint32_t nowMs)
{
    if (usedPercent < ERRAND_PRESSURE_PERCENT)
        return false;

    return ErrandRetryOver(measure, nowMs);
}

// Straight-line minutes at the bot's run speed
inline float WalkMinutes(float yards, float yardsPerSecond)
{
    if (yardsPerSecond <= 0.0f)
        return std::numeric_limits<float>::max();

    return yards / yardsPerSecond / SECONDS_PER_MINUTE;
}

struct TripInput
{
    SlotRates Rates;
    uint32_t FreeSlots = 0;          // free general slots now
    uint32_t SellableSlots = 0;      // stacks a vendor would take
    float WalkMinutes = 0.0f;        // to the vendor
    float MinutesToNextStop = 0.0f;  // until the bot passes a vendor anyway
};

inline TripInput MakeTripInput(SlotMeasure const& measure, SlotBand const& band, uint32_t freeSlots,
                               uint32_t sellableSlots, float walkMinutes, uint32_t minutesToNextStop)
{
    TripInput in;
    in.Rates = CurrentRates(measure, band);
    in.FreeSlots = freeSlots;
    in.SellableSlots = sellableSlots;
    in.WalkMinutes = walkMinutes;
    in.MinutesToNextStop = static_cast<float>(minutesToNextStop);
    return in;
}

// A trip just for selling is worth it when staying this full until the next natural stop loses more than the trip
// costs: the slots it frees are worth SlotsWorthPerHour until that stop; the trip takes a usual trip's minutes (t)
// plus the walk to this vendor (standing for the way back) at the earning rate
inline bool TripWorthIt(TripInput const& in)
{
    if (!in.SellableSlots || in.WalkMinutes > ERRAND_MAX_WALK_MINUTES)
        return false;

    float const saved =
        SlotsWorthPerHour(in.Rates, in.FreeSlots, in.SellableSlots) * in.MinutesToNextStop / MINUTES_PER_HOUR;
    float const cost = (in.Rates.TripMinutes + in.WalkMinutes) * in.Rates.EarnCopperPerMinute;
    return saved > cost;
}

// While looting, a full bot leaves loot for a trip only when the errand can start now: it may leave its RPG status (a
// questing or travelling bot may not) and is due for an errand check. Otherwise the destroy rule without a trip holds,
// so a questing bot still makes room for a quest drop.
inline bool LootWaitsForTrip(bool mayLeaveStatus, SlotMeasure const& measure, uint32_t usedPercent, uint32_t nowMs,
                             TripInput const& in)
{
    return mayLeaveStatus && ErrandDue(measure, usedPercent, nowMs) && TripWorthIt(in);
}
}  // namespace WorldTravel

#endif
