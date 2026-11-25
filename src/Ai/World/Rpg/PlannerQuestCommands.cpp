/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The quest index's GM commands and harnesses, apart from its server side in PlannerQuests.cpp (CS37a).
// `planner quest`, `questuse`, `classquests`, `sources`, `places` and `capability` with the capability census;
// the automatic class-quest completion (upkeep WU21a).
// The questing circuit's job wrapper, its real-place harness and `planner circuit` (WT13b; the planner: QuestCircuit.h)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include "BotKind.h"
#include "Chat.h"
#include "ClassQuestCompletion.h"
#include "ClassQuestJoin.h"
#include "Config.h"
#include "DBCStores.h"
#include "GameObjectData.h"
#include "GoalInterface.h"
#include "GoalModel.h"
#include "GoalModelHarness.h"
#include "GoalModelLog.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlannerIndexes.h"
#include "PlannerQuestInternal.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestCircuit.h"
#include "QuestCircuitHarness.h"
#include "QuestCircuitLog.h"
#include "QuestDef.h"
#include "QuestExecRules.h"
#include "QuestFailRecord.h"
#include "QuestFields.h"
#include "QuestIndex.h"
#include "QuestStats.h"
#include "QuestUseRules.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Upkeep.h"
#include "WorldLog.h"

namespace Planner
{
namespace
{
constexpr std::size_t QUERY_PLACES = 4;  // givers and enders listed by `planner quest`

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
    if (quest.event != QuestEvent::None)
        text += Acore::StringFormat("{}{}", text.empty() ? "" : ", ",
                                    quest.event == QuestEvent::Escort ? "escort" : "script event");
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

// WT36a: what the planner's feed offers by the capability table (QuestExecRules.h): per side and level band (of the
// minimum level, QuestStats.h) the quests of the side, those offered (the executor does every objective), those not
// by their first refused work; and per side the offered quests by each work they hold
constexpr std::size_t CAPABILITY_BANDS = 17;  // levels 0-84
constexpr std::size_t REFUSAL_PREFIX = 12;    // "unsupported:" before the refused work's word
constexpr std::array<char const*, SIDES> SIDE_NAMES = {"Alliance", "Horde"};

struct CapabilityCensus
{
    std::array<std::array<uint32, CAPABILITY_BANDS>, SIDES> total{};
    std::array<std::array<uint32, CAPABILITY_BANDS>, SIDES> offered{};
    std::array<std::array<std::array<uint32, CAPABILITY_BANDS>, QuestExec::WORKS>, SIDES> refused{};
    std::array<std::array<uint32, QuestExec::WORKS>, SIDES> offeredWith{};
    uint32 offeredUnsupported{0};  // the invariant: a record the feed passes that the executor refuses (must be 0)
};

bool OfSide(QuestRecord const& quest, std::uint8_t side)
{
    return !quest.raceMask || (quest.raceMask & SIDE_RACE_MASKS[side]);
}

char const* RefusedWord(QuestExec::Work work) { return QuestExec::CapabilityOf(work).refusal + REFUSAL_PREFIX; }

// Whether the feed passes the quest for some character of the side: a race of the side, a class and a level the
// template allows (Eligible, with its capability refusal)
bool FeedPasses(QuestRecord const& quest, std::uint8_t side)
{
    uint8 const level = static_cast<uint8>(std::clamp<uint32>(quest.minLevel, 1, 255));
    uint8 cls = 1;
    while (quest.classMask && cls < 32 && !(quest.classMask & (1u << (cls - 1))))
        ++cls;
    for (uint8 race = 1; race <= 32; ++race)
        if ((SIDE_RACE_MASKS[side] & (1u << (race - 1))) && Eligible(quest, race, cls, level))
            return true;
    return false;
}

CapabilityCensus CensusOf(QuestIndex const& quests)
{
    CapabilityCensus census;
    for (QuestRecord const& quest : quests.quests)
        for (std::uint8_t side = 0; side < SIDES; ++side)
        {
            if (!OfSide(quest, side))
                continue;
            std::size_t const band = std::min<std::size_t>(LevelBandOf(quest.minLevel), CAPABILITY_BANDS - 1);
            ++census.total[side][band];
            QuestExec::Step const refusal = QuestExec::FirstRefusal(quest, side);
            if (refusal.act == QuestExec::Act::Refused)
            {
                ++census.refused[side][static_cast<std::size_t>(refusal.work)][band];
                if (FeedPasses(quest, side))
                    ++census.offeredUnsupported;
                continue;
            }
            ++census.offered[side][band];
            std::array<bool, QuestExec::WORKS> with{};
            for (uint32 i = 0; i < QuestExec::ObjectiveCount(quest); ++i)
                if (QuestExec::Work const work = QuestExec::WorkOf(QuestExec::ObjectiveAt(quest, i), side);
                    work != QuestExec::Work::Count)
                    with[static_cast<std::size_t>(work)] = true;
            for (std::size_t w = 0; w < QuestExec::WORKS; ++w)
                census.offeredWith[side][w] += with[w] ? 1 : 0;
        }
    return census;
}

uint32 SumOf(std::array<uint32, CAPABILITY_BANDS> const& bands)
{
    return std::accumulate(bands.begin(), bands.end(), uint32{0});
}

// "escort 12, event 30, ..." over the works with a count; "none" without one
template <typename CountOf>
std::string WorkCounts(CountOf countOf, bool refusals)
{
    std::string text;
    for (std::size_t w = 0; w < QuestExec::WORKS; ++w)
        if (uint32 const n = countOf(w))
            text += Acore::StringFormat(
                "{}{} {}", text.empty() ? "" : ", ",
                refusals ? RefusedWord(static_cast<QuestExec::Work>(w)) : QuestExec::CAPABILITIES[w].name, n);
    return text.empty() ? std::string("none") : text;
}

std::string SideLine(CapabilityCensus const& c, std::uint8_t side)
{
    uint32 const total = SumOf(c.total[side]);
    uint32 const offered = SumOf(c.offered[side]);
    return Acore::StringFormat("{}: {} quests, {} offered, {} not offered ({})", SIDE_NAMES[side], total, offered,
                               total - offered,
                               WorkCounts([&](std::size_t w) { return SumOf(c.refused[side][w]); }, true));
}
}  // namespace

void LogCapabilityCensus(QuestIndex const& quests)
{
    CapabilityCensus const capability = CensusOf(quests);
    LOG_INFO("playerbots", "Quest index capability (planner feed): {}; {}; offered and unsupported: {}",
             SideLine(capability, 0), SideLine(capability, 1), capability.offeredUnsupported);
}

// WT36a: `planner capability`: the census of what the feed offers (CensusOf): per side its quests, the offered ones by
// the works they hold and per level band; then the invariant and <LogsDir>/questcapability.txt, one line per quest and
// side not offered: quest, min level, side, first refused work, objective index
bool HandleCapabilityCommand(ChatHandler* handler, char const* /*args*/)
{
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    CapabilityCensus const c = CensusOf(*quests);
    for (std::uint8_t side = 0; side < SIDES; ++side)
    {
        handler->PSendSysMessage("CAPABILITY {}", SideLine(c, side));
        handler->PSendSysMessage("CAPABILITY {} offered quests by work: {}", SIDE_NAMES[side],
                                 WorkCounts([&](std::size_t w) { return c.offeredWith[side][w]; }, false));
        for (std::size_t band = 0; band < CAPABILITY_BANDS; ++band)
            if (c.total[side][band])
                handler->PSendSysMessage(
                    "CAPABILITY {} levels {}-{}: {} quests, {} offered, not offered: {}", SIDE_NAMES[side],
                    band * LEVEL_BAND_LEVELS, band * LEVEL_BAND_LEVELS + LEVEL_BAND_LEVELS - 1, c.total[side][band],
                    c.offered[side][band], WorkCounts([&](std::size_t w) { return c.refused[side][w][band]; }, true));
    }
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "questcapability.txt";
    uint32 lines = 0;
    if (std::ofstream file(path, std::ios::trunc); file)
        for (QuestRecord const& quest : quests->quests)
            for (std::uint8_t side = 0; side < SIDES; ++side)
            {
                QuestExec::Step const refusal = QuestExec::FirstRefusal(quest, side);
                if (!OfSide(quest, side) || refusal.act != QuestExec::Act::Refused)
                    continue;
                file << quest.id << '\t' << quest.minLevel << '\t' << SIDE_NAMES[side] << '\t'
                     << RefusedWord(refusal.work) << '\t' << refusal.objective << '\n';
                ++lines;
            }
    handler->PSendSysMessage("CAPABILITY offered and unsupported: {}; {} quests by side not offered, list {}",
                             c.offeredUnsupported, lines, path);
    return true;
}

// WU38: `planner classquests [<quest>]`: the class-quest table joined with the quest index: per class quest and race
// path its chain, each step with its level, whether its giver is indexed and the executor's refusal per side; the
// counts. Without an argument it also writes <LogsDir>/classquests.txt, one line per table row as the index holds it:
// quest, in index, class mask, race mask, min level, prev, prevAny (signed, comma list, - none), refused Alliance,
// refused Horde, giver indexed (the data of tests/class-quest-selftest.cpp's comparison)
bool HandleClassQuestsCommand(ChatHandler* handler, char const* args)
{
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    uint32 const only = args && *args ? static_cast<uint32>(std::strtoul(args, nullptr, 10)) : 0;
    ClassQuests::Rows const& rows = quests->classRows;
    ClassQuests::JoinCounts const counts = ClassQuests::CountsOf(rows);
    handler->PSendSysMessage("CLASSQUESTS {} class quests, {} steps, {} refused on a side, {} not in the index",
                             counts.classQuests, counts.steps, counts.refused, counts.missing);
    auto const list = [](auto const& values)
    {
        std::string out;
        for (auto const value : values)
            out += (out.empty() ? "" : ",") + std::to_string(value);
        return out.empty() ? std::string("-") : out;
    };
    for (ClassQuests::Row const& row : rows)
    {
        if (!ClassQuests::IsClassQuest(row) || (only && row.quest() != only))
            continue;
        handler->PSendSysMessage("CLASSQUEST {} {} learns {}{}", row.quest(), ClassQuests::CauseWord(row.own->cause),
                                 row.own->learns, row.inIndex ? "" : ": not in the index");
        std::map<std::string, std::string> paths;  // the chain's text -> its races
        for (std::uint32_t race = 1; race <= 11; ++race)
        {
            if (race == 9 || (row.raceMask && !((row.raceMask >> (race - 1)) & 1u)))
                continue;
            std::string chain;
            for (ClassQuests::Row const* step : ClassQuests::ChainOf(rows, row, race))
            {
                chain += Acore::StringFormat("{}{} L{} giver {}", chain.empty() ? "" : " > ", step->quest(),
                                             step->minLevel, step->giverIndexed ? "indexed" : "not indexed");
                if (step->refused[0] || step->refused[1])
                    chain +=
                        Acore::StringFormat(" refused {}{}", step->refused[0] ? "A" : "", step->refused[1] ? "H" : "");
                if (!step->active.empty())
                    chain += " needs active " + list(step->active);
                if (!step->inIndex)
                    chain += " (not in the index)";
            }
            std::string& races = paths[chain];
            races += (races.empty() ? "" : ",") + std::to_string(race);
        }
        for (auto const& [chain, races] : paths)
            handler->PSendSysMessage("CLASSQUEST {} races {}: {}", row.quest(), races, chain);
    }
    if (only)
        return true;
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "classquests.txt";
    if (std::ofstream file(path, std::ios::trunc); file)
    {
        for (ClassQuests::Row const& row : rows)
        {
            QuestRecord const* record = quests->Find(row.quest());
            file << row.quest() << '\t' << (record ? 1 : 0) << '\t' << (record ? record->classMask : 0) << '\t'
                 << (record ? record->raceMask : 0) << '\t' << (record ? record->minLevel : 0) << '\t'
                 << (record ? record->prev : 0) << '\t' << (record ? list(record->prevAny) : std::string("-")) << '\t'
                 << row.refused[0] << '\t' << row.refused[1] << '\t' << row.giverIndexed << '\n';
        }
        handler->PSendSysMessage("CLASSQUESTS {} rows written to {}", rows.size(), path);
    }
    return true;
}

bool HandleSourcesCommand(ChatHandler* handler, char const* args)
{
    uint32 item = 0;
    if (!(std::istringstream(args ? args : "") >> item) || !item)
    {
        handler->SendErrorMessage("Usage: playerbots planner sources <item id>");
        return false;
    }
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    std::vector<ItemSource> const sources = quests->SourcesOf(item);
    std::array<std::size_t, SOURCE_KINDS> byKind{};
    for (ItemSource const& source : sources)
        ++byKind[static_cast<std::size_t>(source.kind)];
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    handler->PSendSysMessage("SOURCES {} \"{}\": {} sources ({} corpse, {} skin, {} object)", item,
                             proto ? proto->Name1 : "?", sources.size(), byKind[0], byKind[1], byKind[2]);
    constexpr std::size_t SHOWN = 40;  // world drops have thousands
    for (std::size_t i = 0; i < std::min(SHOWN, sources.size()); ++i)
    {
        ItemSource const& source = sources[i];
        GameObjectTemplate const* object =
            source.kind == SourceKind::Object ? sObjectMgr->GetGameObjectTemplate(source.entry) : nullptr;
        CreatureTemplate const* creature =
            source.kind == SourceKind::Object ? nullptr : sObjectMgr->GetCreatureTemplate(source.entry);
        handler->PSendSysMessage("  {} {} \"{}\": {:.2f}%, count {}-{}, {} spawns{}", SourceKindName(source.kind),
                                 source.entry,
                                 object     ? object->name
                                 : creature ? creature->Name
                                            : "?",
                                 source.chance * 100.0f, source.minCount, source.maxCount, source.spawns,
                                 source.questOnly ? ", quest only" : "");
    }
    if (sources.size() > SHOWN)
        handler->PSendSysMessage("  ... {} more", sources.size() - SHOWN);
    return true;
}

// `playerbots planner places poionly [all]`: the objectives whose only place is a POI polygon (what a baked floor would
// serve, WT33a). Prints the count and a sample; the full list goes to <LogsDir>/poionly.txt, one row per objective
bool HandlePlacesCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string what;
    std::string how;
    in >> what >> how;
    if (what != "poionly")
    {
        handler->SendErrorMessage("Usage: playerbots planner places poionly [all]");
        return false;
    }
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    std::vector<std::string> rows;
    for (QuestRecord const& quest : quests->quests)
        for (std::size_t i = 0; i < quest.objectives.size(); ++i)
        {
            QuestObjective const& objective = quest.objectives[i];
            // The objectives rule 5 serves: a POI polygon, floored (poi) or not (none, WT33b)
            bool const poiOnly =
                objective.placeFrom == PlaceFrom::Poi || (objective.placeFrom == PlaceFrom::None && objective.polygon);
            if (objective.kind == ObjectiveKind::Turnin || !poiOnly)
                continue;
            uint32 points = 0;
            for (AreaPoint const& area : objective.areas)
                points += area.points;
            rows.push_back(Acore::StringFormat(
                "quest {} objective {} {} {} \"{}\" x{} poi_points {} areas {} map {} zone {}", quest.id, i,
                ObjectiveName(objective.kind), objective.target, TargetName(objective), objective.count, points,
                objective.areas.size(), objective.areas.empty() ? 0 : objective.areas.front().mapId, quest.zoneOrSort));
        }
    handler->PSendSysMessage("PLACES poionly: {} objectives have only a POI polygon", rows.size());
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "poionly.txt";
    if (std::ofstream file(path, std::ios::trunc); file)
    {
        for (std::string const& row : rows)
            file << row << '\n';
        handler->PSendSysMessage("  full list: {}", path);
    }
    std::size_t const shown = how == "all" ? rows.size() : std::min<std::size_t>(rows.size(), 15);
    for (std::size_t i = 0; i < shown; ++i)
        handler->PSendSysMessage("  {}", rows[i]);
    if (shown < rows.size())
        handler->PSendSysMessage("  ... {} more (`places poionly all` prints every row)", rows.size() - shown);
    return true;
}

