/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeIntentEffect.h"

#include "ThreadCheck.h"

namespace PlayerbotBridge
{
PostMapJoin::PostMapJoin() : _thread(std::this_thread::get_id()) {}

bool PostMapJoin::Current() const
{
    // Unlike the diagnostic checker, an uninitialized world thread never grants authority.
    return _thread == std::this_thread::get_id() &&
           _thread == PlayerbotThreadCheck::worldThread.load(std::memory_order_relaxed);
}
}  // namespace PlayerbotBridge
