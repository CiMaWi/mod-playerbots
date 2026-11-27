/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "BotTactics.h"
#include "BotTravel.h"
#include "CheckMountStateAction.h"
#include "Engine.h"
#include "EventLog.h"
#include "Group.h"
#include "Map.h"
#include "MeshMoveRules.h"
#include "MotionMaster.h"
#include "MountRules.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "NewRpgAction.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StartingHistoryRules.h"
#include "StringFormat.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "Transport.h"
#include "TravelCrossingTypes.h"
#include "TravelGraph.h"
#include "TravelLegRules.h"
#include "TravelLifecycle.h"
#include "TravelTransportGeometry.h"
#include "WorldPacket.h"

using namespace NewRpgInternal;

// The transport leg's game side (WT70c, WT.53; PLAN 3.21): the leg crosses, rides and leaves by the stored crossings
// of its two stops (TravelCrossingTypes.h, read from the travel graph's immutable index). The rules decide in
// TravelLegRules.h (StepTransport, StepOffFirst); this file reads the vessel for them and issues what they ask for: the
// one crossing function (a walk or the player's jump), the walk on the deck by the core's vessel path finder and the
// place aboard. No search, no ray cast, no hop, no teleport: a stop without a usable row is no ride.

namespace
{
// What keeps a bot from a transport move this tick (the debug word), or nothing
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

bool DebugLogged() { return sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG); }

float Yards(Point const& a, Point const& b) { return std::sqrt(TravelLifecycle::DistanceSquared(a, b)); }

// The bot's own offset in the frame of the vessel it rides
Point OffsetOf(Player const& bot)
{
    Position const& at = bot.m_movementInfo.transport.pos;
    return {at.GetPositionX(), at.GetPositionY(), at.GetPositionZ()};
}

// A deck point (an offset in the vessel's frame) as a world point, at the vessel's pose of this call
Point DeckWorld(Transport const& transport, TravelCrossing::Spot const& deck)
{
    float x = deck.x;
    float y = deck.y;
    float z = deck.z;
    transport.CalculatePassengerPosition(x, y, z);
    return {x, y, z};
}

// The vessel stands at the stop's stored pose this tick: the passenger check's own test (TravelLifecycle::SameDock),
// no tolerance of the leg's
bool StandsAt(Transport const& transport, TravelCrossing::Stop const& stop)
{
    return transport.GetMapId() == stop.mapId &&
           TravelLifecycle::SameDock(TravelLegs::PoseOf(stop), TransportGeometry::PoseOf(transport));
}

// The stop record the vessel stands at (the get-off, `route aboard`): a binary search to the car in the graph's
// crossing index, then its few stops by the pose test. A ship or zeppelin is car 0, a lift's or the tram's car its
// spawn
TravelCrossing::Stop const* StopStoodAt(Transport const& transport)
{
    uint32 const car = transport.IsStaticTransport() ? transport.GetSpawnId() : 0;
    return TravelGraph::Instance().Crossings().StopAt(transport.GetEntry(), car,
                                                      [&transport](TravelCrossing::Stop const& stop)
                                                      { return StandsAt(transport, stop); });
}

// A lift's or the tram's car by the spawn a stop record names (WT70d, PLAN 3.21 (2)): the map's spawn store, O(1), as
// the stop dump reads it. Null when that car is not in the bot's map (its grid not loaded)
Transport* CarBySpawn(Map& map, uint32 spawnId, uint32 entry)
{
    auto const range = map.GetGameObjectBySpawnIdStore().equal_range(spawnId);
    for (auto it = range.first; it != range.second; ++it)
    {
        StaticTransport* const car = it->second ? it->second->ToStaticTransport() : nullptr;
        if (car && car->IsInWorld() && car->GetEntry() == entry)
            return car;
    }
    return nullptr;
}

// A vessel's period and its place in it: a ship's or zeppelin's path progress, a car's animation progress (read
// against a stop's window)
void ReadProgress(Transport* transport, uint32& periodMs, uint32& progressMs)
{
    periodMs = 0;
    progressMs = 0;
    if (MotionTransport const* ship = transport->ToMotionTransport())
    {
        periodMs = ship->GetPeriod();
        progressMs = periodMs ? ship->GetPathProgress() % periodMs : 0;
    }
    else if (StaticTransport const* car = transport->ToStaticTransport())
    {
        periodMs = car->GetPeriod();
        progressMs = periodMs ? car->GetPathProgress() % periodMs : 0;
    }
}

// G414: a refused deck move the deck_path line does not show (the trip-less get-off, a trip without a GM order's place,
// a follower) writes one deck_refused line, at most one per DECK_REFUSAL_GAP_MS per bot (debug log only): type -1, the
// vessel has no mesh; else the path's type and how far short of the deck point it ends
constexpr uint32 DECK_REFUSAL_GAP_MS = 30000;

void TraceDeckRefusal(PlayerbotAI* botAI, Player const& bot, HeldTrip const* data, Transport const& ridden,
                      TravelCrossing::Spot const& to, char const* what, int32 type, float shortYards)
{
    uint32& tracedMs = botAI->GetTravel().DeckRefusalTracedMs();
    uint32 const now = getMSTime();
    if (tracedMs && getMSTimeDiff(tracedMs, now) < DECK_REFUSAL_GAP_MS)
        return;
    tracedMs = std::max<uint32>(now, 1);
    Point const from = OffsetOf(bot);
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] deck_refused bot={} trip={} off_first={} entry={} what={} type={} short_yd={:.2f} "
              "from={:.2f},{:.2f},{:.2f} to={:.2f},{:.2f},{:.2f}",
              bot.GetGUID().GetCounter(), data ? data->tripId : 0, data && data->trip.offFirst.sinceMs ? 1 : 0,
              ridden.GetEntry(), what, type, shortYards, from.X, from.Y, from.Z, to.x, to.y, to.z);
}