// WT35g: `questuse facts` lists the four facts of every quest (QuestUseTable::FactsOf: summon, transform, made,
// credit) and each kill objective a use brings forth (QuestObjective::brings), one line each in
// <LogsDir>/questfacts.txt: quest, min level, race mask, objective, word, entry, via, detail
static bool QuestUseFacts(ChatHandler* handler, QuestIndex const& quests)
{
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "questfacts.txt";
    std::ofstream file(path, std::ios::trunc);
    std::array<uint32, static_cast<std::size_t>(FactKind::Count)> counts{};
    uint32 chains = 0;
    for (QuestRecord const& quest : quests.quests)
    {
        auto const line =
            [&](uint32 objective, std::string_view word, uint32 entry, uint32 via, std::string const& detail)
        {
            file << quest.id << '\t' << quest.minLevel << '\t' << quest.raceMask << '\t' << objective << '\t' << word
                 << '\t' << entry << '\t' << via << '\t' << detail << '\n';
        };
        for (UseFact const& fact : quests.uses.FactsOf(quest.id))
        {
            ++counts[static_cast<std::size_t>(fact.kind)];
            std::string detail = fact.viaObject ? "object" : (fact.via ? "item" : "kill");
            for (auto const& [item, count] : fact.reagents)
                detail += Acore::StringFormat(" {}x{}", item, count);
            line(fact.objective, FACT_KIND_NAMES[static_cast<std::size_t>(fact.kind)], fact.entry, fact.via, detail);
        }
        for (std::size_t i = 0; i < quest.objectives.size(); ++i)
            if (quest.objectives[i].brings != UseBrings::Credit)
            {
                ++chains;
                QuestObjective const& objective = quest.objectives[i];
                line(static_cast<uint32>(i), objective.brings == UseBrings::Summon ? "chain:summon" : "chain:transform",
                     objective.target, 0, Acore::StringFormat("{} areas", objective.areas.size()));
            }
    }
    handler->PSendSysMessage(
        "QUESTUSE facts: {} summon, {} transform, {} made, {} credit; {} kill objectives a use brings forth; list {}",
        counts[0], counts[1], counts[2], counts[3], chains, path);
    return true;
}

