/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of where the quest executor walks (questing WT33b, QuestPlaceRules.h): the order of PLAN 3.1's places
// (target spawns, item sources, area trigger, a POI only with a baked floor, else none), an absent and an invalid
// floor (QuestPoiZStore: not baked, off the mesh, another map), a turn-in at the nearest ender spawn and the floored
// turn-in POI only for a quest no spawn ends; and the invariant that the planner (CircuitBuilder's Locate, seen
// through the plan's tasks and turn-ins) and the executor (QuestPlaces) get the same place for the same objective,
// searched over a random fixture index. WT34 fix: an area's place is one of its members (the nearest the centroid),
// never the centroid, which can lie inside the rock between a cave's floors or in the lake a camp stands around.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot
//   tests/quest-places-selftest.cpp
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "QuestCircuit.h"
#include "QuestPlaceRules.h"
#include "QuestPoiZStore.h"

using namespace Planner;

namespace
{
constexpr std::uint32_t MAP_A = 0;
constexpr std::uint32_t MAP_B = 1;
constexpr std::uint8_t ALLIANCE = 0;
constexpr std::uint8_t HORDE = 1;
constexpr std::uint16_t SKINNING = 393;
constexpr std::uint32_t GIVER = 1001;
constexpr std::uint32_t INNKEEPER = 1002;  // ends quest 9 on two floors' worth of places: map A twice, map B once
constexpr std::uint32_t WOLF = 5000;
constexpr std::uint32_t TRIGGER = 77;
constexpr float INF_F = std::numeric_limits<float>::infinity();

int passed = 0;
int failed = 0;

void Check(bool pass, std::string const& text)
{
    if (pass)
        ++passed;
    else
    {
        ++failed;
        std::printf("FAIL: %s\n", text.c_str());
    }
}

bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

QuestRecord Quest(std::uint32_t id)
{
    QuestRecord quest;
    quest.id = id;
    quest.level = 10;
    quest.minLevel = 1;
    quest.xpBase = 100;
    return quest;
}

QuestObjective Objective(ObjectiveKind kind, std::uint32_t target, std::int32_t poiIndex)
{
    return {kind, target, 4, poiIndex, {}};
}

auto const noSkill = [](std::uint16_t) { return false; };
auto const anySkill = [](std::uint16_t) { return true; };

struct Fixture
{
    QuestGather in;

