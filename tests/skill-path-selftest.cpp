/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the skill path (src/Ai/World/Rpg/SkillPathRules.h, professions plan WR10, PLAN 3.8): the
// engine's chances of a skill point, a hand fixture per invariant, a seeded search over random recipe tables, bags,
// skills, gold and zone risks that counts the violations of each of the ten invariants, a negative control per
// invariant (a deliberate fault that must find a counterexample), and on the dumped fixture
// (tests/fixtures/recipe-index-dump.txt) the decisions of sample bots and each profession's cheapest path from 1 to
// 300. The tables go to a file (argv[2], else skill-path-printed.md in the temp folder); the console gets summaries.
// WR29: the economy reference's skill stretch as fixed values (economy-cases.h), the search at stock rates and at ours
// with the fourteenth invariant (higher rates never make a stretch dearer); argv[3] "ours" prints the fixture's
// decisions and paths at our rates (the skill-gain step and chances; the yields stay those the fixture was dumped at).
// Auction house WA7b: the crafts for sale, the last claim (a hand case, and invariants 15-17 of the search under a
// market for each random world's items).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ItemWorthRules.h"
#include "MaterialRules.h"
#include "ProfessionRules.h"
#include "RecipeIndex.h"
#include "SkillPathRules.h"
#include "SkillPathTypes.h"
#include "economy-cases.h"

using namespace SkillPath;
using Recipes::DropKind;
using Recipes::DropRow;
using Recipes::ItemCount;
using Recipes::ItemFact;
using Recipes::NodeRow;
using Recipes::Process;
using Recipes::Recipe;
using Recipes::RecipeIndex;
using Recipes::RecipeSource;
using Recipes::Rung;
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
bool NotAbove(double a, double b) { return a <= b + 1e-6 * std::max(1.0, std::abs(b)); }

std::uint16_t Id(Skill skill) { return static_cast<std::uint16_t>(skill); }

constexpr std::uint16_t SMITH = static_cast<std::uint16_t>(Skill::Blacksmithing);
constexpr std::uint16_t MINER = static_cast<std::uint16_t>(Skill::Mining);
constexpr std::uint32_t SEED = 20261120;
constexpr int SEARCH_WORLDS = 1200;
constexpr int FAULT_WORLDS = 400;
constexpr int INVARIANTS = 17;

// ---- a world: the records of an index and the facts the path asks for ----
struct World
{
    std::vector<Recipe> recipes;
    std::vector<Rung> rungs;
    std::vector<Process> processes;
    std::vector<ItemFact> items;
    std::vector<VendorRow> vendors;
    std::vector<NodeRow> nodes;
    std::map<std::uint32_t, double> sell;
    std::set<std::uint32_t> locked;
    std::vector<Skinnable> skinnables;
    std::vector<Water> waters;
    std::vector<ToolSource> tools;
    Recipes::ServerRates rates;  // the server's (WR29); default: stock

    [[nodiscard]] RecipeIndex Index() const
    {
        RecipeIndex index = RecipeIndex::FromRecords(recipes, rungs, processes, items, vendors, nodes);
        index.SetRates(rates);
        return index;
    }
};

PathWorld WorldOf(RecipeIndex const& index, World const& world, Fault fault = Fault::None)
{
    auto const mayLearn = [&world](std::uint32_t spell) { return !world.locked.count(spell); };
    auto const sellCopper = [&world](std::uint32_t item)
    {
        auto const found = world.sell.find(item);
        return found == world.sell.end() ? 0.0 : found->second;
    };
    return PathWorld{index, mayLearn, sellCopper, world.skinnables, world.waters, fault, world.tools, {}, {}};
}

// A market for a world (WA7b): three of four items priced from a hash of the item (nothing drawn from the search's
// generator), at one to five times its sell price plus up to 40 copper, sold steadily
Market::View SaleMarket(World const& world)
{
    auto snapshot = std::make_shared<AuctionIndex::Snapshot>();
    for (ItemFact const& fact : world.items)
    {
        std::uint32_t const hash = fact.id * 2654435761u;
        if ((hash >> 28) % 4 == 0)
            continue;
        auto const sell = world.sell.find(fact.id);
        double const copper = sell == world.sell.end() ? 0.0 : sell->second;
        AuctionIndex::TypicalPrice& typical = snapshot->items[fact.id].typical;
        typical.unitPrice = static_cast<std::uint32_t>(copper) * (1 + (hash >> 24) % 5) + (hash >> 16) % 40 + 1;
        typical.sample = 10;
        typical.weight = 10.0;
    }
    return {snapshot, {}, {}, {}};
}

Recipe MakeRecipe(std::uint16_t skill, std::uint32_t spell, std::uint32_t product, std::vector<ItemCount> reagents,
                  std::uint32_t yellow, std::uint32_t grey, std::uint32_t castMs)
{
    Recipe recipe;
    recipe.skill = skill;
    recipe.spell = spell;
    recipe.name = "recipe " + std::to_string(spell);
    recipe.minRank = 1;
    recipe.yellow = yellow;
    recipe.grey = grey;
    recipe.acquire = Recipes::ACQUIRE_ON_SKILL_VALUE;
    recipe.effect = Recipes::Effect::Make;
    recipe.product = product;
    recipe.productCount = 1;
    recipe.castMs = castMs;
    recipe.reagents = std::move(reagents);
    return recipe;
}

void Trainer(Recipe& recipe, std::uint32_t rank, std::uint32_t copper, std::uint32_t level = 0)
{
    recipe.acquire = 0;
    recipe.trainers = 1;
    recipe.trainerRank = rank;
    recipe.trainerCopper = copper;
    recipe.trainerLevel = level;
}

std::vector<Rung> Ladder(std::uint16_t skill, std::uint32_t firstSpell)
{
    std::vector<Rung> ladder;
    std::uint32_t const caps[] = {75, 150, 225, 300};
    std::uint32_t const copper[] = {10, 500, 5000, 50000};
    std::uint32_t const levels[] = {0, 10, 20, 35};
    for (std::size_t i = 0; i < std::size(caps); ++i)
    {
        Rung rung;
        rung.skill = skill;
        rung.spell = firstSpell + static_cast<std::uint32_t>(i);
        rung.name = "rank " + std::to_string(i + 1);
        rung.cap = caps[i];
        rung.step = static_cast<std::uint32_t>(i + 1);
        rung.trainers = 1;
        rung.copper = copper[i];
        rung.reqSkill = i ? skill : 0;
        rung.reqRank = i ? caps[i - 1] - 25 : 0;
        rung.reqLevel = levels[i];
        ladder.push_back(rung);
    }
    return ladder;
}

ItemFact Fact(std::uint32_t id)
{
    ItemFact fact;
    fact.id = id;
    fact.known = true;
    fact.name = "item " + std::to_string(id);
    return fact;
}

// ---- the hand world ----
constexpr std::uint32_t ORE = 2770;
constexpr std::uint32_t BAR = 2840;
constexpr std::uint32_t STONE = 2835;
constexpr std::uint32_t THREAD = 2320;
constexpr std::uint32_t HIDE = 9990;
constexpr std::uint32_t SHARP = 2862;
constexpr std::uint32_t BRACERS = 2853;
constexpr std::uint32_t AXE = 3488;
constexpr std::uint32_t BELT = 9991;
constexpr std::uint32_t HELM = 9992;
constexpr std::uint32_t SMELT = 2657;
constexpr std::uint32_t R_SHARP = 2660;
constexpr std::uint32_t R_BRACERS = 2663;
constexpr std::uint32_t R_AXE = 3293;
constexpr std::uint32_t R_BELT = 9001;
constexpr std::uint32_t R_HELM = 9002;
constexpr std::uint32_t SAFE_ZONE = 12;

World HandWorld()
{
    World world;
    world.recipes = {
        MakeRecipe(MINER, SMELT, BAR, {{ORE, 1}}, 25, 70, 1500),
        MakeRecipe(SMITH, R_SHARP, SHARP, {{STONE, 1}}, 15, 55, 1500),
        MakeRecipe(SMITH, R_BRACERS, BRACERS, {{BAR, 2}}, 20, 60, 2000),
        MakeRecipe(SMITH, R_AXE, AXE, {{BAR, 2}, {THREAD, 1}}, 75, 115, 3000),
        MakeRecipe(SMITH, R_BELT, BELT, {{HIDE, 1}}, 40, 80, 1500),
        MakeRecipe(SMITH, R_HELM, HELM, {{THREAD, 1}}, 90, 130, 1500),
    };
    Trainer(world.recipes[3], 35, 250);
    Trainer(world.recipes[5], 50, 100);
    world.rungs = Ladder(SMITH, 2018);
    for (Rung const& rung : Ladder(MINER, 2575))
        world.rungs.push_back(rung);
    for (std::uint32_t const id : {ORE, BAR, STONE, THREAD, HIDE, SHARP, BRACERS, AXE, BELT, HELM})
        world.items.push_back(Fact(id));
    world.vendors = {{THREAD, 50, 5, 3, 0}, {STONE, 5, 1, 1, 0}};
    world.nodes = {{SAFE_ZONE, MINER, 1, 1731, 40, 60.0f, ORE, 2.0},
                   {SAFE_ZONE, MINER, 1, 1731, 40, 60.0f, STONE, 1.0}};
    return world;
}

PathBot HandBot()
{
    PathBot bot;
    bot.level = 20;
    bot.cls = ProfessionRules::WARRIOR;
    bot.money = 100000;
    bot.rates.exchange = 1.0f;
    bot.rates.factor = 1.0f;
    bot.rates.minuteValue = 100.0f;
    bot.skills = {{SMITH, 1, 75}, {MINER, 100, 150}};
    return bot;
}

Stretch const* Find(Path const& path, std::uint16_t skill, std::uint32_t to)
{
    for (Stretch const& stretch : path.weighed)
        if (stretch.skill == skill && stretch.to == to)
            return &stretch;
    return nullptr;
}

std::string Describe(Path const& path)
{
    std::ostringstream out;
    out << std::setprecision(9) << path.chosen << '/' << static_cast<int>(path.reason) << ':';
    for (Stretch const& stretch : path.weighed)
    {
        out << '[' << stretch.skill << ' ' << stretch.from << '-' << stretch.to << ' '
            << static_cast<int>(stretch.reason) << ' ' << stretch.netWorth << ' ' << stretch.copper;
        for (PathStep const& step : stretch.steps)
            out << ' ' << static_cast<int>(step.kind) << ':' << step.id << ':' << step.from << '-' << step.to;
        out << ']';
    }
    for (Purchase const& need : path.needs)
        out << " n" << need.spell;
    for (Want const& want : path.wants)
        out << " w" << want.item << 'x' << want.count;
    for (Craft const& craft : path.queue)
        out << " q" << craft.spell << 'x' << craft.casts;
    return out.str();
}

// The craft errand's worth (WR14): the chosen stretch's net worth by the share of its craft casts the queue makes
void CheckQueueWorth()
{
    Path path;
    Stretch stretch;
    stretch.netWorth = 400.0;
    PathStep craft;
    craft.kind = StepKind::Craft;
    craft.casts = 8.0;
    PathStep learn;
    learn.kind = StepKind::Learn;
    learn.casts = 50.0;
    stretch.steps = {craft, learn};
    path.weighed = {stretch};
    Check(QueueWorth(path) == 0.0, "queue worth: nothing without a chosen stretch");
    path.chosen = 0;
    Check(QueueWorth(path) == 0.0, "queue worth: nothing without a queue");
    path.queue = {{1, 2, true}};
    Check(QueueWorth(path) == 100.0, "queue worth: 2 of the stretch's 8 craft casts bring a quarter of its worth");
    path.queue = {{1, 6, true}, {2, 6, true}};
    Check(QueueWorth(path) == 400.0, "queue worth: a queue past the stretch's casts brings its worth, no more");
    path.queue = {{51005, 6, true, 765}, {1, 2, true}};
    Check(QueueWorth(path) == 100.0, "queue worth: a processing cast is no craft cast");
    // WR15: the uses' crafts bring their own worth and are no skill casts
    path.queue = {{1, 6, false}, {2, 2, true}};
    path.useWorth = 50.0;
    Check(QueueWorth(path) == 150.0, "queue worth: the uses' worth plus the skill casts' share (a use cast is none)");
    path.weighed[0].netWorth = -5.0;
    Check(QueueWorth(path) == 50.0, "queue worth: a stretch without worth gives none, the uses still theirs");
    path.chosen = NONE;
    Check(QueueWorth(path) == 50.0, "queue worth: without a chosen stretch the uses' worth (supply before skill)");
}

// ---- WR15: cooking and first aid as supply (PLAN 3.11, WR.6) ----
constexpr std::uint16_t COOK = static_cast<std::uint16_t>(Skill::Cooking);
constexpr std::uint16_t FIRST_AID = static_cast<std::uint16_t>(Skill::FirstAid);
constexpr std::uint16_t TAILOR = static_cast<std::uint16_t>(Skill::Tailoring);
constexpr std::uint32_t MEAT = 769;
constexpr std::uint32_t WOLF_MEAT = 2672;
constexpr std::uint32_t ROAST = 2681;
constexpr std::uint32_t CHARRED = 2679;
constexpr std::uint32_t CLOTH = 2589;
constexpr std::uint32_t BANDAGE = 1251;
constexpr std::uint32_t SHIRT = 2576;
constexpr std::uint32_t R_ROAST = 2540;
constexpr std::uint32_t R_CHARRED = 2538;
constexpr std::uint32_t R_BANDAGE = 3275;
constexpr std::uint32_t R_SHIRT = 2392;
constexpr std::uint32_t FOOD_GROUP = 17;  // ItemUse::DemandSlot::Food + 1
constexpr std::uint32_t HEAL_GROUP = 1;   // ItemUse::DemandSlot::Heal + 1

World SupplyWorld()
{
    World world;
    world.recipes = {
        MakeRecipe(COOK, R_ROAST, ROAST, {{MEAT, 1}}, 45, 65, 1500),
        MakeRecipe(COOK, R_CHARRED, CHARRED, {{WOLF_MEAT, 1}}, 30, 70, 1500),
        MakeRecipe(FIRST_AID, R_BANDAGE, BANDAGE, {{CLOTH, 1}}, 30, 50, 3000),
        MakeRecipe(TAILOR, R_SHIRT, SHIRT, {{CLOTH, 1}}, 15, 55, 1500),
    };
    world.recipes[0].focus = 4;
    world.recipes[1].focus = 4;
    for (std::uint32_t const id : {MEAT, WOLF_MEAT, ROAST, CHARRED, CLOTH, BANDAGE, SHIRT})
        world.items.push_back(Fact(id));
    return world;
}

