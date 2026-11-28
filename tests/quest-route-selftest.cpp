/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the quest route (questing WQ17c, QuestRoute.h, QuestOrderTypes.h): generated logs over made-up maps
// (the seed is printed; argv[1] replays it, argv[2] prints that log's route) checked against the step's invariants,
// plus hand-built cases whose sums are worked by hand. Each route is replayed stop by stop with the log as the plan
// expects it to be (Route::Advance), and every stop is asked again of the field map with the harness's own questions.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel tests/quest-route-selftest.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "GoalInterface.h"
#include "GoalRoute.h"
#include "QuestRoute.h"
#include "QuestTakeFixture.h"

using namespace Planner;
using namespace Planner::QuestFields;

namespace
{
constexpr std::uint32_t DEFAULT_SEED = 20261125;
constexpr std::size_t LOGS = 800;
constexpr std::size_t NO_LOG = static_cast<std::size_t>(-1);
constexpr std::uint32_t MAP_A = 0;
constexpr std::uint32_t MAP_B = 1;
constexpr std::int32_t CELL_LOW = -6;  // the made-up maps span cells -6 to 5: a cell border and negative coordinates
constexpr std::int32_t CELL_HIGH = 5;
constexpr std::uint32_t FIRST_CREATURE = 100;
constexpr std::uint32_t CREATURES = 24;
constexpr std::uint32_t FIRST_OBJECT = 500;
constexpr std::uint32_t OBJECTS = 8;
constexpr std::uint32_t FIRST_NPC = 700;  // friendly creatures: givers and enders
constexpr std::uint32_t NPCS = 8;
constexpr std::uint32_t FIRST_ITEM = 2000;
constexpr std::uint32_t ITEMS = 8;
constexpr std::uint32_t FIRST_QUEST = 10;
constexpr std::uint8_t HUMAN = 1;  // a race of each side
constexpr std::uint8_t ORC = 2;
constexpr float SLACK = 1.0e-4f;                 // relative slack of float comparisons
constexpr std::uint32_t FAR_SALT = 0x5EEDFA12u;  // the far worlds' seed: the given seed with these bits flipped

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

bool Near(float a, double b) { return std::fabs(static_cast<double>(a) - b) <= 1e-3 * std::max(1.0, std::fabs(b)); }

bool AtLeast(float a, float b) { return a >= b - SLACK * std::max(1.0f, std::fabs(b)); }

// One invariant over every generated log: the first violation names its log so it replays alone
struct Invariant
{
    explicit Invariant(char const* text) : name(text) {}

    char const* name;
    std::uint32_t checked{0};
    std::uint32_t broken{0};
    std::size_t firstLog{0};
    std::string firstWhat;

    void Note(bool ok, std::size_t log, std::string const& what)
    {
        ++checked;
        if (ok)
            return;
        if (!broken++)
        {
            firstLog = log;
            firstWhat = what;
        }
    }
};

void Report(Invariant const& invariant)
{
    std::printf("  %7u  %s\n", invariant.checked, invariant.name);
    Check(invariant.checked > 0, std::string(invariant.name) + ": never exercised");
    Check(invariant.broken == 0, std::string(invariant.name) + ": " + std::to_string(invariant.broken) + " of " +
                                     std::to_string(invariant.checked) + " broken, first in log " +
                                     std::to_string(invariant.firstLog) + " (" + invariant.firstWhat + ")");
}

struct Gen
{
    std::mt19937 random;

    int Int(int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); }
    float Real(float low, float high) { return std::uniform_real_distribution<float>(low, high)(random); }
    bool Chance(float p) { return Real(0.0f, 1.0f) < p; }
    std::uint32_t Pick(std::uint32_t first, std::uint32_t count)
    {
        return first + static_cast<std::uint32_t>(Int(0, static_cast<int>(count) - 1));
    }
};

struct World
{
    std::vector<FieldSpawn> spawns;
    FieldMap map;
    QuestIndex index;
    std::vector<QuestSlot> log;
    std::vector<Need> needs;            // the log's, then the takes
    std::vector<std::uint64_t> closed;  // sorted
    Route::Ask ask;                     // without the closed cells: AskOf adds them
};

Route::Ask AskOf(World const& w)
{
    Route::Ask ask = w.ask;
    ask.closed = w.closed;
    return ask;
}

struct Home
{
    std::uint32_t mapId{MAP_A};
    std::int32_t cellX{0};
    std::int32_t cellY{0};
    std::uint8_t level{1};
};

FieldSpawn MakeSpawn(FieldKind kind, std::uint32_t entry, std::uint32_t mapId, float x, float y, std::uint8_t minLevel,
                     std::uint8_t maxLevel, bool hostile, std::uint32_t respawn)
{
    FieldSpawn spawn;
    spawn.kind = kind;
    spawn.entry = entry;
    spawn.mapId = mapId;
    spawn.x = x;
    spawn.y = y;
    spawn.minLevel = minLevel;
    spawn.maxLevel = maxLevel;
    spawn.hostile = hostile;
    spawn.respawnSecs = respawn;
    return spawn;
}

// Cell-centred coordinates
float Mid(std::int32_t cell) { return (static_cast<float>(cell) + 0.5f) * FIELD_CELL_YARDS; }

LootRow MakeLoot(std::uint32_t lootId, std::uint32_t item, float percent)
{
    LootRow row;
    row.lootId = lootId;
    row.item = item;
    row.chance = percent;
    return row;
}

QuestObjective MakeObjective(ObjectiveKind kind, std::uint32_t target, std::uint32_t count, std::int32_t poi)
{
    QuestObjective objective;
    objective.kind = kind;
    objective.target = target;
    objective.count = count;
    objective.poiIndex = poi;
    return objective;
}

QuestRecord MakeQuest(std::uint32_t id, std::uint32_t xp, std::vector<QuestObjective> objectives)
{
    QuestRecord quest;
    quest.id = id;
    quest.xpBase = xp;
    quest.objectives = std::move(objectives);
    return quest;
}

QuestPlace MakePlace(std::uint32_t entry, std::uint32_t mapId, std::int32_t cellX, std::int32_t cellY)
{
    QuestPlace place;
    place.mapId = mapId;
    place.x = Mid(cellX);
    place.y = Mid(cellY);
    place.entry = entry;
    return place;
}

void AddSpawns(std::vector<FieldSpawn>& spawns, std::uint32_t entry, std::int32_t cellX, std::int32_t cellY, int count,
               std::uint8_t minLevel, std::uint8_t maxLevel, std::uint32_t respawn)
{
    for (int i = 0; i < count; ++i)
        spawns.push_back(MakeSpawn(FieldKind::Creature, entry, MAP_A, Mid(cellX) + static_cast<float>(i), Mid(cellY),
                                   minLevel, maxLevel, true, respawn));
}

