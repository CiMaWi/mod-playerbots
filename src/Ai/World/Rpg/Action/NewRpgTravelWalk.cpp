/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <array>
#include <chrono>
#include <limits>
#include <string>

#include "BotTravel.h"
#include "DetourNavMeshQuery.h"
#include "GoToRules.h"
#include "HeldTrip.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "Map.h"
#include "MapDefines.h"
#include "MeshMoveRules.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NewRpgAction.h"
#include "NewRpgInternal.h"
#include "PathGenerator.h"
#include "PerfMonitor.h"
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
                  TravelWalk::PATH_FARFROMPOLY_END == PATHFIND_FARFROMPOLY_END &&
                  TravelWalk::PATH_FARFROMPOLY_START == PATHFIND_FARFROMPOLY_START,
              "TravelWalk reads the engine's PathType bits");
static_assert(TravelWalk::SEARCH_OUT_OF_NODES == DT_OUT_OF_NODES && TravelWalk::SEARCH_PARTIAL == DT_PARTIAL_RESULT,
              "TravelWalk reads Detour's status details");

// A walk probe slower than this writes its place to the debug log (pathing PA4: the place of the worst probe)
constexpr int64 SLOW_PROBE_MICROSECONDS = 5000;

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

// MeshJoinWord's filter: ground and water, steep ground excluded or included
dtQueryFilter MeshJoinFilter(bool steep)
{
    dtQueryFilter filter;
    filter.setIncludeFlags(NAV_GROUND | NAV_WATER | (steep ? NAV_GROUND_STEEP : 0));
    filter.setExcludeFlags(NAV_MAGMA | NAV_SLIME | (steep ? 0 : NAV_GROUND_STEEP));
    return filter;
}

// The polygon at a Detour point (y, z, x) in the engine's two search boxes (PathGenerator::GetPolyByLocation), 0 for
// none
dtPolyRef PolyAt(dtNavMeshQuery const* query, dtQueryFilter const& filter, float const* point)
{
    dtPolyRef ref = 0;
    float nearest[3] = {0.0f, 0.0f, 0.0f};
    for (float const up : {5.0f, 50.0f})
    {
        float const extents[3] = {3.0f, up, 3.0f};
        if (dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) && ref)
            break;
    }
    return ref;
}

// A refused order's verdict aid (WT64g-2, WT.56; debug log only, on the bot's map thread): does the navmesh join the
// bot's polygon and the place's at all? One search of its own with Detour's largest node pool, under the bots' filter
// (ground and water, no steep ground: PathGenerator::CreateFilter) and, where that finds no way, with steep ground.
// "joins" means the mesh has the walk and the engine's answer fell short of it (its own search limit, a smooth path
// that stopped); "does not join" means the mesh lacks it (data); "joins over steep ground" names the bots' filter.
// The search is the map's wide query (pathing PA4: one per map, made once), no query of its own
std::string MeshJoinWord(Player* bot, WorldPosition const& dest)
{
    dtNavMeshQuery const* const query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetWideNavMeshQuery();
    if (!query)
        return "no mesh";
    float const from[3] = {bot->GetPositionY(), bot->GetPositionZ(), bot->GetPositionX()};
    float const to[3] = {dest.GetPositionY(), dest.GetPositionZ(), dest.GetPositionX()};
    std::string word;
    for (bool const steep : {false, true})
    {
        dtQueryFilter const filter = MeshJoinFilter(steep);
        dtPolyRef const start = PolyAt(query, filter, from);
        dtPolyRef const end = PolyAt(query, filter, to);
        if (!start || !end)
        {
            word = !start ? "has no polygon at the bot" : "has no polygon at the place";
            continue;
        }
        std::array<dtPolyRef, 2048> polys;
        int count = 0;
        dtStatus const status =
            query->findPath(start, end, from, to, &filter, polys.data(), &count, static_cast<int>(polys.size()));
        if (dtStatusSucceed(status) && !dtStatusDetail(status, DT_PARTIAL_RESULT))
        {
            word = steep ? "joins them over steep ground only" : "joins them";
            break;
        }
        word = dtStatusDetail(status, DT_OUT_OF_NODES) ? "search ran out of nodes" : "does not join them";
    }
    return word;
}

// The piece a refused order was on (WT64i trace, debug log only): on a probed walk off the graph, the probed point it
// ordered (PieceTarget's index, from 1) or the walk's end, so that the probe's step ends can be held against it
std::string OrderWord(TravelLegs::Trip const& trip)
{
    if (trip.walk.Active())
        return "a graph link";
    TravelWalk::PieceWalk const& piece = trip.piece;
    if (!piece.For(trip.legIdx, trip.next))
        return "off the graph";
    if (piece.steps == 0)
        return "off the graph, unprobed";
    std::size_t const count = piece.orders.size();
    return piece.index < count ? Acore::StringFormat("off the graph, probed point {} of {} ({} steps)", piece.index + 1,
                                                     count, piece.steps)
                               : Acore::StringFormat("off the graph, the walk's end after {} probed points ({} steps)",
                                                     count, piece.steps);
}

