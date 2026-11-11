/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GOALINTERFACE_H
#define _PLAYERBOT_GOALINTERFACE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "GoalModel.h"

// WT16: the goals interface (3.12) and the rules for applying the planner's answers. Player-free, so the offline
// selftest (tests/goal-apply-selftest.cpp) checks them; PlannerService.cpp applies them on the bot's map thread.
// A goal has a source: the planner's own, or an external one (the bridge, WT18; a group). An external goal outranks
// the planner's until it is done or cancelled. The A/B group (WT.21) keeps today's weighted pick: guid % 10 = 0, the
// rule of the journal's cohort ab-weighted (its complement, ab-planner, gets planner goals).
namespace Planner::GoalApply
{
constexpr std::uint64_t AB_MODULUS = 10;
constexpr std::uint32_t PLAN_WAIT_MS = 10000;    // an idle planner bot waits this long for a late plan, then falls back
constexpr std::uint32_t MIN_SNAPSHOT_MS = 5000;  // a planner bot's jobs are at least this far apart
constexpr float GIVER_YARDS = 40.0f;  // nearer than this to the circuit's start the bot looks for givers there

// The weighted group: the character guid's low part, as dim_bots' guid
inline bool InWeightedGroup(std::uint64_t guidLow) { return guidLow % AB_MODULUS == 0; }

enum class Source : std::uint8_t
{
    Planner,
    Bridge,
    Group,
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Source::Count)> SOURCE_WORDS = {"planner", "bridge",
                                                                                           "group"};

inline char const* SourceWord(Source source)
{
    return source < Source::Count ? SOURCE_WORDS[static_cast<std::size_t>(source)] : "planner";
}

inline bool IsExternal(Source source) { return source != Source::Planner; }

// Why a planner bot took the weighted pick: the reason of its rpg_status row (None: a weighted bot's "weights")
enum class Fallback : std::uint8_t
{
    None,
    Off,       // the test switch Planner.Goals is off
    NotReady,  // the indexes or the quest index are not built yet
    Failed,    // the planner answered without a ranking, or its job failed or was lost
    NoPlan,    // no plan came, or none of its options could start
    Late,      // the bot waited PLAN_WAIT_MS for its job's answer
    Rejected,  // the answer failed its recheck (state, age, place, ticket or scope)
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Fallback::Count)> FALLBACK_WORDS = {
    "weights",          "fallback_off",  "fallback_not_ready", "fallback_failed",
    "fallback_no_plan", "fallback_late", "fallback_rejected"};

inline char const* FallbackWord(Fallback reason)
{
    return reason < Fallback::Count ? FALLBACK_WORDS[static_cast<std::size_t>(reason)] : "weights";
}

// A goal in a bot's slot
struct Active
{
    Goals::GoalKind kind{Goals::GoalKind::None};
    std::uint32_t target{0};
    Goals::TargetType targetType{Goals::TargetType::Default};
    Place place;
    Source source{Source::Planner};
    std::string reason;
    float valuePerMinute{0.0f};
    float minutes{0.0f};
    std::uint32_t startedMs{0};

    bool Is() const { return kind != Goals::GoalKind::None; }
};

inline Active FromOption(Goals::Option const& option, Source source, std::uint32_t nowMs)
{
    Active goal;
    goal.kind = option.kind;
    goal.target = option.target;
    goal.targetType = option.targetType;
    goal.place = option.place;
    goal.source = source;
    goal.reason = option.reason;
    goal.valuePerMinute = option.valuePerMinute;
    goal.minutes = option.minutes;
    goal.startedMs = nowMs;
    return goal;
}

// Precedence: an external goal is replaced only by another external one; the planner's by anyone
inline bool MayReplace(Active const& current, Source incoming)
{
    return !current.Is() || IsExternal(incoming) || !IsExternal(current.source);
}

// One bot's goal: set, query, progress, cancel (3.12). An interlude (errand, bag, rest, hearth) keeps the goal it
// interrupts; when the interlude ends that goal goes on
class Slot
{
public:
    // False when a goal of a higher source holds the slot
    bool Set(Active goal)
    {
        if (!MayReplace(_current, goal.source))
            return false;
        if (Goals::IsInterlude(goal.kind) && _current.Is() && !Goals::IsInterlude(_current.kind))
            _previous = _current;
        else if (!Goals::IsInterlude(goal.kind))
            _previous.reset();
        _current = std::move(goal);
        return true;
    }

