/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of applying the planner's goals (travel plan WT16, GoalInterface.h): the A/B rule, source precedence,
// the ticket match, the idle step's fallback reasons and bounded wait, the interlude's return, the executors (each
// quest option to what it names), the keep list, the circuit's next task, the one stall rule of every goal the
// planner starts (keys, the log signature without XP, the circuit and delivery loops it ends) and the planned point
// of interest.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/goal-apply-selftest.cpp

#include <cstdio>
#include <string>

#include "GoalInterface.h"

namespace
{
using namespace Planner;
using namespace Planner::GoalApply;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

Active GoalOf(Goals::GoalKind kind, std::uint32_t target, Source source)
{
    Active goal;
    goal.kind = kind;
    goal.target = target;
    goal.source = source;
    return goal;
}

// A planner goal as GoalModel makes it: the circuit (reason "circuit"), a delivery (target type quest), a move
Active Typed(Goals::GoalKind kind, Goals::TargetType targetType, char const* reason, std::uint32_t target = 1)
{
    Active goal = GoalOf(kind, target, Source::Planner);
    goal.targetType = targetType;
    goal.reason = reason;
    return goal;
}

CircuitPlan SampleCircuit()
{
    CircuitPlan plan;
    plan.ok = true;
    plan.take = {{101, 0, 1.0f, false}, {102, 0, 1.0f, true}};
    plan.deferred = {103};
    plan.carried = {102};
    CircuitStop first;
    first.x = 100.0f;
    first.y = 100.0f;
    first.tasks = {{201, 0, 100.0f, 100.0f, 0.0f}, {202, 1, 110.0f, 100.0f, 0.0f}};
    CircuitStop second;
    second.x = 500.0f;
    second.y = 500.0f;
    second.tasks = {{201, 1, 500.0f, 500.0f, 0.0f}, {203, 0, 505.0f, 500.0f, 0.0f}};
    plan.stops = {first, second};
    plan.turnIns = {{204, 0, 0}};
    plan.abandon = {{203, AbandonReason::Count}, {999, AbandonReason::Count}};
    return plan;
}
}  // namespace

