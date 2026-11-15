/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"

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
#include "BroadcastHelper.h"
#include "CellImpl.h"
#include "CensusMgr.h"
#include "ChatHelper.h"
#include "CheckMountStateAction.h"
#include "DBCStores.h"
#include "ErrandRules.h"
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
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
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
#include "TravelTransportGeometry.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "WaypointMovementGenerator.h"
#include "World.h"
#include "WorldLog.h"
#include "WorldLogStatus.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldTravelRules.h"

using namespace NewRpgInternal;

namespace
{
// The Travel event sources of an abort ("abort:<reason>"); the trip's abort decision takes the reason
constexpr std::string_view TRAVEL_ABORT_PREFIX = "abort:";
}  // namespace

namespace NewRpgInternal
{
// Gives the bot's trip up: the Travel event (source "abort:<reason>", a literal), the trip's abort decision, idle
void GiveTripUp(PlayerbotAI* botAI, NewRpgInfo::Travel const& data, char const* why, char const* source)
{
    Player* bot = botAI->GetBot();
    if (data.place.empty())
        LOG_DEBUG("playerbots", "[Route] {} gives the trip up: {}", bot->GetName(), why);
    else
        LOG_INFO("playerbots", "[Route] {} gives the trip to {} up: {}", bot->GetName(), data.place, why);
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    EventLog::Emit(bot, "Travel", source, data.tripId, seconds);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "abort",
                       WorldLogFormat::AfterPrefix(source, TRAVEL_ABORT_PREFIX), static_cast<float>(seconds),
                       {{"replans", static_cast<float>(data.trip.replans)}}, data.tripId);
    WorldLog::StatusReason const reasonScope("abort");
    botAI->rpgInfo.ChangeToIdle();
}
}  // namespace NewRpgInternal

namespace
{
constexpr uint32 HEARTHSTONE_SPELL = 8690;
}  // namespace

namespace
{
// A route exists only once the graph is ready (the route service answers only then), and it never changes after
TravelRoute::Point GraphNodeAt(std::uint32_t index)
{
    TravelGraphNode const& node = TravelGraph::Instance().GetNode(index);
    return {node.mapId, node.x, node.y, node.z};
}

// WT58: the graph link a walk leg walks now, between waypoints[next - 1] and waypoints[next]; none on the walks
// between the bot, the graph and the goal, and on every other leg
std::pair<std::uint32_t, std::uint32_t> WalkLinkEnds(TravelLegs::Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != TravelRoute::LegType::Walk)
        return {TravelLegs::NO_NODE, TravelLegs::NO_NODE};
    return TravelLegs::StuckEdge(trip);
}

// The links a walk leg walks along: walk links and a wait point's link to its stop (a ride's points are its voyage)
bool WalkableLink(TravelGraphLink const& link)
{
    return link.type == TravelLinkType::Walk || (link.type == TravelLinkType::Transport && !link.object);
}

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
    filter.setExcludeFlags(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
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
// is probed on the navmesh once, from where the bot stands when it starts it (TravelWalk::ProbePiece: the chain of
// mod-playerbots PR #2747, a fresh path per call, on the bot's map thread). The calls ask for the straight path: its
// corners are the points the bot orders, and its point limit counts corners, where the smooth path's 4 yd steps would
// answer a walk of some 300 yd as too long. A walk to a point on an unloaded tile, or on a map without a navmesh, is
// not probed (walked as before); nor is one with an end on a navmesh fragment (NavFragment), which no path leaves.
// Walked: target becomes the probed point to order next. False: the navmesh refused it
bool WalkPieceTarget(Player* bot, TravelLegs::Trip& trip, TravelRoute::Point const& at, TravelRoute::Point& target)
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
        auto const step = [bot](TravelWalk::ProbePoint const& start, TravelWalk::ProbePoint const& end)
        {
            PathGenerator path(bot);
            path.SetUseStraightPath(true);
            TravelWalk::ProbeAnswer answer;
            answer.result = path.CalculatePath(start.x, start.y, start.z, end.x, end.y, end.z, false);
            answer.type = path.GetPathType();
            for (G3D::Vector3 const& point : path.GetPath())
                answer.points.push_back({point.x, point.y, point.z});
            return answer;
        };
        bool const fromFragment = NavTileLoaded(bot, at) && NavFragment(bot, at);
        bool const toFragment = NavTileLoaded(bot, to.at) && NavFragment(bot, to.at);
        if (!NavTileLoaded(bot, at) || !NavTileLoaded(bot, to.at) || fromFragment || toFragment)
            piece = TravelWalk::PieceWalk{};
        else
            piece = TravelWalk::ProbePiece(at, to.at, step);
        if (fromFragment || toFragment)
            LOG_INFO("playerbots",
                     "[Route] {} walk leg {}/{} off the graph from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, "
                     "{:.1f}) not probed: its {} lies on a navmesh fragment",
                     bot->GetName(), trip.legIdx + 1, trip.legs.size(), at.x, at.y, at.z, to.at.x, to.at.y, to.at.z,
                     fromFragment ? (toFragment ? "start and end" : "start") : "end");
        if (piece.probe == TravelWalk::Probe::None)
            piece.probe = TravelWalk::Probe::Walk;  // not probed: no points, the waypoint itself
        piece.leg = trip.legIdx;
        piece.next = trip.next;
        if (piece.Refused())
            LOG_INFO("playerbots",
                     "[Route] {} walk leg {}/{} off the graph from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, "
                     "{:.1f}) refused by the navmesh: {} after {} path calls",
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

constexpr float HOP_LIFT_YARDS = 0.5f;  // the hop lands this far above the deck point (as the off-mesh snap does)
using TravelLifecycle::MotionKind;
using TravelLifecycle::Outcome;
using TravelLifecycle::PassengerMotion;
using TravelLifecycle::Point;
namespace TransportGeometry = TravelTransportGeometry;

// The current leg is a transport leg past its approach: the bot waits at the dock, boards, rides or steps off. The
// follower scans for the transport then, and the trip's deadline waits (the next departure may be minutes away)
bool AtTransportLeg(TravelLegs::Trip const& trip)
{
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport &&
           trip.boarding != TravelLegs::Boarding::Approach;
}

// The leg's transport, found anew on every scan (never kept across ticks): the one the bot rides, else the nearest
// of the leg's entry to its dock on the bot's map. O(transports on the map), once a scan
Transport* FindLegTransport(Player* bot, TravelRoute::Leg const& leg, uint64 expected)
{
    if (Transport* const ridden = bot->GetTransport())
        return ridden->GetEntry() == leg.object ? ridden : nullptr;
    if (expected)
    {
        Transport* const transport = bot->GetMap()->GetTransport(ObjectGuid(expected));
        if (transport && transport->GetEntry() == leg.object && TransportGeometry::Available(*bot, *transport))
            return transport;
    }
    Transport* best = nullptr;
    float bestYards = std::numeric_limits<float>::max();
    auto const consider = [&](Transport* transport)
    {
        if (!transport || transport->GetEntry() != leg.object || !TransportGeometry::Available(*bot, *transport))
            return;
        TravelRoute::Point const& stop = TravelLegs::StopFrom(leg);
        float const yards = transport->IsStaticTransport() ? transport->GetExactDist(stop.x, stop.y, stop.z)
                                                           : transport->GetExactDist2d(stop.x, stop.y);
        if (yards < bestYards)
        {
            best = transport;
            bestYards = yards;
        }
    };
    for (Transport* transport : bot->GetMap()->GetAllTransports())
        consider(transport);
    if (!best)
    {
        // Grid-loaded static transports may be absent from the motion transport set.
        std::list<GameObject*> objects;
        Acore::AllGameObjectsWithEntryInRange check(bot, leg.object, TravelLegs::STATIC_DOCK_YARDS * 2.0f);
        Acore::GameObjectListSearcher<Acore::AllGameObjectsWithEntryInRange> searcher(bot, objects, check);
        Cell::VisitObjects(bot, searcher, TravelLegs::STATIC_DOCK_YARDS * 2.0f);
        for (GameObject* object : objects)
            consider(object->ToTransport());
    }
    return best;
}

// What keeps a bot from a transport move this tick (the board gate's debug word), or nothing
char const* TransportMoveHold(Player const& bot)
{
    if (!bot.movespline->Finalized())
        return "spline";
    if (bot.HasUnitState(UNIT_STATE_NOT_MOVE))
        return "state";
    if (bot.HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
        return "flag";
    if (bot.IsMovementPreventedByCasting())
        return "cast";
    if (bot.IsFlying())
        return "flying";
    if (bot.Unit::IsFalling())
        return "falling";
    return nullptr;
}

bool ReadyForTransportMove(Player const& bot) { return !TransportMoveHold(bot); }

// A ship's or zeppelin's stop node is the vessel's own position (sea level, or a zeppelin's origin about 18 yd above
// its tower floor): the ground beside it within these is where a walk to the stop ends while no quay is known
constexpr float STOP_GROUND_ACROSS_YARDS = 25.0f;
constexpr float STOP_GROUND_VERTICAL_YARDS = 22.0f;

// The nearest ground beside a vessel's stop or position, looked for at the level of the bot that walks there (WT54-5):
// a rider's deck, or the point an approach walk comes from. Looked for at the stop's own height it is the sea floor
// under a berth, or a zeppelin tower's roof or upper ramp instead of the platform 18 yd below the hull's origin.
// Nothing when the tile is not loaded or none lies within reach
std::optional<Point> GroundBesideStop(Map* map, Point const& stop, float level)
{
    Point at{stop.X, stop.Y, level};
    if (NavMeshSnap::Nearest(map, at.X, at.Y, at.Z, STOP_GROUND_ACROSS_YARDS, STOP_GROUND_VERTICAL_YARDS, NAV_GROUND) !=
        NavMeshSnap::Result::Found)
        return std::nullopt;
    return at;
}

// A vessel's deck below its origin (WT54-7), measured on its model at the hull's centre: casts down from the origin,
// each from just below the surface the last one hit. The last surface is the hull's bottom (run 10: Northrend zeppelins
// -8.94/-17.88/-25.51, deck -17.88); the deck is the lowest surface above it with a body's headroom. The model is the
// same at every pose and on every map, so the yards are kept per entry for every map thread; zero for a vessel whose
// stop does not hang above its ground (a ship: its deck lies above its waterline origin) or with no deck found.
constexpr float DECK_HEADROOM_YARDS = 2.0f;
constexpr uint32 DECK_DROP_HITS = 8;

std::mutex deckDropLock;
std::unordered_map<uint32, float> deckDrops;

std::optional<float> KnownDeckDrop(uint32 entry)
{
    std::lock_guard<std::mutex> guard(deckDropLock);
    auto const known = deckDrops.find(entry);
    return known == deckDrops.end() ? std::nullopt : std::optional<float>(known->second);
}

std::optional<float> MeasureDeckDrop(Transport const& transport, uint32 phase)
{
    if (!transport.m_model)
        return std::nullopt;
    Point const centre = TransportGeometry::HullCentre(transport);
    float const low = transport.m_model->GetBounds().low().z;
    float top = centre.Z;
    std::vector<float> surfaces;
    std::string hits;
    while (surfaces.size() < DECK_DROP_HITS && top > low)
    {
        auto const surface =
            TransportGeometry::DeckHeight(transport, phase, {centre.X, centre.Y, top}, 0.0f, top - low + 1.0f);
        if (!surface)
            break;
        hits += Acore::StringFormat("{}{:.2f}", hits.empty() ? "" : "/", *surface - centre.Z);
        surfaces.push_back(*surface);
        top = *surface - 0.05f;
    }
    std::optional<float> deck;
    for (std::size_t i = 0; i + 1 < surfaces.size(); ++i)
        if ((i ? surfaces[i - 1] : centre.Z) - surfaces[i] >= DECK_HEADROOM_YARDS)
            deck = surfaces[i];
    LOG_DEBUG("playerbots", "[TravelLifecycle] deck_drop entry={} origin_z={:.2f} low={:.2f} hits={} deck={}",
              transport.GetEntry(), centre.Z, low, hits.empty() ? std::string("none") : hits,
              deck ? Acore::StringFormat("{:.2f}", *deck - centre.Z) : std::string("none"));
    if (!deck)
        return std::nullopt;
    return centre.Z - *deck;
}

// The approach walk to a ship's or zeppelin's stop ends on the ground beside it, not at the stop (in the berth's
// water, or in the air at a zeppelin's origin), where the arriving hull would take the bot aboard without a boarding.
// A stop that hangs above its ground (a zeppelin's) is waited for at its deck's level (WT54-7): the ground nearest the
// origin is the tower's roof or upper ramp (Tirisfal 113.09, Orgrimmar 62.41), 18 yd above the platform the gondola
// docks at. The deck is measured once the vessel is on the bot's map (here or by any bot); an end aimed before that is
// aimed again once it is known. The walk's point before the stop is no level (WT54-5: a graph node at the tower's foot)
void AimApproachEnd(Player* bot, TravelRoute::Point& end, TravelRoute::Leg const& leg, uint64 expected)
{
    TravelRoute::Point const& stopAt = TravelLegs::StopFrom(leg);
    if (end.mapId != stopAt.mapId)
        return;
    Point const stop{stopAt.x, stopAt.y, stopAt.z};
    bool const atStop = TravelRoute::Distance(end, stopAt) < 0.1f;
    auto drop = KnownDeckDrop(leg.object);
    if (!drop)
        if (Transport* const transport = FindLegTransport(bot, leg, expected))
            if (transport->ToMotionTransport())
            {
                auto const plain = GroundBesideStop(bot->GetMap(), stop, stop.Z);
                if (plain && transport->m_model)
                {
                    // A failed measurement over a hanging stop is not kept: the next scan measures again
                    bool const overGround = plain->Z < stop.Z - TravelLifecycle::DOCK_FLOOR_YARDS;
                    std::optional<float> const measured =
                        overGround ? MeasureDeckDrop(*transport, bot->GetPhaseMask()) : std::optional<float>(0.0f);
                    if (measured)
                    {
                        drop = measured;
                        std::lock_guard<std::mutex> guard(deckDropLock);
                        deckDrops.emplace(leg.object, *drop);
                    }
                }
            }
    bool const hangs = drop && *drop > TravelLifecycle::DOCK_FLOOR_YARDS;
    float const level = hangs ? stop.Z - *drop : stop.Z;
    if (!atStop && (!hangs || std::fabs(end.z - level) <= 2.0f * TravelLifecycle::DOCK_FLOOR_YARDS))
        return;
    auto const ground = GroundBesideStop(bot->GetMap(), stop, level);
    if (!ground || (!atStop && TravelRoute::Distance(end, {end.mapId, ground->X, ground->Y, ground->Z}) < 0.5f))
        return;
    end = {end.mapId, ground->X, ground->Y, ground->Z};
    LOG_DEBUG("playerbots", "[Route] {} walks to {:.2f}, {:.2f}, {:.2f} beside the stop of {} (deck {} below it)",
              bot->GetName(), ground->X, ground->Y, ground->Z, leg.object,
              hangs ? Acore::StringFormat("{:.2f}", *drop) : std::string("not"));
}

// The ground nearest the rider that lies off the docked hull, within a step of the deck's floor (WT54-3). A route that
// ends at the vessel's stop gives no ground off the hull: beside a stop lies the pier under the docked ship, or the
// ground below a zeppelin's tower. The bot then goes on from this ground; the exit's checks stay as they are.
constexpr float OFF_HULL_REACH_YARDS = 40.0f;
constexpr float OFF_HULL_RING_YARDS = 2.0f;
constexpr uint32 OFF_HULL_BEARINGS = 16;

std::optional<Point> GroundOffHull(Player const& bot, Transport const& ridden, bool trace)
{
    TransportGeometry::GroundGeometry const geometry{bot, &ridden};
    Point const from = TransportGeometry::PositionOf(bot);
    constexpr float FULL_TURN = 6.28318531f;
    // Why the candidates failed (traced bots, WT54-4): no ground mesh within a step of the deck's height, ground under
    // the hull, or a mesh point that is not standing ground (water, or the snap left the point)
    uint32 noMesh = 0;
    uint32 onHull = 0;
    uint32 notGround = 0;
    for (float radius = OFF_HULL_RING_YARDS; radius <= OFF_HULL_REACH_YARDS; radius += OFF_HULL_RING_YARDS)
        for (uint32 bearing = 0; bearing < OFF_HULL_BEARINGS; ++bearing)
        {
            float const angle = FULL_TURN * static_cast<float>(bearing) / static_cast<float>(OFF_HULL_BEARINGS);
            Point const seed{from.X + radius * std::cos(angle), from.Y + radius * std::sin(angle), from.Z};
            auto const ground = geometry.SnapGround(seed, OFF_HULL_RING_YARDS / 2.0f, TravelLifecycle::EXIT_STEP_YARDS);
            if (!ground)
                ++noMesh;
            else if (!geometry.OffHull(*ground))
                ++onHull;
            else if (!geometry.OnGround(*ground))
                ++notGround;
            else
                return ground;
        }
    if (trace)
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] ground_off_hull bot={} entry={} at={:.2f},{:.2f},{:.2f} origin_z={:.2f} "
                  "none: no_mesh={} on_hull={} not_ground={}",
                  bot.GetGUID().GetCounter(), ridden.GetEntry(), from.X, from.Y, from.Z, ridden.GetPositionZ(), noMesh,
                  onHull, notGround);
    return std::nullopt;
}

bool PrepareTransportMove(Player& bot)
{
    if (!ReadyForTransportMove(bot))
        return false;
    if (!bot.IsMounted())
        return true;
    WorldPacket emptyPacket;
    bot.GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    return false;
}

PassengerMotion Intent(Player const& bot, Transport const& transport, MotionKind kind, Point const& endpoint)
{
    PassengerMotion motion;
    motion.Kind = kind;
    motion.Transport = transport.GetGUID().GetRawValue();
    motion.Entry = transport.GetEntry();
    motion.Map = bot.GetMapId();
    motion.Endpoint = endpoint;
    motion.Dock = TransportGeometry::PoseOf(transport);
    return motion;
}

// A boarding walks straight onto the deck; an exit walks across the deck by its deck walk's corners to the deck's edge
// (via) and on to the quay. Via points within 0.2 yd of the point before are left out (a spline refuses a segment
// shorter than 0.1 yd)
void IssueTransportMove(Player& bot, NewRpgInfo::Travel& data, PassengerMotion motion,
                        std::vector<Point> const& via = {})
{
    if (bot.IsSitState())
        bot.SetStandState(UNIT_STAND_STATE_STAND);
    bot.GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(&bot);
    if (!via.empty())
    {
        Movement::PointsArray path{{bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ()}};
        for (Point const& point : via)
            if ((G3D::Vector3{point.X, point.Y, point.Z} - path.back()).length() >= 0.2f)
                path.push_back({point.X, point.Y, point.Z});
        G3D::Vector3 const end{motion.Endpoint.X, motion.Endpoint.Y, motion.Endpoint.Z};
        if (path.size() > 1 && (end - path.back()).length() < 0.2f)
            path.back() = end;
        else
            path.push_back(end);
        init.MovebyPath(path);
    }
    else
        init.MoveTo(motion.Endpoint.X, motion.Endpoint.Y, motion.Endpoint.Z, false, false);
    if (init.Launch() <= 0)
        return;
    motion.Spline = bot.movespline->GetId();
    data.passengerMotion = motion;
}

// A ship or zeppelin at a stop it keeps for the attach's deadline (MotionTransport::IsMoving is private: its key
// frames, as MotionTransport::Update reads them)
bool Docked(MotionTransport const& ship)
{
    uint32 const period = ship.GetPeriod();
    if (!period)
        return false;
    uint32 const timer = ship.GetPathProgress() % period;
    for (KeyFrame const& frame : ship.GetKeyFrames())
        if (timer >= frame.ArriveTime && timer + PlayerbotAI::ABOARD_DEADLINE_MS < frame.DepartureTime)
            return true;
    return false;
}

}  // namespace

