/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

#include "BotTravel.h"
#include "CensusMgr.h"
#include "CheckMountStateAction.h"
#include "Group.h"
#include "Map.h"
#include "MeshMoveRules.h"
#include "MotionMaster.h"
#include "MountRules.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringFormat.h"
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

// A walk on the deck (PLAN 3.21 (6)): the core's vessel path finder from the bot to a deck point (an offset; asked in
// world points at the vessel's pose of this call) and its answer issued at once as a spline, which rides in the
// vessel's frame. Only a complete NORMAL path that ends within DECK_REACH_YARDS of the point is walked: no straight
// line, no second try, no search. Counted as a `deck` move on the path's answer (MeshMoveRules.h). A traced bot's
// path is logged point by point, in offsets. A follower's move (data null) is neither traced nor watched
bool DeckMove(PlayerbotAI* botAI, Player& bot, NewRpgInfo::Travel* data, Transport const& ridden,
              TravelCrossing::Spot const& to, char const* what)
{
    if (!ridden.GetVesselNavMesh())
        return false;
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
    if (data && !data->place.empty() && DebugLogged())
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
        return false;
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
void PlaceAboard(PlayerbotAI* botAI, Player* bot, uint32 entry)
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
        EventLog::Emit(bot, BotTravel::ABOARD_EVENT_TYPE, source.c_str(), entry);
        return;
    }
    Point const deck = DeckWorld(*transport, stop->board[0].deck);
    LOG_INFO("playerbots", "[Route] {} aboard {}: deck {:.1f}, {:.1f}, {:.1f}", bot->GetName(), entry, deck.X, deck.Y,
             deck.Z);
    bot->NearTeleportTo(deck.X, deck.Y, deck.Z, bot->GetOrientation());
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
bool DockPointGivenUp(Player* bot, NewRpgInfo::Travel& data)
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
void CrossByRow(Player* bot, NewRpgInfo::Travel& data, Transport& transport, TravelLegs::Observation const& seen)
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
void WalkDeckToRow(PlayerbotAI* botAI, Player* bot, NewRpgInfo::Travel& data, TravelLegs::Observation const& seen)
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
void TakePlace(PlayerbotAI* botAI, Player* bot, NewRpgInfo::Travel& data, TravelLegs::Observation const& seen)
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
void WatchDeckMove(Player* bot, NewRpgInfo::Travel& data)
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
        EventLog::Emit(bot, TravelLegs::TRANSPORT_EVENT_TYPE,
                       TravelLegs::TransportSource(TravelLegs::TransportEvent::OffDeck), ridden->GetEntry(),
                       static_cast<uint32>(std::lround(yards * 10.0f)));
}

// An aboard scan short of the leave, for the debug log: what the stand test at the far stop saw
void LogAboardScan(Player* bot, TravelLegs::Trip const& trip, TravelLegs::Observation const& seen)
{
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    TravelLegs::TransportSight const& sight = trip.sight;
    TravelCrossing::Stop const* const stop = seen.leaveAt;
    float const yards = stop && stop->mapId == sight.at.mapId
                            ? TravelRoute::Distance(sight.at, {stop->mapId, stop->x, stop->y, stop->z})
                            : -1.0f;
    LOG_DEBUG("playerbots",
              "[Route] {} aboard {} on map {}: {:.1f} yd from the far stop's pose on map {} (-1: another map), o "
              "{:.3f} against {:.3f}, at a stop's pose since {} (0: at none), progress {} of {} ms, mesh {}",
              bot->GetName(), leg.object, sight.at.mapId, yards, leg.to.mapId, sight.o, stop ? stop->o : 0.0f,
              trip.posedMs, sight.progressMs, sight.periodMs, sight.mesh ? 1 : 0);
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

// The get-off (TravelLegs::StepOffFirst) shares the leg's crossing and deck walk: it leaves by the leaving rows of the
// stop the vessel stands at (StopStoodAt; a stop without rows is waited out), under the same stand test
TravelLegs::OffStep GetOffFirst(PlayerbotAI* botAI, Player* bot, NewRpgInfo::Travel& data)
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
    TravelLegs::OffStep const step = TravelLegs::StepOffFirst(trip, seen);
    if (data.passengerMotion && !TravelLegs::CrossUnderWay(trip))
        TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
    if (rides && step == TravelLegs::OffStep::Deck)
        WalkDeckToRow(botAI, bot, data, seen);
    else if (rides && step == TravelLegs::OffStep::Disembark)
        CrossByRow(bot, data, *ridden, seen);
    SendTransportNotes(bot, trip);
    return step;
}

