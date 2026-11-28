/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the quest log's needs and a field's worth (questing WQ14, QuestFields.h): generated logs over
// made-up maps (the seed is printed; argv[1] replays it) checked against the step's invariants,
// plus hand-built cases whose sums are worked by hand. Weights are the circuit's neutral ones (CircuitWeights{}),
// the caution weight 1.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel tests/quest-fields-selftest.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "QuestFields.h"
#include "QuestTakeFixture.h"

using namespace Planner;
using namespace Planner::QuestFields;

namespace
{
constexpr std::uint32_t DEFAULT_SEED = 20261124;
constexpr std::size_t LOGS = 600;
constexpr std::uint32_t ALONE = 1;  // WQ38: the group size Needs is asked with: a lone bot, a party
constexpr std::uint32_t PARTY = 5;
constexpr std::uint32_t MAP_A = 0;
constexpr std::uint32_t MAP_B = 1;
constexpr std::int32_t CELL_LOW = -6;  // the made-up maps span cells -6 to 5: a cell border and negative coordinates
constexpr std::int32_t CELL_HIGH = 5;
constexpr std::uint32_t FIRST_CREATURE = 100;
constexpr std::uint32_t CREATURES = 32;
constexpr std::uint32_t FIRST_UNSPAWNED = 900;  // creatures with no spawn: data gaps
constexpr std::uint32_t UNSPAWNED = 6;
constexpr std::uint32_t FIRST_OBJECT = 500;
constexpr std::uint32_t OBJECTS = 12;
constexpr std::uint32_t FIRST_ENDER = 700;  // friendly creatures: enders and talkers
constexpr std::uint32_t ENDERS = 8;
constexpr std::uint32_t FIRST_ITEM = 2000;
constexpr std::uint32_t ITEMS = 12;
constexpr std::uint32_t FIRST_QUEST = 10;
constexpr std::uint32_t USE_ITEM = 3000;
constexpr std::uint32_t EVENT_SPELL = 5000;
constexpr std::uint32_t UNKNOWN_QUEST = 99999;
constexpr float FAR_RADIUS = 1.0e6f;
constexpr std::uint32_t FIRST_ORDINARY = 1200;  // WQ17b: ordinary hostiles added to a field
constexpr std::uint8_t HUMAN = 1;               // a race of each side
constexpr std::uint8_t ORC = 2;
constexpr float SLACK = 1.0e-4f;  // relative slack of float comparisons

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
    std::uint8_t team{0};
    std::uint32_t groupSize{ALONE};  // WQ38
    FieldAsk ask;
};

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

// Cell-centred coordinates for hand-built cases
float Mid(std::int32_t cell) { return (static_cast<float>(cell) + 0.5f) * FIELD_CELL_YARDS; }

LootRow MakeLoot(std::uint32_t lootId, std::uint32_t item, float percent)
{
    LootRow row;
    row.lootId = lootId;
    row.item = item;
    row.chance = percent;
    return row;
}

void AddSource(SourceGather& gather, SourceKind kind, std::uint32_t owner, std::uint32_t item, float percent,
               bool spawned)
{
    gather.tables[static_cast<std::size_t>(kind)].push_back(MakeLoot(owner, item, percent));
    gather.owners.push_back({kind, owner, owner});
    if (spawned)
    {
        SourceSpawn spawn;
        spawn.entry = owner;
        spawn.isObject = kind == SourceKind::Object;
        gather.spawns.push_back(spawn);
    }
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

World Generate(Gen& gen)
{
    World w;
    w.team = static_cast<std::uint8_t>(gen.Int(0, 1));
    w.groupSize = gen.Chance(0.7f) ? ALONE : static_cast<std::uint32_t>(gen.Int(2, 5));
    std::uint8_t const level = static_cast<std::uint8_t>(gen.Int(1, 60));
    // Homes: an entry's spawns lie in and around one cell; creatures within a few levels of the bot, some far above
    std::vector<Home> creatureHomes(CREATURES);
    std::vector<Home> objectHomes(OBJECTS);
    std::vector<Home> enderHomes(ENDERS);
    auto const home = [&gen, level](Home& h)
    {
        h.mapId = gen.Chance(0.8f) ? MAP_A : MAP_B;
        h.cellX = gen.Int(CELL_LOW, CELL_HIGH);
        h.cellY = gen.Int(CELL_LOW, CELL_HIGH);
        h.level = static_cast<std::uint8_t>(gen.Int(std::max(1, level - 8), level + 10));
    };
    for (Home& h : creatureHomes)
        home(h);
    for (Home& h : objectHomes)
        home(h);
    for (Home& h : enderHomes)
        home(h);

    int const spawnCount = gen.Int(20, 160);
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
            hostile = gen.Chance(0.7f);
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
            std::uint32_t const at = static_cast<std::uint32_t>(gen.Int(0, ENDERS - 1));
            entry = FIRST_ENDER + at;
            h = &enderHomes[at];
        }
        float const x = (static_cast<float>(h->cellX + gen.Int(-1, 1)) + gen.Real(0.01f, 0.99f)) * FIELD_CELL_YARDS;
        float const y = (static_cast<float>(h->cellY + gen.Int(-1, 1)) + gen.Real(0.01f, 0.99f)) * FIELD_CELL_YARDS;
        std::uint8_t const minLevel = kind == FieldKind::Object ? 0 : h->level;
        std::uint8_t const maxLevel =
            kind == FieldKind::Object ? 0 : static_cast<std::uint8_t>(minLevel + gen.Int(0, 3));
        FieldSpawn spawn = MakeSpawn(kind, entry, h->mapId, x, y, minLevel, maxLevel, hostile,
                                     gen.Chance(0.1f) ? 0 : static_cast<std::uint32_t>(gen.Int(30, 900)));
        spawn.elite = kind == FieldKind::Creature && gen.Chance(0.1f);
        spawn.share = gen.Chance(0.8f) ? 1.0f : 0.5f;
        w.spawns.push_back(spawn);
    }
    w.map = FieldMap::Build(w.spawns);

    // Item sources: corpses (some of creatures that never spawn), skins and chests, some below the chance floor
    SourceGather gather;
    std::set<std::tuple<SourceKind, std::uint32_t, std::uint32_t>> used;
    std::set<std::pair<SourceKind, std::uint32_t>> owners;
    for (std::uint32_t item = FIRST_ITEM; item < FIRST_ITEM + ITEMS; ++item)
        for (int s = gen.Int(1, 3); s > 0; --s)
        {
            int const roll = gen.Int(0, 99);
            SourceKind const kind =
                roll < 70 ? SourceKind::Corpse : (roll < 80 ? SourceKind::Skin : SourceKind::Object);
            std::uint32_t owner = kind == SourceKind::Object ? gen.Pick(FIRST_OBJECT, OBJECTS)
                                  : gen.Chance(0.15f)        ? gen.Pick(FIRST_UNSPAWNED, UNSPAWNED)
                                                             : gen.Pick(FIRST_CREATURE, CREATURES);
            if (!used.insert({kind, owner, item}).second)
                continue;
            float const percent = gen.Chance(0.15f) ? gen.Real(0.1f, 1.9f) : gen.Real(2.0f, 100.0f);
            gather.tables[static_cast<std::size_t>(kind)].push_back(MakeLoot(owner, item, percent));
            if (owners.insert({kind, owner}).second)
            {
                gather.owners.push_back({kind, owner, owner});
                if (owner < FIRST_UNSPAWNED)
                {
                    SourceSpawn spawn;
                    spawn.entry = owner;
                    spawn.isObject = kind == SourceKind::Object;
                    gather.spawns.push_back(spawn);
                }
            }
        }
    w.index.sources = SourceTable::Build(gather);

    int const questCount = gen.Int(1, 8);
    for (int q = 0; q < questCount; ++q)
    {
        QuestRecord quest;
        quest.id = FIRST_QUEST + static_cast<std::uint32_t>(q) * 3;
        quest.xpBase = static_cast<std::uint32_t>(gen.Int(0, 2000));
        quest.copperReward = gen.Int(-200, 5000);
        std::int32_t slots = 0;
        int const objectives = gen.Int(1, 4);
        for (int j = 0; j < objectives; ++j)
        {
            int roll = gen.Int(0, 6);
            if (slots >= Circuit::KILL_SLOTS && (roll == 0 || roll == 2 || roll == 4 || roll == 5))
                roll = 3;
            std::uint32_t const at = static_cast<std::uint32_t>(quest.objectives.size());
            switch (roll)
            {
                case 0:
                    quest.objectives.push_back(MakeObjective(
                        ObjectiveKind::Kill,
                        gen.Chance(0.15f) ? gen.Pick(FIRST_UNSPAWNED, UNSPAWNED) : gen.Pick(FIRST_CREATURE, CREATURES),
                        static_cast<std::uint32_t>(gen.Int(1, 20)), slots++));
                    break;
                case 1:
                {
                    QuestObjective objective =
                        MakeObjective(ObjectiveKind::Item, gen.Pick(FIRST_ITEM, ITEMS),
                                      static_cast<std::uint32_t>(gen.Int(1, 20)), POI_FIRST_ITEM + j);
                    objective.sourceKinds = SourceKindsOf(w.index.SourcesOf(objective.target));
                    objective.given = gen.Chance(0.05f);
                    quest.objectives.push_back(objective);
                    break;
                }
                case 2:
                    quest.objectives.push_back(MakeObjective(ObjectiveKind::Object, gen.Pick(FIRST_OBJECT, OBJECTS),
                                                             static_cast<std::uint32_t>(gen.Int(1, 10)), slots++));
                    break;
                case 3:
                    quest.objectives.push_back(MakeObjective(ObjectiveKind::Explore, 4000 + at,
                                                             static_cast<std::uint32_t>(gen.Int(0, 1)), POI_NONE));
                    break;
                case 4:
                {
                    QuestObjective objective =
                        MakeObjective(ObjectiveKind::Kill, gen.Pick(FIRST_ENDER, ENDERS), 1, slots++);
                    objective.talk.how = TalkHow::Hello;
                    objective.talk.friendlyTo = TEAM_BOTH;
                    quest.objectives.push_back(objective);
                    break;
                }
                case 5:
                {
                    QuestObjective objective = MakeObjective(ObjectiveKind::Kill, gen.Pick(FIRST_UNSPAWNED, UNSPAWNED),
                                                             static_cast<std::uint32_t>(gen.Int(1, 8)), slots++);
                    objective.use = UseHow::Item;
                    quest.objectives.push_back(objective);
                    QuestUseRule rule;
                    rule.questId = quest.id;
                    rule.objective = at;
                    rule.itemId = USE_ITEM;
                    rule.entries = {gen.Pick(FIRST_CREATURE, CREATURES), gen.Pick(FIRST_CREATURE, CREATURES)};
                    std::sort(rule.entries.begin(), rule.entries.end());
                    w.index.uses.rules.push_back(rule);
                    break;
                }
                default:
                    quest.objectives.push_back(MakeObjective(ObjectiveKind::Turnin, 0, 0, POI_TURNIN));
                    break;
            }
        }
        if (gen.Chance(0.1f))
        {
            quest.event = QuestEvent::Script;
            quest.eventUse.use = UseHow::Spell;
            QuestUseRule rule;
            rule.questId = quest.id;
            rule.spellId = EVENT_SPELL;
            rule.kind = UseKind::Object;
            rule.entries = {gen.Pick(FIRST_OBJECT, OBJECTS)};
            w.index.uses.rules.push_back(rule);
        }
        for (int e = gen.Int(0, 2); e > 0; --e)
        {
            Home const& h = enderHomes[static_cast<std::size_t>(gen.Int(0, ENDERS - 1))];
            QuestPlace place;
            place.mapId = h.mapId;
            place.x = Mid(h.cellX);
            place.y = Mid(h.cellY);
            place.entry = FIRST_ENDER + static_cast<std::uint32_t>(&h - enderHomes.data());
            quest.enders.push_back(static_cast<std::uint32_t>(w.index.places.size()));
            w.index.places.push_back(place);
        }
        if (gen.Chance(0.1f))
            quest.refused[0] = static_cast<std::uint8_t>(QuestExec::Work::Escort) + 1;
        if (gen.Chance(0.1f))
            quest.refused[1] = static_cast<std::uint8_t>(QuestExec::Work::Event) + 1;
        // WQ38: some quests listed as impossible, some elite, group or dungeon quests
        if (gen.Chance(0.05f))
            quest.impossible = "data";
        if (gen.Chance(0.15f))
        {
            std::uint16_t const kinds[] = {QuestFlag::ELITE, QuestFlag::GROUP, QuestFlag::DUNGEON};
            quest.flags |= kinds[gen.Int(0, 2)];
        }
        if (gen.Chance(0.75f))
        {
            QuestSlot slot;
            slot.quest = quest.id;
            slot.state = gen.Chance(0.2f) ? Circuit::SLOT_COMPLETE : 0;
            for (std::uint16_t& counter : slot.counters)
                counter = static_cast<std::uint16_t>(gen.Int(0, 22));
            w.log.push_back(slot);
        }
        w.index.quests.push_back(std::move(quest));
    }
    std::stable_sort(w.index.uses.rules.begin(), w.index.uses.rules.end(),
                     [](QuestUseRule const& a, QuestUseRule const& b)
                     { return std::tie(a.questId, a.objective) < std::tie(b.questId, b.objective); });
    if (gen.Chance(0.1f))
        w.log.push_back({UNKNOWN_QUEST, 0, {}});

    w.ask.mapId = gen.Chance(0.8f) ? MAP_A : MAP_B;
    w.ask.x = (static_cast<float>(gen.Int(CELL_LOW, CELL_HIGH)) + gen.Real(0.0f, 1.0f)) * FIELD_CELL_YARDS;
    w.ask.y = (static_cast<float>(gen.Int(CELL_LOW, CELL_HIGH)) + gen.Real(0.0f, 1.0f)) * FIELD_CELL_YARDS;
    w.ask.radius = gen.Real(100.0f, 900.0f);
    w.ask.level = level;
    w.ask.stayMinutes = gen.Real(1.0f, 15.0f);
    w.ask.runSpeed = gen.Chance(0.5f) ? 0.0f : gen.Real(5.0f, 10.0f);
    w.ask.groupSize = gen.Chance(0.2f) ? 2 : 0;
    return w;
}

