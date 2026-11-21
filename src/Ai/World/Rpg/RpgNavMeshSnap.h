/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_RPGNAVMESHSNAP_H
#define _PLAYERBOT_RPGNAVMESHSNAP_H

#include <algorithm>
#include <array>
#include <cmath>

#include "Map.h"
#include "MapCollisionData.h"
#include "PathGenerator.h"
#include "TravelWalkTypes.h"
#include "WorldTravelRules.h"

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
    filter.setExcludeFlags(TravelWalk::BOT_EXCLUDE_FLAGS);
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
// The place a typed approach is satisfied (NewRpgBaseAction::Approach accepts a stand point within its distance of the
// object, 3D): the nearest point of a polygon the bots may walk within `reach` yd of the target, found under the bots'
// filter. x, y, z hold the target and, on Found, that point. None: no such polygon (see ClassifyMiss for why)
inline Result StandPoint(Map* map, float& x, float& y, float& z, float reach)
{
    float const targetX = x;
    float const targetY = y;
    float const targetZ = z;
    Result const found = Nearest(map, x, y, z, reach, reach);
    if (found != Result::Found)
        return found;
    float const dx = x - targetX;
    float const dy = y - targetY;
    float const dz = z - targetZ;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) <= reach)
        return Result::Found;
    x = targetX;
    y = targetY;
    z = targetZ;
    return Result::None;
}

// Why StandPoint found nothing: steep ground (a polygon the bots' filter leaves out, area NAV_GROUND_STEEP) within
// `reach` in 3D, or a polygon the bots may walk within `reach` horizontally on another level (`tallVertical` yd up and
// down). Read only for a target StandPoint answered None
struct Miss
{
    bool steep{false};
    bool otherFloor{false};
};

inline Miss ClassifyMiss(Map* map, float x, float y, float z, float reach, float tallVertical)
{
    Miss miss;
    if (!map)
        return miss;
    MMapData& mmap = map->GetMapCollisionData().GetMMapData();
    dtNavMesh const* mesh = mmap.GetNavMesh();
    dtNavMeshQuery const* query = mmap.GetNavMeshQuery();
    if (!mesh || !query)
        return miss;

    float const point[3] = {y, z, x};
    dtQueryFilter steepOk;
    steepOk.setIncludeFlags(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP);
    steepOk.setExcludeFlags(static_cast<uint16>(TravelWalk::BOT_EXCLUDE_FLAGS & ~NAV_GROUND_STEEP));
    float const box[3] = {reach, reach, reach};
    dtPolyRef poly = 0;
    float closest[3] = {0.0f, 0.0f, 0.0f};
    dtMeshTile const* tile = nullptr;
    dtPoly const* data = nullptr;
    if (dtStatusSucceed(query->findNearestPoly(point, box, &steepOk, &poly, closest)) && poly &&
        dtStatusSucceed(mesh->getTileAndPolyByRef(poly, &tile, &data)) && data && data->getArea() == NAV_GROUND_STEEP &&
        std::sqrt((closest[0] - y) * (closest[0] - y) + (closest[1] - z) * (closest[1] - z) +
                  (closest[2] - x) * (closest[2] - x)) <= reach)
        miss.steep = true;

    dtQueryFilter bots;
    bots.setIncludeFlags(NAV_GROUND | NAV_WATER);
    bots.setExcludeFlags(TravelWalk::BOT_EXCLUDE_FLAGS);
    float const tall[3] = {reach, tallVertical, reach};
    poly = 0;
    if (dtStatusSucceed(query->findNearestPoly(point, tall, &bots, &poly, closest)) && poly)
    {
        float const dx = closest[2] - x;
        float const dy = closest[0] - y;
        miss.otherFloor = std::sqrt(dx * dx + dy * dy) <= reach;
    }
    return miss;
}

// The polygons a navmesh fragment has at most (IsFragment): a cheap bound on the flood, once per walk off the graph
constexpr std::size_t FRAGMENT_POLYS = 16;

// A point on a navmesh fragment: the nearest polygon a bot may walk (Nearest's filter, the off-mesh box) belongs to
// a connected piece of fewer than FRAGMENT_POLYS polygons, linked to nothing else. The extractor leaves such pieces
// where it cut walkable floor apart (the polygons of a hall floor with no neighbours, WT56): the bots walk there, but
// no path leaves such a piece, so the navmesh cannot judge a walk from or to it. False where no polygon is in the box
// (the bot is off the mesh: the follower's off-mesh recovery) or the map has no navmesh
inline bool IsFragment(Map* map, float x, float y, float z)
{
    MMapData& mmap = map->GetMapCollisionData().GetMMapData();
    dtNavMesh const* const mesh = mmap.GetNavMesh();
    dtNavMeshQuery const* const query = mmap.GetNavMeshQuery();
    if (!mesh || !query)
        return false;
    dtQueryFilter filter;
    filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
    filter.setExcludeFlags(TravelWalk::BOT_EXCLUDE_FLAGS);
    float const point[3] = {y, z, x};
    float const extents[3] = {WorldTravel::OFF_MESH_ACROSS_YARDS, WorldTravel::OFF_MESH_VERTICAL_YARDS,
                              WorldTravel::OFF_MESH_ACROSS_YARDS};
    dtPolyRef start = 0;
    float nearest[3] = {0.0f, 0.0f, 0.0f};
    if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &start, nearest)) || !start)
        return false;
    std::array<dtPolyRef, FRAGMENT_POLYS> piece{start};
    std::size_t count = 1;
    for (std::size_t i = 0; i < count; ++i)
    {
        dtMeshTile const* tile = nullptr;
        dtPoly const* poly = nullptr;
        mesh->getTileAndPolyByRefUnsafe(piece[i], &tile, &poly);
        for (unsigned int link = poly->firstLink; link != DT_NULL_LINK; link = tile->links[link].next)
        {
            dtPolyRef const neighbour = tile->links[link].ref;
            if (!neighbour || std::find(piece.begin(), piece.begin() + count, neighbour) != piece.begin() + count)
                continue;
            dtMeshTile const* nextTile = nullptr;
            dtPoly const* nextPoly = nullptr;
            mesh->getTileAndPolyByRefUnsafe(neighbour, &nextTile, &nextPoly);
            if (!filter.passFilter(neighbour, nextTile, nextPoly))
                continue;
            if (count == FRAGMENT_POLYS)
                return false;
            piece[count++] = neighbour;
        }
    }
    return true;
}
}  // namespace NavMeshSnap

#endif
