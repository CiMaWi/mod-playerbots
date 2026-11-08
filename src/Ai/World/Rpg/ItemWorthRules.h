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

// The Player-free rules of item worth, slot worth, destroying for space and buying bags for persistent bots. All worth
// is in copper. Header-only so tests/item-worth-selftest.cpp checks the same code the server runs.
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
    Priceless  // quest, guild task, kept or banked items, the hearthstone: never destroyed
};

struct ItemFacts
{
    bool Quest = false;  // a quest item, a quest starter or a guild task item
    bool Kept = false;   // kept on purpose (exact quest amount, stock to keep) or banked
    bool Hearthstone = false;
    bool Upgrade = false;  // an equipment upgrade
    bool Used = false;     // a consumable, reagent, tradeskill item or ammo the bot uses
};

inline SpaceClass ClassifyForSpace(ItemFacts const& facts)
{
    if (facts.Quest || facts.Kept || facts.Hearthstone || facts.Upgrade)
        return SpaceClass::Priceless;

    return facts.Used ? SpaceClass::Use : SpaceClass::Sell;
}

// sellMultiplier: what a vendor pays this bot per copper of the item's sell price
inline uint32_t ItemWorth(SpaceClass spaceClass, uint32_t sellPrice, uint32_t buyPrice, uint32_t count,
                          float sellMultiplier)
{
    if (spaceClass == SpaceClass::Priceless)
        return WORTH_PRICELESS;

    double each = sellPrice * static_cast<double>(std::max(sellMultiplier, 0.0f));
    if (spaceClass == SpaceClass::Use)
        each = std::max<double>(buyPrice, static_cast<double>(sellPrice) * BUY_PER_SELL_PRICE);

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

struct SlotRates
{
    float LootSlotsPerHour = 0.0f;     // r
    float TripMinutes = 0.0f;          // t
    float LostCopperPerHour = 0.0f;    // d
    float EarnCopperPerMinute = 0.0f;  // e
};

// The bot's own r and d once measured, else the band's; t and e are the band's until WT3 measures them
inline SlotRates CurrentRates(SlotMeasure const& measure, SlotBand const& band)
{
    SlotRates rates;
    rates.LootSlotsPerHour = measure.HasOwn ? measure.LootSlotsPerHour : static_cast<float>(band.LootSlotsPerHour);
    rates.TripMinutes = static_cast<float>(band.TripMinutes);
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
}  // namespace WorldTravel

#endif
