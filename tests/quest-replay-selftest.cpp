/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The replay of questing WQ16: where would the field ranking (QuestFields.h: FieldOffer, TopFields) have sent the user
// of the Westfall fixture (tests/fixtures/quest-replay/westfall-3301, WQ15)? For every moment of the fixture it builds
// the needs from the log as it stood, ranks the fields around the user's position and prints the top five, the rank
// and worth of the field the user worked next, and the sweeps of WQ.37 (a field of one cell or nine, stays of 2, 5 and
// 10 minutes, the crowd of other characters on or off). WQ17d appends the replay by route (RouteReplay: Route::Build at
// every moment, beside where the user went next). Prints only: no share is a pass mark (WQ.34, WQ.42). It fails on an
// unreadable fixture, a moment not processed or an internal sanity check. Nothing here reads the census.
// Notes: the fixture holds the log's open needs (kind, entry, count left) but not the quest index, so the needs are
// built here by the rules of Needs (kill: the creature; item: the corpses above SOURCE_CHANCE_FLOOR, else the
// objects) from sources.tsv, with one worth per credit (the unit is credits per minute). A field of one cell is
// the same field map with every cell moved apart (x3) so that no neighbour is left.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel tests/quest-replay-selftest.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "QuestRoute.h"

using namespace Planner;
using namespace Planner::QuestFields;

