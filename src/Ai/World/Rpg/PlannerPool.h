/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERPOOL_H
#define _PLAYERBOT_PLANNERPOOL_H

// The planner's workers beside the map threads (travel plan WT12a). A map thread fills the bot's Snapshot and
// Submit()s a Job; a worker runs the job's pure function of that snapshot (later also of immutable indexes); the
// bot's own update TryTake()s the Result and Recheck()s it before use. Post() runs one-off background work. No game
// object ever reaches a worker. Player-free, so tests/planner-pool-selftest.cpp runs PoolSelftest() on it; the
// server's instance, FillSnapshot and the per-bot hook live in PlannerService.cpp.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "PlannerSnapshot.h"
#include "TraitRules.h"

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
    Noop,     // proves the machinery (WorldTravel.PlannerNoopSeconds); its answer changes nothing
    Circuit,  // a questing circuit at the bot's hub (WT13b, QuestCircuit.h)
    Goals,    // the ranked goals with their circuit (WT14, GoalModel.h); PlannerService applies them (WT16)
};

struct NoopAnswer
{
    std::uint64_t checksum{0};
};

struct CircuitPlan;  // QuestCircuit.h

struct CircuitAnswer
{
    std::shared_ptr<CircuitPlan const> plan;
};

namespace Goals
{
struct GoalPlan;  // GoalModel.h
}

struct GoalsAnswer
{
    std::shared_ptr<Goals::GoalPlan const> plan;
};

using Payload = std::variant<std::monostate, NoopAnswer, CircuitAnswer, GoalsAnswer>;
// A pure function of the snapshot. None or an exception = failed: the bot gets no result and keeps today's pick
using Compute = std::function<std::optional<Payload>(Snapshot const&)>;

struct Job
{
    JobKind kind{JobKind::Noop};
    Snapshot snapshot;
    Compute compute;
};

struct Result
{
    std::uint64_t bot{0};
    std::uint32_t stateVersion{0};  // the snapshot's, with its time and place, for the recheck
    std::uint32_t takenMs{0};
    Place position;
    JobKind kind{JobKind::Noop};
    Payload payload;
    std::uint32_t computeNs{0};
    std::uint32_t waitNs{0};
    std::uint64_t ticket{0};  // Submit's answer for this job: a bot applies only the result of the job it waits for
};

// A few microseconds of work that depends only on the snapshot (FNV-1a over its main fields)
inline std::optional<Payload> NoopCompute(Snapshot const& snapshot)
{
    constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;
    std::uint64_t hash = FNV_OFFSET;
    auto const mix = [&hash](std::uint64_t value) { hash = (hash ^ value) * FNV_PRIME; };
    mix(snapshot.bot);
    mix(snapshot.stateVersion);
    mix(snapshot.level);
    mix(snapshot.zone);
    mix(snapshot.money);
    for (QuestSlot const& slot : snapshot.quests)
        mix(slot.quest);
    for (std::uint32_t const quest : snapshot.rewardedQuests)
        mix(quest);
    for (std::uint32_t const word : snapshot.taxiMask)
        mix(word);
    return Payload{NoopAnswer{hash}};
}

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

namespace Detail
{
inline thread_local bool workerThread = false;
}

class PlannerPool
{
public:
    using Clock = std::chrono::steady_clock;
    // Runs on a worker after each job: its compute and queue wait microseconds (the server's PerfMonitor rows)
    using Sink = std::function<void(std::uint64_t computeUs, std::uint64_t waitUs)>;

    explicit PlannerPool(std::size_t queueMax = QUEUE_MAX, std::uint32_t resultKeepMs = RESULT_KEEP_MS)
        : _queueMax(std::max<std::size_t>(queueMax, 1)), _resultKeep(std::chrono::milliseconds(resultKeepMs))
    {
    }
    ~PlannerPool() { Stop(); }
    PlannerPool(PlannerPool const&) = delete;
    PlannerPool& operator=(PlannerPool const&) = delete;

    // True on this pool's (any pool's) worker threads
    static bool IsWorkerThread() { return Detail::workerThread; }

