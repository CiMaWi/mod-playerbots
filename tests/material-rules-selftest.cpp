/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the material rules (src/Ai/World/Rpg/MaterialRules.h, professions plan WR9): the sources and
// unit costs of a hand index, a seeded search over random recipe worlds for the stated invariants (a material without
// a source is blocked, never free; cost rises with the price of a minute; a more dangerous place never makes a
// material cheaper; a bought material never costs a poorer bot less; a cycle of recipes ends at the bounds; the
// answer does not depend on the order of the rows), a negative control (a caution of the wrong sign must make danger
// cheaper), and the tables of the dumped fixture (tests/fixtures/recipe-index-dump.txt): per profession each base
// material's cheapest source and cost for a safe and a dangerous zone and a poor and a flush bot. WR20a adds the drop
// rows (Kind::Dropped, the fight estimate): hand checks, and random worlds whose drop rows the drop build makes from
// random creatures, spawns and sources, searched for five more invariants with a negative control each (a dearer or
// more dangerous row never makes a material cheaper; a skin is never offered without its skill; no row stands on an
// instance map; the chosen row is the cheapest; a creature friendly to the bot's side is never a source). The auction
// house's WA7a adds the auction as a source (Kind::Auction at the market's typical price, weighed by the need factor)
// and what a listing fetches (Market::ProceedsEach): hand checks, and random worlds with random prices searched for
// five invariants with a negative control each (the auction is taken only with a price and only when strictly cheaper,
// a strictly cheaper auction is never passed over; an empty market changes no cost; a dearer auction price never makes
// a material cheaper; a poorer bot never pays more copper for a material than a richer one).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "MarketRules.h"
#include "MaterialRules.h"
#include "ProfessionRules.h"
#include "RecipeIndex.h"

using namespace Materials;
using Recipes::DropKind;
using Recipes::DropRow;
using Recipes::DropSource;
using Recipes::DropSpawn;
using Recipes::ItemCount;
using Recipes::ItemFact;
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

constexpr std::uint32_t ORE = 2770;
constexpr std::uint32_t BAR = 2840;
constexpr std::uint32_t BRACERS = 2853;
constexpr std::uint32_t THREAD = 2320;
constexpr std::uint32_t LIMITED = 4470;
constexpr std::uint32_t LINEN = 2589;
constexpr std::uint32_t HERB = 765;
constexpr std::uint32_t PIGMENT = 39151;
constexpr std::uint32_t INK = 39469;
constexpr std::uint32_t CYCLE_A = 90001;
constexpr std::uint32_t CYCLE_B = 90002;
constexpr std::uint32_t SAFE_ZONE = 12;
constexpr std::uint32_t NEAR_ZONE = 40;
constexpr std::uint32_t SEED = 20261120;
constexpr int SEARCH_WORLDS = 4000;
constexpr std::uint32_t WORLD_ITEMS = 12;
constexpr std::uint32_t LEATHER = 2318;
constexpr std::uint32_t BOAR_MEAT = 769;
constexpr std::uint32_t INSTANCE_MAP = 33;
constexpr std::uint32_t DROP_KINDS = 8;  // creature kinds of a random world
constexpr float CHANCE_FLOOR = 0.02f;    // questing's SOURCE_CHANCE_FLOOR

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

RecipeIndex HandIndex()
{
    std::vector<Recipe> recipes = {
        MakeRecipe(Skill::Mining, 2657, BAR, {{ORE, 1}}, 0),
        MakeRecipe(Skill::Blacksmithing, 2663, BRACERS, {{BAR, 2}, {THREAD, 1}}, 5000),
        MakeRecipe(Skill::Blacksmithing, 90011, CYCLE_A, {{CYCLE_B, 1}}, 0),
        MakeRecipe(Skill::Blacksmithing, 90012, CYCLE_B, {{CYCLE_A, 1}}, 0),
        MakeRecipe(Skill::Inscription, 52738, INK, {{PIGMENT, 1}}, 0),
    };
    Process mill;
    mill.kind = ProcessKind::Mill;
    mill.source = HERB;
    mill.skill = Id(Skill::Inscription);
    mill.rank = 1;
    mill.consumed = Recipes::PROSPECT_MILL_COUNT;
    mill.yields = {{PIGMENT, 2.5}};
    std::vector<ItemFact> items;
    for (std::uint32_t const id : {ORE, BAR, BRACERS, THREAD, LIMITED, LINEN, HERB, PIGMENT, INK, CYCLE_A, CYCLE_B})
    {
        ItemFact fact;
        fact.id = id;
        fact.known = true;
        fact.name = "item " + std::to_string(id);
        items.push_back(fact);
    }
    std::vector<VendorRow> vendors = {{THREAD, 50, 5, 3, 0}, {LIMITED, 500, 1, 0, 2}};
    std::vector<NodeRow> nodes = {
        {SAFE_ZONE, Id(Skill::Mining), 1, 1731, 40, 70.0f, ORE, 2.0},
        {NEAR_ZONE, Id(Skill::Mining), 1, 1731, 25, 35.0f, ORE, 2.0},
        {SAFE_ZONE, Id(Skill::Herbalism), 1, 1617, 30, 80.0f, HERB, 1.5},
    };
    return RecipeIndex::FromRecords(std::move(recipes), {}, {mill}, std::move(items), std::move(vendors),
                                    std::move(nodes));
}

Bot HandBot()
{
    Bot bot;
    for (Skill const skill : {Skill::Mining, Skill::Herbalism, Skill::Blacksmithing, Skill::Inscription})
        bot.skills[Id(skill)] = 100;
    bot.rates.exchange = 1.0f;
    bot.rates.factor = 1.0f;
    bot.rates.minuteValue = 100.0f;
    return bot;
}

