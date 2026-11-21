/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTINDEX_H
#define _PLAYERBOT_QUESTINDEX_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "PlannerIndexes.h"
#include "QuestSources.h"
#include "QuestUseRules.h"

// The planner's quest index (travel plan WT13a, PLAN 3.9): every quest with its givers and enders, limits, objectives
// with their areas and chain links, built once on a worker from a copy the world thread gathered. No game header:
// tests/quest-index-selftest.cpp builds it from synthetic input.
// Hubs (WT12b review note b): quests use camps, their givers and enders clustered at CAMP_CELL_YARDS. The services
// hubs stay as WT12b built them; CampHub() joins a camp to a hub only inside the hub's core, so a wide city hub does
// not take the camps around it
namespace Planner
{
// quest_template.QuestInfoID values used here; PlannerQuests.cpp checks them against the core's QuestTypes
namespace QuestType
{
constexpr std::uint32_t ELITE = 1;
constexpr std::uint32_t PVP = 41;
constexpr std::uint32_t RAID = 62;
constexpr std::uint32_t DUNGEON = 81;
constexpr std::uint32_t HEROIC = 85;
constexpr std::uint32_t RAID_10 = 88;
constexpr std::uint32_t RAID_25 = 89;
}  // namespace QuestType

namespace QuestFlag
{
constexpr std::uint16_t ELITE = 0x0001;
constexpr std::uint16_t GROUP = 0x0002;  // more than one suggested player
constexpr std::uint16_t DUNGEON = 0x0004;
constexpr std::uint16_t RAID = 0x0008;
constexpr std::uint16_t PVP = 0x0010;
constexpr std::uint16_t DAILY = 0x0020;
constexpr std::uint16_t WEEKLY = 0x0040;
constexpr std::uint16_t REPEATABLE = 0x0080;
constexpr std::uint16_t SEASONAL = 0x0100;
}  // namespace QuestFlag

enum class ObjectiveKind : std::uint8_t
{
    Kill,
    Item,
    Object,
    Explore,  // an area trigger
    Turnin,   // the turn-in POI
    Count
};
constexpr std::size_t OBJECTIVE_KINDS = static_cast<std::size_t>(ObjectiveKind::Count);
constexpr std::array<char const*, OBJECTIVE_KINDS> OBJECTIVE_NAMES = {"kill", "item", "object", "explore", "turnin"};

inline char const* ObjectiveName(ObjectiveKind kind) { return OBJECTIVE_NAMES[static_cast<std::size_t>(kind)]; }

constexpr std::uint32_t NO_CAMP = 0xFFFFFFFFu;
constexpr float CAMP_CELL_YARDS = 25.0f;        // givers and enders 25 to 70 yd apart share a camp
constexpr float HUB_CORE_YARDS = 150.0f;        // a camp joins a hub within its radius and at most this from its centre
constexpr std::size_t CAMP_HUB_CANDIDATES = 3;  // hubs tried by CampHub, nearest first
constexpr float AREA_CELL_YARDS = 200.0f;       // a target's spawns 200 to 560 yd apart share an area
constexpr std::size_t MAX_OBJECTIVE_AREAS = 4;  // an objective keeps its largest areas
constexpr float GIVER_CELL_YARDS = 200.0f;      // the GiversNear grid
constexpr std::int32_t POI_TURNIN = -1;         // quest_poi.ObjectiveIndex of the turn-in place
constexpr std::int32_t POI_FIRST_ITEM = 4;      // item objectives follow the 4 creature or object slots
constexpr std::int32_t POI_NONE = -2;           // an objective without a POI slot (area triggers)
constexpr float NO_FLOOR = std::numeric_limits<float>::quiet_NaN();  // a POI point without a baked floor (WT33b)

// A spawn of a quest giver or ender
struct QuestPlace
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::uint32_t entry{0};  // creature or gameobject template
    bool isObject{false};
    std::uint32_t zoneId{0};  // 0 = unresolved at gather time (a map not loaded then, e.g. an instance)
    std::uint32_t areaId{0};
    std::uint32_t camp{NO_CAMP};
    std::uint8_t hostileTo{0};  // WT35h: 1 << team for each side the creature is hostile to (no hand-in for it)
    bool otherPhase{false};     // WT35h: spawned outside the default phase (no ender for a bot in it)
};

// Where an objective is done: a group of spawns or a POI polygon's floored points, as the member nearest the group's
// centre (a bot walks to it) and the farthest member's distance from it (2D)
struct AreaPoint
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};  // that member's own height: a spawn's, or a POI point's baked floor (WT33b, WT34 fix)
    float radius{0.0f};
    std::uint32_t points{0};  // spawns or POI points behind it
    // WT32b: what a bot needs to use the area, set on areas built from item sources only (the index is shared by
    // every bot, so the reader filters, AreaUsable)
    std::uint16_t need{0};     // a skill id the bot must have (skinning, herbalism, mining, engineering); 0 = none
    std::uint8_t notTeams{0};  // a bit per team (1 << Snapshot::team) the area is none for: its creatures are friendly
    // WT35h: a turn-in area's ender (its template and whether it is an object); 0 on every other area. Its notTeams
    // are the sides the ender is hostile to
    std::uint32_t ender{0};
    bool enderObject{false};
};

// Where an objective's places come from (PLAN 3.1's six); the census counts objectives by it
enum class PlaceFrom : std::uint8_t
{
    Target,   // the spawns of its kill or object target
    Sources,  // the spawns of its item's sources
    Trigger,  // its area trigger
    UseRule,  // the spawns of its use rule's targets (WT35b)
    Poi,      // a POI polygon's points with a baked floor (rule 5, WT33a/b)
    None,     // no place
    Count
};
constexpr std::size_t PLACE_FROMS = static_cast<std::size_t>(PlaceFrom::Count);
constexpr std::array<char const*, PLACE_FROMS> PLACE_FROM_NAMES = {"target", "sources", "trigger",
                                                                   "use",    "poi",     "none"};

inline char const* PlaceFromName(PlaceFrom from) { return PLACE_FROM_NAMES[static_cast<std::size_t>(from)]; }

