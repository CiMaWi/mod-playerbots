/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteCheckVessel.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "Chat.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "GameTime.h"
#include "Log.h"
#include "MMapMgr.h"
#include "PathGenerator.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RouteCheckVesselRules.h"
#include "Timer.h"
#include "World.h"

namespace
{
using Clock = std::chrono::steady_clock;
namespace Rules = RouteCheckVesselRules;

static_assert(Rules::GROUND == NAV_GROUND && Rules::MAGMA == NAV_MAGMA && Rules::SLIME == NAV_SLIME &&
                  Rules::WATER == NAV_WATER && Rules::GROUND_STEEP == NAV_GROUND_STEEP,
              "the rules' deck flags are NavTerrain's");

constexpr float SURFACE_RADIUS = 1000.0f;   // the deck-place radius that reaches a whole surface
constexpr float ON_MESH_HORIZONTAL = 0.1f;  // yd: a path point is on the mesh when a polygon lies this near
constexpr float ON_MESH_VERTICAL = 0.5f;    // yd: ... horizontally, and its surface this near vertically
constexpr float SEGMENT_MISS = 0.1f;        // yd: a segment leaves the mesh when its ray stops this short
constexpr float ON_DECK_YARDS = 7.0f;       // the vessel search's "near" (PathGenerator::BuildVesselPath)
constexpr float BESIDE_YARDS = 5.0f;        // off-deck point beyond the tile's bounds (the search's box: 3 yd)
constexpr float UNDER_YARDS = 10.0f;        // off-deck point under the tile's lowest point (over the 7 yd)
constexpr float NEAR_EXTENTS[3] = {3.0f, 5.0f, 3.0f};  // the search's two boxes (GetPolyByLocation)
constexpr float FAR_EXTENTS[3] = {3.0f, 50.0f, 3.0f};
constexpr int RAY_POLYS = 256;
constexpr float RAY_NUDGE = 0.05f;  // yd along a segment to the polygon its ray starts from

// The probe's generator, seeded per vessel so a vessel's run is the same alone and in `all`. The probe runs on the
// world thread only.
std::mt19937 rng;

float ProbeRandom() { return static_cast<float>(rng() >> 8) * (1.0f / 16777216.0f); }

// the deck's rule, as PathGenerator::CalculateVesselPath gives a bot aboard, or with `bots` the bots' filter ashore
dtQueryFilterExt DeckFilter(bool bots)
{
    Rules::FilterFlags const flags = Rules::DeckFilter(bots);
    dtQueryFilterExt filter;
    filter.setIncludeFlags(flags.include);
    filter.setExcludeFlags(flags.exclude);
    filter.setAreaCost(NAV_WATER, flags.waterCost);
    return filter;
}

// offsets (x, y, z) against Detour's order (y, z, x)
void ToDetour(G3D::Vector3 const& offset, float* point)
{
    point[0] = offset.y;
    point[1] = offset.z;
    point[2] = offset.x;
}

G3D::Vector3 FromDetour(float const* point) { return G3D::Vector3(point[2], point[0], point[1]); }

struct Counts
{
    std::uint32_t pairs = 0;
    std::uint32_t normal = 0;
    std::uint32_t incomplete = 0;
    std::uint32_t noPath = 0;
    std::uint32_t shortPath = 0;
    std::uint32_t pointsOff = 0;
    std::uint32_t segmentsOff = 0;
    std::uint32_t badPairs = 0;  // a pair that is not a normal path with every point on the mesh
    double micros = 0.0;
    double maxMicros = 0.0;
    float maxMiss = 0.0f;
};

struct Surface
{
    std::uint32_t polys = 0;
    float area = 0.0f;
    float minZ = FLT_MAX;
    float maxZ = -FLT_MAX;
    Counts same;
};

struct Vessel
{
    std::uint32_t displayId = 0;
    std::shared_ptr<dtNavMesh const> mesh;
    MMAP::ManagedNavMeshQuery query;
    dtMeshTile const* tile = nullptr;
    dtPolyRef base = 0;
    std::vector<int> surfaceOf;     // per polygon, -1 when the filter leaves it out
    std::vector<Surface> surfaces;  // largest area first
};

struct PathResult
{
    PathType type = PATHFIND_BLANK;
    float length = 0.0f;
    std::size_t points = 0;
    std::uint32_t pointsOff = 0;
    std::uint32_t segmentsOff = 0;
    float maxMiss = 0.0f;  // yd: the most a segment's ray stops short of the next point
    double micros = 0.0;
};

struct Request
{
    std::vector<std::uint32_t> displayIds;
    bool pointMode = false;
    bool pairsMode = false;
    bool bots = false;
    std::string pairsFile;
    G3D::Vector3 start;
    G3D::Vector3 end;
    std::uint32_t count = 0;
    std::uint32_t seed = 0;
};

float PolyArea(dtMeshTile const* tile, dtPoly const* poly)
{
    float area = 0.0f;
    for (unsigned int i = 0; i < poly->vertCount; ++i)
    {
        float const* a = &tile->verts[poly->verts[i] * 3];
        float const* b = &tile->verts[poly->verts[(i + 1) % poly->vertCount] * 3];
        area += a[0] * b[2] - b[0] * a[2];
    }
    return std::fabs(area) * 0.5f;
}

// The connected surfaces of the mesh among the polygons the filter lets through, by the tile's links
bool Load(Vessel& vessel, dtQueryFilterExt const& filter)
{
    vessel.mesh = MMAP::MMapMgr::GetVesselNavMesh(vessel.displayId);
    if (!vessel.mesh)
        return false;
    vessel.query = MMAP::MMapMgr::CreateNavMeshQuery(vessel.mesh.get());
    vessel.tile = vessel.mesh->getTile(0);
    if (!vessel.query || !vessel.tile || !vessel.tile->header)
        return false;
    vessel.base = vessel.mesh->getPolyRefBase(vessel.tile);

    int const polyCount = vessel.tile->header->polyCount;
    std::vector<int> component(polyCount, -1);
    std::vector<Surface> found;
    for (int first = 0; first < polyCount; ++first)
    {
        dtPoly const* poly = &vessel.tile->polys[first];
        if (component[first] >= 0 || poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION ||
            !filter.passFilter(vessel.base | static_cast<dtPolyRef>(first), vessel.tile, poly))
            continue;
        int const id = static_cast<int>(found.size());
        found.emplace_back();
        std::vector<int> open{first};
        component[first] = id;
        while (!open.empty())
        {
            int const at = open.back();
            open.pop_back();
            dtPoly const* current = &vessel.tile->polys[at];
            Surface& surface = found[id];
            ++surface.polys;
            surface.area += PolyArea(vessel.tile, current);
            for (unsigned int i = 0; i < current->vertCount; ++i)
            {
                float const z = vessel.tile->verts[current->verts[i] * 3 + 1];
                surface.minZ = std::min(surface.minZ, z);
                surface.maxZ = std::max(surface.maxZ, z);
            }
            for (unsigned int link = current->firstLink; link != DT_NULL_LINK; link = vessel.tile->links[link].next)
            {
                dtPolyRef const ref = vessel.tile->links[link].ref;
                if (ref < vessel.base || ref - vessel.base >= static_cast<dtPolyRef>(polyCount))
                    continue;
                int const next = static_cast<int>(ref - vessel.base);
                dtPoly const* nextPoly = &vessel.tile->polys[next];
                if (component[next] >= 0 || nextPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION ||
                    !filter.passFilter(ref, vessel.tile, nextPoly))
                    continue;
                component[next] = id;
                open.push_back(next);
            }
        }
    }

    std::vector<int> order(found.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&found](int a, int b) { return found[a].area > found[b].area; });
    std::vector<int> rank(found.size());
    vessel.surfaces.clear();
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        rank[order[i]] = static_cast<int>(i);
        vessel.surfaces.push_back(found[order[i]]);
    }
    vessel.surfaceOf.assign(polyCount, -1);
    for (int i = 0; i < polyCount; ++i)
        if (component[i] >= 0)
            vessel.surfaceOf[i] = rank[component[i]];
    return true;
}