// A walk on the deck (PLAN 3.21 (6)): the core's vessel path finder from the bot to a deck point (an offset; asked in
// world points at the vessel's pose of this call) and its answer issued at once as a spline, which rides in the
// vessel's frame. Only a complete NORMAL path that ends within DECK_REACH_YARDS of the point is walked: no straight
// line, no second try, no search. Counted as a `deck` move on the path's answer (MeshMoveRules.h). A traced bot's
// path is logged point by point, in offsets. A follower's move (data null) is not watched; any untraced refusal writes
// deck_refused (TraceDeckRefusal)
bool DeckMove(PlayerbotAI* botAI, Player& bot, HeldTrip* data, Transport const& ridden, TravelCrossing::Spot const& to,
              char const* what)
{
    bool const traced = data && !data->place.empty() && DebugLogged();
    if (!ridden.GetVesselNavMesh())
    {
        if (DebugLogged())
            TraceDeckRefusal(botAI, bot, data, ridden, to, what, -1, 0.0f);
        return false;
    }
    Point const end = DeckWorld(ridden, to);
    PathGenerator path(&bot);
    bool const found = path.CalculatePath(end.X, end.Y, end.Z, false);
    uint32 const type = path.GetPathType();
    Movement::PointsArray const& points = path.GetPath();
    botAI->GetTravel().NoteMeshMove(MeshMove::Kind::Deck, type, MeshMove::LastStep(points));
    bool const normal = found && type == PATHFIND_NORMAL && points.size() > 1;
    // Where the path ends against the point asked for (both world points of this call's pose): a normal answer may lie
    // short of it, and then it is refused as no path is (WT.55)
    float const shortYards = points.empty() ? 0.0f : (points.back() - G3D::Vector3(end.X, end.Y, end.Z)).length();
    bool const taken = TravelLegs::DeckPathTaken(normal, shortYards);
    if (traced)
    {
        std::string offsets;
        for (G3D::Vector3 const& point : points)
        {
            float x = point.x;
            float y = point.y;
            float z = point.z;
            ridden.CalculatePassengerOffset(x, y, z);
            offsets += Acore::StringFormat("{}{:.2f},{:.2f},{:.2f}", offsets.empty() ? "" : "/", x, y, z);
        }
        Point const from = OffsetOf(bot);
        LOG_DEBUG(
            "playerbots",
            "[TravelLifecycle] deck_path bot={} trip={} leg={} entry={} what={} type={} walked={} short_yd={:.2f} "
            "from={:.2f},{:.2f},{:.2f} to={:.2f},{:.2f},{:.2f} points={}",
            bot.GetGUID().GetCounter(), data->tripId, data->trip.legIdx, ridden.GetEntry(), what, type, taken ? 1 : 0,
            shortYards, from.X, from.Y, from.Z, to.x, to.y, to.z, offsets.empty() ? std::string("none") : offsets);
    }
    if (!taken)
    {
        if (!traced && DebugLogged())
            TraceDeckRefusal(botAI, bot, data, ridden, to, what, static_cast<int32>(type), shortYards);
        return false;
    }
    if (bot.IsSitState())
        bot.SetStandState(UNIT_STAND_STATE_STAND);
    bot.GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(&bot);
    init.MovebyPath(points);
    if (init.Launch() <= 0)
        return false;
    if (data)
        data->deckSpline = bot.movespline->GetId();
    return true;
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

// The leg's transport, found anew on every scan (never kept across ticks): the one the bot rides; else a lift's or the
// tram's car by the spawn the leg's stop record names (O(1)), a ship or zeppelin as the nearest of the leg's entry to
// its from-stop on the bot's map (O(transports on the map), once a scan)
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
    TravelCrossing::Stop const* const record = TravelGraph::Instance().Crossings().At(leg.crossFrom);
    if (record && record->guid)
    {
        Transport* const car = CarBySpawn(*bot->GetMap(), record->guid, leg.object);
        return car && TransportGeometry::Available(*bot, *car) ? car : nullptr;
    }
    TravelRoute::Point const& stop = leg.waitFrom ? leg.stopFrom : leg.from;
    Transport* best = nullptr;
    float bestYards = std::numeric_limits<float>::max();
    auto const consider = [&](Transport* transport)
    {
        if (!transport || transport->GetEntry() != leg.object || !TransportGeometry::Available(*bot, *transport))
            return;
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

// The one function that issues a crossing (PLAN 3.21 (5)): a spline launched at once, from the bot over the row's
// first point (the run-up; left out within PATH_SKIP_YARDS of the bot) to the motion's endpoint. No path request. The
// spline takes its frame from the unit: ashore the points stay world points (a boarding ends where the deck point is
// while the vessel stands), aboard they become offsets and the move rides with the vessel (a leaving ends on the dock
// point). A walk runs the path at the bot's run speed. A jump is the same spline at the speed that covers the crossing
// in the jump's time, with the player's parabola over the crossing's share of it; never MoveJump, which the core's
// motion master runs in the world's frame. The motion goes into its owner's slot: the leg's, or a follower's
void IssueTransportMove(Player& bot, std::optional<PassengerMotion>& slot, PassengerMotion motion, Point const& first,
                        TravelCrossing::Move const& move)
{
    if (bot.IsSitState())
        bot.SetStandState(UNIT_STAND_STATE_STAND);
    bot.GetMotionMaster()->Clear();
    G3D::Vector3 const start{bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ()};
    G3D::Vector3 const middle{first.X, first.Y, first.Z};
    G3D::Vector3 const end{motion.Endpoint.X, motion.Endpoint.Y, motion.Endpoint.Z};
    // A spline refuses a segment under 0.1 yd: a first point that near the end is left out as well
    bool const runUp = move.runUp && (end - middle).length() >= TravelCrossing::PATH_SKIP_YARDS;
    Movement::MoveSplineInit init(&bot);
    if (runUp)
        init.MovebyPath(Movement::PointsArray{start, middle, end});
    else
        init.MoveTo(end.x, end.y, end.z, false, false);
    if (move.jump)
    {
        // SetParabolic takes the share of the spline's time after which the parabola begins (0..1): the run-up's
        float const before = runUp ? (middle - start).length() : 0.0f;
        float const across = (end - (runUp ? middle : start)).length();
        init.SetVelocity(move.speed);
        init.SetParabolic(move.amplitude, before / std::max(before + across, 0.01f));
    }
    if (init.Launch() <= 0)
        return;
    motion.Spline = bot.movespline->GetId();
    slot = motion;
}

// The crossing by a row of the stop the vessel stands at: onto it from the row's dock point, or off it from its deck
// point, issued into the slot. What the leg's debug line reads: the bot's start, the two points and the move
CrossingPlan IssueCrossing(Player& bot, std::optional<PassengerMotion>& slot, Transport const& transport,
                           TravelCrossing::Row const& row, TravelCrossing::Direction direction)
{
    bool const boarding = direction == TravelCrossing::Direction::Board;
    Point const dock{row.dock.x, row.dock.y, row.dock.z};
    Point const deck = DeckWorld(transport, row.deck);
    CrossingPlan plan;
    plan.first = boarding ? dock : deck;
    plan.end = boarding ? deck : dock;
    plan.from = TransportGeometry::PositionOf(bot);
    plan.startYards = Yards(plan.from, plan.first);
    plan.move = TravelCrossing::MoveOf(row, direction, plan.startYards);
    PassengerMotion motion = Intent(bot, transport, boarding ? MotionKind::Board : MotionKind::Exit, plan.end);
    motion.Jump = plan.move.jump;
    IssueTransportMove(bot, slot, motion, plan.first, plan.move);
    return plan;
}

// `route aboard` (tests): the bot is stood on the first boarding row's deck point of the stop the vessel of that entry
// nearest it stands at, a lift's or the tram's car as a ship. No explicit attach: PlayerbotAI's passenger check
// attaches it to that vessel after the teleport (BeginAboard's intent). Refused: absent, away (at no stop), no_row
void PlaceAboard(PlayerbotAI* botAI, Player* bot, uint32 entry, TravelCrossing::Spot const* at)
{
    Transport* transport = nullptr;
    float nearest = std::numeric_limits<float>::max();
    auto const consider = [&](Transport* candidate)
    {
        if (!candidate || !candidate->IsInWorld() || candidate->GetEntry() != entry)
            return;
        float const yards = candidate->GetExactDist2d(bot);
        if (yards < nearest)
        {
            transport = candidate;
            nearest = yards;
        }
    };
    for (Transport* candidate : bot->GetMap()->GetAllTransports())
        consider(candidate);
    // A lift's or the tram's cars by the spawns the crossing index names
    uint32 car = 0;
    for (TravelCrossing::Stop const& stop : TravelGraph::Instance().Crossings().StopsOf(entry))
    {
        if (!stop.guid || stop.guid == car)
            continue;
        car = stop.guid;
        consider(CarBySpawn(*bot->GetMap(), car, entry));
    }
    TravelCrossing::Stop const* const stop = transport ? StopStoodAt(*transport) : nullptr;
    char const* refusal = nullptr;
    if (!transport)
        refusal = "absent";
    else if (!stop)
        refusal = "away";
    else if (!stop->boardCount)
        refusal = "no_row";
    if (refusal)
    {
        LOG_INFO("playerbots", "[Route] {} aboard {} refused: {}", bot->GetName(), entry, refusal);
        std::string const source = std::string("refused:") + refusal;
        EventLog::Emit(bot, EventLog::EventType::Aboard, source.c_str(), entry);
        return;
    }
    // On the vessel's mesh nearest the first boarding row's deck point: the passenger check attaches a bot without a
    // move of its own only where it stands on the deck (TravelLifecycle::StandsOnDeck), and a row's deck point may lie
    // off the mesh (G99: 0.61 yd at 164871). A named offset is taken only within that check's half yard of the mesh
    TravelCrossing::Spot spot = at ? *at : stop->board[0].deck;
    TravelCrossing::Spot const rowSpot = spot;
    Position onMesh;
    bool const found = transport->GetNearestDeckPoint(Position(spot.x, spot.y, spot.z, 0.0f), onMesh);
    if (found)
        spot = {onMesh.GetPositionX(), onMesh.GetPositionY(), onMesh.GetPositionZ()};
    if (at && (!found || TravelLegs::OffsetYards({at->x, at->y, at->z}, spot) > TravelLifecycle::DECK_MESH_YARDS))
    {
        LOG_INFO("playerbots", "[Route] {} aboard {} refused: off_mesh", bot->GetName(), entry);
        EventLog::Emit(bot, EventLog::EventType::Aboard, "refused:off_mesh", entry);
        return;
    }
    Point const deck = DeckWorld(*transport, spot);
    LOG_INFO("playerbots", "[Route] {} aboard {}: deck {:.1f}, {:.1f}, {:.1f}", bot->GetName(), entry, deck.X, deck.Y,
             deck.Z);
    // PA68 trace (debug log only, once per placement): the row's deck point, the mesh point found for it, the vessel
    if (sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
    {
        Point const rowWorld = DeckWorld(*transport, rowSpot);
        float const dx = spot.x - rowSpot.x;
        float const dy = spot.y - rowSpot.y;
        float const dz = spot.z - rowSpot.z;
        LOG_DEBUG("playerbots",
                  "[Route] pa68_place bot={} entry={} guid={} stop={} stop_guid={} row_deck={:.2f},{:.2f},{:.2f} "
                  "found={} moved={:.2f} dz={:.2f} vessel={:.1f},{:.1f},{:.1f},{:.2f} row_world={:.1f},{:.1f},{:.1f} "
                  "world={:.1f},{:.1f},{:.1f} from={:.1f},{:.1f},{:.1f}",
                  bot->GetName(), entry, transport->GetSpawnId(), static_cast<int>(stop->stop), stop->guid, rowSpot.x,
                  rowSpot.y, rowSpot.z, found ? 1 : 0, std::sqrt(dx * dx + dy * dy + dz * dz), dz,
                  transport->GetPositionX(), transport->GetPositionY(), transport->GetPositionZ(),
                  transport->GetOrientation(), rowWorld.X, rowWorld.Y, rowWorld.Z, deck.X, deck.Y, deck.Z,
                  bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    }
    bot->NearTeleportTo(deck.X, deck.Y, deck.Z, bot->GetOrientation());
    botAI->GetTravel().BeginAboard(entry, transport->GetGUID().GetRawValue());
}

std::optional<Outcome> TakePassengerOutcome(Player& bot, HeldTrip& data)
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

// The leg's two stop records and, aboard, the bot's own offset on the vessel (TravelLegs::Observation), read anew
// every tick from the graph's immutable crossing index: two array lookups, nothing kept
void FillCrossings(Player const& bot, TravelLegs::Trip const& trip, TravelLegs::Observation& seen)
{
    if (bot.GetTransport())
        seen.offset = OffsetOf(bot);
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != TravelRoute::LegType::Transport)
        return;
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    TravelCrossing::Index const& crossings = TravelGraph::Instance().Crossings();
    seen.boardAt = crossings.At(leg.crossFrom);
    seen.leaveAt = crossings.At(leg.crossTo);
}

// At its transport leg the bot goes on foot before it boards (WT54-7; TravelKeepsOnFoot keeps it so): a stop lasts
// seconds, and a dismount at the crossing costs a tick of them. A flying mount's flag stays set on the ground
// (WT54-11): only a bot in the air keeps its mount
void KeepOnFootAtTransport(Player* bot, TravelLegs::Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != TravelRoute::LegType::Transport ||
        trip.passengerGuid || !bot->IsMounted() || bot->IsInCombat())
        return;
    float const ground = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    if (bot->IsFlying() && bot->GetPositionZ() - ground > TravelLegs::AIRBORNE_YARDS)
        return;
    WorldPacket emptyPacket;
    bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
}

// The mover gave a boarding row's dock point up (the walk budget of the approach ran out; PLAN 3.21 (3)): the row is
// spent and the leg goes on with the stop's next row, or fails crossing_failed without one. False: not at an approach
bool DockPointGivenUp(Player* bot, HeldTrip& data)
{
    TravelLegs::Trip& trip = data.trip;
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != TravelRoute::LegType::Transport ||
        trip.boarding != TravelLegs::Boarding::Approach || trip.passengerGuid)
        return false;
    TravelLegs::Observation seen;
    seen.nowMs = getMSTime();
    FillCrossings(*bot, trip, seen);
    TravelLegs::RowRef const ref = TravelLegs::RowNow(trip, seen);
    TravelCrossing::Row const* const row = ref.Get();
    if (!row)
        return false;
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] dock_given_up bot={} trip={} leg={} entry={} amount={} at={:.2f},{:.2f},{:.2f} "
              "dock={:.2f},{:.2f},{:.2f} yards={:.2f}",
              bot->GetGUID().GetCounter(), data.tripId, trip.legIdx, ref.stop->entry, ref.Amount(), bot->GetPositionX(),
              bot->GetPositionY(), bot->GetPositionZ(), row->dock.x, row->dock.y, row->dock.z,
              bot->GetExactDist(row->dock.x, row->dock.y, row->dock.z));
    TravelLegs::RowRefused(trip, seen);
    SendTransportNotes(bot, trip);
    data.progress.Recover();
    return true;
}

