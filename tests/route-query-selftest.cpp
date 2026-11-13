/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of `playerbots route query` (src/Mgr/Travel/RouteQuery.h): the argument parsing and, on the
// shipped travel graph, the legs of Orgrimmar -> Undercity (The Thundercaller, entry 164871).
// Usage: route-query-selftest [module root] [dbc dir] (defaults: the current directory,
// C:/Games/AzerothCore/Server/data/dbc)
#include <iostream>
#include <string>
#include <vector>

#include "RouteQuery.h"
#include "TravelGraphSql.h"

int main(int argc, char** argv)
{
    std::filesystem::path const root = argc > 1 ? argv[1] : ".";
    std::filesystem::path const dbcDir = argc > 2 ? argv[2] : "C:/Games/AzerothCore/Server/data/dbc";
    int failures = 0;
    int checks = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
        {
            ++failures;
            std::cout << "FAIL: " << what << "\n";
        }
    };

    RouteQuery::Parsed parsed;
    std::string error;
    check(RouteQuery::Parse("orgrimmar undercity", parsed, error) && parsed.team == 1 && parsed.level == 80 &&
              !parsed.sideGiven,
          "default side/level");
    check(RouteQuery::Parse("stormwind  darnassus alliance 60", parsed, error) && parsed.team == 0 &&
              parsed.level == 60 && parsed.sideGiven,
          "side and level");
    check(RouteQuery::Parse("1,2,3,0 orgrimmar horde", parsed, error) && parsed.from.mapId == 0 && parsed.team == 1,
          "literal position");
    check(!RouteQuery::Parse("orgrimmar", parsed, error), "one argument refused");
    check(!RouteQuery::Parse("orgrimmar nowhere", parsed, error) && error.find("nowhere") != std::string::npos,
          "unknown place named");
    check(!RouteQuery::Parse("orgrimmar undercity 99", parsed, error), "level above 80 refused");

    TravelGraph graph;
    TravelGraphSql::LoadInfo info;
    if (!TravelGraphSql::Load(root, dbcDir, graph, info, error))
    {
        std::cout << "route-query-selftest: graph build failed: " << error << "\n";
        return 2;
    }
    RouteQuery::Parse("orgrimmar undercity", parsed, error);
    std::vector<std::string> const lines = RouteQuery::Run(graph, parsed);
    bool thundercaller = false;
    for (std::string const& line : lines)
    {
        std::cout << "  " << line << "\n";
        thundercaller = thundercaller || line.find("The Thundercaller") != std::string::npos;
    }
    check(thundercaller, "orgrimmar -> undercity uses The Thundercaller");
    check(lines.size() >= 3 && lines.back().find("legs") != std::string::npos, "totals line");
    std::cout << "route-query-selftest: " << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
