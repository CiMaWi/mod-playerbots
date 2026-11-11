/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The server side of the quest index (travel plan WT13a): the world-thread gather, the worker build, the published
// index and the GM query; and the population's quest statistics: the events, the load, the flush and the view.
// The questing circuit's job wrapper, its real-place harness and `planner circuit` (WT13b; the planner: QuestCircuit.h)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <variant>

#include "Chat.h"
#include "DBCStores.h"
#include "GameObjectData.h"
#include "GoalModel.h"
#include "GoalModelHarness.h"
#include "GoalModelLog.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "QuestCircuit.h"
#include "QuestCircuitHarness.h"
#include "QuestCircuitLog.h"
#include "QuestDef.h"
#include "QuestIndex.h"
#include "QuestLevelRules.h"
#include "QuestStats.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "World.h"
#include "WorldLog.h"

namespace Planner
{
static_assert(QuestType::ELITE == QUEST_TYPE_ELITE && QuestType::PVP == QUEST_TYPE_PVP &&
                  QuestType::RAID == QUEST_TYPE_RAID && QuestType::DUNGEON == QUEST_TYPE_DUNGEON &&
                  QuestType::HEROIC == QUEST_TYPE_HEROIC && QuestType::RAID_10 == QUEST_TYPE_RAID_10 &&
                  QuestType::RAID_25 == QUEST_TYPE_RAID_25,
              "the planner's quest types are the core's");
static_assert(POI_FIRST_ITEM == QUEST_OBJECTIVES_COUNT, "item POIs follow the creature or object slots");
static_assert(Circuit::MAX_LEVELS_ABOVE == QUEST_MAX_LEVELS_ABOVE_BOT, "the circuit's level limit is today's");
static_assert(Circuit::SLOT_COMPLETE == static_cast<uint32>(QUEST_STATE_COMPLETE),
              "the circuit reads the log slot's complete flag");

namespace
{
enum class QuestIndexState : uint8
{
    NotStarted,
    Building,
    Ready,
    Failed
};

constexpr uint32 AREATRIGGER_ID_LIMIT = 0xFFFF;  // the area trigger ids looked up for exploration quests
constexpr std::size_t QUERY_PLACES = 4;          // givers and enders listed by `planner quest`
constexpr double BYTES_PER_MB = 1024.0 * 1024.0;

std::atomic<QuestIndexState> questState{QuestIndexState::NotStarted};
QuestBook questBook;

// The quest statistics (QuestStats.h): the book takes events from map threads; the rest is the world thread's
enum class StatsLoad : uint8
{
    NotLoaded,
    Loaded,
    NoTable
};

constexpr uint32 STATS_FLUSH_MS = 300 * IN_MILLISECONDS;  // the world thread writes the changed cells this often

std::atomic<bool> statsOn{false};  // set after the load; events before it are not counted
QuestStatsBook statsBook;
QuestStatsSlot statsSlot;
StatsLoad statsLoad = StatsLoad::NotLoaded;
uint32 statsLoadedRows = 0;
uint32 statsBadRows = 0;  // rows whose minutes column did not parse: counts kept, samples dropped
uint32 statsFlushTimer = 0;
uint32 statsFlushes = 0;
uint32 statsLastRows = 0;
uint64 statsRowsWritten = 0;
std::optional<std::chrono::steady_clock::time_point> statsLastFlush;

uint32 MsSince(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

uint64 StatsNowMs()
{
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Map threads: the event with its scope check (persistent random bots, as the planner hook), timed as PerfMonitor
// row `planner quest event`
template <typename Event>
void TimedEvent(Player* bot, Event const& event)
{
    auto const started = std::chrono::steady_clock::now();
    if (PlayerbotFactory::KeepsPossessions(bot))
        event(bot->GetGUID().GetCounter(), static_cast<uint32>(bot->GetLevel()));
    auto const us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "planner quest event"))
        row->Record(static_cast<uint64>(std::max<long long>(us, 0)));
}

bool StatsOn(Player* bot) { return bot && statsOn.load(std::memory_order_acquire); }

// World thread, at startup before any bot logs in (one query, as BotTraitMgr): the stored totals
void LoadQuestStats()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::LoadQuestStats");
    // The table comes with an update file that a server with Playerbots.Updates.EnableDatabases = 0 may lack: then
    // the statistics live in memory only
    bool const table = PlayerbotsDatabase.Query(
                           "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = "
                           "DATABASE() AND TABLE_NAME = 'playerbots_quest_stats'") != nullptr;
    statsLoad = table ? StatsLoad::Loaded : StatsLoad::NoTable;
    if (!table)
        LOG_WARN("playerbots",
                 "Quest stats: no table playerbots_quest_stats (update 2026_11_09_01 not applied): the "
                 "statistics are kept in memory only");
    else if (QueryResult result = PlayerbotsDatabase.Query(
                 "SELECT quest_id, band, accepted, completed, abandoned, deaths, minutes FROM playerbots_quest_stats"))
    {
        do
        {
            Field* fields = result->Fetch();
            QuestStatCell cell;
            cell.accepted = fields[2].Get<uint32>();
            cell.completed = fields[3].Get<uint32>();
            cell.abandoned = fields[4].Get<uint32>();
            cell.deaths = fields[5].Get<uint32>();
            if (!DecodeMinutes(fields[6].Get<std::string>(), cell))
                ++statsBadRows;
            statsBook.Restore(fields[0].Get<uint32>(), fields[1].Get<uint8>(), cell);
            ++statsLoadedRows;
        } while (result->NextRow());
    }
    statsSlot.Publish(statsBook.View());
    statsOn.store(true, std::memory_order_release);
    LOG_INFO("playerbots", "Quest stats: loaded {} rows{}{}", statsLoadedRows,
             statsBadRows ? Acore::StringFormat(" ({} with a bad minutes column)", statsBadRows) : "",
             table ? "" : " (no table: memory only)");
}

std::string MinutesText(float minutes) { return minutes < 0.0f ? "-" : Acore::StringFormat("{:.1f}", minutes); }

bool HandleQuestStats(ChatHandler* handler)
{
    QuestBookCounts const c = statsBook.Counts();
    std::shared_ptr<QuestStatsView const> const view = QuestStats();
    std::string const load = statsLoad == StatsLoad::Loaded    ? Acore::StringFormat("loaded {} rows", statsLoadedRows)
                             : statsLoad == StatsLoad::NoTable ? std::string("no table, memory only")
                                                               : std::string("not loaded");
    std::string const last =
        statsLastFlush
            ? Acore::StringFormat("last {} s ago ({} rows)", MsSince(*statsLastFlush) / IN_MILLISECONDS, statsLastRows)
            : std::string("none yet");
    handler->PSendSysMessage("QUEST stats: {}; {} rows in memory, {} dirty, {} open quests; view {} rows", load,
                             c.cells, c.dirty, c.open, view ? view->rows.size() : 0);
    handler->PSendSysMessage(
        "QUEST stats events: {} accepts, {} turn-ins ({} with minutes, {} without), {} abandons, {} drops, {} "
        "deaths; {} open quests pruned, {} accepts without a start (full)",
        c.accepts, c.turnIns, c.samples, c.unsampled, c.abandons, c.drops, c.deaths, c.pruned, c.full);
    handler->PSendSysMessage("QUEST stats flushes: {} every {} s, {}, {} rows written since startup", statsFlushes,
                             STATS_FLUSH_MS / IN_MILLISECONDS, last, statsRowsWritten);
    return true;
}

void SendQuestStats(ChatHandler* handler, uint32 questId)
{
    if (!statsOn.load(std::memory_order_acquire))
    {
        handler->PSendSysMessage("  statistics: not loaded");
        return;
    }
    std::vector<std::pair<uint8, QuestStatCell>> const cells = statsBook.CellsOf(questId);
    if (cells.empty())
        handler->PSendSysMessage("  statistics: none recorded yet");
    for (auto const& [band, cell] : cells)
    {
        QuestStatSummary const s = Summarize(cell);
        handler->PSendSysMessage(
            "  stats band {} (levels {}-{}): accepted {}, completed {}, median {} min ({} samples), deaths per "
            "completion {}, abandoned {}, deaths {}",
            band, band * LEVEL_BAND_LEVELS, band * LEVEL_BAND_LEVELS + LEVEL_BAND_LEVELS - 1, s.accepted, s.completed,
            MinutesText(s.medianMinutes), s.samples,
            s.deathsPerCompletion < 0.0f ? std::string("-") : Acore::StringFormat("{:.2f}", s.deathsPerCompletion),
            s.abandoned, s.deaths);
    }
}

uint16 FlagsOf(Quest const* quest)
{
    uint32 const type = quest->GetType();
    uint16 flags = 0;
    if (type == QuestType::ELITE)
        flags |= QuestFlag::ELITE;
    if (quest->GetSuggestedPlayers() > 1)
        flags |= QuestFlag::GROUP;
    if (type == QuestType::DUNGEON || type == QuestType::HEROIC)
        flags |= QuestFlag::DUNGEON;
    if (type == QuestType::RAID || type == QuestType::RAID_10 || type == QuestType::RAID_25)
        flags |= QuestFlag::RAID;
    if (type == QuestType::PVP || quest->HasFlag(QUEST_FLAGS_FLAGS_PVP))
        flags |= QuestFlag::PVP;
    if (quest->IsDaily())
        flags |= QuestFlag::DAILY;
    if (quest->IsWeekly())
        flags |= QuestFlag::WEEKLY;
    if (quest->IsRepeatable())
        flags |= QuestFlag::REPEATABLE;
    if (quest->IsSeasonal())
        flags |= QuestFlag::SEASONAL;
    return flags;
}

QuestRecord RecordOf(Quest const* quest)
{
    QuestRecord record;
    record.id = quest->GetQuestId();
    record.level = quest->GetQuestLevel();
    record.minLevel = quest->GetMinLevel();
    record.maxLevel = quest->GetMaxLevel();
    record.raceMask = quest->GetAllowableRaces();
    record.classMask = quest->GetRequiredClasses();
    record.zoneOrSort = quest->GetZoneOrSort();
    record.type = quest->GetType();
    record.suggestedPlayers = quest->GetSuggestedPlayers();
    record.flags = FlagsOf(quest);
    // A scaling quest (level -1) is valued at its minimum level
    int32 const level = record.level > 0 ? record.level : static_cast<int32>(record.minLevel);
    uint8 const at = static_cast<uint8>(std::clamp<int32>(level, 1, std::numeric_limits<uint8>::max()));
    record.xpBase = quest->XPValue(at);
    record.copperReward = quest->GetRewOrReqMoney(at);
    record.prev = quest->GetPrevQuestId();
    record.next = quest->GetNextQuestId();
    record.nextInChain = quest->GetNextQuestInChain();
    record.exclusiveGroup = quest->GetExclusiveGroup();
    record.breadcrumbFor = quest->GetBreadcrumbForQuestId();
    for (int32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;
        bool const object = npcOrGo < 0;
        record.objectives.push_back({object ? ObjectiveKind::Object : ObjectiveKind::Kill,
                                     static_cast<uint32>(std::abs(npcOrGo)),
                                     quest->RequiredNpcOrGoCount[i],
                                     i,
                                     {}});
    }
    for (int32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i])
            record.objectives.push_back(
                {ObjectiveKind::Item, quest->RequiredItemId[i], quest->RequiredItemCount[i], POI_FIRST_ITEM + i, {}});
    return record;
}