void CheckHand()
{
    RecipeIndex const index = HandIndex();
    Bot bot = HandBot();
    PlaceRisk const refused{10.0f, 0.0f, 0.0f, 0.0f};  // 1.5 raw risk, above the refusal

    double const ore = (35.0 / (7.0 * 60.0) + 0.25) / 2.0 * 100.0;
    double const bar = ore + 1.5 / 60.0 * 100.0;
    double const thread = 50.0 / 5.0;
    {
        Pricer const pricer(index, bot);
        Cost const c = pricer.UnitCost(ORE);
        Check(c.kind == Kind::Gathered && c.via == NEAR_ZONE && Near(c.value, ore), "ore is gathered in the near zone");
        Check(pricer.UnitCost(BAR).kind == Kind::Made && Near(pricer.UnitCost(BAR).value, bar),
              "a bar costs its ore plus a cast at the global cooldown");
        Cost const t = pricer.UnitCost(THREAD);
        Check(t.kind == Kind::Bought && Near(t.value, thread) && Near(t.copperPaid, thread),
              "thread is bought at its price over the buy stack");
        Cost const b = pricer.UnitCost(BRACERS);
        Check(b.kind == Kind::Made && Near(b.value, 2 * bar + thread + 5000.0 / 60000.0 * 100.0) &&
                  Near(b.copperPaid, thread),
              "bracers cost two bars, a thread and a five second cast");
        Check(pricer.UnitCost(LIMITED).Blocked() && pricer.UnitCost(LIMITED).cause == Cause::NoSource,
              "a vendor with a limited stock is not counted on");
        Check(pricer.UnitCost(LINEN).cause == Cause::NoSource && pricer.UnitCost(123).cause == Cause::NoSource,
              "an item without a source is blocked");
        Check(pricer.UnitCost(CYCLE_A).Blocked() && pricer.UnitCost(CYCLE_A).cause == Cause::Bound,
              "a cycle of recipes ends at the depth bound");
        double const herb = (80.0 / 420.0 + 0.25) / 1.5 * 100.0;
        double const pigment = (5.0 * herb + 3000.0 / 60000.0 * 100.0) / 2.5;
        Cost const p = pricer.UnitCost(PIGMENT);
        Check(p.kind == Kind::Processed && p.via == HERB && Near(p.value, pigment),
              "a pigment costs five herbs and a mill cast over the expected yield");
        Check(pricer.UnitCost(INK).kind == Kind::Made, "ink is made from the milled pigment");

        Expansion const three = pricer.Expand(BRACERS, 3);
        bool ok = !three.Blocked() && three.lines.size() == 4 && three.lines[0].item == BRACERS &&
                  three.lines[0].quantity == 3;
        for (Line const& line : three.lines)
            ok = ok && (line.item != BAR || line.quantity == 6) && (line.item != ORE || line.quantity == 6) &&
                 (line.item != THREAD || line.quantity == 3);
        Check(ok, "three bracers expand to six bars, six ore and three thread");
        Check(pricer.Expand(BRACERS, 600).cause == Cause::Bound, "an expansion beyond the quantity bound is blocked");
        Expansion const ten = pricer.Expand(PIGMENT, 10);
        Check(!ten.Blocked() && ten.lines.size() == 2 && ten.lines[1].item == HERB && ten.lines[1].quantity == 20,
              "ten pigments take four mill casts of five herbs");
    }
    {
        Bot danger = bot;
        danger.zones[NEAR_ZONE] = refused;
        Pricer const pricer(index, danger);
        Check(pricer.UnitCost(ORE).via == SAFE_ZONE, "a refused zone leaves the safe one");
        danger.zones[SAFE_ZONE] = refused;
        Pricer const none(index, danger);
        Check(none.UnitCost(ORE).cause == Cause::TooDangerous && none.UnitCost(BRACERS).cause == Cause::TooDangerous,
              "a material found only where the bot cannot go safely is out of reach, and so is what is made of it");
        Bot deaths = bot;
        deaths.anyZone.deathsPerHour = 1.0f;
        Pricer const dying(index, deaths);
        double const weighted = 1.0 * 20.0 / 60.0;
        double const perMinute = 100.0 * (1.0 + weighted + 1.0 / 60.0 * 5.0);
        Check(Near(dying.UnitCost(ORE).value, ore / 100.0 * perMinute),
              "deaths weigh a minute by the risk term and the expected corpse run");
    }
    {
        Bot noMining = bot;
        noMining.skills.erase(Id(Skill::Mining));
        Pricer const pricer(index, noMining);
        Check(pricer.UnitCost(ORE).cause == Cause::NoSource && pricer.UnitCost(BAR).cause == Cause::NoSource,
              "a bot without the gathering skill finds no source");
        Bot noScribe = bot;
        noScribe.skills.erase(Id(Skill::Inscription));
        Check(Pricer(index, noScribe).UnitCost(PIGMENT).Blocked(), "milling needs the skill");
    }
    Check(std::string(KindWord(Kind::Fished)) == "fished" && std::string(KindWord(Kind::Dropped)) == "dropped" &&
              std::string(KindWord(Kind::Auction)) == "auction" && std::string(CauseWord(Cause::Bound)) == "bound",
          "the empty kinds and the causes have words");

    std::string const text = index.Dump("test", "core");
    RecipeIndex back;
    std::string error;
    Check(RecipeIndex::Parse(text, back, error) && back.Dump("test", "core") == text && back.Vendor(THREAD) &&
              back.Vendor(THREAD)->buyCount == 5 && back.NodesOf(ORE).size() == 2 &&
              back.YieldedBy(PIGMENT).size() == 1,
          "vendor and node rows survive the dump: " + error);
}

// ---- drop rows (WR20a) ----
double KillMinutesOf(DropRow const& row, std::uint32_t botLevel, double handleSeconds)
{
    double const level = (row.minLevel + row.maxLevel) / 2.0;
    return row.spacing / (Planner::Circuit::DEFAULT_RUN_SPEED * 60.0) + FightMinutes(level, botLevel, row.eliteShare) +
           handleSeconds / 60.0;
}

void CheckDrops()
{
    constexpr std::uint32_t BOAR = 1;
    constexpr std::uint32_t ELITE = 2;
    constexpr std::uint32_t HIGH = 3;
    constexpr std::uint32_t FRIEND = 4;
    constexpr std::uint32_t ELITE_ZONE = 42;
    constexpr std::uint32_t HIGH_ZONE = 43;
    constexpr std::uint32_t FRIEND_ZONE = 44;
    std::uint16_t const skinning = Id(Skill::Skinning);
    DropRow const boar{SAFE_ZONE, DropKind::Corpse, BOAR, LINEN, 10, 40.0f, 10, 11, 0.0f, 0, 0, 0.5, 300};
    DropRow const wolf{NEAR_ZONE, DropKind::Corpse, BOAR, LINEN, 10, 40.0f, 10, 11, 0.0f, 0, 0, 0.2, 300};
    DropRow const hide{SAFE_ZONE, DropKind::Skin, BOAR, LEATHER, 10, 40.0f, 10, 11, 0.0f, 0, skinning, 1.0, 300};
    DropRow const elite{ELITE_ZONE, DropKind::Corpse, ELITE, LINEN, 4, 40.0f, 10, 10, 1.0f, 0, 0, 2.0, 300};
    DropRow const high{HIGH_ZONE, DropKind::Corpse, HIGH, LINEN, 10, 40.0f, 20, 20, 0.0f, 0, 0, 3.0, 300};
    DropRow const friendly{FRIEND_ZONE, DropKind::Corpse, FRIEND, LINEN, 10, 40.0f, 10, 10, 0.0f, 1, 0, 5.0, 300};
    std::vector<ItemFact> items;
    for (std::uint32_t const id : {LINEN, LEATHER})
    {
        ItemFact fact;
        fact.id = id;
        fact.known = true;
        items.push_back(fact);
    }
    auto const indexOf = [&items](std::vector<DropRow> rows)
    {
        RecipeIndex index = RecipeIndex::FromRecords({}, {}, {}, items, {}, {});
        index.SetDrops(std::move(rows), {});
        return index;
    };
    RecipeIndex const index = indexOf({boar, wolf, hide, elite, high, friendly});
    Bot bot = HandBot();
    bot.level = 10;
    bot.side = SideBit(0);  // Alliance: the friendly row is friendly to it

    Cost const linen = Pricer(index, bot).UnitCost(LINEN);
    double const boarUnit = KillMinutesOf(boar, 10, LOOT_SECONDS) / boar.perKill;
    Check(linen.kind == Kind::Dropped && linen.via == SAFE_ZONE && Near(linen.minutes, boarUnit) &&
              Near(linen.value, boarUnit * 100.0) && Near(linen.copperPaid, 0.0),
          "linen drops from the boar row: walk, fight and loot over the units a kill gives, at the minute's price");
    Check(Near(KillMinutesOf(boar, 10, LOOT_SECONDS), 40.0 / 420.0 + 30.0 * (1.0 + 0.5 * 0.15) / 60.0 + 3.0 / 60.0),
          "the fight estimate: 30 s at the bot's level, 15 % more a level above");
    Check(Pricer(index, bot).UnitCost(LEATHER).cause == Cause::NoSource, "a skin is no source without skinning");
    bot.skills[skinning] = 5;
    Check(Pricer(index, bot).UnitCost(LEATHER).cause == Cause::NoSource,
          "a skin is no source below the skill its creature asks (level 11: 10)");
    bot.skills[skinning] = 10;
    Cost const leather = Pricer(index, bot).UnitCost(LEATHER);
    Check(leather.kind == Kind::Dropped && Near(leather.minutes, KillMinutesOf(hide, 10, SKIN_SECONDS)),
          "at the skill a skin is a source, its kill timed with the skin's cast");

    Bot horde = bot;
    horde.side = SideBit(1);
    Check(Pricer(index, horde).UnitCost(LINEN).via == FRIEND_ZONE,
          "a creature friendly only to the other side is a source (the cheapest here)");
    Bot unknown = bot;
    unknown.side = BOTH_SIDES;
    Check(Pricer(index, unknown).UnitCost(LINEN).via == SAFE_ZONE, "a bot of no known side takes no friendly creature");
    Bot grouped = bot;
    grouped.rates.groupSize = 5;
    Cost const eliteLinen = Pricer(index, grouped).UnitCost(LINEN);
    Check(eliteLinen.via == ELITE_ZONE &&
              Near(eliteLinen.minutes, KillMinutesOf(elite, 10, LOOT_SECONDS) / elite.perKill) &&
              KillMinutesOf(elite, 10, LOOT_SECONDS) > KillMinutesOf(boar, 10, LOOT_SECONDS),
          "a group takes the elite row, whose fight takes longer; a bot alone refuses it");
    Check(Pricer(indexOf({elite}), bot).UnitCost(LINEN).cause == Cause::TooDangerous,
          "an elite at the bot's level is too dangerous alone");
    Check(Pricer(indexOf({high}), bot).UnitCost(LINEN).cause == Cause::TooDangerous,
          "a creature 10 levels above the bot is too dangerous");
    Bot seventeen = bot;
    seventeen.level = 17;
    Check(Pricer(indexOf({high}), seventeen).UnitCost(LINEN).kind == Kind::Dropped,
          "three levels above the bot it is a source");
    Bot far = bot;
    far.zones[SAFE_ZONE] = {10.0f, 0.0f, 0.0f, 0.0f};  // refused
    Check(Pricer(index, far).UnitCost(LINEN).via == NEAR_ZONE, "a refused zone's row is passed for a safer one");
    Bot outside = bot;
    outside.reach = {NEAR_ZONE};
    Check(Pricer(index, outside).UnitCost(LINEN).via == NEAR_ZONE, "a row outside the bot's reach is no source");
    Expansion const ten = Pricer(index, bot).Expand(LINEN, 10);
    Check(!ten.Blocked() && ten.lines.size() == 1 && ten.lines[0].kind == Kind::Dropped && ten.lines[0].quantity == 10,
          "a dropped material expands to itself");
    Check(FightMinutes(12.0, 10, 0.0f) > FightMinutes(10.0, 10, 0.0f) &&
              FightMinutes(10.0, 10, 0.5f) > FightMinutes(10.0, 10, 0.0f) &&
              Near(FightMinutes(1.0, 60, 0.0f), FIGHT_SECONDS * FIGHT_FLOOR_SHARE / 60.0),
          "the fight estimate rises with level and elite share, down to its floor far below");
    Check(SkinRequirement(9) == 0 && SkinRequirement(15) == 50 && SkinRequirement(30) == 150,
          "the skin requirement by level");
}

