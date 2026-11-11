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

char const* ActWord(Act act)
{
    switch (act)
    {
        case Act::Walk:
            return "walk";
        case Act::UseTrigger:
            return "trigger";
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

Observation At(Point bot, std::uint32_t nowMs, std::uint32_t money = 10000, bool inFlight = false, bool stuck = false)
{
    Observation seen;
    seen.bot = bot;
    seen.nowMs = nowMs;
    seen.money = money;
    seen.inFlight = inFlight;
    seen.stuck = stuck;
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
    Decision const stuck = TravelLegs::Advance(trip, At(NODES[1], 3000, 10000, false, true), NodeAt);
    Check(stuck.act == Act::Fail && stuck.reason == FailReason::Stuck, "stuck: Fail(stuck) mid-walk");

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
    Trip ship = StartedTrip({MakeLeg(LegType::Transport, NODES[0], NODES[1], {0, 1}, 120.0f)});
    Check(TravelLegs::StepLeg(ship, At(NODES[0], 2000)).reason == FailReason::Unsupported, "transport: unsupported");

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
    float nearestMoveFarDis{1.0e9f};
};

void CheckResetAndDeadline()
{
    FakeInfo travelling;
    travelling.data = FakeInfo::Travel{StartedTrip(FullRoute())};
    std::get<FakeInfo::Travel>(travelling.data).trip.legIdx = 2;  // on the portal leg
    travelling.startT = 1234;
    travelling.routeRequests = 17;
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
    Check(!TravelLegs::PastDeadline(bound, 1000.0f, false), "deadline: not past at the bound");
    Check(TravelLegs::PastDeadline(bound + 1, 1000.0f, false), "deadline: past one ms after the bound");
    Check(!TravelLegs::PastDeadline(bound + 1, 1000.0f, true), "deadline: waits while the bot is in flight");
    Check(!TravelLegs::PastDeadline(bound + 1, 2000.0f, false), "deadline: a replan's longer estimate moves the bound");
    Check(TravelLegs::PastDeadline(TravelLegs::TRIP_SLACK_MS + 1, 0.0f, false),
          "deadline: before the first answer the slack alone");
}

void CheckEventSources()
{
    bool legs = true;
    for (LegType const type :
         {LegType::Walk, LegType::Portal, LegType::Flight, LegType::Transport, LegType::Hearth, LegType::Fly})
        legs = legs && std::string(TravelLegs::LegDoneSource(type)) ==
                           std::string("leg_done:") + TravelRoute::LegTypeWord(type);
    Check(legs, "events: leg_done:<leg type> for every leg type");
    bool reasons = true;
    for (int r = 0; r <= static_cast<int>(FailReason::Takeoff); ++r)
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
}  // namespace

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
    CheckDeathsAndRecord();
    CheckFlyLeg();
    CheckPlaces();
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
