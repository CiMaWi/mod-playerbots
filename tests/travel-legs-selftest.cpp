/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of RPG_TRAVEL's leg sequencing (src/Ai/World/Rpg/TravelLegRules.h) with scripted observations:
// a route walk -> portal -> walk -> flight -> walk with no-op walks at both ends, the timeouts per leg type, a stuck
// walk with its edge and the replans up to the abort, a wrong map, an unaffordable fare, the leg predicates, the
// waypoints of a leg, the judgement of route answers, what survives the AI's full reset (a far teleport keeps a
// trip) and the whole trip's deadline; then the flight leg's failure (taxi) and its reported hop, the deadline in
// flight and after a replan, the Travel event sources, and the named places of `playerbots route go` (TravelPlaces.h;
// their graph check is in route-rules-selftest). Also what a stuck walk leaves out of the trip's later routes (a direct
// walk, the first hop, the goal hop; a second stuck walk at one waypoint ends the trip) and the late answers asked
// again. Deaths on a leg (the second fails it, the third ends the trip) and a leg's mount and combat record. Fly legs
// (own flying mount): their waypoints, takeoff, height check, landing, failures and the replan without flying.
// Transport legs by stored crossings (WT70c, PLAN 3.21 (10)): a ship across maps through its states by the rows of its
// two stop records (case 1), the vessel leaving under a boarding walk or jump (3), a row that fails while it stands and
// the next one (4), a stop or vessel without a crossing (5), the wait clock (6), a crossing without an outcome (7), a
// lost ride (8), the far stop's rows spent and the way back (9), a missed exit that rides round (10), the place aboard
// (12), a fight past the far stop (13) and the get-off by the rows of the stop the vessel stands at (13, 14); the stand
// test (a ship's window, a car's second scan) and the crossing's time; the Transport event words.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "MountRules.h"
#include "TravelLegRules.h"
#include "TravelPlaces.h"

using TravelLegs::Act;
using TravelLegs::Decision;
using TravelLegs::FailReason;
using TravelLegs::Observation;
using TravelLegs::Trip;
using TravelRoute::Leg;
using TravelRoute::LegType;
using TravelRoute::Point;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << name << "\n";
}

// A fake graph: node index -> position
std::vector<Point> const NODES = {
    {0, 0.0f, 0.0f, 0.0f},        // 0 start node
    {0, 100.0f, 0.0f, 0.0f},      // 1
    {0, 200.0f, 0.0f, 0.0f},      // 2 the portal's trigger
    {1, 1000.0f, 1000.0f, 0.0f},  // 3 the portal's exit
    {1, 1100.0f, 1000.0f, 0.0f},  // 4
    {1, 1200.0f, 1000.0f, 0.0f},  // 5 a flight master
    {1, 5000.0f, 5000.0f, 0.0f},  // 6 the landing
    {0, 40.0f, 0.0f, 0.0f},       // 7 the wait point of pier A (node 1), beyond a ship's dock reach of it
    {1, 1000.0f, 950.0f, 0.0f},   // 8 the wait point of pier B (node 3)
};
Point const START{0, -2.0f, 0.0f, 0.0f};
Point const GOAL{1, 5055.0f, 5000.0f, 0.0f};
std::uint32_t const FARE = 500;

Point NodeAt(std::uint32_t index) { return NODES[index]; }

Leg MakeLeg(LegType type, Point from, Point to, std::vector<std::uint32_t> nodes, float seconds, std::uint32_t fare = 0)
{
    Leg leg;
    leg.type = type;
    leg.from = from;
    leg.to = to;
    leg.nodes = std::move(nodes);
    leg.seconds = seconds;
    leg.fareCopper = fare;
    if (type == LegType::Portal)
        leg.object = 99;
    if (type == LegType::StaticPortal)
        leg.object = 190960;
    if (type == LegType::Flight)
        leg.taxiNodes = {10, 11};
    return leg;
}

std::vector<Leg> FullRoute()
{
    return {
        MakeLeg(LegType::Walk, START, NODES[0], {0}, 0.0f),            // a 0-second start walk
        MakeLeg(LegType::Walk, NODES[0], NODES[2], {0, 1, 2}, 30.0f),  // two graph links
        MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 3.0f),    // onto map 1
        MakeLeg(LegType::Walk, NODES[3], NODES[5], {3, 4, 5}, 30.0f),  // to the flight master
        MakeLeg(LegType::Flight, NODES[5], NODES[6], {5, 6}, 200.0f, FARE),
        MakeLeg(LegType::Walk, NODES[6], GOAL, {6}, 8.0f),  // off the graph to the goal
        MakeLeg(LegType::Walk, GOAL, GOAL, {}, 0.0f),       // a 0-second end walk
    };
}

Observation At(Point bot, std::uint32_t nowMs, std::uint32_t money = 10000, bool inFlight = false)
{
    Observation seen;
    seen.bot = bot;
    seen.nowMs = nowMs;
    seen.money = money;
    seen.inFlight = inFlight;
    return seen;
}

Trip StartedTrip(std::vector<Leg> legs, std::uint32_t nowMs = 1000)
{
    Trip trip;
    trip.legs = std::move(legs);
    TravelLegs::StartLeg(trip, 0, nowMs, NodeAt);
    return trip;
}

// The follower's leg clock (TravelProgress::State::LegMs) on a leg nothing interrupts: the time since the leg began
Decision Step(Trip& trip, Observation seen)
{
    seen.legMs = seen.nowMs - trip.legStartMs;
    return TravelLegs::StepLeg(trip, seen);
}

template <class NodeAtFn>
Decision Go(Trip& trip, Observation seen, NodeAtFn const& nodeAt)
{
    seen.legMs = trip.legIdx < trip.legs.size() ? seen.nowMs - trip.legStartMs : 0;
    return TravelLegs::Advance(trip, seen, nodeAt);
}

// The whole route with a simulated bot that does what it is told: walks arrive at once, the trigger takes the bot
// through one tick after it is sent, the taxi takes off one tick after it is taken and lands after one tick of riding
void CheckFullRoute()
{
    Trip trip = StartedTrip(FullRoute());
    Check(trip.legIdx == 1, "the 0-second start walk is skipped");
    Check(trip.waypoints.size() == 3 && trip.waypoints[0].node == 0 && trip.waypoints[2].node == 2,
          "the first walk's waypoints are its nodes 0, 1, 2");

    Point bot = START;
    std::uint32_t money = 10000;
    std::uint32_t nowMs = 1000;
    bool inFlight = false;
    bool portalSent = false;
    bool taxiTaken = false;
    std::string trace;
    std::vector<Point> walked;
    for (int tick = 0; tick < 40; ++tick)
    {
        nowMs += 1000;
        Decision const next = Go(trip, At(bot, nowMs, money, inFlight), NodeAt);
        trace += std::string(trace.empty() ? "" : " ") + ActWord(next.act);
        if (next.act == Act::Arrived || next.act == Act::Fail)
            break;
        switch (next.act)
        {
            case Act::Walk:
                walked.push_back(next.target);
                bot = next.target;
                break;
            case Act::UseTrigger:
                trip.triggered = true;
                portalSent = true;
                break;
            case Act::TalkFlightMaster:
                trip.taxiActivated = true;
                money -= FARE;
                taxiTaken = true;
                break;
            case Act::Ride:
                inFlight = false;
                bot = NODES[6];
                break;
            case Act::Wait:
                if (portalSent)
                {
                    bot = NODES[3];
                    portalSent = false;
                }
                if (taxiTaken)
                {
                    inFlight = true;
                    taxiTaken = false;
                }
                break;
            default:
                break;
        }
    }
    std::string const expected = "walk walk trigger wait walk walk talk wait ride walk arrived";
    Check(trace == expected, "full route: got \"" + trace + "\", want \"" + expected + "\"");
    bool const order = walked.size() == 5 && walked[0].x == 100.0f && walked[1].x == 200.0f && walked[2].x == 1100.0f &&
                       walked[3].x == 1200.0f && walked[4].x == GOAL.x;
    Check(order, "full route: walk targets are nodes 1, 2, 4, 5 and the goal in order");
    Check(money == 10000 - FARE, "full route: the fare is paid once");
    Check(trip.legIdx == trip.legs.size() && trip.replans == 0, "full route: all legs passed without a replan");
}

void CheckTimeouts()
{
    // Walk: estimate x 3 + 60 s; completion by position wins over the timeout
    Leg const walk = MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 100.0f);
    Check(TravelLegs::LegTimeoutMs(walk) == 360000, "walk timeout = 100 s x 3 + 60 s");
    Trip trip = StartedTrip({walk}, 0);
    Check(Step(trip, At(NODES[0], 359999)).act == Act::Walk, "walk: no timeout before 360 s");
    Decision const late = Step(trip, At(NODES[0], 360001));
    Check(late.act == Act::Fail && late.reason == FailReason::Timeout, "walk: Fail(timeout) after 360 s");
    Check(Step(trip, At(NODES[1], 999999)).act == Act::Done, "walk: an arrival after the time is done");

    // A 0-second walk whose end lies 700 yd away: walked, with time for 700 yd at run speed
    Leg const moved = MakeLeg(LegType::Walk, NODES[6], {1, 5700.0f, 5000.0f, 0.0f}, {6}, 0.0f);
    Check(!TravelLegs::IsNoOp(moved), "a 0-second walk whose end moved is not skipped");
    Check(TravelLegs::LegTimeoutMs(moved) == 360000, "a moved walk's timeout uses its straight line (100 s)");
    Check(TravelLegs::IsNoOp(MakeLeg(LegType::Walk, GOAL, GOAL, {}, 0.0f)), "a 0-second walk in place is skipped");
    Check(!TravelLegs::IsNoOp(MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 0.0f)), "a portal is no no-op");

    // Portal: 20 s after the leg began
    Trip portal = StartedTrip({MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 3.0f)}, 0);
    portal.triggered = true;
    Check(Step(portal, At(NODES[2], 19000)).act == Act::Wait, "portal: waits for the trigger");
    Decision const stuckPortal = Step(portal, At(NODES[2], 21000));
    Check(stuckPortal.act == Act::Fail && stuckPortal.reason == FailReason::Portal, "portal: Fail(portal) after 20 s");

    // Flight: path seconds x 2 + 60 s before take-off; none in the air
    Leg const flight = MakeLeg(LegType::Flight, NODES[5], NODES[6], {5, 6}, 200.0f, FARE);
    Check(TravelLegs::LegTimeoutMs(flight) == 460000, "flight timeout = 200 s x 2 + 60 s");
    Trip flying = StartedTrip({flight}, 0);
    flying.taxiActivated = true;
    Check(Step(flying, At(NODES[5], 459000)).act == Act::Wait, "flight: waits for take-off");
    Check(Step(flying, At(NODES[5], 461000)).reason == FailReason::Timeout,
          "flight: Fail(timeout) when the taxi never takes off");
    Check(Step(flying, At({1, 3000.0f, 3000.0f, 300.0f}, 900000, 0, true)).act == Act::Ride,
          "flight: no timeout in the air");
}

void CheckStuckAndReplans()
{
    Trip trip = StartedTrip(FullRoute());
    Check(Go(trip, At(NODES[1], 2000), NodeAt).act == Act::Walk && trip.next == 2, "stuck: the walk heads for node 2");
    auto const edge = TravelLegs::StuckEdge(trip);
    Check(edge.first == 1 && edge.second == 2, "stuck: the edge is node 1 -> node 2");

    Check(TravelLegs::AfterFailure(trip, FailReason::Stuck) == TravelLegs::Recovery::Replan && trip.avoid.size() == 1 &&
              trip.avoid[0] == 2 && !trip.noDirect,
          "replan 1 of 3 is granted; node 2 is no end of the next route");
    for (int replan = 2; replan <= TravelLegs::MAX_REPLANS; ++replan)
        Check(TravelLegs::AfterFailure(trip, FailReason::Timeout) == TravelLegs::Recovery::Replan,
              "replan " + std::to_string(replan) + " of 3 is granted");
    Check(TravelLegs::AfterFailure(trip, FailReason::Stuck) == TravelLegs::Recovery::Abort,
          "the fourth failure aborts the trip");
    Trip fresh;
    Check(TravelLegs::AfterFailure(fresh, FailReason::Unsupported) == TravelLegs::Recovery::Abort,
          "an unsupported leg aborts at once");
    Check(TravelLegs::AfterFailure(fresh, FailReason::NoRoute) == TravelLegs::Recovery::Abort,
          "no route aborts at once");
    Check(TravelLegs::AfterFailure(fresh, FailReason::OffMesh) == TravelLegs::Recovery::Abort && fresh.avoid.empty(),
          "off the mesh aborts at once and leaves no node out");
    Check(fresh.replans == 0, "an abort spends no replan");

    // The first walk from the bot's own start has no graph edge yet; the portal's edge is its two nodes
    Trip fromStart = StartedTrip({MakeLeg(LegType::Walk, START, NODES[1], {0, 1}, 20.0f)});
    Check(fromStart.waypoints.size() == 3 && fromStart.waypoints[0].node == TravelLegs::NO_NODE,
          "a walk from the bot's start: start, node 0, node 1");
    Check(TravelLegs::StuckEdge(fromStart).first == TravelLegs::NO_NODE, "no edge from the bot's start point");
    Trip portal = StartedTrip({MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 3.0f)});
    auto const portalEdge = TravelLegs::StuckEdge(portal);
    Check(portalEdge.first == 2 && portalEdge.second == 3, "the portal's edge is node 2 -> node 3");
}

// A stuck walk's start or goal hop is no graph edge: the trip leaves it out of its later routes itself
void CheckStuckAvoid()
{
    using TravelLegs::Recovery;
    Point const near{0, 60.0f, 30.0f, 0.0f};

    // A direct walk, then a route through nodes 0 and 1 that gets stuck at the same goal
    Trip direct = StartedTrip({MakeLeg(LegType::Walk, START, near, {}, 9.0f)});
    Check(direct.waypoints.size() == 2 && TravelLegs::StuckEdge(direct).first == TravelLegs::NO_NODE,
          "a direct walk has no graph edge");
    Check(TravelLegs::AfterFailure(direct, FailReason::Stuck) == Recovery::Replan && direct.noDirect &&
              direct.avoid.empty(),
          "stuck on a direct walk: the next route walks no straight line to the goal");
    direct.legs = {MakeLeg(LegType::Walk, START, near, {0, 1}, 20.0f)};
    TravelLegs::StartLeg(direct, 0, 5000, NodeAt);
    direct.next = static_cast<std::uint32_t>(direct.waypoints.size() - 1);
    Check(TravelLegs::AfterFailure(direct, FailReason::Stuck) == Recovery::Abort && direct.replans == 1,
          "stuck at the same goal again: the trip ends with 2 replans left");
    Check(direct.avoid.size() == 1 && direct.avoid[0] == 1, "stuck on the goal hop: node 1 before it is left out");

    // The first hop from the bot's start, then the next hop: another waypoint, another replan
    Trip first = StartedTrip({MakeLeg(LegType::Walk, START, NODES[1], {0, 1}, 20.0f)});
    Check(first.next == 1 && TravelLegs::AfterFailure(first, FailReason::Stuck) == Recovery::Replan &&
              first.avoid.size() == 1 && first.avoid[0] == 0 && !first.noDirect,
          "stuck on the first hop: node 0 is no end of the next route");
    first.next = 2;
    Check(TravelLegs::AfterFailure(first, FailReason::Stuck) == Recovery::Replan && first.avoid.size() == 2,
          "stuck at another waypoint: a replan, node 1 left out too");

    // The goal hop from a node: no straight walk, and node 1 is no end
    Trip last = StartedTrip({MakeLeg(LegType::Walk, NODES[0], near, {0, 1}, 20.0f)});
    last.next = 2;
    Check(last.waypoints.size() == 3 && TravelLegs::AfterFailure(last, FailReason::Stuck) == Recovery::Replan &&
              last.noDirect && last.avoid.size() == 1 && last.avoid[0] == 1,
          "stuck on the goal hop: no straight walk, node 1 is no end of the next route");

    // At most AVOID_NODES nodes; a hearth leg walks nowhere
    Trip many = StartedTrip({MakeLeg(LegType::Walk, START, NODES[2], {0, 1, 2}, 30.0f),
                             MakeLeg(LegType::Walk, NODES[3], NODES[6], {3, 4, 5, 6}, 30.0f)});
    for (std::uint32_t leg = 0; leg < 2; ++leg)
    {
        TravelLegs::StartLeg(many, leg, 1000, NodeAt);
        for (std::uint32_t next = 1; next < many.waypoints.size(); ++next)
        {
            many.next = next;
            TravelLegs::NoteStuck(many);
        }
    }
    Check(many.avoid.size() == TravelLegs::AVOID_NODES && many.avoid.front() == 0 && many.avoid.back() == 4,
          "at most 4 nodes are left out");
    Trip hearth = StartedTrip({MakeLeg(LegType::Hearth, START, NODES[3], {}, 10.0f)});
    Check(!TravelLegs::NoteStuck(hearth) && !hearth.noDirect && hearth.avoid.empty() && !hearth.stuckSeen,
          "a hearth leg leaves nothing out");

    // WT58: a walk link refused for no path: the replan leaves the link out (both ways), no node; a new leg walks
    // no link yet
    Trip refused = StartedTrip({MakeLeg(LegType::Walk, START, NODES[2], {0, 1, 2}, 30.0f),
                                MakeLeg(LegType::Walk, NODES[3], NODES[6], {3, 4, 5, 6}, 30.0f)});
    refused.next = 2;
    refused.walk.mode = TravelWalk::Mode::Refused;
    refused.walk.from = 0;
    refused.walk.to = 1;
    Check(TravelLegs::AfterFailure(refused, FailReason::NoPath) == Recovery::Replan && refused.avoidEdges.size() == 1 &&
              refused.avoidEdges[0] == std::pair<std::uint32_t, std::uint32_t>{0, 1} && refused.avoid.empty() &&
              !refused.noDirect && refused.replans == 1,
          "no path: the refused link 0 -> 1 is left out of the replan, no node");
    refused.next = 1;
    Check(TravelLegs::AfterFailure(refused, FailReason::NoPath) == Recovery::Replan && refused.avoidEdges.size() == 1,
          "no path on the hop from the bot's start: no edge to leave out");
    TravelLegs::StartLeg(refused, 1, 6000, NodeAt);
    Check(!refused.walk.Active() && refused.walk.from == TravelLegs::NO_NODE, "a new leg starts with no link walked");

    // WT56: a walk off the graph the navmesh refused (no_walk): the replan does not walk between its two points again
    // (the bot's start to the first node, the last node to the goal, the direct walk); no node or edge is left out. A
    // graph link between two nodes leaves nothing out (no_path's case); the fourth refusal ends the trip
    auto const isWalk = [](std::pair<Point, Point> const& walk, Point const& from, Point const& to)
    { return TravelRoute::Distance(walk.first, from) < 0.01f && TravelRoute::Distance(walk.second, to) < 0.01f; };
    Trip offGraph = StartedTrip({MakeLeg(LegType::Walk, START, NODES[2], {0, 1}, 30.0f)});
    Check(offGraph.waypoints.size() == 4 && offGraph.waypoints[0].node == TravelLegs::NO_NODE &&
              offGraph.waypoints[3].node == TravelLegs::NO_NODE,
          "no walk: the leg's start and end are off the graph");
    Check(TravelLegs::AfterFailure(offGraph, FailReason::NoWalk) == Recovery::Replan &&
              offGraph.avoidWalks.size() == 1 && isWalk(offGraph.avoidWalks[0], START, NODES[0]) &&
              offGraph.avoid.empty() && !offGraph.noDirect && offGraph.avoidEdges.empty() && offGraph.replans == 1,
          "no walk to the first node: the replan leaves the start -> node 0 walk out");
    offGraph.next = 3;
    Check(TravelLegs::AfterFailure(offGraph, FailReason::NoWalk) == Recovery::Replan &&
              offGraph.avoidWalks.size() == 2 && isWalk(offGraph.avoidWalks[1], NODES[1], NODES[2]) &&
              offGraph.avoid.empty(),
          "no walk from the last node to the goal: the replan leaves the node 1 -> goal walk out");
    offGraph.next = 2;
    Check(TravelLegs::AfterFailure(offGraph, FailReason::NoWalk) == Recovery::Replan &&
              offGraph.avoidWalks.size() == 2 && offGraph.avoidEdges.empty() && offGraph.replans == 3,
          "no walk on a graph link leaves nothing out");
    Check(TravelLegs::AfterFailure(offGraph, FailReason::NoWalk) == Recovery::Abort,
          "no walk after MAX_REPLANS replans ends the trip");
    Point const nearGoal{0, 58.0f, 0.0f, 0.0f};
    Trip straight = StartedTrip({MakeLeg(LegType::Walk, START, nearGoal, {}, 10.0f)});
    Check(TravelLegs::AfterFailure(straight, FailReason::NoWalk) == Recovery::Replan &&
              straight.avoidWalks.size() == 1 && isWalk(straight.avoidWalks[0], START, nearGoal) &&
              !straight.noDirect && straight.avoid.empty() && straight.replans == 1,
          "no walk on the direct walk: the replan leaves the start -> goal walk out");
    TravelRoute::Request avoiding;
    avoiding.avoidWalks = straight.avoidWalks;
    Check(avoiding.AvoidsWalk({0, START.x + 6.0f, 6.0f, 0.0f}, {0, nearGoal.x, 7.0f, 0.0f}) &&
              !avoiding.AvoidsWalk(nearGoal, START) &&
              !avoiding.AvoidsWalk(START, {0, nearGoal.x + 11.0f, 0.0f, 0.0f}) &&
              !avoiding.AvoidsWalk({1, START.x, 0.0f, 0.0f}, {1, nearGoal.x, 0.0f, 0.0f}),
          "an avoided walk: both ends within 10 yd, in its direction, on its map");
    Check(std::string(TravelLegs::FailWord(FailReason::NoWalk)) == "no_walk" &&
              std::string(TravelLegs::LegFailedSource(FailReason::NoWalk)) == "leg_failed:no_walk" &&
              std::string(TravelLegs::AbortSource(FailReason::NoWalk)) == "abort:no_walk",
          "no walk: its words");
    straight.piece.probe = TravelWalk::Probe::Walk;
    straight.piece.leg = 0;
    straight.piece.next = 1;
    straight.piece.orders.push_back({10.0f, 0.0f, 0.0f});
    TravelLegs::StartLeg(straight, 0, 7000, NodeAt);
    Check(!straight.piece.For(0, 1) && straight.piece.orders.empty() && straight.piece.probe == TravelWalk::Probe::None,
          "a new leg starts with no piece probed");
}