namespace
{
using Row = std::vector<std::string>;

constexpr std::uint32_t MAP_ID = 0;
constexpr int SPREAD = 3;               // the one-cell field's map: cells x3 apart
constexpr float SPREAD_SHIFT = 100.0f;  // 3 x centre - shift = the spread centre
constexpr float WALK_RADIUS = FIELD_WALK_YARDS;
constexpr std::size_t SHOWN = 5;
constexpr std::int64_t HAND_IN_ROUND_MIN = 3;  // quests handed in between a moment and the next work stay
constexpr std::uint32_t CROWDED_OTHERS = 3;    // the research's 3+ others
constexpr float KILLS_ALONE = 2.17f;           // WQ35 research: kills per minute alone, with 1-2 others, 3+
constexpr float KILLS_FEW = 1.45f;
constexpr float KILLS_MANY = 0.43f;
constexpr std::uint32_t HOSTILE_ENTRY = 0xF0000000u;

int failed = 0;

void Fail(std::string const& what)
{
    ++failed;
    std::printf("FAIL: %s\n", what.c_str());
}

std::vector<Row> ReadTsv(std::string const& path)
{
    std::vector<Row> rows;
    std::ifstream in(path);
    if (!in)
    {
        Fail("cannot read " + path);
        return rows;
    }
    std::string line;
    bool header = true;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (header)
        {
            header = false;
            continue;
        }
        Row row;
        std::size_t from = 0;
        while (true)
        {
            std::size_t const at = line.find('\t', from);
            row.push_back(line.substr(from, at == std::string::npos ? std::string::npos : at - from));
            if (at == std::string::npos)
                break;
            from = at + 1;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

double Num(Row const& row, std::size_t at) { return at < row.size() ? std::atof(row[at].c_str()) : 0.0; }
std::int64_t Int(Row const& row, std::size_t at) { return static_cast<std::int64_t>(Num(row, at)); }

struct Moment
{
    int n{0};
    std::string kind;
    std::int64_t t{0};
    double minute{0};
    float x{0}, y{0};
    int level{0};
    int quests{0};
    int stay{0};
    int cellI{0}, cellJ{0};
    int kills{0};
};

struct Stay
{
    std::int64_t arr{0}, dep{0};
    int cellI{0}, cellJ{0};
    bool work{false};
};

struct Walk
{
    double yards{0}, straight{0}, taxiMin{0};
};

struct SourceRow
{
    char kind;
    std::uint32_t entry;
    std::uint32_t item;
    float chance;
};

struct QuestEvent
{
    std::int64_t t{0};
    std::string type;  // QuestAccept, QuestDone, QuestAbandon
    std::uint32_t quest{0};
    float x{0}, y{0};
    std::string name;
};

struct Fixture
{
    std::vector<QuestEvent> questEvents;
    std::vector<Row> questRows;
    std::vector<Moment> moments;
    std::map<int, std::vector<Row>> needs;
    std::map<int, std::map<std::pair<int, int>, std::pair<int, int>>> crowd;  // moment -> cell -> (kills, bots)
    std::map<int, Stay> stays;
    std::map<int, Walk> walkTo;
    std::vector<std::int64_t> handIns;
    std::vector<std::vector<std::int64_t>> killEvents;  // t, x, y
    std::vector<SourceRow> sources;
    std::vector<FieldSpawn> nine;
    std::vector<FieldSpawn> one;
    std::size_t fieldRows{0};
};

void AddSpawns(Fixture& f, Row const& r)
{
    // The aggregated row of fields.tsv back into spawns: the same count, weight, respawn, elites and levels
    int const cellX = static_cast<int>(Int(r, 1));
    int const cellY = static_cast<int>(Int(r, 2));
    std::uint32_t const spawns = static_cast<std::uint32_t>(Int(r, 8));
    for (std::uint32_t i = 0; i < spawns; ++i)
    {
        FieldSpawn s;
        s.kind = r[3] == "o" ? FieldKind::Object : FieldKind::Creature;
        s.entry = static_cast<std::uint32_t>(Int(r, 4));
        s.mapId = static_cast<std::uint32_t>(Int(r, 0));
        s.x = static_cast<float>(Num(r, 14));
        s.y = static_cast<float>(Num(r, 15));
        s.z = static_cast<float>(Num(r, 16));
        s.minLevel = static_cast<std::uint8_t>(Int(r, 10));
        s.maxLevel = static_cast<std::uint8_t>(Int(r, 11));
        s.elite = i < Int(r, 12);
        s.respawnSecs = static_cast<std::uint32_t>(Int(r, 13));
        s.share = spawns ? static_cast<float>(Num(r, 9) / spawns) : 0.0f;
        f.nine.push_back(s);
        s.x += static_cast<float>(cellX) * (SPREAD - 1) * FIELD_CELL_YARDS;
        s.y += static_cast<float>(cellY) * (SPREAD - 1) * FIELD_CELL_YARDS;
        f.one.push_back(s);
    }
}

void AddHostiles(Fixture& f, Row const& r, std::uint32_t serial)
{
    // The cell's hostile summary (count, weight, lowest and highest level, elites) as spawns of one made-up entry
    std::uint32_t const count = static_cast<std::uint32_t>(Int(r, 20));
    int const cellX = static_cast<int>(Int(r, 1));
    int const cellY = static_cast<int>(Int(r, 2));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        FieldSpawn s;
        s.entry = HOSTILE_ENTRY + serial;
        s.mapId = static_cast<std::uint32_t>(Int(r, 0));
        s.x = (static_cast<float>(cellX) + 0.5f) * FIELD_CELL_YARDS;
        s.y = (static_cast<float>(cellY) + 0.5f) * FIELD_CELL_YARDS;
        s.hostile = true;
        s.minLevel = static_cast<std::uint8_t>(i == 0 || count == 1 ? Int(r, 22) : Int(r, 23));
        s.maxLevel = static_cast<std::uint8_t>(i + 1 == count ? Int(r, 23) : Int(r, 22));
        if (count > 1 && i + 1 < count && i > 0)
            s.minLevel = static_cast<std::uint8_t>(Int(r, 22));
        s.elite = i < Int(r, 24);
        s.share = static_cast<float>(Num(r, 21) / count);
        f.nine.push_back(s);
        s.x += static_cast<float>(cellX) * (SPREAD - 1) * FIELD_CELL_YARDS;
        s.y += static_cast<float>(cellY) * (SPREAD - 1) * FIELD_CELL_YARDS;
        f.one.push_back(s);
    }
}

bool Load(std::string const& dir, Fixture& f)
{
    for (Row const& r : ReadTsv(dir + "/moments.tsv"))
    {
        Moment m;
        m.n = static_cast<int>(Int(r, 0));
        m.kind = r[1];
        m.t = Int(r, 2);
        m.minute = Num(r, 3);
        m.x = static_cast<float>(Num(r, 5));
        m.y = static_cast<float>(Num(r, 6));
        m.level = static_cast<int>(Int(r, 7));
        m.quests = static_cast<int>(Int(r, 8));
        m.stay = static_cast<int>(Int(r, 9));
        m.cellI = static_cast<int>(Int(r, 10));
        m.cellJ = static_cast<int>(Int(r, 11));
        m.kills = static_cast<int>(Int(r, 14));
        f.moments.push_back(m);
    }
    for (Row const& r : ReadTsv(dir + "/needs.tsv"))
        f.needs[static_cast<int>(Int(r, 0))].push_back(r);
    for (Row const& r : ReadTsv(dir + "/crowd.tsv"))
        f.crowd[static_cast<int>(Int(r, 0))][{static_cast<int>(Int(r, 1)), static_cast<int>(Int(r, 2))}] = {
            static_cast<int>(Int(r, 3)), static_cast<int>(Int(r, 4))};
    for (Row const& r : ReadTsv(dir + "/stays.tsv"))
    {
        Stay s;
        s.arr = Int(r, 1);
        s.dep = Int(r, 2);
        s.cellI = static_cast<int>(Int(r, 7));
        s.cellJ = static_cast<int>(Int(r, 8));
        s.work = Int(r, 12) != 0;
        f.stays[static_cast<int>(Int(r, 0))] = s;
    }
    for (Row const& r : ReadTsv(dir + "/walks.tsv"))
        f.walkTo[static_cast<int>(Int(r, 1))] = {Num(r, 5), Num(r, 6), Num(r, 8)};
    for (Row const& r : ReadTsv(dir + "/events.tsv"))
    {
        if (r.size() > 1 && r[1] == "QuestDone")
            f.handIns.push_back(Int(r, 0));
        if (r.size() > 10 && (r[1] == "QuestAccept" || r[1] == "QuestDone" || r[1] == "QuestAbandon"))
            f.questEvents.push_back({Int(r, 0), r[1], static_cast<std::uint32_t>(Int(r, 3)),
                                     static_cast<float>(Num(r, 8)), static_cast<float>(Num(r, 9)), r[10]});
        if (r.size() > 9 && r[1] == "Kill")
            f.killEvents.push_back({Int(r, 0), Int(r, 8), Int(r, 9)});
    }
    f.questRows = ReadTsv(dir + "/quests.tsv");
    for (Row const& r : ReadTsv(dir + "/sources.tsv"))
        f.sources.push_back({r[0][0], static_cast<std::uint32_t>(Int(r, 1)), static_cast<std::uint32_t>(Int(r, 2)),
                             static_cast<float>(Num(r, 3) / 100.0)});
    std::set<std::tuple<int, int, int>> hostileDone;
    std::uint32_t serial = 0;
    for (Row const& r : ReadTsv(dir + "/fields.tsv"))
    {
        ++f.fieldRows;
        AddSpawns(f, r);
        if (Int(r, 20) &&
            hostileDone.insert({static_cast<int>(Int(r, 0)), static_cast<int>(Int(r, 1)), static_cast<int>(Int(r, 2))})
                .second)
            AddHostiles(f, r, serial++);
    }
    return !f.moments.empty() && f.fieldRows != 0;
}

// The needs of a moment as Needs would make them from the log: a kill objective's creature, an item's corpses above
// the floor (the skins are not in the fixture), else its objects. Worth 1 a credit, so a worth is credits
std::vector<Need> NeedsOf(Fixture const& f, int moment, std::vector<std::string>& labels)
{
    std::vector<Need> needs;
    auto const found = f.needs.find(moment);
    if (found == f.needs.end())
        return needs;
    for (Row const& r : found->second)
    {
        Need need;
        need.quest = static_cast<std::uint32_t>(Int(r, 4));
        need.objective = 0;
        need.left = static_cast<std::uint32_t>(Int(r, 3));
        need.worthPerCredit = 1.0f;
        std::uint32_t const entry = static_cast<std::uint32_t>(Int(r, 2));
        if (r[1] == "npc")
        {
            need.kind = NeedKind::Kill;
            need.seconds = Circuit::KILL_SECONDS;
            need.sources.push_back({FieldKind::Creature, entry, 1.0f});
        }
        else
        {
            need.seconds = Circuit::ITEM_SECONDS;
            bool corpses = false;
            for (SourceRow const& s : f.sources)
                corpses |= s.kind == 'c' && s.item == entry && s.chance >= SOURCE_CHANCE_FLOOR;
            need.kind = corpses ? NeedKind::Loot : NeedKind::Chest;
            for (SourceRow const& s : f.sources)
                if (s.item == entry && s.kind == (corpses ? 'c' : 'o') && s.chance >= SOURCE_CHANCE_FLOOR)
                    QuestFields::Detail::AddSource(need.sources, corpses ? FieldKind::Creature : FieldKind::Object,
                                                   s.entry, std::min(s.chance, 1.0f));
        }
        labels.push_back(r[5] == "-" ? std::string(r[1]) + " " + r[2] : r[5]);
        needs.push_back(std::move(need));
    }
    return needs;
}

struct Config
{
    int cells;   // 1 or 9
    float stay;  // minutes
    bool crowd;
};

struct Field
{
    int cellI{0}, cellJ{0};
    float value{0};
    float perMinute{0};  // worth over the stay, before walk and risk
    float walkMinutes{0};
    float risk{1};
    float factor{1};
    std::uint32_t others{0};
    float worth{0};
    float walkYards{0};
    std::size_t needCount{0};
};

struct Ranked2
{
    std::vector<Field> fields;
    std::uint32_t refused{0};  // centres TopFields refused by risk
    std::map<NoField, int> gaps;
    std::vector<std::uint8_t> gapKinds;
};

float CrowdFactor(std::uint32_t others)
{
    return others == 0 ? 1.0f : (others < CROWDED_OTHERS ? KILLS_FEW / KILLS_ALONE : KILLS_MANY / KILLS_ALONE);
}

int Mod(int a, int b) { return ((a % b) + b) % b; }

FieldAsk AskOf(Moment const& m, Config const& cfg)
{
    bool const spread = cfg.cells == 1;
    float const scale = spread ? static_cast<float>(SPREAD) : 1.0f;
    FieldAsk ask;
    ask.mapId = MAP_ID;
    ask.x = m.x * scale - (spread ? SPREAD_SHIFT : 0.0f);
    ask.y = m.y * scale - (spread ? SPREAD_SHIFT : 0.0f);
    ask.radius = WALK_RADIUS * scale;
    ask.runSpeed = Circuit::DEFAULT_RUN_SPEED * scale;
    ask.level = static_cast<std::uint8_t>(m.level);
    ask.stayMinutes = cfg.stay;
    return ask;
}

// Whether TopFields refuses the nine-cell field at the user's next cell by its risk (the same test as TopFields)
bool RefusedAtUser(FieldMap const& nineMap, Moment const& m, std::vector<Need> const& needs, Config const& cfg)
{
    FieldAsk const ask = AskOf(m, cfg);
    Offer const offer = FieldOffer(nineMap, needs, FieldCellKey(MAP_ID, m.cellI, m.cellJ), ask);
    return offer.worth > 0.0f && Goals::RawRisk(offer.risk, ask.groupSize) * ask.caution > Goals::RISK_REFUSE;
}

Ranked2 Rank(FieldMap const& nineMap, FieldMap const& oneMap, Fixture const& f, Moment const& m,
             std::vector<Need> const& needs, Config const& cfg)
{
    bool const spread = cfg.cells == 1;
    FieldMap const& map = spread ? oneMap : nineMap;
    Ranking const ranking = TopFields(map, needs, AskOf(m, cfg));
    Ranked2 out;
    out.refused = ranking.refused;
    auto const crowdIt = f.crowd.find(m.n);
    for (Ranked const& r : ranking.fields)
    {
        int const cx = FieldCellXOf(r.offer.centre);
        int const cy = FieldCellYOf(r.offer.centre);
        if (spread && (Mod(cx, SPREAD) || Mod(cy, SPREAD)))
            continue;  // a centre beside a cell: the one-cell field has no such field
        Field field;
        field.cellI = spread ? cx / SPREAD : cx;
        field.cellJ = spread ? cy / SPREAD : cy;
        int const reach = spread ? 0 : 1;
        if (crowdIt != f.crowd.end())
            for (int dx = -reach; dx <= reach; ++dx)
                for (int dy = -reach; dy <= reach; ++dy)
                {
                    auto const cell = crowdIt->second.find({field.cellI + dx, field.cellJ + dy});
                    if (cell != crowdIt->second.end())
                        field.others =
                            std::max<std::uint32_t>(field.others, static_cast<std::uint32_t>(cell->second.second));
                }
        field.factor = CrowdFactor(field.others);
        field.value = r.value * (cfg.crowd ? field.factor : 1.0f);
        field.worth = r.offer.worth;
        field.perMinute = r.offer.worth / cfg.stay;
        field.walkMinutes = r.walkMinutes;
        field.risk = r.riskFactor;
        field.walkYards = r.walkMinutes * Circuit::DEFAULT_RUN_SPEED * SECONDS_PER_MINUTE;
        for (NeedOffer const& one : r.offer.needs)
            field.needCount += one.credits > 0.0f ? 1 : 0;
        out.fields.push_back(field);
    }
    std::sort(out.fields.begin(), out.fields.end(),
              [](Field const& a, Field const& b)
              {
                  if (a.value != b.value)
                      return a.value > b.value;
                  return std::make_pair(a.cellI, a.cellJ) < std::make_pair(b.cellI, b.cellJ);
              });
    for (Missing const& g : ranking.noField)
        ++out.gaps[g.why];
    return out;
}

int RankOf(Ranked2 const& r, int cellI, int cellJ)
{
    for (std::size_t i = 0; i < r.fields.size(); ++i)
        if (r.fields[i].cellI == cellI && r.fields[i].cellJ == cellJ)
            return static_cast<int>(i) + 1;
    return 0;
}

// Whether a field within `top` ranks lies at or beside the cell (the walk would pass the user's work)
bool NearTop(Ranked2 const& r, int cellI, int cellJ, std::size_t top)
{
    for (std::size_t i = 0; i < std::min(top, r.fields.size()); ++i)
        if (std::abs(r.fields[i].cellI - cellI) <= 1 && std::abs(r.fields[i].cellJ - cellJ) <= 1)
            return true;
    return false;
}

constexpr Config SWEEP[] = {{9, 5.0f, false},  {9, 5.0f, true}, {1, 5.0f, false}, {1, 5.0f, true},  {9, 2.0f, false},
                            {9, 10.0f, false}, {9, 2.0f, true}, {9, 10.0f, true}, {1, 2.0f, false}, {1, 10.0f, false}};
constexpr std::size_t CONFIGS = sizeof(SWEEP) / sizeof(SWEEP[0]);

std::string Name(Config const& c)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%dc/%.0fm/%s", c.cells, static_cast<double>(c.stay), c.crowd ? "crowd" : "plain");
    return buf;
}

enum class Miss
{
    HandIn,
    Crowded,
    OutsideRadius,
    Refused,
    Walk,
    Unranked,
    Other,
    Count
};
constexpr char const* MISS_NAMES[] = {"hand-in round",
                                      "crowded field",
                                      "target outside the radius",
                                      "the user's field refused by risk",
                                      "walk the straight line misjudges",
                                      "no worth at the user's field (other)",
                                      "other"};

// WQ17d: the replay by route. The fixture holds no quest index, so one is made here from what it holds: the quests of
// quests.tsv with their objectives (kill: the creature; item: the sources of sources.tsv), a giver at each QuestAccept
// event's position and an ender at each QuestDone event's (the user stood at the NPC), with a made-up friendly NPC
// spawn there so the field map finds its cell. At a moment the log is the quests accepted and neither handed in nor
// abandoned before it; one with no open row in needs.tsv is ready (its hand-in is a need); the quests the user took
// from the moment to the end of the next stay are the takes on offer. A quest's worth is 2.5 times its credits, so an
// objective credit is worth 1 as in WQ16 (the planner's split by step: a take 10 %, the credits 40 %, the hand-in 50
// %). Far stops are priced by a made-up walk (straight yards x ROUTE_DETOUR at run speed) that finds no way to one
// place
constexpr float ROUTE_DETOUR = 1.3f;
constexpr std::uint32_t NPC_ENTRY = 0xE0000000u;  // the made-up givers and enders: this plus the place's index
constexpr std::uint8_t NPC_LEVEL = 10;
constexpr float REFUSED_X = -9547.0f;  // the made-up walk finds no way within REFUSED_YARDS of here: the farm east
constexpr float REFUSED_Y = -1403.0f;  // of Elwynn where the user handed in two quests (a named no_route)
constexpr float REFUSED_YARDS = 150.0f;
constexpr char const* REFUSED_NAME = "the farm east of Elwynn (-9547,-1403)";
constexpr float CHEAP_STOP_MINUTES = 1.0f;
// The moments WQ16's report named as missed by the ranking: a hand-in round, a target outside the radius, the user's
// field refused by risk (WQ17b's lone-rare rule has since lifted the last two refusals)
constexpr std::pair<int, char const*> WQ16_MISSES[] = {
    {18, "hand-in round"},      {19, "hand-in round"},      {21, "hand-in round"},
    {1, "outside the radius"},  {23, "outside the radius"}, {24, "outside the radius"},
    {28, "outside the radius"}, {22, "refused by risk"},    {29, "refused by risk"}};

struct RouteWorld
{
    QuestIndex index;
    FieldMap map;
    std::map<std::uint32_t, std::string> names;
    Pricing pricing;
};

RouteWorld MakeRouteWorld(Fixture const& f)
{
    RouteWorld rw;
    SourceGather gather;
    std::set<std::pair<SourceKind, std::uint32_t>> owners;
    for (SourceRow const& s : f.sources)
    {
        if (s.kind != 'c' && s.kind != 'o')
            continue;
        SourceKind const kind = s.kind == 'o' ? SourceKind::Object : SourceKind::Corpse;
        LootRow row;
        row.lootId = s.entry;
        row.item = s.item;
        row.chance = s.chance * 100.0f;
        gather.tables[static_cast<std::size_t>(kind)].push_back(row);
        if (owners.insert({kind, s.entry}).second)
        {
            gather.owners.push_back({kind, s.entry, s.entry});
            SourceSpawn spawn;
            spawn.entry = s.entry;
            spawn.isObject = kind == SourceKind::Object;
            gather.spawns.push_back(spawn);
        }
    }
    rw.index.sources = SourceTable::Build(std::move(gather));

    std::map<std::uint32_t, QuestRecord> quests;
    for (Row const& r : f.questRows)
    {
        std::uint32_t const id = static_cast<std::uint32_t>(Int(r, 0));
        QuestRecord& quest = quests[id];
        quest.id = id;
        quest.level = static_cast<std::int32_t>(Int(r, 1));
        rw.names[id] = r.size() > 6 ? r[6] : "";
        QuestObjective objective;
        objective.target = static_cast<std::uint32_t>(Int(r, 4));
        objective.count = static_cast<std::uint32_t>(Int(r, 5));
        if (r[2] == "npc")
        {
            objective.kind = ObjectiveKind::Kill;
            objective.poiIndex = static_cast<std::int32_t>(Int(r, 3)) - 1;
        }
        else
        {
            objective.kind = ObjectiveKind::Item;
            objective.poiIndex = POI_FIRST_ITEM + static_cast<std::int32_t>(quest.objectives.size());
            objective.sourceKinds = SourceKindsOf(rw.index.SourcesOf(objective.target));
        }
        quest.objectives.push_back(objective);
    }
    std::vector<FieldSpawn> spawns = f.nine;
    for (QuestEvent const& e : f.questEvents)
    {
        if (e.type == "QuestAbandon")
            continue;
        QuestRecord& quest = quests[e.quest];
        quest.id = e.quest;
        rw.names.emplace(e.quest, e.name);
        QuestPlace place;
        place.mapId = MAP_ID;
        place.x = e.x;
        place.y = e.y;
        place.entry = NPC_ENTRY + static_cast<std::uint32_t>(rw.index.places.size());
        (e.type == "QuestDone" ? quest.enders : quest.givers)
            .push_back(static_cast<std::uint32_t>(rw.index.places.size()));
        rw.index.places.push_back(place);
        FieldSpawn npc;
        npc.kind = FieldKind::Creature;
        npc.entry = place.entry;
        npc.mapId = MAP_ID;
        npc.x = e.x;
        npc.y = e.y;
        npc.minLevel = NPC_LEVEL;
        npc.maxLevel = NPC_LEVEL;
        npc.hostile = false;
        npc.respawnSecs = 300;
        npc.share = 1.0f;
        spawns.push_back(npc);
    }
    for (auto& [id, quest] : quests)
    {
        quest.xpBase = std::max<std::uint32_t>(1, QuestFields::Detail::ObjectiveCredits(quest));
        rw.index.quests.push_back(quest);
    }
    rw.map = FieldMap::Build(spawns);
    rw.pricing.weights.xp = 1.0f / CREDITS_SHARE;
    return rw;
}

struct Held
{
    std::vector<std::uint32_t> log;    // ascending
    std::vector<std::uint32_t> takes;  // in the order taken
    int handIns{0};                    // quests the user handed in before the next stay
};

Held HeldAt(Fixture const& f, Moment const& m)
{
    Held held;
    std::set<std::uint32_t> in;
    for (QuestEvent const& e : f.questEvents)
        if (e.t < m.t)
        {
            if (e.type == "QuestAccept")
                in.insert(e.quest);
            else
                in.erase(e.quest);
        }
    held.log.assign(in.begin(), in.end());
    auto const next = f.stays.find(m.stay);
    std::int64_t const until = next != f.stays.end() ? next->second.dep : m.t;
    std::int64_t const arrive = next != f.stays.end() ? next->second.arr : m.t;
    for (QuestEvent const& e : f.questEvents)
    {
        if (e.t < m.t)
            continue;
        if (e.type == "QuestAccept" && e.t <= until && !in.count(e.quest) &&
            std::find(held.takes.begin(), held.takes.end(), e.quest) == held.takes.end())
            held.takes.push_back(e.quest);
        held.handIns += e.type == "QuestDone" && e.t <= arrive ? 1 : 0;
    }
    return held;
}

// The route's needs at a moment: WQ16's objective needs (each told apart by its objective in the made-up index), the
// ready quests' hand-ins and the takes
std::vector<Need> RouteNeeds(Fixture const& f, RouteWorld const& rw, Moment const& m, Held const& held, int& ready)
{
    std::vector<std::string> labels;
    std::vector<Need> needs = NeedsOf(f, m.n, labels);
    auto const rows = f.needs.find(m.n);
    std::set<std::uint32_t> open;
    for (std::size_t i = 0; i < needs.size(); ++i)
    {
        Need& need = needs[i];
        open.insert(need.quest);
        need.objective = 100 + static_cast<std::uint32_t>(i);
        QuestRecord const* quest = rw.index.Find(need.quest);
        std::uint32_t const entry = static_cast<std::uint32_t>(Int(rows->second[i], 2));
        for (std::uint32_t at = 0; quest && at < quest->objectives.size(); ++at)
            if (quest->objectives[at].target == entry)
                need.objective = at;
    }
    for (std::uint32_t const id : held.log)
    {
        if (open.count(id) || !rw.index.Find(id))
            continue;
        QuestSlot slot;
        slot.quest = id;
        slot.state = Circuit::SLOT_COMPLETE;
        for (Need& need : Needs(rw.index, std::span<QuestSlot const>(&slot, 1), 0, rw.pricing, 1))
            needs.push_back(std::move(need));
        ++ready;
    }
    for (std::uint32_t const id : held.takes)
        if (QuestRecord const* quest = rw.index.Find(id))
            if (std::optional<Need> take = TakeNeed(rw.index, *quest, 0, rw.pricing, MAP_ID))
                needs.push_back(std::move(*take));
    return needs;
}

enum class RouteEnd
{
    Empty,
    Rule,
    Horizon,
    Stops,
    NoneLeft,
    Count
};
constexpr char const* ROUTE_END_NAMES[] = {"no route", "the rule", "the horizon", "the stop count", "none left"};

RouteEnd EndOf(Route::Plan const& plan, Route::Plan const& longer, Route::Ask const& ask)
{
    if (plan.stops.empty())
        return RouteEnd::Empty;
    if (longer.stops.size() > plan.stops.size())
        return RouteEnd::Rule;
    if (plan.stops.size() >= ask.maxStops)
        return RouteEnd::Stops;
    return plan.minutes >= ask.horizonMinutes ? RouteEnd::Horizon : RouteEnd::NoneLeft;
}

// 1 or 2: the stop at or beside the user's next cell among the first two; 0: neither. exact: the very cell
int HitOf(Route::Plan const& plan, Moment const& m, bool& exact)
{
    exact = false;
    for (std::size_t k = 0; k < std::min<std::size_t>(2, plan.stops.size()); ++k)
    {
        std::uint64_t const cell = plan.stops[k].order.cell;
        int const di = FieldCellXOf(cell) - m.cellI;
        int const dj = FieldCellYOf(cell) - m.cellJ;
        if (std::abs(di) <= 1 && std::abs(dj) <= 1)
        {
            exact = !di && !dj;
            return static_cast<int>(k) + 1;
        }
    }
    return 0;
}

std::string StopText(RouteWorld const& rw, Route::Stop const& stop, float yards)
{
    QuestOrder const& order = stop.order;
    char line[256];
    std::snprintf(line, sizeof(line), "%s cell (%d,%d) %s %.0f yd, walk %.1f min, stay %.2f min, worth %.1f, %.2f/min:",
                  QuestOrderWordName(order.word), FieldCellXOf(order.cell), FieldCellYOf(order.cell),
                  yards > WALK_RADIUS ? "far" : "near", static_cast<double>(yards),
                  static_cast<double>(order.walkMinutes), static_cast<double>(order.minutes),
                  static_cast<double>(stop.worth),
                  static_cast<double>(Route::StopValue(stop.worth, order.walkMinutes, order.minutes)));
    std::string text = line;
    for (QuestOrderNeed const& need : order.expected)
    {
        auto const name = rw.names.find(need.quest);
        std::string const title = name != rw.names.end() ? name->second : std::to_string(need.quest);
        if (need.objective == HAND_IN)
            text += " hand in " + title + ";";
        else if (need.objective == TAKE)
            text += " take " + title + ";";
        else
            text += " " + title + " x" + std::to_string(need.credits) + ";";
    }
    return text;
}

struct RouteTally
{
    int stops[4]{0, 0, 0, 0};  // 0, 1, 2, 3+
    int ends[static_cast<int>(RouteEnd::Count)]{};
    int hits[3]{0, 0, 0};  // neither, stop 1, stop 2
    int exact[3]{0, 0, 0};
    int handInStops{0};
    double minutes{0};
};

void Tally(RouteTally& t, Route::Plan const& plan, RouteEnd end, int hit, bool exact)
{
    ++t.stops[std::min<std::size_t>(3, plan.stops.size())];
    ++t.ends[static_cast<int>(end)];
    ++t.hits[hit];
    t.exact[hit] += exact ? 1 : 0;
    t.minutes += static_cast<double>(plan.minutes);
    for (Route::Stop const& stop : plan.stops)
        t.handInStops += stop.order.word == QuestOrderWord::HandIn ? 1 : 0;
}

void PrintTally(char const* name, RouteTally const& t, int moments)
{
    std::printf("  %-13s stops 0/1/2/3+ %d/%d/%d/%d, minutes %.1f on average; ends by", name, t.stops[0], t.stops[1],
                t.stops[2], t.stops[3], moments ? t.minutes / moments : 0.0);
    for (int e = 0; e < static_cast<int>(RouteEnd::Count); ++e)
        std::printf(" %s %d%s", ROUTE_END_NAMES[e], t.ends[e], e + 1 < static_cast<int>(RouteEnd::Count) ? "," : "");
    std::printf("; the user's next stay at stop 1 %d (exact %d), stop 2 %d (exact %d), neither %d; hand_in stops %d\n",
                t.hits[1], t.exact[1], t.hits[2], t.exact[2], t.hits[0], t.handInStops);
}

int RouteReplay(Fixture const& f)
{
    RouteWorld const rw = MakeRouteWorld(f);
    std::printf(
        "\n== the replay by route (WQ17d): made-up index %zu quests, %zu giver and ender places; walk = straight "
        "yards x %.1f at run speed, no way to %s; budget %u calls a route\n",
        rw.index.quests.size(), rw.index.places.size(), static_cast<double>(ROUTE_DETOUR), REFUSED_NAME,
        Route::WALK_BUDGET);
    RouteTally tally[2];
    RouteTally rounds[2];
    int roundMoments = 0;
    int processed = 0;
    int cheap = 0;
    int cheapEnds = 0;
    int cheapField = 0;
    std::string cheapList;
    std::uint32_t calls = 0;
    int farStops = 0;
    int noRoute = 0;
    std::uint32_t capped = 0;
    std::string missList;
    for (Moment const& m : f.moments)
    {
        Held const held = HeldAt(f, m);
        int ready = 0;
        std::vector<Need> const needs = RouteNeeds(f, rw, m, held, ready);
        Route::Ask ask;
        ask.mapId = MAP_ID;
        ask.x = m.x;
        ask.y = m.y;
        ask.level = static_cast<std::uint8_t>(m.level);
        ask.team = 0;
        ask.stayMinutes = 5.0f;
        ask.pricing = rw.pricing;
        ask.walk = [](float fromX, float fromY, float toX, float toY) -> std::optional<float>
        {
            if (std::hypot(toX - REFUSED_X, toY - REFUSED_Y) < REFUSED_YARDS)
                return std::nullopt;
            return std::hypot(toX - fromX, toY - fromY) * ROUTE_DETOUR / Circuit::DEFAULT_RUN_SPEED /
                   SECONDS_PER_MINUTE;
        };
        Route::Plan const plan = Route::Build(rw.map, rw.index, needs, ask);
        Route::Ask alt = ask;
        alt.stopRule = Route::StopRule::HorizonOnly;
        Route::Plan const longer = Route::Build(rw.map, rw.index, needs, alt);
        bool prefix = longer.stops.size() >= plan.stops.size();
        for (std::size_t k = 0; prefix && k < plan.stops.size(); ++k)
            prefix = longer.stops[k].order.cell == plan.stops[k].order.cell;
        if (!prefix)
            Fail("the horizon-only route does not begin with the default route, moment " + std::to_string(m.n));
        if (plan.walkCalls > ask.walkBudget || longer.walkCalls > ask.walkBudget)
            Fail("walk calls beyond the budget, moment " + std::to_string(m.n));
        RouteEnd const end = EndOf(plan, longer, ask);
        bool exact = false;
        bool altExact = false;
        int const hit = HitOf(plan, m, exact);
        int const altHit = HitOf(longer, m, altExact);
        Tally(tally[0], plan, end, hit, exact);
        Tally(tally[1], longer, longer.stops.empty() ? RouteEnd::Empty : EndOf(longer, longer, alt), altHit, altExact);
        bool const round = held.handIns >= HAND_IN_ROUND_MIN;
        if (round)
        {
            ++roundMoments;
            Tally(rounds[0], plan, end, hit, exact);
            Tally(rounds[1], longer, EndOf(longer, longer, alt), altHit, altExact);
        }
        calls += plan.walkCalls;
        capped += plan.capped;
        noRoute += static_cast<int>(plan.gaps.size());

        std::printf(
            "moment %d route: log %zu quests (fixture %d), %d ready, %zu takes on offer, needs %zu; %zu stops, "
            "%.1f min, worth %.1f, %.2f/min; ends by %s; walk calls %u, no_route %zu, capped %u\n",
            m.n, held.log.size(), m.quests, ready, held.takes.size(), needs.size(), plan.stops.size(),
            static_cast<double>(plan.minutes), static_cast<double>(plan.worth),
            static_cast<double>(Route::WorthPerMinute(plan)), ROUTE_END_NAMES[static_cast<int>(end)], plan.walkCalls,
            plan.gaps.size(), plan.capped);
        float x = m.x;
        float y = m.y;
        for (std::size_t k = 0; k < longer.stops.size(); ++k)
        {
            Route::Stop const& stop = longer.stops[k];
            float const yards = std::hypot(stop.order.x - x, stop.order.y - y);
            farStops += k < plan.stops.size() && yards > WALK_RADIUS ? 1 : 0;
            std::printf("    %s%zu %s\n", k < plan.stops.size() ? "" : "(alt) ", k + 1,
                        StopText(rw, stop, yards).c_str());
            x = stop.order.x;
            y = stop.order.y;
        }
        for (Route::FarGap const& gap : plan.gaps)
        {
            std::string quests;
            for (QuestOrderNeed const& need : gap.needs)
                quests += " " + std::to_string(need.quest);
            std::printf("    no_route: cell (%d,%d), quests%s\n", FieldCellXOf(gap.cell), FieldCellYOf(gap.cell),
                        quests.c_str());
        }
        auto const walk = f.walkTo.find(m.stay);
        std::printf(
            "    user: next stay %d at (%d,%d), %d hand-ins before it%s; default: %s, horizon only: %s", m.stay,
            m.cellI, m.cellJ, held.handIns, round ? " (a hand-in round)" : "",
            hit ? ("stop " + std::to_string(hit) + (exact ? " exact" : " beside")).c_str() : "neither",
            altHit ? ("stop " + std::to_string(altHit) + (altExact ? " exact" : " beside")).c_str() : "neither");
        if (walk != f.walkTo.end())
            std::printf("; the user's walk into it %.0f yd%s = %.1f min", walk->second.yards,
                        walk->second.taxiMin > 0 ? " with a flight" : "",
                        walk->second.taxiMin > 0
                            ? walk->second.taxiMin +
                                  walk->second.straight / (static_cast<double>(Circuit::DEFAULT_RUN_SPEED) * 60.0)
                            : walk->second.yards / (static_cast<double>(Circuit::DEFAULT_RUN_SPEED) * 60.0));
        std::printf("\n");

        Route::Stop const* first = plan.stops.empty() ? nullptr : &plan.stops.front();
        if (first && first->order.word == QuestOrderWord::HandIn && first->order.minutes < CHEAP_STOP_MINUTES)
        {
            ++cheap;
            cheapEnds += plan.stops.size() == 1 ? 1 : 0;
            Route::Stop const* after = longer.stops.size() > 1 ? &longer.stops[1] : nullptr;
            cheapField += plan.stops.size() > 1 && plan.stops[1].order.word == QuestOrderWord::Field ? 1 : 0;
            char item[160];
            std::snprintf(
                item, sizeof(item), " m%d %.2f/min vs %s %.2f/min%s;", m.n,
                static_cast<double>(Route::StopValue(first->worth, first->order.walkMinutes, first->order.minutes)),
                after ? QuestOrderWordName(after->order.word) : "nothing",
                after ? static_cast<double>(
                            Route::StopValue(after->worth, after->order.walkMinutes, after->order.minutes))
                      : 0.0,
                plan.stops.size() == 1 ? " (ends)" : "");
            cheapList += item;
        }
        for (auto const& [moment, why] : WQ16_MISSES)
        {
            if (moment != m.n)
                continue;
            char item[240];
            std::snprintf(item, sizeof(item),
                          "  moment %d (%s): route %s, then %s, %zu stops (horizon only %zu); the user's next stay: "
                          "default %s, horizon only %s\n",
                          m.n, why, first ? QuestOrderWordName(first->order.word) : "none",
                          plan.stops.size() > 1 ? QuestOrderWordName(plan.stops[1].order.word) : "the end",
                          plan.stops.size(), longer.stops.size(),
                          hit ? ("stop " + std::to_string(hit)).c_str() : "neither",
                          altHit ? ("stop " + std::to_string(altHit)).c_str() : "neither");
            missList += item;
        }
        ++processed;
    }
    std::printf("\n== route shares over %d moments (default = WQ17c's rule; horizon only = no rate test)\n", processed);
    PrintTally("default", tally[0], processed);
    PrintTally("horizon only", tally[1], processed);
    std::printf("== the hand-in rounds (%d moments with %lld or more hand-ins before the next stay)\n", roundMoments,
                static_cast<long long>(HAND_IN_ROUND_MIN));
    PrintTally("default", rounds[0], roundMoments);
    PrintTally("horizon only", rounds[1], roundMoments);
    std::printf(
        "== cheap first stop (hand_in under %.0f min): %d routes, of them end there %d, go on to a field %d;"
        " stop 1 against the next candidate:%s\n",
        static_cast<double>(CHEAP_STOP_MINUTES), cheap, cheapEnds, cheapField, cheapList.c_str());
    std::printf("== WQ16's misses, what the route does now\n%s", missList.c_str());
    std::printf("== far: walk calls %u, far stops %d, no_route gaps %d, needs capped by the budget %u\n", calls,
                farStops, noRoute, capped);
    return processed;
}
}  // namespace

