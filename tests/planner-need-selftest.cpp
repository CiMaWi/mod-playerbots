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
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "GoalModel.h"
#include "GoalModelHarness.h"
#include "NeedGoalBridge.h"
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
        bot.traits->Values[TraitRules::Index(TraitRules::Trait::Thoroughness)] =
            static_cast<std::int8_t>(search.Pick(-100, 100));
        bot.traits->Values[TraitRules::Index(TraitRules::Trait::Greed)] =
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
}  // namespace

int main(int argc, char** argv)
{
    std::uint64_t const seed = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : DEFAULT_SEED;
    int const count = argc > 2 ? std::atoi(argv[2]) : DEFAULT_SNAPSHOTS;
    UpkeepIndex const index = Fixture();
    ShortForRiding(index);
    ReserveCovered(index);
    FrugalSavesEarlier(index);
    SickRests(index);
    InvariantSearch(index, seed, count);
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
