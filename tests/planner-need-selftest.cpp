/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone harness of the need factor and sickness in the planner (upkeep WU16): upkeep's needs list
// (UpkeepNeedRules.h) bridged into the goal model (NeedGoalBridge.h, GoalModel.h). Named scenarios print their ranked
// options with the reasons: a bot 12 g short at 39 for journeyman riding earns, and quests again once the rank is
// bought; a bot with its reserve covered quests on; a frugal bot saves earlier than a spendthrift; a sick bot rests.
// Then an invariant search over generated snapshots (seed and count printed, the first counterexample shown):
//  I1 lowering the gold never lowers an earning option's value per minute and never raises the circuit's; a broke bot
//     with a shortfall ranks the copper-heavy spot first as money-making, reason gold_short
//  I2 a bot without a shortfall has no gold_short option and is not short of the reserve
//  I3 a sick bot's first option is Rest, reason sick, and no option fights or walks to a fight
//  I4 every option has a reason and finite values, and the ranking is deterministic
// With `--repo <module root>` the skill path live (professions WR11, SkillPathBridge.h) on the dumped recipe index:
// a worthwhile stretch yields needs and a queue, one that is not yields none; a poorer bot's stretch spends no more
// copper; the same snapshot gives the same path (the worker's and the bot thread's); the cost per plan, timed.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "AuctionBuyList.h"
#include "GoalModel.h"
#include "GoalModelHarness.h"
#include "NeedGoalBridge.h"
#include "SkillPathBridge.h"
#include "upkeep-fixture.h"

namespace
{
using Goals::GoalKind;
using Goals::Option;
using Goals::Ranked;
using Goals::Situation;

constexpr std::uint32_t SPOT_XP = 11;
constexpr std::uint32_t SPOT_GOLD = 12;
constexpr float CIRCUIT_MINUTES = 20.0f;
constexpr float CIRCUIT_XP_SHARE = 0.95f;       // the circuit's rate: this share of the band's XP per minute
constexpr float GOLD_SPOT_XP_SHARE = 0.3f;      // the copper-heavy spot: little XP ...
constexpr float GOLD_SPOT_COPPER_SHARE = 0.6f;  // ... and copper worth 0.6 of the band rate at the reserve
constexpr float XP_SPOT_XP_SHARE = 0.8f;
constexpr float XP_SPOT_COPPER_SHARE = 0.05f;
constexpr std::uint32_t SICK_MS = 5 * 60 * 1000;
constexpr float WALK_MINUTES_PER_HOUR = 50.0f;    // a bot that walks most of its hour ...
constexpr float EARN_COPPER_PER_MINUTE = 250.0f;  // ... and earns well: journeyman riding (60 g) pays for itself at 39
constexpr std::uint64_t DEFAULT_SEED = 0x5EEDC0FFEEULL;
constexpr int DEFAULT_SNAPSHOTS = 20000;
constexpr float TOLERANCE = 1e-4f;

float BandXp(std::uint8_t level) { return Goals::BAND_XP_PER_MINUTE[WorldTravel::LevelBand(level)]; }

float BandEarn(std::uint8_t level)
{
    return static_cast<float>(WorldTravel::DEFAULT_SLOT_BANDS[WorldTravel::LevelBand(level)].EarnCopperPerMinute);
}

// The named scenarios' rates, as the bot's slot measure would give them
NeedRates Walker(std::uint8_t level)
{
    NeedRates rates = Rates(level, WALK_MINUTES_PER_HOUR);
    rates.earnCopperPerMinute = EARN_COPPER_PER_MINUTE;
    return rates;
}

// A fixture bot placed where the goal model can rank its options
Snapshot PlanBot(UpkeepIndex const& index, std::uint8_t cls, std::uint8_t level, std::uint32_t money,
                 std::uint8_t trainedTo, std::vector<std::uint32_t> const& riding, int thrift)
{
    Snapshot bot = Bot(index, cls, level, money, trainedTo, riding);
    Snapshot const placed = GoalHarness::Bot(level, money);
    bot.bot = placed.bot;
    bot.position = placed.position;
    bot.zone = placed.zone;
    bot.area = placed.area;
    bot.bagSlots = placed.bagSlots;
    bot.bagFree = placed.bagFree;
    bot.runSpeed = placed.runSpeed;
    bot.traits = Traits(thrift, 0, false);
    return bot;
}

Goals::GrindSpot SpotFor(std::uint32_t id, std::uint8_t level, float xpShare, float copperShare)
{
    Goals::GrindSpot spot = GoalHarness::Spot(id, xpShare * BandXp(level), copperShare * BandEarn(level),
                                              level > 1 ? level - 1u : 1u, level + 1u);
    spot.travelMinutes = 0.0f;
    return spot;
}

// The situation every case ranks: an XP circuit at the bot's hub, an XP spot and a copper-heavy spot, the needs
// bridged in
Ranked Plan(Snapshot const& bot, NeedList const& needs)
{
    Situation situation = GoalHarness::Base();
    situation.here.zoneLow = bot.level > 3 ? bot.level - 3u : 1u;
    situation.here.zoneHigh = bot.level + 3u;
    NeedBridge::Fill(situation, NeedBridge::InputsOf(needs));
    situation.spots = {SpotFor(SPOT_XP, bot.level, XP_SPOT_XP_SHARE, XP_SPOT_COPPER_SHARE),
                       SpotFor(SPOT_GOLD, bot.level, GOLD_SPOT_XP_SHARE, GOLD_SPOT_COPPER_SHARE)};
    CircuitPlan const circuit = GoalHarness::Circuit(
        bot, situation, CIRCUIT_XP_SHARE * BandXp(bot.level) * CIRCUIT_MINUTES, 0.0f, CIRCUIT_MINUTES);
    situation.circuit = &circuit;
    return Goals::Rank(bot, situation);
}

void Print(std::string const& who, NeedList const& needs, Ranked const& ranked)
{
    std::cout << "  " << who << ": reserve " << Money(needs.reserve) << ", shortfall " << Money(needs.shortfall)
              << (ranked.shortText.empty() ? "" : " (" + ranked.shortText + ")") << "\n    "
              << GoalHarness::Summary(ranked) << "\n";
}

bool HasReason(Ranked const& ranked, char const* reason)
{
    for (Option const& option : ranked.options)
        if (option.reason == reason)
            return true;
    return false;
}

bool Fights(GoalKind kind)
{
    return kind == GoalKind::Quest || kind == GoalKind::Grind || kind == GoalKind::Money || kind == GoalKind::Gather ||
           kind == GoalKind::MoveZone;
}

Option const* Spot(Ranked const& ranked, std::uint32_t id)
{
    for (Option const& option : ranked.options)
        if ((option.kind == GoalKind::Grind || option.kind == GoalKind::Money) && option.target == id)
            return &option;
    return nullptr;
}

std::vector<std::uint32_t> const APPRENTICE = {SPELL_APPRENTICE};

// 12 g short at 39 for journeyman riding: money-making first; at 40 with the rank bought and the reserve covered, the
// circuit again
void ShortForRiding(UpkeepIndex const& index)
{
    std::cout << "12 g short for riding at 39:\n";
    NeedRates const rates = Walker(39);
    Snapshot bot = PlanBot(index, CLASS_WARRIOR, 39, 0, 39, APPRENTICE, 0);
    NeedList const broke = ComputeNeeds(bot, index, rates);
    bot.money = static_cast<std::uint32_t>(broke.reserve > 12 * GOLD ? broke.reserve - 12 * GOLD : 0);
    NeedList const needs = ComputeNeeds(bot, index, rates);
    Check(needs.reserveRiding > 0 && needs.shortfall == 12 * GOLD,
          "the case: journeyman riding in the reserve, 12 g short (reserve riding " + Money(needs.reserveRiding) + ")");
    Ranked const ranked = Plan(bot, needs);
    Print("short", needs, ranked);
    Check(GoalHarness::TopIs(ranked, GoalKind::Money, SPOT_GOLD, "gold_short"), "a short bot earns first");
    Check(ranked.shortText.find("short for journeyman riding") != std::string::npos && ranked.shortfallCopper > 0,
          "the shortfall is named: " + ranked.shortText);

    Snapshot after = PlanBot(index, CLASS_WARRIOR, 40, 0, 40, {SPELL_APPRENTICE, SPELL_JOURNEYMAN}, 0);
    NeedRates const rates40 = Walker(40);
    after.money = static_cast<std::uint32_t>(ComputeNeeds(after, index, rates40).reserve);
    NeedList const covered = ComputeNeeds(after, index, rates40);
    Ranked const quests = Plan(after, covered);
    Print("rank bought at 40", covered, quests);
    Check(covered.shortfall == 0 && covered.reserveRiding == 0, "the bought rank leaves no shortfall");
    Check(GoalHarness::TopIs(quests, GoalKind::Quest, GoalHarness::HOME_HUB, Goals::CIRCUIT_REASON) &&
              !HasReason(quests, "gold_short") && quests.shortText.empty(),
          "the bot quests again");
}

void ReserveCovered(UpkeepIndex const& index)
{
    std::cout << "reserve covered:\n";
    NeedRates const rates = Walker(39);
    Snapshot bot = PlanBot(index, CLASS_WARRIOR, 39, 0, 39, APPRENTICE, 0);
    bot.money = static_cast<std::uint32_t>(2 * ComputeNeeds(bot, index, rates).reserve);
    NeedList const needs = ComputeNeeds(bot, index, rates);
    Ranked const ranked = Plan(bot, needs);
    Print("twice the reserve", needs, ranked);
    Check(GoalHarness::TopIs(ranked, GoalKind::Quest, GoalHarness::HOME_HUB, Goals::CIRCUIT_REASON) &&
              !HasReason(ranked, "gold_short"),
          "a covered bot quests on, no gold_short option");
}

void FrugalSavesEarlier(UpkeepIndex const& index)
{
    std::cout << "frugal and spendthrift with the same gold:\n";
    NeedRates const rates = Walker(39);
    Snapshot frugal = PlanBot(index, CLASS_WARRIOR, 39, 0, 39, APPRENTICE, 100);
    Snapshot spender = PlanBot(index, CLASS_WARRIOR, 39, 0, 39, APPRENTICE, -100);
    std::uint64_t const frugalReserve = ComputeNeeds(frugal, index, rates).reserve;
    frugal.money = spender.money = static_cast<std::uint32_t>(frugalReserve > GOLD ? frugalReserve - GOLD : 0);
    NeedList const frugalNeeds = ComputeNeeds(frugal, index, rates);
    NeedList const spenderNeeds = ComputeNeeds(spender, index, rates);
    Ranked const frugalRanked = Plan(frugal, frugalNeeds);
    Ranked const spenderRanked = Plan(spender, spenderNeeds);
    Print("frugal", frugalNeeds, frugalRanked);
    Print("spendthrift", spenderNeeds, spenderRanked);
    Option const* frugalSpot = Spot(frugalRanked, SPOT_GOLD);
    Option const* spenderSpot = Spot(spenderRanked, SPOT_GOLD);
    Check(frugalNeeds.reserve > spenderNeeds.reserve && frugalNeeds.shortfall > 0 && spenderNeeds.shortfall == 0,
          "the frugal bot's reserve is larger: it is short, the spendthrift is not");
    Check(frugalSpot && spenderSpot && frugalSpot->valuePerMinute > spenderSpot->valuePerMinute &&
              frugalSpot->reason == "gold_short" && !HasReason(spenderRanked, "gold_short"),
          "the frugal bot values the copper more and saves (gold_short); the spendthrift does not");
}

void SickRests(UpkeepIndex const& index)
{
    std::cout << "sick:\n";
    NeedRates const rates = Walker(39);
    Snapshot bot = PlanBot(index, CLASS_WARRIOR, 39, 0, 39, APPRENTICE, 0);
    std::uint64_t const reserve = ComputeNeeds(bot, index, rates).reserve;
    bot.money = static_cast<std::uint32_t>(reserve > 12 * GOLD ? reserve - 12 * GOLD : 0);
    NeedList const needs = ComputeNeeds(bot, index, rates);
    Ranked const well = Plan(bot, needs);
    bot.sickMs = SICK_MS;
    Ranked const sick = Plan(bot, needs);
    Print("sick and short", needs, sick);
    bool fights = false;
    for (Option const& option : sick.options)
        fights = fights || Fights(option.kind);
    Check(GoalHarness::TopIs(sick, GoalKind::Rest, 0, "sick") && !fights, "a sick bot rests; no option fights");
    bool allDropped = true;
    for (Option const& option : well.options)
    {
        bool dropped = false;
        for (Goals::Dropped const& out : sick.dropped)
            dropped = dropped || (out.kind == option.kind && out.target == option.target && out.reason == "sick");
        allDropped = allDropped && dropped;
    }
    Check(allDropped && !well.options.empty(), "every option the well bot ranks is dropped as sick");
}

struct Search
{
    std::mt19937_64 rng;
    int Pick(int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); }
};

