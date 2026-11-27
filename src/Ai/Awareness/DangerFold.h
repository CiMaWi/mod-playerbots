/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_DANGER_FOLD_H
#define _PLAYERBOT_DANGER_FOLD_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "DangerRules.h"
#include "RouteTypes.h"
#include "TravelGraph.h"

// The known danger over the travel graph (awareness AW37a, INTERFACE.md §4.4, §4.5): the fold of every walk link and
// a route's walk links. Reads the graph and writes none of it; pure, so the selftest folds generated graphs. Kept out
// of DangerRules.h, which the route rules include without the graph's folder on a selftest's path
namespace DangerRules
{
// A walk link's line: its points, or the straight line between its nodes where it has fewer than two
inline void LineOf(TravelGraph const& graph, TravelGraphLink const& link, std::vector<Place>& line)
{
    line.clear();
    std::span<TravelGraphPoint const> const points = graph.PathOf(link);
    if (points.size() >= 2)
    {
        for (TravelGraphPoint const& point : points)
            line.push_back({point.mapId, point.x, point.y, point.z});
        return;
    }
    for (std::uint32_t const node : {link.from, link.to})
    {
        TravelGraphNode const& at = graph.GetNode(node);
        line.push_back({at.mapId, at.x, at.y, at.z});
    }
}

// Every link's danger by link index then side (Known::links); a flight, ride, lift, portal or air link has none.
// false when `stop` was set before the end (polled every 1024 links)
inline bool FoldGraph(Known const& facts, TravelGraph const& graph, std::vector<LinkDanger>& out,
                      std::atomic<bool> const* stop = nullptr)
{
    std::size_t const count = graph.LinkCount();
    out.assign(count * SIDE_COUNT, LinkDanger{});
    std::vector<Place> line;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (stop && !(index & 1023) && stop->load(std::memory_order_relaxed))
            return false;
        TravelGraphLink const& link = graph.GetLink(static_cast<std::uint32_t>(index));
        if (link.type != TravelLinkType::Walk)
            continue;
        LineOf(graph, link, line);
        std::array<LinkDanger, SIDE_COUNT> const folded = FoldLink(&facts, line);
        for (std::uint8_t side = 0; side < SIDE_COUNT; ++side)
            out[index * SIDE_COUNT + side] = folded[side];
    }
    return true;
}

// The yards of a walk link deadly for the side and level (DeadlyYardsOf over its line), for the console's readings
inline double DeadlyYardsOfLink(Known const& known, TravelGraph const& graph, std::uint32_t index, std::uint8_t side,
                                std::uint8_t level)
{
    std::vector<Place> line;
    LineOf(graph, graph.GetLink(index), line);
    return DeadlyYardsOf(&known, line, side, level);
}

// The facts that make a walk link deadly for the side and level (DecidingFactsOf over its line), for `aw ways`
inline CellFacts DecidingFactsOfLink(Known const& known, TravelGraph const& graph, std::uint32_t index,
                                     std::uint8_t side, std::uint8_t level)
{
    std::vector<Place> line;
    LineOf(graph, graph.GetLink(index), line);
    return DecidingFactsOf(&known, line, side, level);
}

// A route's walk links by index: between two consecutive nodes of a walk leg, the shortest walk link from one to the
// other; a pair with none (a walk off the graph) adds nothing
inline std::vector<std::uint32_t> WalkLinksOf(TravelGraph const& graph, std::span<TravelRoute::Leg const> legs)
{
    std::vector<std::uint32_t> out;
    std::size_t const nodes = graph.NodeCount();
    for (TravelRoute::Leg const& leg : legs)
    {
        if (leg.type != TravelRoute::LegType::Walk)
            continue;
        for (std::size_t i = 1; i < leg.nodes.size(); ++i)
        {
            std::uint32_t const from = leg.nodes[i - 1];
            std::uint32_t const to = leg.nodes[i];
            if (from >= nodes || to >= nodes)
                continue;
            TravelGraphLink const* best = nullptr;
            for (TravelGraphLink const& link : graph.LinksOf(from))
                if (link.to == to && link.type == TravelLinkType::Walk && (!best || link.distance < best->distance))
                    best = &link;
            if (best)
                out.push_back(graph.IndexOfLink(*best));
        }
    }
    return out;
}
}  // namespace DangerRules

#endif
