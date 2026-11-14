/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of `playerbots route query` (src/Mgr/Travel/RouteQuery.h): the argument parsing and, on the
// shipped travel graph, the legs of Orgrimmar -> Undercity (The Thundercaller, entry 164871). Given a graph directory
// (a graph with wait points, the navigation plan's), also five crossings by their vessels, each one transport leg
// through both wait points.
// Usage: route-query-selftest [module root] [dbc dir] [graph dir] (defaults: the current directory,
// C:/Games/AzerothCore/Server/data/dbc, the shipped graph)
#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "RouteQuery.h"
#include "TravelGraphSql.h"

int main(int argc, char** argv)
{
    std::filesystem::path const root = argc > 1 ? argv[1] : ".";
    std::filesystem::path const dbcDir = argc > 2 ? argv[2] : "C:/Games/AzerothCore/Server/data/dbc";
    std::filesystem::path const graphDir = argc > 3 ? argv[3] : "";
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
    if (!TravelGraphSql::Load(root, dbcDir, graph, info, error, graphDir))
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

    // No transport leg starts or ends at a stop with a wait point: the bot boards from the wait point (lifts too)
    auto const boardsFromWaitPoints = [&graph](TravelRoute::Result const& route)
    {
        return std::none_of(route.legs.begin(), route.legs.end(),
                            [&graph](TravelRoute::Leg const& leg)
                            {
                                return leg.type == TravelRoute::LegType::Transport &&
                                       (leg.nodes.empty() || graph.GetNode(leg.nodes.front()).waitStop ||
                                        graph.GetNode(leg.nodes.back()).waitStop);
                            });
    };
    check(boardsFromWaitPoints(TravelRoute::Search(graph, RouteQuery::RequestOf(parsed), {}, {})),
          "orgrimmar -> undercity boards no transport at a stop with a wait point");

    if (!graphDir.empty())
    {
        // Five crossings by their vessels (innkeepers' and Warsong Hold's positions), each one transport leg that runs
        // wait point, stop, stop, wait point
        struct Crossing
        {
            char const* args;
            std::uint32_t vessel;
            char const* name;
        };
        Crossing const crossings[] = {
            {"orgrimmar undercity horde", 164871, "The Thundercaller"},
            {"ratchet -14457.7,495.3,15.2,0 horde", 20808, "The Maiden's Fancy"},
            {"stormwind 6406.5,515.4,8.7,1 alliance", 176310, "The Bravery"},
            {"stormwind 2282.3,5210.8,12.6,571 alliance", 190536, "Stormwind's Pride"},
            {"orgrimmar 2822.7,6171.7,122.1,571 horde", 186238, "The Mighty Wind"},
        };
        auto const endsWith = [](std::string_view name, std::string_view tail)
        { return name.size() >= tail.size() && name.substr(name.size() - tail.size()) == tail; };
        for (Crossing const& crossing : crossings)
        {
            std::string const label = std::string(crossing.args) + " (" + crossing.name + ")";
            if (!RouteQuery::Parse(crossing.args, parsed, error))
            {
                check(false, label + ": " + error);
                continue;
            }
            for (std::string const& line : RouteQuery::Run(graph, parsed))
                std::cout << "  " << line << "\n";
            TravelRoute::Result const route = TravelRoute::Search(graph, RouteQuery::RequestOf(parsed), {}, {});
            auto const ride =
                std::find_if(route.legs.begin(), route.legs.end(), [&crossing](TravelRoute::Leg const& leg)
                             { return leg.type == TravelRoute::LegType::Transport && leg.object == crossing.vessel; });
            bool const rides = route.ok && ride != route.legs.end();
            check(rides, label + ": no transport leg of the vessel");
            if (!rides)
                continue;
            bool const waits = ride->nodes.size() == 4 && endsWith(graph.GetName(ride->nodes.front()), "dock") &&
                               endsWith(graph.GetName(ride->nodes.back()), "dock") &&
                               graph.GetNode(ride->nodes[1]).waitStop && graph.GetNode(ride->nodes[2]).waitStop;
            check(waits, label + ": the leg runs wait point, stop, stop, wait point");
            check(std::count_if(route.legs.begin(), route.legs.end(), [](TravelRoute::Leg const& leg)
                                { return leg.type == TravelRoute::LegType::Transport && !leg.object; }) == 0,
                  label + ": a transport leg without a vessel");
            check(boardsFromWaitPoints(route), label + ": a transport boarded at a stop with a wait point");
        }
    }
    std::cout << "route-query-selftest: " << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
