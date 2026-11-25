/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the reference value (src/Ai/World/Rpg/AuctionRules.h, auction house plan WA2a): planted cases
// on a hand index (a vendor-stocked reagent, a gathered ore at the band its skill first fits, a bar made from it, a
// drop valued by the shape, a poor item at its sell price, a bind-on-pickup item left out, a recipe cycle cut off by
// the bounds, the dump round trip); a seeded search over random item sets and recipe worlds for the invariants, each
// with a negative control (a broken variant of the rule the invariant must catch in the same run); and the table of
// the dumped real data (tests/fixtures/auction-reference-inputs.txt with tests/fixtures/recipe-index-dump.txt), printed
// as markdown for the user. Exit code 1 on a failed check.
// WA2b (AuctionPricing.h): the economy reference's auction cases as fixed values, planted asks and purchases, a
// seeded search over markets and bots for ten invariants with their controls, and the printed decisions.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "AuctionBuyList.h"
#include "AuctionMemory.h"
#include "AuctionPricing.h"
#include "AuctionRules.h"
#include "ProfessionRules.h"
#include "RecipeIndex.h"

using namespace AuctionRules;
using Recipes::ItemCount;
using Recipes::NodeRow;
using Recipes::Process;
using Recipes::ProcessKind;
using Recipes::Recipe;
using Recipes::RecipeIndex;
using Recipes::VendorRow;
using Skill = ProfessionRules::Skill;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::cout << "FAIL: " << what << '\n';
    }
}

bool Near(double a, double b) { return std::abs(a - b) <= 1e-6 * std::max(1.0, std::abs(b)); }

std::uint16_t Id(Skill skill) { return static_cast<std::uint16_t>(skill); }

WorldTravel::SlotBandTable const BANDS = WorldTravel::DEFAULT_SLOT_BANDS;

double Earn(std::uint32_t band) { return BANDS[band].EarnCopperPerMinute; }

Recipe MakeRecipe(Skill skill, std::uint32_t spell, std::uint32_t product, std::vector<ItemCount> reagents,
                  std::uint32_t castMs, std::uint32_t productCount = 1, std::uint32_t minRank = 1)
{
    Recipe recipe;
    recipe.skill = Id(skill);
    recipe.spell = spell;
    recipe.minRank = minRank;
    recipe.effect = Recipes::Effect::Make;
    recipe.product = product;
    recipe.productCount = productCount;
    recipe.castMs = castMs;
    recipe.reagents = std::move(reagents);
    return recipe;
}

ItemInput Item(std::uint32_t id, std::uint32_t quality, std::uint32_t itemLevel, std::uint32_t buy, std::uint32_t sell,
               std::uint32_t buyCount = 1, bool stocked = false)
{
    ItemInput item;
    item.id = id;
    item.name = "item " + std::to_string(id);
    item.classId = 7;
    item.quality = quality;
    item.itemLevel = itemLevel;
    item.buyPrice = buy;
    item.sellPrice = sell;
    item.buyCount = buyCount;
    item.vendorStocked = stocked;
    return item;
}

// ---- the rule with one step broken: the negative controls ----
enum class Fault
{
    None,
    NoClamp,          // invariant 1
    NoCap,            // invariant 2
    MadeMargin,       // invariant 3: a made thing a quarter above its inputs
    FirstRowWins,     // invariant 4: the first row of an id in input order wins
    PerItemConstant,  // invariant 5: a value that depends on the item's place in the set
    InvertedLevel,    // invariant 6: the level multiplier inverted
    SwappedRates      // invariant 7: the bands' earning rates in reverse
};

ItemReference DecideVariant(ItemInput const& item, Costs const& costs, std::uint8_t sources, Fault fault)
{
    std::uint32_t const band = ItemBand(costs, item.requiredLevel);
    Pick pick = FromSource(costs.cost[band], costs.copper[band]);
    if (fault != Fault::NoCap)
        pick = Cap(item, pick);
    if (!pick.sourced)
    {
        pick = Shaped(item);
        if (fault == Fault::InvertedLevel && pick.basis == Basis::Shaped)
            pick.copper /= LevelMultiplier(item.itemLevel) * LevelMultiplier(item.itemLevel);
    }
    if (fault == Fault::MadeMargin && pick.basis == Basis::Made)
        pick.copper *= 1.25;
    if (fault != Fault::NoClamp)
        pick = Clamp(item, pick);
    return Reference(item, pick, band, costs, sources);
}

Table BuildVariant(std::vector<ItemInput> inputs, RecipeIndex const& index, Fault fault)
{
    if (fault == Fault::None)
        return Build(std::move(inputs), index, BANDS);
    if (fault == Fault::SwappedRates)
    {
        WorldTravel::SlotBandTable swapped = BANDS;
        std::reverse(swapped.begin(), swapped.end());
        return Build(std::move(inputs), index, swapped);
    }
    if (fault == Fault::FirstRowWins)
    {
        std::vector<ItemInput> first;
        for (ItemInput const& item : inputs)
            if (std::none_of(first.begin(), first.end(), [&item](ItemInput const& f) { return f.id == item.id; }))
                first.push_back(item);
        std::stable_sort(first.begin(), first.end(),
                         [](ItemInput const& a, ItemInput const& b) { return a.id < b.id; });
        return Build(std::move(first), index, BANDS);
    }
    Deduplicate(inputs);
    BandPricers const pricers(index, BANDS);
    Table table;
    for (ItemInput const& item : inputs)
    {
        if (LeftOutOf(item) != LeftOut::None)
            continue;
        std::uint8_t const sources = pricers.SourcesOf(item.id);
        ItemReference reference = DecideVariant(item, pricers.Of(item.id, sources), sources, fault);
        if (!reference.copper)
            continue;
        if (fault == Fault::PerItemConstant)
            reference.copper += table.rows.size() + 1;
        table.rows.push_back(reference);
    }
    table.inputs = std::move(inputs);
    return table;
}

bool SameRow(ItemReference const& a, ItemReference const& b)
{
    return a.id == b.id && a.copper == b.copper && a.basis == b.basis && a.band == b.band && a.sources == b.sources &&
           a.byBand == b.byBand;
}

bool SameTable(Table const& a, Table const& b)
{
    return a.rows.size() == b.rows.size() && std::equal(a.rows.begin(), a.rows.end(), b.rows.begin(), SameRow);
}

// ---- planted cases ----
constexpr std::uint32_t ORE = 2770;
constexpr std::uint32_t BAR = 2840;
constexpr std::uint32_t THREAD = 2320;
constexpr std::uint32_t HERB = 2447;
constexpr std::uint32_t HERB_DEAR = 785;
constexpr std::uint32_t LINEN = 2589;
constexpr std::uint32_t DROP = 9001;
constexpr std::uint32_t POOR = 9002;
constexpr std::uint32_t BOP = 9003;
constexpr std::uint32_t NOTHING = 9004;
constexpr std::uint32_t CYCLE_A = 90001;
constexpr std::uint32_t CYCLE_B = 90002;
constexpr std::uint32_t ZONE = 12;

RecipeIndex HandIndex()
{
    std::vector<Recipe> recipes = {
        MakeRecipe(Skill::Mining, 2657, BAR, {{ORE, 1}}, 0),
        MakeRecipe(Skill::Blacksmithing, 90011, CYCLE_A, {{CYCLE_B, 1}}, 0),
        MakeRecipe(Skill::Blacksmithing, 90012, CYCLE_B, {{CYCLE_A, 1}}, 0),
    };
    // The herbs' vendors are not in the index (it lists the vendors of recipe materials): the table's own cap decides
    std::vector<VendorRow> vendors = {{THREAD, 50, 5, 3, 0}};
    std::vector<NodeRow> nodes = {
        {ZONE, Id(Skill::Mining), 150, 1731, 40, 70.0f, ORE, 2.0},
        {ZONE, Id(Skill::Herbalism), 1, 1617, 30, 80.0f, HERB, 1.5},
        {ZONE, Id(Skill::Herbalism), 50, 1618, 30, 80.0f, HERB_DEAR, 1.5},
    };
    return RecipeIndex::FromRecords(std::move(recipes), {}, {}, {}, std::move(vendors), std::move(nodes));
}

std::vector<ItemInput> HandInputs()
{
    ItemInput bop = Item(BOP, 3, 60, 40000, 10000);
    bop.bonding = BIND_ON_PICKUP;
    return {Item(ORE, 1, 5, 0, 0),
            Item(BAR, 1, 10, 40, 10),
            Item(THREAD, 1, 5, 50, 2, 5, true),
            Item(HERB, 1, 5, 5, 1, 1, true),
            Item(HERB_DEAR, 1, 10, 20, 1, 1, true),
            Item(LINEN, 1, 5, 40, 10),
            Item(DROP, 2, 30, 1000, 250),
            Item(POOR, 0, 20, 28, 7),
            bop,
            Item(NOTHING, 1, 5, 0, 0),
            Item(CYCLE_A, 1, 5, 0, 3),
            Item(CYCLE_B, 1, 5, 0, 3)};
}

void CheckHand()
{
    Check(TypicalSkill(0) == 107 && TypicalSkill(1) == 219 && TypicalSkill(2) == 332 && TypicalSkill(3) == 388 &&
              TypicalSkill(4) == SKILL_CAP,
          "the typical skill keeps pace with the level and reaches the cap at the top band");
    RecipeIndex const index = HandIndex();
    Table const table = Build(HandInputs(), index, BANDS);

    ItemReference const* thread = table.Find(THREAD);
    Check(thread && thread->copper == 10 && thread->basis == Basis::VendorCap,
          "a vendor-stocked reagent is worth its vendor price per unit");

    double const herb = (80.0 / (7.0 * 60.0) + 0.25) / 1.5 * Earn(0);
    ItemReference const* cheapHerb = table.Find(HERB);
    ItemReference const* dearHerb = table.Find(HERB_DEAR);
    Check(cheapHerb && cheapHerb->copper == 5 && cheapHerb->basis == Basis::VendorCap &&
              cheapHerb->byBand[0] == Round(herb),
          "a gatherable herb also sold cheaper: the vendor price");
    Check(dearHerb && dearHerb->copper == Round(herb) && dearHerb->basis == Basis::Gathered,
          "a gatherable herb sold dearer: the gathering cost");

    double const ore = (70.0 / (7.0 * 60.0) + 0.25) / 2.0;  // minutes per ore
    ItemReference const* oreRow = table.Find(ORE);
    Check(oreRow && oreRow->band == 1 && oreRow->basis == Basis::Gathered && oreRow->byBand[0] == NO_COST &&
              oreRow->copper == Round(ore * Earn(1)) && oreRow->byBand[4] == Round(ore * Earn(4)),
          "an ore needing skill 150: band 1, minutes at the band's earning rate; no price, still a row");
    ItemReference const* bar = table.Find(BAR);
    Check(bar && bar->band == 1 && bar->basis == Basis::Made && bar->copper == Round((ore + 1.5 / 60.0) * Earn(1)),
          "a bar: the ore plus a cast at the bar's band");

    ItemReference const* drop = table.Find(DROP);
    double const shaped = 1000.0 * LevelMultiplier(30) * QualityMultiplier(2);
    Check(drop && drop->basis == Basis::Shaped && drop->copper == Round(shaped) && drop->byBand[2] == NO_COST,
          "a drop: the vendor price scaled by quality and item level");
    ItemInput blue = HandInputs()[6];
    blue.quality = 3;
    ItemInput higher = HandInputs()[6];
    higher.itemLevel = 60;
    Costs const none = BandPricers(index, BANDS).Of(DROP, 0);
    Check(Decide(blue, none, 0).copper > drop->copper && Decide(higher, none, 0).copper > drop->copper,
          "a drop: a better quality and a higher level are worth more");
    Check(Near(LevelMultiplier(0), 0.5 + std::exp(0.1 / 60.0) / 2.0) && QualityMultiplier(1) < QualityMultiplier(2) &&
              QualityMultiplier(2) < QualityMultiplier(3) && QualityMultiplier(3) < QualityMultiplier(4),
          "the shapes: cmangos's level curve, the rarity multiplier rising with quality");

    ItemReference const* poor = table.Find(POOR);
    Check(poor && poor->copper == 7 && poor->basis == Basis::Floor, "a poor item: its sell price");
    ItemReference const* linen = table.Find(LINEN);
    Check(linen && linen->basis == Basis::Shaped &&
              linen->copper == Round(40.0 * LevelMultiplier(5) * QualityMultiplier(1)),
          "a trade good with no source in the index: shaped from its buy price");

    Check(!table.Find(BOP) && table.counts.leftOut[static_cast<std::size_t>(LeftOut::BindOnPickup)] == 1,
          "a bind-on-pickup item is left out and counted");
    Check(!table.Find(NOTHING) && table.counts.leftOut[static_cast<std::size_t>(LeftOut::NoValue)] == 1,
          "no source and no price: left out and counted");

    ItemReference const* cycle = table.Find(CYCLE_A);
    Costs const cycleCosts = BandPricers(index, BANDS).Of(CYCLE_A, SOURCE_RECIPE);
    Check(cycleCosts.cost[0].cause == Materials::Cause::Bound && cycle && cycle->basis == Basis::Shaped &&
              cycle->copper == Round(12.0 * LevelMultiplier(5) * QualityMultiplier(1)) && table.counts.bound == 2,
          "a recipe cycle: cut off by the bounds, valued by the shape and counted");
    Check(table.rows.size() + table.counts.LeftOutTotal() == HandInputs().size() && table.counts.Of(Basis::Made) == 1 &&
              table.counts.Of(Basis::Gathered) == 2 && table.counts.Of(Basis::VendorCap) == 2,
          "the counts add up to the items read");

    // The dump round trip, a malformed line refused
    std::string const text = DumpInputs(table.inputs, "core0");
    std::vector<ItemInput> back;
    std::string core;
    std::string error;
    Check(ParseInputs(text, back, core, error) && core == "core0" && back.size() == table.inputs.size() &&
              std::equal(back.begin(), back.end(), table.inputs.begin(),
                         [](ItemInput const& a, ItemInput const& b) { return a.Key() == b.Key(); }) &&
              DumpInputs(back, core) == text,
          "the inputs survive the dump: " + error);
    std::string broken = text;
    broken.replace(broken.find("\nI\t") + 3, 1, "x");
    Check(!ParseInputs(broken, back, core, error), "a malformed dump line is refused");

    // The command's line
    std::string const line = Line(*thread, *table.Input(THREAD));
    Check(line ==
              "AUCTION ref 2320 item 2320: 10 (10c) basis vendor band 0 | vendor sell 2 buy 50 per 5 stocked | "
              "by band 10 10 10 10 10",
          "the command's line: " + line);
    Check(Money(1234567) == "123g 45s 67c" && Money(99) == "99c" && Money(10000) == "1g 0s 0c", "money as g s c");
}

// ---- the search ----
constexpr std::uint32_t WORLD_ITEMS = 12;
constexpr std::uint32_t LONE_ITEMS = 1000;  // ids above every index source: valued by the shape
constexpr std::uint32_t SEEDS[] = {20261125, 7, 424242};
constexpr int WORLDS_PER_SEED = 400;

struct World
{
    std::vector<Recipe> recipes;
    std::vector<Process> processes;
    std::vector<VendorRow> vendors;
    std::vector<NodeRow> nodes;
    std::vector<ItemInput> inputs;