namespace NewRpgInternal
{
// `route aboard` (tests): the bot is stood on the deck of the docked ship or zeppelin of that entry nearest it on its
// map, at the boarding deck point from where it stands (the test puts it on the dock first). No explicit attach:
// AddPassenger takes the current position as the offset, so PlayerbotAI's passenger check attaches it after the
// teleport (BeginAboard). Refused: absent (none on the map), not_mo (an elevator), away (not at a stop), no_deck
void PlaceAboard(PlayerbotAI* botAI, Player* bot, uint32 entry)
{
    Transport* transport = nullptr;
    float nearest = std::numeric_limits<float>::max();
    for (Transport* candidate : bot->GetMap()->GetAllTransports())
    {
        if (!candidate->IsInWorld() || candidate->GetEntry() != entry)
            continue;
        float const yards = candidate->GetExactDist2d(bot);
        if (yards < nearest)
        {
            transport = candidate;
            nearest = yards;
        }
    }
    MotionTransport const* ship = transport ? transport->ToMotionTransport() : nullptr;
    std::optional<Point> deck;
    char const* refusal = nullptr;
    if (!transport)
        refusal = "absent";
    else if (!ship)
        refusal = "not_mo";
    else if (!Docked(*ship))
        refusal = "away";
    else
    {
        deck = TransportGeometry::GroundGeometry{*bot, transport}.DeckFrom(TransportGeometry::PositionOf(*bot));
        if (!deck ||
            bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), deck->X, deck->Y, deck->Z, bot) != transport)
            refusal = "no_deck";
    }
    if (refusal)
    {
        LOG_INFO("playerbots", "[Route] {} aboard {} refused: {}", bot->GetName(), entry, refusal);
        std::string const source = std::string("refused:") + refusal;
        EventLog::Emit(bot, PlayerbotAI::ABOARD_EVENT_TYPE, source.c_str(), entry);
        return;
    }
    LOG_INFO("playerbots", "[Route] {} aboard {}: deck {:.1f}, {:.1f}, {:.1f}", bot->GetName(), entry, deck->X, deck->Y,
             deck->Z);
    bot->NearTeleportTo(deck->X, deck->Y, deck->Z + HOP_LIFT_YARDS, bot->GetOrientation());
    botAI->BeginAboard(entry);
}
}  // namespace NewRpgInternal

