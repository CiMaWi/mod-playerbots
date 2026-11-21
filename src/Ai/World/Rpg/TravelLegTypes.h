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
#include "TravelCrossingTypes.h"
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
    Board,             // transport: cross onto the vessel by the current boarding row (TravelLegs::RowNow)
    Disembark,         // transport: cross off the vessel by the current leaving row (TravelLegs::RowNow)
    Stand,             // transport (WT70c): aboard, take a place on the deck once (a deck point near the exit)
    Deck,              // transport (WT70c): aboard, walk on the deck to the leaving row's deck point
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
    NoExit,   // the far stop's leaving rows are spent: the bot left again at the stop it boarded at (Boarding::Back)
    Takeoff,  // a fly leg: no takeoff where the bot stands, or no flying mount after FLY_MOUNT_TRIES tries
    NoPath,   // a walk along a graph link: the engine has no path at its far node (TravelWalk, WT58): refused
    NoWalk,   // a walk off the graph: the navmesh probe found no real path when the bot started it (WT56): refused
    // WT70c (WT.53): the stop has no stored crossing row, or the vessel seen has no mesh: no ride (before any walk)
    NoCrossing,
    CrossingFailed  // WT70c: every stored boarding row of the stop failed while the vessel stood
};

// Transport event sources (EventLog::Emit(bot, EventType::Transport, TransportSource(event), entry, amount)), id = the
// transport's gameobject entry. Amounts: attempt = seconds of the walk to the dock point, boarded = seconds waited
// there, arrived, leave and back = seconds aboard, missed = the leg's misses so far, stuck_aboard = seconds since
// the leave (or the off_first), no_exit = 0, off_first = the bound's seconds (the transport's period).
// The sources that name a stored crossing row (cross, crossed, cross_failed, cross_cut, no_crossing) carry
// TravelCrossing::EventAmount: stop * 100 + direction * 10 + seq; place = 1 taken, 0 stayed; off_deck = tenths of a
// yard; fight = seconds fought aboard
enum class TransportEvent : std::uint8_t
{
    Attempt,  // the bot reached its first row's dock point
    Boarded,
    Arrived,       // off the transport at the leg's end
    Missed,        // the transport left the dock without the bot
    Leave,         // aboard, the transport stands at the leg's end: the bot walks off
    StuckAboard,   // the transport left the leg's end with the bot still aboard after a leave
    NoExit,        // the far stop's leaving rows are all spent: the leg rides back
    OffFirst,      // a pending trip operation waits until the bot is ashore
    Lost,          // verified membership loss without a safe exit
    Back,          // after a no_exit, off the transport where it boarded, by that stop's leaving rows
    Cross,         // a crossing by a stored row was issued
    Crossed,       // boarded or exited by it
    CrossFailed,   // cancelled while the vessel stood: the row is spent
    CrossCut,      // the vessel left during it
    NoCrossing,    // no usable row, or a vessel without a mesh
    Place,         // aboard: the place on the deck
    OffDeck,       // a deck move ended off the vessel's mesh (the corner-cut watch)
    Fight,         // the leg ended after a fight aboard
    FollowTimeout  // a follower's owed ride ran out of the wait clock (WT70e)
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
    std::uint32_t periodMs{0};  // its period; 0 unknown
    Point at;                   // its position
    std::uint64_t guid{0};
    // WT70c: its orientation (with `at` the pose the stand test compares with a stop record's), its place in its
    // period (a ship's or zeppelin's path progress, read against the stop's window) and whether it holds a vessel mesh
    // (deck points and deck paths; none: no_crossing)
    float o{0.0f};
    std::uint32_t progressMs{0};
    bool mesh{false};
};

// A follower's step at its master's vessel (WT70d, PLAN 3.21 (8); TravelLegs::FollowStep): nothing (the follow code
// goes on), walk on the world mesh to a boarding row's dock point, walk on the deck to a leaving row's deck point, wait
// on the point for the vessel to stand long enough, cross by the row, or a crossing under way
enum class FollowAct : std::uint8_t
{
    None,
    Walk,
    Deck,
    Wait,
    Cross,
    UnderWay
};