std::string Dump(Snapshot const& bot, NeedList const& needs, Ranked const& ranked)
{
    return "level " + std::to_string(bot.level) + " class " + std::to_string(bot.cls) + " money " + Money(bot.money) +
           " reserve " + Money(needs.reserve) + " shortfall " + Money(needs.shortfall) + " scale " +
           std::to_string(needs.reserveScale) + " sick " + std::to_string(bot.sickMs) + " ms | " +
           GoalHarness::Summary(ranked);
}

void InvariantSearch(UpkeepIndex const& index, std::uint64_t seed, int count)
{
    std::cout << "invariant search: seed " << seed << ", " << count << " snapshots\n";
    constexpr std::array<std::uint8_t, 7> CLASSES = {1, 2, 3, 5, 8, 9, 11};
    constexpr std::array<std::uint32_t, 4> RANKS = {SPELL_APPRENTICE, SPELL_JOURNEYMAN, SPELL_EXPERT, SPELL_ARTISAN};
    Search search{std::mt19937_64(seed)};
    std::array<int, 4> violations{};
    std::array<int, 4> reached{};
    int brokeCases = 0;
    bool shown = false;
    auto const fail = [&violations, &shown](int invariant, std::string const& text)
    {
        ++violations[invariant];
        if (shown)
            return;
        shown = true;
        std::cout << "  first counterexample, I" << invariant + 1 << ": " << text << "\n";
    };
    for (int n = 0; n < count; ++n)
    {
        std::uint8_t const level = static_cast<std::uint8_t>(search.Pick(1, 80));
        std::uint8_t const cls = CLASSES[static_cast<std::size_t>(search.Pick(0, CLASSES.size() - 1))];
        std::uint8_t const trained = static_cast<std::uint8_t>(std::max(1, level - search.Pick(0, 6)));
        std::vector<std::uint32_t> riding(RANKS.begin(), RANKS.begin() + search.Pick(0, RANKS.size()));
        Snapshot bot = PlanBot(index, cls, level, 0, trained, riding, search.Pick(-100, 100));
        bot.traits->Values[TraitRules::Index(TraitRules::Trait::Orderliness)] =
            static_cast<std::int8_t>(search.Pick(-100, 100));
        bot.traits->Values[TraitRules::Index(TraitRules::Trait::Generosity)] =
            static_cast<std::int8_t>(search.Pick(-100, 100));
        if (search.Pick(0, 1))
        {
            bot.repairCost = static_cast<std::uint32_t>(search.Pick(0, level * 300));
            bot.lowestDurabilityPercent = static_cast<std::uint8_t>(search.Pick(0, 100));
            bot.durabilityPercent = static_cast<std::uint8_t>(search.Pick(bot.lowestDurabilityPercent, 100));
            bot.brokenItems = bot.lowestDurabilityPercent ? 0 : 1;
        }
        SetStock(bot, UpkeepStock::Food, static_cast<std::uint32_t>(search.Pick(0, 40)));
        if (!search.Pick(0, 3))
            bot.sickMs = static_cast<std::uint32_t>(search.Pick(1, 600000));
        NeedRates const rates = Rates(level, static_cast<float>(search.Pick(5, 40)));
        std::uint64_t const reserve = ComputeNeeds(bot, index, rates).reserve;
        std::uint64_t const top = std::min<std::uint64_t>(reserve ? 3 * reserve : 50ull * level * level,
                                                          std::numeric_limits<std::int32_t>::max());
        bot.money = static_cast<std::uint32_t>(std::uniform_int_distribution<std::uint64_t>(0, top)(search.rng));

        NeedList const needs = ComputeNeeds(bot, index, rates);
        Ranked const ranked = Plan(bot, needs);

        // I4: reasons, finite values, determinism
        ++reached[3];
        bool sound = ranked.ok;
        for (Option const& option : ranked.options)
            sound =
                sound && !option.reason.empty() && std::isfinite(option.value) && std::isfinite(option.valuePerMinute);
        if (!sound || !GoalHarness::SameList(ranked, Plan(bot, needs)))
            fail(3, Dump(bot, needs, ranked));

        if (bot.sickMs)
        {
            // I3
            ++reached[2];
            bool fights = false;
            for (Option const& option : ranked.options)
                fights = fights || Fights(option.kind);
            if (!GoalHarness::TopIs(ranked, GoalKind::Rest, 0, "sick") || fights)
                fail(2, Dump(bot, needs, ranked));
            continue;
        }

        // I1: half the gold never lowers an earning option and never raises the circuit
        ++reached[0];
        Snapshot half = bot;
        half.money = bot.money / 2;
        NeedList const halfNeeds = ComputeNeeds(half, index, rates);
        Ranked const halfRanked = Plan(half, halfNeeds);
        for (Option const& option : ranked.options)
        {
            Option const* other = nullptr;
            if (option.copper > 0.0f)
                other = Spot(halfRanked, option.target);
            else if (option.kind == GoalKind::Quest)
                other = GoalHarness::Find(halfRanked, GoalKind::Quest, option.target);
            if (!other)
                continue;
            float const slack = TOLERANCE * std::max(1.0f, option.valuePerMinute);
            bool const ok = option.copper > 0.0f ? other->valuePerMinute + slack >= option.valuePerMinute
                                                 : other->valuePerMinute <= option.valuePerMinute + slack;
            if (!ok)
                fail(0, Dump(bot, needs, ranked) + " || half: " + Dump(half, halfNeeds, halfRanked));
        }
        Snapshot broke = bot;
        broke.money = 0;
        NeedList const brokeNeeds = ComputeNeeds(broke, index, rates);
        if (brokeNeeds.shortfall)
        {
            ++brokeCases;
            Ranked const brokeRanked = Plan(broke, brokeNeeds);
            if (!GoalHarness::TopIs(brokeRanked, GoalKind::Money, SPOT_GOLD, "gold_short"))
                fail(0, "broke: " + Dump(broke, brokeNeeds, brokeRanked));
        }

        // I2: no shortfall, no gold_short
        if (!needs.shortfall)
        {
            ++reached[1];
            if (HasReason(ranked, "gold_short") || ranked.need.shortOfReserve || ranked.shortfallCopper)
                fail(1, Dump(bot, needs, ranked));
        }
    }
    std::cout << "  reached: I1 " << reached[0] << " (broke with a shortfall " << brokeCases << "), I2 " << reached[1]
              << ", I3 " << reached[2] << ", I4 " << reached[3] << "\n";
    Check(violations[0] == 0 && brokeCases > 0, "I1 the gold's worth is monotone; a broke short bot earns first");
    Check(violations[1] == 0 && reached[1] > 0, "I2 no shortfall, no gold_short");
    Check(violations[2] == 0 && reached[2] > 0, "I3 a sick bot rests first and nothing fights");
    Check(violations[3] == 0, "I4 reasons, finite values, a deterministic ranking");
}

