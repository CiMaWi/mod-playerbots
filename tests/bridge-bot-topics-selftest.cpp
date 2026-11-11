/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the bridge's planner topics (travel plan WT18a, BridgeBotTopics.h): the per-topic limits, the row
// kinds and their order, the planner's scope, the word forms (no trait number reaches a row) and the route rows.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot /I src/Ai/World/Rpg /I src/Mgr/Travel
// tests/bridge-bot-topics-selftest.cpp

#include <cstdio>
#include <memory>
#include <string>

#include "BridgeBotTopics.h"

namespace
{
using namespace PlayerbotBridge;
using namespace PlayerbotBridge::Topics;
using namespace Planner;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

bool HasDigit(std::string const& text) { return text.find_first_of("0123456789") != std::string::npos; }

std::uint8_t Code(BotStateItem const& item) { return item.code; }

template <typename Row>
bool Is(BotStateItem const& item, Row row)
{
    return item.code == static_cast<std::uint8_t>(row);
}

Goals::Option OptionOf(Goals::GoalKind kind, std::uint32_t target, char const* reason, float perMinute, float minutes)
{
    Goals::Option option;
    option.kind = kind;
    option.target = target;
    option.reason = reason;
    option.valuePerMinute = perMinute;
    option.minutes = minutes;
    return option;
}

// A bot in scope with a plan: ten stops, stop i holds quest 100 + i (stop 2 also quest 50, closed), the bot at 0,0,0
PlanInput InScope()
{
    PlanInput in;
    in.inScope = true;
    in.seen = true;
    in.nowMs = 200000;
    auto plan = std::make_shared<Goals::GoalPlan>();
    plan->circuit.ok = true;
    for (std::uint32_t i = 0; i < 10; ++i)
    {
        CircuitStop stop;
        stop.x = 30.0f + static_cast<float>(i);
        stop.y = 40.0f;
        if (i == 2)
            stop.tasks.push_back({50, 0, 0.0f, 0.0f, 0.0f});
        stop.tasks.push_back({100 + i, 0, 0.0f, 0.0f, 0.0f});
        if (i == 0)
            stop.tasks.push_back({100, 1, 0.0f, 0.0f, 0.0f});
        plan->circuit.stops.push_back(stop);
    }
    plan->ranked.ok = true;
    in.view.plan = plan;
    in.open = [](std::uint32_t quest) { return quest >= 100; };
    in.title = [](std::uint32_t quest) { return "Quest " + std::to_string(quest); };
    return in;
}

void SetGoal(PlanInput& in, Goals::GoalKind kind, std::uint32_t target, GoalApply::Source source, char const* reason)
{
    in.view.goal.kind = kind;
    in.view.goal.target = target;
    in.view.goal.source = source;
    in.view.goal.reason = reason;
    in.view.goal.minutes = 12.5f;
    in.view.goal.startedMs = 110000;
}

Goals::GoalPlan& PlanOf(PlanInput& in) { return const_cast<Goals::GoalPlan&>(*in.view.plan); }
}  // namespace