// WT35b: `questuse all` lists every quest the executor can use an item or a spell for: an objective (or the quest's
// event) with a use rule and a place, one id per line in <LogsDir>/questuse.txt (the failure record's cleanup reads it)
static bool QuestUseAll(ChatHandler* handler, QuestIndex const& quests)
{
    std::vector<uint32> ids;
    for (QuestRecord const& quest : quests.quests)
    {
        auto const placed = [](QuestObjective const& objective)
        { return objective.use != UseHow::None && !objective.areas.empty(); };
        if (placed(quest.eventUse) || std::any_of(quest.objectives.begin(), quest.objectives.end(), placed))
            ids.push_back(quest.id);
    }
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "questuse.txt";
    if (std::ofstream file(path, std::ios::trunc); file)
        for (uint32 const id : ids)
            file << id << '\n';
    handler->PSendSysMessage("QUESTUSE all: {} quests have a use rule and a place; list {}", ids.size(), path);
    return true;
}

bool HandleQuestUseCommand(ChatHandler* handler, char const* args)
{
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (args && std::string_view(args) == "all")
    {
        if (!quests)
        {
            handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
            return false;
        }
        return QuestUseAll(handler, *quests);
    }
    if (args && std::string_view(args) == "facts")
    {
        if (!quests)
        {
            handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
            return false;
        }
        return QuestUseFacts(handler, *quests);
    }
    uint32 id = 0;
    if (!(std::istringstream(args ? args : "") >> id) || !id)
    {
        handler->SendErrorMessage("Usage: playerbots planner questuse <quest id>|all|facts");
        return false;
    }
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
    auto const [first, last] = quests->uses.RulesFor(id);
    std::vector<UseItem const*> const items = quests->uses.ItemsOf(id);
    Quest const* proto = sObjectMgr->GetQuestTemplate(id);
    handler->PSendSysMessage("QUESTUSE {} \"{}\": {} rules, {} source items with an on-use spell", id,
                             proto ? proto->GetTitle() : "?", std::distance(first, last), items.size());
    for (auto it = first; it != last; ++it)
    {
        QuestUseRule const& rule = *it;
        bool const objects = rule.kind == UseKind::Object || rule.kind == UseKind::Focus;
        std::string targets;
        for (uint32 const entry : rule.entries)
        {
            GameObjectTemplate const* object = objects ? sObjectMgr->GetGameObjectTemplate(entry) : nullptr;
            CreatureTemplate const* creature = objects ? nullptr : sObjectMgr->GetCreatureTemplate(entry);
            targets += Acore::StringFormat("{}{} \"{}\"", targets.empty() ? "" : ", ", entry,
                                           object     ? object->name
                                           : creature ? creature->Name
                                                      : "?");
        }
        std::string const what = rule.spellId == 0 ? std::string("use the object")
                                 : rule.itemId     ? Acore::StringFormat("item {} spell {}", rule.itemId, rule.spellId)
                                                   : Acore::StringFormat("class spell {}", rule.spellId);
        std::string const goal =
            rule.objective < quest->objectives.size()
                ? Acore::StringFormat("{} {}", ObjectiveName(quest->objectives[rule.objective].kind),
                                      quest->objectives[rule.objective].target)
                : std::string("completes the quest");
        handler->PSendSysMessage("  {}: {} on {}; range {:.0f} yd{}{}; -> {}", UseKindName(rule.kind), what, targets,
                                 rule.rangeYards, rule.mustBeDead ? ", target lies dead" : "",
                                 rule.standAtFocus ? Acore::StringFormat(", stand at focus {}", rule.focusId) : "",
                                 goal);
    }
    for (UseItem const* item : items)
    {
        if (std::any_of(first, last, [item](QuestUseRule const& rule)
                        { return rule.itemId == item->itemId && rule.spellId == item->spellId; }))
            continue;
        UseSpell const* spell = quests->uses.Spell(item->spellId);
        handler->PSendSysMessage("  no rule: item {} spell {} names no target (targets {:#x}, focus {})", item->itemId,
                                 item->spellId, spell ? spell->targets : 0, spell ? spell->focusId : 0);
    }
    return true;
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
        if (!QuestStatsLoaded())
        {
            handler->SendErrorMessage("PLANNER: the quest statistics are not loaded");
            return false;
        }
        uint32 const rows = QuestStatsFlushNow();
        handler->PSendSysMessage("QUEST stats flush: {} rows written (asynchronous)", rows);
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

// The automatic class-quest completion (upkeep WU21a, ClassQuestCompletion.h)
namespace ClassQuestCompletion
{
namespace
{
std::mutex waitLock;
std::unordered_map<uint32, ClassQuests::When> waits;  // guid counter -> the widest When asked (Creation first)
std::atomic<uint32> waiting{0};

bool Ready() { return QuestsReady() && UpkeepIndexReady(); }

void Wait(Player* bot, ClassQuests::When when)
{
    std::lock_guard<std::mutex> const guard(waitLock);
    auto const [it, added] = waits.emplace(bot->GetGUID().GetCounter(), when);
    if (added)
        waiting.fetch_add(1, std::memory_order_release);
    else if (when < it->second)
        it->second = when;
    LOG_DEBUG("playerbots", "[ClassQuest] {} waits for the quest index (when {})", bot->GetName(),
              static_cast<uint32>(it->second));
}

ClassQuests::BotFacts FactsOf(Player* bot, bool governed, ClassQuests::When when)
{
    ClassQuests::BotFacts facts;
    facts.cls = bot->getClass();
    facts.race = bot->getRace();
    facts.level = bot->GetLevel();
    facts.governed = governed;
    for (uint32 const quest : bot->getRewardedQuests())
        facts.rewarded.push_back(quest);
    std::sort(facts.rewarded.begin(), facts.rewarded.end());
    if (governed)
        facts.givenUp = QuestFailRecord::GivenUpQuests(bot->GetGUID().GetCounter(), ClassQuests::GIVE_UP_LIMIT);
    // A new character did its lower class quests: a spell the factory taught beside them (InitSpecialSpells' Death
    // Gate) settles none of them
    if (when != ClassQuests::When::Creation)
        facts.knownSpells = KnownSpells(bot);
    return facts;
}

// One due quest: the chain's steps leave the log and are rewarded with the class quest, its reward spell is cast; no
// XP, gold or items (Player::RewardQuest is not called)
bool CompleteOne(Player* bot, ClassQuests::Rows const& rows, ClassQuests::BotFacts const& facts,
                 ClassQuests::Due const& due, bool creation, char const* source)
{
    ClassQuests::Row const* const row = ClassQuests::Find(rows, due.quest);
    if (!row || !ClassQuests::IsClassQuest(*row) || bot->IsQuestRewarded(due.quest) ||
        (!creation && due.spell && bot->HasSpell(due.spell)))
        return false;
    std::vector<uint32> const steps = ClassQuests::CompletionSteps(rows, *row, facts);
    for (uint32 const step : steps)
    {
        uint16 const slot = bot->FindQuestSlot(step);
        if (slot < MAX_QUEST_LOG_SIZE)
        {
            bot->SetQuestSlot(slot, 0);
            bot->TakeQuestSourceItem(step, false);
        }
        if (bot->GetQuestStatus(step) != QUEST_STATUS_NONE)
            bot->RemoveActiveQuest(step, false);
        bot->SetRewardedQuest(step);
    }
    if (Quest const* quest = sObjectMgr->GetQuestTemplate(due.quest))
        bot->learnQuestRewardedSpells(quest);
    // The reward spell's cast teaches its spells; a cast that does not (its result is logged) leaves the quest's
    // defined effect to direct learns: every spell the reward spell teaches (Path of Defense: three), then the due one
    int32 cast = -1;  // not cast
    if (due.spell && !bot->HasSpell(due.spell) && row->own->rewardSpell)
        cast = static_cast<int32>(bot->CastSpell(bot, row->own->rewardSpell, true));
    bool const learned = due.spell && !bot->HasSpell(due.spell);
    if (learned)
    {
        if (SpellInfo const* const reward = sSpellMgr->GetSpellInfo(row->own->rewardSpell))
        {
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                uint32 const taught = reward->Effects[i].TriggerSpell;
                if (reward->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL && taught && !bot->HasSpell(taught))
                    bot->learnSpell(taught);
            }
        }
        if (!bot->HasSpell(due.spell))
            bot->learnSpell(due.spell);
    }
    Upkeep::Note(bot, UpkeepKind::ClassQuest, source, due.quest);
    LOG_INFO("playerbots",
             "[ClassQuest] {} completes class quest {} ({}): {} quests rewarded, spell {} {} (cast {} result {}, "
             "alive {}, in world {}, learned directly {})",
             bot->GetName(), due.quest, source, steps.size(), due.spell,
             due.spell && bot->HasSpell(due.spell) ? "known" : "not known", row->own->rewardSpell, cast, bot->IsAlive(),
             bot->IsInWorld(), learned);
    return true;
}

void Complete(Player* bot, std::vector<ClassQuests::Due> const& due, ClassQuests::Rows const& rows,
              ClassQuests::BotFacts const& facts, ClassQuests::When when)
{
    bool const creation = when == ClassQuests::When::Creation;
    for (ClassQuests::Due const& one : due)
        CompleteOne(bot, rows, facts, one, creation,
                    one.failed ? Upkeep::SOURCE_CLASS_QUEST_FAILED : (creation ? SOURCE_CREATION : SOURCE_LEVELUP));
}

void Run(Player* bot, ClassQuests::When when)
{
    std::shared_ptr<QuestIndex const> const quests = Quests();
    std::shared_ptr<UpkeepIndex const> const upkeep = CurrentUpkeepIndex();
    bool const governed = Governed(bot);
    if (!quests || !upkeep || (when == ClassQuests::When::Check && !governed))
        return;
    ClassQuests::BotFacts const facts = FactsOf(bot, governed, when);
    std::vector<uint32> const taught = ClassTrainerTaught(*upkeep, bot->getClass());
    auto const teaches = [&taught](uint32 spell) { return std::binary_search(taught.begin(), taught.end(), spell); };
    // The map thread asks no way: a failed way is the planner's verdict (CompleteFailed)
    auto const route = [](uint32) { return ClassQuests::Route::Unknown; };
    Complete(bot, ClassQuests::DueQuests(quests->classRows, facts, when, teaches, route), quests->classRows, facts,
             when);
}
}  // namespace

bool Governed(Player* bot) { return BotKind::KeepsPossessions(bot); }

void Request(Player* bot, ClassQuests::When when)
{
    if (!bot || (when == ClassQuests::When::Check && !Governed(bot)))
        return;
    if (!Ready())
        Wait(bot, when);
    else
        Run(bot, when);
}

void CompleteFailed(Player* bot, std::vector<ClassQuests::Due> const& due)
{
    std::shared_ptr<QuestIndex const> const quests = Ready() ? Quests() : nullptr;
    if (!bot || due.empty() || !quests || !Governed(bot))
        return;
    std::vector<ClassQuests::Due> failed;
    for (ClassQuests::Due const& one : due)
        if (one.failed)
            failed.push_back(one);
    Complete(bot, failed, quests->classRows, FactsOf(bot, true, ClassQuests::When::Check), ClassQuests::When::Check);
}

void Drain(Player* bot)
{
    if (!waiting.load(std::memory_order_acquire) || !Ready())
        return;
    ClassQuests::When when;
    {
        std::lock_guard<std::mutex> const guard(waitLock);
        auto const it = waits.find(bot->GetGUID().GetCounter());
        if (it == waits.end())
            return;
        when = it->second;
        waits.erase(it);
        waiting.fetch_sub(1, std::memory_order_release);
    }
    Run(bot, when);
}

void Forget(Player* bot)
{
    if (!bot || !waiting.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> const guard(waitLock);
    if (waits.erase(bot->GetGUID().GetCounter()))
        waiting.fetch_sub(1, std::memory_order_release);
}

std::vector<uint32> KnownSpells(Player* bot)
{
    std::vector<uint32> known;
    for (ClassQuests::Entry const& entry : ClassQuests::TABLE)
    {
        for (uint32 const spell : {entry.learns, entry.rewardSpell})
            if (spell && bot->HasSpell(spell))
                known.push_back(spell);
    }
    std::sort(known.begin(), known.end());
    known.erase(std::unique(known.begin(), known.end()), known.end());
    return known;
}
}  // namespace ClassQuestCompletion

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
    Goals::Situation situation = GoalHarness::Base(bot.level);
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
    // Read only, on the world thread: the circuit and the Situation inputs the world gives now; a bot's are the live
    // job's (vendor, repairer, bag offers: LiveSituation), a player's only health and mana. A caller offers a hearth
    // option only when WorldTravel::InnCheckDue says so; no inns are given here, so none is offered
    CircuitData const data = CircuitDataNow();
    Snapshot const snapshot = FillSnapshot(player, 0);
    Goals::Situation situation;
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        situation = LiveSituation(ai, player);
    else
    {
        situation.here.hub = NO_HUB;  // as the live job: found from the circuit
        situation.healthPercent = player->GetHealthPct();
        uint32 const maxMana = player->GetMaxPower(POWER_MANA);
        situation.manaPercent =
            maxMana > 0 ? 100.0f * static_cast<float>(player->GetPower(POWER_MANA)) / static_cast<float>(maxMana)
                        : -1.0f;
    }
    // One line naming the bot (a scenario's log keeps only those): the errand inputs the model weighs
    handler->PSendSysMessage(
        "GOALS {} inputs: vendor {} {:.1f}, repairer {} {:.1f}, sellable {}, offers {}, durability {}, free {}/{}, "
        "inns {} first {}, hearthstone {}",
        player->GetName(), situation.bags.vendor.id, situation.bags.vendor.walkMinutes, situation.bags.repairer.id,
        situation.bags.repairer.walkMinutes, situation.bags.sellableSlots, situation.bagOffers.size(),
        snapshot.durabilityPercent, snapshot.bagFree, snapshot.bagSlots, situation.inns.size(),
        situation.inns.empty() ? 0 : situation.inns.front().id, snapshot.hasHearthstone);
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
        handler->PSendSysMessage("  {} {} {:.1f}/min over {:.1f} min: {}", player->GetName(), Goals::ChoiceWord(option),
                                 option.valuePerMinute, option.minutes, option.reason);
    return true;
}

