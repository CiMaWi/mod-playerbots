/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotTravel.h"
#include "HeldTrip.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "MeshMoveRules.h"
#include "MoveSpline.h"
#include "NewRpgAction.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "TravelLegRules.h"
#include "TravelMgr.h"
#include "TravelProgress.h"
#include "TravelWalkRules.h"

// The far walk of the route follower's legs (WT64b): the walk NewRpgBaseAction::MoveFarTo was, for a held trip only.
// No status calls it: a status walks by GoTo (NewRpgGoTo.cpp), and its walk beyond a local piece is a trip whose legs
// are walked here. The stepping stone in a forward cone stays until WT64g

namespace
{
// The failed-act trace writes at most one line per bot in this time
constexpr uint32 WALK_FAIL_TRACE_MS = 10 * IN_MILLISECONDS;

static_assert(TravelWalk::PATH_NOPATH == PATHFIND_NOPATH &&
                  TravelWalk::PATH_NOT_USING_PATH == PATHFIND_NOT_USING_PATH &&
                  TravelWalk::PATH_FARFROMPOLY_END == PATHFIND_FARFROMPOLY_END,
              "TravelWalk reads the engine's PathType bits");

// An order's engine answer as TravelWalk judges it: the type, the path's last step (a shortcut's whole line) and what
// its end gains on dest
TravelWalk::EngineAnswer ReadWalkAnswer(Player* bot, PathGenerator const& path, bool result, WorldPosition const& dest)
{
    TravelWalk::EngineAnswer answer;
    answer.result = result;
    answer.type = path.GetPathType();
    Movement::PointsArray const& points = path.GetPath();
    if (points.size() >= 2)
        answer.lastStep = (points[points.size() - 1] - points[points.size() - 2]).length();
    G3D::Vector3 const& end = path.GetActualEndPosition();
    answer.gain = bot->GetExactDist(dest) - dest.GetExactDist(end.x, end.y, end.z);
    return answer;
}
}  // namespace

bool NewRpgTravelAction::MoveFarTo(WorldPosition dest)
{
    auto* const travel = botAI->GetTravel().Trip();
    if (!travel)
        return false;
    if (!TravelLegs::HasWalkTarget(travel->trip, travel->bindInn))
        return MoveFarToImpl(*travel, dest);
    return TravelProgress::ContinueWalk(
        travel->progress, [&] { return botAI->GetTravel().ReadTravelWalk(); },
        [&] { return MoveFarToImpl(*travel, dest); },
        [&](char const* reason) { botAI->GetTravel().TraceTravelInteraction("route_resume", reason); });
}

