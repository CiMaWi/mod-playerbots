/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTFIELDS_H
#define _PLAYERBOT_QUESTFIELDS_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "FieldMap.h"
#include "GoalModel.h"
#include "PlannerSnapshot.h"
#include "QuestCircuit.h"
#include "QuestExecRules.h"
#include "QuestIndex.h"
#include "QuestTakeRules.h"

// What the quest log needs and what a field is worth (questing plan WQ14, PLAN 3.10 parts 2 and 3): one list of needs
// from the log and the two indexes, and per field (a cell of the field map with its eight neighbours) the credits a
// bot can expect in a stay, their worth per minute and the risk of the creatures there. WQ17b: the quests the bot may
// take are needs too (TakeNeeds), a quest's worth is split by step, a need no spawn places takes the index's places.
// Pure: the quest index, the field map and the log's slots; no game object, no config.
// tests/quest-fields-selftest.cpp checks it over generated logs; `playerbots fields census` reads it over every quest
// the feed offers
namespace Planner
{
namespace QuestFields
{
constexpr float FIELD_WALK_YARDS = 600.0f;      // WQ.37 (2): straight yards at run speed this far; a route beyond
constexpr std::uint32_t HAND_IN = 0xFFFFFFFFu;  // Need::objective of a ready quest's hand-in
constexpr std::uint32_t TAKE = 0xFFFFFFFEu;     // Need::objective of a quest the bot may take, at its givers
constexpr float SECONDS_PER_MINUTE = 60.0f;

// WQ17b (WQ.37 points 2 and 3): a quest's worth by step, tuning values. The reward comes at the hand-in, so it holds
// the largest share, more than the credits' whole share and so more than any one credit; the take, which only opens
// the rest, the smallest. A quest without a worked objective gives all but the take's share at its hand-in
constexpr float TAKE_SHARE = 0.1f;
constexpr float HAND_IN_SHARE = 0.5f;
constexpr float CREDITS_SHARE = 1.0f - TAKE_SHARE - HAND_IN_SHARE;
static_assert(HAND_IN_SHARE > CREDITS_SHARE && TAKE_SHARE < HAND_IN_SHARE && CREDITS_SHARE > 0.0f);
constexpr float TAKE_SECONDS = Circuit::TURNIN_SECONDS;  // at a giver, as a hand-in at an ender

// WQ17b (PLAN 3.10 part 3): a cell of a field whose lowest hostile lies beyond the bot's reach, that holds no needed
// creature beyond reach and at most this share of the field's hostiles (a lone rare), counts its hostiles to the elite
// share instead of setting the level the field is judged by. A tuning value
constexpr float FEW_HOSTILES_SHARE = 0.1f;

enum class NeedKind : std::uint8_t
{
    Kill,
    Loot,    // an item from creatures' corpses or skins
    Chest,   // an item from objects
    Object,  // an object used
    Use,     // a quest item used on the rule's targets
    Cast,    // a class spell cast on them
    Explore,
    Talk,
    HandIn,
    Take,   // WQ17b: a quest the bot may take, at its givers
    Other,  // a work the executor refuses; only in a quest the capability table did not mark
    Count
};
constexpr std::size_t NEED_KINDS = static_cast<std::size_t>(NeedKind::Count);
constexpr std::array<char const*, NEED_KINDS> NEED_KIND_NAMES = {"kill",    "loot", "chest",  "object", "use",  "cast",
                                                                 "explore", "talk", "handin", "take",   "other"};

inline char const* NeedKindName(NeedKind kind) { return NEED_KIND_NAMES[static_cast<std::size_t>(kind)]; }

// Explore is done at an area trigger and talk at a friendly creature: neither is worked over a field of spawns. WQ17b:
// they are worked at the index's places for the objective (UsesPlaces)
inline bool Fieldless(NeedKind kind)
{
    return kind == NeedKind::Explore || kind == NeedKind::Talk || kind == NeedKind::Other;
}

// Why a need has no field, in the order they are told apart
enum class NoField : std::uint8_t
{
    Fieldless,    // explore, talk: needs no field, and the index has no place for it
    NoSource,     // no entry gives it: an item without a source above SOURCE_CHANCE_FLOOR, a quest without an ender
    NoCredit,     // its count left is 0, or its spawns stand no share of the time
    NoSpawn,      // its entries spawn nowhere on the field map
    OtherMap,     // they spawn, but not on this map
    OutOfRadius,  // not within the radius
    TooRisky,     // only in fields refused for risk, or above the bot's reach
    Count
};
constexpr std::size_t NO_FIELDS = static_cast<std::size_t>(NoField::Count);
constexpr std::array<char const*, NO_FIELDS> NO_FIELD_NAMES = {
    "no_field_needed", "no_source", "no_credit", "no_spawn", "other_map", "out_of_radius", "too_risky"};

inline char const* NoFieldName(NoField why) { return NO_FIELD_NAMES[static_cast<std::size_t>(why)]; }

struct NeedSource
{
    FieldKind kind{FieldKind::Creature};
    std::uint32_t entry{0};
    float chance{1.0f};  // credits per target: an item's chance per loot, 1 otherwise
};

// WQ17b: where the index places an objective (an AreaPoint: a group of spawns or a POI's points)
struct NeedPlace
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float radius{0.0f};
};

struct Need
{
    std::uint32_t quest{0};
    std::uint32_t objective{HAND_IN};  // QuestExec::ObjectiveAt's index (the event's use after the objectives), or
                                       // HAND_IN, TAKE
    NeedKind kind{NeedKind::Kill};
    std::vector<NeedSource> sources;  // the spawns that give its credits
    std::uint32_t left{0};
    float worthPerCredit{0.0f};     // the credits' share of the quest's worth over them; a hand-in's or take's share
    float seconds{0.0f};            // per credit: the circuit's fixed seconds
    std::vector<NeedPlace> places;  // WQ17b: the objective's places, used when no spawn places it (UsesPlaces)
};

// A bot's prices: the circuit's weights (CircuitWeights; its defaults are neutral) and the class quests' unlock bonus
struct Pricing
{
    CircuitWeights weights{};
    std::span<std::pair<std::uint32_t, float> const> unlockBonus{};  // (quest, bonus XP), sorted
};

inline float UnlockBonusOf(Pricing const& pricing, std::uint32_t quest)
{
    auto const it = std::lower_bound(pricing.unlockBonus.begin(), pricing.unlockBonus.end(), quest,
                                     [](std::pair<std::uint32_t, float> const& entry, std::uint32_t id)
                                     { return entry.first < id; });
    return it != pricing.unlockBonus.end() && it->first == quest ? it->second : 0.0f;
}

// The quest's worth as the circuit prices it (Circuit's Value)
inline float QuestWorth(QuestRecord const& quest, Pricing const& pricing)
{
    return (static_cast<float>(quest.xpBase) + UnlockBonusOf(pricing, quest.id)) * pricing.weights.xp +
           static_cast<float>(std::max(quest.copperReward, 0)) * pricing.weights.copper;
}

namespace Detail
{
// An objective a need comes from: not the turn-in POI among the objectives, not an item the quest gives when taken
inline bool Worked(QuestRecord const& quest, std::uint32_t at)
{
    QuestObjective const& objective = QuestExec::ObjectiveAt(quest, at);
    return !(at < quest.objectives.size() && objective.kind == ObjectiveKind::Turnin) && !objective.given;
}

// The quest's credits before the hand-in: each worked objective's count (at least 1)
inline std::uint32_t ObjectiveCredits(QuestRecord const& quest)
{
    std::uint32_t credits = 0;
    for (std::uint32_t at = 0; at < QuestExec::ObjectiveCount(quest); ++at)
        if (Worked(quest, at))
            credits += std::max<std::uint32_t>(QuestExec::ObjectiveAt(quest, at).count, 1);
    return credits;
}

// As the circuit counts it: kills and objects from the slot's counters; items, explores and uses at their count (the
// snapshot holds no bag counts)
inline std::uint32_t LeftOf(QuestObjective const& objective, QuestSlot const& slot)
{
    if ((objective.kind == ObjectiveKind::Kill || objective.kind == ObjectiveKind::Object) && objective.poiIndex >= 0 &&
        objective.poiIndex < Circuit::KILL_SLOTS)
    {
        std::uint32_t const done = slot.counters[static_cast<std::size_t>(objective.poiIndex)];
        return done >= objective.count ? 0 : objective.count - done;
    }
    return std::max<std::uint32_t>(objective.count, 1);
}

inline float SecondsOf(QuestObjective const& objective)
{
    float const seconds = Circuit::ObjectiveSeconds(objective.kind);
    return seconds > 0.0f ? seconds : Circuit::OBJECT_SECONDS;  // the event's use (kind Turnin)
}

inline NeedKind KindOf(QuestExec::Work work)
{
    switch (work)
    {
        case QuestExec::Work::Kill:
            return NeedKind::Kill;
        case QuestExec::Work::Loot:
            return NeedKind::Loot;
        case QuestExec::Work::Chest:
            return NeedKind::Chest;
        case QuestExec::Work::Object:
            return NeedKind::Object;
        case QuestExec::Work::Use:
            return NeedKind::Use;
        case QuestExec::Work::Cast:
            return NeedKind::Cast;
        case QuestExec::Work::Explore:
            return NeedKind::Explore;
        case QuestExec::Work::Talk:
            return NeedKind::Talk;
        default:
            return NeedKind::Other;
    }
}

inline void AddSource(std::vector<NeedSource>& sources, FieldKind kind, std::uint32_t entry, float chance)
{
    for (NeedSource& source : sources)
        if (source.kind == kind && source.entry == entry)
        {
            source.chance = std::max(source.chance, chance);
            return;
        }
    sources.push_back({kind, entry, chance});
}

// An item's sources above the chance floor, as the executor works it: the corpses, or the skins when no corpse
// drops it (SeekEntries' rule); a chest's objects
inline std::vector<NeedSource> ItemSources(QuestIndex const& index, std::uint32_t item, bool chest)
{
    std::vector<ItemSource> const all = index.SourcesOf(item);
    bool const corpses =
        std::any_of(all.begin(), all.end(), [](ItemSource const& source)
                    { return source.kind == SourceKind::Corpse && source.chance >= SOURCE_CHANCE_FLOOR; });
    SourceKind const wanted = chest ? SourceKind::Object : (corpses ? SourceKind::Corpse : SourceKind::Skin);
    std::vector<NeedSource> sources;
    for (ItemSource const& source : all)
        if (source.kind == wanted && source.chance >= SOURCE_CHANCE_FLOOR)
            AddSource(sources, chest ? FieldKind::Object : FieldKind::Creature, source.entry,
                      std::min(source.chance, 1.0f));
    return sources;
}

// The use rule the index attached to the objective (QuestIndex's attach: the first rule that fits, in table order)
inline QuestUseRule const* UseRuleOf(QuestIndex const& index, QuestRecord const& quest, std::uint32_t at)
{
    std::size_t const size = quest.objectives.size();
    for (auto [rule, last] = index.uses.RulesFor(quest.id); rule != last; ++rule)
    {
        if (rule->objective < size)
        {
            if (rule->objective == at)
                return &*rule;
            continue;
        }
        if (quest.event == QuestEvent::Script)
        {
            if (at == size)
                return &*rule;
            continue;
        }
        if (at >= size)
            continue;
        QuestObjective const& objective = quest.objectives[at];
        if ((objective.kind == ObjectiveKind::Kill || objective.kind == ObjectiveKind::Item) &&
            objective.placeFrom != PlaceFrom::Target && objective.placeFrom != PlaceFrom::Sources)
            return &*rule;
    }
    return nullptr;
}

inline std::vector<NeedSource> SourcesOf(QuestIndex const& index, QuestRecord const& quest, std::uint32_t at,
                                         NeedKind kind)
{
    QuestObjective const& objective = QuestExec::ObjectiveAt(quest, at);
    std::vector<NeedSource> sources;
    switch (kind)
    {
        case NeedKind::Kill:
        case NeedKind::Talk:
            for (std::uint32_t const entry : QuestExec::KillEntries(objective))
                AddSource(sources, FieldKind::Creature, entry, 1.0f);
            break;
        case NeedKind::Loot:
        case NeedKind::Chest:
            sources = ItemSources(index, objective.target, kind == NeedKind::Chest);
            break;
        case NeedKind::Object:
            AddSource(sources, FieldKind::Object, objective.target, 1.0f);
            break;
        case NeedKind::Use:
        case NeedKind::Cast:
            if (QuestUseRule const* rule = UseRuleOf(index, quest, at))
                for (std::uint32_t const entry : rule->entries)
                    AddSource(sources,
                              rule->kind == UseKind::Object || rule->kind == UseKind::Focus ? FieldKind::Object
                                                                                            : FieldKind::Creature,
                              entry, 1.0f);
            break;
        default:
            break;
    }
    return sources;
}

// The objective's places a bot of the side may use (AreaPoint::notTeams); the skill an area asks is not checked here
inline std::vector<NeedPlace> PlacesOf(QuestObjective const& objective, std::uint8_t team)
{
    std::vector<NeedPlace> places;
    for (AreaPoint const& area : objective.areas)
        if (team >= SIDES || !(area.notTeams & (1u << team)))
            places.push_back({area.mapId, area.x, area.y, area.radius});
    return places;
}
}  // namespace Detail

// WQ17b: a quest's worth by step: the take's, each credit's (the credits' share spread evenly over them) and the
// hand-in's; they sum to QuestWorth
struct QuestShares
{
    float take{0.0f};
    float perCredit{0.0f};
    float handIn{0.0f};
    std::uint32_t credits{0};  // Detail::ObjectiveCredits
};

inline QuestShares SharesOf(QuestRecord const& quest, Pricing const& pricing)
{
    float const worth = QuestWorth(quest, pricing);
    QuestShares shares;
    shares.credits = Detail::ObjectiveCredits(quest);
    shares.take = worth * TAKE_SHARE;
    if (shares.credits)
    {
        shares.perCredit = worth * CREDITS_SHARE / static_cast<float>(shares.credits);
        shares.handIn = worth * HAND_IN_SHARE;
    }
    else
        shares.handIn = worth * (1.0f - TAKE_SHARE);
    return shares;
}

// Whether the need is worked at its places, not at a field of spawns: its kind needs no spawns, or none of its
// entries spawns on the field map. Places it has none of leave it a named gap
inline bool UsesPlaces(FieldMap const& map, Need const& need)
{
    if (need.places.empty())
        return false;
    if (Fieldless(need.kind))
        return true;
    return std::none_of(need.sources.begin(), need.sources.end(),
                        [&map](NeedSource const& source) { return !map.CellsOf(source.kind, source.entry).empty(); });
}

// Every open objective of the log as a need, and a ready quest as its hand-in. A quest the capability table refuses for
// the bot's side gives none; a slot of a quest the index lacks is passed over
inline std::vector<Need> Needs(QuestIndex const& index, std::span<QuestSlot const> log, std::uint8_t team,
                               Pricing const& pricing)
{
    std::vector<Need> needs;
    for (QuestSlot const& slot : log)
    {
        QuestRecord const* quest = slot.quest ? index.Find(slot.quest) : nullptr;
        if (!quest || RefusedFor(*quest, team))
            continue;
        QuestShares const shares = SharesOf(*quest, pricing);
        if (slot.state & Circuit::SLOT_COMPLETE)
        {
            Need need{quest->id, HAND_IN, NeedKind::HandIn, {}, 1, shares.handIn, Circuit::TURNIN_SECONDS, {}};
            for (std::uint32_t const at : quest->enders)
                if (at < index.places.size())
                    Detail::AddSource(need.sources, index.places[at].isObject ? FieldKind::Object : FieldKind::Creature,
                                      index.places[at].entry, 1.0f);
            needs.push_back(std::move(need));
            continue;
        }
        for (std::uint32_t at = 0; at < QuestExec::ObjectiveCount(*quest); ++at)
        {
            if (!Detail::Worked(*quest, at))
                continue;
            QuestObjective const& objective = QuestExec::ObjectiveAt(*quest, at);
            std::uint32_t const left = Detail::LeftOf(objective, slot);
            if (!left)
                continue;
            NeedKind const kind = Detail::KindOf(QuestExec::WorkOf(objective, team));
            needs.push_back({quest->id, at, kind, Detail::SourcesOf(index, *quest, at, kind), left, shares.perCredit,
                             Detail::SecondsOf(objective), Detail::PlacesOf(objective, team)});
        }
    }
    return needs;
}

// WQ17b: the take of a quest as a need at its givers (on that map, any map when none): one credit worth the take's
// share. None for a quest without a giver there (one only an item starts) or whose givers are hostile to the side or
// spawned outside the default phase. Not asked whether the bot may take it (TakeNeeds is)
inline std::optional<Need> TakeNeed(QuestIndex const& index, QuestRecord const& quest, std::uint8_t team,
                                    Pricing const& pricing, std::optional<std::uint32_t> mapId = std::nullopt)
{
    Need need{quest.id, TAKE, NeedKind::Take, {}, 1, SharesOf(quest, pricing).take, TAKE_SECONDS, {}};
    for (std::uint32_t const at : quest.givers)
    {
        if (at >= index.places.size())
            continue;
        QuestPlace const& place = index.places[at];
        if ((mapId && place.mapId != *mapId) || place.otherPhase || (team < SIDES && (place.hostileTo & (1u << team))))
            continue;
        Detail::AddSource(need.sources, place.isObject ? FieldKind::Object : FieldKind::Creature, place.entry, 1.0f);
    }
    if (need.sources.empty())
        return std::nullopt;
    return need;
}

// WQ17b: the quests given on the map near a point (QuestIndex::GiversNear), ascending: the candidates for TakeNeeds,
// bounded by the givers of the bot's map, not by the quest table
inline std::vector<std::uint32_t> TakeCandidates(QuestIndex const& index, std::uint32_t mapId, float x, float y,
                                                 float yards)
{
    std::vector<std::uint32_t> ids;
    for (std::uint32_t const place : index.GiversNear(mapId, x, y, yards))
        for (std::uint32_t const quest : index.QuestsGivenAt(place))
            ids.push_back(quest);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// WQ17b: the exclusive groups of the log's and the rewarded quests, ascending (Take::Facts::takenGroups; the circuit
// builds its own the same way)
inline std::vector<std::int32_t> TakenGroups(QuestIndex const& index, std::span<std::uint32_t const> inLog,
                                             std::span<std::uint32_t const> rewarded)
{
    std::vector<std::int32_t> groups;
    for (std::span<std::uint32_t const> const ids : {inLog, rewarded})
        for (std::uint32_t const id : ids)
            if (QuestRecord const* record = index.Find(id))
                if (record->exclusiveGroup > 0)
                    groups.push_back(record->exclusiveGroup);
    std::sort(groups.begin(), groups.end());
    return groups;
}

// WQ17b: every candidate quest the take rules allow the bot (Take::May, no elite or group quest: WQ.43 point 3) as a
// need at its givers on the map. A separate list from the log's Needs; a caller ranking both concatenates them
inline std::vector<Need> TakeNeeds(QuestIndex const& index, Take::Facts const& facts, std::uint32_t mapId,
                                   std::span<std::uint32_t const> candidates, Pricing const& pricing)
{
    Take::Facts bot = facts;
    bot.eliteGroup = false;
    std::uint8_t const team = SideOfRace(facts.race);
    std::vector<std::uint32_t> ids(candidates.begin(), candidates.end());
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<Need> needs;
    for (std::uint32_t const id : ids)
    {
        QuestRecord const* quest = index.Find(id);
        if (!quest || Take::May(*quest, bot, index))
            continue;
        if (std::optional<Need> need = TakeNeed(index, *quest, team, pricing, mapId))
            needs.push_back(std::move(*need));
    }
    return needs;
}

// What a field is asked about: the bot (map, point, level, speed, caution, group) and the stay
struct FieldAsk
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float radius{FIELD_WALK_YARDS};  // TopFields: centres within this many yards
    std::uint8_t level{1};
    float stayMinutes{10.0f};
    float runSpeed{0.0f};  // yards per second; 0: Circuit::DEFAULT_RUN_SPEED
    float caution{1.0f};   // the planner's caution weight; 1 is neutral
    std::uint8_t groupSize{0};
    float crowd{1.0f};     // PLAN 3.10 part 6 scales the worth by it later; 1 for now
    std::size_t limit{0};  // TopFields keeps the best this many; 0: all
};

struct NeedOffer
{
    std::uint32_t need{0};    // into the needs
    float targets{0.0f};      // what stands plus what respawns in the stay, times the chance, within reach
    float credits{0.0f};      // capped by the count left and by what one bot does in the stay
    bool beyondReach{false};  // a row of it was left out: its creatures lie too far above the bot
};

struct Offer
{
    std::uint64_t centre{0};
    float x{0.0f};  // the centre cell's middle
    float y{0.0f};
    std::vector<NeedOffer> needs;  // the needs with a row in the field
    float worth{0.0f};             // the credits times their worth per credit, over the stay
    float worthPerMinute{0.0f};    // worth x crowd over the stay's minutes, before risk and walk
    Goals::RiskInput risk{};
};

namespace Detail
{
inline float MiddleLevel(FieldRow const& row)
{
    return (static_cast<float>(row.minLevel) + static_cast<float>(row.maxLevel)) / 2.0f;
}

// A row the bot works: an object, or creatures whose level alone keeps the weighted risk at or below the refusal. Not
// asked of a hand-in's ender: a friendly creature, often far above the quest's level (a camp's marshal)
inline bool WithinReach(FieldRow const& row, FieldAsk const& ask)
{
    float const above = std::max(MiddleLevel(row) - static_cast<float>(ask.level), 0.0f);
    return row.kind == FieldKind::Object || above * Goals::RISK_PER_LEVEL_ABOVE * ask.caution <= Goals::RISK_REFUSE;
}

inline FieldRow const* FindRow(FieldMap const& map, std::uint64_t cell, FieldKind kind, std::uint32_t entry)
{
    std::span<FieldRow const> const rows = map.RowsOf(cell);
    auto const it = std::lower_bound(rows.begin(), rows.end(), std::make_pair(kind, entry),
                                     [](FieldRow const& row, std::pair<FieldKind, std::uint32_t> const& key)
                                     { return std::make_pair(row.kind, row.entry) < key; });
    return it != rows.end() && it->kind == kind && it->entry == entry ? &*it : nullptr;
}

inline float CentreOf(std::int32_t cell) { return (static_cast<float>(cell) + 0.5f) * FIELD_CELL_YARDS; }

// Whether the level alone keeps the weighted risk at or below the refusal (WithinReach's rule)
inline bool LevelWithinReach(float above, FieldAsk const& ask)
{
    return std::max(above, 0.0f) * Goals::RISK_PER_LEVEL_ABOVE * ask.caution <= Goals::RISK_REFUSE;
}

// Whether one of the need's places lies in the field around the centre cell (its cell or a neighbour)
inline bool PlaceInField(Need const& need, std::uint32_t mapId, std::int32_t cellX, std::int32_t cellY)
{
    for (NeedPlace const& place : need.places)
    {
        if (place.mapId != mapId)
            continue;
        std::uint64_t const key = FieldCellKeyAt(place.mapId, place.x, place.y);
        if (std::abs(FieldCellXOf(key) - cellX) <= 1 && std::abs(FieldCellYOf(key) - cellY) <= 1)
            return true;
    }
    return false;
}
}  // namespace Detail

// The field around a centre cell for the needs and a stay: per need the targets of its entries there plus those that
// respawn in the stay, times the chance, capped by the count left and by the stay over the need's seconds (a need
// worked at its places: its count left when a place lies in the field); the worth per minute; and the risk inputs: how
// far the needed creatures (rows within reach) and the cells' other hostile creatures (the lowest of each cell) lie
// above the bot, and the larger of the two elite shares. WQ17b: a cell beyond reach without a needed row beyond reach
// and with a small share of the field's hostiles (a lone rare) adds them to the elite share and not to the level
inline Offer FieldOffer(FieldMap const& map, std::span<Need const> needs, std::uint64_t centre, FieldAsk const& ask)
{
    Offer offer;
    offer.centre = centre;
    std::uint32_t const mapId = FieldMapOf(centre);
    std::int32_t const cellX = FieldCellXOf(centre);
    std::int32_t const cellY = FieldCellYOf(centre);
    offer.x = Detail::CentreOf(cellX);
    offer.y = Detail::CentreOf(cellY);
    float const stay = ask.stayMinutes * SECONDS_PER_MINUTE;
    if (!(stay > 0.0f))
        return offer;
    std::array<FieldCell const*, 9> cells{};
    std::size_t cellCount = 0;
    for (std::int32_t dx = -1; dx <= 1; ++dx)
        for (std::int32_t dy = -1; dy <= 1; ++dy)
            if (FieldCell const* cell = map.Cell(FieldCellKey(mapId, cellX + dx, cellY + dy)))
                cells[cellCount++] = cell;
    float const level = static_cast<float>(ask.level);
    float neededAbove = 0.0f;
    float neededSpawns = 0.0f;
    float neededElites = 0.0f;
    std::array<bool, 9> neededBeyond{};  // a cell holds a needed creature row beyond the bot's reach
    for (std::uint32_t i = 0; i < needs.size(); ++i)
    {
        Need const& need = needs[i];
        float const byTime = need.seconds > 0.0f ? stay / need.seconds : static_cast<float>(need.left);
        if (UsesPlaces(map, need))
        {
            if (!Detail::PlaceInField(need, mapId, cellX, cellY))
                continue;
            NeedOffer one{i};
            one.targets = static_cast<float>(need.left);
            one.credits = std::min(one.targets, byTime);
            offer.worth += one.credits * need.worthPerCredit;
            offer.needs.push_back(one);
            continue;
        }
        if (Fieldless(need.kind))
            continue;
        NeedOffer one{i};
        bool found = false;
        bool const atNpc = need.kind == NeedKind::HandIn || need.kind == NeedKind::Take;  // a friendly giver, ender
        for (std::size_t c = 0; c < cellCount; ++c)
            for (NeedSource const& source : need.sources)
            {
                FieldRow const* row = Detail::FindRow(map, cells[c]->key, source.kind, source.entry);
                if (!row)
                    continue;
                found = true;
                if (!atNpc && !Detail::WithinReach(*row, ask))
                {
                    one.beyondReach = true;
                    neededBeyond[c] = neededBeyond[c] || need.left > 0;
                    continue;
                }
                float const respawns = row->medianRespawn ? stay / static_cast<float>(row->medianRespawn) : 0.0f;
                one.targets += row->weight * (1.0f + respawns) * source.chance;
                if (row->kind == FieldKind::Creature && !atNpc && need.left)
                {
                    neededAbove = std::max(neededAbove, Detail::MiddleLevel(*row) - level);
                    neededSpawns += static_cast<float>(row->spawns);
                    neededElites += static_cast<float>(row->elites);
                }
            }
        if (!found)
            continue;
        one.credits = std::min({one.targets, static_cast<float>(need.left), byTime});
        offer.worth += one.credits * need.worthPerCredit;
        offer.needs.push_back(one);
    }
    float othersAbove = 0.0f;
    float hostiles = 0.0f;
    float hostileElites = 0.0f;
    for (std::size_t c = 0; c < cellCount; ++c)
        hostiles += static_cast<float>(cells[c]->hostiles);
    for (std::size_t c = 0; c < cellCount; ++c)
        if (cells[c]->hostiles)
        {
            float const above = static_cast<float>(cells[c]->hostileMinLevel) - level;
            float const count = static_cast<float>(cells[c]->hostiles);
            if (!neededBeyond[c] && !Detail::LevelWithinReach(above, ask) && count <= FEW_HOSTILES_SHARE * hostiles)
            {
                hostileElites += count;
                continue;
            }
            othersAbove = std::max(othersAbove, above);
            hostileElites += static_cast<float>(cells[c]->hostileElites);
        }
    offer.risk.levelsAbove = std::max({0.0f, neededAbove, othersAbove});
    offer.risk.eliteShare = std::max(neededSpawns > 0.0f ? neededElites / neededSpawns : 0.0f,
                                     hostiles > 0.0f ? hostileElites / hostiles : 0.0f);
    offer.worthPerMinute = offer.worth * ask.crowd / ask.stayMinutes;
    return offer;
}

struct Ranked
{
    Offer offer;
    float walkMinutes{0.0f};
    float riskFactor{1.0f};  // 1 / (1 + weighted risk), as the planner's Add
    float value{0.0f};       // worth x risk factor x crowd over the walk's and the stay's minutes
};

struct Missing
{
    std::uint32_t need{0};
    NoField why{NoField::NoSource};
};

struct Ranking
{
    std::vector<Ranked> fields;    // best first; ties by centre cell
    std::vector<Missing> noField;  // every need no ranked field gives a credit, with why
    std::uint32_t considered{0};   // centres priced
    std::uint32_t refused{0};      // of them refused for risk
};

// The fields whose centre lies within the radius of the bot, ranked by worth over walk plus stay (straight yards at run
// speed), scaled by the planner's risk term and refused above it; a need no ranked field gives a credit is in the list
// with its reason. Centres: the cells of every needed entry's rows on the map and their neighbours
inline Ranking TopFields(FieldMap const& map, std::span<Need const> needs, FieldAsk const& ask)
{
    Ranking ranking;
    std::vector<std::uint64_t> centres;
    std::vector<std::uint8_t> anyCell(needs.size(), 0);
    std::vector<std::uint8_t> onMap(needs.size(), 0);
    std::vector<std::uint8_t> inRadius(needs.size(), 0);
    std::vector<std::uint8_t> placed(needs.size(), 0);
    Circuit::Point const bot{ask.x, ask.y, 0.0f};
    auto const around = [&](std::uint32_t i, std::uint64_t key)
    {
        onMap[i] = 1;
        for (std::int32_t dx = -1; dx <= 1; ++dx)
            for (std::int32_t dy = -1; dy <= 1; ++dy)
            {
                std::int32_t const cellX = FieldCellXOf(key) + dx;
                std::int32_t const cellY = FieldCellYOf(key) + dy;
                if (Circuit::Yards(bot, {Detail::CentreOf(cellX), Detail::CentreOf(cellY), 0.0f}) > ask.radius)
                    continue;
                inRadius[i] = 1;
                centres.push_back(FieldCellKey(ask.mapId, cellX, cellY));
            }
    };
    for (std::uint32_t i = 0; i < needs.size(); ++i)
    {
        placed[i] = UsesPlaces(map, needs[i]) ? 1 : 0;
        if (!needs[i].left)
            continue;
        if (placed[i])
        {
            for (NeedPlace const& place : needs[i].places)
                if (place.mapId == ask.mapId)
                    around(i, FieldCellKeyAt(place.mapId, place.x, place.y));
            continue;
        }
        if (Fieldless(needs[i].kind))
            continue;
        for (NeedSource const& source : needs[i].sources)
            for (std::uint64_t const key : map.CellsOf(source.kind, source.entry))
            {
                anyCell[i] = 1;
                if (FieldMapOf(key) == ask.mapId)
                    around(i, key);
            }
    }
    std::sort(centres.begin(), centres.end());
    centres.erase(std::unique(centres.begin(), centres.end()), centres.end());
    std::vector<std::uint8_t> covered(needs.size(), 0);
    std::vector<std::uint8_t> risky(needs.size(), 0);
    float const speed = ask.runSpeed > 0.0f ? ask.runSpeed : Circuit::DEFAULT_RUN_SPEED;
    for (std::uint64_t const centre : centres)
    {
        Offer offer = FieldOffer(map, needs, centre, ask);
        ++ranking.considered;
        for (NeedOffer const& one : offer.needs)
            risky[one.need] |= one.beyondReach ? 1 : 0;
        if (!(offer.worth > 0.0f))
            continue;
        float const weighted = Goals::RawRisk(offer.risk, ask.groupSize) * ask.caution;
        if (weighted > Goals::RISK_REFUSE)
        {
            ++ranking.refused;
            for (NeedOffer const& one : offer.needs)
                risky[one.need] |= one.credits > 0.0f ? 1 : 0;
            continue;
        }
        Ranked field;
        field.walkMinutes = Circuit::Yards(bot, {offer.x, offer.y, 0.0f}) / speed / SECONDS_PER_MINUTE;
        field.riskFactor = 1.0f / (1.0f + weighted);
        field.value = offer.worth * field.riskFactor * ask.crowd / (field.walkMinutes + ask.stayMinutes);
        for (NeedOffer const& one : offer.needs)
            covered[one.need] |= one.credits > 0.0f ? 1 : 0;
        field.offer = std::move(offer);
        ranking.fields.push_back(std::move(field));
    }
    std::sort(ranking.fields.begin(), ranking.fields.end(), [](Ranked const& a, Ranked const& b)
              { return a.value != b.value ? a.value > b.value : a.offer.centre < b.offer.centre; });
    if (ask.limit && ranking.fields.size() > ask.limit)
        ranking.fields.resize(ask.limit);
    for (std::uint32_t i = 0; i < needs.size(); ++i)
    {
        if (covered[i])
            continue;
        Need const& need = needs[i];
        NoField const why = placed[i]              ? (!need.left     ? NoField::NoCredit
                                                      : !onMap[i]    ? NoField::OtherMap
                                                      : !inRadius[i] ? NoField::OutOfRadius
                                                      : risky[i]     ? NoField::TooRisky
                                                                     : NoField::NoCredit)
                            : Fieldless(need.kind) ? NoField::Fieldless
                            : need.sources.empty() ? NoField::NoSource
                            : !need.left           ? NoField::NoCredit
                            : !anyCell[i]          ? NoField::NoSpawn
                            : !onMap[i]            ? NoField::OtherMap
                            : !inRadius[i]         ? NoField::OutOfRadius
                            : risky[i]             ? NoField::TooRisky
                                                   : NoField::NoCredit;
        ranking.noField.push_back({i, why});
    }
    return ranking;
}

// The census's question: whether the need has a field of its entries on one of these maps (any map when empty), and
// if not, why: none needed, no entry gives it, they spawn nowhere on the field map, or on other maps only. WQ17b: a
// need worked at its places has one when a place lies on those maps, else they are on other maps
inline std::optional<NoField> MapGap(FieldMap const& map, Need const& need, std::span<std::uint32_t const> maps)
{
    if (UsesPlaces(map, need))
    {
        for (NeedPlace const& place : need.places)
            if (maps.empty() || std::find(maps.begin(), maps.end(), place.mapId) != maps.end())
                return std::nullopt;
        return NoField::OtherMap;
    }
    if (Fieldless(need.kind))
        return NoField::Fieldless;
    if (need.sources.empty())
        return NoField::NoSource;
    bool spawned = false;
    for (NeedSource const& source : need.sources)
        for (std::uint64_t const key : map.CellsOf(source.kind, source.entry))
        {
            spawned = true;
            if (maps.empty() || std::find(maps.begin(), maps.end(), FieldMapOf(key)) != maps.end())
                return std::nullopt;
        }
    return spawned ? NoField::OtherMap : NoField::NoSpawn;
}
}  // namespace QuestFields
}  // namespace Planner

#endif
