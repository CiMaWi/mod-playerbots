/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteCheckOutdoor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Chat.h"
#include "DBCEnums.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GridDefines.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RouteCheckOutdoorRules.h"
#include "RouteCheckRules.h"
#include "Timer.h"
#include "TravelGraph.h"

namespace
{
using Clock = std::chrono::steady_clock;
using RouteCheckOutdoorRules::PointSpec;

static_assert(RouteCheckOutdoorRules::AREA_FLAG_OUTLAND_BIT == AREA_FLAG_OUTLAND);
static_assert(RouteCheckOutdoorRules::AREA_FLAG_NO_FLY_ZONE_BIT == AREA_FLAG_NO_FLY_ZONE);
static_assert(RouteCheckOutdoorRules::LIQUID_ABOVE == LIQUID_MAP_ABOVE_WATER);
static_assert(RouteCheckOutdoorRules::LIQUID_WALK == LIQUID_MAP_WATER_WALK);
static_assert(RouteCheckOutdoorRules::LIQUID_IN == LIQUID_MAP_IN_WATER);
static_assert(RouteCheckOutdoorRules::LIQUID_UNDER == LIQUID_MAP_UNDER_WATER);

constexpr std::chrono::milliseconds SLICE{20};  // world-thread time per slice

struct Job
{
    bool running{false};
    std::string source;  // "graph" or the file
    std::vector<PointSpec> points;
    std::size_t next{0};
    std::vector<std::string> rows;
    RouteCheckOutdoorRules::Counts counts;
    std::uint32_t gridsCreated{0};
    std::uint32_t slices{0};
    Clock::time_point started;
    Clock::duration busy{};
};

Job job;  // touched on the world thread only (the console's call, the queued start and the slices)

// Maps the server answers without an instance; a dungeon's terrain is asked per instance, not here
Map* AnswerMap(std::uint32_t mapId)
{
    MapEntry const* entry = sMapStore.LookupEntry(mapId);
    if (!entry || entry->Instanceable())
        return nullptr;
    return sMapMgr->CreateBaseMap(mapId);
}

void CheckPoint(PointSpec const& point)
{
    std::string const key = RouteCheck::NodeKey(point.mapId, point.x, point.y, point.z);
    std::string const where =
        Acore::StringFormat("{}\t{}\t{:.4f}\t{:.4f}\t{:.4f}", key, point.mapId, point.x, point.y, point.z);
    Map* map = AnswerMap(point.mapId);
    if (!map)
    {
        RouteCheckOutdoorRules::Count(job.counts, false, false, false, 0);
        job.rows.push_back(where + "\tnomap\t-\t-\t-\t-\t-\t" + point.label);
        return;
    }
    if (!map->IsGridCreated(point.x, point.y))
        ++job.gridsCreated;  // GetFullTerrainStatusForPosition creates it (terrain, vmap and mmap tile)

    // a player's own question (WorldObject::UpdatePositionData)
    PositionFullTerrainStatus data;
    map->GetFullTerrainStatusForPosition(PHASEMASK_NORMAL, point.x, point.y, point.z, DEFAULT_COLLISION_HEIGHT, data);
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(data.areaId);
    std::uint32_t const zone = area && area->zone ? area->zone : data.areaId;
    AreaTableEntry const* entry = area ? area : sAreaTableStore.LookupEntry(zone);
    bool const flyable = RouteCheckOutdoorRules::AreaAllowsFlight(entry != nullptr, entry ? entry->flags : 0);
    std::uint32_t const liquid = static_cast<std::uint32_t>(data.liquidInfo.Status);
    RouteCheckOutdoorRules::Count(job.counts, true, data.outdoors, flyable, liquid);
    std::string const floor =
        data.floorZ > VMAP_INVALID_HEIGHT ? Acore::StringFormat("{:.4f}", data.floorZ) : std::string("-");
    job.rows.push_back(Acore::StringFormat("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", where, data.outdoors ? 1 : 0, data.areaId,
                                           zone, flyable ? 1 : 0, RouteCheckOutdoorRules::LiquidWord(liquid), floor,
                                           point.label));
}

void Finish()
{
    std::string const stamp = Acore::Time::TimeToTimestampStr(GameTime::GetGameTime(), "%Y%m%d-%H%M%S");
    std::filesystem::path const file =
        std::filesystem::path(sLog->GetLogsDir()) / ("route-check-outdoor-" + stamp + ".tsv");
    std::string const summary = RouteCheckOutdoorRules::SummaryLine(job.counts);
    double const elapsed = std::chrono::duration<double>(Clock::now() - job.started).count();
    double const busy = std::chrono::duration<double>(job.busy).count();
    std::ofstream out(file, std::ios::binary);
    if (out)
    {
        out << "# route check outdoor (navigation plan NV32), " << stamp << ", points from " << job.source
            << "; the engine's answer for a player standing at the point (phase 1, collision height "
            << DEFAULT_COLLISION_HEIGHT << "): outdoor 1/0, area (a WMO's where the point is in or on one), zone, "
            << "flyable 1/0 (the area allows flying: AREA_FLAG_OUTLAND and not AREA_FLAG_NO_FLY_ZONE), liquid "
            << "none|above|walk|in|under, floor_z (-: none); nomap: no answer (an instance map or unknown)\n";
        out << "key\tmap\tx\ty\tz\toutdoor\tarea\tzone\tflyable\tliquid\tfloor_z\tlabel\n";
        for (std::string const& row : job.rows)
            out << row << "\n";
        out << "# " << summary << "\n";
    }
    LOG_INFO("playerbots", "{}", summary);
    LOG_INFO("playerbots",
             "ROUTE check outdoor: done: {:.1f} s elapsed, {:.1f} s on the world thread in {} slices, {} grids "
             "created; report {}",
             elapsed, busy, job.slices, job.gridsCreated, out ? file.string() : "not written: " + file.string());
    job = Job();
}

bool QueueSlice();

class SliceOperation final : public PlayerbotOperation
{
public:
    bool Execute() override
    {
        if (!job.running)
            return true;
        Clock::time_point const start = Clock::now();
        while (job.next < job.points.size() && Clock::now() - start < SLICE)
            CheckPoint(job.points[job.next++]);
        job.busy += Clock::now() - start;
        ++job.slices;
        if (job.next >= job.points.size())
            Finish();
        else if (!QueueSlice())
        {
            LOG_ERROR("playerbots", "ROUTE check outdoor: the world thread's queue is full; stopped after {} points",
                      job.next);
            job.points.resize(job.next);
            Finish();
        }
        return true;
    }