std::uint32_t QueuedCasts(Path const& path, std::uint32_t spell, bool forSkill)
{
    std::uint32_t casts = 0;
    for (Craft const& craft : path.queue)
        if (craft.spell == spell && craft.forSkill == forSkill)
            casts += craft.casts;
    return casts;
}

void CheckSupply()
{
    World const world = SupplyWorld();
    RecipeIndex const index = world.Index();
    PathWorld const paths = WorldOf(index, world);

    // A cook with no stretch (no profession weighed): its food is claimed from its meat, up to the food gap
    PathBot cook = HandBot();
    cook.skills.clear();
    cook.knownRecipes = {R_ROAST};
    cook.held = {{MEAT, 10}};
    cook.consumables = {{ROAST, 12, 0, 25.0f, FOOD_GROUP, true}};
    Path const fed = Weigh(cook, paths);
    Check(fed.chosen == NONE && QueuedCasts(fed, R_ROAST, false) == 10 && fed.foodUnits == 10,
          "supply: a cook without a chosen stretch cooks its ten meat for its food (" + Describe(fed) + ")");
    Check(Near(fed.useWorth, 250.0) && Near(QueueWorth(fed), 250.0),
          "supply: the food it cooks is worth what it saves, the queue's worth without a stretch");
    SkillPath::Material const* meat = MaterialOf(fed, MEAT);
    Check(meat && meat->used == 10, "supply: the claimed meat is a material the path uses (kept, not sold)");

    // The gap is shared by every food it makes: twelve of twenty, the lower item first
    PathBot both = cook;
    both.knownRecipes = {R_ROAST, R_CHARRED};
    both.held = {{MEAT, 10}, {WOLF_MEAT, 10}};
    both.consumables = {{ROAST, 12, 0, 25.0f, FOOD_GROUP, true}, {CHARRED, 12, 0, 25.0f, FOOD_GROUP, true}};
    Path const shared = Weigh(both, paths);
    Check(QueuedCasts(shared, R_CHARRED, false) == 10 && QueuedCasts(shared, R_ROAST, false) == 2 &&
              shared.foodUnits == 12,
          "supply: two foods share one gap of twelve (" + Describe(shared) + ")");

    // Supply before skill: the chosen stretch's casts take only the meat the food leaves
    PathBot skilled = cook;
    skilled.consumables = {{ROAST, 6, 0, 25.0f, FOOD_GROUP, true}};
    Path skillPath;
    Stretch stretch;
    stretch.skill = COOK;
    PathStep craft;
    craft.kind = StepKind::Craft;
    craft.id = R_ROAST;
    craft.from = 1;
    craft.to = 20;
    craft.casts = 20.0;
    stretch.steps.push_back(craft);
    skillPath.weighed.push_back(stretch);
    skillPath.chosen = 0;
    Claims const claims = Weigher(skilled, paths).Claim(skillPath);
    Check(claims.queue.size() == 2 && !claims.queue[0].forSkill && claims.queue[0].casts == 6 &&
              claims.queue[1].forSkill && claims.queue[1].casts == 4 && claims.foodUnits == 6,
          "supply: the food (six) before the skill casts (the four meat left)");

    // Bandages up to the heal slot's stock from the cloth held; a tailor without first aid claims none (WR.2)
    PathBot medic = HandBot();
    medic.skills.clear();
    medic.knownRecipes = {R_BANDAGE};
    medic.held = {{CLOTH, 10}};
    medic.consumables = {{BANDAGE, 6, 0, 40.0f, HEAL_GROUP, false}};
    Path const bandaged = Weigh(medic, paths);
    Check(QueuedCasts(bandaged, R_BANDAGE, false) == 6 && bandaged.foodUnits == 0,
          "supply: a first aider makes six bandages of its ten cloth (" + Describe(bandaged) + ")");
    PathBot tailor = medic;
    tailor.knownRecipes = {R_SHIRT};
    Path const tailored = Weigh(tailor, paths);
    Check(tailored.queue.empty() && !MaterialOf(tailored, CLOTH),
          "supply: a tailor without first aid has no bandage claim and keeps no cloth for one");
}

void CheckChances()
{
    Recipes::ServerRates const stock;
    Check(CraftChancePerMille(10, 60, 20, stock) == 1000, "a craft at or below yellow is orange (100 %)");
    Check(CraftChancePerMille(40, 60, 20, stock) == 500,
          "half way from yellow to grey is 50 % (the core's straight line)");
    Check(CraftChancePerMille(60, 60, 20, stock) == 0, "a craft at grey gives no point");
    Check(CraftChancePerMille(5, 10, 10, stock) == 1000 && CraftChancePerMille(10, 10, 10, stock) == 0,
          "equal thresholds keep the core's orange/grey boundary");
    Check(GatherChancePerMille(24, 0, MINER, stock) == 1000 && GatherChancePerMille(25, 0, MINER, stock) == 750 &&
              GatherChancePerMille(50, 0, MINER, stock) == 250 && GatherChancePerMille(100, 0, MINER, stock) == 0,
          "a node: orange below +25, 75 % to +50, 25 % to +100, then never");
    // the server's chances (WR29): SkillChance.* and the mining and skinning steps
    Recipes::ServerRates shifted;
    shifted.orange = 900;
    shifted.grey = 100;
    shifted.yellow = 600;
    shifted.green = 300;
    shifted.miningSteps = 75;
    Check(CraftChancePerMille(10, 60, 20, shifted) == 900 && CraftChancePerMille(40, 60, 20, shifted) == 500 &&
              CraftChancePerMille(60, 60, 20, shifted) == 100,
          "a craft's line runs from the server's orange to its grey");
    Check(GatherChancePerMille(30, 0, MINER, shifted) == 600 && GatherChancePerMille(80, 0, MINER, shifted) == 150 &&
              GatherChancePerMille(160, 100, MINER, shifted) == 75 &&
              GatherChancePerMille(80, 0, Id(Skill::Herbalism), shifted) == 300,
          "mining's chance halves every MiningSteps points; herbalism has no steps");
    Check(FishChancePerMille(1) == 1000 && FishChancePerMille(115) == 1000 && FishChancePerMille(116) == 500 &&
              FishChancePerMille(150) == 330 && FishChancePerMille(0) == 0,
          "fishing: one catch per point to 115, two to 135, three (33 %) to 160");
    Check(SkinRequirement(5) == 0 && SkinRequirement(15) == 50 && SkinRequirement(30) == 150,
          "skinning asks 0 below level 10, (level - 10) x 10 below 20, level x 5 above");
    Check(Near(Nearness(50, 10), 1.0) && Near(Nearness(125, 10), 0.0) && Near(Nearness(87, 10), 1.0 - 37.0 / 75.0),
          "nearness: full to level x 5, nothing one rank (75) above it");
    Check(Near(UseWeight(10, 10), 1.0) && UseWeight(10 + ProfessionRules::SoonLevels(10) + 1, 10) == 0.0 &&
              UseWeight(12, 10) < 1.0 && UseWeight(12, 10) > 0.0,
          "a use counts less the further its level lies within the look-ahead");
}

// ---- the economy reference's skill stretch (WR29) ----
// Linen Bandage (3275: yellow 30, grey 60, one Linen Cloth) from First Aid 47 to at least 51, the cloth at its
// template salvage of 13 c and a bandage at 8 c (the case's inputs), against craft_processing.skill_stretch's values
void CheckEconomySkill()
{
    using EconomyCases::At;
    constexpr std::uint16_t FIRST_AID = 129;
    constexpr std::uint32_t LINEN = 2589;
    constexpr std::uint32_t LINEN_BANDAGE = 1251;
    constexpr std::uint32_t R_LINEN_BANDAGE = 3275;
    constexpr double CLOTH_SALE = 13.0;
    constexpr double BANDAGE_SALE = 8.0;
    struct Climbed
    {
        Stretch stretch;
        std::uint32_t capped{0};  // the points of a skill-up at 74 of 75
    };
    auto const climb = [&](Recipes::ServerRates const& rates)
    {
        World world;
        world.rates = rates;
        world.recipes = {MakeRecipe(FIRST_AID, R_LINEN_BANDAGE, LINEN_BANDAGE, {{LINEN, 1}}, 30, 60, 3000)};
        world.items = {Fact(LINEN), Fact(LINEN_BANDAGE)};
        world.vendors = {{LINEN, static_cast<std::uint32_t>(CLOTH_SALE), 1, 1, 0}};
        world.sell[LINEN_BANDAGE] = BANDAGE_SALE;
        PathBot bot = HandBot();
        bot.rates.minuteValue = 0.0f;  // the case counts materials only
        bot.skills = {{FIRST_AID, 47, 75}};
        bot.knownRecipes = {R_LINEN_BANDAGE};
        RecipeIndex const index = world.Index();
        PathWorld const paths = WorldOf(index, world);
        Weigher const weigher(bot, paths);
        return Climbed{weigher.Climb(FIRST_AID, 47, 51), weigher.Points(true, 74, 75)};
    };
    std::vector<EconomyCases::Case> const cases = {
        {"skill_stretch",
         {{"47 integer roll threshold", "q47", At::Both, 433.0},
          {"49 integer roll threshold", "q49", At::Both, 366.0},
          {"Base expected casts at +1", "casts", At::Stock, 185492645.0 / 17591058.0},
          {"Configured expected casts and cloth units", "casts", At::Ours, 399500.0 / 79239.0},
          {"Expected casts with no gain, configured", "failed", At::Ours, 241022.0 / 79239.0},
          {"Expected cloth forgone at NPC, configured", "cloth", At::Ours, 5193500.0 / 79239.0},
          {"Expected sale of all resulting bandages", "bandages", At::Ours, 3196000.0 / 79239.0},
          {"Expected net material opportunity cost", "net", At::Ours, 1997500.0 / 79239.0},
          {"Configured finish in minimum two casts", "minimum", At::Ours, 79239.0 / 500000.0},
          {"Rank-cap truncates a +2 gain at 74 of 75", "capped", At::Ours, 1.0}},
         [&](Recipes::ServerRates const& rates)
         {
             Climbed const climbed = climb(rates);
             double casts = 0.0;
             std::uint32_t skillUps = 0;
             for (PathStep const& step : climbed.stretch.steps)
                 if (step.kind == StepKind::Craft)
                 {
                     casts += step.casts;
                     skillUps += (step.to - step.from + rates.craftStep - 1) / rates.craftStep;
                 }
             double const q47 = CraftChancePerMille(47, 60, 30, rates);
             double const q49 = CraftChancePerMille(49, 60, 30, rates);
             return std::map<std::string, double>{{"q47", q47},
                                                  {"q49", q49},
                                                  {"casts", casts},
                                                  {"failed", casts - skillUps},
                                                  {"cloth", casts * CLOTH_SALE},
                                                  {"bandages", casts * BANDAGE_SALE},
                                                  {"net", climbed.stretch.copper},
                                                  {"minimum", q47 * q49 / (PER_MILLE * PER_MILLE)},
                                                  {"capped", static_cast<double>(climbed.capped)}};
         }},
    };
    EconomyCases::Run(cases, Check);
    Climbed const ours = climb(EconomyCases::Ours());
    Check(ours.stretch.reason == Reason::Open && ours.stretch.steps.size() == 1 && ours.stretch.steps[0].from == 47 &&
              ours.stretch.steps[0].to == 51,
          "craft_processing.skill_stretch: at our rates the climb passes 47 and 49 (configured_path) to 51");
}

// ---- the hand fixture: one check per invariant ----
// WR.27: a trainer recipe behind an ability the bot lacks is not learnable, with it it is; the negative control
// (Fault::IgnoreAbility, a guard that ignores the abilities) must make it learnable without
void CheckAbilityGuard()
{
    constexpr std::uint32_t SPECIALIST = 20219;
    Recipe recipe;
    recipe.skill = SMITH;
    recipe.spell = 777001;
    recipe.name = "Specialist Bolt";
    recipe.minRank = 50;
    recipe.yellow = 60;
    recipe.grey = 70;
    recipe.effect = Recipes::Effect::Make;
    recipe.product = 777002;
    recipe.productCount = 1;
    Trainer(recipe, 50, 100);
    recipe.trainerAbilities = {SPECIALIST};
    RecipeIndex const index = RecipeIndex::FromRecords({recipe}, {}, {}, {});
    PathWorld const world{index, {}, {}, {}, {}, Fault::None, {}, {}, {}};
    PathWorld const ignoring{index, {}, {}, {}, {}, Fault::IgnoreAbility, {}, {}, {}};
    PathBot bot = HandBot();
    Check(!Weigher(bot, world).LearnOf(recipe).ok, "WR.27: a recipe behind an ability the bot lacks is not learnable");
    Check(Weigher(bot, ignoring).LearnOf(recipe).ok,
          "WR.27 negative control: a guard that ignores the ability makes it learnable");
    bot.knownRecipes = {SPECIALIST};
    Learning const learned = Weigher(bot, world).LearnOf(recipe);
    Check(learned.ok && learned.step && learned.fee == 100, "WR.27: with the ability the trainer teaches it");
}

// WR20a: drop rows in the path. A tailor whose cloth had no source was blocked (no_source); with a drop row its
// stretch is weighed and its path's cloth is dropped. A skinner's creatures come from the skin rows (SkinnablesOf), so
// it gets a stretch; a creature friendly to its side or far above it is none. A bot whose materials had sources keeps
// its path when drop rows come that are dearer or for other materials
std::string MaterialsOf(Path const& path)
{
    std::ostringstream out;
    for (Material const& material : path.materials)
        out << material.item << ':' << material.used << ':' << static_cast<int>(material.source) << ':' << material.via
            << ':' << material.copperEach << ' ';
    return out.str();
}

