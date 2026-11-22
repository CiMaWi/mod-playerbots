/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVELLEGRULES_H
#define PLAYERBOTS_TRAVELLEGRULES_H

// RPG_TRAVEL's leg sequencing (WT8), Player-free: what the route follower does next, from plain observations of the
// bot. NewRpgTravelAction feeds it on the bot's map thread and carries the decisions out;
// tests/travel-legs-selftest.cpp drives it with scripted observations. A leg ends by a position or map predicate, never
// by time: time only fails it. A failed leg asks for a new route (at most MAX_REPLANS per trip), never a teleport.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "FailedAttemptRules.h"
#include "MountRules.h"
#include "RouteRules.h"
#include "TravelLegTypes.h"
#include "TravelLifecycle.h"
#include "TravelWalkRules.h"

namespace TravelLegs
{
using TravelRoute::Distance;

static_assert(TravelGraph::NO_NODE == NO_NODE, "TravelLegs and TravelWalk share the graph's NO_NODE");
constexpr float SAME_POINT_YARDS = 0.5f;       // a leg end this close to a graph node is that node
constexpr float WAYPOINT_YARDS = 5.0f;         // a walk waypoint is reached this close
constexpr float GOAL_YARDS = 10.0f;            // the route's last waypoint
constexpr float NOOP_SECONDS = 1.0f;           // a walk leg this short with ends within WAYPOINT_YARDS is no step
constexpr float PORTAL_EXIT_YARDS = 50.0f;     // a portal within one map has put the bot this close to its exit
constexpr float FLIGHT_MASTER_YARDS = 15.0f;   // the flight leg's node stands at the master: talk from here
constexpr float LANDING_YARDS = 30.0f;         // a flight has landed at its leg's end
constexpr float WALK_FLOOR_SPEED = 7.0f;       // yd/s: a walk leg's time is at least its straight line at run speed
constexpr float WALK_TIMEOUT_FACTOR = 3.0f;    // walk: estimate x 3 + slack
constexpr float FLIGHT_TIMEOUT_FACTOR = 2.0f;  // flight: path seconds x 2 + slack
constexpr std::uint32_t LEG_SLACK_MS = 60 * 1000;
constexpr std::uint32_t PORTAL_TIMEOUT_MS = 20 * 1000;
constexpr std::uint32_t TRIP_SLACK_MS = 10 * 60 * 1000;  // the current route's estimate x 3 + this
constexpr std::uint8_t MAX_REPLANS = 3;       // new routes per trip after failed legs; then the trip is given up
constexpr std::uint8_t MAX_SPOT_FAILS = 4;    // takeoffs refused where the bot stands that replan outside MAX_REPLANS
constexpr float HEARTH_ARRIVE_YARDS = 40.0f;  // a hearth leg is done within this of the bind point
constexpr std::uint32_t HEARTH_SLACK_MS = 30 * 1000;  // a hearth leg's time beyond its cooldown wait and cast
constexpr std::uint32_t REPLAN_DELAY_MS = 2000;       // the failed leg's stuck report reaches the published book first
constexpr std::uint32_t LEGS_PER_TICK = 4;            // finished legs passed in one tick at most
constexpr std::uint32_t WAYPOINTS_PER_TICK = 4;       // reached waypoints passed in one tick at most
constexpr std::uint8_t MAX_RESUBMITS = 3;             // route requests asked again per trip after a late or lost answer
constexpr std::uint8_t LEG_DEATHS = 2;                // deaths on one leg fail it (FailReason::Death)
constexpr std::uint8_t TRIP_DEATHS = 3;               // deaths on one trip end it
// Fly legs (own flying mount, WU10)
constexpr float FLY_WAYPOINT_YARDS = 25.0f;    // a flight waypoint is reached this close (3D)
constexpr float FLY_CLIMB_YARDS = 30.0f;       // the climb after the takeoff and the descent before the landing
constexpr float LANDING_CHECK_YARDS = 150.0f;  // the landing spot is checked from this close (2D; NeedsLandingCheck)
constexpr float LANDING_SEARCH_YARDS = 60.0f;  // an indoor landing moves at most this far back along the flight
constexpr float LANDING_STEP_YARDS = 4.0f;     // the outdoor search's step
constexpr std::uint8_t FLY_MOUNT_TRIES = 3;    // real mount tries (NoteMountTry) before the takeoff fails
constexpr std::uint32_t FLY_MOUNT_RETRY_MS = 5 * 1000;  // a mount cast is tried again after this
constexpr float FLY_MIN_CLEARANCE_YARDS = 15.0f;        // the real-height check: at least this above the ground ahead
constexpr float FLY_RAISE_CAP_YARDS = 250.0f;           // a raise lifts a waypoint at most this far above the plan
constexpr float FLY_NO_GROUND = -50000.0f;              // a height sample at or below: no ground found there
constexpr float AIRBORNE_YARDS = 5.0f;                  // a bot on its flyer this far above the floor flies

// The flight down after a fly leg failed in the air takes at most this (StepDescent)
constexpr std::uint32_t DESCENT_TIMEOUT_MS = 90 * 1000;

// A bot on a flying mount at z over the floor floorZ (the map's height below it) is in the air (WU11b fix 8). A bot
// sitting on its flyer on the ground keeps the flying flag (bots send no landing packet, and the stale-flag cleanup
// waits for the mount's aura to go), and the core refuses a mounted player's attack: the flag alone kept such a bot
// mounted in a fight until it died (Seyo at Agmar's Hammer). The height decides for every flyer, flag or not (WU11c: a
// flight aura applied in the air sets CAN_FLY only, Meldel). No floor found: in the air (a fall kills).
inline bool InTheAir(float z, float floorZ) { return floorZ <= FLY_NO_GROUND || z - floorZ > AIRBORNE_YARDS; }

// The height a bot killed at z leaves its body at (WU11b fix 10), groundZ the map's ground below it and waterZ the
// surface of the water over that ground (FLY_NO_GROUND: none). A player's body falls to the floor (its client falls
// it); a bot's stayed where it died, and its revival at the corpse dropped it to its death (Daenlean, killed 117 yd
// up). The floor is the higher of ground and water surface: a body over the sea floats (fix 10 moved Almaadon 569 yd
// down to the seabed), one that died swimming stays. More than AIRBORNE_YARDS above a found floor: the floor; else
// where it is.
inline float BodyZ(float z, float groundZ, float waterZ)
{
    float const floorZ = std::max(groundZ, waterZ);
    return floorZ > FLY_NO_GROUND && z - floorZ > AIRBORNE_YARDS ? floorZ : z;
}

// Transport legs (WT19; stored crossings WT70c): ships, zeppelins, elevators and the Deeprun Tram
// The follower looks for the leg's transport this often, and a bot on a transport leg ticks at this pace
// (BotTravel::PaceMs). A lift stands 1.3-5 s: at 1000 ms the first sight's latency and the deck walk used up a stand
// before the row's time plus CROSS_SLACK_MS (WT70d-b, H6 refuted at 1000 ms)
constexpr std::uint32_t TRANSPORT_SCAN_MS = 250;
// ... but only around a docking (ScanPaceMs, WT70e, PLAN 3.21 (13)); away from one at this pace, the one before WT70d
constexpr std::uint32_t TRANSPORT_SLOW_SCAN_MS = 1000;
// PlayerbotAI adds a passenger once a second: a departure without the bot counts as a miss after this
constexpr std::uint32_t MISS_GRACE_MS = 2500;
constexpr std::uint32_t WAIT_PERIODS = 5;  // periods waited off the transport at most
constexpr std::uint32_t RIDE_PERIODS = 2;  // periods aboard at most (a missed exit rides round)
// A transport leg's seconds are half a period's wait plus the ride: about a period
constexpr float PERIOD_FACTOR = 2.0f;
constexpr std::uint32_t MIN_PERIOD_MS = 30 * 1000;  // the period assumed before a scan read it is at least this
// Stored crossings (WT70c, WT.53): plain tuning values (WT.40)
constexpr float DOCK_REACH_YARDS = 1.0f;        // the bot stands on a row's dock point within this
constexpr float DECK_REACH_YARDS = 0.5f;        // and, aboard, on its deck point (by its own offset on the vessel)
constexpr float PLACE_RADIUS_YARDS = 6.0f;      // the place aboard is drawn this near the exit's deck point
constexpr float OFF_DECK_YARDS = 0.5f;          // a deck move that ended further off the vessel's mesh: off_deck
constexpr std::uint32_t CROSS_SLACK_MS = 1000;  // a crossing starts only with its own time plus this left at the stop
constexpr std::uint32_t CROSS_MAX_MS = 5000;    // a crossing without an outcome after this is cancelled
// Aboard, the rider walks to its leaving row's deck point from this long before the vessel docks at that stop (by the
// stop's window), so the stand holds only the crossing (WT70d-b: a lift that stands 1.3 s)
constexpr std::uint32_t DECK_LEAD_MS = 5000;

// A deck move's path is taken only when it is normal and ends within the reach of the point asked for: the vessel's
// path finder answers normal for the nearest point it reaches, which may lie short (WT.55). Any other path is refused
inline bool DeckPathTaken(bool normal, float endYards) { return normal && endYards <= DECK_REACH_YARDS; }

// The rider's contact (WT70e-a): where the ray under its feet finds no deck, the vessel's mesh decides (mesh first,
// WT.51). A rider within OFF_DECK_YARDS of the mesh stands on the deck. meshYards < 0: no mesh or no answer
constexpr TravelLifecycle::Contact DeckContact(TravelLifecycle::Contact ray, float meshYards)
{
    return ray == TravelLifecycle::Contact::OffSurface && meshYards >= 0.0f && meshYards <= OFF_DECK_YARDS
               ? TravelLifecycle::Contact::OnMesh
               : ray;
}

// The follower's act for the logs (the case's hold line, the selftest's traces)
inline char const* ActWord(Act act)
{
    switch (act)
    {
        case Act::Walk:
            return "walk";
        case Act::UseTrigger:
            return "trigger";
        case Act::UseObject:
            return "use";
        case Act::TalkFlightMaster:
            return "talk";
        case Act::UseHearth:
            return "hearth";
        case Act::Ride:
            return "ride";
        case Act::Mount:
            return "mount";
        case Act::Fly:
            return "fly";
        case Act::Board:
            return "board";
        case Act::Disembark:
            return "disembark";
        case Act::Stand:
            return "stand";
        case Act::Deck:
            return "deck";
        case Act::Descend:
            return "descend";
        case Act::Fall:
            return "fall";
        case Act::Wait:
            return "wait";
        case Act::Done:
            return "done";
        case Act::Arrived:
            return "arrived";
        case Act::Fail:
            return "fail";
    }
    return "?";
}

inline char const* FailWord(FailReason reason)
{
    switch (reason)
    {
        case FailReason::None:
            return "none";
        case FailReason::Timeout:
            return "timeout";
        case FailReason::WrongMap:
            return "wrong_map";
        case FailReason::Stuck:
            return "stuck";
        case FailReason::Portal:
            return "portal";
        case FailReason::Fare:
            return "fare";
        case FailReason::Landing:
            return "landing";
        case FailReason::Unsupported:
            return "unsupported";
        case FailReason::NoAnswer:
            return "no_answer";
        case FailReason::NoRoute:
            return "no_route";
        case FailReason::Taxi:
            return "taxi";
        case FailReason::Hearth:
            return "hearth";
        case FailReason::OffMesh:
            return "off_mesh";
        case FailReason::Death:
            return "death";
        case FailReason::TransportLost:
            return "transport_lost";
        case FailReason::NoExit:
            return "no_exit";
        case FailReason::Takeoff:
            return "takeoff";
        case FailReason::NoPath:
            return "no_path";
        case FailReason::NoWalk:
            return "no_walk";
        case FailReason::NoCrossing:
            return "no_crossing";
        case FailReason::CrossingFailed:
            return "crossing_failed";
    }
    return "?";
}

// Travel event sources (EventLog::Emit(bot, EventType::Travel, source, ...); literals, as the log keeps the
// pointer's text): "start", "arrived", "leg_done:<leg type>", "leg_failed:<reason>", "abort:<reason>" and the
// aborts that are no failed leg: "abort:deadline", "abort:stalled", "abort:no_service", "abort:stopped",
// "abort:sick"
inline char const* LegDoneSource(LegType type)
{
    switch (type)
    {
        case LegType::Walk:
            return "leg_done:walk";
        case LegType::Portal:
            return "leg_done:portal";
        case LegType::Flight:
            return "leg_done:flight";
        case LegType::Transport:
            return "leg_done:transport";
        case LegType::Hearth:
            return "leg_done:hearth";
        case LegType::Fly:
            return "leg_done:fly";
        case LegType::StaticPortal:
            return "leg_done:static_portal";
    }
    return "leg_done:?";
}

inline char const* LegFailedSource(FailReason reason)
{
    switch (reason)
    {
        case FailReason::None:
            return "leg_failed:none";
        case FailReason::Timeout:
            return "leg_failed:timeout";
        case FailReason::WrongMap:
            return "leg_failed:wrong_map";
        case FailReason::Stuck:
            return "leg_failed:stuck";
        case FailReason::Portal:
            return "leg_failed:portal";
        case FailReason::Fare:
            return "leg_failed:fare";
        case FailReason::Landing:
            return "leg_failed:landing";
        case FailReason::Unsupported:
            return "leg_failed:unsupported";
        case FailReason::NoAnswer:
            return "leg_failed:no_answer";
        case FailReason::NoRoute:
            return "leg_failed:no_route";
        case FailReason::Taxi:
            return "leg_failed:taxi";
        case FailReason::Hearth:
            return "leg_failed:hearth";
        case FailReason::OffMesh:
            return "leg_failed:off_mesh";
        case FailReason::Death:
            return "leg_failed:death";
        case FailReason::TransportLost:
            return "leg_failed:transport_lost";
        case FailReason::NoExit:
            return "leg_failed:no_exit";
        case FailReason::Takeoff:
            return "leg_failed:takeoff";
        case FailReason::NoPath:
            return "leg_failed:no_path";
        case FailReason::NoWalk:
            return "leg_failed:no_walk";
        case FailReason::NoCrossing:
            return "leg_failed:no_crossing";
        case FailReason::CrossingFailed:
            return "leg_failed:crossing_failed";
    }
    return "leg_failed:?";
}

inline char const* AbortSource(FailReason reason)
{
    switch (reason)
    {
        case FailReason::None:
            return "abort:none";
        case FailReason::Timeout:
            return "abort:timeout";
        case FailReason::WrongMap:
            return "abort:wrong_map";
        case FailReason::Stuck:
            return "abort:stuck";
        case FailReason::Portal:
            return "abort:portal";
        case FailReason::Fare:
            return "abort:fare";
        case FailReason::Landing:
            return "abort:landing";
        case FailReason::Unsupported:
            return "abort:unsupported";
        case FailReason::NoAnswer:
            return "abort:no_answer";
        case FailReason::NoRoute:
            return "abort:no_route";
        case FailReason::Taxi:
            return "abort:taxi";
        case FailReason::Hearth:
            return "abort:hearth";
        case FailReason::OffMesh:
            return "abort:off_mesh";
        case FailReason::Death:
            return "abort:death";
        case FailReason::TransportLost:
            return "abort:transport_lost";
        case FailReason::NoExit:
            return "abort:no_exit";
        case FailReason::Takeoff:
            return "abort:takeoff";
        case FailReason::NoPath:
            return "abort:no_path";
        case FailReason::NoWalk:
            return "abort:no_walk";
        case FailReason::NoCrossing:
            return "abort:no_crossing";
        case FailReason::CrossingFailed:
            return "abort:crossing_failed";
    }
    return "abort:?";
}

// The outcome of a trip a failed leg ends (RpgStatus::End): a timed-out leg TimedOut, a stuck walk or a bot off the
// mesh Stuck, every other failure Abandoned
constexpr StatusEnd::Outcome FailOutcome(FailReason reason)
{
    switch (reason)
    {
        case FailReason::Timeout:
            return StatusEnd::Outcome::TimedOut;
        case FailReason::Stuck:
        case FailReason::OffMesh:
            return StatusEnd::Outcome::Stuck;
        default:
            return StatusEnd::Outcome::Abandoned;
    }
}

inline char const* TransportSource(TransportEvent event)
{
    switch (event)
    {
        case TransportEvent::Attempt:
            return "attempt";
        case TransportEvent::Boarded:
            return "boarded";
        case TransportEvent::Arrived:
            return "arrived";
        case TransportEvent::Missed:
            return "missed";
        case TransportEvent::Leave:
            return "leave";
        case TransportEvent::StuckAboard:
            return "stuck_aboard";
        case TransportEvent::NoExit:
            return "no_exit";
        case TransportEvent::OffFirst:
            return "off_first";
        case TransportEvent::Lost:
            return "lost";
        case TransportEvent::Back:
            return "back";
        case TransportEvent::Cross:
            return "cross";
        case TransportEvent::Crossed:
            return "crossed";
        case TransportEvent::CrossFailed:
            return "cross_failed";
        case TransportEvent::CrossCut:
            return "cross_cut";
        case TransportEvent::NoCrossing:
            return "no_crossing";
        case TransportEvent::Place:
            return "place";
        case TransportEvent::OffDeck:
            return "off_deck";
        case TransportEvent::Fight:
            return "fight";
        case TransportEvent::FollowTimeout:
            return "follow_timeout";
    }
    return "?";
}

// What the follower sees of the bot this tick
struct Observation
{
    Point bot;
    bool inFlight{false};      // on a taxi
    bool flyingMount{false};   // on a mount that flies here (a fly leg's takeoff)
    bool flying{false};        // in the air on it (MOVEMENTFLAG_FLYING); no taxi
    bool takeoffHere{true};    // a fly leg: the bot may mount a flyer and lift off where it stands (outdoors, a flying
                               // area by its real area, not in water, no battle)
    std::uint32_t money{0};    // copper
    std::uint32_t nowMs{0};    // getMSTime()
    std::uint32_t aboard{0};   // the gameobject entry of the transport the bot is a passenger of; 0 none
    TransportSight transport;  // transport legs only
    std::uint64_t aboardGuid{0};
    bool membershipAvailable{true};
    bool boardMotion{false};  // the bot's own boarding move onto the transport it is aboard has not reported yet
    std::optional<TravelLifecycle::Outcome> passengerOutcome;
    std::uint32_t legMs{0};  // the leg clock (TravelProgress::State::LegMs): the time the bot could act on the leg
    // WT70c: the leg's two stop records (Leg::crossFrom, crossTo in the graph's crossing index; null without one) and,
    // aboard, the bot's own offset in the vessel's frame (the deck walk's reach test)
    TravelCrossing::Stop const* boardAt{nullptr};
    TravelCrossing::Stop const* leaveAt{nullptr};
    TravelLifecycle::Point offset;
};

struct Decision
{
    Act act{Act::Wait};
    // Walk, Fly: where to; UseTrigger, TalkFlightMaster, Mount: the leg's start; Ride: the leg's end. A transport
    // leg's acts name no deck point: the follower reads the row from the leg's stop record (RowNow). Its Walk goes to
    // the row's dock point; Board, Deck and Disembark carry that dock point and Stand the leg's end, for the hold line
    Point target;
    FailReason reason{FailReason::None};
};

enum class Verdict : std::uint8_t
{
    Apply,   // the answer to the latest request, still on its map and young
    Drop,    // an answer to an older request: keep waiting
    Refresh  // the bot changed maps or the answer is too old: the route is asked again (Resubmit)
};

inline Verdict Judge(RouteWait const& wait, std::uint32_t answerId, std::uint32_t botMapId, std::uint32_t nowMs)
{
    if (answerId != wait.requestId)
        return Verdict::Drop;
    if (botMapId != wait.mapId || nowMs - wait.requestedMs > TravelRoute::ANSWER_KEEP_MS)
        return Verdict::Refresh;
    return Verdict::Apply;
}

inline Verdict Judge(RouteWait const& wait, std::uint32_t answerId, Point const& bot, std::uint32_t nowMs)
{
    Verdict const verdict = Judge(wait, answerId, bot.mapId, nowMs);
    if (verdict == Verdict::Apply && wait.start && Distance(*wait.start, bot) > GOAL_YARDS)
        return Verdict::Refresh;
    return verdict;
}

// Why the route is asked again (WT64f; a leg that fails NoAnswer writes it in its trip row no_answer, world-log.md):
// no answer before the wait ran out (Late), only an answer to an older request (Stale), or the latest request's
// answer refused by Judge: the bot changed maps (OtherMap), the answer is older than ANSWER_KEEP_MS (Old) or the bot
// stands more than GOAL_YARDS from where it asked (Moved). answerId: the answer taken, none without one
enum class AskWhy : std::uint8_t
{
    Late,
    Stale,
    OtherMap,
    Old,
    Moved
};

inline AskWhy WhyAskAgain(RouteWait const& wait, std::optional<std::uint32_t> answerId, Point const& bot,
                          std::uint32_t nowMs)
{
    if (!answerId)
        return AskWhy::Late;
    if (*answerId != wait.requestId)
        return AskWhy::Stale;
    if (bot.mapId != wait.mapId)
        return AskWhy::OtherMap;
    if (nowMs - wait.requestedMs > TravelRoute::ANSWER_KEEP_MS)
        return AskWhy::Old;
    return AskWhy::Moved;
}

inline char const* AskWhyWord(AskWhy why)
{
    switch (why)
    {
        case AskWhy::Late:
            return "late";
        case AskWhy::Stale:
            return "stale";
        case AskWhy::OtherMap:
            return "other_map";
        case AskWhy::Old:
            return "old";
        case AskWhy::Moved:
            return "moved";
    }
    return "late";
}

// A late or lost answer is no failed leg: the route is asked again from where the bot stands, at most MAX_RESUBMITS
// times per trip (the trip's deadline bounds them too); false: the leg fails as NoAnswer
inline bool Resubmit(Trip& trip)
{
    if (trip.resubmits >= MAX_RESUBMITS)
        return false;
    ++trip.resubmits;
    return true;
}

// Routes begin and end with walks of 0 seconds (start or goal on a node); a 0-second walk whose ends lie apart is
// walked, not skipped
inline bool IsNoOp(Leg const& leg)
{
    return leg.type == LegType::Walk && leg.seconds < NOOP_SECONDS && leg.from.mapId == leg.to.mapId &&
           Distance(leg.from, leg.to) <= WAYPOINT_YARDS;
}

// An elevator's period, which the graph does not know (a vessel's comes with the leg, from the core): twice the leg's
// seconds, MIN_PERIOD_MS at least
inline std::uint32_t LiftPeriodMs(Leg const& leg)
{
    return std::max(static_cast<std::uint32_t>(leg.seconds * PERIOD_FACTOR * 1000.0f), MIN_PERIOD_MS);
}

inline std::uint32_t LegTimeoutMs(Leg const& leg)
{
    switch (leg.type)
    {
        case LegType::Walk:
        {
            float seconds = leg.seconds;
            if (leg.from.mapId == leg.to.mapId)
                seconds = std::max(seconds, Distance(leg.from, leg.to) / WALK_FLOOR_SPEED);
            return static_cast<std::uint32_t>(seconds * WALK_TIMEOUT_FACTOR * 1000.0f) + LEG_SLACK_MS;
        }
        case LegType::Portal:
        case LegType::StaticPortal:
            return PORTAL_TIMEOUT_MS;
        case LegType::Flight:
        case LegType::Fly:
            return static_cast<std::uint32_t>(leg.seconds * FLIGHT_TIMEOUT_FACTOR * 1000.0f) + LEG_SLACK_MS;
        case LegType::Hearth:
            // The route's seconds hold the cooldown wait and the cast
            return static_cast<std::uint32_t>(leg.seconds * 1000.0f) + HEARTH_SLACK_MS;
        default:  // a transport leg runs on its vessel's bounds (NextDeadlineMs)
            return LEG_SLACK_MS;
    }
}

// A fly leg's points (WU9 review NOTE 3): the takeoff, a climb point FLY_CLIMB_YARDS on at the first flight point's
// height, the planned flight points, a descent point FLY_CLIMB_YARDS before the landing at the last one's height, and
// the landing. The plan's takeoff and landing ramps are straight lines over the first and last gap that no terrain
// check saw: the bot climbs and descends steeply near the ends instead
inline std::vector<Waypoint> FlyWaypoints(Leg const& leg)
{
    std::vector<Point> const& planned = leg.flightPoints;
    if (planned.size() < 3)
        return {{leg.from, NO_NODE}, {leg.to, NO_NODE}};
    auto const toward = [](Point const& from, Point const& to, float z)
    {
        float const gap = std::hypot(to.x - from.x, to.y - from.y);
        float const share = gap > 0.0f ? std::min(FLY_CLIMB_YARDS, gap / 2.0f) / gap : 0.0f;
        return Point{from.mapId, from.x + (to.x - from.x) * share, from.y + (to.y - from.y) * share, z};
    };
    Point const& takeoff = planned.front();
    Point const& landing = planned.back();
    std::vector<Waypoint> points;
    points.reserve(planned.size() + 2);
    points.push_back({takeoff, NO_NODE});
    points.push_back({toward(takeoff, planned[1], planned[1].z), NO_NODE});
    for (std::size_t k = 1; k + 1 < planned.size(); ++k)
        points.push_back({planned[k], NO_NODE});
    points.push_back({toward(landing, planned[planned.size() - 2], planned[planned.size() - 2].z), NO_NODE});
    points.push_back({landing, NO_NODE});
    return points;
}

// The real-height check's rule (the follower samples the ground ahead on the map thread, buildings included, once per
// reached waypoint): a target below the highest sample + FLY_MIN_CLEARANCE_YARDS climbs to that sample + the bot's
// clearance, at most FLY_RAISE_CAP_YARDS above the plan; samples at or below FLY_NO_GROUND are no ground
inline float RequiredZ(std::vector<float> const& samples, float plannedZ, float clearance)
{
    float highest = FLY_NO_GROUND;
    for (float const sample : samples)
        if (std::isfinite(sample) && sample > highest)
            highest = sample;
    if (highest <= FLY_NO_GROUND || plannedZ >= highest + FLY_MIN_CLEARANCE_YARDS)
        return plannedZ;
    return std::min(highest + std::max(clearance, FLY_MIN_CLEARANCE_YARDS), plannedZ + FLY_RAISE_CAP_YARDS);
}

// The points a leg passes: its start, then for a walk every graph node in order, then its end. Points within
// SAME_POINT_YARDS of the previous one merge (a leg starting or ending on a node). A fly leg's: FlyWaypoints.
// nodeAt(index) -> Point
template <class NodeAt>
std::vector<Waypoint> Waypoints(Leg const& leg, NodeAt const& nodeAt)
{
    if (leg.type == LegType::Fly)
        return FlyWaypoints(leg);
    std::vector<Waypoint> points{{leg.from, NO_NODE}};
    auto const add = [&points](Point const& at, std::uint32_t node)
    {
        Waypoint& last = points.back();
        if (at.mapId == last.at.mapId && Distance(at, last.at) <= SAME_POINT_YARDS)
        {
            if (node != NO_NODE)
                last.node = node;
            return;
        }
        points.push_back({at, node});
    };
    if (leg.type == LegType::Walk)
    {
        for (std::uint32_t const node : leg.nodes)
            add(nodeAt(node), node);
    }
    else if (!leg.nodes.empty())
        add(nodeAt(leg.nodes.front()), leg.nodes.front());
    add(leg.to, leg.type != LegType::Walk && !leg.nodes.empty() ? leg.nodes.back() : NO_NODE);
    return points;
}

// A new leg's record: a combat stretch running at the leg change counts for the new leg from now
inline LegRecord NextRecord(LegRecord const& before, std::uint32_t nowMs)
{
    LegRecord next;
    next.combatSinceMs = before.combatSinceMs ? std::max<std::uint32_t>(nowMs, 1) : 0;
    return next;
}

// The bot's engine as PlayerbotAI changes it (out of combat, in combat, dead)
enum class Life : std::uint8_t
{
    Calm,
    Combat,
    Dead
};

// A travelling bot's engine change or death (map thread): closes a running combat stretch, opens one in combat, and
// counts a death for the trip and, while a leg runs, for the leg
inline void NoteLife(Trip& trip, Life life, std::uint32_t nowMs)
{
    LegRecord& record = trip.record;
    if (record.combatSinceMs)
    {
        record.combatMs += nowMs - record.combatSinceMs;
        record.combatSinceMs = 0;
    }
    if (life == Life::Combat)
    {
        record.combatSinceMs = std::max<std::uint32_t>(nowMs, 1);
        if (trip.seenInFlight && trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == LegType::Fly)
            trip.foughtInFlight = true;
    }
    else if (life == Life::Dead)
    {
        trip.deaths = static_cast<std::uint8_t>(std::min(trip.deaths + 1, 255));
        if (trip.legIdx < trip.legs.size())
            record.deaths = static_cast<std::uint8_t>(std::min(record.deaths + 1, 255));
    }
}

// A follower tick on the current leg
inline void NoteTick(Trip& trip, bool mounted)
{
    ++trip.record.ticks;
    if (mounted)
        ++trip.record.mountedTicks;
}

// A fly leg's mount try that really happened (the glue skips tries in combat or while casting: those do not count)
inline void NoteMountTry(Trip& trip, std::uint32_t nowMs)
{
    if (trip.mountTries < 255)
        ++trip.mountTries;
    trip.mountTriedMs = nowMs;
}

// A fly leg's try at the mount owner's verdict (MountToFly, WT44): MountRules::FlyTryOf, then the count. Refused and
// Cast are real tries (NoteMountTry); Refused leaves the verdict's word for the fly_leg_failed row (nomount, on_foot)
inline MountRules::FlyTry TakeFlyTry(Trip& trip, MountRules::Verdict const& verdict, bool settled, std::uint32_t nowMs)
{
    MountRules::FlyTry const step = MountRules::FlyTryOf(verdict, settled);
    if (step == MountRules::FlyTry::Refused || step == MountRules::FlyTry::Cast)
        NoteMountTry(trip, nowMs);
    if (step == MountRules::FlyTry::Refused)
        trip.mountWhy = verdict.why;
    return step;
}

// The leg's time in combat until now (a running stretch included)
inline std::uint32_t CombatMs(LegRecord const& record, std::uint32_t nowMs)
{
    return record.combatMs + (record.combatSinceMs ? nowMs - record.combatSinceMs : 0);
}

inline float CombatSeconds(LegRecord const& record, std::uint32_t nowMs)
{
    return static_cast<float>(CombatMs(record, nowMs)) / 1000.0f;
}

// How the bot went on the leg, from its follower ticks: foot (never mounted), mounted (always), part
inline char const* MountWord(LegRecord const& record)
{
    if (!record.mountedTicks)
        return "foot";
    return record.mountedTicks >= record.ticks ? "mounted" : "part";
}

// The FlightFall row's word for a fly leg that came down away from its end (FailReason::Landing), by the bot's height
// above the ground then: combat when it fought after the takeoff and is on the ground (WU11b fix 9: Olina, attacked
// on the descent, dismounted to fight), else fall (fallen or dismounted in the air; Daenlean, killed in the air and
// revived 117 yd up, fought too)
inline char const* LandingWord(Trip const& trip, float heightYards)
{
    return trip.foughtInFlight && heightYards <= AIRBORNE_YARDS ? "combat" : "fall";
}

// The walk_start row's movement word (PA17a): how the bot moves where a walk's start was refused, in this order: in the
// air under its own wings or a flying mount (flying), falling, swimming, on a transport, on a ground mount (mounted),
// else on foot (ground)
inline char const* WalkStartMovement(bool flying, bool falling, bool swimming, bool onTransport, bool mounted)
{
    if (flying)
        return "flying";
    if (falling)
        return "falling";
    if (swimming)
        return "swimming";
    if (onTransport)
        return "transport";
    return mounted ? "mounted" : "ground";
}

// Killed too often: LEG_DEATHS on the leg or TRIP_DEATHS on the trip fail the leg as FailReason::Death
inline bool DiedTooOften(Trip const& trip) { return trip.record.deaths >= LEG_DEATHS || trip.deaths >= TRIP_DEATHS; }

// Starts leg index (no-op walks skipped); index past the last leg: the trip has arrived
template <class NodeAt>
void StartLeg(Trip& trip, std::uint32_t index, std::uint32_t nowMs, NodeAt const& nodeAt)
{
    while (index < trip.legs.size() && IsNoOp(trip.legs[index]))
        ++index;
    trip.legIdx = index;
    trip.legStartMs = nowMs;
    trip.next = 1;
    trip.walk = {};
    trip.piece = {};
    trip.approached = false;
    trip.triggered = false;
    trip.taxiActivated = false;
    trip.seenInFlight = false;
    trip.foughtInFlight = false;
    trip.heightChecked = 0;
    trip.landingChecked = false;
    trip.climbs = 0;
    trip.peakZ = 0.0f;
    trip.mountTries = 0;
    trip.mountTriedMs = 0;
    trip.mountWhy = "";
    trip.boarding = Boarding::Approach;
    trip.sight = TransportSight{};
    trip.sightMs = 0;
    trip.periodMs = 0;
    trip.stateMs = nowMs;
    trip.waitSinceMs = nowMs;
    trip.boardedMs = nowMs;
    trip.departed = false;
    trip.departedMs = 0;
    trip.misses = 0;
    trip.passengerGuid = 0;
    trip.row = 0;
    trip.leaveSpent = 0;
    trip.backSpent = 0;
    trip.placed = false;
    trip.posedMs = 0;
    trip.crossMs = 0;
    trip.crossAmount = 0;
    trip.cancelled = false;
    trip.attempted = false;
    trip.boardCombatMs = 0;
    trip.followersWoken = false;
    trip.record = NextRecord(trip.record, nowMs);
    trip.waypoints.clear();
    if (index < trip.legs.size())
        trip.waypoints = Waypoints(trip.legs[index], nodeAt);
}

// The vessel the bot means to board (WT62, WT.48): its transport leg's sighted vessel, past the approach and before
// the ride; 0 none. The passenger check probes a bot on no transport for this vessel only (BotTravel::PreGate)
inline std::uint64_t BoardingIntent(Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != LegType::Transport || trip.passengerGuid ||
        trip.boarding == Boarding::Approach)
        return 0;
    return trip.sight.guid;
}

