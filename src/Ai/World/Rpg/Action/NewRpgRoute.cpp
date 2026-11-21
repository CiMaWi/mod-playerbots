/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "AreaDefines.h"
#include "BankRulesInput.h"
#include "BankVisit.h"
#include "BotTraitMgr.h"
#include "BotTravel.h"
#include "BroadcastHelper.h"
#include "CellImpl.h"
#include "ChatHelper.h"
#include "CheckMountStateAction.h"
#include "DBCStores.h"
#include "ErrandRules.h"
#include "EventLog.h"
#include "G3D/Vector2.h"
#include "GameObjectModel.h"
#include "GameTime.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "IVMapMgr.h"
#include "InventorySlots.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "MailVisit.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "MountRules.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "NewRpgAction.h"
#include "NewRpgErrandInternal.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "NewRpgStrategy.h"
#include "NewRpgTriggers.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestIndex.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "RouteService.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "StartingHistoryRules.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "Trainer.h"
#include "Transport.h"
#include "TravelGraph.h"
#include "TravelLegRules.h"
#include "TravelMgr.h"
#include "TravelProgress.h"
#include "TravelTransportGeometry.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "WaypointMovementGenerator.h"
#include "World.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldTravelRules.h"

using namespace NewRpgInternal;

namespace
{
constexpr uint32 HEARTHSTONE_SPELL = 8690;
}  // namespace

namespace NewRpgInternal
{
// A route exists only once the graph is ready (the route service answers only then), and it never changes after
TravelRoute::Point GraphNodeAt(std::uint32_t index)
{
    TravelGraphNode const& node = TravelGraph::Instance().GetNode(index);
    return {node.mapId, node.x, node.y, node.z};
}
}  // namespace NewRpgInternal

namespace
{

// WT58: the graph link a walk leg walks now, between waypoints[next - 1] and waypoints[next]; none on the walks
// between the bot, the graph and the goal, and on every other leg
std::pair<std::uint32_t, std::uint32_t> WalkLinkEnds(TravelLegs::Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != TravelRoute::LegType::Walk)
        return {TravelLegs::NO_NODE, TravelLegs::NO_NODE};
    return TravelLegs::StuckEdge(trip);
}

// The links a walk leg walks along: walk links only (a wait point's link to its stop is part of its ride, WT.56)
bool WalkableLink(TravelGraphLink const& link) { return link.type == TravelLinkType::Walk; }

// A walk leg's order along its graph link (TravelWalkRules.h): the link's next stored point, else target (the far
// node, or a waypoint off the graph). The points stay in the graph; the trip holds an index
TravelRoute::Point WalkLinkTarget(TravelLegs::Trip& trip, TravelRoute::Point const& bot,
                                  TravelRoute::Point const& target)
{
    TravelWalk::LinkWalk& walk = trip.walk;
    auto const [from, to] = WalkLinkEnds(trip);
    if (from == TravelLegs::NO_NODE)
    {
        walk = {};
        return target;
    }
    TravelGraph const& graph = TravelGraph::Instance();
    if (!walk.On(from, to))
    {
        TravelWalk::Rows<TravelGraphLink> const rows = TravelWalk::PickRows<TravelGraphLink>(
            [&graph](std::uint32_t node) { return graph.LinksOf(node); }, from, to, WalkableLink);
        std::span<TravelGraphPoint const> const points =
            rows.link ? graph.PathOf(*rows.link) : std::span<TravelGraphPoint const>{};
        TravelWalk::Begin(walk, from, to, rows.link ? graph.IndexOfLink(*rows.link) : TravelWalk::NO_LINK, points,
                          rows.reversed, graph.GetNode(from), graph.GetNode(to));
    }
    if (walk.mode != TravelWalk::Mode::Stored)
        return target;
    std::span<TravelGraphPoint const> const points = graph.PathOf(graph.GetLink(walk.link));
    std::uint32_t const point = TravelWalk::Target(walk, points, bot, graph.GetNode(to));
    if (point == TravelWalk::NO_POINT)
        return target;
    TravelGraphPoint const& at = TravelWalk::PointAt(points, walk.reversed, point);
    return {at.mapId, at.x, at.y, at.z};
}

static_assert(TravelWalk::PATH_NORMAL == PATHFIND_NORMAL && TravelWalk::PATH_INCOMPLETE == PATHFIND_INCOMPLETE &&
                  TravelWalk::PATH_NOPATH == PATHFIND_NOPATH &&
                  TravelWalk::PATH_NOT_USING_PATH == PATHFIND_NOT_USING_PATH &&
                  TravelWalk::PATH_SHORT == PATHFIND_SHORT,
              "TravelWalk::Accepts reads the engine's PathType bits");

// The navmesh tile under a point is loaded (PathGenerator::HaveTile): the engine loads a grid's tiles with the grid,
// and a path call to a point on an unloaded tile answers the straight line whatever the ground
bool NavTileLoaded(Player* bot, TravelRoute::Point const& at)
{
    dtNavMesh const* const mesh = bot->GetMap()->GetMapCollisionData().GetMMapData().GetNavMesh();
    if (!mesh)
        return false;
    int tx = -1;
    int ty = -1;
    float const point[3] = {at.y, at.z, at.x};
    mesh->calcTileLoc(point, &tx, &ty);
    return tx >= 0 && ty >= 0 && mesh->getTileAt(tx, ty, 0) != nullptr;
}

// The polygons a navmesh fragment has at most (NavFragment): a cheap bound on the flood, once per walk off the graph
constexpr std::size_t FRAGMENT_POLYS = 16;

// A point on a navmesh fragment: the nearest polygon a bot may walk (NavMeshSnap's filter, the off-mesh box) belongs to
// a connected piece of fewer than FRAGMENT_POLYS polygons, linked to nothing else. The extractor leaves such pieces
// where it cut walkable floor apart (the polygons of a hall floor with no neighbours, WT56): the bots walk there, but
// no path leaves such a piece, so the navmesh cannot judge a walk from or to it. False where no polygon is in the box
// (the bot is off the mesh: the follower's off-mesh recovery) or the map has no navmesh
bool NavFragment(Player* bot, TravelRoute::Point const& at)
{
    MMapData& mmap = bot->GetMap()->GetMapCollisionData().GetMMapData();
    dtNavMesh const* const mesh = mmap.GetNavMesh();
    dtNavMeshQuery const* const query = mmap.GetNavMeshQuery();
    if (!mesh || !query)
        return false;
    dtQueryFilter filter;
    filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
    filter.setExcludeFlags(TravelWalk::BOT_EXCLUDE_FLAGS);
    float const point[3] = {at.y, at.z, at.x};
    float const extents[3] = {WorldTravel::OFF_MESH_ACROSS_YARDS, WorldTravel::OFF_MESH_VERTICAL_YARDS,
                              WorldTravel::OFF_MESH_ACROSS_YARDS};
    dtPolyRef start = 0;
    float nearest[3] = {0.0f, 0.0f, 0.0f};
    if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &start, nearest)) || !start)
        return false;
    std::array<dtPolyRef, FRAGMENT_POLYS> piece{start};
    std::size_t count = 1;
    for (std::size_t i = 0; i < count; ++i)
    {
        dtMeshTile const* tile = nullptr;
        dtPoly const* poly = nullptr;
        mesh->getTileAndPolyByRefUnsafe(piece[i], &tile, &poly);
        for (unsigned int link = poly->firstLink; link != DT_NULL_LINK; link = tile->links[link].next)
        {
            dtPolyRef const neighbour = tile->links[link].ref;
            if (!neighbour || std::find(piece.begin(), piece.begin() + count, neighbour) != piece.begin() + count)
                continue;
            dtMeshTile const* nextTile = nullptr;
            dtPoly const* nextPoly = nullptr;
            mesh->getTileAndPolyByRefUnsafe(neighbour, &nextTile, &nextPoly);
            if (!filter.passFilter(neighbour, nextTile, nextPoly))
                continue;
            if (count == FRAGMENT_POLYS)
                return false;
            piece[count++] = neighbour;
        }
    }
    return true;
}

