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
// Transport legs (WT19): a ship across maps and an elevator through their states, misses, the last-resort hop and its
// conditions, the timeouts, wrong maps and the Transport event words; the leave at the end (WT19-d: the end node is
// the transport's own stop, a leave ends the leg off the hull; a missed exit is stuck_aboard) and the get-off before
// a trip's end, route request or walk aboard a ship (off_first, bounded by one period).
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <variant>
#include <vector>

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
        Decision const next = TravelLegs::Advance(trip, At(bot, nowMs, money, inFlight), NodeAt);
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
    Check(TravelLegs::StepLeg(trip, At(NODES[0], 359999)).act == Act::Walk, "walk: no timeout before 360 s");
    Decision const late = TravelLegs::StepLeg(trip, At(NODES[0], 360001));
    Check(late.act == Act::Fail && late.reason == FailReason::Timeout, "walk: Fail(timeout) after 360 s");
    Check(TravelLegs::StepLeg(trip, At(NODES[1], 999999)).act == Act::Done, "walk: an arrival after the time is done");

    // A 0-second walk whose end lies 700 yd away: walked, with time for 700 yd at run speed
    Leg const moved = MakeLeg(LegType::Walk, NODES[6], {1, 5700.0f, 5000.0f, 0.0f}, {6}, 0.0f);
    Check(!TravelLegs::IsNoOp(moved), "a 0-second walk whose end moved is not skipped");
    Check(TravelLegs::LegTimeoutMs(moved) == 360000, "a moved walk's timeout uses its straight line (100 s)");
    Check(TravelLegs::IsNoOp(MakeLeg(LegType::Walk, GOAL, GOAL, {}, 0.0f)), "a 0-second walk in place is skipped");
    Check(!TravelLegs::IsNoOp(MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 0.0f)), "a portal is no no-op");

    // Portal: 20 s after the leg began
    Trip portal = StartedTrip({MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 3.0f)}, 0);
    portal.triggered = true;
    Check(TravelLegs::StepLeg(portal, At(NODES[2], 19000)).act == Act::Wait, "portal: waits for the trigger");
    Decision const stuckPortal = TravelLegs::StepLeg(portal, At(NODES[2], 21000));
    Check(stuckPortal.act == Act::Fail && stuckPortal.reason == FailReason::Portal, "portal: Fail(portal) after 20 s");

    // Flight: path seconds x 2 + 60 s before take-off; none in the air
    Leg const flight = MakeLeg(LegType::Flight, NODES[5], NODES[6], {5, 6}, 200.0f, FARE);
    Check(TravelLegs::LegTimeoutMs(flight) == 460000, "flight timeout = 200 s x 2 + 60 s");
    Trip flying = StartedTrip({flight}, 0);
    flying.taxiActivated = true;
    Check(TravelLegs::StepLeg(flying, At(NODES[5], 459000)).act == Act::Wait, "flight: waits for take-off");
    Check(TravelLegs::StepLeg(flying, At(NODES[5], 461000)).reason == FailReason::Timeout,
          "flight: Fail(timeout) when the taxi never takes off");
    Check(TravelLegs::StepLeg(flying, At({1, 3000.0f, 3000.0f, 300.0f}, 900000, 0, true)).act == Act::Ride,
          "flight: no timeout in the air");
}

void CheckStuckAndReplans()
{
    Trip trip = StartedTrip(FullRoute());
    Check(TravelLegs::Advance(trip, At(NODES[1], 2000), NodeAt).act == Act::Walk && trip.next == 2,
          "stuck: the walk heads for node 2");
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
    Decision const wrong = TravelLegs::StepLeg(walk, At(NODES[4], 2000));
    Check(wrong.act == Act::Fail && wrong.reason == FailReason::WrongMap, "a walk on another map fails (wrong map)");

    Trip portal = StartedTrip({MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 3.0f)});
    Check(TravelLegs::StepLeg(portal, At({2, 0.0f, 0.0f, 0.0f}, 2000)).reason == FailReason::WrongMap,
          "a portal leg on a third map fails (wrong map)");

    Leg const flight = MakeLeg(LegType::Flight, NODES[5], NODES[6], {5, 6}, 200.0f, FARE);
    Trip poor = StartedTrip({flight});
    Decision const far = TravelLegs::StepLeg(poor, At(NODES[4], 2000, FARE - 1));
    Check(far.act == Act::Fail && far.reason == FailReason::Fare, "an unaffordable flight fails before the walk");
    Decision const there = TravelLegs::StepLeg(poor, At(NODES[5], 2000, FARE - 1));
    Check(there.act == Act::Fail && there.reason == FailReason::Fare && !poor.taxiActivated,
          "an unaffordable flight never reaches the flight master's talk");
    Trip rich = StartedTrip({flight});
    Check(TravelLegs::StepLeg(rich, At(NODES[4], 2000, FARE)).act == Act::Walk, "an affordable flight walks first");
    Check(TravelLegs::StepLeg(rich, At(NODES[5], 2000, FARE)).act == Act::TalkFlightMaster,
          "an affordable flight talks at the master");
}

void CheckPredicates()
{
    Point const sevenYards{0, 107.0f, 0.0f, 0.0f};
    // The goal is reached within 10 yd, a waypoint before it within 5 yd
    Trip last = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 20.0f)});
    Check(TravelLegs::Advance(last, At(sevenYards, 2000), NodeAt).act == Act::Arrived, "the goal counts at 7 yd");
    Trip middle = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 20.0f),
                               MakeLeg(LegType::Walk, NODES[1], NODES[2], {1, 2}, 20.0f)});
    Decision const notYet = TravelLegs::Advance(middle, At(sevenYards, 2000), NodeAt);
    Check(notYet.act == Act::Walk && middle.legIdx == 0, "a waypoint before the goal does not count at 7 yd");
    // A path past a node: the waypoint after the current one counts too
    Trip skip = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[2], {0, 1, 2}, 30.0f),
                             MakeLeg(LegType::Walk, NODES[2], {0, 300.0f, 0.0f, 0.0f}, {2}, 20.0f)});
    TravelLegs::Advance(skip, At(NODES[2], 2000), NodeAt);
    Check(skip.legIdx == 1, "reaching the waypoint after the current one passes both");

    // Portal across maps: done on the exit's map; within one map: done near the exit
    Trip cross = StartedTrip({MakeLeg(LegType::Portal, NODES[2], NODES[3], {2, 3}, 3.0f)});
    Check(TravelLegs::StepLeg(cross, At({0, 150.0f, 0.0f, 0.0f}, 2000)).act == Act::Walk,
          "portal: walk to its node first");
    Check(TravelLegs::StepLeg(cross, At(NODES[2], 2000)).act == Act::UseTrigger, "portal: trigger at its node");
    // On the way from the node to a trigger centre 8 yd further on: no turning back to the node
    Check(TravelLegs::StepLeg(cross, At({0, 208.0f, 0.0f, 0.0f}, 2500)).act == Act::UseTrigger,
          "portal: past its node the bot keeps heading for the trigger");
    Check(TravelLegs::StepLeg(cross, At({1, 1300.0f, 1300.0f, 0.0f}, 3000)).act == Act::Done,
          "portal: done anywhere on the exit's map");
    Trip inside = StartedTrip({MakeLeg(LegType::Portal, NODES[1], NODES[2], {1, 2}, 3.0f)});
    inside.triggered = true;
    Check(TravelLegs::StepLeg(inside, At({0, 230.0f, 0.0f, 0.0f}, 3000)).act == Act::Done,
          "portal within a map: done 30 yd from the exit");

    // Flight: done on landing within 30 yd; a landing elsewhere fails
    Leg const flight = MakeLeg(LegType::Flight, NODES[5], NODES[6], {5, 6}, 200.0f, FARE);
    Trip landed = StartedTrip({flight});
    landed.taxiActivated = true;
    TravelLegs::StepLeg(landed, At({1, 3000.0f, 3000.0f, 300.0f}, 2000, 0, true));
    Check(TravelLegs::StepLeg(landed, At({1, 5020.0f, 5000.0f, 0.0f}, 3000)).act == Act::Done,
          "flight: done on landing 20 yd from its end");
    Trip astray = StartedTrip({flight});
    astray.taxiActivated = true;
    TravelLegs::StepLeg(astray, At({1, 3000.0f, 3000.0f, 300.0f}, 2000, 0, true));
    Decision const elsewhere = TravelLegs::StepLeg(astray, At({1, 5100.0f, 5000.0f, 0.0f}, 3000));
    Check(elsewhere.act == Act::Fail && elsewhere.reason == FailReason::Landing, "flight: a landing 100 yd off fails");

    // The hearthstone leg: cast it where the bot stands, done within 40 yd of the bind (its map), the cast and a
    // cooldown wait are inside the timeout
    Point const bind{1, 1300.0f, 1300.0f, 0.0f};
    Trip hearth = StartedTrip({MakeLeg(LegType::Hearth, NODES[0], bind, {}, 10.0f)});
    Check(TravelLegs::StepLeg(hearth, At(NODES[0], 2000)).act == Act::UseHearth, "hearth: casts at the start");
    Check(TravelLegs::StepLeg(hearth, At({1, 1330.0f, 1300.0f, 0.0f}, 12000)).act == Act::Done,
          "hearth: done within 40 yd of the bind");
    Check(TravelLegs::StepLeg(hearth, At({1, 1345.0f, 1300.0f, 0.0f}, 12000)).reason == FailReason::WrongMap,
          "hearth: 45 yd away is not there; on the bind's map it is no start map either");
    Check(TravelLegs::StepLeg(hearth, At(NODES[0], 1000 + 40000)).act == Act::UseHearth, "hearth: 40 s are allowed");
    Decision const lateHearth = TravelLegs::StepLeg(hearth, At(NODES[0], 1000 + 40001));
    Check(lateHearth.act == Act::Fail && lateHearth.reason == FailReason::Timeout, "hearth: past 40 s fails");
    Trip cooling = StartedTrip({MakeLeg(LegType::Hearth, NODES[0], bind, {}, 130.0f)});
    Check(TravelLegs::StepLeg(cooling, At(NODES[0], 1000 + 150000)).act == Act::UseHearth,
          "hearth: a 2 minute cooldown wait is inside the timeout");
    Check(TravelLegs::StepLeg(cooling, At(NODES[0], 1000 + 160001)).reason == FailReason::Timeout,
          "hearth: the timeout is the leg's seconds plus 30 s");
    Trip hearthThenWalk = StartedTrip(
        {MakeLeg(LegType::Hearth, NODES[0], bind, {}, 10.0f), MakeLeg(LegType::Walk, bind, GOAL, {}, 50.0f)});
    Check(TravelLegs::Advance(hearthThenWalk, At(bind, 12000), NodeAt).act == Act::Walk && hearthThenWalk.legIdx == 1,
          "hearth: the walk after it starts on the bind's map");

    // A route of no-op walks only has arrived
    Trip none = StartedTrip({MakeLeg(LegType::Walk, GOAL, GOAL, {}, 0.0f)});
    Check(TravelLegs::Advance(none, At(GOAL, 2000), NodeAt).act == Act::Arrived, "a route of no-ops has arrived");
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
    Check(!TravelLegs::WaitExpired(wait, 0x0000D000u) && TravelLegs::WaitExpired(wait, 0x0000F000u),
          "the wait expires after 60 s, when the service drops the answer");

    // A late or lost answer is asked again three times per trip; the fourth fails the leg (NoAnswer)
    Trip asking;
    for (int ask = 1; ask <= TravelLegs::MAX_RESUBMITS; ++ask)
        Check(TravelLegs::Resubmit(asking), "late answer " + std::to_string(ask) + " of 3 is asked again");
    Check(!TravelLegs::Resubmit(asking) && asking.replans == 0, "the fourth late answer fails the leg");
}

