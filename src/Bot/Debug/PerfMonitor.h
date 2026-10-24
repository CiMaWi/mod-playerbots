/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

/*
 * Ported from the CMaNGOS playerbots project (https://github.com/cmangos/playerbots), GPL v2,
 * with modifications for AzerothCore.
 * Original authors:
 *   ike3 <ike@email.org> - original author
 *   Sebastiaan Keek (mostlikely4r) <sebastiaan.keek@gmail.com>
 */

#ifndef PLAYERBOTS_PERFMONITOR_H
#define PLAYERBOTS_PERFMONITOR_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct PerformanceData;

// The rows of the operations a bot has started and not finished yet, innermost last
typedef std::vector<PerformanceData*> PerformanceStack;

struct PerformanceSnapshot
{
    uint64_t minTime = 0;
    uint64_t maxTime = 0;
    uint64_t totalTime = 0;
    uint64_t count = 0;
    uint64_t blocks = 0;
};

// Each thread records into its own shard (its own cache line) with relaxed atomics, so the map threads neither lock nor
// share a line; readers sum the shards. The shard of a thread is fixed (see PerfMonitor.cpp).
constexpr std::size_t PerfMonitorCacheLineSize = 64;

struct alignas(PerfMonitorCacheLineSize) PerformanceShard
{
    std::atomic<uint64_t> minTime{0};
    std::atomic<uint64_t> maxTime{0};
    std::atomic<uint64_t> totalTime{0};
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> blocks{0};
};

// One row of the monitor. Rows are created by the PerfMonitor registry and never freed, so the pointers that values,
// multipliers and performance stacks keep stay valid; Reset zeroes them in place.
struct PerformanceData
{
    static constexpr std::size_t ShardCount = 8;

    PerformanceData(std::string name, std::string chain);

    void Record(uint64_t elapsed);
    void CountBlock();
    PerformanceSnapshot Snapshot() const;
    void Clear();

    // Plain operation name; a child operation lists it in its own row name
    std::string const Name;
    // What the row name of a child operation puts between its brackets: this name, then the names of the parents
    std::string const Chain;

private:
    std::array<PerformanceShard, ShardCount> _shards;
};

enum PerformanceMetric
{
    PERF_MON_TRIGGER,
    PERF_MON_VALUE,
    PERF_MON_ACTION,
    PERF_MON_MULTIPLIER,
    PERF_MON_RNDBOT,
    PERF_MON_TOTAL
};

class PerfMonitorOperation
{
public:
    void finish();

private:
    friend class PerfMonitor;

    PerformanceData* _data = nullptr;
    PerformanceStack* _stack = nullptr;
    std::chrono::microseconds _started{};
};

class PerfMonitorScope
{
public:
    explicit PerfMonitorScope(PerformanceData* data);
    ~PerfMonitorScope();

    PerfMonitorScope(PerfMonitorScope const&) = delete;
    PerfMonitorScope& operator=(PerfMonitorScope const&) = delete;

private:
    PerformanceData* data;
    std::chrono::microseconds started{};
};

class PerfMonitor
{
public:
    static PerfMonitor& instance()
    {
        static PerfMonitor instance;

        return instance;
    }

    static bool IsEnabled();

    PerfMonitorOperation* start(PerformanceMetric metric, std::string_view name, PerformanceStack* stack = nullptr);
    PerformanceData* acquire(PerformanceMetric metric, std::string_view name);
    static void CountBlock(PerformanceData* data);
    void PrintStats(bool perTick = false, bool fullStack = false);
    void DumpJson(bool perTick = false);
    void Reset();

private:
    typedef std::map<PerformanceMetric, std::map<std::string, PerformanceSnapshot>> Snapshots;

    PerfMonitor() = default;
    virtual ~PerfMonitor() = default;

    PerfMonitor(PerfMonitor const&) = delete;
    PerfMonitor& operator=(PerfMonitor const&) = delete;

    PerfMonitor(PerfMonitor&&) = delete;
    PerfMonitor& operator=(PerfMonitor&&) = delete;

    PerformanceData* Find(PerformanceMetric metric, PerformanceData const* parent, std::string_view name);
    PerformanceData* GetOrCreate(PerformanceMetric metric, PerformanceData const* parent, std::string_view name);
    Snapshots TakeSnapshots();

    std::map<PerformanceMetric, std::map<std::string, PerformanceData*>> _rows;
    std::mutex _registryLock;
};

#define sPerfMonitor PerfMonitor::instance()

#endif