namespace
{

std::optional<Outcome> TakePassengerOutcome(Player& bot, NewRpgInfo::Travel& data)
{
    if (!data.passengerMotion)
        return std::nullopt;
    auto const result =
        TravelLifecycle::PassengerOutcome(*data.passengerMotion, TransportGeometry::ReadMembership(bot));
    if (!result)
        return std::nullopt;
    TransportGeometry::DiscardMotion(bot, data.passengerMotion);
    return result;
}

// The crossing checks' words (TravelLifecycle::CrossingCheck), for the debug log
constexpr std::array<char const*, TravelLifecycle::CrossingTally::REASONS> CROSSING_NAMES = {
    "accepted", "no_shore", "no_deck",  "on_hull",  "height",   "distance",
    "ground",   "onward",   "crossing", "corridor", "deck_walk"};

// A failed exit or quay search, for the debug log: the hull box, the count of each failed check and its first sample
void LogCrossingSearch(Player const& bot, NewRpgInfo::Travel const& data, Transport const& transport, char const* kind,
                       TravelLifecycle::CrossingTally const& tally)
{
    TravelLifecycle::Footprint const hull = TransportGeometry::Hull(bot, transport);
    TransportGeometry::GroundGeometry const geometry{bot, &transport};
    std::string reasons;
    for (std::size_t i = 0; i < CROSSING_NAMES.size(); ++i)
    {
        if (!tally.Counts[i])
            continue;
        auto const& sample = *tally.First[i];
        reasons +=
            Acore::StringFormat(" {}={}@{:.2f},{:.2f},{:.2f}>{:.2f},{:.2f},{:.2f}", CROSSING_NAMES[i], tally.Counts[i],
                                sample.From.X, sample.From.Y, sample.From.Z, sample.To.X, sample.To.Y, sample.To.Z);
        if (i == static_cast<std::size_t>(TravelLifecycle::CrossingCheck::Crossing))
            reasons += Acore::StringFormat(
                "/{}", TransportGeometry::GroundGeometry::BlockName(geometry.CrossingBlock(sample.From, sample.To)));
        if (i == static_cast<std::size_t>(TravelLifecycle::CrossingCheck::Corridor) && tally.Onward)
            reasons += Acore::StringFormat(
                "/{}", TransportGeometry::GroundGeometry::ReachName(geometry.GroundReach(sample.To, *tally.Onward)));
    }
    std::string const onward =
        tally.Onward ? Acore::StringFormat("{:.2f},{:.2f},{:.2f}", tally.Onward->X, tally.Onward->Y, tally.Onward->Z)
                     : std::string("none");
    Point const centre = TransportGeometry::HullCentre(transport);
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] crossing_search bot={} trip={} leg={} kind={} entry={} map={} at={:.2f},{:.2f},{:.2f} "
              "origin={:.2f},{:.2f},{:.2f} o={:.3f} hull={:.1f},{:.1f},{:.1f}..{:.1f},{:.1f},{:.1f} "
              "centre={:.2f},{:.2f} onward={} deck_cells={}{}",
              bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, kind, transport.GetEntry(), bot.GetMapId(),
              bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ(), transport.GetPositionX(),
              transport.GetPositionY(), transport.GetPositionZ(), transport.GetOrientation(), hull.Low.X, hull.Low.Y,
              hull.Low.Z, hull.High.X, hull.High.Y, hull.High.Z, centre.X, centre.Y, onward, tally.DeckCells, reasons);
    // The exit probe: per bearing (degrees from the route's side), the candidate that got furthest through the checks
    for (std::size_t turn = 0; turn < tally.Bearings.size(); ++turn)
    {
        auto const& furthest = tally.Bearings[turn];
        if (!furthest)
            continue;
        int const side = static_cast<int>((turn + 1) / 2) * (turn % 2 ? 1 : -1);
        std::string block;
        if (furthest->Check == TravelLifecycle::CrossingCheck::Crossing)
            block = Acore::StringFormat("/{}", TransportGeometry::GroundGeometry::BlockName(
                                                   geometry.CrossingBlock(furthest->From, furthest->To)));
        else if (furthest->Check == TravelLifecycle::CrossingCheck::Corridor && tally.Onward)
            block = Acore::StringFormat(
                "/{}", TransportGeometry::GroundGeometry::ReachName(geometry.GroundReach(furthest->To, *tally.Onward)));
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] exit_probe bot={} trip={} bearing={} at={:.0f} check={}{} "
                  "from={:.2f},{:.2f},{:.2f} to={:.2f},{:.2f},{:.2f}",
                  bot.GetGUID().GetCounter(), data.tripId,
                  side * static_cast<int>(360 / TravelLifecycle::CROSSING_BEARINGS), furthest->Distance,
                  CROSSING_NAMES[static_cast<std::size_t>(furthest->Check)], block, furthest->From.X, furthest->From.Y,
                  furthest->From.Z, furthest->To.X, furthest->To.Y, furthest->To.Z);
    }
}

// WT43b: the boarding crossing walked back where the transport stands as it did at the boarding (the passenger
// observer confirms it as any exit, ConfirmExit). Its shore may touch the hull (a zeppelin's gondola beside its tower):
// then the walk goes on from the shore, along the crossing's line, to the first point off the hull up to
// BACK_OUT_YARDS out, and that step is the exit's crossing (CheckExit from the shore; the deck-to-shore part is the
// boarding's, checked then at this pose). Onward ground: a step beyond
struct BackPath
{
    Point Deck;
    Point From;  // where the exit's crossing starts: the deck, or the shore when the walk goes on from it
    Point Ground;
    Point Onward;
    std::vector<Point> Walk;  // the deck walk's corners, ending at Deck
};

std::optional<BackPath> BackCrossing(Player const& bot, NewRpgInfo::Travel const& data, Transport const& ridden,
                                     bool trace)
{
    constexpr float ONWARD_YARDS = 4.0f;
    constexpr float BACK_OUT_YARDS = 3.0f;
    auto const walk = TravelLegs::CrossingBack(data.trip.boardCrossing, ridden.GetGUID().GetRawValue(),
                                               TransportGeometry::PoseOf(ridden));
    if (!walk)
        return std::nullopt;
    float const dx = walk->shore.X - walk->deck.X;
    float const dy = walk->shore.Y - walk->deck.Y;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.1f)
        return std::nullopt;
    TransportGeometry::GroundGeometry const geometry{bot, &ridden};
    auto const out = [&](Point const& from, float yards)
    {
        return geometry.SnapGround({from.X + dx / length * yards, from.Y + dy / length * yards, from.Z}, 1.5f,
                                   TravelLifecycle::DOCK_FLOOR_YARDS);
    };
    TravelLifecycle::CrossingCheck check = TravelLifecycle::CrossingCheck::NoShore;
    std::optional<BackPath> path;
    float extra = 0.0f;
    for (; extra <= BACK_OUT_YARDS && !path; extra += 1.0f)
    {
        Point const from = extra > 0.0f ? walk->shore : walk->deck;
        auto const ground = extra > 0.0f ? out(walk->shore, extra) : std::optional<Point>{walk->shore};
        auto const onward = ground ? out(*ground, ONWARD_YARDS) : std::nullopt;
        if (!ground || !onward)
        {
            check = ground ? TravelLifecycle::CrossingCheck::Onward : TravelLifecycle::CrossingCheck::NoShore;
            continue;
        }
        check = TravelLifecycle::CheckExit(geometry, from, *ground, *onward);
        if (check == TravelLifecycle::CrossingCheck::Accepted)
            path = BackPath{walk->deck, from, *ground, *onward};
    }
    if (path)
    {
        std::uint32_t expanded = 0;
        auto deckWalk = geometry.DeckPath(TransportGeometry::PositionOf(bot), walk->deck,
                                          TravelLifecycle::DECK_PATH_CELLS, expanded);
        if (deckWalk)
            path->Walk = std::move(*deckWalk);
        else
        {
            check = TravelLifecycle::CrossingCheck::DeckWalk;
            path.reset();
        }
    }
    if (trace)
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] exit_back bot={} trip={} leg={} entry={} check={} out={:.0f} "
                  "deck={:.2f},{:.2f},{:.2f} shore={:.2f},{:.2f},{:.2f}",
                  bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, ridden.GetEntry(),
                  CROSSING_NAMES[static_cast<std::size_t>(check)], extra - 1.0f, walk->deck.X, walk->deck.Y,
                  walk->deck.Z, walk->shore.X, walk->shore.Y, walk->shore.Z);
    return path;
}

// The exit to a stop's wait point, for the debug log (WT60): the first gate that held it (the crossing checks' words),
// the deck point, and whether the wait point lies off the hull and on ground by the old search's tests
void LogExitGate(Player const& bot, NewRpgInfo::Travel const& data, Transport const& ridden, Point const& wait,
                 std::optional<TravelLifecycle::ExitPath> const& exit, TravelLifecycle::CrossingCheck gate)
{
    TransportGeometry::GroundGeometry const geometry{bot, &ridden};
    std::string deck("none");
    if (exit)
        deck = Acore::StringFormat("{:.2f},{:.2f},{:.2f}", exit->Deck.X, exit->Deck.Y, exit->Deck.Z);
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] exit_gate bot={} trip={} leg={} entry={} at={:.2f},{:.2f},{:.2f} gate={} "
              "wait={:.2f},{:.2f},{:.2f} deck={} wait_off_hull={} wait_ground={} corners={}",
              bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, ridden.GetEntry(), bot.GetPositionX(),
              bot.GetPositionY(), bot.GetPositionZ(), CROSSING_NAMES[static_cast<std::size_t>(gate)], wait.X, wait.Y,
              wait.Z, deck, geometry.OffHull(wait) ? 1 : 0, geometry.OnGround(wait) ? 1 : 0,
              exit ? exit->Walk.size() : 0);
}

// Leaving is boarding in reverse (WT60): at a stop with a wait point the exit crosses from the deck point beside it
// (FindWaitExit). Where it has none, or that crossing fails (the model between the wait point and the deck point
// toward the hull's centre, as at Ratchet and Auberdine's pier), the old search finds ground off the hull toward the
// route (FindExit; GroundOffHull only where no wait point and no route point gives the onward point). A failed exit
// tries the boarding crossing walked back (WT43b), else it is a no_exit and the leg rides back
void WalkOff(Player& bot, NewRpgInfo::Travel& data, Transport const& ridden, std::optional<Point> const& waitPoint,
             std::vector<Point> const& route)
{
    if (data.passengerMotion || data.trip.exitSought || !PrepareTransportMove(bot))
        return;
    bool const trace = !data.place.empty() && sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG);
    TravelLifecycle::CrossingTally tally;
    std::optional<TravelLifecycle::ExitPath> exit;
    if (waitPoint)
    {
        TravelLifecycle::CrossingCheck gate = TravelLifecycle::CrossingCheck::NoDeck;
        if (TransportGeometry::Available(bot, ridden))
            exit = TravelLifecycle::FindWaitExit(TransportGeometry::GroundGeometry{bot, &ridden},
                                                 TransportGeometry::PositionOf(bot), *waitPoint,
                                                 TransportGeometry::HullCentre(ridden), gate);
        if (trace)
            LogExitGate(bot, data, ridden, *waitPoint, exit, gate);
    }
    bool const toWaitPoint = exit.has_value();
    if (!exit)
    {
        std::vector<Point> onward = route;
        if (!waitPoint && !TravelLifecycle::OnwardPoint(TransportGeometry::GroundGeometry{bot, &ridden}, route))
            if (auto const ground = GroundOffHull(bot, ridden, trace))
                onward.push_back(*ground);
        exit = TransportGeometry::FindExit(bot, ridden, onward, trace ? &tally : nullptr);
        if (!exit && trace)
            LogCrossingSearch(bot, data, ridden, "exit", tally);
    }
    if (exit)
    {
        if (trace)
        {
            std::string walk;
            for (std::size_t i = 0; i + 1 < exit->Walk.size(); ++i)
                walk += Acore::StringFormat("{}{:.1f},{:.1f},{:.1f}", walk.empty() ? "" : "/", exit->Walk[i].X,
                                            exit->Walk[i].Y, exit->Walk[i].Z);
            LOG_DEBUG("playerbots",
                      "[TravelLifecycle] exit_path bot={} trip={} leg={} entry={} map={} at={:.2f},{:.2f},{:.2f} "
                      "deck={:.2f},{:.2f},{:.2f} ground={:.2f},{:.2f},{:.2f} onward={:.2f},{:.2f},{:.2f} "
                      "corners={} cells={}",
                      bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, ridden.GetEntry(), bot.GetMapId(),
                      bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ(), exit->Deck.X, exit->Deck.Y,
                      exit->Deck.Z, exit->Ground.X, exit->Ground.Y, exit->Ground.Z, exit->Onward.X, exit->Onward.Y,
                      exit->Onward.Z, walk.empty() ? std::string("none") : walk, tally.DeckCells);
        }
        PassengerMotion motion = Intent(bot, ridden, MotionKind::Exit, exit->Ground);
        motion.ExitFrom = exit->Deck;
        motion.Onward = exit->Onward;
        motion.ToWaitPoint = toWaitPoint;
        IssueTransportMove(bot, data, motion, exit->Walk);
        return;
    }
    if (auto const back = BackCrossing(bot, data, ridden, trace))
    {
        PassengerMotion motion = Intent(bot, ridden, MotionKind::Exit, back->Ground);
        motion.ExitFrom = back->From;
        motion.Onward = back->Onward;
        std::vector<Point> via = back->Walk;
        via.push_back(back->From);
        IssueTransportMove(bot, data, motion, via);
        return;
    }
    data.trip.exitSought = true;
    data.trip.exitless = true;
    EventLog::Emit(&bot, TravelLegs::TRANSPORT_EVENT_TYPE,
                   TravelLegs::TransportSource(TravelLegs::TransportEvent::NoExit), ridden.GetEntry(), 0);
}

