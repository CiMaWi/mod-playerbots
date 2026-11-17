/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the travel graph (src/Mgr/Travel/TravelGraph.h) on the shipped data: parses the INSERT rows of
// data/sql/playerbots/base/playerbots_travelnode*.sql and of the updates under data/sql/playerbots/updates/ (or of a
// graph directory's files alone) into the same TravelGraphBuilder the server's loader feeds, then runs
// TravelGraph::Selftest (the counts, the per-node link ranges, the nearest-node grid and three routes) - the checks
// `playerbots route selftest` runs on the loaded graph.
// Usage: travel-graph-selftest [module root] [graph dir] (defaults: the current directory, the shipped graph)
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "RouteRules.h"
#include "TravelGraph.h"
#include "TravelGraphSelftest.h"
#include "TravelGraphSql.h"

namespace
{
// The route search from one node to another, as a bot with transports allowed (the graph's own positions)
TravelRoute::Result RouteBetween(TravelGraph const& graph, std::uint32_t from, std::uint32_t to)
{
    TravelGraphNode const& a = graph.GetNode(from);
    TravelGraphNode const& b = graph.GetNode(to);
    TravelRoute::Request request;
    request.snapshot.position = {a.mapId, a.x, a.y, a.z};
    request.snapshot.level = 80;
    request.snapshot.allowTransport = true;
    request.goal = {b.mapId, b.x, b.y, b.z};
    return TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, {});
}

TravelGraphBuilder::LinkRow Walk(std::uint32_t fromId, std::uint32_t toId, std::uint32_t type = 1)
{
    TravelGraphBuilder::LinkRow row;
    row.fromId = fromId;
    row.toId = toId;
    row.type = type;
    row.distance = 70.0f;
    return row;
}

