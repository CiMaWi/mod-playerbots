/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the recipe index (src/Ai/World/Rpg/RecipeIndex.h, professions plan WR3a) on a small hand-made
// gather: skill thresholds, reagents, tools and station, the three ways a recipe is learned (trainer, an item that is
// sold or found, the skill itself), a rank ladder, the loot stores' expected yields, "what an item is made from", and
// the dump read back unchanged. The dumped fixture's checks are WR3b's.
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "RecipeIndex.h"

using namespace Recipes;

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

constexpr std::uint16_t ALCHEMY = 171;
constexpr std::uint16_t BLACKSMITHING = 164;

SpellRow Spell(std::uint32_t id, std::string name, Effect effect = Effect::Make, std::uint32_t product = 0)
{
    SpellRow row;
    row.spell = id;
    row.name = std::move(name);
    row.effect = effect;
    row.product = product;
    row.productCount = product ? 1 : 0;
    row.castMs = 3000;
    return row;
}

AbilityRow Ability(std::uint16_t skill, std::uint32_t spell, std::uint32_t orange, std::uint32_t yellow,
                   std::uint32_t grey, std::uint32_t acquire)
{
    AbilityRow row;
    row.skill = skill;
    row.spell = spell;
    row.minRank = orange;
    row.yellow = yellow;
    row.grey = grey;
    row.acquire = acquire;
    return row;
}

ItemFact Item(std::uint32_t id, std::string name, bool known = true)
{
    ItemFact fact;
    fact.id = id;
    fact.known = known;
    fact.name = std::move(name);
    return fact;
}

Gather HandGather()
{
    Gather g;
    // 1000: an elixir taught by a trainer through a wrapper spell (1001), two reagents
    g.abilities.push_back(Ability(ALCHEMY, 1000, 90, 110, 130, ACQUIRE_ON_SKILL_VALUE));
    g.spells.push_back(Spell(1000, "Elixir of Testing", Effect::Make, 9001));
    g.spells.back().reagents = {{2447, 2}, {3371, 1}};
    g.trainers.push_back({100, 1001, 1000, 500, ALCHEMY, 90, 10});
    g.trainers.push_back({101, 1001, 1000, 400, ALCHEMY, 90, 10});  // a second, cheaper trainer
    // 3000: learned from an item that two vendors sell
    g.abilities.push_back(Ability(ALCHEMY, 3000, 150, 170, 190, 0));
    g.spells.push_back(Spell(3000, "Potion of Recipes", Effect::Make, 9001));
    g.spells.back().reagents = {{2447, 3}};
    g.recipeItems.push_back({6000, 3000, 2, 2000});
    // 4000: learned from an item found in the world (no vendor)
    g.abilities.push_back(Ability(ALCHEMY, 4000, 200, 215, 230, 0));
    g.spells.push_back(Spell(4000, "Flask of Finding", Effect::Make, 9002));
    g.spells.back().reagents = {{2447, 1}, {9001, 1}};
    g.recipeItems.push_back({6001, 4000, 0, 0});
    // 5000: no source at all; 5100: learned with the skill
    g.abilities.push_back(Ability(ALCHEMY, 5000, 250, 260, 270, 0));
    g.spells.push_back(Spell(5000, "Orphan Brew", Effect::Make, 9003));
    g.abilities.push_back(Ability(ALCHEMY, 5100, 1, 1, 1, ACQUIRE_ON_SKILL_LEARN));
    g.spells.push_back(Spell(5100, "Basic Know-how", Effect::Other));
    // 2000: blacksmithing, with a tool and a station
    g.abilities.push_back(Ability(BLACKSMITHING, 2000, 1, 20, 40, ACQUIRE_ON_SKILL_VALUE));
    g.spells.push_back(Spell(2000, "Copper Rod", Effect::Make, 9004));
    g.spells.back().reagents = {{2840, 1}};
    g.spells.back().tools = {5956};
    g.spells.back().focus = 3;
    g.spells.back().toolCategories = {14};
    g.trainers.push_back({102, 2000, 2000, 10, BLACKSMITHING, 1, 1});
    // The alchemy rank ladder, two rungs through a wrapper
    std::uint32_t const rungs[][3] = {{2259, 75, 1}, {3101, 150, 2}, {3464, 225, 3}, {11611, 300, 4}, {28596, 375, 5}};
    for (auto const& rung : rungs)
    {
        SpellRow row = Spell(rung[0], "Rank " + std::to_string(rung[2]), Effect::Other);
        row.rankSkill = ALCHEMY;
        row.rankCap = rung[1];
        row.rankStep = rung[2];
        g.spells.push_back(row);
        g.trainers.push_back({103, rung[0], rung[0], 100 * rung[2], ALCHEMY, rung[1] - 75, 5 * rung[2]});
    }
    g.processes.push_back({ProcessKind::Prospect, 2770, 755, 1, 20, 20, 2, PROSPECT_MILL_COUNT, {{774, 0.5}}});
    for (std::uint32_t const id : {2447u, 3371u, 2840u, 5956u, 9001u, 9002u, 9003u, 9004u, 6000u, 6001u})
        g.items.push_back(Item(id, "item " + std::to_string(id)));
    return g;
}

