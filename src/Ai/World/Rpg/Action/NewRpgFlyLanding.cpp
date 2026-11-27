/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

#include "Log.h"
#include "Map.h"
#include "NewRpgInternal.h"
#include "Player.h"
#include "TravelGraph.h"
#include "TravelLegRules.h"
#include "TravelWalkRules.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

namespace
{
constexpr float LANDING_PROBE_YARDS = 5.0f;   // the landing check searches the ground down from this above a point
constexpr float LANDING_PROBE_DEPTH = 40.0f;  // and this far down

// The fly_landing row's word: kept (open_sky; unchecked without a map or a line), moved, node or refused
char const* LandingWord(TravelLegs::LandingCheck check, bool checked)
{
    switch (check)
    {
        case TravelLegs::LandingCheck::Kept:
            return checked ? "open_sky" : "unchecked";
        case TravelLegs::LandingCheck::Moved:
            return "moved";
        case TravelLegs::LandingCheck::Node:
            return "node";
        case TravelLegs::LandingCheck::Refused:
            return "refused";
    }
    return "unchecked";
}

// The graph nodes a landing may move to (pathing PA73): the LANDING_NODES nearest the end, within LANDING_NODE_YARDS
// (2D), not read indoors (playerbots_travelnode.outdoor 0) and no wait stop, with a walk link (a walk leaves there);
// nearest first. Their z is the node's; the check finds the ground below it
std::vector<TravelRoute::Point> LandingNodes(TravelRoute::Point const& end)
{
    TravelGraph const& graph = TravelGraph::Instance();
    std::vector<TravelRoute::Point> nodes;
    for (std::uint32_t const index : graph.NearestNodes(end.mapId, end.x, end.y, end.z, TravelLegs::LANDING_NODES))
    {
        TravelGraphNode const& node = graph.GetNode(index);
        std::span<TravelGraphLink const> const links = graph.LinksOf(index);
        bool const walks = std::any_of(links.begin(), links.end(),
                                       [](TravelGraphLink const& link) { return link.type == TravelLinkType::Walk; });
        if (walks && node.outdoor != 0 && !node.waitStop &&
            std::hypot(node.x - end.x, node.y - end.y) <= TravelLegs::LANDING_NODE_YARDS)
            nodes.push_back({node.mapId, node.x, node.y, node.z});
    }
    return nodes;
}
}  // namespace

