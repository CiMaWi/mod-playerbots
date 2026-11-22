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
#include "BridgeItemsTopic.h"
#include "BridgeNeedsTopic.h"
#include "BridgeProfessionsTopic.h"
#include "RecordWords.h"

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
    return item.code == static_cast<std::uint8_t>(row) && item.word == RowWord(row);
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
              Limit(BotStateTopic::QuestPlan) == 12 && Limit(BotStateTopic::Bags) == 12 &&
              Limit(BotStateTopic::Places) == 24,
          "limits plan 8, options 5, route 12, quest_plan 12, bags 12, places 24");
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
        in.view.lastFallback = GoalApply::Fallback::NoOption;
        PlanItems(in, items);
        Check(items.size() == 2 && items[1].name == "fallback_no_option", "plan: no goal, the last fallback word");
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

    // Bags, WB9b: the waiting mail and the kept items
    {
        auto const named = [](std::vector<BotStateItem> const& list, char const* name)
        { return std::count_if(list.begin(), list.end(), [&](BotStateItem const& i) { return i.name == name; }); };
        auto const kept = [](char const* name, std::uint32_t level, bool inBank, bool due)
        { return KeptLine{name, level, inBank, due}; };

        // mail waiting, nothing kept (a governed bot): mail_waiting and the bank's free slots only
        BagsInput bags;
        bags.freeSlots = 3;
        bags.slots = 52;
        bags.mailWaiting = 3;
        bags.keepsPossessions = true;
        bags.bankFree = 28;
        bags.bankSlots = 28;
        BagsItems(bags, items);
        Check(items.size() == 3 && items[1].name == "mail_waiting" && items[1].value == 3 &&
                  Is(items[1], BagRow::MailWaiting) && items[2].name == "bank_free" && items[2].value == 28 &&
                  items[2].extra == 28 && Is(items[2], BagRow::BankFree) && !named(items, "mail_seen") &&
                  !named(items, "mail_expected"),
              "bags: mail waiting and nothing kept: mail_waiting and bank_free only");

        // items left in the mail and gold expected; no mail waiting gives no mail_waiting
        bags.mailWaiting = 0;
        bags.mailSeenCopper = 5000;
        bags.mailExpectedCopper = 120;
        BagsItems(bags, items);
        Check(!named(items, "mail_waiting") && named(items, "mail_seen") == 1 && named(items, "mail_expected") == 1 &&
                  items[1].name == "mail_seen" && items[1].value == 5000 && Is(items[1], BagRow::MailSeen) &&
                  items[2].name == "mail_expected" && items[2].value == 120 && Is(items[2], BagRow::MailExpected),
              "bags: mail seen and expected rows with their copper, no mail_waiting at zero");

        // a due item in the bank and a kept one in the bags: names stay names, due first, bank 1 / bags 0
        bags.mailSeenCopper = 0;
        bags.mailExpectedCopper = 0;
        bags.kept = {kept("Frostweave Robe", 60, false, false), kept("Chromatic Cloak", 57, true, true)};
        bags.bankFree = 26;
        BagsItems(bags, items);
        Check(items.size() == 4 && items[1].name == "Chromatic Cloak" && items[1].value == 57 && items[1].extra == 1 &&
                  Is(items[1], BagRow::Kept) && items[2].name == "Frostweave Robe" && items[2].value == 60 &&
                  items[2].extra == 0 && Is(items[2], BagRow::Kept) && items[3].name == "bank_free" &&
                  items[3].value == 26,
              "bags: a due bank item before a kept bag item, by name, then bank_free");

        // more kept items than the cap: seven kept, two due (the last two given), four shown, due ones first
        bags.kept.clear();
        for (int index = 0; index < 5; ++index)
            bags.kept.push_back(kept("Later Item", 70 + index, true, false));
        bags.kept.push_back(kept("Due One", 55, true, true));
        bags.kept.push_back(kept("Due Two", 56, false, true));
        BagsItems(bags, items);
        Check(named(items, "Later Item") == 2 && items.size() == 1 + KEPT_SHOWN + 1 && items[1].name == "Due One" &&
                  items[2].name == "Due Two" && items.back().name == "bank_free" &&
                  items.size() <= Limit(BotStateTopic::Bags),
              "bags: seven kept show four, both due ones first");

        // nothing at all
        BagsInput none;
        none.freeSlots = 5;
        none.slots = 20;
        BagsItems(none, items);
        Check(items.size() == 1 && items[0].name == "free_slots",
              "bags: nothing waiting, nothing kept: free_slots only");
        none.keepsPossessions = true;
        none.bankFree = 28;
        none.bankSlots = 28;
        BagsItems(none, items);
        Check(items.size() == 2 && items[1].name == "bank_free", "bags: a governed bot with nothing kept: bank_free");

        // a bot the rule does not govern: mail rows only, even with kept filled
        BagsInput free;
        free.freeSlots = 5;
        free.slots = 20;
        free.mailWaiting = 2;
        free.kept = {kept("Chromatic Cloak", 57, true, true)};
        free.bankFree = 10;
        BagsItems(free, items);
        Check(items.size() == 2 && items[1].name == "mail_waiting" && !named(items, "Chromatic Cloak") &&
                  !named(items, "bank_free"),
              "bags: a bot the bank rule does not govern: mail rows only, no kept, no bank_free");

        // out of the planner scope: the mail rows still appear
        PlanInput out = InScope();
        out.inScope = false;
        bags.plan = &out;
        bags.kept.clear();
        bags.mailWaiting = 2;
        BagsItems(bags, items);
        Check(items.size() == 3 && items[1].name == "mail_waiting" && items[2].name == "bank_free",
              "bags: out of scope, the mail rows and bank_free still appear");

        // the biggest answer fits the limit: plan rows (3), mail (3), four kept, bank_free = 11
        PlanInput in = InScope();
        PlanOf(in).ranked.options = {OptionOf(Goals::GoalKind::Errand, 77, "mail", 90.0f, 2.0f),
                                     OptionOf(Goals::GoalKind::BuyBag, 4500, "bag_worth_price", 70.0f, 4.0f)};
        bags.plan = &in;
        bags.mailWaiting = 2;
        bags.mailSeenCopper = 10;
        bags.mailExpectedCopper = 10;
        bags.kept.clear();
        for (int index = 0; index < 6; ++index)
            bags.kept.push_back(kept("Thing", 60 + index, index % 2 == 0, false));
        BagsItems(bags, items);
        Check(items.size() == 11 && items.size() <= Limit(BotStateTopic::Bags), "bags: the fullest answer is 11 rows");

        // the mail rows are fixed words and carry no digit, no mail content
        bool plain = true;
        for (BotStateItem const& item : items)
            if (item.name.rfind("mail_", 0) == 0)
                plain = plain && !HasDigit(item.name) &&
                        (item.name == "mail_waiting" || item.name == "mail_seen" || item.name == "mail_expected");
        Check(plain && named(items, "mail_waiting") && named(items, "mail_seen") && named(items, "mail_expected"),
              "bags: the mail rows are their fixed words, no digit, no mail content");

        // errand reasons read as words
        for (char const* reason : {"mail", "bank_withdraw", "bank_deposit"})
        {
            PlanOf(in).ranked.options = {OptionOf(Goals::GoalKind::Errand, 77, reason, 90.0f, 2.0f)};
            BagsItems(bags, items);
            Check(items.size() > 1 && items[1].name == reason && Is(items[1], BagRow::Sell),
                  "bags: an errand reason reads as its word");
        }
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

    // LC16b: a topic beyond the fourteen, on a test-only row: one argument of a name kind, items in lines
    {
        static constexpr ParamRow item[] = {{"item", ParamKind::Item, OrderField::ItemName, "An item you carry.", {}}};
        static constexpr ParamRow two[] = {{"item", ParamKind::Item, OrderField::ItemName, "An item you carry.", {}},
                                           {"spell", ParamKind::Spell, OrderField::SpellName, "A spell you know.", {}}};
        BotStateTopic const next = static_cast<BotStateTopic>(std::size(TopicTable));
        TopicRow const lines[] = {{next, "item_story", "What you know of an item.", item, 12, true}};
        TopicRow const plain[] = {{next, "item_story", "What you know of an item.", item, 12, false}};
        TopicRow const twice[] = {{next, "item_story", "What you know of an item.", two, 12, true}};
        Check(TopicRowsValid(TopicTable) && TopicRowsValid(lines) && !TopicRowsValid(plain) && !TopicRowsValid(twice),
              "topics: a new topic takes one name and answers lines");
    }

    // WU23: the needs topic words one NeedList: worn gear and enough gold, short for a riding rank, nothing due, a bot
    // the rules do not govern, more needs than the cap, and the mutants of the three guards
    {
        using UpkeepNeeds::Need;
        using UpkeepNeeds::NeedList;
        using UpkeepNeeds::NeedOrder;
        constexpr std::uint64_t GOLD = UpkeepNeeds::COPPER_PER_GOLD;
        auto const need = [](NeedOrder order, std::uint64_t copper, std::uint64_t funded, bool due, std::uint32_t item)
        {
            Need made;
            made.order = order;
            made.copper = copper;
            made.funded = funded;
            made.due = due;
            made.item = item;
            made.units = 1;
            made.without = "fights with worn or broken gear";
            return made;
        };
        constexpr std::uint32_t BREAD_ID = 4542;
        auto const name = [](std::uint32_t id) { return id == BREAD_ID ? std::string("Good Bread") : std::string(); };
        auto const idAsText = [](std::uint32_t id) { return std::to_string(id); };  // the mutant's lookup
        auto const make = [&name](NeedList const& list, bool governed = true)
        {
            NeedsInput in;
            in.governed = governed;
            in.cls = 1;
            in.list = &list;
            in.itemName = name;
            std::vector<BotStateItem> items;
            NeedsItems(in, items);
            return items;
        };
        auto const has = [](std::vector<BotStateItem> const& items, NeedRow row)
        {
            return std::any_of(items.begin(), items.end(),
                               [row](BotStateItem const& item) { return item.code == static_cast<std::uint8_t>(row); });
        };
        auto const lines = [](std::vector<BotStateItem> const& items)
        {
            bool every = !items.empty();
            for (BotStateItem const& item : items)
                every = every && !item.text.empty() && !item.name.empty();
            return every;
        };

        // Worn gear and enough gold: the repair is due and set aside, the reserve is shown, no saving entry
        NeedList worn;
        worn.needs.push_back(need(NeedOrder::Repair, 1200, 1200, true, 0));
        worn.money = 5 * GOLD;
        worn.reserve = 1200;
        std::vector<BotStateItem> const wornItems = make(worn);
        Check(wornItems.size() == 2 && wornItems[0].name == "repair" && Is(wornItems[0], NeedRow::Due) &&
                  wornItems[0].value == 1200 && wornItems[0].extra == 1200 &&
                  wornItems[0].text.find("repairs") != std::string::npos &&
                  wornItems[0].text.find("the gold is set aside") != std::string::npos &&
                  Is(wornItems[1], NeedRow::Reserve) && wornItems[1].extra == 5 * GOLD &&
                  !has(wornItems, NeedRow::Saving) && lines(wornItems),
              "needs: worn gear and enough gold: the repair, the reserve, no saving entry");
        // Mutant: a shortfall forced onto the same list would add the saving entry the real answer withholds
        NeedList forced = worn;
        forced.shortfall = 1;
        Check(has(make(forced), NeedRow::Saving) && !has(wornItems, NeedRow::Saving),
              "needs: a saving entry only with a shortfall (mutant: a shortfall of 1 adds one)");

        // Short for a riding rank: the saving entry names the rank and the amount
        NeedList riding;
        riding.money = 2 * GOLD;
        riding.reserve = 40 * GOLD;
        riding.reserveRiding = 40 * GOLD;
        riding.shortfall = 38 * GOLD;
        UpkeepNeeds::RankValue rank;
        rank.rank = UpkeepNeeds::RIDING_RANKS[1];
        rank.reserved = true;
        riding.ranks.push_back(rank);
        std::vector<BotStateItem> const ridingItems = make(riding);
        Check(ridingItems.size() == 3 && Is(ridingItems[0], NeedRow::Nothing) && Is(ridingItems[1], NeedRow::Reserve) &&
                  Is(ridingItems[2], NeedRow::Saving) &&
                  ridingItems[2].text == "I am saving: 38g 0s 0c short for journeyman riding." &&
                  ridingItems[2].value == 38 * GOLD && ridingItems[2].extra == 2 * GOLD,
              "needs: short for a riding rank: nothing due, the saving entry names the rank and the amount");
        // A need its gold does not cover is what the bot is short for, named in words
        NeedList training = riding;
        training.needs.push_back(need(NeedOrder::Training, 6000, 100, true, 0));
        training.needs.back().units = 3;
        training.needs.back().without = "spells it could cast";
        std::vector<BotStateItem> const trainingItems = make(training);
        Check(trainingItems.size() == 3 && trainingItems[0].name == "training" &&
                  trainingItems[0].text.find("3 spells at the warrior trainer") != std::string::npos &&
                  trainingItems[2].text.find("short for 3 spells at the warrior trainer") != std::string::npos &&
                  trainingItems[0].text.find("only 1s 0c is set aside") != std::string::npos,
              "needs: the unfunded need is the one the bot is short for, by its trainer");

        // Nothing due says so in one entry
        NeedList idle;
        idle.money = GOLD;
        std::vector<BotStateItem> const idleItems = make(idle);
        Check(idleItems.size() == 1 && idleItems[0].name == "nothing" && Is(idleItems[0], NeedRow::Nothing) &&
                  lines(idleItems),
              "needs: nothing due answers in one entry");
        // A need at no cost (a pet to tame) names no price
        NeedList pet;
        pet.needs.push_back(need(NeedOrder::Pet, 0, 0, true, 0));
        pet.needs.back().without = "no pet";
        std::vector<BotStateItem> const petItems = make(pet);
        Check(
            petItems.size() == 1 && petItems[0].text == "I must get a new pet, at no cost; going without means no pet.",
            "needs: a need at no cost names no price");

        // A bot the rules do not govern: one provided entry, claiming no need; mutant: the list formatted anyway
        std::vector<BotStateItem> const provided = make(worn, false);
        Check(provided.size() == 1 && provided[0].name == "provided" && Is(provided[0], NeedRow::Provided) &&
                  !has(provided, NeedRow::Due) && !has(provided, NeedRow::Reserve) &&
                  has(make(worn, true), NeedRow::Due),
              "needs: a bot the rules do not govern answers provided (mutant: governed formats the list)");

        // Items by name, never by id (mutant: a lookup that answers the id)
        NeedList bread;
        bread.needs.push_back(need(NeedOrder::FoodWater, 800, 800, true, BREAD_ID));
        bread.needs.back().without = "hunger between fights";
        bread.reserve = 800;
        std::vector<BotStateItem> const breadItems = make(bread);
        NeedsInput mutant;
        mutant.governed = true;
        mutant.list = &bread;
        mutant.itemName = idAsText;
        std::vector<BotStateItem> mutantItems;
        NeedsItems(mutant, mutantItems);
        Check(breadItems[0].text.find("Good Bread") != std::string::npos &&
                  breadItems[0].text.find("4542") == std::string::npos &&
                  mutantItems[0].text.find("4542") != std::string::npos,
              "needs: an item by its name, not its id (mutant: the id as text)");

        // More needs than the cap: cut at eight, the reserve and the saving entry kept at the end
        NeedList many;
        for (std::uint32_t index = 0; index < 12; ++index)
            many.needs.push_back(need(NeedOrder::FoodWater, 100, 100, true, BREAD_ID));
        many.reserve = 1200;
        many.money = 100;
        many.shortfall = 1100;
        std::vector<BotStateItem> const manyItems = make(many);
        Check(manyItems.size() == NEEDS_SHOWN && manyItems.size() == Limit(BotStateTopic::Needs) &&
                  Is(manyItems[NEEDS_SHOWN - 2], NeedRow::Reserve) && Is(manyItems[NEEDS_SHOWN - 1], NeedRow::Saving),
              "needs: more needs than the cap are cut, the reserve and the saving entry kept");

        // One list: the topic's need entries are the command's needs, in its order
        NeedList both;
        both.needs.push_back(need(NeedOrder::Repair, 1200, 1200, true, 0));
        both.needs.push_back(need(NeedOrder::FoodWater, 800, 300, false, BREAD_ID));
        both.needs.push_back(need(NeedOrder::Training, 6000, 0, true, 0));
        std::vector<BotStateItem> const bothItems = make(both);
        bool same = bothItems.size() == both.needs.size();
        for (std::size_t index = 0; same && index < both.needs.size(); ++index)
            same = bothItems[index].name == UpkeepNeeds::OrderWord(both.needs[index].order) &&
                   bothItems[index].value == both.needs[index].copper &&
                   bothItems[index].extra == both.needs[index].funded &&
                   Is(bothItems[index], both.needs[index].due ? NeedRow::Due : NeedRow::Passing);
        Check(same, "needs: the topic's entries name the same needs as the command's list, in order");
    }

    // LCB1d: every row carries its word beside its code (Is() above also holds the item to RowWord of its row), the
    // words are the ones the module used to derive from the codes, and the bags rows the bridge has no phrase for
    // carry a sentence
    {
        auto const words = [](auto const& rows, std::initializer_list<char const*> expected)
        {
            std::size_t index = 0;
            bool same = rows.size() == expected.size();
            for (char const* word : expected)
                same = same && index < rows.size() && std::string(RowWord(rows[index++])) == word;
            return same;
        };
        Check(words(std::vector<PlanRow>{PlanRow::Goal, PlanRow::Source, PlanRow::Reason, PlanRow::Stop},
                    {"goal", "source", "reason", "stop"}) &&
                  words(std::vector<OptionRow>{OptionRow::Option, OptionRow::Current}, {"option", "current"}) &&
                  words(std::vector<RouteRow>{RouteRow::Total, RouteRow::Leg, RouteRow::Failed},
                        {"total", "leg", "failed"}) &&
                  words(std::vector<QuestRow>{QuestRow::Take, QuestRow::Task, QuestRow::TurnIn, QuestRow::Carried,
                                              QuestRow::Deferred},
                        {"take", "task", "turn_in", "carried", "deferred"}) &&
                  words(std::vector<BagRow>{BagRow::Free, BagRow::Sell, BagRow::Bag, BagRow::MailWaiting,
                                            BagRow::MailSeen, BagRow::MailExpected, BagRow::Kept, BagRow::BankFree},
                        {"free", "sell", "bag", "mail", "mail_seen", "mail_expected", "kept", "bank_free"}) &&
                  words(std::vector<PlaceRow>{PlaceRow::Hearth, PlaceRow::Flight, PlaceRow::FlightAway},
                        {"hearth", "flight_here", "flight_elsewhere"}),
              "words: each planner topic's row words as the module's table named them");
        Check(std::string(RecordWords::HistoryWord(0)) == "event" &&
                  std::string(RecordWords::HistoryWord(1)) == "zone" &&
                  std::string(RecordWords::HistoryWord(2)) == "dungeon" &&
                  std::string(RecordWords::HistoryWord(3)) == "boss" &&
                  std::string(RecordWords::DeedWord(0)) == "deed" && std::string(RecordWords::KnownWord(0)) == "bot",
              "words: history, lately and known rows");
        Check(std::string(RowWord(NeedRow::Provided)) == "provided" && std::string(RowWord(NeedRow::Due)) == "due" &&
                  std::string(RowWord(NeedRow::Passing)) == "passing" &&
                  std::string(RowWord(NeedRow::Saving)) == "saving",
              "words: needs rows");

        // A fixture with every optional row: each item of every old planner topic has a word of [a-z_]
        auto const wordy = [](std::vector<BotStateItem> const& list)
        {
            bool fine = !list.empty();
            for (BotStateItem const& item : list)
            {
                fine = fine && !item.word.empty() &&
                       item.word.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == std::string::npos;
            }
            return fine;
        };
        PlanInput in = InScope();
        SetGoal(in, Goals::GoalKind::Quest, 1, GoalApply::Source::Planner, "circuit");
        PlanItems(in, items);
        Check(wordy(items), "words: every plan row has a word");
        OptionsItems(in, items);
        Check(items.empty() || wordy(items), "words: every options row has a word");
        QuestPlanItems(in, items);
        Check(items.empty() || wordy(items), "words: every quest_plan row has a word");
        BagsInput bags;
        bags.freeSlots = 4;
        bags.slots = 52;
        bags.mailWaiting = 3;
        bags.mailSeenCopper = 5000;
        bags.mailExpectedCopper = 700;
        bags.keepsPossessions = true;
        bags.kept.push_back({"Linen Cloth", 20, true, false});
        bags.kept.push_back({"Healing Potion", 5, false, true});
        bags.bankFree = 20;
        bags.bankSlots = 28;
        BagsItems(bags, items);
        bool texts = wordy(items) && items.size() == 7;
        for (BotStateItem const& item : items)
        {
            bool const mailOrBank = Is(item, BagRow::MailWaiting) || Is(item, BagRow::MailSeen) ||
                                    Is(item, BagRow::MailExpected) || Is(item, BagRow::Kept) ||
                                    Is(item, BagRow::BankFree);
            texts = texts && (mailOrBank ? !item.text.empty() && item.text.size() <= 200 && !HasDigit(item.text)
                                         : item.text.empty());
        }
        Check(texts,
              "words: bags rows 3-7 carry a word and a sentence under 200 bytes with no number, rows 0-2 no text");
        Check(items.size() == 7 && items[4].text == "Healing Potion is carried for a later level." &&
                  items[5].text == "Linen Cloth waits in the bank for a later level.",
              "words: a kept row's sentence names the item and where it is");
        bags.kept.clear();
        bags.kept.push_back({std::string(300, 'x'), 20, true, false});
        BagsItems(bags, items);
        Check(items.size() >= 5 && items[4].text.size() <= 200,
              "words: a very long item name still fits the 200 bytes");
        PlacesInput places;
        places.hearthArea = "Goldshire";
        places.known.push_back({10, "Here", {0, 1.0f, 1.0f, 0.0f}});
        places.known.push_back({20, "There", {1, 1.0f, 1.0f, 0.0f}});
        PlacesItems(places, items);
        Check(
            wordy(items) && items.size() == 3 && items[1].word == "flight_here" && items[2].word == "flight_elsewhere",
            "words: places rows hearth, flight_here, flight_elsewhere");
        RouteInput route;
        route.known = false;
        RouteItems(route, items);
        Check(wordy(items) && items[0].word == "failed", "words: route rows");
    }

    // WI10b: the items topic words the usable items by kind, the cooldowns, the last uses and the lacks; a bot that is
    // not governed gets provided; no digit in any text; at most twelve entries; mutants of the guards
    {
        ItemsInput in;
        in.governed = true;
        in.carried = {{"heal", "Healing Potion", 3},
                      {"heal", "Major Healing Potion", 1},
                      {"mana", "Mana Potion", 2},
                      {"boost", "Elixir of 5 Agility", 1}};
        in.cooling = {{"Healing Potion", 40}, {"Mana Potion", 12}, {"Elixir of Agility", 5}};
        in.used = {{"Healing Potion", "used", 30}, {"Mana Potion", "refused:not_ready", 90}};
        in.lacks = {{"heal", 4}, {"food", 2}, {"flask", 1}};
        std::vector<BotStateItem> items;
        ItemsItems(in, items);
        auto const count = [](std::vector<BotStateItem> const& list, ItemsRow row)
        {
            std::size_t found = 0;
            for (BotStateItem const& item : list)
                found += item.code == static_cast<std::uint8_t>(row);
            return found;
        };
        Check(count(items, ItemsRow::Carried) == 3 && items[0].name == "heal" && items[0].value == 2 &&
                  items[0].extra == 4 &&
                  items[0].text.find("Healing Potion, Major Healing Potion") != std::string::npos,
              "items: one entry per kind with its distinct items and units, the names listed");
        Check(count(items, ItemsRow::Cooling) == ITEMS_COOLING_SHOWN && items[3].name == "Healing Potion" &&
                  items[3].value == 40,
              "items: the cooldowns are cut at two");
        Check(count(items, ItemsRow::Used) == 2 && items[5].text.find("it worked") != std::string::npos &&
                  items[6].text.find("I could not: not ready") != std::string::npos,
              "items: the last uses say how each went");
        Check(count(items, ItemsRow::Lacks) == ITEMS_LACKS_SHOWN && count(items, ItemsRow::Provided) == 0 &&
                  items.back().name == "food",
              "items: the lacks are listed in slot order and cut at two");
        bool digits = false;
        for (BotStateItem const& item : items)
            digits = digits || item.text.find_first_of("0123456789") != std::string::npos ||
                     item.name.find_first_of("0123456789") != std::string::npos;
        Check(!digits, "items: no digit in any name or text (an item name with a number loses it)");
        Check(items.size() <= Limit(BotStateTopic::Items) && Limit(BotStateTopic::Items) == 12,
              "items: within the topic's limit of twelve");

        ItemsInput bare = in;
        bare.governed = false;
        std::vector<BotStateItem> bareItems;
        ItemsItems(bare, bareItems);
        Check(count(bareItems, ItemsRow::Provided) == 1 && count(bareItems, ItemsRow::Lacks) == 0 &&
                  bareItems.back().name == "provided" && count(items, ItemsRow::Provided) == 0,
              "items: a bot that is not governed answers provided and no lacks (mutant: governed lists them)");

        ItemsInput many;
        many.governed = true;
        for (int kind = 0; kind < 9; ++kind)
            many.carried.push_back({std::string("kind") + static_cast<char>('a' + kind), "Thing", 1});
        for (int at = 0; at < 6; ++at)
        {
            many.cooling.push_back({"Thing", 5});
            many.used.push_back({"Thing", "used", 5});
            many.lacks.push_back({"heal", 1});
        }
        std::vector<BotStateItem> manyItems;
        ItemsItems(many, manyItems);
        Check(manyItems.size() == ITEMS_CARRIED_SHOWN + ITEMS_COOLING_SHOWN + ITEMS_USED_SHOWN + ITEMS_LACKS_SHOWN &&
                  manyItems.size() == Limit(BotStateTopic::Items),
              "items: more of every part than its share are cut to the topic's twelve");

        ItemsInput nothing;
        nothing.governed = true;
        std::vector<BotStateItem> nothingItems;
        ItemsItems(nothing, nothingItems);
        Check(nothingItems.empty(), "items: a governed bot with nothing to say answers no entries");
        bool worded = !items.empty() && !bareItems.empty();
        for (BotStateItem const& item : items)
            worded = worded && item.word == RowWord(static_cast<ItemsRow>(item.code));
        Check(worded && bareItems.back().word == "provided", "items: every row carries its row word");
    }

    // WR18: the professions topic words a crafter, a gatherer, a bot without professions and one the rules do not
    // govern; the cut keeps the lacks row; no digit reaches a row (the mutant with a number is caught)
    {
        auto const hasDigit = [](std::vector<BotStateItem> const& rows)
        {
            for (BotStateItem const& row : rows)
                for (char const letter : row.text + row.name + row.word)
                    if (letter >= '0' && letter <= '9')
                        return true;
            return false;
        };
        ProfessionsInput crafter;
        crafter.governed = true;
        crafter.lines.push_back({"Blacksmithing", "journeyman", ProfessionGoal::Recipe, true});
        crafter.lines.push_back({"Mining", "apprentice", ProfessionGoal::None, false});
        crafter.lacks = {ProfessionLack::Recipe, ProfessionLack::Materials};
        std::vector<BotStateItem> rows;
        ProfessionsItems(crafter, rows);
        Check(rows.size() == 3 && Is(rows[0], ProfessionRow::Profession) && Is(rows[2], ProfessionRow::Lacks) &&
                  rows[0].name == "Blacksmithing" && rows[0].text.find("journeyman") != std::string::npos &&
                  rows[0].text.find("making things") != std::string::npos && !hasDigit(rows),
              "professions: a crafter's lines with its rank and goal, then what it lacks, in words");
        Check(rows[1].text.find("nothing more in view") != std::string::npos &&
                  rows[2].text == "You still lack a recipe, materials.",
              "professions: a gatherer's line without a goal, the lacks joined");

        ProfessionsInput gatherer;
        gatherer.governed = true;
        gatherer.lines.push_back({"Herbalism", "expert", ProfessionGoal::Node, false});
        ProfessionsItems(gatherer, rows);
        Check(rows.size() == 1 && rows[0].text.find("working towards a kind of node") != std::string::npos,
              "professions: a gatherer working towards a node kind, no lacks row without a lack");

        ProfessionsInput none;
        none.governed = true;
        ProfessionsItems(none, rows);
        Check(rows.size() == 1 && Is(rows[0], ProfessionRow::Nothing) && !rows[0].text.empty(),
              "professions: a bot without professions answers one defined row");
        ProfessionsInput provided = crafter;
        provided.governed = false;
        ProfessionsItems(provided, rows);
        Check(rows.size() == 1 && Is(rows[0], ProfessionRow::Provided), "professions: an ungoverned bot: provided");

        ProfessionsInput many = crafter;
        for (int at = 0; at < 12; ++at)
            many.lines.push_back({"Fishing", "master", ProfessionGoal::Rank, false});
        ProfessionsItems(many, rows);
        Check(rows.size() == Limit(BotStateTopic::Professions) && Is(rows.back(), ProfessionRow::Lacks),
              "professions: more lines than the cap are cut, the lacks row kept");

        bool worded = true;
        for (BotStateItem const& row : rows)
            worded = worded && row.word == RowWord(static_cast<ProfessionRow>(row.code));
        Check(worded, "professions: every row carries its row word");

        ProfessionsInput mutant = crafter;
        mutant.lines[0].rank = "75";
        ProfessionsItems(mutant, rows);
        Check(hasDigit(rows), "professions: mutant, a skill number in a row, is seen by the digit guard");
        static_assert(static_cast<std::size_t>(BotStateTopic::Professions) + 1 == std::size(TopicTable));
    }

    std::printf("bridge-bot-topics selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