// A made-up map (creatures and objects around their homes, friendly NPCs at theirs), quests with givers and enders at
// the NPCs, a log (empty in some worlds), the takes the rules allow, a bot (in some worlds at the ender of a ready
// quest), closed cells in some, and the route's tuning values at their defaults or varied
World Generate(Gen& gen)
{
    World w;
    std::uint8_t const team = static_cast<std::uint8_t>(gen.Int(0, 1));
    std::uint8_t const level = static_cast<std::uint8_t>(gen.Int(1, 60));
    std::vector<Home> creatureHomes(CREATURES);
    std::vector<Home> objectHomes(OBJECTS);
    std::vector<Home> npcHomes(NPCS);
    auto const home = [&gen, level](Home& h)
    {
        h.mapId = gen.Chance(0.85f) ? MAP_A : MAP_B;
        h.cellX = gen.Int(CELL_LOW, CELL_HIGH);
        h.cellY = gen.Int(CELL_LOW, CELL_HIGH);
        h.level = static_cast<std::uint8_t>(gen.Int(std::max(1, level - 8), level + 7));
    };
    for (Home& h : creatureHomes)
        home(h);
    for (Home& h : objectHomes)
        home(h);
    for (Home& h : npcHomes)
        home(h);

    int const spawnCount = gen.Int(30, 160);
    for (int i = 0; i < spawnCount; ++i)
    {
        int const roll = gen.Int(0, 99);
        FieldKind kind = FieldKind::Creature;
        std::uint32_t entry = 0;
        Home const* h = nullptr;
        bool hostile = false;
        if (roll < 70)
        {
            std::uint32_t const at = static_cast<std::uint32_t>(gen.Int(0, CREATURES - 1));
            entry = FIRST_CREATURE + at;
            h = &creatureHomes[at];
            hostile = gen.Chance(0.8f);
        }
        else if (roll < 85)
        {
            std::uint32_t const at = static_cast<std::uint32_t>(gen.Int(0, OBJECTS - 1));
            kind = FieldKind::Object;
            entry = FIRST_OBJECT + at;
            h = &objectHomes[at];
        }
        else
        {
            std::uint32_t const at = static_cast<std::uint32_t>(gen.Int(0, NPCS - 1));
            entry = FIRST_NPC + at;
            h = &npcHomes[at];
        }
        float const x = (static_cast<float>(h->cellX + gen.Int(-1, 1)) + gen.Real(0.01f, 0.99f)) * FIELD_CELL_YARDS;
        float const y = (static_cast<float>(h->cellY + gen.Int(-1, 1)) + gen.Real(0.01f, 0.99f)) * FIELD_CELL_YARDS;
        std::uint8_t const minLevel = kind == FieldKind::Object ? 0 : h->level;
        std::uint8_t const maxLevel =
            kind == FieldKind::Object ? 0 : static_cast<std::uint8_t>(minLevel + gen.Int(0, 3));
        FieldSpawn spawn = MakeSpawn(kind, entry, h->mapId, x, y, minLevel, maxLevel, hostile,
                                     gen.Chance(0.1f) ? 0 : static_cast<std::uint32_t>(gen.Int(30, 900)));
        spawn.elite = hostile && gen.Chance(0.08f);
        spawn.share = gen.Chance(0.8f) ? 1.0f : 0.5f;
        w.spawns.push_back(spawn);
    }
    for (std::uint32_t at = 0; at < NPCS; ++at)
        if (gen.Chance(0.8f))
        {
            Home const& h = npcHomes[at];
            FieldSpawn spawn =
                MakeSpawn(FieldKind::Creature, FIRST_NPC + at, h.mapId, Mid(h.cellX) + gen.Real(-40.0f, 40.0f),
                          Mid(h.cellY) + gen.Real(-40.0f, 40.0f), h.level, h.level, false, 300);
            spawn.share = gen.Chance(0.9f) ? 1.0f : 0.3f;  // a pooled NPC: less than half a hand-in expected
            w.spawns.push_back(spawn);
        }
    w.map = FieldMap::Build(w.spawns);

    SourceGather gather;
    std::set<std::pair<std::uint32_t, std::uint32_t>> used;
    std::set<std::pair<SourceKind, std::uint32_t>> owners;
    for (std::uint32_t item = FIRST_ITEM; item < FIRST_ITEM + ITEMS; ++item)
        for (int s = gen.Int(1, 2); s > 0; --s)
        {
            SourceKind const kind = gen.Chance(0.8f) ? SourceKind::Corpse : SourceKind::Object;
            std::uint32_t const owner =
                kind == SourceKind::Object ? gen.Pick(FIRST_OBJECT, OBJECTS) : gen.Pick(FIRST_CREATURE, CREATURES);
            if (!used.insert({owner, item}).second)
                continue;
            gather.tables[static_cast<std::size_t>(kind)].push_back(MakeLoot(owner, item, gen.Real(3.0f, 100.0f)));
            if (owners.insert({kind, owner}).second)
            {
                gather.owners.push_back({kind, owner, owner});
                SourceSpawn spawn;
                spawn.entry = owner;
                spawn.isObject = kind == SourceKind::Object;
                gather.spawns.push_back(spawn);
            }
        }
    w.index.sources = SourceTable::Build(gather);

    bool const emptyLog = gen.Chance(0.15f);
    int const questCount = gen.Int(1, 8);
    for (int q = 0; q < questCount; ++q)
    {
        QuestRecord quest;
        quest.id = FIRST_QUEST + static_cast<std::uint32_t>(q) * 3;
        quest.xpBase = gen.Chance(0.05f) ? 0 : static_cast<std::uint32_t>(gen.Int(1, 2000));
        quest.copperReward = gen.Int(-200, 5000);
        std::int32_t slots = 0;
        for (int j = gen.Chance(0.1f) ? 0 : gen.Int(1, 3); j > 0; --j)
        {
            int const roll = gen.Int(0, 9);
            std::uint32_t const at = static_cast<std::uint32_t>(quest.objectives.size());
            if (roll < 4)
                quest.objectives.push_back(MakeObjective(ObjectiveKind::Kill, gen.Pick(FIRST_CREATURE, CREATURES),
                                                         static_cast<std::uint32_t>(gen.Int(1, 12)), slots++));
            else if (roll < 6)
            {
                QuestObjective objective =
                    MakeObjective(ObjectiveKind::Item, gen.Pick(FIRST_ITEM, ITEMS),
                                  static_cast<std::uint32_t>(gen.Int(1, 8)), POI_FIRST_ITEM + static_cast<int>(at));
                objective.sourceKinds = SourceKindsOf(w.index.SourcesOf(objective.target));
                quest.objectives.push_back(objective);
            }
            else if (roll < 8)
                quest.objectives.push_back(MakeObjective(ObjectiveKind::Object, gen.Pick(FIRST_OBJECT, OBJECTS),
                                                         static_cast<std::uint32_t>(gen.Int(1, 6)), slots++));
            else if (roll < 9)
                quest.objectives.push_back(MakeObjective(ObjectiveKind::Explore, 4000 + at, 1, POI_NONE));
            else
                quest.objectives.push_back(MakeObjective(ObjectiveKind::Turnin, 0, 0, POI_TURNIN));
        }
        for (int e = gen.Int(0, 2); e > 0; --e)
        {
            std::uint32_t const at = static_cast<std::uint32_t>(gen.Int(0, NPCS - 1));
            quest.enders.push_back(static_cast<std::uint32_t>(w.index.places.size()));
            w.index.places.push_back(
                MakePlace(FIRST_NPC + at, npcHomes[at].mapId, npcHomes[at].cellX, npcHomes[at].cellY));
        }
        for (int g = gen.Int(0, 2); g > 0; --g)
        {
            std::uint32_t const at = static_cast<std::uint32_t>(gen.Int(0, NPCS - 1));
            QuestPlace place = MakePlace(FIRST_NPC + at, npcHomes[at].mapId, npcHomes[at].cellX, npcHomes[at].cellY);
            place.hostileTo = gen.Chance(0.05f) ? static_cast<std::uint8_t>(1u << team) : std::uint8_t{0};
            quest.givers.push_back(static_cast<std::uint32_t>(w.index.places.size()));
            w.index.places.push_back(place);
        }
        quest.minLevel = static_cast<std::uint32_t>(std::max(1, level - gen.Int(-1, 6)));
        quest.level = gen.Chance(0.1f) ? -1 : std::max(1, level + gen.Int(-6, 4));
        if (gen.Chance(0.05f))
            quest.flags |= QuestFlag::ELITE;
        if (gen.Chance(0.1f))
            quest.exclusiveGroup = gen.Int(1, 3);
        if (gen.Chance(0.1f))
            quest.prev = static_cast<std::int32_t>(FIRST_QUEST + 3 * gen.Int(0, 7));
        if (!emptyLog && gen.Chance(0.6f))
        {
            QuestSlot slot;
            slot.quest = quest.id;
            slot.state = gen.Chance(0.25f) ? Circuit::SLOT_COMPLETE : 0;
            for (std::uint16_t& counter : slot.counters)
                counter = static_cast<std::uint16_t>(gen.Int(0, 14));
            w.log.push_back(slot);
        }
        w.index.quests.push_back(std::move(quest));
    }

    Pricing const pricing{};
    w.needs = Needs(w.index, w.log, team, pricing, 5);  // WQ38: as a party asks, every need of the log

    // The bot: anywhere, or at the spawn of a ready quest's ender (register row G344)
    Route::Ask& ask = w.ask;
    ask.mapId = gen.Chance(0.85f) ? MAP_A : MAP_B;
    ask.x = (static_cast<float>(gen.Int(CELL_LOW, CELL_HIGH)) + gen.Real(0.0f, 1.0f)) * FIELD_CELL_YARDS;
    ask.y = (static_cast<float>(gen.Int(CELL_LOW, CELL_HIGH)) + gen.Real(0.0f, 1.0f)) * FIELD_CELL_YARDS;
    if (gen.Chance(0.35f))
    {
        std::vector<FieldSpawn const*> enders;
        for (Need const& need : w.needs)
            if (need.kind == NeedKind::HandIn)
                for (NeedSource const& source : need.sources)
                    for (FieldSpawn const& spawn : w.spawns)
                        if (spawn.kind == source.kind && spawn.entry == source.entry)
                            enders.push_back(&spawn);
        if (!enders.empty())
        {
            FieldSpawn const& at = *enders[static_cast<std::size_t>(gen.Int(0, static_cast<int>(enders.size()) - 1))];
            ask.mapId = at.mapId;
            ask.x = at.x;
            ask.y = at.y;
        }
    }
    ask.level = level;
    ask.team = team;
    ask.runSpeed = gen.Chance(0.5f) ? 0.0f : gen.Real(5.0f, 10.0f);
    ask.caution = gen.Chance(0.8f) ? 1.0f : gen.Real(0.5f, 2.0f);
    ask.groupSize = gen.Chance(0.2f) ? 2 : 0;
    ask.stayMinutes = gen.Chance(0.7f) ? Route::STAY_MINUTES : gen.Real(1.0f, 10.0f);
    ask.radius = gen.Chance(0.7f) ? FIELD_WALK_YARDS : gen.Real(150.0f, 900.0f);
    ask.horizonMinutes = gen.Chance(0.7f) ? Route::HORIZON_MINUTES : gen.Real(2.0f, 60.0f);
    ask.maxStops = gen.Chance(0.6f) ? Route::MAX_STOPS : static_cast<std::size_t>(gen.Int(1, 6));

    // The takes: the quests the rules allow the bot, at their givers on its map
    std::vector<std::uint32_t> inLog;
    for (QuestSlot const& slot : w.log)
        inLog.push_back(slot.quest);
    std::sort(inLog.begin(), inLog.end());
    std::vector<std::uint32_t> rewarded;
    std::vector<std::uint32_t> candidates;
    for (QuestRecord const& quest : w.index.quests)
    {
        candidates.push_back(quest.id);
        if (!std::binary_search(inLog.begin(), inLog.end(), quest.id) && gen.Chance(0.2f))
            rewarded.push_back(quest.id);
    }
    std::vector<std::uint32_t> const answer =
        TakeFixture::Answer(w.index, team ? ORC : HUMAN, 1, level, rewarded, inLog);
    Take::Facts facts;
    facts.race = team ? ORC : HUMAN;
    facts.level = level;
    facts.rewarded = rewarded;
    facts.inLog = inLog;
    facts.takeable = answer;
    for (Need& take : TakeNeeds(w.index, facts, ask.mapId, candidates, pricing))
        w.needs.push_back(std::move(take));

    if (gen.Chance(0.4f))
    {
        for (int c = gen.Int(1, 4); c > 0; --c)
            w.closed.push_back(
                FieldCellKey(ask.mapId, gen.Int(CELL_LOW - 1, CELL_HIGH + 1), gen.Int(CELL_LOW - 1, CELL_HIGH + 1)));
        if (gen.Chance(0.2f))
            w.closed.push_back(FieldCellKeyAt(ask.mapId, ask.x, ask.y));
        std::sort(w.closed.begin(), w.closed.end());
        w.closed.erase(std::unique(w.closed.begin(), w.closed.end()), w.closed.end());
    }
    return w;
}

