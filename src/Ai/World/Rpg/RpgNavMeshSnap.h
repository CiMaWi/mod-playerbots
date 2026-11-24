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
#include <utility>
#include <vector>

#include "Map.h"
#include "MapCollisionData.h"
#include "ModelIgnoreFlags.h"
#include "PathGenerator.h"
#include "StandRules.h"
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
inline bool PieceIsClosed(dtNavMesh const* mesh, dtQueryFilter const& filter, dtPolyRef start);

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
    return PieceIsClosed(mesh, filter, start);
}

// IsFragment's flood from a known polygon: its connected piece under `filter` has fewer than FRAGMENT_POLYS polygons
inline bool PieceIsClosed(dtNavMesh const* mesh, dtQueryFilter const& filter, dtPolyRef start)
{
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

// The sight test: from a bot's eye over the stand point to the target's body low and high (a fire in a hearth, a head
// over a counter), through walls, floors and terrain only: doodads (counters, teller grilles, furniture) do not hide a
// target, and the core's own interaction check asks no sight at all
constexpr float SIGHT_EYE_YARDS = 2.0f;
constexpr std::array<float, 2> SIGHT_TARGET_YARDS{0.5f, 2.0f};

struct Stand
{
    Result result{Result::Unknown};  // Found: the rule's point; None: see answer.why; Unknown: no tile at the target
    StandRules::Answer answer;
    float x{0.0f};  // the point on Found, and on a fragment the closed piece's nearest point
    float y{0.0f};
    float z{0.0f};
};

// The mesh half of the stand point (pathing PA.16; the rule is StandRules::Pick): samples rings around the target
// (StandRules::Rings, RING_ANGLES each, the first straight in front), snaps each onto a polygon the bots may walk,
// keeps the points in the band and gives each its facts: joined (not a closed piece, PieceIsClosed, one flood per
// polygon), in sight (SIGHT_TARGET_YARDS), occupied (an occupant within BODY_YARDS). Sight is asked side by side in the
// rule's order and stops at the first side with a free point in sight, since the rule picks within that side alone.
// Once per approach or walk, on the map's update thread or the world thread outside the map updates; never per tick
inline Stand StandPoint(Map* map, StandRules::Target const& target, uint32 phaseMask,
                        std::vector<std::array<float, 3>> const& occupants, uint64 key)
{
    Stand stand;
    if (!map)
        return stand;
    MMapData& mmap = map->GetMapCollisionData().GetMMapData();
    dtNavMesh const* const mesh = mmap.GetNavMesh();
    dtNavMeshQuery const* const query = mmap.GetNavMeshQuery();
    if (!mesh || !query)
        return stand;
    float const centre[3] = {target.y, target.z, target.x};
    int tileX = 0;
    int tileY = 0;
    mesh->calcTileLoc(centre, &tileX, &tileY);
    if (!mesh->getTileAt(tileX, tileY, 0))
        return stand;
    stand.result = Result::None;

    dtQueryFilter filter;
    filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
    filter.setExcludeFlags(TravelWalk::BOT_EXCLUDE_FLAGS);
    float const outer = StandRules::OuterYards(target);
    float const across = StandRules::RING_STEP_YARDS * 0.5f;
    float const extents[3] = {across, outer, across};
    std::vector<StandRules::Candidate> candidates;
    std::vector<StandRules::Side> sides;
    std::vector<std::pair<dtPolyRef, bool>> closed;  // the floods asked so far
    for (float const ring : StandRules::Rings(target.minYards, outer))
    {
        for (int step = 0; step < StandRules::RING_ANGLES; ++step)
        {
            float const angle = target.facing + 2.0f * StandRules::PI * static_cast<float>(step) /
                                                    static_cast<float>(StandRules::RING_ANGLES);
            float const sample[3] = {target.y + ring * std::sin(angle), target.z, target.x + ring * std::cos(angle)};
            dtPolyRef poly = 0;
            float nearest[3] = {0.0f, 0.0f, 0.0f};
            if (dtStatusFailed(query->findNearestPoly(sample, extents, &filter, &poly, nearest)) || !poly)
                continue;
            StandRules::Candidate candidate;
            candidate.x = nearest[2];
            candidate.y = nearest[0];
            candidate.z = nearest[1];
            if (!StandRules::InBand(target, candidate.x, candidate.y, candidate.z))
                continue;
            bool known = false;
            for (StandRules::Candidate const& other : candidates)
                known = known || StandRules::Distance(other.x, other.y, other.z, candidate.x, candidate.y,
                                                      candidate.z) < StandRules::SAME_POINT_YARDS;
            if (known)
                continue;
            auto const flood =
                std::find_if(closed.begin(), closed.end(),
                             [poly](std::pair<dtPolyRef, bool> const& entry) { return entry.first == poly; });
            bool const isClosed = flood != closed.end()
                                      ? flood->second
                                      : closed.emplace_back(poly, PieceIsClosed(mesh, filter, poly)).second;
            candidate.joined = !isClosed;
            for (std::array<float, 3> const& occupant : occupants)
                candidate.occupied =
                    candidate.occupied || StandRules::Distance(occupant[0], occupant[1], occupant[2], candidate.x,
                                                               candidate.y, candidate.z) < StandRules::BODY_YARDS;
            candidates.push_back(candidate);
            sides.push_back(StandRules::SideOf(target, candidate.x, candidate.y));
        }
    }
    for (StandRules::Side const side : {StandRules::Side::Front, StandRules::Side::Beside, StandRules::Side::Behind})
    {
        bool served = false;
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            StandRules::Candidate& candidate = candidates[i];
            if (sides[i] != side || !candidate.joined)
                continue;
            for (float const height : SIGHT_TARGET_YARDS)
                candidate.inSight =
                    candidate.inSight || map->isInLineOfSight(candidate.x, candidate.y, candidate.z + SIGHT_EYE_YARDS,
                                                              target.x, target.y, target.z + height, phaseMask,
                                                              LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::M2);
            served = served || (candidate.inSight && !candidate.occupied);
        }
        if (served)
            break;
    }
    stand.answer = StandRules::Pick(target, candidates, key);
    if (stand.answer.why == StandRules::Why::Found || stand.answer.why == StandRules::Why::Fragment)
    {
        stand.x = candidates[stand.answer.index].x;
        stand.y = candidates[stand.answer.index].y;
        stand.z = candidates[stand.answer.index].z;
    }
    if (stand.answer.why == StandRules::Why::Found)
        stand.result = Result::Found;
    return stand;
}
}  // namespace NavMeshSnap

#endif