// WT56: a walk leg's walk off the graph (the bot's start to its first node, the last node to the goal, a direct walk)
// is probed on the navmesh once, from where the bot stands when it starts it (NewRpgInternal::ProbeWalkPiece: the
// chain of mod-playerbots PR #2747, each step the order's own smooth path, judged as the order). A walk to a point
// on an unloaded tile, or on a map without a navmesh, is not probed (the follower's judged order to the waypoint
// itself); one with an end on a navmesh fragment (NavFragment), which no path leaves, is refused (WT64i, WT.59).
// Walked: target becomes the probed point to order next. False: the navmesh refused it. tripId: the trip's id
bool WalkPieceTarget(Player* bot, TravelLegs::Trip& trip, uint32 tripId, TravelRoute::Point const& at,
                     TravelRoute::Point& target)
{
    if (trip.walk.Active() || trip.legIdx >= trip.legs.size() ||
        trip.legs[trip.legIdx].type != TravelRoute::LegType::Walk || trip.next == 0 ||
        trip.next >= trip.waypoints.size())
        return true;
    TravelLegs::Waypoint const& from = trip.waypoints[trip.next - 1];
    TravelLegs::Waypoint const& to = trip.waypoints[trip.next];
    if ((from.node != TravelLegs::NO_NODE && to.node != TravelLegs::NO_NODE) || to.at.mapId != bot->GetMapId() ||
        TravelRoute::Distance(target, to.at) > TravelLegs::WAYPOINT_YARDS)
        return true;  // a graph link (WalkLinkTarget), another map, or an order that is not the waypoint (a quay)
    TravelWalk::PieceWalk& piece = trip.piece;
    if (!piece.For(trip.legIdx, trip.next))
    {
        bool const fragment =
            (NavTileLoaded(bot, at) && NavFragment(bot, at)) || (NavTileLoaded(bot, to.at) && NavFragment(bot, to.at));
        piece = NewRpgInternal::ProbeWalkPiece(bot, at, to.at, TravelLegs::WaypointReach(trip, trip.next), fragment,
                                               NavTileLoaded(bot, at) && NavTileLoaded(bot, to.at));
        // Not probed: a trip row says why (WT70g, the night checks count such walks and how their trips end); no
        // points, the waypoint itself
        if (piece.probe == TravelWalk::Probe::None)
        {
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "walk_unprobed", "no_tile",
                               TravelRoute::Distance(at, to.at), {{"leg", static_cast<float>(trip.legIdx + 1)}},
                               tripId);
            piece.probe = TravelWalk::Probe::Walk;
        }
        piece.leg = trip.legIdx;
        piece.next = trip.next;
        if (piece.Refused())
            LOG_INFO("playerbots",
                     "[Route] {} walk leg {}/{} off the graph from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, "
                     "{:.1f}) refused by the navmesh: {} after {} probe steps",
                     bot->GetName(), trip.legIdx + 1, trip.legs.size(), at.x, at.y, at.z, to.at.x, to.at.y, to.at.z,
                     TravelWalk::ProbeWord(piece.probe), piece.steps);
    }
    if (piece.Refused())
        return false;
    std::uint32_t const point = TravelWalk::PieceTarget(piece, at, to.at);
    if (point != TravelWalk::NO_POINT)
        target = {target.mapId, piece.orders[point].x, piece.orders[point].y, piece.orders[point].z};
    return true;
}

// A walk leg's end (WT.23): its trip decision row, walk_leg when done or walk_leg_failed (how the bot went: foot, part
// or mounted; its seconds in combat; its seconds against the estimate), and in the debug log its speed: its path
// through the waypoints, its straight line, the time taken
void LogWalkLeg(Player* bot, HeldTrip const& data, uint32 index, TravelLegs::LegRecord const& record, uint32 nowMs,
                uint32 elapsedMs, char const* how, bool done)
{
    TravelRoute::Leg const& leg = data.trip.legs[index];
    float const seconds = static_cast<float>(elapsedMs) / 1000.0f;
    float const combat = TravelLegs::CombatSeconds(record, nowMs);
    WorldLog::Decision(
        bot, WorldLogFormat::DecisionKind::Trip, done ? "walk_leg" : "walk_leg_failed", TravelLegs::MountWord(record),
        combat, {{"seconds", seconds}, {"estimate", leg.seconds}, {"leg", static_cast<float>(index + 1)}}, data.tripId);
    if (!sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    std::vector<TravelLegs::Waypoint> const points = TravelLegs::Waypoints(leg, GraphNodeAt);
    float path = 0.0f;
    for (size_t i = 1; i < points.size(); ++i)
        path += TravelRoute::Distance(points[i - 1].at, points[i].at);
    TravelRoute::Point const at{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    LOG_DEBUG("playerbots",
              "[Route] {} walk leg {}/{} {}: {:.0f} yd straight, {:.0f} yd path in {:.0f} s ({:.1f} yd/s), estimate "
              "{:.0f} s, {:.0f} yd from its end, {} ({} of {} ticks mounted), {:.0f} s in combat",
              bot->GetName(), index + 1, data.trip.legs.size(), how, TravelRoute::Distance(leg.from, leg.to), path,
              seconds, seconds > 0.0f ? path / seconds : 0.0f, leg.seconds, TravelRoute::Distance(at, leg.to),
              TravelLegs::MountWord(record), record.mountedTicks, record.ticks, combat);
}

// The trace of a walk leg's pieces (WT70g-c, debug log only): at its start, its first and last piece, each off the
// graph (the bot's start to a node, the last node to the goal, a direct walk) or a graph link, and its end; when it
// fails, the piece it failed on. A walk leg's leg timeout off the graph fails no_walk (OverdueWalkReason)
void LogWalkPiece(Player* bot, TravelLegs::Trip const& trip, char const* when)
{
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != TravelRoute::LegType::Walk ||
        trip.waypoints.size() < 2 || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    auto const kind = [&trip](size_t index)
    {
        return trip.waypoints[index - 1].node == TravelLegs::NO_NODE ||
                       trip.waypoints[index].node == TravelLegs::NO_NODE
                   ? "off the graph"
                   : "a graph link";
    };
    size_t const count = trip.waypoints.size() - 1;
    size_t const piece = std::clamp<size_t>(trip.next, 1, count);
    TravelRoute::Point const& to = trip.legs[trip.legIdx].to;
    TravelRoute::Point const& at = trip.waypoints[piece].at;
    LOG_DEBUG("playerbots",
              "[Route] {} walk leg {}/{} {}: piece {}/{} {} to ({:.1f}, {:.1f}, {:.1f}); first {}, last {}; leg end "
              "({:.1f}, {:.1f}, {:.1f})",
              bot->GetName(), trip.legIdx + 1, trip.legs.size(), when, piece, count, kind(piece), at.x, at.y, at.z,
              kind(1), kind(count), to.x, to.y, to.z);
}

// A graph link's walk (WT58): its trip decision row, walk_link once the walk moved on past it or walk_link_failed when
// the leg failed on it: how (stored, nodes, fallback, refused), its moves, its ends' node ids and its hops
void LogWalkLink(Player* bot, HeldTrip const& data, TravelWalk::LinkWalk const& walk, bool done)
{
    if (!walk.Active())
        return;
    TravelGraph const& graph = TravelGraph::Instance();
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, done ? "walk_link" : "walk_link_failed",
                       TravelWalk::RowWord(walk), static_cast<float>(walk.moves),
                       {{"from", static_cast<float>(graph.GetNode(walk.from).id)},
                        {"to", static_cast<float>(graph.GetNode(walk.to).id)},
                        {"hops", static_cast<float>(walk.hops)}},
                       data.tripId);
}

constexpr float FLY_SAMPLE_YARDS = 50.0f;     // the real-height check samples the ground ahead this far apart
constexpr float LANDING_PROBE_YARDS = 5.0f;   // the landing check searches the ground down from this above the landing
constexpr float LANDING_PROBE_DEPTH = 40.0f;  // and this far down
constexpr uint32 FLY_SAMPLES_MAX = 8;         // and at most this often per waypoint

// A fly leg's end (WU10): its trip rows, fly_leg when done or fly_leg_failed (as walk_leg: how the bot went, its
// seconds in combat, its seconds against the estimate; a failed takeoff's row has why instead of how the bot went),
// then fly_height (raised when the real-height check lifted a waypoint, else planned): the highest z flown, against
// the plan's highest z, its levels and the raises
void LogFlyLeg(Player* bot, HeldTrip const& data, uint32 index, TravelLegs::LegRecord const& record, uint32 nowMs,
               uint32 elapsedMs, uint16 climbs, float peakZ, char const* how, bool done, char const* why = nullptr)
{
    TravelRoute::Leg const& leg = data.trip.legs[index];
    float const seconds = static_cast<float>(elapsedMs) / 1000.0f;
    float const combat = TravelLegs::CombatSeconds(record, nowMs);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, done ? "fly_leg" : "fly_leg_failed",
                       why ? why : TravelLegs::MountWord(record), combat,
                       {{"seconds", seconds}, {"estimate", leg.seconds}, {"leg", static_cast<float>(index + 1)}},
                       data.tripId);
    float plannedZ = leg.to.z;
    for (TravelRoute::Point const& point : leg.flightPoints)
        plannedZ = std::max(plannedZ, point.z);
    WorldLog::Decision(
        bot, WorldLogFormat::DecisionKind::Trip, "fly_height", climbs ? "raised" : "planned", peakZ,
        {{"max_z", plannedZ}, {"levels", static_cast<float>(leg.flightLevels)}, {"climbs", static_cast<float>(climbs)}},
        data.tripId);
    LOG_DEBUG("playerbots",
              "[Route] {} fly leg {}/{} {}: {:.0f} yd straight, {:.0f} yd planned at {} levels up to z {:.0f}, flown "
              "up to z {:.0f} with {} raises in {:.0f} s, estimate {:.0f} s, {:.0f} s in combat",
              bot->GetName(), index + 1, data.trip.legs.size(), how, TravelRoute::Distance(leg.from, leg.to),
              leg.flightYards, leg.flightLevels, plannedZ, peakZ, climbs, seconds, leg.seconds, combat);
}

