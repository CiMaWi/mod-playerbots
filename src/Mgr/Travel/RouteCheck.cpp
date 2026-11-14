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
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_set>
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
              RouteCheck::PATH_NOT_USING_PATH == PATHFIND_NOT_USING_PATH && RouteCheck::PATH_SHORT == PATHFIND_SHORT &&
              RouteCheck::PATH_FARFROMPOLY == PATHFIND_FARFROMPOLY);

namespace
{
using namespace RouteCheck;
using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds SLICE{20};  // world-thread time per slice; slices come at most every 50 ms
constexpr std::uint32_t CONTINENTS[] = {0, 1, 530, 571};
// The bots' polygons (PathGenerator::CreateFilter for a headless player, core v21): ground, steep ground and water;
// no lava, slime or fatigue water; steep ground (STEEP_COST) and water cost more. `model=v20` allows fatigue water
// and leaves steep ground out; `steep=` sets steep ground apart (BotIncludeFlags, BotExcludeFlags). Fatigue water is
// NAV_DEEP_WATER on core v21 and an unused area (NAV_UNUSED2) on v20: a literal, so the module builds on both
constexpr std::uint16_t NAV_FATIGUE_WATER = 0x20;
constexpr float BOT_WATER_COST = 5.0f;
constexpr float HAZARD_PLAIN = 0.5f;  // yd: a point this near a plain polygon is not on a hazard

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
    CheckOptions options;         // the walk model's words: model, mode, steep
    bool pairsMode{false};        // `route check pairs`: the listed point pairs instead of the graph's links
    std::vector<PairSpec> pairs;  // sorted by map
    std::size_t nextPair{0};
    std::unordered_set<std::uint32_t> offNodes;  // nodes of the map over OFFMESH_LIMIT yd from the mesh (or none)
    NodeLinkCounts nodeLinks;                    // per node key: its non-ok links by class
    Clock::time_point started;
    Clock::duration busy{};
    std::uint32_t slices{0};
    std::uint32_t gridsCreated{0};
};

std::mutex jobMutex;  // the console and the slices both run on the world thread; in-game chat may not
Job job;

char const* Title() { return job.pairsMode ? "pairs" : "links"; }

std::uint16_t BotIncludeFlags()
{
    return static_cast<std::uint16_t>(NAV_GROUND | NAV_WATER | (job.options.SteepExcluded() ? 0 : NAV_GROUND_STEEP) |
                                      (job.options.areasAll ? (NAV_MAGMA | NAV_FATIGUE_WATER) : 0));
}

std::uint16_t BotExcludeFlags()
{
    std::uint16_t const steep = job.options.SteepExcluded() ? NAV_GROUND_STEEP : 0;
    if (job.options.areasAll)  // slime stays out: the engine's player filter does not include it
        return static_cast<std::uint16_t>(NAV_SLIME | steep);
    return static_cast<std::uint16_t>(NAV_MAGMA | NAV_SLIME | (job.options.v20Model ? 0 : NAV_FATIGUE_WATER) | steep);
}

// Takes the walk model's words (CheckOptions) out of the arguments, each at its last place in text (the words follow
// a pairs file's path, which keeps its own text); false with the first bad word in bad
bool TakeOptions(std::string& text, std::vector<std::string>& words, CheckOptions& options, std::string& bad)
{
    for (auto it = words.begin(); it != words.end();)
    {
        if (!IsCheckWord(*it))
        {
            ++it;
            continue;
        }
        if (!TakeCheckWord(*it, options))
        {
            bad = *it;
            return false;
        }
        std::size_t const at = text.rfind(*it);
        if (at != std::string::npos)
            text.erase(at, it->size());
        it = words.erase(it);
    }
    return true;
}

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
    job.offNodes.clear();
    job.nodeLinks.clear();
    Map* map = sMapMgr->CreateBaseMap(mapId);
    if (!map)
        return;
    job.probe = std::make_unique<Probe>();
    job.probe->SetMap(map);
    if (job.pairsMode)
        return;
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
        AddNodeLinkLines(job.rows, job.nodeLinks);
        tiles = TilesLoaded(job.probe->GetMap()->GetMapCollisionData().GetMMapData().GetNavMesh());
        job.probe.reset();
        job.done.push_back(job.current);
        job.notes.push_back(Acore::StringFormat("map {}: {} mmap tiles loaded", job.current.mapId, tiles));
    }
    else
        job.notes.push_back(Acore::StringFormat("map {}: no such map, skipped", job.current.mapId));
    ++job.mapIndex;
}