    std::string GetName() const override { return "route check outdoor"; }
};

bool QueueSlice()
{
    return PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<SliceOperation>());
}

// Reads the points (the graph's nodes, or the file once) and queues the first slice; one line for the caller
std::string Start(std::string const& path)
{
    if (job.running)
        return "ROUTE check outdoor: a check is running";
    std::vector<PointSpec> points;
    std::uint32_t skipped = 0;
    if (path.empty())
    {
        TravelGraph const& graph = TravelGraph::Instance();
        if (!graph.IsReady())
            return "ROUTE check outdoor: the travel graph is not ready (see playerbots route status)";
        for (std::uint32_t index = 0; index < graph.NodeCount(); ++index)
        {
            TravelGraphNode const& node = graph.GetNode(index);
            points.push_back({node.mapId, node.x, node.y, node.z, "node " + std::to_string(node.id)});
        }
    }
    else
    {
        std::ifstream in{std::filesystem::path(path)};
        if (!in)
            return "ROUTE check outdoor: cannot read " + path;
        std::string line;
        while (std::getline(in, line))
        {
            PointSpec point;
            if (RouteCheckOutdoorRules::ParsePointLine(line, point))
                points.push_back(std::move(point));
            else if (line.find_first_not_of(" \t\r") != std::string::npos &&
                     line[line.find_first_not_of(" \t\r")] != '#')
                ++skipped;
        }
    }
    if (points.empty())
        return Acore::StringFormat("ROUTE check outdoor: no point to ask ({} lines skipped)", skipped);
    std::stable_sort(points.begin(), points.end(),
                     [](PointSpec const& a, PointSpec const& b) { return a.mapId < b.mapId; });
    job = Job();
    job.running = true;
    job.source = path.empty() ? std::string("the loaded graph") : path;
    job.started = Clock::now();
    std::size_t const count = points.size();
    job.points = std::move(points);
    if (!QueueSlice())
    {
        job = Job();
        return "ROUTE check outdoor: the world thread's queue is full";
    }
    return Acore::StringFormat(
        "ROUTE check outdoor: started, {} points from {}, {} lines skipped; the result goes "
        "to the server log",
        count, job.source, skipped);
}

class StartOperation final : public PlayerbotOperation
{
public:
    explicit StartOperation(std::string path) : _path(std::move(path)) {}

    bool Execute() override
    {
        LOG_INFO("playerbots", "{}", Start(_path));
        return true;
    }

    std::string GetName() const override { return "route check outdoor start"; }

private:
    std::string _path;
};
}  // namespace

bool RouteCheckOutdoor::HandleOutdoorCommand(ChatHandler* handler, char const* args)
{
    std::string path = args ? args : "";
    std::size_t const first = path.find_first_not_of(' ');
    path = first == std::string::npos ? std::string() : path.substr(first);
    while (!path.empty() && path.back() == ' ')
        path.pop_back();

    // a session's command runs on its map's thread: the start goes to the world thread
    if (handler->GetSession())
    {
        if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<StartOperation>(path)))
        {
            handler->SendErrorMessage("ROUTE check outdoor: the world thread's queue is full");
            return false;
        }
        handler->PSendSysMessage("ROUTE check outdoor: queued; the result goes to the server log");
        return true;
    }

    handler->PSendSysMessage("{}", Start(path));
    return true;
}