// The builder's rules on a hand-made graph: bad links skipped, orphan points counted, the path length summed per map,
// out-of-order points and a duplicate node id refused; wait points and static portals
std::uint32_t BuilderChecks(std::vector<std::string>& failures)
{
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, char const* what)
    {
        ++checks;
        if (!ok)
            failures.push_back(std::string("builder: ") + what);
    };

    std::string error;
    TravelGraphBuilder builder;
    builder.AddNode(20, "c", 0, 140.0f, 0.0f, 0.0f);
    builder.AddNode(10, "a", 0, 0.0f, 0.0f, 0.0f);
    builder.AddNode(15, "b", 0, 70.0f, 0.0f, 0.0f);
    builder.AddLink(Walk(10, 15));
    builder.AddLink(Walk(15, 20));
    builder.AddLink(Walk(10, 10));     // to itself
    builder.AddLink(Walk(10, 99));     // unknown node
    builder.AddLink(Walk(15, 10, 7));  // unknown type
    builder.AddLink(Walk(15, 10, 5));  // unknown type
    check(builder.EndLinks(error), "a small graph ends its links");
    builder.AddPoint(10, 15, 0, 0, 0.0f, 0.0f, 0.0f);
    builder.AddPoint(10, 15, 1, 0, 3.0f, 4.0f, 0.0f);   // 5 yd
    builder.AddPoint(10, 15, 2, 1, 50.0f, 0.0f, 0.0f);  // another map: no length
    builder.AddPoint(10, 15, 3, 1, 50.0f, 0.0f, 2.0f);  // 2 yd
    builder.AddPoint(30, 40, 0, 0, 0.0f, 0.0f, 0.0f);   // no such link
    TravelGraph graph;
    check(builder.Finish(graph, error), "a small graph finishes");
    graph.Publish(0, 0);
    TravelGraphStats const& stats = graph.Stats();
    check(stats.links == 2 && stats.skippedLinks == 4 && stats.orphanPoints == 1, "bad links skipped, orphans counted");
    check(graph.IndexOfId(10) == 0 && graph.IndexOfId(20) == 2 && graph.IndexOfId(11) == TravelGraph::NO_NODE,
          "nodes ordered by id");
    TravelGraphLink const& link = graph.LinksOf(0)[0];
    check(link.pathCount == 4 && std::fabs(link.pathLength - 7.0f) < 0.001f, "path length summed on one map");
    TravelRoute::Result const route = RouteBetween(graph, 0, 2);
    check(route.ok && std::fabs(route.totalSeconds - 20.0f) < 0.5f, "a two-link route");
    TravelRoute::Result const back = RouteBetween(graph, 2, 0);
    check(!back.ok || (back.legs.size() == 1 && back.legs[0].type == TravelRoute::LegType::Walk),
          "no route against the links (only the straight walk off the graph)");

    TravelGraphBuilder disordered;
    disordered.AddNode(1, "a", 0, 0.0f, 0.0f, 0.0f);
    disordered.AddNode(2, "b", 0, 0.0f, 0.0f, 0.0f);
    disordered.AddLink(Walk(1, 2));
    disordered.AddLink(Walk(2, 1));
    disordered.EndLinks(error);
    disordered.AddPoint(1, 2, 0, 0, 0.0f, 0.0f, 0.0f);
    disordered.AddPoint(2, 1, 0, 0, 0.0f, 0.0f, 0.0f);
    disordered.AddPoint(1, 2, 1, 0, 0.0f, 0.0f, 0.0f);  // the link's points come in two groups
    TravelGraph refused;
    check(!disordered.Finish(refused, error), "points out of order refused");

    TravelGraphBuilder twice;
    twice.AddNode(1, "a", 0, 0.0f, 0.0f, 0.0f);
    twice.AddNode(1, "b", 0, 0.0f, 0.0f, 0.0f);
    twice.AddLink(Walk(1, 1));
    check(!twice.EndLinks(error), "a node id twice refused");

    // Land 1 - wait point 2 - stop 3 = ride = stop 4 - wait point 5 - land 6, a walk 1 - 3 onto the stop, a static
    // portal 1 - 6 and a dangling wait-point link 6 - 7 (7 has no ride). Node id 0 is a node like any other
    TravelGraphBuilder waits;
    // Places for the route search, which prices a walk off the links by its yards: the far land (4, 5) lies 20000 yd
    // east, the portal's landing (6, 7) 50000 yd north, so only the crossing and the portal reach them quickly
    for (std::uint32_t id = 0; id <= 7; ++id)
    {
        float const x = 100.0f * static_cast<float>(id) + (id == 4 || id == 5 ? 20000.0f : 0.0f);
        float const y = id >= 6 ? 50000.0f : 0.0f;
        waits.AddNode(id, "n", 0, x, y, 0.0f);
    }
    auto const row = [](std::uint32_t from, std::uint32_t to, std::uint32_t type, std::uint32_t object)
    {
        TravelGraphBuilder::LinkRow made = Walk(from, to, type);
        made.object = object;
        made.distance = type == 1 || !object ? 30.0f : 0.1f;
        made.extraCost = type == 6 ? 3.0f : 0.0f;          // a static portal's seconds
        made.periodMs = type == 3 && object ? 240000 : 0;  // a ship's period (the loader reads the core's)
        return made;
    };
    for (auto const& [from, to] : {std::pair{0u, 1u}, std::pair{1u, 2u}, std::pair{1u, 3u}, std::pair{5u, 6u}})
        waits.AddLink(row(from, to, 1, 0));
    waits.AddLink(row(2, 3, 3, 0));
    waits.AddLink(row(3, 4, 3, 164871));
    waits.AddLink(row(4, 5, 3, 0));
    waits.AddLink(row(6, 7, 3, 0));
    waits.AddLink(row(1, 6, 6, 193604));
    check(waits.EndLinks(error), "a graph with wait points ends its links");
    waits.AddPoint(0, 1, 0, 0, 0.0f, 0.0f, 0.0f);
    TravelGraph waited;
    check(waits.Finish(waited, error), "a graph with wait points finishes");
    waited.Publish(0, 0);
    TravelGraphStats const& waitStats = waited.Stats();
    check(waitStats.links == 9 && !waitStats.skippedLinks && waitStats.linksByType[6] == 1 &&
              waitStats.waitPointLinks == 3 && waitStats.waitStops == 2,
          "static portals and wait-point links load, the two stops are marked");
    check(waited.GetNode(waited.IndexOfId(3)).waitStop && waited.GetNode(waited.IndexOfId(4)).waitStop &&
              !waited.GetNode(waited.IndexOfId(2)).waitStop && !waited.GetNode(waited.IndexOfId(7)).waitStop,
          "a stop is a wait-point link's end with a ride");
    auto const passedIds = [&waited](TravelRoute::Result const& route)
    {
        std::vector<std::uint32_t> passed;
        for (TravelRoute::Leg const& leg : route.legs)
            for (std::uint32_t const node : leg.nodes)
                if (passed.empty() || passed.back() != waited.GetNode(node).id)
                    passed.push_back(waited.GetNode(node).id);
        return passed;
    };
    TravelRoute::Result const crossing = RouteBetween(waited, waited.IndexOfId(0), waited.IndexOfId(5));
    check(crossing.ok && passedIds(crossing) == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5},
          "the route crosses through both wait points, not by the walk onto the stop");
    TravelRoute::Result const portal = RouteBetween(waited, waited.IndexOfId(0), waited.IndexOfId(6));
    check(portal.ok && passedIds(portal) == std::vector<std::uint32_t>{0, 1, 6},
          "the route takes the static portal (WT55d)");
    auto const arrivals = waited.ArrivalsOnto(0);
    check(arrivals.size() == 2 && arrivals[0].node == waited.IndexOfId(4) && arrivals[1].node == waited.IndexOfId(6) &&
              arrivals[1].fixedSeconds == 3.0f,
          "the ride and the static portal arrive (its seconds fixed): no arrival by a wait-point link on one map");
    return checks;
}
}  // namespace

int main(int argc, char** argv)
{
    std::filesystem::path const root = argc > 1 ? argv[1] : ".";
    std::filesystem::path const graphDir = argc > 2 ? argv[2] : "";
    TravelGraph graph;
    TravelGraphSql::LoadInfo info;
    std::string error;
    if (!TravelGraphSql::Load(root, {}, graph, info, error, graphDir))
    {
        std::cout << "travel-graph-selftest: build failed: " << error << "\n";
        return error.rfind("no shipped graph", 0) == 0 ? 2 : 1;
    }
    std::size_t const* rows = info.rows;
    std::uint32_t const loadMs = info.loadMs;

    TravelGraphStats const& stats = graph.Stats();
    std::cout << "rows read: " << rows[0] << " nodes, " << rows[1] << " links, " << rows[2] << " points\n";
    std::cout << "graph: " << stats.nodes << " nodes, " << stats.links << " links, " << stats.points << " points, "
              << static_cast<double>(stats.memoryBytes) / (1024.0 * 1024.0) << " MB, parsed in " << loadMs << " ms\n";

    std::vector<std::string> failures;
    std::vector<std::string> notes;
    auto const routeStart = std::chrono::steady_clock::now();
    std::uint32_t const checks = graph.Selftest(failures, notes) + BuilderChecks(failures);
    auto const selftestMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - routeStart).count();
    for (std::string const& note : notes)
        std::cout << "  " << note << "\n";
    for (std::string const& failure : failures)
        std::cout << "FAIL: " << failure << "\n";
    std::cout << "travel-graph-selftest: " << (checks - failures.size()) << "/" << checks << " passed (checks in "
              << selftestMs << " ms)\n";
    return failures.empty() ? 0 : 1;
}