// The vessel of the bot's boarding intent docked under it at the wait point and the passenger check attached it: that
// is the boarding, no crossing was needed
inline bool BoardedByIntent(Trip const& trip, std::uint64_t aboardGuid)
{
    return aboardGuid && aboardGuid == BoardingIntent(trip);
}

inline std::uint32_t TransportPeriodMs(Trip const& trip, Leg const& leg)
{
    if (trip.periodMs)
        return trip.periodMs;
    return leg.periodMs ? leg.periodMs : LiftPeriodMs(leg);
}

// Off the transport (approach, wait, board) the leg fails WAIT_PERIODS periods after its start
inline std::uint32_t TransportWaitMs(Trip const& trip, Leg const& leg)
{
    return LEG_SLACK_MS + WAIT_PERIODS * TransportPeriodMs(trip, leg);
}

// Aboard, RIDE_PERIODS periods after the boarding
inline std::uint32_t TransportRideMs(Trip const& trip, Leg const& leg)
{
    return LEG_SLACK_MS + RIDE_PERIODS * TransportPeriodMs(trip, leg);
}

constexpr std::int64_t NO_DEADLINE = std::numeric_limits<std::int64_t>::max();

// The trip's one expiry, read by the follower (StepLeg, StepOffFirst, TakeRoute) and the progress guard (Maintain): the
// ms left before the current state is overdue, negative once it is. A pending route answer, the get-off and a transport
// leg run on the wall clock (the service's keep, the vessel's timetable), every other leg on the leg clock legMs (the
// time the bot could act on it, TravelProgress::State::LegMs). route: the request the follower waits for, if any
inline std::int64_t NextDeadlineMs(Trip const& trip, std::uint32_t nowMs, std::uint32_t legMs,
                                   RouteWait const* route = nullptr)
{
    auto const left = [](std::uint32_t elapsedMs, std::uint32_t boundMs)
    { return static_cast<std::int64_t>(boundMs) - static_cast<std::int64_t>(elapsedMs); };
    if (route && route->waiting)
        return left(nowMs - route->requestedMs, TravelRoute::ANSWER_KEEP_MS);
    if (trip.offFirst.sinceMs)
        return left(nowMs - trip.offFirst.sinceMs, trip.offFirst.boundMs);
    if (trip.legIdx >= trip.legs.size())
        return NO_DEADLINE;
    Leg const& leg = trip.legs[trip.legIdx];
    if (leg.type == LegType::Transport)
        return trip.passengerGuid ? left(nowMs - trip.boardedMs, TransportRideMs(trip, leg))
                                  : left(nowMs - trip.legStartMs, TransportWaitMs(trip, leg));
    return left(legMs, LegTimeoutMs(leg));
}

