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

#include "BridgeGoals.h"
#include "GatherRules.h"
#include "GoalModel.h"
#include "SkillPathTypes.h"
#include "StatusEndRules.h"

// WT16: the goals interface (3.12) and the rules for applying the planner's answers. Player-free, so the offline
// selftest (tests/goal-apply-selftest.cpp) checks them; PlannerService.cpp applies them on the bot's map thread.
// A goal has a source: the planner's own, or an external one (the bridge, WT18). An external goal outranks
// the planner's until it is done or cancelled. The A/B group (WT.21) keeps today's weighted pick: guid % 10 = 0, the
// rule of the journal's cohort ab-weighted (its complement, ab-planner, gets planner goals).
namespace Planner::GoalApply
{
constexpr std::uint64_t AB_MODULUS = 10;
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

// The weighted group: the character guid's low part, as dim_bots' guid
inline bool InWeightedGroup(std::uint64_t guidLow) { return guidLow % AB_MODULUS == 0; }

// A governed bot (upkeep 3.11): a planner bot that keeps its possessions (the master term is the caller's,
// PlayerbotAI::KeepsPossessions), outside the weighted group, whatever the handout share. Its class quests get the
// unlock bonus (ClassQuestRules.h)
inline bool Governed(bool keepsPossessions, std::uint64_t guidLow)
{
    return keepsPossessions && !InWeightedGroup(guidLow);
}

// A planner bot (3.19): governed, and its planner not held by a test (TestConstraints' planner row). Only the
// planner starts what such a bot does; everybody else keeps today's behaviour
inline bool Owns(bool keepsPossessions, std::uint64_t guidLow, bool plannerFree)
{
    return Governed(keepsPossessions, guidLow) && plannerFree;
}

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

// Why a planner bot took the weighted pick: the reason of its rpg_status row (None: a weighted bot's "weights")
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
    Fish     // a fishing trip (professions WR16b): DO_FISH at the goal's water
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
// a flight, outdoor PvP, a grind or camp walk, a quest or a gather; the weighted pick (the cohort, a bot whose planner
// a test holds) keeps upstream's full list. With none of them available the pick rests (RandomChangeStatus)
constexpr std::array<std::uint32_t, 3> FALLBACK_STATUSES = {StatusEnd::STATUS_WANDER_RANDOM,
                                                            StatusEnd::STATUS_WANDER_NPC, StatusEnd::STATUS_REST};
constexpr std::array<std::uint32_t, 9> WEIGHTED_STATUSES = {
    StatusEnd::STATUS_GO_CAMP,    StatusEnd::STATUS_GO_GRIND,    StatusEnd::STATUS_WANDER_RANDOM,
    StatusEnd::STATUS_WANDER_NPC, StatusEnd::STATUS_DO_QUEST,    StatusEnd::STATUS_TRAVEL_FLIGHT,
    StatusEnd::STATUS_REST,       StatusEnd::STATUS_OUTDOOR_PVP, StatusEnd::STATUS_DO_GATHER};

// The fallback list; past the no-option bound (moving, WN4) without rest, so the bot walks on instead of resting at
// the same spot again
inline std::vector<std::uint32_t> IdleList(bool fallback, bool moving)
{
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

// The keys whose goals ended without progress, all for one log signature: a changed signature lifts them all
constexpr std::size_t MAX_STALLED = 8;

struct Stalled
{
    std::uint64_t log{0};
    std::vector<std::uint64_t> keys;  // GoalKey, oldest first
};

// The goals whose product is experience or money: a grind or a gather gains XP without a log change
inline bool XpIsProgress(Goals::GoalKind kind)
{
    return kind == Goals::GoalKind::Grind || kind == Goals::GoalKind::Gather || kind == Goals::GoalKind::Money;
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
    char const* lastNote{""};  // a literal (StatusEnd::Note)
};

// A stall EndRun recorded, until the planner writes its event row (GoalStalled): the goal's kind and what last
// ended its executor (3.19 (6): the nights rank stalls by it)
struct StallRow
{
    Goals::GoalKind kind{Goals::GoalKind::None};
    bool judged{false};
    StatusEnd::Outcome outcome{StatusEnd::Outcome::Finished};
    char const* note{""};
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
    Run run;                                             // the current goal's run (the stall rule)
    std::optional<Stalled> stalled;                      // keys whose goals ended without progress
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
    SeenQuests seen;
    bool paused{false};  // the bridge paused the bot's plan (BridgePause)
    std::uint32_t pausedMs{0};
    std::uint32_t publishedChanges{0};  // the slot's change count and the pause as last published (MarkPublished)
    bool publishedPaused{false};
    bool shortOpen{false};  // a NeedShort window is open: the plans taken since its begin had a shortfall
    std::string shortKind;  // its kind word, as at the begin (the end row repeats it)
    // By goal kind: when an interlude of that kind last failed (NoteInterludeEnd); 0 = none
    std::array<std::uint32_t, static_cast<std::size_t>(Goals::GoalKind::Count)> failedMs{};
    std::uint16_t bagSlotsSeen{0};  // the bag capacity as the errand trigger last saw it (BagSlotsChanged); 0 = not yet
    std::uint8_t bindSeen{0};  // the bind as the inn trigger last saw it (BindFarChanged): 1 near, 2 far; 0 = not yet
    bool bagBought{
        false};  // the planner's bag purchase bought its bag (Planner::NoteBagBought); a goal's start clears it
    std::optional<StallRow> stallRow;  // EndRun's newest stall, until its row is written
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

// The bind now (Planner::NoteBindFar): true when a bind seen near turned far; the first look records it (the goals'
// first snapshot weighs the bind as it is)
inline bool BindFarChanged(BotGoals& goals, bool bindFar)
{
    std::uint8_t const seen = std::exchange(goals.bindSeen, static_cast<std::uint8_t>(bindFar ? 2 : 1));
    return bindFar && seen == 1;
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

// A checked plan replaces the old one. The current goal's own progress (QuestGoal) stays, and a running circuit goal
// keeps its own circuit: a plan for another circuit (the bot away on an interlude's trip to a far vendor or bank,
// 3.19 (6)) does not replace it, a plan for the same circuit refreshes it
inline void TakePlan(BotGoals& goals, std::shared_ptr<Goals::GoalPlan const> plan, std::uint32_t planMs)
{
    goals.lastRejected = false;
    goals.lastFailed = false;
    goals.lastPlanMs = goals.planMs;
    goals.plan = std::move(plan);
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

// A goal with this key may start: not while its key stalled and the log is still as it was then
inline bool MayStart(BotGoals const& goals, std::uint64_t key, std::uint64_t log)
{
    return !goals.stalled || goals.stalled->log != log ||
           std::find(goals.stalled->keys.begin(), goals.stalled->keys.end(), key) == goals.stalled->keys.end();
}

// A goal (no interlude) starts: its run and its circuit progress start anew
inline void BeginRun(BotGoals& goals, std::uint64_t key, std::uint64_t log, std::uint64_t xp = 0, bool xpCounts = false)
{
    goals.quest = {};
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

// An interlude's status ended (3.19 (6)): one that timed out, got stuck or was abandoned stamps its kind, which then
// waits out the errand retry time (RetryWaits); one that arrived or finished clears the stamp. An end from outside
// (interrupted, ordered, sick) says nothing about the interlude. A rest has no result to miss: its overrun is no
// failure (PlannedEnd). A bag trip that finished without its bag (bagBought unset) missed its result
inline void NoteInterludeEnd(BotGoals& goals, Goals::GoalKind kind, StatusEnd::Outcome outcome, std::uint32_t nowMs)
{
    if (!Goals::IsInterlude(kind) || kind == Goals::GoalKind::Rest)
        return;
    std::uint32_t& failed = goals.failedMs[static_cast<std::size_t>(kind)];
    bool const noBag = kind == Goals::GoalKind::BuyBag && outcome == StatusEnd::Outcome::Finished && !goals.bagBought;
    if (!noBag && (outcome == StatusEnd::Outcome::Arrived || outcome == StatusEnd::Outcome::Finished))
        failed = 0;
    else if (StatusEnd::Judged(outcome))
        failed = std::max<std::uint32_t>(nowMs, 1);
}

// What a planned status's end reads besides the goal (3.19 (4)), on the bot's map thread
struct PlannedFacts
{
    std::uint32_t status{0};  // StatusEnd::STATUS_*
    std::uint32_t nowMs{0};
    bool owns{false};           // Planner::Owns: no cohort bot, no bot whose planner a test holds
    bool inFallback{false};     // the idle step's fallback status (NoteFallback)
    bool sick{false};           // a sick bot rests until the sickness ends (Upkeep::RestEnds)
    std::uint32_t statusMs{0};  // how long the status runs (a fallback's length)
    float healthPercent{100.0f};
    float manaPercent{-1.0f};  // -1: no mana
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
    constexpr float MS_PER_MINUTE = 60000.0f;
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

// The running goal's executor ended: its key stalls when the log is as it was at the goal's start; a changed log
// lifts every stall. A grind, gather or money goal that gained XP made progress: it does not stall, and the others
// stay stalled (a kill on the way lifts nothing). A run ended from outside (NoteStatusEnd) closes without a stall;
// a changed log still lifts every stall
inline void EndRun(BotGoals& goals, std::uint64_t log, std::uint64_t xp = 0)
{
    if (!goals.run.running)
        return;
    goals.run.running = false;
    bool const outside = std::exchange(goals.run.outside, false);
    if (log != goals.run.startLog)
    {
        goals.stalled.reset();
        return;
    }
    if (outside)
        return;
    if (goals.run.xpCounts && xp > goals.run.startXp)
        return;
    if (!goals.stalled || goals.stalled->log != log)
        goals.stalled = Stalled{log, {}};
    std::vector<std::uint64_t>& keys = goals.stalled->keys;
    if (std::find(keys.begin(), keys.end(), goals.run.key) != keys.end())
        return;
    goals.stallRow = StallRow{goals.run.kind, goals.run.judged, goals.run.lastOutcome, goals.run.lastNote};
    if (keys.size() >= MAX_STALLED)
        keys.erase(keys.begin());
    keys.push_back(goals.run.key);
}

// How a goal's start went (3.5); anything but Started changed nothing
enum class StartResult : std::uint8_t
{
    Started,
    NoExecutor,  // nothing starts it from here (Executor::None, a circuit without a plan)
    Stalled,     // its key stalled at this log (MayStart)
    Refused,     // its executor cannot start it now (no place, the errand entry's refusal)
    Retry        // its kind failed as an interlude within the retry time (RetryWaits)
};

// What a goal's start reads besides the goal: its stall key, the log and XP now
struct StartFacts
{
    std::uint64_t key{0};
    std::uint64_t log{0};
    std::uint64_t xp{0};
    bool stallRule{true};  // false: the bridge's order, the partner's wish
};

// One goal started by its executor (3.5): `start` (StartGoal) returns false having ended nothing, and then the goals
// are as before. A start ends the running status as interrupted, which marks the running run outside
// (NoteStatusEnd), so that run closes without a stall and the new goal's run begins unmarked. An interlude begins no
// run: the goal it interrupts keeps its run and progress. The caller sets the slot (Slot::Set keeps that goal)
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
    if (facts.stallRule && !MayStart(goals, facts.key, facts.log))
        return StartResult::Stalled;
    QuestGoal progress = std::exchange(goals.quest, {});  // a goal's circuit progress starts anew
    if (!start())
    {
        goals.quest = std::move(progress);
        return StartResult::Refused;
    }
    EndRun(goals, facts.log, facts.xp);
    goals.run = {facts.key, facts.log, facts.xp, XpIsProgress(kind), true, false, kind};
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

// The idle step fell back to the weighted pick: its status is a fallback until it ends (NoteStatusEnd)
inline void NoteFallback(BotGoals& goals, Fallback reason, std::uint32_t nowMs)
{
    goals.lastFallback = reason;
    goals.lastFallbackMs = nowMs;
    goals.inFallback = true;
}

// What a fresh plan does to the running status (3.5): nothing; a replan of the planner's goal (ReplanDecision); or,
// during a fallback status, the plan's best option that can start. A goal-less status that is no fallback (a
// sickness rest, a command, a test's order) and an external goal stay
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
    return goals.inFallback && goals.plan && goals.plan->ranked.ok ? FreshPlan::Take : FreshPlan::Keep;
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
// idle, in either group (a weighted bot has no planner job whose answer would publish it later)
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