int SurfaceOf(Vessel const& vessel, dtPolyRef ref)
{
    if (!ref || ref < vessel.base || ref - vessel.base >= vessel.surfaceOf.size())
        return -1;
    return vessel.surfaceOf[ref - vessel.base];
}

// The polygon the vessel search takes for an end (its two boxes) and the distance to it; 0 when there is none
dtPolyRef EndPoly(Vessel const& vessel, dtQueryFilterExt const& filter, G3D::Vector3 const& offset, float& distance)
{
    float point[3];
    ToDetour(offset, point);
    float closest[3] = {0.0f, 0.0f, 0.0f};
    distance = -1.0f;
    float const* const boxes[] = {NEAR_EXTENTS, FAR_EXTENTS};
    for (float const* extents : boxes)
    {
        dtPolyRef ref = 0;
        if (dtStatusSucceed(vessel.query->findNearestPoly(point, extents, &filter, &ref, closest)) && ref)
        {
            distance = dtVdist(point, closest);
            return ref;
        }
    }
    return 0;
}

// "-" for an end the search does not call on the deck, else the surface's number
std::string EndSurface(Vessel const& vessel, dtQueryFilterExt const& filter, G3D::Vector3 const& offset)
{
    float distance = 0.0f;
    dtPolyRef const ref = EndPoly(vessel, filter, offset, distance);
    if (!ref || distance > ON_DECK_YARDS)
        return "-";
    return std::to_string(SurfaceOf(vessel, ref));
}

