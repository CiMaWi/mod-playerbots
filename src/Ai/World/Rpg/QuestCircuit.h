/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTCIRCUIT_H
#define _PLAYERBOT_QUESTCIRCUIT_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "PlannerIndexes.h"
#include "PlannerPool.h"
#include "QuestIndex.h"
#include "QuestStats.h"

// The questing circuit (travel plan WT13b, PLAN 3.9, WT.21): at its hub a bot takes most fitting quests while its log
// has room, chooses this trip's set from its log, does the objectives that lie together in one loop, turns the quests
// in together and moves on when the hub runs dry. When the log is full it keeps the quests it can work on here, defers
// the ones not worth this trip and abandons the ones it cannot do from here. Deliveries to another hub, and to an ender
// too far out of this trip's way, are carried there when the bot moves on. Natural, not optimal: a sensible loop, not
// the shortest one. A pure function of the bot's snapshot and the immutable indexes: it runs on a planner worker, its
// plan is rechecked on the bot's map thread and applied by WT16. No game header: tests/quest-circuit-selftest.cpp runs
// it on synthetic worlds.
// The value model is simple and replaceable (WT14 builds the full one): value = XP + copper x need, per minute of
// travel and action. Today's pick (a random log quest, its objectives in random order, a trip to the ender after each
// quest) is modelled by BaselineYards, printed as information only (WT.21).
namespace Planner
{
namespace Circuit
{
// All values are estimates [E] that WT14/WT17 tune
constexpr std::uint32_t GREY_DIFF = 4;          // a quest this many levels below the bot is grey: the core's
                                                // Quests.LowLevelHideDiff default; the server passes its own value
constexpr std::int32_t MAX_LEVELS_ABOVE = 3;    // QUEST_MAX_LEVELS_ABOVE_BOT (QuestLevelRules.h), as today
constexpr std::size_t LOG_RESERVE = 2;          // free log slots kept, as today's drop pass keeps them
constexpr float HUB_GIVER_YARDS = 120.0f;       // the quest givers this near the bot make its hub
constexpr float FAR_YARDS = 4000.0f;            // objectives farther from the hub (about a zone's width) belong
                                                // to another hub, and so does an ender this far
constexpr float GREY_SOON_MINUTES = 5.0f;       // a grey log quest needing more minutes than this is abandoned
constexpr float MERGE_YARDS = 80.0f;            // objective points this close are done at one stop
constexpr std::size_t EXACT_STOPS = 8;          // stops ordered exactly up to this; nearest neighbour + 2-opt above
constexpr std::size_t TWO_OPT_PASSES = 16;      // bound on the 2-opt passes
constexpr float TWO_OPT_GAIN = 0.01f;           // yards a 2-opt move must save
constexpr std::uint32_t STATS_MIN_SAMPLES = 5;  // a statistics cell counts with this many samples
constexpr float DEATHS_PER_COMPLETION_LIMIT = 0.5f;  // more (divided by caution) make a quest too risky to take
constexpr float KILL_SECONDS = 30.0f;                // per kill, finding the target included
constexpr float ITEM_SECONDS = 45.0f;                // per item, drop rates included
constexpr float OBJECT_SECONDS = 20.0f;              // per object used
constexpr float EXPLORE_SECONDS = 10.0f;             // per place explored
constexpr float TURNIN_SECONDS = 15.0f;              // per quest at its ender
constexpr float MIN_MINUTES = 0.5f;                  // floor of a quest's minutes in its value per minute
constexpr float RETURN_RATIO = 0.5f;       // a stop's quests, or an ender camp's turn-ins, wait for later below this
                                           // share of the trip's rate (it stands in for the next circuit's, rule 5)
constexpr float DEFAULT_RUN_SPEED = 7.0f;  // yards per second when the snapshot has none
constexpr float OVERTIME_FACTOR = 3.0f;    // a log quest older than this x its median is abandoned
constexpr float OVERTIME_MIN_MINUTES = 20.0f;    // and at least this old
constexpr float DEATHS_FACTOR = 3.0f;            // a log quest with more deaths than this x its deaths per completion
constexpr std::uint32_t DEATHS_MIN = 3;          // and at least this many is abandoned
constexpr std::uint32_t MOVE_MIN_QUESTS = 2;     // a camp worth moving to offers this many takeable quests
constexpr std::size_t MOVE_CAMPS_CHECKED = 200;  // camps tried for the bracket move, nearest first
constexpr std::uint32_t CROWD_BOTS_LIMIT = 25;   // a camp whose area holds more bots is skipped (WT14: crowding model)
constexpr std::uint32_t BASELINE_SEEDS = 50;     // runs of today's random pick behind the baseline's mean
constexpr std::uint32_t SLOT_COMPLETE = 0x01;    // QUEST_STATE_COMPLETE in the log slot's state
constexpr std::int32_t KILL_SLOTS = 4;           // creature or object counters in a log slot
constexpr std::uint32_t NONE = 0xFFFFFFFFu;
constexpr std::uint16_t INSTANCE_FLAGS = QuestFlag::DUNGEON | QuestFlag::RAID | QuestFlag::PVP;
constexpr std::uint16_t GROUP_FLAGS = QuestFlag::ELITE | QuestFlag::GROUP;
constexpr std::uint16_t REPEAT_FLAGS =
    QuestFlag::DAILY | QuestFlag::WEEKLY | QuestFlag::REPEATABLE | QuestFlag::SEASONAL;
}  // namespace Circuit

// Weights from the bot's traits (WT14 fills them); the defaults are neutral
struct CircuitWeights
{
    float xp{1.0f};            // per XP point
    float copper{0.5f};        // XP worth of a copper: the need factor (a poor bot raises it)
    float thoroughness{1.0f};  // above 1 a bot takes and finishes more quests per circuit
    float caution{1.0f};       // above 1 a bot avoids risky quests sooner
};

// What the bot knows about a log quest beyond its slot (WT16 provides it; the harness supplies it)
struct QuestProgress
{
    std::uint32_t quest{0};
    float elapsedMinutes{0.0f};
    std::uint32_t deaths{0};
};

// Seconds from one place to another, negative when unreachable
using TravelCost = std::function<float(Place const&, Place const&)>;

struct CircuitContext
{
    QuestIndex const* quests{nullptr};
    PlannerIndexes const* indexes{nullptr};  // zones for the bracket move and hubs for the reasons; may be null
    QuestStatsView const* stats{nullptr};
    CrowdingSnapshot const* crowding{nullptr};
    CircuitWeights weights;
    std::vector<std::uint32_t> protectedQuests;  // never abandoned: a group's or the bridge's goal (WT16 fills)
    std::vector<QuestProgress> progress;
    std::uint32_t greyDiff{Circuit::GREY_DIFF};
    TravelCost travel;  // moving on; empty = straight yards at the run speed (same map only)
};

enum class SkipReason : std::uint8_t
{
    Rewarded,
    InLog,
    Repeatable,
    Limits,  // race, class or maximum level
    TooHigh,
    Grey,
    Instance,  // dungeon, raid or PvP
    Solo,      // elite or group quest while solo
    Prerequisite,
    Exclusive,
    Breadcrumb,  // its target is done or takeable
    Risky,
    NoArea,
    TooFar,
    LogFull,
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(SkipReason::Count)> SKIP_NAMES = {
    "rewarded",         "in log",       "repeatable", "race/class/max level", "too high", "grey",    "instance",
    "elite/group solo", "prerequisite", "exclusive",  "breadcrumb",           "risky",    "no area", "too far",
    "log full"};

enum class AbandonReason : std::uint8_t
{
    Grey,
    Overtime,
    Deaths,
    LogSpace,  // the log is full and the bot cannot work on the quest from here
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(AbandonReason::Count)> ABANDON_NAMES = {
    "grey, nothing left in reach", "far over the usual time", "far over the usual deaths",
    "log space: not doable from here (far away, or elite/group solo)"};

enum class MoveReason : std::uint8_t
{
    None,
    Chain,
    Delivery,  // a carried quest's ender: a delivery or a breadcrumb
    Bracket,
    Nowhere,
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(MoveReason::Count)> MOVE_NAMES = {
    "none", "chain", "delivery", "zone bracket", "nowhere"};

inline char const* SkipName(SkipReason reason) { return SKIP_NAMES[static_cast<std::size_t>(reason)]; }
inline char const* AbandonName(AbandonReason reason) { return ABANDON_NAMES[static_cast<std::size_t>(reason)]; }
inline char const* MoveName(MoveReason reason) { return MOVE_NAMES[static_cast<std::size_t>(reason)]; }

struct CircuitTake
{
    std::uint32_t quest{0};
    std::uint32_t giver{Circuit::NONE};  // place index
    float valuePerMinute{0.0f};
    bool carried{false};  // no objective, its ender at another hub: delivered when moving on
};

struct CircuitTask
{
    std::uint32_t quest{0};
    std::uint8_t objective{0};  // index into the record's objectives
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct CircuitStop
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::vector<CircuitTask> tasks;
    std::vector<std::uint32_t> completes;  // quests whose last objective is here
};

struct CircuitTurnIn
{
    std::uint32_t quest{0};
    std::uint32_t ender{Circuit::NONE};  // place index
    std::uint32_t camp{NO_CAMP};
};

struct CircuitSkip
{
    std::uint32_t quest{0};
    SkipReason reason{SkipReason::Count};
};

struct CircuitAbandon
{
    std::uint32_t quest{0};
    AbandonReason reason{AbandonReason::Count};
};

struct CircuitMove
{
    MoveReason reason{MoveReason::None};
    std::uint32_t camp{NO_CAMP};
    std::uint32_t hub{NO_HUB};
    std::uint32_t zone{0};
    std::uint32_t quest{0};  // the chain's or breadcrumb's quest
    float seconds{0.0f};
    std::string word;  // Nowhere: the failed test as a log word (QuestCircuitLog.h)
};

struct CircuitPlan
{
    bool ok{false};
    std::string failure;
    Place start;
    std::uint32_t homeCamp{NO_CAMP};
    std::vector<CircuitTake> take;
    std::vector<CircuitSkip> skipped;  // the hub's quests not taken
    std::vector<CircuitStop> stops;    // in walking order
    std::vector<CircuitTurnIn> turnIns;
    std::vector<std::uint32_t> enderCamps;  // in visiting order, one visit each
    std::vector<std::uint32_t> followUps;   // taken at the turn-in stop
    std::vector<std::uint32_t> deferred;    // log quests (taken now or before) left for a later trip
    std::vector<std::uint32_t> carried;     // turned in at another hub when the bot moves on, not on this loop
    std::vector<CircuitAbandon> abandon;
    CircuitMove moveOn;
    Place returnTo;
    float yards{0.0f};
    float minutes{0.0f};
    float value{0.0f};
    std::vector<std::string> reasons;  // one line per decision
};

namespace Circuit
{
struct Point
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

inline float Yards(Point const& a, Point const& b) { return std::hypot(a.x - b.x, a.y - b.y); }

inline Point PointOf(QuestPlace const& place) { return {place.x, place.y, place.z}; }

inline float PathYards(Point const& start, std::vector<Point> const& points, std::vector<std::uint32_t> const& order,
                       Point const& end)
{
    float yards = 0.0f;
    Point at = start;
    for (std::uint32_t const i : order)
    {
        yards += Yards(at, points[i]);
        at = points[i];
    }
    return yards + Yards(at, end);
}

// The shortest path from start through every point to end (Held-Karp over the subsets); ties to the lower index
inline std::vector<std::uint32_t> ExactOrder(Point const& start, std::vector<Point> const& points, Point const& end)
{
    std::size_t const n = points.size();
    if (!n)
        return {};
    constexpr float INF = std::numeric_limits<float>::infinity();
    std::size_t const full = std::size_t{1} << n;
    std::vector<float> cost(full * n, INF);
    std::vector<std::uint8_t> parent(full * n, 0xFF);
    for (std::size_t j = 0; j < n; ++j)
        cost[(std::size_t{1} << j) * n + j] = Yards(start, points[j]);
    for (std::size_t mask = 1; mask < full; ++mask)
        for (std::size_t j = 0; j < n; ++j)
        {
            float const here = cost[mask * n + j];
            if (!(mask & (std::size_t{1} << j)) || here == INF)
                continue;
            for (std::size_t k = 0; k < n; ++k)
            {
                if (mask & (std::size_t{1} << k))
                    continue;
                std::size_t const next = (mask | (std::size_t{1} << k)) * n + k;
                float const value = here + Yards(points[j], points[k]);
                if (value < cost[next])
                {
                    cost[next] = value;
                    parent[next] = static_cast<std::uint8_t>(j);
                }
            }
        }
    std::size_t last = 0;
    float best = INF;
    for (std::size_t j = 0; j < n; ++j)
    {
        float const value = cost[(full - 1) * n + j] + Yards(points[j], end);
        if (value < best)
        {
            best = value;
            last = j;
        }
    }
    std::vector<std::uint32_t> order(n);
    if (!(best < INF))  // a non-finite position: no path to rebuild, keep the given order
    {
        for (std::uint32_t i = 0; i < n; ++i)
            order[i] = i;
        return order;
    }
    std::size_t mask = full - 1;
    for (std::size_t i = n; i-- > 0;)
    {
        order[i] = static_cast<std::uint32_t>(last);
        std::uint8_t const previous = parent[mask * n + last];
        mask &= ~(std::size_t{1} << last);
        last = previous;
    }
    return order;
}

// Nearest neighbour from the start, then 2-opt with both ends fixed
inline std::vector<std::uint32_t> HeuristicOrder(Point const& start, std::vector<Point> const& points, Point const& end)
{
    std::size_t const n = points.size();
    std::vector<std::uint32_t> order;
    order.reserve(n);
    std::vector<bool> used(n, false);
    Point at = start;
    for (std::size_t step = 0; step < n; ++step)
    {
        std::size_t best = n;
        float bestYards = std::numeric_limits<float>::infinity();
        for (std::size_t i = 0; i < n; ++i)
        {
            float const yards = Yards(at, points[i]);
            if (!used[i] && yards < bestYards)
            {
                best = i;
                bestYards = yards;
            }
        }
        used[best] = true;
        order.push_back(static_cast<std::uint32_t>(best));
        at = points[best];
    }
    auto const pointAt = [&](std::size_t i) -> Point const&
    { return i == 0 ? start : (i == n + 1 ? end : points[order[i - 1]]); };
    for (std::size_t pass = 0; pass < TWO_OPT_PASSES; ++pass)
    {
        bool improved = false;
        for (std::size_t i = 1; i + 1 <= n; ++i)
            for (std::size_t j = i + 1; j <= n; ++j)
            {
                float const before = Yards(pointAt(i - 1), pointAt(i)) + Yards(pointAt(j), pointAt(j + 1));
                float const after = Yards(pointAt(i - 1), pointAt(j)) + Yards(pointAt(i), pointAt(j + 1));
                if (after + TWO_OPT_GAIN < before)
                {
                    std::reverse(order.begin() + static_cast<std::ptrdiff_t>(i - 1),
                                 order.begin() + static_cast<std::ptrdiff_t>(j));
                    improved = true;
                }
            }
        if (!improved)
            break;
    }
    return order;
}

inline std::vector<std::uint32_t> OrderStops(Point const& start, std::vector<Point> const& points, Point const& end)
{
    return points.size() <= EXACT_STOPS ? ExactOrder(start, points, end) : HeuristicOrder(start, points, end);
}

// Groups of task points joined when two lie within MERGE_YARDS (single link), in the order of their first member
inline std::vector<std::vector<std::uint32_t>> ClusterTasks(std::vector<CircuitTask> const& tasks)
{
    std::vector<std::uint32_t> root(tasks.size());
    for (std::uint32_t i = 0; i < root.size(); ++i)
        root[i] = i;
    auto const find = [&root](std::uint32_t i)
    {
        while (root[i] != i)
            i = root[i] = root[root[i]];
        return i;
    };
    for (std::uint32_t i = 0; i < tasks.size(); ++i)
        for (std::uint32_t j = i + 1; j < tasks.size(); ++j)
            if (Yards({tasks[i].x, tasks[i].y, 0.0f}, {tasks[j].x, tasks[j].y, 0.0f}) <= MERGE_YARDS)
            {
                std::uint32_t const a = find(i);
                std::uint32_t const b = find(j);
                if (a != b)
                    root[std::max(a, b)] = std::min(a, b);
            }
    std::vector<std::vector<std::uint32_t>> groups;
    std::vector<std::uint32_t> groupOf(tasks.size(), NONE);
    for (std::uint32_t i = 0; i < tasks.size(); ++i)
    {
        std::uint32_t const r = find(i);
        if (groupOf[r] == NONE)
        {
            groupOf[r] = static_cast<std::uint32_t>(groups.size());
            groups.emplace_back();
        }
        groups[groupOf[r]].push_back(i);
    }
    return groups;
}

inline bool Contains(std::vector<std::uint32_t> const& sorted, std::uint32_t id)
{
    return std::binary_search(sorted.begin(), sorted.end(), id);
}

inline std::int32_t QuestLevel(QuestRecord const& quest, std::uint8_t botLevel)
{
    return quest.level > 0 ? quest.level : static_cast<std::int32_t>(botLevel);
}

inline float ObjectiveSeconds(ObjectiveKind kind)
{
    switch (kind)
    {
        case ObjectiveKind::Kill:
            return KILL_SECONDS;
        case ObjectiveKind::Item:
            return ITEM_SECONDS;
        case ObjectiveKind::Object:
            return OBJECT_SECONDS;
        case ObjectiveKind::Explore:
            return EXPLORE_SECONDS;
        default:
            return 0.0f;
    }
}

// A deterministic generator for the baseline (SplitMix64): the same seed gives the same walk on every platform
struct SplitMix
{
    std::uint64_t state{0};