// An item's source below this chance per loot is no place for the objective (a world drop: thousands of owners at a
// fraction of a percent). A tuning value, read from the census (WT32b)
constexpr float SOURCE_CHANCE_FLOOR = 0.02f;

// Whether a bot of this team that has these skills can use the area. `hasSkill(id)` says whether it has the skill
template <typename HasSkill>
bool AreaUsable(AreaPoint const& area, std::uint8_t team, HasSkill hasSkill)
{
    return (!area.need || hasSkill(area.need)) && !(area.notTeams & (1u << (team & 1u)));
}

// WT35a: the source kinds of an item objective (a bit per SourceKind) and the creatures its seek looks for. Both
// take the sources a place is built from: above the chance floor, with spawns. The seek takes the corpses, the
// likeliest first, or the skinned creatures when the item has no corpse source; at most SEEK_ENTRIES
constexpr std::size_t SEEK_ENTRIES = 8;
constexpr std::uint8_t SourceBit(SourceKind kind)
{
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(kind));
}

inline bool PlaceSource(ItemSource const& source) { return source.chance >= SOURCE_CHANCE_FLOOR && source.spawns; }

inline std::uint8_t SourceKindsOf(std::vector<ItemSource> const& sources)
{
    std::uint8_t kinds = 0;
    for (ItemSource const& source : sources)
        if (PlaceSource(source))
            kinds |= SourceBit(source.kind);
    return kinds;
}

inline std::vector<std::uint32_t> SeekEntries(std::vector<ItemSource> sources)
{
    SourceKind const kind =
        (SourceKindsOf(sources) & SourceBit(SourceKind::Corpse)) ? SourceKind::Corpse : SourceKind::Skin;
    std::stable_sort(sources.begin(), sources.end(),
                     [](ItemSource const& a, ItemSource const& b) { return a.chance > b.chance; });
    std::vector<std::uint32_t> entries;
    for (ItemSource const& source : sources)
    {
        if (!PlaceSource(source) || source.kind != kind ||
            std::find(entries.begin(), entries.end(), source.entry) != entries.end())
            continue;
        entries.push_back(source.entry);
        if (entries.size() == SEEK_ENTRIES)
            break;
    }
    return entries;
}

// WT35e: the containers a chest objective's seek looks for: the object sources a place is built from, the likeliest
// first, at most SEEK_ENTRIES (gameobject entries: a separate id space from SeekEntries' creatures)
inline std::vector<std::uint32_t> SeekObjects(std::vector<ItemSource> sources)
{
    std::stable_sort(sources.begin(), sources.end(),
                     [](ItemSource const& a, ItemSource const& b) { return a.chance > b.chance; });
    std::vector<std::uint32_t> entries;
    for (ItemSource const& source : sources)
    {
        if (!PlaceSource(source) || source.kind != SourceKind::Object ||
            std::find(entries.begin(), entries.end(), source.entry) != entries.end())
            continue;
        entries.push_back(source.entry);
        if (entries.size() == SEEK_ENTRIES)
            break;
    }
    return entries;
}

// WT35f: how a kill objective's creature gives its credit by talk: a SmartAI row of its gossip hello, or of the
// select of an option, credits the creature's own kill; Unknown: a gossip creature without such a row (a C++
// script's, a spell's or an emote's credit)
enum class TalkHow : std::uint8_t
{
    None,
    Hello,
    Select,
    Unknown
};

struct TalkCredit
{
    TalkHow how{TalkHow::None};
    std::uint32_t menu{0};  // Select: the gossip menu and its option (the list id a client's select sends)
    std::uint32_t option{0};
    std::uint8_t friendlyTo{0};  // 1 << team for each side the creature is friendly to (no talk with a foe)
};

struct QuestObjective
{
    ObjectiveKind kind{ObjectiveKind::Kill};
    std::uint32_t target{0};  // creature, gameobject or item entry; the area trigger for Explore; 0 for Turnin
    std::uint32_t count{0};
    std::int32_t poiIndex{POI_NONE};
    std::vector<AreaPoint> areas;  // largest first, at most MAX_OBJECTIVE_AREAS per requirement class (skill, side)
    PlaceFrom placeFrom{PlaceFrom::None};
    bool polygon{false};          // WT33b: its slot has a POI polygon, floored or not (the census, the poionly list)
    std::uint8_t sourceKinds{0};  // WT35a: an item objective's, SourceKindsOf
    std::vector<std::uint32_t> seek{};         // WT35a: an item objective's, SeekEntries
    std::vector<std::uint32_t> seekObjects{};  // WT35e: an item objective's, SeekObjects
    TalkCredit talk{};                         // WT35f: a kill objective's, set by the gather (TalkOf)
    UseHow use{UseHow::None};                  // WT35b: how its first use rule is done, and that rule's kind
    UseKind useKind{UseKind::UnitAlive};
    UseBrings brings{UseBrings::Credit};  // WT35g: a kill objective's creature its use rule brings forth (BringsFor)
};

// WT35a: what a quest's event is (QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT without an area trigger): an escort when
// a giver's SmartAI starts a path or a follow on the quest's accept, another script's otherwise
enum class QuestEvent : std::uint8_t
{
    None,
    Escort,
    Script
};

struct QuestRecord
{
    std::uint32_t id{0};
    std::int32_t level{0};  // -1 = the player's level
    std::uint32_t minLevel{0};
    std::uint32_t maxLevel{0};   // 0 = none
    std::uint32_t raceMask{0};   // 0 = any
    std::uint32_t classMask{0};  // 0 = any
    std::int32_t zoneOrSort{0};  // > 0 a zone, < 0 a quest sort
    std::uint32_t type{0};       // QuestType
    std::uint32_t suggestedPlayers{0};
    std::uint16_t flags{0};  // QuestFlag
    std::uint32_t xpBase{0};
    std::int32_t copperReward{0};  // negative: the quest takes money
    std::int32_t prev{0};          // > 0 must be rewarded first, < 0 must be active
    std::uint32_t next{0};         // the quest that needs this one
    std::uint32_t nextInChain{0};  // offered right after the reward
    std::int32_t exclusiveGroup{0};
    std::uint32_t breadcrumbFor{0};
    std::vector<std::uint32_t> givers;  // indices into QuestIndex::places, ascending
    std::vector<std::uint32_t> enders;
    std::vector<QuestObjective> objectives;
    // The core's prevQuests (PrevQuestId and every quest naming this one as its NextQuestId): one positive rewarded or
    // one negative active opens it (Player::SatisfyQuestPreviousQuest); empty in synthetic records, which give prev
    std::vector<std::int32_t> prevAny;
    QuestEvent event{QuestEvent::None};
    // WT35b: a use rule that completes the quest (its event flag, POI_NONE), worked as an objective; use None: none
    QuestObjective eventUse{ObjectiveKind::Turnin, 0, 1, POI_NONE, {}};