std::string NearestDeck(Vessel const& vessel, dtQueryFilterExt const& filter, G3D::Vector3 const& offset)
{
    float distance = 0.0f;
    if (!EndPoly(vessel, filter, offset, distance))
        return "none in the boxes";
    return Acore::StringFormat("{:.1f} yd", distance);
}

dtPolyRef PointPoly(Vessel const& vessel, dtQueryFilterExt const& filter, G3D::Vector3 const& offset, bool& onMesh)
{
    float point[3];
    ToDetour(offset, point);
    float const extents[3] = {ON_MESH_HORIZONTAL, ON_MESH_VERTICAL, ON_MESH_HORIZONTAL};
    float closest[3] = {0.0f, 0.0f, 0.0f};
    dtPolyRef ref = 0;
    onMesh = false;
    if (dtStatusFailed(vessel.query->findNearestPoly(point, extents, &filter, &ref, closest)) || !ref)
        return 0;
    float const dx = closest[0] - point[0];
    float const dz = closest[2] - point[2];
    onMesh = dx * dx + dz * dz <= ON_MESH_HORIZONTAL * ON_MESH_HORIZONTAL &&
             std::fabs(closest[1] - point[1]) <= ON_MESH_VERTICAL;
    return ref;
}

PathResult RunPath(Vessel const& vessel, dtQueryFilterExt const& filter, G3D::Vector3 const& start,
                   G3D::Vector3 const& end, Movement::PointsArray& points)
{
    PathResult result;
    points.clear();
    Clock::time_point const started = Clock::now();
    result.type = PathGenerator::FindVesselPath(vessel.mesh.get(), vessel.query.get(), filter, start, end, points);
    result.micros = std::chrono::duration<double, std::micro>(Clock::now() - started).count();
    result.points = points.size();
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        bool onMesh = false;
        dtPolyRef ref = PointPoly(vessel, filter, points[i], onMesh);
        if (!onMesh)
            ++result.pointsOff;
        if (i + 1 >= points.size())
            continue;
        result.length += (points[i + 1] - points[i]).length();
        if (!ref)
            continue;
        // The segment to the next point, cast along the mesh: one that stops short leaves the walkable deck. A point
        // on a polygon's edge belongs to both sides: the ray starts from the polygon a step along the segment.
        float from[3];
        float to[3];
        ToDetour(points[i], from);
        ToDetour(points[i + 1], to);
        float const flat = dtVdist2D(from, to);
        if (flat > RAY_NUDGE)
        {
            G3D::Vector3 const nudged = points[i] + (points[i + 1] - points[i]) * (RAY_NUDGE / flat);
            bool nudgedOnMesh = false;
            dtPolyRef const nudgedRef = PointPoly(vessel, filter, nudged, nudgedOnMesh);
            if (nudgedRef)
                ref = nudgedRef;
        }
        float hit = 0.0f;
        float normal[3] = {0.0f, 0.0f, 0.0f};
        dtPolyRef rayPolys[RAY_POLYS];
        int rayCount = 0;
        if (dtStatusFailed(vessel.query->raycast(ref, from, to, &filter, &hit, normal, rayPolys, &rayCount, RAY_POLYS)))
            continue;
        float const miss = hit != FLT_MAX ? (1.0f - hit) * flat : 0.0f;
        if (miss > SEGMENT_MISS)
            ++result.segmentsOff;
        result.maxMiss = std::max(result.maxMiss, miss);
    }
    return result;
}

