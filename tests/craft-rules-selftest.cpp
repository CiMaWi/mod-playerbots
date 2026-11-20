/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the craft rules (src/Ai/World/Rpg/CraftRules.h, professions plan WR6): the first cause against
// a cast in the order of the visit's ends, the casts the bags allow (stack room, reagent stacks), the order of a queue
// with the bags reserved across recipes, and a seeded search over the recipes of the dumped fixture
// (tests/fixtures/recipe-index-dump.txt): a queue Order returns runs through on the bags without a refused cast, and
// the same search without the reservation finds a counterexample (negative control).
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "CraftRules.h"
#include "RecipeIndex.h"

using namespace CraftRules;

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

constexpr std::uint32_t COPPER_ORE = 2770;
constexpr std::uint32_t COPPER_BAR = 2840;
constexpr std::uint32_t COPPER_BRACERS = 2853;
constexpr std::uint32_t RUNED_ROD_TOOL = 6339;
constexpr std::uint32_t SMELT_COPPER = 2657;
constexpr std::uint32_t BRACERS_SPELL = 2663;
constexpr std::uint32_t TOTEM_HAMMER = 162;
constexpr std::uint32_t BAG_SLOTS = 16;
constexpr std::uint32_t ORE_STACK = 20;
constexpr std::uint32_t SEED = 20261120;
constexpr int SEARCH_RUNS = 20000;
constexpr int MAX_QUEUE = 6;

RecipeView Smelt()
{
    RecipeView recipe;
    recipe.spell = SMELT_COPPER;
    recipe.skillNeeded = 1;
    recipe.product = COPPER_BAR;
    recipe.productCount = 1;
    recipe.focus = Focus::FORGE;
    recipe.reagents = {{COPPER_ORE, 1}};
    return recipe;
}

RecipeView Bracers()
{
    RecipeView recipe;
    recipe.spell = BRACERS_SPELL;
    recipe.skillNeeded = 1;
    recipe.product = COPPER_BRACERS;
    recipe.productCount = 1;
    recipe.focus = Focus::ANVIL;
    recipe.reagents = {{COPPER_BAR, 2}};
    recipe.tools = {RUNED_ROD_TOOL};
    recipe.toolCategories = {TOTEM_HAMMER};
    return recipe;
}

BagState Bags()
{
    BagState bags;
    bags.capacity = BAG_SLOTS;
    bags.skill = 1;
    bags.maxStack[COPPER_ORE] = ORE_STACK;
    bags.maxStack[COPPER_BAR] = ORE_STACK;
    bags.items[COPPER_ORE] = 10;
    bags.items[RUNED_ROD_TOOL] = 1;
    bags.categoriesHeld = {TOTEM_HAMMER};
    bags.focusInReach = {Focus::FORGE, Focus::ANVIL};
    return bags;
}

void CheckCauses()
{
    BagState bags = Bags();
    Check(Check(Smelt(), bags) == Verdict::Castable, "a smelt at a forge with ore is castable");
    Check(VerdictWord(Verdict::NoStation) == "no_station" && VerdictWord(Verdict::BagsFull) == "bags_full",
          "the cause words are the decision reasons");

    BagState noSkill = bags;
    noSkill.skill = 0;
    Check(Check(Smelt(), noSkill) == Verdict::NoSkill, "skill below the recipe's orange value");

    BagState noOre = bags;
    noOre.items.erase(COPPER_ORE);
    Check(Check(Smelt(), noOre) == Verdict::NoMaterials, "no ore");
    Check(Check(Bracers(), bags) == Verdict::NoMaterials, "bracers without bars");

    BagState bars = bags;
    bars.items[COPPER_BAR] = 4;
    Check(Check(Bracers(), bars) == Verdict::Castable, "bracers with bars, tool and an anvil");
    BagState noTool = bars;
    noTool.items.erase(RUNED_ROD_TOOL);
    Check(Check(Bracers(), noTool) == Verdict::NoTool, "a missing tool item");
    BagState noCategory = bars;
    noCategory.categoriesHeld.clear();
    Check(Check(Bracers(), noCategory) == Verdict::NoTool, "a missing tool category");

    BagState noStation = bars;
    noStation.focusInReach = {Focus::FORGE};
    Check(Check(Bracers(), noStation) == Verdict::NoStation, "an anvil recipe at a forge only");

    BagState full = bars;
    full.capacity = full.SlotsUsed();
    Check(Check(Bracers(), full) == Verdict::BagsFull, "no free slot for a product that cannot stack");
    BagState stacks = full;
    stacks.items[COPPER_BAR] = 4;
    Check(Check(Smelt(), stacks) == Verdict::Castable, "a smelt whose bar joins a partly filled stack");
    BagState fullStack = full;
    fullStack.items[COPPER_BAR] = ORE_STACK;
    fullStack.items[COPPER_ORE] = 1;
    BagState rooms = fullStack;
    rooms.capacity = rooms.SlotsUsed();
    Check(Check(Smelt(), rooms) == Verdict::Castable, "the last ore frees its slot for the bar");

    BagState two = bars;
    two.skill = 0;
    two.items.erase(RUNED_ROD_TOOL);
    Check(Check(Bracers(), two) == Verdict::NoSkill, "skill is the first cause");
    Check(InStationReach(4.9f, 5.0f) && InStationReach(5.0f, 5.0f) && !InStationReach(5.1f, 5.0f),
          "the station's own range is inclusive");
    Check(IsStationFocus(Focus::FORGE) && !IsStationFocus(2), "the station focus set");
}