    std::uint64_t Next()
    {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    template <typename T>
    void Shuffle(std::vector<T>& list)
    {
        for (std::size_t i = list.size(); i > 1; --i)
            std::swap(list[i - 1], list[Next() % i]);
    }
};

// The planner's working state for one snapshot
class CircuitBuilder
{
public:
    CircuitBuilder(Snapshot const& bot, CircuitContext const& context)
        : _bot(bot), _context(context), _quests(*context.quests), _weights(context.weights)
    {
    }

    CircuitPlan Run()
    {
        _plan.start = _bot.position;
        _start = {_bot.position.x, _bot.position.y, _bot.position.z};
        if (!Detail::Finite(_start.x, _start.y, _start.z))
            return Fail("bad position");
        _speed = std::isfinite(_bot.runSpeed) && _bot.runSpeed > 0.0f ? _bot.runSpeed : DEFAULT_RUN_SPEED;
        _band = LevelBandOf(_bot.level);
        _rewarded = _bot.rewardedQuests;
        if (!std::is_sorted(_rewarded.begin(), _rewarded.end()))
            std::sort(_rewarded.begin(), _rewarded.end());
        _protected = _context.protectedQuests;
        std::sort(_protected.begin(), _protected.end());
        ReadLog();
        FindHub();
        Abandon();
        Choose();
        BuildCircuit();
        FollowUps();
        MoveOn();
        _plan.ok = true;
        return std::move(_plan);
    }

private:
    struct LogQuest
    {
        QuestSlot slot;
        QuestRecord const* record{nullptr};
    };