std::string ClassName(PathType type)
{
    std::string name;
    if (type & PATHFIND_NOPATH)
        name = "nopath";
    else if (type & PATHFIND_INCOMPLETE)
        name = "incomplete";
    else if (type & PATHFIND_NORMAL)
        name = "normal";
    else
        name = "type" + std::to_string(static_cast<std::uint32_t>(type));
    if (type & PATHFIND_SHORT)
        name += "+short";
    return name;
}

void Add(Counts& counts, PathResult const& result)
{
    ++counts.pairs;
    if (result.type & PATHFIND_NOPATH)
        ++counts.noPath;
    else if (result.type & PATHFIND_INCOMPLETE)
        ++counts.incomplete;
    else if (result.type & PATHFIND_NORMAL)
        ++counts.normal;
    if (result.type & PATHFIND_SHORT)
        ++counts.shortPath;
    counts.pointsOff += result.pointsOff;
    counts.segmentsOff += result.segmentsOff;
    bool const good = (result.type & PATHFIND_NORMAL) && !(result.type & (PATHFIND_INCOMPLETE | PATHFIND_NOPATH)) &&
                      !result.pointsOff;
    if (!good)
        ++counts.badPairs;
    counts.micros += result.micros;
    counts.maxMicros = std::max(counts.maxMicros, result.micros);
    counts.maxMiss = std::max(counts.maxMiss, result.maxMiss);
}

std::string CountsText(Counts const& counts)
{
    return Acore::StringFormat(
        "{} pairs: normal {}, incomplete {}, no path {}, short {}; points off the mesh {}, segments leaving it {} (at "
        "most {:.2f} yd "
        "short); {:.1f} us mean, {:.1f} us max",
        counts.pairs, counts.normal, counts.incomplete, counts.noPath, counts.shortPath, counts.pointsOff,
        counts.segmentsOff, counts.maxMiss, counts.pairs ? counts.micros / counts.pairs : 0.0, counts.maxMicros);
}

std::string Offset(G3D::Vector3 const& offset)
{
    return Acore::StringFormat("{:.2f}\t{:.2f}\t{:.2f}", offset.x, offset.y, offset.z);
}

std::string Row(std::uint32_t displayId, char const* kind, std::uint32_t index, Vessel const& vessel,
                dtQueryFilterExt const& filter, G3D::Vector3 const& start, G3D::Vector3 const& end,
                PathResult const& result)
{
    return Acore::StringFormat("{:05}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:.2f}\t{}\t{}\t{}\t{:.2f}\t{:.1f}", displayId, kind,
                               index, Offset(start), Offset(end), EndSurface(vessel, filter, start),
                               EndSurface(vessel, filter, end), ClassName(result.type), result.length, result.points,
                               result.pointsOff, result.segmentsOff, result.maxMiss, result.micros);
}

std::string const PREFIX = "ROUTE check vessel ";

void ProbePoints(Request const& request, dtQueryFilterExt const& filter, std::vector<std::string>& lines)
{
    Vessel vessel;
    vessel.displayId = request.displayIds.front();
    if (!Load(vessel, filter))
    {
        lines.push_back(Acore::StringFormat("{}{:05}: no mesh", PREFIX, vessel.displayId));
        return;
    }
    Movement::PointsArray points;
    PathResult const result = RunPath(vessel, filter, request.start, request.end, points);
    lines.push_back(Acore::StringFormat(
        "{}{:05} ({} filter): {}, {:.2f} yd, {} points; start surface {} (nearest deck {}), end surface {} (nearest "
        "deck {}); points off the mesh {}, segments leaving it {} (at most {:.2f} yd short); {:.1f} us",
        PREFIX, vessel.displayId, Rules::FilterName(request.bots), ClassName(result.type), result.length, result.points,
        EndSurface(vessel, filter, request.start), NearestDeck(vessel, filter, request.start),
        EndSurface(vessel, filter, request.end), NearestDeck(vessel, filter, request.end), result.pointsOff,
        result.segmentsOff, result.maxMiss, result.micros));
    // the corridor as the search sees it: Detour's answer between the two ends' polygons (the search's boxes)
    float startDistance = 0.0f;
    float endDistance = 0.0f;
    dtPolyRef const startRef = EndPoly(vessel, filter, request.start, startDistance);
    dtPolyRef const endRef = EndPoly(vessel, filter, request.end, endDistance);
    if (startRef && endRef)
    {
        float from[3];
        float to[3];
        ToDetour(request.start, from);
        ToDetour(request.end, to);
        dtPolyRef corridor[MAX_PATH_LENGTH];
        int corridorLength = 0;
        dtStatus const status =
            vessel.query->findPath(startRef, endRef, from, to, &filter, corridor, &corridorLength, MAX_PATH_LENGTH);
        float straight[MAX_POINT_PATH_LENGTH * 3];
        int straightCount = 0;
        if (corridorLength)
            vessel.query->findStraightPath(from, to, corridor, corridorLength, straight, nullptr, nullptr,
                                           &straightCount, MAX_POINT_PATH_LENGTH);
        lines.push_back(Acore::StringFormat(
            "{}corridor: {} {} polygons (limit {}), {}{}{}, ends at the end's polygon {}; straight path {} points; "
            "{:.2f} yd between the ends",
            PREFIX, dtStatusSucceed(status) ? "found" : "failed", corridorLength, MAX_PATH_LENGTH,
            dtStatusDetail(status, DT_PARTIAL_RESULT) ? "partial" : "whole",
            dtStatusDetail(status, DT_OUT_OF_NODES) ? ", out of nodes" : "",
            dtStatusDetail(status, DT_BUFFER_TOO_SMALL) ? ", buffer too small" : "",
            corridorLength && corridor[corridorLength - 1] == endRef ? "yes" : "no", straightCount,
            (request.end - request.start).length()));
    }
    std::string text;
    for (G3D::Vector3 const& point : points)
        text += Acore::StringFormat(" ({:.2f} {:.2f} {:.2f})", point.x, point.y, point.z);
    if (!text.empty())
        lines.push_back(PREFIX + "points:" + text);
}