std::string RouteText(Route::Plan const& plan)
{
    std::string text;
    char line[256];
    for (Route::Stop const& stop : plan.stops)
    {
        QuestOrder const& order = stop.order;
        std::snprintf(line, sizeof(line), "%s/%s %llx (%.3f,%.3f) walk=%.6f stay=%.6f worth=%.6f rate=%.6f leave=%.6f:",
                      QuestOrderWordName(order.word), QuestOrderEndName(order.end),
                      static_cast<unsigned long long>(order.cell), static_cast<double>(order.x),
                      static_cast<double>(order.y), static_cast<double>(order.walkMinutes),
                      static_cast<double>(order.minutes), static_cast<double>(stop.worth),
                      static_cast<double>(order.worthPerMinute), static_cast<double>(order.leaveBelow));
        text += line;
        for (QuestOrderNeed const& expected : order.expected)
        {
            std::snprintf(line, sizeof(line), "%u.%u.%u,", expected.quest, expected.objective, expected.credits);
            text += line;
        }
        text += "|";
    }
    std::snprintf(line, sizeof(line), "worth=%.6f minutes=%.6f", static_cast<double>(plan.worth),
                  static_cast<double>(plan.minutes));
    return text + line;
}

// The harness's own questions of a route, not the header's
FieldAsk FieldAskOf(Route::Ask const& ask, float x, float y)
{
    FieldAsk from;
    from.mapId = ask.mapId;
    from.x = x;
    from.y = y;
    from.radius = ask.radius;
    from.level = ask.level;
    from.stayMinutes = ask.stayMinutes;
    from.runSpeed = ask.runSpeed;
    from.caution = ask.caution;
    from.groupSize = ask.groupSize;
    return from;
}

bool NearCell(std::uint64_t a, std::uint64_t b)
{
    return FieldMapOf(a) == FieldMapOf(b) && std::abs(FieldCellXOf(a) - FieldCellXOf(b)) <= 1 &&
           std::abs(FieldCellYOf(a) - FieldCellYOf(b)) <= 1;
}

bool NearClosed(std::span<std::uint64_t const> closed, std::uint64_t key)
{
    return std::any_of(closed.begin(), closed.end(), [key](std::uint64_t cell) { return NearCell(cell, key); });
}

std::optional<std::uint32_t> OpenNeed(std::vector<Need> const& needs, std::uint32_t quest, std::uint32_t objective)
{
    for (std::uint32_t i = 0; i < needs.size(); ++i)
        if (needs[i].quest == quest && needs[i].objective == objective && needs[i].left)
            return i;
    return std::nullopt;
}

// Whether a ranked field gives the bot work the route counts: a need with credits and a worth
bool Works(Ranked const& field, std::vector<Need> const& needs)
{
    for (NeedOffer const& one : field.offer.needs)
        if (one.credits > 0.0f && needs[one.need].worthPerCredit > 0.0f)
            return true;
    return false;
}

bool Expects(QuestOrder const& order, std::uint32_t quest, std::uint32_t objective)
{
    return std::find(order.expected.begin(), order.expected.end(), QuestOrderNeed{quest, objective, 0}) !=
           order.expected.end();
}

bool SameNeeds(Route::Candidate const& a, Route::Candidate const& b) { return a.needs == b.needs; }

// Questing WQ44, the harness's own reading: a hand-in, or the only open objective need of its quest
bool Favoured(std::vector<Need> const& needs, QuestOrderNeed const& expected)
{
    if (expected.objective == HAND_IN)
        return true;
    if (expected.objective == TAKE)
        return false;
    return std::count_if(needs.begin(), needs.end(), [&expected](Need const& need)
                         { return need.quest == expected.quest && need.objective < TAKE; }) == 1;
}

bool FirstFavoured(Route::Plan const& plan, std::vector<Need> const& needs)
{
    if (plan.stops.empty())
        return false;
    std::vector<QuestOrderNeed> const& expected = plan.stops.front().order.expected;
    return std::any_of(expected.begin(), expected.end(),
                       [&needs](QuestOrderNeed const& one) { return Favoured(needs, one); });
}