// ---- the search ----
struct World
{
    std::vector<Recipe> recipes;
    std::vector<Process> processes;
    std::vector<VendorRow> vendors;
    std::vector<NodeRow> nodes;
    Recipes::DropWorld drops;
    std::map<std::uint32_t, std::vector<DropSource>> dropSources;

    [[nodiscard]] RecipeIndex Index() const
    {
        RecipeIndex index = RecipeIndex::FromRecords(recipes, {}, processes, {}, vendors, nodes);
        std::vector<std::uint32_t> items;
        for (auto const& [item, sources] : dropSources)
            items.push_back(item);
        auto const sourcesOf = [this](std::uint32_t item)
        {
            auto const found = dropSources.find(item);
            return found == dropSources.end() ? std::vector<DropSource>{} : found->second;
        };
        Recipes::DropBuild build = Recipes::BuildDrops(std::move(items), sourcesOf, drops, CHANCE_FLOOR);
        index.SetDrops(std::move(build.rows), std::move(build.spawns));
        return index;
    }
};

// Creatures with levels and an elite share, spawns in zones 1-3 or on an instance map, and sources of the items: kill
// loot and skins, some under the floor, quest-only or friendly to a side or both
void RandomDrops(std::mt19937& rng, World& world)
{
    auto pick = [&](int percent) { return static_cast<int>(rng() % 100) < percent; };
    auto range = [&](std::uint32_t low, std::uint32_t high) { return low + rng() % (high - low + 1); };
    auto real = [&](double low, double high) { return std::uniform_real_distribution<double>(low, high)(rng); };
    for (std::uint32_t entry = 1; entry <= DROP_KINDS; ++entry)
    {
        std::uint32_t const low = range(1, 78);
        world.drops.creatures.push_back({entry, low, low + range(0, 2), pick(15)});
        for (std::uint32_t n = range(1, 6); n; --n)
        {
            bool const instance = pick(20);
            DropSpawn spawn;
            spawn.spawnId = static_cast<std::uint32_t>(world.drops.spawns.size()) + 1;
            spawn.entry = entry;
            spawn.mapId = instance ? INSTANCE_MAP : 0;
            spawn.zone = instance ? 0 : range(1, 3);
            spawn.x = static_cast<float>(real(-300.0, 300.0));
            spawn.y = static_cast<float>(real(-300.0, 300.0));
            spawn.respawnSecs = range(60, 600);
            world.drops.spawns.push_back(spawn);
        }
    }
    world.drops.instanceMaps = {INSTANCE_MAP};
    for (std::uint32_t item = 1; item <= WORLD_ITEMS; ++item)
        if (pick(50))
            for (std::uint32_t n = range(1, 3); n; --n)
            {
                DropSource source;
                source.kind = pick(30) ? DropKind::Skin : DropKind::Corpse;
                source.entry = range(1, DROP_KINDS);
                source.chance = static_cast<float>(real(0.0, 0.6));
                source.minCount = static_cast<std::uint8_t>(range(1, 2));
                source.maxCount = static_cast<std::uint8_t>(source.minCount + range(0, 2));
                source.units = source.chance * (source.minCount + source.maxCount) / 2.0;  // a hand template
                source.questOnly = pick(5);
                source.skill = source.kind == DropKind::Skin ? Id(Skill::Skinning) : std::uint16_t{0};
                source.friendlyTo = pick(70) ? std::uint8_t{0} : static_cast<std::uint8_t>(range(1, 3));
                world.dropSources[item].push_back(source);
            }
}

World RandomWorld(std::mt19937& rng)
{
    auto pick = [&](int percent) { return static_cast<int>(rng() % 100) < percent; };
    auto range = [&](std::uint32_t low, std::uint32_t high) { return low + rng() % (high - low + 1); };
    auto real = [&](double low, double high) { return std::uniform_real_distribution<double>(low, high)(rng); };
    World world;
    std::uint32_t entry = 1;
    for (std::uint32_t item = 1; item <= WORLD_ITEMS; ++item)
    {
        if (pick(40))
            world.vendors.push_back({item, range(1, 500), range(1, 5), pick(60) ? range(1, 3) : 0u, range(0, 2)});
        if (pick(30))
            for (std::uint32_t row = range(1, 2); row; --row)
                world.nodes.push_back({range(1, 3), Id(pick(50) ? Skill::Mining : Skill::Herbalism), range(0, 300),
                                       entry++, range(0, 50), static_cast<float>(real(10.0, 200.0)), item,
                                       pick(10) ? 0.0 : real(0.2, 3.0)});
    }
    for (std::uint32_t spell = 1000 + range(0, 14); spell > 1000; --spell)
    {
        std::vector<ItemCount> reagents;
        for (std::uint32_t n = range(0, 3); n; --n)
            reagents.push_back({range(1, WORLD_ITEMS), range(1, 4)});
        world.recipes.push_back(MakeRecipe(pick(50) ? Skill::Blacksmithing : Skill::Inscription, spell,
                                           range(1, WORLD_ITEMS), std::move(reagents), range(0, 6000), range(1, 3),
                                           range(0, 300)));
    }
    for (std::uint32_t n = range(0, 3); n; --n)
    {
        Process process;
        process.kind = pick(20) ? ProcessKind::Disenchant : pick(50) ? ProcessKind::Prospect : ProcessKind::Mill;
        process.source = range(1, WORLD_ITEMS);
        process.skill = Id(Skill::Jewelcrafting);
        process.rank = range(0, 300);
        process.consumed = Recipes::PROSPECT_MILL_COUNT;
        for (std::uint32_t y = range(1, 2); y; --y)
            process.yields.push_back({range(1, WORLD_ITEMS), real(0.1, 3.0)});
        if (process.kind == ProcessKind::Disenchant)
            process.saleCopper = range(0, 2000);  // 0: a group whose sale is unknown
        world.processes.push_back(process);
    }
    RandomDrops(rng, world);
    return world;
}

