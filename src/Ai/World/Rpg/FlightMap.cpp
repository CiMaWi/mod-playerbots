/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FlightMap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <thread>
#include <unordered_set>

#include "AreaDefines.h"
#include "DBCStores.h"
#include "GridDefines.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "MapTree.h"
#include "ModelInstance.h"
#include "PerfMonitor.h"
#include "PlannerIndexes.h"
#include "PlayerbotAIConfig.h"
#include "StringFormat.h"
#include "VMapDefinitions.h"
#include "VMapMgr2.h"
#include "World.h"
#include "WorldModel.h"

namespace Planner
{
namespace
{
enum class FlightMapState : uint8
{
    NotStarted,
    Building,
    Ready,
    Failed
};

struct FlyingMap
{
    uint32 id;
    char const* name;
};

constexpr std::array<FlyingMap, 2> FLYING_MAPS = {{{530, "Outland"}, {571, "Northrend"}}};
constexpr int32 GRIDS = MAX_NUMBER_OF_GRIDS;
constexpr int32 SQUARES = MAP_RESOLUTION;  // height squares per grid side: a V8 point at the centre, V9 at the corners
constexpr int32 AREAS = 16;                // area ids per grid side
constexpr float COLLISION_HEIGHT = 2.0f;   // only for the liquid lookup
constexpr double CORNER_INSET = 0.01;      // a square's V9 corner sample, just inside the square
constexpr double BYTES_PER_KB = 1024.0;
constexpr float WMO_PROBE_YARDS = 1.0f;   // a taxi node's WMO area is read this far above the node
constexpr uint32 MOGP_OUTDOOR = 0x8;      // a WMO group's outdoor flag, as Map::GetFullTerrainStatusForPosition
constexpr uint32 WMO_AREA_OUTDOOR = 0x4;  // WMOAreaTable flags overriding it
constexpr uint32 WMO_AREA_INDOOR = 0x2;

std::atomic<FlightMapState> flightState{FlightMapState::NotStarted};
std::atomic<bool> flightStop{false};
std::thread flightLoader;
SharedSlot<FlightRules::FlightMapSet> flightBook;

struct TaxiSpot
{
    uint32 node;
    uint32 mapId;
    float x;
    float y;
    float z;
};

struct LoaderInput
{
    std::string dataPath;
    std::vector<bool> openArea;       // by area id: a flying area (IsFlyable) without the no-fly flag
    std::vector<TaxiSpot> taxiSpots;  // the flying maps' taxi nodes: candidates for takeoff spots on platforms
    FlightRules::FlightParams params;
};

uint32 ElapsedMs(std::chrono::steady_clock::time_point started)
{
    return static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
}

// The world coordinate at a fraction of grid `grid` (0 at its high edge, `steps` at its low edge): grid g covers
// world x (or y) in ((31 - g), (32 - g)] grids, as the core names the terrain files and reads them
double WorldAt(int32 grid, double step, int32 steps)
{
    return SIZE_OF_GRIDS * (CENTER_GRID_ID - (static_cast<double>(grid) * steps + step) / steps);
}

// The cells under a model floating in the air (WU11b fix 12: four fly legs from Dragonblight into Zul'Drak, planned
// at the plateau's level, flew into the necropolis Voltarus over Zeramas; the core's indoor check took the mount and
// the bots came down), closed as a no-fly area's: each WMO spawn of the map's vmap tiles (the spawn records only, no
// model is loaded) whose lowest point FloatsOver a cell's ground. A WMO with a taxi node in its box is a platform the
// bots fly to (Dalaran: Krasus' Landing, the platform rule and MarkTakeoffs) and stays open. Returns the cells closed
uint32 CloseUnderFloatingModels(FlyingMap const& flying, LoaderInput const& input, double origin, double cell,
                                int32 side, std::vector<float> const& highest, std::vector<uint8>& closed)
{
    auto const cellAt = [origin, cell, side](float v)
    { return std::clamp<int32>(static_cast<int32>(std::floor((v - origin) / cell)), 0, side - 1); };
    std::string const base = input.dataPath + "vmaps/";
    std::unordered_set<uint32> seen;  // a model crossing tiles is in each tile's file
    uint32 count = 0;
    for (uint32 tileX = 0; tileX < static_cast<uint32>(GRIDS); ++tileX)
        for (uint32 tileY = 0; tileY < static_cast<uint32>(GRIDS); ++tileY)
        {
            if (flightStop.load(std::memory_order_relaxed))
                return count;
            std::string const file = base + VMAP::StaticMapTree::getTileFileName(flying.id, tileX, tileY);
            FILE* tile = std::fopen(file.c_str(), "rb");
            if (!tile)
                continue;
            // As StaticMapTree::LoadMapTile reads it: the magic, the spawn count, each spawn and its tree index
            char magic[8];
            uint32 spawns = 0;
            bool ok = std::fread(magic, 1, sizeof(magic), tile) == sizeof(magic) &&
                      std::memcmp(magic, VMAP::VMAP_MAGIC, sizeof(magic)) == 0 &&
                      std::fread(&spawns, sizeof(spawns), 1, tile) == 1;
            for (uint32 i = 0; ok && i < spawns; ++i)
            {
                VMAP::ModelSpawn spawn;
                uint32 treeIndex = 0;
                ok = VMAP::ModelSpawn::readFromFile(tile, spawn) &&
                     std::fread(&treeIndex, sizeof(treeIndex), 1, tile) == 1;
                if (!ok || (spawn.flags & VMAP::MOD_M2) || !(spawn.flags & VMAP::MOD_HAS_BOUND) ||
                    !seen.insert(spawn.ID).second)
                    continue;
                // The bounds are in the vmap's axes; the conversion is its own inverse
                G3D::Vector3 const a = VMAP::VMapMgr2::convertPositionToInternalRep(
                    spawn.iBound.low().x, spawn.iBound.low().y, spawn.iBound.low().z);
                G3D::Vector3 const b = VMAP::VMapMgr2::convertPositionToInternalRep(
                    spawn.iBound.high().x, spawn.iBound.high().y, spawn.iBound.high().z);
                float const minX = std::min(a.x, b.x);
                float const maxX = std::max(a.x, b.x);
                float const minY = std::min(a.y, b.y);
                float const maxY = std::max(a.y, b.y);
                float const bottom = std::min(a.z, b.z);
                float const top = std::max(a.z, b.z);
                bool const platform = std::any_of(input.taxiSpots.begin(), input.taxiSpots.end(),
                                                  [&](TaxiSpot const& spot)
                                                  {
                                                      return spot.mapId == flying.id && spot.x >= minX &&
                                                             spot.x <= maxX && spot.y >= minY && spot.y <= maxY &&
                                                             spot.z >= bottom && spot.z <= top;
                                                  });
                if (platform)
                    continue;
                for (int32 cy = cellAt(minY); cy <= cellAt(maxY); ++cy)
                    for (int32 cx = cellAt(minX); cx <= cellAt(maxX); ++cx)
                    {
                        size_t const index = static_cast<size_t>(cy) * side + cx;
                        if (closed[index] || !(highest[index] > MIN_HEIGHT) ||
                            !FlightRules::FloatsOver(bottom, highest[index]))
                            continue;
                        closed[index] = 1;
                        ++count;
                    }
            }
            std::fclose(tile);
        }
    return count;
}

// One map's height map: the highest valid terrain or liquid sample per cell, closed where any of the cell's area
// samples is not a flying area or has the no-fly flag, where a model floats over it (CloseUnderFloatingModels), or
// the cell has no terrain above the map floor (MIN_HEIGHT: the void around Outland's islands); cropped to the open
// cells' box.
// False when the shutdown stopped it
bool BuildMap(FlyingMap const& flying, LoaderInput const& input, FlightRules::HeightMap& out, uint32& unreadable)
{
    int32 const cellsPerGrid =
        std::max<int32>(1, static_cast<int32>(std::lround(SIZE_OF_GRIDS / input.params.cellYards)));
    double const cell = SIZE_OF_GRIDS / cellsPerGrid;
    int32 const side = GRIDS * cellsPerGrid;
    double const origin = -SIZE_OF_GRIDS * CENTER_GRID_ID;
    float const none = -std::numeric_limits<float>::infinity();
    std::vector<float> highest(static_cast<size_t>(side) * side, none);
    std::vector<uint8> closed(static_cast<size_t>(side) * side, 0);
    auto const cellOf = [cell, origin, side](double x, double y)
    {
        int32 const cx = std::clamp<int32>(static_cast<int32>(std::floor((x - origin) / cell)), 0, side - 1);
        int32 const cy = std::clamp<int32>(static_cast<int32>(std::floor((y - origin) / cell)), 0, side - 1);
        return static_cast<size_t>(cy) * side + cx;
    };
    auto const valid = [](float height) { return height > INVALID_HEIGHT + 1.0f; };

    for (int32 gx = 0; gx < GRIDS; ++gx)
        for (int32 gy = 0; gy < GRIDS; ++gy)
        {
            if (flightStop.load(std::memory_order_relaxed))
                return false;
            std::string const file =
                Acore::StringFormat("{}maps/{:03}{:02}{:02}.map", input.dataPath, flying.id, gx, gy);
            GridTerrainData terrain;
            TerrainMapDataReadResult const result = terrain.Load(file);
            if (result == TerrainMapDataReadResult::NotFound)
                continue;
            if (result != TerrainMapDataReadResult::Success)
            {
                ++unreadable;
                continue;
            }
            ++out.files;
            for (int32 i = 0; i < SQUARES; ++i)
                for (int32 j = 0; j < SQUARES; ++j)
                {
                    float const x = static_cast<float>(WorldAt(gx, i + 0.5, SQUARES));
                    float const y = static_cast<float>(WorldAt(gy, j + 0.5, SQUARES));
                    float const centre = terrain.getHeight(x, y);
                    float const corner = terrain.getHeight(static_cast<float>(WorldAt(gx, i + CORNER_INSET, SQUARES)),
                                                           static_cast<float>(WorldAt(gy, j + CORNER_INSET, SQUARES)));
                    float ground = none;
                    if (valid(centre))
                    {
                        ground = centre;
                        LiquidData const liquid = terrain.GetLiquidData(x, y, centre, COLLISION_HEIGHT, {});
                        if (liquid.Status != LIQUID_MAP_NO_WATER)
                            ground = std::max(ground, liquid.Level);
                    }
                    if (valid(corner))
                        ground = std::max(ground, corner);
                    float& best = highest[cellOf(x, y)];
                    best = std::max(best, ground);
                }
            for (int32 a = 0; a < AREAS; ++a)
                for (int32 b = 0; b < AREAS; ++b)
                {
                    double const x = WorldAt(gx, a + 0.5, AREAS);
                    double const y = WorldAt(gy, b + 0.5, AREAS);
                    uint16 const area = terrain.getArea(static_cast<float>(x), static_cast<float>(y));
                    if (area >= input.openArea.size() || !input.openArea[area])
                        closed[cellOf(x, y)] = 1;
                }
        }
    out.floatingCells = CloseUnderFloatingModels(flying, input, origin, cell, side, highest, closed);
    if (flightStop.load(std::memory_order_relaxed))
        return false;

    int32 minX = side;
    int32 minY = side;
    int32 maxX = -1;
    int32 maxY = -1;
    auto const open = [&highest, &closed](size_t index) { return !closed[index] && highest[index] > MIN_HEIGHT; };
    for (int32 cy = 0; cy < side; ++cy)
        for (int32 cx = 0; cx < side; ++cx)
            if (open(static_cast<size_t>(cy) * side + cx))
            {
                minX = std::min(minX, cx);
                minY = std::min(minY, cy);
                maxX = std::max(maxX, cx);
                maxY = std::max(maxY, cy);
            }
    out.mapId = flying.id;
    out.cellYards = static_cast<float>(cell);
    if (maxX < 0)
        return true;  // no open cell: an empty map
    out.originX = static_cast<float>(origin + minX * cell);
    out.originY = static_cast<float>(origin + minY * cell);
    out.width = maxX - minX + 1;
    out.height = maxY - minY + 1;
    out.ground.assign(static_cast<size_t>(out.width) * out.height, FlightRules::CLOSED);
    for (int32 cy = minY; cy <= maxY; ++cy)
        for (int32 cx = minX; cx <= maxX; ++cx)
        {
            size_t const index = static_cast<size_t>(cy) * side + cx;
            if (!open(index))
                continue;
            float const ground = std::clamp(std::ceil(highest[index]), -32767.0f, 32767.0f);
            out.ground[static_cast<size_t>(cy - minY) * out.width + (cx - minX)] = static_cast<int16>(ground);
        }
    out.Finish();
    return true;
}

// The taxi nodes on platforms (WU11b fix 7: Krasus' Landing over Dalaran's no-fly city): a node more than
// PLATFORM_YARDS above its open cell's ground is a takeoff spot when the WMO area under it lets a bot take off there,
// outdoors in a flying area without the no-fly flag (CheckMountStateAction::NoTakeoffHere's rule). The height map
// knows only the terrain's areas; the map's vmap tree, read here on its own for the platforms' tiles and dropped
// after, knows the WMO's
void MarkTakeoffs(FlyingMap const& flying, LoaderInput const& input, FlightRules::HeightMap& map)
{
    std::unique_ptr<VMAP::StaticMapTree> tree;
    std::unordered_set<uint32> tiles;
    for (TaxiSpot const& spot : input.taxiSpots)
    {
        if (spot.mapId != flying.id || !map.IsOpen(spot.x, spot.y) ||
            spot.z - map.Ground(map.CellX(spot.x), map.CellY(spot.y)) <= FlightRules::PLATFORM_YARDS)
            continue;
        if (!tree)
        {
            tree = std::make_unique<VMAP::StaticMapTree>(flying.id, input.dataPath + "vmaps");
            if (!tree->InitMap(VMAP::VMapMgr2::getMapFileName(flying.id)))
            {
                LOG_ERROR("playerbots", "Flight map: no vmap tree of map {} ({}) under {}vmaps; no takeoff spots",
                          flying.id, flying.name, input.dataPath);
                return;
            }
        }
        // The vmap tile of a point, as its terrain file (WorldAt)
        uint32 const tileX = static_cast<uint32>(
            std::clamp<int32>(static_cast<int32>(std::floor(CENTER_GRID_ID - spot.x / SIZE_OF_GRIDS)), 0, GRIDS - 1));
        uint32 const tileY = static_cast<uint32>(
            std::clamp<int32>(static_cast<int32>(std::floor(CENTER_GRID_ID - spot.y / SIZE_OF_GRIDS)), 0, GRIDS - 1));
        if (tiles.insert(VMAP::StaticMapTree::packTileID(tileX, tileY)).second)
            tree->LoadMapTile(tileX, tileY);
        VMAP::LocationInfo info;
        G3D::Vector3 const probe =
            VMAP::VMapMgr2::convertPositionToInternalRep(spot.x, spot.y, spot.z + WMO_PROBE_YARDS);
        uint32 area = 0;
        bool outdoors = false;
        if (tree->GetLocationInfo(probe, info) && info.hitModel && info.hitInstance)
        {
            outdoors = (info.hitModel->GetMogpFlags() & MOGP_OUTDOOR) != 0;
            if (WMOAreaTableEntry const* wmo =
                    GetWMOAreaTableEntryByTripple(info.rootId, info.hitInstance->adtId, info.hitModel->GetWmoID()))
            {
                area = wmo->areaId;
                if (wmo->Flags & WMO_AREA_OUTDOOR)
                    outdoors = true;
                else if (wmo->Flags & WMO_AREA_INDOOR)
                    outdoors = false;
            }
        }
        bool const takeoff = area && outdoors && area < input.openArea.size() && input.openArea[area];
        LOG_INFO("playerbots", "Flight map: taxi node {} at ({:.1f}, {:.1f}, {:.1f}) on a platform, WMO area {} {}: {}",
                 spot.node, spot.x, spot.y, spot.z, area, outdoors ? "outdoors" : "indoors",
                 takeoff ? "a takeoff spot" : "no takeoff");
        if (takeoff)
            map.takeoffs.push_back({spot.x, spot.y, spot.z});
    }
}

char const* MapName(uint32 mapId)
{
    for (FlyingMap const& flying : FLYING_MAPS)
        if (flying.id == mapId)
            return flying.name;
    return "?";
}

std::string MapLine(FlightRules::HeightMap const& map)
{
    return Acore::StringFormat(
        "{} (map {}): {}x{} cells of {:.1f} yd, {} open, {} closed ({} under floating models), ground {}..{} yd, "
        "{} terrain files, {} takeoff spots on platforms, built in {} ms, {:.1f} KB",
        MapName(map.mapId), map.mapId, map.width, map.height, map.cellYards, map.openCells, map.closedCells,
        map.floatingCells, map.minGround, map.maxGround, map.files, map.takeoffs.size(), map.buildMs,
        static_cast<double>(map.Bytes()) / BYTES_PER_KB);
}

void LoadFlightMaps(LoaderInput const& input)
{
    auto const started = std::chrono::steady_clock::now();
    try
    {
        auto set = std::make_shared<FlightRules::FlightMapSet>();
        set->params = input.params;
        size_t bytes = 0;
        for (FlyingMap const& flying : FLYING_MAPS)
        {
            auto const mapStarted = std::chrono::steady_clock::now();
            FlightRules::HeightMap map;
            uint32 unreadable = 0;
            if (!BuildMap(flying, input, map, unreadable))
            {
                flightState.store(FlightMapState::Failed, std::memory_order_release);
                LOG_INFO("playerbots", "Flight map: stopped by the shutdown");
                return;
            }
            if (!map.Empty())
                MarkTakeoffs(flying, input, map);
            map.buildMs = ElapsedMs(mapStarted);
            if (unreadable)
                LOG_ERROR("playerbots", "Flight map: {} unreadable terrain files for map {} ({})", unreadable,
                          flying.id, flying.name);
            if (!map.files || map.Empty())
            {
                LOG_ERROR("playerbots", "Flight map: {} for map {} ({}) under {}maps; no flying legs there",
                          map.files ? "no open cell" : "no terrain files", flying.id, flying.name, input.dataPath);
                continue;
            }
            LOG_INFO("playerbots", "Flight map: {}", MapLine(map));
            bytes += map.Bytes();
            set->maps.push_back(std::move(map));
        }
        set->buildMs = ElapsedMs(started);
        bool const any = !set->maps.empty();
        LOG_INFO("playerbots", "Flight maps built in {} ms on their own thread: {} of {} maps, {:.1f} KB", set->buildMs,
                 set->maps.size(), FLYING_MAPS.size(), static_cast<double>(bytes) / BYTES_PER_KB);
        flightBook.Publish(std::move(set));
        flightState.store(any ? FlightMapState::Ready : FlightMapState::Failed, std::memory_order_release);
    }
    catch (std::exception const& e)
    {
        flightState.store(FlightMapState::Failed, std::memory_order_release);
        LOG_ERROR("playerbots", "Flight map: the build failed: {}", e.what());
    }
}
}  // namespace

void StartFlightMap()
{
    FlightMapState expected = FlightMapState::NotStarted;
    if (!flightState.compare_exchange_strong(expected, FlightMapState::Building))
        return;  // once: the terrain files do not change while the server runs

    LoaderInput input;
    input.dataPath = sWorld->GetDataPath();
    input.openArea.assign(sAreaTableStore.GetNumRows(), false);
    uint32 flyable = 0;
    // Wintergrasp (the zone and its areas) stays closed too: its battle every few hours takes the flying mounts
    // (Restricted Flight Area), so a leg planned in peace can end in a fall (WU11b fix 14: Ermini over Flamewatch
    // Tower); the bots fly around it
    for (uint32 id = 0; id < sAreaTableStore.GetNumRows(); ++id)
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(id))
            if (area->IsFlyable() && !(area->flags & AREA_FLAG_NO_FLY_ZONE) && area->ID != AREA_WINTERGRASP &&
                area->zone != AREA_WINTERGRASP)
            {
                input.openArea[id] = true;
                ++flyable;
            }
    for (uint32 id = 0; id < sTaxiNodesStore.GetNumRows(); ++id)
        if (TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(id))
            for (FlyingMap const& flying : FLYING_MAPS)
                if (node->map_id == flying.id)
                    input.taxiSpots.push_back({node->ID, node->map_id, node->x, node->y, node->z});
    UpkeepConfig const& upkeep = sPlayerbotAIConfig.Upkeep;
    input.params.cellYards = static_cast<float>(upkeep.HeightCellYards);
    input.params.stretchYards = static_cast<float>(upkeep.FlightStretchYards);
    input.params.clearanceMinYards = static_cast<float>(upkeep.FlightClearanceMinYards);
    input.params.clearanceMaxYards = static_cast<float>(upkeep.FlightClearanceMaxYards);
    input.params.groundHopYards = static_cast<float>(upkeep.GroundHopYards);
    LOG_INFO("server.loading",
             "Flight map: reading the terrain of Outland and Northrend on its own thread ({} "
             "flying areas)",
             flyable);
    try
    {
        flightLoader = std::thread([input = std::move(input)]() { LoadFlightMaps(input); });
    }
    catch (std::exception const& e)
    {
        flightState.store(FlightMapState::Failed, std::memory_order_release);
        LOG_ERROR("server.loading", "Flight map: the loader thread did not start: {}", e.what());
    }
}

