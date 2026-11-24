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
#include "ObjectMgr.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RouteCheckTargetsRules.h"
#include "RpgNavMeshSnap.h"
#include "StandRules.h"
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

bool StartsWith(std::string const& text, char const* head) { return text.rfind(head, 0) == 0; }

bool IsObjectKind(std::string const& kind)
{
    return kind == "mailbox" || kind == "questobject" || StartsWith(kind, "station_");
}

// The stand rule's target for a row: the spawn's facing, the core's reach for its kind and size (a creature's smallest
// model, an object's type and scaled model box) from the server's stores; a station keeps the row's reach (a spell
// focus is measured centre to centre, the list already less a margin), a control or an unknown spawn too, faceless
StandRules::Target StandTarget(Target const& row, std::uint32_t& phaseMask)
{
    StandRules::Target target;
    target.x = row.x;
    target.y = row.y;
    target.z = row.z;
    target.faced = false;
    target.maxYards = row.reach;
    phaseMask = PHASEMASK_NORMAL;
    std::uint32_t spawnId = 0;
    try
    {
        spawnId = static_cast<std::uint32_t>(std::stoul(row.guid));
    }
    catch (...)
    {
        spawnId = 0;
    }
    if (!spawnId || StartsWith(row.kind, "control_"))
    {
        target.minYards = StandRules::MinYards(target.maxYards);
        return target;
    }
    if (IsObjectKind(row.kind))
    {
        GameObjectData const* data = sObjectMgr->GetGameObjectData(spawnId);
        GameObjectTemplate const* info = data ? sObjectMgr->GetGameObjectTemplate(data->id) : nullptr;
        if (data && info && data->mapid == row.mapId)
        {
            target.facing = data->orientation;
            target.faced = true;
            phaseMask = data->phaseMask;
            if (!StartsWith(row.kind, "station_"))
            {
                target.maxYards = StandRules::MaxYards(StandRules::ObjectReach(info->type));
                if (GameObjectDisplayInfoEntry const* display =
                        sGameObjectDisplayInfoStore.LookupEntry(info->displayId))
                {
                    target.boxed = true;
                    target.box = {display->minX * info->size, display->minY * info->size, display->minZ * info->size,
                                  display->maxX * info->size, display->maxY * info->size, display->maxZ * info->size};
                    target.rotation[0] = data->rotation.x;
                    target.rotation[1] = data->rotation.y;
                    target.rotation[2] = data->rotation.z;
                    target.rotation[3] = data->rotation.w;
                }
            }
        }
    }
    else
    {
        CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
        CreatureTemplate const* info = data ? sObjectMgr->GetCreatureTemplate(data->id) : nullptr;
        if (data && info && data->mapid == row.mapId)
        {
            target.facing = data->orientation;
            target.faced = true;
            phaseMask = data->phaseMask;
            float size = DEFAULT_WORLD_OBJECT_SIZE;
            bool sized = false;
            for (CreatureModel const& model : info->Models)
            {
                CreatureModelInfo const* modelInfo = sObjectMgr->GetCreatureModelInfo(model.CreatureDisplayID);
                float const reach = (modelInfo && modelInfo->combat_reach > 0.0f ? modelInfo->combat_reach
                                                                                 : DEFAULT_WORLD_OBJECT_SIZE) *
                                    model.DisplayScale;
                if (!sized || reach < size)
                    size = reach;
                sized = true;
            }
            target.maxYards = StandRules::MaxYards(StandRules::CreatureReach(size));
        }
    }
    target.minYards = StandRules::MinYards(target.maxYards);
    return target;
}

void CheckTarget(std::size_t index)
{
    Target const& target = job.targets[index];
    Row row;
    row.index = static_cast<std::uint32_t>(index + 1);
    row.target = target;
    Facts facts;
    std::uint32_t phaseMask = PHASEMASK_NORMAL;
    StandRules::Target const stand = StandTarget(target, phaseMask);
    row.maxYards = stand.maxYards;
    Map* map = AnswerMap(target.mapId);
    if (map)
    {
        float const outer = StandRules::OuterYards(stand);
        EnsureGrids(map, target.x - outer, target.x + outer, target.y - outer, target.y + outer);
        NavMeshSnap::Stand const found = NavMeshSnap::StandPoint(map, stand, phaseMask, {}, 0);
        facts.tileKnown = found.result != NavMeshSnap::Result::Unknown;
        facts.stand = found.answer.why == StandRules::Why::Found || found.answer.why == StandRules::Why::Fragment;
        if (facts.tileKnown && facts.stand)
        {
            row.sx = found.x;
            row.sy = found.y;
            row.sz = found.z;
            row.standYards = found.answer.yards;
            row.fragment = found.answer.why == StandRules::Why::Fragment;
            row.side = StandRules::Word(found.answer.side);
            row.inSight = found.answer.inSight;
        }
        else if (facts.tileKnown)
        {
            NavMeshSnap::Miss const miss = NavMeshSnap::ClassifyMiss(
                map, target.x, target.y, target.z, stand.maxYards, RouteCheckTargetsRules::OTHER_FLOOR_VERTICAL_YARDS);
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
               "magma, slime or steep ground): result stand (the stand rule's point, StandRules.h: sx sy sz, stand_yd "
               "3D, side; fragment 1: no joined floor in the band, the closed piece's nearest point), steep_only (only "
               "steep ground within max_yd), other_floor (a polygon within max_yd horizontally on another level), "
               "no_poly, no_tile (no answer: no navmesh, tile not loaded, instance map); node, node_yd: the nearest "
               "graph node from the stand point (the target when none), nodes5: the five nearest id:yd; max_yd: the "
               "rule's greatest distance (the core's check for the spawn less a margin, to the model box for an object "
               "with one); sight: 1 the stand point sees the target, 0 none of the band's free joined points does (the "
               "best out of sight), - a fragment or no stand; - none\n";
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
