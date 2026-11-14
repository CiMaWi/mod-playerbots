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
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "RouteRules.h"
#include "TravelLifecycle.h"
#include "TravelWalkRules.h"

namespace TravelLegs
{
using TravelRoute::Distance;
using TravelRoute::Leg;
using TravelRoute::LegType;
using TravelRoute::Point;

constexpr std::uint32_t NO_NODE = TravelGraph::NO_NODE;
static_assert(TravelWalk::NO_NODE == NO_NODE, "TravelWalk shares the graph's NO_NODE");
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
constexpr std::size_t AVOID_NODES = 4;                // route ends a trip leaves out after stuck walks
constexpr std::size_t AVOID_EDGES = 4;                // graph edges a trip leaves out (deaths, timed-out transports)
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

// Transport legs (WT19): ships, zeppelins, elevators and the Deeprun Tram
constexpr std::uint32_t TRANSPORT_SCAN_MS = 1000;  // the follower looks for the leg's transport this often
constexpr float SHIP_DOCK_YARDS = 80.0f;           // a stopped ship's or zeppelin's centre beside its dock node (2D)
constexpr float STATIC_DOCK_YARDS = 20.0f;         // an elevator's or the tram's centre beside its stop node (2D)
constexpr float STATIC_DOCK_Z_YARDS = 5.0f;        // and level with the stop node's floor
constexpr float STILL_YARDS = 0.5f;                // a static transport that moved less between two scans stands
constexpr float WAIT_RETURN_YARDS = 15.0f;         // waiting further from the dock node, the bot walks back to it
constexpr float QUAY_REACH_YARDS = 1.0f;
// PlayerbotAI adds a passenger once a second: a departure without the bot counts as a miss after this
constexpr std::uint32_t MISS_GRACE_MS = 2500;
// Riding, one scan may miss the passenger (a cross-map ship's far teleport, that once-a-second check): the bot is off
// the transport only when this many scans in a row saw it off
constexpr std::uint8_t HOP_AFTER_MISSES = 3;  // misses before the one last-resort hop onto the deck (WT.14)
constexpr std::uint32_t HOP_TRY_MS = 3000;    // the boarding walk's time at a docking before the hop
constexpr float HOP_MAX_YARDS = 20.0f;  // the hop only crosses the gap: the bot this close to the transport's centre
constexpr std::uint32_t WAIT_PERIODS = HOP_AFTER_MISSES + 2;  // periods waited off the transport at most
constexpr std::uint32_t RIDE_PERIODS = 2;                     // periods aboard at most (a missed exit rides round)
// A transport leg's seconds are half a period's wait plus the ride: about a period
constexpr float PERIOD_FACTOR = 2.0f;
constexpr std::uint32_t MIN_PERIOD_MS = 30 * 1000;  // the period assumed before a scan read it is at least this

enum class Act : std::uint8_t
{
    Walk,              // MoveFarTo(target) on the bot's map
    UseTrigger,        // step into the portal leg's area trigger
    UseObject,         // use the static portal leg's object at its node, as a player clicks it
    TalkFlightMaster,  // at the flight master of the leg's start: learn the node, pay, take off
    UseHearth,         // the hearthstone leg: cast it (wait for the cooldown and the cast)
    Ride,              // in flight (a cross-map flight continues)
    Mount,             // a fly leg: mount a mount that flies here, where the bot stands
    Fly,               // a fly leg: MoveTo(target) with its z on the flying mount (no path generation in the air)
    Board,             // transport: a straight walk (decks are off the mesh) onto the docked transport at target
    Hop,               // transport: the last-resort hop onto the deck near target (unseen, counted: WT.14)
    Disembark,         // transport: a straight walk off the transport, docked at the leg's end, to the nearest ground
    Descend,           // a failed fly leg, in the air: MoveTo(target) down to the floor on the flying mount
    Fall,              // a failed fly leg, in the air without a flying mount: it falls to target (CompleteDismount)
    Wait,              // nothing to do this tick (a sent trigger or an activated taxi takes effect)
    Done,              // the leg ended (Advance starts the next one)
    Arrived,           // the last leg ended
    Fail
};

enum class FailReason : std::uint8_t
{
    None,
    Timeout,      // the leg took longer than its timeout
    WrongMap,     // the bot is on a map the leg does not run on
    Stuck,        // MoveFarTo gave the waypoint up
    Portal,       // the area trigger or the portal object did not take the bot through
    Fare,         // the purse does not pay the flight
    Landing,      // the flight ended away from the leg's end
    Unsupported,  // a leg type the follower does not run (none since WT19)
    NoAnswer,     // the route service did not answer in time, or the answer was for another map or too old
    NoRoute,      // the search found no route
    Taxi,         // no flight master at the leg's start, the path is not discovered, or the core refused the taxi
    Hearth,       // the hearthstone is missing or its cast failed
    OffMesh,      // the bot stands off the walkable mesh (MoveFarTo's off-mesh check): no walk can start there
    Death,        // the bot died LEG_DEATHS times on the leg or TRIP_DEATHS times on the trip (NoteLife)
    TransportLost,
    NoExit,   // no exit at the leg's end: the bot walked its boarding crossing back where it boarded (Boarding::Back)
    Takeoff,  // a fly leg: no takeoff where the bot stands, or no flying mount after FLY_MOUNT_TRIES tries
    NoPath    // a walk along a graph link: the engine has no path at its far node (TravelWalk, WT58): refused
};

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
        case Act::Hop:
            return "hop";
        case Act::Disembark:
            return "disembark";
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
    }
    return "?";
}