// One vessel's random run; false when the vessel fails the pass rule
bool ProbeRandomPairs(std::uint32_t displayId, Request const& request, dtQueryFilterExt const& filter,
                      std::vector<std::string>& lines, std::vector<std::string>& rows, bool& hasMesh)
{
    Vessel vessel;
    vessel.displayId = displayId;
    hasMesh = Load(vessel, filter);
    if (!hasMesh)
    {
        lines.push_back(Acore::StringFormat("{}{:05}: no mesh", PREFIX, displayId));
        return true;
    }

    rng.seed(request.seed * 1000003u + displayId);
    Counts same;
    Counts cross;
    Counts off;
    std::uint32_t noRandomPoint = 0;
    G3D::Vector3 anchor;
    bool haveAnchor = false;
    Movement::PointsArray points;
    for (std::uint32_t i = 0; i < request.count; ++i)
    {
        dtPolyRef refA = 0;
        float pointA[3];
        G3D::Vector3 b;
        if (dtStatusFailed(vessel.query->findRandomPoint(&filter, ProbeRandom, &refA, pointA)) || !refA ||
            !PathGenerator::FindVesselRandomPoint(vessel.query.get(), filter, FromDetour(pointA), SURFACE_RADIUS, b,
                                                  ProbeRandom))
        {
            ++noRandomPoint;
            continue;
        }
        G3D::Vector3 const a = FromDetour(pointA);
        if (!haveAnchor)
        {
            anchor = a;
            haveAnchor = true;
        }
        int const surfaceA = SurfaceOf(vessel, refA);
        PathResult const result = RunPath(vessel, filter, a, b, points);
        Add(same, result);
        if (surfaceA >= 0)
            Add(vessel.surfaces[surfaceA].same, result);
        rows.push_back(Row(displayId, "same", i, vessel, filter, a, b, result));

        dtPolyRef refC = 0;
        float pointC[3];
        if (dtStatusFailed(vessel.query->findRandomPoint(&filter, ProbeRandom, &refC, pointC)) || !refC ||
            SurfaceOf(vessel, refC) == surfaceA)
            continue;
        G3D::Vector3 const c = FromDetour(pointC);
        PathResult const crossResult = RunPath(vessel, filter, a, c, points);
        Add(cross, crossResult);
        rows.push_back(Row(displayId, "cross", i, vessel, filter, a, c, crossResult));
    }

    // Off the deck: beside the hull, beyond the tile's bounds, and under the tile's lowest point; as end and start
    std::string offText = "no deck point";
    if (haveAnchor)
    {
        float const* bmin = vessel.tile->header->bmin;
        float const* bmax = vessel.tile->header->bmax;
        G3D::Vector3 const beside(bmax[2] + BESIDE_YARDS, anchor.y, anchor.z);
        G3D::Vector3 const under(anchor.x, anchor.y, bmin[1] - UNDER_YARDS);
        std::uint32_t index = 0;
        for (G3D::Vector3 const& place : {beside, under})
        {
            PathResult const toPlace = RunPath(vessel, filter, anchor, place, points);
            Add(off, toPlace);
            rows.push_back(Row(displayId, "off-end", index, vessel, filter, anchor, place, toPlace));
            PathResult const fromPlace = RunPath(vessel, filter, place, anchor, points);
            Add(off, fromPlace);
            rows.push_back(Row(displayId, "off-start", index, vessel, filter, place, anchor, fromPlace));
            ++index;
        }
        offText = Acore::StringFormat(
            "no path {} of {} (beside: {} yd past the bounds, nearest deck {}; under: {} yd "
            "below the lowest point, nearest deck {})",
            off.noPath, off.pairs, BESIDE_YARDS, NearestDeck(vessel, filter, beside), UNDER_YARDS,
            NearestDeck(vessel, filter, under));
    }

    bool const pass = haveAnchor && !same.badPairs && off.noPath == off.pairs && !noRandomPoint;
    lines.push_back(Acore::StringFormat(
        "{}{:05}: {} surfaces, {} polygons; same surface {} | across surfaces {} | "
        "off the deck: {}{} | {}",
        PREFIX, displayId, vessel.surfaces.size(), vessel.tile->header->polyCount, CountsText(same), CountsText(cross),
        offText, noRandomPoint ? Acore::StringFormat(" | {} draws without a point", noRandomPoint) : std::string(),
        pass ? "PASS" : "FAIL"));
    for (std::size_t i = 0; i < vessel.surfaces.size(); ++i)
    {
        Surface const& surface = vessel.surfaces[i];
        lines.push_back(Acore::StringFormat("{}{:05} surface {}: {} polygons, {:.1f} yd2, z {:.2f} to {:.2f}; {}",
                                            PREFIX, displayId, i, surface.polys, surface.area, surface.minZ,
                                            surface.maxZ, CountsText(surface.same)));
    }
    return pass;
}

