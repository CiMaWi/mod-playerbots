/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the recipe index (src/Ai/World/Rpg/RecipeIndex.h, professions plan WR3a) on a small hand-made
// gather: skill thresholds, reagents, tools and station, the three ways a recipe is learned (trainer, an item that is
// sold or found, the skill itself), a rank ladder, the loot stores' expected yields, "what an item is made from", and
// the dump read back unchanged. WR3b adds checks on the dumped fixture (tests/fixtures/recipe-index-dump.txt, written
// by `playerbots professions index dump` on a lab): every profession has its old-world ladder to 300, every reagent is
// a known item, no recipe is its own input, and WR2's table of crafts by class holds (Usable: gear the class
// wears, item use's demand answer for the class), with every craft for every class as a negative control that
// must fail. WR20a adds the trainer rows' abilities (WR.27: a recipe behind an ability the learner lacks is not
// taught, with a rule that ignores them as the negative control) and the drop rows: built from hand-made sources and
// creatures (the floor, quest-only, both-sides and instance sources left out, a control without the floor), dump v6
// round trip, and a v5 dump still read.
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ItemDemandRules.h"
#include "ItemUseFacts.h"
#include "ProfessionRules.h"
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
    // WR.27: 7000 taught only to a learner with the specialisation 7100 (a profession spell, no rank); 7200 listed by
    // a cheap trainer that asks for 7100 and a dearer one that asks for nothing; 7300 behind a rank spell (3101, the
    // ladder's: not listed)
    g.abilities.push_back(Ability(ALCHEMY, 7100, 1, 1, 1, 0));
    g.spells.push_back(Spell(7100, "Elixir Specialist", Effect::Other));
    g.abilities.push_back(Ability(ALCHEMY, 7000, 100, 120, 140, 0));
    g.spells.push_back(Spell(7000, "Specialist Elixir", Effect::Make, 9005));
    g.spells.back().reagents = {{2447, 1}};
    g.trainers.push_back({104, 7000, 7000, 300, ALCHEMY, 100, 10, {7100, 0, 0}});
    g.abilities.push_back(Ability(ALCHEMY, 7200, 100, 120, 140, 0));
    g.spells.push_back(Spell(7200, "Either Elixir", Effect::Make, 9006));
    g.spells.back().reagents = {{2447, 1}};
    g.trainers.push_back({105, 7200, 7200, 100, ALCHEMY, 100, 10, {0, 7100, 0}});
    g.trainers.push_back({106, 7200, 7200, 200, ALCHEMY, 100, 10});
    g.abilities.push_back(Ability(ALCHEMY, 7300, 100, 120, 140, 0));
    g.spells.push_back(Spell(7300, "Ranked Elixir", Effect::Make, 9007));
    g.spells.back().reagents = {{2447, 1}};
    g.trainers.push_back({107, 7300, 7300, 50, ALCHEMY, 100, 10, {3101, 0, 0}});
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
    g.items.back().stats = (std::uint64_t{1} << 5) | (std::uint64_t{1} << 45);  // a stat profile survives the dump
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
    Check(text.rfind("# recipe-index-dump v6 module test core core records ", 0) == 0, "dump header");
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
    Recipe const* spec = Find(back, 7000);
    Check(spec && spec->trainerAbilities == std::vector<std::uint32_t>{7100}, "a trainer's abilities survive the dump");

    // An older dump (v5: no trainer abilities, no drop rows) still reads
    std::istringstream lines(text.substr(text.find('\n') + 1));
    std::string line;
    std::string body;
    std::size_t records = 0;
    while (std::getline(lines, line))
    {
        if (line.rfind("#D", 0) == 0 || line.rfind("D\t", 0) == 0)
            continue;
        if (line.rfind("R\t", 0) == 0)
            line.erase(line.rfind('\t'));
        if (!line.empty() && line[0] != '#')
            ++records;
        body += line + '\n';
    }
    std::string const older =
        "# recipe-index-dump v5 module test core core records " + std::to_string(records) + "\n" + body;
    RecipeIndex fromOlder;
    Check(RecipeIndex::Parse(older, fromOlder, error), "a v5 dump still parses: " + error);
    Check(fromOlder.Recipes().size() == index.Recipes().size() && Find(fromOlder, 7000) &&
              Find(fromOlder, 7000)->trainerAbilities.empty() && fromOlder.Drops().empty(),
          "a v5 dump reads its recipes, without abilities or drop rows");
}

// ---- WR20a: the trainer rows' abilities (WR.27) ----

using Teaches = std::function<bool(Recipe const&, std::function<bool(std::uint32_t)> const&)>;

// The rule: a recipe behind an ability is not taught to a learner without it, is with it, and a recipe a trainer
// teaches without one is taught to anyone
bool GuardHolds(RecipeIndex const& index, Teaches const& teaches)
{
    Recipe const* spec = Find(index, 7000);
    Recipe const* either = Find(index, 7200);
    Recipe const* elixir = Find(index, 1000);
    auto const knowsNothing = [](std::uint32_t) { return false; };
    auto const knowsSpecialist = [](std::uint32_t spell) { return spell == 7100; };
    return spec && either && elixir && !teaches(*spec, knowsNothing) && teaches(*spec, knowsSpecialist) &&
           teaches(*either, knowsNothing) && teaches(*elixir, knowsNothing);
}

