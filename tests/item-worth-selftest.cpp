/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the item and slot worth rules (src/Ai/World/Rpg/ItemWorthRules.h): level bands, item worth and
// the never-destroy classes, the lowest-worth victim and the margin rule, the rolling measurements, slot worth with
// its falling margin, bag worth and the bag purchase against price, reserve, thrift and budget.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "ItemWorthRules.h"

using WorldTravel::BagBuyInput;
using WorldTravel::DestroyForSpaceInput;
using WorldTravel::ItemFacts;
using WorldTravel::SlotMeasure;
using WorldTravel::SlotRates;
using WorldTravel::SpaceCandidate;
using WorldTravel::SpaceClass;

namespace
{
constexpr uint32_t BAND_LEVEL_30 = 1;
// Heavy Brown Bag: a 10-slot vendor bag for 2 gold
constexpr uint32_t BAG_SLOTS = 10;
constexpr uint32_t FAIR_PRICE = 20000;
constexpr uint32_t GOLD = 10000;
constexpr uint32_t TRAINING_RESERVE = 10 * GOLD;
constexpr uint32_t HIGH_LOOT_SLOTS_PER_HOUR = 40;
constexpr uint32_t FREE_SLOTS = 4;
constexpr uint32_t BUDGET_PERCENT = 50;

int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << name << "\n";
}