void ProbeRandomRun(Request const& request, dtQueryFilterExt const& filter, std::vector<std::string>& lines)
{
    Clock::time_point const started = Clock::now();
    std::vector<std::string> rows;
    std::vector<std::string> failed;
    std::uint32_t meshes = 0;
    for (std::uint32_t displayId : request.displayIds)
    {
        bool hasMesh = false;
        if (!ProbeRandomPairs(displayId, request, filter, lines, rows, hasMesh))
            failed.push_back(Acore::StringFormat("{:05}", displayId));
        if (hasMesh)
            ++meshes;
    }

    std::string const stamp = Acore::Time::TimeToTimestampStr(GameTime::GetGameTime(), "%Y%m%d-%H%M%S");
    std::filesystem::path const file = std::filesystem::path(sLog->GetLogsDir()) /
                                       ("route-check-vessel-" + stamp + (request.bots ? "-bots" : "") + ".tsv");
    std::ofstream out(file, std::ios::binary);
    if (out)
    {
        out << "# route check vessel, " << Rules::FilterName(request.bots) << " filter, seed " << request.seed << ", "
            << request.count << " pairs per vessel; navmesh " << sWorld->GetDataPath()
            << "mmaps/vessels; offsets in yd; surface - = "
            << "no deck polygon within " << ON_DECK_YARDS << " yd; a point is on the mesh within " << ON_MESH_HORIZONTAL
            << " yd horizontally and " << ON_MESH_VERTICAL << " yd vertically\n";
        out << "display\tkind\tpair\tsx\tsy\tsz\tex\tey\tez\tsurface_start\tsurface_end\tclass\tlength\tpoints"
            << "\tpoints_off\tsegments_off\tmax_miss\tmicros\n";
        for (std::string const& row : rows)
            out << row << "\n";
    }
    std::string failedText;
    for (std::string const& id : failed)
        failedText += " " + id;
    lines.push_back(Acore::StringFormat(
        "{}done ({} filter): {} display ids, {} with a mesh, {} fail{}; seed {}, {} pairs each; {:.1f} ms; report {}",
        PREFIX, Rules::FilterName(request.bots), request.displayIds.size(), meshes, failed.size(),
        failed.empty() ? std::string() : ":" + failedText, request.seed, request.count,
        std::chrono::duration<double, std::milli>(Clock::now() - started).count(),
        out ? file.string() : "not written"));
}

