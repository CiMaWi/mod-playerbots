/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the route search (src/Ai/World/Rpg/RouteRules.h) on the shipped travel graph and the 3.3.5
// TaxiPath/TaxiNodes DBC files: the fixed snapshots a) - f) and the fare budget h) of TravelRoute::Selftest and the
// worker test g) of TravelRoute::ServiceSelftest (RouteService.h) and the named places p) of TravelPlaces::Selftest
// (TravelPlaces.h), the checks `playerbots route selftest` also runs on the loaded graph; i) a trip's replans after
// stuck walks (TravelLegRules.h) on that graph; and w) a crossing through wait points on a hand-made graph.
// Usage: route-rules-selftest [module root] [dbc dir] [graph dir] (defaults: the current directory,
// C:/Games/AzerothCore/Server/data/dbc, the shipped graph)
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "RouteRules.h"
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
    auto const build = [](TravelGraph& graph, bool waitPoints)
    {
        TravelGraphBuilder builder;
        builder.AddNode(A, "A", 0, 0.0f, 0.0f, 0.0f);
        builder.AddNode(W1, "W1dock", 0, 60.0f, 0.0f, 0.0f);
        builder.AddNode(S1, "S1", 0, 80.0f, 0.0f, 0.0f);
        builder.AddNode(S2, "S2", 1, 0.0f, 0.0f, 0.0f);
        builder.AddNode(W2, "W2dock", 1, 20.0f, 0.0f, 0.0f);
        builder.AddNode(B, "B", 1, 80.0f, 0.0f, 0.0f);
        builder.AddNode(W3, "W3dock", 0, 90.0f, 0.0f, 200.0f);
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
                builder.AddLink(row);
            }
        };
        link(A, W1, 1, 0, 60.0f);
        link(A, S1, 1, 0, 80.0f);
        link(S2, B, 1, 0, 80.0f);
        link(W2, B, 1, 0, 60.0f);
        link(S1, S2, 3, SHIP, 0.1f);
        if (waitPoints)
        {
            link(W1, S1, 3, 0, 20.0f);
            link(S2, W2, 3, 0, 20.0f);
            link(S1, W3, 3, 0, 30.0f);
        }
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
    build(waits, true);
    TravelRoute::Result const crossed = TravelRoute::Search(waits, request, TravelRoute::StuckEdges{}, {});
    auto const ride = std::find_if(crossed.legs.begin(), crossed.legs.end(), [](TravelRoute::Leg const& leg)
                                   { return leg.type == TravelRoute::LegType::Transport; });
    if (check(crossed.ok && transports(crossed) == 1 && ride != crossed.legs.end(), "no single transport leg"))
    {
        check(ids(waits, *ride) == std::vector<std::uint32_t>{W1, S1, S2, W2} && ride->object == SHIP,
              "the transport leg runs W1, S1, S2, W2 with the ship");
        check(ride->from.x == 60.0f && ride->to.mapId == 1 && ride->to.x == 20.0f,
              "the transport leg runs from wait point to wait point");
        check(crossed.legs.size() == 3 && crossed.legs.front().type == TravelRoute::LegType::Walk &&
                  crossed.legs.back().type == TravelRoute::LegType::Walk,
              "a walk to the wait point, the crossing, a walk on");
        float const walks = (20.0f + 20.0f) / request.snapshot.runSpeed;
        check(std::fabs(ride->seconds -
                        (walks + TravelRoute::Params{}.transportWaitSeconds + TravelRoute::SHIP_RIDE_SECONDS)) < 0.01f,
              "the wait points' links cost their walk, the ride its wait and crossing");

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
    build(plain, false);
    TravelRoute::Result const old = TravelRoute::Search(plain, request, TravelRoute::StuckEdges{}, {});
    auto const oldRide = std::find_if(old.legs.begin(), old.legs.end(), [](TravelRoute::Leg const& leg)
                                      { return leg.type == TravelRoute::LegType::Transport; });
    check(old.ok && transports(old) == 1 && oldRide != old.legs.end() &&
              ids(plain, *oldRide) == std::vector<std::uint32_t>{S1, S2} && oldRide->from.x == 80.0f,
          "without wait points the ride runs S1, S2 from the stop");

    TravelRoute::Request up = request;
    up.goal = {0, 91.0f, 0.0f, 200.0f};
    TravelRoute::Result const climbed = TravelRoute::Search(waits, up, TravelRoute::StuckEdges{}, {});
    check(climbed.ok && climbed.legs.size() == 1 && climbed.legs.front().type == TravelRoute::LegType::Walk &&
              ids(waits, climbed.legs.front()) == std::vector<std::uint32_t>{W1, S1, W3},
          "wait-point links without a ride are one walk leg");
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
    std::uint32_t const checks = TravelRoute::Selftest(graph, failures, notes) +
                                 TravelRoute::ServiceSelftest(graph, failures, notes) +
                                 TravelPlaces::Selftest(graph, failures, notes) +
                                 StuckReplanSelftest(graph, failures, notes) + WaitPointSelftest(failures);
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