void CheckDropSources()
{
    constexpr std::uint16_t TAILOR = static_cast<std::uint16_t>(Skill::Tailoring);
    constexpr std::uint16_t SKINNER = static_cast<std::uint16_t>(Skill::Skinning);
    constexpr std::uint32_t LINEN = 2589;
    constexpr std::uint32_t BOLT = 2996;
    constexpr std::uint32_t R_BOLT = 2963;
    constexpr std::uint32_t SHIRT = 2576;
    constexpr std::uint32_t R_SHIRT = 3915;
    constexpr std::uint32_t LEATHER = 2318;
    constexpr std::uint32_t DROP_ZONE = 40;
    DropRow const linen{DROP_ZONE, DropKind::Corpse, 1, LINEN, 10, 40.0f, 8, 9, 0.0f, 0, 0, 0.5, 300};

    World tailoring;
    tailoring.recipes = {MakeRecipe(TAILOR, R_BOLT, BOLT, {{LINEN, 2}}, 25, 75, 1500),
                         MakeRecipe(TAILOR, R_SHIRT, SHIRT, {{BOLT, 1}}, 50, 90, 1500)};
    Trainer(tailoring.recipes[1], 40, 100);
    tailoring.rungs = Ladder(TAILOR, 3908);
    tailoring.items = {Fact(LINEN), Fact(BOLT), Fact(SHIRT)};
    PathBot tailor = HandBot();
    tailor.level = 10;
    tailor.side = Materials::SideBit(0);
    tailor.skills = {{TAILOR, 1, 75}};
    RecipeIndex const bare = tailoring.Index();
    Check(Weigher(tailor, WorldOf(bare, tailoring)).Climb(TAILOR, 1, 50).reason == Reason::NoSource,
          "WR20a: without a drop row the tailor's cloth has no source (no_source)");
    RecipeIndex dropped = tailoring.Index();
    dropped.SetDrops({linen}, {});
    Stretch const weighed = Weigher(tailor, WorldOf(dropped, tailoring)).Climb(TAILOR, 1, 50);
    Check(weighed.reason == Reason::Open && weighed.cost.materials > 0.0 && weighed.minutes > 0.0,
          "WR20a: with a drop row the tailor's stretch is weighed, its cloth at the kills' minutes");
    tailor.anyStretch = true;
    Path const tailorPath = Weigh(tailor, WorldOf(dropped, tailoring));
    Material const* cloth = MaterialOf(tailorPath, LINEN);
    Check(tailorPath.chosen != NONE && cloth && cloth->source == static_cast<std::uint8_t>(Materials::Kind::Dropped) &&
              cloth->via == DROP_ZONE,
          "WR20a: the tailor's path takes its cloth from the drop row's zone: " + Describe(tailorPath) + " | " +
              MaterialsOf(tailorPath));
    PathBot horde = tailor;
    horde.side = Materials::SideBit(1);
    RecipeIndex friendly = tailoring.Index();
    DropRow alliance = linen;
    alliance.friendlyTo = 2;  // friendly to the Horde
    friendly.SetDrops({alliance}, {});
    Check(Weigher(horde, WorldOf(friendly, tailoring)).Climb(TAILOR, 1, 50).reason == Reason::NoSource,
          "WR20a: a creature friendly to the tailor's side gives it no cloth");

    // a skinner
    World skinning;
    skinning.rungs = Ladder(SKINNER, 8613);
    skinning.items = {Fact(LEATHER)};
    skinning.sell[LEATHER] = 10.0;
    DropRow const low{DROP_ZONE, DropKind::Skin, 1, LEATHER, 10, 40.0f, 8, 9, 0.0f, 0, SKINNER, 1.0, 300};
    DropRow const mid{DROP_ZONE, DropKind::Skin, 2, LEATHER, 10, 40.0f, 14, 15, 0.0f, 0, SKINNER, 1.0, 300};
    DropRow const side{DROP_ZONE, DropKind::Skin, 3, LEATHER, 10, 40.0f, 12, 12, 0.0f, 1, SKINNER, 1.0, 300};
    DropRow const high{DROP_ZONE, DropKind::Skin, 4, LEATHER, 10, 40.0f, 30, 30, 0.0f, 0, SKINNER, 1.0, 300};
    RecipeIndex skins = skinning.Index();
    skins.SetDrops({low, mid, side, high}, {});
    Materials::Bot killer;
    killer.level = 15;
    killer.side = Materials::SideBit(0);
    auto const sell = [&skinning](std::uint32_t item) { return skinning.sell[item]; };
    std::vector<Skinnable> const met = SkinnablesOf(skins, killer, sell);
    Check(met.size() == 2 && met[0].level == 9 && met[1].level == 15 && met[0].zone == DROP_ZONE &&
              Near(met[0].copperPerSkin, 10.0) && Near(met[1].minutesPerKill, Materials::KillOf(mid, killer).minutes),
          "WR20a: a skinner meets the skin rows' creatures, not one friendly to its side nor one far above it");
    PathBot skinner = HandBot();
    skinner.level = 15;
    skinner.side = killer.side;
    skinner.skills = {{SKINNER, 1, 75}};
    Path const without = Weigh(skinner, WorldOf(skins, skinning));
    skinning.skinnables = met;
    Path const with = Weigh(skinner, WorldOf(skins, skinning));
    auto const skinStretch = [](Path const& path)
    {
        return std::any_of(path.weighed.begin(), path.weighed.end(), [](Stretch const& stretch)
                           { return stretch.skill == SKINNER && stretch.milestone == Milestone::Skinnable; });
    };
    Check(!skinStretch(without) && skinStretch(with),
          "WR20a: with its creatures from the skin rows a skinner gets a stretch");

    // a bot whose materials had sources: drop rows of another material and a dearer ore row change nothing
    World hand = HandWorld();
    RecipeIndex const plain = hand.Index();
    RecipeIndex withDrops = hand.Index();
    withDrops.SetDrops({linen, {SAFE_ZONE, DropKind::Corpse, 5, ORE, 3, 300.0f, 20, 20, 0.0f, 0, 0, 0.01, 300}}, {});
    PathBot smith = HandBot();
    smith.side = Materials::SideBit(0);
    smith.anyStretch = true;  // a chosen stretch, so the path has materials
    Path const before = Weigh(smith, WorldOf(plain, hand));
    Path const after = Weigh(smith, WorldOf(withDrops, hand));
    Check(Describe(before) == Describe(after) && MaterialsOf(before) == MaterialsOf(after) && !before.materials.empty(),
          "WR20a: nothing changes for a bot whose materials had sources: " + Describe(after) + " | " +
              MaterialsOf(before) + "| " + MaterialsOf(after));
}

void CheckHand()
{
    World const world = HandWorld();
    RecipeIndex const index = world.Index();
    PathWorld const paths = WorldOf(index, world);
    PathBot bot = HandBot();
    Weigher const weigher(bot, paths);

    // 6: the cheapest recipe at a skill; at 1 the stone (5 c bought + a 1.5 s cast at 100 a minute) beats the bracers
    Option first;
    Check(weigher.Best(SMITH, 1, first) && first.id == R_SHARP && Near(first.value, 5.0 + 2.5),
          "at 1 the sharpening stone is the cheapest point (7.5)");
    for (std::uint32_t at = 1; at < 140; ++at)
    {
        Option best;
        if (!weigher.Best(SMITH, at, best))
            continue;
        bool cheapest = true;
        for (Option const& option : weigher.Options(SMITH, at))
            cheapest = cheapest && NotAbove(best.value, option.value);
        Check(cheapest, "6: no cheaper option than the chosen one at " + std::to_string(at));
    }

    // 4 and 5: the belt's hide has no source, so it is never chosen; nothing is chosen at or past grey
    Stretch const climb = weigher.Climb(SMITH, 1, 74);
    bool sourced = true;
    bool coloured = true;
    for (PathStep const& step : climb.steps)
    {
        sourced = sourced && step.id != R_BELT;
        if (step.kind == StepKind::Craft)
            for (Recipe const& recipe : index.Recipes())
                if (recipe.spell == step.id)
                    coloured = coloured && step.to - 1 < recipe.grey;
    }
    Check(sourced, "5: a recipe whose material has no source is never chosen");
    Check(coloured, "4: no point is planned on a grey recipe");
    Check(climb.reason == Reason::Open && climb.to == 74, "the climb from 1 to 74 is open");

    // the rank at the cap: from 74 to 80 the climb buys Journeyman (500 c)
    Stretch const rank = weigher.Climb(SMITH, 70, 80);
    bool bought = false;
    for (PathStep const& step : rank.steps)
        bought = bought || (step.kind == StepKind::Rank && step.id == 2019 && step.from == 75);
    Check(bought, "a climb past the cap buys the next rank at the cap");

    // 9: a locked rank stops the climb; a locked recipe is never chosen
    World lockedWorld = HandWorld();
    lockedWorld.locked = {2019, R_AXE};
    RecipeIndex const lockedIndex = lockedWorld.Index();
    PathWorld const lockedPaths = WorldOf(lockedIndex, lockedWorld);
    Weigher const lockedWeigher(bot, lockedPaths);
    Check(lockedWeigher.Climb(SMITH, 70, 80).reason == Reason::Locked, "9: a locked rank stops the climb (Locked)");
    bool axeUsed = false;
    for (PathStep const& step : lockedWeigher.Climb(SMITH, 36, 74).steps)
        axeUsed = axeUsed || step.id == R_AXE;
    Check(!axeUsed, "9: a locked recipe is never chosen");

    // 1: the bracers as a use: worth it when the use is worth more than the stretch, not when it is worth little
    bot.uses = {{BRACERS, 0, 5000.0f, 20}};
    bot.skills = {{SMITH, 1, 75}, {MINER, 100, 150}};
    World useWorld = HandWorld();
    useWorld.recipes[2].acquire = 0;
    Trainer(useWorld.recipes[2], 10, 50);
    RecipeIndex const useIndex = useWorld.Index();
    PathWorld const usePaths = WorldOf(useIndex, useWorld);
    Path const worth = Weigh(bot, usePaths);
    Stretch const* toBracers = Find(worth, SMITH, 10);
    Check(toBracers && toBracers->milestone == Milestone::Recipe && toBracers->milestoneId == R_BRACERS,
          "the bracers' recipe at 10 is a milestone");
    Check(worth.chosen != NONE && worth.weighed[worth.chosen].netWorth > 0.0, "1: a stretch worth more is taken");
    bot.uses = {{BRACERS, 0, 1.0f, 20}};
    Path const little = Weigh(bot, usePaths);
    Check(little.chosen == NONE && little.reason == Reason::NotWorthIt, "1: a stretch worth less is not taken");

    // the test constraint anypath (WR14b): the same bot takes the fastest climb, worth a minute per point, and weighs
    // at least the stretches it weighs without the constraint
    PathBot anyBot = bot;
    anyBot.anyStretch = true;
    Path const any = Weigh(anyBot, usePaths);
    bool fastest = any.chosen != NONE;
    bool perPoint = fastest;
    for (Stretch const& stretch : any.weighed)
    {
        bool const open = stretch.reason == Reason::Open || stretch.reason == Reason::Chosen;
        perPoint = perPoint && (!open || Near(stretch.netWorth, 100.0 * (stretch.to - stretch.from)));
        fastest = fastest && (!open || NotAbove(stretch.perMinute, any.weighed[any.chosen].perMinute));
    }
    Check(fastest && perPoint && any.weighed.size() >= little.weighed.size() && !any.wants.empty() &&
              any.reason == Reason::Chosen,
          "anypath: the fastest climb is taken, worth a minute per point, with its wants");

    // 2: a dearer stone makes the stretch worth no more
    World dear = useWorld;
    dear.vendors[1].buyCopper = 50;
    RecipeIndex const dearIndex = dear.Index();
    PathWorld const dearPaths = WorldOf(dearIndex, dear);
    bot.uses = {{BRACERS, 0, 5000.0f, 20}};
    Path const cheapPath = Weigh(bot, usePaths);
    Path const dearPath = Weigh(bot, dearPaths);
    Stretch const* cheapStretch = Find(cheapPath, SMITH, 10);
    Stretch const* dearStretch = Find(dearPath, SMITH, 10);
    Check(cheapStretch && dearStretch && dearStretch->netWorth < cheapStretch->netWorth,
          "2: a dearer material lowers the stretch's net worth");

    // 3: a poorer bot gathers the stone where the flush one buys it
    World gatherWorld = HandWorld();
    gatherWorld.vendors[1].buyCopper = 40;
    RecipeIndex const gatherIndex = gatherWorld.Index();
    PathWorld const gatherPaths = WorldOf(gatherIndex, gatherWorld);
    PathBot flush = HandBot();
    flush.rates.factor = 0.1f;
    flush.rates.minuteValue = 300.0f;
    PathBot poor = flush;
    poor.rates.factor = 4.0f;
    poor.money = 1000;
    Stretch const flushClimb = Weigher(flush, gatherPaths).Climb(SMITH, 1, 10);
    Stretch const poorClimb = Weigher(poor, gatherPaths).Climb(SMITH, 1, 10);
    Check(poorClimb.copper < flushClimb.copper, "3: the poor bot spends less copper on the same stretch (" +
                                                    std::to_string(poorClimb.copper) + " against " +
                                                    std::to_string(flushClimb.copper) + ")");

    // 7 and 8: three bars held; the bracers are a use (two bars) and the skill crafts want bars too
    PathBot claims = HandBot();
    claims.knownRecipes = {R_BRACERS};
    claims.held = {{BAR, 3}};
    claims.consumables = {{BRACERS, 1, 0, 100.0f}};
    Path skillPath;
    Stretch stretch;
    stretch.skill = SMITH;
    PathStep craft;
    craft.kind = StepKind::Craft;
    craft.id = R_BRACERS;
    craft.from = 1;
    craft.to = 5;
    craft.casts = 4.0;
    stretch.steps.push_back(craft);
    skillPath.weighed.push_back(stretch);
    skillPath.chosen = 0;
    Weigher const claimer(claims, paths);
    Claims const both = claimer.Claim(skillPath);
    Claims const usesOnly = claimer.Claim(Path{});
    Check(both.useCasts == usesOnly.useCasts && both.useCasts.at(R_BRACERS) == 1,
          "8: the use keeps its cast whatever the skill crafts want");
    Check(both.reserved.at(BAR) <= 3 && both.queue.size() == 1,
          "7: reserved bars never exceed the three held; the skill craft gets none (one bar left)");

    // 10: the same path whatever the order of the recipes and bags
    World reversed = useWorld;
    std::reverse(reversed.recipes.begin(), reversed.recipes.end());
    std::reverse(reversed.nodes.begin(), reversed.nodes.end());
    RecipeIndex const reversedIndex = reversed.Index();
    PathWorld const reversedPaths = WorldOf(reversedIndex, reversed);
    Check(Describe(Weigh(bot, usePaths)) == Describe(Weigh(bot, reversedPaths)),
          "10: the path does not depend on the order of the recipes");

    // a gatherer climbs on nodes: at 100 a node of skill 1 is grey, so mining at 100 needs a node above 0
    PathBot miner = HandBot();
    miner.skills = {{MINER, 1, 75}};
    Stretch const mining = Weigher(miner, paths).Climb(MINER, 1, 60);
    bool gathered = false;
    for (PathStep const& step : mining.steps)
        gathered = gathered || step.kind == StepKind::Gather || step.id == SMELT;
    Check(mining.reason == Reason::Open && gathered, "a miner climbs by nodes and smelting");
    Check(Weigher(miner, paths).Climb(MINER, 100, 110).reason == Reason::NoSource,
          "a miner at 100 with only skill-1 nodes and a grey smelt has no point (NoSource)");
}