// Every pair of a file, as the two-offset form answers one; a row per pair (its id first) and the gap between the
// path's last point and the requested end, to <LogsDir>/route-check-vessel-pairs-<time>-<filter>.tsv
void ProbePairsRun(Request const& request, dtQueryFilterExt const& filter, std::vector<std::string>& lines)
{
    Clock::time_point const started = Clock::now();
    std::ifstream in(request.pairsFile, std::ios::binary);
    if (!in)
    {
        lines.push_back(Acore::StringFormat("{}pairs: cannot read {}", PREFIX, request.pairsFile));
        return;
    }
    std::map<std::uint32_t, std::unique_ptr<Vessel>> vessels;
    std::map<std::uint32_t, Counts> counts;
    std::map<Rules::LineKind, std::uint32_t> kinds;
    std::vector<std::string> rows;
    std::uint32_t noMesh = 0;
    std::uint32_t overLimit = 0;
    std::string const filterName = Rules::FilterName(request.bots);
    Movement::PointsArray points;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(in, line))
    {
        ++lineNumber;
        Rules::PairLine pair;
        Rules::LineKind const kind = Rules::ParsePairLine(line, lineNumber, pair);
        ++kinds[kind];
        if (kind != Rules::LineKind::Pair)
            continue;
        if (rows.size() >= Rules::MAX_FILE_PAIRS)
        {
            ++overLimit;
            continue;
        }
        auto found = vessels.find(pair.displayId);
        if (found == vessels.end())
        {
            std::unique_ptr<Vessel> vessel = std::make_unique<Vessel>();
            vessel->displayId = pair.displayId;
            if (!Load(*vessel, filter))
                vessel.reset();
            found = vessels.emplace(pair.displayId, std::move(vessel)).first;
        }
        G3D::Vector3 const start(pair.offsets[0], pair.offsets[1], pair.offsets[2]);
        G3D::Vector3 const end(pair.offsets[3], pair.offsets[4], pair.offsets[5]);
        std::string const head = pair.id + "\t" + filterName + "\t";
        if (!found->second)
        {
            ++noMesh;
            rows.push_back(head + Acore::StringFormat("{:05}\tpair\t{}\t{}\t{}\t-\t-\tnomesh\t-\t-\t-\t-\t-\t-\t-",
                                                      pair.displayId, lineNumber, Offset(start), Offset(end)));
            continue;
        }
        Vessel const& vessel = *found->second;
        PathResult const result = RunPath(vessel, filter, start, end, points);
        Add(counts[pair.displayId], result);
        float const last[3] = {points.empty() ? 0.0f : points.back().x, points.empty() ? 0.0f : points.back().y,
                               points.empty() ? 0.0f : points.back().z};
        float const gap = Rules::EndGap(points.size(), last, &pair.offsets[3]);
        rows.push_back(
            head +
            Row(pair.displayId, "pair", static_cast<std::uint32_t>(lineNumber), vessel, filter, start, end, result) +
            (gap < 0.0f ? std::string("\t-") : Acore::StringFormat("\t{:.2f}", gap)));
    }

    std::string const stamp = Acore::Time::TimeToTimestampStr(GameTime::GetGameTime(), "%Y%m%d-%H%M%S");
    std::filesystem::path const file =
        std::filesystem::path(sLog->GetLogsDir()) / ("route-check-vessel-pairs-" + stamp + "-" + filterName + ".tsv");
    std::ofstream out(file, std::ios::binary);
    if (out)
    {
        out << "# route check vessel pairs, " << filterName << " filter, from " << request.pairsFile << "; navmesh "
            << sWorld->GetDataPath() << "mmaps/vessels; offsets in yd; surface - = no deck polygon within "
            << ON_DECK_YARDS << " yd; end_gap = yd from the path's last point to the requested end (- no point)\n";
        out << "id\tfilter\tdisplay\tkind\tpair\tsx\tsy\tsz\tex\tey\tez\tsurface_start\tsurface_end\tclass\tlength"
            << "\tpoints\tpoints_off\tsegments_off\tmax_miss\tmicros\tend_gap\n";
        for (std::string const& row : rows)
            out << row << "\n";
    }
    for (auto const& [displayId, displayCounts] : counts)
        lines.push_back(Acore::StringFormat("{}{:05} pairs ({} filter): {}", PREFIX, displayId, filterName,
                                            CountsText(displayCounts)));
    std::string kindText;
    for (auto const& [kind, number] : kinds)
        if (kind != Rules::LineKind::Pair)
            kindText += Acore::StringFormat(", {} {}", number, Rules::LineKindName(kind));
    lines.push_back(Acore::StringFormat(
        "{}pairs done ({} filter): {} lines, {} pairs{}; {} without a mesh, {} over the limit of {}; {:.1f} ms; "
        "report {}",
        PREFIX, filterName, lineNumber, kinds[Rules::LineKind::Pair], kindText, noMesh, overLimit,
        Rules::MAX_FILE_PAIRS, std::chrono::duration<double, std::milli>(Clock::now() - started).count(),
        out ? file.string() : "not written"));
}