// The needs a quest's slot must give, worked out again from the step's text
// WQ34: a ready quest gives its hand-in, whatever its kind; an open one that is refused or listed (WQ38) gives nothing,
// an open elite, group or instance quest nothing to a lone bot
std::size_t ExpectedNeeds(QuestRecord const& quest, QuestSlot const& slot, std::uint8_t team, std::uint32_t groupSize)
{
    if (slot.state & Circuit::SLOT_COMPLETE)
        return 1;
    if (quest.refused[team] || !quest.impossible.empty())
        return 0;
    bool const waits =
        (quest.flags & (QuestFlag::ELITE | QuestFlag::GROUP | QuestFlag::DUNGEON | QuestFlag::RAID | QuestFlag::PVP)) !=
        0;
    if (waits && groupSize <= 1)
        return 0;
    std::size_t count = 0;
    for (std::uint32_t at = 0; at < QuestExec::ObjectiveCount(quest); ++at)
    {
        QuestObjective const& objective = QuestExec::ObjectiveAt(quest, at);
        if ((at < quest.objectives.size() && objective.kind == ObjectiveKind::Turnin) || objective.given)
            continue;
        std::uint32_t left = std::max<std::uint32_t>(objective.count, 1);
        if ((objective.kind == ObjectiveKind::Kill || objective.kind == ObjectiveKind::Object) &&
            objective.poiIndex >= 0 && objective.poiIndex < Circuit::KILL_SLOTS)
        {
            std::uint32_t const done = slot.counters[static_cast<std::size_t>(objective.poiIndex)];
            left = done >= objective.count ? 0 : objective.count - done;
        }
        count += left ? 1 : 0;
    }
    return count;
}

std::string NeedsText(std::vector<Need> const& needs)
{
    std::string text;
    char line[128];
    for (Need const& need : needs)
    {
        std::snprintf(line, sizeof(line), "%u/%u/%s/%u/%.6f/%.1f:", need.quest, need.objective, NeedKindName(need.kind),
                      need.left, static_cast<double>(need.worthPerCredit), static_cast<double>(need.seconds));
        text += line;
        for (NeedSource const& source : need.sources)
        {
            std::snprintf(line, sizeof(line), "%d.%u.%.6f,", static_cast<int>(source.kind), source.entry,
                          static_cast<double>(source.chance));
            text += line;
        }
        text += "|";
    }
    return text;
}

std::string RankingText(Ranking const& ranking)
{
    std::string text;
    char line[160];
    for (Ranked const& field : ranking.fields)
    {
        std::snprintf(line, sizeof(line), "%llx %.6f %.6f %.6f %.6f|",
                      static_cast<unsigned long long>(field.offer.centre), static_cast<double>(field.value),
                      static_cast<double>(field.offer.worth), static_cast<double>(field.walkMinutes),
                      static_cast<double>(field.riskFactor));
        text += line;
        for (NeedOffer const& one : field.offer.needs)
        {
            std::snprintf(line, sizeof(line), "%u:%.6f,", one.need, static_cast<double>(one.credits));
            text += line;
        }
    }
    for (Missing const& missing : ranking.noField)
    {
        std::snprintf(line, sizeof(line), "m%u:%s,", missing.need, NoFieldName(missing.why));
        text += line;
    }
    return text;
}

Ranked const* FindField(Ranking const& ranking, std::uint64_t centre)
{
    for (Ranked const& field : ranking.fields)
        if (field.offer.centre == centre)
            return &field;
    return nullptr;
}

float Weighted(Goals::RiskInput const& risk, FieldAsk const& ask)
{
    return Goals::RawRisk(risk, ask.groupSize) * ask.caution;
}

