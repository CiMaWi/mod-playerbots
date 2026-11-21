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
// w) a crossing through wait points on a hand-made graph; x) the two ends of every ride on one map (WT70g); y) no walk
// leg holds a wait point's link and no transport leg is without a ride (WT70g-d and its review); z) an end at a node
// joins the graph there alone at no cost (NodeEnds, with a mutant per rule).
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
// The record of failed attempts as the follower writes it at a leg's typed end (NewRpgTravelAction::LegFailed): the
// leg's key (TravelLegs::FailureOf) under its fail word, then AfterFailure. The replan's request carries the plain copy
// of the live entries (TravelLegs::NextRequest)
constexpr std::uint32_t RECORD_NOW_MS = 1000;
constexpr std::uint32_t RECORD_BASE_MS = 600 * 1000;

TravelLegs::Recovery Failed(FailedAttempts::Record& record, TravelLegs::Trip& trip, TravelLegs::FailReason reason,
                            TravelRoute::Point const& goal, TravelRoute::Point const& spot, bool spotClosed = false)
{
    FailedAttempts::NoteFailure(record, TravelLegs::FailWord(reason), TravelLegs::FailureOf(trip, goal, spot),
                                RECORD_NOW_MS, RECORD_BASE_MS);
    return TravelLegs::AfterFailure(trip, reason, spotClosed);
}

TravelRoute::Request Copied(FailedAttempts::Record const& record, TravelRoute::Request const& request)
{
    TravelRoute::Request next =
        TravelLegs::NextRequest(request.snapshot, request.goal, FailedAttempts::LiveEntries(record, RECORD_NOW_MS));
    next.freeRides = request.freeRides;
    return next;
}

