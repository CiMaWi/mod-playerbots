/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FleeAttackersAction.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "AiObjectContext.h"
#include "CensusMgr.h"
#include "Event.h"
#include "Log.h"
#include "Map.h"
#include "PathGenerator.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldLog.h"

namespace
{
// One step, and how long a step under way holds the turn (a 20 yd step takes about 3 s at run speed)
constexpr float STEP_YARDS = 20.0f;
constexpr uint32 STEP_HOLD_MS = 4 * IN_MILLISECONDS;
// A step this close to its end is done
constexpr float STEP_DONE_YARDS = 2.0f;
// A direction whose way ends this close is blocked, as is one whose path is this many times as long as it reaches
constexpr float STEP_BLOCKED_YARDS = 3.0f;
constexpr float STEP_DETOUR = 2.0f;
constexpr float FAN_STEP = static_cast<float>(M_PI) / 8.0f;
constexpr float FAN_LIMIT = static_cast<float>(M_PI) / 2.0f;
// After a look round that took no direction the next one waits this long (a look is up to nine paths)
constexpr uint32 LOOK_AGAIN_MS = IN_MILLISECONDS;
// The debug line's letter for each Step: taken, wall, edge, refused
constexpr char STEP_CODES[] = "TWER";
constexpr char const* EVENT_FLEE = "Flee";
}  // namespace

bool FleeAttackersAction::Due(PlayerbotAI* botAI, bool wants)
{
    FleeRules::Flight& flight = botAI->GetAiObjectContext()->GetValue<FleeRules::Flight&>("flee flight")->Get();
    return FleeRules::Due(flight, wants, botAI->GetBot()->GetCombatManager().HasPvPCombat());
}

bool FleeAttackersAction::isUseful() { return bot->IsAlive(); }

bool FleeAttackersAction::Execute(Event /*event*/)
{
    FleeRules::Flight& flight = context->GetValue<FleeRules::Flight&>("flee flight")->Get();
    uint32 const nowMs = getMSTime();
    // A flight does not follow the bot to another map (a teleport): it is dropped
    if (flight.active && flight.mapId != bot->GetMapId())
        flight = FleeRules::Flight();

    float x = 0.0f;
    float y = 0.0f;
    float nearestYards = 0.0f;
    uint32 const foes = Foes(x, y, nearestYards);
    FleeRules::Limits const limits{static_cast<float>(sPlayerbotAIConfig.FleeEscapeYards),
                                   sPlayerbotAIConfig.FleeCorneredSeconds * IN_MILLISECONDS,
                                   sPlayerbotAIConfig.FleeBoundSeconds * IN_MILLISECONDS};
    if (!flight.active)
    {
        if (!FleeRules::Begins(flight, foes, nearestYards, limits))
            return false;
        FleeRules::Begin(flight, nearestYards, nowMs);
        flight.mapId = bot->GetMapId();
        flight.startX = bot->GetPositionX();
        flight.startY = bot->GetPositionY();
        LOG_DEBUG("playerbots",
                  "[Flee] {} begins at ({:.1f}, {:.1f}, {:.1f}): {} foes, their centre ({:.1f}, {:.1f}) {:.1f} yd "
                  "off, the nearest {:.1f} yd",
                  bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), foes, x, y,
                  bot->GetExactDist2d(x, y), nearestYards);
    }

    bool const canMove = IsMovingAllowed();
    FleeRules::Outcome const outcome = FleeRules::Judge(flight, foes, nearestYards, canMove, nowMs, limits);
    if (outcome != FleeRules::Outcome::None)
    {
        End(flight, outcome, foes, nearestYards, nowMs);
        return false;
    }
    // A bot that cannot move now (rooted, stunned, feared) waits and may fight back meanwhile
    if (!canMove)
        return false;
    return StepAway(flight, x, y, nowMs);
}

uint32 FleeAttackersAction::Foes(float& x, float& y, float& nearestYards)
{
    x = 0.0f;
    y = 0.0f;
    nearestYards = 0.0f;
    uint32 foes = 0;
    // The PvP combat references hold whoever attacks the bot or it attacked. (getAttackers() holds only the units
    // whose melee victim the bot is: no caster)
    for (auto const& [guid, ref] : bot->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* const other = ref ? ref->GetOther(bot) : nullptr;
        if (!other || !other->IsInWorld() || other->GetMapId() != bot->GetMapId() || !other->IsAlive() ||
            other->IsTotem())
            continue;
        float const yards = bot->GetExactDist2d(other);
        if (!foes || yards < nearestYards)
            nearestYards = yards;
        x += other->GetPositionX();
        y += other->GetPositionY();
        ++foes;
    }
    if (foes)
    {
        x /= foes;
        y /= foes;
    }
    return foes;
}

void FleeAttackersAction::End(FleeRules::Flight& flight, FleeRules::Outcome outcome, uint32 foes, float nearestYards,
                              uint32 nowMs)
{
    float const yards = bot->GetExactDist2d(flight.startX, flight.startY);
    LOG_DEBUG("playerbots",
              "[Flee] {} at ({:.1f}, {:.1f}, {:.1f}) ends {} after {:.1f} s: {:.1f} yd from its start, {} foes, the "
              "nearest {:.1f} yd off",
              bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), FleeRules::Word(outcome),
              (nowMs - flight.startMs) / 1000.0f, yards, foes, nearestYards);
    // The row's id: the nearest attacker's yards at the end (at least 1), 0 when none is left
    if (WorldLog::Active())
        EventLog::Emit(bot, EVENT_FLEE, FleeRules::Word(outcome),
                       foes ? std::max<uint32>(1, static_cast<uint32>(std::lround(nearestYards))) : 0,
                       static_cast<int64>(std::lround(yards)));
    FleeRules::End(flight, outcome);
}

