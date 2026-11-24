/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FLIGHTCHOICERULES_H
#define _PLAYERBOT_FLIGHTCHOICERULES_H

#include <cstddef>
#include <cstdint>
#include <vector>

// The flight's destination of a status order (TravelMgr::GetOptimalFlightDestinations), free of the core so that
// tests/flight-choice-selftest.cpp proves it
namespace FlightChoice
{
// capitals: the side's and neutral capitals when the order drew them (ProbTeleToBankers), else empty; levelZones: the
// zones of the bot's level bracket. nodesOf(zone): the zone's flight nodes the bot may fly to; pathTo(node): the taxi
// path from the start node, empty when there is none (or, for a persistent bot, one through an unknown node);
// pick(n): a random index below n. PA69: the capital draw is a preference, not a refusal: with no capital reachable
// the level zones follow (G358); a zone counts through any of its nodes with a path, drawn in random order, so a zone
// is passed over only when none of its nodes is reachable, as the level zones on another continent are (G378). The
// path chosen, empty when none
template <class NodesOf, class PathTo, class Pick>
std::vector<std::uint32_t> Choose(std::vector<std::uint32_t> const& capitals,
                                  std::vector<std::uint32_t> const& levelZones, NodesOf const& nodesOf,
                                  PathTo const& pathTo, Pick const& pick)
{
    for (std::vector<std::uint32_t> const* const list : {&capitals, &levelZones})
    {
        std::vector<std::uint32_t> zones = *list;
        while (!zones.empty())
        {
            std::size_t const index = pick(zones.size());
            std::vector<std::uint32_t> nodes = nodesOf(zones[index]);
            while (!nodes.empty())
            {
                std::size_t const node = pick(nodes.size());
                std::vector<std::uint32_t> path = pathTo(nodes[node]);
                if (!path.empty())
                    return path;
                nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(node));
            }
            zones.erase(zones.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
    return {};
}

// PA69 (G196): a flight master is a candidate only when the bot sees it. The bot's phase at the master's spawn is the
// union of the phase masks of the area's automatic phase rows (spell_area, a phase aura) that fit its quest state, or
// the normal phase when none fits (Unit::GetPhaseByAuras, Player::SetGameMaster's restore); the master is seen when
// its spawn's phase mask shares a bit with it. rows: each row's phaseMask; fits(row): the row applies to the bot
constexpr std::uint32_t NORMAL_PHASE_MASK = 1;
template <class Rows, class Fits>
bool MasterSeen(std::uint32_t spawnPhaseMask, Rows const& rows, Fits const& fits)
{
    std::uint32_t phase = 0;
    for (auto const& row : rows)
        if (fits(row))
            phase |= row.phaseMask;
    return ((phase ? phase : NORMAL_PHASE_MASK) & spawnPhaseMask) != 0;
}
}  // namespace FlightChoice

#endif