// World thread only: copies the quest templates, relations, POIs, area triggers and the spawns of givers, enders and
// kill or object targets (GM commands change these stores at runtime), so the worker reads only this copy
QuestGather GatherQuests()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::GatherQuests");
    QuestGather input;
    std::unordered_set<uint64> targets;  // EntryKey of kill and object targets
    for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
    {
        if (!quest)
            continue;
        QuestRecord record = RecordOf(quest);
        for (QuestObjective const& objective : record.objectives)
            if (objective.kind == ObjectiveKind::Kill || objective.kind == ObjectiveKind::Object)
                targets.insert(Detail::EntryKey(objective.target, objective.kind == ObjectiveKind::Object));
        if (QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(id))
            for (QuestPOI const& poi : *pois)
            {
                PoiInput entry{id, poi.ObjectiveIndex, poi.MapId, {}};
                entry.points.reserve(poi.points.size());
                for (QuestPOIPoint const& point : poi.points)
                    entry.points.emplace_back(static_cast<float>(point.x), static_cast<float>(point.y));
                input.pois.push_back(std::move(entry));
            }
        input.quests.push_back(std::move(record));
    }

    std::unordered_set<uint64> related;  // EntryKey of givers and enders
    auto const relate = [&input, &related](QuestRelations const* relations, bool isObject, bool ender)
    {
        if (!relations)
            return;
        for (auto const& [entry, questId] : *relations)
        {
            input.relations.push_back({questId, entry, isObject, ender});
            related.insert(Detail::EntryKey(entry, isObject));
        }
    };
    relate(sObjectMgr->GetCreatureQuestRelationMap(), false, false);
    relate(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), false, true);
    relate(sObjectMgr->GetGOQuestRelationMap(), true, false);
    relate(sObjectMgr->GetGOQuestInvolvedRelationMap(), true, true);

    auto const spawn =
        [&input, &related, &targets](bool isObject, uint32 entry, uint32 mapId, float x, float y, float z)
    {
        uint64 const key = Detail::EntryKey(entry, isObject);
        if (related.count(key))
        {
            QuestPlace place{mapId, x, y, z, entry, isObject};
            if (!ResolveArea(mapId, x, y, z, place.zoneId, place.areaId))
                ++input.unresolved;
            input.places.push_back(place);
        }
        if (targets.count(key))
            input.targets.push_back({entry, isObject, mapId, x, y, z});
    };
    // Game event spawns exist only while their event runs, as for the services
    std::unordered_set<uint32> const creatureEvents = EventCreatureSpawns();
    for (auto const& [guid, data] : sObjectMgr->GetAllCreatureData())
        if (!creatureEvents.count(guid))
            spawn(false, data.id, data.mapid, data.posX, data.posY, data.posZ);
    std::unordered_set<uint32> const objectEvents = EventObjectSpawns();
    for (auto const& [guid, data] : sObjectMgr->GetAllGOData())
        if (!objectEvents.count(guid))
            spawn(true, data.id, data.mapid, data.posX, data.posY, data.posZ);

    for (uint32 trigger = 1; trigger < AREATRIGGER_ID_LIMIT; ++trigger)
    {
        uint32 const questId = sObjectMgr->GetQuestForAreaTrigger(trigger);
        AreaTrigger const* at = questId ? sObjectMgr->GetAreaTrigger(trigger) : nullptr;
        if (!at)
            continue;
        float const radius = at->radius > 0.0f ? at->radius : 0.5f * std::hypot(at->length, at->width);
        input.triggers.push_back({questId, trigger, at->map, at->x, at->y, at->z, radius});
    }
    return input;
}

