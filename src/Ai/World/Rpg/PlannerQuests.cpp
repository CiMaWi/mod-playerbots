/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The server side of the quest index (travel plan WT13a): the world-thread gather, the worker build, the published
// index and the GM query

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_set>

#include "Chat.h"
#include "DBCStores.h"
#include "GameObjectData.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PlannerService.h"
#include "QuestDef.h"
#include "QuestIndex.h"
#include "StringFormat.h"
#include "ThreadCheck.h"

namespace Planner
{
static_assert(QuestType::ELITE == QUEST_TYPE_ELITE && QuestType::PVP == QUEST_TYPE_PVP &&
                  QuestType::RAID == QUEST_TYPE_RAID && QuestType::DUNGEON == QUEST_TYPE_DUNGEON &&
                  QuestType::HEROIC == QUEST_TYPE_HEROIC && QuestType::RAID_10 == QUEST_TYPE_RAID_10 &&
                  QuestType::RAID_25 == QUEST_TYPE_RAID_25,
              "the planner's quest types are the core's");
static_assert(POI_FIRST_ITEM == QUEST_OBJECTIVES_COUNT, "item POIs follow the creature or object slots");

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

uint32 MsSince(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
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
    uint32 id = 0;
    if (!(in >> id) || !id)
    {
        handler->SendErrorMessage("Usage: playerbots planner quest <id>");
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
    return true;
}
}  // namespace Planner