// A bot on its own transport leg leaves the vessel to the leg: the follower's step does nothing
bool OnOwnTransportLeg(PlayerbotAI* botAI)
{
    auto const* const travel = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    return travel && (travel->passengerMotion || AtTransportLeg(travel->trip));
}

// The stop a bot master leaves the vessel both ride at: its transport leg's far stop record while that leg's vessel is
// the one ridden (null for a master without one: a player, a bot off its transport leg). O(1)
TravelCrossing::Stop const* MasterLeavesAt(Player* master, Transport const& vessel)
{
    PlayerbotAI* const masterAI = master ? GET_PLAYERBOT_AI(master) : nullptr;
    auto const* const travel = masterAI ? std::get_if<NewRpgInfo::Travel>(&masterAI->rpgInfo.data) : nullptr;
    if (!travel || !AtTransportLeg(travel->trip))
        return nullptr;
    TravelRoute::Leg const& leg = travel->trip.legs[travel->trip.legIdx];
    return leg.object == vessel.GetEntry() ? TravelGraph::Instance().Crossings().At(leg.crossTo) : nullptr;
}

// The vessel a follower works at and the direction: the master aboard and the bot not, the master's vessel (Board);
// the bot aboard and its master not, its own (Leave); else none. With it the stop the vessel stands at (one StopAt
// search) and that record's index. Both aboard the same vessel: no stop, the stop the master leaves it at (ahead) and
// the vessel's progress
Transport* FollowVessel(Player* bot, Player* master, Transport* masterVessel, TravelLegs::FollowView& view)
{
    Transport* const ridden = bot->GetTransport();
    if (ridden && ridden == masterVessel)
    {
        view.ahead = TransportGeometry::Available(*bot, *ridden) ? MasterLeavesAt(master, *ridden) : nullptr;
        if (view.ahead)
        {
            uint32 periodMs = 0;
            ReadProgress(ridden, periodMs, view.progressMs);
        }
        return ridden;
    }
    Transport* vessel = nullptr;
    if (masterVessel && !ridden)
        vessel = masterVessel;
    else if (ridden && !masterVessel)
    {
        vessel = ridden;
        view.direction = TravelCrossing::Direction::Leave;
    }
    view.stop = vessel && TransportGeometry::Available(*bot, *vessel) ? StopStoodAt(*vessel) : nullptr;
    if (view.stop)
    {
        view.stopIndex = TravelGraph::Instance().Crossings().IndexOf(*view.stop);
        uint32 periodMs = 0;
        ReadProgress(vessel, periodMs, view.progressMs);
    }
    return vessel;
}