struct MeshAnswer
{
    float distance{-1.0f};  // to the nearest polygon bots may walk on; < 0: none in the search box
    bool steep{false};      // a polygon bots may not walk on lies within OFFMESH_LISTED
};

// The box reaches OFFMESH_LIMIT yd each way (horizontally), so a polygon within that distance is found
MeshAnswer MeshDistance(Map* map, Vec const& at)
{
    MMapData& mmap = map->GetMapCollisionData().GetMMapData();
    dtNavMeshQuery const* query = mmap.GetNavMeshQuery();
    float const point[3] = {at.y, at.z, at.x};
    float const extents[3] = {OFFMESH_LIMIT, 5.0f, OFFMESH_LIMIT};
    auto const nearest = [&](dtQueryFilter const& filter, dtPolyRef& ref)
    {
        float closest[3] = {0.0f, 0.0f, 0.0f};
        ref = 0;
        if (!query || dtStatusFailed(query->findNearestPoly(point, extents, &filter, &ref, closest)) || !ref)
            return -1.0f;
        return dtVdist(closest, point);
    };
    MeshAnswer answer;
    dtQueryFilter bots;
    bots.setIncludeFlags(BotIncludeFlags());
    bots.setExcludeFlags(BotExcludeFlags());
    dtPolyRef ref = 0;
    answer.distance = nearest(bots, ref);
    if (answer.distance >= 0.0f && answer.distance <= OFFMESH_LISTED)
        return answer;
    dtQueryFilter any;
    any.setIncludeFlags(0xffff);
    any.setExcludeFlags(0);
    float const anyDistance = nearest(any, ref);
    dtMeshTile const* tile = nullptr;
    dtPoly const* poly = nullptr;
    if (anyDistance >= 0.0f && anyDistance <= OFFMESH_LISTED &&
        dtStatusSucceed(mmap.GetNavMesh()->getTileAndPolyByRef(ref, &tile, &poly)) && poly)
        answer.steep = poly->getArea() == NAV_GROUND_STEEP;
    return answer;
}

void CheckNode(TravelGraph const& graph, std::uint32_t index)
{
    TravelGraphNode const& node = graph.GetNode(index);
    Map* map = job.probe->GetMap();
    EnsureGrids(map, node.x, node.x, node.y, node.y, 0);
    ++job.current.nodes;
    MeshAnswer const answer = MeshDistance(map, At(node));
    if (answer.distance >= 0.0f && answer.distance <= OFFMESH_LISTED)
        return;
    ++job.current.offMesh;
    if (IsOffMesh(answer.distance))
    {
        ++job.current.offMesh4;
        job.offNodes.insert(index);
    }
    if (answer.steep)
        ++job.current.offMeshSteep;
    job.rows.push_back(Line("nodes.offmesh", NodeKey(node.mapId, node.x, node.y, node.z),
                            OffMeshValue(answer.distance, answer.steep)));
}

