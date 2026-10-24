/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_THREADCHECK_H
#define PLAYERBOTS_THREADCHECK_H

#include <atomic>
#include <functional>
#include <thread>

#include "Log.h"

// Debug facility (Playerbots.DebugThreadChecks): detects world-thread-only code running on another thread, e.g. on a
// map update thread with MapUpdate.Threads > 1. Mark a world-only site with PLAYERBOT_CHECK_WORLD_THREAD("name").
namespace PlayerbotThreadCheck
{
// Set from PlayerbotAIConfig::Initialize; atomic so a config reload cannot race, read relaxed: one load when off.
inline std::atomic<bool> enabled{false};

// The world thread's id, saved at the top of the module's world OnUpdate. Default id until the first update.
inline std::atomic<std::thread::id> worldThread{};

inline void SetWorldThread() { worldThread.store(std::this_thread::get_id(), std::memory_order_relaxed); }

// True on the world thread, and on any thread before the first world update (startup loading, no false alarms).
inline bool IsWorldThread()
{
    std::thread::id const world = worldThread.load(std::memory_order_relaxed);
    return world == std::thread::id() || world == std::this_thread::get_id();
}

// Logs one error per call site; `reported` is the call site's own flag.
inline void ReportWrongThread(std::atomic<bool>& reported, char const* site, char const* file, int line)
{
    if (reported.exchange(true, std::memory_order_relaxed))
        return;

    LOG_ERROR("playerbots", "Thread check: {} ({}:{}) ran on thread {}, not on the world thread", site, file, line,
              std::hash<std::thread::id>{}(std::this_thread::get_id()));
}
}  // namespace PlayerbotThreadCheck

// A macro for the per-site flag and __FILE__/__LINE__; the static flag is local to each expansion.
#define PLAYERBOT_CHECK_WORLD_THREAD(site)                                                                           \
    do                                                                                                               \
    {                                                                                                                \
        if (PlayerbotThreadCheck::enabled.load(std::memory_order_relaxed) && !PlayerbotThreadCheck::IsWorldThread()) \
        {                                                                                                            \
            static std::atomic<bool> playerbotThreadCheckReported{false};                                            \
            PlayerbotThreadCheck::ReportWrongThread(playerbotThreadCheckReported, site, __FILE__, __LINE__);         \
        }                                                                                                            \
    } while (0)

#endif