void CheckAbilities(RecipeIndex const& index)
{
    Recipe const* spec = Find(index, 7000);
    Recipe const* either = Find(index, 7200);
    Recipe const* ranked = Find(index, 7300);
    Check(spec && spec->trainerAbilities == std::vector<std::uint32_t>{7100}, "a trainer row's specialisation is kept");
    Check(either && either->trainerAbilities.empty() && either->trainerCopper == 200 && either->trainers == 2,
          "a trainer row that asks for nothing wins over a cheaper one that asks");
    Check(ranked && ranked->trainerAbilities.empty(), "a rank spell asked for is the ladder's, not listed");
    Teaches const rule = [](Recipe const& recipe, std::function<bool(std::uint32_t)> const& knows)
    { return TrainerTeaches(recipe, knows); };
    Teaches const ignoring = [](Recipe const&, std::function<bool(std::uint32_t)> const&) { return true; };
    Check(GuardHolds(index, rule),
          "WR.27: a recipe behind an ability the learner lacks is not learnable; with it, it is");
    Check(!GuardHolds(index, ignoring), "negative control: a rule that ignores the abilities is caught");
}

// ---- WR20a: drop rows ----

constexpr std::uint32_t LINEN = 2589;
constexpr std::uint32_t LEATHER = 2318;
constexpr std::uint32_t MEAT = 769;
constexpr std::uint16_t SKINNING = 393;

DropWorld HandDropWorld()
{
    DropWorld world;
    world.creatures = {
        {800, 5, 7, false}, {801, 20, 21, true}, {802, 10, 10, false}, {803, 30, 30, false}, {804, 8, 8, false}};
    // 800: three spawns in zone 12 (nearest 30, 30, 40 yards), one in zone 40; 801 two in zone 12; 802 (friendly to
    // both sides) and 804 (a rare drop) one each; 803 has none in the open world (an instance creature)
    world.spawns = {{1, 800, 12, 0, 0.0f, 0.0f, 0.0f, 300},    {2, 800, 12, 0, 30.0f, 0.0f, 0.0f, 300},
                    {3, 800, 12, 0, 30.0f, 40.0f, 0.0f, 600},  {4, 800, 40, 1, 0.0f, 0.0f, 0.0f, 120},
                    {5, 801, 12, 0, 500.0f, 0.0f, 0.0f, 900},  {6, 801, 12, 0, 500.0f, 100.0f, 0.0f, 900},
                    {7, 802, 12, 0, 100.0f, 100.0f, 0.0f, 60}, {8, 804, 12, 0, 200.0f, 200.0f, 0.0f, 60}};
    return world;
}

std::vector<DropSource> HandSources(std::uint32_t item)
{
    auto const source =
        [](DropKind kind, std::uint32_t entry, float chance, std::uint8_t minCount, std::uint8_t maxCount)
    {
        DropSource one;
        one.kind = kind;
        one.entry = entry;
        one.chance = chance;
        one.minCount = minCount;
        one.maxCount = maxCount;
        return one;
    };
    std::vector<DropSource> sources;
    if (item == LINEN)
    {
        sources.push_back(source(DropKind::Corpse, 800, 0.3f, 1, 2));
        sources.push_back(source(DropKind::Corpse, 801, 0.5f, 1, 1));
        sources.push_back(source(DropKind::Corpse, 802, 0.9f, 1, 1));
        sources.back().friendlyTo = DROP_BOTH_SIDES;
        sources.push_back(source(DropKind::Corpse, 803, 0.4f, 1, 1));
        sources.push_back(source(DropKind::Corpse, 804, 0.01f, 1, 1));
    }
    else if (item == LEATHER)
    {
        sources.push_back(source(DropKind::Skin, 800, 1.0f, 1, 1));
        sources.back().skill = SKINNING;
        sources.back().friendlyTo = 1;  // friendly to one side: kept, the side is the reader's filter
    }
    else if (item == MEAT)
    {
        sources.push_back(source(DropKind::Corpse, 800, 0.5f, 1, 1));
        sources.back().questOnly = true;
    }
    return sources;
}

bool Near(double a, double b) { return a > b - 0.01 && a < b + 0.01; }

DropRow const* RowOf(std::vector<DropRow> const& rows, std::uint32_t item, std::uint32_t zone, std::uint32_t entry)
{
    for (DropRow const& row : rows)
        if (row.item == item && row.zone == zone && row.entry == entry)
            return &row;
    return nullptr;
}