// WT55b, data for the navigation plan's NV18 (it goes with the searches, WT.47 and WT70): at a stop with a wait
// point the old searches decide nothing. Once per vessel, stop and role in a server run they still run for a traced
// bot, and a dock_foot line says where they would have put the crossing's foot. A bounded set (vessels x stops x 2
// roles) the map threads share; the board role runs in the rate-limited transport scan, the exit role at the exit
// search
std::mutex dockFootLock;
std::unordered_set<uint64> dockFeet;

bool TracedDockFoot(NewRpgInfo::Travel const& data, uint32 entry, uint32 stopNode, bool exit)
{
    if (data.place.empty() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return false;
    uint64 const key = (uint64(entry) << 32) | (uint64(stopNode) << 1) | (exit ? 1u : 0u);
    std::lock_guard<std::mutex> guard(dockFootLock);
    return dockFeet.insert(key).second;
}

std::string DockFootPoint(std::optional<Point> const& at)
{
    return at ? Acore::StringFormat("{:.2f},{:.2f},{:.2f}", at->X, at->Y, at->Z) : std::string("none");
}

void LogDockFoot(TravelRoute::Leg const& leg, bool exit, std::optional<Point> const& foot,
                 std::optional<Point> const& deck, char const* firstFail)
{
    TravelGraph const& graph = TravelGraph::Instance();
    uint32 const stopNode = leg.nodes[exit ? leg.rideAt + 1 : leg.rideAt];
    uint32 const waitNode = exit ? leg.nodes.back() : leg.nodes.front();
    TravelRoute::Point const& waitAt = exit ? leg.to : leg.from;
    TravelRoute::Point const& stopAt = exit ? TravelLegs::StopTo(leg) : TravelLegs::StopFrom(leg);
    Point const wait{waitAt.x, waitAt.y, waitAt.z};
    auto const flat = [](Point const& a, Point const& b) { return std::hypot(a.X - b.X, a.Y - b.Y); };
    auto const full = [](Point const& a, Point const& b)
    { return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z)); };
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] dock_foot entry={} stop={} stopname='{}' wait={} role={} wait_at={} foot={} deck={} "
              "d_wait_foot={:.1f} d_foot_deck={:.1f} d_wait_stop={:.1f} first_fail={}",
              leg.object, graph.GetNode(stopNode).id, graph.GetName(stopNode), graph.GetNode(waitNode).id,
              exit ? "exit" : "board", DockFootPoint(wait), DockFootPoint(foot), DockFootPoint(deck),
              foot ? flat(wait, *foot) : -1.0f, foot && deck ? full(*foot, *deck) : -1.0f,
              flat(wait, Point{stopAt.x, stopAt.y, stopAt.z}), firstFail);
}

// The exit role: the ground off the hull the rider would have stepped onto, from the deck at the far stop
void LogExitDockFoot(Player& bot, NewRpgInfo::Travel const& data, Transport const& ridden)
{
    TravelRoute::Leg const& leg = data.trip.legs[data.trip.legIdx];
    if (!leg.waitTo || leg.rideAt + 1 >= leg.nodes.size() || data.passengerMotion || data.trip.exitSought ||
        !TracedDockFoot(data, leg.object, leg.nodes[leg.rideAt + 1], true))
        return;
    std::optional<Point> const foot = GroundOffHull(bot, ridden, true);
    LogDockFoot(leg, true, foot, TransportGeometry::PositionOf(bot), "none");
}

// Where the bot goes after a transport's quay, for the exit search: the far stop's wait point where the leg has one,
// then the next walk leg's waypoints after its start (its graph nodes, then its end), or the start of the next leg of
// another kind; the trip's destination when the next leg lies on another map or there is none. deckZ: the rider's
// floor, the level the ground beside the stop is looked for at
std::vector<Point> OnwardRoute(TravelLegs::Trip const& trip, Map* map, uint32 mapId, Point const& dest, float deckZ)
{
    std::vector<Point> route;
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    if (leg.waitTo && leg.to.mapId == mapId)
        route.push_back({leg.to.x, leg.to.y, leg.to.z});
    if (trip.legIdx + 1 < trip.legs.size())
    {
        TravelRoute::Leg const& onward = trip.legs[trip.legIdx + 1];
        if (onward.type == TravelRoute::LegType::Walk)
        {
            std::vector<TravelLegs::Waypoint> const points = TravelLegs::Waypoints(onward, GraphNodeAt);
            for (std::size_t i = 1; i < points.size(); ++i)
                if (points[i].at.mapId == mapId)
                    route.push_back({points[i].at.x, points[i].at.y, points[i].at.z});
        }
        else if (onward.from.mapId == mapId)
            route.push_back({onward.from.x, onward.from.y, onward.from.z});
    }
    if (route.empty())
        route.push_back(dest);
    // A point at the leg's end node is the vessel's own stop (in the berth's water, or in the air at a zeppelin's
    // origin), with no ground to walk on: the ground beside it instead, as for the approach walk (SeekQuay). A leg
    // that ends at a wait point ends on walkable ground
    TravelRoute::Point const& end = leg.to;
    for (Point& point : route)
        if (!leg.waitTo && end.mapId == mapId &&
            TravelRoute::Distance(end, {mapId, point.X, point.Y, point.Z}) <= TravelLegs::SAME_POINT_YARDS)
            point = GroundBesideStop(map, point, deckZ).value_or(point);
    return route;
}

// An aboard scan short of the leave, for the debug log: what the dock test at the leg's end saw
void LogAboardScan(Player* bot, TravelLegs::Trip const& trip, float moved)
{
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    TravelLegs::TransportSight const& sight = trip.sight;
    TravelRoute::Point const& stop = TravelLegs::StopTo(leg);
    float const dx = sight.at.x - stop.x;
    float const dy = sight.at.y - stop.y;
    LOG_DEBUG("playerbots",
              "[Route] {} aboard {} on map {}: {:.1f} yd (2D) from the leg's end on map {}, still {}, moving {}, "
              "static {}, {:.1f} yd moved since the last scan (-1: none)",
              bot->GetName(), leg.object, sight.at.mapId, std::sqrt(dx * dx + dy * dy), leg.to.mapId, trip.still,
              sight.moving, sight.isStatic, moved);
}

float SeeTransport(Player* bot, Transport* transport, TravelLegs::Trip const& trip, TravelLegs::TransportSight& sight);

uint32 MotionQuayLeg(TravelLegs::Trip const& trip)
{
    uint32 index = trip.legIdx;
    if (index >= trip.legs.size())
        return TravelLegs::NO_NODE;
    TravelRoute::Leg const& current = trip.legs[index];
    if (current.type == TravelRoute::LegType::Walk && index + 1 < trip.legs.size())
    {
        TravelRoute::Point const& dock = trip.legs[++index].from;
        if (current.to.mapId != dock.mapId || TravelRoute::Distance(current.to, dock) > 1.0f)
            return TravelLegs::NO_NODE;
    }
    TravelRoute::Leg const& leg = trip.legs[index];
    // A stop with a wait point is waited at there (WT.42): no quay to look for
    if (leg.type != TravelRoute::LegType::Transport || leg.waitFrom)
        return TravelLegs::NO_NODE;
    GameObjectTemplate const* const info = sObjectMgr->GetGameObjectTemplate(leg.object);
    return info && info->type == GAMEOBJECT_TYPE_MO_TRANSPORT ? index : TravelLegs::NO_NODE;
}

