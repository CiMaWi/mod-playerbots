/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the questing circuit (travel plan WT13b, WT.21): the stop order (exact against brute force, the
// heuristic against exact), the clustering, and the scenarios of QuestCircuitHarness.h on a synthetic world built as
// small copies of Goldshire, the Crossroads and Grom'gol. Functional checks of the decisions: most fitting quests
// taken while the log has room, the log kept within its limit (keep, defer, abandon when full), quests that lie
// together done at one stop and turned in together, carried deliveries kept out of the loop, moving on from a dry hub
// (chain, delivery, zone bracket), the abandon rules and the protected set, a group, determinism and bad positions.
// The yards against today's random pick are printed as information only.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/quest-circuit-selftest.cpp

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "QuestCircuitHarness.h"

using namespace Planner;

namespace
{
constexpr std::uint32_t EASTERN = 0;
constexpr std::uint32_t KALIMDOR = 1;
constexpr std::uint32_t ELWYNN = 12;
constexpr std::uint32_t WESTFALL = 40;
constexpr std::uint32_t STRANGLETHORN = 33;
constexpr std::uint32_t BARRENS = 17;
constexpr std::uint32_t STONETALON = 406;
constexpr std::uint32_t NEEDLES = 400;
constexpr std::uint32_t STONETALON_CROWDED_AREA = 4061;
constexpr std::uint32_t STONETALON_QUIET_AREA = 4062;
constexpr std::uint8_t HUMAN = 1;
constexpr std::uint8_t ORC = 2;
constexpr std::uint8_t TROLL = 8;
constexpr std::uint8_t WARRIOR = 1;
constexpr std::uint8_t HUNTER = 3;
constexpr std::uint32_t PALADIN_MASK = 0x2;
constexpr std::uint32_t ALLIANCE_RACES = 0x44D;
constexpr std::uint32_t CROWD = 40;
constexpr float RUN_SPEED = 7.0f;
constexpr std::uint32_t TIMING_RUNS = 200;
constexpr std::uint32_t ORDER_SETS = 30;
constexpr float ORDER_SPAN = 1000.0f;
constexpr float HEURISTIC_LIMIT = 1.25f;  // nearest neighbour + 2-opt within this of the exact order
constexpr float YARD_TOLERANCE = 0.05f;
constexpr std::uint32_t LONELY_XP = 360;  // taken, but too little for its detour from this trip: deferred
constexpr float THOROUGH = 2.0f;

int passed = 0;
int failed = 0;

void Check(bool pass, std::string const& text)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", text.c_str());
    (pass ? passed : failed)++;
}

struct Point2
{
    float x;
    float y;
};

// Places, quests and their objectives of the synthetic world
class World
{
public:
    QuestGather gather;
    GatherInput services;

    void Npc(std::uint32_t entry, std::uint32_t map, Point2 at, std::uint32_t zone, std::uint32_t area)
    {
        gather.places.push_back({map, at.x, at.y, 0.0f, entry, false, zone, area, NO_CAMP});
    }

    static QuestRecord Make(std::uint32_t id, std::int32_t level, std::uint32_t xp)
    {
        QuestRecord quest;
        quest.id = id;
        quest.level = level;
        quest.minLevel = static_cast<std::uint32_t>(std::max(level - 3, 1));
        quest.xpBase = xp;
        return quest;
    }

    void Kill(QuestRecord& quest, std::uint32_t target, std::uint32_t count, std::uint32_t map, Point2 at)
    {
        std::int32_t const slot = static_cast<std::int32_t>(
            std::count_if(quest.objectives.begin(), quest.objectives.end(), [](QuestObjective const& o)
                          { return o.kind == ObjectiveKind::Kill || o.kind == ObjectiveKind::Object; }));
        quest.objectives.push_back({ObjectiveKind::Kill, target, count, slot, {}});
        if (!_targets.insert(target).second)
            return;
        for (Point2 const offset : {Point2{-8.0f, -8.0f}, Point2{8.0f, -6.0f}, Point2{0.0f, 9.0f}})
            gather.targets.push_back({target, false, map, at.x + offset.x, at.y + offset.y, 0.0f});
    }

    void Item(QuestRecord& quest, std::uint32_t item, std::uint32_t count, std::uint32_t map, Point2 at)
    {
        std::int32_t const slot =
            POI_FIRST_ITEM + static_cast<std::int32_t>(std::count_if(quest.objectives.begin(), quest.objectives.end(),
                                                                     [](QuestObjective const& o)
                                                                     { return o.kind == ObjectiveKind::Item; }));
        quest.objectives.push_back({ObjectiveKind::Item, item, count, slot, {}});
        gather.pois.push_back(
            {quest.id, slot, map, {{at.x - 10.0f, at.y - 10.0f}, {at.x + 10.0f, at.y - 10.0f}, {at.x, at.y + 10.0f}}});
    }

    void Explore(std::uint32_t quest, std::uint32_t trigger, std::uint32_t map, Point2 at)
    {
        gather.triggers.push_back({quest, trigger, map, at.x, at.y, 0.0f, 10.0f});
    }

    void Add(QuestRecord quest, std::uint32_t giver, std::uint32_t ender)
    {
        gather.relations.push_back({quest.id, giver, false, false});
        gather.relations.push_back({quest.id, ender, false, true});
        gather.quests.push_back(std::move(quest));
    }

private:
    std::set<std::uint32_t> _targets;
};

// Positions as in the world (2D; heights left out)
constexpr Point2 GOLDSHIRE{-9465.0f, 40.0f};
constexpr Point2 GOLDSHIRE_INN{-9450.0f, 55.0f};
constexpr Point2 SENTINEL_HILL{-10630.0f, 1090.0f};
constexpr Point2 EASTVALE{-9450.0f, -1300.0f};
constexpr Point2 ELWYNN_NW{-9265.0f, -110.0f};
constexpr Point2 ELWYNN_E{-9600.0f, 330.0f};
constexpr Point2 ELWYNN_S{-9800.0f, -50.0f};
constexpr Point2 ELWYNN_FAR{-8900.0f, -1500.0f};
constexpr Point2 ELWYNN_LONELY{-8600.0f, 500.0f};  // about 980 yd from Goldshire, away from the other stops
constexpr Point2 LONE_CAMP{-7465.0f, 1540.0f};     // 2500 yd from Goldshire: an ender who gives no quest
constexpr Point2 CROSSROADS{-450.0f, -2640.0f};
constexpr Point2 CROSSROADS_INN{-440.0f, -2625.0f};
constexpr Point2 BARRENS_N{-150.0f, -2450.0f};
constexpr Point2 BARRENS_E{-500.0f, -2200.0f};
constexpr Point2 BARRENS_SW{-800.0f, -2950.0f};
constexpr Point2 BARRENS_FAR{-450.0f, -7000.0f};  // beyond FAR_YARDS
constexpr Point2 RATCHET{-950.0f, -3700.0f};
constexpr Point2 NEEDLES_CAMP{-450.0f, -3300.0f};
constexpr Point2 STONETALON_BUSY{-1100.0f, -2200.0f};
constexpr Point2 STONETALON_QUIET{-1300.0f, -2000.0f};
constexpr Point2 GROMGOL{-12360.0f, 200.0f};
constexpr Point2 GROMGOL_HUT{-12345.0f, 215.0f};
constexpr Point2 JUNGLE_A{-12100.0f, 50.0f};
constexpr Point2 JUNGLE_A2{-12150.0f, 0.0f};
constexpr Point2 JUNGLE_B{-12600.0f, 450.0f};
constexpr Point2 JUNGLE_C{-12200.0f, 600.0f};
constexpr Point2 DUSKWOOD_FAR{-10000.0f, -3500.0f};   // beyond FAR_YARDS
constexpr Point2 ELWYNN_TOO_FAR{-5000.0f, -1300.0f};  // beyond FAR_YARDS from Goldshire and Eastvale

Point2 Offset(Point2 at, float dx, float dy) { return {at.x + dx, at.y + dy}; }

// Giver entries
constexpr std::uint32_t MARSHAL = 1001;
constexpr std::uint32_t INNKEEPER = 1002;
constexpr std::uint32_t SENTINEL = 1010;
constexpr std::uint32_t FARMER = 1011;
constexpr std::uint32_t LONE_ENDER = 1012;
constexpr std::uint32_t CROSSROADS_GIVER = 1003;
constexpr std::uint32_t CROSSROADS_HOST = 1004;
constexpr std::uint32_t RATCHET_GIVER = 1023;
constexpr std::uint32_t NEEDLES_GIVER = 1020;
constexpr std::uint32_t STONETALON_BUSY_GIVER = 1021;
constexpr std::uint32_t STONETALON_QUIET_GIVER = 1022;
constexpr std::uint32_t GROMGOL_GIVER = 1005;
constexpr std::uint32_t GROMGOL_HOST = 1006;

void BuildElwynn(World& w)
{
    w.Npc(MARSHAL, EASTERN, GOLDSHIRE, ELWYNN, 87);
    w.Npc(INNKEEPER, EASTERN, GOLDSHIRE_INN, ELWYNN, 87);
    w.Npc(SENTINEL, EASTERN, SENTINEL_HILL, WESTFALL, 108);
    w.Npc(FARMER, EASTERN, EASTVALE, ELWYNN, 88);
    w.Npc(LONE_ENDER, EASTERN, LONE_CAMP, ELWYNN, 89);
    QuestRecord q = World::Make(101, 11, 700);
    w.Kill(q, 5001, 8, EASTERN, ELWYNN_NW);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(102, 11, 650);
    w.Item(q, 9001, 6, EASTERN, Offset(ELWYNN_NW, 20.0f, 0.0f));
    w.Add(q, INNKEEPER, MARSHAL);
    q = World::Make(103, 12, 750);
    w.Kill(q, 5002, 10, EASTERN, ELWYNN_E);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(104, 12, 650);
    w.Item(q, 9002, 8, EASTERN, Offset(ELWYNN_E, 0.0f, 25.0f));
    w.Add(q, MARSHAL, INNKEEPER);
    w.Add(World::Make(105, 10, 300), MARSHAL, MARSHAL);
    w.Explore(105, 701, EASTERN, Offset(ELWYNN_NW, -20.0f, 20.0f));
    q = World::Make(106, 12, 1500);
    q.flags = QuestFlag::ELITE;
    q.type = QuestType::ELITE;
    w.Kill(q, 5004, 1, EASTERN, Offset(ELWYNN_E, 30.0f, 0.0f));
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(107, 12, 1500);
    q.flags = QuestFlag::GROUP;
    q.suggestedPlayers = 3;
    w.Kill(q, 5005, 1, EASTERN, ELWYNN_S);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(108, 6, 200);
    w.Kill(q, 5001, 5, EASTERN, ELWYNN_NW);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(109, 12, 700);
    q.classMask = PALADIN_MASK;
    w.Kill(q, 5003, 4, EASTERN, ELWYNN_S);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(110, 12, 700);
    q.exclusiveGroup = 50;
    w.Kill(q, 5003, 8, EASTERN, ELWYNN_S);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(111, 12, 400);
    q.exclusiveGroup = 50;
    w.Kill(q, 5003, 6, EASTERN, ELWYNN_S);
    w.Add(q, INNKEEPER, MARSHAL);
    q = World::Make(112, 12, 100);
    q.breadcrumbFor = 201;
    w.Add(q, MARSHAL, SENTINEL);
    q = World::Make(113, 10, 400);
    q.nextInChain = 114;
    w.Kill(q, 5001, 2, EASTERN, ELWYNN_NW);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(114, 12, 800);
    q.prev = 113;
    q.nextInChain = 115;
    w.Kill(q, 5002, 6, EASTERN, ELWYNN_E);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(115, 12, 800);
    q.prev = 114;
    w.Kill(q, 5003, 5, EASTERN, ELWYNN_S);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(116, 16, 900);
    w.Kill(q, 5003, 5, EASTERN, ELWYNN_S);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(117, 12, 100);
    q.breadcrumbFor = 202;
    w.Add(q, INNKEEPER, SENTINEL);
    q = World::Make(118, 12, 500);
    q.flags = QuestFlag::DAILY;
    w.Kill(q, 5001, 4, EASTERN, ELWYNN_NW);
    w.Add(q, MARSHAL, MARSHAL);
    q = World::Make(120, 12, 500);
    w.Kill(q, 5001, 4, EASTERN, ELWYNN_NW);
    w.Add(q, INNKEEPER, INNKEEPER);
    for (std::uint32_t const id : {121u, 122u})
    {
        q = World::Make(id, 5, 150);
        w.Kill(q, 5006, 5, EASTERN, ELWYNN_FAR);
        w.Add(q, FARMER, FARMER);
    }
    q = World::Make(123, 12, 1200);
    q.flags = QuestFlag::ELITE;
    w.Kill(q, 5006, 1, EASTERN, ELWYNN_FAR);
    w.Add(q, FARMER, FARMER);
    q = World::Make(124, 12, LONELY_XP);
    w.Kill(q, 5009, 5, EASTERN, ELWYNN_LONELY);
    w.Add(q, MARSHAL, MARSHAL);
    w.Add(World::Make(125, 12, 100), SENTINEL, LONE_ENDER);  // a delivery to the lone ender
    q = World::Make(129, 9, 300);
    w.Kill(q, 5007, 3, EASTERN, Offset(EASTVALE, 100.0f, 0.0f));
    w.Add(q, FARMER, FARMER);
    q = World::Make(130, 10, 450);
    q.prev = 129;
    w.Kill(q, 5007, 6, EASTERN, Offset(EASTVALE, 100.0f, 0.0f));
    w.Add(q, FARMER, FARMER);
    q = World::Make(201, 12, 800);
    w.Kill(q, 5008, 8, EASTERN, Offset(SENTINEL_HILL, 200.0f, 0.0f));
    w.Add(q, SENTINEL, SENTINEL);
    q = World::Make(202, 14, 900);
    q.minLevel = 14;
    w.Kill(q, 5008, 8, EASTERN, Offset(SENTINEL_HILL, 200.0f, 0.0f));
    w.Add(q, SENTINEL, SENTINEL);
    q = World::Make(203, 13, 900);
    w.Kill(q, 5008, 8, EASTERN, Offset(SENTINEL_HILL, 200.0f, 0.0f));
    w.Add(q, SENTINEL, SENTINEL);
}

void BuildBarrens(World& w)
{
    w.Npc(CROSSROADS_GIVER, KALIMDOR, CROSSROADS, BARRENS, 380);
    w.Npc(CROSSROADS_HOST, KALIMDOR, CROSSROADS_INN, BARRENS, 380);
    w.Npc(NEEDLES_GIVER, KALIMDOR, NEEDLES_CAMP, NEEDLES, 4000);
    w.Npc(STONETALON_BUSY_GIVER, KALIMDOR, STONETALON_BUSY, STONETALON, STONETALON_CROWDED_AREA);
    w.Npc(STONETALON_QUIET_GIVER, KALIMDOR, STONETALON_QUIET, STONETALON, STONETALON_QUIET_AREA);
    w.Npc(RATCHET_GIVER, KALIMDOR, RATCHET, BARRENS, 392);
    QuestRecord q = World::Make(301, 21, 1800);
    w.Kill(q, 5101, 10, KALIMDOR, BARRENS_N);
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(302, 21, 1500);
    w.Item(q, 9101, 8, KALIMDOR, Offset(BARRENS_N, 20.0f, 20.0f));
    w.Add(q, CROSSROADS_HOST, CROSSROADS_GIVER);
    q = World::Make(303, 22, 1900);
    w.Kill(q, 5102, 12, KALIMDOR, BARRENS_E);
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(304, 23, 2000);
    w.Kill(q, 5103, 4, KALIMDOR, Offset(BARRENS_E, 15.0f, 0.0f));
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_HOST);
    q = World::Make(305, 22, 1700);
    w.Kill(q, 5104, 10, KALIMDOR, BARRENS_SW);
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(306, 22, 1800);
    w.Kill(q, 5105, 8, KALIMDOR, Offset(BARRENS_SW, 0.0f, 30.0f));
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(307, 22, 1600);
    w.Item(q, 9102, 6, KALIMDOR, Offset(BARRENS_SW, -20.0f, 0.0f));
    w.Add(q, CROSSROADS_HOST, CROSSROADS_GIVER);
    q = World::Make(308, 22, 1500);
    q.flags = QuestFlag::PVP;
    q.type = QuestType::PVP;
    w.Kill(q, 5106, 5, KALIMDOR, BARRENS_E);
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(309, 26, 2500);
    q.minLevel = 22;
    w.Kill(q, 5104, 10, KALIMDOR, BARRENS_SW);
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(310, 22, 1500);
    q.raceMask = ALLIANCE_RACES;
    w.Kill(q, 5104, 10, KALIMDOR, BARRENS_SW);
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(311, 23, 1700);
    q.flags = QuestFlag::GROUP;
    q.suggestedPlayers = 3;
    w.Kill(q, 5107, 6, KALIMDOR, Offset(BARRENS_E, 0.0f, 20.0f));
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(320, 22, 1500);
    w.Kill(q, 5101, 10, KALIMDOR, BARRENS_N);
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(321, 22, 1500);
    w.Kill(q, 5104, 10, KALIMDOR, BARRENS_SW);
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(322, 22, 1500);
    w.Kill(q, 5102, 10, KALIMDOR, BARRENS_E);
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    q = World::Make(323, 22, 1500);
    w.Kill(q, 5101, 10, KALIMDOR, BARRENS_N);
    w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    for (std::uint32_t id = 330; id < 350; ++id)
    {
        q = World::Make(id, 22, 1000);
        w.Kill(q, 5108, 5, KALIMDOR, BARRENS_FAR);
        w.Add(q, CROSSROADS_GIVER, CROSSROADS_GIVER);
    }
    // Given at Ratchet, done around the Crossroads and turned in there: log quests a bot brings to the hub
    Point2 const around[] = {BARRENS_N, BARRENS_E, BARRENS_SW};
    for (std::uint32_t id = 350; id < 370; ++id)
    {
        q = World::Make(id, 22, 1200);
        w.Kill(q, 5150 + id % 3, 5, KALIMDOR, Offset(around[id % 3], 30.0f, -30.0f));
        w.Add(q, RATCHET_GIVER, CROSSROADS_GIVER);
    }
    std::uint32_t const campQuests[][3] = {
        {STONETALON_QUIET_GIVER, 501, 502}, {STONETALON_BUSY_GIVER, 503, 504}, {NEEDLES_GIVER, 505, 506}};
    for (auto const& camp : campQuests)
        for (std::uint32_t i = 1; i <= 2; ++i)
        {
            q = World::Make(camp[i], camp[0] == NEEDLES_GIVER ? 24 : 22, 1500);
            w.Kill(q, 5110 + camp[i], 8, KALIMDOR,
                   Offset(camp[0] == NEEDLES_GIVER ? NEEDLES_CAMP : STONETALON_BUSY, 50.0f, 50.0f));
            w.Add(q, camp[0], camp[0]);
        }
}