// The stand test (WT70c, PLAN 3.21 (4)). A vessel stands at a stop when the scan saw it at the stop's stored pose
// (place and heading by the passenger check's own test, TravelLifecycle::SameDock: 0.2 yd, cos 0.99995; no tolerance of
// the leg's own), and
// - a ship or zeppelin is inside the stop's window of its period: the time left there decides whether a crossing may
//   still start;
// - a car without a window (one that waits for a state change) was at that pose on the scan before as well
//   (Trip::posedMs); a looping car has its window from the load, as a ship has.
inline TravelLifecycle::DockPose PoseOf(TravelCrossing::Stop const& stop) { return {{stop.x, stop.y, stop.z}, stop.o}; }

inline TravelLifecycle::DockPose PoseOf(TransportSight const& sight)
{
    return {{sight.at.x, sight.at.y, sight.at.z}, sight.o};
}

inline bool PosedAt(TransportSight const& sight, TravelCrossing::Stop const& stop)
{
    return sight.found && sight.at.mapId == stop.mapId && TravelLifecycle::SameDock(PoseOf(stop), PoseOf(sight));
}

// A scan (once a TRANSPORT_SCAN_MS; between scans the trip keeps the last one): the vessel's pose and period, when it
// was taken, and since which scan the vessel has kept the pose of one of the observation's stops (posedMs; 0: it is at
// neither)
inline void NoteScan(Trip& trip, Observation const& seen)
{
    TransportSight const& sight = seen.transport;
    bool const posed =
        (seen.boardAt && PosedAt(sight, *seen.boardAt)) || (seen.leaveAt && PosedAt(sight, *seen.leaveAt));
    bool const unmoved = trip.sight.found && trip.sight.guid == sight.guid && trip.sight.at.mapId == sight.at.mapId &&
                         TravelLifecycle::SameDock(PoseOf(trip.sight), PoseOf(sight));
    std::uint32_t const scanMs = std::max<std::uint32_t>(seen.nowMs, 1);
    if (!posed)
        trip.posedMs = 0;
    else if (!trip.posedMs || !unmoved)
        trip.posedMs = scanMs;
    if (sight.periodMs)
        trip.periodMs = sight.periodMs;
    trip.sight = sight;
    trip.sightMs = scanMs;
}

// The ms the vessel still stands at the stop, by the last scan and the time since: negative when it does not stand
// there (not at the pose; a ship at the pose outside the stop's window is under way; a car seen there by one scan
// only), NO_DEADLINE for a car that stands (it has no window). A window may wrap its period's end
// The ms of a stop's window left at progressMs of the vessel's period; negative outside the window
inline std::int64_t WindowLeftMs(TravelCrossing::Stop const& stop, std::uint32_t progressMs)
{
    std::uint32_t const period = stop.periodMs;
    std::uint32_t const arrive = stop.arriveMs % period;
    std::uint32_t const length = (stop.departMs % period + period - arrive) % period;
    std::uint32_t const in = (progressMs % period + period - arrive) % period;
    return in > length ? -1 : static_cast<std::int64_t>(length - in);
}

inline std::int64_t StandLeftMs(Trip const& trip, TravelCrossing::Stop const& stop, std::uint32_t nowMs)
{
    if (!PosedAt(trip.sight, stop))
        return -1;
    if (!stop.hasWindow || !stop.periodMs)
        return trip.posedMs && trip.sightMs != trip.posedMs ? NO_DEADLINE : -1;
    std::int64_t const left = WindowLeftMs(stop, trip.sight.progressMs);
    return left < 0 ? -1 : left - static_cast<std::int64_t>(nowMs - trip.sightMs);
}

// The last scan saw the vessel standing at the stop (whatever time has passed since)
inline bool Stood(Trip const& trip, TravelCrossing::Stop const& stop)
{
    return StandLeftMs(trip, stop, trip.sightMs) >= 0;
}

// Milliseconds until the vessel docks at the stop, by the stop's window and the vessel's place in its period now: 0
// while it stands there by its window, NO_DEADLINE when the docking cannot be foreseen (no window)
inline std::int64_t DocksInMs(TravelCrossing::Stop const& stop, std::uint32_t progressMs)
{
    if (!stop.hasWindow || !stop.periodMs)
        return NO_DEADLINE;
    std::uint32_t const period = stop.periodMs;
    std::uint32_t const progress = progressMs % period;
    if (WindowLeftMs(stop, progress) >= 0)
        return 0;
    return static_cast<std::int64_t>((stop.arriveMs % period + period - progress) % period);
}

// The same by the last scan's progress (NO_DEADLINE also when no scan saw the vessel)
inline std::int64_t DocksInMs(Trip const& trip, TravelCrossing::Stop const& stop, std::uint32_t nowMs)
{
    if (!stop.hasWindow || !stop.periodMs || !trip.sight.found)
        return NO_DEADLINE;
    std::uint32_t const period = stop.periodMs;
    return DocksInMs(stop, (trip.sight.progressMs % period + (nowMs - trip.sightMs) % period) % period);
}

// The bot's offset on the vessel (Observation::offset) from a row's deck point, yards
inline float OffsetYards(TravelLifecycle::Point const& offset, TravelCrossing::Spot const& deck)
{
    return std::sqrt(TravelLifecycle::DistanceSquared(offset, {deck.x, deck.y, deck.z}));
}

// The deck walk ahead of the docking (WT70d-b): from DECK_LEAD_MS before the vessel docks at the stop the rider leaves
// at (DocksInMs; 0 while it stands there) the rider walks to its leaving row's deck point (Walk) and then stays on it
// (OnPoint), so a short stand holds only the crossing; before that it rides (Ride). The leg's ride (StepTransport)
// and a follower riding with its master (FollowStep) share it
enum class DeckLead : std::uint8_t
{
    Ride,
    Walk,
    OnPoint
};

inline DeckLead DeckLeadOf(std::int64_t docksInMs, TravelLifecycle::Point const& offset, TravelCrossing::Row const& row)
{
    if (docksInMs > static_cast<std::int64_t>(DECK_LEAD_MS))
        return DeckLead::Ride;
    return OffsetYards(offset, row.deck) > DECK_REACH_YARDS ? DeckLead::Walk : DeckLead::OnPoint;
}

// A crossing starts only if the stop has the crossing's own time plus CROSS_SLACK_MS left
inline bool HasTimeFor(Trip const& trip, TravelCrossing::Stop const& stop, TravelCrossing::Move const& move,
                       std::uint32_t nowMs)
{
    std::int64_t const needMs = static_cast<std::int64_t>(std::ceil(move.seconds * 1000.0f)) + CROSS_SLACK_MS;
    return StandLeftMs(trip, stop, nowMs) >= needMs;
}

// A follower at its master's vessel (WT70d, PLAN 3.21 (8)): the master stands on a vessel and the bot does not (Board),
// or the other way (Leave). What the glue read this tick: the record of the stop the vessel stands at (StopStoodAt;
// null under way or at a place without a record) and its index, the vessel's progress in its period, the bot's world
// point and, aboard, its offset, and the passenger check's verdict on the issued crossing
struct FollowView
{
    std::uint32_t nowMs{0};
    TravelCrossing::Stop const* stop{nullptr};
    std::uint32_t stopIndex{TravelCrossing::NO_STOP};
    TravelCrossing::Direction direction{TravelCrossing::Direction::Board};
    std::uint32_t progressMs{0};
    TravelRoute::Point bot;
    TravelLifecycle::Point offset;
    std::optional<TravelLifecycle::Outcome> outcome;
    // The bot and its master ride the same vessel (no stop then, Board): the stop the master leaves it at (a bot
    // master's transport leg: its far stop record; null for a master without one), with progressMs the vessel's
    TravelCrossing::Stop const* ahead{nullptr};
    // The ride owed (WT70e, PLAN 3.21 (8)): the vessel the view is of (guid, entry; 0: none) and its period, whether
    // the bot and its master stand on it, a bot master's transport leg's stops (NO_STOP: none), whether the master is
    // off the vessel and nearer the bot's stop than its far stop, and the records of the owed ride's stops (the glue
    // resolves FollowCrossing::owed's indexes; null: none)
    std::uint64_t vessel{0};
    std::uint32_t entry{0};
    std::uint32_t periodMs{0};
    bool aboard{false};
    bool masterAboard{false};
    bool masterBack{false};
    std::uint32_t masterFrom{TravelCrossing::NO_STOP};
    std::uint32_t masterTo{TravelCrossing::NO_STOP};
    TravelCrossing::Stop const* owedStop{nullptr};
    TravelCrossing::Stop const* owedLeave{nullptr};
};

struct FollowDecision
{
    FollowAct act{FollowAct::None};
    TravelCrossing::Row const* row{nullptr};  // Walk, Deck, Wait, Cross: the row in use
    TravelCrossing::Move move;                // Cross: the move as the stand test timed it
};

