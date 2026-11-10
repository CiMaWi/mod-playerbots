/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTCIRCUITHARNESS_H
#define _PLAYERBOT_QUESTCIRCUITHARNESS_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "QuestCircuit.h"
#include "QuestCircuitLog.h"

// The circuit planner's scenario runner (travel plan WT13b): one line per check. Shared by the offline selftest
// (tests/quest-circuit-selftest.cpp, synthetic worlds with exact expectations) and `playerbots planner circuit
// selftest` (the real indexes, looser checks). Proof is functional (WT.21): the checks show the decisions; the yards
// against today's pick are printed as information. Player-free
namespace Planner
{
namespace CircuitHarness
{
constexpr double GUARD_MICROSECONDS = 1000;  // a rejected bad position answers within this
constexpr std::size_t FOLLOW_HOPS = 6;       // moves followed from hub to hub when looking for a circle

struct Expect
{
    bool exactTake{false};
    std::vector<std::uint32_t> take;     // the exact set when exactTake
    std::vector<std::uint32_t> notTake;  // never taken
    std::size_t minTake{1};
    bool checkAbandon{false};
    std::vector<std::uint32_t> abandonFrom;  // when checkAbandon: every abandoned quest is one of these
    std::vector<std::uint32_t> abandonMust;  // abandoned
    std::vector<std::uint32_t> keep;         // never abandoned
    bool checkDeferred{false};
    std::vector<std::uint32_t> deferred;               // the exact set left for a later trip when checkDeferred
    std::vector<std::vector<std::uint32_t>> together;  // each: done at one stop of this trip, turned in in one visit
    std::vector<std::uint32_t> carried;                // carried to another hub, not turned in on this loop
    std::vector<std::uint32_t> turnIns;                // turned in on this loop
    bool checkEnderCamps{false};
    std::vector<std::uint32_t> enderCamps;  // the exact camps visited, in order, when checkEnderCamps
    std::vector<std::uint32_t> followUps;
    bool checkMove{false};
    MoveReason move{MoveReason::None};
    std::uint32_t moveCamp{NO_CAMP};  // NO_CAMP: any camp
    bool moveInBracket{false};        // real data: a dry hub moves on, a bracket move into a zone that fits
};

// The abandon set exactly
inline void ExactAbandon(Expect& expect, std::vector<std::uint32_t> const& ids)
{
    expect.checkAbandon = true;
    expect.abandonFrom = ids;
    expect.abandonMust = ids;
}

struct Scenario
{
    std::string name;
    Snapshot bot;
    CircuitContext context;
    Expect expect;
};

struct Check
{
    bool pass{true};
    std::string text;
};

struct Report
{
    std::vector<Check> checks;
    CircuitPlan plan;
    float baselineYards{0.0f};
    double p50Microseconds{0.0};
    double p95Microseconds{0.0};
};

inline std::string IdList(std::vector<std::uint32_t> ids)
{
    std::sort(ids.begin(), ids.end());
    std::string text;
    for (std::uint32_t const id : ids)
        text += (text.empty() ? "" : ",") + std::to_string(id);
    return "{" + text + "}";
}

inline std::vector<std::uint32_t> TakenIds(CircuitPlan const& plan)
{
    std::vector<std::uint32_t> ids;
    for (CircuitTake const& take : plan.take)
        ids.push_back(take.quest);
    std::sort(ids.begin(), ids.end());
    return ids;
}

inline std::vector<std::uint32_t> AbandonedIds(CircuitPlan const& plan)
{
    std::vector<std::uint32_t> ids;
    for (CircuitAbandon const& abandon : plan.abandon)
        ids.push_back(abandon.quest);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Every decision of the plan in one text: equal texts mean equal plans
inline std::string PlanSummary(CircuitPlan const& plan)
{
    std::string text = plan.ok ? "ok" : "failed " + plan.failure;
    for (std::string const& reason : plan.reasons)
        text += "|" + reason;
    for (CircuitStop const& stop : plan.stops)
        text += "|" + std::to_string(stop.x) + "," + std::to_string(stop.y);
    return text + "|" + std::to_string(plan.yards) + "|" + std::to_string(plan.minutes);
}

// The plan in a few lines for the GM command and the notes
inline std::vector<std::string> PlanLines(CircuitPlan const& plan, float baselineYards)
{
    std::vector<std::string> lines;
    if (!plan.ok)
    {
        lines.push_back("no plan: " + plan.failure);
        return lines;
    }
    std::size_t const quests = plan.turnIns.size();
    lines.push_back("take " + IdList(TakenIds(plan)) + ", " + std::to_string(plan.stops.size()) + " stops, " +
                    std::to_string(quests) + " turn-ins at " + std::to_string(plan.enderCamps.size()) + " camp(s), " +
                    std::to_string(static_cast<int>(plan.yards)) + " yd (today's pick " +
                    std::to_string(static_cast<int>(baselineYards)) + " yd), " +
                    std::to_string(static_cast<int>(plan.minutes)) + " min, deferred " + IdList(plan.deferred) +
                    ", carried " + IdList(plan.carried) + ", abandon " + IdList(AbandonedIds(plan)) + ", follow-ups " +
                    IdList(plan.followUps) + ", move " + MoveName(plan.moveOn.reason));
    for (std::string const& reason : plan.reasons)
        lines.push_back("  " + reason);
    return lines;
}

inline void Add(Report& report, std::string const& name, bool pass, std::string const& text)
{
    report.checks.push_back({pass, name + ": " + text});
}

// The decision rows of a plan (WT13b r6c): one per take, abandon and move on, deferrals within the cap, words without
// spaces or separators, the same plan the same rows. Returns the problem, or empty
inline std::string LogRowsProblem(CircuitPlan const& plan, CircuitPlan const& again)
{
    std::vector<CircuitLog::Row> const rows = CircuitLog::Rows(plan);
    std::vector<CircuitLog::Row> const rowsAgain = CircuitLog::Rows(again);
    std::size_t const fixed = plan.take.size() + plan.abandon.size() + (plan.moveOn.reason != MoveReason::None);
    if (rows.size() < fixed)
        return "fewer rows (" + std::to_string(rows.size()) + ") than takes, abandons and the move (" +
               std::to_string(fixed) + ")";
    if (rows.size() > std::max(CircuitLog::MAX_ROWS, fixed))
        return std::to_string(rows.size()) + " rows, over the cap";
    if (plan.moveOn.reason != MoveReason::None && rows.back().choice.rfind("move:", 0) != 0)
        return "the last row is not the move";
    std::size_t takes = 0;
    for (CircuitLog::Row const& row : rows)
    {
        takes += row.choice.rfind("take:", 0) == 0;
        for (std::string const* word : {&row.choice, &row.reason})
        {
            if (word->empty())
                return "an empty word in row " + row.choice;
            for (char const c : *word)
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':'))
                    return "bad character in '" + *word + "'";
        }
    }
    if (takes != plan.take.size())
        return "take rows " + std::to_string(takes) + " for " + std::to_string(plan.take.size()) + " takes";
    if (rows.size() != rowsAgain.size())
        return "the same plan gave a different row count";
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].choice != rowsAgain[i].choice || rows[i].reason != rowsAgain[i].reason ||
            rows[i].value != rowsAgain[i].value)
            return "the same plan gave different rows";
    return {};
}

