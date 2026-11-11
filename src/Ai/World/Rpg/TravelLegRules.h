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
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

#include "RouteRules.h"

namespace TravelLegs
{
using TravelRoute::Distance;
using TravelRoute::Leg;
using TravelRoute::LegType;
using TravelRoute::Point;

constexpr std::uint32_t NO_NODE = TravelGraph::NO_NODE;
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
constexpr float HEARTH_ARRIVE_YARDS = 40.0f;  // a hearth leg is done within this of the bind point
constexpr std::uint32_t HEARTH_SLACK_MS = 30 * 1000;  // a hearth leg's time beyond its cooldown wait and cast
constexpr std::uint32_t REPLAN_DELAY_MS = 2000;       // the failed leg's stuck report reaches the published book first
constexpr std::uint32_t LEGS_PER_TICK = 4;            // finished legs passed in one tick at most
constexpr std::uint32_t WAYPOINTS_PER_TICK = 4;       // reached waypoints passed in one tick at most
constexpr std::uint8_t MAX_RESUBMITS = 3;             // route requests asked again per trip after a late or lost answer
constexpr std::size_t AVOID_NODES = 4;                // route ends a trip leaves out after stuck walks
constexpr std::size_t AVOID_EDGES = 4;                // graph edges a trip leaves out after deaths on them
constexpr std::uint8_t LEG_DEATHS = 2;                // deaths on one leg fail it (FailReason::Death)
constexpr std::uint8_t TRIP_DEATHS = 3;               // deaths on one trip end it

enum class Act : std::uint8_t
{
    Walk,              // MoveFarTo(target) on the bot's map
    UseTrigger,        // step into the portal leg's area trigger
    TalkFlightMaster,  // at the flight master of the leg's start: learn the node, pay, take off
    UseHearth,         // the hearthstone leg: cast it (wait for the cooldown and the cast)
    Ride,              // in flight (a cross-map flight continues)
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
    Portal,       // the area trigger did not take the bot through
    Fare,         // the purse does not pay the flight
    Landing,      // the flight ended away from the leg's end
    Unsupported,  // a transport leg (WT19): not produced while the request leaves them out
    NoAnswer,     // the route service did not answer in time, or the answer was for another map or too old
    NoRoute,      // the search found no route
    Taxi,         // no flight master at the leg's start, the path is not discovered, or the core refused the taxi
    Hearth,       // the hearthstone is missing or its cast failed
    OffMesh,      // the bot stands off the walkable mesh (MoveFarTo's off-mesh check): no walk can start there
    Death         // the bot died LEG_DEATHS times on the leg or TRIP_DEATHS times on the trip (NoteLife)
};

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
    }
    return "?";
}

// Travel event sources (EventLog::Emit(bot, "Travel", source, ...); literals, as the log keeps the pointer's text):
// "start", "arrived", "leg_done:<leg type>", "leg_failed:<reason>", "abort:<reason>" and the aborts that are no failed
// leg: "abort:deadline", "abort:no_service", "abort:stopped"
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
    }
    return "abort:?";
}

struct Waypoint
{
    Point at;
    std::uint32_t node{NO_NODE};  // the graph node at this point; NO_NODE for the bot's start or the goal
};

// What the follower sees of the bot this tick
struct Observation
{
    Point bot;
    bool inFlight{false};
    std::uint32_t money{0};  // copper
    bool stuck{false};       // MoveFarTo gave the current waypoint up (a traveller does not go IDLE there)
    std::uint32_t nowMs{0};  // getMSTime()
};