// The real-height check's samples (WU10): the ground between the bot and its next waypoint in the air, buildings
// included (vmap), searched down from far above the flight line so a roof above it counts too
std::vector<float> SampleFlightGround(Player* bot, TravelRoute::Point const& target)
{
    float const fromX = bot->GetPositionX();
    float const fromY = bot->GetPositionY();
    float const yards = std::hypot(target.x - fromX, target.y - fromY);
    uint32 const count = std::clamp<uint32>(static_cast<uint32>(yards / FLY_SAMPLE_YARDS) + 1, 1, FLY_SAMPLES_MAX);
    float const top = std::max(target.z, bot->GetPositionZ()) + TravelLegs::FLY_RAISE_CAP_YARDS;
    std::vector<float> samples;
    samples.reserve(count);
    for (uint32 i = 1; i <= count; ++i)
    {
        float const share = static_cast<float>(i) / static_cast<float>(count);
        samples.push_back(bot->GetMapHeight(fromX + (target.x - fromX) * share, fromY + (target.y - fromY) * share, top,
                                            true, 2.0f * TravelLegs::FLY_RAISE_CAP_YARDS));
    }
    return samples;
}

// The landing's open-sky check (TravelLegs::NeedsLandingCheck): each point of the landing line on the ground below it,
// searched down from LANDING_PROBE_YARDS above the landing's height (a roof higher up is not the ground there), and
// outdoors there by the map's terrain status (WMO areas count). True when the landing moved (TravelLegs::MoveLanding)
bool CheckLanding(Player* bot, TravelLegs::Trip& trip)
{
    Map* map = bot->GetMap();
    std::vector<TravelRoute::Point> line = TravelLegs::LandingLine(trip);
    if (!map || line.empty())
        return TravelLegs::MoveLanding(trip, trip.waypoints.empty() ? TravelRoute::Point{} : trip.waypoints.back().at);
    uint32 const phase = bot->GetPhaseMask();
    for (TravelRoute::Point& point : line)
        point.z = map->GetHeight(phase, point.x, point.y, point.z + LANDING_PROBE_YARDS, true, LANDING_PROBE_DEPTH);
    float const collision = bot->GetCollisionHeight();
    auto const outdoors = [&](TravelRoute::Point const& point)
    {
        if (point.z <= INVALID_HEIGHT)
            return false;
        PositionFullTerrainStatus status;
        map->GetFullTerrainStatusForPosition(phase, point.x, point.y, point.z, collision, status);
        return status.outdoors;
    };
    std::size_t const index = TravelLegs::LandingIndex(line, outdoors);
    TravelRoute::Point const planned = trip.waypoints.back().at;
    if (!TravelLegs::MoveLanding(trip, index + 1 == line.size() ? planned : line[index]))
        return false;
    LOG_DEBUG("playerbots",
              "[Route] {} landing at ({:.0f}, {:.0f}, {:.0f}) is indoors: lands {:.0f} yd back at ({:.0f}, {:.0f}, "
              "{:.0f}) and walks in",
              bot->GetName(), planned.x, planned.y, planned.z, TravelRoute::Distance(planned, line[index]),
              line[index].x, line[index].y, line[index].z);
    return true;
}

// A fly leg's mount (WU10): off a ground mount, then the bot's fastest flying mount (CheckMountStateAction::
// CastFlyingMount; the engine's "check mount state" refused a bot standing still on open ground, WU11b), as the mount
// owner's policy allows (AskMountPolicy, WT44). A try in combat, while casting, while the bot loots or where it cannot
// take off is skipped and does not count (the follower asks again; the last fails the leg at once); so is a try while
// the bot moves or sits: the cast checks refuse it in the tick of the stop (WU11b: Edhoon's "cast result 255, moving
// true"), so it stops or stands up now and casts later. A counted try that starts no cast leaves its word for the
// fly_leg_failed row (the policy's nomount or on_foot; no_strategy; no_mount: no flying mount; refused: every cast)
bool MountToFly(Player* bot, PlayerbotAI* botAI, TravelLegs::Trip& trip, uint32 nowMs)
{
    if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(true) || CheckMountStateAction::NoTakeoffHere(bot))
        return true;
    MountRules::Verdict const verdict = CheckMountStateAction::AskMountPolicy(bot, botAI, MountRules::Kind::Flying);
    auto const step = TravelLegs::TakeFlyTry(trip, verdict, !bot->isMoving() && bot->IsStandState(), nowMs);
    if (step == MountRules::FlyTry::Wait || step == MountRules::FlyTry::Refused)
        return true;
    if (step == MountRules::FlyTry::Settle)
    {
        if (bot->isMoving())
            bot->StopMoving();
        if (!bot->IsStandState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        return true;
    }
    CheckMountStateAction::Dismount(bot, MountRules::How::Fly);
    if (!botAI->HasStrategy("mount", BOT_STATE_NON_COMBAT))
        trip.mountWhy = "no_strategy";
    else
        trip.mountWhy = CheckMountStateAction::CastFlyingMount(bot, botAI);
    return true;
}

// Why a fly leg's takeoff failed (the fly_leg_failed row's word): where the bot stands (NoTakeoffHere's words), else
// the last try's refusal, else what the tries left: a ground mount (ground_mount) or none after the cast (no_mount)
char const* TakeoffWord(Player* bot, TravelLegs::Trip const& trip)
{
    if (char const* here = CheckMountStateAction::NoTakeoffHere(bot))
        return here;
    if (*trip.mountWhy)
        return trip.mountWhy;
    return bot->IsMounted() ? "ground_mount" : "no_mount";
}

// The world log's route decision (only when it is on): the leg chain without no-op walks (an air link's leg is
// air:<from node id>-<to node id>), why it was searched, its seconds; weighed against a straight walk at run speed
// (when the chain is not one walk on the same map) and its leg count. The route service returns one route, so no
// other route is searched for the row.
void LogRouteDecision(Player* bot, HeldTrip const& data, TravelRoute::Result const& route)
{
    WorldLogFormat::WordChain chain;
    uint32 legs = 0;
    bool walksOnly = true;
    for (TravelRoute::Leg const& leg : route.legs)
    {
        if (TravelLegs::IsNoOp(leg))
            continue;
        if (leg.air && leg.nodes.size() == 2)
            chain.Add("air:" + std::to_string(TravelGraph::Instance().GetNode(leg.nodes.front()).id) + "-" +
                      std::to_string(TravelGraph::Instance().GetNode(leg.nodes.back()).id));
        else
            chain.Add(TravelRoute::LegTypeWord(leg.type));
        ++legs;
        walksOnly = walksOnly && leg.type == TravelRoute::LegType::Walk;
    }
    WorldLogFormat::Alternatives alternatives;
    if (!(legs == 1 && walksOnly) && bot->GetMapId() == data.dest.GetMapId())
    {
        float const yards =
            bot->GetExactDist(data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ());
        alternatives.Add({"walk_direct", WorldLogFormat::WalkSeconds(yards, bot->GetSpeed(MOVE_RUN))});
    }
    alternatives.Add({"legs", static_cast<float>(legs)});
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Route, legs ? chain.View() : "here",
                       data.trip.replans || data.trip.spotFails ? "replan" : "search", route.totalSeconds, alternatives,
                       data.tripId);
}

TravelRoute::Point BotPoint(Player* bot)
{
    return {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
}

// The failed leg or trip as the record of failed attempts keys it (TravelLegs::FailureOf)
FailedAttempts::Failure FailureOf(Player* bot, HeldTrip const& data)
{
    return TravelLegs::FailureOf(
        data.trip, {data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ()},
        BotPoint(bot));
}

// Mounted with a flight aura (TravelLegs::Observation::flyingMount)
bool OnFlyingMount(Player* bot)
{
    return bot->IsMounted() && (bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura());
}

// A done or failed walk or fly leg's time into the bot's travel shares (upkeep WU14, the riding ranks' worth); the
// area where it ended tells whether a flying mount would fly there (FlightMap's open areas)
void NoteTravelShares(PlayerbotAI* botAI, Player* bot, TravelRoute::LegType type, uint32 legMs, uint32 nowMs)
{
    constexpr uint32 SPELL_COLD_WEATHER_FLYING = 54197;
    WorldTravel::LegKind const kind = type == TravelRoute::LegType::Walk  ? WorldTravel::LegKind::Walk
                                      : type == TravelRoute::LegType::Fly ? WorldTravel::LegKind::Fly
                                                                          : WorldTravel::LegKind::Other;
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId());
    bool const open = area && area->IsFlyable() && !(area->flags & AREA_FLAG_NO_FLY_ZONE) &&
                      area->ID != AREA_WINTERGRASP && area->zone != AREA_WINTERGRASP;
    uint8 const shares = WorldTravel::LegShares(kind, bot->GetMapId(), open, bot->HasSpell(SPELL_COLD_WEATHER_FLYING));
    WorldTravel::SlotMeasure& measure =
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    WorldTravel::NoteTravelLeg(measure, shares, legMs, nowMs);
}
}  // namespace

bool TravelKeepsOnFoot(PlayerbotAI* botAI)
{
    // An errand stop's tame goes on foot (Tame Beast is not cast mounted; WU21b-2), and so does its stable visit (a
    // mounted hunter's pet is away, WU21c run 1: a remount after each dismount kept the visit waiting)
    if (auto const* const errands = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data))
        return errands->arrived && errands->stop < errands->stops.size() &&
               (errands->stops[errands->stop].tameEntry || errands->stops[errands->stop].stableEntry);
    auto const* const data = botAI->GetTravel().Trip();
    if (!data)
        return false;
    TravelLegs::Trip const& trip = data->trip;
    if (trip.offFirst.sinceMs)
        return true;
    // A transport leg goes on foot from its dock on (WT54-7: also while it waits there, KeepOnFootAtTransport)
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport;
}