// Whether a row of a needed entry in the field lies within the bot's reach, worked out again: an object, or a creature
// whose middle level alone keeps the weighted risk at or below the refusal
bool AnyNeededWithinReach(FieldMap const& map, std::vector<Need> const& needs, std::uint64_t centre,
                          FieldAsk const& ask)
{
    std::int32_t const cellX = FieldCellXOf(centre);
    std::int32_t const cellY = FieldCellYOf(centre);
    for (std::int32_t dx = -1; dx <= 1; ++dx)
        for (std::int32_t dy = -1; dy <= 1; ++dy)
            for (FieldRow const& row : map.RowsOf(FieldCellKey(FieldMapOf(centre), cellX + dx, cellY + dy)))
                for (Need const& need : needs)
                {
                    if (!need.left || Fieldless(need.kind))
                        continue;
                    for (NeedSource const& source : need.sources)
                    {
                        if (source.kind != row.kind || source.entry != row.entry)
                            continue;
                        float const above =
                            (static_cast<float>(row.minLevel) + static_cast<float>(row.maxLevel)) / 2.0f -
                            static_cast<float>(ask.level);
                        if (row.kind == FieldKind::Object || need.kind == NeedKind::HandIn ||
                            std::max(above, 0.0f) * Goals::RISK_PER_LEVEL_ABOVE * ask.caution <= Goals::RISK_REFUSE)
                            return true;
                    }
                }
    return false;
}

// The generated logs, every invariant over all of them
void GeneratedLogs(std::uint32_t seed)
{
    Gen gen{std::mt19937(seed)};
    Pricing const pricing{};
    Invariant emptyLog{"the needs of an empty log are empty"};
    Invariant perQuest{"a ready quest gives one hand-in, a refused open one none, an open one a need per objective"};
    Invariant needShape{"every need has a count left, a finite worth per credit, its seconds, chances in (0, 1]"};
    Invariant zeroField{"a field with no open need is worth 0"};
    Invariant zeroNeed{"a need whose count left is 0 adds nothing"};
    Invariant finite{"every field's worth is finite and not negative, every ranked value finite and above 0"};
    Invariant spawnsUp{"more spawns of a needed entry never lower a field's worth or its ranked value"};
    Invariant walkUp{"a longer walk never raises a field's ranked value"};
    Invariant refuse{"a field whose needed targets all lie above the refuse level is never ranked"};
    Invariant same{"the same input gives the same output (twice, and from spawns in another order)"};
    Invariant covered{"every need has a field within the radius or is in the no-field list, never both or lost"};
    Invariant sorted{"the fields are sorted by worth over walk and stay, ties by cell"};
    std::size_t ranked = 0;
    std::size_t missing = 0;
    std::size_t needCount = 0;
    std::array<std::size_t, NO_FIELDS> reasons{};
    for (std::size_t logIndex = 0; logIndex < LOGS; ++logIndex)
    {
        World w = Generate(gen);
        FieldAsk const& ask = w.ask;
        std::vector<Need> const needs = Needs(w.index, w.log, w.team, pricing, w.groupSize);
        needCount += needs.size();

        emptyLog.Note(Needs(w.index, std::span<QuestSlot const>(), w.team, pricing, w.groupSize).empty(), logIndex,
                      "not empty");

        for (QuestSlot const& slot : w.log)
        {
            QuestRecord const* quest = w.index.Find(slot.quest);
            std::size_t mine = 0;
            std::size_t handIns = 0;
            for (Need const& need : needs)
                if (need.quest == slot.quest)
                {
                    ++mine;
                    handIns += need.kind == NeedKind::HandIn ? 1 : 0;
                }
            std::size_t const expected = quest ? ExpectedNeeds(*quest, slot, w.team, w.groupSize) : 0;
            bool const ready = quest && (slot.state & Circuit::SLOT_COMPLETE);
            perQuest.Note(mine == expected && handIns == (ready ? 1u : 0u), logIndex,
                          "quest " + std::to_string(slot.quest) + ": " + std::to_string(mine) + " needs, expected " +
                              std::to_string(expected));
        }
        for (Need const& need : needs)
        {
            bool ok = need.left > 0 && std::isfinite(need.worthPerCredit) && need.worthPerCredit >= 0.0f &&
                      need.seconds > 0.0f;
            for (NeedSource const& source : need.sources)
                ok = ok && source.chance > 0.0f && source.chance <= 1.0f &&
                     (need.kind != NeedKind::Loot || source.chance >= SOURCE_CHANCE_FLOOR);
            needShape.Note(ok, logIndex, "quest " + std::to_string(need.quest));
        }

        Ranking const ranking = TopFields(w.map, needs, ask);
        ranked += ranking.fields.size();
        missing += ranking.noField.size();
        for (Missing const& m : ranking.noField)
            ++reasons[static_cast<std::size_t>(m.why)];

        // Coverage: a need with credit in a ranked field, or in the list, exactly one of both
        std::vector<int> seen(needs.size(), 0);
        for (Ranked const& field : ranking.fields)
            for (NeedOffer const& one : field.offer.needs)
                if (one.credits > 0.0f && one.need < needs.size())
                    seen[one.need] |= 1;
        bool listOk = true;
        for (Missing const& m : ranking.noField)
        {
            listOk = listOk && m.need < needs.size() && !(seen[m.need] & 2) &&
                     (m.why == NoField::Fieldless) == Fieldless(needs[m.need].kind);
            if (m.need < needs.size())
                seen[m.need] |= 2;
        }
        covered.Note(listOk && std::all_of(seen.begin(), seen.end(), [](int s) { return s == 1 || s == 2; }), logIndex,
                     "a need lost or in both");

        bool order = true;
        for (std::size_t i = 1; i < ranking.fields.size(); ++i)
        {
            Ranked const& a = ranking.fields[i - 1];
            Ranked const& b = ranking.fields[i];
            order = order && (a.value > b.value || (a.value == b.value && a.offer.centre < b.offer.centre));
        }
        sorted.Note(order, logIndex, "out of order");

        same.Note(NeedsText(needs) == NeedsText(Needs(w.index, w.log, w.team, pricing, w.groupSize)) &&
                      RankingText(ranking) == RankingText(TopFields(w.map, needs, ask)),
                  logIndex, "a second call differs");
        {
            std::vector<FieldSpawn> shuffled = w.spawns;
            std::shuffle(shuffled.begin(), shuffled.end(), gen.random);
            FieldMap const other = FieldMap::Build(shuffled);
            same.Note(RankingText(ranking) == RankingText(TopFields(other, needs, ask)), logIndex,
                      "spawns in another order differ");
        }

        for (std::size_t f = 0; f < ranking.fields.size(); ++f)
        {
            Ranked const& field = ranking.fields[f];
            Offer const& offer = field.offer;
            finite.Note(std::isfinite(offer.worth) && offer.worth >= 0.0f && std::isfinite(field.value) &&
                            field.value > 0.0f && std::isfinite(field.walkMinutes),
                        logIndex, "field worth or value");
            refuse.Note(Weighted(offer.risk, ask) <= Goals::RISK_REFUSE &&
                            AnyNeededWithinReach(w.map, needs, offer.centre, ask),
                        logIndex, "a refused field ranked");
            if (f >= 3)
                continue;

            // Zero: no needs, every count left at 0, one need's count left at 0
            zeroField.Note(FieldOffer(w.map, std::span<Need const>(), offer.centre, ask).worth == 0.0f, logIndex,
                           "no needs");
            std::vector<Need> none = needs;
            for (Need& need : none)
                need.left = 0;
            Offer const empty = FieldOffer(w.map, none, offer.centre, ask);
            zeroField.Note(empty.worth == 0.0f && std::all_of(empty.needs.begin(), empty.needs.end(),
                                                              [](NeedOffer const& one) { return one.credits == 0.0f; }),
                           logIndex, "every count left 0");
            for (NeedOffer const& one : offer.needs)
            {
                std::vector<Need> less = needs;
                less[one.need].left = 0;
                Offer const without = FieldOffer(w.map, less, offer.centre, ask);
                float credits = -1.0f;
                for (NeedOffer const& other : without.needs)
                    if (other.need == one.need)
                        credits = other.credits;
                float const expect = offer.worth - one.credits * needs[one.need].worthPerCredit;
                zeroNeed.Note(
                    credits == 0.0f && std::fabs(without.worth - expect) <= SLACK * std::max(1.0f, offer.worth),
                    logIndex, "need " + std::to_string(one.need));
            }

            // A longer walk: the bot farther out on the ray from the field's centre
            FieldAsk nearAsk = ask;
            nearAsk.radius = FAR_RADIUS;
            FieldAsk farAsk = nearAsk;
            float dx = ask.x - offer.x;
            float dy = ask.y - offer.y;
            float const length = std::hypot(dx, dy);
            if (length < 1.0f)
            {
                dx = 1.0f;
                dy = 0.0f;
            }
            else
            {
                dx /= length;
                dy /= length;
            }
            float const farther = length + gen.Real(50.0f, 500.0f);
            farAsk.x = offer.x + dx * farther;
            farAsk.y = offer.y + dy * farther;
            Ranking const nearRanking = TopFields(w.map, needs, nearAsk);
            Ranked const* nearField = FindField(nearRanking, offer.centre);
            Ranking const farRanking = TopFields(w.map, needs, farAsk);
            Ranked const* farField = FindField(farRanking, offer.centre);
            walkUp.Note(nearField && farField && farField->value <= nearField->value * (1.0f + SLACK), logIndex,
                        "field " + std::to_string(offer.centre));
        }

        // More spawns: copy a spawn of a needed entry in the best field (not elite, at its row's median respawn, at
        // the same point and levels), so the field's level ranges and medians stay as they were
        if (!ranking.fields.empty())
        {
            Ranked const& best = ranking.fields.front();
            std::int32_t const cellX = FieldCellXOf(best.offer.centre);
            std::int32_t const cellY = FieldCellYOf(best.offer.centre);
            std::uint32_t const mapId = FieldMapOf(best.offer.centre);
            bool copied = false;
            std::vector<FieldSpawn> more = w.spawns;
            for (NeedOffer const& one : best.offer.needs)
            {
                if (one.credits <= 0.0f || copied)
                    continue;
                for (NeedSource const& source : needs[one.need].sources)
                {
                    for (FieldSpawn const& spawn : w.spawns)
                    {
                        std::uint64_t const cell = FieldCellKeyAt(spawn.mapId, spawn.x, spawn.y);
                        if (spawn.kind != source.kind || spawn.entry != source.entry || spawn.mapId != mapId ||
                            std::abs(FieldCellXOf(cell) - cellX) > 1 || std::abs(FieldCellYOf(cell) - cellY) > 1)
                            continue;
                        FieldSpawn copy = spawn;
                        copy.elite = false;
                        for (FieldRow const& row : w.map.RowsOf(cell))
                            if (row.kind == spawn.kind && row.entry == spawn.entry)
                                copy.respawnSecs = row.medianRespawn;
                        more.push_back(copy);
                        copied = true;
                        break;
                    }
                    if (copied)
                        break;
                }
            }
            if (copied)
            {
                FieldMap const bigger = FieldMap::Build(more);
                Offer const after = FieldOffer(bigger, needs, best.offer.centre, ask);
                Ranking const biggerRanking = TopFields(bigger, needs, ask);
                Ranked const* again = FindField(biggerRanking, best.offer.centre);
                spawnsUp.Note(AtLeast(after.worth, best.offer.worth) &&
                                  Weighted(after.risk, ask) <= Weighted(best.offer.risk, ask) + SLACK && again &&
                                  AtLeast(again->value, best.value),
                              logIndex, "field " + std::to_string(best.offer.centre));
            }
        }
    }
    std::printf("seed=%u logs=%zu needs=%zu ranked fields=%zu no-field needs=%zu (", seed, LOGS, needCount, ranked,
                missing);
    for (std::size_t r = 0; r < NO_FIELDS; ++r)
        std::printf("%s%s %zu", r ? ", " : "", NO_FIELD_NAMES[r], reasons[r]);
    std::printf(")\n");
    for (Invariant const* inv : {&emptyLog, &perQuest, &needShape, &zeroField, &zeroNeed, &finite, &spawnsUp, &walkUp,
                                 &refuse, &same, &covered, &sorted})
    {
        std::string what = std::string(inv->name) + ": " + std::to_string(inv->checked) + " checks";
        if (inv->broken)
            what += ", " + std::to_string(inv->broken) + " broken, first at log " + std::to_string(inv->firstLog) +
                    " of seed " + std::to_string(seed) + " (" + inv->firstWhat + ")";
        Check(inv->checked > 0 && !inv->broken, what);
    }
}