// i) Replans after stuck walks (the record's walk entries, through the request's copy) near a road node: a goal 60 yd
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
    FailedAttempts::Record record;
    auto const replan = [&]() -> TravelRoute::Result
    {
        request = Copied(record, request);
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
    check(Failed(record, trip, FailReason::Stuck, request.goal, request.snapshot.position) == Recovery::Replan &&
              TravelRoute::RefusalsOf(Copied(record, request)).AvoidsWalk(request.snapshot.position, request.goal),
          "i) stuck on the straight walk: a replan without it");
    TravelRoute::Result const viaNodes = replan();
    std::uint32_t const first = firstNode(viaNodes);
    if (!check(viaNodes.ok && first != TravelGraph::NO_NODE && trip.waypoints.size() >= 3 && trip.next == 1 &&
                   trip.waypoints[1].node == first,
               "i) the route without the straight walk runs through nodes"))
        return checks;

    check(Failed(record, trip, FailReason::Stuck, request.goal, request.snapshot.position) == Recovery::Replan &&
              TravelRoute::RefusalsOf(Copied(record, request)).AvoidsWalk(request.snapshot.position, nodeAt(first)),
          "i) stuck on the first hop: a replan without the walk to its node");
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
        check(Failed(record, trip, FailReason::Stuck, request.goal, request.snapshot.position) == Recovery::Replan &&
                  before != TravelGraph::NO_NODE,
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
        FailedAttempts::Record record;
        for (std::uint32_t round = 0; round <= TravelLegs::MAX_REPLANS + 1u && run.outcome.empty(); ++round)
        {
            request = Copied(record, request);
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
            if (Failed(record, trip, TravelLegs::FailReason::NoWalk, request.goal, request.snapshot.position) ==
                TravelLegs::Recovery::Abort)
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
    TravelRoute::RefuseWalk(viaNodes, near.snapshot.position, near.goal);
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
//    shape) it is S1, S2 as before; a timed-out crossing leaves the ride out; no route walks through a stop over two
//    wait-point links
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
        FailedAttempts::Record record;
        Failed(record, trip, TravelLegs::FailReason::Timeout, request.goal, request.snapshot.position);
        TravelRoute::Request const again = Copied(record, request);
        TravelRoute::Refusals const refused = TravelRoute::RefusalsOf(again);
        check(refused.links.size() == 1 &&
                  refused.links.front() ==
                      std::pair<std::uint32_t, std::uint32_t>{waits.IndexOfId(S1), waits.IndexOfId(S2)},
              "a timed-out crossing leaves its ride out");
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
        FailedAttempts::Record record;
        Failed(record, trip, TravelLegs::FailReason::Timeout, request.goal, request.snapshot.position);
        TravelRoute::Refusals const refused = TravelRoute::RefusalsOf(Copied(record, request));
        check(refused.links.size() == 1 &&
                  refused.links.front() ==
                      std::pair<std::uint32_t, std::uint32_t>{half.IndexOfId(S1), half.IndexOfId(S2)},
              side + ": a timed-out crossing leaves its ride out");
    }

    TravelRoute::Request up = request;
    up.goal = {0, 91.0f, 0.0f, 200.0f};
    TravelRoute::Result const climbed = TravelRoute::Search(waits, up, TravelRoute::StuckEdges{}, {});
    check(std::none_of(climbed.legs.begin(), climbed.legs.end(),
                       [&](TravelRoute::Leg const& leg)
                       {
                           std::vector<std::uint32_t> const nodes = ids(waits, leg);
                           return std::find(nodes.begin(), nodes.end(), S1) != nodes.end();
                       }),
          "no route walks through a stop over two wait-point links (WT.56)");
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
    // outdoorB, outdoorI: the stored outdoor flag of the air link's two ends (-1 = NULL, no answer), every other node
    // NULL
    auto const build = [&at](TravelGraph& graph, std::uint32_t airLinks, bool bridge, std::int8_t outdoorB = -1,
                             std::int8_t outdoorI = -1)
    {
        TravelGraphBuilder builder;
        for (std::uint32_t node = A; node <= J; ++node)
            builder.AddNode(node, "n" + std::to_string(node), MAP, at[node].x, at[node].y, at[node].z,
                            node == B ? outdoorB : (node == I ? outdoorI : static_cast<std::int8_t>(-1)));
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
    TravelRoute::RefuseFlying(grounded);
    check(TravelRoute::Search(withAir, grounded, TravelRoute::StuckEdges{}, open).failure ==
              TravelRoute::Failure::NoRoute,
          "a request refused flying takes no air link");
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
    // WT72: a node the graph marks as indoors (outdoor 0: a cave, a building under open ground) is no takeoff and no
    // landing, whatever the flight map says; NULL (-1) and 1 behave as without the flag
    TravelGraph caveTakeoff;
    TravelGraph caveLanding;
    TravelGraph openTakeoff;
    TravelGraph caveBridged;
    std::string const flagError = build(caveTakeoff, 1, false, 0, 1) + build(caveLanding, 1, false, 1, 0) +
                                  build(openTakeoff, 1, false, 1, 1) + build(caveBridged, 1, true, 0, 0);
    check(flagError.empty() && caveTakeoff.GetNode(caveTakeoff.IndexOfId(B)).outdoor == 0 &&
              withAir.GetNode(withAir.IndexOfId(B)).outdoor == -1,
          "the loader keeps the outdoor flag of a node: 0 and NULL");
    check(search(caveTakeoff, flyer, island, open).failure == TravelRoute::Failure::NoRoute,
          "a node marked indoors is no takeoff: no air leg from it");
    check(search(caveLanding, flyer, island, open).failure == TravelRoute::Failure::NoRoute,
          "a node marked indoors is no landing: no air leg to it");
    TravelRoute::Result const outdoors = search(openTakeoff, flyer, island, open);
    check(outdoors.ok && airLegs(outdoors) == 1, "nodes marked outdoors take the air leg as a NULL flag does");
    TravelRoute::Result const caveWalk = search(caveBridged, flyer, island, open);
    check(caveWalk.ok && airLegs(caveWalk) == 0, "a takeoff marked indoors leaves the bridge: the bot walks");
    // Without flight maps the flag is not read (as today: every point may take off)
    check(search(caveTakeoff, flyer, island, {}).ok, "without flight maps the indoors flag is not read");
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
    // A trip whose takeoff onto the link failed asks with the link refused (the record's link entry)
    if (flies)
    {
        TravelRoute::Request avoided{flyer, island};
        TravelRoute::RefuseLink(avoided, fly->nodes.front(), fly->nodes.back());
        check(TravelRoute::Search(withAir, avoided, TravelRoute::StuckEdges{}, open).failure ==
                  TravelRoute::Failure::NoRoute,
              "a trip that avoids the air link gets no route onto the island");
        TravelRoute::Result const walked = TravelRoute::Search(bridged, avoided, TravelRoute::StuckEdges{}, open);
        check(walked.ok && airLegs(walked) == 0, "a trip that avoids the air link takes the bridge");
    }
    // WT61b review: a trip that could not take off at a spot starts no air link there either
    FailedAttempts::Record atB;
    FailedAttempts::Failure takeoffB;
    takeoffB.leg = FailedAttempts::Leg::Fly;
    takeoffB.spot = TravelLegs::FailurePoint(at[B]);
    FailedAttempts::NoteFailure(atB, "takeoff", takeoffB, RECORD_NOW_MS, RECORD_BASE_MS);
    TravelRoute::Request const spotB = Copied(atB, TravelRoute::Request{flyer, island});
    TravelRoute::Result const notFromB = TravelRoute::Search(bridged, spotB, TravelRoute::StuckEdges{}, open);
    check(notFromB.ok && airLegs(notFromB) == 0, "no air leg starts at a spot the trip could not take off at");
    // Takeoffs refused spot after spot (lab10: a cave, indoors at every node; the bot went to and fro between two of
    // them). The bot stands at the coast's second node, the goal lies on open ground far north; each refused takeoff
    // replans from the refused spot (the record's takeoff spot, NextRequest). Every replan takes off at a spot whose
    // takeoff did not fail before (the start's own spot is closed by its snapshot only: no failure), and with every
    // node of the coast refused the route walks
    TravelRoute::Point const north{MAP, 200.0f, 1400.0f, 100.0f};
    TravelLegs::Trip cave;
    FailedAttempts::Record caveRecord;
    TravelRoute::Snapshot inCave = flyer;
    inCave.position = at[A + 1];
    inCave.takeoffHere = false;
    std::vector<TravelRoute::Point> spots;
    bool fresh = true;
    TravelRoute::Result last;
    TravelRoute::Request asked;
    for (std::uint32_t round = 0; round < 2 * SIDE_NODES; ++round)
    {
        asked = Copied(caveRecord, TravelRoute::Request{inCave, north});
        last = TravelRoute::Search(walkBridge, asked, TravelRoute::StuckEdges{}, open);
        auto const takeoff = std::find_if(last.legs.begin(), last.legs.end(), [](TravelRoute::Leg const& leg)
                                          { return leg.type == TravelRoute::LegType::Fly; });
        if (!last.ok || takeoff == last.legs.end())
            break;
        fresh = fresh && std::none_of(spots.begin(), spots.end(), [&](TravelRoute::Point const& spot)
                                      { return TravelRoute::Distance(spot, takeoff->from) <= 10.0f; });
        spots.push_back(takeoff->from);
        cave.legs = last.legs;
        cave.legIdx = static_cast<std::uint32_t>(takeoff - last.legs.begin());
        if (Failed(caveRecord, cave, TravelLegs::FailReason::Takeoff, north, takeoff->from, true) !=
            TravelLegs::Recovery::Replan)
            break;
        inCave.position = takeoff->from;
    }
    check(fresh && spots.size() + 1 >= SIDE_NODES && spots.size() <= SIDE_NODES,
          "refused takeoffs: each replan takes off at a new spot (" + std::to_string(spots.size()) + " takeoffs, " +
              (fresh ? "none" : "one") + " at a spot refused before)");
    check(last.ok && !TravelRoute::RefusalsOf(asked).flying &&
              std::none_of(last.legs.begin(), last.legs.end(),
                           [](TravelRoute::Leg const& leg) { return leg.type == TravelRoute::LegType::Fly; }),
          "with a takeoff refused at every node of the coast the route walks");
    return checks;
}

// Case 18 (WT.55): freeride prices every ride at no time, so a test's bot rides where a walk is quicker; a request
// without it is unchanged. The ride leg keeps its own seconds (a lift's period is read from them)
std::uint32_t FreeRideSelftest(std::vector<std::string>& failures)
{
    std::uint32_t checks = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back("r) " + what);
        return ok;
    };
    enum : std::uint32_t
    {
        LOW,
        HIGH
    };
    constexpr std::uint32_t LIFT = 4170;
    constexpr float WALK_YARDS = 280.0f;  // 40 s at 7 yd/s
    constexpr float RIDE_SECONDS = 60.0f;
    TravelGraph graph;
    TravelGraphBuilder builder;
    builder.AddNode(LOW, "low", 0, 0.0f, 0.0f, 0.0f);
    builder.AddNode(HIGH, "high", 0, WALK_YARDS, 0.0f, 0.0f);
    for (std::uint32_t stop = 0; stop < 2; ++stop)  // its stored crossings (no crossing, no ride)
        for (std::uint32_t direction = 0; direction < 2; ++direction)
        {
            TravelGraphBuilder::CrossingRow row;
            row.entry = LIFT;
            row.stop = stop;
            row.x = stop ? WALK_YARDS : 0.0f;
            row.direction = direction;
            builder.AddCrossing(row);
        }
    for (auto const& [from, to] : {std::pair{LOW, HIGH}, std::pair{HIGH, LOW}})
    {
        TravelGraphBuilder::LinkRow walk;
        walk.fromId = from;
        walk.toId = to;
        walk.type = 1;
        walk.distance = WALK_YARDS;
        builder.AddLink(walk);
        TravelGraphBuilder::LinkRow ride = walk;
        ride.type = 3;
        ride.object = LIFT;
        ride.extraCost = RIDE_SECONDS;
        builder.AddLink(ride);
    }
    std::string error;
    builder.EndLinks(error);
    builder.AddPoint(LOW, HIGH, 0, 0, 0.0f, 0.0f, 0.0f);
    builder.Finish(graph, error);
    graph.Publish(0, 0);

    TravelRoute::Request plain;
    plain.snapshot.position = {0, -1.0f, 0.0f, 0.0f};
    plain.snapshot.runSpeed = 7.0f;
    plain.snapshot.allowTransport = true;
    plain.goal = {0, WALK_YARDS + 1.0f, 0.0f, 0.0f};
    TravelRoute::Request riding = plain;
    riding.freeRides = true;
    auto const rides = [](TravelRoute::Result const& route)
    {
        return std::count_if(route.legs.begin(), route.legs.end(),
                             [](TravelRoute::Leg const& leg) { return leg.type == TravelRoute::LegType::Transport; });
    };
    TravelRoute::Result const walked = TravelRoute::Search(graph, plain, TravelRoute::StuckEdges{}, {});
    check(error.empty() && walked.ok && rides(walked) == 0 && walked.totalSeconds < RIDE_SECONDS,
          "case 18: without freeride the walk (40 s) beats the ride (60 s): " + error);
    TravelRoute::Result const ridden = TravelRoute::Search(graph, riding, TravelRoute::StuckEdges{}, {});
    auto const lift = std::find_if(ridden.legs.begin(), ridden.legs.end(), [](TravelRoute::Leg const& leg)
                                   { return leg.type == TravelRoute::LegType::Transport; });
    if (check(ridden.ok && rides(ridden) == 1 && lift != ridden.legs.end(),
              "case 18: with freeride the lift is ridden"))
    {
        float sum = 0.0f;
        for (TravelRoute::Leg const& leg : ridden.legs)
            sum += leg.seconds;
        check(lift->object == LIFT && std::fabs(lift->seconds - RIDE_SECONDS) < 0.01f &&
                  std::fabs(sum - ridden.totalSeconds) < 0.01f,
              "case 18: the ride leg keeps its 60 s and the legs sum to the route's seconds");
    }
    TravelRoute::Params dijkstra;
    dijkstra.heuristic = false;
    TravelRoute::Result const reference = TravelRoute::Search(graph, riding, TravelRoute::StuckEdges{}, dijkstra);
    check(reference.ok && rides(reference) == 1 && std::fabs(reference.totalSeconds - ridden.totalSeconds) < 0.01f,
          "case 18: A* and Dijkstra plan the same free ride");
    return checks;
}

// x) The two ends of every ride on one map in the shipped graph (WT70g, WT.56): a bot at a stop's wait point (the stop
//    itself where it has none) bound for the far stop's wait point. With freeRides the route rides, also where the
//    direct walk is shorter (the crossing's wait-point links are part of the free ride). Without it no pair is a
//    direct walk: both ends are at nodes (NodeEnds, PA11); a direct walk refused (the record walk entry, the replan
//    after no_walk) is never walked again. A short trip with no ride near gets its
//    direct walk at the cost it had, with or without freeRides
std::uint32_t RideEndsSelftest(TravelGraph const& graph, std::vector<std::string>& failures,
                               std::vector<std::string>& notes)
{
    using TravelRoute::LegType;
    using TravelRoute::Point;
    std::uint32_t checks = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back("x) " + what);
        return ok;
    };
    auto const pointOf = [&graph](std::uint32_t index) -> Point
    {
        TravelGraphNode const& node = graph.GetNode(index);
        return {node.mapId, node.x, node.y, node.z};
    };
    auto const endOf = [&](std::uint32_t stop) -> Point
    {
        for (TravelGraphLink const& link : graph.LinksOf(stop))
            if (TravelGraph::IsWaitPointLink(link))
                return pointOf(link.to);
        return pointOf(stop);
    };
    auto const rides = [](TravelRoute::Result const& route)
    {
        return std::any_of(route.legs.begin(), route.legs.end(),
                           [](TravelRoute::Leg const& leg) { return leg.type == LegType::Transport; });
    };
    auto const directWalk = [](TravelRoute::Result const& route)
    {
        return route.ok && route.legs.size() == 1 && route.legs.front().type == LegType::Walk &&
               route.legs.front().nodes.empty();
    };
    auto const name = [&](std::uint32_t object, std::uint32_t from, std::uint32_t to)
    {
        return std::to_string(object) + " " + std::to_string(graph.GetNode(from).id) + " -> " +
               std::to_string(graph.GetNode(to).id);
    };
    std::uint32_t pairs = 0;
    std::uint32_t walkedPairs = 0;
    for (std::uint32_t index = 0; index < graph.NodeCount(); ++index)
        for (TravelGraphLink const& link : graph.LinksOf(index))
        {
            if (link.type != TravelLinkType::Transport || TravelGraph::IsWaitPointLink(link) ||
                graph.GetNode(link.to).mapId != graph.GetNode(index).mapId)
                continue;
            ++pairs;
            TravelRoute::Request request;
            request.snapshot.position = endOf(index);
            request.snapshot.runSpeed = 7.0f;
            request.snapshot.team = 1;
            request.snapshot.level = 80;
            request.snapshot.money = 100000;
            request.snapshot.allowTransport = true;
            request.goal = endOf(link.to);
            TravelRoute::Request riding = request;
            riding.freeRides = true;
            TravelRoute::Result const ridden = TravelRoute::Search(graph, riding, TravelRoute::StuckEdges{}, {});
            check(ridden.ok && rides(ridden), "with freeRides " + name(link.object, index, link.to) + " does not ride");
            TravelRoute::Result const plain = TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, {});
            if (!directWalk(plain))
                continue;
            ++walkedPairs;
            TravelRoute::Request refused = request;
            TravelRoute::RefuseWalk(refused, request.snapshot.position, request.goal);
            TravelRoute::Result const replanned = TravelRoute::Search(graph, refused, TravelRoute::StuckEdges{}, {});
            check(!directWalk(replanned),
                  "the direct walk " + name(link.object, index, link.to) + " refused: the replan walks it again");
        }
    check(pairs > 0 && walkedPairs == 0, "the graph has no rides, or a ride's two ends at nodes are walked directly");
    // A short trip off any ride: 40 yd beside a road node, once with and once without freeRides
    for (std::uint32_t index = 0; index < graph.NodeCount(); ++index)
    {
        Point const at = pointOf(index);
        bool rideNear = false;
        for (std::uint32_t const node : graph.NearestNodes(at.mapId, at.x, at.y, at.z, 20))
            for (TravelGraphLink const& link : graph.LinksOf(node))
                rideNear = rideNear || link.type == TravelLinkType::Transport;
        if (rideNear || graph.LinksOf(index).empty())
            continue;
        TravelRoute::Request request;
        request.snapshot.position = {at.mapId, at.x + 1.0f, at.y + 1.0f, at.z};
        request.snapshot.runSpeed = 7.0f;
        request.snapshot.allowTransport = true;
        request.goal = {at.mapId, at.x + 41.0f, at.y + 1.0f, at.z};
        TravelRoute::Request riding = request;
        riding.freeRides = true;
        TravelRoute::Result const walked = TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, {});
        TravelRoute::Result const free = TravelRoute::Search(graph, riding, TravelRoute::StuckEdges{}, {});
        check(directWalk(walked) && directWalk(free) && std::fabs(walked.totalSeconds - 40.0f / 7.0f) < 0.01f &&
                  std::fabs(free.totalSeconds - walked.totalSeconds) < 0.01f,
              "a short trip beside node " + std::to_string(graph.GetNode(index).id) +
                  " is not one direct walk at its distance's cost");
        break;
    }
    notes.push_back("x) ride ends: " + std::to_string(pairs) + " rides, " + std::to_string(walkedPairs) +
                    " with a direct walk that wins without freeRides");
    return checks;
}