// ---- the review's fixes (WR10/WR11 review): intermediates and tools ----
void CheckDependencies()
{
    // the hand world without the stone, belt and helm recipes: the bracers are the smith's only way up to the axe
    World world = HandWorld();
    world.recipes.erase(
        std::remove_if(world.recipes.begin(), world.recipes.end(), [](Recipe const& recipe)
                       { return recipe.spell == R_SHARP || recipe.spell == R_BELT || recipe.spell == R_HELM; }),
        world.recipes.end());
    PathBot smith = HandBot();
    smith.skills = {{SMITH, 20, 75}, {MINER, 30, 75}};
    smith.knownRecipes = {SMELT, R_BRACERS};
    smith.held = {{ORE, 10}};
    smith.uses = {{AXE, 0, 5000.0f, 20}};
    RecipeIndex const index = world.Index();
    PathWorld const paths = WorldOf(index, world);
    Path const plain = Weigh(smith, paths);
    std::cout << "a smith with 10 ore who knows the smelt and the bracers: " << Describe(plain) << "\n";
    bool const bracers = plain.chosen != NONE && plain.weighed[plain.chosen].skill == SMITH;
    Check(bracers, "11: the smith takes a bracers stretch toward the axe");
    auto const at = [&plain](std::uint32_t spell)
    {
        for (std::size_t i = 0; i < plain.queue.size(); ++i)
            if (plain.queue[i].spell == spell)
                return static_cast<int>(i);
        return NONE;
    };
    bool wantsBar = false;
    bool wantsOre = false;
    for (Want const& wanted : plain.wants)
    {
        wantsBar = wantsBar || wanted.item == BAR;
        wantsOre = wantsOre || wanted.item == ORE;
    }
    Check(bracers && at(SMELT) != NONE && at(R_BRACERS) != NONE && at(SMELT) < at(R_BRACERS) &&
              plain.queue[at(SMELT)].casts == 10 && plain.queue[at(SMELT)].forSkill &&
              plain.queue[at(R_BRACERS)].casts == 5,
          "11: the ten ore are smelted into bars before the five bracers casts they allow");
    Check(bracers && !wantsBar && wantsOre, "11: the stretch wants ore, not the bars it smelts");
    PathWorld noExpand = WorldOf(index, world, Fault::NoExpand);
    bool controlBar = false;
    for (Want const& wanted : Weigh(smith, noExpand).wants)
        controlBar = controlBar || wanted.item == BAR;
    Check(controlBar, "11: the negative control (no expansion) wants bars, as before the review");
    // WR12: the path's materials are what the bot keeps: the ten held ore with the ore it still wants
    Material const* ore = MaterialOf(plain, ORE);
    std::uint32_t oreWanted = 0;
    for (Want const& wanted : plain.wants)
        if (wanted.item == ORE)
            oreWanted = wanted.count;
    std::cout << "the smith's materials:";
    for (Material const& material : plain.materials)
        std::cout << " " << material.item << " used " << material.used << " wanted " << material.wanted << " at "
                  << material.copperEach << " c due at " << material.dueSkill;
    std::cout << "\n";
    Check(ore && ore->used == 10 + oreWanted && ore->wanted == oreWanted && ore->copperEach > 0 && ore->dueSkill == 0,
          "13: the held ore and the ore still wanted are the path's material, due now");
    Check(!MaterialOf(plain, BAR), "13: bars the smith smelts itself and holds none of are no material");
    Material const* controlOre = MaterialOf(Weigh(smith, WorldOf(index, world, Fault::WantsOnly)), ORE);
    Check(controlOre && controlOre->used == controlOre->wanted,
          "13: the negative control (wants only) loses the held ore, as a counterexample shows");

    // tools: the bracers ask for a hammer (totem category 162)
    constexpr std::uint32_t HAMMER = 5956;
    constexpr std::uint32_t HAMMER_CATEGORY = 162;
    constexpr std::uint32_t HAMMER_COPPER = 18;  // item_template.BuyPrice of the Blacksmith Hammer
    constexpr std::uint32_t R_HAMMER = 9003;
    World tooled = world;
    for (Recipe& recipe : tooled.recipes)
        if (recipe.spell == R_BRACERS)
            recipe.toolCategories = {HAMMER_CATEGORY};
    auto const toolSteps = [](Path const& path, std::uint32_t item, bool made)
    {
        int count = 0;
        if (path.chosen != NONE)
            for (PathStep const& step : path.weighed[path.chosen].steps)
                count += step.kind == StepKind::Tool && step.id == item && (step.casts > 0.0) == made;
        return count;
    };
    tooled.tools = {};
    RecipeIndex const noSourceIndex = tooled.Index();
    Path const noSource = Weigh(smith, WorldOf(noSourceIndex, tooled));
    Check(noSource.chosen == NONE, "12: a tool without a source keeps the bracers out of the path");
    tooled.tools = {{HAMMER, HAMMER_CATEGORY, HAMMER_COPPER}};
    RecipeIndex const soldIndex = tooled.Index();
    Path const bought = Weigh(smith, WorldOf(soldIndex, tooled));
    std::cout << "the same smith without a hammer, a vendor selling it: " << Describe(bought) << "\n";
    bool const needed = std::any_of(bought.needs.begin(), bought.needs.end(), [](Purchase const& purchase)
                                    { return purchase.kind == StepKind::Tool && purchase.spell == HAMMER; });
    Check(toolSteps(bought, HAMMER, false) == 1 && needed &&
              Near(plain.weighed[plain.chosen].netWorth - bought.weighed[bought.chosen].netWorth, HAMMER_COPPER),
          "12: a vendor's tool is bought once, at its price, as a step and a need");
    PathBot holds = smith;
    holds.categoriesHeld = {HAMMER_CATEGORY};
    Check(Describe(Weigh(holds, WorldOf(soldIndex, tooled))) == Describe(plain),
          "12: with the hammer held the path is the one without tools");
    // made: a smith recipe of its own makes the hammer from a stone (grey at 20: no skill point of its own)
    Recipe hammer = MakeRecipe(SMITH, R_HAMMER, HAMMER, {{STONE, 1}}, 5, 15, 1500);
    tooled.recipes.push_back(hammer);
    tooled.items.push_back(Fact(HAMMER));
    tooled.tools = {{HAMMER, HAMMER_CATEGORY, 0}};
    PathBot maker = smith;
    maker.knownRecipes.push_back(R_HAMMER);
    maker.held.push_back({STONE, 1});
    RecipeIndex const madeIndex = tooled.Index();
    Path const made = Weigh(maker, WorldOf(madeIndex, tooled));
    std::cout << "the same smith making its hammer from a stone: " << Describe(made) << "\n";
    bool hammerFirst = false;
    for (std::size_t i = 0; i < made.queue.size(); ++i)
        if (made.queue[i].spell == R_HAMMER)
            hammerFirst = std::none_of(made.queue.begin(), made.queue.begin() + static_cast<std::ptrdiff_t>(i),
                                       [](Craft const& craft) { return craft.spell == R_BRACERS; });
    Check(toolSteps(made, HAMMER, true) == 1 && hammerFirst &&
              std::none_of(made.needs.begin(), made.needs.end(),
                           [](Purchase const& purchase) { return purchase.kind == StepKind::Tool; }),
          "12: a made tool is a cast before the casts that need it, no purchase");
    Check(Weigh(maker, WorldOf(madeIndex, tooled, Fault::ToolFree)).chosen != NONE,
          "12: the negative control crafts without the tool");
}

// ---- the search ----
// Professions WR14d: an unwanted green is kept for disenchanting by an enchanter and sold by a non-enchanter. The
// enchanter's path climbs on the dust of its own gear, so the outlet (ItemUsageValue::DisenchantOutletOf, its pure
// part) prices the yields at what they cost the path and pays for a green below its group's sale; a dearer green is
// sold. A tailor's path uses no dust, so its outlet is worth nothing and the green is sold. Both under the test
// fixture anyStretch (the stretch's choice is not what is checked here)
void CheckDisenchantOutlet()
{
    constexpr std::uint16_t ENCHANTER = static_cast<std::uint16_t>(Skill::Enchanting);
    constexpr std::uint16_t TAILOR = static_cast<std::uint16_t>(Skill::Tailoring);
    constexpr std::uint32_t DUST = 10940;
    constexpr std::uint32_t ESSENCE = 10938;
    constexpr std::uint32_t CLOTH = 2589;
    constexpr std::uint32_t BANDAGE = 2570;
    constexpr std::uint32_t GROUP = 1;
    World world;
    world.recipes = {MakeRecipe(ENCHANTER, 7418, 0, {{DUST, 1}}, 70, 90, 3000),
                     MakeRecipe(ENCHANTER, 7420, 0, {{DUST, 2}}, 70, 90, 3000),
                     MakeRecipe(TAILOR, 2387, BANDAGE, {{CLOTH, 1}}, 70, 90, 3000),
                     MakeRecipe(TAILOR, 2389, BANDAGE + 1, {{CLOTH, 2}}, 70, 90, 3000)};
    world.recipes[0].effect = world.recipes[1].effect = Recipes::Effect::Enchant;
    world.recipes[1].minRank = world.recipes[3].minRank = 40;  // a milestone above the start
    world.rungs = Ladder(ENCHANTER, 7411);
    for (Rung const& rung : Ladder(TAILOR, 3908))
        world.rungs.push_back(rung);
    Process disenchant;
    disenchant.kind = Recipes::ProcessKind::Disenchant;
    disenchant.source = GROUP;
    disenchant.skill = ENCHANTER;
    disenchant.rank = 1;
    disenchant.yields = {{DUST, 1.2}, {ESSENCE, 0.3}};
    disenchant.saleCopper = 187;
    world.processes = {disenchant};
    for (std::uint32_t const id : {DUST, ESSENCE, CLOTH, BANDAGE, BANDAGE + 1})
        world.items.push_back(Fact(id));
    world.vendors = {{CLOTH, 20, 1, 1, 0}};
    RecipeIndex const index = world.Index();
    PathWorld const paths = WorldOf(index, world);

    auto const keptForDisenchant = [&disenchant](Path const& path, std::uint32_t sale)
    {
        std::vector<WorldTravel::OutletYield> yields;
        for (Recipes::Yield const& yield : disenchant.yields)
        {
            Material const* material = MaterialOf(path, yield.item);
            yields.push_back({yield.expected, WorldTravel::YieldWorthEach(material ? material->copperEach : 0, 0, 0)});
        }
        return WorldTravel::DisenchantPays(WorldTravel::DisenchantWorth(yields), sale);
    };

    PathBot enchanter = HandBot();
    enchanter.skills = {{ENCHANTER, 1, 75}};
    enchanter.anyStretch = true;
    Path const enchanting = Weigh(enchanter, paths);
    Material const* dust = MaterialOf(enchanting, DUST);
    Check(dust && dust->copperEach > 0, "an enchanter's path climbs on the dust of its own gear, at its cost");
    Check(keptForDisenchant(enchanting, 94),
          "an enchanter keeps an unwanted green below its group's sale to disenchant");
    Check(!keptForDisenchant(enchanting, 400), "an enchanter sells a green dearer than its materials");

    PathBot tailor = HandBot();
    tailor.skills = {{TAILOR, 1, 75}};
    tailor.anyStretch = true;
    Path const tailoring = Weigh(tailor, paths);
    Check(!MaterialOf(tailoring, DUST) && !keptForDisenchant(tailoring, 94),
          "a non-enchanter's path uses no dust, so it sells the same green");
    std::cout << "disenchant outlet: enchanter dust " << (dust ? dust->copperEach : 0) << " c each, "
              << Describe(enchanting) << "; tailor " << Describe(tailoring) << '\n';
}

// Processing in the claims (WR14e, PLAN 3.10): an intermediate no known recipe makes is milled (or prospected) from
// a held source the bot's skill takes, queued before the craft that takes it; a disenchant is never queued
std::string QueueText(std::vector<Craft> const& queue)
{
    std::ostringstream out;
    for (Craft const& craft : queue)
        out << (out.tellp() ? ", " : "") << craft.spell << " x" << craft.casts
            << (craft.forSkill  ? " (skill)"
                : craft.forSale ? " (sale)"
                                : "");
    return out.tellp() ? out.str() : "none";
}

// WA7b (PLAN 3.7, professions WR.6): a craft for sale is the last claim. Five bars held; the bracers are a use (two
// bars), the chain sells at the auction: the chain takes the three bars the use leaves. None where the bars fetch more
// than the chains, none without a market. Controls: the sale claiming first takes the use's bars; the sale at a loss
// is cast
void CheckSaleClaim()
{
    constexpr std::uint32_t CHAIN = 9302;
    constexpr std::uint32_t R_CHAIN = 9301;
    World world = HandWorld();
    world.recipes.push_back(MakeRecipe(SMITH, R_CHAIN, CHAIN, {{BAR, 1}}, 15, 50, 1000));
    world.items.push_back(Fact(CHAIN));
    world.sell[CHAIN] = 5;
    world.sell[BAR] = 2;
    RecipeIndex const index = world.Index();
    PathBot bot = HandBot();
    bot.knownRecipes = {R_BRACERS, R_CHAIN};
    bot.held = {{BAR, 5}};
    bot.consumables = {{BRACERS, 1, 0, 100.0f}};
    auto const marketOf = [=](std::uint32_t chain, std::uint32_t bar)
    {
        auto snapshot = std::make_shared<AuctionIndex::Snapshot>();
        for (auto const& [item, price] : {std::pair{CHAIN, chain}, std::pair{BAR, bar}})
            if (price)
            {
                AuctionIndex::TypicalPrice& typical = snapshot->items[item].typical;
                typical.unitPrice = price;
                typical.sample = 10;
                typical.weight = 10.0;
            }
        return Market::View{snapshot, {}, {}, {}};
    };
    auto const queueOf = [&](Market::View const& market, Fault fault)
    {
        PathWorld paths = WorldOf(index, world, fault);
        paths.market = market;
        Weigher const weigher(bot, paths);
        Path path;
        weigher.TakeClaims(path, weigher.Claim(path));
        return path.queue;
    };
    Market::View const sells = marketOf(20000, 0);
    Market::View const dearBars = marketOf(20000, 100000);
    std::vector<Craft> const sold = queueOf(sells, Fault::None);
    std::vector<Craft> const kept = queueOf(dearBars, Fault::None);
    std::vector<Craft> const plain = queueOf({}, Fault::None);
    std::vector<Craft> const first = queueOf(sells, Fault::SaleFirst);
    std::vector<Craft> const loss = queueOf(dearBars, Fault::SaleAtLoss);
    std::cout << "sale claim (WA7b): market " << QueueText(sold) << "; bars dearer than chains " << QueueText(kept)
              << "; no market " << QueueText(plain) << "; control sale_first " << QueueText(first)
              << "; control sale_at_loss " << QueueText(loss) << "\n";
    Check(sold.size() == 2 && sold[0].spell == R_BRACERS && sold[0].casts == 1 && !sold[0].forSale &&
              sold[1].spell == R_CHAIN && sold[1].casts == 3 && sold[1].forSale && !sold[1].forSkill,
          "sale: the use's bracers first, then three chains for sale from the bars the use leaves");
    Check(kept.size() == 1 && !kept[0].forSale, "sale: no chain where the bars fetch more than the chains");
    Check(plain.size() == 1 && !plain[0].forSale, "sale: no craft for sale without a market");
    bool const starved = std::none_of(first.begin(), first.end(),
                                      [](Craft const& craft) { return craft.spell == R_BRACERS && craft.casts == 1; });
    Check(starved, "control sale_first: the sale takes the use's bars (caught)");
    Check(std::any_of(loss.begin(), loss.end(), [](Craft const& craft) { return craft.forSale; }),
          "control sale_at_loss: a chain for sale where the bars fetch more (caught)");
}