void CheckWrongMapAndFare()
{
    Trip walk = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[2], {0, 1, 2}, 30.0f)});
    Decision const wrong = Step(walk, At(NODES[4], 2000));
    Check(wrong.act == Act::Fail && wrong.reason == FailReason::WrongMap, "a walk on another map fails (wrong map)");

    Trip portal = StartedTrip({MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 3.0f)});
    Check(Step(portal, At({2, 0.0f, 0.0f, 0.0f}, 2000)).reason == FailReason::WrongMap,
          "a portal leg on a third map fails (wrong map)");

    Leg const flight = MakeLeg(LegType::Flight, NODES[5], NODES[6], {5, 6}, 200.0f, FARE);
    Trip poor = StartedTrip({flight});
    Decision const far = Step(poor, At(NODES[4], 2000, FARE - 1));
    Check(far.act == Act::Fail && far.reason == FailReason::Fare, "an unaffordable flight fails before the walk");
    Decision const there = Step(poor, At(NODES[5], 2000, FARE - 1));
    Check(there.act == Act::Fail && there.reason == FailReason::Fare && !poor.taxiActivated,
          "an unaffordable flight never reaches the flight master's talk");
    Trip rich = StartedTrip({flight});
    Check(Step(rich, At(NODES[4], 2000, FARE)).act == Act::Walk, "an affordable flight walks first");
    Check(Step(rich, At(NODES[5], 2000, FARE)).act == Act::TalkFlightMaster,
          "an affordable flight talks at the master");
}

void CheckPredicates()
{
    Point const sevenYards{0, 107.0f, 0.0f, 0.0f};
    // The goal is reached within 10 yd, a waypoint before it within 5 yd
    Trip last = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 20.0f)});
    Check(Go(last, At(sevenYards, 2000), NodeAt).act == Act::Arrived, "the goal counts at 7 yd");
    Trip middle = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 20.0f),
                               MakeLeg(LegType::Walk, NODES[1], NODES[2], {1, 2}, 20.0f)});
    Decision const notYet = Go(middle, At(sevenYards, 2000), NodeAt);
    Check(notYet.act == Act::Walk && middle.legIdx == 0, "a waypoint before the goal does not count at 7 yd");
    // A path past a node: the waypoint after the current one counts too
    Trip skip = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[2], {0, 1, 2}, 30.0f),
                             MakeLeg(LegType::Walk, NODES[2], {0, 300.0f, 0.0f, 0.0f}, {2}, 20.0f)});
    Go(skip, At(NODES[2], 2000), NodeAt);
    Check(skip.legIdx == 1, "reaching the waypoint after the current one passes both");

    // Portal across maps: done on the exit's map; within one map: done near the exit
    Trip cross = StartedTrip({MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 3.0f)});
    Check(Step(cross, At({0, 150.0f, 0.0f, 0.0f}, 2000)).act == Act::Walk, "portal: walk to its node first");
    Check(Step(cross, At(NODES[2], 2000)).act == Act::UseTrigger, "portal: trigger at its node");
    // On the way from the node to a trigger centre 8 yd further on: no turning back to the node
    Check(Step(cross, At({0, 208.0f, 0.0f, 0.0f}, 2500)).act == Act::UseTrigger,
          "portal: past its node the bot keeps heading for the trigger");
    Check(Step(cross, At({1, 1300.0f, 1300.0f, 0.0f}, 3000)).act == Act::Done,
          "portal: done anywhere on the exit's map");
    Trip inside = StartedTrip({MakeLeg(LegType::Portal, NODES[1], NODES[2], {1, 2}, 3.0f)});
    inside.triggered = true;
    Check(Step(inside, At({0, 230.0f, 0.0f, 0.0f}, 3000)).act == Act::Done,
          "portal within a map: done 30 yd from the exit");

    // Flight: done on landing within 30 yd; a landing elsewhere fails
    Leg const flight = MakeLeg(LegType::Flight, NODES[5], NODES[6], {5, 6}, 200.0f, FARE);
    Trip landed = StartedTrip({flight});
    landed.taxiActivated = true;
    Step(landed, At({1, 3000.0f, 3000.0f, 300.0f}, 2000, 0, true));
    Check(Step(landed, At({1, 5020.0f, 5000.0f, 0.0f}, 3000)).act == Act::Done,
          "flight: done on landing 20 yd from its end");
    Trip astray = StartedTrip({flight});
    astray.taxiActivated = true;
    Step(astray, At({1, 3000.0f, 3000.0f, 300.0f}, 2000, 0, true));
    Decision const elsewhere = Step(astray, At({1, 5100.0f, 5000.0f, 0.0f}, 3000));
    Check(elsewhere.act == Act::Fail && elsewhere.reason == FailReason::Landing, "flight: a landing 100 yd off fails");

    // The hearthstone leg: cast it where the bot stands, done within 40 yd of the bind (its map), the cast and a
    // cooldown wait are inside the timeout
    Point const bind{1, 1300.0f, 1300.0f, 0.0f};
    Trip hearth = StartedTrip({MakeLeg(LegType::Hearth, NODES[0], bind, {}, 10.0f)});
    Check(Step(hearth, At(NODES[0], 2000)).act == Act::UseHearth, "hearth: casts at the start");
    Check(Step(hearth, At({1, 1330.0f, 1300.0f, 0.0f}, 12000)).act == Act::Done,
          "hearth: done within 40 yd of the bind");
    Check(Step(hearth, At({1, 1345.0f, 1300.0f, 0.0f}, 12000)).reason == FailReason::WrongMap,
          "hearth: 45 yd away is not there; on the bind's map it is no start map either");
    Check(Step(hearth, At(NODES[0], 1000 + 40000)).act == Act::UseHearth, "hearth: 40 s are allowed");
    Decision const lateHearth = Step(hearth, At(NODES[0], 1000 + 40001));
    Check(lateHearth.act == Act::Fail && lateHearth.reason == FailReason::Timeout, "hearth: past 40 s fails");
    Trip cooling = StartedTrip({MakeLeg(LegType::Hearth, NODES[0], bind, {}, 130.0f)});
    Check(Step(cooling, At(NODES[0], 1000 + 150000)).act == Act::UseHearth,
          "hearth: a 2 minute cooldown wait is inside the timeout");
    Check(Step(cooling, At(NODES[0], 1000 + 160001)).reason == FailReason::Timeout,
          "hearth: the timeout is the leg's seconds plus 30 s");
    Trip hearthThenWalk = StartedTrip(
        {MakeLeg(LegType::Hearth, NODES[0], bind, {}, 10.0f), MakeLeg(LegType::Walk, bind, GOAL, {}, 50.0f)});
    Check(Go(hearthThenWalk, At(bind, 12000), NodeAt).act == Act::Walk && hearthThenWalk.legIdx == 1,
          "hearth: the walk after it starts on the bind's map");

    // A route of no-op walks only has arrived
    Trip none = StartedTrip({MakeLeg(LegType::Walk, GOAL, GOAL, {}, 0.0f)});
    Check(Go(none, At(GOAL, 2000), NodeAt).act == Act::Arrived, "a route of no-ops has arrived");
}

void CheckWaypointsAndAnswers()
{
    std::vector<TravelLegs::Waypoint> const points =
        TravelLegs::Waypoints(MakeLeg(LegType::Walk, START, GOAL, {0, 1}, 50.0f), NodeAt);
    Check(points.size() == 4 && points[1].node == 0 && points[2].node == 1 && points[3].node == TravelLegs::NO_NODE,
          "waypoints: start, node 0, node 1, goal");
    std::vector<TravelLegs::Waypoint> const onNodes =
        TravelLegs::Waypoints(MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 20.0f), NodeAt);
    Check(onNodes.size() == 2 && onNodes[0].node == 0 && onNodes[1].node == 1, "waypoints: ends on nodes merge");

    TravelLegs::RouteWait wait{7, 0, 0xFFFFF000u, true};  // the clock wraps while waiting
    Check(TravelLegs::Judge(wait, 6, 0, 0xFFFFF100u) == TravelLegs::Verdict::Drop, "an older answer is dropped");
    Check(TravelLegs::Judge(wait, 7, 0, 0x00001000u) == TravelLegs::Verdict::Apply, "a young answer is applied");
    Check(TravelLegs::Judge(wait, 7, 0, 0x00007000u) == TravelLegs::Verdict::Apply,
          "an answer 33 s old is applied (it lives 60 s)");
    Check(TravelLegs::Judge(wait, 7, 1, 0x00001000u) == TravelLegs::Verdict::Refresh, "an answer for another map");
    Check(TravelLegs::Judge(wait, 7, 0, 0x0000F000u) == TravelLegs::Verdict::Refresh, "an answer older than 60 s");
    Check(TravelLegs::NextDeadlineMs({}, 0x0000D000u, 0, &wait) >= 0 &&
              TravelLegs::NextDeadlineMs({}, 0x0000F000u, 0, &wait) < 0,
          "the wait expires after 60 s, when the service drops the answer");

    // A late or lost answer is asked again three times per trip; the fourth fails the leg (NoAnswer)
    Trip asking;
    for (int ask = 1; ask <= TravelLegs::MAX_RESUBMITS; ++ask)
        Check(TravelLegs::Resubmit(asking), "late answer " + std::to_string(ask) + " of 3 is asked again");
    Check(!TravelLegs::Resubmit(asking) && asking.replans == 0, "the fourth late answer fails the leg");
}

void CheckResetAndDeadline()
{
    // A far teleport (the portal's worldport) keeps the trip on its leg (StatusEnd::KeepsStatus, status-end); the kept
    // portal leg is done on the exit's map at the next tick
    Trip portal = StartedTrip(FullRoute());
    portal.legIdx = 2;  // on the portal leg
    Check(Step(portal, At({1, 1000.0f, 1000.0f, 0.0f}, 3000)).act == Act::Done,
          "the kept portal leg is done on the exit's map");

    // The whole trip's bound: estimate x 3 + 10 min; before the first answer the slack alone
    Check(TravelLegs::TripDeadlineMs(0.0f) == 600000u, "trip deadline: 10 min before a route");
    Check(TravelLegs::TripDeadlineMs(1000.0f) == 3600000u, "trip deadline: a 1000 s route gets 60 min");
    Check(TravelLegs::TripDeadlineMs(1.0e9f) > 3000000000u, "trip deadline: no overflow on a huge estimate");
}

void CheckFlightFailureAndDeadline()
{
    // A flight that cannot be taken (no master, undiscovered path, the core refuses) replans and reports its first hop
    Trip trip = StartedTrip(FullRoute());
    trip.legIdx = 4;  // the flight leg, nodes 5 -> 6
    auto const hop = TravelLegs::FirstFlightHop(trip);
    Check(hop.first == 5 && hop.second == 6, "taxi: the reported edge is the flight's first hop 5 -> 6");
    Check(!TravelLegs::HasWalkTarget(trip, true), "a flight leg has no walk target");
    trip.legIdx = 1;
    Check(TravelLegs::FirstFlightHop(trip).first == TravelLegs::NO_NODE, "taxi: a walk leg has no flight hop");
    Check(TravelLegs::HasWalkTarget(trip, false) &&
              TravelLegs::OwnerOf(trip, false, {}) == TravelLegs::TravelOwner::Follower,
          "a walk leg is walked; the passenger code does not own it");
    Trip noLeg;
    Check(TravelLegs::HasWalkTarget(noLeg, true) && !TravelLegs::HasWalkTarget(noLeg, false),
          "past the last leg only an inn errand walks");
    Check(TravelLegs::AfterFailure(trip, FailReason::Taxi) == TravelLegs::Recovery::Replan && trip.replans == 1,
          "taxi: a failed taxi start asks for a new route");
    Check(std::string(TravelLegs::FailWord(FailReason::Taxi)) == "taxi", "taxi: its word");

    // The deadline: counted from the current route, never in the air
    std::uint32_t const bound = TravelLegs::TripDeadlineMs(1000.0f);
    Check(!TravelLegs::PastDeadline(bound, 1000.0f), "deadline: not past at the bound");
    Check(TravelLegs::PastDeadline(bound + 1, 1000.0f), "deadline: past one ms after the bound");
    Check(!TravelLegs::PastDeadline(bound + 1, 2000.0f), "deadline: a replan's longer estimate moves the bound");
    Check(TravelLegs::PastDeadline(TravelLegs::TRIP_SLACK_MS + 1, 0.0f), "deadline: zero estimate keeps the slack");
}

// A static portal (WT55d), the portal object a player clicks: walk to its node, use the object there, wait for its
// teleport; done on the exit's map or near the exit within one map, Fail(portal) after the portal's 20 s
void CheckStaticPortal()
{
    Leg const leg = MakeLeg(LegType::StaticPortal, NODES[2], NODES[3], {2, 3}, 3.0f);
    Check(!TravelLegs::IsNoOp(leg) && TravelLegs::LegTimeoutMs(leg) == TravelLegs::PORTAL_TIMEOUT_MS,
          "static portal: no no-op, the portal's timeout");
    Check(std::string(TravelLegs::ActWord(Act::UseObject)) == "use", "static portal: the act's word");
    Trip cross = StartedTrip({leg});
    Check(Step(cross, At({0, 150.0f, 0.0f, 0.0f}, 2000)).act == Act::Walk, "static portal: walk to its node first");
    Decision const use = Step(cross, At(NODES[2], 2000));
    Check(use.act == Act::UseObject && use.target.x == NODES[2].x, "static portal: use the object at its node");
    Check(Step(cross, At({0, 208.0f, 0.0f, 0.0f}, 2500)).act == Act::UseObject,
          "static portal: past its node the bot keeps heading for the object");
    auto const edge = TravelLegs::StuckEdge(cross);
    Check(edge.first == 2 && edge.second == 3, "static portal: its edge is node 2 -> node 3");
    cross.triggered = true;
    Check(Step(cross, At(NODES[2], 3000)).act == Act::Wait, "static portal: waits once used");
    Check(Step(cross, At({1, 1300.0f, 1300.0f, 0.0f}, 4000)).act == Act::Done,
          "static portal: done anywhere on the exit's map");
    Trip inside = StartedTrip({MakeLeg(LegType::StaticPortal, NODES[1], NODES[2], {1, 2}, 3.0f)});
    inside.triggered = true;
    Check(Step(inside, At(NODES[1], 3000)).act == Act::Wait, "static portal within a map: not done at its own node");
    Check(Step(inside, At({0, 230.0f, 0.0f, 0.0f}, 3000)).act == Act::Done,
          "static portal within a map: done 30 yd from the exit");
    Trip stuck = StartedTrip({leg}, 0);
    stuck.triggered = true;
    Decision const late = Step(stuck, At(NODES[2], 21000));
    Check(late.act == Act::Fail && late.reason == FailReason::Portal, "static portal: Fail(portal) after 20 s");
    Trip wrong = StartedTrip({leg});
    Decision const away = Step(wrong, At({530, 0.0f, 0.0f, 0.0f}, 2000));
    Check(away.act == Act::Fail && away.reason == FailReason::WrongMap, "static portal: Fail(wrong_map) elsewhere");
    // Through and on: the far side's walk follows
    Trip through = StartedTrip({leg, MakeLeg(LegType::Walk, NODES[3], NODES[4], {3, 4}, 15.0f)});
    through.triggered = true;
    Decision const after = Go(through, At(NODES[3], 3000), NodeAt);
    Check(through.legIdx == 1 && after.act == Act::Walk, "static portal: the walk on the far side follows");
}

void CheckEventSources()
{
    bool legs = true;
    for (LegType const type : {LegType::Walk, LegType::Portal, LegType::Flight, LegType::Transport, LegType::Hearth,
                               LegType::Fly, LegType::StaticPortal})
        legs = legs && std::string(TravelLegs::LegDoneSource(type)) ==
                           std::string("leg_done:") + TravelRoute::LegTypeWord(type);
    Check(legs, "events: leg_done:<leg type> for every leg type");
    bool reasons = true;
    for (int r = 0; r <= static_cast<int>(FailReason::CrossingFailed); ++r)
    {
        FailReason const reason = static_cast<FailReason>(r);
        std::string const word = TravelLegs::FailWord(reason);
        reasons = reasons && word != "?" && TravelLegs::LegFailedSource(reason) == "leg_failed:" + word &&
                  TravelLegs::AbortSource(reason) == "abort:" + word;
    }
    Check(reasons, "events: leg_failed:<reason> and abort:<reason> for every reason");
    // The trip's outcome of a failed leg (RpgStatus::End), one line per reason
    using StatusEnd::Outcome;
    std::pair<FailReason, Outcome> const outcomes[] = {
        {FailReason::None, Outcome::Abandoned},
        {FailReason::Timeout, Outcome::TimedOut},
        {FailReason::WrongMap, Outcome::Abandoned},
        {FailReason::Stuck, Outcome::Stuck},
        {FailReason::Portal, Outcome::Abandoned},
        {FailReason::Fare, Outcome::Abandoned},
        {FailReason::Landing, Outcome::Abandoned},
        {FailReason::Unsupported, Outcome::Abandoned},
        {FailReason::NoAnswer, Outcome::Abandoned},
        {FailReason::NoRoute, Outcome::Abandoned},
        {FailReason::Taxi, Outcome::Abandoned},
        {FailReason::Hearth, Outcome::Abandoned},
        {FailReason::OffMesh, Outcome::Stuck},
        {FailReason::Death, Outcome::Abandoned},
        {FailReason::TransportLost, Outcome::Abandoned},
        {FailReason::NoExit, Outcome::Abandoned},
        {FailReason::Takeoff, Outcome::Abandoned},
        {FailReason::NoPath, Outcome::Abandoned},
        {FailReason::NoWalk, Outcome::Abandoned},
        {FailReason::NoCrossing, Outcome::Abandoned},
        {FailReason::CrossingFailed, Outcome::Abandoned},
    };
    static_assert(sizeof(outcomes) / sizeof(outcomes[0]) == static_cast<std::size_t>(FailReason::CrossingFailed) + 1,
                  "every fail reason has its outcome line");
    for (auto const& [reason, outcome] : outcomes)
        Check(TravelLegs::FailOutcome(reason) == outcome, std::string("outcome: ") + TravelLegs::FailWord(reason) +
                                                              " ends the trip as " + StatusEnd::Word(outcome));
    Check(std::string(TravelLegs::LegFailedSource(FailReason::OffMesh)) == "leg_failed:off_mesh" &&
              std::string(TravelLegs::AbortSource(FailReason::OffMesh)) == "abort:off_mesh",
          "events: off_mesh words");
    Check(std::string(TravelLegs::LegFailedSource(FailReason::Death)) == "leg_failed:death" &&
              std::string(TravelLegs::AbortSource(FailReason::Death)) == "abort:death",
          "events: death words");
}

// Deaths (PlayerbotAI's engine changes, NoteLife): the second on a leg fails it as death and the next route leaves its
// edge out; the third on the trip ends it. A leg's combat seconds and mount word
void CheckDeathsAndRecord()
{
    using TravelLegs::Life;
    using TravelLegs::Recovery;
    Trip trip = StartedTrip(FullRoute());
    Check(Go(trip, At(NODES[1], 2000), NodeAt).act == Act::Walk && trip.next == 2, "deaths: the walk heads for node 2");
    TravelLegs::NoteLife(trip, Life::Combat, 3000);
    TravelLegs::NoteLife(trip, Life::Calm, 8000);
    TravelLegs::NoteLife(trip, Life::Combat, 10000);
    Check(TravelLegs::CombatSeconds(trip.record, 12000) == 7.0f, "record: 5 s of ended and 2 s of running combat");
    TravelLegs::NoteLife(trip, Life::Dead, 13000);
    Check(trip.record.combatSinceMs == 0 && trip.record.combatMs == 8000 && trip.record.deaths == 1 && trip.deaths == 1,
          "record: a death ends the combat stretch and counts once");
    Check(!TravelLegs::DiedTooOften(trip), "deaths: one death on the leg goes on");
    TravelLegs::NoteLife(trip, Life::Calm, 60000);  // revived
    TravelLegs::NoteLife(trip, Life::Dead, 70000);
    Check(TravelLegs::DiedTooOften(trip), "deaths: the second death on the leg fails it");
    Check(TravelLegs::AfterFailure(trip, FailReason::Death) == Recovery::Replan && trip.avoidEdges.size() == 1 &&
              trip.avoidEdges[0].first == 1 && trip.avoidEdges[0].second == 2 && trip.record.deaths == 0 &&
              trip.deaths == 2 && trip.replans == 1,
          "deaths: a replan that leaves edge 1 -> 2 out");
    Check(!TravelLegs::DiedTooOften(trip), "deaths: the failed leg's count ends with it");
    trip.legs = FullRoute();
    TravelLegs::StartLeg(trip, 0, 80000, NodeAt);
    TravelLegs::NoteLife(trip, Life::Dead, 90000);
    Check(trip.record.deaths == 1 && trip.deaths == 3 && TravelLegs::DiedTooOften(trip),
          "deaths: the third on the trip fails the leg");
    Check(TravelLegs::AfterFailure(trip, FailReason::Death) == Recovery::Abort, "deaths: the third ends the trip");

    Trip waiting;
    TravelLegs::NoteLife(waiting, Life::Dead, 1000);
    Check(waiting.deaths == 1 && waiting.record.deaths == 0, "deaths: between routes only the trip counts");
    Trip fromStart = StartedTrip({MakeLeg(LegType::Walk, START, NODES[1], {0, 1}, 20.0f)});
    Check(TravelLegs::AfterFailure(fromStart, FailReason::Death) == Recovery::Replan && fromStart.avoidEdges.empty() &&
              fromStart.avoid.size() == 1 && fromStart.avoid[0] == 0,
          "deaths: on the hop from the bot's start node 0 is no start of the next route");

    Trip riding = StartedTrip(FullRoute());
    Check(std::string(TravelLegs::MountWord(riding.record)) == "foot", "record: no tick: foot");
    TravelLegs::NoteTick(riding, true);
    TravelLegs::NoteTick(riding, true);
    Check(std::string(TravelLegs::MountWord(riding.record)) == "mounted", "record: mounted on every tick");
    TravelLegs::NoteTick(riding, false);
    Check(std::string(TravelLegs::MountWord(riding.record)) == "part", "record: mounted on some ticks: part");
    TravelLegs::NoteLife(riding, Life::Combat, 5000);
    TravelLegs::StartLeg(riding, 1, 6000, NodeAt);
    Check(riding.record.ticks == 0 && riding.record.combatSinceMs == 6000 &&
              TravelLegs::CombatSeconds(riding.record, 7000) == 1.0f,
          "record: a new leg starts anew, a running combat counts from the leg change");
}