// WQ13: `fields status`: the field map's counters
bool HandleFieldsStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    std::shared_ptr<FieldMap const> const fields = FieldsReady() ? Fields() : nullptr;
    if (!fields)
    {
        handler->SendErrorMessage("PLANNER: the field map is not ready yet (built at startup beside the quest index)");
        return false;
    }
    FieldMapStats const& s = fields->Stats();
    std::size_t const bytes = fields->ApproxBytes();
    handler->PSendSysMessage(
        "FIELDS ready: {} rows ({} creature, {} object), {} cells of {} yd, {} spawns ({} hostile), {} bytes (about "
        "{:.1f} MB), gathered in {} ms with the quest index, built in {} ms",
        s.rows, s.creatureRows, s.objectRows, s.cells, FIELD_CELL_YARDS, s.spawns, s.hostiles, bytes,
        static_cast<double>(bytes) / (1024.0 * 1024.0), s.gatherMs, s.buildMs);
    return true;
}

namespace
{
constexpr float ZONE_BOX_PERCENT = 100.0f;
constexpr float ZONE_BOX_MARGIN = 2.0f;  // percent of the zone's world map box around it

// WQ13: false when the point lies outside the zone's world map box (WorldMapArea.dbc); true for a zone without one,
// whose coordinates Map2ZoneCoordinates returns unchanged
bool InZoneBox(uint32 zoneId, float x, float y)
{
    float zoneX = x;
    float zoneY = y;
    Map2ZoneCoordinates(zoneX, zoneY, zoneId);
    if (zoneX == x && zoneY == y)
        return true;
    return zoneX >= -ZONE_BOX_MARGIN && zoneX <= ZONE_BOX_PERCENT + ZONE_BOX_MARGIN && zoneY >= -ZONE_BOX_MARGIN &&
           zoneY <= ZONE_BOX_PERCENT + ZONE_BOX_MARGIN;
}

struct FieldPointArea
{
    uint32 zone{0};
    uint32 area{0};
};
}  // namespace