bool NewRpgTravelAction::MoveFarToImpl(HeldTrip& travel, WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    // The taxi or transport still owns movement. Consume the tick while the bot is aboard.
    if (bot->IsInFlight() || bot->GetTransport() || bot->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
        return true;

    // A route follower's waypoints are graph nodes: the route search avoids stuck edges itself (StuckBook), so no
    // avoided-spot memo is read here (a status's refusal is GoTo's step 2)
    bool const newDest = dest != travel.walkTo;
    if (newDest)
        travel.walkTo = dest;

    // performance optimization
    if (!newDest && IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    // Let previously committed movement finish before recomputing.
    //
    // MoveTo internally caps its stored delay at maxWaitForMove (default 5s), but a long path (200+ yd routed around a
    // mountain) takes 30+ seconds to walk. After 5s IsWaitingForLastMove returns false and MoveFarTo re-enters. Without
    // this gate, DoMovePoint would call mm->Clear() and reissue MovePoint from the new bot position — and from a new
    // position mmap's partial-path endpoint often differs, so the bot gets clobbered mid-walk and ends up oscillating
    // (e.g. cave entrance -> inside cave -> cave entrance -> mountain base -> cave entrance...) around an unreachable
    // destination.
    //
    // If the bot is still actively walking toward its last committed point on the same map, just let the current spline
    // finish. The travel observer samples owned splines even while action delay or this gate prevents reissuing a move.
    //
    // A changed destination skips this gate: letting the old spline finish would keep walking the bot toward a target
    // it no longer has.
    //
    // A finalized move spline is no move to wait for, whatever the movement flags say: they can stay "moving" after the
    // spline ended (WU11b fix 13: Throke in the air, moving 71 s after its last move), and the gate held such a bot
    // forever.
    if (!newDest)
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && !bot->movespline->Finalized() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    float const disToDest = bot->GetDistance(dest);
    if (travel.progress.ShouldProbeOffMesh() && BotOffMesh())
    {
        travel.progress.Latch(TravelProgress::Overdue::OffMesh);
        return true;
    }

    // WT58: along a graph link each order is judged on the engine's answer before the move (TravelWalkRules.h); near
    // dest MovePoint would walk a "no path" answer as a straight line. Not taken: the follower's next tick orders the
    // far node (fallback) or fails the leg (refused). A refusal where the bot stands off the mesh goes to the off-mesh
    // recovery instead, as the walk's own probe does
    TravelWalk::LinkWalk* const walk = travel.trip.walk.Active() ? &travel.trip.walk : nullptr;
    // WT71: the moves ordered below are counted on their path's answer; a walk's hop is counted in the link's hops
    bool hopCounted = false;
    MeshMove::Kind const meshKind = walk ? MeshMove::Kind::TravelWalk : MeshMove::Kind::Other;
    auto const taken = [&](PathGenerator const& path, bool result, bool direct)
    {
        TravelWalk::Verdict const verdict = TravelWalk::Judge(*walk, ReadWalkAnswer(bot, path, result, dest), direct);
        hopCounted = verdict == TravelWalk::Verdict::Hop;
        if (verdict == TravelWalk::Verdict::Refuse && BotOffMesh())
        {
            walk->mode = TravelWalk::Mode::Fallback;
            travel.progress.Latch(TravelProgress::Overdue::OffMesh);
        }
        return verdict == TravelWalk::Verdict::Walk || verdict == TravelWalk::Verdict::Hop;
    };

    // The trace of a failed act (WT64b diagnosis: a follower that stands while its order fails each tick and the leg
    // waits for its clock): which order failed and the engine's answer, at debug level, one line per
    // WALK_FAIL_TRACE_MS. It goes with this walk at WT64g
    auto const ordered = [&](bool moved, char const* how, uint32 type, float endYards)
    {
        if (!moved && (!travel.walkFailMs || GetMSTimeDiffToNow(travel.walkFailMs) >= WALK_FAIL_TRACE_MS))
        {
            travel.walkFailMs = std::max<uint32>(getMSTime(), 1);
            LOG_DEBUG("playerbots",
                      "[Route] {} walk act failed: {} to ({:.1f}, {:.1f}, {:.1f}) {:.0f} yd away, {}, type 0x{:x}, end "
                      "{:.0f} yd from it, moving {}, can move {}",
                      bot->GetName(), how, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), disToDest,
                      walk ? "a graph link" : "off the graph", type, endYards, bot->isMoving(), botAI->CanMove());
        }
        return moved;
    };

    float dis = bot->GetExactDist(dest);
    if (dis < pathFinderDis)
    {
        if (walk)
        {
            PathGenerator path(bot);
            bool const result = path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
            if (!taken(path, result, true))
                return true;
            return ordered(MoveCounted(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
                                       meshKind, path, hopCounted),
                           "the judged order to the place", path.GetPathType(), 0.0f);
        }
        return ordered(MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false,
                              false, false, true),
                       "the order to the place", 0, 0.0f);
    }

    const uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
    uint32 primaryType = 0;  // the trace's: the answer to the true destination and where its path ends
    float primaryEnd = 0.0f;

    // Primary strategy: ask mmap for a route to the TRUE destination. If mmap can reach it directly (PATHFIND_NORMAL)
    // or partially (PATHFIND_INCOMPLETE — destinations beyond the smooth-path cap of ~296 yards, or where local
    // geometry blocks the final step), walk to the furthest reachable waypoint mmap computed. This lets bots follow the
    // real route around obstacles (mountains, cave walls, cliffs) instead of trying to cut straight through. The spline
    // system walks the whole returned path smoothly, so subsequent ticks early-out via IsWaitingForLastMove and no
    // further PathGenerator calls fire until the bot arrives.
    {
        PathGenerator path(bot);
        bool const result = path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        if (walk && !taken(path, result, false))
            return true;
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));
        primaryType = type;
        if (canReach)
        {
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            // Only commit if the mmap endpoint actually makes progress toward the destination. For pathological
            // INCOMPLETE results (e.g. disconnected polys that still report INCOMPLETE) the endpoint can land right
            // under the bot; fall through to cone sampling in that case.
            float endDistToDest = dest.GetExactDist(endPos.x, endPos.y, endPos.z);
            primaryEnd = endDistToDest;
            if (endDistToDest + 5.0f < disToDest)
            {
                return ordered(MoveCounted(bot->GetMapId(), endPos.x, endPos.y, endPos.z, meshKind, path, hopCounted),
                               "the order to the path's end", type, endDistToDest);
            }
        }
    }

    // Fallback: mmap couldn't route to the destination. Sample the forward cone for a reachable stepping stone so the
    // bot keeps moving and can try again from a new vantage point. Cap at 2 samples — we already spent one
    // PathGenerator call above and at 3000 bots every extra CalculatePath matters.
    float minDelta = M_PI;
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    const float baseAngle = bot->GetAngle(&dest);
    float rx, ry, rz;
    bool found = false;
    uint32 foundType = 0;
    float foundLastStep = 0.0f;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        float delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);  // ±π/2, forward cone
        float sampleDis = (0.5f + rand_norm() * 0.5f) * pathFinderDis;
        float angle = baseAngle + delta;
        float dx = x + cos(angle) * sampleDis;
        float dy = y + sin(angle) * sampleDis;
        float dz = z + 0.5f;
        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));

        if (canReach && fabs(delta) <= minDelta)
        {
            found = true;
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            rx = endPos.x;
            ry = endPos.y;
            rz = endPos.z;
            foundType = type;
            foundLastStep = MeshMove::LastStep(path.GetPath());
            minDelta = fabs(delta);
        }
    }
    if (found)
    {
        if (walk)
            TravelWalk::NoteMove(*walk);
        return ordered(MoveCounted(bot->GetMapId(), rx, ry, rz, meshKind, foundType, foundLastStep),
                       "the order to a sampled point", foundType, 0.0f);
    }
    return ordered(false, "no nearer path end and no sampled point", primaryType, primaryEnd);
}