// A fly leg on map 1: takeoff at z 10, flight points at 80, 80, 90, landing at z 12 600 yd on; 40 s, clearance 30 yd
Leg FlyLeg()
{
    Leg leg = MakeLeg(LegType::Fly, {1, 0.0f, 0.0f, 10.0f}, {1, 600.0f, 0.0f, 12.0f}, {}, 40.0f);
    leg.flightPoints = {{1, 0.0f, 0.0f, 10.0f},
                        {1, 100.0f, 0.0f, 80.0f},
                        {1, 300.0f, 0.0f, 80.0f},
                        {1, 500.0f, 0.0f, 90.0f},
                        {1, 600.0f, 0.0f, 12.0f}};
    leg.flightClearance = 30.0f;
    return leg;
}

Observation Flying(Point bot, std::uint32_t nowMs, bool mounted, bool flying)
{
    Observation seen = At(bot, nowMs);
    seen.flyingMount = mounted;
    seen.flying = flying;
    return seen;
}

// A fly leg's tries under the one mount owner (WT44): MountToFly's dispatch (MountRules::Policy for Kind::Flying, then
// TravelLegs::TakeFlyTry, the same call MountToFly makes) against StepLeg's takeoff rule
void CheckFlyLegMountOwner()
{
    using MountRules::FlyTry;
    Leg const leg = FlyLeg();
    Point const takeoff = leg.from;
    auto const tryOnce = [](Trip& trip, MountRules::Facts const& facts, bool settled, std::uint32_t nowMs)
    { return TravelLegs::TakeFlyTry(trip, MountRules::Policy(facts, MountRules::Kind::Flying), settled, nowMs); };

    MountRules::Facts noMount;
    noMount.noMount = true;
    Trip test = StartedTrip({leg}, 0);
    bool refused = true;
    for (std::uint32_t const at : {1000u, 6000u, 11000u})
        refused = refused && Step(test, Flying(takeoff, at, false, false)).act == Act::Mount &&
                  tryOnce(test, noMount, true, at) == FlyTry::Refused;
    Decision const stopped = Step(test, Flying(takeoff, 16000, false, false));
    Check(refused && test.mountTries == 3 && stopped.act == Act::Fail && stopped.reason == FailReason::Takeoff &&
              std::string(test.mountWhy) == "nomount",
          "fly owner: nomount refuses every try: the leg fails as takeoff with the word nomount");

    MountRules::Facts onFoot;
    onFoot.onFoot = true;
    Trip foot = StartedTrip({leg}, 0);
    Check(tryOnce(foot, onFoot, true, 1000) == FlyTry::Refused && foot.mountTries == 1 &&
              std::string(foot.mountWhy) == "on_foot",
          "fly owner: the on-foot rule refuses a counted try with its word");

    MountRules::Facts lootNear;
    lootNear.lootNear = true;
    Trip loot = StartedTrip({leg}, 0);
    bool held = true;
    for (std::uint32_t at = 1000; at <= 140000; at += 1000)
        held = held && Step(loot, Flying(takeoff, at, false, false)).act == Act::Mount &&
               tryOnce(loot, lootNear, true, at) == FlyTry::Wait;
    Decision const late = Step(loot, Flying(takeoff, 141000, false, false));
    Check(held && loot.mountTries == 0 && late.act == Act::Fail && late.reason == FailReason::Timeout,
          "fly owner: a corpse in reach holds the try uncounted; the leg's timeout (140 s) ends the hold");

    Trip settle = StartedTrip({leg}, 0);
    Check(tryOnce(settle, MountRules::Facts{}, false, 1000) == FlyTry::Settle && settle.mountTries == 0 &&
              tryOnce(settle, MountRules::Facts{}, true, 2000) == FlyTry::Cast && settle.mountTries == 1 &&
              *settle.mountWhy == 0,
          "fly owner: a moving or sitting bot settles first (uncounted), then a free bot casts (a real try)");
}

// An indoor landing (WU11b, Golyn at Garadar's inn): checked once near the end, the landing moves to the outdoor
// point nearest to it back along the flight, the descent point before it, and a walk leg in to the planned end follows
void CheckLanding()
{
    Trip trip = StartedTrip({FlyLeg()}, 0);
    Check(!TravelLegs::NeedsLandingCheck(trip, {1, 590.0f, 0.0f, 90.0f}), "landing: checked before the descent");
    trip.next = 5;  // heading for the descent point
    trip.seenInFlight = true;
    Check(!TravelLegs::NeedsLandingCheck(trip, {1, 300.0f, 0.0f, 90.0f}), "landing: checked 300 yd out");
    Check(TravelLegs::NeedsLandingCheck(trip, {1, 500.0f, 0.0f, 90.0f}), "landing: not checked on the descent");
    std::vector<Point> const line = TravelLegs::LandingLine(trip);
    Check(line.size() == 16 && line.back().x == 600.0f && std::fabs(line.front().x - 540.0f) < 0.01f &&
              line[1].x > line[0].x && line.front().z == 12.0f,
          "landing: the line does not run from 60 yd back to the landing, 4 yd apart, at its height");
    auto const house = [](Point const& at) { return at.x < 590.0f; };  // indoors within 10 yd of the landing
    std::size_t const index = TravelLegs::LandingIndex(line, house);
    Check(index + 1 < line.size() && std::fabs(line[index].x - 588.0f) < 0.01f,
          "landing: indoors, not the outdoor point nearest to the landing");
    Check(TravelLegs::LandingIndex(line, [](Point const&) { return true; }) + 1 == line.size() &&
              TravelLegs::LandingIndex(line, [](Point const&) { return false; }) + 1 == line.size(),
          "landing: outdoors or no outdoor point, not the planned landing");

    Trip kept = trip;
    Check(!TravelLegs::MoveLanding(kept, kept.waypoints.back().at) && kept.landingChecked && kept.legs.size() == 1 &&
              !TravelLegs::NeedsLandingCheck(kept, {1, 595.0f, 0.0f, 20.0f}),
          "landing: the planned landing changed the leg or is checked again");
    Point const outside{1, line[index].x, 0.0f, 11.0f};
    Check(TravelLegs::MoveLanding(trip, outside) && trip.legs.size() == 2 && trip.legs[0].to.x == outside.x &&
              trip.legs[0].flightPoints.back().x == outside.x && trip.waypoints.back().at.x == outside.x &&
              std::fabs(trip.waypoints[5].at.x - (outside.x - 30.0f)) < 0.01f && trip.waypoints[5].at.z == 90.0f &&
              trip.legs[1].type == LegType::Walk && trip.legs[1].from.x == outside.x && trip.legs[1].to.x == 600.0f,
          "landing: not moved 12 yd back with the descent before it and a walk in");
    Decision const in = Go(trip, Flying({1, outside.x + 1.0f, 0.0f, 11.0f}, 30000, true, false), NodeAt);
    Check(trip.legIdx == 1 && in.act == Act::Walk && in.target.x == 600.0f,
          "landing: down at the moved landing, the bot does not walk in");
}

// Fly legs: the derived climb and descent points, the takeoff (mount, tried again every 5 s), the waypoints in the
// air, the real-height check (a building raises the next waypoint, capped, once per waypoint, never the landing), the
// landing at the end (Done) or away from it (Landing), the leg's timeout, the takeoff's failures (at once where the bot
// cannot take off, after FLY_MOUNT_TRIES real mount tries; a skipped try does not count), a wrong map, and the replan
// after a failed fly leg without flying (noFly)
void CheckFlyLeg()
{
    using TravelLegs::Recovery;
    using TravelLegs::RequiredZ;
    Leg const leg = FlyLeg();
    std::vector<TravelLegs::Waypoint> const points = TravelLegs::Waypoints(leg, NodeAt);
    auto const about = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
    Check(points.size() == 7 && points[0].at.z == 10.0f && about(points[1].at.x, 30.0f) && points[1].at.z == 80.0f &&
              points[2].at.x == 100.0f && about(points[5].at.x, 570.0f) && points[5].at.z == 90.0f &&
              points[6].at.x == 600.0f && points[6].at.z == 12.0f,
          "fly: takeoff, a climb point 30 yd on at the first height, the flight, a descent point 30 yd before the "
          "landing at the last height, the landing");
    Leg bare = leg;
    bare.flightPoints.clear();
    Check(TravelLegs::Waypoints(bare, NodeAt).size() == 2, "fly: without a planned flight, its two ends");
    Check(TravelLegs::LegTimeoutMs(leg) == 140 * 1000, "fly: timeout 40 s x 2 + 60 s");

    Check(RequiredZ({}, 80.0f, 30.0f) == 80.0f, "height: no samples, no raise");
    Check(RequiredZ({60.0f}, 80.0f, 30.0f) == 80.0f, "height: 20 yd above the ground ahead is enough");
    Check(RequiredZ({70.0f}, 80.0f, 30.0f) == 100.0f, "height: 10 yd above a roof climbs to the roof + 30 yd");
    Check(RequiredZ({70.0f}, 80.0f, 5.0f) == 85.0f, "height: the clearance is at least 15 yd");
    Check(RequiredZ({5000.0f}, 80.0f, 30.0f) == 330.0f, "height: a raise is capped 250 yd above the plan");
    Check(RequiredZ({-200000.0f, std::numeric_limits<float>::quiet_NaN()}, 80.0f, 30.0f) == 80.0f,
          "height: invalid samples are no ground");

    // Take off, climb, fly with a building in the way, land at the end
    Trip trip = StartedTrip({leg}, 0);
    Point const takeoff = leg.from;
    Check(Step(trip, Flying(takeoff, 1000, false, false)).act == Act::Mount, "fly: on foot it mounts first");
    TravelLegs::NoteMountTry(trip, 1000);
    Check(Step(trip, Flying(takeoff, 3000, false, false)).act == Act::Wait, "fly: no second mount cast within 5 s");
    Check(Step(trip, Flying(takeoff, 6000, false, false)).act == Act::Mount, "fly: the mount is tried again after 5 s");
    TravelLegs::NoteMountTry(trip, 6000);
    Decision const up = Step(trip, Flying(takeoff, 7000, true, false));
    Check(up.act == Act::Fly && about(up.target.x, 30.0f) && up.target.z == 80.0f,
          "fly: mounted, it climbs to the climb point");
    Check(TravelLegs::NeedsHeightCheck(trip), "fly: the height check is due for the climb point");
    Check(!TravelLegs::CheckHeight(trip, {20.0f, 40.0f}) && !TravelLegs::NeedsHeightCheck(trip) && trip.climbs == 0,
          "fly: open ground below: no raise, one check per waypoint");
    Decision const air = Step(trip, Flying({1, 28.0f, 0.0f, 75.0f}, 9000, true, true));
    Check(air.act == Act::Fly && air.target.x == 100.0f && trip.seenInFlight && trip.peakZ == 75.0f,
          "fly: past the climb point it heads for the first flight point");
    Check(TravelLegs::CheckHeight(trip, {60.0f, 110.0f, TravelLegs::FLY_NO_GROUND - 1.0f}) && trip.climbs == 1 &&
              trip.waypoints[trip.next].at.z == 140.0f,
          "fly: a 110 yd building ahead raises the waypoint to 110 + 30 yd");
    Check(!TravelLegs::CheckHeight(trip, {500.0f}) && trip.climbs == 1, "fly: a waypoint is checked once");
    Decision const over = Step(trip, Flying({1, 100.0f, 0.0f, 138.0f}, 20000, true, true));
    Check(over.act == Act::Fly && over.target.x == 300.0f && trip.peakZ == 138.0f,
          "fly: over the building, on to the next flight point");
    Decision const past = Step(trip, Flying({1, 500.0f, 0.0f, 90.0f}, 40000, true, true));
    Check(past.act == Act::Fly && past.target.x == 570.0f && past.target.z == 90.0f,
          "fly: a waypoint passed counts, then the descent point");
    Trip atLanding = trip;
    atLanding.next = 6;
    Check(!TravelLegs::NeedsHeightCheck(atLanding), "fly: the landing is never raised");
    Check(Step(trip, Flying({1, 600.0f, 0.0f, 13.0f}, 50000, true, false)).act == Act::Done,
          "fly: the landing reached: done");

    // Down near the end (within 30 yd) is done; down elsewhere after the takeoff fails as landing
    Trip near = StartedTrip({leg}, 0);
    Step(near, Flying({1, 50.0f, 0.0f, 80.0f}, 5000, true, true));
    Check(Step(near, Flying({1, 590.0f, 10.0f, 12.0f}, 30000, true, false)).act == Act::Done,
          "fly: down within 30 yd of the end: done");
    Trip fell = StartedTrip({leg}, 0);
    Step(fell, Flying({1, 50.0f, 0.0f, 80.0f}, 5000, true, true));
    Decision const down = Step(fell, Flying({1, 300.0f, 0.0f, 10.0f}, 20000, false, false));
    Check(down.act == Act::Fail && down.reason == FailReason::Landing, "fly: down away from the end fails as landing");
    Check(TravelLegs::AfterFailure(fell, FailReason::Landing) == Recovery::Replan && fell.noFly && fell.replans == 1,
          "fly: a failed fly leg replans without flying");

    // The leg's timeout in the air; the takeoff's failures; a wrong map
    Trip slow = StartedTrip({leg}, 0);
    Step(slow, Flying({1, 50.0f, 0.0f, 80.0f}, 5000, true, true));
    Decision const late = Step(slow, Flying({1, 300.0f, 50.0f, 80.0f}, 141000, true, true));
    Check(late.act == Act::Fail && late.reason == FailReason::Timeout, "fly: past 140 s in the air: timeout");
    Trip tries = StartedTrip({leg}, 0);
    bool triedThrice = true;
    for (std::uint32_t const at : {1000u, 6000u, 11000u})
    {
        triedThrice = triedThrice && Step(tries, Flying(takeoff, at, false, false)).act == Act::Mount;
        TravelLegs::NoteMountTry(tries, at);
    }
    Check(triedThrice && tries.mountTries == 3, "fly: three real mount tries 5 s apart");
    Check(Step(tries, Flying(takeoff, 13000, false, false)).act == Act::Wait, "fly: the third try's cast gets its 5 s");
    Decision const unmounted = Step(tries, Flying(takeoff, 16000, false, false));
    Check(unmounted.act == Act::Fail && unmounted.reason == FailReason::Takeoff,
          "fly: no flying mount after three real tries: takeoff");
    Trip skipped = StartedTrip({leg}, 0);
    bool askedAgain = true;
    for (std::uint32_t at = 1000; at <= 61000; at += 1000)
        askedAgain = askedAgain && Step(skipped, Flying(takeoff, at, false, false)).act == Act::Mount;
    Check(askedAgain && skipped.mountTries == 0, "fly: tries the glue skipped (combat, a cast) do not count");
    Trip inside = StartedTrip({leg}, 0);
    Observation indoors = Flying(takeoff, 1000, false, false);
    indoors.takeoffHere = false;
    Decision const noTakeoff = Step(inside, indoors);
    Check(noTakeoff.act == Act::Fail && noTakeoff.reason == FailReason::Takeoff && inside.mountTries == 0,
          "fly: no takeoff where the bot stands (indoors, no-fly area, water) fails at once");
    Check(TravelLegs::AfterFailure(inside, FailReason::Takeoff, true) == Recovery::Replan && !inside.noFly &&
              inside.replans == 0 && inside.spotFails == 1,
          "fly: a takeoff refused where the bot stands replans and keeps flying");
    Check(TravelLegs::AfterFailure(tries, FailReason::Takeoff) == Recovery::Replan && tries.noFly,
          "fly: a takeoff without a flying mount replans without flying");
    Trip city = StartedTrip({leg}, 0);
    bool keptFlying = true;
    for (std::uint8_t i = 0; i < TravelLegs::MAX_SPOT_FAILS; ++i)
        keptFlying = keptFlying && TravelLegs::AfterFailure(city, FailReason::Takeoff, true) == Recovery::Replan;
    Check(keptFlying && !city.noFly && city.replans == 0,
          "fly: refused takeoffs where the bot stands replan flying outside the replan budget (closed city nodes)");
    Check(TravelLegs::AfterFailure(city, FailReason::Takeoff, true) == Recovery::Replan && city.noFly &&
              city.replans == 1 && city.spotFails == TravelLegs::MAX_SPOT_FAILS,
          "fly: a spot failure past MAX_SPOT_FAILS replans without flying and spends a replan");
    Trip spent = StartedTrip({leg}, 0);
    spent.replans = TravelLegs::MAX_REPLANS;
    Check(TravelLegs::AfterFailure(spent, FailReason::Takeoff, true) == Recovery::Replan && !spent.noFly,
          "fly: a spot failure replans flying after the replans are spent");
    spent.spotFails = TravelLegs::MAX_SPOT_FAILS;
    Check(TravelLegs::AfterFailure(spent, FailReason::Takeoff, true) == Recovery::Abort,
          "fly: with both bounds spent the trip ends");
    // The trip's route requests keep the spots where the bot could not take off (WT61b review: the snapshot closes
    // only the spot of the moment, so two refused spots took turns)
    Trip spots = StartedTrip({leg}, 0);
    TravelRoute::Snapshot grounded;
    grounded.position = {1, 0.0f, 0.0f, 80.0f};
    grounded.takeoffHere = false;
    Point const far{1, 900.0f, 0.0f, 80.0f};
    TravelRoute::Request const asked = TravelLegs::NextRequest(spots, grounded, far);
    Check(spots.noTakeoffs.size() == 1 && asked.noTakeoffs.size() == 1 && asked.NoTakeoffAt(grounded.position) &&
              asked.NoTakeoffAt({1, 6.0f, 0.0f, 80.0f}) && !asked.NoTakeoffAt({1, 30.0f, 0.0f, 80.0f}) &&
              !asked.NoTakeoffAt({0, 0.0f, 0.0f, 80.0f}),
          "fly: a route asked where the bot cannot take off closes that spot for the trip");
    TravelLegs::NextRequest(spots, grounded, far);
    Check(spots.noTakeoffs.size() == 1, "fly: the same spot is kept once");
    grounded.position.x = 30.0f;
    TravelRoute::Request const second = TravelLegs::NextRequest(spots, grounded, far);
    Check(second.noTakeoffs.size() == 2 && second.NoTakeoffAt({1, 0.0f, 0.0f, 80.0f}) &&
              second.NoTakeoffAt(grounded.position),
          "fly: the next refused spot joins the first, which stays closed");
    grounded.position.x = 60.0f;
    grounded.takeoffHere = true;
    Check(TravelLegs::NextRequest(spots, grounded, far).noTakeoffs.size() == 2 && spots.noTakeoffs.size() == 2,
          "fly: a spot the bot may take off at is not kept");
    grounded.takeoffHere = false;
    for (std::uint32_t i = 0; i < 2 * TravelLegs::NO_TAKEOFF_SPOTS; ++i)
    {
        grounded.position.x += 30.0f;
        TravelLegs::NextRequest(spots, grounded, far);
    }
    Check(spots.noTakeoffs.size() == TravelLegs::NO_TAKEOFF_SPOTS, "fly: the kept spots are bounded");
    spots.noFly = true;
    spots.noDirect = true;
    spots.avoid = {3};
    spots.avoidEdges = {{7, 9}};
    spots.avoidWalks = {{grounded.position, far}};
    TravelRoute::Request const carried = TravelLegs::NextRequest(spots, grounded, far);
    Check(carried.noFly && carried.noDirect && carried.avoidNodes == spots.avoid &&
              carried.avoidEdges == spots.avoidEdges && carried.avoidWalks.size() == 1 &&
              carried.snapshot.position.x == grounded.position.x && carried.goal.x == far.x,
          "a trip's route request carries what its failed legs left out");
    // A failed fly leg over an air link (WT61): the trip's later routes leave the link out, a refused takeoff too
    Leg airLeg = leg;
    airLeg.air = true;
    airLeg.nodes = {7, 9};
    std::pair<std::uint32_t, std::uint32_t> const airEdge{7, 9};
    Trip airSpot = StartedTrip({airLeg}, 0);
    Check(TravelLegs::AfterFailure(airSpot, FailReason::Takeoff, true) == Recovery::Replan && !airSpot.noFly &&
              airSpot.avoidEdges.size() == 1 && airSpot.avoidEdges.front() == airEdge,
          "fly: a takeoff refused at an air link's node keeps flying and leaves the link out of the trip");
    Check(TravelLegs::AfterFailure(airSpot, FailReason::Takeoff, true) == Recovery::Replan &&
              airSpot.avoidEdges.size() == 1,
          "fly: the same air link is left out once");
    Trip airLost = StartedTrip({airLeg}, 0);
    Check(TravelLegs::AfterFailure(airLost, FailReason::Landing) == Recovery::Replan && airLost.noFly &&
              airLost.avoidEdges.size() == 1 && airLost.avoidEdges.front() == airEdge,
          "fly: an air leg failed in the air replans without flying and without its link");
    Trip airWait = StartedTrip({airLeg}, 0);
    TravelLegs::AfterFailure(airWait, FailReason::NoAnswer);
    Check(airWait.avoidEdges.empty(), "fly: a route without an answer leaves no air link out");
    Check(inside.avoidEdges.empty() && city.avoidEdges.empty() && fell.avoidEdges.empty(),
          "fly: a failed fly leg of the bot's own leaves no edge out");
    Trip overCity = StartedTrip({leg}, 0);
    Step(overCity, Flying({1, 50.0f, 0.0f, 80.0f}, 5000, true, true));
    Observation aloft = Flying({1, 150.0f, 0.0f, 80.0f}, 9000, true, true);
    aloft.takeoffHere = false;
    Check(Step(overCity, aloft).act == Act::Fly, "fly: in the air the spot below does not matter");
    Trip moved = StartedTrip({leg}, 0);
    Decision const away = Step(moved, Flying({0, 50.0f, 0.0f, 80.0f}, 5000, true, true));
    Check(away.act == Act::Fail && away.reason == FailReason::WrongMap, "fly: on another map: wrong map");

    // noFly only after a fly leg's own failure; a stuck report on a fly leg avoids no node
    Trip walking = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[2], {0, 1, 2}, 30.0f)});
    Check(TravelLegs::AfterFailure(walking, FailReason::Timeout) == Recovery::Replan && !walking.noFly,
          "fly: a failed walk keeps flying");
    Trip unanswered = StartedTrip({leg}, 0);
    TravelLegs::AfterFailure(unanswered, FailReason::NoAnswer);
    Check(!unanswered.noFly, "fly: a late route answer keeps flying");
    Trip stuck = StartedTrip({leg}, 0);
    Check(!TravelLegs::NoteStuck(stuck) && stuck.avoid.empty() && !stuck.noDirect,
          "fly: a stuck report on a fly leg avoids nothing");
    Check(std::string(TravelLegs::LegDoneSource(LegType::Fly)) == "leg_done:fly" &&
              std::string(TravelLegs::LegFailedSource(FailReason::Takeoff)) == "leg_failed:takeoff" &&
              std::string(TravelLegs::AbortSource(FailReason::Takeoff)) == "abort:takeoff",
          "fly: event words");
}

