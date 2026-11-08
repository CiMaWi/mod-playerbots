/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the route search (src/Ai/World/Rpg/RouteRules.h) on the shipped travel graph and the 3.3.5
// TaxiPath/TaxiNodes DBC files: the fixed snapshots a) - f) of TravelRoute::Selftest, the checks `playerbots route
// selftest` also runs on the loaded graph.
// Usage: route-rules-selftest [module root] [dbc dir] (defaults: the current directory,
// C:/Games/AzerothCore/Server/data/dbc)
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "RouteRules.h"
#include "TravelGraphSql.h"

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
    std::uint32_t const checks = TravelRoute::Selftest(graph, failures, notes);
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