inline double Percentile(std::vector<double> values, double share)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    std::size_t const at = std::min(values.size() - 1, static_cast<std::size_t>(share * (values.size() - 1) + 0.5));
    return values[at];
}

// Whether a taken quest is one the bot may take: limits, levels, solo rules (the real-data check)
inline bool FitsBot(QuestRecord const& quest, Snapshot const& bot, std::uint32_t greyDiff)
{
    std::int32_t const level = Circuit::QuestLevel(quest, bot.level);
    bool const solo = bot.groupSize <= 1;
    return Eligible(quest, bot.race, bot.cls, bot.level) &&
           level <= static_cast<std::int32_t>(bot.level) + Circuit::MAX_LEVELS_ABOVE &&
           static_cast<std::int32_t>(bot.level) <= level + static_cast<std::int32_t>(greyDiff) &&
           !(quest.flags & Circuit::INSTANCE_FLAGS) && !(quest.flags & Circuit::REPEAT_FLAGS) &&
           !(solo && (quest.flags & Circuit::GROUP_FLAGS));
}

// The bot after the plan's loop, standing at the camp it moves on to: abandons and turn-ins leave the log, the turn-ins
// are rewarded, the takes join it (a carried take has no objectives, so it is done)
inline Snapshot AfterMove(Snapshot bot, CircuitPlan const& plan, QuestIndex const& quests)
{
    QuestCamp const& camp = quests.camps[plan.moveOn.camp];
    bot.position = {camp.mapId, camp.x, camp.y, camp.z};
    bot.zone = camp.zoneId;
    bot.area = camp.areaId;
    std::vector<std::uint32_t> gone = AbandonedIds(plan);
    for (CircuitTurnIn const& turnIn : plan.turnIns)
    {
        gone.push_back(turnIn.quest);
        bot.rewardedQuests.push_back(turnIn.quest);
    }
    std::sort(bot.rewardedQuests.begin(), bot.rewardedQuests.end());
    bot.rewardedQuests.erase(std::unique(bot.rewardedQuests.begin(), bot.rewardedQuests.end()),
                             bot.rewardedQuests.end());
    bot.quests.erase(std::remove_if(bot.quests.begin(), bot.quests.end(), [&gone](QuestSlot const& slot)
                                    { return std::find(gone.begin(), gone.end(), slot.quest) != gone.end(); }),
                     bot.quests.end());
    for (CircuitTake const& take : plan.take)
        if (std::find(gone.begin(), gone.end(), take.quest) == gone.end())
            bot.quests.push_back({take.quest, take.carried ? Circuit::SLOT_COMPLETE : 0u, {}});
    return bot;
}