void GeneratedLogs(std::uint32_t seed, std::size_t show)
{
    Gen gen{std::mt19937(seed)};
    Invariant work{"every order is work, a hand-in or a take, never a bare walk: open needs expected, a worth, a stay"};
    Invariant risk{"no order for a field the risk rule refuses; every expected need has credits in its field"};
    Invariant shut{"no order in a closed cell or its neighbours"};
    Invariant same{"the same input gives the same route (twice, and from spawns in another order)"};
    Invariant reach{"an open need with a field in reach gives a route, and no field in reach gives none"};
    Invariant giver{"an empty log with a quest to take in reach gets a route whose first order takes at a giver"};
    Invariant atHand{"a ready quest whose ender stands in the bot's cell is handed in by the first order"};
    Invariant longer{"a longer walk to a stop never raises the route's worth per minute or the stop's value"};
    Invariant closing{"closing a cell never leaves or adds an order in it or beside it"};
    Invariant cells{"no two orders of a route share a cell"};
    Invariant rise{"each stop keeps the route's worth per minute from falling"};
    Invariant bounds{"stops begin before the horizon, at most the limit; each walk is straight yards within reach"};
    Invariant sums{"the route's worth and minutes are its stops' sums; rate, leave-below and stay as stated"};
    Invariant thin{"of neighbouring candidates with the same needs only the best stays; the best always stays"};
    Invariant bags{"more quest items never turn a first order with a hand-in or finishing need into one without"};
    Invariant none{"with no quest item the route is that of a default pricing"};
    std::size_t turned = 0;  // logs whose first order the quest items turned to a hand-in or finishing need
    std::size_t routes = 0;
    std::size_t stops = 0;
    std::size_t fieldOrders = 0;
    std::size_t handInOrders = 0;
    std::size_t several = 0;
    std::size_t projected = 0;  // orders expecting a need the log got from an earlier order
    std::size_t thinned = 0;
    for (std::size_t logIndex = 0; logIndex < LOGS; ++logIndex)
    {
        World const w = Generate(gen);
        Route::Ask const ask = AskOf(w);
        Route::Plan const plan = Route::Build(w.map, w.index, w.needs, ask);
        std::string const text = RouteText(plan);
        if (logIndex == show)
            std::printf("log %zu: needs=%zu closed=%zu at (%.1f,%.1f)\n  %s\n", logIndex, w.needs.size(),
                        w.closed.size(), static_cast<double>(ask.x), static_cast<double>(ask.y), text.c_str());
        routes += plan.stops.empty() ? 0 : 1;
        stops += plan.stops.size();
        several += plan.stops.size() > 1 ? 1 : 0;

        std::vector<FieldSpawn> const reversed(w.spawns.rbegin(), w.spawns.rend());
        FieldMap const again = FieldMap::Build(reversed);
        same.Note(RouteText(Route::Build(w.map, w.index, w.needs, ask)) == text &&
                      RouteText(Route::Build(again, w.index, w.needs, ask)) == text,
                  logIndex, "another route");

        // Questing WQ44: the log's needs priced per share of quest items, the same takes after them
        bool favoured = false;
        bool atZero = false;
        for (float const share : {0.0f, 0.25f, 0.5f, 1.0f})
        {
            Pricing pricing{};
            pricing.questItemShare = share;
            std::vector<Need> needs = Needs(w.index, w.log, ask.team, pricing, 5);
            for (Need const& need : w.needs)
                if (need.objective == TAKE)
                    needs.push_back(need);
            Route::Ask held = ask;
            held.pricing = pricing;
            Route::Plan const laden = Route::Build(w.map, w.index, needs, held);
            bool const now = FirstFavoured(laden, needs);
            if (share == 0.0f)
            {
                atZero = now;
                none.Note(RouteText(laden) == text, logIndex, "another route");
            }
            if (favoured)
                bags.Note(now, logIndex, "share " + std::to_string(share));
            favoured = favoured || now;
        }
        turned += !atZero && favoured ? 1 : 0;

        // Reach, the giver and the ender at hand: asked of TopFields with the harness's own rules
        std::uint64_t const here = FieldCellKeyAt(ask.mapId, ask.x, ask.y);
        Ranking const ranking = TopFields(w.map, w.needs, FieldAskOf(ask, ask.x, ask.y));
        bool inReach = false;
        bool around = false;
        for (Ranked const& field : ranking.fields)
            if (!NearClosed(ask.closed, field.offer.centre) && Works(field, w.needs))
            {
                inReach = true;
                around = around || NearCell(field.offer.centre, here);
            }
        reach.Note(inReach == !plan.stops.empty(), logIndex, inReach ? "an empty route" : "a route from nothing");
        if (w.log.empty() && inReach)
        {
            QuestOrder const* first = plan.stops.empty() ? nullptr : &plan.stops.front().order;
            giver.Note(first && first->word == QuestOrderWord::HandIn &&
                           std::any_of(first->expected.begin(), first->expected.end(),
                                       [](QuestOrderNeed const& need) { return need.objective == TAKE; }),
                       logIndex, "no take first");
        }
        if (around)
            for (Need const& need : w.needs)
            {
                if (need.kind != NeedKind::HandIn || !need.left)
                    continue;
                bool stands = false;
                for (NeedSource const& source : need.sources)
                {
                    std::span<std::uint64_t const> const at = w.map.CellsOf(source.kind, source.entry);
                    stands = stands || std::find(at.begin(), at.end(), here) != at.end();
                }
                if (stands)
                    atHand.Note(!plan.stops.empty() && Expects(plan.stops.front().order, need.quest, HAND_IN) &&
                                    NearCell(plan.stops.front().order.cell, here),
                                logIndex, "quest " + std::to_string(need.quest) + " not handed in first");
            }

        // The route, replayed with the log as the plan expects it to be
        Route::Log log{w.needs};
        float x = ask.x;
        float y = ask.y;
        float worth = 0.0f;
        float minutes = 0.0f;
        float before = 0.0f;
        float const speed = ask.runSpeed > 0.0f ? ask.runSpeed : Circuit::DEFAULT_RUN_SPEED;
        std::set<std::uint64_t> seen;
        for (std::size_t k = 0; k < plan.stops.size(); ++k)
        {
            Route::Stop const& stop = plan.stops[k];
            QuestOrder const& order = stop.order;
            std::string const where = "stop " + std::to_string(k);
            (order.word == QuestOrderWord::Field ? fieldOrders : handInOrders) += 1;
            FieldAsk const from = FieldAskOf(ask, x, y);
            Offer const offer = FieldOffer(w.map, log.needs, order.cell, from);
            bool isWork = order.word != QuestOrderWord::Way && order.end == QuestOrderEnd::Done &&
                          !order.expected.empty() && stop.worth > 0.0f && order.minutes > 0.0f &&
                          order.mapId == ask.mapId && order.cell == FieldCellKeyAt(order.mapId, order.x, order.y);
            bool present = true;
            bool atNpc = true;
            bool later = false;
            for (QuestOrderNeed const& expected : order.expected)
            {
                std::optional<std::uint32_t> const at = OpenNeed(log.needs, expected.quest, expected.objective);
                isWork = isWork && at && expected.credits <= log.needs[*at].left;
                bool const mark = expected.objective == HAND_IN || expected.objective == TAKE;
                atNpc = atNpc && mark;
                later = later || (at && *at >= w.needs.size());
                present = present && at &&
                          std::any_of(offer.needs.begin(), offer.needs.end(),
                                      [&at](NeedOffer const& one) { return one.need == *at && one.credits > 0.0f; });
            }
            projected += later ? 1 : 0;
            work.Note(isWork && (order.word == QuestOrderWord::HandIn) == atNpc, logIndex, where);
            risk.Note(present && Goals::RawRisk(offer.risk, from.groupSize) * from.caution <= Goals::RISK_REFUSE,
                      logIndex, where);
            shut.Note(!NearClosed(ask.closed, order.cell), logIndex, where);
            cells.Note(seen.insert(order.cell).second, logIndex, where);
            float const yards = std::hypot(order.x - x, order.y - y);
            bounds.Note(minutes < ask.horizonMinutes && k < ask.maxStops && yards <= ask.radius * (1.0f + SLACK) &&
                            Near(order.walkMinutes, yards / speed / SECONDS_PER_MINUTE),
                        logIndex, where);
            worth += stop.worth;
            minutes += order.walkMinutes + order.minutes;
            float const now = worth / minutes;
            rise.Note(!k || AtLeast(now, before), logIndex, where);
            before = now;
            float const next =
                k + 1 < plan.stops.size()
                    ? plan.stops[k + 1].worth / (plan.stops[k + 1].order.walkMinutes + plan.stops[k + 1].order.minutes)
                    : 0.0f;
            bool const stay = atNpc ? order.minutes <= ask.stayMinutes &&
                                          order.minutes >= std::min(Route::NPC_STAY_FLOOR_MINUTES, ask.stayMinutes)
                                    : order.minutes == ask.stayMinutes;
            sums.Note(stay && Near(order.worthPerMinute, stop.worth / order.minutes) && Near(order.leaveBelow, next) &&
                          order.allowanceYards == ask.allowanceYards &&
                          Near(Route::StopValue(stop.worth, order.walkMinutes, order.minutes),
                               stop.worth / (order.walkMinutes + order.minutes)),
                      logIndex, where);

            Route::Plan far = plan;
            float const extra = gen.Real(0.05f, 5.0f);
            far.stops[k].order.walkMinutes += extra;
            Route::Price(far);
            longer.Note(Route::WorthPerMinute(far) <= Route::WorthPerMinute(plan) &&
                            Route::StopValue(stop.worth, order.walkMinutes + extra, order.minutes) <=
                                Route::StopValue(stop.worth, order.walkMinutes, order.minutes),
                        logIndex, where);

            Route::Advance(w.index, ask, log, order);
            x = order.x;
            y = order.y;
        }
        sums.Note(Near(plan.worth, worth) && Near(plan.minutes, minutes) &&
                      Near(Route::WorthPerMinute(plan), minutes > 0.0f ? worth / minutes : 0.0f),
                  logIndex, "the totals");

        // Closing each stop's cell and one more: no order in it or beside it
        std::vector<std::uint64_t> toClose;
        for (Route::Stop const& stop : plan.stops)
            toClose.push_back(stop.order.cell);
        toClose.push_back(FieldCellKey(ask.mapId, gen.Int(CELL_LOW, CELL_HIGH), gen.Int(CELL_LOW, CELL_HIGH)));
        for (std::uint64_t const cell : toClose)
        {
            std::vector<std::uint64_t> closed = w.closed;
            closed.push_back(cell);
            std::sort(closed.begin(), closed.end());
            closed.erase(std::unique(closed.begin(), closed.end()), closed.end());
            Route::Ask closedAsk = ask;
            closedAsk.closed = closed;
            Route::Plan const other = Route::Build(w.map, w.index, w.needs, closedAsk);
            closing.Note(std::none_of(other.stops.begin(), other.stops.end(),
                                      [&](Route::Stop const& stop) { return NearClosed(closed, stop.order.cell); }),
                         logIndex, "an order at a closed cell");
        }

        // The candidates of the first stop, priced and thinned
        Route::Log const start{w.needs};
        std::vector<Route::Candidate> const priced = Route::Priced(w.map, start, ask, ask.x, ask.y);
        std::vector<Route::Candidate> const kept = Route::Thinned(priced);
        thinned += priced.size() - std::min(priced.size(), kept.size());
        bool thinOk = priced.empty() == kept.empty() && priced.empty() != inReach &&
                      (priced.empty() || priced.front().order.cell == kept.front().order.cell);
        for (std::size_t i = 0; i < priced.size(); ++i)
        {
            Route::Candidate const& one = priced[i];
            thinOk = thinOk && !NearClosed(ask.closed, one.order.cell) &&
                     (!i || priced[i - 1].value > one.value ||
                      (priced[i - 1].value == one.value && priced[i - 1].order.cell < one.order.cell));
            bool beaten = false;
            for (std::size_t j = 0; j < i; ++j)
                beaten = beaten || (NearCell(priced[j].order.cell, one.order.cell) && SameNeeds(priced[j], one));
            bool const stays = std::any_of(kept.begin(), kept.end(), [&one](Route::Candidate const& other)
                                           { return other.order.cell == one.order.cell; });
            thinOk = thinOk && stays != beaten;
        }
        thin.Note(thinOk && kept.size() <= priced.size(), logIndex, "the candidates");
    }
    std::printf(
        "seed=%u logs=%zu routes=%zu stops=%zu (field %zu, hand_in %zu) several=%zu projected=%zu "
        "thinned=%zu\n",
        seed, LOGS, routes, stops, fieldOrders, handInOrders, several, projected, thinned);
    std::printf("quest items turned the first order in %zu logs\n", turned);
    for (Invariant const* invariant : {&work, &risk, &shut, &same, &reach, &giver, &atHand, &longer, &closing, &cells,
                                       &rise, &bounds, &sums, &thin, &bags, &none})
        Report(*invariant);
    Check(several > 0 && projected > 0 && thinned > 0 && fieldOrders > 0 && handInOrders > 0,
          "the generated logs give routes of several stops, projected needs, thinned candidates and both words");
}

// The hand-built world: eight wolves (level 9-10) in cell (0, 0), eight boars in cell (7, 0), an NPC in cell (3, 0).
// Quest 90 (1000 XP, two wolves) is handed in at the NPC, who gives quest 91 (500 XP, six boars)
constexpr std::uint32_t WOLF = 320;
constexpr std::uint32_t BOAR = 321;
constexpr std::uint32_t CLERK = 700;

struct Fixed
{
    FieldMap map;
    QuestIndex index;
    std::vector<Need> needs;
    Route::Ask ask;
};

Fixed WolvesAndBoars(bool wolvesInLog)
{
    Fixed f;
    std::vector<FieldSpawn> spawns;
    AddSpawns(spawns, WOLF, 0, 0, 8, 9, 10, 300);
    AddSpawns(spawns, BOAR, 7, 0, 8, 9, 10, 300);
    spawns.push_back(MakeSpawn(FieldKind::Creature, CLERK, MAP_A, Mid(3), Mid(0), 10, 10, false, 300));
    f.map = FieldMap::Build(spawns);
    QuestRecord wolves = MakeQuest(90, 1000, {MakeObjective(ObjectiveKind::Kill, WOLF, 2, 0)});
    wolves.enders = {0};
    QuestRecord boars = MakeQuest(91, 500, {MakeObjective(ObjectiveKind::Kill, BOAR, 6, 0)});
    boars.givers = {0};
    f.index.quests = {wolves, boars};
    f.index.places.push_back(MakePlace(CLERK, MAP_A, 3, 0));
    std::vector<QuestSlot> log;
    if (wolvesInLog)
        log.push_back({90, 0, {}});
    f.needs = Needs(f.index, log, 0, Pricing{}, 1);
    if (std::optional<Need> take = TakeNeed(f.index, f.index.quests[1], 0, Pricing{}, MAP_A))
        f.needs.push_back(std::move(*take));
    f.ask.mapId = MAP_A;
    f.ask.x = Mid(0);
    f.ask.y = Mid(0);
    f.ask.level = 10;
    return f;
}

