/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <limits>

#include "BotTravel.h"
#include "GoToRules.h"
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
#include "TravelLegRules.h"
#include "TravelMgr.h"
#include "TravelProgress.h"
#include "TravelWalkRules.h"

// The walk of the route follower's legs (WT64b, WT64g), for a held trip only. No status calls it: a status walks by
// GoTo (NewRpgGoTo.cpp), and its walk beyond a local piece is a trip whose legs are walked here, one judged order at a
// time (GoTo::LegCarried). A refused order fails the leg; no stepping stone, no straight line

namespace
{
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
    bool const moved = !TravelLegs::HasWalkTarget(travel->trip, travel->bindInn)
                           ? MoveFarToImpl(*travel, dest)
                           : TravelProgress::ContinueWalk(
                                 travel->progress, [&] { return botAI->GetTravel().ReadTravelWalk(); },
                                 [&] { return MoveFarToImpl(*travel, dest); }, [&](char const* reason)
                                 { botAI->GetTravel().TraceTravelInteraction("route_resume", reason); });
    TravelLegs::FailReason const refused = travel->walkRefused;
    if (refused == TravelLegs::FailReason::None)
        return moved;
    // A refused order is the leg's typed end (WT64g): the replan's, never another move. After the last leg (the inn's
    // last yards) the walk is stuck out of reach
    travel->walkRefused = TravelLegs::FailReason::None;
    if (travel->trip.legIdx < travel->trip.legs.size())
        return LegFailed(*travel, refused);
    return Arrive(*travel, true);
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
    {
        travel.walkTo = dest;
        travel.walkOrderAt = WorldPosition();
        travel.walkStill = 0;
    }

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

    float const yards = bot->GetExactDist(dest);
    if (travel.progress.ShouldProbeOffMesh() && BotOffMesh())
    {
        travel.progress.Latch(TravelProgress::Overdue::OffMesh);
        return true;
    }

    // One judged order (WT64g; PLAN 3.20 (1), site 11): the path to dest, its answer judged before the move. Along a
    // graph link TravelWalk::Judge reads it first (WT58: a stored point off the mesh or without progress falls back to
    // the far node at the next tick; no path refuses the link, or, where the bot stands off the mesh, goes to the
    // off-mesh recovery). Then GoTo::LegCarried: the order is the carried path's end within reach of dest (beyond the
    // local range a carried end that gains on it), counted as kind travel_walk. A refused order, or an accepted one
    // that moved nothing (TravelWalk::StillOrder), is the leg's typed end (MoveFarTo); no other move is tried
    TravelWalk::LinkWalk* const walk = travel.trip.walk.Active() ? &travel.trip.walk : nullptr;
    PathGenerator path(bot);
    bool const result = path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
    bool hopTaken = false;
    if (walk)
    {
        TravelWalk::Verdict const verdict = TravelWalk::Judge(*walk, ReadWalkAnswer(bot, path, result, dest));
        if (verdict == TravelWalk::Verdict::Fallback)
            return true;
        if (verdict == TravelWalk::Verdict::Refuse && BotOffMesh())
        {
            walk->mode = TravelWalk::Mode::Fallback;
            travel.progress.Latch(TravelProgress::Overdue::OffMesh);
            return true;
        }
        hopTaken = verdict == TravelWalk::Verdict::Hop;
    }
    G3D::Vector3 const& end = path.GetActualEndPosition();
    uint32 const type = path.GetPathType();
    float const lastStep = MeshMove::LastStep(path.GetPath());
    float const endYards = dest.GetExactDist(end.x, end.y, end.z);
    bool const carried = result && (!walk || walk->mode != TravelWalk::Mode::Refused) &&
                         ::GoTo::LegCarried(type, lastStep, hopTaken, endYards, yards, TravelWalk::REACH_YARDS);
    float const moved = travel.walkOrderAt == WorldPosition() ? std::numeric_limits<float>::max()
                                                              : bot->GetExactDist(travel.walkOrderAt);
    if (!carried || TravelWalk::StillOrder(travel.walkStill, !newDest, moved))
    {
        LOG_DEBUG("playerbots",
                  "[Route] {} walk order to ({:.1f}, {:.1f}, {:.1f}) {:.0f} yd refused, {}: type 0x{:x}, last step "
                  "{:.1f} yd, end {:.1f} yd from it, {}",
                  bot->GetName(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), yards,
                  walk ? "a graph link" : "off the graph", type, lastStep, endYards,
                  carried ? "it moved nothing" : "not carried");
        if (walk)
            walk->mode = TravelWalk::Mode::Refused;
        travel.walkRefused = walk ? TravelLegs::FailReason::NoPath : TravelLegs::FailReason::NoWalk;
        return true;
    }
    if (!MoveCounted(bot->GetMapId(), end.x, end.y, end.z, MeshMove::Kind::TravelWalk, type, lastStep, hopTaken))
        return false;
    travel.walkOrderAt = WorldPosition(bot);
    return true;
}