    // Where a quest's remaining objectives and its ender lie, as seen from the hub
    struct Placement
    {
        std::vector<CircuitTask> tasks;
        bool hasObjectives{false};
        bool unplaced{false};  // an objective without an area on this map: done only if it falls out of a stop
        float farthest{0.0f};
        std::uint32_t ender{NONE};
        float enderYards{0.0f};
        float actionSeconds{0.0f};
    };

    struct Candidate
    {
        QuestRecord const* record{nullptr};
        std::uint32_t giver{NONE};
        Placement placement;
        float value{0.0f};
        float valuePerMinute{0.0f};
        bool carried{false};
    };

    // A quest in the circuit: taken now or already in the log
    struct Member
    {
        QuestRecord const* record{nullptr};
        Placement placement;
        float value{0.0f};
        bool fromLog{false};
        bool turnInHere{true};  // false: its ender is at another hub, it is carried there when the bot moves on
        bool later{false};      // its ender camp is too far out of this trip's way: carried like the above
    };

    CircuitPlan Fail(char const* why)
    {
        _plan.ok = false;
        _plan.failure = why;
        return std::move(_plan);
    }

    void Reason(std::string line) { _plan.reasons.push_back(std::move(line)); }

    void ReadLog()
    {
        for (QuestSlot const& slot : _bot.quests)
        {
            if (!slot.quest)
                continue;
            _logIds.push_back(slot.quest);
            if (QuestRecord const* record = _quests.Find(slot.quest))
                _log.push_back({slot, record});
        }
        std::sort(_logIds.begin(), _logIds.end());
        std::sort(_log.begin(), _log.end(),
                  [](LogQuest const& a, LogQuest const& b) { return a.slot.quest < b.slot.quest; });
        for (LogQuest const& quest : _log)
            if (quest.record->exclusiveGroup > 0)
                _takenGroups.push_back(quest.record->exclusiveGroup);
        for (std::uint32_t const id : _rewarded)
            if (QuestRecord const* record = _quests.Find(id))
                if (record->exclusiveGroup > 0)
                    _takenGroups.push_back(record->exclusiveGroup);
        std::sort(_takenGroups.begin(), _takenGroups.end());
    }

    void FindHub()
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> offered;  // quest, nearest giver
        for (std::uint32_t const place : _quests.GiversNear(_bot.position.map, _start.x, _start.y, HUB_GIVER_YARDS))
        {
            if (_plan.homeCamp == NO_CAMP)
                _plan.homeCamp = _quests.places[place].camp;
            for (std::uint32_t const quest : _quests.QuestsGivenAt(place))
                offered.emplace_back(quest, place);
        }
        std::stable_sort(offered.begin(), offered.end(),
                         [](auto const& a, auto const& b) { return a.first < b.first; });
        offered.erase(std::unique(offered.begin(), offered.end(),
                                  [](auto const& a, auto const& b) { return a.first == b.first; }),
                      offered.end());
        _offered = std::move(offered);
    }

    bool InLog(std::uint32_t id) const { return Contains(_logIds, id); }

    bool Rewarded(std::uint32_t id) const { return Contains(_rewarded, id); }

    // Whether the bot may take the quest now; the first rule it fails
    std::optional<SkipReason> Takeable(QuestRecord const& quest, bool breadcrumbRule = true) const
    {
        if (Rewarded(quest.id))
            return SkipReason::Rewarded;
        if (InLog(quest.id))
            return SkipReason::InLog;
        if (quest.flags & REPEAT_FLAGS)
            return SkipReason::Repeatable;
        if (_bot.level < quest.minLevel)
            return SkipReason::TooHigh;
        if (!Eligible(quest, _bot.race, _bot.cls, _bot.level))
            return SkipReason::Limits;
        std::int32_t const level = QuestLevel(quest, _bot.level);
        if (level > static_cast<std::int32_t>(_bot.level) + MAX_LEVELS_ABOVE)
            return SkipReason::TooHigh;
        if (IsGrey(quest))
            return SkipReason::Grey;
        if (quest.flags & INSTANCE_FLAGS)
            return SkipReason::Instance;
        if ((quest.flags & GROUP_FLAGS) && _bot.groupSize <= 1)
            return SkipReason::Solo;
        if ((quest.prev > 0 && !Rewarded(static_cast<std::uint32_t>(quest.prev))) ||
            (quest.prev < 0 && !InLog(static_cast<std::uint32_t>(-quest.prev))))
            return SkipReason::Prerequisite;
        if (quest.exclusiveGroup > 0 &&
            std::binary_search(_takenGroups.begin(), _takenGroups.end(), quest.exclusiveGroup))
            return SkipReason::Exclusive;
        if (breadcrumbRule && quest.breadcrumbFor)
        {
            QuestRecord const* target = _quests.Find(quest.breadcrumbFor);
            if (!target || !Takeable(*target, false).has_value() || Rewarded(target->id) || InLog(target->id))
                return SkipReason::Breadcrumb;
        }
        return std::nullopt;
    }

    bool IsGrey(QuestRecord const& quest) const
    {
        return static_cast<std::int32_t>(_bot.level) >
               QuestLevel(quest, _bot.level) + static_cast<std::int32_t>(_context.greyDiff);
    }

    // The quest's remaining objectives at their area nearest the hub, its nearest ender and its action time
    Placement Locate(QuestRecord const& quest, QuestSlot const* slot) const
    {
        return Locate(quest, slot, _bot.position);
    }