bool NewRpgTravelAction::ConsumeScheduledWork(PlayerbotAI* botAI)
{
    using TravelProgress::Overdue;
    using TravelProgress::Work;
    auto* const data = botAI->GetTravel().Trip();
    Overdue const reason = data ? data->progress.Reason() : Overdue::None;
    bool const lootExpired = botAI->GetTravel().LootInteractionExpired();
    if (reason == Overdue::None && !lootExpired)
        return false;

    Player* const bot = botAI->GetBot();
    bool const walkFailure = reason == Overdue::Walk || reason == Overdue::OffMesh || reason == Overdue::RouteDeadline;
    if (reason != Overdue::None &&
        ((walkFailure && PassengerOwnsTravel(*bot, *data)) || !TransportGeometry::ReadMembership(*bot).Available))
        return false;

    bool const canMove = bot->IsAlive() && !bot->IsInCombat() && botAI->CanMove();
    Work const work =
        TravelProgress::ScheduledWork(canMove, reason, botAI->GetTravel().HasLootInteraction(), lootExpired);
    if (work == Work::Recover)
        return NewRpgTravelAction(botAI).StepTravel(data, data->progress.TakeReason());
    return work == Work::DeferLoot && botAI->GetTravel().ConsumeLootExpiry();
}

bool NewRpgTravelAction::Execute(Event /*event*/)
{
    auto* const data = botAI->GetTravel().Trip();
    if (!data || !bot->IsAlive() || bot->IsInCombat() || (!bot->IsInFlight() && !botAI->CanMove()))
        return false;
    if (ConsumeScheduledWork(botAI))
        return true;
    if (botAI->GetTravel().HasLootInteraction() && !PassengerOwnsTravel(*bot, *data))
        return false;
    return StepTravel(data, TravelProgress::Overdue::None);
}

