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
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "BridgeGoals.h"
#include "FieldMap.h"
#include "GatherRules.h"
#include "GoalModel.h"
#include "InterludeInputs.h"
#include "QuestFailRows.h"
#include "QuestLogRules.h"
#include "SkillPathTypes.h"
#include "StatusEndRules.h"

// WT16: the goals interface (3.12) and the rules for applying the planner's answers. Player-free, so the offline
// selftest (tests/goal-apply-selftest.cpp) checks them; PlannerService.cpp applies them on the bot's map thread.
// A goal has a source: the planner's own, or an external one (the bridge, WT18). An external goal outranks
// the planner's until it is done or cancelled. The planner leads every bot that keeps its possessions and whose
// planner no test holds (Owns; questing WQ.39); any other bot keeps upstream's pick, accept and drop.
namespace Planner::GoalApply
{
constexpr std::uint32_t PLAN_WAIT_MS = 10000;    // an idle planner bot waits this long for a late plan, then falls back
constexpr std::uint32_t MIN_SNAPSHOT_MS = 5000;  // a planner bot's jobs are at least this far apart
// A planner bot's fallback status (3.19 (5)) lasts this long unless a usable plan ends it first (WT.40: tuning)
constexpr std::uint32_t FALLBACK_STATUS_MS = 120000;
// A planner bot's no-option answers in a row, with no goal started between them, from which on its fallback leaves
// out rest (BoundNoOption, planning WN4: a tuning value; about three to ten minutes of rests at one spot)
constexpr std::uint8_t NO_OPTION_BOUND = 5;
// A planned status (PlannedEnd) ends timed out once its order ran this many times its minutes (WT.40: a tuning value)
constexpr float ORDER_OVERRUN_FACTOR = 2.0f;
// The interludes of one order move its clock on by at most this much in all (Slot::End, 3.19 (6)); their time beyond
// counts against the order, so interludes chosen again and again cannot keep it from ending (WT63e-d: the general
// bound, one errand trip's deadline; WT.40: a tuning value)
constexpr std::uint32_t INTERLUDE_SHIFT_MAX_MS = WorldTravel::ERRAND_DEADLINE_MS;
// A planned rest ends rested with health and mana at least here: the top of every bot's rest threshold
// (Goals::RestThreshold), so the option that made the rest does not come back at once (WT.40)
constexpr float RESTED_PERCENT = Goals::REST_MAX * Goals::PERCENT_F;
constexpr float GIVER_YARDS = 40.0f;  // nearer than this to the circuit's start the bot looks for givers there

// A planner bot (3.19): it keeps its possessions (the master term is the caller's, PlayerbotAI::KeepsPossessions)
// and its planner is not held by a test (TestConstraints' planner row). Only the planner starts what such a bot does;
// a bot that keeps no possessions (not persistent, a master at a client) or whose planner a test holds keeps
// upstream's behaviour. A bot that keeps its possessions is governed (upkeep 3.11): its class quests get the unlock
// bonus (ClassQuestRules.h)
inline bool Owns(bool keepsPossessions, bool plannerFree) { return keepsPossessions && plannerFree; }

enum class Source : std::uint8_t
{
    Planner,
    Bridge,
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Source::Count)> SOURCE_WORDS = {"planner", "bridge"};

inline char const* SourceWord(Source source)
{
    return source < Source::Count ? SOURCE_WORDS[static_cast<std::size_t>(source)] : "planner";
}

inline bool IsExternal(Source source) { return source != Source::Planner; }

// Why a planner bot took the fallback pick: the reason of its rpg_status row (None: "weights", upstream's pick of a
// bot the planner does not lead)
enum class Fallback : std::uint8_t
{
    None,
    NotReady,       // the indexes or the quest index are not built yet
    Failed,         // the planner answered without a ranking, or its job failed or was lost
    Late,           // the bot waited PLAN_WAIT_MS for its job's answer
    Rejected,       // the answer failed its recheck (state, age, place or scope)
    NoOption,       // the plan ranked nothing, or no plan is there and none is coming (WT63e)
    CannotStart,    // options were ranked, none could start (WT63e)
    Stalled,        // every option that could start has a stalled key (WT63e)
    NoOptionStuck,  // NoOption at or past NO_OPTION_BOUND answers in a row without a start (WN4, BoundNoOption)
    Count
};
// One word per line (clang-format would pack them). Stored words: appended, none renamed; fallback_no_plan (until
// WT63e, between failed and late) is split into no_option, cannot_start and stalled and written no more
// clang-format off
constexpr std::array<char const*, static_cast<std::size_t>(Fallback::Count)> FALLBACK_WORDS = {
    "weights",
    "fallback_not_ready",
    "fallback_failed",
    "fallback_late",
    "fallback_rejected",
    "fallback_no_option",
    "fallback_cannot_start",
    "fallback_stalled",
    "fallback_no_option_stuck"};
// clang-format on

inline char const* FallbackWord(Fallback reason)
{
    return reason < Fallback::Count ? FALLBACK_WORDS[static_cast<std::size_t>(reason)] : "weights";
}

// A goal in a bot's slot: the goal, who set it and when
struct Active : Goals::Goal
{
    Source source{Source::Planner};
    std::uint32_t startedMs{0};

    bool Is() const { return kind != Goals::GoalKind::None; }
};

inline Active FromOption(Goals::Option const& option, Source source, std::uint32_t nowMs)
{
    Active goal;
    static_cast<Goals::Goal&>(goal) = option;
    goal.source = source;
    goal.startedMs = nowMs;
    return goal;
}

// Precedence: a bridge goal is replaced only by another bridge goal; the planner's by anyone
inline bool MayReplace(Active const& current, Source incoming)
{
    return !current.Is() || IsExternal(incoming) || !IsExternal(current.source);
}

// One bot's goal: set, query, progress, cancel (3.12). An interlude (errand, bag, rest, hearth) keeps the goal it
// interrupts; when the interlude ends that goal goes on, its clock moved on by the interlude's length (3.19 (6): an
// interlude never counts against the order it interrupts, PlannedEnd), up to INTERLUDE_SHIFT_MAX_MS per goal
class Slot
{
public:
    // False when a goal of a higher source holds the slot
    bool Set(Active goal)
    {
        if (!MayReplace(_current, goal.source))
            return false;
        if (Goals::IsInterlude(goal.kind) && _current.Is() && !Goals::IsInterlude(_current.kind))
        {
            _previous = _current;
            _interruptedMs = goal.startedMs;
        }
        else if (!Goals::IsInterlude(goal.kind))
        {
            _previous.reset();
            _shiftedMs = 0;
        }
        _current = std::move(goal);
        ++_changes;
        return true;
    }

    Active const& Query() const { return _current; }

    // A goal of this kind holds the slot or waits behind the interlude that holds it
    bool Holds(Goals::GoalKind kind) const
    {
        return (_current.Is() && _current.kind == kind) || (_previous && _previous->kind == kind);
    }

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
        _shiftedMs = 0;
        ++_changes;
        return true;
    }

    // The goal's executor ended. True when an interlude ended and the goal it interrupted goes on: its start moves on
    // by the time since the first interlude began (interludes in a row count as one), so the order counts its own time;
    // all its interludes together move it at most INTERLUDE_SHIFT_MAX_MS, the rest counts as the order's (Bounded)
    bool End(std::uint32_t nowMs)
    {
        if (_current.Is() || _previous)
            ++_changes;
        _bounded = false;
        if (_previous)  // set only while an interlude holds the slot
        {
            _current = std::move(*_previous);
            std::uint32_t const length = nowMs - _interruptedMs;
            std::uint32_t const shift = std::min(length, INTERLUDE_SHIFT_MAX_MS - _shiftedMs);
            _bounded = shift < length;
            _shiftedMs += shift;
            _current.startedMs += shift;
            _previous.reset();
            return true;
        }
        _current = {};
        _previous.reset();
        _shiftedMs = 0;
        return false;
    }

    // The last End resumed a goal whose interludes passed INTERLUDE_SHIFT_MAX_MS: part of this one counted against it
    bool Bounded() const { return _bounded; }