void CheckDrops(RecipeIndex const& hand)
{
    DropWorld const world = HandDropWorld();
    DropBuild const built = BuildDrops({LINEN, LEATHER, MEAT, LINEN}, HandSources, world, 0.02f);
    std::cout << "drop rows: " << built.rows.size() << " rows, " << built.spawns.size() << " spawns; left out "
              << built.counts.underFloor << " under the floor, " << built.counts.questOnly << " quest-only, "
              << built.counts.bothSides << " both sides, " << built.counts.noOpenSpawn << " without open spawn\n";
    Check(built.rows.size() == 5, "linen from 800 in two zones and 801, leather skinned from 800 in two zones");
    DropRow const* boars = RowOf(built.rows, LINEN, 12, 800);
    Check(boars && boars->spawns == 3 && Near(boars->spacing, 100.0 / 3.0) && boars->respawnSecs == 400 &&
              Near(boars->perKill, 0.45) && boars->minLevel == 5 && boars->maxLevel == 7 && boars->eliteShare == 0.0f &&
              boars->kind == DropKind::Corpse && boars->skill == 0,
          "a corpse row: spawns, mean nearest spacing, mean respawn, chance x mean count, levels");
    DropRow const* lone = RowOf(built.rows, LINEN, 40, 800);
    Check(lone && lone->spawns == 1 && lone->spacing == LONE_SPAWN_YARDS, "a lone spawn takes the lone spacing");
    DropRow const* elites = RowOf(built.rows, LINEN, 12, 801);
    Check(elites && elites->eliteShare == 1.0f && Near(elites->perKill, 0.5), "an elite kind's row");
    DropRow const* skin = RowOf(built.rows, LEATHER, 12, 800);
    Check(skin && skin->kind == DropKind::Skin && skin->skill == SKINNING && skin->friendlyTo == 1,
          "a skin row carries its skill line and the side it is friendly to");
    Check(!RowOf(built.rows, LINEN, 12, 802) && !RowOf(built.rows, LINEN, 12, 803) &&
              !RowOf(built.rows, LINEN, 12, 804) && !RowOf(built.rows, MEAT, 12, 800),
          "left out: friendly to both sides, no open-world spawn, under the floor, quest-only");
    Check(built.counts.underFloor == 1 && built.counts.questOnly == 1 && built.counts.bothSides == 1 &&
              built.counts.noOpenSpawn == 1 && built.counts.items == 2 && built.counts.creatures == 2 &&
              built.counts.zones == 2,
          "the counts of what was kept and left out");
    Check(built.spawns.size() == 6, "the spawns of the rows' kinds are kept, no other");
    DropBuild const floorless = BuildDrops({LINEN}, HandSources, world, 0.0f);
    Check(RowOf(floorless.rows, LINEN, 12, 804) != nullptr,
          "control: without the floor the rare source is a row (the floor check can fail)");

    RecipeIndex index = hand;
    index.SetDrops(built.rows, built.spawns);
    Check(index.DropsOf(LINEN).size() == 3 && index.DropsOf(LEATHER).size() == 2 && index.DropsOf(MEAT).empty(),
          "DropsOf by item");
    std::span<DropSpawn const> const at = index.DropSpawnsOf(12, 800);
    Check(at.size() == 3 && at[0].spawnId == 1 && at[2].spawnId == 3, "a kind's spawn points in a zone, by spawn id");
    Check(index.DropSpawnsOf(12, 802).empty(), "no spawn table entry for a kind without a row");

    std::string const text = index.Dump("test", "core");
    RecipeIndex back;
    std::string error;
    Check(RecipeIndex::Parse(text, back, error), "a dump with drop rows parses: " + error);
    Check(back.Dump("test", "core") == text, "the drop rows read back unchanged");
    Check(back.Drops().size() == 5 && back.DropsOf(LINEN).size() == 3 && back.DropSpawns().empty(),
          "the read-back rows answer the same; their spawns are not in the dump");
    std::string broken = text;
    broken.erase(broken.rfind("D\t"));
    Check(!RecipeIndex::Parse(broken, back, error), "control: a dump that lost a drop row is refused");
}

// ---- WR3b: the dumped fixture ----