// Hand-built: a quest index of these records over these loot rows, the item objectives' source kinds filled
QuestIndex FixedIndex(std::vector<QuestRecord> quests, SourceGather const& gather)
{
    QuestIndex index;
    index.sources = SourceTable::Build(gather);
    for (QuestRecord& quest : quests)
        for (QuestObjective& objective : quest.objectives)
            if (objective.kind == ObjectiveKind::Item)
                objective.sourceKinds = SourceKindsOf(index.SourcesOf(objective.target));
    index.quests = std::move(quests);
    return index;
}

QuestRecord MakeQuest(std::uint32_t id, std::uint32_t xp, std::vector<QuestObjective> objectives)
{
    QuestRecord quest;
    quest.id = id;
    quest.xpBase = xp;
    quest.objectives = std::move(objectives);
    return quest;
}

void AddSpawns(std::vector<FieldSpawn>& spawns, std::uint32_t entry, std::int32_t cellX, std::int32_t cellY, int count,
               std::uint8_t minLevel, std::uint8_t maxLevel, std::uint32_t respawn)
{
    for (int i = 0; i < count; ++i)
        spawns.push_back(MakeSpawn(FieldKind::Creature, entry, MAP_A, Mid(cellX) + static_cast<float>(i), Mid(cellY),
                                   minLevel, maxLevel, true, respawn));
}

// The research's minute 10 (WQ35-run-fields.md D): Harvest Watchers 20, Goretusk Liver 8, Murloc Eye 3, worked by hand
void MomentTen()
{
    constexpr std::uint32_t WATCHER = 114;
    constexpr std::uint32_t GORETUSK = 157;
    constexpr std::uint32_t MURLOC = 458;
    constexpr std::uint32_t LIVER = 723;
    constexpr std::uint32_t EYE = 730;
    std::vector<FieldSpawn> spawns;
    AddSpawns(spawns, WATCHER, 0, 0, 6, 10, 11, 300);
    AddSpawns(spawns, GORETUSK, 1, 0, 4, 10, 11, 300);
    AddSpawns(spawns, MURLOC, 20, 20, 3, 11, 12, 300);
    FieldMap const map = FieldMap::Build(spawns);
    SourceGather gather;
    AddSource(gather, SourceKind::Corpse, GORETUSK, LIVER, 50.0f, true);
    AddSource(gather, SourceKind::Corpse, MURLOC, EYE, 40.0f, true);
    QuestIndex const index =
        FixedIndex({MakeQuest(9, 840, {MakeObjective(ObjectiveKind::Kill, WATCHER, 20, 0)}),
                    MakeQuest(22, 875, {MakeObjective(ObjectiveKind::Item, LIVER, 8, POI_FIRST_ITEM)}),
                    MakeQuest(38, 400, {MakeObjective(ObjectiveKind::Item, EYE, 3, POI_FIRST_ITEM)})},
                   gather);
    std::vector<QuestSlot> const log = {{9, 0, {}}, {22, 0, {}}, {38, 0, {}}};
    std::vector<Need> const needs = Needs(index, log, 0, Pricing{}, ALONE);
    Check(needs.size() == 3 && needs[0].kind == NeedKind::Kill && needs[1].kind == NeedKind::Loot &&
              needs[2].kind == NeedKind::Loot,
          "minute 10: three needs, a kill and two loots");
    if (needs.size() != 3)
        return;
    // Worth per credit (WQ17b): the credits' 40 % of the XP over the objective's credits (copper 0)
    Check(Near(needs[0].worthPerCredit, 840.0 * 0.4 / 20.0) && Near(needs[1].worthPerCredit, 875.0 * 0.4 / 8.0) &&
              Near(needs[2].worthPerCredit, 400.0 * 0.4 / 3.0),
          "minute 10: worth per credit 16.8, 43.75, 53.3");
    Check(
        needs[1].sources.size() == 1 && needs[1].sources[0].entry == GORETUSK && Near(needs[1].sources[0].chance, 0.5),
        "minute 10: the liver from Goretusks at 50 %");
    FieldAsk ask;
    ask.mapId = MAP_A;
    ask.x = Mid(0);
    ask.y = Mid(0);
    ask.level = 12;
    ask.stayMinutes = 10.0f;
    Ranking const ranking = TopFields(map, needs, ask);
    // Watchers 6 x (1 + 600 / 300) = 18 of 20 (the cap 600 / 30 = 20); livers 4 x 3 x 0.5 = 6 of 8 (cap 13.3)
    double const worth = 18.0 * 16.8 + 6.0 * 43.75;
    Check(!ranking.fields.empty() && ranking.fields[0].offer.centre == FieldCellKey(MAP_A, 0, 0) &&
              Near(ranking.fields[0].offer.worth, worth) && Near(ranking.fields[0].value, worth / 10.0) &&
              Near(ranking.fields[0].offer.worthPerMinute, worth / 10.0),
          "minute 10: the field on the watchers is first, worth 564.9 over 10 minutes");
    Check(ranking.noField.size() == 1 && ranking.noField[0].need == 2 && ranking.noField[0].why == NoField::OutOfRadius,
          "minute 10: the eyes' murlocs lie 2,000 yd off: out of the radius");
    Offer const offer = FieldOffer(map, needs, FieldCellKey(MAP_A, 0, 0), ask);
    Check(offer.needs.size() == 2 && Near(offer.needs[0].credits, 18.0) && Near(offer.needs[1].credits, 6.0) &&
              offer.risk.levelsAbove == 0.0f,
          "minute 10: 18 kills and 6 livers, no level above the bot");
    FieldAsk shortStay = ask;
    shortStay.stayMinutes = 1.0f;  // 6 x (1 + 0.2) = 7.2 watchers, capped at 60 / 30 = 2; livers capped at 1.33
    Offer const brief = FieldOffer(map, needs, FieldCellKey(MAP_A, 0, 0), shortStay);
    Check(brief.needs.size() == 2 && Near(brief.needs[0].credits, 2.0) && Near(brief.needs[1].credits, 60.0 / 45.0),
          "minute 10: one minute caps the credits at what one bot kills");
}