void StopFlightMap()
{
    flightStop.store(true, std::memory_order_relaxed);
    if (flightLoader.joinable())
        flightLoader.join();
}

std::shared_ptr<FlightRules::FlightMapSet const> FlightMaps() { return flightBook.Current(); }

bool FlightMapsReady() { return flightState.load(std::memory_order_acquire) == FlightMapState::Ready; }

std::vector<std::string> FlightMapStatus()
{
    FlightMapState const state = flightState.load(std::memory_order_acquire);
    std::shared_ptr<FlightRules::FlightMapSet const> const set =
        state == FlightMapState::Ready ? FlightMaps() : nullptr;
    if (!set)
        return {Acore::StringFormat("PLANNER flight maps: {}", state == FlightMapState::Building ? "building"
                                                               : state == FlightMapState::Failed ? "failed"
                                                                                                 : "not started")};
    std::vector<std::string> lines;
    lines.push_back(
        Acore::StringFormat("PLANNER flight maps: ready, built in {} ms on their own thread", set->buildMs));
    for (FlyingMap const& flying : FLYING_MAPS)
    {
        FlightRules::HeightMap const* map = set->Find(flying.id);
        lines.push_back(map ? "PLANNER flight map " + MapLine(*map)
                            : Acore::StringFormat("PLANNER flight map {} (map {}): none", flying.name, flying.id));
    }
    return lines;
}

FlightRules::FlightRoute SearchFlight(FlightRules::FlightRequest const& request)
{
    static FlightRules::HeightMap const noMap;
    std::shared_ptr<FlightRules::FlightMapSet const> const set = FlightMapsReady() ? FlightMaps() : nullptr;
    FlightRules::HeightMap const* map = set ? set->Find(request.mapId) : nullptr;
    PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "flight search");
    auto const started = row ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    FlightRules::FlightRoute route =
        FlightRules::Search(map ? *map : noMap, request, set ? set->params : FlightRules::FlightParams{});
    if (row)
        row->Record(static_cast<uint64>(std::max<long long>(
            0, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
                   .count())));
    return route;
}
}  // namespace Planner