// The hazard areas an answer's path walks on (`areas=all`), read at its points and the middle of each leg: the nearest
// polygon bots may use within a yard sideways and 4 yd up or down is lava, slime or fatigue water, and no plain polygon
// (ground, water, steep ground) lies within HAZARD_PLAIN yd of the point, so a bank that borders a hazard does not
// count. The end of a hop was copied in and is skipped
std::uint32_t AreasUnder(Map* map, Movement::PointsArray const& points, bool hop)
{
    MMapData& mmap = map->GetMapCollisionData().GetMMapData();
    dtNavMeshQuery const* query = mmap.GetNavMeshQuery();
    if (!query)
        return 0;
    dtQueryFilter walk;
    walk.setIncludeFlags(BotIncludeFlags());
    walk.setExcludeFlags(BotExcludeFlags());
    dtQueryFilter plain;
    plain.setIncludeFlags(NAV_GROUND | NAV_WATER | NAV_GROUND_STEEP);
    plain.setExcludeFlags(0);
    float const extents[3] = {1.0f, 4.0f, 1.0f};
    std::uint32_t areas = 0;
    auto const take = [&](G3D::Vector3 const& at)
    {
        float const point[3] = {at.y, at.z, at.x};
        dtPolyRef ref = 0;
        float closest[3] = {0.0f, 0.0f, 0.0f};
        dtMeshTile const* tile = nullptr;
        dtPoly const* poly = nullptr;
        if (!dtStatusSucceed(query->findNearestPoly(point, extents, &walk, &ref, closest)) || !ref ||
            !dtStatusSucceed(mmap.GetNavMesh()->getTileAndPolyByRef(ref, &tile, &poly)) || !poly)
            return;
        std::uint32_t const bit = AreaBit(poly->getArea());
        if (!bit)
            return;
        dtPolyRef bank = 0;
        if (dtStatusSucceed(query->findNearestPoly(point, extents, &plain, &bank, closest)) && bank &&
            dtVdist(closest, point) <= HAZARD_PLAIN)
            return;
        areas |= bit;
    };
    std::size_t const count = hop && !points.empty() ? points.size() - 1 : points.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        take(points[i]);
        if (i + 1 < count)
            take((points[i] + points[i + 1]) * 0.5f);
    }
    return areas;
}

// One engine answer as a bot's MovePoint asks it: smooth path, no forced destination; the bots' polygons. The
// points are copied only for a trace
WalkAnswer Ask(Probe const* probe, Vec const& at, Vec const& target, bool keepPoints)
{
    PathGenerator path(probe);
    path.SetExcludeFlags(BotExcludeFlags());
    if (!job.options.SteepExcluded())
        path.SetNavTerrainCost(NAV_GROUND_STEEP, job.options.steepCost);
    path.SetNavTerrainCost(NAV_WATER, BOT_WATER_COST);
    WalkAnswer answer;
    answer.result = path.CalculatePath(at.x, at.y, at.z, target.x, target.y, target.z, false);
    answer.type = path.GetPathType();
    answer.beforeLast = at;
    answer.last = at;
    Movement::PointsArray const& points = path.GetPath();
    answer.pointCount = static_cast<std::uint32_t>(points.size());
    for (std::size_t i = 1; i < points.size(); ++i)
        answer.length += (points[i] - points[i - 1]).length();
    if (points.size() >= 2)
        answer.beforeLast = {points[points.size() - 2].x, points[points.size() - 2].y, points[points.size() - 2].z};
    if (!points.empty())
        answer.last = {points.back().x, points.back().y, points.back().z};
    if (keepPoints)
        for (G3D::Vector3 const& point : points)
            answer.points.push_back({point.x, point.y, point.z});
    G3D::Vector3 const& actual = path.GetActualEndPosition();
    answer.actualEnd = {actual.x, actual.y, actual.z};
    if (job.options.areasAll)
        answer.areas = AreasUnder(probe->GetMap(), points, IsHop(answer));
    return answer;
}

// The walk a bot makes over the waypoints (RouteCheckRules.h WalkPath; node to node: two), as its movement asks the
// engine; the points it passes go to trace when given
WalkResult WalkDirection(Probe const* probe, std::vector<Vec> const& way, std::mt19937& generator,
                         std::vector<Vec>* trace)
{
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    return WalkPath(
        way, [probe, trace](Vec const& at, Vec const& target) { return Ask(probe, at, target, trace != nullptr); },
        [&generator, &unit]() { return unit(generator); }, trace);
}

