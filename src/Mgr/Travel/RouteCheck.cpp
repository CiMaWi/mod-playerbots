/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteCheck.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Chat.h"
#include "DBCStores.h"
#include "DetourCommon.h"
#include "GameTime.h"
#include "GridDefines.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "PathGenerator.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RouteCheckRules.h"
#include "Timer.h"
#include "TravelGraph.h"
#include "World.h"

static_assert(RouteCheck::PATH_NORMAL == PATHFIND_NORMAL && RouteCheck::PATH_SHORTCUT == PATHFIND_SHORTCUT &&
              RouteCheck::PATH_INCOMPLETE == PATHFIND_INCOMPLETE && RouteCheck::PATH_NOPATH == PATHFIND_NOPATH &&
              RouteCheck::PATH_NOT_USING_PATH == PATHFIND_NOT_USING_PATH && RouteCheck::PATH_SHORT == PATHFIND_SHORT);

namespace
{
using namespace RouteCheck;
using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds SLICE{20};  // world-thread time per slice; slices come at most every 50 ms
constexpr std::uint32_t CONTINENTS[] = {0, 1, 530, 571};
// The bots' polygons (PathGenerator::CreateFilter for a headless player): ground and water, no steep slopes
constexpr std::uint16_t BOT_EXCLUDE = NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP;
constexpr float BOT_WATER_COST = 5.0f;

// The path owner: PathGenerator needs a WorldObject on the map, not a unit. Never added to the map or the world;
// as no creature it gets the player filter (the bots' exclusions are set on top) and NORMAL|NOT_USING_PATH for a hole
class Probe final : public WorldObject
{
public:
    Probe()
    {
        m_valuesCount = OBJECT_END;
        _InitValues();
    }

    ~Probe() override
    {
        if (FindMap())
            ResetMap();
    }

