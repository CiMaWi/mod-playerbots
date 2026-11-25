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
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

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
}
}  // namespace

int main(int argc, char** argv)
{
    CheckHand();
    Search();
    Tables(argc > 1 ? argv[1] : ".");
    std::cout << "auction-rules: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