    // The same, seen from a hub around `from` (a camp the bot may move on to)
    Placement Locate(QuestRecord const& quest, QuestSlot const* slot, Place const& from) const
    {
        Point const origin{from.x, from.y, from.z};
        Placement placement;
        bool const complete = slot && (slot->state & SLOT_COMPLETE);
        for (std::size_t i = 0; i < quest.objectives.size() && !complete; ++i)
        {
            QuestObjective const& objective = quest.objectives[i];
            if (objective.kind == ObjectiveKind::Turnin)
                continue;
            placement.hasObjectives = true;
            std::uint32_t left = std::max<std::uint32_t>(objective.count, 1);
            bool const counted = objective.kind == ObjectiveKind::Kill || objective.kind == ObjectiveKind::Object;
            if (slot && counted && objective.poiIndex >= 0 && objective.poiIndex < KILL_SLOTS)
            {
                std::uint32_t const done = slot->counters[static_cast<std::size_t>(objective.poiIndex)];
                left = done >= objective.count ? 0 : objective.count - done;
            }
            if (!left)
                continue;
            AreaPoint const* best = nullptr;
            float bestYards = std::numeric_limits<float>::infinity();
            for (AreaPoint const& area : objective.areas)
            {
                float const yards = Yards(origin, {area.x, area.y, area.z});
                if (area.mapId == from.map && yards < bestYards)
                {
                    best = &area;
                    bestYards = yards;
                }
            }
            placement.actionSeconds += static_cast<float>(left) * ObjectiveSeconds(objective.kind);
            if (!best)
            {
                placement.unplaced = true;
                continue;
            }
            placement.farthest = std::max(placement.farthest, bestYards);
            placement.tasks.push_back({quest.id, static_cast<std::uint8_t>(i), best->x, best->y, best->z});
        }
        placement.enderYards = std::numeric_limits<float>::infinity();
        for (std::uint32_t const place : quest.enders)
        {
            QuestPlace const& ender = _quests.places[place];
            float const yards = Yards(origin, PointOf(ender));
            if (ender.mapId == from.map && yards < placement.enderYards)
            {
                placement.ender = place;
                placement.enderYards = yards;
            }
        }
        placement.actionSeconds += TURNIN_SECONDS;
        return placement;
    }

    float Value(QuestRecord const& quest) const
    {
        return static_cast<float>(quest.xpBase) * _weights.xp +
               static_cast<float>(std::max(quest.copperReward, 0)) * _weights.copper;
    }

    QuestStatSummary const* Stats(std::uint32_t quest) const
    {
        if (!_context.stats)
            return nullptr;
        QuestStatSummary const* summary = _context.stats->Find(quest, _band);
        return summary && summary->samples >= STATS_MIN_SAMPLES ? summary : nullptr;
    }

    QuestProgress const* Progress(std::uint32_t quest) const
    {
        for (QuestProgress const& progress : _context.progress)
            if (progress.quest == quest)
                return &progress;
        return nullptr;
    }

    // The quest's minutes: the population's median when the cell has enough samples, else objectives and a round trip
    // to the farthest of its objective places and its ender here (a delivery's walk is its time)
    float Minutes(std::uint32_t quest, Placement const& placement) const
    {
        if (QuestStatSummary const* stats = Stats(quest); stats && stats->medianMinutes > 0.0f)
            return std::max(stats->medianMinutes, MIN_MINUTES);
        float trip = TurnsInHere(placement) ? placement.enderYards : 0.0f;
        for (CircuitTask const& task : placement.tasks)
            trip = std::max(trip, Yards(_start, {task.x, task.y, task.z}));
        float const seconds = placement.actionSeconds + 2.0f * trip / _speed;
        return std::max(seconds / 60.0f, MIN_MINUTES);
    }

    // What is left of the quest lies within the hub's reach: placed objectives within FAR_YARDS, or none left
    static bool WorkHere(Placement const& placement)
    {
        return placement.farthest <= FAR_YARDS && !(placement.unplaced && placement.tasks.empty());
    }

    // The quest is turned in on a loop from this hub: its ender stands here or at a camp that is no other hub. An ender
    // at another quest camp (the next hub), beyond FAR_YARDS or on another map is reached when the bot moves on
    bool TurnsInHere(Placement const& placement) const
    {
        if (placement.ender == NONE || placement.enderYards > FAR_YARDS)
            return false;
        std::uint32_t const camp = _quests.places[placement.ender].camp;
        return camp == NO_CAMP || camp == _plan.homeCamp || _quests.camps[camp].quests.empty() ||
               AtHub(_quests.camps[camp]);
    }

    void Carry(std::uint32_t quest, char const* why)
    {
        _plan.carried.push_back(quest);
        Reason("carry " + std::to_string(quest) + ": " + why);
    }

    // Elite, group and instance quests wait for a group
    bool NeedsGroup(QuestRecord const& quest) const
    {
        return (quest.flags & (GROUP_FLAGS | INSTANCE_FLAGS)) && _bot.groupSize <= 1;
    }

    void Skip(std::uint32_t quest, SkipReason reason) { _plan.skipped.push_back({quest, reason}); }

    void AbandonQuest(std::uint32_t quest, AbandonReason reason)
    {
        _plan.abandon.push_back({quest, reason});
        _abandoned.push_back(quest);
        Reason("abandon " + std::to_string(quest) + ": " + AbandonName(reason));
    }

    bool Abandoned(std::uint32_t quest) const
    {
        return std::find(_abandoned.begin(), _abandoned.end(), quest) != _abandoned.end();
    }

    // Grey quests not finished soon; quests far over the population's time or deaths. Never a protected quest
    void Abandon()
    {
        for (LogQuest const& quest : _log)
        {
            std::uint32_t const id = quest.slot.quest;
            if ((quest.slot.state & SLOT_COMPLETE) || Contains(_protected, id))
                continue;
            if (IsGrey(*quest.record))
            {
                Placement const placement = Locate(*quest.record, &quest.slot);
                if (!WorkHere(placement) || placement.unplaced || placement.tasks.empty() ||
                    Minutes(id, placement) > GREY_SOON_MINUTES)
                {
                    AbandonQuest(id, AbandonReason::Grey);
                    continue;
                }
            }
            QuestStatSummary const* stats = Stats(id);
            QuestProgress const* progress = Progress(id);
            if (!stats || !progress)
                continue;
            if (stats->medianMinutes > 0.0f && progress->elapsedMinutes >= OVERTIME_MIN_MINUTES &&
                progress->elapsedMinutes > OVERTIME_FACTOR * stats->medianMinutes)
                AbandonQuest(id, AbandonReason::Overtime);
            else if (progress->deaths >= DEATHS_MIN && stats->deathsPerCompletion >= 0.0f &&
                     static_cast<float>(progress->deaths) > DEATHS_FACTOR * stats->deathsPerCompletion)
                AbandonQuest(id, AbandonReason::Deaths);
        }
    }

    // Whether the bot would take the quest at a hub around `from`: takeable, its objectives placed within FAR_YARDS of
    // it, not too risky. The first rule it fails; the placement seen from there when it passes. Choose takes by this,
    // and moving on goes only to where it holds (review 5)
    std::optional<SkipReason> WouldTake(QuestRecord const& quest, Place const& from,
                                        Placement* placement = nullptr) const
    {
        if (std::optional<SkipReason> const why = Takeable(quest))
            return why;
        Placement located = Locate(quest, nullptr, from);
        if (located.hasObjectives && located.tasks.empty())
            return SkipReason::NoArea;
        if (located.hasObjectives && !WorkHere(located))
            return SkipReason::TooFar;
        if (QuestStatSummary const* stats = Stats(quest.id);
            stats && stats->deathsPerCompletion >= 0.0f &&
            stats->deathsPerCompletion > DEATHS_PER_COMPLETION_LIMIT / Positive(_weights.caution))
            return SkipReason::Risky;
        if (placement)
            *placement = std::move(located);
        return std::nullopt;
    }