int main()
{
    // Limits: the six planner topics have their own, the old five the general cap
    Check(Limit(BotStateTopic::Plan) == 8 && Limit(BotStateTopic::Options) == 5 && Limit(BotStateTopic::Route) == 12 &&
              Limit(BotStateTopic::QuestPlan) == 12 && Limit(BotStateTopic::Bags) == 6 &&
              Limit(BotStateTopic::Places) == 24,
          "limits plan 8, options 5, route 12, quest_plan 12, bags 6, places 24");
    Check(Limit(BotStateTopic::Spells) == BotStateTopicMaxItems && Limit(BotStateTopic::Inventory) == 64,
          "the old topics keep the general cap");
    Check(static_cast<int>(BotStateTopic::Spells) == 4 && static_cast<int>(BotStateTopic::Plan) == 5 &&
              static_cast<int>(BotStateTopic::Places) == 10,
          "wire order: the planner topics follow spells, places last");
    Check(Limit(BotStateTopic::Places) <= BotStateTopicMaxItems, "every limit fits the general cap");

    // Word forms
    Check(Words("Low Health") == "low health" && Words("curious 0.73") == "curious" &&
              Words("careful +12%, rooted") == "careful rooted" && Words("bags_full") == "bags_full",
          "Words: lower case, numbers dropped, separators one space");
    Check(Word("graph not ready") == "graph_not_ready" && Word("no route") == "no_route", "Word joins by _");
    Check(RoundUp(0.1f) == 1 && RoundUp(-3.0f) == 0 && RoundUp(std::nanf("")) == 0 && Rounded(2.4f) == 2 &&
              Rounded(2.6f) == 3,
          "seconds round up, values to the nearest, never negative or NaN");

    std::vector<BotStateItem> items;

    // Plan: out of scope, unseen, without a goal
    {
        PlanInput in = InScope();
        in.inScope = false;
        SetGoal(in, Goals::GoalKind::Quest, 1, GoalApply::Source::Planner, "circuit");
        PlanItems(in, items);
        Check(items.size() == 2 && items[0].name == "none" && Is(items[0], PlanRow::Goal) &&
                  items[1].name == "not_planner" && Is(items[1], PlanRow::Reason),
              "plan: a bot out of the planner's scope (a master) shows none, not_planner");
        in = InScope();
        in.seen = false;
        PlanItems(in, items);
        Check(items.size() == 2 && items[1].name == "not_planner", "plan: a bot the planner never saw: not_planner");
        in = InScope();
        in.view.jobOut = true;
        PlanItems(in, items);
        Check(items.size() == 2 && items[0].name == "none" && items[1].name == "planning", "plan: no goal, job out");
        in.view.jobOut = false;
        in.view.lastFallback = GoalApply::Fallback::NoPlan;
        PlanItems(in, items);
        Check(items.size() == 2 && items[1].name == "fallback_no_plan", "plan: no goal, the last fallback word");
        in.view.lastFallback = GoalApply::Fallback::None;
        PlanItems(in, items);
        Check(items.size() == 2 && items[1].name == "idle", "plan: no goal, nothing pending: idle");
        in.view.paused = true;
        in.view.jobOut = true;
        PlanItems(in, items);
        Check(items.size() == 2 && items[0].name == "none" && items[1].name == "paused",
              "plan: a paused bot shows none, paused (before planning)");
    }

    // Plan: a circuit goal with its next stops, bounded
    {
        PlanInput in = InScope();
        SetGoal(in, Goals::GoalKind::Quest, 7, GoalApply::Source::Bridge, "Circuit 0.75 curious");
        PlanItems(in, items);
        Check(items.size() == Limit(BotStateTopic::Plan), "plan: ten open stops are cut to the plan limit");
        Check(items.size() >= 4 && items[0].name == "quest" && Is(items[0], PlanRow::Goal) && items[0].value == 90 &&
                  items[0].extra == 750,
              "plan: goal row kind word, 90 s in, 750 s planned");
        Check(items.size() >= 4 && items[1].name == "bridge" && Is(items[1], PlanRow::Source), "plan: source row");
        Check(items.size() >= 4 && items[2].name == "circuit curious" && Is(items[2], PlanRow::Reason),
              "plan: reason row as words, the trait number dropped");
        bool stops = items.size() == 8;
        for (std::size_t i = 3; i < items.size(); ++i)
            stops = stops && Is(items[i], PlanRow::Stop);
        Check(stops, "plan: rows 4-8 are stops");
        Check(items.size() >= 6 && items[3].name == "Quest 100" && items[3].value == 2 && items[3].extra == 50 &&
                  items[5].name == "Quest 102" && items[5].value == 1,
              "plan: a stop shows its first open quest, its open tasks and yards; closed tasks are not counted");
        bool clean = true;
        for (std::size_t i = 0; i < 3 && i < items.size(); ++i)
            clean = clean && !HasDigit(items[i].name);
        Check(clean, "plan: no number in the goal, source and reason rows");
        in.open = [](std::uint32_t) { return false; };
        PlanItems(in, items);
        Check(items.size() == 3, "plan: stops without open tasks are left out");
        in = InScope();
        SetGoal(in, Goals::GoalKind::Grind, 7, GoalApply::Source::Planner, "xp");
        PlanItems(in, items);
        Check(items.size() == 3 && items[0].name == "grind" && items[1].name == "planner",
              "plan: a non-quest goal has no stops");
    }

    // Options: top five, words, the current goal flagged, scope
    {
        PlanInput in = InScope();
        SetGoal(in, Goals::GoalKind::Grind, 3, GoalApply::Source::Planner, "xp");
        Goals::GoalPlan& plan = PlanOf(in);
        for (std::uint32_t i = 0; i < 7; ++i)
            plan.ranked.options.push_back(
                OptionOf(Goals::GoalKind::Grind, i + 1, "xp", 450.4f - static_cast<float>(i), 20.0f));
        plan.ranked.options[0].traitNotes = {"careful 0.8", "curious"};
        plan.ranked.options[1].kind = Goals::GoalKind::Rest;
        plan.ranked.options[1].reason = "low_health";
        OptionsItems(in, items);
        Check(items.size() == Limit(BotStateTopic::Options), "options: seven options are cut to five");
        Check(items.size() == 5 && items[0].name == "grind xp careful curious" && items[0].value == 450 &&
                  items[0].extra == 1200 && items[1].name == "rest low_health" && items[4].value == 446,
              "options: kind, reason and trait words in ranking order, value per minute, seconds");
        bool clean = true;
        for (BotStateItem const& item : items)
            clean = clean && !HasDigit(item.name);
        Check(clean, "options: no trait number in any row");
        Check(items.size() == 5 && Code(items[2]) == 1 && Code(items[0]) == 0 && Code(items[3]) == 0,
              "options: code 1 only on the goal pursued now (grind 3)");
        in.inScope = false;
        OptionsItems(in, items);
        Check(items.empty(), "options: empty out of scope");
        in = InScope();
        in.view.plan.reset();
        OptionsItems(in, items);
        Check(items.empty(), "options: empty before a plan");
    }

    // Quest plan: one row per quest in walking order, bounded
    {
        PlanInput in = InScope();
        Goals::GoalPlan& plan = PlanOf(in);
        plan.circuit.stops.clear();
        plan.circuit.take = {{1, 0, 0.0f, false}, {2, 0, 0.0f, true}};
        CircuitStop first;
        first.tasks = {{1, 0, 0.0f, 0.0f, 0.0f}, {3, 0, 0.0f, 0.0f, 0.0f}};
        CircuitStop second;
        second.tasks = {{1, 1, 0.0f, 0.0f, 0.0f}};
        plan.circuit.stops = {first, second};
        plan.circuit.turnIns = {{1, 0, 0}, {3, 0, 0}, {4, 0, 0}};
        plan.circuit.carried = {5};
        plan.circuit.deferred = {6, 1};
        QuestPlanItems(in, items);
        Check(items.size() == 6 && items[0].value == 1 && Is(items[0], QuestRow::Take) && items[1].value == 2 &&
                  Is(items[1], QuestRow::Carried) && items[2].value == 3 && Is(items[2], QuestRow::Task) &&
                  items[2].extra == 1 && items[3].value == 4 && Is(items[3], QuestRow::TurnIn) && items[4].value == 5 &&
                  items[5].value == 6 && Is(items[5], QuestRow::Deferred),
              "quest_plan: take, carried, task (its points), turn in, carried, deferred; each quest once");
        Check(items.size() == 6 && items[0].name == "Quest 1", "quest_plan: the title names the quest");
        for (std::uint32_t i = 0; i < 20; ++i)
            plan.circuit.deferred.push_back(200 + i);
        QuestPlanItems(in, items);
        Check(items.size() == Limit(BotStateTopic::QuestPlan), "quest_plan: a long plan is cut to twelve");
        plan.circuit.ok = false;
        QuestPlanItems(in, items);
        Check(items.empty(), "quest_plan: empty without a circuit");
        plan.circuit.ok = true;
        in.inScope = false;
        QuestPlanItems(in, items);
        Check(items.empty(), "quest_plan: empty out of scope");
    }

    // Bags: free slots, then the plan's first errand and bag purchase
    {
        BagsInput bags;
        bags.freeSlots = 3;
        bags.slots = 52;
        BagsItems(bags, items);
        Check(items.size() == 1 && items[0].name == "free_slots" && items[0].value == 3 && items[0].extra == 52 &&
                  Is(items[0], BagRow::Free),
              "bags: free slots without a plan");
        PlanInput in = InScope();
        Goals::GoalPlan& plan = PlanOf(in);
        plan.ranked.options = {OptionOf(Goals::GoalKind::Grind, 1, "xp", 400.0f, 20.0f),
                               OptionOf(Goals::GoalKind::Errand, 77, "bags full vendor near", 90.0f, 2.0f),
                               OptionOf(Goals::GoalKind::Errand, 78, "repair", 80.0f, 3.0f),
                               OptionOf(Goals::GoalKind::BuyBag, 4500, "bag_worth_price", 70.0f, 4.0f)};
        bags.plan = &in;
        BagsItems(bags, items);
        Check(items.size() == 3 && Is(items[1], BagRow::Sell) && items[1].name == "bags_full_vendor_near" &&
                  items[1].value == 120 && items[1].extra == 77 && Is(items[2], BagRow::Bag) &&
                  items[2].extra == 4500 && items[2].value == 240,
              "bags: the first errand is the next sell stop, then the bag plan");
        in.inScope = false;
        BagsItems(bags, items);
        Check(items.size() == 1, "bags: out of scope only the free slots");
    }

    // Places: the hearth, then known flight points, the bot's map nearest first, bounded
    {
        PlacesInput in;
        in.bot = {1, 0.0f, 0.0f, 0.0f};
        in.hasHearthstone = true;
        in.hearthCooldownMs = 1500;
        in.hearthArea = "Razor Hill";
        for (std::uint32_t i = 0; i < 30; ++i)
        {
            bool const here = i % 3 == 0;
            in.known.push_back({40 - i,
                                "Node " + std::to_string(40 - i),
                                {here ? 1u : 0u, here ? 100.0f * static_cast<float>(i) : 5.0f, 0.0f, 0.0f}});
        }
        PlacesItems(in, items);
        Check(items.size() == Limit(BotStateTopic::Places), "places: thirty known flight points are cut to 24 rows");
        Check(!items.empty() && items[0].name == "Razor Hill" && Is(items[0], PlaceRow::Hearth) &&
                  items[0].value == 2 && items[0].extra == 1,
              "places: the hearth first, cooldown seconds rounded up, carries a stone");
        Check(items.size() > 11 && Is(items[1], PlaceRow::Flight) && items[1].value == 40 && items[1].extra == 0 &&
                  items[2].value == 37 && items[2].extra == 300 && Is(items[10], PlaceRow::Flight) &&
                  Is(items[11], PlaceRow::FlightAway) && items[11].value == 11 && items[11].extra == 0,
              "places: same-map points nearest first (yards), then other maps by node (map id)");
        in.known.clear();
        PlacesItems(in, items);
        Check(items.size() == 1, "places: no known flight point, only the hearth");
    }

    // Route: failures, total and legs, bounded
    {
        RouteInput in;
        RouteItems(in, items);
        Check(items.size() == 1 && items[0].name == "unknown_place" && Is(items[0], RouteRow::Failed),
              "route: an unknown place");
        in.known = true;
        in.place = "thunderbluff";
        in.route.failure = TravelRoute::Failure::GraphNotReady;
        RouteItems(in, items);
        Check(items.size() == 1 && items[0].name == "graph_not_ready" && Is(items[0], RouteRow::Failed),
              "route: the graph not ready");
        in.route.failure = TravelRoute::Failure::NoRoute;
        RouteItems(in, items);
        Check(items.size() == 1 && items[0].name == "no_route", "route: no route");
        in.route.ok = true;
        in.route.failure = TravelRoute::Failure::None;
        in.route.totalSeconds = 400.2f;
        for (std::uint32_t i = 0; i < 3; ++i)
        {
            TravelRoute::Leg leg;
            leg.type = i == 1 ? TravelRoute::LegType::Flight : TravelRoute::LegType::Walk;
            leg.seconds = 100.5f;
            leg.fareCopper = i == 1 ? 150 : 0;
            in.route.legs.push_back(leg);
        }
        RouteItems(in, items);
        Check(items.size() == 4 && items[0].name == "thunderbluff" && Is(items[0], RouteRow::Total) &&
                  items[0].value == 401 && items[0].extra == 150 && items[1].name == "walk" &&
                  items[2].name == "flight" && items[2].value == 101 && items[2].extra == 150 &&
                  Is(items[3], RouteRow::Leg),
              "route: total (seconds, fare), then the legs in order");
        for (std::uint32_t i = 0; i < 20; ++i)
            in.route.legs.push_back(in.route.legs.front());
        RouteItems(in, items);
        Check(items.size() == Limit(BotStateTopic::Route) && Is(items[0], RouteRow::Total),
              "route: a long route is cut to twelve rows, the total kept");
    }

    std::printf("bridge-bot-topics selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