std::string AreaName(ChatHandler* handler, uint32 id)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(id);
    return area ? area->area_name[handler->GetSessionDbcLocale()] : "unknown";
}

std::string EntryName(uint32 entry, bool isObject)
{
    if (isObject)
    {
        GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(entry);
        return object ? object->name : "unknown object";
    }
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
    return creature ? creature->Name : "unknown creature";
}

std::string TargetName(QuestObjective const& objective)
{
    switch (objective.kind)
    {
        case ObjectiveKind::Kill:
            return EntryName(objective.target, false);
        case ObjectiveKind::Object:
            return EntryName(objective.target, true);
        case ObjectiveKind::Item:
        {
            ItemTemplate const* item = sObjectMgr->GetItemTemplate(objective.target);
            return item ? item->Name1 : "unknown item";
        }
        case ObjectiveKind::Explore:
            return "area trigger";
        default:
            return "turn-in";
    }
}

std::string FlagText(QuestRecord const& quest)
{
    static constexpr std::array<std::pair<uint16, char const*>, 9> NAMES = {{{QuestFlag::ELITE, "elite"},
                                                                             {QuestFlag::GROUP, "group"},
                                                                             {QuestFlag::DUNGEON, "dungeon"},
                                                                             {QuestFlag::RAID, "raid"},
                                                                             {QuestFlag::PVP, "pvp"},
                                                                             {QuestFlag::DAILY, "daily"},
                                                                             {QuestFlag::WEEKLY, "weekly"},
                                                                             {QuestFlag::REPEATABLE, "repeatable"},
                                                                             {QuestFlag::SEASONAL, "seasonal"}}};
    std::string text;
    for (auto const& [flag, name] : NAMES)
        if (quest.Has(flag))
            text += Acore::StringFormat("{}{}", text.empty() ? "" : ", ", name);
    return text.empty() ? "none" : text;
}

std::string ListText(std::vector<uint32> const& ids)
{
    std::string text;
    for (uint32 const id : ids)
        text += Acore::StringFormat("{}{}", text.empty() ? "" : ", ", id);
    return text.empty() ? "none" : text;
}

// The camp and the services hub it joins (QuestIndex.h, CampHub)
std::string CampText(QuestIndex const& quests, PlannerIndexes const* indexes, uint32 campIndex)
{
    if (campIndex >= quests.camps.size())
        return "no camp";
    QuestCamp const& camp = quests.camps[campIndex];
    std::string text = Acore::StringFormat("camp #{} ({} places, {} quests, {:.0f} yd)", campIndex, camp.places.size(),
                                           camp.quests.size(), camp.radius);
    if (!indexes)
        return text + ", hubs not ready";
    if (std::optional<Nearby> const hub = CampHub(camp, *indexes))
        return text + Acore::StringFormat(", hub #{} ({:.0f} yd from its centre)", hub->index, hub->distance);
    std::optional<Nearby> const nearest = indexes->NearestHub(camp.mapId, camp.x, camp.y, camp.z);
    return text + (nearest ? Acore::StringFormat(", in no hub's core (nearest hub #{} {:.0f} yd)", nearest->index,
                                                 nearest->distance)
                           : std::string(", no hub on the map"));
}

void SendPlaces(ChatHandler* handler, QuestIndex const& quests, PlannerIndexes const* indexes,
                std::vector<uint32> const& list, char const* role)
{
    if (list.empty())
        handler->PSendSysMessage("  {}: none", role);
    for (std::size_t i = 0; i < list.size() && i < QUERY_PLACES; ++i)
    {
        QuestPlace const& place = quests.places[list[i]];
        handler->PSendSysMessage("  {} {} {} at ({}, {:.0f}, {:.0f}, {:.0f}), {} in {}; {}", role, place.entry,
                                 EntryName(place.entry, place.isObject), place.mapId, place.x, place.y, place.z,
                                 AreaName(handler, place.areaId), AreaName(handler, place.zoneId),
                                 CampText(quests, indexes, place.camp));
    }
    if (list.size() > QUERY_PLACES)
        handler->PSendSysMessage("  ... and {} more {} spawns", list.size() - QUERY_PLACES, role);
}

// The circuit harness's real places (WT13b): at fixed map coordinates among the quest givers, checked against the
// quest index when the harness runs (the givers within the hub's reach, the nearest camp)
struct CircuitPlace
{
    char const* name;
    uint8 level;
    uint8 race;
    uint8 cls;
    uint8 team;  // 0 Alliance, 1 Horde
    uint32 mapId;
    float x;
    float y;
    float z;
};

// Goldshire's quests are levels 7-10: at level 12 they are grey (grey difference 4) and only deliveries are left, so
// the human is 9 (the coordinator's call after the WT13b review)
constexpr std::array<CircuitPlace, 3> CIRCUIT_PLACES{{
    {"goldshire-9-human", 9, RACE_HUMAN, CLASS_WARRIOR, 0, 0, -9465.0f, 40.0f, 57.0f},
    {"crossroads-22-orc", 22, RACE_ORC, CLASS_WARRIOR, 1, 1, -441.0f, -2596.0f, 92.0f},
    {"gromgol-35-troll", 35, RACE_TROLL, CLASS_HUNTER, 1, 0, -12370.0f, 175.0f, 4.0f},
}};
constexpr uint32 CIRCUIT_TIMING_RUNS = 50;      // plans per real scenario behind the job time's p50/p95
constexpr float CIRCUIT_CAMP_YARDS = 300.0f;    // the nearest quest camp to a place is looked for this far
constexpr std::size_t CIRCUIT_CAMPS_TRIED = 8;  // camps tried for one with quests, nearest first
constexpr uint16 CIRCUIT_BAG_SLOTS = 16;        // the synthetic bot's free bag slots

// The published data a circuit job reads, taken on the calling thread with the core's grey level difference
struct CircuitData
{
    std::shared_ptr<PlannerIndexes const> indexes;
    std::shared_ptr<QuestIndex const> quests;
    std::shared_ptr<QuestStatsView const> stats;
    std::shared_ptr<CrowdingSnapshot const> crowding;
    uint32 greyDiff{Circuit::GREY_DIFF};

    // The pointers stay valid while this data (or a copy of its shared pointers) lives
    CircuitContext Context() const
    {
        CircuitContext context;
        context.quests = quests.get();
        context.indexes = indexes.get();
        context.stats = stats.get();
        context.crowding = crowding.get();
        context.greyDiff = greyDiff;
        return context;
    }
};

CircuitData CircuitDataNow()
{
    CircuitData data;
    data.indexes = IndexesReady() ? Indexes() : nullptr;
    data.quests = QuestsReady() ? Quests() : nullptr;
    data.stats = QuestStats();
    data.crowding = Crowding();
    data.greyDiff = sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF);
    return data;
}

Snapshot CircuitBot(CircuitPlace const& place, Place const& start)
{
    Snapshot bot;
    bot.level = place.level;
    bot.race = place.race;
    bot.cls = place.cls;
    bot.team = place.team;
    bot.position = start;
    bot.runSpeed = Circuit::DEFAULT_RUN_SPEED;
    bot.bagSlots = CIRCUIT_BAG_SLOTS;
    bot.bagFree = CIRCUIT_BAG_SLOTS;
    return bot;
}

