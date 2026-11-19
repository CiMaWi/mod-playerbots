/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKOUTDOORRULES_H
#define PLAYERBOTS_ROUTECHECKOUTDOORRULES_H

// Pure rules of `playerbots route check outdoor` (navigation plan NV32, G88/G91): no core headers, so the selftest
// (tests/route-check-outdoor-selftest.cpp) compiles them alone.
//
// What a row says about a point (the invariant the graph build relies on):
// - outdoor: the engine's PositionFullTerrainStatus::outdoors (Map::GetFullTerrainStatusForPosition) for a player
//   standing there, phase PHASEMASK_NORMAL and DEFAULT_COLLISION_HEIGHT; WorldObject::IsOutdoors() returns the same
//   after UpdatePositionData, and a mount spell (SPELL_ATTR0_ONLY_OUTDOORS) is refused where it is false.
// - area: the area at the point, a WMO area where the point stands in or on a WMO (Dalaran's city areas over
//   Crystalsong Forest), else the terrain's; zone: that area's zone (the area itself when it is a zone).
// - flyable: the area allows flying: its AreaTable entry (the zone's when the area has none) carries
//   AREA_FLAG_OUTLAND (AreaTableEntry::IsFlyable, as SpellInfo::CheckLocation asks) and not AREA_FLAG_NO_FLY_ZONE
//   (Spell::CheckCast asks the caster's area). Dalaran's no-fly WMO areas are flyable 0 although the terrain
//   below is flyable. Not in the column (they belong to the bot or the moment): Cold Weather Flying, a running
//   Wintergrasp battle, water.
// - liquid: the liquid status at the point (none, above, walk, in, under); a flyer mounts only out of water.
// A flight may start or end at a point only when outdoor = 1, flyable = 1 and liquid is neither in nor under; the
// graph build decides on the raw columns.

#include <cstdint>
#include <sstream>
#include <string>

namespace RouteCheckOutdoorRules
{
// AreaTableEntry::flags bits (DBCEnums.h; RouteCheckOutdoor.cpp asserts they match the core's)
constexpr std::uint32_t AREA_FLAG_OUTLAND_BIT = 0x00000400;
constexpr std::uint32_t AREA_FLAG_NO_FLY_ZONE_BIT = 0x20000000;

// LiquidStatus values (GridTerrainData.h; asserted in RouteCheckOutdoor.cpp)
constexpr std::uint32_t LIQUID_NONE = 0x0;
constexpr std::uint32_t LIQUID_ABOVE = 0x1;
constexpr std::uint32_t LIQUID_WALK = 0x2;
constexpr std::uint32_t LIQUID_IN = 0x4;
constexpr std::uint32_t LIQUID_UNDER = 0x8;

struct PointSpec
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::string label;
};

// hasEntry: an AreaTable entry was found for the area or, failing that, its zone; flags: that entry's
inline bool AreaAllowsFlight(bool hasEntry, std::uint32_t flags)
{
    return hasEntry && (flags & AREA_FLAG_OUTLAND_BIT) && !(flags & AREA_FLAG_NO_FLY_ZONE_BIT);
}

inline std::string LiquidWord(std::uint32_t status)
{
    switch (status)
    {
        case LIQUID_NONE:
            return "none";
        case LIQUID_ABOVE:
            return "above";
        case LIQUID_WALK:
            return "walk";
        case LIQUID_IN:
            return "in";
        case LIQUID_UNDER:
            return "under";
        default:
            return std::to_string(status);
    }
}

// A point line: `map x y z [label]`, whitespace separated; `#` starts a comment line
inline bool ParsePointLine(std::string const& line, PointSpec& out)
{
    std::istringstream in(line);
    std::string first;
    if (!(in >> first) || first[0] == '#')
        return false;
    PointSpec point;
    try
    {
        std::size_t used = 0;
        unsigned long const map = std::stoul(first, &used);
        if (used != first.size())
            return false;
        point.mapId = static_cast<std::uint32_t>(map);
    }
    catch (...)
    {
        return false;
    }
    if (!(in >> point.x >> point.y >> point.z))
        return false;
    std::getline(in >> std::ws, point.label);
    while (!point.label.empty() && (point.label.back() == '\r' || point.label.back() == ' '))
        point.label.pop_back();
    out = point;
    return true;
}

struct Counts
{
    std::uint32_t points{0};
    std::uint32_t outdoors{0};
    std::uint32_t indoors{0};
    std::uint32_t withoutMap{0};
    std::uint32_t flyable{0};
    std::uint32_t takeoff{0};  // outdoor, flyable and not in or under water
};

inline void Count(Counts& counts, bool answered, bool outdoor, bool flyable, std::uint32_t liquid)
{
    ++counts.points;
    if (!answered)
    {
        ++counts.withoutMap;
        return;
    }
    ++(outdoor ? counts.outdoors : counts.indoors);
    if (flyable)
        ++counts.flyable;
    if (outdoor && flyable && liquid != LIQUID_IN && liquid != LIQUID_UNDER)
        ++counts.takeoff;
}

inline std::string SummaryLine(Counts const& counts)
{
    return "ROUTE check outdoor: " + std::to_string(counts.points) + " points, " + std::to_string(counts.outdoors) +
           " outdoors, " + std::to_string(counts.indoors) + " indoors, " + std::to_string(counts.withoutMap) +
           " without map; " + std::to_string(counts.flyable) + " in a flying area, " + std::to_string(counts.takeoff) +
           " where a flight may start";
}
}  // namespace RouteCheckOutdoorRules

#endif