// The crossing by the current row (TravelLegs::RowNow): onto the vessel from the row's dock point, or off it from its
// deck point, the get-off's too. Issued only while no crossing is tracked and no move of the leg's is pending, with
// the bot ready (a mounted bot dismounts first) and the vessel at the stop's pose this tick. Once the spline is
// launched the rules track it (CrossIssued: `cross`). The debug line carries what hypothesis H1 reads: how far the
// bot stood from the row's first point when it started
void CrossByRow(Player* bot, HeldTrip& data, Transport& transport, TravelLegs::Observation const& seen)
{
    using TravelCrossing::Direction;
    TravelLegs::Trip& trip = data.trip;
    TravelLegs::RowRef const ref = TravelLegs::RowNow(trip, seen);
    TravelCrossing::Row const* const row = ref.Get();
    if (!row || TravelLegs::CrossUnderWay(trip) || data.passengerMotion || !PrepareTransportMove(*bot))
        return;
    if (!TransportGeometry::Available(*bot, transport) || !StandsAt(transport, *ref.stop))
        return;
    CrossingPlan const plan = IssueCrossing(*bot, data.passengerMotion, transport, *row, ref.direction);
    if (!data.passengerMotion)
        return;
    TravelLegs::CrossIssued(trip, seen);
    bool const boarding = ref.direction == Direction::Board;
    Point const& from = plan.from;
    Point const& first = plan.first;
    Point const& end = plan.end;
    TravelCrossing::Move const& move = plan.move;
    if (DebugLogged())
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] cross bot={} trip={} leg={} entry={} amount={} dir={} kind={} start_off={:.2f} "
                  "run_up={} speed={:.2f} amplitude={:.2f} seconds={:.2f} gap={:.2f} rise={:.2f} "
                  "at={:.2f},{:.2f},{:.2f} first={:.2f},{:.2f},{:.2f} end={:.2f},{:.2f},{:.2f}",
                  bot->GetGUID().GetCounter(), data.tripId, trip.legIdx, transport.GetEntry(), ref.Amount(),
                  boarding ? 0 : 1, move.jump ? "jump" : "walk", plan.startYards, move.runUp ? 1 : 0, move.speed,
                  move.amplitude, move.seconds, row->gap, TravelCrossing::RiseOf(*row, ref.direction), from.X, from.Y,
                  from.Z, first.X, first.Y, first.Z, end.X, end.Y, end.Z);
}

// Act::Deck and the get-off's deck step: aboard, the walk on the deck to the current leaving row's deck point. A path
// the vessel's path finder refuses spends the row (RowRefused); a bot that is not ready is asked again
void WalkDeckToRow(PlayerbotAI* botAI, Player* bot, HeldTrip& data, TravelLegs::Observation const& seen)
{
    Transport const* const ridden = bot->GetTransport();
    TravelCrossing::Row const* const row = TravelLegs::RowNow(data.trip, seen).Get();
    if (!ridden || !row || data.passengerMotion || !PrepareTransportMove(*bot))
        return;
    if (!DeckMove(botAI, *bot, &data, *ridden, row->deck, "exit"))
        TravelLegs::RowRefused(data.trip, seen);
}

// Act::Stand: the bot's place aboard, once per boarding (PLAN 3.21 (6)): a random deck point within
// PLACE_RADIUS_YARDS of the far stop's first leaving row's deck point, walked to on the deck. A false draw or a path
// that is not complete leaves the bot where it is: no second draw, no straight line (`place` 1 taken, 0 stayed)
void TakePlace(PlayerbotAI* botAI, Player* bot, HeldTrip& data, TravelLegs::Observation const& seen)
{
    Transport const* const ridden = bot->GetTransport();
    if (!ridden || !ReadyForTransportMove(*bot))
        return;  // asked again next tick: NotePlace was not called
    bool taken = false;
    if (seen.leaveAt && seen.leaveAt->leaveCount)
    {
        TravelCrossing::Spot const& exit = seen.leaveAt->leave[0].deck;
        Position const beside(exit.x, exit.y, exit.z);
        Position place;
        if (ridden->GetRandomDeckPoint(beside, TravelLegs::PLACE_RADIUS_YARDS, place))
            taken = DeckMove(botAI, *bot, &data, *ridden,
                             {place.GetPositionX(), place.GetPositionY(), place.GetPositionZ()}, "place");
    }
    TravelLegs::NotePlace(data.trip, taken);
}

// The corner-cut watch (PLAN 3.21 (6)): when a deck move of the leg's ended, the deck point nearest the bot's offset
// is asked for once, and `off_deck` (tenths of a yard) says when the bot stands further than OFF_DECK_YARDS off the
// vessel's mesh. A compare per tick while a deck move runs; a move another owner replaced is not judged
void WatchDeckMove(Player* bot, HeldTrip& data)
{
    if (!data.deckSpline)
        return;
    bool const own = bot->movespline->GetId() == data.deckSpline;
    if (own && !bot->movespline->Finalized())
        return;
    data.deckSpline = 0;
    Transport const* const ridden = bot->GetTransport();
    if (!own || !ridden)
        return;
    Position const offset = bot->m_movementInfo.transport.pos;
    Position nearest;
    if (!ridden->GetNearestDeckPoint(offset, nearest))
        return;
    float const yards = offset.GetExactDist(&nearest);
    if (yards > TravelLegs::OFF_DECK_YARDS)
        EventLog::Emit(bot, EventLog::EventType::Transport,
                       TravelLegs::TransportSource(TravelLegs::TransportEvent::OffDeck), ridden->GetEntry(),
                       static_cast<uint32>(std::lround(yards * 10.0f)));
}