    bool Has(std::uint16_t flag) const { return (flags & flag) != 0; }
};

// Quest givers and enders standing together
struct QuestCamp
{
    std::uint32_t mapId{0};
    float x{0.0f};  // centroid
    float y{0.0f};
    float z{0.0f};
    float radius{0.0f};
    std::uint32_t zoneId{0};  // the places' most common resolved zone and area, 0 = none
    std::uint32_t areaId{0};
    std::vector<std::uint32_t> places;  // ascending
    std::vector<std::uint32_t> quests;  // given here, ascending
};

// The world thread's copy (GatherQuests)
struct QuestRelation
{
    std::uint32_t questId{0};
    std::uint32_t entry{0};
    bool isObject{false};
    bool ender{false};
};

struct TargetSpawn
{
    std::uint32_t entry{0};
    bool isObject{false};
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct PoiInput
{
    std::uint32_t questId{0};
    std::int32_t objectiveIndex{0};
    std::uint32_t mapId{0};
    std::vector<std::pair<float, float>> points;
    std::vector<float> floors;  // WT33b: per point, the baked floor (NO_FLOOR or missing = none)
    std::uint32_t poiId{0};     // quest_poi.id
};

struct TriggerInput
{
    std::uint32_t questId{0};
    std::uint32_t triggerId{0};
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float radius{0.0f};
};

struct QuestGather
{
    std::vector<QuestRecord> quests;  // fields and objectives without areas, no givers or enders
    std::vector<QuestRelation> relations;
    std::vector<QuestPlace> places;  // the spawns of every related entry
    std::vector<TargetSpawn> targets;
    std::vector<PoiInput> pois;
    std::vector<TriggerInput> triggers;
    QuestUseGather uses;          // source items with an on-use spell and their targets (WU20b)
    SourceGather sources;         // loot rows, their owners and the owners' spawns (WT32a)
    std::uint32_t unresolved{0};  // places without a zone
};

struct QuestIndexStats
{
    std::uint32_t quests{0};
    std::uint32_t places{0};
    std::uint32_t givers{0};  // places that give a quest
    std::uint32_t enders{0};  // places that take one
    std::uint32_t camps{0};
    std::uint32_t objectives{0};
    std::uint32_t withoutArea{0};
    std::uint32_t withoutGiver{0};  // quests no spawn gives (items, scripts, events)
    // WT32b census: objectives (turn-ins apart) by where their places come from, all and item objectives only; what
    // the item objectives were placed by before the sources (the POI polygon, or none)
    std::array<std::uint32_t, PLACE_FROMS> placeFrom{};
    std::array<std::uint32_t, PLACE_FROMS> itemPlaceFrom{};
    std::uint32_t turnins{0};
    std::uint32_t itemBeforePoi{0};
    std::uint32_t itemBeforeNone{0};
    std::uint32_t unresolved{0};
    std::uint64_t estimatedBytes{0};  // vectors' capacity x element size plus the hash nodes: an estimate
    std::uint32_t gatherMs{0};        // world thread
    std::uint32_t buildMs{0};         // worker
};

namespace Detail
{
inline std::uint64_t EntryKey(std::uint32_t entry, bool isObject)
{
    return (static_cast<std::uint64_t>(isObject) << 32) | entry;
}

inline bool PlaceLess(QuestPlace const& a, QuestPlace const& b)
{
    return std::tie(a.mapId, a.x, a.y, a.z, a.entry, a.isObject) <
           std::tie(b.mapId, b.x, b.y, b.z, b.entry, b.isObject);
}

inline void SortUnique(std::vector<std::uint32_t>& list)
{
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    list.shrink_to_fit();
}

// A group's centroid and the farthest point's distance from it in the plane, for a camp's centre (a ranking and
// grouping point, nothing walks to it). pointOf(i) returns {x, y, z}
template <typename PointOf>
AreaPoint CentreOf(std::uint32_t mapId, std::vector<std::uint32_t> const& members, PointOf pointOf)
{
    AreaPoint area;
    area.mapId = mapId;
    area.points = static_cast<std::uint32_t>(members.size());
    if (members.empty())
        return area;
    double sx = 0.0;
    double sy = 0.0;
    double sz = 0.0;
    for (std::uint32_t const i : members)
    {
        auto const [x, y, z] = pointOf(i);
        sx += x;
        sy += y;
        sz += z;
    }
    double const n = static_cast<double>(members.size());
    area.x = static_cast<float>(sx / n);
    area.y = static_cast<float>(sy / n);
    area.z = static_cast<float>(sz / n);
    for (std::uint32_t const i : members)
    {
        auto const [x, y, z] = pointOf(i);
        area.radius = std::max(area.radius, std::hypot(x - area.x, y - area.y));
    }
    return area;
}

// One objective area from a group of points (WT34 fix): the member nearest the centroid in the plane (ties by
// position), at that member's own position and height, and the farthest member's distance from it. The centroid
// itself is no place: the mean of a camp outside and a cave's floors lies inside the rock, of a ring of spawns in the
// lake they stand around. pointOf(i) returns {x, y, z}
template <typename PointOf>
AreaPoint AreaOf(std::uint32_t mapId, std::vector<std::uint32_t> const& members, PointOf pointOf)
{
    AreaPoint area = CentreOf(mapId, members, pointOf);
    if (members.empty())
        return area;
    float const cx = area.x;
    float const cy = area.y;
    float bestYards = 0.0f;
    bool found = false;
    for (std::uint32_t const i : members)
    {
        auto const [x, y, z] = pointOf(i);
        float const yards = std::hypot(x - cx, y - cy);
        if (!found || std::tie(yards, x, y, z) < std::tie(bestYards, area.x, area.y, area.z))
        {
            found = true;
            bestYards = yards;
            area.x = x;
            area.y = y;
            area.z = z;
        }
    }
    area.radius = 0.0f;
    for (std::uint32_t const i : members)
    {
        auto const [x, y, z] = pointOf(i);
        area.radius = std::max(area.radius, std::hypot(x - area.x, y - area.y));
    }
    return area;
}

// Largest first, ties by position: the order does not depend on the input order
inline bool AreaFirst(AreaPoint const& a, AreaPoint const& b)
{
    return std::tie(b.points, a.mapId, a.x, a.y) < std::tie(a.points, b.mapId, b.x, b.y);
}

inline void KeepLargest(std::vector<AreaPoint>& areas)
{
    std::sort(areas.begin(), areas.end(), AreaFirst);
    if (areas.size() > MAX_OBJECTIVE_AREAS)
        areas.resize(MAX_OBJECTIVE_AREAS);
    areas.shrink_to_fit();
}

inline std::vector<Nearby> NearestFirst(std::vector<Nearby> found, std::size_t k)
{
    std::sort(found.begin(), found.end(), [](Nearby const& a, Nearby const& b)
              { return a.distance < b.distance || (a.distance == b.distance && a.index < b.index); });
    if (found.size() > k)
        found.resize(k);
    return found;
}
}  // namespace Detail

// Immutable once built. Readers on any thread hold a shared_ptr to it
class QuestIndex
{
public:
    std::vector<QuestRecord> quests;  // ascending id
    std::vector<QuestPlace> places;   // canonical order: map, position, entry
    std::vector<QuestCamp> camps;     // in the order of their first cell
    QuestUseTable uses;               // what to use where, per quest
    SourceTable sources;              // where an item comes from
    QuestIndexStats stats;

