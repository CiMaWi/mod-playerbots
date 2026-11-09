/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERINDEXES_H
#define _PLAYERBOT_PLANNERINDEXES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// What the planner reads (travel plan WT12b, PLAN 3.8): the immutable indexes (zones, services, hubs) built once on a
// worker from a copy the world thread gathered, and the crowding counts the world thread publishes every few seconds.
// No game header: the selftest (tests/planner-indexes-selftest.cpp) builds them from synthetic input. The travel graph
// is the planner's third index; it has its own handle (TravelGraph::Instance(), IsReady()) and is not copied here
namespace Planner
{
// The creature template's npcflag bits used here; PlannerIndexes.cpp checks them against the core's UNIT_NPC_FLAG_*
namespace NpcFlag
{
constexpr std::uint32_t QUESTGIVER = 0x00000002;
constexpr std::uint32_t TRAINER = 0x00000010;
constexpr std::uint32_t TRAINER_CLASS = 0x00000020;
constexpr std::uint32_t TRAINER_PROFESSION = 0x00000040;
constexpr std::uint32_t VENDOR = 0x00000080;
constexpr std::uint32_t REPAIR = 0x00001000;
constexpr std::uint32_t FLIGHTMASTER = 0x00002000;
constexpr std::uint32_t INNKEEPER = 0x00010000;
constexpr std::uint32_t BANKER = 0x00020000;
constexpr std::uint32_t AUCTIONEER = 0x00200000;
constexpr std::uint32_t SERVICES = QUESTGIVER | TRAINER | TRAINER_CLASS | TRAINER_PROFESSION | VENDOR | REPAIR |
                                   FLIGHTMASTER | INNKEEPER | BANKER | AUCTIONEER;
}  // namespace NpcFlag

enum class ServiceKind : std::uint8_t
{
    Vendor,
    BagVendor,  // a vendor whose list holds a bag
    Repairer,
    Trainer,  // class or profession: Service::npcFlags tells which
    Innkeeper,
    FlightMaster,
    Banker,
    Mailbox,  // a gameobject
    Auctioneer,
    QuestGiver,
    Count
};
constexpr std::size_t SERVICE_KINDS = static_cast<std::size_t>(ServiceKind::Count);
constexpr std::array<char const*, SERVICE_KINDS> SERVICE_NAMES = {"vendor",     "bagvendor",    "repairer", "trainer",
                                                                  "innkeeper",  "flightmaster", "banker",   "mailbox",
                                                                  "auctioneer", "questgiver"};

inline char const* ServiceName(ServiceKind kind) { return SERVICE_NAMES[static_cast<std::size_t>(kind)]; }

inline std::optional<ServiceKind> ParseServiceKind(std::string_view name)
{
    for (std::size_t i = 0; i < SERVICE_KINDS; ++i)
        if (name == SERVICE_NAMES[i])
            return static_cast<ServiceKind>(i);
    return std::nullopt;
}

enum class ZoneSide : std::uint8_t
{
    Contested,
    Alliance,
    Horde,
    Sanctuary
};

// The world thread's copy (GatherWorld): one creature or mailbox spawn with a service
struct SpawnRecord
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::uint32_t entry{0};
    std::uint32_t npcFlags{0};
    std::uint32_t factionTemplate{0};
    std::uint32_t zoneId{0};
    std::uint32_t areaId{0};
    std::uint8_t bagSlots{0};  // the largest bag the vendor sells, 0 = none
    bool mailbox{false};
};

struct ZoneRecord
{
    std::uint32_t zoneId{0};
    std::uint32_t low{0};  // level bracket, 0/0 = none known
    std::uint32_t high{0};
    ZoneSide side{ZoneSide::Contested};
    std::uint32_t mapId{0};  // the continent
};

struct GatherInput
{
    std::vector<SpawnRecord> spawns;
    std::vector<ZoneRecord> zones;
    std::uint32_t unresolved{0};  // spawns without a map or area at gather time, skipped
};

