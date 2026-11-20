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
// material's cheapest source and cost for a safe and a dangerous zone and a poor and a flush bot.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "MaterialRules.h"
#include "ProfessionRules.h"
#include "RecipeIndex.h"

using namespace Materials;
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

// ---- the search ----
struct World
{
    std::vector<Recipe> recipes;
    std::vector<Process> processes;
    std::vector<VendorRow> vendors;
    std::vector<NodeRow> nodes;

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
        world.processes.push_back(process);
    }
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
                                !index.MadeBy(item).empty() || !index.YieldedBy(item).empty();
            unsourced += source ? 0 : 1;
            if ((!source && !cost.Blocked()) || (cost.Blocked() && cost.cause == Cause::None) ||
                (!cost.Blocked() && !(cost.value > 0.0 && std::isfinite(cost.value))))
                ++freeOrUnsourced;
            bounded += cost.cause == Cause::Bound ? 1 : 0;
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

        std::shuffle(world.recipes.begin(), world.recipes.end(), rng);
        std::shuffle(world.processes.begin(), world.processes.end(), rng);
        std::shuffle(world.nodes.begin(), world.nodes.end(), rng);
        std::shuffle(world.vendors.begin(), world.vendors.end(), rng);
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
    Check(broken == 0, "on the fixture danger and poverty never make a material cheaper, and nothing is free");
}
}  // namespace

int main(int argc, char** argv)
{
    CheckHand();
    Search();
    Tables(argc > 1 ? argv[1] : ".");
    std::cout << "MATERIAL rules selftest: " << passed + failed << " checks, " << failed << " failed ("
              << (failed ? "FAIL" : "PASS") << ")\n";
    return failed ? 1 : 0;
}