void CheckPlaces()
{
    auto const thunderBluff = TravelPlaces::Resolve("Thunder Bluff");
    Check(thunderBluff && thunderBluff->mapId == 1, "places: \"Thunder Bluff\" names thunderbluff on Kalimdor");
    auto const stormwind = TravelPlaces::Resolve("STORMWIND");
    Check(stormwind && stormwind->mapId == 0, "places: names ignore case");
    Check(TravelPlaces::Key("Sentinel Hill") == "sentinelhill", "places: names ignore spaces");
    auto const literal = TravelPlaces::Resolve(" -8825.5, 630.25 ,97.75, 0");
    Check(literal && literal->mapId == 0 && literal->x == -8825.5f && literal->y == 630.25f && literal->z == 97.75f,
          "places: a literal x,y,z,map");
    auto const outland = TravelPlaces::Resolve("1,2,3,530");
    Check(outland && outland->mapId == 530, "places: a literal's map");
    Check(!TravelPlaces::Resolve("1,2,3"), "places: a literal needs four numbers");
    Check(!TravelPlaces::Resolve("1,2,3,0x"), "places: nothing after the map");
    Check(!TravelPlaces::Resolve("1,2,3,-1"), "places: no negative map");
    Check(!TravelPlaces::Resolve("atlantis"), "places: an unknown name");
    Check(!TravelPlaces::Resolve("nan,0,0,0") && !TravelPlaces::Resolve("0,inf,0,0") &&
              !TravelPlaces::Resolve("0,0,-infinity,0"),
          "places: a literal with a non-finite coordinate");
    Check(!TravelPlaces::Resolve(""), "places: empty text");

    bool starts = true;
    bool unique = true;
    bool bothSides = false;
    bool bothTeams[2] = {false, false};
    auto const& all = TravelPlaces::All();
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        TravelPlaces::Place const* from = TravelPlaces::Find(all[i].testFrom);
        starts = starts && from && from != &all[i] && TravelPlaces::Find(all[i].name) == &all[i];
        for (std::size_t j = i + 1; j < all.size(); ++j)
            unique = unique && TravelPlaces::Key(all[i].name) != TravelPlaces::Key(all[j].name);
        bothSides = bothSides || all[i].at.mapId == 1;
        bothTeams[all[i].testTeam & 1] = true;
    }
    Check(starts, "places: every place finds itself by name and has another place as its test start");
    Check(unique, "places: names are unique");
    Check(bothSides && all.front().at.mapId == 0 && bothTeams[0] && bothTeams[1],
          "places: both continents and both factions");
    Check(TravelPlaces::Names().find("darnassus") != std::string::npos, "places: the usage lists the names");
}

// WU11b fix 8: a bot on its flyer is in the air by its height over the floor, not by the flying flag (Seyo stood on
// the ground with the flag and never dismounted to fight)
void CheckInTheAir()
{
    Check(!TravelLegs::InTheAir(118.2f, 117.9f), "in the air: not on the ground (0.3 yd over the floor, Seyo)");
    Check(!TravelLegs::InTheAir(95.0f, 90.0f), "in the air: not at AIRBORNE_YARDS (5 yd)");
    Check(TravelLegs::InTheAir(95.5f, 90.0f), "in the air: just above AIRBORNE_YARDS");
    Check(TravelLegs::InTheAir(126.0f, 90.0f), "in the air: 36 yd up on the climb (Nennaro)");
    Check(!TravelLegs::InTheAir(10.0f, 12.0f), "in the air: not under a floor (a sample above the bot)");
    Check(TravelLegs::InTheAir(500.0f, -200000.0f), "in the air: no floor found (INVALID_HEIGHT), the flag decides");
    Check(TravelLegs::InTheAir(500.0f, -100000.0f), "in the air: no vmap floor (VMAP_INVALID_HEIGHT_VALUE)");
}

// WU11b fix 9: a fly leg that comes down after a fight in the air is a combat landing, not a fall (Olina, attacked on
// the descent); combat before the takeoff or on another leg type does not count
void CheckCombatLanding()
{
    using TravelLegs::Life;
    Trip quiet = StartedTrip({FlyLeg()}, 0);
    quiet.seenInFlight = true;
    Check(std::string(TravelLegs::LandingWord(quiet, 0.0f)) == "fall", "combat landing: no fight, a fall");
    Trip ground = StartedTrip({FlyLeg()}, 0);
    TravelLegs::NoteLife(ground, Life::Combat, 1000);
    TravelLegs::NoteLife(ground, Life::Calm, 5000);
    ground.seenInFlight = true;
    Check(!ground.foughtInFlight && std::string(TravelLegs::LandingWord(ground, 0.0f)) == "fall",
          "combat landing: a fight before the takeoff is no combat landing");
    Trip fought = StartedTrip({FlyLeg()}, 0);
    fought.seenInFlight = true;
    TravelLegs::NoteLife(fought, Life::Combat, 2000);
    TravelLegs::NoteLife(fought, Life::Calm, 18000);
    Check(fought.foughtInFlight && std::string(TravelLegs::LandingWord(fought, 0.0f)) == "combat",
          "combat landing: a fight after the takeoff, on the ground (Olina)");
    Check(std::string(TravelLegs::LandingWord(fought, 117.0f)) == "fall",
          "combat landing: a fight, then down 117 yd in the air, is a fall (Daenlean)");
    fought.legs.push_back(FlyLeg());
    TravelLegs::StartLeg(fought, 1, 20000, NodeAt);
    Check(!fought.foughtInFlight, "combat landing: a new leg starts without the fight");
    Trip walking = StartedTrip(FullRoute());
    walking.seenInFlight = true;
    TravelLegs::NoteLife(walking, Life::Combat, 3000);
    Check(!walking.foughtInFlight, "combat landing: only on a fly leg");
}

void CheckBodyZ()
{
    float const none = TravelLegs::FLY_NO_GROUND;
    Check(TravelLegs::BodyZ(192.0f, 75.0f, none) == 75.0f, "body: killed 117 yd up lies on the floor (Daenlean)");
    Check(TravelLegs::BodyZ(78.0f, 75.0f, none) == 78.0f, "body: within AIRBORNE_YARDS of the floor stays");
    Check(TravelLegs::BodyZ(75.0f, 75.0f, none) == 75.0f, "body: on the floor stays");
    Check(TravelLegs::BodyZ(70.0f, 75.0f, none) == 70.0f, "body: below the probe's floor (a cave) stays");
    Check(TravelLegs::BodyZ(192.0f, none, none) == 192.0f, "body: no floor found stays");
    Check(TravelLegs::BodyZ(192.0f, -100000.0f, none) == 192.0f, "body: the map's invalid height stays");
    Check(TravelLegs::BodyZ(69.0f, -500.0f, 0.0f) == 0.0f, "body: killed over the sea floats on it (Almaadon)");
    Check(TravelLegs::BodyZ(-3.0f, -500.0f, 0.0f) == -3.0f, "body: died swimming below the surface stays");
    Check(TravelLegs::BodyZ(2.0f, -500.0f, 0.0f) == 2.0f, "body: within AIRBORNE_YARDS of the surface stays");
    Check(TravelLegs::BodyZ(150.0f, 40.0f, 10.0f) == 40.0f, "body: ground above the water's level is the floor");
    Check(TravelLegs::BodyZ(150.0f, none, 0.0f) == 0.0f, "body: water with no ground found is the floor");
}

// WU11c: a fly leg that failed in the air comes down before the replan: on its flying mount straight down to the floor
// (the higher of ground and water), without one it falls (Balthas, Farnin, Elyvia hovered where the leg failed and
// every walk replan failed at that spot)
void CheckDescent()
{
    using TravelLegs::Recovery;
    float const none = TravelLegs::FLY_NO_GROUND;
    Point const up{1, 300.0f, 0.0f, 202.7f};
    auto const isFloor = [](Point const& at, float z)
    { return at.mapId == 1 && at.x == 300.0f && at.y == 0.0f && at.z == z; };

    Trip walk = StartedTrip(FullRoute());
    Check(TravelLegs::StartDescent(walk, up, 90.0f, none, true, 5000).act == Act::Done && !walk.descending,
          "descent: only after a fly leg");

    Trip trip = StartedTrip({FlyLeg()}, 0);
    trip.seenInFlight = true;
    Check(TravelLegs::AfterFailure(trip, FailReason::Landing) == Recovery::Replan && trip.noFly,
          "descent: the failed fly leg replans walking (AfterFailure first, as LegFailed calls them)");
    Decision const start = TravelLegs::StartDescent(trip, up, 90.0f, none, true, 5000);
    Check(start.act == Act::Descend && isFloor(start.target, 90.0f) && trip.descending && trip.descentStartMs == 5000,
          "descent: 113 yd up on the flying mount, straight down to the ground below (Elyvia)");
    Decision const on = TravelLegs::StepDescent(trip, {1, 300.0f, 0.0f, 150.0f}, true, 20000);
    Check(on.act == Act::Descend && isFloor(on.target, 90.0f) && trip.descending, "descent: 60 yd up, on down");
    Decision const down = TravelLegs::StepDescent(trip, {1, 300.0f, 0.0f, 94.0f}, true, 30000);
    Check(down.act == Act::Done && isFloor(down.target, 90.0f) && !trip.descending,
          "descent: within AIRBORNE_YARDS of the floor it is down (the glue lands it)");
    Check(TravelLegs::StepDescent(trip, up, true, 31000).act == Act::Done, "descent: none running, nothing to do");

    TravelLegs::StartDescent(trip, up, 90.0f, none, true, 40000);
    Decision const lost = TravelLegs::StepDescent(trip, {1, 300.0f, 0.0f, 150.0f}, false, 45000);
    Check(lost.act == Act::Fall && isFloor(lost.target, 90.0f) && !trip.descending,
          "descent: the flying mount gone on the way, it falls the rest");
    TravelLegs::StartDescent(trip, up, 90.0f, none, true, 50000);
    Check(TravelLegs::StepDescent(trip, up, true, 50000 + TravelLegs::DESCENT_TIMEOUT_MS).act == Act::Descend,
          "descent: still on at DESCENT_TIMEOUT_MS");
    Decision const late = TravelLegs::StepDescent(trip, up, true, 50001 + TravelLegs::DESCENT_TIMEOUT_MS);
    Check(late.act == Act::Fail && late.reason == FailReason::Timeout && !trip.descending,
          "descent: past DESCENT_TIMEOUT_MS it gives up, the replan starts where the bot is");
    TravelLegs::StartDescent(trip, up, 90.0f, none, true, 60000);
    Decision const away = TravelLegs::StepDescent(trip, {0, 300.0f, 0.0f, 150.0f}, true, 61000);
    Check(away.act == Act::Fail && away.reason == FailReason::WrongMap && !trip.descending,
          "descent: on another map it ends");

    Decision const fall = TravelLegs::StartDescent(trip, up, 90.0f, none, false, 70000);
    Check(fall.act == Act::Fall && isFloor(fall.target, 90.0f) && !trip.descending,
          "descent: without a flying mount it falls to the floor");
    Check(TravelLegs::StartDescent(trip, {1, 300.0f, 0.0f, 94.0f}, 90.0f, none, true, 70000).act == Act::Done &&
              !trip.descending,
          "descent: within AIRBORNE_YARDS of the floor nothing to do");
    Check(TravelLegs::StartDescent(trip, up, none, none, true, 70000).act == Act::Done && !trip.descending,
          "descent: no floor found, nothing to do");
    Check(isFloor(TravelLegs::StartDescent(trip, up, -500.0f, 0.0f, true, 70000).target, 0.0f) && trip.descending,
          "descent: over the sea down to its surface, not the seabed");
    Check(isFloor(TravelLegs::StartDescent(trip, up, 120.0f, 40.0f, true, 70000).target, 120.0f),
          "descent: ground above the water's level is the floor");
    // WU12b: an abort after a fly leg failed in the air comes down before the trip ends (WU11c review NOTE 1)
    Trip abortTrip = StartedTrip({FlyLeg()}, 0);
    abortTrip.seenInFlight = true;
    Decision const aDown = TravelLegs::AbortDescent(abortTrip, FailReason::Timeout, up, 90.0f, none, true, 80000);
    Check(aDown.act == Act::Descend && isFloor(aDown.target, 90.0f) && abortTrip.descending &&
              abortTrip.endAfterDescent && abortTrip.endReason == FailReason::Timeout &&
              !TravelLegs::EndsAfterDescent(abortTrip, aDown),
          "abort descent: 113 yd up on the flying mount, down first; the trip does not end yet");
    Decision const aOn = TravelLegs::StepDescent(abortTrip, {1, 300.0f, 0.0f, 150.0f}, true, 85000);
    Check(aOn.act == Act::Descend && !TravelLegs::EndsAfterDescent(abortTrip, aOn),
          "abort descent: on the way down the trip stays");
    Decision const aDone = TravelLegs::StepDescent(abortTrip, {1, 300.0f, 0.0f, 94.0f}, true, 90000);
    Check(aDone.act == Act::Done && TravelLegs::EndsAfterDescent(abortTrip, aDone) &&
              abortTrip.endReason == FailReason::Timeout,
          "abort descent: down within AIRBORNE_YARDS, the trip ends with its reason");
    TravelLegs::AbortDescent(abortTrip, FailReason::Landing, up, 90.0f, none, true, 100000);
    Decision const aLate = TravelLegs::StepDescent(abortTrip, up, true, 100001 + TravelLegs::DESCENT_TIMEOUT_MS);
    Check(aLate.act == Act::Fail && TravelLegs::EndsAfterDescent(abortTrip, aLate),
          "abort descent: a descent that times out ends the trip too");
    Decision const aFall = TravelLegs::AbortDescent(abortTrip, FailReason::Landing, up, 90.0f, none, false, 110000);
    Check(aFall.act == Act::Fall && !abortTrip.descending && TravelLegs::EndsAfterDescent(abortTrip, aFall),
          "abort descent: without a flying mount it falls and the trip ends");
    Decision const aGround =
        TravelLegs::AbortDescent(abortTrip, FailReason::Landing, {1, 300.0f, 0.0f, 94.0f}, 90.0f, none, true, 120000);
    Check(aGround.act == Act::Done && !abortTrip.descending && TravelLegs::EndsAfterDescent(abortTrip, aGround),
          "abort descent: already on the floor, the trip ends at once");
    Trip abortWalk = StartedTrip(FullRoute());
    Check(TravelLegs::AbortDescent(abortWalk, FailReason::Stuck, up, 90.0f, none, true, 130000).act == Act::Done &&
              !abortWalk.descending,
          "abort descent: a walk leg has no descent, the trip ends at once");
    TravelLegs::StartDescent(abortTrip, up, 90.0f, none, true, 140000);
    Check(!abortTrip.endAfterDescent, "abort descent: a replan's descent (StartDescent) does not end the trip");
    Check(std::string(TravelLegs::ActWord(Act::Descend)) == "descend" &&
              std::string(TravelLegs::ActWord(Act::Fall)) == "fall",
          "descent: the acts' words for the hold line");
}

// Transport legs (WT70c, PLAN 3.21): a ship from pier A (map 0) to pier B (map 1) by the stored crossings of its two
// stops, and an elevator (a car: no window) from its top to its bottom. The harness plays the follower: it issues the
// crossing the rules ask for (CrossIssued), reports the place (NotePlace) and a refused row (RowRefused), and feeds
// the passenger check's outcomes
namespace TC = TravelCrossing;
using Passenger = TravelLifecycle::Outcome;

std::uint32_t const SHIP = 164871;
std::uint32_t const LIFT = 4170;
std::uint64_t const VESSEL = 900;  // the ship's guid
std::uint32_t const PERIOD = 480000;
Point const PIER_A = NODES[1];
Point const PIER_B = NODES[3];
Point const SEA_0{0, 2000.0f, 0.0f, 0.0f};
Point const SEA_1{1, 3000.0f, 1000.0f, 0.0f};
Point const LIFT_TOP{0, 500.0f, 500.0f, 50.0f};
Point const LIFT_BOTTOM{0, 500.0f, 500.0f, 10.0f};

TC::Row MakeRow(TC::Kind kind, std::uint8_t seq, TC::Spot dock, TC::Spot deck, float gap, float rise)
{
    TC::Row row;
    row.kind = kind;
    row.seq = seq;
    row.dock = dock;
    row.deck = deck;
    row.gap = gap;
    row.rise = rise;
    return row;
}

TC::Stop MakeStop(std::uint32_t entry, std::uint32_t number, Point pose, float o)
{
    TC::Stop stop;
    stop.entry = entry;
    stop.stop = number;
    stop.mapId = pose.mapId;
    stop.x = pose.x;
    stop.y = pose.y;
    stop.z = pose.z;
    stop.o = o;
    return stop;
}

void Window(TC::Stop& stop, std::uint32_t arriveMs, std::uint32_t departMs)
{
    stop.hasWindow = true;
    stop.arriveMs = arriveMs;
    stop.departMs = departMs;
    stop.periodMs = PERIOD;
}

// Pier A, stop 1: the ship stands at (130, 0, 0), heading 0, from 0 to 60 s of its period. Two boarding rows (a walk,
// then a jump) and one leaving row
TC::Stop StopA()
{
    TC::Stop stop = MakeStop(SHIP, 1, {0, 130.0f, 0.0f, 0.0f}, 0.0f);
    Window(stop, 0, 60000);
    stop.boardCount = 2;
    stop.board[0] = MakeRow(TC::Kind::Walk, 0, {118.0f, 0.0f, 0.0f}, {-10.0f, 0.0f, 1.0f}, 2.0f, 1.0f);
    stop.board[1] = MakeRow(TC::Kind::Jump, 1, {118.0f, 6.0f, 0.0f}, {-10.0f, 6.0f, 0.5f}, 3.0f, 0.5f);
    stop.leaveCount = 1;
    stop.leave[0] = MakeRow(TC::Kind::Walk, 0, {118.0f, -6.0f, 0.0f}, {-10.0f, -6.0f, 1.0f}, 2.0f, 1.0f);
    return stop;
}

// Pier B, stop 2: at (1000, 1040, 0), heading 1.5, from 240 to 300 s. One boarding row and two leaving rows
TC::Stop StopB()
{
    TC::Stop stop = MakeStop(SHIP, 2, {1, 1000.0f, 1040.0f, 0.0f}, 1.5f);
    Window(stop, 240000, 300000);
    stop.boardCount = 1;
    stop.board[0] = MakeRow(TC::Kind::Walk, 0, {1000.0f, 1028.0f, 0.0f}, {-10.0f, 0.0f, 1.0f}, 2.0f, 1.0f);
    stop.leaveCount = 2;
    stop.leave[0] = MakeRow(TC::Kind::Walk, 0, {1000.0f, 1028.0f, 0.0f}, {-10.0f, 0.0f, 1.0f}, 2.0f, 1.0f);
    stop.leave[1] = MakeRow(TC::Kind::Walk, 1, {1006.0f, 1028.0f, 0.0f}, {-10.0f, -6.0f, 1.0f}, 2.0f, 1.0f);
    return stop;
}

// Pier A with a jump as its only boarding row
TC::Stop JumpStop()
{
    TC::Stop stop = StopA();
    stop.boardCount = 1;
    stop.board[0] = MakeRow(TC::Kind::Jump, 0, {118.0f, 0.0f, 0.0f}, {-10.0f, 0.0f, 0.5f}, 3.0f, 0.5f);
    return stop;
}

// An elevator's stop: a car (its spawn 7) without a window, one row each way
TC::Stop LiftStop(std::uint32_t number, Point pose)
{
    TC::Stop stop = MakeStop(LIFT, number, pose, 0.0f);
    stop.guid = 7;
    stop.boardCount = 1;
    stop.board[0] = MakeRow(TC::Kind::Walk, 0, {pose.x - 5.0f, pose.y, pose.z}, {-2.0f, 0.0f, 0.0f}, 1.0f, 0.0f);
    stop.leaveCount = 1;
    stop.leave[0] = stop.board[0];
    return stop;
}

