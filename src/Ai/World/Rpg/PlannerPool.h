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
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

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
constexpr std::size_t QUEST_LOG_SLOTS = 25;
constexpr std::size_t QUEST_COUNTERS = 4;
constexpr std::size_t TAXI_WORDS = 14;  // PlayerTaxi's mask
constexpr std::size_t RECENT_GOALS = 4;

struct Place
{
    std::uint32_t map{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct QuestSlot
{
    std::uint32_t quest{0};
    std::uint32_t state{0};  // the quest log slot's state flags
    std::array<std::uint16_t, QUEST_COUNTERS> counters{};
};

struct Skill
{
    std::uint16_t id{0};
    std::uint16_t value{0};
    std::uint16_t max{0};
};

// A carried item that counts for an upkeep stock kind (upkeep plan WU2, UpkeepIndex.h)
struct StockItem
{
    std::uint32_t item{0};
    std::uint32_t count{0};
    std::uint8_t kinds{0};  // a bit per UpkeepStock
    std::uint8_t diet{0};   // the pet food bit (FoodType - 1); 0 = no pet eats it
};

struct Goal
{
    std::uint32_t kind{0};
    std::uint32_t target{0};
    std::uint32_t startedMs{0};
};

// The bot as the planner sees it (WT.11), filled on its map thread; plain data, a few KB
struct Snapshot
{
    std::uint64_t bot{0};  // the guid's raw value
    std::uint32_t stateVersion{0};
    std::uint32_t takenMs{0};
    std::uint8_t level{0};
    std::uint8_t race{0};
    std::uint8_t cls{0};
    std::uint8_t team{0};  // 0 Alliance, 1 Horde
    std::uint32_t xp{0};
    Place position;
    std::uint32_t zone{0};
    std::uint32_t area{0};
    std::uint32_t money{0};
    std::uint16_t bagSlots{0};  // general slots: the backpack and plain bags
    std::uint16_t bagFree{0};
    std::uint32_t lowestItemWorth{0};  // not filled yet: worth per item is not cheap on the map thread
    std::uint8_t durabilityPercent{100};
    std::uint8_t lowestDurabilityPercent{100};  // the most worn equipped item
    std::uint8_t brokenItems{0};                // equipped items at durability 0
    std::vector<Skill> skills;                  // riding and the professions the bot has
    std::vector<QuestSlot> quests;
    std::vector<std::uint32_t> rewardedQuests;  // sorted
    bool hasHearthstone{false};
    Place hearth;
    std::uint32_t hearthCooldownMs{0};
    std::array<std::uint32_t, TAXI_WORDS> taxiMask{};
    std::optional<TraitRules::BotTraits> traits;
    float runSpeed{0.0f};
    bool mounted{false};
    std::uint8_t groupSize{0};  // 0 = no group
    std::uint64_t groupLeader{0};
    std::uint32_t deaths{0};  // not filled yet: no cheap per-bot death count exists
    Goal currentGoal;         // WT16 fills the goals
    std::vector<Goal> recentGoals;
    // What the upkeep needs rules read (upkeep plan WU2, FillUpkeep in PlannerUpkeep.cpp)
    std::vector<StockItem> stock;                  // the backpack and every bag, one entry per item id
    std::uint8_t petDiet{0};                       // the hunter pet's food mask (bit FoodType - 1); 0 = no hunter pet
    std::uint32_t upkeepVersion{0};                // the upkeep index the bit sets align to; 0 = none yet
    std::vector<std::uint64_t> knownClassSpells;   // bit i: the class trainer list's spell i is known
    std::vector<std::uint64_t> knownRidingSpells;  // the same over the riding trainer list
    float walkMinutesPerHour{0.0f};                // not filled yet: the travel shares are measured later (WU14)
    float flightMinutesPerHour{0.0f};              // not filled yet

    std::uint32_t StockCount(std::uint8_t kindBit) const
    {
        std::uint32_t total = 0;
        for (StockItem const& entry : stock)
            if (entry.kinds & kindBit)
                total += entry.count;
        return total;
    }

    std::size_t ApproxBytes() const
    {
        return sizeof(Snapshot) + skills.capacity() * sizeof(Skill) + quests.capacity() * sizeof(QuestSlot) +
               rewardedQuests.capacity() * sizeof(std::uint32_t) + recentGoals.capacity() * sizeof(Goal) +
               stock.capacity() * sizeof(StockItem) +
               (knownClassSpells.capacity() + knownRidingSpells.capacity()) * sizeof(std::uint64_t);
    }
};

enum class JobKind : std::uint8_t
{
    Noop,  // proves the machinery (WorldTravel.PlannerNoopSeconds); its answer changes nothing
};

struct NoopAnswer
{
    std::uint64_t checksum{0};
};

using Payload = std::variant<std::monostate, NoopAnswer>;
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

    // Any thread; never waits for a job. False = the pool is not running. A bot has at most one waiting job: a newer
    // one replaces it in place
    bool Submit(std::uint64_t bot, Job job)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_workers.empty() || _stopping)
                return false;
            ++_stats.submitted;
            auto const waiting = _waiting.find(bot);
            _expiredFor.erase(bot);
            if (waiting != _waiting.end())
            {
                waiting->second = {std::move(job), Clock::now(), ++_sequence};
                ++_stats.replaced;
                return true;
            }
            if (_waiting.size() >= _queueMax)
            {
                _waiting.erase(_order.front());
                _order.pop_front();
                ++_stats.dropped;
            }
            _waiting.emplace(bot, Pending{std::move(job), Clock::now(), ++_sequence});
            _order.push_back(bot);
            _stats.peakWaiting = std::max(_stats.peakWaiting, static_cast<std::uint32_t>(_waiting.size()));
        }
        _wake.notify_one();
        return true;
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
                          waitNs};
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