namespace
{
// PA36 trace (debug log only): the car's surface under the bot by the passenger check's own ray (PassengerAdapter::
// Probe: from 0.5 yd above the feet straight down; it holds the bot aboard where it hits within 1.3 yd), followed here
// up to 10 yd: the yards below the feet, -1 where it finds none; the transport the engine's own hull test
// (Map::GetTransportForPos) puts the bot on (its entry, 0 none); and the bot's place in the car's frame (its offset
// from the car's position, turned by the car's heading)
struct HullProbe
{
    float belowFeet{-1.0f};
    uint32 hullEntry{0};
    float frameX{0.0f};
    float frameY{0.0f};
    float frameZ{0.0f};
};

HullProbe ProbeHull(Player* bot, Transport const* car)
{
    HullProbe probe;
    if (car)
    {
        float const dx = bot->GetPositionX() - car->GetPositionX();
        float const dy = bot->GetPositionY() - car->GetPositionY();
        float const o = car->GetOrientation();
        probe.frameX = dx * std::cos(o) + dy * std::sin(o);
        probe.frameY = dy * std::cos(o) - dx * std::sin(o);
        probe.frameZ = bot->GetPositionZ() - car->GetPositionZ();
    }
    if (car && car->m_model)
    {
        G3D::Vector3 const origin(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() + 0.5f);
        G3D::Ray const ray(origin, G3D::Vector3(0.0f, 0.0f, -1.0f));
        float distance = 10.5f;
        if (car->m_model->intersectRay(ray, distance, false, bot->GetPhaseMask(), VMAP::ModelIgnoreFlags::Nothing))
            probe.belowFeet = distance - 0.5f;
    }
    if (Transport const* const hull = bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(),
                                                                        bot->GetPositionY(), bot->GetPositionZ(), bot))
        probe.hullEntry = hull->GetEntry();
    return probe;
}

std::string HullWords(HullProbe const& probe)
{
    return Acore::StringFormat("car_frame={:.2f},{:.2f},{:.2f} car_below_feet={:.2f} engine_hull={}", probe.frameX,
                               probe.frameY, probe.frameZ, probe.belowFeet, probe.hullEntry);
}
}  // namespace

void TraceTransportStep(Player* bot, TravelLegs::Trip const& trip, TravelLegs::Observation const& seen,
                        TravelLegs::Act act, uint32 sightBefore)
{
    if (!DebugLogged() || trip.legIdx >= trip.legs.size() ||
        trip.legs[trip.legIdx].type != TravelRoute::LegType::Transport)
        return;
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    Transport const* const ridden = bot->GetTransport();
    TravelLegs::RowRef const ref = TravelLegs::RowNow(trip, seen);
    TravelCrossing::Row const* const row = ref.Get();
    // C (G331): the leave's start and a ride on past the stop, with the stand left, the need and the scan gap; A: the
    // same moment's passenger and hull view
    for (TravelLegs::TransportNote const& note : trip.transportNotes)
    {
        bool const leave = note.event == TravelLegs::TransportEvent::Leave;
        if (!leave && note.event != TravelLegs::TransportEvent::StuckAboard)
            continue;
        TravelCrossing::Stop const* const stop = ref.stop ? ref.stop : seen.leaveAt;
        float const away = row ? TravelLegs::OffsetYards(seen.offset, row->deck) : -1.0f;
        int64 needMs = -1;
        if (row)
        {
            TravelCrossing::Move const move = TravelCrossing::MoveOf(*row, ref.direction, away);
            needMs = static_cast<int64>(std::ceil(move.seconds * 1000.0f)) + TravelLegs::CROSS_SLACK_MS;
        }
        LOG_DEBUG("playerbots",
                  "[Route] pa36_leave bot={} event={} entry={} spawn={} stop={} seq={} stand_left={} need={} "
                  "away={:.2f} scan_gap_ms={} scan={} now={} progress={} window={}-{} riders={} act={} placed={} "
                  "ahead_refused={} docks_in={} {}",
                  bot->GetName(), leave ? "leave" : "stuck_aboard", leg.object, stop ? stop->guid : 0,
                  stop ? static_cast<int>(stop->stop) : -1, row ? static_cast<int>(row->seq) : -1,
                  stop ? TravelLegs::StandLeftMs(trip, *stop, seen.nowMs) : -1, needMs, away,
                  trip.sightMs - sightBefore, trip.sightMs, seen.nowMs, trip.sight.progressMs,
                  stop ? stop->arriveMs : 0, stop ? stop->departMs : 0, ridden ? ridden->GetPassengers().size() : 0,
                  TravelLegs::ActWord(act), trip.placed ? 1 : 0, trip.aheadRefused ? 1 : 0,
                  stop ? TravelLegs::DocksInMs(trip, *stop, seen.nowMs) : -1, HullWords(ProbeHull(bot, ridden)));
    }
    // Berolaos (149045): an approach that has not reached its dock 30 s into the leg, once a 10 s per bot
    if (trip.passengerGuid || trip.boarding != TravelLegs::Boarding::Approach || trip.attempted ||
        seen.nowMs - trip.legStartMs < 30000)
        return;
    {
        static std::mutex lock;
        static std::unordered_map<uint64, uint32> logged;
        std::lock_guard<std::mutex> const guard(lock);
        uint32& lastMs = logged[bot->GetGUID().GetRawValue()];
        if (lastMs && seen.nowMs - lastMs < 10000 && lastMs - trip.legStartMs < seen.nowMs - trip.legStartMs)
            return;
        lastMs = seen.nowMs;
    }
    using RoutePoint = decltype(TravelLegs::Observation::bot);
    RoutePoint const dock =
        row && ref.stop ? RoutePoint{ref.stop->mapId, row->dock.x, row->dock.y, row->dock.z} : leg.from;
    // Why the walk does not arrive: the rule's reach (3D, DOCK_REACH_YARDS) against the gap across and in height, where
    // the running spline ends, and where the world mesh's path to the dock point ends (its type: a path that ends
    // short of the point leaves the bot there, walked again each check). One path a 10 s, debug log only
    G3D::Vector3 const splineEnd = bot->movespline->Finalized() ? G3D::Vector3(seen.bot.x, seen.bot.y, seen.bot.z)
                                                                : bot->movespline->FinalDestination();
    PathGenerator path(bot);
    path.CalculatePath(dock.x, dock.y, dock.z, false);
    G3D::Vector3 const pathEnd = path.GetActualEndPosition();
    float const across = std::hypot(seen.bot.x - dock.x, seen.bot.y - dock.y);
    LOG_DEBUG("playerbots",
              "[Route] pa36_approach bot={} entry={} spawn={} stop={} seq={} leg_s={} dock={:.1f},{:.1f},{:.1f} "
              "yards={:.2f} across={:.2f} dz={:.2f} reach={:.1f} act={} moving={} motion={} at={:.1f},{:.1f},{:.1f} "
              "spline_end={:.1f},{:.1f},{:.1f} spline_end_dock={:.2f} path_type={} path_end={:.1f},{:.1f},{:.1f} "
              "path_end_dock={:.2f} path_points={}",
              bot->GetName(), leg.object, ref.stop ? ref.stop->guid : 0,
              ref.stop ? static_cast<int>(ref.stop->stop) : -1, row ? static_cast<int>(row->seq) : -1,
              (seen.nowMs - trip.legStartMs) / 1000, dock.x, dock.y, dock.z, TravelRoute::Distance(seen.bot, dock),
              across, seen.bot.z - dock.z, TravelLegs::DOCK_REACH_YARDS, TravelLegs::ActWord(act),
              bot->isMoving() ? 1 : 0, static_cast<int>(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()),
              seen.bot.x, seen.bot.y, seen.bot.z, splineEnd.x, splineEnd.y, splineEnd.z,
              (splineEnd - G3D::Vector3(dock.x, dock.y, dock.z)).length(), static_cast<uint32>(path.GetPathType()),
              pathEnd.x, pathEnd.y, pathEnd.z, (pathEnd - G3D::Vector3(dock.x, dock.y, dock.z)).length(),
              path.GetPath().size());
}