// The follower's issued crossing ended without success, or its row was refused: the row is spent for this docking
inline void FollowSpend(FollowCrossing& follow, std::uint32_t entry, std::uint32_t amount,
                        std::vector<TransportNote>& notes)
{
    notes.push_back({TransportEvent::CrossFailed, entry, amount});
    ++follow.spent;
    follow.walkYards = 0.0f;
    follow.walkMark = 0;
}

// The row the follower uses at the stop now (null: all spent)
inline TravelCrossing::Row const* FollowRow(FollowCrossing const& follow, TravelCrossing::Stop const& stop,
                                            TravelCrossing::Direction direction)
{
    std::span<TravelCrossing::Row const> const rows = stop.Rows(direction);
    return follow.spent < rows.size() ? &rows[follow.spent] : nullptr;
}

// A follower riding with its master, who leaves at a stop with a window (FollowView::ahead): ahead of that docking it
// walks to the stop's first leaving row's deck point and stays on it, as the leg's rider does (DeckLeadOf), unless the
// walk was refused on this ride. Once the master is off, FollowStep crosses by that row at the stop (WT70d-b)
inline FollowDecision FollowAhead(FollowCrossing const& follow, FollowView const& view)
{
    FollowDecision decision;
    std::span<TravelCrossing::Row const> const rows =
        view.ahead ? view.ahead->Rows(TravelCrossing::Direction::Leave) : std::span<TravelCrossing::Row const>{};
    if (follow.aheadRefused || rows.empty())
        return decision;
    DeckLead const lead = DeckLeadOf(DocksInMs(*view.ahead, view.progressMs), view.offset, rows[0]);
    if (lead == DeckLead::Ride)
        return decision;
    decision.row = &rows[0];
    decision.act = lead == DeckLead::Walk ? FollowAct::Deck : FollowAct::Wait;
    return decision;
}

// The stop a follower owes a ride from (WT70e, PLAN 3.21 (8)): its master stands on a vessel with stop records that
// the bot is not on and that is under way, and the bot saw it stand at a stop to board it there (FollowStep's stop)
// or a bot master's leg names the stop it boarded at. NO_STOP: no debt
inline std::uint32_t OwedFrom(FollowCrossing const& follow, FollowView const& view)
{
    if (follow.owed.vessel || view.stop || view.aboard || !view.masterAboard || !view.vessel)
        return TravelCrossing::NO_STOP;
    if (follow.stop != TravelCrossing::NO_STOP && follow.direction == TravelCrossing::Direction::Board)
        return follow.stop;
    return view.masterFrom;
}

// Forget the docking's state, keep the owed ride
inline void FollowForget(FollowCrossing& follow)
{
    FollowCrossing::OwedRide const owed = follow.owed;
    follow = {};
    follow.owed = owed;
}

// A follower that owes a ride (FollowCrossing::owed). The debt ends with its master back at its stop or aboard with
// it, or at the wait clock's end (follow_timeout). Off the vessel the bot walks to the owed stop's boarding row's dock
// point and waits there; when the vessel stands at that stop, the stop's rows as ever (nullopt: FollowStep goes on),
// and the rows a docking spent stay spent for the debt (none left: it ends). Aboard it rides on until the stop its
// master left at (or, unnamed, the first other stop) and leaves there by its rows (nullopt). Wait: no follow move
inline std::optional<FollowDecision> FollowOwed(FollowCrossing& follow, FollowView const& view,
                                                std::vector<TransportNote>& notes)
{
    using TravelCrossing::Direction;
    FollowCrossing::OwedRide& owed = follow.owed;
    FollowDecision decision;
    if ((view.masterBack && !view.aboard) || (view.masterAboard && view.aboard))
    {
        owed = {};
        return std::nullopt;
    }
    if (view.nowMs - owed.sinceMs >= owed.boundMs)
    {
        notes.push_back({TransportEvent::FollowTimeout, owed.entry, (view.nowMs - owed.sinceMs) / 1000});
        owed = {};
        FollowForget(follow);
        return decision;
    }
    decision.act = FollowAct::Wait;
    if (view.aboard)
    {
        bool const leavesHere = view.stop && view.direction == Direction::Leave && view.stopIndex != owed.from &&
                                (owed.to == TravelCrossing::NO_STOP || view.stopIndex == owed.to);
        if (leavesHere)
            return std::nullopt;
        FollowForget(follow);
        return decision;
    }
    if (view.stop && view.stopIndex == owed.from && view.direction == Direction::Board)
        return std::nullopt;
    follow.stop = owed.from;
    follow.direction = Direction::Board;
    follow.posedMs = 0;
    if (!view.owedStop)
        return decision;
    TravelCrossing::Row const* const row = FollowRow(follow, *view.owedStop, Direction::Board);
    if (!row)
    {
        owed = {};
        decision.act = FollowAct::None;
        return decision;
    }
    decision.row = row;
    TravelRoute::Point const dock{view.owedStop->mapId, row->dock.x, row->dock.y, row->dock.z};
    if (view.bot.mapId != dock.mapId || TravelRoute::Distance(view.bot, dock) > DOCK_REACH_YARDS)
        decision.act = FollowAct::Walk;
    return decision;
}

// One step of a follower. The issued crossing first: its outcome (crossed; cancelled or no outcome after CROSS_MAX_MS:
// cross_failed and the row spent while the vessel still stands at that stop, else cross_cut). An owed ride next
// (FollowOwed); a vessel its master rides that left without the bot starts one (OwedFrom). Then it acts only while
// the vessel stands at a stop with an unspent row of the direction: walk to the row's first point (the dock point
// ashore, the deck point aboard; DOCK_REACH_YARDS, DECK_REACH_YARDS), wait there until the vessel stands for the row as
// the leg's stand test says (the window's time left, or a car without one seen at the pose a scan's time), cross. The
// vessel gone or the rows spent with no ride owed, it does nothing and forgets the stop (a new docking has every row
// again). Riding with its master, the walk ahead of the master's docking (FollowAhead)
inline FollowDecision FollowStep(FollowCrossing& follow, FollowView const& view, std::vector<TransportNote>& notes)
{
    using TravelCrossing::Direction;
    using TravelLifecycle::Outcome;
    FollowDecision decision;
    if (follow.crossMs)
    {
        if (view.outcome == Outcome::Boarded || view.outcome == Outcome::Exited)
        {
            notes.push_back({TransportEvent::Crossed, follow.crossEntry, follow.crossAmount});
            bool const owedBoard = follow.owed.vessel && view.outcome == Outcome::Boarded;
            FollowForget(follow);
            if (owedBoard)
            {
                // Aboard the owed ride: the wait clock bounds the ride from here
                follow.owed.aboard = true;
                follow.owed.sinceMs = std::max<std::uint32_t>(view.nowMs, 1);
                decision.act = FollowAct::Wait;
            }
            else
                follow.owed = {};
            return decision;
        }
        bool const ended = view.outcome.has_value() || view.nowMs - follow.crossMs >= CROSS_MAX_MS;
        if (!ended)
        {
            decision.act = FollowAct::UnderWay;
            return decision;
        }
        follow.crossMs = 0;
        if (view.stop && view.stopIndex == follow.stop)
            FollowSpend(follow, follow.crossEntry, follow.crossAmount, notes);
        else
            notes.push_back({TransportEvent::CrossCut, follow.crossEntry, follow.crossAmount});
    }
    if (follow.owed.vessel)
    {
        if (std::optional<FollowDecision> const owing = FollowOwed(follow, view, notes))
            return *owing;
    }
    else if (std::uint32_t const from = OwedFrom(follow, view); from != TravelCrossing::NO_STOP)
    {
        std::uint32_t const periodMs = view.periodMs ? view.periodMs : MIN_PERIOD_MS;
        follow = {};
        follow.owed = {view.vessel,
                       view.entry,
                       from,
                       view.masterTo,
                       std::max<std::uint32_t>(view.nowMs, 1),
                       LEG_SLACK_MS + WAIT_PERIODS * periodMs,
                       false};
        follow.stop = from;
        follow.direction = Direction::Board;
        decision.act = FollowAct::Wait;
        return decision;
    }
    if (!view.stop)
    {
        bool const aheadRefused = follow.aheadRefused && view.ahead;
        FollowForget(follow);
        follow.aheadRefused = aheadRefused;
        return FollowAhead(follow, view);
    }
    if (view.stopIndex != follow.stop || view.direction != follow.direction)
    {
        FollowForget(follow);
        follow.stop = view.stopIndex;
        follow.direction = view.direction;
    }
    if (!follow.posedMs)
        follow.posedMs = std::max<std::uint32_t>(view.nowMs, 1);
    TravelCrossing::Stop const& stop = *view.stop;
    TravelCrossing::Row const* const row = FollowRow(follow, stop, view.direction);
    if (!row)
    {
        // An owed ride: no usable row left at the stop it boards at ends it; aboard, it rides on
        if (follow.owed.vessel && !follow.owed.aboard)
            follow.owed = {};
        else if (follow.owed.vessel)
            decision.act = FollowAct::Wait;
        return decision;
    }
    decision.row = row;
    float startYards = 0.0f;
    if (view.direction == Direction::Board)
    {
        TravelRoute::Point const dock{stop.mapId, row->dock.x, row->dock.y, row->dock.z};
        startYards = view.bot.mapId == dock.mapId ? TravelRoute::Distance(view.bot, dock) : DOCK_REACH_YARDS + 1.0f;
        if (startYards > DOCK_REACH_YARDS)
        {
            decision.act = FollowAct::Walk;
            return decision;
        }
    }
    else
    {
        startYards = OffsetYards(view.offset, row->deck);
        if (startYards > DECK_REACH_YARDS)
        {
            decision.act = FollowAct::Deck;
            return decision;
        }
    }
    decision.move = TravelCrossing::MoveOf(*row, view.direction, startYards);
    std::int64_t const needMs = static_cast<std::int64_t>(std::ceil(decision.move.seconds * 1000.0f)) + CROSS_SLACK_MS;
    bool const stands = stop.hasWindow && stop.periodMs ? WindowLeftMs(stop, view.progressMs) >= needMs
                                                        : view.nowMs - follow.posedMs >= TRANSPORT_SCAN_MS;
    decision.act = stands ? FollowAct::Cross : FollowAct::Wait;
    return decision;
}

// Whether the follower's step has work now, asked by the follow action while the follower stands near its master (no
// follow move due): a crossing under way, or the vessel at a stop with an unspent row of the direction (a new docking
// or direction has every row again), or riding with its master the walk ahead of its docking (FollowAhead). Reads
// only; FollowStep acts
inline bool FollowDue(FollowCrossing const& follow, FollowView const& view)
{
    // A ride owed, or one that starts now: the step runs, the follow move is not issued
    if (follow.crossMs || follow.owed.vessel || OwedFrom(follow, view) != TravelCrossing::NO_STOP)
        return true;
    if (!view.stop)
        return FollowAhead(follow, view).act != FollowAct::None;
    if (view.stopIndex != follow.stop || view.direction != follow.direction)
        return !view.stop->Rows(view.direction).empty();
    return FollowRow(follow, *view.stop, view.direction) != nullptr;
}

// A docking the bot needs is near (WT70e, PLAN 3.21 (13)): from DECK_LEAD_MS before the vessel stands at the stop
// until it left, and always at a stop without a window (the leg cannot tell)
inline bool NearDocking(TravelCrossing::Stop const& stop, std::uint32_t progressMs)
{
    return !stop.hasWindow || !stop.periodMs || DocksInMs(stop, progressMs) <= static_cast<std::int64_t>(DECK_LEAD_MS);
}

// The follower's own step is around a docking: a crossing under way, the vessel standing at a stop it works at, an
// owed ride's docking near (its boarding stop off the vessel, the stop it leaves at aboard), or riding with its master
// the walk ahead of the master's docking (FollowAhead)
inline bool FollowFast(FollowCrossing const& follow, FollowView const& view)
{
    if (follow.crossMs || view.stop)
        return true;
    if (follow.owed.vessel)
    {
        TravelCrossing::Stop const* const needs = view.aboard ? view.owedLeave : view.owedStop;
        return view.vessel && (needs ? NearDocking(*needs, view.progressMs) : view.aboard);
    }
    return FollowAhead(follow, view).act != FollowAct::None;
}

// The pace of a follower's checks (BotTravel::PaceMs; WT70e, PLAN 3.21 (13)): the scan's (TRANSPORT_SCAN_MS) while
// its own step is around a docking (FollowFast), else its master's transport leg's pace (ScanPaceMs; masterPaceMs,
// none for a master off one), else, owing a ride, TRANSPORT_SLOW_SCAN_MS; none: the follow code's own pace
inline std::optional<std::uint32_t> FollowPaceMs(FollowCrossing const& follow, FollowView const& view,
                                                 std::optional<std::uint32_t> masterPaceMs)
{
    if (FollowFast(follow, view))
        return TRANSPORT_SCAN_MS;
    if (masterPaceMs)
        return masterPaceMs;
    if (follow.owed.vessel)
        return TRANSPORT_SLOW_SCAN_MS;
    return std::nullopt;
}

// A bot on a transport leg, its walk to the dock included: its followers check at the scan's pace, so the short stand
// at the dock is read as often by them as by it (WT70d-b replay 6: a follower slept through a 2.4 s stand aboard)
inline bool PacesFollowers(Trip const& trip)
{
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == LegType::Transport;
}

// The stop a transport leg's pace waits for (ScanPaceMs, CheckPaceMs, ScanDue): the boarding stop while the bot waits
// or boards, the far stop while it rides (the from-stop once the far stop's rows are spent); none in the other states
inline TravelCrossing::Stop const* PacedStop(Trip const& trip, TravelCrossing::Stop const* boardAt,
                                             TravelCrossing::Stop const* leaveAt)
{
    switch (trip.boarding)
    {
        case Boarding::Wait:
        case Boarding::Board:
            return boardAt;
        case Boarding::Ride:
            return !leaveAt || trip.leaveSpent >= leaveAt->leaveCount ? boardAt : leaveAt;
        default:
            return nullptr;
    }
}

// The scan's pace of a bot on a transport leg (WT70e, PLAN 3.21 (13)), the leg's scan and its checks alike:
// TRANSPORT_SCAN_MS from DECK_LEAD_MS before its vessel stands at the stop it needs (its boarding stop while it waits,
// the far stop while it rides; the from-stop once the far stop's rows are spent) until it crossed or the vessel left,
// and while that stop has no window and the vessel is on the bot's map; a crossing tracked, a leave and the get-off
// too. Else TRANSPORT_SLOW_SCAN_MS: the walk to the dock, a wait or a ride with no docking near, a vessel on another
// map or not seen yet. O(1): the last scan's progress against the stop's window
inline std::uint32_t ScanPaceMs(Trip const& trip, TravelCrossing::Stop const* boardAt,
                                TravelCrossing::Stop const* leaveAt, std::uint32_t mapId, std::uint32_t nowMs)
{
    if (trip.offFirst.sinceMs || trip.crossMs || trip.cancelled)
        return TRANSPORT_SCAN_MS;
    if (!PacesFollowers(trip))
        return TRANSPORT_SLOW_SCAN_MS;
    if (trip.boarding == Boarding::Leave || trip.boarding == Boarding::Back)
        return TRANSPORT_SCAN_MS;
    TravelCrossing::Stop const* const needs = PacedStop(trip, boardAt, leaveAt);
    if (!needs || !trip.sight.found || trip.sight.at.mapId != mapId)
        return TRANSPORT_SLOW_SCAN_MS;
    if (!needs->hasWindow || !needs->periodMs)
        return TRANSPORT_SCAN_MS;
    return DocksInMs(trip, *needs, nowMs) <= static_cast<std::int64_t>(DECK_LEAD_MS) ? TRANSPORT_SCAN_MS
                                                                                     : TRANSPORT_SLOW_SCAN_MS;
}