void BuildStranglethorn(World& w)
{
    w.Npc(GROMGOL_GIVER, EASTERN, GROMGOL, STRANGLETHORN, 117);
    w.Npc(GROMGOL_HOST, EASTERN, GROMGOL_HUT, STRANGLETHORN, 117);
    struct Row
    {
        std::uint32_t id;
        std::int32_t level;
        std::uint32_t xp;
        ObjectiveKind kind;
        Point2 at;
    };
    for (Row const& row :
         {Row{401, 35, 3200, ObjectiveKind::Kill, JUNGLE_A}, Row{402, 35, 3100, ObjectiveKind::Kill, JUNGLE_A2},
          Row{403, 36, 3300, ObjectiveKind::Kill, JUNGLE_B}, Row{404, 34, 900, ObjectiveKind::Item, JUNGLE_B},
          Row{405, 35, 3000, ObjectiveKind::Kill, JUNGLE_C}})
    {
        QuestRecord q = World::Make(row.id, row.level, row.xp);
        if (row.kind == ObjectiveKind::Kill)
            w.Kill(q, 5200 + row.id, 10, EASTERN, row.at);
        else
            w.Item(q, 9200 + row.id, 10, EASTERN, Offset(row.at, 20.0f, 0.0f));
        w.Add(q, GROMGOL_GIVER, GROMGOL_GIVER);
    }
    w.Add(World::Make(406, 33, 500), GROMGOL_HOST, GROMGOL_GIVER);
    w.Explore(406, 702, EASTERN, Offset(JUNGLE_C, 20.0f, 0.0f));
    QuestRecord q = World::Make(420, 35, 1500);
    q.flags = QuestFlag::ELITE;
    w.Kill(q, 5299, 1, EASTERN, DUSKWOOD_FAR);
    w.Add(q, GROMGOL_GIVER, GROMGOL_GIVER);
    q = World::Make(421, 35, 1500);
    q.flags = QuestFlag::GROUP;
    q.suggestedPlayers = 3;
    w.Kill(q, 5299, 1, EASTERN, DUSKWOOD_FAR);
    w.Add(q, GROMGOL_GIVER, GROMGOL_GIVER);
    for (std::uint32_t id = 430; id < 449; ++id)
    {
        q = World::Make(id, 34, 2000);
        w.Kill(q, 5298, 5, EASTERN, DUSKWOOD_FAR);
        w.Add(q, GROMGOL_GIVER, GROMGOL_GIVER);
    }
}

