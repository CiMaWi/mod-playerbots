/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the route search (src/Ai/World/Rpg/RouteRules.h) on the shipped travel graph and the 3.3.5
// TaxiPath/TaxiNodes DBC files: the fixed snapshots a) - f) and the fare budget h) of TravelRoute::Selftest and the
// worker test g) of TravelRoute::ServiceSelftest (RouteService.h) and the named places p) of TravelPlaces::Selftest
// (TravelPlaces.h), the checks `playerbots route selftest` also runs on the loaded graph; i) a trip's replans after
// stuck walks (TravelLegRules.h) on that graph; n) replans after walks off the graph the navmesh refused (WT56) at
// Thunder Bluff's foot and near a road node, and a source check that the planner holds no slope or climb rule; and
// w) a crossing through wait points on a hand-made graph.
// Usage: route-rules-selftest [module root] [dbc dir] [graph dir] (defaults: the current directory,
// C:/Games/AzerothCore/Server/data/dbc, the shipped graph)
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "RouteRules.h"
#include "RouteSelftest.h"
#include "RouteService.h"
#include "TravelGraphSql.h"
#include "TravelLegRules.h"
#include "TravelPlaces.h"

namespace
{
// i) Replans after stuck walks (TravelLegs::NoteStuck -> Request::avoidNodes, noDirect) near a road node: a goal 60 yd
//    off is walked straight; stuck there, the new route runs through nodes; stuck on its first hop, the next starts at
//    another node; stuck on that one's goal hop, the next ends at another node (or there is none)
std::uint32_t StuckReplanSelftest(TravelGraph const& graph, std::vector<std::string>& failures,
                                  std::vector<std::string>& notes)
{
    using TravelLegs::FailReason;
    using TravelLegs::Recovery;
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back(what);
        return ok;
    };
    auto const nodeAt = [&graph](std::uint32_t index) -> TravelRoute::Point
    {
        TravelGraphNode const& node = graph.GetNode(index);
        return {node.mapId, node.x, node.y, node.z};
    };
    std::uint32_t road = TravelGraph::NO_NODE;
    for (std::uint32_t index = 0; index < graph.NodeCount() && road == TravelGraph::NO_NODE; ++index)
    {
        std::size_t walks = 0;
        for (TravelGraphLink const& link : graph.LinksOf(index))
            walks += link.type == TravelLinkType::Walk ? 1 : 0;
        if (walks >= 2)
            road = index;
    }
    if (!check(road != TravelGraph::NO_NODE, "i) no node with two walk links"))
        return checks;

    TravelRoute::Point const at = nodeAt(road);
    TravelRoute::Request request;
    request.snapshot.position = {at.mapId, at.x + 15.0f, at.y, at.z};
    request.goal = {at.mapId, at.x + 75.0f, at.y, at.z};
    TravelLegs::Trip trip;
    auto const replan = [&]() -> TravelRoute::Result
    {
        request.avoidNodes = trip.avoid;
        request.noDirect = trip.noDirect;
        TravelRoute::Result route =
            TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, TravelRoute::Params{});
        trip.legs = route.legs;
        TravelLegs::StartLeg(trip, 0, 0, nodeAt);
        return route;
    };
    auto const firstNode = [](TravelRoute::Result const& route)
    {
        return route.legs.empty() || route.legs.front().nodes.empty() ? TravelGraph::NO_NODE
                                                                      : route.legs.front().nodes.front();
    };
    auto const lastNode = [](TravelRoute::Result const& route)
    {
        return route.legs.empty() || route.legs.back().nodes.empty() ? TravelGraph::NO_NODE
                                                                     : route.legs.back().nodes.back();
    };

    TravelRoute::Result const straight = replan();
    check(straight.ok && straight.legs.size() == 1 && straight.legs.front().nodes.empty(),
          "i) a goal 60 yd off is walked straight");
    check(TravelLegs::AfterFailure(trip, FailReason::Stuck) == Recovery::Replan && trip.noDirect,
          "i) stuck on the straight walk: a replan without it");
    TravelRoute::Result const viaNodes = replan();
    std::uint32_t const first = firstNode(viaNodes);
    if (!check(viaNodes.ok && first != TravelGraph::NO_NODE && trip.waypoints.size() >= 3 && trip.next == 1 &&
                   trip.waypoints[1].node == first,
               "i) the route without the straight walk runs through nodes"))
        return checks;

    check(TravelLegs::AfterFailure(trip, FailReason::Stuck) == Recovery::Replan && trip.avoid.size() == 1 &&
              trip.avoid[0] == first,
          "i) stuck on the first hop: a replan without its node");
    TravelRoute::Result const around = replan();
    check(!around.ok || (firstNode(around) != TravelGraph::NO_NODE && firstNode(around) != first),
          "i) the route after a stuck first hop starts at another node");
    std::string summary = "i) node " + std::to_string(graph.GetNode(road).id) + ": straight, then via " +
                          std::to_string(viaNodes.legs.front().nodes.size()) + " nodes, then " +
                          (around.ok ? "around via " + std::to_string(around.legs.front().nodes.size()) + " nodes"
                                     : std::string(TravelRoute::FailureWord(around.failure)));
    if (around.ok && trip.waypoints.size() >= 3)
    {
        trip.next = static_cast<std::uint32_t>(trip.waypoints.size() - 1);  // heading for the goal from the last node
        std::uint32_t const before = trip.waypoints[trip.next - 1].node;
        check(TravelLegs::AfterFailure(trip, FailReason::Stuck) == Recovery::Replan && before != TravelGraph::NO_NODE,
              "i) stuck on the goal hop: a replan");
        TravelRoute::Result const goalSide = replan();
        check(!goalSide.ok || (lastNode(goalSide) != before && lastNode(goalSide) != TravelGraph::NO_NODE),
              "i) the route after a stuck goal hop ends at another node");
        summary += goalSide.ok ? ", then to the goal from another node"
                               : std::string(", then ") + TravelRoute::FailureWord(goalSide.failure);
    }
    notes.push_back(summary);
    return checks;
}

