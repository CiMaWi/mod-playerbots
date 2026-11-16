/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TRAVELGRAPHSELFTEST_H
#define _PLAYERBOT_TRAVELGRAPHSELFTEST_H

#include <cstdint>
#include <string>
#include <vector>

#include "TravelGraph.h"

// TravelGraph::Selftest's body (TravelGraph.h declares it), shared by tests/travel-graph-selftest.cpp and
// `playerbots route selftest` (TravelGraph.cpp); only they include this header.
// Any graph: the shipped one (data/sql/playerbots/base/playerbots_travelnode*.sql plus its updates) or one that
// replaces the three tables whole (the navigation plan's builds)
inline std::uint32_t TravelGraph::Selftest(std::vector<std::string>& failures, std::vector<std::string>& notes) const
{
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back(what);
        return ok;
    };
    auto const num = [](double value) { return std::to_string(std::llround(value)); };
    auto const expect = [&check](std::size_t actual, std::size_t wanted, char const* what)
    {
        return check(actual == wanted,
                     std::string(what) + " " + std::to_string(actual) + " (expected " + std::to_string(wanted) + ")");
    };

    if (!check(IsReady(), "graph ready"))
        return checks;

    // 1. The counts agree: every link has a known type, every point a link
    std::size_t typed = 0;
    for (std::uint32_t const count : _stats.linksByType)
        typed += count;
    check(!_nodes.empty() && !_links.empty() && !_points.empty(), "a graph with nodes, links and points");
    expect(typed, _links.size(), "links by type");
    expect(_stats.linksByType[0] + _stats.linksByType[5], 0, "links of no type");
    expect(_stats.waitPointLinks,
           static_cast<std::size_t>(std::count_if(_links.begin(), _links.end(), IsWaitPointLink)), "wait-point links");
    expect(_stats.skippedLinks, 0, "skipped links");
    expect(_stats.orphanPoints, 0, "points without a link");

    // 2. Per-node link ranges: contiguous, sorted by target, in range, no link to itself; point ranges disjoint
    std::size_t badLinks = 0;
    std::size_t nextLink = 0;
    std::size_t pointsUsed = 0;
    std::size_t badPaths = 0;
    std::size_t flightsWithoutLength = 0;
    std::size_t thundercaller = 0;
    std::size_t waitStops = 0;
    for (std::uint32_t index = 0; index < _nodes.size(); ++index)
    {
        TravelGraphNode const& node = _nodes[index];
        if (node.linkCount && node.firstLink != nextLink)
            ++badLinks;
        waitStops += node.waitStop ? 1 : 0;
        nextLink += node.linkCount;
        std::uint32_t previousTo = NO_NODE;
        for (TravelGraphLink const& link : LinksOf(index))
        {
            if (link.from != index || link.to >= _nodes.size() || link.to == index ||
                (previousTo != NO_NODE && previousTo >= link.to))
                ++badLinks;
            previousTo = link.to;
            if (static_cast<std::size_t>(link.pathFirst) + link.pathCount > _points.size())
                ++badPaths;
            pointsUsed += link.pathCount;
            if (link.type == TravelLinkType::Flight && !(link.pathLength > 0.0f))
                ++flightsWithoutLength;
            // The zeppelin between Orgrimmar and Tirisfal Glades (The Thundercaller), both ways
            if (link.type == TravelLinkType::Transport && link.object == 164871)
                ++thundercaller;
        }
    }
    expect(nextLink, _links.size(), "links in node ranges");
    expect(badLinks, 0, "links out of place");
    expect(badPaths, 0, "point ranges out of bounds");
    expect(pointsUsed, _points.size(), "points in link ranges");
    expect(flightsWithoutLength, 0, "flight links without a path length");
    expect(waitStops, _stats.waitStops, "stops with a wait point");
    check(thundercaller >= 2, "The Thundercaller rides " + std::to_string(thundercaller) + " (expected both ways)");
    char const* const typeNames[TRAVEL_LINK_TYPES] = {"none",   "walk",   "portal",       "transport",
                                                      "flight", "type 5", "static portal"};
    std::string counts = "links by type:";
    for (std::size_t type = 1; type < TRAVEL_LINK_TYPES; ++type)
        if (_stats.linksByType[type])
            counts += std::string(" ") + typeNames[type] + " " + std::to_string(_stats.linksByType[type]);
    notes.push_back(counts + "; wait-point links " + std::to_string(_stats.waitPointLinks) +
                    ", stops with a wait point " + std::to_string(_stats.waitStops));

    // 3. The grid answers like a scan over the map's nodes (squared distances of the k nearest)
    std::size_t gridMisses = 0;
    std::size_t gridQueries = 0;
    for (std::uint32_t index = 0; index < _nodes.size(); index += 37)
    {
        TravelGraphNode const& node = _nodes[index];
        float const x = node.x + 37.5f;
        float const y = node.y - 23.25f;
        float const z = node.z + 4.0f;
        auto const squared = [x, y, z](TravelGraphNode const& other)
        {
            float const dx = other.x - x;
            float const dy = other.y - y;
            float const dz = other.z - z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::vector<float> scan;
        for (TravelGraphNode const& other : _nodes)
            if (other.mapId == node.mapId)
                scan.push_back(squared(other));
        std::sort(scan.begin(), scan.end());
        std::vector<std::uint32_t> const nearest = NearestNodes(node.mapId, x, y, z, 4);
        ++gridQueries;
        bool same = nearest.size() == std::min<std::size_t>(4, scan.size());
        for (std::size_t i = 0; same && i < nearest.size(); ++i)
            same = _nodes[nearest[i]].mapId == node.mapId && squared(_nodes[nearest[i]]) == scan[i];
        if (!same)
            ++gridMisses;
    }
    check(gridMisses == 0,
          "nearest nodes differ from the scan in " + std::to_string(gridMisses) + " of " + std::to_string(gridQueries));

    // 4. Three routes over the simple cost. Places by name, nearest to a known position (names are not unique)
    struct Place
    {
        char const* name;
        std::uint32_t mapId;
        float x;
        float y;
    };
    struct RouteCase
    {
        char const* label;
        Place from;
        Place to;
        TravelLinkType mustUse;
        std::uint32_t mustObject;  // the object of a mustUse link, 0 = any
        float minSeconds;
        float maxSeconds;
    };
    // Ranges (simple cost: run 7 yd/s, fly 32 yd/s, ship or zeppelin 180 s, portal 3 s; the shipped graph gives 373,
    // 468 and 967 s):
    // - Goldshire -> Ironforge: about 1200 yd of running to Stormwind's flight master (170 s) and the 6500 yd flight
    //   (200 s; about 3.5 min in the game); on foot through the Deeprun Tram the walk alone takes more than 5 min
    // - Orgrimmar -> Undercity: about 1900 yd of running (inn to tower, tower to inn with the Undercity elevators) and
    //   The Thundercaller (180 s). A graph with static portals takes Orgrimmar's portal to the Blasted Lands, the
    //   Dark Portal, a flight to Shattrath and its portal to Undercity (about 420 s) instead: the simple cost has no
    //   level or side
    // - Darnassus -> Stormwind: the portal to Rut'theran Village, at least one ship and long flights; the shipped graph
    //   has no walk link onto The Bravery at Auberdine, so its route flies to Ratchet and sails to Booty Bay (The
    //   Maiden's Fancy). More than 30 min would be a detour. A graph with static portals (the navigation plan's)
    //   takes Darnassus' portal to the Blasted Lands, the Dark Portal and the Stair of Destiny's portal to Stormwind
    //   (about 200 s) instead
    bool const staticPortals = _stats.linksByType[static_cast<std::size_t>(TravelLinkType::StaticPortal)] > 0;
    RouteCase const cases[] = {
        {"Goldshire -> Ironforge",
         {"Goldshire innkeeper", 0, -9462.7f, 16.2f},
         {"flightMaster", 0, -4821.8f, -1152.4f},
         TravelLinkType::Flight,
         0,
         250.0f,
         600.0f},
        {"Orgrimmar -> Undercity",
         {"Orgrimmar innkeeper", 1, 1633.9f, -4439.4f},
         {"Undercity innkeeper", 0, 1637.0f, 220.0f},
         staticPortals ? TravelLinkType::StaticPortal : TravelLinkType::Transport,
         staticPortals ? 183327u : 164871u,
         300.0f,
         900.0f},
        {"Darnassus -> Stormwind",
         {"Darnassus", 1, 9947.0f, 2482.0f},
         {"Stormwind City innkeeper", 0, -8867.8f, 673.7f},
         staticPortals ? TravelLinkType::StaticPortal : TravelLinkType::Transport,
         0,
         staticPortals ? 120.0f : 600.0f,
         1800.0f},
    };

    for (RouteCase const& route : cases)
    {
        std::uint32_t const from = FindNear(route.from.name, route.from.mapId, route.from.x, route.from.y);
        std::uint32_t const to = FindNear(route.to.name, route.to.mapId, route.to.x, route.to.y);
        if (!check(from != NO_NODE && to != NO_NODE, std::string(route.label) + ": places not found"))
            continue;

        TravelGraphRoute const found = SimpleRoute(from, to);
        if (!check(found.found, std::string(route.label) + ": no route"))
            continue;

        std::uint32_t legs[TRAVEL_LINK_TYPES]{};
        float yards[TRAVEL_LINK_TYPES]{};
        bool usedObject = !route.mustObject;
        std::string chain;
        float sum = 0.0f;
        bool chained = true;
        std::uint32_t at = from;
        for (std::uint32_t const linkIndex : found.links)
        {
            TravelGraphLink const& link = _links[linkIndex];
            chained = chained && link.from == at;
            at = link.to;
            sum += SimpleCost(link);
            std::size_t const type = static_cast<std::size_t>(link.type);
            ++legs[type];
            yards[type] += link.type == TravelLinkType::Walk ? link.distance : link.pathLength;
            usedObject = usedObject || (link.type == route.mustUse && link.object == route.mustObject);
            if (link.type != TravelLinkType::Walk)
                chain += std::string("; ") + typeNames[type] + " " + std::to_string(link.object) + " " +
                         std::string(GetName(link.from)) + " -> " + std::string(GetName(link.to));
        }
        check(chained && at == to, std::string(route.label) + ": links do not chain from start to goal");
        check(std::fabs(sum - found.seconds) <= 0.01f * found.seconds + 0.1f,
              std::string(route.label) + ": cost " + num(found.seconds) + " s is not the sum of its links " + num(sum));
        check(legs[static_cast<std::size_t>(route.mustUse)] > 0,
              std::string(route.label) + ": no " + typeNames[static_cast<std::size_t>(route.mustUse)] + " link");
        check(usedObject, std::string(route.label) + ": no link of object " + std::to_string(route.mustObject));
        check(found.seconds >= route.minSeconds && found.seconds <= route.maxSeconds,
              std::string(route.label) + ": " + num(found.seconds) + " s outside " + num(route.minSeconds) + ".." +
                  num(route.maxSeconds) + " s");

        std::string note = std::string(route.label) + ": " + std::string(GetName(from)) + " (" +
                           std::to_string(_nodes[from].id) + ") -> " + std::string(GetName(to)) + " (" +
                           std::to_string(_nodes[to].id) + "), " + num(found.seconds) + " s over " +
                           std::to_string(found.links.size()) + " links:";
        for (std::size_t type = 1; type < TRAVEL_LINK_TYPES; ++type)
            if (legs[type])
                note += std::string(" ") + typeNames[type] + " " + std::to_string(legs[type]) + " (" +
                        num(yards[type]) + " yd)";
        notes.push_back(note + chain);
    }

    return checks;
}

#endif
