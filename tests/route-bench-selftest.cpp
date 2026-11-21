/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline cost of the route search (pathing PA10: the graph's size against the search's time). Loads a travel graph
// (the shipped one, or a graph directory's SQL files) and times TravelRoute::Search, the call the route service makes
// for a bot, over a request file (`map x y z map x y z team` per line, `#` lines skipped) or, without one, over
// requests drawn with a fixed seed between the graph's own nodes of one map. The first requests warm up and are not
// timed. Prints the graph's size and memory (TravelGraph::Stats), the load time and the microseconds per search
// (mean, p50, p95, max). The same binary and request file on two graphs give the factor of the bigger graph.
// Usage: route-bench-selftest [module root] [dbc dir] [graph dir|-] [request file|-] [timed requests] [warm-up]
// (defaults: the current directory, C:/Games/AzerothCore/Server/data/dbc, the shipped graph, drawn requests, 400, 50)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "RouteQuery.h"
#include "TravelGraphSql.h"

namespace
{
std::vector<RouteQuery::Parsed> ReadRequests(std::filesystem::path const& file)
{
    std::vector<RouteQuery::Parsed> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream fields(line);
        RouteQuery::Parsed request;
        unsigned team = 1;
        if (fields >> request.from.mapId >> request.from.x >> request.from.y >> request.from.z >> request.to.mapId >>
            request.to.x >> request.to.y >> request.to.z >> team)
        {
            request.team = static_cast<std::uint8_t>(team ? 1 : 0);
            out.push_back(request);
        }
    }
    return out;
}

// Pairs of the graph's nodes on one map, both ends drawn uniformly (seed fixed: the same graph gives the same list)
std::vector<RouteQuery::Parsed> DrawRequests(TravelGraph const& graph, std::size_t count)
{
    std::vector<RouteQuery::Parsed> out;
    if (graph.NodeCount() < 2)
        return out;
    std::mt19937 random(20261122u);
    std::uniform_int_distribution<std::size_t> pick(0, graph.NodeCount() - 1);
    for (std::size_t tries = 0; out.size() < count && tries < count * 100; ++tries)
    {
        TravelGraphNode const& a = graph.GetNode(static_cast<std::uint32_t>(pick(random)));
        TravelGraphNode const& b = graph.GetNode(static_cast<std::uint32_t>(pick(random)));
        if (a.mapId != b.mapId || (a.x == b.x && a.y == b.y))
            continue;
        RouteQuery::Parsed request;
        request.from = {a.mapId, a.x, a.y, a.z};
        request.to = {b.mapId, b.x, b.y, b.z};
        request.team = static_cast<std::uint8_t>(random() & 1u);
        out.push_back(request);
    }
    return out;
}

double Percentile(std::vector<double> const& sorted, double share)
{
    if (sorted.empty())
        return 0.0;
    std::size_t const at = std::min(sorted.size() - 1, static_cast<std::size_t>(share * (sorted.size() - 1) + 0.5));
    return sorted[at];
}
}  // namespace

int main(int argc, char** argv)
{
    auto const arg = [&](int i, char const* fallback) -> std::string
    { return argc > i && std::string(argv[i]) != "-" ? argv[i] : fallback; };
    std::filesystem::path const root = arg(1, ".");
    std::filesystem::path const dbcDir = arg(2, "C:/Games/AzerothCore/Server/data/dbc");
    std::filesystem::path const graphDir = arg(3, "");
    std::filesystem::path const requestFile = arg(4, "");
    std::size_t const timed = static_cast<std::size_t>(std::strtoul(arg(5, "400").c_str(), nullptr, 10));
    std::size_t const warmUp = static_cast<std::size_t>(std::strtoul(arg(6, "50").c_str(), nullptr, 10));

    TravelGraph graph;
    TravelGraphSql::LoadInfo info;
    std::string error;
    if (!TravelGraphSql::Load(root, dbcDir, graph, info, error, graphDir))
    {
        std::cout << "route-bench-selftest: graph build failed: " << error << "\n";
        return 2;
    }
    std::vector<RouteQuery::Parsed> const requests =
        requestFile.empty() ? DrawRequests(graph, warmUp + timed) : ReadRequests(requestFile);
    if (requests.size() < warmUp + timed)
    {
        std::cout << "route-bench-selftest: " << requests.size() << " requests, " << warmUp + timed << " needed\n";
        return 2;
    }

    std::vector<double> micros;
    micros.reserve(timed);
    std::size_t found = 0;
    std::uint64_t expanded = 0;
    for (std::size_t i = 0; i < warmUp + timed; ++i)
    {
        TravelRoute::Request const request = RouteQuery::RequestOf(requests[i]);
        auto const start = std::chrono::steady_clock::now();
        TravelRoute::Result const route = TravelRoute::Search(graph, request, {}, {});
        auto const end = std::chrono::steady_clock::now();
        if (i < warmUp)
            continue;
        micros.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        found += route.ok ? 1 : 0;
        expanded += route.expandedNodes;
    }
    double total = 0.0;
    for (double const us : micros)
        total += us;
    std::vector<double> sorted = micros;
    std::sort(sorted.begin(), sorted.end());

    TravelGraphStats const& stats = graph.Stats();
    std::cout.setf(std::ios::fixed);
    std::cout.precision(1);
    std::cout << "route-bench: graph " << (graphDir.empty() ? std::string("shipped") : graphDir.string()) << ", nodes "
              << stats.nodes << ", links " << stats.links << ", points " << stats.points << ", memory "
              << stats.memoryBytes << " bytes (" << static_cast<double>(stats.memoryBytes) / (1024.0 * 1024.0)
              << " MB), load " << info.loadMs << " ms\n";
    std::cout << "route-bench: requests "
              << (requestFile.empty() ? std::string("drawn (seed 20261122)") : requestFile.string()) << ", warm-up "
              << warmUp << ", expanded nodes mean "
              << (micros.empty() ? 0.0 : static_cast<double>(expanded) / static_cast<double>(micros.size())) << "\n";
    std::cout << "route-bench: " << micros.size() << " searches (ok " << found << "), us mean "
              << (micros.empty() ? 0.0 : total / static_cast<double>(micros.size())) << " p50 "
              << Percentile(sorted, 0.50) << " p95 " << Percentile(sorted, 0.95) << " max "
              << (sorted.empty() ? 0.0 : sorted.back()) << "\n";
    return micros.size() == timed && found > 0 ? 0 : 1;
}
