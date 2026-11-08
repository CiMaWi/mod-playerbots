/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVELPLACES_H
#define PLAYERBOTS_TRAVELPLACES_H

// Named places for `playerbots route go <bot> <place>` (WT8, tests), Player-free: towns on both continents for both
// factions that a route reaches without a ship or zeppelin (Booty Bay, Menethil, Theramore and the Exodar need one).
// Each sits at an inn or flight master, so a graph node stands within a few yards. A place is also given literally as
// "x,y,z,map". Selftest (tests/route-rules-selftest.cpp, `playerbots route selftest`): every place lies within
// NODE_YARDS of a graph node and has a route from its test start.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "RouteRules.h"

namespace TravelPlaces
{
using TravelRoute::Point;

constexpr float NODE_YARDS = 100.0f;

struct Place
{
    char const* name;  // lower case letters only; a command's text is compared without case, spaces or apostrophes
    Point at;
    char const* testFrom;   // the selftest's start for the route check
    std::uint8_t testTeam;  // the selftest's TeamId (0 Alliance, 1 Horde): a flight needs a master of that side
};

inline std::vector<Place> const& All()
{
    static std::vector<Place> const places = {
        // Eastern Kingdoms
        {"stormwind", {0, -8825.53f, 630.162f, 97.8435f}, "goldshire", 0},
        {"goldshire", {0, -9458.72f, 49.5112f, 60.3729f}, "stormwind", 0},
        {"sentinelhill", {0, -10653.4f, 1166.52f, 34.4647f}, "goldshire", 0},
        {"lakeshire", {0, -9223.98f, -2157.12f, 64.0168f}, "goldshire", 0},
        {"ironforge", {0, -4840.67f, -857.094f, 501.997f}, "stormwind", 0},
        {"southshore", {0, -857.096f, -570.751f, 11.0649f}, "ironforge", 0},
        {"undercity", {0, 1635.4f, 223.331f, -43.0198f}, "brill", 1},
        {"brill", {0, 2234.76f, 252.124f, 37.3704f}, "undercity", 1},
        {"tarrenmill", {0, -5.96873f, -942.282f, 57.1621f}, "undercity", 1},
        {"silvermoon", {530, 9565.68f, -7222.8f, 16.4272f}, "undercity", 1},
        // Kalimdor
        {"orgrimmar", {1, 1633.99f, -4439.37f, 15.5167f}, "razorhill", 1},
        {"razorhill", {1, 340.362f, -4686.29f, 16.5411f}, "orgrimmar", 1},
        {"crossroads", {1, -407.123f, -2645.22f, 96.3063f}, "orgrimmar", 1},
        {"thunderbluff", {1, -1274.74f, 121.512f, 134.977f}, "crossroads", 1},
        {"ratchet", {1, -1050.04f, -3664.8f, 23.9684f}, "crossroads", 1},
        {"astranaar", {1, 2781.16f, -432.997f, 116.665f}, "crossroads", 0},
        {"darnassus", {1, 9948.88f, 2484.54f, 1316.35f}, "astranaar", 0},
    };
    return places;
}

// The text's letters in lower case: "Thunder Bluff" and "thunderbluff" are one place
inline std::string Key(std::string_view text)
{
    std::string key;
    for (char const c : text)
        if (std::isalpha(static_cast<unsigned char>(c)))
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return key;
}

inline Place const* Find(std::string_view text)
{
    std::string const key = Key(text);
    if (key.empty())
        return nullptr;
    for (Place const& place : All())
        if (key == place.name)
            return &place;
    return nullptr;
}

// "x,y,z,map" (spaces allowed around the numbers)
inline std::optional<Point> ParseLiteral(std::string_view text)
{
    std::string const copy(text);
    char const* cursor = copy.c_str();
    float values[3] = {};
    for (float& value : values)
    {
        char* end = nullptr;
        value = std::strtof(cursor, &end);
        if (end == cursor)
            return std::nullopt;
        cursor = end;
        while (*cursor == ' ')
            ++cursor;
        if (*cursor != ',')
            return std::nullopt;
        ++cursor;
    }
    while (*cursor == ' ')
        ++cursor;
    if (!std::isdigit(static_cast<unsigned char>(*cursor)))
        return std::nullopt;
    char* end = nullptr;
    unsigned long const mapId = std::strtoul(cursor, &end, 10);
    while (*end == ' ')
        ++end;
    if (*end != '\0' || mapId > 0xFFFFu)
        return std::nullopt;
    for (float const value : values)
        if (!std::isfinite(value))
            return std::nullopt;
    return Point{static_cast<std::uint32_t>(mapId), values[0], values[1], values[2]};
}

// A place's name or a literal position
inline std::optional<Point> Resolve(std::string_view text)
{
    if (Place const* place = Find(text))
        return place->at;
    return ParseLiteral(text);
}

inline std::string Names()
{
    std::string names;
    for (Place const& place : All())
        names += (names.empty() ? "" : ", ") + std::string(place.name);
    return names;
}

// Every place within NODE_YARDS of a graph node and with a route from its test start (every taxi node known, money
// for any fare, no hearthstone, no transport). Returns the number of checks
inline std::uint32_t Selftest(TravelGraph const& graph, std::vector<std::string>& failures,
                              std::vector<std::string>& notes)
{
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back(what);
        return ok;
    };
    if (!check(graph.IsReady(), "p) graph ready"))
        return checks;

    std::uint32_t routed = 0;
    float slowest = 0.0f;
    for (Place const& place : All())
    {
        Point const& at = place.at;
        std::vector<std::uint32_t> const nearest = graph.NearestNodes(at.mapId, at.x, at.y, at.z, 1);
        float yards = 1.0e9f;
        if (!nearest.empty())
        {
            TravelGraphNode const& node = graph.GetNode(nearest.front());
            yards = TravelRoute::Distance(at, Point{node.mapId, node.x, node.y, node.z});
        }
        check(yards <= NODE_YARDS, std::string("p) ") + place.name + " lies within 100 yd of a graph node");

        Place const* from = Find(place.testFrom);
        if (!check(from && from != &place, std::string("p) ") + place.name + " has a test start"))
            continue;
        TravelRoute::Request request;
        request.snapshot.position = from->at;
        request.snapshot.team = place.testTeam;
        request.snapshot.level = 80;
        request.snapshot.money = 100000000;
        request.snapshot.knownTaxi.fill(~0u);
        request.goal = at;
        TravelRoute::Result const route = TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, {});
        if (check(route.ok, std::string("p) ") + place.name + " has a route from " + from->name))
        {
            ++routed;
            slowest = std::max(slowest, route.totalSeconds);
        }
    }
    notes.push_back("p) " + std::to_string(routed) + "/" + std::to_string(All().size()) +
                    " places routed from their test starts, slowest " + std::to_string(static_cast<int>(slowest)) +
                    " s");
    return checks;
}
}  // namespace TravelPlaces

#endif