// An item from two sources at different chances: 2 x 0.6 (no respawn) + 5 x (1 + 600 / 600) x 0.1 = 2.2
void TwoSources()
{
    constexpr std::uint32_t STRONG = 300;
    constexpr std::uint32_t WEAK = 301;
    constexpr std::uint32_t ITEM = 800;
    std::vector<FieldSpawn> spawns;
    AddSpawns(spawns, STRONG, 0, 0, 2, 5, 5, 0);
    AddSpawns(spawns, WEAK, 0, 0, 5, 5, 5, 600);
    SourceGather gather;
    AddSource(gather, SourceKind::Corpse, STRONG, ITEM, 60.0f, true);
    AddSource(gather, SourceKind::Corpse, WEAK, ITEM, 10.0f, true);
    QuestIndex const index =
        FixedIndex({MakeQuest(50, 100, {MakeObjective(ObjectiveKind::Item, ITEM, 10, POI_FIRST_ITEM)})}, gather);
    std::vector<QuestSlot> const log = {{50, 0, {}}};
    std::vector<Need> const needs = Needs(index, log, 0, Pricing{}, ALONE);
    FieldAsk ask;
    ask.x = Mid(0);
    ask.y = Mid(0);
    ask.level = 5;
    ask.stayMinutes = 10.0f;
    Offer const offer = FieldOffer(FieldMap::Build(spawns), needs, FieldCellKey(MAP_A, 0, 0), ask);
    Check(needs.size() == 1 && needs[0].sources.size() == 2 && offer.needs.size() == 1 &&
              Near(offer.needs[0].credits, 2.2),
          "two sources at 60 % and 10 %: 2.2 items in ten minutes");
}

// questing-2 QS.6: a held quest by its colour for the bot: a red one gives no need, its hand-in neither; an orange
// and a green one count a share of their worth; without a bot level no colour is asked
void Colours()
{
    constexpr std::uint32_t MOB = 320;
    auto const quest = [](std::uint32_t id, std::int32_t level)
    {
        QuestRecord record = MakeQuest(id, 1000, {MakeObjective(ObjectiveKind::Kill, MOB, 10, 0)});
        record.level = level;
        return record;
    };
    QuestIndex const index = FixedIndex({quest(70, 20), quest(71, 24), quest(72, 25), quest(73, 16)}, SourceGather{});
    std::vector<QuestSlot> const log = {{70, 0, {}}, {71, 0, {}}, {72, 0, {}}, {73, 0, {}}};
    Pricing at20{};
    at20.level = 20;
    std::vector<Need> const needs = Needs(index, log, 0, at20, ALONE);
    auto const worth = [&](std::uint32_t id) -> float
    {
        for (Need const& need : needs)
            if (need.quest == id)
                return need.worthPerCredit;
        return -1.0f;
    };
    Check(worth(72) < 0.0f, "colours: a red quest gives no need");
    Check(worth(70) > 0.0f && std::fabs(worth(71) - 0.3f * worth(70)) < 1e-4f * worth(70),
          "colours: an orange quest counts 0.3 of a yellow one");
    Check(std::fabs(worth(73) - worth(71)) < 1e-4f * worth(70), "colours: a green quest counts as an orange one");
    std::vector<QuestSlot> const ready = {{72, Circuit::SLOT_COMPLETE, {}}};
    Check(Needs(index, ready, 0, at20, ALONE).empty(), "colours: a ready red quest waits too");
    Check(Needs(index, log, 0, Pricing{}, ALONE).size() == 4, "colours: no bot level, no colour");
    Check(QuestWorth(index.quests[1], at20) == 0.3f * QuestWorth(index.quests[1], Pricing{}),
          "colours: the quest's worth carries the share");
}

// Needed targets far above the bot: never ranked, listed as too risky; a little above: ranked at a lower worth
void TooHigh()
{
    constexpr std::uint32_t HIGH = 310;
    constexpr std::uint32_t ABOVE = 311;
    std::vector<FieldSpawn> spawns;
    AddSpawns(spawns, HIGH, 0, 0, 5, 30, 30, 300);
    AddSpawns(spawns, ABOVE, 5, 5, 5, 15, 15, 300);
    QuestIndex const index = FixedIndex({MakeQuest(60, 500, {MakeObjective(ObjectiveKind::Kill, HIGH, 5, 0)}),
                                         MakeQuest(61, 500, {MakeObjective(ObjectiveKind::Kill, ABOVE, 5, 0)})},
                                        SourceGather{});
    std::vector<QuestSlot> const log = {{60, 0, {}}, {61, 0, {}}};
    std::vector<Need> const needs = Needs(index, log, 0, Pricing{}, ALONE);
    FieldAsk ask;
    ask.x = Mid(2);
    ask.y = Mid(2);
    ask.level = 10;
    ask.stayMinutes = 10.0f;
    Ranking const ranking = TopFields(FieldMap::Build(spawns), needs, ask);
    bool highRanked = false;
    for (Ranked const& field : ranking.fields)
        for (NeedOffer const& one : field.offer.needs)
            highRanked = highRanked || (one.need == 0 && one.credits > 0.0f);
    Check(!highRanked && ranking.noField.size() == 1 && ranking.noField[0].need == 0 &&
              ranking.noField[0].why == NoField::TooRisky,
          "targets 20 levels above the bot: never ranked, listed too risky");
    Ranked const* above = FindField(ranking, FieldCellKey(MAP_A, 5, 5));
    Check(above && Near(above->riskFactor, 1.0 / 1.75), "targets 5 levels above: ranked at 1 / (1 + 5 x 0.15)");
}

// Explore and talk need no field; a refused quest gives no need; a ready quest is a hand-in at its ender
void Kinds()
{
    constexpr std::uint32_t TALKER = 720;
    constexpr std::uint32_t ENDER = 721;
    std::vector<FieldSpawn> spawns;
    spawns.push_back(MakeSpawn(FieldKind::Creature, TALKER, MAP_A, Mid(0), Mid(0), 20, 20, false, 300));
    spawns.push_back(MakeSpawn(FieldKind::Creature, ENDER, MAP_A, Mid(1), Mid(1), 20, 20, false, 300));
    QuestObjective talk = MakeObjective(ObjectiveKind::Kill, TALKER, 1, 0);
    talk.talk.how = TalkHow::Select;
    talk.talk.friendlyTo = TEAM_BOTH;
    QuestRecord refused = MakeQuest(72, 300, {MakeObjective(ObjectiveKind::Kill, TALKER, 3, 0)});
    refused.refused[0] = static_cast<std::uint8_t>(QuestExec::Work::Escort) + 1;
    QuestRecord ready = MakeQuest(73, 300, {MakeObjective(ObjectiveKind::Kill, TALKER, 3, 0)});
    ready.enders = {0};
    QuestIndex index = FixedIndex(
        {MakeQuest(70, 300, {MakeObjective(ObjectiveKind::Explore, 4001, 0, POI_NONE), talk}), refused, ready},
        SourceGather{});
    QuestPlace place;
    place.mapId = MAP_A;
    place.x = Mid(1);
    place.y = Mid(1);
    place.entry = ENDER;
    index.places.push_back(place);
    std::vector<QuestSlot> const log = {{70, 0, {}}, {72, 0, {}}, {73, Circuit::SLOT_COMPLETE, {3, 0, 0, 0}}};
    std::vector<Need> const needs = Needs(index, log, 0, Pricing{}, ALONE);
    Check(needs.size() == 3 && needs[0].kind == NeedKind::Explore && needs[1].kind == NeedKind::Talk &&
              needs[1].sources.size() == 1 && needs[1].sources[0].entry == TALKER &&
              needs[2].kind == NeedKind::HandIn && needs[2].objective == HAND_IN && needs[2].sources.size() == 1 &&
              needs[2].sources[0].entry == ENDER && needs[2].seconds == Circuit::TURNIN_SECONDS &&
              Near(needs[2].worthPerCredit, 300.0 * 0.5),
          "explore and talk listed, the refused quest left out, the ready one a hand-in at its ender");
    FieldAsk ask;
    ask.x = Mid(0);
    ask.y = Mid(0);
    ask.level = 20;
    ask.stayMinutes = 5.0f;
    Ranking const ranking = TopFields(FieldMap::Build(spawns), needs, ask);
    Check(ranking.noField.size() == 2 && ranking.noField[0].why == NoField::Fieldless &&
              ranking.noField[1].why == NoField::Fieldless && !ranking.fields.empty() &&
              ranking.fields[0].offer.needs.size() == 1 && ranking.fields[0].offer.needs[0].need == 2 &&
              Near(ranking.fields[0].offer.needs[0].credits, 1.0),
          "explore and talk need no field; the hand-in's field holds its ender");
    FieldAsk low = ask;
    low.level = 1;
    Ranking const lowRanking = TopFields(FieldMap::Build(spawns), needs, low);
    Check(lowRanking.noField.size() == 2 && !lowRanking.fields.empty() &&
              lowRanking.fields[0].offer.needs.size() == 1 && lowRanking.fields[0].offer.needs[0].need == 2 &&
              Near(lowRanking.fields[0].offer.needs[0].credits, 1.0),
          "the hand-in's ender 19 levels above the bot: a friendly creature, its field still ranked");
    std::array<std::uint32_t, 1> const maps = {MAP_A};
    std::array<std::uint32_t, 1> const otherMaps = {MAP_B};
    FieldMap const map = FieldMap::Build(spawns);
    Check(!MapGap(map, needs[2], maps) && MapGap(map, needs[2], otherMaps) == NoField::OtherMap &&
              MapGap(map, needs[0], maps) == NoField::Fieldless,
          "the census's gap: a field on the quest's map, another map, none needed");
}

