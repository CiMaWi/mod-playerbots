/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLANNERBOTSTATE_H
#define PLAYERBOTS_PLANNERBOTSTATE_H

#include <memory>

#include "Define.h"

namespace Planner::GoalApply
{
struct BotGoals;  // GoalInterface.h
}

namespace Planner
{
// The planner's view of the bot (PlayerbotAI::planner; Planner::PlannerHook, PlannerService.cpp); a reset
// starts it fresh, which only costs a new snapshot (StatusEnd::ResetOf)
struct BotState
{
    uint32 stateVersion{0};    // counts the bot's situation changes; a result of an older version is not used
    uint32 stateKey{0};        // level, zone, life and group as last seen
    uint32 lastSnapshotMs{0};  // 0 = none yet
    bool snapshotDue{true};    // the situation changed or a goal ended since the last snapshot
    bool pending{false};       // a job is out; its result is taken before the next one is submitted
    std::shared_ptr<Planner::GoalApply::BotGoals> goals;  // the goal and plan of a planner bot (WT16)
};
}  // namespace Planner

#endif