// The place's start: its own position when a quest giver stands within the hub's reach, else the nearest camp that
// gives quests. False when neither has a giver within the hub's reach
bool CircuitStart(QuestIndex const& quests, CircuitPlace const& place, Place& start, std::string& text)
{
    start = {place.mapId, place.x, place.y, place.z};
    std::size_t givers = quests.GiversNear(place.mapId, place.x, place.y, Circuit::HUB_GIVER_YARDS).size();
    std::string where = Acore::StringFormat("({:.0f}, {:.0f}) on map {}", place.x, place.y, place.mapId);
    std::string camp = Acore::StringFormat("no camp with quests within {:.0f} yd", CIRCUIT_CAMP_YARDS);
    for (Nearby const& nearby :
         quests.NearestCamps(place.mapId, place.x, place.y, place.z, CIRCUIT_CAMPS_TRIED, CIRCUIT_CAMP_YARDS))
    {
        QuestCamp const& found = quests.camps[nearby.index];
        if (found.quests.empty())
            continue;
        camp =
            Acore::StringFormat("camp {} ({} quests, {} places, radius {:.0f} yd, zone {}) at {:.0f} yd", nearby.index,
                                found.quests.size(), found.places.size(), found.radius, found.zoneId, nearby.distance);
        if (!givers)
        {
            start = {place.mapId, found.x, found.y, found.z};
            givers = quests.GiversNear(place.mapId, found.x, found.y, Circuit::HUB_GIVER_YARDS).size();
            where = Acore::StringFormat("the camp's centre ({:.0f}, {:.0f})", found.x, found.y);
        }
        break;
    }
    text = Acore::StringFormat("{} quest givers within {:.0f} yd of {}; nearest {}", givers, Circuit::HUB_GIVER_YARDS,
                               where, camp);
    return givers > 0;
}

// The synthetic bot's history: every quest grey for its level, as a bot that levelled through the world has done
// (prerequisites of the hub's chains met; the grey ones skipped either way), ascending as the index
std::vector<uint32> GreyHistory(QuestIndex const& quests, uint8 level, uint32 greyDiff)
{
    std::vector<uint32> ids;
    for (QuestRecord const& quest : quests.quests)
        if (quest.level > 0 &&
            static_cast<int32>(level) > Circuit::QuestLevel(quest, level) + static_cast<int32>(greyDiff))
            ids.push_back(quest.id);
    return ids;
}

// Every quest the givers within the hub's reach offer, sorted: rewarded, they make the hub dry
std::vector<uint32> HubQuests(QuestIndex const& quests, Place const& at)
{
    std::vector<uint32> ids;
    for (uint32 const place : quests.GiversNear(at.map, at.x, at.y, Circuit::HUB_GIVER_YARDS))
        for (uint32 const id : quests.QuestsGivenAt(place))
            ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Log quests for the full-log fixture, ascending: every objective on another map than the place's (work the bot
// cannot do from the hub), grey for its level or not, never a repeatable, elite, group, instance or exclusive quest,
// and none that a hub quest names (prerequisite, breadcrumb), so the hub offers what it offers to an empty log
std::vector<uint32> FarQuests(QuestIndex const& quests, CircuitPlace const& place, std::vector<uint32> const& hub,
                              uint32 greyDiff, bool grey, std::size_t count)
{
    std::vector<uint32> named;
    for (uint32 const id : hub)
        if (QuestRecord const* quest = quests.Find(id))
        {
            named.push_back(static_cast<uint32>(quest->prev < 0 ? -quest->prev : quest->prev));
            named.push_back(quest->breadcrumbFor);
        }
    std::vector<uint32> ids;
    for (QuestRecord const& quest : quests.quests)
    {
        if (ids.size() >= count)
            break;
        bool const isGrey =
            static_cast<int32>(place.level) > Circuit::QuestLevel(quest, place.level) + static_cast<int32>(greyDiff);
        bool const above =
            Circuit::QuestLevel(quest, place.level) > static_cast<int32>(place.level) + Circuit::MAX_LEVELS_ABOVE;
        if (quest.level <= 0 || isGrey != grey || above || quest.exclusiveGroup ||
            (quest.flags & (Circuit::REPEAT_FLAGS | Circuit::GROUP_FLAGS | Circuit::INSTANCE_FLAGS)) ||
            std::binary_search(hub.begin(), hub.end(), quest.id) ||
            std::find(named.begin(), named.end(), quest.id) != named.end())
            continue;
        bool placed = false;
        bool elsewhere = true;
        for (QuestObjective const& objective : quest.objectives)
            for (AreaPoint const& area : objective.areas)
            {
                placed = placed || objective.kind != ObjectiveKind::Turnin;
                elsewhere = elsewhere && area.mapId != place.mapId;
            }
        if (placed && elsewhere)
            ids.push_back(quest.id);
    }
    return ids;
}

QuestSlot LogSlot(uint32 quest, uint32 state)
{
    QuestSlot slot;
    slot.quest = quest;
    slot.state = state;
    return slot;
}

void AddChecks(std::vector<CircuitHarness::Check>& checks, std::vector<CircuitHarness::Check> const& more)
{
    checks.insert(checks.end(), more.begin(), more.end());
}

void AddPlanLines(std::vector<std::string>& lines, std::string const& name, CircuitHarness::Report const& report)
{
    lines.push_back(Acore::StringFormat(
        "{} (hub camp {}):", name,
        report.plan.homeCamp == NO_CAMP ? std::string("none") : std::to_string(report.plan.homeCamp)));
    for (std::string const& line : CircuitHarness::PlanLines(report.plan, report.baselineYards))
        lines.push_back("  " + line);
}
}  // namespace

void StartQuestIndex()
{
    QuestIndexState expected = QuestIndexState::NotStarted;
    if (!questState.compare_exchange_strong(expected, QuestIndexState::Building))
        return;  // once: quest data is static after startup (WT.12)

    auto const gatherStart = std::chrono::steady_clock::now();
    auto input = std::make_shared<QuestGather>(GatherQuests());
    uint32 const gatherMs = MsSince(gatherStart);
    LOG_INFO("server.loading",
             "Quest index: gathered {} quests in {} ms ({} relations, {} giver and ender spawns, {} target spawns, {} "
             "POIs, {} area triggers; {} spawns unresolved)",
             input->quests.size(), gatherMs, input->relations.size(), input->places.size(), input->targets.size(),
             input->pois.size(), input->triggers.size(), input->unresolved);

    bool const posted = Pool().Post(
        [input, gatherMs]
        {
            auto const buildStart = std::chrono::steady_clock::now();
            try
            {
                auto index = std::make_shared<QuestIndex>(QuestIndex::Build(std::move(*input)));
                index->stats.gatherMs = gatherMs;
                index->stats.buildMs = MsSince(buildStart);
                QuestIndexStats const& s = index->stats;
                LOG_INFO("playerbots",
                         "Quest index built in {} ms on a worker: {} quests, {} givers, {} enders, {} camps, {} "
                         "objectives ({} without area), {} quests without a giver, about {:.1f} MB (estimate)",
                         s.buildMs, s.quests, s.givers, s.enders, s.camps, s.objectives, s.withoutArea, s.withoutGiver,
                         static_cast<double>(s.estimatedBytes) / BYTES_PER_MB);
                questBook.Publish(std::move(index));
                questState.store(QuestIndexState::Ready, std::memory_order_release);
            }
            catch (std::exception const& e)
            {
                questState.store(QuestIndexState::Failed, std::memory_order_release);
                LOG_ERROR("playerbots", "Quest index: the build failed: {}", e.what());
            }
        });
    if (!posted)
    {
        questState.store(QuestIndexState::Failed, std::memory_order_release);
        LOG_ERROR("server.loading", "Quest index: the planner pool is not running; no quest index");
    }
    LoadQuestStats();
}