void CheckCounts()
{
    BagState bags = Bags();
    Check(CastsAllowed(Smelt(), bags) == 10, "ten ore, ten smelts");
    bags.items[COPPER_ORE] = 5;
    Check(CastsAllowed(Smelt(), bags) == 5, "five ore, five smelts");

    BagState tight = Bags();
    tight.items[COPPER_ORE] = 15;
    tight.capacity = tight.SlotsUsed() + 1;  // the ore stack, the tool and one free slot
    Check(CastsAllowed(Smelt(), tight) == 15, "fifteen bars fit one stack in the free slot");
    tight.capacity = tight.SlotsUsed();
    Check(CastsAllowed(Smelt(), tight) == 0, "no free slot, the first bar has no room");

    BagState noRoom = Bags();
    noRoom.items[COPPER_ORE] = 15;
    noRoom.items[COPPER_BAR] = ORE_STACK;
    noRoom.capacity = noRoom.SlotsUsed();
    Check(CastsAllowed(Smelt(), noRoom) == 0, "a full bar stack and no free slot: no room");

    BagState stationless = Bags();
    stationless.focusInReach.clear();
    Check(CastsAllowed(Smelt(), stationless) == 0, "no station, no casts");
}

void CheckOrder()
{
    BagState bags = Bags();
    QueueEntry bracers{Bracers(), 2, 0};
    QueueEntry smelt{Smelt(), 1, 0};
    std::vector<Planned> const plan = Order({bracers, smelt}, bags);
    Check(plan.size() == 2 && plan[0].spell == SMELT_COPPER && plan[0].casts == 10 && plan[1].spell == BRACERS_SPELL &&
              plan[1].casts == 5,
          "smelt first, its bars feed the bracers (ten bars, five bracers)");
    Check(Order({bracers}, bags).empty(), "bracers alone are not castable without bars");

    // Two recipes on the same ore: the first takes it, the second gets what is left
    RecipeView other = Smelt();
    other.spell = SMELT_COPPER + 1;
    other.product = COPPER_BAR + 1;
    std::vector<Planned> const split = Order({QueueEntry{other, 1, 6}, QueueEntry{Smelt(), 1, 6}}, bags);
    Check(split.size() == 2 && split[0].spell == SMELT_COPPER && split[0].casts == 6 && split[1].casts == 4,
          "equal rank goes by spell id and the bags are reserved (6 then 4 of 10)");
    std::vector<Planned> const wanted = Order({QueueEntry{Smelt(), 1, 3}}, bags);
    Check(wanted.size() == 1 && wanted[0].casts == 3, "wanted caps the casts");
}

// ---- the search ----

CraftRules::RecipeView ViewOf(Recipes::Recipe const& recipe)
{
    RecipeView view;
    view.spell = recipe.spell;
    view.skillNeeded = recipe.minRank;
    view.focus = recipe.focus;
    if (recipe.effect == Recipes::Effect::Make)
    {
        view.product = recipe.product;
        view.productCount = recipe.productCount;
    }
    for (Recipes::ItemCount const& reagent : recipe.reagents)
        view.reagents.push_back({reagent.item, reagent.count});
    view.tools = recipe.tools;
    view.toolCategories = recipe.toolCategories;
    return view;
}

std::uint32_t StackOfItem(std::uint32_t item) { return item % 3 == 0 ? 1u : ORE_STACK; }

// Replays a plan on a copy of the bags; false at the first refused cast
bool Replay(std::vector<Planned> const& plan, std::vector<QueueEntry> const& queue, BagState bags)
{
    for (Planned const& step : plan)
    {
        RecipeView const* recipe = nullptr;
        for (QueueEntry const& entry : queue)
            if (entry.recipe.spell == step.spell)
                recipe = &entry.recipe;
        if (!recipe)
            return false;
        for (std::uint32_t i = 0; i < step.casts; ++i)
        {
            if (Check(*recipe, bags) != Verdict::Castable)
                return false;
            Apply(*recipe, bags);
        }
    }
    return true;
}