// Travel event sources (EventLog::Emit(bot, "Travel", source, ...); literals, as the log keeps the pointer's text):
// "start", "arrived", "leg_done:<leg type>", "leg_failed:<reason>", "abort:<reason>" and the aborts that are no failed
// leg: "abort:deadline", "abort:stalled", "abort:no_service", "abort:stopped", "abort:sick"
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
    }
    return "abort:?";
}

// Transport event sources (EventLog::Emit(bot, TRANSPORT_EVENT_TYPE, TransportSource(event), entry, amount)), id = the
// transport's gameobject entry. Amounts: attempt = seconds of the walk to the dock node, boarded = seconds waited
// there, arrived, leave and back = seconds aboard, missed and hop = the leg's misses so far, stuck_aboard = seconds
// since the leave (or the off_first), no_exit = 0, off_first = the bound's seconds (the transport's period). The hop
// also gets EventLog::Honesty(bot, HOP_HONESTY_KIND, HOP_HONESTY_SOURCE, entry, misses) for `wlog report honesty`
enum class TransportEvent : std::uint8_t
{
    Attempt,  // the bot reached the dock node
    Boarded,
    Arrived,  // off the transport at the leg's end
    Missed,   // the transport left the dock without the bot
    Hop,
    Leave,        // aboard, the transport stands at the leg's end: the bot walks off
    StuckAboard,  // the transport left the leg's end with the bot still aboard after a leave
    NoExit,       // the follower found no ground beside the deck to walk off to (once per leave)
    OffFirst,     // a pending trip operation waits until the bot is ashore
    Lost,         // verified membership loss without a safe exit
    Back          // after a no_exit, off the transport where it boarded, by its boarding crossing walked back
};

constexpr char const* TRANSPORT_EVENT_TYPE = "Transport";
constexpr char const* HOP_HONESTY_KIND = "Teleport";
constexpr char const* HOP_HONESTY_SOURCE = "transport_hop";

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
        case TransportEvent::Hop:
            return "hop";
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
    }
    return "?";
}

// A Transport event for the follower to send (Trip::transportNotes)
struct TransportNote
{
    TransportEvent event{TransportEvent::Attempt};
    std::uint32_t entry{0};  // the transport's gameobject entry (Leg::object)
    std::uint32_t amount{0};
};

struct Waypoint
{
    Point at;
    std::uint32_t node{NO_NODE};  // the graph node at this point; NO_NODE for the bot's start or the goal
};

// A transport leg's transport as the follower's scan saw it (every TRANSPORT_SCAN_MS, on the bot's map only): the one
// the bot rides, else the nearest of the leg's entry to the leg's start
struct TransportSight
{
    bool scanned{false};        // this observation carries a scan; between scans the trip keeps the last one
    bool found{false};          // a transport of the leg's entry is on the bot's map
    bool moving{false};         // a ship or zeppelin under way (MotionTransport::IsMoving); false for static ones
    bool isStatic{false};       // an elevator or the tram (IsStaticTransport): its stops are judged by its position
    bool unseen{false};         // no real player sees a hop onto it (IsTeleportSeen); filled only when WantsSightCheck
    std::uint32_t periodMs{0};  // its period; 0 unknown
    Point at;                   // its position: the target of Board and Hop
    std::uint64_t guid{0};
};

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
    bool boardingReady{true};
    bool boardMotion{false};  // the bot's own boarding move onto the transport it is aboard has not reported yet
    std::optional<TravelLifecycle::Outcome> passengerOutcome;
    std::uint32_t quayLeg{NO_NODE};
    std::optional<std::uint32_t> activeLegMs;
};

struct Decision
{
    Act act{Act::Wait};
    // Walk, Fly: where to; UseTrigger, TalkFlightMaster, Mount: the leg's start; Ride, Disembark: the leg's end (the
    // follower picks the ground beside the deck); Board, Hop: the transport's position (the follower picks the deck
    // point nearest the bot)
    Point target;
    FailReason reason{FailReason::None};
};

// What the current leg saw of the bot: follower ticks mounted, time in combat and deaths. The follower counts the
// ticks; NoteLife counts combat (PlayerbotAI's engine changes) and deaths (CensusMgr's death hooks), because the
// follower runs in neither the combat nor the dead engine
struct LegRecord
{
    std::uint32_t ticks{0};
    std::uint32_t mountedTicks{0};
    std::uint32_t combatMs{0};       // ended combat stretches
    std::uint32_t combatSinceMs{0};  // the running stretch's start; 0 = not in combat
    std::uint8_t deaths{0};
};

// A transport leg's states: walk to the dock node, wait there, board, ride and leave at the destination; after a
// no_exit there, walk back off where the bot boarded (Back)
enum class Boarding : std::uint8_t
{
    Approach,
    Wait,
    Board,
    Ride,
    Leave,
    Back
};

// WT43b: the boarding's crossing (deck point and shore) in the transport's own frame: the offset from its origin,
// turned by -o (z unturned). A ship or zeppelin repeats its pose at a stop, so where it stands as it did at the
// boarding the same crossing, walked in reverse, gets the bot off
constexpr float BACK_STOP_YARDS = 3.0f;  // the stored shore, placed by the current pose, lies this near its old place

inline TravelLifecycle::Point LocalOffset(TravelLifecycle::DockPose const& pose, TravelLifecycle::Point const& world)
{
    float const dx = world.X - pose.Position.X;
    float const dy = world.Y - pose.Position.Y;
    float const c = std::cos(pose.Orientation);
    float const s = std::sin(pose.Orientation);
    return {dx * c + dy * s, dy * c - dx * s, world.Z - pose.Position.Z};
}

inline TravelLifecycle::Point WorldPoint(TravelLifecycle::DockPose const& pose, TravelLifecycle::Point const& local)
{
    float const c = std::cos(pose.Orientation);
    float const s = std::sin(pose.Orientation);
    return {pose.Position.X + local.X * c - local.Y * s, pose.Position.Y + local.X * s + local.Y * c,
            pose.Position.Z + local.Z};
}