TC::Stop const STOP_A = StopA();
TC::Stop const STOP_B = StopB();
TC::Stop const STOP_J = JumpStop();
TC::Stop const STOP_TOP = LiftStop(1, LIFT_TOP);
TC::Stop const STOP_BOTTOM = LiftStop(2, LIFT_BOTTOM);
Point const DOCK_A0{0, 118.0f, 0.0f, 0.0f};
Point const DOCK_A1{0, 118.0f, 6.0f, 0.0f};
Point const DOCK_AL{0, 118.0f, -6.0f, 0.0f};  // pier A's leaving row
Point const DOCK_B0{1, 1000.0f, 1028.0f, 0.0f};
Point const DOCK_B1{1, 1006.0f, 1028.0f, 0.0f};
TravelLifecycle::Point const DECK_A0{-10.0f, 0.0f, 1.0f};
TravelLifecycle::Point const DECK_AL{-10.0f, -6.0f, 1.0f};
TravelLifecycle::Point const DECK_B0{-10.0f, 0.0f, 1.0f};
TravelLifecycle::Point const DECK_B1{-10.0f, -6.0f, 1.0f};
TravelLifecycle::Point const PLACE{-4.0f, 2.0f, 1.0f};  // the bot's place aboard

Leg ShipLeg()
{
    Leg leg = MakeLeg(LegType::Transport, PIER_A, PIER_B, {1, 3}, 240.0f);
    leg.object = SHIP;
    leg.periodMs = PERIOD;  // the vessel's period from the graph
    return leg;
}

Leg LiftLeg()
{
    Leg leg = MakeLeg(LegType::Transport, LIFT_TOP, LIFT_BOTTOM, {}, 8.6f);
    leg.object = LIFT;
    return leg;
}

// A scan that found the vessel standing at a stop's pose, progressMs into its period
TravelLegs::TransportSight Docked(TC::Stop const& stop, std::uint32_t progressMs)
{
    TravelLegs::TransportSight sight;
    sight.scanned = true;
    sight.found = true;
    sight.at = {stop.mapId, stop.x, stop.y, stop.z};
    sight.o = stop.o;
    sight.progressMs = progressMs;
    sight.periodMs = stop.periodMs;
    sight.guid = VESSEL;
    sight.mesh = true;
    return sight;
}

// A scan that found it under way
TravelLegs::TransportSight UnderWay(Point at, std::uint32_t progressMs = 100000)
{
    TravelLegs::TransportSight sight;
    sight.scanned = true;
    sight.found = true;
    sight.at = at;
    sight.o = 0.7f;
    sight.progressMs = progressMs;
    sight.periodMs = PERIOD;
    sight.guid = VESSEL;
    sight.mesh = true;
    return sight;
}

// A scan that found none on the bot's map
TravelLegs::TransportSight Gone()
{
    TravelLegs::TransportSight sight;
    sight.scanned = true;
    return sight;
}

// Off the vessel, the ship leg's two stop records in view
Observation Seen(Point bot, std::uint32_t nowMs, TravelLegs::TransportSight sight = {})
{
    Observation seen = At(bot, nowMs);
    seen.transport = sight;
    seen.boardAt = &STOP_A;
    seen.leaveAt = &STOP_B;
    return seen;
}

// Aboard the ship, at an offset in its frame
Observation Riding(std::uint32_t nowMs, TravelLifecycle::Point offset, TravelLegs::TransportSight sight = {})
{
    Observation seen = Seen(sight.found ? sight.at : SEA_1, nowMs, sight);
    seen.aboard = SHIP;
    seen.aboardGuid = VESSEL;
    seen.offset = offset;
    return seen;
}

// The get-off's view: aboard, and the record of the stop the vessel stands at (null at none)
Observation GetOff(std::uint32_t nowMs, TravelLifecycle::Point offset, TC::Stop const* at,
                   TravelLegs::TransportSight sight = {})
{
    Observation seen = Riding(nowMs, offset, sight);
    seen.boardAt = nullptr;
    seen.leaveAt = at;
    return seen;
}

Observation With(Observation seen, Passenger outcome)
{
    seen.passengerOutcome = outcome;
    return seen;
}

Observation From(Observation seen, TC::Stop const& stop)
{
    seen.boardAt = &stop;
    return seen;
}

bool Near(Point const& a, Point const& b) { return a.mapId == b.mapId && TravelRoute::Distance(a, b) < 0.01f; }

bool Noted(Trip const& trip, std::size_t index, TravelLegs::TransportEvent event, std::uint32_t entry,
           std::uint32_t amount)
{
    return index < trip.transportNotes.size() && trip.transportNotes[index].event == event &&
           trip.transportNotes[index].entry == entry && trip.transportNotes[index].amount == amount;
}

bool Last(Trip const& trip, TravelLegs::TransportEvent event, std::uint32_t entry, std::uint32_t amount)
{
    return !trip.transportNotes.empty() && Noted(trip, trip.transportNotes.size() - 1, event, entry, amount);
}

// The trip's Transport events in order, by their words
std::string Words(Trip const& trip)
{
    std::string words;
    for (TravelLegs::TransportNote const& note : trip.transportNotes)
    {
        if (!words.empty())
            words += ',';
        words += TravelLegs::TransportSource(note.event);
    }
    return words;
}

bool AvoidsRide(Trip const& trip)
{
    return trip.avoidEdges.size() == 1 && trip.avoidEdges[0] == std::pair<std::uint32_t, std::uint32_t>{1, 3};
}

// On row 0's dock point of pier A at 3 s, the ship not in sight: attempt, waits
Trip AtDock()
{
    Trip trip = StartedTrip({ShipLeg(), MakeLeg(LegType::Walk, PIER_B, NODES[4], {3, 4}, 15.0f)});
    Step(trip, Seen(DOCK_A0, 3000, Gone()));
    return trip;
}

// ... the ship stands at pier A at 10 s, 10 s into its window: the follower issues the crossing by row 0
Trip Crossing()
{
    Trip trip = AtDock();
    Observation const seen = Seen(DOCK_A0, 10000, Docked(STOP_A, 10000));
    Step(trip, seen);
    TravelLegs::CrossIssued(trip, seen);
    return trip;
}

// ... boarded at 12 s (and the place taken)
Trip Aboard(bool placed = true)
{
    Trip trip = Crossing();
    Step(trip, With(Riding(12000, DECK_A0, Docked(STOP_A, 12000)), Passenger::Boarded));
    if (placed)
        TravelLegs::NotePlace(trip, true);
    return trip;
}

// Case 1: a walk row boards, the bot rides, takes its place and leaves by the far stop's row
void CheckCrossing()
{
    using TravelLegs::Boarding;
    using TravelLegs::CrossUnderWay;
    using TravelLegs::RowNow;
    using TravelLegs::RowRef;
    using TravelLegs::TransportEvent;

    Trip trip = StartedTrip({ShipLeg(), MakeLeg(LegType::Walk, PIER_B, NODES[4], {3, 4}, 15.0f)});
    Decision const approach = Step(trip, Seen({0, 70.0f, 0.0f, 0.0f}, 2000));
    Check(approach.act == Act::Walk && Near(approach.target, DOCK_A0) && trip.boarding == Boarding::Approach,
          "case 1: walks to the first boarding row's dock point");
    Check(Step(trip, Seen({0, 116.5f, 0.0f, 0.0f}, 2500)).act == Act::Walk && trip.transportNotes.empty(),
          "case 1: 1.5 yd from the dock point is not on it");
    Check(Step(trip, Seen(DOCK_A0, 3000, Gone())).act == Act::Wait && trip.boarding == Boarding::Wait &&
              Words(trip) == "attempt" && Noted(trip, 0, TransportEvent::Attempt, SHIP, 2),
          "case 1: on the dock point: attempt (2 s walk), waits");
    Check(Step(trip, Seen(DOCK_A0, 9000, UnderWay(SEA_0))).act == Act::Wait && trip.boarding == Boarding::Wait,
          "case 1: the ship under way: waits");
    Observation const docked = Seen(DOCK_A0, 10000, Docked(STOP_A, 10000));
    Decision const board = Step(trip, docked);
    RowRef const first = RowNow(trip, docked);
    Check(board.act == Act::Board && Near(board.target, DOCK_A0) && trip.boarding == Boarding::Board &&
              first.stop == &STOP_A && first.direction == TC::Direction::Board && first.index == 0 &&
              first.Get() == &STOP_A.board[0] && Words(trip) == "attempt",
          "case 1: the ship stands at the stop's pose with time for the row: board by row 0");
    TravelLegs::CrossIssued(trip, docked);
    Check(CrossUnderWay(trip) && Noted(trip, 1, TransportEvent::Cross, SHIP, 100),
          "case 1: the crossing issued: cross (stop 1, boarding, row 0)");
    Check(Step(trip, Seen(DOCK_A0, 10500)).act == Act::Board && trip.transportNotes.size() == 2,
          "case 1: the crossing under way: nothing new");
    Decision const ride = Step(trip, With(Riding(12000, DECK_A0, Docked(STOP_A, 12000)), Passenger::Boarded));
    Check(ride.act == Act::Stand && trip.boarding == Boarding::Ride && trip.passengerGuid == VESSEL &&
              !CrossUnderWay(trip) && Noted(trip, 2, TransportEvent::Crossed, SHIP, 100) &&
              Noted(trip, 3, TransportEvent::Boarded, SHIP, 9),
          "case 1: the passenger check's boarding: crossed, boarded (9 s waited), the place is asked for");
    TravelLegs::NotePlace(trip, true);
    Check(Noted(trip, 4, TransportEvent::Place, SHIP, 1) && Step(trip, Riding(13000, PLACE)).act == Act::Wait,
          "case 1: the place taken: place 1, then the bot stays");
    Check(Step(trip, Riding(100000, PLACE, UnderWay(SEA_1))).act == Act::Wait && trip.boarding == Boarding::Ride &&
              trip.transportNotes.size() == 5,
          "case 1: under way: rides");
    Observation const farStop = Riding(250000, PLACE, Docked(STOP_B, 250000));
    Decision const deck = Step(trip, farStop);
    RowRef const leaveRow = RowNow(trip, farStop);
    Check(
        deck.act == Act::Deck && Near(deck.target, DOCK_B0) && trip.boarding == Boarding::Leave &&
            Noted(trip, 5, TransportEvent::Leave, SHIP, 238) && leaveRow.stop == &STOP_B &&
            leaveRow.direction == TC::Direction::Leave && leaveRow.index == 0,
        "case 1: the ship stands at the far stop: leave (238 s aboard), the deck walk to the leaving row's deck point");
    Check(Step(trip, Riding(251000, {-10.0f, 0.6f, 1.0f})).act == Act::Deck &&
              Step(trip, Riding(251500, {-10.0f, 0.4f, 1.0f})).act == Act::Disembark,
          "case 1: on the deck point within 0.5 yd of the bot's own offset");
    Observation const atRow = Riding(252000, DECK_B0);
    Decision const leave = Step(trip, atRow);
    Check(leave.act == Act::Disembark && Near(leave.target, DOCK_B0), "case 1: crosses by the leaving row");
    TravelLegs::CrossIssued(trip, atRow);
    Check(Noted(trip, 6, TransportEvent::Cross, SHIP, 210) && Step(trip, atRow).act == Act::Disembark,
          "case 1: cross (stop 2, leaving, row 0), under way");
    Trip onward = trip;
    Decision const done = Step(trip, With(Seen(DOCK_B0, 254000), Passenger::Exited));
    Check(done.act == Act::Done && Words(trip) == "attempt,cross,crossed,boarded,place,leave,cross,crossed,arrived" &&
              Noted(trip, 7, TransportEvent::Crossed, SHIP, 210) && Noted(trip, 8, TransportEvent::Arrived, SHIP, 242),
          "case 1: the passenger check's exit: crossed, arrived (242 s aboard), the leg is done");
    Decision const next = Go(onward, With(Seen(DOCK_B0, 254000), Passenger::Exited), NodeAt);
    Check(next.act == Act::Walk && onward.legIdx == 1 && onward.row == 0 && !CrossUnderWay(onward),
          "case 1: the next leg starts");
}

// The tick's observation is of the leg it was taken on (the glue reads the stop records and the scan for that leg): a
// walk that ends before a ship leg hands the leg to the next tick, and the ship leg never reads "no crossing" from
// the walk's empty observation (WT70c's first run: every ride after a walk failed so)
void CheckLegHandOver()
{
    Trip trip = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 30.0f), ShipLeg()});
    Point bot = NODES[0];
    std::uint32_t nowMs = 1000;
    Decision last{Act::Walk, bot, FailReason::None};
    for (int tick = 0; tick < 6 && trip.legIdx == 0; ++tick)
    {
        nowMs += 1000;
        last = Go(trip, At(bot, nowMs), NodeAt);  // a walk's observation: no stop records, no scan
        if (last.act == Act::Walk)
            bot = last.target;
    }
    Check(trip.legIdx == 1 && last.act == Act::Wait && trip.transportNotes.empty() &&
              trip.boarding == TravelLegs::Boarding::Approach,
          "hand-over: the walk's last tick starts the ship leg and waits, nothing noted");
    Decision const first = Go(trip, Seen(bot, nowMs + 1000), NodeAt);
    Check(first.act == Act::Walk && Near(first.target, DOCK_A0) && Words(trip).find("no_crossing") == std::string::npos,
          "hand-over: the ship leg's first step has its own records: the approach to the row's dock point");
}

// The stand test (PLAN 3.21 (4)) and the crossing's time (5)
void CheckStandTest()
{
    using TravelLegs::Boarding;
    using TravelLegs::StandLeftMs;
    using TravelLegs::Stood;
    auto const scan = [](Trip& trip, TC::Stop const& stop, TravelLegs::TransportSight sight, std::uint32_t nowMs)
    { TravelLegs::NoteScan(trip, From(Seen(SEA_0, nowMs, sight), stop)); };

    // A ship: at the pose, inside the stop's window; the time left counts down between scans
    Trip trip = StartedTrip({ShipLeg()});
    scan(trip, STOP_A, Docked(STOP_A, 10000), 5000);
    Check(StandLeftMs(trip, STOP_A, 5000) == 50000 && StandLeftMs(trip, STOP_A, 5800) == 49200 && Stood(trip, STOP_A),
          "stand test: 10 s into a 60 s window: 50 s left, less the time since the scan");
    Check(StandLeftMs(trip, STOP_B, 5000) < 0 && !Stood(trip, STOP_B), "stand test: not at another stop's pose");
    TravelLegs::TransportSight off = Docked(STOP_A, 10000);
    off.at.x += 0.3f;
    scan(trip, STOP_A, off, 6000);
    Check(!Stood(trip, STOP_A), "stand test: 0.3 yd off the stored pose is not at it");
    TravelLegs::TransportSight turned = Docked(STOP_A, 10000);
    turned.o += 0.02f;
    scan(trip, STOP_A, turned, 7000);
    Check(!Stood(trip, STOP_A), "stand test: a heading 0.02 rad off the stored one is not at it");
    scan(trip, STOP_A, Docked(STOP_A, 70000), 8000);
    Check(StandLeftMs(trip, STOP_A, 8000) < 0 && !Stood(trip, STOP_A),
          "stand test: at the pose outside the stop's window: under way");
    // A window across the period's end: from 470 s to 20 s of 480 s
    TC::Stop wrap = STOP_A;
    wrap.arriveMs = 470000;
    wrap.departMs = 20000;
    scan(trip, wrap, Docked(wrap, 475000), 9000);
    Check(StandLeftMs(trip, wrap, 9000) == 25000, "stand test: a wrapping window, before the period's end: 25 s left");
    scan(trip, wrap, Docked(wrap, 10000), 10000);
    Check(StandLeftMs(trip, wrap, 10000) == 10000, "stand test: a wrapping window, after the period's end: 10 s left");
    scan(trip, wrap, Docked(wrap, 30000), 11000);
    Check(!Stood(trip, wrap), "stand test: past a wrapping window");

    // The crossing's time: a walk at run speed; a jump in its own time after the run-up at the jump's speed
    TC::Move const walk = TC::MoveOf(STOP_A.board[0], TC::Direction::Board, 0.1f);
    Check(!walk.jump && !walk.runUp && walk.speed == 0.0f && std::fabs(walk.seconds - 0.3194f) < 0.001f,
          "crossing: a walk of 2.24 yd takes 0.32 s; a start within 0.2 yd of the first point leaves it out");
    TC::Move const jump = TC::MoveOf(STOP_J.board[0], TC::Direction::Board, 0.5f);
    Check(jump.jump && jump.runUp && std::fabs(jump.speed - 4.0188f) < 0.01f &&
              std::fabs(jump.startSeconds - 0.1244f) < 0.001f && std::fabs(jump.seconds - 0.8812f) < 0.001f &&
              std::fabs(jump.amplitude - 1.3810f) < 0.001f,
          "crossing: a jump up 0.5 yd over 3 yd: 0.757 s in the air at 4.02 yd/s, after a run-up of 0.124 s");
    TC::Move const down = TC::MoveOf(STOP_J.board[0], TC::Direction::Leave, 0.0f);
    Check(down.jump && std::fabs(down.seconds - 0.8839f) < 0.001f && down.startSeconds == 0.0f,
          "crossing: the same row leaving is a jump down 0.5 yd: 0.884 s");
    Check(!TC::MoveOf(TC::Kind::Jump, 0.0f, 0.0f, 0.1f).jump, "crossing: a jump no longer than 0.2 yd is a step");

    // The time test at the dock: the jump's 757 ms plus 1 s slack
    Trip late = StartedTrip({ShipLeg()});
    Step(late, From(Seen(DOCK_A0, 3000, Gone()), STOP_J));
    Check(Step(late, From(Seen(DOCK_A0, 58300, Docked(STOP_J, 58300)), STOP_J)).act == Act::Wait &&
              late.boarding == Boarding::Wait,
          "stand test: 1.7 s left at the stop: no crossing starts");
    Check(Step(late, From(Seen(DOCK_A0, 538200, Docked(STOP_J, 58200)), STOP_J)).act == Act::Board &&
              late.boarding == Boarding::Board,
          "stand test: 1.8 s left: the jump starts");

    // A car has no window: it stands when the scan before saw it at the pose as well
    auto const lift = [](Point bot, std::uint32_t nowMs, TravelLegs::TransportSight sight)
    {
        Observation seen = At(bot, nowMs);
        seen.transport = sight;
        seen.boardAt = &STOP_TOP;
        seen.leaveAt = &STOP_BOTTOM;
        return seen;
    };
    Point const liftDock{0, 495.0f, 500.0f, 50.0f};
    Trip car = StartedTrip({LiftLeg()});
    Check(Step(car, lift(liftDock, 2000, Docked(STOP_TOP, 0))).act == Act::Wait && car.boarding == Boarding::Wait,
          "stand test: a car at the pose on one scan: not yet standing");
    Check(Step(car, lift(liftDock, 2500, {})).act == Act::Wait, "stand test: between scans: no second sight");
    Trip passing = car;
    Check(Step(car, lift(liftDock, 3000, Docked(STOP_TOP, 0))).act == Act::Board && car.boarding == Boarding::Board,
          "stand test: at the pose on the next scan as well: it stands");
    Step(passing, lift(liftDock, 3000, UnderWay({0, 500.0f, 500.0f, 30.0f})));
    Check(Step(passing, lift(liftDock, 4000, Docked(STOP_TOP, 0))).act == Act::Wait &&
              Step(passing, lift(liftDock, 5000, Docked(STOP_TOP, 0))).act == Act::Board,
          "stand test: a scan off the pose starts the count anew");
}

// Case 3: the vessel leaves during a boarding walk (stopped by the passenger check) and during a jump (it runs out)
void CheckCutCrossing()
{
    using TravelLegs::Boarding;
    using TravelLegs::CrossUnderWay;
    using TravelLegs::TransportEvent;

    Point const gangway{0, 119.5f, 0.0f, 0.3f};
    Trip trip = Crossing();
    Decision const cut = Step(trip, With(Seen(gangway, 11000, UnderWay(SEA_0)), Passenger::Cancelled));
    Check(cut.act == Act::Wait && trip.boarding == Boarding::Board && !CrossUnderWay(trip) &&
              Words(trip) == "attempt,cross,cross_cut" && Last(trip, TransportEvent::CrossCut, SHIP, 100) &&
              trip.row == 0,
          "case 3: the ship left under the walk: cross_cut, the row is not spent");
    Check(Step(trip, Seen(gangway, 13499)).act == Act::Wait && trip.transportNotes.size() == 3,
          "case 3: within the grace: no miss yet");
    Check(Step(trip, Seen(gangway, 13500)).act == Act::Wait && trip.boarding == Boarding::Wait &&
              Last(trip, TransportEvent::Missed, SHIP, 1) && trip.misses == 1,
          "case 3: 2.5 s after it left: missed, waits");
    Decision const back = Step(trip, Seen(gangway, 14000));
    Check(back.act == Act::Walk && Near(back.target, DOCK_A0) && trip.boarding == Boarding::Approach,
          "case 3: off the dock point: walks back to it");
    Check(Step(trip, Seen(DOCK_A0, 15000)).act == Act::Wait && trip.boarding == Boarding::Wait &&
              trip.transportNotes.size() == 4,
          "case 3: on the dock point again: waits, no second attempt");
    Observation const again = Seen(DOCK_A0, 490000, Docked(STOP_A, 10000));
    Check(Step(trip, again).act == Act::Board && TravelLegs::RowNow(trip, again).index == 0,
          "case 3: the next docking: the same row");

    // A jump is not stopped in the air: its outcome comes when it ran out, and the verdict waits for a scan
    Trip jump = StartedTrip({ShipLeg()});
    Step(jump, From(Seen(DOCK_A0, 3000, Gone()), STOP_J));
    Observation const start = From(Seen(DOCK_A0, 10000, Docked(STOP_J, 10000)), STOP_J);
    Check(Step(jump, start).act == Act::Board, "case 3: the jump row: board");
    TravelLegs::CrossIssued(jump, start);
    Check(Step(jump, From(Seen(DOCK_A0, 10500, UnderWay(SEA_0)), STOP_J)).act == Act::Board && CrossUnderWay(jump) &&
              Words(jump) == "attempt,cross",
          "case 3: the ship leaves under the jump: the jump runs on");
    Check(Step(jump, With(From(Seen(DOCK_A0, 10900), STOP_J), Passenger::Cancelled)).act == Act::Wait &&
              !CrossUnderWay(jump) && Words(jump) == "attempt,cross",
          "case 3: it ran out without a boarding: no verdict before a scan");
    Check(Step(jump, From(Seen(DOCK_A0, 11500, UnderWay(SEA_0)), STOP_J)).act == Act::Wait &&
              Words(jump) == "attempt,cross,cross_cut" && jump.row == 0,
          "case 3: the scan saw the ship gone: cross_cut");
    Check(Step(jump, From(Seen(DOCK_A0, 14000), STOP_J)).act == Act::Wait &&
              Last(jump, TransportEvent::Missed, SHIP, 1) && jump.boarding == Boarding::Wait,
          "case 3: then missed, and the same row at the next docking");
}

