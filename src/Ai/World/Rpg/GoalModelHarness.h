/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GOALMODELHARNESS_H
#define _PLAYERBOT_GOALMODELHARNESS_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "FishRules.h"
#include "GatherRules.h"
#include "GoalModel.h"
#include "GoalModelLog.h"
#include "InterludeInputs.h"
#include "NeedGoalBridge.h"
#include "QuestTakeFixture.h"
#include "UpkeepRules.h"

// The goal model's scenarios (travel plan WT14): synthetic bots and places, one check per decision. Proof is
// functional (WT.21): each check is about the chosen goal and its reason word, never a number of our own. Shared by
// tests/goal-model-selftest.cpp and `playerbots planner goals selftest`. Player-free
namespace Planner
{
namespace GoalHarness
{
using namespace Goals;

constexpr std::uint32_t EASTERN = 0;
constexpr std::uint32_t ELWYNN = 12;
constexpr std::uint32_t WESTFALL = 40;
constexpr std::uint32_t GOLDSHIRE_AREA = 87;
constexpr std::uint8_t HUMAN = 1;
constexpr std::uint8_t WARRIOR = 1;
constexpr std::uint32_t HOME_HUB = 1;
constexpr std::uint32_t FLUSH = 100000;  // copper
constexpr int STRONG_TRAIT = 80;
constexpr std::uint16_t HERBALISM = 182;
constexpr std::uint32_t TIMING_RUNS = 200;
constexpr std::uint32_t GIVER = 90001;
constexpr std::uint32_t PLAIN_QUEST = 501;
constexpr std::uint32_t ELITE_QUEST = 502;
constexpr float ELITE_DEATHS = 0.4f;  // per completion: neutral and daring take it, careful skips it (Risky)
constexpr std::uint32_t DRY_CAMP = 55;
constexpr std::uint32_t DELIVERY_QUEST = 600;
constexpr std::uint32_t RICH_DELIVERY = 601;
constexpr std::uint32_t FAR_DELIVERY = 602;
constexpr std::uint32_t DONE_CARRIED = 610;  // carried quests: done in the log
constexpr std::uint32_t OPEN_CARRIED = 611;  // in the log with its objective left
constexpr std::uint32_t BARE_TAKE = 612;     // a take with no objective but the turn-in
constexpr std::uint32_t OPEN_TAKE = 613;     // a take with an objective, carried for later
constexpr std::uint32_t CARRIED_ENDER = 90002;
constexpr std::uint32_t OUTSIDE_TARGET = 700;
constexpr std::uint32_t KALIMDOR = 1;
constexpr std::uint32_t WETLANDS = 11;
constexpr std::uint32_t REDRIDGE = 44;
constexpr std::uint32_t LOCH_MODAN = 38;
constexpr std::uint32_t SILVERPINE = 130;
constexpr std::uint32_t DARKSHORE = 148;
constexpr std::uint32_t ASHENVALE = 331;
constexpr std::uint32_t STONETALON = 406;
constexpr std::uint32_t HORDE_RACES = 690;          // orc, undead, tauren, troll, blood elf
constexpr std::uint32_t NEXT_ZONE_QUESTS = 800;     // quest ids of the next-zone world
constexpr std::uint32_t NEXT_ZONE_ENTRIES = 91000;  // its creature entries
constexpr float BOT_X = -9465.0f;                   // Bot()'s spot
constexpr float BOT_Y = 40.0f;
constexpr float FAR_BIND_SECONDS = 900.0f;  // the route back from a bind on another map

struct Check
{
    std::string name;
    bool pass{false};
    std::string text;
};

// Questing WQ18a: every Rank and PlanGoals call of this harness runs again with the route's inputs, and Routes() reads
// the tally. With the circuit flag set (over a route worth taking), or the flag clear and no route (none, or one
// without an order), the result is the plain one; with a route its option never stands beside the circuit's option
// or a delivery on the bot's map. The two objects below hide Goals::Rank and Goals::PlanGoals from the cases
struct RouteTally
{
    std::uint32_t cases{0};
    std::uint32_t differs{0};   // a result that must equal the plain one does not
    std::uint32_t offered{0};   // cases whose route option was kept
    std::uint32_t beside{0};    // of them with the circuit's option or a same-map delivery beside it
    std::uint32_t replaced{0};  // of them whose plain result had the circuit's option or such a delivery
    std::uint32_t added{0};     // of them with a quest or move option the plain result lacks
    std::string first;          // the first fault
};

inline thread_local RouteTally routeTally;

// A route worth taking: one field order 150 yd from the bot
inline RoutePlan SampleRoute(Snapshot const& bot, float worth = 50000.0f)
{
    QuestOrder order;
    order.mapId = bot.position.map;
    order.x = bot.position.x + 150.0f;
    order.y = bot.position.y;
    order.cell = 1;
    order.expected = {{PLAIN_QUEST, 0, 5}};
    order.minutes = 5.0f;
    RoutePlan route;
    route.orders = {order};
    route.worth = worth;
    route.minutes = 6.0f;
    return route;
}

inline bool IsRoute(Option const& option)
{
    return option.kind == GoalKind::Quest && option.targetType == TargetType::Route;
}

// The circuit's option or a delivery on the bot's map: what a route replaces
inline bool OwnMapQuesting(Option const& option)
{
    return option.kind == GoalKind::Quest &&
           (option.reason == CIRCUIT_REASON || (option.targetType == TargetType::Quest && option.reason == "delivery"));
}

inline bool SameOptions(Ranked const& a, Ranked const& b)
{
    return a.ok == b.ok && a.failure == b.failure &&
           std::equal(a.options.begin(), a.options.end(), b.options.begin(), b.options.end(),
                      [](Option const& x, Option const& y)
                      {
                          return SameTarget(x, y) && x.reason == y.reason && x.value == y.value &&
                                 x.valuePerMinute == y.valuePerMinute && x.minutes == y.minutes;
                      });
}

inline bool SameRanked(Ranked const& a, Ranked const& b)
{
    return SameOptions(a, b) && a.considered == b.considered && a.referenceRate == b.referenceRate &&
           std::equal(a.dropped.begin(), a.dropped.end(), b.dropped.begin(), b.dropped.end(),
                      [](Dropped const& x, Dropped const& y)
                      { return x.kind == y.kind && x.target == y.target && std::string(x.reason) == y.reason; });
}

template <typename Ranker>
void NoteRouteWays(Snapshot const& bot, Situation const& situation, Ranked const& plain, Ranker const& rank)
{
    if (!situation.onCircuit)
        return;  // a route case of its own
    RouteTally& tally = routeTally;
    ++tally.cases;
    RoutePlan const none;
    RoutePlan const sample = SampleRoute(bot);
    auto const with = [&situation, &rank](bool onCircuit, RoutePlan const* route)
    {
        Situation other = situation;
        other.onCircuit = onCircuit;
        other.route = route;
        return rank(other);
    };
    auto const fault = [&tally](std::uint32_t& count, char const* what)
    {
        if (!count++ && tally.first.empty())
            tally.first = std::string(what) + " in case " + std::to_string(tally.cases);
    };
    if (!SameRanked(plain, with(true, &sample)))
        fault(tally.differs, "the flag set over a route");
    if (!SameRanked(plain, with(false, nullptr)))
        fault(tally.differs, "the flag clear and no route");
    if (!SameRanked(plain, with(false, &none)))
        fault(tally.differs, "a route without an order");
    Ranked const routed = with(false, &sample);
    if (std::none_of(routed.options.begin(), routed.options.end(), IsRoute))
    {
        if (!SameOptions(plain, routed))
            fault(tally.differs, "a dropped route");
        return;
    }
    ++tally.offered;
    if (std::any_of(routed.options.begin(), routed.options.end(), OwnMapQuesting))
        fault(tally.beside, "the circuit or a same-map delivery beside the route");
    if (std::any_of(plain.options.begin(), plain.options.end(), OwnMapQuesting))
        ++tally.replaced;
    if (plain.options.size() >= RANK_KEEP)
        return;  // the list was cut: an option the cut left out may come back
    for (Option const& option : routed.options)
        if (!IsRoute(option) && (option.kind == GoalKind::Quest || option.kind == GoalKind::MoveZone) &&
            std::none_of(plain.options.begin(), plain.options.end(), [&option](Option const& old)
                         { return SameTarget(old, option) && old.reason == option.reason; }))
            fault(tally.added, "a new quest or move option beside the route");
}

struct RankBothWays
{
    Ranked operator()(Snapshot const& bot, Situation const& situation) const
    {
        Ranked plain = Goals::Rank(bot, situation);
        NoteRouteWays(bot, situation, plain, [&bot](Situation const& other) { return Goals::Rank(bot, other); });
        return plain;
    }
};

struct PlanBothWays
{
    GoalPlan operator()(Snapshot const& bot, CircuitContext const& context, Situation const& situation) const
    {
        GoalPlan plain = Goals::PlanGoals(TakeFixture::Answered(bot, context), context, situation);
        NoteRouteWays(bot, situation, plain.ranked, [&bot, &context](Situation const& other)
                      { return Goals::PlanGoals(TakeFixture::Answered(bot, context), context, other).ranked; });
        return plain;
    }
};

inline constexpr RankBothWays Rank{};
inline constexpr PlanBothWays PlanGoals{};

inline TraitRules::BotTraits Traits(std::initializer_list<std::pair<TraitRules::Trait, int>> values)
{
    TraitRules::BotTraits traits;
    for (auto const& [trait, value] : values)
        traits.Values[TraitRules::Index(trait)] = static_cast<int8_t>(value);
    return traits;
}

inline Snapshot Bot(std::uint8_t level, std::uint32_t money)
{
    Snapshot bot;
    bot.bot = 1;
    bot.level = level;
    bot.race = HUMAN;
    bot.cls = WARRIOR;
    bot.position = {EASTERN, -9465.0f, 40.0f, 0.0f};
    bot.zone = ELWYNN;
    bot.area = GOLDSHIRE_AREA;
    bot.money = money;
    bot.bagSlots = 16;
    bot.bagFree = 8;
    bot.runSpeed = 7.0f;
    bot.hasHearthstone = true;
    bot.hearth = bot.position;
    return bot;
}

// The reserve a bot of this level keeps (copper): upkeep's real value is its own, the harness uses level^2 x 15
inline std::uint64_t ReserveAt(std::uint32_t level) { return static_cast<std::uint64_t>(level) * level * 15; }

inline Situation Reserved(Situation situation, std::uint32_t level)
{
    situation.reserveCopper = ReserveAt(level);
    return situation;
}

inline Situation Base(std::uint32_t level = 10)
{
    Situation situation;
    situation.reserveCopper = ReserveAt(level);
    situation.here.hub = HOME_HUB;
    situation.here.zoneLow = 1;
    situation.here.zoneHigh = 10;
    return situation;
}

// A circuit as PlanCircuit values it: XP + copper x the bot's need factor
inline CircuitPlan Circuit(Snapshot const& bot, Situation const& situation, float xp, float copper, float minutes)
{
    CircuitPlan plan;
    plan.ok = true;
    plan.start = bot.position;
    plan.returnTo = bot.position;
    plan.value = xp + copper * Prepare(bot, situation).need.factor;
    plan.minutes = minutes;
    return plan;
}

inline GrindSpot Spot(std::uint32_t id, float xpPerKill, float copperPerKill, std::uint32_t minLevel = 9,
                      std::uint32_t maxLevel = 11)
{
    GrindSpot spot;
    spot.id = id;
    spot.zone = ELWYNN;
    spot.place = {EASTERN, -9400.0f, 100.0f, 0.0f};
    spot.minLevel = minLevel;
    spot.maxLevel = maxLevel;
    spot.xpPerKill = xpPerKill;
    spot.copperPerKill = copperPerKill;
    spot.killsPerMinute = 1.0f;
    spot.travelMinutes = 2.0f;
    return spot;
}

inline HubOption Hub(std::uint32_t id, std::uint32_t zone, float rate, float travelMinutes)
{
    HubOption hub;
    hub.hub = id;
    hub.zone = zone;
    hub.place = {EASTERN, -9000.0f, 200.0f, 0.0f};
    hub.valuePerMinute = rate;
    hub.travelMinutes = travelMinutes;
    return hub;
}

inline Option Made(GoalKind kind, std::uint32_t target, float rate, float minutes = 60.0f)
{
    Option option;
    option.kind = kind;
    option.target = target;
    option.minutes = minutes;
    option.valuePerMinute = rate;
    option.value = rate * minutes;
    option.reason = "made";
    return option;
}

inline Ranked Fresh(std::vector<Option> options)
{
    Ranked ranked;
    ranked.ok = true;
    std::sort(options.begin(), options.end(), RankBefore);
    ranked.options = std::move(options);
    return ranked;
}

inline std::string Summary(Ranked const& ranked)
{
    if (!ranked.ok)
        return "failed: " + ranked.failure;
    std::string text;
    for (Option const& option : ranked.options)
    {
        text += text.empty() ? "" : "; ";
        text +=
            ChoiceWord(option) + " " + option.reason + " " + std::to_string(static_cast<int>(option.valuePerMinute));
        for (std::string const& note : option.traitNotes)
            text += " " + note;
    }
    return text.empty() ? "no options" : text;
}

inline Option const* Top(Ranked const& ranked) { return ranked.options.empty() ? nullptr : &ranked.options.front(); }

inline Option const* Find(Ranked const& ranked, GoalKind kind, std::uint32_t target)
{
    for (Option const& option : ranked.options)
        if (option.kind == kind && option.target == target)
            return &option;
    return nullptr;
}

inline bool Has(Ranked const& ranked, GoalKind kind)
{
    return std::any_of(ranked.options.begin(), ranked.options.end(),
                       [kind](Option const& option) { return option.kind == kind; });
}

inline bool HasNote(Option const& option, char const* note)
{
    return std::find(option.traitNotes.begin(), option.traitNotes.end(), note) != option.traitNotes.end();
}

inline bool TopIs(Ranked const& ranked, GoalKind kind, std::uint32_t target, char const* reason = nullptr)
{
    Option const* top = Top(ranked);
    return top && top->kind == kind && top->target == target && (!reason || top->reason == reason);
}

inline bool SameList(Ranked const& a, Ranked const& b)
{
    if (a.ok != b.ok || a.options.size() != b.options.size())
        return false;
    for (std::size_t i = 0; i < a.options.size(); ++i)
        if (ChoiceWord(a.options[i]) != ChoiceWord(b.options[i]) || a.options[i].reason != b.options[i].reason ||
            a.options[i].valuePerMinute != b.options[i].valuePerMinute)
            return false;
    return true;
}

inline void Add(std::vector<Check>& checks, std::string name, bool pass, std::string text)
{
    checks.push_back({std::move(name), pass, std::move(text)});
}

// A hub with a plain kill quest and one against elite mobs whose statistics show deaths: the careful bot's circuit
// skips the elite one through the circuit's own Risky rule, fed by the caution weight
inline QuestIndex EliteQuests()
{
    QuestGather gather;
    gather.places.push_back({EASTERN, -9465.0f, 45.0f, 0.0f, GIVER, false, ELWYNN, GOLDSHIRE_AREA, NO_CAMP});
    std::uint32_t target = 90101;
    for (std::uint32_t id : {PLAIN_QUEST, ELITE_QUEST})
    {
        QuestRecord quest;
        quest.id = id;
        quest.level = 10;
        quest.minLevel = 7;
        quest.xpBase = id == PLAIN_QUEST ? 400 : 900;
        quest.objectives.push_back({ObjectiveKind::Kill, target, id == PLAIN_QUEST ? 8u : 3u, 0, {}});
        float const x = id == PLAIN_QUEST ? -9315.0f : -9465.0f;
        float const y = id == PLAIN_QUEST ? 40.0f : 220.0f;
        for (float const offset : {-8.0f, 0.0f, 8.0f})
            gather.targets.push_back({target, false, EASTERN, x + offset, y + offset, 0.0f});
        gather.relations.push_back({id, GIVER, false, false});
        gather.relations.push_back({id, GIVER, false, true});
        gather.quests.push_back(std::move(quest));
        ++target;
    }
    return QuestIndex::Build(std::move(gather));
}

inline bool Takes(CircuitPlan const& plan, std::uint32_t quest)
{
    return std::any_of(plan.take.begin(), plan.take.end(),
                       [quest](CircuitTake const& take) { return take.quest == quest; });
}

inline bool Skips(CircuitPlan const& plan, std::uint32_t quest, SkipReason reason)
{
    return std::any_of(plan.skipped.begin(), plan.skipped.end(), [quest, reason](CircuitSkip const& skip)
                       { return skip.quest == quest && skip.reason == reason; });
}

inline void Broke(std::vector<Check>& checks)
{
    for (std::uint32_t const money : {0u, FLUSH})
    {
        Snapshot bot = Bot(10, money);
        Situation situation = Base();
        CircuitPlan const circuit = Circuit(bot, situation, 1500.0f, 200.0f, 20.0f);
        situation.circuit = &circuit;
        situation.spots = {Spot(11, 75.0f, 10.0f), Spot(12, 45.0f, 50.0f)};
        Ranked const ranked = Rank(bot, situation);
        if (!money)
            Add(checks, "money: a broke bot chooses money-making", TopIs(ranked, GoalKind::Money, 12, "gold_short"),
                Summary(ranked));
        else
        {
            Option const* spot = Find(ranked, GoalKind::Grind, 12);
            Add(checks, "money: a flush bot does not choose money-making, the same spot is a grind",
                Top(ranked) && Top(ranked)->kind != GoalKind::Money && spot, Summary(ranked));
        }
    }
}

inline void Errands(std::vector<Check>& checks)
{
    Snapshot bot = Bot(10, FLUSH);
    bot.bagFree = 0;
    Situation situation = Base();
    CircuitPlan const circuit = Circuit(bot, situation, 1500.0f, 200.0f, 20.0f);
    situation.circuit = &circuit;
    situation.bags.sellableSlots = 10;
    situation.bags.vendor = {77, 1.0f, true};
    Ranked const ranked = Rank(bot, situation);
    Add(checks, "errand: full bags near a vendor sell", TopIs(ranked, GoalKind::Errand, 77, "bags_full_vendor_near"),
        Summary(ranked));

    // WPC9: above the gather status's bag limit no circuit is planned (GatherLive FillGatherCircuit asks the limit);
    // the errand the bot then needs ranks
    Snapshot over = bot;
    over.bagSlots = 16;
    over.bagFree = 3;
    Snapshot under = bot;
    under.bagSlots = 48;
    under.bagFree = 10;
    std::uint32_t const overUsed = Gather::BagUsedPercent(over.bagSlots, over.bagFree);
    std::uint32_t const underUsed = Gather::BagUsedPercent(under.bagSlots, under.bagFree);
    Ranked const overRanked = Rank(over, situation);
    Add(checks, "errand (WPC9): a gatherer at 81 % gets no circuit and its sell errand ranks, at 79 % the circuit is",
        overUsed == 81 && Gather::BagsTooFull(overUsed) && Has(overRanked, GoalKind::Errand) && underUsed == 79 &&
            !Gather::BagsTooFull(underUsed),
        Summary(overRanked) + " " + std::to_string(overUsed) + " " + std::to_string(underUsed));

    ReplanInput in;
    in.current = {GoalKind::Quest, HOME_HUB, 0.0f, false, false};
    in.fresh = &ranked;
    in.delta.bagsFullNearVendor = true;
    Replan const replan = ReplanDecision(in);
    Add(checks, "errand: the vendor stop is picked up and the circuit goes on",
        replan.verdict == ReplanVerdict::PickUp && replan.reason == "opportunity_bags_full" && replan.chosen &&
            replan.chosen->kind == GoalKind::Errand,
        std::string(ReplanWord(replan.verdict)) + " " + replan.reason);

    Situation distant = situation;
    distant.bags.vendor.walkMinutes = WorldTravel::ERRAND_MAX_WALK_MINUTES + 5.0f;
    Ranked const farRanked = Rank(bot, distant);
    Add(checks, "errand: no trip to a vendor beyond the walk limit", !Has(farRanked, GoalKind::Errand),
        Summary(farRanked));

    Snapshot half = bot;
    half.bagFree = 8;
    Ranked const halfRanked = Rank(half, situation);
    Add(checks, "errand: half-full bags make no trip", !Has(halfRanked, GoalKind::Errand), Summary(halfRanked));

    // The craft queue (professions WR14b): worth its walk to the station, a trip of its own naming no vendor; no
    // worth, a worth below the errand rules' detour, or a station beyond the walk limit, makes none
    auto const crafts = [](Ranked const& ranked)
    {
        return std::any_of(
            ranked.options.begin(), ranked.options.end(), [](Option const& option)
            { return option.kind == GoalKind::Errand && option.target == 0 && option.reason == "craft"; });
    };
    Situation queued = situation;
    queued.craft = {1000000.0f, 3.0f, 2.0f};
    Ranked const queuedRanked = Rank(half, queued);
    Add(checks, "errand: a craft queue worth its walk makes a craft trip", crafts(queuedRanked), Summary(queuedRanked));
    Situation worthless = queued;
    worthless.craft.worth = 0.0f;
    Ranked const worthlessRanked = Rank(half, worthless);
    Add(checks, "errand: a craft queue worth nothing makes no trip", !Has(worthlessRanked, GoalKind::Errand),
        Summary(worthlessRanked));
    Situation little = queued;
    little.craft.worth = 1.0f;
    Ranked const littleRanked = Rank(half, little);
    Add(checks, "errand: a craft queue worth less than the errand rules' detour makes no trip", !crafts(littleRanked),
        Summary(littleRanked));
    Situation farStation = queued;
    farStation.craft.walkMinutes = WorldTravel::ERRAND_MAX_WALK_MINUTES + 5.0f;
    Ranked const farStationRanked = Rank(half, farStation);
    Add(checks, "errand: no craft trip to a station beyond the walk limit", !crafts(farStationRanked),
        Summary(farStationRanked));

    // Three quarters full: an orderly bot sells now, an untidy one waits
    for (int const order : {STRONG_TRAIT, -STRONG_TRAIT})
    {
        Snapshot fill = bot;
        fill.bagFree = 4;
        fill.traits = Traits({{TraitRules::Trait::Orderliness, order}});
        Situation some = situation;
        some.bags.sellableSlots = 8;
        Ranked const orderRanked = Rank(fill, some);
        bool const errand = Has(orderRanked, GoalKind::Errand);
        Add(checks, order > 0 ? "errand: an orderly bot sells three quarters full" : "errand: an untidy bot waits",
            order > 0 ? errand : !errand, Summary(orderRanked));
    }

    // A bag: bought by a flush bot, not by a broke one (WT2's ShouldBuyBag)
    for (std::uint32_t const money : {FLUSH, 300u})
    {
        Snapshot buyer = Bot(10, money);
        buyer.bagFree = 2;
        Situation offer = Base();
        offer.bagOffers = {{4496, 77, 1.0f, 400, 6, 0}};
        Ranked const bagRanked = Rank(buyer, offer);
        bool const buys = Has(bagRanked, GoalKind::BuyBag);
        Add(checks, money == FLUSH ? "bag: a flush bot buys a bag worth its price" : "bag: a broke bot does not",
            money == FLUSH ? buys : !buys, Summary(bagRanked));
    }

    // The live Situation's inputs (WT63c, InterludeInputs.h): the nearest vendor, the nearest repairer behind it, and
    // the bags larger than the smallest one carried within the walk limit
    constexpr float RUN_YARDS_PER_SECOND = 7.0f;
    Situation live = Base();
    FillErrandInputs(live, {{77, 60.0f, false}, {78, 400.0f, true}, {79, 9000.0f, true}},
                     {{4496, 80, 120.0f, 400, 6}, {4497, 80, 120.0f, 300, 4}, {4498, 81, 9000.0f, 400, 10}},
                     RUN_YARDS_PER_SECOND, 4);
    bool const filled = live.bags.vendor.id == 77 && !live.bags.vendor.repairs && live.bags.vendor.walkMinutes > 0.0f &&
                        live.bags.repairer.id == 78 && live.bags.repairer.repairs && live.bagOffers.size() == 1 &&
                        live.bagOffers[0].item == 4496 && live.bagOffers[0].vendor == 80 &&
                        live.bagOffers[0].replacedSlots == 4;
    Add(checks, "live inputs: the nearest vendor, the nearest repairer and the bag offers in reach", filled,
        std::to_string(live.bags.vendor.id) + " " + std::to_string(live.bags.repairer.id) + " " +
            std::to_string(live.bagOffers.size()));
    Snapshot worn = Bot(10, FLUSH);
    worn.durabilityPercent = 5;
    worn.lowestDurabilityPercent = 0;
    worn.brokenItems = 1;
    worn.repairCheapest = 40;
    Ranked const wornRanked = Rank(worn, live);
    bool const repairs =
        std::any_of(wornRanked.options.begin(), wornRanked.options.end(), [](Option const& option)
                    { return option.kind == GoalKind::Errand && option.target == 78 && option.reason == "repair"; });
    Add(checks, "live inputs: worn gear goes to the repairer, not to the nearer vendor that does not repair", repairs,
        Summary(wornRanked));
    Snapshot buyer = Bot(10, FLUSH);
    buyer.bagFree = 2;
    Ranked const liveBags = Rank(buyer, live);
    Add(checks, "live inputs: the bag offer is weighed", Has(liveBags, GoalKind::BuyBag), Summary(liveBags));
}

inline void Careful(std::vector<Check>& checks)
{
    QuestIndex const quests = EliteQuests();
    QuestStatsView stats;
    QuestStatSummary summary;
    summary.accepted = 12;
    summary.completed = 10;
    summary.samples = 10;
    summary.medianMinutes = 8.0f;
    summary.deathsPerCompletion = ELITE_DEATHS;
    stats.rows[StatKey(ELITE_QUEST, LevelBandOf(10))] = summary;
    CircuitContext context;
    context.quests = &quests;
    context.stats = &stats;

    for (int const caution : {STRONG_TRAIT, -STRONG_TRAIT, 0})
    {
        Snapshot bot = Bot(10, FLUSH);
        if (caution)
            bot.traits = Traits({{TraitRules::Trait::Caution, caution}});
        GoalPlan const plan = PlanGoals(TakeFixture::Answered(bot, context), context, Base());
        std::string const text = "take " + std::to_string(plan.circuit.take.size()) + ", " + Summary(plan.ranked);
        if (caution > 0)
            Add(checks, "careful: a careful bot's circuit skips the elite quest as risky",
                Skips(plan.circuit, ELITE_QUEST, SkipReason::Risky) && Takes(plan.circuit, PLAIN_QUEST) &&
                    TopIs(plan.ranked, GoalKind::Quest, HOME_HUB),
                text);
        else
            Add(checks, caution < 0 ? "careful: a daring bot takes the elite quest" : "careful: a neutral bot takes it",
                Takes(plan.circuit, ELITE_QUEST) && Takes(plan.circuit, PLAIN_QUEST), text);
    }

    // The same weight at the goal level: an elite grinding spot is refused by the careful bot, the daring one goes
    for (int const caution : {STRONG_TRAIT, -STRONG_TRAIT})
    {
        Snapshot bot = Bot(10, FLUSH);
        bot.traits = Traits({{TraitRules::Trait::Caution, caution}});
        Situation situation = Base();
        GrindSpot elite = Spot(21, 150.0f, 5.0f);
        elite.eliteShare = 0.8f;
        elite.killsPerMinute = 0.8f;
        situation.spots = {elite, Spot(22, 60.0f, 5.0f)};
        Ranked const ranked = Rank(bot, situation);
        bool const refused = std::any_of(ranked.dropped.begin(), ranked.dropped.end(), [](Dropped const& dropped)
                                         { return dropped.target == 21 && dropped.reason == "too_risky:careful"; });
        if (caution > 0)
            Add(checks, "careful: a careful bot refuses the elite spot", refused && TopIs(ranked, GoalKind::Grind, 22),
                Summary(ranked));
        else
            Add(checks, "careful: a daring bot grinds the elite spot", TopIs(ranked, GoalKind::Grind, 21),
                Summary(ranked));
    }
}

inline void Curious(std::vector<Check>& checks)
{
    for (int const curiosity : {STRONG_TRAIT, 0, -STRONG_TRAIT})
    {
        Snapshot bot = Bot(10, FLUSH);
        if (curiosity)
            bot.traits = Traits({{TraitRules::Trait::Curiosity, curiosity}});
        Situation situation = Base();
        CircuitPlan const circuit = Circuit(bot, situation, 400.0f, 0.0f, 20.0f);
        situation.circuit = &circuit;
        HubOption unexplored = Hub(22, ELWYNN, 90.0f, 5.0f);
        unexplored.explored = false;
        situation.hubs = {Hub(21, ELWYNN, 100.0f, 5.0f), unexplored};
        Ranked const ranked = Rank(bot, situation);
        if (curiosity > 0)
            Add(checks, "curious: a curious bot takes the unexplored hub",
                TopIs(ranked, GoalKind::Quest, 22, "unexplored") && HasNote(*Top(ranked), "curious"), Summary(ranked));
        else
            Add(checks,
                curiosity < 0 ? "curious: an incurious bot keeps to the known hub"
                              : "curious: a neutral bot takes the better known hub",
                TopIs(ranked, GoalKind::Quest, 21), Summary(ranked));
    }
}

inline void MidTrip(std::vector<Check>& checks)
{
    ReplanInput in;
    in.current = {GoalKind::Quest, 30, 100.0f, false, true};
    in.lastPlanMs = 1000;
    in.nowMs = 2000;

    Ranked const same = Fresh({Made(GoalKind::Quest, 30, 100.0f)});
    in.fresh = &same;
    in.delta.giver = PassingGiver{900, 601, 1.0f, 600.0f, true};
    Replan const pick = ReplanDecision(in);
    Add(checks, "mid-trip: the bot picks up a quest from a giver it passes and goes on",
        pick.verdict == ReplanVerdict::PickUp && pick.reason == "opportunity_giver" && pick.giver &&
            pick.giver->quest == 601,
        std::string(ReplanWord(pick.verdict)) + " " + pick.reason);

    Ranked const better = Fresh({Made(GoalKind::Quest, 31, 200.0f), Made(GoalKind::Quest, 30, 100.0f)});
    in.fresh = &better;
    Replan const breakOff = ReplanDecision(in);
    Add(checks, "mid-trip: the bot breaks off its trip for a clearly better quest",
        breakOff.verdict == ReplanVerdict::Switch && breakOff.reason == "opportunity_better" && breakOff.chosen &&
            breakOff.chosen->target == 31,
        std::string(ReplanWord(breakOff.verdict)) + " " + breakOff.reason);

    Ranked const slightly = Fresh({Made(GoalKind::Quest, 31, 115.0f), Made(GoalKind::Quest, 30, 100.0f)});
    in.fresh = &slightly;
    in.delta.giver.reset();
    in.delta.situation = true;
    Replan const stay = ReplanDecision(in);
    Add(checks, "mid-trip: a slightly better quest does not break the trip",
        stay.verdict == ReplanVerdict::Stay && stay.reason == "current_best", std::string(ReplanWord(stay.verdict)));

    in.delta.situation = false;
    in.fresh = &same;
    in.delta.giver = PassingGiver{901, 602, 3.0f, 50.0f, true};
    Replan const detour = ReplanDecision(in);
    Add(checks, "mid-trip: a giver whose detour costs more than its quest is passed by",
        detour.verdict == ReplanVerdict::Stay, std::string(ReplanWord(detour.verdict)) + " " + detour.reason);

    in.delta.giver = PassingGiver{902, 603, 0.5f, 600.0f, false};
    Replan const refused = ReplanDecision(in);
    Add(checks, "mid-trip: a quest the circuit would not take is not picked up", refused.verdict == ReplanVerdict::Stay,
        std::string(ReplanWord(refused.verdict)));

    in.delta.giver.reset();
    Replan const quiet = ReplanDecision(in);
    Add(checks, "replan: no trigger, no re-planning",
        quiet.verdict == ReplanVerdict::Stay && quiet.reason == "no_trigger", quiet.reason);

    in.nowMs = in.lastPlanMs + REPLAN_INTERVAL_MS;
    in.fresh = &better;
    in.current.travelling = false;
    Replan const interval = ReplanDecision(in);
    Add(checks, "replan: every few minutes the bot re-plans",
        interval.verdict == ReplanVerdict::Switch && interval.reason == "interval", interval.reason);

    in.current.ended = true;
    Replan const ended = ReplanDecision(in);
    Add(checks, "replan: a goal that ended always re-plans",
        ended.verdict == ReplanVerdict::Switch && ended.reason == "goal_ended" && ended.chosen &&
            ended.chosen->target == 31,
        ended.reason);
}

inline void NoFlapping(std::vector<Check>& checks)
{
    ReplanInput in;
    in.current = {GoalKind::Quest, 40, 100.0f, false, false};
    in.delta.situation = true;
    std::uint32_t switches = 0;
    for (std::uint32_t round = 0; round < 20; ++round)
    {
        bool const odd = round % 2;
        Ranked const fresh =
            Fresh({Made(GoalKind::Quest, 40, odd ? 100.0f : 108.0f), Made(GoalKind::Grind, 41, odd ? 110.0f : 95.0f)});
        in.fresh = &fresh;
        Replan const replan = ReplanDecision(in);
        if (replan.verdict == ReplanVerdict::Switch && replan.chosen)
        {
            ++switches;
            in.current = {replan.chosen->kind, replan.chosen->target, replan.chosen->valuePerMinute, false, false};
        }
    }
    Add(checks, "hysteresis: goals within the margin never flap", !switches,
        std::to_string(switches) + " switches in 20 rounds");

    Ranked const clear = Fresh({Made(GoalKind::Grind, 41, 140.0f), Made(GoalKind::Quest, 40, 100.0f)});
    in.fresh = &clear;
    Replan const first = ReplanDecision(in);
    in.current = {GoalKind::Grind, 41, 140.0f, false, false};
    Ranked const back = Fresh({Made(GoalKind::Quest, 40, 150.0f), Made(GoalKind::Grind, 41, 140.0f)});
    in.fresh = &back;
    Replan const second = ReplanDecision(in);
    Add(checks, "hysteresis: a clear gain switches once and does not switch back",
        first.verdict == ReplanVerdict::Switch && second.verdict == ReplanVerdict::Stay,
        std::string(ReplanWord(first.verdict)) + ", then " + ReplanWord(second.verdict));

    Weights patient;
    patient.patience = 0.8f;
    Weights driven;
    driven.drive = 0.8f;
    in.current = {GoalKind::Quest, 40, 100.0f, false, false};
    Ranked const gain = Fresh({Made(GoalKind::Grind, 41, 130.0f), Made(GoalKind::Quest, 40, 100.0f)});
    in.fresh = &gain;
    in.weights = patient;
    Replan const patientReplan = ReplanDecision(in);
    in.weights = Weights{};
    Replan const neutralReplan = ReplanDecision(in);
    Add(checks, "hysteresis: patience widens the margin, drive narrows it",
        ReplanMargin(patient) > ReplanMargin(Weights{}) && ReplanMargin(Weights{}) > ReplanMargin(driven) &&
            patientReplan.verdict == ReplanVerdict::Stay && neutralReplan.verdict == ReplanVerdict::Switch,
        std::string("patient ") + ReplanWord(patientReplan.verdict) + ", neutral " + ReplanWord(neutralReplan.verdict));

    in.delta.situation = false;
    in.delta.group = true;
    Ranked const group = Fresh({Made(GoalKind::Quest, 42, 120.0f), Made(GoalKind::Quest, 40, 100.0f)});
    in.fresh = &group;
    Replan const joined = ReplanDecision(in);
    Add(checks, "replan: a group doing the same quest lowers the margin",
        joined.verdict == ReplanVerdict::Switch && joined.reason == "opportunity_group", joined.reason);
}

// The next-zone world (WT20-b): quest camps east of the bot's Goldshire spot, each with two quests and a services hub
// around it. Every zone lies on the bot's map except Darkshore, at the bot's own coordinates on Kalimdor
struct ZoneCamp
{
    char const* name;
    std::uint32_t map;
    float east;  // yards east of the bot
    std::uint32_t zone;
    std::int32_t questLevel;
    std::uint32_t minLevel;
    std::uint32_t raceMask;
};

constexpr ZoneCamp NEXT_ZONE_CAMPS[] = {
    {"home", EASTERN, 0.0f, ELWYNN, 10, 1, 0},       {"X", EASTERN, 600.0f, WETLANDS, 17, 10, 0},
    {"W1", EASTERN, 1000.0f, WESTFALL, 12, 8, 0},    {"E", EASTERN, 1200.0f, SILVERPINE, 14, 10, 0},
    {"R", EASTERN, 1500.0f, REDRIDGE, 16, 12, 0},    {"L", EASTERN, 2000.0f, LOCH_MODAN, 14, 10, 0},
    {"W2", EASTERN, 2200.0f, WESTFALL, 12, 8, 0},    {"S1", EASTERN, 2500.0f, STONETALON, 16, 12, HORDE_RACES},
    {"S2", EASTERN, 3000.0f, STONETALON, 16, 12, 0}, {"A", EASTERN, 5000.0f, ASHENVALE, 17, 12, 0},
    {"K", KALIMDOR, 0.0f, DARKSHORE, 14, 10, 0}};
constexpr std::size_t HOME_CAMP = 0;
constexpr std::size_t W1_CAMP = 2;
constexpr std::size_t R_CAMP = 4;
constexpr std::size_t L_CAMP = 5;
constexpr std::size_t W2_CAMP = 6;
constexpr std::size_t S2_CAMP = 8;

struct NextZoneWorld
{
    QuestIndex quests;
    PlannerIndexes indexes;
    std::vector<std::uint32_t> hubs;  // per NEXT_ZONE_CAMPS entry
};

inline NextZoneWorld NextZones()
{
    GatherInput services;
    services.zones = {
        {ELWYNN, 1, 10, ZoneSide::Alliance, EASTERN},       {WETLANDS, 20, 30, ZoneSide::Alliance, EASTERN},
        {WESTFALL, 10, 20, ZoneSide::Alliance, EASTERN},    {SILVERPINE, 10, 20, ZoneSide::Horde, EASTERN},
        {REDRIDGE, 15, 25, ZoneSide::Alliance, EASTERN},    {LOCH_MODAN, 10, 20, ZoneSide::Alliance, EASTERN},
        {STONETALON, 15, 27, ZoneSide::Contested, EASTERN}, {ASHENVALE, 15, 30, ZoneSide::Contested, EASTERN},
        {DARKSHORE, 10, 20, ZoneSide::Alliance, KALIMDOR}};
    QuestGather gather;
    std::uint32_t entry = NEXT_ZONE_ENTRIES;
    std::uint32_t questId = NEXT_ZONE_QUESTS;
    for (ZoneCamp const& camp : NEXT_ZONE_CAMPS)
    {
        float const x = BOT_X + camp.east;
        for (float const dx : {-10.0f, 10.0f})
            for (float const dy : {-10.0f, 10.0f})
                services.spawns.push_back(
                    {camp.map, x + dx, BOT_Y + dy, 0.0f, ++entry, NpcFlag::VENDOR, 0, camp.zone, camp.zone, 0, false});
        std::uint32_t const giver = ++entry;
        gather.places.push_back({camp.map, x, BOT_Y + 5.0f, 0.0f, giver, false, camp.zone, camp.zone, NO_CAMP});
        for (float const north : {0.0f, 60.0f})
        {
            QuestRecord quest;
            quest.id = ++questId;
            quest.level = camp.questLevel;
            quest.minLevel = camp.minLevel;
            quest.raceMask = camp.raceMask;
            quest.xpBase = 400;
            std::uint32_t const target = ++entry;
            quest.objectives.push_back({ObjectiveKind::Kill, target, 6u, 0, {}});
            for (float const offset : {-8.0f, 0.0f, 8.0f})
                gather.targets.push_back({target, false, camp.map, x + 60.0f + offset, BOT_Y + north + offset, 0.0f});
            gather.relations.push_back({quest.id, giver, false, false});
            gather.relations.push_back({quest.id, giver, false, true});
            gather.quests.push_back(std::move(quest));
        }
    }
    NextZoneWorld world{QuestIndex::Build(std::move(gather)), PlannerIndexes::Build(std::move(services)), {}};
    for (ZoneCamp const& camp : NEXT_ZONE_CAMPS)
    {
        std::vector<Nearby> const hub = world.indexes.NearestHubs(camp.map, BOT_X + camp.east, BOT_Y, 0.0f, 1);
        world.hubs.push_back(hub.empty() ? NO_HUB : hub.front().index);
    }
    return world;
}

// The hubs the live fill offers, by camp name
inline std::vector<std::uint32_t> Filled(Snapshot const& input, CircuitContext const& context, Situation& situation)
{
    Snapshot const bot = TakeFixture::Answered(input, context);  // the fixtures' bots are no players (WQ41b)
    FillFromIndexes(bot, context, Prepare(bot, situation), situation);
    std::vector<std::uint32_t> hubs;
    for (HubOption const& hub : situation.hubs)
        hubs.push_back(hub.hub);
    return hubs;
}

inline std::string CampNames(NextZoneWorld const& world, std::vector<std::uint32_t> const& hubs)
{
    std::string text;
    for (std::uint32_t const hub : hubs)
    {
        auto const at = std::find(world.hubs.begin(), world.hubs.end(), hub);
        text += text.empty() ? "" : ",";
        text += at == world.hubs.end() ? std::to_string(hub) : NEXT_ZONE_CAMPS[at - world.hubs.begin()].name;
    }
    return text.empty() ? "none" : text;
}

// The live fill (FillFromIndexes): the zone's bracket from the indexes, and for a bot past it the next zones' hubs
inline void Outlevelled(std::vector<Check>& checks)
{
    NextZoneWorld const world = NextZones();
    CircuitContext context;
    context.quests = &world.quests;
    context.indexes = &world.indexes;
    Situation live;
    live.here.hub = NO_HUB;  // as SituationOf leaves it
    auto const moves = [](Ranked const& ranked)
    {
        return std::count_if(ranked.options.begin(), ranked.options.end(),
                             [](Option const& option) { return option.kind == GoalKind::MoveZone; });
    };

    // Grey quests at home leave a dry circuit (the circuit skips grey quests), so the estimate of an unknown hub wins
    Snapshot const outlevelled = Bot(16, FLUSH);
    GoalPlan const moved = PlanGoals(TakeFixture::Answered(outlevelled, context), context, Reserved(live, 16));
    Add(checks, "outlevelled: grey quests at home leave a dry circuit and the nearest next zone's hub wins",
        moved.circuit.take.empty() && moved.hub == world.hubs[HOME_CAMP] &&
            TopIs(moved.ranked, GoalKind::MoveZone, world.hubs[W1_CAMP], "outlevelled") && moves(moved.ranked) >= 2,
        Summary(moved.ranked));

    Snapshot const inBracket = Bot(10, FLUSH);
    Situation kept = live;
    std::vector<std::uint32_t> const none = Filled(inBracket, context, kept);
    GoalPlan const stays = PlanGoals(TakeFixture::Answered(inBracket, context), context, Reserved(live, 10));
    Add(checks, "outlevelled: a bot within its bracket keeps questing here and gets no next zones",
        none.empty() && kept.here.zoneLow == 1 && kept.here.zoneHigh == 10 &&
            TopIs(stays.ranked, GoalKind::Quest, world.hubs[HOME_CAMP], "circuit") && !moves(stays.ranked),
        "hubs " + CampNames(world, none) + "; " + Summary(stays.ranked));

    // Wetlands is above the bracket, Silverpine the enemy's, Stonetalon's first camp gives Horde quests only,
    // Westfall's second camp is a zone already listed, Ashenvale past the cap, Darkshore on another map
    Situation listed = live;
    std::vector<std::uint32_t> const hubs = Filled(outlevelled, context, listed);
    std::vector<std::uint32_t> const expected = {world.hubs[W1_CAMP], world.hubs[R_CAMP], world.hubs[L_CAMP],
                                                 world.hubs[S2_CAMP]};
    Add(checks, "next zones: the nearest hubs of zones in the bracket, one per zone, none of the enemy's, at most four",
        hubs == expected && listed.hubs.front().zone == WESTFALL && listed.hubs.front().travelMinutes > 0.0f &&
            listed.hubs.front().travelMinutes < listed.hubs.back().travelMinutes,
        "hubs " + CampNames(world, hubs));

    Situation beyond = live;
    std::vector<std::uint32_t> const past = Filled(Bot(40, FLUSH), context, beyond);
    GoalPlan const nowhere = PlanGoals(TakeFixture::Answered(Bot(40, FLUSH), context), context, Reserved(live, 40));
    CircuitContext bare = context;
    bare.indexes = nullptr;
    Situation unset = live;
    std::vector<std::uint32_t> const unknown = Filled(outlevelled, bare, unset);
    Add(checks, "next zones: no zone in the bracket or no indexes yet gives no hub options",
        past.empty() && beyond.here.zoneHigh == 10 && nowhere.ranked.ok && !moves(nowhere.ranked) && unknown.empty() &&
            !unset.here.zoneHigh,
        "level 40 " + CampNames(world, past) + "; no indexes " + CampNames(world, unknown));

    // A travel cost that finds no way to Westfall's first hub: its second hub stands for the zone. Its straight line
    // prices Darkshore too: a zone on another map with a way there is offered beside the bot's own map's (WN1b)
    auto const& blocked = world.indexes.hubs[world.hubs[W1_CAMP]];
    CircuitContext costed = context;
    costed.travel = [&blocked](Place const& from, Place const& to)
    {
        return std::hypot(to.x - blocked.x, to.y - blocked.y) < 1.0f ? -1.0f
                                                                     : std::hypot(to.x - from.x, to.y - from.y) / 7.0f;
    };
    Situation reached = live;
    std::vector<std::uint32_t> const reachable = Filled(outlevelled, costed, reached);
    Add(checks, "next zones: a hub without a way there is skipped",
        reachable.size() == NEXT_ZONE_HUBS + 1 && reachable.back() == world.hubs.back() &&
            std::find(reachable.begin(), reachable.end(), world.hubs[W1_CAMP]) == reachable.end() &&
            std::find(reachable.begin(), reachable.end(), world.hubs[W2_CAMP]) != reachable.end(),
        "hubs " + CampNames(world, reachable));
}

inline void Busy(std::vector<Check>& checks)
{
    for (int const social : {STRONG_TRAIT, -STRONG_TRAIT})
    {
        Snapshot bot = Bot(10, FLUSH);
        bot.traits = Traits({{TraitRules::Trait::Sociability, social}});
        Situation situation = Base();
        CircuitPlan const circuit = Circuit(bot, situation, 400.0f, 0.0f, 20.0f);
        situation.circuit = &circuit;
        HubOption busy = Hub(51, ELWYNN, 100.0f, 5.0f);
        busy.botsAtHub = 20;
        busy.competingBots = 4;
        situation.hubs = {busy, Hub(52, ELWYNN, 80.0f, 5.0f)};
        Ranked const ranked = Rank(bot, situation);
        if (social > 0)
            Add(checks, "busy hub: a gregarious bot tolerates a busy hub",
                TopIs(ranked, GoalKind::Quest, 51) && HasNote(*Top(ranked), "gregarious"), Summary(ranked));
        else
            Add(checks, "busy hub: a solitary bot goes to the quiet hub", TopIs(ranked, GoalKind::Quest, 52),
                Summary(ranked));
    }

    // Family rule 10: crowding costs only where bots compete for kills
    Snapshot bot = Bot(10, FLUSH);
    Situation situation = Base();
    HubOption town = Hub(53, ELWYNN, 100.0f, 5.0f);
    town.botsAtHub = 20;
    HubOption contested = town;
    contested.hub = 54;
    contested.competingBots = 4;
    situation.hubs = {town, contested};
    Ranked const ranked = Rank(bot, situation);
    Option const* quiet = Find(ranked, GoalKind::Quest, 53);
    Option const* fought = Find(ranked, GoalKind::Quest, 54);
    Add(checks, "busy hub: bots at a hub cost nothing, bots at its kills do",
        quiet && fought && quiet->crowd == 1.0f && quiet->traitFactor == 1.0f && fought->crowd < 1.0f, Summary(ranked));
}

inline void RestAndHearth(std::vector<Check>& checks)
{
    for (int const caution : {STRONG_TRAIT, -STRONG_TRAIT})
    {
        Snapshot bot = Bot(10, FLUSH);
        bot.traits = Traits({{TraitRules::Trait::Caution, caution}});
        Situation situation = Base();
        CircuitPlan const circuit = Circuit(bot, situation, 1500.0f, 200.0f, 20.0f);
        situation.circuit = &circuit;
        situation.healthPercent = 55.0f;
        Ranked const ranked = Rank(bot, situation);
        if (caution > 0)
            Add(checks, "rest: a careful bot rests at 55% health", TopIs(ranked, GoalKind::Rest, 0, "low_health"),
                Summary(ranked));
        else
            Add(checks, "rest: a daring bot fights on", !Has(ranked, GoalKind::Rest), Summary(ranked));
    }

    Snapshot bot = Bot(10, FLUSH);
    bot.hearth = {1, 9950.0f, 2280.0f, 0.0f};  // Darnassus: another map
    Situation situation = Base();
    situation.bindSeconds = FAR_BIND_SECONDS;  // the way back as the planner's route cost prices it (planning WN1b)
    Inn nearer{701, {EASTERN, -9460.0f, 20.0f, 0.0f}, 60.0f, true, false};
    Inn usual{702, {EASTERN, -9390.0f, 60.0f, 0.0f}, 75.0f, true, true};
    situation.inns = {nearer, usual};
    Ranked const neutral = Rank(bot, situation);
    Add(checks, "hearth: a far bind makes the nearest fitting inn a goal",
        TopIs(neutral, GoalKind::Hearth, 701, "bind_far"), Summary(neutral));
    Situation boundThere = situation;
    boundThere.inns[0].usual = true;
    boundThere.inns[1].usual = false;
    Ranked const sameInn = Rank(bot, boundThere);
    Add(checks, "hearth: the inn the bot is bound at is no bind goal (it saves nothing), the next one ranks",
        TopIs(sameInn, GoalKind::Hearth, 702, "bind_far"), Summary(sameInn));
    Snapshot atInn = Bot(10, FLUSH);
    Ranked const bound = Rank(atInn, situation);
    Add(checks, "hearth: a bind nearby needs no inn", !Has(bound, GoalKind::Hearth), Summary(bound));

    // WT63d: the live inputs (InterludeInputs.h InnInputs) from the innkeepers the bot's map thread found in the index
    std::vector<InnSeen> const seen = {{295, {EASTERN, -9460.0f, 20.0f, 0.0f}, 60.0f, true, 12},
                                       {6740, {EASTERN, -9390.0f, 60.0f, 0.0f}, 75.0f, true, 12}};
    auto const live = [&situation](std::vector<InnSeen> const& inns, std::vector<FailedAttempts::Entry> const& refused,
                                   Place const& bind)
    {
        Situation out = situation;
        out.inns = InnInputs(inns, bind, refused);
        return out;
    };
    // The record of failed attempts' live not_bound place of an inn spot (its expiry: failed-attempts-selftest)
    auto const notBoundAt = [](WorldTravel::SightPoint const& spot)
    {
        FailedAttempts::Record record;
        FailedAttempts::Failure failure;
        failure.goal = spot;
        FailedAttempts::NoteFailure(record, "abort:not_bound", failure, 900, 600000);
        return FailedAttempts::LiveEntries(record, 1000);
    };
    Snapshot farBound = Bot(10, FLUSH);
    farBound.hearth = {1, 9950.0f, 2280.0f, 0.0f};
    Ranked const seenFar = Rank(farBound, live(seen, {}, farBound.hearth));
    Add(checks, "hearth (live inputs): a far bind ranks the nearest seen inn, its innkeeper the target",
        TopIs(seenFar, GoalKind::Hearth, 295, "bind_far"), Summary(seenFar));
    Snapshot stoneless = farBound;
    stoneless.hasHearthstone = false;
    Ranked const noStone = Rank(stoneless, live(seen, {}, farBound.hearth));
    Add(checks, "hearth (live inputs): no hearthstone, no bind", !Has(noStone, GoalKind::Hearth), Summary(noStone));
    Ranked const nearBind = Rank(Bot(10, FLUSH), live(seen, {}, Bot(10, FLUSH).hearth));
    Add(checks, "hearth (live inputs): a bind nearby, no bind", !Has(nearBind, GoalKind::Hearth), Summary(nearBind));
    std::vector<InnSeen> unfit = seen;
    for (InnSeen& inn : unfit)
        inn.fits = false;
    Ranked const noFit = Rank(farBound, live(unfit, {}, farBound.hearth));
    Add(checks, "hearth (live inputs): no inn fits the level, no bind", !Has(noFit, GoalKind::Hearth), Summary(noFit));
    std::vector<InnSeen> beyond = seen;
    for (InnSeen& inn : beyond)
        inn.yards = WorldTravel::INN_MAX_WALK_SECONDS * farBound.runSpeed + 1.0f;
    Ranked const noReach = Rank(farBound, live(beyond, {}, farBound.hearth));
    Add(checks, "hearth (live inputs): no inn within the walk, no bind", !Has(noReach, GoalKind::Hearth),
        Summary(noReach));
    std::vector<FailedAttempts::Entry> const avoided = notBoundAt({EASTERN, -9461.0f, 21.0f, 0.0f});
    Ranked const skipped = Rank(farBound, live(seen, avoided, farBound.hearth));
    Add(checks, "hearth (live inputs): a spot whose bind found no innkeeper lately is skipped, the next inn ranks",
        TopIs(skipped, GoalKind::Hearth, 6740, "bind_far"), Summary(skipped));

    // WN1c invariant over the hearth states: a one-off turn-in in the circuit at hand does not move the bind's rate
    // (the bar is the rate the bot keeps up), so a turn-in at hand worth more than the bind goes first
    auto const hearthRate = [](Snapshot const& who, Situation state, float oneOff)
    {
        CircuitPlan circuit = Circuit(who, state, 1500.0f, 200.0f, 20.0f);
        circuit.value += oneOff;
        circuit.oneOffValue = oneOff;
        state.circuit = &circuit;
        Ranked const ranked = Rank(who, state);
        for (Option const& option : ranked.options)
            if (option.kind == GoalKind::Hearth)
                return option.valuePerMinute;
        return -1.0f;
    };
    std::vector<std::pair<Snapshot, Situation>> const hearthStates = {{bot, situation},
                                                                      {farBound, live(seen, {}, farBound.hearth)},
                                                                      {farBound, live(seen, avoided, farBound.hearth)}};
    std::string moved;
    for (auto const& [who, state] : hearthStates)
    {
        float const plain = hearthRate(who, state, 0.0f);
        float const paid = hearthRate(who, state, 6000.0f);
        if (plain <= 0.0f || std::abs(paid - plain) > plain * 1.0e-4f)
            moved += " " + std::to_string(plain) + "->" + std::to_string(paid);
    }
    Add(checks, "hearth invariant: a one-off turn-in at hand moves no bind's rate", moved.empty(), moved);
    Situation atEnder = live(seen, {}, farBound.hearth);
    for (Inn& inn : atEnder.inns)
        inn.yards = 420.0f * farBound.runSpeed;  // Auberdine's inn unfit, the next 420 s away
    atEnder.bindSeconds = 560.0f;                // the crossing back to the bind
    CircuitPlan turnIn = Circuit(farBound, atEnder, 100.0f, 0.0f, 1.0f);
    turnIn.value += 4800.0f;
    turnIn.oneOffValue = 4800.0f;
    atEnder.circuit = &turnIn;
    Ranked const ender = Rank(farBound, atEnder);
    Add(checks, "hearth (WN1c delivery-far-A): the turn-in 70 yd away goes before the far bind",
        TopIs(ender, GoalKind::Quest, HOME_HUB, CIRCUIT_REASON) && Has(ender, GoalKind::Hearth), Summary(ender));
    // NE3: any live failure at an inn's place drops the inn (the inn trip's goal words, a place another goal failed at
    // within PLACE_YARDS), so a failed inn is not ordered again while its entry lives
    auto const failedAt = [](char const* word, WorldTravel::SightPoint const& spot)
    {
        FailedAttempts::Record record;
        FailedAttempts::Failure failure;
        failure.goal = spot;
        FailedAttempts::NoteFailure(record, word, failure, 900, 600000);
        return FailedAttempts::LiveEntries(record, 1000);
    };
    std::string kept;
    for (char const* word : {"abort:no_route", "abort:unreachable_poi", "trip_deaths", "inn_no_walk"})
    {
        std::vector<FailedAttempts::Entry> const refused = failedAt(word, {EASTERN, -9461.0f, 21.0f, 0.0f});
        Ranked const dropped = Rank(farBound, live(seen, refused, farBound.hearth));
        if (refused.empty() || !TopIs(dropped, GoalKind::Hearth, 6740, "bind_far") ||
            PickBindInn(InnInputs(seen, farBound.hearth, refused), 295, 1000.0f) != -1)
            kept += std::string(" ") + word;
    }
    Add(checks,
        "hearth (live inputs): an inn with a live no_route, unreachable_poi, trip_deaths or inn_no_walk entry is "
        "dropped, the next inn ranks",
        kept.empty(), kept);
    Ranked const beside =
        Rank(farBound, live(seen, failedAt("abort:no_route", {EASTERN, -9452.0f, 20.0f, 0.0f}), farBound.hearth));
    Ranked const apart =
        Rank(farBound, live(seen, failedAt("abort:no_route", {EASTERN, -9448.0f, 20.0f, 0.0f}), farBound.hearth));
    Add(checks, "hearth (live inputs): a failed place 8 yd from an inn's spot drops the inn, one 12 yd off does not",
        TopIs(beside, GoalKind::Hearth, 6740, "bind_far") && TopIs(apart, GoalKind::Hearth, 295, "bind_far"),
        Summary(beside) + " | " + Summary(apart));
    // NE3: an inn is usual when the bind is not far from it (a bind there saves nothing), so a bind that took 12-17 yd
    // off the spot leaves no inn of that town to order
    auto const usualIds = [](std::vector<Inn> const& inns)
    {
        std::string ids;
        for (Inn const& inn : inns)
            if (inn.usual)
                ids += " " + std::to_string(inn.id);
        return ids;
    };
    std::vector<Inn> const boundBeside = InnInputs(seen, {EASTERN, -9475.0f, 20.0f, 0.0f}, {});
    std::vector<Inn> const boundAway = InnInputs(seen, {EASTERN, -8360.0f, 20.0f, 0.0f}, {});
    std::vector<Inn> const boundElsewhere = InnInputs(seen, {1, -9460.0f, 20.0f, 0.0f}, {});
    Add(checks,
        "hearth (live inputs): an inn 15 yd from the bind is usual (and one 95 yd from it), none 1100 yd away or on "
        "another map",
        usualIds(boundBeside) == " 295 6740" && usualIds(boundAway).empty() && usualIds(boundElsewhere).empty(),
        usualIds(boundBeside) + " |" + usualIds(boundAway) + " |" + usualIds(boundElsewhere));
    Add(checks, "hearth (live inputs): the entry never picks a usual inn, ordered or not",
        PickBindInn(boundBeside, 295, 1000.0f) == -1 && PickBindInn(boundBeside, 0, 1000.0f) == -1 &&
            PickBindInn(boundAway, 295, 1000.0f) == 0,
        "pick");
    Add(checks, "hearth (live inputs): the entry picks the order's innkeeper, else the nearest fitting one",
        PickBindInn(InnInputs(seen, farBound.hearth, {}), 6740, 1000.0f) == 1 &&
            PickBindInn(InnInputs(seen, farBound.hearth, {}), 0, 1000.0f) == 0 &&
            PickBindInn(InnInputs(seen, farBound.hearth, {}), 9999, 1000.0f) == -1 &&
            PickBindInn(InnInputs(unfit, farBound.hearth, {}), 0, 1000.0f) == -1 &&
            PickBindInn(InnInputs(seen, farBound.hearth, {}), 0, 50.0f) == -1,
        "pick");
    // WT63d-b: the bound on a bind that does not take. Its spot goes to the record (the trip's not_bound end,
    // InnEndOf), so the next replan neither ranks that inn nor lets the entry start an order for it while it lives
    std::vector<FailedAttempts::Entry> failed;
    if (WorldTravel::InnEndOf(false, WorldTravel::InnStep::Bind).avoid)
        failed = notBoundAt({EASTERN, -9460.0f, 20.0f, 0.0f});
    Ranked const after = Rank(farBound, live(seen, failed, farBound.hearth));
    Add(checks, "hearth (live inputs): after a bind that did not take, that inn is neither ranked nor started again",
        !failed.empty() && !TopIs(after, GoalKind::Hearth, 295) &&
            PickBindInn(InnInputs(seen, farBound.hearth, failed), 295, 1000.0f) == -1,
        Summary(after));
}

// NR.5: the bind waits for a stay near the inn. Two inns 1426 yd apart and a work spot more than 1000 yd from both
// (NR2's back and forth, in Tanaris)
inline void HearthStays(std::vector<Check>& checks)
{
    constexpr std::uint32_t first = 7733;
    constexpr std::uint32_t second = 18542;
    constexpr std::uint32_t minute = 60000;
    constexpr std::uint32_t dwell = 15 * minute;
    Place const atFirst{1, -7159.0f, -3842.0f, 9.0f};
    Place const atSecond{1, -8159.0f, -4859.0f, -25.0f};
    Place const work{1, -7021.0f, -4873.0f, -3.5f};

    auto const seenFrom = [&](Place const& at)
    {
        auto const yards = [&at](Place const& to) { return std::hypot(to.x - at.x, to.y - at.y); };
        return std::vector<InnSeen>{{first, atFirst, yards(atFirst), true, 0},
                                    {second, atSecond, yards(atSecond), true, 0}};
    };

    // The stay rule
    std::vector<InnStay> stays;
    NoteInnStays(stays, seenFrom(atFirst), atFirst, minute);
    NoteInnStays(stays, seenFrom(atFirst), atFirst, 5 * minute);
    bool const entered = stays.size() == 1 && stays.front().entry == first && stays.front().sinceMs == minute;
    std::vector<Inn> inns = InnInputs(seenFrom(atFirst), work, {});
    MarkUnstayed(inns, stays, minute + dwell - 1, dwell);
    bool const waits = inns[0].unstayed && inns[1].unstayed;
    MarkUnstayed(inns, stays, minute + dwell, dwell);
    bool const stayed = !inns[0].unstayed && inns[1].unstayed;
    MarkUnstayed(inns, stays, minute, 0);
    bool const noDwell = !inns[0].unstayed && !inns[1].unstayed && !StayRipened(stays, minute, 0);
    bool const early = StayRipened(stays, minute + dwell - 1, dwell);
    bool const due = StayRipened(stays, minute + dwell, dwell);
    bool const again = StayRipened(stays, minute + 2 * dwell, dwell);
    DropLeftStays(stays, work);
    bool const left = stays.empty();
    NoteInnStays(stays, seenFrom(atFirst), atFirst, minute);
    DropLeftStays(stays, {0, atFirst.x, atFirst.y, atFirst.z});
    Add(checks,
        "inn stay: starts in range, counts from the first look, reaches the dwell once, ends out of range and on "
        "another map; dwell 0 marks no inn",
        entered && waits && stayed && noDwell && !early && due && !again && left && stays.empty(), "stays");

    stays.clear();
    auto const rankAt =
        [&](Place const& at, Place const& bind, std::uint32_t nowMs, std::uint32_t dwellMs, Situation situation)
    {
        std::vector<InnSeen> const seen = seenFrom(at);
        NoteInnStays(stays, seen, at, nowMs);
        Snapshot bot = Bot(45, FLUSH);
        bot.position = at;
        bot.hearth = bind;
        situation.inns = InnInputs(seen, bind, {});
        MarkUnstayed(situation.inns, stays, nowMs, dwellMs);
        return Rank(bot, situation);
    };
    auto const orders = [](Ranked const& ranked, std::uint32_t inn)
    { return Find(ranked, GoalKind::Hearth, inn) != nullptr; };

    Ranked const control = rankAt(work, atFirst, minute, 0, Base(45));
    Add(checks, "inn stay (control): without a dwell the work spot orders the other inn at once",
        orders(control, second), Summary(control));
    stays.clear();
    Ranked const atWork = rankAt(work, atFirst, minute, dwell, Base(45));
    Add(checks, "inn stay (NR2): bound at one inn, at work far from both: no inn is ordered",
        !Has(atWork, GoalKind::Hearth), Summary(atWork));
    rankAt(atSecond, atFirst, 2 * minute, dwell, Base(45));
    Ranked const shortStay = rankAt(atSecond, atFirst, 4 * minute, dwell, Base(45));
    Ranked const settled = rankAt(atSecond, atFirst, 2 * minute + dwell, dwell, Base(45));
    Add(checks, "inn stay: near a new inn the order waits for the dwell, then comes",
        !Has(shortStay, GoalKind::Hearth) && orders(settled, second), Summary(shortStay) + " | " + Summary(settled));
    // Bound at the second inn now: back at the first for two minutes, at work, back again
    Ranked const back = rankAt(atFirst, atSecond, 20 * minute, dwell, Base(45));
    Ranked const stillBack = rankAt(atFirst, atSecond, 22 * minute, dwell, Base(45));
    Ranked const workAgain = rankAt(work, atSecond, 30 * minute, dwell, Base(45));
    Ranked const returned = rankAt(atFirst, atSecond, 40 * minute, dwell, Base(45));
    Add(checks,
        "inn stay (NR2): a short stay back near the first inn, at work, back again: no second order of the first",
        !Has(back, GoalKind::Hearth) && !Has(stillBack, GoalKind::Hearth) && !Has(workAgain, GoalKind::Hearth) &&
            !Has(returned, GoalKind::Hearth),
        Summary(back) + " | " + Summary(workAgain) + " | " + Summary(returned));

    RoutePlan route;
    QuestOrder order;
    order.mapId = atSecond.map;
    order.x = atSecond.x;
    order.y = atSecond.y;
    route.orders = {order};
    Situation routed = Base(45);
    routed.onCircuit = false;
    routed.route = &route;
    stays.clear();
    Ranked const nextRoute = rankAt(work, atFirst, minute, dwell, routed);
    CircuitPlan moving;
    moving.moveOn.reason = MoveReason::Bracket;
    moving.moveOn.camp = 1;
    moving.moveOn.place = atSecond;
    Situation moves = Base(45);
    moves.circuit = &moving;
    stays.clear();
    Ranked const nextMove = rankAt(work, atFirst, minute, dwell, moves);
    Add(checks, "inn stay: an inn that holds the next planned goal (route order, circuit move) needs no stay",
        orders(nextRoute, second) && orders(nextMove, second), Summary(nextRoute) + " | " + Summary(nextMove));
}

// WR13b: the planner's gather circuit (GatherRules.h CircuitOption) from a herbalist's place
inline Gather::Node HerbAt(std::uint32_t id, float dx, float dy, std::uint32_t required, std::uint32_t pathCopper,
                           std::uint32_t respawnSecs = 300)
{
    Gather::Node node;
    node.id = id;
    node.skill = HERBALISM;
    node.required = required;
    node.at = {-9465.0f + dx, 40.0f + dy, 0.0f};
    node.respawnSecs = respawnSecs;
    node.yields = {{2447, 1.5, pathCopper, 10u, 0u}};
    return node;
}

inline Gather::Gatherer HerbalistOf(Snapshot const& bot, std::uint32_t value)
{
    Weights const weights = FromTraits(bot);
    Need const need = NeedFor(bot, weights, ReserveAt(bot.level));
    Gather::Gatherer gatherer;
    gatherer.skills = {{HERBALISM, value, 75}};
    gatherer.rates.exchange = need.exchange;
    gatherer.rates.factor = need.factor;
    gatherer.rates.minuteValue = BAND_XP_PER_MINUTE[WorldTravel::LevelBand(bot.level)];
    gatherer.rates.cautionWeight = CautionWeight(weights);
    return gatherer;
}

inline Option const* Offered(Ranked const& ranked, std::uint32_t target)
{
    for (Option const& option : ranked.options)
        if ((option.kind == GoalKind::Gather || option.kind == GoalKind::Money) && option.target == target)
            return &option;
    return nullptr;
}

inline void GatherCircuits(std::vector<Check>& checks)
{
    Snapshot bot = Bot(10, 0);
    bot.skills = {{HERBALISM, 1, 75}};
    Gather::Gatherer const herbalist = HerbalistOf(bot, 1);
    Planner::Circuit::Point const here{bot.position.x, bot.position.y, bot.position.z};
    auto const offer = [&](std::vector<Gather::Node> const& nodes, Gather::Gatherer const& gatherer)
    { return Gather::CircuitOption(Gather::BuildCircuit(here, nodes, gatherer), nodes, gatherer, EASTERN); };
    auto const ranked = [&](GatherNode const& option)
    {
        Situation situation = Base();
        if (option.id)
            situation.nodes = {option};
        return Rank(bot, situation);
    };

    // The path pays more than a vendor: the circuit's worth is item worth, an orange node brings skill points
    std::vector<Gather::Node> const wanted = {HerbAt(201, 60.0f, 0.0f, 1, 200), HerbAt(202, 120.0f, 40.0f, 1, 200),
                                              HerbAt(203, 60.0f, 90.0f, 1, 200)};
    GatherNode const paid = offer(wanted, herbalist);
    Ranked const paidRanked = ranked(paid);
    Option const* paidOption = Offered(paidRanked, paid.id);
    Add(checks, "gather circuit: three herbs offered as one circuit of item worth and skill points, ranked",
        paid.stops.size() == 3 && paid.itemWorth > 0.0f && paid.copperWorth == 0.0f && paid.skillPoints > 0.0f &&
            paid.id == paid.stops.front() && paidOption,
        Summary(paidRanked));

    // A vendor's price only: copper, worth less than the path's price
    std::vector<Gather::Node> sold = wanted;
    for (Gather::Node& node : sold)
        node.yields.front().pathCopper = 0;
    GatherNode const vendor = offer(sold, herbalist);
    Ranked const vendorRanked = ranked(vendor);  // kept alive: Offered points into its options
    Option const* vendorOption = Offered(vendorRanked, vendor.id);
    Add(checks, "gather circuit: at a vendor's price the worth is copper and the circuit is worth less",
        vendor.itemWorth == 0.0f && vendor.copperWorth > 0.0f && vendorOption && paidOption &&
            vendorOption->value < paidOption->value,
        vendorOption ? std::to_string(vendorOption->value) : std::string("not offered"));

    // The same vendor-only circuit: money while the bot is short of its reserve, a gather once it is not
    {
        Ranked const brokeRanked = ranked(vendor);
        Option const* const brokeOption = Offered(brokeRanked, vendor.id);
        Snapshot flushBot = Bot(10, FLUSH);
        flushBot.skills = {{HERBALISM, 1, 75}};
        Gather::Gatherer const flushHerbalist = HerbalistOf(flushBot, 1);
        GatherNode const flushVendor =
            Gather::CircuitOption(Gather::BuildCircuit(here, sold, flushHerbalist), sold, flushHerbalist, EASTERN);
        Situation situation = Base();
        situation.nodes = {flushVendor};
        Ranked const flushRanked = Rank(flushBot, situation);
        Option const* flushOption = Offered(flushRanked, flushVendor.id);
        Add(checks, "gather circuit: a flush bot's vendor-only circuit is a gather, not money",
            flushOption && flushOption->kind == GoalKind::Gather && flushOption->reason != "gold_short" &&
                flushOption->value > 0.0f,
            flushOption ? flushOption->reason + " " + std::to_string(flushOption->value) : std::string("not offered"));
        Add(checks, "gather circuit: a broke bot's vendor-only circuit is money (reason gold_short)",
            brokeOption && brokeOption->kind == GoalKind::Money && brokeOption->reason == "gold_short",
            brokeOption ? brokeOption->reason : std::string("not offered"));
    }

    // Nodes far above the bot's level: every stop refused, no option
    std::vector<Gather::Node> distant = wanted;
    for (Gather::Node& node : distant)
        node.risk.levelsAbove = 40.0f;
    GatherNode const refused = offer(distant, herbalist);
    Add(checks, "gather circuit: nodes far above the bot's level give no option",
        !refused.id && !Has(ranked(refused), GoalKind::Gather), std::to_string(refused.stops.size()));

    // A mild risk at one stop: offered, the stops' mean risk carried
    std::vector<Gather::Node> mild = wanted;
    mild[1].risk.levelsAbove = 3.0f;
    GatherNode const risky = offer(mild, herbalist);
    Add(checks, "gather circuit: a stop a few levels above keeps the circuit and carries the mean risk",
        risky.id && risky.levelsAbove > 0.0f && risky.levelsAbove < 3.0f, std::to_string(risky.levelsAbove));

    // The skill below every node: no option
    std::vector<Gather::Node> high = wanted;
    for (Gather::Node& node : high)
        node.required = 100;
    GatherNode const unskilled = offer(high, herbalist);
    Add(checks, "gather circuit: the skill below every node gives no option",
        !unskilled.id && !Has(ranked(unskilled), GoalKind::Gather), std::to_string(unskilled.stops.size()));

    // A circuit of kills without a skin (WR20b): it asks no skill, its kills' XP is ranked with its coins
    {
        GatherNode kills;
        kills.id = Gather::StopKey(true, 301);
        kills.skill = 0;
        kills.place = paid.place;
        kills.gatherMinutes = 20.0f;
        kills.copperWorth = 300.0f;
        kills.skillPoints = 0.0f;
        kills.xpWorth = 4000.0f;
        kills.stops = {kills.id};
        Ranked const killRanked = ranked(kills);
        Option const* killOption = Offered(killRanked, kills.id);
        Add(checks, "gather circuit: kills without a skin are offered with no skill, their XP counted",
            killOption && killOption->xp == 4000.0f && killOption->reason != "skill_up",
            killOption ? killOption->reason + " " + std::to_string(killOption->xp) : std::string("not offered"));
    }

    // Few nodes with a long respawn: walked once; quick respawns: laps over the session, their minutes summed
    std::vector<Gather::Node> quick = wanted;
    for (Gather::Node& node : quick)
        node.respawnSecs = 1;
    GatherNode const lapping = offer(quick, herbalist);
    Add(checks, "gather circuit: a lap shorter than the respawn is one pass, quick respawns lap",
        paid.laps == 1 && lapping.laps > 1 &&
            std::abs(lapping.gatherMinutes - lapping.laps * (paid.gatherMinutes / paid.laps)) < 0.01f,
        std::to_string(paid.laps) + " / " + std::to_string(lapping.laps));

    // Creature circuits (professions WR20c): a camp of three spawns of a level-9 to 10 kind beside the bot whose loot
    // sells at a vendor, with cloth the path pays for (wanted) or without; long respawns (one pass each); the gatherer
    // has no gathering skill
    auto const camp = [&](std::uint32_t first, bool cloth, std::uint32_t maxLevel)
    {
        std::vector<Gather::Node> nodes;
        for (std::uint32_t i = 0; i < 3; ++i)
        {
            Gather::Node node;
            node.kind = Gather::StopKind::Creature;
            node.id = first + i;
            node.at = {-9465.0f + 60.0f + 40.0f * static_cast<float>(i), 40.0f + 30.0f * static_cast<float>(i), 0.0f};
            node.respawnSecs = 3600;
            node.creature.minLevel = maxLevel - 1;
            node.creature.maxLevel = maxLevel;
            node.risk.levelsAbove = maxLevel > bot.level ? static_cast<float>(maxLevel - bot.level) : 0.0f;
            node.yields = {{4865, 1.0, 0u, 100u, 0u}};
            if (cloth)
                node.yields.push_back({2589, 1.0, 400u, 13u, 0u});
            nodes.push_back(node);
        }
        return nodes;
    };
    auto const fighterOf = [](Gather::Gatherer gatherer, std::uint32_t level, bool shortOfGold)
    {
        gatherer.skills.clear();
        gatherer.level = level;
        gatherer.shortOfGold = shortOfGold;
        return gatherer;
    };
    {
        Snapshot flushBot = Bot(10, FLUSH);
        Gather::Gatherer const flushFighter = fighterOf(HerbalistOf(flushBot, 1), 10, false);
        Gather::Gatherer const brokeFighter = fighterOf(herbalist, 10, true);
        auto const rankedFor = [&](Snapshot const& who, GatherNode const& option)
        {
            Situation situation = Base();
            if (option.id)
                situation.nodes = {option};
            return Rank(who, situation);
        };
        std::vector<Gather::Node> const cloth = camp(401, true, 10);
        std::vector<Gather::Node> const plain = camp(401, false, 10);
        GatherNode const flushCloth = offer(cloth, flushFighter);
        GatherNode const flushPlain = offer(plain, flushFighter);
        Ranked const flushClothRanked = rankedFor(flushBot, flushCloth);
        Option const* flushClothOption = Offered(flushClothRanked, flushCloth.id);
        Add(checks, "creature circuit: a flush bot goes for a wanted material, never for a camp without one",
            flushClothOption && flushCloth.itemWorth > 0.0f && Gather::IsCreatureStop(flushCloth.id) && !flushPlain.id,
            flushClothOption ? flushClothOption->reason + " " + std::to_string(flushClothOption->value)
                             : std::string("not offered"));
        GatherNode const brokeCloth = offer(cloth, brokeFighter);
        GatherNode const brokePlain = offer(plain, brokeFighter);
        Ranked const brokeClothRanked = ranked(brokeCloth);
        Ranked const brokePlainRanked = ranked(brokePlain);
        Option const* brokeClothOption = Offered(brokeClothRanked, brokeCloth.id);
        Option const* brokePlainOption = Offered(brokePlainRanked, brokePlain.id);
        Add(checks, "creature circuit: a circuit with a wanted material outranks one without",
            brokeClothOption && brokePlainOption && brokeClothOption->value > brokePlainOption->value,
            std::to_string(brokeCloth.stops.size()) + " stops " +
                (brokeClothOption ? std::to_string(brokeClothOption->value) : std::string("none")) + " vs " +
                std::to_string(brokePlain.stops.size()) + " stops " +
                (brokePlainOption ? std::to_string(brokePlainOption->value) : std::string("none")));
        Add(checks, "creature circuit: a poor bot's circuit without a wanted material is money (reason gold_short)",
            brokePlainOption && brokePlainOption->kind == GoalKind::Money && brokePlainOption->reason == "gold_short",
            brokePlainOption ? brokePlainOption->reason : std::string("not offered"));
        GatherNode const harder = offer(camp(401, true, 12), flushFighter);
        Ranked const harderRanked = rankedFor(flushBot, harder);
        Option const* harderOption = Offered(harderRanked, harder.id);
        Add(checks, "creature circuit: a dangerous camp loses to a safe one",
            flushClothOption && harderOption && harderOption->value < flushClothOption->value,
            (harderOption ? std::to_string(harderOption->value) : std::string("not offered")) + " vs " +
                (flushClothOption ? std::to_string(flushClothOption->value) : std::string("not offered")));
        GatherNode const above = offer(camp(401, true, 20), flushFighter);
        Add(checks, "creature circuit: a camp far above the bot gives no option", !above.id,
            std::to_string(above.stops.size()));
    }
}

inline void Gathering(std::vector<Check>& checks)
{
    for (bool const skilled : {true, false})
    {
        Snapshot bot = Bot(10, FLUSH);
        if (skilled)
            bot.skills = {{HERBALISM, 40, 75}};
        Situation situation = Base();
        GatherNode node;
        node.id = 81;
        node.skill = HERBALISM;
        node.required = 25;
        node.walkMinutes = 0.5f;
        node.copperWorth = 60.0f;
        situation.nodes = {node};
        Ranked const ranked = Rank(bot, situation);
        if (skilled)
            Add(checks, "gather: an herbalist takes a node on the way", TopIs(ranked, GoalKind::Gather, 81, "skill_up"),
                Summary(ranked));
        else
            Add(checks, "gather: no skill, no node", !Has(ranked, GoalKind::Gather), Summary(ranked));
    }
    GatherCircuits(checks);
}

// Fishing trips (professions WR16b): the fill's option (Fish::TripOption) ranked as a Fish goal, never money
inline void Fishing(std::vector<Check>& checks)
{
    Snapshot bot = Bot(30, FLUSH);
    Weights const weights = FromTraits(bot);
    Need const need = NeedFor(bot, weights, ReserveAt(bot.level));
    Fish::Fisher fisher;
    fisher.skill = 100;
    fisher.maxSkill = 150;
    fisher.poleHeld = true;
    fisher.rates.exchange = need.exchange;
    fisher.rates.factor = need.factor;
    fisher.rates.minuteValue = BAND_XP_PER_MINUTE[WorldTravel::LevelBand(bot.level)];
    fisher.rates.cautionWeight = CautionWeight(weights);
    Fish::Water water;
    water.area = 40;
    water.baseSkill = 55;
    water.pools = 12;
    water.openYields = {{6291, 1.0, 0u, 1u, 0u}};
    water.poolYields = {{6359, 1.0, 0u, 4u, 0u}};
    Fish::Spot const spot{0, -9800.0f, 1820.0f, 0.0f};
    auto const ranked = [&](GatherNode const& option)
    {
        Situation situation = Base();
        if (option.id)
            situation.waters = {option};
        return Rank(bot, situation);
    };
    auto const offered = [](Ranked const& from) -> Option const*
    {
        for (Option const& option : from.options)
            if (option.kind == GoalKind::Fish)
                return &option;
        return nullptr;
    };

    // A skilled fisher with a pole: the pool is the pick (the sure catch), the trip ranked as a Fish goal
    bool pool = false;
    std::size_t const pick = Fish::Pick({{water, {}}}, fisher, pool);
    GatherNode const trip = Fish::TripOption(water, fisher, {}, pool, spot, 1.0f);
    Ranked const tripRanked = ranked(trip);
    Option const* tripOption = offered(tripRanked);
    Add(checks, "fish: a fisher with a pole takes the pool of its zone's water, a Fish goal at the spot",
        pick == 0 && pool && trip.id == 40 && trip.skill == Fish::FISHING && trip.place.x == spot.x &&
            trip.skillPoints > 0.0f && trip.copperWorth > 0.0f && tripOption && tripOption->reason == "skill_up" &&
            std::abs(tripOption->minutes - (1.0f + Fish::TRIP_MINUTES)) < 0.01f,
        Summary(tripRanked));

    // No pole: no option (the pole is the tool need's purchase, WR5b)
    Fish::Fisher poleless = fisher;
    poleless.poleHeld = false;
    GatherNode const none = Fish::TripOption(water, poleless, {}, true, spot, 1.0f);
    Add(checks, "fish: no pole, no trip", !none.id && !Has(ranked(none), GoalKind::Fish), std::to_string(none.id));

    // Copper alone (the skill at its cap, fish worth a lot): still a Fish goal, never a money goal
    Fish::Fisher capped = fisher;
    capped.skill = 150;
    Fish::Water rich = water;
    rich.poolYields.front().vendorCopper = 4000;
    GatherNode const sold = Fish::TripOption(rich, capped, {}, true, spot, 1.0f);
    Ranked const soldRanked = ranked(sold);
    Option const* soldOption = offered(soldRanked);
    Add(checks, "fish: a trip for its copper stays a Fish goal (income), not money",
        sold.id && sold.skillPoints == 0.0f && soldOption && soldOption->reason == "income" &&
            !Has(soldRanked, GoalKind::Money),
        soldOption ? soldOption->reason : std::string("not offered"));

    // The fishing interest adds its minutes of play: the same trip is worth more to a bot that enjoys it
    Fish::Fisher fond = capped;
    fond.interest = 1.0;
    Fish::Water plain = water;
    GatherNode const pleasure = Fish::TripOption(plain, fond, {}, true, spot, 1.0f);
    GatherNode const chore = Fish::TripOption(plain, capped, {}, true, spot, 1.0f);
    Ranked const fondRanked = ranked(pleasure);
    Ranked const choreRanked = ranked(chore);
    Option const* fondOption = offered(fondRanked);
    Option const* choreOption = offered(choreRanked);
    Add(checks, "fish: the fishing interest makes the trip worth more (interest)",
        pleasure.pleasureMinutes > 0.0f && fondOption && fondOption->reason == "interest" &&
            (!choreOption || choreOption->value < fondOption->value),
        fondOption ? std::to_string(fondOption->value) : std::string("not offered"));

    // Open water below a fair chance and no pool: nothing pays, no option
    Fish::Water open = water;
    open.pools = 0;
    open.baseSkill = 400;
    Fish::Fisher novice = fisher;
    novice.skill = 30;
    bool openPool = true;
    Add(checks, "fish: open water below a fair catch chance without a pool gives no trip",
        Fish::Pick({{open, {}}}, novice, openPool) == Fish::NO_PICK &&
            !Fish::TripOption(open, novice, {}, false, spot, 1.0f).id,
        std::to_string(Fish::CatchChance(30, 400, false)));
}

inline void Robust(std::vector<Check>& checks)
{
    Snapshot bot = Bot(10, FLUSH);
    bot.traits = Traits({{TraitRules::Trait::Curiosity, 50}, {TraitRules::Trait::Sociability, -40}});
    Situation situation = Base();
    CircuitPlan const circuit = Circuit(bot, situation, 1500.0f, 200.0f, 20.0f);
    situation.circuit = &circuit;
    situation.spots = {Spot(11, 75.0f, 10.0f), Spot(12, 45.0f, 50.0f), Spot(13, 60.0f, 20.0f)};
    HubOption distant = Hub(22, ELWYNN, 90.0f, 5.0f);
    distant.explored = false;
    situation.hubs = {Hub(21, ELWYNN, 100.0f, 5.0f), distant, Hub(23, ELWYNN, 100.0f, 5.0f)};
    Ranked const first = Rank(bot, situation);
    Ranked const again = Rank(bot, situation);
    Add(checks, "robust: the same input gives the same ranked list", SameList(first, again), Summary(first));

    Situation reversed = situation;
    std::reverse(reversed.spots.begin(), reversed.spots.end());
    std::reverse(reversed.hubs.begin(), reversed.hubs.end());
    Ranked const turned = Rank(bot, reversed);
    Add(checks, "robust: the input order does not change the list", SameList(first, turned), Summary(turned));

    Snapshot plain = Bot(10, FLUSH);
    Weights const neutral = FromTraits(plain);
    CircuitWeights const circuitWeights = CircuitWeightsFor(Prepare(plain, situation));
    Add(checks, "robust: no traits, neutral weights",
        neutral.caution == 0.0f && neutral.curiosity == 0.0f && CautionWeight(neutral) == 1.0f &&
            circuitWeights.caution == 1.0f && circuitWeights.thoroughness == 1.0f &&
            Factor(neutral.sociability, SOCIAL_SPAN) == 1.0f,
        "caution " + std::to_string(circuitWeights.caution));

    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();
    Snapshot lost = bot;
    lost.position.x = nan;
    Ranked const nowhere = Rank(lost, situation);
    Situation broken = situation;
    broken.spots[0].xpPerKill = nan;
    broken.hubs[0].valuePerMinute = inf;
    broken.healthPercent = nan;
    broken.bags.vendor.walkMinutes = inf;
    Ranked const cleaned = Rank(bot, broken);
    bool const droppedBad = std::count_if(cleaned.dropped.begin(), cleaned.dropped.end(),
                                          [](Dropped const& dropped) { return dropped.reason == "bad_input"; }) == 2;
    Add(checks, "robust: nan and inf inputs are rejected, the rest still ranks",
        !nowhere.ok && nowhere.failure == "bad position" && cleaned.ok && droppedBad && !cleaned.options.empty() &&
            !Has(cleaned, GoalKind::Rest),
        Summary(cleaned));
}

inline void LogRows(std::vector<Check>& checks)
{
    Snapshot bot = Bot(10, 0);
    Situation situation = Base();
    CircuitPlan const circuit = Circuit(bot, situation, 1500.0f, 200.0f, 20.0f);
    situation.circuit = &circuit;
    situation.spots = {Spot(11, 75.0f, 10.0f), Spot(12, 45.0f, 50.0f)};
    Ranked const ranked = Rank(bot, situation);
    std::vector<GoalLog::Row> const rows = GoalLog::Rows(ranked);
    bool words = rows.size() == 1;
    for (GoalLog::Row const& row : rows)
    {
        words = words && row.choice == ChoiceWord(ranked.options.front()) && row.reason == LogWord(row.reason) &&
                row.alternatives.size() == std::min<std::size_t>(ranked.options.size() - 1, GoalLog::ALTERNATIVES);
        for (GoalLog::Alt const& alt : row.alternatives)
            words = words && alt.choice == LogWord(alt.choice);
    }
    Add(checks, "log: the top option is the row, the next ones its alternatives, all words", words, Summary(ranked));
    Ranked failed;
    failed.failure = "No index!";
    std::vector<GoalLog::Row> const failedRows = GoalLog::Rows(failed);
    Add(checks, "log: a failed ranking is one none row with the failure as its reason",
        failedRows.size() == 1 && failedRows[0].choice == "none:0" && failedRows[0].reason == "no_index_", "failure");
}

inline Delivery Carried(std::uint32_t quest, float xp, float minutes, bool otherMap)
{
    Delivery delivery;
    delivery.quest = quest;
    delivery.place = {otherMap ? 1u : EASTERN, -9000.0f, 300.0f, 0.0f};
    delivery.travelMinutes = minutes;
    delivery.xp = xp;
    delivery.otherMap = otherMap;
    return delivery;
}

inline std::size_t RankOf(Ranked const& ranked, GoalKind kind, std::uint32_t target)
{
    for (std::size_t i = 0; i < ranked.options.size(); ++i)
        if (ranked.options[i].kind == kind && ranked.options[i].target == target)
            return i;
    return ranked.options.size();
}

// A dry circuit's move to a camp without a hub, and carried quests weighed by their reward and journey
inline void DryMovesAndDeliveries(std::vector<Check>& checks)
{
    Snapshot const bot = Bot(10, FLUSH);
    Situation situation = Base();
    CircuitPlan dry;
    dry.ok = true;
    dry.start = bot.position;
    dry.returnTo = bot.position;
    dry.moveOn.reason = MoveReason::Bracket;
    dry.moveOn.camp = DRY_CAMP;
    dry.moveOn.zone = WESTFALL;
    dry.moveOn.seconds = 300.0f;
    dry.moveOn.place = {EASTERN, -10500.0f, 1000.0f, 40.0f};
    situation.circuit = &dry;
    Ranked const ranked = Rank(bot, situation);
    Option const* move = Top(ranked);
    Add(checks, "dry hub: a move to a camp without a hub names the camp and leads there",
        move && ChoiceWord(*move) == "move_zone:camp_55" && move->place.x == dry.moveOn.place.x &&
            move->place.y == dry.moveOn.place.y && move->reason == "circuit_dry",
        Summary(ranked));

    CircuitPlan delivery = dry;
    delivery.moveOn.reason = MoveReason::Delivery;
    delivery.moveOn.quest = DELIVERY_QUEST;
    situation.circuit = &delivery;
    Ranked const plain = Rank(bot, situation);
    situation.deliveries = {Carried(DELIVERY_QUEST, 2000.0f, 5.0f, false)};
    Ranked const paid = Rank(bot, situation);
    Option const* plainMove = Find(plain, GoalKind::MoveZone, DRY_CAMP);
    Option const* paidMove = Find(paid, GoalKind::MoveZone, DRY_CAMP);
    Add(checks, "delivery: a dry hub's delivery move earns the carried quest's reward",
        plainMove && paidMove && paidMove->valuePerMinute > plainMove->valuePerMinute && paidMove->reason == "delivery",
        Summary(paid));
    CircuitPlan across = delivery;
    across.moveOn.place.map = 1;  // the ender's camp across the sea (WN1c: Stormwind to Auberdine)
    situation.circuit = &across;
    Ranked const crossing = Rank(bot, situation);
    Option const* crossMove = Find(crossing, GoalKind::MoveZone, DRY_CAMP);
    Add(checks, "delivery: a dry hub's delivery move to another map says so",
        crossMove && crossMove->reason == "delivery_other_map", Summary(crossing));

    Situation questing = Base();
    CircuitPlan const circuit = Circuit(bot, questing, 1500.0f, 200.0f, 20.0f);
    questing.circuit = &circuit;
    questing.deliveries = {Carried(RICH_DELIVERY, 3000.0f, 4.0f, false), Carried(FAR_DELIVERY, 300.0f, 30.0f, true)};
    Ranked const both = Rank(bot, questing);
    Option const* rich = Find(both, GoalKind::Quest, RICH_DELIVERY);
    Add(checks, "delivery: a rich delivery close by goes before the circuit, a poor one on another map after it",
        TopIs(both, GoalKind::Quest, RICH_DELIVERY, "delivery") && rich && ChoiceWord(*rich) == "quest:quest_601" &&
            RankOf(both, GoalKind::Quest, HOME_HUB) < RankOf(both, GoalKind::Quest, FAR_DELIVERY),
        Summary(both));

    // Only what can be turned in now is a delivery: a done log quest and a take without objectives go to their
    // ender; a log quest with its objective left and a take with one, carried for later, do not
    QuestGather gather;
    gather.places.push_back({EASTERN, -9000.0f, 300.0f, 0.0f, CARRIED_ENDER, false, ELWYNN, GOLDSHIRE_AREA, NO_CAMP});
    for (std::uint32_t const id : {DONE_CARRIED, OPEN_CARRIED, BARE_TAKE, OPEN_TAKE})
    {
        QuestRecord quest;
        quest.id = id;
        quest.level = 10;
        quest.minLevel = 7;
        quest.xpBase = 500;
        if (id != BARE_TAKE)
            quest.objectives.push_back({ObjectiveKind::Kill, 90201, 5u, 0, {}});
        gather.relations.push_back({id, CARRIED_ENDER, false, true});
        gather.quests.push_back(std::move(quest));
    }
    QuestIndex const carriedIndex = QuestIndex::Build(std::move(gather));
    CircuitContext context;
    context.quests = &carriedIndex;
    Snapshot carrier = bot;
    carrier.quests = {{DONE_CARRIED, Circuit::SLOT_COMPLETE, {}}, {OPEN_CARRIED, 0, {}}};
    CircuitPlan held = circuit;
    held.carried = {DONE_CARRIED, OPEN_CARRIED, BARE_TAKE, OPEN_TAKE};
    std::vector<Delivery> const due = Deliveries(carrier, context, held, carrier.runSpeed);
    Situation carrying = Base();
    carrying.circuit = &held;
    carrying.deliveries = due;
    Ranked const offered = Rank(carrier, carrying);
    std::string dueText;
    for (Delivery const& delivery : due)
        dueText += std::to_string(delivery.quest) + " ";
    Add(checks, "delivery: only a done log quest and a take without objectives are delivered now",
        due.size() == 2 && due[0].quest == DONE_CARRIED && due[1].quest == BARE_TAKE &&
            !Find(offered, GoalKind::Quest, OPEN_CARRIED) && !Find(offered, GoalKind::Quest, OPEN_TAKE),
        "due " + dueText + "| " + Summary(offered));
}

// A forced goal (the sick rest, a bridge goal's stand-in) has no value of its own: it stays until it ends, and only a
// stop worth more breaks in
inline void OutsideGoal(std::vector<Check>& checks)
{
    Option outside = Made(GoalKind::TravelElsewhere, OUTSIDE_TARGET, 0.0f, 10.0f);
    outside.forced = true;
    outside.reason = "user_asked";
    Ranked fresh = Fresh({outside, Made(GoalKind::Grind, 41, 120.0f), Made(GoalKind::Quest, 40, 100.0f)});
    fresh.referenceRate = 120.0f;
    ReplanInput in;
    in.current = {GoalKind::TravelElsewhere, OUTSIDE_TARGET, 0.0f, false, true};
    in.fresh = &fresh;
    std::uint32_t switches = 0;
    std::string last;
    for (std::uint32_t round = 0; round < 10; ++round)
    {
        in.lastPlanMs = round * REPLAN_INTERVAL_MS;
        in.nowMs = in.lastPlanMs + REPLAN_INTERVAL_MS;
        Replan const replan = ReplanDecision(in);
        if (replan.verdict == ReplanVerdict::Switch && replan.chosen)
        {
            ++switches;
            in.current = {replan.chosen->kind, replan.chosen->target, replan.chosen->valuePerMinute, false, false};
        }
        last = replan.reason;
    }
    Add(checks, "outside goal: an LLM goal stays at every interval until it ends", !switches && last == "outside_goal",
        std::to_string(switches) + " switches in 10 intervals, last " + last);

    in.current = {GoalKind::TravelElsewhere, OUTSIDE_TARGET, 0.0f, false, true};
    in.delta.situation = true;
    Ranked hurt = Fresh({outside, Made(GoalKind::Rest, 0, 500.0f, 0.5f), Made(GoalKind::Grind, 41, 120.0f)});
    hurt.referenceRate = 120.0f;
    in.fresh = &hurt;
    Replan const rest = ReplanDecision(in);
    Option other = outside;
    other.target = OUTSIDE_TARGET + 1;
    Ranked const next = Fresh({other, Made(GoalKind::Grind, 41, 120.0f)});
    in.fresh = &next;
    Replan const replaced = ReplanDecision(in);
    Add(checks, "outside goal: a needed rest breaks in, a new outside goal replaces it",
        rest.verdict == ReplanVerdict::PickUp && rest.reason == "situation" && rest.chosen &&
            rest.chosen->kind == GoalKind::Rest && replaced.verdict == ReplanVerdict::Switch && replaced.chosen &&
            replaced.chosen->target == OUTSIDE_TARGET + 1,
        std::string(ReplanWord(rest.verdict)) + " " + rest.reason + ", then " + ReplanWord(replaced.verdict));
}

// A goal ranked above a short stop but failing the margin must not hide the stop
inline void HiddenStops(std::vector<Check>& checks)
{
    ReplanInput in;
    in.current = {GoalKind::Quest, 40, 100.0f, false, false};
    in.delta.bagsFullNearVendor = true;
    Ranked const sell = Fresh({Made(GoalKind::Grind, 41, 120.0f), Made(GoalKind::Errand, 77, 110.0f, 3.0f),
                               Made(GoalKind::Quest, 40, 100.0f)});
    in.fresh = &sell;
    Replan const sold = ReplanDecision(in);
    Add(checks, "replan: full bags near a vendor sell though a grind spot ranks above the errand",
        sold.verdict == ReplanVerdict::PickUp && sold.reason == "opportunity_bags_full" && sold.chosen &&
            sold.chosen->kind == GoalKind::Errand,
        std::string(ReplanWord(sold.verdict)) + " " + sold.reason);

    in.delta = Delta{};
    in.delta.situation = true;
    Ranked const tired = Fresh(
        {Made(GoalKind::Grind, 41, 120.0f), Made(GoalKind::Rest, 0, 110.0f, 0.5f), Made(GoalKind::Quest, 40, 100.0f)});
    in.fresh = &tired;
    Replan const rested = ReplanDecision(in);
    Ranked const clear = Fresh(
        {Made(GoalKind::Grind, 41, 200.0f), Made(GoalKind::Rest, 0, 110.0f, 0.5f), Made(GoalKind::Quest, 40, 100.0f)});
    in.fresh = &clear;
    Replan const switched = ReplanDecision(in);
    Add(checks, "replan: a rest below a goal that fails the margin is taken, a clearly better goal still wins",
        rested.verdict == ReplanVerdict::PickUp && rested.reason == "situation" &&
            switched.verdict == ReplanVerdict::Switch && switched.chosen && switched.chosen->kind == GoalKind::Grind,
        std::string(ReplanWord(rested.verdict)) + " " + rested.reason + ", then " + ReplanWord(switched.verdict));
}

// A money goal says what its id is; a repair errand keeps to the vendor walk limit
inline void TargetsAndRepairs(std::vector<Check>& checks)
{
    Snapshot const broke = Bot(10, 0);
    Situation situation = Base();
    situation.spots = {Spot(11, 20.0f, 50.0f)};
    Ranked const ranked = Rank(broke, situation);
    Option const* top = Top(ranked);
    Add(checks, "money: a money goal names its grind spot", top && ChoiceWord(*top) == "money:spot_11",
        Summary(ranked));

    Snapshot worn = Bot(10, FLUSH);
    worn.durabilityPercent = 5;
    worn.lowestDurabilityPercent = 0;
    worn.brokenItems = 1;
    worn.repairCheapest = 40;
    Situation repair = Base();
    repair.bags.vendor = {78, WorldTravel::ERRAND_MAX_WALK_MINUTES + 5.0f, true};
    Ranked const farRanked = Rank(worn, repair);
    repair.bags.vendor.walkMinutes = 2.0f;
    Ranked const nearRanked = Rank(worn, repair);
    Add(checks, "errand: a repair keeps to the vendor walk limit",
        !Has(farRanked, GoalKind::Errand) && Has(nearRanked, GoalKind::Errand),
        Summary(farRanked) + " / " + Summary(nearRanked));
}

// A due repair during a quest (WT63c): a near repairer's errand breaks in on the quest; the same repair a long walk
// away (beyond the vendor walk limit) is no option
inline void RepairDuringQuest(std::vector<Check>& checks)
{
    Snapshot worn = Bot(10, FLUSH);
    worn.durabilityPercent = 11;
    worn.lowestDurabilityPercent = 11;
    worn.repairCheapest = 40;
    Situation situation = Base();
    CircuitPlan const circuit = Circuit(worn, situation, 1500.0f, 200.0f, 20.0f);
    situation.circuit = &circuit;
    auto const replanAt = [&](float walkMinutes, std::string& text)
    {
        situation.bags.repairer = {78, walkMinutes, true};
        Ranked const ranked = Rank(worn, situation);
        ReplanInput in;
        in.current = {GoalKind::Quest, HOME_HUB, 0.0f, false, false};
        in.fresh = &ranked;
        in.delta.situation = true;
        Replan const replan = ReplanDecision(in);
        text += (text.empty() ? "" : " / ") + Summary(ranked) + ": " + ReplanWord(replan.verdict) + " " + replan.reason;
        return replan;
    };
    std::string text;
    Replan const close = replanAt(1.0f, text);
    Replan const distant = replanAt(WorldTravel::ERRAND_MAX_WALK_MINUTES + 1.0f, text);
    Add(checks, "errand: a due repair near the quest replaces it, the same repair beyond the walk limit does not",
        close.verdict == ReplanVerdict::Switch && close.chosen && close.chosen->kind == GoalKind::Errand &&
            close.chosen->reason == "repair" && distant.verdict == ReplanVerdict::Stay,
        text);
}

// NE2: one repair rule (upkeep's RepairErrandDue) and a due repair ranks first, above a quest at a rich hub
inline void RepairFirst(std::vector<Check>& checks)
{
    auto const worn = [](std::uint8_t broken, std::uint8_t lowest, std::uint8_t average, std::uint32_t money)
    {
        Snapshot bot = Bot(10, money);
        bot.brokenItems = broken;
        bot.lowestDurabilityPercent = lowest;
        bot.durabilityPercent = average;
        bot.repairCost = 100;
        bot.repairCheapest = 40;
        return bot;
    };
    Situation situation = Base();
    situation.bags.repairer = {78, 1.0f, true};
    Snapshot const broken = worn(1, 0, 100, FLUSH);
    CircuitPlan const rich = Circuit(broken, situation, 20000.0f, 200.0f, 20.0f);
    situation.circuit = &rich;
    Ranked const first = Rank(broken, situation);
    Option const* top = Top(first);
    Add(checks, "repair: one broken item, the rest whole, a repairer and a rich quest hub: the repair ranks first",
        top && top->kind == GoalKind::Errand && top->reason == "repair" && top->due && Has(first, GoalKind::Quest),
        Summary(first));

    auto const due = [&](Snapshot const& bot)
    {
        Ranked const ranked = Rank(bot, situation);
        Option const* best = Top(ranked);
        return best && best->kind == GoalKind::Errand && best->reason == "repair";
    };
    Add(checks, "repair: lowest 25 % with the average at 80 % is due", due(worn(0, 25, 80, FLUSH)), "");
    Add(checks, "repair: lowest 40 %, none broken, is not due", !due(worn(0, 40, 90, FLUSH)), "");
    Add(checks, "repair: gold below the cheapest worn item is not due", !due(worn(1, 0, 90, 10)), "");

    // NR3 (NR.4): a sick bot takes a goal only once recover has handed it over (Snapshot::sickErrandOpen); then the
    // sick gate passes an errand whose stops keep to the safe area it stands in, and the rest is its fallback. Until
    // then, and with nothing the gate passes, it rests (forced) as before
    Snapshot sick = worn(1, 0, 90, FLUSH);
    sick.sickMs = 60000;
    Situation inside = situation;
    inside.sickSafeHere = true;
    inside.bags.repairer.safe = true;
    Ranked const rests = Rank(sick, inside);
    Option const* restTop = Top(rests);
    Add(checks, "repair: a sick bot whose recover is not done (not clear or not restored) rests, no repair option",
        restTop && restTop->kind == GoalKind::Rest && restTop->forced && !Has(rests, GoalKind::Errand) &&
            !rests.sickGoal,
        Summary(rests));
    Snapshot handed = sick;
    handed.sickErrandOpen = true;
    Ranked const goes = Rank(handed, inside);
    Option const* goTop = Top(goes);
    Add(checks, "repair: a sick bot recover handed over gets the due repair in its safe area, the rest is the fallback",
        goTop && goTop->kind == GoalKind::Errand && goTop->reason == "repair" && goTop->due && !goTop->forced &&
            goes.sickGoal && Has(goes, GoalKind::Rest),
        Summary(goes));
    bool const onlyErrandAndRest =
        std::all_of(goes.options.begin(), goes.options.end(), [](Option const& option)
                    { return option.kind == GoalKind::Errand || option.kind == GoalKind::Rest; });
    bool const questDropped = std::any_of(goes.dropped.begin(), goes.dropped.end(), [](Dropped const& dropped)
                                          { return dropped.kind == GoalKind::Quest && dropped.reason == SICK_REASON; });
    Add(checks, "repair: the sick bot is offered only the errand and the rest: the quest hub is dropped (sick)",
        onlyErrandAndRest && questDropped && Has(goes, GoalKind::Rest) && Has(first, GoalKind::Quest), Summary(goes));
    Ranked const outside = Rank(handed, situation);
    Add(checks, "repair: the same repairer outside the bot's safe area is no errand: the bot rests (forced)",
        Top(outside) && Top(outside)->kind == GoalKind::Rest && Top(outside)->forced && !Has(outside, GoalKind::Errand),
        Summary(outside));
    Snapshot whole = worn(0, 90, 95, FLUSH);
    whole.sickMs = 60000;
    whole.sickErrandOpen = true;
    Ranked const idle = Rank(whole, inside);
    Option const* idleTop = Top(idle);
    Add(checks, "repair: a sick bot with nothing due rests (forced) and gets no errand",
        idleTop && idleTop->kind == GoalKind::Rest && idleTop->forced && !Has(idle, GoalKind::Errand), Summary(idle));

    ReplanInput running;
    running.current = {GoalKind::Quest, HOME_HUB, 0.0f, false, false};
    Ranked const fresh = Rank(broken, situation);
    running.fresh = &fresh;
    running.delta.situation = true;
    Replan const switched = ReplanDecision(running);
    running.current = {GoalKind::Errand, 78, 0.0f, false, false};
    Replan const staying = ReplanDecision(running);
    Add(checks, "repair: a quest in progress is replaced by the due repair, the running repair stays",
        switched.verdict == ReplanVerdict::Switch && switched.reason == "repair" &&
            staying.verdict == ReplanVerdict::Stay,
        std::string(ReplanWord(switched.verdict)) + " " + switched.reason + " / " + ReplanWord(staying.verdict) + " " +
            staying.reason);
}

// NR3 (NR.4, stage 1): where a sick bot is safe (Upkeep::SafeAreas, as the map thread fills them), the sick gate of
// the goal model on those answers and the sick rest at a clear point
inline void SickSafeAreas(std::vector<Check>& checks)
{
    constexpr std::uint32_t STORMWIND = 1519;
    constexpr std::uint32_t LOW_ZONE = 40;    // an own side's zone whose range ends at 20
    constexpr std::uint32_t LEVEL_ZONE = 44;  // a zone of the bot's level
    constexpr std::uint32_t SANCTUARY = 3703;
    auto const sickBot = [](std::uint8_t level)
    {
        Snapshot bot = Bot(level, FLUSH);
        bot.sickMs = 300000;
        bot.sickErrandOpen = true;
        return bot;
    };
    auto const city = [](std::uint32_t zone)
    {
        Upkeep::SafeAreas areas;
        areas.city = zone;
        return areas;
    };
    auto const low = [](std::uint32_t zone, std::uint32_t high, bool own, std::uint32_t level)
    {
        Upkeep::SafeAreas areas;
        areas.lowZone = Upkeep::LowZoneSafe(high, own, GreyLevel(level)) ? zone : 0;
        return areas;
    };
    Upkeep::SafeAreas const nowhere;
    // The bot's situation as the map thread fills it: its own areas and those of the vendor's spot
    auto const placed = [](Situation situation, Upkeep::SafeAreas const& here, Upkeep::SafeAreas const& vendor)
    {
        situation.sickSafeHere = Upkeep::IsSafe(here);
        bool const safe = Upkeep::SafeLeg(Upkeep::SafeLegKind::Foot, here, vendor);
        situation.bags.vendor.safe = safe;
        situation.bags.repairer.safe = safe;
        return situation;
    };
    auto const onlySafeKinds = [](Ranked const& ranked)
    {
        return std::all_of(ranked.options.begin(), ranked.options.end(),
                           [](Option const& option)
                           {
                               return option.kind == GoalKind::Errand || option.kind == GoalKind::Rest ||
                                      option.kind == GoalKind::Hearth;
                           });
    };
    auto const droppedSick = [](Ranked const& ranked, GoalKind kind)
    {
        return std::any_of(ranked.dropped.begin(), ranked.dropped.end(), [kind](Dropped const& dropped)
                           { return dropped.kind == kind && dropped.reason == SICK_REASON; });
    };

    // The predicate's parts
    Add(checks,
        "safe: a hub of the other side is not safe, nor one without a flight master or an innkeeper, nor one "
        "with a member hostile to the bot; its own or a neutral hub with an inn is",
        !Upkeep::HubSafe(true, true, false) && !Upkeep::HubSafe(false, false, false) &&
            !Upkeep::HubSafe(false, true, true) && Upkeep::HubSafe(false, true, false) && Upkeep::HostileMember(0) &&
            Upkeep::HostileMember(1) && !Upkeep::HostileMember(2),
        "");
    Add(checks,
        "safe: a zone ending at 20 is low for a level 60 bot of its side, not for the other side, not at level 20",
        low(LOW_ZONE, 20, true, 60).lowZone == LOW_ZONE && !low(LOW_ZONE, 20, false, 60).lowZone &&
            !low(LOW_ZONE, 20, true, 20).lowZone && !low(LOW_ZONE, 0, true, 60).lowZone,
        "");
    Add(checks, "safe: an own capital and a sanctuary (zone or area) are cities, the other side's capital is not",
        Upkeep::SafeCity(STORMWIND, 0, true, false, false) == STORMWIND &&
            Upkeep::SafeCity(SANCTUARY, 0, false, true, false) == SANCTUARY &&
            Upkeep::SafeCity(LEVEL_ZONE, 7, false, false, true) == 7 && !Upkeep::SafeCity(1637, 0, false, false, false),
        "");
    Upkeep::SafeAreas hubA;
    hubA.hubs = {3};
    Upkeep::SafeAreas hubB;
    hubB.hubs = {4};
    Add(checks, "safe: a walk stays in one safe area; the stone needs only its end safe, a flight both ends",
        Upkeep::SafeLeg(Upkeep::SafeLegKind::Foot, hubA, hubA) &&
            !Upkeep::SafeLeg(Upkeep::SafeLegKind::Foot, hubA, hubB) &&
            !Upkeep::SafeLeg(Upkeep::SafeLegKind::Foot, city(STORMWIND), nowhere) &&
            Upkeep::SafeLeg(Upkeep::SafeLegKind::Hearthstone, nowhere, hubA) &&
            !Upkeep::SafeLeg(Upkeep::SafeLegKind::Flight, nowhere, hubA) &&
            Upkeep::SafeLeg(Upkeep::SafeLegKind::Flight, hubB, hubA),
        "");

    // A capital, a vendor stop due (full bags), no repair due: the errand (NE11 gave it rest)
    Situation base = Base();
    base.bags.sellableSlots = 10;
    base.bags.vendor = {77, 1.0f, false};
    base.spots = {Spot(11, 75.0f, 10.0f)};
    Snapshot full = sickBot(10);
    full.bagFree = 0;
    CircuitPlan const circuit = Circuit(full, base, 1500.0f, 200.0f, 20.0f);
    base.circuit = &circuit;
    Situation const capital = placed(base, city(STORMWIND), city(STORMWIND));
    Ranked const sells = Rank(full, capital);
    Option const* sellTop = Top(sells);
    Add(checks, "sick: in a capital with a vendor stop due and no repair due the bot gets the errand, rest below it",
        sellTop && sellTop->kind == GoalKind::Errand && sellTop->reason.rfind("bags_full", 0) == 0 && !sellTop->due &&
            !sellTop->forced && sells.sickGoal && Has(sells, GoalKind::Rest),
        Summary(sells));
    Add(checks, "sick: quest and grind options stay dropped while sick",
        onlySafeKinds(sells) && droppedSick(sells, GoalKind::Quest) &&
            (droppedSick(sells, GoalKind::Grind) || droppedSick(sells, GoalKind::Money)),
        Summary(sells));

    // A zone of its level outside every hub, a repair due 300 yd away: rest at the clear point recover walks it to
    Snapshot outside = sickBot(20);
    outside.brokenItems = 1;
    outside.lowestDurabilityPercent = 0;
    outside.repairCheapest = 40;
    outside.position = {EASTERN, -9000.0f, -2000.0f, 50.0f};
    Place const clear{EASTERN, -9010.0f, -2004.0f, 51.0f};
    outside.sickRest = clear;
    Situation away = Base(20);
    away.bags.repairer = {78, WorldTravel::WalkMinutes(300.0f, 7.0f), true};
    away = placed(away, low(LEVEL_ZONE, 25, true, 20), city(STORMWIND));
    Ranked const rests = Rank(outside, away);
    Option const* restTop = Top(rests);
    bool const atClear =
        restTop && restTop->place.map == clear.map && restTop->place.x == clear.x && restTop->place.y == clear.y;
    Add(checks,
        "sick: outside every hub in a zone of its level the bot rests at a clear point, also with a repair due "
        "300 yd away",
        restTop && restTop->kind == GoalKind::Rest && restTop->forced && atClear && !Has(rests, GoalKind::Errand) &&
            !rests.sickGoal,
        Summary(rests));

    // The same bot with its stone ready and bound in a safe hub: the stone, then its errand at the inn
    Snapshot stone = outside;
    stone.hasHearthstone = true;
    stone.hearthCooldownMs = 0;
    stone.hearth = {EASTERN, -9460.0f, 42.0f, 0.0f};
    Situation bound = away;
    bound.sickHearthSafe = Upkeep::SafeLeg(Upkeep::SafeLegKind::Hearthstone, low(LEVEL_ZONE, 25, true, 20), hubA);
    Ranked const hearths = Rank(stone, bound);
    Option const* hearthTop = Top(hearths);
    Add(checks, "sick: the same bot with its hearthstone ready and bound in a safe hub takes the stone to its errand",
        hearthTop && hearthTop->kind == GoalKind::Errand && hearthTop->reason == SICK_HEARTH_REASON &&
            hearthTop->place.x == stone.hearth.x && hearthTop->place.y == stone.hearth.y && hearths.sickGoal,
        Summary(hearths));
    Snapshot cooling = stone;
    cooling.hearthCooldownMs = 600000;
    Situation unbound = bound;
    unbound.sickHearthSafe = false;
    Ranked const cooled = Rank(cooling, bound);
    Ranked const elsewhere = Rank(stone, unbound);
    Add(checks, "sick: a stone on cooldown or bound outside every safe area gives no way in: the bot rests",
        Top(cooled) && Top(cooled)->kind == GoalKind::Rest && Top(cooled)->forced && Top(elsewhere) &&
            Top(elsewhere)->kind == GoalKind::Rest && Top(elsewhere)->forced,
        Summary(cooled) + " / " + Summary(elsewhere));

    // Awareness AW38c, S1 to S5: a road, a safe hub that a quiet way on foot joins to the bot's place (a grave in the
    // open country). The map thread asks a hub when RoadAsked passes it and adds it when the way the search answers is
    // quiet (Awareness::QuietWalk); here that answer is the fact `quietWay`
    std::uint32_t const roadHub = hubA.hubs.front();
    auto const grave = [roadHub](bool quietWay, bool hubSafe, bool holdsBot, bool inOwnArea)
    {
        Upkeep::SafeAreas areas;
        if (Upkeep::RoadAsked(hubSafe, holdsBot, inOwnArea, true) && quietWay)
            areas.roads = {roadHub};
        return areas;
    };
    Upkeep::SafeAreas const road = grave(true, true, false, false);
    Situation const toHub = placed(away, road, hubA);
    Ranked const roadErrand = Rank(outside, toHub);
    Add(checks,
        "S1 road: a grave with a road to hub H is safe, its word is road, a stop in H is a safe foot leg and the "
        "sick gate passes the errand there",
        Upkeep::IsSafe(road) && std::string(Upkeep::SafeAreaWord(road)) == "road" &&
            Upkeep::SafeLeg(Upkeep::SafeLegKind::Foot, road, hubA) &&
            Upkeep::SafeLeg(Upkeep::SafeLegKind::Foot, hubA, road) && Top(roadErrand) &&
            Top(roadErrand)->kind == GoalKind::Errand && roadErrand.sickGoal && toHub.bags.repairer.safe,
        Summary(roadErrand));
    Situation const toOther = placed(away, road, hubB);
    Ranked const otherHub = Rank(outside, toOther);
    Add(checks, "S2 road: a stop in another hub is no safe foot leg; the bot rests",
        !Upkeep::SafeLeg(Upkeep::SafeLegKind::Foot, road, hubB) && !toOther.bags.repairer.safe &&
            !Has(otherHub, GoalKind::Errand) && Top(otherHub) && Top(otherHub)->kind == GoalKind::Rest,
        Summary(otherHub));
    Add(checks, "S3 road: two places that each have a road to H share nothing",
        !Upkeep::ShareArea(road, road) && !Upkeep::SafeLeg(Upkeep::SafeLegKind::Foot, road, road), "");
    Upkeep::SafeAreas const passable = grave(false, true, false, false);
    Situation noRoad = placed(away, passable, hubA);
    noRoad.sickHearthSafe = true;
    Ranked const stoneIn = Rank(stone, noRoad);
    Situation roadStone = toHub;
    roadStone.sickHearthSafe = true;
    Ranked const roadNoStone = Rank(stone, roadStone);
    Add(checks,
        "S4 road: without a road (the way passable) the grave is outside and the stone in is offered as today; with "
        "the road the errand in H comes first",
        !Upkeep::IsSafe(passable) && std::string(Upkeep::SafeAreaWord(passable)) == "outside" && Top(stoneIn) &&
            Top(stoneIn)->kind == GoalKind::Errand && Top(stoneIn)->reason == SICK_HEARTH_REASON && Top(roadNoStone) &&
            Top(roadNoStone)->kind == GoalKind::Errand && Top(roadNoStone)->reason != SICK_HEARTH_REASON,
        Summary(stoneIn) + " / " + Summary(roadNoStone));
    Add(checks,
        "S5 road: RoadAsked is false for a hub that is not safe (a town of the other side), that holds the bot, that "
        "lies in the bot's own area or beyond the walk's reach",
        Upkeep::RoadAsked(Upkeep::HubSafe(false, true, false), false, false, true) &&
            !Upkeep::RoadAsked(Upkeep::HubSafe(true, true, false), false, false, true) &&
            !Upkeep::RoadAsked(true, true, false, true) && !Upkeep::RoadAsked(true, false, true, true) &&
            !Upkeep::RoadAsked(true, false, false, false) && !Upkeep::IsSafe(grave(true, false, false, false)) &&
            !Upkeep::IsSafe(grave(true, true, true, false)) && !Upkeep::IsSafe(grave(true, true, false, true)),
        "");

    // A level 60 bot in its side's zone that ends at 20: the zone is safe, its errand runs there
    Snapshot old = sickBot(60);
    old.bagFree = 0;
    Situation lowZone = placed(base, low(LOW_ZONE, 20, true, 60), low(LOW_ZONE, 20, true, 60));
    lowZone.circuit = nullptr;
    Ranked const oldSells = Rank(old, lowZone);
    Add(checks, "sick: a level 60 bot in a zone of its side that ends at level 20 gets its errand",
        Top(oldSells) && Top(oldSells)->kind == GoalKind::Errand && oldSells.sickGoal, Summary(oldSells));

    // A healthy bot's options do not read the sick facts
    Snapshot healthy = full;
    healthy.sickMs = 0;
    healthy.sickErrandOpen = false;
    Ranked const plain = Rank(healthy, base);
    Ranked const flagged = Rank(healthy, capital);
    bool same = plain.options.size() == flagged.options.size() && plain.dropped.size() == flagged.dropped.size();
    for (std::size_t i = 0; same && i < plain.options.size(); ++i)
        same = SameTarget(plain.options[i], flagged.options[i]) &&
               plain.options[i].valuePerMinute == flagged.options[i].valuePerMinute;
    Add(checks, "sick: a healthy bot's options are unchanged by the safe-area facts", same && !plain.sickGoal,
        Summary(plain) + " / " + Summary(flagged));
}

// WN4: no option names the no-hub or no-camp marker (WN3's side fact: circuit rows quest:4294967295). Searched over
// the bot's level and money, its hub (or none), the circuit (none, valued with a home camp or none, dry towards a
// camp, a hub, neither, nowhere), a grind spot, a hub and a carried quest: every option of every ranking and every
// row word. A valued circuit outside every hub and camp is offered as quest:field_0
inline void Targets(std::vector<Check>& checks)
{
    std::size_t states = 0;
    std::size_t options = 0;
    std::size_t field = 0;
    std::string bad;
    constexpr std::uint32_t CIRCUIT_KINDS = 7;
    for (std::uint32_t const level : {10u, 30u})
        for (std::uint32_t const money : {0u, FLUSH})
            for (std::uint32_t const hub : {HOME_HUB, NO_HUB})
                for (std::uint32_t circuitKind = 0; circuitKind < CIRCUIT_KINDS; ++circuitKind)
                    for (std::uint32_t extras = 0; extras < 8; ++extras)
                    {
                        Snapshot const bot = Bot(static_cast<std::uint8_t>(level), money);
                        Situation situation = Base(level);
                        situation.here.hub = hub;
                        CircuitPlan circuit = Circuit(bot, situation, 1500.0f, 200.0f, 20.0f);
                        if (circuitKind == 2)
                            circuit.homeCamp = DRY_CAMP;
                        if (circuitKind >= 3)
                        {
                            circuit.value = 0.0f;
                            circuit.moveOn.reason = circuitKind == 6 ? MoveReason::Nowhere : MoveReason::Bracket;
                            circuit.moveOn.zone = WESTFALL;
                            circuit.moveOn.seconds = 300.0f;
                            circuit.moveOn.place = {EASTERN, -10500.0f, 1000.0f, 40.0f};
                            if (circuitKind == 3)
                                circuit.moveOn.camp = DRY_CAMP;
                            if (circuitKind == 4)
                                circuit.moveOn.hub = 21;
                        }
                        if (circuitKind)
                            situation.circuit = &circuit;
                        if (extras & 1)
                            situation.spots = {Spot(11, 75.0f, 10.0f)};
                        if (extras & 2)
                            situation.hubs = {Hub(21, ELWYNN, 100.0f, 5.0f)};
                        if (extras & 4)
                            situation.deliveries = {Carried(DELIVERY_QUEST, 2000.0f, 5.0f, false)};
                        Ranked const ranked = Rank(bot, situation);
                        ++states;
                        std::string words;
                        for (Option const& option : ranked.options)
                        {
                            ++options;
                            words += ChoiceWord(option) + " ";
                            if (option.target == NO_HUB || option.target == NO_CAMP)
                                bad += " state " + std::to_string(states) + " " + ChoiceWord(option);
                            if (option.reason == CIRCUIT_REASON && option.targetType == TargetType::Field)
                                field += option.target == 0 && ChoiceWord(option) == "quest:field_0";
                        }
                        for (GoalLog::Row const& row : GoalLog::Rows(ranked))
                        {
                            words += row.choice + " ";
                            for (GoalLog::Alt const& alt : row.alternatives)
                                words += alt.choice + " ";
                        }
                        if (words.find("4294967295") != std::string::npos)
                            bad += " state " + std::to_string(states) + " words " + words;
                        bool const fieldCircuit = hub == NO_HUB && circuitKind == 1;
                        if (fieldCircuit && !Find(ranked, GoalKind::Quest, 0))
                            bad += " state " + std::to_string(states) + " no field circuit: " + Summary(ranked);
                    }
    Add(checks,
        "targets: no option or row names the no-hub or no-camp marker; a circuit outside every hub and camp is "
        "offered as quest:field_0",
        states == 2 * 2 * 2 * CIRCUIT_KINDS * 8 && bad.empty() && field > 0,
        std::to_string(states) + " states, " + std::to_string(options) + " options, " + std::to_string(field) +
            " field circuits" + bad);
}

// Questing WQ18a: the route beside the circuit, one owner per decision (WQ.43 point 4)
inline void Routes(std::vector<Check>& checks)
{
    auto const has = [](Ranked const& ranked, auto const& test)
    { return std::any_of(ranked.options.begin(), ranked.options.end(), test); };
    auto const routeOf = [](Ranked const& ranked) -> Option const*
    {
        auto const it = std::find_if(ranked.options.begin(), ranked.options.end(), IsRoute);
        return it == ranked.options.end() ? nullptr : &*it;
    };
    Snapshot const bot = Bot(10, FLUSH);
    Situation questing = Base();
    CircuitPlan const circuit = Circuit(bot, questing, 1500.0f, 200.0f, 20.0f);
    questing.circuit = &circuit;
    questing.deliveries = {Carried(RICH_DELIVERY, 3000.0f, 4.0f, false), Carried(FAR_DELIVERY, 300.0f, 30.0f, true)};
    questing.hubs = {Hub(2, ELWYNN, 40.0f, 6.0f)};
    Ranked const plain = Goals::Rank(bot, questing);
    RoutePlan const route = SampleRoute(bot);
    questing.route = &route;
    questing.onCircuit = false;
    Ranked const routed = Goals::Rank(bot, questing);
    Option const* option = routeOf(routed);
    Add(checks, "route: one option of its own, named route_0, with the route's worth and minutes and no crowd factor",
        option && ChoiceWord(*option) == "quest:route_0" && option->reason == ROUTE_REASON &&
            option->expected == route.worth && option->minutes == route.minutes && option->crowd == 1.0f &&
            option->risk == 1.0f && option->place.x == route.orders.front().x,
        Summary(routed));
    Add(checks,
        "route: with an order neither the circuit's option nor a delivery on the bot's map is offered (mutant: "
        "AddCircuit or AddDeliveries without the test)",
        option && has(plain, OwnMapQuesting) && !has(routed, OwnMapQuesting), Summary(routed));
    auto const farDelivery = [](Option const& one)
    { return one.target == FAR_DELIVERY && one.reason == "delivery_other_map"; };
    auto const otherHub = [](Option const& one)
    { return one.target == 2 && one.targetType == TargetType::Default && one.kind != GoalKind::Grind; };
    Add(checks, "route: a delivery on another map and another hub stay offered beside it",
        has(plain, farDelivery) && has(plain, otherHub) && has(routed, farDelivery) && has(routed, otherHub),
        Summary(routed));

    Situation held = questing;
    held.onCircuit = true;
    RoutePlan const none;
    Situation empty = questing;
    empty.route = &none;
    Add(checks,
        "route: a bot held on the circuit and a route without an order get the options of before (mutant: the flag "
        "unread)",
        SameRanked(plain, Goals::Rank(bot, held)) && SameRanked(plain, Goals::Rank(bot, empty)) && !routeOf(plain) &&
            has(plain, OwnMapQuesting),
        Summary(Goals::Rank(bot, held)));

    Situation drySituation = Base();
    CircuitPlan dry;
    dry.ok = true;
    dry.start = bot.position;
    dry.returnTo = bot.position;
    dry.moveOn.reason = MoveReason::Bracket;
    dry.moveOn.camp = DRY_CAMP;
    dry.moveOn.zone = WESTFALL;
    dry.moveOn.seconds = 300.0f;
    dry.moveOn.place = {EASTERN, -10500.0f, 1000.0f, 40.0f};
    drySituation.circuit = &dry;
    drySituation.route = &route;
    drySituation.onCircuit = false;
    Ranked const dryRanked = Goals::Rank(bot, drySituation);
    Add(checks, "route: a dry hub's move stays offered beside the route",
        routeOf(dryRanked) && Find(dryRanked, GoalKind::MoveZone, DRY_CAMP), Summary(dryRanked));

    RoutePlan wary = route;
    wary.risk.levelsAbove = 2.0f;
    questing.route = &wary;
    Ranked const cautious = Goals::Rank(bot, questing);
    Option const* waryOption = routeOf(cautious);
    RoutePlan deadly = route;
    deadly.risk.levelsAbove = 40.0f;
    deadly.risk.eliteShare = 1.0f;
    deadly.risk.deaths = 50.0f;
    questing.route = &deadly;
    Ranked const refused = Goals::Rank(bot, questing);
    Add(checks, "route: the risk of its first stop lowers its value, as any option's",
        option && waryOption && waryOption->risk < 1.0f && waryOption->value < option->value, Summary(cautious));
    Add(checks,
        "route: a route the risk rule drops leaves the circuit's option and the deliveries as they were (mutant: the "
        "route owns the map before it is kept)",
        !routeOf(refused) && SameOptions(plain, refused), Summary(refused));
    questing.route = &route;

    // PlanGoals: the build function for a bot not held on the circuit; a route handed in is taken as it is
    std::uint32_t builds = 0;
    float pricedCopper = 0.0f;
    RouteBuild const build =
        [&](Snapshot const&, CircuitContext const& context, Model const&, Situation const& situation)
    {
        ++builds;
        pricedCopper = context.weights.copper;
        RoutePlan made = SampleRoute(bot, 777.0f);
        made.capped = static_cast<std::uint32_t>(situation.closedCells.size());
        return made;
    };
    Situation job = Base();
    job.onCircuit = false;
    job.closedCells = {7, 9};
    GoalPlan const made = Goals::PlanGoals(bot, CircuitContext{}, job, build);
    GoalPlan const again = Goals::PlanGoals(bot, CircuitContext{}, job, build);
    std::uint32_t const builtTwice = builds;
    Option const* madeOption = routeOf(made.ranked);
    job.route = &route;
    GoalPlan const handed = Goals::PlanGoals(bot, CircuitContext{}, job, build);
    std::uint32_t const builtHanded = builds;
    job.onCircuit = true;
    GoalPlan const heldPlan = Goals::PlanGoals(bot, CircuitContext{}, job, build);
    Add(checks,
        "route: PlanGoals builds the route of a bot that is not held on the circuit, once, at the circuit's prices and "
        "with the situation's closed cells, and the plan carries it",
        builtTwice == 2 && pricedCopper == CircuitWeightsFor(Prepare(bot, job)).copper && made.route.capped == 2 &&
            made.route.worth == 777.0f && made.route.orders.size() == 1 && madeOption && madeOption->expected == 777.0f,
        Summary(made.ranked));
    Add(checks, "route: a route handed in is taken as it is; a bot held on the circuit gets none and no build",
        builtHanded == 2 && handed.route.worth == route.worth && routeOf(handed.ranked) && builds == 2 &&
            heldPlan.route.orders.empty() && !routeOf(heldPlan.ranked),
        Summary(heldPlan.ranked));
    Add(checks, "route: the same input gives the same plan",
        SameRanked(made.ranked, again.ranked) && made.route.orders.size() == again.route.orders.size() &&
            made.route.worth == again.route.worth && SameRanked(routed, Goals::Rank(bot, questing)),
        Summary(again.ranked));

    RouteTally const& tally = routeTally;
    Add(checks,
        "route: every case of this harness run again with the route's inputs: flag set or no order, the same result; "
        "with an order never the circuit's option or a same-map delivery beside it, and no other option new",
        tally.cases >= 40 && !tally.differs && tally.offered > 0 && !tally.beside && tally.replaced > 0 && !tally.added,
        std::to_string(tally.cases) + " cases, " + std::to_string(tally.offered) + " with the route kept, " +
            std::to_string(tally.replaced) + " replacing the circuit or a delivery; differs " +
            std::to_string(tally.differs) + ", beside " + std::to_string(tally.beside) + ", added " +
            std::to_string(tally.added) + (tally.first.empty() ? "" : "; first: " + tally.first));
}

// A test bot held from quests (TestConstraints noquest, Situation::questsHeld) is offered no quest goal: the circuit
// and the hub in its zone are dropped, the rest stays
inline void HeldFromQuests(std::vector<Check>& checks)
{
    Snapshot const bot = Bot(10, FLUSH);
    Situation situation = Base();
    CircuitPlan const circuit = Circuit(bot, situation, 400.0f, 0.0f, 20.0f);
    situation.circuit = &circuit;
    situation.hubs = {Hub(21, ELWYNN, 100.0f, 5.0f)};
    Ranked const free = Rank(bot, situation);
    situation.questsHeld = true;
    Ranked const held = Rank(bot, situation);
    bool const named = std::any_of(held.dropped.begin(), held.dropped.end(),
                                   [](auto const& dropped) { return dropped.kind == GoalKind::Quest; });
    Add(checks, "held from quests: a free bot is offered quest goals, a held one none (each named dropped)",
        Has(free, GoalKind::Quest) && !Has(held, GoalKind::Quest) && named, Summary(held));

    // Questing WQL5b: the route's option (WQ18a) is a quest goal too, dropped by its kind under the same word
    RoutePlan const route = SampleRoute(bot);
    situation.route = &route;
    situation.onCircuit = false;
    situation.questsHeld = false;
    Ranked const routedFree = Rank(bot, situation);
    situation.questsHeld = true;
    Ranked const routedHeld = Rank(bot, situation);
    auto const heldWords = [](Ranked const& ranked)
    {
        return std::count_if(ranked.dropped.begin(), ranked.dropped.end(), [](Dropped const& dropped)
                             { return dropped.kind == GoalKind::Quest && dropped.reason == "noquest"; });
    };
    bool const offered = std::any_of(routedFree.options.begin(), routedFree.options.end(), [](Option const& one)
                                     { return IsRoute(one) && ChoiceWord(one) == "quest:route_0"; });
    Add(checks,
        "held from quests: a held bot with a route is offered no quest goal of either kind, the route dropped as "
        "noquest (mutant: the gate keyed on the circuit's reason)",
        offered && !Has(routedHeld, GoalKind::Quest) && heldWords(routedHeld) == heldWords(held) + 1,
        Summary(routedHeld));
}

// Questing only (questing WQ37, WQ.50 item 5): the keep list as the test's setting states it, written here and not
// read from the rule. Quests (the circuit, the route, deliveries, hubs, class quests), a move to another zone, rest,
// the hearthstone, a bag, an errand for full bags or a repair; money only while a due repair or training is unpayable
inline bool QuestingKept(Option const& option, bool moneyAllowed)
{
    switch (option.kind)
    {
        case GoalKind::Quest:
        case GoalKind::MoveZone:
        case GoalKind::Rest:
        case GoalKind::Hearth:
        case GoalKind::BuyBag:
            return true;
        case GoalKind::Errand:
            return option.reason == "bags_full" || option.reason == "bags_full_vendor_near" ||
                   option.reason == "repair";
        case GoalKind::Money:
            return moneyAllowed;
        default:
            return false;
    }
}

// With the setting no option outside the keep list is ranked, and each one the same bot is ranked without it is named
// dropped as questing_only; over generated bots (money, wear, training, bags, health, questing inputs) that every
// forbidden kind is offered without the setting, and every kept one ranked with it, so the check is not vacuous. The
// model builds no TravelElsewhere option (only a bridge order sets one), so none is generated here
inline void QuestingOnly(std::vector<Check>& checks)
{
    constexpr std::size_t KINDS = static_cast<std::size_t>(GoalKind::Count);
    std::array<bool, KINDS> offered{};  // forbidden kinds ranked without the setting
    std::array<bool, KINDS> kept{};     // kinds ranked with it
    bool craftOffered = false;
    bool repairKept = false;
    bool keptOnly = true;
    bool named = true;
    std::string fault;
    std::uint32_t cases = 0;
    std::vector<GoalLog::Row> skippedRows;

    Snapshot const fisherBot = Bot(10, FLUSH);
    Weights const weights = FromTraits(fisherBot);
    Need const need = NeedFor(fisherBot, weights, ReserveAt(fisherBot.level));
    Fish::Fisher fisher;
    fisher.skill = 100;
    fisher.maxSkill = 150;
    fisher.poleHeld = true;
    fisher.rates.exchange = need.exchange;
    fisher.rates.factor = need.factor;
    fisher.rates.minuteValue = BAND_XP_PER_MINUTE[WorldTravel::LevelBand(fisherBot.level)];
    fisher.rates.cautionWeight = CautionWeight(weights);
    Fish::Water water;
    water.area = 40;
    water.baseSkill = 55;
    water.pools = 12;
    water.openYields = {{6291, 1.0, 0u, 1u, 0u}};
    water.poolYields = {{6359, 1.0, 0u, 4u, 0u}};
    GatherNode const fishing = Fish::TripOption(water, fisher, {}, true, {0, -9800.0f, 1820.0f, 0.0f}, 1.0f);
    GatherNode herb;
    herb.id = 81;
    herb.skill = HERBALISM;
    herb.required = 25;
    herb.walkMinutes = 0.5f;
    herb.copperWorth = 60.0f;

    for (std::uint32_t const money : {0u, 300u, FLUSH})
        for (int const worn : {0, 1, 2})  // none, a repair the gold pays, one it does not
            for (bool const training : {false, true})
                for (std::uint16_t const free : {std::uint16_t{8}, std::uint16_t{2}, std::uint16_t{0}})
                    for (bool const hurt : {false, true})
                        for (int const questing : {0, 1, 2})  // the circuit, the route, nothing to quest
                        {
                            Snapshot bot = Bot(10, money);
                            bot.skills = {{HERBALISM, 40, 75}};
                            bot.bagFree = free;
                            bot.hearth = {1, 9950.0f, 2280.0f, 0.0f};  // Darnassus: a far bind
                            if (worn)
                            {
                                bot.durabilityPercent = 5;
                                bot.lowestDurabilityPercent = 0;
                                bot.brokenItems = 1;
                                bot.repairCheapest = worn == 1 ? 40 : FLUSH + 1;
                            }
                            Situation situation = Base();
                            CircuitPlan const circuit = Circuit(bot, situation, 1500.0f, 200.0f, 20.0f);
                            RoutePlan const route = SampleRoute(bot);
                            if (questing == 0)
                                situation.circuit = &circuit;
                            if (questing == 1)
                            {
                                situation.route = &route;
                                situation.onCircuit = false;
                            }
                            if (questing < 2)
                            {
                                situation.deliveries = {Carried(DELIVERY_QUEST, 300.0f, 5.0f, false)};
                                situation.hubs = {Hub(21, ELWYNN, 100.0f, 5.0f), Hub(22, WESTFALL, 400.0f, 10.0f)};
                                // The craft queue and the far bind outrank a grind: only beside the quest inputs
                                situation.craft = {1000000.0f, 3.0f, 2.0f};
                                situation.inns = {{701, {EASTERN, -9460.0f, 20.0f, 0.0f}, 60.0f, true, false}};
                                situation.bindSeconds = FAR_BIND_SECONDS;
                            }
                            situation.spots = {Spot(11, 150.0f, 10.0f), Spot(12, 45.0f, 50.0f)};
                            situation.nodes = {herb};
                            situation.waters = {fishing};
                            situation.bags.sellableSlots = 10;
                            situation.bags.vendor = {77, 1.0f, true};
                            situation.bagOffers = {{4496, 77, 1.0f, 400, 6, 0}};
                            situation.healthPercent = hurt ? 30.0f : 100.0f;
                            situation.trainingUnpayable = training;
                            Ranked const off = Rank(bot, situation);
                            situation.questingOnly = true;
                            Ranked const on = Rank(bot, situation);
                            ++cases;
                            bool const moneyAllowed = training || (worn != 0 && bot.repairCheapest > bot.money);
                            for (Option const& option : off.options)
                            {
                                if (QuestingKept(option, moneyAllowed))
                                    continue;
                                offered[static_cast<std::size_t>(option.kind)] = true;
                                craftOffered = craftOffered || option.reason == "craft";
                                bool const dropped = std::any_of(on.dropped.begin(), on.dropped.end(),
                                                                 [&option](Dropped const& entry)
                                                                 {
                                                                     return entry.kind == option.kind &&
                                                                            entry.target == option.target &&
                                                                            entry.reason == QUESTING_ONLY_REASON;
                                                                 });
                                if (!dropped && named)
                                {
                                    named = false;
                                    fault += " not named: " + ChoiceWord(option) + " " + option.reason + ";";
                                }
                            }
                            for (Option const& option : on.options)
                            {
                                kept[static_cast<std::size_t>(option.kind)] = true;
                                repairKept =
                                    repairKept || (option.kind == GoalKind::Errand && option.reason == "repair");
                                if (!QuestingKept(option, moneyAllowed) && keptOnly)
                                {
                                    keptOnly = false;
                                    fault += " ranked: " + Summary(on) + ";";
                                }
                            }
                            if (skippedRows.empty() && !on.dropped.empty())
                            {
                                std::vector<GoalLog::Row> const rows = GoalLog::Rows(on);
                                if (rows.size() > 1)
                                    skippedRows = rows;
                            }
                        }

    std::string missing;
    for (GoalKind const kind : {GoalKind::Grind, GoalKind::Gather, GoalKind::Fish, GoalKind::Money})
        if (!offered[static_cast<std::size_t>(kind)])
            missing += std::string(" offered ") + KindWord(kind);
    if (!craftOffered)
        missing += " offered craft";
    for (GoalKind const kind : {GoalKind::Quest, GoalKind::MoveZone, GoalKind::Rest, GoalKind::Hearth, GoalKind::BuyBag,
                                GoalKind::Errand, GoalKind::Money})
        if (!kept[static_cast<std::size_t>(kind)])
            missing += std::string(" kept ") + KindWord(kind);
    if (!repairKept)
        missing += " kept repair";
    Add(checks, "questing only: no forbidden option is ranked, each one is named dropped as questing_only",
        keptOnly && named, std::to_string(cases) + " bots;" + fault);
    Add(checks, "questing only: every forbidden kind is offered without the setting, every kept one ranked with it",
        missing.empty(), missing.empty() ? std::to_string(cases) + " bots" : missing);

    // The goal rows show the drop: one skipped:questing_only row after the ranking's, also after a none row when
    // the setting dropped every option; without drops there is none (LogRows: one row per ranking)
    bool const skipped = skippedRows.size() == 2 && skippedRows[1].choice == "skipped:questing_only" &&
                         skippedRows[1].reason == QUESTING_ONLY_REASON && skippedRows[1].value >= 1.0f &&
                         !skippedRows[1].alternatives.empty() &&
                         skippedRows[1].alternatives.size() <= GoalLog::ALTERNATIVES &&
                         skippedRows[1].alternatives[0].choice == LogWord(skippedRows[1].alternatives[0].choice);
    Snapshot const grinder = Bot(10, FLUSH);
    Situation only = Base();
    only.spots = {Spot(11, 75.0f, 10.0f)};
    only.questingOnly = true;
    Ranked const none = Rank(grinder, only);
    std::vector<GoalLog::Row> const noneRows = GoalLog::Rows(none);
    bool const noneSkipped = none.options.empty() && noneRows.size() == 2 && noneRows[0].choice == "none:0" &&
                             noneRows[1].choice == "skipped:questing_only" && noneRows[1].value == 1.0f &&
                             noneRows[1].alternatives.size() == 1 && noneRows[1].alternatives[0].choice == "grind:11";
    Add(checks, "questing only: the goal rows name the dropped options in a skipped:questing_only row",
        skipped && noneSkipped,
        std::to_string(skippedRows.size()) + " rows; only forbidden: " + Summary(none) + ", " +
            std::to_string(noneRows.size()) + " rows");
}

// NR14 (NR.14): riding, class training or a hunter's pet that upkeep's need rules make due and the gold pays is an
// errand goal of its own, through NeedBridge as the planner's job fills it. The rule is upkeep's (R1): the goal model
// offers what the needs list says and ranks it as the due repair
inline void OwnStops(std::vector<Check>& checks)
{
    constexpr std::uint32_t CLASS_TRAINER = 911;
    constexpr std::uint32_t RIDING_TRAINER = 4732;
    constexpr std::uint32_t MOUNT_VENDOR = 384;
    constexpr std::uint32_t BEAST = 2956;
    constexpr std::uint32_t TAME_BEAST = 1515;
    constexpr std::uint32_t RIDING_PRICE = 40000;
    constexpr std::uint32_t SPELL_PRICE = 100;
    constexpr float WALK = 6.0f;
    constexpr float FAR_WALK = 45.0f;
    using UpkeepNeeds::Need;
    using UpkeepNeeds::NeedOrder;
    auto const riding = [](std::uint64_t funded, std::uint32_t trainer)
    {
        Need need;
        need.order = NeedOrder::Riding;
        need.kind = UpkeepKind::Riding;
        need.copper = RIDING_PRICE;
        need.funded = funded;
        need.due = true;
        need.trainer = trainer;
        need.vendor = MOUNT_VENDOR;
        return need;
    };
    auto const training = [](std::uint64_t funded, bool due)
    {
        Need need;
        need.order = NeedOrder::Training;
        need.kind = UpkeepKind::TrainerSpell;
        need.copper = SPELL_PRICE * 3;
        need.cheapest = SPELL_PRICE;
        need.funded = funded;
        need.due = due;
        need.trainer = CLASS_TRAINER;
        return need;
    };
    Place const spawn{EASTERN, -9300.0f, 150.0f, 50.0f};
    auto const pet = [&spawn]()
    {
        Need need;
        need.order = NeedOrder::Pet;
        need.kind = UpkeepKind::Pet;
        need.item = TAME_BEAST;
        need.due = true;
        need.beast = BEAST;
        need.beastAt = spawn;
        return need;
    };
    // The situation the job fills: the needs through the bridge, the stops' walks as the map thread gives them
    auto const filled = [](Situation situation, std::vector<Need> const& list, bool safe = false)
    {
        UpkeepNeeds::NeedList needs;
        needs.needs = list;
        NeedBridge::Fill(situation, NeedBridge::InputsOf(needs));
        for (OwnStop* stop : {&situation.training, &situation.riding, &situation.pet})
        {
            stop->walkMinutes = WALK;
            stop->safe = safe;
        }
        return situation;
    };
    auto const errands = [](Ranked const& ranked)
    {
        return std::count_if(ranked.options.begin(), ranked.options.end(),
                             [](Option const& option) { return option.kind == GoalKind::Errand; });
    };
    auto const dropped = [](Ranked const& ranked, char const* reason)
    {
        return std::any_of(ranked.dropped.begin(), ranked.dropped.end(), [reason](Dropped const& drop)
                           { return drop.kind == GoalKind::Errand && drop.target == BEAST && drop.reason == reason; });
    };

    // Riding at 20 and at 40, the gold paying the rank and its mount, beside a good circuit at the bot's hub: the
    // trip is offered, due, and ranks over the quests (the reading NR14 asks for: once it is affordable, always)
    bool ridingWins = true;
    std::string ridingDetail;
    for (std::uint8_t const level : {20, 40})
    {
        Snapshot const bot = Bot(level, RIDING_PRICE * 2);
        Situation base = Base(level);
        base.here.zoneHigh = level + 2;
        CircuitPlan const circuit = Circuit(bot, base, 400.0f * level, 2000.0f, 20.0f);
        base.circuit = &circuit;
        Ranked const ranked = Rank(bot, filled(base, {riding(RIDING_PRICE, RIDING_TRAINER)}));
        Option const* top = Top(ranked);
        ridingWins = ridingWins && TopIs(ranked, GoalKind::Errand, RIDING_TRAINER, RIDING_REASON) && top->due &&
                     top->peaceful && Has(ranked, GoalKind::Quest);
        ridingDetail += std::to_string(level) + ": " + Summary(ranked) + "; ";
    }
    Add(checks,
        "own stop (NR14): riding due and affordable at 20 and 40 is offered the trip to its trainer, due, over the "
        "quests",
        ridingWins, ridingDetail);

    Snapshot const bot = Bot(20, RIDING_PRICE * 2);
    Situation base = Base(20);
    base.here.zoneHigh = 22;
    CircuitPlan const circuit = Circuit(bot, base, 8000.0f, 2000.0f, 20.0f);
    base.circuit = &circuit;
    Ranked const shortRide = Rank(bot, filled(base, {riding(RIDING_PRICE - 1, RIDING_TRAINER)}));
    Ranked const mountOnly = Rank(bot, filled(base, {riding(RIDING_PRICE, 0)}));
    Add(checks,
        "own stop: riding whose funded share does not pay it whole (RidingErrandDue) is offered no trip; a known "
        "rank's trip goes to its mount vendor",
        !Has(shortRide, GoalKind::Errand) && TopIs(mountOnly, GoalKind::Errand, MOUNT_VENDOR, RIDING_REASON),
        Summary(shortRide) + " / " + Summary(mountOnly));

    Ranked const train = Rank(bot, filled(base, {training(SPELL_PRICE, true)}));
    Situation const unpaid = filled(base, {training(SPELL_PRICE - 1, true)});
    Ranked const trainShort = Rank(bot, unpaid);
    Ranked const notDue = Rank(bot, filled(base, {training(SPELL_PRICE * 3, false)}));
    Add(checks,
        "own stop: class training due and paid (TrainingErrandDue) is offered the trip to the trainer; short of the "
        "cheapest spell or not due by upkeep's levels, none",
        TopIs(train, GoalKind::Errand, CLASS_TRAINER, TRAINING_REASON) && Top(train)->due &&
            !Has(trainShort, GoalKind::Errand) && unpaid.trainingUnpayable && !Has(notDue, GoalKind::Errand),
        Summary(train) + " / " + Summary(trainShort) + " / " + Summary(notDue));

    // The trip's worth is upkeep's verdict, not a threshold of the goal model: a far trainer is offered as a near one,
    // a stop the map thread knows no walk to is not
    Situation distant = filled(base, {riding(RIDING_PRICE, RIDING_TRAINER)});
    distant.riding.walkMinutes = FAR_WALK;
    Situation unknown = distant;
    unknown.riding.walkMinutes = -1.0f;
    Ranked const farRanked = Rank(bot, distant);
    Ranked const unknownRanked = Rank(bot, unknown);
    Add(checks,
        "own stop: the worth is upkeep's (R1): a trainer 45 minutes away is still the due trip, one without a known "
        "walk is none",
        TopIs(farRanked, GoalKind::Errand, RIDING_TRAINER, RIDING_REASON) && Top(farRanked)->due &&
            !Has(unknownRanked, GoalKind::Errand),
        Summary(farRanked) + " / " + Summary(unknownRanked));

    // Training and riding both due: one errand option, named by training (the needs' fixed order), both in its worth
    Ranked const both = Rank(bot, filled(base, {training(SPELL_PRICE, true), riding(RIDING_PRICE, RIDING_TRAINER)}));
    Ranked const one = Rank(bot, filled(base, {riding(RIDING_PRICE, RIDING_TRAINER)}));
    bool const twoWorth = Top(both) && Top(one) && std::abs(Top(both)->expected - 2.0f * Top(one)->expected) < 0.01f;
    Add(checks, "own stop: training and riding due make one errand named by training, both in its worth",
        TopIs(both, GoalKind::Errand, CLASS_TRAINER, TRAINING_REASON) && errands(both) == 1 && twoWorth, Summary(both));

    // Full bags with riding due: still one errand, the sale names it; the trip takes the trainer if it passes, the next
    // plan names riding. One due option, so a due current is it (ReplanDecision)
    Snapshot full = bot;
    full.bagFree = 0;
    Situation bags = filled(base, {riding(RIDING_PRICE, RIDING_TRAINER)});
    bags.bags.sellableSlots = 10;
    bags.bags.vendor = {77, 1.0f, true};
    Ranked const sale = Rank(full, bags);
    Add(checks, "own stop: full bags with riding due make one due errand, named by the sale",
        errands(sale) == 1 && Top(sale) && Top(sale)->kind == GoalKind::Errand && Top(sale)->target == 77 &&
            Top(sale)->due,
        Summary(sale));

    // A hunter without a pet: the tame trip to the beast's spawn, which fights (not peaceful). A sick bot handed over
    // in its safe area is not offered it, nor one under a test's nofight; both drops name the beast
    Snapshot const hunter = Bot(10, 5000);
    Situation const tame = filled(Base(10), {pet()}, true);
    Ranked const tamed = Rank(hunter, tame);
    Option const* tameTop = Top(tamed);
    Snapshot sickHunter = hunter;
    sickHunter.sickMs = 60000;
    sickHunter.sickErrandOpen = true;
    Situation sickTame = tame;
    sickTame.sickSafeHere = true;
    Ranked const sickRanked = Rank(sickHunter, sickTame);
    Situation held = tame;
    held.fightsHeld = true;
    Ranked const heldRanked = Rank(hunter, held);
    Add(checks,
        "own stop: a hunter without a pet is offered the tame trip (not peaceful), not while sick and not under "
        "nofight",
        TopIs(tamed, GoalKind::Errand, BEAST, PET_REASON) && tameTop->due && !tameTop->peaceful &&
            tameTop->place.x == spawn.x && !Has(sickRanked, GoalKind::Errand) && dropped(sickRanked, SICK_REASON) &&
            !Has(heldRanked, GoalKind::Errand) && dropped(heldRanked, NOFIGHT_REASON),
        Summary(tamed) + " / " + Summary(sickRanked) + " / " + Summary(heldRanked));

    // Riding and training pass the sick gate like any errand: peaceful, and offered when the stop is in the safe area
    Snapshot sickBot = bot;
    sickBot.sickMs = 60000;
    sickBot.sickErrandOpen = true;
    Situation safe = filled(base, {riding(RIDING_PRICE, RIDING_TRAINER)}, true);
    safe.sickSafeHere = true;
    Situation unsafe = filled(base, {riding(RIDING_PRICE, RIDING_TRAINER)}, false);
    unsafe.sickSafeHere = true;
    Ranked const sickSafe = Rank(sickBot, safe);
    Ranked const sickUnsafe = Rank(sickBot, unsafe);
    Add(checks, "own stop: a sick bot handed over gets the due riding trip to a trainer in its safe area, none outside",
        TopIs(sickSafe, GoalKind::Errand, RIDING_TRAINER, RIDING_REASON) && !Has(sickUnsafe, GoalKind::Errand),
        Summary(sickSafe) + " / " + Summary(sickUnsafe));

    Situation only = filled(base, {riding(RIDING_PRICE, RIDING_TRAINER)});
    only.questingOnly = true;
    Ranked const kept = Rank(bot, only);
    Add(checks, "own stop: the questing test keeps the trip of its own (it serves levelling)",
        TopIs(kept, GoalKind::Errand, RIDING_TRAINER, RIDING_REASON), Summary(kept));

    ReplanInput running;
    running.current = {GoalKind::Quest, HOME_HUB, 0.0f, false, false};
    Ranked const fresh = Rank(bot, filled(base, {riding(RIDING_PRICE, RIDING_TRAINER)}));
    running.fresh = &fresh;
    running.delta.situation = true;
    Replan const switched = ReplanDecision(running);
    running.current = {GoalKind::Errand, RIDING_TRAINER, 0.0f, false, false};
    Replan const staying = ReplanDecision(running);
    Add(checks, "own stop: a quest in progress is replaced by the due riding trip, the running trip stays",
        switched.verdict == ReplanVerdict::Switch && switched.reason == RIDING_REASON &&
            staying.verdict == ReplanVerdict::Stay,
        std::string(ReplanWord(switched.verdict)) + " " + switched.reason + " / " + ReplanWord(staying.verdict));

    // The walks the job gives the due stops (NeedBridge::PlaceOwnStops) from the map thread's stops seen: a trainer of
    // the bot's class with the need's key, never one of another class; a sick bot's safe spawn before a nearer unsafe
    // one; the beast's spawn by its distance, none past the errand walk
    constexpr std::uint32_t OWN_SPAWN = 5113;
    constexpr std::uint32_t FAR_SPAWN = 5114;
    constexpr std::uint32_t OTHER_SPAWN = 6000;
    constexpr std::uint32_t NO_LIST = 99;
    UpkeepIndex index;
    Planner::UpkeepTrainer own;
    own.key = CLASS_TRAINER;
    own.kind = Planner::TrainerKind::Class;
    own.requirement = WARRIOR;
    own.list = NO_LIST;
    own.entries = {OWN_SPAWN, FAR_SPAWN};
    Planner::UpkeepTrainer other = own;
    other.key = CLASS_TRAINER + 1;
    other.requirement = WARRIOR + 1;
    other.entries = {OTHER_SPAWN};
    index.trainers = {own, other};
    UpkeepNeeds::NeedList placed;
    placed.needs = {training(SPELL_PRICE, true), pet()};
    auto const placedBy = [&](std::vector<StopSeen> const& seen, Place const& beastAt)
    {
        UpkeepNeeds::NeedList needs = placed;
        needs.needs.back().beastAt = beastAt;
        Situation situation = base;
        NeedBridge::Fill(situation, NeedBridge::InputsOf(needs));
        situation.trainersSeen = seen;
        NeedBridge::PlaceOwnStops(situation, needs, index, bot.race, bot.cls, bot.position, bot.runSpeed);
        return situation;
    };
    Situation const plain = placedBy({{OTHER_SPAWN, 1.0f, false}, {OWN_SPAWN, 3.0f, false}}, spawn);
    Situation const sickPlaced =
        placedBy({{OTHER_SPAWN, 1.0f, true}, {OWN_SPAWN, 3.0f, false}, {FAR_SPAWN, 5.0f, true}}, spawn);
    Place farBeast = spawn;
    farBeast.x += WorldTravel::ERRAND_MAX_WALK_MINUTES * 60.0f * bot.runSpeed + 100.0f;
    Situation const noneSeen = placedBy({{OTHER_SPAWN, 1.0f, false}}, farBeast);
    float const beastMinutes = WorldTravel::WalkMinutes(
        std::hypot(spawn.x - bot.position.x, spawn.y - bot.position.y, spawn.z - bot.position.z), bot.runSpeed);
    Add(checks,
        "own stop: the job's walk is the nearest trainer of the bot's class that serves the need (a sick bot's safe "
        "one first) and the beast's distance; none seen or past the errand walk leaves no trip",
        plain.training.walkMinutes == 3.0f && sickPlaced.training.walkMinutes == 5.0f && sickPlaced.training.safe &&
            std::abs(plain.pet.walkMinutes - beastMinutes) < 0.01f && noneSeen.training.walkMinutes < 0.0f &&
            noneSeen.pet.walkMinutes < 0.0f,
        std::to_string(plain.training.walkMinutes) + " " + std::to_string(sickPlaced.training.walkMinutes) + " " +
            std::to_string(plain.pet.walkMinutes) + " " + std::to_string(noneSeen.training.walkMinutes) + " " +
            std::to_string(noneSeen.pet.walkMinutes));

    // NR14c: the plan names the lists the next snapshot seeks (the map thread's FillOwnStopsSeen does no search for a
    // bot with none of the three due): due riding wants the trainers and the mount vendors, due training the
    // trainers, a due pet (its walk is the beast's spawn) and nothing due neither; a wanted list the snapshot skipped
    // (the last plan wanted none) makes the plan ask for the next one at once, an unwanted skipped list does not
    auto const wanted = [&](std::vector<Need> const& list, bool trainersSkipped = false, bool vendorsSkipped = false)
    {
        Situation situation = filled(base, list);
        situation.trainersSkipped = trainersSkipped;
        situation.mountVendorsSkipped = vendorsSkipped;
        GoalPlan const plan = Goals::PlanGoals(bot, CircuitContext{}, situation);
        return std::to_string(plan.trainersWanted) + std::to_string(plan.mountVendorsWanted) +
               std::to_string(plan.ownStopsSkipped);
    };
    std::string const ridingWants = wanted({riding(RIDING_PRICE, RIDING_TRAINER)});
    std::string const trainingWants = wanted({training(SPELL_PRICE, true)});
    std::string const petWants = wanted({pet()}, true, true);
    std::string const noneWants = wanted({training(SPELL_PRICE * 3, false)}, true, true);
    std::string const ridingSkipped = wanted({riding(RIDING_PRICE, RIDING_TRAINER)}, false, true);
    std::string const trainingSkipped = wanted({training(SPELL_PRICE, true)}, true, false);
    std::string const trainingOtherSkipped = wanted({training(SPELL_PRICE, true)}, false, true);
    Add(checks,
        "own stop: the plan wants the trainers and mount vendors for due riding, the trainers for due training, no "
        "list for the pet or with nothing due; a wanted list its snapshot skipped asks for the next plan (mutant: the "
        "due stop waits for a plan that comes when the running goal ends)",
        ridingWants == "110" && trainingWants == "100" && petWants == "000" && noneWants == "000" &&
            ridingSkipped == "111" && trainingSkipped == "101" && trainingOtherSkipped == "100",
        ridingWants + " " + trainingWants + " " + petWants + " " + noneWants + " " + ridingSkipped + " " +
            trainingSkipped + " " + trainingOtherSkipped);
}

inline std::vector<Check> SyntheticChecks()
{
    std::vector<Check> checks;
    routeTally = {};
    Broke(checks);
    Errands(checks);
    Careful(checks);
    Curious(checks);
    MidTrip(checks);
    NoFlapping(checks);
    Outlevelled(checks);
    Busy(checks);
    RestAndHearth(checks);
    HearthStays(checks);
    Gathering(checks);
    Fishing(checks);
    Robust(checks);
    LogRows(checks);
    DryMovesAndDeliveries(checks);
    OutsideGoal(checks);
    HiddenStops(checks);
    TargetsAndRepairs(checks);
    RepairDuringQuest(checks);
    RepairFirst(checks);
    SickSafeAreas(checks);
    OwnStops(checks);
    Targets(checks);
    Routes(checks);
    HeldFromQuests(checks);
    QuestingOnly(checks);
    return checks;
}

// Information only: the microseconds of Rank over a busy situation (C2: a job stays well under a millisecond)
inline double RankMicroseconds()
{
    Snapshot bot = Bot(30, FLUSH);
    Situation situation = Base(30);
    for (std::uint32_t i = 0; i < 40; ++i)
    {
        situation.spots.push_back(Spot(100 + i, 300.0f + static_cast<float>(i), 80.0f, 28, 32));
        situation.hubs.push_back(Hub(200 + i, WESTFALL, 500.0f + static_cast<float>(i), 5.0f + static_cast<float>(i)));
    }
    auto const start = std::chrono::steady_clock::now();
    std::size_t kept = 0;
    for (std::uint32_t run = 0; run < TIMING_RUNS; ++run)
        kept += Goals::Rank(bot, situation).options.size();
    auto const elapsed = std::chrono::steady_clock::now() - start;
    double const total = std::chrono::duration<double, std::micro>(elapsed).count();
    return kept ? total / TIMING_RUNS : total;
}
}  // namespace GoalHarness
}  // namespace Planner

#endif