// Without the reservation: every entry gets the casts the original bags allow
std::vector<Planned> OrderUnreserved(std::vector<QueueEntry> queue, BagState const& bags)
{
    std::stable_sort(queue.begin(), queue.end(), [](QueueEntry const& a, QueueEntry const& b)
                     { return a.rank != b.rank ? a.rank < b.rank : a.recipe.spell < b.recipe.spell; });
    std::vector<Planned> plan;
    for (QueueEntry const& entry : queue)
        if (std::uint32_t const casts = CastsAllowed(entry.recipe, bags))
            plan.push_back({entry.recipe.spell, casts});
    return plan;
}

void Search(std::string const& root)
{
    std::ifstream in(root + "/tests/fixtures/recipe-index-dump.txt", std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    Recipes::RecipeIndex index;
    std::string error;
    bool const parsed = Recipes::RecipeIndex::Parse(buffer.str(), index, error);
    Check(parsed, "the dumped fixture parses: " + error);
    if (!parsed)
        return;
    std::vector<RecipeView> makes;
    for (Recipes::Recipe const& recipe : index.Recipes())
        if (recipe.effect == Recipes::Effect::Make && !recipe.reagents.empty())
            makes.push_back(ViewOf(recipe));
    Check(makes.size() > 1000, "the fixture holds many crafts");

    std::mt19937 rng(SEED);
    auto const pick = [&rng](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
    int refusedWithReservation = 0;
    int refusedWithout = 0;
    int accepted = 0;
    int boundBroken = 0;
    for (int run = 0; run < SEARCH_RUNS; ++run)
    {
        std::vector<QueueEntry> queue;
        BagState bags;
        bags.capacity = 4 + static_cast<std::uint32_t>(pick(30));
        bags.skill = static_cast<std::uint32_t>(pick(451));
        std::size_t const size = 1 + pick(MAX_QUEUE);
        for (std::size_t i = 0; i < size; ++i)
        {
            QueueEntry entry;
            entry.recipe = makes[pick(makes.size())];
            entry.rank = static_cast<std::uint32_t>(pick(3));
            entry.wanted = pick(3) == 0 ? static_cast<std::uint32_t>(1 + pick(5)) : 0;
            // Stock the bags for the recipe: whole multiples, sometimes one short; chain a bar into a recipe that
            // uses another's product now and then
            std::uint32_t const multiples = static_cast<std::uint32_t>(pick(4));
            for (Reagent const& reagent : entry.recipe.reagents)
            {
                std::uint32_t const have = reagent.count * multiples - (pick(5) == 0 && multiples ? 1u : 0u);
                bags.items[reagent.item] += have;
                bags.maxStack[reagent.item] = StackOfItem(reagent.item);
            }
            if (entry.recipe.product)
                bags.maxStack[entry.recipe.product] = StackOfItem(entry.recipe.product);
            for (std::uint32_t const tool : entry.recipe.tools)
                if (pick(10))
                    bags.items[tool] = 1;
            for (std::uint32_t const category : entry.recipe.toolCategories)
                if (pick(10))
                    bags.categoriesHeld.insert(category);
            if (entry.recipe.focus && pick(10))
                bags.focusInReach.insert(entry.recipe.focus);
            queue.push_back(entry);
        }
        for (auto it = bags.items.begin(); it != bags.items.end();)
            it = it->second ? std::next(it) : bags.items.erase(it);

        std::vector<Planned> const plan = Order(queue, bags);
        accepted += plan.empty() ? 0 : 1;
        if (!Replay(plan, queue, bags))
            ++refusedWithReservation;
        // CastsAllowed never exceeds what the bags hold of any reagent
        for (QueueEntry const& entry : queue)
        {
            std::uint32_t const casts = CastsAllowed(entry.recipe, bags);
            for (Reagent const& reagent : entry.recipe.reagents)
            {
                std::uint32_t const back = reagent.item == entry.recipe.product ? entry.recipe.productCount : 0u;
                if (casts * reagent.count > bags.Held(reagent.item) + casts * back)
                    ++boundBroken;
            }
            if (casts && Check(entry.recipe, bags) != Verdict::Castable)
                ++boundBroken;
        }
        if (!Replay(OrderUnreserved(queue, bags), queue, bags))
            ++refusedWithout;
    }
    std::cout << "search: " << SEARCH_RUNS << " queues, " << accepted << " with casts, " << refusedWithReservation
              << " refused with the reservation, " << refusedWithout << " without (negative control)\n";
    Check(accepted > SEARCH_RUNS / 10, "the search accepts queues often enough to mean something");
    Check(refusedWithReservation == 0, "every queue Order returns runs through without a refused cast");
    Check(boundBroken == 0, "CastsAllowed never exceeds what the bags hold and agrees with Check");
    Check(refusedWithout > 0, "without the reservation the search finds a counterexample (negative control)");
}
}  // namespace

int main(int argc, char** argv)
{
    CheckCauses();
    CheckCounts();
    CheckOrder();
    Search(argc > 1 ? argv[1] : ".");
    std::cout << "CRAFT rules selftest: " << passed + failed << " checks, " << failed << " failed ("
              << (failed ? "FAIL" : "PASS") << ")\n";
    return failed ? 1 : 0;
}