// An aboard scan riding or at the leave, for the debug log: what the stand test at the far stop saw, and at the leave
// the leaving row's numbers (the bot's distance from its deck point, the stand left, the crossing's need) and the act
void LogAboardScan(Player* bot, TravelLegs::Trip const& trip, TravelLegs::Observation const& seen, TravelLegs::Act act)
{
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    TravelLegs::TransportSight const& sight = trip.sight;
    TravelCrossing::Stop const* const stop = seen.leaveAt;
    float const yards = stop && stop->mapId == sight.at.mapId
                            ? TravelRoute::Distance(sight.at, {stop->mapId, stop->x, stop->y, stop->z})
                            : -1.0f;
    float away = -1.0f;
    int64 needMs = -1;
    if (stop && trip.leaveSpent < stop->leaveCount)
    {
        TravelCrossing::Row const& row = stop->leave[trip.leaveSpent];
        away = TravelLegs::OffsetYards(seen.offset, row.deck);
        TravelCrossing::Move const move = TravelCrossing::MoveOf(row, TravelCrossing::Direction::Leave, away);
        needMs = static_cast<int64>(std::ceil(move.seconds * 1000.0f)) + TravelLegs::CROSS_SLACK_MS;
    }
    LOG_DEBUG("playerbots",
              "[Route] {} aboard {} on map {}: {:.1f} yd from the far stop's pose on map {} (-1: another map), o "
              "{:.3f} against {:.3f}, at a stop's pose since {} (0: at none), progress {} of {} ms, mesh {}; "
              "boarding {} scan {} now {} window {}-{} ({}) stand_left {} away {:.2f} need {} act {}",
              bot->GetName(), leg.object, sight.at.mapId, yards, leg.to.mapId, sight.o, stop ? stop->o : 0.0f,
              trip.posedMs, sight.progressMs, sight.periodMs, sight.mesh ? 1 : 0, static_cast<int>(trip.boarding),
              trip.sightMs, seen.nowMs, stop ? stop->arriveMs : 0, stop ? stop->departMs : 0,
              stop && stop->hasWindow ? 1 : 0, stop ? TravelLegs::StandLeftMs(trip, *stop, seen.nowMs) : -1, away,
              needMs, static_cast<int>(act));
}

// A miss this tick, for the debug log: whether the bot stood over the hull (the passenger check's test) or beside it
void LogMisses(Player* bot, TravelLegs::Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size() || !DebugLogged())
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

// A scan of the transport: its pose (the stand test compares it with a stop record's), its period and its place in it
// (a ship's path progress, a looping car's animation progress: read against the stop's window), and whether it holds a
// vessel mesh
void SeeTransport(Player* bot, Transport* transport, TravelLegs::TransportSight& sight)
{
    sight.found = true;
    sight.guid = transport->GetGUID().GetRawValue();
    sight.at = {bot->GetMapId(), transport->GetPositionX(), transport->GetPositionY(), transport->GetPositionZ()};
    sight.o = transport->GetOrientation();
    sight.mesh = transport->GetVesselNavMesh() != nullptr;
    ReadProgress(transport, sight.periodMs, sight.progressMs);
}

namespace
{
// A transport leg's cross_failed (WT64i trace for navigation's G113, debug log only): the row (its stop, direction and
// rank from the event's amount, TravelCrossing::EventAmount; a lift's first car with that stop), the bot's place (on a
// vessel also its offset in the vessel's frame) and the row's dock point (world) and deck point (vessel frame)
void TraceCrossFailed(Player* bot, TravelLegs::TransportNote const& note)
{
    if (!sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    uint32 const stopNumber = note.amount / 100;
    auto const direction = static_cast<TravelCrossing::Direction>((note.amount / 10) % 10);
    uint32 const seq = note.amount % 10;
    TravelCrossing::Row const* row = nullptr;
    for (TravelCrossing::Stop const& stop : TravelGraph::Instance().Crossings().StopsOf(note.entry))
        for (TravelCrossing::Row const& candidate : stop.Rows(direction))
            if (!row && stop.stop == stopNumber && candidate.seq == seq)
                row = &candidate;
    TravelCrossing::Row const none{};
    TravelCrossing::Row const& shown = row ? *row : none;
    LOG_DEBUG("playerbots",
              "[Route] {} cross_failed row {} (entry {}, stop {}, {}, rank {}{}): at ({:.1f}, {:.1f}, {:.1f}) map {}, "
              "{} offset ({:.1f}, {:.1f}, {:.1f}); dock ({:.1f}, {:.1f}, {:.1f}), deck ({:.1f}, {:.1f}, {:.1f}); {}",
              bot->GetName(), note.amount, note.entry, stopNumber,
              direction == TravelCrossing::Direction::Board ? "board" : "leave", seq, row ? "" : ", no such row",
              bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(),
              bot->GetTransport() ? "aboard" : "ashore", bot->GetTransOffsetX(), bot->GetTransOffsetY(),
              bot->GetTransOffsetZ(), shown.dock.x, shown.dock.y, shown.dock.z, shown.deck.x, shown.deck.y,
              shown.deck.z, HullWords(ProbeHull(bot, bot->GetTransport())));
}
}  // namespace

void SendTransportNotes(Player* bot, TravelLegs::Trip& trip)
{
    for (TravelLegs::TransportNote const& note : trip.transportNotes)
    {
        EventLog::Emit(bot, EventLog::EventType::Transport, TravelLegs::TransportSource(note.event), note.entry,
                       note.amount);
        if (note.event == TravelLegs::TransportEvent::CrossFailed)
            TraceCrossFailed(bot, note);
    }
    trip.transportNotes.clear();
}

// The gate before a trip's end, a route request or a walk (WT19-d, TravelLegs::BeginOffFirst): true when the bot
// rides a ship or zeppelin and gets off first (`Transport off_first`)
bool OffFirstBegins(Player* bot, HeldTrip& data, TravelLegs::OffFirstEnd end, char const* why, char const* source,
                    StatusEnd::Outcome outcome)
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

namespace
{
// PA76 (G414): the bound of a rider that cannot get off (TravelLegs::StrandedStep), after a get-off tick: its
// hearthstone, the stone's own spell (no bot teleport), cast at the stop right after its rows there were refused
// again. The stone is looked at only then; boxed: OffFirst::boxed before the tick
void LeaveStranded(PlayerbotAI* botAI, Player* bot, TravelLegs::Trip& trip, TravelLegs::Observation const& seen,
                   TravelLegs::OffStep step, uint8 boxed)
{
    constexpr uint32 HEARTHSTONE_SPELL = 8690;  // the stone's spell (NewRpgRoute.cpp's HEARTHSTONE_SPELL)
    using TravelLegs::OffStep;
    using TravelLegs::Stone;
    bool const ends = step == OffStep::Off || step == OffStep::Lost || step == OffStep::GiveUp;
    if (!ends && trip.offFirst.boxed == boxed)
        return;
    Stone stone = Stone::Missing;
    if (!ends && TestConstraints::HearthAllowed(botAI->testConstraints) &&
        bot->HasItemCount(StartingHistory::ITEM_HEARTHSTONE, 1, false))
        stone =
            bot->HasSpellCooldown(HEARTHSTONE_SPELL) || bot->IsNonMeleeSpellCast(false) ? Stone::Cooling : Stone::Ready;
    TravelLegs::Stranded& stranded = botAI->GetTravel().StrandedRider();
    TravelLegs::StrandedExit const exit =
        TravelLegs::StrandedStep(stranded, trip.offFirst, step, boxed, seen.offset, stone);
    if (exit == TravelLegs::StrandedExit::None)
        return;
    bool const hearth = exit == TravelLegs::StrandedExit::Hearth;
    if (hearth && bot->IsMounted())
        CheckMountStateAction::Dismount(bot, MountRules::How::Hearth);
    bool const cast = hearth && botAI->DoSpecificAction("hearthstone", Event(), true);
    TravelLegs::NoteStrandedExit(trip, stranded, cast);
    LOG_DEBUG("playerbots", "[Route] {} stranded aboard {}: hearthstone {}", bot->GetName(), trip.offFirst.entry,
              cast ? "cast" : "not cast");
}
}  // namespace

// The get-off (TravelLegs::StepOffFirst) shares the leg's crossing and deck walk: it leaves by the leaving rows of the
// stop the vessel stands at (StopStoodAt; a stop without rows is waited out), under the same stand test
TravelLegs::OffStep GetOffFirst(PlayerbotAI* botAI, Player* bot, HeldTrip& data)
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
    bool const rides = ridden && membership.Available && membership.Guid == trip.offFirst.guid;
    if (rides)
    {
        seen.offset = OffsetOf(*bot);
        seen.leaveAt = StopStoodAt(*ridden);
    }
    WatchDeckMove(bot, data);
    // A scan also on the tick a crossing's outcome is taken: its verdict is read from a scan at or after it
    if (seen.passengerOutcome || getMSTimeDiff(data.transportScanMs, seen.nowMs) >= TravelLegs::TRANSPORT_SCAN_MS)
    {
        data.transportScanMs = seen.nowMs;
        seen.transport.scanned = true;
        if (rides)
            SeeTransport(bot, ridden, seen.transport);
    }
    uint8 const boxed = trip.offFirst.boxed;
    TravelLegs::OffStep const step = TravelLegs::StepOffFirst(trip, seen);
    if (data.passengerMotion && !TravelLegs::CrossUnderWay(trip))
        TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
    if (rides && step == TravelLegs::OffStep::Deck)
        WalkDeckToRow(botAI, bot, data, seen);
    else if (rides && step == TravelLegs::OffStep::Disembark)
        CrossByRow(bot, data, *ridden, seen);
    LeaveStranded(botAI, bot, trip, seen, step, boxed);
    SendTransportNotes(bot, trip);
    return step;
}