    static QuestIndex Build(QuestGather input)
    {
        QuestIndex index;
        index.stats.unresolved = input.unresolved;
        index.BuildQuests(std::move(input.quests));
        index.sources = SourceTable::Build(std::move(input.sources));  // before the objectives, which read it
        index.BuildPlaces(std::move(input.places), input.relations);
        index.BuildUses(std::move(input.uses));  // WT35b: before the objectives, which place the rules' targets
        index.BuildObjectives(std::move(input.targets), std::move(input.pois), std::move(input.triggers));
        index.BuildCamps();
        index.BuildGiverGrid();
        index.BuildUnlocks();
        index.CountStats();
        index.stats.estimatedBytes = index.EstimateBytes();
        return index;
    }

    QuestRecord const* Find(std::uint32_t id) const
    {
        auto const it =
            std::lower_bound(quests.begin(), quests.end(), id,
                             [](QuestRecord const& quest, std::uint32_t value) { return quest.id < value; });
        return it != quests.end() && it->id == id ? &*it : nullptr;
    }

    // The creatures, skins and objects the item comes from, by kind and entry
    std::vector<ItemSource> SourcesOf(std::uint32_t item) const { return sources.SourcesOf(item); }

    // The quests the place gives (takes), ascending
    std::vector<std::uint32_t> const& QuestsGivenAt(std::uint32_t place) const
    {
        return place < _givenAt.size() ? _givenAt[place] : None();
    }

    std::vector<std::uint32_t> const& QuestsEndedAt(std::uint32_t place) const
    {
        return place < _endedAt.size() ? _endedAt[place] : None();
    }

    // The quests a camp's places give, ascending
    std::vector<std::uint32_t> const& QuestsOfGiverCluster(std::uint32_t camp) const
    {
        return camp < camps.size() ? camps[camp].quests : None();
    }

    // The quests this one opens: those naming it as their previous quest, its next quest and its next in chain
    std::vector<std::uint32_t> const& Unlocks(std::uint32_t id) const
    {
        auto const it = _unlocks.find(id);
        return it == _unlocks.end() ? None() : it->second;
    }

    // The places on the map that give a quest within yards of the point in the plane, nearest first (ties by index)
    std::vector<std::uint32_t> GiversNear(std::uint32_t mapId, float x, float y, float yards) const
    {
        std::vector<std::uint32_t> result;
        auto const bounds = _giverBounds.find(mapId);
        if (bounds == _giverBounds.end() || !(yards > 0.0f) || !Detail::Finite(x, y, yards))
            return result;
        CellRange const& b = bounds->second;
        std::int32_t const lowX = std::max(Detail::Cell(x - yards, GIVER_CELL_YARDS), b.minX);
        std::int32_t const highX = std::min(Detail::Cell(x + yards, GIVER_CELL_YARDS), b.maxX);
        std::int32_t const lowY = std::max(Detail::Cell(y - yards, GIVER_CELL_YARDS), b.minY);
        std::int32_t const highY = std::min(Detail::Cell(y + yards, GIVER_CELL_YARDS), b.maxY);
        std::vector<Nearby> found;
        for (std::int32_t cx = lowX; cx <= highX; ++cx)
            for (std::int32_t cy = lowY; cy <= highY; ++cy)
            {
                auto const cell = _giverCells.find(Detail::CellKey(mapId, cx, cy));
                if (cell == _giverCells.end())
                    continue;
                for (std::uint32_t const place : cell->second)
                {
                    float const d = std::hypot(places[place].x - x, places[place].y - y);
                    if (d <= yards)
                        found.push_back({place, d});
                }
            }
        std::size_t const all = found.size();
        found = Detail::NearestFirst(std::move(found), all);
        result.reserve(found.size());
        for (Nearby const& one : found)
            result.push_back(one.index);
        return result;
    }

    // Up to k camps on the map nearest to the point, nearest first; within maxYards when given
    std::vector<Nearby> NearestCamps(std::uint32_t mapId, float x, float y, float z, std::size_t k,
                                     float maxYards = 0.0f) const
    {
        auto const onMap = _campsByMap.find(mapId);
        if (!k || onMap == _campsByMap.end() || !Detail::Finite(x, y, z))
            return {};
        std::vector<Nearby> found;
        for (std::uint32_t const index : onMap->second)
        {
            QuestCamp const& camp = camps[index];
            float const d = Detail::Distance(x, y, z, camp.x, camp.y, camp.z);
            if (maxYards <= 0.0f || d <= maxYards)
                found.push_back({index, d});
        }
        return Detail::NearestFirst(std::move(found), k);
    }