    [[nodiscard]] RecipeIndex Index() const
    {
        return RecipeIndex::FromRecords(recipes, {}, processes, {}, vendors, nodes);
    }
};

World RandomWorld(std::mt19937& rng)
{
    auto pick = [&](int percent) { return static_cast<int>(rng() % 100) < percent; };
    auto range = [&](std::uint32_t low, std::uint32_t high) { return low + rng() % (high - low + 1); };
    auto real = [&](double low, double high) { return std::uniform_real_distribution<double>(low, high)(rng); };
    World world;
    std::uint32_t entry = 1;
    for (std::uint32_t item = 1; item <= WORLD_ITEMS + 4; ++item)
    {
        std::uint32_t const id = item <= WORLD_ITEMS ? item : LONE_ITEMS + item;
        ItemInput input;
        input.id = id;
        input.name = "w" + std::to_string(id);
        input.classId = range(0, 9);
        input.quality = range(0, 5);
        input.itemLevel = range(1, 250);
        input.requiredLevel = range(0, 80);
        input.bonding = pick(10) ? BIND_ON_PICKUP : range(0, 3) == 1 ? 1 + range(1, 2) : 0;
        input.buyCount = range(1, 5);
        input.buyPrice = pick(20) ? 0 : range(1, 100000);
        input.sellPrice = pick(10) ? 0 : range(0, input.buyPrice / 2 + 50);
        if (id <= WORLD_ITEMS && pick(40))
        {
            std::uint32_t const stocked = pick(60) ? range(1, 3) : 0u;
            world.vendors.push_back(
                {id, input.buyPrice ? input.buyPrice : range(1, 500), input.buyCount, stocked, range(0, 2)});
            input.buyPrice = world.vendors.back().buyCopper;
            input.vendorStocked = stocked > 0;
        }
        if (id > WORLD_ITEMS && input.buyPrice && pick(40))
            input.vendorStocked = true;  // a vendor item no recipe uses: only the table's cap knows it
        if (id <= WORLD_ITEMS && pick(30))
            for (std::uint32_t row = range(1, 2); row; --row)
                world.nodes.push_back({range(1, 3), Id(pick(50) ? Skill::Mining : Skill::Herbalism), range(0, 450),
                                       entry++, range(0, 50), static_cast<float>(real(10.0, 200.0)), id,
                                       pick(10) ? 0.0 : real(0.2, 3.0)});
        world.inputs.push_back(input);
        if (pick(5))  // a second row of the same id with other prices: the order of the rows must not decide
        {
            ItemInput twin = input;
            twin.sellPrice = input.sellPrice + range(1, 500);
            twin.buyPrice = input.buyPrice + range(1, 500);
            twin.vendorStocked = false;
            world.inputs.push_back(twin);
        }
    }
    for (std::uint32_t spell = 1000 + range(0, 14); spell > 1000; --spell)
    {
        std::vector<ItemCount> reagents;
        for (std::uint32_t n = range(0, 3); n; --n)
            reagents.push_back({range(1, WORLD_ITEMS), range(1, 4)});
        world.recipes.push_back(MakeRecipe(pick(50) ? Skill::Blacksmithing : Skill::Inscription, spell,
                                           range(1, WORLD_ITEMS), std::move(reagents), range(0, 6000), range(1, 3),
                                           range(0, 450)));
    }
    for (std::uint32_t n = range(0, 3); n; --n)
    {
        Process process;
        process.kind = pick(20) ? ProcessKind::Disenchant : pick(50) ? ProcessKind::Prospect : ProcessKind::Mill;
        process.source = range(1, WORLD_ITEMS);
        process.skill = Id(Skill::Jewelcrafting);
        process.rank = range(0, 450);
        process.consumed = Recipes::PROSPECT_MILL_COUNT;
        for (std::uint32_t y = range(1, 2); y; --y)
            process.yields.push_back({range(1, WORLD_ITEMS), real(0.1, 3.0)});
        if (process.kind == ProcessKind::Disenchant)
            process.saleCopper = range(0, 2000);
        world.processes.push_back(process);
    }
    return world;
}

// A counterexample count per invariant, for the rule and for its control
struct Tally
{
    int belowSell{0};
    int aboveVendor{0};
    int aboveInputs{0};
    int orderDepends{0};
    int otherChanged{0};
    int shapeFalls{0};
    int rateFalls{0};
};

int BelowSell(Table const& table)
{
    int n = 0;
    for (ItemReference const& row : table.rows)
        if (row.copper < table.Input(row.id)->sellPrice)
            ++n;
    return n;
}

int AboveVendor(Table const& table)
{
    int n = 0;
    for (ItemReference const& row : table.rows)
    {
        ItemInput const& item = *table.Input(row.id);
        double const vendor = VendorPrice(item);
        if (vendor > 0.0 && static_cast<double>(row.copper) > std::max(vendor, double(item.sellPrice)) + 0.5)
            ++n;
    }
    return n;
}

// Whether a cycle of recipes or processes lies below an item: there the cost rule's depth bound decides how often a
// cycle that gives more than it takes is followed, so an input priced alone (one level more) can be cheaper than inside
bool CycleBelow(RecipeIndex const& index, std::uint32_t item)
{
    std::vector<std::uint32_t> path;
    std::vector<std::uint32_t> done;
    auto visit = [&](auto const& self, std::uint32_t at) -> bool
    {
        if (std::find(path.begin(), path.end(), at) != path.end())
            return true;
        if (std::find(done.begin(), done.end(), at) != done.end())
            return false;
        path.push_back(at);
        bool cycle = false;
        for (std::size_t const r : index.MadeBy(at))
            for (ItemCount const& reagent : index.Recipes()[r].reagents)
                cycle = cycle || self(self, reagent.item);
        for (std::size_t const p : index.YieldedBy(at))
            cycle = cycle || self(self, index.Processes()[p].source);
        path.pop_back();
        done.push_back(at);
        return cycle;
    };
    return visit(visit, item);
}

// A made thing at its band: never above its cheapest feasible recipe's inputs (their getting costs at that band)
// plus the cast, unless the floor raised it. Counted apart where a recipe cycle lies below (cycles, the second count)
int AboveInputs(Table const& table, RecipeIndex const& index, int* cycles = nullptr)
{
    BandPricers const pricers(index, BANDS);
    int n = 0;
    for (ItemReference const& row : table.rows)
    {
        double best = -1.0;
        for (std::size_t const at : index.MadeBy(row.id))
        {
            Recipe const& recipe = index.Recipes()[at];
            if (TypicalSkill(row.band) < recipe.minRank)
                continue;
            double sum = Materials::CastMinutes(static_cast<float>(recipe.castMs)) * Earn(row.band);
            bool feasible = true;
            for (ItemCount const& reagent : recipe.reagents)
            {
                Costs const costs = pricers.Of(reagent.item, pricers.SourcesOf(reagent.item));
                if (costs.cost[row.band].Blocked())
                    feasible = false;
                else
                    sum += costs.copper[row.band] * reagent.count;
            }
            if (feasible)
            {
                sum /= std::max<std::uint32_t>(recipe.productCount, 1);
                best = best < 0.0 ? sum : std::min(best, sum);
            }
        }
        if (best >= 0.0 &&
            static_cast<double>(row.copper) > std::max(best, double(table.Input(row.id)->sellPrice)) * 1.000001 + 0.5)
        {
            if (!CycleBelow(index, row.id))
                ++n;
            else if (cycles)
                ++*cycles;
        }
    }
    return n;
}

int OtherChanged(Table const& all, Table const& without, std::uint32_t removed)
{
    int n = 0;
    for (ItemReference const& row : without.rows)
    {
        ItemReference const* before = all.Find(row.id);
        if (row.id != removed && (!before || !SameRow(*before, row)))
            ++n;
    }
    return n;
}

// Raising quality or item level never lowers a lone item's value
int ShapeFalls(std::vector<ItemInput> const& inputs, RecipeIndex const& index, Fault fault)
{
    BandPricers const pricers(index, BANDS);
    int n = 0;
    for (ItemInput const& item : inputs)
    {
        if (item.id < LONE_ITEMS || LeftOutOf(item) != LeftOut::None)
            continue;
        Costs const costs = pricers.Of(item.id, 0);
        std::uint64_t const base = DecideVariant(item, costs, 0, fault).copper;
        ItemInput better = item;
        better.quality = std::min<std::uint32_t>(item.quality + 1, 7);
        ItemInput higher = item;
        higher.itemLevel = item.itemLevel + 15;
        if (DecideVariant(better, costs, 0, fault).copper < base ||
            DecideVariant(higher, costs, 0, fault).copper < base)
            ++n;
    }
    return n;
}

// A gathered material's copper per minute never falls from one band to a higher one
int RateFalls(Table const& table, RecipeIndex const& index, WorldTravel::SlotBandTable const& bands)
{
    BandPricers const pricers(index, bands);
    int n = 0;
    for (ItemReference const& row : table.rows)
    {
        Costs const costs = pricers.Of(row.id, row.sources);
        double previous = -1.0;
        for (std::uint32_t band = 0; band < BAND_COUNT; ++band)
        {
            Materials::Cost const& cost = costs.cost[band];
            if (cost.kind != Materials::Kind::Gathered || cost.minutes <= 0.0)
                continue;
            double const rate = costs.copper[band] / cost.minutes;
            if (rate < previous * (1.0 - 1e-6))
                ++n;
            previous = rate;
        }
    }
    return n;
}

template <typename T>
void Shuffle(std::vector<T>& rows, std::mt19937& rng)
{
    std::shuffle(rows.begin(), rows.end(), rng);
}

void Search()
{
    Tally rule;
    Tally control;
    int worlds = 0;
    int madeRows = 0;
    int shapedRows = 0;
    int gatheredRows = 0;
    int cycleBound = 0;  // invariant 3 above the inputs where a gainful cycle and the depth bound decide
    for (std::uint32_t const seed : SEEDS)
    {
        std::mt19937 rng(seed);
        for (int w = 0; w < WORLDS_PER_SEED; ++w, ++worlds)
        {
            World const world = RandomWorld(rng);
            RecipeIndex const index = world.Index();
            Table const table = Build(world.inputs, index, BANDS);
            madeRows += table.counts.Of(Basis::Made);
            shapedRows += table.counts.Of(Basis::Shaped);
            gatheredRows += table.counts.Of(Basis::Gathered);

            // The harness's composition is the rule's
            Check(SameTable(table, BuildVariant(world.inputs, index, Fault::None)), "the harness's rule equals Build");

            rule.belowSell += BelowSell(table);
            control.belowSell += BelowSell(BuildVariant(world.inputs, index, Fault::NoClamp));
            rule.aboveVendor += AboveVendor(table);
            control.aboveVendor += AboveVendor(BuildVariant(world.inputs, index, Fault::NoCap));
            rule.aboveInputs += AboveInputs(table, index, &cycleBound);
            control.aboveInputs += AboveInputs(BuildVariant(world.inputs, index, Fault::MadeMargin), index);

            // Shuffled item, recipe, process, vendor and node rows
            World shuffled = world;
            Shuffle(shuffled.inputs, rng);
            Shuffle(shuffled.recipes, rng);
            Shuffle(shuffled.processes, rng);
            Shuffle(shuffled.vendors, rng);
            Shuffle(shuffled.nodes, rng);
            RecipeIndex const shuffledIndex = shuffled.Index();
            rule.orderDepends += SameTable(table, Build(shuffled.inputs, shuffledIndex, BANDS)) ? 0 : 1;
            control.orderDepends += SameTable(BuildVariant(world.inputs, index, Fault::FirstRowWins),
                                              BuildVariant(shuffled.inputs, shuffledIndex, Fault::FirstRowWins))
                                        ? 0
                                        : 1;

            // One item less: no other item's value moves
            std::vector<ItemInput> fewer = world.inputs;
            std::uint32_t const removed = fewer[rng() % fewer.size()].id;
            fewer.erase(std::remove_if(fewer.begin(), fewer.end(),
                                       [removed](ItemInput const& item) { return item.id == removed; }),
                        fewer.end());
            rule.otherChanged += OtherChanged(table, Build(fewer, index, BANDS), removed);
            control.otherChanged += OtherChanged(BuildVariant(world.inputs, index, Fault::PerItemConstant),
                                                 BuildVariant(fewer, index, Fault::PerItemConstant), removed);

            rule.shapeFalls += ShapeFalls(world.inputs, index, Fault::None);
            control.shapeFalls += ShapeFalls(world.inputs, index, Fault::InvertedLevel);

            WorldTravel::SlotBandTable swapped = BANDS;
            std::reverse(swapped.begin(), swapped.end());
            rule.rateFalls += RateFalls(table, index, BANDS);
            control.rateFalls += RateFalls(table, index, swapped);
        }
    }
    std::cout << "search: " << worlds << " worlds (" << madeRows << " made, " << gatheredRows << " gathered, "
              << shapedRows << " shaped rows); counterexamples (rule / control): sell " << rule.belowSell << "/"
              << control.belowSell << ", vendor " << rule.aboveVendor << "/" << control.aboveVendor << ", inputs "
              << rule.aboveInputs << "/" << control.aboveInputs << " (+" << cycleBound << " below a cycle), order "
              << rule.orderDepends << "/" << control.orderDepends << ", others " << rule.otherChanged << "/"
              << control.otherChanged << ", shape " << rule.shapeFalls << "/" << control.shapeFalls << ", rate "
              << rule.rateFalls << "/" << control.rateFalls << '\n';
    Check(rule.belowSell == 0, "1. never below the vendor sell price");
    Check(control.belowSell > 0, "1. control: without the clamp a value falls below the sell price (caught)");
    Check(rule.aboveVendor == 0, "2. a vendor-stocked item never above its vendor price");
    Check(control.aboveVendor > 0, "2. control: without the cap a stocked item exceeds its vendor price (caught)");
    Check(rule.aboveInputs == 0, "3. a made thing never above its inputs plus casts");
    Check(control.aboveInputs > 0, "3. control: a margin on made things exceeds the inputs (caught)");
    Check(rule.orderDepends == 0, "4. the same data in any order gives the same table");
    Check(control.orderDepends > 0, "4. control: the first row of an id winning depends on the order (caught)");
    Check(rule.otherChanged == 0, "5. one item less never moves another item's value");
    Check(control.otherChanged > 0, "5. control: a per-item constant moves the others (caught)");
    Check(rule.shapeFalls == 0, "6. quality and item level never lower a shaped value");
    Check(control.shapeFalls > 0, "6. control: an inverted level multiplier lowers it (caught)");
    Check(rule.rateFalls == 0, "7. a higher band's rate never makes gathering cheaper per minute");
    Check(control.rateFalls > 0, "7. control: swapped rates make it cheaper (caught)");
}

// ---- WA2b: asking and paying (AuctionPricing.h) ----
Fees const FACTION_FEES{{5, 5}, {1.0f, 1.0f, 1.0f}};
Fees const GOBLIN_FEES{{25, 15}, {1.0f, 1.0f, 1.0f}};
constexpr std::uint32_t SECONDS_12H = 12 * 3600;
constexpr std::uint32_t SECONDS_24H = 24 * 3600;
constexpr std::uint32_t SECONDS_48H = 48 * 3600;
using AuctionWords::Reason;

