/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_RPGNAVMESHSNAP_H
#define _PLAYERBOT_RPGNAVMESHSNAP_H

#include "Map.h"
#include "MapCollisionData.h"
#include "PathGenerator.h"

// The nearest nav mesh poly a bot may walk (the bots' path filter: ground or water, no magma, slime or steep ground)
// around a point: grind spots and level teleport spots snap onto it, a stuck bot finds out whether it stands off the
// mesh. It uses the map's own nav mesh query, which is not thread safe: call it on the map's update thread, or on the
// world thread outside the map updates.
namespace NavMeshSnap
{
enum class Result : uint8
{
    Unknown,  // the map has no nav mesh or the point's tile is not loaded: nothing can be judged
    Found,    // x, y, z now hold the nearest point on that poly
    None      // no walkable poly within the box
};

// Searches `across` yd to each side and `vertical` yd up and down; `include` NAV_GROUND alone leaves water out
inline Result Nearest(Map* map, float& x, float& y, float& z, float across, float vertical,
                      uint16 include = NAV_GROUND | NAV_WATER)
{
    if (!map)
        return Result::Unknown;

    MMapData& mmap = map->GetMapCollisionData().GetMMapData();
    dtNavMesh const* mesh = mmap.GetNavMesh();
    dtNavMeshQuery const* query = mmap.GetNavMeshQuery();
    if (!mesh || !query)
        return Result::Unknown;

    float const point[3] = {y, z, x};
    int tileX = 0;
    int tileY = 0;
    mesh->calcTileLoc(point, &tileX, &tileY);
    if (!mesh->getTileAt(tileX, tileY, 0))
        return Result::Unknown;

    dtQueryFilter filter;
    filter.setIncludeFlags(include);
    filter.setExcludeFlags(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
    float const extents[3] = {across, vertical, across};
    dtPolyRef poly = 0;
    float nearest[3] = {0.0f, 0.0f, 0.0f};
    if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &poly, nearest)) || !poly)
        return Result::None;

    x = nearest[2];
    y = nearest[0];
    z = nearest[1];
    return Result::Found;
}
}  // namespace NavMeshSnap

#endif
