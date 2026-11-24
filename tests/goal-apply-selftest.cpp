/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of applying the planner's goals (travel plan WT16, GoalInterface.h): the A/B rule, source precedence,
// the ticket match, the idle step's fallback reasons and bounded wait, the interlude's return, the executors (each
// quest option to what it names), the keep list, the circuit's next task, the one stall rule of every goal the
// planner starts (keys, the log signature without XP, the circuit and delivery loops it ends), the ends from outside
// it does not judge (change 2, CS14e) and the planned point of interest.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/goal-apply-selftest.cpp

#include <algorithm>
#include <cstdio>
#include <string>

#include "ClassQuestRules.h"
#include "GoalInterface.h"
#include "QuestFailBook.h"
#include "WorldTravelRules.h"

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
        Check(slot.Cancel(Source::Bridge) && !slot.Query().Is(), "cancel ends the goal");
        Check(slot.Set(GoalOf(Goals::GoalKind::Quest, 6, Source::Planner)) && !slot.HasExternal(),
              "after the cancel the planner sets again");
    }

    // Interludes: a rest during a quest goal gives the slot back to the quest
    {
        Slot slot;
        slot.Set(GoalOf(Goals::GoalKind::Quest, 7, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(slot.End(0) && slot.Query().kind == Goals::GoalKind::Quest && slot.Query().target == 7,
              "the quest goal goes on after the rest");
        Check(!slot.End(0) && !slot.Query().Is(), "the quest goal then ends for good");
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(!slot.End(0) && !slot.Query().Is(), "an interlude without a goal before it just ends");
        slot.Set(GoalOf(Goals::GoalKind::Quest, 8, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Grind, 9, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(slot.End(0) && slot.Query().kind == Goals::GoalKind::Grind,
              "a new goal replaces the interrupted one: the grind goes on, not the quest");
        slot.Set(GoalOf(Goals::GoalKind::Errand, 0, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(slot.End(0) && slot.Query().kind == Goals::GoalKind::Grind, "two interludes return to the goal");
    }

    // The idle step
    {
        IdleInput in;
        in.ready = true;
        in.weighted = true;
        Check(DecideIdle(in).action == IdleAction::Weights, "a weighted bot keeps the weighted pick");
        in.weighted = false;
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
        Check(DecideIdle(in).reason == Fallback::NoOption,
              "no plan and none coming: fallback_no_option (WT63e: fallback_no_plan is written no more)");
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

    // WT63e: the planner bot's fallback (3.19 (5)): its words, its list and its length
    {
        std::string words;
        for (std::size_t i = 0; i <= static_cast<std::size_t>(Fallback::Count); ++i)
            words += std::string(FallbackWord(static_cast<Fallback>(i))) + " ";
        Check(words ==
                  "weights fallback_not_ready fallback_failed fallback_late fallback_rejected fallback_no_option "
                  "fallback_cannot_start fallback_stalled fallback_no_option_stuck weights ",
              "every reason word, the new ones appended (WT63e three, WN4 one), out of range weights; fallback_no_plan "
              "gone");
        Check(NotStartedReason(0, false) == Fallback::NoOption && NotStartedReason(0, true) == Fallback::NoOption &&
                  NotStartedReason(3, false) == Fallback::CannotStart && NotStartedReason(3, true) == Fallback::Stalled,
              "a plan that started nothing: no option, none could start, every startable one stalled");
        using namespace StatusEnd;
        std::vector<std::uint32_t> const narrow = IdleList(true, false);
        std::vector<std::uint32_t> const full = IdleList(false, false);
        auto const has = [](std::vector<std::uint32_t> const& list, std::uint32_t status)
        { return std::find(list.begin(), list.end(), status) != list.end(); };
        Check(narrow.size() == 3 && has(narrow, STATUS_WANDER_RANDOM) && has(narrow, STATUS_WANDER_NPC) &&
                  has(narrow, STATUS_REST),
              "the planner bot's fallback list: wander random, wander npc, rest");
        bool never = true;
        for (std::uint32_t status : {STATUS_TRAVEL_FLIGHT, STATUS_OUTDOOR_PVP, STATUS_GO_GRIND, STATUS_GO_CAMP,
                                     STATUS_DO_QUEST, STATUS_DO_GATHER, STATUS_TRAVEL, STATUS_ERRANDS})
            never = never && !has(narrow, status) && !FallbackStatus(status);
        Check(never, "the fallback never flies, takes outdoor PvP, a grind or camp walk, a quest or a gather");
        Check(full.size() == 9 && has(full, STATUS_TRAVEL_FLIGHT) && has(full, STATUS_OUTDOOR_PVP) &&
                  has(full, STATUS_GO_GRIND) && has(full, STATUS_GO_CAMP) && has(full, STATUS_DO_QUEST) &&
                  has(full, STATUS_DO_GATHER) && has(full, STATUS_REST),
              "the weighted pick (the cohort, a held planner) keeps upstream's full list");
        bool same = true;
        for (std::uint32_t status = 0; status < STATUS_COUNT; ++status)
            same = same && FallbackStatus(status) == has(narrow, status);
        Check(same, "FallbackStatus is the list");

        // its length: a goal-less fallback status of the list ends at FALLBACK_STATUS_MS, no other goal-less one
        Check(FALLBACK_STATUS_MS > PLAN_WAIT_MS && FALLBACK_STATUS_MS < 5 * 60000,
              "the fallback is short: longer than the wait for a plan, shorter than upstream's wander timers");
        auto const fallback = [](std::uint32_t status, std::uint32_t statusMs)
        {
            PlannedFacts in;
            in.status = status;
            in.owns = true;
            in.inFallback = true;
            in.statusMs = statusMs;
            return in;
        };
        bool length = true;
        for (std::uint32_t status : narrow)
        {
            PlannedAnswer const before = PlannedEnd(Active{}, fallback(status, FALLBACK_STATUS_MS - 1));
            PlannedAnswer const at = PlannedEnd(Active{}, fallback(status, FALLBACK_STATUS_MS));
            length = length && before.planned && !before.ends && at.planned && at.ends &&
                     at.outcome == Outcome::TimedOut && at.word && std::string(at.word) == "fallback_time";
        }
        Check(length, "each status of the list ends timed out, fallback_time, at its length (not upstream's timer)");
        PlannedFacts noFallback = fallback(STATUS_WANDER_NPC, FALLBACK_STATUS_MS);
        noFallback.inFallback = false;
        PlannedFacts cohort = fallback(STATUS_WANDER_RANDOM, FALLBACK_STATUS_MS);
        cohort.owns = false;
        PlannedFacts sick = fallback(STATUS_REST, FALLBACK_STATUS_MS);
        sick.sick = true;
        Check(!PlannedEnd(Active{}, noFallback).planned && !PlannedEnd(Active{}, cohort).planned &&
                  !PlannedEnd(Active{}, sick).planned &&
                  !PlannedEnd(Active{}, fallback(STATUS_TRAVEL_FLIGHT, FALLBACK_STATUS_MS)).planned &&
                  !PlannedEnd(Active{}, fallback(STATUS_DO_QUEST, FALLBACK_STATUS_MS)).planned,
              "no other goal-less status ends by the fallback's length: a sickness rest, a command, a test's order, "
              "the cohort, a held planner, a status off the list keep their timers");
        Active quest = GoalOf(Goals::GoalKind::Quest, 1, Source::Planner);
        Check(!PlannedEnd(quest, fallback(STATUS_WANDER_NPC, FALLBACK_STATUS_MS)).planned,
              "a status with a goal is no fallback's (mutant: a goal's wander ends at the fallback's length)");
    }

    // WN4: the no-option loop's bound (WN3's G3: rest, ask, rank nothing, rest again, all night at one spot). OnIdle's
    // answer as PlannerService composes the pure steps: DecideIdle; an applied plan starts (StartActive) or names why
    // not (NotStartedReason); BoundNoOption. Searched, first one answer from every idle input at every count, then
    // every sequence of NO_OPTION_BOUND answers of eleven kinds from every count
    {
        using namespace StatusEnd;
        enum class Applied : std::uint8_t
        {
            Started,
            Interlude,
            NoOptions,
            CannotStart,
            Stalled,
            Count
        };
        struct Answer
        {
            IdleAction action{IdleAction::Weights};
            Fallback reason{Fallback::None};
            bool started{false};
        };
        auto const answer = [](BotGoals& goals, IdleInput const& in, Applied applied)
        {
            IdleDecision const decision = DecideIdle(in);
            if (decision.action == IdleAction::Wait || decision.action == IdleAction::Weights)
                return Answer{decision.action, Fallback::None, false};
            Fallback reason = decision.reason;
            if (decision.action == IdleAction::Apply)
            {
                if (applied == Applied::Started || applied == Applied::Interlude)
                {
                    Goals::GoalKind const kind =
                        applied == Applied::Started ? Goals::GoalKind::Grind : Goals::GoalKind::Rest;
                    bool const started = StartActive(goals, kind, Executor::Grind, {7, 1, 0, false},
                                                     [] { return true; }) == StartResult::Started;
                    return Answer{IdleAction::Apply, Fallback::None, started};
                }
                reason = applied == Applied::NoOptions ? NotStartedReason(0, false)
                                                       : NotStartedReason(3, applied == Applied::Stalled);
            }
            return Answer{IdleAction::Fallback, BoundNoOption(goals, reason), false};
        };
        std::vector<std::uint32_t> const moving = IdleList(true, true);
        bool movingList = !moving.empty();
        for (std::uint32_t const status : moving)
            movingList = movingList && status != STATUS_REST && FallbackStatus(status);
        Check(movingList && moving.size() == 2 && IdleList(false, true).size() == 9,
              "past the bound the fallback list leaves out rest, keeps wander random and npc (each ends at the "
              "fallback's length); the weighted list is unchanged");

        // One answer from every state: 2^6 inputs x 3 waits x counts 0..bound x 5 outcomes of an applied plan
        std::size_t states = 0;
        std::size_t bad = 0;
        std::size_t stuckSeen = 0;
        std::uint32_t const waits[] = {0, PLAN_WAIT_MS - 1, PLAN_WAIT_MS};
        for (std::uint32_t bits = 0; bits < 64; ++bits)
            for (std::uint32_t const wait : waits)
                for (std::uint8_t start = 0; start <= NO_OPTION_BOUND; ++start)
                    for (std::uint8_t a = 0; a < static_cast<std::uint8_t>(Applied::Count); ++a)
                    {
                        IdleInput in;
                        in.weighted = (bits & 1) != 0;
                        in.ready = (bits & 2) != 0;
                        in.freshPlan = (bits & 4) != 0;
                        in.jobComing = (bits & 8) != 0;
                        in.lastRejected = (bits & 16) != 0;
                        in.lastFailed = (bits & 32) != 0;
                        in.idleForMs = wait;
                        BotGoals goals;
                        goals.noOptionRun = start;
                        Answer const got = answer(goals, in, static_cast<Applied>(a));
                        std::uint8_t const after = goals.noOptionRun;
                        bool ok = true;
                        if (in.weighted)  // 4: the cohort never counts and never gets the word
                            ok = got.action == IdleAction::Weights && after == start;
                        if (got.action == IdleAction::Wait)  // 1: every wait ends
                            ok = ok && in.jobComing && wait < PLAN_WAIT_MS && after == start;
                        if (got.reason == Fallback::NoOption)
                            ok = ok && start + 1 < NO_OPTION_BOUND && after == start + 1;
                        else if (got.reason == Fallback::NoOptionStuck)
                            ok = ok && start + 1 >= NO_OPTION_BOUND && after == NO_OPTION_BOUND;
                        else if (got.started)  // 3: a started goal resets the count
                            ok = ok && after == 0;
                        else
                            ok = ok && after == start;
                        stuckSeen += got.reason == Fallback::NoOptionStuck;
                        ++states;
                        bad += !ok;
                    }
        std::printf("info: WN4 one answer: %zu states, %zu at the bound, %zu counterexamples\n", states, stuckSeen,
                    bad);
        Check(states == 5760 && bad == 0 && stuckSeen > 0,
              "one answer from every idle state: no-option answers count to the bound and stay there, the bound's "
              "answer is fallback_no_option_stuck, a start resets, nothing else counts, weights never");

        // 2: every sequence of NO_OPTION_BOUND answers from every count. A bot that gets no work answers
        // fallback_no_option_stuck by its NO_OPTION_BOUND-th no-option answer at the latest, and from then on every
        // no-option answer is stuck until a goal starts
        struct Kind
        {
            IdleInput in;
            Applied applied{Applied::NoOptions};
        };
        auto const kindOf =
            [](bool fresh, bool coming, std::uint32_t wait, bool rejected, bool failed, bool ready, Applied applied)
        {
            Kind kind;
            kind.in.ready = ready;
            kind.in.freshPlan = fresh;
            kind.in.jobComing = coming;
            kind.in.idleForMs = wait;
            kind.in.lastRejected = rejected;
            kind.in.lastFailed = failed;
            kind.applied = applied;
            return kind;
        };
        std::vector<Kind> const kinds = {
            kindOf(false, false, 0, false, false, true, Applied::NoOptions),            // no plan, none coming
            kindOf(true, false, 0, false, false, true, Applied::NoOptions),             // a plan that ranked nothing
            kindOf(false, true, 0, false, false, true, Applied::NoOptions),             // a wait
            kindOf(false, true, PLAN_WAIT_MS, false, false, true, Applied::NoOptions),  // late
            kindOf(false, false, 0, true, false, true, Applied::NoOptions),             // rejected
            kindOf(false, false, 0, false, true, true, Applied::NoOptions),             // failed
            kindOf(false, false, 0, false, false, false, Applied::NoOptions),           // not ready
            kindOf(true, false, 0, false, false, true, Applied::CannotStart),           // cannot start
            kindOf(true, false, 0, false, false, true, Applied::Stalled),               // stalled
            kindOf(true, false, 0, false, false, true, Applied::Started),               // a goal starts
            kindOf(true, false, 0, false, false, true, Applied::Interlude)};            // an interlude starts
        std::size_t sequences = 0;
        std::size_t badSequences = 0;
        std::size_t total = 1;
        for (std::uint8_t i = 0; i < NO_OPTION_BOUND; ++i)
            total *= kinds.size();
        for (std::uint8_t start = 0; start <= NO_OPTION_BOUND; ++start)
            for (std::size_t code = 0; code < total; ++code)
            {
                BotGoals goals;
                goals.noOptionRun = start;
                std::uint32_t counted = start;  // no-option answers in a row, as the bound should see them
                bool stuck = start >= NO_OPTION_BOUND;
                bool ok = true;
                std::size_t rest = code;
                for (std::uint8_t step = 0; step < NO_OPTION_BOUND; ++step)
                {
                    Kind const& kind = kinds[rest % kinds.size()];
                    rest /= kinds.size();
                    Answer const got = answer(goals, kind.in, kind.applied);
                    bool const noOption = got.reason == Fallback::NoOption || got.reason == Fallback::NoOptionStuck;
                    if (got.started)
                    {
                        counted = 0;
                        stuck = false;
                    }
                    else if (noOption)
                    {
                        ++counted;
                        // within the bound: the bound-th no-option answer in a row is stuck at the latest
                        ok = ok && (got.reason == Fallback::NoOptionStuck) == (counted >= NO_OPTION_BOUND);
                        // and once stuck, stuck until a start
                        ok = ok && (!stuck || got.reason == Fallback::NoOptionStuck);
                        stuck = stuck || got.reason == Fallback::NoOptionStuck;
                    }
                    ok = ok && goals.noOptionRun == std::min<std::uint32_t>(counted, NO_OPTION_BOUND);
                }
                ++sequences;
                badSequences += !ok;
            }
        std::printf("info: WN4 sequences: %zu sequences of %u answers from counts 0..%u, %zu counterexamples\n",
                    sequences, static_cast<unsigned>(NO_OPTION_BOUND), static_cast<unsigned>(NO_OPTION_BOUND),
                    badSequences);
        Check(sequences == total * (NO_OPTION_BOUND + 1) && badSequences == 0,
              "every sequence: the bound's word comes by the bound-th no-option answer in a row, stays until a goal "
              "or an interlude starts; late, failed, rejected, not ready, cannot start and stalled neither count nor "
              "reset");
        BotGoals fromZero;
        Fallback last = Fallback::None;
        for (std::uint8_t i = 0; i < NO_OPTION_BOUND; ++i)
            last = BoundNoOption(fromZero, Fallback::NoOption);
        Check(last == Fallback::NoOptionStuck && std::string(FallbackWord(last)) == "fallback_no_option_stuck" &&
                  BoundNoOption(fromZero, Fallback::Rejected) == Fallback::Rejected,
              "from a fresh bot the bound-th no-option answer is fallback_no_option_stuck; a rejected answer stays");
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
    {
        // WT35h: a turn-in given up (no_ender, QuestGiveUp::Noted) GIVE_UP_LIMIT times is not delivered again. A pin of
        // the chain: the record's feed (QuestFail::GivenUp) in the turn-in test the hook passes (PlannerService
        // TestsOf: complete and the picker's workable, !IsGivenUp) gives the delivery no executor on its map
        std::uint32_t constexpr BOT = 7;
        QuestFail::Book record;
        auto const reach = [&record, &delivery]()
        {
            Reach complete;
            complete.sameMap = true;
            complete.deliverable = !QuestFail::IsGivenUp(
                QuestFail::GivenUp(record.RowsOf(BOT), ClassQuests::GIVE_UP_LIMIT), delivery.target);
            return complete;
        };
        Check(ExecutorFor(delivery, reach()) == Executor::Deliver, "a complete quest never given up is delivered");
        record.Note(BOT, delivery.target, "no_ender", 4, 1);
        Check(ExecutorFor(delivery, reach()) == Executor::Deliver,
              "given up once (no_ender), it is delivered once more");
        record.Note(BOT, delivery.target, "no_ender", 3, 2);
        Check(ExecutorFor(delivery, reach()) == Executor::None,
              "given up twice, the delivery is not started again (mutant: the feed without its limit)");
    }
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
    Check(ExecutorFor(Typed(Goals::GoalKind::Errand, Goals::TargetType::Default, "x"), away) == Executor::Errand &&
              ExecutorFor(Typed(Goals::GoalKind::BuyBag, Goals::TargetType::Default, "x"), away) == Executor::Errand &&
              ExecutorFor(Typed(Goals::GoalKind::Hearth, Goals::TargetType::Default, "x"), away) == Executor::Bind,
          "errands and bags start through the errand entry, a hearth through the bind entry (WT63d)");

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
        // An interlude cut the givers visit short (WT63c, live: an errand trip to a far bank): the resumed goal goes
        // to its givers again instead of ending with nothing left
        ResumeQuest(goal);
        Check(AdvanceQuest(goal, plan, none, none).step == QuestStep::Givers,
              "a resumed goal visits its givers again (mutant: the visit stays spent, the goal ends)");
        Check(AdvanceQuest(goal, plan, none, none).step == QuestStep::Done, "once per resume");

        // The bag capacity edge (WT63c): the first look records, a change is a changed situation, the same is none
        {
            BotGoals seen;
            bool const firstLook = BagSlotsChanged(seen, 16);
            bool const same = BagSlotsChanged(seen, 16);
            bool const grown = BagSlotsChanged(seen, 40);
            bool const lost = BagSlotsChanged(seen, 16);
            Check(!firstLook && !same && grown && lost && seen.bagSlotsSeen == 16,
                  "the bag capacity's change only is an edge (mutant: every look, or never)");
        }

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

        // A running circuit goal keeps its own circuit (3.19 (6)): a plan taken away from its hub (an interlude's trip
        // to a far vendor or bank) does not replace it, one for the same circuit refreshes it, and once the goal ends
        // the newest plan's circuit counts again
        BotGoals away;
        auto const home = std::make_shared<Goals::GoalPlan>();
        home->hub = 7;
        auto const city = std::make_shared<Goals::GoalPlan>();
        city->hub = 9;
        TakePlan(away, home, 1000);
        BeginRun(away, CircuitKey(*home), 5);
        TakePlan(away, city, 2000);
        bool const kept = away.circuitPlan == home && away.plan == city;
        auto const homeAgain = std::make_shared<Goals::GoalPlan>(*home);
        TakePlan(away, homeAgain, 3000);
        bool const refreshed = away.circuitPlan == homeAgain;
        EndRun(away, 6);
        TakePlan(away, city, 4000);
        Check(kept && refreshed && away.circuitPlan == city,
              "a circuit goal runs its own hub's circuit while a plan for another comes (mutant: the newest plan's "
              "always, or never refreshed)");
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

    // Change 2 (consolidation CS14e): the status core tells the run why its status ended (NoteStatusEnd). An end the
    // goal did not cause leaves its run unjudged; an end by itself is judged by the log as before
    {
        using StatusEnd::Outcome;
        constexpr std::uint64_t KEY = 50;
        constexpr std::uint64_t LOG = 7;
        auto const begun = [](bool xpCounts = false)
        {
            BotGoals goals;
            BeginRun(goals, KEY, LOG, 100, xpCounts);
            return goals;
        };
        // The run ends with the log unchanged (and the given XP): does its key stall?
        auto const stalls = [](BotGoals& goals, std::uint64_t xp = 100)
        {
            EndRun(goals, LOG, xp);
            return !goals.run.running && !MayStart(goals, KEY, LOG);
        };
        auto const endedBy = [&begun, &stalls](Outcome outcome)
        {
            BotGoals goals = begun();
            NoteStatusEnd(goals, outcome, true);
            return stalls(goals);
        };
        Check(!endedBy(Outcome::Interrupted), "ended from outside, the goal is not stalled: a planner switch");
        Check(!endedBy(Outcome::Ordered), "ended from outside, the goal is not stalled: a hold");
        Check(!endedBy(Outcome::Ordered), "ended from outside, the goal is not stalled: a pause");
        Check(!endedBy(Outcome::Ordered), "ended from outside, the goal is not stalled: an order");
        Check(!endedBy(Outcome::Sick), "ended from outside, the goal is not stalled: sickness");
        Check(endedBy(Outcome::Arrived) && endedBy(Outcome::Finished) && endedBy(Outcome::TimedOut) &&
                  endedBy(Outcome::Stuck) && endedBy(Outcome::Abandoned),
              "arrived, finished, timeout, stuck, abandoned with the log unchanged: stalled as before");
        {
            BotGoals goals = begun();
            NoteStatusEnd(goals, Outcome::Interrupted, true);
            NoteStatusEnd(goals, Outcome::Finished, true);
            Check(!stalls(goals), "the interrupter's own end does not judge the goal (the mark is sticky)");
        }
        {
            BotGoals goals = begun();
            NoteStatusEnd(goals, Outcome::Ordered, true);
            ContinueRun(goals);
            NoteStatusEnd(goals, Outcome::Finished, true);
            Check(stalls(goals), "interrupted, resumed, then ended by itself: stalled (mutant: the mark never clears)");
        }
        {
            BotGoals goals = begun(true);
            NoteStatusEnd(goals, Outcome::Arrived, true);
            NoteStatusEnd(goals, Outcome::TimedOut, true);
            Check(stalls(goals), "go grind arrived, then the wander timed out without XP: stalled as before");
            goals = begun(true);
            NoteStatusEnd(goals, Outcome::Arrived, true);
            NoteStatusEnd(goals, Outcome::TimedOut, true);
            Check(!stalls(goals, 101), "the same chain with XP gained: progress, no stall");
        }
        {
            BotGoals goals = begun();
            EndRun(goals, LOG);
            BeginRun(goals, KEY + 1, LOG);
            NoteStatusEnd(goals, Outcome::Interrupted, true);
            EndRun(goals, LOG + 1);
            Check(!goals.stalled && MayStart(goals, KEY, LOG), "an outside end with a changed log lifts every stall");
            NoteStatusEnd(goals, Outcome::Interrupted, true);
            Check(!goals.run.outside, "an end while no run runs marks nothing");
            BeginRun(goals, KEY, LOG);
            goals.idleSinceMs = 5;
            goals.waitCounted = true;
            NoteStatusEnd(goals, Outcome::Finished, false);
            Check(goals.idleSinceMs == 5 && goals.waitCounted && !goals.run.outside,
                  "a judged end to a next status keeps the wait as it was");
            NoteStatusEnd(goals, Outcome::Finished, true);
            Check(goals.idleSinceMs == 0 && !goals.waitCounted, "an end to idle ends a wait for a plan");
        }
        // A start that replaces the goal's status (CS14f): an errand or inn start ends it as Interrupted, a sick start
        // as Sick, both to a next status: the run is marked, the wait stays as it was
        for (Outcome const outcome : {Outcome::Interrupted, Outcome::Sick})
        {
            BotGoals goals = begun();
            goals.idleSinceMs = 5;
            goals.waitCounted = true;
            NoteStatusEnd(goals, outcome, false);
            bool const sick = outcome == Outcome::Sick;
            Check(goals.run.outside && goals.idleSinceMs == 5 && goals.waitCounted,
                  sick ? "a sick start over the goal's status marks the run and keeps the wait"
                       : "an errand start over the goal's status marks the run and keeps the wait");
            NoteStatusEnd(goals, Outcome::Finished, true);
            Check(!stalls(goals), sick ? "the rest's own end then does not stall the goal"
                                       : "the errand trip's own end then does not stall the goal");
        }
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
        for (BridgeOrder order : orders)
        {
            scope = scope && answer(order, [](BridgeFacts& f) { f.inScope = false; }) == BridgeAnswer::NotPlanner &&
                    searches == 0;
        }
        Check(scope, "outside the planner's scope every order is not_planner, before a search (mutant: scope ignored)");
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
        Check(TakeBridgeGoal(goals, grind) && goals.slot.Query().source == Source::Bridge && !goals.paused,
              "a bridge goal replaces the planner's and ends a pause");
        Check(!goals.slot.Cancel(Source::Planner) && goals.slot.Query().Is(), "the planner never cancels it");
        Check(TakeBridgeGoal(goals, go) && goals.slot.Query().kind == Goals::GoalKind::TravelElsewhere,
              "a new bridge goal replaces the bridge's");
        BotGoals errand;
        errand.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        Check(TakeBridgeGoal(errand, BridgeGoalFor(BridgeOrder::Errands, here, at, 0, 0)) && errand.slot.End(0) &&
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
        Check(!viewed.slot.End(0) && ViewStale(viewed),
              "a bridge goal's end makes the view stale (mutant: a weighted bot keeps showing its ended goal)");
        MarkPublished(viewed);
        viewed.slot.End(0);
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
        Check(interlude.slot.End(0) && ViewStale(interlude), "an interlude's end, its goal going on, shows");
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

    // NeedShort: a window opens at the first short plan, stays open while plans stay short, closes at 0
    {
        BotGoals goals;
        bool const quiet = NoteShort(goals, 0, "") == ShortChange::None;
        bool const begins = NoteShort(goals, 120000, "Riding") == ShortChange::Begin && goals.shortOpen;
        bool const stays = NoteShort(goals, 90000, "Repair") == ShortChange::None && goals.shortKind == "Riding";
        bool const ends = NoteShort(goals, 0, "") == ShortChange::End && !goals.shortOpen;
        bool const endKind = goals.shortKind == "Riding";
        bool const again = NoteShort(goals, 0, "") == ShortChange::None &&
                           NoteShort(goals, 500, "Food") == ShortChange::Begin && goals.shortKind == "Food";
        Check(quiet && begins && stays && ends && endKind && again,
              "NeedShort: a begin at the first shortfall, none while it lasts (kind kept), an end at 0, then anew");
    }

    // Planner bots (3.19 (1), WT63b): in scope, outside the weighted group, the planner not held by a test
    Check(Owns(true, 11, true) && !Owns(false, 11, true) && !Owns(true, 10, true) && !Owns(true, 11, false),
          "Owns: only an in-scope bot outside the cohort whose planner no test holds (mutant: the hold ignored)");

    // A switch is one start (3.5, WT63b): StartActive with a fake executor. The fake plays the status core: a start
    // that works ends the running status as interrupted (OnStatusEnd -> NoteStatusEnd), one that fails ends nothing
    {
        using StatusEnd::Outcome;
        constexpr std::uint64_t OLD_KEY = 40;
        constexpr std::uint64_t NEW_KEY = 50;
        constexpr std::uint64_t LOG = 5;
        int ends = 0;
        int calls = 0;
        auto const running = []
        {
            BotGoals goals;
            goals.slot.Set(GoalOf(Goals::GoalKind::Grind, 1, Source::Planner));
            BeginRun(goals, OLD_KEY, LOG);
            goals.quest.started = {7};
            return goals;
        };
        auto const startsOver = [&ends, &calls](BotGoals& goals) -> std::function<bool()>
        {
            return [&ends, &calls, &goals]
            {
                ++calls;
                ++ends;
                NoteStatusEnd(goals, Outcome::Interrupted, false);
                return true;
            };
        };
        std::function<bool()> const refuses = [&calls]
        {
            ++calls;
            return false;
        };
        auto const unchanged = [](BotGoals const& goals)
        {
            return goals.run.running && goals.run.key == OLD_KEY && !goals.run.outside &&
                   goals.quest.started.size() == 1 && goals.slot.Query().kind == Goals::GoalKind::Grind &&
                   !goals.stalled;
        };

        BotGoals refused = running();
        Check(StartActive(refused, Goals::GoalKind::Quest, Executor::Circuit, {NEW_KEY, LOG}, refuses) ==
                      StartResult::Refused &&
                  calls == 1 && ends == 0 && unchanged(refused),
              "a refused start ends nothing: the run, its progress and the slot as before (mutant: progress lost)");
        calls = 0;
        BotGoals none = running();
        Check(StartActive(none, Goals::GoalKind::Errand, Executor::None, {NEW_KEY, LOG}, startsOver(none)) ==
                      StartResult::NoExecutor &&
                  calls == 0 && ends == 0 && unchanged(none),
              "an option without an executor is not started and ends nothing");
        BotGoals stalled = running();
        stalled.stalled = Stalled{LOG, {NEW_KEY}};
        Check(StartActive(stalled, Goals::GoalKind::Grind, Executor::Grind, {NEW_KEY, LOG}, startsOver(stalled)) ==
                      StartResult::Stalled &&
                  calls == 0 && ends == 0 && stalled.run.key == OLD_KEY && !stalled.run.outside,
              "a stalled option is not started and ends nothing");
        Check(StartActive(stalled, Goals::GoalKind::Grind, Executor::Grind, {NEW_KEY, LOG, 0, false},
                          startsOver(stalled)) == StartResult::Started,
              "the bridge's order skips the stall rule");

        ends = 0;
        BotGoals switched = running();
        bool const started = StartActive(switched, Goals::GoalKind::Grind, Executor::Grind, {NEW_KEY, LOG, 0},
                                         startsOver(switched)) == StartResult::Started;
        Check(started && ends == 1 && switched.run.running && switched.run.key == NEW_KEY && !switched.run.outside &&
                  switched.quest.started.empty() && !switched.stalled,
              "a switch that starts ends one status, closes the old run without a stall and begins the new one "
              "unmarked (mutant: the run begun before the start)");
        NoteStatusEnd(switched, Outcome::Finished, true);
        EndRun(switched, LOG);
        Check(switched.stalled && switched.stalled->keys.size() == 1 && switched.stalled->keys[0] == NEW_KEY,
              "the switched-to goal's own end is judged: its key stalls, the old one does not");

        BotGoals changed = running();
        changed.stalled = Stalled{LOG, {99}};
        StartActive(changed, Goals::GoalKind::Grind, Executor::Grind, {NEW_KEY, LOG + 1, 0}, startsOver(changed));
        Check(!changed.stalled && changed.run.startLog == LOG + 1,
              "closing the old run at a changed log still lifts the stalls");

        // An errand order wins over a running goal: no run of its own, the goal resumes with its run
        BotGoals errand = running();
        Active const order = Typed(Goals::GoalKind::Errand, Goals::TargetType::Default, "stock_due");
        bool const errandStarted = StartActive(errand, order.kind, ExecutorFor(order, Reach{}), {NEW_KEY, LOG},
                                               startsOver(errand)) == StartResult::Started;
        errand.slot.Set(order);
        Check(errandStarted && errand.run.running && errand.run.key == OLD_KEY && errand.run.outside &&
                  errand.quest.started.size() == 1 && errand.slot.Query().kind == Goals::GoalKind::Errand,
              "an errand order starts as an interlude: the goal keeps its run (marked outside) and progress");
        NoteStatusEnd(errand, Outcome::Arrived, true);  // the trip's own end
        bool const resumed = errand.slot.End(0) && errand.slot.Query().kind == Goals::GoalKind::Grind;
        ContinueRun(errand);
        Check(resumed && errand.run.running && errand.run.key == OLD_KEY && !errand.run.outside,
              "after the errand the interrupted goal resumes with its run");
        BotGoals refusedErrand = running();
        Check(
            StartActive(refusedErrand, order.kind, Executor::Errand, {NEW_KEY, LOG}, refuses) == StartResult::Refused &&
                unchanged(refusedErrand),
            "an errand the entry refuses changes nothing");

        // The bridge's errand goal starts through the entry, as an interlude
        Place const here{0, 1.0f, 2.0f, 3.0f};
        Active const bridged = BridgeGoalFor(BridgeOrder::Errands, here, here, 0, 0);
        BotGoals bridge = running();
        ends = 0;
        bool const bridgeStarted = ExecutorFor(bridged, Reach{}) == Executor::Errand &&
                                   StartActive(bridge, bridged.kind, ExecutorFor(bridged, Reach{}),
                                               {NEW_KEY, LOG, 0, false}, startsOver(bridge)) == StartResult::Started;
        Check(bridgeStarted && ends == 1 && TakeBridgeGoal(bridge, bridged) && bridge.run.key == OLD_KEY &&
                  bridge.slot.End(0) && bridge.slot.Query().kind == Goals::GoalKind::Grind,
              "the bridge's errand goal starts through the errand entry and the planner's goal goes on after it");

        // WT63d: a bind order that wins starts through the bind entry as an interlude; the goal resumes after it
        BotGoals bind = running();
        Active const hearth = Typed(Goals::GoalKind::Hearth, Goals::TargetType::Default, "bind_far", 1405);
        ends = 0;
        bool const bindStarted =
            ExecutorFor(hearth, Reach{}) == Executor::Bind &&
            StartActive(bind, hearth.kind, Executor::Bind, {NEW_KEY, LOG}, startsOver(bind)) == StartResult::Started;
        bind.slot.Set(hearth);
        Check(bindStarted && ends == 1 && bind.run.running && bind.run.key == OLD_KEY && bind.run.outside &&
                  bind.quest.started.size() == 1 && bind.slot.Query().kind == Goals::GoalKind::Hearth &&
                  bind.slot.Query().target == 1405,
              "a bind order that wins starts as an interlude: the goal keeps its run and progress, the slot its inn");
        NoteStatusEnd(bind, Outcome::Arrived, true);  // the bind trip's own end (RPG_TRAVEL)
        bool const bindResumed = bind.slot.End(0) && bind.slot.Query().kind == Goals::GoalKind::Grind;
        ContinueRun(bind);
        Check(bindResumed && bind.run.running && bind.run.key == OLD_KEY && !bind.run.outside && !bind.stalled,
              "after the bind the interrupted goal resumes with its run, unstalled");
        BotGoals refusedBind = running();
        Check(StartActive(refusedBind, hearth.kind, Executor::Bind, {NEW_KEY, LOG}, refuses) == StartResult::Refused &&
                  unchanged(refusedBind),
              "a bind the entry refuses (no inn in reach) changes nothing");

        // The bridge's set-hearth starts through the same bind entry; the goal is taken only when it started, a refusal
        // finds nothing to do here
        Active const setHearth = BridgeGoalFor(BridgeOrder::SetHearth, here, here, 0, 0);
        BotGoals bridgeBind = running();
        ends = 0;
        bool const setStarted = ExecutorFor(setHearth, Reach{}) == Executor::Bind && setHearth.target == 0 &&
                                StartActive(bridgeBind, setHearth.kind, ExecutorFor(setHearth, Reach{}),
                                            {NEW_KEY, LOG, 0, false}, startsOver(bridgeBind)) == StartResult::Started;
        Check(setStarted && ends == 1 && TakeBridgeGoal(bridgeBind, setHearth) && bridgeBind.run.key == OLD_KEY &&
                  bridgeBind.slot.End(0) && bridgeBind.slot.Query().kind == Goals::GoalKind::Grind,
              "the bridge's set-hearth starts through the bind entry (no inn named: the entry picks) and the goal goes "
              "on");
        BotGoals bridgeRefused = running();
        Check(StartActive(bridgeRefused, setHearth.kind, Executor::Bind, {NEW_KEY, LOG, 0, false}, refuses) ==
                      StartResult::Refused &&
                  unchanged(bridgeRefused) && BridgeRefused(BridgeOrder::SetHearth) == BridgeAnswer::NothingHere &&
                  BridgeRefused(BridgeOrder::Errands) == BridgeAnswer::NothingHere &&
                  BridgeRefused(BridgeOrder::GoTo) == BridgeAnswer::Failed,
              "a set-hearth the entry refuses takes no goal and answers nothing here (mutant: failed)");
        Check(!Owns(true, 11, false) && Owns(true, 11, true) && !Owns(true, 10, true),
              "a bot whose planner a test holds is not asked: its trigger binds as the cohort's (mutant: Owns ignores "
              "the hold)");
        BotGoals edge;
        bool const firstLook = BindFarChanged(edge, true);
        bool const turnedNear = BindFarChanged(edge, false);
        bool const turnedFar = BindFarChanged(edge, true);
        bool const stillFar = BindFarChanged(edge, true);
        Check(!firstLook && !turnedNear && turnedFar && !stillFar,
              "the inn trigger's edge: a bind seen near that turns far reports once, the first look records (mutant: "
              "every far check reports)");
    }

    // A failed interlude stamps its kind's retry time (WT63c, 3.19 (6)): the planner passes the kind over until then, a
    // success clears the stamp, and the goal it interrupted resumes without a stall
    {
        using StatusEnd::Outcome;
        constexpr std::uint64_t QUEST_KEY = 11;
        constexpr std::uint64_t ERRAND_KEY = 12;
        constexpr std::uint64_t LOG = 22;
        constexpr std::uint32_t NOW = 1000;
        constexpr std::uint32_t RETRY = WorldTravel::ERRAND_RETRY_MS;
        auto const starts = [] { return true; };
        BotGoals goals;
        Active const quest = GoalOf(Goals::GoalKind::Quest, 7, Source::Planner);
        bool const questStarted =
            StartActive(goals, quest.kind, Executor::Circuit, {QUEST_KEY, LOG}, starts) == StartResult::Started;
        goals.slot.Set(quest);
        Active const errand = GoalOf(Goals::GoalKind::Errand, 77, Source::Planner);
        bool const errandStarted =
            StartActive(goals, errand.kind, Executor::Errand, {ERRAND_KEY, LOG}, starts) == StartResult::Started;
        NoteStatusEnd(goals, Outcome::Interrupted, false);  // the quest's status, cut by the errand's start
        goals.slot.Set(errand);
        NoteStatusEnd(goals, Outcome::TimedOut, true);
        NoteInterludeEnd(goals, errand.kind, Outcome::TimedOut, NOW);
        Check(questStarted && errandStarted && RetryWaits(goals, Goals::GoalKind::Errand, NOW + 1) &&
                  RetryWaits(goals, Goals::GoalKind::Errand, NOW + RETRY - 1) &&
                  !RetryWaits(goals, Goals::GoalKind::Errand, NOW + RETRY),
              "a failed errand is passed over for the retry time, then weighed again (mutant: no stamp)");
        Check(
            !RetryWaits(goals, Goals::GoalKind::BuyBag, NOW + 1) && !RetryWaits(goals, Goals::GoalKind::Quest, NOW + 1),
            "the stamp holds back its own kind only");
        bool const resumed = goals.slot.End(0) && goals.slot.Query().kind == Goals::GoalKind::Quest;
        ContinueRun(goals);
        Check(resumed && goals.run.running && goals.run.key == QUEST_KEY && !goals.run.outside &&
                  MayStart(goals, QUEST_KEY, LOG),
              "after the failed errand the interrupted goal resumes with its run and does not stall");
        NoteInterludeEnd(goals, Goals::GoalKind::Errand, Outcome::Interrupted, NOW + 5);
        Check(RetryWaits(goals, Goals::GoalKind::Errand, NOW + 6), "an end from outside leaves the stamp as it was");
        NoteInterludeEnd(goals, Goals::GoalKind::Errand, Outcome::Finished, NOW + 10);
        Check(!RetryWaits(goals, Goals::GoalKind::Errand, NOW + 11), "a finished errand clears the stamp");
        BotGoals other;
        NoteInterludeEnd(other, Goals::GoalKind::BuyBag, Outcome::Interrupted, NOW);
        NoteInterludeEnd(other, Goals::GoalKind::Quest, Outcome::TimedOut, NOW);
        Check(
            !RetryWaits(other, Goals::GoalKind::BuyBag, NOW + 1) && !RetryWaits(other, Goals::GoalKind::Quest, NOW + 1),
            "an interrupted interlude and a goal that is no interlude stamp nothing");
        NoteInterludeEnd(other, Goals::GoalKind::Rest, Outcome::TimedOut, NOW);
        Check(!RetryWaits(other, Goals::GoalKind::Rest, NOW + 1),
              "a rest that ran its time stamps nothing (mutant: every rest waits out the retry time)");
        constexpr std::uint32_t LATE = 0xFFFFFF00u;
        NoteInterludeEnd(other, Goals::GoalKind::BuyBag, Outcome::Stuck, LATE);
        Check(RetryWaits(other, Goals::GoalKind::BuyBag, 0x100u), "the wait holds across the clock's wrap");
        // WT63d: a bind that failed (the trip timed out, got stuck or was abandoned) stamps the hearth kind; the goal
        // it interrupted resumes without a stall; a bind set (arrived) clears the stamp
        BotGoals hearth;
        Active const quest2 = GoalOf(Goals::GoalKind::Quest, 7, Source::Planner);
        StartActive(hearth, quest2.kind, Executor::Circuit, {QUEST_KEY, LOG}, starts);
        hearth.slot.Set(quest2);
        Active const bind = GoalOf(Goals::GoalKind::Hearth, 1405, Source::Planner);
        bool const bindStarted =
            StartActive(hearth, bind.kind, Executor::Bind, {ERRAND_KEY, LOG}, starts) == StartResult::Started;
        NoteStatusEnd(hearth, Outcome::Interrupted, false);
        hearth.slot.Set(bind);
        bool allFailures = true;
        for (Outcome const outcome : {Outcome::TimedOut, Outcome::Stuck, Outcome::Abandoned})
        {
            BotGoals each;
            NoteInterludeEnd(each, Goals::GoalKind::Hearth, outcome, NOW);
            allFailures = allFailures && RetryWaits(each, Goals::GoalKind::Hearth, NOW + RETRY - 1) &&
                          !RetryWaits(each, Goals::GoalKind::Hearth, NOW + RETRY) &&
                          !RetryWaits(each, Goals::GoalKind::Errand, NOW + 1);
        }
        NoteStatusEnd(hearth, Outcome::Stuck, true);
        NoteInterludeEnd(hearth, hearth.slot.Query().kind, Outcome::Stuck, NOW);
        bool const passedOver = RetryWaits(hearth, Goals::GoalKind::Hearth, NOW + 1);
        bool const bindResumed = hearth.slot.End(0) && hearth.slot.Query().kind == Goals::GoalKind::Quest;
        ContinueRun(hearth);
        Check(bindStarted && allFailures && passedOver && bindResumed && hearth.run.running &&
                  hearth.run.key == QUEST_KEY && MayStart(hearth, QUEST_KEY, LOG),
              "a failed bind stamps the hearth's retry time and the interrupted goal resumes unstalled (mutant: no "
              "stamp for hearth)");
        NoteInterludeEnd(hearth, Goals::GoalKind::Hearth, Outcome::Arrived, NOW + 10);
        Check(!RetryWaits(hearth, Goals::GoalKind::Hearth, NOW + 11), "a bind set at the inn clears the stamp");
        // WT63d-b: an arrival at the inn whose bind did not take is no arrival: it stamps the hearth kind, so the
        // planner does not send the bot to an inn again within the retry time (lab14: a bot ordered the same inn six
        // times in 54 s while its bind never took); a bind that took clears it
        BotGoals loop;
        loop.slot.Set(bind);
        NoteInterludeEnd(loop, loop.slot.Query().kind, WorldTravel::InnEndOf(false, WorldTravel::InnStep::Bind).outcome,
                         NOW);
        bool const unboundStamps = RetryWaits(loop, Goals::GoalKind::Hearth, NOW + RETRY - 1);
        BotGoals stuckOut;
        NoteInterludeEnd(stuckOut, Goals::GoalKind::Hearth,
                         WorldTravel::InnEndOf(false, WorldTravel::InnStep::Fail).outcome, NOW);
        BotGoals took;
        NoteInterludeEnd(took, Goals::GoalKind::Hearth, Outcome::Abandoned, NOW);
        NoteInterludeEnd(took, Goals::GoalKind::Hearth, WorldTravel::InnEndOf(true, WorldTravel::InnStep::Bind).outcome,
                         NOW + 1);
        Check(unboundStamps && RetryWaits(stuckOut, Goals::GoalKind::Hearth, NOW + 1) &&
                  !RetryWaits(took, Goals::GoalKind::Hearth, NOW + 2),
              "an unbound arrival at the inn stamps the hearth's retry time, a bind that took clears it (mutant: the "
              "unbound end arrives)");
    }

    // A plan that arrives during a fallback status is taken at once; other goal-less statuses stay
    {
        BotGoals goals;
        goals.plan = std::make_shared<Goals::GoalPlan>();
        std::const_pointer_cast<Goals::GoalPlan>(goals.plan)->ranked.ok = true;
        Check(OnFreshPlan(goals, true) == FreshPlan::Keep,
              "a goal-less status that is no fallback (sickness, a command) keeps going");
        NoteFallback(goals, Fallback::NoOption, 100);
        Check(goals.lastFallback == Fallback::NoOption && goals.lastFallbackMs == 100 &&
                  OnFreshPlan(goals, true) == FreshPlan::Take,
              "during a fallback status a usable plan is taken at once (mutant: it waits for the status's end)");
        Check(OnFreshPlan(goals, false) == FreshPlan::Keep, "a fallback status that may not be cut stays");
        BotGoals unusable = goals;
        unusable.plan = std::make_shared<Goals::GoalPlan>();
        Check(OnFreshPlan(unusable, true) == FreshPlan::Keep, "a plan without a ranking is not taken");
        NoteStatusEnd(goals, StatusEnd::Outcome::Interrupted, false);
        Check(!goals.inFallback && OnFreshPlan(goals, true) == FreshPlan::Keep,
              "any end of the fallback status ends the fallback (mutant: the flag sticks)");
        goals.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        Check(OnFreshPlan(goals, true) == FreshPlan::Replan && OnFreshPlan(goals, false) == FreshPlan::Keep,
              "a planner goal is replanned only in a status a switch may cut");
        goals.slot.Set(GoalOf(Goals::GoalKind::Grind, 1, Source::Bridge));
        Check(OnFreshPlan(goals, true) == FreshPlan::Keep, "an external goal is never switched");
    }

    // WT63e: a planned status ends by its order's condition (3.19 (4)): the end table per kind and the overrun bound
    {
        using Goals::GoalKind;
        using StatusEnd::Outcome;
        constexpr std::uint32_t START = 1000;
        constexpr std::uint32_t MINUTE = 60000;
        constexpr float MINUTES = 10.0f;
        constexpr std::uint32_t AT = START + 10 * MINUTE;
        std::uint32_t const OVER = START + static_cast<std::uint32_t>(ORDER_OVERRUN_FACTOR * MINUTES * MINUTE);
        auto const order = [](GoalKind kind, Source source = Source::Planner)
        {
            Active goal = GoalOf(kind, 1, source);
            goal.minutes = MINUTES;
            goal.startedMs = START;
            return goal;
        };
        auto const facts = [](std::uint32_t status, std::uint32_t nowMs, float health = 100.0f, float mana = 100.0f)
        {
            PlannedFacts in;
            in.status = status;
            in.nowMs = nowMs;
            in.owns = true;
            in.healthPercent = health;
            in.manaPercent = mana;
            return in;
        };
        auto const goes = [](PlannedAnswer const& a) { return a.planned && !a.ends; };
        auto const ends = [](PlannedAnswer const& a, Outcome outcome, char const* word)
        { return a.planned && a.ends && a.outcome == outcome && a.word && std::string(a.word) == word; };
        using namespace StatusEnd;

        // grind, money at a spot: the minutes at the spot, the overrun on the way
        for (GoalKind kind : {GoalKind::Grind, GoalKind::Money})
        {
            Active const goal = order(kind);
            Check(goes(PlannedEnd(goal, facts(STATUS_WANDER_RANDOM, AT - 1))) &&
                      ends(PlannedEnd(goal, facts(STATUS_WANDER_RANDOM, AT)), Outcome::Finished, "order_time"),
                  "a planned grind or money spot ends finished order_time when the order's minutes are over (mutant: "
                  "upstream's timer)");
            Check(goes(PlannedEnd(goal, facts(STATUS_GO_GRIND, AT))) &&
                      goes(PlannedEnd(goal, facts(STATUS_GO_GRIND, OVER - 1))) &&
                      ends(PlannedEnd(goal, facts(STATUS_GO_GRIND, OVER)), Outcome::TimedOut, "order_overrun"),
                  "on the way to the spot only the overrun ends it, timed out order_overrun at the factor times the "
                  "minutes");
        }
        // gather, money at a node: the minutes; the executor's own ends (finished, bags_full) stay its own
        for (GoalKind kind : {GoalKind::Gather, GoalKind::Money})
            Check(goes(PlannedEnd(order(kind), facts(STATUS_DO_GATHER, AT - 1))) &&
                      ends(PlannedEnd(order(kind), facts(STATUS_DO_GATHER, AT)), Outcome::Finished, "order_time"),
                  "a planned gather ends finished order_time when its minutes are over");
        Check(!PlannedEnd(order(GoalKind::Gather), facts(STATUS_WANDER_RANDOM, OVER)).planned &&
                  !PlannedEnd(order(GoalKind::Grind), facts(STATUS_DO_GATHER, OVER)).planned &&
                  !PlannedEnd(order(GoalKind::Grind), facts(STATUS_WANDER_NPC, OVER)).planned,
              "a status that does not carry the goal's kind out is not planned: upstream's timer");
        // rest: rested at the model's top threshold, the overrun, the sick rest's own end
        Active const rest = order(GoalKind::Rest);
        Check(ends(PlannedEnd(rest, facts(STATUS_REST, START, RESTED_PERCENT, RESTED_PERCENT)), Outcome::Finished,
                   "rested") &&
                  ends(PlannedEnd(rest, facts(STATUS_REST, START, 100.0f, -1.0f)), Outcome::Finished, "rested"),
              "a planned rest ends finished rested once health and mana (a bot without mana: health) are back at the "
              "rested threshold");
        Check(goes(PlannedEnd(rest, facts(STATUS_REST, OVER - 1, RESTED_PERCENT - 1.0f, 100.0f))) &&
                  goes(PlannedEnd(rest, facts(STATUS_REST, OVER - 1, 100.0f, RESTED_PERCENT - 1.0f))) &&
                  ends(PlannedEnd(rest, facts(STATUS_REST, OVER, 50.0f, 100.0f)), Outcome::TimedOut, "order_overrun"),
              "a rest not yet rested goes on (its minutes are no end) until the overrun bound");
        Check(RESTED_PERCENT > Goals::REST_MAX * Goals::PERCENT_F - 0.01f,
              "the rested threshold is at or above every bot's rest threshold: the option does not re-fire at once");
        PlannedFacts sick = facts(STATUS_REST, START, 100.0f, 100.0f);
        sick.sick = true;
        Check(!PlannedEnd(rest, sick).planned, "a sick bot's rest is no planned end: Upkeep::RestEnds decides");
        // the kinds whose executors end them: never PlannedEnd, in any status
        bool none = true;
        for (GoalKind kind : {GoalKind::Quest, GoalKind::MoveZone, GoalKind::TravelElsewhere, GoalKind::Errand,
                              GoalKind::BuyBag, GoalKind::Hearth})
            for (std::uint32_t status = STATUS_IDLE; status < STATUS_COUNT; ++status)
                none = none && !PlannedEnd(order(kind), facts(status, OVER)).planned;
        Check(none,
              "quest, circuit, delivery, move, travel, errand, buy_bag and hearth keep their executors' ends "
              "and timers (mutant: PlannedEnd answers them)");
        // not planned: the cohort or a held planner, a bridge goal, a fallback status, an order without minutes
        PlannedFacts cohort = facts(STATUS_WANDER_RANDOM, OVER);
        cohort.owns = false;
        PlannedFacts fallback = facts(STATUS_WANDER_RANDOM, OVER);
        fallback.inFallback = true;
        Active noMinutes = order(GoalKind::Grind);
        noMinutes.minutes = 0.0f;
        Check(!PlannedEnd(order(GoalKind::Grind), cohort).planned &&
                  !PlannedEnd(order(GoalKind::Grind, Source::Bridge), facts(STATUS_WANDER_RANDOM, OVER)).planned &&
                  !PlannedEnd(order(GoalKind::Grind), fallback).planned &&
                  !PlannedEnd(noMinutes, facts(STATUS_WANDER_RANDOM, OVER)).planned &&
                  !PlannedEnd(Active{}, facts(STATUS_WANDER_RANDOM, OVER)).planned,
              "the cohort, a held planner, a bridge goal, a fallback status and an order without minutes keep "
              "upstream's timer");
    }

    // WT63e review: an interlude's time does not count against the order it interrupts (3.19 (6)): the resumed goal's
    // clock moves on by the interlude's length (Slot::End), so a resumed order ends by its own minutes
    {
        using Goals::GoalKind;
        using namespace StatusEnd;
        constexpr std::uint32_t MINUTE = 60000;
        constexpr std::uint32_t START = 1000;
        auto const grind = [](std::uint32_t startedMs)
        {
            Active goal = GoalOf(GoalKind::Grind, 1, Source::Planner);
            goal.minutes = 10.0f;
            goal.startedMs = startedMs;
            return goal;
        };
        auto const interlude = [](GoalKind kind, std::uint32_t startedMs)
        {
            Active goal = GoalOf(kind, 0, Source::Planner);
            goal.startedMs = startedMs;
            return goal;
        };
        auto const facts = [](std::uint32_t status, std::uint32_t nowMs)
        {
            PlannedFacts in;
            in.status = status;
            in.nowMs = nowMs;
            in.owns = true;
            return in;
        };
        auto const word = [](PlannedAnswer const& a, char const* w)
        { return a.planned && a.ends && a.word && std::string(a.word) == w; };
        auto const goes = [](PlannedAnswer const& a) { return a.planned && !a.ends; };

        // a walk of 3 minutes, a 20-minute errand trip, the walk resumed
        Slot walk;
        walk.Set(grind(START));
        walk.Set(interlude(GoalKind::Errand, START + 3 * MINUTE));
        std::uint32_t const back = START + 23 * MINUTE;
        bool const resumed = walk.End(back) && walk.Query().kind == GoalKind::Grind;
        Check(resumed && walk.Query().startedMs == START + 20 * MINUTE &&
                  goes(PlannedEnd(walk.Query(), facts(STATUS_GO_GRIND, back))),
              "a walk resumed after a 20-minute errand counts only its own 3 minutes: no order_overrun at once "
              "(mutant: the clock counts the interlude)");
        std::uint32_t const over =
            START + 20 * MINUTE + static_cast<std::uint32_t>(ORDER_OVERRUN_FACTOR * 10.0f * MINUTE);
        Check(goes(PlannedEnd(walk.Query(), facts(STATUS_GO_GRIND, over - 1))) &&
                  word(PlannedEnd(walk.Query(), facts(STATUS_GO_GRIND, over)), "order_overrun"),
              "its overrun comes the interlude's length later");

        // 9 of its 10 minutes at the spot, then a bag trip and the inn in a row (20 minutes), resumed at the spot
        Slot spot;
        spot.Set(grind(START));
        spot.Set(interlude(GoalKind::BuyBag, START + 9 * MINUTE));
        spot.Set(interlude(GoalKind::Hearth, START + 15 * MINUTE));
        std::uint32_t const again = START + 29 * MINUTE;
        Check(spot.End(again) && goes(PlannedEnd(spot.Query(), facts(STATUS_WANDER_RANDOM, again))) &&
                  goes(PlannedEnd(spot.Query(), facts(STATUS_WANDER_RANDOM, again + MINUTE - 1))) &&
                  word(PlannedEnd(spot.Query(), facts(STATUS_WANDER_RANDOM, again + MINUTE)), "order_time"),
              "an order resumed after 9 of its 10 minutes ends order_time after its remaining minute; two interludes "
              "in a row count from the first one's start");

        // no interlude: the clock as set; a new goal after an interlude starts its own clock
        Slot plain;
        plain.Set(grind(START));
        Check(plain.Query().startedMs == START &&
                  word(PlannedEnd(plain.Query(), facts(STATUS_WANDER_RANDOM, START + 10 * MINUTE)), "order_time"),
              "a goal no interlude cut keeps its clock");
        plain.Set(interlude(GoalKind::Errand, START + MINUTE));
        plain.Set(grind(START + 2 * MINUTE));
        plain.Set(interlude(GoalKind::Rest, START + 3 * MINUTE));
        Check(plain.End(START + 5 * MINUTE) && plain.Query().startedMs == START + 4 * MINUTE,
              "a new goal after an interlude: only the interlude that cut it moves its clock");
        Check(!walk.Bounded() && !spot.Bounded() && !plain.Bounded(),
              "interludes within the bound (20 minutes in all, the errand deadline) hit no bound");

        // WT63e-d, the general bound: an order's interludes move its clock at most INTERLUDE_SHIFT_MAX_MS in all
        Slot past;
        past.Set(grind(START));
        past.Set(interlude(GoalKind::Errand, START + 3 * MINUTE));
        std::uint32_t const late = START + 3 * MINUTE + INTERLUDE_SHIFT_MAX_MS + 5 * MINUTE;
        Check(past.End(late) && past.Bounded() && past.Query().startedMs == START + INTERLUDE_SHIFT_MAX_MS &&
                  goes(PlannedEnd(past.Query(), facts(STATUS_GO_GRIND, late))),
              "an interlude 5 minutes past the bound: those 5 minutes count against the order (own 8 of 10 minutes)");
        Slot many;
        many.Set(grind(START));
        std::uint32_t at = START + MINUTE;
        bool hit = false;
        for (int i = 0; i < 1000; ++i)
        {
            many.Set(interlude(i % 2 ? GoalKind::Rest : GoalKind::Errand, at));
            at += 20 * MINUTE;
            bool const resumed = many.End(at);
            hit = hit || (resumed && many.Bounded());
        }
        Check(hit && many.Query().kind == GoalKind::Grind && many.MinutesSince(at) >= ORDER_OVERRUN_FACTOR * 10.0f &&
                  word(PlannedEnd(many.Query(), facts(STATUS_WANDER_RANDOM, at)), "order_time") &&
                  word(PlannedEnd(many.Query(), facts(STATUS_GO_GRIND, at)), "order_overrun"),
              "1000 interludes of 20 minutes with no own time between them: the order ends, order_time at the spot "
              "and order_overrun on the walk (mutant: no bound, 1.00 own minute, never ends)");
        many.Set(grind(at));
        many.Set(interlude(GoalKind::Errand, at + MINUTE));
        Check(many.End(at + 21 * MINUTE) && !many.Bounded() && many.Query().startedMs == at + 20 * MINUTE,
              "a new goal gets the whole bound again");

        // the interlude does not stall the goal it interrupts: the resumed walk runs on
        BotGoals goals;
        goals.slot.Set(grind(START));
        BeginRun(goals, 40, 5, 100, true);
        NoteStatusEnd(goals, Outcome::Interrupted, false);  // the planner's switch ends the walk for the errand
        goals.slot.Set(interlude(GoalKind::Errand, START + 3 * MINUTE));
        NoteStatusEnd(goals, Outcome::Arrived, true);  // the errand trip's own end
        goals.slot.End(back);
        ContinueRun(goals);
        PlannedAnswer const first = PlannedEnd(goals.slot.Query(), facts(STATUS_GO_GRIND, back));
        if (first.ends)  // as the status core would: a judged end, then the run's end
        {
            NoteStatusEnd(goals, first.outcome, true, first.word);
            EndRun(goals, 5, 100);
        }
        Check(!first.ends && goals.run.running && !goals.stalled && !goals.stallRow,
              "the interrupted walk resumes and runs on: no judged end at once, no stall without XP (mutant: "
              "order_overrun at the resume stalls the key)");
    }

    // WT63e: the hearth's ends (WT63d-b) and the retry stamps per interlude kind (3.19 (6)); a bag trip without its bag
    {
        using Goals::GoalKind;
        using StatusEnd::Outcome;
        constexpr std::uint32_t NOW = 5000;
        WorldTravel::InnEnd const bound = WorldTravel::InnEndOf(true, WorldTravel::InnStep::Bind);
        WorldTravel::InnEnd const unbound = WorldTravel::InnEndOf(false, WorldTravel::InnStep::Bind);
        WorldTravel::InnEnd const stuck = WorldTravel::InnEndOf(false, WorldTravel::InnStep::Fail);
        Check(bound.outcome == Outcome::Arrived && !bound.avoid && unbound.outcome == Outcome::Abandoned &&
                  unbound.avoid && stuck.outcome == Outcome::Stuck && !stuck.avoid,
              "the hearth's ends: bound arrived, unbound abandoned (the inn avoided), stuck stuck (not avoided)");
        auto const stamps = [](GoalKind kind, Outcome outcome, bool bagBought)
        {
            BotGoals goals;
            goals.bagBought = bagBought;
            NoteInterludeEnd(goals, kind, outcome, NOW);
            return RetryWaits(goals, kind, NOW + 1);
        };
        auto const clears = [](GoalKind kind, Outcome outcome, bool bagBought)
        {
            BotGoals goals;
            goals.failedMs[static_cast<std::size_t>(kind)] = NOW;
            goals.bagBought = bagBought;
            NoteInterludeEnd(goals, kind, outcome, NOW + 1);
            return !RetryWaits(goals, kind, NOW + 2);
        };
        bool table = true;
        for (GoalKind kind : {GoalKind::Errand, GoalKind::BuyBag, GoalKind::Hearth})
        {
            for (Outcome failure : {Outcome::TimedOut, Outcome::Stuck, Outcome::Abandoned})
                table = table && stamps(kind, failure, true);
            table = table && clears(kind, Outcome::Arrived, true) && clears(kind, Outcome::Finished, true);
            for (Outcome outside : {Outcome::Interrupted, Outcome::Ordered, Outcome::Sick})
                table = table && !stamps(kind, outside, false) && !clears(kind, outside, true);
        }
        Check(table,
              "errand, buy_bag and hearth stamp on timed out, stuck or abandoned, clear on arrived or "
              "finished; an end from outside changes nothing");
        Check(stamps(GoalKind::BuyBag, Outcome::Finished, false) && !stamps(GoalKind::Errand, Outcome::Finished, false),
              "a bag trip that finished without its bag stamps the buy_bag retry (mutant: any finish clears)");
        // NE2 review: an errand that finished with its repair still due (a repairer dead or gone, a stand point off
        // the mesh) waits out the retry time, so the due repair, which ranks first, does not start the same trip again
        // at once; with the repair done it clears; an end from outside and a bag trip read nothing of it
        auto const repairEnd = [](GoalKind kind, Outcome outcome, bool repairDue, std::uint32_t at)
        {
            BotGoals goals;
            goals.bagBought = true;
            NoteInterludeEnd(goals, kind, outcome, NOW, repairDue);
            return RetryWaits(goals, kind, at);
        };
        constexpr std::uint32_t RETRY = WorldTravel::ERRAND_RETRY_MS;
        BotGoals repaired;
        repaired.failedMs[static_cast<std::size_t>(GoalKind::Errand)] = NOW;
        NoteInterludeEnd(repaired, GoalKind::Errand, Outcome::Finished, NOW + 1, false);
        Check(repairEnd(GoalKind::Errand, Outcome::Finished, true, NOW + RETRY - 1) &&
                  !repairEnd(GoalKind::Errand, Outcome::Finished, true, NOW + RETRY) &&
                  !RetryWaits(repaired, GoalKind::Errand, NOW + 2),
              "an errand that finished with its repair still due waits out the retry time, one that repaired clears "
              "it (mutant: any finished errand clears, the due repair restarts the trip at once)");
        Check(!repairEnd(GoalKind::Errand, Outcome::Interrupted, true, NOW + 1) &&
                  !repairEnd(GoalKind::BuyBag, Outcome::Finished, true, NOW + 1),
              "a due repair stamps only a finished errand: an end from outside and a bag trip with its bag stamp "
              "nothing");
        bool rest = true;
        for (Outcome outcome : {Outcome::TimedOut, Outcome::Stuck, Outcome::Abandoned, Outcome::Finished})
            rest = rest && !stamps(GoalKind::Rest, outcome, false);
        Check(rest, "a rest stamps nothing: its order_overrun is no failure to retry");
    }

    // WT63e: the run keeps its last judged outcome and note; a stall's row carries them (3.19 (6))
    {
        using Goals::GoalKind;
        using StatusEnd::Outcome;
        constexpr std::uint64_t KEY = 9;
        constexpr std::uint64_t LOG = 7;
        auto const start = [](BotGoals& goals, GoalKind kind)
        {
            goals.slot.Set(GoalOf(kind, 1, Source::Planner));
            return StartActive(goals, kind, Executor::Grind, {KEY, LOG, 100}, [] { return true; });
        };
        BotGoals judged;
        start(judged, GoalKind::Grind);
        NoteStatusEnd(judged, Outcome::Arrived, false);
        NoteStatusEnd(judged, Outcome::Finished, true, "order_time");
        EndRun(judged, LOG, 100);
        Check(judged.stallRow && judged.stallRow->kind == GoalKind::Grind && judged.stallRow->judged &&
                  judged.stallRow->outcome == Outcome::Finished && std::string(judged.stallRow->note) == "order_time",
              "a judged end with the log as at the start stalls, and the stall's row carries the kind, the last "
              "outcome and its note");
        BotGoals defaulted;
        start(defaulted, GoalKind::Grind);
        NoteStatusEnd(defaulted, Outcome::Stuck, true);
        EndRun(defaulted, LOG, 100);
        Check(defaulted.stallRow && std::string(defaulted.stallRow->note) == "stuck",
              "an end without a note carries the outcome's word");
        bool outside = true;
        for (Outcome from : {Outcome::Interrupted, Outcome::Ordered, Outcome::Sick})
        {
            BotGoals goals;
            start(goals, GoalKind::Grind);
            NoteStatusEnd(goals, Outcome::TimedOut, false, "timeout");
            NoteStatusEnd(goals, from, true);
            outside = outside && goals.run.judged && goals.run.lastOutcome == Outcome::TimedOut;
            EndRun(goals, LOG, 100);
            outside = outside && !goals.stallRow && !goals.stalled;
        }
        Check(outside,
              "an end from outside (interrupted, ordered, sick) stalls nothing and keeps the earlier judged "
              "outcome");
        BotGoals interlude;
        start(interlude, GoalKind::Grind);
        NoteStatusEnd(interlude, Outcome::Arrived, false, "arrived");
        interlude.slot.Set(GoalOf(GoalKind::Errand, 2, Source::Planner));
        NoteStatusEnd(interlude, Outcome::Finished, true, "finished");
        Check(interlude.run.lastOutcome == Outcome::Arrived && std::string(interlude.run.lastNote) == "arrived",
              "an interlude's end is not the interrupted goal's outcome");
        BotGoals progressed;
        start(progressed, GoalKind::Grind);
        NoteStatusEnd(progressed, Outcome::Finished, true, "order_time");
        EndRun(progressed, LOG, 150);
        BotGoals unjudged;
        start(unjudged, GoalKind::Quest);
        EndRun(unjudged, LOG);
        Check(!progressed.stallRow && unjudged.stallRow && !unjudged.stallRow->judged,
              "XP gained writes no stall; a run that ended with no judged end stalls with no outcome");
    }

    // WT63e: the planner's bag order buys once (WT63c review: an order bought at every stop) and alone
    {
        BotGoals goals;
        Active bag = GoalOf(Goals::GoalKind::BuyBag, 4498, Source::Planner);
        goals.slot.Set(bag);
        bool const ordered = OrderedBagOf(goals) == 4498;
        goals.bagBought = true;
        Check(ordered && OrderedBagOf(goals) == 0,
              "after its purchase the order names no bag (mutant: every stop "
              "buys)");
        BotGoals bridge;
        bridge.slot.Set(GoalOf(Goals::GoalKind::BuyBag, 4498, Source::Bridge));
        BotGoals grind;
        grind.slot.Set(GoalOf(Goals::GoalKind::Grind, 4498, Source::Planner));
        Check(OrderedBagOf(bridge) == 0 && OrderedBagOf(grind) == 0, "only the planner's bag purchase orders a bag");
    }

    std::printf("goal apply selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