// ---- the skill path live (WR11) ----
using ProfessionRules::Skill;
constexpr std::uint32_t PATH_REPAIR = 2000;  // a reserve to be poor against: the repair bill
constexpr std::array<std::uint32_t, 6> PATH_MONEY = {300, 1000, 3000, 10000, 40000, 200000};
constexpr int TIMED_RUNS = 3;
constexpr double SELL_COPPER_PER_LEVEL_SQUARED = 0.06;  // [E] as WR10's harness
constexpr std::uint8_t CLASS_ROGUE_ID = 4;
constexpr std::uint8_t CLASS_MAGE_ID = 8;
constexpr std::size_t TIMED_BOTS = 2 * ProfessionRules::PROFESSION_COUNT;

struct PathSample
{
    char const* name;
    std::uint8_t level;
    std::uint8_t cls;
    Skill first;
    std::uint16_t firstValue;
    Skill second;
    std::uint16_t secondValue;
    std::vector<std::uint32_t> known;
    std::vector<std::uint32_t> tools;
    std::vector<std::uint32_t> categories;                 // what the tools cover (162 hammer, 165 pick)
    std::vector<SkillPath::ConsumableWant> consumables{};  // item use's wants
    std::vector<SkillPath::UseWant> uses{};                // upgrades it would use (travel's worth)
};

std::uint16_t CapOf(std::uint16_t value) { return value < 75 ? 75 : value < 150 ? 150 : value < 225 ? 225 : 300; }

Snapshot PathSnapshot(UpkeepIndex const& index, PathSample const& sample, std::uint32_t money)
{
    Snapshot bot = Bot(index, sample.cls, sample.level, money, sample.level);
    bot.repairCost = PATH_REPAIR;
    bot.brokenItems = 1;
    ProfessionRules::BotProfessions person;
    person.Mains = {sample.first, sample.second};
    bot.person = person;
    for (std::pair<Skill, std::uint16_t> const held :
         {std::pair{sample.first, sample.firstValue}, std::pair{sample.second, sample.secondValue}})
        if (held.first != Skill::None && held.second)
            bot.skills.push_back({static_cast<std::uint16_t>(held.first), held.second, CapOf(held.second)});
    bot.knownRecipes = sample.known;
    std::sort(bot.knownRecipes.begin(), bot.knownRecipes.end());
    bot.toolsHeld = sample.tools;
    return bot;
}

// sells: products and nodes fetch WR10's stand-in price; false: nothing fetches anything
// A vendor's price for the tools of ProfessionRules::TOOLS: the world DB's item_template.BuyPrice (2026-11-20)
std::uint32_t ToolPrice(std::uint32_t item)
{
    static std::map<std::uint32_t, std::uint32_t> const prices = {{2901, 81},   {5956, 18},     {7005, 82},  {6256, 23},
                                                                  {20815, 800}, {20824, 25000}, {39505, 750}};
    auto const found = prices.find(item);
    return found == prices.end() ? 0 : found->second;
}

// WR10's harness stand-in for the vendor's sell price (live: the item template's)
double StandInSell(Recipes::RecipeIndex const& recipes, std::uint32_t item)
{
    Recipes::ItemFact const* fact = recipes.Item(item);
    double const level = fact ? fact->itemLevel : 0.0;
    return fact ? std::floor(level * level * SELL_COPPER_PER_LEVEL_SQUARED * std::max<std::uint32_t>(fact->quality, 1))
                : 0.0;
}

// market: the typical prices the path sees (WA7a); empty: only vendors buy and sell
SkillPathBridge::Computed PathOf(Snapshot const& bot, UpkeepIndex const& index, PathSample const& sample,
                                 Recipes::RecipeIndex const& recipes, bool sells = true, Market::View market = {},
                                 SkillPath::Fault fault = SkillPath::Fault::None)
{
    SkillPath::PathWorld world{recipes,
                               [](std::uint32_t spell) { return MayLearn(spell); },
                               [&recipes](std::uint32_t item) { return StandInSell(recipes, item); },
                               {},
                               {},
                               fault,
                               SkillPathBridge::ToolSourcesOf(ToolPrice),
                               std::move(market)};
    if (!sells)
        world.sellCopper = nullptr;
    SkillPath::PathInputs inputs;
    inputs.consumables = sample.consumables;
    inputs.uses = sample.uses;
    return SkillPathBridge::Compute(bot, index, Rates(bot.level), inputs, world, {}, sample.categories);
}

// A harness market (WA7a): the items `priced` picks at MARKET_TIMES x the stand-in sell price (at least
// MARKET_FLOOR_COPPER), sold steadily (10 sales in the window, no expiry), listed by the stack (Market::MAX_LOT) at
// the shared house's fees (5/5). unpriced: the same entries without a sale (a market that prices nothing)
constexpr std::uint32_t MARKET_TIMES = 3;
constexpr std::uint32_t MARKET_FLOOR_COPPER = 200;

