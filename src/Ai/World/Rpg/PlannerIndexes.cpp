/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The server side of the planner indexes (travel plan WT12b): the world-thread gather, the worker build, the
// published indexes, the crowding refresh and the GM queries

#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

#include "Chat.h"
#include "DBCStores.h"
#include "GameEventMgr.h"
#include "GameObjectData.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "TravelGraph.h"
#include "TravelMgr.h"
#include "World.h"

namespace Planner
{
static_assert(NpcFlag::QUESTGIVER == UNIT_NPC_FLAG_QUESTGIVER && NpcFlag::TRAINER == UNIT_NPC_FLAG_TRAINER &&
                  NpcFlag::TRAINER_CLASS == UNIT_NPC_FLAG_TRAINER_CLASS &&
                  NpcFlag::TRAINER_PROFESSION == UNIT_NPC_FLAG_TRAINER_PROFESSION &&
                  NpcFlag::VENDOR == UNIT_NPC_FLAG_VENDOR && NpcFlag::REPAIR == UNIT_NPC_FLAG_REPAIR &&
                  NpcFlag::FLIGHTMASTER == UNIT_NPC_FLAG_FLIGHTMASTER &&
                  NpcFlag::INNKEEPER == UNIT_NPC_FLAG_INNKEEPER && NpcFlag::BANKER == UNIT_NPC_FLAG_BANKER &&
                  NpcFlag::AUCTIONEER == UNIT_NPC_FLAG_AUCTIONEER,
              "the planner's npcflag bits are the core's");

namespace
{
enum class IndexState : uint8
{
    NotStarted,
    Building,
    Ready,
    Failed
};

constexpr std::size_t QUERY_RESULTS = 3;
constexpr std::size_t MORE_HUBS = 3;
constexpr std::size_t BUSIEST_AREAS = 5;
constexpr std::size_t POSITION_ARGS = 4;
constexpr double BYTES_PER_MB = 1024.0 * 1024.0;

std::atomic<IndexState> indexState{IndexState::NotStarted};
IndexBook indexBook;
CrowdingBook crowdingBook;
uint32 crowdingTimer = 0;     // world thread only
uint64 crowdingSequence = 0;  // world thread only

uint32 MsSince(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

// Positive game events' spawns exist only while the event runs (as TravelMgr::PrepareDestinationCache)
template <typename Guids>
std::unordered_set<ObjectGuid::LowType> EventSpawns(Guids const& guids)
{
    std::unordered_set<ObjectGuid::LowType> spawns;
    for (std::size_t i = sGameEventMgr->GetEventMap().size(); i < guids.size(); ++i)
        spawns.insert(guids[i].begin(), guids[i].end());
    return spawns;
}

// The largest bag (container class, bag subclass) on the vendor's list, 0 = none
uint8 BagSlotsSold(uint32 entry)
{
    VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry);
    if (!items)
        return 0;
    uint32 slots = 0;
    for (VendorItem const* item : items->m_items)
    {
        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
        if (proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER)
            slots = std::max(slots, proto->ContainerSlots);
    }
    return static_cast<uint8>(std::min<uint32>(slots, 0xFF));
}

ZoneSide SideOf(AreaTableEntry const* area)
{
    if (area->IsSanctuary())
        return ZoneSide::Sanctuary;
    if (area->team == AREATEAM_ALLY)
        return ZoneSide::Alliance;
    if (area->team == AREATEAM_HORDE)
        return ZoneSide::Horde;
    return ZoneSide::Contested;
}

char const* SideName(ZoneSide side)
{
    switch (side)
    {
        case ZoneSide::Alliance:
            return "alliance";
        case ZoneSide::Horde:
            return "horde";
        case ZoneSide::Sanctuary:
            return "sanctuary";
        default:
            return "contested";
    }
}

// World thread only: copies what the build needs from the core's stores and TravelMgr's caches (both change at
// runtime: GM spawns, `.reload config` rebuilds TravelMgr), so the worker reads only this copy
GatherInput GatherWorld()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::GatherWorld");
    GatherInput input;
    std::unordered_set<uint32> const botMaps(sPlayerbotAIConfig.RandomBotMaps.begin(),
                                             sPlayerbotAIConfig.RandomBotMaps.end());
    std::unordered_map<uint32, uint8> bagSlots;  // per vendor entry

