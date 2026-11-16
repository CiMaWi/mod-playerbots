/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVELLEGTYPES_H
#define PLAYERBOTS_TRAVELLEGTYPES_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "RouteTypes.h"
#include "StatusEndRules.h"
#include "TravelLifecycleTypes.h"
#include "TravelWalkTypes.h"

// A trip's state (TravelLegRules.h): the legs, the follower's place on them, the transport boarding and the
// route request waited for. The leg sequencing that steps it stays in TravelLegRules.h.
namespace TravelLegs
{
using TravelRoute::Leg;
using TravelRoute::LegType;
using TravelRoute::Point;

constexpr std::uint32_t NO_NODE = TravelWalk::NO_NODE;  // TravelGraph::NO_NODE

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
    NoPath,   // a walk along a graph link: the engine has no path at its far node (TravelWalk, WT58): refused
    NoWalk    // a walk off the graph: the navmesh probe found no real path when the bot started it (WT56): refused
};

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

struct BoardCrossing
{
    std::uint64_t guid{0};  // the transport boarded; 0 none
    TravelLifecycle::Point deck;
    TravelLifecycle::Point shore;
    TravelLifecycle::Point shoreWorld;  // where the bot stood ashore
};

struct CrossingWalk
{
    TravelLifecycle::Point deck;
    TravelLifecycle::Point shore;
};

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
    char const* why{nullptr};  // Abort: EndTrip's words (string literals) and the end's outcome
    char const* source{nullptr};
    StatusEnd::Outcome outcome{StatusEnd::Outcome::Abandoned};
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
    // walk: the walk off the graph between waypoints[next - 1] and waypoints[next] as probed on the navmesh (WT56;
    // StartLeg resets it)
    TravelWalk::PieceWalk piece;
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
    // after walks off the graph the navmesh refused (NoteWalkRefused): their ends, which later routes do not walk
    std::vector<std::pair<Point, Point>> avoidWalks;
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
    // WT55e: this docking's crossing from the stop's wait point was checked (refused, the quay was searched from it),
    // and the quay is the search's place, not the wait point (NoteBoardCheck; the docking's end clears both)
    bool boardChecked{false};
    bool searchedQuay{false};
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
}  // namespace TravelLegs

#endif