struct Both
{
    WalkResult fwd;
    WalkResult rev;
    bool revAsked{false};
    LinkVerdict verdict;
    std::vector<Vec> fwdPath;
    std::vector<Vec> revPath;
};

// The way's first and last points are the link's ends; the reverse walks it backwards and is walked only when the
// forward walk is neither ok nor capped. The cone's samples come from a generator seeded by the key, so a run repeats
Both ProbeBoth(Probe const* probe, std::vector<Vec> const& way, bool fromOff, bool toOff, std::string const& key,
               bool wantPaths)
{
    Both both;
    std::mt19937 generator(static_cast<std::uint32_t>(std::hash<std::string>{}(key)));
    both.fwd = WalkDirection(probe, way, generator, wantPaths ? &both.fwdPath : nullptr);
    Direction const forward = DirectionOf(both.fwd);
    both.revAsked = forward != Direction::Ok && forward != Direction::Capped;
    if (both.revAsked)
    {
        std::vector<Vec> const back(way.rbegin(), way.rend());
        both.rev = WalkDirection(probe, back, generator, wantPaths ? &both.revPath : nullptr);
    }
    both.verdict = ClassifyLink(both.fwd, both.revAsked ? &both.rev : nullptr, way.front(), way.back(), fromOff, toOff);
    return both;
}

void CheckLink(TravelGraph const& graph, std::uint32_t index)
{
    TravelGraphLink const& link = graph.GetLink(index);
    TravelGraphNode const& from = graph.GetNode(link.from);
    TravelGraphNode const& to = graph.GetNode(link.to);
    std::string const fromKey = NodeKey(from.mapId, from.x, from.y, from.z);
    std::string const toKey = NodeKey(to.mapId, to.x, to.y, to.z);
    std::string const key = LinkKey(fromKey, toKey);
    ++job.current.links;
    if (job.options.pathMode && !link.pathCount)
    {
        // Nothing stored to walk (NV.15): a class of its own, listed apart
        ++job.current.byClass[static_cast<int>(Verdict::NoPath)];
        job.rows.push_back(Line("links.nopath", key, VerdictWord(Verdict::NoPath)));
        return;
    }
    Map* map = job.probe->GetMap();
    float minX = std::min(from.x, to.x);
    float maxX = std::max(from.x, to.x);
    float minY = std::min(from.y, to.y);
    float maxY = std::max(from.y, to.y);
    std::vector<Vec> way{At(from)};
    for (TravelGraphPoint const& point : graph.PathOf(link))
    {
        if (point.mapId != from.mapId)
            continue;
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
        if (job.options.pathMode)
            way.push_back({point.x, point.y, point.z});
    }
    way.push_back(At(to));
    // One grid around the stored path: Detour finds no path through a tile that is not loaded
    EnsureGrids(map, minX, maxX, minY, maxY, 1);
    dtNavMesh const* navMesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
    bool const startTile = TileLoaded(navMesh, At(from));
    bool const endTile = TileLoaded(navMesh, At(to));
    Both const both = ProbeBoth(job.probe.get(), way, job.offNodes.count(link.from) != 0,
                                job.offNodes.count(link.to) != 0, key, false);
    ++job.current.byClass[static_cast<int>(both.verdict.verdict)];
    if (both.verdict.verdict == Verdict::Ok)
    {
        if (both.fwd.moves > 1)
            ++job.current.chained;
        // `areas=all`: an ok link whose walk touched lava or fatigue water is listed too (the others have their row)
        if (job.options.areasAll && (both.fwd.areas || (both.revAsked && both.rev.areas)))
            job.rows.push_back(Line("links.areas", key, LinkValue(both.verdict, both.fwd, nullptr, false)));
        return;
    }
    job.rows.push_back(
        Line(both.verdict.verdict == Verdict::Capped ? "links.capped" : "links.class", key,
             LinkValue(both.verdict, both.fwd, both.revAsked ? &both.rev : nullptr, !startTile || !endTile)));
    CountNodeLink(job.nodeLinks, both.verdict.verdict, fromKey, toKey);
}