std::shared_ptr<QuestStatsView const> QuestStats() { return statsSlot.Current(); }

void QuestStatAccepted(Player* bot, uint32 questId)
{
    if (StatsOn(bot))
        TimedEvent(bot, [questId](uint32 guid, uint32 level) { statsBook.Accept(guid, questId, level, StatsNowMs()); });
}

void QuestStatRewarded(Player* bot, uint32 questId)
{
    if (StatsOn(bot))
        TimedEvent(bot, [questId](uint32 guid, uint32 level) { statsBook.Reward(guid, questId, level, StatsNowMs()); });
}

void QuestStatAbandoned(Player* bot, uint32 questId)
{
    if (StatsOn(bot))
        TimedEvent(bot,
                   [questId](uint32 guid, uint32 level) { statsBook.Abandon(guid, questId, level, StatsNowMs()); });
}

void QuestStatDropped(Player* bot, uint32 questId)
{
    if (StatsOn(bot))
        TimedEvent(bot, [questId](uint32 guid, uint32 level) { statsBook.Drop(guid, questId, level); });
}

void QuestStatDeath(Player* bot)
{
    if (!StatsOn(bot))
        return;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;
    // The death counts for the quest the bot's rpg status is doing (its map thread, like the status update)
    auto const* doing = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (!doing || !doing->questId)
        return;
    uint32 const questId = doing->questId;
    TimedEvent(bot, [questId](uint32 guid, uint32 level) { statsBook.Death(guid, questId, level); });
}

uint32 QuestStatsFlushNow()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::QuestStatsFlushNow");
    if (!statsOn.load(std::memory_order_acquire))
        return 0;
    statsFlushTimer = 0;
    std::vector<std::pair<uint64, QuestStatCell>> const rows = statsBook.TakeDirty(StatsNowMs());
    if (statsLoad == StatsLoad::Loaded && !rows.empty())
    {
        // Totals with explicit values: a repeated or late write leaves the same row
        PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
        for (auto const& [key, cell] : rows)
        {
            std::string const minutes = EncodeMinutes(cell);
            transaction->Append(Acore::StringFormat(
                "INSERT INTO playerbots_quest_stats (quest_id, band, accepted, completed, abandoned, deaths, minutes) "
                "VALUES ({}, {}, {}, {}, {}, {}, '{}') ON DUPLICATE KEY UPDATE accepted = {}, completed = {}, "
                "abandoned = {}, deaths = {}, minutes = '{}'",
                StatQuest(key), StatBand(key), cell.accepted, cell.completed, cell.abandoned, cell.deaths, minutes,
                cell.accepted, cell.completed, cell.abandoned, cell.deaths, minutes));
        }
        PlayerbotsDatabase.CommitTransaction(transaction);
        statsRowsWritten += rows.size();
    }
    statsSlot.Publish(statsBook.View());
    ++statsFlushes;
    statsLastRows = static_cast<uint32>(rows.size());
    statsLastFlush = std::chrono::steady_clock::now();
    return statsLastRows;
}

void UpdateQuestStats(uint32 diff)
{
    if (!statsOn.load(std::memory_order_relaxed))
        return;
    statsFlushTimer += diff;
    if (statsFlushTimer >= STATS_FLUSH_MS)
        QuestStatsFlushNow();
}

void QuestStatsShutdown()
{
    if (!statsOn.load(std::memory_order_acquire))
        return;
    uint32 const rows = QuestStatsFlushNow();
    statsOn.store(false, std::memory_order_release);
    LOG_INFO("playerbots", "Quest stats: {} rows written at shutdown, {} since startup", rows, statsRowsWritten);
}

std::shared_ptr<QuestIndex const> Quests() { return questBook.Current(); }

bool QuestsReady() { return questState.load(std::memory_order_acquire) == QuestIndexState::Ready; }

std::string QuestIndexStatus()
{
    QuestIndexState const state = questState.load(std::memory_order_acquire);
    std::shared_ptr<QuestIndex const> const quests = state == QuestIndexState::Ready ? Quests() : nullptr;
    if (!quests)
        return Acore::StringFormat("PLANNER quest index: {}", state == QuestIndexState::Building ? "building"
                                                              : state == QuestIndexState::Failed ? "failed"
                                                                                                 : "not started");
    QuestIndexStats const& s = quests->stats;
    return Acore::StringFormat(
        "PLANNER quest index: ready, {} quests, {} places ({} givers, {} enders, {} unresolved), {} camps, {} "
        "objectives "
        "({} without area), {} quests without a giver, gathered in {} ms, built in {} ms on a worker, about {:.1f} MB "
        "(estimate)",
        s.quests, s.places, s.givers, s.enders, s.unresolved, s.camps, s.objectives, s.withoutArea, s.withoutGiver,
        s.gatherMs, s.buildMs, static_cast<double>(s.estimatedBytes) / BYTES_PER_MB);
}

bool HandleQuestCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string word;
    in >> word;
    if (word == "stats")
        return HandleQuestStats(handler);
    if (word == "flush")
    {
        if (!statsOn.load(std::memory_order_acquire))
        {
            handler->SendErrorMessage("PLANNER: the quest statistics are not loaded");
            return false;
        }
        uint32 const rows = QuestStatsFlushNow();
        handler->PSendSysMessage(
            "QUEST stats flush: {} rows {}", rows,
            statsLoad == StatsLoad::Loaded ? "written (asynchronous)" : "taken, memory only (no table)");
        return true;
    }
    uint32 id = 0;
    if (!(std::istringstream(word) >> id) || !id)
    {
        handler->SendErrorMessage("Usage: playerbots planner quest <id> | stats | flush");
        return false;
    }
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    QuestRecord const* quest = quests->Find(id);
    if (!quest)
    {
        handler->SendErrorMessage("PLANNER: no quest {}", id);
        return false;
    }
    std::shared_ptr<PlannerIndexes const> const indexes = IndexesReady() ? Indexes() : nullptr;
    Quest const* proto = sObjectMgr->GetQuestTemplate(id);
    std::string const zone = quest->zoneOrSort > 0 ? AreaName(handler, static_cast<uint32>(quest->zoneOrSort))
                                                   : Acore::StringFormat("quest sort {}", -quest->zoneOrSort);
    handler->PSendSysMessage(
        "QUEST {} \"{}\": level {}, min {}, max {}, type {}, {} suggested, flags {}, races {:#x}, classes {:#x}, {}, "
        "xp {}, money {}",
        id, proto ? proto->GetTitle() : "?", quest->level, quest->minLevel, quest->maxLevel, quest->type,
        quest->suggestedPlayers, FlagText(*quest), quest->raceMask, quest->classMask, zone, quest->xpBase,
        quest->copperReward);
    handler->PSendSysMessage(
        "  chain: prev {}, next {}, next in chain {}, exclusive group {}, breadcrumb for {}; "
        "unlocks {}",
        quest->prev, quest->next, quest->nextInChain, quest->exclusiveGroup, quest->breadcrumbFor,
        ListText(quests->Unlocks(id)));
    SendPlaces(handler, *quests, indexes.get(), quest->givers, "giver");
    SendPlaces(handler, *quests, indexes.get(), quest->enders, "ender");
    for (QuestObjective const& objective : quest->objectives)
    {
        std::string areas;
        for (AreaPoint const& area : objective.areas)
            areas += Acore::StringFormat("{}({}, {:.0f}, {:.0f}) {:.0f} yd, {} points", areas.empty() ? "" : "; ",
                                         area.mapId, area.x, area.y, area.radius, area.points);
        handler->PSendSysMessage("  {} {} {} x{}: {} areas{}{}", ObjectiveName(objective.kind), objective.target,
                                 TargetName(objective), objective.count, objective.areas.size(),
                                 areas.empty() ? "" : ": ", areas);
    }
    SendQuestStats(handler, id);
    return true;
}