    // Most of the hub's quests, as a player takes them: takeable, their objectives within FAR_YARDS, not too risky, as
    // many as the log holds. The best value per minute first when the log is short of room; quests the bot cannot do
    // from here make room. Which of them this trip does is chosen later (BuildCircuit)
    void Choose()
    {
        std::vector<Candidate> candidates;
        for (auto const& [id, giver] : _offered)
        {
            QuestRecord const* record = _quests.Find(id);
            if (!record)
                continue;
            Candidate candidate{record, giver, {}};
            if (std::optional<SkipReason> const why = WouldTake(*record, _bot.position, &candidate.placement))
            {
                Skip(id, *why);
                continue;
            }
            Placement const& placement = candidate.placement;
            candidate.carried = !placement.hasObjectives && !TurnsInHere(placement);
            candidate.value = Value(*record);
            candidate.valuePerMinute = candidate.value / Minutes(id, placement);
            candidates.push_back(std::move(candidate));
        }
        // Carried quests last; the rest by value per minute, ties by id
        std::sort(candidates.begin(), candidates.end(),
                  [](Candidate const& a, Candidate const& b)
                  {
                      if (a.carried != b.carried)
                          return !a.carried;
                      if (a.valuePerMinute != b.valuePerMinute)
                          return a.valuePerMinute > b.valuePerMinute;
                      return a.record->id < b.record->id;
                  });
        std::vector<std::int32_t> groups;
        std::vector<Candidate> wanted;
        for (Candidate& candidate : candidates)
        {
            std::int32_t const group = candidate.record->exclusiveGroup;
            if (group > 0 && std::find(groups.begin(), groups.end(), group) != groups.end())
            {
                Skip(candidate.record->id, SkipReason::Exclusive);
                continue;
            }
            if (group > 0)
                groups.push_back(group);
            wanted.push_back(std::move(candidate));
        }
        MakeRoom(wanted.size());
        std::size_t const free = FreeSlots();
        for (Candidate& candidate : wanted)
        {
            if (_plan.take.size() >= free)
            {
                Skip(candidate.record->id, SkipReason::LogFull);
                continue;
            }
            std::uint32_t const id = candidate.record->id;
            _plan.take.push_back({id, candidate.giver, candidate.valuePerMinute, candidate.carried});
            if (candidate.carried)
                _carriedTakes.push_back(id);
            else
            {
                bool const here = TurnsInHere(candidate.placement);
                _members.push_back({candidate.record, std::move(candidate.placement), candidate.value, false, here});
            }
        }
        std::sort(_plan.skipped.begin(), _plan.skipped.end(),
                  [](CircuitSkip const& a, CircuitSkip const& b) { return a.quest < b.quest; });
        for (CircuitTake const& take : _plan.take)
        {
            std::string const what = take.carried
                                         ? std::string("a delivery to another hub")
                                         : std::to_string(static_cast<int>(take.valuePerMinute)) + " value/min";
            Reason("take " + std::to_string(take.quest) + ": " + what);
        }
        for (CircuitSkip const& skip : _plan.skipped)
            Reason("skip " + std::to_string(skip.quest) + ": " + SkipName(skip.reason));
    }

    // The log's free slots after its abandons, keeping LOG_RESERVE free
    std::size_t FreeSlots() const
    {
        std::size_t const used = _logIds.size() - std::min(_logIds.size(), _abandoned.size());
        return QUEST_LOG_SLOTS > used + LOG_RESERVE ? QUEST_LOG_SLOTS - used - LOG_RESERVE : 0;
    }

    // A full log: the quests the bot cannot work on from here (far away, or elite/group while solo) give their slots
    // to the hub's, lowest value first, beyond the slots that complete log quests turned in here free (the hub's
    // quests wait for those; quests this trip completes are not counted, the conservative side).
    // Quests it can do here are kept (a later trip does them); never a complete or protected quest
    void MakeRoom(std::size_t wanted)
    {
        std::size_t freed = 0;  // complete log quests turned in on a loop from here
        for (LogQuest const& quest : _log)
            if ((quest.slot.state & SLOT_COMPLETE) && !Abandoned(quest.slot.quest) &&
                TurnsInHere(Locate(*quest.record, &quest.slot)))
                ++freed;
        if (wanted <= FreeSlots() + freed)
            return;
        std::vector<std::pair<float, std::uint32_t>> idle;  // value, quest
        for (LogQuest const& quest : _log)
        {
            std::uint32_t const id = quest.slot.quest;
            if ((quest.slot.state & SLOT_COMPLETE) || Contains(_protected, id) || Abandoned(id))
                continue;
            if (NeedsGroup(*quest.record) || !WorkHere(Locate(*quest.record, &quest.slot)))
                idle.emplace_back(Value(*quest.record), id);
        }
        std::sort(idle.begin(), idle.end());
        for (auto const& entry : idle)
        {
            if (wanted <= FreeSlots() + freed)
                break;
            AbandonQuest(entry.second, AbandonReason::LogSpace);
        }
    }

    static float Positive(float weight) { return std::isfinite(weight) && weight > 0.0f ? weight : 1.0f; }

    // A member left out of this trip stays in the log (taken now or before) and waits for a later trip
    void LeaveOut(std::size_t m)
    {
        std::uint32_t const id = _members[m].record->id;
        _plan.deferred.push_back(id);
        Reason("defer " + std::to_string(id) + ": its stops earn too little on this trip");
        _members.erase(_members.begin() + static_cast<std::ptrdiff_t>(m));
    }

    // Log quests join the circuit when what is left of them lies in reach and the bot can do it now. A quest with
    // nothing left to do whose ender is at another hub is carried there, never walked to from here
    void AddLogMembers()
    {
        for (LogQuest const& quest : _log)
        {
            if (Abandoned(quest.slot.quest) || (!(quest.slot.state & SLOT_COMPLETE) && NeedsGroup(*quest.record)))
                continue;
            Placement placement = Locate(*quest.record, &quest.slot);
            if (!WorkHere(placement))
                continue;
            bool const here = TurnsInHere(placement);
            if (placement.tasks.empty() && !here)
            {
                Carry(quest.slot.quest, "done, its ender is at another hub");
                continue;
            }
            _members.push_back({quest.record, std::move(placement), Value(*quest.record), true, here});
        }
        std::sort(_members.begin(), _members.end(),
                  [](Member const& a, Member const& b) { return a.record->id < b.record->id; });
    }

    struct Layout
    {
        std::vector<CircuitTask> tasks;
        std::vector<std::uint32_t> owner;  // the member of each task
        std::vector<std::vector<std::uint32_t>> clusters;
        std::vector<Point> centres;
        std::vector<std::uint32_t> order;
        std::vector<std::vector<std::uint32_t>> walk;  // per stop in order: its tasks in walking order
        Point end;
        float yards{0.0f};
    };

    Layout Lay(Point const& end) const
    {
        Layout layout;
        layout.end = end;
        for (std::uint32_t m = 0; m < _members.size(); ++m)
            for (CircuitTask const& task : _members[m].placement.tasks)
            {
                layout.tasks.push_back(task);
                layout.owner.push_back(m);
            }
        layout.clusters = ClusterTasks(layout.tasks);
        for (std::vector<std::uint32_t> const& cluster : layout.clusters)
        {
            Point centre;
            for (std::uint32_t const i : cluster)
            {
                centre.x += layout.tasks[i].x;
                centre.y += layout.tasks[i].y;
                centre.z += layout.tasks[i].z;
            }
            float const n = static_cast<float>(cluster.size());
            layout.centres.push_back({centre.x / n, centre.y / n, centre.z / n});
        }
        layout.order = OrderStops(_start, layout.centres, end);
        layout.yards = Walk(layout, layout.order, &layout.walk);
        return layout;
    }

    // The yards from the start through the stops in `order` to the layout's end, every task point walked: a stop's
    // points in their best order from where the bot comes in to the next stop's centre
    float Walk(Layout const& layout, std::vector<std::uint32_t> const& order,
               std::vector<std::vector<std::uint32_t>>* walk = nullptr) const
    {
        float yards = 0.0f;
        Point at = _start;
        std::vector<Point> points;
        for (std::size_t s = 0; s < order.size(); ++s)
        {
            std::vector<std::uint32_t> const& cluster = layout.clusters[order[s]];
            points.clear();
            for (std::uint32_t const i : cluster)
                points.push_back({layout.tasks[i].x, layout.tasks[i].y, layout.tasks[i].z});
            Point const next = s + 1 < order.size() ? layout.centres[order[s + 1]] : layout.end;
            if (walk)
                walk->emplace_back();
            for (std::uint32_t const j : OrderStops(at, points, next))
            {
                yards += Yards(at, points[j]);
                at = points[j];
                if (walk)
                    walk->back().push_back(cluster[j]);
            }
        }
        return yards + Yards(at, layout.end);
    }

    // The main ender camp: where most of the circuit's quests end (ties: nearer the bot, then the lower index)
    Point EndPoint()
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> counts;  // camp, quests
        for (Member const& member : _members)
        {
            if (!member.turnInHere)
                continue;
            std::uint32_t const camp = _quests.places[member.placement.ender].camp;
            auto it = std::find_if(counts.begin(), counts.end(), [camp](auto const& c) { return c.first == camp; });
            if (it == counts.end())
                counts.emplace_back(camp, 1);
            else
                ++it->second;
        }
        _mainCamp = NO_CAMP;
        float bestYards = std::numeric_limits<float>::infinity();
        std::uint32_t bestCount = 0;
        for (auto const& [camp, count] : counts)
        {
            QuestCamp const& c = _quests.camps[camp];
            float const yards = Yards(_start, {c.x, c.y, c.z});
            if (count > bestCount ||
                (count == bestCount && (yards < bestYards || (yards == bestYards && camp < _mainCamp))))
            {
                _mainCamp = camp;
                bestCount = count;
                bestYards = yards;
            }
        }
        if (_mainCamp == NO_CAMP)
            return _start;
        QuestCamp const& c = _quests.camps[_mainCamp];
        return {c.x, c.y, c.z};
    }