// What the travel block keeps of a follower's crossing: the stop record the vessel stands at (an index), the direction
// and the rows of it spent at this docking, since when the vessel was seen there (the stand test of a car without a
// window), and the one crossing issued (crossMs, 0: none; its entry and event amount). Riding with its master: whether
// the vessel's path finder refused the deck walk ahead of the docking (WT70d-b; once per ride, as the leg's). The walk
// to the current boarding row's dock point: how far the bot was when it was issued (0: none on this row) and the
// glue's mark of that move (its last-move time; the walk is under way while that move is the bot's last and runs)
struct FollowCrossing
{
    std::uint32_t stop{TravelCrossing::NO_STOP};
    TravelCrossing::Direction direction{TravelCrossing::Direction::Board};
    std::uint8_t spent{0};
    bool aheadRefused{false};
    float walkYards{0.0f};
    std::uint32_t walkMark{0};
    std::uint32_t posedMs{0};
    std::uint32_t crossMs{0};
    std::uint32_t crossEntry{0};
    std::uint32_t crossAmount{0};
    // The ride owed (WT70e, PLAN 3.21 (8)): the vessel its master boarded without it (guid; 0: none), the stop it
    // boards at and the stop it leaves at (NO_STOP: the vessel's next stop other than the boarding one), since when the
    // wait clock runs (from the debt's start; aboard, from the boarding) and its bound, and whether it is aboard
    struct OwedRide
    {
        std::uint64_t vessel{0};
        std::uint32_t entry{0};
        std::uint32_t from{TravelCrossing::NO_STOP};
        std::uint32_t to{TravelCrossing::NO_STOP};
        std::uint32_t sinceMs{0};
        std::uint32_t boundMs{0};
        bool aboard{false};
    };
    OwedRide owed;
};

// What the current leg saw of the bot: follower ticks mounted, time in combat and deaths. The follower counts the
// ticks; NoteLife counts combat (PlayerbotAI's engine changes) and deaths (WorldLogWriter's death hooks), because the
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
    // WT70c: the stop (its number) the vessel stood at last, and that stop's leaving rows spent (another stop: 0 again)
    std::uint32_t stop{TravelCrossing::NO_STOP};
    std::uint8_t spent{0};
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
    std::uint8_t replans{0};
    std::uint8_t spotFails{0};  // takeoffs refused where the bot stood that replanned flying (AfterFailure)
    std::uint8_t resubmits{0};  // route requests asked again after a late or lost answer (Resubmit)
    bool stuckSeen{false};
    Point stuckAt;           // the waypoint of the trip's last stuck walk (StuckAgain)
    LegRecord record;        // the current leg's (StartLeg starts it anew)
    std::uint8_t deaths{0};  // on the whole trip
    // Transport legs (StartLeg resets them; the notes stay until the follower sends and clears them)
    Boarding boarding{Boarding::Approach};
    TransportSight sight;          // the last scan
    std::uint32_t periodMs{0};     // the transport's period once a scan read it
    std::uint32_t stateMs{0};      // when boarding entered its state
    std::uint32_t waitSinceMs{0};  // when the bot reached the dock node (the leg's start until then)
    std::uint32_t boardedMs{0};
    bool departed{false};  // Board: the transport left without the bot, at departedMs
    std::uint32_t departedMs{0};
    std::uint8_t misses{0};
    std::uint64_t passengerGuid{0};  // confirmed boarding for this leg
    // WT70c (stored crossings): the from-stop's boarding row in use (the rows before it are spent), the leaving rows
    // spent at the stop the bot leaves by, whether it took its place aboard (once per boarding), whether the vessel's
    // path finder refused the deck walk ahead of the docking (WT70d-b; once per boarding), and since when the scans saw
    // the vessel at the stop's pose (0: not there; the stand test of a vessel without a window)
    std::uint8_t row{0};
    std::uint8_t leaveSpent{0};
    bool placed{false};
    bool aheadRefused{false};
    std::uint32_t posedMs{0};
    // WT70d-b: the bot's followers were woken as this transport leg started (once per leg; StartLeg resets it)
    bool followersWoken{false};
    // WT70c round 2: when the last scan was taken; the tracked crossing (issued at crossMs, 0: none; its event amount)
    // and its end without success, whose verdict waits for the next scan (cancelled: the vessel still at the pose
    // spends the row, gone it was cut); the from-stop's leaving rows spent on the way back (Boarding::Back); whether
    // attempt was noted (once per leg); and the leg's combat ms when the bot boarded (a fight aboard is what came
    // after)
    std::uint32_t sightMs{0};
    std::uint32_t crossMs{0};
    std::uint32_t crossAmount{0};
    bool cancelled{false};
    std::uint8_t backSpent{0};
    bool attempted{false};
    std::uint32_t boardCombatMs{0};
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