Recipe const* Find(RecipeIndex const& index, std::uint32_t spell)
{
    for (Recipe const& recipe : index.Recipes())
        if (recipe.spell == spell)
            return &recipe;
    return nullptr;
}

void CheckThresholds(RecipeIndex const& index)
{
    Recipe const* elixir = Find(index, 1000);
    Check(elixir != nullptr, "the elixir is in the index");
    if (!elixir)
        return;
    Check(elixir->minRank == 90 && elixir->yellow == 110 && elixir->grey == 130, "orange, yellow, grey kept");
    Check(elixir->Green() == 120, "green is the middle of yellow and grey");
    Check(elixir->reagents.size() == 2 && elixir->reagents[0].item == 2447 && elixir->reagents[0].count == 2,
          "reagents and counts");
    Check(elixir->product == 9001 && elixir->productCount == 1 && elixir->castMs == 3000, "product and cast time");

    Recipe const* rod = Find(index, 2000);
    Check(rod && rod->tools == std::vector<std::uint32_t>{5956} && rod->focus == 3 &&
              rod->toolCategories == std::vector<std::uint32_t>{14},
          "tool, tool category and station");
    // sorted by skill, then threshold
    Check(index.Recipes().front().skill == BLACKSMITHING, "recipes sorted by skill line");
    Check(index.Recipes()[1].spell == 5100 || index.Recipes()[1].minRank <= index.Recipes()[2].minRank,
          "recipes sorted by threshold within a skill");
}

void CheckLearned(RecipeIndex const& index)
{
    Recipe const* elixir = Find(index, 1000);
    Check(elixir && HowLearned(*elixir) == "trainer", "a trainer's recipe says trainer");
    Check(elixir && elixir->trainers == 2 && elixir->trainerCopper == 400 && elixir->trainerRank == 90 &&
              elixir->trainerLevel == 10,
          "the cheapest of two trainers, counted");
    Recipe const* potion = Find(index, 3000);
    Check(potion && HowLearned(*potion) == "item-vendor" && potion->items.size() == 1 &&
              potion->items[0].item == 6000 && potion->items[0].vendors == 2 && potion->items[0].buyCopper == 2000,
          "a recipe item that vendors sell");
    Recipe const* flask = Find(index, 4000);
    Check(flask && HowLearned(*flask) == "item-found", "a recipe item nobody sells is found");
    Recipe const* orphan = Find(index, 5000);
    Check(orphan && HowLearned(*orphan) == "none", "a recipe without a source says none");
    Recipe const* skill = Find(index, 5100);
    Check(skill && HowLearned(*skill) == "skill", "a recipe learned with the skill");
    Recipe const* rod = Find(index, 2000);
    Check(rod && HowLearned(*rod) == "trainer", "a trainer wins over the rank rule");
}

void CheckLadder(RecipeIndex const& index)
{
    std::vector<Rung> const ladder = index.Ladder(ALCHEMY);
    Check(ladder.size() == 5, "five rungs");
    bool ordered = true;
    for (std::size_t i = 1; i < ladder.size(); ++i)
        ordered = ordered && ladder[i - 1].cap < ladder[i].cap;
    Check(ordered, "rungs by cap");
    Check(ladder.size() == 5 && ladder[3].cap == 300 && ladder[3].copper == 400 && ladder[3].reqRank == 225 &&
              ladder[3].reqLevel == 20,
          "the 300 rung: price, skill needed, level");
    Check(index.Ladder(BLACKSMITHING).empty(), "no rungs gathered for another skill");
}