Market::View MarketOf(Recipes::RecipeIndex const& recipes, std::function<bool(std::uint32_t)> const& priced,
                      bool unpriced = false)
{
    auto snapshot = std::make_shared<AuctionIndex::Snapshot>();
    auto const add = [&](std::uint32_t item)
    {
        if (!item || !priced(item) || snapshot->items.count(item))
            return;
        AuctionIndex::TypicalPrice& typical = snapshot->items[item].typical;
        if (unpriced)
            return;
        typical.unitPrice =
            std::max(static_cast<std::uint32_t>(StandInSell(recipes, item)) * MARKET_TIMES, MARKET_FLOOR_COPPER);
        typical.sample = 10;
        typical.weight = 10.0;
    };
    for (Recipes::NodeRow const& row : recipes.Nodes())
        add(row.item);
    for (Recipes::DropRow const& row : recipes.Drops())
        add(row.item);
    for (Recipes::ItemFact const& fact : recipes.Items())
        add(fact.id);
    return {snapshot, {}, {}, [](std::uint32_t) { return Market::MAX_LOT; }};
}

// What the gatherers yield: a node's item of herbalism or mining, a skin
std::function<bool(std::uint32_t)> GatheredBy(Recipes::RecipeIndex const& recipes)
{
    auto items = std::make_shared<std::set<std::uint32_t>>();
    for (Recipes::NodeRow const& row : recipes.Nodes())
        if (row.skill == static_cast<std::uint16_t>(Skill::Herbalism) ||
            row.skill == static_cast<std::uint16_t>(Skill::Mining))
            items->insert(row.item);
    for (Recipes::DropRow const& row : recipes.Drops())
        if (row.kind == Recipes::DropKind::Skin)
            items->insert(row.item);
    return [items](std::uint32_t item) { return items->count(item) > 0; };
}

constexpr std::uint32_t LINEN_CLOTH = 2589;
constexpr std::uint32_t WOOL_CLOTH = 2592;

std::string Serial(SkillPathBridge::Computed const& computed)
{
    std::ostringstream out;
    SkillPath::Path const& path = computed.path;
    out << path.chosen << ' ' << static_cast<int>(path.reason);
    for (SkillPath::Stretch const& stretch : path.weighed)
        out << " s" << stretch.skill << ':' << stretch.to << ':' << stretch.netWorth << ':' << stretch.copper;
    for (SkillPath::Purchase const& purchase : path.needs)
        out << " p" << purchase.spell << ':' << purchase.copper;
    for (SkillPath::Want const& want : path.wants)
        out << " w" << want.item << ':' << want.count;
    for (SkillPath::Craft const& craft : path.queue)
        out << " q" << craft.spell << ':' << craft.casts << (craft.forSale ? "s" : "");
    SkillPath::SecondChoice const& second = path.second;
    out << " c" << second.craft << ':' << second.gatherer << ':' << second.take << ':'
        << static_cast<int>(second.reason) << ':' << second.craftWorth << ':' << second.gathererWorth;
    for (Need const& need : computed.needs.needs)
        out << " n" << static_cast<int>(need.order) << ':' << need.item << ':' << need.copper << ':' << need.funded;
    out << " r" << computed.needs.reserve;
    return out.str();
}

std::size_t RecipeNeeds(NeedList const& list)
{
    return static_cast<std::size_t>(std::count_if(
        list.needs.begin(), list.needs.end(), [](Need const& need) { return need.step == ProfessionStep::Recipe; }));
}

std::string ChoiceText(SkillPath::Path const& path)
{
    if (path.chosen < 0)
        return std::string("none (") + SkillPath::ReasonWord(path.reason) + ")";
    SkillPath::Stretch const& stretch = path.weighed[static_cast<std::size_t>(path.chosen)];
    return "skill " + std::to_string(stretch.skill) + " " + std::to_string(stretch.from) + "->" +
           std::to_string(stretch.to);
}

std::vector<PathSample> PathSamples()
{
    return {
        {"level-8 warrior, mining 40 and blacksmithing 25",
         8,
         CLASS_WARRIOR,
         Skill::Mining,
         40,
         Skill::Blacksmithing,
         25,
         {2657, 2660, 2663, 3320},
         {2901, 5956},
         {162, 165}},
        {"level-20 rogue, herbalism 90 and alchemy 70",
         20,
         CLASS_ROGUE_ID,
         Skill::Herbalism,
         90,
         Skill::Alchemy,
         70,
         {2329, 2330, 3170, 7934},
         {},
         {}},
        {"level-25 warrior, mining 110 and engineering 90",
         25,
         CLASS_WARRIOR,
         Skill::Mining,
         110,
         Skill::Engineering,
         90,
         {2657, 2659, 3918, 3919, 3920, 3922},
         {2901, 5956},
         {162, 165}},
        {"level-45 mage, tailoring 225 and enchanting 200",
         45,
         CLASS_MAGE_ID,
         Skill::Tailoring,
         225,
         Skill::Enchanting,
         200,
         {2387, 3914, 7421, 7418},
         {6218},
         {}},
        {"level-20 rogue, herbalism only (a gatherer)",
         20,
         CLASS_ROGUE_ID,
         Skill::Herbalism,
         70,
         Skill::None,
         0,
         {},
         {},
         {}},
        {"level-20 rogue, herbalism 90 and alchemy 70, potions wanted",
         20,
         CLASS_ROGUE_ID,
         Skill::Herbalism,
         90,
         Skill::Alchemy,
         70,
         {2329, 2330, 3170, 7934},
         {},
         {},
         {{858, 10, 0, 40.0f}, {929, 10, 0, 100.0f}, {3385, 10, 0, 80.0f}}},
    };
}

// The chosen stretch's first craft with its reagents in bags and bank and its recipe known: true when crafts queue
bool QueuesWithReagents(Snapshot const& bot, UpkeepIndex const& index, PathSample const& sample,
                        Recipes::RecipeIndex const& recipes, SkillPath::Stretch const& stretch)
{
    for (SkillPath::PathStep const& step : stretch.steps)
    {
        if (step.kind != SkillPath::StepKind::Craft)
            continue;
        auto const recipe = std::find_if(recipes.Recipes().begin(), recipes.Recipes().end(),
                                         [&step](Recipes::Recipe const& r) { return r.spell == step.id; });
        if (recipe == recipes.Recipes().end())
            continue;
        Snapshot stocked = bot;
        for (Recipes::ItemCount const& reagent : recipe->reagents)
            stocked.materials.push_back({reagent.item, reagent.count * 10, reagent.count * 5});
        std::sort(stocked.materials.begin(), stocked.materials.end(),
                  [](Planner::MaterialHeld const& a, Planner::MaterialHeld const& b) { return a.item < b.item; });
        stocked.knownRecipes.push_back(step.id);
        std::sort(stocked.knownRecipes.begin(), stocked.knownRecipes.end());
        SkillPathBridge::Computed const queued = PathOf(stocked, index, sample, recipes);
        std::cout << "    with the reagents of " << recipe->spell << " in bags and bank: " << queued.path.queue.size()
                  << " crafts queued, choice " << ChoiceText(queued.path) << "\n";
        return !queued.path.queue.empty();
    }
    return false;
}

std::size_t LearnsOf(NeedList const& list, Skill skill)
{
    return static_cast<std::size_t>(std::count_if(
        list.needs.begin(), list.needs.end(), [skill](Need const& need)
        { return need.step == ProfessionStep::Learn && need.skill == static_cast<std::uint16_t>(skill); }));
}

std::string SecondText(SkillPath::SecondChoice const& second)
{
    std::ostringstream out;
    out << "craft " << second.craft << " worth " << second.craftWorth << ", gatherer " << second.gatherer << " worth "
        << second.gathererWorth << ", take " << second.take << " (" << SkillPath::ReasonWord(second.reason) << "), "
        << second.money << " c above the reserve";
    return out.str();
}