// The check aimed at the docking (WT70e-d): the engine starts a bot's next check its stagger (CheckStaggerMs: the
// guid's 0-200 ms) after the delay asked for, so at the scan's pace the first check in a stand came up to the pace plus
// the stagger (and the map's tick) into it, and a lift's 1.3-s stand was spent before the crossing's time plus
// CROSS_SLACK_MS (152614, WT70e-d3). So, from a check with the docking docksInMs ahead, the time to it is cut into
// equal steps of at most the pace plus the stagger, and the delay asked is a step minus the stagger: the last check
// lands on the docking's first ms. Equal steps, because a regular step can leave the docking nearer than the stagger,
// which no delay reaches; each check recomputes from its own time, so a late tick is made up by the next step. The
// docking inside the stagger already: the shortest delay. Standing, or no docking foreseen: the pace. O(1)
inline std::uint32_t AimedPaceMs(std::uint32_t paceMs, std::int64_t docksInMs, std::uint32_t staggerMs)
{
    if (docksInMs <= 0 || docksInMs == NO_DEADLINE)
        return paceMs;
    std::int64_t const stepMs = static_cast<std::int64_t>(paceMs) + staggerMs;
    std::int64_t const steps = (docksInMs + stepMs - 1) / stepMs;
    return static_cast<std::uint32_t>(std::max<std::int64_t>(1, docksInMs / steps - staggerMs));
}

// The pace of a transport-leg bot's checks (BotTravel::PaceMs through TransportLegPaceMs): the scan's pace
// (ScanPaceMs), aimed at the docking (AimedPaceMs) while it is fast for the docking of the stop it needs
inline std::uint32_t CheckPaceMs(Trip const& trip, TravelCrossing::Stop const* boardAt,
                                 TravelCrossing::Stop const* leaveAt, std::uint32_t mapId, std::uint32_t nowMs,
                                 std::uint32_t staggerMs)
{
    std::uint32_t const paceMs = ScanPaceMs(trip, boardAt, leaveAt, mapId, nowMs);
    TravelCrossing::Stop const* const needs = PacedStop(trip, boardAt, leaveAt);
    if (paceMs != TRANSPORT_SCAN_MS || !needs || trip.offFirst.sinceMs || trip.crossMs || trip.cancelled)
        return paceMs;
    return AimedPaceMs(paceMs, DocksInMs(trip, *needs, nowMs), staggerMs);
}

// The leg's scan is due at the scan's pace (ScanPaceMs since the last scan, sinceScanMs) and also at the first check
// at or after the docking of the stop it needs when the last sight saw the vessel still on its way there: the aimed
// check (AimedPaceMs) comes sooner than the pace after a scan, and the stand test reads a sight at the stop. O(1)
inline bool ScanDue(Trip const& trip, TravelCrossing::Stop const* boardAt, TravelCrossing::Stop const* leaveAt,
                    std::uint32_t mapId, std::uint32_t sinceScanMs, std::uint32_t nowMs)
{
    if (sinceScanMs >= ScanPaceMs(trip, boardAt, leaveAt, mapId, nowMs))
        return true;
    TravelCrossing::Stop const* const needs = PacedStop(trip, boardAt, leaveAt);
    if (!needs || !trip.sight.found)
        return false;
    std::int64_t const docksInMs = DocksInMs(trip, *needs, trip.sightMs);
    return docksInMs > 0 && docksInMs != NO_DEADLINE && static_cast<std::int64_t>(nowMs - trip.sightMs) >= docksInMs;
}

// A crossing in flight owns the bot (WT70e-b3): while the spline of its own crossing runs (the leg's or a follower's;
// crossingLeftMs its time left), the bot's next check comes after the move's end plus the pace it has (none:
// TRANSPORT_SCAN_MS), at most CROSS_MAX_MS out. A check between replaced the boats' jumps mid-air (an action's stop or
// facing is a new spline at the bot's place), and the passenger check judges a crossing by its own spline. None
// running: the pace as it is
inline std::optional<std::uint32_t> CrossingPaceMs(std::optional<std::uint32_t> paceMs, std::int32_t crossingLeftMs)
{
    if (crossingLeftMs <= 0)
        return paceMs;
    std::uint32_t const leftMs = std::min<std::uint32_t>(static_cast<std::uint32_t>(crossingLeftMs), CROSS_MAX_MS);
    return leftMs + paceMs.value_or(TRANSPORT_SCAN_MS);
}

// Once per transport leg, as it starts: the bot wakes its followers (their next check at the scan's pace), so the
// switch to the scan's pace does not wait for a follower's slow check
inline bool WakeFollowers(Trip& trip)
{
    if (!PacesFollowers(trip) || trip.followersWoken)
        return false;
    trip.followersWoken = true;
    return true;
}

// The follower's walk to the current boarding row's dock point was issued from `yards` away (mark: the glue's)
inline void FollowWalkIssued(FollowCrossing& follow, float yards, std::uint32_t mark)
{
    follow.walkYards = std::max(yards, 0.01f);
    follow.walkMark = mark;
}

// The follower's walk to the dock point ended short of it, the bot `yards` away, or was refused for lack of a path
// (refused). A walk that brought the bot at least DOCK_REACH_YARDS nearer is walked again (true); one that did not,
// or a refused one, gives the dock point up: the row is spent and the next row is walked to, as the leg's rider
// spends a row whose dock point its mover gave up (DockPointGivenUp, RowRefused). No walk issued on this row: walk
inline bool FollowWalkEnded(FollowCrossing& follow, TravelCrossing::Stop const& stop, float yards, bool refused,
                            std::vector<TransportNote>& notes)
{
    bool const issued = follow.walkYards > 0.0f;
    if (!refused && (!issued || follow.walkYards - yards >= DOCK_REACH_YARDS))
    {
        follow.walkYards = 0.0f;
        follow.walkMark = 0;
        return true;
    }
    if (TravelCrossing::Row const* const row = FollowRow(follow, stop, follow.direction))
        FollowSpend(follow, stop.entry, TravelCrossing::EventAmount(stop.stop, follow.direction, row->seq), notes);
    return false;
}

// The glue launched the follower's crossing by the current row: `cross`, tracked until its outcome
inline void FollowCrossIssued(FollowCrossing& follow, TravelCrossing::Stop const& stop,
                              TravelCrossing::Direction direction, std::uint32_t nowMs,
                              std::vector<TransportNote>& notes)
{
    TravelCrossing::Row const* const row = FollowRow(follow, stop, direction);
    if (!row)
        return;
    follow.crossMs = std::max<std::uint32_t>(nowMs, 1);
    follow.crossEntry = stop.entry;
    follow.crossAmount = TravelCrossing::EventAmount(stop.stop, direction, row->seq);
    notes.push_back({TransportEvent::Cross, follow.crossEntry, follow.crossAmount});
}

// Riding with its master: the vessel's path finder refused the deck walk ahead of the docking. No row is spent (at
// the stop the row is tried as ever) and the walk ahead is not asked again on this ride
inline void FollowAheadRefused(FollowCrossing& follow) { follow.aheadRefused = true; }

// The vessel's path finder refused the deck path to the current leaving row's deck point: the row is spent
inline void FollowRowRefused(FollowCrossing& follow, TravelCrossing::Stop const& stop,
                             TravelCrossing::Direction direction, std::vector<TransportNote>& notes)
{
    if (TravelCrossing::Row const* const row = FollowRow(follow, stop, direction))
        FollowSpend(follow, stop.entry, TravelCrossing::EventAmount(stop.stop, direction, row->seq), notes);
}

// The stored row a transport leg (or the get-off) works with now: the from-stop's boarding row in use before the ride
// (the rows before it are spent), the far stop's first unspent leaving row in Leave, the from-stop's in Back, and for
// the get-off the first unspent leaving row of the stop the vessel stands at (Observation::leaveAt). The follower reads
// its crossing's points from it; the rules name none
struct RowRef
{
    TravelCrossing::Stop const* stop{nullptr};
    TravelCrossing::Direction direction{TravelCrossing::Direction::Board};
    std::uint8_t index{0};

    // Null: no stop record, or its rows of this direction are spent
    TravelCrossing::Row const* Get() const
    {
        if (!stop)
            return nullptr;
        std::span<TravelCrossing::Row const> const rows = stop->Rows(direction);
        return index < rows.size() ? &rows[index] : nullptr;
    }

    // The row's Transport event amount (TravelCrossing::EventAmount; 0 without a stop record)
    std::uint32_t Amount() const
    {
        if (!stop)
            return 0;
        TravelCrossing::Row const* const row = Get();
        return TravelCrossing::EventAmount(stop->stop, direction, row ? row->seq : index);
    }
};

// Riding: the row the bot leaves by next, the far stop's while a leaving row is left there, else the from-stop's (the
// way back); its Get() is null when none is left
inline RowRef RideExitRow(Trip const& trip, Observation const& seen)
{
    using TravelCrossing::Direction;
    if (seen.leaveAt && trip.leaveSpent < seen.leaveAt->leaveCount)
        return {seen.leaveAt, Direction::Leave, trip.leaveSpent};
    return {seen.boardAt, Direction::Leave, trip.backSpent};
}

inline RowRef RowNow(Trip const& trip, Observation const& seen)
{
    using TravelCrossing::Direction;
    if (trip.offFirst.sinceMs)
        return {seen.leaveAt, Direction::Leave, trip.offFirst.spent};
    switch (trip.boarding)
    {
        case Boarding::Ride:
            return RideExitRow(trip, seen);
        case Boarding::Leave:
            return {seen.leaveAt, Direction::Leave, trip.leaveSpent};
        case Boarding::Back:
            return {seen.boardAt, Direction::Leave, trip.backSpent};
        default:
            return {seen.boardAt, Direction::Board, trip.row};
    }
}

// The follower issued the crossing by the current row (its one crossing function launched the spline): `cross`. The
// move is tracked from here until its outcome, at most CROSS_MAX_MS
inline void CrossIssued(Trip& trip, Observation const& seen)
{
    RowRef const ref = RowNow(trip, seen);
    if (!ref.Get())
        return;
    trip.crossMs = std::max<std::uint32_t>(seen.nowMs, 1);
    trip.crossAmount = ref.Amount();
    trip.cancelled = false;
    trip.transportNotes.push_back({TransportEvent::Cross, ref.stop->entry, trip.crossAmount});
}

// A crossing is tracked. The follower issues none meanwhile, and discards a move of its own that the rules no longer
// track (CROSS_MAX_MS passed, the get-off began, the leg ended)
inline bool CrossUnderWay(Trip const& trip) { return trip.crossMs != 0; }

// The current row is spent (WT.53: a row that failed is not tried again on the leg): cross_failed with its amount. A
// boarding row: back to the approach, for the next row. The far stop's last leaving row: no_exit (the leg rides back,
// Boarding::Back)
inline void SpendRow(Trip& trip, Observation const& seen)
{
    RowRef const ref = RowNow(trip, seen);
    bool const getOff = trip.offFirst.sinceMs != 0;
    if (!ref.Get() || (!getOff && trip.boarding == Boarding::Ride))
        return;
    trip.transportNotes.push_back({TransportEvent::CrossFailed, ref.stop->entry, ref.Amount()});
    if (getOff)
    {
        ++trip.offFirst.spent;
        return;
    }
    switch (trip.boarding)
    {
        case Boarding::Leave:
            if (++trip.leaveSpent >= ref.stop->leaveCount)
                trip.transportNotes.push_back({TransportEvent::NoExit, ref.stop->entry, 0});
            break;
        case Boarding::Back:
            ++trip.backSpent;
            break;
        default:
            ++trip.row;
            trip.boarding = Boarding::Approach;
            trip.stateMs = seen.nowMs;
            break;
    }
}

// The follower could not use the current row: the mover gave its dock point up (the approach), or the vessel's path
// finder refused the deck path to its deck point (Leave, Back, the get-off). The row is spent; the next step takes the
// next one or ends as the state table says. Not while a crossing is tracked (its outcome decides)
inline void RowRefused(Trip& trip, Observation const& seen)
{
    if (trip.crossMs || trip.cancelled)
        return;
    // The walk ahead of the docking (riding): not asked again on this ride; at the stop the row is tried as ever
    if (trip.boarding == Boarding::Ride && !trip.offFirst.sinceMs)
        trip.aheadRefused = true;
    else
        SpendRow(trip, seen);
}

// The follower's one try for a place aboard (Act::Stand): taken (a complete deck path to the drawn deck point), or the
// bot stays where it is. Once per boarding either way: no second draw, no straight line
inline void NotePlace(Trip& trip, bool taken)
{
    if (trip.placed || !trip.passengerGuid || trip.legIdx >= trip.legs.size())
        return;
    trip.placed = true;
    trip.transportNotes.push_back({TransportEvent::Place, trip.legs[trip.legIdx].object, taken ? 1u : 0u});
}

enum class CrossState : std::uint8_t
{
    None,      // no crossing is tracked
    UnderWay,  // issued, no outcome yet
    Pending,   // it ended without success; the next scan says whether the vessel still stands
    Failed,    // the vessel stood: cross_failed, the row is spent (SpendRow)
    Cut        // the vessel had left: cross_cut, the row stays
};

// A tracked crossing that did not succeed: the passenger check cancelled it, or it has no outcome CROSS_MAX_MS after
// its issue. Whether the row failed or the vessel left under it is read from a scan taken at or after that moment,
// never from an older one (a departure between two scans would spend a good row)
inline CrossState SettleCrossing(Trip& trip, Observation const& seen, TravelCrossing::Stop const* stop,
                                 std::uint32_t entry)
{
    using TravelLifecycle::Outcome;
    bool const cancelled = seen.passengerOutcome == Outcome::Cancelled || seen.passengerOutcome == Outcome::Lost;
    if (trip.crossMs && (cancelled || seen.nowMs - trip.crossMs >= CROSS_MAX_MS))
    {
        trip.crossMs = 0;
        trip.cancelled = true;
    }
    if (trip.crossMs)
        return CrossState::UnderWay;
    if (!trip.cancelled)
        return CrossState::None;
    if (!seen.transport.scanned)
        return CrossState::Pending;
    trip.cancelled = false;
    if (stop && PosedAt(trip.sight, *stop))
    {
        SpendRow(trip, seen);
        return CrossState::Failed;
    }
    trip.transportNotes.push_back({TransportEvent::CrossCut, entry, trip.crossAmount});
    return CrossState::Cut;
}

// The leg's combat time so far (ended stretches and the running one)
inline std::uint32_t CombatMs(Trip const& trip, std::uint32_t nowMs)
{
    LegRecord const& record = trip.record;
    return record.combatMs + (record.combatSinceMs ? nowMs - record.combatSinceMs : 0);
}

