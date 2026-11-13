/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldPvpStrategy.h"

#include <cmath>

#include "Event.h"
#include "FightBook.h"
#include "FightWiring.h"
#include "Map.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Timer.h"

namespace
{
// One step of the retreat, and how long a step under way holds the turn (a 20 yd step takes about 3 s at run speed)
constexpr float STEP_YARDS = 20.0f;
constexpr uint32 STEP_HOLD_MS = 4 * IN_MILLISECONDS;
// A step this close to its end is done
constexpr float STEP_DONE_YARDS = 2.0f;
// A direction whose first collision lies this close is blocked
constexpr float STEP_BLOCKED_YARDS = 3.0f;
constexpr float FAN_STEP = static_cast<float>(M_PI) / 8.0f;
constexpr float FAN_LIMIT = static_cast<float>(M_PI) / 2.0f;
}  // namespace

void WorldPvpStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("world pvp retreat", {NextAction("world pvp retreat", ACTION_EMERGENCY + 1.0f)}));
}

bool WorldPvpRetreatTrigger::IsActive()
{
    float x = 0.0f;
    float y = 0.0f;
    return WorldPvp::RetreatFrom(botAI, x, y);
}

bool WorldPvpRetreatAction::isUseful()
{
    float x = 0.0f;
    float y = 0.0f;
    return bot->IsAlive() && WorldPvp::RetreatFrom(botAI, x, y);
}

bool WorldPvpRetreatAction::Execute(Event /*event*/)
{
    float fromX = 0.0f;
    float fromY = 0.0f;
    if (!WorldPvp::RetreatFrom(botAI, fromX, fromY))
        return false;

    uint32 const nowMs = getMSTime();
    if (bot->isMoving() && nowMs - _stepMs < STEP_HOLD_MS && bot->GetExactDist2d(_stepX, _stepY) > STEP_DONE_YARDS)
        return true;
    // A bot that cannot move now (rooted, stunned, feared) waits and may fight back meanwhile; the window bounds it
    if (!IsMovingAllowed())
        return false;
    // The last combat move (the rotation's approach, a step's end) still runs its delay, so a new move is refused:
    // the turn is held and the next tick steps
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
        return true;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "WorldPvp retreat")
                                                    : nullptr);
    float const dx = bot->GetPositionX() - fromX;
    float const dy = bot->GetPositionY() - fromY;
    float const away = dx * dx + dy * dy > 1.0f ? std::atan2(dy, dx) : bot->GetOrientation() + static_cast<float>(M_PI);
    uint32 tried = 0;
    uint32 blocked = 0;
    auto const step = [&](float angle)
    {
        bool wall = false;
        bool const taken = StepTo(angle, nowMs, wall);
        ++tried;
        blocked += wall ? 1 : 0;
        return taken;
    };
    if (step(away))
        return true;
    for (float delta = FAN_STEP; delta <= FAN_LIMIT + 0.01f; delta += FAN_STEP)
        if (step(away + delta) || step(away - delta))
            return true;
    // Only walls in every direction give the retreat up (the bot fights where it stands); a refused move holds
    if (!WorldPvp::GivesUpRetreat(blocked, tried))
        return true;
    WorldPvp::GiveUpRetreat(botAI);
    return false;
}

bool WorldPvpRetreatAction::StepTo(float angle, uint32 nowMs, bool& blocked)
{
    float x = bot->GetPositionX() + std::cos(angle) * STEP_YARDS;
    float y = bot->GetPositionY() + std::sin(angle) * STEP_YARDS;
    float z = bot->GetPositionZ();
    bool exact = true;
    blocked = false;
    bool const clear = bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                                      bot->GetPositionZ(), x, y, z);
    // The straight line ends at (x, y): at the first collision, or, with no collision, where the navmesh ends (a
    // ledge, a slope too steep, most walls). An end right in front blocks the direction either way: a step there does
    // not move the bot, and taken every tick it kept the other directions untried for the whole retreat
    if (bot->GetExactDist2d(x, y) < STEP_BLOCKED_YARDS)
    {
        blocked = true;
        return false;
    }
    if (!clear)
    {
        // A farther collision is walked around on a path to the unclipped point
        x = bot->GetPositionX() + std::cos(angle) * STEP_YARDS;
        y = bot->GetPositionY() + std::sin(angle) * STEP_YARDS;
        z = bot->GetPositionZ();
        exact = false;
    }
    if (!MoveTo(bot->GetMapId(), x, y, z, false, false, true, exact, MovementPriority::MOVEMENT_COMBAT))
        return false;
    _stepMs = nowMs;
    _stepX = x;
    _stepY = y;
    return true;
}
