/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BotTravel.h"
#include "CellImpl.h"
#include "CensusMgr.h"
#include "CheckMountStateAction.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MountRules.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RpgNavMeshSnap.h"
#include "Timer.h"
#include "Transport.h"
#include "TravelGraph.h"
#include "TravelLegRules.h"
#include "TravelLifecycle.h"
#include "TravelTransportGeometry.h"
#include "WorldPacket.h"

using namespace NewRpgInternal;

namespace
{
constexpr float HOP_LIFT_YARDS = 0.5f;  // the hop lands this far above the deck point (as the off-mesh snap does)

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

// A ship or zeppelin at a stop it keeps for the attach's deadline (MotionTransport::IsMoving is private: its key
// frames, as MotionTransport::Update reads them)
bool Docked(MotionTransport const& ship)
{
    uint32 const period = ship.GetPeriod();
    if (!period)
        return false;
    uint32 const timer = ship.GetPathProgress() % period;
    for (KeyFrame const& frame : ship.GetKeyFrames())
        if (timer >= frame.ArriveTime && timer + BotTravel::ABOARD_DEADLINE_MS < frame.DepartureTime)
            return true;
    return false;
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

// WT55b, data for the navigation plan's NV18 (it goes with the searches, WT.47 and WT70): at a stop with a wait point
// the old searches decide nothing. Once per vessel, stop and role in a server run they still run for a traced bot, and
// a dock_foot line says where they would have put the crossing's foot. A bounded set (vessels x stops x 2 roles) the
// map threads share; the board role runs in the rate-limited transport scan, the exit role at the exit search
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
}  // namespace

namespace NewRpgInternal
{
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

bool ReadyForTransportMove(Player const& bot) { return !TransportMoveHold(bot); }

bool PrepareTransportMove(Player& bot)
{
    if (!ReadyForTransportMove(bot))
        return false;
    if (!bot.IsMounted())
        return true;
    CheckMountStateAction::Dismount(&bot, MountRules::How::Transport);
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

// A boarding walks straight onto the deck; an exit crosses the deck by its deck walk's corners to the deck's edge (via)
// then to the quay. A via point within 0.2 yd of the one before is left out (a spline refuses a segment under 0.1 yd)
void IssueTransportMove(Player& bot, NewRpgInfo::Travel& data, PassengerMotion motion, std::vector<Point> const& via)
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

// `route aboard` (tests): the bot is stood on the deck of the docked ship or zeppelin of that entry nearest it on its
// map, at the boarding deck point from where it stands (the test puts it on the dock first). No explicit attach:
// AddPassenger takes the current position as the offset, so PlayerbotAI's passenger check attaches it to that vessel
// after the teleport (BeginAboard's intent). Refused: absent, not_mo (an elevator), away (not at a stop), no_deck
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
        EventLog::Emit(bot, BotTravel::ABOARD_EVENT_TYPE, source.c_str(), entry);
        return;
    }
    LOG_INFO("playerbots", "[Route] {} aboard {}: deck {:.1f}, {:.1f}, {:.1f}", bot->GetName(), entry, deck->X, deck->Y,
             deck->Z);
    bot->NearTeleportTo(deck->X, deck->Y, deck->Z + HOP_LIFT_YARDS, bot->GetOrientation());
    botAI->GetTravel().BeginAboard(entry, transport->GetGUID().GetRawValue());
}

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
bool OffFirstBegins(Player* bot, NewRpgInfo::Travel& data, TravelLegs::OffFirstEnd end, char const* why,
                    char const* source, StatusEnd::Outcome outcome)
{
    Transport const* const ridden = bot->GetTransport();
    if (!ridden)
        return false;
    TravelLegs::Trip& trip = data.trip;
    if (trip.offFirst.sinceMs)
        return true;
    uint32 const period = TransportGeometry::Period(*ridden);
    if (!TravelLegs::BeginOffFirst(trip, ridden->GetEntry(), ridden->GetGUID().GetRawValue(), period, getMSTime(), end,
                                   why, source, outcome))
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

bool PassengerOwnsTravel(Player const& bot, NewRpgInfo::Travel const& data)
{
    TravelLegs::Aboard const ride{bot.GetTransport() || bot.HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT),
                                  bot.IsInFlight(), bot.GetVehicle() != nullptr};
    return TravelLegs::OwnerOf(data.trip, data.passengerMotion.has_value(), ride) == TravelLegs::TravelOwner::Passenger;
}
}  // namespace NewRpgInternal