struct Service
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::uint32_t entry{0};  // creature or gameobject template
    std::uint32_t npcFlags{0};
    std::uint32_t factionTemplate{0};
    std::uint32_t zoneId{0};
    std::uint32_t areaId{0};
    std::uint32_t hub{0};  // index into hubs, NO_HUB outside one
    std::uint8_t bagSlots{0};
    bool mailbox{false};
};

struct Hub
{
    std::uint32_t mapId{0};
    float x{0.0f};  // centroid
    float y{0.0f};
    float z{0.0f};
    float radius{0.0f};       // farthest member from the centroid
    std::uint32_t zoneId{0};  // the members' most common zone and area
    std::uint32_t areaId{0};
    std::array<std::uint16_t, SERVICE_KINDS> counts{};
    std::vector<std::uint32_t> members;  // indices into services

    std::uint16_t Count(ServiceKind kind) const { return counts[static_cast<std::size_t>(kind)]; }
    bool Has(ServiceKind kind) const { return Count(kind) > 0; }
};

struct IndexStats
{
    std::uint32_t spawns{0};
    std::uint32_t unresolved{0};
    std::array<std::uint32_t, SERVICE_KINDS> perKind{};
    std::uint64_t estimatedBytes{0};  // vectors' capacity x element size plus the grid cells: an estimate
    std::uint32_t gatherMs{0};        // world thread
    std::uint32_t buildMs{0};         // worker
};

struct Nearby
{
    std::uint32_t index{0};  // into services or hubs
    float distance{0.0f};
};

constexpr std::uint32_t NO_HUB = 0xFFFFFFFFu;
constexpr float SERVICE_CELL_YARDS = 200.0f;  // nearest-service grid, as TravelGraph's nearest nodes
constexpr float HUB_CELL_YARDS = 100.0f;      // hub clustering: occupied cells joined with their 8 neighbours
constexpr std::uint32_t HUB_MIN_SPAWNS = 4;   // fewer spawns make no hub
constexpr std::int32_t CELL_OFFSET = 1 << 15;
constexpr std::size_t NODE_BYTES = 64;    // a hash node's overhead, for the memory estimate
constexpr float HUB_WIDE_YARDS = 300.0f;  // a hub wider than this may hold more than one place (review note WT12b b)
constexpr std::size_t HUB_WIDEST = 5;     // the widest hubs listed by RadiusStats

// The spread of the hubs' radii (PlannerIndexes::RadiusStats)
struct HubRadii
{
    std::uint32_t count{0};
    float p50{0.0f};
    float p95{0.0f};
    float max{0.0f};
    std::uint32_t wide{0};              // hubs above HUB_WIDE_YARDS
    std::vector<std::uint32_t> widest;  // hub indices, widest first, at most HUB_WIDEST
};

namespace Detail
{
// Clamped into the cell keys' 16-bit range: a huge or non-finite coordinate never overflows the cast (NaN lands on
// the lower edge; queries reject non-finite points first)
inline std::int32_t Cell(float coordinate, float size)
{
    float const cell = std::floor(coordinate / size);
    if (!(cell > static_cast<float>(-CELL_OFFSET)))
        return -CELL_OFFSET;
    if (cell >= static_cast<float>(CELL_OFFSET - 1))
        return CELL_OFFSET - 1;
    return static_cast<std::int32_t>(cell);
}

inline bool Finite(float x, float y, float z) { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }

inline std::uint64_t CellKey(std::uint32_t tag, std::int32_t cx, std::int32_t cy)
{
    return (static_cast<std::uint64_t>(tag) << 32) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx + CELL_OFFSET) & 0xFFFFu) << 16) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy + CELL_OFFSET) & 0xFFFFu));
}