// n) Walks off the graph (WT56): the planner's source holds no slope or climb rule (it proposes such walks by distance
//    alone; the bot's map thread probes them on the navmesh). With a model of the navmesh, the follower's refusals
//    (AfterFailure with NoWalk, from where the refused walk starts) never plan the same walk again, and the trip ends
//    in a route without a refused walk, in no route, or in an abort within MAX_REPLANS. Thunder Bluff's foot, whose
//    model refuses every walk off the graph between the mesa's top and the ground below; a road node whose goal 60 yd
//    off takes no walk from the start (the bot's start walks only along the graph): the first node's walk is refused
//    and the next route starts at another node; and one whose goal takes no walk to it (a goal in a closed building)
std::uint32_t NoWalkReplanSelftest(TravelGraph const& graph, std::filesystem::path const& root,
                                   std::vector<std::string>& failures, std::vector<std::string>& notes)
{
    using TravelRoute::Point;
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back("n) " + what);
        return ok;
    };
    std::ifstream in(root / "src/Ai/World/Rpg/RouteRules.h", std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::string const source = text.str();
    check(source.find("GraphSearch") != std::string::npos, "RouteRules.h not read");
    for (char const* token : {"StraightWalk", "CLIMB_", "fabs(a.z - b.z)"})
        check(source.find(token) == std::string::npos, std::string("the planner's source holds ") + token);

    auto const nodeAt = [&graph](std::uint32_t index) -> Point
    {
        TravelGraphNode const& node = graph.GetNode(index);
        return {node.mapId, node.x, node.y, node.z};
    };
    auto const samePiece = [](std::pair<Point, Point> const& a, std::pair<Point, Point> const& b)
    {
        return TravelRoute::Distance(a.first, b.first) <= TravelLegs::WAYPOINT_YARDS &&
               TravelRoute::Distance(a.second, b.second) <= TravelRoute::SAME_WALK_YARDS;
    };
    struct Loop
    {
        bool proposedDirect{false};
        bool repeated{false};
        std::vector<std::pair<Point, Point>> refused;
        std::vector<TravelRoute::Result> routes;
        std::string outcome;
        std::string story;
    };
    // Plans, refuses the first walk off the graph the model refuses, replans from that walk's start, until a route
    // takes no refused walk, there is no route or the trip aborts
    auto const loop = [&](TravelRoute::Request request, auto const& refuses)
    {
        Loop run;
        TravelLegs::Trip trip;
        for (std::uint32_t round = 0; round <= TravelLegs::MAX_REPLANS + 1u && run.outcome.empty(); ++round)
        {
            request.avoidNodes = trip.avoid;
            request.noDirect = trip.noDirect;
            request.avoidWalks = trip.avoidWalks;
            TravelRoute::Result const route =
                TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, TravelRoute::Params{});
            run.routes.push_back(route);
            if (!route.ok)
            {
                run.outcome = TravelRoute::FailureWord(route.failure);
                break;
            }
            if (round == 0)
                run.proposedDirect = route.legs.size() == 1 && route.legs.front().type == TravelRoute::LegType::Walk &&
                                     route.legs.front().nodes.empty();
            trip.legs = route.legs;
            bool found = false;
            for (std::uint32_t i = 0; i < trip.legs.size() && !found; ++i)
            {
                if (trip.legs[i].type != TravelRoute::LegType::Walk || TravelLegs::IsNoOp(trip.legs[i]))
                    continue;
                TravelLegs::StartLeg(trip, i, 0, nodeAt);
                for (std::uint32_t k = 1; k < trip.waypoints.size() && !found; ++k)
                {
                    TravelLegs::Waypoint const& from = trip.waypoints[k - 1];
                    TravelLegs::Waypoint const& to = trip.waypoints[k];
                    if ((from.node != TravelGraph::NO_NODE && to.node != TravelGraph::NO_NODE) || !refuses(from, to))
                        continue;
                    std::pair<Point, Point> const piece{from.at, to.at};
                    for (auto const& before : run.refused)
                        run.repeated = run.repeated || samePiece(before, piece);
                    run.refused.push_back(piece);
                    trip.next = k;
                    found = true;
                    run.story += from.node == TravelGraph::NO_NODE && to.node == TravelGraph::NO_NODE ? " direct"
                                 : to.node == TravelGraph::NO_NODE                                    ? " to the goal"
                                                                                                      : " to a node";
                    request.snapshot.position = from.at;  // the follower refuses a walk where it starts
                }
            }
            if (!found)
            {
                run.outcome = "a route of";
                for (TravelRoute::Leg const& leg : route.legs)
                    run.outcome += std::string(" ") + TravelRoute::LegTypeWord(leg.type);
                break;
            }
            if (TravelLegs::AfterFailure(trip, TravelLegs::FailReason::NoWalk) == TravelLegs::Recovery::Abort)
                run.outcome = "abort:no_walk";
        }
        return run;
    };
    auto const firstNode = [](TravelRoute::Result const& route)
    {
        return !route.ok || route.legs.empty() || route.legs.front().nodes.empty() ? TravelGraph::NO_NODE
                                                                                   : route.legs.front().nodes.front();
    };

    // Thunder Bluff: the mesa's top is its own mesh
    Point const bluffFoot{1, -1290.7f, 172.0f, 67.4f};
    Point const bluffTop{1, -1274.74f, 121.51f, 134.98f};
    auto const onTop = [&](Point const& at)
    { return at.mapId == 1 && at.z > 120.0f && std::hypot(at.x - bluffTop.x, at.y - bluffTop.y) < 150.0f; };
    auto const below = [&](Point const& at)
    { return at.mapId == 1 && at.z < 100.0f && std::hypot(at.x - bluffTop.x, at.y - bluffTop.y) < 250.0f; };
    TravelRoute::Request climb;
    climb.snapshot.position = bluffFoot;
    climb.snapshot.team = 1;
    climb.snapshot.level = 10;
    climb.snapshot.money = 10000;
    climb.snapshot.allowTransport = true;
    climb.goal = bluffTop;
    Loop const mesa = loop(climb, [&](TravelLegs::Waypoint const& from, TravelLegs::Waypoint const& to)
                           { return (onTop(from.at) && below(to.at)) || (below(from.at) && onTop(to.at)); });
    check(mesa.proposedDirect, "Thunder Bluff: the planner does not propose the 86 yd direct walk from the foot");
    check(!mesa.refused.empty() && !mesa.repeated && !mesa.outcome.empty(),
          "Thunder Bluff: a refused walk was planned again, or the refusals did not end within MAX_REPLANS");
    notes.push_back("n) Thunder Bluff foot -> mesa, refused:" + mesa.story + ", then " + mesa.outcome);

    // A road node (two walk links) with a goal 60 yd off, as i)
    std::uint32_t road = TravelGraph::NO_NODE;
    for (std::uint32_t index = 0; index < graph.NodeCount() && road == TravelGraph::NO_NODE; ++index)
    {
        std::size_t walks = 0;
        for (TravelGraphLink const& link : graph.LinksOf(index))
            walks += link.type == TravelLinkType::Walk ? 1 : 0;
        if (walks >= 2)
            road = index;
    }
    if (!check(road != TravelGraph::NO_NODE, "no node with two walk links"))
        return checks;
    Point const at = nodeAt(road);
    TravelRoute::Request near;
    near.snapshot.position = {at.mapId, at.x + 15.0f, at.y, at.z};
    near.goal = {at.mapId, at.x + 75.0f, at.y, at.z};
    // The start takes the direct walk and the walk to the first route's first node only
    TravelRoute::Request viaNodes = near;
    viaNodes.noDirect = true;
    std::uint32_t const firstRefused =
        firstNode(TravelRoute::Search(graph, viaNodes, TravelRoute::StuckEdges{}, TravelRoute::Params{}));
    Loop const start = loop(near,
                            [&](TravelLegs::Waypoint const& from, TravelLegs::Waypoint const& to)
                            {
                                return TravelRoute::Distance(from.at, near.snapshot.position) <= 1.0f &&
                                       (to.node == TravelGraph::NO_NODE || to.node == firstRefused);
                            });
    check(start.proposedDirect && start.refused.size() == 2 && !start.repeated && start.routes.size() == 3 &&
              firstRefused != TravelGraph::NO_NODE && firstNode(start.routes[1]) == firstRefused &&
              (!start.routes[2].ok ||
               (firstNode(start.routes[2]) != TravelGraph::NO_NODE && firstNode(start.routes[2]) != firstRefused)),
          "road: after the direct walk and the walk to the first node were refused, the route does not start at "
          "another node");
    notes.push_back("n) road node " + std::to_string(graph.GetNode(road).id) + ", refused:" + start.story + ", then " +
                    start.outcome);
    // The goal takes no walk to it: every refusal is a new walk, and the trip ends
    Loop const closed =
        loop(near, [&](TravelLegs::Waypoint const&, TravelLegs::Waypoint const& to)
             { return to.node == TravelGraph::NO_NODE && TravelRoute::Distance(to.at, near.goal) < 1.0f; });
    check(!closed.repeated && closed.refused.size() >= 2 &&
              (closed.outcome == "abort:no_walk" || closed.outcome == "no route" ||
               closed.outcome == "no travel node on the goal map"),
          "road: a goal without a walk to it does not end the trip by refusals, or repeats one");
    notes.push_back("n) road node, closed goal, refused:" + closed.story + ", then " + closed.outcome);
    return checks;
}