    std::unordered_set<ObjectGuid::LowType> const creatureEvents = EventCreatureSpawns();
    for (auto const& [guid, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!botMaps.count(data.mapid) || creatureEvents.count(guid))
            continue;
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(data.id);
        if (!creature || !(creature->npcflag & NpcFlag::SERVICES))
            continue;
        SpawnRecord spawn;
        spawn.mapId = data.mapid;
        spawn.x = data.posX;
        spawn.y = data.posY;
        spawn.z = data.posZ;
        spawn.entry = data.id;
        spawn.npcFlags = static_cast<uint32>(creature->npcflag);
        spawn.factionTemplate = creature->faction;
        if (!ResolveArea(spawn.mapId, spawn.x, spawn.y, spawn.z, spawn.zoneId, spawn.areaId))
        {
            ++input.unresolved;
            continue;
        }
        if (spawn.npcFlags & NpcFlag::VENDOR)
        {
            auto const [it, added] = bagSlots.try_emplace(spawn.entry, 0);
            if (added)
                it->second = BagSlotsSold(spawn.entry);
            spawn.bagSlots = it->second;
        }
        input.spawns.push_back(spawn);
    }

    std::unordered_set<ObjectGuid::LowType> const objectEvents = EventObjectSpawns();
    for (auto const& [guid, data] : sObjectMgr->GetAllGOData())
    {
        if (!botMaps.count(data.mapid) || objectEvents.count(guid))
            continue;
        GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!object || object->type != GAMEOBJECT_TYPE_MAILBOX)
            continue;
        SpawnRecord spawn;
        spawn.mapId = data.mapid;
        spawn.x = data.posX;
        spawn.y = data.posY;
        spawn.z = data.posZ;
        spawn.entry = data.id;
        spawn.mailbox = true;
        if (!ResolveArea(spawn.mapId, spawn.x, spawn.y, spawn.z, spawn.zoneId, spawn.areaId))
        {
            ++input.unresolved;
            continue;
        }
        input.spawns.push_back(spawn);
    }

    for (uint32 id = 0; id < sAreaTableStore.GetNumRows(); ++id)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(id);
        if (!area || area->zone || !botMaps.count(area->mapid))
            continue;
        ZoneRecord zone;
        zone.zoneId = area->ID;
        sTravelMgr.GetZoneBracket(area->ID, zone.low, zone.high);
        zone.side = SideOf(area);
        zone.mapId = area->mapid;
        input.zones.push_back(zone);
    }
    return input;
}

std::string AreaName(ChatHandler* handler, uint32 id)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(id);
    return area ? area->area_name[handler->GetSessionDbcLocale()] : "unknown";
}

std::string SpawnName(Service const& service)
{
    if (service.mailbox)
    {
        GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(service.entry);
        return object ? object->name : "mailbox";
    }
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(service.entry);
    return creature ? creature->Name : "unknown";
}

std::string KindCounts(Hub const& hub)
{
    std::string text;
    for (std::size_t i = 0; i < SERVICE_KINDS; ++i)
        if (hub.counts[i])
            text += Acore::StringFormat("{}{} {}", text.empty() ? "" : ", ", SERVICE_NAMES[i], hub.counts[i]);
    return text;
}

std::vector<std::string> Words(char const* args)
{
    std::vector<std::string> words;
    std::istringstream in(args ? args : "");
    for (std::string word; in >> word;)
        words.push_back(word);
    return words;
}

struct Where
{
    uint32 mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::string who;
    Player* player{nullptr};
};