bool NewRpgTravelAction::StepTravel(HeldTrip* data, TravelProgress::Overdue overdue)
{
    using TravelProgress::Overdue;
    auto const membership = TransportGeometry::ReadMembership(*bot);
    if (!membership.Available && !bot->IsInFlight())
        return true;
    data->stepped = true;  // the trip's walk clock runs from the follower's first step (TravelProgress::OwnerOf)
    // A trip operation requested aboard waits for an acknowledged exit within one period.
    if (data->trip.offFirst.sinceMs)
    {
        TravelLegs::OffStep const step = GetOffFirst(botAI, bot, *data);
        if (step == TravelLegs::OffStep::Wait || step == TravelLegs::OffStep::Deck ||
            step == TravelLegs::OffStep::Disembark)
            return true;
        if (step == TravelLegs::OffStep::GiveUp || step == TravelLegs::OffStep::Lost)
        {
            bool const expired = step == TravelLegs::OffStep::GiveUp;
            botAI->GetTravel().TraceTravelInteraction("route_recovery", expired ? "stuck_aboard" : "transport_lost");
            EndTrip(*data, expired ? StatusEnd::Outcome::Stuck : StatusEnd::Outcome::Abandoned,
                    expired ? "stuck aboard" : "transport lost",
                    expired ? "abort:stuck_aboard" : "abort:transport_lost");
            return true;
        }
        TravelLegs::OffFirst const& off = data->trip.offFirst;
        if (off.end == TravelLegs::OffFirstEnd::Arrive)
            return Arrive(*data);
        if (off.end == TravelLegs::OffFirstEnd::Abort)
            EndTrip(*data, off.outcome, off.why, off.source, true);
        return true;
    }
    Transport* const ridden = bot->GetTransport();
    TravelLegs::Trip const& trip = data->trip;
    // A route may have been superseded aboard. Only the route whose boarding the ride is may use it: its own boarding
    // result, or the leg's vessel docked under the bot at the wait point and attached by the passenger check (WT62)
    bool const ownsBoard = data->passengerMotion && data->passengerMotion->Kind == MotionKind::Board &&
                           data->passengerMotion->Transport == membership.Guid;
    bool const boardedByIntent = ridden && TravelLegs::BoardedByIntent(trip, membership.Guid);
    if (ridden && !boardedByIntent && (!AtTransportLeg(trip) || (!trip.passengerGuid && !ownsBoard)))
    {
        OffFirstBegins(bot, *data);
        return true;
    }
    if (overdue == Overdue::RouteDeadline)
    {
        botAI->GetTravel().TraceTravelInteraction("route_recovery", "deadline");
        EndTrip(*data, StatusEnd::Outcome::TimedOut, "deadline", "abort:deadline");
        return true;
    }
    if (overdue == Overdue::Walk || overdue == Overdue::OffMesh)
    {
        // A walk off the graph that never moved or stalled fails no_walk and replans without it (WT.56)
        bool const attempted = data->progress.AttemptedWalk();
        bool offMesh = false;
        char const* cause = nullptr;
        if (attempted)
        {
            if (data->progress.Owns(bot->movespline->GetId()) && !bot->movespline->Finalized())
                bot->StopMoving();
            WorldTravel::StuckCause const stuck = GiveUpWalk(data->walkTo);
            offMesh = stuck == WorldTravel::StuckCause::OffMesh;
            // The give-up's cause is the stuck leg's word in the record; a waypoint beyond reach is no goal's fault
            if (stuck != WorldTravel::StuckCause::UnreachablePoi)
                cause = WorldTravel::StuckCauseWord(stuck);
            data->walkTo = WorldPosition();
            if (!offMesh && !data->CurrentLeg() && data->bindInn)
            {
                botAI->GetTravel().TraceTravelInteraction("route_recovery", "stuck");
                return Arrive(*data, true);
            }
            // The mover gave a boarding row's dock point up (WT70c): the row is spent, the leg goes on with the next
            if (!offMesh && DockPointGivenUp(bot, *data))
                return true;
        }
        TravelLegs::FailReason const reason = TravelLegs::OverdueWalkReason(data->trip, attempted, offMesh);
        if (reason == TravelLegs::FailReason::None)
        {
            botAI->GetTravel().TraceTravelInteraction("route_recovery", "stalled");
            EndTrip(*data, StatusEnd::Outcome::Stuck, "stalled", "abort:stalled");
            return true;
        }
        return LegFailed(*data, reason, cause);
    }
    // Killed twice on the leg or three times on the trip (PlayerbotAI::ChangeEngine counts it, the follower runs
    // again once the bot is back): the leg fails, its edge is avoided; the third death ends the trip
    if (TravelLegs::DiedTooOften(data->trip))
        return LegFailed(*data, TravelLegs::FailReason::Death);
    // A fly leg failed in the air: down to the floor before the replan
    if (data->trip.descending)
        return FlyDown(*data, TravelLegs::StepDescent(data->trip, BotPoint(bot), OnFlyingMount(bot), getMSTime()));
    if (data->route.waiting)
        return TakeRoute(*data);
    if (data->trip.legs.empty())
    {
        // The trip's start, or a new route once the failed leg's stuck report has reached the published book; none
        // while a taxi carries the bot (WT64b: the request's place falls behind it, its answer is refused as moved)
        if (bot->IsInFlight() || (data->failedMs && GetMSTimeDiffToNow(data->failedMs) < TravelLegs::REPLAN_DELAY_MS))
            return true;
        return RequestRoute(*data);
    }
    TravelLegs::Observation seen;
    seen.bot = BotPoint(bot);
    seen.inFlight = bot->IsInFlight();
    seen.flyingMount = OnFlyingMount(bot);
    // Flying flags count only on the flying mount: a dismount in the air can leave them (no client sends the landing)
    seen.flying = seen.flyingMount && bot->IsFlying() && !seen.inFlight;
    // Before a fly leg's takeoff: may the bot mount a flyer and lift off here (a no-fly spot fails the leg at once).
    // Read also on the walk leg before it, so the tick that ends the walk and starts the flight already knows
    auto const isFly = [&](size_t idx)
    { return idx < data->trip.legs.size() && data->trip.legs[idx].type == TravelRoute::LegType::Fly; };
    if (!data->trip.seenInFlight && (isFly(data->trip.legIdx) || isFly(data->trip.legIdx + 1)))
        seen.takeoffHere = !CheckMountStateAction::NoTakeoffHere(bot);
    seen.money = bot->GetMoney();
    seen.nowMs = getMSTime();
    seen.legMs = data->progress.LegMs();
    seen.aboard = membership.Entry;
    seen.aboardGuid = membership.Guid;
    seen.membershipAvailable = membership.Available;
    seen.boardMotion = ownsBoard;
    seen.passengerOutcome = TakePassengerOutcome(*bot, *data);
    FillCrossings(*bot, trip, seen);
    KeepOnFootAtTransport(bot, trip);
    WatchDeckMove(bot, *data);
    // The transport scan (WT19): at the scan's pace while a transport leg is past its approach (ScanPaceMs: fast only
    // around a docking, WT70e; ScanDue: also at the check aimed at the docking, WT70e-d), on the bot's map only, and on
    // the tick a crossing's outcome is taken (a cancelled crossing's verdict is read from a scan at or after it). The
    // Transport* lives for this tick only
    Transport* transport = nullptr;
    if (AtTransportLeg(trip) &&
        (seen.passengerOutcome || TravelLegs::ScanDue(trip, seen.boardAt, seen.leaveAt, bot->GetMapId(),
                                                      getMSTimeDiff(data->transportScanMs, seen.nowMs), seen.nowMs)))
    {
        data->transportScanMs = seen.nowMs;
        seen.transport.scanned = true;
        uint64 const expected = data->passengerMotion ? data->passengerMotion->Transport : trip.sight.guid;
        transport = FindLegTransport(bot, trip.legs[trip.legIdx], expected);
        if (transport)
            SeeTransport(bot, transport, seen.transport);
    }
    TravelLegs::NoteTick(data->trip, bot->IsMounted());
    TravelLegs::LegRecord const recordBefore = trip.record;  // Advance starts a new leg's record
    uint32 const legBefore = trip.legIdx;
    uint32 const legStartBefore = trip.legStartMs;
    uint32 const waypointBefore = trip.next;
    uint16 const climbsBefore = trip.climbs;
    float const peakBefore = trip.peakZ;
    bool const wasWalk = data->CurrentLeg() && data->CurrentLeg()->type == TravelRoute::LegType::Walk &&
                         !TravelLegs::IsNoOp(*data->CurrentLeg());
    TravelWalk::LinkWalk const walkBefore = trip.walk;
    TravelLegs::Decision next = TravelLegs::Advance(data->trip, seen, GraphNodeAt);
    bool const legChanged = trip.legIdx != legBefore;
    // The graph link walked until now ends with its row once the walk moved on past it (WT58)
    if (walkBefore.Active())
    {
        auto const [linkFrom, linkTo] = WalkLinkEnds(trip);
        if (legChanged || linkFrom != walkBefore.from || linkTo != walkBefore.to)
        {
            LogWalkLink(bot, *data, walkBefore, true);
            data->trip.walk = {};
        }
    }
    bool const waypointReached =
        wasWalk && (legChanged || trip.next > waypointBefore) && data->progress.WaypointReached();
    if (waypointReached)
        botAI->GetTravel().TraceTravelInteraction("route_progress", "waypoint", ObjectGuid::Empty, 0, "none", 0.0f,
                                                  std::pair{legBefore, legStartBefore});
    if (legChanged)
    {
        data->progress.StartLeg(!wasWalk || waypointReached);
        LogWalkPiece(bot, trip, "starts");
    }
    // A move of the leg's that the rules track no more (CROSS_MAX_MS passed, the leg changed) is discarded
    if (data->passengerMotion && (legChanged || !TravelLegs::CrossUnderWay(trip)))
        TransportGeometry::DiscardMotion(*bot, data->passengerMotion);
    LogMisses(bot, trip);
    SendTransportNotes(bot, data->trip);
    if (seen.transport.scanned && ridden && trip.legIdx == legBefore &&
        (trip.boarding == TravelLegs::Boarding::Ride || trip.boarding == TravelLegs::Boarding::Leave) &&
        sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        LogAboardScan(bot, trip, seen, next.act);
    // The legs passed this tick (no-op walks are no legs to a reader): id the trip, amount the leg's seconds
    for (uint32 i = legBefore; i < trip.legIdx && i < trip.legs.size(); ++i)
    {
        if (TravelLegs::IsNoOp(trip.legs[i]))
            continue;
        uint32 const seconds = i == legBefore ? (seen.nowMs - legStartBefore) / IN_MILLISECONDS : 0;
        EventLog::Emit(bot, EventLog::EventType::Travel, TravelLegs::LegDoneSource(trip.legs[i].type), data->tripId,
                       seconds);
        if (i == legBefore &&
            (trip.legs[i].type == TravelRoute::LegType::Walk || trip.legs[i].type == TravelRoute::LegType::Fly))
            NoteTravelShares(botAI, bot, trip.legs[i].type, seen.nowMs - legStartBefore, seen.nowMs);
        if (i == legBefore && trip.legs[i].type == TravelRoute::LegType::Walk)
            LogWalkLeg(bot, *data, i, recordBefore, seen.nowMs, seen.nowMs - legStartBefore, "done", true);
        if (i == legBefore && trip.legs[i].type == TravelRoute::LegType::Fly)
        {
            LogFlyLeg(bot, *data, i, recordBefore, seen.nowMs, seen.nowMs - legStartBefore, climbsBefore, peakBefore,
                      "done", true);
            CheckMountStateAction::LandAndDismount(bot);
        }
    }
    // A test's flyfail or flyabort row: past the leg's middle and in the air the fly leg fails as its timeout would
    // (the way down after a failure in the air, upkeep WU26b); flyabort spends the trip's replans first. One bit test
    // outside such a test
    uint32 const constraints = botAI->testConstraints;
    if (next.act == TravelLegs::Act::Fly && (constraints & TestConstraints::FLY_FAILS))
    {
        TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
        auto const [groundZ, waterZ] = CheckMountStateAction::FloorBelow(bot);
        bool const inTheAir = TravelLegs::BodyZ(seen.bot.z, groundZ, waterZ) < seen.bot.z;
        if (TestConstraints::FlyLegFails(constraints, std::hypot(seen.bot.x - leg.from.x, seen.bot.y - leg.from.y),
                                         std::hypot(leg.to.x - leg.from.x, leg.to.y - leg.from.y), inTheAir))
        {
            if (TestConstraints::FlyFailAborts(constraints))
                data->trip.replans = TravelLegs::MAX_REPLANS;
            LOG_INFO("playerbots", "[Route] {} fly leg {}/{} fails in the air by the test constraint {}",
                     bot->GetName(), trip.legIdx + 1, trip.legs.size(),
                     TestConstraints::Format(constraints & TestConstraints::FLY_FAILS));
            return LegFailed(*data, TravelLegs::FailReason::Timeout);
        }
    }
    // Once per waypoint in the air: the real ground ahead (the flight map has no buildings) may lift it
    if (next.act == TravelLegs::Act::Fly && TravelLegs::NeedsHeightCheck(trip))
    {
        TravelLegs::CheckHeight(data->trip, SampleFlightGround(bot, trip.waypoints[trip.next].at));
        next.target = trip.waypoints[trip.next].at;
    }
    // Once per fly leg near its end: an indoor landing (an inn's floor under its roof) moves to the outdoor spot
    // nearest to it back along the flight, and the bot walks in from there
    if (next.act == TravelLegs::Act::Fly && TravelLegs::NeedsLandingCheck(trip, seen.bot) &&
        CheckLanding(bot, data->trip))
        next.target = trip.waypoints[trip.next].at;
    // A transport leg's first tick wakes the bot's followers (their checks at the scan's pace from now on)
    NewRpgInternal::WakeFollowers(bot, data->trip);
    // For the case's hold line: what the follower did last, and when
    data->trip.lastAct = next.act;
    data->trip.lastTarget = next.target;
    data->trip.lastActMs = seen.nowMs;
    switch (next.act)
    {
        case TravelLegs::Act::Arrived:
            return Arrive(*data);
        case TravelLegs::Act::Walk:
            // A transport leg's approach: on the world mesh to the boarding row's dock point (no graph link, no
            // probe of a walk piece)
            if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport)
                return MoveFarTo(WorldPosition(next.target.mapId, next.target.x, next.target.y, next.target.z));
            // Along a graph link: its stored points (WT58); the engine's "no path" at its far node refused it
            if (trip.walk.mode == TravelWalk::Mode::Refused)
                return LegFailed(*data, TravelLegs::FailReason::NoPath);
            next.target = WalkLinkTarget(data->trip, seen.bot, next.target);
            // Off the graph: the navmesh probe's points (WT56); a walk the navmesh refused fails the leg at once, and
            // the replan leaves it out, also where the bot stands off the mesh (WT.56: one path for a refused walk)
            if (!WalkPieceTarget(bot, data->trip, data->tripId, seen.bot, next.target))
                return LegFailed(*data, TravelLegs::FailReason::NoWalk);
            data->trip.lastTarget = next.target;
            return MoveFarTo(WorldPosition(next.target.mapId, next.target.x, next.target.y, next.target.z));
        case TravelLegs::Act::UseTrigger:
            return UsePortal(*data);
        case TravelLegs::Act::UseObject:
            return UseStaticPortal(*data);
        case TravelLegs::Act::TalkFlightMaster:
            return TalkFlightMaster(*data);
        case TravelLegs::Act::UseHearth:
            return UseHearth(*data);
        case TravelLegs::Act::Mount:
            return MountToFly(bot, botAI, data->trip, seen.nowMs);
        case TravelLegs::Act::Fly:
            // On the flying mount MoveTo gives the bot the flying flags and a straight line to the point, no path;
            // a move still running keeps the follower in charge
            MoveTo(next.target.mapId, next.target.x, next.target.y, next.target.z, false, false, false, true,
                   MovementPriority::MOVEMENT_NORMAL, true);
            return true;
        case TravelLegs::Act::Ride:
            // As the random flight status does: nothing else to do in the air
            ContinueCrossMapTaxi();
            return false;
        case TravelLegs::Act::Board:
            // Onto the vessel by the current boarding row (between scans the vessel is looked up by its guid)
            if (!transport)
                transport = FindLegTransport(bot, trip.legs[trip.legIdx], trip.sight.guid);
            if (transport)
                CrossByRow(bot, *data, *transport, seen);
            SendTransportNotes(bot, data->trip);
            return true;
        case TravelLegs::Act::Stand:
            TakePlace(botAI, bot, *data, seen);
            SendTransportNotes(bot, data->trip);
            return true;
        case TravelLegs::Act::Deck:
            WalkDeckToRow(botAI, bot, *data, seen);
            SendTransportNotes(bot, data->trip);
            return true;
        case TravelLegs::Act::Disembark:
            // Off the vessel by the current leaving row (the far stop's, or after a no_exit the from-stop's)
            if (ridden)
                CrossByRow(bot, *data, *ridden, seen);
            SendTransportNotes(bot, data->trip);
            return true;
        case TravelLegs::Act::Fail:
            return LegFailed(*data, next.reason);
        case TravelLegs::Act::Descend:  // StartDescent's and StepDescent's only (FlyDown)
        case TravelLegs::Act::Fall:
        case TravelLegs::Act::Wait:
        case TravelLegs::Act::Done:
            return true;
    }
    return false;
}