void CheckMadeBy(RecipeIndex const& index)
{
    std::vector<std::size_t> const& made = index.MadeBy(9001);
    Check(made.size() == 2, "two recipes make the item");
    Check(index.MadeBy(1).empty(), "an item nothing makes");
    for (std::size_t const at : made)
        Check(index.Recipes()[at].product == 9001, "MadeBy points at recipes that make it");
    // the graph checks WR3b runs on the dump
    for (Recipe const& recipe : index.Recipes())
        for (ItemCount const& reagent : recipe.reagents)
        {
            Check(reagent.item != recipe.product, "no recipe is its own input (" + recipe.name + ")");
            ItemFact const* fact = index.Item(reagent.item);
            Check(fact && fact->known, "every reagent is a known item (" + recipe.name + ")");
        }
}

void CheckLoot()
{
    LootTable table;
    table[1] = {{10, 0, 50.0f, 0, 1, 3},                           // 0.5 x 2 average
                {11, 0, 0.0f, 1, 1, 1},                            // a group of two zero-chance items: half each
                {12, 0, 0.0f, 1, 1, 1},  {13, 0, 30.0f, 2, 2, 2},  // an explicit 30 % and the 70 % rest
                {14, 0, 0.0f, 2, 1, 1},  {0, 2, 25.0f, 0, 1, 3}};  // a quarter chance to roll template 2 three times
    table[2] = {{20, 0, 100.0f, 0, 2, 2}};
    auto const expected = [&](std::uint32_t item)
    {
        for (Yield const& yield : ExpectedYield(table, 1))
            if (yield.item == item)
                return yield.expected;
        return 0.0;
    };
    auto const same = [](double a, double b) { return a > b - 1e-9 && a < b + 1e-9; };
    Check(same(expected(10), 1.0), "ungrouped chance times the average count");
    Check(same(expected(11), 0.5) && same(expected(12), 0.5), "a zero-chance group splits the roll");
    Check(same(expected(13), 0.6) && same(expected(14), 0.7), "explicit chance, then the rest to the unset item");
    Check(same(expected(20), 1.5), "a reference rolls its template max-count times at its chance");
    Check(ExpectedYield(table, 99).empty(), "an unknown template yields nothing");

    // The server's references name templates of the reference table, not of the table the row is in
    LootTable references;
    references[2] = {{30, 0, 100.0f, 0, 1, 1}};
    double fromReference = 0.0;
    bool ownTwenty = false;
    for (Yield const& yield : ExpectedYield(table, 1, &references))
    {
        if (yield.item == 30)
            fromReference = yield.expected;
        ownTwenty = ownTwenty || yield.item == 20;
    }
    Check(same(fromReference, 0.75) && !ownTwenty, "a reference is read in the reference table");
}

void CheckDump(RecipeIndex const& index)
{
    std::string const text = index.Dump("test", "core");
    Check(text.rfind("# recipe-index-dump v1 module test core core records ", 0) == 0, "dump header");
    RecipeIndex back;
    std::string error;
    Check(RecipeIndex::Parse(text, back, error), "the dump parses: " + error);
    Check(back.Dump("test", "core") == text, "the dump reads back unchanged");
    Check(back.Records() == index.Records() && back.MadeBy(9001).size() == 2 && back.Ladder(ALCHEMY).size() == 5,
          "the read-back index answers the same");
    Check(back.Processes().size() == 1 && back.Processes()[0].yields.size() == 1 &&
              back.Processes()[0].consumed == PROSPECT_MILL_COUNT,
          "a processing row with its yields survives the dump");

    std::string broken = text;
    broken.erase(broken.rfind("I\t"));  // lose the last item record: the header's count no longer holds
    Check(!RecipeIndex::Parse(broken, back, error), "a dump with a missing record is refused");
    Check(!RecipeIndex::Parse("R\tx\n", back, error), "a dump without header or fields is refused");
}
}  // namespace

int main()
{
    std::cout << "RECIPES selftest (RecipeIndex.h)\n";
    RecipeIndex const index = RecipeIndex::Build(HandGather());
    std::cout << index.Summary(ALCHEMY, "alchemy") << '\n' << index.Summary(BLACKSMITHING, "blacksmithing") << '\n';
    Check(index.Recipes().size() == 6, "six abilities become six recipes");
    CheckThresholds(index);
    CheckLearned(index);
    CheckLadder(index);
    CheckMadeBy(index);
    CheckLoot();
    CheckDump(index);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