Bot RandomBot(std::mt19937& rng)
{
    auto real = [&](double low, double high) { return std::uniform_real_distribution<double>(low, high)(rng); };
    Bot bot;
    for (Skill const skill :
         {Skill::Blacksmithing, Skill::Inscription, Skill::Jewelcrafting, Skill::Mining, Skill::Herbalism})
        bot.skills[Id(skill)] = static_cast<std::uint32_t>(rng() % 451);
    bot.rates.exchange = static_cast<float>(real(0.5, 2.0));
    bot.rates.factor = bot.rates.exchange * static_cast<float>(real(0.3, 3.0));
    bot.rates.minuteValue = static_cast<float>(real(10.0, 300.0));
    bot.rates.cautionWeight = static_cast<float>(real(0.5, 1.5));
    for (std::uint32_t zone = 1; zone <= 3; ++zone)
        bot.zones[zone] = {static_cast<float>(real(0.0, 4.0)), static_cast<float>(real(0.0, 0.4)),
                           static_cast<float>(real(0.0, 1.0)), static_cast<float>(real(0.0, 0.4))};
    bot.skills[Id(Skill::Skinning)] = static_cast<std::uint32_t>(rng() % 451);
    bot.level = 1 + static_cast<std::uint32_t>(rng() % 80);
    bot.side = SideBit(rng() % 2);
    bot.rates.groupSize = rng() % 5 ? 1 : 5;
    return bot;
}

std::vector<Cost> Costs(RecipeIndex const& index, Bot const& bot)
{
    Pricer const pricer(index, bot);
    std::vector<Cost> costs;
    for (std::uint32_t item = 1; item <= WORLD_ITEMS; ++item)
        costs.push_back(pricer.UnitCost(item));
    return costs;
}

bool Same(std::vector<Cost> const& a, std::vector<Cost> const& b)
{
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].kind != b[i].kind || a[i].via != b[i].via || a[i].cause != b[i].cause || a[i].value != b[i].value)
            return false;
    return true;
}

// The index with the drop rows a predicate picks left out
template <typename Leave>
RecipeIndex Without(RecipeIndex const& index, Leave const& leave)
{
    std::vector<DropRow> rows;
    for (DropRow const& row : index.Drops())
        if (!leave(row))
            rows.push_back(row);
    RecipeIndex copy = index;
    copy.SetDrops(std::move(rows), index.DropSpawns());
    return copy;
}

// Counterexamples: a skin the bot lacks the skill for is priced, or leaving those rows out changes a cost
int SkinsWithoutSkill(RecipeIndex const& index, Bot const& bot)
{
    auto const lacks = [&bot](DropRow const& row)
    {
        std::uint32_t const skill = bot.Skill(row.skill);
        return row.kind == DropKind::Skin && (!skill || skill < SkinRequirement(row.maxLevel));
    };
    int found = 0;
    for (DropRow const& row : index.Drops())
        found += lacks(row) && !DropCost(row, bot).Blocked() ? 1 : 0;
    return found + (Same(Costs(index, bot), Costs(Without(index, lacks), bot)) ? 0 : 1);
}

// Counterexamples: a creature friendly to the bot's side is priced, or leaving those rows out changes a cost
int FriendlySources(RecipeIndex const& index, Bot const& bot)
{
    auto const friendly = [&bot](DropRow const& row) { return (row.friendlyTo & bot.side) != 0; };
    int found = 0;
    for (DropRow const& row : index.Drops())
        found += friendly(row) && !DropCost(row, bot).Blocked() ? 1 : 0;
    return found + (Same(Costs(index, bot), Costs(Without(index, friendly), bot)) ? 0 : 1);
}

// Counterexamples: a row whose spawns are not its zone's open-world ones, or stand on an instance map
int OnInstance(RecipeIndex const& index)
{
    int found = 0;
    for (DropRow const& row : index.Drops())
    {
        std::span<DropSpawn const> const spawns = index.DropSpawnsOf(row.zone, row.entry);
        found += spawns.size() == row.spawns ? 0 : 1;
        for (DropSpawn const& spawn : spawns)
            found += spawn.mapId == INSTANCE_MAP ? 1 : 0;
    }
    return found;
}

// Counterexamples: a priced row in the bot's reach beats the item's cost, or a dropped cost matches no row
int NotCheapestRow(RecipeIndex const& index, Bot const& bot)
{
    Pricer const pricer(index, bot);
    int found = 0;
    for (std::uint32_t item = 1; item <= WORLD_ITEMS; ++item)
    {
        Cost const unit = pricer.UnitCost(item);
        if (unit.cause == Cause::Bound)
            continue;
        bool matched = false;
        for (std::size_t const at : index.DropsOf(item))
        {
            DropRow const& row = index.Drops()[at];
            if (!bot.Reaches(row.zone))
                continue;
            Cost const cost = DropCost(row, bot);
            found += !cost.Blocked() && Cheaper(cost, unit) ? 1 : 0;
            matched = matched || (!cost.Blocked() && row.zone == unit.via && Near(cost.value, unit.value));
        }
        found += unit.kind == Kind::Dropped && !matched ? 1 : 0;
    }
    return found;
}

// One drop row made dearer (farther apart, fewer units a kill) or more dangerous (higher, elite)
RecipeIndex WorseRow(RecipeIndex const& index, std::mt19937& rng)
{
    std::vector<DropRow> rows = index.Drops();
    DropRow& row = rows[rng() % rows.size()];
    double const more = 1.1 + static_cast<double>(rng() % 200) / 100.0;
    switch (rng() % 4)
    {
        case 0:
            row.spacing = static_cast<float>(row.spacing * more);
            break;
        case 1:
            row.perKill /= more;
            break;
        case 2:
            row.minLevel += 1 + rng() % 4;
            row.maxLevel = std::max<std::uint32_t>(row.maxLevel + 1 + rng() % 4, row.minLevel);
            break;
        default:
            row.eliteShare = 1.0f;
            break;
    }
    RecipeIndex worse = index;
    worse.SetDrops(std::move(rows), index.DropSpawns());
    return worse;
}

// The costs of b are never below those of a; blockedMayGrow allows b to block what a does not
bool NotCheaper(std::vector<Cost> const& a, std::vector<Cost> const& b, bool blockedMayGrow)
{
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].Blocked() != b[i].Blocked() && !(blockedMayGrow && b[i].Blocked()))
            return false;
        if (!a[i].Blocked() && !b[i].Blocked() && b[i].value < a[i].value - 1e-9 * std::max(1.0, a[i].value))
            return false;
    }
    return true;
}

Bot MoreDangerous(Bot bot, std::mt19937& rng)
{
    PlaceRisk& risk = bot.zones[1 + rng() % 3];
    float const more = 0.1f + static_cast<float>(rng() % 30) / 10.0f;
    switch (rng() % 4)
    {
        case 0:
            risk.levelsAbove += more;
            break;
        case 1:
            risk.eliteShare += more / 10.0f;
            break;
        case 2:
            risk.deathsPerHour += more;
            break;
        default:
            risk.enemyShare += more / 10.0f;
            break;
    }
    return bot;
}