namespace
{
// PA39b's trace of the trip-less get-off (debug log only, bounded): a pa39b_step line when the step, the stop the
// vessel stands at, the membership or the map changes, or after more than PA39B_TRACE_GAP_MS between two steps
constexpr uint32 PA39B_TRACE_GAP_MS = 2000;

void TraceVesselLeave(PlayerbotAI* botAI, Player* bot, HeldTrip const& off, TravelLegs::OffStep step)
{
    if (!sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    BotTravel::VesselOffTrace& trace = botAI->GetTravel().VesselOffTraced();
    uint32 const now = getMSTime();
    uint32 const gap = trace.stepMs ? getMSTimeDiff(trace.stepMs, now) : 0;
    trace.stepMs = now;
    Transport const* const ridden = bot->GetTransport();
    TravelCrossing::Stop const* const at = ridden ? StopStoodAt(*ridden) : nullptr;
    auto const membership = TransportGeometry::ReadMembership(*bot);
    TravelLegs::Trip const& trip = off.trip;
    bool const aboard = membership.Available && membership.Guid == trip.offFirst.guid;
    uint32 const key = static_cast<uint32>(step) | (at ? (at->stop + 1) << 4 : 0) |
                       (membership.Available ? 1u << 12 : 0) | (aboard ? 1u << 13 : 0) | (bot->GetMapId() << 14);
    if (key == trace.key && gap <= PA39B_TRACE_GAP_MS)
        return;
    trace.key = key;
    float away = -1.0f;
    if (at && trip.offFirst.spent < at->leaveCount)
        away = TravelLegs::OffsetYards(OffsetOf(*bot), at->leave[trip.offFirst.spent].deck);
    std::optional<uint32> const pace = botAI->GetTravel().PaceMs();
    LOG_DEBUG("playerbots",
              "[Route] pa39b_step bot={} step={} gap_ms={} map={} avail={} aboard={} ridden={} at_stop={} rows={} "
              "off_stop={} spent={} stood={} stand_left={} sight_age={} away={:.1f} cross={} pace={} since_s={}",
              bot->GetName(), static_cast<uint32>(step), gap, bot->GetMapId(), membership.Available, aboard,
              ridden ? ridden->GetEntry() : 0, at ? static_cast<int32>(at->stop) : -1,
              at ? static_cast<uint32>(at->leaveCount) : 0, trip.offFirst.stop,
              static_cast<uint32>(trip.offFirst.spent), at ? TravelLegs::Stood(trip, *at) : false,
              at ? TravelLegs::StandLeftMs(trip, *at, now) : -1, trip.sightMs ? getMSTimeDiff(trip.sightMs, now) : 0,
              away, trip.crossMs != 0, pace ? *pace : 0,
              trip.offFirst.sinceMs ? getMSTimeDiff(trip.offFirst.sinceMs, now) / 1000 : 0);
}
}  // namespace

// PA39b (G340): a bot that rides a vessel with no trip of its own (its status runs on the deck: rest, wander) gets off
// at the vessel's next stop by that stop's leaving rows, as a trip's get-off does (GetOffFirst) on a trip without legs,
// bounded by one period: `Transport off_first`, then `left` ashore or `stuck_aboard`, after which the next check begins
// again. A lift's or the tram's car too where its stop records have leaving rows (G412). O(1): the bot's own
// membership and its master's transport; a car's records by a binary search, only for a car's rider that would begin
bool VesselLeaveStep(PlayerbotAI* botAI, Player* bot)
{
    BotTravel& travel = botAI->GetTravel();
    HeldTrip& off = travel.VesselOff();
    uint32 const sinceMs = off.trip.offFirst.sinceMs;
    auto const membership = TransportGeometry::ReadMembership(*bot);
    Transport const* const ridden = bot->GetTransport();
    uint64 const aboard = membership.Available && ridden ? membership.Guid : 0;
    Player* const master = botAI->GetMaster();
    bool const followed =
        master && master != bot &&
        (!GET_PLAYERBOT_AI(master) || (master->GetTransport() && master->GetTransport() == bot->GetTransport()));
    bool const owned = followed || (botAI->GetWork() & BotWork::Aboard) || bot->InBattleground();
    TravelLegs::VesselLeaveFacts facts{aboard, travel.Trip() != nullptr, owned, sinceMs != 0};
    facts.car = aboard && ridden->IsStaticTransport();
    if (facts.car && !facts.tripHeld && !owned && !sinceMs)
        for (TravelCrossing::Stop const& stop :
             TravelGraph::Instance().Crossings().StopsOf(ridden->GetEntry(), ridden->GetSpawnId()))
            facts.carLeaves = facts.carLeaves || stop.leaveCount != 0;
    TravelLegs::VesselLeave const act = TravelLegs::DecideVesselLeave(facts);
    if (act == TravelLegs::VesselLeave::None)
    {
        if (sinceMs || off.passengerMotion)
        {
            TransportGeometry::DiscardMotion(*bot, off.passengerMotion);
            off = HeldTrip{};
        }
        return false;
    }
    if (act == TravelLegs::VesselLeave::Begin)
    {
        off = HeldTrip{};
        travel.VesselOffTraced() = {};
        if (!bot->IsStandState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        return OffFirstBegins(bot, off);
    }
    uint32 const entry = off.trip.offFirst.entry;
    TravelLegs::OffStep const step = GetOffFirst(botAI, bot, off);
    TraceVesselLeave(botAI, bot, off, step);
    switch (step)
    {
        case TravelLegs::OffStep::Off:
            TravelLegs::NoteLeft(off.trip, entry, sinceMs, getMSTime());
            SendTransportNotes(bot, off.trip);
            LOG_DEBUG("playerbots", "[Route] {} left transport {} without a trip", bot->GetName(), entry);
            off = HeldTrip{};
            return false;
        case TravelLegs::OffStep::GiveUp:
        case TravelLegs::OffStep::Lost:
            off = HeldTrip{};
            return false;
        default:
            return true;
    }
}

// A bot on its own transport leg leaves the vessel to the leg: the follower's step does nothing
bool OnOwnTransportLeg(PlayerbotAI* botAI)
{
    auto const* const travel = botAI->GetTravel().Trip();
    return travel && (travel->passengerMotion || AtTransportLeg(travel->trip));
}

// The stop a bot master leaves the vessel both ride at: its transport leg's far stop record while that leg's vessel is
// the one ridden (null for a master without one: a player, a bot off its transport leg). O(1)
TravelRoute::Leg const* MasterLegOn(Player* master, Transport const& vessel)
{
    PlayerbotAI* const masterAI = master ? GET_PLAYERBOT_AI(master) : nullptr;
    auto const* const travel = masterAI ? masterAI->GetTravel().Trip() : nullptr;
    if (!travel || !AtTransportLeg(travel->trip))
        return nullptr;
    TravelRoute::Leg const& leg = travel->trip.legs[travel->trip.legIdx];
    return leg.object == vessel.GetEntry() ? &leg : nullptr;
}

TravelCrossing::Stop const* MasterLeavesAt(Player* master, Transport const& vessel)
{
    TravelRoute::Leg const* const leg = MasterLegOn(master, vessel);
    return leg ? TravelGraph::Instance().Crossings().At(leg->crossTo) : nullptr;
}

// Whether the master, off the owed vessel, stands nearer the bot's stop (its first boarding row's dock point) than the
// stop the bot would leave at (its first leaving row's): it came back (WT70e, PLAN 3.21 (8))
bool MasterBack(Player* master, TravelCrossing::Stop const& from, TravelCrossing::Stop const& to)
{
    if (!from.boardCount || !to.leaveCount)
        return false;
    TravelCrossing::Spot const& homeDock = from.board[0].dock;
    TravelCrossing::Spot const& farDock = to.leave[0].dock;
    return master->GetExactDist(homeDock.x, homeDock.y, homeDock.z) <
           master->GetExactDist(farDock.x, farDock.y, farDock.z);
}

// The vessel a follower works at and the direction: an owed ride's vessel (WT70e; by its guid on the bot's map, null
// while it is on another), else the master aboard and the bot not, the master's vessel (Board); the bot aboard and its
// master not, its own (Leave); else none. With it the stop the vessel stands at (one StopAt search), that record's
// index, the vessel's progress and period, who stands on it, a bot master's leg's stops and, owing, the owed stops'
// records and whether the master came back. Both aboard the same vessel: no stop, the stop the master leaves it at
// (ahead)
Transport* FollowVessel(Player* bot, Player* master, TravelLegs::FollowCrossing const& follow,
                        TravelLegs::FollowView& view)
{
    Transport* const masterVessel = master->GetTransport();
    Transport* const ridden = bot->GetTransport();
    TravelCrossing::Index const& crossings = TravelGraph::Instance().Crossings();
    uint32 periodMs = 0;
    if (ridden && ridden == masterVessel)
    {
        view.vessel = ridden->GetGUID().GetRawValue();
        view.entry = ridden->GetEntry();
        view.aboard = view.masterAboard = true;
        view.ahead = TransportGeometry::Available(*bot, *ridden) ? MasterLeavesAt(master, *ridden) : nullptr;
        if (view.ahead)
            ReadProgress(ridden, periodMs, view.progressMs);
        return ridden;
    }
    Transport* vessel = nullptr;
    if (follow.owed.vessel)
        vessel = bot->GetMap()->GetTransport(ObjectGuid(follow.owed.vessel));
    else if (masterVessel && !ridden)
        vessel = masterVessel;
    else if (ridden && !masterVessel)
        vessel = ridden;
    if (follow.owed.vessel)
    {
        view.owedStop = crossings.At(follow.owed.from);
        view.owedLeave = crossings.At(follow.owed.to);
        view.masterBack =
            !masterVessel && view.owedStop && view.owedLeave && MasterBack(master, *view.owedStop, *view.owedLeave);
    }
    if (!vessel)
        return nullptr;
    view.vessel = vessel->GetGUID().GetRawValue();
    view.entry = vessel->GetEntry();
    view.aboard = ridden == vessel;
    view.masterAboard = masterVessel == vessel;
    if (view.aboard)
        view.direction = TravelCrossing::Direction::Leave;
    if (!TransportGeometry::Available(*bot, *vessel))
        return vessel;
    ReadProgress(vessel, periodMs, view.progressMs);
    view.periodMs = periodMs;
    view.stop = StopStoodAt(*vessel);
    if (view.stop)
        view.stopIndex = crossings.IndexOf(*view.stop);
    TravelRoute::Leg const* const masterLeg =
        view.masterAboard && !view.aboard ? MasterLegOn(master, *vessel) : nullptr;
    if (masterLeg)
    {
        view.masterFrom = masterLeg->crossFrom;
        view.masterTo = masterLeg->crossTo;
    }
    return vessel;
}

// The master a follower works with: in the world in the bot's own map (the same Map object, so on this map thread: the
// step reads the master's travel data; BotTravel's MasterTransportGuid asks the same), and the bot alive (the follow
// strategy, and with it the "follow aboard" trigger, is in the dead engine too: a dead bot takes no deck walk and no
// crossing). Else none: the step has nothing to do
Player* FollowedMaster(PlayerbotAI* botAI, Player* bot)
{
    Player* const master = botAI->GetMaster();
    return master && bot->IsAlive() && master->IsInWorld() && bot->IsInWorld() && master->FindMap() == bot->FindMap()
               ? master
               : nullptr;
}

// A follower at its master's vessel (WT70d, PLAN 3.21 (8); BotTravel::FollowAboard): the master aboard and the bot not,
// the boarding rows of the stop that vessel stands at; the bot aboard and its master not, the leaving rows of its own
// vessel's stop. The rules decide (TravelLegs::FollowStep); this reads the vessel (one StopAt search), issues the deck
// walk and the crossing by the leg's own functions, and hands a walk to the dock point back to the follow action
TravelLegs::FollowAct FollowAboardStep(PlayerbotAI* botAI, Player* bot, TravelLegs::FollowCrossing& follow,
                                       std::optional<PassengerMotion>& motion, Position& walkTo)
{
    Player* const master = FollowedMaster(botAI, bot);
    if (!master || OnOwnTransportLeg(botAI))
        return TravelLegs::FollowAct::None;
    Transport* const ridden = bot->GetTransport();
    TravelLegs::FollowView view;
    view.nowMs = getMSTime();
    if (motion)
    {
        view.outcome = TravelLifecycle::PassengerOutcome(*motion, TransportGeometry::ReadMembership(*bot));
        if (view.outcome)
            TransportGeometry::DiscardMotion(*bot, motion);
    }
    Transport* const vessel = FollowVessel(bot, master, follow, view);
    TravelCrossing::Stop const* const stop = view.stop;
    view.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    if (ridden)
        view.offset = OffsetOf(*bot);
    std::vector<TravelLegs::TransportNote> notes;
    TravelLegs::FollowDecision const decision = TravelLegs::FollowStep(follow, view, notes);
    // A crossing the rules track no more (no outcome after CROSS_MAX_MS) is discarded, as the leg's is (StepTravel):
    // the next deck walk or crossing needs the slot free
    if (motion && !follow.crossMs)
        TransportGeometry::DiscardMotion(*bot, motion);
    TravelCrossing::Stop const* const shown = stop            ? stop
                                              : !decision.row ? nullptr
                                              : view.ahead    ? view.ahead
                                                              : view.owedStop;
    if (shown && DebugLogged())
    {
        // Only while the vessel stands at a stop with a record, or riding ahead of the master's docking: a few lines a
        // docking (act 0: nothing to do; dir ahead, owed: left_ms is the time until that docking)
        std::int64_t const leftMs = !stop ? TravelLegs::DocksInMs(*shown, view.progressMs)
                                    : stop->hasWindow && stop->periodMs
                                        ? TravelLegs::WindowLeftMs(*stop, view.progressMs)
                                        : -2;
        TravelCrossing::Row const* const row = decision.row;
        LOG_DEBUG("playerbots",
                  "[Route] follow_aboard bot={} entry={} stop={} dir={} act={} spent={} dock_yd={:.1f} deck_yd={:.1f} "
                  "left_ms={} move_s={:.2f}",
                  bot->GetGUID().GetCounter(), shown->entry, shown->stop,
                  !stop                                                ? (view.ahead ? "ahead" : "owed")
                  : view.direction == TravelCrossing::Direction::Board ? "board"
                                                                       : "leave",
                  static_cast<int>(decision.act), follow.spent,
                  row ? bot->GetExactDist(row->dock.x, row->dock.y, row->dock.z) : -1.0f,
                  ridden && row ? TravelLegs::OffsetYards(view.offset, row->deck) : -1.0f, leftMs,
                  decision.move.seconds);
    }
    switch (decision.act)
    {
        case TravelLegs::FollowAct::Walk:
            walkTo.Relocate(decision.row->dock.x, decision.row->dock.y, decision.row->dock.z);
            break;
        case TravelLegs::FollowAct::Deck:
            if (!motion && PrepareTransportMove(*bot) &&
                !DeckMove(botAI, *bot, nullptr, *vessel, decision.row->deck, "follow"))
            {
                if (stop)
                    TravelLegs::FollowRowRefused(follow, *stop, view.direction, notes);
                else
                    TravelLegs::FollowAheadRefused(follow);
            }
            break;
        case TravelLegs::FollowAct::Cross:
            if (!motion && PrepareTransportMove(*bot))
            {
                IssueCrossing(*bot, motion, *vessel, *decision.row, view.direction);
                if (motion)
                    TravelLegs::FollowCrossIssued(follow, *stop, view.direction, view.nowMs, notes);
            }
            break;
        default:
            break;
    }
    for (TravelLegs::TransportNote const& note : notes)
        EventLog::Emit(bot, EventLog::EventType::Transport, TravelLegs::TransportSource(note.event), note.entry,
                       note.amount);
    return decision.act;
}

// Whether a follower has work at its master's vessel (TravelLegs::FollowDue; the "follow aboard" trigger and action):
// a crossing under way, the vessel at a stop with an unspent row, or riding with its master the walk ahead of the
// master's docking. The StopAt search runs only while one of the bot and its master stands on a vessel and the other
// does not; riding together it reads the master's leg (O(1))
bool FollowAboardDue(PlayerbotAI* botAI, Player* bot, TravelLegs::FollowCrossing const& follow)
{
    Player* const master = FollowedMaster(botAI, bot);
    if (!master || OnOwnTransportLeg(botAI))
        return false;
    TravelLegs::FollowView view;
    FollowVessel(bot, master, follow, view);
    return TravelLegs::FollowDue(follow, view);
}

// The pace of a transport-leg bot's checks (TravelLegs::CheckPaceMs: ScanPaceMs, aimed at the docking; WT70e, PLAN 3.21
// (13), WT70e-d) by its leg's stop records; staggerMs: what the engine adds to the checked bot's next check
uint32 TransportLegPaceMs(Player const& bot, TravelLegs::Trip const& trip, uint32 staggerMs)
{
    TravelCrossing::Stop const* boardAt = nullptr;
    TravelCrossing::Stop const* leaveAt = nullptr;
    if (AtTransportLeg(trip))
    {
        TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
        boardAt = TravelGraph::Instance().Crossings().At(leg.crossFrom);
        leaveAt = TravelGraph::Instance().Crossings().At(leg.crossTo);
    }
    return TravelLegs::CheckPaceMs(trip, boardAt, leaveAt, bot.GetMapId(), getMSTime(), staggerMs);
}

// A follower's pace (TravelLegs::FollowPaceMs; BotTravel::PaceMs): its own step around a docking, else its master's
// transport leg's pace (the master's travel data, read on this map thread as MasterLeavesAt does), else none
std::optional<uint32> FollowAboardPaceMs(PlayerbotAI* botAI, Player* bot, TravelLegs::FollowCrossing const& follow)
{
    Player* const master = FollowedMaster(botAI, bot);
    if (!master || OnOwnTransportLeg(botAI))
        return std::nullopt;
    PlayerbotAI* const masterAI = GET_PLAYERBOT_AI(master);
    auto const* const travel = masterAI ? masterAI->GetTravel().Trip() : nullptr;
    std::optional<uint32> masterMs;
    if (travel && TravelLegs::PacesFollowers(travel->trip))
        masterMs = TransportLegPaceMs(*master, travel->trip, botAI->GetTactics().GetCheckStaggerMs());
    TravelLegs::FollowView view;
    FollowVessel(bot, master, follow, view);
    return TravelLegs::FollowPaceMs(follow, view, masterMs);
}

// The time left of the bot's own crossing while its spline runs (the move the passenger check judges), else 0
static int32 CrossingLeftMs(Player const& bot, std::optional<TravelLifecycle::PassengerMotion> const& motion)
{
    if (!motion || motion->Result != TravelLifecycle::Outcome::Pending || bot.movespline->GetId() != motion->Spline ||
        bot.movespline->Finalized())
        return 0;
    return bot.movespline->timeElapsed();
}

// BotTravel::PaceMs's scan part (WT70e, PLAN 3.21 (13)): a transport leg or the get-off by ScanPaceMs; a follower whose
// own step is around a docking, or whose master is on a transport leg, by the same rule; an errand's tame or stable
// stop (TravelKeepsOnFoot; upkeep's WU21) at the pace it had before WT70d. None: not the scan's to set. The bot's own
// crossing in flight, the leg's or the follower's (followMotion), holds the next check until its end
// (TravelLegs::CrossingPaceMs, WT70e-b3)
std::optional<uint32> TravelScanPaceMs(PlayerbotAI* botAI, Player* bot, TravelLegs::FollowCrossing const& follow,
                                       std::optional<TravelLifecycle::PassengerMotion> const& followMotion)
{
    auto const* const travel = botAI->GetTravel().Trip();
    HeldTrip const& vesselOff = botAI->GetTravel().VesselOff();
    std::optional<uint32> paceMs;
    if (travel && (travel->trip.offFirst.sinceMs || TravelLegs::PacesFollowers(travel->trip)))
        paceMs = TransportLegPaceMs(*bot, travel->trip, botAI->GetTactics().GetCheckStaggerMs());
    else if (vesselOff.trip.offFirst.sinceMs)
        paceMs = TransportLegPaceMs(*bot, vesselOff.trip, botAI->GetTactics().GetCheckStaggerMs());
    else if (std::optional<uint32> const followMs = FollowAboardPaceMs(botAI, bot, follow))
        paceMs = followMs;
    else if (TravelKeepsOnFoot(botAI))
        paceMs = TravelLegs::TRANSPORT_SLOW_SCAN_MS;
    std::optional<TravelLifecycle::PassengerMotion> const& own = travel && travel->passengerMotion
                                                                     ? travel->passengerMotion
                                                                 : vesselOff.passengerMotion ? vesselOff.passengerMotion
                                                                                             : followMotion;
    return TravelLegs::CrossingPaceMs(paceMs, CrossingLeftMs(*bot, own));
}

// The 'lost while leaving' class (WT70e-b): at the passenger sample that ends a crossing without success, the spline
// the bot runs now against the crossing's own (another id: a later move replaced it; the same id finalized short of
// its end: it was stopped), the movement generator on top and the engine's last action. One debug line
void TracePassengerEnd(PlayerbotAI* botAI, Player* bot, TravelLifecycle::PassengerMotion const& motion,
                       TravelLifecycle::Outcome before)
{
    using TravelLifecycle::Outcome;
    if (before != Outcome::Pending || (motion.Result != Outcome::Cancelled && motion.Result != Outcome::Lost) ||
        !DebugLogged())
        return;
    Engine* const engine = botAI->GetCurrentEngine();
    Transport const* const ridden = bot->GetTransport();
    LOG_DEBUG("playerbots",
              "[Route] passenger_end bot={} result={} kind={} jump={} transport={} on={} motion_spline={} "
              "bot_spline={} finalized={} spline_local={} gen={} last_action={} at={:.2f},{:.2f},{:.2f} "
              "local={:.2f},{:.2f},{:.2f} end={:.2f},{:.2f},{:.2f} {}",
              bot->GetGUID().GetCounter(), motion.Result == Outcome::Lost ? "lost" : "cancelled",
              motion.Kind == TravelLifecycle::MotionKind::Exit ? "exit" : "board", motion.Jump, motion.Transport,
              ridden ? ridden->GetGUID().GetRawValue() : 0, motion.Spline, bot->movespline->GetId(),
              bot->movespline->Finalized(), bot->movespline->onTransport,
              static_cast<int>(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()),
              engine ? engine->GetLastAction() : std::string("-"), bot->GetPositionX(), bot->GetPositionY(),
              bot->GetPositionZ(), bot->GetTransOffsetX(), bot->GetTransOffsetY(), bot->GetTransOffsetZ(),
              motion.Endpoint.X, motion.Endpoint.Y, motion.Endpoint.Z, HullWords(ProbeHull(bot, ridden)));
}

// The follow action's walk to the current boarding row's dock point ended short of it or was refused for lack of a
// path (TravelLegs::FollowWalkEnded): true, walk again; false, the row is spent (cross_failed) and the next is taken
bool FollowWalkEnded(Player* bot, TravelLegs::FollowCrossing& follow, float yards, bool refused)
{
    TravelCrossing::Stop const* const stop = TravelGraph::Instance().Crossings().At(follow.stop);
    if (!stop)
        return false;
    std::vector<TravelLegs::TransportNote> notes;
    bool const again = TravelLegs::FollowWalkEnded(follow, *stop, yards, refused, notes);
    for (TravelLegs::TransportNote const& note : notes)
        EventLog::Emit(bot, EventLog::EventType::Transport, TravelLegs::TransportSource(note.event), note.entry,
                       note.amount);
    return again;
}

void WakeFollowers(Player* bot, TravelLegs::Trip& trip)
{
    if (!TravelLegs::WakeFollowers(trip))
        return;
    Group* const group = bot->GetGroup();
    uint32 woken = 0;
    for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref; ref = ref->next())
    {
        Player* const member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMap() != bot->GetMap())
            continue;
        PlayerbotAI* const memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI || memberAI->GetMaster() != bot)
            continue;
        // Same map, same map thread: the follower's next check at the scan's pace
        memberAI->SetNextCheckDelay(TravelLegs::TRANSPORT_SCAN_MS);
        ++woken;
    }
    if (DebugLogged())
        LOG_DEBUG("playerbots", "[Route] followers_woken bot={} leg={} woken={}", bot->GetGUID().GetCounter(),
                  trip.legIdx, woken);
}