// (d) the second profession when it is due (PLAN 3.2, 3.3): a flush bot takes its craft; a bot short of gold with a
// craft worth little takes a second gatherer in its place (stored as its person on the bot's thread); not due while
// the first main is unlearned, the gatherer's level not reached or the second slot learned (WR.7)
void SecondChoices(UpkeepIndex const& index, Recipes::RecipeIndex const& recipes)
{
    std::cout << "Scenario: the second profession (WR11 d)\n";
    // named fixtures (the dumped index, WR10's stand-in sell prices): an engineer's gadgets and ammunition pay at any
    // gold; enchanting from its first point makes nothing that fetches anything
    PathSample const engineer{"level-20 warrior, mining 90, engineering unlearned",
                              20,
                              CLASS_WARRIOR,
                              Skill::Mining,
                              90,
                              Skill::Engineering,
                              0,
                              {},
                              {2901, 5956},
                              {162, 165}};
    PathSample const enchanter{"level-20 mage, tailoring 90, enchanting unlearned",
                               20,
                               CLASS_MAGE_ID,
                               Skill::Tailoring,
                               90,
                               Skill::Enchanting,
                               0,
                               {},
                               {},
                               {}};
    PathSample const smith{"level-20 warrior, mining 90, blacksmithing unlearned",
                           20,
                           CLASS_WARRIOR,
                           Skill::Mining,
                           90,
                           Skill::Blacksmithing,
                           0,
                           {},
                           {2901, 5956},
                           {162, 165}};
    std::uint32_t const flush = PATH_MONEY[5];
    std::uint32_t const poor = PATH_MONEY[0];  // below the repair reserve: short of gold

    Snapshot const flushEngineer = PathSnapshot(index, engineer, flush);
    SkillPathBridge::Computed const tookCraft = PathOf(flushEngineer, index, engineer, recipes);
    std::cout << "  " << engineer.name << ", " << Money(flush) << ": " << SecondText(tookCraft.path.second) << "\n";
    Check(tookCraft.path.second.craft == static_cast<std::uint16_t>(Skill::Engineering) &&
              tookCraft.path.second.take == static_cast<std::uint16_t>(Skill::Engineering) &&
              tookCraft.path.second.reason == SkillPath::Reason::Chosen && tookCraft.path.second.craftWorth > 0.0 &&
              !tookCraft.path.second.gatherer,
          std::string(engineer.name) + ", flush: takes its craft");
    Check(LearnsOf(tookCraft.needs, Skill::Engineering) ==
              LearnsOf(UpkeepNeeds::ComputeNeeds(flushEngineer, index, Rates(flushEngineer.level)), Skill::Engineering),
          "a craft taken keeps its learn need as without the path");

    // without a market only vendors buy materials (WR11's hold): no gatherer takes the craft's place, the craft's
    // learn waits
    Snapshot const poorEnchanter = PathSnapshot(index, enchanter, poor);
    SkillPathBridge::Computed const held = PathOf(poorEnchanter, index, enchanter, recipes);
    std::cout << "  " << enchanter.name << ", " << Money(poor)
              << " (short of gold), no market: " << SecondText(held.path.second) << "\n";
    Check(held.path.second.craft && !held.path.second.gatherer && !held.path.second.take,
          std::string(enchanter.name) + ", short of gold, no market: no second gatherer, the craft waits");
    // a market that buys no gatherer's yields (cloth only, WA7a): the craft still waits; the control weighs a gatherer
    // for any market view and must take one
    Market::View const cloth =
        MarketOf(recipes, [](std::uint32_t item) { return item == LINEN_CLOTH || item == WOOL_CLOTH; });
    SkillPath::SecondChoice const clothOnly = PathOf(poorEnchanter, index, enchanter, recipes, true, cloth).path.second;
    SkillPath::SecondChoice const anyMarket =
        PathOf(poorEnchanter, index, enchanter, recipes, true, cloth, SkillPath::Fault::AnyMarket).path.second;
    std::cout << "  " << enchanter.name << ", " << Money(poor)
              << " (short of gold), a market for cloth only: " << SecondText(clothOnly)
              << "\n    control any_market: " << SecondText(anyMarket) << "\n";
    Check(clothOnly.craft && !clothOnly.gatherer && !clothOnly.take,
          std::string(enchanter.name) + ", a market for no gatherer's yields: no second gatherer, the craft waits");
    Check(anyMarket.gatherer != 0, "control any_market: a gatherer weighed where the market buys none of its yields");
    // a market that buys the gatherers' yields above the vendors (WR23, WA7a): the gatherer weighed with its proceeds
    Market::View const gathered = MarketOf(recipes, GatheredBy(recipes));
    SkillPathBridge::Computed const tookGatherer = PathOf(poorEnchanter, index, enchanter, recipes, true, gathered);
    SkillPath::SecondChoice const& replaced = tookGatherer.path.second;
    std::cout << "  " << enchanter.name << ", " << Money(poor)
              << " (short of gold), a market for gathered yields: " << SecondText(replaced) << "\n";
    Check(replaced.craft == static_cast<std::uint16_t>(Skill::Enchanting) && replaced.take == replaced.gatherer &&
              ProfessionRules::Is(static_cast<Skill>(replaced.take), ProfessionRules::Kind::Gatherer) &&
              replaced.reason == SkillPath::Reason::GoldShort && replaced.gathererWorth > 0.0 &&
              replaced.gathererWorth > replaced.craftWorth,
          std::string(enchanter.name) + ", short of gold: a second gatherer worth more than its craft");
    Check(!LearnsOf(tookGatherer.needs, Skill::Enchanting), "the craft a gatherer replaces is no learn need");
    // the person stored with the gatherer (BotProfessionMgr::ChooseSecond): no choice is due any more
    Snapshot stored = poorEnchanter;
    stored.person->Mains[1] = static_cast<Skill>(replaced.take);
    stored.person->From = ProfessionRules::Origin::Chosen;
    SkillPathBridge::Computed const after = PathOf(stored, index, enchanter, recipes, true, gathered);
    std::cout << "    stored as the person: " << SecondText(after.path.second) << ", "
              << LearnsOf(after.needs, static_cast<Skill>(replaced.take)) << " learn needs of the gatherer\n";
    Check(!after.path.second.craft, "once the gatherer is stored as the person no second choice is due");

    // the same enchanter with gold weighs no gatherer: enchanting is not worth it yet, its learn waits
    Snapshot const flushEnchanter = PathSnapshot(index, enchanter, flush);
    SkillPathBridge::Computed const waits = PathOf(flushEnchanter, index, enchanter, recipes);
    std::cout << "  " << enchanter.name << ", " << Money(flush) << ": " << SecondText(waits.path.second) << "\n";
    Check(waits.path.second.craft && !waits.path.second.gatherer && !waits.path.second.take &&
              waits.path.second.reason == SkillPath::Reason::NotWorthIt,
          "a bot with gold weighs no second gatherer; a craft not worth it waits");
    Check(!LearnsOf(waits.needs, Skill::Enchanting), "a craft that waits is no learn need");

    // where nothing sells (no vendor, a market without a sale), no gatherer brings anything either: the craft waits,
    // nothing replaces it
    SkillPathBridge::Computed const unsold = PathOf(PathSnapshot(index, enchanter, poor), index, enchanter, recipes,
                                                    false, MarketOf(recipes, GatheredBy(recipes), true));
    std::cout << "  " << enchanter.name << ", " << Money(poor) << ", nothing sold: " << SecondText(unsold.path.second)
              << "\n";
    Check(unsold.path.second.craft && !unsold.path.second.take, "where nothing sells no gatherer replaces the craft");

    // not due: the first main unlearned, below the gatherer's level, the second slot learned, a gatherer second
    PathSample unlearned = smith;
    unlearned.firstValue = 0;
    PathSample learned = smith;
    learned.secondValue = 10;
    Snapshot young = PathSnapshot(index, smith, flush);
    young.level = 4;
    Check(!PathOf(PathSnapshot(index, unlearned, flush), index, unlearned, recipes).path.second.craft &&
              !PathOf(PathSnapshot(index, learned, poor), index, learned, recipes).path.second.craft &&
              !PathOf(young, index, smith, recipes).path.second.craft &&
              !PathOf(PathSnapshot(index, PathSamples()[4], poor), index, PathSamples()[4], recipes).path.second.craft,
          "no second choice before the first main, below the gatherer's level, once learned, or without a craft");

    // the cost of a due bot's plan: three more weighs at most (the craft and two gatherers)
    double best = std::numeric_limits<double>::max();
    for (int run = 0; run < TIMED_RUNS; ++run)
    {
        auto const started = std::chrono::steady_clock::now();
        SkillPathBridge::Computed const computed = PathOf(poorEnchanter, index, enchanter, recipes);
        double const us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
        best = std::min(best, computed.path.second.craft ? us : -1.0);
    }
    std::cout << "  cost per plan of a due bot (the short enchanter): " << static_cast<long long>(best) << " us\n";
    Check(best > 0.0, "a due bot's compute is timed");
}