// Cases 4 and 7: a row fails while the vessel stands (cancelled, or no outcome within CROSS_MAX_MS): the next row; all
// spent: crossing_failed, the ride's edge is left out and the trip replans
void CheckFailedRows()
{
    using TravelLegs::Boarding;
    using TravelLegs::CrossUnderWay;
    using TravelLegs::Recovery;
    using TravelLegs::TransportEvent;

    Trip trip = Crossing();
    Decision const spent = Step(trip, With(Seen(DOCK_A0, 11000, Docked(STOP_A, 11000)), Passenger::Cancelled));
    Check(spent.act == Act::Walk && Near(spent.target, DOCK_A1) && trip.boarding == Boarding::Approach &&
              trip.row == 1 && Last(trip, TransportEvent::CrossFailed, SHIP, 100),
          "case 4: cancelled while the ship stands: cross_failed, row 0 is spent, on to row 1's dock point");
    Observation const second = Seen(DOCK_A1, 12000);
    Decision const board = Step(trip, second);
    Check(board.act == Act::Board && Near(board.target, DOCK_A1) && TravelLegs::RowNow(trip, second).index == 1 &&
              trip.transportNotes.size() == 3,
          "case 4: on row 1's dock point with the ship still there: board by it, no second attempt");
    TravelLegs::CrossIssued(trip, second);
    Check(Last(trip, TransportEvent::Cross, SHIP, 101), "case 4: cross (stop 1, boarding, row 1)");
    Decision const failed = Step(trip, With(Seen(DOCK_A1, 13000, Docked(STOP_A, 13000)), Passenger::Cancelled));
    Check(failed.act == Act::Fail && failed.reason == FailReason::CrossingFailed &&
              Words(trip) == "attempt,cross,cross_failed,cross,cross_failed" &&
              Last(trip, TransportEvent::CrossFailed, SHIP, 101),
          "case 4: the last row failed too: the leg fails crossing_failed");
    Check(TravelLegs::AfterFailure(trip, FailReason::CrossingFailed) == Recovery::Replan && AvoidsRide(trip) &&
              trip.replans == 1,
          "case 4: the replan leaves the ride's edge out");
    Check(std::string(TravelLegs::LegFailedSource(FailReason::CrossingFailed)) == "leg_failed:crossing_failed" &&
              std::string(TravelLegs::AbortSource(FailReason::CrossingFailed)) == "abort:crossing_failed" &&
              TravelLegs::FailOutcome(FailReason::CrossingFailed) == StatusEnd::Outcome::Abandoned,
          "case 4: crossing_failed: its words, abandoned");

    // The mover gives a row's dock point up: the row is spent before any crossing
    Trip stuck = StartedTrip({ShipLeg()});
    Observation const walking = Seen({0, 70.0f, 0.0f, 0.0f}, 2000);
    Step(stuck, walking);
    TravelLegs::RowRefused(stuck, walking);
    Decision const other = Step(stuck, Seen({0, 70.0f, 0.0f, 0.0f}, 2500));
    Check(other.act == Act::Walk && Near(other.target, DOCK_A1) && Words(stuck) == "cross_failed" && stuck.row == 1,
          "case 4: the mover gave row 0's dock point up: cross_failed, walks to row 1's");
    TravelLegs::RowRefused(stuck, walking);
    Decision const none = Step(stuck, Seen({0, 70.0f, 0.0f, 0.0f}, 3000));
    Check(none.act == Act::Fail && none.reason == FailReason::CrossingFailed,
          "case 4: no dock point left: crossing_failed");

    // Case 7: a crossing without an outcome is cancelled CROSS_MAX_MS after its issue
    Trip silent = Crossing();
    Check(Step(silent, Seen(DOCK_A0, 14999)).act == Act::Board && CrossUnderWay(silent),
          "case 7: 4999 ms without an outcome: still under way");
    Check(Step(silent, Seen(DOCK_A0, 15000)).act == Act::Wait && !CrossUnderWay(silent) &&
              silent.transportNotes.size() == 2,
          "case 7: at 5 s it is cancelled (the follower discards its move); the verdict waits for a scan");
    Decision const next = Step(silent, Seen(DOCK_A0, 15500, Docked(STOP_A, 15500)));
    Check(next.act == Act::Walk && Near(next.target, DOCK_A1) && Last(silent, TransportEvent::CrossFailed, SHIP, 100),
          "case 7: the ship still stands: cross_failed, the next row");
}

// Case 5: no row, or a vessel without a mesh: no_crossing before any walk
void CheckNoCrossing()
{
    using TravelLegs::Recovery;
    using TravelLegs::TransportEvent;
    Point const away{0, 70.0f, 0.0f, 0.0f};
    auto const refused = [](Decision const& decision)
    { return decision.act == Act::Fail && decision.reason == FailReason::NoCrossing; };

    Trip bare = StartedTrip({ShipLeg()});
    Observation none = Seen(away, 2000);
    none.boardAt = nullptr;
    none.leaveAt = nullptr;
    Check(refused(Step(bare, none)) && Words(bare) == "no_crossing" && Last(bare, TransportEvent::NoCrossing, SHIP, 0),
          "case 5: a leg without stop records: no_crossing at once, no walk");
    Check(TravelLegs::AfterFailure(bare, FailReason::NoCrossing) == Recovery::Replan && AvoidsRide(bare),
          "case 5: the replan leaves the ride's edge out");

    TC::Stop rowless = STOP_A;
    rowless.boardCount = 0;
    Trip noBoard = StartedTrip({ShipLeg()});
    Check(
        refused(Step(noBoard, From(Seen(away, 2000), rowless))) && Last(noBoard, TransportEvent::NoCrossing, SHIP, 100),
        "case 5: a from-stop without a boarding row: no_crossing (stop 1, boarding)");
    TC::Stop exitless = STOP_B;
    exitless.leaveCount = 0;
    Trip noLeave = StartedTrip({ShipLeg()});
    Observation toExitless = Seen(away, 2000);
    toExitless.leaveAt = &exitless;
    Check(refused(Step(noLeave, toExitless)), "case 5: a far stop without a leaving row: no_crossing");

    Trip meshless = AtDock();
    TravelLegs::TransportSight hull = Docked(STOP_A, 10000);
    hull.mesh = false;
    Check(refused(Step(meshless, Seen(DOCK_A0, 10000, hull))) && Words(meshless) == "attempt,no_crossing",
          "case 5: the vessel seen holds no mesh: no_crossing");
    Check(std::string(TravelLegs::LegFailedSource(FailReason::NoCrossing)) == "leg_failed:no_crossing" &&
              std::string(TravelLegs::AbortSource(FailReason::NoCrossing)) == "abort:no_crossing" &&
              TravelLegs::FailOutcome(FailReason::NoCrossing) == StatusEnd::Outcome::Abandoned,
          "case 5: no_crossing: its words, abandoned");
}

// Cases 6 and 8: the vessel never comes (the wait clock), a wrong map, and a ride lost during the exit walk
void CheckWaitClockAndLoss()
{
    using TravelLegs::Recovery;
    using TravelLegs::TransportEvent;

    // 60 s slack + 5 periods of 480 s from the leg's start at 1 s
    Trip waiting = AtDock();
    Check(Step(waiting, Seen(DOCK_A0, 1000 + 2460000, Gone())).act == Act::Wait,
          "case 6: the ship never comes: waits to the wait clock's end");
    Decision const late = Step(waiting, Seen(DOCK_A0, 1000 + 2460001, Gone()));
    Check(late.act == Act::Fail && late.reason == FailReason::Timeout && Words(waiting) == "attempt",
          "case 6: one ms past 5 periods: timeout");
    Check(TravelLegs::AfterFailure(waiting, FailReason::Timeout) == Recovery::Replan && AvoidsRide(waiting),
          "case 6: the replan leaves the ride's edge out");
    // The same clock ends a bot held aboard a vessel that is not its ride
    Trip other = AtDock();
    Observation another = Riding(1000 + 2460001, PLACE);
    another.aboardGuid = VESSEL + 1;
    another.bot = DOCK_A0;
    Check(Step(other, another).act == Act::Fail, "case 6: aboard another vessel: the wait clock ends that too");
    Trip wrong = AtDock();
    Decision const map = Step(wrong, Seen(PIER_B, 4000));
    Check(map.act == Act::Fail && map.reason == FailReason::WrongMap, "transport: off the vessel on another map");

    // Case 8: the ship leaves during the exit walk and the bot goes past the deck's edge
    Trip trip = Aboard();
    Step(trip, Riding(250000, PLACE, Docked(STOP_B, 250000)));
    Observation const atRow = Riding(252000, DECK_B0);
    Step(trip, atRow);
    TravelLegs::CrossIssued(trip, atRow);
    Decision const lost = Step(trip, With(Seen(SEA_1, 253000, UnderWay(SEA_1)), Passenger::Lost));
    Check(lost.act == Act::Fail && lost.reason == FailReason::TransportLost &&
              Words(trip) == "attempt,cross,crossed,boarded,place,leave,cross,lost" &&
              Last(trip, TransportEvent::Lost, SHIP, 241) && !TravelLegs::CrossUnderWay(trip),
          "case 8: off the vessel without an exit: lost, the leg fails transport_lost");
    Check(TravelLegs::AfterFailure(trip, FailReason::TransportLost) == Recovery::Replan && AvoidsRide(trip),
          "case 8: the replan leaves the ride's edge out");
}

// Case 9: the far stop's leaving rows are spent: no_exit, the ride back, off at the from-stop by that stop's leaving
// rows, and the leg fails no_exit; those spent too: the ride clock
void CheckBack()
{
    using TravelLegs::Boarding;
    using TravelLegs::Recovery;
    using TravelLegs::RowNow;
    using TravelLegs::TransportEvent;

    Trip trip = Aboard();
    Observation const farStop = Riding(250000, PLACE, Docked(STOP_B, 250000));
    Step(trip, farStop);
    TravelLegs::RowRefused(trip, farStop);
    Check(
        Last(trip, TransportEvent::CrossFailed, SHIP, 210) && trip.leaveSpent == 1 && trip.boarding == Boarding::Leave,
        "case 9: the deck path to row 0's deck point refused: cross_failed, the row is spent");
    Observation const onward = Riding(250500, PLACE);
    Decision const second = Step(trip, onward);
    Check(second.act == Act::Deck && Near(second.target, DOCK_B1) && RowNow(trip, onward).index == 1,
          "case 9: the deck walk to row 1's deck point");
    Observation const atRow = Riding(252000, DECK_B1);
    Check(Step(trip, atRow).act == Act::Disembark, "case 9: crosses by row 1");
    TravelLegs::CrossIssued(trip, atRow);
    Decision const spent = Step(trip, With(Riding(253000, DECK_B1, Docked(STOP_B, 253000)), Passenger::Cancelled));
    Check(spent.act == Act::Wait && trip.boarding == Boarding::Ride &&
              Words(trip) == "attempt,cross,crossed,boarded,place,leave,cross_failed,cross,cross_failed,no_exit" &&
              Noted(trip, 8, TransportEvent::CrossFailed, SHIP, 211),
          "case 9: cancelled while the ship stands: the last leaving row is spent: no_exit, rides on");
    Check(Step(trip, Riding(301000, PLACE, UnderWay(SEA_1))).act == Act::Wait && trip.transportNotes.size() == 10,
          "case 9: the ship leaves: no stuck_aboard, the bot was not leaving");
    Trip round = trip;
    Check(Step(round, Riding(730000, PLACE, Docked(STOP_B, 250000))).act == Act::Wait &&
              round.boarding == Boarding::Ride && round.transportNotes.size() == 10,
          "case 9: at the far stop again: a spent row is not tried again on the leg");
    Observation const home = Riding(485000, PLACE, Docked(STOP_A, 5000));
    Decision const back = Step(trip, home);
    Check(back.act == Act::Deck && Near(back.target, DOCK_AL) && trip.boarding == Boarding::Back &&
              RowNow(trip, home).stop == &STOP_A && RowNow(trip, home).direction == TC::Direction::Leave &&
              RowNow(trip, home).index == 0,
          "case 9: the ship stands at the from-stop: back, by that stop's leaving row");
    Trip stranded = trip;
    Observation const atBack = Riding(487000, DECK_AL);
    Check(Step(trip, atBack).act == Act::Disembark, "case 9: crosses by the from-stop's leaving row");
    TravelLegs::CrossIssued(trip, atBack);
    Decision const off = Step(trip, With(Seen(DOCK_AL, 489000), Passenger::Exited));
    Check(off.act == Act::Fail && off.reason == FailReason::NoExit &&
              Noted(trip, 10, TransportEvent::Cross, SHIP, 110) &&
              Noted(trip, 11, TransportEvent::Crossed, SHIP, 110) && Last(trip, TransportEvent::Back, SHIP, 477) &&
              trip.transportNotes.size() == 13,
          "case 9: off where it boarded: crossed, back (477 s aboard), the leg fails no_exit");
    Check(TravelLegs::AfterFailure(trip, FailReason::NoExit) == Recovery::Replan && AvoidsRide(trip),
          "case 9: the replan leaves the ride's edge out");

    // The from-stop's leaving row fails too: the bot rides until the ride clock (60 s + 2 periods from the boarding)
    TravelLegs::RowRefused(stranded, home);
    Check(Last(stranded, TransportEvent::CrossFailed, SHIP, 110) && stranded.backSpent == 1 &&
              Step(stranded, Riding(486000, PLACE)).act == Act::Wait && stranded.boarding == Boarding::Ride &&
              stranded.transportNotes.size() == 11,
          "case 9: that stop's rows spent too: rides on");
    Check(Step(stranded, Riding(12000 + 1020000, PLACE)).act == Act::Wait, "case 9: to the ride clock's end");
    Decision const late = Step(stranded, Riding(12000 + 1020001, PLACE));
    Check(late.act == Act::Fail && late.reason == FailReason::Timeout, "case 9: one ms past it: timeout");
}

// Case 17 (WT.55): a normal deck path whose end lies further than the reach from its deck point is refused as no path
// is: no deck walk, the leaving row is spent with its cross_failed, the next row; none left: no_exit
void CheckDeckReach()
{
    using TravelLegs::Boarding;
    using TravelLegs::RowNow;
    using TravelLegs::TransportEvent;

    Check(TravelLegs::DeckPathTaken(true, 0.0f) && TravelLegs::DeckPathTaken(true, TravelLegs::DECK_REACH_YARDS) &&
              !TravelLegs::DeckPathTaken(true, 0.61f) && !TravelLegs::DeckPathTaken(false, 0.0f),
          "case 17: a normal path ending within the reach is taken; 0.61 yd short, or not normal, is refused");
    Trip trip = Aboard();
    Observation const farStop = Riding(250000, PLACE, Docked(STOP_B, 250000));
    Decision const first = Step(trip, farStop);
    Check(first.act == Act::Deck && RowNow(trip, farStop).index == 0, "case 17: the deck walk to row 0 is asked");
    TravelLegs::RowRefused(trip, farStop);  // the glue's answer to a refused path: DeckMove issued no walk
    Check(
        Last(trip, TransportEvent::CrossFailed, SHIP, 210) && trip.leaveSpent == 1 && trip.boarding == Boarding::Leave,
        "case 17: the path ends short: cross_failed (stop 2, leaving, row 0), the row is spent");
    Observation const onward = Riding(250500, PLACE);
    Decision const second = Step(trip, onward);
    Check(second.act == Act::Deck && Near(second.target, DOCK_B1) && RowNow(trip, onward).index == 1,
          "case 17: the deck walk to row 1's deck point");
    TravelLegs::RowRefused(trip, onward);
    Decision const none = Step(trip, Riding(251000, PLACE));
    Check(none.act == Act::Wait && trip.boarding == Boarding::Ride &&
              Noted(trip, 7, TransportEvent::CrossFailed, SHIP, 211) &&
              Words(trip) == "attempt,cross,crossed,boarded,place,leave,cross_failed,cross_failed,no_exit",
          "case 17: row 1's path refused too: no leaving row left, no_exit, rides on");
}

// Case 11 (WT70d): a car without a window boards only on its second scan at the pose (CheckStandTest); a looping car's
// stop has its window from the load: the first scan at the pose with time for the row boards, a window too short for
// the row waits for the next docking
void CheckCarWindow()
{
    using TravelLegs::Boarding;
    std::uint32_t const carPeriod = 10000;
    TC::Stop top = STOP_TOP;  // stands from 2 s to 5 s of its 10 s period
    top.hasWindow = true;
    top.arriveMs = 2000;
    top.departMs = 5000;
    top.periodMs = carPeriod;
    auto const lift = [&top](Point bot, std::uint32_t nowMs, TravelLegs::TransportSight sight)
    {
        Observation seen = At(bot, nowMs);
        seen.transport = sight;
        seen.boardAt = &top;
        seen.leaveAt = &STOP_BOTTOM;
        return seen;
    };
    Point const liftDock{0, 495.0f, 500.0f, 50.0f};
    Trip car = StartedTrip({LiftLeg()});
    Step(car, lift(liftDock, 1500, Gone()));
    Check(Step(car, lift(liftDock, 2000, Docked(top, 2500))).act == Act::Board && car.boarding == Boarding::Board,
          "case 11: a car with a window, 2.5 s left: boards on the first scan at the pose");
    Trip late = StartedTrip({LiftLeg()});
    Step(late, lift(liftDock, 1500, Gone()));
    Check(Step(late, lift(liftDock, 2000, Docked(top, 3900))).act == Act::Wait && late.boarding == Boarding::Wait,
          "case 11: 1.1 s left, the row needs 1.15 s: waits");
    TravelLegs::TransportSight moving = UnderWay({0, 500.0f, 500.0f, 30.0f}, 7000);
    moving.periodMs = carPeriod;
    Check(Step(late, lift(liftDock, 5100, moving)).act == Act::Wait && late.boarding == Boarding::Wait,
          "case 11: the car under way: waits");
    Check(Step(late, lift(liftDock, 10200, Docked(top, 2100))).act == Act::Board && late.boarding == Boarding::Board,
          "case 11: the next docking, 2.9 s left: boards");

    // H6 (WT70d-b, a lift's first docking): the far stop stands 3233 ms; the rider's first scan at the pose comes up to
    // a scan after the car stops, its deck walk (about 1.2 s) ends between ticks, and a bot on a transport leg ticks at
    // the scan's pace (BotTravel::PaceMs). At its first tick on the deck point a 0.13-s row still has its time plus the
    // slack (with a scan of 1000 ms: 233 ms left, the car leaves with the rider aboard)
    TC::Stop stand = STOP_BOTTOM;
    stand.hasWindow = true;
    stand.arriveMs = 0;
    stand.departMs = 3233;
    stand.periodMs = 49933;
    TC::Move row;
    row.seconds = 0.13f;
    std::uint32_t const scanMs = TravelLegs::TRANSPORT_SCAN_MS;
    std::uint32_t const onDeckMs = scanMs + (1200 + scanMs - 1) / scanMs * scanMs;  // the first tick on the deck point
    Trip rider = StartedTrip({LiftLeg()});
    Observation docked = At(LIFT_BOTTOM, 200000 + onDeckMs);
    docked.transport = Docked(stand, onDeckMs);
    docked.leaveAt = &stand;
    TravelLegs::NoteScan(rider, docked);
    Check(TravelLegs::HasTimeFor(rider, stand, row, docked.nowMs),
          "case 11: a 3233-ms stand, seen a scan late, a 1.2-s deck walk: the 0.13-s row crosses on this docking");

    // The deck walk ahead (WT70d-b, a lift whose far stop stands 1333 ms of 6667): placed aboard, the rider walks to
    // its leaving row's deck point once the docking is DECK_LEAD_MS away by the stop's window, and stands on it when
    // the car docks; a refused walk ahead spends no row
    TC::Stop brief = STOP_BOTTOM;
    brief.hasWindow = true;
    brief.arriveMs = 0;
    brief.departMs = 1333;
    brief.periodMs = 6667;
    auto const aboard = [&brief](std::uint32_t nowMs, TravelLifecycle::Point offset, TravelLegs::TransportSight sight)
    {
        Observation seen = At(sight.at, nowMs);
        seen.transport = sight;
        seen.boardAt = &STOP_TOP;
        seen.leaveAt = &brief;
        seen.aboard = LIFT;
        seen.aboardGuid = VESSEL;
        seen.offset = offset;
        return seen;
    };
    auto const underWay = [&brief](std::uint32_t progressMs)
    {
        TravelLegs::TransportSight sight = UnderWay({0, 500.0f, 500.0f, 30.0f}, progressMs);
        sight.periodMs = brief.periodMs;
        return sight;
    };
    TravelLifecycle::Point const place{1.0f, 1.5f, 0.0f};
    TravelLifecycle::Point const exitDeck{-2.0f, 0.0f, 0.0f};
    Trip ahead = StartedTrip({LiftLeg()}, 300000);
    ahead.passengerGuid = VESSEL;
    ahead.boarding = Boarding::Ride;
    ahead.boardedMs = 300000;
    ahead.placed = true;
    Check(Step(ahead, aboard(301500, place, underWay(1500))).act == Act::Wait,
          "case 11: the far stop's docking 5.2 s away: rides at its place");
    Observation const soon = aboard(302000, place, underWay(2000));
    Decision const walkAhead = Step(ahead, soon);
    TravelLegs::RowRef const exitRow = TravelLegs::RowNow(ahead, soon);
    Check(walkAhead.act == Act::Deck && ahead.boarding == Boarding::Ride && exitRow.stop == &brief &&
              exitRow.direction == TC::Direction::Leave && exitRow.index == 0,
          "case 11: the docking 4.7 s away: the deck walk ahead to the leaving row's deck point");
    Check(Step(ahead, aboard(305000, exitDeck, underWay(5000))).act == Act::Wait,
          "case 11: on the deck point under way: waits there");
    Observation const docks = aboard(306800, exitDeck, Docked(brief, 133));
    Decision const atStop = Step(ahead, docks);
    Check(atStop.act != Act::Deck && ahead.boarding == Boarding::Leave,
          "case 11: the car docks with the rider on its deck point: no deck walk at the stop");
    Trip refused = StartedTrip({LiftLeg()}, 300000);
    refused.passengerGuid = VESSEL;
    refused.boarding = Boarding::Ride;
    refused.boardedMs = 300000;
    refused.placed = true;
    Step(refused, soon);
    TravelLegs::RowRefused(refused, soon);
    Check(refused.leaveSpent == 0 && refused.transportNotes.empty() &&
              Step(refused, aboard(302250, place, underWay(2250))).act == Act::Wait,
          "case 11: the walk ahead refused: no row spent, not asked again on this ride");
}