struct Decision
{
    Act act{Act::Wait};
    Point target;  // Walk: where to; UseTrigger, TalkFlightMaster: the leg's start; Ride: the leg's end
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

// A route's legs and the follower's place on them
struct Trip
{
    std::vector<Leg> legs;
    std::uint32_t legIdx{0};
    std::uint32_t legStartMs{0};
    std::vector<Waypoint> waypoints;  // the current leg's: [0] its start; the bot heads for waypoints[next]
    std::uint32_t next{1};
    bool approached{false};     // portal: the bot reached the trigger's node; from there it heads for the trigger
    bool triggered{false};      // portal: the area trigger was sent
    bool taxiActivated{false};  // flight: the taxi was taken (fare paid)
    bool seenInFlight{false};   // flight: the bot was seen in flight
    std::uint8_t replans{0};
    std::uint8_t resubmits{0};         // route requests asked again after a late or lost answer (Resubmit)
    std::vector<std::uint32_t> avoid;  // after stuck walks: nodes later routes neither start nor end at (NoteStuck)
    bool noDirect{false};              // after a stuck walk towards the goal: no straight walk from the bot to it
    bool stuckSeen{false};
    Point stuckAt;           // the waypoint of the trip's last stuck walk
    LegRecord record;        // the current leg's (StartLeg starts it anew)
    std::uint8_t deaths{0};  // on the whole trip
    // After deaths on a walk (NoteDeath): graph edges later routes do not use (this bot's trip only)
    std::vector<std::pair<std::uint32_t, std::uint32_t>> avoidEdges;
};

// The route request the follower waits for (the bot's latest; RouteService answers older ones too)
struct RouteWait
{
    std::uint32_t requestId{0};
    std::uint32_t mapId{0};
    std::uint32_t requestedMs{0};
    bool waiting{false};
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
            return PORTAL_TIMEOUT_MS;
        case LegType::Flight:
            return static_cast<std::uint32_t>(leg.seconds * FLIGHT_TIMEOUT_FACTOR * 1000.0f) + LEG_SLACK_MS;
        case LegType::Hearth:
            // The route's seconds hold the cooldown wait and the cast
            return static_cast<std::uint32_t>(leg.seconds * 1000.0f) + HEARTH_SLACK_MS;
        default:
            return LEG_SLACK_MS;
    }
}