// WQ13: `fields dump <map> <zone>`: a cell is the zone's when the point of one of its rows lies in the zone (resolved
// here on the world thread, only for points inside the zone's box); every row of those cells goes to the TSV
bool HandleFieldsDumpCommand(ChatHandler* handler, char const* args)
{
    std::shared_ptr<FieldMap const> const fields = FieldsReady() ? Fields() : nullptr;
    if (!fields)
    {
        handler->SendErrorMessage("PLANNER: the field map is not ready yet (built at startup beside the quest index)");
        return false;
    }
    uint32 mapId = 0;
    uint32 zoneId = 0;
    if (!(std::istringstream(args ? args : "") >> mapId >> zoneId) || !zoneId)
    {
        handler->SendErrorMessage("Usage: playerbots fields dump <map> <zone>");
        return false;
    }
    auto const start = std::chrono::steady_clock::now();
    std::vector<FieldRow> const& rows = fields->Rows();
    std::unordered_map<uint32, FieldPointArea> areas;  // by row index, resolved once
    auto const areaOf = [&areas, &rows, mapId](uint32 at) -> FieldPointArea const&
    {
        auto const [it, added] = areas.try_emplace(at);
        if (added)
            ResolveArea(mapId, rows[at].x, rows[at].y, rows[at].z, it->second.zone, it->second.area);
        return it->second;
    };
    std::vector<FieldCell const*> cells;
    for (FieldCell const& cell : fields->Cells())
        if (FieldMapOf(cell.key) == mapId)
            for (uint32 at = cell.firstRow; at < cell.firstRow + cell.rowCount; ++at)
                if (InZoneBox(zoneId, rows[at].x, rows[at].y) && areaOf(at).zone == zoneId)
                {
                    cells.push_back(&cell);
                    break;
                }

    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + Acore::StringFormat("fields-{}-{}.tsv", mapId, zoneId);
    std::ofstream file(path, std::ios::trunc);
    if (!file)
    {
        handler->SendErrorMessage("FIELDS: cannot write {}", path);
        return false;
    }
    file << "map\tcellX\tcellY\tkind\tentry\tname\ttype\tnpcflag\tspawns\tweight\tminLevel\tmaxLevel\telites\t"
            "medianRespawn\tx\ty\tz\tzone\tarea\tareaName\tcellHostiles\tcellHostileWeight\tcellHostileMinLevel\t"
            "cellHostileMaxLevel\tcellHostileElites\n";
    uint32 creatureRows = 0;
    uint32 objectRows = 0;
    uint32 spawns = 0;
    for (FieldCell const* cell : cells)
        for (uint32 at = cell->firstRow; at < cell->firstRow + cell->rowCount; ++at)
        {
            FieldRow const& row = rows[at];
            FieldPointArea const& where = areaOf(at);
            std::string name;
            uint32 type = 0;
            uint32 npcFlags = 0;
            if (row.kind == FieldKind::Creature)
            {
                ++creatureRows;
                if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(row.entry))
                {
                    name = creature->Name;
                    type = creature->type;
                    npcFlags = creature->npcflag;
                }
            }
            else
            {
                ++objectRows;
                if (GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(row.entry))
                {
                    name = object->name;
                    type = object->type;
                }
            }
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(where.area);
            file << Acore::StringFormat(
                "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:.3f}\t{}\t{}\t{}\t{}\t{:.1f}\t{:.1f}\t{:.1f}\t{}\t{}\t{}\t{}\t{:"
                ".3f}\t{}\t{}\t{}\n",
                mapId, FieldCellXOf(row.cell), FieldCellYOf(row.cell), row.kind == FieldKind::Creature ? "c" : "o",
                row.entry, name, type, npcFlags, row.spawns, row.weight, row.minLevel, row.maxLevel, row.elites,
                row.medianRespawn, row.x, row.y, row.z, where.zone, where.area, area ? area->area_name[0] : "",
                cell->hostiles, cell->hostileWeight, cell->hostileMinLevel, cell->hostileMaxLevel, cell->hostileElites);
            spawns += row.spawns;
        }
    handler->PSendSysMessage(
        "FIELDS dump map {} zone {}: {} cells, {} rows ({} creature, {} object), {} spawns; {} "
        "points resolved in {} ms; file {}",
        mapId, zoneId, cells.size(), creatureRows + objectRows, creatureRows, objectRows, spawns, areas.size(),
        MsSince(start), path);
    return true;
}