bool NewRpgTravelAction::RequestRoute(HeldTrip& data)
{
    TravelRoute::Snapshot snapshot = TravelRoute::FillSnapshot(bot, botAI->GetTravel().NextRouteRequest());
    // Transports run (WT19; a failed ride's link is the record's, left out through the request). Flights only to
    // discovered nodes (the snapshot's taxi mask), their fares within the purse (the search's fare budget). The
    // hearthstone is a leg while its stone is in the bags and it is ready or cooling for at most the wait cap (the
    // search); not while the record refuses it (a hearth leg failed)
    snapshot.hasHearthstone = snapshot.hasHearthstone && TestConstraints::HearthAllowed(botAI->testConstraints);
    snapshot.allowTransport = true;
    if (!TestConstraints::TaxiAllowed(botAI->testConstraints))
        snapshot.knownTaxi.fill(0);
    if (!TestConstraints::OwnFlyingAllowed(botAI->testConstraints))  // nofly: a taxi is the bot's only flying
        snapshot.flyOutland = snapshot.flyNorthrend = false;
    // With the plain copy of what the record of failed attempts refuses now (TravelLegs::NextRequest, CONTRACT C3)
    TravelRoute::Request request = TravelLegs::NextRequest(
        snapshot, {data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ()},
        FailedAttempts::LiveEntries(botAI->GetTravel().Failures(), getMSTime()));
    request.freeRides = TestConstraints::RidesFree(botAI->testConstraints);  // a test's case that must ride
    TravelLegs::RouteWait const waiting{snapshot.requestId, snapshot.position.mapId, snapshot.takenMs, true,
                                        snapshot.position};
    auto const submit = [&]
    {
        return TravelRoute::RouteService::Instance().Submit(bot->GetGUID().GetRawValue(), request,
                                                            TravelRoute::ConfiguredParams());
    };
    if (!TravelProgress::SubmitRoute(data.route, waiting, submit))
    {
        // The graph is not loaded yet: never wait
        EndTrip(data, StatusEnd::Outcome::Abandoned, "no route service", "abort:no_service");
        return true;
    }
    botAI->GetTravel().TraceTravelInteraction("route_request", "issued");
    return true;
}

bool NewRpgTravelAction::TakeRoute(HeldTrip& data)
{
    uint32 const now = getMSTime();
    std::optional<TravelRoute::Answer> answer =
        TravelRoute::RouteService::Instance().TryTake(bot->GetGUID().GetRawValue());
    TravelRoute::Point const current{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    // A late or lost answer asks again from here, a few times per trip, before it fails the leg; the failing ask's why
    // goes into the trip row no_answer beside the leg's leg_failed:no_answer (WT64f)
    auto const askAgain = [&](std::optional<uint32> answerId)
    {
        char const* const why = TravelLegs::AskWhyWord(TravelLegs::WhyAskAgain(data.route, answerId, current, now));
        uint32 const waitedMs = now - data.route.requestedMs;
        if (!TravelLegs::Resubmit(data.trip))
        {
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "no_answer", why, waitedMs / 1000.0f,
                               {{"replans", static_cast<float>(data.trip.replans)}}, data.tripId);
            return LegFailed(data, TravelLegs::FailReason::NoAnswer);
        }
        botAI->GetTravel().TraceTravelInteraction("route_recovery", "resubmit");
        LOG_DEBUG("playerbots", "[Route] {} asked again: {} ({} ms since the request)", bot->GetName(), why, waitedMs);
        return RequestRoute(data);
    };
    auto const answerLate = [&] { return TravelLegs::NextDeadlineMs(data.trip, now, 0, &data.route) < 0; };
    if (!answer)
        return answerLate() ? askAgain(std::nullopt) : true;
    switch (TravelLegs::Judge(data.route, answer->requestId, current, now))
    {
        case TravelLegs::Verdict::Drop:
            return answerLate() ? askAgain(answer->requestId) : true;
        case TravelLegs::Verdict::Refresh:
            return askAgain(answer->requestId);
        case TravelLegs::Verdict::Apply:
            break;
    }

    data.route.waiting = false;
    TravelRoute::Result& route = answer->result;
    if (!route.ok)
    {
        LOG_DEBUG("playerbots",
                  "[Route] {} has no route: {} from ({}, {:.0f}, {:.0f}, {:.0f}) to ({}, {:.0f}, {:.0f}, {:.0f}), "
                  "replans {}",
                  bot->GetName(), TravelRoute::FailureWord(route.failure), bot->GetMapId(), bot->GetPositionX(),
                  bot->GetPositionY(), bot->GetPositionZ(), data.dest.GetMapId(), data.dest.GetPositionX(),
                  data.dest.GetPositionY(), data.dest.GetPositionZ(), data.trip.replans);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Route, "none", TravelRoute::FailureWord(route.failure),
                           0.0f, {}, data.tripId);
        return LegFailed(data, TravelLegs::FailReason::NoRoute);
    }
    if (data.estimateSeconds <= 0.0f)
    {
        // The trip's clock starts at its start row, so the ends' seconds span the logged trip (the wait for the
        // first route is not counted; a trip that ends before this row counts from its pick)
        data.tripStartMs = now;
        data.estimateSeconds = route.totalSeconds;
        EventLog::Emit(bot, EventLog::EventType::Travel, "start", data.tripId, static_cast<int64>(route.totalSeconds));
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "start", RpgStatus::TripGoal(botAI, data.bindInn),
                           route.totalSeconds, {{"legs", static_cast<float>(route.legs.size())}}, data.tripId);
        if (!data.place.empty())
            LOG_INFO("playerbots", "[Route] {} travel to {}: {} legs, {:.0f} s", bot->GetName(), data.place,
                     route.legs.size(), route.totalSeconds);
    }
    if (WorldLog::Active())
        LogRouteDecision(bot, data, route);
    data.progress.AcceptRoute();
    data.routeSeconds = route.totalSeconds;
    LOG_DEBUG("playerbots",
              "[Route] {} to ({}, {:.0f}, {:.0f}, {:.0f}): {} legs, {:.0f} s; taken {} ms after the request (in combat "
              "{}, active {})",
              bot->GetName(), data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(),
              data.dest.GetPositionZ(), route.legs.size(), route.totalSeconds, now - data.route.requestedMs,
              bot->IsInCombat(), botAI->AllowActivity());
    if (route.legs.empty())
        return Arrive(data);  // already there
    data.trip.legs = std::move(route.legs);
    TravelLegs::StartLeg(data.trip, 0, now, GraphNodeAt);
    LogWalkPiece(bot, data.trip, "starts");
    return true;
}

bool NewRpgTravelAction::UsePortal(HeldTrip& data)
{
    uint32 const triggerId = data.trip.legs[data.trip.legIdx].object;
    AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
    if (!trigger || !sObjectMgr->GetAreaTriggerTeleport(triggerId) || trigger->map != bot->GetMapId())
        return LegFailed(data, TravelLegs::FailReason::Portal);
    if (!bot->IsInAreaTriggerRadius(trigger))
        return MoveFarTo(WorldPosition(trigger->map, trigger->x, trigger->y, trigger->z));

    // What a player's client sends on entering the trigger: the core's own handler checks it and teleports
    WorldPacket packet(CMSG_AREATRIGGER);
    packet << triggerId;
    packet.rpos(0);
    bot->GetSession()->HandleAreaTriggerOpcode(packet);
    data.trip.triggered = true;
    return true;
}