    // Leaving a stop out drops every member with a task there, and the stops only they need go with them: the value
    // per minute those quests earn for the walk and the time they add
    float LeaveOutRate(Layout const& layout, std::uint32_t cluster, std::vector<bool>& drop) const
    {
        drop.assign(_members.size(), false);
        float value = 0.0f;
        float action = 0.0f;
        for (std::uint32_t const i : layout.clusters[cluster])
        {
            std::uint32_t const m = layout.owner[i];
            if (drop[m])
                continue;
            drop[m] = true;
            value += _members[m].value;
            action += _members[m].placement.actionSeconds;
        }
        std::vector<std::uint32_t> rest;
        for (std::uint32_t const c : layout.order)
            if (std::any_of(layout.clusters[c].begin(), layout.clusters[c].end(),
                            [&](std::uint32_t i) { return !drop[layout.owner[i]]; }))
                rest.push_back(c);
        float const saving = layout.yards - Walk(layout, rest);
        return value / std::max((saving / _speed + action) / 60.0f, MIN_MINUTES);
    }

    // The camps besides the main one where members are turned in, ascending
    std::vector<std::uint32_t> OtherEnderCamps() const
    {
        std::vector<std::uint32_t> camps;
        for (Member const& member : _members)
        {
            if (!member.turnInHere)
                continue;
            std::uint32_t const camp = _quests.places[member.placement.ender].camp;
            if (camp != _mainCamp && std::find(camps.begin(), camps.end(), camp) == camps.end())
                camps.push_back(camp);
        }
        std::sort(camps.begin(), camps.end());
        return camps;
    }

    // The camps visited after the loop's end, nearest first from where the bot is; their walk added to `yards`
    std::vector<std::uint32_t> Tour(Point at, std::vector<std::uint32_t> camps, float& yards) const
    {
        std::vector<std::uint32_t> order;
        while (!camps.empty())
        {
            std::size_t best = 0;
            float bestYards = std::numeric_limits<float>::infinity();
            for (std::size_t i = 0; i < camps.size(); ++i)
            {
                QuestCamp const& c = _quests.camps[camps[i]];
                float const yards = Yards(at, {c.x, c.y, c.z});
                if (yards < bestYards)
                {
                    best = i;
                    bestYards = yards;
                }
            }
            QuestCamp const& c = _quests.camps[camps[best]];
            yards += bestYards;
            at = {c.x, c.y, c.z};
            order.push_back(camps[best]);
            camps.erase(camps.begin() + static_cast<std::ptrdiff_t>(best));
        }
        return order;
    }

    // Leaving an ender camp out of this trip carries its quests to a later visit: the value per minute its turn-ins
    // earn for the walk there and their time
    float CampLeaveOutRate(Point const& end, std::vector<std::uint32_t> const& camps, std::uint32_t camp) const
    {
        float value = 0.0f;
        float action = 0.0f;
        for (Member const& member : _members)
            if (member.turnInHere && _quests.places[member.placement.ender].camp == camp)
            {
                value += member.value;
                action += TURNIN_SECONDS;
            }
        std::vector<std::uint32_t> rest = camps;
        rest.erase(std::remove(rest.begin(), rest.end(), camp), rest.end());
        float all = 0.0f;
        float without = 0.0f;
        Tour(end, camps, all);
        Tour(end, rest, without);
        return value / std::max(((all - without) / _speed + action) / 60.0f, MIN_MINUTES);
    }

    void LeaveCampOut(std::uint32_t camp)
    {
        for (Member& member : _members)
            if (member.turnInHere && _quests.places[member.placement.ender].camp == camp)
            {
                member.turnInHere = false;
                member.later = true;
            }
        Reason("leave camp " + std::to_string(camp) + " for later: its turn-ins earn too little for the walk");
    }

    // Order the stops; leave out the stops whose quests, and the ender camps whose turn-ins, earn too little for their
    // walk (rule 5), then lay it out. With no quest left to walk for, nothing is left out
    void BuildCircuit()
    {
        AddLogMembers();
        Layout layout;
        for (;;)
        {
            layout = Lay(EndPoint());
            // The trip's rate counts the quests walked for: a turn-in without objectives does not raise the bar
            float total = 0.0f;
            float seconds = layout.yards / _speed;
            std::size_t withTasks = 0;
            for (Member const& member : _members)
                if (!member.placement.tasks.empty())
                {
                    total += member.value;
                    seconds += member.placement.actionSeconds;
                    ++withTasks;
                }
            float const rate = seconds > 0.0f ? total / (seconds / 60.0f) : 0.0f;
            std::vector<bool> worst;
            std::vector<bool> drop;
            float worstRate = std::numeric_limits<float>::infinity();
            for (std::uint32_t const c : layout.order)
            {
                float const own = LeaveOutRate(layout, c, drop);
                if (static_cast<std::size_t>(std::count(drop.begin(), drop.end(), true)) >= withTasks)
                    continue;  // the circuit keeps at least one quest with objectives
                if (own < worstRate)
                {
                    worstRate = own;
                    worst = drop;
                }
            }
            // An ender camp besides the main one: leaving it out carries its quests, their objectives stay on the loop
            std::uint32_t worstCamp = NO_CAMP;
            std::vector<std::uint32_t> const camps = OtherEnderCamps();
            for (std::uint32_t const camp : camps)
            {
                float const own = CampLeaveOutRate(layout.end, camps, camp);
                if (own < worstRate)
                {
                    worstRate = own;
                    worstCamp = camp;
                }
            }
            if ((worst.empty() && worstCamp == NO_CAMP) ||
                worstRate >= rate * RETURN_RATIO / Positive(_weights.thoroughness))
                break;
            if (worstCamp != NO_CAMP)
            {
                LeaveCampOut(worstCamp);
                continue;
            }
            for (std::size_t m = _members.size(); m-- > 0;)
                if (worst[m])
                    LeaveOut(m);
        }
        // The stops in walking order; a quest completes at its last stop
        std::vector<std::uint32_t> lastStop(_members.size(), NONE);
        for (std::size_t s = 0; s < layout.order.size(); ++s)
        {
            std::uint32_t const c = layout.order[s];
            CircuitStop stop{layout.centres[c].x, layout.centres[c].y, layout.centres[c].z, {}, {}};
            for (std::uint32_t const i : layout.walk[s])
            {
                stop.tasks.push_back(layout.tasks[i]);
                lastStop[layout.owner[i]] = static_cast<std::uint32_t>(s);
            }
            _plan.stops.push_back(std::move(stop));
        }
        for (std::size_t m = 0; m < _members.size(); ++m)
            if (lastStop[m] != NONE)
                _plan.stops[lastStop[m]].completes.push_back(_members[m].record->id);
        TurnIns(layout);
        float seconds = _plan.yards / _speed;
        for (Member const& member : _members)
        {
            _plan.value += member.value;
            seconds += member.placement.actionSeconds;
        }
        _plan.minutes = seconds / 60.0f;
        for (std::size_t s = 0; s < _plan.stops.size(); ++s)
        {
            std::string line = "stop " + std::to_string(s + 1) + ":";
            for (CircuitTask const& task : _plan.stops[s].tasks)
                line += " " + std::to_string(task.quest) + "/" + std::to_string(task.objective);
            if (!_plan.stops[s].completes.empty())
                line += " (completes " + std::to_string(_plan.stops[s].completes.size()) + ")";
            Reason(line);
        }
    }

