/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_WORLDTHREAD_H
#define PLAYERBOTS_WORLDTHREAD_H

#include <atomic>
#include <thread>

namespace PlayerbotThreadCheck
{
// The world thread's id, saved at the top of the module's world OnUpdate. Default id until the first update.
inline std::atomic<std::thread::id> worldThread{};

inline void SetWorldThread() { worldThread.store(std::this_thread::get_id(), std::memory_order_relaxed); }

// True on the world thread, and on any thread before the first world update (startup loading, no false alarms).
inline bool IsWorldThread()
{
    std::thread::id const world = worldThread.load(std::memory_order_relaxed);
    return world == std::thread::id() || world == std::this_thread::get_id();
}
}  // namespace PlayerbotThreadCheck

#endif