    std::uint64_t EstimateBytes() const
    {
        std::uint64_t bytes =
            sizeof(*this) + Detail::VectorBytes(quests) + Detail::VectorBytes(places) + Detail::VectorBytes(camps);
        for (QuestRecord const& quest : quests)
        {
            bytes += Detail::VectorBytes(quest.givers) + Detail::VectorBytes(quest.enders) +
                     Detail::VectorBytes(quest.objectives);
            for (QuestObjective const& objective : quest.objectives)
                bytes += Detail::VectorBytes(objective.areas);
        }
        for (QuestCamp const& camp : camps)
            bytes += Detail::VectorBytes(camp.places) + Detail::VectorBytes(camp.quests);
        bytes += Detail::VectorBytes(_givenAt) + Detail::VectorBytes(_endedAt);
        for (std::size_t i = 0; i < _givenAt.size(); ++i)
            bytes += Detail::VectorBytes(_givenAt[i]) + Detail::VectorBytes(_endedAt[i]);
        for (auto const& [id, list] : _unlocks)
            bytes += NODE_BYTES + Detail::VectorBytes(list);
        for (auto const& [key, list] : _giverCells)
            bytes += NODE_BYTES + Detail::VectorBytes(list);
        for (auto const& [map, list] : _campsByMap)
            bytes += NODE_BYTES + Detail::VectorBytes(list);
        bytes += _giverBounds.size() * NODE_BYTES;
        bytes += Detail::VectorBytes(uses.rules);
        for (QuestUseRule const& rule : uses.rules)
            bytes += Detail::VectorBytes(rule.entries);
        bytes += sources.EstimateBytes();
        return bytes;
    }

private:
    struct CellRange
    {
        std::int32_t minX{0};
        std::int32_t maxX{0};
        std::int32_t minY{0};
        std::int32_t maxY{0};
    };

    std::vector<std::vector<std::uint32_t>> _givenAt;  // per place
    std::vector<std::vector<std::uint32_t>> _endedAt;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> _unlocks;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> _giverCells;  // (map, cell) -> giver places
    std::unordered_map<std::uint32_t, CellRange> _giverBounds;                  // map -> occupied cell range
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> _campsByMap;

    static std::vector<std::uint32_t> const& None()
    {
        static std::vector<std::uint32_t> const none;
        return none;
    }

    QuestRecord* FindMutable(std::uint32_t id) { return const_cast<QuestRecord*>(Find(id)); }

    void BuildQuests(std::vector<QuestRecord> input)
    {
        quests = std::move(input);
        std::sort(quests.begin(), quests.end(), [](QuestRecord const& a, QuestRecord const& b) { return a.id < b.id; });
        quests.erase(std::unique(quests.begin(), quests.end(),
                                 [](QuestRecord const& a, QuestRecord const& b) { return a.id == b.id; }),
                     quests.end());
        quests.shrink_to_fit();
    }

    void BuildPlaces(std::vector<QuestPlace> input, std::vector<QuestRelation> const& relations)
    {
        std::sort(input.begin(), input.end(), Detail::PlaceLess);
        input.erase(std::unique(input.begin(), input.end(), [](QuestPlace const& a, QuestPlace const& b)
                                { return !Detail::PlaceLess(a, b) && !Detail::PlaceLess(b, a); }),
                    input.end());
        places = std::move(input);
        places.shrink_to_fit();
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> byEntry;
        for (std::uint32_t i = 0; i < places.size(); ++i)
            byEntry[Detail::EntryKey(places[i].entry, places[i].isObject)].push_back(i);
        _givenAt.assign(places.size(), {});
        _endedAt.assign(places.size(), {});
        for (QuestRelation const& relation : relations)
        {
            QuestRecord* quest = FindMutable(relation.questId);
            auto const spawns = byEntry.find(Detail::EntryKey(relation.entry, relation.isObject));
            if (!quest || spawns == byEntry.end())
                continue;
            std::vector<std::uint32_t>& list = relation.ender ? quest->enders : quest->givers;
            list.insert(list.end(), spawns->second.begin(), spawns->second.end());
            for (std::uint32_t const place : spawns->second)
                (relation.ender ? _endedAt : _givenAt)[place].push_back(relation.questId);
        }
        for (QuestRecord& quest : quests)
        {
            Detail::SortUnique(quest.givers);
            Detail::SortUnique(quest.enders);
        }
        for (std::size_t i = 0; i < places.size(); ++i)
        {
            Detail::SortUnique(_givenAt[i]);
            Detail::SortUnique(_endedAt[i]);
        }
    }