bool ReadFile(std::string const& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

// "A ladder to 300 taught in the old world": the rungs of cap 75, 150, 225 and 300 exist and a trainer teaches each
// (the 375 and 450 rungs are Outland's and Northrend's)
void CheckLadders(RecipeIndex const& index)
{
    for (ProfessionRules::ProfessionInfo const& profession : ProfessionRules::PROFESSIONS)
    {
        std::uint16_t const skill = static_cast<std::uint16_t>(profession.Which);
        std::string missing;
        for (std::uint32_t const cap : {75u, 150u, 225u, 300u})
        {
            bool taught = false;
            for (Rung const& rung : index.Ladder(skill))
                taught = taught || (rung.cap == cap && rung.trainers > 0);
            if (!taught)
                missing += " " + std::to_string(cap);
        }
        Check(missing.empty(), std::string(profession.Name) + " has a trained ladder to 300 (missing:" + missing + ")");
    }
}

// Recipes whose product is one of their reagents on purpose: reloading a device gives the device back. The recipe
// graph (WA) reads them as a reload, never as a source of the item
std::set<std::uint32_t> const RELOADS = {13240};  // The Mortar: Reloaded

void CheckReagents(RecipeIndex const& index)
{
    std::size_t reagents = 0;
    std::size_t unknown = 0;
    std::size_t selfInput = 0;
    for (Recipe const& recipe : index.Recipes())
        for (ItemCount const& reagent : recipe.reagents)
        {
            ++reagents;
            ItemFact const* fact = index.Item(reagent.item);
            if (!fact || !fact->known)
            {
                ++unknown;
                std::cout << "unknown reagent " << reagent.item << " of " << recipe.spell << ' ' << recipe.name << '\n';
            }
            if (reagent.item == recipe.product && recipe.product && !RELOADS.count(recipe.spell))
            {
                ++selfInput;
                std::cout << "own input: " << recipe.spell << ' ' << recipe.name << '\n';
            }
        }
    std::cout << "reagents: " << reagents << " in " << index.Recipes().size() << " recipes, " << unknown << " unknown, "
              << selfInput << " own inputs (reloads excepted: " << RELOADS.size() << ")\n";
    Check(reagents > 1000, "the dump has the reagents of the real recipes");
    Check(unknown == 0, "every reagent is a known item");
    Check(selfInput == 0, "no recipe is its own input (reloads excepted)");
}

// ---- WR2's table of crafts by class, against what each craft makes ----
// A product the class can use: gear it wears at the band's last level, a gem or a glyph it may use, ammunition for the
// classes with bows and guns, or an item item use has a record for (it knows how to use it:
// tests/fixtures/item-use-dump.txt; the demand rules need the bot's state, so the record stands for the demand answer)

using ProfessionRules::ClassBit;

constexpr std::uint8_t CLASSES[] = {ProfessionRules::WARRIOR, ProfessionRules::PALADIN, ProfessionRules::HUNTER,
                                    ProfessionRules::ROGUE,   ProfessionRules::PRIEST,  ProfessionRules::DEATH_KNIGHT,
                                    ProfessionRules::SHAMAN,  ProfessionRules::MAGE,    ProfessionRules::WARLOCK,
                                    ProfessionRules::DRUID};

std::string ClassName(std::uint8_t cls)
{
    char const* const names[] = {"",   "warrior", "paladin", "hunter",  "rogue", "priest",
                                 "dk", "shaman",  "mage",    "warlock", "",      "druid"};
    return cls < std::size(names) ? names[cls] : "class " + std::to_string(cls);
}

constexpr std::uint32_t Classes(std::initializer_list<std::uint8_t> list)
{
    std::uint32_t mask = 0;
    for (std::uint8_t const cls : list)
        mask |= ClassBit(cls);
    return mask;
}

// Weapon subclasses (item class 2) to the classes that can learn their skill
std::uint32_t WeaponClasses(std::uint32_t subClass)
{
    using namespace ProfessionRules;
    switch (subClass)
    {
        case 0:  // axe
            return Classes({WARRIOR, PALADIN, HUNTER, ROGUE, SHAMAN, DEATH_KNIGHT});
        case 1:  // two-hand axe
            return Classes({WARRIOR, PALADIN, HUNTER, SHAMAN, DEATH_KNIGHT});
        case 2:   // bow
        case 3:   // gun
        case 16:  // thrown
        case 18:  // crossbow
            return Classes({WARRIOR, HUNTER, ROGUE});
        case 4:  // mace
            return Classes({WARRIOR, PALADIN, ROGUE, PRIEST, SHAMAN, DRUID, DEATH_KNIGHT});
        case 5:  // two-hand mace
            return Classes({WARRIOR, PALADIN, SHAMAN, DRUID, DEATH_KNIGHT});
        case 6:  // polearm
            return Classes({WARRIOR, PALADIN, HUNTER, DRUID, DEATH_KNIGHT});
        case 7:  // sword
            return Classes({WARRIOR, PALADIN, HUNTER, ROGUE, MAGE, WARLOCK, DEATH_KNIGHT});
        case 8:  // two-hand sword
            return Classes({WARRIOR, PALADIN, HUNTER, DEATH_KNIGHT});
        case 10:  // staff
            return Classes({WARRIOR, HUNTER, PRIEST, SHAMAN, MAGE, WARLOCK, DRUID});
        case 13:  // fist
            return Classes({WARRIOR, HUNTER, ROGUE, SHAMAN, DRUID});
        case 15:  // dagger
            return Classes({WARRIOR, HUNTER, ROGUE, PRIEST, SHAMAN, MAGE, WARLOCK, DRUID});
        case 19:  // wand
            return Classes({PRIEST, MAGE, WARLOCK});
        case 20:  // fishing pole
            return ALL_CLASSES;
        default:
            return 0;
    }
}

// Level bands of ten up to the cap 80; a bot of the band has skill level x 5 (WR2's grant), so the band's recipes are
// those learned at 5 x its first level up to 5 x its last level + 4 (the last band up to 450)
struct Band
{
    std::uint32_t first;
    std::uint32_t last;
};
constexpr Band BANDS[] = {{1, 9}, {10, 19}, {20, 29}, {30, 39}, {40, 49}, {50, 59}, {60, 69}, {70, 80}};
constexpr std::uint32_t DEATH_KNIGHT_START = 55;  // a death knight has no band below its start level

// The level a bot of the band counts what it will wear or use soon (WR.2): the band's first level plus the look-ahead
// (ProfessionRules::SoonLevels, the same function the skill path uses): a level-1 bot counts what it can wear at 16
std::uint32_t Reach(Band const& band) { return band.first + ProfessionRules::SoonLevels(band.first); }

// Armour (item class 4) a class wears in a band: its own armour type (hunters and shamans leather, mail from 40;
// warriors and paladins mail, plate from 40; both in the band 40-49, where they change over), shields for the shield
// classes, its relic; every class wears cloaks, rings, necks, trinkets and items held in the off hand. Shirts and
// tabards are dress, not gear: they count for nobody
bool WearsArmor(ItemFact const& item, std::uint8_t cls, Band const& band)
{
    using namespace ProfessionRules;
    std::uint32_t const bit = ClassBit(cls);
    bool const before = band.first < 50;  // the lighter type, up to the change-over band
    bool const after = band.last >= 40;   // the heavier type, from the change-over band
    if (item.inventoryType == 4 || item.inventoryType == 19)
        return false;
    switch (item.subClass)
    {
        case 0:  // neck, finger, trinket, held in the off hand
            return item.inventoryType == 2 || item.inventoryType == 11 || item.inventoryType == 12 ||
                   item.inventoryType == 23;
        case 1:  // cloth: cloaks for everybody, the rest for the cloth classes
            return item.inventoryType == 16 || (bit & Classes({PRIEST, MAGE, WARLOCK})) != 0;
        case 2:  // leather
            return (bit & Classes({ROGUE, DRUID})) != 0 || ((bit & Classes({HUNTER, SHAMAN})) != 0 && before);
        case 3:  // mail
            return ((bit & Classes({WARRIOR, PALADIN})) != 0 && before) ||
                   ((bit & Classes({HUNTER, SHAMAN})) != 0 && after);
        case 4:  // plate
            return cls == DEATH_KNIGHT || ((bit & Classes({WARRIOR, PALADIN})) != 0 && after);
        case 6:  // shield
            return (bit & Classes({WARRIOR, PALADIN, SHAMAN})) != 0;
        case 7:
            return cls == PALADIN;  // libram
        case 8:
            return cls == DRUID;  // idol
        case 9:
            return cls == SHAMAN;  // totem
        case 10:
            return cls == DEATH_KNIGHT;  // sigil
        default:
            return false;
    }
}

// An enchanter's enchant ("Enchant <slot> - <effect>") counts for a class that wears or wields its slot: bracers,
// chest, cloak, boots, gloves and weapons every class; rings every class (an enchanter enchants its own); shields the
// shield classes; two-hand weapons every class with one; staves the staff classes. Another slot word, and the other
// crafts' own add-ons (embroidery, fur lining, sockets, attachments, master's inscriptions: no "Enchant " name), do not
// count: which class wants them depends on their stat, which the index does not carry
bool EnchantFits(std::string const& name, std::uint8_t cls)
{
    using namespace ProfessionRules;
    std::string const prefix = "Enchant ";
    if (name.rfind(prefix, 0) != 0)
        return false;
    std::size_t const dash = name.find(" - ");
    std::string const slot = name.substr(prefix.size(), dash == std::string::npos ? dash : dash - prefix.size());
    std::uint32_t const bit = ClassBit(cls);
    if (slot == "Bracer" || slot == "Bracers" || slot == "Chest" || slot == "Cloak" || slot == "Boots" ||
        slot == "Gloves" || slot == "Ring" || slot == "Weapon")
        return true;
    if (slot == "Shield")
        return (bit & Classes({WARRIOR, PALADIN, SHAMAN})) != 0;
    if (slot == "2H Weapon")
        return ((WeaponClasses(1) | WeaponClasses(5) | WeaponClasses(6) | WeaponClasses(8) | WeaponClasses(10)) &
                bit) != 0;
    if (slot == "Staff")
        return (WeaponClasses(10) & bit) != 0;
    return false;
}

// An item's stat groups by the stat types of its profile (ItemModType): strength, agility, intellect (with mana),
// spirit, the physical ratings and attack power, the caster ratings and spell power (with mana regeneration), the tank
// ratings. Stamina, health, the general hit, crit and haste ratings and resilience serve every class: they define
// nothing
constexpr std::uint64_t StatBits(std::initializer_list<std::uint32_t> types)
{
    std::uint64_t bits = 0;
    for (std::uint32_t const type : types)
        bits |= std::uint64_t{1} << type;
    return bits;
}
constexpr std::uint64_t STAT_STRENGTH = StatBits({4});
constexpr std::uint64_t STAT_AGILITY = StatBits({3});
constexpr std::uint64_t STAT_INTELLECT = StatBits({0, 5});
constexpr std::uint64_t STAT_SPIRIT = StatBits({6});
constexpr std::uint64_t STAT_PHYSICAL = StatBits({16, 17, 19, 20, 28, 29, 37, 38, 39, 40, 44});
constexpr std::uint64_t STAT_CASTER = StatBits({18, 21, 30, 41, 42, 43, 45, 47});
constexpr std::uint64_t STAT_TANK = StatBits({12, 13, 14, 15, 48});
constexpr std::uint64_t STAT_DEFINING =
    STAT_STRENGTH | STAT_AGILITY | STAT_INTELLECT | STAT_SPIRIT | STAT_PHYSICAL | STAT_CASTER | STAT_TANK;

// The stat groups a class plays with in some role: warriors and death knights strength, physical and tank stats
// (warriors agility too); paladins strength, intellect, physical, caster and tank stats; hunters agility, intellect and
// physical stats; rogues strength, agility and physical stats; priests, mages and warlocks intellect, spirit and caster
// stats; shamans everything but spirit and tank stats; druids everything
std::uint64_t StatsOf(std::uint8_t cls)
{
    using namespace ProfessionRules;
    switch (cls)
    {
        case WARRIOR:
            return STAT_STRENGTH | STAT_AGILITY | STAT_PHYSICAL | STAT_TANK;
        case DEATH_KNIGHT:
            return STAT_STRENGTH | STAT_PHYSICAL | STAT_TANK;
        case PALADIN:
            return STAT_STRENGTH | STAT_INTELLECT | STAT_PHYSICAL | STAT_CASTER | STAT_TANK;
        case HUNTER:
            return STAT_AGILITY | STAT_INTELLECT | STAT_PHYSICAL;
        case ROGUE:
            return STAT_STRENGTH | STAT_AGILITY | STAT_PHYSICAL;
        case SHAMAN:
            return STAT_STRENGTH | STAT_AGILITY | STAT_INTELLECT | STAT_PHYSICAL | STAT_CASTER;
        case DRUID:
            return STAT_DEFINING;
        default:
            return STAT_INTELLECT | STAT_SPIRIT | STAT_CASTER;
    }
}

// Gear suits a class by its stats: an item without a defining stat suits whoever wears it, a statted one only a class
// that plays with one of its defining stats (a mage does not take a strength sword, a warrior no intellect cloak)
bool StatsSuit(ItemFact const& item, std::uint8_t cls)
{
    std::uint64_t const defining = item.stats & STAT_DEFINING;
    return defining == 0 || (defining & StatsOf(cls)) != 0;
}

// Item use's consumed records from its dump, the first of each item (as tests/item-demand-selftest.cpp reads them)
using Uses = std::map<std::uint32_t, ItemUse::Record>;

std::vector<ItemUse::Role> RolesOf(std::uint8_t cls)
{
    using ItemUse::Role;
    using namespace ProfessionRules;
    switch (cls)
    {
        case WARRIOR:
        case DEATH_KNIGHT:
            return {Role::Tank, Role::Melee};
        case PALADIN:
            return {Role::Tank, Role::Healer, Role::Melee};
        case HUNTER:
            return {Role::Ranged};
        case ROGUE:
            return {Role::Melee};
        case PRIEST:
            return {Role::Healer, Role::Caster};
        case SHAMAN:
            return {Role::Healer, Role::Melee, Role::Caster};
        case DRUID:
            return {Role::Tank, Role::Healer, Role::Melee, Role::Caster};
        default:
            return {Role::Caster};
    }
}

// Item use's demand answer (ItemDemandRules.h, pure) for a bot of the class at the band's last level that holds one
// unit and the craft at level x 5: the item is kept (use or keep) in some role the class plays. Coatings count only
// where the coat picker picks them, which needs the enchantment's stats; the dumps carry none, so stones and oils do
// not count here
bool Wanted(Uses const& uses, std::uint32_t entry, std::uint8_t cls, std::uint32_t level, std::uint16_t craft)
{
    auto const it = uses.find(entry);
    if (it == uses.end())
        return false;
    ItemUse::Record const& record = it->second;
    ItemUse::Classified const classified = ItemUse::Classify(record);
    ItemUse::UsableEntry held;
    held.record = 0;
    held.item.entry = record.itemEntry;
    held.kind = classified.kind;
    held.sub = classified.sub;
    held.rank = classified.rank;
    held.count = 1;
    held.consumed = record.consumed;
    held.potion = record.potion;
    held.replaceCopper = record.replaceCopper;
    held.ready = true;
    held.shape = record.shape;
    held.radiusYards = record.radiusYards;
    held.rangeYards = record.rangeYards;
    held.minRangeYards = record.minRangeYards;
    held.rangeFriendYards = record.rangeFriendYards;
    std::uint8_t const canUse = 1;
    std::uint8_t const offered = 0;
    ItemUse::DemandSkill const skill{craft, static_cast<std::uint16_t>(std::min<std::uint32_t>(5 * level, 450))};
    for (ItemUse::Role const role : RolesOf(cls))
    {
        ItemUse::DemandInput input;
        input.cls = cls;
        input.race = 1;
        input.level = static_cast<std::uint8_t>(level);
        input.role = role;
        input.classBit = ClassBit(cls);
        input.raceBit = 1u;
        input.usesMana =
            cls != ProfessionRules::WARRIOR && cls != ProfessionRules::ROGUE && cls != ProfessionRules::DEATH_KNIGHT;
        input.skills = std::span<ItemUse::DemandSkill const>(&skill, 1);
        input.earnCopperPerMinute = 1000;
        input.deathCopper = ItemUse::DeathCopper(1000, 20000);
        input.persistent = true;
        input.list = std::span<ItemUse::UsableEntry const>(&held, 1);
        input.records = std::span<ItemUse::Record const>(&record, 1);
        input.canUse = std::span<std::uint8_t const>(&canUse, 1);
        input.offered = std::span<std::uint8_t const>(&offered, 1);
        if (ItemUse::VerdictOf(ItemUse::Demand(input), entry) <= ItemUse::DemandVerdict::Keep)
            return true;
    }
    return false;
}

// A product the class can use in the band (the class check of WR3, PLAN 3.2 and WR.2): gear it wears or wields within
// the look-ahead of the band's first level (Reach), with stats that suit it (StatsSuit); an enchant on a slot it has
// (EnchantFits); gems from level 60, when every class's gear has sockets (the gem's stat is not in the index); glyphs
// of its own class; ammunition and quivers for the bow and gun users; anything else only when item use's demand answer
// wants it for the class (Wanted). Bags, trade goods, devices and mounts count for nobody: every class could use them,
// so they say nothing about whether a craft serves a class
bool Usable(RecipeIndex const& index, Recipe const& recipe, Uses const& uses, std::uint8_t cls, Band const& band)
{
    if (recipe.classMask && !(recipe.classMask & ClassBit(cls)))
        return false;
    if (recipe.effect == Effect::Enchant)
        return EnchantFits(recipe.name, cls);
    if (recipe.effect != Effect::Make)
        return false;
    ItemFact const* item = index.Item(recipe.product);
    if (!item || !item->known || item->requiredLevel > Reach(band))
        return false;
    if (item->allowableClass && item->allowableClass != 0xFFFFFFFFu && !(item->allowableClass & ClassBit(cls)))
        return false;
    switch (item->itemClass)
    {
        case 2:
            return (WeaponClasses(item->subClass) & ClassBit(cls)) != 0 && StatsSuit(*item, cls);
        case 4:
            return WearsArmor(*item, cls, band) && StatsSuit(*item, cls);
        case 3:  // gems
            return Reach(band) >= 60;
        case 16:  // glyphs: only a glyph that names its classes (the allowable class test above)
            return item->allowableClass &&
                   (item->allowableClass & ProfessionRules::ALL_CLASSES) != ProfessionRules::ALL_CLASSES;
        case 6:   // ammunition
        case 11:  // quivers and ammunition pouches
            return (WeaponClasses(2) & ClassBit(cls)) != 0;
        default:
            return Wanted(uses, item->id, cls, band.last, recipe.skill);
    }
}

// The skill a recipe is learned at: the trainer's rank, else the ability's orange, else its yellow (abilities learned
// from a recipe item mostly keep orange 1 in the data)
std::uint32_t LearnRank(Recipe const& recipe)
{
    std::uint32_t const rank = recipe.trainers ? recipe.trainerRank : recipe.minRank;
    return rank <= 1 && recipe.yellow > 1 ? recipe.yellow : rank;
}

using CraftClass = std::set<std::pair<std::uint16_t, std::uint8_t>>;

// The craft-class pairs with a band where the class has nothing to use: of WR2's table (OpenPairs), or, as the
// negative control, every craft for every class
CraftClass FailingPairs(RecipeIndex const& index, Uses const& uses, bool everyCraft, std::size_t& checked)
{
    std::map<std::uint16_t, std::vector<Recipe const*>> bySkill;
    for (Recipe const& recipe : index.Recipes())
        bySkill[recipe.skill].push_back(&recipe);
    CraftClass failing;
    checked = 0;
    for (std::uint8_t const cls : CLASSES)
    {
        std::set<ProfessionRules::Skill> crafts;
        if (everyCraft)
            for (ProfessionRules::Craft const& craft : ProfessionRules::CRAFTS)
                crafts.insert(craft.Which);
        else
            for (ProfessionRules::Pair const& pair : ProfessionRules::OpenPairs(cls))
                for (ProfessionRules::Skill const skill : {pair.First, pair.Second})
                    if (ProfessionRules::Is(skill, ProfessionRules::Kind::Craft))
                        crafts.insert(skill);  // a gatherer makes nothing: it earns
        for (ProfessionRules::Skill const craft : crafts)
        {
            std::uint16_t const skill = static_cast<std::uint16_t>(craft);
            std::string empty;
            for (Band const& band : BANDS)
            {
                if (cls == ProfessionRules::DEATH_KNIGHT && band.last < DEATH_KNIGHT_START)
                    continue;
                std::uint32_t const from = band.first == 1 ? 1 : 5 * band.first;
                std::uint32_t const to = band.last >= 80 ? 450 : 5 * band.last + 4;
                bool found = false;
                for (Recipe const* recipe : bySkill[skill])
                    if (!found && LearnRank(*recipe) >= from && LearnRank(*recipe) <= to)
                        found = Usable(index, *recipe, uses, cls, band);
                if (!found)
                    empty += " " + std::to_string(band.first) + "-" + std::to_string(band.last);
            }
            ++checked;
            if (empty.empty())
                continue;
            failing.insert({skill, cls});
            char const* const label = everyCraft ? "control" : "FAILING PAIR";
            std::cout << label << ": " << ProfessionRules::Name(craft) << " for " << ClassName(cls)
                      << ": nothing the class can use at levels" << empty << '\n';
        }
    }
    return failing;
}

void CheckCraftsByClass(RecipeIndex const& index, Uses const& uses)
{
    std::size_t checked = 0;
    CraftClass const failing = FailingPairs(index, uses, false, checked);
    std::cout << "crafts by class: " << checked << " craft-class pairs of WR2's table, " << failing.size()
              << " with an empty band\n";
    Check(checked >= 50, "every class has its crafts checked");
    Check(failing.empty(), "WR2's crafts by class: no pair of the table has an empty band (no known exception left)");

    // Negative control (the grouped review's probe): every craft for every class must fail where the class has no
    // use, so the check can fail on a table that gives a class a craft it does not serve
    std::size_t controlChecked = 0;
    CraftClass const control = FailingPairs(index, uses, true, controlChecked);
    std::cout << "negative control: " << controlChecked << " craft-class pairs (every craft for every class), "
              << control.size() << " with an empty band\n";
    std::uint16_t const blacksmithing = static_cast<std::uint16_t>(ProfessionRules::Skill::Blacksmithing);
    std::uint16_t const tailoring = static_cast<std::uint16_t>(ProfessionRules::Skill::Tailoring);
    Check(control.count({blacksmithing, ProfessionRules::MAGE}) && control.count({tailoring, ProfessionRules::WARRIOR}),
          "negative control: blacksmithing for a mage and tailoring for a warrior fail");
    Check(control.size() > failing.size(), "negative control: every craft for every class fails more pairs");
}

void CheckDumpedFixture(std::string const& root)
{
    std::string text;
    std::string error;
    RecipeIndex index;
    Check(ReadFile(root + "/tests/fixtures/recipe-index-dump.txt", text), "the dumped fixture is readable");
    Check(RecipeIndex::Parse(text, index, error), "the dumped fixture parses: " + error);
    std::string const again = index.Dump("x", "y");
    if (text.rfind("# recipe-index-dump v6", 0) == 0)
    {
        Check(again.substr(again.find('\n')) == text.substr(text.find('\n')),
              "the dumped fixture reads back unchanged");
        // WR20a: the live drop rows have cloth, leather and meat
        Check(!index.DropsOf(LINEN).empty() && !index.DropsOf(LEATHER).empty() && !index.DropsOf(MEAT).empty(),
              "the fixture's drop rows give linen, light leather and boar meat");
        std::size_t specialised = 0;
        for (Recipe const& recipe : index.Recipes())
            specialised += !recipe.trainerAbilities.empty();
        Check(specialised > 0, "the fixture's trainer rows ask for specialisations");
        std::cout << "dumped fixture: " << index.Drops().size() << " drop rows, " << specialised
                  << " recipes behind a trainer ability\n";
    }
    else
    {
        RecipeIndex reread;
        Check(RecipeIndex::Parse(again, reread, error) && reread.Records() == index.Records(),
              "an older fixture reads back as v6 with the same records");
    }
    std::cout << "dumped fixture: " << text.substr(0, text.find('\n')) << '\n';
    for (ProfessionRules::ProfessionInfo const& profession : ProfessionRules::PROFESSIONS)
        std::cout << index.Summary(static_cast<std::uint16_t>(profession.Which), std::string(profession.Name)) << '\n';

    Uses uses;
    std::string useText;
    Check(ReadFile(root + "/tests/fixtures/item-use-dump.txt", useText), "the item use fixture is readable");
    std::istringstream lines(useText);
    std::string line;
    while (std::getline(lines, line))
        if (std::optional<ItemUse::DumpRow> const row = ItemUse::ParseDumpLine(line))
            if (row->record.source == ItemUse::Source::ItemSpell && row->record.consumed && row->record.itemEntry)
                uses.emplace(row->record.itemEntry, row->record);
    Check(uses.size() > 500, "item use's consumed records are read");

    CheckLadders(index);
    CheckReagents(index);
    CheckCraftsByClass(index, uses);
}
}  // namespace

int main(int argc, char** argv)
{
    std::cout << "RECIPES selftest (RecipeIndex.h)\n";
    RecipeIndex const index = RecipeIndex::Build(HandGather());
    std::cout << index.Summary(ALCHEMY, "alchemy") << '\n' << index.Summary(BLACKSMITHING, "blacksmithing") << '\n';
    Check(index.Recipes().size() == 10, "ten abilities become ten recipes");
    CheckThresholds(index);
    CheckLearned(index);
    CheckLadder(index);
    CheckMadeBy(index);
    CheckLoot();
    CheckDump(index);
    CheckAbilities(index);
    CheckDrops(index);
    if (argc > 1)
        CheckDumpedFixture(argv[1]);
    else
        Check(false, "the repo root is the first argument (tests/selftests.json passes it)");
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
