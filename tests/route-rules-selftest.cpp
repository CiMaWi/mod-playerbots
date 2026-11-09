/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the route search (src/Ai/World/Rpg/RouteRules.h) on the shipped travel graph and the 3.3.5
// TaxiPath/TaxiNodes DBC files: the fixed snapshots a) - f) and the fare budget h) of TravelRoute::Selftest and the
// worker test g) of TravelRoute::ServiceSelftest (RouteService.h) and the named places p) of TravelPlaces::Selftest
// (TravelPlaces.h), the checks `playerbots route selftest` also runs on the loaded graph; and i) a trip's replans after
// stuck walks (TravelLegRules.h) on that graph.
// Usage: route-rules-selftest [module root] [dbc dir] (defaults: the current directory,
// C:/Games/AzerothCore/Server/data/dbc)
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
}  // namespace

int main(int argc, char** argv)
{
    std::filesystem::path const root = argc > 1 ? argv[1] : ".";
    std::filesystem::path const dbcDir = argc > 2 ? argv[2] : "C:/Games/AzerothCore/Server/data/dbc";

    TravelGraph graph;
    TravelGraphSql::LoadInfo info;
    std::string error;
    if (!TravelGraphSql::Load(root, dbcDir, graph, info, error))
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
        TravelPlaces::Selftest(graph, failures, notes) + StuckReplanSelftest(graph, failures, notes);
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