Compute CircuitCompute(CircuitWeights const& weights, std::vector<uint32> const& protectedQuests,
                       std::vector<QuestProgress> const& progress)
{
    CircuitData data = CircuitDataNow();
    CircuitContext context = data.Context();
    context.weights = weights;
    context.protectedQuests = protectedQuests;
    context.progress = progress;
    return [data = std::move(data), context = std::move(context)](Snapshot const& snapshot) -> std::optional<Payload>
    {
        if (!data.quests)
            return std::nullopt;
        auto plan = std::make_shared<CircuitPlan const>(PlanCircuit(snapshot, context));
        if (!plan->ok)
            return std::nullopt;
        return Payload{CircuitAnswer{std::move(plan)}};
    };
}

bool RunCircuitHarness(ChatHandler* handler)
{
    if (!IndexesReady() || !QuestsReady())
    {
        handler->SendErrorMessage("PLANNER circuit selftest: not ready (see `playerbots planner indexes`)");
        return false;
    }
    auto const started = std::chrono::steady_clock::now();
    CircuitData const data = CircuitDataNow();
    CircuitContext const context = data.Context();
    QuestIndex const& quests = *data.quests;
    std::vector<CircuitHarness::Check> checks;
    std::vector<std::string> plans;
    for (CircuitPlace const& place : CIRCUIT_PLACES)
    {
        Place start;
        std::string text;
        bool const found = CircuitStart(quests, place, start, text);
        checks.push_back({found, std::string(place.name) + ": " + text});
        if (!found)
            continue;

        // An empty log at the hub after a levelled bot's history: most fitting quests taken, only fitting ones, the log
        // within its limit, one turn-in batch
        CircuitHarness::Scenario scenario{place.name, CircuitBot(place, start), context, {}};
        scenario.bot.rewardedQuests = GreyHistory(quests, place.level, data.greyDiff);
        CircuitHarness::Report const report = CircuitHarness::RunScenario(scenario, CIRCUIT_TIMING_RUNS);
        AddChecks(checks, report.checks);
        AddPlanLines(plans, scenario.name, report);
        if (&place == &CIRCUIT_PLACES.front())
        {
            AddChecks(checks, CircuitHarness::GuardChecks(scenario));
            std::optional<Payload> const answer = CircuitCompute(CircuitWeights{}, {}, {})(scenario.bot);
            CircuitAnswer const* circuit = answer ? std::get_if<CircuitAnswer>(&*answer) : nullptr;
            bool const same = circuit && circuit->plan &&
                              CircuitHarness::PlanSummary(*circuit->plan) == CircuitHarness::PlanSummary(report.plan);
            checks.push_back({same, scenario.name + ": the job wrapper answers the same plan"});
        }
        std::vector<uint32> const hub = HubQuests(quests, start);

        // The next circuit with the carried quests done in the log (review 2, finding A): they stay out of the loop,
        // which visits the same ender camps
        if (!report.plan.carried.empty())
        {
            CircuitHarness::Scenario carrying{scenario.name + "-carrying", scenario.bot, context, {}};
            for (uint32 const id : report.plan.carried)
                carrying.bot.quests.push_back(LogSlot(id, Circuit::SLOT_COMPLETE));
            carrying.expect.minTake = 0;
            carrying.expect.carried = report.plan.carried;
            carrying.expect.checkEnderCamps = true;
            carrying.expect.enderCamps = report.plan.enderCamps;
            CircuitHarness::Report const carryingReport = CircuitHarness::RunScenario(carrying, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, carryingReport.checks);
            AddPlanLines(plans, carrying.name, carryingReport);
        }

        // A full log of quests from another map and a grey one: the grey one goes, far ones make room for every quest
        // the empty log took to work here (a delivery only when worth more than the far quests it pushes out, so it
        // may wait for log space), a protected far one stays
        std::vector<uint32> const fillers = FarQuests(quests, place, hub, data.greyDiff, false, QUEST_LOG_SLOTS - 1);
        std::vector<uint32> const grey = FarQuests(quests, place, hub, data.greyDiff, true, 1);
        bool const fixture = fillers.size() == QUEST_LOG_SLOTS - 1 && grey.size() == 1;
        checks.push_back({fixture, Acore::StringFormat("{}-full-log: {} far quests and {} grey one fill the log",
                                                       scenario.name, fillers.size(), grey.size())});
        if (fixture)
        {
            CircuitHarness::Scenario full{scenario.name + "-full-log", scenario.bot, context, {}};
            std::erase(full.bot.rewardedQuests, grey.front());
            for (uint32 const id : fillers)
                full.bot.quests.push_back(LogSlot(id, 0));
            full.bot.quests.push_back(LogSlot(grey.front(), 0));
            full.context.protectedQuests = {fillers.front()};
            std::vector<uint32> worked;  // the empty log's takes worked here
            for (CircuitTake const& take : report.plan.take)
                if (!take.carried)
                    worked.push_back(take.quest);
            std::sort(worked.begin(), worked.end());
            full.expect.minTake = worked.size();
            full.expect.checkAbandon = true;
            full.expect.abandonFrom.assign(fillers.begin() + 1, fillers.end());
            full.expect.abandonFrom.push_back(grey.front());
            full.expect.abandonMust = grey;
            full.expect.keep = {fillers.front()};
            CircuitHarness::Report const fullReport = CircuitHarness::RunScenario(full, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, fullReport.checks);
            std::vector<uint32> const was = CircuitHarness::TakenIds(report.plan);
            std::vector<uint32> const fullTaken = CircuitHarness::TakenIds(fullReport.plan);
            bool const keepsWorked = std::includes(fullTaken.begin(), fullTaken.end(), worked.begin(), worked.end());
            bool const onlyEmpty = std::includes(was.begin(), was.end(), fullTaken.begin(), fullTaken.end());
            checks.push_back({keepsWorked && onlyEmpty,
                              Acore::StringFormat("{}-full-log: takes {}: each the empty log works here, none it left",
                                                  scenario.name, CircuitHarness::IdList(fullTaken))});
            AddPlanLines(plans, full.name, fullReport);

            // The log full of those quests done (review 3, finding 1): complete quests are never abandoned and none is
            // turned in here, so the hub's quests wait for log space that only delivering frees. No slot frees at a
            // chain or camp, so the bot moves on only to deliver (reviews 4 and 5); with every ender on another map no
            // delivery is in reach, so it finds nowhere. The generic checks hold it to that and follow its moves
            CircuitHarness::Scenario done{scenario.name + "-full-log-done", scenario.bot, context, {}};
            for (std::size_t i = 0; i + Circuit::LOG_RESERVE < QUEST_LOG_SLOTS; ++i)
                done.bot.quests.push_back(LogSlot(fillers[i], Circuit::SLOT_COMPLETE));
            done.expect.exactTake = true;
            done.expect.minTake = 0;
            CircuitHarness::ExactAbandon(done.expect, {});
            CircuitHarness::Report const doneReport = CircuitHarness::RunScenario(done, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, doneReport.checks);
            AddPlanLines(plans, done.name, doneReport);
        }

        // The same hub with every quest its givers offer rewarded: the bot moves on
        CircuitHarness::Scenario dry{scenario.name + "-dry", scenario.bot, context, {}};
        std::vector<uint32> done;
        std::set_union(scenario.bot.rewardedQuests.begin(), scenario.bot.rewardedQuests.end(), hub.begin(), hub.end(),
                       std::back_inserter(done));
        dry.bot.rewardedQuests = std::move(done);
        dry.expect.minTake = 0;
        dry.expect.moveInBracket = true;
        CircuitHarness::Report const dryReport = CircuitHarness::RunScenario(dry, CIRCUIT_TIMING_RUNS);
        AddChecks(checks, dryReport.checks);
        AddPlanLines(plans, dry.name, dryReport);

        // The dry hub with the log full of the far quests, none protected (review 5, finding 1): any hub that wants a
        // slot gives one of them up, so the bot still moves on, and the generic checks see it take there
        if (fixture)
        {
            CircuitHarness::Scenario dryFull{scenario.name + "-dry-full-log", dry.bot, context, {}};
            for (uint32 const id : fillers)
            {
                std::erase(dryFull.bot.rewardedQuests, id);
                dryFull.bot.quests.push_back(LogSlot(id, 0));
            }
            dryFull.expect.minTake = 0;
            dryFull.expect.moveInBracket = true;
            CircuitHarness::ExactAbandon(dryFull.expect, {});
            CircuitHarness::Report const dryFullReport = CircuitHarness::RunScenario(dryFull, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, dryFullReport.checks);
            AddPlanLines(plans, dryFull.name, dryFullReport);
        }
    }

    std::size_t failed = 0;
    for (CircuitHarness::Check const& check : checks)
    {
        failed += check.pass ? 0 : 1;
        handler->PSendSysMessage("{} {}", check.pass ? "PASS" : "FAIL", check.text);
    }
    for (std::string const& line : plans)
        handler->PSendSysMessage("{}", line);
    std::string const summary =
        Acore::StringFormat("PLANNER circuit selftest: {}, {} checks, {} failed, in {} ms (grey difference {})",
                            failed ? "FAIL" : "PASS", checks.size(), failed, MsSince(started), data.greyDiff);
    handler->PSendSysMessage("{}", summary);
    LOG_INFO("playerbots", "{}", summary);
    return !failed;
}

