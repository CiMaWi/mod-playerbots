/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERPOOLTYPES_H
#define _PLAYERBOT_PLANNERPOOLTYPES_H

// The planner pool's plain types (PlannerPool.h): its limits, a job and its result, the recheck before a result is
// used and the pool's counters. Without the pool itself, so PlannerService.h's includers do not compile it

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

#include "PlannerSnapshot.h"

namespace Planner
{
constexpr std::size_t QUEUE_MAX = 4096;       // bots waiting for a job; the oldest is dropped beyond
constexpr std::size_t TIMING_SAMPLES = 1024;  // recent jobs behind the p50/p95
constexpr std::uint32_t THREADS_MAX = 8;
constexpr std::uint32_t RESULT_KEEP_MS = 120000;  // a result nobody took in this time expires
constexpr std::uint32_t SWEEP_MS = 1000;
constexpr std::uint32_t STALE_MS = 30000;  // WT.10; the server reads WorldTravel.PlannerStaleSeconds
constexpr float RECHECK_YARDS = 100.0f;    // the server reads WorldTravel.PlannerRecheckYards

enum class JobKind : std::uint8_t
{
    Goals,  // the ranked goals with their circuit (WT14, GoalModel.h); PlannerService applies them (WT16)
};

namespace Goals
{
struct GoalPlan;  // GoalModel.h
}
}  // namespace Planner

namespace SkillPath
{
struct Path;  // SkillPathTypes.h
}

namespace Materials
{
struct Rates;  // MaterialRules.h
}

namespace Planner
{
struct PathRecord;  // SkillPathLive.h

struct GoalsAnswer
{
    std::shared_ptr<Goals::GoalPlan const> plan;
    std::shared_ptr<SkillPath::Path const> path;  // the bot's skill path, computed with the plan (WR11); null: none
    // The rates the gather rules weighed the plan's circuit at (professions WR13c: the pass-by gate reads them)
    std::shared_ptr<Materials::Rates const> gatherRates;
    std::shared_ptr<PathRecord const> pathRecord;  // what the path was computed from (WPC14b); null with no path
};

using Payload = std::variant<std::monostate, GoalsAnswer>;
// A pure function of the snapshot. None or an exception = failed: the bot gets no result and keeps today's pick
using Compute = std::function<std::optional<Payload>(Snapshot const&)>;

struct Job
{
    JobKind kind{JobKind::Goals};
    Snapshot snapshot;
    Compute compute;
};

struct Result
{
    std::uint64_t bot{0};
    std::uint32_t stateVersion{0};  // the snapshot's, with its time and place, for the recheck
    std::uint32_t takenMs{0};
    Place position;
    JobKind kind{JobKind::Goals};
    Payload payload;
    std::uint32_t computeNs{0};
    std::uint32_t waitNs{0};
};

enum class Verdict : std::uint8_t
{
    Use,
    Dead,
    Version,
    Age,
    Position,
};
constexpr std::size_t VERDICT_COUNT = 5;

struct CurrentState
{
    bool alive{true};
    std::uint32_t stateVersion{0};
    Place position;
    std::uint32_t nowMs{0};
};

struct Limits
{
    std::uint32_t maxAgeMs{STALE_MS};
    float nearYards{RECHECK_YARDS};
};

// The bot's map thread, before a result is used. At exactly a limit the result is still used
inline Verdict Recheck(Result const& result, CurrentState const& now, Limits const& limits)
{
    if (!now.alive)
        return Verdict::Dead;
    if (now.stateVersion != result.stateVersion)
        return Verdict::Version;
    if (now.nowMs - result.takenMs > limits.maxAgeMs)  // unsigned: the millisecond clock's wrap is harmless
        return Verdict::Age;
    if (now.position.map != result.position.map)
        return Verdict::Position;
    float const dx = now.position.x - result.position.x;
    float const dy = now.position.y - result.position.y;
    float const dz = now.position.z - result.position.z;
    if (dx * dx + dy * dy + dz * dz > limits.nearYards * limits.nearYards)
        return Verdict::Position;
    return Verdict::Use;
}

struct PoolStats
{
    std::uint32_t threads{0};
    std::uint64_t submitted{0};   // accepted by Submit
    std::uint64_t replaced{0};    // a waiting job superseded by the same bot's newer one
    std::uint64_t dropped{0};     // the oldest waiting job when the queue was full, and those waiting at Stop
    std::uint64_t run{0};         // jobs a worker ran
    std::uint64_t failures{0};    // jobs that returned none or threw
    std::uint64_t made{0};        // results of the jobs that did not fail
    std::uint64_t taken{0};       // results taken by their bot
    std::uint64_t expired{0};     // results nobody took within the keep time
    std::uint64_t superseded{0};  // results not kept or replaced: the bot's later job was waiting or done
    std::uint64_t posted{0};      // Post()ed tasks
    std::uint64_t tasksRun{0};
    std::uint64_t taskFailures{0};
    std::uint64_t tasksDropped{0};                        // waiting at Stop
    std::array<std::uint64_t, VERDICT_COUNT> verdicts{};  // the bots' rechecks of taken results, by Verdict
    std::uint32_t waiting{0};
    std::uint32_t peakWaiting{0};
    std::uint32_t results{0};  // made, not yet taken or expired
    // Nanoseconds: a no-op job takes well under a microsecond
    std::uint32_t computeP50Ns{0};
    std::uint32_t computeP95Ns{0};
    std::uint32_t waitP50Ns{0};
    std::uint32_t waitP95Ns{0};
    std::uint64_t computeNs{0};  // all jobs' compute time: mean = computeNs / run
    std::uint64_t busyNs{0};     // all workers' time in jobs and tasks: busy share = busyNs / (threads x uptimeNs)
    std::uint64_t uptimeNs{0};   // since Start
};
}  // namespace Planner

#endif