void CheckProcessingClaims()
{
    constexpr std::uint16_t SCRIBE = static_cast<std::uint16_t>(Skill::Inscription);
    constexpr std::uint32_t HERB = 765;
    constexpr std::uint32_t PIGMENT = 39151;
    constexpr std::uint32_t INK = 37101;
    constexpr std::uint32_t R_INK = 52738;
    constexpr std::uint32_t MILL = 51005;
    World world;
    world.recipes = {MakeRecipe(SCRIBE, R_INK, INK, {{PIGMENT, 1}}, 15, 35, 3000)};
    world.rungs = Ladder(SCRIBE, 45357);
    Process mill;
    mill.kind = Recipes::ProcessKind::Mill;
    mill.source = HERB;
    mill.skill = SCRIBE;
    mill.rank = 1;
    mill.consumed = Recipes::PROSPECT_MILL_COUNT;
    mill.yields = {{PIGMENT, 3.0}};
    world.processes = {mill};
    for (std::uint32_t const id : {HERB, PIGMENT, INK})
        world.items.push_back(Fact(id));
    RecipeIndex const index = world.Index();
    PathWorld const paths = WorldOf(index, world);

    Path path;
    Stretch stretch;
    stretch.skill = SCRIBE;
    PathStep craft;
    craft.kind = StepKind::Craft;
    craft.id = R_INK;
    craft.from = 1;
    craft.to = 5;
    craft.casts = 5.0;
    stretch.steps.push_back(craft);
    path.weighed.push_back(stretch);
    path.chosen = 0;
    auto const claimsOf = [&](std::uint32_t herbs, std::uint32_t skill)
    {
        PathBot scribe = HandBot();
        scribe.skills = {{SCRIBE, skill, 75}};
        scribe.knownRecipes = {R_INK};
        scribe.held = {{HERB, herbs}};
        return Weigher(scribe, paths).Claim(path);
    };
    Claims const twelve = claimsOf(12, 1);
    Check(twelve.queue.size() == 2 && twelve.queue[0].spell == MILL && twelve.queue[0].source == HERB &&
              twelve.queue[0].casts == 2 && twelve.queue[0].forSkill && twelve.queue[1].spell == R_INK &&
              twelve.queue[1].casts == 5 && !twelve.queue[1].source,
          "processing: 12 herbs are milled twice (6 pigments expected) before the 5 inks that take them");
    Check(twelve.reserved.at(HERB) == 10, "processing: the two mills reserve 10 of the 12 herbs");
    Claims const few = claimsOf(4, 1);
    Check(few.queue.empty() && !few.reserved.count(HERB), "processing: 4 herbs are no mill (it takes 5)");
    Claims const unskilled = claimsOf(12, 0);
    Check(unskilled.queue.empty(), "processing: no mill below its rank");
    Check(NextCasts(path, HandBot(), paths).empty(), "processing: a bot without the source queues nothing");

    Process disenchant = mill;
    disenchant.kind = Recipes::ProcessKind::Disenchant;
    world.processes = {disenchant};
    RecipeIndex const enchantIndex = world.Index();
    PathBot scribe = HandBot();
    scribe.skills = {{SCRIBE, 1, 75}};
    scribe.knownRecipes = {R_INK};
    scribe.held = {{HERB, 12}};
    Check(Weigher(scribe, WorldOf(enchantIndex, world)).Claim(path).queue.empty(),
          "processing: a disenchant is never queued (the item rules' outlet drives it)");
}

struct Tally
{
    std::array<long, INVARIANTS + 1> runs{};
    std::array<long, INVARIANTS + 1> violations{};

    void Add(int invariant, bool violated)
    {
        ++runs[invariant];
        violations[invariant] += violated;
    }
};

struct Case
{
    World world;
    PathBot bot;
    std::vector<std::uint32_t> bases;
};

Case RandomCase(std::mt19937& rng)
{
    auto pick = [&rng](int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); };
    Case one;
    World& world = one.world;
    constexpr std::uint32_t BASE = 1001;
    constexpr std::uint32_t BARS = 1101;
    int const bases = pick(3, 6);
    for (int i = 0; i < bases; ++i)
    {
        std::uint32_t const item = BASE + i;
        one.bases.push_back(item);
        world.items.push_back(Fact(item));
        world.sell[item] = pick(0, 5);
        int const source = pick(0, 9);
        if (source <= 4)
            world.vendors.push_back({item, static_cast<std::uint32_t>(5 * pick(1, 10)), 1, 1, 0});
        if (source >= 3 && source <= 8)
            world.nodes.push_back({static_cast<std::uint32_t>(pick(1, 3)), MINER,
                                   static_cast<std::uint32_t>(1 + 25 * pick(0, 3)), static_cast<std::uint32_t>(500 + i),
                                   static_cast<std::uint32_t>(pick(5, 40)), 20.0f * pick(1, 5), item,
                                   static_cast<double>(pick(1, 3))});
    }
    for (int i = 0; i < 2; ++i)
    {
        std::uint32_t const bar = BARS + i;
        world.items.push_back(Fact(bar));
        std::uint32_t const rank = i ? 65 : 1;
        Recipe smelt =
            MakeRecipe(MINER, 3001 + i, bar, {{BASE + static_cast<std::uint32_t>(i), 1}}, rank + 25, rank + 70, 1500);
        smelt.minRank = rank;
        world.recipes.push_back(smelt);
    }
    std::vector<std::uint32_t> materials = one.bases;
    materials.push_back(BARS);
    materials.push_back(BARS + 1);
    int const crafts = pick(4, 9);
    for (int i = 0; i < crafts; ++i)
    {
        std::uint32_t const spell = 4001 + i;
        std::uint32_t const product = 2001 + i;
        world.items.push_back(Fact(product));
        Recipe recipe;
        if (i && pick(0, 4) == 0)
        {
            // a duplicate: the same reagents, colours, cast and sell price under another spell
            recipe = world.recipes[world.recipes.size() - 1];
            world.sell[product] = world.sell[recipe.product];
            recipe.spell = spell;
            recipe.product = product;
        }
        else
        {
            std::vector<ItemCount> reagents;
            int const count = pick(1, 3);
            std::set<std::uint32_t> used;
            for (int r = 0; r < count; ++r)
            {
                std::uint32_t const item = materials[pick(0, static_cast<int>(materials.size()) - 1)];
                if (used.insert(item).second)
                    reagents.push_back({item, static_cast<std::uint32_t>(pick(1, 3))});
            }
            std::uint32_t const learn = static_cast<std::uint32_t>(pick(1, 120));
            std::uint32_t const yellow = learn + 10 * pick(1, 4);
            recipe = MakeRecipe(SMITH, spell, product, reagents, yellow, yellow + 10 * pick(1, 4), 500 * pick(0, 6));
            recipe.productCount = static_cast<std::uint32_t>(pick(1, 2));
            world.sell[product] = 5 * pick(0, 12);
            int const how = pick(0, 9);
            if (how <= 2)
                recipe.minRank = learn;
            else if (how <= 7)
                Trainer(recipe, learn, 50 * pick(0, 6), pick(0, 30));
            else
            {
                recipe.acquire = 0;
                recipe.items.push_back({9000 + spell, static_cast<std::uint32_t>(pick(0, 1)),
                                        static_cast<std::uint32_t>(100 * pick(1, 5))});
                if (pick(0, 1))
                    one.bot.recipeItems.push_back(9000 + spell);
            }
        }
        world.recipes.push_back(recipe);
        if (pick(0, 6) == 0)
            world.locked.insert(spell);
        if (pick(0, 3) == 0)
            one.bot.knownRecipes.push_back(spell);
    }
    world.rungs = Ladder(SMITH, 2018);
    if (pick(0, 9) == 0)
        world.locked.insert(2020);

    PathBot& bot = one.bot;
    bot.level = static_cast<std::uint32_t>(pick(5, 60));
    bot.cls = ProfessionRules::WARRIOR;
    bot.money = static_cast<std::uint64_t>(pick(0, 20000));
    bot.rates.exchange = 1.0f;
    float const factors[] = {0.5f, 1.0f, 2.0f};
    float const minutes[] = {1.0f, 2.0f, 4.0f};
    bot.rates.factor = factors[pick(0, 2)];
    bot.rates.minuteValue = minutes[pick(0, 2)];
    std::uint32_t const value = static_cast<std::uint32_t>(pick(1, 140));
    std::uint32_t const max = value < 75 ? 75 : value < 150 ? 150 : 225;
    bot.skills = {{SMITH, value, max}, {MINER, static_cast<std::uint32_t>(pick(0, 150)), 300}};
    for (std::uint32_t zone = 1; zone <= 3; ++zone)
    {
        if (pick(0, 1))
            bot.zones.push_back(zone);
        bot.risks[zone] = {static_cast<float>(pick(0, 4)), pick(0, 1) * 0.2f, 0.0f, 0.0f};
    }
    for (std::uint32_t const item : materials)
        if (pick(0, 2) == 0)
            bot.held.push_back({item, static_cast<std::uint32_t>(pick(1, 6))});
    for (int i = 0; i < pick(0, 2); ++i)
        bot.consumables.push_back({static_cast<std::uint32_t>(2001 + pick(0, crafts - 1)),
                                   static_cast<std::uint32_t>(pick(1, 4)), static_cast<std::uint32_t>(pick(0, 1)),
                                   static_cast<float>(10 * pick(1, 8))});
    for (int i = 0; i < pick(0, 2); ++i)
        bot.uses.push_back({static_cast<std::uint32_t>(2001 + pick(0, crafts - 1)), 0,
                            static_cast<float>(50 * pick(1, 20)), static_cast<std::uint32_t>(pick(1, 60))});

    // tools (the WR10/WR11 review): a category a vendor sells, one a smith recipe makes from a base, one without a
    // source; some recipes ask for one, the bot holds some; and the smelts it may know
    constexpr std::uint32_t TOOL_SOLD = 901;
    constexpr std::uint32_t TOOL_ITEM = 3901;
    world.tools = {{TOOL_ITEM, TOOL_SOLD, static_cast<std::uint32_t>(5 * pick(1, 40))},
                   {TOOL_ITEM + 1, TOOL_SOLD + 1, 0}};
    world.items.push_back(Fact(TOOL_ITEM + 1));
    Recipe maker = MakeRecipe(SMITH, 4901, TOOL_ITEM + 1, {{BASE, 1}}, 30, 80, 1500);
    maker.minRank = static_cast<std::uint32_t>(pick(1, 60));
    if (pick(0, 1))
        Trainer(maker, maker.minRank, 50 * pick(0, 4));
    world.recipes.push_back(maker);
    for (Recipe& recipe : world.recipes)
        if (recipe.skill == SMITH && recipe.spell != maker.spell && pick(0, 2) == 0)
            recipe.toolCategories.push_back(TOOL_SOLD + static_cast<std::uint32_t>(pick(0, 2)));
    for (std::uint32_t category = TOOL_SOLD; category <= TOOL_SOLD + 1; ++category)
        if (pick(0, 3) == 0)
            bot.categoriesHeld.push_back(category);
    for (std::uint32_t const smelt : {3001u, 3002u})
        if (pick(0, 1))
            bot.knownRecipes.push_back(smelt);
    return one;
}

// The case at a set of server rates: the index's rates, and each node's yield from its loot row at them (an odd node's
// row drops at 50 % twice the stock count, an even node's at 100 %: both give the stock yield at stock rates)
Case AtRates(Case const& one, Recipes::ServerRates const& rates)
{
    Case at = one;
    at.world.rates = rates;
    for (std::size_t i = 0; i < at.world.nodes.size(); ++i)
    {
        NodeRow& node = at.world.nodes[i];
        bool const odd = i % 2;
        auto const count = static_cast<std::uint32_t>(std::lround(one.world.nodes[i].perNode * (odd ? 2.0 : 1.0)));
        Recipes::LootTable table;
        table[1] = {{node.item, 0, odd ? 50.0f : 100.0f, 0, count, count, 1}};
        std::vector<Recipes::Yield> const yields = Recipes::ExpectedYield(table, 1, nullptr, rates);
        node.perNode = yields.empty() ? 0.0 : yields.front().expected;
    }
    return at;
}

// Every stretch matched by skill and target in another path
template <class Visit>
void Matched(Path const& a, Path const& b, Visit const& visit)
{
    for (Stretch const& left : a.weighed)
        if (Stretch const* right = Find(b, left.skill, left.to))
            visit(left, *right);
}

bool Spends(Stretch const& stretch) { return stretch.reason != Reason::GoldShort; }

// A stretch whose climb reaches its end (a blocked one is not attractive at all, its cost stops where it stops)
bool Weighable(Stretch const& stretch)
{
    return stretch.reason == Reason::Open || stretch.reason == Reason::Chosen || stretch.reason == Reason::NotWorthIt ||
           stretch.reason == Reason::GoldShort;
}