int main(int argc, char** argv)
{
    std::string dir = argc > 2 && std::string(argv[1]) == "--replay" ? argv[2] : (argc > 1 ? argv[1] : "");
    if (dir.empty())
    {
        std::printf("usage: quest-replay-selftest [--replay] <fixture dir>\n");
        return 2;
    }
    Fixture f;
    if (!Load(dir, f))
        Fail("fixture unreadable or empty: " + dir);
    if (failed)
        return 1;
    FieldMap const nineMap = FieldMap::Build(f.nine);
    FieldMap const oneMap = FieldMap::Build(f.one);
    std::printf("fixture %s: %zu moments, %zu field rows (%u cells, one-cell map %u)\n", dir.c_str(), f.moments.size(),
                f.fieldRows, nineMap.Stats().cells, oneMap.Stats().cells);
    if (nineMap.Stats().cells != oneMap.Stats().cells)
        Fail("the one-cell map has other cells than the nine-cell map");

    struct Tally
    {
        int exact[3]{0, 0, 0};  // top 1, 3, 5
        int beside[3]{0, 0, 0};
        int unlisted{0};
    };
    Tally tally[CONFIGS];
    int missKinds[static_cast<int>(Miss::Count)]{};
    std::string missExamples[static_cast<int>(Miss::Count)];
    std::map<NoField, int> gapTotal;
    int processed = 0;
    int walkFlip = 0;
    int walkKnown = 0;
    int refusedMoments = 0;
    int userRefused = 0;
    std::uint32_t refusedTotal = 0;
    constexpr std::size_t TOPS[3] = {1, 3, 5};

    for (Moment const& m : f.moments)
    {
        std::vector<std::string> labels;
        std::vector<Need> const needs = NeedsOf(f, m.n, labels);
        std::printf("moment %d %s minute %.1f at (%.0f,%.0f) level %d quests %d needs %zu\n", m.n, m.kind.c_str(),
                    m.minute, static_cast<double>(m.x), static_cast<double>(m.y), m.level, m.quests, needs.size());
        std::string line = "  needs:";
        for (std::size_t i = 0; i < needs.size() && i < 8; ++i)
            line += " " + labels[i] + " " + std::to_string(needs[i].left) + ";";
        std::printf("%s\n", line.c_str());
        Ranked2 base;
        int baseRank = 0;
        std::string ranks;
        for (std::size_t c = 0; c < CONFIGS; ++c)
        {
            Ranked2 const r = Rank(nineMap, oneMap, f, m, needs, SWEEP[c]);
            int const rank = RankOf(r, m.cellI, m.cellJ);
            for (int k = 0; k < 3; ++k)
            {
                tally[c].exact[k] += rank && rank <= static_cast<int>(TOPS[k]);
                tally[c].beside[k] += NearTop(r, m.cellI, m.cellJ, TOPS[k]);
            }
            tally[c].unlisted += rank == 0;
            ranks += " " + Name(SWEEP[c]) + "=" + (rank ? std::to_string(rank) : "-") + "/" +
                     std::to_string(r.fields.size());
            if (c == 0)
            {
                base = r;
                baseRank = rank;
            }
        }
        for (std::size_t i = 0; i < std::min(SHOWN, base.fields.size()); ++i)
        {
            Field const& fd = base.fields[i];
            std::printf(
                "  #%zu cell (%d,%d) %.2f credits/min over the stay, value %.3f, walk %.0f yd (%.1f min), "
                "risk factor %.2f, others %u, needs %zu\n",
                i + 1, fd.cellI, fd.cellJ, static_cast<double>(fd.perMinute), static_cast<double>(fd.value),
                static_cast<double>(fd.walkYards), static_cast<double>(fd.walkMinutes), static_cast<double>(fd.risk),
                fd.others, fd.needCount);
        }
        // sanity: the printed list is the one the rank was read from
        if (baseRank && (base.fields[static_cast<std::size_t>(baseRank - 1)].cellI != m.cellI ||
                         base.fields[static_cast<std::size_t>(baseRank - 1)].cellJ != m.cellJ))
            Fail("rank does not point at the user's cell, moment " + std::to_string(m.n));
        for (std::size_t i = 1; i < base.fields.size(); ++i)
            if (base.fields[i].value > base.fields[i - 1].value)
                Fail("list not sorted, moment " + std::to_string(m.n));
        double userWorth = 0.0;
        if (baseRank)
            userWorth = static_cast<double>(base.fields[static_cast<std::size_t>(baseRank - 1)].perMinute);
        auto const walk = f.walkTo.find(m.stay);
        bool const refusedHere = !baseRank && RefusedAtUser(nineMap, m, needs, SWEEP[0]);
        refusedMoments += base.refused != 0;
        refusedTotal += base.refused;
        userRefused += refusedHere;
        std::printf("  user: next stay %d at cell (%d,%d): rank %s of %zu, %.2f credits/min, %d kills on the way",
                    m.stay, m.cellI, m.cellJ,
                    baseRank ? std::to_string(baseRank).c_str() : (refusedHere ? "refused by risk" : "not listed"),
                    base.fields.size(), userWorth, m.kills);
        if (walk != f.walkTo.end())
            std::printf(", walked %.0f yd (straight %.0f yd%s)", walk->second.yards, walk->second.straight,
                        walk->second.taxiMin > 0 ? ", with a flight" : "");
        std::printf(", %u fields refused by risk", base.refused);
        std::printf("\n  ranks (cell/stay/crowd = rank/listed):%s\n", ranks.c_str());
        // the walk with the user's real path, in the default setting
        int actualRank = baseRank;
        if (baseRank && walk != f.walkTo.end())
        {
            Field user = base.fields[static_cast<std::size_t>(baseRank - 1)];
            float const actualMinutes = static_cast<float>(
                walk->second.taxiMin > 0
                    ? walk->second.taxiMin +
                          walk->second.straight / (static_cast<double>(Circuit::DEFAULT_RUN_SPEED) * 60.0)
                    : walk->second.yards / (static_cast<double>(Circuit::DEFAULT_RUN_SPEED) * 60.0));
            float const value = user.worth * user.risk / (actualMinutes + SWEEP[0].stay);
            actualRank = 1;
            for (Field const& other : base.fields)
                if (!(other.cellI == m.cellI && other.cellJ == m.cellJ) && other.value > value)
                    ++actualRank;
            ++walkKnown;
            walkFlip += (actualRank <= 5) != (baseRank <= 5);
            std::printf("  with the walk as the user walked it (%.1f min): rank %d\n",
                        static_cast<double>(actualMinutes), actualRank);
        }
        for (auto const& [why, count] : base.gaps)
            gapTotal[why] += count;
        if (!baseRank || baseRank > 5)
        {
            Miss kind = Miss::Other;
            Stay const& next = f.stays[m.stay];
            std::int64_t done = 0;
            for (std::int64_t const t : f.handIns)
                done += t >= m.t && t <= next.arr;
            float const dx = (static_cast<float>(m.cellI) + 0.5f) * FIELD_CELL_YARDS - m.x;
            float const dy = (static_cast<float>(m.cellJ) + 0.5f) * FIELD_CELL_YARDS - m.y;
            Ranked2 const withCrowd = Rank(nineMap, oneMap, f, m, needs, SWEEP[1]);
            int const crowdRank = RankOf(withCrowd, m.cellI, m.cellJ);
            if (done >= HAND_IN_ROUND_MIN)
                kind = Miss::HandIn;
            else if (std::sqrt(dx * dx + dy * dy) > WALK_RADIUS)
                kind = Miss::OutsideRadius;
            else if (refusedHere)
                kind = Miss::Refused;
            else if (walk != f.walkTo.end() && baseRank && actualRank && actualRank <= 5)
                kind = Miss::Walk;
            else if (!base.fields.empty() && (base.fields[0].others >= CROWDED_OTHERS || (crowdRank && crowdRank <= 5)))
                kind = Miss::Crowded;
            else if (!baseRank)
                kind = Miss::Unranked;
            ++missKinds[static_cast<int>(kind)];
            char example[160];
            std::snprintf(example, sizeof(example),
                          "%s moment %d (minute %.1f, rank %s, %lld hand-ins before the stay)",
                          missExamples[static_cast<int>(kind)].empty() ? "" : "; ", m.n, m.minute,
                          baseRank ? std::to_string(baseRank).c_str() : "-", static_cast<long long>(done));
            if (missExamples[static_cast<int>(kind)].size() < 400)
                missExamples[static_cast<int>(kind)] += example;
            std::printf("  miss kind: %s\n", MISS_NAMES[static_cast<int>(kind)]);
        }
        ++processed;
    }

    std::printf(
        "\n== shares: the user's next stay in the top 1 / 3 / 5 fields (exact cell; or the cell beside a field's "
        "centre)\n");
    for (std::size_t c = 0; c < CONFIGS; ++c)
        std::printf("  %-14s exact %d/%d/%d  within a cell %d/%d/%d  of %d, not listed %d\n", Name(SWEEP[c]).c_str(),
                    tally[c].exact[0], tally[c].exact[1], tally[c].exact[2], tally[c].beside[0], tally[c].beside[1],
                    tally[c].beside[2], processed, tally[c].unlisted);
    std::printf("== misses (default 9c/5m/plain, outside the top 5)\n");
    for (int k = 0; k < static_cast<int>(Miss::Count); ++k)
        std::printf("  %-38s %d  %s\n", MISS_NAMES[k], missKinds[k], missExamples[k].c_str());
    std::printf("== walks: moments with the user's own walk %d, of them the top 5 changes with the real walk %d\n",
                walkKnown, walkFlip);
    std::printf(
        "== risk (default): moments with a field refused %d, refusals summed %u, moments whose next field "
        "was refused %d\n",
        refusedMoments, refusedTotal, userRefused);
    std::printf("== needs no field gave a credit (default, summed over moments)\n");
    for (auto const& [why, count] : gapTotal)
        std::printf("  %-16s %d\n", NoFieldName(why), count);

    // the user's kills at each work stay: in its own cell, within the ring of nine
    int stays = 0;
    int ownCell = 0;
    int ringKills = 0;
    int allKills = 0;
    std::set<int> seen;
    for (Moment const& m : f.moments)
    {
        if (!seen.insert(m.stay).second || !f.stays[m.stay].work)
            continue;
        ++stays;
        Stay const& s = f.stays[m.stay];
        for (std::vector<std::int64_t> const& k : f.killEvents)
            if (k[0] >= s.arr && k[0] <= s.dep)
            {
                ++allKills;
                int const i = FieldCellOf(static_cast<float>(k[1]));
                int const j = FieldCellOf(static_cast<float>(k[2]));
                ownCell += i == s.cellI && j == s.cellJ;
                ringKills += std::abs(i - s.cellI) <= 1 && std::abs(j - s.cellJ) <= 1;
            }
    }
    std::printf(
        "== the user's kills during the %d work stays that follow a moment: %d, in the stay's cell %d, within "
        "its ring of nine %d\n",
        stays, allKills, ownCell, ringKills);

    if (RouteReplay(f) != static_cast<int>(f.moments.size()))
        Fail("a moment was not processed by the route");
    if (processed != static_cast<int>(f.moments.size()))
        Fail("a moment was not processed");
    std::printf("replayed %d moments\n", processed);
    return failed ? 1 : 0;
}