    // Once; later calls and calls after Stop do nothing. threads is clamped to 1..THREADS_MAX
    void Start(std::uint32_t threads, Sink sink = {})
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_workers.empty() || _stopping)
            return;
        _sink = std::move(sink);
        _started = Clock::now();
        std::uint32_t const count = std::clamp<std::uint32_t>(threads, 1, THREADS_MAX);
        _busyNs.assign(count, 0);
        for (std::uint32_t i = 0; i < count; ++i)
            _workers.emplace_back([this, i] { Work(i); });
    }

    // Wakes and joins the workers; waiting jobs and tasks are dropped. A stopped pool accepts nothing. Twice is
    // harmless
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stopping = true;
        }
        _wake.notify_all();
        for (std::thread& worker : _workers)
            if (worker.joinable())
                worker.join();
        std::lock_guard<std::mutex> lock(_mutex);
        _stats.dropped += _waiting.size();
        _stats.tasksDropped += _tasks.size();
        _order.clear();
        _waiting.clear();
        _tasks.clear();
        _workers.clear();
    }

    // Any thread; never waits for a job. Answers the job's ticket (its result carries it); 0 = the pool is not
    // running. A bot has at most one waiting job: a newer one replaces it in place
    std::uint64_t Submit(std::uint64_t bot, Job job)
    {
        std::uint64_t ticket = 0;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_workers.empty() || _stopping)
                return 0;
            ++_stats.submitted;
            auto const waiting = _waiting.find(bot);
            _expiredFor.erase(bot);
            if (waiting != _waiting.end())
            {
                waiting->second = {std::move(job), Clock::now(), ++_sequence};
                ++_stats.replaced;
                return _sequence;
            }
            if (_waiting.size() >= _queueMax)
            {
                _waiting.erase(_order.front());
                _order.pop_front();
                ++_stats.dropped;
            }
            _waiting.emplace(bot, Pending{std::move(job), Clock::now(), ++_sequence});
            ticket = _sequence;
            _order.push_back(bot);
            _stats.peakWaiting = std::max(_stats.peakWaiting, static_cast<std::uint32_t>(_waiting.size()));
        }
        _wake.notify_one();
        return ticket;
    }

    // Any thread: one-off background work with no bot (WT12b's index build). False = the pool is not running
    bool Post(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_workers.empty() || _stopping)
                return false;
            ++_stats.posted;
            _tasks.push_back(std::move(task));
        }
        _wake.notify_one();
        return true;
    }

    // Any thread: the bot's result once its job is done, while younger than the keep time. One atomic load when no
    // result waits for any bot
    std::optional<Result> TryTake(std::uint64_t bot)
    {
        if (!_resultCount.load(std::memory_order_acquire))
            return std::nullopt;
        std::lock_guard<std::mutex> lock(_mutex);
        Clock::time_point const now = Clock::now();
        Sweep(now);
        auto const done = _done.find(bot);
        if (done == _done.end())
            return std::nullopt;
        std::optional<Result> result;
        if (now - done->second.at <= _resultKeep)
        {
            result = std::move(done->second.result);
            ++_stats.taken;
        }
        else
            Expire(bot);
        _done.erase(done);
        _resultCount.store(static_cast<std::uint32_t>(_done.size()), std::memory_order_release);
        return result;
    }

    // Any thread, for a bot that gave up waiting: true (once) when its result expired untaken, already counted as
    // expired; false when its job left no result (dropped or failed)
    bool ForgetExpired(std::uint64_t bot)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        Sweep(Clock::now());
        return _expiredFor.erase(bot) > 0;
    }

    // Any thread: the bot's recheck of a taken result
    void CountVerdict(Verdict verdict)
    {
        _verdicts[static_cast<std::size_t>(verdict)].fetch_add(1, std::memory_order_relaxed);
    }

    PoolStats Stats() const
    {
        std::vector<std::uint32_t> computes;
        std::vector<std::uint32_t> waits;
        PoolStats stats;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            stats = _stats;
            stats.threads = static_cast<std::uint32_t>(_workers.size());
            stats.waiting = static_cast<std::uint32_t>(_waiting.size());
            stats.results = static_cast<std::uint32_t>(_done.size());
            for (std::uint64_t const busy : _busyNs)
                stats.busyNs += busy;
            if (!_workers.empty())
                stats.uptimeNs = Nanos64(Clock::now() - _started);
            computes = _computeNs;
            waits = _waitNs;
        }
        for (std::size_t i = 0; i < VERDICT_COUNT; ++i)
            stats.verdicts[i] = _verdicts[i].load(std::memory_order_relaxed);
        auto const percentile = [](std::vector<std::uint32_t>& samples, std::size_t percent) -> std::uint32_t
        {
            if (samples.empty())
                return 0;
            auto const at = samples.begin() + static_cast<std::ptrdiff_t>((samples.size() - 1) * percent / 100);
            std::nth_element(samples.begin(), at, samples.end());
            return *at;
        };
        stats.computeP50Ns = percentile(computes, 50);
        stats.computeP95Ns = percentile(computes, 95);
        stats.waitP50Ns = percentile(waits, 50);
        stats.waitP95Ns = percentile(waits, 95);
        return stats;
    }