    // One pass over the ender camps: the main camp ends the loop, others follow nearest first. Quests whose ender is at
    // another hub, or out of this trip's way, are carried there
    void TurnIns(Layout const& layout)
    {
        _plan.yards = layout.yards;
        for (Member const& member : _members)
            if (!member.turnInHere)
                Carry(member.record->id,
                      member.later ? "its ender is out of this trip's way, for later" : "its ender is at another hub");
        for (std::uint32_t const id : _carriedTakes)
            _plan.carried.push_back(id);
        std::sort(_plan.carried.begin(), _plan.carried.end());
        if (_mainCamp == NO_CAMP)
        {
            _plan.returnTo = _bot.position;
            return;
        }
        for (Member const& member : _members)
            if (member.turnInHere)
                _plan.turnIns.push_back(
                    {member.record->id, member.placement.ender, _quests.places[member.placement.ender].camp});
        _plan.enderCamps.push_back(_mainCamp);
        for (std::uint32_t const camp : Tour(layout.end, OtherEnderCamps(), _plan.yards))
            _plan.enderCamps.push_back(camp);
        std::stable_sort(_plan.turnIns.begin(), _plan.turnIns.end(),
                         [this](CircuitTurnIn const& a, CircuitTurnIn const& b)
                         { return CampRank(a.camp) < CampRank(b.camp); });
        QuestCamp const& last = _quests.camps[_plan.enderCamps.back()];
        _plan.returnTo = {last.mapId, last.x, last.y, last.z};
        Reason("turn in " + std::to_string(_plan.turnIns.size()) + " quests at " +
               std::to_string(_plan.enderCamps.size()) + " camp(s), " + std::to_string(static_cast<int>(_plan.yards)) +
               " yd in all");
    }

    std::size_t CampRank(std::uint32_t camp) const
    {
        return static_cast<std::size_t>(std::find(_plan.enderCamps.begin(), _plan.enderCamps.end(), camp) -
                                        _plan.enderCamps.begin());
    }

    // The rewarded quests once this circuit's turn-ins are done
    std::vector<std::uint32_t> RewardedAfter() const
    {
        std::vector<std::uint32_t> after = _rewarded;
        for (CircuitTurnIn const& turnIn : _plan.turnIns)
            after.push_back(turnIn.quest);
        std::sort(after.begin(), after.end());
        after.erase(std::unique(after.begin(), after.end()), after.end());
        return after;
    }

    // The next quests in the chains, offered where the circuit turns in (rule 7)
    void FollowUps()
    {
        if (_plan.turnIns.empty())
            return;
        std::vector<std::uint32_t> const saved = _rewarded;
        std::vector<std::uint32_t> const savedLog = _logIds;
        _rewarded = RewardedAfter();
        for (CircuitTurnIn const& turnIn : _plan.turnIns)
            _logIds.erase(std::remove(_logIds.begin(), _logIds.end(), turnIn.quest), _logIds.end());
        for (CircuitTurnIn const& turnIn : _plan.turnIns)
            for (std::uint32_t const next : _quests.Unlocks(turnIn.quest))
            {
                QuestRecord const* record = _quests.Find(next);
                if (!record || Takeable(*record).has_value() ||
                    std::find(_plan.followUps.begin(), _plan.followUps.end(), next) != _plan.followUps.end())
                    continue;
                for (std::uint32_t const giver : record->givers)
                    if (std::find(_plan.enderCamps.begin(), _plan.enderCamps.end(), _quests.places[giver].camp) !=
                        _plan.enderCamps.end())
                    {
                        _plan.followUps.push_back(next);
                        Reason("follow-up " + std::to_string(next) + " after " + std::to_string(turnIn.quest));
                        break;
                    }
            }
        std::sort(_plan.followUps.begin(), _plan.followUps.end());
        _rewarded = saved;
        _logIds = savedLog;
    }

    float Seconds(QuestCamp const& camp) const
    {
        if (_context.travel)
        {
            float const seconds = _context.travel(_bot.position, CampPlace(camp));
            if (seconds >= 0.0f)
                return seconds;
            return std::numeric_limits<float>::infinity();
        }
        if (camp.mapId != _bot.position.map)
            return std::numeric_limits<float>::infinity();
        return Yards(_start, {camp.x, camp.y, camp.z}) / _speed;
    }

    bool Crowded(QuestCamp const& camp) const
    {
        if (!_context.crowding)
            return false;
        auto const it = _context.crowding->byArea.find(AreaKey(camp.mapId, camp.areaId));
        return it != _context.crowding->byArea.end() && it->second.bots > CROWD_BOTS_LIMIT;
    }

    bool AtHub(QuestCamp const& camp) const
    {
        return camp.mapId == _bot.position.map && Yards(_start, {camp.x, camp.y, camp.z}) <= HUB_GIVER_YARDS;
    }

    static Place CampPlace(QuestCamp const& camp) { return {camp.mapId, camp.x, camp.y, camp.z}; }

    // Why moving on found nowhere: the chain and camp targets in reach that failed the arrival test
    struct MoveTally
    {
        std::uint32_t refused{0};  // chain targets whose quest the bot would not take there
        std::uint32_t refusedQuest{0};
        SkipReason refusedWhy{SkipReason::Count};
        std::uint32_t campsRefused{0};  // bracket camps whose quests it may take but would not take there
        std::uint32_t noRoom{0};        // targets with a quest to take where a full log frees no slot
    };

    // Rule 8: when nothing is left to do here, the best chain or delivery elsewhere, then a camp in the bracket. The
    // bot moves on only to where it can act on arrival (review 5): a delivery always (its turn-in frees a slot), a
    // chain or camp only through a quest it would take there, and with the log still full after this trip only where a
    // slot frees (ActsAt). With nothing of that in reach the plan finds nowhere, says which test failed, and leaves the
    // bot to its other goals
    void MoveOn()
    {
        if (!_plan.followUps.empty())
            return;
        std::vector<std::uint32_t> const saved = _rewarded;
        _rewarded = RewardedAfter();
        bool dry = true;
        for (auto const& [id, giver] : _offered)
        {
            QuestRecord const* record = _quests.Find(id);
            if (!record || WouldTake(*record, _bot.position).has_value())
                continue;
            if (std::any_of(_plan.take.begin(), _plan.take.end(), [id](CircuitTake const& t) { return t.quest == id; }))
                continue;
            if (std::any_of(_plan.skipped.begin(), _plan.skipped.end(),
                            [this, id](CircuitSkip const& skip) { return skip.quest == id && Lasting(skip.reason); }))
                continue;
            dry = false;
            break;
        }
        if (std::any_of(_plan.take.begin(), _plan.take.end(), [](CircuitTake const& t) { return !t.carried; }) ||
            std::any_of(_members.begin(), _members.end(), [](Member const& m) { return !m.placement.tasks.empty(); }))
            dry = false;  // quests to do here, taken now or in the log
        if (!dry)
        {
            _rewarded = saved;
            return;
        }
        bool const full = LogFullAfter();
        if (full)
            Reason("log full after this trip: a chain or camp counts only where a slot frees");
        MoveTally tally;
        CircuitMove best;
        best.seconds = std::numeric_limits<float>::infinity();
        auto const consider = [&](QuestRecord const& quest, std::uint32_t place, MoveReason reason)
        {
            QuestPlace const& at = _quests.places[place];
            if (at.camp == NO_CAMP || AtHub(_quests.camps[at.camp]))
                return;
            QuestCamp const& camp = _quests.camps[at.camp];
            float const seconds = Seconds(camp);
            if (!(seconds < best.seconds || (seconds == best.seconds && quest.id < best.quest)))
                return;
            if (reason == MoveReason::Chain && !ActsAt(quest, CampPlace(camp), full, tally))
                return;
            best.reason = reason;
            best.camp = at.camp;
            best.quest = quest.id;
            best.seconds = seconds;
        };
        for (std::uint32_t const done : _rewarded)
            for (std::uint32_t const next : _quests.Unlocks(done))
                if (QuestRecord const* record = _quests.Find(next); record && !Takeable(*record).has_value())
                    for (std::uint32_t const giver : record->givers)
                        consider(*record, giver, MoveReason::Chain);
        for (std::uint32_t const id : _plan.carried)
            if (QuestRecord const* record = _quests.Find(id))
                for (std::uint32_t const ender : record->enders)
                    consider(*record, ender, MoveReason::Delivery);
        if (best.reason == MoveReason::None)
            BracketMove(best, full, tally);
        if (best.reason == MoveReason::None)
        {
            best.reason = MoveReason::Nowhere;
            best.seconds = 0.0f;
            best.word = tally.refused || tally.campsRefused ? "target_not_actionable"
                        : tally.noRoom || full              ? "log_full_no_delivery"
                                                            : "no_target";
        }
        if (best.camp != NO_CAMP)
        {
            QuestCamp const& camp = _quests.camps[best.camp];
            best.zone = camp.zoneId;
            if (_context.indexes)
                if (std::optional<Nearby> const hub = CampHub(camp, *_context.indexes))
                    best.hub = hub->index;
        }
        _plan.moveOn = best;
        Reason(std::string("move on: ") + MoveName(best.reason) +
               (best.camp == NO_CAMP
                    ? (best.reason == MoveReason::Nowhere ? ": " + NowhereWhy(full, tally) : std::string())
                    : " to camp " + std::to_string(best.camp) + " (zone " + std::to_string(best.zone) + ", quest " +
                          std::to_string(best.quest) + "), " + std::to_string(static_cast<int>(best.seconds)) + " s"));
        _rewarded = saved;
    }