// w) Wait points on a hand-made graph: land A (map 0) - wait point W1 - stop S1 = ship = stop S2 (map 1) - wait point
//    W2 - land B, with walks straight onto both stops that no route takes, and a second wait point W3 of S1 up a
//    cliff. The crossing is one transport leg W1, S1, S2, W2; without the wait points' links (the shipped graph's
//    shape) it is S1, S2 as before; a timed-out crossing leaves the ride out; wait-point links without a ride are a
//    walk
std::uint32_t WaitPointSelftest(std::vector<std::string>& failures)
{
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back("w) " + what);
        return ok;
    };
    enum : std::uint32_t
    {
        A,
        W1,
        S1,
        S2,
        W2,
        B,
        W3
    };
    constexpr std::uint32_t SHIP = 20808;
    constexpr std::uint32_t SHIP_PERIOD_MS = 240000;  // an explicit test period: the wait is half of it
    constexpr float SHIP_RIDE_S = 90.0f;
    // near, far: the wait points of S1, S2; a stop with one is reached only through it (no tie with a direct walk)
    auto const build = [](TravelGraph& graph, bool near, bool far)
    {
        TravelGraphBuilder builder;
        builder.AddNode(A, "A", 0, 0.0f, 0.0f, 0.0f);
        builder.AddNode(W1, "W1dock", 0, 60.0f, 0.0f, 0.0f);
        builder.AddNode(S1, "S1", 0, 80.0f, 0.0f, 0.0f);
        builder.AddNode(S2, "S2", 1, 0.0f, 0.0f, 0.0f);
        builder.AddNode(W2, "W2dock", 1, 20.0f, 0.0f, 0.0f);
        builder.AddNode(B, "B", 1, 80.0f, 0.0f, 0.0f);
        builder.AddNode(W3, "W3dock", 0, 90.0f, 0.0f, 200.0f);
        // The ship's stored crossings (WT70c: no crossing, no ride): a boarding and a leaving row at both stops
        for (std::uint32_t stop = 0; stop < 2; ++stop)
            for (std::uint32_t direction = 0; direction < 2; ++direction)
            {
                TravelGraphBuilder::CrossingRow row;
                row.entry = SHIP;
                row.stop = stop;
                row.mapId = stop;
                row.x = stop ? 0.0f : 80.0f;
                row.direction = direction;
                builder.AddCrossing(row);
            }
        auto const link =
            [&builder](std::uint32_t from, std::uint32_t to, std::uint32_t type, std::uint32_t object, float distance)
        {
            for (auto const& [a, b] : {std::pair{from, to}, std::pair{to, from}})
            {
                TravelGraphBuilder::LinkRow row;
                row.fromId = a;
                row.toId = b;
                row.type = type;
                row.object = object;
                row.distance = distance;
                row.extraCost = type == 3 && object ? 0.0033f : 0.0f;
                row.periodMs = type == 3 && object ? SHIP_PERIOD_MS : 0;
                row.rideSeconds = type == 3 && object ? SHIP_RIDE_S : 0.0f;
                builder.AddLink(row);
            }
        };
        link(A, W1, 1, 0, 60.0f);
        link(W2, B, 1, 0, 60.0f);
        link(S1, S2, 3, SHIP, 0.1f);
        if (near)
        {
            link(W1, S1, 3, 0, 20.0f);
            link(S1, W3, 3, 0, 30.0f);
        }
        else
            link(A, S1, 1, 0, 80.0f);
        if (far)
            link(S2, W2, 3, 0, 20.0f);
        else
            link(S2, B, 1, 0, 80.0f);
        std::string error;
        builder.EndLinks(error);
        builder.AddPoint(A, W1, 0, 0, 0.0f, 0.0f, 0.0f);
        builder.Finish(graph, error);
        graph.Publish(0, 0);
    };

    TravelRoute::Request request;
    request.snapshot.position = {0, 1.0f, 0.0f, 0.0f};
    request.snapshot.allowTransport = true;
    request.goal = {1, 81.0f, 0.0f, 0.0f};
    auto const ids = [](TravelGraph const& graph, TravelRoute::Leg const& leg)
    {
        std::vector<std::uint32_t> out;
        for (std::uint32_t const node : leg.nodes)
            out.push_back(graph.GetNode(node).id);
        return out;
    };
    auto const transports = [](TravelRoute::Result const& route)
    {
        return std::count_if(route.legs.begin(), route.legs.end(),
                             [](TravelRoute::Leg const& leg) { return leg.type == TravelRoute::LegType::Transport; });
    };

    TravelGraph waits;
    build(waits, true, true);
    TravelRoute::Result const crossed = TravelRoute::Search(waits, request, TravelRoute::StuckEdges{}, {});
    auto const ride = std::find_if(crossed.legs.begin(), crossed.legs.end(), [](TravelRoute::Leg const& leg)
                                   { return leg.type == TravelRoute::LegType::Transport; });
    if (check(crossed.ok && transports(crossed) == 1 && ride != crossed.legs.end(), "no single transport leg"))
    {
        check(ids(waits, *ride) == std::vector<std::uint32_t>{W1, S1, S2, W2} && ride->object == SHIP,
              "the transport leg runs W1, S1, S2, W2 with the ship");
        check(ride->from.x == 60.0f && ride->to.mapId == 1 && ride->to.x == 20.0f,
              "the transport leg runs from wait point to wait point");
        check(ride->rideAt == 1 && ride->waitFrom && ride->waitTo && ride->stopFrom.x == 80.0f &&
                  ride->stopTo.mapId == 1 && ride->stopTo.x == 0.0f,
              "the transport leg knows its ride and its stops");
        check(crossed.legs.size() == 3 && crossed.legs.front().type == TravelRoute::LegType::Walk &&
                  crossed.legs.back().type == TravelRoute::LegType::Walk,
              "a walk to the wait point, the crossing, a walk on");
        float const walks = (20.0f + 20.0f) / request.snapshot.runSpeed;
        check(std::fabs(ride->seconds - (walks + SHIP_PERIOD_MS / 2000.0f + SHIP_RIDE_S)) < 0.01f &&
                  ride->periodMs == SHIP_PERIOD_MS,
              "the wait points' links cost their walk, the ride half its period plus the crossing; the leg has the "
              "period");

        TravelLegs::Trip trip;
        trip.legs = crossed.legs;
        trip.legIdx = static_cast<std::uint32_t>(ride - crossed.legs.begin());
        TravelLegs::NoteTransportTimeout(trip);
        check(trip.avoidEdges.size() == 1 &&
                  trip.avoidEdges.front() ==
                      std::pair<std::uint32_t, std::uint32_t>{waits.IndexOfId(S1), waits.IndexOfId(S2)},
              "a timed-out crossing leaves its ride out");
        TravelRoute::Request again = request;
        again.avoidEdges = trip.avoidEdges;
        check(TravelRoute::Search(waits, again, TravelRoute::StuckEdges{}, {}).failure == TravelRoute::Failure::NoRoute,
              "without the ride no route");
    }
    TravelRoute::Request ashore = request;
    ashore.snapshot.allowTransport = false;
    check(TravelRoute::Search(waits, ashore, TravelRoute::StuckEdges{}, {}).failure == TravelRoute::Failure::NoRoute,
          "without transports no route");

    TravelGraph plain;
    build(plain, false, false);
    TravelRoute::Result const old = TravelRoute::Search(plain, request, TravelRoute::StuckEdges{}, {});
    auto const oldRide = std::find_if(old.legs.begin(), old.legs.end(), [](TravelRoute::Leg const& leg)
                                      { return leg.type == TravelRoute::LegType::Transport; });
    check(old.ok && transports(old) == 1 && oldRide != old.legs.end() &&
              ids(plain, *oldRide) == std::vector<std::uint32_t>{S1, S2} && oldRide->from.x == 80.0f,
          "without wait points the ride runs S1, S2 from the stop");
    check(oldRide != old.legs.end() && oldRide->rideAt == 0 && !oldRide->waitFrom && !oldRide->waitTo,
          "without wait points the leg has none");

    // A wait point at one end only (WT55b): a timeout leaves the ride out, not the wait point's link
    for (bool const nearEnd : {true, false})
    {
        std::string const side = nearEnd ? "near wait point only" : "far wait point only";
        TravelGraph half;
        build(half, nearEnd, !nearEnd);
        TravelRoute::Result const route = TravelRoute::Search(half, request, TravelRoute::StuckEdges{}, {});
        auto const leg = std::find_if(route.legs.begin(), route.legs.end(), [](TravelRoute::Leg const& each)
                                      { return each.type == TravelRoute::LegType::Transport; });
        if (!check(route.ok && transports(route) == 1 && leg != route.legs.end(), side + ": no single transport leg"))
            continue;
        std::vector<std::uint32_t> const expected =
            nearEnd ? std::vector<std::uint32_t>{W1, S1, S2} : std::vector<std::uint32_t>{S1, S2, W2};
        check(ids(half, *leg) == expected && leg->waitFrom == nearEnd && leg->waitTo == !nearEnd &&
                  leg->rideAt == (nearEnd ? 1u : 0u) && leg->stopFrom.x == 80.0f && leg->stopTo.x == 0.0f &&
                  leg->from.x == (nearEnd ? 60.0f : 80.0f) && leg->to.x == (nearEnd ? 0.0f : 20.0f),
              side + ": its nodes, ends and stops");
        TravelLegs::Trip trip;
        trip.legs = route.legs;
        trip.legIdx = static_cast<std::uint32_t>(leg - route.legs.begin());
        TravelLegs::NoteTransportTimeout(trip);
        check(trip.avoidEdges.size() == 1 &&
                  trip.avoidEdges.front() ==
                      std::pair<std::uint32_t, std::uint32_t>{half.IndexOfId(S1), half.IndexOfId(S2)},
              side + ": a timed-out crossing leaves its ride out");
    }

    TravelRoute::Request up = request;
    up.goal = {0, 91.0f, 0.0f, 200.0f};
    TravelRoute::Result const climbed = TravelRoute::Search(waits, up, TravelRoute::StuckEdges{}, {});
    check(climbed.ok && climbed.legs.size() == 1 && climbed.legs.front().type == TravelRoute::LegType::Walk &&
              ids(waits, climbed.legs.front()) == std::vector<std::uint32_t>{W1, S1, W3},
          "wait-point links without a ride are one walk leg");
    return checks;
}