// The leg's end aboard: `fight` with the seconds the bot fought since it boarded. Fights aboard are not the leg's (the
// follower does not run in combat); the ride clock ends a long one
inline void NoteFight(Trip& trip, std::uint32_t entry, std::uint32_t nowMs)
{
    if (!trip.passengerGuid)
        return;
    std::uint32_t const totalMs = CombatMs(trip, nowMs);
    std::uint32_t const aboardMs = totalMs > trip.boardCombatMs ? totalMs - trip.boardCombatMs : 0;
    if (aboardMs >= 1000)
        trip.transportNotes.push_back({TransportEvent::Fight, entry, aboardMs / 1000});
}

// A transport leg by stored crossings (WT70c, WT.53; PLAN 3.21 (3)): no search, no ray cast, no hop, no teleport.
// - Approach: walk on the world mesh to the current boarding row's dock point; within DOCK_REACH_YARDS `attempt` (once
//   per leg) and Wait. No boarding row, no leaving row at the far stop, or a seen vessel without a mesh: `no_crossing`
//   at once, before any walk.
// - Wait: stand on the dock point; off it, back to Approach. The vessel stands at the stop's pose with time for the
//   row: Board.
// - Board: the follower crosses by the row (one tracked move, CrossIssued). Boarded: `crossed`, `boarded`, Ride. The
//   move cancelled and the vessel gone: `cross_cut`, after MISS_GRACE_MS `missed`, Wait, the same row at the next
//   docking. Cancelled while the vessel stands: `cross_failed`, the row is spent, Approach with the next; none left:
//   the leg fails crossing_failed.
// - Ride: take a place once (Act::Stand, NotePlace), then stay; from DECK_LEAD_MS before the docking at the stop it
//   leaves by (by that stop's window) on the leaving row's deck point (Act::Deck). The vessel stands at the far stop:
//   `leave`, Leave. The
//   far stop's leaving rows spent and it stands at the from-stop: Back.
// - Leave: walk the deck to the leaving row's deck point (Act::Deck), then cross by the row (Act::Disembark). Exited:
//   `crossed`, `arrived`, done. The vessel leaves first: `stuck_aboard`, Ride (the round trip brings it back). The deck
//   path refused (RowRefused) or the crossing cancelled while it stands: the row is spent, the next; none: `no_exit`,
//   Ride.
// - Back: as Leave, at the from-stop by that stop's leaving rows. Exited: `back`, the leg fails no_exit. No row works:
//   Ride.
// Every state ends: off the vessel by the wait clock (WAIT_PERIODS periods from the leg's start), aboard by the ride
// clock (RIDE_PERIODS from the boarding), a crossing by CROSS_MAX_MS (NextDeadlineMs is the clocks' one reader). A
// vessel that docks under the waiting bot boards it by intent (BoardedByIntent), without a crossing
inline Decision StepTransport(Trip& trip, Observation const& seen)
{
    using TravelCrossing::Direction;
    using TravelLifecycle::Outcome;
    Leg const& leg = trip.legs[trip.legIdx];
    std::uint32_t const nowMs = seen.nowMs;
    TravelCrossing::Stop const* const boardAt = seen.boardAt;
    TravelCrossing::Stop const* const leaveAt = seen.leaveAt;
    auto const enter = [&trip, nowMs](Boarding state)
    {
        trip.boarding = state;
        trip.stateMs = nowMs;
    };
    auto const note = [&trip, &leg](TransportEvent event, std::uint32_t amount)
    { trip.transportNotes.push_back({event, leg.object, amount}); };
    auto const seconds = [nowMs](std::uint32_t sinceMs) { return (nowMs - sinceMs) / 1000; };
    // The leg's end: no crossing is tracked any more, and a fight aboard is reported
    auto const end = [&trip, &leg, nowMs](Act act, FailReason reason)
    {
        trip.crossMs = 0;
        trip.cancelled = false;
        NoteFight(trip, leg.object, nowMs);
        return Decision{act, act == Act::Done ? leg.to : Point{}, reason};
    };
    auto const fail = [&end](FailReason reason) { return end(Act::Fail, reason); };
    auto const dockOf = [](TravelCrossing::Stop const& stop, TravelCrossing::Row const& row)
    { return Point{stop.mapId, row.dock.x, row.dock.y, row.dock.z}; };

    if (!seen.membershipAvailable)
        return {Act::Wait, leg.to, FailReason::None};
    if (seen.transport.scanned)
        NoteScan(trip, seen);
    // Boarded: by the crossing's outcome, or the leg's vessel docked under the bot and the passenger check attached it
    bool const crossed = trip.boarding == Boarding::Board && seen.passengerOutcome == Outcome::Boarded;
    if (!trip.passengerGuid && seen.aboardGuid &&
        ((crossed && seen.aboard == leg.object) || (!seen.boardMotion && BoardedByIntent(trip, seen.aboardGuid))))
    {
        if (crossed)
            note(TransportEvent::Crossed, trip.crossAmount);
        note(TransportEvent::Boarded, seconds(trip.waitSinceMs));
        trip.passengerGuid = seen.aboardGuid;
        trip.boardedMs = nowMs;
        trip.crossMs = 0;
        trip.cancelled = false;
        trip.placed = false;
        trip.aheadRefused = false;
        trip.boardCombatMs = CombatMs(trip, nowMs);
        enter(Boarding::Ride);
    }
    if (trip.passengerGuid)
    {
        bool const back = trip.boarding == Boarding::Back;
        bool const leaving = trip.boarding == Boarding::Leave || back;
        if (seen.aboardGuid != trip.passengerGuid)
        {
            if (!seen.aboardGuid && leaving && seen.passengerOutcome == Outcome::Exited)
            {
                note(TransportEvent::Crossed, trip.crossAmount);
                note(back ? TransportEvent::Back : TransportEvent::Arrived, seconds(trip.boardedMs));
                // Back ashore where it boarded: the leg failed, the replan leaves this link out (FailureOf)
                return back ? fail(FailReason::NoExit) : end(Act::Done, FailReason::None);
            }
            note(TransportEvent::Lost, seconds(trip.boardedMs));
            return fail(FailReason::TransportLost);
        }
        if (NextDeadlineMs(trip, nowMs, seen.legMs) < 0)
            return fail(FailReason::Timeout);
        if (seen.bot.mapId != leg.from.mapId && seen.bot.mapId != leg.to.mapId)
            return fail(FailReason::WrongMap);
        TravelCrossing::Stop const* const exitAt = back ? boardAt : leaveAt;
        if (leaving)
        {
            CrossState const crossing = SettleCrossing(trip, seen, exitAt, leg.object);
            if (crossing == CrossState::UnderWay)
                return {Act::Disembark, leg.to, FailReason::None};
            if (crossing == CrossState::Pending)
                return {Act::Wait, leg.to, FailReason::None};
        }
        // Where it leaves now: the far stop while a leaving row is left there; with those spent, the from-stop
        bool const exitless = !leaveAt || trip.leaveSpent >= leaveAt->leaveCount;
        TravelCrossing::Stop const* by = nullptr;
        if (!exitless && Stood(trip, *leaveAt))
            by = leaveAt;
        else if (exitless && boardAt && trip.backSpent < boardAt->leaveCount && Stood(trip, *boardAt))
            by = boardAt;
        if (by)
        {
            Boarding const state = by == leaveAt ? Boarding::Leave : Boarding::Back;
            if (trip.boarding != state)
            {
                if (state == Boarding::Leave)
                    note(TransportEvent::Leave, seconds(trip.boardedMs));
                enter(state);
            }
            TravelCrossing::Row const& row = by->leave[state == Boarding::Leave ? trip.leaveSpent : trip.backSpent];
            Point const dock = dockOf(*by, row);
            float const away = OffsetYards(seen.offset, row.deck);
            if (away > DECK_REACH_YARDS)
                return {Act::Deck, dock, FailReason::None};
            // Too late for the row at this docking: it leaves with the bot (stuck_aboard) and comes round again
            if (!HasTimeFor(trip, *by, TravelCrossing::MoveOf(row, Direction::Leave, away), nowMs))
                return {Act::Wait, dock, FailReason::None};
            return {Act::Disembark, dock, FailReason::None};
        }
        if (leaving)
        {
            // It left with the bot (stuck_aboard), or the stop's rows are spent while it stands: the ride goes on
            if (!exitAt || !Stood(trip, *exitAt))
                note(TransportEvent::StuckAboard, seconds(trip.stateMs));
            enter(Boarding::Ride);
        }
        // Ahead of the docking (WT70d-b): placed, the rider walks to the row it leaves by once the vessel docks at that
        // stop within DECK_LEAD_MS by its window, so a short stand holds only the crossing. A docking that cannot be
        // foreseen (no window) is walked at the stop, as above
        RowRef const ahead = RideExitRow(trip, seen);
        std::int64_t const docksInMs = ahead.Get() ? DocksInMs(trip, *ahead.stop, nowMs) : NO_DEADLINE;
        // No place within the lead (WT70e, PLAN 3.21 (6)): a rider whose far docking is that near at its boarding takes
        // none (place:0) and walks to the leaving row at once: no two walks in a ride of a few seconds
        if (!trip.placed && docksInMs <= static_cast<std::int64_t>(DECK_LEAD_MS))
            NotePlace(trip, false);
        if (trip.placed && !trip.aheadRefused && ahead.Get() &&
            DeckLeadOf(docksInMs, seen.offset, *ahead.Get()) == DeckLead::Walk)
            return {Act::Deck, dockOf(*ahead.stop, *ahead.Get()), FailReason::None};
        return {trip.placed ? Act::Wait : Act::Stand, leg.to, FailReason::None};
    }

    if (NextDeadlineMs(trip, nowMs, seen.legMs) < 0)
        return fail(FailReason::Timeout);
    // Aboard a vessel that is not this leg's ride (yet): the passenger check settles it, the wait clock bounds it
    if (seen.aboardGuid)
        return {Act::Wait, leg.to, FailReason::None};
    CrossState const crossing = SettleCrossing(trip, seen, boardAt, leg.object);
    // No crossing, no ride (WT.53): every boarding row failed while the vessel stood; or the stop has no boarding row,
    // the far stop no leaving row, or the vessel seen holds no mesh. Before any walk, and never a fallback
    if (boardAt && boardAt->boardCount && trip.row >= boardAt->boardCount)
        return fail(FailReason::CrossingFailed);
    RowRef const board{boardAt, Direction::Board, trip.row};
    if (!board.Get() || !leaveAt || !leaveAt->leaveCount || (trip.sight.found && !trip.sight.mesh))
    {
        note(TransportEvent::NoCrossing, board.Amount());
        return fail(FailReason::NoCrossing);
    }
    if (seen.bot.mapId != leg.from.mapId)
        return fail(FailReason::WrongMap);
    TravelCrossing::Row const& row = *board.Get();
    Point const dock = dockOf(*boardAt, row);
    if (crossing == CrossState::UnderWay)
        return {Act::Board, dock, FailReason::None};
    if (crossing == CrossState::Pending)
        return {Act::Wait, dock, FailReason::None};
    if (crossing == CrossState::Cut)
    {
        trip.departed = true;
        trip.departedMs = nowMs;
    }
    bool const atDock = seen.bot.mapId == dock.mapId && Distance(seen.bot, dock) <= DOCK_REACH_YARDS;
    if (trip.boarding == Boarding::Approach)
    {
        if (!atDock)
            return {Act::Walk, dock, FailReason::None};
        if (!trip.attempted)
        {
            trip.attempted = true;
            note(TransportEvent::Attempt, seconds(trip.legStartMs));
            trip.waitSinceMs = nowMs;
        }
        enter(Boarding::Wait);
    }
    bool const ready =
        atDock &&
        HasTimeFor(trip, *boardAt, TravelCrossing::MoveOf(row, Direction::Board, Distance(seen.bot, dock)), nowMs);
    if (trip.boarding == Boarding::Wait)
    {
        if (!atDock)
        {
            enter(Boarding::Approach);
            return {Act::Walk, dock, FailReason::None};
        }
        if (!ready)
            return {Act::Wait, dock, FailReason::None};
        trip.departed = false;
        enter(Boarding::Board);
    }
    // Board: the follower issues the crossing by the row once the bot is ready for it (its readiness gate)
    if (Stood(trip, *boardAt))
    {
        trip.departed = false;
        return {ready ? Act::Board : Act::Wait, dock, FailReason::None};
    }
    if (!trip.departed)
    {
        trip.departed = true;
        trip.departedMs = nowMs;
    }
    if (nowMs - trip.departedMs < MISS_GRACE_MS)
        return {Act::Wait, dock, FailReason::None};
    trip.misses = static_cast<std::uint8_t>(std::min(trip.misses + 1, 255));
    note(TransportEvent::Missed, trip.misses);
    enter(Boarding::Wait);
    return {Act::Wait, dock, FailReason::None};
}

// The bot walks off the graph now (WT56): its walk leg's waypoint pair is no graph link (the bot's start to a node, the
// last node to the goal, a direct walk)
inline bool WalksOffGraph(Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != LegType::Walk || trip.next == 0 ||
        trip.next >= trip.waypoints.size())
        return false;
    return trip.waypoints[trip.next - 1].node == NO_NODE || trip.waypoints[trip.next].node == NO_NODE;
}

// An overdue walk (its walk budget spent, or the bot latched off the mesh; attempted: the follower ordered a move on
// it, offMesh: the give-up found the bot off the mesh). One path for a refused walk (WT.56): a walk off the graph that
// was refused at its start, never moved on or stalled under way fails NoWalk, and the replan leaves it out, within
// MAX_REPLANS; only a bot that moved and stands off the mesh keeps its recovery (OffMesh: the hearth or the snap,
// which a replan's walk would break). A graph link's walk: Stuck after a move, None without one (the trip ends
// stalled)
inline FailReason OverdueWalkReason(Trip const& trip, bool attempted, bool offMesh)
{
    if (attempted && offMesh)
        return FailReason::OffMesh;
    if (WalksOffGraph(trip))
        return FailReason::NoWalk;
    return attempted ? FailReason::Stuck : FailReason::None;
}

// How close a walk leg's waypoint counts as reached: the route's last one GOAL_YARDS, the others WAYPOINT_YARDS. The
// follower orders a waypoint until then, and the probe of a walk off the graph is done there (TravelWalk::ProbePiece)
inline float WaypointReach(Trip const& trip, std::uint32_t index)
{
    return trip.legIdx + 1 == trip.legs.size() && index + 1 == trip.waypoints.size() ? GOAL_YARDS : WAYPOINT_YARDS;
}

