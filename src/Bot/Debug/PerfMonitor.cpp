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

#include "PerfMonitor.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#include "Config.h"
#include "Playerbots.h"

namespace
{
std::string MetricName(PerformanceMetric metric)
{
    switch (metric)
    {
        case PERF_MON_TRIGGER:
            return "Trigger";
        case PERF_MON_VALUE:
            return "Value";
        case PERF_MON_ACTION:
            return "Action";
        case PERF_MON_MULTIPLIER:
            return "Mult";
        case PERF_MON_RNDBOT:
            return "RndBot";
        case PERF_MON_TOTAL:
            return "Total";
        default:
            return "?";
    }
}

std::string JsonEscape(std::string const& value)
{
    std::ostringstream out;
    for (char c : value)
    {
        switch (c)
        {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<uint32>(c) << std::dec;
                else
                    out << c;
                break;
        }
    }

    return out.str();
}

std::string JsonNumber(double value)
{
    if (!std::isfinite(value))
        value = 0.0;

    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;

    return out.str();
}

std::string JsonRatio(double value, double total) { return JsonNumber(total > 0.0 ? value / total : 0.0); }

std::string JsonPath(bool perTick)
{
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir.push_back('/');

    return dir + (perTick ? "pmon_tick.json" : "pmon_total.json");
}
}  // namespace

namespace
{
static_assert((PerformanceData::ShardCount & (PerformanceData::ShardCount - 1)) == 0,
              "PerformanceData::ShardCount must be a power of two");

constexpr std::size_t HashGoldenRatio = 0x9e3779b97f4a7c15ull;
constexpr std::size_t HashShiftLeft = 6;
constexpr std::size_t HashShiftRight = 2;
constexpr std::size_t RowTableInitialSize = 1024;
constexpr std::size_t MaxPooledOperations = 4096;

std::chrono::microseconds Now()
{
    return (std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now()))
        .time_since_epoch();
}

// Threads get consecutive shards in the order they first record; with ShardCount at least the number of threads that
// run bot AI (map threads plus the world thread) no two of them share a shard.
std::size_t ThreadShard()
{
    static std::atomic<std::size_t> nextShard{0};
    thread_local std::size_t const shard =
        nextShard.fetch_add(1, std::memory_order_relaxed) & (PerformanceData::ShardCount - 1);

    return shard;
}

void HashCombine(std::size_t& hash, std::size_t value)
{
    hash ^= value + HashGoldenRatio + (hash << HashShiftLeft) + (hash >> HashShiftRight);
}

std::size_t HashKey(PerformanceMetric metric, PerformanceData const* parent, std::string_view name)
{
    std::size_t hash = std::hash<std::string_view>{}(name);
    HashCombine(hash, std::hash<PerformanceData const*>{}(parent));
    HashCombine(hash, static_cast<std::size_t>(metric));

    return hash;
}

struct RowTableEntry
{
    std::size_t hash = 0;
    PerformanceData const* parent = nullptr;
    PerformanceData* row = nullptr;
    PerformanceMetric metric = PERF_MON_TRIGGER;
};

// Per-thread map from (metric, parent row, name) to the row, so that start() finds its row without the registry mutex
// and without building the row name; only a miss goes to the registry. Rows are never freed, so entries stay valid.
class ThreadRowTable
{
public:
    PerformanceData* Find(std::size_t hash, PerformanceMetric metric, PerformanceData const* parent,
                          std::string_view name) const
    {
        if (_entries.empty())
            return nullptr;

        std::size_t const mask = _entries.size() - 1;
        // The table is at most half full, so the probe always reaches an empty slot
        for (std::size_t i = hash & mask; _entries[i].row; i = (i + 1) & mask)
        {
            RowTableEntry const& entry = _entries[i];
            if (entry.hash == hash && entry.parent == parent && entry.metric == metric && entry.row->Name == name)
                return entry.row;
        }

        return nullptr;
    }