    bool HasExternal() const { return _current.Is() && IsExternal(_current.source); }

    // Counts the slot's changes (set, cancel, end): a published view older than the count is stale (ViewStale)
    std::uint32_t Changes() const { return _changes; }

private:
    Active _current;
    std::optional<Active> _previous;
    std::uint32_t _interruptedMs{0};  // when the interlude that set _previous began
    std::uint32_t _shiftedMs{0};      // how far the interludes moved the goal's clock so far (at most the bound)
    bool _bounded{false};
    std::uint32_t _changes{0};
};

// What starts a goal of this kind: today's statuses are the executors. None: nothing to do from here
enum class Executor : std::uint8_t
{
    None,
    Circuit,  // the local circuit: its next task or turn-in to DO_QUEST, else its givers
    Deliver,  // DO_QUEST on the goal's complete quest: it walks to the ender
    Grind,
    Gather,
    Travel,
    Rest,
    Errand,  // the errand entry (an errand or a bag): the trip around the vendor, an interlude
    Bind,    // the bind entry (a hearth: bind at this inn): the trip to the inn, bound on arrival, an interlude
    Fish,    // a fishing trip (professions WR16b): DO_FISH at the goal's water
    Route    // the quest route (questing WQ18a): DO_QUEST with the route's next order (AdvanceRoute)
};

// What a quest goal names (GoalModel): the circuit at the bot's own hub, a carried quest's delivery to its ender, a
// move to another hub or camp in the bot's zone (how a starting area hands on to its next hub), or the quest route
// on the bot's map (questing WQ18a)
enum class QuestRole : std::uint8_t
{
    Circuit,
    Delivery,
    Move,
    Route
};

inline QuestRole RoleOf(Goals::TargetType targetType, std::string const& reason)
{
    if (targetType == Goals::TargetType::Route)
        return QuestRole::Route;
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
    bool workable{false};     // its quest is in the log and not complete yet
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
                case QuestRole::Route:
                    return Executor::Route;
                case QuestRole::Delivery:
                    // An external quest goal also works its quest (DO_QUEST); the planner's deliveries only deliver
                    if (!reach.deliverable)
                        return IsExternal(goal.source) && reach.workable ? Executor::Deliver : Executor::None;
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
        case Goals::GoalKind::Errand:
        case Goals::GoalKind::BuyBag:
            return Executor::Errand;
        case Goals::GoalKind::Hearth:
            return Executor::Bind;
        case Goals::GoalKind::Fish:
            return Executor::Fish;
        default:
            return Executor::None;
    }
}

// The goal runs the local circuit (Executor::Circuit wherever the bot is)
inline bool RunsCircuit(Active const& goal)
{
    return goal.kind == Goals::GoalKind::Quest && RoleOf(goal.targetType, goal.reason) == QuestRole::Circuit;
}

// The goal runs the quest route (Executor::Route wherever the bot is)
inline bool RunsRoute(Active const& goal)
{
    return goal.kind == Goals::GoalKind::Quest && RoleOf(goal.targetType, goal.reason) == QuestRole::Route;
}

// What an idle bot does
enum class IdleAction : std::uint8_t
{
    Apply,     // a fresh plan is in hand
    Wait,      // its plan is coming
    Fallback,  // today's pick, with the reason
};

struct IdleInput
{
    bool ready{false};           // the indexes and the quest index
    bool freshPlan{false};       // an unused plan within the stale limit
    bool jobComing{false};       // a job is out or due
    std::uint32_t idleForMs{0};  // since the bot went idle without a plan
    bool lastRejected{false};
    bool lastFailed{false};  // the last job failed or was lost
};

struct IdleDecision
{
    IdleAction action{IdleAction::Fallback};
    Fallback reason{Fallback::NotReady};
};

inline IdleDecision DecideIdle(IdleInput const& in)
{
    if (!in.ready)
        return {IdleAction::Fallback, Fallback::NotReady};
    if (in.freshPlan)
        return {IdleAction::Apply, Fallback::None};
    if (in.jobComing)
        return in.idleForMs < PLAN_WAIT_MS ? IdleDecision{IdleAction::Wait, Fallback::None}
                                           : IdleDecision{IdleAction::Fallback, Fallback::Late};
    if (in.lastFailed)
        return {IdleAction::Fallback, Fallback::Failed};
    return {IdleAction::Fallback, in.lastRejected ? Fallback::Rejected : Fallback::NoOption};
}

// A fresh plan that started nothing (3.19 (5)): it ranked no option; an option met its stalled key and none started,
// so every one that could start stalled; else none could start (no executor, refused, waiting for its retry)
inline Fallback NotStartedReason(std::size_t options, bool anyStalled)
{
    return !options ? Fallback::NoOption : anyStalled ? Fallback::Stalled : Fallback::CannotStart;
}

// The idle pick's statuses (3.19 (5); (2) rows 8, 18, 19): a planner bot's fallback takes a short local status, never
// a flight, outdoor PvP, a grind or camp walk, a quest or a gather; the weighted pick (a bot that keeps no
// possessions, a bot whose planner a test holds) keeps upstream's full list. With none of them available the pick
// rests (RandomChangeStatus)
constexpr std::array<std::uint32_t, 3> FALLBACK_STATUSES = {StatusEnd::STATUS_WANDER_RANDOM,
                                                            StatusEnd::STATUS_WANDER_NPC, StatusEnd::STATUS_REST};
constexpr std::array<std::uint32_t, 9> WEIGHTED_STATUSES = {
    StatusEnd::STATUS_GO_CAMP,    StatusEnd::STATUS_GO_GRIND,    StatusEnd::STATUS_WANDER_RANDOM,
    StatusEnd::STATUS_WANDER_NPC, StatusEnd::STATUS_DO_QUEST,    StatusEnd::STATUS_TRAVEL_FLIGHT,
    StatusEnd::STATUS_REST,       StatusEnd::STATUS_OUTDOOR_PVP, StatusEnd::STATUS_DO_GATHER};

// The fallback list; past the no-option bound (moving, WN4) without rest, so the bot walks on instead of resting at
// the same spot again. With the questing test's setting (questing WQ37) a planner bot's fallback is a rest, past the
// bound too: a wander serves no levelling, and the time it rests counts as the plan's fault
inline std::vector<std::uint32_t> IdleList(bool fallback, bool moving, bool questingOnly = false)
{
    if (fallback && questingOnly)
        return {StatusEnd::STATUS_REST};
    std::vector<std::uint32_t> list =
        fallback ? std::vector<std::uint32_t>(FALLBACK_STATUSES.begin(), FALLBACK_STATUSES.end())
                 : std::vector<std::uint32_t>(WEIGHTED_STATUSES.begin(), WEIGHTED_STATUSES.end());
    if (fallback && moving)
        list.erase(std::remove(list.begin(), list.end(), StatusEnd::STATUS_REST), list.end());
    return list;
}