inline float Distance(float ax, float ay, float az, float bx, float by, float bz)
{
    float const dx = ax - bx;
    float const dy = ay - by;
    float const dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The kinds a spawn serves
inline std::vector<ServiceKind> KindsOf(std::uint32_t flags, std::uint8_t bagSlots, bool mailbox)
{
    std::vector<ServiceKind> kinds;
    if (mailbox)
    {
        kinds.push_back(ServiceKind::Mailbox);
        return kinds;
    }
    if (flags & NpcFlag::VENDOR)
    {
        kinds.push_back(ServiceKind::Vendor);
        if (bagSlots)
            kinds.push_back(ServiceKind::BagVendor);
    }
    if (flags & NpcFlag::REPAIR)
        kinds.push_back(ServiceKind::Repairer);
    if (flags & (NpcFlag::TRAINER | NpcFlag::TRAINER_CLASS | NpcFlag::TRAINER_PROFESSION))
        kinds.push_back(ServiceKind::Trainer);
    if (flags & NpcFlag::INNKEEPER)
        kinds.push_back(ServiceKind::Innkeeper);
    if (flags & NpcFlag::FLIGHTMASTER)
        kinds.push_back(ServiceKind::FlightMaster);
    if (flags & NpcFlag::BANKER)
        kinds.push_back(ServiceKind::Banker);
    if (flags & NpcFlag::AUCTIONEER)
        kinds.push_back(ServiceKind::Auctioneer);
    if (flags & NpcFlag::QUESTGIVER)
        kinds.push_back(ServiceKind::QuestGiver);
    return kinds;
}

inline bool SpawnLess(SpawnRecord const& a, SpawnRecord const& b)
{
    return std::tie(a.mapId, a.x, a.y, a.z, a.entry, a.npcFlags, a.mailbox) <
           std::tie(b.mapId, b.x, b.y, b.z, b.entry, b.npcFlags, b.mailbox);
}

template <typename T>
std::uint64_t VectorBytes(std::vector<T> const& v)
{
    return static_cast<std::uint64_t>(v.capacity()) * sizeof(T);
}

// Groups of points whose cells touch: the occupied cells of cellYards per map joined with their 8 neighbours
// (union-find). Groups come in the order of their first cell (map, x, y), members ascending, so canonically ordered
// points give the same groups whatever the input order. pointOf(i) returns {mapId, x, y}
template <typename PointOf>
std::vector<std::vector<std::uint32_t>> ClusterByCells(std::uint32_t count, float cellYards, PointOf pointOf)
{
    std::map<std::uint64_t, std::vector<std::uint32_t>> cells;  // sorted: deterministic
    for (std::uint32_t i = 0; i < count; ++i)
    {
        auto const [mapId, x, y] = pointOf(i);
        cells[CellKey(mapId, Cell(x, cellYards), Cell(y, cellYards))].push_back(i);
    }
    std::vector<std::uint64_t> keys;
    keys.reserve(cells.size());
    for (auto const& [key, list] : cells)
        keys.push_back(key);
    std::vector<std::uint32_t> parent(keys.size());
    for (std::uint32_t i = 0; i < parent.size(); ++i)
        parent[i] = i;
    auto const find = [&parent](std::uint32_t i)
    {
        while (parent[i] != i)
        {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    auto const indexOf = [&keys](std::uint64_t key) -> std::optional<std::uint32_t>
    {
        auto const it = std::lower_bound(keys.begin(), keys.end(), key);
        if (it == keys.end() || *it != key)
            return std::nullopt;
        return static_cast<std::uint32_t>(it - keys.begin());
    };
    for (std::uint32_t i = 0; i < keys.size(); ++i)
    {
        std::uint32_t const tag = static_cast<std::uint32_t>(keys[i] >> 32);
        std::int32_t const cx = static_cast<std::int32_t>((keys[i] >> 16) & 0xFFFFu) - CELL_OFFSET;
        std::int32_t const cy = static_cast<std::int32_t>(keys[i] & 0xFFFFu) - CELL_OFFSET;
        for (std::int32_t dx = -1; dx <= 1; ++dx)
            for (std::int32_t dy = -1; dy <= 1; ++dy)
                if (std::optional<std::uint32_t> const j = indexOf(CellKey(tag, cx + dx, cy + dy)))
                {
                    std::uint32_t const a = find(i);
                    std::uint32_t const b = find(*j);
                    if (a != b)
                        parent[std::max(a, b)] = std::min(a, b);  // the smaller key's cell is the root
                }
    }

    std::map<std::uint32_t, std::vector<std::uint32_t>> groups;  // root -> members, in key order
    for (std::uint32_t i = 0; i < keys.size(); ++i)
    {
        std::vector<std::uint32_t>& group = groups[find(i)];
        std::vector<std::uint32_t> const& list = cells[keys[i]];
        group.insert(group.end(), list.begin(), list.end());
    }
    std::vector<std::vector<std::uint32_t>> result;
    result.reserve(groups.size());
    for (auto& [root, members] : groups)
    {
        std::sort(members.begin(), members.end());
        result.push_back(std::move(members));
    }
    return result;
}
}  // namespace Detail

// Immutable once built. Readers on any thread hold a shared_ptr to it
class PlannerIndexes
{
public:
    std::vector<ZoneRecord> zones;  // sorted by zone id
    std::vector<Service> services;  // in a canonical order (map, position, entry)
    std::vector<Hub> hubs;          // ordered by map, then their first spawn
    IndexStats stats;

    static PlannerIndexes Build(GatherInput input)
    {
        PlannerIndexes indexes;
        indexes.stats.unresolved = input.unresolved;
        indexes.BuildZones(std::move(input.zones));
        indexes.BuildServices(std::move(input.spawns));
        indexes.BuildHubs();
        indexes.stats.estimatedBytes = indexes.EstimateBytes();
        return indexes;
    }

    ZoneRecord const* FindZone(std::uint32_t zoneId) const
    {
        auto const it = std::lower_bound(zones.begin(), zones.end(), zoneId,
                                         [](ZoneRecord const& zone, std::uint32_t id) { return zone.zoneId < id; });
        return it != zones.end() && it->zoneId == zoneId ? &*it : nullptr;
    }

    // Up to k services of the kind nearest to the point on its map, nearest first; within maxYards when given
    std::vector<Nearby> Nearest(ServiceKind kind, std::uint32_t mapId, float x, float y, float z, std::size_t k,
                                float maxYards = 0.0f) const
    {
        std::vector<Nearby> best;
        auto const bounds = _bounds.find(GridTag(kind, mapId));
        if (!k || bounds == _bounds.end() || !Detail::Finite(x, y, z))
            return best;
        Bounds const& b = bounds->second;
        // A point outside the occupied cells starts from the nearest edge cell: every occupied cell is at least as
        // many cells from the point as from that cell, so the ring floor below holds and the rings stay in the span
        std::int32_t const cx = std::clamp(Detail::Cell(x, SERVICE_CELL_YARDS), b.minX, b.maxX);
        std::int32_t const cy = std::clamp(Detail::Cell(y, SERVICE_CELL_YARDS), b.minY, b.maxY);
        std::int32_t const maxRing = std::max({cx - b.minX, b.maxX - cx, cy - b.minY, b.maxY - cy});
        std::uint32_t const tag = GridTag(kind, mapId);
        for (std::int32_t ring = 0; ring <= maxRing; ++ring)
        {
            // Every point in a later ring is at least ring x cell away in the plane
            float const floorDistance = static_cast<float>(ring - 1) * SERVICE_CELL_YARDS;
            if (maxYards > 0.0f && floorDistance > maxYards)
                break;
            if (best.size() >= k && best[k - 1].distance <= floorDistance)
                break;
            for (std::int32_t dx = std::max(-ring, b.minX - cx); dx <= std::min(ring, b.maxX - cx); ++dx)
                for (std::int32_t dy = std::max(-ring, b.minY - cy); dy <= std::min(ring, b.maxY - cy); ++dy)
                {
                    if (std::abs(dx) != ring && std::abs(dy) != ring)
                        continue;
                    auto const cell = _cells.find(Detail::CellKey(tag, cx + dx, cy + dy));
                    if (cell == _cells.end())
                        continue;
                    for (std::uint32_t const index : cell->second)
                    {
                        Service const& s = services[index];
                        float const d = Detail::Distance(x, y, z, s.x, s.y, s.z);
                        if (maxYards > 0.0f && d > maxYards)
                            continue;
                        Insert(best, {index, d}, k);
                    }
                }
        }
        return best;
    }

    // Up to k hubs on the map nearest to the point, nearest first; within maxYards when given
    std::vector<Nearby> NearestHubs(std::uint32_t mapId, float x, float y, float z, std::size_t k,
                                    float maxYards = 0.0f) const
    {
        std::vector<Nearby> best;
        auto const onMap = _hubsByMap.find(mapId);
        if (!k || onMap == _hubsByMap.end() || !Detail::Finite(x, y, z))
            return best;
        for (std::uint32_t const index : onMap->second)
        {
            Hub const& hub = hubs[index];
            float const d = Detail::Distance(x, y, z, hub.x, hub.y, hub.z);
            if (maxYards > 0.0f && d > maxYards)
                continue;
            Insert(best, {index, d}, k);
        }
        return best;
    }

    std::optional<Nearby> NearestHub(std::uint32_t mapId, float x, float y, float z, float maxYards = 0.0f) const
    {
        std::vector<Nearby> const one = NearestHubs(mapId, x, y, z, 1, maxYards);
        return one.empty() ? std::nullopt : std::optional<Nearby>(one.front());
    }

    std::vector<std::uint32_t> HubsInZone(std::uint32_t zoneId) const
    {
        auto const it = _hubsByZone.find(zoneId);
        return it == _hubsByZone.end() ? std::vector<std::uint32_t>{} : it->second;
    }

    std::vector<std::uint32_t> const& ServicesInHub(std::uint32_t hub) const { return hubs[hub].members; }

    // Nearest-rank percentiles of the hubs' radii and the widest hubs (ties by index)
    HubRadii RadiusStats() const
    {
        HubRadii r;
        r.count = static_cast<std::uint32_t>(hubs.size());
        if (hubs.empty())
            return r;
        std::vector<std::uint32_t> order(hubs.size());
        for (std::uint32_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [this](std::uint32_t a, std::uint32_t b)
                  { return hubs[a].radius > hubs[b].radius || (hubs[a].radius == hubs[b].radius && a < b); });
        // order is widest first: the share-th percentile is ceil(share x n) from the bottom
        auto const rank = [this, &order](double share)
        {
            std::size_t const n = order.size();
            std::size_t const fromBottom = static_cast<std::size_t>(std::ceil(share * static_cast<double>(n)));
            return hubs[order[n - std::max<std::size_t>(fromBottom, 1)]].radius;
        };
        r.p50 = rank(0.5);
        r.p95 = rank(0.95);
        r.max = hubs[order.front()].radius;
        for (Hub const& hub : hubs)
            r.wide += hub.radius > HUB_WIDE_YARDS ? 1 : 0;
        r.widest.assign(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(std::min(HUB_WIDEST, order.size())));
        return r;
    }

    std::uint64_t EstimateBytes() const
    {
        std::uint64_t bytes =
            sizeof(*this) + Detail::VectorBytes(zones) + Detail::VectorBytes(services) + Detail::VectorBytes(hubs);
        for (Hub const& hub : hubs)
            bytes += Detail::VectorBytes(hub.members);
        for (auto const& [key, cell] : _cells)
            bytes += NODE_BYTES + Detail::VectorBytes(cell);
        bytes += _bounds.size() * NODE_BYTES;
        for (auto const& [map, list] : _hubsByMap)
            bytes += NODE_BYTES + Detail::VectorBytes(list);
        for (auto const& [zone, list] : _hubsByZone)
            bytes += NODE_BYTES + Detail::VectorBytes(list);
        return bytes;
    }

private:
    struct Bounds
    {
        std::int32_t minX{0};
        std::int32_t maxX{0};
        std::int32_t minY{0};
        std::int32_t maxY{0};
    };

    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> _cells;  // (kind, map, cell) -> services
    std::unordered_map<std::uint32_t, Bounds> _bounds;                     // (kind, map) -> occupied cell range
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> _hubsByMap;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> _hubsByZone;

    static std::uint32_t GridTag(ServiceKind kind, std::uint32_t mapId)
    {
        return (static_cast<std::uint32_t>(kind) << 24) | (mapId & 0xFFFFFFu);
    }

    // Keeps best sorted by distance (ties by index, so results are deterministic) and at most k long
    static void Insert(std::vector<Nearby>& best, Nearby candidate, std::size_t k)
    {
        auto const less = [](Nearby const& a, Nearby const& b)
        { return a.distance < b.distance || (a.distance == b.distance && a.index < b.index); };
        if (best.size() >= k && !less(candidate, best.back()))
            return;
        best.insert(std::upper_bound(best.begin(), best.end(), candidate, less), candidate);
        if (best.size() > k)
            best.pop_back();
    }

    void BuildZones(std::vector<ZoneRecord> input)
    {
        zones = std::move(input);
        std::sort(zones.begin(), zones.end(),
                  [](ZoneRecord const& a, ZoneRecord const& b) { return a.zoneId < b.zoneId; });
        zones.erase(std::unique(zones.begin(), zones.end(),
                                [](ZoneRecord const& a, ZoneRecord const& b) { return a.zoneId == b.zoneId; }),
                    zones.end());
        zones.shrink_to_fit();
    }

    void BuildServices(std::vector<SpawnRecord> spawns)
    {
        std::sort(spawns.begin(), spawns.end(), Detail::SpawnLess);
        stats.spawns = static_cast<std::uint32_t>(spawns.size());
        services.reserve(spawns.size());
        for (SpawnRecord const& spawn : spawns)
        {
            std::vector<ServiceKind> const kinds = Detail::KindsOf(spawn.npcFlags, spawn.bagSlots, spawn.mailbox);
            if (kinds.empty())
                continue;
            std::uint32_t const index = static_cast<std::uint32_t>(services.size());
            services.push_back({spawn.mapId, spawn.x, spawn.y, spawn.z, spawn.entry, spawn.npcFlags,
                                spawn.factionTemplate, spawn.zoneId, spawn.areaId, NO_HUB, spawn.bagSlots,
                                spawn.mailbox});
            std::int32_t const cx = Detail::Cell(spawn.x, SERVICE_CELL_YARDS);
            std::int32_t const cy = Detail::Cell(spawn.y, SERVICE_CELL_YARDS);
            for (ServiceKind const kind : kinds)
            {
                ++stats.perKind[static_cast<std::size_t>(kind)];
                std::uint32_t const tag = GridTag(kind, spawn.mapId);
                _cells[Detail::CellKey(tag, cx, cy)].push_back(index);
                auto const [it, added] = _bounds.try_emplace(tag, Bounds{cx, cx, cy, cy});
                if (!added)
                {
                    it->second.minX = std::min(it->second.minX, cx);
                    it->second.maxX = std::max(it->second.maxX, cx);
                    it->second.minY = std::min(it->second.minY, cy);
                    it->second.maxY = std::max(it->second.maxY, cy);
                }
            }
        }
        services.shrink_to_fit();
    }

    // Occupied 100-yard cells per map joined with their 8 neighbours; a group of at least HUB_MIN_SPAWNS services is
    // a hub. Services are in a canonical order: the result does not depend on the input order
    void BuildHubs()
    {
        std::vector<std::vector<std::uint32_t>> groups =
            Detail::ClusterByCells(static_cast<std::uint32_t>(services.size()), HUB_CELL_YARDS, [this](std::uint32_t i)
                                   { return std::make_tuple(services[i].mapId, services[i].x, services[i].y); });
        for (std::vector<std::uint32_t>& members : groups)
        {
            if (members.size() < HUB_MIN_SPAWNS)
                continue;
            Hub hub;
            hub.mapId = services[members.front()].mapId;
            double sx = 0.0;
            double sy = 0.0;
            double sz = 0.0;
            std::map<std::uint32_t, std::uint32_t> zoneVotes;
            std::map<std::uint32_t, std::uint32_t> areaVotes;
            for (std::uint32_t const index : members)
            {
                Service const& s = services[index];
                sx += s.x;
                sy += s.y;
                sz += s.z;
                ++zoneVotes[s.zoneId];
                ++areaVotes[s.areaId];
                for (ServiceKind const kind : Detail::KindsOf(s.npcFlags, s.bagSlots, s.mailbox))
                {
                    std::uint16_t& count = hub.counts[static_cast<std::size_t>(kind)];
                    count = static_cast<std::uint16_t>(std::min<std::uint32_t>(count + 1u, 0xFFFFu));
                }
            }
            double const n = static_cast<double>(members.size());
            hub.x = static_cast<float>(sx / n);
            hub.y = static_cast<float>(sy / n);
            hub.z = static_cast<float>(sz / n);
            auto const top = [](std::map<std::uint32_t, std::uint32_t> const& votes)
            {
                return std::max_element(votes.begin(), votes.end(),
                                        [](auto const& a, auto const& b) { return a.second < b.second; })
                    ->first;
            };
            hub.zoneId = top(zoneVotes);
            hub.areaId = top(areaVotes);
            std::uint32_t const hubIndex = static_cast<std::uint32_t>(hubs.size());
            for (std::uint32_t const index : members)
            {
                Service& s = services[index];
                s.hub = hubIndex;
                hub.radius = std::max(hub.radius, Detail::Distance(hub.x, hub.y, hub.z, s.x, s.y, s.z));
            }
            hub.members = std::move(members);
            hub.members.shrink_to_fit();
            _hubsByMap[hub.mapId].push_back(hubIndex);
            _hubsByZone[hub.zoneId].push_back(hubIndex);
            hubs.push_back(std::move(hub));
        }
        hubs.shrink_to_fit();
    }
};

// One immutable value, swapped whole: the writer Publish()es a new one, readers copy the shared_ptr (Current()) and
// keep theirs however often it changes meanwhile (StuckBook's pattern). Any thread
template <typename T>
class SharedSlot
{
public:
    void Publish(std::shared_ptr<T const> value)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _current.swap(value);
    }  // the old value dies outside the lock when the last reader drops it

    std::shared_ptr<T const> Current() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _current;
    }

private:
    mutable std::mutex _mutex;
    std::shared_ptr<T const> _current;
};