// x) Air links (WT61) on a made-up Northrend graph: a coast A - B, an island I - J (five nodes each: an end's nearest
//    nodes stay on its side) that only an air link B <-> I joins, and with bridge a long walk A - J. Over a flat open
//    height map a bot that may fly gets a fly leg onto the island, one that may not gets no route; without the air rows
//    both routes on the coast are as before; an island whose cells are closed fails the flight, and the search then
//    leaves the link out
std::uint32_t AirSelftest(std::vector<std::string>& failures)
{
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back("x) " + what);
        return ok;
    };
    constexpr std::uint32_t SIDE_NODES = 5;
    constexpr std::uint32_t A = 0;
    constexpr std::uint32_t B = SIDE_NODES - 1;
    constexpr std::uint32_t I = SIDE_NODES;
    constexpr std::uint32_t J = 2 * SIDE_NODES - 1;
    constexpr std::uint32_t MAP = TravelRoute::FLY_MAP_NORTHREND;
    std::vector<TravelRoute::Point> at;
    for (std::uint32_t node = 0; node < SIDE_NODES; ++node)
        at.push_back({MAP, 100.0f * static_cast<float>(node), 0.0f, 100.0f});
    for (std::uint32_t node = 0; node < SIDE_NODES; ++node)
        at.push_back({MAP, 400.0f + 60.0f * static_cast<float>(node), 700.0f, 300.0f});
    auto const build = [&at](TravelGraph& graph, std::uint32_t airLinks, bool bridge)
    {
        TravelGraphBuilder builder;
        for (std::uint32_t node = A; node <= J; ++node)
            builder.AddNode(node, "n" + std::to_string(node), MAP, at[node].x, at[node].y, at[node].z);
        auto const link = [&builder, &at](std::uint32_t from, std::uint32_t to, std::uint32_t type, float distance)
        {
            for (auto const& [a, b] : {std::pair{from, to}, std::pair{to, from}})
            {
                TravelGraphBuilder::LinkRow row;
                row.fromId = a;
                row.toId = b;
                row.type = type;
                row.distance = distance > 0.0f ? distance : TravelRoute::Distance(at[a], at[b]);
                builder.AddLink(row);
            }
        };
        for (std::uint32_t node = 0; node + 1 < SIDE_NODES; ++node)
        {
            link(A + node, A + node + 1, 1, 0.0f);
            link(I + node, I + node + 1, 1, 0.0f);
        }
        for (std::uint32_t k = 0; k < airLinks; ++k)
            link(B - k, I + k, 7, 0.0f);
        if (bridge)
            link(A, J, 1, 6000.0f);
        std::string error;
        builder.EndLinks(error);
        builder.AddPoint(A, A + 1, 0, MAP, 0.0f, 0.0f, 100.0f);
        builder.Finish(graph, error);
        graph.Publish(0, 0);
        return error;
    };
    TravelGraph withAir;
    TravelGraph without;
    TravelGraph bridged;
    TravelGraph threeAir;
    TravelGraph walkBridge;
    std::string const error = build(withAir, 1, false) + build(without, 0, false) + build(bridged, 1, true) +
                              build(threeAir, 3, true) + build(walkBridge, 0, true);
    if (!check(error.empty() && withAir.IsReady() && withAir.Stats().linksByType[7] == 2 &&
                   without.Stats().linksByType[7] == 0 && withAir.Stats().skippedLinks == 0,
               "the loader keeps the air links (2 and 0): " + error + " air " +
                   std::to_string(withAir.Stats().linksByType[7]) + ", skipped " +
                   std::to_string(withAir.Stats().skippedLinks)))
        return checks;

    auto const maps = [](std::function<bool(float, float)> const& openAt)
    {
        auto set = std::make_shared<FlightRules::FlightMapSet>();
        set->maps.push_back(
            FlightRules::MakeMap(MAP, -1500.0f, -1500.0f, 67.0f, 60, 60, [](float, float) { return 100.0f; }, openAt));
        return std::shared_ptr<FlightRules::FlightMapSet const>(std::move(set));
    };
    TravelRoute::Params open;
    open.flightMaps = maps([](float, float) { return true; });
    TravelRoute::Params closedIsland;
    closedIsland.flightMaps = maps([](float, float y) { return y < 500.0f; });

    TravelRoute::Snapshot flyer;
    flyer.position = {MAP, 2.0f, 0.0f, 100.0f};
    flyer.level = 80;
    flyer.flyNorthrend = true;
    flyer.flySpeed = 7.0f * 3.8f;
    TravelRoute::Snapshot walker = flyer;
    walker.flyNorthrend = false;
    walker.flySpeed = 0.0f;
    TravelRoute::Point const island{MAP, 642.0f, 700.0f, 300.0f};
    TravelRoute::Point const coast{MAP, 402.0f, 0.0f, 100.0f};
    auto const search = [](TravelGraph const& graph, TravelRoute::Snapshot const& bot, TravelRoute::Point const& goal,
                           TravelRoute::Params const& params)
    { return TravelRoute::Search(graph, TravelRoute::Request{bot, goal}, TravelRoute::StuckEdges{}, params); };
    auto const airLegs = [](TravelRoute::Result const& route)
    {
        return std::count_if(route.legs.begin(), route.legs.end(), [](TravelRoute::Leg const& leg) { return leg.air; });
    };
    auto const same = [](TravelRoute::Point const& a, TravelRoute::Point const& b)
    { return a.mapId == b.mapId && TravelRoute::Distance(a, b) < 0.5f; };

    TravelRoute::Result const flown = search(withAir, flyer, island, open);
    auto const fly =
        std::find_if(flown.legs.begin(), flown.legs.end(), [](TravelRoute::Leg const& leg) { return leg.air; });
    bool const flies = flown.ok && airLegs(flown) == 1 && fly != flown.legs.end();
    check(flies, "a bot that may fly gets a route with one air leg onto the island");
    if (flies)
        check(fly->type == TravelRoute::LegType::Fly && same(fly->from, at[B]) && same(fly->to, at[I]) &&
                  fly->flightPoints.size() >= 2 && same(fly->flightPoints.front(), at[B]) &&
                  same(fly->flightPoints.back(), at[I]) && fly->nodes.size() == 2 &&
                  withAir.GetNode(fly->nodes.front()).id == B && withAir.GetNode(fly->nodes.back()).id == I &&
                  flown.legs.back().type == TravelRoute::LegType::Walk && same(flown.legs.back().to, island),
              "the air leg is a fly leg from B to I over its planned flight, then a walk on the island");
    TravelRoute::Params dijkstra = open;
    dijkstra.heuristic = false;
    TravelRoute::Result const reference = search(withAir, flyer, island, dijkstra);
    check(reference.ok && std::fabs(reference.totalSeconds - flown.totalSeconds) < 0.01f,
          "A* and Dijkstra plan the same seconds over an air link");
    // Without flight maps every point may take off and land (WT61b): the flyer's legs are fly legs, listed unflown
    TravelRoute::Result const unflown = search(withAir, flyer, island, {});
    check(unflown.ok && unflown.flightSearches == 0 &&
              std::any_of(unflown.legs.begin(), unflown.legs.end(),
                          [](TravelRoute::Leg const& leg) { return leg.type == TravelRoute::LegType::Fly; }) &&
              std::all_of(unflown.legs.begin(), unflown.legs.end(),
                          [](TravelRoute::Leg const& leg) { return leg.flightPoints.empty(); }),
          "without flight maps the fly legs are listed unflown");
    check(search(withAir, walker, island, open).failure == TravelRoute::Failure::NoRoute,
          "a bot that may not fly gets no route onto the island");
    TravelRoute::Request grounded{flyer, island};
    grounded.noFly = true;
    check(TravelRoute::Search(withAir, grounded, TravelRoute::StuckEdges{}, open).failure ==
              TravelRoute::Failure::NoRoute,
          "a request with noFly takes no air link");
    check(search(without, flyer, island, open).failure == TravelRoute::Failure::NoRoute,
          "without air links the island has no route");
    for (TravelRoute::Snapshot const& bot : {flyer, walker})
    {
        TravelRoute::Result const before = search(without, bot, coast, open);
        TravelRoute::Result const after = search(withAir, bot, coast, open);
        bool equal = before.ok && after.ok && before.legs.size() == after.legs.size() &&
                     std::fabs(before.totalSeconds - after.totalSeconds) < 0.001f;
        for (std::size_t k = 0; equal && k < before.legs.size(); ++k)
            equal = before.legs[k].type == after.legs[k].type && before.legs[k].nodes == after.legs[k].nodes;
        check(equal, std::string(bot.flySpeed > 0.0f ? "flyer" : "walker") +
                         ": a coast route with air links is the one without");
    }
    TravelRoute::Result const refused = search(withAir, flyer, island, closedIsland);
    check(refused.failure == TravelRoute::Failure::NoRoute && refused.flightSearches >= 1,
          "a closed island fails the air leg's flight: no route");
    TravelRoute::Result const around = search(bridged, flyer, island, closedIsland);
    check(around.ok && airLegs(around) == 0 && around.flightSearches >= 1,
          "a failed air flight leaves the link out: the bot takes the bridge");
    TravelRoute::Result const over = search(bridged, flyer, island, open);
    check(over.ok && airLegs(over) == 1, "with the bridge the open island is still reached by air");
    // WT61b: more failed flights than the search's tries never hide the ground way (the last search flies no edge)
    TravelRoute::Result const afterThree = search(threeAir, flyer, island, closedIsland);
    check(afterThree.ok && airLegs(afterThree) == 0 && afterThree.flightSearches >= 1,
          "after three failed air flights the bot takes the bridge");
    TravelRoute::Snapshot fromB = flyer;
    fromB.position = at[B];
    fromB.takeoffHere = false;
    TravelRoute::Result const spot = search(bridged, fromB, island, open);
    check(spot.ok && airLegs(spot) == 0, "no air leg starts where the bot stands and cannot take off");
    // A trip whose fly leg over the link failed (TravelLegs::NoteAirFailed) asks with the link among its avoided edges
    if (flies)
    {
        TravelRoute::Request avoided{flyer, island};
        avoided.avoidEdges.push_back({fly->nodes.front(), fly->nodes.back()});
        check(TravelRoute::Search(withAir, avoided, TravelRoute::StuckEdges{}, open).failure ==
                  TravelRoute::Failure::NoRoute,
              "a trip that avoids the air link gets no route onto the island");
        TravelRoute::Result const walked = TravelRoute::Search(bridged, avoided, TravelRoute::StuckEdges{}, open);
        check(walked.ok && airLegs(walked) == 0, "a trip that avoids the air link takes the bridge");
    }
    // WT61b review: a trip that could not take off at a spot starts no air link there either
    TravelRoute::Request spotB{flyer, island};
    spotB.noTakeoffs.push_back(at[B]);
    TravelRoute::Result const notFromB = TravelRoute::Search(bridged, spotB, TravelRoute::StuckEdges{}, open);
    check(notFromB.ok && airLegs(notFromB) == 0, "no air leg starts at a spot the trip could not take off at");
    // Takeoffs refused spot after spot (lab10: a cave, indoors at every node; the bot went to and fro between two of
    // them). The bot stands at the coast's second node, the goal lies on open ground far north; each refused takeoff
    // replans from the refused spot (TravelLegs::AfterFailure, NextRequest). Every replan takes off at a new node,
    // and with every node of the coast refused the route walks
    TravelRoute::Point const north{MAP, 200.0f, 1400.0f, 100.0f};
    TravelLegs::Trip cave;
    TravelRoute::Snapshot inCave = flyer;
    inCave.position = at[A + 1];
    inCave.takeoffHere = false;
    std::vector<TravelRoute::Point> spots{inCave.position};
    bool fresh = true;
    TravelRoute::Result last;
    for (std::uint32_t round = 0; round < 2 * SIDE_NODES; ++round)
    {
        last = TravelRoute::Search(walkBridge, TravelLegs::NextRequest(cave, inCave, north), TravelRoute::StuckEdges{},
                                   open);
        auto const takeoff = std::find_if(last.legs.begin(), last.legs.end(), [](TravelRoute::Leg const& leg)
                                          { return leg.type == TravelRoute::LegType::Fly; });
        if (!last.ok || takeoff == last.legs.end())
            break;
        fresh = fresh && std::none_of(spots.begin(), spots.end(), [&](TravelRoute::Point const& spot)
                                      { return TravelRoute::Distance(spot, takeoff->from) <= 10.0f; });
        spots.push_back(takeoff->from);
        cave.legs = last.legs;
        cave.legIdx = static_cast<std::uint32_t>(takeoff - last.legs.begin());
        if (TravelLegs::AfterFailure(cave, TravelLegs::FailReason::Takeoff, true) != TravelLegs::Recovery::Replan)
            break;
        inCave.position = takeoff->from;
    }
    check(fresh && spots.size() == SIDE_NODES, "refused takeoffs: each replan takes off at a new spot (" +
                                                   std::to_string(spots.size() - 1) + " takeoffs, " +
                                                   (fresh ? "none" : "one") + " at a spot refused before)");
    check(last.ok && !cave.noFly &&
              std::none_of(last.legs.begin(), last.legs.end(),
                           [](TravelRoute::Leg const& leg) { return leg.type == TravelRoute::LegType::Fly; }),
          "with a takeoff refused at every node of the coast the route walks");
    return checks;
}
}  // namespace