void CheckBands()
{
    Check(WorldTravel::LevelBand(0) == 0, "level 0 in the first band");
    Check(WorldTravel::LevelBand(1) == 0, "level 1 in band 1-19");
    Check(WorldTravel::LevelBand(19) == 0, "level 19 in band 1-19");
    Check(WorldTravel::LevelBand(20) == 1, "level 20 in band 20-39");
    Check(WorldTravel::LevelBand(39) == 1, "level 39 in band 20-39");
    Check(WorldTravel::LevelBand(40) == 2, "level 40 in band 40-59");
    Check(WorldTravel::LevelBand(60) == 3, "level 60 in band 60-69");
    Check(WorldTravel::LevelBand(70) == 4, "level 70 in band 70-80");
    Check(WorldTravel::LevelBand(80) == 4, "level 80 in band 70-80");
    Check(WorldTravel::LevelBand(255) == 4, "levels above 80 in the last band");

    SlotMeasure const fresh;
    SlotRates const rates = WorldTravel::CurrentRates(fresh, WorldTravel::DEFAULT_SLOT_BANDS[BAND_LEVEL_30]);
    Check(rates.LootSlotsPerHour == 20.0f && rates.TripMinutes == 10.0f && rates.EarnCopperPerMinute == 100.0f &&
              rates.LostCopperPerHour == 0.0f,
          "a bot without its own numbers uses its band's defaults");
    for (uint32_t band = 1; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
        Check(WorldTravel::DEFAULT_SLOT_BANDS[band].EarnCopperPerMinute >
                  WorldTravel::DEFAULT_SLOT_BANDS[band - 1].EarnCopperPerMinute,
              "higher bands earn more per minute (band " + std::to_string(band) + ")");
}

void CheckItemWorth()
{
    ItemFacts none;
    Check(WorldTravel::ClassifyForSpace(none) == SpaceClass::Sell, "plain loot sells");
    ItemFacts used;
    used.Used = true;
    Check(WorldTravel::ClassifyForSpace(used) == SpaceClass::Use, "a consumable the bot uses");

    ItemFacts quest;
    quest.Quest = true;
    ItemFacts kept;
    kept.Kept = true;
    ItemFacts hearth;
    hearth.Hearthstone = true;
    ItemFacts upgrade;
    upgrade.Upgrade = true;
    upgrade.Used = true;
    Check(WorldTravel::ClassifyForSpace(quest) == SpaceClass::Priceless, "never destroy: quest items");
    Check(WorldTravel::ClassifyForSpace(kept) == SpaceClass::Priceless, "never destroy: kept or banked items");
    Check(WorldTravel::ClassifyForSpace(hearth) == SpaceClass::Priceless, "never destroy: the hearthstone");
    Check(WorldTravel::ClassifyForSpace(upgrade) == SpaceClass::Priceless, "never destroy: upgrades");

    Check(WorldTravel::ItemWorth(SpaceClass::Sell, 125, 500, 3, 1.0f) == 375, "sell worth: vendor price x count");
    Check(WorldTravel::ItemWorth(SpaceClass::Sell, 100, 400, 1, 0.5f) == 50, "sell worth follows the multiplier");
    Check(WorldTravel::ItemWorth(SpaceClass::Use, 25, 100, 5, 1.0f) == 500, "use worth: buy price x count");
    Check(WorldTravel::ItemWorth(SpaceClass::Use, 100, 0, 1, 1.0f) == 400, "use worth without a buy price");
    Check(WorldTravel::ItemWorth(SpaceClass::Priceless, 0, 0, 1, 1.0f) == WorldTravel::WORTH_PRICELESS,
          "priceless worth");
    Check(WorldTravel::ItemWorth(SpaceClass::Sell, 0, 0, 1, 1.0f) == 0, "an item without a price is worth 0");
}

void CheckDestroy()
{
    std::vector<SpaceCandidate> const candidates = {
        {500, false}, {100, true}, {200, false}, {WorldTravel::WORTH_PRICELESS, false}, {200, false}};
    Check(WorldTravel::LowestWorthVictim(candidates) == 2, "victim: lowest worth, protected and priceless skipped");
    Check(
        WorldTravel::LowestWorthVictim({{100, true}, {WorldTravel::WORTH_PRICELESS, false}}) == WorldTravel::NO_VICTIM,
        "no victim when everything is protected");
    Check(WorldTravel::LowestWorthVictim({}) == WorldTravel::NO_VICTIM, "no victim in empty bags");

    DestroyForSpaceInput world;
    world.VictimWorth = 100;
    world.MarginPercent = 50;
    world.LootWorth = 150;
    Check(!WorldTravel::ShouldDestroyFor(world), "world: loot at the margin is not enough");
    world.LootWorth = 151;
    Check(WorldTravel::ShouldDestroyFor(world), "world: loot above the margin wins");
    world.TripWorthIt = true;
    Check(!WorldTravel::ShouldDestroyFor(world), "world: no destroying when a trip is worth it");

    DestroyForSpaceInput dungeon;
    dungeon.InInstance = true;
    dungeon.TripWorthIt = true;
    dungeon.MarginPercent = 50;
    dungeon.VictimWorth = 100;
    dungeon.LootWorth = 101;
    Check(WorldTravel::ShouldDestroyFor(dungeon), "instance: any better loot wins, no margin");
    dungeon.LootWorth = 100;
    Check(!WorldTravel::ShouldDestroyFor(dungeon), "instance: equal loot does not");
    dungeon.LootWorth = WorldTravel::WORTH_PRICELESS;
    Check(WorldTravel::ShouldDestroyFor(dungeon), "a quest item beats any priced victim");
    dungeon.VictimWorth = WorldTravel::WORTH_PRICELESS;
    Check(!WorldTravel::ShouldDestroyFor(dungeon), "a priceless victim is never destroyed");
    dungeon.VictimWorth = 0;
    dungeon.LootWorth = 0;
    Check(!WorldTravel::ShouldDestroyFor(dungeon), "worthless loot destroys nothing");
}

void CheckMeasure()
{
    SlotMeasure measure;
    WorldTravel::AddLootSlots(measure, 1000, 10.0f);
    WorldTravel::AddLootSlots(measure, 1000 + WorldTravel::MS_PER_HOUR / 2, 20.0f);
    WorldTravel::AddLostWorth(measure, 1000 + WorldTravel::MS_PER_HOUR / 2, 600);
    Check(!measure.HasOwn, "no own numbers inside the first window");
    WorldTravel::AddLootSlots(measure, 1000 + WorldTravel::MS_PER_HOUR, 5.0f);
    Check(measure.HasOwn && measure.LootSlotsPerHour == 30.0f && measure.LostCopperPerHour == 600.0f,
          "the first closed window sets r and d");
    WorldTravel::RollWindow(measure, 1000 + 3 * WorldTravel::MS_PER_HOUR);
    Check(measure.LootSlotsPerHour == 16.25f, "a long window averages per hour, then blends with the old rate");

    SlotMeasure wrapped;
    uint32_t const start = UINT32_MAX - 1000;
    WorldTravel::AddLootSlots(wrapped, start, 12.0f);
    WorldTravel::RollWindow(wrapped, start + WorldTravel::MS_PER_HOUR);
    Check(wrapped.HasOwn && wrapped.LootSlotsPerHour == 12.0f, "the window survives the millisecond clock wrapping");

    WorldTravel::AddLostWorth(wrapped, start + WorldTravel::MS_PER_HOUR, WorldTravel::WORTH_PRICELESS);
    Check(wrapped.WindowLostCopper == 0.0f, "a priceless loss is not counted");

    SlotRates const own = WorldTravel::CurrentRates(measure, WorldTravel::DEFAULT_SLOT_BANDS[BAND_LEVEL_30]);
    Check(own.LootSlotsPerHour == 16.25f && own.TripMinutes == 10.0f, "own r once measured, t from the band");
}

SlotRates HighLootRates()
{
    SlotMeasure measure;
    measure.HasOwn = true;
    measure.LootSlotsPerHour = static_cast<float>(HIGH_LOOT_SLOTS_PER_HOUR);
    return WorldTravel::CurrentRates(measure, WorldTravel::DEFAULT_SLOT_BANDS[BAND_LEVEL_30]);
}

void CheckSlotWorth()
{
    SlotRates const rates = WorldTravel::CurrentRates(SlotMeasure{}, WorldTravel::DEFAULT_SLOT_BANDS[BAND_LEVEL_30]);
    float const at4 = WorldTravel::SlotWorthPerHour(rates, 4);
    float const at8 = WorldTravel::SlotWorthPerHour(rates, 8);
    float const at20 = WorldTravel::SlotWorthPerHour(rates, 20);
    Check(at4 > at8 && at8 > at20 && at20 > 0.0f, "more free slots: a lower marginal slot worth");
    Check(at4 == 1000.0f, "slot worth at 4 free: (50 - 40) minutes x 100 copper");
    Check(WorldTravel::TripMinutesPerHour(HighLootRates(), 1) == WorldTravel::MINUTES_PER_HOUR,
          "trip minutes never exceed the hour");
    Check(WorldTravel::SlotWorthPerHour(rates, 0) == WorldTravel::SlotWorthPerHour(rates, 1), "0 free counts as 1");

    SlotRates lossy = rates;
    lossy.LostCopperPerHour = 1000.0f;
    Check(std::fabs(WorldTravel::SlotWorthPerHour(lossy, 4) - 1200.0f) < 0.01f,
          "a fifth of the losses counts for one slot at 4 free");

    Check(WorldTravel::BagWorth(rates, 4, 10, 10, 30) == 0, "a bag adding no slots is worth nothing");
    Check(WorldTravel::BagWorth(rates, 4, 8, 10, 30) == 0, "a smaller bag is worth nothing");
    Check(WorldTravel::BagWorth(rates, 4, 10, 6, 30) < WorldTravel::BagWorth(rates, 4, 10, 0, 30),
          "replacing a bag adds fewer slots than an empty bag slot");
}

void CheckBagPurchase()
{
    SlotRates const rates = HighLootRates();
    uint32_t const worth = WorldTravel::BagWorth(rates, FREE_SLOTS, BAG_SLOTS, 0,
                                                 WorldTravel::DEFAULT_SLOT_BANDS[BAND_LEVEL_30].BagHorizonHours);

    BagBuyInput fair;
    fair.Price = FAIR_PRICE;
    fair.Worth = worth;
    fair.Gold = 30 * GOLD;
    fair.Reserve = TRAINING_RESERVE;
    fair.BudgetPercent = BUDGET_PERCENT;
    Check(WorldTravel::ShouldBuyBag(fair), "exit: 4 free slots, high loot rate: the 10-slot bag at a fair price");

    BagBuyInput dear = fair;
    dear.Price = 10 * FAIR_PRICE;
    Check(!WorldTravel::ShouldBuyBag(dear), "exit: refused at ten times the price");

    BagBuyInput poor = fair;
    poor.Gold = TRAINING_RESERVE + GOLD;
    Check(!WorldTravel::ShouldBuyBag(poor), "exit: refused when it would eat the training reserve");
    BagBuyInput noReserve = poor;
    noReserve.Reserve = 0;
    Check(WorldTravel::ShouldBuyBag(noReserve), "the same gold buys it without the reserve");

    BagBuyInput thrifty = poor;
    thrifty.ThriftPercent = 50;
    Check(WorldTravel::ShouldBuyBag(thrifty), "low thrift halves the reserve");
    thrifty.ThriftPercent = 150;
    thrifty.Gold = 14 * GOLD;
    Check(!WorldTravel::ShouldBuyBag(thrifty), "high thrift raises the reserve above the gold");

    BagBuyInput budget = fair;
    budget.Gold = TRAINING_RESERVE + 3 * GOLD;
    Check(!WorldTravel::ShouldBuyBag(budget), "one bag takes at most the budget share above the reserve");

    BagBuyInput noSlots = fair;
    noSlots.Worth = WorldTravel::BagWorth(rates, FREE_SLOTS, BAG_SLOTS, BAG_SLOTS, 30);
    noSlots.Price = 0;
    Check(!WorldTravel::ShouldBuyBag(noSlots), "a bag adding no slots is refused even for free");

    uint32_t const roomy = WorldTravel::BagWorth(rates, 30, BAG_SLOTS, 0, 30);
    Check(roomy < worth, "a bot with room values the same bag less");
}
}  // namespace

int main()
{
    CheckBands();
    CheckItemWorth();
    CheckDestroy();
    CheckMeasure();
    CheckSlotWorth();
    CheckBagPurchase();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