// WQ17b: a quest's worth by step: the take 10 %, the credits 40 % spread over them, the hand-in 50 %; a quest without
// a worked objective gives 90 % at its hand-in
void Steps()
{
    constexpr std::uint32_t WOLF = 320;
    QuestRecord worked = MakeQuest(
        90, 1000,
        {MakeObjective(ObjectiveKind::Kill, WOLF, 4, 0), MakeObjective(ObjectiveKind::Explore, 4002, 0, POI_NONE)});
    worked.givers = {0};
    QuestRecord bare = MakeQuest(91, 1000, {});
    bare.givers = {0};
    QuestIndex index = FixedIndex({worked, bare}, SourceGather{});
    QuestPlace giver;
    giver.mapId = MAP_A;
    giver.x = Mid(0);
    giver.y = Mid(0);
    giver.entry = FIRST_ENDER;
    index.places.push_back(giver);
    std::vector<QuestSlot> const open = {{90, 0, {}}};
    std::vector<QuestSlot> const ready = {{90, Circuit::SLOT_COMPLETE, {4, 0, 0, 0}}, {91, Circuit::SLOT_COMPLETE, {}}};
    std::vector<Need> const credits = Needs(index, open, 0, Pricing{}, ALONE);
    std::vector<Need> const handIns = Needs(index, ready, 0, Pricing{}, ALONE);
    std::optional<Need> const take = TakeNeed(index, index.quests[0], 0, Pricing{});
    std::optional<Need> const bareTake = TakeNeed(index, index.quests[1], 0, Pricing{});
    Check(credits.size() == 2 && credits[0].left == 4 && Near(credits[0].worthPerCredit, 80.0) &&
              credits[1].left == 1 && Near(credits[1].worthPerCredit, 80.0),
          "steps: five credits at 400 / 5 = 80");
    Check(handIns.size() == 2 && Near(handIns[0].worthPerCredit, 500.0) && Near(handIns[1].worthPerCredit, 900.0),
          "steps: the hand-in 500, without an objective 900");
    Check(take && take->kind == NeedKind::Take && take->objective == TAKE && take->left == 1 &&
              Near(take->worthPerCredit, 100.0) && take->sources.size() == 1 && take->sources[0].entry == FIRST_ENDER &&
              bareTake && Near(bareTake->worthPerCredit, 100.0),
          "steps: the take 100 at the giver");
}

// WQ17b, the replay's moments 22 and 29: a level-26 rare alone in a cell beside a level-12 field no longer closes it
// and counts to the elite share; a camp of level-20 elites beside the field still closes it
void LoneRare()
{
    constexpr std::uint32_t BOAR = 330;  // needed, level 11-12
    constexpr std::uint32_t WOLF = 331;  // ordinary hostiles around
    constexpr std::uint32_t RARE = 332;
    constexpr std::uint32_t GUARD = 333;
    std::vector<FieldSpawn> spawns;
    AddSpawns(spawns, BOAR, 0, 0, 6, 11, 12, 300);
    AddSpawns(spawns, WOLF, -1, 0, 5, 10, 12, 300);
    AddSpawns(spawns, WOLF, 0, -1, 5, 10, 12, 300);
    AddSpawns(spawns, RARE, 1, 1, 1, 26, 26, 0);
    QuestIndex const index =
        FixedIndex({MakeQuest(95, 800, {MakeObjective(ObjectiveKind::Kill, BOAR, 10, 0)})}, SourceGather{});
    std::vector<QuestSlot> const log = {{95, 0, {}}};
    std::vector<Need> const needs = Needs(index, log, 0, Pricing{}, ALONE);
    FieldAsk ask;
    ask.x = Mid(0);
    ask.y = Mid(0);
    ask.level = 12;
    ask.stayMinutes = 5.0f;
    std::uint64_t const centre = FieldCellKey(MAP_A, 0, 0);
    FieldMap const lone = FieldMap::Build(spawns);
    Offer const open = FieldOffer(lone, needs, centre, ask);
    Check(open.risk.levelsAbove == 0.0f && Near(open.risk.eliteShare, 1.0 / 17.0) &&
              Weighted(open.risk, ask) <= Goals::RISK_REFUSE && FindField(TopFields(lone, needs, ask), centre),
          "a level-26 rare alone beside a level-12 field: the field open, the rare one of 17 in its elite share");
    std::vector<FieldSpawn> camp = spawns;
    for (std::pair<std::int32_t, std::int32_t> const& cell :
         {std::make_pair(1, 0), std::make_pair(1, -1), std::make_pair(0, 1)})
    {
        std::size_t const first = camp.size();
        AddSpawns(camp, GUARD, cell.first, cell.second, 5, 20, 20, 300);
        for (std::size_t i = first; i < camp.size(); ++i)
            camp[i].elite = true;
    }
    FieldMap const guarded = FieldMap::Build(camp);
    Offer const closed = FieldOffer(guarded, needs, centre, ask);
    Check(Weighted(closed.risk, ask) > Goals::RISK_REFUSE && !FindField(TopFields(guarded, needs, ask), centre),
          "a camp of fifteen level-20 elites beside the field: still refused");
}

// WQ17b: an explore with a place in the index gets a field there and no gap, one without stays a named gap; the takes
// are the quests given near the bot that the rules allow, at their givers
void PlacesAndTakes()
{
    QuestObjective placedExplore = MakeObjective(ObjectiveKind::Explore, 4003, 0, POI_NONE);
    AreaPoint area;
    area.mapId = MAP_A;
    area.x = Mid(3);
    area.y = Mid(3);
    area.radius = 20.0f;
    area.points = 1;
    placedExplore.areas.push_back(area);
    QuestIndex const index =
        FixedIndex({MakeQuest(100, 500, {placedExplore, MakeObjective(ObjectiveKind::Explore, 4004, 0, POI_NONE)})},
                   SourceGather{});
    std::vector<QuestSlot> const log = {{100, 0, {}}};
    std::vector<Need> const needs = Needs(index, log, 0, Pricing{}, ALONE);
    std::vector<FieldSpawn> spawns;
    AddSpawns(spawns, 340, 3, 3, 2, 5, 5, 300);
    FieldMap const map = FieldMap::Build(spawns);
    FieldAsk ask;
    ask.x = Mid(2);
    ask.y = Mid(2);
    ask.level = 5;
    ask.stayMinutes = 5.0f;
    Ranking const ranking = TopFields(map, needs, ask);
    std::array<std::uint32_t, 1> const maps = {MAP_A};
    Check(needs.size() == 2 && needs[0].places.size() == 1 && needs[1].places.empty() && UsesPlaces(map, needs[0]) &&
              !ranking.fields.empty() && ranking.fields[0].offer.needs.size() == 1 &&
              ranking.fields[0].offer.needs[0].need == 0 && Near(ranking.fields[0].offer.needs[0].credits, 1.0) &&
              ranking.noField.size() == 1 && ranking.noField[0].need == 1 &&
              ranking.noField[0].why == NoField::Fieldless && !MapGap(map, needs[0], maps) &&
              MapGap(map, needs[1], maps) == NoField::Fieldless,
          "an explore with a place: a field there, no gap; one without a place: a named gap");

    QuestGather gather;
    QuestRecord high = MakeQuest(111, 400, {});
    high.minLevel = 30;
    QuestRecord elite = MakeQuest(115, 400, {});
    elite.flags = QuestFlag::ELITE;
    gather.quests = {MakeQuest(110, 400, {}), high, MakeQuest(112, 400, {}), MakeQuest(113, 400, {}),
                     MakeQuest(114, 400, {}), elite};
    QuestPlace near;
    near.mapId = MAP_A;
    near.x = Mid(0);
    near.y = Mid(0);
    near.entry = 750;
    QuestPlace away = near;
    away.x = Mid(40);
    away.entry = 751;
    gather.places = {near, away};
    gather.relations = {{110, 750, false, false},
                        {111, 750, false, false},
                        {112, 750, false, false},
                        {113, 751, false, false},
                        {115, 750, false, false}};  // 114: only an item starts it
    QuestIndex const built = QuestIndex::Build(gather);
    std::vector<std::uint32_t> const candidates = TakeCandidates(built, MAP_A, Mid(0), Mid(0), 200.0f);
    std::array<std::uint32_t, 1> const inLog = {112};
    std::vector<std::uint32_t> const answer = TakeFixture::Answer(built, HUMAN, 1, 10, {}, inLog);
    Take::Facts facts;
    facts.race = HUMAN;
    facts.level = 10;
    facts.eliteGroup = true;
    facts.inLog = inLog;
    facts.takeable = answer;
    std::vector<Need> const takes = TakeNeeds(built, facts, MAP_A, candidates, Pricing{});
    Check(candidates == std::vector<std::uint32_t>{110, 111, 112, 115} && takes.size() == 1 && takes[0].quest == 110 &&
              takes[0].kind == NeedKind::Take && takes[0].sources.size() == 1 && takes[0].sources[0].entry == 750 &&
              Near(takes[0].worthPerCredit, 40.0),
          "takes: of the quests given near the bot the one the rules allow (not too low, in the log or elite)");

    // WQ18b part 3 (fault F5): the far takes. Forty givers in a row, a hundred yards apart, one quest each
    QuestGather wide;
    std::vector<std::uint32_t> nearest;
    for (std::uint32_t i = 0; i < 40; ++i)
    {
        QuestRecord quest = MakeQuest(200 + i, 400, {});
        if (i == 5)
            quest.minLevel = 30;  // refused: the next giver's quest takes its turn
        else if (i >= 1 && nearest.size() < FAR_TAKES)
            nearest.push_back(200 + i);
        wide.quests.push_back(quest);
        QuestPlace place = near;
        place.x = Mid(0) + 100.0f * static_cast<float>(i + 1);
        place.entry = 800 + i;
        wide.places.push_back(place);
        wide.relations.push_back({200 + i, 800 + i, false, false});
    }
    QuestIndex const row = QuestIndex::Build(wide);
    std::vector<std::uint32_t> const rowAnswer = TakeFixture::Answer(row, HUMAN, 1, 10, {}, {});
    Take::Facts roamer = facts;
    roamer.inLog = {};
    roamer.takeable = rowAnswer;
    std::vector<std::uint32_t> const close = TakeCandidates(row, MAP_A, Mid(0), Mid(0), 150.0f);
    std::vector<std::uint32_t> const far =
        FarTakeCandidates(row, roamer, MAP_A, Mid(0), Mid(0), 150.0f, close, Pricing{});
    // From 2000 yd before the row: its first ten givers lie within reach, none within the near radius
    std::vector<std::uint32_t> const reached =
        FarTakeCandidates(row, roamer, MAP_A, Mid(0) - 2000.0f, Mid(0), 150.0f, {}, Pricing{});
    Check(close == std::vector<std::uint32_t>{200} && far.size() == FAR_TAKES && far == nearest && far.back() == 225 &&
              reached == std::vector<std::uint32_t>{200, 201, 202, 203, 204, 206, 207, 208, 209},
          "far takes: beyond the near radius the quests of the nearest givers within reach that the rules allow, at "
          "most FAR_TAKES, none of the near ones (mutants: no count limit, no reach)");

    // One far giver place with more quests than the cap: the count stops inside the place too (WQL5b review L9)
    QuestGather crowded;
    for (std::uint32_t i = 0; i < FAR_TAKES + 2; ++i)
    {
        crowded.quests.push_back(MakeQuest(300 + i, 400, {}));
        crowded.relations.push_back({300 + i, 900, false, false});
    }
    QuestPlace hub = near;
    hub.x = Mid(0) + 500.0f;
    hub.entry = 900;
    crowded.places.push_back(hub);
    QuestIndex const busy = QuestIndex::Build(crowded);
    std::vector<std::uint32_t> const busyAnswer = TakeFixture::Answer(busy, HUMAN, 1, 10, {}, {});
    Take::Facts busyRoamer = roamer;
    busyRoamer.takeable = busyAnswer;
    std::vector<std::uint32_t> const capped =
        FarTakeCandidates(busy, busyRoamer, MAP_A, Mid(0), Mid(0), 150.0f, {}, Pricing{});
    Check(capped.size() == FAR_TAKES,
          "far takes: a far place that gives more quests than FAR_TAKES gives at most FAR_TAKES (mutant: the cap "
          "inside a place removed)");
}