    // Kill and object targets get their spawn areas (computed once per target), the rest and targets without spawns
    // their POIs; area triggers make Explore objectives and the turn-in POI a Turnin objective
    void BuildObjectives(std::vector<TargetSpawn> targets, std::vector<PoiInput> pois,
                         std::vector<TriggerInput> triggers)
    {
        auto const targetLess = [](TargetSpawn const& a, TargetSpawn const& b)
        {
            return std::tie(a.isObject, a.entry, a.mapId, a.x, a.y, a.z) <
                   std::tie(b.isObject, b.entry, b.mapId, b.x, b.y, b.z);
        };
        std::sort(targets.begin(), targets.end(), targetLess);
        std::unordered_map<std::uint64_t, std::pair<std::size_t, std::size_t>> spawnRanges;  // target -> [first, last)
        for (std::size_t i = 0; i < targets.size();)
        {
            std::size_t j = i;
            while (j < targets.size() && targets[j].entry == targets[i].entry &&
                   targets[j].isObject == targets[i].isObject)
                ++j;
            spawnRanges[Detail::EntryKey(targets[i].entry, targets[i].isObject)] = {i, j};
            i = j;
        }
        std::unordered_map<std::uint64_t, std::vector<AreaPoint>> spawnAreas;
        auto const areasOfTarget = [&](std::uint32_t entry, bool isObject) -> std::vector<AreaPoint> const&
        {
            std::uint64_t const key = Detail::EntryKey(entry, isObject);
            auto const [cached, added] = spawnAreas.try_emplace(key);
            auto const range = spawnRanges.find(key);
            if (!added || range == spawnRanges.end())
                return cached->second;
            std::size_t const first = range->second.first;
            std::uint32_t const count = static_cast<std::uint32_t>(range->second.second - first);
            for (std::vector<std::uint32_t> const& group :
                 Detail::ClusterByCells(count, AREA_CELL_YARDS,
                                        [&targets, first](std::uint32_t i)
                                        {
                                            TargetSpawn const& t = targets[first + i];
                                            return std::make_tuple(t.mapId, t.x, t.y);
                                        }))
                cached->second.push_back(Detail::AreaOf(targets[first + group.front()].mapId, group,
                                                        [&targets, first](std::uint32_t i)
                                                        {
                                                            TargetSpawn const& t = targets[first + i];
                                                            return std::make_tuple(t.x, t.y, t.z);
                                                        }));
            Detail::KeepLargest(cached->second);
            return cached->second;
        };

        // WT32b: an item's places are the spawns of its sources above the chance floor, clustered per requirement
        // class (skill, side) so a reader can drop the ones its bot cannot use. Friendly to both sides: no place
        struct Spot
        {
            std::uint16_t need;
            std::uint8_t notTeams;
            std::uint32_t mapId;
            float x;
            float y;
            float z;
        };
        std::unordered_map<std::uint32_t, std::vector<AreaPoint>> sourceAreas;
        auto const areasOfItem = [&](std::uint32_t item) -> std::vector<AreaPoint> const&
        {
            auto const [cached, added] = sourceAreas.try_emplace(item);
            if (!added)
                return cached->second;
            std::vector<Spot> spots;
            for (ItemSource const& source : sources.SourcesOf(item))
            {
                if (source.chance < SOURCE_CHANCE_FLOOR || !source.spawns)
                    continue;
                bool const isObject = source.kind == SourceKind::Object;
                auto const [first, last] = sources.SpawnsOf(source.entry, isObject);
                for (auto spawn = first; spawn != last; ++spawn)
                {
                    std::uint8_t const notTeams = isObject ? 0 : spawn->friendlyTo;
                    if (notTeams == TEAM_BOTH)
                        continue;
                    spots.push_back({source.kind == SourceKind::Skin ? spawn->skill : std::uint16_t{0}, notTeams,
                                     spawn->mapId, spawn->x, spawn->y, spawn->z});
                }
            }
            std::sort(spots.begin(), spots.end(),
                      [](Spot const& a, Spot const& b)
                      {
                          return std::tie(a.need, a.notTeams, a.mapId, a.x, a.y, a.z) <
                                 std::tie(b.need, b.notTeams, b.mapId, b.x, b.y, b.z);
                      });
            for (std::size_t i = 0; i < spots.size();)
            {
                std::size_t j = i;
                while (j < spots.size() && spots[j].need == spots[i].need && spots[j].notTeams == spots[i].notTeams)
                    ++j;
                std::vector<AreaPoint> klass;
                for (std::vector<std::uint32_t> const& group :
                     Detail::ClusterByCells(static_cast<std::uint32_t>(j - i), AREA_CELL_YARDS,
                                            [&spots, i](std::uint32_t k)
                                            {
                                                Spot const& t = spots[i + k];
                                                return std::make_tuple(t.mapId, t.x, t.y);
                                            }))
                {
                    AreaPoint area = Detail::AreaOf(spots[i + group.front()].mapId, group,
                                                    [&spots, i](std::uint32_t k)
                                                    {
                                                        Spot const& t = spots[i + k];
                                                        return std::make_tuple(t.x, t.y, t.z);
                                                    });
                    area.need = spots[i].need;
                    area.notTeams = spots[i].notTeams;
                    klass.push_back(area);
                }
                Detail::KeepLargest(klass);
                cached->second.insert(cached->second.end(), klass.begin(), klass.end());
                i = j;
            }
            std::sort(cached->second.begin(), cached->second.end(), Detail::AreaFirst);
            cached->second.shrink_to_fit();
            return cached->second;
        };

        std::sort(pois.begin(), pois.end(),
                  [](PoiInput const& a, PoiInput const& b)
                  {
                      return std::tie(a.questId, a.objectiveIndex, a.mapId, a.points) <
                             std::tie(b.questId, b.objectiveIndex, b.mapId, b.points);
                  });
        auto const poiRange = [&pois](std::uint32_t questId, std::int32_t objectiveIndex)
        {
            auto const first = std::lower_bound(
                pois.begin(), pois.end(), std::make_pair(questId, objectiveIndex),
                [](PoiInput const& poi, std::pair<std::uint32_t, std::int32_t> const& key)
                { return std::tie(poi.questId, poi.objectiveIndex) < std::tie(key.first, key.second); });
            auto last = first;
            while (last != pois.end() && last->questId == questId && last->objectiveIndex == objectiveIndex)
                ++last;
            return std::make_pair(first, last);
        };
        auto const hasPolygon = [&poiRange](std::uint32_t questId, std::int32_t objectiveIndex)
        {
            auto const [first, last] = poiRange(questId, objectiveIndex);
            return std::any_of(first, last, [](PoiInput const& poi) { return !poi.points.empty(); });
        };
        // WT33b: a POI is a place only through its points with a baked floor (WQ.10: a floor only where no spawn
        // gives a place); a polygon without one is no place (rule 6)
        auto const poiAreas = [&poiRange](std::uint32_t questId, std::int32_t objectiveIndex)
        {
            std::vector<AreaPoint> areas;
            auto const [first, last] = poiRange(questId, objectiveIndex);
            for (auto it = first; it != last; ++it)
            {
                std::vector<std::uint32_t> members;
                for (std::uint32_t i = 0; i < it->points.size() && i < it->floors.size(); ++i)
                    if (std::isfinite(it->floors[i]))
                        members.push_back(i);
                if (members.empty())
                    continue;
                PoiInput const& poi = *it;
                areas.push_back(Detail::AreaOf(
                    poi.mapId, members, [&poi](std::uint32_t i)
                    { return std::make_tuple(poi.points[i].first, poi.points[i].second, poi.floors[i]); }));
            }
            Detail::KeepLargest(areas);
            return areas;
        };

        std::sort(triggers.begin(), triggers.end(), [](TriggerInput const& a, TriggerInput const& b)
                  { return std::tie(a.questId, a.triggerId) < std::tie(b.questId, b.triggerId); });
        for (TriggerInput const& trigger : triggers)
            if (QuestRecord* quest = FindMutable(trigger.questId))
            {
                quest->objectives.push_back({ObjectiveKind::Explore,
                                             trigger.triggerId,
                                             1,
                                             POI_NONE,
                                             {{trigger.mapId, trigger.x, trigger.y, trigger.z, trigger.radius, 1}}});
                quest->objectives.back().placeFrom = PlaceFrom::Trigger;
                quest->event = QuestEvent::None;  // its event is the trigger's
            }

        for (QuestRecord& quest : quests)
        {
            for (QuestObjective& objective : quest.objectives)
            {
                // An Explore objective keeps its trigger's place (PlaceFrom::Trigger, set above)
                if (objective.kind == ObjectiveKind::Kill || objective.kind == ObjectiveKind::Object)
                {
                    objective.areas = areasOfTarget(objective.target, objective.kind == ObjectiveKind::Object);
                    if (!objective.areas.empty())
                        objective.placeFrom = PlaceFrom::Target;
                }
                objective.polygon = objective.poiIndex != POI_NONE && hasPolygon(quest.id, objective.poiIndex);
                if (objective.kind == ObjectiveKind::Item)
                {
                    // The sources first; the POI polygon only where no source gives a place. What the polygon alone
                    // would have given is the census's "before"
                    std::vector<AreaPoint> poi = objective.poiIndex != POI_NONE ? poiAreas(quest.id, objective.poiIndex)
                                                                                : std::vector<AreaPoint>{};
                    ++(objective.polygon ? stats.itemBeforePoi : stats.itemBeforeNone);
                    std::vector<ItemSource> const itemSources = sources.SourcesOf(objective.target);
                    objective.sourceKinds = SourceKindsOf(itemSources);
                    objective.seek = SeekEntries(itemSources);
                    objective.seekObjects = SeekObjects(itemSources);
                    objective.areas = areasOfItem(objective.target);
                    if (!objective.areas.empty())
                        objective.placeFrom = PlaceFrom::Sources;
                    else if (!poi.empty())
                    {
                        objective.areas = std::move(poi);
                        objective.placeFrom = PlaceFrom::Poi;
                    }
                }
                else if (objective.areas.empty() && objective.poiIndex != POI_NONE)
                {
                    objective.areas = poiAreas(quest.id, objective.poiIndex);
                    if (!objective.areas.empty())
                        objective.placeFrom = PlaceFrom::Poi;
                }
            }
            // WT35b: an objective with a use rule is done at the spawns of the rule's targets, creatures or objects
            // (place 4; without a spawn it keeps its places). A rule that completes the quest is its event's, else
            // that of each objective no spawn places (a credit the use gives: 9303's owlkin credit no creature has)
            auto const attach = [&](QuestUseRule const& rule, QuestObjective& objective)
            {
                if (objective.use != UseHow::None)
                    return;
                objective.use = HowOf(rule);
                objective.useKind = rule.kind;
                if (objective.kind == ObjectiveKind::Kill)
                    objective.brings = BringsFor(rule, objective.target, objective.placeFrom == PlaceFrom::Target);
                std::vector<AreaPoint> areas;
                for (std::uint32_t const entry : rule.entries)
                    for (AreaPoint const& area :
                         areasOfTarget(entry, rule.kind == UseKind::Object || rule.kind == UseKind::Focus))
                        areas.push_back(area);
                Detail::KeepLargest(areas);
                if (!areas.empty())
                    std::tie(objective.areas, objective.placeFrom) = std::make_pair(areas, PlaceFrom::UseRule);
            };
            for (auto [rule, last] = uses.RulesFor(quest.id); rule != last; ++rule)
                if (rule->objective < quest.objectives.size())
                    attach(*rule, quest.objectives[rule->objective]);
                else if (quest.event == QuestEvent::Script)
                    attach(*rule, quest.eventUse);
                else
                    for (QuestObjective& objective : quest.objectives)
                        if ((objective.kind == ObjectiveKind::Kill || objective.kind == ObjectiveKind::Item) &&
                            objective.placeFrom != PlaceFrom::Target && objective.placeFrom != PlaceFrom::Sources)
                            attach(*rule, objective);
            // WT33b: the turn-in is at its enders' spawns, one area each (an ender stands where it is: no centroid);
            // the turn-in POI with a baked floor only for a quest no spawn ends (rule 5 after rule 1). WT35h: a spawn
            // outside the default phase is no ender; an area keeps its ender and the sides it is hostile to
            std::vector<AreaPoint> turnin;
            for (std::uint32_t const ender : quest.enders)
            {
                QuestPlace const& place = places[ender];
                if (!place.otherPhase)
                    turnin.push_back({place.mapId, place.x, place.y, place.z, 0.0f, 1, 0, place.hostileTo, place.entry,
                                      place.isObject});
            }
            PlaceFrom const turninFrom = turnin.empty() ? PlaceFrom::Poi : PlaceFrom::Target;
            if (turnin.empty())
                turnin = poiAreas(quest.id, POI_TURNIN);
            if (!turnin.empty())
            {
                quest.objectives.push_back({ObjectiveKind::Turnin, 0, 1, POI_TURNIN, std::move(turnin)});
                quest.objectives.back().placeFrom = turninFrom;
                quest.objectives.back().polygon = hasPolygon(quest.id, POI_TURNIN);
            }
            quest.objectives.shrink_to_fit();
        }
    }