std::vector<std::string> RunProbe(Request const& request)
{
    dtQueryFilterExt const filter = DeckFilter(request.bots);
    std::vector<std::string> lines;
    if (request.pairsMode)
        ProbePairsRun(request, filter, lines);
    else if (request.pointMode)
        ProbePoints(request, filter, lines);
    else
        ProbeRandomRun(request, filter, lines);
    return lines;
}

class VesselProbeOperation final : public PlayerbotOperation
{
public:
    explicit VesselProbeOperation(Request request) : _request(std::move(request)) {}

    bool Execute() override
    {
        for (std::string const& line : RunProbe(_request))
            LOG_INFO("playerbots", "{}", line);
        return true;
    }

    std::string GetName() const override { return "route check vessel"; }

private:
    Request _request;
};

// the display ids of the dataset's vessel files (<5 digits>.mmtile), in order
std::vector<std::uint32_t> DatasetVessels()
{
    std::vector<std::uint32_t> ids;
    std::error_code error;
    std::filesystem::path const folder = std::filesystem::path(sWorld->GetDataPath()) / "mmaps" / "vessels";
    for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(folder, error))
    {
        std::string const name = entry.path().filename().string();
        std::uint32_t id = 0;
        if (name.size() == 12 && name.substr(5) == ".mmtile" && Rules::ParseUInt(name.substr(0, 5), id))
            ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}
}  // namespace

bool RouteCheckVessel::HandleVesselCommand(ChatHandler* handler, char const* args)
{
    Rules::Command const command = Rules::ParseCommand(Rules::Split(args ? args : ""));
    if (command.form == Rules::Form::Bad)
    {
        handler->SendErrorMessage(
            "Usage: .playerbots route check vessel <display id> <x1 y1 z1 x2 y2 z2> | <display id>|all <count 1-{}> "
            "<seed> | pairs <file> (console only), each with a last word bots for the bots' filter ashore (default: "
            "the deck's rule); points are passenger offsets",
            Rules::MAX_PAIRS);
        return false;
    }
    Request request;
    request.bots = command.bots;
    request.count = command.count;
    request.seed = command.seed;
    request.start = G3D::Vector3(command.offsets[0], command.offsets[1], command.offsets[2]);
    request.end = G3D::Vector3(command.offsets[3], command.offsets[4], command.offsets[5]);
    request.pointMode = command.form == Rules::Form::Points;
    request.pairsMode = command.form == Rules::Form::Pairs;
    request.pairsFile = command.file;
    if (request.pairsMode)
    {
        // it reads a file the caller names: the server's console only
        if (handler->GetSession())
        {
            handler->SendErrorMessage("ROUTE check vessel pairs: the server's console only");
            return false;
        }
        for (std::string const& line : RunProbe(request))
            handler->PSendSysMessage("{}", line);
        return true;
    }
    if (command.all)
        request.displayIds = DatasetVessels();
    else
        request.displayIds.push_back(command.displayId);
    if (request.displayIds.empty())
    {
        handler->SendErrorMessage("ROUTE check vessel: no vessel file under {}mmaps/vessels", sWorld->GetDataPath());
        return false;
    }

    // a session's command runs on its map's thread: the probe goes to the world thread and writes to the log
    if (handler->GetSession())
    {
        if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(
                std::make_unique<VesselProbeOperation>(std::move(request))))
        {
            handler->SendErrorMessage("ROUTE check vessel: the world thread's queue is full");
            return false;
        }
        handler->PSendSysMessage("ROUTE check vessel: queued; the result goes to the server log");
        return true;
    }

    for (std::string const& line : RunProbe(request))
        handler->PSendSysMessage("{}", line);
    return true;
}