// y) A wait point's link to its stop is part of the ride (WT70g-d, WT.56): no route walks it. Every ride pair of x)
//    and the replans of a bot at lift 190118's top wait point bound for its foot (WT70g-c, Almin's seed: the direct
//    walk and the walk to the foot's node refused), with and without freeRides: no walk leg has two nodes in a row
//    joined only by a wait-point link, and no transport leg is without a ride (the review of WT70g-d: the route kept
//    the link 2210 -> 2209, which joins no stop, as a transport leg of its own). Across every wait-point link of the
//    graph the same; the shipped graph's stops have one wait point each (the search keeps one label per vertex, so a
//    second wait point of a stop is not reached after a ride when the first one labelled the stop: a per-arrival
//    label is due when this check fails); and on a hand-made lift a bot that may fly does not leave a stop it
//    entered over its wait point's link by a fly edge
std::uint32_t WaitLinkWalkSelftest(TravelGraph const& graph, std::vector<std::string>& failures)
{
    using TravelRoute::LegType;
    using TravelRoute::Point;
    std::uint32_t checks = 0;
    auto const pointOf = [&graph](std::uint32_t index) -> Point
    {
        TravelGraphNode const& node = graph.GetNode(index);
        return {node.mapId, node.x, node.y, node.z};
    };
    // The first wait-point link a walk leg of the route walks, as "leg i: id -> id"; empty when none
    auto const walkedWaitLink = [&graph](TravelRoute::Result const& route) -> std::string
    {
        for (std::size_t i = 0; i < route.legs.size(); ++i)
        {
            TravelRoute::Leg const& leg = route.legs[i];
            if (leg.type != LegType::Walk)
                continue;
            for (std::size_t k = 1; k < leg.nodes.size(); ++k)
            {
                bool walk = false;
                bool wait = false;
                for (TravelGraphLink const& link : graph.LinksOf(leg.nodes[k - 1]))
                    if (link.to == leg.nodes[k])
                    {
                        walk = walk || link.type == TravelLinkType::Walk;
                        wait = wait || TravelGraph::IsWaitPointLink(link);
                    }
                if (wait && !walk)
                    return "leg " + std::to_string(i + 1) + "/" + std::to_string(route.legs.size()) + ": " +
                           std::to_string(graph.GetNode(leg.nodes[k - 1]).id) + " -> " +
                           std::to_string(graph.GetNode(leg.nodes[k]).id);
            }
        }
        return {};
    };
    // The first transport leg of the route without a ride, as "leg i/n: id id"; empty when none
    auto const ridelessLeg = [](TravelGraph const& of, TravelRoute::Result const& route) -> std::string
    {
        for (std::size_t i = 0; i < route.legs.size(); ++i)
        {
            TravelRoute::Leg const& leg = route.legs[i];
            if (leg.type != LegType::Transport || leg.object)
                continue;
            std::string nodes;
            for (std::uint32_t const node : leg.nodes)
                nodes += " " + std::to_string(of.GetNode(node).id);
            return "leg " + std::to_string(i + 1) + "/" + std::to_string(route.legs.size()) + ":" + nodes;
        }
        return {};
    };
    auto const check = [&](TravelRoute::Request const& request, std::string const& what)
    {
        ++checks;
        TravelRoute::Result const route = TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, {});
        std::string const walked = walkedWaitLink(route);
        std::string const rideless = ridelessLeg(graph, route);
        if (!walked.empty())
            failures.push_back("y) " + what + (request.freeRides ? " (freeRides)" : "") +
                               " walks a wait point's link, " + walked);
        else if (!rideless.empty())
            failures.push_back("y) " + what + (request.freeRides ? " (freeRides)" : "") +
                               " has a transport leg without a ride, " + rideless);
    };
    TravelRoute::Request base;
    base.snapshot.runSpeed = 7.0f;
    base.snapshot.team = 1;
    base.snapshot.level = 80;
    base.snapshot.money = 100000;
    base.snapshot.allowTransport = true;
    for (bool const freeRides : {false, true})
    {
        TravelRoute::Request lift = base;
        lift.freeRides = freeRides;
        lift.snapshot.position = {571, 423.0f, -6186.8f, 305.8f};
        lift.goal = {571, 418.7f, -6197.1f, 69.7f};
        check(lift, "190118 top to foot");
        TravelRoute::Result const first = TravelRoute::Search(graph, lift, TravelRoute::StuckEdges{}, {});
        for (TravelRoute::Leg const& leg : first.legs)
            if (leg.type == LegType::Transport && leg.rideAt + 1 < leg.nodes.size())
                TravelRoute::RefuseLink(lift, leg.nodes[leg.rideAt], leg.nodes[leg.rideAt + 1]);
        check(lift, "190118 top to foot, the ride failed");
        TravelRoute::RefuseWalk(lift, lift.snapshot.position, lift.goal);
        check(lift, "190118 top to foot, the direct walk refused");
        TravelRoute::RefuseWalk(lift, lift.snapshot.position, Point{571, 423.7f, -6182.1f, 53.4f});
        check(lift, "190118 top to foot, the walk to its node refused");
        for (std::uint32_t index = 0; index < graph.NodeCount(); ++index)
            for (TravelGraphLink const& link : graph.LinksOf(index))
            {
                if (link.type != TravelLinkType::Transport || TravelGraph::IsWaitPointLink(link) ||
                    graph.GetNode(link.to).mapId != graph.GetNode(index).mapId)
                    continue;
                for (TravelGraphLink const& near : graph.LinksOf(index))
                {
                    if (!TravelGraph::IsWaitPointLink(near))
                        continue;
                    TravelRoute::Request pair = base;
                    pair.freeRides = freeRides;
                    pair.snapshot.position = pointOf(near.to);
                    pair.goal = pointOf(link.to);
                    check(pair, "ride " + std::to_string(link.object) + " wait point " +
                                    std::to_string(graph.GetNode(near.to).id) + " to stop " +
                                    std::to_string(graph.GetNode(link.to).id));
                }
            }
        // Across every wait-point link, its far end the goal: also the links that join no stop (their ride is not in
        // the graph); one failure line for all of them
        std::uint32_t rideless = 0;
        std::string firstRideless;
        for (std::uint32_t index = 0; index < graph.NodeCount(); ++index)
            for (TravelGraphLink const& link : graph.LinksOf(index))
            {
                if (!TravelGraph::IsWaitPointLink(link))
                    continue;
                TravelRoute::Request across = base;
                across.freeRides = freeRides;
                across.snapshot.position = pointOf(index);
                across.goal = pointOf(link.to);
                TravelRoute::Result const route = TravelRoute::Search(graph, across, TravelRoute::StuckEdges{}, {});
                std::string const leg = ridelessLeg(graph, route);
                if (leg.empty())
                    continue;
                if (!rideless++)
                    firstRideless = std::to_string(graph.GetNode(index).id) + " to " +
                                    std::to_string(graph.GetNode(link.to).id) + ", " + leg;
            }
        ++checks;
        if (rideless)
            failures.push_back("y) across " + std::to_string(rideless) + " wait-point links" +
                               (freeRides ? " (freeRides)" : "") +
                               " the route has a transport leg without a ride, first " + firstRideless);
    }
    // One label per vertex holds while a stop has one wait point
    std::uint32_t manyWaits = 0;
    for (std::uint32_t index = 0; index < graph.NodeCount(); ++index)
    {
        std::vector<std::uint32_t> waits;
        for (TravelGraphLink const& link : graph.LinksOf(index))
            if (TravelGraph::IsWaitPointLink(link) && std::find(waits.begin(), waits.end(), link.to) == waits.end())
                waits.push_back(link.to);
        manyWaits += graph.GetNode(index).waitStop && waits.size() > 1 ? 1 : 0;
    }
    ++checks;
    if (manyWaits)
        failures.push_back("y) " + std::to_string(manyWaits) + " stops of the shipped graph have several wait points");

    // A hand-made lift on Northrend: land A - wait point W (top) - stop S (the car's pose, 100 yd down the shaft) =
    // lift = stop T - wait point V - land B, 500 yd on. Its ride failed (its link refused, a replan): a bot that may
    // fly flies from where it stands, not from S over the wait point's link (free under freeRides, and S is nearer B)
    enum : std::uint32_t
    {
        A,
        W,
        S,
        T,
        V,
        B
    };
    constexpr std::uint32_t LIFT = 190118;
    TravelGraph lift;
    {
        TravelGraphBuilder builder;
        builder.AddNode(A, "A", 571, 0.0f, 0.0f, 300.0f);
        builder.AddNode(W, "Wdock", 571, 60.0f, 0.0f, 300.0f);
        builder.AddNode(S, "S", 571, 80.0f, 0.0f, 200.0f);
        builder.AddNode(T, "T", 571, 80.0f, 0.0f, 0.0f);
        builder.AddNode(V, "Vdock", 571, 100.0f, 0.0f, 0.0f);
        builder.AddNode(B, "B", 571, 600.0f, 0.0f, 0.0f);
        for (std::uint32_t stop = 0; stop < 2; ++stop)
            for (std::uint32_t direction = 0; direction < 2; ++direction)
            {
                TravelGraphBuilder::CrossingRow row;
                row.entry = LIFT;
                row.stop = stop;
                row.mapId = 571;
                row.x = 80.0f;
                row.z = stop ? 0.0f : 200.0f;
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
                row.extraCost = type == 3 && object ? 20.0f : 0.0f;
                builder.AddLink(row);
            }
        };
        link(A, W, 1, 0, 60.0f);
        link(W, S, 3, 0, 100.0f);
        link(S, T, 3, LIFT, 0.1f);
        link(T, V, 3, 0, 20.0f);
        link(V, B, 1, 0, 500.0f);
        std::string error;
        builder.EndLinks(error);
        builder.AddPoint(A, W, 0, 571, 0.0f, 0.0f, 300.0f);
        builder.Finish(lift, error);
        lift.Publish(0, 0);
    }
    TravelRoute::Request flyer = base;
    flyer.freeRides = true;
    flyer.snapshot.flyNorthrend = true;
    flyer.snapshot.flySpeed = 20.0f;
    flyer.snapshot.position = {571, 59.0f, 0.0f, 300.0f};
    flyer.goal = {571, 601.0f, 0.0f, 0.0f};
    TravelRoute::Result const ridden = TravelRoute::Search(lift, flyer, TravelRoute::StuckEdges{}, {});
    ++checks;
    if (!ridden.ok || std::none_of(ridden.legs.begin(), ridden.legs.end(),
                                   [](TravelRoute::Leg const& leg) { return leg.object == LIFT; }))
        failures.push_back("y) the hand-made lift is not ridden");
    TravelRoute::RefuseLink(flyer, lift.IndexOfId(S), lift.IndexOfId(T));
    TravelRoute::Result const flown = TravelRoute::Search(lift, flyer, TravelRoute::StuckEdges{}, {});
    std::string const rideless = ridelessLeg(lift, flown);
    ++checks;
    if (!flown.ok || !rideless.empty())
        failures.push_back("y) the hand-made lift, its ride failed: " +
                           (flown.ok ? "a fly edge leaves the stop entered over its wait point's link, " + rideless
                                     : std::string("no route")));
    return checks;
}