// No walking in circles (review 4) and no wandering (review 5): follow the plan's moves from camp to camp, planning
// again at each, until a plan takes or turns in a quest (the bot's state changes) or stays; it must not come back to a
// camp it left before that, nor walk FOLLOW_HOPS moves without either
inline Check FollowMoves(Scenario const& scenario, CircuitPlan const& first)
{
    QuestIndex const& quests = *scenario.context.quests;
    std::string const name = scenario.name + ": no circle without a take or turn-in, ";
    std::vector<std::uint32_t> left{first.homeCamp};
    std::string path = first.homeCamp == NO_CAMP ? std::string("the field") : "camp " + std::to_string(first.homeCamp);
    Snapshot bot = scenario.bot;
    CircuitPlan plan = first;
    for (std::size_t hop = 0; hop < FOLLOW_HOPS; ++hop)
    {
        CircuitMove const move = plan.moveOn;
        if (move.reason == MoveReason::None || move.camp == NO_CAMP)
            return {true, name + path + (hop ? ", stays" : " stays") + " (move " + MoveName(move.reason) + ")"};
        path += " -> " + std::string(MoveName(move.reason)) + " to camp " + std::to_string(move.camp);
        if (std::find(left.begin(), left.end(), move.camp) != left.end())
            return {false, name + path + ": back at a camp it left"};
        bot = AfterMove(bot, plan, quests);
        plan = PlanCircuit(bot, scenario.context);
        if (!plan.ok)
            return {false, name + path + ": no plan there (" + plan.failure + ")"};
        left.push_back(move.camp);
        if (plan.homeCamp != move.camp)
            left.push_back(plan.homeCamp);
        if (!plan.take.empty() || !plan.turnIns.empty())
            return {true, name + path + ": takes " + IdList(TakenIds(plan)) + ", turns in " +
                              std::to_string(plan.turnIns.size()) + " there"};
    }
    return {false, name + path + ": a wander, no take or turn-in in " + std::to_string(FOLLOW_HOPS) + " moves"};
}

// Moving on goes only to where the bot can act on arrival (review 5): planned again at the camp it moves to, a chain
// or camp move takes a quest or works one of the log there, a delivery turns one in. A log still full after this trip
// frees a slot there: it gives a quest up, turns one in or works one
inline Check Arrival(Scenario const& scenario, CircuitPlan const& plan, bool fullAfter)
{
    std::string const name = scenario.name + ": acts on arrival, ";
    CircuitMove const& move = plan.moveOn;
    if (move.camp == NO_CAMP)
        return {true, name + "no move (" + MoveName(move.reason) + ")"};
    CircuitPlan const there = PlanCircuit(AfterMove(scenario.bot, plan, *scenario.context.quests), scenario.context);
    std::string const path = std::string(MoveName(move.reason)) + " to camp " + std::to_string(move.camp);
    if (!there.ok)
        return {false, name + path + ": no plan there (" + there.failure + ")"};
    bool const acts =
        move.reason == MoveReason::Delivery ? !there.turnIns.empty() : !there.take.empty() || !there.stops.empty();
    bool const frees = !fullAfter || move.reason == MoveReason::Delivery || !there.abandon.empty() ||
                       !there.turnIns.empty() || !there.stops.empty();
    return {acts && frees, name + path + (fullAfter ? " with the log full" : "") + ": takes " +
                               IdList(TakenIds(there)) + ", " + std::to_string(there.stops.size()) +
                               " stops, turns in " + std::to_string(there.turnIns.size()) + ", abandons " +
                               IdList(AbandonedIds(there))};
}