// A pair of points: a link without a stored link; the points each walk passed are written beside the class
void CheckPair(PairSpec const& pair)
{
    Map* map = job.probe->GetMap();
    EnsureGrids(map, std::min(pair.from.x, pair.to.x), std::max(pair.from.x, pair.to.x),
                std::min(pair.from.y, pair.to.y), std::max(pair.from.y, pair.to.y), 1);
    dtNavMesh const* navMesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
    bool const notile = !TileLoaded(navMesh, pair.from) || !TileLoaded(navMesh, pair.to);
    std::string const key = PairKey(NodeKey(pair.mapId, pair.from.x, pair.from.y, pair.from.z),
                                    NodeKey(pair.mapId, pair.to.x, pair.to.y, pair.to.z), pair.label);
    Both const both = ProbeBoth(job.probe.get(), {pair.from, pair.to}, IsOffMesh(MeshDistance(map, pair.from).distance),
                                IsOffMesh(MeshDistance(map, pair.to).distance), key, true);
    ++job.current.links;
    ++job.current.byClass[static_cast<int>(both.verdict.verdict)];
    if (both.verdict.verdict == Verdict::Ok && both.fwd.moves > 1)
        ++job.current.chained;
    job.rows.push_back(
        Line("pair.class", key, LinkValue(both.verdict, both.fwd, both.revAsked ? &both.rev : nullptr, notile)));
    job.rows.push_back(Line("pair.path.fwd", key, PathValue(both.fwdPath)));
    if (both.revAsked)
        job.rows.push_back(Line("pair.path.rev", key, PathValue(both.revPath)));
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
    if (job.pairsMode)
    {
        if (job.nextPair < job.pairs.size() && job.pairs[job.nextPair].mapId == job.maps[job.mapIndex])
            CheckPair(job.pairs[job.nextPair++]);
        else
        {
            EndMap();
            return job.mapIndex >= job.maps.size();
        }
    }
    else if (job.nextNode < job.nodes.size())
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
        std::filesystem::path(sLog->GetLogsDir()) /
        (std::string("route-check-") + Title() + "-" + job.options.Name() + "-" + stamp + ".tsv");
    double const elapsed = Seconds(Clock::now() - job.started);
    TravelGraphStats const& stats = graph.Stats();
    std::string const prefix = std::string("ROUTE check ") + Title() + ": ";
    std::vector<std::string> summary;
    MapCounts total;
    for (MapCounts const& counts : job.done)
    {
        summary.push_back(prefix + SummaryLine("map " + std::to_string(counts.mapId), counts));
        total.Add(counts);
    }
    summary.push_back(prefix + SummaryLine("total", total));
    for (std::string const& note : job.notes)
        summary.push_back(prefix + note);
    summary.push_back(Acore::StringFormat(
        "{}{}: {:.1f} s elapsed, {:.1f} s on the world thread in {} slices, {} grids created; report {}", prefix,
        job.stop ? "stopped" : "done", elapsed, Seconds(job.busy), job.slices, job.gridsCreated, file.string()));
    std::ofstream out(file, std::ios::binary);
    if (out)
    {
        out << "# route check " << Title() << " [model " << job.options.ModelWord() << ", mode "
            << job.options.ModeWord() << ", steep " << job.options.SteepWord() << ", areas "
            << job.options.AreasOption() << "] (PLAN navigation §3.2 check 3), " << stamp << "\n";
        out << "# graph: " << stats.nodes << " nodes, " << stats.links << " links ("
            << stats.linksByType[static_cast<int>(TravelLinkType::Walk)] << " walk), " << stats.points
            << " path points\n";
        out << "# off the mesh: over " << OFFMESH_LIMIT
            << " yd from the bots' mesh or none; cause: a walk stopped within " << CAUSE_RADIUS
            << " yd (2D) of the end node\n";
        out << "# walk: the bot's, modelled (orders within " << WALK_ORDER_RANGE << " yd, arrival within " << WALK_GOAL
            << " yd 3D, stuck after " << WALK_STUCK_MOVES << " moves without " << WALK_GAIN << " yd gain, at most "
            << WALK_MAX_MOVES << " moves); ok: on the mesh only, shortcut: with a straight move (also the hop: an "
            << "INCOMPLETE path whose last leg is over " << HOP_MIN << " yd, every core; MMAP_VERSION " << MMAP_VERSION
            << "); mode path: over the stored points, "
            << "no-path: none stored\n";
        out << "# navmesh: " << sWorld->GetDataPath() << "mmaps\n";
        out << "# " << (job.stop ? "stopped" : "complete") << (job.limit ? ", first links only" : "") << ", "
            << Acore::StringFormat("{:.1f}", elapsed) << " s\n";
        for (std::string const& line : ReportLines(job.done, std::move(job.rows)))
            out << line << "\n";
    }
    else
        summary.push_back(prefix + "could not write " + file.string());
    for (std::string const& line : summary)
        LOG_INFO("playerbots", "{}", line);
    job.summary = std::move(summary);
    job.rows.clear();
    job.pairs.clear();
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

    std::string GetName() const override { return std::string("route check ") + Title(); }
};