// The review's probes (WR10/WR11 review): a smith's bars made from its ore, an engineer with and without a hammer, a
// young herbalist short of gold
void ReviewProbes(UpkeepIndex const& index, Recipes::RecipeIndex const& recipes)
{
    std::cout << "Scenario: the review's probes (WR10/WR11 review)\n";
    constexpr std::uint32_t COPPER_ORE = 2770;
    constexpr std::uint32_t COPPER_BAR = 2840;
    constexpr std::uint32_t SMELT_COPPER = 2657;
    // the probe's case on the dumped index: a smith with 40 copper ore who knows the smelt and the bracers, its chosen
    // stretch 38 casts of the bracers (76 bars); before the review it wanted 76 bars and queued nothing
    constexpr std::uint32_t COPPER_BRACERS = 2663;
    SkillPath::PathBot smith;
    smith.level = 8;
    smith.cls = CLASS_WARRIOR;
    smith.money = PATH_MONEY[5];
    smith.rates.exchange = 1.0f;
    smith.rates.factor = 1.0f;
    smith.rates.minuteValue = BandXp(8);
    smith.skills = {{static_cast<std::uint16_t>(Skill::Mining), 40, 75},
                    {static_cast<std::uint16_t>(Skill::Blacksmithing), 25, 75}};
    smith.knownRecipes = {SMELT_COPPER, COPPER_BRACERS};
    smith.toolsHeld = {2901, 5956};
    smith.categoriesHeld = {162, 165};
    smith.held = {{COPPER_ORE, 40}};
    SkillPath::Path bracers;
    SkillPath::Stretch stretch;
    stretch.skill = static_cast<std::uint16_t>(Skill::Blacksmithing);
    stretch.from = 25;
    stretch.to = 60;
    stretch.reason = SkillPath::Reason::Chosen;
    SkillPath::PathStep casts;
    casts.kind = SkillPath::StepKind::Craft;
    casts.id = COPPER_BRACERS;
    casts.from = 25;
    casts.to = 60;
    casts.casts = 38.0;
    stretch.steps.push_back(casts);
    bracers.weighed.push_back(stretch);
    bracers.chosen = 0;
    for (SkillPath::Fault const fault : {SkillPath::Fault::NoExpand, SkillPath::Fault::None})
    {
        SkillPath::PathWorld const world{
            recipes, [](std::uint32_t spell) { return MayLearn(spell); }, nullptr, {}, {}, fault, {}, {}};
        SkillPath::Weigher const weigher(smith, world);
        SkillPath::Claims const claims = weigher.Claim(bracers);
        std::vector<SkillPath::Want> const wants = weigher.Wants(bracers, claims);
        std::ostringstream shown;
        for (SkillPath::Want const& want : wants)
            shown << " want " << want.item << " x" << want.count;
        for (SkillPath::Craft const& craft : claims.queue)
            shown << " craft " << craft.spell << " x" << craft.casts << (craft.forSkill ? " (skill)" : "");
        bool const fixed = fault == SkillPath::Fault::None;
        std::cout << "  a smith with 40 copper ore, 38 bracers casts chosen"
                  << (fixed ? "" : " (wants unexpanded, as the review found)") << ":" << shown.str() << "\n";
        if (!fixed)
            continue;
        bool const wantsBar = std::any_of(wants.begin(), wants.end(),
                                          [](SkillPath::Want const& want) { return want.item == COPPER_BAR; });
        bool const wantsOre = std::any_of(wants.begin(), wants.end(), [](SkillPath::Want const& want)
                                          { return want.item == COPPER_ORE && want.count == 36; });
        Check(!wantsBar && wantsOre, "the smith wants 36 more ore, not the 76 bars it smelts");
        Check(claims.queue.size() == 2 && claims.queue[0].spell == SMELT_COPPER && claims.queue[0].casts == 40 &&
                  claims.queue[0].forSkill && claims.queue[1].spell == COPPER_BRACERS && claims.queue[1].casts == 20,
              "its queue smelts the 40 ore first, then the 20 bracers casts the bars allow");
    }

    // a flush level-20 miner with engineering unlearned, with and without a hammer in its bags
    PathSample const hammer{"level-20 warrior, mining 90, engineering unlearned, a hammer",
                            20,
                            CLASS_WARRIOR,
                            Skill::Mining,
                            90,
                            Skill::Engineering,
                            0,
                            {},
                            {2901, 5956},
                            {162, 165}};
    PathSample noHammer = hammer;
    noHammer.name = "level-20 warrior, mining 90, engineering unlearned, no hammer";
    noHammer.tools = {2901};
    noHammer.categories = {165};
    std::uint32_t const flush = PATH_MONEY[5];
    SkillPath::SecondChoice const with = PathOf(PathSnapshot(index, hammer, flush), index, hammer, recipes).path.second;
    SkillPath::SecondChoice const without =
        PathOf(PathSnapshot(index, noHammer, flush), index, noHammer, recipes).path.second;
    std::cout << "  " << hammer.name << ": " << SecondText(with) << "\n  " << noHammer.name << ": "
              << SecondText(without) << "\n";
    Check(with.take == static_cast<std::uint16_t>(Skill::Engineering) &&
              without.take == static_cast<std::uint16_t>(Skill::Engineering) && without.craftWorth > 0.0 &&
              without.craftWorth <= with.craftWorth + 1e-6,
          "a flush miner takes engineering with or without a hammer; the hammer to buy is worth no more");
    PathSample engineer = noHammer;
    engineer.name = "level-20 warrior, mining 90, engineering 1, no hammer";
    engineer.secondValue = 1;
    SkillPathBridge::Computed const climbs = PathOf(PathSnapshot(index, engineer, flush), index, engineer, recipes);
    bool toolStep = false;
    for (SkillPath::Stretch const& stretch : climbs.path.weighed)
        for (SkillPath::PathStep const& step : stretch.steps)
            toolStep = toolStep || (step.kind == SkillPath::StepKind::Tool && step.id == 5956);
    std::cout << "  " << engineer.name << ": " << ChoiceText(climbs.path) << ", " << climbs.path.needs.size()
              << " path needs\n";
    Check(toolStep && climbs.path.chosen != SkillPath::NONE,
          std::string(engineer.name) + ": the hammer is a step of its climb, and a stretch is chosen");

    // a level-6 herbalist with alchemy unlearned, short of gold: never a second gatherer without a market
    PathSample const herbalist{"level-6 rogue, herbalism 20, alchemy unlearned",
                               6,
                               CLASS_ROGUE_ID,
                               Skill::Herbalism,
                               20,
                               Skill::Alchemy,
                               0,
                               {},
                               {},
                               {}};
    bool gatherer = false;
    Market::View const gathered = MarketOf(recipes, GatheredBy(recipes));
    for (std::uint32_t const money : {PATH_MONEY[0], PATH_MONEY[1], PATH_MONEY[2]})
    {
        Snapshot const young = PathSnapshot(index, herbalist, money);
        SkillPath::SecondChoice const second = PathOf(young, index, herbalist, recipes).path.second;
        SkillPath::SecondChoice const priced = PathOf(young, index, herbalist, recipes, true, gathered).path.second;
        std::cout << "  " << herbalist.name << ", " << Money(money) << ": " << SecondText(second)
                  << "; with a market for gathered yields: " << SecondText(priced) << "\n";
        gatherer =
            gatherer || second.gatherer || (second.take && second.take != static_cast<std::uint16_t>(Skill::Alchemy));
    }
    Check(!gatherer, std::string(herbalist.name) + " at 3s, 10s and 30s: no second gatherer without a market");
}

std::string SkillWord(std::uint16_t skill)
{
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        if (static_cast<std::uint16_t>(info.Which) == skill)
            return std::string(info.Name);
    return skill ? "skill " + std::to_string(skill) : std::string("none");
}

// One line of the printed paths (WA7a-paths.txt): the stretch taken and the second-profession choice, in words
std::string PathWords(SkillPath::Path const& path)
{
    std::ostringstream out;
    if (path.chosen < 0)
        out << "no stretch taken (" << SkillPath::ReasonWord(path.reason) << ")";
    else
    {
        SkillPath::Stretch const& stretch = path.weighed[static_cast<std::size_t>(path.chosen)];
        double const copper = static_cast<double>(stretch.copper);
        out << SkillWord(stretch.skill) << " " << stretch.from << " to " << stretch.to << ", "
            << (copper < 0.0 ? "earns " : "spends ") << Money(static_cast<std::uint32_t>(std::fabs(copper)))
            << " net, net worth " << std::lround(stretch.netWorth);
    }
    SkillPath::SecondChoice const& second = path.second;
    out << "; second profession: ";
    if (!second.craft)
        out << "not due";
    else
    {
        out << (second.take ? "takes " + SkillWord(second.take) : std::string("waits")) << " ("
            << SkillPath::ReasonWord(second.reason) << "; craft " << SkillWord(second.craft) << " worth "
            << std::lround(second.craftWorth);
        if (second.gatherer)
            out << ", gatherer " << SkillWord(second.gatherer) << " worth " << std::lround(second.gathererWorth);
        out << ")";
    }
    return out.str();
}

