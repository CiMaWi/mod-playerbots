/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the planner's quest index (travel plan WT13a): records, eligibility, chain links, givers and
// camps, objective areas and their cap, the camp-to-hub rule, guards against bad points, determinism, the memory
// estimate and the build time of a full-size synthetic world; and the quest statistics (QuestStats.h): bands,
// median, merge, the column text, the book's event rules and its use from several threads.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/quest-index-selftest.cpp

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <thread>

#include "QuestIndex.h"
#include "QuestStats.h"

using namespace Planner;

namespace
{
constexpr std::uint32_t MAP_A = 0;
constexpr std::uint32_t MAP_B = 1;
constexpr std::uint32_t ZONE_TOWN = 12;
constexpr std::uint32_t AREA_TOWN = 87;
constexpr std::uint32_t ZONE_FAR = 40;
constexpr std::uint32_t MARSHAL = 1001;  // gives the chain start and most quests
constexpr std::uint32_t DEPUTY = 1002;   // ends 1 and 2, gives 2
constexpr std::uint32_t SIGN = 2001;     // a gameobject giving the breadcrumb
constexpr std::uint32_t RANGER = 1003;   // far camp: chain end, exclusive group
constexpr std::uint32_t ORC = 1004;      // map B
constexpr std::uint32_t TWIN = 1005;     // two spawns, two maps
constexpr std::uint32_t FARMER = 1006;   // the filler quests
constexpr std::uint32_t WOLF = 5000;     // five spawn groups
constexpr std::uint32_t GHOST = 5001;    // no spawns, no POI
constexpr std::uint32_t CRATE = 6000;    // an object target
constexpr std::uint32_t PELT = 9000;     // an item objective with a POI
constexpr std::uint32_t TRIGGER = 77;
constexpr std::uint32_t FILLERS = 16;
constexpr std::uint32_t FILLER_FIRST = 100;
constexpr std::uint32_t FILLER_TARGET = 7000;
constexpr std::uint32_t ALLIANCE_RACES = 0x44D;
constexpr std::uint8_t HUMAN = 1;
constexpr std::uint8_t ORC_RACE = 2;
constexpr std::uint8_t WARRIOR = 1;
constexpr std::uint8_t PALADIN = 2;
constexpr int FULL_QUESTS = 9000;
constexpr int FULL_TARGET_SPAWNS = 120000;
constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();
constexpr float HUGE_F = 1e30f;
constexpr long long GUARD_MS = 100;

int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

QuestRecord Quest(std::uint32_t id, std::int32_t level, std::uint32_t minLevel)
{
    QuestRecord quest;
    quest.id = id;
    quest.level = level;
    quest.minLevel = minLevel;
    quest.zoneOrSort = static_cast<std::int32_t>(ZONE_TOWN);
    quest.xpBase = 100 * id;
    quest.copperReward = static_cast<std::int32_t>(10 * id);
    return quest;
}

QuestObjective Objective(ObjectiveKind kind, std::uint32_t target, std::uint32_t count, std::int32_t poiIndex)
{
    return {kind, target, count, poiIndex, {}};
}

// Map A: the town camp (Marshal, Deputy, a sign) at (100, 100); the ranger 1400 yd east; the farmer far out. Map B:
// the orc. The twin stands on both maps. Wolves in five groups (two on map B), crates near the town
QuestGather Synthetic()
{
    QuestGather in;
    auto add = [&in](QuestRecord quest) { in.quests.push_back(std::move(quest)); };
    auto relate = [&in](std::uint32_t quest, std::uint32_t entry, bool ender, bool object = false)
    { in.relations.push_back({quest, entry, object, ender}); };

    QuestRecord first = Quest(1, 5, 1);
    first.raceMask = ALLIANCE_RACES;
    first.nextInChain = 2;
    add(first);
    QuestRecord second = Quest(2, 6, 2);
    second.prev = 1;
    add(second);
    QuestRecord third = Quest(3, 8, 4);
    third.prev = 2;
    add(third);
    QuestRecord crumb = Quest(5, 7, 3);
    crumb.breadcrumbFor = 3;
    add(crumb);
    QuestRecord elite = Quest(10, 12, 10);
    elite.type = QuestType::ELITE;
    elite.flags = QuestFlag::ELITE;
    elite.objectives.push_back(Objective(ObjectiveKind::Object, CRATE, 2, 0));
    add(elite);
    QuestRecord group = Quest(11, 12, 10);
    group.suggestedPlayers = 3;
    group.flags = QuestFlag::GROUP;
    add(group);
    QuestRecord daily = Quest(12, 10, 8);
    daily.flags = QuestFlag::DAILY | QuestFlag::REPEATABLE;
    add(daily);
    QuestRecord paladin = Quest(13, 10, 8);
    paladin.classMask = 1u << (PALADIN - 1);
    paladin.maxLevel = 20;
    add(paladin);
    QuestRecord wolves = Quest(20, 9, 6);
    wolves.objectives.push_back(Objective(ObjectiveKind::Kill, WOLF, 10, 0));
    add(wolves);
    QuestRecord pelts = Quest(21, 9, 6);
    pelts.objectives.push_back(Objective(ObjectiveKind::Item, PELT, 5, POI_FIRST_ITEM));
    add(pelts);
    QuestRecord ghost = Quest(22, 9, 6);
    ghost.objectives.push_back(Objective(ObjectiveKind::Kill, GHOST, 1, 0));
    add(ghost);
    QuestRecord left = Quest(30, 15, 12);
    left.exclusiveGroup = 30;
    add(left);
    QuestRecord right = Quest(31, 15, 12);
    right.exclusiveGroup = 30;
    add(right);
    QuestRecord raid = Quest(40, 60, 58);
    raid.type = QuestType::RAID;
    raid.flags = QuestFlag::RAID;
    add(raid);
    QuestRecord dungeon = Quest(41, 30, 25);
    dungeon.type = QuestType::DUNGEON;
    dungeon.flags = QuestFlag::DUNGEON;
    add(dungeon);
    QuestRecord pvp = Quest(42, 30, 25);
    pvp.type = QuestType::PVP;
    pvp.flags = QuestFlag::PVP;
    add(pvp);
    add(Quest(50, 20, 15));
    add(Quest(60, 20, 15));  // started by an item: no giver
    QuestRecord scaled = Quest(12000, -1, 1);
    add(scaled);

    for (std::uint32_t q : {1u, 10u, 11u, 12u, 13u, 20u, 21u, 22u})
        relate(q, MARSHAL, false);
    relate(1, DEPUTY, true);
    relate(2, DEPUTY, false);
    relate(2, DEPUTY, true);
    relate(5, SIGN, false, true);
    for (std::uint32_t q : {3u, 30u, 31u})
        relate(q, RANGER, false);
    relate(3, RANGER, true);
    relate(5, RANGER, true);
    for (std::uint32_t q : {40u, 41u, 42u})
        relate(q, ORC, false);
    relate(50, TWIN, false);
    relate(12000, MARSHAL, false);
    relate(999, MARSHAL, false);  // a relation to an unknown quest is ignored

    in.places.push_back({MAP_A, 100.0f, 100.0f, 10.0f, MARSHAL, false, ZONE_TOWN, AREA_TOWN});
    in.places.push_back({MAP_A, 130.0f, 110.0f, 10.0f, DEPUTY, false, ZONE_TOWN, AREA_TOWN});
    in.places.push_back({MAP_A, 110.0f, 140.0f, 10.0f, SIGN, true, ZONE_TOWN, AREA_TOWN});
    in.places.push_back({MAP_A, 1500.0f, 100.0f, 20.0f, RANGER, false, ZONE_FAR, ZONE_FAR});
    in.places.push_back({MAP_B, 0.0f, 0.0f, 0.0f, ORC, false, 0, 0});  // unresolved
    in.places.push_back({MAP_A, 2000.0f, 2000.0f, 0.0f, TWIN, false, ZONE_FAR, ZONE_FAR});
    in.places.push_back({MAP_B, 500.0f, 500.0f, 0.0f, TWIN, false, 0, 0});
    in.places.push_back({MAP_A, 3000.0f, 3000.0f, 0.0f, FARMER, false, ZONE_FAR, ZONE_FAR});
    in.places.push_back(in.places.front());  // a duplicate spawn record collapses
    in.unresolved = 2;

    for (std::uint32_t i = 0; i < FILLERS; ++i)
    {
        QuestRecord filler = Quest(FILLER_FIRST + i, 25, 20);
        filler.objectives.push_back(Objective(ObjectiveKind::Kill, FILLER_TARGET + i, 3, 0));
        add(filler);
        relate(FILLER_FIRST + i, FARMER, false);
        in.targets.push_back({FILLER_TARGET + i, false, MAP_A, 3100.0f + 10.0f * static_cast<float>(i), 3100.0f, 0.0f});
    }

    // Wolves: 6 spawns at (500, 500), 3 at (1500, 1500), 2 on map B, single ones at (2500, 2500) and (3500, 500)
    for (int i = 0; i < 6; ++i)
        in.targets.push_back({WOLF, false, MAP_A, 480.0f + 8.0f * static_cast<float>(i), 500.0f, 5.0f});
    for (int i = 0; i < 3; ++i)
        in.targets.push_back({WOLF, false, MAP_A, 1500.0f, 1490.0f + 10.0f * static_cast<float>(i), 5.0f});
    in.targets.push_back({WOLF, false, MAP_B, 900.0f, 900.0f, 5.0f});
    in.targets.push_back({WOLF, false, MAP_B, 920.0f, 900.0f, 5.0f});
    in.targets.push_back({WOLF, false, MAP_A, 2500.0f, 2500.0f, 5.0f});
    in.targets.push_back({WOLF, false, MAP_A, 3500.0f, 500.0f, 5.0f});
    in.targets.push_back({CRATE, true, MAP_A, 300.0f, 300.0f, 0.0f});
    in.targets.push_back({CRATE, true, MAP_A, 320.0f, 300.0f, 0.0f});

    in.pois.push_back(
        {21, POI_FIRST_ITEM, MAP_A, {{800.0f, 800.0f}, {900.0f, 800.0f}, {900.0f, 900.0f}, {800.0f, 900.0f}}});
    in.pois.push_back({21, POI_TURNIN, MAP_A, {{100.0f, 100.0f}}});
    in.pois.push_back({20, 0, MAP_A, {{0.0f, 0.0f}}});  // a POI for a target with spawns: the spawns win
    in.triggers.push_back({12, TRIGGER, MAP_A, 400.0f, 400.0f, 0.0f, 10.0f});
    in.triggers.push_back({998, TRIGGER + 1, MAP_A, 0.0f, 0.0f, 0.0f, 5.0f});  // unknown quest: ignored
    return in;
}

std::uint32_t PlaceOf(QuestIndex const& index, std::uint32_t entry, std::uint32_t map)
{
    for (std::uint32_t i = 0; i < index.places.size(); ++i)
        if (index.places[i].entry == entry && index.places[i].mapId == map)
            return i;
    return NO_CAMP;
}

bool SameIndex(QuestIndex const& a, QuestIndex const& b)
{
    if (a.quests.size() != b.quests.size() || a.places.size() != b.places.size() || a.camps.size() != b.camps.size())
        return false;
    for (std::size_t i = 0; i < a.quests.size(); ++i)
    {
        QuestRecord const& qa = a.quests[i];
        QuestRecord const& qb = b.quests[i];
        if (qa.id != qb.id || qa.givers != qb.givers || qa.enders != qb.enders ||
            qa.objectives.size() != qb.objectives.size())
            return false;
        for (std::size_t o = 0; o < qa.objectives.size(); ++o)
        {
            std::vector<AreaPoint> const& aa = qa.objectives[o].areas;
            std::vector<AreaPoint> const& ab = qb.objectives[o].areas;
            if (aa.size() != ab.size())
                return false;
            for (std::size_t k = 0; k < aa.size(); ++k)
                if (aa[k].x != ab[k].x || aa[k].y != ab[k].y || aa[k].points != ab[k].points)
                    return false;
        }
    }
    for (std::size_t i = 0; i < a.camps.size(); ++i)
        if (a.camps[i].places != b.camps[i].places || a.camps[i].quests != b.camps[i].quests ||
            a.camps[i].x != b.camps[i].x)
            return false;
    return true;
}

void Records(QuestIndex const& idx)
{
    QuestIndexStats const& s = idx.stats;
    Check(s.quests == 19 + FILLERS, "quests: 19 named + 16 fillers (" + std::to_string(s.quests) + ")");
    Check(s.places == 8 && s.unresolved == 2, "8 places (a duplicate collapsed), 2 unresolved carried");
    Check(s.givers == 8 && s.enders == 2, "8 giver places, 2 ender places");
    Check(s.withoutGiver == 1, "one quest without a giver (60)");
    QuestRecord const* elite = idx.Find(10);
    Check(elite && elite->level == 12 && elite->minLevel == 10 && elite->type == QuestType::ELITE &&
              elite->Has(QuestFlag::ELITE) && !elite->Has(QuestFlag::GROUP) && elite->xpBase == 1000 &&
              elite->copperReward == 100,
          "record fields carried (level, min level, type, flags, xp, money)");
    Check(idx.Find(11) && idx.Find(11)->Has(QuestFlag::GROUP) && idx.Find(11)->suggestedPlayers == 3, "group quest");
    Check(idx.Find(12) && idx.Find(12)->Has(QuestFlag::DAILY), "daily quest");
    Check(idx.Find(40) && idx.Find(40)->Has(QuestFlag::RAID) && idx.Find(41)->Has(QuestFlag::DUNGEON) &&
              idx.Find(42)->Has(QuestFlag::PVP),
          "raid, dungeon and PvP flags");
    Check(idx.Find(12000) && idx.Find(12000)->level == -1, "a scaling quest keeps level -1");
    Check(!idx.Find(999) && !idx.Find(0) && !idx.Find(4), "unknown ids are absent");
    bool sorted = true;
    for (std::size_t i = 1; i < idx.quests.size(); ++i)
        sorted = sorted && idx.quests[i - 1].id < idx.quests[i].id;
    Check(sorted, "quests ascend by id");
}

void Eligibility(QuestIndex const& idx)
{
    QuestRecord const& first = *idx.Find(1);
    Check(Eligible(first, HUMAN, WARRIOR, 1) && !Eligible(first, ORC_RACE, WARRIOR, 1), "race mask");
    QuestRecord const& paladin = *idx.Find(13);
    Check(Eligible(paladin, ORC_RACE, PALADIN, 8) && !Eligible(paladin, HUMAN, WARRIOR, 8), "class mask");
    Check(!Eligible(paladin, HUMAN, PALADIN, 7) && !Eligible(paladin, HUMAN, PALADIN, 21) &&
              Eligible(paladin, HUMAN, PALADIN, 20),
          "minimum and maximum level");
    Check(!Eligible(first, 0, WARRIOR, 10) && Eligible(*idx.Find(10), 0, 0, 10), "race 0 fails a mask, passes none");
}

void Chains(QuestIndex const& idx)
{
    Check(idx.Find(2)->prev == 1 && idx.Find(3)->prev == 2 && idx.Find(1)->nextInChain == 2, "chain links stored");
    std::vector<std::uint32_t> const& afterFirst = idx.Unlocks(1);
    std::vector<std::uint32_t> const& afterSecond = idx.Unlocks(2);
    Check(afterFirst == std::vector<std::uint32_t>{2}, "Unlocks(1) = {2} (prev and next in chain, once)");
    Check(afterSecond == std::vector<std::uint32_t>{3} && idx.Unlocks(3).empty(),
          "Unlocks(2) = {3}, the end opens none");
    Check(idx.Find(5)->breadcrumbFor == 3, "the breadcrumb leads to 3");
    Check(idx.Find(30)->exclusiveGroup == 30 && idx.Find(31)->exclusiveGroup == 30, "the exclusive group");
}

void Givers(QuestIndex const& idx)
{
    std::uint32_t const marshal = PlaceOf(idx, MARSHAL, MAP_A);
    std::uint32_t const deputy = PlaceOf(idx, DEPUTY, MAP_A);
    std::uint32_t const sign = PlaceOf(idx, SIGN, MAP_A);
    Check(marshal != NO_CAMP && deputy != NO_CAMP && sign != NO_CAMP && idx.places[sign].isObject, "places found");
    Check(idx.QuestsGivenAt(marshal) == std::vector<std::uint32_t>{1, 10, 11, 12, 13, 20, 21, 22, 12000},
          "the marshal gives 9 quests");
    Check(idx.QuestsEndedAt(deputy) == std::vector<std::uint32_t>{1, 2}, "the deputy ends 1 and 2");
    Check(idx.Find(1)->givers == std::vector<std::uint32_t>{marshal} &&
              idx.Find(1)->enders == std::vector<std::uint32_t>{deputy},
          "quest 1: giver and ender");
    Check(idx.Find(50)->givers.size() == 2, "the twin gives quest 50 on both maps");
    Check(idx.GiversNear(MAP_A, 100.0f, 100.0f, 50.0f) == std::vector<std::uint32_t>{marshal, deputy, sign},
          "GiversNear 50 yd: nearest first");
    Check(idx.GiversNear(MAP_A, 100.0f, 100.0f, 35.0f).size() == 2, "GiversNear 35 yd: two");
    Check(idx.GiversNear(MAP_B, 0.0f, 0.0f, 10.0f).size() == 1 && idx.GiversNear(7, 0.0f, 0.0f, 100.0f).empty(),
          "GiversNear per map; an unknown map is empty");
    Check(idx.QuestsGivenAt(9999).empty() && idx.QuestsOfGiverCluster(9999).empty(), "out-of-range indices are empty");
}

void Camps(QuestIndex const& idx)
{
    Check(idx.stats.camps == 6, "six camps (" + std::to_string(idx.stats.camps) + ")");
    std::uint32_t const camp = idx.places[PlaceOf(idx, MARSHAL, MAP_A)].camp;
    Check(camp != NO_CAMP && idx.camps[camp].places.size() == 3 && idx.camps[camp].zoneId == ZONE_TOWN,
          "the town camp holds the marshal, the deputy and the sign");
    Check(idx.QuestsOfGiverCluster(camp) == std::vector<std::uint32_t>{1, 2, 5, 10, 11, 12, 13, 20, 21, 22, 12000},
          "the town camp gives 11 quests");
    Check(idx.camps[idx.places[PlaceOf(idx, ORC, MAP_B)].camp].zoneId == 0, "an unresolved camp has zone 0");
    std::vector<Nearby> const nearest = idx.NearestCamps(MAP_A, 1400.0f, 100.0f, 20.0f, 2);
    Check(nearest.size() == 2 && nearest[0].index == idx.places[PlaceOf(idx, RANGER, MAP_A)].camp,
          "NearestCamps: the ranger's first");
    Check(idx.NearestCamps(MAP_A, 1400.0f, 100.0f, 20.0f, 5, 200.0f).size() == 1, "NearestCamps within 200 yd");
}

void Objectives(QuestIndex const& idx)
{
    std::vector<QuestObjective> const& wolves = idx.Find(20)->objectives;
    Check(wolves.size() == 1 && wolves[0].areas.size() == MAX_OBJECTIVE_AREAS,
          "five wolf groups capped at " + std::to_string(MAX_OBJECTIVE_AREAS));
    if (wolves.size() == 1 && wolves[0].areas.size() == MAX_OBJECTIVE_AREAS)
    {
        std::vector<AreaPoint> const& a = wolves[0].areas;
        Check(a[0].points == 6 && a[1].points == 3 && a[2].points == 2 && a[3].points == 1,
              "largest groups first: 6, 3, 2, 1");
        Check(std::fabs(a[0].x - 500.0f) < 0.01f && std::fabs(a[0].y - 500.0f) < 0.01f && a[0].radius < 25.0f,
              "the first area's centre and radius");
        Check(a[2].mapId == MAP_B && a[3].x == 2500.0f, "the map B group; of two singles the lower position stays");
    }
    std::vector<QuestObjective> const& pelts = idx.Find(21)->objectives;
    Check(pelts.size() == 2 && pelts[0].kind == ObjectiveKind::Item && pelts[0].areas.size() == 1 &&
              std::fabs(pelts[0].areas[0].x - 850.0f) < 0.01f && std::fabs(pelts[0].areas[0].radius - 70.71f) < 0.1f,
          "an item objective takes its POI polygon");
    Check(pelts.size() == 2 && pelts[1].kind == ObjectiveKind::Turnin && pelts[1].areas.size() == 1,
          "the turn-in POI makes a Turnin objective");
    Check(idx.Find(10)->objectives[0].areas.size() == 1 && idx.Find(10)->objectives[0].areas[0].points == 2,
          "an object target's spawns");
    std::vector<QuestObjective> const& explore = idx.Find(12)->objectives;
    Check(explore.size() == 1 && explore[0].kind == ObjectiveKind::Explore && explore[0].target == TRIGGER &&
              explore[0].areas.size() == 1 && explore[0].areas[0].radius == 10.0f,
          "an area trigger makes an Explore objective");
    Check(idx.Find(22)->objectives.size() == 1 && idx.Find(22)->objectives[0].areas.empty(),
          "a target without spawns or POI has no area");
    Check(idx.stats.objectives == FILLERS + 6 && idx.stats.withoutArea == 1,
          "objectives " + std::to_string(idx.stats.objectives) + ", without area " +
              std::to_string(idx.stats.withoutArea));
}

void Hubs(QuestIndex const& idx)
{
    // A small hub on the town camp; a wide hub (services 80 yd apart along x) around the ranger's camp at its edge
    GatherInput services;
    std::uint32_t entry = 1;
    for (int i = 0; i < 5; ++i)
        services.spawns.push_back({MAP_A, 100.0f + 10.0f * static_cast<float>(i), 110.0f, 10.0f, entry++,
                                   NpcFlag::VENDOR, 1, ZONE_TOWN, AREA_TOWN, 0, false});
    for (int i = 0; i < 12; ++i)
        services.spawns.push_back({MAP_A, 1500.0f + 80.0f * static_cast<float>(i), 100.0f, 20.0f, entry++,
                                   NpcFlag::VENDOR, 1, ZONE_FAR, ZONE_FAR, 0, false});
    PlannerIndexes const hubs = PlannerIndexes::Build(services);
    Check(hubs.hubs.size() == 2 && hubs.hubs[1].radius > HUB_CORE_YARDS, "two hubs, the second wider than its core");
    std::optional<Nearby> const town = CampHub(idx.camps[idx.places[PlaceOf(idx, MARSHAL, MAP_A)].camp], hubs);
    Check(town && town->index == 0, "the town camp joins the town hub");
    Check(!CampHub(idx.camps[idx.places[PlaceOf(idx, RANGER, MAP_A)].camp], hubs),
          "the ranger's camp at the wide hub's edge joins none");
    Check(!CampHub(idx.camps[idx.places[PlaceOf(idx, FARMER, MAP_A)].camp], hubs), "the farmer's camp joins none");
}

void Guards(QuestIndex const& idx)
{
    auto const start = std::chrono::steady_clock::now();
    Check(idx.GiversNear(MAP_A, NAN_F, 100.0f, 50.0f).empty() && idx.GiversNear(MAP_A, 100.0f, INF_F, 50.0f).empty() &&
              idx.GiversNear(MAP_A, 100.0f, 100.0f, INF_F).empty() &&
              idx.GiversNear(MAP_A, 100.0f, 100.0f, NAN_F).empty() &&
              idx.GiversNear(MAP_A, 100.0f, 100.0f, -5.0f).empty(),
          "GiversNear: NaN, inf and negative input give none");
    Check(idx.GiversNear(MAP_A, HUGE_F, 100.0f, 50.0f).empty() && idx.GiversNear(MAP_A, -HUGE_F, 100.0f, 50.0f).empty(),
          "GiversNear: 1e30 gives none");
    Check(idx.GiversNear(MAP_A, 100.0f, 100.0f, HUGE_F).size() == 6, "GiversNear: a 1e30 yd radius is every giver");
    Check(
        idx.NearestCamps(MAP_A, NAN_F, 0.0f, 0.0f, 3).empty() && idx.NearestCamps(MAP_A, 0.0f, 0.0f, INF_F, 3).empty(),
        "NearestCamps: NaN and inf give none");
    Check(idx.NearestCamps(MAP_A, HUGE_F, 0.0f, 0.0f, 3).size() == 3, "NearestCamps: 1e30 still answers");
    long long const ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    Check(ms < GUARD_MS, "the guard queries return at once (" + std::to_string(ms) + " ms)");
}

void Determinism(QuestIndex const& idx, QuestGather const& input)
{
    Check(SameIndex(idx, QuestIndex::Build(input)), "two builds are equal");
    QuestGather shuffled = input;
    std::mt19937 rng(777);
    std::shuffle(shuffled.quests.begin(), shuffled.quests.end(), rng);
    std::shuffle(shuffled.relations.begin(), shuffled.relations.end(), rng);
    std::shuffle(shuffled.places.begin(), shuffled.places.end(), rng);
    std::shuffle(shuffled.targets.begin(), shuffled.targets.end(), rng);
    std::shuffle(shuffled.pois.begin(), shuffled.pois.end(), rng);
    std::shuffle(shuffled.triggers.begin(), shuffled.triggers.end(), rng);
    Check(SameIndex(idx, QuestIndex::Build(shuffled)), "the build does not depend on the input order");
}

// About lab4's size: 9000 quests, a giver and an ender each among 4000 places, two objectives each, 120k spawns
void FullSize()
{
    QuestGather in;
    std::mt19937 rng(4242);
    std::uniform_real_distribution<float> coord(-8000.0f, 8000.0f);
    for (int i = 0; i < 4000; ++i)
        in.places.push_back({static_cast<std::uint32_t>(i % 2), coord(rng), coord(rng), 0.0f,
                             static_cast<std::uint32_t>(10000 + i), false, ZONE_TOWN, AREA_TOWN});
    for (int q = 1; q <= FULL_QUESTS; ++q)
    {
        QuestRecord quest = Quest(static_cast<std::uint32_t>(q), 10, 5);
        quest.objectives.push_back(Objective(ObjectiveKind::Kill, static_cast<std::uint32_t>(20000 + q % 3000), 8, 0));
        quest.objectives.push_back(
            Objective(ObjectiveKind::Item, static_cast<std::uint32_t>(30000 + q), 4, POI_FIRST_ITEM));
        in.quests.push_back(std::move(quest));
        in.relations.push_back(
            {static_cast<std::uint32_t>(q), static_cast<std::uint32_t>(10000 + q % 4000), false, false});
        in.relations.push_back(
            {static_cast<std::uint32_t>(q), static_cast<std::uint32_t>(10000 + (q * 7) % 4000), false, true});
        in.pois.push_back({static_cast<std::uint32_t>(q), POI_FIRST_ITEM, 0, {{coord(rng), coord(rng)}}});
    }
    for (int i = 0; i < FULL_TARGET_SPAWNS; ++i)
        in.targets.push_back({static_cast<std::uint32_t>(20000 + i % 3000), false, static_cast<std::uint32_t>(i % 2),
                              coord(rng), coord(rng), 0.0f});
    auto const start = std::chrono::steady_clock::now();
    QuestIndex const idx = QuestIndex::Build(std::move(in));
    long long const ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    Check(idx.stats.quests == FULL_QUESTS && idx.stats.withoutArea == 0 && idx.stats.withoutGiver == 0,
          "the full-size index is complete");
    Check(idx.stats.estimatedBytes > 0, "the memory estimate is not zero");
    std::printf(
        "QUEST index full size: %u quests, %u places, %u camps, %u objectives, built in %lld ms (unoptimised),"
        " about %.1f MB\n",
        idx.stats.quests, idx.stats.places, idx.stats.camps, idx.stats.objectives, ms,
        static_cast<double>(idx.stats.estimatedBytes) / (1024.0 * 1024.0));
}

constexpr std::uint64_t MINUTE = MS_PER_MINUTE;
constexpr std::uint32_t BOT = 42;
constexpr std::uint32_t STAT_QUEST = 783;
constexpr int STAT_THREADS = 4;
constexpr int STAT_EVENTS = 5000;  // per thread: an accept and a turn-in each
constexpr int STAT_QUESTS = 50;
constexpr int STAT_RUNS = 5;

bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

// Bands, median, deaths per completion, merge, the over-cap sample and the column text
void StatRules()
{
    Check(LevelBandOf(0) == 0 && LevelBandOf(4) == 0 && LevelBandOf(5) == 1 && LevelBandOf(80) == 16,
          "level bands are five levels wide");
    QuestStatCell empty;
    Check(!MedianMinutes(empty) && !DeathsPerCompletion(empty) && Samples(empty) == 0, "an empty cell has no median");
    QuestStatCell cell;
    for (int i = 0; i < 2; ++i)
    {
        AddSample(cell, 30 * MINUTE);
        AddSample(cell, 60 * MINUTE);
    }
    std::optional<float> const median = MedianMinutes(cell);
    Check(median && Near(*median, 30.0f), "median of 30, 30, 60, 60 minutes is the top of the 30 minute bucket");
    QuestStatCell five;
    for (int i = 0; i < 5; ++i)
        AddSample(five, 8 * MINUTE);
    std::optional<float> const inside = MedianMinutes(five);
    Check(inside && *inside > 7.0f && *inside <= 10.0f, "median of five 8 minute samples lies in their bucket");
    QuestStatCell one;
    AddSample(one, MINUTE / 2);
    Check(MedianMinutes(one) && *MedianMinutes(one) <= 1.0f, "one half-minute sample has a median within a minute");
    QuestStatCell capped;
    Check(!AddSample(capped, SAMPLE_CAP_MS + 1) && Samples(capped) == 0, "a sample over the cap is ignored");
    Check(AddSample(capped, SAMPLE_CAP_MS) && capped.minutes[MINUTE_BUCKETS - 1] == 1, "a sample at the cap counts");
    QuestStatCell deaths;
    deaths.completed = 2;
    deaths.deaths = 3;
    Check(DeathsPerCompletion(deaths) && Near(*DeathsPerCompletion(deaths), 1.5f), "3 deaths over 2 turn-ins is 1.5");
    QuestStatCell merged = cell;
    merged.accepted = 4;
    Merge(merged, five);
    Merge(merged, deaths);
    Check(merged.accepted == 4 && merged.completed == 2 && merged.deaths == 3 && Samples(merged) == 9,
          "merge adds counts and buckets");
    QuestStatCell decoded;
    Check(DecodeMinutes(EncodeMinutes(merged), decoded) && decoded.minutes == merged.minutes,
          "the minutes column round-trips");
    QuestStatCell untouched = decoded;
    Check(!DecodeMinutes("1,2", untouched) && !DecodeMinutes("a", untouched) &&
              !DecodeMinutes(EncodeMinutes(merged) + ",1", untouched) && untouched == decoded,
          "a malformed minutes column is rejected and changes nothing");
    Check(DecodeMinutes("", untouched) && Samples(untouched) == 0, "an empty minutes column is no sample");
}

QuestStatCell CellOf(QuestStatsBook const& book, std::uint32_t questId, std::uint8_t band)
{
    for (auto const& [b, cell] : book.CellsOf(questId))
        if (b == band)
            return cell;
    return {};
}

// The event rules: bands of the accept, samples, abandons counted once, deaths, pruning, restore and the view
void StatBook()
{
    QuestStatsBook book;
    book.Accept(BOT, STAT_QUEST, 12, 0);
    book.Death(BOT, STAT_QUEST, 13);
    book.Reward(BOT, STAT_QUEST, 13, 10 * MINUTE);
    QuestStatCell const band2 = CellOf(book, STAT_QUEST, 2);
    Check(band2.accepted == 1 && band2.completed == 1 && band2.deaths == 1 && Samples(band2) == 1 &&
              band2.minutes[MinuteBucket(10 * MINUTE)] == 1 && CellOf(book, STAT_QUEST, 3).completed == 0,
          "accept, death and turn-in count in the band of the accept, with a 10 minute sample");
    book.Reward(BOT, STAT_QUEST + 1, 20, MINUTE);
    QuestStatCell const lost = CellOf(book, STAT_QUEST + 1, 4);
    Check(lost.completed == 1 && Samples(lost) == 0, "a turn-in without a known accept counts, without a sample");
    book.Accept(BOT, STAT_QUEST + 2, 20, 0);
    book.Reward(BOT, STAT_QUEST + 2, 20, SAMPLE_CAP_MS + MINUTE);
    QuestStatCell const late = CellOf(book, STAT_QUEST + 2, 4);
    Check(late.completed == 1 && Samples(late) == 0, "a turn-in after the cap counts, without a sample");
    book.Accept(BOT, STAT_QUEST + 3, 20, 0);
    book.Abandon(BOT, STAT_QUEST + 3, 20, MINUTE);
    book.Abandon(BOT, STAT_QUEST + 3, 20, 2 * MINUTE);
    book.Drop(BOT, STAT_QUEST + 3, 20);
    Check(CellOf(book, STAT_QUEST + 3, 4).abandoned == 1, "abandon, abandon again and the drop count one abandon");
    book.Accept(BOT, STAT_QUEST + 4, 20, 0);
    book.Drop(BOT, STAT_QUEST + 4, 20);
    book.Drop(BOT + 1, STAT_QUEST + 4, 21);
    Check(CellOf(book, STAT_QUEST + 4, 4).abandoned == 2, "a drop with or without an accept counts an abandon");
    book.Abandon(BOT, STAT_QUEST + 5, 30, 0);
    book.Reward(BOT, STAT_QUEST + 5, 30, MINUTE);
    QuestStatCell const marked = CellOf(book, STAT_QUEST + 5, 6);
    Check(marked.abandoned == 1 && marked.completed == 1 && Samples(marked) == 0,
          "an abandon without an accept, then a turn-in: both count, no sample");
    book.Death(BOT + 2, STAT_QUEST + 6, 44);
    Check(CellOf(book, STAT_QUEST + 6, 8).deaths == 1, "a death without an accept counts in the current band");

    QuestBookCounts const before = book.Counts();
    Check(before.open == 0 && before.dirty == before.cells && before.samples == 1 && before.unsampled == 3,
          "the counters follow the events");
    auto const rows = book.TakeDirty(0);
    Check(rows.size() == before.cells && book.Counts().dirty == 0 && book.TakeDirty(0).empty(),
          "a flush takes each changed cell once");
    book.Accept(BOT, STAT_QUEST + 7, 20, 0);
    book.TakeDirty(OPEN_QUEST_MAX_AGE_MS + 1);
    book.Reward(BOT, STAT_QUEST + 7, 20, OPEN_QUEST_MAX_AGE_MS + MINUTE);
    Check(book.Counts().pruned == 1 && Samples(CellOf(book, STAT_QUEST + 7, 4)) == 0,
          "an open quest older than the cap is forgotten at the flush");

    QuestStatsBook loaded;
    QuestStatCell stored = band2;
    stored.completed = 2;
    stored.deaths = 3;
    loaded.Restore(STAT_QUEST, 2, stored);
    Check(loaded.TakeDirty(0).empty() && loaded.Counts().cells == 1, "restored totals are not dirty");
    loaded.Accept(BOT, STAT_QUEST, 14, 0);
    loaded.Reward(BOT, STAT_QUEST, 14, 10 * MINUTE);
    auto const written = loaded.TakeDirty(0);
    Check(written.size() == 1 && written[0].second.completed == 3 && written[0].second.accepted == 2 &&
              Samples(written[0].second) == 2,
          "after a load the flush writes the stored totals plus the new events");
    std::shared_ptr<QuestStatsView const> const view = loaded.View();
    QuestStatSummary const* found = view->Find(STAT_QUEST, 2);
    Check(found && found->completed == 3 && Near(found->deathsPerCompletion, 1.0f) && found->samples == 2 &&
              found->medianMinutes > 7.0f && found->medianMinutes <= 10.0f && !view->Find(STAT_QUEST, 3),
          "the view holds the summary: median, deaths per turn-in");
}

// Several map threads recording while the world thread flushes and publishes views
void StatThreads()
{
    bool all = true;
    for (int run = 0; run < STAT_RUNS; ++run)
    {
        QuestStatsBook book;
        QuestStatsSlot slot;
        std::atomic<bool> done{false};
        std::uint64_t flushed = 0;
        std::thread world(
            [&]
            {
                while (!done.load())
                {
                    flushed += book.TakeDirty(0).size();
                    slot.Publish(book.View());
                }
            });
        std::vector<std::thread> maps;
        for (int t = 0; t < STAT_THREADS; ++t)
            maps.emplace_back(
                [&book, t]
                {
                    for (int i = 0; i < STAT_EVENTS; ++i)
                    {
                        std::uint32_t const bot = static_cast<std::uint32_t>(t * 100000 + i);
                        std::uint32_t const questId = static_cast<std::uint32_t>(1 + i % STAT_QUESTS);
                        book.Accept(bot, questId, 10, static_cast<std::uint64_t>(i));
                        book.Reward(bot, questId, 10, static_cast<std::uint64_t>(i) + MINUTE);
                    }
                });
        for (std::thread& thread : maps)
            thread.join();
        done.store(true);
        world.join();
        slot.Publish(book.View());
        std::uint64_t accepted = 0;
        std::uint64_t completed = 0;
        std::uint64_t samples = 0;
        for (auto const& [key, row] : slot.Current()->rows)
        {
            accepted += row.accepted;
            completed += row.completed;
            samples += row.samples;
        }
        QuestBookCounts const counts = book.Counts();
        std::uint64_t const events = static_cast<std::uint64_t>(STAT_THREADS) * STAT_EVENTS;
        all = all && accepted == events && completed == events && samples == events && counts.open == 0 &&
              counts.cells == STAT_QUESTS && slot.Current()->rows.size() == STAT_QUESTS && flushed > 0;
    }
    Check(all, "4 threads x 5000 accepts and turn-ins beside a flushing, publishing thread add up, 5 runs");
}
}  // namespace

int main()
{
    QuestGather const input = Synthetic();
    QuestIndex const idx = QuestIndex::Build(input);
    Records(idx);
    Eligibility(idx);
    Chains(idx);
    Givers(idx);
    Camps(idx);
    Objectives(idx);
    Hubs(idx);
    Guards(idx);
    Determinism(idx, input);
    FullSize();
    StatRules();
    StatBook();
    StatThreads();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