// The economy reference's auction cases: hub docs/economy-reference/cases/auction_vendor.json, read 2026-11-25, and
// the vendor payout of worn gear from gear_trade.json (WA3a), copied with their ids and never regenerated from the
// functions under test. Of bids_and_cancellation and two_listings only the checks a function here answers
struct CaseCheck
{
    char const* id;
    char const* name;
    std::int64_t num;
    std::int64_t den;
};

constexpr CaseCheck CASES[] = {
    {"auction_vendor.linen_posting_terms", "vendor_exit", 260, 1},
    {"auction_vendor.linen_posting_terms", "raw_12h_rational", 39, 1},
    {"auction_vendor.linen_posting_terms", "due_12h", 100, 1},
    {"auction_vendor.linen_posting_terms", "due_24h", 100, 1},
    {"auction_vendor.linen_posting_terms", "due_48h", 156, 1},
    {"auction_vendor.linen_posting_terms", "zero_sell_price_deposit_any_valid_duration", 100, 1},
    {"auction_vendor.house_selection", "template_sell_total", 975, 1},
    {"auction_vendor.house_selection", "faction_or_adopted_deposit_12h", 146, 1},
    {"auction_vendor.house_selection", "neutral_or_present_lab6_deposit_12h", 731, 1},
    {"auction_vendor.house_selection", "faction_or_adopted_cut", 99, 1},
    {"auction_vendor.house_selection", "neutral_or_present_lab6_cut", 299, 1},
    {"auction_vendor.house_selection", "difference_in_required_posting_cash", 585, 1},
    {"auction_vendor.house_selection", "faction_net_if_sold", 1900, 1},
    {"auction_vendor.house_selection", "neutral_net_if_sold", 1700, 1},
    {"auction_vendor.rate_rounding", "expiry", 6, 1},
    {"auction_vendor.rate_rounding", "base_cut", 99, 1},
    {"auction_vendor.rate_rounding", "cut_at_sensitivity_rate", 148, 1},
    {"auction_vendor.rate_rounding", "scaled_minimum", 50, 1},
    {"auction_vendor.rate_rounding", "deposit_from_selected_nominal_duration", 73, 1},
    {"auction_vendor.sale_expiry_threshold", "comparison_unit_bid", 311, 20},
    {"auction_vendor.sale_expiry_threshold", "whole_lot_cut", 15, 1},
    {"auction_vendor.sale_expiry_threshold", "immediate_post_cash", -100, 1},
    {"auction_vendor.sale_expiry_threshold", "seller_mail_after_sale", 396, 1},
    {"auction_vendor.sale_expiry_threshold", "sale_net_from_listing", 296, 1},
    {"auction_vendor.sale_expiry_threshold", "net_over_immediate_vendor_if_sold", 36, 1},
    {"auction_vendor.sale_expiry_threshold", "expiry_with_no_sale_cash", -100, 1},
    {"auction_vendor.sale_expiry_threshold", "expiry_then_vendor_total", 160, 1},
    {"auction_vendor.sale_expiry_threshold", "probability_for_parity", 25, 34},
    {"auction_vendor.sale_expiry_threshold", "conditional_mean_including_failed_exit", 262, 1},
    {"auction_vendor.sale_expiry_threshold", "conditional_premium_over_vendor", 2, 1},
    {"auction_vendor.sale_expiry_threshold", "successful_cash_delay", 3600, 1},
    {"auction_vendor.bids_and_cancellation", "starting_12h_deposit", 180, 1},
    {"auction_vendor.bids_and_cancellation", "minimum_raise", 75, 1},
    {"auction_vendor.bids_and_cancellation", "minimum_next_total", 1576, 1},
    {"auction_vendor.bids_and_cancellation", "incumbent_sample_cash_short_of_precheck", 1476, 1},
    {"auction_vendor.bids_and_cancellation", "buyout_seller_net", 1900, 1},
    {"auction_vendor.bids_and_cancellation", "minimum_raise_on_tiny_bid", 1, 1},
    {"auction_vendor.two_listings", "large_unit", 15, 1},
    {"auction_vendor.two_listings", "small_unit", 18, 1},
    {"auction_vendor.two_listings", "large_cash_short", 100, 1},
    {"auction_vendor.two_listings", "small_cash_left", 128, 1},
    {"auction_vendor.two_listings", "large_surplus", 16, 1},
    {"auction_vendor.two_listings", "large_net_if_funded_and_surplus_immediately_vendored", 92, 1},
    {"auction_vendor.two_listings", "small_net_for_need", 72, 1},
    {"auction_vendor.two_listings", "difference_with_surplus_exit", 20, 1},
    {"gear_trade.sell_boundary", "wear seven: deduction equals sell price", 6, 1},
    {"gear_trade.sell_boundary", "wear seven: paid", 0, 1},
    {"gear_trade.sell_boundary", "wear eight: deduction exceeds sell price", 7, 1},
    {"gear_trade.sell_boundary", "wear eight: strict-overrun branch paid", 1, 1},
};

AskInput Lot(std::uint32_t count, std::uint32_t sellPrice, Fees const& fees = FACTION_FEES)
{
    AskInput in;
    in.count = count;
    in.sellPrice = sellPrice;
    in.vendorPayout = static_cast<std::uint64_t>(count) * sellPrice;
    in.fees = fees;
    return in;
}

OfferInput Offer(std::uint32_t id, std::uint32_t count, std::uint32_t buyout, std::uint32_t need,
                 std::uint64_t excessUnitWorth, std::int64_t endTime)
{
    OfferInput offer;
    offer.line.id = id;
    offer.line.count = count;
    offer.line.buyout = buyout;
    offer.line.endTime = endTime;
    offer.unitsNeeded = need;
    offer.excessUnitWorth = excessUnitWorth;
    return offer;
}

// The case two_listings' lots for a need of 4 linen, the surplus worth its vendor payout
OfferInput const LARGE = Offer(1, 20, 300, 4, 13, SECONDS_12H);
OfferInput const SMALL = Offer(2, 4, 72, 4, 13, SECONDS_12H);

BuyInput Wallet(std::uint64_t ceilingPerUnit, std::uint64_t spendable)
{
    BuyInput in;
    in.ceilingPerUnit = ceilingPerUnit;
    in.spendable = spendable;
    in.ownGuid = 9;
    return in;
}

// Each case check answered by the functions under test
std::map<std::string, double> OurCaseValues()
{
    std::map<std::string, double> v;
    auto put = [&v](char const* id, char const* name, double value)
    { v[std::string("auction_vendor.") + id + "." + name] = value; };
    auto d = [](std::uint64_t copper) { return static_cast<double>(copper); };

    put("linen_posting_terms", "vendor_exit", d(SellTotal(Lot(20, 13))));
    put("linen_posting_terms", "raw_12h_rational", d(RawDeposit(FACTION_FEES, 13, 20, SECONDS_12H)));
    put("linen_posting_terms", "due_12h", d(Deposit(FACTION_FEES, 13, 20, SECONDS_12H)));
    put("linen_posting_terms", "due_24h", d(Deposit(FACTION_FEES, 13, 20, SECONDS_24H)));
    put("linen_posting_terms", "due_48h", d(Deposit(FACTION_FEES, 13, 20, SECONDS_48H)));
    put("linen_posting_terms", "zero_sell_price_deposit_any_valid_duration",
        d(Deposit(FACTION_FEES, 0, 20, SECONDS_12H)));

    std::uint32_t const faction = Deposit(FACTION_FEES, 75, 13, SECONDS_12H);
    std::uint32_t const neutral = Deposit(GOBLIN_FEES, 75, 13, SECONDS_12H);
    put("house_selection", "template_sell_total", d(SellTotal(Lot(13, 75))));
    put("house_selection", "faction_or_adopted_deposit_12h", d(faction));
    put("house_selection", "neutral_or_present_lab6_deposit_12h", d(neutral));
    put("house_selection", "faction_or_adopted_cut", d(Cut(FACTION_FEES, 1999)));
    put("house_selection", "neutral_or_present_lab6_cut", d(Cut(GOBLIN_FEES, 1999)));
    put("house_selection", "difference_in_required_posting_cash", d(neutral) - d(faction));
    put("house_selection", "faction_net_if_sold", 1999.0 - d(Cut(FACTION_FEES, 1999)));
    put("house_selection", "neutral_net_if_sold", 1999.0 - d(Cut(GOBLIN_FEES, 1999)));

    Fees const rated{{5, 5}, {0.5f, 1.5f, 0.5f}};
    Fees plainCut = rated;
    plainCut.rates.cut = 1.0f;
    put("rate_rounding", "expiry", d(ExpirySeconds(rated, 12)) / SECONDS_PER_HOUR);
    put("rate_rounding", "base_cut", d(Cut(plainCut, 1999)));
    put("rate_rounding", "cut_at_sensitivity_rate", d(Cut(rated, 1999)));
    put("rate_rounding", "scaled_minimum", d(Deposit(rated, 0, 13, SECONDS_12H)));
    put("rate_rounding", "deposit_from_selected_nominal_duration", d(Deposit(rated, 75, 13, SECONDS_12H)));

    std::uint64_t const gross = 311;
    std::uint64_t const vendor = SellTotal(Lot(20, 13));
    std::uint64_t const deposit = Deposit(FACTION_FEES, 13, 20, SECONDS_12H);
    std::uint64_t const cut = Cut(FACTION_FEES, 311);
    double const mean = ListingValue(0.75, gross, cut, deposit, vendor);
    put("sale_expiry_threshold", "comparison_unit_bid", d(gross) / 20.0);
    put("sale_expiry_threshold", "whole_lot_cut", d(cut));
    put("sale_expiry_threshold", "immediate_post_cash", -d(deposit));
    put("sale_expiry_threshold", "seller_mail_after_sale", d(gross + deposit - cut));
    put("sale_expiry_threshold", "sale_net_from_listing", d(gross - cut));
    put("sale_expiry_threshold", "net_over_immediate_vendor_if_sold", d(gross - cut) - d(vendor));
    put("sale_expiry_threshold", "expiry_with_no_sale_cash", -d(deposit));
    put("sale_expiry_threshold", "expiry_then_vendor_total", d(vendor) - d(deposit));
    put("sale_expiry_threshold", "probability_for_parity", BreakEvenP(gross, cut, deposit, vendor));
    put("sale_expiry_threshold", "conditional_mean_including_failed_exit", mean);
    put("sale_expiry_threshold", "conditional_premium_over_vendor", mean - d(vendor));
    put("sale_expiry_threshold", "successful_cash_delay", 3600.0);  // the case's input delay: no rule's result

    AuctionIndex::AuctionFacts bidOn;
    bidOn.startBid = 1500;
    bidOn.bid = 1501;
    OfferInput outbid = Offer(3, 3, 0, 3, 0, 3600);
    outbid.line.startBid = 1500;
    outbid.line.bid = 1501;
    outbid.line.bidder = 77;
    BuyChoice const shortOfCash = ChooseBuy({outbid}, Wallet(5000, 100));
    put("bids_and_cancellation", "starting_12h_deposit", d(Deposit(FACTION_FEES, 400, 3, SECONDS_12H)));
    put("bids_and_cancellation", "minimum_raise", d(OutBid(1501)));
    put("bids_and_cancellation", "minimum_next_total", d(NextBid(bidOn)));
    put("bids_and_cancellation", "incumbent_sample_cash_short_of_precheck",
        shortOfCash.kind == BuyKind::Skip && shortOfCash.reason == Reason::Budget ? d(NextBid(bidOn)) - 100.0 : -1.0);
    put("bids_and_cancellation", "buyout_seller_net", 2000.0 - d(Cut(FACTION_FEES, 2000)));
    put("bids_and_cancellation", "minimum_raise_on_tiny_bid", d(OutBid(7)));

    BuyChoice const largeOnly = ChooseBuy({LARGE}, Wallet(100, 200));
    BuyChoice const both = ChooseBuy({LARGE, SMALL}, Wallet(100, 200));
    put("two_listings", "large_unit", d(AuctionIndex::UnitPrice(300, 20)));
    put("two_listings", "small_unit", d(AuctionIndex::UnitPrice(72, 4)));
    put("two_listings", "large_cash_short",
        largeOnly.kind == BuyKind::Skip && largeOnly.reason == Reason::Budget ? 300.0 - 200.0 : -1.0);
    put("two_listings", "small_cash_left", both.kind == BuyKind::Buyout ? 200.0 - d(both.price) : -1.0);
    put("two_listings", "large_surplus", d(LotUnits(LARGE.line) - Covered(LARGE)));
    put("two_listings", "large_net_if_funded_and_surplus_immediately_vendored", d(LineCost(LARGE, 300)));
    put("two_listings", "small_net_for_need", d(LineCost(SMALL, 72)));
    put("two_listings", "difference_with_surplus_exit", d(LineCost(LARGE, 300)) - d(LineCost(SMALL, 72)));

    // The Outfitter Belt (2186): sell 6, multiplier 1, the quality's factor the case's q
    float const beltQuality = 13421773.0f / 16777216.0f;
    auto gear = [&v](char const* name, double value) { v[std::string("gear_trade.sell_boundary.") + name] = value; };
    gear("wear seven: deduction equals sell price", d(DurabilityDeduction(7, 1, beltQuality)));
    gear("wear seven: paid", d(VendorPayout(6, 1, 7, 1, beltQuality)));
    gear("wear eight: deduction exceeds sell price", d(DurabilityDeduction(8, 1, beltQuality)));
    gear("wear eight: strict-overrun branch paid", d(VendorPayout(6, 1, 8, 1, beltQuality)));
    return v;
}

void CheckCases()
{
    std::map<std::string, double> const ours = OurCaseValues();
    std::size_t met = 0;
    for (CaseCheck const& check : CASES)
    {
        std::string const key = std::string(check.id) + "." + check.name;
        auto const found = ours.find(key);
        double const expected = static_cast<double>(check.num) / static_cast<double>(check.den);
        bool const ok = found != ours.end() &&
                        (check.den == 1 ? found->second == expected : std::abs(found->second - expected) <= 1e-12);
        std::ostringstream what;
        what << "case " << key << ": expected " << check.num << (check.den == 1 ? "" : "/" + std::to_string(check.den))
             << ", ours " << (found == ours.end() ? std::string("unmapped") : std::to_string(found->second));
        Check(ok, what.str());
        met += ok ? 1 : 0;
    }
    std::cout << "cases: " << met << " of " << std::size(CASES) << " economy reference values met\n";
}