// The master a follower works with: in the world on the bot's map (else none: the step has nothing to do)
Player* FollowedMaster(PlayerbotAI* botAI, Player* bot)
{
    Player* const master = botAI->GetMaster();
    return master && master->IsInWorld() && bot->IsInWorld() && master->GetMapId() == bot->GetMapId() ? master
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
    Transport* const vessel = FollowVessel(bot, master, master->GetTransport(), view);
    TravelCrossing::Stop const* const stop = view.stop;
    view.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    if (ridden)
        view.offset = OffsetOf(*bot);
    std::vector<TravelLegs::TransportNote> notes;
    TravelLegs::FollowDecision const decision = TravelLegs::FollowStep(follow, view, notes);
    TravelCrossing::Stop const* const shown = stop ? stop : decision.row ? view.ahead : nullptr;
    if (shown && DebugLogged())
    {
        // Only while the vessel stands at a stop with a record, or riding ahead of the master's docking: a few lines a
        // docking (act 0: nothing to do; dir ahead: left_ms is the time until that docking)
        std::int64_t const leftMs = !stop ? TravelLegs::DocksInMs(*shown, view.progressMs)
                                    : stop->hasWindow && stop->periodMs
                                        ? TravelLegs::WindowLeftMs(*stop, view.progressMs)
                                        : -2;
        TravelCrossing::Row const* const row = decision.row;
        LOG_DEBUG("playerbots",
                  "[Route] follow_aboard bot={} entry={} stop={} dir={} act={} spent={} dock_yd={:.1f} deck_yd={:.1f} "
                  "left_ms={} move_s={:.2f}",
                  bot->GetGUID().GetCounter(), shown->entry, shown->stop,
                  !stop                                                ? "ahead"
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
        EventLog::Emit(bot, TravelLegs::TRANSPORT_EVENT_TYPE, TravelLegs::TransportSource(note.event), note.entry,
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
    FollowVessel(bot, master, master->GetTransport(), view);
    return TravelLegs::FollowDue(follow, view);
}

// Whether a follower checks at the leg's scan pace (TravelLegs::FollowPaced; BotTravel::PaceMs): its master on a
// transport leg, its walk to the dock included (TravelLegs::PacesFollowers; the master's travel data, read on this map
// thread as MasterLeavesAt does), or its step due
bool FollowAboardPaced(PlayerbotAI* botAI, Player* bot, TravelLegs::FollowCrossing const& follow)
{
    Player* const master = FollowedMaster(botAI, bot);
    if (!master || OnOwnTransportLeg(botAI))
        return false;
    PlayerbotAI* const masterAI = GET_PLAYERBOT_AI(master);
    auto const* const travel = masterAI ? std::get_if<NewRpgInfo::Travel>(&masterAI->rpgInfo.data) : nullptr;
    TravelLegs::FollowView view;
    FollowVessel(bot, master, master->GetTransport(), view);
    return TravelLegs::FollowPaced(follow, view, travel && TravelLegs::PacesFollowers(travel->trip));
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
        EventLog::Emit(bot, TravelLegs::TRANSPORT_EVENT_TYPE, TravelLegs::TransportSource(note.event), note.entry,
                       note.amount);
    return again;
}

// One debug line per check of a follower whose master is on a transport leg (WT70d-b): the delay to its next check
// and its source (scan: the leg's pace; react: the follow code's), FollowPaced, FollowDue and the engine's last action
void TraceFollowCheck(PlayerbotAI* botAI, Player* bot, TravelLegs::FollowCrossing const& follow, uint32 delayMs,
                      char const* lastAction)
{
    if (!DebugLogged())
        return;
    Player* const master = FollowedMaster(botAI, bot);
    PlayerbotAI* const masterAI = master ? GET_PLAYERBOT_AI(master) : nullptr;
    auto const* const travel = masterAI ? std::get_if<NewRpgInfo::Travel>(&masterAI->rpgInfo.data) : nullptr;
    if (!travel || !TravelLegs::PacesFollowers(travel->trip))
        return;
    bool const own = OnOwnTransportLeg(botAI);
    bool const paced = !own && FollowAboardPaced(botAI, bot, follow);
    bool const due = !own && FollowAboardDue(botAI, bot, follow);
    LOG_DEBUG(
        "playerbots",
        "[Route] follow_check bot={} master={} master_boarding={} delay_ms={} source={} combat={} paced={} due={} "
        "own_leg={} last_action={} yd={:.1f}",
        bot->GetGUID().GetCounter(), master->GetGUID().GetCounter(), static_cast<int>(travel->trip.boarding), delayMs,
        !bot->IsInCombat() && (paced || own) ? "scan" : "react", bot->IsInCombat(), paced, due, own, lastAction,
        bot->GetExactDist(master));
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

bool PassengerOwnsTravel(Player const& bot, NewRpgInfo::Travel const& data)
{
    TravelLegs::Aboard const ride{bot.GetTransport() || bot.HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT),
                                  bot.IsInFlight(), bot.GetVehicle() != nullptr};
    return TravelLegs::OwnerOf(data.trip, data.passengerMotion.has_value(), ride) == TravelLegs::TravelOwner::Passenger;
}
}  // namespace NewRpgInternal