// The field, then the hand-in and the take at one NPC, planned with the log as it will be; the boars the take opens
// are worth less per minute than the route so far and end it
void FieldThenHandIn()
{
    Fixed const f = WolvesAndBoars(true);
    Route::Plan const plan = Route::Build(f.map, f.index, f.needs, f.ask);
    double const walk = 200.0 / 7.0 / 60.0;  // from the wolves' cell to cell (2, 0), the nearest field with the NPC
    Check(plan.stops.size() == 2, "wolves, then the NPC: two orders");
    if (plan.stops.size() != 2)
        return;
    QuestOrder const& first = plan.stops[0].order;
    QuestOrder const& second = plan.stops[1].order;
    Check(first.word == QuestOrderWord::Field && first.cell == FieldCellKey(MAP_A, 0, 0) &&
              first.expected.size() == 1 && first.expected[0] == QuestOrderNeed{90, 0, 0} &&
              first.expected[0].credits == 2 && Near(plan.stops[0].worth, 400.0) && first.walkMinutes == 0.0f &&
              Near(first.minutes, 5.0) && Near(first.worthPerMinute, 80.0) && first.end == QuestOrderEnd::Done &&
              Near(first.allowanceYards, 184.0),
          "the field: two wolves for 400 in 5 minutes, 80 a minute, allowance 184 yd");
    Check(second.word == QuestOrderWord::HandIn && second.cell == FieldCellKey(MAP_A, 2, 0) &&
              second.expected.size() == 2 && second.expected[0] == QuestOrderNeed{90, HAND_IN, 0} &&
              second.expected[1] == QuestOrderNeed{91, TAKE, 0} && Near(plan.stops[1].worth, 550.0) &&
              Near(second.walkMinutes, walk) && Near(second.minutes, 0.5) && Near(second.worthPerMinute, 1100.0),
          "the NPC: the hand-in 500 and the take 50 in two quarter minutes, 200 yd away");
    Check(Near(first.leaveBelow, 550.0 / (walk + 0.5)) && second.leaveBelow == 0.0f,
          "leaving the field pays below the NPC's worth per minute of walk and stay; the last order has none");
    Check(Near(plan.worth, 950.0) && Near(plan.minutes, 5.5 + walk) &&
              Near(Route::WorthPerMinute(plan), 950.0 / (5.5 + walk)),
          "the route: 950 in 5.5 minutes and the walk");

    Route::Log log{f.needs};
    Route::Advance(f.index, f.ask, log, first);
    std::optional<std::uint32_t> const handIn = OpenNeed(log.needs, 90, HAND_IN);
    Check(!OpenNeed(log.needs, 90, 0) && handIn && Near(log.needs[*handIn].worthPerCredit, 500.0),
          "after the field the log's wolves are done and the quest has its hand-in need");
    Route::Advance(f.index, f.ask, log, second);
    std::optional<std::uint32_t> const boars = OpenNeed(log.needs, 91, 0);
    Check(!OpenNeed(log.needs, 90, HAND_IN) && !OpenNeed(log.needs, 91, TAKE) && boars && log.needs[*boars].left == 6,
          "after the NPC the taken quest has its six boars");

    Route::Log const start{f.needs};
    std::vector<Route::Candidate> const priced = Route::Priced(f.map, start, f.ask, f.ask.x, f.ask.y);
    std::vector<Route::Candidate> const kept = Route::Thinned(priced);
    Check(priced.size() == 18 && kept.size() == 2 && kept[0].order.cell == FieldCellKey(MAP_A, 0, 0) &&
              kept[1].order.cell == FieldCellKey(MAP_A, 2, 0),
          "nine shifts of the wolves' area and nine of the NPC's are two candidates");
}

// Closed cells, and the bot without a quest
void ClosedAndEmpty()
{
    Fixed const f = WolvesAndBoars(true);
    std::vector<std::uint64_t> const closed = {FieldCellKey(MAP_A, 0, 0)};
    Route::Ask ask = f.ask;
    ask.closed = closed;
    Route::Plan const plan = Route::Build(f.map, f.index, f.needs, ask);
    Check(plan.stops.size() == 1 && plan.stops[0].order.word == QuestOrderWord::HandIn &&
              plan.stops[0].order.cell == FieldCellKey(MAP_A, 2, 0) && Expects(plan.stops[0].order, 91, TAKE) &&
              plan.stops[0].order.expected.size() == 1 && Near(plan.stops[0].order.minutes, 0.25) &&
              Near(plan.stops[0].worth, 50.0),
          "the wolves' cell closed: no order there or beside it, the take at the NPC for 50 in a quarter minute");

    Fixed const bare = WolvesAndBoars(false);
    Route::Plan const taking = Route::Build(bare.map, bare.index, bare.needs, bare.ask);
    Check(bare.needs.size() == 1 && taking.stops.size() == 1 && taking.stops[0].order.word == QuestOrderWord::HandIn &&
              Expects(taking.stops[0].order, 91, TAKE),
          "an empty log and a giver in reach: a route to the giver");
    Check(Route::Build(bare.map, bare.index, std::span<Need const>(), bare.ask).stops.empty(), "no needs: no route");
}

// Register row G344: the bot stands at the ender of a ready quest worth little, a rich field 300 yd away. The plan
// hands in here first, then goes to the field
void HandInHere()
{
    std::vector<FieldSpawn> spawns;
    AddSpawns(spawns, WOLF, 3, 0, 8, 9, 10, 300);
    spawns.push_back(MakeSpawn(FieldKind::Creature, CLERK, MAP_A, Mid(0), Mid(0), 10, 10, false, 300));
    FieldMap const map = FieldMap::Build(spawns);
    QuestRecord small = MakeQuest(92, 10, {MakeObjective(ObjectiveKind::Kill, WOLF, 1, 0)});
    small.enders = {0};
    QuestIndex index;
    index.quests = {small, MakeQuest(93, 2000, {MakeObjective(ObjectiveKind::Kill, WOLF, 10, 0)})};
    index.places.push_back(MakePlace(CLERK, MAP_A, 0, 0));
    std::vector<QuestSlot> const log = {{92, Circuit::SLOT_COMPLETE, {1, 0, 0, 0}}, {93, 0, {}}};
    std::vector<Need> const needs = Needs(index, log, 0, Pricing{}, 1);
    Route::Ask ask;
    ask.mapId = MAP_A;
    ask.x = Mid(0);
    ask.y = Mid(0);
    ask.level = 10;
    Route::Log const start{needs};
    std::vector<Route::Candidate> const priced = Route::Priced(map, start, ask, ask.x, ask.y);
    Check(!priced.empty() && priced.front().order.cell == FieldCellKey(MAP_A, 2, 0) &&
              priced.front().order.word == QuestOrderWord::Field,
          "by worth per minute alone the wolves' field comes before the small hand-in");
    Route::Plan const plan = Route::Build(map, index, needs, ask);
    Check(plan.stops.size() >= 2 && plan.stops[0].order.word == QuestOrderWord::HandIn &&
              plan.stops[0].order.cell == FieldCellKey(MAP_A, 0, 0) && Expects(plan.stops[0].order, 92, HAND_IN) &&
              Near(plan.stops[0].worth, 5.0) && Near(plan.stops[0].order.minutes, 0.25) &&
              plan.stops[1].order.word == QuestOrderWord::Field &&
              plan.stops[1].order.cell == FieldCellKey(MAP_A, 2, 0) && Expects(plan.stops[1].order, 93, 0),
          "the ready quest is handed in here by the first order, the field is the second");
}

// WQ17d: far stops. Generated worlds with a made-up walk function (straight yards times a detour; it refuses the cells
// of one residue) and a budget, often a smaller radius. The builder never calls it beyond the budget; a far stop never
// beats a near one worth more per minute; a need whose fields all lie far gets a route when nothing near is open; a
// refused field is a gap once and never a stop. No call allowed: the route of no walk function; the horizon-only rule
// begins with the default route
bool Residue(std::uint64_t cell, int residue)
{
    return (((FieldCellXOf(cell) * 7 + FieldCellYOf(cell) * 3) % 5) + 5) % 5 == residue;
}