// The pool's checks (tests/planner-pool-selftest.cpp, `playerbots planner selftest`): queue semantics with a gated
// worker, expiry, failures, Stop, every Recheck verdict and its limits, the counter sums, then 4 submitter threads x
// botsPerThread bots on 2 workers (every result equals the single-thread computation, none lost, none for the wrong
// bot, an older job never overwrites a newer result), Post tasks run once, IsWorkerThread, the snapshot sizes.
// Returns the number of checks
inline std::uint32_t PoolSelftest(std::vector<std::string>& failures, std::vector<std::string>& notes,
                                  std::uint32_t botsPerThread)
{
    using namespace std::chrono_literals;
    static constexpr auto DEADLINE = std::chrono::seconds(30);
    constexpr std::uint32_t SUBMITTERS = 4;
    constexpr std::uint32_t WORKERS = 2;
    constexpr std::uint32_t RACE_ROUNDS = 49;
    constexpr std::uint32_t TASKS = 100;
    constexpr std::uint32_t SHORT_KEEP_MS = 50;
    constexpr std::size_t SMALL_QUEUE = 4;
    constexpr std::size_t WORST_BYTES = 16 * 1024;
    constexpr std::size_t TYPICAL_BYTES = 8 * 1024;

    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back(what);
        return ok;
    };
    auto const sums = [&check](PoolStats const& s, std::string const& where)
    {
        check(s.submitted == s.run + s.replaced + s.dropped + s.waiting,
              where + ": submitted = run + replaced + dropped + waiting");
        check(s.run == s.made + s.failures, where + ": run = made + failures");
        check(s.made == s.taken + s.expired + s.superseded + s.results,
              where + ": made = taken + expired + superseded + results");
    };
    auto const makeSnapshot = [](std::uint64_t bot, std::uint32_t version)
    {
        Snapshot snapshot;
        snapshot.bot = bot;
        snapshot.stateVersion = version;
        snapshot.takenMs = static_cast<std::uint32_t>(bot * 7);
        snapshot.level = static_cast<std::uint8_t>(1 + bot % 80);
        snapshot.zone = static_cast<std::uint32_t>(bot % 4000);
        snapshot.position = {static_cast<std::uint32_t>(bot % 2), static_cast<float>(bot % 1000), 1.0f, 2.0f};
        for (std::uint32_t quest = 0; quest < bot % 50; ++quest)
            snapshot.rewardedQuests.push_back(quest * 3 + static_cast<std::uint32_t>(bot % 7));
        return snapshot;
    };
    auto const job = [&makeSnapshot](std::uint64_t bot, std::uint32_t version, Compute compute = NoopCompute)
    { return Job{JobKind::Noop, makeSnapshot(bot, version), std::move(compute)}; };
    auto const checksum = [](Result const& result) -> std::uint64_t
    {
        NoopAnswer const* answer = std::get_if<NoopAnswer>(&result.payload);
        return answer ? answer->checksum : 0;
    };
    auto const expected = [&makeSnapshot](std::uint64_t bot, std::uint32_t version) -> std::uint64_t
    { return std::get<NoopAnswer>(*NoopCompute(makeSnapshot(bot, version))).checksum; };
    auto const waitFor = [](auto const& condition)
    {
        auto const until = std::chrono::steady_clock::now() + DEADLINE;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() > until)
                return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    };

    // a) Never started, stopped: nothing is accepted; Stop twice; Start after Stop does nothing
    {
        PlannerPool pool;
        check(!pool.Submit(1, job(1, 1)), "a never-started pool refuses Submit");
        check(!pool.Post([] {}), "a never-started pool refuses Post");
        check(!pool.TryTake(1), "a never-started pool has no result");
        pool.Stop();
        pool.Stop();
        pool.Start(WORKERS);
        check(pool.Stats().threads == 0, "Start after Stop starts no worker");
        check(!pool.Submit(1, job(1, 1)), "a stopped pool refuses Submit");
        check(!PlannerPool::IsWorkerThread(), "IsWorkerThread is false on the test thread");
    }

    // b) One gated worker: replace in place, the cap drops the oldest waiting job, the fast path, the sums
    {
        PlannerPool pool(SMALL_QUEUE);
        pool.Start(1);
        std::atomic<bool> entered{false};
        std::atomic<bool> open{false};
        Compute const gated = [&entered, &open](Snapshot const& snapshot) -> std::optional<Payload>
        {
            entered = true;
            while (!open)
                std::this_thread::yield();
            return NoopCompute(snapshot);
        };
        check(pool.Submit(1, job(1, 1, gated)), "Submit to a running pool");
        check(waitFor([&entered] { return entered.load(); }), "the gated job starts");
        check(!pool.TryTake(99), "no result: the fast path");
        pool.Submit(2, job(2, 1));
        pool.Submit(2, job(2, 2));
        for (std::uint64_t bot = 3; bot <= 6; ++bot)
            pool.Submit(bot, job(bot, 1));
        PoolStats const queued = pool.Stats();
        check(queued.replaced == 1, "a newer job replaces the bot's waiting one");
        check(queued.dropped == 1 && queued.waiting == SMALL_QUEUE, "the cap drops the oldest waiting job");
        check(queued.peakWaiting == SMALL_QUEUE, "the peak queue depth");
        open = true;
        std::vector<std::uint64_t> const kept = {1, 3, 4, 5, 6};
        std::size_t got = 0;
        check(waitFor(
                  [&]
                  {
                      for (std::uint64_t const bot : kept)
                          if (std::optional<Result> const result = pool.TryTake(bot))
                          {
                              ++got;
                              check(result->bot == bot && checksum(*result) == expected(bot, 1),
                                    "a gated-queue result is the bot's own");
                          }
                      return got == kept.size();
                  }),
              "every kept job's result arrives");
        check(!pool.TryTake(2), "the dropped bot gets no result");
        sums(pool.Stats(), "gated queue");
    }

    // c) Expiry, failures and Stop dropping waiting jobs
    {
        PlannerPool pool(QUEUE_MAX, SHORT_KEEP_MS);
        pool.Start(1);
        pool.Submit(7, job(7, 1));
        pool.Submit(8, job(8, 1, [](Snapshot const&) -> std::optional<Payload> { return std::nullopt; }));
        pool.Submit(9, job(9, 1, [](Snapshot const&) -> std::optional<Payload> { throw std::exception(); }));
        check(waitFor([&pool] { return pool.Stats().run == 3; }), "three jobs run");
        std::this_thread::sleep_for(std::chrono::milliseconds(SHORT_KEEP_MS * 3));
        check(!pool.TryTake(7), "a result older than the keep time is not taken");
        PoolStats const stats = pool.Stats();
        check(stats.expired == 1, "the old result counts as expired");
        check(pool.ForgetExpired(7) && !pool.ForgetExpired(7), "the expiry is told to its bot once");
        check(!pool.ForgetExpired(8), "a failed job's bot learns of no expired result");
        check(stats.failures == 2, "a job returning none and one throwing count as failures");
        sums(stats, "expiry and failures");
    }
    {
        PlannerPool pool;
        pool.Start(1);
        std::atomic<bool> entered{false};
        std::atomic<bool> open{false};
        pool.Submit(1, job(1, 1,
                           [&entered, &open](Snapshot const& snapshot) -> std::optional<Payload>
                           {
                               entered = true;
                               while (!open)
                                   std::this_thread::yield();
                               return NoopCompute(snapshot);
                           }));
        waitFor([&entered] { return entered.load(); });
        for (std::uint64_t bot = 2; bot <= 4; ++bot)
            pool.Submit(bot, job(bot, 1));
        pool.Post([] {});
        std::thread opener(
            [&open]
            {
                std::this_thread::sleep_for(50ms);
                open = true;
            });
        pool.Stop();
        opener.join();
        PoolStats const stats = pool.Stats();
        check(stats.dropped == 3 && stats.tasksDropped == 1, "Stop drops the waiting jobs and tasks");
        check(!pool.Submit(5, job(5, 1)), "after Stop the pool accepts nothing");
        pool.Stop();
        sums(pool.Stats(), "stop");
    }

    // d) Recheck: every verdict and the limits
    {
        Result result;
        result.stateVersion = 3;
        result.takenMs = 1000;
        result.position = {0, 0.0f, 0.0f, 0.0f};
        Limits const limits{STALE_MS, RECHECK_YARDS};
        CurrentState now{true, 3, {0, 0.0f, 0.0f, 0.0f}, 1000};
        check(Recheck(result, now, limits) == Verdict::Use, "recheck: unchanged is used");
        CurrentState dead = now;
        dead.alive = false;
        check(Recheck(result, dead, limits) == Verdict::Dead, "recheck: dead");
        CurrentState version = now;
        version.stateVersion = 4;
        check(Recheck(result, version, limits) == Verdict::Version, "recheck: state version changed");
        CurrentState edge = now;
        edge.position = {0, 60.0f, 80.0f, 0.0f};  // exactly RECHECK_YARDS
        check(Recheck(result, edge, limits) == Verdict::Use, "recheck: exactly at the yards limit is used");
        edge.position.y = 80.5f;
        check(Recheck(result, edge, limits) == Verdict::Position, "recheck: beyond the yards limit");
        CurrentState otherMap = now;
        otherMap.position.map = 1;
        check(Recheck(result, otherMap, limits) == Verdict::Position, "recheck: another map");
        CurrentState age = now;
        age.nowMs = 1000 + STALE_MS;
        check(Recheck(result, age, limits) == Verdict::Use, "recheck: exactly at the age limit is used");
        age.nowMs = 1001 + STALE_MS;
        check(Recheck(result, age, limits) == Verdict::Age, "recheck: older than the age limit");
        Result wrapped = result;
        wrapped.takenMs = 0xFFFFFF00u;
        CurrentState afterWrap = now;
        afterWrap.nowMs = 0x100u;
        check(Recheck(wrapped, afterWrap, limits) == Verdict::Use, "recheck: across the clock's wrap");
    }

    // e) Threads: SUBMITTERS threads x botsPerThread bots on WORKERS workers, then the older/newer race, Post tasks
    {
        PlannerPool pool;
        pool.Start(WORKERS);
        std::atomic<std::uint32_t> outsideWorker{0};
        Compute const noted = [&outsideWorker](Snapshot const& snapshot)
        {
            if (!PlannerPool::IsWorkerThread())
                ++outsideWorker;
            return NoopCompute(snapshot);
        };
        std::vector<std::vector<std::string>> threadFailures(SUBMITTERS);
        std::vector<std::thread> submitters;
        for (std::uint32_t t = 0; t < SUBMITTERS; ++t)
            submitters.emplace_back(
                [&, t]
                {
                    std::vector<std::string>& mine = threadFailures[t];
                    std::uint64_t const base = (t + 1) * 1000000ULL;
                    for (std::uint64_t i = 0; i < botsPerThread; ++i)
                        if (!pool.Submit(base + i, job(base + i, 1, noted)))
                            mine.push_back("a running pool refused a Submit");
                    std::vector<bool> got(botsPerThread, false);
                    std::uint32_t left = botsPerThread;
                    auto const until = std::chrono::steady_clock::now() + DEADLINE;
                    while (left && std::chrono::steady_clock::now() < until)
                    {
                        for (std::uint64_t i = 0; i < botsPerThread; ++i)
                        {
                            if (got[i])
                                continue;
                            std::optional<Result> const result = pool.TryTake(base + i);
                            if (!result)
                                continue;
                            got[i] = true;
                            --left;
                            if (result->bot != base + i || checksum(*result) != expected(base + i, 1))
                                mine.push_back("a threaded result differs from the single-thread one");
                        }
                        std::this_thread::yield();
                    }
                    if (left)
                        mine.push_back(std::to_string(left) + " threaded results lost");
                    // The race: an older job must never overwrite the newer one's result
                    for (std::uint64_t i = 0; i < RACE_ROUNDS; ++i)
                    {
                        std::uint64_t const bot = base + botsPerThread + i;
                        pool.Submit(bot, job(bot, 1, noted));
                        pool.Submit(bot, job(bot, 2, noted));
                        bool newer = false;
                        auto const raceUntil = std::chrono::steady_clock::now() + DEADLINE;
                        while (!newer && std::chrono::steady_clock::now() < raceUntil)
                        {
                            if (std::optional<Result> const result = pool.TryTake(bot))
                                newer = result->stateVersion == 2;
                            else
                                std::this_thread::yield();
                        }
                        if (!newer)
                            mine.push_back("the newer job's result never arrived");
                        else if (pool.TryTake(bot))
                            mine.push_back("an older result arrived after the newer one");
                    }
                });
        std::vector<std::atomic<std::uint32_t>> ran(TASKS);
        for (std::uint32_t i = 0; i < TASKS; ++i)
            pool.Post([&ran, i] { ++ran[i]; });
        for (std::thread& submitter : submitters)
            submitter.join();
        for (std::vector<std::string> const& mine : threadFailures)
            check(mine.empty(), mine.empty() ? "threads" : "threads: " + mine.front());
        check(waitFor([&pool] { return pool.Stats().tasksRun == TASKS; }), "every posted task runs");
        bool once = true;
        for (std::atomic<std::uint32_t> const& count : ran)
            once = once && count.load() == 1;
        check(once, "every posted task runs exactly once");
        check(outsideWorker.load() == 0, "IsWorkerThread is true inside every job");
        PoolStats const stats = pool.Stats();
        sums(stats, "threads");
        check(stats.threads == WORKERS && stats.busyNs > 0 && stats.computeNs > 0, "worker count and busy time");
        notes.push_back("threads: " + std::to_string(stats.run) + " jobs, compute p50/p95 " +
                        std::to_string(stats.computeP50Ns) + "/" + std::to_string(stats.computeP95Ns) +
                        " ns, queue wait p50/p95 " + std::to_string(stats.waitP50Ns) + "/" +
                        std::to_string(stats.waitP95Ns) + " ns, " + std::to_string(stats.superseded) + " superseded");
        pool.Stop();
    }

    // f) Snapshot sizes: worst case (25 quests, 2000 rewarded quests, every skill, the recent goals) and a typical bot
    {
        constexpr std::size_t WORST_REWARDED = 2000;
        constexpr std::size_t TYPICAL_QUESTS = 15;
        constexpr std::size_t TYPICAL_REWARDED = 300;
        constexpr std::size_t ALL_SKILLS = 15;
        constexpr std::size_t TYPICAL_SKILLS = 4;
        Snapshot worst;
        worst.quests.resize(QUEST_LOG_SLOTS);
        worst.rewardedQuests.resize(WORST_REWARDED);
        worst.skills.resize(ALL_SKILLS);
        worst.recentGoals.resize(RECENT_GOALS);
        worst.traits = TraitRules::BotTraits{};
        Snapshot typical;
        typical.quests.resize(TYPICAL_QUESTS);
        typical.rewardedQuests.resize(TYPICAL_REWARDED);
        typical.skills.resize(TYPICAL_SKILLS);
        typical.traits = TraitRules::BotTraits{};
        check(worst.ApproxBytes() < WORST_BYTES, "the worst-case snapshot stays under 16 KB");
        check(typical.ApproxBytes() < TYPICAL_BYTES, "a typical snapshot stays under 8 KB");
        notes.push_back("snapshot bytes: worst " + std::to_string(worst.ApproxBytes()) + ", typical " +
                        std::to_string(typical.ApproxBytes()));
    }
    return checks;
}
}  // namespace Planner

#endif