bool QueueSlice()
{
    return PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<SliceOperation>());
}

void PrintStatus(ChatHandler* handler)
{
    if (job.running)
    {
        if (job.pairsMode)
            handler->PSendSysMessage("ROUTE check pairs: running, map {} ({} of {}): {} of {} pairs; {:.1f} s elapsed",
                                     job.maps[std::min(job.mapIndex, job.maps.size() - 1)], job.mapIndex + 1,
                                     job.maps.size(), job.nextPair, job.pairs.size(),
                                     Seconds(Clock::now() - job.started));
        else
            handler->PSendSysMessage(
                "ROUTE check links: running, map {} ({} of {}): {} of {} nodes, {} of {} links; {:.1f} s elapsed",
                job.maps[std::min(job.mapIndex, job.maps.size() - 1)], job.mapIndex + 1, job.maps.size(), job.nextNode,
                job.nodes.size(), job.nextLink, job.links.size(), Seconds(Clock::now() - job.started));
        return;
    }
    if (job.summary.empty())
    {
        handler->PSendSysMessage(
            "ROUTE check: no check has run (playerbots route check links [map [count]] | pairs <file>)");
        return;
    }
    for (std::string const& line : job.summary)
        handler->PSendSysMessage("{}", line);
}

std::vector<std::string> Words(char const* args)
{
    std::string const text = args ? args : "";
    std::vector<std::string> words;
    for (std::size_t at = 0; at < text.size();)
    {
        std::size_t const end = std::min(text.find(' ', at), text.size());
        if (end > at)
            words.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return words;
}

// `status` and `stop` serve both commands: one job at a time; the caller holds jobMutex
bool HandleStatusStop(ChatHandler* handler, std::vector<std::string> const& words)
{
    if (!words.empty() && words[0] == "status")
    {
        PrintStatus(handler);
        return true;
    }
    if (!words.empty() && words[0] == "stop")
    {
        job.stop = job.running;
        handler->SendSysMessage(job.running ? "ROUTE check: stopping after this slice"
                                            : "ROUTE check: no check is running");
        return true;
    }
    return false;
}

// Starts the queued job (`job` is filled); the caller holds jobMutex
bool StartJob(ChatHandler* handler, std::vector<std::uint32_t> const& maps, std::string const& detail)
{
    job.running = true;
    job.maps = maps;
    job.started = Clock::now();
    if (!QueueSlice())
    {
        job.running = false;
        handler->SendErrorMessage("ROUTE check {}: the world-thread queue refused the job", Title());
        return false;
    }
    std::string mapList;
    for (std::uint32_t const mapId : maps)
        mapList += (mapList.empty() ? "" : " ") + std::to_string(mapId);
    handler->PSendSysMessage("ROUTE check {}: started on map(s) {}{}; poll playerbots route check links status",
                             Title(), mapList, detail);
    return true;
}
}  // namespace

