/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKPROBE_H
#define PLAYERBOTS_ROUTECHECKPROBE_H

#include <cstdint>
#include <optional>
#include <string>

#include "TravelWalkTypes.h"

// `playerbots route check pairs <file> ... probe` (pathing PA4, plan item 7): each pair is also walked as a bot's walk
// off the graph is, by the follower's probe (TravelWalk::StartPiece over TravelWalk::OrderStep steps, judged by
// GoTo::ProbeCarried, done within the pair's arrival, ArriveYards), once with the map's query of 1,024 nodes
// (control) and once with its wide query (PathGenerator::SetWideSearch), under the check's filter. A pair is `ok` for
// the probe when it walks. Fragment ends are not refused here: the bots' fragment rule (NavFragment) is file-local
// to NewRpgRoute.cpp, so such a pair gets the search's own answer.

class WorldObject;

namespace RouteCheckProbe
{
// The check's polygons, as its Ask sets them on a PathGenerator (RouteCheck.cpp: the bots' exclusions and costs)
struct Filter
{
    std::uint16_t include{0};
    std::uint16_t exclude{0};
    std::optional<float> steepCost;  // none: steep ground excluded
    float waterCost{1.0f};
};

// Where the bots' walk is done: a label's last `@<yards>` (`a-W9@10`: a route's last waypoint, GOAL_YARDS; pathing
// PA4c), else TravelLegs::WAYPOINT_YARDS (5 yd), as a waypoint on the way
float ArriveYards(std::string const& label);

// The row value of one pair: `control=<probe>:<why>:<steps>:<us>:<type>:<end> wide=...` (the probe's word, the
// refusal's reason word, its path calls, its microseconds, the last call's PathType and how far its end lies from `to`)
std::string PairValue(WorldObject const* owner, TravelWalk::ProbePoint const& from, TravelWalk::ProbePoint const& to,
                      Filter const& filter, float arriveYards);
}  // namespace RouteCheckProbe

#endif