// The current leg's next step. Done, Fail and the others; never Arrived
inline Decision StepLeg(Trip& trip, Observation const& seen)
{
    Leg const& leg = trip.legs[trip.legIdx];
    auto const overdue = [&trip, &seen] { return NextDeadlineMs(trip, seen.nowMs, seen.legMs) < 0; };
    auto const fail = [](FailReason reason) { return Decision{Act::Fail, {}, reason}; };
    auto const within = [&seen](Point const& at, float yards)
    { return seen.bot.mapId == at.mapId && Distance(seen.bot, at) <= yards; };

    switch (leg.type)
    {
        case LegType::Walk:
        {
            if (seen.bot.mapId != leg.to.mapId)
                return fail(FailReason::WrongMap);
            std::uint32_t const count = static_cast<std::uint32_t>(trip.waypoints.size());
            auto const reached = [&](std::uint32_t index)
            { return within(trip.waypoints[index].at, WaypointReach(trip, index)); };
            // The current waypoint, or the one after it when the path went past a node
            for (std::uint32_t passed = 0; passed < WAYPOINTS_PER_TICK && trip.next < count; ++passed)
            {
                if (reached(trip.next))
                    ++trip.next;
                else if (trip.next + 1 < count && reached(trip.next + 1))
                    trip.next += 2;
                else
                    break;
            }
            if (trip.next >= count)
                return {Act::Done, leg.to, FailReason::None};
            // A walk off the graph past the deadline did not carry the bot: one path for a refused walk (WT.56,
            // OverdueWalkReason), no_walk and a replan without it; a graph link's walk times out
            if (overdue())
                return fail(WalksOffGraph(trip) ? OverdueWalkReason(trip, true, false) : FailReason::Timeout);
            return {Act::Walk, trip.waypoints[trip.next].at, FailReason::None};
        }
        case LegType::Portal:
        case LegType::StaticPortal:
        {
            bool const exited =
                leg.from.mapId != leg.to.mapId ? seen.bot.mapId == leg.to.mapId : within(leg.to, PORTAL_EXIT_YARDS);
            if (exited)
                return {Act::Done, leg.to, FailReason::None};
            if (overdue())
                return fail(FailReason::Portal);
            if (seen.bot.mapId != leg.from.mapId)
                return fail(FailReason::WrongMap);
            if (trip.triggered)
                return {Act::Wait, leg.to, FailReason::None};
            // From the trigger's (object's) node on, the bot heads for the trigger or object itself (UsePortal,
            // UseStaticPortal), which may lie further than WAYPOINT_YARDS on: turning back to the node would alternate
            // with that walk until the leg timed out
            if (!trip.approached && !within(leg.from, WAYPOINT_YARDS))
                return {Act::Walk, leg.from, FailReason::None};
            trip.approached = true;
            return {leg.type == LegType::Portal ? Act::UseTrigger : Act::UseObject, leg.from, FailReason::None};
        }
        case LegType::Flight:
        {
            if (!seen.inFlight && within(leg.to, LANDING_YARDS))
                return {Act::Done, leg.to, FailReason::None};
            if (seen.inFlight)
            {
                // A flight always ends: no timeout in the air (a bot cannot leave the taxi anyway)
                trip.seenInFlight = true;
                return {Act::Ride, leg.to, FailReason::None};
            }
            if (overdue())
                return fail(FailReason::Timeout);
            if (trip.seenInFlight)
                return fail(FailReason::Landing);
            if (trip.taxiActivated)
                return {Act::Wait, leg.to, FailReason::None};
            if (seen.money < leg.fareCopper)
                return fail(FailReason::Fare);
            if (seen.bot.mapId != leg.from.mapId)
                return fail(FailReason::WrongMap);
            if (!within(leg.from, FLIGHT_MASTER_YARDS))
                return {Act::Walk, leg.from, FailReason::None};
            return {Act::TalkFlightMaster, leg.from, FailReason::None};
        }
        case LegType::Fly:
        {
            if (seen.bot.mapId != leg.to.mapId)
                return fail(FailReason::WrongMap);
            std::uint32_t const count = static_cast<std::uint32_t>(trip.waypoints.size());
            // The landing (on the ground) is reached within WAYPOINT_YARDS, a point in the air within
            // FLY_WAYPOINT_YARDS
            auto const reached = [&](std::uint32_t index)
            { return within(trip.waypoints[index].at, index + 1 == count ? WAYPOINT_YARDS : FLY_WAYPOINT_YARDS); };
            for (std::uint32_t passed = 0; passed < WAYPOINTS_PER_TICK && trip.next < count; ++passed)
            {
                if (reached(trip.next))
                    ++trip.next;
                else if (trip.next + 1 < count && reached(trip.next + 1))
                    trip.next += 2;
                else
                    break;
            }
            if (trip.next >= count)
                return {Act::Done, leg.to, FailReason::None};
            if (seen.flying)
            {
                trip.peakZ = trip.seenInFlight ? std::max(trip.peakZ, seen.bot.z) : seen.bot.z;
                trip.seenInFlight = true;
            }
            else if (trip.seenInFlight)
            {
                // Down after the takeoff: landed at the end, or fell or was dismounted on the way
                if (within(leg.to, LANDING_YARDS))
                    return {Act::Done, leg.to, FailReason::None};
                return fail(FailReason::Landing);
            }
            if (overdue())
                return fail(FailReason::Timeout);
            if (!trip.seenInFlight)
            {
                // No takeoff where the bot stands fails at once: the next route starts no fly leg there
                if (!seen.takeoffHere)
                    return fail(FailReason::Takeoff);
                if (!seen.flyingMount)
                {
                    // A try asked for but skipped by the glue (combat, a cast) is asked for again next tick
                    if (trip.mountTries && seen.nowMs - trip.mountTriedMs < FLY_MOUNT_RETRY_MS)
                        return {Act::Wait, leg.from, FailReason::None};
                    if (trip.mountTries >= FLY_MOUNT_TRIES)
                        return fail(FailReason::Takeoff);
                    return {Act::Mount, leg.from, FailReason::None};
                }
            }
            return {Act::Fly, trip.waypoints[trip.next].at, FailReason::None};
        }
        case LegType::Hearth:
        {
            // The hearthstone's teleport is a far teleport (the trip survives it): done on the bind's map near the bind
            if (within(leg.to, HEARTH_ARRIVE_YARDS))
                return {Act::Done, leg.to, FailReason::None};
            if (overdue())
                return fail(FailReason::Timeout);
            if (seen.bot.mapId != leg.from.mapId)
                return fail(FailReason::WrongMap);
            return {Act::UseHearth, leg.to, FailReason::None};
        }
        case LegType::Transport:
            return StepTransport(trip, seen);
        default:
            return fail(FailReason::Unsupported);
    }
}

// The gate before a trip's end, a route request or a walk (WT19-d): aboard a ship or zeppelin (its entry, 0 = none) the
// bot gets off first, bounded by the ship's period (LEG_SLACK_MS while unknown). True: the end, request or walk waits
// for StepOffFirst. Not while one is under way, and not again on a trip after a period passed aboard
inline bool BeginOffFirst(Trip& trip, std::uint32_t shipEntry, std::uint64_t guid, std::uint32_t periodMs,
                          std::uint32_t nowMs, OffFirstEnd end = OffFirstEnd::None, char const* why = nullptr,
                          char const* source = nullptr, StatusEnd::Outcome outcome = StatusEnd::Outcome::Abandoned)
{
    OffFirst& off = trip.offFirst;
    if (!guid || off.sinceMs || off.gaveUp)
        return false;
    off.sinceMs = std::max<std::uint32_t>(nowMs, 1);
    off.entry = shipEntry;
    off.guid = guid;
    off.boundMs = periodMs ? periodMs : LEG_SLACK_MS;
    off.end = end;
    off.why = why;
    off.source = source;
    off.outcome = outcome;
    off.stop = TravelCrossing::NO_STOP;
    off.spent = 0;
    // The scans start anew, and a crossing of the leg is tracked no more (the follower discards its move)
    trip.sight = {};
    trip.sightMs = 0;
    trip.posedMs = 0;
    trip.crossMs = 0;
    trip.cancelled = false;
    trip.transportNotes.push_back({TransportEvent::OffFirst, shipEntry, off.boundMs / 1000});
    return true;
}

enum class OffStep : std::uint8_t
{
    Wait,
    Deck,       // walk the deck to the leaving row's deck point (RowNow)
    Disembark,  // cross by that row
    Off,
    Lost,
    GiveUp
};

// One tick of the get-off (seen.aboard; seen.transport: the ridden vessel's scan, once a TRANSPORT_SCAN_MS;
// seen.leaveAt: the stop record the vessel stands at, null at none). It leaves by that stop's leaving rows under the
// leg's stand test; a stop without rows, or with its rows spent, is waited out. Off and GiveUp end it (sinceMs 0;
// off.end and its words stay for the follower)
inline OffStep StepOffFirst(Trip& trip, Observation const& seen)
{
    OffFirst& off = trip.offFirst;
    if (seen.transport.scanned)
        NoteScan(trip, seen);
    if (!seen.membershipAvailable)
        return OffStep::Wait;
    auto const close = [&trip, &off]
    {
        off.sinceMs = 0;
        trip.crossMs = 0;
        trip.cancelled = false;
    };
    if (seen.aboardGuid != off.guid)
    {
        bool const exited = !seen.aboardGuid && seen.passengerOutcome == TravelLifecycle::Outcome::Exited;
        if (exited && trip.crossMs)
            trip.transportNotes.push_back({TransportEvent::Crossed, off.entry, trip.crossAmount});
        close();
        if (exited)
            return OffStep::Off;
        off.gaveUp = true;
        trip.transportNotes.push_back({TransportEvent::Lost, off.entry, 0});
        return OffStep::Lost;
    }
    if (off.sinceMs && NextDeadlineMs(trip, seen.nowMs, seen.legMs) < 0)
    {
        trip.transportNotes.push_back({TransportEvent::StuckAboard, off.entry, (seen.nowMs - off.sinceMs) / 1000});
        close();
        off.gaveUp = true;
        return OffStep::GiveUp;
    }
    TravelCrossing::Stop const* const stop = seen.leaveAt;
    // Another stop: its rows are all unspent
    if (stop && stop->stop != off.stop && !trip.crossMs && !trip.cancelled)
    {
        off.stop = stop->stop;
        off.spent = 0;
    }
    CrossState const crossing = SettleCrossing(trip, seen, stop, off.entry);
    if (crossing == CrossState::UnderWay)
        return OffStep::Disembark;
    if (crossing == CrossState::Pending)
        return OffStep::Wait;
    if (!stop || off.spent >= stop->leaveCount || !Stood(trip, *stop))
        return OffStep::Wait;
    TravelCrossing::Row const& row = stop->leave[off.spent];
    float const away = OffsetYards(seen.offset, row.deck);
    if (away > DECK_REACH_YARDS)
        return OffStep::Deck;
    TravelCrossing::Move const move = TravelCrossing::MoveOf(row, TravelCrossing::Direction::Leave, away);
    return HasTimeFor(trip, *stop, move, seen.nowMs) ? OffStep::Disembark : OffStep::Wait;
}

// The trip's next step: finished legs are passed (at most LEGS_PER_TICK), then the current leg's step; Arrived after
// the last leg. A transport leg that a finished leg leads to takes its first step on the next tick (Wait now)
template <class NodeAt>
Decision Advance(Trip& trip, Observation seen, NodeAt const& nodeAt)
{
    for (std::uint32_t passed = 0; passed < LEGS_PER_TICK; ++passed)
    {
        if (trip.legIdx >= trip.legs.size())
            return {Act::Arrived, seen.bot, FailReason::None};
        Decision const decision = StepLeg(trip, seen);
        if (decision.act != Act::Done)
            return decision;
        StartLeg(trip, trip.legIdx + 1, seen.nowMs, nodeAt);
        seen.legMs = 0;  // the next leg's clock starts with it
        // An observation is of the leg it was taken on: its stop records and its scan (boardAt, leaveAt, transport)
        // are read once per tick for that leg. A transport leg therefore begins with the next tick's own, never with
        // the empty ones of the leg before it, which would read as "no crossing"
        if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == LegType::Transport)
            return {Act::Wait, seen.bot, FailReason::None};
    }
    if (trip.legIdx >= trip.legs.size())
        return {Act::Arrived, seen.bot, FailReason::None};
    return {Act::Wait, seen.bot, FailReason::None};
}

enum class Recovery : std::uint8_t
{
    Replan,  // ask for a new route from where the bot stands
    Abort    // give the trip up (IDLE)
};

// A stuck walk at the waypoint of the trip's last one: the second ends the trip (AfterFailure). What the walk failed
// at is the record's (FailureOf, BotTravel::NoteFailure). A walk heads for its next waypoint, a portal or flight leg
// for its start; a hearth or fly leg walks nowhere. Each call keeps the waypoint for the next
inline bool StuckAgain(Trip& trip)
{
    if (trip.legIdx >= trip.legs.size() || trip.waypoints.empty())
        return false;
    LegType const type = trip.legs[trip.legIdx].type;
    if (type == LegType::Hearth || type == LegType::Fly)
        return false;  // no walk
    std::uint32_t const last = static_cast<std::uint32_t>(trip.waypoints.size() - 1);
    std::uint32_t const index = type == LegType::Walk ? std::min(trip.next, last) : 0;
    Point const& target = trip.waypoints[index].at;
    bool const again =
        trip.stuckSeen && target.mapId == trip.stuckAt.mapId && Distance(target, trip.stuckAt) <= WAYPOINT_YARDS;
    trip.stuckSeen = true;
    trip.stuckAt = target;
    return again;
}

// The graph edge the bot is on: a walk's two waypoints around it, or a (static) portal's two nodes; {NO_NODE, NO_NODE}
// when an end is no graph node (the bot's start, the goal)
inline std::pair<std::uint32_t, std::uint32_t> StuckEdge(Trip const& trip)
{
    std::pair<std::uint32_t, std::uint32_t> const none{NO_NODE, NO_NODE};
    if (trip.legIdx >= trip.legs.size() || trip.next == 0 || trip.next >= trip.waypoints.size())
        return none;
    LegType const type = trip.legs[trip.legIdx].type;
    if (type != LegType::Walk && type != LegType::Portal && type != LegType::StaticPortal)
        return none;
    std::uint32_t const from = trip.waypoints[trip.next - 1].node;
    std::uint32_t const to = trip.waypoints[trip.next].node;
    if (from == NO_NODE || to == NO_NODE)
        return none;
    return {from, to};
}

// The trip's next route request: the bot's snapshot and the plain copy of its record's live entries
// (FailedAttempts::LiveEntries, taken on the map thread: CONTRACT C3). What this trip's failed legs and the bot's
// earlier trips failed at is in it (a graph link, a walk off the graph, a takeoff spot, a place, flying, the
// hearthstone: TravelRoute::Refusals), so the replan leaves it out
inline TravelRoute::Request NextRequest(TravelRoute::Snapshot const& snapshot, Point const& goal,
                                        std::vector<FailedAttempts::Entry> refused)
{
    TravelRoute::Request request{snapshot, goal};
    request.refused = std::move(refused);
    return request;
}

// After a failed leg, whose failure the record holds already (FailureOf, BotTravel::NoteFailure; the replan's request
// leaves it out: NextRequest): a new route while replans are left; an unsupported leg or a search without a route
// ends the trip at once (a new search would give the same answer), and so does a second stuck walk at the same
// waypoint (StuckAgain). A bot off the mesh ends its trip at the first such leg: it recovers by hearth or a short
// move (MoveFarTo), not by replans. Deaths: TRIP_DEATHS on the trip end it. A takeoff refused where the bot stands
// (spotClosed: a no-fly area, a battle, indoors, water) replans flying: the record refuses that spot (cause takeoff)
// and the ones before, so the bot walks to the next open point and tries there. Such replans have their own bound,
// MAX_SPOT_FAILS, outside MAX_REPLANS (a city like Dalaran has many closed points in a row); a later one is an
// ordinary takeoff failure
inline Recovery AfterFailure(Trip& trip, FailReason reason, bool spotClosed = false)
{
    bool const spotRetry = reason == FailReason::Takeoff && spotClosed && trip.spotFails < MAX_SPOT_FAILS;
    std::uint32_t const combatSinceMs = trip.record.combatSinceMs;
    trip.record = LegRecord{};  // the failed leg's record ends (a running combat stretch goes on)
    trip.record.combatSinceMs = combatSinceMs;
    if (spotRetry)
    {
        ++trip.spotFails;
        return Recovery::Replan;
    }
    if (reason == FailReason::Unsupported || reason == FailReason::NoRoute || reason == FailReason::OffMesh ||
        (reason == FailReason::Death && trip.deaths >= TRIP_DEATHS) || trip.replans >= MAX_REPLANS)
        return Recovery::Abort;
    if (reason == FailReason::Stuck && StuckAgain(trip))
        return Recovery::Abort;
    ++trip.replans;
    return Recovery::Replan;
}

