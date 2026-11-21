/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The server side of the planner indexes (travel plan WT12b): the world-thread gather, the worker build, the
// published indexes, the crowding refresh and the GM queries

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

#include "Chat.h"
#include "CraftRules.h"
#include "DBCStores.h"
#include "FlightMap.h"
#include "GameEventMgr.h"
#include "GameObjectData.h"
#include "InnPhase.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerRecipes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ReputationMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "TravelGraph.h"
#include "TravelMgr.h"
#include "World.h"
#include "WorldTravelRules.h"

namespace Planner
{
static_assert(NpcFlag::QUESTGIVER == UNIT_NPC_FLAG_QUESTGIVER && NpcFlag::TRAINER == UNIT_NPC_FLAG_TRAINER &&
                  NpcFlag::TRAINER_CLASS == UNIT_NPC_FLAG_TRAINER_CLASS &&
                  NpcFlag::TRAINER_PROFESSION == UNIT_NPC_FLAG_TRAINER_PROFESSION &&
                  NpcFlag::VENDOR == UNIT_NPC_FLAG_VENDOR && NpcFlag::REPAIR == UNIT_NPC_FLAG_REPAIR &&
                  NpcFlag::FLIGHTMASTER == UNIT_NPC_FLAG_FLIGHTMASTER &&
                  NpcFlag::INNKEEPER == UNIT_NPC_FLAG_INNKEEPER && NpcFlag::BANKER == UNIT_NPC_FLAG_BANKER &&
                  NpcFlag::AUCTIONEER == UNIT_NPC_FLAG_AUCTIONEER &&
                  NpcFlag::STABLEMASTER == UNIT_NPC_FLAG_STABLEMASTER,
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
constexpr std::uint32_t QUERY_RESULTS_STATIONS = 20;
// A station's area is looked up this far above its spawn (see the gather)
constexpr float STATION_AREA_LIFT_YARDS = 1.0f;
constexpr std::size_t MORE_HUBS = 3;
constexpr std::size_t BUSIEST_AREAS = 5;
constexpr std::size_t POSITION_ARGS = 4;
constexpr double BYTES_PER_MB = 1024.0 * 1024.0;
constexpr std::size_t SIDELESS_LOGGED = 20;  // the hubs without a side the start-up log names; the console lists all

// LC18g: the player faction templates and the first race of each team (Human, Orc) the sides are judged against, and
// a class every one of those races may be (the base standing's class mask)
constexpr uint32 ALLIANCE_PLAYER_TEMPLATE = 1;
constexpr uint32 HORDE_PLAYER_TEMPLATE = 2;
constexpr uint32 ALLIANCE_RACE_MASK = 1u << (RACE_HUMAN - 1);
constexpr uint32 HORDE_RACE_MASK = 1u << (RACE_ORC - 1);
constexpr uint32 ANY_RACE_CLASS_MASK = 1u << (CLASS_WARRIOR - 1);
constexpr std::size_t BASE_REPUTATIONS = 4;

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

// LC18g: how a creature of the faction template stands to a new player of the team, as Unit::GetFactionReactionTo
// judges a player without standing of its own: a faction with reputation by the team's base standing
// (ReputationMgr::GetBaseReputation's rule), any other by the two templates
ReputationRank ReactionToTeam(FactionTemplateEntry const& npc, uint32 playerTemplate, uint32 raceMask)
{
    if (FactionEntry const* faction = sFactionStore.LookupEntry(npc.faction); faction && faction->CanHaveReputation())
    {
        for (std::size_t i = 0; i < BASE_REPUTATIONS; ++i)
            if (((faction->BaseRepRaceMask[i] & raceMask) ||
                 (!faction->BaseRepRaceMask[i] && faction->BaseRepClassMask[i])) &&
                ((faction->BaseRepClassMask[i] & ANY_RACE_CLASS_MASK) || !faction->BaseRepClassMask[i]))
                return ReputationMgr::ReputationToRank(faction->BaseRepValue[i]);
        return ReputationMgr::ReputationToRank(0);
    }
    return Unit::GetFactionReactionTo(&npc, sFactionTemplateStore.LookupEntry(playerTemplate));
}

}  // namespace

// Hostile is unfriendly or worse: the core's test for a service it will not give (BankVisit's)
bool ReactionsOf(uint32 factionTemplate, bool& hostileToAlliance, bool& hostileToHorde)
{
    FactionTemplateEntry const* npc = sFactionTemplateStore.LookupEntry(factionTemplate);
    if (!npc)
        return false;
    hostileToAlliance = ReactionToTeam(*npc, ALLIANCE_PLAYER_TEMPLATE, ALLIANCE_RACE_MASK) <= REP_UNFRIENDLY;
    hostileToHorde = ReactionToTeam(*npc, HORDE_PLAYER_TEMPLATE, HORDE_RACE_MASK) <= REP_UNFRIENDLY;
    return true;
}

namespace
{
// World thread only: copies what the build needs from the core's stores and TravelMgr's caches (both change at
// runtime: GM spawns, `.reload config` rebuilds TravelMgr), so the worker reads only this copy
GatherInput GatherWorld()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::GatherWorld");
    GatherInput input;
    std::unordered_set<uint32> const botMaps(sPlayerbotAIConfig.RandomBotMaps.begin(),
                                             sPlayerbotAIConfig.RandomBotMaps.end());
    std::unordered_map<uint32, uint8> bagSlots;  // per vendor entry
    struct Reactions
    {
        bool answered{false};
        bool hostileToAlliance{false};
        bool hostileToHorde{false};
    };
    std::unordered_map<uint32, Reactions> reactions;  // per faction template

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
        auto const [known, added] = reactions.try_emplace(creature->faction);
        if (added)
            known->second.answered =
                ReactionsOf(creature->faction, known->second.hostileToAlliance, known->second.hostileToHorde);
        spawn.answered = known->second.answered;
        spawn.hostileToAlliance = known->second.hostileToAlliance;
        spawn.hostileToHorde = known->second.hostileToHorde;
        spawn.orientation = data.orientation;
        if (!ResolveArea(spawn.mapId, spawn.x, spawn.y, spawn.z, spawn.zoneId, spawn.areaId))
        {
            ++input.unresolved;
            continue;
        }
        // An innkeeper no bot can use is no inn (as TravelMgr's inn list, planning WT63d): out of the normal phase,
        // hostile to every player or phased out by its area
        if (spawn.npcFlags & NpcFlag::INNKEEPER)
        {
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(spawn.factionTemplate);
            bool const hostile = !faction || (faction->hostileMask & FACTION_MASK_PLAYER) != 0;
            if (!WorldTravel::InnSpawnUsable(data.phaseMask, hostile,
                                             WorldTravel::InnPhasedOut(spawn.areaId, spawn.zoneId, data.phaseMask)))
                spawn.npcFlags &= ~NpcFlag::INNKEEPER;
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
        bool const mailbox = object && object->type == GAMEOBJECT_TYPE_MAILBOX;
        // A station: a spell focus object a recipe asks for (a forge, an anvil, a cooking fire ...)
        bool const station = object && object->type == GAMEOBJECT_TYPE_SPELL_FOCUS &&
                             CraftRules::IsStationFocus(object->spellFocus.focusId);
        if (!mailbox && !station)
            continue;
        SpawnRecord spawn;
        spawn.mapId = data.mapid;
        spawn.x = data.posX;
        spawn.y = data.posY;
        spawn.z = data.posZ;
        spawn.entry = data.id;
        spawn.mailbox = mailbox;
        if (station)
        {
            spawn.focus = object->spellFocus.focusId;
            spawn.range = static_cast<float>(object->spellFocus.dist);
        }
        // A station's base may sit in the floor (Undercity's anvils 0.37 yd below its forges): below a building's floor
        // the core's area lookup takes the land's area above (Map::GetAreaId, GROUND_HEIGHT_TOLERANCE)
        float const areaZ = station ? spawn.z + STATION_AREA_LIFT_YARDS : spawn.z;
        if (!ResolveArea(spawn.mapId, spawn.x, spawn.y, areaZ, spawn.zoneId, spawn.areaId))
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
    if (service.mailbox || service.focus)
    {
        GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(service.entry);
        return object ? object->name : service.focus ? "station" : "mailbox";
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

// A hub with a service NPC the game gives no side (LC18g's miss)
bool Sideless(Hub const& hub)
{
    return hub.side == PlaceSide::None && hub.votes[static_cast<std::size_t>(PlaceSide::None)] > 0;
}

// LC18g: the sides at the build, on the worker (ids only; `playerbots bridge sides` names them)
void LogPlaceSides(PlannerIndexes const& indexes)
{
    std::array<uint32, PLACE_SIDES> hubs{};
    std::array<uint32, PLACE_SIDES> services{};
    uint32 sideless = 0;
    for (Hub const& hub : indexes.hubs)
    {
        ++hubs[static_cast<std::size_t>(hub.side)];
        sideless += Sideless(hub);
    }
    for (Service const& service : indexes.services)
        ++services[static_cast<std::size_t>(service.side)];
    LOG_INFO("playerbots",
             "Place sides: {} hubs: {} alliance, {} horde, {} neutral, {} none ({} with a service NPC of no side); {} "
             "services: {} alliance, {} horde, {} neutral, {} none",
             indexes.hubs.size(), hubs[2], hubs[3], hubs[1], hubs[0], sideless, indexes.services.size(), services[2],
             services[3], services[1], services[0]);
    std::size_t logged = 0;
    for (std::size_t i = 0; i < indexes.hubs.size() && logged < SIDELESS_LOGGED; ++i)
        if (Hub const& hub = indexes.hubs[i]; Sideless(hub))
        {
            ++logged;
            LOG_INFO("playerbots",
                     "Place sides: hub #{} (map {}, {:.0f}, {:.0f}, zone {}, area {}) has no side: {} of {} "
                     "service NPCs without an answer",
                     i, hub.mapId, hub.x, hub.y, hub.zoneId, hub.areaId,
                     hub.votes[static_cast<std::size_t>(PlaceSide::None)],
                     hub.votes[0] + hub.votes[1] + hub.votes[2] + hub.votes[3]);
        }
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
                         "mailboxes {}, auctioneers {}, quest givers {}, stable masters {}, stations {}), about {:.1f} "
                         "MB (estimate)",
                         s.buildMs, indexes->zones.size(), indexes->hubs.size(), indexes->services.size(),
                         kind(ServiceKind::Vendor), kind(ServiceKind::BagVendor), kind(ServiceKind::Repairer),
                         kind(ServiceKind::Trainer), kind(ServiceKind::Innkeeper), kind(ServiceKind::FlightMaster),
                         kind(ServiceKind::Banker), kind(ServiceKind::Mailbox), kind(ServiceKind::Auctioneer),
                         kind(ServiceKind::QuestGiver), kind(ServiceKind::StableMaster), kind(ServiceKind::Station),
                         static_cast<double>(s.estimatedBytes) / BYTES_PER_MB);
                HubRadii const radii = indexes->RadiusStats();
                LOG_INFO("playerbots",
                         "Planner hubs: radius p50 {:.0f} yd, p95 {:.0f} yd, max {:.0f} yd, {} of {} above {:.0f} yd "
                         "(`playerbots planner indexes` names the widest)",
                         radii.p50, radii.p95, radii.max, radii.wide, radii.count, HUB_WIDE_YARDS);
                LogPlaceSides(*indexes);
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
        // The players are only read here, on the world thread, while no map update runs (as the world log writer's
        // state pass)
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
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
            snapshot->Add(player->GetMapId(), zoneId, areaId, botAI != nullptr,
                          botAI && botAI->rpgInfo.GetStatus() == RPG_DO_GATHER);
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
    for (std::string const& line : UpkeepIndexStatus())
        lines.push_back(line);
    for (std::string const& line : RecipeIndexStatus())
        lines.push_back(line);
    for (std::string const& line : FlightMapStatus())
        lines.push_back(line);
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
        std::string const extra = s.bagSlots ? Acore::StringFormat(", sells {}-slot bags", s.bagSlots)
                                  : s.focus  ? Acore::StringFormat(", focus {} range {:.0f} yd", s.focus, s.range)
                                             : "";
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

// LC18g: every town and camp with its side and the votes of its service NPCs, then every service NPC's entry with how
// the game judges it now (hostile to the Alliance, to the Horde) and whom the index offers it to
bool HandleSidesCommand(ChatHandler* handler, char const* /*args*/)
{
    std::shared_ptr<PlannerIndexes const> const indexes = ReadyIndexes(handler);
    if (!indexes)
        return false;
    struct Entry
    {
        ServiceKind kind{ServiceKind::Vendor};
        uint32 faction{0};
        PlaceSide side{PlaceSide::None};
        uint32 spawns{0};
        bool mixed{false};  // spawns of the entry with different sides
    };
    std::map<uint32, Entry> entries;
    for (Service const& service : indexes->services)
    {
        if (service.mailbox || service.focus)
            continue;
        std::vector<ServiceKind> const kinds = Detail::KindsOf(service.npcFlags, service.bagSlots, service.mailbox);
        auto const [it, added] = entries.try_emplace(service.entry);
        Entry& entry = it->second;
        if (added)
        {
            entry.kind = kinds.empty() ? ServiceKind::QuestGiver : kinds.front();
            entry.faction = service.factionTemplate;
            entry.side = service.side;
        }
        entry.mixed |= entry.side != service.side;
        ++entry.spawns;
    }
    std::array<uint32, PLACE_SIDES> hubs{};
    uint32 sideless = 0;
    for (Hub const& hub : indexes->hubs)
    {
        ++hubs[static_cast<std::size_t>(hub.side)];
        sideless += Sideless(hub);
    }
    uint32 offeredHostile = 0;
    std::vector<std::string> lines;
    lines.reserve(entries.size());
    for (auto const& [id, entry] : entries)
    {
        bool hostileToAlliance = false;
        bool hostileToHorde = false;
        bool const answered = ReactionsOf(entry.faction, hostileToAlliance, hostileToHorde);
        bool const toAlliance = Serves(entry.side, PlaceSide::Alliance);
        bool const toHorde = Serves(entry.side, PlaceSide::Horde);
        offeredHostile += (toAlliance && hostileToAlliance) || (toHorde && hostileToHorde);
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(id);
        lines.push_back(Acore::StringFormat("SERVICE {}|{}|{}|{}|{}|{}|{}|{}|{}", ServiceName(entry.kind), id,
                                            answered ? (hostileToAlliance ? 1 : 0) : -1,
                                            answered ? (hostileToHorde ? 1 : 0) : -1,
                                            toAlliance ? (toHorde ? "both" : "alliance") : (toHorde ? "horde" : "none"),
                                            entry.mixed ? "mixed" : PlaceSideWord(entry.side), entry.spawns,
                                            entry.faction, creature ? creature->Name : "unknown"));
    }
    handler->PSendSysMessage(
        "PLACESIDES {} hubs: {} alliance, {} horde, {} neutral, {} none ({} with a service NPC); {} "
        "service entries, {} offered to a side they are hostile to",
        indexes->hubs.size(), hubs[2], hubs[3], hubs[1], hubs[0], sideless, entries.size(), offeredHostile);
    for (Hub const& hub : indexes->hubs)
        handler->PSendSysMessage("PLACESIDE {}|{}|{}|{}|{}|{}|{}|{}|{:.0f}|{:.0f}", AreaName(handler, hub.areaId),
                                 AreaName(handler, hub.zoneId), PlaceSideWord(hub.side), hub.votes[2], hub.votes[3],
                                 hub.votes[1], hub.votes[0], hub.mapId, hub.x, hub.y);
    for (std::string const& line : lines)
        handler->PSendSysMessage("{}", line);
    handler->PSendSysMessage("PLACESIDES end");
    return true;
}

namespace
{
struct Capital
{
    char const* name;
    std::uint32_t zoneId;
};
// The eight capitals' zones (AreaTable)
constexpr std::array<Capital, 8> CAPITALS = {{{"Stormwind", 1519},
                                              {"Ironforge", 1537},
                                              {"Darnassus", 1657},
                                              {"Orgrimmar", 1637},
                                              {"Thunder Bluff", 1638},
                                              {"Undercity", 1497},
                                              {"Exodar", 3557},
                                              {"Silvermoon", 3487}}};
}  // namespace

bool HandleStationCommand(ChatHandler* handler, std::vector<std::string> const& words)
{
    std::shared_ptr<PlannerIndexes const> const indexes = ReadyIndexes(handler);
    if (!indexes)
    {
        handler->SendSysMessage("STATIONS: the planner's indexes are not ready");
        return false;
    }
    std::optional<std::uint32_t> only;
    if (!words.empty())
    {
        only = Acore::StringTo<std::uint32_t>(words.front());
        if (!only || words.size() > 1)
        {
            handler->SendErrorMessage("Usage: playerbots professions stations [<spell focus id>]");
            return false;
        }
    }

    std::map<std::uint32_t, std::uint32_t> perFocus;
    for (Service const& s : indexes->services)
        if (s.focus)
            ++perFocus[s.focus];
    std::uint32_t total = 0;
    for (auto const& [focus, count] : perFocus)
    {
        if (only && *only != focus)
            continue;
        total += count;
        handler->PSendSysMessage("STATION focus {} ({}): {} spawned", focus, CraftRules::FocusName(focus), count);
        if (only)
        {
            std::uint32_t shown = 0;
            for (Service const& s : indexes->services)
            {
                if (s.focus != focus || shown >= QUERY_RESULTS_STATIONS)
                    continue;
                ++shown;
                handler->PSendSysMessage("STATION   {} {} at ({}, {:.0f}, {:.0f}, {:.0f}), range {:.0f} yd, {}",
                                         s.entry, SpawnName(s), s.mapId, s.x, s.y, s.z, s.range,
                                         AreaName(handler, s.areaId));
            }
            continue;
        }
        for (Capital const& capital : CAPITALS)
        {
            Service const* nearest = nullptr;
            std::uint32_t inZone = 0;
            for (Service const& s : indexes->services)
                if (s.focus == focus && s.zoneId == capital.zoneId && ++inZone == 1)
                    nearest = &s;
            if (nearest)
                handler->PSendSysMessage("STATION   {}: {} at ({}, {:.0f}, {:.0f}, {:.0f}), range {:.0f} yd",
                                         capital.name, inZone, nearest->mapId, nearest->x, nearest->y, nearest->z,
                                         nearest->range);
        }
    }
    handler->PSendSysMessage("STATIONS: {} spawned in {} spell focus kinds", total, only ? 1u : perFocus.size());
    return true;
}

}  // namespace Planner