// The yards a running spline has left along its own points (from the unit to the next point, then point to point): a
// committed walk is judged by the path it walks, not by the straight line to its end (PA69, G332)
float SplineYardsLeft(Unit const& unit)
{
    Movement::MoveSpline const& spline = *unit.movespline;
    auto const& points = spline.getPath();
    int32 const index = spline.currentPathIdx();
    if (index < 0)
        return 0.0f;
    G3D::Vector3 at(unit.GetPositionX(), unit.GetPositionY(), unit.GetPositionZ());
    float yards = 0.0f;
    for (std::size_t next = static_cast<std::size_t>(index) + 1; next < points.size(); ++next)
    {
        yards += (points[next] - at).length();
        at = points[next];
    }
    return yards;
}
}  // namespace

// One path call as a probe step reads it: the call's result, the path's type and its points (forceDest off, as the
// follower's order asks it), searched with the map's wide query, whose node pool spans a walk off the graph (pathing
// PA4: a search of 1,024 nodes ran out on long walks the mesh joins), and why it fell short (SearchWhy; which end
// lacks a polygon is asked only when the engine answered its straight line for one). An approach's judge asks the same
// call (pathing PA5), wide where TravelWalk::ApproachAsksWide says so
TravelWalk::StepAnswer NewRpgInternal::AskTravelPath(Player* bot, TravelWalk::ProbePoint const& start,
                                                     TravelWalk::ProbePoint const& end, bool straight, bool wide)
{
    PathGenerator path(bot);
    path.SetUseStraightPath(straight);
    if (wide)
        path.SetWideSearch(true);
    TravelWalk::StepAnswer answer;
    answer.result = path.CalculatePath(start.x, start.y, start.z, end.x, end.y, end.z, false);
    answer.type = path.GetPathType();
    for (G3D::Vector3 const& point : path.GetPath())
        answer.points.push_back({point.x, point.y, point.z});
    bool startPoly = true;
    if (answer.type & TravelWalk::PATH_NOT_USING_PATH)
    {
        dtNavMeshQuery const* const query = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
        float const point[3] = {start.y, start.z, start.x};
        startPoly = !query || PolyAt(query, MeshJoinFilter(false), point);
    }
    answer.why = TravelWalk::SearchWhy(answer.type, path.GetSearchStatus(), startPoly);
    return answer;
}

