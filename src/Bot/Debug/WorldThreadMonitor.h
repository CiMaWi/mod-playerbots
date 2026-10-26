/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDTHREADMONITOR_H
#define _PLAYERBOT_WORLDTHREADMONITOR_H

#include <atomic>

#include "Define.h"

class ChatHandler;

// Busy share of the world thread (Playerbots.PerfMonWorldThread): the CPU time the world thread used divided by the
// wall time that passed, between two calls of the module's world OnUpdate (one full world tick). Waiting for the map
// update threads inside MapMgr::Update and the core's minimum sleep cost no CPU, so the rest of the wall time is the
// world thread waiting. The thread CPU clock on Windows is QueryThreadCycleTime (TSC cycles the thread ran), turned
// into microseconds with the TSC rate of the same tick (RDTSC against the steady clock); GetThreadTimes is kept as a
// cross-check (it charges whole clock ticks of 15.6 ms and misses a thread that wakes on the timer and sleeps before
// the next one). Elsewhere CLOCK_THREAD_CPUTIME_ID. The counters are cumulative: a reader takes the difference of two
// readings. O(1) per tick: a few clock reads at the top of OnUpdate, one at the bottom.
class WorldThreadMonitor
{
public:
    static WorldThreadMonitor& Instance()
    {
        static WorldThreadMonitor instance;
        return instance;
    }

    // Top of the module's world OnUpdate (world thread): closes the tick that ended here. Returns at once when off.
    void BeginHook();
    // Bottom of the module's world OnUpdate: adds the module hook's own wall time.
    void EndHook();

    // .playerbots worldthread: the cumulative counters and the core's tick percentiles (p50, p80, p95, max).
    static bool HandleCommand(ChatHandler* handler, char const* args);

private:
    WorldThreadMonitor() = default;

    // World thread only: the previous tick boundary (0 = none yet, also after the option was off) and the hook start.
    uint64 _lastWallUs = 0;
    uint64 _lastCpu = 0;  // Windows: thread cycles; elsewhere: thread CPU microseconds
    uint64 _lastTsc = 0;  // Windows only
    uint64 _lastTimesUs = 0;
    uint64 _hookStartUs = 0;

    // Read by the command (any thread)
    std::atomic<uint64> _ticks{0};
    std::atomic<uint64> _wallUs{0};
    std::atomic<uint64> _cpuUs{0};
    std::atomic<uint64> _timesUs{0};  // GetThreadTimes (Windows) or the same clock as _cpuUs
    std::atomic<uint64> _hookUs{0};
};

#endif