// Professions WR14d: an enchanter's disenchant is a source of its yields, one cast at the sale given up (the group's
// median sale at a copper's neutral worth) and its cast, spread over every unit the cast gives; no expansion below it
void CheckDisenchant()
{
    constexpr std::uint32_t DUST = 10940;
    constexpr std::uint32_t ESSENCE = 10938;
    constexpr std::uint32_t GROUP = 1;
    Process disenchant;
    disenchant.kind = ProcessKind::Disenchant;
    disenchant.source = GROUP;
    disenchant.skill = Id(Skill::Enchanting);
    disenchant.rank = 1;
    disenchant.yields = {{DUST, 1.2}, {ESSENCE, 0.3}};
    disenchant.saleCopper = 187;
    std::vector<ItemFact> items;
    for (std::uint32_t const id : {DUST, ESSENCE})
    {
        ItemFact fact;
        fact.id = id;
        fact.known = true;
        items.push_back(fact);
    }
    RecipeIndex const index = RecipeIndex::FromRecords({}, {}, {disenchant}, items, {}, {});
    Bot enchanter = HandBot();
    enchanter.skills[Id(Skill::Enchanting)] = 1;
    double const each = (187.0 + 3000.0 / 60000.0 * 100.0) / 1.5;
    Cost const dust = Pricer(index, enchanter).UnitCost(DUST);
    Check(dust.kind == Kind::Processed && dust.via == GROUP && Near(dust.value, each) && Near(dust.copperPaid, 0.0),
          "an enchanter's dust costs the gear's sale and a cast over all the units a disenchant gives");
    Check(Near(Pricer(index, enchanter).UnitCost(ESSENCE).value, each), "every unit of a disenchant costs the same");
    Bot poor = enchanter;
    poor.rates.factor = 3.0f;
    Check(Near(Pricer(index, poor).UnitCost(DUST).value, each),
          "the sale given up is weighed at a copper's neutral worth, not the need factor");
    Expansion const ten = Pricer(index, enchanter).Expand(DUST, 10);
    Check(!ten.Blocked() && ten.lines.size() == 1 && ten.lines[0].quantity == 10,
          "the expansion stops at a disenchanted material (the gear is got anyway)");
    Check(Pricer(index, HandBot()).UnitCost(DUST).cause == Cause::NoSource,
          "a bot without enchanting has no disenchant source");
    disenchant.rank = 25;
    RecipeIndex const high = RecipeIndex::FromRecords({}, {}, {disenchant}, items, {}, {});
    Check(Pricer(high, enchanter).UnitCost(DUST).Blocked(), "a group above the bot's skill is no source");
    disenchant.rank = 1;
    disenchant.saleCopper = 0;
    RecipeIndex const unknown = RecipeIndex::FromRecords({}, {}, {disenchant}, items, {}, {});
    Check(Pricer(unknown, enchanter).UnitCost(DUST).Blocked(), "a disenchant without a known sale is no source");
    std::string const text = index.Dump("test", "core");
    RecipeIndex back;
    std::string error;
    Check(RecipeIndex::Parse(text, back, error) && back.Processes().size() == 1 &&
              back.Processes()[0].saleCopper == 187 && back.Dump("test", "core") == text,
          "a disenchant's sale survives the dump");
}

void Search()
{
    std::mt19937 rng(SEED);
    int unsourced = 0;
    int freeOrUnsourced = 0;
    int bounded = 0;
    int dearer = 0;
    int dangerCheaper = 0;
    int poorerCheaper = 0;
    int orderDiffers = 0;
    int expandBroken = 0;
    int controlCheaper = 0;
    int dropRows = 0;
    int droppedChosen = 0;
    int worseRowCheaper = 0;
    int skinWithoutSkill = 0;
    int friendlySource = 0;
    int onInstance = 0;
    int notCheapest = 0;
    int controlWorseRow = 0;
    int controlSkin = 0;
    int controlFriendly = 0;
    int controlInstance = 0;
    int controlCheapest = 0;
    for (int run = 0; run < SEARCH_WORLDS; ++run)
    {
        World world = RandomWorld(rng);
        RecipeIndex const index = world.Index();
        Bot const bot = RandomBot(rng);
        std::vector<Cost> const costs = Costs(index, bot);
        Pricer const pricer(index, bot);
        for (std::uint32_t item = 1; item <= WORLD_ITEMS; ++item)
        {
            Cost const& cost = costs[item - 1];
            VendorRow const* vendor = index.Vendor(item);
            bool const source = (vendor && vendor->stocked && vendor->buyCopper) || !index.NodesOf(item).empty() ||
                                !index.MadeBy(item).empty() || !index.YieldedBy(item).empty() ||
                                !index.DropsOf(item).empty();
            unsourced += source ? 0 : 1;
            if ((!source && !cost.Blocked()) || (cost.Blocked() && cost.cause == Cause::None) ||
                (!cost.Blocked() && !(cost.value > 0.0 && std::isfinite(cost.value))))
                ++freeOrUnsourced;
            bounded += cost.cause == Cause::Bound ? 1 : 0;
            droppedChosen += cost.kind == Kind::Dropped ? 1 : 0;
            if (!cost.Blocked())
            {
                Expansion const one = pricer.Expand(item, 1);
                if (one.lines.empty() || one.lines[0].item != item || (one.Blocked() && one.cause != Cause::Bound))
                    ++expandBroken;
            }
        }

        Bot rate = bot;
        rate.rates.minuteValue *= 1.0f + static_cast<float>(rng() % 200) / 100.0f;
        dearer += NotCheaper(costs, Costs(index, rate), false) ? 0 : 1;
        Bot const danger = MoreDangerous(bot, rng);
        dangerCheaper += NotCheaper(costs, Costs(index, danger), true) ? 0 : 1;
        Bot poorer = bot;
        poorer.rates.factor *= 1.1f + static_cast<float>(rng() % 200) / 100.0f;
        poorerCheaper += NotCheaper(costs, Costs(index, poorer), false) ? 0 : 1;

        // WR20a: the drop rows' invariants, each with a rule broken on purpose that the same check must catch
        dropRows += static_cast<int>(index.Drops().size());
        auto const faulty = [&bot](Fault fault)
        {
            Bot broken = bot;
            broken.fault = fault;
            return broken;
        };
        if (!index.Drops().empty())
        {
            RecipeIndex const worse = WorseRow(index, rng);
            worseRowCheaper += NotCheaper(costs, Costs(worse, bot), true) ? 0 : 1;
            Bot const danger = faulty(Fault::DangerCheaper);
            controlWorseRow += NotCheaper(Costs(index, danger), Costs(worse, danger), true) ? 0 : 1;
        }
        skinWithoutSkill += SkinsWithoutSkill(index, bot);
        controlSkin += SkinsWithoutSkill(index, faulty(Fault::IgnoreSkinSkill));
        friendlySource += FriendlySources(index, bot);
        controlFriendly += FriendlySources(index, faulty(Fault::IgnoreSide));
        onInstance += OnInstance(index);
        World blind = world;
        blind.drops.instanceMaps.clear();  // a build that does not know the instance maps
        controlInstance += OnInstance(blind.Index());
        notCheapest += NotCheapestRow(index, bot);
        controlCheapest += NotCheapestRow(index, faulty(Fault::FirstDrop));

        std::shuffle(world.recipes.begin(), world.recipes.end(), rng);
        std::shuffle(world.processes.begin(), world.processes.end(), rng);
        std::shuffle(world.nodes.begin(), world.nodes.end(), rng);
        std::shuffle(world.vendors.begin(), world.vendors.end(), rng);
        std::shuffle(world.drops.spawns.begin(), world.drops.spawns.end(), rng);
        for (auto& [item, sources] : world.dropSources)
            std::shuffle(sources.begin(), sources.end(), rng);
        std::vector<Cost> const shuffled = Costs(world.Index(), bot);
        for (std::size_t i = 0; i < costs.size(); ++i)
            if (costs[i].kind != shuffled[i].kind || costs[i].via != shuffled[i].via ||
                costs[i].cause != shuffled[i].cause || costs[i].value != shuffled[i].value)
            {
                ++orderDiffers;
                break;
            }

        // Negative control: a caution of the wrong sign turns the risk term into a discount
        Bot wrong = bot;
        wrong.rates.cautionWeight = -0.9f;
        Bot wrongDanger = danger;
        wrongDanger.rates.cautionWeight = -0.9f;
        controlCheaper += NotCheaper(Costs(index, wrong), Costs(index, wrongDanger), true) ? 0 : 1;
    }
    std::cout << "search: " << SEARCH_WORLDS << " worlds, " << unsourced << " items without a source, " << bounded
              << " ended at a bound; counterexamples: " << freeOrUnsourced << " free or unsourced, " << dearer
              << " cheaper at a dearer minute, " << dangerCheaper << " cheaper in danger, " << poorerCheaper
              << " cheaper when poorer, " << orderDiffers << " order-dependent, " << expandBroken
              << " broken expansions; " << controlCheaper
              << " cheaper in danger with the wrong sign (negative control)\n";
    Check(unsourced > SEARCH_WORLDS && bounded > 0, "the search reaches unsourced items and the bounds");
    Check(freeOrUnsourced == 0, "a material without a source is blocked, never free");
    Check(dearer == 0, "cost rises with the price of a minute");
    Check(dangerCheaper == 0, "a more dangerous place never makes a material cheaper");
    Check(poorerCheaper == 0, "a bought material never costs a poorer bot less");
    Check(orderDiffers == 0, "the answer does not depend on the order of the rows");
    Check(expandBroken == 0, "a priced item expands from itself");
    Check(controlCheaper > 0, "with the wrong sign the search finds a counterexample (negative control)");

    std::cout << "drop search: " << dropRows << " drop rows, " << droppedChosen
              << " items dropped; counterexamples: " << worseRowCheaper
              << " cheaper by a dearer or more dangerous row, " << skinWithoutSkill << " skins without the skill, "
              << onInstance << " on an instance map, " << notCheapest << " not the cheapest row, " << friendlySource
              << " friendly sources; negative controls: " << controlWorseRow << " danger cheaper, " << controlSkin
              << " skill ignored, " << controlInstance << " instance maps unknown, " << controlCheapest
              << " first row taken, " << controlFriendly << " side ignored\n";
    Check(dropRows > SEARCH_WORLDS && droppedChosen > 0, "the search reaches drop rows and dropped materials");
    Check(worseRowCheaper == 0, "WR20a: a dearer or more dangerous drop row never makes a material cheaper");
    Check(skinWithoutSkill == 0, "WR20a: a skin is never offered without the skill its creature asks");
    Check(onInstance == 0, "WR20a: no drop row stands on an instance map");
    Check(notCheapest == 0, "WR20a: the chosen drop row is the cheapest");
    Check(friendlySource == 0, "WR20a: a creature friendly to the bot's side is never a source");
    Check(controlWorseRow > 0, "negative control: a fight estimate that falls with danger is caught");
    Check(controlSkin > 0, "negative control: skins offered without the skill are caught");
    Check(controlInstance > 0, "negative control: a build blind to instance maps is caught");
    Check(controlCheapest > 0, "negative control: taking the first drop row is caught");
    Check(controlFriendly > 0, "negative control: ignoring the bot's side is caught");
}