bool FleeAttackersAction::StepAway(FleeRules::Flight& flight, float fromX, float fromY, uint32 nowMs)
{
    if (bot->isMoving() && nowMs - flight.stepMs < STEP_HOLD_MS &&
        bot->GetExactDist2d(flight.stepX, flight.stepY) > STEP_DONE_YARDS)
        return true;
    // The last combat move (the rotation's approach, a step's end) still runs its delay, so a new move is refused:
    // the turn is held and the next tick steps
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
        return true;
    if (flight.lookFailed && nowMs - flight.lookMs < LOOK_AGAIN_MS)
        return flight.lookHolds;

    PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Flee attackers") : nullptr);
    float const dx = bot->GetPositionX() - fromX;
    float const dy = bot->GetPositionY() - fromY;
    float const away = dx * dx + dy * dy > 1.0f ? std::atan2(dy, dx) : bot->GetOrientation() + static_cast<float>(M_PI);
    uint32 tried = 0;
    uint32 blocked = 0;
    bool const logged = sLog->ShouldLog("playerbots", LogLevel::LOG_LEVEL_DEBUG);
    std::string fan;
    auto const step = [&](float delta)
    {
        float endYards = 0.0f;
        Step const result = StepTo(flight, away + delta, nowMs, endYards);
        ++tried;
        blocked += result == Step::Wall || result == Step::Edge ? 1 : 0;
        if (logged)
            fan += Acore::StringFormat(" {:+.0f}:{}{:.1f}", delta * 180.0f / static_cast<float>(M_PI),
                                       STEP_CODES[static_cast<uint8>(result)], endYards);
        return result == Step::Taken;
    };
    // A turn whose straight step was not taken says what each direction came to
    auto const note = [&](char const* outcome)
    {
        if (logged && tried > 1)
            LOG_DEBUG("playerbots", "[Flee] {} at ({:.1f}, {:.1f}, {:.1f}) {}, {} of {} blocked:{}", bot->GetName(),
                      bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), outcome, blocked, tried, fan);
    };
    bool taken = step(0.0f);
    for (float delta = FAN_STEP; !taken && delta <= FAN_LIMIT + 0.01f; delta += FAN_STEP)
        taken = step(delta) || step(-delta);
    flight.lookFailed = !taken;
    if (taken)
    {
        note("steps aside");
        return true;
    }
    // Blocked all round, the bot may fight back where it stands (the flight ends cornered if that lasts); a refused
    // move holds the turn
    flight.lookMs = nowMs;
    flight.lookHolds = blocked != tried;
    note(flight.lookHolds ? "holds" : "is blocked");
    return flight.lookHolds;
}

FleeAttackersAction::Step FleeAttackersAction::StepTo(FleeRules::Flight& flight, float angle, uint32 nowMs,
                                                      float& endYards)
{
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const botZ = bot->GetPositionZ();
    float x = botX + std::cos(angle) * STEP_YARDS;
    float y = botY + std::sin(angle) * STEP_YARDS;
    float z = botZ;
    // A step goes as far as the navmesh path to its point leads. (The navmesh ray of CheckCollisionAndGetValidCoords
    // ends at the bot itself on plain walkable ground, and MoveTo refuses a point its path does not reach.) A path
    // that ends right in front (a ledge, the mesh's end) or winds to its end is an edge
    PathGenerator path(bot);
    path.CalculatePath(botX, botY, botZ, x, y, z, false);
    Movement::PointsArray const& points = path.GetPath();
    endYards = 0.0f;
    if (path.GetPathType() & PATHFIND_NOT_USING_PATH)
    {
        // Swimming, flying or no navmesh under the bot: the straight line, as far as nothing is hit
        bool const clear = bot->GetMap()->CheckCollisionAndGetValidCoords(bot, botX, botY, botZ, x, y, z);
        endYards = bot->GetExactDist2d(x, y);
        if (!clear || endYards < STEP_BLOCKED_YARDS)
            return Step::Wall;
    }
    else
    {
        if ((path.GetPathType() & PATHFIND_NOPATH) || points.size() < 2)
            return Step::Wall;
        float length = 0.0f;
        for (std::size_t i = 1; i < points.size(); ++i)
            length += (points[i] - points[i - 1]).length();
        x = points.back().x;
        y = points.back().y;
        z = points.back().z;
        endYards = bot->GetExactDist2d(x, y);
        if (endYards < STEP_BLOCKED_YARDS || length > endYards * STEP_DETOUR)
            return Step::Edge;
    }
    if (!MoveTo(bot->GetMapId(), x, y, z, false, false, true, true, MovementPriority::MOVEMENT_COMBAT))
        return Step::Refused;
    // It runs: it attacks nobody meanwhile (a bot that could not move may have fought back)
    if (bot->GetVictim())
        bot->AttackStop();
    context->GetValue<Unit*>("current target")->Set(nullptr);
    flight.stepMs = nowMs;
    flight.stepX = x;
    flight.stepY = y;
    return Step::Taken;
}