// WQ17b: the generated worlds again (their own stream of the seed) with givers, the take rules' fields and objective
// places added: the shares, the takes, the places and ordinary hostiles over every log
// WQ38, WQ34: a quest on the list of impossible quests gives no objective need; an elite, group or dungeon quest gives
// a lone bot none and a party all of them; a ready quest of any of them gives its hand-in. A given-up quest gives none,
// its ready hand-in neither (a failed hand-in is a give-up: no trip back to the ender)
void ListedAndAlone()
{
    constexpr std::uint32_t BOAR = 101;
    QuestRecord listed = MakeQuest(40, 400, {MakeObjective(ObjectiveKind::Kill, BOAR, 5, 0)});
    listed.impossible = "far_trip";
    QuestRecord elite = MakeQuest(41, 400, {MakeObjective(ObjectiveKind::Kill, BOAR, 5, 0)});
    elite.flags = QuestFlag::ELITE;
    QuestRecord group = MakeQuest(42, 400, {MakeObjective(ObjectiveKind::Kill, BOAR, 5, 0)});
    group.flags = QuestFlag::GROUP;
    QuestRecord dungeon = MakeQuest(43, 400, {MakeObjective(ObjectiveKind::Kill, BOAR, 5, 0)});
    dungeon.flags = QuestFlag::DUNGEON;
    QuestRecord plain = MakeQuest(44, 400, {MakeObjective(ObjectiveKind::Kill, BOAR, 5, 0)});
    QuestIndex const index = FixedIndex({listed, elite, group, dungeon, plain}, SourceGather{});
    std::vector<QuestSlot> const open = {{40, 0, {}}, {41, 0, {}}, {42, 0, {}}, {43, 0, {}}, {44, 0, {}}};
    std::vector<QuestSlot> const ready = {{40, Circuit::SLOT_COMPLETE, {}},
                                          {41, Circuit::SLOT_COMPLETE, {}},
                                          {42, Circuit::SLOT_COMPLETE, {}},
                                          {43, Circuit::SLOT_COMPLETE, {}},
                                          {44, Circuit::SLOT_COMPLETE, {}}};
    auto const quests = [](std::vector<Need> const& needs)
    {
        std::vector<std::uint32_t> ids;
        for (Need const& need : needs)
            ids.push_back(need.quest);
        return ids;
    };
    Check(quests(Needs(index, open, 0, Pricing{}, ALONE)) == std::vector<std::uint32_t>{44},
          "listed and alone: a lone bot's open needs are the plain quest's only");
    Check(quests(Needs(index, open, 0, Pricing{}, 2)) == std::vector<std::uint32_t>{41, 42, 43, 44},
          "listed and alone: a bot in a group of two works the elite, group and dungeon quests too, never the listed");
    std::vector<Need> const handIns = Needs(index, ready, 0, Pricing{}, ALONE);
    Check(
        quests(handIns) == std::vector<std::uint32_t>{40, 41, 42, 43, 44} &&
            std::all_of(handIns.begin(), handIns.end(), [](Need const& need) { return need.kind == NeedKind::HandIn; }),
        "listed and alone: a lone bot hands in every ready quest, a listed or flagged one too");
    std::vector<std::uint32_t> const givenUp = {40, 42, 44};
    Check(quests(Needs(index, open, 0, Pricing{}, 2, givenUp)) == std::vector<std::uint32_t>{41, 43},
          "given up: a given-up quest gives no need");
    Check(quests(Needs(index, ready, 0, Pricing{}, ALONE, givenUp)) == std::vector<std::uint32_t>{41, 43},
          "given up: a given-up quest gives no hand-in");
}