// z) An end at a node (NodeEnds) on a hand-made map: a bot within NODE_REACH_YARDS of a node starts there, a goal so
//    close to a node ends there, both at no cost, though a priced walk to another node would be cheaper; an end beyond
//    reach (in 3D), at a refused node or with its walk to the node refused joins its END_NODES nearest as before; the
//    direct walk stays for ends at no node and is no way between two ends at nodes; a bot at a node with no way out
//    is routed from its END_NODES nearest. Each mutant breaks one rule
struct StartIgnoresReach : TravelRoute::NodeEnds
{
    static bool AtNode(TravelRoute::Point const& end, TravelRoute::Point const& node, bool start)
    {
        return !start && NodeEnds::AtNode(end, node, start);
    }
};
struct GoalIgnoresReach : TravelRoute::NodeEnds
{
    static bool AtNode(TravelRoute::Point const& end, TravelRoute::Point const& node, bool start)
    {
        return start && NodeEnds::AtNode(end, node, start);
    }
};
struct PricedEnds : TravelRoute::NodeEnds
{
    static float Seconds(bool /*atNode*/, float walkSeconds) { return walkSeconds; }
};
struct FlatReach : TravelRoute::NodeEnds
{
    static bool AtNode(TravelRoute::Point const& end, TravelRoute::Point const& node, bool /*start*/)
    {
        return std::hypot(end.x - node.x, end.y - node.y) <= TravelRoute::NODE_REACH_YARDS;
    }
};
struct NoRetry : TravelRoute::NodeEnds
{
    static bool Retries() { return false; }
};
struct ReachExclusive : TravelRoute::NodeEnds
{
    static bool AtNode(TravelRoute::Point const& end, TravelRoute::Point const& node, bool /*start*/)
    {
        return TravelRoute::Distance(end, node) < TravelRoute::NODE_REACH_YARDS;
    }
};