void SetZones(World& w)
{
    w.services.zones = {
        {ELWYNN, 1, 10, ZoneSide::Alliance, EASTERN},          {WESTFALL, 10, 20, ZoneSide::Alliance, EASTERN},
        {STRANGLETHORN, 30, 45, ZoneSide::Contested, EASTERN}, {BARRENS, 10, 25, ZoneSide::Horde, KALIMDOR},
        {STONETALON, 15, 27, ZoneSide::Contested, KALIMDOR},   {NEEDLES, 25, 35, ZoneSide::Contested, KALIMDOR}};
}

// Elwynn with the chain quests 114 (Goldshire) and 130 (Eastvale) changed so that a bot arriving at either camp would
// not take them (review 5, finding 2): their objectives beyond FAR_YARDS of the givers, or without an area
World ChainVariant(bool noArea)
{
    constexpr std::uint32_t UNSPAWNED = 5020;  // a kill target without spawns: the objective has no area
    constexpr std::uint32_t FAR_TARGET = 5021;
    World w;
    BuildElwynn(w);
    for (QuestRecord& quest : w.gather.quests)
        if (quest.id == 114 || quest.id == 130)
        {
            quest.objectives.clear();
            if (noArea)
                quest.objectives.push_back({ObjectiveKind::Kill, UNSPAWNED, 6, 0, {}});
            else
                w.Kill(quest, FAR_TARGET, 6, EASTERN, ELWYNN_TOO_FAR);
        }
    SetZones(w);
    return w;
}

