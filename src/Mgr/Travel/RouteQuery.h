/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTEQUERY_H
#define PLAYERBOTS_ROUTEQUERY_H

// `playerbots route query <from> <to> [side] [level]` (NV3a): the legs the route search plans for a made-up bot,
// with no bot online. The text parsing and the printing are header-only so tests/route-query-selftest.cpp runs them
// on the shipped graph without the engine; RouteQuery.cpp is the console wiring.
// <from>/<to> are a TravelPlaces name or "x,y,z,map" (no spaces); [side] alliance|horde (default: the side a route to
// the named <to> is tested for, else Horde); [level] 1-80 (default 80). The bot is rich, knows every flight master
// and may use ships, zeppelins and elevators (Snapshot::allowTransport), so the transport legs show.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "RouteRules.h"
#include "TravelPlaces.h"

class ChatHandler;

namespace RouteQuery
{
// The console command (RouteQuery.cpp)
bool HandleCommand(ChatHandler* handler, char const* args);

struct Parsed
{
    TravelRoute::Point from;
    TravelRoute::Point to;
    std::uint8_t team{1};  // TeamId: 0 Alliance, 1 Horde
    std::uint8_t level{80};
    bool sideGiven{false};
};

inline std::vector<std::string> Tokens(std::string_view text)
{
    std::vector<std::string> tokens;
    std::string token;
    for (char const c : text)
    {
        if (c == ' ' || c == '\t')
        {
            if (!token.empty())
                tokens.push_back(token);
            token.clear();
        }
        else
            token.push_back(c);
    }
    if (!token.empty())
        tokens.push_back(token);
    return tokens;
}

inline char const* Usage()
{
    return "Usage: .playerbots route query <from> <to> [alliance|horde] [level]; a place name or x,y,z,map "
           "(no spaces) for <from> and <to>";
}

// False with an error text when the arguments do not parse
inline bool Parse(std::string_view args, Parsed& out, std::string& error)
{
    std::vector<std::string> const tokens = Tokens(args);
    if (tokens.size() < 2 || tokens.size() > 4)
    {
        error = Usage();
        return false;
    }
    std::optional<TravelRoute::Point> const from = TravelPlaces::Resolve(tokens[0]);
    std::optional<TravelRoute::Point> const to = TravelPlaces::Resolve(tokens[1]);
    if (!from || !to)
    {
        error = std::string("unknown place '") + tokens[from ? 1 : 0] + "'; places: " + TravelPlaces::Names();
        return false;
    }
    out = Parsed{};
    out.from = *from;
    out.to = *to;
    // Default side: the one a route to <to> is tested for (undercity: Horde), else Horde
    if (TravelPlaces::Place const* place = TravelPlaces::Find(tokens[1]))
        out.team = place->testTeam;
    for (std::size_t i = 2; i < tokens.size(); ++i)
    {
        std::string const word = TravelPlaces::Key(tokens[i]);
        if (word == "alliance" || word == "horde")
        {
            out.team = word == "horde" ? 1 : 0;
            out.sideGiven = true;
        }
        else
        {
            char* end = nullptr;
            unsigned long const level = std::strtoul(tokens[i].c_str(), &end, 10);
            if (end == tokens[i].c_str() || *end != '\0' || level < 1 || level > 80)
            {
                error = std::string("bad side or level '") + tokens[i] + "'. " + Usage();
                return false;
            }
            out.level = static_cast<std::uint8_t>(level);
        }
    }
    return true;
}

inline std::string PointText(TravelRoute::Point const& p)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "map %u (%.0f, %.0f, %.0f)", p.mapId, p.x, p.y, p.z);
    return buffer;
}

inline std::string ObjectText(TravelRoute::Leg const& leg)
{
    if (!leg.object)
        return "";
    std::string text = ", object " + std::to_string(leg.object);
    if (leg.type == TravelRoute::LegType::Transport && leg.object == 164871)
        text += " (The Thundercaller)";
    return text;
}

// Plans the route on the graph and returns the output lines
inline std::vector<std::string> Run(TravelGraph const& graph, Parsed const& query)
{
    std::vector<std::string> lines;
    TravelRoute::Request request;
    request.snapshot.position = query.from;
    request.snapshot.team = query.team;
    request.snapshot.level = query.level;
    request.snapshot.money = 100000000;
    request.snapshot.knownTaxi.fill(~0u);
    request.snapshot.allowTransport = true;
    request.goal = query.to;
    lines.push_back("ROUTE query: " + PointText(query.from) + " -> " + PointText(query.to) + ", " +
                    (query.team ? "Horde" : "Alliance") + (query.sideGiven ? "" : " (default side)") + ", level " +
                    std::to_string(query.level) + ", all flight masters known, transport allowed");
    TravelRoute::Result const route = TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, {});
    if (!route.ok)
    {
        lines.push_back(std::string("ROUTE query: Failure: ") + TravelRoute::FailureWord(route.failure));
        return lines;
    }
    std::uint32_t number = 0;
    for (TravelRoute::Leg const& leg : route.legs)
    {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%u. %-9s %6.1f s", ++number, TravelRoute::LegTypeWord(leg.type),
                      leg.seconds);
        lines.push_back(std::string("ROUTE query: ") + buffer + "  " + PointText(leg.from) + " -> " +
                        PointText(leg.to) + ObjectText(leg));
    }
    lines.push_back("ROUTE query: " + std::to_string(route.legs.size()) + " legs, " +
                    std::to_string(static_cast<int>(route.totalSeconds)) + " s, fare " +
                    std::to_string(TravelRoute::RouteFare(route)) + " copper, " + std::to_string(route.expandedNodes) +
                    " nodes expanded");
    return lines;
}
}  // namespace RouteQuery

#endif