// NewRpgInfo's shape for TravelLegs::AfterReset: a status variant, its start and the request counter beside other state
struct FakeInfo
{
    struct Idle
    {
    };
    struct Travel
    {
        Trip trip;
    };
    struct Errands
    {
        std::uint32_t vendor{0};
    };
    std::variant<Idle, Travel, Errands> data{Idle{}};
    std::uint32_t startT{0};
    std::uint32_t routeRequests{0};
    std::uint16_t constraints{0};
    float nearestMoveFarDis{1.0e9f};
};

void CheckResetAndDeadline()
{
    FakeInfo travelling;
    travelling.data = FakeInfo::Travel{StartedTrip(FullRoute())};
    std::get<FakeInfo::Travel>(travelling.data).trip.legIdx = 2;  // on the portal leg
    travelling.startT = 1234;
    travelling.routeRequests = 17;
    travelling.constraints = 48;
    travelling.nearestMoveFarDis = 3.0f;

    // A far teleport (the portal's worldport) keeps the trip where it was
    FakeInfo const ported = TravelLegs::AfterReset<FakeInfo::Travel>(travelling, true);
    FakeInfo::Travel const* kept = std::get_if<FakeInfo::Travel>(&ported.data);
    Check(kept && kept->trip.legIdx == 2 && kept->trip.legs.size() == FullRoute().size() && ported.startT == 1234,
          "a far teleport keeps the trip on its leg");
    Check(ported.routeRequests == 17 && ported.nearestMoveFarDis == 1.0e9f,
          "a far teleport keeps the request ids and resets the MOVE_FAR state");
    // The kept portal leg is done on the exit's map at the next tick
    Trip portal = kept ? kept->trip : Trip{};
    Check(kept && TravelLegs::StepLeg(portal, At({1, 1000.0f, 1000.0f, 0.0f}, 3000)).act == Act::Done,
          "the kept portal leg is done on the exit's map");

    // Any other full reset ends the trip but keeps the request ids
    FakeInfo const reset = TravelLegs::AfterReset<FakeInfo::Travel>(travelling, false);
    Check(std::holds_alternative<FakeInfo::Idle>(reset.data) && reset.startT == 0 && reset.routeRequests == 17,
          "a full reset ends the trip and keeps the request ids");
    Check(ported.constraints == 48 && reset.constraints == 48,
          "a far teleport and a full reset keep a test's constraints");
    // Other statuses start over on a far teleport, as before
    FakeInfo errands;
    errands.data = FakeInfo::Errands{42};
    errands.startT = 99;
    errands.routeRequests = 5;
    FakeInfo const away = TravelLegs::AfterReset<FakeInfo::Travel>(errands, true);
    Check(std::holds_alternative<FakeInfo::Idle>(away.data) && away.startT == 0 && away.routeRequests == 5,
          "a far teleport resets other statuses (request ids kept)");

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
    trip.legIdx = 1;
    Check(TravelLegs::FirstFlightHop(trip).first == TravelLegs::NO_NODE, "taxi: a walk leg has no flight hop");
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
    Check(TravelLegs::StepLeg(cross, At({0, 150.0f, 0.0f, 0.0f}, 2000)).act == Act::Walk,
          "static portal: walk to its node first");
    Decision const use = TravelLegs::StepLeg(cross, At(NODES[2], 2000));
    Check(use.act == Act::UseObject && use.target.x == NODES[2].x, "static portal: use the object at its node");
    Check(TravelLegs::StepLeg(cross, At({0, 208.0f, 0.0f, 0.0f}, 2500)).act == Act::UseObject,
          "static portal: past its node the bot keeps heading for the object");
    auto const edge = TravelLegs::StuckEdge(cross);
    Check(edge.first == 2 && edge.second == 3, "static portal: its edge is node 2 -> node 3");
    cross.triggered = true;
    Check(TravelLegs::StepLeg(cross, At(NODES[2], 3000)).act == Act::Wait, "static portal: waits once used");
    Check(TravelLegs::StepLeg(cross, At({1, 1300.0f, 1300.0f, 0.0f}, 4000)).act == Act::Done,
          "static portal: done anywhere on the exit's map");
    Trip inside = StartedTrip({MakeLeg(LegType::StaticPortal, NODES[1], NODES[2], {1, 2}, 3.0f)});
    inside.triggered = true;
    Check(TravelLegs::StepLeg(inside, At(NODES[1], 3000)).act == Act::Wait,
          "static portal within a map: not done at its own node");
    Check(TravelLegs::StepLeg(inside, At({0, 230.0f, 0.0f, 0.0f}, 3000)).act == Act::Done,
          "static portal within a map: done 30 yd from the exit");
    Trip stuck = StartedTrip({leg}, 0);
    stuck.triggered = true;
    Decision const late = TravelLegs::StepLeg(stuck, At(NODES[2], 21000));
    Check(late.act == Act::Fail && late.reason == FailReason::Portal, "static portal: Fail(portal) after 20 s");
    Trip wrong = StartedTrip({leg});
    Decision const away = TravelLegs::StepLeg(wrong, At({530, 0.0f, 0.0f, 0.0f}, 2000));
    Check(away.act == Act::Fail && away.reason == FailReason::WrongMap, "static portal: Fail(wrong_map) elsewhere");
    // Through and on: the far side's walk follows
    Trip through = StartedTrip({leg, MakeLeg(LegType::Walk, NODES[3], NODES[4], {3, 4}, 15.0f)});
    through.triggered = true;
    Decision const after = TravelLegs::Advance(through, At(NODES[3], 3000), NodeAt);
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
    for (int r = 0; r <= static_cast<int>(FailReason::NoWalk); ++r)
    {
        FailReason const reason = static_cast<FailReason>(r);
        std::string const word = TravelLegs::FailWord(reason);
        reasons = reasons && word != "?" && TravelLegs::LegFailedSource(reason) == "leg_failed:" + word &&
                  TravelLegs::AbortSource(reason) == "abort:" + word;
    }
    Check(reasons, "events: leg_failed:<reason> and abort:<reason> for every reason");
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
    Check(TravelLegs::Advance(trip, At(NODES[1], 2000), NodeAt).act == Act::Walk && trip.next == 2,
          "deaths: the walk heads for node 2");
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
    Decision const in =
        TravelLegs::Advance(trip, Flying({1, outside.x + 1.0f, 0.0f, 11.0f}, 30000, true, false), NodeAt);
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
    Check(TravelLegs::StepLeg(trip, Flying(takeoff, 1000, false, false)).act == Act::Mount,
          "fly: on foot it mounts first");
    TravelLegs::NoteMountTry(trip, 1000);
    Check(TravelLegs::StepLeg(trip, Flying(takeoff, 3000, false, false)).act == Act::Wait,
          "fly: no second mount cast within 5 s");
    Check(TravelLegs::StepLeg(trip, Flying(takeoff, 6000, false, false)).act == Act::Mount,
          "fly: the mount is tried again after 5 s");
    TravelLegs::NoteMountTry(trip, 6000);
    Decision const up = TravelLegs::StepLeg(trip, Flying(takeoff, 7000, true, false));
    Check(up.act == Act::Fly && about(up.target.x, 30.0f) && up.target.z == 80.0f,
          "fly: mounted, it climbs to the climb point");
    Check(TravelLegs::NeedsHeightCheck(trip), "fly: the height check is due for the climb point");
    Check(!TravelLegs::CheckHeight(trip, {20.0f, 40.0f}) && !TravelLegs::NeedsHeightCheck(trip) && trip.climbs == 0,
          "fly: open ground below: no raise, one check per waypoint");
    Decision const air = TravelLegs::StepLeg(trip, Flying({1, 28.0f, 0.0f, 75.0f}, 9000, true, true));
    Check(air.act == Act::Fly && air.target.x == 100.0f && trip.seenInFlight && trip.peakZ == 75.0f,
          "fly: past the climb point it heads for the first flight point");
    Check(TravelLegs::CheckHeight(trip, {60.0f, 110.0f, TravelLegs::FLY_NO_GROUND - 1.0f}) && trip.climbs == 1 &&
              trip.waypoints[trip.next].at.z == 140.0f,
          "fly: a 110 yd building ahead raises the waypoint to 110 + 30 yd");
    Check(!TravelLegs::CheckHeight(trip, {500.0f}) && trip.climbs == 1, "fly: a waypoint is checked once");
    Decision const over = TravelLegs::StepLeg(trip, Flying({1, 100.0f, 0.0f, 138.0f}, 20000, true, true));
    Check(over.act == Act::Fly && over.target.x == 300.0f && trip.peakZ == 138.0f,
          "fly: over the building, on to the next flight point");
    Decision const past = TravelLegs::StepLeg(trip, Flying({1, 500.0f, 0.0f, 90.0f}, 40000, true, true));
    Check(past.act == Act::Fly && past.target.x == 570.0f && past.target.z == 90.0f,
          "fly: a waypoint passed counts, then the descent point");
    Trip atLanding = trip;
    atLanding.next = 6;
    Check(!TravelLegs::NeedsHeightCheck(atLanding), "fly: the landing is never raised");
    Check(TravelLegs::StepLeg(trip, Flying({1, 600.0f, 0.0f, 13.0f}, 50000, true, false)).act == Act::Done,
          "fly: the landing reached: done");

    // Down near the end (within 30 yd) is done; down elsewhere after the takeoff fails as landing
    Trip near = StartedTrip({leg}, 0);
    TravelLegs::StepLeg(near, Flying({1, 50.0f, 0.0f, 80.0f}, 5000, true, true));
    Check(TravelLegs::StepLeg(near, Flying({1, 590.0f, 10.0f, 12.0f}, 30000, true, false)).act == Act::Done,
          "fly: down within 30 yd of the end: done");
    Trip fell = StartedTrip({leg}, 0);
    TravelLegs::StepLeg(fell, Flying({1, 50.0f, 0.0f, 80.0f}, 5000, true, true));
    Decision const down = TravelLegs::StepLeg(fell, Flying({1, 300.0f, 0.0f, 10.0f}, 20000, false, false));
    Check(down.act == Act::Fail && down.reason == FailReason::Landing, "fly: down away from the end fails as landing");
    Check(TravelLegs::AfterFailure(fell, FailReason::Landing) == Recovery::Replan && fell.noFly && fell.replans == 1,
          "fly: a failed fly leg replans without flying");

    // The leg's timeout in the air; the takeoff's failures; a wrong map
    Trip slow = StartedTrip({leg}, 0);
    TravelLegs::StepLeg(slow, Flying({1, 50.0f, 0.0f, 80.0f}, 5000, true, true));
    Decision const late = TravelLegs::StepLeg(slow, Flying({1, 300.0f, 50.0f, 80.0f}, 141000, true, true));
    Check(late.act == Act::Fail && late.reason == FailReason::Timeout, "fly: past 140 s in the air: timeout");
    Trip tries = StartedTrip({leg}, 0);
    bool triedThrice = true;
    for (std::uint32_t const at : {1000u, 6000u, 11000u})
    {
        triedThrice = triedThrice && TravelLegs::StepLeg(tries, Flying(takeoff, at, false, false)).act == Act::Mount;
        TravelLegs::NoteMountTry(tries, at);
    }
    Check(triedThrice && tries.mountTries == 3, "fly: three real mount tries 5 s apart");
    Check(TravelLegs::StepLeg(tries, Flying(takeoff, 13000, false, false)).act == Act::Wait,
          "fly: the third try's cast gets its 5 s");
    Decision const unmounted = TravelLegs::StepLeg(tries, Flying(takeoff, 16000, false, false));
    Check(unmounted.act == Act::Fail && unmounted.reason == FailReason::Takeoff,
          "fly: no flying mount after three real tries: takeoff");
    Trip skipped = StartedTrip({leg}, 0);
    bool askedAgain = true;
    for (std::uint32_t at = 1000; at <= 61000; at += 1000)
        askedAgain = askedAgain && TravelLegs::StepLeg(skipped, Flying(takeoff, at, false, false)).act == Act::Mount;
    Check(askedAgain && skipped.mountTries == 0, "fly: tries the glue skipped (combat, a cast) do not count");
    Trip inside = StartedTrip({leg}, 0);
    Observation indoors = Flying(takeoff, 1000, false, false);
    indoors.takeoffHere = false;
    Decision const noTakeoff = TravelLegs::StepLeg(inside, indoors);
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
    Trip overCity = StartedTrip({leg}, 0);
    TravelLegs::StepLeg(overCity, Flying({1, 50.0f, 0.0f, 80.0f}, 5000, true, true));
    Observation aloft = Flying({1, 150.0f, 0.0f, 80.0f}, 9000, true, true);
    aloft.takeoffHere = false;
    Check(TravelLegs::StepLeg(overCity, aloft).act == Act::Fly, "fly: in the air the spot below does not matter");
    Trip moved = StartedTrip({leg}, 0);
    Decision const away = TravelLegs::StepLeg(moved, Flying({0, 50.0f, 0.0f, 80.0f}, 5000, true, true));
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

// Transport legs (WT19): a ship from pier A (map 0) to pier B (map 1) and an elevator from its top to its bottom
std::uint32_t const SHIP = 164871;
std::uint32_t const LIFT = 4170;
Point const PIER_A = NODES[1];
Point const PIER_B = NODES[3];
Point const SHIP_AT_A{0, 130.0f, 0.0f, 0.0f};      // stopped 30 yd off pier A
Point const SHIP_AT_B{1, 1000.0f, 1040.0f, 0.0f};  // 40 yd off pier B
Point const DECK{0, 112.0f, 0.0f, 2.0f};           // 12 yd from pier A, 18 yd from the ship's centre
Point const SEA_0{0, 2000.0f, 0.0f, 0.0f};
Point const SEA_1{1, 3000.0f, 1000.0f, 0.0f};
Point const LIFT_TOP{0, 500.0f, 500.0f, 50.0f};
Point const LIFT_BOTTOM{0, 500.0f, 500.0f, 10.0f};

Leg ShipLeg()
{
    Leg leg = MakeLeg(LegType::Transport, PIER_A, PIER_B, {1, 3}, 240.0f);
    leg.object = SHIP;
    return leg;
}

Leg LiftLeg()
{
    Leg leg = MakeLeg(LegType::Transport, LIFT_TOP, LIFT_BOTTOM, {}, 8.6f);
    leg.object = LIFT;
    return leg;
}

// A scan that found the transport at `at`
TravelLegs::TransportSight Sight(Point at, bool moving, bool isStatic = false, bool unseen = false,
                                 std::uint32_t periodMs = 0)
{
    TravelLegs::TransportSight sight;
    sight.scanned = true;
    sight.found = true;
    sight.moving = moving;
    sight.isStatic = isStatic;
    sight.unseen = unseen;
    sight.periodMs = periodMs;
    sight.at = at;
    return sight;
}

TravelLegs::TransportSight Gone()
{
    TravelLegs::TransportSight sight;
    sight.scanned = true;
    return sight;
}

Observation On(Point bot, std::uint32_t nowMs, std::uint32_t aboard, TravelLegs::TransportSight sight = {})
{
    Observation seen = At(bot, nowMs);
    seen.aboard = aboard;
    seen.aboardGuid = aboard;
    seen.transport = sight;
    return seen;
}

Observation Boarded(Point bot, std::uint32_t nowMs, std::uint32_t entry, TravelLegs::TransportSight sight = {})
{
    Observation seen = On(bot, nowMs, entry, sight);
    seen.passengerOutcome = TravelLifecycle::Outcome::Boarded;
    return seen;
}

Observation Exited(Point bot, std::uint32_t nowMs)
{
    Observation seen = On(bot, nowMs, 0);
    seen.passengerOutcome = TravelLifecycle::Outcome::Exited;
    return seen;
}

bool Near(Point const& a, Point const& b) { return a.mapId == b.mapId && TravelRoute::Distance(a, b) < 0.01f; }

bool Noted(Trip const& trip, std::size_t index, TravelLegs::TransportEvent event, std::uint32_t entry,
           std::uint32_t amount)
{
    return index < trip.transportNotes.size() && trip.transportNotes[index].event == event &&
           trip.transportNotes[index].entry == entry && trip.transportNotes[index].amount == amount;
}

// A trip whose first leg is a transport leg the follower already looked at for a quay and found none (SeekQuay): the
// bot waits at the dock node
Trip SoughtTrip(std::vector<Leg> legs, std::uint32_t nowMs = 1000)
{
    Trip trip = StartedTrip(std::move(legs), nowMs);
    TravelLegs::SetQuay(trip, nullptr);
    return trip;
}

void CheckTransport()
{
    using TravelLegs::Boarding;
    using TravelLegs::StepLeg;
    using TravelLegs::TransportEvent;

    // A ship across maps: approach, wait, board, ride, walk off, done; the next leg starts
    Trip trip = SoughtTrip({ShipLeg(), MakeLeg(LegType::Walk, PIER_B, NODES[4], {3, 4}, 15.0f)});
    Decision const approach = StepLeg(trip, On({0, 70.0f, 0.0f, 0.0f}, 2000, 0));
    Check(approach.act == Act::Walk && Near(approach.target, PIER_A) && trip.boarding == Boarding::Approach,
          "transport: walks to the dock node first");
    Check(StepLeg(trip, On(PIER_A, 3000, 0, Gone())).act == Act::Wait && trip.boarding == Boarding::Wait &&
              Noted(trip, 0, TransportEvent::Attempt, SHIP, 2),
          "transport: at the dock node: attempt (2 s walk), waits");
    Check(StepLeg(trip, On(PIER_A, 4000, 0, Sight(SHIP_AT_A, true))).act == Act::Wait,
          "transport: a ship under way beside the dock is not docked");
    Check(StepLeg(trip, On(PIER_A, 4500, 0, Sight({0, 400.0f, 0.0f, 0.0f}, false))).act == Act::Wait,
          "transport: a ship stopped 300 yd away is not docked here");
    Decision const board = StepLeg(trip, On(PIER_A, 5000, 0, Sight(SHIP_AT_A, false)));
    Check(board.act == Act::Board && Near(board.target, SHIP_AT_A) && trip.boarding == Boarding::Board,
          "transport: boards the docked ship (target: its position)");
    Check(StepLeg(trip, On(DECK, 6000, 0)).act == Act::Board, "transport: between scans the last scan holds");
    Observation pending = On(DECK, 6500, SHIP);
    pending.boardMotion = true;
    Check(StepLeg(trip, pending).act == Act::Wait && trip.boarding == Boarding::Board,
          "transport: pointer alone cannot acknowledge a boarded leg while its own boarding move is pending");
    Check(StepLeg(trip, Boarded(DECK, 7000, SHIP, Sight(SHIP_AT_A, false))).act == Act::Wait &&
              trip.boarding == Boarding::Ride && Noted(trip, 1, TransportEvent::Boarded, SHIP, 4),
          "transport: aboard: rides (boarded after 4 s at the dock)");
    Check(StepLeg(trip, On(SEA_0, 500000, SHIP, Sight(SEA_0, true))).act == Act::Wait,
          "transport: aboard at sea: waits");
    Check(StepLeg(trip, On(SEA_1, 600000, SHIP, Sight(SEA_1, true))).act == Act::Wait,
          "transport: the ride goes on on the end's map");
    Decision const off = StepLeg(trip, On({1, 1000.0f, 1030.0f, 3.0f}, 700000, SHIP, Sight(SHIP_AT_B, false)));
    Check(off.act == Act::Disembark && Near(off.target, PIER_B) && trip.boarding == Boarding::Leave &&
              Noted(trip, 2, TransportEvent::Leave, SHIP, 693),
          "transport: docked at the end: leave (693 s aboard), walks off");
    Trip const pendingExit = trip;
    Check(StepLeg(trip, On(PIER_B, 700500, 0)).reason == FailReason::TransportLost,
          "transport: membership disappearance at destination is a loss without exit acknowledgment");
    trip = pendingExit;
    Decision const next = TravelLegs::Advance(trip, Exited({1, 1000.0f, 1015.0f, 0.0f}, 701000), NodeAt);
    Check(next.act == Act::Walk && trip.legIdx == 1 && Noted(trip, 3, TransportEvent::Arrived, SHIP, 694) &&
              trip.transportNotes.size() == 4,
          "transport: acknowledged exit advances the leg and starts normal walking");

    // The passenger list catching up within the grace after the departure is no miss
    Trip late = SoughtTrip({ShipLeg()});
    StepLeg(late, On(PIER_A, 2000, 0, Sight(SHIP_AT_A, false)));
    Check(late.boarding == Boarding::Board, "transport: at the dock node while it stands: boards at once");
    Check(StepLeg(late, On(DECK, 3000, 0, Sight({0, 140.0f, 0.0f, 0.0f}, true))).act == Act::Wait && !late.misses,
          "transport: departed: waits out the grace");
    Check(StepLeg(late, Boarded(DECK, 4000, SHIP)).act == Act::Wait && late.boarding == Boarding::Ride && !late.misses,
          "transport: aboard within the grace: no miss");

    // Departures without the bot: each a miss after the grace, then the next departure; no hop before the third miss
    Trip missing = SoughtTrip({ShipLeg()});
    bool boardsOnly = true;
    bool counted = true;
    std::uint32_t nowMs = 2000;
    for (std::uint8_t miss = 1; miss <= TravelLegs::HOP_AFTER_MISSES; ++miss)
    {
        boardsOnly =
            boardsOnly &&
            StepLeg(missing, On(PIER_A, nowMs, 0, Sight(SHIP_AT_A, false, false, true))).act == Act::Board &&
            StepLeg(missing, On(DECK, nowMs + 4000, 0, Sight(SHIP_AT_A, false, false, true))).act == Act::Board &&
            !TravelLegs::WantsSightCheck(missing);
        Decision const grace = StepLeg(missing, On(DECK, nowMs + 5000, 0, Sight({0, 160.0f, 0.0f, 0.0f}, true)));
        Decision const gone = StepLeg(missing, On(DECK, nowMs + 8000, 0));
        counted = counted && grace.act == Act::Wait && gone.act == Act::Wait && missing.misses == miss &&
                  missing.boarding == Boarding::Wait &&
                  Noted(missing, missing.transportNotes.size() - 1, TransportEvent::Missed, SHIP, miss);
        nowMs += 300000;
    }
    Check(boardsOnly, "transport: before the third miss no hop and no sight check, unseen or not");
    Check(counted && missing.transportNotes.size() == 4, "transport: three departures without the bot: three misses");
    Decision const back = StepLeg(missing, On({0, 140.0f, 0.0f, 0.0f}, nowMs - 1000, 0));
    Check(back.act == Act::Walk && Near(back.target, PIER_A), "transport: waiting 40 yd off the dock node: walks back");

    // The fourth docking: the hop only after HOP_TRY_MS of boarding, unseen, within HOP_MAX_YARDS, on a scan; once
    Check(StepLeg(missing, On(PIER_A, nowMs, 0, Sight(SHIP_AT_A, false, false, true))).act == Act::Board &&
              TravelLegs::WantsSightCheck(missing),
          "transport: after three misses: boards first and asks for the sight check");
    Check(StepLeg(missing, On(DECK, nowMs + 2000, 0, Sight(SHIP_AT_A, false, false, true))).act == Act::Board,
          "transport: no hop before HOP_TRY_MS");
    Check(StepLeg(missing, On(DECK, nowMs + 4000, 0, Sight(SHIP_AT_A, false, false, false))).act == Act::Board,
          "transport: no hop while a real player sees it");
    Check(StepLeg(missing, On(PIER_A, nowMs + 4500, 0, Sight(SHIP_AT_A, false, false, true))).act == Act::Board,
          "transport: no hop from 30 yd (more than HOP_MAX_YARDS)");
    Check(StepLeg(missing, On(DECK, nowMs + 5000, 0)).act == Act::Board, "transport: no hop between scans");
    Decision const hop = StepLeg(missing, On(DECK, nowMs + 6000, 0, Sight(SHIP_AT_A, false, false, true)));
    Check(hop.act == Act::Hop && Near(hop.target, SHIP_AT_A) && missing.hopped &&
              Noted(missing, 4, TransportEvent::Hop, SHIP, 3),
          "transport: the hop: unseen, close, after three misses, counted with them");
    Check(StepLeg(missing, On(DECK, nowMs + 7000, 0, Sight(SHIP_AT_A, false, false, true))).act == Act::Board &&
              !TravelLegs::WantsSightCheck(missing),
          "transport: one hop per leg");
    Check(StepLeg(missing, Boarded(SHIP_AT_A, nowMs + 8000, SHIP, Sight(SHIP_AT_A, false))).act == Act::Wait &&
              missing.boarding == Boarding::Ride && Noted(missing, 5, TransportEvent::Boarded, SHIP, 908),
          "transport: aboard after the hop (boarded: seconds since the bot reached the dock)");
    std::size_t const notes = missing.transportNotes.size();
    TravelLegs::StartLeg(missing, 0, nowMs + 9000, NodeAt);
    Check(!missing.misses && !missing.hopped && missing.boarding == Boarding::Approach &&
              missing.transportNotes.size() == notes,
          "transport: a new leg counts anew; the notes wait for the follower");

    // An elevator (static): its stop is where it stood still for two scans at the floor
    Trip lift = SoughtTrip({LiftLeg()});
    Point const down{0, 505.0f, 500.0f, 10.0f};
    Point const half{0, 505.0f, 500.0f, 30.0f};
    Point const up{0, 505.0f, 500.0f, 50.0f};
    StepLeg(lift, On(LIFT_TOP, 2000, 0, Sight(down, false, true)));
    Check(StepLeg(lift, On(LIFT_TOP, 3000, 0, Sight(down, false, true))).act == Act::Wait,
          "elevator: standing at the other floor is not docked here");
    Check(StepLeg(lift, On(LIFT_TOP, 4000, 0, Sight(half, false, true))).act == Act::Wait, "elevator: on its way");
    Check(StepLeg(lift, On(LIFT_TOP, 5000, 0, Sight(up, false, true))).act == Act::Wait,
          "elevator: one scan at the floor is no stop (it may pass)");
    Check(StepLeg(lift, On(LIFT_TOP, 6000, 0, Sight(up, false, true))).act == Act::Board,
          "elevator: still at the floor for two scans: boards");
    Check(StepLeg(lift, Boarded(up, 7000, LIFT, Sight(up, false, true))).act == Act::Wait &&
              lift.boarding == Boarding::Ride,
          "elevator: aboard at the top: rides (the top is not the end)");
    StepLeg(lift, On(half, 9000, LIFT, Sight(half, false, true)));
    Check(StepLeg(lift, On(down, 11000, LIFT, Sight(down, false, true))).act == Act::Wait,
          "elevator: first scan at the bottom: not yet stopped");
    Decision const liftOff = StepLeg(lift, On(down, 12000, LIFT, Sight(down, false, true)));
    Check(liftOff.act == Act::Disembark && Near(liftOff.target, LIFT_BOTTOM) &&
              Noted(lift, 2, TransportEvent::Leave, LIFT, 5),
          "elevator: stands at the bottom: leave (5 s aboard), walks off");
    Check(
        StepLeg(lift, Exited(LIFT_BOTTOM, 13000)).act == Act::Done && Noted(lift, 3, TransportEvent::Arrived, LIFT, 6),
        "elevator: done at the bottom (6 s aboard)");

    // The graph's end node is the ship's own stop (its centre, over the water): a ship still under way there is no
    // dock; standing, the bot leaves, and off the hull on two scans the leg is done wherever beside the ship
    Leg stopLeg = MakeLeg(LegType::Transport, SHIP_AT_A, SHIP_AT_B, {1, 3}, 240.0f);
    stopLeg.object = SHIP;
    Trip stop = StartedTrip({stopLeg});
    stop.boarding = Boarding::Board;
    StepLeg(stop, Boarded(DECK, 2000, SHIP));
    StepLeg(stop, On(SEA_1, 3000, SHIP, Sight(SEA_1, true)));
    Check(StepLeg(stop, On({1, 1000.0f, 1035.0f, 6.0f}, 4000, SHIP, Sight(SHIP_AT_B, true))).act == Act::Wait &&
              stop.boarding == Boarding::Ride,
          "transport: at the end stop but moved since the scan before: rides");
    Decision const leave = StepLeg(stop, On({1, 1000.0f, 1035.0f, 6.0f}, 5000, SHIP, Sight(SHIP_AT_B, false)));
    Check(leave.act == Act::Disembark && stop.boarding == Boarding::Leave &&
              Noted(stop, 1, TransportEvent::Leave, SHIP, 3),
          "transport: standing on its end node: leave (3 s aboard)");
    Decision const landed = StepLeg(stop, Exited({1, 1000.0f, 1010.0f, 5.0f}, 7000));
    Check(
        landed.act == Act::Done && Noted(stop, 2, TransportEvent::Arrived, SHIP, 5) && stop.transportNotes.size() == 3,
        "transport: acknowledged exit 30 yd from the stop node: done, arrived (5 s aboard)");

    // Timeouts: off the transport WAIT_PERIODS periods (fallback: twice the leg's seconds, at least 30 s), aboard none
    // but RIDE_PERIODS from the boarding, ashore short of the end LEG_SLACK_MS
    Check(TravelLegs::LegTimeoutMs(ShipLeg()) == 2460000,
          "transport timeout: 60 s + 5 x 480 s before the period is read");
    Check(TravelLegs::LegTimeoutMs(LiftLeg()) == 210000, "elevator timeout: the fallback period is at least 30 s");
    Trip idle = SoughtTrip({ShipLeg()}, 0);
    Check(StepLeg(idle, On(PIER_A, 2460000, 0, Gone())).act == Act::Wait, "transport: waits 2460 s for its transport");
    Check(StepLeg(idle, On(PIER_A, 2460001, 0)).reason == FailReason::Timeout, "transport: Fail(timeout) after that");
    Trip period = SoughtTrip({ShipLeg()}, 0);
    StepLeg(period, On(PIER_A, 1000, 0, Sight(SEA_0, true, false, false, 300000)));
    Check(StepLeg(period, On(PIER_A, 1560000, 0, Gone())).act == Act::Wait &&
              StepLeg(period, On(PIER_A, 1560001, 0)).reason == FailReason::Timeout,
          "transport: a read period of 300 s: 60 s + 5 x 300 s");
    Trip ride = SoughtTrip({ShipLeg()}, 0);
    StepLeg(ride, On(PIER_A, 2400000, 0, Sight(SHIP_AT_A, false)));
    StepLeg(ride, Boarded(DECK, 2450000, SHIP, Sight(SHIP_AT_A, false)));
    Check(StepLeg(ride, On(SEA_0, 2500000, SHIP, Sight(SEA_0, true))).act == Act::Wait,
          "transport: aboard past the wait bound: rides");
    Check(StepLeg(ride, On(SEA_0, 2450000 + 1020000, SHIP)).act == Act::Wait &&
              StepLeg(ride, On(SEA_0, 2450000 + 1020001, SHIP)).reason == FailReason::Timeout,
          "transport: aboard: Fail(timeout) 60 s + 2 x 480 s after the boarding");
    Trip fell = StartedTrip({ShipLeg()});
    fell.boarding = Boarding::Board;
    StepLeg(fell, Boarded(DECK, 2000, SHIP));
    StepLeg(fell, On(SEA_1, 3000, SHIP, Sight(SEA_1, true)));
    Decision const fall = StepLeg(fell, On(SEA_1, 4000, 0, Gone()));
    Check(fall.reason == FailReason::TransportLost && Noted(fell, 1, TransportEvent::Lost, SHIP, 2) &&
              TravelLegs::AfterFailure(fell, fall.reason) == TravelLegs::Recovery::Replan,
          "transport: real fall records a loss and replans from the observed location");

    // At sea one scan may miss the passenger (a cross-map ship's far teleport): no walk off the deck; only two scans
    // in a row off count
    Trip blink = StartedTrip({ShipLeg()});
    blink.boarding = Boarding::Board;
    StepLeg(blink, Boarded(DECK, 2000, SHIP));
    StepLeg(blink, On(SEA_0, 3000, SHIP, Sight(SEA_0, true)));
    Observation transfer = On(SEA_1, 4000, 0, Sight(SEA_1, true));
    transfer.membershipAvailable = false;
    Check(StepLeg(blink, transfer).act == Act::Wait && StepLeg(blink, transfer).act == Act::Wait &&
              blink.boarding == Boarding::Ride && StepLeg(blink, On(SEA_1, 4800, SHIP)).act == Act::Wait,
          "transport: a map transfer with unavailable membership preserves the ride");
    Observation wrongCar = On(SEA_1, 6000, SHIP);
    ++wrongCar.aboardGuid;
    Check(StepLeg(blink, wrongCar).reason == FailReason::TransportLost,
          "transport: a different raw GUID with the same entry cannot continue the ride");

    // Undercity's lower lift doors (20651/20654/20657) are transports whose stops lie 8.1 yd apart in z: the leg ends
    // at once on the dock node, in either direction, without a wait
    Point const doorLow{0, 1595.21f, 178.693f, -40.5223f};
    Point const doorHigh{0, 1595.21f, 178.693f, -32.392f};
    Leg door = MakeLeg(LegType::Transport, doorLow, doorHigh, {}, 11.667f);
    door.object = 20654;
    Trip doorUp = StartedTrip({door});
    Check(StepLeg(doorUp, On({0, 1599.0f, 180.0f, -40.6f}, 2000, 0)).act != Act::Done,
          "transport: proximity from the lower floor cannot finish a door transport leg");
    door.from = doorHigh;
    door.to = doorLow;
    Trip doorDown = StartedTrip({door});
    Check(StepLeg(doorDown, On({0, 1599.0f, 180.0f, -40.6f}, 2000, 0)).act != Act::Done,
          "transport: a bot already below the door's upper stop has no ride acknowledgment");

    // A missed exit rides the round trip; wrong maps
    Trip stay = StartedTrip({ShipLeg()});
    stay.boarding = Boarding::Board;
    StepLeg(stay, Boarded(DECK, 2000, SHIP));
    StepLeg(stay, On(SHIP_AT_B, 3000, SHIP, Sight(SHIP_AT_B, false)));
    Check(stay.boarding == Boarding::Leave &&
              StepLeg(stay, On(SEA_1, 4000, SHIP, Sight(SEA_1, true))).act == Act::Wait &&
              stay.boarding == Boarding::Ride && Noted(stay, 2, TransportEvent::StuckAboard, SHIP, 1),
          "transport: still aboard when it leaves the end: stuck_aboard (1 s after the leave), rides on");
    Trip lost = StartedTrip({ShipLeg()});
    Check(StepLeg(lost, On(NODES[4], 2000, 0)).reason == FailReason::WrongMap,
          "transport: off the transport on another map than the start's: wrong map");
    Trip astray = StartedTrip({ShipLeg()});
    astray.boarding = Boarding::Board;
    StepLeg(astray, Boarded(DECK, 2000, SHIP));
    Check(StepLeg(astray, On({2, 0.0f, 0.0f, 0.0f}, 3000, SHIP)).reason == FailReason::WrongMap,
          "transport: aboard on a third map: wrong map");
    Trip other = SoughtTrip({ShipLeg()});
    Check(StepLeg(other, On(PIER_A, 2000, SHIP + 1)).act == Act::Wait && other.boarding == Boarding::Approach,
          "transport: on another transport is not aboard the leg's");

    // A timed-out transport leg replans without its link; a walk's timeout leaves no edge out
    Trip failedShip = StartedTrip({ShipLeg()});
    Check(TravelLegs::AfterFailure(failedShip, FailReason::Timeout) == TravelLegs::Recovery::Replan &&
              failedShip.avoidEdges.size() == 1 && failedShip.avoidEdges[0] == std::make_pair(1u, 3u),
          "transport: a timed-out leg leaves its link out of the next route");
    Trip failedWalk = StartedTrip({MakeLeg(LegType::Walk, NODES[0], NODES[1], {0, 1}, 30.0f)});
    Check(TravelLegs::AfterFailure(failedWalk, FailReason::Timeout) == TravelLegs::Recovery::Replan &&
              failedWalk.avoidEdges.empty(),
          "a walk's timeout leaves no edge out");

    // The Transport words
    Check(std::string(TravelLegs::TransportSource(TransportEvent::Attempt)) == "attempt" &&
              std::string(TravelLegs::TransportSource(TransportEvent::Boarded)) == "boarded" &&
              std::string(TravelLegs::TransportSource(TransportEvent::Arrived)) == "arrived" &&
              std::string(TravelLegs::TransportSource(TransportEvent::Missed)) == "missed" &&
              std::string(TravelLegs::TransportSource(TransportEvent::Hop)) == "hop" &&
              std::string(TravelLegs::TransportSource(TransportEvent::Leave)) == "leave" &&
              std::string(TravelLegs::TransportSource(TransportEvent::StuckAboard)) == "stuck_aboard" &&
              std::string(TravelLegs::TransportSource(TransportEvent::NoExit)) == "no_exit" &&
              std::string(TravelLegs::TransportSource(TransportEvent::Back)) == "back" &&
              std::string(TravelLegs::TRANSPORT_EVENT_TYPE) == "Transport" &&
              std::string(TravelLegs::HOP_HONESTY_KIND) == "Teleport" &&
              std::string(TravelLegs::HOP_HONESTY_SOURCE) == "transport_hop",
          "transport: event words");
}

bool Close(TravelLifecycle::Point const& a, TravelLifecycle::Point const& b)
{
    return TravelLifecycle::DistanceSquared(a, b) < 0.01f * 0.01f;
}

// WT43b: a rider without an exit at the end walks its boarding crossing back where it boarded, and the replan leaves
// the ride out
// WT54: the leg's vessel docks under the bot waiting beside its stop and the passenger rule attaches it: boarded
void CheckTakenAboard()
{
    using TravelLegs::Boarding;
    using TravelLegs::StepLeg;
    using TravelLegs::TransportEvent;

    Trip waiting = SoughtTrip({ShipLeg()});
    StepLeg(waiting, On(PIER_A, 3000, 0, Gone()));
    Check(waiting.boarding == Boarding::Wait, "taken aboard: the bot waits at the dock node");
    Check(TravelLegs::TakenAboard(waiting, DECK, SHIP), "taken aboard: the leg's vessel at its start");
    Check(!TravelLegs::TakenAboard(waiting, DECK, LIFT), "taken aboard: never by another entry");
    Check(!TravelLegs::TakenAboard(waiting, SEA_0, SHIP), "taken aboard: never far from the leg's start");
    Check(StepLeg(waiting, On(DECK, 9000, SHIP, Sight(SHIP_AT_A, true))).act == Act::Wait &&
              waiting.boarding == Boarding::Ride && waiting.passengerGuid == SHIP &&
              Noted(waiting, 1, TransportEvent::Boarded, SHIP, 6),
          "taken aboard while waiting: boarded (6 s waited), rides");
    Check(!TravelLegs::TakenAboard(waiting, DECK, SHIP), "taken aboard: once boarded, the ride owns it");

    Trip approach = SoughtTrip({ShipLeg()});
    Observation pending = On(DECK, 4000, SHIP, Sight(SHIP_AT_A, true));
    pending.boardMotion = true;
    StepLeg(approach, pending);
    Check(approach.boarding == Boarding::Approach && !approach.passengerGuid,
          "taken aboard: not while the bot's own boarding move is pending");
    StepLeg(approach, On(DECK, 5000, SHIP, Sight(SHIP_AT_A, true)));
    Check(approach.boarding == Boarding::Ride && Noted(approach, 0, TransportEvent::Boarded, SHIP, 4),
          "taken aboard before the wait began: boarded (4 s since the leg's start), rides");

    Trip astray = SoughtTrip({ShipLeg()});
    Check(StepLeg(astray, On(SEA_0, 4000, SHIP, Sight(SEA_0, true))).act == Act::Wait && !astray.passengerGuid,
          "taken aboard far from the start: not a boarding");

    // WT54-6: the vessel docked over the walk's last yards takes the bot aboard before the walk to the dock is done
    Leg const walk = MakeLeg(LegType::Walk, NODES[0], PIER_A, {0, 1}, 30.0f);
    Trip walking = StartedTrip({walk, ShipLeg()});
    Check(TravelLegs::WalkTakenAboard(walking, DECK, SHIP), "taken aboard on the walk: the next leg's vessel");
    Check(!TravelLegs::WalkTakenAboard(walking, DECK, LIFT) && !TravelLegs::WalkTakenAboard(walking, SEA_0, SHIP) &&
              !TravelLegs::WalkTakenAboard(walking, DECK, 0),
          "taken aboard on the walk: never by another entry, far from the dock or ashore");
    Trip lastWalk = StartedTrip({ShipLeg(), walk});
    TravelLegs::StartLeg(lastWalk, 1, 1000, NodeAt);
    Check(!TravelLegs::WalkTakenAboard(lastWalk, DECK, SHIP), "taken aboard on the walk: only before a transport leg");
    Decision const ahead = TravelLegs::Advance(walking, On(DECK, 4000, SHIP, Sight(SHIP_AT_A, false)), NodeAt);
    Check(ahead.act == Act::Wait && walking.legIdx == 1 && walking.boarding == Boarding::Ride &&
              walking.passengerGuid == SHIP && Noted(walking, 0, TransportEvent::Boarded, SHIP, 0),
          "taken aboard on the walk: the walk is done, the ship's leg boarded and rides");
}

void CheckBack()
{
    using TravelLegs::Boarding;
    using TravelLegs::StepLeg;
    using TravelLegs::TransportEvent;
    // The crossing in the transport's frame: placed back by the same pose it is the same points; a stop repeats its
    // pose within a few tenths; at the far stop, or aboard another transport, there is none
    TravelLifecycle::DockPose const stopA{{1318.0f, -4660.0f, 55.0f}, 1.2f};
    TravelLifecycle::Point const deck{1318.1f, -4658.0f, 55.3f};
    TravelLifecycle::Point const shore{1318.67f, -4651.05f, 53.91f};
    TravelLegs::BoardCrossing const crossing = TravelLegs::StoreCrossing(7, stopA, deck, shore);
    auto const same = TravelLegs::CrossingBack(crossing, 7, stopA);
    Check(same && Close(same->deck, deck) && Close(same->shore, shore),
          "back: the crossing in the transport's frame comes back at the same pose");
    TravelLifecycle::DockPose const again{{1318.2f, -4659.9f, 55.1f}, 1.21f};
    auto const near = TravelLegs::CrossingBack(crossing, 7, again);
    Check(near && TravelLifecycle::DistanceSquared(near->shore, shore) < 0.5f * 0.5f,
          "back: the stop repeats its pose within a few tenths: the crossing holds");
    TravelLifecycle::DockPose const turned{{1318.0f, -4660.0f, 55.0f}, 2.2f};
    Check(!TravelLegs::CrossingBack(crossing, 7, {{2066.0f, 288.0f, 97.0f}, 4.0f}) &&
              !TravelLegs::CrossingBack(crossing, 7, turned) && !TravelLegs::CrossingBack(crossing, 8, stopA) &&
              !TravelLegs::CrossingBack(TravelLegs::BoardCrossing{}, 0, stopA),
          "back: none at the far stop, turned, aboard another transport or without a boarding walk");
    TravelLifecycle::Point const local =
        TravelLegs::LocalOffset({{10.0f, 20.0f, 5.0f}, 1.5707964f}, {10.0f, 30.0f, 6.0f});
    Check(std::fabs(local.X - 10.0f) < 0.001f && std::fabs(local.Y) < 0.001f && std::fabs(local.Z - 1.0f) < 0.001f,
          "back: the local frame turns by -o (10 yd north of a ship facing north lies 10 yd ahead)");

    // Without a no_exit the bot rides on past its boarding stop
    Trip ride = StartedTrip({ShipLeg()});
    ride.boarding = Boarding::Board;
    StepLeg(ride, Boarded(DECK, 2000, SHIP, Sight(SHIP_AT_A, false)));
    StepLeg(ride, On(SHIP_AT_B, 3000, SHIP, Sight(SHIP_AT_B, false)));
    StepLeg(ride, On(SEA_1, 4000, SHIP, Sight(SEA_1, true)));
    Check(StepLeg(ride, On(DECK, 5000, SHIP, Sight(SHIP_AT_A, false))).act == Act::Wait &&
              ride.boarding == Boarding::Ride,
          "back: without a no_exit the boarding stop is no exit");

    // After a no_exit at the end: at the boarding stop it walks off, a `back` row, the leg fails as no_exit and the
    // replan leaves the ride out
    Trip back = StartedTrip({ShipLeg()});
    back.boarding = Boarding::Board;
    StepLeg(back, Boarded(DECK, 2000, SHIP, Sight(SHIP_AT_A, false)));
    StepLeg(back, On(SHIP_AT_B, 3000, SHIP, Sight(SHIP_AT_B, false)));
    back.exitSought = true;
    back.exitless = true;  // the follower's WalkOff found no exit at B
    StepLeg(back, On(SEA_1, 4000, SHIP, Sight(SEA_1, true)));
    Decision const off = StepLeg(back, On(DECK, 5000, SHIP, Sight(SHIP_AT_A, false)));
    Check(off.act == Act::Disembark && Near(off.target, PIER_A) && back.boarding == Boarding::Back && !back.exitSought,
          "back: after no_exit, docked where it boarded: walks off towards the boarding side");
    Decision const ashore = StepLeg(back, Exited(PIER_A, 6000));
    Check(ashore.act == Act::Fail && ashore.reason == FailReason::NoExit &&
              Noted(back, back.transportNotes.size() - 1, TransportEvent::Back, SHIP, 4),
          "back: off where it boarded: `back` (4 s aboard), the leg fails as no_exit");
    Check(TravelLegs::AfterFailure(back, ashore.reason) == TravelLegs::Recovery::Replan &&
              back.avoidEdges.size() == 1 && back.avoidEdges[0] == std::make_pair(1u, 3u),
          "back: the replan leaves the ride's link out");

    // The back exit missed: the transport leaves again, stuck_aboard, and the next stop tries again
    Trip again2 = StartedTrip({ShipLeg()});
    again2.boarding = Boarding::Board;
    StepLeg(again2, Boarded(DECK, 2000, SHIP, Sight(SHIP_AT_A, false)));
    again2.exitless = true;
    StepLeg(again2, On(SEA_0, 3000, SHIP, Sight(SEA_0, true)));
    StepLeg(again2, On(DECK, 4000, SHIP, Sight(SHIP_AT_A, false)));
    std::size_t const before = again2.transportNotes.size();
    Check(StepLeg(again2, On(SEA_0, 5000, SHIP, Sight(SEA_0, true))).act == Act::Wait &&
              again2.boarding == Boarding::Ride && Noted(again2, before, TransportEvent::StuckAboard, SHIP, 1),
          "back: still aboard as it leaves the boarding stop: stuck_aboard, rides on");
    Check(StepLeg(again2, On(SHIP_AT_B, 6000, SHIP, Sight(SHIP_AT_B, false))).act == Act::Disembark &&
              again2.boarding == Boarding::Leave,
          "back: the end stop is still tried first");

    // A new leg starts without the no_exit and the stored crossing
    TravelLegs::StartLeg(again2, 0, 7000, NodeAt);
    Check(!again2.exitless && !again2.boardCrossing.guid, "back: a new leg forgets the no_exit and the crossing");
}

// WT19-d: a trip end, a route request or a walk while the bot rides a ship gets the bot off first, within one period
void CheckOffFirst()
{
    using TravelLegs::BeginOffFirst;
    using TravelLegs::OffFirstEnd;
    using TravelLegs::OffStep;
    using TravelLegs::StepOffFirst;
    using TravelLegs::TransportEvent;

    Leg const goalWalk = MakeLeg(LegType::Walk, PIER_B, NODES[4], {3, 4}, 15.0f);
    Trip ashore = StartedTrip({goalWalk});
    Check(!BeginOffFirst(ashore, 0, 0, 300000, 2000, OffFirstEnd::Arrive) && !ashore.offFirst.sinceMs &&
              ashore.transportNotes.empty(),
          "off first: not aboard a ship: nothing waits");

    // The goal walk ended on the deck: the arrival waits (off_first, the period's 300 s as its bound)
    Trip trip = StartedTrip({goalWalk});
    Check(BeginOffFirst(trip, SHIP, SHIP, 300000, 2000, OffFirstEnd::Arrive) &&
              trip.offFirst.end == OffFirstEnd::Arrive && Noted(trip, 0, TransportEvent::OffFirst, SHIP, 300),
          "off first: an arrival aboard waits (off_first, 300 s)");
    Check(
        !BeginOffFirst(trip, SHIP, SHIP, 300000, 2500, OffFirstEnd::Abort) && trip.offFirst.end == OffFirstEnd::Arrive,
        "off first: one at a time");
    Check(StepOffFirst(trip, On(SEA_1, 3000, SHIP, Sight(SEA_1, true))) == OffStep::Wait,
          "off first: the ship under way: waits aboard");
    Check(StepOffFirst(trip, On(SHIP_AT_B, 5000, SHIP, Sight(SHIP_AT_B, false))) == OffStep::Disembark &&
              StepOffFirst(trip, On(SHIP_AT_B, 5500, SHIP)) == OffStep::Disembark,
          "off first: the ship stands: walks off (between scans too)");
    Trip loss = trip;
    Check(StepOffFirst(loss, On(PIER_B, 6000, 0, Gone())) == OffStep::Lost,
          "off first: loss at a dock cannot authorize arrival");
    Check(StepOffFirst(trip, Exited(PIER_B, 7000)) == OffStep::Off && !trip.offFirst.sinceMs &&
              trip.offFirst.end == OffFirstEnd::Arrive && trip.transportNotes.size() == 1,
          "off first: acknowledged exit releases the arrival");
    Check(BeginOffFirst(trip, LIFT, LIFT, 0, 8000) && trip.offFirst.end == OffFirstEnd::None &&
              trip.offFirst.boundMs == TravelLegs::LEG_SLACK_MS,
          "off first: after one that got off another may start; an unknown period bounds it by 60 s");

    // Still aboard a period after the start: stuck_aboard, the abort goes on, no second wait on the trip
    Trip stuck = StartedTrip({goalWalk});
    BeginOffFirst(stuck, SHIP, SHIP, 300000, 2000, OffFirstEnd::Arrive);
    StepOffFirst(stuck, On(SHIP_AT_B, 3000, SHIP, Sight(SHIP_AT_B, true)));
    Check(StepOffFirst(stuck, On(SHIP_AT_B, 302000, SHIP, Sight(SHIP_AT_B, false))) == OffStep::Disembark,
          "off first: aboard at the bound: still walks off");
    Check(StepOffFirst(stuck, On(SHIP_AT_B, 302001, SHIP)) == OffStep::GiveUp &&
              Noted(stuck, 1, TransportEvent::StuckAboard, SHIP, 300) && stuck.offFirst.gaveUp &&
              !stuck.offFirst.sinceMs && stuck.offFirst.end == OffFirstEnd::Arrive,
          "off first: expiry reports stuck_aboard and gives the follower a failure instead of Off");
    Check(!BeginOffFirst(stuck, SHIP, SHIP, 300000, 400000, OffFirstEnd::Abort, "timeout", "abort:timeout"),
          "off first: no second wait on a trip after a period aboard");
    Check(std::string(TravelLegs::TransportSource(TransportEvent::OffFirst)) == "off_first", "off first: its word");
}

// WT19-e: a ship's dock node is its own stop (over the water), so the bot walks to the quay beside it, attempts and
// waits there and boards from there; a miss sends it back
// to wait and the next docking walks anew. Elevators keep their stop node
void CheckBoarding()
{
    using TravelLegs::Boarding;
    using TravelLegs::StepLeg;
    using TravelLegs::TransportEvent;
    Point const quay{0, 118.0f, 12.0f, 5.0f};  // 17.7 yd from the ship's stop SHIP_AT_A, 5 yd above the water
    Leg stopLeg = MakeLeg(LegType::Transport, SHIP_AT_A, SHIP_AT_B, {1, 3}, 240.0f);
    stopLeg.object = SHIP;

    Trip bare = StartedTrip({stopLeg});
    Decision const toStop = StepLeg(bare, On(quay, 2000, 0));
    Check(toStop.act == Act::Walk && Near(toStop.target, SHIP_AT_A) && bare.boarding == Boarding::Approach,
          "boarding: without a quay the bot walks to the stop node");

    Trip trip = StartedTrip({stopLeg});
    TravelLegs::SetQuay(trip, &quay);
    Decision const approach = StepLeg(trip, On({0, 70.0f, 0.0f, 0.0f}, 2000, 0));
    Check(approach.act == Act::Walk && Near(approach.target, quay) && trip.quaySought,
          "boarding: walks to the quay, not to the ship's stop over the water");
    Check(StepLeg(trip, On(quay, 3000, 0, Gone())).act == Act::Wait && trip.boarding == Boarding::Wait &&
              Noted(trip, 0, TransportEvent::Attempt, SHIP, 2),
          "boarding: at the quay: attempt (2 s walk), waits");
    Decision const back = StepLeg(trip, On({0, 70.0f, 0.0f, 0.0f}, 3500, 0, Gone()));
    Check(back.act == Act::Walk && Near(back.target, quay), "boarding: drawn off while waiting: back to the quay");
    Decision const board = StepLeg(trip, On(quay, 5000, 0, Sight(SHIP_AT_A, false)));
    Check(board.act == Act::Board && Near(board.target, SHIP_AT_A) && trip.boarding == Boarding::Board,
          "boarding: docked at its stop: boards from the quay");
    Point const walking{0, 122.0f, 6.0f, 4.0f};
    Check(StepLeg(trip, On(walking, 6000, 0, Sight({0, 131.0f, 0.0f, 0.0f}, true))).act == Act::Wait && !trip.misses,
          "boarding: the ship leaves while the bot walks in: no miss within the grace");
    Check(StepLeg(trip, On(walking, 8400, 0)).act == Act::Wait && !trip.misses, "boarding: 2.4 s later: still no miss");
    Decision const missed = StepLeg(trip, On(walking, 8600, 0, Sight({0, 160.0f, 0.0f, 0.0f}, true)));
    Check(missed.act == Act::Wait && trip.misses == 1 && trip.boarding == Boarding::Wait &&
              Noted(trip, 1, TransportEvent::Missed, SHIP, 1),
          "boarding: not aboard after the grace: a miss, waits beside the quay");
    StepLeg(trip, On(quay, 100000, 0, Sight(SHIP_AT_A, false)));
    Check(trip.boarding == Boarding::Board, "boarding: the next docking permits another attempt");
    Check(StepLeg(trip, Boarded(walking, 102000, SHIP, Sight(SHIP_AT_A, false))).act == Act::Wait &&
              trip.boarding == Boarding::Ride,
          "boarding: aboard from the walk: rides");

    // WT21's case: the walk leg before ends at the ship's stop and the ship leg begins in the same tick, before the
    // follower looked for the quay: no attempt and no wait there, then the walk to the quay
    Point const atStop{0, 129.0f, 0.0f, 0.0f};  // 1 yd from SHIP_AT_A, in the water
    Trip chained = StartedTrip({MakeLeg(LegType::Walk, PIER_A, SHIP_AT_A, {}, 10.0f), stopLeg});
    Decision const handover = TravelLegs::Advance(chained, On(atStop, 2000, 0), NodeAt);
    Check(handover.act == Act::Wait && chained.legIdx == 1 && chained.boarding == Boarding::Approach &&
              !chained.quaySought && chained.transportNotes.empty(),
          "boarding: a walk leg ends at the ship's stop: no attempt and no wait there before the quay is looked for");
    TravelLegs::SetQuay(chained, &quay);
    Decision const toQuay = StepLeg(chained, On(atStop, 3000, 0));
    Check(toQuay.act == Act::Walk && Near(toQuay.target, quay) && chained.boarding == Boarding::Approach,
          "boarding: the quay found a tick later: walks from the stop to the quay");
    Check(StepLeg(chained, On(quay, 5000, 0, Gone())).act == Act::Wait && chained.boarding == Boarding::Wait &&
              Noted(chained, 0, TransportEvent::Attempt, SHIP, 3),
          "boarding: at the quay: attempt (3 s since the leg began), waits");

    Trip retargeted = StartedTrip({MakeLeg(LegType::Walk, PIER_A, SHIP_AT_A, {}, 10.0f), stopLeg});
    TravelLegs::SetQuay(retargeted, &quay);
    retargeted.waypoints.back().at = quay;
    Check(TravelLegs::Advance(retargeted, On(quay, 2000, 0), NodeAt).act == Act::Wait && retargeted.legIdx == 1 &&
              retargeted.boarding == Boarding::Wait && Near(TravelLegs::WaitPoint(retargeted, stopLeg), quay),
          "boarding: the next leg retains the ground waypoint without returning to the synthetic stop");

    Trip lift = StartedTrip({LiftLeg()});
    TravelLegs::SetQuay(lift, nullptr);
    Check(lift.quaySought && !lift.hasQuay && Near(TravelLegs::WaitPoint(lift, lift.legs[0]), LIFT_TOP),
          "boarding: an elevator waits at its stop node");

    Trip pending = StartedTrip({MakeLeg(LegType::Walk, PIER_A, SHIP_AT_A, {}, 10.0f), stopLeg});
    Observation waiting = On(PIER_A, 2000, 0);
    waiting.quayLeg = 1;
    Decision const waitingDecision = TravelLegs::Advance(pending, waiting, NodeAt);
    Check(waitingDecision.act == Act::Walk && Near(waitingDecision.target, SHIP_AT_A) && pending.legIdx == 0,
          "unavailable quay does not erase the last walking edge");
    waiting.bot = SHIP_AT_A;
    Check(TravelLegs::Advance(pending, waiting, NodeAt).act == Act::Wait && pending.legIdx == 1 &&
              pending.boarding == Boarding::Approach,
          "the missing-quay wait begins only after the actual walking endpoint is reached");
    waiting.nowMs += TravelLegs::TransportWaitMs(pending, stopLeg) + 1;
    Check(StepLeg(pending, waiting).reason == FailReason::Timeout, "a missing physical quay uses the dock wait bound");

    Point const brill{0, 2234.76f, 252.124f, 37.3704f};
    Point const stop1964{0, 2062.599f, 235.264f, 117.734f};
    Point const interior{0, 2061.870f, 247.465f, 100.636f};
    Point const lowerQuay{0, 2056.750f, 236.750f, 99.767f};
    Leg departure = stopLeg;
    departure.from = stop1964;
    Trip groundApproach;
    groundApproach.legs = {MakeLeg(LegType::Walk, brill, stop1964, {143, 1964}, 90.0f), departure};
    auto const departureNode = [&](std::uint32_t node) { return node == 143 ? brill : stop1964; };
    TravelLegs::StartLeg(groundApproach, 0, 1000, departureNode);
    Observation ground = On(brill, 2000, 0);
    ground.quayLeg = 1;
    Decision step = TravelLegs::Advance(groundApproach, ground, departureNode);
    Check(groundApproach.waypoints.size() == 2 && groundApproach.legIdx == 0 && step.act == Act::Walk &&
              Near(step.target, stop1964),
          "143 to 1964: graph endpoints do not credit the walk at the distant lower Brill vertex");
    TravelLegs::SetQuay(groundApproach, nullptr);
    Check(!groundApproach.quaySought,
          "a failed quay search during walking permits another sample as the bot approaches");
    ground.bot = interior;
    step = TravelLegs::Advance(groundApproach, ground, departureNode);
    Check(step.act == Act::Walk && groundApproach.legIdx == 0 && Near(step.target, stop1964),
          "an interior ground sample is observed movement, not an extra graph waypoint or completed edge");
    TravelLegs::SetQuay(groundApproach, &lowerQuay);
    groundApproach.waypoints.back().at = lowerQuay;
    step = TravelLegs::Advance(groundApproach, ground, departureNode);
    Check(step.act == Act::Walk && Near(step.target, lowerQuay) && groundApproach.legIdx == 0,
          "a validated replacement physical approach must itself be reached");
    ground.bot = lowerQuay;
    step = TravelLegs::Advance(groundApproach, ground, departureNode);
    Check(step.act == Act::Wait && groundApproach.legIdx == 1 && groundApproach.hasQuay,
          "only arrival on the selected physical quay hands off to the transport leg");
}
}  // namespace