void FarStops(std::uint32_t seed)
{
    Gen gen{std::mt19937(seed ^ FAR_SALT)};
    Invariant budget{"the builder never calls the walk function more often than its budget (over the whole route)"};
    Invariant nearFirst{
        "a far stop is never chosen while a near one is worth more per minute; it is priced by the walk"};
    Invariant farOnly{"a need whose only fields are far and reachable gives a route when nothing near is open"};
    Invariant refused{"a refused far field is in the route's gaps once and never a stop"};
    Invariant plain{"no walk call allowed: the route of no walk function; the same input, the same route and calls"};
    Invariant prefix{"the horizon-only route begins with the default route: the rule only ends it"};
    std::size_t farStops = 0;
    std::size_t gapCount = 0;
    std::size_t cappedRoutes = 0;
    std::size_t calls = 0;
    std::size_t farOnlyRoutes = 0;
    for (std::size_t logIndex = 0; logIndex < LOGS; ++logIndex)
    {
        World const w = Generate(gen);
        Route::Ask ask = AskOf(w);
        if (gen.Chance(0.5f))
            ask.radius = gen.Real(100.0f, 400.0f);
        float const detour = gen.Real(1.0f, 2.0f);
        int const residue = gen.Int(0, 4);
        ask.walkBudget = gen.Chance(0.5f) ? Route::WALK_BUDGET : static_cast<std::uint32_t>(gen.Int(0, 6));
        float const speed = ask.runSpeed > 0.0f ? ask.runSpeed : Circuit::DEFAULT_RUN_SPEED;
        std::uint32_t const mapId = ask.mapId;
        std::uint32_t called = 0;
        std::vector<std::uint64_t> refusedCells;
        ask.walk = [&called, &refusedCells, mapId, residue, detour, speed](float fromX, float fromY, float toX,
                                                                           float toY) -> std::optional<float>
        {
            ++called;
            std::uint64_t const cell = FieldCellKeyAt(mapId, toX, toY);
            if (Residue(cell, residue))
            {
                refusedCells.push_back(cell);
                return std::nullopt;
            }
            return std::hypot(toX - fromX, toY - fromY) * detour / speed / SECONDS_PER_MINUTE;
        };
        Route::Plan const plan = Route::Build(w.map, w.index, w.needs, ask);
        std::string const text = RouteText(plan);
        std::uint32_t const firstCalls = called;
        calls += called;
        cappedRoutes += plan.capped ? 1 : 0;
        gapCount += plan.gaps.size();
        budget.Note(called <= ask.walkBudget && called == plan.walkCalls, logIndex, std::to_string(called) + " calls");

        std::vector<std::uint64_t> gapCells;
        bool named = true;
        for (Route::FarGap const& gap : plan.gaps)
        {
            gapCells.push_back(gap.cell);
            named = named && !gap.needs.empty();
        }
        std::vector<std::uint64_t> sortedRefused = refusedCells;
        std::sort(sortedRefused.begin(), sortedRefused.end());
        std::sort(gapCells.begin(), gapCells.end());
        bool const once = std::adjacent_find(gapCells.begin(), gapCells.end()) == gapCells.end();
        bool const noStop =
            std::none_of(plan.stops.begin(), plan.stops.end(), [&refusedCells](Route::Stop const& stop)
                         { return std::count(refusedCells.begin(), refusedCells.end(), stop.order.cell) > 0; });
        refused.Note(named && once && noStop && gapCells == sortedRefused, logIndex, "the gaps");

        // Each far stop against the near candidates from the point before, with the log as the plan expects it
        Route::Ask nearAsk = ask;
        nearAsk.walk = nullptr;
        Route::Log log{w.needs};
        float x = ask.x;
        float y = ask.y;
        std::vector<std::uint64_t> used;
        for (std::size_t k = 0; k < plan.stops.size(); ++k)
        {
            Route::Stop const& stop = plan.stops[k];
            QuestOrder const& order = stop.order;
            float const yards = std::hypot(order.x - x, order.y - y);
            if (yards > ask.radius)
            {
                ++farStops;
                float const value = Route::StopValue(stop.worth, order.walkMinutes, order.minutes);
                bool ok = Near(order.walkMinutes, yards * detour / speed / SECONDS_PER_MINUTE);
                for (Route::Candidate const& near : Route::Priced(w.map, log, nearAsk, x, y, used))
                    if (!std::count(refusedCells.begin(), refusedCells.end(), near.order.cell))
                        ok = ok && AtLeast(value, near.value);
                nearFirst.Note(ok, logIndex, "stop " + std::to_string(k));
            }
            Route::Advance(w.index, ask, log, order);
            used.push_back(order.cell);
            x = order.x;
            y = order.y;
        }

        // Nothing near open from the start and a field far: a walk that refuses nothing reaches it
        Route::Log const start{w.needs};
        if (Route::Priced(w.map, start, nearAsk, ask.x, ask.y).empty())
        {
            FieldAsk all = FieldAskOf(ask, ask.x, ask.y);
            all.radius = Route::FAR_RADIUS;
            Ranking const ranking = TopFields(w.map, w.needs, all);
            bool const farWork =
                std::any_of(ranking.fields.begin(), ranking.fields.end(), [&](Ranked const& field)
                            { return !NearClosed(ask.closed, field.offer.centre) && Works(field, w.needs); });
            if (farWork)
            {
                Route::Ask open = nearAsk;
                open.walkBudget = Route::WALK_BUDGET;
                open.walk = [speed](float fromX, float fromY, float toX, float toY) -> std::optional<float>
                { return std::hypot(toX - fromX, toY - fromY) / speed / SECONDS_PER_MINUTE; };
                Route::Plan const reached = Route::Build(w.map, w.index, w.needs, open);
                bool const went =
                    !reached.stops.empty() && reached.gaps.empty() &&
                    std::hypot(reached.stops[0].order.x - ask.x, reached.stops[0].order.y - ask.y) > ask.radius;
                farOnlyRoutes += went ? 1 : 0;
                farOnly.Note(went, logIndex, "no far route");
            }
        }

        // No call allowed, the same input again, and the horizon-only rule
        Route::Ask none = ask;
        none.walkBudget = 0;
        Route::Plan const zero = Route::Build(w.map, w.index, w.needs, none);
        called = 0;
        refusedCells.clear();
        Route::Plan const again = Route::Build(w.map, w.index, w.needs, ask);
        plain.Note(RouteText(zero) == RouteText(Route::Build(w.map, w.index, w.needs, nearAsk)) &&
                       zero.walkCalls == 0 && RouteText(again) == text && again.walkCalls == plan.walkCalls &&
                       called == firstCalls && again.gaps.size() == plan.gaps.size(),
                   logIndex, "another route");
        Route::Ask horizon = ask;
        horizon.stopRule = Route::StopRule::HorizonOnly;
        Route::Plan const longer = Route::Build(w.map, w.index, w.needs, horizon);
        bool begins = longer.stops.size() >= plan.stops.size();
        for (std::size_t k = 0; begins && k < plan.stops.size(); ++k)
            begins = longer.stops[k].order.cell == plan.stops[k].order.cell &&
                     longer.stops[k].order.expected == plan.stops[k].order.expected;
        prefix.Note(begins, logIndex, "another beginning");
    }
    std::printf("far: seed=%u logs=%zu far stops=%zu gaps=%zu capped routes=%zu calls=%zu far-only routes=%zu\n", seed,
                LOGS, farStops, gapCount, cappedRoutes, calls, farOnlyRoutes);
    for (Invariant const* invariant : {&budget, &nearFirst, &farOnly, &refused, &plain, &prefix})
        Report(*invariant);
    Check(farStops > 0 && gapCount > 0 && cappedRoutes > 0 && farOnlyRoutes > 0,
          "the far worlds give far stops, gaps, a spent budget and routes that reach only far");
}

// Two wolves to kill, the wolves' cell 900 yd from the bot: nothing near. The walk is twice the straight yards
void FarField()
{
    std::vector<FieldSpawn> spawns;
    AddSpawns(spawns, WOLF, 0, 0, 8, 9, 10, 300);
    FieldMap const map = FieldMap::Build(spawns);
    QuestIndex index;
    index.quests = {MakeQuest(90, 1000, {MakeObjective(ObjectiveKind::Kill, WOLF, 2, 0)})};
    std::vector<QuestSlot> const log = {{90, 0, {}}};
    std::vector<Need> const needs = Needs(index, log, 0, Pricing{}, 1);
    Route::Ask ask;
    ask.mapId = MAP_A;
    ask.x = Mid(9);
    ask.y = Mid(0);
    ask.level = 10;
    Check(Route::Build(map, index, needs, ask).stops.empty(), "no walk function: a field 900 yd away is no stop");

    std::uint64_t const nearest = FieldCellKey(MAP_A, 1, 0);
    std::uint32_t called = 0;
    std::vector<std::uint64_t> refuse;
    ask.walk = [&called, &refuse](float fromX, float fromY, float toX, float toY) -> std::optional<float>
    {
        ++called;
        if (std::count(refuse.begin(), refuse.end(), FieldCellKeyAt(MAP_A, toX, toY)))
            return std::nullopt;
        return 2.0f * std::hypot(toX - fromX, toY - fromY) / Circuit::DEFAULT_RUN_SPEED / SECONDS_PER_MINUTE;
    };
    Route::Plan const plan = Route::Build(map, index, needs, ask);
    Check(plan.stops.size() == 1 && plan.stops[0].order.cell == nearest && plan.walkCalls == 1 && called == 1 &&
              Near(plan.stops[0].order.walkMinutes, 1600.0 / 7.0 / 60.0) && Near(plan.stops[0].worth, 400.0) &&
              plan.gaps.empty(),
          "the far field: the nearest of the wolves' nine centres, 800 yd, walked as 1600 yd, one call");

    refuse = {nearest};
    called = 0;
    Route::Plan const around = Route::Build(map, index, needs, ask);
    Check(around.stops.size() == 1 && FieldCellXOf(around.stops[0].order.cell) == 1 &&
              around.stops[0].order.cell != nearest && around.walkCalls == 2 && around.gaps.size() == 1 &&
              around.gaps[0].cell == nearest && around.gaps[0].needs.size() == 1 &&
              around.gaps[0].needs[0] == QuestOrderNeed{90, 0, 0},
          "the nearest centre refused: a gap naming the wolves, the next nearest centre is the stop");

    for (std::int32_t cx = -1; cx <= 1; ++cx)
        for (std::int32_t cy = -1; cy <= 1; ++cy)
            refuse.push_back(FieldCellKey(MAP_A, cx, cy));
    Route::Plan const nowhere = Route::Build(map, index, needs, ask);
    Check(nowhere.stops.empty() && nowhere.gaps.size() == 9 && nowhere.walkCalls == 9,
          "every centre refused: no route, nine gaps, nine calls");

    ask.walkBudget = 0;
    Route::Plan const spent = Route::Build(map, index, needs, ask);
    Check(spent.stops.empty() && spent.walkCalls == 0 && spent.capped == 1,
          "no call allowed: no route, the need capped");

    // WQ18b part 3: the far reach (Route::FAR_RADIUS). The same wolves from 2900 yd are a far stop; from 3900 yd
    // they are beyond the reach: no stop, and the walk function is not asked
    ask.walkBudget = Route::WALK_BUDGET;
    refuse.clear();
    called = 0;
    ask.x = Mid(30);
    Check(Route::Build(map, index, needs, ask).stops.size() == 1 && called > 0,
          "a field 2900 yd away is within the far reach: a stop");
    called = 0;
    ask.x = Mid(40);
    Route::Plan const beyond = Route::Build(map, index, needs, ask);
    Check(beyond.stops.empty() && beyond.walkCalls == 0 && called == 0,
          "a field 3900 yd away is beyond the far reach: no stop, no walk asked");
}
}  // namespace

// Questing WQ18a: the goals' route (GoalRoute.h's build over the generated worlds) and its option
constexpr std::uint32_t GOAL_SALT = 0x18A0C0DEu;  // the goal worlds' seed: the given seed with these bits flipped

Snapshot BotOf(World const& w)
{
    Snapshot bot;
    bot.bot = 1;
    bot.level = w.ask.level;
    bot.team = w.ask.team;
    bot.race = w.ask.team ? ORC : HUMAN;
    bot.cls = 1;
    bot.position = {w.ask.mapId, w.ask.x, w.ask.y, 12.0f};
    bot.quests = w.log;
    bot.groupSize = w.ask.groupSize;
    bot.runSpeed = 7.0f;
    return bot;
}

bool IsRouteOption(Goals::Option const& option)
{
    return option.kind == Goals::GoalKind::Quest && option.targetType == Goals::TargetType::Route;
}

