/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteCheckProbe.h"

#include <chrono>

#include "DetourNavMeshQuery.h"
#include "GoToRules.h"
#include "Map.h"
#include "MapDefines.h"
#include "Object.h"
#include "PathGenerator.h"
#include "StringFormat.h"
#include "TravelLegRules.h"
#include "TravelWalkRules.h"

namespace
{
// The polygon at a Detour point (y, z, x) in the engine's two search boxes (PathGenerator::GetPolyByLocation), 0 for
// none: which end the engine's straight line (PATH_NOT_USING_PATH) lacks a polygon at
dtPolyRef PolyAt(dtNavMeshQuery const* query, dtQueryFilter const& filter, float const* point)
{
    dtPolyRef ref = 0;
    float nearest[3] = {0.0f, 0.0f, 0.0f};
    for (float const up : {5.0f, 50.0f})
    {
        float const extents[3] = {3.0f, up, 3.0f};
        if (dtStatusSucceed(query->findNearestPoly(point, extents, &filter, &ref, nearest)) && ref)
            break;
    }
    return ref;
}

// One probe step's engine call, as NewRpgTravelWalk.cpp's AskPath asks it for a bot (forceDest off), with the check's
// filter on the check's path owner
TravelWalk::StepAnswer AskPath(WorldObject const* owner, RouteCheckProbe::Filter const& filter, bool wide,
                               TravelWalk::ProbePoint const& start, TravelWalk::ProbePoint const& end, bool straight)
{
    PathGenerator path(owner);
    path.SetExcludeFlags(filter.exclude);
    if (filter.steepCost)
        path.SetNavTerrainCost(NAV_GROUND_STEEP, *filter.steepCost);
    path.SetNavTerrainCost(NAV_WATER, filter.waterCost);
    path.SetUseStraightPath(straight);
    path.SetWideSearch(wide);
    TravelWalk::StepAnswer answer;
    answer.result = path.CalculatePath(start.x, start.y, start.z, end.x, end.y, end.z, false);
    answer.type = path.GetPathType();
    for (G3D::Vector3 const& point : path.GetPath())
        answer.points.push_back({point.x, point.y, point.z});
    bool startPoly = true;
    if (answer.type & TravelWalk::PATH_NOT_USING_PATH)
    {
        dtNavMeshQuery const* const query = owner->GetMap()->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
        dtQueryFilter polys;
        polys.setIncludeFlags(filter.include);
        polys.setExcludeFlags(filter.exclude);
        float const point[3] = {start.y, start.z, start.x};
        startPoly = !query || PolyAt(query, polys, point);
    }
    answer.why = TravelWalk::SearchWhy(answer.type, path.GetSearchStatus(), startPoly);
    return answer;
}

// The probe of one pair with one pool: `<probe>:<why>:<steps>:<us>:<type>:<end>`
std::string ProbeWord(WorldObject const* owner, TravelWalk::ProbePoint const& from, TravelWalk::ProbePoint const& to,
                      RouteCheckProbe::Filter const& filter, bool wide, float arriveYards)
{
    auto const straight = [&](TravelWalk::ProbePoint const& start, TravelWalk::ProbePoint const& end)
    { return AskPath(owner, filter, wide, start, end, true); };
    auto const smooth = [&](TravelWalk::ProbePoint const& start, TravelWalk::ProbePoint const& aim)
    { return AskPath(owner, filter, wide, start, aim, false); };
    float const aimYards = MAX_POINT_PATH_LENGTH * SMOOTH_PATH_STEP_SIZE / 2.0f;
    TravelWalk::StepAnswer last;
    auto const step = [&](TravelWalk::ProbePoint const& start, TravelWalk::ProbePoint const& end)
    {
        last = TravelWalk::OrderStep(start, end, straight, smooth, aimYards);
        return last;
    };
    auto const started = std::chrono::steady_clock::now();
    TravelWalk::PieceWalk const piece =
        TravelWalk::StartPiece(false, true, from, to, arriveYards, step, ::GoTo::ProbeCarried);
    long long const micros =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    float const endYards = last.points.empty() ? -1.0f : TravelWalk::Distance(last.points.back(), to);
    return Acore::StringFormat("{}:{}:{}:{}:0x{:x}:{:.1f}", TravelWalk::ProbeWord(piece.probe),
                               TravelWalk::WhyWord(piece.why), +piece.steps, micros, last.type, endYards);
}
}  // namespace

float RouteCheckProbe::ArriveYards(std::string const& label)
{
    std::size_t const at = label.rfind('@');
    if (at != std::string::npos)
    {
        try
        {
            float const yards = std::stof(label.substr(at + 1));
            if (yards > 0.0f)
                return yards;
        }
        catch (...)
        {
        }
    }
    return TravelLegs::WAYPOINT_YARDS;
}

std::string RouteCheckProbe::PairValue(WorldObject const* owner, TravelWalk::ProbePoint const& from,
                                       TravelWalk::ProbePoint const& to, Filter const& filter, float arriveYards)
{
    return "control=" + ProbeWord(owner, from, to, filter, false, arriveYards) +
           " wide=" + ProbeWord(owner, from, to, filter, true, arriveYards);
}