    void AddToObjectUpdate() override {}
    void RemoveFromObjectUpdate() override {}
};

struct Job
{
    bool running{false};
    bool stop{false};
    std::vector<std::uint32_t> maps;
    std::uint32_t limit{0};  // 0: every walk link of the map
    std::size_t mapIndex{0};
    std::unique_ptr<Probe> probe;  // set while a map is checked
    std::vector<std::uint32_t> nodes;
    std::vector<std::uint32_t> links;
    std::size_t nextNode{0};
    std::size_t nextLink{0};
    MapCounts current;
    std::vector<MapCounts> done;
    std::vector<std::string> rows;
    std::vector<std::string> notes;  // per map: grids created, tiles loaded
    std::vector<std::string> summary;
    Clock::time_point started;
    Clock::duration busy{};
    std::uint32_t slices{0};
    std::uint32_t gridsCreated{0};
};

std::mutex jobMutex;  // the console and the slices both run on the world thread; in-game chat may not
Job job;

Vec At(TravelGraphNode const& node) { return {node.x, node.y, node.z}; }

double Seconds(Clock::duration duration) { return std::chrono::duration<double>(duration).count(); }

// Creates the grids (terrain, vmap and mmap tile, no creatures) that cover the box, as a player's grid load does
void EnsureGrids(Map* map, float minX, float maxX, float minY, float maxY, int margin)
{
    GridCoord const low = Acore::ComputeGridCoord(maxX, maxY);  // grid indices fall as x and y grow
    GridCoord const high = Acore::ComputeGridCoord(minX, minY);
    int const lastGrid = MAX_NUMBER_OF_GRIDS - 1;
    for (int gx = std::max(0, static_cast<int>(low.x_coord) - margin);
         gx <= std::min(lastGrid, static_cast<int>(high.x_coord) + margin); ++gx)
    {
        for (int gy = std::max(0, static_cast<int>(low.y_coord) - margin);
             gy <= std::min(lastGrid, static_cast<int>(high.y_coord) + margin); ++gy)
        {
            GridCoord const grid(gx, gy);
            if (map->IsGridCreated(grid))
                continue;
            map->GetGridTerrainData(grid);
            ++job.gridsCreated;
        }
    }
}

// PathGenerator::HaveTile's test
bool TileLoaded(dtNavMesh const* navMesh, Vec const& at)
{
    if (!navMesh)
        return false;
    float const point[3] = {at.y, at.z, at.x};
    int tileX = -1;
    int tileY = -1;
    navMesh->calcTileLoc(point, &tileX, &tileY);
    return tileX >= 0 && tileY >= 0 && navMesh->getTileAt(tileX, tileY, 0);
}

std::uint32_t TilesLoaded(dtNavMesh const* navMesh)
{
    std::uint32_t count = 0;
    for (int i = 0; navMesh && i < navMesh->getMaxTiles(); ++i)
    {
        dtMeshTile const* tile = navMesh->getTile(i);
        if (tile && tile->header)
            ++count;
    }
    return count;
}

void BeginMap(TravelGraph const& graph)
{
    std::uint32_t const mapId = job.maps[job.mapIndex];
    job.current = MapCounts();
    job.current.mapId = mapId;
    job.nodes.clear();
    job.links.clear();
    job.nextNode = 0;
    job.nextLink = 0;
    Map* map = sMapMgr->CreateBaseMap(mapId);
    if (!map)
        return;
    job.probe = std::make_unique<Probe>();
    job.probe->SetMap(map);
    for (std::uint32_t index = 0; index < graph.LinkCount(); ++index)
    {
        TravelGraphLink const& link = graph.GetLink(index);
        if (link.type == TravelLinkType::Walk && graph.GetNode(link.from).mapId == mapId)
            job.links.push_back(index);
        if (job.limit && job.links.size() >= job.limit)
            break;
    }
    if (job.limit)
    {
        for (std::uint32_t const index : job.links)
        {
            job.nodes.push_back(graph.GetLink(index).from);
            job.nodes.push_back(graph.GetLink(index).to);
        }
        std::sort(job.nodes.begin(), job.nodes.end());
        job.nodes.erase(std::unique(job.nodes.begin(), job.nodes.end()), job.nodes.end());
    }
    else
    {
        for (std::uint32_t index = 0; index < graph.NodeCount(); ++index)
            if (graph.GetNode(index).mapId == mapId)
                job.nodes.push_back(index);
    }
}

void EndMap()
{
    std::uint32_t tiles = 0;
    if (job.probe)
    {
        tiles = TilesLoaded(job.probe->GetMap()->GetMapCollisionData().GetMMapData().GetNavMesh());
        job.probe.reset();
        job.done.push_back(job.current);
        job.notes.push_back(Acore::StringFormat("map {}: {} mmap tiles loaded", job.current.mapId, tiles));
    }
    else
        job.notes.push_back(Acore::StringFormat("map {}: no such map, skipped", job.current.mapId));
    ++job.mapIndex;
}

void CheckNode(TravelGraph const& graph, std::uint32_t index)
{
    TravelGraphNode const& node = graph.GetNode(index);
    Map* map = job.probe->GetMap();
    EnsureGrids(map, node.x, node.x, node.y, node.y, 0);
    ++job.current.nodes;
    MMapData& mmap = map->GetMapCollisionData().GetMMapData();
    dtNavMeshQuery const* query = mmap.GetNavMeshQuery();
    float const point[3] = {node.y, node.z, node.x};
    float const extents[3] = {3.0f, 5.0f, 3.0f};  // PathGenerator::GetPolyByLocation's first box
    auto const nearest = [&](dtQueryFilter const& filter, dtPolyRef& ref)
    {
        float closest[3] = {0.0f, 0.0f, 0.0f};
        ref = 0;
        if (!query || dtStatusFailed(query->findNearestPoly(point, extents, &filter, &ref, closest)) || !ref)
            return -1.0f;
        return dtVdist(closest, point);
    };
    dtQueryFilter bots;
    bots.setIncludeFlags(NAV_GROUND | NAV_WATER);
    bots.setExcludeFlags(BOT_EXCLUDE);
    dtPolyRef ref = 0;
    float const distance = nearest(bots, ref);
    if (distance >= 0.0f && distance <= OFFMESH_LIMIT)
        return;
    dtQueryFilter any;
    any.setIncludeFlags(0xffff);
    any.setExcludeFlags(0);
    float const anyDistance = nearest(any, ref);
    bool steep = false;
    dtMeshTile const* tile = nullptr;
    dtPoly const* poly = nullptr;
    if (anyDistance >= 0.0f && anyDistance <= OFFMESH_LIMIT &&
        dtStatusSucceed(mmap.GetNavMesh()->getTileAndPolyByRef(ref, &tile, &poly)) && poly)
        steep = poly->getArea() == NAV_GROUND_STEEP;
    ++job.current.offMesh;
    if (steep)
        ++job.current.offMeshSteep;
    job.rows.push_back(
        Line("nodes.offmesh", NodeKey(node.mapId, node.x, node.y, node.z), OffMeshValue(distance, steep)));
}

void CheckLink(TravelGraph const& graph, std::uint32_t index)
{
    TravelGraphLink const& link = graph.GetLink(index);
    TravelGraphNode const& from = graph.GetNode(link.from);
    TravelGraphNode const& to = graph.GetNode(link.to);
    Map* map = job.probe->GetMap();
    float minX = std::min(from.x, to.x);
    float maxX = std::max(from.x, to.x);
    float minY = std::min(from.y, to.y);
    float maxY = std::max(from.y, to.y);
    for (TravelGraphPoint const& point : graph.PathOf(link))
    {
        if (point.mapId != from.mapId)
            continue;
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    // One grid around the stored path: Detour finds no path through a tile that is not loaded
    EnsureGrids(map, minX, maxX, minY, maxY, 1);
    dtNavMesh const* navMesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
    bool const startTile = TileLoaded(navMesh, At(from));
    bool const endTile = TileLoaded(navMesh, At(to));
    Probe const* probe = job.probe.get();
    LinkResult const result = Chain(At(from), At(to),
                                    [probe](Vec const& at, Vec const& end)
                                    {
                                        PathGenerator path(probe);
                                        // Corners, not 4 yd steps: the 148-point cap then binds by polygons
                                        path.SetUseStraightPath(true);
                                        path.SetExcludeFlags(BOT_EXCLUDE);
                                        path.SetNavTerrainCost(NAV_WATER, BOT_WATER_COST);
                                        path.CalculatePath(at.x, at.y, at.z, end.x, end.y, end.z, false);
                                        Hop hop;
                                        hop.type = path.GetPathType();
                                        hop.last = at;
                                        if (!path.GetPath().empty())
                                        {
                                            G3D::Vector3 const& last = path.GetPath().back();
                                            hop.last = {last.x, last.y, last.z};
                                        }
                                        return hop;
                                    });
    ++job.current.links;
    ++job.current.byClass[static_cast<int>(result.cls)];
    if (result.cls == LinkClass::Real)
    {
        if (result.hops > 1)
            ++job.current.chained;
        return;
    }
    std::string const key = LinkKey(NodeKey(from.mapId, from.x, from.y, from.z), NodeKey(to.mapId, to.x, to.y, to.z));
    job.rows.push_back(Line(result.cls == LinkClass::Capped ? "links.capped" : "links.broken", key,
                            BrokenValue(result, startTile, endTile)));
}

// One unit of work; true when the job has nothing left
bool Step(TravelGraph const& graph)
{
    if (job.stop || job.mapIndex >= job.maps.size())
        return true;
    if (!job.probe)
    {
        BeginMap(graph);
        if (!job.probe)
        {
            EndMap();
            return job.mapIndex >= job.maps.size();
        }
    }
    if (job.nextNode < job.nodes.size())
        CheckNode(graph, job.nodes[job.nextNode++]);
    else if (job.nextLink < job.links.size())
        CheckLink(graph, job.links[job.nextLink++]);
    else
    {
        EndMap();
        return job.mapIndex >= job.maps.size();
    }
    return false;
}

void Finish(TravelGraph const& graph)
{
    if (job.probe)
        EndMap();  // stopped inside a map: its partial counts
    std::string const stamp = Acore::Time::TimeToTimestampStr(GameTime::GetGameTime(), "%Y%m%d-%H%M%S");
    std::filesystem::path const file =
        std::filesystem::path(sLog->GetLogsDir()) / ("route-check-links-" + stamp + ".tsv");
    double const elapsed = Seconds(Clock::now() - job.started);
    TravelGraphStats const& stats = graph.Stats();
    std::vector<std::string> summary;
    MapCounts total;
    for (MapCounts const& counts : job.done)
    {
        summary.push_back("ROUTE check links: " + SummaryLine("map " + std::to_string(counts.mapId), counts));
        total.Add(counts);
    }
    summary.push_back("ROUTE check links: " + SummaryLine("total", total));
    for (std::string const& note : job.notes)
        summary.push_back("ROUTE check links: " + note);
    summary.push_back(Acore::StringFormat(
        "ROUTE check links: {}: {:.1f} s elapsed, {:.1f} s on the world thread in {} slices, {} grids created; "
        "report {}",
        job.stop ? "stopped" : "done", elapsed, Seconds(job.busy), job.slices, job.gridsCreated, file.string()));
    std::ofstream out(file, std::ios::binary);
    if (out)
    {
        out << "# route check links (PLAN navigation §3.2 check 3), " << stamp << "\n";
        out << "# graph: " << stats.nodes << " nodes, " << stats.links << " links ("
            << stats.linksByType[static_cast<int>(TravelLinkType::Walk)] << " walk), " << stats.points
            << " path points\n";
        out << "# navmesh: " << sWorld->GetDataPath() << "mmaps\n";
        out << "# " << (job.stop ? "stopped" : "complete") << (job.limit ? ", first links only" : "") << ", "
            << Acore::StringFormat("{:.1f}", elapsed) << " s\n";
        for (std::string const& line : ReportLines(job.done, std::move(job.rows)))
            out << line << "\n";
    }
    else
        summary.push_back("ROUTE check links: could not write " + file.string());
    for (std::string const& line : summary)
        LOG_INFO("playerbots", "{}", line);
    job.summary = std::move(summary);
    job.rows.clear();
    job.running = false;
}

bool QueueSlice();

class SliceOperation final : public PlayerbotOperation
{
public:
    bool Execute() override
    {
        std::lock_guard<std::mutex> guard(jobMutex);
        if (!job.running)
            return true;
        TravelGraph const& graph = TravelGraph::Instance();
        Clock::time_point const sliceStart = Clock::now();
        bool finished = false;
        while (!finished && Clock::now() - sliceStart < SLICE)
            finished = Step(graph);
        job.busy += Clock::now() - sliceStart;
        ++job.slices;
        if (finished)
            Finish(graph);
        else if (!QueueSlice())
        {
            job.stop = true;
            Finish(graph);
        }
        return true;
    }

    std::string GetName() const override { return "route check links"; }
};

bool QueueSlice()
{
    return PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<SliceOperation>());
}

void PrintStatus(ChatHandler* handler)
{
    if (job.running)
    {
        handler->PSendSysMessage(
            "ROUTE check links: running, map {} ({} of {}): {} of {} nodes, {} of {} links; {:.1f} s elapsed",
            job.maps[std::min(job.mapIndex, job.maps.size() - 1)], job.mapIndex + 1, job.maps.size(), job.nextNode,
            job.nodes.size(), job.nextLink, job.links.size(), Seconds(Clock::now() - job.started));
        return;
    }
    if (job.summary.empty())
    {
        handler->PSendSysMessage("ROUTE check links: no check has run (playerbots route check links [map [count]])");
        return;
    }
    for (std::string const& line : job.summary)
        handler->PSendSysMessage("{}", line);
}
}  // namespace