// The queue's crafts for sale (WA7b), in words
std::string SaleWords(SkillPath::Path const& path)
{
    std::ostringstream out;
    for (SkillPath::Craft const& craft : path.queue)
        if (craft.forSale)
            out << (out.tellp() ? ", " : "") << craft.spell << " x" << craft.casts;
    return out.tellp() ? out.str() : "none";
}

std::size_t SaleCrafts(SkillPath::Path const& path)
{
    return static_cast<std::size_t>(std::count_if(path.queue.begin(), path.queue.end(),
                                                  [](SkillPath::Craft const& craft) { return craft.forSale; }));
}

// WA7a (PLAN 3.7): sample bots' paths and second choices without and with a market, and the empty-market equality: a
// view with an empty snapshot and one whose entries carry no sale give the vendor-only path and needs byte for byte
// (Serial). Control: weighing a second gatherer for any view (Fault::AnyMarket) must break it
void MarketPaths(UpkeepIndex const& index, Recipes::RecipeIndex const& recipes)
{
    std::cout << "Scenario: paths and second choices without and with a market (WA7a)\n";
    std::vector<PathSample> samples = PathSamples();
    samples.push_back({"level-20 mage, tailoring 90, enchanting unlearned",
                       20,
                       CLASS_MAGE_ID,
                       Skill::Tailoring,
                       90,
                       Skill::Enchanting,
                       0,
                       {},
                       {},
                       {}});
    samples.push_back({"level-20 warrior, mining 90, engineering unlearned",
                       20,
                       CLASS_WARRIOR,
                       Skill::Mining,
                       90,
                       Skill::Engineering,
                       0,
                       {},
                       {2901, 5956},
                       {162, 165}});
    Market::View const gathered = MarketOf(recipes, GatheredBy(recipes));
    Market::View const everything = MarketOf(recipes, [](std::uint32_t) { return true; });
    Market::View const emptySnapshot{std::make_shared<AuctionIndex::Snapshot>(), {}, {}, {}};
    Market::View const unsold = MarketOf(recipes, [](std::uint32_t) { return true; }, true);
    // WA7b: a market that sells every item at a sixth of the stand-in's price, under the vendors' and often under the
    // gathering's cost, so the auction is the cheapest source of some materials
    auto cheapSnapshot = std::make_shared<AuctionIndex::Snapshot>(*everything.snapshot);
    for (auto& [item, entry] : cheapSnapshot->items)
        entry.typical.unitPrice = std::max<std::uint32_t>(entry.typical.unitPrice / (MARKET_TIMES * 2), 1);
    Market::View cheap = everything;
    cheap.snapshot = cheapSnapshot;
    std::size_t compared = 0;
    std::size_t differ = 0;
    std::size_t controlDiffer = 0;
    std::size_t changed = 0;
    // The path's buy lines (WA7b): exactly the wanted materials the auction sells cheapest with another way, each at
    // the other way's cost, not below the auction's; none without a market
    std::uint8_t const auctionKind = static_cast<std::uint8_t>(Materials::Kind::Auction);
    std::size_t plainLines = 0;
    std::size_t marketLines = 0;
    std::size_t lineViolations = 0;
    std::size_t controlCaught = 0;
    std::size_t plainSales = 0;  // crafts for sale (WA7b): none without a market
    std::size_t marketSales = 0;
    auto const linesOf = [&](SkillPath::Path const& path)
    {
        // the sample bots hold nothing, as when their paths were computed: every want stands. At the reserve (the
        // need factor equal to the exchange) a line's ceiling is the path's copper
        auto const none = [](std::uint32_t) { return 0u; };
        AuctionRules::CopperWeight const atReserve{1.0f, 1.0f};
        std::vector<AuctionRules::BuyLine> const lines =
            AuctionRules::BuyList(AuctionRules::PathLines(path.materials, auctionKind, atReserve, none, none));
        std::size_t expected = 0;
        for (SkillPath::Material const& material : path.materials)
        {
            lineViolations += material.source != auctionKind && material.otherCopperEach != 0;
            if (!material.wanted || material.source != auctionKind)
                continue;
            ++expected;
            auto const line = std::find_if(lines.begin(), lines.end(), [&material](AuctionRules::BuyLine const& l)
                                           { return l.item == material.item; });
            // the cheapest other way's cost, or (AH8) what the path counted at the auction, its only source
            std::uint32_t const ceiling = material.otherCopperEach ? material.otherCopperEach : material.copperEach;
            lineViolations += line == lines.end() || line->units != material.wanted ||
                              AuctionRules::CeilingOf(*line) != ceiling ||
                              (material.otherCopperEach && material.otherCopperEach < material.copperEach);
            // control: a ceiling at the auction's own cost is never above that cost
            controlCaught +=
                AuctionRules::CeilingOf(AuctionRules::PathLine(material.item, material.wanted, material.copperEach, 0,
                                                               atReserve, 0)) <= material.copperEach;
        }
        lineViolations += lines.size() != expected;
        return lines.size();
    };
    for (PathSample const& sample : samples)
        for (std::uint32_t const money : PATH_MONEY)
        {
            Snapshot const bot = PathSnapshot(index, sample, money);
            SkillPathBridge::Computed const plain = PathOf(bot, index, sample, recipes);
            std::string const serial = Serial(plain);
            bool const same = serial == Serial(PathOf(bot, index, sample, recipes, true, emptySnapshot)) &&
                              serial == Serial(PathOf(bot, index, sample, recipes, true, unsold));
            ++compared;
            differ += same ? 0 : 1;
            plainLines += linesOf(plain.path);
            plainSales += SaleCrafts(plain.path);
            std::string const control =
                Serial(PathOf(bot, index, sample, recipes, true, unsold, SkillPath::Fault::AnyMarket));
            controlDiffer += serial != control ? 1 : 0;
            if (money != PATH_MONEY[0] && money != PATH_MONEY[4])
                continue;
            SkillPathBridge::Computed const sold = PathOf(bot, index, sample, recipes, true, gathered);
            SkillPathBridge::Computed const all = PathOf(bot, index, sample, recipes, true, everything);
            changed += Serial(sold) != serial || Serial(all) != serial ? 1 : 0;
            SkillPathBridge::Computed const bargain = PathOf(bot, index, sample, recipes, true, cheap);
            std::size_t const bargainLines = linesOf(bargain.path);
            marketLines += linesOf(sold.path) + linesOf(all.path) + bargainLines;
            marketSales += SaleCrafts(sold.path) + SaleCrafts(all.path) + SaleCrafts(bargain.path);
            std::cout << "  " << sample.name << ", " << Money(money) << "\n"
                      << "    no market:                  " << PathWords(plain.path) << "\n"
                      << "    market for gathered yields: " << PathWords(sold.path) << "\n"
                      << "    market for every item:      " << PathWords(all.path) << "\n"
                      << "    an empty market gives the no-market path and needs: " << (same ? "yes" : "NO") << "\n"
                      << "    a market at a sixth of the price: " << PathWords(bargain.path) << "; " << bargainLines
                      << " path buy lines\n"
                      << "    crafts for sale: gathered yields " << SaleWords(sold.path) << "; every item "
                      << SaleWords(all.path) << "; a sixth of the price " << SaleWords(bargain.path) << "\n";
        }
    std::cout << "  empty-market equality: " << compared << " bots compared, " << differ
              << " differ; control any_market: " << controlDiffer << " differ; " << changed
              << " printed bots change with a market\n";
    Check(compared > 0 && differ == 0, "an empty market gives every sample the vendor-only path and needs");
    Check(controlDiffer > 0, "control any_market: a market without prices changes a decision");
    std::cout << "  path buy lines (WA7b): " << plainLines << " without a market, " << marketLines
              << " with one; violations " << lineViolations << "; control auction-cost ceilings: " << controlCaught
              << '\n';
    Check(plainLines == 0 && marketLines > 0 && lineViolations == 0,
          "a path line exactly where the auction sells a wanted material cheapest, at the other way's cost");
    Check(controlCaught == marketLines, "control: a ceiling at the auction's own cost bounds nothing (caught)");
    std::cout << "  crafts for sale (WA7b): " << plainSales << " without a market, " << marketSales
              << " in the printed bots' markets\n";
    Check(plainSales == 0, "no craft for sale without a market");
}