void SeekQuay(Player* bot, NewRpgInfo::Travel& data, TravelLegs::Observation& seen)
{
    TravelLegs::Trip& trip = data.trip;
    if (trip.legIdx >= trip.legs.size() || trip.boarding != TravelLegs::Boarding::Approach)
        return;
    // At its transport's dock the bot waits on foot (WT54-7; TravelKeepsOnFoot keeps it so): Thunder Bluff's zeppelin
    // left while the bot spent the scan after its docking on the dismount. A flying mount's flag stays set on the
    // ground (WT54-11: Ditlis at Valiance Keep, held as "flying" for 800 s): only a bot in the air keeps its mount
    auto const airborne = [bot]
    {
        float const ground = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        return bot->IsFlying() && bot->GetPositionZ() - ground > TravelLifecycle::DOCK_FLOOR_YARDS;
    };
    if (trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport && bot->IsMounted() && !bot->IsInCombat() &&
        !airborne())
    {
        WorldPacket emptyPacket;
        bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    }

    // A stop with a wait point: StepTransport waits and crosses there, no search
    if (trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport && trip.legs[trip.legIdx].waitFrom)
        return;
    if (trip.hasQuay || getMSTimeDiff(data.transportScanMs, seen.nowMs) < TravelLegs::TRANSPORT_SCAN_MS)
        return;
    if (seen.quayLeg == TravelLegs::NO_NODE)
    {
        if (trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport && !trip.quaySought)
            TravelLegs::SetQuay(trip, nullptr);
        return;
    }

    bool const approachWalk = seen.quayLeg != trip.legIdx;
    TravelRoute::Leg const& leg = trip.legs[seen.quayLeg];
    if (bot->GetMapId() != leg.from.mapId)
        return;

    data.transportScanMs = seen.nowMs;
    // The approach walk ends on the ground beside the stop (AimApproachEnd); once docked, the quay
    if (approachWalk && !trip.waypoints.empty())
        AimApproachEnd(bot, trip.waypoints.back().at, leg, trip.sight.guid);
    if (approachWalk && (trip.waypoints.empty() || trip.next + 1 < trip.waypoints.size()))
        return;
    TravelLegs::TransportSight sight;
    Transport* const transport = FindLegTransport(bot, leg, trip.sight.guid);
    if (transport)
        SeeTransport(bot, transport, trip, sight);
    TravelLegs::NoteSight(trip, sight);
    // A traced bot at its transport leg that does not look for its quay this scan, for the debug log (WT54-11: walk
    // done, no quay in 800 s at Valiance Keep): the reason, the scan's still verdict and the vessel's yards to the dock
    auto const holdQuay = [&](char const* reason)
    {
        if (approachWalk || data.place.empty() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
            return;
        float const dock =
            transport ? transport->GetExactDist2d(TravelLegs::StopFrom(leg).x, TravelLegs::StopFrom(leg).y) : -1.0f;
        if (transport && !trip.still && dock > TravelLegs::SHIP_DOCK_YARDS)
            return;  // under way far off: the common case of a wait
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] quay_hold bot={} trip={} leg={} entry={} reason={} still={} dock={:.1f} "
                  "at={:.2f},{:.2f},{:.2f}",
                  bot->GetGUID().GetCounter(), data.tripId, trip.legIdx, leg.object, reason, trip.still ? 1 : 0, dock,
                  bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    };
    if (!transport || !TravelLegs::DockedAt(trip, TravelLegs::StopFrom(leg)))
    {
        trip.quaySought = false;
        holdQuay(transport ? "undocked" : "no_transport");
        return;
    }
    if (trip.quaySought || !ReadyForTransportMove(*bot))
    {
        char const* const hold = TransportMoveHold(*bot);
        holdQuay(trip.quaySought ? "sought" : (hold ? hold : "hold"));
        return;
    }

    TransportGeometry::GroundGeometry const geometry{*bot, transport};
    Point from = TransportGeometry::PositionOf(*bot);
    if (!approachWalk)
    {
        // A bot whose approach walk ended before the deck's level was known waits off that level (WT54-10:
        // Orgrimmar's upper ramp 62.41, the platform 52.0): its quay is looked for from the ground beside the stop at
        // the deck's level, and the walk to the quay takes it there. Once per docking, so no cost per scan
        TravelRoute::Point seed{leg.from.mapId, from.X, from.Y, from.Z};
        AimApproachEnd(bot, seed, leg, trip.sight.guid);
        from = Point{seed.x, seed.y, seed.z};
    }
    bool const trace = !data.place.empty() && sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG);
    TravelLifecycle::CrossingTally tally;
    auto const shore = TravelLifecycle::FindQuay(geometry, from, TransportGeometry::PositionOf(*transport),
                                                 TransportGeometry::HullCentre(*transport), trace ? &tally : nullptr);
    if (!shore)
    {
        TravelLegs::SetQuay(trip, nullptr);
        LOG_DEBUG("playerbots", "[Route] {} found no reachable boarding floor for {}", bot->GetName(), leg.object);
        if (trace)
        {
            LogCrossingSearch(*bot, data, *transport, "quay", tally);
            // The first quay that failed only on the walk to it, and how the ground mesh joins the bot to it (WT54-7)
            auto const& corridor = tally.First[static_cast<std::size_t>(TravelLifecycle::CrossingCheck::Corridor)];
            if (corridor)
                LOG_DEBUG("playerbots",
                          "[TravelLifecycle] quay_reach bot={} at={:.2f},{:.2f},{:.2f} shore={:.2f},{:.2f},{:.2f} "
                          "reach={}",
                          bot->GetGUID().GetCounter(), from.X, from.Y, from.Z, corridor->From.X, corridor->From.Y,
                          corridor->From.Z,
                          TransportGeometry::GroundGeometry::ReachName(geometry.GroundReach(from, corridor->From)));
        }
        return;
    }

    TravelRoute::Point const quay{leg.from.mapId, shore->X, shore->Y, shore->Z};
    TravelLegs::SetQuay(trip, &quay);
    if (approachWalk)
        trip.waypoints.back().at = quay;
    LOG_DEBUG("playerbots", "[Route] {} waits for transport {} on ground at {:.2f}, {:.2f}, {:.2f}", bot->GetName(),
              leg.object, quay.x, quay.y, quay.z);
}

// The board role of the dock_foot line (LogDockFoot): the quay FindQuay finds from the wait point, with the vessel
// docked, and the deck point from there
void LogBoardDockFoot(Player* bot, NewRpgInfo::Travel const& data, Transport& transport)
{
    TravelLegs::Trip const& trip = data.trip;
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    if (!leg.waitFrom || leg.rideAt + 1 >= leg.nodes.size() ||
        !TracedDockFoot(data, leg.object, leg.nodes[leg.rideAt], false))
        return;
    TravelRoute::Point seed = leg.from;
    AimApproachEnd(bot, seed, leg, trip.sight.guid);
    Point const from{seed.x, seed.y, seed.z};
    TransportGeometry::GroundGeometry const geometry{*bot, &transport};
    TravelLifecycle::CrossingTally tally;
    auto const foot = TravelLifecycle::FindQuay(geometry, from, TransportGeometry::PositionOf(transport),
                                                TransportGeometry::HullCentre(transport), &tally);
    char const* firstFail = "none";
    for (std::size_t i = 0; !foot && i < CROSSING_NAMES.size(); ++i)
        if (tally.Counts[i])
        {
            firstFail = CROSSING_NAMES[i];
            break;
        }
    LogDockFoot(leg, false, foot, geometry.DeckFrom(foot ? *foot : from), firstFail);
}

// Where a boarding starts (WT54-10): the quay the bot waits at, whose crossing FindQuay accepted, else where it
// stands. A bot within QUAY_REACH_YARDS of its quay does not walk to it, and from its own spot the crossing can differ
// (run 12, Orgrimmar's platform: accepted from the quay, blocked 0.6 yd beside it), so the board check, its deck point
// and the walk all start at the quay
Point BoardStart(Player const& bot, TravelLegs::Trip const& trip)
{
    if (trip.hasQuay && trip.quay.mapId == bot.GetMapId())
        return Point{trip.quay.x, trip.quay.y, trip.quay.z};
    return TransportGeometry::PositionOf(bot);
}

// Where the boarding starts, for the debug log (WT55e): the stop's wait point, a quay a search found (from the wait
// point after its crossing was refused, or at a stop without one), or where the bot stands
char const* BoardStartName(TravelLegs::Trip const& trip)
{
    if (!trip.hasQuay)
        return "bot";
    bool const atWait = trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].waitFrom && !trip.searchedQuay;
    return atWait ? "wait" : "quay";
}

// A stop's wait point whose crossing the board check refuses (WT55e, WT.42: no deck point, the deck's height, the
// crossing): the old quay search runs from the wait point, as at a stop without one, and the quay it finds is the
// boarding's start (BoardStart); the bot walks there first. Where the check accepts, nothing changes and no search
// runs. Once per docking (Trip::boardChecked, cleared when the vessel leaves). Returns whether the bot walks to a quay
bool SeekBoardQuay(Player* bot, NewRpgInfo::Travel& data, Transport& transport)
{
    TravelLegs::Trip& trip = data.trip;
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    if (!leg.waitFrom || trip.boardChecked || !TransportGeometry::Available(*bot, transport))
        return false;
    bool const trace = !data.place.empty() && sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG);
    TravelLifecycle::CrossingTally tally;
    Point const wait{leg.from.x, leg.from.y, leg.from.z};
    TravelLifecycle::BoardChoice const choice = TravelLifecycle::BoardFrom(
        TransportGeometry::GroundGeometry{*bot, &transport}, wait, TransportGeometry::PositionOf(transport),
        TransportGeometry::HullCentre(transport), trace ? &tally : nullptr);
    TravelRoute::Point const quay{leg.from.mapId, choice.Start.X, choice.Start.Y, choice.Start.Z};
    TravelLegs::NoteBoardCheck(trip, choice.Found ? &quay : nullptr);
    if (!trace || !choice.Searched)
        return choice.Found;
    std::string const found =
        choice.Found ? Acore::StringFormat("{:.2f},{:.2f},{:.2f}", quay.x, quay.y, quay.z) : std::string("none");
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] board_search bot={} trip={} leg={} entry={} gate={} wait={:.2f},{:.2f},{:.2f} "
              "quay={} d={:.1f}",
              bot->GetGUID().GetCounter(), data.tripId, trip.legIdx, transport.GetEntry(),
              CROSSING_NAMES[static_cast<std::size_t>(choice.Gate)], wait.X, wait.Y, wait.Z, found,
              std::sqrt(TravelLifecycle::DistanceSquared(wait, choice.Start)));
    if (!choice.Found)
        LogCrossingSearch(*bot, data, transport, "quay", tally);
    return choice.Found;
}

// A Board walk, for the debug log: the deck point, the bot's height and the distance, and where the boarding starts
void LogBoardMove(Player* bot, uint32 entry, Point const& deck, char const* start)
{
    if (!sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    LOG_DEBUG("playerbots",
              "[Route] {} boards transport {}: deck point ({:.1f}, {:.1f}, {:.1f}), bot z {:.1f}, {:.1f} yd, start {}",
              bot->GetName(), entry, deck.X, deck.Y, deck.Z, bot->GetPositionZ(),
              bot->GetExactDist(deck.X, deck.Y, deck.Z), start);
}

// A scan at a still transport that does not board a traced bot, for the debug log: the first gate that holds it back.
// wait_z: off the wait point's floor; no_deck: no deck point; deck_z: the deck point beyond a floor's step from the
// bot; ground: not on ground; prepare: PrepareTransportMove (mounted, casting); accept: the crossing check. start:
// where the boarding starts (BoardStartName)
void LogBoardGate(Player* bot, NewRpgInfo::Travel const& data, Transport const& transport,
                  std::optional<Point> const& deck, bool ready, bool accept)
{
    TravelLegs::Trip const& trip = data.trip;
    if (data.place.empty() || trip.legIdx >= trip.legs.size() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    float const waitDz = std::fabs(bot->GetPositionZ() - TravelLegs::WaitPoint(trip, trip.legs[trip.legIdx]).z);
    char const* gate = "accept";
    if (!deck)
        gate = waitDz > TravelLifecycle::DOCK_FLOOR_YARDS ? "wait_z" : "no_deck";
    else if (!ready)
        gate = std::fabs(deck->Z - bot->GetPositionZ()) > TravelLifecycle::DOCK_FLOOR_YARDS ? "deck_z" : "ground";
    else if (!accept)
        gate = "prepare";
    std::string const point =
        deck ? Acore::StringFormat("{:.2f},{:.2f},{:.2f}", deck->X, deck->Y, deck->Z) : std::string("none");
    char const* const hold = TransportMoveHold(*bot);
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] board_gate bot={} trip={} leg={} entry={} at={:.2f},{:.2f},{:.2f} gate={} "
              "wait_dz={:.2f} deck={} mounted={} hold={} start={}",
              bot->GetGUID().GetCounter(), data.tripId, trip.legIdx, transport.GetEntry(), bot->GetPositionX(),
              bot->GetPositionY(), bot->GetPositionZ(), gate, waitDz, point, bot->IsMounted() ? 1 : 0,
              hold ? hold : "none", BoardStartName(trip));
}

// A miss this tick, for the debug log: whether the bot stood over the hull (the passenger check's test) or beside it
void LogMisses(Player* bot, TravelLegs::Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    for (TravelLegs::TransportNote const& note : trip.transportNotes)
    {
        if (note.event != TravelLegs::TransportEvent::Missed)
            continue;
        Transport* const transport = FindLegTransport(bot, trip.legs[trip.legIdx], trip.sight.guid);
        bool const onHull = transport && bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(),
                                                                           bot->GetPositionY(), bot->GetPositionZ(),
                                                                           nullptr) == transport;
        LOG_DEBUG("playerbots", "[Route] {} missed transport {} (miss {}): {}, {:.1f} yd from its centre, bot z {:.1f}",
                  bot->GetName(), note.entry, note.amount, onHull ? "over the hull" : "off the hull",
                  transport ? bot->GetExactDist2d(transport) : -1.0f, bot->GetPositionZ());
        return;
    }
}