// The points a leg passes: its start, then for a walk every graph node in order, then its end. Points within
// SAME_POINT_YARDS of the previous one merge (a leg starting or ending on a node). nodeAt(index) -> Point
template <class NodeAt>
std::vector<Waypoint> Waypoints(Leg const& leg, NodeAt const& nodeAt)
{
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
        record.combatSinceMs = std::max<std::uint32_t>(nowMs, 1);
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

// The leg's seconds in combat until now (a running stretch included)
inline float CombatSeconds(LegRecord const& record, std::uint32_t nowMs)
{
    std::uint32_t const ms = record.combatMs + (record.combatSinceMs ? nowMs - record.combatSinceMs : 0);
    return static_cast<float>(ms) / 1000.0f;
}

// How the bot went on the leg, from its follower ticks: foot (never mounted), mounted (always), part
inline char const* MountWord(LegRecord const& record)
{
    if (!record.mountedTicks)
        return "foot";
    return record.mountedTicks >= record.ticks ? "mounted" : "part";
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
    trip.approached = false;
    trip.triggered = false;
    trip.taxiActivated = false;
    trip.seenInFlight = false;
    trip.record = NextRecord(trip.record, nowMs);
    trip.waypoints.clear();
    if (index < trip.legs.size())
        trip.waypoints = Waypoints(trip.legs[index], nodeAt);
}

// The current leg's next step. Done, Fail and the others; never Arrived
inline Decision StepLeg(Trip& trip, Observation const& seen)
{
    Leg const& leg = trip.legs[trip.legIdx];
    bool const lastLeg = trip.legIdx + 1 == trip.legs.size();
    std::uint32_t const legMs = seen.nowMs - trip.legStartMs;  // unsigned: wraps as getMSTimeDiff does
    auto const fail = [](FailReason reason) { return Decision{Act::Fail, {}, reason}; };
    auto const within = [&seen](Point const& at, float yards)
    { return seen.bot.mapId == at.mapId && Distance(seen.bot, at) <= yards; };

    if (seen.stuck)
        return fail(FailReason::Stuck);

    switch (leg.type)
    {
        case LegType::Walk:
        {
            if (seen.bot.mapId != leg.to.mapId)
                return fail(FailReason::WrongMap);
            std::uint32_t const count = static_cast<std::uint32_t>(trip.waypoints.size());
            auto const reached = [&](std::uint32_t index)
            { return within(trip.waypoints[index].at, lastLeg && index + 1 == count ? GOAL_YARDS : WAYPOINT_YARDS); };
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
            // From the trigger's node on, the bot heads for the trigger itself (UsePortal), which may lie further than
            // WAYPOINT_YARDS on: turning back to the node would alternate with that walk until the leg timed out
            if (!trip.approached && !within(leg.from, WAYPOINT_YARDS))
                return {Act::Walk, leg.from, FailReason::None};
            trip.approached = true;
            return {Act::UseTrigger, leg.from, FailReason::None};
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
        default:
            return fail(FailReason::Unsupported);
    }
}

// The trip's next step: finished legs are passed (at most LEGS_PER_TICK), then the current leg's step; Arrived after
// the last leg
template <class NodeAt>
Decision Advance(Trip& trip, Observation const& seen, NodeAt const& nodeAt)
{
    for (std::uint32_t passed = 0; passed < LEGS_PER_TICK; ++passed)
    {
        if (trip.legIdx >= trip.legs.size())
            return {Act::Arrived, seen.bot, FailReason::None};
        Decision const decision = StepLeg(trip, seen);
        if (decision.act != Act::Done)
            return decision;
        StartLeg(trip, trip.legIdx + 1, seen.nowMs, nodeAt);
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
    if (type == LegType::Hearth)
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

// The graph edge the bot is on: a walk's two waypoints around it, or a portal's two nodes; {NO_NODE, NO_NODE} when an
// end is no graph node (the bot's start, the goal)
inline std::pair<std::uint32_t, std::uint32_t> StuckEdge(Trip const& trip)
{
    std::pair<std::uint32_t, std::uint32_t> const none{NO_NODE, NO_NODE};
    if (trip.legIdx >= trip.legs.size() || trip.next == 0 || trip.next >= trip.waypoints.size())
        return none;
    LegType const type = trip.legs[trip.legIdx].type;
    if (type != LegType::Walk && type != LegType::Portal)
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

// After a failed leg: a new route while replans are left; an unsupported leg or a search without a route ends the trip
// at once (a new search would give the same answer), and so does a second stuck walk at the same waypoint. A bot off
// the mesh ends its trip at the first such leg: it recovers by hearth or a short move (MoveFarTo), not by replans.
// Deaths: LEG_DEATHS on a leg replan around its edge (NoteDeath), TRIP_DEATHS on the trip end it
inline Recovery AfterFailure(Trip& trip, FailReason reason)
{
    std::uint32_t const combatSinceMs = trip.record.combatSinceMs;
    trip.record = LegRecord{};  // the failed leg's record ends (a running combat stretch goes on)
    trip.record.combatSinceMs = combatSinceMs;
    if (reason == FailReason::Unsupported || reason == FailReason::NoRoute || reason == FailReason::OffMesh ||
        (reason == FailReason::Death && trip.deaths >= TRIP_DEATHS) || trip.replans >= MAX_REPLANS)
        return Recovery::Abort;
    if (reason == FailReason::Stuck && NoteStuck(trip))
        return Recovery::Abort;
    if (reason == FailReason::Death)
        NoteDeath(trip);
    ++trip.replans;
    return Recovery::Replan;
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

// The deadline counts from the current route's start against that route's estimate (a replan starts a new bound;
// MAX_REPLANS bounds their number) and waits while the bot is in flight: a bot cannot leave the taxi, so ending the
// trip mid-air would only drop the legs after the landing
inline bool PastDeadline(std::uint32_t routeMs, float routeSeconds, bool inFlight)
{
    return !inFlight && routeMs > TripDeadlineMs(routeSeconds);
}

// PlayerbotAI::Reset(true) renews the bot's RPG state, also after every far teleport (HandleTeleportAck). What
// survives it: the route request counter always (ids restarting at 0 would let an older answer pass Judge as
// current); on a far teleport also a trip in progress with its status start (a portal or a cross-map flight leg ends
// on the new map; a teleport that is not the trip's fails the leg as wrong_map and replans). Every other status and
// the MOVE_FAR state start over as before. Info: NewRpgInfo (data a std::variant, startT, routeRequests); TravelData:
// its RPG_TRAVEL alternative
template <class TravelData, class Info>
Info AfterReset(Info const& before, bool farTeleport)
{
    Info after;
    after.routeRequests = before.routeRequests;
    if (farTeleport && std::holds_alternative<TravelData>(before.data))
    {
        after.startT = before.startT;
        after.data = before.data;
    }
    return after;
}
}  // namespace TravelLegs

#endif