int main()
{
    // The A/B rule: exactly guid % 10 = 0 keeps the weighted pick
    Check(InWeightedGroup(0) && InWeightedGroup(10) && InWeightedGroup(1230), "guid % 10 = 0 is the weighted group");
    Check(!InWeightedGroup(1) && !InWeightedGroup(15) && !InWeightedGroup(1239), "every other guid is a planner bot");

    // Precedence: an external goal outranks the planner's until done or cancelled
    {
        Slot slot;
        Check(slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner)), "the planner sets an empty slot");
        Check(slot.Set(GoalOf(Goals::GoalKind::Grind, 2, Source::Planner)), "the planner replaces its own goal");
        Check(slot.Set(GoalOf(Goals::GoalKind::TravelElsewhere, 3, Source::Bridge)), "the bridge replaces the planner");
        Check(!slot.Set(GoalOf(Goals::GoalKind::Quest, 4, Source::Planner)) && slot.Query().target == 3,
              "the planner cannot replace the bridge's goal");
        Check(slot.HasExternal(), "an external goal makes the bot busy");
        Check(!slot.Cancel(Source::Planner) && slot.Query().Is(), "the planner cannot cancel an external goal");
        Check(slot.Set(GoalOf(Goals::GoalKind::MoveZone, 5, Source::Group)), "a group goal replaces the bridge's");
        Check(slot.Cancel(Source::Group) && !slot.Query().Is(), "cancel ends the goal");
        Check(slot.Set(GoalOf(Goals::GoalKind::Quest, 6, Source::Planner)) && !slot.HasExternal(),
              "after the cancel the planner sets again");
    }

    // Interludes: a rest during a quest goal gives the slot back to the quest
    {
        Slot slot;
        slot.Set(GoalOf(Goals::GoalKind::Quest, 7, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(slot.End() && slot.Query().kind == Goals::GoalKind::Quest && slot.Query().target == 7,
              "the quest goal goes on after the rest");
        Check(!slot.End() && !slot.Query().Is(), "the quest goal then ends for good");
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(!slot.End() && !slot.Query().Is(), "an interlude without a goal before it just ends");
        slot.Set(GoalOf(Goals::GoalKind::Quest, 8, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Grind, 9, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(slot.End() && slot.Query().kind == Goals::GoalKind::Grind,
              "a new goal replaces the interrupted one: the grind goes on, not the quest");
        slot.Set(GoalOf(Goals::GoalKind::Errand, 0, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(slot.End() && slot.Query().kind == Goals::GoalKind::Grind, "two interludes return to the goal");
    }

    // The ticket: only the result of the job waited for is applied
    Check(TicketMatches(42, 42), "the job's own result is applied");
    Check(!TicketMatches(41, 42), "a stray result (an older job's) is refused");
    Check(!TicketMatches(0, 0) && !TicketMatches(7, 0), "no job out: nothing is applied");

    // The idle step
    {
        IdleInput in;
        in.ready = true;
        in.weighted = true;
        Check(DecideIdle(in).action == IdleAction::Weights, "a weighted bot keeps the weighted pick");
        in.weighted = false;
        in.switchOn = false;
        Check(DecideIdle(in).reason == Fallback::Off, "the switch off: fallback_off");
        in.switchOn = true;
        in.ready = false;
        in.freshPlan = true;
        Check(DecideIdle(in).reason == Fallback::NotReady, "indexes not ready: fallback_not_ready");
        in.ready = true;
        Check(DecideIdle(in).action == IdleAction::Apply, "a fresh plan is applied");
        in.freshPlan = false;
        in.jobComing = true;
        in.idleForMs = PLAN_WAIT_MS - 1;
        Check(DecideIdle(in).action == IdleAction::Wait, "a coming plan is waited for");
        in.idleForMs = PLAN_WAIT_MS;
        Check(DecideIdle(in).reason == Fallback::Late, "the wait is bounded: fallback_late");
        in.jobComing = false;
        Check(DecideIdle(in).reason == Fallback::NoPlan, "no plan and none coming: fallback_no_plan");
        in.lastRejected = true;
        Check(DecideIdle(in).reason == Fallback::Rejected, "a rejected answer: fallback_rejected");
        in.lastRejected = false;
        in.lastFailed = true;
        Check(DecideIdle(in).reason == Fallback::Failed, "a failed or lost job: fallback_failed at once");
        in.jobComing = true;
        in.idleForMs = 0;
        Check(DecideIdle(in).action == IdleAction::Wait, "a new job coming after a failure is waited for");
        in.jobComing = false;
        in.lastFailed = false;
        Check(std::string(FallbackWord(Fallback::None)) == "weights" &&
                  std::string(FallbackWord(Fallback::Failed)) == "fallback_failed",
              "the reason words");
    }

    // The executors: each quest option goes to what it names
    Reach const away;
    Reach onMap;
    onMap.deliverable = true;
    onMap.sameMap = true;
    Reach otherMap;
    otherMap.deliverable = true;
    Reach there;
    there.sameMap = true;
    there.there = true;
    Active const circuit = Typed(Goals::GoalKind::Quest, Goals::TargetType::Default, Goals::CIRCUIT_REASON, 40);
    Active const delivery = Typed(Goals::GoalKind::Quest, Goals::TargetType::Quest, "delivery", 102);
    Active const hubMove = Typed(Goals::GoalKind::Quest, Goals::TargetType::Default, "circuit_dry", 41);
    Active const campMove = Typed(Goals::GoalKind::Quest, Goals::TargetType::Camp, "hub", 9);
    Active const campCircuit = Typed(Goals::GoalKind::Quest, Goals::TargetType::Camp, Goals::CIRCUIT_REASON, 9);
    Check(ExecutorFor(circuit, away) == Executor::Circuit && ExecutorFor(campCircuit, there) == Executor::Circuit,
          "the plan's own circuit (at a hub or a camp) runs the local circuit");
    Check(ExecutorFor(delivery, onMap) == Executor::Deliver,
          "a delivery on the bot's map: DO_QUEST on the carried quest, which walks to its ender");
    Check(ExecutorFor(delivery, otherMap) == Executor::Travel, "a delivery on another map travels to the ender");
    Check(ExecutorFor(delivery, away) == Executor::None, "a delivery without a complete quest has nothing to do");
    Check(ExecutorFor(hubMove, away) == Executor::Travel && ExecutorFor(campMove, away) == Executor::Travel,
          "a move to the next hub or camp in the zone travels there (Northshire to Goldshire)");
    Check(ExecutorFor(hubMove, there) == Executor::None,
          "a move that is there already does not start: the next plan's circuit stands for that hub");
    {
        int circuits = 0;
        for (Active const& goal : {delivery, hubMove, campMove})
            for (Reach const& reach : {away, onMap, otherMap, there})
                circuits += ExecutorFor(goal, reach) == Executor::Circuit ? 1 : 0;
        Check(circuits == 0, "no delivery or move runs the local circuit (mutant: every quest option runs it)");
    }
    Check(ExecutorFor(Typed(Goals::GoalKind::Money, Goals::TargetType::Node, "gold"), away) == Executor::Gather &&
              ExecutorFor(Typed(Goals::GoalKind::Money, Goals::TargetType::Spot, "gold"), away) == Executor::Grind,
          "money: gather at a node, grind at a spot");
    Active const zoneMove = Typed(Goals::GoalKind::MoveZone, Goals::TargetType::Camp, "next_zone");
    Active const elsewhere = Typed(Goals::GoalKind::TravelElsewhere, Goals::TargetType::Default, "x");
    Check(ExecutorFor(zoneMove, away) == Executor::Travel && ExecutorFor(elsewhere, away) == Executor::Travel,
          "moves to another zone travel by route");
    Check(ExecutorFor(Typed(Goals::GoalKind::Errand, Goals::TargetType::Default, "x"), away) == Executor::None &&
              ExecutorFor(Typed(Goals::GoalKind::Hearth, Goals::TargetType::Default, "x"), away) == Executor::None,
          "errands and hearths start on their own triggers");

    // The plan's keep list, next task and planned point of interest
    {
        CircuitPlan const plan = SampleCircuit();
        std::vector<std::uint32_t> const keep = KeepList(plan);
        Check(Keeps(keep, 101) && Keeps(keep, 102) && Keeps(keep, 103) && Keeps(keep, 201) && Keeps(keep, 204),
              "taken, carried, deferred, worked and turned-in quests are kept");
        Check(!Keeps(keep, 203) && !Keeps(keep, 999) && !Keeps(keep, 300), "abandons and other quests are not");
        CircuitTask const* task = NextTask(plan, [](std::uint32_t quest) { return quest != 201; });
        Check(task && task->quest == 202, "the next task skips what the bot cannot work");
        Check(!NextTask(plan, [](std::uint32_t) { return false; }), "no workable task: none");
        std::vector<std::pair<float, float>> const points = {{900.0f, 900.0f}, {490.0f, 505.0f}, {120.0f, 90.0f}};
        Check(NearestToTasks(plan, 201, points) == 1, "the point nearest any of the quest's tasks");
        Check(NearestToTasks(plan, 201, {{900.0f, 900.0f}, {120.0f, 90.0f}}) == 1, "a task's nearest point");
        Check(NearestToTasks(plan, 203, points) == 1, "another quest's tasks");
        Check(NearestToTasks(plan, 300, points) == -1, "a quest without tasks: the executor's own pick");
    }

    // Turn-ins: complete log quests are walked to in the ender camps' order, after the incomplete tasks
    {
        CircuitPlan plan = SampleCircuit();
        plan.enderCamps = {7, 3};
        plan.turnIns = {{301, 0, 3}, {302, 0, 7}, {303, 0, 7}, {304, 0, 9}};
        auto const completeOf = [](std::vector<std::uint32_t> quests)
        {
            return [quests](std::uint32_t quest)
            { return std::find(quests.begin(), quests.end(), quest) != quests.end(); };
        };
        CircuitTurnIn const* turnIn = NextTurnIn(plan, completeOf({301, 303, 304}));
        // Mutant: the list's own order would answer 301 (camp 3, the second camp)
        CircuitTurnIn const* listOrder = nullptr;
        for (CircuitTurnIn const& candidate : plan.turnIns)
            if (!listOrder && completeOf({301, 303, 304})(candidate.quest))
                listOrder = &candidate;
        Check(turnIn && turnIn->quest == 303 && listOrder && listOrder->quest != turnIn->quest,
              "the first ender camp's complete quest first, not the list's order (mutant: list order)");
        turnIn = NextTurnIn(plan, completeOf({301, 304}));
        Check(turnIn && turnIn->quest == 301, "then the next camp's");
        turnIn = NextTurnIn(plan, completeOf({304}));
        Check(turnIn && turnIn->quest == 304, "a camp outside enderCamps comes last");
        Check(!NextTurnIn(plan, completeOf({})), "nothing complete: no turn-in");

        // A quest goal's steps: tasks, then turn-ins, then once its givers, then done
        auto const none = [](std::uint32_t) { return false; };
        auto const all = [](std::uint32_t) { return true; };
        QuestGoal goal;
        QuestNext next = AdvanceQuest(goal, plan, all, completeOf({302}));
        Check(next.step == QuestStep::Task && next.quest == 201, "a workable task comes first");
        next = AdvanceQuest(goal, plan, none, completeOf({302}));
        Check(next.step == QuestStep::TurnIn && next.quest == 302,
              "no task left: a complete quest's turn-in (mutant: incomplete quests only)");
        next = AdvanceQuest(goal, plan, none, completeOf({302}));
        Check(next.step == QuestStep::Givers, "each turn-in once per goal, then the givers");
        next = AdvanceQuest(goal, plan, none, completeOf({302}));
        Check(next.step == QuestStep::Done, "the givers once per goal, then the goal ends");
        CircuitPlan bare = SampleCircuit();
        bare.take.clear();
        bare.turnIns.clear();
        QuestGoal idle;
        Check(AdvanceQuest(idle, bare, none, none).step == QuestStep::Done,
              "no takes and no turn-ins: no givers visit");

        // The limit holds per goal, not per plan: a new plan at every idle keeps the goal's progress
        BotGoals goals;
        auto const shared = std::make_shared<Goals::GoalPlan>();
        shared->circuit = plan;
        TakePlan(goals, shared, 1000);
        BeginRun(goals, 40, 5);
        Check(AdvanceQuest(goals.quest, goals.plan->circuit, all, none).quest == 201, "the goal's first task");
        Check(AdvanceQuest(goals.quest, goals.plan->circuit, none, none).step == QuestStep::Givers, "then its givers");
        TakePlan(goals, shared, 6000);
        Check(goals.planMs == 6000 && goals.lastPlanMs == 1000 && !goals.planUsed, "the new plan is taken");
        Check(AdvanceQuest(goals.quest, goals.plan->circuit, none, none).step == QuestStep::Done,
              "a new plan does not send the bot to the givers again");
        Check(AdvanceQuest(goals.quest, goals.plan->circuit, all, none).quest == 202,
              "nor repeat the goal's started task");
        // Mutant: a plan that resets the goal's progress (WT16-a's per-plan limit) visits the givers again
        QuestGoal perPlan;
        Check(AdvanceQuest(perPlan, goals.plan->circuit, none, none).step == QuestStep::Givers,
              "(mutant: per-plan reset) the loop the per-goal limit ends");
        BeginRun(goals, 41, 5);
        Check(goals.quest.started.empty() && !goals.quest.giversVisited && goals.run.startLog == 5 &&
                  goals.run.key == 41 && goals.run.running,
              "a new goal starts its run and its circuit progress anew");
    }

    // The stall rule: a goal that ended without progress does not start again under its key while the log, level and
    // quest items stay the same
    {
        std::vector<QuestSlot> log(2);
        log[0].quest = 201;
        log[1].quest = 302;
        log[1].state = 1;
        std::vector<std::uint16_t> const items(12, 0);
        std::uint64_t const before = LogSignature(log, 10, items);
        Check(before == LogSignature(log, 10, items), "the same log and level: the same signature");
        std::vector<QuestSlot> counted = log;
        counted[0].counters[1] = 3;
        std::vector<QuestSlot> taken = log;
        taken.push_back({});
        taken.back().quest = 205;
        Check(LogSignature(counted, 10, items) != before && LogSignature(taken, 10, items) != before &&
                  LogSignature(log, 11, items) != before,
              "an objective counted, a quest taken or a level gained changes it");
        std::vector<std::uint16_t> collected = items;
        collected[7] = 1;
        Check(LogSignature(log, 10, collected) != before,
              "a quest item collected changes it (mutant: level and slots only); XP is no input, kills lift nothing");

        Goals::GoalPlan hubbed;
        hubbed.hub = 40;
        hubbed.circuit.homeCamp = 40;
        Goals::GoalPlan hubless;
        hubless.circuit.homeCamp = 40;
        Check(CircuitKey(hubbed) == 40 && CircuitKey(hubless) == (CAMP_KEY | 40),
              "a circuit's stall key: its hub, else its home camp, never a hub's key");

        BotGoals goals;
        Check(MayStart(goals, 40, before), "no stall yet: the circuit runs");
        BeginRun(goals, 40, before);
        EndRun(goals, before);
        Check(!goals.run.running && !MayStart(goals, 40, before), "the hub stalled: no goal runs its circuit again");
        // Mutant: a stall that ignores the log would also refuse after progress
        Check(MayStart(goals, 40, LogSignature(taken, 10, items)) && MayStart(goals, 40, LogSignature(log, 11, items)),
              "a changed log or level lifts the stall (mutant: stall by hub only)");
        EndRun(goals, LogSignature(counted, 10, items));
        Check(!MayStart(goals, 40, before), "a goal whose run already ended marks and lifts nothing");
        // A move between two hubs leaves the log as it was: each hub's circuit runs once, then both stay stalled
        Check(MayStart(goals, 41, before), "after a move the next hub's circuit runs once");
        BeginRun(goals, 41, before);
        EndRun(goals, before);
        Check(!MayStart(goals, 41, before) && !MayStart(goals, 40, before),
              "back at the first hub both stay stalled (mutant: one stall entry, the bot bounces between them)");
        BeginRun(goals, 40, before);
        EndRun(goals, LogSignature(counted, 10, items));
        Check(!goals.stalled && MayStart(goals, 40, before), "a goal that made progress lifts every stall");
        for (std::uint64_t key = 100; key < 100 + MAX_STALLED + 1; ++key)
        {
            BeginRun(goals, key, before);
            EndRun(goals, before);
        }
        Check(goals.stalled && goals.stalled->keys.size() == MAX_STALLED && MayStart(goals, 100, before) &&
                  !MayStart(goals, 101, before),
              "the stall list is bounded: the oldest key starts again first");
    }

    // Keys: one per goal and executor, never a circuit's
    {
        std::uint64_t const circuitKey = 40;
        std::uint64_t const deliverKey = GoalKey(delivery, Executor::Deliver, circuitKey);
        std::uint64_t const tripKey = GoalKey(delivery, Executor::Travel, circuitKey);
        Check(GoalKey(circuit, Executor::Circuit, circuitKey) == circuitKey &&
                  GoalKey(campCircuit, Executor::Circuit, circuitKey) == circuitKey,
              "every goal that runs the local circuit has the circuit's key, whatever its option named");
        Check(deliverKey != tripKey,
              "a delivery's trip to the ender's map and its turn-in there stall apart (mutant: key without executor)");
        Active const otherQuest = Typed(Goals::GoalKind::Quest, Goals::TargetType::Quest, "delivery", 103);
        Active const sameHubMove = Typed(Goals::GoalKind::Quest, Goals::TargetType::Default, "circuit_dry", 40);
        Active const grind = Typed(Goals::GoalKind::Grind, Goals::TargetType::Spot, "grind", 102);
        std::uint64_t const keys[] = {deliverKey,
                                      tripKey,
                                      GoalKey(otherQuest, Executor::Deliver, circuitKey),
                                      GoalKey(sameHubMove, Executor::Travel, circuitKey),
                                      GoalKey(grind, Executor::Grind, circuitKey),
                                      circuitKey,
                                      CAMP_KEY | 40};
        bool distinct = true;
        for (std::size_t i = 0; i < std::size(keys); ++i)
            for (std::size_t j = i + 1; j < std::size(keys); ++j)
                distinct = distinct && keys[i] != keys[j];
        Check(distinct, "deliveries, moves, grinds and circuits never share a key (a move to hub 40: not its circuit)");
    }

    // Review 2's and 3's loops, one rule: at each idle the bot starts the first option that may start (its key not
    // stalled), else the counted fallback runs; every run here ends with the log unchanged (takes the givers refuse,
    // a delivery that fails at the ender)
    {
        auto const plan = std::make_shared<Goals::GoalPlan>();
        plan->hub = 40;
        plan->circuit = SampleCircuit();
        struct Tally
        {
            int runs = 0;
            int deliveries = 0;
            int trips = 0;
            int fallbacks = 0;
        };
        // circuitOnly: the mutant of fix 2's rule, where only circuit goals have a run
        auto const simulate = [&plan](std::vector<Active> const& ranked, Reach const& reach, std::uint64_t log,
                                      int idles, bool circuitOnly)
        {
            BotGoals goals;
            TakePlan(goals, plan, 1000);
            Tally tally;
            for (int idle = 0; idle < idles; ++idle)
            {
                bool started = false;
                for (Active const& option : ranked)
                {
                    Executor const executor = ExecutorFor(option, reach);
                    if (executor == Executor::None)
                        continue;
                    bool const tracked = !circuitOnly || executor == Executor::Circuit;
                    std::uint64_t const key = GoalKey(option, executor, CircuitKey(*goals.plan));
                    if (tracked && !MayStart(goals, key, log))
                        continue;
                    if (tracked)
                        BeginRun(goals, key, log);
                    tally.runs += executor == Executor::Circuit ? 1 : 0;
                    tally.deliveries += executor == Executor::Deliver ? 1 : 0;
                    tally.trips += executor == Executor::Travel ? 1 : 0;
                    EndRun(goals, log);
                    started = true;
                    break;
                }
                tally.fallbacks += started ? 0 : 1;
            }
            return tally;
        };
        std::vector<Active> const ranked = {circuit, delivery};
        Tally const one = simulate(ranked, onMap, 77, 6, false);
        Check(one.runs == 1 && one.deliveries == 1 && one.fallbacks == 4,
              "the stalled circuit runs once, the failing delivery once, then the counted fallback");
        Tally const old = simulate(ranked, onMap, 77, 6, true);
        Check(old.runs == 1 && old.deliveries == 5 && old.fallbacks == 0,
              "(mutant: only circuits stall) the failing delivery starts at every idle and never falls back");
        // A delivery on another map without a route: the trip aborts at once, stalls, the fallback runs
        Tally const trip = simulate({delivery}, otherMap, 77, 4, false);
        Check(trip.trips == 1 && trip.fallbacks == 3, "a trip that aborts stalls (mutant: travel, abort, again)");
        // A trip that arrived (on the ender's map now, the log unchanged): the turn-in there still starts
        BotGoals goals;
        TakePlan(goals, plan, 1000);
        BeginRun(goals, GoalKey(delivery, ExecutorFor(delivery, otherMap), 40), 77);
        EndRun(goals, 77);
        Check(MayStart(goals, GoalKey(delivery, ExecutorFor(delivery, onMap), 40), 77),
              "after the trip the turn-in at the ender starts (mutant: one key per goal blocks it)");
        // Progress lifts every stall: the circuit and the delivery run again
        Check(MayStart(goals, 40, 78) && MayStart(goals, GoalKey(delivery, Executor::Travel, 40), 78),
              "a changed log lets every stalled goal start again");
        // Interludes have no run: a rest during a run keeps it, so the run ends (and is judged) once
        Reach const restReach;
        Check(ExecutorFor(Typed(Goals::GoalKind::Rest, Goals::TargetType::Default, "rest"), restReach) ==
                      Executor::Rest &&
                  Goals::IsInterlude(Goals::GoalKind::Rest),
              "a rest is an interlude: it starts no run of its own");
        // Mutant: review 2's rules, every quest option runs the circuit and one stall keyed by the option's target
        std::optional<std::uint32_t> stalledTarget;
        int mutantRuns = 0;
        for (int idle = 0; idle < 6; ++idle)
            for (Active const& option : ranked)
            {
                if (stalledTarget && *stalledTarget == option.target)
                    continue;
                ++mutantRuns;
                stalledTarget = option.target;
                break;
            }
        Check(mutantRuns == 6, "(mutant: route by kind, stall by target) the circuit runs at every idle");
    }

    // WT18a: the XP exemption. A grind, gather or money run that gained XP ends without a stall; a quest run's XP is no
    // progress (a kill on the way), and a grind with neither XP nor a log change stalls
    {
        BotGoals goals;
        BeginRun(goals, 50, 7, 1000, XpIsProgress(Goals::GoalKind::Grind));
        Check(goals.run.startXp == 1000 && goals.run.xpCounts, "a grind's run keeps its XP at the start");
        EndRun(goals, 7, 1400);
        Check(!goals.run.running && MayStart(goals, 50, 7),
              "a grind that gained XP does not stall (mutant: XP ignored)");
        BeginRun(goals, 50, 7, 1400, true);
        EndRun(goals, 7, 1400);
        Check(!MayStart(goals, 50, 7), "a grind without XP and without a log change stalls");
        BeginRun(goals, 51, 7, 1400, XpIsProgress(Goals::GoalKind::Gather));
        EndRun(goals, 7, 1500);
        Check(MayStart(goals, 51, 7) && !MayStart(goals, 50, 7),
              "a gather that gained XP does not stall, the grind's stall stays (mutant: XP lifts every stall)");
        BeginRun(goals, 52, 7, 1500, XpIsProgress(Goals::GoalKind::Quest));
        EndRun(goals, 7, 1900);
        Check(!MayStart(goals, 52, 7),
              "a quest goal's XP is no progress: it stalls (mutant: XP counts for every kind)");
        Check(XpIsProgress(Goals::GoalKind::Money) && !XpIsProgress(Goals::GoalKind::TravelElsewhere) &&
                  !XpIsProgress(Goals::GoalKind::Rest),
              "money counts its XP, a trip and a rest do not");
        std::uint64_t const levelUp = std::uint64_t{11} << 32;
        BeginRun(goals, 53, 7, (std::uint64_t{10} << 32) | 9000, true);
        EndRun(goals, 7, levelUp);
        Check(MayStart(goals, 53, 7), "a level gained (XP back to 0) is still XP gained in the level and XP number");
    }

    // WT18a: the bridge's goal orders through the plausibility gate
    {
        std::array<BridgeOrder, 9> const orders = {
            BridgeOrder::GoTo,      BridgeOrder::Quest,     BridgeOrder::Errands,
            BridgeOrder::SetHearth, BridgeOrder::GrindHere, BridgeOrder::GatherHere,
            BridgeOrder::MeetAt,    BridgeOrder::Pause,     BridgeOrder::Resume};
        int searches = 0;
        bool found = true;
        BridgeFacts open;
        open.inScope = true;
        open.placeKnown = true;
        open.questInLog = true;
        open.canGather = true;
        open.route = [&searches, &found]
        {
            ++searches;
            return found;
        };
        bool allPass = true;
        for (BridgeOrder order : orders)
            allPass = allPass && BridgeCheck(order, open) == BridgeAnswer::Done;
        Check(allPass && searches == 2, "every order passes in a plain case; only the two trips search a route");
        auto const answer = [&](BridgeOrder order, auto change)
        {
            BridgeFacts facts = open;
            change(facts);
            searches = 0;
            return BridgeCheck(order, facts);
        };
        bool scope = true;
        bool held = true;
        for (BridgeOrder order : orders)
        {
            scope = scope && answer(order, [](BridgeFacts& f) { f.inScope = false; }) == BridgeAnswer::NotPlanner &&
                    searches == 0;
            held =
                held &&
                answer(order, [](BridgeFacts& f) { f.current = GoalOf(Goals::GoalKind::Grind, 3, Source::Group); }) ==
                    BridgeAnswer::GoalHeld &&
                searches == 0;
        }
        Check(scope, "outside the planner's scope every order is not_planner, before a search (mutant: scope ignored)");
        Check(held, "a group's goal holds the slot against every order (mutant: the bridge replaces a group's goal)");
        Check(answer(BridgeOrder::GoTo, [](BridgeFacts& f)
                     { f.current = GoalOf(Goals::GoalKind::Grind, 3, Source::Bridge); }) == BridgeAnswer::Done &&
                  answer(BridgeOrder::GrindHere, [](BridgeFacts& f)
                         { f.current = GoalOf(Goals::GoalKind::Quest, 3, Source::Planner); }) == BridgeAnswer::Done,
              "the bridge's own goal and the planner's give way");
        for (BridgeOrder trip : {BridgeOrder::GoTo, BridgeOrder::MeetAt})
        {
            Check(answer(trip, [](BridgeFacts& f) { f.placeKnown = false; }) == BridgeAnswer::UnknownPlace &&
                      searches == 0,
                  "an unknown place is refused before a search (mutant: no unknown_place)");
            Check(answer(trip, [](BridgeFacts& f) { f.there = true; }) == BridgeAnswer::AlreadyThere && searches == 0,
                  "a bot at the place is already there (mutant: no already_there)");
            Check(answer(trip, [](BridgeFacts& f) { f.openWorld = false; }) == BridgeAnswer::CannotNow && searches == 0,
                  "no trip from inside an instance");
            found = false;
            Check(answer(trip, [](BridgeFacts&) {}) == BridgeAnswer::NoRoute && searches == 1,
                  "no route: no_route after one search (mutant: no no_route)");
            found = true;
            Check(answer(trip, [](BridgeFacts& f) { f.route = nullptr; }) == BridgeAnswer::NoRoute,
                  "without a route search no trip passes");
        }
        Check(answer(BridgeOrder::Quest, [](BridgeFacts& f) { f.questInLog = false; }) == BridgeAnswer::NoSuchQuest,
              "a quest not in the log is no_such_quest (mutant: no no_such_quest)");
        Check(answer(BridgeOrder::GoTo, [](BridgeFacts& f) { f.questInLog = false; }) == BridgeAnswer::Done &&
                  answer(BridgeOrder::Quest, [](BridgeFacts& f) { f.placeKnown = false; }) == BridgeAnswer::Done,
              "each order checks only its own facts");
        Check(answer(BridgeOrder::GrindHere, [](BridgeFacts& f) { f.canGrind = false; }) == BridgeAnswer::NothingHere &&
                  answer(BridgeOrder::GatherHere, [](BridgeFacts& f) { f.canGather = false; }) ==
                      BridgeAnswer::NothingHere,
              "nothing to grind in a town, nothing to gather without a skill (mutant: no nothing_here)");
        Check(
            answer(BridgeOrder::GrindHere, [](BridgeFacts& f) { f.openWorld = false; }) == BridgeAnswer::CannotNow &&
                answer(BridgeOrder::GatherHere, [](BridgeFacts& f) { f.openWorld = false; }) == BridgeAnswer::CannotNow,
            "no grind or gather inside an instance");

        Place const here{0, 1.0f, 2.0f, 3.0f};
        Place const at{1, 10.0f, 20.0f, 30.0f};
        Active const go = BridgeGoalFor(BridgeOrder::GoTo, here, at, 0, 500);
        Active const meet = BridgeGoalFor(BridgeOrder::MeetAt, here, at, 0, 500);
        Check(go.kind == Goals::GoalKind::TravelElsewhere && go.source == Source::Bridge && go.place.map == 1 &&
                  go.place.x == 10.0f && go.reason == "go_to" && go.startedMs == 500 &&
                  ExecutorFor(go, Reach{}) == Executor::Travel,
              "go_to: a bridge trip to the place (mutant: planner source)");
        Check(meet.kind == Goals::GoalKind::TravelElsewhere && meet.reason == "meet_at" && meet.place.map == 1,
              "meet_at: the same trip with the meeting as its reason");
        Active const quest = BridgeGoalFor(BridgeOrder::Quest, here, at, 77, 500);
        Reach working;
        working.workable = true;
        working.sameMap = true;
        Reach done;
        done.deliverable = true;
        done.sameMap = true;
        Reach stuck;
        stuck.sameMap = true;
        Active plannerQuest = quest;
        plannerQuest.source = Source::Planner;
        Check(quest.kind == Goals::GoalKind::Quest && quest.target == 77 &&
                  quest.targetType == Goals::TargetType::Quest && quest.place.map == 0 &&
                  ExecutorFor(quest, working) == Executor::Deliver && ExecutorFor(quest, done) == Executor::Deliver &&
                  ExecutorFor(plannerQuest, working) == Executor::None && ExecutorFor(quest, stuck) == Executor::None,
              "quest: an external quest goal works or turns in its quest; a planner delivery only delivers");
        Active const grind = BridgeGoalFor(BridgeOrder::GrindHere, here, at, 0, 500);
        Active const gather = BridgeGoalFor(BridgeOrder::GatherHere, here, at, 0, 500);
        Check(grind.kind == Goals::GoalKind::Grind && grind.place.map == 0 && grind.place.x == 1.0f &&
                  ExecutorFor(grind, Reach{}) == Executor::Grind && gather.kind == Goals::GoalKind::Gather &&
                  ExecutorFor(gather, Reach{}) == Executor::Gather && grind.reason == "grind_here",
              "grind_here and gather_here: around the bot's own position");
        Check(Goals::IsInterlude(BridgeGoalFor(BridgeOrder::Errands, here, at, 0, 0).kind) &&
                  BridgeGoalFor(BridgeOrder::SetHearth, here, at, 0, 0).kind == Goals::GoalKind::Hearth &&
                  !BridgeGoalFor(BridgeOrder::Pause, here, at, 0, 0).Is() &&
                  !BridgeGoalFor(BridgeOrder::Resume, here, at, 0, 0).Is(),
              "errands and set_hearth are interludes; a pause and a resume set no goal");

        BotGoals goals;
        goals.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        goals.paused = true;
        goals.switchTo = Goals::Option{};
        Check(TakeBridgeGoal(goals, grind) && goals.slot.Query().source == Source::Bridge && !goals.paused &&
                  !goals.switchTo,
              "a bridge goal replaces the planner's, ends a pause and drops a pending switch");
        Check(!goals.slot.Cancel(Source::Planner) && goals.slot.Query().Is(), "the planner never cancels it");
        Check(TakeBridgeGoal(goals, go) && goals.slot.Query().kind == Goals::GoalKind::TravelElsewhere,
              "a new bridge goal replaces the bridge's");
        goals.slot.Set(GoalOf(Goals::GoalKind::Grind, 2, Source::Group));
        Check(!TakeBridgeGoal(goals, go) && goals.slot.Query().source == Source::Group,
              "a group's goal is never replaced (mutant: the bridge replaces a group's goal)");
        BotGoals errand;
        errand.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        Check(TakeBridgeGoal(errand, BridgeGoalFor(BridgeOrder::Errands, here, at, 0, 0)) && errand.slot.End() &&
                  errand.slot.Query().source == Source::Planner,
              "an errand trip interrupts the planner's goal, which goes on after it");

        BotGoals pausing;
        pausing.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        BeginRun(pausing, 40, 5);
        Check(BridgePause(pausing, true, 1000) && !pausing.slot.Query().Is() && pausing.paused &&
                  !pausing.run.running && !pausing.stalled,
              "a pause ends the planner's goal without a stall");
        Check(StillPaused(pausing, 1000 + PAUSE_MS - 1) && pausing.paused, "a paused bot waits");
        Check(!StillPaused(pausing, 1000 + PAUSE_MS) && !pausing.paused, "a pause ends by itself after PAUSE_MS");
        BotGoals wrapped;
        BridgePause(wrapped, true, 0xFFFFFF00u);
        Check(StillPaused(wrapped, 0xFFFFFF80u) && StillPaused(wrapped, 0x100u),
              "the pause's age survives the clock's wrap");

        // The published view: OnIdle publishes a stale one for a bot of either group (a weighted bot has no job)
        BotGoals viewed;
        Check(!ViewStale(viewed), "a fresh bot's view is current");
        TakeBridgeGoal(viewed, grind);
        Check(ViewStale(viewed), "a bridge goal makes the view stale");
        MarkPublished(viewed);
        Check(!ViewStale(viewed) && !viewed.slot.Cancel(Source::Planner) && !ViewStale(viewed),
              "a published view is current; a refused cancel changes nothing");
        Check(!viewed.slot.End() && ViewStale(viewed),
              "a bridge goal's end makes the view stale (mutant: a weighted bot keeps showing its ended goal)");
        MarkPublished(viewed);
        viewed.slot.End();
        Check(!ViewStale(viewed), "an empty slot's end changes nothing");
        BridgePause(viewed, true, 100);
        MarkPublished(viewed);
        Check(StillPaused(viewed, 100 + PAUSE_MS - 1) && !ViewStale(viewed), "a pause that goes on keeps the view");
        Check(!StillPaused(viewed, 100 + PAUSE_MS) && ViewStale(viewed),
              "a pause's end by itself makes the view stale (mutant: the bot stays busy)");
        BotGoals resumed;
        TakeBridgeGoal(resumed, grind);
        MarkPublished(resumed);
        Check(BridgePause(resumed, false, 0) && ViewStale(resumed), "a resume's cancel of the bridge goal shows");
        BotGoals interlude;
        interlude.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        TakeBridgeGoal(interlude, BridgeGoalFor(BridgeOrder::Errands, here, at, 0, 0));
        MarkPublished(interlude);
        Check(interlude.slot.End() && ViewStale(interlude), "an interlude's end, its goal going on, shows");
        BotGoals resuming;
        TakeBridgeGoal(resuming, grind);
        BridgePause(resuming, true, 10);
        TakeBridgeGoal(resuming, grind);
        resuming.situationChanged = false;
        Check(BridgePause(resuming, false, 20) && !resuming.slot.Query().Is() && !resuming.paused &&
                  resuming.situationChanged,
              "a resume ends the bridge's goal and asks for a fresh plan");
        BotGoals own;
        own.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        Check(!BridgePause(own, false, 0) && own.slot.Query().Is(), "a resume keeps the planner's own goal");
        BotGoals group;
        group.slot.Set(GoalOf(Goals::GoalKind::Grind, 2, Source::Group));
        Check(!BridgePause(group, true, 0) && group.slot.Query().Is() && !group.paused &&
                  !BridgePause(group, false, 0) && group.slot.Query().Is(),
              "a pause or a resume leaves a group's goal (mutant: pause cancels a group goal)");
    }

    // The accept times the snapshot lacks
    {
        SeenQuests seen;
        UpdateSeen(seen, {5, 3}, 1000);
        UpdateSeen(seen, {3, 7}, 61000);
        std::vector<QuestProgress> const progress = ProgressOf(seen, 121000);
        Check(progress.size() == 2 && progress[0].quest == 3 && progress[0].elapsedMinutes == 2.0f &&
                  progress[1].quest == 7 && progress[1].elapsedMinutes == 1.0f,
              "a log quest's age runs from when it was first seen; a dropped one is forgotten");
    }

    // The goal row: the started option first
    {
        Goals::Ranked ranked;
        ranked.ok = true;
        for (std::uint32_t i = 0; i < 3; ++i)
        {
            Goals::Option option;
            option.kind = Goals::GoalKind::Grind;
            option.target = i;
            ranked.options.push_back(option);
        }
        Goals::Ranked const rotated = ChosenFirst(ranked, 2);
        Check(rotated.options[0].target == 2 && rotated.options[1].target == 0 && rotated.options[2].target == 1,
              "the started option leads the row, the others keep their order");
    }

    std::printf("goal apply selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