    // The log has no slot for another quest after this trip: the takes fill its free slots and no turn-in frees one
    bool LogFullAfter() const { return _plan.take.size() >= FreeSlots() + _plan.turnIns.size(); }

    // Skips that a circuit here cannot change: the hub stays dry for these. A quest left for log space waits only for
    // a slot this trip frees; with the log still full after it, a slot frees only elsewhere (ActsAt)
    bool Lasting(SkipReason reason) const { return reason != SkipReason::LogFull || LogFullAfter(); }

    // A chain or camp is worth the walk only when the bot can act on arrival: it would take the quest there, and with
    // the log still full after this trip a slot frees there
    bool ActsAt(QuestRecord const& quest, Place const& at, bool full, MoveTally& tally) const
    {
        if (std::optional<SkipReason> const why = WouldTake(quest, at))
        {
            if (!tally.refused || quest.id < tally.refusedQuest)
            {
                tally.refusedQuest = quest.id;
                tally.refusedWhy = *why;
            }
            ++tally.refused;
            return false;
        }
        if (full && !SlotFreesAt(at))
        {
            ++tally.noRoom;
            return false;
        }
        return true;
    }

    // A full log frees a slot at a hub around `at` when it holds a quest MakeRoom gives up there (incomplete,
    // unprotected, elite/group while solo or not workable from there) or one the bot works on there (its turn-in frees
    // the slot). Complete quests free theirs only by delivering, protected ones only by being done
    bool SlotFreesAt(Place const& at) const
    {
        for (LogQuest const& quest : _log)
        {
            std::uint32_t const id = quest.slot.quest;
            if ((quest.slot.state & SLOT_COMPLETE) || Abandoned(id))
                continue;
            bool const group = NeedsGroup(*quest.record);
            Placement const placement = Locate(*quest.record, &quest.slot, at);
            bool const works = !group && WorkHere(placement) && !placement.tasks.empty();
            if (works || (!Contains(_protected, id) && (group || !WorkHere(placement))))
                return true;
        }
        return false;
    }

    std::string NowhereWhy(bool full, MoveTally const& tally) const
    {
        std::string why =
            full ? "the log stays full after this trip and no delivery is in reach" : "no delivery is in reach";
        if (tally.refused)
            why += "; chain quest " + std::to_string(tally.refusedQuest) +
                   (tally.refused > 1 ? " and " + std::to_string(tally.refused - 1) + " more" : std::string()) +
                   " would not be taken there (" + SkipName(tally.refusedWhy) + ")";
        if (tally.campsRefused)
            why += "; " + std::to_string(tally.campsRefused) +
                   " camp(s) in its bracket offer quests it would not take there";
        if (tally.noRoom)
            why += "; " + std::to_string(tally.noRoom) +
                   " chain or camp target(s) offer a quest, but no slot frees there (nothing to give up or work on)";
        if (!tally.refused && !tally.campsRefused && !tally.noRoom)
            why += "; no chain and no camp in its bracket";
        return why;
    }

    // The nearest camp in the bracket where the bot would take MOVE_MIN_QUESTS quests on arrival (ActsAt's rule)
    void BracketMove(CircuitMove& best, bool full, MoveTally& tally) const
    {
        if (!_context.indexes)
            return;
        std::size_t checked = 0;
        for (Nearby const& nearby :
             _quests.NearestCamps(_bot.position.map, _start.x, _start.y, _start.z, _quests.camps.size()))
        {
            if (++checked > MOVE_CAMPS_CHECKED)
                break;
            QuestCamp const& camp = _quests.camps[nearby.index];
            if (AtHub(camp) || Crowded(camp))
                continue;
            ZoneRecord const* zone = _context.indexes->FindZone(camp.zoneId);
            if (!zone || !zone->high || _bot.level < zone->low || _bot.level > zone->high)
                continue;
            Place const at = CampPlace(camp);
            std::uint32_t offered = 0;
            std::uint32_t taken = 0;
            std::uint32_t first = 0;
            for (std::uint32_t const id : _quests.QuestsOfGiverCluster(nearby.index))
                if (QuestRecord const* record = _quests.Find(id); record && !Takeable(*record).has_value())
                {
                    ++offered;
                    if (WouldTake(*record, at).has_value())
                        continue;
                    first = first ? first : id;
                    ++taken;
                }
            if (taken < MOVE_MIN_QUESTS)
            {
                tally.campsRefused += offered >= MOVE_MIN_QUESTS ? 1 : 0;
                continue;
            }
            if (full && !SlotFreesAt(at))
            {
                ++tally.noRoom;
                continue;
            }
            best.reason = MoveReason::Bracket;
            best.camp = nearby.index;
            best.quest = first;
            best.seconds = Seconds(camp);
            return;
        }
    }

    Snapshot const& _bot;
    CircuitContext const& _context;
    QuestIndex const& _quests;
    CircuitWeights const _weights;
    CircuitPlan _plan;
    Point _start;
    float _speed{DEFAULT_RUN_SPEED};
    std::uint8_t _band{0};
    std::vector<std::uint32_t> _rewarded;
    std::vector<std::uint32_t> _protected;
    std::vector<std::uint32_t> _logIds;
    std::vector<LogQuest> _log;
    std::vector<std::int32_t> _takenGroups;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> _offered;
    std::vector<std::uint32_t> _abandoned;
    std::vector<Member> _members;
    std::vector<std::uint32_t> _carriedTakes;
    std::uint32_t _mainCamp{NO_CAMP};
};
}  // namespace Circuit

// The circuit at the bot's hub. Pure and deterministic: the same input gives the same plan
inline CircuitPlan PlanCircuit(Snapshot const& bot, CircuitContext const& context)
{
    if (!context.quests)
    {
        CircuitPlan plan;
        plan.failure = "no quest index";
        return plan;
    }
    return Circuit::CircuitBuilder(bot, context).Run();
}

// Today's pick over the plan's quests (PLAN 3.9 baseline, NewRpgBaseAction/NewRpgAction): from the start a random log
// quest, its remaining objectives in random order at the same places, then its nearest ender before the next quest.
// The mean yards over `seeds` fixed seeds
namespace Circuit
{
struct BaselineQuest
{
    std::vector<Point> tasks;
    Point ender;
};

// The plan's completed quests with their task points and their ender, as today's pick walks them
inline std::vector<BaselineQuest> BaselineQuests(CircuitPlan const& plan, QuestIndex const& quests)
{
    std::vector<BaselineQuest> list;
    for (CircuitTurnIn const& turnIn : plan.turnIns)
    {
        BaselineQuest quest;
        for (CircuitStop const& stop : plan.stops)
            for (CircuitTask const& task : stop.tasks)
                if (task.quest == turnIn.quest)
                    quest.tasks.push_back({task.x, task.y, task.z});
        quest.ender = PointOf(quests.places[turnIn.ender]);
        list.push_back(std::move(quest));
    }
    return list;
}
}  // namespace Circuit

inline float BaselineYards(CircuitPlan const& plan, QuestIndex const& quests,
                           std::uint32_t seeds = Circuit::BASELINE_SEEDS)
{
    using Quest = Circuit::BaselineQuest;
    std::vector<Quest> const list = Circuit::BaselineQuests(plan, quests);
    if (list.empty() || !seeds)
        return 0.0f;
    Circuit::Point const start{plan.start.x, plan.start.y, plan.start.z};
    double total = 0.0;
    for (std::uint32_t seed = 1; seed <= seeds; ++seed)
    {
        Circuit::SplitMix random{seed};
        std::vector<Quest> order = list;
        random.Shuffle(order);
        Circuit::Point at = start;
        double yards = 0.0;
        for (Quest& quest : order)
        {
            random.Shuffle(quest.tasks);
            for (Circuit::Point const& task : quest.tasks)
            {
                yards += Circuit::Yards(at, task);
                at = task;
            }
            yards += Circuit::Yards(at, quest.ender);
            at = quest.ender;
        }
        total += yards;
    }
    return static_cast<float>(total / seeds);
}

}  // namespace Planner

#endif