// A scan of the transport: where it is, whether a ship or zeppelin moved since the scan before (trip.sight), its
// period. Returns the yards moved since that scan on this map, -1 none
float SeeTransport(Player* bot, Transport* transport, TravelLegs::Trip const& trip, TravelLegs::TransportSight& sight)
{
    float moved = -1.0f;
    sight.found = true;
    sight.guid = transport->GetGUID().GetRawValue();
    sight.isStatic = transport->IsStaticTransport();
    sight.at = {bot->GetMapId(), transport->GetPositionX(), transport->GetPositionY(), transport->GetPositionZ()};
    TravelLegs::TransportSight const& before = trip.sight;
    if (before.found && before.guid == sight.guid && before.at.mapId == sight.at.mapId)
        moved = TravelRoute::Distance(before.at, sight.at);
    if (MotionTransport const* ship = transport->ToMotionTransport())
    {
        // MotionTransport::IsMoving is private: a ship or zeppelin is under way when it moved since the scan before
        // (on the first scan it counts as under way, so no docking is read from one position)
        sight.moving = moved < 0.0f || moved > TravelLegs::STILL_YARDS;
        sight.periodMs = ship->GetPeriod();
    }
    else if (StaticTransport const* fixed = transport->ToStaticTransport())
        sight.periodMs = fixed->GetPeriod();
    return moved;
}

void SendTransportNotes(Player* bot, TravelLegs::Trip& trip)
{
    for (TravelLegs::TransportNote const& note : trip.transportNotes)
        EventLog::Emit(bot, TravelLegs::TRANSPORT_EVENT_TYPE, TravelLegs::TransportSource(note.event), note.entry,
                       note.amount);
    trip.transportNotes.clear();
}

// The gate before a trip's end, a route request or a walk (WT19-d, TravelLegs::BeginOffFirst): true when the bot
// rides a ship or zeppelin and gets off first (`Transport off_first`)
bool OffFirstBegins(Player* bot, NewRpgInfo::Travel& data, TravelLegs::OffFirstEnd end = TravelLegs::OffFirstEnd::None,
                    char const* why = nullptr, char const* source = nullptr)
{
    Transport const* const ridden = bot->GetTransport();
    if (!ridden)
        return false;
    TravelLegs::Trip& trip = data.trip;
    if (trip.offFirst.sinceMs)
        return true;
    uint32 const period = TransportGeometry::Period(*ridden);
    if (!TravelLegs::BeginOffFirst(trip, ridden->GetEntry(), ridden->GetGUID().GetRawValue(), period, getMSTime(), end,
                                   why, source))
        return false;
    TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
    SendTransportNotes(bot, trip);
    char const* const waiting = end == TravelLegs::OffFirstEnd::Arrive  ? "arrival"
                                : end == TravelLegs::OffFirstEnd::Abort ? "abort"
                                                                        : "walk or route";
    LOG_DEBUG("playerbots", "[Route] {} gets off transport {} first (the {} waits)", bot->GetName(), ridden->GetEntry(),
              waiting);
    return true;
}

// OffFirst shares the normal local-frame exit, including membership acknowledgment.
TravelLegs::OffStep GetOffFirst(Player* bot, NewRpgInfo::Travel& data)
{
    TravelLegs::Trip& trip = data.trip;
    Transport* const ridden = bot->GetTransport();
    TravelLegs::Observation seen;
    seen.nowMs = getMSTime();
    seen.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    auto const membership = TransportGeometry::ReadMembership(*bot);
    seen.aboardGuid = membership.Guid;
    seen.membershipAvailable = membership.Available;
    if (membership.Available)
        seen.passengerOutcome = TakePassengerOutcome(*bot, data);
    if (getMSTimeDiff(data.transportScanMs, seen.nowMs) >= TravelLegs::TRANSPORT_SCAN_MS)
    {
        data.transportScanMs = seen.nowMs;
        seen.transport.scanned = true;
        if (ridden && membership.Available && membership.Guid == trip.offFirst.guid)
            SeeTransport(bot, ridden, trip, seen.transport);
    }
    bool const wasStill = trip.still;
    TravelLegs::OffStep const step = TravelLegs::StepOffFirst(trip, seen);
    if (seen.transport.scanned && ridden && membership.Available)
    {
        if (step == TravelLegs::OffStep::Disembark)
        {
            // Off where it stands, onto the ground beside the vessel: the trip's goal may be another vessel stop
            // (no ground) or on another map, and the pending operation plans from the shore
            Point const onward =
                GroundBesideStop(bot->GetMap(), TransportGeometry::PositionOf(*ridden), bot->GetPositionZ())
                    .value_or(TransportGeometry::PositionOf(*bot));
            WalkOff(*bot, data, *ridden, std::nullopt, std::vector<Point>{onward});
        }
        else if (step == TravelLegs::OffStep::Wait && wasStill)
        {
            trip.transportNotes.push_back({TravelLegs::TransportEvent::StuckAboard, ridden->GetEntry(), 0});
            trip.exitSought = false;
            TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
        }
    }
    SendTransportNotes(bot, trip);
    return step;
}

// The last-resort hop (WT.14): a few yards onto the docked deck, only unseen (the scan's sight check), counted (its
// Transport hop event went with the trip's notes; the honesty row for `wlog report honesty`)
void HopOnto(Player* bot, NewRpgInfo::Travel& data, Transport const& transport, Point deck, uint32 misses)
{
    deck.Z += HOP_LIFT_YARDS;
    PassengerMotion motion = Intent(*bot, transport, MotionKind::Board, deck);
    motion.ContactOnly = true;
    bot->GetMotionMaster()->Clear();
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    if (bot->TeleportTo(bot->GetMapId(), deck.X, deck.Y, deck.Z, bot->GetOrientation()))
    {
        data.passengerMotion = motion;
        EventLog::Honesty(bot, TravelLegs::HOP_HONESTY_KIND, TravelLegs::HOP_HONESTY_SOURCE, motion.Entry, misses);
    }
}

// A walk leg's end (WT.23): its trip decision row, walk_leg when done or walk_leg_failed (how the bot went: foot, part
// or mounted; its seconds in combat; its seconds against the estimate), and in the debug log its speed: its path
// through the waypoints, its straight line, the time taken
void LogWalkLeg(Player* bot, NewRpgInfo::Travel const& data, uint32 index, TravelLegs::LegRecord const& record,
                uint32 nowMs, uint32 elapsedMs, char const* how, bool done)
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