    void BuildCamps()
    {
        std::vector<std::vector<std::uint32_t>> groups =
            Detail::ClusterByCells(static_cast<std::uint32_t>(places.size()), CAMP_CELL_YARDS, [this](std::uint32_t i)
                                   { return std::make_tuple(places[i].mapId, places[i].x, places[i].y); });
        camps.reserve(groups.size());
        for (std::vector<std::uint32_t>& members : groups)
        {
            std::uint32_t const campIndex = static_cast<std::uint32_t>(camps.size());
            AreaPoint const area = Detail::CentreOf(places[members.front()].mapId, members, [this](std::uint32_t i)
                                                    { return std::make_tuple(places[i].x, places[i].y, places[i].z); });
            QuestCamp camp;
            camp.mapId = area.mapId;
            camp.x = area.x;
            camp.y = area.y;
            camp.z = area.z;
            camp.radius = area.radius;
            std::map<std::uint32_t, std::uint32_t> zoneVotes;
            std::map<std::uint32_t, std::uint32_t> areaVotes;
            for (std::uint32_t const place : members)
            {
                places[place].camp = campIndex;
                if (places[place].zoneId)
                {
                    ++zoneVotes[places[place].zoneId];
                    ++areaVotes[places[place].areaId];
                }
                camp.quests.insert(camp.quests.end(), _givenAt[place].begin(), _givenAt[place].end());
            }
            auto const top = [](std::map<std::uint32_t, std::uint32_t> const& votes) -> std::uint32_t
            {
                auto const best = std::max_element(votes.begin(), votes.end(),
                                                   [](auto const& a, auto const& b) { return a.second < b.second; });
                return best == votes.end() ? 0 : best->first;
            };
            camp.zoneId = top(zoneVotes);
            camp.areaId = top(areaVotes);
            Detail::SortUnique(camp.quests);
            camp.places = std::move(members);
            camp.places.shrink_to_fit();
            _campsByMap[camp.mapId].push_back(campIndex);
            camps.push_back(std::move(camp));
        }
    }