inline bool FallbackStatus(std::uint32_t status)
{
    for (std::uint32_t const listed : FALLBACK_STATUSES)
        if (listed == status)
            return true;
    return false;
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

// An interlude ended and the quest goal it interrupted goes on: the interlude may have cut the givers visit short and
// taken the bot away (an errand trip to a far vendor or bank), so the resumed circuit may send it to its givers again
inline void ResumeQuest(QuestGoal& goal) { goal.giversVisited = false; }

// A route goal's own progress (questing WQ18a), kept across plans as the circuit's is: the order its executor runs
// and how far the held route (the newest plan's) is walked. A new goal starts it anew (StartActive)
struct RouteGoal
{
    std::optional<QuestOrder> running;  // the order the executor runs
    std::size_t next{0};                // into the held route's orders: those before it lie behind the bot
    bool replaced{false};               // the newest route begins with another order: the running one ends as replaced
};

enum class RouteStep : std::uint8_t
{
    Done,  // no order left: the goal ends
    Order  // DO_QUEST with this order
};

struct RouteNext
{
    RouteStep step{RouteStep::Done};
    QuestOrder const* order{nullptr};  // into the held route
};

// An interlude ended and the route goal it interrupted goes on: its executor did not end the order it ran, so the
// route gives that order again (unless a newer route replaced it meanwhile)
inline void ResumeRoute(RouteGoal& goal)
{
    if (goal.running && !goal.replaced && goal.next > 0)
        --goal.next;
    goal.running.reset();
    goal.replaced = false;
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
// with the log signature as it was at the goal's start, that key stalls until the signature changes or the block
// time passed (WQ6b), and so does the key of a start its executor refused; options with a stalled key do not start,
// so the plan's next option or the counted fallback runs. It ends every "start,
// fail, start again" loop: takes the givers refuse, a delivery without a reward POI or one that failed at the ender,
// a trip without a route, a move between two dry hubs. A run whose status was ended from outside (a planner switch,
// an order, a hold, a pause, sickness: StatusEnd::Judged false) is not judged (NoteStatusEnd, consolidation CS14e).
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

// A route's own key is never blocked (questing WQ18a, WQ.43 point 6): what a route remembers is the cell of an order
// that ended dry, a block in the same list under a key of its own kind. The tag is bit 63, which no other key sets:
// FieldMap.h's cell key holds the map in bits 48 to 63 and the game's map ids stay far below 32768, a goal's key
// holds its executor in bits 56 to 63, a circuit's key nothing above bit 32
constexpr std::uint64_t CELL_KEY = 1ULL << 63;
static_assert(static_cast<std::uint64_t>(Executor::Route) < 128);

inline bool IsRouteKey(std::uint64_t key) { return (key >> 56) == static_cast<std::uint64_t>(Executor::Route); }

inline std::uint64_t CellBlockKey(std::uint64_t cell) { return CELL_KEY | cell; }

inline bool IsCellBlock(std::uint64_t key) { return (key & CELL_KEY) != 0; }

inline std::uint64_t CellOfBlock(std::uint64_t key) { return key & ~CELL_KEY; }

// The cell is closed to the bot: one of the closed cells or a neighbour of one, as the route's build leaves them out
// (Route::Detail::NearClosed; tests/quest-route-selftest.cpp holds the two together)
inline bool CellClosed(std::span<std::uint64_t const> closed, std::uint64_t cell)
{
    return std::any_of(closed.begin(), closed.end(),
                       [cell](std::uint64_t shut)
                       {
                           return FieldMapOf(shut) == FieldMapOf(cell) &&
                                  std::abs(FieldCellXOf(shut) - FieldCellXOf(cell)) <= 1 &&
                                  std::abs(FieldCellYOf(shut) - FieldCellYOf(cell)) <= 1;
                       });
}

// The keys whose goals ended without progress or whose starts were refused, all for one log signature: a changed
// signature lifts them all, and each block also ends by itself once the block time passed (questing WQ6b, the
// stopgap of WQ.33: Planner.BlockMinutes), so whatever a bot's goals did, some goal may start again within the block
// time and one fallback status. Times are the planner's getMSTime(), compared as differences (it wraps)
constexpr std::size_t MAX_STALLED = 8;

struct Block
{
    std::uint64_t key{0};      // GoalKey
    std::uint32_t sinceMs{0};  // set then
};

struct Stalled
{
    std::uint64_t log{0};
    std::vector<Block> blocks;  // oldest first
};

// A block still holds at nowMs
inline bool BlockHolds(Block const& block, std::uint32_t nowMs, std::uint32_t blockMs)
{
    return nowMs - block.sinceMs < blockMs;
}

// The goals whose product is experience or money: a grind or a gather gains XP without a log change
inline bool XpIsProgress(Goals::GoalKind kind)
{
    return kind == Goals::GoalKind::Grind || kind == Goals::GoalKind::Gather || kind == Goals::GoalKind::Money;
}

// The goals judged by what they took (professions WR20c): a gather or a money goal that gained items or copper made
// progress, whether or not its kills gave XP
inline bool GainIsProgress(Goals::GoalKind kind)
{
    return kind == Goals::GoalKind::Gather || kind == Goals::GoalKind::Money;
}

// The goal in the slot (no interlude) while its executor runs: its key, the log signature and the experience (level
// and XP, XpNow) when it started
struct Run
{
    std::uint64_t key{0};
    std::uint64_t startLog{0};
    std::uint64_t startXp{0};
    bool xpCounts{false};  // XpIsProgress(kind): XP gained is progress
    bool running{false};
    bool outside{false};  // its status was ended from outside since the executor last started (NoteStatusEnd)
    Goals::GoalKind kind{Goals::GoalKind::None};
    bool judged{false};  // a judged end of its executor came (NoteStatusEnd): the last one's outcome and note
    StatusEnd::Outcome lastOutcome{StatusEnd::Outcome::Finished};
    char const* lastNote{""};    // a literal (StatusEnd::Note)
    std::uint64_t startGain{0};  // the bot's copper plus its bag items' count at the start (Planner GainNow)
    bool gainCounts{false};      // GainIsProgress(kind): a gain of them is progress
};

// A stall EndRun recorded, until the planner writes its event row (GoalStalled): the goal's kind and what last
// ended its executor (3.19 (6): the nights rank stalls by it), or a refused start's block (StartActive, WQ6b)
struct StallRow
{
    Goals::GoalKind kind{Goals::GoalKind::None};
    bool judged{false};
    StatusEnd::Outcome outcome{StatusEnd::Outcome::Finished};
    char const* note{""};
    bool refused{false};  // the block of a start its executor refused: the row's word is "refused"
};

constexpr std::uint32_t PAUSE_MS = 10 * 60 * 1000;  // a paused plan goes on by itself after this long

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

// The last interlude that missed what it went for (NoteInterludeEnd) or whose start was refused (NoteRefused): the
// goal (kind, target, reason) at the bot's level then, when, and how many misses of it in a row came before
struct Missed
{
    Goals::GoalKind kind{Goals::GoalKind::None};
    Goals::TargetType targetType{Goals::TargetType::Default};
    std::uint32_t target{0};
    std::string reason;
    std::uint8_t level{0};
    std::uint32_t lastMs{0};  // 0 = none
    std::uint32_t repeats{0};
};

// A bot's planner side on its map thread (Planner::BotState::goals). A reset of the AI starts it anew
struct BotGoals
{
    Slot slot;
    std::shared_ptr<Goals::GoalPlan const> plan;         // the newest plan taken
    std::shared_ptr<Goals::GoalPlan const> circuitPlan;  // the plan whose circuit the circuit goal runs (TakePlan)
    std::uint32_t planMs{0};                             // its snapshot's time; 0 = none
    std::uint32_t lastPlanMs{0};                         // the plan before it
    bool planUsed{false};                                // a plan starts at most one new goal
    std::vector<std::uint32_t> keep;                     // the plan's KeepList
    QuestGoal quest;                                     // the current circuit goal's progress, across plans
    RouteGoal route;                                     // the current route goal's progress, across plans
    Run run;                                             // the current goal's run (the stall rule)
    std::optional<Stalled> stalled;                      // keys blocked: no progress, or a refused start
    bool circuitLogged{false};                           // this plan's circuit rows are written
    bool inFallback{false};                              // the status is the idle step's fallback (NoteFallback)
    bool jobOut{false};            // a job is out; Submit drops an older one, so its result is the one
    std::uint32_t idleSinceMs{0};  // waiting for a plan since; 0 = not waiting
    bool waitCounted{false};
    bool lastRejected{false};
    bool lastFailed{false};
    std::uint8_t noOptionRun{0};  // no-option answers in a row since the last started goal (BoundNoOption)
    Fallback lastFallback{Fallback::None};
    std::uint32_t lastFallbackMs{0};
    bool situationChanged{false};  // level, zone, life or group changed since the last plan
    QuestLog::Round pickups;       // a bot on an order: the accepts since its last plan (NotePickup)
    SeenQuests seen;
    bool paused{false};  // the bridge paused the bot's plan (BridgePause)
    std::uint32_t pausedMs{0};
    std::uint32_t publishedChanges{0};  // the slot's change count and the pause as last published (MarkPublished)
    bool publishedPaused{false};
    bool shortOpen{false};  // a NeedShort window is open: the plans taken since its begin had a shortfall
    std::string shortKind;  // its kind word, as at the begin (the end row repeats it)
    // By goal kind: when an interlude of that kind last failed (NoteInterludeEnd); 0 = none
    std::array<std::uint32_t, static_cast<std::size_t>(Goals::GoalKind::Count)> failedMs{};
    Missed missed;                  // the same goal's own, longer wait after misses in a row (MissedWaits)
    std::uint16_t bagSlotsSeen{0};  // the bag capacity as the errand trigger last saw it (BagSlotsChanged); 0 = not yet
    std::uint8_t bindSeen{0};  // the bind as the inn trigger last saw it (BindFarChanged): 1 near, 2 far; 0 = not yet
    bool sickOpenSeen{false};  // recover's hand-over of a sick bot as the errand trigger last saw it (SickOpenChanged)
    std::vector<Goals::InnStay> innStays;  // the inns the bot is near and since when (NR.5)
    bool bagBought{
        false};  // the planner's bag purchase bought its bag (Planner::NoteBagBought); a goal's start clears it
    // NR14: the planner's errand to a stop of its own (Goals::OwnStopReason) got what it went for there (the spells,
    // the rank or its mount, the tame); a goal's start clears it
    bool ownStopDone{false};
    std::optional<StallRow> stallRow;  // EndRun's newest stall, until its row is written
    QuestFailRows::Sums questStalls;   // questing WQ38: per quest worked on orders, the time without a credit
    // The skill path the newest plan brought (professions WR11), rebuilt with every plan; null: none yet
    std::shared_ptr<SkillPath::Path const> path;
    std::uint32_t pathMs{0};  // its snapshot's time
    // The newest plan's gather rates (professions WR13c), null: none yet, so nothing is gated; and the nodes and
    // hides the pass-by gate judged in the running status (GatherPassBy.h)
    std::shared_ptr<Materials::Rates const> gatherRates;
    Gather::Passes passes;
};

// The bag capacity now (Planner::NoteBagSlots): true when it changed since the last look; the first look records it
inline bool BagSlotsChanged(BotGoals& goals, std::uint16_t slots)
{
    std::uint16_t const seen = std::exchange(goals.bagSlotsSeen, slots);
    return seen && seen != slots;
}

// Recover's hand-over of a sick bot now (Planner::NoteSickOpen, NR3): true only where it turned open, so the planner
// plans from the hand-over once and not at every check; a healthy bot or one not handed over is seen closed
inline bool SickOpenChanged(BotGoals& goals, bool open) { return !std::exchange(goals.sickOpenSeen, open) && open; }

// The bind now (Planner::NoteBindFar): true when a bind seen near turned far or one seen far turned near (a plan
// taken before the bind is then stale); the first look records it (the goals' first snapshot weighs the bind as it is)
inline bool BindFarChanged(BotGoals& goals, bool bindFar)
{
    std::uint8_t const now = bindFar ? 2 : 1;
    std::uint8_t const seen = std::exchange(goals.bindSeen, now);
    return seen != 0 && seen != now;
}

// A bot's goal and newest plan as its map thread last published them (PlannerService's view), copied for a reader on
// another thread; the plan is immutable and the copy keeps it alive
struct PlanView
{
    Active goal;
    std::shared_ptr<Goals::GoalPlan const> plan;
    std::uint32_t planMs{0};
    bool jobOut{false};
    Fallback lastFallback{Fallback::None};
    bool paused{false};  // the bridge paused the bot's plan
};

// A new plan's route replaces the held one (questing WQ18a). The order that is running goes on when the new route's
// first order is it (SameOrder: its cell and its word), and the walk goes on behind it: a refreshed route never
// starts the running order again, which would begin its stay anew with every plan. Otherwise the running order is
// replaced (the executor ends it by that word), and the bot's next order is the new route's first
inline void RefreshRoute(RouteGoal& goal, Goals::RoutePlan const* fresh)
{
    bool const same =
        goal.running && fresh && !fresh->orders.empty() && SameOrder(fresh->orders.front(), *goal.running);
    goal.next = same ? 1 : 0;
    goal.replaced = goal.running.has_value() && !same;
}

// A checked plan replaces the old one. The current goal's own progress (QuestGoal) stays, and a running circuit goal
// keeps its own circuit: a plan for another circuit (the bot away on an interlude's trip to a far vendor or bank,
// 3.19 (6)) does not replace it, a plan for the same circuit refreshes it. Its route replaces the held one
// (RefreshRoute)
inline void TakePlan(BotGoals& goals, std::shared_ptr<Goals::GoalPlan const> plan, std::uint32_t planMs)
{
    goals.lastRejected = false;
    goals.lastFailed = false;
    goals.lastPlanMs = goals.planMs;
    goals.plan = std::move(plan);
    RefreshRoute(goals.route, goals.plan ? &goals.plan->route : nullptr);
    bool const ownRun = goals.run.running && goals.circuitPlan && goals.run.key == CircuitKey(*goals.circuitPlan);
    if (!ownRun || (goals.plan && CircuitKey(*goals.plan) == goals.run.key))
        goals.circuitPlan = goals.plan;
    goals.planMs = planMs;
    goals.planUsed = false;
    goals.keep = goals.plan ? KeepList(goals.plan->circuit) : std::vector<std::uint32_t>();
    goals.circuitLogged = false;
}

// NeedShort (upkeep §3.14): a taken plan's shortfall opens a window when there was none and closes it when it is back
// at 0. A change of the unpaid kind inside a window is no new window
enum class ShortChange : std::uint8_t
{
    None,
    Begin,
    End
};

inline ShortChange NoteShort(BotGoals& goals, std::uint64_t shortfallCopper, std::string const& kind)
{
    if (shortfallCopper > 0 && !goals.shortOpen)
    {
        goals.shortOpen = true;
        goals.shortKind = kind;
        return ShortChange::Begin;
    }
    if (shortfallCopper == 0 && goals.shortOpen)
    {
        goals.shortOpen = false;
        return ShortChange::End;
    }
    return ShortChange::None;
}

// A goal with this key may start: not while its key is blocked, the log still as it was then and the block time
// (blockMs) not yet passed. A scan of at most MAX_STALLED entries
inline bool MayStart(BotGoals const& goals, std::uint64_t key, std::uint64_t log, std::uint32_t nowMs,
                     std::uint32_t blockMs)
{
    if (!goals.stalled || goals.stalled->log != log)
        return true;
    return std::none_of(goals.stalled->blocks.begin(), goals.stalled->blocks.end(),
                        [&](Block const& block) { return block.key == key && BlockHolds(block, nowMs, blockMs); });
}

// Blocks the key from nowMs at this log (a changed log starts the list anew). The blocks that ended leave the list,
// and the key's own older entry too, so a key blocked again after its block ended is blocked anew; when the list is
// full its oldest entry goes. True when the key was not blocked: a new block, which gets its GoalStalled row
inline bool BlockKey(BotGoals& goals, std::uint64_t key, std::uint64_t log, std::uint32_t nowMs, std::uint32_t blockMs)
{
    if (!goals.stalled || goals.stalled->log != log)
        goals.stalled = Stalled{log, {}};
    std::vector<Block>& blocks = goals.stalled->blocks;
    bool held = false;
    std::erase_if(blocks,
                  [&](Block const& block)
                  {
                      bool const holds = BlockHolds(block, nowMs, blockMs);
                      held = held || (block.key == key && holds);
                      return block.key == key || !holds;
                  });
    if (blocks.size() >= MAX_STALLED)
        blocks.erase(blocks.begin());
    blocks.push_back({key, nowMs});
    return !held;
}

// The cells closed to the bot now, sorted (Situation::closedCells, AdvanceRoute): those of the orders that ended dry
// at this log within the block time. A changed log opens them again, as it lifts every stall
inline std::vector<std::uint64_t> ClosedCells(BotGoals const& goals, std::uint64_t log, std::uint32_t nowMs,
                                              std::uint32_t blockMs)
{
    std::vector<std::uint64_t> cells;
    if (!goals.stalled || goals.stalled->log != log)
        return cells;
    for (Block const& block : goals.stalled->blocks)
        if (IsCellBlock(block.key) && BlockHolds(block, nowMs, blockMs))
            cells.push_back(CellOfBlock(block.key));
    std::sort(cells.begin(), cells.end());
    return cells;
}

// A changed log lifts every stall. A cell closed at this very log (an order that ended dry while the goal ran)
// stays closed: its block is newer than the change
inline void LiftStalls(BotGoals& goals, std::uint64_t log)
{
    if (goals.stalled && goals.stalled->log == log)
        std::erase_if(goals.stalled->blocks, [](Block const& block) { return !IsCellBlock(block.key); });
    if (goals.stalled && (goals.stalled->log != log || goals.stalled->blocks.empty()))
        goals.stalled.reset();
}

// Whether an order's own end closes its cell (questing WQ18b part 3: the coordinator's ruling, which revises WQ.43
// point 6 narrowly). One rule: the place gave nothing (dry) or could not be reached (`time` before the arrival: the
// walk's bound passed). A stay that passed at the place, a done, a replaced, a no_ender and a bags end close nothing
constexpr bool ClosesCell(QuestOrderEnd end, bool reached)
{
    return end == QuestOrderEnd::Dry || (end == QuestOrderEnd::Time && !reached);
}

// The same rule for a status end that is not the order's own: a judged failure of the order's walk or work (stuck,
// abandoned by the walk's word, timed out by a walk) is a place that could not be reached. Any other end took the bot
// over (an interlude, a switch, a hold, sickness), and the order is given again (NextOrder)
constexpr bool FailsOrder(StatusEnd::Outcome outcome)
{
    return outcome == StatusEnd::Outcome::Stuck || outcome == StatusEnd::Outcome::Abandoned ||
           outcome == StatusEnd::Outcome::TimedOut;
}

// An order's cell closes, and with it the cell's neighbours (CellClosed), for the block time: a route's whole memory
// until batch F3. True: a new block, which gets its one GoalStalled row (a quest goal's order, with the outcome and
// the word of what ended it: timeout/dry, timeout/time, abandoned/no_walk)
inline bool CloseOrderCell(BotGoals& goals, QuestOrder const& order, StatusEnd::Outcome outcome, char const* note,
                           std::uint64_t log, std::uint32_t nowMs, std::uint32_t blockMs)
{
    if (!BlockKey(goals, CellBlockKey(order.cell), log, nowMs, blockMs))
        return false;
    goals.stallRow = StallRow{Goals::GoalKind::Quest, true, outcome, note};
    return true;
}

// The running order ended, by the executor's word for its end; `reached`: the bot had arrived at its place. An end
// that closes the cell (ClosesCell) blocks it; no other end blocks anything, and the route itself is never blocked
// (EndRun, StartActive). True: a new block
inline bool EndOrder(BotGoals& goals, QuestOrderEnd end, std::uint64_t log, std::uint32_t nowMs, std::uint32_t blockMs,
                     bool reached = true)
{
    std::optional<QuestOrder> const order = std::exchange(goals.route.running, std::nullopt);
    goals.route.replaced = false;
    if (!order || !ClosesCell(end, reached))
        return false;
    return CloseOrderCell(goals, *order, StatusEnd::Outcome::TimedOut, QuestOrderEndName(end), log, nowMs, blockMs);
}

// The running order's status ended by a failure that is not the order's own end (FailsOrder; `note`: a literal, null:
// the outcome's word): the order is over, not given again, and its cell closes. True: a new block
inline bool FailOrder(BotGoals& goals, StatusEnd::Outcome outcome, char const* note, std::uint64_t log,
                      std::uint32_t nowMs, std::uint32_t blockMs)
{
    std::optional<QuestOrder> const order = std::exchange(goals.route.running, std::nullopt);
    goals.route.replaced = false;
    if (!order)
        return false;
    return CloseOrderCell(goals, *order, outcome, note ? note : StatusEnd::Word(outcome), log, nowMs, blockMs);
}

// The route goal's next order (questing WQ18a): the held route's next whose cell is not closed and one of whose
// expected needs is still open (`open`, read from the bot's log on its map thread). An order with nothing open is
// never started, so a way, which expects nothing, is no route's order until a batch plans one. None left: done; the
// goal ends and the held plan starts nothing more (planUsed), so the idle step asks for a new one
inline RouteNext AdvanceRoute(BotGoals& goals, std::function<bool(QuestOrderNeed const&)> const& open,
                              std::span<std::uint64_t const> closed)
{
    RouteGoal& goal = goals.route;
    goal.running.reset();
    goal.replaced = false;
    if (goals.plan)
    {
        std::vector<QuestOrder> const& orders = goals.plan->route.orders;
        while (goal.next < orders.size())
        {
            QuestOrder const& order = orders[goal.next++];
            if (CellClosed(closed, order.cell) || std::none_of(order.expected.begin(), order.expected.end(), open))
                continue;
            goal.running = order;
            return {RouteStep::Order, &order};
        }
    }
    goals.planUsed = true;
    return {};
}

// The executor ended an order by its own end (Planner::OnOrderEnd, questing WQ18b). Only the route's running order
// ends by it: another order (a test's `route field` elsewhere) is not the route's. True: a new block (EndOrder)
inline bool EndRouteOrder(BotGoals& goals, QuestOrder const& ended, QuestOrderEnd end, std::uint64_t log,
                          std::uint32_t nowMs, std::uint32_t blockMs, bool reached = true)
{
    if (!goals.route.running || !SameOrder(*goals.route.running, ended))
        return false;
    return EndOrder(goals, end, log, nowMs, blockMs, reached);
}

// An order's status ended by a failure that is not its own end (Planner::OnOrderFailed, FailsOrder): the route's
// running order only, as EndRouteOrder. True: a new block (FailOrder)
inline bool FailRouteOrder(BotGoals& goals, QuestOrder const& ended, StatusEnd::Outcome outcome, char const* note,
                           std::uint64_t log, std::uint32_t nowMs, std::uint32_t blockMs)
{
    if (!goals.route.running || !SameOrder(*goals.route.running, ended))
        return false;
    return FailOrder(goals, outcome, note, log, nowMs, blockMs);
}

// The route goal's executor starts (questing WQ18b): a new goal, the continuation after its order's end, the goal
// resumed after an interlude. An order its executor did not end (the bot was taken from it: an interlude, a status
// ended from outside) is given again (ResumeRoute), unless a newer route replaced it; then the route's next order
// (AdvanceRoute). A bot a test holds on the circuit (the constraint `circuit`, part 3) gets no order: a route goal it
// held from before the hold ends at this order boundary, and the held plan starts nothing more
inline RouteNext NextOrder(BotGoals& goals, std::function<bool(QuestOrderNeed const&)> const& open,
                           std::span<std::uint64_t const> closed, bool heldOnCircuit = false)
{
    if (heldOnCircuit)
    {
        goals.route = {};
        goals.planUsed = true;
        return {};
    }
    if (goals.route.running)
        ResumeRoute(goals.route);
    return AdvanceRoute(goals, open, closed);
}

// The order NextOrder gave could not start (its executor refused): the route gives it again at its next start
inline void OrderNotStarted(BotGoals& goals) { ResumeRoute(goals.route); }

// Whether the running order ends as replaced: the newest route begins with another order (RefreshRoute). The service
// tells the executor at each plan taken (QuestOrderRun::replaced), in both directions: a later plan that begins with
// the running order again lets it go on
inline bool OrderReplaced(BotGoals const& goals) { return goals.route.running && goals.route.replaced; }

// A goal (no interlude) starts: its run and its circuit progress start anew
inline void BeginRun(BotGoals& goals, std::uint64_t key, std::uint64_t log, std::uint64_t xp = 0, bool xpCounts = false)
{
    goals.quest = {};
    goals.route = {};
    goals.run = {key, log, xp, xpCounts, true, false};
}

// The status core tells the planner that the bot's status ended (Planner::OnStatusEnd). An end the running goal did
// not cause (StatusEnd::Judged false: a planner switch, a hold, a pause, an order, sickness) marks its run "ended from
// outside", and EndRun closes it without a stall. The mark is sticky: the interrupter's own end does not clear it;
// the goal's executor starting again does (ContinueRun), and a new goal's run starts without it. An end to idle also
// ends a wait for a plan: a wait that another action ended (an errand, an inn, an order) leaves no stale start behind.
// Any end ends a fallback status. A judged end of the running goal's executor (not an interlude's) is the run's last
// outcome, with its note (`note`: a literal; null: the outcome's word): a stall's row carries it (EndRun)
inline void NoteStatusEnd(BotGoals& goals, StatusEnd::Outcome outcome, bool toIdle, char const* note = nullptr)
{
    goals.inFallback = false;
    if (toIdle)
    {
        goals.idleSinceMs = 0;
        goals.waitCounted = false;
    }
    if (goals.run.running && !StatusEnd::Judged(outcome))
        goals.run.outside = true;
    else if (goals.run.running && !Goals::IsInterlude(goals.slot.Query().kind))
    {
        goals.run.judged = true;
        goals.run.lastOutcome = outcome;
        goals.run.lastNote = note ? note : StatusEnd::Word(outcome);
    }
}

// The running goal's executor starts again (the circuit's next step, the goal resumed after an interlude): the end
// of what it starts now is judged again
inline void ContinueRun(BotGoals& goals) { goals.run.outside = false; }

// The goal is the missed one: its kind, target and reason, at the level it missed at
inline bool SameMissed(Missed const& missed, Active const& goal, std::uint8_t level)
{
    return missed.lastMs && missed.kind == goal.kind && missed.targetType == goal.targetType &&
           missed.target == goal.target && missed.reason == goal.reason && missed.level == level;
}

// A miss of the goal at the bot's level: the same goal missed again counts one repeat more (at most
// WorldTravel::STUCK_REPEAT_MAX); another goal, or a new level (it brings new due needs), starts the count anew
inline void NoteMissed(BotGoals& goals, Active const& goal, std::uint8_t level, std::uint32_t nowMs)
{
    std::uint32_t const repeats =
        SameMissed(goals.missed, goal, level) ? std::min(goals.missed.repeats + 1, WorldTravel::STUCK_REPEAT_MAX) : 0;
    goals.missed = {goal.kind, goal.targetType, goal.target, goal.reason, level, std::max<std::uint32_t>(nowMs, 1),
                    repeats};
}

// An interlude's status ended (3.19 (6)): one that timed out, got stuck or was abandoned stamps its kind, which then
// waits out the errand retry time (RetryWaits); one that arrived or finished clears the stamp. An end from outside
// (interrupted, ordered, sick) says nothing about the interlude. A rest has no result to miss: its overrun is no
// failure (PlannedEnd). An interlude that finished without its result missed it, as a failure: a bag trip without its
// bag (bagBought unset), an errand with the repair still due (repairDue, read at that end: a repairer dead or gone, a
// stand point off the mesh; the due repair ranks first and would start the same trip again at once), an errand to a
// stop of its own without what it went for (ownStopDone unset, NR14: the due stop ranks first as the repair does).
// Every failure is also the goal's miss (NoteMissed), a success of that goal ends its count
inline void NoteInterludeEnd(BotGoals& goals, Goals::GoalKind kind, StatusEnd::Outcome outcome, std::uint32_t nowMs,
                             bool repairDue = false, std::uint8_t level = 0)
{
    if (!Goals::IsInterlude(kind) || kind == Goals::GoalKind::Rest)
        return;
    Active const& goal = goals.slot.Query();
    std::uint32_t& failed = goals.failedMs[static_cast<std::size_t>(kind)];
    bool const finished = outcome == StatusEnd::Outcome::Finished;
    bool const ownStop = Goals::OwnStopReason(goal.reason) && !goals.ownStopDone;
    bool const missed = finished && ((kind == Goals::GoalKind::BuyBag && !goals.bagBought) ||
                                     (kind == Goals::GoalKind::Errand && (repairDue || ownStop)));
    if (!missed && (outcome == StatusEnd::Outcome::Arrived || finished))
    {
        failed = 0;
        if (SameMissed(goals.missed, goal, level))
            goals.missed = {};
    }
    else if (StatusEnd::Judged(outcome))
    {
        failed = std::max<std::uint32_t>(nowMs, 1);
        NoteMissed(goals, goal, level, nowMs);
    }
}

// An interlude whose start was refused (StartActive's Refused; a rest's refusal is none to wait on) counts as its
// miss: the errand start, which computes the needs, is not tried again at every plan
inline void NoteRefused(BotGoals& goals, Active const& goal, std::uint8_t level, std::uint32_t nowMs)
{
    if (Goals::IsInterlude(goal.kind) && goal.kind != Goals::GoalKind::Rest)
        NoteMissed(goals, goal, level, nowMs);
}

// What a planned status's end reads besides the goal (3.19 (4)), on the bot's map thread
struct PlannedFacts
{
    std::uint32_t status{0};  // StatusEnd::STATUS_*
    std::uint32_t nowMs{0};
    bool owns{false};           // Planner::Owns: it keeps its possessions and no test holds its planner
    bool inFallback{false};     // the idle step's fallback status (NoteFallback)
    bool sick{false};           // a sick bot rests until the sickness ends (Upkeep::RestEnds)
    std::uint32_t statusMs{0};  // how long the status runs (a fallback's length)
    float healthPercent{100.0f};
    float manaPercent{-1.0f};  // -1: no mana
    bool atBind{false};        // the bot stands at its hearthstone's bind (NR3: the sick bot's stone took it there)
};

struct PlannedAnswer
{
    bool planned{false};  // false: upstream's timer decides
    bool ends{false};
    StatusEnd::Outcome outcome{StatusEnd::Outcome::Finished};
    char const* word{nullptr};  // the Status row's word
};

// A status the planner started for its goal ends by the goal's condition, not by upstream's timer (3.19 (4)): a grind
// or money spot when the order's minutes since its start are over (finished, order_time; on the way to the spot only
// the overrun); a gather by its executor's own ends (circuit_done, its stops' ends; professions WR13d-2: a circuit's
// minutes are a straight-line estimate, and a one-stop lap of under a minute ended before its walk arrived), so only
// the overrun bounds it; a rest once rested (finished, rested); any of them once the order ran
// ORDER_OVERRUN_FACTOR times its minutes (timed out, order_overrun). Planned: a planner goal of those kinds in the
// status that carries it out, for a bot the planner owns, no fallback, no sick rest. The other kinds keep their
// executors' ends and timers (quests: WT35a); a bridge goal keeps upstream's timer. The idle step's fallback (3.19
// (5)): a goal-less status of its list ends when its length is over (timed out, fallback_time); a usable plan ends it
// before (OnFreshPlan). Pure, O(1)
inline PlannedAnswer PlannedEnd(Active const& goal, PlannedFacts const& in)
{
    using Goals::GoalKind;
    if (in.inFallback)
        return in.owns && !in.sick && !goal.Is() && FallbackStatus(in.status)
                   ? PlannedAnswer{true, in.statusMs >= FALLBACK_STATUS_MS, StatusEnd::Outcome::TimedOut,
                                   "fallback_time"}
                   : PlannedAnswer{};
    // NR3 (NR.4): a sick bot's stone in (the errand option sick_hearth, its cast in a rest) has arrived once the bot
    // stands at its bind: the errand there is planned from the new place. A cast the bot never finished (a fight broke
    // it) is overrun
    constexpr float MS_PER_MINUTE = 60000.0f;
    if (in.owns && goal.source == Source::Planner && goal.kind == GoalKind::Errand &&
        goal.reason == Goals::SICK_HEARTH_REASON && in.status == StatusEnd::STATUS_REST)
    {
        if (in.atBind)
            return {true, true, StatusEnd::Outcome::Arrived, "hearth_bind"};
        float const minutes = static_cast<float>(in.nowMs - goal.startedMs) / MS_PER_MINUTE;
        if (minutes >= ORDER_OVERRUN_FACTOR * goal.minutes)
            return {true, true, StatusEnd::Outcome::TimedOut, "order_overrun"};
        return {true, false};
    }
    if (!in.owns || in.sick || goal.source != Source::Planner || goal.minutes <= 0.0f)
        return {};
    bool const spot = in.status == StatusEnd::STATUS_WANDER_RANDOM;
    bool const way = in.status == StatusEnd::STATUS_GO_GRIND;
    bool const node = in.status == StatusEnd::STATUS_DO_GATHER;
    bool const planned = ((goal.kind == GoalKind::Grind || goal.kind == GoalKind::Money) && (spot || way)) ||
                         ((goal.kind == GoalKind::Gather || goal.kind == GoalKind::Money) && node) ||
                         (goal.kind == GoalKind::Rest && in.status == StatusEnd::STATUS_REST);
    if (!planned)
        return {};
    float const minutes = static_cast<float>(in.nowMs - goal.startedMs) / MS_PER_MINUTE;
    if (goal.kind == GoalKind::Rest)
    {
        if (in.healthPercent >= RESTED_PERCENT && (in.manaPercent < 0.0f || in.manaPercent >= RESTED_PERCENT))
            return {true, true, StatusEnd::Outcome::Finished, "rested"};
    }
    else if (!way && !node && minutes >= goal.minutes)
        return {true, true, StatusEnd::Outcome::Finished, "order_time"};
    if (minutes >= ORDER_OVERRUN_FACTOR * goal.minutes)
        return {true, true, StatusEnd::Outcome::TimedOut, "order_overrun"};
    return {true, false};
}

// The bag the planner's running bag purchase is for, until it bought it (bagBought): 0 = none (Planner::OrderedBag)
inline std::uint32_t OrderedBagOf(BotGoals const& goals)
{
    Active const& current = goals.slot.Query();
    bool const ordered = current.kind == Goals::GoalKind::BuyBag && current.source == Source::Planner;
    return ordered && !goals.bagBought ? current.target : 0;
}

// The kind failed as an interlude within the errand retry time: the planner passes it over until then
inline bool RetryWaits(BotGoals const& goals, Goals::GoalKind kind, std::uint32_t nowMs)
{
    if (kind >= Goals::GoalKind::Count)
        return false;
    std::uint32_t const failed = goals.failedMs[static_cast<std::size_t>(kind)];
    return failed && nowMs - failed < WorldTravel::ERRAND_RETRY_MS;
}

// The missed goal waits the errand retry time doubled per repeat (WorldTravel::StuckAvoidMs: 5, 10, 20, then 40 min),
// so a due stop that keeps failing, which ranks first, is tried less and less often and the bot's quests fill the time
inline bool MissedWaits(BotGoals const& goals, Active const& goal, std::uint8_t level, std::uint32_t nowMs)
{
    return SameMissed(goals.missed, goal, level) &&
           nowMs - goals.missed.lastMs < WorldTravel::StuckAvoidMs(WorldTravel::ERRAND_RETRY_MS, goals.missed.repeats);
}

// The running goal's executor ended: its key stalls for the block time (BlockKey) when the log is as it was at the
// goal's start; a changed log lifts every stall. A grind, gather or money goal that gained XP made progress, and so
// did a gather or money goal that gained items or copper (gain: GainNow, WR20c): it does not stall, and the others
// stay stalled (a kill on the way lifts nothing). A run ended from outside (NoteStatusEnd) closes without a stall; a
// changed log still lifts every stall (LiftStalls). A route's run never stalls: its dry orders close their cells
// (EndOrder)
inline void EndRun(BotGoals& goals, std::uint64_t log, std::uint64_t xp, std::uint32_t nowMs, std::uint32_t blockMs,
                   std::uint64_t gain = 0)
{
    if (!goals.run.running)
        return;
    goals.run.running = false;
    bool const outside = std::exchange(goals.run.outside, false);
    if (log != goals.run.startLog)
    {
        LiftStalls(goals, log);
        return;
    }
    if (outside || IsRouteKey(goals.run.key))
        return;
    if (goals.run.xpCounts && xp > goals.run.startXp)
        return;
    if (goals.run.gainCounts && gain > goals.run.startGain)
        return;
    if (BlockKey(goals, goals.run.key, log, nowMs, blockMs))
        goals.stallRow = StallRow{goals.run.kind, goals.run.judged, goals.run.lastOutcome, goals.run.lastNote};
}

// A test's release (TestConstraints::Releases, TravelOrders::ApplyConstraints): every planner goal the bot holds ends,
// of any kind and also one behind an interlude, with its run and without a stall, and the plan taken before the
// release starts no goal, so the first goal after it comes from the release's own snapshot, chosen under the
// constraints the release keeps (professions WR30: a zone move taken at the pool spot before the hold walked the
// released bot away from its fixture). A quest goal under a kept noquest is one such goal; the plans offer it none
// (Goals::Situation::questsHeld). The bridge's goal stays. True when a goal ended
inline bool Release(BotGoals& goals)
{
    goals.planUsed = true;
    if (!goals.slot.Cancel(Source::Planner))
        return false;
    goals.run = {};
    return true;
}

// How a goal's start went (3.5); anything but Started changed nothing but a refused goal's block (StartActive)
enum class StartResult : std::uint8_t
{
    Started,
    NoExecutor,  // nothing starts it from here (Executor::None, a circuit without a plan)
    Stalled,     // its key stalled at this log (MayStart)
    Refused,     // its executor cannot start it now (no place, the errand entry's refusal)
    Retry        // its kind failed as an interlude within the retry time (RetryWaits)
};

// What a goal's start reads besides the goal: its stall key, the log and XP now, the clock and the block time
struct StartFacts
{
    std::uint64_t key{0};
    std::uint64_t log{0};
    std::uint64_t xp{0};
    bool stallRule{true};      // false: the bridge's order, the partner's wish
    std::uint32_t nowMs{0};    // the planner's clock (getMSTime)
    std::uint32_t blockMs{0};  // how long a block holds (Planner.BlockMinutes); 0: none holds
    std::uint64_t gain{0};     // the bot's copper plus its bag items' count (GainNow)
};

// One goal started by its executor (3.5): `start` (StartGoal) returns false having ended nothing, and then the goals
// are as before, but a goal's key (no interlude, no bridge order) is blocked as a goal without progress is (WQ6b):
// the next idles try the plan's other options. A start ends the running status as interrupted, which marks the
// running run outside (NoteStatusEnd), so that run closes without a stall and the new goal's run begins unmarked. An
// interlude begins no run: the goal it interrupts keeps its run and progress. The caller sets the slot (Slot::Set
// keeps that goal)
inline StartResult StartActive(BotGoals& goals, Goals::GoalKind kind, Executor executor, StartFacts const& facts,
                               std::function<bool()> const& start)
{
    if (executor == Executor::None)
        return StartResult::NoExecutor;
    if (Goals::IsInterlude(kind))
    {
        if (!start())
            return StartResult::Refused;
        goals.noOptionRun = 0;  // the bot got work: the no-option bound counts anew
        return StartResult::Started;
    }
    if (facts.stallRule && !MayStart(goals, facts.key, facts.log, facts.nowMs, facts.blockMs))
        return StartResult::Stalled;
    QuestGoal progress = std::exchange(goals.quest, {});  // a goal's circuit progress starts anew
    RouteGoal route = std::exchange(goals.route, {});     // and its route progress
    if (!start())
    {
        goals.quest = std::move(progress);
        goals.route = std::move(route);
        if (facts.stallRule && !IsRouteKey(facts.key) &&
            BlockKey(goals, facts.key, facts.log, facts.nowMs, facts.blockMs))
            goals.stallRow = StallRow{kind, false, StatusEnd::Outcome::Finished, "", true};
        return StartResult::Refused;
    }
    EndRun(goals, facts.log, facts.xp, facts.nowMs, facts.blockMs, facts.gain);
    goals.run = {facts.key, facts.log, facts.xp, XpIsProgress(kind), true, false, kind};
    goals.run.startGain = facts.gain;
    goals.run.gainCounts = GainIsProgress(kind);
    goals.noOptionRun = 0;
    return StartResult::Started;
}

// The bound on the no-option loop (planning WN4): a planner bot whose plan ranks nothing rests, asks again and ranks
// nothing again (WN3's G3: all night at one spot). Its NO_OPTION_BOUND-th no-option answer in a row and every one
// after it, until a goal starts (StartActive), is fallback_no_option_stuck, whose list leaves out rest (IdleList), so
// the bot walks on. Other answers neither count nor reset. What it should do instead is a design question (night
// register N9). Pure, O(1)
inline Fallback BoundNoOption(BotGoals& goals, Fallback reason)
{
    if (reason != Fallback::NoOption)
        return reason;
    if (goals.noOptionRun < NO_OPTION_BOUND)
        ++goals.noOptionRun;
    return goals.noOptionRun >= NO_OPTION_BOUND ? Fallback::NoOptionStuck : Fallback::NoOption;
}

// The idle step fell back to the narrow pick: its status is a fallback until it ends (NoteStatusEnd)
inline void NoteFallback(BotGoals& goals, Fallback reason, std::uint32_t nowMs)
{
    goals.lastFallback = reason;
    goals.lastFallbackMs = nowMs;
    goals.inFallback = true;
}

// What a fresh plan does to the running status (3.5): nothing; a replan of the planner's goal (ReplanDecision); or,
// during a fallback status, the plan's best option that can start. A goal-less status that is no fallback (a
// sickness rest, a command, a test's order) and an external goal stay; a sickness rest yields to a plan whose best
// option passed the sick gate (NR3, Ranked::sickGoal: an errand in the safe area, the bind, the stone in)
enum class FreshPlan : std::uint8_t
{
    Keep,
    Replan,
    Take
};

inline FreshPlan OnFreshPlan(BotGoals const& goals, bool preemptible)
{
    Active const& current = goals.slot.Query();
    if (!preemptible || (current.Is() && IsExternal(current.source)))
        return FreshPlan::Keep;
    if (current.Is())
        return FreshPlan::Replan;
    if (!goals.plan || !goals.plan->ranked.ok)
        return FreshPlan::Keep;
    return goals.inFallback || goals.plan->ranked.sickGoal ? FreshPlan::Take : FreshPlan::Keep;
}

// The bridge's goal orders (WT18a, BridgeGoals.h). The planner's goal and the bridge's own give way
constexpr std::array<char const*, 9> BRIDGE_REASONS = {
    "go_to", "quest", "errands", "set_hearth", "grind_here", "gather_here", "meet_at", "pause_plan", "resume_plan"};

inline char const* BridgeReason(BridgeOrder order)
{
    auto const index = static_cast<std::size_t>(order);
    return index < BRIDGE_REASONS.size() ? BRIDGE_REASONS[index] : "bridge";
}

// What an order meets in the bot, read on the world thread after the map join. The route is searched last, only for
// a trip every other check let through
struct BridgeFacts
{
    bool inScope{false};          // the planner saw the bot in scope, no real master, persistent
    Active current;               // the goal in the slot
    bool placeKnown{false};       // a trip's place resolved
    bool there{false};            // the place is on the bot's map within GIVER_YARDS
    bool openWorld{true};         // the bot is not in an instance
    bool questInLog{false};       // the quest is in the log and has not failed
    bool canGrind{true};          // the bot is not resting (a town or an inn)
    bool canGather{false};        // herbalism or mining
    std::function<bool()> route;  // a route from the bot to the place exists
};

// The plausibility gate's order: scope, the holder, then the order's own rule; Done lets the order through
inline BridgeAnswer BridgeCheck(BridgeOrder order, BridgeFacts const& facts)
{
    if (!facts.inScope)
        return BridgeAnswer::NotPlanner;
    switch (order)
    {
        case BridgeOrder::GoTo:
        case BridgeOrder::MeetAt:
            if (!facts.placeKnown)
                return BridgeAnswer::UnknownPlace;
            if (facts.there)
                return BridgeAnswer::AlreadyThere;
            if (!facts.openWorld)
                return BridgeAnswer::CannotNow;
            return facts.route && facts.route() ? BridgeAnswer::Done : BridgeAnswer::NoRoute;
        case BridgeOrder::Quest:
            return facts.questInLog ? BridgeAnswer::Done : BridgeAnswer::NoSuchQuest;
        case BridgeOrder::GrindHere:
            if (!facts.openWorld)
                return BridgeAnswer::CannotNow;
            return facts.canGrind ? BridgeAnswer::Done : BridgeAnswer::NothingHere;
        case BridgeOrder::GatherHere:
            if (!facts.openWorld)
                return BridgeAnswer::CannotNow;
            return facts.canGather ? BridgeAnswer::Done : BridgeAnswer::NothingHere;
        default:
            return BridgeAnswer::Done;
    }
}

// The goal an order sets (none for a pause or a resume): here is the bot's position, at a trip's resolved place
inline Active BridgeGoalFor(BridgeOrder order, Place const& here, Place const& at, std::uint32_t quest,
                            std::uint32_t nowMs)
{
    Active goal;
    goal.source = Source::Bridge;
    goal.reason = BridgeReason(order);
    goal.place = here;
    goal.startedMs = nowMs;
    switch (order)
    {
        case BridgeOrder::GoTo:
        case BridgeOrder::MeetAt:
            goal.kind = Goals::GoalKind::TravelElsewhere;
            goal.place = at;
            break;
        case BridgeOrder::Quest:
            goal.kind = Goals::GoalKind::Quest;
            goal.target = quest;
            goal.targetType = Goals::TargetType::Quest;
            break;
        case BridgeOrder::Errands:
            goal.kind = Goals::GoalKind::Errand;
            break;
        case BridgeOrder::SetHearth:
            goal.kind = Goals::GoalKind::Hearth;
            break;
        case BridgeOrder::GrindHere:
            goal.kind = Goals::GoalKind::Grind;
            break;
        case BridgeOrder::GatherHere:
            goal.kind = Goals::GoalKind::Gather;
            break;
        default:
            break;
    }
    return goal;
}

// A bridge order its executor refused: an interlude (errands, set-hearth) finds nothing to do here, any other fails
inline BridgeAnswer BridgeRefused(BridgeOrder order)
{
    return order == BridgeOrder::Errands || order == BridgeOrder::SetHearth ? BridgeAnswer::NothingHere
                                                                            : BridgeAnswer::Failed;
}

// A started bridge goal takes the slot and a pause ends. False (nothing changed) when the slot refuses it
inline bool TakeBridgeGoal(BotGoals& goals, Active goal)
{
    if (!goals.slot.Set(std::move(goal)))
        return false;
    goals.paused = false;
    return true;
}

// Pause ends the goal in the slot without a stall and holds the plan; Resume ends a pause and a bridge goal and asks
// for a fresh plan. True when the bot's current executor must stop
inline bool BridgePause(BotGoals& goals, bool pause, std::uint32_t nowMs)
{
    Active const& current = goals.slot.Query();
    bool const bridgeGoal = current.Is() && current.source == Source::Bridge;
    if (pause || bridgeGoal)
    {
        goals.slot.Cancel(Source::Bridge);
        goals.run.running = false;
    }
    goals.paused = pause;
    goals.pausedMs = pause ? nowMs : 0;
    goals.situationChanged = goals.situationChanged || !pause;
    return pause || bridgeGoal;
}

// An idle bot's plan is still paused; a pause older than PAUSE_MS ends here
inline bool StillPaused(BotGoals& goals, std::uint32_t nowMs)
{
    if (goals.paused && nowMs - goals.pausedMs < PAUSE_MS)
        return true;
    goals.paused = false;
    return false;
}

// The published view no longer shows the bot's slot or pause: a goal's end or a pause's end publishes at the next
// idle, also when the bot then waits for its plan (a wait publishes nothing until the job's answer)
inline bool ViewStale(BotGoals const& goals)
{
    return goals.slot.Changes() != goals.publishedChanges || goals.paused != goals.publishedPaused;
}

// The view of this slot and pause is published
inline void MarkPublished(BotGoals& goals)
{
    goals.publishedChanges = goals.slot.Changes();
    goals.publishedPaused = goals.paused;
}
}  // namespace Planner::GoalApply

#endif
