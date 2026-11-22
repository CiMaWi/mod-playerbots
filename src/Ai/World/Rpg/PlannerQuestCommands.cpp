/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The quest index's GM commands and harnesses, apart from its server side in PlannerQuests.cpp (CS37a).
// The questing circuit's job wrapper, its real-place harness and `planner circuit` (WT13b; the planner: QuestCircuit.h)

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <memory>
#include <sstream>

#include "Chat.h"
#include "GoalModel.h"
#include "GoalModelHarness.h"
#include "GoalModelLog.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlannerIndexes.h"
#include "PlannerQuestInternal.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestCircuit.h"
#include "QuestCircuitHarness.h"
#include "QuestCircuitLog.h"
#include "QuestDef.h"
#include "QuestIndex.h"
#include "StringFormat.h"
#include "WorldLog.h"

namespace Planner
{
namespace
{
// The circuit harness's real places (WT13b): at fixed map coordinates among the quest givers, checked against the
// quest index when the harness runs (the givers within the hub's reach, the nearest camp)
struct CircuitPlace
{
    char const* name;
    uint8 level;
    uint8 race;
    uint8 cls;
    uint8 team;  // 0 Alliance, 1 Horde
    uint32 mapId;
    float x;
    float y;
    float z;
};

// Goldshire's quests are levels 7-10: at level 12 they are grey (grey difference 4) and only deliveries are left, so
// the human is 9 (the coordinator's call after the WT13b review)
constexpr std::array<CircuitPlace, 3> CIRCUIT_PLACES{{
    {"goldshire-9-human", 9, RACE_HUMAN, CLASS_WARRIOR, 0, 0, -9465.0f, 40.0f, 57.0f},
    {"crossroads-22-orc", 22, RACE_ORC, CLASS_WARRIOR, 1, 1, -441.0f, -2596.0f, 92.0f},
    {"gromgol-35-troll", 35, RACE_TROLL, CLASS_HUNTER, 1, 0, -12370.0f, 175.0f, 4.0f},
}};
constexpr uint32 CIRCUIT_TIMING_RUNS = 50;      // plans per real scenario behind the job time's p50/p95
constexpr float CIRCUIT_CAMP_YARDS = 300.0f;    // the nearest quest camp to a place is looked for this far
constexpr std::size_t CIRCUIT_CAMPS_TRIED = 8;  // camps tried for one with quests, nearest first
constexpr uint16 CIRCUIT_BAG_SLOTS = 16;        // the synthetic bot's free bag slots

Snapshot CircuitBot(CircuitPlace const& place, Place const& start)
{
    Snapshot bot;
    bot.level = place.level;
    bot.race = place.race;
    bot.cls = place.cls;
    bot.team = place.team;
    bot.position = start;
    bot.runSpeed = Circuit::DEFAULT_RUN_SPEED;
    bot.bagSlots = CIRCUIT_BAG_SLOTS;
    bot.bagFree = CIRCUIT_BAG_SLOTS;
    return bot;
}

// The place's start: its own position when a quest giver stands within the hub's reach, else the nearest camp that
// gives quests. False when neither has a giver within the hub's reach
bool CircuitStart(QuestIndex const& quests, CircuitPlace const& place, Place& start, std::string& text)
{
    start = {place.mapId, place.x, place.y, place.z};
    std::size_t givers = quests.GiversNear(place.mapId, place.x, place.y, Circuit::HUB_GIVER_YARDS).size();
    std::string where = Acore::StringFormat("({:.0f}, {:.0f}) on map {}", place.x, place.y, place.mapId);
    std::string camp = Acore::StringFormat("no camp with quests within {:.0f} yd", CIRCUIT_CAMP_YARDS);
    for (Nearby const& nearby :
         quests.NearestCamps(place.mapId, place.x, place.y, place.z, CIRCUIT_CAMPS_TRIED, CIRCUIT_CAMP_YARDS))
    {
        QuestCamp const& found = quests.camps[nearby.index];
        if (found.quests.empty())
            continue;
        camp =
            Acore::StringFormat("camp {} ({} quests, {} places, radius {:.0f} yd, zone {}) at {:.0f} yd", nearby.index,
                                found.quests.size(), found.places.size(), found.radius, found.zoneId, nearby.distance);
        if (!givers)
        {
            start = {place.mapId, found.x, found.y, found.z};
            givers = quests.GiversNear(place.mapId, found.x, found.y, Circuit::HUB_GIVER_YARDS).size();
            where = Acore::StringFormat("the camp's centre ({:.0f}, {:.0f})", found.x, found.y);
        }
        break;
    }
    text = Acore::StringFormat("{} quest givers within {:.0f} yd of {}; nearest {}", givers, Circuit::HUB_GIVER_YARDS,
                               where, camp);
    return givers > 0;
}

// The synthetic bot's history: every quest grey for its level, as a bot that levelled through the world has done
// (prerequisites of the hub's chains met; the grey ones skipped either way), ascending as the index
std::vector<uint32> GreyHistory(QuestIndex const& quests, uint8 level, uint32 greyDiff)
{
    std::vector<uint32> ids;
    for (QuestRecord const& quest : quests.quests)
        if (quest.level > 0 &&
            static_cast<int32>(level) > Circuit::QuestLevel(quest, level) + static_cast<int32>(greyDiff))
            ids.push_back(quest.id);
    return ids;
}

// Every quest the givers within the hub's reach offer, sorted: rewarded, they make the hub dry
std::vector<uint32> HubQuests(QuestIndex const& quests, Place const& at)
{
    std::vector<uint32> ids;
    for (uint32 const place : quests.GiversNear(at.map, at.x, at.y, Circuit::HUB_GIVER_YARDS))
        for (uint32 const id : quests.QuestsGivenAt(place))
            ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Log quests for the full-log fixture, ascending: every objective on another map than the place's (work the bot
// cannot do from the hub), grey for its level or not, never a repeatable, elite, group, instance or exclusive quest,
// and none that a hub quest names (prerequisite, breadcrumb), so the hub offers what it offers to an empty log
std::vector<uint32> FarQuests(QuestIndex const& quests, CircuitPlace const& place, std::vector<uint32> const& hub,
                              uint32 greyDiff, bool grey, std::size_t count)
{
    std::vector<uint32> named;
    for (uint32 const id : hub)
        if (QuestRecord const* quest = quests.Find(id))
        {
            named.push_back(static_cast<uint32>(quest->prev < 0 ? -quest->prev : quest->prev));
            named.push_back(quest->breadcrumbFor);
        }
    std::vector<uint32> ids;
    for (QuestRecord const& quest : quests.quests)
    {
        if (ids.size() >= count)
            break;
        bool const isGrey =
            static_cast<int32>(place.level) > Circuit::QuestLevel(quest, place.level) + static_cast<int32>(greyDiff);
        bool const above =
            Circuit::QuestLevel(quest, place.level) > static_cast<int32>(place.level) + Circuit::MAX_LEVELS_ABOVE;
        if (quest.level <= 0 || isGrey != grey || above || quest.exclusiveGroup ||
            (quest.flags & (Circuit::REPEAT_FLAGS | Circuit::GROUP_FLAGS | Circuit::INSTANCE_FLAGS)) ||
            std::binary_search(hub.begin(), hub.end(), quest.id) ||
            std::find(named.begin(), named.end(), quest.id) != named.end())
            continue;
        bool placed = false;
        bool elsewhere = true;
        for (QuestObjective const& objective : quest.objectives)
            for (AreaPoint const& area : objective.areas)
            {
                placed = placed || objective.kind != ObjectiveKind::Turnin;
                elsewhere = elsewhere && area.mapId != place.mapId;
            }
        if (placed && elsewhere)
            ids.push_back(quest.id);
    }
    return ids;
}

QuestSlot LogSlot(uint32 quest, uint32 state)
{
    QuestSlot slot;
    slot.quest = quest;
    slot.state = state;
    return slot;
}

void AddChecks(std::vector<CircuitHarness::Check>& checks, std::vector<CircuitHarness::Check> const& more)
{
    checks.insert(checks.end(), more.begin(), more.end());
}

void AddPlanLines(std::vector<std::string>& lines, std::string const& name, CircuitHarness::Report const& report)
{
    lines.push_back(Acore::StringFormat(
        "{} (hub camp {}):", name,
        report.plan.homeCamp == NO_CAMP ? std::string("none") : std::to_string(report.plan.homeCamp)));
    for (std::string const& line : CircuitHarness::PlanLines(report.plan, report.baselineYards))
        lines.push_back("  " + line);
}
}  // namespace

bool RunCircuitHarness(ChatHandler* handler)
{
    if (!IndexesReady() || !QuestsReady())
    {
        handler->SendErrorMessage("PLANNER circuit selftest: not ready (see `playerbots planner indexes`)");
        return false;
    }
    auto const started = std::chrono::steady_clock::now();
    CircuitData const data = CircuitDataNow();
    CircuitContext const context = data.Context();
    QuestIndex const& quests = *data.quests;
    std::vector<CircuitHarness::Check> checks;
    std::vector<std::string> plans;
    for (CircuitPlace const& place : CIRCUIT_PLACES)
    {
        Place start;
        std::string text;
        bool const found = CircuitStart(quests, place, start, text);
        checks.push_back({found, std::string(place.name) + ": " + text});
        if (!found)
            continue;

        // An empty log at the hub after a levelled bot's history: most fitting quests taken, only fitting ones, the log
        // within its limit, one turn-in batch
        CircuitHarness::Scenario scenario{place.name, CircuitBot(place, start), context, {}};
        scenario.bot.rewardedQuests = GreyHistory(quests, place.level, data.greyDiff);
        CircuitHarness::Report const report = CircuitHarness::RunScenario(scenario, CIRCUIT_TIMING_RUNS);
        AddChecks(checks, report.checks);
        AddPlanLines(plans, scenario.name, report);
        if (&place == &CIRCUIT_PLACES.front())
        {
            AddChecks(checks, CircuitHarness::GuardChecks(scenario));
        }
        std::vector<uint32> const hub = HubQuests(quests, start);

        // The next circuit with the carried quests done in the log (review 2, finding A): they stay out of the loop,
        // which visits the same ender camps
        if (!report.plan.carried.empty())
        {
            CircuitHarness::Scenario carrying{scenario.name + "-carrying", scenario.bot, context, {}};
            for (uint32 const id : report.plan.carried)
                carrying.bot.quests.push_back(LogSlot(id, Circuit::SLOT_COMPLETE));
            carrying.expect.minTake = 0;
            carrying.expect.carried = report.plan.carried;
            carrying.expect.checkEnderCamps = true;
            carrying.expect.enderCamps = report.plan.enderCamps;
            CircuitHarness::Report const carryingReport = CircuitHarness::RunScenario(carrying, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, carryingReport.checks);
            AddPlanLines(plans, carrying.name, carryingReport);
        }

        // A full log of quests from another map and a grey one: the grey one goes, far ones make room for every quest
        // the empty log took to work here (a delivery only when worth more than the far quests it pushes out, so it
        // may wait for log space), a protected far one stays
        std::vector<uint32> const fillers = FarQuests(quests, place, hub, data.greyDiff, false, QUEST_LOG_SLOTS - 1);
        std::vector<uint32> const grey = FarQuests(quests, place, hub, data.greyDiff, true, 1);
        bool const fixture = fillers.size() == QUEST_LOG_SLOTS - 1 && grey.size() == 1;
        checks.push_back({fixture, Acore::StringFormat("{}-full-log: {} far quests and {} grey one fill the log",
                                                       scenario.name, fillers.size(), grey.size())});
        if (fixture)
        {
            CircuitHarness::Scenario full{scenario.name + "-full-log", scenario.bot, context, {}};
            std::erase(full.bot.rewardedQuests, grey.front());
            for (uint32 const id : fillers)
                full.bot.quests.push_back(LogSlot(id, 0));
            full.bot.quests.push_back(LogSlot(grey.front(), 0));
            full.context.protectedQuests = {fillers.front()};
            std::vector<uint32> worked;  // the empty log's takes worked here
            for (CircuitTake const& take : report.plan.take)
                if (!take.carried)
                    worked.push_back(take.quest);
            std::sort(worked.begin(), worked.end());
            full.expect.minTake = worked.size();
            full.expect.checkAbandon = true;
            full.expect.abandonFrom.assign(fillers.begin() + 1, fillers.end());
            full.expect.abandonFrom.push_back(grey.front());
            full.expect.abandonMust = grey;
            full.expect.keep = {fillers.front()};
            CircuitHarness::Report const fullReport = CircuitHarness::RunScenario(full, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, fullReport.checks);
            std::vector<uint32> const was = CircuitHarness::TakenIds(report.plan);
            std::vector<uint32> const fullTaken = CircuitHarness::TakenIds(fullReport.plan);
            bool const keepsWorked = std::includes(fullTaken.begin(), fullTaken.end(), worked.begin(), worked.end());
            bool const onlyEmpty = std::includes(was.begin(), was.end(), fullTaken.begin(), fullTaken.end());
            checks.push_back({keepsWorked && onlyEmpty,
                              Acore::StringFormat("{}-full-log: takes {}: each the empty log works here, none it left",
                                                  scenario.name, CircuitHarness::IdList(fullTaken))});
            AddPlanLines(plans, full.name, fullReport);

            // The log full of those quests done (review 3, finding 1): complete quests are never abandoned and none is
            // turned in here, so the hub's quests wait for log space that only delivering frees. No slot frees at a
            // chain or camp, so the bot moves on only to deliver (reviews 4 and 5); with every ender on another map no
            // delivery is in reach, so it finds nowhere. The generic checks hold it to that and follow its moves
            CircuitHarness::Scenario done{scenario.name + "-full-log-done", scenario.bot, context, {}};
            for (std::size_t i = 0; i + Circuit::LOG_RESERVE < QUEST_LOG_SLOTS; ++i)
                done.bot.quests.push_back(LogSlot(fillers[i], Circuit::SLOT_COMPLETE));
            done.expect.exactTake = true;
            done.expect.minTake = 0;
            CircuitHarness::ExactAbandon(done.expect, {});
            CircuitHarness::Report const doneReport = CircuitHarness::RunScenario(done, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, doneReport.checks);
            AddPlanLines(plans, done.name, doneReport);
        }

        // The same hub with every quest its givers offer rewarded: the bot moves on
        CircuitHarness::Scenario dry{scenario.name + "-dry", scenario.bot, context, {}};
        std::vector<uint32> done;
        std::set_union(scenario.bot.rewardedQuests.begin(), scenario.bot.rewardedQuests.end(), hub.begin(), hub.end(),
                       std::back_inserter(done));
        dry.bot.rewardedQuests = std::move(done);
        dry.expect.minTake = 0;
        dry.expect.moveInBracket = true;
        CircuitHarness::Report const dryReport = CircuitHarness::RunScenario(dry, CIRCUIT_TIMING_RUNS);
        AddChecks(checks, dryReport.checks);
        AddPlanLines(plans, dry.name, dryReport);

        // The dry hub with the log full of the far quests, none protected (review 5, finding 1): any hub that wants a
        // slot gives one of them up, so the bot still moves on, and the generic checks see it take there
        if (fixture)
        {
            CircuitHarness::Scenario dryFull{scenario.name + "-dry-full-log", dry.bot, context, {}};
            for (uint32 const id : fillers)
            {
                std::erase(dryFull.bot.rewardedQuests, id);
                dryFull.bot.quests.push_back(LogSlot(id, 0));
            }
            dryFull.expect.minTake = 0;
            dryFull.expect.moveInBracket = true;
            CircuitHarness::ExactAbandon(dryFull.expect, {});
            CircuitHarness::Report const dryFullReport = CircuitHarness::RunScenario(dryFull, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, dryFullReport.checks);
            AddPlanLines(plans, dryFull.name, dryFullReport);
        }
    }

    std::size_t failed = 0;
    for (CircuitHarness::Check const& check : checks)
    {
        failed += check.pass ? 0 : 1;
        handler->PSendSysMessage("{} {}", check.pass ? "PASS" : "FAIL", check.text);
    }
    for (std::string const& line : plans)
        handler->PSendSysMessage("{}", line);
    std::string const summary =
        Acore::StringFormat("PLANNER circuit selftest: {}, {} checks, {} failed, in {} ms (grey difference {})",
                            failed ? "FAIL" : "PASS", checks.size(), failed, MsSince(started), data.greyDiff);
    handler->PSendSysMessage("{}", summary);
    LOG_INFO("playerbots", "{}", summary);
    return !failed;
}

bool HandleCircuitCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string word;
    in >> word;
    if (word.empty())
    {
        handler->SendErrorMessage("Usage: playerbots planner circuit selftest | <player name>");
        return false;
    }
    // The summary line carries the verdict; false here would only add the usage text
    if (word == "selftest")
    {
        RunCircuitHarness(handler);
        return true;
    }
    if (!IndexesReady() || !QuestsReady())
    {
        handler->SendErrorMessage("PLANNER circuit: not ready (see `playerbots planner indexes`)");
        return false;
    }
    std::string name = word;
    Player* player = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    if (!player || !player->IsInWorld())
    {
        handler->SendErrorMessage("PLANNER circuit: no player {} online", word);
        return false;
    }
    // Read only, on the world thread between map updates; the log quests' elapsed minutes come with WT16
    CircuitData const data = CircuitDataNow();
    Snapshot const snapshot = FillSnapshot(player, 0);
    CircuitPlan const plan = PlanCircuit(snapshot, data.Context());
    // The world log's decision rows of this plan (kind circuit); WT16 emits the same rows where it applies plans
    if (WorldLog::Active())
        for (CircuitLog::Row const& row : CircuitLog::Rows(plan))
            WorldLog::Decision(player, WorldLogFormat::DecisionKind::Circuit, row.choice, row.reason, row.value);
    handler->PSendSysMessage("CIRCUIT {}: level {}, map {} ({:.0f}, {:.0f}), {} log quests, hub camp {}",
                             player->GetName(), snapshot.level, snapshot.position.map, snapshot.position.x,
                             snapshot.position.y, snapshot.quests.size(),
                             plan.homeCamp == NO_CAMP ? std::string("none") : std::to_string(plan.homeCamp));
    float const baseline = plan.ok ? BaselineYards(plan, *data.quests) : 0.0f;
    for (std::string const& line : CircuitHarness::PlanLines(plan, baseline))
        handler->PSendSysMessage("  {}", line);
    return true;
}

// WT14-2: the goal model on real data. The circuit, hubs, quests and services are the real indexes at the circuit
// harness's places; the grind spots and the herb node are synthetic (the world has no spot or node index yet, WT16)
namespace
{
constexpr float GOALS_SPOT_XP = 60.0f;  // synthetic grind spots: XP and copper per kill
constexpr float GOALS_SPOT_COPPER = 30.0f;
constexpr float GOALS_NODE_COPPER = 400.0f;  // synthetic herb node: what its yield sells for
constexpr uint32 GOALS_SPOT_ID = 9001;
constexpr uint32 GOALS_SPOT_GREY_ID = 9002;
constexpr uint32 GOALS_NODE_ID = 9101;
constexpr uint32 GOALS_ZONE_SPAN = 5;  // the synthetic zone bracket around the bot's level
constexpr uint32 GOALS_FLUSH = 1000000;

Goals::Situation GoalsSituation(Snapshot const& bot, Place const& start)
{
    Goals::Situation situation = GoalHarness::Base(bot.level);
    situation.here.zoneLow = bot.level > GOALS_ZONE_SPAN ? bot.level - GOALS_ZONE_SPAN : 1;
    situation.here.zoneHigh = bot.level + GOALS_ZONE_SPAN;
    uint32 const level = bot.level;
    Goals::GrindSpot spot =
        GoalHarness::Spot(GOALS_SPOT_ID, GOALS_SPOT_XP, GOALS_SPOT_COPPER, level > 1 ? level - 1 : 1, level + 1);
    spot.place = start;
    Goals::GrindSpot grey = spot;
    grey.id = GOALS_SPOT_GREY_ID;
    grey.minLevel = 1;
    grey.maxLevel = std::max(Goals::GreyLevel(level), 1u);
    situation.spots = {spot, grey};
    Goals::GatherNode node;
    node.id = GOALS_NODE_ID;
    node.skill = GoalHarness::HERBALISM;
    node.place = start;
    node.walkMinutes = 1.0f;
    node.copperWorth = GOALS_NODE_COPPER;
    situation.nodes = {node};
    return situation;
}

std::string GoalsLine(Goals::Ranked const& ranked)
{
    return Acore::StringFormat("  ranked: {}", GoalHarness::Summary(ranked));
}

// A real option names a hub, camp, spot, node, quest, vendor, item or inn, never the no-hub or no-camp marker, and a
// move to a camp leads there, not back to where the circuit ends
bool GoalTargetsOk(Goals::GoalPlan const& plan)
{
    for (Goals::Option const& option : plan.ranked.options)
    {
        if (option.target == NO_HUB || option.target == NO_CAMP)
            return false;
        if (option.targetType == Goals::TargetType::Camp && option.place.x == plan.circuit.returnTo.x &&
            option.place.y == plan.circuit.returnTo.y)
            return false;
    }
    return true;
}
}  // namespace

bool RunGoalsHarness(ChatHandler* handler)
{
    auto const started = std::chrono::steady_clock::now();
    std::vector<GoalHarness::Check> checks = GoalHarness::SyntheticChecks();
    std::size_t const synthetic = checks.size();
    std::vector<std::string> lines;
    if (!IndexesReady() || !QuestsReady())
        checks.push_back({"real: data", false, "indexes or quests not ready (see `playerbots planner indexes`)"});
    else
    {
        CircuitData const data = CircuitDataNow();
        CircuitContext const context = data.Context();
        for (CircuitPlace const& place : CIRCUIT_PLACES)
        {
            Place start;
            std::string text;
            bool const found = CircuitStart(*data.quests, place, start, text);
            std::string const name = std::string("real ") + place.name;
            if (!found)
            {
                checks.push_back({name + ": start", false, text});
                continue;
            }
            Snapshot bot = CircuitBot(place, start);
            bot.rewardedQuests = GreyHistory(*data.quests, place.level, data.greyDiff);
            bot.money = GOALS_FLUSH;
            Goals::GoalPlan const flush = Goals::PlanGoals(bot, context, GoalsSituation(bot, start));
            Goals::GoalPlan const again = Goals::PlanGoals(bot, context, GoalsSituation(bot, start));
            lines.push_back(name + " flush");
            lines.push_back(GoalsLine(flush.ranked));
            checks.push_back(
                {name + ": a flush bot's ranking is made, with the circuit among its options",
                 flush.ranked.ok && (!flush.circuit.ok || GoalHarness::Has(flush.ranked, Goals::GoalKind::Quest)),
                 GoalHarness::Summary(flush.ranked)});
            checks.push_back({name + ": a flush bot does not choose money-making",
                              flush.ranked.ok && !flush.ranked.options.empty() &&
                                  flush.ranked.options.front().kind != Goals::GoalKind::Money,
                              GoalHarness::Summary(flush.ranked)});
            checks.push_back({name + ": the same input gives the same ranking",
                              GoalHarness::SameList(flush.ranked, again.ranked), "determinism"});
            bot.money = 0;
            Goals::GoalPlan const broke = Goals::PlanGoals(bot, context, GoalsSituation(bot, start));
            lines.push_back(name + " broke");
            lines.push_back(GoalsLine(broke.ranked));
            checks.push_back({name + ": a broke bot weighs money-making",
                              broke.ranked.ok && GoalHarness::Has(broke.ranked, Goals::GoalKind::Money),
                              GoalHarness::Summary(broke.ranked)});
            std::vector<GoalLog::Row> const rows = GoalLog::Rows(broke.ranked);
            bool const rowOk = rows.size() == 1 && !broke.ranked.options.empty() &&
                               rows[0].choice == Goals::ChoiceWord(broke.ranked.options.front());
            checks.push_back({name + ": the ranking maps to one goal row", rowOk,
                              rows.empty() ? std::string("no row") : rows[0].choice + " " + rows[0].reason});
            // A dry hub: every quest of the hub done; the moves are the circuit harness's proof, here the ranking
            Snapshot dry = bot;
            std::vector<uint32> const hub = HubQuests(*data.quests, start);
            std::vector<uint32> done;
            std::set_union(dry.rewardedQuests.begin(), dry.rewardedQuests.end(), hub.begin(), hub.end(),
                           std::back_inserter(done));
            dry.rewardedQuests = std::move(done);
            dry.money = GOALS_FLUSH;
            Goals::GoalPlan const dryPlan = Goals::PlanGoals(dry, context, GoalsSituation(dry, start));
            lines.push_back(name + " dry hub");
            lines.push_back(GoalsLine(dryPlan.ranked));
            checks.push_back({name + ": a dry hub still ranks goals",
                              dryPlan.ranked.ok && !dryPlan.ranked.options.empty(),
                              GoalHarness::Summary(dryPlan.ranked)});
            checks.push_back({name + ": every option names a real target",
                              GoalTargetsOk(flush) && GoalTargetsOk(broke) && GoalTargetsOk(dryPlan),
                              GoalHarness::Summary(dryPlan.ranked)});
        }
    }
    std::size_t failed = 0;
    for (GoalHarness::Check const& check : checks)
    {
        failed += check.pass ? 0 : 1;
        handler->PSendSysMessage("{} {}: {}", check.pass ? "PASS" : "FAIL", check.name, check.text);
    }
    for (std::string const& line : lines)
        handler->PSendSysMessage("{}", line);
    std::string const summary = Acore::StringFormat(
        "PLANNER goals selftest: {}, {} checks ({} synthetic), {} failed, in {} ms, rank {:.1f} us",
        failed ? "FAIL" : "PASS", checks.size(), synthetic, failed, MsSince(started), GoalHarness::RankMicroseconds());
    handler->PSendSysMessage("{}", summary);
    LOG_INFO("playerbots", "{}", summary);
    return !failed;
}

bool HandleGoalsCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string word;
    in >> word;
    if (word.empty())
    {
        handler->SendErrorMessage("Usage: playerbots planner goals selftest | <player name>");
        return false;
    }
    if (word == "selftest")
    {
        RunGoalsHarness(handler);
        return true;
    }
    if (!IndexesReady() || !QuestsReady())
    {
        handler->SendErrorMessage("PLANNER goals: not ready (see `playerbots planner indexes`)");
        return false;
    }
    std::string name = word;
    Player* player = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    if (!player || !player->IsInWorld())
    {
        handler->SendErrorMessage("PLANNER goals: no player {} online", word);
        return false;
    }
    // Read only, on the world thread: the circuit and the Situation inputs the world gives now; a bot's are the live
    // job's (vendor, repairer, bag offers: LiveSituation), a player's only health and mana. A caller offers a hearth
    // option only when WorldTravel::InnCheckDue says so; no inns are given here, so none is offered
    CircuitData const data = CircuitDataNow();
    Snapshot const snapshot = FillSnapshot(player, 0);
    Goals::Situation situation;
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        situation = LiveSituation(ai, player);
    else
    {
        situation.here.hub = NO_HUB;  // as the live job: found from the circuit
        situation.healthPercent = player->GetHealthPct();
        uint32 const maxMana = player->GetMaxPower(POWER_MANA);
        situation.manaPercent =
            maxMana > 0 ? 100.0f * static_cast<float>(player->GetPower(POWER_MANA)) / static_cast<float>(maxMana)
                        : -1.0f;
    }
    // One line naming the bot (a scenario's log keeps only those): the errand inputs the model weighs
    handler->PSendSysMessage(
        "GOALS {} inputs: vendor {} {:.1f}, repairer {} {:.1f}, sellable {}, offers {}, durability {}, free {}/{}, "
        "inns {} first {}, hearthstone {}",
        player->GetName(), situation.bags.vendor.id, situation.bags.vendor.walkMinutes, situation.bags.repairer.id,
        situation.bags.repairer.walkMinutes, situation.bags.sellableSlots, situation.bagOffers.size(),
        snapshot.durabilityPercent, snapshot.bagFree, snapshot.bagSlots, situation.inns.size(),
        situation.inns.empty() ? 0 : situation.inns.front().id, snapshot.hasHearthstone);
    Goals::GoalPlan const plan = Goals::PlanGoals(snapshot, data.Context(), std::move(situation));
    if (WorldLog::Active())
        for (GoalLog::Row const& row : GoalLog::Rows(plan.ranked))
        {
            WorldLogFormat::Alternatives alternatives;
            for (GoalLog::Alt const& alt : row.alternatives)
                alternatives.Add({alt.choice, alt.value});
            WorldLog::Decision(player, WorldLogFormat::DecisionKind::Goal, row.choice, row.reason, row.value,
                               alternatives);
        }
    handler->PSendSysMessage("GOALS {}: level {}, map {} ({:.0f}, {:.0f}), money {}, need factor {:.2f}, {} considered",
                             player->GetName(), snapshot.level, snapshot.position.map, snapshot.position.x,
                             snapshot.position.y, snapshot.money, plan.ranked.need.factor, plan.ranked.considered);
    if (!plan.ranked.ok)
        handler->PSendSysMessage("  no ranking: {}", plan.ranked.failure);
    for (Goals::Option const& option : plan.ranked.options)
        handler->PSendSysMessage("  {} {} {:.1f}/min over {:.1f} min: {}", player->GetName(), Goals::ChoiceWord(option),
                                 option.valuePerMinute, option.minutes, option.reason);
    return true;
}
}  // namespace Planner