// A static portal leg (WT55d): the portal object a player clicks, which stands at the leg's node. Looked up only on
// the ticks the bot stands there (StepLeg's UseObject, until the use is sent); used as a player's client does, and
// the core's own handler checks the use and casts the portal's teleport
bool NewRpgTravelAction::UseStaticPortal(HeldTrip& data)
{
    constexpr float PORTAL_OBJECT_YARDS = 20.0f;  // the object's spawn this far from its node at most
    uint32 const entry = data.trip.legs[data.trip.legIdx].object;
    GameObject* portal = bot->FindNearestGameObject(entry, PORTAL_OBJECT_YARDS);
    if (!portal || !portal->isSpawned())
    {
        // Graph data, not leg code (WT.41): the node names an object the world has not spawned there
        LOG_INFO("playerbots",
                 "[Route] {} static portal: no object {} within {:.0f} yd at map {} ({:.0f}, {:.0f}, {:.0f})",
                 bot->GetName(), entry, PORTAL_OBJECT_YARDS, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                 bot->GetPositionZ());
        return LegFailed(data, TravelLegs::FailReason::Portal);
    }
    if (!portal->IsWithinDistInMap(bot, portal->GetInteractionDistance()))
        return MoveFarTo(
            WorldPosition(portal->GetMapId(), portal->GetPositionX(), portal->GetPositionY(), portal->GetPositionZ()));

    if (bot->isMoving())
        bot->StopMoving();
    bot->SetFacingToObject(portal);
    WorldPacket use(CMSG_GAMEOBJ_USE, 8);
    use << portal->GetGUID();
    bot->GetSession()->HandleGameObjectUseOpcode(use);
    data.trip.triggered = true;
    return true;
}

bool NewRpgTravelAction::TalkFlightMaster(HeldTrip& data)
{
    TravelRoute::Leg const& leg = data.trip.legs[data.trip.legIdx];
    if (leg.taxiNodes.size() < 2)
        return LegFailed(data, TravelLegs::FailReason::Taxi);
    // The master of the leg's first taxi node nearest the leg's start (startup cache, read only; scanned only on the
    // few ticks the bot stands at the master)
    TravelMgr::FlightMasterInfo const* master = nullptr;
    float nearest = TravelLegs::FLIGHT_MASTER_YARDS * 2.0f;
    for (auto const& [dbGuid, info] : sTravelMgr.GetFlightMasters(bot->GetTeamId()))
    {
        if (info.taxiNodeId != leg.taxiNodes.front() || info.pos.GetMapId() != leg.from.mapId)
            continue;
        float const yards = TravelRoute::Distance(
            {info.pos.GetMapId(), info.pos.GetPositionX(), info.pos.GetPositionY(), info.pos.GetPositionZ()}, leg.from);
        if (yards < nearest)
        {
            nearest = yards;
            master = &info;
        }
    }
    if (!master)
        return LegFailed(data, TravelLegs::FailReason::Taxi);

    auto const approach = [this](WorldPosition const& to)
    { return MoveFarTo(to) ? TaxiStart::Moving : TaxiStart::Stalled; };
    switch (StartTaxi(master->templateEntry, master->pos, leg.taxiNodes, approach))
    {
        case TaxiStart::Moving:
            return true;
        case TaxiStart::Stalled:
            return false;
        case TaxiStart::Started:
            data.trip.taxiActivated = true;
            return true;
        case TaxiStart::Refused:
        {
            // The core's fare differs from the snapshot's: a purse short of the chain's price fails as the fare
            uint32 cost = 0;
            for (size_t i = 0; i + 1 < leg.taxiNodes.size(); ++i)
            {
                uint32 path = 0;
                uint32 price = 0;
                sObjectMgr->GetTaxiPath(leg.taxiNodes[i], leg.taxiNodes[i + 1], path, price);
                cost += price;
            }
            return LegFailed(data,
                             bot->GetMoney() < cost ? TravelLegs::FailReason::Fare : TravelLegs::FailReason::Taxi);
        }
        case TaxiStart::NoMaster:
        case TaxiStart::Undiscovered:
            break;
    }
    return LegFailed(data, TravelLegs::FailReason::Taxi);
}

bool NewRpgTravelAction::UseHearth(HeldTrip& data)
{
    // The cast takes 10 s and a cooling stone is waited for (the route's seconds hold both; the leg's timeout bounds
    // them). The teleport is the stone's own spell: no bot teleport, no sight check
    if (bot->IsNonMeleeSpellCast(false) || bot->HasSpellCooldown(HEARTHSTONE_SPELL))
        return true;
    // A form (bear, cat, ghost wolf) makes the core answer 68 (not shapeshifted): leave it on the ground, once while
    // the stone is ready (a cooling stone returned above); a flight form stays for CastHearth's own guard
    if (!bot->IsFlying() && !bot->IsInFlight() && !bot->IsInCombat() && bot->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
        botAI->RemoveShapeshift();
    char const* refusal = nullptr;
    float castResult = 0.0f;
    if (!CastHearth(refusal, castResult))
    {
        // The world log's row names why (WorldTravel::HearthRefusalWord; cast_failed: the value is the core's result)
        LOG_DEBUG("playerbots", "[Route] {} hearth refused: {} (cast result {:.0f})", bot->GetName(),
                  refusal ? refusal : "?", castResult);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "hearth_refused", refusal ? refusal : "?",
                           castResult, {}, data.tripId);
        return LegFailed(data, TravelLegs::FailReason::Hearth);
    }
    return true;
}

bool NewRpgTravelAction::LegFailed(HeldTrip& data, TravelLegs::FailReason reason, char const* cause)
{
    botAI->GetTravel().TraceTravelInteraction("route_recovery", TravelLegs::FailWord(reason));
    // The leg's typed end in the record of failed attempts, keyed by the leg as it failed; an off_mesh end is noted by
    // the recovery where it leaves the bot (RecoverOffMesh)
    if (reason != TravelLegs::FailReason::OffMesh)
        botAI->GetTravel().NoteFailure(
            reason == TravelLegs::FailReason::Stuck && cause ? cause : TravelLegs::FailWord(reason),
            FailureOf(bot, data), data.tripId);
    // A refused order on a transport's approach or a taxi's walk: its walk too (WT.59)
    if (auto const walk = TravelLegs::RefusedOrderWalk(data.trip, reason, BotPoint(bot)))
        botAI->GetTravel().NoteFailure(TravelLegs::FailWord(reason), *walk, data.tripId);
    TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
    TravelLegs::Trip& trip = data.trip;
    LOG_DEBUG("playerbots", "[Route] {} leg {}/{} failed: {}", bot->GetName(), trip.legIdx + 1, trip.legs.size(),
              TravelLegs::FailWord(reason));
    uint32 const legSeconds =
        trip.legIdx < trip.legs.size() ? GetMSTimeDiffToNow(trip.legStartMs) / IN_MILLISECONDS : 0;
    EventLog::Emit(bot, EventLog::EventType::Travel, TravelLegs::LegFailedSource(reason), data.tripId, legSeconds);
    if (trip.legIdx < trip.legs.size() && (trip.legs[trip.legIdx].type == TravelRoute::LegType::Walk ||
                                           trip.legs[trip.legIdx].type == TravelRoute::LegType::Fly))
    {
        uint32 const now = getMSTime();
        NoteTravelShares(botAI, bot, trip.legs[trip.legIdx].type, now - trip.legStartMs, now);
    }
    // A failed walk's row before AfterFailure starts the next record
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Walk)
    {
        uint32 const now = getMSTime();
        LogWalkLeg(bot, data, trip.legIdx, trip.record, now, now - trip.legStartMs,
                   reason == TravelLegs::FailReason::Timeout ? "timed out" : TravelLegs::FailWord(reason), false);
        LogWalkPiece(bot, trip, "failed");
    }
    // A failed fly leg's rows; down away from its end after the takeoff is a fall (or a dismount in the air): its
    // height above the ground now. A bot that fails in the air comes down before the replan (StartDescent, below)
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Fly)
    {
        uint32 const now = getMSTime();
        LogFlyLeg(bot, data, trip.legIdx, trip.record, now, now - trip.legStartMs, trip.climbs, trip.peakZ,
                  reason == TravelLegs::FailReason::Timeout ? "timed out" : TravelLegs::FailWord(reason), false,
                  reason == TravelLegs::FailReason::Takeoff ? TakeoffWord(bot, trip) : nullptr);
        if (reason == TravelLegs::FailReason::Landing)
        {
            float const ground = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            float const height = ground > INVALID_HEIGHT ? std::max(0.0f, bot->GetPositionZ() - ground) : 0.0f;
            char const* const word = TravelLegs::LandingWord(trip, height);
            EventLog::Emit(bot, EventLog::EventType::FlightFall, word, data.tripId, std::lround(height));
            // What brought it down (WU11b fix 7: Elyvia and Meldel, both in Zul'Drak's area 4310): a fight after the
            // takeoff (combat) or not (fall), the mount, its flight aura and the flying flag now, the core's indoor
            // state (its vmap indoor check takes an outdoor-only mount: fix 10's lead for area 4310), the height and
            // area
            LOG_INFO("playerbots",
                     "[Route] {} came down on fly leg {}/{} ({}): mounted {}, flight aura {}, flying {}, outdoors {}, "
                     "{:.0f} yd above the ground in area {} at ({:.0f}, {:.0f}, {:.0f})",
                     bot->GetName(), trip.legIdx + 1, trip.legs.size(), word, bot->IsMounted(),
                     bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura(), bot->IsFlying(), bot->IsOutdoors(),
                     height, bot->GetAreaId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        }
    }
    // The graph link the leg failed on (WT58): its row; a refusal's line names the link's ends
    if (trip.walk.Active())
    {
        if (reason == TravelLegs::FailReason::NoPath)
        {
            TravelGraph const& graph = TravelGraph::Instance();
            LOG_DEBUG("playerbots", "[Route] {} walk leg {}/{} link {} ({}) -> {} ({}) refused: no path",
                      bot->GetName(), trip.legIdx + 1, trip.legs.size(), graph.GetNode(trip.walk.from).id,
                      graph.GetName(trip.walk.from), graph.GetNode(trip.walk.to).id, graph.GetName(trip.walk.to));
        }
        LogWalkLink(bot, data, trip.walk, false);
        trip.walk = {};
    }
    // A takeoff refused where the bot stands keeps flying: the record refuses the spot, the replan takes off elsewhere
    bool const spotClosed =
        reason == TravelLegs::FailReason::Takeoff && CheckMountStateAction::NoTakeoffHere(bot) != nullptr;
    bool const aborts = TravelLegs::AfterFailure(trip, reason, spotClosed) == TravelLegs::Recovery::Abort;
    // A fly leg failed in the air (no client gravity holds a bot up where it failed, and every walk replan failed at
    // that spot): on its flying mount it flies straight down to the floor, without one it falls; then the replan. An
    // aborting trip comes down too and ends once it is down (FlyDown; WU12b: it hovered where the leg failed)
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Fly)
    {
        auto const [groundZ, waterZ] = CheckMountStateAction::FloorBelow(bot);
        bool const flyingMount = OnFlyingMount(bot);
        TravelLegs::Decision const down =
            aborts ? TravelLegs::AbortDescent(trip, reason, BotPoint(bot), groundZ, waterZ, flyingMount, getMSTime())
                   : TravelLegs::StartDescent(trip, BotPoint(bot), groundZ, waterZ, flyingMount, getMSTime());
        if (down.act != TravelLegs::Act::Done)
        {
            LOG_INFO("playerbots",
                     "[Route] {} fly leg {}/{} failed ({}) {:.0f} yd above the floor in area {} at ({:.0f}, {:.0f}, "
                     "{:.0f}): {}",
                     bot->GetName(), trip.legIdx + 1, trip.legs.size(), TravelLegs::FailWord(reason),
                     bot->GetPositionZ() - down.target.z, bot->GetAreaId(), bot->GetPositionX(), bot->GetPositionY(),
                     bot->GetPositionZ(), down.act == TravelLegs::Act::Descend ? "flies down" : "falls");
            FlyDown(data, down);
            if (aborts)
                return true;  // the trip ends in FlyDown, now or once it is down
        }
    }
    if (aborts)
    {
        EndTrip(data, TravelLegs::FailOutcome(reason), TravelLegs::FailWord(reason), TravelLegs::AbortSource(reason),
                true);
        return true;
    }
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "replan", TravelLegs::FailWord(reason),
                       static_cast<float>(legSeconds), {{"leg", static_cast<float>(trip.legIdx + 1)}}, data.tripId);
    trip.legs.clear();
    trip.legIdx = 0;
    trip.waypoints.clear();
    data.route.waiting = false;
    data.progress.Recover();
    data.failedMs = std::max<uint32>(getMSTime(), 1);  // 0 = no failed leg
    return true;
}