private:
    struct Pending
    {
        Job job;
        Clock::time_point submitted;
        std::uint64_t sequence{0};  // Submit's order over all bots: the bot's later job has a higher one
    };

    struct Running
    {
        std::uint32_t count{0};   // the bot's jobs on workers now
        std::uint64_t newest{0};  // the highest sequence among those picked since count was last 0
    };

    struct Done
    {
        Result result;
        Clock::time_point at;
        std::uint64_t sequence{0};
    };

    static constexpr std::uint64_t NANOS_PER_MICRO = 1000;

    static std::uint64_t Nanos64(Clock::duration duration)
    {
        auto const count = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        return static_cast<std::uint64_t>(std::max<long long>(count, 0));
    }

    // Capped at about 4.3 s, plenty for one job's sample
    static std::uint32_t Nanos(Clock::duration duration)
    {
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(Nanos64(duration), 0xFFFFFFFFULL));
    }

    // Under _mutex: a result nobody took. The bot's ForgetExpired() learns it (bounded: cleared when it grows large)
    void Expire(std::uint64_t bot)
    {
        ++_stats.expired;
        if (_expiredFor.size() >= _queueMax)
            _expiredFor.clear();
        _expiredFor.insert(bot);
    }

    static void Sample(std::vector<std::uint32_t>& samples, std::size_t& next, std::uint32_t value)
    {
        if (samples.size() < TIMING_SAMPLES)
            samples.push_back(value);
        else
            samples[next] = value;
        next = (next + 1) % TIMING_SAMPLES;
    }

    // Under _mutex: results older than the keep time go
    void Sweep(Clock::time_point now)
    {
        if (now - _swept < std::chrono::milliseconds(SWEEP_MS))
            return;
        _swept = now;
        for (auto it = _done.begin(); it != _done.end();)
        {
            if (now - it->second.at > _resultKeep)
            {
                Expire(it->first);
                it = _done.erase(it);
            }
            else
                ++it;
        }
        _resultCount.store(static_cast<std::uint32_t>(_done.size()), std::memory_order_release);
    }

    // Under _mutex (the lock is released while the job runs): the bot's job, its result stored
    void RunJob(std::unique_lock<std::mutex>& lock, std::size_t worker)
    {
        std::uint64_t const bot = _order.front();
        _order.pop_front();
        auto const waiting = _waiting.find(bot);
        Pending pending = std::move(waiting->second);
        _waiting.erase(waiting);
        Running& running = _running[bot];
        ++running.count;
        running.newest = std::max(running.newest, pending.sequence);
        lock.unlock();

        Clock::time_point const picked = Clock::now();
        std::optional<Payload> payload;
        try
        {
            if (pending.job.compute)
                payload = pending.job.compute(pending.job.snapshot);
        }
        catch (...)
        {
            payload.reset();
        }
        Clock::time_point const now = Clock::now();
        std::uint32_t const computeNs = Nanos(now - picked);
        std::uint32_t const waitNs = Nanos(picked - pending.submitted);

        lock.lock();
        auto const runningIt = _running.find(bot);
        bool const newerPicked = runningIt->second.newest > pending.sequence;
        if (!--runningIt->second.count)
            _running.erase(runningIt);
        ++_stats.run;
        _stats.computeNs += computeNs;
        _busyNs[worker] += computeNs;
        Sample(_computeNs, _nextCompute, computeNs);
        Sample(_waitNs, _nextWait, waitNs);
        if (!payload)
            ++_stats.failures;
        else
        {
            ++_stats.made;
            Snapshot const& snapshot = pending.job.snapshot;
            Result result{bot,
                          snapshot.stateVersion,
                          snapshot.takenMs,
                          snapshot.position,
                          pending.job.kind,
                          std::move(*payload),
                          computeNs,
                          waitNs,
                          pending.sequence};
            // With several workers the bot's later job can finish first (and be taken already), run beside this
            // one, or still wait: only the latest result is kept, else the bot would take the older one
            auto const done = _done.find(bot);
            if (_waiting.contains(bot) || newerPicked ||
                (done != _done.end() && done->second.sequence > pending.sequence))
                ++_stats.superseded;
            else
            {
                if (done != _done.end())
                    ++_stats.superseded;  // the older result, never taken
                _done[bot] = {std::move(result), now, pending.sequence};
            }
        }
        Sweep(now);
        _resultCount.store(static_cast<std::uint32_t>(_done.size()), std::memory_order_release);
        lock.unlock();

        if (_sink)
            _sink(computeNs / NANOS_PER_MICRO, waitNs / NANOS_PER_MICRO);
        lock.lock();
    }

    void RunTask(std::unique_lock<std::mutex>& lock, std::size_t worker)
    {
        std::function<void()> task = std::move(_tasks.front());
        _tasks.pop_front();
        lock.unlock();

        Clock::time_point const started = Clock::now();
        bool ok = true;
        try
        {
            task();
        }
        catch (...)
        {
            ok = false;
        }
        std::uint64_t const busyNs = Nanos64(Clock::now() - started);

        lock.lock();
        _busyNs[worker] += busyNs;
        if (ok)
            ++_stats.tasksRun;
        else
            ++_stats.taskFailures;
    }

    void Work(std::size_t worker)
    {
        Detail::workerThread = true;
        std::unique_lock<std::mutex> lock(_mutex);
        while (true)
        {
            _wake.wait(lock, [this] { return _stopping || !_tasks.empty() || !_order.empty(); });
            if (_stopping)
                return;
            if (!_tasks.empty())
                RunTask(lock, worker);
            else
                RunJob(lock, worker);
        }
    }

    std::size_t const _queueMax;
    Clock::duration const _resultKeep;
    Sink _sink;  // set before the workers start, read-only afterwards

    mutable std::mutex _mutex;
    std::condition_variable _wake;
    bool _stopping{false};
    std::vector<std::thread> _workers;
    Clock::time_point _started{};
    std::deque<std::uint64_t> _order;  // bots with a waiting job, oldest first
    std::unordered_map<std::uint64_t, Pending> _waiting;
    std::deque<std::function<void()>> _tasks;
    std::uint64_t _sequence{0};
    std::unordered_map<std::uint64_t, Done> _done;
    std::atomic<std::uint32_t> _resultCount{0};
    std::array<std::atomic<std::uint64_t>, VERDICT_COUNT> _verdicts{};
    PoolStats _stats;
    std::vector<std::uint64_t> _busyNs;  // per worker
    std::vector<std::uint32_t> _computeNs;
    std::vector<std::uint32_t> _waitNs;
    std::unordered_set<std::uint64_t> _expiredFor;        // bots whose result expired untaken, until ForgetExpired
    std::unordered_map<std::uint64_t, Running> _running;  // bots with a job on a worker: at most one entry per worker
    std::size_t _nextCompute{0};
    std::size_t _nextWait{0};
    Clock::time_point _swept{};
};
}  // namespace Planner

#endif