bool RouteCheck::HandleLinksCommand(ChatHandler* handler, char const* args)
{
    std::lock_guard<std::mutex> guard(jobMutex);
    std::string const text = args ? args : "";
    std::vector<std::string> words;
    for (std::size_t at = 0; at < text.size();)
    {
        std::size_t const end = std::min(text.find(' ', at), text.size());
        if (end > at)
            words.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    if (!words.empty() && words[0] == "status")
    {
        PrintStatus(handler);
        return true;
    }
    if (!words.empty() && words[0] == "stop")
    {
        job.stop = job.running;
        handler->SendSysMessage(job.running ? "ROUTE check links: stopping after this slice"
                                            : "ROUTE check links: no check is running");
        return true;
    }
    if (job.running)
    {
        handler->PSendSysMessage("ROUTE check links: a check is running (playerbots route check links status|stop)");
        return true;
    }
    TravelGraph const& graph = TravelGraph::Instance();
    if (!graph.IsReady())
    {
        handler->PSendSysMessage("ROUTE check links: the travel graph is not ready (see playerbots route status)");
        return true;
    }
    std::vector<std::uint32_t> maps(std::begin(CONTINENTS), std::end(CONTINENTS));
    std::uint32_t limit = 0;
    if (!words.empty() && words[0] != "all")
    {
        char* end = nullptr;
        unsigned long const mapId = std::strtoul(words[0].c_str(), &end, 10);
        if (!end || *end)
        {
            handler->SendErrorMessage("ROUTE check links: usage: [map [count]] | all | status | stop");
            return false;
        }
        // CreateBaseMap asserts on a map id the DBC does not know: refuse it here
        if (!sMapStore.LookupEntry(static_cast<std::uint32_t>(mapId)))
        {
            handler->SendErrorMessage("ROUTE check links: no map {}", mapId);
            return false;
        }
        maps = {static_cast<std::uint32_t>(mapId)};
        if (words.size() > 1)
            limit = static_cast<std::uint32_t>(std::strtoul(words[1].c_str(), nullptr, 10));
    }
    job = Job();
    job.running = true;
    job.maps = maps;
    job.limit = limit;
    job.started = Clock::now();
    if (!QueueSlice())
    {
        job.running = false;
        handler->SendErrorMessage("ROUTE check links: the world-thread queue refused the job");
        return false;
    }
    std::string mapList;
    for (std::uint32_t const mapId : maps)
        mapList += (mapList.empty() ? "" : " ") + std::to_string(mapId);
    handler->PSendSysMessage("ROUTE check links: started on map(s) {}{}; poll playerbots route check links status",
                             mapList, limit ? Acore::StringFormat(", first {} walk links each", limit) : "");
    return true;
}