namespace
{
// WQ14: the census's causes: the field map's (QuestFields::NoField) and `instance`, a no_spawn whose own places (the
// objective's areas, the enders) lie on a map the field map does not hold (a dungeon's)
constexpr std::size_t CENSUS_INSTANCE = QuestFields::NO_FIELDS;
constexpr std::size_t CENSUS_CAUSES = QuestFields::NO_FIELDS + 1;
constexpr std::size_t CENSUS_ENTRIES = 4;  // entries a list line names

char const* CensusCauseName(std::size_t cause)
{
    return cause == CENSUS_INSTANCE ? "instance" : QuestFields::NO_FIELD_NAMES[cause];
}

struct FieldCensus
{
    std::array<uint32, SIDES> offered{};
    std::array<uint32, SIDES> withGap{};        // a need that needs a field and has none on the quest's maps
    std::array<uint32, SIDES> withFieldless{};  // a need that needs none (explore, talk) and has no place
    std::array<uint32, SIDES> atPlace{};        // WQ17b: needs worked at the index's places (QuestFields::UsesPlaces)
    std::array<std::array<uint32, QuestFields::NEED_KINDS>, SIDES> needs{};
    std::array<std::array<uint32, QuestFields::NEED_KINDS>, SIDES> without{};
    std::array<std::array<uint32, CENSUS_CAUSES>, SIDES> causeNeeds{};
    std::array<std::array<uint32, CENSUS_CAUSES>, SIDES> causeQuests{};
};

// The maps a quest is worked on: its givers' and enders'
std::vector<uint32> CensusMaps(QuestIndex const& quests, QuestRecord const& quest)
{
    std::vector<uint32> maps;
    for (std::vector<uint32> const* list : {&quest.givers, &quest.enders})
        for (uint32 const at : *list)
            if (at < quests.places.size())
                maps.push_back(quests.places[at].mapId);
    std::sort(maps.begin(), maps.end());
    maps.erase(std::unique(maps.begin(), maps.end()), maps.end());
    return maps;
}

bool OffFieldMaps(QuestIndex const& quests, QuestRecord const& quest, QuestFields::Need const& need,
                  std::vector<uint32> const& fieldMaps)
{
    auto const off = [&fieldMaps](uint32 mapId)
    { return !std::binary_search(fieldMaps.begin(), fieldMaps.end(), mapId); };
    if (need.objective == QuestFields::HAND_IN)
        return std::any_of(quest.enders.begin(), quest.enders.end(),
                           [&](uint32 at) { return at < quests.places.size() && off(quests.places[at].mapId); });
    QuestObjective const& objective = QuestExec::ObjectiveAt(quest, need.objective);
    return std::any_of(objective.areas.begin(), objective.areas.end(),
                       [&off](AreaPoint const& area) { return off(area.mapId); });
}

std::string CensusEntries(QuestFields::Need const& need)
{
    std::string text;
    for (std::size_t i = 0; i < need.sources.size() && i < CENSUS_ENTRIES; ++i)
        text += Acore::StringFormat("{}{}{}", i ? "," : "", need.sources[i].kind == FieldKind::Object ? "o" : "c",
                                    need.sources[i].entry);
    if (need.sources.size() > CENSUS_ENTRIES)
        text += Acore::StringFormat(",+{}", need.sources.size() - CENSUS_ENTRIES);
    return text.empty() ? std::string("-") : text;
}

std::string CensusObjective(QuestFields::Need const& need)
{
    return need.objective == QuestFields::HAND_IN ? std::string("handin")
           : need.objective == QuestFields::TAKE  ? std::string("take")
                                                  : std::to_string(need.objective);
}

// WQ17b: a quest's needs as the census counts them: every objective open, the hand-in when ready, the take at its
// givers (none for a quest only an item starts)
std::vector<QuestFields::Need> CensusNeeds(QuestIndex const& quests, QuestRecord const& quest, std::uint8_t side)
{
    QuestFields::Pricing const pricing{};  // neutral weights: the census counts needs and reads no worth
    std::array<QuestSlot, 2> const log = {QuestSlot{quest.id, 0, {}}, QuestSlot{quest.id, Circuit::SLOT_COMPLETE, {}}};
    std::vector<QuestFields::Need> needs = QuestFields::Needs(quests, log, side, pricing);
    if (std::optional<QuestFields::Need> take = QuestFields::TakeNeed(quests, quest, side, pricing))
        needs.push_back(std::move(*take));
    return needs;
}

// The census's cause of a gap: the field map's, or `instance`
std::size_t CensusCause(QuestIndex const& quests, QuestRecord const& quest, QuestFields::Need const& need,
                        QuestFields::NoField gap, std::vector<uint32> const& fieldMaps)
{
    return gap == QuestFields::NoField::NoSpawn && OffFieldMaps(quests, quest, need, fieldMaps)
               ? CENSUS_INSTANCE
               : static_cast<std::size_t>(gap);
}

// WQ17b: the class quests of the questing plan's WQ29 list, WQ30's paladin quests and two warrior quests: what each
// need has on the quest's maps (a field, a place or the gap by name), a first reading for WQ30
constexpr std::array<uint32, 26> CENSUS_CLASS_QUESTS = {1527, 1678, 1683, 1689, 1739, 1785, 1788, 1795, 6081,
                                                        6082, 6085, 6086, 6088, 6089, 6102, 6103, 6125, 6130,
                                                        7603, 9485, 9582, 9593, 9600, 9619, 9673, 9675};
}  // namespace