struct CrowdCounts
{
    std::uint32_t bots{0};
    std::uint32_t players{0};
};

inline std::uint64_t AreaKey(std::uint32_t mapId, std::uint32_t areaId)
{
    return (static_cast<std::uint64_t>(mapId) << 32) | areaId;
}

// Bots and real players per area and zone, as one world-thread pass counted them
struct CrowdingSnapshot
{
    std::unordered_map<std::uint64_t, CrowdCounts> byArea;  // AreaKey(map, area)
    std::unordered_map<std::uint32_t, CrowdCounts> byZone;
    CrowdCounts total;
    std::uint32_t takenMs{0};
    std::uint64_t sequence{0};         // the refresh's number, from 1
    std::uint32_t positionLookups{0};  // players whose zone and area were pending and looked up in the pass

    void Add(std::uint32_t mapId, std::uint32_t zoneId, std::uint32_t areaId, bool bot)
    {
        CrowdCounts& area = byArea[AreaKey(mapId, areaId)];
        CrowdCounts& zone = byZone[zoneId];
        std::uint32_t CrowdCounts::* const field = bot ? &CrowdCounts::bots : &CrowdCounts::players;
        ++(area.*field);
        ++(zone.*field);
        ++(total.*field);
    }
};

using CrowdingBook = SharedSlot<CrowdingSnapshot>;
using IndexBook = SharedSlot<PlannerIndexes>;
}  // namespace Planner

#endif