// Elwynn and the Barrens with the breadcrumb 117 worth more than the Barrens' far quests 330-349 and less than 350-369
// (WT14 fix): a carried take weighed against the idle quests a full log gives up for it
World DeliveryVariant()
{
    World w;
    BuildElwynn(w);
    BuildBarrens(w);
    for (QuestRecord& quest : w.gather.quests)
        if (quest.id == 117)
            quest.xpBase = 1100;
    SetZones(w);
    return w;
}

Snapshot Bot(std::uint8_t level, std::uint8_t race, std::uint8_t cls, std::uint8_t team, std::uint32_t map, Point2 at)
{
    Snapshot bot;
    bot.level = level;
    bot.race = race;
    bot.cls = cls;
    bot.team = team;
    bot.position = {map, at.x + 5.0f, at.y + 5.0f, 0.0f};
    bot.runSpeed = RUN_SPEED;
    bot.bagFree = 20;
    return bot;
}

QuestSlot Slot(std::uint32_t quest, std::uint32_t state = 0, std::uint16_t first = 0)
{
    QuestSlot slot;
    slot.quest = quest;
    slot.state = state;
    slot.counters[0] = first;
    return slot;
}

void Stat(QuestStatsView& view, std::uint32_t quest, std::uint8_t level, float median, float deathsPerCompletion)
{
    QuestStatSummary summary;
    summary.accepted = 12;
    summary.completed = 10;
    summary.samples = 10;
    summary.medianMinutes = median;
    summary.deathsPerCompletion = deathsPerCompletion;
    view.rows[StatKey(quest, LevelBandOf(level))] = summary;
}

std::uint32_t CampAt(QuestIndex const& index, std::uint32_t map, Point2 at)
{
    std::vector<Nearby> const near = index.NearestCamps(map, at.x, at.y, 0.0f, 1);
    return near.empty() ? NO_CAMP : near.front().index;
}

void Run(CircuitHarness::Scenario const& scenario, bool guards)
{
    CircuitHarness::Report const report = CircuitHarness::RunScenario(scenario, TIMING_RUNS);
    for (CircuitHarness::Check const& check : report.checks)
        Check(check.pass, check.text);
    if (guards)
        for (CircuitHarness::Check const& check : CircuitHarness::GuardChecks(scenario))
            Check(check.pass, check.text);
    for (std::string const& line : CircuitHarness::PlanLines(report.plan, report.baselineYards))
        std::printf("     %s\n", line.c_str());
}

// Brute force over the permutations
float BestYards(Circuit::Point const& start, std::vector<Circuit::Point> const& points, Circuit::Point const& end)
{
    std::vector<std::uint32_t> order(points.size());
    std::iota(order.begin(), order.end(), 0u);
    float best = std::numeric_limits<float>::infinity();
    do
        best = std::min(best, Circuit::PathYards(start, points, order, end));
    while (std::next_permutation(order.begin(), order.end()));
    return best;
}