bool HandleCircuitCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string word;
    in >> word;
    if (word.empty())
    {
        handler->SendErrorMessage("Usage: playerbots planner circuit selftest | <player name>");
        return false;
    }
    // The summary line carries the verdict; false here would only add the usage text
    if (word == "selftest")
    {
        RunCircuitHarness(handler);
        return true;
    }
    if (!IndexesReady() || !QuestsReady())
    {
        handler->SendErrorMessage("PLANNER circuit: not ready (see `playerbots planner indexes`)");
        return false;
    }
    std::string name = word;
    Player* player = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    if (!player || !player->IsInWorld())
    {
        handler->SendErrorMessage("PLANNER circuit: no player {} online", word);
        return false;
    }
    // Read only, on the world thread between map updates; the log quests' elapsed minutes come with WT16
    CircuitData const data = CircuitDataNow();
    Snapshot const snapshot = FillSnapshot(player, 0);
    CircuitPlan const plan = PlanCircuit(snapshot, data.Context());
    // The world log's decision rows of this plan (kind circuit); WT16 emits the same rows where it applies plans
    if (WorldLog::Active())
        for (CircuitLog::Row const& row : CircuitLog::Rows(plan))
            WorldLog::Decision(player, WorldLogFormat::DecisionKind::Circuit, row.choice, row.reason, row.value);
    handler->PSendSysMessage("CIRCUIT {}: level {}, map {} ({:.0f}, {:.0f}), {} log quests, hub camp {}",
                             player->GetName(), snapshot.level, snapshot.position.map, snapshot.position.x,
                             snapshot.position.y, snapshot.quests.size(),
                             plan.homeCamp == NO_CAMP ? std::string("none") : std::to_string(plan.homeCamp));
    float const baseline = plan.ok ? BaselineYards(plan, *data.quests) : 0.0f;
    for (std::string const& line : CircuitHarness::PlanLines(plan, baseline))
        handler->PSendSysMessage("  {}", line);
    return true;
}

// WT14-2: the goal model on real data. The circuit, hubs, quests and services are the real indexes at the circuit
// harness's places; the grind spots and the herb node are synthetic (the world has no spot or node index yet, WT16)
namespace
{
constexpr float GOALS_SPOT_XP = 60.0f;  // synthetic grind spots: XP and copper per kill
constexpr float GOALS_SPOT_COPPER = 30.0f;
constexpr float GOALS_NODE_COPPER = 400.0f;  // synthetic herb node: what its yield sells for
constexpr uint32 GOALS_SPOT_ID = 9001;
constexpr uint32 GOALS_SPOT_GREY_ID = 9002;
constexpr uint32 GOALS_NODE_ID = 9101;
constexpr uint32 GOALS_ZONE_SPAN = 5;  // the synthetic zone bracket around the bot's level
constexpr uint32 GOALS_FLUSH = 1000000;

Goals::Situation GoalsSituation(Snapshot const& bot, Place const& start)
{
    Goals::Situation situation = GoalHarness::Base();
    situation.here.zoneLow = bot.level > GOALS_ZONE_SPAN ? bot.level - GOALS_ZONE_SPAN : 1;
    situation.here.zoneHigh = bot.level + GOALS_ZONE_SPAN;
    uint32 const level = bot.level;
    Goals::GrindSpot spot =
        GoalHarness::Spot(GOALS_SPOT_ID, GOALS_SPOT_XP, GOALS_SPOT_COPPER, level > 1 ? level - 1 : 1, level + 1);
    spot.place = start;
    Goals::GrindSpot grey = spot;
    grey.id = GOALS_SPOT_GREY_ID;
    grey.minLevel = 1;
    grey.maxLevel = std::max(Goals::GreyLevel(level), 1u);
    situation.spots = {spot, grey};
    Goals::GatherNode node;
    node.id = GOALS_NODE_ID;
    node.skill = GoalHarness::HERBALISM;
    node.place = start;
    node.walkMinutes = 1.0f;
    node.copperWorth = GOALS_NODE_COPPER;
    situation.nodes = {node};
    return situation;
}

std::string GoalsLine(Goals::Ranked const& ranked)
{
    return Acore::StringFormat("  ranked: {}", GoalHarness::Summary(ranked));
}

// A real option names a hub, camp, spot, node, quest, vendor, item or inn, never the no-hub or no-camp marker, and a
// move to a camp leads there, not back to where the circuit ends
bool GoalTargetsOk(Goals::GoalPlan const& plan)
{
    for (Goals::Option const& option : plan.ranked.options)
    {
        if (option.target == NO_HUB || option.target == NO_CAMP)
            return false;
        if (option.targetType == Goals::TargetType::Camp && option.place.x == plan.circuit.returnTo.x &&
            option.place.y == plan.circuit.returnTo.y)
            return false;
    }
    return true;
}
}  // namespace