// World thread (a command): [<player name> | <mapId> <x> <y> <z>]; none = the GM's own character
std::optional<Where> ResolveWhere(ChatHandler* handler, std::vector<std::string> const& words, std::size_t first)
{
    std::size_t const count = words.size() - std::min(first, words.size());
    Where where;
    if (count == POSITION_ARGS)
    {
        try
        {
            where.mapId = static_cast<uint32>(std::stoul(words[first]));
            where.x = std::stof(words[first + 1]);
            where.y = std::stof(words[first + 2]);
            where.z = std::stof(words[first + 3]);
        }
        catch (...)
        {
            handler->SendErrorMessage("PLANNER: bad position; give <mapId> <x> <y> <z>");
            return std::nullopt;
        }
        // Rejects NaN, inf and points off the map before any index sees them (WT12b review note a)
        if (!MapMgr::IsValidMapCoord(where.mapId, where.x, where.y, where.z))
        {
            handler->SendErrorMessage("PLANNER: bad position; ({}, {}, {}) is not on map {}", words[first + 1],
                                      words[first + 2], words[first + 3], words[first]);
            return std::nullopt;
        }
        where.who = Acore::StringFormat("({}, {:.0f}, {:.0f}, {:.0f})", where.mapId, where.x, where.y, where.z);
        return where;
    }
    Player* player = nullptr;
    if (count == 1)
    {
        std::string name = words[first];
        if (normalizePlayerName(name))
            player = ObjectAccessor::FindPlayerByName(name);
        if (!player)
        {
            handler->SendErrorMessage("PLANNER: {} is not online", words[first]);
            return std::nullopt;
        }
    }
    else if (!count && handler->GetSession())
        player = handler->GetSession()->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        handler->SendErrorMessage("PLANNER: give a character name or <mapId> <x> <y> <z>");
        return std::nullopt;
    }
    where.mapId = player->GetMapId();
    where.x = player->GetPositionX();
    where.y = player->GetPositionY();
    where.z = player->GetPositionZ();
    where.who = player->GetName();
    where.player = player;
    return where;
}

std::shared_ptr<PlannerIndexes const> ReadyIndexes(ChatHandler* handler)
{
    std::shared_ptr<PlannerIndexes const> indexes = IndexesReady() ? Indexes() : nullptr;
    if (!indexes)
        handler->SendErrorMessage("PLANNER: the indexes are not ready yet (see `playerbots planner indexes`)");
    return indexes;
}

// The server's DBC locale: for status lines, which have no session
std::string AreaName(uint32 id)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(id);
    return area ? area->area_name[sWorld->GetDefaultDbcLocale()] : "unknown";
}

std::string ZoneText(ChatHandler* handler, PlannerIndexes const& indexes, uint32 zoneId)
{
    ZoneRecord const* zone = indexes.FindZone(zoneId);
    std::string levels = zone && zone->high ? Acore::StringFormat("levels {}-{}", zone->low, zone->high) : "no bracket";
    return Acore::StringFormat("{} ({}, {}, {})", AreaName(handler, zoneId), zoneId, levels,
                               zone ? SideName(zone->side) : "unknown side");
}
}  // namespace

std::unordered_set<uint32> EventCreatureSpawns() { return EventSpawns(sGameEventMgr->GameEventCreatureGuids); }

std::unordered_set<uint32> EventObjectSpawns() { return EventSpawns(sGameEventMgr->GameEventGameobjectGuids); }

// World thread only: the point's area on its loaded map; false when the map or the area is unknown
bool ResolveArea(uint32 mapId, float x, float y, float z, uint32& zoneId, uint32& areaId)
{
    Map* map = sMapMgr->FindMap(mapId, 0);
    if (!map)
        return false;
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(map->GetAreaId(PHASEMASK_NORMAL, x, y, z));
    if (!area)
        return false;
    areaId = area->ID;
    zoneId = area->zone ? area->zone : area->ID;
    return true;
}