// ---- the market (auction house WA7a): the auction as a source, and what a listing fetches ----
using Prices = std::map<std::uint32_t, std::uint32_t>;  // item -> typical unit price

constexpr std::uint32_t MARKET_SEED = 20261126;
constexpr int MARKET_WORLDS = 2000;
constexpr std::uint32_t MARKET_PRICE_MAX = 600;  // copper, around the random worlds' vendor and gathering costs

Bot WithMarket(Bot bot, Prices const& prices)
{
    auto const shared = std::make_shared<Prices const>(prices);
    bot.auctionCopper = [shared](std::uint32_t item)
    {
        auto const found = shared->find(item);
        return found == shared->end() ? 0u : found->second;
    };
    return bot;
}

void CheckAuction()
{
    RecipeIndex const index = HandIndex();
    Bot const bot = HandBot();
    double const ore = (35.0 / (7.0 * 60.0) + 0.25) / 2.0 * 100.0;  // gathered in the near zone, as in CheckHand
    Bot const market = WithMarket(bot, {{ORE, 10}, {THREAD, 10}, {LINEN, 30}, {BRACERS, 1000000}});
    Pricer const pricer(index, market);
    Cost const o = pricer.UnitCost(ORE);
    Check(o.kind == Kind::Auction && o.via == ORE && Near(o.value, 10.0) && Near(o.copperPaid, 10.0) && 10.0 < ore,
          "WA7a: ore the market sells below its gathering cost is bought at auction, at the typical price");
    Cost const bar = pricer.UnitCost(BAR);
    Check(bar.kind == Kind::Made && Near(bar.value, 10.0 + 1.5 / 60.0 * 100.0) && Near(bar.copperPaid, 10.0),
          "WA7a: a bar is made from ore bought at auction");
    Check(pricer.UnitCost(THREAD).kind == Kind::Bought, "WA7a: at the vendor's price the vendor wins the tie");
    Cost const linen = pricer.UnitCost(LINEN);
    Check(linen.kind == Kind::Auction && Near(linen.value, 30.0),
          "WA7a: an item without another source is bought at auction");
    Check(pricer.UnitCost(BRACERS).kind == Kind::Made, "WA7a: a dear auction price leaves the cheaper source");
    Check(pricer.UnitCost(LIMITED).Blocked() && pricer.UnitCost(123).Blocked(), "WA7a: no price, no auction source");
    Expansion const three = pricer.Expand(BRACERS, 3);
    bool ok = !three.Blocked();
    for (Line const& line : three.lines)
        ok = ok && (line.item != ORE || (line.kind == Kind::Auction && line.via == ORE && line.quantity == 6));
    Check(ok, "WA7a: three bracers expand to six ore bought at auction");
    Bot poor = market;
    poor.rates.factor = 3.0f;
    Pricer const poorPricer(index, poor);
    Check(poorPricer.UnitCost(ORE).kind == Kind::Gathered && Near(poorPricer.UnitCost(LINEN).value, 90.0),
          "WA7a: a poor bot weighs the auction price by its need factor: it gathers the ore");

    // The view reads the typical price only; no view, an empty one or an item without a price is no market
    auto snapshot = std::make_shared<AuctionIndex::Snapshot>();
    snapshot->items[ORE].typical = {10, 4, 4.0, 0};
    snapshot->items[LINEN].offers.cheapestUnitBuyout = 5;  // offers only: the current offers are not read (WA.4)
    Market::View const view{snapshot, {}, {}};
    Bot viewBot = bot;
    viewBot.auctionCopper = view.BuyCopperOf();
    Pricer const viewPricer(index, viewBot);
    Check(viewPricer.UnitCost(ORE).kind == Kind::Auction && viewPricer.UnitCost(LINEN).Blocked(),
          "WA7a: the market view reads the typical price, never the current offers");
    Bot emptyBot = bot;
    emptyBot.auctionCopper = Market::View{std::make_shared<AuctionIndex::Snapshot>(), {}, {}}.BuyCopperOf();
    Check(Market::View{}.Empty() && !Market::View{}.BuyCopperOf() && !emptyBot.auctionCopper,
          "WA7a: no snapshot, or an empty one, is no market");
    bool same = true;
    Pricer const plain(index, bot);
    Bot const zeroBot = WithMarket(bot, {{ORE, 0}});
    Pricer const unpriced(index, zeroBot);
    for (std::uint32_t const item : {ORE, BAR, BRACERS, THREAD, LIMITED, LINEN, HERB, PIGMENT, INK, CYCLE_A})
    {
        Cost const a = plain.UnitCost(item);
        Cost const b = unpriced.UnitCost(item);
        same = same && a.kind == b.kind && a.via == b.via && a.cause == b.cause && a.value == b.value;
    }
    Check(same, "WA7a: a market without a price for the materials leaves every cost as the vendor-only one");
}