// WA3a: what listing a lot as stock is worth. The case sale_expiry_threshold's lot terms follow from its sell total, so
// one unit that sells for 260 with a typical price of 311 is its 20 linen; p from the history, (sales + 1) / (sales +
// expiries + 2): 2 sales give 3/4, none 1/2, 24 sales and 8 expiries 25/34
void CheckStockWorth()
{
    AuctionConfig const config;
    auto lot = [](std::uint32_t sales, std::uint32_t expiries)
    {
        AskInput in = Lot(1, 260);
        in.typical.unitPrice = 311;
        in.typical.sample = sales;
        in.typical.weight = static_cast<double>(sales);
        in.typical.expiries = expiries;
        return in;
    };
    AskInput const threeQuarters = lot(2, 0);
    AskInput const half = lot(0, 0);
    AskInput const parity = lot(24, 8);
    Check(SaleChance(threeQuarters.typical) == 0.75 && SaleChance(half.typical) == 0.5 &&
              Near(SaleChance(parity.typical), 25.0 / 34.0),
          "stock: the planted histories give p = 3/4, 1/2 and 25/34");
    std::uint32_t const atThreeQuarters = StockWorth(threeQuarters, config);
    std::uint32_t const atHalf = StockWorth(half, config);
    std::uint32_t const atParity = StockWorth(parity, config);
    std::cout << "stock worth of the case's lot (vendor 260): p 3/4 " << atThreeQuarters << ", p 1/2 " << atHalf
              << ", p 25/34 " << atParity << "\n";
    Check(atThreeQuarters == 262, "stock: case sale_expiry_threshold at p = 3/4 is worth 262, above the vendor's 260");
    Check(atHalf <= 260 && atParity <= 260,
          "stock: at p = 1/2 and at the break-even p = 25/34 the lot is worth no more than the vendor's 260");
    AskResult const lifted = Ask(half, config);
    Check(lifted.kind == AskResult::Kind::List && lifted.ask > 311 && lifted.expected > 260.0 && atHalf == 0,
          "stock: control: Ask lists the p = 1/2 lot at its floor above the evidence, worth more than the vendor; "
          "stock asks the evidence (caught)");
}

bool Equal(AskResult const& a, AskResult const& b)
{
    return a.kind == b.kind && a.reason == b.reason && a.ask == b.ask && a.floor == b.floor &&
           a.startBid == b.startBid && a.deposit == b.deposit && a.cut == b.cut && a.hours == b.hours && a.p == b.p;
}

bool Equal(BuyChoice const& a, BuyChoice const& b)
{
    return a.kind == b.kind && a.price == b.price && a.auctionId == b.auctionId && a.reason == b.reason;
}

void CheckPricingHand()
{
    AuctionConfig const config;
    Check(OutBid(0) == 1 && OutBid(19) == 1 && OutBid(100) == 5 && OutBid(1501) == 75, "the outbid step");
    Check(DurationHours(1) == 12 && DurationHours(12) == 12 && DurationHours(24) == 24 && DurationHours(47) == 24 &&
              DurationHours(48) == 48 && DurationHours(100) == 48,
          "the listing durations the client offers");
    Check(Deposit(FACTION_FEES, 0, 20, SECONDS_24H) == 100 && Deposit(FACTION_FEES, 0, 1, SECONDS_48H) == 100,
          "no sell price: the minimum at any duration");
    Check(ExpirySeconds(FACTION_FEES, 24) == SECONDS_24H, "rate 1: a listing runs its nominal time");
    Check(Near(SaleChance({}), 0.5), "no history: the sale chance 1/2");
    AuctionIndex::TypicalPrice history;
    history.sample = 5;
    history.expiries = 1;
    Check(Near(SaleChance(history), 0.75), "5 sales, 1 expiry: the sale chance 3/4");

    // The case's linen: worth listing at 3/4, not at 0.7 (planted beside the case)
    Check(WorthListing(0.75, 311, 15, 100, 260), "linen at 311: worth listing at p 3/4");
    Check(!WorthListing(0.7, 311, 15, 100, 260), "linen at 311: not worth listing at p 0.7");
    Check(std::isinf(BreakEvenP(270, 15, 100, 260)), "a sale netting less than the vendor: never worth listing");
    std::uint64_t const floor = ListFloor(FACTION_FEES, 0.75, 13, 20, 260, 12);
    Check(floor <= 311 && WorthListing(0.75, floor, Cut(FACTION_FEES, static_cast<std::uint32_t>(floor)), 100, 260) &&
              !WorthListing(0.75, floor - 1, Cut(FACTION_FEES, static_cast<std::uint32_t>(floor - 1)), 100, 260),
          "the linen floor at 12 h: the lowest ask worth listing (" + std::to_string(floor) + ")");
    Check(ListFloor(FACTION_FEES, 0.0, 13, 20, 260, 12) == NEVER_LISTS, "a sale chance of 0: nothing lists");
    Fees allCut = FACTION_FEES;
    allCut.row.cutPercent = 100;
    Check(ListFloor(allCut, 0.9, 13, 20, 260, 12) == NEVER_LISTS, "a cut of everything: nothing lists");

    // Asks
    AskInput linen = Lot(20, 13);
    linen.reference = 16;
    AskResult const cheap = Ask(linen, config);
    Check(cheap.kind == AskResult::Kind::List && cheap.ask == cheap.floor && cheap.floor > 320,
          "no evidence, the reference value (320) below the floor: listed at the floor (" +
              std::to_string(cheap.floor) + ")");
    linen.reference = 20;
    AskResult const plain = Ask(linen, config);
    Check(plain.kind == AskResult::Kind::List && plain.ask == 400 && plain.hours == 24 &&
              plain.startBid >= plain.floor && plain.startBid <= plain.ask,
          "no evidence: the reference value at 24 h, a start bid between floor and buyout");
    linen.cheapestUnitBuyout = 10;
    AskResult const undercut = Ask(linen, config);
    Check(undercut.kind == AskResult::Kind::Hold && undercut.reason == Reason::BelowFloor,
          "offers below the floor: hold");
    linen.typical.unitPrice = 30;
    linen.typical.sample = 5;
    linen.cheapestUnitBuyout = 25;
    AskResult const offered = Ask(linen, config);
    Check(offered.kind == AskResult::Kind::List && offered.ask == 25 * 20 * 97 / 100,
          "with evidence at the auctioneer: 3 % under the cheapest buyout (" + std::to_string(offered.ask) + ")");
    linen.expiredSteps = 1;
    AskResult const relisted = Ask(linen, config);
    Check(relisted.kind == AskResult::Kind::List && relisted.reason == Reason::Expired && relisted.hours == 48 &&
              relisted.ask <= offered.ask,
          "after an expiry: listed again for 48 h, not higher");
    AskInput ore = Lot(10, 5);
    ore.reference = 5;
    AskResult const atFloor = Ask(ore, config);
    ore.expiredSteps = 1;
    ore.typical.expiries = 1;
    AskResult const afterExpiry = Ask(ore, config);
    Check(atFloor.kind == AskResult::Kind::List && atFloor.ask == atFloor.floor &&
              afterExpiry.kind == AskResult::Kind::Hold && afterExpiry.reason == Reason::BelowFloor &&
              afterExpiry.floor > atFloor.floor,
          "a lot listed at the floor expired: the index's expiry lifts the floor, so it is held, not listed higher");
    linen.expiredSteps = config.RelistLimit + 1;
    AskResult const spent = Ask(linen, config);
    Check(spent.kind == AskResult::Kind::Hold && spent.reason == Reason::Expired,
          "past the relist limit: hold for the vendor");
    AskInput keep = Lot(1, 100);
    keep.reference = 150;
    keep.keepWorth = 900;
    AskResult const kept = Ask(keep, config);
    Check(kept.floor == 900 && kept.ask == 900 && kept.kind == AskResult::Kind::List, "the keep worth is a floor");
    keep.typical.unitPrice = 1000;
    keep.typical.sample = 3;
    Check(Ask(keep, config).kind == AskResult::Kind::List && Ask(keep, config).ask == 1000,
          "a typical price above the keep worth lists");

    // Buying
    OfferInput const one = Offer(10, 1, 100, 1, 0, 43200);
    Check(ChooseBuy({one}, Wallet(150, 1000)).kind == BuyKind::Buyout, "a rich bot buys out under its ceiling");
    Check(Ceiling({150.0, -1.0, 1.0f}) == 150 && Ceiling({150.0, -1.0, 2.0f}) == 75 &&
              Ceiling({0.0, 50.0, 1.0f}) == 0 && Ceiling({150.0, 40.0, 1.0f}) == 40,
          "the ceiling: the lesser of worth and getting, over the factor; no use: 0");
    BuyChoice const poorSkip = ChooseBuy({one}, Wallet(Ceiling({150.0, -1.0, 2.0f}), 1000));
    Check(poorSkip.kind == BuyKind::Skip && poorSkip.reason == Reason::OverCeiling,
          "a poor bot of the same state refuses what the rich one takes");
    OfferInput bidOnly = Offer(11, 1, 0, 1, 0, 3600);
    bidOnly.line.startBid = 50;
    BuyInput capped = Wallet(150, 1000);
    capped.openBids = 3;
    Check(ChooseBuy({bidOnly}, capped).reason == Reason::BidCap, "three open bids: no fourth");
    capped.openBids = 2;
    BuyChoice const bid = ChooseBuy({bidOnly}, capped);
    Check(bid.kind == BuyKind::Bid && bid.price == 50, "a bid at the start bid within the patience");
    OfferInput late = bidOnly;
    late.line.endTime = 3 * 3600;
    Check(ChooseBuy({late}, capped).kind == BuyKind::Skip, "an auction ending after the patience: no bid");
    OfferInput leading = bidOnly;
    leading.line.bid = 60;
    leading.line.bidder = 9;
    Check(ChooseBuy({leading}, capped).kind == BuyKind::Skip, "never a bid on an auction the bot leads");
    OfferInput incumbent = Offer(12, 3, 2000, 3, 0, 3600);
    incumbent.line.startBid = 1500;
    incumbent.line.bid = 1576;
    incumbent.line.bidder = 9;
    BuyChoice const debitOnly = ChooseBuy({incumbent}, Wallet(5000, 424));
    Check(debitOnly.kind == BuyKind::Skip && debitOnly.reason == Reason::Budget,
          "the leader's buyout: the debit (424) in hand is not enough, the full 2000 is needed");
    Check(ChooseBuy({incumbent}, Wallet(5000, 2000)).kind == BuyKind::Buyout, "the leader's buyout with 2000 in hand");
    incumbent.line.buyout = 0;
    Check(ChooseBuy({incumbent}, Wallet(5000, 100)).kind == BuyKind::Skip, "the incumbent with 100 does not raise");
    std::vector<std::size_t> const order = Rank({LARGE, SMALL});
    BuyChoice const both = ChooseBuy({LARGE, SMALL}, Wallet(100, 1000));
    Check(order.front() == 1 && both.auctionId == 2,
          "the big lot cheaper per unit loses to the small one when its excess is worth less than the saving");
    Check(ChooseBuy({}, Wallet(100, 1000)).reason == Reason::NoOffer, "no offer: skip");
}

// ---- the search over markets and bots ----
struct Dice
{
    std::mt19937& rng;
    bool Pick(int percent) { return static_cast<int>(rng() % 100) < percent; }
    std::uint32_t Range(std::uint32_t low, std::uint32_t high) { return low + rng() % (high - low + 1); }
    double Real(double low, double high) { return std::uniform_real_distribution<double>(low, high)(rng); }
};

Fees RandomFees(Dice& dice)
{
    Fees fees = dice.Pick(50) ? FACTION_FEES : GOBLIN_FEES;
    if (dice.Pick(40))
    {
        auto rate = [&dice]() { return static_cast<float>(dice.Real(0.5, 2.0)); };
        fees.rates.deposit = rate();
        fees.rates.cut = rate();
        fees.rates.time = rate();
    }
    return fees;
}

AuctionConfig RandomConfig(Dice& dice)
{
    AuctionConfig config;
    if (dice.Pick(50))
        return config;
    config.UndercutPercent = dice.Range(0, 10);
    config.RelistStepPercent = dice.Range(1, 30);
    config.RelistLimit = dice.Range(0, 4);
    config.FirstDurationHours = LISTING_HOURS[dice.Range(0, 2)];
    config.RelistDurationHours = LISTING_HOURS[dice.Range(0, 2)];
    return config;
}

AskInput RandomAsk(Dice& dice)
{
    AskInput in;
    in.count = dice.Pick(40) ? 1 : dice.Pick(90) ? dice.Range(2, 20) : dice.Range(21, 200);
    in.sellPrice = dice.Pick(10) ? 0 : dice.Pick(60) ? dice.Range(1, 100) : dice.Range(101, 20000);
    std::uint64_t const sellTotal = SellTotal(in);
    in.vendorPayout = dice.Pick(80) ? sellTotal : sellTotal * dice.Range(0, 100) / 100;
    in.keepWorth = dice.Pick(70) ? 0 : dice.Range(0, static_cast<std::uint32_t>(3 * sellTotal + 500));
    in.reference = dice.Pick(10) ? 0 : in.sellPrice + dice.Range(0, 8 * in.sellPrice + 200);
    double const base = static_cast<double>(std::max<std::uint64_t>(in.reference, 1));
    if (dice.Pick(60))
    {
        in.typical.unitPrice = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(base * dice.Real(0.3, 3.0)));
        in.typical.sample = dice.Range(1, 20);
    }
    in.typical.expiries = dice.Range(0, 12);
    if (dice.Pick(50))
        in.cheapestUnitBuyout = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(base * dice.Real(0.2, 2.0)));
    in.expiredSteps = dice.Pick(60) ? 0 : dice.Range(1, 5);
    in.quickSale = dice.Pick(20);
    in.fees = RandomFees(dice);
    return in;
}

// The rule recomposed from its pieces, and broken variants for the controls
enum class AskFault
{
    None,
    NoClamp,          // 1: the floor clamp skipped
    VendorFloor,      // 2: lists whenever the ask beats the vendor, cut and deposit ignored
    StepUp,           // 6: an expiry raises the ask
    RaiseToUndercut,  // 7: the ask follows the cheapest offer up
    PerItem           // 8: a constant per item id
};

AskResult AskVariant(AskInput const& in, AuctionConfig const& config, AskFault fault, std::uint32_t item = 0)
{
    double const p = SaleChance(in.typical);
    std::uint64_t floor = AskFloor(in, config, p);
    if (fault == AskFault::VendorFloor)
        floor = std::max({in.vendorPayout + 1, in.keepWorth, SellTotal(in)});
    std::uint64_t const lowest = floor == NEVER_LISTS ? SellTotal(in) : floor;
    std::uint64_t ask = EvidenceAsk(in, config);
    if (fault == AskFault::StepUp)
    {
        AskInput fresh = in;
        fresh.expiredSteps = 0;
        double lot = static_cast<double>(EvidenceAsk(fresh, config));
        for (std::uint32_t n = in.expiredSteps; n; --n)
            lot *= 1.0 + config.RelistStepPercent / 100.0;
        ask = static_cast<std::uint64_t>(lot);
    }
    bool below = false;
    if (in.cheapestUnitBuyout)
    {
        std::uint64_t const undercut = UndercutAsk(in.cheapestUnitBuyout, in.count, config.UndercutPercent);
        below = undercut < lowest;
        ask = fault == AskFault::RaiseToUndercut ? undercut : std::min(ask, undercut);
    }
    if (in.expiredSteps && ask < lowest)
        below = true;
    if (fault == AskFault::PerItem)
        ask += item % 97 + 1;
    if (fault != AskFault::NoClamp)
        ask = std::max(ask, lowest);
    return ListingTerms(in, config, p, floor, ask, below);
}

// Invariant 2, recomputed here: a listing beats the vendor at its own terms
bool ListsWorth(AskResult const& result, AskInput const& in)
{
    std::uint32_t const deposit = Deposit(in.fees, in.sellPrice, Units(in.count), result.hours * SECONDS_PER_HOUR);
    std::uint32_t const cut = Cut(in.fees, static_cast<std::uint32_t>(result.ask));
    double const vendor = static_cast<double>(in.vendorPayout);
    double const sold = static_cast<double>(result.ask) - cut;
    double const expired = vendor - deposit;
    return result.deposit == deposit && result.cut == cut && result.p * sold + (1.0 - result.p) * expired > vendor;
}