void GeneratedSteps(std::uint32_t seed)
{
    Gen gen{std::mt19937(seed + 1)};
    Pricing const pricing{};
    Invariant sums{"the shares of every quest sum to its worth (take, credits, hand-in)"};
    Invariant handIn{"a ready quest's hand-in is worth more than one of its credits was"};
    Invariant takeLog{"a quest in the log gives no take"};
    Invariant takeRule{"a quest Take::May refuses gives no need, one it allows with a giver on the map exactly one"};
    Invariant noGap{"a need with a place in the index is never returned as a gap"};
    Invariant ordinary{"more ordinary hostiles of a level the bot can fight never close a field"};
    std::size_t takeCount = 0;
    std::size_t refusedCount = 0;
    std::size_t placedCount = 0;
    std::size_t grownCount = 0;
    for (std::size_t logIndex = 0; logIndex < LOGS; ++logIndex)
    {
        World w = Generate(gen);
        int const level = w.ask.level;
        for (QuestRecord& quest : w.index.quests)
        {
            for (int g = gen.Int(0, 2); g > 0; --g)
            {
                QuestPlace place;
                place.mapId = gen.Chance(0.8f) ? MAP_A : MAP_B;
                place.x = Mid(gen.Int(CELL_LOW, CELL_HIGH));
                place.y = Mid(gen.Int(CELL_LOW, CELL_HIGH));
                place.entry = gen.Pick(FIRST_ENDER, ENDERS);
                place.hostileTo = gen.Chance(0.1f) ? static_cast<std::uint8_t>(1u << w.team) : std::uint8_t{0};
                place.otherPhase = gen.Chance(0.05f);
                quest.givers.push_back(static_cast<std::uint32_t>(w.index.places.size()));
                w.index.places.push_back(place);
            }
            quest.minLevel = static_cast<std::uint32_t>(std::max(1, level - gen.Int(-1, 6)));
            quest.level = gen.Chance(0.1f) ? -1 : std::max(1, level + gen.Int(-6, 4));
            if (gen.Chance(0.1f))
                quest.flags |= QuestFlag::ELITE;
            if (gen.Chance(0.05f))
                quest.flags |= QuestFlag::REPEATABLE;
            if (gen.Chance(0.2f))
                quest.exclusiveGroup = gen.Int(1, 3);
            if (gen.Chance(0.2f))
                quest.prev = (gen.Chance(0.5f) ? 1 : -1) * static_cast<std::int32_t>(FIRST_QUEST + 3 * gen.Int(0, 7));
            for (QuestObjective& objective : quest.objectives)
                for (int a = gen.Chance(0.5f) ? gen.Int(1, 2) : 0; a > 0; --a)
                {
                    AreaPoint area;
                    area.mapId = gen.Chance(0.8f) ? MAP_A : MAP_B;
                    area.x =
                        (static_cast<float>(gen.Int(CELL_LOW, CELL_HIGH)) + gen.Real(0.01f, 0.99f)) * FIELD_CELL_YARDS;
                    area.y =
                        (static_cast<float>(gen.Int(CELL_LOW, CELL_HIGH)) + gen.Real(0.01f, 0.99f)) * FIELD_CELL_YARDS;
                    area.radius = gen.Real(5.0f, 60.0f);
                    area.points = 1;
                    area.notTeams = gen.Chance(0.1f) ? static_cast<std::uint8_t>(1u << w.team) : std::uint8_t{0};
                    objective.areas.push_back(area);
                }
        }
        std::vector<std::uint32_t> inLog;
        for (QuestSlot const& slot : w.log)
            inLog.push_back(slot.quest);
        std::sort(inLog.begin(), inLog.end());
        inLog.erase(std::unique(inLog.begin(), inLog.end()), inLog.end());
        std::vector<std::uint32_t> rewarded;
        std::vector<std::uint32_t> givenUp;
        std::vector<std::uint32_t> candidates;
        for (QuestRecord const& quest : w.index.quests)
        {
            candidates.push_back(quest.id);
            if (std::binary_search(inLog.begin(), inLog.end(), quest.id))
                continue;
            if (gen.Chance(0.2f))
                rewarded.push_back(quest.id);
            else if (gen.Chance(0.1f))
                givenUp.push_back(quest.id);
        }
        candidates.push_back(UNKNOWN_QUEST);
        std::vector<std::uint32_t> const answer =
            TakeFixture::Answer(w.index, w.team ? ORC : HUMAN, 1, w.ask.level, rewarded, inLog);
        Take::Facts facts;
        facts.race = w.team ? ORC : HUMAN;
        facts.level = w.ask.level;
        facts.eliteGroup = gen.Chance(0.5f);
        facts.rewarded = rewarded;
        facts.inLog = inLog;
        facts.givenUp = givenUp;
        facts.takeable = answer;
        Take::Facts strict = facts;
        strict.eliteGroup = false;
        std::vector<Need> const takes = TakeNeeds(w.index, facts, w.ask.mapId, candidates, pricing);
        takeCount += takes.size();

        for (QuestRecord const& quest : w.index.quests)
        {
            std::size_t mine = 0;
            for (Need const& take : takes)
                mine += take.quest == quest.id ? 1 : 0;
            bool giver = false;
            for (std::uint32_t const at : quest.givers)
            {
                QuestPlace const& place = w.index.places[at];
                giver =
                    giver || (place.mapId == w.ask.mapId && !place.otherPhase && !(place.hostileTo & (1u << w.team)));
            }
            std::optional<Take::Reason> const why = Take::May(quest, strict);
            refusedCount += why ? 1 : 0;
            takeRule.Note(why ? mine == 0 : mine == (giver ? 1u : 0u), logIndex, "quest " + std::to_string(quest.id));
            if (std::binary_search(inLog.begin(), inLog.end(), quest.id))
                takeLog.Note(mine == 0, logIndex, "quest " + std::to_string(quest.id));

            if (RefusedFor(quest, w.team) || Listed(quest))
                continue;
            float const worth = QuestWorth(quest, pricing);
            QuestShares const shares = SharesOf(quest, pricing);
            std::array<QuestSlot, 1> const open = {QuestSlot{quest.id, 0, {}}};
            std::array<QuestSlot, 1> const ready = {QuestSlot{quest.id, Circuit::SLOT_COMPLETE, {}}};
            std::vector<Need> const credits = Needs(w.index, open, w.team, pricing, PARTY);
            std::vector<Need> const handIns = Needs(w.index, ready, w.team, pricing, PARTY);
            std::optional<Need> const take = TakeNeed(w.index, quest, w.team, pricing);
            double sum = take ? take->worthPerCredit : shares.take;
            float most = 0.0f;
            for (Need const& need : credits)
            {
                sum += static_cast<double>(need.left) * need.worthPerCredit;
                most = std::max(most, need.worthPerCredit);
            }
            sum += handIns.size() == 1 ? handIns[0].worthPerCredit : 0.0f;
            sums.Note(handIns.size() == 1 &&
                          std::fabs(sum - worth) <= 1e-3 * std::max(1.0, std::fabs(static_cast<double>(worth))),
                      logIndex, "quest " + std::to_string(quest.id));
            if (worth > 0.0f && !credits.empty() && handIns.size() == 1)
                handIn.Note(handIns[0].worthPerCredit > most, logIndex, "quest " + std::to_string(quest.id));
        }

        std::vector<Need> needs = Needs(w.index, w.log, w.team, pricing, PARTY);
        needs.insert(needs.end(), takes.begin(), takes.end());
        Ranking const ranking = TopFields(w.map, needs, w.ask);
        for (std::uint32_t i = 0; i < needs.size(); ++i)
        {
            if (needs[i].places.empty())
                continue;
            placedCount += UsesPlaces(w.map, needs[i]) ? 1 : 0;
            bool ok = !MapGap(w.map, needs[i], {});
            for (Missing const& m : ranking.noField)
                ok = ok && (m.need != i ||
                            (m.why != NoField::NoSource && m.why != NoField::NoSpawn && m.why != NoField::Fieldless));
            noGap.Note(ok, logIndex, "need " + std::to_string(i) + " of quest " + std::to_string(needs[i].quest));
        }

        // Ordinary hostiles: new creatures, not elite, at levels up to the one the field is judged by (or the bot's),
        // in the field's nine cells
        for (std::size_t f = 0; f < ranking.fields.size() && f < 3; ++f)
        {
            Ranked const& field = ranking.fields[f];
            std::uint64_t const centre = field.offer.centre;
            int const top = std::clamp(
                static_cast<int>(std::floor(static_cast<float>(level) + field.offer.risk.levelsAbove)), 1, 250);
            std::vector<FieldSpawn> more = w.spawns;
            for (int k = gen.Int(1, 12); k > 0; --k)
            {
                float const x = (static_cast<float>(FieldCellXOf(centre) + gen.Int(-1, 1)) + gen.Real(0.01f, 0.99f)) *
                                FIELD_CELL_YARDS;
                float const y = (static_cast<float>(FieldCellYOf(centre) + gen.Int(-1, 1)) + gen.Real(0.01f, 0.99f)) *
                                FIELD_CELL_YARDS;
                std::uint8_t const low = static_cast<std::uint8_t>(gen.Int(1, top));
                more.push_back(MakeSpawn(FieldKind::Creature, FIRST_ORDINARY + static_cast<std::uint32_t>(k),
                                         FieldMapOf(centre), x, y, low, static_cast<std::uint8_t>(low + gen.Int(0, 3)),
                                         true, static_cast<std::uint32_t>(gen.Int(30, 900))));
            }
            FieldMap const bigger = FieldMap::Build(more);
            Offer const after = FieldOffer(bigger, needs, centre, w.ask);
            float const was = Weighted(field.offer.risk, w.ask);
            float const now = Weighted(after.risk, w.ask);
            ordinary.Note(now <= was + SLACK && now <= Goals::RISK_REFUSE &&
                              FindField(TopFields(bigger, needs, w.ask), centre) != nullptr,
                          logIndex, "field " + std::to_string(centre));
            ++grownCount;
        }
    }
    std::printf("steps seed=%u logs=%zu takes=%zu refused takes=%zu needs at places=%zu grown fields=%zu\n", seed, LOGS,
                takeCount, refusedCount, placedCount, grownCount);
    for (Invariant const* inv : {&sums, &handIn, &takeLog, &takeRule, &noGap, &ordinary})
    {
        std::printf("  %s: %u checks\n", inv->name, inv->checked);
        std::string what = std::string(inv->name) + ": " + std::to_string(inv->checked) + " checks";
        if (inv->broken)
            what += ", " + std::to_string(inv->broken) + " broken, first at log " + std::to_string(inv->firstLog) +
                    " of seed " + std::to_string(seed) + " (" + inv->firstWhat + ")";
        Check(inv->checked > 0 && !inv->broken, what);
    }
}
}  // namespace

int main(int argc, char** argv)
{
    std::uint32_t seed = DEFAULT_SEED;
    if (argc > 1)
        seed = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    MomentTen();
    TwoSources();
    TooHigh();
    Colours();
    Kinds();
    GeneratedLogs(seed);
    Steps();
    LoneRare();
    PlacesAndTakes();
    ListedAndAlone();
    GeneratedSteps(seed);
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