// Case 15 (WT70d, PLAN 3.21 (8)): a follower boards and leaves by the rows of the stop its master's vessel stands at
// (here the lift: no window, so it stands from a scan's time at the pose on), each row once per docking; the vessel
// gone, the step does nothing and the follow code goes on
TravelLegs::FollowView FollowAt(TC::Stop const* stop, TC::Direction direction, std::uint32_t nowMs, Point bot,
                                TravelLifecycle::Point offset = {}, std::optional<Passenger> outcome = std::nullopt)
{
    TravelLegs::FollowView view;
    view.nowMs = nowMs;
    view.stop = stop;
    view.stopIndex = stop ? stop->stop : TC::NO_STOP;
    view.direction = direction;
    view.bot = bot;
    view.offset = offset;
    view.outcome = outcome;
    return view;
}

void CheckFollowAboard()
{
    using TravelLegs::FollowAct;
    using TravelLegs::FollowStep;
    using TravelLegs::TransportEvent;
    TC::Direction const on = TC::Direction::Board;
    TC::Direction const off = TC::Direction::Leave;
    Point const away{0, 480.0f, 500.0f, 50.0f};
    Point const dock{0, 495.0f, 500.0f, 50.0f};
    TravelLifecycle::Point const deck{-2.0f, 0.0f, 0.0f};
    auto const last =
        [](std::vector<TravelLegs::TransportNote> const& notes, TransportEvent event, std::uint32_t amount)
    {
        return !notes.empty() && notes.back().event == event && notes.back().entry == LIFT &&
               notes.back().amount == amount;
    };

    TravelLegs::FollowCrossing follow;
    std::vector<TravelLegs::TransportNote> notes;
    Check(FollowStep(follow, FollowAt(nullptr, on, 1000, away), notes).act == FollowAct::None && notes.empty(),
          "case 15: the master's vessel at no stop: nothing to do");
    TravelLegs::FollowDecision const walk = FollowStep(follow, FollowAt(&STOP_TOP, on, 2000, away), notes);
    Check(walk.act == FollowAct::Walk && walk.row == &STOP_TOP.board[0],
          "case 15: the master aboard at a stop: walk to its boarding row's dock point");
    Check(FollowStep(follow, FollowAt(&STOP_TOP, on, 2000 + TravelLegs::TRANSPORT_SCAN_MS / 2, dock), notes).act ==
              FollowAct::Wait,
          "case 15: on the dock point, the car at the pose for half a scan's time: waits");
    TravelLegs::FollowDecision const cross = FollowStep(follow, FollowAt(&STOP_TOP, on, 3000, dock), notes);
    Check(cross.act == FollowAct::Cross && cross.row == &STOP_TOP.board[0] && !cross.move.jump,
          "case 15: at the pose for a scan's time: crosses by row 0");
    TravelLegs::FollowCrossIssued(follow, STOP_TOP, on, 3000, notes);
    Check(last(notes, TransportEvent::Cross, 100) &&
              FollowStep(follow, FollowAt(&STOP_TOP, on, 3200, dock), notes).act == FollowAct::UnderWay,
          "case 15: cross (stop 1, boarding, row 0), under way");
    Check(FollowStep(follow, FollowAt(nullptr, on, 3500, dock, {}, Passenger::Boarded), notes).act == FollowAct::None &&
              last(notes, TransportEvent::Crossed, 100) && follow.stop == TC::NO_STOP && !follow.crossMs,
          "case 15: boarded: crossed, the follow code goes on aboard");

    // Off by the leaving rows of the stop the car stands at
    Check(
        FollowStep(follow, FollowAt(&STOP_BOTTOM, off, 20000, dock, {1.0f, 0.0f, 0.0f}), notes).act == FollowAct::Deck,
        "case 15: the master ashore, the car at a stop: the deck walk to its leaving row's deck point");
    Check(FollowStep(follow,
                     FollowAt(&STOP_BOTTOM, off, 20000 + TravelLegs::TRANSPORT_SCAN_MS / 2, dock, {-2.0f, 0.3f, 0.0f}),
                     notes)
                  .act == FollowAct::Wait,
          "case 15: on the deck point, the car at the pose for half a scan's time: waits");
    Check(FollowStep(follow, FollowAt(&STOP_BOTTOM, off, 21000, dock, deck), notes).act == FollowAct::Cross,
          "case 15: crosses off by the leaving row");
    TravelLegs::FollowCrossIssued(follow, STOP_BOTTOM, off, 21000, notes);
    Check(FollowStep(follow, FollowAt(&STOP_BOTTOM, off, 21500, dock, deck, Passenger::Exited), notes).act ==
                  FollowAct::None &&
              last(notes, TransportEvent::Crossed, 210),
          "case 15: exited: crossed (stop 2, leaving, row 0)");

    // A row that fails while the car stands is spent for this docking; the next docking has it again
    TravelLegs::FollowCrossing failing;
    notes.clear();
    FollowStep(failing, FollowAt(&STOP_TOP, on, 30000, dock), notes);
    Check(FollowStep(failing, FollowAt(&STOP_TOP, on, 31000, dock), notes).act == FollowAct::Cross,
          "case 15: a second follower crosses");
    TravelLegs::FollowCrossIssued(failing, STOP_TOP, on, 31000, notes);
    Check(FollowStep(failing, FollowAt(&STOP_TOP, on, 31500, dock, {}, Passenger::Cancelled), notes).act ==
                  FollowAct::None &&
              last(notes, TransportEvent::CrossFailed, 100),
          "case 15: cancelled while the car stands: cross_failed, its only row spent, nothing left to do");
    Check(FollowStep(failing, FollowAt(&STOP_TOP, on, 33000, dock), notes).act == FollowAct::None,
          "case 15: no second try at this docking");
    Check(FollowStep(failing, FollowAt(nullptr, on, 34000, dock), notes).act == FollowAct::None &&
              FollowStep(failing, FollowAt(&STOP_TOP, on, 44000, dock), notes).act == FollowAct::Wait &&
              FollowStep(failing, FollowAt(&STOP_TOP, on, 45000, dock), notes).act == FollowAct::Cross,
          "case 15: the car left and stands there again: row 0 anew");

    // The vessel leaves under a crossing without an outcome: cut by the bound, the row kept; and the walk ends
    TravelLegs::FollowCrossIssued(failing, STOP_TOP, on, 45000, notes);
    Check(FollowStep(failing, FollowAt(nullptr, on, 49000, dock), notes).act == FollowAct::UnderWay,
          "case 15: no outcome yet within the bound: under way");
    Check(FollowStep(failing, FollowAt(nullptr, on, 50000, dock), notes).act == FollowAct::None &&
              last(notes, TransportEvent::CrossCut, 100) && !failing.crossMs,
          "case 15: no outcome after 5 s and the car gone: cross_cut, nothing to do");
    TravelLegs::FollowCrossing walker;
    FollowStep(walker, FollowAt(&STOP_TOP, on, 60000, away), notes);
    Check(FollowStep(walker, FollowAt(nullptr, on, 61000, away), notes).act == FollowAct::None &&
              walker.stop == TC::NO_STOP,
          "case 15: the car left during the walk to the dock point: nothing to do, the follow code goes on");

    // A refused deck path spends the row; a stop with a window decides by the vessel's progress
    TravelLegs::FollowCrossing refused;
    FollowStep(refused, FollowAt(&STOP_BOTTOM, off, 70000, dock, {1.0f, 0.0f, 0.0f}), notes);
    TravelLegs::FollowRowRefused(refused, STOP_BOTTOM, off, notes);
    Check(last(notes, TransportEvent::CrossFailed, 210) &&
              FollowStep(refused, FollowAt(&STOP_BOTTOM, off, 70500, dock, deck), notes).act == FollowAct::None,
          "case 15: the deck path refused: cross_failed, no leaving row left at this docking");
    TravelLegs::FollowCrossing ship;
    TravelLegs::FollowView late = FollowAt(&STOP_A, on, 80000, DOCK_A0);
    late.stopIndex = 0;
    late.progressMs = 59000;
    Check(FollowStep(ship, late, notes).act == FollowAct::Wait, "case 15: a ship with 1 s of its window left: waits");
    late.progressMs = 10000;
    Check(FollowStep(ship, late, notes).act == FollowAct::Cross, "case 15: 50 s left: crosses at once");

    // Riding with its master, who leaves at the car's next stop (WT70d-b): the leg's deck walk ahead of the docking
    // (the car stands 4 s of 20 there), on the point it stays while the master is aboard, and once the master is off
    // it crosses at once; a refused walk ahead spends no row; a stop without a window is walked at the stop
    TC::Stop windowed = STOP_BOTTOM;
    windowed.hasWindow = true;
    windowed.arriveMs = 0;
    windowed.departMs = 4000;
    windowed.periodMs = 20000;
    TravelLifecycle::Point const place{1.0f, 1.5f, 0.0f};
    auto const riding = [&windowed, dock](std::uint32_t nowMs, std::uint32_t progressMs, TravelLifecycle::Point offset,
                                          TC::Stop const* ahead = nullptr)
    {
        TravelLegs::FollowView view = FollowAt(nullptr, TC::Direction::Board, nowMs, dock, offset);
        view.ahead = ahead ? ahead : &windowed;
        view.progressMs = progressMs;
        return view;
    };
    TravelLegs::FollowCrossing rider;
    notes.clear();
    Check(FollowStep(rider, riding(100000, 14000, place), notes).act == FollowAct::None &&
              !TravelLegs::FollowDue(rider, riding(100000, 14000, place)),
          "case 15: riding with its master, the master's stop 6 s away: the follow code goes on");
    TravelLegs::FollowDecision const walkAhead = FollowStep(rider, riding(101500, 15500, place), notes);
    Check(walkAhead.act == FollowAct::Deck && walkAhead.row == &windowed.leave[0] &&
              TravelLegs::FollowDue(rider, riding(101500, 15500, place)),
          "case 15: the master's stop 4.5 s away: the deck walk ahead to its leaving row's deck point");
    Check(FollowStep(rider, riding(103000, 17000, deck), notes).act == FollowAct::Wait,
          "case 15: on the deck point under way: waits there");
    Check(FollowStep(rider, riding(106200, 200, deck), notes).act == FollowAct::Wait &&
              TravelLegs::FollowDue(rider, riding(106200, 200, deck)),
          "case 15: docked, the master still aboard: stays on the point");
    TravelLegs::FollowView masterOff = FollowAt(&windowed, off, 107300, dock, deck);
    masterOff.progressMs = 1300;
    Check(FollowStep(rider, masterOff, notes).act == FollowAct::Cross && notes.empty(),
          "case 15: the master off, the follower on its deck point: crosses at once by row 0");
    TravelLegs::FollowCrossing aheadRefused;
    FollowStep(aheadRefused, riding(101500, 15500, place), notes);
    TravelLegs::FollowAheadRefused(aheadRefused);
    Check(notes.empty() && FollowStep(aheadRefused, riding(102000, 16000, place), notes).act == FollowAct::None &&
              !TravelLegs::FollowDue(aheadRefused, riding(102000, 16000, place)),
          "case 15: the walk ahead refused: no row spent, not asked again on this ride");
    TravelLegs::FollowView masterOffAtPlace = masterOff;
    masterOffAtPlace.offset = place;
    Check(FollowStep(aheadRefused, masterOffAtPlace, notes).act == FollowAct::Deck,
          "case 15: refused ahead, at the stop the leaving row is tried as ever");
    TravelLegs::FollowCrossing noWindow;
    Check(FollowStep(noWindow, riding(110000, 0, place, &STOP_BOTTOM), notes).act == FollowAct::None,
          "case 15: the master's stop without a window: no walk ahead");

    // The follow action asks for the step while the follower stands near its master (FollowAction::isUseful): due
    // while the vessel stands at a stop with an unspent row of the direction, or while a crossing is under way
    using TravelLegs::FollowDue;
    TravelLegs::FollowCrossing near;
    Check(!FollowDue(near, FollowAt(nullptr, on, 90000, dock)),
          "case 15: near its master, the car at no stop: not due");
    Check(FollowDue(near, FollowAt(&STOP_TOP, on, 90000, dock)),
          "case 15: near its master, the car at a stop with a boarding row: due");
    FollowStep(near, FollowAt(&STOP_TOP, on, 90000, dock), notes);
    Check(FollowDue(near, FollowAt(&STOP_TOP, on, 90500, dock)), "case 15: waiting for the stand: still due");
    FollowStep(near, FollowAt(&STOP_TOP, on, 91000, dock), notes);
    TravelLegs::FollowCrossIssued(near, STOP_TOP, on, 91000, notes);
    Check(FollowDue(near, FollowAt(nullptr, on, 91500, dock)), "case 15: a crossing under way: due, the car gone too");
    FollowStep(near, FollowAt(&STOP_TOP, on, 92000, dock, {}, Passenger::Cancelled), notes);
    Check(!FollowDue(near, FollowAt(&STOP_TOP, on, 92500, dock)), "case 15: the stop's rows spent: not due");
    Check(FollowDue(near, FollowAt(&STOP_BOTTOM, off, 93000, dock, deck)),
          "case 15: aboard at another stop with a leaving row: due");

    // The walk to the dock point (WT70d-b replay 6): a walk the navmesh did not carry the bot on (its move ended
    // without bringing it a reach nearer) or refused for lack of a path gives the dock point up: the row is spent and
    // the next row is walked to, as the rider's DockPointGivenUp; a walk that brought it nearer is walked again
    TravelLegs::FollowView atPier = FollowAt(&STOP_A, on, 120000, {0, 98.0f, 0.0f, 0.0f});
    atPier.stopIndex = 0;
    atPier.progressMs = 10000;
    TravelLegs::FollowCrossing walking;
    notes.clear();
    Check(FollowStep(walking, atPier, notes).act == FollowAct::Walk &&
              TravelLegs::FollowWalkEnded(walking, STOP_A, 20.0f, false, notes) && notes.empty(),
          "case 15: 20 yd from row 0's dock point and no walk issued yet: walks");
    TravelLegs::FollowWalkIssued(walking, 20.0f, 1);
    Check(TravelLegs::FollowWalkEnded(walking, STOP_A, 12.0f, false, notes) && notes.empty() && walking.spent == 0,
          "case 15: the walk ended 8 yd nearer: walked again, no row spent");
    TravelLegs::FollowWalkIssued(walking, 12.0f, 2);
    bool const walkAgain = TravelLegs::FollowWalkEnded(walking, STOP_A, 12.5f, false, notes);
    TravelLegs::FollowDecision const nextRow = FollowStep(walking, atPier, notes);
    Check(!walkAgain && walking.spent == 1 && !notes.empty() && notes.back().event == TransportEvent::CrossFailed &&
              nextRow.act == FollowAct::Walk && nextRow.row == &STOP_A.board[1],
          "case 15: the walk ended no nearer: cross_failed, the next row's dock point is walked to");
    TravelLegs::FollowWalkIssued(walking, 20.0f, 3);
    Check(!TravelLegs::FollowWalkEnded(walking, STOP_A, 20.0f, true, notes) && walking.spent == 2 &&
              FollowStep(walking, atPier, notes).act == FollowAct::None,
          "case 15: the walk refused for lack of a path: the row spent, no row left at this docking");

    // The master on its transport leg from its walk to the dock on (WT70d-b replay 6): its followers check at the
    // scan's pace, and it wakes them once as the leg starts, so a short stand is not slept through
    Trip leader = StartedTrip({MakeLeg(LegType::Walk, NODES[0], PIER_A, {0, 1}, 30.0f),
                               MakeLeg(LegType::Transport, PIER_A, PIER_B, {1, 3}, 240.0f)});
    bool const walkPaces = TravelLegs::PacesFollowers(leader);
    bool const walkWakes = TravelLegs::WakeFollowers(leader);
    TravelLegs::StartLeg(leader, 1, 2000, NodeAt);
    bool const approachPaces = leader.boarding == TravelLegs::Boarding::Approach && TravelLegs::PacesFollowers(leader);
    bool const wakesOnce = TravelLegs::WakeFollowers(leader) && !TravelLegs::WakeFollowers(leader);
    TravelLegs::StartLeg(leader, 1, 3000, NodeAt);
    Check(!walkPaces && !walkWakes && approachPaces && wakesOnce && TravelLegs::WakeFollowers(leader),
          "case 15: the master walks to its dock: its followers at the scan's pace, woken once per transport leg");

    // The follower's pace (BotTravel::PaceMs, WT70d-b): the follow code checks every 1 to 3 s, the leg's rider every
    // scan. While its master is on a transport leg or its step has work the follower checks at the scan's pace too.
    // The master boards late in a short docking (the car stands 4 s of 20, the master aboard with the follower's
    // crossing plus 1 s of the window left); the follower on the dock point crosses in that docking at every phase of
    // its checks, while at a 2 s pace it misses the docking at some phases
    using TravelLegs::FollowPaced;
    TravelLegs::FollowCrossing idle;
    Check(!FollowPaced(idle, FollowAt(nullptr, on, 200000, dock), false),
          "case 15: the master on no vessel and no transport leg: the follow code's own pace");
    Check(FollowPaced(idle, FollowAt(nullptr, on, 200000, dock), true),
          "case 15: the master on a transport leg (waiting at its dock): the scan's pace");
    TC::Stop shortStand = STOP_TOP;
    shortStand.hasWindow = true;
    shortStand.arriveMs = 0;
    shortStand.departMs = 4000;
    shortStand.periodMs = 20000;
    std::uint32_t const needMs =
        static_cast<std::uint32_t>(std::ceil(TC::MoveOf(shortStand.Rows(on)[0], on, 0.0f).seconds * 1000.0f)) +
        TravelLegs::CROSS_SLACK_MS;
    std::uint32_t const masterAboardMs = shortStand.departMs - needMs - 1000;
    auto const crossesInDocking = [&shortStand, masterAboardMs, dock](std::uint32_t phaseMs, bool paced)
    {
        // The car docks at 310 s (its progress 0) and leaves at 314 s; the master is on its leg all along
        std::uint32_t constexpr DOCKS_MS = 310000;
        TravelLegs::FollowCrossing follower;
        std::vector<TravelLegs::TransportNote> ignored;
        for (std::uint32_t nowMs = 300000 + phaseMs; nowMs < DOCKS_MS + shortStand.departMs;)
        {
            std::uint32_t const progressMs = (nowMs + shortStand.periodMs - DOCKS_MS) % shortStand.periodMs;
            bool const masterAboardAtStop = nowMs >= DOCKS_MS + masterAboardMs && progressMs < shortStand.departMs;
            TravelLegs::FollowView view = FollowAt(masterAboardAtStop ? &shortStand : nullptr, on, nowMs, dock);
            view.progressMs = progressMs;
            if (TravelLegs::FollowStep(follower, view, ignored).act == TravelLegs::FollowAct::Cross)
                return true;
            nowMs += paced && FollowPaced(follower, view, true) ? TravelLegs::TRANSPORT_SCAN_MS : 2000;
        }
        return false;
    };
    bool pacedAll = true;
    std::uint32_t slowMisses = 0;
    for (std::uint32_t phaseMs = 0; phaseMs < 2000; phaseMs += 50)
    {
        pacedAll = pacedAll && crossesInDocking(phaseMs, true);
        slowMisses += crossesInDocking(phaseMs, false) ? 0 : 1;
    }
    Check(pacedAll && slowMisses > 0,
          "case 15: the master boards late in a short docking: at the scan's pace the follower crosses in it at every "
          "phase; at a 2 s pace it misses at some");
}