namespace NewRpgInternal
{
// The landing check (TravelLegs::NeedsLandingCheck; pathing PA56, PA73): each point asked (the landing line, then the
// graph nodes nearest the end) on the ground below it, searched down from LANDING_PROBE_YARDS above it (a roof
// higher up is not the ground there); open when the map's terrain status reads it outdoors and TravelLegs::OpenSky
// finds no terrain and nothing the vmap holds (a roof, rock, ice) above its floor up to the descent's height (the bot's
// or the descent point's, the higher); joined to the planned landing by the walk in's navmesh walk (PA56, G356: one
// smooth wide path call as a probe step asks, a normal path whose end lies within WAYPOINT_YARDS of the landing; asked
// of open points only, at most 19 per fly leg). The outcome: TravelLegs::ChooseLanding, before the descent; one
// fly_landing row per check, whichever branch ran
TravelLegs::LandingCheck CheckLanding(Player* bot, TravelLegs::Trip& trip, uint32 tripId)
{
    Map* map = bot->GetMap();
    std::vector<TravelRoute::Point> line = TravelLegs::LandingLine(trip);
    if (!map || line.empty())
    {
        TravelLegs::MoveLanding(trip, trip.waypoints.empty() ? TravelRoute::Point{} : trip.waypoints.back().at);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "fly_landing", "unchecked", 0.0f, {}, tripId);
        return TravelLegs::LandingCheck::Kept;
    }
    uint32 const phase = bot->GetPhaseMask();
    auto const ground = [&](TravelRoute::Point& point)
    { point.z = map->GetHeight(phase, point.x, point.y, point.z + LANDING_PROBE_YARDS, true, LANDING_PROBE_DEPTH); };
    for (TravelRoute::Point& point : line)
        ground(point);
    std::size_t const count = trip.waypoints.size();
    float const top = std::max(bot->GetPositionZ(), count >= 3 ? trip.waypoints[count - 2].at.z : 0.0f);
    // Over a point's floor: the terrain's height there and the highest surface found down from top (the floor itself
    // where nothing covers it)
    auto const cover = [&](TravelRoute::Point const& point)
    {
        float const above = top > point.z + TravelLegs::LANDING_SKY_YARDS
                                ? map->GetHeight(phase, point.x, point.y, top, true, top - point.z + 1.0f)
                                : point.z;
        return std::pair<float, float>{map->GetGridHeight(point.x, point.y), above};
    };
    float const collision = bot->GetCollisionHeight();
    auto const open = [&](TravelRoute::Point const& point)
    {
        if (point.z <= INVALID_HEIGHT)
            return false;
        PositionFullTerrainStatus status;
        map->GetFullTerrainStatusForPosition(phase, point.x, point.y, point.z, collision, status);
        if (!status.outdoors)
            return false;
        auto const [terrain, above] = cover(point);
        return TravelLegs::OpenSky(point.z, terrain, above);
    };
    TravelRoute::Point const planned = trip.waypoints.back().at;
    TravelWalk::ProbePoint const end{planned.x, planned.y, planned.z};
    uint32 probes = 0;
    auto const joined = [&](TravelRoute::Point const& point)
    {
        ++probes;
        TravelWalk::ProbePoint const from{point.x, point.y, point.z};
        TravelWalk::StepAnswer const answer = NewRpgInternal::AskTravelPath(bot, from, end, false, true);
        TravelWalk::ProbePoint const last = answer.points.empty() ? from : answer.points.back();
        return answer.type == TravelWalk::PATH_NORMAL && TravelWalk::Distance(last, end) <= TravelLegs::WAYPOINT_YARDS;
    };
    std::size_t asked = 0;
    auto const nodes = [&]()
    {
        std::vector<TravelRoute::Point> points = LandingNodes(planned);
        for (TravelRoute::Point& point : points)
            ground(point);
        asked = points.size();
        return points;
    };
    TravelRoute::Point const& plannedFloor = line.back();
    auto const [terrainOver, aboveOver] = cover(plannedFloor);
    float const covered =
        plannedFloor.z > INVALID_HEIGHT ? std::max(0.0f, std::max(terrainOver, aboveOver) - plannedFloor.z) : -1.0f;
    TravelLegs::LandingCheck const check = TravelLegs::ChooseLanding(trip, line, nodes, open, joined);
    TravelRoute::Point const& landing = trip.waypoints.back().at;
    float const moved = TravelRoute::Distance(planned, landing);
    // The row (PA73): the word, the yards moved, the cover over the planned floor (0 open, -1 no floor found), the
    // walk probes asked and the graph nodes looked at
    WorldLog::Decision(
        bot, WorldLogFormat::DecisionKind::Trip, "fly_landing", LandingWord(check, true), moved,
        {{"cover", covered}, {"probes", static_cast<float>(probes)}, {"nodes", static_cast<float>(asked)}}, tripId);
    if (check == TravelLegs::LandingCheck::Refused)
        LOG_DEBUG("playerbots",
                  "[Route] {} landing at ({:.0f}, {:.0f}, {:.0f}) is not under open sky ({:.0f} yd covered): none "
                  "joined of the line and {} graph nodes ({} probes); the fly leg fails landing before its descent",
                  bot->GetName(), planned.x, planned.y, planned.z, covered, asked, probes);
    else if (check == TravelLegs::LandingCheck::Kept)
        LOG_DEBUG("playerbots", "[Route] {} landing at ({:.0f}, {:.0f}, {:.0f}) is under open sky: kept",
                  bot->GetName(), planned.x, planned.y, planned.z);
    else
        LOG_DEBUG("playerbots",
                  "[Route] {} landing at ({:.0f}, {:.0f}, {:.0f}) is not under open sky ({:.0f} yd covered): lands "
                  "{:.0f} yd away at {} ({:.0f}, {:.0f}, {:.0f}), joined ({} probes), and walks in",
                  bot->GetName(), planned.x, planned.y, planned.z, covered, moved,
                  check == TravelLegs::LandingCheck::Node ? "a graph node" : "a point back along the flight", landing.x,
                  landing.y, landing.z, probes);
    return check;
}
}  // namespace NewRpgInternal