void CheckProceeds()
{
    AuctionRules::Fees const shared;  // the shared house's row as WA.9 sets it: deposit 5 %, cut 5 %, rates 1
    AuctionConfig const config;
    AuctionIndex::TypicalPrice const typical{1000, 9, 9.0, 1};
    Check(Market::ProceedsEach({}, 250, 20, shared, config) == 0.0, "WA7a: no typical price, no proceeds");
    // 20 units at 1000c, 24 h: ask 20000, cut 1000, deposit 1500, V 5000, p 10/12: 16416 over 20 units
    double const each = Market::ProceedsEach(typical, 250, 20, shared, config);
    Check(Near(each, 820.8), "WA7a: proceeds are the listing's expected value less cut and deposit, per unit");
    Check(Market::ProceedsEach({2, 3, 3.0, 0}, 1, 1, shared, config) == 0.0,
          "WA7a: a listing that does not beat the vendor fetches nothing at auction (the vendor is the outlet)");
    int belowVendor = 0;
    int falls = 0;
    int listed = 0;
    for (std::uint32_t const lot : {1u, 5u, 20u})
        for (std::uint32_t sell = 0; sell <= 600; sell += 37)
        {
            double before = 0.0;
            for (std::uint32_t unit = 1; unit <= 3000; unit += 7)
            {
                double const proceeds = Market::ProceedsEach({unit, 3, 3.0, 1}, sell, lot, shared, config);
                listed += proceeds > 0.0 ? 1 : 0;
                belowVendor += proceeds > 0.0 && proceeds < sell ? 1 : 0;
                falls += proceeds < before ? 1 : 0;
                before = proceeds;
            }
        }
    std::cout << "proceeds: " << listed << " listings worth more than the vendor; counterexamples: " << belowVendor
              << " below the vendor's price, " << falls << " lower at a higher typical price\n";
    Check(listed > 0 && belowVendor == 0 && falls == 0,
          "WA7a: proceeds are 0 or at least the vendor's price, and never fall as the typical price rises");
}

// Counterexamples of the auction as a source in one world
struct AuctionChoice
{
    int chosen{0};
    int unpriced{0};    // taken without a price
    int notCheaper{0};  // taken while another source is as cheap
    int passedOver{0};  // not taken while strictly cheaper
    int misweighed{0};  // not costing the price times the need factor
};

AuctionChoice ChoiceOf(RecipeIndex const& index, Bot const& bot, Prices const& prices)
{
    AuctionChoice found;
    std::vector<Cost> const costs = Costs(index, WithMarket(bot, prices));
    for (std::uint32_t item = 1; item <= WORLD_ITEMS; ++item)
    {
        Cost const& cost = costs[item - 1];
        if (cost.cause == Cause::Bound)
            continue;
        auto const priced = prices.find(item);
        std::uint32_t const price = priced == prices.end() ? 0 : priced->second;
        double const weighed = static_cast<double>(price) * bot.rates.factor;
        if (cost.kind == Kind::Auction)
        {
            Prices others = prices;
            others.erase(item);
            Bot const otherBot = WithMarket(bot, others);
            Cost const other = Pricer(index, otherBot).UnitCost(item);  // every source but this item's auction
            ++found.chosen;
            found.unpriced += price ? 0 : 1;
            found.notCheaper += Cheaper(cost, other) ? 0 : 1;
            found.misweighed += Near(cost.copperPaid, price) && Near(cost.value, weighed) ? 0 : 1;
        }
        else if (price)
        {
            Cost offer;
            offer.kind = Kind::Auction;
            offer.cause = Cause::None;
            offer.copperPaid = price;
            offer.value = weighed;
            offer.via = item;
            found.passedOver += Cheaper(offer, cost) ? 1 : 0;
        }
    }
    return found;
}

// 1 when the poorer bot pays more copper for some material than the richer one
int MoreCopper(std::vector<Cost> const& rich, std::vector<Cost> const& poor)
{
    for (std::size_t i = 0; i < rich.size(); ++i)
        if (!rich[i].Blocked() && !poor[i].Blocked() &&
            poor[i].copperPaid > rich[i].copperPaid + 1e-9 * std::max(1.0, rich[i].copperPaid))
            return 1;
    return 0;
}

void MarketSearch()
{
    std::mt19937 rng(MARKET_SEED);  // its own stream: the searches above print as before WA7a
    AuctionChoice total;
    AuctionChoice controlFirst;
    AuctionChoice controlUnpriced;
    int emptyDiffers = 0;
    int dearerCheaper = 0;
    int poorerCheaper = 0;
    int poorerMoreCopper = 0;
    int controlEmpty = 0;
    int controlInverse = 0;
    int controlUnweighed = 0;
    auto add = [](AuctionChoice& sum, AuctionChoice const& one)
    {
        sum.chosen += one.chosen;
        sum.unpriced += one.unpriced;
        sum.notCheaper += one.notCheaper;
        sum.passedOver += one.passedOver;
        sum.misweighed += one.misweighed;
    };
    for (int run = 0; run < MARKET_WORLDS; ++run)
    {
        RecipeIndex const index = RandomWorld(rng).Index();
        Bot const bot = RandomBot(rng);
        Prices prices;
        for (std::uint32_t item = 1; item <= WORLD_ITEMS; ++item)
            if (rng() % 2)
                prices[item] = 1 + static_cast<std::uint32_t>(rng() % MARKET_PRICE_MAX);
        auto const faulty = [&bot](Fault fault)
        {
            Bot broken = bot;
            broken.fault = fault;
            return broken;
        };
        std::vector<Cost> const vendorOnly = Costs(index, bot);
        std::vector<Cost> const market = Costs(index, WithMarket(bot, prices));

        add(total, ChoiceOf(index, bot, prices));
        add(controlFirst, ChoiceOf(index, faulty(Fault::AuctionFirst), prices));
        add(controlUnpriced, ChoiceOf(index, faulty(Fault::AuctionUnpriced), prices));

        emptyDiffers += Same(vendorOnly, Costs(index, WithMarket(bot, {}))) ? 0 : 1;
        controlEmpty += Same(vendorOnly, Costs(index, WithMarket(faulty(Fault::AuctionUnpriced), {}))) ? 0 : 1;

        if (!prices.empty())
        {
            Prices dearer = prices;
            std::uint32_t& raised = std::next(dearer.begin(), static_cast<long>(rng() % dearer.size()))->second;
            raised = static_cast<std::uint32_t>(raised * (1.1 + static_cast<double>(rng() % 200) / 100.0)) + 1;
            dearerCheaper += NotCheaper(market, Costs(index, WithMarket(bot, dearer)), false) ? 0 : 1;
            Bot const inverse = faulty(Fault::AuctionInverse);
            controlInverse +=
                NotCheaper(Costs(index, WithMarket(inverse, prices)), Costs(index, WithMarket(inverse, dearer)), false)
                    ? 0
                    : 1;
        }

        float const poorer = 1.1f + static_cast<float>(rng() % 200) / 100.0f;
        Bot poor = bot;
        poor.rates.factor *= poorer;
        std::vector<Cost> const poorMarket = Costs(index, WithMarket(poor, prices));
        poorerCheaper += NotCheaper(market, poorMarket, false) ? 0 : 1;
        poorerMoreCopper += MoreCopper(market, poorMarket);
        Bot const unweighed = faulty(Fault::AuctionUnweighed);
        Bot poorUnweighed = unweighed;
        poorUnweighed.rates.factor *= poorer;
        controlUnweighed +=
            MoreCopper(Costs(index, WithMarket(unweighed, prices)), Costs(index, WithMarket(poorUnweighed, prices)));
    }
    std::cout << "market search: " << MARKET_WORLDS << " worlds, " << total.chosen
              << " materials bought at auction; counterexamples: " << total.unpriced << " taken without a price, "
              << total.notCheaper << " taken while not strictly cheaper, " << total.passedOver
              << " strictly cheaper but passed over, " << total.misweighed << " not the price x the need factor, "
              << emptyDiffers << " changed by an empty market, " << dearerCheaper
              << " cheaper at a dearer auction price, " << poorerCheaper << " cheaper when poorer, " << poorerMoreCopper
              << " more copper when poorer; negative controls: " << controlFirst.notCheaper << " auction taken first, "
              << controlUnpriced.unpriced << " unpriced offered, " << controlEmpty << " empty market offering, "
              << controlInverse << " inverse price, " << controlUnweighed << " unweighed by need\n";
    Check(total.chosen > MARKET_WORLDS && total.misweighed == 0,
          "WA7a: the search buys materials at auction, at the price times the need factor");
    Check(total.unpriced == 0 && total.notCheaper == 0 && total.passedOver == 0,
          "WA7a: the auction is taken only with a price and only when strictly cheaper; the cheapest source wins");
    Check(emptyDiffers == 0, "WA7a: with an empty market every cost is the vendor-only one");
    Check(dearerCheaper == 0, "WA7a: a dearer auction price never makes a material cheaper");
    Check(poorerCheaper == 0 && poorerMoreCopper == 0,
          "WA7a: with a market a poorer bot never pays less value or more copper for a material");
    Check(controlFirst.notCheaper > 0, "negative control: taking the auction first is caught");
    Check(controlUnpriced.unpriced > 0 && controlEmpty > 0, "negative control: offering unpriced items is caught");
    Check(controlInverse > 0, "negative control: an auction cost falling with its price is caught");
    Check(controlUnweighed > 0, "negative control: an auction price not weighed by the need factor is caught");
}