inline Report RunScenario(Scenario const& scenario, std::uint32_t timingRuns)
{
    Report report;
    std::string const& name = scenario.name;
    Expect const& expect = scenario.expect;
    QuestIndex const& quests = *scenario.context.quests;
    std::vector<double> times;
    for (std::uint32_t run = 0; run <= timingRuns; ++run)
    {
        auto const begin = std::chrono::steady_clock::now();
        CircuitPlan plan = PlanCircuit(scenario.bot, scenario.context);
        auto const end = std::chrono::steady_clock::now();
        if (run)
            times.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
        else
            report.plan = std::move(plan);
    }
    report.p50Microseconds = Percentile(times, 0.5);
    report.p95Microseconds = Percentile(times, 0.95);
    CircuitPlan const& plan = report.plan;
    Add(report, name, plan.ok, plan.ok ? "plan made" : "no plan: " + plan.failure);
    if (!plan.ok)
        return report;

    std::vector<std::uint32_t> const taken = TakenIds(plan);
    if (expect.exactTake)
    {
        std::vector<std::uint32_t> want = expect.take;
        std::sort(want.begin(), want.end());
        Add(report, name, taken == want, "takes " + IdList(taken) + ", expected " + IdList(want));
    }
    else
        Add(report, name, taken.size() >= expect.minTake, "takes " + IdList(taken));
    for (std::uint32_t const id : expect.notTake)
        Add(report, name, !std::binary_search(taken.begin(), taken.end(), id), "does not take " + std::to_string(id));
    bool fits = true;
    std::string misfits;
    for (CircuitTake const& take : plan.take)
    {
        QuestRecord const* record = quests.Find(take.quest);
        if (!record || !FitsBot(*record, scenario.bot, scenario.context.greyDiff))
        {
            fits = false;
            misfits += " " + std::to_string(take.quest);
        }
    }
    Add(report, name, fits, fits ? "every taken quest fits the bot" : "taken but not fitting:" + misfits);

    // The log: within its limit after the plan, and every fitting quest here taken while it has room
    std::vector<std::uint32_t> const abandoned = AbandonedIds(plan);
    std::size_t const before = static_cast<std::size_t>(std::count_if(
        scenario.bot.quests.begin(), scenario.bot.quests.end(), [](QuestSlot const& slot) { return slot.quest != 0; }));
    std::size_t const after = before - std::min(before, abandoned.size()) + taken.size();
    std::size_t const limit = QUEST_LOG_SLOTS - Circuit::LOG_RESERVE;
    std::size_t const left =
        static_cast<std::size_t>(std::count_if(plan.skipped.begin(), plan.skipped.end(), [](CircuitSkip const& skip)
                                               { return skip.reason == SkipReason::LogFull; }));
    Add(report, name, taken.empty() || after <= limit,
        "log " + std::to_string(before) + " -> " + std::to_string(after) + " quests (" +
            std::to_string(abandoned.size()) + " abandoned, " + std::to_string(taken.size()) + " taken), limit " +
            std::to_string(limit));
    Add(report, name, !left || after >= limit,
        "takes every fitting quest here while the log has room (" + std::to_string(left) + " left for log space)");

    std::size_t const completed = plan.turnIns.size();
    auto const turnedIn = [&plan](std::uint32_t id)
    {
        return std::any_of(plan.turnIns.begin(), plan.turnIns.end(),
                           [id](CircuitTurnIn const& turnIn) { return turnIn.quest == id; });
    };
    if (completed >= 2)
    {
        std::vector<std::uint32_t> camps;
        for (CircuitTurnIn const& turnIn : plan.turnIns)
            camps.push_back(turnIn.camp);
        std::sort(camps.begin(), camps.end());
        camps.erase(std::unique(camps.begin(), camps.end()), camps.end());
        Add(report, name, plan.enderCamps.size() <= camps.size() && plan.enderCamps.size() < completed,
            "turn-ins batched: " + std::to_string(plan.enderCamps.size()) + " ender visits for " +
                std::to_string(completed) + " quests at " + std::to_string(camps.size()) + " camp(s)");
    }
    // Quests whose objectives lie together: a stop of this trip holds a task of each, one ender visit turns them in
    for (std::vector<std::uint32_t> const& group : expect.together)
    {
        bool shared = false;
        for (CircuitStop const& stop : plan.stops)
            shared = shared || std::all_of(group.begin(), group.end(),
                                           [&stop](std::uint32_t id)
                                           {
                                               return std::any_of(stop.tasks.begin(), stop.tasks.end(),
                                                                  [id](CircuitTask const& t) { return t.quest == id; });
                                           });
        std::vector<std::uint32_t> camps;
        bool all = true;
        for (CircuitTurnIn const& turnIn : plan.turnIns)
            if (std::find(group.begin(), group.end(), turnIn.quest) != group.end())
                camps.push_back(turnIn.camp);
        for (std::uint32_t const id : group)
            all = all && turnedIn(id);
        bool const one = all && std::adjacent_find(camps.begin(), camps.end(), std::not_equal_to<>()) == camps.end();
        Add(report, name, shared && one,
            "together " + IdList(group) + ": one stop " + (shared ? "yes" : "no") + ", turned in at one visit " +
                (one ? "yes" : "no"));
    }
    // Carried quests are never turned in on this loop: their ender camps do not pull it away from the hub
    bool walkedTo = false;
    for (std::uint32_t const id : plan.carried)
        walkedTo = walkedTo || turnedIn(id);
    Add(report, name, !walkedTo, "carried " + IdList(plan.carried) + " not turned in on this loop");
    for (std::uint32_t const id : expect.carried)
        Add(report, name, std::binary_search(plan.carried.begin(), plan.carried.end(), id),
            "carries " + std::to_string(id) + " to its hub");
    for (std::uint32_t const id : expect.turnIns)
        Add(report, name, turnedIn(id), "turns in " + std::to_string(id) + " on this loop");
    if (expect.checkEnderCamps)
        Add(report, name, plan.enderCamps == expect.enderCamps,
            "ender camps " + IdList(plan.enderCamps) + ", expected " + IdList(expect.enderCamps));

    if (expect.checkAbandon)
    {
        std::vector<std::uint32_t> from = expect.abandonFrom;
        std::sort(from.begin(), from.end());
        bool const within = std::includes(from.begin(), from.end(), abandoned.begin(), abandoned.end());
        Add(report, name, within, "abandons " + IdList(abandoned) + ", only from " + IdList(from));
    }
    for (std::uint32_t const id : expect.abandonMust)
        Add(report, name, std::binary_search(abandoned.begin(), abandoned.end(), id), "abandons " + std::to_string(id));
    for (std::uint32_t const id : expect.keep)
        Add(report, name, !std::binary_search(abandoned.begin(), abandoned.end(), id), "keeps " + std::to_string(id));
    if (expect.checkDeferred)
    {
        std::vector<std::uint32_t> deferred = plan.deferred;
        std::sort(deferred.begin(), deferred.end());
        std::vector<std::uint32_t> want = expect.deferred;
        std::sort(want.begin(), want.end());
        Add(report, name, deferred == want, "defers " + IdList(deferred) + ", expected " + IdList(want));
    }
    if (!expect.followUps.empty())
        Add(report, name, plan.followUps == expect.followUps,
            "follow-ups " + IdList(plan.followUps) + ", expected " + IdList(expect.followUps));
    if (expect.checkMove)
        Add(report, name,
            plan.moveOn.reason == expect.move && (expect.moveCamp == NO_CAMP || plan.moveOn.camp == expect.moveCamp),
            std::string("moves on: ") + MoveName(plan.moveOn.reason) + " to camp " + std::to_string(plan.moveOn.camp) +
                ", expected " + MoveName(expect.move) + " to camp " + std::to_string(expect.moveCamp));
    if (expect.moveInBracket)
    {
        bool pass = plan.moveOn.reason == MoveReason::Chain || plan.moveOn.reason == MoveReason::Delivery;
        if (plan.moveOn.reason == MoveReason::Bracket && scenario.context.indexes)
            if (ZoneRecord const* zone = scenario.context.indexes->FindZone(plan.moveOn.zone))
                pass = zone->low <= scenario.bot.level && scenario.bot.level <= zone->high;
        Add(report, name, pass,
            std::string("dry hub moves on: ") + MoveName(plan.moveOn.reason) + " to camp " +
                std::to_string(plan.moveOn.camp) + " in zone " + std::to_string(plan.moveOn.zone) + " (quest " +
                std::to_string(plan.moveOn.quest) + ", " + std::to_string(static_cast<int>(plan.moveOn.seconds)) +
                " s)");
    }

    // Never a dead end (review 3, finding 1): the bot takes, walks, turns in or moves on. Nowhere hands the bot over to
    // its other goals (grinding, travel: WT14)
    Add(report, name,
        !plan.take.empty() || !plan.stops.empty() || !plan.turnIns.empty() || plan.moveOn.reason != MoveReason::None,
        std::string("the plan acts here or moves on (move ") + MoveName(plan.moveOn.reason) + ")");
    // A move leads where the bot acts on arrival; a log still full after this trip moves on only to deliver or to
    // where a slot frees (review 4, finding 1; review 5, findings 1 and 2)
    bool const fullAfter = after >= limit + plan.turnIns.size();
    report.checks.push_back(Arrival(scenario, plan, fullAfter));
    report.checks.push_back(FollowMoves(scenario, plan));

    // Information only (WT.21): no numeric travel target
    report.baselineYards = BaselineYards(plan, quests);
    if (completed)
    {
        float const circuit = plan.yards / static_cast<float>(completed);
        float const baseline = report.baselineYards / static_cast<float>(completed);
        Add(report, name, true,
            "information: yards per completed quest " + std::to_string(static_cast<int>(circuit)) +
                " vs today's random pick " + std::to_string(static_cast<int>(baseline)) + " (" +
                std::to_string(completed) + " quests)");
    }
    CircuitPlan const again = PlanCircuit(scenario.bot, scenario.context);
    Add(report, name, PlanSummary(again) == PlanSummary(plan), "same input, same plan");
    {
        std::string const problem = LogRowsProblem(plan, again);
        Add(report, name, problem.empty(),
            problem.empty() ? std::to_string(CircuitLog::Rows(plan).size()) + " decision rows, words clean, stable"
                            : "decision rows: " + problem);
    }
    Add(report, name, true,
        "job time p50 " + std::to_string(static_cast<int>(report.p50Microseconds)) + " us, p95 " +
            std::to_string(static_cast<int>(report.p95Microseconds)) + " us over " + std::to_string(times.size()) +
            " runs");
    return report;
}