// The vessel has stop records (a ship or zeppelin by its entry, a car by its spawn): upstream's boarding assist is not
// for it (WT.52)
bool HasStopRecords(Transport const& transport)
{
    uint32 const car = transport.IsStaticTransport() ? transport.GetSpawnId() : 0;
    return !TravelGraph::Instance().Crossings().StopsOf(transport.GetEntry(), car).empty();
}

bool PassengerOwnsTravel(Player const& bot, HeldTrip const& data)
{
    TravelLegs::Aboard const ride{bot.GetTransport() || bot.HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT),
                                  bot.IsInFlight(), bot.GetVehicle() != nullptr};
    return TravelLegs::OwnerOf(data.trip, data.passengerMotion.has_value(), ride) == TravelLegs::TravelOwner::Passenger;
}
}  // namespace NewRpgInternal

bool TravelHoldsUpkeep(PlayerbotAI* botAI)
{
    // The engine's facts for TravelLegs::HoldsUpkeep: the held trip and the docking its leg needs (the stop the scan's
    // pace reads, PacedStop; two index lookups). O(1)
    auto const* const data = botAI->GetTravel().Trip();
    if (!data)
        return false;
    TravelLegs::Trip const& trip = data->trip;
    std::int64_t docksInMs = TravelLegs::NO_DEADLINE;
    if (AtTransportLeg(trip))
    {
        TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
        TravelCrossing::Stop const* const needs =
            TravelLegs::PacedStop(trip, TravelGraph::Instance().Crossings().At(leg.crossFrom),
                                  TravelGraph::Instance().Crossings().At(leg.crossTo));
        if (needs)
            docksInMs = TravelLegs::DocksInMs(trip, *needs, getMSTime());
    }
    return TravelLegs::HoldsUpkeep(&trip, docksInMs);
}
