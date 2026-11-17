/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTEQUERY_H
#define PLAYERBOTS_ROUTEQUERY_H

// `playerbots route query <from> <to> [side] [level] [fly]` (NV3a): the legs the route search plans for a made-up bot,
// with no bot online. The text parsing and the printing are header-only so tests/route-query-selftest.cpp runs them
// on the shipped graph without the engine; RouteQuery.cpp is the console wiring.
// <from>/<to> are a TravelPlaces name or "x,y,z,map" (no spaces); [side] alliance|horde (default: the side a route to
// the named <to> is tested for, else Horde); [level] 1-80 (default 80). The bot is rich, knows every flight master
// and may use ships, zeppelins and elevators (Snapshot::allowTransport), so the transport legs show. [fly]: the bot
// may fly on Outland and Northrend (a 280% mount), so the air links (WT61) show; the console flies them over its
// flight maps, a query without maps lists them unflown.

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
    bool fly{false};
};

constexpr float QUERY_FLY_SPEED = 7.0f * 3.8f;  // yd/s: the core's base flight speed with a 280% mount

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
    return "Usage: .playerbots route query <from> <to> [alliance|horde] [level] [fly]; a place name or x,y,z,map "
           "(no spaces) for <from> and <to>; fly: the bot may fly";
}

// False with an error text when the arguments do not parse
inline bool Parse(std::string_view args, Parsed& out, std::string& error)
{
    std::vector<std::string> const tokens = Tokens(args);
    if (tokens.size() < 2 || tokens.size() > 5)
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
        else if (word == "fly")
            out.fly = true;
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

// The made-up bot's request
inline TravelRoute::Request RequestOf(Parsed const& query)
{
    TravelRoute::Request request;
    request.snapshot.position = query.from;
    request.snapshot.team = query.team;
    request.snapshot.level = query.level;
    request.snapshot.money = 100000000;
    request.snapshot.knownTaxi.fill(~0u);
    request.snapshot.allowTransport = true;
    if (query.fly)
    {
        request.snapshot.flyOutland = true;
        request.snapshot.flyNorthrend = true;
        request.snapshot.flySpeed = QUERY_FLY_SPEED;
    }
    request.goal = query.to;
    return request;
}

// A transport leg's wait points, where the graph has them: an end joined to its stop by a wait-point link (waitFrom,
// waitTo), else none (the leg ends at the stop)
inline std::string WaitPointText(TravelGraph const& graph, TravelRoute::Leg const& leg)
{
    if (leg.type != TravelRoute::LegType::Transport || leg.nodes.empty() || (!leg.waitFrom && !leg.waitTo))
        return "";
    auto const name = [&graph](bool wait, std::uint32_t node)
    { return wait ? "'" + std::string(graph.GetName(node)) + "'" : std::string("none"); };
    return ", wait points " + name(leg.waitFrom, leg.nodes.front()) + " -> " + name(leg.waitTo, leg.nodes.back());
}

// Plans the route on the graph and returns the output lines; params carry the flight maps an air link is flown over
inline std::vector<std::string> Run(TravelGraph const& graph, Parsed const& query,
                                    TravelRoute::Params const& params = {})
{
    std::vector<std::string> lines;
    TravelRoute::Request const request = RequestOf(query);
    lines.push_back("ROUTE query: " + PointText(query.from) + " -> " + PointText(query.to) + ", " +
                    (query.team ? "Horde" : "Alliance") + (query.sideGiven ? "" : " (default side)") + ", level " +
                    std::to_string(query.level) + ", all flight masters known, transport allowed" +
                    (!query.fly          ? ""
                     : params.flightMaps ? ", bot may fly"
                                         : ", bot may fly (no flight maps: air links unflown)"));
    TravelRoute::Result const route = TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, params);
    if (!route.ok)
    {
        lines.push_back(std::string("ROUTE query: Failure: ") + TravelRoute::FailureWord(route.failure));
        return lines;
    }
    std::uint32_t number = 0;
    for (TravelRoute::Leg const& leg : route.legs)
    {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%u. %-9s %6.1f s", ++number,
                      leg.air ? "air" : TravelRoute::LegTypeWord(leg.type), leg.seconds);
        std::string const air = leg.air && leg.nodes.size() == 2
                                    ? ", air link " + std::to_string(graph.GetNode(leg.nodes.front()).id) + " -> " +
                                          std::to_string(graph.GetNode(leg.nodes.back()).id) +
                                          (leg.type == TravelRoute::LegType::Fly ? ", flown" : ", unflown")
                                    : "";
        lines.push_back(std::string("ROUTE query: ") + buffer + "  " + PointText(leg.from) + " -> " +
                        PointText(leg.to) + ObjectText(leg) + WaitPointText(graph, leg) + air);
    }
    lines.push_back("ROUTE query: " + std::to_string(route.legs.size()) + " legs, " +
                    std::to_string(static_cast<int>(route.totalSeconds)) + " s, fare " +
                    std::to_string(TravelRoute::RouteFare(route)) + " copper, " + std::to_string(route.expandedNodes) +
                    " nodes expanded");
    return lines;
}
}  // namespace RouteQuery

#endif