bool RouteCheck::HandleLinksCommand(ChatHandler* handler, char const* args)
{
    std::lock_guard<std::mutex> guard(jobMutex);
    std::string argText = args ? args : "";
    std::vector<std::string> words = Words(argText.c_str());
    CheckOptions options;
    std::string bad;
    if (!TakeOptions(argText, words, options, bad))
    {
        handler->SendErrorMessage("ROUTE check links: {}: use model=v20|v21, mode=nodes|path, steep=excluded|<cost>",
                                  bad);
        return false;
    }
    if (HandleStatusStop(handler, words))
        return true;
    if (job.running)
    {
        handler->PSendSysMessage("ROUTE check {}: a check is running (playerbots route check links status|stop)",
                                 Title());
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
            handler->SendErrorMessage(
                "ROUTE check links: usage: [map [count]] | all | status | stop (+ model=v20|v21 "
                "mode=nodes|path steep=excluded|<cost>)");
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
    job.options = options;
    job.limit = limit;
    return StartJob(handler, maps,
                    " [" + options.Name() + "]" +
                        (limit ? Acore::StringFormat(", first {} walk links each", limit) : std::string()));
}

// `playerbots route check pairs <file>`: the file is read here once (small), never in a slice
bool RouteCheck::HandlePairsCommand(ChatHandler* handler, char const* args)
{
    std::lock_guard<std::mutex> guard(jobMutex);
    std::string path = args ? args : "";
    std::vector<std::string> words = Words(path.c_str());
    CheckOptions options;
    std::string bad;
    if (!TakeOptions(path, words, options, bad))
    {
        handler->SendErrorMessage("ROUTE check pairs: {}: use model=v20|v21, steep=excluded|<cost>", bad);
        return false;
    }
    while (!path.empty() && path.back() == ' ')
        path.pop_back();
    if (HandleStatusStop(handler, words))
        return true;
    if (options.pathMode)
    {
        handler->SendErrorMessage("ROUTE check pairs: a pair has no stored points, so mode=path does not apply");
        return false;
    }
    if (job.running)
    {
        handler->PSendSysMessage("ROUTE check {}: a check is running (playerbots route check links status|stop)",
                                 Title());
        return true;
    }
    path = path.substr(path.find_first_not_of(' ') == std::string::npos ? 0 : path.find_first_not_of(' '));
    std::ifstream in{std::filesystem::path(path)};
    if (words.empty() || !in)
    {
        handler->SendErrorMessage(
            "ROUTE check pairs: usage: <file> (lines: map x1 y1 z1 x2 y2 z2 [label]) | status | stop");
        return false;
    }
    std::vector<PairSpec> pairs;
    std::string line;
    std::uint32_t skipped = 0;
    while (std::getline(in, line))
    {
        PairSpec pair;
        if (!ParsePairLine(line, pair))
        {
            if (line.find_first_not_of(" \t\r") != std::string::npos && line[line.find_first_not_of(" \t\r")] != '#')
                ++skipped;
            continue;
        }
        if (!sMapStore.LookupEntry(pair.mapId))
        {
            ++skipped;
            continue;
        }
        pairs.push_back(std::move(pair));
    }
    if (pairs.empty())
    {
        handler->SendErrorMessage("ROUTE check pairs: no usable pair in the file ({} lines skipped)", skipped);
        return false;
    }
    std::stable_sort(pairs.begin(), pairs.end(),
                     [](PairSpec const& a, PairSpec const& b) { return a.mapId < b.mapId; });
    std::vector<std::uint32_t> maps;
    for (PairSpec const& pair : pairs)
        if (maps.empty() || maps.back() != pair.mapId)
            maps.push_back(pair.mapId);
    job = Job();
    job.pairsMode = true;
    job.options = options;
    std::size_t const count = pairs.size();
    job.pairs = std::move(pairs);
    return StartJob(handler, maps,
                    " [" + options.Name() + "]" + Acore::StringFormat(", {} pairs, {} lines skipped", count, skipped));
}