    void BuildGiverGrid()
    {
        for (std::uint32_t i = 0; i < places.size(); ++i)
        {
            if (_givenAt[i].empty())
                continue;
            QuestPlace const& place = places[i];
            std::int32_t const cx = Detail::Cell(place.x, GIVER_CELL_YARDS);
            std::int32_t const cy = Detail::Cell(place.y, GIVER_CELL_YARDS);
            _giverCells[Detail::CellKey(place.mapId, cx, cy)].push_back(i);
            auto const [it, added] = _giverBounds.try_emplace(place.mapId, CellRange{cx, cx, cy, cy});
            if (!added)
            {
                it->second.minX = std::min(it->second.minX, cx);
                it->second.maxX = std::max(it->second.maxX, cx);
                it->second.minY = std::min(it->second.minY, cy);
                it->second.maxY = std::max(it->second.maxY, cy);
            }
        }
    }

    void BuildUnlocks()
    {
        for (QuestRecord const& quest : quests)
        {
            if (quest.prev > 0)
                _unlocks[static_cast<std::uint32_t>(quest.prev)].push_back(quest.id);
            for (std::int32_t const prev : quest.prevAny)
                if (prev > 0)
                    _unlocks[static_cast<std::uint32_t>(prev)].push_back(quest.id);
            if (quest.next)
                _unlocks[quest.id].push_back(quest.next);
            if (quest.nextInChain)
                _unlocks[quest.id].push_back(quest.nextInChain);
        }
        for (auto& [id, list] : _unlocks)
            Detail::SortUnique(list);
    }

    void BuildUses(QuestUseGather input)
    {
        std::vector<UseQuest> list;
        list.reserve(quests.size());
        for (QuestRecord const& quest : quests)
        {
            UseQuest use{quest.id, quest.classMask, {}};
            for (QuestObjective const& objective : quest.objectives)
                use.goals.emplace_back(objective.kind == ObjectiveKind::Kill     ? UseGoalKind::Kill
                                       : objective.kind == ObjectiveKind::Object ? UseGoalKind::Object
                                       : objective.kind == ObjectiveKind::Item   ? UseGoalKind::Item
                                                                                 : UseGoalKind::Other,
                                       objective.target);
            list.push_back(std::move(use));
        }
        uses = QuestUseTable::Build(std::move(input), list);
    }

    void CountStats()
    {
        stats.quests = static_cast<std::uint32_t>(quests.size());
        stats.places = static_cast<std::uint32_t>(places.size());
        stats.camps = static_cast<std::uint32_t>(camps.size());
        for (std::size_t i = 0; i < places.size(); ++i)
        {
            stats.givers += _givenAt[i].empty() ? 0 : 1;
            stats.enders += _endedAt[i].empty() ? 0 : 1;
        }
        for (QuestRecord const& quest : quests)
        {
            stats.withoutGiver += quest.givers.empty() ? 1 : 0;
            stats.objectives += static_cast<std::uint32_t>(quest.objectives.size());
            for (QuestObjective const& objective : quest.objectives)
            {
                stats.withoutArea += objective.areas.empty() ? 1 : 0;
                if (objective.kind == ObjectiveKind::Turnin)
                {
                    ++stats.turnins;
                    continue;
                }
                ++stats.placeFrom[static_cast<std::size_t>(objective.placeFrom)];
                if (objective.kind == ObjectiveKind::Item)
                    ++stats.itemPlaceFrom[static_cast<std::size_t>(objective.placeFrom)];
            }
        }
    }
};

// Whether a character of this race, class and level may take the quest (the template's limits; chain and
// reputation rules are not here)
inline bool Eligible(QuestRecord const& quest, std::uint8_t race, std::uint8_t classId, std::uint8_t level)
{
    auto const allows = [](std::uint32_t mask, std::uint8_t id)
    { return !mask || (id >= 1 && id <= 32 && (mask & (1u << (id - 1)))); };
    return allows(quest.raceMask, race) && allows(quest.classMask, classId) && level >= quest.minLevel &&
           (!quest.maxLevel || level <= quest.maxLevel);
}

// The services hub a camp belongs to: the nearest hub whose core (its radius, at most HUB_CORE_YARDS from its
// centre) holds the camp's centre; none for a camp at the edge of a wide hub or outside every hub
inline std::optional<Nearby> CampHub(QuestCamp const& camp, PlannerIndexes const& indexes)
{
    for (Nearby const& hub :
         indexes.NearestHubs(camp.mapId, camp.x, camp.y, camp.z, CAMP_HUB_CANDIDATES, HUB_CORE_YARDS))
        if (hub.distance <= std::min(indexes.hubs[hub.index].radius, HUB_CORE_YARDS))
            return hub;
    return std::nullopt;
}

using QuestBook = SharedSlot<QuestIndex>;
}  // namespace Planner

#endif