void CheckCase(Case const& one, Fault fault, std::mt19937& rng, Tally& tally, int only)
{
    auto want = [only](int invariant) { return !only || only == invariant; };
    RecipeIndex const index = one.world.Index();
    PathWorld const world = WorldOf(index, one.world, fault);
    Weigher const weigher(one.bot, world);
    Path const path = weigher.Weigh();

    if (want(1) && path.chosen != NONE)
        tally.Add(1, !(path.weighed[path.chosen].netWorth > 0.0));

    std::set<std::uint32_t> const toolsHeld(one.bot.toolsHeld.begin(), one.bot.toolsHeld.end());
    std::set<std::uint32_t> const categoriesHeld(one.bot.categoriesHeld.begin(), one.bot.categoriesHeld.end());
    for (Stretch const& stretch : path.weighed)
    {
        Acquired acquired;  // the tools the stretch's earlier steps got
        for (PathStep const& step : stretch.steps)
        {
            if (step.kind == StepKind::Tool)
                weigher.Acquire(acquired, step.id);
            Recipe const* recipe = nullptr;
            for (Recipe const& candidate : index.Recipes())
                if (candidate.spell == step.id && candidate.skill == stretch.skill)
                    recipe = &candidate;
            // the skills a step's skill-ups start at: from, then by the server's step (WR29)
            std::uint32_t const stride = std::max<std::uint32_t>(index.Rates().craftStep, 1);
            if (want(4) && step.kind == StepKind::Craft && recipe)
                for (std::uint32_t at = step.from; at < step.to; at += stride)
                    tally.Add(4, at >= recipe->grey);
            if (want(5) && step.kind == StepKind::Craft && recipe)
            {
                Materials::Bot const materials = weigher.MaterialBot(stretch.skill, step.from);
                Materials::Pricer const pricer(index, materials);
                bool blocked = false;
                for (ItemCount const& reagent : recipe->reagents)
                    blocked = blocked || pricer.UnitCost(reagent.item).Blocked();
                tally.Add(5, blocked);
            }
            if (want(6) && step.kind == StepKind::Craft)
                for (std::uint32_t at = step.from; at < step.to; at += stride)
                {
                    Option chosen;
                    double cheapest = 0.0;
                    bool any = false;
                    // the climb's cap for a skill-up's points: the rank's cap or the stretch's end, where the step ends
                    for (Option const& option : weigher.Options(stretch.skill, at, acquired, step.to))
                    {
                        if (option.kind == step.kind && option.id == step.id)
                            chosen = option;
                        double const perPoint = option.value / option.points;
                        cheapest = any ? std::min(cheapest, perPoint) : perPoint;
                        any = true;
                    }
                    tally.Add(6, !NotAbove(chosen.value / chosen.points, cheapest));
                }
            bool const known = std::find(one.bot.knownRecipes.begin(), one.bot.knownRecipes.end(), step.id) !=
                               one.bot.knownRecipes.end();
            if (want(9) && (step.kind == StepKind::Rank || step.kind == StepKind::Learn ||
                            (step.kind == StepKind::Craft && !known)))
                tally.Add(9, one.world.locked.count(step.id) > 0);
            if (want(12) && step.kind == StepKind::Craft && recipe)
            {
                bool missing = false;
                for (std::uint32_t const category : recipe->toolCategories)
                    missing = missing || !(categoriesHeld.count(category) || acquired.categories.count(category));
                for (std::uint32_t const tool : recipe->tools)
                    missing = missing || !(toolsHeld.count(tool) || acquired.items.count(tool));
                tally.Add(12, missing);
            }
        }
    }

    if (want(13) && path.chosen != NONE)
    {
        // every want is a material with that count wanted, the materials are sorted by item, and every reagent of a
        // planned craft that the bot holds is a material the path keeps
        bool wrong = !std::is_sorted(path.materials.begin(), path.materials.end(),
                                     [](Material const& a, Material const& b) { return a.item < b.item; });
        for (Want const& wanted : path.wants)
        {
            Material const* material = MaterialOf(path, wanted.item);
            wrong = wrong || !material || material->wanted != wanted.count || material->used < material->wanted;
        }
        std::set<std::uint32_t> held;
        for (ItemCount const& item : one.bot.held)
            if (item.count)
                held.insert(item.item);
        Stretch const& taken = path.weighed[path.chosen];
        for (PathStep const& step : taken.steps)
            for (Recipe const& recipe : index.Recipes())
                if (step.kind == StepKind::Craft && recipe.spell == step.id && recipe.skill == taken.skill)
                    for (ItemCount const& reagent : recipe.reagents)
                        wrong = wrong || (held.count(reagent.item) && !MaterialOf(path, reagent.item));
        tally.Add(13, wrong);
    }

    if (want(11) && path.chosen != NONE)
    {
        // a want is what the bot gets, never an item it makes by a recipe it knows or learns, or processes
        Stretch const& taken = path.weighed[path.chosen];
        std::set<std::uint32_t> knows(one.bot.knownRecipes.begin(), one.bot.knownRecipes.end());
        for (PathStep const& step : taken.steps)
            if (step.kind == StepKind::Learn)
                knows.insert(step.id);
        Materials::Bot const materials = weigher.MaterialBot(taken.skill, taken.from);
        Materials::Pricer const pricer(index, materials);
        for (Want const& wanted : path.wants)
        {
            if (wanted.count > Materials::MAX_QUANTITY)
                continue;
            Materials::Cost const unit = pricer.UnitCost(wanted.item);
            tally.Add(11, (unit.kind == Materials::Kind::Made && knows.count(unit.via)) ||
                              unit.kind == Materials::Kind::Processed);
        }
    }

    if (want(7) || want(8))
    {
        Claims const claims = weigher.Claim(path);
        std::map<std::uint32_t, std::uint32_t> held;
        for (ItemCount const& item : one.bot.held)
            held[item.item] += item.count;
        if (want(7))
            for (auto const& [item, reserved] : claims.reserved)
                tally.Add(7, reserved > held[item]);
        if (want(8) && path.chosen != NONE)
            tally.Add(8, claims.useCasts != weigher.Claim(Path{}).useCasts);
    }

    if (want(15) || want(16) || want(17))
    {
        // the crafts for sale (WA7b) under a market for the world's items; the reference: the unfaulted claims of the
        // same path, which hold no sale. 15: no sale without a market; a sale takes only units held beyond what the
        // path uses and the other crafts take. 16: the other crafts, the uses' casts and worth equal the reference's.
        // 17: a sale's proceeds beat its inputs at their cost; its product is no path material, its spell no other
        // craft's, its tools held, at most a lot of products
        if (want(15))
            for (Craft const& craft : path.queue)
                tally.Add(15, craft.forSale);
        PathWorld marketWorld = WorldOf(index, one.world, fault);
        marketWorld.market = SaleMarket(one.world);
        Path const sold = Weigher(one.bot, marketWorld).Weigh();
        PathWorld referenceWorld = WorldOf(index, one.world);
        referenceWorld.market = marketWorld.market;
        Weigher const reference(one.bot, referenceWorld);
        Claims const claims = reference.Claim(sold);
        std::vector<Material> materials;
        static_cast<void>(reference.Wants(sold, claims, &materials));
        std::map<std::uint32_t, std::uint32_t> held;
        for (ItemCount const& item : one.bot.held)
            held[item.item] += item.count;
        std::map<std::uint32_t, std::uint64_t> taken;
        std::vector<Craft> others;
        std::map<std::uint32_t, std::uint32_t> useCasts;
        Market::View const& market = marketWorld.market;
        for (Craft const& craft : sold.queue)
        {
            if (!craft.forSale)
            {
                others.push_back(craft);
                if (!craft.forSkill)
                    useCasts[craft.spell] += craft.casts;
                continue;
            }
            Recipe const* recipe = index.RecipeOf(craft.spell);
            if (!recipe)
            {
                tally.Add(17, true);
                continue;
            }
            for (ItemCount const& reagent : recipe->reagents)
                taken[reagent.item] += static_cast<std::uint64_t>(reagent.count) * craft.casts;
            if (!want(17))
                continue;
            std::uint32_t skill = 0;
            for (SkillLevel const& level : one.bot.skills)
                if (level.skill == recipe->skill)
                    skill = level.value;
            Materials::Bot const bot = reference.MaterialBot(recipe->skill, skill);
            Materials::Pricer const pricer(index, bot);
            double const vendor = marketWorld.sellCopper(recipe->product);
            double const proceeds =
                market.ProceedsOf(recipe->product, static_cast<std::uint32_t>(std::max(vendor, 0.0))) *
                std::max<std::uint32_t>(recipe->productCount, 1);
            double inputs = 0.0;
            for (ItemCount const& reagent : recipe->reagents)
            {
                Materials::Cost const unit = pricer.UnitCost(reagent.item);
                double const cost = unit.Blocked() ? 0.0 : unit.value / static_cast<double>(one.bot.rates.exchange);
                inputs +=
                    std::max(cost, market.SaleOf(reagent.item, marketWorld.sellCopper(reagent.item))) * reagent.count;
            }
            bool wrong = !(proceeds > inputs) || craft.casts * std::max<std::uint32_t>(recipe->productCount, 1) >
                                                     std::max<std::uint32_t>(Market::MAX_LOT, recipe->productCount);
            for (Material const& material : materials)
                wrong = wrong || material.item == recipe->product;
            for (Craft const& other : claims.queue)
                wrong = wrong || other.spell == craft.spell;
            for (std::uint32_t const tool : recipe->tools)
                wrong = wrong ||
                        std::find(one.bot.toolsHeld.begin(), one.bot.toolsHeld.end(), tool) == one.bot.toolsHeld.end();
            for (std::uint32_t const category : recipe->toolCategories)
                wrong = wrong || std::find(one.bot.categoriesHeld.begin(), one.bot.categoriesHeld.end(), category) ==
                                     one.bot.categoriesHeld.end();
            tally.Add(17, wrong);
        }
        if (want(15))
            for (auto const& [item, units] : taken)
            {
                auto const reserved = claims.reserved.find(item);
                std::uint64_t kept = reserved == claims.reserved.end() ? 0 : reserved->second;
                for (Material const& material : materials)
                    if (material.item == item)
                        kept = std::max<std::uint64_t>(kept, material.used);
                tally.Add(15, units + kept > held[item]);
            }
        if (want(16))
        {
            bool same = others.size() == claims.queue.size() && useCasts == claims.useCasts &&
                        sold.useWorth == claims.useWorth && sold.foodUnits == claims.foodUnits;
            for (std::size_t i = 0; same && i < others.size(); ++i)
                same = others[i].spell == claims.queue[i].spell && others[i].casts == claims.queue[i].casts &&
                       others[i].forSkill == claims.queue[i].forSkill && others[i].source == claims.queue[i].source;
            tally.Add(16, !same);
        }
    }

    auto pick = [&rng](int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); };
    if (want(2))
    {
        // a dearer material: its price raised, or the zones of its nodes made more dangerous
        Case dear = one;
        std::uint32_t const item = one.bases[pick(0, static_cast<int>(one.bases.size()) - 1)];
        for (VendorRow& vendor : dear.world.vendors)
            if (vendor.item == item)
                vendor.buyCopper = vendor.buyCopper * 2 + 10;
        for (NodeRow const& node : dear.world.nodes)
            if (node.item == item)
                dear.bot.risks[node.zone].levelsAbove += 2.0f;
        RecipeIndex const dearIndex = dear.world.Index();
        PathWorld const dearWorld = WorldOf(dearIndex, dear.world, fault);
        Matched(path, Weigh(dear.bot, dearWorld),
                [&tally](Stretch const& before, Stretch const& after)
                {
                    if (Weighable(before) && Weighable(after))
                        tally.Add(2, !NotAbove(after.netWorth, before.netWorth));
                });
    }
    if (want(3))
    {
        // a poorer bot in the same state: its copper weighs twice as much, it holds half the gold
        PathBot poor = one.bot;
        poor.rates.factor *= 2.0f;
        poor.money /= 2;
        Matched(Weigh(poor, world), path,
                [&tally](Stretch const& poorer, Stretch const& richer)
                {
                    if (!Weighable(poorer) || !Weighable(richer))
                        return;
                    double const spent = Spends(poorer) ? poorer.copper : 0.0;
                    tally.Add(3, !NotAbove(spent, richer.copper));
                });
    }
    if (want(14) && one.world.rates == Recipes::ServerRates{})
    {
        // higher skill-gain and loot rates (ours) never make a stretch dearer: every stretch reachable at both whose
        // stock climb's skill-ups each cost something costs at most its stock cost (a skill-up that pays for itself
        // is a gain the faster climb makes fewer of)
        Case const higher = AtRates(one, EconomyCases::Ours());
        RecipeIndex const higherIndex = higher.world.Index();
        PathWorld const higherWorld = WorldOf(higherIndex, higher.world, fault);
        Matched(path, Weigh(higher.bot, higherWorld),
                [&tally](Stretch const& before, Stretch const& after)
                {
                    bool const eachCosts = std::all_of(before.steps.begin(), before.steps.end(),
                                                       [](PathStep const& step) { return step.value >= 0.0; });
                    if (Weighable(before) && Weighable(after) && eachCosts)
                        tally.Add(14, !NotAbove(after.cost.Total(), before.cost.Total()));
                });
    }
    if (want(10))
    {
        Case shuffled = one;
        std::shuffle(shuffled.world.recipes.begin(), shuffled.world.recipes.end(), rng);
        std::shuffle(shuffled.world.nodes.begin(), shuffled.world.nodes.end(), rng);
        std::shuffle(shuffled.world.vendors.begin(), shuffled.world.vendors.end(), rng);
        std::shuffle(shuffled.bot.held.begin(), shuffled.bot.held.end(), rng);
        std::shuffle(shuffled.bot.knownRecipes.begin(), shuffled.bot.knownRecipes.end(), rng);
        std::shuffle(shuffled.bot.consumables.begin(), shuffled.bot.consumables.end(), rng);
        std::shuffle(shuffled.bot.uses.begin(), shuffled.bot.uses.end(), rng);
        std::reverse(shuffled.bot.skills.begin(), shuffled.bot.skills.end());
        RecipeIndex const shuffledIndex = shuffled.world.Index();
        PathWorld const shuffledWorld = WorldOf(shuffledIndex, shuffled.world, fault);
        tally.Add(10, Describe(path) != Describe(Weigh(shuffled.bot, shuffledWorld)));
    }
}

char const* const INVARIANT_NAMES[INVARIANTS + 1] = {
    "",
    "1 no stretch taken whose net worth is not above zero",
    "2 a dearer or more dangerous material never raises a stretch's net worth",
    "3 a poorer bot never spends more copper on a stretch than a richer one",
    "4 no grey recipe planned for skill",
    "5 no material without a source",
    "6 no cheaper option at a skill than the chosen one",
    "7 reserved never exceeds held",
    "8 a use is never starved by a skill craft",
    "9 a locked spell is never chosen",
    "10 the same input gives the same path",
    "11 a want is never an item the bot makes or processes itself",
    "12 a craft is planned only with its tools held or got by an earlier step",
    "13 every want and every held reagent of a planned craft is a material the path keeps",
    "14 higher skill-gain or loot rates never make a stretch dearer (WR.28)",
    "15 a craft for sale takes only units held beyond the path's use and the other crafts (WA7b)",
    "16 a use or skill craft is never starved by a craft for sale (WA7b)",
    "17 a craft for sale's proceeds beat its inputs at their cost (WA7b)",
};