enum class BuyFault
{
    None,
    DoubleCeiling,   // 3: twice the ceiling
    UnitPriceRank,   // 5: ranked by unit price
    PositionTie,     // 9: ties kept in input order
    DifferenceOnly,  // 10: the leader's own bid counted as in hand
};

BuyChoice ConsiderVariant(OfferInput const& offer, BuyInput const& in, BuyFault fault)
{
    AuctionIndex::AuctionFacts const& line = offer.line;
    std::uint64_t const ceiling = fault == BuyFault::DoubleCeiling ? 2 * in.ceilingPerUnit : in.ceilingPerUnit;
    std::uint64_t const held = fault == BuyFault::DifferenceOnly && line.bidder == in.ownGuid ? line.bid : 0;
    BuyChoice choice;
    choice.auctionId = line.id;
    if (line.endTime < in.now)
        return choice;
    bool budget = false;
    if (line.buyout && WithinCeiling(offer, ceiling, line.buyout))
    {
        if (in.spendable + held >= line.buyout)
            return {BuyKind::Buyout, line.buyout, line.id, Reason::NoOffer};
        budget = true;
    }
    std::uint64_t const next = NextBid(line);
    bool const biddable = line.bidder != in.ownGuid && (!line.buyout || next < line.buyout) &&
                          line.endTime - in.now <= in.patienceSeconds;
    bool cap = false;
    if (biddable && WithinCeiling(offer, ceiling, next))
    {
        if (in.openBids >= in.openBidsMax)
            cap = true;
        else if (in.spendable < next)
            budget = true;
        else
            return {BuyKind::Bid, next, line.id,
                    !line.buyout ? Reason::NoOffer
                    : budget     ? Reason::Budget
                                 : Reason::OverCeiling};
    }
    choice.reason = budget                      ? Reason::Budget
                    : cap                       ? Reason::BidCap
                    : (line.buyout || biddable) ? Reason::OverCeiling
                                                : Reason::NoOffer;
    return choice;
}

std::vector<std::size_t> RankVariant(std::vector<OfferInput> const& offers, BuyFault fault)
{
    std::vector<std::size_t> order(offers.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    auto unitBefore = [&offers](std::size_t a, std::size_t b)
    {
        std::uint64_t const unitA = RankPrice(offers[a].line) * LotUnits(offers[b].line);
        std::uint64_t const unitB = RankPrice(offers[b].line) * LotUnits(offers[a].line);
        return unitA != unitB ? unitA < unitB : offers[a].line.id < offers[b].line.id;
    };
    auto keyBefore = [&offers](std::size_t a, std::size_t b)
    {
        OfferInput tieA = offers[a];
        OfferInput tieB = offers[b];
        tieA.line.id = 0;
        tieB.line.id = 0;
        return RankedBefore(tieA, tieB);
    };
    if (fault == BuyFault::UnitPriceRank)
        std::sort(order.begin(), order.end(), unitBefore);
    else if (fault == BuyFault::PositionTie)
        std::stable_sort(order.begin(), order.end(), keyBefore);
    else
        std::sort(order.begin(), order.end(),
                  [&offers](std::size_t a, std::size_t b) { return RankedBefore(offers[a], offers[b]); });
    return order;
}

BuyChoice ChooseVariant(std::vector<OfferInput> const& offers, BuyInput const& in, BuyFault fault)
{
    BuyChoice skip;
    bool seen = false;
    for (std::size_t const index : RankVariant(offers, fault))
    {
        BuyChoice const choice = ConsiderVariant(offers[index], in, fault);
        if (choice.kind != BuyKind::Skip)
            return choice;
        if (!seen || SkipWeight(choice.reason) > SkipWeight(skip.reason))
            skip = choice;
        seen = true;
    }
    return skip;
}

// A need's market and one bot's state; the poorer twin has a higher factor and less to spend
struct Market
{
    std::vector<OfferInput> offers;
    BuyInput rich;
    BuyInput poor;
    CeilingInput richCeiling;
    CeilingInput poorCeiling;
};

// way 0: none; 1: a vendor's price (copper, weighed by the factor); 2: its own getting (value units)
CeilingInput CeilingOf(double worthValue, int way, double wayAmount, float factor)
{
    double const getting = way == 0 ? -1.0 : way == 1 ? wayAmount * factor : wayAmount;
    return {worthValue, getting, factor};
}

Market RandomMarket(Dice& dice)
{
    Market market;
    std::uint32_t const need = dice.Pick(30) ? 1 : dice.Range(2, 20);
    std::uint32_t const unit = dice.Range(1, 5000);
    std::uint64_t const excess = dice.Pick(40) ? 0 : dice.Range(0, unit);
    std::int64_t const now = 1000000;
    std::uint64_t const own = 1;
    std::uint32_t const lines = dice.Range(0, static_cast<std::uint32_t>(AuctionIndex::OFFER_LINES_MAX));
    for (std::uint32_t n = 0; n < lines; ++n)
    {
        std::uint32_t const id = 100 + n * 10 + dice.Range(0, 9);
        if (n && dice.Pick(30))  // the same auction again under another id: a tie
        {
            OfferInput twin = market.offers[dice.Range(0, n - 1)];
            twin.line.id = id;
            market.offers.push_back(twin);
            continue;
        }
        OfferInput offer;
        offer.unitsNeeded = need;
        offer.excessUnitWorth = excess;
        offer.line.id = id;
        offer.line.count = dice.Pick(30) ? need : dice.Range(1, 3 * need + 5);
        double const lot = unit * dice.Real(0.3, 2.5) * offer.line.count;
        offer.line.buyout = dice.Pick(20) ? 0 : std::max<std::uint32_t>(1, static_cast<std::uint32_t>(lot));
        offer.line.startBid = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(lot * dice.Real(0.3, 1.0)));
        if (dice.Pick(60))
        {
            offer.line.bid = offer.line.startBid + dice.Range(0, offer.line.startBid);
            offer.line.bidder = dice.Pick(30) ? own : dice.Range(2, 1000);
        }
        std::int64_t const ends = dice.Pick(5)    ? -static_cast<std::int64_t>(dice.Range(1, 100))
                                  : dice.Pick(50) ? dice.Range(0, 7200)
                                                  : dice.Range(0, 172800);
        offer.line.endTime = now + ends;
        market.offers.push_back(offer);
    }
    float const factor = static_cast<float>(dice.Real(0.05, 20.0));
    float const poorFactor = factor * static_cast<float>(dice.Real(1.0, 4.0));
    double const worth = dice.Pick(10) ? 0.0 : unit * dice.Real(0.1, 3.0) * 5.0;
    int const way = static_cast<int>(dice.Range(0, 2));
    double const wayAmount = way == 1 ? unit * dice.Real(0.3, 2.0) : unit * dice.Real(0.1, 3.0) * 5.0;
    market.richCeiling = CeilingOf(worth, way, wayAmount, factor);
    market.poorCeiling = CeilingOf(worth, way, wayAmount, poorFactor);
    market.rich.ceilingPerUnit = Ceiling(market.richCeiling);
    market.rich.spendable = dice.Pick(15) ? 0 : dice.Range(0, unit * need * 3);
    market.rich.openBids = dice.Range(0, 4);
    market.rich.openBidsMax = dice.Pick(80) ? 3 : dice.Range(0, 4);
    market.rich.now = now;
    market.rich.ownGuid = own;
    market.poor = market.rich;
    market.poor.ceilingPerUnit = Ceiling(market.poorCeiling);
    market.poor.spendable =
        static_cast<std::uint64_t>(static_cast<double>(market.rich.spendable) * dice.Real(0.0, 1.0));
    return market;
}

// Invariant 3, recomputed here: the line cost at the chosen price within the ceiling for the units it covers
bool OverCeiling(BuyChoice const& choice, std::vector<OfferInput> const& offers, BuyInput const& in)
{
    if (choice.kind == BuyKind::Skip)
        return false;
    for (OfferInput const& offer : offers)
    {
        if (offer.line.id != choice.auctionId)
            continue;
        std::uint64_t const count = std::max<std::uint32_t>(offer.line.count, 1);
        std::uint64_t const covered = std::min<std::uint64_t>(count, std::max<std::uint32_t>(offer.unitsNeeded, 1));
        std::uint64_t const beyondWorth = (count - covered) * offer.excessUnitWorth;
        std::uint64_t const cost = choice.price - std::min(choice.price, beyondWorth);
        return cost > in.ceilingPerUnit * covered;
    }
    return true;
}

// Invariant 5, recomputed here: consecutive ranked offers by line cost per unit of the need covered
int RankCostFalls(std::vector<OfferInput> const& offers, std::vector<std::size_t> const& order)
{
    auto cost = [&offers](std::size_t i, std::uint64_t& covered)
    {
        OfferInput const& offer = offers[i];
        std::uint64_t const count = std::max<std::uint32_t>(offer.line.count, 1);
        covered = std::min<std::uint64_t>(count, std::max<std::uint32_t>(offer.unitsNeeded, 1));
        std::uint64_t const price = offer.line.buyout ? offer.line.buyout : NextBid(offer.line);
        return price - std::min(price, (count - covered) * offer.excessUnitWorth);
    };
    int n = 0;
    for (std::size_t i = 1; i < order.size(); ++i)
    {
        std::uint64_t coveredA = 0;
        std::uint64_t coveredB = 0;
        std::uint64_t const costA = cost(order[i - 1], coveredA);
        std::uint64_t const costB = cost(order[i], coveredB);
        if (costA * coveredB > costB * coveredA)
            ++n;
    }
    return n;
}

std::uint64_t Paid(BuyChoice const& choice) { return choice.kind == BuyKind::Skip ? 0 : choice.price; }

// The ceiling with the factor multiplied instead of divided: a control for invariant 4
std::uint64_t InvertedCeiling(CeilingInput const& in)
{
    double const value = in.gettingValue >= 0.0 ? std::min(in.worthValue, in.gettingValue) : in.worthValue;
    return static_cast<std::uint64_t>(std::floor(value * in.factor));
}

struct PriceTally
{
    int belowSell{0};
    int notWorth{0};
    int overCeiling{0};
    int poorerPays{0};
    int rankCost{0};
    int expiryRaises{0};
    int offersRaise{0};
    int perItem{0};
    int orderDepends{0};
    int overPurse{0};
};

struct PriceCoverage
{
    int asks{0};
    int listed{0};
    int markets{0};
    int buyouts{0};
    int bids{0};
    int leaderBuyouts{0};
};

void SearchAsk(Dice& dice, PriceTally& rule, PriceTally& control, PriceCoverage& seen)
{
    AuctionConfig const config = RandomConfig(dice);
    AskInput const in = RandomAsk(dice);
    AskResult const result = Ask(in, config);
    Check(Equal(result, AskVariant(in, config, AskFault::None)), "the harness's ask equals Ask");
    ++seen.asks;
    seen.listed += result.kind == AskResult::Kind::List ? 1 : 0;

    auto belowSell = [&in](AskResult const& r)
    { return r.ask < SellTotal(in) || (r.kind == AskResult::Kind::List && r.startBid < SellTotal(in)); };
    rule.belowSell += belowSell(result) ? 1 : 0;
    control.belowSell += belowSell(AskVariant(in, config, AskFault::NoClamp)) ? 1 : 0;

    rule.notWorth += result.kind == AskResult::Kind::List && !ListsWorth(result, in) ? 1 : 0;
    AskResult const vendorFloor = AskVariant(in, config, AskFault::VendorFloor);
    control.notWorth += vendorFloor.kind == AskResult::Kind::List && !ListsWorth(vendorFloor, in) ? 1 : 0;

    // 6: after an expiry a lot is never listed higher, nor listed when it was held; the index counts the bot's own
    // expiry too (the sale chance falls and the floor rises), or it lies outside the window
    auto raised = [](AskResult const& before, AskResult const& after)
    { return after.kind == AskResult::Kind::List && (before.kind != AskResult::Kind::List || after.ask > before.ask); };
    for (std::uint32_t n = 0; n < 4; ++n)
        for (bool const counted : {false, true})
        {
            AskInput now = in;
            now.expiredSteps = n;
            AskInput after = now;
            after.expiredSteps = n + 1;
            after.typical.expiries += counted ? 1 : 0;
            rule.expiryRaises += raised(Ask(now, config), Ask(after, config)) ? 1 : 0;
            control.expiryRaises +=
                raised(AskVariant(now, config, AskFault::StepUp), AskVariant(after, config, AskFault::StepUp)) ? 1 : 0;
        }

    AskInput none = in;
    none.cheapestUnitBuyout = 0;
    AskInput cheaper = in;
    cheaper.cheapestUnitBuyout = dice.Range(1, 4000);
    AskInput dearer = cheaper;
    dearer.cheapestUnitBuyout += dice.Range(1, 4000);
    auto raises = [&](AskFault fault)
    {
        std::uint64_t const without = AskVariant(none, config, fault).ask;
        std::uint64_t const low = AskVariant(cheaper, config, fault).ask;
        std::uint64_t const high = AskVariant(dearer, config, fault).ask;
        return (low > without ? 1 : 0) + (high > without ? 1 : 0) + (low > high ? 1 : 0);
    };
    rule.offersRaise += raises(AskFault::None);
    control.offersRaise += raises(AskFault::RaiseToUndercut);

    std::uint32_t const item = dice.Range(1, 60000);
    std::uint32_t const twin = item + dice.Range(1, 1000);
    rule.perItem +=
        Equal(AskVariant(in, config, AskFault::None, item), AskVariant(in, config, AskFault::None, twin)) ? 0 : 1;
    control.perItem +=
        Equal(AskVariant(in, config, AskFault::PerItem, item), AskVariant(in, config, AskFault::PerItem, twin)) ? 0 : 1;
    rule.orderDepends += Equal(Ask(in, config), result) ? 0 : 1;
}