int main(int argc, char** argv)
{
    std::filesystem::path const root = argc > 1 ? argv[1] : ".";
    std::filesystem::path const dbcDir = argc > 2 ? argv[2] : "C:/Games/AzerothCore/Server/data/dbc";
    std::filesystem::path const graphDir = argc > 3 ? argv[3] : "";

    TravelGraph graph;
    TravelGraphSql::LoadInfo info;
    std::string error;
    if (!TravelGraphSql::Load(root, dbcDir, graph, info, error, graphDir))
    {
        std::cout << "route-rules-selftest: build failed: " << error << "\n";
        return 2;
    }
    TravelGraphStats const& stats = graph.Stats();
    std::cout << "graph: " << stats.nodes << " nodes, " << stats.links << " links, " << stats.taxiPaths
              << " taxi paths (" << stats.flightsWithoutTaxi << " flight links without one), parsed in " << info.loadMs
              << " ms\n";

    std::vector<std::string> failures;
    std::vector<std::string> notes;
    auto const started = std::chrono::steady_clock::now();
    std::uint32_t const checks =
        TravelRoute::Selftest(graph, failures, notes) + TravelRoute::ServiceSelftest(graph, failures, notes) +
        TravelPlaces::Selftest(graph, failures, notes) + StuckReplanSelftest(graph, failures, notes) +
        NoWalkReplanSelftest(graph, root, failures, notes) + WaitPointSelftest(failures) + AirSelftest(failures);
    auto const selftestMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    for (std::string const& note : notes)
        std::cout << "  " << note << "\n";
    for (std::string const& failure : failures)
        std::cout << "FAIL: " << failure << "\n";
    std::cout << "route-rules-selftest: " << (checks - failures.size()) << "/" << checks << " passed (checks in "
              << selftestMs << " ms)\n";
    return failures.empty() ? 0 : 1;
}