std::string GoalText(Goals::RoutePlan const& route, Goals::Ranked const& ranked)
{
    std::string text;
    for (QuestOrder const& order : route.orders)
    {
        text += std::to_string(order.cell) + ":" + QuestOrderWordName(order.word) + ":" +
                std::to_string(order.minutes) + ":" + std::to_string(order.walkMinutes);
        for (QuestOrderNeed const& need : order.expected)
            text += "," + std::to_string(need.quest) + "/" + std::to_string(need.objective) + "/" +
                    std::to_string(need.credits);
        text += ";";
    }
    text += std::to_string(route.worth) + "|" + std::to_string(route.minutes) + "|";
    for (Goals::Option const& option : ranked.options)
        text += Goals::ChoiceWord(option) + "=" + option.reason + "=" + std::to_string(option.valuePerMinute) + ";";
    return text;
}

// The build as Goals::PlanGoals calls it (the model and the weights it hands on) and the options Goals::Rank makes of
// the result. Not through PlanGoals: a generated world's index is filled by hand, without the lookups PlanCircuit
// reads; GoalRouteTakes plans over a built one
void GoalRoutes(std::uint32_t seed)
{
    Gen gen{std::mt19937(seed ^ GOAL_SALT)};
    Invariant carried{"the goals' route is the one Route::Build makes of the bot's needs"};
    Invariant twice{"the same bot and situation give the same route and the same options"};
    Invariant shut{
        "no order in a cell the situation closes or beside one; the map thread's test agrees with the route's"};
    Invariant held{"a bot held on the circuit is offered no route: its options are those of a situation without one"};
    Invariant owner{"a route is offered as the route's option when it has an order, and only then"};
    Invariant searches{"far stops are priced by the job's search at the bot's height; a capped place is no gap"};
    std::size_t routes = 0;
    std::size_t farRoutes = 0;
    for (std::size_t logIndex = 0; logIndex < LOGS / 2; ++logIndex)
    {
        World const w = Generate(gen);
        Snapshot const bot = BotOf(w);
        bool const withSearch = gen.Chance(0.4f);
        int const noWay = gen.Int(0, 4);
        bool const caps = gen.Chance(0.3f);
        float const speed = 7.0f;
        std::uint32_t asked = 0;
        bool flat = true;
        // The job's search: straight yards with a detour; no way to the cells of one class, past the cap for another
        auto const seconds = [&](float fromX, float fromY, float toX, float toY) -> float
        {
            std::uint64_t const cell = FieldCellKeyAt(bot.position.map, toX, toY);
            int const cls = ((FieldCellXOf(cell) * 3 + FieldCellYOf(cell)) % 5 + 5) % 5;
            if (cls == noWay)
                return TRAVEL_NO_ROUTE;
            if (caps && cls == (noWay + 1) % 5)
                return TRAVEL_CAPPED;
            return 1.3f * std::hypot(toX - fromX, toY - fromY) / speed;
        };
        Goals::Situation situation;
        situation.onCircuit = false;
        situation.closedCells = w.closed;
        Goals::Model const model = Goals::Prepare(bot, situation);
        CircuitContext context;
        context.quests = &w.index;
        context.weights = Goals::CircuitWeightsFor(model);
        if (withSearch)
            context.travel = [&](Place const& from, Place const& to)
            {
                ++asked;
                flat = flat && from.map == bot.position.map && to.map == bot.position.map && from.z == bot.position.z &&
                       to.z == bot.position.z;
                return seconds(from.x, from.y, to.x, to.y);
            };
        Goals::RoutePlan const route =
            GoalRoute::Build(w.map, TakeFixture::Answered(bot, context), context, model, situation);
        std::uint32_t const askedByRoute = asked;
        situation.route = &route;
        Goals::Ranked const ranked = Goals::Rank(bot, situation);
        std::string const where = "log " + std::to_string(logIndex);

        // The same route from the needs as this test states them
        Pricing const pricing{context.weights, {}};
        std::vector<Need> needs = Needs(w.index, w.log, bot.team, pricing, bot.groupSize);
        std::vector<std::uint32_t> inLog;
        for (QuestSlot const& slot : w.log)
            inLog.push_back(slot.quest);
        std::sort(inLog.begin(), inLog.end());
        Route::Ask ask;
        ask.mapId = bot.position.map;
        ask.x = bot.position.x;
        ask.y = bot.position.y;
        ask.level = bot.level;
        ask.team = bot.team;
        ask.runSpeed = model.runSpeed;
        ask.caution = model.cautionWeight;
        ask.groupSize = bot.groupSize;
        ask.closed = w.closed;
        ask.pricing = pricing;
        std::uint32_t directCapped = 0;
        if (withSearch)
            ask.walk = [&](float fromX, float fromY, float toX, float toY) -> std::optional<float>
            {
                float const answer = seconds(fromX, fromY, toX, toY);
                directCapped += answer == TRAVEL_CAPPED ? 1 : 0;
                if (answer < 0.0f)
                    return std::nullopt;
                return answer / SECONDS_PER_MINUTE;
            };
        Route::Plan const direct = Route::Build(w.map, w.index, needs, ask);
        bool same = route.orders.size() == direct.stops.size() && Near(route.worth, direct.worth) &&
                    Near(route.minutes, direct.minutes);
        for (std::size_t k = 0; same && k < direct.stops.size(); ++k)
        {
            QuestOrder const& mine = route.orders[k];
            QuestOrder const& theirs = direct.stops[k].order;
            same = SameOrder(mine, theirs) && mine.expected == theirs.expected && mine.minutes == theirs.minutes &&
                   mine.walkMinutes == theirs.walkMinutes;
        }
        carried.Note(same, logIndex, where);
        routes += route.orders.empty() ? 0 : 1;
        if (withSearch)
        {
            searches.Note(flat && route.walkCalls == direct.walkCalls && route.walkCalls == askedByRoute &&
                              route.walkCalls <= Route::WALK_BUDGET &&
                              route.gaps + directCapped == direct.gaps.size() &&
                              route.capped == direct.capped + directCapped,
                          logIndex, where);
            farRoutes += route.walkCalls ? 1 : 0;
        }

        Goals::RoutePlan const again =
            GoalRoute::Build(w.map, TakeFixture::Answered(bot, context), context, model, situation);
        Goals::Situation second = situation;
        second.route = &again;
        twice.Note(GoalText(again, Goals::Rank(bot, second)) == GoalText(route, ranked), logIndex, where);

        for (QuestOrder const& order : route.orders)
            shut.Note(!NearClosed(w.closed, order.cell) && !GoalApply::CellClosed(w.closed, order.cell), logIndex,
                      where);
        for (int k = 0; k < 8; ++k)
        {
            std::uint32_t const cellMap = gen.Chance(0.9f) ? bot.position.map : bot.position.map + 1;
            std::int32_t const cellX = gen.Int(CELL_LOW - 2, CELL_HIGH + 2);
            std::int32_t const cellY = gen.Int(CELL_LOW - 2, CELL_HIGH + 2);
            std::uint64_t const cell = FieldCellKey(cellMap, cellX, cellY);
            shut.Note(GoalApply::CellClosed(w.closed, cell) == NearClosed(w.closed, cell) &&
                          GoalApply::CellClosed(w.closed, cell) == Route::Detail::NearClosed(w.closed, cell),
                      logIndex, where);
        }

        bool const offered = std::any_of(ranked.options.begin(), ranked.options.end(), IsRouteOption);
        owner.Note(ranked.ok && offered == !route.orders.empty(), logIndex, where);

        Goals::Situation onCircuit = situation;
        onCircuit.onCircuit = true;
        Goals::Situation without = onCircuit;
        without.route = nullptr;
        Goals::Ranked const heldRanked = Goals::Rank(bot, onCircuit);
        held.Note(GoalText({}, heldRanked) == GoalText({}, Goals::Rank(bot, without)) &&
                      std::none_of(heldRanked.options.begin(), heldRanked.options.end(), IsRouteOption),
                  logIndex, where);
    }
    std::printf("goal routes: %zu of %zu worlds with a route, %zu with far searches\n", routes, LOGS / 2, farRoutes);
    Check(routes > LOGS / 8 && farRoutes > 0, "the goal routes: too few worlds with a route or with far searches");
    Report(carried);
    Report(twice);
    Report(shut);
    Report(held);
    Report(owner);
    Report(searches);
}