void StartPlannerIndexes()
{
    IndexState expected = IndexState::NotStarted;
    if (!indexState.compare_exchange_strong(expected, IndexState::Building))
        return;  // once: the data does not depend on the config, so `.reload config` does not rebuild it

    auto const gatherStart = std::chrono::steady_clock::now();
    auto input = std::make_shared<GatherInput>(GatherWorld());
    uint32 const gatherMs = MsSince(gatherStart);
    LOG_INFO("server.loading", "Planner indexes: gathered {} spawns and {} zones in {} ms ({} unresolved)",
             input->spawns.size(), input->zones.size(), gatherMs, input->unresolved);

    bool const posted = Pool().Post(
        [input, gatherMs]
        {
            auto const buildStart = std::chrono::steady_clock::now();
            try
            {
                auto indexes = std::make_shared<PlannerIndexes>(PlannerIndexes::Build(std::move(*input)));
                indexes->stats.gatherMs = gatherMs;
                indexes->stats.buildMs = MsSince(buildStart);
                IndexStats const& s = indexes->stats;
                auto const kind = [&s](ServiceKind k) { return s.perKind[static_cast<std::size_t>(k)]; };
                LOG_INFO("playerbots",
                         "Planner indexes built in {} ms on a worker: {} zones, {} hubs, {} services (vendors {}, bag "
                         "vendors {}, repairers {}, trainers {}, innkeepers {}, flight masters {}, bankers {}, "
                         "mailboxes {}, auctioneers {}, quest givers {}), about {:.1f} MB (estimate)",
                         s.buildMs, indexes->zones.size(), indexes->hubs.size(), indexes->services.size(),
                         kind(ServiceKind::Vendor), kind(ServiceKind::BagVendor), kind(ServiceKind::Repairer),
                         kind(ServiceKind::Trainer), kind(ServiceKind::Innkeeper), kind(ServiceKind::FlightMaster),
                         kind(ServiceKind::Banker), kind(ServiceKind::Mailbox), kind(ServiceKind::Auctioneer),
                         kind(ServiceKind::QuestGiver), static_cast<double>(s.estimatedBytes) / BYTES_PER_MB);
                HubRadii const radii = indexes->RadiusStats();
                LOG_INFO("playerbots",
                         "Planner hubs: radius p50 {:.0f} yd, p95 {:.0f} yd, max {:.0f} yd, {} of {} above {:.0f} yd "
                         "(`playerbots planner indexes` names the widest)",
                         radii.p50, radii.p95, radii.max, radii.wide, radii.count, HUB_WIDE_YARDS);
                indexBook.Publish(std::move(indexes));
                indexState.store(IndexState::Ready, std::memory_order_release);
            }
            catch (std::exception const& e)
            {
                indexState.store(IndexState::Failed, std::memory_order_release);
                LOG_ERROR("playerbots", "Planner indexes: the build failed: {}", e.what());
            }
        });
    if (!posted)
    {
        indexState.store(IndexState::Failed, std::memory_order_release);
        LOG_ERROR("server.loading", "Planner indexes: the planner pool is not running; planners keep today's picks");
    }
}

std::shared_ptr<PlannerIndexes const> Indexes() { return indexBook.Current(); }

bool IndexesReady() { return indexState.load(std::memory_order_acquire) == IndexState::Ready; }

std::shared_ptr<CrowdingSnapshot const> Crowding() { return crowdingBook.Current(); }

void UpdateCrowding(uint32 diff)
{
    crowdingTimer += diff;
    if (crowdingTimer < sPlayerbotAIConfig.WorldTravel.CrowdingRefreshSeconds * IN_MILLISECONDS)
        return;
    crowdingTimer = 0;
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::UpdateCrowding");
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "planner crowding");

    auto snapshot = std::make_shared<CrowdingSnapshot>();
    {
        // The players are only read here, on the world thread, while no map update runs (as CensusMgr::Sample)
        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player || !player->IsInWorld())
                continue;
            // The cached zone and area; a pending one is looked up once (the player moved since its last update)
            if (player->IsPositionDataUpdatePending())
                ++snapshot->positionLookups;
            uint32 zoneId = 0;
            uint32 areaId = 0;
            player->GetZoneAndAreaId(zoneId, areaId);
            snapshot->Add(player->GetMapId(), zoneId, areaId, GET_PLAYERBOT_AI(player) != nullptr);
        }
    }
    snapshot->takenMs = getMSTime();
    snapshot->sequence = ++crowdingSequence;
    crowdingBook.Publish(std::move(snapshot));

    if (pmo)
        pmo->finish();
}