struct BoardCrossing
{
    std::uint64_t guid{0};  // the transport boarded; 0 none
    TravelLifecycle::Point deck;
    TravelLifecycle::Point shore;
    TravelLifecycle::Point shoreWorld;  // where the bot stood ashore
};

inline BoardCrossing StoreCrossing(std::uint64_t guid, TravelLifecycle::DockPose const& pose,
                                   TravelLifecycle::Point const& deck, TravelLifecycle::Point const& shore)
{
    return {guid, LocalOffset(pose, deck), LocalOffset(pose, shore), shore};
}

struct CrossingWalk
{
    TravelLifecycle::Point deck;
    TravelLifecycle::Point shore;
};

// The stored crossing in world coordinates, only aboard the same transport standing where the bot boarded it
inline std::optional<CrossingWalk> CrossingBack(BoardCrossing const& crossing, std::uint64_t guid,
                                                TravelLifecycle::DockPose const& pose)
{
    if (!crossing.guid || crossing.guid != guid)
        return std::nullopt;
    TravelLifecycle::Point const shore = WorldPoint(pose, crossing.shore);
    if (TravelLifecycle::DistanceSquared(shore, crossing.shoreWorld) > BACK_STOP_YARDS * BACK_STOP_YARDS)
        return std::nullopt;
    return CrossingWalk{WorldPoint(pose, crossing.deck), shore};
}

// What waits for the bot to get off a ship or zeppelin (OffFirst): a walk or a route request (none), the trip's
// arrival, or its abort
enum class OffFirstEnd : std::uint8_t
{
    None,
    Arrive,
    Abort
};

// WT19-d: a trip that would end, ask for a route or walk while the bot still rides a ship or zeppelin first gets the
// bot off: it walks off once the transport stands, within one period (BeginOffFirst, StepOffFirst)
struct OffFirst
{
    std::uint32_t sinceMs{0};  // 0: none under way
    std::uint32_t entry{0};    // the ship's gameobject entry
    std::uint64_t guid{0};
    std::uint32_t boundMs{0};
    OffFirstEnd end{OffFirstEnd::None};
    char const* why{nullptr};  // Abort: EndTrip's words (string literals)
    char const* source{nullptr};
    bool gaveUp{false};  // a period passed aboard: no second wait on this trip
};

// A route's legs and the follower's place on them
struct Trip
{
    std::vector<Leg> legs;
    std::uint32_t legIdx{0};
    std::uint32_t legStartMs{0};
    std::vector<Waypoint> waypoints;  // the current leg's: [0] its start; the bot heads for waypoints[next]
    std::uint32_t next{1};
    // walk: the graph link between waypoints[next - 1] and waypoints[next] as walked (TravelWalk; StartLeg resets it)
    TravelWalk::LinkWalk walk;
    bool approached{false};     // portal: the bot reached the trigger's (object's) node; then it heads for that
    bool triggered{false};      // portal: the area trigger was sent (static portal: the object was used)
    bool taxiActivated{false};  // flight: the taxi was taken (fare paid)
    bool seenInFlight{false};   // flight: the bot was seen in flight; fly: in the air
    // fly: in combat after the takeoff (NoteLife); coming down away from the end then is a combat landing, not a fall
    bool foughtInFlight{false};
    // fly: the waypoint the real-height check looked at last (NeedsHeightCheck), its raises, the highest z flown, the
    // real mount tries and the last one's time (NoteMountTry), and why the last try did not start a cast ("": it did;
    // the glue's word for the fly_leg_failed row)
    std::uint32_t heightChecked{0};
    std::uint16_t climbs{0};
    float peakZ{0.0f};
    std::uint8_t mountTries{0};
    std::uint32_t mountTriedMs{0};
    char const* mountWhy{""};
    // fly: the landing spot was checked for the open sky (NeedsLandingCheck, MoveLanding)
    bool landingChecked{false};
    bool noFly{false};  // after a failed fly leg: later routes of the trip fly no more (Request::noFly)
    std::uint8_t replans{0};
    std::uint8_t spotFails{0};         // takeoffs refused where the bot stood that replanned flying (AfterFailure)
    std::uint8_t resubmits{0};         // route requests asked again after a late or lost answer (Resubmit)
    std::vector<std::uint32_t> avoid;  // after stuck walks: nodes later routes neither start nor end at (NoteStuck)
    bool noDirect{false};              // after a stuck walk towards the goal: no straight walk from the bot to it
    bool stuckSeen{false};
    Point stuckAt;           // the waypoint of the trip's last stuck walk
    LegRecord record;        // the current leg's (StartLeg starts it anew)
    std::uint8_t deaths{0};  // on the whole trip
    // After deaths on a walk (NoteDeath) or a timed-out transport leg (NoteTransportTimeout): graph edges later
    // routes do not use (this bot's trip only)
    std::vector<std::pair<std::uint32_t, std::uint32_t>> avoidEdges;
    // Transport legs (StartLeg resets them; the notes stay until the follower sends and clears them)
    Boarding boarding{Boarding::Approach};
    TransportSight sight;          // the last scan
    bool still{false};             // the last scan saw the transport standing
    std::uint32_t periodMs{0};     // the transport's period once a scan read it
    std::uint32_t stateMs{0};      // when boarding entered its state
    std::uint32_t waitSinceMs{0};  // when the bot reached the dock node (the leg's start until then)
    std::uint32_t boardedMs{0};
    bool departed{false};  // Board: the transport left without the bot, at departedMs
    std::uint32_t departedMs{0};
    std::uint8_t misses{0};
    bool hopped{false};
    std::uint64_t passengerGuid{0};  // confirmed boarding for this leg
    bool exitSought{false};          // this docking already reported no safe exit
    bool exitless{false};            // WT43b: an exit search of this leg found nothing (no_exit): go Back
    BoardCrossing boardCrossing;     // the issued boarding walk (StoreCrossing); none after a hop
    // WT19-e: a ship's or zeppelin's dock node is its own stop (over the water or in the air), so the bot walks to and
    // A motion transport waits on the physical quay. The follower looks once per observed docking.
    Point quay;
    bool hasQuay{false};
    bool quaySought{false};
    std::vector<TransportNote> transportNotes;
    OffFirst offFirst;  // the whole trip's (StartLeg leaves it)
    // After a fly leg failed in the air (StartDescent): the bot flies down to descentTo before the replan
    bool descending{false};
    Point descentTo;
    std::uint32_t descentStartMs{0};
    // The trip is aborting (AfterFailure) and ends once the descent is over (AbortDescent, EndsAfterDescent), why
    bool endAfterDescent{false};
    FailReason endReason{FailReason::None};
    // The follower's last decision and its time (the glue notes them for the case's hold line)
    Act lastAct{Act::Wait};
    Point lastTarget;
    std::uint32_t lastActMs{0};
};