// Bad positions are rejected at once; a missing run speed takes the default; the log's order does not matter
inline std::vector<Check> GuardChecks(Scenario const& base)
{
    std::vector<Check> checks;
    for (float const bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        Snapshot bot = base.bot;
        bot.position.x = bad;
        auto const begin = std::chrono::steady_clock::now();
        CircuitPlan const plan = PlanCircuit(bot, base.context);
        double const us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
        checks.push_back({!plan.ok && us < GUARD_MICROSECONDS, base.name + ": " + (std::isnan(bad) ? "nan" : "inf") +
                                                                   " position rejected (" + plan.failure + ") in " +
                                                                   std::to_string(static_cast<int>(us)) + " us"});
    }
    Snapshot slow = base.bot;
    slow.runSpeed = std::numeric_limits<float>::quiet_NaN();
    CircuitPlan const plan = PlanCircuit(slow, base.context);
    checks.push_back({plan.ok && std::isfinite(plan.minutes), base.name + ": nan run speed takes the default"});
    Snapshot reversed = base.bot;
    std::reverse(reversed.quests.begin(), reversed.quests.end());
    checks.push_back(
        {PlanSummary(PlanCircuit(reversed, base.context)) == PlanSummary(PlanCircuit(base.bot, base.context)),
         base.name + ": the log's order does not change the plan"});
    return checks;
}
}  // namespace CircuitHarness
}  // namespace Planner

#endif