void CheckOrders()
{
    std::mt19937 random(7);
    std::uniform_real_distribution<float> span(0.0f, ORDER_SPAN);
    bool exact = true;
    float worst = 1.0f;
    bool permutation = true;
    for (std::uint32_t set = 0; set < ORDER_SETS; ++set)
    {
        std::size_t const n = 1 + set % 7;
        std::vector<Circuit::Point> points;
        for (std::size_t i = 0; i < n; ++i)
            points.push_back({span(random), span(random), 0.0f});
        Circuit::Point const start{span(random), span(random), 0.0f};
        Circuit::Point const end{span(random), span(random), 0.0f};
        float const best = BestYards(start, points, end);
        float const found = Circuit::PathYards(start, points, Circuit::ExactOrder(start, points, end), end);
        exact = exact && std::abs(found - best) <= YARD_TOLERANCE;
        std::vector<Circuit::Point> big = points;
        for (std::size_t i = n; i < 8; ++i)
            big.push_back({span(random), span(random), 0.0f});
        float const bigBest = Circuit::PathYards(start, big, Circuit::ExactOrder(start, big, end), end);
        std::vector<std::uint32_t> heuristic = Circuit::HeuristicOrder(start, big, end);
        worst = std::max(worst, Circuit::PathYards(start, big, heuristic, end) / bigBest);
        std::sort(heuristic.begin(), heuristic.end());
        for (std::uint32_t i = 0; i < heuristic.size(); ++i)
            permutation = permutation && heuristic[i] == i;
    }
    Check(exact, "exact order equals brute force on 30 sets of 1-7 stops");
    Check(worst <= HEURISTIC_LIMIT,
          "nearest neighbour + 2-opt within " + std::to_string(worst) + " x the exact order on 8 stops (limit 1.25)");
    Check(permutation, "the heuristic order visits every stop once");
    std::vector<Circuit::Point> many;
    for (std::uint32_t i = 0; i < 14; ++i)
        many.push_back({span(random), span(random), 0.0f});
    std::vector<std::uint32_t> order = Circuit::OrderStops({0.0f, 0.0f, 0.0f}, many, {0.0f, 0.0f, 0.0f});
    std::sort(order.begin(), order.end());
    Check(order.size() == many.size() && std::adjacent_find(order.begin(), order.end()) == order.end(),
          "14 stops are ordered by the heuristic, each once");
}

void CheckClusters()
{
    std::vector<CircuitTask> tasks = {
        {1, 0, 0.0f, 0.0f, 0.0f}, {2, 0, 70.0f, 0.0f, 0.0f}, {3, 0, 140.0f, 0.0f, 0.0f}, {4, 0, 300.0f, 0.0f, 0.0f}};
    std::vector<std::vector<std::uint32_t>> const groups = Circuit::ClusterTasks(tasks);
    Check(groups.size() == 2 && groups[0].size() == 3 && groups[1].size() == 1,
          "points 70 yd apart chain into one stop, 160 yd stays apart");
}
}  // namespace

