/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ACTIVITYBUDGET_H
#define _PLAYERBOT_ACTIVITYBUDGET_H

#include <atomic>

#include "Define.h"

// The tick time (ms) smart scale (AutoScaleActivity) follows. With Playerbots.botActiveAloneSmartScalePercentile = 0 it
// is the core's maximum over its last ticks, read directly. Otherwise Update() stores that percentile of the core's
// last-500-tick table once per world tick: the core sorts a shared array without a lock, so only the world thread may
// call it, and map threads only read the atomic.
class ActivityBudget
{
public:
    static ActivityBudget& Instance()
    {
        static ActivityBudget instance;
        return instance;
    }

    // World thread only (world OnUpdate)
    void Update();

    // Any thread
    uint32 GetTickMs() const;

private:
    ActivityBudget() = default;

    std::atomic<uint32> _tickMs{0};
};

#endif
