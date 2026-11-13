/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GOALMODELHARNESS_H
#define _PLAYERBOT_GOALMODELHARNESS_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "GoalModel.h"
#include "GoalModelLog.h"

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

struct Check
{
    std::string name;
    bool pass{false};
    std::string text;
};

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

inline Situation Base()
{
    Situation situation;
    situation.here.hub = HOME_HUB;
    situation.here.zoneLow = 1;
    situation.here.zoneHigh = 10;
    situation.here.home = true;
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
        GoalPlan const plan = PlanGoals(bot, context, Base());
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
inline std::vector<std::uint32_t> Filled(Snapshot const& bot, CircuitContext const& context, Situation& situation)
{
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
    GoalPlan const moved = PlanGoals(outlevelled, context, live);
    Add(checks, "outlevelled: grey quests at home leave a dry circuit and the nearest next zone's hub wins",
        moved.circuit.take.empty() && moved.hub == world.hubs[HOME_CAMP] &&
            TopIs(moved.ranked, GoalKind::MoveZone, world.hubs[W1_CAMP], "outlevelled") && moves(moved.ranked) >= 2,
        Summary(moved.ranked));

    Snapshot const inBracket = Bot(10, FLUSH);
    Situation kept = live;
    std::vector<std::uint32_t> const none = Filled(inBracket, context, kept);
    GoalPlan const stays = PlanGoals(inBracket, context, live);
    Add(checks, "outlevelled: a bot within its bracket keeps questing here and gets no next zones",
        none.empty() && kept.here.zoneLow == 1 && kept.here.zoneHigh == 10 && kept.here.home &&
            TopIs(stays.ranked, GoalKind::Quest, world.hubs[HOME_CAMP], "circuit") && !moves(stays.ranked),
        "hubs " + CampNames(world, none) + "; " + Summary(stays.ranked));

    // Wetlands is above the bracket, Silverpine the enemy's, Stonetalon's first camp gives Horde quests only,
    // Westfall's second camp is a zone already listed, Ashenvale past the cap, Darkshore on another map
    Situation listed = live;
    std::vector<std::uint32_t> const hubs = Filled(outlevelled, context, listed);
    std::vector<std::uint32_t> const expected = {world.hubs[W1_CAMP], world.hubs[R_CAMP], world.hubs[L_CAMP],
                                                 world.hubs[S2_CAMP]};
    Add(checks, "next zones: the nearest hubs of zones in the bracket, one per zone, none of the enemy's, at most four",
        hubs == expected && listed.hubs.front().zone == WESTFALL && listed.hubs.front().home &&
            !listed.hubs.back().home && listed.hubs.front().travelMinutes > 0.0f &&
            listed.hubs.front().travelMinutes < listed.hubs.back().travelMinutes,
        "hubs " + CampNames(world, hubs));

    Situation beyond = live;
    std::vector<std::uint32_t> const past = Filled(Bot(40, FLUSH), context, beyond);
    GoalPlan const nowhere = PlanGoals(Bot(40, FLUSH), context, live);
    CircuitContext bare = context;
    bare.indexes = nullptr;
    Situation unset = live;
    std::vector<std::uint32_t> const unknown = Filled(outlevelled, bare, unset);
    Add(checks, "next zones: no zone in the bracket or no indexes yet gives no hub options",
        past.empty() && beyond.here.zoneHigh == 10 && nowhere.ranked.ok && !moves(nowhere.ranked) && unknown.empty() &&
            !unset.here.zoneHigh,
        "level 40 " + CampNames(world, past) + "; no indexes " + CampNames(world, unknown));

    // A travel cost that finds no way to Westfall's first hub: its second hub stands for the zone
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
        reachable.size() == NEXT_ZONE_HUBS &&
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
    Inn nearer{701, {EASTERN, -9460.0f, 20.0f, 0.0f}, 60.0f, true, false, false};
    Inn usual{702, {EASTERN, -9390.0f, 60.0f, 0.0f}, 75.0f, true, false, true};
    situation.inns = {nearer, usual};
    Ranked const neutral = Rank(bot, situation);
    Add(checks, "hearth: a far bind makes the nearest fitting inn a goal",
        TopIs(neutral, GoalKind::Hearth, 701, "bind_far"), Summary(neutral));
    bot.traits = Traits({{TraitRules::Trait::Orderliness, STRONG_TRAIT}});
    Ranked const orderly = Rank(bot, situation);
    Add(checks, "hearth: an orderly bot goes back to its usual inn", TopIs(orderly, GoalKind::Hearth, 702),
        Summary(orderly));
    Snapshot atInn = Bot(10, FLUSH);
    Ranked const bound = Rank(atInn, situation);
    Add(checks, "hearth: a bind nearby needs no inn", !Has(bound, GoalKind::Hearth), Summary(bound));
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

    Situation outside = situation;
    outside.external =
        External{GoalKind::TravelElsewhere, 1519, {EASTERN, -8900.0f, 550.0f, 0.0f}, 12.0f, "Visit the Cathedral"};
    Ranked const told = Rank(bot, outside);
    Add(checks, "robust: an outside goal is carried first with its reason",
        TopIs(told, GoalKind::TravelElsewhere, 1519, "visit_the_cathedral"), Summary(told));
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

// An outside (LLM) goal has no value of its own: it stays until it ends, and only a stop worth more breaks in
inline void OutsideGoal(std::vector<Check>& checks)
{
    Option outside = Made(GoalKind::TravelElsewhere, OUTSIDE_TARGET, 0.0f, 10.0f);
    outside.pinned = true;
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
    Situation repair = Base();
    repair.bags.vendor = {78, WorldTravel::ERRAND_MAX_WALK_MINUTES + 5.0f, true};
    Ranked const farRanked = Rank(worn, repair);
    repair.bags.vendor.walkMinutes = 2.0f;
    Ranked const nearRanked = Rank(worn, repair);
    Add(checks, "errand: a repair keeps to the vendor walk limit",
        !Has(farRanked, GoalKind::Errand) && Has(nearRanked, GoalKind::Errand),
        Summary(farRanked) + " / " + Summary(nearRanked));
}

inline std::vector<Check> SyntheticChecks()
{
    std::vector<Check> checks;
    Broke(checks);
    Errands(checks);
    Careful(checks);
    Curious(checks);
    MidTrip(checks);
    NoFlapping(checks);
    Outlevelled(checks);
    Busy(checks);
    RestAndHearth(checks);
    Gathering(checks);
    Robust(checks);
    LogRows(checks);
    DryMovesAndDeliveries(checks);
    OutsideGoal(checks);
    HiddenStops(checks);
    TargetsAndRepairs(checks);
    return checks;
}

// Information only: the microseconds of Rank over a busy situation (C2: a job stays well under a millisecond)
inline double RankMicroseconds()
{
    Snapshot bot = Bot(30, FLUSH);
    Situation situation = Base();
    for (std::uint32_t i = 0; i < 40; ++i)
    {
        situation.spots.push_back(Spot(100 + i, 300.0f + static_cast<float>(i), 80.0f, 28, 32));
        situation.hubs.push_back(Hub(200 + i, WESTFALL, 500.0f + static_cast<float>(i), 5.0f + static_cast<float>(i)));
    }
    auto const start = std::chrono::steady_clock::now();
    std::size_t kept = 0;
    for (std::uint32_t run = 0; run < TIMING_RUNS; ++run)
        kept += Rank(bot, situation).options.size();
    auto const elapsed = std::chrono::steady_clock::now() - start;
    double const total = std::chrono::duration<double, std::micro>(elapsed).count();
    return kept ? total / TIMING_RUNS : total;
}
}  // namespace GoalHarness
}  // namespace Planner

#endif