bool NewRpgTravelAction::FlyDown(HeldTrip& data, TravelLegs::Decision const& down)
{
    TravelLegs::Trip& trip = data.trip;
    uint32 const now = getMSTime();
    trip.lastAct = down.act;
    trip.lastTarget = down.target;
    trip.lastActMs = now;
    // An aborting trip ends once the descent is over, whatever ended it (down, fell, gave up)
    bool const endsTrip = TravelLegs::EndsAfterDescent(trip, down);
    TravelLegs::FailReason const endReason = trip.endReason;
    switch (down.act)
    {
        case TravelLegs::Act::Descend:
            // Straight down on the flying mount, as the fly leg's follower moves (no path generation in the air)
            MoveTo(down.target.mapId, down.target.x, down.target.y, down.target.z, false, false, false, true,
                   MovementPriority::MOVEMENT_NORMAL, true);
            return true;
        case TravelLegs::Act::Done:
            LOG_INFO("playerbots", "[Route] {} flew down to the floor in {} s, area {} at ({:.0f}, {:.0f}, {:.0f})",
                     bot->GetName(), getMSTimeDiff(trip.descentStartMs, now) / IN_MILLISECONDS, bot->GetAreaId(),
                     bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            CheckMountStateAction::LandAndDismount(bot);
            break;
        case TravelLegs::Act::Fall:
            // As a player falls (the fall's damage too): the mount's aura and the flying flags go first
            LOG_INFO("playerbots", "[Route] {} falls {:.0f} yd without a flying mount in area {} at ({:.0f}, {:.0f})",
                     bot->GetName(), bot->GetPositionZ() - down.target.z, bot->GetAreaId(), bot->GetPositionX(),
                     bot->GetPositionY());
            CheckMountStateAction::LandAndDismount(bot);
            break;
        case TravelLegs::Act::Fail:
            // The replan starts where the bot is
            LOG_INFO("playerbots", "[Route] {} gives the flight down up ({}) at z {:.0f}, floor {:.0f}, in area {}",
                     bot->GetName(), TravelLegs::FailWord(down.reason), bot->GetPositionZ(), trip.descentTo.z,
                     bot->GetAreaId());
            break;
        default:
            break;
    }
    if (endsTrip)
        EndTrip(data, TravelLegs::FailOutcome(endReason), TravelLegs::FailWord(endReason),
                TravelLegs::AbortSource(endReason), true);
    return true;
}

bool NewRpgTravelAction::Arrive(HeldTrip& data, bool stuck)
{
    // Not on a ship's deck: the bot gets off first, then arrives (WT19-d)
    if (OffFirstBegins(bot, data, TravelLegs::OffFirstEnd::Arrive))
        return true;
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    uint32 const replans = data.trip.replans;
    if (data.bindInn)
    {
        // The route ends in front of the innkeeper (InnInfo::loc). Walk until close enough to talk to him, then bind
        // directly (not through the "home" action, which needs a cleared target and a cached npc list); a failed bind
        // is tried again a few times (after one out of his sight beside him), a stuck bot out of reach gives up
        Unit* keeper = data.innKeeper ? botAI->GetUnit(data.innKeeper) : nullptr;
        float const yards = keeper ? bot->GetExactDist(keeper) : bot->GetExactDist(data.dest);
        WorldTravel::InnStep const step = WorldTravel::InnNext(yards, stuck, data.bindTries, keeper != nullptr);
        if (step == WorldTravel::InnStep::Walk)
            return MoveFarTo(keeper ? WorldPosition(keeper) : data.dest);
        bool bound = false;
        char const* why = "stuck out of reach";
        if (step == WorldTravel::InnStep::Bind)
        {
            ++data.bindTries;
            bound = NewRpgInternal::BindAtInn(botAI, why, data.innKeeper);
            if (!bound && data.bindTries < WorldTravel::INN_BIND_TRIES)
            {
                LOG_DEBUG("playerbots", "[Route] {} bind try {} failed ({}), {:.1f} yd from the {}", bot->GetName(),
                          static_cast<uint32>(data.bindTries), why, yards, keeper ? "innkeeper" : "inn spot");
                WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "retry", WorldTravel::InnAvoidWord(why),
                                   yards, {}, data.tripId);
                return true;
            }
        }
        LOG_INFO("playerbots", "[Route] {} {} at the inn after {} s ({} replans, {} binds, {:.1f} yd{}{})",
                 bot->GetName(), bound ? "bound its hearthstone" : "could not bind", seconds, replans,
                 static_cast<uint32>(data.bindTries), yards, bound ? "" : ": ", bound ? "" : why);
        EventLog::Emit(bot, EventLog::EventType::Travel, bound ? "inn_bound" : "inn_failed", data.tripId, seconds);
        // A spot where the bot stood but could not bind (no usable innkeeper, a bind that did not take) is refused to
        // its next inn trips for a while: the trip's not_bound end writes its place to the record of failed attempts.
        // The unbound trip is given up, which stamps the planner's hearth retry (WT63d-b: an arrived end cleared it,
        // and the planner sent the bot to the inn again)
        WorldTravel::InnEnd const end = WorldTravel::InnEndOf(bound, step);
        if (end.avoid)
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "avoid", WorldTravel::InnAvoidWord(why), yards,
                               {}, data.tripId);
        if (!bound)
        {
            EndTrip(data, end.outcome, why, end.avoid ? "abort:not_bound" : "abort:stuck");
            return true;
        }
    }
    // The trip's arrive arm (the log line, the arrived Travel event); only travel's own status ends with it (WT64b)
    RpgStatus::TripEnded(botAI, StatusEnd::Outcome::Arrived);
    return true;
}

void NewRpgTravelAction::EndTrip(HeldTrip& data, StatusEnd::Outcome outcome, char const* why, char const* source,
                                 bool noted)
{
    // The trip's own typed end in the record of failed attempts (its source's cause word: abort:no_route, ...)
    if (!noted)
        botAI->GetTravel().NoteFailure(source, FailureOf(bot, data), data.tripId);
    // Not on a ship's deck: the bot gets off first, then the trip ends (WT19-d)
    if (OffFirstBegins(bot, data, TravelLegs::OffFirstEnd::Abort, why, source, outcome))
        return;
    // The status core's abort arm: the log line, the Travel event and the trip's abort decision
    RpgStatus::TripEnded(botAI, outcome, {.word = "abort", .source = source, .text = why});
}