// A graph link's walk (WT58): its trip decision row, walk_link once the walk moved on past it or walk_link_failed when
// the leg failed on it: how (stored, nodes, fallback, refused), its moves, its ends' node ids and its hops
void LogWalkLink(Player* bot, NewRpgInfo::Travel const& data, TravelWalk::LinkWalk const& walk, bool done)
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
void LogFlyLeg(Player* bot, NewRpgInfo::Travel const& data, uint32 index, TravelLegs::LegRecord const& record,
               uint32 nowMs, uint32 elapsedMs, uint16 climbs, float peakZ, char const* how, bool done,
               char const* why = nullptr)
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
// CastFlyingMount; the engine's "check mount state" refused a bot standing still on open ground, WU11b). A try in
// combat, while casting or where the bot cannot take off is skipped and does not count (the follower asks again; the
// last fails the leg at once); so is a try while the bot moves or sits: the cast checks refuse it in the tick of the
// stop (WU11b: Edhoon's "cast result 255, moving true"), so it stops or stands up now and casts on a later tick. A
// counted try that starts no cast leaves its word for the fly_leg_failed row (no_strategy: no mount strategy;
// no_mount: no flying mount known; refused: the cast check refused every one)
bool MountToFly(Player* bot, PlayerbotAI* botAI, TravelLegs::Trip& trip, uint32 nowMs)
{
    if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(true) || CheckMountStateAction::NoTakeoffHere(bot))
        return true;
    if (bot->isMoving() || !bot->IsStandState())
    {
        if (bot->isMoving())
            bot->StopMoving();
        if (!bot->IsStandState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        return true;
    }
    TravelLegs::NoteMountTry(trip, nowMs);
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
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

// The world log's route decision (only when it is on): the leg chain without no-op walks, why it was searched,
// its seconds; weighed against a straight walk at run speed (when the chain is not one walk on the same map) and its
// leg count. The route service returns one route, so no other route is searched for the row.
void LogRouteDecision(Player* bot, NewRpgInfo::Travel const& data, TravelRoute::Result const& route)
{
    WorldLogFormat::WordChain chain;
    uint32 legs = 0;
    bool walksOnly = true;
    for (TravelRoute::Leg const& leg : route.legs)
    {
        if (TravelLegs::IsNoOp(leg))
            continue;
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

// The bot's place as the travel rules see it
TravelRoute::Point BotPoint(Player* bot)
{
    return {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
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
bool PassengerOwnsTravel(Player const& bot, NewRpgInfo::Travel const& data)
{
    return data.PassengerOwnsProgress() || bot.GetTransport() || bot.HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) ||
           bot.IsInFlight() || bot.GetVehicle();
}
}  // namespace

bool TravelKeepsOnFoot(PlayerbotAI* botAI)
{
    // An errand stop's tame goes on foot (Tame Beast is not cast mounted; WU21b-2), and so does its stable visit (a
    // mounted hunter's pet is away, WU21c run 1: a remount after each dismount kept the visit waiting)
    if (auto const* const errands = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data))
        return errands->arrived && errands->stop < errands->stops.size() &&
               (errands->stops[errands->stop].tameEntry || errands->stops[errands->stop].stableEntry);
    auto const* const data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (!data)
        return false;
    TravelLegs::Trip const& trip = data->trip;
    if (trip.offFirst.sinceMs)
        return true;
    // A transport leg goes on foot from its dock on (WT54-7: also while it waits there, SeekQuay)
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport;
}

bool NewRpgTravelAction::ConsumeScheduledWork(PlayerbotAI* botAI)
{
    using TravelProgress::Overdue;
    using TravelProgress::Work;
    auto* const data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    Overdue const reason = data ? data->progress.Reason() : Overdue::None;
    bool const lootExpired = botAI->LootInteractionExpired();
    if (reason == Overdue::None && !lootExpired)
        return false;

    Player* const bot = botAI->GetBot();
    bool const walkFailure = reason == Overdue::Walk || reason == Overdue::OffMesh || reason == Overdue::RouteDeadline;
    if (reason != Overdue::None &&
        ((walkFailure && PassengerOwnsTravel(*bot, *data)) || !TransportGeometry::ReadMembership(*bot).Available))
        return false;

    bool const canMove = bot->IsAlive() && !bot->IsInCombat() && botAI->CanMove();
    Work const work = TravelProgress::ScheduledWork(canMove, reason, botAI->HasLootInteraction(), lootExpired);
    if (work == Work::Recover)
        return NewRpgTravelAction(botAI).StepTravel(data, data->progress.TakeReason());
    return work == Work::DeferLoot && botAI->ConsumeLootExpiry();
}

bool NewRpgTravelAction::Execute(Event /*event*/)
{
    auto* const data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (!data || !bot->IsAlive() || bot->IsInCombat() || (!bot->IsInFlight() && !botAI->CanMove()))
        return false;
    if (ConsumeScheduledWork(botAI))
        return true;
    if (botAI->HasLootInteraction() && !PassengerOwnsTravel(*bot, *data))
        return false;
    return StepTravel(data, TravelProgress::Overdue::None);
}

bool NewRpgTravelAction::StepTravel(NewRpgInfo::Travel* data, TravelProgress::Overdue overdue)
{
    using TravelProgress::Overdue;
    auto const membership = TransportGeometry::ReadMembership(*bot);
    if (!membership.Available && !bot->IsInFlight())
        return true;
    // A trip operation requested aboard waits for an acknowledged exit within one period.
    if (data->trip.offFirst.sinceMs)
    {
        TravelLegs::OffStep const step = GetOffFirst(bot, *data);
        if (step == TravelLegs::OffStep::Wait || step == TravelLegs::OffStep::Disembark)
            return true;
        if (step == TravelLegs::OffStep::GiveUp || step == TravelLegs::OffStep::Lost)
        {
            bool const expired = step == TravelLegs::OffStep::GiveUp;
            botAI->TraceTravelInteraction("route_recovery", expired ? "stuck_aboard" : "transport_lost");
            EndTrip(*data, expired ? "stuck aboard" : "transport lost",
                    expired ? "abort:stuck_aboard" : "abort:transport_lost");
            return true;
        }
        TravelLegs::OffFirst const& off = data->trip.offFirst;
        if (off.end == TravelLegs::OffFirstEnd::Arrive)
            return Arrive(*data);
        if (off.end == TravelLegs::OffFirstEnd::Abort)
            EndTrip(*data, off.why, off.source);
        return true;
    }
    Transport* const ridden = bot->GetTransport();
    TravelLegs::Trip const& trip = data->trip;
    // A route may have been superseded aboard. Only the route with this ride or its own boarding result may use it.
    bool const ownsBoard = data->passengerMotion && data->passengerMotion->Kind == MotionKind::Board &&
                           data->passengerMotion->Transport == membership.Guid;
    // Taken aboard by the leg's vessel at the leg's start (its deck docked under the waiting bot) is the boarding
    TravelRoute::Point const standing{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    // or on the walk to that leg's start (the walk is done, WT54-6); not by an elevator or the tram
    bool const takenAboard =
        ridden && (TravelLegs::TakenAboard(trip, standing, membership.Entry) ||
                   (!ridden->IsStaticTransport() && TravelLegs::WalkTakenAboard(trip, standing, membership.Entry)));
    if (ridden && !takenAboard && (!AtTransportLeg(trip) || (!trip.passengerGuid && !ownsBoard)))
    {
        OffFirstBegins(bot, *data);
        return true;
    }
    uint32 const quayLeg = trip.boarding == TravelLegs::Boarding::Approach ? MotionQuayLeg(trip) : TravelLegs::NO_NODE;
    if (overdue == Overdue::RouteDeadline)
    {
        botAI->TraceTravelInteraction("route_recovery", "deadline");
        EndTrip(*data, "deadline", "abort:deadline");
        return true;
    }
    if (overdue == Overdue::Walk || overdue == Overdue::OffMesh)
    {
        if (!data->progress.AttemptedWalk())
        {
            botAI->TraceTravelInteraction("route_recovery", "stalled");
            EndTrip(*data, "stalled", "abort:stalled");
            return true;
        }

        if (data->progress.Owns(bot->movespline->GetId()) && !bot->movespline->Finalized())
            bot->StopMoving();
        bool const offMesh = GiveUpMove(botAI->rpgInfo.moveFarPos);
        if (!offMesh && !data->CurrentLeg() && data->bindInn)
        {
            botAI->TraceTravelInteraction("route_recovery", "stuck");
            return Arrive(*data, true);
        }
        return LegFailed(*data, offMesh ? TravelLegs::FailReason::OffMesh : TravelLegs::FailReason::Stuck);
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
        // The trip's start, or a new route once the failed leg's stuck report has reached the published book
        if (data->failedMs && GetMSTimeDiffToNow(data->failedMs) < TravelLegs::REPLAN_DELAY_MS)
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
    seen.activeLegMs = data->progress.LegMs();
    seen.aboard = membership.Entry;
    seen.aboardGuid = membership.Guid;
    seen.membershipAvailable = membership.Available;
    seen.boardMotion = ownsBoard;
    seen.passengerOutcome = TakePassengerOutcome(*bot, *data);
    seen.quayLeg = quayLeg;
    SeekQuay(bot, *data, seen);
    // The transport scan (WT19): once a TRANSPORT_SCAN_MS while a transport leg is past its approach, on the bot's
    // map only. The deck point and the Transport* live for this tick only
    std::optional<Point> deck;
    Transport* transport = nullptr;
    float moved = -1.0f;  // the transport's yards since the scan before on this map; -1 none
    if (AtTransportLeg(trip) && getMSTimeDiff(data->transportScanMs, seen.nowMs) >= TravelLegs::TRANSPORT_SCAN_MS)
    {
        data->transportScanMs = seen.nowMs;
        TravelLegs::TransportSight& sight = seen.transport;
        sight.scanned = true;
        uint64 const expected = data->passengerMotion ? data->passengerMotion->Transport : trip.sight.guid;
        transport = FindLegTransport(bot, trip.legs[trip.legIdx], expected);
        if (transport)
        {
            moved = SeeTransport(bot, transport, trip, sight);
            TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
            if (!ridden && !sight.moving && !data->passengerMotion &&
                std::fabs(seen.bot.z - TravelLegs::WaitPoint(trip, leg).z) <= TravelLifecycle::DOCK_FLOOR_YARDS)
            {
                Point const from = BoardStart(*bot, trip);
                deck = TransportGeometry::GroundGeometry{*bot, transport}.DeckFrom(from);
                if (deck)
                {
                    seen.boardingReady = std::fabs(deck->Z - from.Z) <= TravelLifecycle::DOCK_FLOOR_YARDS &&
                                         TransportGeometry::GroundGeometry{*bot}.OnGround(from);
                    if (seen.boardingReady && TravelLegs::WantsSightCheck(trip) && !bot->IsMounted() &&
                        ReadyForTransportMove(*bot))
                        sight.unseen = !sRandomPlayerbotMgr.IsTeleportSeen(
                            bot, WorldPosition(bot->GetMapId(), deck->X, deck->Y, deck->Z));
                }
            }
        }
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
        botAI->TraceTravelInteraction("route_progress", "waypoint", ObjectGuid::Empty, 0, "none", 0.0f,
                                      std::pair{legBefore, legStartBefore});
    if (legChanged)
        data->progress.StartLeg(!wasWalk || waypointReached);
    if (data->passengerMotion &&
        (trip.legIdx != legBefore || (next.act != TravelLegs::Act::Board && next.act != TravelLegs::Act::Disembark)))
        TransportGeometry::DiscardMotion(*bot, data->passengerMotion);
    LogMisses(bot, trip);
    SendTransportNotes(bot, data->trip);
    if (seen.transport.scanned && ridden && trip.legIdx == legBefore && trip.boarding == TravelLegs::Boarding::Ride &&
        sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        LogAboardScan(bot, trip, moved);
    // The legs passed this tick (no-op walks are no legs to a reader): id the trip, amount the leg's seconds
    for (uint32 i = legBefore; i < trip.legIdx && i < trip.legs.size(); ++i)
    {
        if (TravelLegs::IsNoOp(trip.legs[i]))
            continue;
        uint32 const seconds = i == legBefore ? (seen.nowMs - legStartBefore) / IN_MILLISECONDS : 0;
        EventLog::Emit(bot, "Travel", TravelLegs::LegDoneSource(trip.legs[i].type), data->tripId, seconds);
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
    uint16 const constraints = botAI->rpgInfo.constraints;
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
    // For the case's hold line: what the follower did last, and when
    data->trip.lastAct = next.act;
    data->trip.lastTarget = next.target;
    data->trip.lastActMs = seen.nowMs;
    switch (next.act)
    {
        case TravelLegs::Act::Arrived:
            return Arrive(*data);
        case TravelLegs::Act::Walk:
            if (trip.legIdx != legBefore && seen.quayLeg != trip.legIdx && MotionQuayLeg(trip) != TravelLegs::NO_NODE)
                return true;  // the new approach gets a quay observation before walking
            // Along a graph link: its stored points (WT58); the engine's "no path" at its far node refused it
            if (trip.walk.mode == TravelWalk::Mode::Refused)
                return LegFailed(*data, TravelLegs::FailReason::NoPath);
            next.target = WalkLinkTarget(data->trip, seen.bot, next.target);
            // Off the graph: the navmesh probe's points (WT56); a walk the navmesh refused fails the leg at once, and
            // the replan leaves it out. Refused where the bot stands off the mesh: the off-mesh recovery instead
            if (!WalkPieceTarget(bot, data->trip, seen.bot, next.target))
            {
                if (!BotOffMesh())
                    return LegFailed(*data, TravelLegs::FailReason::NoWalk);
                data->trip.piece.probe = TravelWalk::Probe::Walk;
                data->trip.piece.orders.clear();
                data->progress.Latch(TravelProgress::Overdue::OffMesh);
                return true;
            }
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
            if (transport)
                LogBoardDockFoot(bot, *data, *transport);
            // Refused at the wait point: a quay the search found there is walked to first (WT55e)
            if (transport && !data->passengerMotion && SeekBoardQuay(bot, *data, *transport))
                return true;
            if (transport && deck && seen.boardingReady && !data->passengerMotion && PrepareTransportMove(*bot))
            {
                Point const from = BoardStart(*bot, data->trip);
                if (TravelLifecycle::AcceptBoard(TransportGeometry::GroundGeometry{*bot, transport}, from, *deck))
                {
                    LogBoardMove(bot, transport->GetEntry(), *deck, BoardStartName(data->trip));
                    IssueTransportMove(*bot, *data, Intent(*bot, *transport, MotionKind::Board, *deck), {from});
                    if (data->passengerMotion)
                        data->trip.boardCrossing = TravelLegs::StoreCrossing(
                            transport->GetGUID().GetRawValue(), TransportGeometry::PoseOf(*transport), *deck, from);
                }
                else
                    LogBoardGate(bot, *data, *transport, deck, true, true);
            }
            else if (transport && !data->passengerMotion && !seen.transport.moving)
                LogBoardGate(bot, *data, *transport, deck, seen.boardingReady, false);
            return true;
        case TravelLegs::Act::Hop:
            data->trip.boardCrossing = {};  // a hop is no crossing to walk back
            if (transport && deck)
                HopOnto(bot, *data, *transport, *deck, trip.misses);
            return true;
        case TravelLegs::Act::Disembark:
            if (ridden && seen.transport.scanned && seen.transport.guid == membership.Guid)
            {
                Point const dest{data->dest.GetPositionX(), data->dest.GetPositionY(), data->dest.GetPositionZ()};
                TravelRoute::Leg const& rideLeg = trip.legs[trip.legIdx];
                // Off at the stop's wait point where it has one: the far one, or after no exit the one it waited on
                Point const target{next.target.x, next.target.y, next.target.z};
                bool const back = trip.boarding == TravelLegs::Boarding::Back;
                bool const atWaitPoint =
                    (back ? rideLeg.waitFrom : rideLeg.waitTo) && next.target.mapId == bot->GetMapId();
                std::optional<Point> const waitPoint = atWaitPoint ? std::optional<Point>{target} : std::nullopt;
                if (back)  // the quay or wait point it waited on
                    WalkOff(*bot, *data, *ridden, waitPoint, std::vector<Point>{target});
                else
                {
                    LogExitDockFoot(*bot, *data, *ridden);
                    WalkOff(*bot, *data, *ridden, waitPoint,
                            OnwardRoute(trip, bot->GetMap(), bot->GetMapId(), dest, bot->GetPositionZ()));
                }
            }
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

bool NewRpgTravelAction::RequestRoute(NewRpgInfo::Travel& data)
{
    TravelRoute::Snapshot snapshot = TravelRoute::FillSnapshot(bot, ++botAI->rpgInfo.routeRequests);
    // Transports run (WT19; a timed-out transport leg's link is left out through avoidEdges). Flights only to
    // discovered nodes (the snapshot's taxi mask), their fares within the purse (the search's fare budget). The
    // hearthstone is a leg while its stone is in the bags and it is ready or cooling for at most the wait cap (the
    // search); not again after a hearth leg of this trip failed
    snapshot.hasHearthstone =
        snapshot.hasHearthstone && !data.noHearth && TestConstraints::HearthAllowed(botAI->rpgInfo.constraints);
    snapshot.allowTransport = true;
    if (!TestConstraints::TaxiAllowed(botAI->rpgInfo.constraints))
        snapshot.knownTaxi.fill(0);
    TravelRoute::Request request;
    request.snapshot = snapshot;
    request.goal = {data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ()};
    // The trip's stuck walks: their start or goal hops are left out (TravelLegs::NoteStuck)
    request.avoidNodes = data.trip.avoid;
    request.noDirect = data.trip.noDirect;
    // The walks this bot died on twice (TravelLegs::NoteDeath): their graph edges are left out, for its trip only
    request.avoidEdges = data.trip.avoidEdges;
    // The walks off the graph the navmesh refused (TravelLegs::NoteWalkRefused): not walked again in this trip
    request.avoidWalks = data.trip.avoidWalks;
    // After a failed fly leg the trip's later routes walk (TravelLegs::AfterFailure)
    request.noFly = data.trip.noFly;
    TravelLegs::RouteWait const waiting{snapshot.requestId, snapshot.position.mapId, snapshot.takenMs, true,
                                        snapshot.position};
    auto const submit = [&]
    {
        return TravelRoute::RouteService::Instance().Submit(bot->GetGUID().GetRawValue(), request,
                                                            TravelRoute::ConfiguredParams());
    };
    if (!TravelProgress::SubmitRoute(data.route, waiting, submit))
    {
        EndTrip(data, "no route service", "abort:no_service");  // the graph is not loaded yet: never wait
        return true;
    }
    botAI->TraceTravelInteraction("route_request", "issued");
    return true;
}

bool NewRpgTravelAction::TakeRoute(NewRpgInfo::Travel& data)
{
    uint32 const now = getMSTime();
    std::optional<TravelRoute::Answer> answer =
        TravelRoute::RouteService::Instance().TryTake(bot->GetGUID().GetRawValue());
    // A late or lost answer asks again from here, a few times per trip, before it fails the leg
    auto const askAgain = [&](char const* why)
    {
        if (!TravelLegs::Resubmit(data.trip))
            return LegFailed(data, TravelLegs::FailReason::NoAnswer);
        botAI->TraceTravelInteraction("route_recovery", "resubmit");
        LOG_DEBUG("playerbots", "[Route] {} {} ({} ms since the request), asked again", bot->GetName(), why,
                  now - data.route.requestedMs);
        return RequestRoute(data);
    };
    if (!answer)
        return TravelLegs::WaitExpired(data.route, now) ? askAgain("got no answer") : true;
    TravelRoute::Point const current{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    switch (TravelLegs::Judge(data.route, answer->requestId, current, now))
    {
        case TravelLegs::Verdict::Drop:
            return TravelLegs::WaitExpired(data.route, now) ? askAgain("got only a stale answer") : true;
        case TravelLegs::Verdict::Refresh:
            return askAgain(bot->GetMapId() != data.route.mapId ? "changed maps" : "moved or took the answer too late");
        case TravelLegs::Verdict::Apply:
            break;
    }

    data.route.waiting = false;
    TravelRoute::Result& route = answer->result;
    if (!route.ok)
    {
        LOG_DEBUG("playerbots",
                  "[Route] {} has no route: {} from ({}, {:.0f}, {:.0f}, {:.0f}) to ({}, {:.0f}, {:.0f}, {:.0f}), "
                  "noHearth {}, replans {}",
                  bot->GetName(), TravelRoute::FailureWord(route.failure), bot->GetMapId(), bot->GetPositionX(),
                  bot->GetPositionY(), bot->GetPositionZ(), data.dest.GetMapId(), data.dest.GetPositionX(),
                  data.dest.GetPositionY(), data.dest.GetPositionZ(), data.noHearth, data.trip.replans);
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
        EventLog::Emit(bot, "Travel", "start", data.tripId, static_cast<int64>(route.totalSeconds));
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "start", data.bindInn ? "inn" : "order",
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
    return true;
}

bool NewRpgTravelAction::UsePortal(NewRpgInfo::Travel& data)
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
bool NewRpgTravelAction::UseStaticPortal(NewRpgInfo::Travel& data)
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

bool NewRpgTravelAction::TalkFlightMaster(NewRpgInfo::Travel& data)
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

    switch (StartTaxi(master->templateEntry, master->pos, leg.taxiNodes))
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

bool NewRpgTravelAction::UseHearth(NewRpgInfo::Travel& data)
{
    // The cast takes 10 s and a cooling stone is waited for (the route's seconds hold both; the leg's timeout bounds
    // them). The teleport is the stone's own spell: no bot teleport, no sight check
    if (bot->IsNonMeleeSpellCast(false) || bot->HasSpellCooldown(HEARTHSTONE_SPELL))
        return true;
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

bool NewRpgTravelAction::LegFailed(NewRpgInfo::Travel& data, TravelLegs::FailReason reason)
{
    botAI->TraceTravelInteraction("route_recovery", TravelLegs::FailWord(reason));
    TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
    TravelLegs::Trip& trip = data.trip;
    LOG_DEBUG("playerbots", "[Route] {} leg {}/{} failed: {}", bot->GetName(), trip.legIdx + 1, trip.legs.size(),
              TravelLegs::FailWord(reason));
    uint32 const legSeconds =
        trip.legIdx < trip.legs.size() ? GetMSTimeDiffToNow(trip.legStartMs) / IN_MILLISECONDS : 0;
    EventLog::Emit(bot, "Travel", TravelLegs::LegFailedSource(reason), data.tripId, legSeconds);
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
            EventLog::Emit(bot, Upkeep::EVENT_FLIGHT_FALL, word, data.tripId, std::lround(height));
            // What brought it down (WU11b fix 7: Elyvia and Meldel, both in Zul'Drak's area 4310): a fight after the
            // takeoff (combat) or not (fall), the mount, its flight aura and the flying flag now, the core's indoor
            // state (its vmap indoor check takes an outdoor-only mount: fix 10's lead for area 4310), the height and area
            LOG_INFO("playerbots",
                     "[Route] {} came down on fly leg {}/{} ({}): mounted {}, flight aura {}, flying {}, outdoors {}, "
                     "{:.0f} yd above the ground in area {} at ({:.0f}, {:.0f}, {:.0f})",
                     bot->GetName(), trip.legIdx + 1, trip.legs.size(), word, bot->IsMounted(),
                     bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura(), bot->IsFlying(), bot->IsOutdoors(),
                     height, bot->GetAreaId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        }
    }
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Hearth)
        data.noHearth = true;
    // A portal that did not take the bot through, or a flight that could not be taken: the search avoids that edge
    // for a while (a stuck walk was reported by MoveFarTo's give-up already)
    if (reason == TravelLegs::FailReason::Portal || reason == TravelLegs::FailReason::Taxi)
    {
        auto const [from, to] =
            reason == TravelLegs::FailReason::Portal ? TravelLegs::StuckEdge(trip) : TravelLegs::FirstFlightHop(trip);
        if (from != TravelLegs::NO_NODE)
        {
            TravelRoute::StuckBook::Report report;
            report.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
            report.destination = report.bot;
            report.fromNode = from;
            report.toNode = to;
            TravelRoute::StuckBook::Instance().Push(report);
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
    // A takeoff refused where the bot stands keeps flying: the replan starts no flight at this spot (AddFlyLegs)
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
        EndTrip(data, TravelLegs::FailWord(reason), TravelLegs::AbortSource(reason));
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

bool NewRpgTravelAction::FlyDown(NewRpgInfo::Travel& data, TravelLegs::Decision const& down)
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
        EndTrip(data, TravelLegs::FailWord(endReason), TravelLegs::AbortSource(endReason));
    return true;
}

bool NewRpgTravelAction::Arrive(NewRpgInfo::Travel& data, bool stuck)
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
        // is tried again a few times, a stuck bot out of reach gives up
        float const yards =
            bot->GetExactDist(data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ());
        WorldTravel::InnStep const step = WorldTravel::InnNext(yards, stuck, data.bindTries);
        if (step == WorldTravel::InnStep::Walk)
            return MoveFarTo(data.dest);
        bool bound = false;
        char const* why = "stuck out of reach";
        if (step == WorldTravel::InnStep::Bind)
        {
            ++data.bindTries;
            why = "no innkeeper in reach";
            std::list<Unit*> units;
            Acore::AnyUnitInObjectRangeCheck check(bot, WorldTravel::INN_REACH_YARDS);
            Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
            Cell::VisitObjects(bot, searcher, WorldTravel::INN_REACH_YARDS);
            for (Unit* unit : units)
            {
                if (!unit->IsCreature() || !unit->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
                    continue;
                Creature* innkeeper = bot->GetNPCIfCanInteractWith(unit->GetGUID(), UNIT_NPC_FLAG_INNKEEPER);
                if (!innkeeper)
                {
                    why = "innkeeper not interactable";
                    continue;
                }
                bot->GetSession()->SendBindPoint(innkeeper);
                bound = true;
                break;
            }
            if (!bound && data.bindTries < WorldTravel::INN_BIND_TRIES)
            {
                LOG_DEBUG("playerbots", "[Route] {} bind try {} failed ({}), {:.1f} yd from the inn spot",
                          bot->GetName(), static_cast<uint32>(data.bindTries), why, yards);
                return true;
            }
        }
        LOG_INFO("playerbots", "[Route] {} {} at the inn after {} s ({} replans, {} binds, {:.1f} yd{}{})",
                 bot->GetName(), bound ? "bound its hearthstone" : "could not bind", seconds, replans,
                 static_cast<uint32>(data.bindTries), yards, bound ? "" : ": ", bound ? "" : why);
        EventLog::Emit(bot, "Travel", bound ? "inn_bound" : "inn_failed", data.tripId, seconds);
        // A spot where the bot stood but found no usable innkeeper (phased out, dead, hostile) is avoided by its next
        // inn errands for a while (WorldTravel::InnAvoided); a bot stuck out of reach blames no spot
        bool const noInnkeeper = std::string_view(why) == "no innkeeper in reach";
        if (!bound && (noInnkeeper || std::string_view(why) == "innkeeper not interactable"))
        {
            WorldTravel::AvoidInn(
                botAI->rpgInfo.innAvoid, getMSTime(),
                {data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ()});
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "avoid",
                               noInnkeeper ? "no_innkeeper" : "not_interactable", yards, {}, data.tripId);
        }
    }
    if (data.place.empty())
        LOG_DEBUG("playerbots", "[Route] {} arrived after {} s (estimate {:.0f} s, {} replans)", bot->GetName(),
                  seconds, data.estimateSeconds, replans);
    else
        LOG_INFO("playerbots", "[Route] {} arrived at {} after {} s (estimate {:.0f} s, {} replans)", bot->GetName(),
                 data.place, seconds, data.estimateSeconds, replans);
    EventLog::Emit(bot, "Travel", "arrived", data.tripId, seconds);
    WorldLog::StatusReason const reasonScope("arrived");
    botAI->rpgInfo.ChangeToIdle();
    return true;
}

void NewRpgTravelAction::EndTrip(NewRpgInfo::Travel& data, char const* why, char const* source)
{
    // Not on a ship's deck: the bot gets off first, then the trip ends (WT19-d)
    if (OffFirstBegins(bot, data, TravelLegs::OffFirstEnd::Abort, why, source))
        return;
    GiveTripUp(botAI, data, why, source);
}