void SearchMarket(Dice& dice, PriceTally& rule, PriceTally& control, PriceCoverage& seen)
{
    Market const market = RandomMarket(dice);
    BuyChoice const choice = ChooseBuy(market.offers, market.rich);
    Check(Equal(choice, ChooseVariant(market.offers, market.rich, BuyFault::None)),
          "the harness's choice equals ChooseBuy");
    ++seen.markets;
    seen.buyouts += choice.kind == BuyKind::Buyout ? 1 : 0;
    seen.bids += choice.kind == BuyKind::Bid ? 1 : 0;

    rule.overCeiling += OverCeiling(choice, market.offers, market.rich) ? 1 : 0;
    BuyChoice const doubled = ChooseVariant(market.offers, market.rich, BuyFault::DoubleCeiling);
    control.overCeiling += OverCeiling(doubled, market.offers, market.rich) ? 1 : 0;

    // 4: a poorer bot's ceiling is never higher, and it never pays more for the same auction
    rule.poorerPays += Ceiling(market.poorCeiling) > Ceiling(market.richCeiling) ? 1 : 0;
    BuyInput invertedRich = market.rich;
    BuyInput invertedPoor = market.poor;
    invertedRich.ceilingPerUnit = InvertedCeiling(market.richCeiling);
    invertedPoor.ceilingPerUnit = InvertedCeiling(market.poorCeiling);
    for (OfferInput const& offer : market.offers)
    {
        rule.poorerPays += Paid(ChooseBuy({offer}, market.poor)) > Paid(ChooseBuy({offer}, market.rich)) ? 1 : 0;
        control.poorerPays += Paid(ChooseBuy({offer}, invertedPoor)) > Paid(ChooseBuy({offer}, invertedRich)) ? 1 : 0;
    }

    rule.rankCost += RankCostFalls(market.offers, Rank(market.offers));
    control.rankCost += RankCostFalls(market.offers, RankVariant(market.offers, BuyFault::UnitPriceRank));

    std::vector<OfferInput> shuffled = market.offers;
    Shuffle(shuffled, dice.rng);
    rule.orderDepends += Equal(choice, ChooseBuy(shuffled, market.rich)) ? 0 : 1;
    rule.orderDepends += Equal(choice, ChooseBuy(market.offers, market.rich)) ? 0 : 1;
    control.orderDepends += Equal(ChooseVariant(market.offers, market.rich, BuyFault::PositionTie),
                                  ChooseVariant(shuffled, market.rich, BuyFault::PositionTie))
                                ? 0
                                : 1;

    for (BuyInput const& buyer : {market.rich, market.poor})
    {
        BuyChoice const taken = ChooseBuy(market.offers, buyer);
        rule.overPurse += Paid(taken) > buyer.spendable ? 1 : 0;
        control.overPurse += Paid(ChooseVariant(market.offers, buyer, BuyFault::DifferenceOnly)) > buyer.spendable;
        for (OfferInput const& offer : market.offers)
            seen.leaderBuyouts +=
                taken.kind == BuyKind::Buyout && offer.line.id == taken.auctionId && offer.line.bidder == buyer.ownGuid
                    ? 1
                    : 0;
    }
}

void SearchPricing()
{
    PriceTally rule;
    PriceTally control;
    PriceCoverage seen;
    for (std::uint32_t const seed : SEEDS)
    {
        std::mt19937 rng(seed + 2);
        Dice dice{rng};
        for (int w = 0; w < WORLDS_PER_SEED; ++w)
        {
            for (int a = 0; a < 6; ++a)
                SearchAsk(dice, rule, control, seen);
            for (int m = 0; m < 4; ++m)
                SearchMarket(dice, rule, control, seen);
        }
    }
    std::cout << "pricing search: " << seen.asks << " asks (" << seen.listed << " listed), " << seen.markets
              << " markets (" << seen.buyouts << " buyouts, " << seen.bids << " bids, " << seen.leaderBuyouts
              << " leader buyouts); counterexamples (rule / control): sell " << rule.belowSell << "/"
              << control.belowSell << ", worth " << rule.notWorth << "/" << control.notWorth << ", ceiling "
              << rule.overCeiling << "/" << control.overCeiling << ", poorer " << rule.poorerPays << "/"
              << control.poorerPays << ", rank " << rule.rankCost << "/" << control.rankCost << ", expiry "
              << rule.expiryRaises << "/" << control.expiryRaises << ", offers " << rule.offersRaise << "/"
              << control.offersRaise << ", item " << rule.perItem << "/" << control.perItem << ", order "
              << rule.orderDepends << "/" << control.orderDepends << ", purse " << rule.overPurse << "/"
              << control.overPurse << '\n';
    Check(rule.belowSell == 0, "P1. no ask below the vendor sell total");
    Check(control.belowSell > 0, "P1. control: without the floor clamp an ask falls below it (caught)");
    Check(rule.notWorth == 0, "P2. every listing beats the vendor at its terms");
    Check(control.notWorth > 0, "P2. control: listing whenever the ask beats the vendor alone (caught)");
    Check(rule.overCeiling == 0, "P3. no purchase above the ceiling");
    Check(control.overCeiling > 0, "P3. control: twice the ceiling (caught)");
    Check(rule.poorerPays == 0, "P4. a poorer bot never pays more for the same auction");
    Check(control.poorerPays > 0, "P4. control: an inverted factor (caught)");
    Check(rule.rankCost == 0, "P5. the offer that costs the need more never ranks first");
    Check(control.rankCost > 0, "P5. control: ranked by unit price (caught)");
    Check(rule.expiryRaises == 0, "P6. an expiry never raises the next ask");
    Check(control.expiryRaises > 0, "P6. control: a step upward (caught)");
    Check(rule.offersRaise == 0, "P7. more or cheaper offers never raise an ask");
    Check(control.offersRaise > 0, "P7. control: the ask follows the offer up (caught)");
    Check(rule.perItem == 0, "P8. items equal but for their id get the same ask");
    Check(control.perItem > 0, "P8. control: a constant per item (caught)");
    Check(rule.orderDepends == 0, "P9. the same input, in any order, gives the same answer");
    Check(control.orderDepends > 0, "P9. control: ties kept in input order (caught)");
    Check(rule.overPurse == 0, "P10. the full price in hand, also for the leader");
    Check(control.overPurse > 0, "P10. control: the leader's own bid counted as in hand (caught)");
}

// ---- what a bot buys (WA4a): the index's short list, the buy list and a buying visit ----
using AuctionIndex::AuctionFacts;
std::string ChoiceText(BuyChoice const& choice, std::vector<OfferInput> const& offers);
constexpr std::uint64_t BUYER_GUID = 9;
constexpr std::int64_t MARKET_NOW = 1000000;
constexpr std::uint8_t BUYER_HOUSE = 7;

// One item's lots: varied lot sizes, often crowded by 20-stacks cheaper per unit than the rest
std::vector<AuctionFacts> RandomLots(Dice& dice, std::uint32_t item, std::uint32_t& nextId, std::uint32_t unit)
{
    constexpr std::uint32_t SIZES[] = {1, 2, 4, 5, 10, 12, 20};
    std::vector<AuctionFacts> lots;
    bool const crowded = dice.Pick(40);
    std::uint32_t const n = crowded ? dice.Range(6, 12) : dice.Range(0, 10);
    for (std::uint32_t i = 0; i < n; ++i)
    {
        AuctionFacts lot;
        lot.id = nextId++;
        lot.item = item;
        bool const big = crowded && i < 6;
        lot.count = big ? 20 : dice.Pick(70) ? SIZES[dice.Range(0, 6)] : dice.Range(1, 40);
        double const perUnit = unit * (big ? dice.Real(0.4, 0.8) : dice.Real(0.5, 2.0));
        double const price = std::max(1.0, perUnit * lot.count);
        lot.buyout = dice.Pick(15) ? 0 : static_cast<std::uint32_t>(price);
        lot.startBid = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(price * dice.Real(0.3, 1.0)));
        if (dice.Pick(40))
        {
            lot.bid = lot.startBid + dice.Range(0, lot.startBid / 2);
            if (lot.buyout && lot.bid >= lot.buyout)
                lot.bid = lot.startBid;
            lot.bidder = dice.Pick(25) ? BUYER_GUID : dice.Range(10, 1000);
        }
        lot.endTime = MARKET_NOW + (dice.Pick(50) ? dice.Range(60, 7200) : dice.Range(60, 172800));
        lot.houseId = dice.Pick(90) ? BUYER_HOUSE : 2;
        lots.push_back(lot);
    }
    return lots;
}

AuctionIndex::Snapshot Publish(std::vector<AuctionFacts> const& lots)
{
    AuctionIndex::Book book;
    for (AuctionFacts const& lot : lots)
        book.Add(lot);
    AuctionIndex::Snapshot snapshot;
    book.Fill(snapshot, MARKET_NOW, 7 * AuctionIndex::SECONDS_PER_DAY);
    return snapshot;
}

// The least line cost per unit of the need covered among the buyouts a bot takes (within the ceiling, in hand);
// covered 0: none
struct LineBest
{
    std::uint64_t cost{0};
    std::uint64_t covered{0};
};

LineBest BestBuyout(std::vector<AuctionFacts> const& lots, std::uint32_t need, std::uint64_t excess,
                    std::uint64_t ceiling, std::uint64_t spendable)
{
    LineBest best;
    for (AuctionFacts const& lot : lots)
    {
        OfferInput const offer{lot, need, excess};
        if (!lot.buyout || lot.buyout > spendable || !WithinCeiling(offer, ceiling, lot.buyout))
            continue;
        LineBest const here{LineCost(offer, lot.buyout), Covered(offer)};
        if (!best.covered || here.cost * best.covered < best.cost * here.covered)
            best = here;
    }
    return best;
}

bool SameBest(LineBest const& a, LineBest const& b)
{
    return !a.covered || !b.covered ? a.covered == b.covered : a.cost * b.covered == b.cost * a.covered;
}

// The controls: the old short list (the cheapest OFFER_LINES_MAX by unit), and that list with the lot of the least
// total buyout added
std::vector<AuctionFacts> UnitFive(std::vector<AuctionFacts> lots, bool withLeastTotal)
{
    auto key = [](AuctionFacts const& lot)
    {
        return lot.buyout ? std::make_tuple(false, AuctionIndex::UnitPrice(lot.buyout, lot.count), lot.id)
                          : std::make_tuple(true, AuctionIndex::UnitPrice(lot.bid ? lot.bid : lot.startBid, lot.count),
                                            lot.id);
    };
    std::sort(lots.begin(), lots.end(),
              [&key](AuctionFacts const& a, AuctionFacts const& b) { return key(a) < key(b); });
    std::size_t const keep = std::min(lots.size(), AuctionIndex::OFFER_LINES_MAX);
    std::vector<AuctionFacts> list(lots.begin(), lots.begin() + static_cast<std::ptrdiff_t>(keep));
    AuctionFacts const* least = nullptr;
    for (AuctionFacts const& lot : lots)
        if (lot.buyout && (!least || lot.buyout < least->buyout))
            least = &lot;
    if (withLeastTotal && least &&
        std::none_of(list.begin(), list.end(), [least](AuctionFacts const& lot) { return lot.id == least->id; }))
        list.push_back(*least);
    return list;
}

struct WidthTally
{
    int books{0};
    int needs{0};
    int wider{0};
    std::size_t longest{0};
    int rule{0};
    int unitFive{0};
    int leastTotal{0};
};

// The short list holds, for every need size, excess worth, ceiling and purse, a buyout as good as the whole book's
void SearchWidth(Dice& dice, WidthTally& tally)
{
    std::uint32_t nextId = 1;
    std::uint32_t const unit = dice.Range(1, 5000);
    std::vector<AuctionFacts> const lots = RandomLots(dice, 500, nextId, unit);
    AuctionIndex::Snapshot const snapshot = Publish(lots);
    AuctionIndex::ItemEntry const* entry = snapshot.Find(500);
    std::vector<AuctionFacts> const listed = entry ? entry->offers.cheapest : std::vector<AuctionFacts>{};
    std::vector<AuctionFacts> const five = UnitFive(lots, false);
    std::vector<AuctionFacts> const total = UnitFive(lots, true);
    ++tally.books;
    tally.wider += listed.size() > AuctionIndex::OFFER_LINES_MAX ? 1 : 0;
    tally.longest = std::max(tally.longest, listed.size());
    for (std::uint32_t const need : {1u, 4u, dice.Range(1, 40)})
    {
        std::uint64_t const excess = dice.Pick(50) ? 0 : dice.Range(0, unit);
        std::uint64_t const ceiling = dice.Pick(50) ? MAX_LOT_PRICE : dice.Range(1, 3 * unit);
        std::uint64_t const spendable = dice.Pick(50) ? MAX_LOT_PRICE : dice.Range(0, 30 * unit);
        LineBest const book = BestBuyout(lots, need, excess, ceiling, spendable);
        ++tally.needs;
        tally.rule += SameBest(BestBuyout(listed, need, excess, ceiling, spendable), book) ? 0 : 1;
        tally.unitFive += SameBest(BestBuyout(five, need, excess, ceiling, spendable), book) ? 0 : 1;
        tally.leastTotal += SameBest(BestBuyout(total, need, excess, ceiling, spendable), book) ? 0 : 1;
    }
}

// The buy list's pieces by hand, with the decisions printed
void CheckBuyListHand()
{
    std::cout << "buy list by hand:\n";
    std::vector<BuyLine> const lines =
        BuyList({LineOf(LineOwner::Gear, 801, 1, 90.0, 60.0, 0.5f, 0),
                 LineOf(LineOwner::Path, 802, 3, 600.0, -1.0, 3.0f, 0), BagLine(803, 5000, 0),
                 UseLine(804, 4, 120, 100, 5), UseLine(805, 0, 120, 100, 5), UseLine(806, 4, 0, 100, 5)});
    for (BuyLine const& line : lines)
        std::cout << "  " << OwnerWord(line.owner) << " " << line.item << " x" << line.units << " ceiling "
                  << CeilingOf(line) << '\n';
    bool const ordered = lines.size() == 4 && lines[0].item == 804 && lines[1].item == 803 && lines[2].item == 802 &&
                         lines[3].item == 801;
    Check(ordered, "the buy list: use, bag, path, gear; a line without units or worth is dropped");
    Check(ordered && CeilingOf(lines[0]) == 100 && CeilingOf(lines[1]) == 5000 && CeilingOf(lines[2]) == 200 &&
              CeilingOf(lines[3]) == 120,
          "ceilings: use at the vendor's price under its worth, a bag at its worth, the path and gear stubs in value "
          "units over the factor");
    Check(CeilingOf(UseLine(807, 1, 80, 100, 0)) == 80 && CeilingOf(UseLine(808, 1, 120, 0, 0)) == 120,
          "a use line's ceiling is its worth under a dearer vendor or with none");

    BuyLine const use = lines.empty() ? BuyLine{} : lines[0];
    Check(TypicalCopper(use, 60) == 240 && WantedWorth(use, 60, 1000) == 160 && WantedWorth(use, 60, 130) == 80,
          "typical 60 under the ceiling 100: claims 240 for 4, worth 40 each, as many as the gold buys");
    Check(TypicalCopper(use, 100) == 0 && WantedWorth(use, 100, 1000) == 0 && TypicalCopper(use, 0) == 0 &&
              WantedWorth(use, 0, 1000) == 0,
          "typical at the ceiling or unknown: no claim, no worth");

    // The bot leads auction 1 (item 700, bid 300: the core took it from the purse) and auction 2 in another house
    std::vector<AuctionFacts> lots;
    auto lot = [&lots](std::uint32_t id, std::uint32_t item, std::uint32_t count, std::uint32_t bid,
                       std::uint32_t buyout, std::uint64_t bidder, std::uint8_t house)
    {
        AuctionFacts facts;
        facts.id = id;
        facts.item = item;
        facts.count = count;
        facts.startBid = bid ? bid : 1;
        facts.bid = bid;
        facts.buyout = buyout;
        facts.bidder = bidder;
        facts.endTime = MARKET_NOW + 3600;
        facts.houseId = house;
        lots.push_back(facts);
    };
    lot(1, 700, 1, 300, 900, BUYER_GUID, BUYER_HOUSE);
    lot(2, 701, 1, 50, 0, BUYER_GUID, 2);
    lot(3, 804, 4, 0, 380, 0, BUYER_HOUSE);  // the bot's own listing
    lot(4, 804, 4, 0, 390, 0, 2);            // another house
    lot(5, 804, 4, 0, 400, 0, BUYER_HOUSE);
    lot(6, 804, 4, 200, 600, BUYER_GUID, BUYER_HOUSE);
    AuctionIndex::Snapshot const snapshot = Publish(lots);
    AuctionIndex::ItemEntry const* entry = snapshot.Find(804);
    std::vector<std::uint32_t> const own = {3};
    std::vector<OfferInput> const offers =
        entry ? OffersFor(entry->offers, use, BUYER_HOUSE, own) : std::vector<OfferInput>{};
    Check(OpenBids(snapshot, BUYER_GUID) == 3, "open bids: the index's lines the bot leads, in any house");
    Check(offers.size() == 2 && offers[0].line.id == 5 && offers[1].line.id == 6 && offers[0].unitsNeeded == 4 &&
              offers[0].excessUnitWorth == 5,
          "the line's offers: this house only, without the bot's own auction");

    BuyInput buyer;
    buyer.spendable = Spendable(1500, 1000, 0);
    buyer.openBids = OpenBids(snapshot, BUYER_GUID);
    buyer.now = MARKET_NOW;
    buyer.ownGuid = BUYER_GUID;
    BuyChoice const bought = entry ? ChooseLine(entry->offers, use, BUYER_HOUSE, own, buyer) : BuyChoice{};
    std::cout << "  purse 1500 (the open bids already out of it), reserve 1000: " << ChoiceText(bought, offers) << '\n';
    Check(buyer.spendable == 500 && bought.kind == BuyKind::Buyout && bought.auctionId == 5 && bought.price == 400,
          "the open bids are not taken off the purse twice: 500 above the reserve buys auction 5 out for 400");
    BuyInput twice = buyer;
    twice.spendable = Spendable(1500, 1000, 550);
    Check(entry && ChooseLine(entry->offers, use, BUYER_HOUSE, own, twice).kind == BuyKind::Skip,
          "control: the open bids' 550 taken off again leaves nothing for it (caught)");
    Check(Spendable(1200, 1000, 300) == 0 && Spendable(900, 1000, 0) == 0,
          "the use items' held copper and a purse below the reserve leave nothing");

    // Auction 6: the bot leads with 200, buyout 600; the core takes only 400 but checks the full 600 in hand
    BuyLine dear = use;
    dear.ceiling = {200.0, -1.0, 1.0f};
    std::vector<OfferInput> const led = {{lots[5], 4, 0}};
    BuyInput short5 = buyer;
    short5.spendable = 500;
    short5.ceilingPerUnit = CeilingOf(dear);
    BuyInput full = short5;
    full.spendable = 600;
    BuyChoice const no = ChooseBuy(led, short5);
    BuyChoice const yes = ChooseBuy(led, full);
    Check(no.kind == BuyKind::Skip && no.reason == AuctionWords::Reason::Budget && yes.kind == BuyKind::Buyout &&
              yes.price == 600,
          "the leader's buyout needs its whole 600 above the reserve, not the 400 the core takes");
}