// WQ14: `fields census`: every quest the capability table offers a side, its needs as taken (every objective open)
// and when ready (the hand-in), against the field map: per side the quests, those with a need without a field on
// their maps (givers' and enders'), the needs by kind and by cause; <LogsDir>/fieldcensus.txt lists each such need
bool HandleFieldsCensusCommand(ChatHandler* handler, char const* /*args*/)
{
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    std::shared_ptr<FieldMap const> const fields = FieldsReady() ? Fields() : nullptr;
    if (!quests || !fields)
    {
        handler->SendErrorMessage(
            "PLANNER: the quest index or the field map is not ready yet (see `playerbots fields status`)");
        return false;
    }
    auto const start = std::chrono::steady_clock::now();
    std::vector<uint32> fieldMaps;  // the cells are sorted by key, the map its top bits
    for (FieldCell const& cell : fields->Cells())
        if (fieldMaps.empty() || fieldMaps.back() != FieldMapOf(cell.key))
            fieldMaps.push_back(FieldMapOf(cell.key));
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "fieldcensus.txt";
    std::ofstream file(path, std::ios::trunc);
    if (!file)
    {
        handler->SendErrorMessage("FIELDS: cannot write {}", path);
        return false;
    }
    file << "quest\tside\tminLevel\tobjective\tkind\tcause\tentries\n";
    std::size_t const fieldless = static_cast<std::size_t>(QuestFields::NoField::Fieldless);
    FieldCensus c;
    uint32 lines = 0;
    for (QuestRecord const& quest : quests->quests)
        for (std::uint8_t side = 0; side < SIDES; ++side)
        {
            if (!OfSide(quest, side) || QuestExec::FirstRefusal(quest, side).act == QuestExec::Act::Refused)
                continue;
            ++c.offered[side];
            std::vector<uint32> const maps = CensusMaps(*quests, quest);
            std::array<bool, CENSUS_CAUSES> causes{};
            for (QuestFields::Need const& need : CensusNeeds(*quests, quest, side))
            {
                std::size_t const kind = static_cast<std::size_t>(need.kind);
                ++c.needs[side][kind];
                std::optional<QuestFields::NoField> const gap = QuestFields::MapGap(*fields, need, maps);
                if (!gap)
                {
                    c.atPlace[side] += QuestFields::UsesPlaces(*fields, need) ? 1 : 0;
                    continue;
                }
                std::size_t const cause = CensusCause(*quests, quest, need, *gap, fieldMaps);
                ++c.without[side][kind];
                ++c.causeNeeds[side][cause];
                causes[cause] = true;
                file << quest.id << '\t' << SIDE_NAMES[side] << '\t' << quest.minLevel << '\t' << CensusObjective(need)
                     << '\t' << QuestFields::NeedKindName(need.kind) << '\t' << CensusCauseName(cause) << '\t'
                     << CensusEntries(need) << '\n';
                ++lines;
            }
            bool gapped = false;
            for (std::size_t k = 0; k < CENSUS_CAUSES; ++k)
            {
                c.causeQuests[side][k] += causes[k] ? 1 : 0;
                gapped = gapped || (causes[k] && k != fieldless);
            }
            c.withGap[side] += gapped ? 1 : 0;
            c.withFieldless[side] += causes[fieldless] ? 1 : 0;
        }
    for (std::uint8_t side = 0; side < SIDES; ++side)
    {
        handler->PSendSysMessage(
            "FIELDS CENSUS {}: {} offered quests, {} with a need without a field or place on their maps, {} with a "
            "need that needs no field (explore, talk) and has no place; {} needs worked at a place",
            SIDE_NAMES[side], c.offered[side], c.withGap[side], c.withFieldless[side], c.atPlace[side]);
        std::string kinds;
        for (std::size_t k = 0; k < QuestFields::NEED_KINDS; ++k)
            if (c.needs[side][k])
                kinds += Acore::StringFormat("{}{} {} ({} without a field)", kinds.empty() ? "" : ", ",
                                             QuestFields::NEED_KIND_NAMES[k], c.needs[side][k], c.without[side][k]);
        std::string causeText;
        for (std::size_t k = 0; k < CENSUS_CAUSES; ++k)
            if (c.causeNeeds[side][k])
                causeText += Acore::StringFormat("{}{} {} needs in {} quests", causeText.empty() ? "" : ", ",
                                                 CensusCauseName(k), c.causeNeeds[side][k], c.causeQuests[side][k]);
        handler->PSendSysMessage("FIELDS CENSUS {} needs by kind: {}", SIDE_NAMES[side],
                                 kinds.empty() ? "none" : kinds);
        handler->PSendSysMessage("FIELDS CENSUS {} without a field by cause: {}", SIDE_NAMES[side],
                                 causeText.empty() ? "none" : causeText);
    }
    for (uint32 const id : CENSUS_CLASS_QUESTS)
    {
        QuestRecord const* quest = quests->Find(id);
        bool offered = false;
        for (std::uint8_t side = 0; quest && side < SIDES; ++side)
        {
            if (!OfSide(*quest, side) || QuestExec::FirstRefusal(*quest, side).act == QuestExec::Act::Refused)
                continue;
            offered = true;
            std::vector<uint32> const maps = CensusMaps(*quests, *quest);
            std::string text;
            for (QuestFields::Need const& need : CensusNeeds(*quests, *quest, side))
            {
                std::optional<QuestFields::NoField> const gap = QuestFields::MapGap(*fields, need, maps);
                std::string const has = !gap ? (QuestFields::UsesPlaces(*fields, need) ? "place" : "field")
                                             : CensusCauseName(CensusCause(*quests, *quest, need, *gap, fieldMaps));
                text += Acore::StringFormat("{}{} {} {}", text.empty() ? "" : ", ", CensusObjective(need),
                                            QuestFields::NeedKindName(need.kind), has);
            }
            handler->PSendSysMessage("FIELDS CENSUS class quest {} {} (minLevel {}): {}", id, SIDE_NAMES[side],
                                     quest->minLevel, text.empty() ? "no need" : text);
        }
        if (!offered)
            handler->PSendSysMessage("FIELDS CENSUS class quest {}: {}", id,
                                     quest ? "not offered (the capability table refuses it)" : "not in the index");
    }
    handler->PSendSysMessage("FIELDS CENSUS list: {} needs without a field in {} ({} ms)", lines, path, MsSince(start));
    return true;
}
}  // namespace Planner