Fault const FAULTS[INVARIANTS + 1] = {
    Fault::None,        Fault::TakeNonPositive, Fault::DearerCheaper,   Fault::PoorInverted, Fault::AllowGrey,
    Fault::BlockedFree, Fault::FirstFound,      Fault::ReserveUncapped, Fault::SkillFirst,   Fault::IgnoreLock,
    Fault::OrderTie,    Fault::NoExpand,        Fault::ToolFree,        Fault::WantsOnly,    Fault::RatesInverted,
    Fault::SaleFirst,   Fault::SaleFirst,       Fault::SaleAtLoss};

void Search(std::ostream& file)
{
    std::mt19937 rng(SEED);
    Tally tally;
    for (int i = 0; i < SEARCH_WORLDS; ++i)
        CheckCase(RandomCase(rng), Fault::None, rng, tally, 0);
    // the same worlds at our rates (WR29): every invariant but the fourteenth, which compares the two
    std::mt19937 oursRng(SEED);
    Tally ours;
    for (int i = 0; i < SEARCH_WORLDS; ++i)
        CheckCase(AtRates(RandomCase(oursRng), EconomyCases::Ours()), Fault::None, oursRng, ours, 0);
    Tally controls;
    for (int invariant = 1; invariant <= INVARIANTS; ++invariant)
    {
        std::mt19937 faultRng(SEED + invariant);
        for (int i = 0; i < FAULT_WORLDS; ++i)
            CheckCase(RandomCase(faultRng), FAULTS[invariant], faultRng, controls, invariant);
    }

    file << "## 3. The search\n\nSeed " << SEED << ", " << SEARCH_WORLDS << " random worlds (3-6 base materials "
         << "bought, gathered in three zones of random danger or without a source, two smelts, 4-9 recipes with "
         << "trainers, recipe items and duplicates, a rank ladder, random skills, gold, need factor, bags, uses, "
         << "locks, and tools: one sold, one made by a smith recipe, one without a source). Each negative control runs "
         << FAULT_WORLDS << " worlds with one deliberate fault.\n\n"
         << "The search runs at stock rates and again on the same worlds at our rates ("
         << RatesText(EconomyCases::Ours()) << "); the fourteenth invariant compares the two.\n\n"
         << "| invariant | checks | violations | at our rates: checks | violations | negative control: fault | its "
            "counterexamples |\n|---|---|---|---|---|---|---|\n";
    long total = 0;
    for (int invariant = 1; invariant <= INVARIANTS; ++invariant)
    {
        total += tally.runs[invariant];
        file << "| " << INVARIANT_NAMES[invariant] << " | " << tally.runs[invariant] << " | "
             << tally.violations[invariant] << " | " << ours.runs[invariant] << " | " << ours.violations[invariant]
             << " | " << FaultWord(FAULTS[invariant]) << " | " << controls.violations[invariant] << " of "
             << controls.runs[invariant] << " |\n";
        Check(tally.runs[invariant] > 0, std::string("the search checked invariant ") + INVARIANT_NAMES[invariant]);
        Check(tally.violations[invariant] == 0, std::string("no violation of ") + INVARIANT_NAMES[invariant]);
        Check(invariant == 14 || ours.runs[invariant] > 0,
              std::string("the search at our rates checked invariant ") + INVARIANT_NAMES[invariant]);
        Check(ours.violations[invariant] == 0,
              std::string("no violation at our rates of ") + INVARIANT_NAMES[invariant]);
        Check(controls.violations[invariant] > 0,
              std::string("the negative control finds a counterexample to ") + INVARIANT_NAMES[invariant]);
        std::cout << "invariant " << INVARIANT_NAMES[invariant] << ": " << tally.runs[invariant] << " checks, "
                  << tally.violations[invariant] << " violations; at our rates " << ours.runs[invariant] << " checks, "
                  << ours.violations[invariant] << " violations; control " << controls.violations[invariant]
                  << " counterexamples\n";
    }
    file << "\n";
    std::cout << "search: " << SEARCH_WORLDS << " worlds, " << total << " checks\n";
}

// ---- the real fixture: stand-ins for what WR11 fills live, stated in the printed assumptions ----
constexpr double SELL_COPPER_PER_LEVEL_SQUARED = 0.06;  // [E] harness stand-in for the vendor sell price
constexpr float UPGRADE_COPPER_PER_ITEM_LEVEL = 20.0f;  // [E] harness stand-in for travel's worth of an upgrade
constexpr std::uint32_t CONSUMABLE_STOCK = 5;           // [E] harness stand-in for item use's wanted stock
constexpr float CONSUMABLE_SELL_MULTIPLE = 4.0f;        // [E] a consumable used is worth 4x its sell price
constexpr std::uint32_t ITEM_CLASS_CONSUMABLE = 0;
constexpr std::uint32_t ITEM_CLASS_WEAPON = 2;
constexpr std::uint32_t ITEM_CLASS_ARMOR = 4;
constexpr std::uint32_t MIDDLE_LEVEL = 60;       // trains every rank to 300
constexpr std::uint32_t MIDDLE_RATE_LEVEL = 30;  // the rates of a level-30 bot
constexpr std::uint32_t PATH_TOP = 300;
constexpr float KILL_MINUTES = 1.0f;        // [E] a creature killed for its skin
constexpr float FISH_CAST_MINUTES = 0.25f;  // [E] one cast and its catch
constexpr float POOR_SHARE = 3.0f;          // money at a third of the reserve (GoalModel NEED_BROKE..NEED_AT_RESERVE)
constexpr std::uint64_t POOR_COPPER_PER_LEVEL = 100;
constexpr std::uint64_t FLUSH_COPPER_PER_LEVEL = 10000;

double SellStandIn(ItemFact const* fact)
{
    if (!fact)
        return 0.0;
    double const level = fact->itemLevel;
    return std::floor(level * level * SELL_COPPER_PER_LEVEL_SQUARED * std::max<std::uint32_t>(fact->quality, 1));
}

Materials::Rates RatesOf(std::uint32_t level, float share)
{
    using namespace Planner::Goals;
    std::uint32_t const band = WorldTravel::LevelBand(level);
    Materials::Rates rates;
    rates.exchange =
        BAND_XP_PER_MINUTE[band] / static_cast<float>(WorldTravel::DEFAULT_SLOT_BANDS[band].EarnCopperPerMinute);
    rates.factor = rates.exchange * share;
    rates.minuteValue = BAND_XP_PER_MINUTE[band];
    return rates;
}

std::string NameOf(RecipeIndex const& index, std::uint32_t spell)
{
    for (Recipe const& recipe : index.Recipes())
        if (recipe.spell == spell)
            return recipe.name;
    for (Rung const& rung : index.Rungs())
        if (rung.spell == spell)
            return rung.name;
    return "?";
}

std::string ProfessionName(std::uint16_t skill)
{
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        if (Id(info.Which) == skill)
            return std::string(info.Name);
    return std::to_string(skill);
}

std::string Copper(double value, Materials::Rates const& rates)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << (rates.exchange > 0.0f ? value / rates.exchange : 0.0) << " c";
    return out.str();
}

std::string Fixed(double value, int digits = 1)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
}

// The sources of a stretch's materials (bought, gathered, made, processed, loot) by its craft steps' reagents
std::string Mix(RecipeIndex const& index, Weigher const& weigher, std::uint16_t skill, PathStep const& step,
                std::set<std::uint32_t> const& loot)
{
    if (step.kind != StepKind::Craft)
        return "-";
    std::map<std::string, int> kinds;
    Materials::Bot const materials = weigher.MaterialBot(skill, step.from);
    Materials::Pricer const pricer(index, materials);
    for (Recipe const& recipe : index.Recipes())
        if (recipe.spell == step.id && recipe.skill == skill)
            for (ItemCount const& reagent : recipe.reagents)
                ++kinds[loot.count(reagent.item) ? "loot" : Materials::KindWord(pricer.UnitCost(reagent.item).kind)];
    std::string mix;
    for (auto const& [kind, count] : kinds)
        mix += (mix.empty() ? "" : ", ") + kind + (count > 1 ? " x" + std::to_string(count) : "");
    return mix.empty() ? "none" : mix;
}

void AllTools(RecipeIndex const& index, PathBot& bot)
{
    std::set<std::uint32_t> tools;
    std::set<std::uint32_t> categories;
    for (Recipe const& recipe : index.Recipes())
    {
        tools.insert(recipe.tools.begin(), recipe.tools.end());
        categories.insert(recipe.toolCategories.begin(), recipe.toolCategories.end());
    }
    bot.toolsHeld.assign(tools.begin(), tools.end());
    bot.categoriesHeld.assign(categories.begin(), categories.end());
}

// The cheapest path from 1 to 300 of every profession for a middle bot
void Paths(std::ostream& file, RecipeIndex const& index, World const& facts, std::string const& title,
           std::set<std::uint32_t> const& loot)
{
    Materials::Rates const rates = RatesOf(MIDDLE_RATE_LEVEL, Planner::Goals::NEED_AT_RESERVE);
    file << "### " << title << "\n\n";
    int open = 0;
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
    {
        std::uint16_t const skill = Id(info.Which);
        PathBot bot;
        bot.level = MIDDLE_LEVEL;
        bot.cls = ProfessionRules::WARRIOR;
        bot.money = FLUSH_COPPER_PER_LEVEL * MIDDLE_LEVEL * 100;
        bot.rates = rates;
        bot.skills = {{skill, 1, 75}};
        for (ProfessionRules::Craft const& craft : ProfessionRules::CRAFTS)
            if (craft.Which == info.Which && craft.Gatherer != Skill::None)
                bot.skills.push_back({Id(craft.Gatherer), PATH_TOP, PATH_TOP});
        AllTools(index, bot);
        PathWorld const world = WorldOf(index, facts);
        Weigher const weigher(bot, world);
        Stretch const climb = weigher.Climb(skill, 1, PATH_TOP);
        file << "#### " << info.Name << ": " << ReasonWord(climb.reason);
        if (climb.reason != Reason::Open)
            file << " at " << climb.blockedAt;
        file << "; " << Copper(climb.cost.Total(), rates) << " in all (materials "
             << Copper(climb.cost.materials, rates) << ", time " << Copper(climb.cost.time, rates) << ", fees "
             << Copper(climb.cost.fees, rates) << "), " << Fixed(climb.copper, 0) << " c paid, " << Fixed(climb.minutes)
             << " min\n\n"
             << "| skill | step | casts | materials from | copper paid | minutes |\n|---|---|---|---|---|---|\n";
        for (PathStep const& step : climb.steps)
        {
            std::string what;
            switch (step.kind)
            {
                case StepKind::Rank:
                    what = "rank: " + NameOf(index, step.id) + " (" + std::to_string(step.id) + ")";
                    break;
                case StepKind::Learn:
                    what = "learn: " + NameOf(index, step.id) + " (" + std::to_string(step.id) + ")";
                    break;
                case StepKind::Gather:
                    what = "gather node " + std::to_string(step.id) + " in zone " + std::to_string(step.zone);
                    break;
                case StepKind::Skin:
                    what = "skin level-" + std::to_string(step.id) + " creatures";
                    break;
                case StepKind::Fish:
                    what = "fish";
                    break;
                default:
                    what = NameOf(index, step.id) + " (" + std::to_string(step.id) + ")";
            }
            file << "| " << step.from << (step.to > step.from ? "-" + std::to_string(step.to) : "") << " | " << what
                 << " | " << Fixed(step.casts) << " | " << Mix(index, weigher, skill, step, loot) << " | "
                 << Fixed(step.copper, 0) << " | " << Fixed(step.minutes) << " |\n";
        }
        file << "\n";
        open += climb.reason == Reason::Open;
    }
    std::cout << title << ": " << open << " of " << ProfessionRules::PROFESSION_COUNT << " professions reach 300\n";
}

constexpr std::uint32_t HELD_RECIPE_ITEM = 6053;  // Recipe: Holy Protection Potion (alchemy, spell 7255)

struct Sample
{
    std::string name;
    std::uint8_t cls;
    std::uint32_t level;
    std::vector<SkillLevel> skills;
    std::vector<std::uint32_t> zones;  // where it plays and has played: materials are gathered only there (WR11)
    std::vector<std::uint32_t> recipeItems{};  // recipe items in its bags that it has not learned
};

// Stand-ins for item use's demand answer and travel's worth: the consumables the bot's crafts make for its level band
// and one upgrade per equipment slot within its look-ahead
void Wants(RecipeIndex const& index, PathBot& bot)
{
    std::uint32_t const soon = ProfessionRules::SoonLevels(bot.level);
    std::map<std::uint32_t, ItemFact const*> slots;
    std::set<std::uint32_t> seen;
    for (Recipe const& recipe : index.Recipes())
    {
        bool mine = false;
        for (SkillLevel const& skill : bot.skills)
            mine = mine || skill.skill == recipe.skill;
        ItemFact const* fact = recipe.product ? index.Item(recipe.product) : nullptr;
        if (!mine || !fact || !seen.insert(fact->id).second)
            continue;
        bool const forClass = !fact->allowableClass || (fact->allowableClass & ProfessionRules::ClassBit(bot.cls));
        if (fact->itemClass == ITEM_CLASS_CONSUMABLE && fact->requiredLevel <= bot.level + soon &&
            fact->requiredLevel + soon >= bot.level)
            bot.consumables.push_back(
                {fact->id, CONSUMABLE_STOCK, 0, static_cast<float>(SellStandIn(fact)) * CONSUMABLE_SELL_MULTIPLE});
        if ((fact->itemClass == ITEM_CLASS_WEAPON || fact->itemClass == ITEM_CLASS_ARMOR) && forClass &&
            fact->requiredLevel > bot.level && fact->requiredLevel <= bot.level + soon)
        {
            ItemFact const*& best = slots[fact->inventoryType];
            if (!best || fact->itemLevel > best->itemLevel)
                best = fact;
        }
    }
    for (auto const& [slot, fact] : slots)
        bot.uses.push_back({fact->id, 0, fact->itemLevel * UPGRADE_COPPER_PER_ITEM_LEVEL, fact->requiredLevel});
}