std::vector<std::string> IndexStatus()
{
    std::vector<std::string> lines;
    IndexState const state = indexState.load(std::memory_order_acquire);
    std::shared_ptr<PlannerIndexes const> const indexes = state == IndexState::Ready ? Indexes() : nullptr;
    if (indexes)
    {
        IndexStats const& s = indexes->stats;
        lines.push_back(Acore::StringFormat(
            "PLANNER indexes: ready, {} zones, {} hubs, {} services from {} spawns ({} unresolved), gathered in {} "
            "ms, built in {} ms on a worker, about {:.1f} MB (estimate)",
            indexes->zones.size(), indexes->hubs.size(), indexes->services.size(), s.spawns, s.unresolved, s.gatherMs,
            s.buildMs, static_cast<double>(s.estimatedBytes) / BYTES_PER_MB));
        HubRadii const radii = indexes->RadiusStats();
        std::string widest;
        for (uint32 const index : radii.widest)
        {
            Hub const& hub = indexes->hubs[index];
            widest += Acore::StringFormat("{}#{} {} in {} ({:.0f} yd, {} services)", widest.empty() ? "" : "; ", index,
                                          AreaName(hub.areaId), AreaName(hub.zoneId), hub.radius, hub.members.size());
        }
        lines.push_back(Acore::StringFormat(
            "PLANNER hubs: radius p50 {:.0f} yd, p95 {:.0f} yd, max {:.0f} yd, {} of {} above {:.0f} yd; widest: {}",
            radii.p50, radii.p95, radii.max, radii.wide, radii.count, HUB_WIDE_YARDS, widest));
    }
    else
        lines.push_back(Acore::StringFormat("PLANNER indexes: {}", state == IndexState::Building ? "building"
                                                                   : state == IndexState::Failed ? "failed"
                                                                                                 : "not started"));
    std::shared_ptr<CrowdingSnapshot const> const crowd = Crowding();
    std::string graph = TravelGraph::Instance().IsReady() ? "ready" : "not ready";
    if (crowd)
        lines.push_back(Acore::StringFormat(
            "PLANNER crowding: snapshot #{}, age {} ms, {} bots, {} players in {} areas, {} zones, {} position "
            "lookups; travel graph {}",
            crowd->sequence, getMSTimeDiff(crowd->takenMs, getMSTime()), crowd->total.bots, crowd->total.players,
            crowd->byArea.size(), crowd->byZone.size(), crowd->positionLookups, graph));
    else
        lines.push_back(Acore::StringFormat("PLANNER crowding: no snapshot yet; travel graph {}", graph));
    lines.push_back(QuestIndexStatus());
    return lines;
}

bool HandleIndexesCommand(ChatHandler* handler, char const* /*args*/)
{
    for (std::string const& line : IndexStatus())
        handler->PSendSysMessage("{}", line);
    return true;
}

bool HandleHubCommand(ChatHandler* handler, char const* args)
{
    std::shared_ptr<PlannerIndexes const> const indexes = ReadyIndexes(handler);
    if (!indexes)
        return false;
    std::optional<Where> const where = ResolveWhere(handler, Words(args), 0);
    if (!where)
        return false;
    std::vector<Nearby> const nearby = indexes->NearestHubs(where->mapId, where->x, where->y, where->z, 1 + MORE_HUBS);
    if (nearby.empty())
    {
        handler->PSendSysMessage("HUB near {}: none on map {}", where->who, where->mapId);
        return true;
    }
    Hub const& hub = indexes->hubs[nearby.front().index];
    handler->PSendSysMessage(
        "HUB near {}: #{} at ({}, {:.0f}, {:.0f}, {:.0f}), {:.0f} yd, radius {:.0f} yd, zone {}, "
        "area {} ({}); {}",
        where->who, nearby.front().index, hub.mapId, hub.x, hub.y, hub.z, nearby.front().distance, hub.radius,
        ZoneText(handler, *indexes, hub.zoneId), AreaName(handler, hub.areaId), hub.areaId, KindCounts(hub));
    for (std::size_t i = 1; i < nearby.size(); ++i)
    {
        Hub const& more = indexes->hubs[nearby[i].index];
        handler->PSendSysMessage("  more: #{} {:.0f} yd, {} in {}; {}", nearby[i].index, nearby[i].distance,
                                 AreaName(handler, more.areaId), AreaName(handler, more.zoneId), KindCounts(more));
    }
    return true;
}