// ---- the dumped fixture's tables ----
struct Variant
{
    char const* name;
    PlaceRisk risk;
    float factor;  // times the exchange
};

constexpr float SAMPLE_MINUTE = 100.0f;  // a minute is worth a silver to the sample bot
constexpr std::uint32_t SAMPLE_SKILL = 450;
constexpr std::uint32_t SAMPLE_LEVEL = 80;
constexpr std::size_t NAME_WIDTH = 28;

std::string Cell(Cost const& cost, Rates const& rates)
{
    std::ostringstream out;
    if (cost.Blocked())
        out << "blocked:" << CauseWord(cost.cause);
    else
        out << KindWord(cost.kind) << ' ' << std::fixed << std::setprecision(1) << cost.CopperWorth(rates) << 'c';
    return out.str();
}

void Tables(std::string const& root)
{
    std::ifstream in(root + "/tests/fixtures/recipe-index-dump.txt", std::ios::binary);
    std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    RecipeIndex index;
    std::string error;
    Check(RecipeIndex::Parse(text, index, error), "the dumped fixture parses: " + error);
    std::cout << "fixture: " << text.substr(0, text.find('\n')) << "; " << index.Vendors().size() << " vendor rows, "
              << index.Nodes().size() << " node rows\n";

    Variant const variants[] = {{"safe/poor", {}, 3.0f},
                                {"safe/flush", {}, 0.5f},
                                {"danger/poor", {3.0f, 0.3f, 0.0f, 0.0f}, 3.0f},
                                {"danger/flush", {3.0f, 0.3f, 0.0f, 0.0f}, 0.5f}};
    std::vector<Pricer> pricers;
    std::vector<Bot> bots;
    bots.reserve(std::size(variants));
    for (Variant const& variant : variants)
    {
        Bot bot;
        for (ProfessionRules::ProfessionInfo const& profession : ProfessionRules::PROFESSIONS)
            bot.skills[Id(profession.Which)] = SAMPLE_SKILL;
        bot.rates.exchange = 1.0f;
        bot.rates.factor = variant.factor;
        bot.rates.minuteValue = SAMPLE_MINUTE;
        bot.anyZone = variant.risk;
        bot.level = SAMPLE_LEVEL;
        bot.side = SideBit(0);
        bots.push_back(bot);
    }
    for (Bot const& bot : bots)
        pricers.emplace_back(index, bot);

    std::map<std::string, int> kinds;
    int rows = 0;
    int broken = 0;
    for (ProfessionRules::ProfessionInfo const& profession : ProfessionRules::PROFESSIONS)
    {
        std::set<std::uint32_t> base;
        for (Recipe const& recipe : index.Recipes())
            if (recipe.skill == Id(profession.Which))
                for (ItemCount const& reagent : recipe.reagents)
                    if (index.MadeBy(reagent.item).empty())
                        base.insert(reagent.item);
        if (base.empty())
            continue;
        std::cout << "== " << profession.Name << ": " << base.size() << " base materials (item, name";
        for (Variant const& variant : variants)
            std::cout << ", " << variant.name;
        std::cout << ")\n";
        for (std::uint32_t const item : base)
        {
            ItemFact const* fact = index.Item(item);
            std::string name = fact ? fact->name : "?";
            name.resize(NAME_WIDTH, ' ');
            std::cout << std::setw(6) << item << ' ' << name;
            std::vector<Cost> cells;
            for (std::size_t v = 0; v < pricers.size(); ++v)
            {
                cells.push_back(pricers[v].UnitCost(item));
                std::cout << " | " << std::left << std::setw(20) << Cell(cells.back(), bots[v].rates) << std::right;
            }
            std::cout << '\n';
            ++rows;
            ++kinds[cells[1].Blocked() ? std::string("blocked:") + CauseWord(cells[1].cause) : KindWord(cells[1].kind)];
            auto const dearer = [](Cost const& low, Cost const& high)
            { return low.Blocked() ? high.Blocked() : high.Blocked() || high.value >= low.value - 1e-9; };
            for (Cost const& cell : cells)
                broken += (cell.Blocked() && cell.cause == Cause::None) || (!cell.Blocked() && !(cell.value > 0.0));
            // danger never cheaper than safe, poor never cheaper than flush
            broken += !dearer(cells[0], cells[2]) || !dearer(cells[1], cells[3]);
            broken += !dearer(cells[1], cells[0]) || !dearer(cells[3], cells[2]);
        }
    }
    std::cout << "fixture tables: " << rows << " rows; safe/flush by kind:";
    for (auto const& [kind, count] : kinds)
        std::cout << ' ' << kind << '=' << count;
    std::cout << '\n';
    Check(rows > 100, "the fixture's professions have base materials");
    std::cout << "fixture drops: " << index.Drops().size() << " rows; safe/flush linen "
              << Cell(pricers[1].UnitCost(LINEN), bots[1].rates) << ", light leather "
              << Cell(pricers[1].UnitCost(LEATHER), bots[1].rates) << ", boar meat "
              << Cell(pricers[1].UnitCost(BOAR_MEAT), bots[1].rates) << '\n';
    Check(!index.Drops().empty() && pricers[1].UnitCost(LINEN).kind == Kind::Dropped &&
              pricers[1].UnitCost(LEATHER).kind == Kind::Dropped &&
              pricers[1].UnitCost(BOAR_MEAT).kind == Kind::Dropped,
          "WR20a: on the fixture cloth, leather and meat drop");
    Check(broken == 0, "on the fixture danger and poverty never make a material cheaper, and nothing is free");
}
}  // namespace

int main(int argc, char** argv)
{
    CheckHand();
    CheckDisenchant();
    CheckDrops();
    CheckAuction();
    CheckProceeds();
    Search();
    MarketSearch();
    Tables(argc > 1 ? argv[1] : ".");
    std::cout << "MATERIAL rules selftest: " << passed + failed << " checks, " << failed << " failed ("
              << (failed ? "FAIL" : "PASS") << ")\n";
    return failed ? 1 : 0;
}
