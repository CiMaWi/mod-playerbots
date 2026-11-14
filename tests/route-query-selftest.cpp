/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of `playerbots route query` (src/Mgr/Travel/RouteQuery.h): the argument parsing and, on the
// shipped travel graph, the legs from Orgrimmar's zeppelin tower to Undercity (The Thundercaller, entry 164871). Given
// a graph directory (a graph with wait points, the navigation plan's), also five crossings by their vessels, each one
// transport leg through both wait points, and three trips through static portals (WT55d). The crossings start at the
// vessel's tower or harbour where a city's portals would beat the vessel from its inn.
// Usage: route-query-selftest [module root] [dbc dir] [graph dir] (defaults: the current directory,
// C:/Games/AzerothCore/Server/data/dbc, the shipped graph)
#include <algorithm>
#include <cmath>
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
    RouteQuery::Parse("1331.11,-4649.45,53.63,1 undercity", parsed, error);
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

    // The wait points' text names an end only where the leg has a wait point there (WT55b)
    TravelRoute::Leg half;
    half.type = TravelRoute::LegType::Transport;
    half.nodes = {0, 1, 2};
    half.rideAt = 1;
    half.waitFrom = true;
    check(RouteQuery::WaitPointText(graph, half) == ", wait points '" + std::string(graph.GetName(0)) + "' -> none",
          "wait points text: the near end only");
    half.nodes = {0, 1};
    half.rideAt = 0;
    half.waitFrom = false;
    check(RouteQuery::WaitPointText(graph, half).empty(), "wait points text: none for a leg from stop to stop");
    half.nodes = {0, 1, 2};
    check(RouteQuery::WaitPointText(graph, half).empty(), "wait points text: three nodes alone name no wait point");

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
            {"1331.11,-4649.45,53.63,1 undercity horde", 164871, "The Thundercaller"},
            {"ratchet -14457.7,495.3,15.2,0 horde", 20808, "The Maiden's Fancy"},
            {"-8647.4,1329.38,5.7,0 6406.5,515.4,8.7,1 alliance", 176310, "The Bravery"},
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
            std::vector<std::string> const run = RouteQuery::Run(graph, parsed);
            for (std::string const& line : run)
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
            std::string const text = ", wait points '" + std::string(graph.GetName(ride->nodes.front())) + "' -> '" +
                                     std::string(graph.GetName(ride->nodes.back())) + "'";
            check(std::any_of(run.begin(), run.end(),
                              [&text](std::string const& line) { return line.find(text) != std::string::npos; }),
                  label + ": the route names both wait points");
            check(std::count_if(route.legs.begin(), route.legs.end(), [](TravelRoute::Leg const& leg)
                                { return leg.type == TravelRoute::LegType::Transport && !leg.object; }) == 0,
                  label + ": a transport leg without a vessel");
            check(boardsFromWaitPoints(route), label + ": a transport boarded at a stop with a wait point");
        }

        // Static portals (WT55d): a trip from a portal object a player clicks takes it, one static portal leg of
        // the object between its two nodes (graph ids); plain Dijkstra plans the same seconds
        struct Through
        {
            char const* args;
            std::uint32_t object;
            std::uint32_t fromId;
            std::uint32_t toId;
        };
        Through const portals[] = {
            {"5719.19,719.681,641.728,571 stormwind alliance", 190960, 2186, 5708},    // Dalaran -> Stormwind
            {"-1792.78,5406.54,-12.4279,530 stormwind alliance", 183325, 2503, 5708},  // Shattrath -> Stormwind
            {"5822.35,837.076,680.657,571 5848.48,853.706,843.182,571 horde", 194011, 2410, 2411},  // one map
        };
        TravelRoute::Params dijkstra;
        dijkstra.heuristic = false;
        for (Through const& through : portals)
        {
            std::string const label =
                std::string(through.args) + " (static portal " + std::to_string(through.object) + ")";
            if (!RouteQuery::Parse(through.args, parsed, error))
            {
                check(false, label + ": " + error);
                continue;
            }
            for (std::string const& line : RouteQuery::Run(graph, parsed))
                std::cout << "  " << line << "\n";
            TravelRoute::Result const route = TravelRoute::Search(graph, RouteQuery::RequestOf(parsed), {}, {});
            auto const leg = std::find_if(route.legs.begin(), route.legs.end(), [](TravelRoute::Leg const& candidate)
                                          { return candidate.type == TravelRoute::LegType::StaticPortal; });
            bool const takes = route.ok && leg != route.legs.end();
            check(takes, label + ": no static portal leg");
            if (!takes)
                continue;
            check(leg->object == through.object && leg->nodes.size() == 2 &&
                      graph.GetNode(leg->nodes.front()).id == through.fromId &&
                      graph.GetNode(leg->nodes.back()).id == through.toId,
                  label + ": the leg is the object's link between its nodes");
            TravelRoute::Result const plain = TravelRoute::Search(graph, RouteQuery::RequestOf(parsed), {}, dijkstra);
            check(plain.ok && std::fabs(plain.totalSeconds - route.totalSeconds) < 0.01f,
                  label + ": A* and Dijkstra plan the same seconds");
        }
    }
    std::cout << "route-query-selftest: " << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