    Active const& Query() const { return _current; }

    // Minutes since the goal started, 0 without one
    float MinutesSince(std::uint32_t nowMs) const
    {
        constexpr float MS_PER_MINUTE = 60000.0f;
        return _current.Is() ? static_cast<float>(nowMs - _current.startedMs) / MS_PER_MINUTE : 0.0f;
    }

    // The goal of this source ends; the planner never cancels an external goal. True when one ended
    bool Cancel(Source source)
    {
        if (!_current.Is() || (IsExternal(_current.source) && !IsExternal(source)))
            return false;
        _current = {};
        _previous.reset();
        return true;
    }

    // The goal's executor ended. True when an interlude ended and the goal it interrupted goes on
    bool End()
    {
        if (_previous)  // set only while an interlude holds the slot
        {
            _current = std::move(*_previous);
            _previous.reset();
            return true;
        }
        _current = {};
        _previous.reset();
        return false;
    }

    bool HasExternal() const { return _current.Is() && IsExternal(_current.source); }

private:
    Active _current;
    std::optional<Active> _previous;
};

// What starts a goal of this kind: today's statuses are the executors. None: no executor reachable from the planner
// yet (an errand, a bag or a hearth start on their own triggers, WT15), or nothing to do from here
enum class Executor : std::uint8_t
{
    None,
    Circuit,  // the local circuit: its next task or turn-in to DO_QUEST, else its givers
    Deliver,  // DO_QUEST on the goal's complete quest: it walks to the ender
    Grind,
    Gather,
    Travel,
    Rest
};

// What a quest goal names (GoalModel): the circuit at the bot's own hub, a carried quest's delivery to its ender, or
// a move to another hub or camp in the bot's zone (how a starting area hands on to its next hub)
enum class QuestRole : std::uint8_t
{
    Circuit,
    Delivery,
    Move
};

inline QuestRole RoleOf(Goals::TargetType targetType, std::string const& reason)
{
    if (targetType == Goals::TargetType::Quest)
        return QuestRole::Delivery;
    return reason == Goals::CIRCUIT_REASON ? QuestRole::Circuit : QuestRole::Move;
}

// Where a goal's place and quest are for the bot, read on its map thread
struct Reach
{
    bool deliverable{false};  // its quest is complete; on its map: the circuit's turn-in test (not low priority, a POI)
    bool sameMap{false};      // its place is on the bot's map
    bool there{false};        // its place is within GIVER_YARDS
};

inline Executor ExecutorFor(Active const& goal, Reach const& reach)
{
    switch (goal.kind)
    {
        case Goals::GoalKind::Quest:
            switch (RoleOf(goal.targetType, goal.reason))
            {
                case QuestRole::Circuit:
                    return Executor::Circuit;
                case QuestRole::Delivery:
                    if (!reach.deliverable)
                        return Executor::None;  // nothing left to deliver
                    return reach.sameMap ? Executor::Deliver : Executor::Travel;
                default:  // the next plan there offers its circuit
                    return reach.there ? Executor::None : Executor::Travel;
            }
        case Goals::GoalKind::Grind:
            return Executor::Grind;
        case Goals::GoalKind::Money:
            return goal.targetType == Goals::TargetType::Node ? Executor::Gather : Executor::Grind;
        case Goals::GoalKind::Gather:
            return Executor::Gather;
        case Goals::GoalKind::MoveZone:
        case Goals::GoalKind::TravelElsewhere:
            return Executor::Travel;
        case Goals::GoalKind::Rest:
            return Executor::Rest;
        default:
            return Executor::None;
    }
}

// The goal runs the local circuit (Executor::Circuit wherever the bot is)
inline bool RunsCircuit(Active const& goal)
{
    return goal.kind == Goals::GoalKind::Quest && RoleOf(goal.targetType, goal.reason) == QuestRole::Circuit;
}

// The ticket a job's result must carry: the pool's sequence returned by Submit. 0 = no job out
inline bool TicketMatches(std::uint64_t resultTicket, std::uint64_t expected)
{
    return expected != 0 && resultTicket == expected;
}

// What an idle bot does
enum class IdleAction : std::uint8_t
{
    Weights,   // a weighted-group bot: today's pick
    Apply,     // a fresh plan is in hand
    Wait,      // its plan is coming
    Fallback,  // today's pick, with the reason
};

struct IdleInput
{
    bool weighted{false};
    bool switchOn{true};
    bool ready{false};           // the indexes and the quest index
    bool freshPlan{false};       // an unused plan within the stale limit
    bool jobComing{false};       // a job is out or due
    std::uint32_t idleForMs{0};  // since the bot went idle without a plan
    bool lastRejected{false};
    bool lastFailed{false};  // the last job failed or was lost
};

struct IdleDecision
{
    IdleAction action{IdleAction::Weights};
    Fallback reason{Fallback::None};
};

inline IdleDecision DecideIdle(IdleInput const& in)
{
    if (in.weighted)
        return {IdleAction::Weights, Fallback::None};
    if (!in.switchOn)
        return {IdleAction::Fallback, Fallback::Off};
    if (!in.ready)
        return {IdleAction::Fallback, Fallback::NotReady};
    if (in.freshPlan)
        return {IdleAction::Apply, Fallback::None};
    if (in.jobComing)
        return in.idleForMs < PLAN_WAIT_MS ? IdleDecision{IdleAction::Wait, Fallback::None}
                                           : IdleDecision{IdleAction::Fallback, Fallback::Late};
    if (in.lastFailed)
        return {IdleAction::Fallback, Fallback::Failed};
    return {IdleAction::Fallback, in.lastRejected ? Fallback::Rejected : Fallback::NoPlan};
}

// The plan's quests the bot keeps in its log (OrganizeQuestLog): taken, deferred, carried, worked or turned in on this
// loop, minus its abandons; sorted. Follow-ups are not in it: the bot's givers logic takes them, not the plan
inline std::vector<std::uint32_t> KeepList(CircuitPlan const& plan)
{
    std::vector<std::uint32_t> keep;
    for (CircuitTake const& take : plan.take)
        keep.push_back(take.quest);
    keep.insert(keep.end(), plan.deferred.begin(), plan.deferred.end());
    keep.insert(keep.end(), plan.carried.begin(), plan.carried.end());
    for (CircuitStop const& stop : plan.stops)
        for (CircuitTask const& task : stop.tasks)
            keep.push_back(task.quest);
    for (CircuitTurnIn const& turnIn : plan.turnIns)
        keep.push_back(turnIn.quest);
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    for (CircuitAbandon const& abandon : plan.abandon)
    {
        auto const it = std::lower_bound(keep.begin(), keep.end(), abandon.quest);
        if (it != keep.end() && *it == abandon.quest)
            keep.erase(it);
    }
    return keep;
}

inline bool Keeps(std::vector<std::uint32_t> const& keep, std::uint32_t quest)
{
    return std::binary_search(keep.begin(), keep.end(), quest);
}

// The circuit's next task in walking order whose quest the bot can work now
inline CircuitTask const* NextTask(CircuitPlan const& plan, std::function<bool(std::uint32_t)> const& workable)
{
    for (CircuitStop const& stop : plan.stops)
        for (CircuitTask const& task : stop.tasks)
            if (workable(task.quest))
                return &task;
    return nullptr;
}

// Of the circuit's turn-ins whose quest is complete, the first in its ender camps' visiting order (enderCamps); a
// camp not in that list comes last
inline CircuitTurnIn const* NextTurnIn(CircuitPlan const& plan, std::function<bool(std::uint32_t)> const& complete)
{
    CircuitTurnIn const* best = nullptr;
    std::size_t bestRank = 0;
    for (CircuitTurnIn const& turnIn : plan.turnIns)
    {
        if (!complete(turnIn.quest))
            continue;
        std::size_t const rank = static_cast<std::size_t>(
            std::find(plan.enderCamps.begin(), plan.enderCamps.end(), turnIn.camp) - plan.enderCamps.begin());
        if (!best || rank < bestRank)
        {
            best = &turnIn;
            bestRank = rank;
        }
    }
    return best;
}

// A quest goal's own progress, kept across plans while the goal goes on: a plan comes at every idle (5 s apart at
// least), so a per-plan limit would repeat its quests and its givers visit without end
struct QuestGoal
{
    std::vector<std::uint32_t> started;  // quests it started, each once
    bool giversVisited{false};           // it sent the bot to its circuit's givers (once)
};

enum class QuestStep : std::uint8_t
{
    Done,    // nothing left: the goal ends
    Task,    // DO_QUEST on an incomplete quest's objective
    TurnIn,  // DO_QUEST on a complete quest: it walks to the ender
    Givers   // the circuit's start, to take its quests
};

struct QuestNext
{
    QuestStep step{QuestStep::Done};
    std::uint32_t quest{0};
};

// A quest goal's next step: the circuit's next task in walking order, else a completed quest's turn-in in ender camp
// order, else once per goal its givers (while the circuit has takes or turn-ins). Records what it starts
inline QuestNext AdvanceQuest(QuestGoal& goal, CircuitPlan const& plan,
                              std::function<bool(std::uint32_t)> const& workable,
                              std::function<bool(std::uint32_t)> const& completed)
{
    auto const fresh = [&goal](std::uint32_t quest)
    { return std::find(goal.started.begin(), goal.started.end(), quest) == goal.started.end(); };
    if (CircuitTask const* task =
            NextTask(plan, [&fresh, &workable](std::uint32_t quest) { return fresh(quest) && workable(quest); }))
    {
        goal.started.push_back(task->quest);
        return {QuestStep::Task, task->quest};
    }
    if (CircuitTurnIn const* turnIn =
            NextTurnIn(plan, [&fresh, &completed](std::uint32_t quest) { return fresh(quest) && completed(quest); }))
    {
        goal.started.push_back(turnIn->quest);
        return {QuestStep::TurnIn, turnIn->quest};
    }
    if (goal.giversVisited || (plan.take.empty() && plan.turnIns.empty()))
        return {};
    goal.giversVisited = true;
    return {QuestStep::Givers, 0};
}

// The quest log and the level as one number (FNV-1a), a goal's progress: a quest taken, completed, turned in or
// abandoned, an objective counted, a quest item collected (items: the log quests' item counts) or a level gained
// changes it. XP is not in it: a kill on the way would lift every stall
inline std::uint64_t LogSignature(std::vector<QuestSlot> const& quests, std::uint32_t level,
                                  std::vector<std::uint16_t> const& items)
{
    constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;
    std::uint64_t hash = FNV_OFFSET;
    auto const mix = [&hash](std::uint64_t value) { hash = (hash ^ value) * FNV_PRIME; };
    mix(level);
    for (QuestSlot const& slot : quests)
    {
        mix(slot.quest);
        mix(slot.state);
        for (std::uint16_t const counter : slot.counters)
            mix(counter);
    }
    for (std::uint16_t const count : items)
        mix(count);
    return hash;
}

// The stall rule, one for every goal the planner starts (a circuit, a delivery, a move, a grind, any later kind) but
// the interludes, which hand the slot back to the goal they interrupted: a goal has a key; when its executor ends
// with the log signature as it was at the goal's start, that key stalls until the signature changes, and options
// with a stalled key do not start, so the plan's next option or the counted fallback runs. It ends every "start,
// fail, start again" loop: takes the givers refuse, a delivery without a reward POI or one that failed at the ender,
// a trip without a route, a move between two dry hubs.
//
// The local circuit's key: its hub, else its home camp (a circuit outside every hub's core), never equal to a hub's
// key
constexpr std::uint64_t CAMP_KEY = 1ULL << 32;

inline std::uint64_t CircuitKey(Goals::GoalPlan const& plan)
{
    return plan.hub != NO_HUB ? plan.hub : CAMP_KEY | plan.circuit.homeCamp;
}

// A goal's key: the local circuit's for every goal that runs it, whatever its option named; for any other goal its
// executor, kind, target type and target, so a delivery's trip to the ender's map and its turn-in there stall apart.
// Never equal to a circuit's key (its kind and executor sit at bit 48 and above)
inline std::uint64_t GoalKey(Active const& goal, Executor executor, std::uint64_t circuitKey)
{
    if (executor == Executor::Circuit)
        return circuitKey;
    return (static_cast<std::uint64_t>(executor) << 56) | (static_cast<std::uint64_t>(goal.kind) << 48) |
           (static_cast<std::uint64_t>(goal.targetType) << 40) | goal.target;
}

// The keys whose goals ended without progress, all for one log signature: a changed signature lifts them all
constexpr std::size_t MAX_STALLED = 8;

struct Stalled
{
    std::uint64_t log{0};
    std::vector<std::uint64_t> keys;  // GoalKey, oldest first
};

// The goal in the slot (no interlude) while its executor runs: its key and the log signature when it started
struct Run
{
    std::uint64_t key{0};
    std::uint64_t startLog{0};
    bool running{false};
};

// Of the quest's points of interest the one nearest to any of the plan's tasks for it; -1 when the plan has none
inline int NearestToTasks(CircuitPlan const& plan, std::uint32_t quest,
                          std::vector<std::pair<float, float>> const& points)
{
    int best = -1;
    float bestSquared = 0.0f;
    for (CircuitStop const& stop : plan.stops)
        for (CircuitTask const& task : stop.tasks)
        {
            if (task.quest != quest)
                continue;
            for (std::size_t i = 0; i < points.size(); ++i)
            {
                float const dx = points[i].first - task.x;
                float const dy = points[i].second - task.y;
                float const squared = dx * dx + dy * dy;
                if (best < 0 || squared < bestSquared)
                {
                    best = static_cast<int>(i);
                    bestSquared = squared;
                }
            }
        }
    return best;
}

// The quests in the log with the time they were first seen there (the accept time the snapshot lacks), sorted
using SeenQuests = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

inline void UpdateSeen(SeenQuests& seen, std::vector<std::uint32_t> log, std::uint32_t nowMs)
{
    std::sort(log.begin(), log.end());
    SeenQuests next;
    next.reserve(log.size());
    for (std::uint32_t const quest : log)
    {
        auto const it = std::lower_bound(seen.begin(), seen.end(), std::make_pair(quest, std::uint32_t{0}));
        next.emplace_back(quest, it != seen.end() && it->first == quest ? it->second : nowMs);
    }
    seen = std::move(next);
}

inline std::vector<QuestProgress> ProgressOf(SeenQuests const& seen, std::uint32_t nowMs)
{
    constexpr float MS_PER_MINUTE = 60000.0f;
    std::vector<QuestProgress> progress;
    progress.reserve(seen.size());
    for (auto const& [quest, sinceMs] : seen)
        progress.push_back({quest, static_cast<float>(nowMs - sinceMs) / MS_PER_MINUTE, 0});
    return progress;
}

// The ranking with the started option first: the goal row's choice is what the bot does, the others its alternatives
inline Goals::Ranked ChosenFirst(Goals::Ranked ranked, std::size_t index)
{
    if (index > 0 && index < ranked.options.size())
        std::rotate(ranked.options.begin(), ranked.options.begin() + static_cast<std::ptrdiff_t>(index),
                    ranked.options.begin() + static_cast<std::ptrdiff_t>(index) + 1);
    return ranked;
}

// A bot's planner side on its map thread (NewRpgInfo::PlannerState::goals). A reset of the AI starts it anew
struct BotGoals
{
    Slot slot;
    std::shared_ptr<Goals::GoalPlan const> plan;  // the newest plan taken
    std::uint32_t planMs{0};                      // its snapshot's time; 0 = none
    std::uint32_t lastPlanMs{0};                  // the plan before it
    bool planUsed{false};                         // a plan starts at most one new goal
    std::vector<std::uint32_t> keep;              // the plan's KeepList
    QuestGoal quest;                              // the current circuit goal's progress, across plans
    Run run;                                      // the current goal's run (the stall rule)
    std::optional<Stalled> stalled;               // keys whose goals ended without progress
    bool circuitLogged{false};                    // this plan's circuit rows are written
    std::optional<Goals::Option> switchTo;        // a replan's choice, started at the next idle
    std::uint64_t ticket{0};                      // the job out; its result must carry it
    std::uint32_t idleSinceMs{0};                 // waiting for a plan since; 0 = not waiting
    bool waitCounted{false};
    bool lastRejected{false};
    bool lastFailed{false};
    Fallback lastFallback{Fallback::None};
    std::uint32_t lastFallbackMs{0};
    bool situationChanged{false};  // level, zone, life or group changed since the last plan
    SeenQuests seen;
    std::optional<Goals::External> external;  // an external goal for the next job (WT18 sets it)
    Source externalSource{Source::Bridge};
};

// A checked plan replaces the old one. The current goal's own progress (QuestGoal) stays
inline void TakePlan(BotGoals& goals, std::shared_ptr<Goals::GoalPlan const> plan, std::uint32_t planMs)
{
    goals.lastRejected = false;
    goals.lastFailed = false;
    goals.lastPlanMs = goals.planMs;
    goals.plan = std::move(plan);
    goals.planMs = planMs;
    goals.planUsed = false;
    goals.keep = goals.plan ? KeepList(goals.plan->circuit) : std::vector<std::uint32_t>();
    goals.circuitLogged = false;
}

// A goal with this key may start: not while its key stalled and the log is still as it was then
inline bool MayStart(BotGoals const& goals, std::uint64_t key, std::uint64_t log)
{
    return !goals.stalled || goals.stalled->log != log ||
           std::find(goals.stalled->keys.begin(), goals.stalled->keys.end(), key) == goals.stalled->keys.end();
}

// A goal (no interlude) starts: its run and its circuit progress start anew
inline void BeginRun(BotGoals& goals, std::uint64_t key, std::uint64_t log)
{
    goals.quest = {};
    goals.run = {key, log, true};
}

// The running goal's executor ended: its key stalls when the log is as it was at the goal's start; a changed log
// lifts every stall
inline void EndRun(BotGoals& goals, std::uint64_t log)
{
    if (!goals.run.running)
        return;
    goals.run.running = false;
    if (log != goals.run.startLog)
    {
        goals.stalled.reset();
        return;
    }
    if (!goals.stalled || goals.stalled->log != log)
        goals.stalled = Stalled{log, {}};
    std::vector<std::uint64_t>& keys = goals.stalled->keys;
    if (std::find(keys.begin(), keys.end(), goals.run.key) != keys.end())
        return;
    if (keys.size() >= MAX_STALLED)
        keys.erase(keys.begin());
    keys.push_back(goals.run.key);
}
}  // namespace Planner::GoalApply

#endif