int main()
{
    CheckOrders();
    CheckClusters();

    World world;
    BuildElwynn(world);
    BuildBarrens(world);
    BuildStranglethorn(world);
    SetZones(world);
    QuestIndex const quests = QuestIndex::Build(world.gather);
    PlannerIndexes const indexes = PlannerIndexes::Build(world.services);
    QuestStatsView stats;
    Stat(stats, 306, 22, 12.0f, 1.2f);
    Stat(stats, 307, 22, 4.0f, 0.0f);
    Stat(stats, 320, 22, 15.0f, 0.1f);
    Stat(stats, 321, 22, 10.0f, 0.5f);
    Stat(stats, 322, 22, 15.0f, 0.1f);
    CrowdingSnapshot crowding;
    crowding.byArea[AreaKey(KALIMDOR, STONETALON_CROWDED_AREA)] = {CROWD, 0};

    CircuitContext base;
    base.quests = &quests;
    base.indexes = &indexes;
    base.stats = &stats;
    base.crowding = &crowding;

    std::uint32_t const goldshireCamp = CampAt(quests, EASTERN, GOLDSHIRE);

    // A level 12 human at Goldshire: takes most quests here, a complete quest to turn in, a grey one far away, a
    // protected one, a solo elite; the breadcrumb 117 to Sentinel Hill is carried, not walked
    CircuitHarness::Scenario goldshire{"goldshire-12-human", Bot(12, HUMAN, WARRIOR, 0, EASTERN, GOLDSHIRE), base, {}};
    goldshire.bot.rewardedQuests = {113};
    goldshire.bot.quests = {Slot(120, Circuit::SLOT_COMPLETE, 4), Slot(121), Slot(122), Slot(123)};
    goldshire.context.protectedQuests = {122};
    goldshire.expect.exactTake = true;
    goldshire.expect.take = {101, 102, 103, 104, 105, 110, 114, 117, 124};
    goldshire.expect.notTake = {106, 107, 108, 109, 111, 112, 115, 116, 118};
    CircuitHarness::ExactAbandon(goldshire.expect, {121});
    goldshire.expect.keep = {122, 123};
    goldshire.expect.followUps = {115};
    goldshire.expect.checkMove = true;
    goldshire.expect.move = MoveReason::None;
    // This trip's set: the lonely 124 is taken but waits for a later trip, so does the protected grey 122 far out;
    // the solo elite 123 waits for a group
    goldshire.expect.checkDeferred = true;
    goldshire.expect.deferred = {122, 124};
    goldshire.expect.together = {{101, 102, 105}, {103, 104}};
    goldshire.expect.carried = {117};
    goldshire.expect.checkEnderCamps = true;
    goldshire.expect.enderCamps = {goldshireCamp};
    Run(goldshire, true);

    // A thorough bot does the lonely quest on this trip
    CircuitHarness::Scenario thorough = goldshire;
    thorough.name = "goldshire-12-thorough";
    thorough.context.weights.thoroughness = THOROUGH;
    thorough.expect.deferred = {122};
    Run(thorough, false);

    // The next circuit with the carried 117 done in the log (review 2, finding A): it stays out of the loop, the loop
    // still ends at Goldshire, which has quests left
    CircuitHarness::Scenario carrying = goldshire;
    carrying.name = "goldshire-12-carrying";
    carrying.bot.quests.push_back(Slot(117, Circuit::SLOT_COMPLETE));
    carrying.expect.take = {101, 102, 103, 104, 105, 110, 114, 124};
    carrying.expect.notTake = {117};
    Run(carrying, false);

    // A done delivery whose ender stands alone 2500 yd away (review 3, note 1): its turn-in earns too little for the
    // walk while the hub has work, so the trip leaves that camp for later and still ends at Goldshire
    CircuitHarness::Scenario lone = goldshire;
    lone.name = "goldshire-12-lone-delivery";
    lone.bot.quests.push_back(Slot(125, Circuit::SLOT_COMPLETE));
    lone.expect.carried = {117, 125};
    Run(lone, false);

    // A log full of done quests the bot can neither abandon nor turn in here (review 3, finding 1): the hub's quests
    // wait for log space that only delivering frees, so the bot moves on to deliver at Eastvale
    CircuitHarness::Scenario fullDone{
        "goldshire-12-full-log-done", Bot(12, HUMAN, WARRIOR, 0, EASTERN, GOLDSHIRE), base, {}};
    fullDone.bot.rewardedQuests = {113};
    for (std::uint32_t id = 330; id < 350; ++id)
        fullDone.bot.quests.push_back(Slot(id, Circuit::SLOT_COMPLETE));
    for (std::uint32_t const id : {121u, 122u, 123u})
        fullDone.bot.quests.push_back(Slot(id, Circuit::SLOT_COMPLETE));
    fullDone.expect.exactTake = true;
    fullDone.expect.minTake = 0;
    CircuitHarness::ExactAbandon(fullDone.expect, {});
    fullDone.expect.carried = {121, 122, 123};
    fullDone.expect.checkMove = true;
    fullDone.expect.move = MoveReason::Delivery;
    fullDone.expect.moveCamp = CampAt(quests, EASTERN, EASTVALE);
    Run(fullDone, false);

    // The same log with one done quest ending here: its turn-in frees a slot on this trip, so the hub's quests wait
    // for it and the bot stays
    CircuitHarness::Scenario fullTurnIn = fullDone;
    fullTurnIn.name = "goldshire-12-full-log-turn-in";
    fullTurnIn.bot.quests.back() = Slot(120, Circuit::SLOT_COMPLETE, 4);
    fullTurnIn.expect.carried = {121, 122};
    fullTurnIn.expect.turnIns = {120};
    fullTurnIn.expect.move = MoveReason::None;
    fullTurnIn.expect.moveCamp = NO_CAMP;
    Run(fullTurnIn, false);

    // A log of 20 far quests (another map, each worth 1200; WT14 fix): the hub's quests push the five cheapest out,
    // the carried 117 (a delivery worth 100) pushes none out and waits at the hub for log space
    CircuitHarness::Scenario cheapDelivery{
        "goldshire-12-full-log-cheap-delivery", Bot(12, HUMAN, WARRIOR, 0, EASTERN, GOLDSHIRE), base, {}};
    cheapDelivery.bot.rewardedQuests = {113, 120};
    for (std::uint32_t id = 350; id < 370; ++id)
        cheapDelivery.bot.quests.push_back(Slot(id));
    cheapDelivery.expect.exactTake = true;
    cheapDelivery.expect.take = {101, 102, 103, 104, 105, 110, 114, 124};
    cheapDelivery.expect.notTake = {117};
    CircuitHarness::ExactAbandon(cheapDelivery.expect, {350, 351, 352, 353, 354});
    Run(cheapDelivery, false);

    // The same full log (23 far quests) with 117 worth 1100: the hub's quests push out 330-337, then 117 pushes out
    // 338 (worth 1000) but no 350 quest (worth 1200)
    World const deliveryWorld = DeliveryVariant();
    QuestIndex const deliveryQuests = QuestIndex::Build(deliveryWorld.gather);
    PlannerIndexes const deliveryIndexes = PlannerIndexes::Build(deliveryWorld.services);
    CircuitHarness::Scenario dearDelivery = cheapDelivery;
    dearDelivery.name = "goldshire-12-full-log-dear-delivery";
    dearDelivery.context.quests = &deliveryQuests;
    dearDelivery.context.indexes = &deliveryIndexes;
    dearDelivery.bot.quests.clear();
    for (std::uint32_t id = 330; id < 340; ++id)
        dearDelivery.bot.quests.push_back(Slot(id));
    for (std::uint32_t id = 350; id < 363; ++id)
        dearDelivery.bot.quests.push_back(Slot(id));
    dearDelivery.expect.take = {101, 102, 103, 104, 105, 110, 114, 117, 124};
    dearDelivery.expect.notTake = {};
    dearDelivery.expect.carried = {117};
    CircuitHarness::ExactAbandon(dearDelivery.expect, {330, 331, 332, 333, 334, 335, 336, 337, 338});
    for (std::uint32_t id = 350; id < 363; ++id)
        dearDelivery.expect.keep.push_back(id);
    Run(dearDelivery, false);

    // Log quests the quest index does not know (WT14 fix): they would hold their slots for good, so they go; a
    // protected one stays
    CircuitHarness::Scenario unindexed = goldshire;
    unindexed.name = "goldshire-12-unindexed";
    unindexed.bot.quests.push_back(Slot(999));
    unindexed.bot.quests.push_back(Slot(998, Circuit::SLOT_COMPLETE));
    unindexed.bot.quests.push_back(Slot(997));
    unindexed.context.protectedQuests.push_back(997);
    CircuitHarness::ExactAbandon(unindexed.expect, {121, 998, 999});
    unindexed.expect.keep = {122, 123, 997};
    Run(unindexed, false);
    {
        CircuitPlan const plan = PlanCircuit(unindexed.bot, unindexed.context);
        std::size_t named = 0;
        for (CircuitAbandon const& abandon : plan.abandon)
            named += (abandon.quest == 998 || abandon.quest == 999) && abandon.reason == AbandonReason::Unindexed;
        Check(named == 2, unindexed.name + ": 998 and 999 abandoned as not in the quest index");
        std::vector<CircuitLog::Row> const rows = CircuitLog::Rows(plan);
        Check(std::any_of(rows.begin(), rows.end(), [](CircuitLog::Row const& row)
                          { return row.choice == "abandon:999" && row.reason == "unindexed"; }),
              unindexed.name + ": decision row abandon:999 unindexed");
    }

    // A full log of done quests with a chain nearer than the deliveries (review 4, finding 1): 112, 117 and 201 end
    // at Sentinel Hill, the rest on Kalimdor; done quests free no slot at Eastvale, so the chain 130 there offers
    // nothing a full log could take, and the bot delivers at Sentinel Hill instead of walking between the two
    CircuitHarness::Scenario fullDeliver{
        "goldshire-12-full-log-deliver", Bot(12, HUMAN, WARRIOR, 0, EASTERN, GOLDSHIRE), base, {}};
    fullDeliver.bot.rewardedQuests = {113, 129};
    for (std::uint32_t id = 330; id < 350; ++id)
        fullDeliver.bot.quests.push_back(Slot(id, Circuit::SLOT_COMPLETE));
    for (std::uint32_t const id : {112u, 117u, 201u})
        fullDeliver.bot.quests.push_back(Slot(id, Circuit::SLOT_COMPLETE));
    fullDeliver.expect.exactTake = true;
    fullDeliver.expect.minTake = 0;
    CircuitHarness::ExactAbandon(fullDeliver.expect, {});
    fullDeliver.expect.carried = {112, 117, 201};
    fullDeliver.expect.checkMove = true;
    fullDeliver.expect.move = MoveReason::Delivery;
    fullDeliver.expect.moveCamp = CampAt(quests, EASTERN, SENTINEL_HILL);
    Run(fullDeliver, false);

    // The same bot at Eastvale, where the chain back to Goldshire waits: it delivers at Sentinel Hill as well
    CircuitHarness::Scenario fullEastvale = fullDeliver;
    fullEastvale.name = "eastvale-12-full-log-deliver";
    fullEastvale.bot.position = Bot(12, HUMAN, WARRIOR, 0, EASTERN, EASTVALE).position;
    Run(fullEastvale, false);

    // The log full of done quests whose enders are all on another map: no delivery is in reach, and no slot frees at a
    // chain or camp here, so the plan finds nowhere (the other goals take over, WT14)
    CircuitHarness::Scenario fullFar = fullDeliver;
    fullFar.name = "goldshire-12-full-log-far";
    fullFar.bot.quests.resize(20);
    for (std::uint32_t id = 350; id < 353; ++id)
        fullFar.bot.quests.push_back(Slot(id, Circuit::SLOT_COMPLETE));
    fullFar.expect.carried = {};
    fullFar.expect.move = MoveReason::Nowhere;
    fullFar.expect.moveCamp = NO_CAMP;
    Run(fullFar, false);

    // The same hub once every quest there is done: the chain leads to Eastvale
    CircuitHarness::Scenario goldshireDry{"goldshire-dry", Bot(12, HUMAN, WARRIOR, 0, EASTERN, GOLDSHIRE), base, {}};
    for (std::uint32_t id = 101; id <= 120; ++id)
        goldshireDry.bot.rewardedQuests.push_back(id);
    goldshireDry.bot.rewardedQuests.push_back(124);
    goldshireDry.bot.rewardedQuests.push_back(129);
    goldshireDry.expect.minTake = 0;
    goldshireDry.expect.checkMove = true;
    goldshireDry.expect.move = MoveReason::Chain;
    goldshireDry.expect.moveCamp = CampAt(quests, EASTERN, EASTVALE);
    Run(goldshireDry, false);

    // With nothing left to walk for here, the lone delivery is turned in on the loop: left for later, never dropped
    CircuitHarness::Scenario dryLone = goldshireDry;
    dryLone.name = "goldshire-dry-lone-delivery";
    dryLone.bot.quests = {Slot(125, Circuit::SLOT_COMPLETE)};
    dryLone.expect.turnIns = {125};
    Run(dryLone, false);

    // A dry hub with no chain: the bot carries the done 117 to Sentinel Hill
    CircuitHarness::Scenario delivery{
        "goldshire-dry-delivery", Bot(12, HUMAN, WARRIOR, 0, EASTERN, GOLDSHIRE), base, {}};
    for (std::uint32_t id = 101; id <= 124; ++id)
        if (id != 112 && id != 117)
            delivery.bot.rewardedQuests.push_back(id);
    delivery.bot.quests = {Slot(117, Circuit::SLOT_COMPLETE)};
    delivery.expect.minTake = 0;
    delivery.expect.carried = {117};
    delivery.expect.checkMove = true;
    delivery.expect.move = MoveReason::Delivery;
    delivery.expect.moveCamp = CampAt(quests, EASTERN, SENTINEL_HILL);
    Run(delivery, false);

    // The dry hub with a log full of quests it cannot work from here (review 5, finding 1): their objectives lie on
    // Kalimdor, so any hub that wants a slot gives one of them up. The chain 130 is worth the walk to Eastvale; there
    // the bot gives up 330 for it
    CircuitHarness::Scenario dryFull = goldshireDry;
    dryFull.name = "goldshire-dry-full-log-far";
    for (std::uint32_t id = 330; id < 353; ++id)
        dryFull.bot.quests.push_back(Slot(id));
    CircuitHarness::ExactAbandon(dryFull.expect, {});
    Run(dryFull, false);

    CircuitHarness::Scenario dryFullThere = dryFull;
    dryFullThere.name = "eastvale-dry-full-log-far";
    dryFullThere.bot.position = Bot(12, HUMAN, WARRIOR, 0, EASTERN, EASTVALE).position;
    dryFullThere.expect.exactTake = true;
    dryFullThere.expect.take = {130};
    CircuitHarness::ExactAbandon(dryFullThere.expect, {330});
    dryFullThere.expect.checkMove = false;
    Run(dryFullThere, false);

    // Twenty done quests whose enders are on Kalimdor and three incomplete ones: the incomplete ones free a slot
    CircuitHarness::Scenario dryMixed = dryFull;
    dryMixed.name = "goldshire-dry-full-log-mixed";
    dryMixed.bot.quests.clear();
    for (std::uint32_t id = 330; id < 350; ++id)
        dryMixed.bot.quests.push_back(Slot(id, Circuit::SLOT_COMPLETE));
    for (std::uint32_t id = 350; id < 353; ++id)
        dryMixed.bot.quests.push_back(Slot(id));
    Run(dryMixed, false);

    // Elite and group quests the bot holds solo free a slot as well
    CircuitHarness::Scenario drySolo = dryFull;
    drySolo.name = "goldshire-dry-full-log-solo-elite";
    drySolo.bot.quests.resize(20);
    for (std::uint32_t const id : {106u, 107u, 123u})
        drySolo.bot.quests.push_back(Slot(id));
    std::erase_if(drySolo.bot.rewardedQuests, [](std::uint32_t id) { return id == 106 || id == 107; });
    Run(drySolo, false);

    // The same log in the field, no giver within reach: nothing is offered here, the chain still leads to Eastvale
    CircuitHarness::Scenario dryField = dryFull;
    dryField.name = "elwynn-field-full-log-far";
    dryField.bot.position = Bot(12, HUMAN, WARRIOR, 0, EASTERN, Offset(ELWYNN_NW, 0.0f, -100.0f)).position;
    Run(dryField, false);

    // A chain whose quest the bot would not take on arrival (review 5, finding 2): 114 here and 130 at Eastvale are
    // both too risky by the statistics, so neither is worth the walk; the bot moves to Sentinel Hill in its bracket
    QuestStatsView riskyStats = stats;
    Stat(riskyStats, 114, 12, 5.0f, 2.0f);
    Stat(riskyStats, 130, 12, 5.0f, 2.0f);
    CircuitHarness::Scenario riskyChain{
        "goldshire-dry-risky-chain", Bot(12, HUMAN, WARRIOR, 0, EASTERN, GOLDSHIRE), base, {}};
    riskyChain.context.stats = &riskyStats;
    for (std::uint32_t id = 101; id <= 120; ++id)
        if (id != 114 && id != 115)
            riskyChain.bot.rewardedQuests.push_back(id);
    riskyChain.bot.rewardedQuests.push_back(124);
    riskyChain.bot.rewardedQuests.push_back(129);
    riskyChain.expect.minTake = 0;
    riskyChain.expect.checkMove = true;
    riskyChain.expect.move = MoveReason::Bracket;
    riskyChain.expect.moveCamp = CampAt(quests, EASTERN, SENTINEL_HILL);
    Run(riskyChain, false);

    // The same chain with its objectives beyond FAR_YARDS of the givers, and without an area
    World const farWorld = ChainVariant(false);
    World const unplacedWorld = ChainVariant(true);
    QuestIndex const farQuests = QuestIndex::Build(farWorld.gather);
    QuestIndex const unplacedQuests = QuestIndex::Build(unplacedWorld.gather);
    PlannerIndexes const farIndexes = PlannerIndexes::Build(farWorld.services);
    PlannerIndexes const unplacedIndexes = PlannerIndexes::Build(unplacedWorld.services);
    CircuitHarness::Scenario farChain = riskyChain;
    farChain.name = "goldshire-dry-far-chain";
    farChain.context.quests = &farQuests;
    farChain.context.indexes = &farIndexes;
    farChain.context.stats = &stats;
    farChain.expect.moveCamp = CampAt(farQuests, EASTERN, SENTINEL_HILL);
    Run(farChain, false);
    CircuitHarness::Scenario unplacedChain = farChain;
    unplacedChain.name = "goldshire-dry-unplaced-chain";
    unplacedChain.context.quests = &unplacedQuests;
    unplacedChain.context.indexes = &unplacedIndexes;
    unplacedChain.expect.moveCamp = CampAt(unplacedQuests, EASTERN, SENTINEL_HILL);
    Run(unplacedChain, false);

    // A level 22 orc at the Crossroads: overlapping kill areas, a risky quest, statistics, overtime and deaths
    CircuitHarness::Scenario crossroads{"crossroads-22-orc", Bot(22, ORC, WARRIOR, 1, KALIMDOR, CROSSROADS), base, {}};
    crossroads.bot.quests = {Slot(320, 0, 3), Slot(321), Slot(322), Slot(323, 0, 5)};
    crossroads.context.protectedQuests = {322};
    crossroads.context.progress = {{320, 120.0f, 0}, {321, 10.0f, 6}, {322, 200.0f, 0}};
    crossroads.expect.exactTake = true;
    crossroads.expect.take = {301, 302, 303, 304, 305, 307};
    crossroads.expect.notTake = {306, 308, 309, 310, 311};
    CircuitHarness::ExactAbandon(crossroads.expect, {320, 321});
    crossroads.expect.keep = {322};
    crossroads.expect.together = {{301, 302, 323}, {303, 304}, {305, 307}};
    Run(crossroads, true);

    CircuitHarness::Scenario grouped = crossroads;
    grouped.name = "crossroads-22-orc-in-group";
    grouped.bot.groupSize = 3;
    grouped.expect.take = {301, 302, 303, 304, 305, 307, 311};
    grouped.expect.notTake = {306, 308, 309, 310};
    Run(grouped, false);

    // A full log: the quests it can do around the hub are kept, three far ones make room for the hub's best, the
    // rest of the hub waits for log space (307 ranks high: its median is 4 minutes)
    CircuitHarness::Scenario full = crossroads;
    full.name = "crossroads-22-orc-full-log";
    std::vector<std::uint32_t> nearLog;
    for (std::uint32_t id = 350; id < 367; ++id)
    {
        full.bot.quests.push_back(Slot(id));
        nearLog.push_back(id);
    }
    for (std::uint32_t id = 330; id < 333; ++id)
        full.bot.quests.push_back(Slot(id));
    full.expect.take = {301, 303, 304, 307};
    full.expect.notTake = {302, 305};
    CircuitHarness::ExactAbandon(full.expect, {320, 321, 330, 331, 332});
    full.expect.keep = nearLog;
    full.expect.keep.push_back(322);
    full.expect.together = {};
    Run(full, false);

    CircuitHarness::Scenario crossroadsDry{"crossroads-dry", Bot(22, ORC, WARRIOR, 1, KALIMDOR, CROSSROADS), base, {}};
    for (std::uint32_t id = 301; id <= 323; ++id)
        crossroadsDry.bot.rewardedQuests.push_back(id);
    for (std::uint32_t id = 350; id < 370; ++id)
        crossroadsDry.bot.rewardedQuests.push_back(id);
    crossroadsDry.expect.minTake = 0;
    crossroadsDry.expect.checkMove = true;
    crossroadsDry.expect.move = MoveReason::Bracket;
    crossroadsDry.expect.moveCamp = CampAt(quests, KALIMDOR, STONETALON_QUIET);
    Run(crossroadsDry, false);

    // The same dry hub with a log full of quests from Stranglethorn (another map; review 5, finding 1): the camp in the
    // bracket is still worth the walk, there two of them make room for 501 and 502
    CircuitHarness::Scenario crossroadsDryFull = crossroadsDry;
    crossroadsDryFull.name = "crossroads-dry-full-log-far";
    for (std::uint32_t const id : {401u, 402u, 403u, 404u})
        crossroadsDryFull.bot.quests.push_back(Slot(id));
    for (std::uint32_t id = 430; id < 449; ++id)
        crossroadsDryFull.bot.quests.push_back(Slot(id));
    CircuitHarness::ExactAbandon(crossroadsDryFull.expect, {});
    Run(crossroadsDryFull, false);

    // A level 35 troll at Grom'gol with a nearly full log: the lowest-value quests it cannot do from here (the solo
    // elite and group quests, then far ones) make room; the protected far 430 stays
    CircuitHarness::Scenario gromgol{"gromgol-35", Bot(35, TROLL, HUNTER, 1, EASTERN, GROMGOL), base, {}};
    gromgol.bot.quests = {Slot(420), Slot(421)};
    for (std::uint32_t id = 430; id < 449; ++id)
        gromgol.bot.quests.push_back(Slot(id));
    gromgol.context.protectedQuests = {430};
    gromgol.expect.exactTake = true;
    gromgol.expect.take = {401, 402, 403, 404, 405, 406};
    CircuitHarness::ExactAbandon(gromgol.expect, {420, 421, 431, 432});
    gromgol.expect.keep = {430};
    gromgol.expect.together = {{401, 402}, {403, 404}, {405, 406}};
    Run(gromgol, true);

    // The same log with 448 done: its turn-in on this trip frees a slot, so one far quest fewer gives its slot and the
    // lowest-ranked hub quest (404) waits for that turn-in
    CircuitHarness::Scenario gromgolTurnIn = gromgol;
    gromgolTurnIn.name = "gromgol-35-turn-in-frees";
    gromgolTurnIn.bot.quests.back() = Slot(448, Circuit::SLOT_COMPLETE);
    gromgolTurnIn.expect.take = {401, 402, 403, 405, 406};
    gromgolTurnIn.expect.notTake = {404};
    CircuitHarness::ExactAbandon(gromgolTurnIn.expect, {420, 421, 431});
    gromgolTurnIn.expect.together = {};
    gromgolTurnIn.expect.turnIns = {448};
    Run(gromgolTurnIn, false);

    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