// A buying visit's world: the buy list, the house's lots of its items (some the bot's own, some it leads), lots of
// another item it leads, and its purse
struct VisitWorld
{
    AuctionIndex::Snapshot snapshot;
    std::vector<BuyLine> lines;
    std::vector<std::uint32_t> own;
    std::uint64_t money{0};
    std::uint64_t reserve{0};
    std::uint64_t held{0};
    std::uint32_t openBidsMax{3};
};

VisitWorld RandomVisit(Dice& dice)
{
    VisitWorld world;
    std::uint32_t nextId = 1;
    std::vector<AuctionFacts> all;
    std::vector<BuyLine> lines;
    std::uint32_t const n = dice.Range(1, 4);
    for (std::uint32_t i = 0; i < n; ++i)
    {
        std::uint32_t const item = 600 + i;
        std::uint32_t const unit = dice.Range(1, 5000);
        std::uint32_t const units = dice.Pick(30) ? 1 : dice.Range(2, 20);
        auto copper = [&dice, unit](double low, double high)
        { return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(unit * dice.Real(low, high))); };
        std::uint32_t const owner = dice.Range(0, 3);
        std::uint32_t const payout = dice.Pick(50) ? 0 : dice.Range(0, unit / 2);
        if (owner == 0)
            lines.push_back(UseLine(item, units, copper(0.3, 3.0), dice.Pick(50) ? copper(0.5, 2.0) : 0, payout));
        else if (owner == 1)
            lines.push_back(BagLine(item, copper(0.3, 3.0), dice.Pick(50) ? copper(0.5, 2.0) : 0));
        else
        {
            float const factor = static_cast<float>(dice.Real(0.05, 20.0));
            double const getting = dice.Pick(50) ? copper(0.5, 2.0) * static_cast<double>(factor) : -1.0;
            lines.push_back(LineOf(static_cast<LineOwner>(owner), item, units,
                                   copper(0.3, 3.0) * static_cast<double>(factor), getting, factor, payout));
        }
        for (AuctionFacts lot : RandomLots(dice, item, nextId, unit))
        {
            if (dice.Pick(15))
            {
                world.own.push_back(lot.id);
                if (lot.bidder == BUYER_GUID)
                    lot.bidder = 11;
            }
            all.push_back(lot);
        }
    }
    for (AuctionFacts const& lot : RandomLots(dice, 700, nextId, 100))
        all.push_back(lot);
    world.snapshot = Publish(all);
    world.lines = BuyList(lines);
    world.money = dice.Range(0, 200000);
    world.reserve = dice.Pick(20) ? 0 : dice.Range(0, 150000);
    world.held = dice.Pick(50) ? 0 : dice.Range(0, 20000);
    world.openBidsMax = dice.Pick(80) ? 3 : dice.Range(0, 4);
    return world;
}

enum class VisitFault
{
    None,
    NoReserve,     // B2: the spendable gold counts the reserve
    BidsInHand,    // B3: the copper in the bot's open bids counted as in hand
    BidsFromZero,  // B4: open bids counted from the visit, not the index
    OwnOffers      // B5: the bot's own auctions left among the offers
};

struct VisitTally
{
    int belowReserve{0};
    int overPurse{0};
    int overCap{0};
    int ownAuction{0};
};

struct VisitSeen
{
    int visits{0};
    int lines{0};
    int buyouts{0};
    int bids{0};
    int leaderBuyouts{0};
    int capped{0};
};

// The visit as WA4b runs it: each line in order, the purse read again after each purchase (the core takes the copper)
void PlayVisit(VisitWorld const& world, VisitFault fault, VisitTally& tally, VisitSeen* seen)
{
    std::uint64_t money = world.money;
    std::uint32_t open = OpenBids(world.snapshot, BUYER_GUID);
    std::uint32_t counted = fault == VisitFault::BidsFromZero ? 0 : open;
    std::uint64_t inBids = 0;
    for (auto const& [item, entry] : world.snapshot.items)
        for (AuctionFacts const& line : entry.offers.cheapest)
            inBids += line.bidder == BUYER_GUID ? line.bid : 0;
    std::vector<std::uint32_t> const none;
    for (BuyLine const& line : world.lines)
    {
        AuctionIndex::ItemEntry const* entry = world.snapshot.Find(line.item);
        if (!entry)
            continue;
        std::uint64_t const spendable = Spendable(money, world.reserve, world.held);
        BuyInput buyer;
        buyer.spendable = fault == VisitFault::NoReserve    ? Spendable(money, 0, world.held)
                          : fault == VisitFault::BidsInHand ? spendable + inBids
                                                            : spendable;
        buyer.openBids = counted;
        buyer.openBidsMax = world.openBidsMax;
        buyer.now = MARKET_NOW;
        buyer.ownGuid = BUYER_GUID;
        BuyChoice const choice =
            ChooseLine(entry->offers, line, BUYER_HOUSE, fault == VisitFault::OwnOffers ? none : world.own, buyer);
        if (seen)
        {
            ++seen->lines;
            seen->capped += choice.kind == BuyKind::Skip && choice.reason == AuctionWords::Reason::BidCap ? 1 : 0;
        }
        if (choice.kind == BuyKind::Skip)
            continue;
        AuctionFacts const* lot = nullptr;
        for (AuctionFacts const& each : entry->offers.cheapest)
            lot = each.id == choice.auctionId ? &each : lot;
        if (!lot)
            continue;
        tally.overPurse += choice.price > spendable ? 1 : 0;
        tally.ownAuction += std::find(world.own.begin(), world.own.end(), choice.auctionId) != world.own.end() ? 1 : 0;
        tally.overCap += choice.kind == BuyKind::Bid && open >= world.openBidsMax ? 1 : 0;
        bool const leads = lot->bidder == BUYER_GUID;
        std::uint64_t taken = choice.price;
        if (choice.kind == BuyKind::Buyout && leads)
        {
            taken -= lot->bid;  // the core takes the difference from the leader
            --open;
            counted -= counted ? 1 : 0;
            inBids -= lot->bid;
        }
        else if (choice.kind == BuyKind::Bid)
        {
            ++open;
            ++counted;
            inBids += choice.price;
        }
        money = money > taken ? money - taken : 0;
        tally.belowReserve += money < world.reserve + world.held ? 1 : 0;
        if (seen)
        {
            seen->buyouts += choice.kind == BuyKind::Buyout ? 1 : 0;
            seen->bids += choice.kind == BuyKind::Bid ? 1 : 0;
            seen->leaderBuyouts += choice.kind == BuyKind::Buyout && leads ? 1 : 0;
        }
    }
}

void SearchBuying()
{
    CheckBuyListHand();
    WidthTally width;
    VisitTally rule;
    VisitSeen seen;
    std::array<VisitTally, 5> control{};
    for (std::uint32_t const seed : SEEDS)
    {
        std::mt19937 rng(seed + 3);
        Dice dice{rng};
        for (int w = 0; w < WORLDS_PER_SEED; ++w)
        {
            SearchWidth(dice, width);
            VisitWorld const world = RandomVisit(dice);
            ++seen.visits;
            PlayVisit(world, VisitFault::None, rule, &seen);
            for (VisitFault const fault :
                 {VisitFault::NoReserve, VisitFault::BidsInHand, VisitFault::BidsFromZero, VisitFault::OwnOffers})
                PlayVisit(world, fault, control[static_cast<std::size_t>(fault)], nullptr);
        }
    }
    std::cout << "visit search: " << seen.visits << " visits, " << seen.lines << " lines (" << seen.buyouts
              << " buyouts, " << seen.bids << " bids, " << seen.leaderBuyouts << " leader buyouts, " << seen.capped
              << " skipped at the cap); counterexamples (rule / control): reserve " << rule.belowReserve << "/"
              << control[1].belowReserve << ", purse " << rule.overPurse << "/" << control[2].overPurse << ", cap "
              << rule.overCap << "/" << control[3].overCap << ", own " << rule.ownAuction << "/"
              << control[4].ownAuction << '\n';
    Check(seen.buyouts > 0 && seen.bids > 0 && seen.leaderBuyouts > 0 && seen.capped > 0,
          "the visits reach buyouts, bids, leader buyouts and the bid cap");
    Check(rule.belowReserve == 0, "B2. a visit never leaves the purse below the reserve and the use items' share");
    Check(control[1].belowReserve > 0, "B2. control: spendable gold that counts the reserve (caught)");
    Check(rule.overPurse == 0, "B3. never a bid or buyout whose full price exceeds the gold above the reserve");
    Check(control[2].overPurse > 0, "B3. control: the copper in open bids counted as in hand (caught)");
    Check(rule.overCap == 0, "B4. never more open bids than the cap, the index's own bids counted");
    Check(control[3].overCap > 0, "B4. control: open bids counted from the visit only (caught)");
    Check(rule.ownAuction == 0, "B5. never a bid or buyout on the bot's own auction");
    Check(control[4].ownAuction > 0, "B5. control: its own auctions left among the offers (caught)");
    std::cout << "short list search: " << width.books << " books, " << width.needs << " needs, " << width.wider
              << " lists past " << AuctionIndex::OFFER_LINES_MAX << " lines (longest " << width.longest
              << "); counterexamples (rule / cheapest five by unit / plus the least total): " << width.rule << "/"
              << width.unitFive << "/" << width.leastTotal << '\n';
    Check(width.rule == 0, "B1. the short list holds the whole book's best buyout for every line");
    Check(width.unitFive > 0, "B1. control: the cheapest five by unit miss a smaller lot (caught)");
    Check(width.leastTotal > 0, "B1. control: five by unit plus the least total still miss one (caught)");
}

// ---- the printed decisions ----
std::string Fixed(double value, int digits)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
}

std::string ChoiceText(BuyChoice const& choice, std::vector<OfferInput> const& offers)
{
    if (choice.kind == BuyKind::Skip)
        return "skip";
    char letter = '?';
    for (std::size_t i = 0; i < offers.size(); ++i)
        if (offers[i].line.id == choice.auctionId)
            letter = static_cast<char>('A' + i);
    return std::string(choice.kind == BuyKind::Buyout ? "buyout " : "bid ") + letter + " " + Money(choice.price);
}

struct SampleItem
{
    std::uint32_t id;
    char const* name;
    std::uint32_t count;
    std::uint32_t sellPrice;  // planted when the fixture lacks the item
    std::uint64_t reference;
};

void PrintAsks(std::vector<SampleItem> const& items, AuctionConfig const& config)
{
    std::cout << "## Asks\n\nSituations: *reference* (no trades, not at the auctioneer) and *market* (typical price "
                 "1.2 x reference from 5 sales and 1 expiry, cheapest buyout seen 1.1 x reference).\n\n";
    std::cout << "| item | count | house | situation | V | reference/u | typical/u | cheapest/u | p | floor | ask | "
                 "start bid | hours | deposit | cut | net if sold | expected | decision | reason |\n";
    std::cout << "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (SampleItem const& item : items)
        for (int house = 0; house < 2; ++house)
            for (int market = 0; market < 2; ++market)
            {
                AskInput in = Lot(item.count, item.sellPrice, house ? GOBLIN_FEES : FACTION_FEES);
                in.reference = item.reference;
                if (market)
                {
                    in.typical.unitPrice =
                        static_cast<std::uint32_t>(std::max<std::uint64_t>(item.reference * 6 / 5, 1));
                    in.typical.sample = 5;
                    in.typical.expiries = 1;
                    in.cheapestUnitBuyout =
                        static_cast<std::uint32_t>(std::max<std::uint64_t>(item.reference * 11 / 10, 1));
                }
                AskResult const r = Ask(in, config);
                std::cout << "| " << item.name << " | " << item.count << " | "
                          << (house ? "goblin 25/15" : "faction 5/5") << " | " << (market ? "market" : "reference")
                          << " | " << in.vendorPayout << " | " << item.reference << " | "
                          << (market ? std::to_string(in.typical.unitPrice) : "-") << " | "
                          << (market ? std::to_string(in.cheapestUnitBuyout) : "-") << " | " << Fixed(r.p, 2) << " | "
                          << (r.floor == NEVER_LISTS ? "never" : Money(r.floor)) << " | " << Money(r.ask) << " | "
                          << Money(r.startBid) << " | " << r.hours << " | " << r.deposit << " | " << r.cut << " | "
                          << Money(r.ask - r.cut) << " | " << Fixed(r.expected, 1) << " | "
                          << (r.kind == AskResult::Kind::List ? "post" : "hold") << " | "
                          << AuctionWords::ReasonWord(r.reason) << " |\n";
            }
}