bool RunGoalsHarness(ChatHandler* handler)
{
    auto const started = std::chrono::steady_clock::now();
    std::vector<GoalHarness::Check> checks = GoalHarness::SyntheticChecks();
    std::size_t const synthetic = checks.size();
    std::vector<std::string> lines;
    if (!IndexesReady() || !QuestsReady())
        checks.push_back({"real: data", false, "indexes or quests not ready (see `playerbots planner indexes`)"});
    else
    {
        CircuitData const data = CircuitDataNow();
        CircuitContext const context = data.Context();
        for (CircuitPlace const& place : CIRCUIT_PLACES)
        {
            Place start;
            std::string text;
            bool const found = CircuitStart(*data.quests, place, start, text);
            std::string const name = std::string("real ") + place.name;
            if (!found)
            {
                checks.push_back({name + ": start", false, text});
                continue;
            }
            Snapshot bot = CircuitBot(place, start);
            bot.rewardedQuests = GreyHistory(*data.quests, place.level, data.greyDiff);
            bot.money = GOALS_FLUSH;
            Goals::GoalPlan const flush = Goals::PlanGoals(bot, context, GoalsSituation(bot, start));
            Goals::GoalPlan const again = Goals::PlanGoals(bot, context, GoalsSituation(bot, start));
            lines.push_back(name + " flush");
            lines.push_back(GoalsLine(flush.ranked));
            checks.push_back(
                {name + ": a flush bot's ranking is made, with the circuit among its options",
                 flush.ranked.ok && (!flush.circuit.ok || GoalHarness::Has(flush.ranked, Goals::GoalKind::Quest)),
                 GoalHarness::Summary(flush.ranked)});
            checks.push_back({name + ": a flush bot does not choose money-making",
                              flush.ranked.ok && !flush.ranked.options.empty() &&
                                  flush.ranked.options.front().kind != Goals::GoalKind::Money,
                              GoalHarness::Summary(flush.ranked)});
            checks.push_back({name + ": the same input gives the same ranking",
                              GoalHarness::SameList(flush.ranked, again.ranked), "determinism"});
            bot.money = 0;
            Goals::GoalPlan const broke = Goals::PlanGoals(bot, context, GoalsSituation(bot, start));
            lines.push_back(name + " broke");
            lines.push_back(GoalsLine(broke.ranked));
            checks.push_back({name + ": a broke bot weighs money-making",
                              broke.ranked.ok && GoalHarness::Has(broke.ranked, Goals::GoalKind::Money),
                              GoalHarness::Summary(broke.ranked)});
            std::vector<GoalLog::Row> const rows = GoalLog::Rows(broke.ranked);
            bool const rowOk = rows.size() == 1 && !broke.ranked.options.empty() &&
                               rows[0].choice == Goals::ChoiceWord(broke.ranked.options.front());
            checks.push_back({name + ": the ranking maps to one goal row", rowOk,
                              rows.empty() ? std::string("no row") : rows[0].choice + " " + rows[0].reason});
            // A dry hub: every quest of the hub done; the moves are the circuit harness's proof, here the ranking
            Snapshot dry = bot;
            std::vector<uint32> const hub = HubQuests(*data.quests, start);
            std::vector<uint32> done;
            std::set_union(dry.rewardedQuests.begin(), dry.rewardedQuests.end(), hub.begin(), hub.end(),
                           std::back_inserter(done));
            dry.rewardedQuests = std::move(done);
            dry.money = GOALS_FLUSH;
            Goals::GoalPlan const dryPlan = Goals::PlanGoals(dry, context, GoalsSituation(dry, start));
            lines.push_back(name + " dry hub");
            lines.push_back(GoalsLine(dryPlan.ranked));
            checks.push_back({name + ": a dry hub still ranks goals",
                              dryPlan.ranked.ok && !dryPlan.ranked.options.empty(),
                              GoalHarness::Summary(dryPlan.ranked)});
            checks.push_back({name + ": every option names a real target",
                              GoalTargetsOk(flush) && GoalTargetsOk(broke) && GoalTargetsOk(dryPlan),
                              GoalHarness::Summary(dryPlan.ranked)});
        }
    }
    std::size_t failed = 0;
    for (GoalHarness::Check const& check : checks)
    {
        failed += check.pass ? 0 : 1;
        handler->PSendSysMessage("{} {}: {}", check.pass ? "PASS" : "FAIL", check.name, check.text);
    }
    for (std::string const& line : lines)
        handler->PSendSysMessage("{}", line);
    std::string const summary = Acore::StringFormat(
        "PLANNER goals selftest: {}, {} checks ({} synthetic), {} failed, in {} ms, rank {:.1f} us",
        failed ? "FAIL" : "PASS", checks.size(), synthetic, failed, MsSince(started), GoalHarness::RankMicroseconds());
    handler->PSendSysMessage("{}", summary);
    LOG_INFO("playerbots", "{}", summary);
    return !failed;
}

bool HandleGoalsCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string word;
    in >> word;
    if (word.empty())
    {
        handler->SendErrorMessage("Usage: playerbots planner goals selftest | <player name>");
        return false;
    }
    if (word == "selftest")
    {
        RunGoalsHarness(handler);
        return true;
    }
    if (!IndexesReady() || !QuestsReady())
    {
        handler->SendErrorMessage("PLANNER goals: not ready (see `playerbots planner indexes`)");
        return false;
    }
    std::string name = word;
    Player* player = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    if (!player || !player->IsInWorld())
    {
        handler->SendErrorMessage("PLANNER goals: no player {} online", word);
        return false;
    }
    // Read only, on the world thread: the circuit and the Situation inputs the world gives now (health, mana); grind
    // spots, nodes, vendors and inns are WT16's inputs. A caller offers a hearth option only when
    // WorldTravel::InnCheckDue says so; no inns are given here, so none is offered
    CircuitData const data = CircuitDataNow();
    Snapshot const snapshot = FillSnapshot(player, 0);
    Goals::Situation situation;
    situation.healthPercent = player->GetHealthPct();
    uint32 const maxMana = player->GetMaxPower(POWER_MANA);
    situation.manaPercent =
        maxMana > 0 ? 100.0f * static_cast<float>(player->GetPower(POWER_MANA)) / static_cast<float>(maxMana) : -1.0f;
    Goals::GoalPlan const plan = Goals::PlanGoals(snapshot, data.Context(), std::move(situation));
    if (WorldLog::Active())
        for (GoalLog::Row const& row : GoalLog::Rows(plan.ranked))
        {
            WorldLogFormat::Alternatives alternatives;
            for (GoalLog::Alt const& alt : row.alternatives)
                alternatives.Add({alt.choice, alt.value});
            WorldLog::Decision(player, WorldLogFormat::DecisionKind::Goal, row.choice, row.reason, row.value,
                               alternatives);
        }
    handler->PSendSysMessage("GOALS {}: level {}, map {} ({:.0f}, {:.0f}), money {}, need factor {:.2f}, {} considered",
                             player->GetName(), snapshot.level, snapshot.position.map, snapshot.position.x,
                             snapshot.position.y, snapshot.money, plan.ranked.need.factor, plan.ranked.considered);
    if (!plan.ranked.ok)
        handler->PSendSysMessage("  no ranking: {}", plan.ranked.failure);
    for (Goals::Option const& option : plan.ranked.options)
        handler->PSendSysMessage("  {} {:.1f}/min over {:.1f} min: {}", Goals::ChoiceWord(option),
                                 option.valuePerMinute, option.minutes, option.reason);
    return true;
}
}  // namespace Planner