    void Insert(std::size_t hash, PerformanceMetric metric, PerformanceData const* parent, PerformanceData* row)
    {
        if ((_used + 1) * 2 > _entries.size())
            Grow();

        Place({hash, parent, row, metric});
        ++_used;
    }

private:
    void Place(RowTableEntry const& entry)
    {
        std::size_t const mask = _entries.size() - 1;
        std::size_t i = entry.hash & mask;
        while (_entries[i].row)
            i = (i + 1) & mask;

        _entries[i] = entry;
    }

    void Grow()
    {
        std::vector<RowTableEntry> old = std::move(_entries);
        _entries.assign(old.empty() ? RowTableInitialSize : old.size() * 2, RowTableEntry());
        for (RowTableEntry const& entry : old)
            if (entry.row)
                Place(entry);
    }

    std::vector<RowTableEntry> _entries;
    std::size_t _used = 0;
};

thread_local ThreadRowTable threadRows;

// Operations are pooled per thread so that start() does not allocate. finish() may run on another thread than start()
// (the FullTick operation ends at the next UpdateAI of its AI, which can run on another map thread), so an operation
// goes back to the pool of the finishing thread; the cap keeps a one-way flow from growing a pool without bound.
struct OperationPool
{
    ~OperationPool()
    {
        for (PerfMonitorOperation* operation : operations)
            delete operation;
    }

    std::vector<PerfMonitorOperation*> operations;
};

thread_local OperationPool operationPool;

PerfMonitorOperation* AcquireOperation()
{
    std::vector<PerfMonitorOperation*>& operations = operationPool.operations;
    if (operations.empty())
        return new PerfMonitorOperation();

    PerfMonitorOperation* operation = operations.back();
    operations.pop_back();

    return operation;
}

void ReleaseOperation(PerfMonitorOperation* operation)
{
    std::vector<PerfMonitorOperation*>& operations = operationPool.operations;
    if (operations.size() >= MaxPooledOperations)
    {
        delete operation;
        return;
    }

    if (operations.capacity() < MaxPooledOperations)
        operations.reserve(MaxPooledOperations);

    operations.push_back(operation);
}

void PopStack(PerformanceStack& stack, PerformanceData const* data)
{
    if (!stack.empty() && stack.back() == data)
    {
        stack.pop_back();
        return;
    }

    // Not properly nested: drop every entry of that name, as the monitor did when the stack held names
    stack.erase(std::remove_if(stack.begin(), stack.end(),
                               [data](PerformanceData const* entry) { return entry->Name == data->Name; }),
                stack.end());
}
}  // namespace

PerformanceData::PerformanceData(std::string name, std::string chain) : Name(std::move(name)), Chain(std::move(chain))
{
}