// The probe's steps ask the order's own call (TravelWalk::OrderStep, WT64i fix round): the smooth path MoveFarToImpl's
// CalculatePath asks, aimed by a straight path's corners within half the smooth path's point limit (its points lie
// SMOOTH_PATH_STEP_SIZE apart; a longer path fails as the one line). Each step's judged answer goes to the debug log
// (WT64i trace): its end, the judge's verdict on it as the order to the point asked, and that corner when it is not
// the walk's end
TravelWalk::PieceWalk NewRpgInternal::ProbeWalkPiece(Player* bot, TravelRoute::Point const& from,
                                                     TravelRoute::Point const& to, float arriveYards, bool fragment,
                                                     bool tilesLoaded)
{
    uint32 calls = 0;
    auto const straight = [bot](TravelWalk::ProbePoint const& start, TravelWalk::ProbePoint const& end)
    { return NewRpgInternal::AskTravelPath(bot, start, end, true, true); };
    auto const smooth = [bot](TravelWalk::ProbePoint const& start, TravelWalk::ProbePoint const& aim)
    { return NewRpgInternal::AskTravelPath(bot, start, aim, false, true); };
    float const aimYards = MAX_POINT_PATH_LENGTH * SMOOTH_PATH_STEP_SIZE / 2.0f;
    auto const step = [&](TravelWalk::ProbePoint const& start, TravelWalk::ProbePoint const& end)
    {
        // A StepAnswer, so that ProbePiece reads the step's reason (a ProbeAnswer would drop it: every refusal "none")
        TravelWalk::StepAnswer const answer = TravelWalk::OrderStep(start, end, straight, smooth, aimYards);
        ++calls;
        TravelWalk::ProbePoint const last = answer.points.empty() ? start : answer.points.back();
        TravelWalk::ProbePoint const aimed = answer.aim.value_or(end);
        std::string const aim =
            TravelWalk::Distance(aimed, end) > 0.1f
                ? Acore::StringFormat(", aimed at the corner ({:.1f}, {:.1f}, {:.1f})", aimed.x, aimed.y, aimed.z)
                : std::string();
        LOG_DEBUG("playerbots",
                  "[Route] {} probe step {} from ({:.1f}, {:.1f}, {:.1f}): type 0x{:x}, {} points, last step {:.1f} "
                  "yd, {}; end ({:.1f}, {:.1f}, {:.1f}) {:.1f} yd from the walk's end ({:.1f}, {:.1f}, {:.1f}){}",
                  bot->GetName(), calls, start.x, start.y, start.z, answer.type, answer.points.size(),
                  MeshMove::LastStep(answer.points),
                  ::GoTo::ProbeCarried(answer, start, aimed) ? "carried" : "not carried", last.x, last.y, last.z,
                  TravelWalk::Distance(last, end), end.x, end.y, end.z, aim);
        return answer;
    };
    // The walk probe as it is (PA3): count and time of the probes actually run (a fragment end or an unloaded tile
    // calls no path), so a change to the probe's pool is one row in two starts.
    PerfMonitorScope scope(!fragment && tilesLoaded && PerfMonitor::IsEnabled()
                               ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Travel::WalkProbe")
                               : nullptr);
    // A refused probe's reason and a slow probe's place (pathing PA4; debug log only: one clock pair per probe)
    bool const trace = !fragment && tilesLoaded && sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG);
    auto const started = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    TravelWalk::PieceWalk piece =
        TravelWalk::StartPiece(fragment, tilesLoaded, from, to, arriveYards, step, ::GoTo::ProbeCarried);
    if (trace)
    {
        int64 const micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
        if (piece.Refused() || micros >= SLOW_PROBE_MICROSECONDS)
            LOG_DEBUG(
                "playerbots",
                "[Route] {} walk probe on map {} from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f}): {} {} "
                "after {} steps, {} us",
                bot->GetName(), bot->GetMapId(), from.x, from.y, from.z, to.x, to.y, to.z,
                TravelWalk::ProbeWord(piece.probe), TravelWalk::WhyWord(piece.why), piece.steps, micros);
    }
    return piece;
}

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
            // Near its end in a straight line, the walk may still be long on its path: a dock point 2 yd off that the
            // mesh joins only through a long corridor was ordered again from each new spot, and the bot oscillated
            // on its first yards (PA69, G332: Bouldercrag's lift 149045, 133-point paths). The path left decides
            if (remaining <= 10.0f)
                remaining = SplineYardsLeft(*bot);
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
    // A walk off the graph (a probed or a direct walk) is ordered with the wide query its probe searched with (pathing
    // PA4); a graph link keeps the engine's 1,024 nodes, which its judge reads
    path.SetWideSearch(!walk);
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
    if (!carried || TravelWalk::StillOrder(travel.walkStill, !newDest, moved, yards))
    {
        LOG_DEBUG("playerbots",
                  "[Route] {} walk order to ({:.1f}, {:.1f}, {:.1f}) {:.0f} yd refused, {}: type 0x{:x}, last step "
                  "{:.1f} yd, end ({:.1f}, {:.1f}, {:.1f}) {:.1f} yd from it, {}; from ({:.1f}, {:.1f}, {:.1f}), {} "
                  "path points, the mesh {}",
                  bot->GetName(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), yards,
                  OrderWord(travel.trip), type, lastStep, end.x, end.y, end.z, endYards,
                  carried ? "it moved nothing" : "not carried", bot->GetPositionX(), bot->GetPositionY(),
                  bot->GetPositionZ(), path.GetPath().size(), MeshJoinWord(bot, dest));
        if (walk)
            walk->mode = TravelWalk::Mode::Refused;
        travel.walkRefused = walk ? TravelLegs::FailReason::NoPath : TravelLegs::FailReason::NoWalk;
        return true;
    }
    if (!MoveCounted(bot->GetMapId(), end.x, end.y, end.z, MeshMove::Kind::TravelWalk, type, lastStep, hopTaken))
        return false;
    // WT64i traces (debug log only, no fix): an order taken again without motion (a still strike; the next such order
    // is refused) with the spline it left, and an order beyond the local range taken on its gain alone (LEG_GAIN_YARDS:
    // its end out of reach of dest); the bot's next order or its leg's end says how each ended
    if (travel.walkStill > 0 || (yards > ::GoTo::LOCAL_RANGE_YARDS && endYards > TravelWalk::REACH_YARDS))
        LOG_DEBUG("playerbots",
                  "[Route] {} walk order to ({:.1f}, {:.1f}, {:.1f}) {:.0f} yd taken, {}: type 0x{:x}, last step "
                  "{:.1f} yd, end ({:.1f}, {:.1f}, {:.1f}) {:.1f} yd from it, {:.1f} yd from the last order's start "
                  "(strike {}); spline {} {} ms {} segments, generator {}, {}",
                  bot->GetName(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), yards,
                  travel.walkStill > 0 ? "still" : "on its gain", type, lastStep, end.x, end.y, end.z, endYards,
                  moved == std::numeric_limits<float>::max() ? -1.0f : moved, travel.walkStill,
                  bot->movespline->Finalized() ? "finalized" : "live", bot->movespline->Duration(),
                  bot->movespline->Initialized() ? bot->movespline->MaxPathIdx() + 1 : 0,
                  static_cast<int>(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()), OrderWord(travel.trip));
    travel.walkOrderAt = WorldPosition(bot);
    return true;
}