void PathLive(UpkeepIndex const& index, std::string const& root)
{
    std::cout << "Scenario: the skill path live (WR11) on the dumped recipe index\n";
    std::ifstream in(root + "/tests/fixtures/recipe-index-dump.txt", std::ios::binary);
    std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Recipes::RecipeIndex recipes;
    std::string error;
    Check(Recipes::RecipeIndex::Parse(text, recipes, error), "the dumped recipe index parses " + error);

    // zones: the bracket stands for the danger until WR13b; Outland and Northrend wait for their levels
    Check(SkillPathBridge::Plays({12, 1, 10, 0, true}, 8) && !SkillPathBridge::Plays({40, 18, 30, 0, true}, 8) &&
              !SkillPathBridge::Plays({3483, 58, 63, 530, true}, 57) &&
              SkillPathBridge::Plays({3483, 58, 63, 530, true}, 60) &&
              !SkillPathBridge::Plays({14, 1, 10, 1, false}, 8),
          "a bot plays in open zones whose bracket starts at most 5 levels above it, on a continent it reaches");
    Check(SkillPathBridge::BracketRisk({40, 10, 20, 0, true}, 8).levelsAbove == 2.0f &&
              SkillPathBridge::BracketRisk({12, 1, 10, 0, true}, 8).levelsAbove == 0.0f,
          "a zone's risk is the levels its bracket starts above the bot");

    bool worthFound = false;
    bool noneFound = false;
    bool queueFound = false;
    int copperChecks = 0;
    int copperViolations = 0;
    for (PathSample const& sample : PathSamples())
    {
        std::vector<SkillPathBridge::Computed> byMoney;
        for (std::uint32_t const money : PATH_MONEY)
        {
            Snapshot const bot = PathSnapshot(index, sample, money);
            SkillPathBridge::Computed const computed = PathOf(bot, index, sample, recipes);
            SkillPath::Path const& path = computed.path;
            std::cout << "  " << sample.name << ", " << Money(money) << ": " << ChoiceText(path) << ", "
                      << path.needs.size() << " purchases, " << RecipeNeeds(computed.needs) << " recipe needs, "
                      << path.wants.size() << " wants, " << path.queue.size() << " crafts\n";
            std::size_t learns = 0;
            for (SkillPath::Purchase const& purchase : path.needs)
                learns += purchase.kind == SkillPath::StepKind::Learn && purchase.copper &&
                          !std::binary_search(bot.knownRecipes.begin(), bot.knownRecipes.end(), purchase.spell);
            if (path.chosen >= 0 && learns)
            {
                worthFound = true;
                Check(RecipeNeeds(computed.needs) == learns,
                      std::string(sample.name) + " at " + Money(money) + ": each recipe the path buys is a need");
                if (!queueFound)
                    queueFound = QueuesWithReagents(bot, index, sample, recipes,
                                                    path.weighed[static_cast<std::size_t>(path.chosen)]);
            }
            // the same bot where nothing it makes or gathers fetches anything: its stretches bring less than they cost
            SkillPathBridge::Computed const unsold = PathOf(bot, index, sample, recipes, false);
            if (unsold.path.chosen < 0 && unsold.path.reason == SkillPath::Reason::NotWorthIt)
            {
                noneFound = true;
                Check(unsold.path.needs.empty() && unsold.path.queue.empty() && unsold.path.wants.empty() &&
                          !RecipeNeeds(unsold.needs),
                      std::string(sample.name) + " at " + Money(money) +
                          ", nothing sold: a stretch not worth it yields nothing");
            }
            byMoney.push_back(computed);
        }
        // the same stretch costs a poorer bot no more copper (WR10's invariant 3, through the live rates)
        for (std::size_t poorer = 0; poorer + 1 < byMoney.size(); ++poorer)
            for (SkillPath::Stretch const& a : byMoney[poorer].path.weighed)
                for (SkillPath::Stretch const& b : byMoney[poorer + 1].path.weighed)
                    if (a.skill == b.skill && a.from == b.from && a.to == b.to && a.blockedAt == 0 &&
                        b.blockedAt == 0 && a.reason != SkillPath::Reason::NoSource &&
                        b.reason != SkillPath::Reason::NoSource)
                    {
                        ++copperChecks;
                        if (a.copper > b.copper + 0.5)
                        {
                            ++copperViolations;
                            std::cout << "    copper rose for the poorer bot: skill " << a.skill << " " << a.from
                                      << "->" << a.to << ": " << a.copper << " > " << b.copper << "\n";
                        }
                    }
        // determinism: the same snapshot, also with its lists in another order, gives the same path and needs
        Snapshot const bot = PathSnapshot(index, sample, PATH_MONEY[1]);
        Snapshot shuffled = bot;
        shuffled.materials = {{2589, 2, 3}, {2770, 4, 0}};
        Snapshot const sorted = shuffled;
        std::reverse(shuffled.materials.begin(), shuffled.materials.end());
        Check(Serial(PathOf(bot, index, sample, recipes)) == Serial(PathOf(Snapshot(bot), index, sample, recipes)) &&
                  Serial(PathOf(sorted, index, sample, recipes)) == Serial(PathOf(shuffled, index, sample, recipes)),
              std::string(sample.name) + ": the same snapshot gives the same path and needs (worker = bot thread)");
    }
    Check(worthFound, "a worthwhile stretch with recipes to buy is found among the samples");
    Check(queueFound, "with its reagents held, the chosen stretch queues crafts for the next stop");
    Check(noneFound, "a stretch not worth it is found among the samples and yields no need");
    std::cout << "  copper by money: " << copperChecks << " stretch pairs, " << copperViolations << " rises\n";
    Check(copperChecks > 0 && copperViolations == 0, "a poorer bot's stretch spends no more copper");

    SecondChoices(index, recipes);
    ReviewProbes(index, recipes);
    MarketPaths(index, recipes);

    // the cost per plan: the whole compute (needs, path, needs) for each profession at two skills
    std::cout << "  cost per plan (microseconds per compute, best of " << TIMED_RUNS << "):\n";
    double worst = 0.0;
    double total = 0.0;
    std::size_t timed = 0;
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        for (std::uint16_t const value : {std::uint16_t{1}, std::uint16_t{150}})
        {
            PathSample const sample{"timed",
                                    static_cast<std::uint8_t>(value == 1 ? 10 : 35),
                                    CLASS_WARRIOR,
                                    info.Which,
                                    value,
                                    Skill::None,
                                    0,
                                    {},
                                    {2901, 5956},
                                    {162, 165}};
            Snapshot const bot = PathSnapshot(index, sample, PATH_MONEY[2]);
            double best = std::numeric_limits<double>::max();
            for (int run = 0; run < TIMED_RUNS; ++run)
            {
                auto const started = std::chrono::steady_clock::now();
                SkillPathBridge::Computed const computed = PathOf(bot, index, sample, recipes);
                double const us =
                    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
                best = std::min(best, computed.path.version == SkillPath::PATH_VERSION ? us : -1.0);
            }
            std::cout << "    skill " << static_cast<int>(info.Which) << " at " << value << ": "
                      << static_cast<long long>(best) << " us\n";
            worst = std::max(worst, best);
            total += best;
            ++timed;
        }
    std::cout << "  cost per plan: mean " << static_cast<long long>(total / static_cast<double>(timed)) << " us, worst "
              << static_cast<long long>(worst) << " us over " << timed << " bots\n";
    Check(timed == TIMED_BOTS && worst > 0.0 && std::isfinite(worst),
          "the path's compute is timed for the 14 professions");
}
}  // namespace

int main(int argc, char** argv)
{
    std::string root;
    if (argc > 2 && std::string(argv[1]) == "--repo")
    {
        root = argv[2];
        argv += 2;
        argc -= 2;
    }
    std::uint64_t const seed = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : DEFAULT_SEED;
    int const count = argc > 2 ? std::atoi(argv[2]) : DEFAULT_SNAPSHOTS;
    UpkeepIndex const index = Fixture();
    if (!root.empty())
        PathLive(index, root);
    ShortForRiding(index);
    ReserveCovered(index);
    FrugalSavesEarlier(index);
    SickRests(index);
    InvariantSearch(index, seed, count);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