// After a fly leg failed and the trip replans (WU11c, WU11b review 2): a bot has no client gravity, so one still up
// stays where the leg failed and every walk replan fails at that spot (Balthas, Farnin no_progress; Elyvia three
// fell_or_swimming 200 yd up) until the trip ends. More than AIRBORNE_YARDS above its floor (the higher of ground and
// water, as BodyZ) it comes down first: on its flying mount by a straight flight down (Descend, StepDescent goes on),
// without one it falls as a player does (Fall). Done: not a fly leg, on the floor, or no floor found
inline Decision StartDescent(Trip& trip, Point const& bot, float groundZ, float waterZ, bool flyingMount,
                             std::uint32_t nowMs)
{
    trip.descending = false;
    trip.endAfterDescent = false;
    Decision const done{Act::Done, bot, FailReason::None};
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != LegType::Fly)
        return done;
    float const floorZ = BodyZ(bot.z, groundZ, waterZ);
    if (floorZ >= bot.z)
        return done;
    Point const floor{bot.mapId, bot.x, bot.y, floorZ};
    if (!flyingMount)
        return {Act::Fall, floor, FailReason::None};
    trip.descending = true;
    trip.descentTo = floor;
    trip.descentStartMs = nowMs;
    return {Act::Descend, floor, FailReason::None};
}

// The flight down's tick: Done within AIRBORNE_YARDS of the floor (the glue lands the bot), Fall when the flying mount
// is gone on the way, Fail on another map (WrongMap) or after DESCENT_TIMEOUT_MS (Timeout: the replan starts where the
// bot is, as before WU11c), else Descend on. Only Descend keeps the descent
inline Decision StepDescent(Trip& trip, Point const& bot, bool flyingMount, std::uint32_t nowMs)
{
    if (!trip.descending)
        return {Act::Done, bot, FailReason::None};
    Point const floor = trip.descentTo;
    trip.descending = false;
    if (bot.mapId != floor.mapId)
        return {Act::Fail, bot, FailReason::WrongMap};
    if (bot.z - floor.z <= AIRBORNE_YARDS)
        return {Act::Done, floor, FailReason::None};
    Point const below{bot.mapId, bot.x, bot.y, floor.z};
    if (!flyingMount)
        return {Act::Fall, below, FailReason::None};
    if (nowMs - trip.descentStartMs > DESCENT_TIMEOUT_MS)  // unsigned: wraps as getMSTimeDiff does
        return {Act::Fail, bot, FailReason::Timeout};
    trip.descending = true;
    return {Act::Descend, floor, FailReason::None};
}

// An aborting trip (AfterFailure: Abort) whose failed leg was a fly leg comes down first, as StartDescent decides, and
// ends once it is down (WU12b, WU11c review NOTE 1: it hovered where the leg failed). The result is StartDescent's;
// EndsAfterDescent says when the trip ends: at once for Done (nothing to descend), after the flight down or the fall
inline Decision AbortDescent(Trip& trip, FailReason reason, Point const& bot, float groundZ, float waterZ,
                             bool flyingMount, std::uint32_t nowMs)
{
    Decision const down = StartDescent(trip, bot, groundZ, waterZ, flyingMount, nowMs);
    trip.endAfterDescent = true;
    trip.endReason = reason;
    return down;
}

// The descent's decision ends the aborting trip: every act but Descend (Done, Fall, Fail)
inline bool EndsAfterDescent(Trip const& trip, Decision const& down)
{
    return trip.endAfterDescent && down.act != Act::Descend;
}

// A fly leg's real-height check is due: the bot heads for a waypoint in the air the check has not looked at yet
inline bool NeedsHeightCheck(Trip const& trip)
{
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == LegType::Fly &&
           trip.next + 1 < trip.waypoints.size() && trip.heightChecked != trip.next;
}

// The check's result: the ground samples along the way to the next waypoint (RequiredZ) raise it when a building or
// a hill the height map missed stands in the way; true when raised (counted in climbs)
inline bool CheckHeight(Trip& trip, std::vector<float> const& samples)
{
    if (!NeedsHeightCheck(trip))
        return false;
    trip.heightChecked = trip.next;
    Point& target = trip.waypoints[trip.next].at;
    float const z = RequiredZ(samples, target.z, trip.legs[trip.legIdx].flightClearance);
    if (z <= target.z)
        return false;
    target.z = z;
    trip.climbs = static_cast<std::uint16_t>(std::min(trip.climbs + 1, 0xFFFF));
    return true;
}

// The landing spot's check (WU11b): a fly leg planned onto an indoor goal (an inn's floor) comes down on the roof
// above it, more than LANDING_YARDS from the leg's end. Once per fly leg, on the descent within LANDING_CHECK_YARDS
// (2D) of the landing, where the bot has the landing's grid loaded, the follower asks whether the landing lies under
// the open sky (the flight map cannot tell: indoors is a WMO property)
inline bool NeedsLandingCheck(Trip const& trip, Point const& bot)
{
    if (trip.landingChecked || trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != LegType::Fly ||
        trip.waypoints.size() < 3 || trip.next + 2 < trip.waypoints.size())
        return false;
    Point const& landing = trip.waypoints.back().at;
    return bot.mapId == landing.mapId && std::hypot(bot.x - landing.x, bot.y - landing.y) <= LANDING_CHECK_YARDS;
}

// The points the check looks at: back from the landing towards the last flight point every LANDING_STEP_YARDS, at
// most LANDING_SEARCH_YARDS, at the landing's height; the farthest first, the landing last
inline std::vector<Point> LandingLine(Trip const& trip)
{
    std::vector<Waypoint> const& points = trip.waypoints;
    if (points.size() < 3)
        return {};
    Point const& landing = points.back().at;
    Point const& before = points[points.size() - 3].at;
    float const gap = std::hypot(before.x - landing.x, before.y - landing.y);
    std::size_t const steps =
        gap > 0.0f ? static_cast<std::size_t>(std::min(gap, LANDING_SEARCH_YARDS) / LANDING_STEP_YARDS) : 0;
    std::vector<Point> line;
    line.reserve(steps + 1);
    for (std::size_t k = steps; k > 0; --k)
    {
        float const share = static_cast<float>(k) * LANDING_STEP_YARDS / gap;
        line.push_back({landing.mapId, landing.x + (before.x - landing.x) * share,
                        landing.y + (before.y - landing.y) * share, landing.z});
    }
    line.push_back(landing);
    return line;
}

// outdoors(point) -> may a bot stand there under the open sky. The landing (the line's last point) when it is
// outdoors or no point is, else the outdoor point nearest to it
template <class Outdoors>
std::size_t LandingIndex(std::vector<Point> const& line, Outdoors const& outdoors)
{
    for (std::size_t k = line.size(); k-- > 0;)
        if (outdoors(line[k]))
            return k;
    return line.empty() ? 0 : line.size() - 1;
}

// Marks the check done. A landing more than SAME_POINT_YARDS from the planned one ends the fly leg there, with the
// descent point moved before it, and a walk leg from there to the planned end follows (the walk in). True when moved
inline bool MoveLanding(Trip& trip, Point const& landing)
{
    trip.landingChecked = true;
    if (trip.legIdx >= trip.legs.size() || trip.waypoints.size() < 3)
        return false;
    Leg& fly = trip.legs[trip.legIdx];
    Point const planned = fly.to;
    if (fly.type != LegType::Fly || landing.mapId != planned.mapId || Distance(landing, planned) <= SAME_POINT_YARDS)
        return false;
    std::size_t const count = trip.waypoints.size();
    Point const before = trip.waypoints[count - 3].at;
    float const gap = std::hypot(before.x - landing.x, before.y - landing.y);
    float const share = gap > 0.0f ? std::min(FLY_CLIMB_YARDS, gap / 2.0f) / gap : 0.0f;
    trip.waypoints[count - 2].at = {landing.mapId, landing.x + (before.x - landing.x) * share,
                                    landing.y + (before.y - landing.y) * share, before.z};
    trip.waypoints[count - 1].at = landing;
    fly.to = landing;
    if (!fly.flightPoints.empty())
        fly.flightPoints.back() = landing;
    Leg walk;
    walk.type = LegType::Walk;
    walk.from = landing;
    walk.to = planned;
    walk.seconds = Distance(landing, planned) / WALK_FLOOR_SPEED;
    trip.legs.insert(trip.legs.begin() + trip.legIdx + 1, std::move(walk));
    return true;
}

// A flight leg's first hop (its first two graph nodes), the edge a failed taxi start is reported on;
// {NO_NODE, NO_NODE} on any other leg
inline std::pair<std::uint32_t, std::uint32_t> FirstFlightHop(Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size())
        return {NO_NODE, NO_NODE};
    Leg const& leg = trip.legs[trip.legIdx];
    if (leg.type != LegType::Flight || leg.nodes.size() < 2)
        return {NO_NODE, NO_NODE};
    return {leg.nodes[0], leg.nodes[1]};
}

inline WorldTravel::SightPoint FailurePoint(Point const& at) { return {at.mapId, at.x, at.y, at.z}; }

// The failed leg or trip as the record of failed attempts keys it (FailedAttemptRules.h): the walk's graph link or its
// two points off the graph, the ride's stops, the portal's or the taxi path's first hop, the fly leg's air link, the
// hearthstone; the trip's goal and the bot's spot. A stuck book cause reaches this only from a bot on the mesh (an
// off-mesh give-up goes to the recovery, which notes its own spot)
inline FailedAttempts::Failure FailureOf(Trip const& trip, Point const& goal, Point const& spot)
{
    FailedAttempts::Failure failure;
    failure.goal = FailurePoint(goal);
    failure.spot = FailurePoint(spot);
    if (trip.legIdx >= trip.legs.size())
        return failure;
    Leg const& leg = trip.legs[trip.legIdx];
    auto const link = [&failure](std::pair<std::uint32_t, std::uint32_t> const& ends, FailedAttempts::Leg kind)
    {
        if (ends.first == NO_NODE || ends.second == NO_NODE)
            return;
        failure.leg = kind;
        failure.fromNode = ends.first;
        failure.toNode = ends.second;
    };
    switch (leg.type)
    {
        case LegType::Walk:
            if (WalksOffGraph(trip))
            {
                failure.leg = FailedAttempts::Leg::OffGraphWalk;
                failure.walkFrom = FailurePoint(trip.waypoints[trip.next - 1].at);
                failure.walkTo = FailurePoint(trip.waypoints[trip.next].at);
            }
            else
                link(StuckEdge(trip), FailedAttempts::Leg::GraphWalk);
            break;
        case LegType::Portal:
        case LegType::StaticPortal:
            link(StuckEdge(trip), FailedAttempts::Leg::Portal);
            break;
        case LegType::Flight:
            link(FirstFlightHop(trip), FailedAttempts::Leg::Taxi);
            break;
        case LegType::Transport:
            if (leg.rideAt + 1 < leg.nodes.size())
                link({leg.nodes[leg.rideAt], leg.nodes[leg.rideAt + 1]}, FailedAttempts::Leg::Ride);
            break;
        case LegType::Fly:
            failure.leg = FailedAttempts::Leg::Fly;
            failure.airLink = leg.air && leg.nodes.size() == 2;
            if (failure.airLink)
            {
                failure.fromNode = leg.nodes.front();
                failure.toNode = leg.nodes.back();
            }
            break;
        case LegType::Hearth:
            failure.leg = FailedAttempts::Leg::Hearth;
            break;
        default:
            break;
    }
    return failure;
}

// A refused order on a transport leg's approach to its dock point or on a taxi leg's walk to its flight master (WT.59;
// MoveFarTo's typed end, NoWalk): beside the leg's own link (FailureOf), the walk from where the bot stood to the
// order's target (Trip::lastTarget) is a walk entry of its own. Empty on any other leg or reason
inline std::optional<FailedAttempts::Failure> RefusedOrderWalk(Trip const& trip, FailReason reason, Point const& spot)
{
    if (reason != FailReason::NoWalk || trip.legIdx >= trip.legs.size())
        return std::nullopt;
    LegType const type = trip.legs[trip.legIdx].type;
    if (type != LegType::Transport && type != LegType::Flight)
        return std::nullopt;
    FailedAttempts::Failure failure;
    failure.leg = FailedAttempts::Leg::OffGraphWalk;
    failure.walkFrom = FailurePoint(spot);
    failure.walkTo = FailurePoint(trip.lastTarget);
    failure.spot = failure.walkFrom;
    return failure;
}

// The whole trip's bound beside the legs' own timeouts: a leg's timeout x MAX_REPLANS can run for hours on a long
// merged walk while the bot counts as busy. Before the first answer the estimate is 0 (the slack only)
inline std::uint32_t TripDeadlineMs(float estimateSeconds)
{
    float const ms = std::min(std::max(estimateSeconds, 0.0f) * WALK_TIMEOUT_FACTOR * 1000.0f, 3.0e9f);
    return static_cast<std::uint32_t>(ms) + TRIP_SLACK_MS;
}

inline bool PastDeadline(std::uint32_t activeMs, float routeSeconds) { return activeMs > TripDeadlineMs(routeSeconds); }

// The trip walks now: its current leg is a walk; past the last leg, an inn errand's steps to the innkeeper
inline bool HasWalkTarget(Trip const& trip, bool bindInn)
{
    return trip.legIdx < trip.legs.size() ? trip.legs[trip.legIdx].type == TravelRoute::LegType::Walk : bindInn;
}

// What the bot stands in or on that moves it, read from the core (NewRpgInternal::PassengerOwnsTravel)
struct Aboard
{
    bool transport{false};  // the core's passenger link or the on-transport movement flag
    bool inFlight{false};   // a taxi
    bool vehicle{false};
};

enum class TravelOwner : std::uint8_t
{
    Follower,  // the travel follower moves the bot: its walk budget, off-mesh probe and route clock apply
    Passenger  // the passenger code, a taxi or a vehicle moves it; the follower only steps the leg's own expiry
};

// Who moves the bot on its trip, the one answer for the progress guard's owner and the follower's walk-failure and
// loot gates: a passenger move runs, the bot is getting off a ship first, the current leg is a transport leg, or it is
// aboard, in flight or in a vehicle. Whose boarding a ride is (WT62's BoardedByIntent, StepTravel's ownsBoard) is
// another question
inline TravelOwner OwnerOf(Trip const& trip, bool passengerMoves, Aboard const& aboard)
{
    bool const transportLeg =
        trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport;
    bool const passenger = passengerMoves || trip.offFirst.sinceMs || transportLeg || aboard.transport ||
                           aboard.inFlight || aboard.vehicle;
    return passenger ? TravelOwner::Passenger : TravelOwner::Follower;
}
}  // namespace TravelLegs

#endif