// Questing WQ18a: a built index (its giver grid gives the take candidates): the field, then the hand-in and the take
// at one NPC, as FieldThenHandIn plans them from needs made by hand
void GoalRouteTakes()
{
    std::vector<FieldSpawn> spawns;
    AddSpawns(spawns, WOLF, 0, 0, 8, 9, 10, 300);
    AddSpawns(spawns, BOAR, 7, 0, 8, 9, 10, 300);
    spawns.push_back(MakeSpawn(FieldKind::Creature, CLERK, MAP_A, Mid(3), Mid(0), 10, 10, false, 300));
    auto const map = std::make_shared<FieldMap const>(FieldMap::Build(spawns));
    QuestGather gather;
    gather.quests = {MakeQuest(90, 1000, {MakeObjective(ObjectiveKind::Kill, WOLF, 2, 0)}),
                     MakeQuest(91, 500, {MakeObjective(ObjectiveKind::Kill, BOAR, 6, 0)})};
    gather.places = {MakePlace(CLERK, MAP_A, 3, 0)};
    gather.relations = {{90, CLERK, false, true}, {91, CLERK, false, false}};
    QuestIndex const index = QuestIndex::Build(gather);
    Snapshot bot;
    bot.bot = 1;
    bot.level = 10;
    bot.race = HUMAN;
    bot.cls = 1;
    bot.position = {MAP_A, Mid(0), Mid(0), 0.0f};
    bot.quests = {{90, 0, {}}};
    bot.runSpeed = 7.0f;
    CircuitContext context;
    context.quests = &index;
    Goals::Situation situation;
    situation.onCircuit = false;
    Goals::RouteBuild const build = GoalRoute::Builder(map);
    auto const takes = [](Goals::GoalPlan const& plan)
    {
        std::size_t count = 0;
        for (QuestOrder const& order : plan.route.orders)
            count += Expects(order, 91, TAKE) ? 1 : 0;
        return count;
    };
    Goals::GoalPlan const plan = Goals::PlanGoals(TakeFixture::Answered(bot, context), context, situation, build);
    std::vector<QuestOrder> const& orders = plan.route.orders;
    Check(orders.size() >= 2 && orders[0].word == QuestOrderWord::Field && Expects(orders[0], 90, 0) &&
              orders[1].word == QuestOrderWord::HandIn && Expects(orders[1], 90, HAND_IN) && takes(plan) == 1 &&
              std::any_of(plan.ranked.options.begin(), plan.ranked.options.end(), IsRouteOption),
          "goal route: the field, then the hand-in and the take at their NPC, offered as the route's option: " +
              GoalText(plan.route, plan.ranked));
    // The first stop's risk, for a bot half a level below the wolves' middle: a field without risk would prove nothing
    Snapshot low = bot;
    low.level = 9;
    Goals::GoalPlan const lowPlan = Goals::PlanGoals(TakeFixture::Answered(low, context), context, situation, build);
    bool const wolvesFirst = !lowPlan.route.orders.empty() && Expects(lowPlan.route.orders[0], 90, 0);
    FieldAsk fieldAsk;
    fieldAsk.mapId = MAP_A;
    fieldAsk.x = low.position.x;
    fieldAsk.y = low.position.y;
    fieldAsk.level = low.level;
    fieldAsk.stayMinutes = Route::STAY_MINUTES;
    fieldAsk.runSpeed = Goals::Prepare(low, situation).runSpeed;
    Offer const offer = wolvesFirst ? FieldOffer(*map, Needs(index, low.quests, 0, Pricing{}, 1),
                                                 lowPlan.route.orders[0].cell, fieldAsk)
                                    : Offer{};
    Goals::RiskInput const& risk = lowPlan.route.risk;
    Check(wolvesFirst && offer.risk.levelsAbove > 0.0f && risk.levelsAbove == offer.risk.levelsAbove &&
              risk.eliteShare == offer.risk.eliteShare && risk.enemyShare == offer.risk.enemyShare &&
              risk.deaths == offer.risk.deaths,
          "goal route: the plan carries the risk of its first stop's field (levels above " +
              std::to_string(risk.levelsAbove) + ", the field's " + std::to_string(offer.risk.levelsAbove) +
              "): " + GoalText(lowPlan.route, lowPlan.ranked));
    Goals::Situation gaveUp = situation;
    gaveUp.givenUpQuests = {91};
    Snapshot rewarded = bot;
    rewarded.rewardedQuests = {91};
    Check(takes(Goals::PlanGoals(TakeFixture::Answered(bot, context), context, gaveUp, build)) == 0 &&
              takes(Goals::PlanGoals(TakeFixture::Answered(rewarded, context), context, situation, build)) == 0,
          "goal route: no take of a quest the bot gave up or was rewarded for");
    // No rule for room (questing WQ40, WQ.46 item 1): a log of 25 or of 60 quests takes as the bot's own
    std::size_t const own = takes(Goals::PlanGoals(TakeFixture::Answered(bot, context), context, situation, build));
    for (std::size_t const held : {std::size_t{25}, std::size_t{60}})
    {
        Snapshot crowded = bot;
        crowded.noLogLimit = true;
        for (std::uint32_t id = 200; crowded.quests.size() < held; ++id)
            crowded.quests.push_back({id, 0, {}});
        Check(own > 0 &&
                  takes(Goals::PlanGoals(TakeFixture::Answered(crowded, context), context, situation, build)) == own,
              "goal route: a log of " + std::to_string(held) + " quests takes as the bot's own (" +
                  std::to_string(own) + ")");
    }
    Check(!GoalRoute::Builder(nullptr) &&
              Goals::PlanGoals(TakeFixture::Answered(bot, context), context, situation, GoalRoute::Builder(nullptr))
                  .route.orders.empty(),
          "goal route: no field map, no build and no route");
}

// Questing WQ44, hand-built. The bot stands in cell (0, 0); every field named lies two cells (200 yd) to either side,
// so the walks are equal and the worths decide
constexpr std::uint32_t ENDER = 701;
constexpr std::uint32_t GHOST = 399;  // spawns nowhere

Route::Plan PlanWithItems(FieldMap const& map, QuestIndex const& index, std::vector<QuestSlot> const& log, float share,
                          std::optional<std::uint32_t> take = std::nullopt)
{
    Route::Ask ask;
    ask.mapId = MAP_A;
    ask.x = Mid(0);
    ask.y = Mid(0);
    ask.level = 10;
    ask.pricing.questItemShare = share;
    std::vector<Need> needs = Needs(index, log, 0, ask.pricing, 1);
    if (take)
        if (std::optional<Need> need = TakeNeed(index, *index.Find(*take), 0, ask.pricing, MAP_A))
            needs.push_back(std::move(*need));
    return Route::Build(map, index, needs, ask);
}

// Quest 90 (100 XP) is ready and its ender stands to one side: 50 at the hand-in. Quest 91 (800 XP) is given on the
// other: 80 at the take. Full of quest items the hand-in is worth 100
void ItemsPreferHandIn()
{
    std::vector<FieldSpawn> spawns;
    spawns.push_back(MakeSpawn(FieldKind::Creature, ENDER, MAP_A, Mid(3), Mid(0), 10, 10, false, 300));
    spawns.push_back(MakeSpawn(FieldKind::Creature, CLERK, MAP_A, Mid(-3), Mid(0), 10, 10, false, 300));
    auto const map = std::make_shared<FieldMap const>(FieldMap::Build(spawns));
    QuestGather gather;
    gather.quests = {MakeQuest(90, 100, {MakeObjective(ObjectiveKind::Kill, WOLF, 2, 0)}),
                     MakeQuest(91, 800, {MakeObjective(ObjectiveKind::Kill, BOAR, 6, 0)})};
    gather.places = {MakePlace(ENDER, MAP_A, 3, 0), MakePlace(CLERK, MAP_A, -3, 0)};
    gather.relations = {{90, ENDER, false, true}, {91, CLERK, false, false}};
    QuestIndex const index = QuestIndex::Build(gather);
    std::vector<QuestSlot> const log = {{90, Circuit::SLOT_COMPLETE, {2, 0, 0, 0}}};

    Route::Plan const empty = PlanWithItems(*map, index, log, 0.0f, 91);
    Route::Plan const full = PlanWithItems(*map, index, log, 1.0f, 91);
    Check(!empty.stops.empty() && Expects(empty.stops[0].order, 91, TAKE) && Near(empty.stops[0].worth, 80.0),
          "no quest item: the take's 80 before the hand-in's 50: " + RouteText(empty));
    Check(!full.stops.empty() && Expects(full.stops[0].order, 90, HAND_IN) && Near(full.stops[0].worth, 100.0),
          "bags full of quest items: the hand-in, worth 100, before the take: " + RouteText(full));

    // The snapshot's slots reach the goals' route
    Snapshot bot;
    bot.bot = 1;
    bot.level = 10;
    bot.race = HUMAN;
    bot.cls = 1;
    bot.position = {MAP_A, Mid(0), Mid(0), 0.0f};
    bot.quests = log;
    bot.runSpeed = 7.0f;
    bot.bagSlots = 16;
    CircuitContext context;
    context.quests = &index;
    Goals::Situation situation;
    situation.onCircuit = false;
    Goals::RouteBuild const build = GoalRoute::Builder(map);
    Snapshot laden = bot;
    laden.questItemSlots = 16;
    Goals::GoalPlan const light = Goals::PlanGoals(TakeFixture::Answered(bot, context), context, situation, build);
    Goals::GoalPlan const heavy = Goals::PlanGoals(TakeFixture::Answered(laden, context), context, situation, build);
    Check(!light.route.orders.empty() && Expects(light.route.orders[0], 91, TAKE) && !heavy.route.orders.empty() &&
              Expects(heavy.route.orders[0], 90, HAND_IN),
          "goal route: the share of bag slots with quest items turns the first order: " +
              GoalText(light.route, light.ranked) + " / " + GoalText(heavy.route, heavy.ranked));
}

// Quest 94 (500 XP) needs two wolves and nothing else: 100 a wolf. Quest 95 (1600 XP) needs two boars and two of a
// creature that spawns nowhere: 160 a boar. Full of quest items a wolf is worth 200
void ItemsPreferFinishing()
{
    std::vector<FieldSpawn> spawns;
    AddSpawns(spawns, WOLF, 3, 0, 8, 9, 10, 300);
    AddSpawns(spawns, BOAR, -3, 0, 8, 9, 10, 300);
    FieldMap const map = FieldMap::Build(spawns);
    QuestIndex index;
    index.quests = {
        MakeQuest(94, 500, {MakeObjective(ObjectiveKind::Kill, WOLF, 2, 0)}),
        MakeQuest(95, 1600,
                  {MakeObjective(ObjectiveKind::Kill, BOAR, 2, 0), MakeObjective(ObjectiveKind::Kill, GHOST, 2, 1)})};
    std::vector<QuestSlot> const log = {{94, 0, {}}, {95, 0, {}}};
    Route::Plan const empty = PlanWithItems(map, index, log, 0.0f);
    Route::Plan const full = PlanWithItems(map, index, log, 1.0f);
    Check(!empty.stops.empty() && Expects(empty.stops[0].order, 95, 0),
          "no quest item: the boars of the quest with two open objectives first: " + RouteText(empty));
    Check(!full.stops.empty() && Expects(full.stops[0].order, 94, 0),
          "bags full of quest items: the wolves that finish their quest first: " + RouteText(full));
}

int main(int argc, char** argv)
{
    std::uint32_t seed = DEFAULT_SEED;
    std::size_t show = NO_LOG;
    if (argc > 1)
        seed = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    if (argc > 2)
        show = static_cast<std::size_t>(std::strtoul(argv[2], nullptr, 10));
    FieldThenHandIn();
    ClosedAndEmpty();
    HandInHere();
    GeneratedLogs(seed, show);
    FarField();
    FarStops(seed);
    GoalRoutes(seed);
    GoalRouteTakes();
    ItemsPreferHandIn();
    ItemsPreferFinishing();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