// A ship whose stops have wait points (WT55b): the bot waits at the near wait point and boards from there, the dock
// test measures the stops, and at the far stop it steps off towards the far wait point
Leg WaitShipLeg()
{
    Leg leg = MakeLeg(LegType::Transport, NODES[7], NODES[8], {7, 1, 3, 8}, 240.0f);
    leg.object = SHIP;
    leg.rideAt = 1;
    leg.stopFrom = PIER_A;
    leg.stopTo = PIER_B;
    leg.waitFrom = true;
    leg.waitTo = true;
    return leg;
}

void CheckWaitPoints()
{
    using TravelLegs::Boarding;
    using TravelLegs::StepLeg;
    using TravelLegs::TransportEvent;
    Point const waitA = NODES[7];
    Point const waitB = NODES[8];
    Trip trip = StartedTrip({WaitShipLeg(), MakeLeg(LegType::Walk, waitB, NODES[4], {8, 4}, 15.0f)});
    Decision const approach = StepLeg(trip, On({0, 10.0f, 0.0f, 0.0f}, 2000, 0));
    Check(approach.act == Act::Walk && Near(approach.target, waitA) && trip.hasQuay && trip.quaySought &&
              Near(trip.quay, waitA),
          "wait point: the approach ends at the wait point, no quay is looked for");
    Check(StepLeg(trip, On(waitA, 3000, 0, Gone())).act == Act::Wait && trip.boarding == Boarding::Wait &&
              Noted(trip, 0, TransportEvent::Attempt, SHIP, 2),
          "wait point: waits there");
    Check(StepLeg(trip, On(waitA, 4000, 0, Sight({0, 40.0f, -60.0f, 0.0f}, false))).act == Act::Wait,
          "wait point: a ship stopped beside the wait point, out of the stop's reach, is not docked");
    Decision const board = StepLeg(trip, On(waitA, 5000, 0, Sight(SHIP_AT_A, false)));
    Check(board.act == Act::Board && Near(board.target, SHIP_AT_A) && trip.boarding == Boarding::Board,
          "wait point: boards the ship docked at the stop, from the wait point");
    Check(StepLeg(trip, Boarded(DECK, 7000, SHIP, Sight(SHIP_AT_A, false))).act == Act::Wait &&
              trip.boarding == Boarding::Ride,
          "wait point: aboard: rides");
    Decision const off = StepLeg(trip, On({1, 1000.0f, 1030.0f, 3.0f}, 700000, SHIP, Sight(SHIP_AT_B, false)));
    Check(off.act == Act::Disembark && Near(off.target, waitB) && trip.boarding == Boarding::Leave,
          "wait point: docked at the far stop: steps off towards the far wait point");
    Check(StepLeg(trip, Exited(waitB, 701000)).act == Act::Done, "wait point: off the ship the leg is done");

    // WT60: a far wait point the deck cannot reach (the follower's WalkOff names the gate and sets exitless): the ship
    // takes the bot back, and at the near stop it steps off towards the wait point it boarded from (WT43b's ride back)
    Trip noExit = StartedTrip({WaitShipLeg(), MakeLeg(LegType::Walk, waitB, NODES[4], {8, 4}, 15.0f)});
    noExit.boarding = Boarding::Board;
    noExit.hasQuay = true;
    noExit.quay = waitA;
    StepLeg(noExit, Boarded(DECK, 2000, SHIP, Sight(SHIP_AT_A, false)));
    Check(StepLeg(noExit, On(SHIP_AT_B, 3000, SHIP, Sight(SHIP_AT_B, false))).act == Act::Disembark &&
              noExit.boarding == Boarding::Leave,
          "wait point: no exit: docked at the far stop it tries the far wait point first");
    noExit.exitSought = true;
    noExit.exitless = true;
    std::size_t const notesBefore = noExit.transportNotes.size();
    StepLeg(noExit, On(SEA_1, 4000, SHIP, Sight(SEA_1, true)));
    Check(Noted(noExit, notesBefore, TransportEvent::StuckAboard, SHIP, 1),
          "wait point: no exit: still aboard as the ship leaves the far stop: stuck_aboard, rides back");
    Decision const backOff = StepLeg(noExit, On(DECK, 5000, SHIP, Sight(SHIP_AT_A, false)));
    Check(backOff.act == Act::Disembark && Near(backOff.target, waitA) && noExit.boarding == Boarding::Back &&
              !noExit.exitSought,
          "wait point: no exit: docked where it boarded it steps off towards the near wait point");
    Decision const backAshore = StepLeg(noExit, Exited(waitA, 6000));
    Check(backAshore.act == Act::Fail && backAshore.reason == FailReason::NoExit,
          "wait point: no exit: off at the near wait point the leg fails as no_exit (the replan leaves the ride out)");
    Trip timedOut = StartedTrip({WaitShipLeg()});
    TravelLegs::NoteTransportTimeout(timedOut);
    Check(timedOut.avoidEdges.size() == 1 &&
              timedOut.avoidEdges.front() == std::pair<std::uint32_t, std::uint32_t>{1, 3},
          "wait point: a timeout leaves the ride out, stop to stop");
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
    CheckLanding();
    CheckPlaces();
    CheckInTheAir();
    CheckCombatLanding();
    CheckBodyZ();
    CheckDescent();
    CheckTransport();
    CheckTakenAboard();
    CheckBack();
    CheckOffFirst();
    CheckBoarding();
    CheckWaitPoints();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