enum : std::uint32_t
{
    END_S,  // the start region: S and, farther from the bots, F with the cheaper link
    END_F,
    END_G,  // the goal region: G and, farther from the goals, H with the cheaper link
    END_H,
    END_P,  // two nodes 40 yd apart, joined by a 60 yd link
    END_Q,
    END_V,  // a node 12 yd above the bot, and X 13 yd off on the ground with the cheaper link to Y
    END_X,
    END_Y,
    END_T,  // a node 3 yd from the bot that no link leaves, and N 12 yd off with a link to Z
    END_N,
    END_Z
};

std::string BuildNodeEnds(TravelGraph& graph)
{
    TravelGraphBuilder builder;
    builder.AddNode(END_S, "S", 0, 0.0f, 0.0f, 0.0f);
    builder.AddNode(END_F, "F", 0, 0.0f, 20.0f, 0.0f);
    builder.AddNode(END_G, "G", 0, 500.0f, 0.0f, 0.0f);
    builder.AddNode(END_H, "H", 0, 500.0f, 19.0f, 0.0f);
    builder.AddNode(END_P, "P", 0, 1000.0f, 0.0f, 0.0f);
    builder.AddNode(END_Q, "Q", 0, 1000.0f, 40.0f, 0.0f);
    builder.AddNode(END_V, "V", 0, 2000.0f, 0.0f, 12.0f);
    builder.AddNode(END_X, "X", 0, 2000.0f, 13.0f, 0.0f);
    builder.AddNode(END_Y, "Y", 0, 2500.0f, 0.0f, 0.0f);
    builder.AddNode(END_T, "T", 0, 3000.0f, 0.0f, 0.0f);
    builder.AddNode(END_N, "N", 0, 3000.0f, 15.0f, 0.0f);
    builder.AddNode(END_Z, "Z", 0, 3500.0f, 0.0f, 0.0f);
    auto const link = [&builder](std::uint32_t from, std::uint32_t to, float distance, bool back = true)
    {
        for (auto const& [a, b] : {std::pair{from, to}, std::pair{to, from}})
        {
            if (a == to && !back)
                continue;
            TravelGraphBuilder::LinkRow row;
            row.fromId = a;
            row.toId = b;
            row.type = 1;
            row.distance = distance;
            builder.AddLink(row);
        }
    };
    link(END_S, END_G, 1000.0f);
    link(END_F, END_G, 400.0f);  // shorter than the straight walk to G, the bots' other way
    link(END_F, END_H, 300.0f);
    link(END_S, END_H, 2000.0f);
    link(END_P, END_Q, 60.0f);
    link(END_V, END_Y, 1000.0f);
    link(END_X, END_Y, 300.0f);
    link(END_N, END_T, 15.0f, false);
    link(END_N, END_Z, 400.0f);  // shorter than the straight walk to Z
    std::string error;
    builder.EndLinks(error);
    builder.AddPoint(END_S, END_G, 0, 0, 0.0f, 0.0f, 0.0f);
    builder.Finish(graph, error);
    graph.Publish(0, 0);
    return error;
}