void Decisions(std::ostream& file, RecipeIndex const& index, World const& facts)
{
    std::vector<Sample> const samples = {
        {"level-8 warrior, mining and blacksmithing",
         ProfessionRules::WARRIOR,
         8,
         {{MINER, 40, 75}, {SMITH, 25, 75}},
         {12, 40}},
        {"level-8 warrior, blacksmithing without its gatherer yet",
         ProfessionRules::WARRIOR,
         8,
         {{SMITH, 15, 75}},
         {12, 40}},
        {"level-20 rogue, herbalism and alchemy",
         ProfessionRules::ROGUE,
         20,
         {{Id(Skill::Herbalism), 90, 150}, {Id(Skill::Alchemy), 70, 150}},
         {12, 40, 38, 44, 10, 11}},
        {"level-20 rogue, herbalism only (a gatherer)",
         ProfessionRules::ROGUE,
         20,
         {{Id(Skill::Herbalism), 70, 150}},
         {12, 40, 38, 44, 10, 11}},
        {"level-20 rogue, herbalism and alchemy, holding Recipe: Holy Protection Potion",
         ProfessionRules::ROGUE,
         20,
         {{Id(Skill::Herbalism), 90, 150}, {Id(Skill::Alchemy), 70, 150}},
         {12, 40, 38, 44, 10, 11},
         {HELD_RECIPE_ITEM}},
        {"level-35 mage, tailoring and enchanting",
         ProfessionRules::MAGE,
         35,
         {{Id(Skill::Tailoring), 150, 225}, {Id(Skill::Enchanting), 140, 225}},
         {33, 405, 45}},
        {"level-50 paladin, mining and jewelcrafting",
         ProfessionRules::PALADIN,
         50,
         {{MINER, 230, 300}, {Id(Skill::Jewelcrafting), 210, 300}},
         {33, 45, 47, 440, 51, 361, 28, 490}},
    };
    struct Variant
    {
        char const* name;
        float share;
        std::uint64_t copperPerLevel;
        bool danger;
    };
    Variant const variants[] = {{"poor", POOR_SHARE, POOR_COPPER_PER_LEVEL, false},
                                {"flush", Planner::Goals::NEED_FLUSH, FLUSH_COPPER_PER_LEVEL, false},
                                {"poor, dangerous zones", POOR_SHARE, POOR_COPPER_PER_LEVEL, true}};
    PathWorld const world = WorldOf(index, facts);
    int chosen = 0;
    int blocks = 0;
    bool heldCounted = false;
    file << "## 1. Decisions for the sample bots\n\nOne block per bot, one line per stretch weighed. Brings (the four "
            "questions of WR.5): use = what it could then make and would use soon, held = recipes it holds and could "
            "then learn, access = nodes or creatures in its zones that become usable (at what they fetch), income = "
            "what it could sell over the materials. All in neutral copper (value / exchange), weighted by nearness. "
            "Costs: materials (with their sources), time (casts, gathering, walking), fees (recipes by their share "
            "of the points they serve, ranks in full).\n\n";
    for (Sample const& sample : samples)
        for (Variant const& variant : variants)
        {
            if (variant.danger && (sample.level != 20 || !sample.recipeItems.empty()))
                continue;
            PathBot bot;
            bot.level = sample.level;
            bot.cls = sample.cls;
            bot.money = variant.copperPerLevel * sample.level;
            bot.rates = RatesOf(sample.level, variant.share);
            bot.skills = sample.skills;
            bot.zones = sample.zones;
            bot.recipeItems = sample.recipeItems;
            for (std::uint32_t const zone : sample.zones)
                bot.risks[zone] =
                    variant.danger ? Materials::PlaceRisk{3.0f, 0.3f, 0.0f, 0.0f} : Materials::PlaceRisk{};
            AllTools(index, bot);
            Wants(index, bot);
            Weigher const weigher(bot, world);
            Path const path = weigher.Weigh();
            Materials::Rates const& rates = bot.rates;
            file << "### " << sample.name << " (" << variant.name << ": " << bot.money << " c, need factor "
                 << Fixed(rates.factor / rates.exchange, 2) << " x exchange, a minute "
                 << Copper(rates.minuteValue, rates) << "; " << bot.consumables.size() << " consumables and "
                 << bot.uses.size() << " upgrades wanted)\n\n";
            for (Stretch const& stretch : path.weighed)
            {
                heldCounted = heldCounted || stretch.brings.recipesHeld > 0.0;
                std::map<std::string, int> sources;
                for (PathStep const& step : stretch.steps)
                    if (step.kind == StepKind::Craft)
                        ++sources[Mix(index, weigher, stretch.skill, step, {})];
                std::string mix;
                for (auto const& [kind, count] : sources)
                    mix += (mix.empty() ? "" : "; ") + kind;
                file << "- " << ProfessionName(stretch.skill) << " " << stretch.from << "->" << stretch.to << " "
                     << MilestoneWord(stretch.milestone) << " " << stretch.milestoneId << " ("
                     << (stretch.milestone == Milestone::Node || stretch.milestone == Milestone::Skinnable
                             ? std::string("node")
                             : NameOf(index, stretch.milestoneId))
                     << "), near " << Fixed(stretch.nearness, 2) << " | use " << Copper(stretch.brings.useSoon, rates)
                     << ", held " << Copper(stretch.brings.recipesHeld, rates) << ", access "
                     << Copper(stretch.brings.access, rates) << ", income " << Copper(stretch.brings.income, rates)
                     << " | materials " << Copper(stretch.cost.materials, rates) << " (" << (mix.empty() ? "-" : mix)
                     << "), time " << Copper(stretch.cost.time, rates) << ", fees " << Copper(stretch.cost.fees, rates)
                     << " | net " << Copper(stretch.netWorth, rates) << ", per minute "
                     << Copper(stretch.perMinute, rates) << " over " << Fixed(stretch.minutes) << " min, "
                     << Fixed(stretch.copper, 0) << " c paid | " << ReasonWord(stretch.reason);
                if (stretch.reason == Reason::NoSource || stretch.reason == Reason::Locked ||
                    stretch.reason == Reason::Level)
                    file << " at " << stretch.blockedAt;
                file << "\n";
            }
            file << "- choice: ";
            if (path.chosen == NONE)
                file << "none (" << ReasonWord(path.reason) << ")\n";
            else
            {
                Stretch const& taken = path.weighed[path.chosen];
                ++chosen;
                file << ProfessionName(taken.skill) << " " << taken.from << "->" << taken.to << "; needs";
                for (Purchase const& need : path.needs)
                    file << " " << NameOf(index, need.spell) << " (" << need.copper << " c)";
                file << "; wants";
                for (Want const& want : path.wants)
                {
                    ItemFact const* fact = index.Item(want.item);
                    file << " " << want.count << "x " << (fact ? fact->name : std::to_string(want.item));
                }
                file << "; queue";
                for (Craft const& craft : path.queue)
                    file << " " << craft.casts << "x " << NameOf(index, craft.spell)
                         << (craft.forSkill ? "" : " (use)");
                file << "\n";
            }
            blocks += path.weighed.empty() ? 0 : 1;
            file << "\n";
        }
    std::cout << "sample decisions: " << chosen << " bots take a stretch, " << blocks << " weighed at least one\n";
    Check(blocks > 0, "the sample bots weighed stretches");
    Check(heldCounted, "a sample bot's held recipe counts in what a stretch brings (WR.5's second question)");
}

void Assumptions(std::ostream& file, Recipes::ServerRates const& server)
{
    Materials::Rates const rates = RatesOf(MIDDLE_RATE_LEVEL, Planner::Goals::NEED_AT_RESERVE);
    file << "## 4. Assumptions\n\n"
         << "- The server's rates (WR29) of the decisions and paths: " << RatesText(server)
         << " (chances in per mille: orange/yellow/green/grey; a skill-up gives the gain step's points, at most to the "
            "cap, and the chance is taken at each skill the climb passes through).\n"
         << "- A recipe item's skill is not in the dump: it is taken as its yellow less " << ITEM_RECIPE_YELLOW_GAP
         << " [E]; a trainer recipe is learnable at the trainer's skill.\n"
         << "- Nearness: full to level x " << EXPECTED_SKILL_PER_LEVEL << ", nothing " << NEARNESS_SPAN
         << " points above [E]; a use counts less the further its level lies in the look-ahead (SoonLevels).\n"
         << "- Income counts " << INCOME_CASTS << " casts of a product sold, access " << ACCESS_GATHERS
         << " nodes or skins of a newly usable kind [E]; at most " << MAX_STRETCHES << " stretches per profession.\n"
         << "- Harness stand-ins (WR11 fills these live): sell price = item level squared x "
         << SELL_COPPER_PER_LEVEL_SQUARED << " x quality copper; an upgrade is worth item level x "
         << UPGRADE_COPPER_PER_ITEM_LEVEL << " c; item use wants " << CONSUMABLE_STOCK
         << " of each consumable the bot's crafts make for its band, each worth " << CONSUMABLE_SELL_MULTIPLE
         << "x its sell price; every tool held; a skin takes " << KILL_MINUTES << " min, a fishing cast "
         << FISH_CAST_MINUTES << " min, in any zone.\n"
         << "- The paths' middle bot: level " << MIDDLE_LEVEL << " (trains every rank), the rates of level "
         << MIDDLE_RATE_LEVEL << ": exchange " << Fixed(rates.exchange, 3) << " value per copper, need factor 1.0 x "
         << "exchange (money at its reserve), a minute worth " << Copper(rates.minuteValue, rates)
         << "; every zone open and safe; the paired gatherer at 300 (mining for blacksmithing, engineering and "
            "jewelcrafting, herbalism for alchemy and inscription, skinning for leatherworking); products counted "
            "at nothing (a guide's view: what the climb costs). The loot table prices every material without a "
            "source (cloth, leather, meat, dust: the dropped kind, WR20) at its stand-in sell price as if looted.\n"
         << "- Sample bots: rates of their level's band; poor = need factor " << POOR_SHARE << " x exchange and "
         << POOR_COPPER_PER_LEVEL << " c per level, flush = " << Planner::Goals::NEED_FLUSH << " x exchange and "
         << FLUSH_COPPER_PER_LEVEL << " c per level; dangerous zones 3 levels above with 30 % elites.\n\n";
}

// The fixture's decisions and paths; at the given rates when set (their skill-gain step and chances: the yields stay
// those the fixture was dumped at)
Recipes::ServerRates Fixture(std::string const& root, std::ostream& file, Recipes::ServerRates const* rates)
{
    std::ifstream in(root + "/tests/fixtures/recipe-index-dump.txt", std::ios::binary);
    std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    RecipeIndex index;
    std::string error;
    Check(RecipeIndex::Parse(text, index, error), "the dumped fixture parses: " + error);
    file << "Fixture: `" << text.substr(0, text.find('\n')) << "`; its yields at `" << RatesText(index.Rates())
         << "`\n\n";
    if (rates)
        index.SetRates(*rates);
    file << "Rates of the decisions and paths: `" << RatesText(index.Rates()) << "`\n\n";

    World facts;
    for (ItemFact const& fact : index.Items())
        facts.sell[fact.id] = SellStandIn(&fact);
    for (std::uint32_t level = 1; level <= 80; ++level)
        facts.skinnables.push_back({0, level, KILL_MINUTES, 0.0f});
    facts.waters.push_back({0, FISH_CAST_MINUTES});

    Decisions(file, index, facts);

    World pathFacts = facts;
    pathFacts.sell.clear();
    file << "## 2. Cheapest path from 1 to 300 per profession\n\n";
    Paths(file, index, pathFacts, "2a. At vendor and gathering costs", {});

    // the loot table: every base material without a source, priced as if looted at its stand-in sell price
    std::vector<VendorRow> vendors = index.Vendors();
    std::set<std::uint32_t> loot;
    Materials::Bot all;
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        all.skills[Id(info.Which)] = PATH_TOP;
    all.rates = RatesOf(MIDDLE_RATE_LEVEL, Planner::Goals::NEED_AT_RESERVE);
    Materials::Pricer const pricer(index, all);
    for (Recipe const& recipe : index.Recipes())
        for (ItemCount const& reagent : recipe.reagents)
            if (!loot.count(reagent.item) && pricer.UnitCost(reagent.item).Blocked())
            {
                loot.insert(reagent.item);
                auto const price = static_cast<std::uint32_t>(std::max(1.0, SellStandIn(index.Item(reagent.item))));
                auto const row = std::find_if(vendors.begin(), vendors.end(), [&reagent](VendorRow const& vendor)
                                              { return vendor.item == reagent.item; });
                if (row == vendors.end())
                    vendors.push_back({reagent.item, price, 1, 1, 0});
                else
                    *row = {reagent.item, price, 1, 1, row->limited};
            }
    RecipeIndex looted = RecipeIndex::FromRecords(index.Recipes(), index.Rungs(), index.Processes(), index.Items(),
                                                  vendors, index.Nodes());
    looted.SetRates(index.Rates());
    Paths(file, looted, pathFacts,
          "2b. With looted materials (" + std::to_string(loot.size()) + " materials without a source priced as loot)",
          loot);
    return index.Rates();
}
}  // namespace

int main(int argc, char** argv)
{
    std::string const root = argc > 1 ? argv[1] : ".";
    std::string const out =
        argc > 2 ? argv[2] : (std::filesystem::temp_directory_path() / "skill-path-printed.md").string();
    Recipes::ServerRates const ours = EconomyCases::Ours();
    bool const atOurs = argc > 3 && std::string(argv[3]) == "ours";
    std::ostringstream file;
    file << (atOurs ? "# WR29: the skill path's printed decisions and paths at our rates"
                    : "# WR10: the skill path's printed decisions and paths")
         << "\n\nGenerated by `tests/skill-path-selftest.cpp` (seed " << SEED << ").\n\n";
    CheckChances();
    CheckEconomySkill();
    CheckQueueWorth();
    CheckSupply();
    CheckHand();
    CheckAbilityGuard();
    CheckDropSources();
    CheckDependencies();
    CheckDisenchantOutlet();
    CheckProcessingClaims();
    CheckSaleClaim();
    std::ostringstream search;
    Search(search);
    std::ostringstream fixture;
    Recipes::ServerRates const printed = Fixture(root, fixture, atOurs ? &ours : nullptr);
    std::ostringstream assumptions;
    Assumptions(assumptions, printed);
    file << fixture.str();
    file << search.str() << assumptions.str();
    std::ofstream written(out, std::ios::binary);
    written << file.str();
    std::cout << "tables: " << out << '\n';
    std::cout << "SKILL path selftest: " << passed + failed << " checks, " << failed << " failed ("
              << (failed ? "FAIL" : "PASS") << ")\n";
    return failed ? 1 : 0;
}
