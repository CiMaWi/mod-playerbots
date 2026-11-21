/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteCheckTargets.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "Chat.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GridDefines.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RouteCheckTargetsRules.h"
#include "RpgNavMeshSnap.h"
#include "Timer.h"
#include "TravelGraph.h"

namespace
{
using Clock = std::chrono::steady_clock;
using RouteCheckTargetsRules::Facts;
using RouteCheckTargetsRules::Row;
using RouteCheckTargetsRules::Target;

constexpr std::chrono::milliseconds SLICE{20};  // world-thread time per slice

struct Job
{
    bool running{false};
    std::string source;
    std::vector<Target> targets;     // input order; row = index + 1
    std::vector<std::size_t> order;  // indices, stable by map: the order the grids are created in
    std::vector<std::string> rows;   // formatted, by input index
    std::size_t next{0};
    RouteCheckTargetsRules::Counts counts;
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

// Creates the grids (terrain, vmap and mmap tile) that cover the box, as `route check pairs` does
void EnsureGrids(Map* map, float minX, float maxX, float minY, float maxY)
{
    GridCoord const low = Acore::ComputeGridCoord(maxX, maxY);  // grid indices fall as x and y grow
    GridCoord const high = Acore::ComputeGridCoord(minX, minY);
    for (std::uint32_t gx = low.x_coord; gx <= high.x_coord; ++gx)
    {
        for (std::uint32_t gy = low.y_coord; gy <= high.y_coord; ++gy)
        {
            GridCoord const grid(gx, gy);
            if (map->IsGridCreated(grid))
                continue;
            map->GetGridTerrainData(grid);
            ++job.gridsCreated;
        }
    }
}

float Distance3D(float ax, float ay, float az, float bx, float by, float bz)
{
    float const dx = ax - bx;
    float const dy = ay - by;
    float const dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void CheckTarget(std::size_t index)
{
    Target const& target = job.targets[index];
    Row row;
    row.index = static_cast<std::uint32_t>(index + 1);
    row.target = target;
    Facts facts;
    Map* map = AnswerMap(target.mapId);
    if (map)
    {
        EnsureGrids(map, target.x - target.reach, target.x + target.reach, target.y - target.reach,
                    target.y + target.reach);
        float x = target.x;
        float y = target.y;
        float z = target.z;
        NavMeshSnap::Result const found = NavMeshSnap::StandPoint(map, x, y, z, target.reach);
        facts.tileKnown = found != NavMeshSnap::Result::Unknown;
        facts.stand = found == NavMeshSnap::Result::Found;
        if (facts.stand)
        {
            row.sx = x;
            row.sy = y;
            row.sz = z;
            row.standYards = Distance3D(x, y, z, target.x, target.y, target.z);
            row.fragment = NavMeshSnap::IsFragment(map, x, y, z);
        }
        else if (facts.tileKnown)
        {
            NavMeshSnap::Miss const miss = NavMeshSnap::ClassifyMiss(
                map, target.x, target.y, target.z, target.reach, RouteCheckTargetsRules::OTHER_FLOOR_VERTICAL_YARDS);
            facts.steepNear = miss.steep;
            facts.otherFloor = miss.otherFloor;
        }
    }
    row.result = RouteCheckTargetsRules::Classify(facts);

    TravelGraph const& graph = TravelGraph::Instance();
    if (graph.IsReady())
    {
        bool const stand = row.result == RouteCheckTargetsRules::Result::Stand;
        float const fromX = stand ? row.sx : target.x;
        float const fromY = stand ? row.sy : target.y;
        float const fromZ = stand ? row.sz : target.z;
        for (std::uint32_t const node :
             graph.NearestNodes(target.mapId, fromX, fromY, fromZ, RouteCheckTargetsRules::NODES_LISTED))
        {
            TravelGraphNode const& at = graph.GetNode(node);
            row.nodes.push_back({at.id, Distance3D(at.x, at.y, at.z, fromX, fromY, fromZ)});
        }
    }
    RouteCheckTargetsRules::Count(job.counts, row);
    job.rows[index] = RouteCheckTargetsRules::FormatRow(row);
}

void Finish()
{
    std::string const stamp = Acore::Time::TimeToTimestampStr(GameTime::GetGameTime(), "%Y%m%d-%H%M%S");
    std::filesystem::path const file =
        std::filesystem::path(sLog->GetLogsDir()) / ("route-check-targets-" + stamp + ".tsv");
    std::string const summary = RouteCheckTargetsRules::SummaryLine(job.counts);
    double const elapsed = std::chrono::duration<double>(Clock::now() - job.started).count();
    double const busy = std::chrono::duration<double>(job.busy).count();
    std::ofstream out(file, std::ios::binary);
    if (out)
    {
        out << "# route check targets (pathing plan PA8), " << stamp << ", targets from " << job.source
            << "; what a bot's typed approach finds at each target (map x y z reach), filter bots (ground or water, no "
               "magma, slime or steep ground): result stand (a polygon point within reach, 3D: sx sy sz, stand_yd; "
               "fragment 1/0: the stand point's polygon belongs to a piece of fewer than 16 polygons), steep_only "
               "(only steep ground within reach), other_floor (a polygon within reach horizontally on another level), "
               "no_poly, no_tile (no answer: no navmesh, tile not loaded, instance map); node, node_yd: the nearest "
               "graph node from the stand point (the target when none), nodes5: the five nearest id:yd; - none\n";
        out << RouteCheckTargetsRules::ColumnLine() << "\n";
        for (std::string const& row : job.rows)
            out << row << "\n";
        out << "# " << summary << "\n# complete\n";
    }
    LOG_INFO("playerbots", "{}", summary);
    LOG_INFO("playerbots",
             "ROUTE check targets: done: {:.1f} s elapsed, {:.1f} s on the world thread in {} slices, {} grids "
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
        while (job.next < job.order.size() && Clock::now() - start < SLICE)
            CheckTarget(job.order[job.next++]);
        job.busy += Clock::now() - start;
        ++job.slices;
        if (job.next >= job.order.size())
            Finish();
        else if (!QueueSlice())
        {
            LOG_ERROR("playerbots", "ROUTE check targets: the world thread's queue is full; stopped after {} rows",
                      job.next);
            job = Job();
        }
        return true;
    }

    std::string GetName() const override { return "route check targets"; }
};

bool QueueSlice()
{
    return PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<SliceOperation>());
}

// Reads the file once and queues the first slice; one line for the caller
std::string Start(std::string const& path)
{
    if (job.running)
        return "ROUTE check targets: a check is running";
    if (path.empty())
        return "ROUTE check targets: usage: playerbots route check targets <file> (map x y z reach [kind [entry "
               "[guid]]] per line)";
    std::ifstream in{std::filesystem::path(path)};
    if (!in)
        return "ROUTE check targets: cannot read " + path;
    std::vector<Target> targets;
    std::uint32_t skipped = 0;
    std::string line;
    while (std::getline(in, line))
    {
        std::size_t const first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#')
            continue;
        Target target;
        if (RouteCheckTargetsRules::ParseTargetLine(line, target) && sMapStore.LookupEntry(target.mapId))
            targets.push_back(std::move(target));
        else
            ++skipped;
    }
    if (targets.empty())
        return Acore::StringFormat("ROUTE check targets: no target to ask ({} lines skipped)", skipped);
    job = Job();
    job.order.resize(targets.size());
    std::iota(job.order.begin(), job.order.end(), std::size_t{0});
    std::stable_sort(job.order.begin(), job.order.end(),
                     [&](std::size_t a, std::size_t b) { return targets[a].mapId < targets[b].mapId; });
    job.rows.assign(targets.size(), std::string());
    std::size_t const count = targets.size();
    job.targets = std::move(targets);
    job.running = true;
    job.source = path;
    job.started = Clock::now();
    if (!QueueSlice())
    {
        job = Job();
        return "ROUTE check targets: the world thread's queue is full";
    }
    return Acore::StringFormat(
        "ROUTE check targets: started, {} rows from {}, {} lines skipped; the result goes to the server log", count,
        path, skipped);
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

    std::string GetName() const override { return "route check targets start"; }

private:
    std::string _path;
};
}  // namespace

bool RouteCheckTargets::HandleTargetsCommand(ChatHandler* handler, char const* args)
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
            handler->SendErrorMessage("ROUTE check targets: the world thread's queue is full");
            return false;
        }
        handler->PSendSysMessage("ROUTE check targets: queued; the result goes to the server log");
        return true;
    }

    handler->PSendSysMessage("{}", Start(path));
    return true;
}
