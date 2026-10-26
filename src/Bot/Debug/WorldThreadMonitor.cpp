/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldThreadMonitor.h"

#include <chrono>

#include "Chat.h"
#include "PlayerbotAIConfig.h"
#include "ThreadCheck.h"
#include "UpdateTime.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <intrin.h>
#include <windows.h>
#else
#include <ctime>
#endif

namespace
{
uint64 WallUs()
{
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// CPU time (user + kernel) of the calling thread in microseconds from the clock-tick accounting (Windows) or the
// precise thread clock (elsewhere); 0 if the clock cannot be read
uint64 ThreadTimesUs()
{
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
        return 0;

    uint64 const kernel100ns = (static_cast<uint64>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    uint64 const user100ns = (static_cast<uint64>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    return (kernel100ns + user100ns) / 10;
#else
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts))
        return 0;

    return static_cast<uint64>(ts.tv_sec) * 1000000 + static_cast<uint64>(ts.tv_nsec) / 1000;
#endif
}

// The precise thread clock: Windows TSC cycles the thread ran, elsewhere thread CPU microseconds
uint64 ThreadCpu()
{
#ifdef _WIN32
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles))
        return 0;

    return cycles;
#else
    return ThreadTimesUs();
#endif
}

uint64 Tsc()
{
#ifdef _WIN32
    return __rdtsc();
#else
    return 0;
#endif
}
}  // namespace

void WorldThreadMonitor::BeginHook()
{
    if (!sPlayerbotAIConfig.PerfMonWorldThread)
    {
        _lastWallUs = 0;
        _hookStartUs = 0;
        return;
    }

    uint64 const wall = WallUs();
    uint64 const cpu = ThreadCpu();
    uint64 const tsc = Tsc();
    uint64 const times = ThreadTimesUs();
    if (_lastWallUs && wall > _lastWallUs && cpu >= _lastCpu && tsc >= _lastTsc && times >= _lastTimesUs)
    {
        uint64 const wallUs = wall - _lastWallUs;
        uint64 cpuUs = cpu - _lastCpu;
#ifdef _WIN32
        // cycles -> microseconds at this tick's TSC rate; without a TSC step the tick is not counted
        uint64 const tscTicks = tsc - _lastTsc;
        cpuUs = tscTicks ? static_cast<uint64>(static_cast<double>(cpuUs) * wallUs / tscTicks) : 0;
        if (tscTicks)
#endif
        {
            _ticks.fetch_add(1, std::memory_order_relaxed);
            _wallUs.fetch_add(wallUs, std::memory_order_relaxed);
            _cpuUs.fetch_add(cpuUs, std::memory_order_relaxed);
            _timesUs.fetch_add(times - _lastTimesUs, std::memory_order_relaxed);
        }
    }

    _lastWallUs = wall;
    _lastCpu = cpu;
    _lastTsc = tsc;
    _lastTimesUs = times;
    _hookStartUs = wall;
}

void WorldThreadMonitor::EndHook()
{
    if (!_hookStartUs)
        return;

    uint64 const wall = WallUs();
    if (wall >= _hookStartUs)
        _hookUs.fetch_add(wall - _hookStartUs, std::memory_order_relaxed);

    _hookStartUs = 0;
}

bool WorldThreadMonitor::HandleCommand(ChatHandler* handler, char const* /*args*/)
{
    WorldThreadMonitor const& monitor = Instance();
    uint64 const ticks = monitor._ticks.load(std::memory_order_relaxed);
    uint64 const wallUs = monitor._wallUs.load(std::memory_order_relaxed);
    uint64 const cpuUs = monitor._cpuUs.load(std::memory_order_relaxed);
    uint64 const hookUs = monitor._hookUs.load(std::memory_order_relaxed);
    uint64 const timesUs = monitor._timesUs.load(std::memory_order_relaxed);

    if (!sPlayerbotAIConfig.PerfMonWorldThread)
        handler->PSendSysMessage("World thread monitor off (Playerbots.PerfMonWorldThread = 0)");

    handler->PSendSysMessage("World thread counters: ticks {} wall {} us cpu {} us hook {} us", ticks, wallUs, cpuUs,
                             hookUs);
    if (wallUs)
    {
        handler->PSendSysMessage(
            "World thread since start: busy {:.1f}% of wall time, playerbots hook {:.1f}%, mean tick {:.1f} ms; "
            "thread times clock {:.1f}%",
            100.0 * cpuUs / wallUs, 100.0 * hookUs / wallUs, ticks ? wallUs / 1000.0 / ticks : 0.0,
            100.0 * timesUs / wallUs);
    }

    // The core sorts its tick table without a lock when it computes a percentile: world thread only (console commands
    // run there; a GM's chat command may run on a map thread)
    if (!PlayerbotThreadCheck::IsWorldThread())
    {
        handler->PSendSysMessage("Tick percentiles: only from the console (world thread)");
        return true;
    }

    if (!sWorldUpdateTime.GetDatasetSize())
    {
        handler->PSendSysMessage("Tick percentiles: no ticks recorded yet");
        return true;
    }

    handler->PSendSysMessage("Tick percentiles (last {} diffs): p50 {}ms, p80 {}ms, p95 {}ms, max {}ms",
                             sWorldUpdateTime.GetDatasetSize(), sWorldUpdateTime.GetPercentile(50),
                             sWorldUpdateTime.GetPercentile(80), sWorldUpdateTime.GetPercentile(95),
                             sWorldUpdateTime.GetPercentile(100));
    return true;
}