void PerformanceData::Record(uint64_t elapsed)
{
    // Relaxed: the counters are independent statistics; readers only need each value to be whole, not ordered
    PerformanceShard& shard = _shards[ThreadShard()];
    if (elapsed > 0)
    {
        uint64_t minTime = shard.minTime.load(std::memory_order_relaxed);
        while ((!minTime || minTime > elapsed) &&
               !shard.minTime.compare_exchange_weak(minTime, elapsed, std::memory_order_relaxed))
        {
        }

        uint64_t maxTime = shard.maxTime.load(std::memory_order_relaxed);
        while (maxTime < elapsed && !shard.maxTime.compare_exchange_weak(maxTime, elapsed, std::memory_order_relaxed))
        {
        }

        shard.totalTime.fetch_add(elapsed, std::memory_order_relaxed);
    }

    shard.count.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceData::CountBlock() { _shards[ThreadShard()].blocks.fetch_add(1, std::memory_order_relaxed); }

PerformanceSnapshot PerformanceData::Snapshot() const
{
    PerformanceSnapshot snapshot;
    for (PerformanceShard const& shard : _shards)
    {
        uint64_t const minTime = shard.minTime.load(std::memory_order_relaxed);
        if (minTime && (!snapshot.minTime || snapshot.minTime > minTime))
            snapshot.minTime = minTime;

        snapshot.maxTime = std::max(snapshot.maxTime, shard.maxTime.load(std::memory_order_relaxed));
        snapshot.totalTime += shard.totalTime.load(std::memory_order_relaxed);
        snapshot.count += shard.count.load(std::memory_order_relaxed);
        snapshot.blocks += shard.blocks.load(std::memory_order_relaxed);
    }

    return snapshot;
}

void PerformanceData::Clear()
{
    for (PerformanceShard& shard : _shards)
    {
        shard.minTime.store(0, std::memory_order_relaxed);
        shard.maxTime.store(0, std::memory_order_relaxed);
        shard.totalTime.store(0, std::memory_order_relaxed);
        shard.count.store(0, std::memory_order_relaxed);
        shard.blocks.store(0, std::memory_order_relaxed);
    }
}

bool PerfMonitor::IsEnabled() { return sPlayerbotAIConfig.PerfMonEnabled; }

PerformanceData* PerfMonitor::GetOrCreate(PerformanceMetric metric, PerformanceData const* parent,
                                          std::string_view name)
{
    // Row name: "name" without parents, else "name [parent|grandparent|...]" (innermost parent first)
    std::string label(name);
    std::string chain(name);
    if (parent)
    {
        label.append(" [").append(parent->Chain).append("]");
        chain.append("|").append(parent->Chain);
    }

    std::lock_guard<std::mutex> guard(_registryLock);
    PerformanceData*& row = _rows[metric][label];
    if (!row)
        row = new PerformanceData(std::string(name), std::move(chain));

    return row;
}

PerformanceData* PerfMonitor::Find(PerformanceMetric metric, PerformanceData const* parent, std::string_view name)
{
    std::size_t const hash = HashKey(metric, parent, name);
    if (PerformanceData* row = threadRows.Find(hash, metric, parent, name))
        return row;

    PerformanceData* row = GetOrCreate(metric, parent, name);
    threadRows.Insert(hash, metric, parent, row);

    return row;
}

PerformanceData* PerfMonitor::acquire(PerformanceMetric metric, std::string_view name)
{
    if (!IsEnabled())
        return nullptr;

    return Find(metric, nullptr, name);
}

void PerfMonitor::CountBlock(PerformanceData* data)
{
    if (!data)
        return;

    data->CountBlock();
}

PerfMonitorOperation* PerfMonitor::start(PerformanceMetric metric, std::string_view name, PerformanceStack* stack)
{
    if (!IsEnabled())
        return nullptr;

    PerformanceData const* parent = stack && !stack->empty() ? stack->back() : nullptr;
    PerformanceData* row = Find(metric, parent, name);
    if (stack)
        stack->push_back(row);

    PerfMonitorOperation* operation = AcquireOperation();
    operation->_data = row;
    operation->_stack = stack;
    operation->_started = Now();

    return operation;
}

PerfMonitor::Snapshots PerfMonitor::TakeSnapshots()
{
    Snapshots snapshots;

    std::lock_guard<std::mutex> guard(_registryLock);
    for (auto const& metricRows : _rows)
    {
        std::map<std::string, PerformanceSnapshot>& rows = snapshots[metricRows.first];
        for (auto const& row : metricRows.second)
            rows[row.first] = row.second->Snapshot();
    }

    return snapshots;
}

void PerfMonitor::PrintStats(bool perTick, bool fullStack)
{
    Snapshots const snapshots = TakeSnapshots();
    if (snapshots.empty())
        return;

    if (!perTick)
    {
        float updateAITotalTime = 0;
        auto const totals = snapshots.find(PERF_MON_TOTAL);
        if (totals != snapshots.end())
            for (auto const& row : totals->second)
                if (row.first.find("PlayerbotAI::UpdateAIInternal") != std::string::npos)
                    updateAITotalTime += static_cast<float>(row.second.totalTime);

        LOG_INFO(
            "playerbots",
            "--------------------------------------[TOTAL BOT]------------------------------------------------------");
        LOG_INFO("playerbots",
                 "percentage     time  |     min ..     max (      avg  of      count) - type      : name");
        LOG_INFO(
            "playerbots",
            "-------------------------------------------------------------------------------------------------------");

        for (auto const& metricRows : snapshots)
        {
            PerformanceMetric const metric = metricRows.first;
            std::map<std::string, PerformanceSnapshot> const& pdMap = metricRows.second;

            std::string const key = MetricName(metric);

            std::vector<std::string> names;

            for (auto const& row : pdMap)
            {
                if (key == "Total" && row.first.find("PlayerbotAI::UpdateAIInternal") == std::string::npos)
                    continue;

                names.push_back(row.first);
            }

            std::sort(names.begin(), names.end(), [&pdMap](std::string const& i, std::string const& j)
                      { return pdMap.at(i).totalTime < pdMap.at(j).totalTime; });

            uint64 typeTotalTime = 0;
            uint64 typeMinTime = 0xffffffffu;
            uint64 typeMaxTime = 0;
            uint64 typeCount = 0;
            uint64 typeBlocks = 0;
            for (auto const& name : names)
            {
                PerformanceSnapshot const& pd = pdMap.at(name);
                typeTotalTime += pd.totalTime;
                typeCount += pd.count;
                typeBlocks += pd.blocks;
                if (typeMinTime > pd.minTime)
                    typeMinTime = pd.minTime;
                if (typeMaxTime < pd.maxTime)
                    typeMaxTime = pd.maxTime;
                float perc = (float)pd.totalTime / updateAITotalTime * 100.0f;
                float time = (float)pd.totalTime / 1000000.0f;
                float minTime = (float)pd.minTime / 1000.0f;
                float maxTime = (float)pd.maxTime / 1000.0f;
                float avg = (float)pd.totalTime / (float)pd.count / 1000.0f;
                std::string disName = name;
                if (!fullStack && disName.find("|") != std::string::npos)
                    disName = disName.substr(0, disName.find("|")) + "]";

                bool show = perc >= 0.1f || avg >= 0.25f || pd.maxTime > 1000;
                if (metric == PERF_MON_MULTIPLIER)
                {
                    if (pd.blocks)
                        disName += " [blocked " + std::to_string(pd.blocks) + "]";

                    show = show || pd.blocks > 0 || perc >= 0.01f;
                }

                if (show)
                {
                    LOG_INFO("playerbots",
                             "{:7.3f}% {:10.3f}s | {:7.1f} .. {:7.1f} ({:10.3f} of {:10d}) - {:6}    : {}", perc, time,
                             minTime, maxTime, avg, pd.count, key.c_str(), disName.c_str());
                }
            }
            float tPerc = (float)typeTotalTime / (float)updateAITotalTime * 100.0f;
            float tTime = (float)typeTotalTime / 1000000.0f;
            float tMinTime = (float)typeMinTime / 1000.0f;
            float tMaxTime = (float)typeMaxTime / 1000.0f;
            float tAvg = (float)typeTotalTime / (float)typeCount / 1000.0f;
            std::string totalName = "Total";
            if (metric == PERF_MON_MULTIPLIER && typeBlocks)
                totalName += " [blocked " + std::to_string(typeBlocks) + "]";

            LOG_INFO("playerbots", "{:7.3f}% {:10.3f}s | {:7.1f} .. {:7.1f} ({:10.3f} of {:10d}) - {:6}    : {}", tPerc,
                     tTime, tMinTime, tMaxTime, tAvg, typeCount, key.c_str(), totalName.c_str());
            LOG_INFO("playerbots", " ");
        }
    }
    else
    {
        auto const totals = snapshots.find(PERF_MON_TOTAL);
        if (totals == snapshots.end() || !totals->second.count("PlayerbotAIBase::FullTick"))
        {
            LOG_INFO("playerbots", "Performance monitor: no PlayerbotAIBase::FullTick sample yet");
            return;
        }

        PerformanceSnapshot const& fullTick = totals->second.at("PlayerbotAIBase::FullTick");
        float fullTickCount = static_cast<float>(fullTick.count);
        float fullTickTotalTime = static_cast<float>(fullTick.totalTime);

        LOG_INFO(
            "playerbots",
            "---------------------------------------[PER TICK]------------------------------------------------------");
        LOG_INFO("playerbots",
                 "percentage     time  |     min ..     max (      avg  of      count) - type      : name");
        LOG_INFO(
            "playerbots",
            "-------------------------------------------------------------------------------------------------------");

        for (auto const& metricRows : snapshots)
        {
            PerformanceMetric const metric = metricRows.first;
            std::map<std::string, PerformanceSnapshot> const& pdMap = metricRows.second;

            std::string const key = MetricName(metric);

            std::vector<std::string> names;

            for (auto const& row : pdMap)
            {
                names.push_back(row.first);
            }

            std::sort(names.begin(), names.end(), [&pdMap](std::string const& i, std::string const& j)
                      { return pdMap.at(i).totalTime < pdMap.at(j).totalTime; });

            uint64 typeTotalTime = 0;
            uint64 typeMinTime = 0xffffffffu;
            uint64 typeMaxTime = 0;
            uint64 typeCount = 0;
            uint64 typeBlocks = 0;
            for (auto const& name : names)
            {
                PerformanceSnapshot const& pd = pdMap.at(name);
                typeTotalTime += pd.totalTime;
                typeCount += pd.count;
                typeBlocks += pd.blocks;
                if (typeMinTime > pd.minTime)
                    typeMinTime = pd.minTime;
                if (typeMaxTime < pd.maxTime)
                    typeMaxTime = pd.maxTime;
                float perc = (float)pd.totalTime / fullTickTotalTime * 100.0f;
                float time = (float)pd.totalTime / fullTickCount / 1000.0f;
                float minTime = (float)pd.minTime / 1000.0f;
                float maxTime = (float)pd.maxTime / 1000.0f;
                float avg = (float)pd.totalTime / (float)pd.count / 1000.0f;
                float amount = (float)pd.count / fullTickCount;
                std::string disName = name;
                if (!fullStack && disName.find("|") != std::string::npos)
                    disName = disName.substr(0, disName.find("|")) + "]";
                bool show = perc >= 0.1f || avg >= 0.25f || pd.maxTime > 1000;
                if (metric == PERF_MON_MULTIPLIER)
                {
                    if (pd.blocks)
                        disName += " [blocked " + std::to_string(pd.blocks) + "]";

                    show = show || pd.blocks > 0 || perc >= 0.01f;
                }

                if (show)
                {
                    LOG_INFO("playerbots",
                             "{:7.3f}% {:9.3f}ms | {:7.1f} .. {:7.1f} ({:10.3f} of {:10.2f}) - {:6}    : {}", perc,
                             time, minTime, maxTime, avg, amount, key.c_str(), disName.c_str());
                }
            }
            if (metric != PERF_MON_TOTAL)
            {
                float tPerc = (float)typeTotalTime / (float)fullTickTotalTime * 100.0f;
                float tTime = (float)typeTotalTime / fullTickCount / 1000.0f;
                float tMinTime = (float)typeMinTime / 1000.0f;
                float tMaxTime = (float)typeMaxTime / 1000.0f;
                float tAvg = (float)typeTotalTime / (float)typeCount / 1000.0f;
                float tAmount = (float)typeCount / fullTickCount;
                std::string totalName = "Total";
                if (metric == PERF_MON_MULTIPLIER && typeBlocks)
                    totalName += " [blocked " + std::to_string(typeBlocks) + "]";

                LOG_INFO("playerbots", "{:7.3f}% {:9.3f}ms | {:7.1f} .. {:7.1f} ({:10.3f} of {:10.2f}) - {:6}    : {}",
                         tPerc, tTime, tMinTime, tMaxTime, tAvg, tAmount, key.c_str(), totalName.c_str());
            }
            LOG_INFO("playerbots", " ");
        }
    }
}

void PerfMonitor::DumpJson(bool perTick)
{
    Snapshots const snapshots = TakeSnapshots();
    if (snapshots.empty())
        return;

    struct Sample
    {
        std::string name;
        uint64 totalTime;
        uint64 minTime;
        uint64 maxTime;
        uint64 count;
        uint64 blocks;
    };

    std::map<PerformanceMetric, std::vector<Sample>> samples;
    for (auto const& metric : snapshots)
    {
        std::vector<Sample>& rows = samples[metric.first];
        for (auto const& entry : metric.second)
        {
            PerformanceSnapshot const& pd = entry.second;
            rows.push_back({entry.first, pd.totalTime, pd.minTime, pd.maxTime, pd.count, pd.blocks});
        }

        std::sort(rows.begin(), rows.end(), [](Sample const& i, Sample const& j) { return i.totalTime > j.totalTime; });
    }

    double updateAiTotalTime = 0.0;
    double fullTickTotalTime = 0.0;
    double fullTickCount = 0.0;
    auto const totals = samples.find(PERF_MON_TOTAL);
    if (totals != samples.end())
    {
        for (Sample const& row : totals->second)
        {
            if (row.name.find("PlayerbotAI::UpdateAIInternal") != std::string::npos)
                updateAiTotalTime += static_cast<double>(row.totalTime);

            if (row.name == "PlayerbotAIBase::FullTick")
            {
                fullTickTotalTime = static_cast<double>(row.totalTime);
                fullTickCount = static_cast<double>(row.count);
            }
        }
    }

    double const denominator = perTick ? fullTickTotalTime : updateAiTotalTime;

    char stamp[32] = "";
    std::time_t const now = std::time(nullptr);
    if (std::tm const* utc = std::gmtime(&now))
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", utc);

    std::ostringstream out;
    out << "{\n";
    out << "  \"generatedAt\": " << static_cast<uint64>(now) << ",\n";
    out << "  \"generatedAtUtc\": \"" << stamp << "\",\n";
    out << "  \"mode\": \"" << (perTick ? "tick" : "total") << "\",\n";
    out << "  \"enabled\": " << (sPlayerbotAIConfig.PerfMonEnabled ? "true" : "false") << ",\n";
    out << "  \"timeUnit\": \"microseconds\",\n";
    out << "  \"fullTickCount\": " << static_cast<uint64>(fullTickCount) << ",\n";
    out << "  \"fullTickTotalTime\": " << static_cast<uint64>(fullTickTotalTime) << ",\n";
    out << "  \"updateAiTotalTime\": " << static_cast<uint64>(updateAiTotalTime) << ",\n";
    out << "  \"metrics\": [\n";

    bool firstMetric = true;
    for (auto const& metric : samples)
    {
        if (!firstMetric)
            out << ",\n";
        firstMetric = false;

        out << "    {\n";
        out << "      \"type\": \"" << MetricName(metric.first) << "\",\n";

        if (metric.first != PERF_MON_TOTAL)
        {
            uint64 typeTotalTime = 0;
            uint64 typeMinTime = 0;
            uint64 typeMaxTime = 0;
            uint64 typeCount = 0;
            uint64 typeBlocks = 0;
            bool firstSample = true;
            for (Sample const& row : metric.second)
            {
                typeTotalTime += row.totalTime;
                typeCount += row.count;
                typeBlocks += row.blocks;
                if (firstSample || typeMinTime > row.minTime)
                    typeMinTime = row.minTime;
                if (typeMaxTime < row.maxTime)
                    typeMaxTime = row.maxTime;
                firstSample = false;
            }

            out << "      \"totalTime\": " << typeTotalTime << ",\n";
            out << "      \"count\": " << typeCount << ",\n";
            out << "      \"blocks\": " << typeBlocks << ",\n";
            out << "      \"minTime\": " << typeMinTime << ",\n";
            out << "      \"maxTime\": " << typeMaxTime << ",\n";
            out << "      \"avgTime\": "
                << JsonRatio(static_cast<double>(typeTotalTime), static_cast<double>(typeCount)) << ",\n";
            out << "      \"percent\": "
                << JsonNumber(denominator > 0.0 ? static_cast<double>(typeTotalTime) / denominator * 100.0 : 0.0)
                << ",\n";
            out << "      \"timePerTick\": " << JsonRatio(static_cast<double>(typeTotalTime), fullTickCount) << ",\n";
            out << "      \"callsPerTick\": " << JsonRatio(static_cast<double>(typeCount), fullTickCount) << ",\n";
        }

        out << "      \"rows\": [\n";

        bool firstRow = true;
        for (Sample const& row : metric.second)
        {
            if (!firstRow)
                out << ",\n";
            firstRow = false;

            out << "        {\"name\": \"" << JsonEscape(row.name) << "\"";
            out << ", \"totalTime\": " << row.totalTime;
            out << ", \"count\": " << row.count;
            out << ", \"blocks\": " << row.blocks;
            out << ", \"minTime\": " << row.minTime;
            out << ", \"maxTime\": " << row.maxTime;
            out << ", \"avgTime\": " << JsonRatio(static_cast<double>(row.totalTime), static_cast<double>(row.count));
            out << ", \"percent\": "
                << JsonNumber(denominator > 0.0 ? static_cast<double>(row.totalTime) / denominator * 100.0 : 0.0);
            out << ", \"timePerTick\": " << JsonRatio(static_cast<double>(row.totalTime), fullTickCount);
            out << ", \"callsPerTick\": " << JsonRatio(static_cast<double>(row.count), fullTickCount);
            out << "}";
        }

        out << (firstRow ? "" : "\n") << "      ]\n";
        out << "    }";
    }

    out << (firstMetric ? "" : "\n") << "  ]\n";
    out << "}\n";

    std::string const path = JsonPath(perTick);
    std::string const tempPath = path + ".tmp";

    std::ofstream file(tempPath.c_str(), std::ios::out | std::ios::trunc);
    if (!file)
    {
        LOG_ERROR("playerbots", "Performance monitor could not write {}", tempPath);
        return;
    }

    file << out.str();
    file.close();

    if (!file)
    {
        LOG_ERROR("playerbots", "Performance monitor failed to write {}", tempPath);

        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        return;
    }

    std::error_code renameError;
    std::filesystem::rename(tempPath, path, renameError);
    if (renameError)
    {
        LOG_ERROR("playerbots", "Performance monitor could not replace {}: {}", path, renameError.message());

        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        return;
    }

    LOG_INFO("playerbots", "Performance monitor dump written to {}", path);
}

void PerfMonitor::Reset()
{
    // Records that run during the reset may be partly lost; a row is never freed, so nothing dangles
    std::lock_guard<std::mutex> guard(_registryLock);
    for (auto const& metricRows : _rows)
        for (auto const& row : metricRows.second)
            row.second->Clear();
}

void PerfMonitorOperation::finish()
{
    _data->Record((Now() - _started).count());

    if (_stack)
        PopStack(*_stack, _data);

    ReleaseOperation(this);
}

PerfMonitorScope::PerfMonitorScope(PerformanceData* data) : data(data)
{
    if (!data)
        return;

    started = Now();
}

PerfMonitorScope::~PerfMonitorScope()
{
    if (!data)
        return;

    data->Record((Now() - started).count());
}