struct EndCases
{
    std::uint32_t cases{0};
    std::vector<std::string> failed;
};

template <class Ends>
EndCases NodeEndCases(TravelGraph const& graph)
{
    EndCases out;
    auto const expect = [&out](bool ok, std::string const& what)
    {
        ++out.cases;
        if (!ok)
            out.failed.push_back(what);
    };
    constexpr float RUN = 7.0f;  // Snapshot::runSpeed's default
    auto const search = [&graph](TravelRoute::Point const& from, TravelRoute::Point const& to,
                                 std::vector<FailedAttempts::Entry> const& refused = {})
    {
        TravelRoute::Request request;
        request.snapshot.position = from;
        request.goal = to;
        request.refused = refused;
        return TravelRoute::GraphSearch<Ends>(graph, request, TravelRoute::StuckEdges{}, TravelRoute::Params{},
                                              TravelRoute::FlyClosures{});
    };
    auto const first = [&graph](TravelRoute::Result const& route)
    {
        return route.ok && !route.legs.empty() && !route.legs.front().nodes.empty()
                   ? graph.GetNode(route.legs.front().nodes.front()).id
                   : TravelRoute::NO_LINK;
    };
    auto const last = [&graph](TravelRoute::Result const& route)
    {
        return route.ok && !route.legs.empty() && !route.legs.back().nodes.empty()
                   ? graph.GetNode(route.legs.back().nodes.back()).id
                   : TravelRoute::NO_LINK;
    };
    auto const near = [](float a, float b) { return std::fabs(a - b) < 0.01f; };

    TravelRoute::Point const nearS{0, 0.0f, 8.0f, 0.0f};  // S 8 yd, F 12 yd
    TravelRoute::Point const atG{0, 500.0f, 0.0f, 0.0f};
    TravelRoute::Result const started = search(nearS, atG);
    expect(first(started) == END_S && near(started.totalSeconds, 1000.0f / RUN),
           "start: a bot 8 yd from S starts at S, its route the links' seconds (via F with priced walks is cheaper)");
    expect(started.legs.size() == 1 && started.legs.front().nodes.size() == 2 && started.legs.front().from.y == 8.0f &&
               started.legs.front().to.x == 500.0f,
           "start: one walk leg from the bot's spot through S and G to the goal");
    TravelRoute::Result const boundary = search({0, 0.0f, -10.0f, 0.0f}, atG);
    expect(first(boundary) == END_S && near(boundary.totalSeconds, 1000.0f / RUN),
           "start: a bot exactly NODE_REACH_YARDS from S starts at S");
    TravelRoute::Result const beyond = search({0, 0.0f, -11.0f, 0.0f}, atG);
    expect(first(beyond) == END_F && near(beyond.totalSeconds, (31.0f + 400.0f) / RUN),
           "start: a bot 11 yd from S walks to the cheaper of its END_NODES nearest (F)");
    TravelRoute::Result const above = search({0, 2000.0f, 0.0f, 0.0f}, {0, 2500.0f, 0.0f, 0.0f});
    expect(first(above) == END_X && near(above.totalSeconds, (13.0f + 300.0f) / RUN),
           "start: a node 12 yd overhead is beyond reach (3D): the bot walks to X");

    TravelRoute::Point const atF{0, 0.0f, 20.0f, 0.0f};
    TravelRoute::Result const ended = search(atF, {0, 500.0f, 4.0f, 0.0f});  // G 4 yd, H 15 yd
    expect(last(ended) == END_G && near(ended.totalSeconds, 400.0f / RUN),
           "goal: a goal 4 yd from G ends at G at no cost (via H with a priced walk is cheaper)");
    expect(ended.ok && ended.legs.back().to.y == 4.0f, "goal: the last leg ends at the goal");
    TravelRoute::Result const off = search(atF, {0, 500.0f, 31.0f, 0.0f});  // H 12 yd, G 31 yd
    expect(last(off) == END_H && near(off.totalSeconds, (300.0f + 12.0f) / RUN),
           "goal: a goal 12 yd from H ends by a priced walk from the cheaper of its END_NODES nearest");

    FailedAttempts::Entry place;
    place.key.kind = FailedAttempts::Kind::Place;
    place.key.from = {0, 0.0f, 0.0f, 0.0f};
    place.cause = "no_route";
    TravelRoute::Result const refusedNode = search(nearS, atG, {place});
    expect(first(refusedNode) == END_F && near(refusedNode.totalSeconds, (12.0f + 400.0f) / RUN),
           "a refused node within reach is no end: the bot walks to F");
    FailedAttempts::Entry walk;
    walk.key.kind = FailedAttempts::Kind::Walk;
    walk.key.from = {0, 0.0f, 8.0f, 0.0f};
    walk.key.to = {0, 0.0f, 0.0f, 0.0f};
    walk.cause = "stuck";
    TravelRoute::Result const refusedWalk = search(nearS, atG, {walk});
    expect(first(refusedWalk) == END_F, "a refused walk to the node within reach: the bot walks to F");

    TravelRoute::Result const direct = search({0, 250.0f, 200.0f, 0.0f}, {0, 290.0f, 200.0f, 0.0f});
    expect(direct.ok && direct.legs.size() == 1 && direct.legs.front().nodes.empty() &&
               near(direct.totalSeconds, 40.0f / RUN),
           "ends at no node 40 yd apart: the direct walk");
    TravelRoute::Result const linked = search({0, 1000.0f, 0.0f, 0.0f}, {0, 1000.0f, 40.0f, 0.0f});
    expect(first(linked) == END_P && last(linked) == END_Q && near(linked.totalSeconds, 60.0f / RUN),
           "both ends at nodes 40 yd apart: the 60 yd link, no direct walk");
    TravelRoute::Result const trapped = search({0, 3000.0f, 3.0f, 0.0f}, {0, 3500.0f, 0.0f, 0.0f});
    expect(first(trapped) == END_N && near(trapped.totalSeconds, (12.0f + 400.0f) / RUN),
           "a bot at a node no link leaves: routed from its END_NODES nearest (N)");
    return out;
}