// The route request the follower waits for (the bot's latest; RouteService answers older ones too)
struct RouteWait
{
    std::uint32_t requestId{0};
    std::uint32_t mapId{0};
    std::uint32_t requestedMs{0};
    bool waiting{false};
    std::optional<Point> start{};
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

// No answer within its life: the service has dropped it (or the request)
inline bool WaitExpired(RouteWait const& wait, std::uint32_t nowMs)
{
    return nowMs - wait.requestedMs > TravelRoute::ANSWER_KEEP_MS;
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

// A transport's period before a scan read it (MIN_PERIOD_MS at least)
inline std::uint32_t FallbackPeriodMs(Leg const& leg)
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
        case LegType::Transport:
            // Off the transport before a scan read its period (StepTransport); aboard RIDE_PERIODS from the boarding
            return LEG_SLACK_MS + WAIT_PERIODS * FallbackPeriodMs(leg);
        default:
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

// Killed too often: LEG_DEATHS on the leg or TRIP_DEATHS on the trip fail the leg as FailReason::Death
inline bool DiedTooOften(Trip const& trip) { return trip.record.deaths >= LEG_DEATHS || trip.deaths >= TRIP_DEATHS; }

// Starts leg index (no-op walks skipped); index past the last leg: the trip has arrived
template <class NodeAt>
void StartLeg(Trip& trip, std::uint32_t index, std::uint32_t nowMs, NodeAt const& nodeAt)
{
    while (index < trip.legs.size() && IsNoOp(trip.legs[index]))
        ++index;
    bool const keepQuay = trip.hasQuay && index == trip.legIdx + 1 && index < trip.legs.size() &&
                          trip.legs[trip.legIdx].type == LegType::Walk && trip.legs[index].type == LegType::Transport;
    trip.legIdx = index;
    trip.legStartMs = nowMs;
    trip.next = 1;
    trip.walk = {};
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
    trip.still = false;
    trip.periodMs = 0;
    trip.stateMs = nowMs;
    trip.waitSinceMs = nowMs;
    trip.boardedMs = nowMs;
    trip.departed = false;
    trip.departedMs = 0;
    trip.misses = 0;
    trip.hopped = false;
    trip.passengerGuid = 0;
    trip.exitSought = false;
    trip.exitless = false;
    trip.boardCrossing = {};
    trip.hasQuay = keepQuay;
    trip.quaySought = keepQuay;
    trip.record = NextRecord(trip.record, nowMs);
    trip.waypoints.clear();
    if (index < trip.legs.size())
        trip.waypoints = Waypoints(trip.legs[index], nodeAt);
}

// A scan's verdict: a ship or zeppelin stands when it is not under way, a static transport when it has not moved since
// the scan before (an elevator passes the floors without stopping)
inline void NoteSight(Trip& trip, TransportSight const& sight)
{
    bool const before = trip.sight.found && trip.sight.guid == sight.guid && trip.sight.at.mapId == sight.at.mapId;
    trip.still = sight.found && !sight.moving &&
                 (!sight.isStatic || (before && Distance(trip.sight.at, sight.at) <= STILL_YARDS));
    if (sight.periodMs)
        trip.periodMs = sight.periodMs;
    trip.sight = sight;
}

// The leg's transport stands at a dock node: a ship within SHIP_DOCK_YARDS of it, a static one beside it at its floor
// A transport leg's stops, where its vessel docks: from and to are the wait points where the graph has them (WT55b)
inline Point const& StopFrom(Leg const& leg) { return leg.waitFrom ? leg.stopFrom : leg.from; }
inline Point const& StopTo(Leg const& leg) { return leg.waitTo ? leg.stopTo : leg.to; }

inline bool DockedAt(Trip const& trip, Point const& dock)
{
    TransportSight const& sight = trip.sight;
    if (!trip.still || sight.at.mapId != dock.mapId)
        return false;
    float const dx = sight.at.x - dock.x;
    float const dy = sight.at.y - dock.y;
    float const flat = std::sqrt(dx * dx + dy * dy);
    if (sight.isStatic)
        return flat <= STATIC_DOCK_YARDS && std::fabs(sight.at.z - dock.z) <= STATIC_DOCK_Z_YARDS;
    return flat <= SHIP_DOCK_YARDS && std::fabs(sight.at.z - dock.z) <= STATIC_DOCK_Z_YARDS;
}

// The leg's vessel took the bot aboard at the leg's start before it boarded (WT54): its deck docked under the bot
// waiting beside the stop and the passenger rule attached it. That is the boarding; no crossing was needed. Only on
// the current transport leg before its ride, the leg's entry, and within a docked hull's reach of the start node
inline bool TakenAboard(Trip const& trip, Point const& bot, std::uint32_t aboard)
{
    if (trip.legIdx >= trip.legs.size() || trip.passengerGuid || !aboard)
        return false;
    Leg const& leg = trip.legs[trip.legIdx];
    Point const& stop = StopFrom(leg);
    if (leg.type != LegType::Transport || aboard != leg.object || bot.mapId != stop.mapId)
        return false;
    float const dx = bot.x - stop.x;
    float const dy = bot.y - stop.y;
    return std::sqrt(dx * dx + dy * dy) <= (trip.sight.isStatic ? STATIC_DOCK_YARDS : SHIP_DOCK_YARDS);
}

// The walk to a ship's or zeppelin's leg ended aboard (WT54-6, the WT47 class "aboard before boarding"): the leg's
// vessel, docked over the walk's last yards, took the bot aboard before the walk reached its end. The walk is done and
// the transport leg's TakenAboard counts the boarding. Only for the walk right before that leg, its entry, and within
// a docked hull's reach of its start; the caller leaves elevators and the tram out (their stops share x and y)
inline bool WalkTakenAboard(Trip const& trip, Point const& bot, std::uint32_t aboard)
{
    if (!aboard || trip.legIdx + 1 >= trip.legs.size() || trip.legs[trip.legIdx].type != LegType::Walk)
        return false;
    Leg const& leg = trip.legs[trip.legIdx + 1];
    Point const& stop = StopFrom(leg);
    if (leg.type != LegType::Transport || aboard != leg.object || bot.mapId != stop.mapId)
        return false;
    float const dx = bot.x - stop.x;
    float const dy = bot.y - stop.y;
    return std::sqrt(dx * dx + dy * dy) <= SHIP_DOCK_YARDS;
}

inline std::uint32_t TransportPeriodMs(Trip const& trip, Leg const& leg)
{
    return trip.periodMs ? trip.periodMs : FallbackPeriodMs(leg);
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

inline bool TransportWaitExpired(Trip const& trip, Leg const& leg, std::uint32_t nowMs)
{
    return nowMs - trip.legStartMs > TransportWaitMs(trip, leg);
}

inline bool TransportRideExpired(Trip const& trip, Leg const& leg, std::uint32_t nowMs)
{
    return nowMs - trip.boardedMs > TransportRideMs(trip, leg);
}

inline bool OffFirstExpired(OffFirst const& off, std::uint32_t nowMs)
{
    return off.sinceMs && nowMs - off.sinceMs > off.boundMs;
}

// The follower's look for the quay beside the current transport leg's dock node: found, or none (the node stays)
inline void SetQuay(Trip& trip, Point const* quay)
{
    // A moving approach changes the floor and corridor evidence; a failed search is not a dock-wide result.
    trip.quaySought = quay || trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != LegType::Walk;
    trip.hasQuay = quay != nullptr;
    if (quay)
        trip.quay = *quay;
}

// Where a transport leg's bot walks to and waits: the wait point or the quay once found, else the dock node
inline Point const& WaitPoint(Trip const& trip, Leg const& leg) { return trip.hasQuay ? trip.quay : leg.from; }

// The hop may come at this docking: the follower then asks whether a real player sees it (TransportSight::unseen)
inline bool WantsSightCheck(Trip const& trip)
{
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == LegType::Transport &&
           trip.boarding == Boarding::Board && trip.misses >= HOP_AFTER_MISSES && !trip.hopped;
}

// A transport leg (WT19): walk to the waiting point (WaitPoint), wait, board the docked transport, ride, walk off
// where it stands at the leg's end. A departure without the bot is a miss and the bot waits for the next one; after
// HOP_AFTER_MISSES misses one hop onto the deck, only unseen. The observer confirms both passenger frame changes.
inline Decision StepTransport(Trip& trip, Observation const& seen)
{
    Leg const& leg = trip.legs[trip.legIdx];
    std::uint32_t const nowMs = seen.nowMs;
    auto const fail = [](FailReason reason) { return Decision{Act::Fail, {}, reason}; };
    auto const within = [&seen](Point const& at, float yards)
    { return seen.bot.mapId == at.mapId && Distance(seen.bot, at) <= yards; };
    auto const enter = [&trip, nowMs](Boarding state)
    {
        trip.boarding = state;
        trip.stateMs = nowMs;
    };
    auto const note = [&trip, &leg](TransportEvent event, std::uint32_t amount)
    { trip.transportNotes.push_back({event, leg.object, amount}); };
    auto const seconds = [nowMs](std::uint32_t sinceMs) { return (nowMs - sinceMs) / 1000; };
    using TravelLifecycle::Outcome;
    if (!seen.membershipAvailable)
        return {Act::Wait, leg.to, FailReason::None};
    if (seen.transport.scanned)
        NoteSight(trip, seen.transport);
    if (trip.boarding == Boarding::Board && seen.passengerOutcome == Outcome::Boarded && seen.aboardGuid &&
        seen.aboard == leg.object)
    {
        trip.passengerGuid = seen.aboardGuid;
        note(TransportEvent::Boarded, seconds(trip.waitSinceMs));
        trip.boardedMs = nowMs;
        enter(Boarding::Ride);
    }
    else if (seen.aboardGuid && !seen.boardMotion && TakenAboard(trip, seen.bot, seen.aboard))
    {
        trip.passengerGuid = seen.aboardGuid;
        note(TransportEvent::Boarded, seconds(trip.waitSinceMs ? trip.waitSinceMs : trip.legStartMs));
        trip.boardedMs = nowMs;
        enter(Boarding::Ride);
    }
    if (trip.passengerGuid)
    {
        if (seen.aboardGuid != trip.passengerGuid)
        {
            if (!seen.aboardGuid && trip.boarding == Boarding::Leave && seen.passengerOutcome == Outcome::Exited)
            {
                note(TransportEvent::Arrived, seconds(trip.boardedMs));
                return {Act::Done, leg.to, FailReason::None};
            }
            // Back ashore where it boarded: the leg failed, the replan leaves this link out (AfterFailure)
            if (!seen.aboardGuid && trip.boarding == Boarding::Back && seen.passengerOutcome == Outcome::Exited)
            {
                note(TransportEvent::Back, seconds(trip.boardedMs));
                return fail(FailReason::NoExit);
            }
            note(TransportEvent::Lost, seconds(trip.boardedMs));
            return fail(FailReason::TransportLost);
        }
        if (TransportRideExpired(trip, leg, nowMs))
            return fail(FailReason::Timeout);
        if (seen.bot.mapId != leg.from.mapId && seen.bot.mapId != leg.to.mapId)
            return fail(FailReason::WrongMap);
        if (DockedAt(trip, StopTo(leg)))
        {
            if (trip.boarding != Boarding::Leave)
            {
                note(TransportEvent::Leave, seconds(trip.boardedMs));
                trip.exitSought = false;
                enter(Boarding::Leave);
            }
            return {Act::Disembark, leg.to, FailReason::None};
        }
        // WT43b: no exit at the end: off where it boarded, towards the quay it waited on
        if (trip.exitless && DockedAt(trip, StopFrom(leg)))
        {
            if (trip.boarding != Boarding::Back)
            {
                trip.exitSought = false;
                enter(Boarding::Back);
            }
            return {Act::Disembark, trip.hasQuay ? trip.quay : leg.from, FailReason::None};
        }
        // It left the end (or the start, after no exit) again with the bot: the round trip brings it back
        if (trip.boarding == Boarding::Leave || trip.boarding == Boarding::Back)
            note(TransportEvent::StuckAboard, seconds(trip.stateMs));
        if (trip.boarding != Boarding::Ride)
            enter(Boarding::Ride);
        return {Act::Wait, leg.to, FailReason::None};
    }
    if (seen.aboardGuid)
        return {Act::Wait, leg.to, FailReason::None};

    if (TransportWaitExpired(trip, leg, nowMs))
        return fail(FailReason::Timeout);
    if (seen.bot.mapId != leg.from.mapId)
        return fail(FailReason::WrongMap);
    // A stop with a wait point (WT.42): the bot waits there and crosses from there; no quay is looked for
    if (leg.waitFrom && !trip.hasQuay)
        SetQuay(trip, &leg.from);
    Point const& waitAt = WaitPoint(trip, leg);
    if (trip.boarding == Boarding::Approach)
    {
        if (seen.quayLeg == trip.legIdx && !trip.hasQuay)
            return {Act::Wait, seen.bot, FailReason::None};
        if (!within(waitAt, trip.hasQuay ? QUAY_REACH_YARDS : WAYPOINT_YARDS))
            return {Act::Walk, waitAt, FailReason::None};
        // At the dock node before the follower looked for the quay (the walk leg before ended there and this leg
        // began in the same tick): no wait there yet, the look comes on a later tick
        if (!trip.quaySought)
            return {Act::Wait, waitAt, FailReason::None};
        note(TransportEvent::Attempt, seconds(trip.legStartMs));
        trip.waitSinceMs = nowMs;
        enter(Boarding::Wait);
    }
    Decision const wait{within(waitAt, WAIT_RETURN_YARDS) ? Act::Wait : Act::Walk, waitAt, FailReason::None};
    bool const docked = DockedAt(trip, StopFrom(leg));
    if (trip.boarding == Boarding::Wait)
    {
        if (!docked)
            return wait;
        if (trip.hasQuay && !within(waitAt, QUAY_REACH_YARDS))
            return {Act::Walk, waitAt, FailReason::None};
        trip.departed = false;
        enter(Boarding::Board);
    }
    if (docked)
    {
        trip.departed = false;
        if (seen.boardingReady && seen.transport.scanned && seen.transport.unseen && trip.misses >= HOP_AFTER_MISSES &&
            !trip.hopped && nowMs - trip.stateMs >= HOP_TRY_MS && within(trip.sight.at, HOP_MAX_YARDS))
        {
            trip.hopped = true;
            note(TransportEvent::Hop, trip.misses);
            return {Act::Hop, trip.sight.at, FailReason::None};
        }
        return {Act::Board, trip.sight.at, FailReason::None};
    }
    if (!trip.departed)
    {
        trip.departed = true;
        trip.departedMs = nowMs;
    }
    if (nowMs - trip.departedMs < MISS_GRACE_MS)
        return {Act::Wait, waitAt, FailReason::None};
    trip.misses = static_cast<std::uint8_t>(std::min(trip.misses + 1, 255));
    note(TransportEvent::Missed, trip.misses);
    enter(Boarding::Wait);
    return wait;
}

// The current leg's next step. Done, Fail and the others; never Arrived
inline Decision StepLeg(Trip& trip, Observation const& seen)
{
    Leg const& leg = trip.legs[trip.legIdx];
    bool const lastLeg = trip.legIdx + 1 == trip.legs.size();
    std::uint32_t const legMs = seen.activeLegMs.value_or(seen.nowMs - trip.legStartMs);
    auto const fail = [](FailReason reason) { return Decision{Act::Fail, {}, reason}; };
    auto const within = [&seen](Point const& at, float yards)
    { return seen.bot.mapId == at.mapId && Distance(seen.bot, at) <= yards; };

    switch (leg.type)
    {
        case LegType::Walk:
        {
            if (seen.bot.mapId != leg.to.mapId)
                return fail(FailReason::WrongMap);
            if (WalkTakenAboard(trip, seen.bot, seen.aboard))
            {
                trip.next = static_cast<std::uint32_t>(trip.waypoints.size());
                return {Act::Done, leg.to, FailReason::None};
            }
            bool const leadsToQuay = seen.quayLeg == trip.legIdx + 1;
            std::uint32_t const count = static_cast<std::uint32_t>(trip.waypoints.size());
            float const waypointYards = leadsToQuay && trip.hasQuay ? QUAY_REACH_YARDS : WAYPOINT_YARDS;
            float const finalYards = lastLeg ? GOAL_YARDS : waypointYards;
            auto const reached = [&](std::uint32_t index)
            { return within(trip.waypoints[index].at, index + 1 == count ? finalYards : WAYPOINT_YARDS); };
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
            if (legMs > LegTimeoutMs(leg))
                return fail(FailReason::Timeout);
            return {Act::Walk, trip.waypoints[trip.next].at, FailReason::None};
        }
        case LegType::Portal:
        case LegType::StaticPortal:
        {
            bool const exited =
                leg.from.mapId != leg.to.mapId ? seen.bot.mapId == leg.to.mapId : within(leg.to, PORTAL_EXIT_YARDS);
            if (exited)
                return {Act::Done, leg.to, FailReason::None};
            if (legMs > PORTAL_TIMEOUT_MS)
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
            if (legMs > LegTimeoutMs(leg))
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
            if (legMs > LegTimeoutMs(leg))
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
            if (legMs > LegTimeoutMs(leg))
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
                          char const* source = nullptr)
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
    // The scans start anew: the first one counts as under way
    trip.sight = {};
    trip.still = false;
    trip.exitSought = false;
    trip.transportNotes.push_back({TransportEvent::OffFirst, shipEntry, off.boundMs / 1000});
    return true;
}

enum class OffStep : std::uint8_t
{
    Wait,
    Disembark,
    Off,
    Lost,
    GiveUp
};

// One tick of the get-off (seen.aboard, seen.transport: the ridden ship's scan, once a TRANSPORT_SCAN_MS). Off and
// GiveUp end it (sinceMs 0; off.end and its words stay for the follower)
inline OffStep StepOffFirst(Trip& trip, Observation const& seen)
{
    OffFirst& off = trip.offFirst;
    if (seen.transport.scanned)
        NoteSight(trip, seen.transport);
    if (!seen.membershipAvailable)
        return OffStep::Wait;
    if (seen.aboardGuid != off.guid)
    {
        off.sinceMs = 0;
        if (!seen.aboardGuid && seen.passengerOutcome == TravelLifecycle::Outcome::Exited)
            return OffStep::Off;
        off.gaveUp = true;
        trip.transportNotes.push_back({TransportEvent::Lost, off.entry, 0});
        return OffStep::Lost;
    }
    if (OffFirstExpired(off, seen.nowMs))
    {
        trip.transportNotes.push_back({TransportEvent::StuckAboard, off.entry, (seen.nowMs - off.sinceMs) / 1000});
        off.sinceMs = 0;
        off.gaveUp = true;
        return OffStep::GiveUp;
    }
    return trip.still ? OffStep::Disembark : OffStep::Wait;
}

// The trip's next step: finished legs are passed (at most LEGS_PER_TICK), then the current leg's step; Arrived after
// the last leg
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
        if (seen.activeLegMs)
            seen.activeLegMs = 0;
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

// What a stuck walk teaches the trip's later routes; the graph edge (StuckEdge) is flagged for every bot, but a start
// or goal hop is no graph edge. Heading for a graph node: no route starts or ends there (a walk to or from it would
// take the same way); heading for the goal: no straight walk from the bot, and no route ends at the node the bot came
// from. True when the bot got stuck at this waypoint before in this trip
inline bool NoteStuck(Trip& trip)
{
    if (trip.legIdx >= trip.legs.size() || trip.waypoints.empty())
        return false;
    LegType const type = trip.legs[trip.legIdx].type;
    if (type == LegType::Hearth || type == LegType::Fly)
        return false;  // no walk
    // A walk heads for its next waypoint, a portal or flight leg for its start
    std::uint32_t const last = static_cast<std::uint32_t>(trip.waypoints.size() - 1);
    std::uint32_t const index = type == LegType::Walk ? std::min(trip.next, last) : 0;
    Waypoint const& target = trip.waypoints[index];
    bool const again =
        trip.stuckSeen && target.at.mapId == trip.stuckAt.mapId && Distance(target.at, trip.stuckAt) <= WAYPOINT_YARDS;
    trip.stuckSeen = true;
    trip.stuckAt = target.at;
    auto const avoid = [&trip](std::uint32_t node)
    {
        if (node != NO_NODE && trip.avoid.size() < AVOID_NODES &&
            std::find(trip.avoid.begin(), trip.avoid.end(), node) == trip.avoid.end())
            trip.avoid.push_back(node);
    };
    if (target.node != NO_NODE)
        avoid(target.node);
    else
    {
        trip.noDirect = true;
        if (index > 0)
            avoid(trip.waypoints[index - 1].node);
    }
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

// A walk leg failed by deaths: later routes of this trip leave the walk's graph edge out (both ways); on a hop from
// the bot's start, which is no edge, its node is no start of them. A goal hop leaves nothing out: the goal is the
// goal, and a further death ends the trip
inline void NoteDeath(Trip& trip)
{
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != LegType::Walk || trip.next == 0 ||
        trip.next >= trip.waypoints.size())
        return;
    auto const [from, to] = StuckEdge(trip);
    if (from != NO_NODE)
    {
        std::pair<std::uint32_t, std::uint32_t> const edge{from, to};
        if (trip.avoidEdges.size() < AVOID_EDGES &&
            std::find(trip.avoidEdges.begin(), trip.avoidEdges.end(), edge) == trip.avoidEdges.end())
            trip.avoidEdges.push_back(edge);
        return;
    }
    std::uint32_t const node = trip.waypoints[trip.next].node;
    if (trip.waypoints[trip.next - 1].node == NO_NODE && node != NO_NODE && trip.avoid.size() < AVOID_NODES &&
        std::find(trip.avoid.begin(), trip.avoid.end(), node) == trip.avoid.end())
        trip.avoid.push_back(node);
}

// A walk along a graph link refused for no path (TravelWalk, WT58): later routes of this trip leave the link out, both
// ways (a refusal comes only between two graph nodes)
inline void NoteRefused(Trip& trip)
{
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != LegType::Walk)
        return;
    auto const [from, to] = StuckEdge(trip);
    std::pair<std::uint32_t, std::uint32_t> const edge{from, to};
    if (from != NO_NODE && trip.avoidEdges.size() < AVOID_EDGES &&
        std::find(trip.avoidEdges.begin(), trip.avoidEdges.end(), edge) == trip.avoidEdges.end())
        trip.avoidEdges.push_back(edge);
}

// A transport leg that timed out (its transport never took the bot, or never let it off): later routes of this trip
// leave its link out, both ways. Its ride's pair of nodes (rideAt), between the wait points where the leg has them
inline void NoteTransportTimeout(Trip& trip)
{
    if (trip.legIdx >= trip.legs.size())
        return;
    Leg const& leg = trip.legs[trip.legIdx];
    if (leg.type != LegType::Transport || leg.rideAt + 1 >= leg.nodes.size())
        return;
    std::pair<std::uint32_t, std::uint32_t> const edge{leg.nodes[leg.rideAt], leg.nodes[leg.rideAt + 1]};
    if (trip.avoidEdges.size() < AVOID_EDGES &&
        std::find(trip.avoidEdges.begin(), trip.avoidEdges.end(), edge) == trip.avoidEdges.end())
        trip.avoidEdges.push_back(edge);
}

// After a failed leg: a new route while replans are left; an unsupported leg or a search without a route ends the trip
// at once (a new search would give the same answer), and so does a second stuck walk at the same waypoint. A bot off
// the mesh ends its trip at the first such leg: it recovers by hearth or a short move (MoveFarTo), not by replans.
// Deaths: LEG_DEATHS on a leg replan around its edge (NoteDeath), TRIP_DEATHS on the trip end it. A failed fly leg:
// the trip's later routes walk (noFly), else the replan would offer the same flight. A takeoff refused where the bot
// stands (spotClosed: a no-fly area, a battle, indoors, water) keeps flying: the replan's snapshot closes that spot
// (Snapshot::takeoffHere, AddFlyLegs), so the bot walks to the next open point and tries there. Such replans have their
// own bound, MAX_SPOT_FAILS, outside MAX_REPLANS (a city like Dalaran has many closed points in a row); a later one is
// an ordinary takeoff failure. A timed-out transport leg, a lost ride and a walk back after no exit replan without
// its link (NoteTransportTimeout), and so does a walk link refused for no path (NoteRefused)
inline Recovery AfterFailure(Trip& trip, FailReason reason, bool spotClosed = false)
{
    bool const spotRetry = reason == FailReason::Takeoff && spotClosed && trip.spotFails < MAX_SPOT_FAILS;
    bool const keepsFlying = reason == FailReason::NoAnswer || spotRetry;
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == LegType::Fly && !keepsFlying)
        trip.noFly = true;
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
    if (reason == FailReason::Stuck && NoteStuck(trip))
        return Recovery::Abort;
    if (reason == FailReason::Death)
        NoteDeath(trip);
    if (reason == FailReason::NoPath)
        NoteRefused(trip);
    if (reason == FailReason::Timeout || reason == FailReason::TransportLost || reason == FailReason::NoExit)
        NoteTransportTimeout(trip);
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

// The whole trip's bound beside the legs' own timeouts: a leg's timeout x MAX_REPLANS can run for hours on a long
// merged walk while the bot counts as busy. Before the first answer the estimate is 0 (the slack only)
inline std::uint32_t TripDeadlineMs(float estimateSeconds)
{
    float const ms = std::min(std::max(estimateSeconds, 0.0f) * WALK_TIMEOUT_FACTOR * 1000.0f, 3.0e9f);
    return static_cast<std::uint32_t>(ms) + TRIP_SLACK_MS;
}

inline bool PastDeadline(std::uint32_t activeMs, float routeSeconds) { return activeMs > TripDeadlineMs(routeSeconds); }

// PlayerbotAI::Reset(true) renews the bot's RPG state, also after every far teleport (HandleTeleportAck). What
// survives it: the route request counter always (ids restarting at 0 would let an older answer pass Judge as
// current) and a test's constraints; on a far teleport also a trip in progress with its status start (a portal or a
// cross-map flight leg ends on the new map; a teleport that is not the trip's fails the leg as wrong_map and replans).
// Every other status and the MOVE_FAR state start over as before. Info: NewRpgInfo (data a std::variant, startT,
// routeRequests, constraints); TravelData: its RPG_TRAVEL alternative
template <class TravelData, class Info>
Info AfterReset(Info const& before, bool farTeleport)
{
    Info after;
    after.routeRequests = before.routeRequests;
    after.constraints = before.constraints;  // a state row's flag stays on the Player
    if (farTeleport && std::holds_alternative<TravelData>(before.data))
    {
        after.startT = before.startT;
        after.data = before.data;
    }
    return after;
}
}  // namespace TravelLegs

#endif