void PrintPays(std::vector<SampleItem> const& items, Table const& table)
{
    std::cout << "\n## Pays\n\nBots: neutral traits, reserve = level^2 x 15 copper (the goal model harness's "
                 "ReserveAt), money 1.2 x the reserve (poor) or 4 x (rich); spendable = money above the reserve "
                 "(below it a bot spends nothing). Worth planted at 1.5 x the reference value in neutral value "
                 "units; getting: a vendor's price x factor for what a vendor always stocks, else the reference value "
                 "in neutral value units (a typical bot's own getting cost). Offers for a need of the sample count: "
                 "A a lot of the need at 1.1 x reference; B three times the lot at 0.95 x reference per unit (the "
                 "excess worth its vendor payout); C a bid-only lot of the need, start bid 0.9 x reference per unit, "
                 "ending in 1 h.\n\n";
    std::cout << "| bot | spendable | item | worth value/u | getting value/u | factor | ceiling/u | offers | choice | "
                 "why |\n";
    std::cout << "|---|---|---|---|---|---|---|---|---|---|\n";
    for (std::uint32_t const level : {15u, 30u, 45u})
        for (int rich = 0; rich < 2; ++rich)
        {
            std::uint64_t const reserve = static_cast<std::uint64_t>(level) * level * 15;
            Planner::Snapshot bot;
            bot.level = static_cast<std::uint8_t>(level);
            bot.money = static_cast<std::uint32_t>(rich ? reserve * 4 : reserve * 6 / 5);
            Planner::Goals::Need const need = Planner::Goals::NeedFor(bot, Planner::Goals::Weights{}, reserve);
            std::uint64_t const spendable = bot.money > reserve ? bot.money - reserve : 0;
            for (SampleItem const& item : items)
            {
                ItemInput const* input = table.Input(item.id);
                double const vendor = input ? VendorPrice(*input) : 0.0;
                double const reference = static_cast<double>(item.reference);
                double const worth = 1.5 * reference * need.exchange;
                double const getting = vendor > 0.0 ? vendor * need.factor : reference * need.exchange;
                std::uint64_t const ceiling = Ceiling({worth, getting, need.factor});
                std::uint32_t const count = item.count;
                std::uint32_t const unit = static_cast<std::uint32_t>(item.reference);
                OfferInput const a =
                    Offer(1, count, std::max(1u, unit * count * 11 / 10), count, item.sellPrice, SECONDS_24H);
                OfferInput const b =
                    Offer(2, 3 * count, std::max(1u, unit * 3 * count * 95 / 100), count, item.sellPrice, SECONDS_24H);
                OfferInput c = Offer(3, count, 0, count, item.sellPrice, 3600);
                c.line.startBid = std::max(1u, unit * count * 9 / 10);
                std::vector<OfferInput> const offers = {a, b, c};
                BuyChoice const choice = ChooseBuy(offers, Wallet(ceiling, spendable));
                std::string const why = choice.kind == BuyKind::Buyout ? "in reach"
                                        : choice.kind == BuyKind::Bid && choice.reason == Reason::NoOffer
                                            ? "no buyout"
                                            : std::string(AuctionWords::ReasonWord(choice.reason));
                std::cout << "| L" << level << (rich ? " rich" : " poor") << " | " << Money(spendable) << " | "
                          << item.name << " | " << Fixed(worth, 0) << " | " << Fixed(getting, 0) << " | "
                          << Fixed(need.factor, 2) << " | " << Money(ceiling) << " | A " << count << " @ "
                          << Money(a.line.buyout) << ", B " << 3 * count << " @ " << Money(b.line.buyout) << ", C bid "
                          << Money(c.line.startBid) << " | " << ChoiceText(choice, offers) << " | " << why << " |\n";
            }
        }
}

void Decisions(Table const& table)
{
    AuctionConfig const config;
    std::vector<SampleItem> items = {{2589, "Linen Cloth", 20, 13, 0},       {2770, "Copper Ore", 10, 5, 0},
                                     {2840, "Copper Bar", 10, 10, 0},        {929, "Healing Potion", 5, 75, 0},
                                     {4496, "Small Brown Pouch", 1, 125, 0}, {2234, "Nightwalker Armor", 1, 1806, 0}};
    std::string planted;
    for (SampleItem& item : items)
    {
        ItemReference const* row = table.Find(item.id);
        ItemInput const* input = table.Input(item.id);
        if (row && input)
        {
            item.reference = row->copper;
            item.sellPrice = input->sellPrice;
            continue;
        }
        item.reference = 4ull * item.sellPrice;
        planted += std::string(planted.empty() ? "" : ", ") + item.name;
    }
    std::cout << "\n<!-- decisions -->\n# Asking and paying: the printed decisions (auction house WA2b)\n\n";
    std::cout << "- Printed by tests/auction-rules-selftest.cpp from AuctionPricing.h; reference values from the "
                 "fixture table above"
              << (planted.empty() ? "" : " (planted at 4 x the sell price: " + planted + ")") << ".\n";
    std::cout << "- Fees: the faction houses' row (Alliance 2, Horde 6) deposit 5 % cut 5 % at rates 1 (our "
                 "configs); the goblin row (7) as the client data has it, deposit 25 % cut 15 %, for contrast. With "
                 "AllowTwoSide.Interaction.Auction every auctioneer uses row 7, which WA1a's world SQL makes 5/5 "
                 "once applied (WA3b). The core's formulas in binary32, the 100-copper minimum deposit.\n";
    std::cout << "- [E] Sale chance p = (sales + 1) / (sales + expiries + 2) in the history window: 1/2 with no "
                 "history. Undercut "
              << config.UndercutPercent << " % under the cheapest buyout seen (only at the auctioneer); an expiry "
              << "lowers the next ask " << config.RelistStepPercent << " %, a quick sale raises it as much; relist "
              << "limit " << config.RelistLimit << "; durations " << config.FirstDurationHours << " h first, "
              << config.RelistDurationHours << " h after an expiry (the shorter when the ask does not cover the "
              << "longer one's deposit); start bid " << START_BID_PERCENT << " % of the buyout, never below the "
              << "floor.\n";
    std::cout << "- Floor: the highest of the lowest ask worth listing (p x (ask - cut) + (1 - p) x (V - deposit) > V "
                 "at the shorter duration), the keep worth and the vendor sell total. V here is the template sell "
                 "total (unworn). The ask is never below the floor: a typical price or reference value below it "
                 "lists at the floor; an undercut offer below it makes the bot hold (keep, bank or vendor: the "
                 "caller's choice).\n";
    std::cout << "- [E] Ceiling per unit: min(worth, getting) / need factor, in copper. Within it: the line cost (the "
                 "price less the vendor payout of the units beyond the need) at most the ceiling x the units of the "
                 "need the lot covers. Offers ranked by line cost per unit covered, then unit price, then id. Bids "
                 "only on auctions ending within "
              << BID_PATIENCE_SECONDS / 3600 << " h, at most " << config.OpenBidsMax << " open bids; a bot never "
              << "raises an auction it leads; the whole price must be above the reserve.\n\n";
    PrintAsks(items, config);
    PrintPays(items, table);
    std::cout << "<!-- /decisions -->\n";
}

// ---- the real data: the table for the user ----
std::string ReadFile(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string Words(std::string const& line, std::string const& after)
{
    std::size_t const at = line.find(after);
    if (at == std::string::npos)
        return "?";
    std::size_t const start = at + after.size();
    return line.substr(start, line.find(' ', start) - start);
}

void Tables(std::string const& root)
{
    std::string const recipeText = ReadFile(root + "/tests/fixtures/recipe-index-dump.txt");
    std::string const inputText = ReadFile(root + "/tests/fixtures/auction-reference-inputs.txt");
    RecipeIndex index;
    std::string error;
    Check(RecipeIndex::Parse(recipeText, index, error), "the recipe index fixture parses: " + error);
    std::vector<ItemInput> inputs;
    std::string core;
    bool const parsed = ParseInputs(inputText, inputs, core, error);
    Check(parsed, "the inputs fixture parses: " + error);
    if (!parsed)
        return;
    std::string const recipeCore = Words(recipeText.substr(0, recipeText.find('\n')), " core ");

    auto const start = std::clock();
    Table const table = Build(inputs, index, BANDS);
    double const ms = 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    int realCycles = 0;
    Check(BelowSell(table) == 0 && AboveVendor(table) == 0 && AboveInputs(table, index, &realCycles) == 0,
          "the real table: invariants 1-3 hold");
    std::cout << "real table: invariant 3 above the inputs below a recipe cycle: " << realCycles << '\n';
    Check(table.Find(2589) && table.Find(2770) && table.Find(2840) && table.Find(2447), "the real table: the anchors");
    std::vector<SampleEntry> const sample = Sample(table, index);
    Check(sample.size() >= 30, "the real table: a sample of every cell");
    for (SampleEntry const& entry : sample)
        std::cout << Line(*table.Find(entry.id), *table.Input(entry.id)) << " [" << entry.group << "]\n";

    Counts const& n = table.counts;
    std::cout << "\n<!-- table -->\n# The reference value of every item (auction house WA2a)\n\n";
    std::cout << "- Printed by tests/auction-rules-selftest.cpp over the fixtures: items core " << core
              << ", recipe index core " << recipeCore << (core == recipeCore ? "" : " (two core revisions)") << ".\n";
    std::cout << "- " << table.rows.size() << " items valued (" << n.Of(Basis::Gathered) << " gathered, "
              << n.Of(Basis::Made) << " made, " << n.Of(Basis::Processed) << " processed, " << n.Of(Basis::VendorCap)
              << " vendor-capped, " << n.Of(Basis::Shaped) << " shaped, " << n.Of(Basis::Floor) << " at the floor; "
              << n.bound << " cut off by the recipe bounds) of " << table.inputs.size() << " item templates; "
              << n.LeftOutTotal() << " left out:";
    for (std::size_t r = 1; r < n.leftOut.size(); ++r)
        std::cout << (r > 1 ? "," : "") << ' ' << n.leftOut[r] << ' ' << LeftOutWord(static_cast<LeftOut>(r));
    std::cout << ". Built in " << static_cast<long>(ms) << " ms (CPU time, the harness).\n";
    std::cout << "- The filter: " << FilterText() << ".\n";
    std::cout << "- A typical bot of a band: every profession skill at 450 x the band's top level / 80 (107, 219, 332, "
                 "388, 450), at its gold reserve, no danger, every zone in reach; a minute is worth the band's "
                 "earning rate (30, 100, 250, 500, 1000 copper). An item's band: the lowest band from its required "
                 "level's on whose typical bot can get it.\n";
    std::cout << "- Missing until professions' WRL5: drop rows (WR20a: no loot-source index yet), so kill loot and "
                 "skinning are no source; the cost rule counts the game's default skill-gain and loot rates (WR29). "
                 "WA7a prints this table again after adopting WRL5.\n";
    std::cout << "- Goods with no way of getting them on purpose: buy price per unit (or 4 x the sell price) x "
                 "cmangos's level multiplier x a rarity multiplier by quality (0.92 common, 1.12 uncommon, 1.58 rare, "
                 "2.59 epic); poor items at their sell price. Never below the sell price.\n\n";
    std::cout
        << "| group | id | name | class/sub | q | ilvl | basis | band | copper | g s c | band 0 | band 1 | band 2 "
           "| band 3 | band 4 | sell | buy |\n";
    std::cout << "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (SampleEntry const& entry : sample)
    {
        ItemReference const& row = *table.Find(entry.id);
        ItemInput const& item = *table.Input(entry.id);
        std::cout << "| " << entry.group << " | " << item.id << " | " << item.name << " | " << item.classId << '/'
                  << item.subClass << " | " << item.quality << " | " << item.itemLevel << " | " << BasisWord(row.basis)
                  << " | " << static_cast<int>(row.band) << " | " << row.copper << " | " << Money(row.copper);
        for (std::uint64_t const copper : row.byBand)
            std::cout << " | " << CostText(copper);
        std::cout << " | " << item.sellPrice << " | " << item.buyPrice << (item.buyCount > 1 ? "/" : "")
                  << (item.buyCount > 1 ? std::to_string(item.buyCount) : "") << (item.vendorStocked ? " stocked" : "")
                  << " |\n";
    }
    std::cout << "<!-- /table -->\n";
    Decisions(table);
}
}  // namespace

// WA3b: the seller's expiry memory the visit's ask reads (expiredSteps, quickSale)
void CheckMemory()
{
    AuctionMemory::SellerMemory memory;
    memory.Listed(1, 1000, 1000 + 24 * 3600);
    memory.Expired(1, 77);
    memory.Drop(1);
    memory.Listed(2, 90000, 90000 + 48 * 3600);
    memory.Expired(2, 77);
    Check(memory.Of(77).expiredSteps == 2 && !memory.Of(77).quickSale && memory.listings.empty(),
          "memory: two expiries are two steps, their listings gone");
    Check(memory.Of(78).expiredSteps == 0, "memory: an item never expired has no step");
    memory.Listed(3, 200000, 200000 + 48 * 3600);
    memory.Sold(3, 77, 200000 + 3600);
    Check(memory.Of(77).expiredSteps == 0 && memory.Of(77).quickSale, "memory: a sale in its first quarter is quick");
    memory.Listed(4, 400000, 400000 + 12 * 3600);
    memory.Sold(4, 77, 400000 + 11 * 3600);
    Check(!memory.Of(77).quickSale && memory.Empty(), "memory: a late sale clears the item; nothing is left");
    memory.Sold(9, 78, 500000);
    Check(memory.Empty(), "memory: a sale of an unknown listing leaves nothing");
    memory.Held(79);
    Check(memory.Of(79).expiredSteps == 1, "memory: a held stack counts a step");
    for (std::uint32_t entry = 1000; entry < 1000 + AuctionMemory::ITEMS_MAX; ++entry)
        memory.Held(entry);
    Check(memory.items.size() == AuctionMemory::ITEMS_MAX && memory.Of(79).expiredSteps == 0,
          "memory: bounded, the oldest item goes first");

    // The ask reads it: an expiry lowers the ask by a step, past the relist limit the stock is held
    AskInput in;
    in.count = 1;
    in.sellPrice = 100;
    in.vendorPayout = 100;
    in.reference = 2000;
    in.fees = FACTION_FEES;
    AuctionConfig config;
    AskResult const first = Ask(in, config);
    in.expiredSteps = 1;
    AskResult const relist = Ask(in, config);
    in.expiredSteps = config.RelistLimit + 1;
    AskResult const past = Ask(in, config);
    Check(first.kind == AskResult::Kind::List && relist.kind == AskResult::Kind::List && relist.ask < first.ask &&
              relist.reason == AuctionWords::Reason::Expired,
          "memory: one expiry relists lower");
    Check(past.kind == AskResult::Kind::Hold && StockWorth(in, config) == 0,
          "memory: past the relist limit the ask holds and the item is no stock");
}

int main(int argc, char** argv)
{
    CheckHand();
    Search();
    CheckCases();
    CheckStockWorth();
    CheckMemory();
    CheckPricingHand();
    SearchPricing();
    SearchBuying();
    Tables(argc > 1 ? argv[1] : ".");
    std::cout << "auction-rules: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