    void Add(QuestRecord quest) { in.quests.push_back(std::move(quest)); }
    void Relate(std::uint32_t quest, std::uint32_t entry, bool ender)
    {
        in.relations.push_back({quest, entry, false, ender});
    }
    void Spawn(std::uint32_t entry, std::uint32_t map, float x, float y, float z)
    {
        in.places.push_back({map, x, y, z, entry, false, 12, 87});
    }
    void Target(std::uint32_t entry, float x, float y, float z)
    {
        in.targets.push_back({entry, false, MAP_A, x, y, z});
    }
    void Poi(std::uint32_t quest, std::int32_t slot, std::vector<std::pair<float, float>> points,
             std::vector<float> floors, std::uint32_t map = MAP_A)
    {
        in.pois.push_back({quest, slot, map, std::move(points), std::move(floors), 0});
    }
    // An item `item` dropped by creature `entry` (100 %; a skinning source with kind Skin), its spawns given separately
    void Source(std::uint32_t entry, std::uint32_t item, SourceKind kind = SourceKind::Corpse)
    {
        LootRow row;
        row.lootId = entry;
        row.item = item;
        row.chance = 100.0f;
        in.sources.tables[static_cast<std::size_t>(kind)].push_back(row);
        in.sources.owners.push_back({kind, entry, entry});
    }
    void SourceSpawn(std::uint32_t entry, float x, float y, float z, std::uint16_t skill = 0,
                     std::uint8_t friendlyTo = 0)
    {
        Planner::SourceSpawn s{entry, false, MAP_A, x, y, z};
        s.skill = skill;
        s.friendlyTo = friendlyTo;
        in.sources.spawns.push_back(s);
    }
};

// What GatherQuests does with a POI's points: each point's floor from the store, NO_FLOOR where it has none
std::vector<float> FloorsOf(QuestPoiZStore const& store, std::uint32_t quest, std::uint32_t poiId, std::uint32_t map,
                            std::uint32_t points)
{
    std::vector<float> floors;
    for (std::uint32_t i = 0; i < points; ++i)
    {
        std::optional<float> const floor = store.Floor(quest, poiId, i, map);
        floors.push_back(floor ? *floor : NO_FLOOR);
    }
    return floors;
}

QuestPoiZStore::Candidate Row(std::uint16_t point, float z, std::uint8_t rank, std::uint8_t confidence, bool navmeshOk,
                              std::uint16_t map = MAP_A, bool trigger = false)
{
    QuestPoiZStore::Candidate c;
    c.pointIdx = point;
    c.z = z;
    c.rank = rank;
    c.confidence = confidence;
    c.navmeshOk = navmeshOk;
    c.mapId = map;
    c.trigger = trigger;
    return c;
}

// The store: rank, then confidence, then trigger evidence; an absent row and an invalid one (off the mesh, another
// map) are no floor
QuestPoiZStore Store()
{
    QuestPoiZStore store;
    store.Add(8, 3, Row(0, 50.0f, 2, 90, true));  // a lower-ranked level under the upper floor
    store.Add(8, 3, Row(0, 64.0f, 1, 40, true));
    store.Add(8, 3, Row(1, 12.0f, 1, 90, false));        // off the mesh: invalid
    store.Add(8, 3, Row(2, 30.0f, 1, 90, true, MAP_B));  // baked for another map: invalid on map A
    store.Add(8, 3, Row(3, 20.0f, 1, 50, true));
    store.Add(8, 3, Row(3, 25.0f, 1, 80, true));  // same rank, higher confidence wins
    store.Add(8, 3, Row(4, 70.0f, 1, 60, true));
    store.Add(8, 3, Row(4, 5.0f, 1, 60, true, MAP_A, true));  // same rank and confidence: trigger evidence wins
    store.Order();
    return store;
}

void StoreRules(QuestPoiZStore const& store)
{
    Check(store.Floor(8, 3, 0, MAP_A) == 64.0f, "rank 1 beats rank 2 whatever the confidence");
    Check(!store.Floor(8, 3, 1, MAP_A), "an invalid floor (off the mesh) is no floor");
    Check(!store.Floor(8, 3, 2, MAP_A) && store.Floor(8, 3, 2, MAP_B) == 30.0f,
          "a floor baked for another map is no floor on this one");
    Check(store.Floor(8, 3, 3, MAP_A) == 25.0f, "at equal rank the higher confidence wins");
    Check(store.Floor(8, 3, 4, MAP_A) == 5.0f, "at equal rank and confidence the trigger evidence wins");
    Check(!store.Floor(8, 3, 9, MAP_A) && !store.Floor(8, 4, 0, MAP_A) && !store.Floor(99, 3, 0, MAP_A),
          "an absent floor (point, POI or quest not baked) is no floor");
    Check(QuestPoiZStore().Pois() == 0 && !QuestPoiZStore().Floor(8, 3, 0, MAP_A), "an empty table: no floor at all");
    std::vector<float> const floors = FloorsOf(store, 8, 3, MAP_A, 5);
    Check(floors.size() == 5 && floors[0] == 64.0f && std::isnan(floors[1]) && std::isnan(floors[2]) &&
              floors[3] == 25.0f && floors[4] == 5.0f,
          "the gather's floors per point: the store's, NO_FLOOR where it has none");
}

QuestIndex Designed(QuestPoiZStore const& store)
{
    Fixture f;
    for (std::uint32_t q = 1; q <= 14; ++q)
        f.Relate(q, GIVER, false);
    f.Spawn(GIVER, MAP_A, 100.0f, 100.0f, 10.0f);

    // 1: a kill objective with target spawns and a floored POI: the spawns, with their height
    QuestRecord q1 = Quest(1);
    q1.objectives.push_back(Objective(ObjectiveKind::Kill, WOLF, 0));
    f.Add(q1);
    for (int i = 0; i < 3; ++i)
        f.Target(WOLF, 1000.0f + 10.0f * static_cast<float>(i), 1000.0f, 30.0f);
    f.Poi(1, 0, {{1000.0f, 1000.0f}}, {99.0f});
    // 2: an item from a creature source and a floored POI: the source's spawns
    QuestRecord q2 = Quest(2);
    q2.objectives.push_back(Objective(ObjectiveKind::Item, 9002, POI_FIRST_ITEM));
    f.Add(q2);
    f.Source(5002, 9002);
    f.SourceSpawn(5002, 2000.0f, 1000.0f, 20.0f);
    f.Poi(2, POI_FIRST_ITEM, {{2000.0f, 1000.0f}}, {99.0f});
    // 3: an area trigger: its own place, but not one the executor walks to
    f.Add(Quest(3));  // the trigger makes its Explore objective
    f.in.triggers.push_back({3, TRIGGER, MAP_A, 300.0f, 300.0f, 12.0f, 10.0f});
    // 4: an item with only a POI, floored on an upper floor: rule 5, the floor's height
    QuestRecord q4 = Quest(4);
    q4.objectives.push_back(Objective(ObjectiveKind::Item, 9004, POI_FIRST_ITEM));
    f.Add(q4);
    f.Poi(4, POI_FIRST_ITEM, {{500.0f, 500.0f}, {520.0f, 500.0f}, {520.0f, 520.0f}}, {64.0f, 64.0f, 64.0f});
    // 5: only a POI without a floor; 6: its floors missing altogether; 7: one point floored, one infinite, one none
    for (std::uint32_t q = 5; q <= 7; ++q)
    {
        QuestRecord quest = Quest(q);
        quest.objectives.push_back(Objective(ObjectiveKind::Item, 9000 + q, POI_FIRST_ITEM));
        f.Add(quest);
    }
    f.Poi(5, POI_FIRST_ITEM, {{600.0f, 600.0f}, {620.0f, 600.0f}}, {NO_FLOOR, NO_FLOOR});
    f.Poi(6, POI_FIRST_ITEM, {{600.0f, 600.0f}, {620.0f, 600.0f}}, {});
    f.Poi(7, POI_FIRST_ITEM, {{600.0f, 600.0f}, {620.0f, 600.0f}, {640.0f, 600.0f}}, {NO_FLOOR, 40.0f, INF_F});
    // 8: a POI floored from the store (points 0 and 3, 4 floored; 1 off the mesh, 2 for another map)
    QuestRecord q8 = Quest(8);
    q8.objectives.push_back(Objective(ObjectiveKind::Item, 9008, POI_FIRST_ITEM));
    f.Add(q8);
    f.Poi(8, POI_FIRST_ITEM, {{800.0f, 800.0f}, {900.0f, 800.0f}, {900.0f, 900.0f}, {800.0f, 900.0f}, {810.0f, 810.0f}},
          FloorsOf(store, 8, 3, MAP_A, 5));
    // 9: an ender inside an inn (57) under the floored turn-in POI on its roof (71.7), a second spawn far out and a
    // third on map B: the nearest ender spawn on the bot's map
    QuestRecord q9 = Quest(9);
    q9.objectives.push_back(Objective(ObjectiveKind::Kill, WOLF, 0));
    f.Add(q9);
    f.Relate(9, INNKEEPER, true);
    f.Spawn(INNKEEPER, MAP_A, 150.0f, 150.0f, 57.0f);
    f.Spawn(INNKEEPER, MAP_A, 3000.0f, 3000.0f, 10.0f);
    f.Spawn(INNKEEPER, MAP_B, 50.0f, 50.0f, 5.0f);
    f.Poi(9, POI_TURNIN, {{150.0f, 150.0f}}, {71.7f});
    // 10: no ender spawn, a floored turn-in POI: the POI; 11: no ender, an unfloored turn-in POI: no turn-in place
    for (std::uint32_t q = 10; q <= 11; ++q)
    {
        QuestRecord quest = Quest(q);
        quest.objectives.push_back(Objective(ObjectiveKind::Kill, WOLF, 0));
        f.Add(quest);
    }
    f.Poi(10, POI_TURNIN, {{400.0f, 100.0f}}, {33.0f});
    f.Poi(11, POI_TURNIN, {{400.0f, 100.0f}}, {NO_FLOOR});
    // 12: three sources: a skinning one at 700, one friendly to the Alliance at 900, a plain one far out at 3000
    QuestRecord q12 = Quest(12);
    q12.objectives.push_back(Objective(ObjectiveKind::Item, 9012, POI_FIRST_ITEM));
    f.Add(q12);
    f.Source(5012, 9012, SourceKind::Skin);
    f.Source(5013, 9012);
    f.Source(5014, 9012);
    f.SourceSpawn(5012, 700.0f, 0.0f, 1.0f, SKINNING);
    f.SourceSpawn(5013, 900.0f, 0.0f, 2.0f, 0, 1u << ALLIANCE);
    f.SourceSpawn(5014, 3000.0f, 0.0f, 3.0f);
    // 13: a kill in slot 0 (wolves at 1000) and an item in slot 4 (the source at 2000): the open ones, nearest first
    QuestRecord q13 = Quest(13);
    q13.objectives.push_back(Objective(ObjectiveKind::Kill, WOLF, 0));
    q13.objectives.push_back(Objective(ObjectiveKind::Item, 9002, POI_FIRST_ITEM));
    f.Add(q13);
    // 14: two groups of a target, the nearer one wins
    QuestRecord q14 = Quest(14);
    q14.objectives.push_back(Objective(ObjectiveKind::Kill, 5100, 0));
    f.Add(q14);
    for (int i = 0; i < 3; ++i)
        f.Target(5100, 2000.0f, 2000.0f + 10.0f * static_cast<float>(i), 7.0f);
    f.Target(5100, 200.0f, 200.0f, 8.0f);
    // 15: a camp outside (z 90) over a cave (z 50) in one cluster: the mean height (70) is inside the rock
    QuestRecord q15 = Quest(15);
    q15.objectives.push_back(Objective(ObjectiveKind::Kill, 5150, 0));
    f.Add(q15);
    for (int i = 0; i < 4; ++i)
    {
        f.Target(5150, 4000.0f + 10.0f * static_cast<float>(i), 1000.0f, 90.0f);
        f.Target(5150, 4005.0f + 10.0f * static_cast<float>(i), 1004.0f, 50.0f);
    }
    // 16: an item whose sources stand in a ring of 60 yd around a lake: the mean point is the lake's middle
    QuestRecord q16 = Quest(16);
    q16.objectives.push_back(Objective(ObjectiveKind::Item, 9016, POI_FIRST_ITEM));
    f.Add(q16);
    f.Source(5160, 9016);
    for (int i = 0; i < 8; ++i)
    {
        float const angle = static_cast<float>(i) * 0.785398f;
        f.SourceSpawn(5160, 5000.0f + 60.0f * std::cos(angle), 1000.0f + 60.0f * std::sin(angle),
                      20.0f + static_cast<float>(i));
    }
    return QuestIndex::Build(f.in);
}

AreaPoint const* PlaceOf(QuestIndex const& idx, std::uint32_t quest, std::uint32_t map, float x, float y,
                         std::uint8_t team = ALLIANCE)
{
    return QuestPlaces::PlaceOf(idx.Find(quest)->objectives[0], map, x, y, team, noSkill);
}

void Order(QuestIndex const& idx)
{
    AreaPoint const* wolves = PlaceOf(idx, 1, MAP_A, 100.0f, 100.0f);
    Check(idx.Find(1)->objectives[0].placeFrom == PlaceFrom::Target && wolves && Near(wolves->z, 30.0f) &&
              Near(wolves->x, 1010.0f),
          "1. target spawns beat a floored POI, with the spawns' height");
    AreaPoint const* source = PlaceOf(idx, 2, MAP_A, 100.0f, 100.0f);
    Check(idx.Find(2)->objectives[0].placeFrom == PlaceFrom::Sources && source && Near(source->z, 20.0f),
          "2. item sources beat a floored POI");
    AreaPoint const* trigger = PlaceOf(idx, 3, MAP_A, 100.0f, 100.0f);
    Check(idx.Find(3)->objectives[0].placeFrom == PlaceFrom::Trigger && trigger && Near(trigger->z, 12.0f) &&
              QuestPlaces::OpenPlaces(
                  *idx.Find(3), MAP_A, 100.0f, 100.0f, ALLIANCE, [](std::int32_t) { return true; }, noSkill)
                  .empty(),
          "3. an area trigger is its objective's place, but the executor does not walk to it");
    AreaPoint const* floor = PlaceOf(idx, 4, MAP_A, 100.0f, 100.0f);
    Check(idx.Find(4)->objectives[0].placeFrom == PlaceFrom::Poi && floor && Near(floor->z, 64.0f) &&
              Near(floor->x, 520.0f) && Near(floor->y, 500.0f),
          "5. a POI with a baked floor: its point nearest the centre, at the floor's height (an upper floor, not the "
          "roof)");
    for (std::uint32_t q = 5; q <= 6; ++q)
        Check(idx.Find(q)->objectives[0].placeFrom == PlaceFrom::None && idx.Find(q)->objectives[0].areas.empty() &&
                  idx.Find(q)->objectives[0].polygon && !PlaceOf(idx, q, MAP_A, 600.0f, 600.0f) &&
                  QuestPlaces::OpenPlaces(
                      *idx.Find(q), MAP_A, 600.0f, 600.0f, ALLIANCE, [](std::int32_t) { return true; }, noSkill)
                      .empty(),
              "6. quest " + std::to_string(q) + ": a POI without a floor is no place (the executor ends no_place)");
    AreaPoint const* partial = PlaceOf(idx, 7, MAP_A, 600.0f, 600.0f);
    Check(partial && partial->points == 1 && Near(partial->x, 620.0f) && Near(partial->z, 40.0f),
          "a POI's place is made of its floored points only (NaN and infinite floors are none)");
    AreaPoint const* stored = PlaceOf(idx, 8, MAP_A, 100.0f, 100.0f);
    Check(stored && stored->points == 3 && Near(stored->x, 810.0f) && Near(stored->z, 5.0f),
          "the store's floors reach the index: the points without a valid floor drop out (point 4 is nearest the "
          "centre of 0, 3 and 4, at its own floor)");
    Check(!PlaceOf(idx, 1, MAP_B, 100.0f, 100.0f), "no place on another map");
    AreaPoint const* nearGroup = PlaceOf(idx, 14, MAP_A, 100.0f, 100.0f);
    AreaPoint const* farGroup = PlaceOf(idx, 14, MAP_A, 2000.0f, 1900.0f);
    Check(nearGroup && Near(nearGroup->x, 200.0f) && Near(nearGroup->z, 8.0f) && farGroup &&
              Near(farGroup->x, 2000.0f) && Near(farGroup->z, 7.0f),
          "the nearest area wins, not the largest");
}

void Usable(QuestIndex const& idx)
{
    QuestObjective const& objective = idx.Find(12)->objectives[0];
    auto const at = [&objective](std::uint8_t team, bool skinner)
    {
        AreaPoint const* area = skinner ? QuestPlaces::PlaceOf(objective, MAP_A, 650.0f, 0.0f, team, anySkill)
                                        : QuestPlaces::PlaceOf(objective, MAP_A, 650.0f, 0.0f, team, noSkill);
        return area ? area->x : -1.0f;
    };
    Check(Near(at(ALLIANCE, true), 700.0f), "a skinner takes the skinning source");
    Check(Near(at(HORDE, false), 900.0f), "a bot without the skill skips it; the Horde may kill the Alliance's friend");
    Check(Near(at(ALLIANCE, false), 3000.0f), "an Alliance bot without the skill walks to the plain source far out");
}

void TurnIns(QuestIndex const& idx)
{
    QuestRecord const& inn = *idx.Find(9);
    QuestObjective const& turnin = inn.objectives.back();
    Check(turnin.kind == ObjectiveKind::Turnin && turnin.placeFrom == PlaceFrom::Target && turnin.areas.size() == 3,
          "a quest with ender spawns: its turn-in is at them, one area each");
    AreaPoint const* nearInn = QuestPlaces::TurnInPlace(inn, MAP_A, 200.0f, 200.0f);
    Check(nearInn && Near(nearInn->x, 150.0f) && Near(nearInn->z, 57.0f),
          "the turn-in at the nearest ender spawn, at its height (the inn's floor, not the roof POI)");
    AreaPoint const* farInn = QuestPlaces::TurnInPlace(inn, MAP_A, 2900.0f, 2900.0f);
    Check(farInn && Near(farInn->x, 3000.0f) && Near(farInn->z, 10.0f), "from far out the other spawn is nearer");
    AreaPoint const* otherMap = QuestPlaces::TurnInPlace(inn, MAP_B, 2900.0f, 2900.0f);
    Check(otherMap && Near(otherMap->x, 50.0f) && Near(otherMap->z, 5.0f), "on another map, the spawn there");
    AreaPoint const* poi = QuestPlaces::TurnInPlace(*idx.Find(10), MAP_A, 100.0f, 100.0f);
    Check(poi && idx.Find(10)->objectives.back().placeFrom == PlaceFrom::Poi && Near(poi->z, 33.0f),
          "without an ender spawn, the floored turn-in POI");
    Check(!QuestPlaces::TurnInPlace(*idx.Find(11), MAP_A, 100.0f, 100.0f) &&
              idx.Find(11)->objectives.back().kind != ObjectiveKind::Turnin,
          "without an ender spawn and a floor, no turn-in place (the executor ends no_place)");
    Check(!QuestPlaces::TurnInPlace(*idx.Find(1), MAP_A, 100.0f, 100.0f), "a quest without enders and POI has none");
}

void Open(QuestIndex const& idx)
{
    QuestRecord const& quest = *idx.Find(13);
    auto const slots = [&quest](bool kill, bool item)
    {
        std::vector<QuestPlaces::Place> const places = QuestPlaces::OpenPlaces(
            quest, MAP_A, 100.0f, 100.0f, ALLIANCE, [kill, item](std::int32_t slot)
            { return slot == 0 ? kill : (slot == POI_FIRST_ITEM ? item : false); }, noSkill);
        std::string text;
        for (QuestPlaces::Place const& place : places)
            text += std::to_string(place.objective) + "@" + std::to_string(static_cast<int>(place.area->x)) + " ";
        return text;
    };
    Check(slots(true, true) == "0@1010 1@2000 ", "both open: the nearer first (" + slots(true, true) + ")");
    Check(slots(false, true) == "1@2000 ", "the kill done: only the item's place");
    Check(slots(false, false).empty(), "all done: no objective place (the turn-in follows)");
}

// After a stay with progress the executor picks again and leaves the place it stayed at for the objective's next
// area (the targets there are thinned out); at a place it roams inside the area, not 8 yd around itself
void Repick(QuestIndex const& idx)
{
    QuestObjective const& groups = idx.Find(14)->objectives[0];
    AreaPoint const* first = QuestPlaces::PlaceOf(groups, MAP_A, 100.0f, 100.0f, ALLIANCE, noSkill);
    QuestPlaces::Skip const left{true, first ? first->x : 0.0f, first ? first->y : 0.0f};
    AreaPoint const* next = QuestPlaces::PlaceOf(groups, MAP_A, 100.0f, 100.0f, ALLIANCE, noSkill, nullptr, left);
    Check(first && next && Near(first->x, 200.0f) && Near(next->x, 2000.0f) && Near(next->y, 2010.0f),
          "a re-pick leaves the place just stayed at for the objective's next area");
    QuestPlaces::Skip const back{true, next ? next->x : 0.0f, next ? next->y : 0.0f};
    AreaPoint const* again = QuestPlaces::PlaceOf(groups, MAP_A, 100.0f, 100.0f, ALLIANCE, noSkill, nullptr, back);
    Check(again == first, "from the second area the next re-pick goes back to the first");
    QuestObjective const& wolves = idx.Find(1)->objectives[0];
    AreaPoint const* only = QuestPlaces::PlaceOf(wolves, MAP_A, 100.0f, 100.0f, ALLIANCE, noSkill);
    QuestPlaces::Skip const same{true, only ? only->x : 0.0f, only ? only->y : 0.0f};
    Check(only && QuestPlaces::PlaceOf(wolves, MAP_A, 100.0f, 100.0f, ALLIANCE, noSkill, nullptr, same) == only,
          "an objective with one area keeps it on a re-pick");
    std::vector<QuestPlaces::Place> const open = QuestPlaces::OpenPlaces(
        *idx.Find(14), MAP_A, 100.0f, 100.0f, ALLIANCE, [](std::int32_t) { return true; }, noSkill, left);
    Check(open.size() == 1 && Near(open[0].area->x, 2000.0f), "OpenPlaces passes the place left to the objectives");
    Check(next && Near(QuestPlaces::Roam(*next), 10.0f) && first && Near(QuestPlaces::Roam(*first), 8.0f),
          "the roam is the area's radius, at least 8 yd");
    AreaPoint wide;
    wide.radius = 300.0f;
    Check(Near(QuestPlaces::Roam(wide), QuestPlaces::ROAM_MAX_YARDS), "a wide area's roam is capped");
}

// WT34 fix: an area's place is a member (a spawn or a floored point) with its own height, never the mean point
void Members(QuestIndex const& idx)
{
    AreaPoint const* cave = PlaceOf(idx, 15, MAP_A, 3800.0f, 1000.0f);
    bool const caveMember =
        cave && ((Near(cave->z, 90.0f) && Near(cave->y, 1000.0f)) || (Near(cave->z, 50.0f) && Near(cave->y, 1004.0f)));
    Check(cave && cave->points == 8 && caveMember && !Near(cave->z, 70.0f),
          "a camp over a cave: the place is a spawn at its own floor, not the mean height inside the rock");
    std::vector<QuestPlaces::Place> const ring = QuestPlaces::OpenPlaces(
        *idx.Find(16), MAP_A, 4800.0f, 1000.0f, ALLIANCE, [](std::int32_t) { return true; }, noSkill);
    bool ringMember = false;
    if (ring.size() == 1)
        for (int i = 0; i < 8; ++i)
        {
            float const angle = static_cast<float>(i) * 0.785398f;
            ringMember = ringMember || (Near(ring[0].area->x, 5000.0f + 60.0f * std::cos(angle)) &&
                                        Near(ring[0].area->y, 1000.0f + 60.0f * std::sin(angle)) &&
                                        Near(ring[0].area->z, 20.0f + static_cast<float>(i)));
        }
    Check(ringMember && std::hypot(ring[0].area->x - 5000.0f, ring[0].area->y - 1000.0f) > 59.0f,
          "spawns around a lake: the executor walks to one of them, not to the lake's middle");
    Check(ring.size() == 1 && ring[0].area->radius > 119.0f && ring[0].area->radius < 121.0f,
          "the radius is the farthest member's distance from the place");
}

// The invariant over a random fixture index: for every objective of a plan task, the executor's place (PlaceOf from
// the bot's position) is the planner's (Locate, the task's x, y, z), and the plan's ender is TurnInPlace's spawn
void Invariant()
{
    std::mt19937 rng(20261119u);
    auto const coord = [&rng](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    auto const pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
    constexpr std::uint32_t QUESTS = 400;
    constexpr std::uint32_t FIRST = 10000;
    Fixture f;
    for (int camp = 0; camp < 6; ++camp)
        f.Spawn(GIVER + 100 + static_cast<std::uint32_t>(camp), MAP_A, coord(0.0f, 3000.0f), coord(0.0f, 3000.0f),
                coord(0.0f, 50.0f));
    for (std::uint32_t q = FIRST; q < FIRST + QUESTS; ++q)
    {
        QuestRecord quest = Quest(q);
        int const objectives = 1 + pick(3);
        for (int o = 0; o < objectives; ++o)
        {
            std::uint32_t const entry = q * 10 + static_cast<std::uint32_t>(o);
            switch (pick(4))
            {
                case 0:  // kill: one to three spawn groups, some on map B
                    quest.objectives.push_back(Objective(ObjectiveKind::Kill, entry, o));
                    for (int g = 0, groups = 1 + pick(3); g < groups; ++g)
                        f.in.targets.push_back({entry, false, pick(5) ? MAP_A : MAP_B, coord(0.0f, 3000.0f),
                                                coord(0.0f, 3000.0f), coord(-20.0f, 120.0f)});
                    break;
                case 1:  // item from sources, some needing skinning or friendly to a side
                    quest.objectives.push_back(
                        Objective(ObjectiveKind::Item, entry, POI_FIRST_ITEM + static_cast<std::int32_t>(o)));
                    for (int s = 0, sources = 1 + pick(3); s < sources; ++s)
                    {
                        std::uint32_t const owner = entry * 10 + static_cast<std::uint32_t>(s);
                        bool const skin = !pick(4);
                        f.Source(owner, entry, skin ? SourceKind::Skin : SourceKind::Corpse);
                        f.SourceSpawn(owner, coord(0.0f, 3000.0f), coord(0.0f, 3000.0f), coord(-20.0f, 120.0f),
                                      skin ? SKINNING : 0, static_cast<std::uint8_t>(pick(4) ? 0 : 1 + pick(2)));
                    }
                    break;
                default:  // item with only a POI: floors present, partial or absent
                {
                    std::int32_t const slot = POI_FIRST_ITEM + static_cast<std::int32_t>(o);
                    quest.objectives.push_back(Objective(ObjectiveKind::Item, entry, slot));
                    std::vector<std::pair<float, float>> points;
                    std::vector<float> floors;
                    float const cx = coord(0.0f, 3000.0f);
                    float const cy = coord(0.0f, 3000.0f);
                    for (int p = 0, n = 1 + pick(5); p < n; ++p)
                    {
                        points.emplace_back(cx + coord(-40.0f, 40.0f), cy + coord(-40.0f, 40.0f));
                        floors.push_back(pick(4) ? coord(0.0f, 140.0f) : NO_FLOOR);
                    }
                    f.Poi(q, slot, std::move(points), std::move(floors));
                    break;
                }
            }
        }
        f.Add(quest);
        f.Relate(q, GIVER + 100 + static_cast<std::uint32_t>(pick(6)), false);
        for (int e = 0, enders = pick(3); e < enders; ++e)
        {
            std::uint32_t const ender = 3000 + q * 4 + static_cast<std::uint32_t>(e);
            f.Relate(q, ender, true);
            f.Spawn(ender, pick(5) ? MAP_A : MAP_B, coord(0.0f, 3000.0f), coord(0.0f, 3000.0f), coord(0.0f, 90.0f));
        }
        if (pick(2))
            f.Poi(q, POI_TURNIN, {{coord(0.0f, 3000.0f), coord(0.0f, 3000.0f)}},
                  {pick(2) ? coord(0.0f, 90.0f) : NO_FLOOR});
    }
    QuestIndex const idx = QuestIndex::Build(f.in);
    CircuitContext context;
    context.quests = &idx;

    int tasks = 0;
    int turnIns = 0;
    int mismatches = 0;
    for (int b = 0; b < 300; ++b)
    {
        Snapshot bot;
        bot.level = 10;
        bot.race = 1;
        bot.cls = 1;
        bot.team = static_cast<std::uint8_t>(pick(2));
        bot.position = {pick(6) ? MAP_A : MAP_B, coord(0.0f, 3000.0f), coord(0.0f, 3000.0f), coord(0.0f, 60.0f)};
        bot.runSpeed = 7.0f;
        bot.bagFree = 20;
        if (pick(2))
            bot.skills.push_back({SKINNING, 75, 150});
        for (int s = 0, log = 3 + pick(8); s < log; ++s)
        {
            QuestSlot slot;
            slot.quest = FIRST + static_cast<std::uint32_t>(pick(static_cast<int>(QUESTS)));
            slot.counters[0] = static_cast<std::uint16_t>(pick(5));
            if (!pick(6))
                slot.state = Circuit::SLOT_COMPLETE;
            if (std::none_of(bot.quests.begin(), bot.quests.end(),
                             [&slot](QuestSlot const& q) { return q.quest == slot.quest; }))
                bot.quests.push_back(slot);
        }
        auto const hasSkill = [&bot](std::uint16_t id)
        {
            return std::any_of(bot.skills.begin(), bot.skills.end(),
                               [id](Skill const& s) { return s.id == id && s.value; });
        };
        CircuitPlan const plan = Circuit::CircuitBuilder(bot, context).Run();
        for (CircuitStop const& stop : plan.stops)
            for (CircuitTask const& task : stop.tasks)
            {
                QuestRecord const* quest = idx.Find(task.quest);
                if (!quest || task.objective >= quest->objectives.size())
                {
                    ++mismatches;
                    continue;
                }
                QuestObjective const& objective = quest->objectives[task.objective];
                if (!QuestPlaces::Walked(objective.kind))
                    continue;
                ++tasks;
                AreaPoint const* area = QuestPlaces::PlaceOf(objective, bot.position.map, bot.position.x,
                                                             bot.position.y, bot.team, hasSkill);
                if (!area || area->x != task.x || area->y != task.y || area->z != task.z)
                {
                    ++mismatches;
                    if (mismatches <= 5)
                        std::printf("     task quest %u objective %u: planner (%.1f %.1f %.1f) executor %s\n",
                                    task.quest, static_cast<unsigned>(task.objective), task.x, task.y, task.z,
                                    area ? (std::to_string(area->x) + " " + std::to_string(area->y) + " " +
                                            std::to_string(area->z))
                                               .c_str()
                                         : "none");
                }
            }
        for (CircuitTurnIn const& turnIn : plan.turnIns)
        {
            QuestRecord const* quest = idx.Find(turnIn.quest);
            if (!quest || turnIn.ender == Circuit::NONE)
                continue;
            ++turnIns;
            QuestPlace const& ender = idx.places[turnIn.ender];
            AreaPoint const* area = QuestPlaces::TurnInPlace(*quest, bot.position.map, bot.position.x, bot.position.y);
            if (!area || area->x != ender.x || area->y != ender.y || area->z != ender.z)
            {
                ++mismatches;
                if (mismatches <= 5)
                    std::printf("     turn-in quest %u: planner's ender (%.1f %.1f %.1f), executor's differs\n",
                                turnIn.quest, ender.x, ender.y, ender.z);
            }
        }
    }
    std::printf("     invariant: %d objective tasks and %d turn-ins of 300 random bots' plans compared\n", tasks,
                turnIns);
    Check(tasks >= 100 && turnIns >= 30,
          "the invariant is exercised (" + std::to_string(tasks) + " tasks, " + std::to_string(turnIns) + " turn-ins)");
    Check(mismatches == 0, "planner and executor get the same place for every objective and turn-in (" +
                               std::to_string(mismatches) + " differ)");
}
}  // namespace

int main()
{
    QuestPoiZStore const store = Store();
    StoreRules(store);
    QuestIndex const idx = Designed(store);
    Order(idx);
    Usable(idx);
    TurnIns(idx);
    Open(idx);
    Repick(idx);
    Members(idx);
    Invariant();
    std::printf("QUEST places selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
