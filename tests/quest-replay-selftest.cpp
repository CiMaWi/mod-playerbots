/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The replay of questing WQ16: where would the field ranking (QuestFields.h: FieldOffer, TopFields) have sent the user
// of the Westfall fixture (tests/fixtures/quest-replay/westfall-3301, WQ15)? For every moment of the fixture it builds
// the needs from the log as it stood, ranks the fields around the user's position and prints the top five, the rank
// and worth of the field the user worked next, and the sweeps of WQ.37 (a field of one cell or nine, stays of 2, 5 and
// 10 minutes, the crowd of other characters on or off). Prints only: no share is a pass mark (WQ.34). It fails on an
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

#include "QuestFields.h"

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

struct Fixture
{
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
        if (r.size() > 9 && r[1] == "Kill")
            f.killEvents.push_back({Int(r, 0), Int(r, 8), Int(r, 9)});
    }
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
    std::map<NoField, int> gaps;
    std::vector<std::uint8_t> gapKinds;
};

float CrowdFactor(std::uint32_t others)
{
    return others == 0 ? 1.0f : (others < CROWDED_OTHERS ? KILLS_FEW / KILLS_ALONE : KILLS_MANY / KILLS_ALONE);
}

int Mod(int a, int b) { return ((a % b) + b) % b; }

Ranked2 Rank(FieldMap const& nineMap, FieldMap const& oneMap, Fixture const& f, Moment const& m,
             std::vector<Need> const& needs, Config const& cfg)
{
    bool const spread = cfg.cells == 1;
    FieldMap const& map = spread ? oneMap : nineMap;
    float const scale = spread ? static_cast<float>(SPREAD) : 1.0f;
    FieldAsk ask;
    ask.mapId = MAP_ID;
    ask.x = m.x * scale - (spread ? SPREAD_SHIFT : 0.0f);
    ask.y = m.y * scale - (spread ? SPREAD_SHIFT : 0.0f);
    ask.radius = WALK_RADIUS * scale;
    ask.runSpeed = Circuit::DEFAULT_RUN_SPEED * scale;
    ask.level = static_cast<std::uint8_t>(m.level);
    ask.stayMinutes = cfg.stay;
    Ranking const ranking = TopFields(map, needs, ask);
    Ranked2 out;
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
    Walk,
    Unranked,
    Other,
    Count
};
constexpr char const* MISS_NAMES[] = {"hand-in round",
                                      "crowded field",
                                      "target outside the radius",
                                      "walk the straight line misjudges",
                                      "no worth at the user's field (other)",
                                      "other"};
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
        std::printf("  user: next stay %d at cell (%d,%d): rank %s of %zu, %.2f credits/min, %d kills on the way",
                    m.stay, m.cellI, m.cellJ, baseRank ? std::to_string(baseRank).c_str() : "not listed",
                    base.fields.size(), userWorth, m.kills);
        if (walk != f.walkTo.end())
            std::printf(", walked %.0f yd (straight %.0f yd%s)", walk->second.yards, walk->second.straight,
                        walk->second.taxiMin > 0 ? ", with a flight" : "");
        std::printf("\n  ranks (cell/stay/crowd = rank/listed):%s\n", ranks.c_str());
        // the walk with the user's real path, in the default setting
        int actualRank = baseRank;
        if (baseRank && walk != f.walkTo.end())
        {
            Field user = base.fields[static_cast<std::size_t>(baseRank - 1)];
            float const actualMinutes =
                static_cast<float>(walk->second.taxiMin > 0
                                       ? walk->second.taxiMin + walk->second.straight / (7.0 * 60.0)
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

    if (processed != static_cast<int>(f.moments.size()))
        Fail("a moment was not processed");
    std::printf("replayed %d moments\n", processed);
    return failed ? 1 : 0;
}