bool HandleServiceCommand(ChatHandler* handler, char const* args)
{
    std::vector<std::string> const words = Words(args);
    std::optional<ServiceKind> const kind = words.empty() ? std::nullopt : ParseServiceKind(words.front());
    if (!kind)
    {
        std::string kinds;
        for (char const* name : SERVICE_NAMES)
            kinds += Acore::StringFormat("{}{}", kinds.empty() ? "" : "|", name);
        handler->SendErrorMessage("Usage: playerbots planner service <{}> [<name> | <mapId> <x> <y> <z>]", kinds);
        return false;
    }
    std::shared_ptr<PlannerIndexes const> const indexes = ReadyIndexes(handler);
    if (!indexes)
        return false;
    std::optional<Where> const where = ResolveWhere(handler, words, 1);
    if (!where)
        return false;
    std::vector<Nearby> const nearby =
        indexes->Nearest(*kind, where->mapId, where->x, where->y, where->z, QUERY_RESULTS);
    handler->PSendSysMessage("SERVICE {} near {}: {} found", ServiceName(*kind), where->who, nearby.size());
    for (Nearby const& found : nearby)
    {
        Service const& s = indexes->services[found.index];
        std::string const extra = s.bagSlots ? Acore::StringFormat(", sells {}-slot bags", s.bagSlots) : "";
        std::string const hub = s.hub == NO_HUB ? "no hub" : Acore::StringFormat("hub #{}", s.hub);
        handler->PSendSysMessage("  {} {} at ({}, {:.0f}, {:.0f}, {:.0f}), {:.0f} yd, faction {}, {} in {}{}", s.entry,
                                 SpawnName(s), s.mapId, s.x, s.y, s.z, found.distance, s.factionTemplate, hub,
                                 AreaName(handler, s.areaId), extra);
    }
    return true;
}

bool HandleCrowdCommand(ChatHandler* handler, char const* args)
{
    std::shared_ptr<CrowdingSnapshot const> const crowd = Crowding();
    if (!crowd)
    {
        handler->SendErrorMessage("CROWD: no snapshot yet");
        return false;
    }
    handler->PSendSysMessage("CROWD snapshot #{}, age {} ms: {} bots, {} players in {} areas, {} zones",
                             crowd->sequence, getMSTimeDiff(crowd->takenMs, getMSTime()), crowd->total.bots,
                             crowd->total.players, crowd->byArea.size(), crowd->byZone.size());
    std::vector<std::string> const words = Words(args);
    if (!words.empty())
    {
        std::optional<Where> const where = ResolveWhere(handler, words, 0);
        if (!where || !where->player)
            return false;
        uint32 zoneId = 0;
        uint32 areaId = 0;
        where->player->GetZoneAndAreaId(zoneId, areaId);
        auto const area = crowd->byArea.find(AreaKey(where->mapId, areaId));
        auto const zone = crowd->byZone.find(zoneId);
        CrowdCounts const inArea = area == crowd->byArea.end() ? CrowdCounts{} : area->second;
        CrowdCounts const inZone = zone == crowd->byZone.end() ? CrowdCounts{} : zone->second;
        handler->PSendSysMessage("CROWD {}: area {} ({}) {} bots, {} players; zone {} ({}) {} bots, {} players",
                                 where->who, AreaName(handler, areaId), areaId, inArea.bots, inArea.players,
                                 AreaName(handler, zoneId), zoneId, inZone.bots, inZone.players);
    }
    std::vector<std::pair<uint64, CrowdCounts>> busiest(crowd->byArea.begin(), crowd->byArea.end());
    auto const people = [](CrowdCounts const& c) { return c.bots + c.players; };
    std::sort(
        busiest.begin(), busiest.end(), [&people](auto const& a, auto const& b)
        { return people(a.second) > people(b.second) || (people(a.second) == people(b.second) && a.first < b.first); });
    for (std::size_t i = 0; i < busiest.size() && i < BUSIEST_AREAS; ++i)
        handler->PSendSysMessage("  busiest: {} (map {}, area {}) {} bots, {} players",
                                 AreaName(handler, static_cast<uint32>(busiest[i].first & 0xFFFFFFFFu)),
                                 busiest[i].first >> 32, busiest[i].first & 0xFFFFFFFFu, busiest[i].second.bots,
                                 busiest[i].second.players);
    return true;
}
}  // namespace Planner