std::uint32_t NodeEndSelftest(std::vector<std::string>& failures)
{
    TravelGraph graph;
    std::string const error = BuildNodeEnds(graph);
    if (!error.empty())
    {
        failures.push_back("z) the hand-made map: " + error);
        return 1;
    }
    EndCases const real = NodeEndCases<TravelRoute::NodeEnds>(graph);
    for (std::string const& what : real.failed)
        failures.push_back("z) " + what);
    std::uint32_t checks = real.cases;
    auto const killed = [&](EndCases const& mutant, std::string const& name)
    {
        ++checks;
        if (mutant.failed.empty())
            failures.push_back("z) mutant " + name + " survived");
    };
    killed(NodeEndCases<StartIgnoresReach>(graph), "start ignores reach");
    killed(NodeEndCases<GoalIgnoresReach>(graph), "goal ignores reach");
    killed(NodeEndCases<PricedEnds>(graph), "end walk priced");
    killed(NodeEndCases<FlatReach>(graph), "reach in 2D");
    killed(NodeEndCases<ReachExclusive>(graph), "reach exclusive");
    killed(NodeEndCases<NoRetry>(graph), "no retry by the END_NODES nearest");
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
        NoWalkReplanSelftest(graph, root, failures, notes) + WaitPointSelftest(failures) + AirSelftest(failures) +
        FreeRideSelftest(failures) + RideEndsSelftest(graph, failures, notes) + WaitLinkWalkSelftest(graph, failures) +
        NodeEndSelftest(failures);
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