// Case 10: a missed exit rides round (stuck_aboard) and leaves at the second docking
void CheckMissedExit()
{
    using TravelLegs::Boarding;
    using TravelLegs::TransportEvent;

    Trip trip = Aboard();
    Step(trip, Riding(250000, PLACE, Docked(STOP_B, 250000)));
    // On the row's deck point too late: 1 s left at the stop, the walk needs 0.32 s plus 1 s of slack
    Trip late = trip;
    Check(Step(late, Riding(299000, DECK_B0, Docked(STOP_B, 299000))).act == Act::Wait &&
              late.boarding == Boarding::Leave && !TravelLegs::CrossUnderWay(late),
          "case 10: too little time left at the stop: no crossing starts");
    // The ship leaves under the exit walk: the passenger check stops it on the deck
    Trip cut = trip;
    Observation const atRow = Riding(252000, DECK_B0);
    Step(cut, atRow);
    TravelLegs::CrossIssued(cut, atRow);
    Check(Step(cut, With(Riding(301000, DECK_B0, UnderWay(SEA_1)), Passenger::Cancelled)).act == Act::Wait &&
              cut.boarding == Boarding::Ride && cut.leaveSpent == 0 &&
              Words(cut) == "attempt,cross,crossed,boarded,place,leave,cross,cross_cut,stuck_aboard",
          "case 10: the ship left under the exit walk: cross_cut, stuck_aboard, the row is not spent");

    Check(Step(trip, Riding(301000, PLACE, UnderWay(SEA_1))).act == Act::Wait && trip.boarding == Boarding::Ride &&
              Last(trip, TransportEvent::StuckAboard, SHIP, 51),
          "case 10: the ship leaves the far stop with the bot: stuck_aboard (51 s since the leave), rides round");
    Decision const again = Step(trip, Riding(730000, PLACE, Docked(STOP_B, 250000)));
    Check(again.act == Act::Deck && trip.boarding == Boarding::Leave && Last(trip, TransportEvent::Leave, SHIP, 718),
          "case 10: the second docking: leave again");
    Observation const second = Riding(732000, DECK_B0);
    Check(Step(trip, second).act == Act::Disembark, "case 10: crosses by the row");
    TravelLegs::CrossIssued(trip, second);
    Decision const done = Step(trip, With(Seen(DOCK_B0, 734000), Passenger::Exited));
    Check(done.act == Act::Done &&
              Words(trip) == "attempt,cross,crossed,boarded,place,leave,stuck_aboard,leave,cross,crossed,arrived",
          "case 10: and arrives");
}

// Cases 12 and 13: the place aboard is tried once; a fight past the far stop ends by the ride clock
void CheckPlaceAndFight()
{
    using TravelLegs::Recovery;
    using TravelLegs::TransportEvent;

    Trip trip = Aboard(false);
    Check(Step(trip, Riding(13000, DECK_A0)).act == Act::Stand && Step(trip, Riding(14000, DECK_A0)).act == Act::Stand,
          "case 12: the place is asked for until the follower tried");
    TravelLegs::NotePlace(trip, false);
    Check(Last(trip, TransportEvent::Place, SHIP, 0) && Step(trip, Riding(15000, DECK_A0)).act == Act::Wait,
          "case 12: a false draw or a path that is not normal: place 0, the bot stays where it is");
    std::size_t const notes = trip.transportNotes.size();
    TravelLegs::NotePlace(trip, true);
    Check(trip.transportNotes.size() == notes && Step(trip, Riding(16000, DECK_A0)).act == Act::Wait,
          "case 12: once per boarding: no second draw");
    Trip ashore = AtDock();
    TravelLegs::NotePlace(ashore, true);
    Check(Words(ashore) == "attempt", "case 12: no place off the vessel");

    // Case 13: the follower does not run in a fight; the ship passed the far stop meanwhile
    Trip fight = Aboard();
    TravelLegs::NoteLife(fight, TravelLegs::Life::Combat, 200000);
    TravelLegs::NoteLife(fight, TravelLegs::Life::Calm, 1040000);
    Decision const late = Step(fight, Riding(1040000, PLACE, UnderWay(SEA_0)));
    Check(late.act == Act::Fail && late.reason == FailReason::Timeout && Last(fight, TransportEvent::Fight, SHIP, 840),
          "case 13: past the ride clock after a fight aboard: timeout, fight (840 s)");
    Check(TravelLegs::AfterFailure(fight, FailReason::Timeout) == Recovery::Replan && AvoidsRide(fight),
          "case 13: the replan leaves the ride's edge out");
    Trip calm = Aboard();
    Step(calm, With(Seen(SEA_1, 20000, UnderWay(SEA_1)), Passenger::Lost));
    Check(Last(calm, TransportEvent::Lost, SHIP, 8), "case 13: no fight aboard: no fight row");

    // ... and the bot gets off first, by the leaving rows of the stop the ship stands at
    using TravelLegs::OffStep;
    using TravelLegs::StepOffFirst;
    Check(TravelLegs::BeginOffFirst(fight, SHIP, VESSEL, PERIOD, 1041000) &&
              Last(fight, TransportEvent::OffFirst, SHIP, 480),
          "case 13: aboard after the failed leg: off first (one period)");
    Check(StepOffFirst(fight, GetOff(1042000, PLACE, nullptr, UnderWay(SEA_1))) == OffStep::Wait,
          "case 13: under way: waits aboard");
    Observation const stands = GetOff(1210000, PLACE, &STOP_B, Docked(STOP_B, 250000));
    Check(StepOffFirst(fight, stands) == OffStep::Deck && TravelLegs::RowNow(fight, stands).stop == &STOP_B &&
              TravelLegs::RowNow(fight, stands).direction == TC::Direction::Leave &&
              TravelLegs::RowNow(fight, stands).index == 0,
          "case 13: the ship stands at a stop: the deck walk to its first leaving row");
    Observation const atRow = GetOff(1212000, DECK_B0, &STOP_B);
    Check(StepOffFirst(fight, atRow) == OffStep::Disembark, "case 13: on the deck point: crosses by the row");
    TravelLegs::CrossIssued(fight, atRow);
    Check(Last(fight, TransportEvent::Cross, SHIP, 210) && StepOffFirst(fight, atRow) == OffStep::Disembark,
          "case 13: cross (stop 2, leaving, row 0), under way");
    Observation ashoreNow = With(Seen(DOCK_B0, 1214000), Passenger::Exited);
    Check(StepOffFirst(fight, ashoreNow) == OffStep::Off && !fight.offFirst.sinceMs &&
              Last(fight, TransportEvent::Crossed, SHIP, 210) && !TravelLegs::CrossUnderWay(fight),
          "case 13: the passenger check's exit: crossed, off");
}

// WT62: the leg's vessel docks under the bot waiting on the dock point and the passenger check, probing for that
// vessel (BoardingIntent), attached it: boarded without a crossing
void CheckBoardedByIntent()
{
    using TravelLegs::Boarding;
    using TravelLegs::BoardingIntent;
    using TravelLegs::TransportEvent;
    TravelLegs::TransportSight const docked = Docked(STOP_A, 10000);

    Trip approach = StartedTrip({ShipLeg()});
    approach.sight = docked;
    Check(!BoardingIntent(approach), "boarding intent: none on the approach to the dock point");

    Trip waiting = AtDock();
    Check(waiting.boarding == Boarding::Wait && !BoardingIntent(waiting),
          "boarding intent: none while the leg's vessel is not seen");
    Step(waiting, Seen(DOCK_A0, 4000, docked));
    Check(BoardingIntent(waiting) == VESSEL, "boarding intent: the leg's sighted vessel at the dock point");
    Observation docking = Riding(9000, DECK_A0, docked);
    Check(Step(waiting, docking).act == Act::Stand && waiting.boarding == Boarding::Ride &&
              waiting.passengerGuid == VESSEL && Words(waiting) == "attempt,boarded" &&
              Last(waiting, TransportEvent::Boarded, SHIP, 6),
          "the leg's vessel docks under the bot: boarded (6 s waited) without a crossing, rides");
    Check(!BoardingIntent(waiting), "boarding intent: none once the ride owns the vessel");

    Trip other = AtDock();
    Step(other, Seen(DOCK_A0, 4000, docked));
    Observation another = docking;
    another.aboardGuid = VESSEL + 1;
    Step(other, another);
    Check(!other.passengerGuid, "another vessel of the leg's entry: not a boarding");

    Trip pending = AtDock();
    Step(pending, Seen(DOCK_A0, 4000, docked));
    docking.boardMotion = true;
    Step(pending, docking);
    Check(!pending.passengerGuid, "not by intent while the bot's own boarding move is pending");

    Leg const walk = MakeLeg(LegType::Walk, NODES[0], PIER_A, {0, 1}, 30.0f);
    Trip walking = StartedTrip({walk, ShipLeg()});
    walking.sight = docked;
    Check(!BoardingIntent(walking), "boarding intent: none on the walk to the leg");
    // The walk before the leg ends at its node as any walk does and hands the leg to the next tick (the walk's
    // observation holds no stop records: CheckLegHandOver); the leg's approach then goes to the row's dock point
    Check(Go(walking, At({0, 96.0f, 0.0f, 0.0f}, 2000), NodeAt).act == Act::Wait && walking.legIdx == 1 &&
              Near(Go(walking, Seen({0, 96.0f, 0.0f, 0.0f}, 2500), NodeAt).target, DOCK_A0),
          "the walk to a transport leg ends within 5 yd of its node; the approach walks to the dock point");
}

// The get-off before a trip's end, route request or walk aboard a vessel (off_first, bounded by one period); case 14:
// a stop without leaving rows is waited out and the bound gives up
void CheckOffFirst()
{
    using TravelLegs::BeginOffFirst;
    using TravelLegs::OffFirstEnd;
    using TravelLegs::OffStep;
    using TravelLegs::RowNow;
    using TravelLegs::StepOffFirst;
    using TravelLegs::TransportEvent;

    Leg const goalWalk = MakeLeg(LegType::Walk, PIER_B, NODES[4], {3, 4}, 15.0f);
    Trip ashore = StartedTrip({goalWalk});
    Check(!BeginOffFirst(ashore, 0, 0, 300000, 2000, OffFirstEnd::Arrive) && !ashore.offFirst.sinceMs &&
              ashore.transportNotes.empty(),
          "off first: not aboard a vessel: nothing waits");

    // The goal walk ended on the deck: the arrival waits (off_first, the period's 300 s as its bound)
    Trip trip = StartedTrip({goalWalk});
    Check(BeginOffFirst(trip, SHIP, VESSEL, 300000, 2000, OffFirstEnd::Arrive) &&
              trip.offFirst.end == OffFirstEnd::Arrive && Noted(trip, 0, TransportEvent::OffFirst, SHIP, 300),
          "off first: an arrival aboard waits (off_first, 300 s)");
    Check(!BeginOffFirst(trip, SHIP, VESSEL, 300000, 2500, OffFirstEnd::Abort) &&
              trip.offFirst.end == OffFirstEnd::Arrive,
          "off first: one at a time");
    Check(StepOffFirst(trip, GetOff(3000, PLACE, nullptr, UnderWay(SEA_1))) == OffStep::Wait,
          "off first: the ship under way: waits aboard");
    Observation const stands = GetOff(5000, PLACE, &STOP_B, Docked(STOP_B, 250000));
    Check(StepOffFirst(trip, stands) == OffStep::Deck &&
              StepOffFirst(trip, GetOff(5500, PLACE, &STOP_B)) == OffStep::Deck,
          "off first: the ship stands at a stop: walks the deck to the leaving row (between scans too)");
    Trip loss = trip;
    Check(StepOffFirst(loss, Seen(PIER_B, 6000, Gone())) == OffStep::Lost && Last(loss, TransportEvent::Lost, SHIP, 0),
          "off first: loss at a dock cannot authorize arrival");

    // A row fails: the next; all spent: waits the stop out; another stop's rows are unspent
    Trip rows = trip;
    TravelLegs::RowRefused(rows, stands);
    Observation const next = GetOff(6000, PLACE, &STOP_B);
    Check(Last(rows, TransportEvent::CrossFailed, SHIP, 210) && StepOffFirst(rows, next) == OffStep::Deck &&
              RowNow(rows, next).index == 1,
          "off first: the deck path refused: cross_failed, the stop's next leaving row");
    Observation const atSecond = GetOff(7000, DECK_B1, &STOP_B);
    Check(StepOffFirst(rows, atSecond) == OffStep::Disembark, "off first: crosses by row 1");
    TravelLegs::CrossIssued(rows, atSecond);
    Check(StepOffFirst(rows, With(GetOff(8000, DECK_B1, &STOP_B, Docked(STOP_B, 253000)), Passenger::Cancelled)) ==
                  OffStep::Wait &&
              Last(rows, TransportEvent::CrossFailed, SHIP, 211) && rows.offFirst.spent == 2,
          "off first: cancelled while the ship stands: that row is spent too, the stop is waited out");
    Observation const other = GetOff(200000, PLACE, &STOP_A, Docked(STOP_A, 5000));
    Check(StepOffFirst(rows, other) == OffStep::Deck && RowNow(rows, other).stop == &STOP_A &&
              RowNow(rows, other).index == 0,
          "off first: at another stop: its first leaving row");

    Observation const atRow = GetOff(7000, DECK_B0, &STOP_B);
    Check(StepOffFirst(trip, atRow) == OffStep::Disembark, "off first: on the deck point: crosses by the row");
    TravelLegs::CrossIssued(trip, atRow);
    Check(StepOffFirst(trip, With(Seen(DOCK_B0, 9000), Passenger::Exited)) == OffStep::Off && !trip.offFirst.sinceMs &&
              trip.offFirst.end == OffFirstEnd::Arrive && Words(trip) == "off_first,cross,crossed",
          "off first: acknowledged exit releases the arrival");
    Check(BeginOffFirst(trip, LIFT, LIFT, 0, 10000) && trip.offFirst.end == OffFirstEnd::None &&
              trip.offFirst.boundMs == TravelLegs::LEG_SLACK_MS,
          "off first: after one that got off another may start; an unknown period bounds it by 60 s");

    // Case 14: a stop without leaving rows, or a place the index has no record of, is waited out; a period after the
    // start: stuck_aboard, the follower gets a failure, no second wait on the trip
    TC::Stop bare = STOP_B;
    bare.leaveCount = 0;
    Trip stuck = StartedTrip({goalWalk});
    BeginOffFirst(stuck, SHIP, VESSEL, 300000, 2000, OffFirstEnd::Arrive);
    Check(StepOffFirst(stuck, GetOff(3000, PLACE, &bare, Docked(bare, 250000))) == OffStep::Wait &&
              StepOffFirst(stuck, GetOff(4000, PLACE, &bare, Docked(bare, 251000))) == OffStep::Wait,
          "case 14: the ship stands at a stop without leaving rows: waits");
    Check(StepOffFirst(stuck, GetOff(5000, PLACE, nullptr, Docked(STOP_B, 252000))) == OffStep::Wait,
          "case 14: it stands where the index has no stop record: waits");
    Check(StepOffFirst(stuck, GetOff(302000, PLACE, &bare, Docked(bare, 250000))) == OffStep::Wait,
          "case 14: aboard at the bound: still waits");
    Check(StepOffFirst(stuck, GetOff(302001, PLACE, &bare)) == OffStep::GiveUp &&
              Noted(stuck, 1, TransportEvent::StuckAboard, SHIP, 300) && stuck.offFirst.gaveUp &&
              !stuck.offFirst.sinceMs && stuck.offFirst.end == OffFirstEnd::Arrive,
          "case 14: one ms past the period: stuck_aboard, the get-off gives up");
    Check(!BeginOffFirst(stuck, SHIP, VESSEL, 300000, 400000, OffFirstEnd::Abort, "timeout", "abort:timeout"),
          "off first: no second wait on a trip after a period aboard");

    // The Transport event words of WT70c
    using TravelLegs::TransportSource;
    Check(std::string(TransportSource(TransportEvent::OffFirst)) == "off_first" &&
              std::string(TransportSource(TransportEvent::Cross)) == "cross" &&
              std::string(TransportSource(TransportEvent::Crossed)) == "crossed" &&
              std::string(TransportSource(TransportEvent::CrossFailed)) == "cross_failed" &&
              std::string(TransportSource(TransportEvent::CrossCut)) == "cross_cut" &&
              std::string(TransportSource(TransportEvent::NoCrossing)) == "no_crossing" &&
              std::string(TransportSource(TransportEvent::Place)) == "place" &&
              std::string(TransportSource(TransportEvent::OffDeck)) == "off_deck" &&
              std::string(TransportSource(TransportEvent::Fight)) == "fight",
          "events: the Transport sources' words");
    Check(std::string(TravelLegs::ActWord(Act::Stand)) == "stand" &&
              std::string(TravelLegs::ActWord(Act::Deck)) == "deck",
          "events: the acts' words for the hold line");
}
}  // namespace

// The one expiry (NextDeadlineMs) per leg type and pending state: 0 ms left at the bound, overdue one ms after it
void CheckNextDeadline()
{
    auto const bounded = [](Trip const& trip, std::uint32_t nowMs, std::uint32_t legMs, std::string const& what,
                            TravelLegs::RouteWait const* route = nullptr)
    {
        // One ms later on both clocks: each state reads only its own
        Check(TravelLegs::NextDeadlineMs(trip, nowMs, legMs, route) == 0 &&
                  TravelLegs::NextDeadlineMs(trip, nowMs + 1, legMs + 1, route) == -1,
              "next deadline: " + what);
    };
    // Leg-clock legs: the wall time since the leg's start does not count, only legMs
    std::vector<std::pair<Leg, std::uint32_t>> const legs = {
        {MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 100.0f), 360000},
        {MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 3.0f), 20000},
        {MakeLeg(LegType::StaticPortal, NODES[2], NODES[3], {2, 3}, 3.0f), 20000},
        {MakeLeg(LegType::Flight, NODES[5], NODES[6], {5, 6}, 200.0f, FARE), 460000},
        {MakeLeg(LegType::Fly, NODES[0], NODES[1], {0, 1}, 40.0f), 140000},
        {MakeLeg(LegType::Hearth, NODES[0], NODES[3], {}, 10.0f), 40000},
    };
    for (auto const& [leg, bound] : legs)
    {
        Trip trip = StartedTrip({leg}, 0);
        std::string const type = "leg type " + std::to_string(static_cast<int>(leg.type));
        bounded(trip, 9999999, bound, type + " on the leg clock");
        // The follower fails the leg from the same expiry: on the leg clock, whatever the wall time
        Observation seen = At(NODES[leg.type == LegType::Flight ? 5 : 0], 9999999);
        seen.legMs = bound;
        Trip early = trip;
        bool const kept = TravelLegs::StepLeg(early, seen).act != Act::Fail;
        seen.legMs = bound + 1;
        Decision const late = TravelLegs::StepLeg(trip, seen);
        Check(
            kept && late.act == Act::Fail &&
                late.reason == (leg.type == LegType::Portal || leg.type == LegType::StaticPortal ? FailReason::Portal
                                                                                                 : FailReason::Timeout),
            "next deadline: the follower fails " + type + " one ms past it, not at it");
    }
    // Transport: off the vessel from the leg's start (fallback, then a read period), aboard from the boarding
    Trip ship = StartedTrip({ShipLeg()}, 1000);
    bounded(ship, 1000 + 2460000, 0, "a transport wait before its period is read");
    ship.periodMs = 300000;
    bounded(ship, 1000 + 1560000, 0, "a transport wait with a read period of 300 s");
    ship.passengerGuid = SHIP;
    ship.boardedMs = 50000;
    bounded(ship, 50000 + 660000, 999999999, "a ride: RIDE_PERIODS from the boarding, the leg clock ignored");
    // Pending states before the leg: the get-off, then a route answer (the clock may wrap while waiting)
    Trip off = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 100.0f)}, 0);
    TravelLegs::BeginOffFirst(off, SHIP, SHIP, 120000, 5000);
    bounded(off, 5000 + 120000, 999999999, "the get-off: its bound from its start");
    TravelLegs::RouteWait const wait{7, 0, 0xFFFFF000u, true};
    bounded(off, 0xFFFFF000u + TravelRoute::ANSWER_KEEP_MS, 999999999, "a route answer, across the clock's wrap",
            &wait);
    TravelLegs::RouteWait const answered{7, 0, 0, false};
    Trip walk = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 100.0f)}, 0);
    bounded(walk, 9999999, 360000, "no route pending: the leg's own bound", &answered);
    Trip done;
    Check(TravelLegs::NextDeadlineMs(done, 9999999, 9999999) == TravelLegs::NO_DEADLINE,
          "next deadline: none past the last leg");

    // Who moves the bot (OwnerOf): the follower, unless one fact hands it to the passenger code, a taxi or a vehicle
    auto const passenger = TravelLegs::TravelOwner::Passenger;
    Check(TravelLegs::OwnerOf(walk, false, {}) == TravelLegs::TravelOwner::Follower, "owner: the follower walks");
    Check(TravelLegs::OwnerOf(walk, true, {}) == passenger, "owner: a running passenger move");
    Check(TravelLegs::OwnerOf(off, false, {}) == passenger, "owner: the get-off from a ship");
    Check(TravelLegs::OwnerOf(ship, false, {}) == passenger, "owner: a transport leg");
    Check(TravelLegs::OwnerOf(walk, false, {true, false, false}) == passenger, "owner: on a transport");
    Check(TravelLegs::OwnerOf(walk, false, {false, true, false}) == passenger, "owner: in flight on a taxi");
    Check(TravelLegs::OwnerOf(walk, false, {false, false, true}) == passenger, "owner: in a vehicle");
}

int main()
{
    CheckFullRoute();
    CheckTimeouts();
    CheckStuckAndReplans();
    CheckStuckAvoid();
    CheckWrongMapAndFare();
    CheckPredicates();
    CheckWaypointsAndAnswers();
    CheckResetAndDeadline();
    CheckFlightFailureAndDeadline();
    CheckEventSources();
    CheckStaticPortal();
    CheckDeathsAndRecord();
    CheckFlyLeg();
    CheckFlyLegMountOwner();
    CheckLanding();
    CheckPlaces();
    CheckInTheAir();
    CheckCombatLanding();
    CheckBodyZ();
    CheckDescent();
    CheckCrossing();
    CheckLegHandOver();
    CheckStandTest();
    CheckCutCrossing();
    CheckFailedRows();
    CheckNoCrossing();
    CheckWaitClockAndLoss();
    CheckBack();
    CheckDeckReach();
    CheckCarWindow();
    CheckFollowAboard();
    CheckMissedExit();
    CheckPlaceAndFight();
    CheckBoardedByIntent();
    CheckOffFirst();
    CheckNextDeadline();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
