/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the executor on an order (questing WQ19a, QuestFieldExec.h over QuestOrderTypes.h's QuestOrderRun):
// generated orders and logs over made-up maps (the seed is printed; argv[1] replays it, argv[2] traces that order),
// each ticked every 2 s with generated candidates and credits and checked against the step's invariants by the
// harness's own rules, plus hand-built cases shaped as the three matrix cases (field-two, field-dry, field-above).
// WQ19b: hand-ins at the order's place (an ender in the field is expected; the order ends done once they are handed
// in, no_ender after the look time without them, one failure row per expected quest left), cases shaped as handin-two
// and turnin-object.
// WQ20: on the way (Detour, OnOrder): before the arrival a field order with an allowance and a way (WayOrder) take
// what lies within the allowance on the way, generated walks from 150 to 600 yards off; cases shaped as way-kill,
// way-far and way-trip.
// WQ33: the bound on a need whose targets give the bot no stand point (StandRefused, RefuseNeed, CarryRefusals):
// a model of the look and the hold over hand-built fields, against the rules and against mutants that remove them.
// WQ38: the stall sum and its rows (QuestFailRows.h): hand cases on the rule's edges and generated timelines checked
// against the harness's own count; the abandon's done share and the two rows' amounts.
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel
//       tests/quest-field-exec-selftest.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "PursuitRules.h"
#include "QuestFailRows.h"
#include "QuestFieldExec.h"

using namespace Planner;
using namespace Planner::QuestFields;
namespace X = Planner::QuestFieldExec;

namespace
{
constexpr std::uint32_t DEFAULT_SEED = 20261125;
constexpr std::size_t ORDERS = 600;
constexpr std::size_t NO_ORDER = static_cast<std::size_t>(-1);
constexpr std::uint32_t TICK_MS = 2000;  // the executor's scan
constexpr std::uint32_t MS_PER_MINUTE = 60 * 1000;
constexpr std::uint32_t MAP_A = 0;
constexpr std::uint32_t MAP_B = 1;
constexpr std::int32_t CELL_LOW = -6;  // the made-up maps span cells -6 to 5: a cell border and negative coordinates
constexpr std::int32_t CELL_HIGH = 5;
constexpr std::uint32_t FIRST_CREATURE = 100;
constexpr std::uint32_t CREATURES = 20;
constexpr std::uint32_t UNSPAWNED = 5;  // entries past the spawned ones: a source no spawn places
constexpr std::uint32_t FIRST_OBJECT = 500;
constexpr std::uint32_t OBJECTS = 8;
constexpr std::uint32_t FIRST_QUEST = 10;
constexpr int NEVER = -1;
constexpr int REACH_LEVELS = 6;  // the harness's own reach rule at a neutral caution: 6 x 0.15 <= 1 < 7 x 0.15

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

// One invariant over every generated order: the first violation names its order so it replays alone
struct Invariant
{
    explicit Invariant(char const* text) : name(text) {}

    char const* name;
    std::uint32_t checked{0};
    std::uint32_t broken{0};
    std::size_t firstOrder{0};
    std::string firstWhat;

    void Note(bool ok, std::size_t order, std::string const& what)
    {
        ++checked;
        if (ok)
            return;
        if (!broken++)
        {
            firstOrder = order;
            firstWhat = what;
        }
    }
};

void Report(Invariant const& invariant)
{
    std::printf("  %7u  %s\n", invariant.checked, invariant.name);
    Check(invariant.checked > 0, std::string(invariant.name) + ": never exercised");
    Check(invariant.broken == 0, std::string(invariant.name) + ": " + std::to_string(invariant.broken) + " of " +
                                     std::to_string(invariant.checked) + " broken, first in order " +
                                     std::to_string(invariant.firstOrder) + " (" + invariant.firstWhat + ")");
}

struct Gen
{
    std::mt19937 random;

    int Int(int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); }
    float Real(float low, float high) { return std::uniform_real_distribution<float>(low, high)(random); }
    bool Chance(float p) { return Real(0.0f, 1.0f) < p; }
};

FieldSpawn MakeSpawn(FieldKind kind, std::uint32_t entry, std::uint32_t mapId, float x, float y, std::uint8_t minLevel,
                     std::uint8_t maxLevel)
{
    FieldSpawn spawn;
    spawn.kind = kind;
    spawn.entry = entry;
    spawn.mapId = mapId;
    spawn.x = x;
    spawn.y = y;
    spawn.minLevel = minLevel;
    spawn.maxLevel = maxLevel;
    spawn.hostile = kind == FieldKind::Creature;
    spawn.respawnSecs = 300;
    return spawn;
}

Need MakeNeed(std::uint32_t quest, std::uint32_t objective, NeedKind kind, std::vector<NeedSource> sources,
              std::uint32_t left)
{
    Need need;
    need.quest = quest;
    need.objective = objective;
    need.kind = kind;
    need.sources = std::move(sources);
    need.left = left;
    return need;
}

// The harness's own rules, written apart from QuestFieldExec.h
std::int32_t CellOf(float coordinate) { return static_cast<std::int32_t>(std::floor(coordinate / 100.0f)); }

bool HarnessInField(QuestOrder const& order, std::uint32_t mapId, float x, float y)
{
    return mapId == order.mapId && std::abs(CellOf(x) - CellOf(order.x)) <= 1 &&
           std::abs(CellOf(y) - CellOf(order.y)) <= 1;
}

bool HarnessReach(X::Candidate const& candidate, int botLevel)
{
    return candidate.kind == FieldKind::Object || static_cast<int>(candidate.level) - botLevel <= REACH_LEVELS;
}

bool HarnessOpen(std::vector<QuestOrderOpen> const& open, std::uint32_t need)
{
    return need < open.size() && open[need].left > 0;
}

double Yards(double ax, double ay, double bx, double by) { return std::hypot(bx - ax, by - ay); }

// How much longer the walk to the place is by the point than straight there, in straight yards
double HarnessExtra(QuestOrder const& order, X::PickAsk const& ask, float x, float y)
{
    return Yards(ask.x, ask.y, x, y) + Yards(x, y, order.x, order.y) - Yards(ask.x, ask.y, order.x, order.y);
}

// Whether the order takes a point: in its field, or before the arrival within the allowance on the way
bool HarnessOnOrder(QuestOrder const& order, X::PickAsk const& ask, std::uint32_t mapId, float x, float y)
{
    return HarnessInField(order, mapId, x, y) || (ask.onWay && order.allowanceYards > 0.0f && mapId == order.mapId &&
                                                  HarnessExtra(order, ask, x, y) <= order.allowanceYards);
}

// A point too near the allowance's edge for the harness's doubles and the rule's floats to agree on
bool Ambiguous(QuestOrder const& order, X::PickAsk const& ask, std::uint32_t mapId, float x, float y)
{
    return ask.onWay && order.allowanceYards > 0.0f && mapId == order.mapId &&
           std::abs(HarnessExtra(order, ask, x, y) - order.allowanceYards) < 0.05;
}

bool HarnessTakes(X::Candidate const& candidate, std::vector<QuestOrderOpen> const& open, QuestOrder const& order,
                  int botLevel, X::PickAsk const& ask)
{
    return HarnessOpen(open, candidate.need) && HarnessReach(candidate, botLevel) &&
           HarnessOnOrder(order, ask, candidate.mapId, candidate.x, candidate.y);
}

// Whether an expected hand-in is open by the harness's own account: its quest not handed in
bool HarnessHandInOpen(QuestOrder const& order, std::set<std::uint32_t> const& handedIn)
{
    for (QuestOrderNeed const& expected : order.expected)
        if (expected.objective == HAND_IN && !handedIn.count(expected.quest))
            return true;
    return false;
}

// Whether an expected need is open: an objective by its count, a hand-in by the harness's own account
bool HarnessExpectedOpen(QuestOrder const& order, std::vector<QuestOrderOpen> const& open,
                         std::set<std::uint32_t> const& handedIn)
{
    for (QuestOrderNeed const& expected : order.expected)
        for (QuestOrderOpen const& need : open)
            if (expected.objective != HAND_IN && need.quest == expected.quest && need.objective == expected.objective &&
                need.left > 0)
                return true;
    return HarnessHandInOpen(order, handedIn);
}

// Whether a need is one the harness expects of a field: an open objective or a ready quest's hand-in (no take) with a
// source (a hand-in's ender) that has a row in one of the nine cells
bool HarnessExpects(FieldMap const& map, Need const& need, std::uint32_t mapId, float x, float y)
{
    if (need.objective == TAKE || !need.left)
        return false;
    for (FieldRow const& row : map.Rows())
    {
        if (FieldMapOf(row.cell) != mapId || std::abs(FieldCellXOf(row.cell) - CellOf(x)) > 1 ||
            std::abs(FieldCellYOf(row.cell) - CellOf(y)) > 1)
            continue;
        for (NeedSource const& source : need.sources)
            if (source.kind == row.kind && source.entry == row.entry)
                return true;
    }
    return false;
}

struct Unpacked
{
    std::uint32_t placeCredits{0};
    std::uint32_t wayCredits{0};
    std::uint32_t minutes{0};
    std::int32_t cellX{0};
    std::int32_t cellY{0};
};

std::int32_t SignedTen(std::int64_t bits)
{
    bits &= 0x3FF;
    return static_cast<std::int32_t>(bits >= 0x200 ? bits - 0x400 : bits);
}

Unpacked Unpack(std::int64_t amount)
{
    Unpacked u;
    u.placeCredits = static_cast<std::uint32_t>(amount & 0xFFF);
    u.wayCredits = static_cast<std::uint32_t>((amount >> 12) & 0xFFF);
    u.minutes = static_cast<std::uint32_t>((amount >> 24) & 0xFFF);
    u.cellX = SignedTen(amount >> 36);
    u.cellY = SignedTen(amount >> 46);
    return u;
}

struct World
{
    FieldMap map;
    std::vector<Need> needs;
    std::uint32_t mapId{MAP_A};
    float x{0.0f};
    float y{0.0f};
    int level{1};
};

// A made-up map (creatures around their homes, objects, a few entries never spawned), a log's needs (open and closed
// objectives, hand-ins and takes, sources spawned or not) and a bot; the order's point at a spawn in most worlds
World Generate(Gen& gen)
{
    World w;
    w.level = gen.Int(1, 60);
    std::vector<std::pair<std::int32_t, std::int32_t>> homes(CREATURES + OBJECTS);
    for (auto& home : homes)
        home = {gen.Int(CELL_LOW, CELL_HIGH), gen.Int(CELL_LOW, CELL_HIGH)};
    std::vector<std::uint8_t> levels(CREATURES);
    for (std::uint8_t& level : levels)
        level = static_cast<std::uint8_t>(gen.Int(std::max(1, w.level - 6), w.level + 12));
    std::vector<FieldSpawn> spawns;
    int const count = gen.Int(20, 120);
    for (int i = 0; i < count; ++i)
    {
        bool const object = gen.Chance(0.25f);
        std::uint32_t const at =
            static_cast<std::uint32_t>(gen.Int(0, static_cast<int>(object ? OBJECTS : CREATURES) - 1));
        auto const& home = homes[object ? CREATURES + at : at];
        std::uint32_t const mapId = gen.Chance(0.85f) ? MAP_A : MAP_B;
        float const x = (static_cast<float>(home.first + gen.Int(-1, 1)) + gen.Real(0.01f, 0.99f)) * FIELD_CELL_YARDS;
        float const y = (static_cast<float>(home.second + gen.Int(-1, 1)) + gen.Real(0.01f, 0.99f)) * FIELD_CELL_YARDS;
        if (object)
            spawns.push_back(MakeSpawn(FieldKind::Object, FIRST_OBJECT + at, mapId, x, y, 0, 0));
        else
            spawns.push_back(MakeSpawn(FieldKind::Creature, FIRST_CREATURE + at, mapId, x, y, levels[at],
                                       static_cast<std::uint8_t>(levels[at] + gen.Int(0, 2))));
    }
    w.mapId = MAP_A;
    if (gen.Chance(0.75f))
    {
        FieldSpawn const& spawn = spawns[static_cast<std::size_t>(gen.Int(0, count - 1))];
        w.x = spawn.x + gen.Real(-30.0f, 30.0f);
        w.y = spawn.y + gen.Real(-30.0f, 30.0f);
    }
    else
    {
        w.x = gen.Real(static_cast<float>(CELL_LOW), static_cast<float>(CELL_HIGH + 1)) * FIELD_CELL_YARDS;
        w.y = gen.Real(static_cast<float>(CELL_LOW), static_cast<float>(CELL_HIGH + 1)) * FIELD_CELL_YARDS;
    }
    w.map = FieldMap::Build(std::move(spawns));

    std::set<std::pair<std::uint32_t, std::uint32_t>> taken;
    int const needs = gen.Int(1, 8);
    for (int i = 0; i < needs; ++i)
    {
        std::uint32_t const quest = FIRST_QUEST + static_cast<std::uint32_t>(gen.Int(0, 5));
        int const roll = gen.Int(0, 99);
        std::uint32_t const objective =
            roll < 10 ? HAND_IN : (roll < 15 ? TAKE : static_cast<std::uint32_t>(gen.Int(0, 5)));
        if (!taken.insert({quest, objective}).second)
            continue;
        std::vector<NeedSource> sources;
        int const sourceCount = gen.Int(1, 2);
        for (int s = 0; s < sourceCount; ++s)
        {
            bool const object = gen.Chance(0.3f);
            std::uint32_t const entry =
                object ? FIRST_OBJECT + static_cast<std::uint32_t>(gen.Int(0, OBJECTS - 1))
                       : FIRST_CREATURE + static_cast<std::uint32_t>(gen.Int(0, CREATURES + UNSPAWNED - 1));
            sources.push_back({object ? FieldKind::Object : FieldKind::Creature, entry, 1.0f});
        }
        NeedKind const kind = objective == HAND_IN ? NeedKind::HandIn
                              : objective == TAKE  ? NeedKind::Take
                                                   : static_cast<NeedKind>(gen.Int(0, 3));
        std::uint32_t const left = objective >= TAKE ? 1 : static_cast<std::uint32_t>(gen.Int(0, 6));
        w.needs.push_back(MakeNeed(quest, objective, kind, std::move(sources), left));
    }
    return w;
}

struct Invariants
{
    Invariant expectExact{"Expect: the expected needs are the open objectives with a source in the field, no more"};
    Invariant openExact{"Open: every need with credits left, with its count"};
    Invariant pickAllowed{"Pick never returns a need whose objective is closed"};
    Invariant pickReach{"a target above the reach rule is never picked"};
    Invariant pickField{"a pick lies in the order's field (its cell and the eight around it, on its map)"};
    Invariant pickNearest{"Pick: the nearest candidate it may take; none only when there is none"};
    Invariant neverDone{"an order never ends done while an expected need is open"};
    Invariant neverDry{"an order never ends dry while a candidate it may take is in reach"};
    Invariant doneLive{"an order ends done once its expected needs are closed"};
    Invariant dryLive{"an order ends once its place gave nothing for the look time"};
    Invariant timeLive{"an order ends once its stay has passed"};
    Invariant bounded{"every order ends within its minutes' bound"};
    Invariant noFailure{"only no_ender is Abandoned (the failure row), each end its outcome"};
    Invariant credits{"the row's credits and quests are the rises of the log's progress"};
    Invariant row{"the row's amount unpacks to its credits, minutes and cell; its source to word and end"};
    Invariant neverDoneHandIn{"an order that expects a hand-in never ends done while that quest is not handed in"};
    Invariant quietEnd{"after the look time an order ends no_ender while an expected hand-in is open, else dry"};
    Invariant quietLate{"dry and no_ender come only after the look time without a find at the place"};
    Invariant noEnderRows{"no_ender gives up each expected hand-in not handed in once, no other quest"};
    Invariant handInsOnce{"handIns counts each quest handed in on the order once"};
    Invariant twice{"the rules applied twice change nothing more (NoteHandIns, End)"};
    Invariant wayOrder{"WayOrder: a way to the point, nothing expected, the end its arrival, the allowance given"};
    Invariant pickWay{"a pick lies in the field or, before the arrival, within the allowance on the way"};
    Invariant wayLine{"on the way a candidate on the line to the place is always taken (open, within reach)"};
    Invariant wayBehind{"a candidate behind the bot beyond the allowance, out of the field, is never taken"};
    Invariant wayWider{"a larger allowance never drops a candidate"};
    Invariant wayAtPlace{"at the place, and with no allowance, the rule is the field rule"};
    Invariant onOrder{"OnOrder: the field, or the allowance on the way, as the harness's rule"};
    Invariant heldOnOrder{"the held target is the nearest one the order takes (a nearer one off the way is left)"};
    Invariant wayEnd{"a way ends arrived once arrived with no work found, time only when never so, else replaced"};
    std::size_t withExpected{0};
    std::size_t withHandIns{0};
    std::size_t ends[6]{};  // by QuestOrderEnd
};

StatusEnd::Outcome ExpectedOutcome(QuestOrderEnd end)
{
    switch (end)
    {
        case QuestOrderEnd::Done:
            return StatusEnd::Outcome::Finished;
        case QuestOrderEnd::Arrived:
            return StatusEnd::Outcome::Arrived;
        case QuestOrderEnd::Bags:
        case QuestOrderEnd::Time:
        case QuestOrderEnd::Dry:
            return StatusEnd::Outcome::TimedOut;
        case QuestOrderEnd::NoEnder:
            return StatusEnd::Outcome::Abandoned;
        default:
            return StatusEnd::Outcome::Ordered;
    }
}

void CheckExpect(World const& w, QuestOrder const& order, Invariants& inv, std::size_t at)
{
    std::set<std::pair<std::uint32_t, std::uint32_t>> want;
    for (Need const& need : w.needs)
        if (HarnessExpects(w.map, need, w.mapId, w.x, w.y))
            want.insert({need.quest, need.objective});
    std::set<std::pair<std::uint32_t, std::uint32_t>> got;
    bool credited = true;
    for (QuestOrderNeed const& expected : order.expected)
    {
        got.insert({expected.quest, expected.objective});
        auto const it = std::find_if(w.needs.begin(), w.needs.end(), [&expected](Need const& need)
                                     { return need.quest == expected.quest && need.objective == expected.objective; });
        credited = credited && it != w.needs.end() && expected.credits == it->left;
    }
    inv.expectExact.Note(
        want == got && got.size() == order.expected.size() && credited, at,
        "expected " + std::to_string(order.expected.size()) + ", harness " + std::to_string(want.size()));
    std::vector<QuestOrderOpen> const open = X::Open(w.needs);
    std::size_t wantOpen = 0;
    bool same = true;
    for (Need const& need : w.needs)
    {
        if (!need.left)
            continue;
        same = same && wantOpen < open.size() && open[wantOpen].quest == need.quest &&
               open[wantOpen].objective == need.objective && open[wantOpen].left == need.left;
        ++wantOpen;
    }
    inv.openExact.Note(same && open.size() == wantOpen, at, "open " + std::to_string(open.size()));
}

std::string Name(std::optional<QuestOrderEnd> end) { return end ? QuestOrderEndName(*end) : "none"; }

// WQ20: a candidate the way's generator adds for an open need (no hand-in: a look seeks no ender) at a point
X::Candidate WayCandidate(Gen& way, QuestOrderRun const& run, World const& w, float x, float y)
{
    X::Candidate candidate;
    candidate.need = static_cast<std::uint32_t>(way.Int(0, static_cast<int>(run.open.size())));
    if (candidate.need < run.open.size() && run.open[candidate.need].objective == HAND_IN)
        candidate.need = static_cast<std::uint32_t>(run.open.size());
    candidate.kind = way.Chance(0.25f) ? FieldKind::Object : FieldKind::Creature;
    candidate.level = candidate.kind == FieldKind::Object
                          ? 0
                          : static_cast<std::uint8_t>(std::clamp(w.level + way.Int(-5, 12), 1, 83));
    candidate.mapId = w.mapId;
    candidate.x = x;
    candidate.y = y;
    return candidate;
}

// One generated order ticked to its end; show traces it. way draws what WQ20 adds, so gen's draws stay WQ19's
void RunOrder(Gen& gen, Gen& way, World const& w, Invariants& inv, std::size_t at, bool show)
{
    QuestOrder order = X::FieldOrder(w.map, w.needs, w.mapId, w.x, w.y, gen.Real(0.5f, 20.0f));
    order.walkMinutes = gen.Chance(0.3f) ? 0.0f : gen.Real(0.1f, 6.0f);
    if (gen.Chance(0.1f))
        order.end = QuestOrderEnd::Arrived;
    CheckExpect(w, order, inv, at);
    inv.withExpected += order.expected.empty() ? 0 : 1;
    inv.withHandIns += HarnessHandInOpen(order, {}) ? 1 : 0;
    // WQ20: a quarter of the orders keep no allowance (WQ19a's field rule), a quarter are field orders with one, half
    // are ways; the bot walks to the place from a point 150 to 600 yards off
    int const shape = way.Int(0, 3);
    float const allowance = way.Real(5.0f, 250.0f);
    bool const isWay = shape >= 2;
    if (isWay)
    {
        QuestOrder made = X::WayOrder(w.mapId, w.x, w.y, allowance);
        inv.wayOrder.Note(made.word == QuestOrderWord::Way && made.end == QuestOrderEnd::Arrived &&
                              made.expected.empty() && made.mapId == w.mapId && made.x == w.x && made.y == w.y &&
                              made.cell == FieldCellKeyAt(w.mapId, w.x, w.y) && made.allowanceYards == allowance &&
                              made.minutes == X::WAY_MINUTES,
                          at, "way order");
        made.walkMinutes = order.walkMinutes;
        order = made;
    }
    else if (shape == 1)
        order.allowanceYards = allowance;
    float const angle = way.Real(0.0f, 6.2831853f);
    float const from = way.Real(150.0f, 600.0f);
    float const startX = w.x + from * std::cos(angle);
    float const startY = w.y + from * std::sin(angle);

    QuestOrderRun run;
    run.order = order;
    // odd stamps: a tick never falls on 0, which the run would keep as 1
    std::uint32_t const base = 1u | (gen.Chance(0.2f) ? 0xFFFFFFFFu - static_cast<std::uint32_t>(gen.Int(0, 400000))
                                                      : static_cast<std::uint32_t>(gen.Int(1, 1000000000)));
    run.startedAt = X::Stamp(base);
    run.open = X::Open(w.needs);
    for (Need const& need : w.needs)  // the executor marks every quest of the log at the start
        X::Note(run.marks, need.quest, 0);
    std::uint32_t const stayMs = static_cast<std::uint32_t>(order.minutes * static_cast<float>(MS_PER_MINUTE));
    std::uint32_t const walkTicks =
        static_cast<std::uint32_t>(order.walkMinutes * static_cast<float>(MS_PER_MINUTE) / TICK_MS);
    int const arrival = gen.Chance(0.15f) ? NEVER : gen.Int(0, static_cast<int>(walkTicks * 3 / 2));
    double const boundMs = (X::WALK_SLACK * order.walkMinutes + 2.0 * order.minutes) * MS_PER_MINUTE + 2.0 * TICK_MS;
    int const cap = static_cast<int>(boundMs / TICK_MS) + 4;
    // WQ20b: before the arrival the walk's clock restarts at each count of the log (a credit, a hand-in); the counts
    // are finite, so every order still ends: within the walk bound once per count and once more, then the stay
    run.lastCount = run.startedAt;
    std::uint32_t counts = 0;
    for (QuestOrderOpen const& need : run.open)
        counts += need.objective == HAND_IN ? 1 : need.left;
    double const countedMs = (X::WALK_SLACK * order.walkMinutes + order.minutes) * MS_PER_MINUTE * (counts + 1) +
                             order.minutes * MS_PER_MINUTE + 2.0 * TICK_MS;
    double const endMs = std::max(boundMs, countedMs);
    int const loopCap = static_cast<int>(endMs / TICK_MS) + 4;
    // WQ19b: the giver search hands a ready quest in at a tick of its own, on the way or at the place; some enders
    // never take it (out of reach, the reward does not fit)
    std::map<std::uint32_t, int> handAt;  // per quest of an open hand-in need
    for (QuestOrderOpen const& need : run.open)
        if (need.objective == HAND_IN)
            handAt[need.quest] = gen.Chance(0.35f) ? NEVER : gen.Int(0, cap);
    std::set<std::uint32_t> handedIn;
    auto const rewarded = [&handedIn](std::uint32_t quest) { return handedIn.count(quest) > 0; };
    std::map<std::uint32_t, std::uint32_t> progress;  // per quest
    std::map<std::uint32_t, std::uint32_t> gained;
    int lastFound = NEVER;
    int freeAt = NEVER;  // WQ20b: the first tick at or after the arrival with no work found
    if (show)
        std::printf("order %zu: map %u (%.1f, %.1f) level %d, %zu needs, %zu expected, %.2f min, walk %.2f min, %s\n",
                    at, w.mapId, w.x, w.y, w.level, w.needs.size(), order.expected.size(), order.minutes,
                    order.walkMinutes, arrival == NEVER ? "never arrives" : "arrives");
    std::optional<QuestOrderEnd> end;
    std::uint32_t lastNow = base;
    bool lastFinding = false;
    int k = 0;
    for (; k <= loopCap && !end; ++k)
    {
        std::uint32_t const now = base + static_cast<std::uint32_t>(k) * TICK_MS;
        bool const arrived = arrival != NEVER && k >= arrival;
        if (arrived && !run.arrivedAt)
            run.arrivedAt = X::Stamp(now);
        bool handedNow = false;
        for (auto const& [quest, tick] : handAt)
            if (tick != NEVER && tick == k)
                handedNow = handedIn.insert(quest).second || handedNow;
        std::uint32_t const handed = X::NoteHandIns(run, now, rewarded);
        if (handed)  // the caller counts the log again
            run.lastCount = X::Stamp(now);
        if (handedNow)
            lastFound = k;
        // on the way the bot stands on the line from its start, short of the place
        float const share =
            std::min(0.97f, static_cast<float>(k) /
                                static_cast<float>(arrival == NEVER ? walkTicks * 3 / 2 + 1 : std::max(arrival, 1)));
        X::PickAsk ask;
        ask.x = arrived ? w.x + gen.Real(-40.0f, 40.0f) : startX + (w.x - startX) * share;
        ask.y = arrived ? w.y + gen.Real(-40.0f, 40.0f) : startY + (w.y - startY) * share;
        ask.level = static_cast<std::uint8_t>(w.level);
        ask.onWay = !arrived;
        std::vector<X::Candidate> candidates(static_cast<std::size_t>(gen.Int(0, arrived ? 5 : 2)));
        for (X::Candidate& candidate : candidates)
        {
            candidate.need = static_cast<std::uint32_t>(gen.Int(0, static_cast<int>(run.open.size())));
            if (candidate.need < run.open.size() && run.open[candidate.need].objective == HAND_IN)
                candidate.need = static_cast<std::uint32_t>(run.open.size());  // a look seeks no ender
            candidate.kind = gen.Chance(0.25f) ? FieldKind::Object : FieldKind::Creature;
            candidate.level = candidate.kind == FieldKind::Object
                                  ? 0
                                  : static_cast<std::uint8_t>(std::clamp(w.level + gen.Int(-5, 12), 1, 83));
            candidate.mapId = gen.Chance(0.95f) ? w.mapId : MAP_B + 7;
            candidate.x = w.x + gen.Real(-260.0f, 260.0f);
            candidate.y = w.y + gen.Real(-260.0f, 260.0f);
        }
        std::erase_if(candidates, [&order, &ask](X::Candidate const& candidate)
                      { return Ambiguous(order, ask, candidate.mapId, candidate.x, candidate.y); });
        // WQ20: on the way one candidate on the line to the place, one behind the bot beyond the allowance, one off the
        // line about the allowance
        std::optional<std::size_t> line;
        std::optional<std::size_t> behind;
        float const dx = w.x - ask.x;
        float const dy = w.y - ask.y;
        float const length = std::hypot(dx, dy);
        if (!arrived && length > 1.0f)
        {
            if (way.Chance(0.6f))
            {
                float const t = way.Real(0.0f, 1.0f);
                candidates.push_back(WayCandidate(way, run, w, ask.x + dx * t, ask.y + dy * t));
                line = candidates.size() - 1;
            }
            if (way.Chance(0.5f))
            {
                float const d = way.Real(0.5f * order.allowanceYards + 1.0f, 0.5f * order.allowanceYards + 200.0f);
                candidates.push_back(WayCandidate(way, run, w, ask.x - dx / length * d, ask.y - dy / length * d));
                behind = candidates.size() - 1;
            }
            if (way.Chance(0.6f))
            {
                float const t = way.Real(-0.2f, 1.2f);
                float const off = way.Real(-1.2f, 1.2f) * std::max(order.allowanceYards, 20.0f);
                X::Candidate const candidate =
                    WayCandidate(way, run, w, ask.x + dx * t - dy / length * off, ask.y + dy * t + dx / length * off);
                if (!Ambiguous(order, ask, candidate.mapId, candidate.x, candidate.y))
                    candidates.push_back(candidate);
            }
        }
        std::optional<std::size_t> const pick = X::Pick(candidates, run.open, run.order, ask);
        std::optional<std::size_t> nearest;
        float nearestYards = 0.0f;
        for (std::size_t c = 0; c < candidates.size(); ++c)
        {
            if (!HarnessTakes(candidates[c], run.open, order, w.level, ask))
                continue;
            float const yards = std::hypot(candidates[c].x - ask.x, candidates[c].y - ask.y);
            if (!nearest || yards < nearestYards)
            {
                nearest = c;
                nearestYards = yards;
            }
        }
        if (pick)
        {
            X::Candidate const& picked = candidates[*pick];
            inv.pickAllowed.Note(HarnessOpen(run.open, picked.need), at, "need " + std::to_string(picked.need));
            inv.pickReach.Note(HarnessReach(picked, w.level), at,
                               "level " + std::to_string(picked.level) + " for " + std::to_string(w.level));
            if (!ask.onWay)
                inv.pickField.Note(HarnessInField(order, picked.mapId, picked.x, picked.y), at, "out of the field");
            inv.pickWay.Note(HarnessOnOrder(order, ask, picked.mapId, picked.x, picked.y), at,
                             "off the way by " + std::to_string(HarnessExtra(order, ask, picked.x, picked.y)));
            // the picked need's seek holds the nearest of its targets that the order takes (ValidTarget's rule)
            std::optional<int> held;
            std::optional<int> want;
            float heldYards = 0.0f;
            float wantYards = 0.0f;
            int const targets = way.Int(1, 4);
            for (int t = 0; t < targets; ++t)
            {
                float const along = way.Real(0.0f, 1.0f);
                bool const near = way.Chance(0.5f);
                float const tx = near ? ask.x + way.Real(-90.0f, 90.0f) : ask.x + dx * along;
                float const ty = near ? ask.y + way.Real(-90.0f, 90.0f) : ask.y + dy * along;
                if (Ambiguous(order, ask, w.mapId, tx, ty))
                    continue;
                float const yards = std::hypot(tx - ask.x, ty - ask.y);
                if (X::OnOrder(order, ask, w.mapId, tx, ty) && (!held || yards < heldYards))
                {
                    held = t;
                    heldYards = yards;
                }
                if (HarnessOnOrder(order, ask, w.mapId, tx, ty) && (!want || yards < wantYards))
                {
                    want = t;
                    wantYards = yards;
                }
            }
            inv.heldOnOrder.Note(held == want, at, "tick " + std::to_string(k));
        }
        {
            X::PickAsk atPlace = ask;
            atPlace.onWay = false;
            QuestOrder noWay = order;
            noWay.allowanceYards = 0.0f;
            QuestOrder wider = order;
            wider.allowanceYards += way.Real(0.0f, 100.0f);
            for (X::Candidate const& candidate : candidates)
            {
                bool const takes = X::Takes(candidate, run.open, order, ask);
                inv.wayWider.Note(!takes || X::Takes(candidate, run.open, wider, ask), at, "tick " + std::to_string(k));
                inv.wayAtPlace.Note(
                    X::Takes(candidate, run.open, order, atPlace) == X::Takes(candidate, run.open, noWay, ask), at,
                    "tick " + std::to_string(k));
                inv.onOrder.Note(X::OnOrder(order, ask, candidate.mapId, candidate.x, candidate.y) ==
                                     HarnessOnOrder(order, ask, candidate.mapId, candidate.x, candidate.y),
                                 at, "tick " + std::to_string(k));
            }
            if (line && order.allowanceYards > 0.0f && HarnessOpen(run.open, candidates[*line].need) &&
                HarnessReach(candidates[*line], w.level))
                inv.wayLine.Note(X::Takes(candidates[*line], run.open, order, ask), at, "tick " + std::to_string(k));
            if (behind &&
                !HarnessInField(order, candidates[*behind].mapId, candidates[*behind].x, candidates[*behind].y))
                inv.wayBehind.Note(!X::Takes(candidates[*behind], run.open, order, ask), at,
                                   "tick " + std::to_string(k));
        }
        float const pickYards = pick ? std::hypot(candidates[*pick].x - ask.x, candidates[*pick].y - ask.y) : 0.0f;
        inv.pickNearest.Note(pick.has_value() == nearest.has_value() && (!pick || pickYards <= nearestYards), at,
                             "tick " + std::to_string(k));

        bool const working = pick.has_value();
        if (working)
        {
            run.lastFound = X::Stamp(now);
            lastFound = k;
            QuestOrderOpen& need = run.open[candidates[*pick].need];
            if (gen.Chance(0.35f) && need.left)
            {
                --need.left;
                std::uint32_t const added = X::Note(run.marks, need.quest, ++progress[need.quest]);
                ++gained[need.quest];
                (run.arrivedAt ? run.placeCredits : run.wayCredits) += added;
                run.lastCount = X::Stamp(now);  // a credit counts the log again
            }
        }
        if (gen.Chance(0.01f) && !progress.empty())  // a quest left the log (abandoned): its progress falls
        {
            auto it = progress.begin();
            std::advance(it, gen.Int(0, static_cast<int>(progress.size()) - 1));
            it->second = 0;
            X::Note(run.marks, it->first, 0);
        }
        if (gen.Chance(0.0015f))
            run.replaced = true;

        bool const finding = working || handed > 0;
        if (arrived && !finding && freeAt == NEVER)
            freeAt = k;
        end = X::End(run, now, finding);
        lastNow = now;
        lastFinding = finding;
        bool const expectedOpen = HarnessExpectedOpen(order, run.open, handedIn);
        if (show)
            std::printf("  tick %d: %zu candidates, pick %s, %s\n", k, candidates.size(),
                        pick ? std::to_string(*pick).c_str() : "none", Name(end).c_str());
        if (end == QuestOrderEnd::Done)
            inv.neverDone.Note(!expectedOpen, at, "tick " + std::to_string(k));
        if (end == QuestOrderEnd::Done)
            inv.neverDoneHandIn.Note(!HarnessHandInOpen(order, handedIn), at, "tick " + std::to_string(k));
        if (end == QuestOrderEnd::Dry)
            inv.neverDry.Note(!nearest, at, "tick " + std::to_string(k));
        if (order.end == QuestOrderEnd::Done && !order.expected.empty() && !expectedOpen && !run.replaced)
            inv.doneLive.Note(end == QuestOrderEnd::Done, at, "tick " + std::to_string(k) + ": " + Name(end));
        int const quietFrom = std::max(arrival, lastFound);
        if (end == QuestOrderEnd::Dry || end == QuestOrderEnd::NoEnder)
        {
            inv.quietEnd.Note((end == QuestOrderEnd::NoEnder) == HarnessHandInOpen(order, handedIn), at,
                              "tick " + std::to_string(k) + ": " + Name(end));
            inv.quietLate.Note(
                arrived && !finding && (k - quietFrom) * static_cast<int>(TICK_MS) >= static_cast<int>(X::LOOK_MS), at,
                "tick " + std::to_string(k) + ": " + Name(end));
        }
        if (arrived && !nearest && (k - quietFrom) * static_cast<int>(TICK_MS) >= static_cast<int>(X::LOOK_MS))
            inv.dryLive.Note(end.has_value(), at, "tick " + std::to_string(k));
        if (arrived && static_cast<double>(k - arrival) * TICK_MS >= stayMs)
            inv.timeLive.Note(end.has_value(), at, "tick " + std::to_string(k));
    }
    inv.bounded.Note(end.has_value() && static_cast<double>(k - 1) * TICK_MS <= endMs, at,
                     "tick " + std::to_string(k - 1) + " of bound " + std::to_string(endMs) + " ms");
    if (!end)
        return;
    ++inv.ends[static_cast<std::size_t>(*end)];
    if (isWay)
        inv.wayEnd.Note((*end == QuestOrderEnd::Arrived && freeAt != NEVER && k - 1 == freeAt) ||
                            (*end == QuestOrderEnd::Time && freeAt == NEVER) || *end == QuestOrderEnd::Replaced,
                        at, Name(end) + " at tick " + std::to_string(k - 1));
    inv.noFailure.Note((X::OutcomeOf(*end) == StatusEnd::Outcome::Abandoned) == (*end == QuestOrderEnd::NoEnder) &&
                           X::OutcomeOf(*end) == ExpectedOutcome(*end),
                       at, Name(end));
    if (*end == QuestOrderEnd::NoEnder)
    {
        std::vector<std::uint32_t> const rows = X::NoEnderQuests(run);
        std::set<std::uint32_t> want;
        for (QuestOrderNeed const& expected : order.expected)
            if (expected.objective == HAND_IN && !handedIn.count(expected.quest))
                want.insert(expected.quest);
        inv.noEnderRows.Note(std::set<std::uint32_t>(rows.begin(), rows.end()) == want && rows.size() == want.size(),
                             at, std::to_string(rows.size()) + " rows, harness " + std::to_string(want.size()));
    }
    inv.handInsOnce.Note(run.handIns == handedIn.size(), at,
                         std::to_string(run.handIns) + " of " + std::to_string(handedIn.size()));
    QuestOrderRun again = run;
    std::uint32_t const more = X::NoteHandIns(again, lastNow, rewarded);
    inv.twice.Note(more == 0 && again.handIns == run.handIns && again.lastFound == run.lastFound &&
                       X::End(again, lastNow, lastFinding) == end,
                   at, std::to_string(more) + " more");
    std::uint32_t sum = 0;
    std::uint32_t quests = 0;
    for (auto const& [quest, n] : gained)
    {
        sum += n;
        quests += n ? 1 : 0;
    }
    std::uint32_t marked = 0;
    for (QuestOrderMark const& mark : run.marks)
        marked += mark.gained;
    inv.credits.Note(
        sum == run.placeCredits + run.wayCredits && sum == marked && quests == X::QuestsCredited(run.marks), at,
        std::to_string(sum) + " credits of " + std::to_string(quests) + " quests");
    std::uint32_t const minutes = static_cast<std::uint32_t>(k) * TICK_MS / MS_PER_MINUTE;
    Unpacked const u = Unpack(X::RowAmount(run.placeCredits, run.wayCredits, minutes, order.cell));
    std::string const source = std::string(QuestOrderWordName(order.word)) + ":" + QuestOrderEndName(*end);
    inv.row.Note(u.placeCredits == run.placeCredits && u.wayCredits == run.wayCredits && u.minutes == minutes &&
                     u.cellX == CellOf(w.x) && u.cellY == CellOf(w.y) && source == X::RowSource(order.word, *end),
                 at, X::RowSource(order.word, *end));
}

void GeneratedOrders(std::uint32_t seed, std::size_t show)
{
    Gen gen{std::mt19937(seed)};
    Gen way{std::mt19937(seed ^ 0x9E3779B9u)};
    Invariants inv;
    for (std::size_t at = 0; at < ORDERS; ++at)
    {
        World const w = Generate(gen);
        RunOrder(gen, way, w, inv, at, at == show);
    }
    std::printf(
        "seed=%u orders=%zu with expected needs=%zu (hand-ins %zu) ends: done %zu, arrived %zu, time %zu, dry %zu, "
        "replaced %zu, no_ender %zu\n",
        seed, ORDERS, inv.withExpected, inv.withHandIns, inv.ends[0], inv.ends[1], inv.ends[2], inv.ends[3],
        inv.ends[4], inv.ends[5]);
    Report(inv.expectExact);
    Report(inv.openExact);
    Report(inv.pickAllowed);
    Report(inv.pickReach);
    Report(inv.pickField);
    Report(inv.pickNearest);
    Report(inv.neverDone);
    Report(inv.neverDry);
    Report(inv.doneLive);
    Report(inv.dryLive);
    Report(inv.timeLive);
    Report(inv.bounded);
    Report(inv.noFailure);
    Report(inv.credits);
    Report(inv.row);
    Report(inv.neverDoneHandIn);
    Report(inv.quietEnd);
    Report(inv.quietLate);
    Report(inv.noEnderRows);
    Report(inv.handInsOnce);
    Report(inv.twice);
    Report(inv.wayOrder);
    Report(inv.pickWay);
    Report(inv.wayLine);
    Report(inv.wayBehind);
    Report(inv.wayWider);
    Report(inv.wayAtPlace);
    Report(inv.onOrder);
    Report(inv.heldOnOrder);
    Report(inv.wayEnd);
    Check(inv.ends[5] > 0 && inv.ends[0] > 0, "the generated orders end no_ender and done");
}

// Ticks an order at its place with the given candidates every tick (none: an empty place) until it ends; a quest of
// handAt is handed in from that second on (one not in it never is). Returns the end and the seconds after the arrival
std::pair<std::optional<QuestOrderEnd>, std::uint32_t> TickAtPlace(
    QuestOrderRun& run, std::vector<X::Candidate> const& candidates, X::PickAsk const& ask, std::uint32_t limitSeconds,
    std::map<std::uint32_t, std::uint32_t> const& handAt = {})
{
    std::uint32_t const base = 5000;
    run.startedAt = base;
    run.arrivedAt = base;
    for (std::uint32_t s = 0; s <= limitSeconds; s += TICK_MS / 1000)
    {
        std::uint32_t const now = base + s * 1000;
        auto const rewarded = [&handAt, s](std::uint32_t quest)
        {
            auto const it = handAt.find(quest);
            return it != handAt.end() && s >= it->second;
        };
        bool const handed = X::NoteHandIns(run, now, rewarded) > 0;
        std::optional<std::size_t> const pick = X::Pick(candidates, run.open, run.order, ask);
        if (pick)
            run.lastFound = now;
        if (std::optional<QuestOrderEnd> const end = X::End(run, now, pick.has_value() || handed))
            return {end, s};
    }
    return {std::nullopt, limitSeconds};
}

// field-above: a level 7 bot among the gnolls of quest 102 (Riverpaw levels 15 to 20 in the field): no pick, the end
// dry after the look time, no credit. The plan's level 12 would take a level 16 gnoll (4 above): why the case runs
// level 7
void FieldAbove()
{
    float const x = -11161.7f;
    float const y = 845.0f;
    std::vector<FieldSpawn> spawns;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> const gnolls = {
        {98, 17}, {124, 15}, {125, 19}, {452, 16}, {453, 18}};
    for (std::size_t i = 0; i < gnolls.size(); ++i)
        spawns.push_back(MakeSpawn(FieldKind::Creature, gnolls[i].first, MAP_A, x + 20.0f * static_cast<float>(i),
                                   y - 15.0f * static_cast<float>(i), gnolls[i].second,
                                   static_cast<std::uint8_t>(gnolls[i].second + 1)));
    FieldMap const map = FieldMap::Build(spawns);
    std::vector<NeedSource> sources;
    for (auto const& [entry, level] : gnolls)
        sources.push_back({FieldKind::Creature, entry, 0.8f});
    std::vector<Need> const needs = {MakeNeed(102, 0, NeedKind::Loot, sources, 8)};
    QuestOrderRun run;
    run.order = X::FieldOrder(map, needs, MAP_A, x, y, 5.0f);
    run.open = X::Open(needs);
    Check(run.order.expected.size() == 1, "field-above: quest 102's paws are expected at the camp");
    std::vector<X::Candidate> candidates;
    for (std::size_t i = 0; i < gnolls.size(); ++i)
        candidates.push_back({0, FieldKind::Creature, gnolls[i].second, MAP_A, x + 20.0f * static_cast<float>(i),
                              y - 15.0f * static_cast<float>(i)});
    X::PickAsk ask{x, y, 7, X::NEUTRAL_CAUTION};
    auto const [end, seconds] = TickAtPlace(run, candidates, ask, 600);
    Check(end == QuestOrderEnd::Dry && seconds == X::LOOK_MS / 1000,
          "field-above: a level 7 bot ends dry after the look time (" + Name(end) + " at " + std::to_string(seconds) +
              " s)");
    Check(X::QuestsCredited(run.marks) == 0 && run.placeCredits == 0, "field-above: no credit");
    X::PickAsk const twelve{x, y, 12, X::NEUTRAL_CAUTION};
    Check(X::Pick(candidates, run.open, run.order, twelve).has_value(),
          "field-above: at the plan's level 12 a level 15 or 16 gnoll is within reach (so the case runs level 7)");
    Check(!X::LevelWithinReach(15, 8) && X::LevelWithinReach(14, 8) && X::LevelWithinReach(99, 8, 0.0f),
          "the reach rule: 7 levels above is refused at a neutral caution, 6 taken; no caution takes any");
}

// field-two: quests 18 (bandanas from Defias Thugs) and 3904 (Milly's Harvest buckets) in the Northshire vineyard,
// cell (-91, -4); a ready quest whose ender stands at the abbey (cell (-90, -2), outside the field) is not expected.
// The order works both and ends done with credits of two quests
void FieldTwo()
{
    float const x = -9058.0f;
    float const y = -346.0f;
    std::vector<FieldSpawn> spawns;
    for (int i = 0; i < 6; ++i)
    {
        spawns.push_back(MakeSpawn(FieldKind::Creature, 38, MAP_A, x + 9.0f * static_cast<float>(i), y + 7.0f, 3, 4));
        spawns.push_back(
            MakeSpawn(FieldKind::Object, 161557, MAP_A, x - 11.0f * static_cast<float>(i), y - 5.0f, 0, 0));
    }
    spawns.push_back(MakeSpawn(FieldKind::Creature, 197, MAP_A, -8902.6f, -162.6f, 10, 10));  // an ender at the abbey
    FieldMap const map = FieldMap::Build(spawns);
    std::vector<Need> const needs = {MakeNeed(18, 0, NeedKind::Loot, {{FieldKind::Creature, 38, 0.6f}}, 8),
                                     MakeNeed(3904, 0, NeedKind::Chest, {{FieldKind::Object, 161557, 1.0f}}, 8),
                                     MakeNeed(33, HAND_IN, NeedKind::HandIn, {{FieldKind::Creature, 197, 1.0f}}, 1)};
    QuestOrderRun run;
    run.order = X::FieldOrder(map, needs, MAP_A, x, y, 15.0f);
    run.open = X::Open(needs);
    Check(run.order.expected.size() == 2 && run.order.cell == FieldCellKeyAt(MAP_A, x, y) &&
              run.order.word == QuestOrderWord::Field && run.order.end == QuestOrderEnd::Done,
          "field-two: both quests expected at the vineyard, not the hand-in");
    run.startedAt = 1000;
    run.arrivedAt = 1000;
    std::uint32_t now = 1000;
    std::uint32_t progress18 = 0;
    std::uint32_t progress3904 = 0;
    X::Note(run.marks, 18, 0);
    X::Note(run.marks, 3904, 0);
    std::optional<QuestOrderEnd> end;
    for (int tick = 0; tick < 200 && !end; ++tick, now += TICK_MS)
    {
        std::vector<X::Candidate> const candidates = {{0, FieldKind::Creature, 4, MAP_A, x + 5.0f, y + 7.0f},
                                                      {1, FieldKind::Object, 0, MAP_A, x - 3.0f, y - 5.0f}};
        X::PickAsk const ask{x + (tick % 2 ? -8.0f : 8.0f), y, 5, X::NEUTRAL_CAUTION};
        std::optional<std::size_t> const pick = X::Pick(candidates, run.open, run.order, ask);
        if (pick)
        {
            run.lastFound = now;
            QuestOrderOpen& need = run.open[candidates[*pick].need];
            if (need.left)
            {
                --need.left;
                run.placeCredits += X::Note(run.marks, need.quest, need.quest == 18 ? ++progress18 : ++progress3904);
            }
        }
        end = X::End(run, now, pick.has_value());
    }
    Check(end == QuestOrderEnd::Done && X::QuestsCredited(run.marks) == 2 && run.placeCredits == 16,
          "field-two: done with 16 credits of two quests (" + Name(end) + ", " +
              std::to_string(X::QuestsCredited(run.marks)) + " quests)");
    Check(std::string(X::RowSource(run.order.word, QuestOrderEnd::Done)) == "field:done",
          "field-two: the row's source");
}

// field-dry: a quest whose sources lie elsewhere (quest 102's gnolls) and an order at the abbey: nothing expected, so
// never done; dry after the look time, never before
void FieldDry()
{
    std::vector<FieldSpawn> spawns = {MakeSpawn(FieldKind::Creature, 98, MAP_A, -11161.0f, 845.0f, 17, 18),
                                      MakeSpawn(FieldKind::Creature, 6, MAP_A, -8903.0f, -150.0f, 1, 2)};
    FieldMap const map = FieldMap::Build(spawns);
    std::vector<Need> const needs = {MakeNeed(102, 0, NeedKind::Loot, {{FieldKind::Creature, 98, 0.8f}}, 8)};
    QuestOrderRun run;
    run.order = X::FieldOrder(map, needs, MAP_A, -8903.0f, -163.0f, 10.0f);
    run.open = X::Open(needs);
    Check(run.order.expected.empty(), "field-dry: nothing expected at the abbey");
    Check(!X::End(run, 5000, false).has_value() || run.arrivedAt, "field-dry: no end before the arrival");
    X::PickAsk const ask{-8903.0f, -163.0f, 5, X::NEUTRAL_CAUTION};
    auto const [end, seconds] = TickAtPlace(run, {}, ask, 600);
    Check(end == QuestOrderEnd::Dry && seconds == X::LOOK_MS / 1000,
          "field-dry: dry after the look time, not done at once (" + Name(end) + " at " + std::to_string(seconds) +
              " s)");
    Check(X::OutcomeOf(QuestOrderEnd::Dry) == StatusEnd::Outcome::TimedOut, "field-dry: its outcome writes no failure");
}

// WQ19b, handin-two and turnin-object: two ready quests with one creature ender at the place and one whose ender (an
// object) stands there too are expected, one ready quest with its ender elsewhere is not. Every ender takes them: done
// with three hand-ins; none does: no_ender after the look time with a failure row for each of the three; one does:
// no_ender later, a row for the other two only; other work found all the while: the stay's time, no failure row
void HandIns()
{
    float const x = -369.7f;
    float const y = -3005.3f;
    FieldMap const map = FieldMap::Build({MakeSpawn(FieldKind::Creature, 3391, MAP_A, x + 6.0f, y - 4.0f, 20, 20),
                                          MakeSpawn(FieldKind::Object, 181643, MAP_A, x, y, 0, 0),
                                          MakeSpawn(FieldKind::Creature, 3390, MAP_A, x + 900.0f, y, 20, 20),
                                          MakeSpawn(FieldKind::Creature, 3255, MAP_A, x + 20.0f, y + 15.0f, 13, 14)});
    std::vector<Need> const needs = {MakeNeed(55, 0, NeedKind::Kill, {{FieldKind::Creature, 3255, 1.0f}}, 4),
                                     MakeNeed(871, HAND_IN, NeedKind::HandIn, {{FieldKind::Creature, 3391, 1.0f}}, 1),
                                     MakeNeed(872, HAND_IN, NeedKind::HandIn, {{FieldKind::Creature, 3391, 1.0f}}, 1),
                                     MakeNeed(9476, HAND_IN, NeedKind::HandIn, {{FieldKind::Object, 181643, 1.0f}}, 1),
                                     MakeNeed(880, HAND_IN, NeedKind::HandIn, {{FieldKind::Creature, 3390, 1.0f}}, 1)};
    QuestOrder const order = X::FieldOrder(map, needs, MAP_A, x, y, 15.0f);
    std::vector<std::uint32_t> handIns;
    for (QuestOrderNeed const& expected : order.expected)
        if (expected.objective == HAND_IN)
            handIns.push_back(expected.quest);
    Check(order.expected.size() == 4 && handIns == std::vector<std::uint32_t>{871, 872, 9476} &&
              order.expected[0].objective == HAND_IN && order.expected[0].credits == 1,
          "handin-two: the three hand-ins at the place expected first (an object ender too), one credit each, not "
          "the one whose ender stands elsewhere; the kill too");
    X::PickAsk const ask{x, y, 20, X::NEUTRAL_CAUTION};
    auto const fresh = [&order, &needs]()
    {
        QuestOrderRun run;
        run.order = order;
        run.open = X::Open(needs);
        run.open[0].left = 0;  // the kill is done: only the hand-ins are left
        return run;
    };

    QuestOrderRun all = fresh();
    auto const [allEnd, allSeconds] = TickAtPlace(all, {}, ask, 600, {{871, 6}, {872, 6}, {9476, 8}});
    Check(allEnd == QuestOrderEnd::Done && allSeconds == 8 && all.handIns == 3,
          "handin-two: done once the three are handed in (" + Name(allEnd) + " at " + std::to_string(allSeconds) +
              " s, " + std::to_string(all.handIns) + " hand-ins)");
    Check(X::NoteHandIns(all, 99000, [](std::uint32_t) { return true; }) == 1 && all.handIns == 4,
          "a ready quest handed in whose ender was not expected counts too, once");
    Check(X::NoteHandIns(all, 99000, [](std::uint32_t) { return true; }) == 0 && all.handIns == 4,
          "a quest handed in is counted once");

    QuestOrderRun none = fresh();
    auto const [noneEnd, noneSeconds] = TickAtPlace(none, {}, ask, 600);
    std::vector<std::uint32_t> const noneRows = X::NoEnderQuests(none);
    Check(noneEnd == QuestOrderEnd::NoEnder && noneSeconds == X::LOOK_MS / 1000 &&
              noneRows == std::vector<std::uint32_t>{871, 872, 9476},
          "no ender takes them: no_ender after the look time, a failure row for each of the three (" + Name(noneEnd) +
              " at " + std::to_string(noneSeconds) + " s, " + std::to_string(noneRows.size()) + " rows)");
    Check(X::OutcomeOf(QuestOrderEnd::NoEnder) == StatusEnd::Outcome::Abandoned &&
              std::string(X::RowSource(QuestOrderWord::Field, QuestOrderEnd::NoEnder)) == "field:no_ender" &&
              std::string(QuestOrderEndName(QuestOrderEnd::NoEnder)) == "no_ender",
          "no_ender: Abandoned (the failure row at the door), the row field:no_ender");

    QuestOrderRun one = fresh();
    auto const [oneEnd, oneSeconds] = TickAtPlace(one, {}, ask, 600, {{872, 10}});
    std::vector<std::uint32_t> const oneRows = X::NoEnderQuests(one);
    Check(oneEnd == QuestOrderEnd::NoEnder && oneSeconds == 10 + X::LOOK_MS / 1000 &&
              oneRows == std::vector<std::uint32_t>{871, 9476} && one.handIns == 1,
          "one ender takes its quest: the look time starts again, no_ender then, no row for the quest handed in (" +
              Name(oneEnd) + " at " + std::to_string(oneSeconds) + " s)");

    QuestOrderRun busy = fresh();
    busy.order.minutes = 2.0f;
    busy.open[0].left = 4;  // the kill goes on all the while, no credit
    std::vector<X::Candidate> const boars = {{0, FieldKind::Creature, 14, MAP_A, x + 20.0f, y + 15.0f}};
    auto const [busyEnd, busySeconds] = TickAtPlace(busy, boars, ask, 600);
    Check(busyEnd == QuestOrderEnd::Time && busySeconds == 120 &&
              X::OutcomeOf(QuestOrderEnd::Time) != StatusEnd::Outcome::Abandoned,
          "work found all the while: the stay's time, no failure row (" + Name(busyEnd) + ")");
}

// WQ20: what a walk to the place did
struct Walked
{
    std::optional<QuestOrderEnd> end;
    int ticks{0};
    int picks{0};
    std::map<std::uint32_t, std::uint32_t> progress;  // per quest
};

// Walks an order from a start to its place, `step` yards a tick, the candidates seen every tick; a pick is worked
// `fightTicks` ticks (the bot stands), then credits its need once and is gone; the arrival within 10 yards of the place
Walked WalkOrder(QuestOrderRun& run, float x, float y, std::vector<X::Candidate> candidates, std::uint8_t level,
                 float step, int fightTicks)
{
    constexpr float ARRIVE_YARDS = 10.0f;  // the executor's QUEST_ARRIVE_YARDS
    std::uint32_t const base = 7001;
    run.startedAt = base;
    run.lastCount = base;
    for (QuestOrderOpen const& need : run.open)
        X::Note(run.marks, need.quest, 0);
    Walked walked;
    std::optional<std::size_t> held;
    int fighting = 0;
    for (int tick = 0; tick < 2000 && !walked.end; ++tick)
    {
        std::uint32_t const now = base + static_cast<std::uint32_t>(tick) * TICK_MS;
        float const left = std::hypot(run.order.x - x, run.order.y - y);
        if (!run.arrivedAt && left <= ARRIVE_YARDS)
            run.arrivedAt = X::Stamp(now);
        bool finding = fighting > 0;
        if (fighting && !--fighting)
        {
            QuestOrderOpen& need = run.open[candidates[*held].need];
            if (need.left)
            {
                --need.left;
                std::uint32_t const added = X::Note(run.marks, need.quest, ++walked.progress[need.quest]);
                (run.arrivedAt ? run.placeCredits : run.wayCredits) += added;
                run.lastCount = X::Stamp(now);
            }
            candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(*held));
            held.reset();
        }
        else if (!fighting)
        {
            X::PickAsk const ask{x, y, level, X::NEUTRAL_CAUTION, !run.arrivedAt};
            held = X::Pick(candidates, run.open, run.order, ask);
            if (held)
            {
                ++walked.picks;
                fighting = fightTicks;
                finding = true;
                run.lastFound = now;
            }
            else if (!run.arrivedAt)
            {
                float const move = std::min(step, left);
                x += (run.order.x - x) / left * move;
                y += (run.order.y - y) / left * move;
            }
        }
        walked.end = X::End(run, now, finding);
        walked.ticks = tick;
    }
    return walked;
}

// WQ20, the three matrix cases' shapes. way-kill: quest 183's boars in Coldridge Valley, a way of 256 yards with the
// allowance 60; a boar 45 yards off the line near the start (beyond grind's 30 yards, out of the place's field) is
// taken on the way: one way credit, the end arrived. way-far: Garrick Padfoot (quest 6, the one spawn of its entry)
// 70 yards off a way of 238 yards with the allowance 20, out of the place's field: never taken, arrived with no credit;
// a nearer Garrick-like target off the way is not held while one on the way is there. way-trip: quest 845's zhevras on
// a way of 449 yards in the Barrens, mounted (22 yards a tick), a minute's fight per pick: two way credits and the
// arrival well within the bound
void WayCases()
{
    Check(X::Detour(0.0f, 0.0f, 50.0f, 0.0f, 100.0f, 0.0f, 1.0f) &&
              !X::Detour(0.0f, 0.0f, 50.0f, 0.0f, 100.0f, 0.0f, 0.0f) &&
              !X::Detour(0.0f, 0.0f, 50.0f, 0.0f, 100.0f, 0.0f, -5.0f),
          "Detour: on the line taken; an allowance of 0 or less takes nothing, not even on the line");
    Check(X::Detour(0.0f, 0.0f, -10.0f, 0.0f, 100.0f, 0.0f, 20.0f) &&
              !X::Detour(0.0f, 0.0f, -10.5f, 0.0f, 100.0f, 0.0f, 20.0f) &&
              X::Detour(0.0f, 0.0f, 50.0f, 30.0f, 100.0f, 0.0f, 20.0f) &&
              !X::Detour(0.0f, 0.0f, 50.0f, 40.0f, 100.0f, 0.0f, 20.0f),
          "Detour: 10 yards back costs 20 (taken at 20, 10.5 back not); 30 yards off the middle of 100 costs 16.6, 40 "
          "costs 28");

    // way-kill
    float const killX = -6257.0f;
    float const killY = 678.1f;
    QuestOrder const kill = X::WayOrder(MAP_A, killX, killY, 60.0f);
    Check(kill.word == QuestOrderWord::Way && kill.end == QuestOrderEnd::Arrived && kill.expected.empty() &&
              kill.allowanceYards == 60.0f && kill.cell == FieldCellKeyAt(MAP_A, killX, killY) &&
              kill.minutes == X::WAY_MINUTES,
          "way-kill: a way order, nothing expected, the end its arrival");
    std::vector<X::Candidate> const boars = {{0, FieldKind::Creature, 4, MAP_A, -6228.7f, 451.3f},
                                             {0, FieldKind::Creature, 4, MAP_A, -6290.0f, 380.0f}};
    Check(
        !X::InField(kill.cell, MAP_A, boars[0].x, boars[0].y) && !X::InField(kill.cell, MAP_A, boars[1].x, boars[1].y),
        "way-kill: the boars lie out of the place's field (the allowance takes them, not the field)");
    QuestOrderRun killRun;
    killRun.order = kill;
    killRun.order.walkMinutes = 256.0f / 7.0f / 60.0f;
    killRun.open = {{183, 0, 12}};
    Walked const killed = WalkOrder(killRun, -6276.2f, 422.8f, boars, 5, 14.0f, 10);
    Unpacked const killRow = Unpack(X::RowAmount(killRun.placeCredits, killRun.wayCredits, 1, killRun.order.cell));
    Check(killed.end == QuestOrderEnd::Arrived && killed.picks == 1 && killRun.wayCredits == 1 &&
              killRun.placeCredits == 0 && X::QuestsCredited(killRun.marks) == 1 && killRow.wayCredits == 1 &&
              std::string(X::RowSource(kill.word, *killed.end)) == "way:arrived",
          "way-kill: the boar 45 yards off the line taken on the way, the one behind left; arrived with one way credit "
          "(" +
              Name(killed.end) + ", " + std::to_string(killed.picks) + " picks, " + std::to_string(killRun.wayCredits) +
              " way credits)");
    QuestOrderRun narrow;
    narrow.order = kill;
    narrow.order.allowanceYards = 20.0f;
    narrow.order.walkMinutes = killRun.order.walkMinutes;
    narrow.open = {{183, 0, 12}};
    Walked const left = WalkOrder(narrow, -6276.2f, 422.8f, boars, 5, 14.0f, 10);
    Check(left.end == QuestOrderEnd::Arrived && left.picks == 0 && narrow.wayCredits == 0,
          "way-kill with the allowance 20: the boar 45 yards off the line is left");
    std::vector<X::Candidate> line;
    for (int i = 0; i < 12; ++i)
    {
        float const t = 0.04f + 0.06f * static_cast<float>(i);
        line.push_back(
            {0, FieldKind::Creature, 4, MAP_A, -6276.2f + (killX + 6276.2f) * t, 422.8f + (killY - 422.8f) * t});
    }
    QuestOrderRun busy;
    busy.order = kill;
    busy.order.walkMinutes = killRun.order.walkMinutes;
    busy.open = {{183, 0, 12}};
    Walked const cleared = WalkOrder(busy, -6276.2f, 422.8f, line, 5, 14.0f, 30);
    float const walkBoundTicks = (X::WALK_SLACK * busy.order.walkMinutes + X::WAY_MINUTES) * 60.0f / 2.0f;
    Check(cleared.end == QuestOrderEnd::Arrived && cleared.picks == 12 && busy.wayCredits == 12 &&
              static_cast<float>(cleared.ticks) > walkBoundTicks,
          "way-kill busy: twelve boars on the line, a minute each, all taken on the way and then the arrival, past "
          "twice the walk plus the stay: each credit restarts the walk's clock (" +
              Name(cleared.end) + " at tick " + std::to_string(cleared.ticks) + ", " + std::to_string(cleared.picks) +
              " picks)");

    // way-far
    float const farX = -8886.7f;
    float const farY = -392.4f;
    QuestOrderRun farRun;
    farRun.order = X::WayOrder(MAP_A, farX, farY, 20.0f);
    farRun.order.walkMinutes = 238.0f / 7.0f / 60.0f;
    farRun.open = {{6, 0, 1}};
    X::Candidate const garrick{0, FieldKind::Creature, 5, MAP_A, -9056.5f, -460.9f};
    Check(!X::InField(farRun.order.cell, MAP_A, garrick.x, garrick.y), "way-far: Garrick out of the place's field");
    Walked const far = WalkOrder(farRun, -9124.9f, -389.3f, {garrick}, 6, 14.0f, 10);
    Check(far.end == QuestOrderEnd::Arrived && far.picks == 0 && farRun.wayCredits == 0 &&
              X::QuestsCredited(farRun.marks) == 0 &&
              std::string(X::RowSource(farRun.order.word, *far.end)) == "way:arrived",
          "way-far: Garrick 70 yards off the line never taken; arrived with no credit (" + Name(far.end) + ", " +
              std::to_string(far.picks) + " picks)");
    X::PickAsk const abreast{-9056.5f, -390.3f, 6, X::NEUTRAL_CAUTION, true};
    X::Candidate const ahead{0, FieldKind::Creature, 5, MAP_A, -8966.5f, -391.0f};
    std::vector<X::Candidate> const both = {garrick, ahead};
    std::optional<std::size_t> const held = X::Pick(both, farRun.open, farRun.order, abreast);
    Check(!X::OnOrder(farRun.order, abreast, MAP_A, garrick.x, garrick.y) &&
              X::OnOrder(farRun.order, abreast, MAP_A, ahead.x, ahead.y) && held == std::size_t{1},
          "way-far: abreast of Garrick (70 yards) a target 90 yards ahead on the line is held, not the nearer one off "
          "the way");
    X::PickAsk arrivedAsk = abreast;
    arrivedAsk.onWay = false;
    Check(X::OnOrder(farRun.order, abreast, MAP_A, -9010.0f, -391.0f) &&
              !X::InField(farRun.order.cell, MAP_A, -9010.0f, -391.0f) &&
              !X::OnOrder(farRun.order, arrivedAsk, MAP_A, -9010.0f, -391.0f) &&
              !X::OnOrder(farRun.order, abreast, MAP_B, -9010.0f, -391.0f),
          "OnOrder: a point on the line out of the field is on the way; after the arrival the allowance is gone; "
          "another map is never on the way");

    // way-trip
    QuestOrderRun trip;
    trip.order = X::WayOrder(MAP_B, 291.3f, -2745.6f, 60.0f);
    trip.order.walkMinutes = 449.0f / 7.0f / 60.0f;  // the plan walks at running speed
    trip.open = {{845, 0, 4}};
    std::vector<X::Candidate> const zhevras = {{0, FieldKind::Creature, 13, MAP_B, 144.4f, -3019.3f},
                                               {0, FieldKind::Creature, 13, MAP_B, 148.7f, -2931.0f},
                                               {0, FieldKind::Creature, 13, MAP_B, 49.9f, -3260.0f}};
    Walked const tripped = WalkOrder(trip, 49.9f, -3124.1f, zhevras, 40, 22.4f, 30);
    float const boundTicks = (X::WALK_SLACK * trip.order.walkMinutes + X::WAY_MINUTES) * 60.0f / 2.0f;
    Check(tripped.end == QuestOrderEnd::Arrived && tripped.picks == 2 && trip.wayCredits == 2 &&
              static_cast<float>(tripped.ticks) < boundTicks,
          "way-trip: two zhevras on the way taken, the one behind left; arrived within the bound after two minutes' "
          "fights (" +
              Name(tripped.end) + " at tick " + std::to_string(tripped.ticks) + ", " + std::to_string(tripped.picks) +
              " picks)");
}

// The bounds: an order that never arrives ends at twice its walk plus its stay; a replaced order ends replaced first;
// a way ends on its arrival; the stamps wrap
void Bounds()
{
    QuestOrderRun run;
    run.order.minutes = 3.0f;
    run.order.walkMinutes = 2.0f;
    run.order.expected.push_back({7, 0, 2});
    run.open.push_back({7, 0, 2});
    run.startedAt = 0xFFFFFFFFu - 30000;  // wraps during the walk
    std::uint32_t const deadline = run.startedAt + 7 * MS_PER_MINUTE;
    Check(!X::End(run, deadline - 1, false).has_value(), "walk bound: no end a millisecond before 2 x walk + stay");
    Check(X::End(run, deadline, false) == QuestOrderEnd::Time, "walk bound: time at 2 x walk + stay, across a wrap");
    run.lastCount = run.startedAt + 4 * MS_PER_MINUTE;  // a credit on the way, after the wrap
    Check(!X::End(run, deadline, false).has_value() &&
              !X::End(run, run.lastCount + 7 * MS_PER_MINUTE - 1, false).has_value() &&
              X::End(run, run.lastCount + 7 * MS_PER_MINUTE, false) == QuestOrderEnd::Time,
          "walk bound: a count on the way (a credit) restarts it");
    run.lastCount = 0;
    run.lastFound = run.startedAt + 4 * MS_PER_MINUTE;  // a candidate seen, no credit: sightings never end
    Check(X::End(run, deadline, false) == QuestOrderEnd::Time,
          "walk bound: a sighting without a credit does not restart it");
    run.lastFound = 0;
    run.arrivedAt = run.startedAt + MS_PER_MINUTE;
    Check(!X::End(run, run.arrivedAt + 3 * MS_PER_MINUTE - 1, true).has_value() &&
              X::End(run, run.arrivedAt + 3 * MS_PER_MINUTE, true) == QuestOrderEnd::Time,
          "stay: time when the stay has passed since the arrival");
    run.open[0].left = 0;
    run.replaced = true;
    Check(X::End(run, run.arrivedAt + 1, true) == QuestOrderEnd::Replaced, "replaced comes before done");
    run.replaced = false;
    Check(X::End(run, run.arrivedAt + 1, true) == QuestOrderEnd::Done, "done once the expected need is closed");
    QuestOrderRun way;
    way.order.word = QuestOrderWord::Way;
    way.order.end = QuestOrderEnd::Arrived;
    way.order.minutes = 5.0f;
    way.startedAt = 100;
    Check(!X::End(way, 200, false).has_value(), "way: no end on the way");
    way.arrivedAt = 300;
    Check(X::End(way, 300, false) == QuestOrderEnd::Arrived, "way: arrived at the place");
    Check(!X::End(way, 300, true).has_value() && X::End(way, 301, false) == QuestOrderEnd::Arrived &&
              X::End(way, 300 + 5 * MS_PER_MINUTE, true) == QuestOrderEnd::Time,
          "way: arrived holding a target, it ends once the target is let go; the stay bounds the hold");
    Check(X::Stamp(0) == 1 && X::Stamp(5) == 5, "a stamp is never 0 (not yet)");
    Unpacked const u = Unpack(X::RowAmount(5000, 3, 12, FieldCellKey(MAP_A, -112, 8)));
    Check(u.placeCredits == X::ROW_COUNT_MAX && u.wayCredits == 3 && u.minutes == 12 && u.cellX == -112 && u.cellY == 8,
          "row: credits capped, a negative cell kept");
    Check(X::RowAmount(4095, 4095, 4095, FieldCellKey(MAP_A, -512, -512)) > 0, "row: the amount is never negative");
}
}  // namespace

// WQ18b: an order of takes only (the pickup at a hub), the end that names the bags, the order's own end at the
// status door
void SwitchRules()
{
    constexpr std::uint32_t QUEST_A = 50;
    constexpr std::uint32_t QUEST_B = 51;
    QuestOrder order;
    order.word = QuestOrderWord::HandIn;
    order.end = QuestOrderEnd::Done;
    order.mapId = MAP_A;
    order.x = -8903.0f;
    order.y = -163.0f;
    order.cell = FieldCellKeyAt(MAP_A, order.x, order.y);
    order.expected = {{QUEST_A, TAKE, 1}, {QUEST_B, TAKE, 1}};
    order.walkMinutes = 1.0f;
    order.minutes = 5.0f;
    std::set<std::uint32_t> taken;
    auto const takeable = [&taken](std::uint32_t quest) { return taken.count(quest) == 0; };
    // The order's count of a log that holds none of its quests
    auto const counted = [&takeable](QuestOrderRun& run)
    {
        run.open.clear();
        X::OpenTakes(run.open, run.order, takeable);
    };
    {
        QuestOrderRun run;
        run.order = order;
        run.startedAt = run.lastCount = X::Stamp(1000);
        counted(run);
        bool const opened = run.open.size() == 2 && X::ExpectedOpen(order, run.open) && !X::End(run, 1000, false);
        X::OpenTakes(run.open, run.order, takeable);
        bool const once = run.open.size() == 2;
        taken.insert(QUEST_A);
        counted(run);
        bool const half = run.open.size() == 1 && !X::End(run, 3000, false);
        taken.insert(QUEST_B);
        counted(run);
        Check(opened && once && half && X::End(run, 5000, false) == QuestOrderEnd::Done,
              "take-only: an order that expects only takes is not done at its first tick; it is done once the last "
              "quest is taken (mutant: OpenTakes opens nothing)");
    }
    {
        taken.clear();
        QuestOrderRun dry;
        dry.order = order;
        dry.startedAt = dry.lastCount = X::Stamp(1000);
        dry.arrivedAt = X::Stamp(2000);
        counted(dry);
        Check(!X::End(dry, 2000 + X::LOOK_MS - 1, false) && X::End(dry, 2000 + X::LOOK_MS, false) == QuestOrderEnd::Dry,
              "take-only: a take that never comes ends dry after the look time at the place (its give-up: "
              "RefusedTakes)");
    }
    {
        std::vector<QuestOrderOpen> const counts = {{7, 0, 2}, {8, HAND_IN, 1}, {9, 1, 0}};
        taken = {QUEST_B};
        bool const byNeed =
            X::NeedOpen(counts, {QUEST_A, TAKE, 1}, takeable) && !X::NeedOpen(counts, {QUEST_B, TAKE, 1}, takeable) &&
            X::NeedOpen(counts, {7, 0, 2}, takeable) && X::NeedOpen(counts, {8, HAND_IN, 1}, takeable) &&
            !X::NeedOpen(counts, {9, 1, 1}, takeable) && !X::NeedOpen(counts, {7, 1, 1}, takeable);
        taken = {QUEST_A, QUEST_B};
        QuestOrderRun gone;
        gone.order = order;
        counted(gone);
        Check(byNeed && gone.open.empty() && !X::NeedOpen(counts, order.expected.front(), takeable),
              "need open: a take while its quest can be taken, any other need while the log has credits left; an "
              "order whose takes are all gone has nothing open, so the route does not start it");
    }

    auto const handIns = []
    {
        QuestOrderRun at;
        at.order.word = QuestOrderWord::HandIn;
        at.order.expected = {{60, HAND_IN, 1}, {61, HAND_IN, 1}, {62, HAND_IN, 1}};
        at.open = {{60, HAND_IN, 1}, {61, HAND_IN, 1}, {62, HAND_IN, 1}, {70, 0, 4}};
        return at;
    };
    {
        QuestOrderRun all = handIns();
        QuestOrderEnd const end = X::NameEnd(all, QuestOrderEnd::NoEnder, [](std::uint32_t) { return true; });
        Check(end == QuestOrderEnd::Bags && X::NoEnderQuests(all).empty() && all.open.back().left == 4 &&
                  X::OutcomeOf(end) == StatusEnd::Outcome::TimedOut &&
                  !Pursuit::WritesFailure(true, X::OutcomeOf(end), QuestOrderEndName(end)) &&
                  std::string(QuestOrderEndName(end)) == "bags" &&
                  std::string(X::RowSource(QuestOrderWord::Field, end)) == "field:bags" &&
                  std::string(X::RowSource(QuestOrderWord::HandIn, end)) == "hand_in:bags" &&
                  std::string(X::RowSource(QuestOrderWord::Way, end)) == "way:bags",
              "bags: every expected hand-in left is one the bags refuse: the order ends bags, by the clock, with no "
              "failure row for any quest (mutant: NameEnd keeps no_ender)");
        QuestOrderRun mixed = handIns();
        QuestOrderEnd const mixedEnd =
            X::NameEnd(mixed, QuestOrderEnd::NoEnder, [](std::uint32_t quest) { return quest == 61; });
        Check(mixedEnd == QuestOrderEnd::NoEnder && X::NoEnderQuests(mixed) == std::vector<std::uint32_t>{60, 62} &&
                  Pursuit::WritesFailure(true, X::OutcomeOf(mixedEnd), QuestOrderEndName(mixedEnd)),
              "bags: with a hand-in its ender failed among them the end stays no_ender, and only that one's quests "
              "get a failure row");
        QuestOrderRun none = handIns();
        bool const kept =
            X::NameEnd(none, QuestOrderEnd::NoEnder, [](std::uint32_t) { return false; }) == QuestOrderEnd::NoEnder &&
            X::NoEnderQuests(none).size() == 3;
        std::size_t passedOn = 0;
        for (QuestOrderEnd const other : {QuestOrderEnd::Done, QuestOrderEnd::Arrived, QuestOrderEnd::Time,
                                          QuestOrderEnd::Dry, QuestOrderEnd::Replaced})
        {
            QuestOrderRun run = handIns();
            passedOn += X::NameEnd(run, other, [](std::uint32_t) { return true; }) == other &&
                        X::NoEnderQuests(run).size() == 3;
        }
        Check(kept && passedOn == 5, "bags: no refusal keeps no_ender with its rows; any other end is named as found");
    }
    {
        std::size_t own = 0;
        for (QuestOrderEnd const end :
             {QuestOrderEnd::Done, QuestOrderEnd::Arrived, QuestOrderEnd::Time, QuestOrderEnd::Dry,
              QuestOrderEnd::Replaced, QuestOrderEnd::NoEnder, QuestOrderEnd::Bags})
            own += X::OwnEnd(QuestOrderEndName(end), X::OutcomeOf(end)) == end;
        Check(own == 7 && !X::OwnEnd(nullptr, StatusEnd::Outcome::Ordered) &&
                  !X::OwnEnd("order", StatusEnd::Outcome::Ordered) &&
                  !X::OwnEnd("dry", StatusEnd::Outcome::Interrupted) &&
                  !X::OwnEnd("replaced", StatusEnd::Outcome::Interrupted) &&
                  !X::OwnEnd("switch", StatusEnd::Outcome::Interrupted),
              "own end: each end's word with its outcome is the order's own end; a status taken over (an order, a "
              "switch, an interruption) is none");
    }
}

// WQ18b part 3 (fault F4): an order at NPCs. Its bounds last the look time longer and its stay restarts at each count
// of the log; it walks to the nearest spot of an expected need that is open, within its field
void NpcOrders()
{
    constexpr std::uint32_t STAY_MS = 15 * 1000;     // 0.25 min
    constexpr std::uint32_t WALK_MS = 2 * 6 * 1000;  // twice 0.1 min
    QuestOrderRun run;
    run.order.word = QuestOrderWord::HandIn;
    run.order.mapId = MAP_A;
    run.order.x = 150.0f;
    run.order.y = 150.0f;
    run.order.cell = FieldCellKeyAt(MAP_A, 150.0f, 150.0f);
    run.order.minutes = 0.25f;
    run.order.walkMinutes = 0.1f;
    run.order.expected = {{7, HAND_IN, 1}, {8, TAKE, 1}};
    run.open = {{7, HAND_IN, 1}, {8, TAKE, 1}};
    run.startedAt = run.lastCount = 1000;
    bool const walk = !X::End(run, 1000 + WALK_MS + STAY_MS + X::LOOK_MS - 1, false) &&
                      X::End(run, 1000 + WALK_MS + STAY_MS + X::LOOK_MS, false) == QuestOrderEnd::Time;
    run.arrivedAt = 5000;
    bool const look =
        !X::End(run, 5000 + X::LOOK_MS - 1, false) && X::End(run, 5000 + X::LOOK_MS, false) == QuestOrderEnd::NoEnder;
    bool const clock = !X::End(run, 5000 + STAY_MS + X::LOOK_MS - 1, true) &&
                       X::End(run, 5000 + STAY_MS + X::LOOK_MS, true) == QuestOrderEnd::Time;
    run.lastCount = run.lastFound = 40000;  // the hand-in came: the log is counted, the take is still open
    run.open[0].left = 0;
    bool const restarted = !X::End(run, 40000 + STAY_MS + X::LOOK_MS - 1, true) &&
                           X::End(run, 40000 + STAY_MS + X::LOOK_MS, true) == QuestOrderEnd::Time &&
                           X::End(run, 40000 + X::LOOK_MS, false) == QuestOrderEnd::Dry;
    QuestOrderRun field = run;
    field.order.word = QuestOrderWord::Field;
    field.open[0].left = 1;
    bool const planned = !X::End(field, 5000 + STAY_MS - 1, true) &&
                         X::End(field, 5000 + STAY_MS, true) == QuestOrderEnd::Time &&
                         X::End(field, 40000 + 1, true) == QuestOrderEnd::Time;
    Check(walk && look && clock && restarted && planned,
          "an order at NPCs: its walk's bound and its stay last the look time longer, so the look ends it (no_ender, "
          "dry) before the clock; a count of the log restarts its stay; a field's stay is the planned one from the "
          "arrival (mutants: End without NpcSlackMs, the stay from the arrival only)");

    run.open = {{7, HAND_IN, 1}, {8, TAKE, 1}};
    std::vector<X::NpcSpot> const spots = {{7, HAND_IN, 180.0f, 150.0f, 5.0f},
                                           {8, TAKE, 120.0f, 150.0f, 6.0f},
                                           {9, HAND_IN, 151.0f, 150.0f, 0.0f},  // no need the order expects
                                           {8, TAKE, 450.0f, 150.0f, 0.0f}};    // outside the field
    bool const nearest = X::NearestSpot(run.order, run.open, spots, 170.0f, 150.0f) == std::size_t{0} &&
                         X::NearestSpot(run.order, run.open, spots, 125.0f, 150.0f) == std::size_t{0} &&
                         X::NearestSpot(run.order, run.open, spots, 440.0f, 150.0f) == std::size_t{0};
    // part 4: a field order walks to its NPCs too and, with none open, to its place; an order at NPCs stays
    bool const fieldToo = X::NearestSpot(field.order, field.open, spots, 125.0f, 150.0f) == std::size_t{0};
    std::optional<X::OrderAim> const toEnder = X::AimOf(field.order, field.open, spots, 125.0f, 150.0f);
    field.open[0].left = field.open[1].left = 0;
    std::optional<X::OrderAim> const toPlace = X::AimOf(field.order, field.open, spots, 125.0f, 150.0f);
    run.open[0].left = 0;
    bool const next = X::NearestSpot(run.order, run.open, spots, 180.0f, 150.0f) == std::size_t{1};
    run.open[1].left = 0;
    bool const none = !X::NearestSpot(run.order, run.open, spots, 180.0f, 150.0f);
    bool const aims = toEnder && toEnder->npc && toEnder->x == 180.0f && toEnder->z == 5.0f && toPlace &&
                      !toPlace->npc && toPlace->x == 150.0f && toPlace->y == 150.0f &&
                      !X::AimOf(run.order, run.open, spots, 180.0f, 150.0f);
    std::vector<X::NpcSpot> const away = {{8, TAKE, 450.0f, 150.0f, 0.0f}};
    run.open = {{7, HAND_IN, 1}, {8, TAKE, 1}};
    bool const keepsPlace = !X::NearestSpot(run.order, run.open, away, 180.0f, 150.0f);
    Check(nearest && next && none && fieldToo && aims && keepsPlace,
          "an order walks to the nearest spot of an expected hand-in still open, within its field, then to its "
          "takes'; a need done sends it on to the next NPC; with none a field order goes to its place and an order at "
          "NPCs stays (mutants: NearestSpot for orders at NPCs only, the nearest spot whatever its need)");
}

// WQ18b part 4 (lab13, 2026-11-25: quest 313 ready, its ender 122 yards from the field order's place, beyond the giver
// search; the order ended no_ender twice and the quest was abandoned). The harness's own executor round an order's
// aim: each tick an NPC within reach that gives closes its need (the hand-in, the take), at the place the field's work
// earns a credit every creditTicks ticks (0: none), a change counts the log and asks AimOf again (a new aim is a new
// walk: the arrival is taken again), the bot steps to its aim and arrives within ten yards, and End speaks
constexpr float WALK_STEP = 14.0f;    // 7 yards a second, a tick of 2 s
constexpr float WALK_ARRIVE = 10.0f;  // the executor's arrival
constexpr float NPC_REACH = 20.0f;    // an NPC this near gives for certain (the game's search reaches farther)
constexpr std::uint32_t WALK_LIMIT_TICKS = 1800;  // an hour
constexpr std::size_t ENDER_WALKS = 600;

struct AimWalk
{
    std::optional<QuestOrderEnd> end;
    float x{0.0f};
    float y{0.0f};
    std::vector<std::uint32_t> handed;  // the quests handed in
    std::vector<std::uint32_t> rows;    // the quests a no_ender end gives up
};

template <typename Gives>
AimWalk WalkAims(QuestOrderRun& run, std::vector<X::NpcSpot> const& spots, Gives const& gives, float x, float y,
                 std::uint32_t creditTicks)
{
    AimWalk walk;
    std::uint32_t now = 1000;
    run.startedAt = run.lastCount = now;
    float toX = run.order.x;
    float toY = run.order.y;
    bool atNpc = false;
    auto const walkMinutes = [&]() { return static_cast<float>(Yards(x, y, toX, toY)) / 7.0f / 60.0f; };
    auto const aim = [&]()
    {
        std::optional<X::OrderAim> const next = X::AimOf(run.order, run.open, spots, x, y);
        if (!next)
            return;
        atNpc = next->npc;
        if (Yards(next->x, next->y, toX, toY) <= 1.0)
            return;
        toX = next->x;
        toY = next->y;
        run.arrivedAt = 0;
        run.order.walkMinutes = walkMinutes();
    };
    run.order.walkMinutes = walkMinutes();
    aim();
    for (std::uint32_t tick = 1; tick <= WALK_LIMIT_TICKS; ++tick)
    {
        now += TICK_MS;
        bool changed = false;
        bool finding = false;
        for (X::NpcSpot const& spot : spots)
            if (gives(spot) && Yards(x, y, spot.x, spot.y) <= NPC_REACH)
                for (QuestOrderOpen& need : run.open)
                    if (need.left && need.quest == spot.quest && need.objective == spot.objective)
                    {
                        need.left = 0;
                        if (spot.objective == HAND_IN)
                        {
                            ++run.handIns;
                            walk.handed.push_back(spot.quest);
                        }
                        else
                            ++run.takes;
                        changed = true;
                    }
        if (run.arrivedAt && !atNpc && creditTicks && tick % creditTicks == 0)
            for (QuestOrderOpen& need : run.open)
                if (need.left && need.objective != HAND_IN && need.objective != TAKE)
                {
                    --need.left;
                    ++run.placeCredits;
                    changed = finding = true;
                    break;
                }
        if (changed)
        {
            run.lastFound = run.lastCount = now;
            aim();
        }
        if (!run.arrivedAt && Yards(x, y, toX, toY) <= WALK_ARRIVE)
            run.arrivedAt = now;
        if (std::optional<QuestOrderEnd> const found = X::End(run, now, finding))
        {
            walk.end = X::NameEnd(
                run, *found, [](std::uint32_t) { return false; },
                [&](std::uint32_t quest) { return X::EnderElsewhere(run.order, spots, quest, x, y); });
            if (walk.end == QuestOrderEnd::NoEnder)
                walk.rows = X::NoEnderQuests(run);
            break;
        }
        if (!run.arrivedAt)
        {
            double const left = Yards(x, y, toX, toY);
            double const step = std::min<double>(WALK_STEP, left);
            x += static_cast<float>((toX - x) * step / left);
            y += static_cast<float>((toY - y) * step / left);
        }
    }
    walk.x = x;
    walk.y = y;
    return walk;
}

// Generated orders with one to three NPCs (enders and givers; nine in ten in the order's field, three in four giving),
// field orders with work at the place among them, each walked from up to 300 yards off
void EnderWalks(std::uint32_t seed)
{
    Gen gen{std::mt19937(seed ^ 0x51ED270Bu)};
    Invariant bounded("part 4: every walked order ends");
    Invariant atEnder(
        "part 4: a no_ender end gives up only a quest whose ender the bot stands at, or that has none in "
        "the order's field");
    Invariant handed(
        "part 4: with every ender of the field taking its quest, no such hand-in is left when the look "
        "ends the order");
    Invariant done("part 4: an order whose NPCs all stand in its field and give, with its field's credits, ends done");
    std::size_t noEnders = 0;
    std::size_t dones = 0;
    std::size_t fieldHandIns = 0;
    for (std::size_t at = 0; at < ENDER_WALKS; ++at)
    {
        QuestOrderRun run;
        QuestOrder& order = run.order;
        bool const field = gen.Chance(0.7f);
        std::int32_t const cellX = gen.Int(CELL_LOW + 2, CELL_HIGH - 2);
        std::int32_t const cellY = gen.Int(CELL_LOW + 2, CELL_HIGH - 2);
        order.word = field ? QuestOrderWord::Field : QuestOrderWord::HandIn;
        order.end = QuestOrderEnd::Done;
        order.mapId = MAP_A;
        order.x = (static_cast<float>(cellX) + (field ? gen.Real(0.05f, 0.95f) : 0.5f)) * 100.0f;
        order.y = (static_cast<float>(cellY) + (field ? gen.Real(0.05f, 0.95f) : 0.5f)) * 100.0f;
        order.cell = FieldCellKeyAt(MAP_A, order.x, order.y);
        order.minutes = field ? 5.0f : 0.25f;
        std::vector<X::NpcSpot> spots;
        std::set<std::uint32_t> giving;
        bool allGive = true;
        bool endersGive = true;
        int const npcs = gen.Int(1, 3);
        for (int n = 0; n < npcs; ++n)
        {
            std::uint32_t const quest = FIRST_QUEST + static_cast<std::uint32_t>(n);
            std::uint32_t const objective = gen.Chance(0.7f) ? HAND_IN : TAKE;
            bool const inField = gen.Chance(0.9f);
            bool const gives = gen.Chance(0.75f);
            // its spot: in one of the field's nine cells, or two cells off
            float const spotX =
                (static_cast<float>(cellX + (inField ? gen.Int(-1, 1) : 2)) + gen.Real(0.02f, 0.98f)) * 100.0f;
            float const spotY = (static_cast<float>(cellY + gen.Int(-1, 1)) + gen.Real(0.02f, 0.98f)) * 100.0f;
            order.expected.push_back({quest, objective, 1});
            run.open.push_back({quest, objective, 1});
            spots.push_back({quest, objective, spotX, spotY, 0.0f});
            if (gives)
                giving.insert(quest);
            allGive = allGive && inField && gives;
            endersGive = endersGive && (objective != HAND_IN || !inField || gives);
            fieldHandIns += field && inField && objective == HAND_IN ? 1 : 0;
        }
        std::uint32_t const credits = field && gen.Chance(0.8f) ? static_cast<std::uint32_t>(gen.Int(1, 5)) : 0;
        std::uint32_t const creditTicks = credits && gen.Chance(0.8f) ? static_cast<std::uint32_t>(gen.Int(2, 20)) : 0;
        if (credits)
        {
            order.expected.push_back({FIRST_QUEST + 9, 0, credits});
            run.open.push_back({FIRST_QUEST + 9, 0, credits});
        }
        float const x = order.x + gen.Real(-300.0f, 300.0f);
        float const y = order.y + gen.Real(-300.0f, 300.0f);
        AimWalk const walk = WalkAims(
            run, spots, [&giving](X::NpcSpot const& spot) { return giving.count(spot.quest) > 0; }, x, y, creditTicks);
        bounded.Note(walk.end.has_value(), at, "no end within an hour");
        if (!walk.end)
            continue;
        noEnders += *walk.end == QuestOrderEnd::NoEnder ? 1 : 0;
        dones += *walk.end == QuestOrderEnd::Done ? 1 : 0;
        for (std::uint32_t const quest : walk.rows)
        {
            bool inField = false;
            bool stoodAt = false;
            for (X::NpcSpot const& spot : spots)
                if (spot.quest == quest && spot.objective == HAND_IN && HarnessInField(order, MAP_A, spot.x, spot.y))
                {
                    inField = true;
                    stoodAt = stoodAt || Yards(walk.x, walk.y, spot.x, spot.y) <= NPC_REACH + 0.01;
                }
            atEnder.Note(!inField || stoodAt, at, "quest " + std::to_string(quest) + " given up away from its ender");
        }
        if (endersGive && (*walk.end == QuestOrderEnd::NoEnder || *walk.end == QuestOrderEnd::Dry))
            for (X::NpcSpot const& spot : spots)
                if (spot.objective == HAND_IN && HarnessInField(order, MAP_A, spot.x, spot.y))
                    handed.Note(std::find(walk.handed.begin(), walk.handed.end(), spot.quest) != walk.handed.end(), at,
                                "quest " + std::to_string(spot.quest) + " not handed in, end " + Name(walk.end));
        if (allGive && (!credits || creditTicks))
            done.Note(*walk.end == QuestOrderEnd::Done, at, Name(walk.end));
    }
    std::printf("part 4: walked orders=%zu, hand-ins in a field order's field=%zu, ends done %zu, no_ender %zu\n",
                ENDER_WALKS, fieldHandIns, dones, noEnders);
    Report(bounded);
    Report(atEnder);
    Report(handed);
    Report(done);
    Check(noEnders > 0 && dones > 0 && fieldHandIns > 0,
          "the walked orders end no_ender and done, field orders among them");
}

// part 4, the lab's case: a field order at -5470 -520 that expects quest 313's hand-in (its ender at -5473 -642, in the
// field, 122 yards from the place) and two credits of quest 317, the bot 285 yards north of the place
void FieldEnder()
{
    auto const make = []()
    {
        QuestOrderRun run;
        run.order.word = QuestOrderWord::Field;
        run.order.end = QuestOrderEnd::Done;
        run.order.mapId = MAP_A;
        run.order.x = -5470.0f;
        run.order.y = -520.0f;
        run.order.cell = FieldCellKeyAt(MAP_A, -5470.0f, -520.0f);
        run.order.minutes = 5.0f;
        run.order.expected = {{313, HAND_IN, 1}, {317, 0, 2}};
        run.open = {{313, HAND_IN, 1}, {317, 0, 2}};
        return run;
    };
    std::vector<X::NpcSpot> const spots = {{313, HAND_IN, -5473.0f, -642.0f, 400.0f}};
    QuestOrderRun taken = make();
    std::optional<X::OrderAim> const first = X::AimOf(taken.order, taken.open, spots, -5494.0f, -235.0f);
    AimWalk const handedIn = WalkAims(taken, spots, [](X::NpcSpot const&) { return true; }, -5494.0f, -235.0f, 10);
    Check(first && first->npc && first->y == -642.0f && handedIn.end == QuestOrderEnd::Done && taken.handIns == 1 &&
              taken.placeCredits == 2 && handedIn.rows.empty(),
          "part 4, the lab's case: a field order whose ready quest's ender stands 122 yards from its place walks to "
          "the ender first, hands in, works its field and ends done (mutant: NearestSpot for orders at NPCs only)");
    QuestOrderRun refused = make();
    AimWalk const failed = WalkAims(refused, spots, [](X::NpcSpot const&) { return false; }, -5494.0f, -235.0f, 10);
    Check(failed.end == QuestOrderEnd::NoEnder && failed.rows == std::vector<std::uint32_t>{313} &&
              Yards(failed.x, failed.y, -5473.0, -642.0) <= WALK_ARRIVE + 1.0,
          "part 4: an ender that does not take the quest: no_ender at the ender after the look time, its one row");

    // The end's name: the bot at quest 60's ender; quest 61's stands 100 yards off in the field, quest 62's outside it
    std::vector<X::NpcSpot> const enders = {{60, HAND_IN, -5470.0f, -520.0f, 0.0f},
                                            {61, HAND_IN, -5470.0f, -620.0f, 0.0f},
                                            {62, HAND_IN, -5470.0f, -820.0f, 0.0f}};
    auto const noBags = [](std::uint32_t) { return false; };
    QuestOrderRun three = make();
    three.order.expected = {{60, HAND_IN, 1}, {61, HAND_IN, 1}, {62, HAND_IN, 1}};
    three.open = {{60, HAND_IN, 1}, {61, HAND_IN, 1}, {62, HAND_IN, 1}};
    QuestOrderEnd const named =
        X::NameEnd(three, QuestOrderEnd::NoEnder, noBags, [&](std::uint32_t quest)
                   { return X::EnderElsewhere(three.order, enders, quest, -5465.0f, -520.0f); });
    bool const rows = named == QuestOrderEnd::NoEnder && X::NoEnderQuests(three) == std::vector<std::uint32_t>{60, 62};
    QuestOrderRun away = make();
    away.order.expected = {{61, HAND_IN, 1}};
    away.open = {{61, HAND_IN, 1}};
    bool const dry =
        X::NameEnd(away, QuestOrderEnd::NoEnder, noBags, [&](std::uint32_t quest)
                   { return X::EnderElsewhere(away.order, enders, quest, -5465.0f, -520.0f); }) == QuestOrderEnd::Dry &&
        X::NoEnderQuests(away).empty();
    Check(rows && dry,
          "part 4: a no_ender end names the hand-in whose ender the bot stands at and the one with no ender in the "
          "field, not the one whose ender stands elsewhere there; with only such left the end is dry with no row "
          "(mutant: NameEnd without the ender's place)");
}

// WQ18c (WQ.44 item 2): a planned take that ends without the accept is one give-up of its quest (cannot_take),
// written by one rule at the order's own end where the bot stands at the giver
void RefusedTakeCases()
{
    constexpr std::uint32_t QUEST_A = 80;
    constexpr std::uint32_t QUEST_B = 81;
    constexpr std::uint32_t QUEST_C = 82;  // a hand-in on the same order, its ender beside the givers
    constexpr float GIVER_X = -8903.0f;
    constexpr float GIVER_Y = -163.0f;
    constexpr std::uint32_t HORIZON_MS = 20 * 60 * 1000;  // past every bound of the orders below
    std::vector<X::NpcSpot> const spots = {{QUEST_A, TAKE, GIVER_X, GIVER_Y, 0.0f},
                                           {QUEST_B, TAKE, GIVER_X + 4.0f, GIVER_Y, 0.0f},
                                           {QUEST_C, HAND_IN, GIVER_X, GIVER_Y + 4.0f, 0.0f}};
    std::set<std::uint32_t> taken;   // in the log
    std::set<std::uint32_t> denied;  // the take test denies it now (a full log, grey, given up)
    float botX = GIVER_X + 2.0f;     // beside the givers
    float const botY = GIVER_Y;
    auto const takeable = [&taken](std::uint32_t quest) { return taken.count(quest) == 0; };
    auto const atGiver = [&spots, &botX, botY](std::uint32_t quest) { return X::AtGiver(spots, quest, botX, botY); };
    auto const mayTake = [&taken, &denied](std::uint32_t quest)
    { return taken.count(quest) == 0 && denied.count(quest) == 0; };
    auto const orderOf = [](QuestOrderWord word, std::vector<QuestOrderNeed> expected)
    {
        QuestOrder order;
        order.word = word;
        order.end = QuestOrderEnd::Done;
        order.mapId = MAP_A;
        order.x = GIVER_X;
        order.y = GIVER_Y;
        order.cell = FieldCellKeyAt(MAP_A, order.x, order.y);
        order.expected = std::move(expected);
        order.walkMinutes = 1.0f;
        order.minutes = 5.0f;
        return order;
    };
    // The order's run at its place, counted as the executor counts it: the log's needs (open), then the takes
    auto const runOf = [&takeable](QuestOrder const& order, std::vector<QuestOrderOpen> open)
    {
        QuestOrderRun run;
        run.order = order;
        run.startedAt = run.lastCount = X::Stamp(1000);
        run.arrivedAt = X::Stamp(2000);
        run.open = std::move(open);
        X::OpenTakes(run.open, run.order, takeable);
        return run;
    };
    // Ticks every TICK_MS with nothing found, as the executor's, until the order's end
    auto const endOf = [](QuestOrderRun const& run)
    {
        for (std::uint32_t now = 2000; now <= 2000 + HORIZON_MS; now += TICK_MS)
            if (std::optional<QuestOrderEnd> const end = X::End(run, now, false))
                return *end;
        return QuestOrderEnd::Replaced;
    };
    using Quests = std::vector<std::uint32_t>;
    auto const rows = [&atGiver, &mayTake](QuestOrderRun const& run, QuestOrderEnd end)
    { return X::RefusedTakes(run, end, atGiver, mayTake); };

    // C1: the giver reached, the quest not offered: the order ends dry after the look time, one row for the quest
    std::size_t c1 = 0;
    for (QuestOrderWord const word : {QuestOrderWord::HandIn, QuestOrderWord::Field})
    {
        QuestOrderRun const one = runOf(orderOf(word, {{QUEST_A, TAKE, 1}}), {});
        QuestOrderRun const two = runOf(orderOf(word, {{QUEST_A, TAKE, 1}, {QUEST_B, TAKE, 1}}), {});
        QuestOrderRun twice = runOf(orderOf(word, {{QUEST_A, TAKE, 1}, {QUEST_A, TAKE, 1}}), {});
        twice.open.push_back({QUEST_A, TAKE, 1});  // a count that listed it twice
        c1 += endOf(one) == QuestOrderEnd::Dry && rows(one, QuestOrderEnd::Dry) == Quests{QUEST_A} &&
              X::OutcomeOf(QuestOrderEnd::Dry) == StatusEnd::Outcome::TimedOut && endOf(two) == QuestOrderEnd::Dry &&
              rows(two, QuestOrderEnd::Dry) == (Quests{QUEST_A, QUEST_B}) && endOf(twice) == QuestOrderEnd::Dry &&
              rows(twice, QuestOrderEnd::Dry) == Quests{QUEST_A};
    }
    Check(c1 == 2,
          "refused take: a planned take whose giver is reached and does not offer the quest ends dry (an order at "
          "NPCs and a field order) with one cannot_take give-up per quest, two quests two, a quest listed twice one "
          "(mutants: no de-duplication; dry gives up no take)");

    // C2: a refused accept (the menu lists the quest, the game refuses it) leaves the take open, as a giver that does
    // not offer it: the same run, the same end, the same row. No second writer exists (NoteRefusedTake is gone)
    {
        QuestOrderRun const refused = runOf(orderOf(QuestOrderWord::HandIn, {{QUEST_A, TAKE, 1}}), {});
        Check(endOf(refused) == QuestOrderEnd::Dry && rows(refused, QuestOrderEnd::Dry) == Quests{QUEST_A},
              "refused take: a refused accept and a giver that offers nothing end alike, one give-up at the order's "
              "end");
    }

    // C3: no other end, and no other need, writes the row
    {
        QuestOrderRun const open = runOf(orderOf(QuestOrderWord::HandIn, {{QUEST_A, TAKE, 1}}), {});
        std::size_t none = 0;
        for (QuestOrderEnd const end :
             {QuestOrderEnd::Done, QuestOrderEnd::Arrived, QuestOrderEnd::Replaced, QuestOrderEnd::Bags})
            none += rows(open, end).empty() && !X::GivesUpTakes(end);
        QuestOrderRun walking = open;
        walking.arrivedAt = 0;
        bool const beforeArrival = rows(walking, QuestOrderEnd::Time).empty();
        bool const timeAtGiver = rows(open, QuestOrderEnd::Time) == Quests{QUEST_A};
        Check(none == 4 && beforeArrival && timeAtGiver,
              "refused take: done, arrived, replaced and bags write no row; time writes one at the giver after the "
              "arrival and none on the way (mutant: every end gives up its takes)");

        botX = GIVER_X + 200.0f;  // the field's place, away from the giver
        QuestOrderRun const field = runOf(orderOf(QuestOrderWord::Field, {{QUEST_A, TAKE, 1}}), {});
        bool const away = endOf(field) == QuestOrderEnd::Dry && rows(field, QuestOrderEnd::Dry).empty() &&
                          rows(field, QuestOrderEnd::Time).empty();
        botX = GIVER_X + 2.0f;
        taken = {QUEST_A};
        QuestOrderRun const tookA =
            runOf(orderOf(QuestOrderWord::HandIn, {{QUEST_A, TAKE, 1}, {QUEST_B, TAKE, 1}}), {});
        bool const tookOne = rows(tookA, QuestOrderEnd::Dry) == Quests{QUEST_B};
        QuestOrderRun stale = open;  // the take came in this tick, after the last count
        bool const staleTaken = rows(stale, QuestOrderEnd::Dry).empty();
        taken.clear();
        stale.open.front().left = 0;
        bool const closed = rows(stale, QuestOrderEnd::Dry).empty();
        denied = {QUEST_A};
        bool const deniedNow = rows(open, QuestOrderEnd::Dry).empty();
        denied.clear();
        Check(away && tookOne && staleTaken && closed && deniedNow,
              "refused take: a field order that ends away from the giver writes none; a take that came, a need closed "
              "and a take the take test now denies (a full log, grey, given up: no refusal of the giver) write none "
              "(mutants: RefusedTakes takes closed needs; mayTake not asked)");

        QuestOrderRun hub = runOf(orderOf(QuestOrderWord::HandIn, {{QUEST_C, HAND_IN, 1}, {QUEST_A, TAKE, 1}}),
                                  {{QUEST_C, HAND_IN, 1}});
        bool const noEnder =
            X::NameEnd(hub, QuestOrderEnd::NoEnder, [](std::uint32_t) { return false; }) == QuestOrderEnd::NoEnder &&
            X::NoEnderQuests(hub) == Quests{QUEST_C} && rows(hub, QuestOrderEnd::NoEnder) == Quests{QUEST_A};
        QuestOrderRun const handIn =
            runOf(orderOf(QuestOrderWord::HandIn, {{QUEST_C, HAND_IN, 1}}), {{QUEST_C, HAND_IN, 1}});
        bool const handInNone =
            rows(handIn, QuestOrderEnd::NoEnder).empty() && rows(handIn, QuestOrderEnd::Dry).empty() &&
            !X::AtGiver(spots, QUEST_C, GIVER_X, GIVER_Y + 4.0f) && X::AtGiver(spots, QUEST_A, GIVER_X, GIVER_Y);
        Check(noEnder && handInNone,
              "refused take: a no_ender end at the giver writes the take's row beside its hand-in's; a hand-in is "
              "never a take row, and its ender is no giver (mutant: RefusedTakes takes hand-ins)");
    }

    // C4: the door writes once per end: the ticks before the end write nothing, the end its rows once
    {
        QuestOrderRun const run = runOf(orderOf(QuestOrderWord::HandIn, {{QUEST_A, TAKE, 1}, {QUEST_B, TAKE, 1}}), {});
        std::size_t written = 0;
        std::size_t ends = 0;
        for (std::uint32_t now = 2000; now <= 2000 + HORIZON_MS && !ends; now += TICK_MS)
            if (std::optional<QuestOrderEnd> const end = X::End(run, now, false))
            {
                ++ends;
                written += rows(run, *end).size();
            }
        Check(ends == 1 && written == 2 && rows(run, QuestOrderEnd::Dry) == rows(run, QuestOrderEnd::Dry),
              "refused take: one end, one row per open take, the same rows however often the rule is asked");
    }
}

// WQ38: the stall sum against the harness's own count. Per quest: the ordered milliseconds since its last credit, each
// pause between two ordered ticks of the quest counted at most 5 s (a tick off the order counts nothing); a credit (its
// progress rose) zeroes the count; a row when the count reaches 10 minutes, then 20, 40 ... after that credit, with the
// count's whole minutes
struct StallModel
{
    std::uint32_t progress{0};
    std::uint32_t count{0};
    std::uint32_t next{0};
    std::uint32_t last{0};
    bool seen{false};
};

std::optional<std::uint32_t> ModelTick(StallModel& m, std::uint32_t progress, std::uint32_t now, std::uint32_t limit)
{
    constexpr std::uint32_t GAP = 5000;
    if (!m.next)
        m.next = limit;
    m.count += m.seen ? std::min(now - m.last, GAP) : 0;
    m.seen = true;
    m.last = now;
    bool const credit = progress > m.progress;
    m.progress = progress;
    if (credit)
    {
        m.count = 0;
        m.next = limit;
        return std::nullopt;
    }
    if (m.count < m.next)
        return std::nullopt;
    m.next *= 2;
    return m.count / Planner::QuestFailRows::MINUTE_MS;
}

void StallRows(std::uint32_t seed)
{
    namespace F = Planner::QuestFailRows;
    constexpr std::uint32_t QUEST = 7;
    constexpr std::uint32_t OTHER = 8;
    constexpr std::uint32_t START = 1000;
    std::uint32_t const limit = F::LimitMs(false);
    Check(limit == 10 * Planner::QuestFailRows::MINUTE_MS && F::LimitMs(true) == 2 * Planner::QuestFailRows::MINUTE_MS,
          "stall: the limit is 10 minutes, 2 under shortgiveup");
    // Ordered 2 s ticks without a credit: the first row at 10 minutes and not a tick before, then at 20 and 40
    {
        F::Sums sums;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> rows;  // tick, minutes
        for (std::uint32_t tick = 0; tick <= 45 * 30; ++tick)
            if (std::optional<F::Row> const row = F::Tick(sums, QUEST, 3, START + tick * TICK_MS, limit))
                rows.emplace_back(tick, row->minutes);
        Check(rows.size() == 3 && rows[0] == std::make_pair(300u, 10u) && rows[1] == std::make_pair(600u, 20u) &&
                  rows[2] == std::make_pair(1200u, 40u),
              "stall: rows at 10, 20 and 40 ordered minutes without a credit, none between");
    }
    // A credit at 9 minutes: no row at 10; the next 10 minutes after the credit
    {
        F::Sums sums;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> rows;
        for (std::uint32_t tick = 0; tick <= 30 * 30; ++tick)
        {
            std::uint32_t const progress = tick >= 270 ? 4 : 3;
            if (std::optional<F::Row> const row = F::Tick(sums, QUEST, progress, START + tick * TICK_MS, limit))
                rows.emplace_back(tick, row->minutes);
        }
        Check(rows.size() == 2 && rows[0] == std::make_pair(570u, 10u) && rows[1] == std::make_pair(870u, 20u),
              "stall: a credit at 9 minutes zeroes the sum: the row 10 minutes after it, then 20");
    }
    // A credit after the first row: the next row 10 minutes after the credit, not at the doubling
    {
        F::Sums sums;
        std::vector<std::uint32_t> ticks;
        for (std::uint32_t tick = 0; tick <= 25 * 30; ++tick)
            if (F::Tick(sums, QUEST, tick >= 400 ? 4 : 3, START + tick * TICK_MS, limit))
                ticks.push_back(tick);
        Check(ticks == std::vector<std::uint32_t>{300, 700}, "stall: a credit after a row starts the 10 minutes anew");
    }
    // Time off the order: 5 minutes ordered, 30 minutes on something else, then ordered again: the pause adds 5 s
    {
        F::Sums sums;
        std::uint32_t rows = 0;
        std::uint32_t now = START;
        for (std::uint32_t tick = 0; tick < 150; ++tick, now += TICK_MS)
            rows += F::Tick(sums, QUEST, 3, now, limit) ? 1 : 0;
        now += 30 * Planner::QuestFailRows::MINUTE_MS;
        std::uint32_t firstRow = 0;
        for (std::uint32_t tick = 0; tick < 160 && !firstRow; ++tick, now += TICK_MS)
            if (F::Tick(sums, QUEST, 3, now, limit))
                firstRow = tick + 1;
        // 149 x 2 s before the pause, 5 s for it, then 2 s a tick: 298 + 5 + 149 x 2 = 601 s at the 150th
        Check(rows == 0 && firstRow == 150, "stall: time off the order adds at most 5 s, row after " +
                                                std::to_string(firstRow) + " ticks back on it");
    }
    // Pauses over 5 s on the order (a fight, a rest): each adds 5 s
    {
        F::Sums sums;
        std::uint32_t now = START;
        std::uint32_t ticks = 0;
        bool row = false;
        for (; !row && ticks < 1000; now += 20 * 1000)
        {
            ++ticks;
            row = F::Tick(sums, QUEST, 3, now, limit).has_value();
        }
        Check(row && ticks == 121,
              "stall: 20 s pauses count 5 s each: the row at the 121st tick, it came at " + std::to_string(ticks));
    }
    // The shortened limit (shortgiveup): the row at 2 minutes
    {
        F::Sums sums;
        std::uint32_t at = 0;
        for (std::uint32_t tick = 0; tick <= 100 && !at; ++tick)
            if (F::Tick(sums, QUEST, 0, START + tick * TICK_MS, F::LimitMs(true)))
                at = tick;
        Check(at == 60, "stall: under shortgiveup the row at 2 minutes");
    }
    // Two quests count apart; a quest gone from the log is forgotten and starts at zero when it is back
    {
        F::Sums sums;
        for (std::uint32_t tick = 0; tick < 300; ++tick)
        {
            F::Tick(sums, QUEST, 0, START + tick * TICK_MS, limit);
            F::Tick(sums, OTHER, 0, START + tick * TICK_MS, limit);
        }
        F::KeepLogged(sums, [](std::uint32_t quest) { return quest != OTHER; });
        bool const kept = sums.quests.size() == 1 && sums.quests[0].quest == QUEST;
        bool const row = F::Tick(sums, QUEST, 0, START + 600 * TICK_MS, limit).has_value();
        bool const anew = !F::Tick(sums, OTHER, 0, START + 600 * TICK_MS, limit).has_value();
        Check(kept && row && anew, "stall: quests count apart; a quest gone from the log is forgotten");
    }
    // Two ticks in one millisecond (two objectives of one quest) add nothing the second time
    {
        F::Sums sums;
        std::uint32_t at = 0;
        for (std::uint32_t tick = 0; tick <= 400 && !at; ++tick)
        {
            std::uint32_t const now = START + tick * TICK_MS;
            bool const row = F::Tick(sums, QUEST, 0, now, limit).has_value();
            if (F::Tick(sums, QUEST, 0, now, limit) || row)
                at = tick;
        }
        Check(at == 300, "stall: a second tick in the same millisecond counts nothing");
    }
    // Generated timelines: ticks on and off orders, pauses, credits, quests leaving the log; the rule's rows are the
    // model's rows
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> roll(0, 999);
    std::size_t timelines = 0;
    std::size_t rowsSeen = 0;
    std::string firstBad;
    constexpr std::size_t TIMELINES = 300;
    for (std::size_t t = 0; t < TIMELINES && firstBad.empty(); ++t)
    {
        F::Sums sums;
        std::map<std::uint32_t, StallModel> model;
        std::map<std::uint32_t, std::uint32_t> progress;
        std::uint32_t const lim = F::LimitMs(roll(rng) < 200);
        std::uint32_t now = static_cast<std::uint32_t>(roll(rng)) * 1000u + 1u;
        for (std::size_t step = 0; step < 4000 && firstBad.empty(); ++step)
        {
            int const r = roll(rng);
            now += r < 900 ? TICK_MS : (r < 980 ? static_cast<std::uint32_t>(roll(rng)) * 60u : 600000u);
            std::uint32_t const quest = 1 + static_cast<std::uint32_t>(roll(rng) % 3);
            if (roll(rng) < 3)
                progress[quest] += 1;
            if (roll(rng) < 1)
            {
                progress.erase(quest);
                model.erase(quest);
                F::KeepLogged(sums, [quest](std::uint32_t q) { return q != quest; });
                continue;
            }
            std::optional<std::uint32_t> const want = ModelTick(model[quest], progress[quest], now, lim);
            std::optional<F::Row> const got = F::Tick(sums, quest, progress[quest], now, lim);
            bool const same =
                want.has_value() == got.has_value() && (!got || (got->quest == quest && got->minutes == *want));
            rowsSeen += got ? 1 : 0;
            if (!same)
                firstBad = " first wrong: timeline " + std::to_string(t) + " step " + std::to_string(step);
        }
        timelines += firstBad.empty() ? 1 : 0;
    }
    Check(timelines == TIMELINES && rowsSeen > 100,
          "stall: " + std::to_string(timelines) + " of 300 generated timelines give the model's rows (seed " +
              std::to_string(seed) + ", " + std::to_string(rowsSeen) + " rows)" + firstBad);
}

// WQ38: the abandon's done share and the two rows' amounts
// questing WQ33: a model of the order's look and hold over one field. The look takes the nearest target the order
// takes (X::Pick over the open needs) that is not one of the last four passed over (the executor's ring of
// QuestExec::PASSED_TARGETS); a target without a stand point is refused and passed over (the rules count it), one
// with a stand point is worked (its act clears the count) and credits its need (the recount carries the counts). The
// night lab5-20261125-175728 held one refused crate after another for hours: more of them than the ring holds
struct StandTarget
{
    std::uint32_t need{0};
    float yards{0.0f};
    bool stands{false};
    bool credits{true};  // worked: its act brings a credit (false: a loot without the item)
};

struct StandBound
{
    std::function<bool(std::span<QuestOrderOpen>, std::size_t)> refuse =
        [](std::span<QuestOrderOpen> open, std::size_t need) { return X::RefuseNeed(open, need); };
    std::function<void(std::span<QuestOrderOpen const>, std::span<QuestOrderOpen>)> carry =
        [](std::span<QuestOrderOpen const> before, std::span<QuestOrderOpen> after)
    { X::CarryRefusals(before, after); };
    std::function<void(std::uint8_t&)> worked = [](std::uint8_t& refusals) { X::StandWorked(refusals); };
};

struct StandSeen
{
    std::vector<int> holds;                 // per need: targets held
    std::vector<int> refused;               // per need: targets refused
    std::vector<int> heldClosed;            // per need: targets held after it closed
    std::vector<int> firstWorked;           // per need: refusals before its first worked target (-1: none worked)
    std::map<std::uint32_t, int> failures;  // per quest: failure rows
    std::vector<QuestOrderOpen> open;
    int looks{0};
};

constexpr int STAND_LOOKS = 200;  // the night's loop ran on: a bound far above the rule's three
constexpr std::size_t STAND_RING = 4;

StandSeen RunStand(std::vector<QuestOrderOpen> open, std::vector<StandTarget> const& targets, StandBound const& bound)
{
    QuestOrder order;
    order.mapId = MAP_A;
    order.x = 50.0f;
    order.y = 50.0f;
    order.cell = FieldCellKeyAt(MAP_A, order.x, order.y);
    X::PickAsk ask;
    ask.x = order.x;
    ask.y = order.y;
    ask.level = 70;
    StandSeen seen;
    std::size_t const needs = open.size();
    seen.holds.assign(needs, 0);
    seen.refused.assign(needs, 0);
    seen.heldClosed.assign(needs, 0);
    seen.firstWorked.assign(needs, -1);
    std::vector<bool> used(targets.size(), false);
    std::array<std::size_t, STAND_RING> ring;
    ring.fill(targets.size());
    std::size_t next = 0;
    for (; seen.looks < STAND_LOOKS; ++seen.looks)
    {
        std::vector<X::Candidate> candidates;
        std::vector<std::size_t> of;
        for (std::size_t at = 0; at < targets.size(); ++at)
            if (!used[at] && std::find(ring.begin(), ring.end(), at) == ring.end())
            {
                candidates.push_back(
                    {targets[at].need, FieldKind::Object, 0, MAP_A, order.x + targets[at].yards, order.y});
                of.push_back(at);
            }
        std::optional<std::size_t> const pick = X::Pick(candidates, open, order, ask);
        if (!pick)
            break;
        std::size_t const at = of[*pick];
        std::uint32_t const need = targets[at].need;
        ++seen.holds[need];
        if (X::StandClosed(open[need].refusals))
            ++seen.heldClosed[need];
        if (!targets[at].stands)
        {
            ++seen.refused[need];
            ring[next] = at;
            next = (next + 1) % STAND_RING;
            if (bound.refuse(open, need))
                ++seen.failures[open[need].quest];
            continue;
        }
        if (seen.firstWorked[need] < 0)
            seen.firstWorked[need] = seen.refused[need];
        bound.worked(open[need].refusals);
        used[at] = true;
        std::vector<QuestOrderOpen> const before = open;
        if (open[need].left && targets[at].credits)
            --open[need].left;
        // the recount after a credit: the log's needs counted again, the counts carried by the rule
        std::vector<QuestOrderOpen> recount = open;
        for (QuestOrderOpen& fresh : recount)
            fresh.refusals = 0;
        bound.carry(before, recount);
        open = recount;
    }
    seen.open = open;
    return seen;
}

// The invariant over the cases; `report` false: the cases are only judged (a mutant's run)
bool StandCases(StandBound const& bound, bool report)
{
    bool all = true;
    auto const check = [&](bool ok, std::string const& what)
    {
        all = all && ok;
        if (report)
            Check(ok, what);
    };
    // Six crates without a stand point (more than the ring holds) and, farther, a reachable target of a second quest
    std::vector<StandTarget> crates;
    for (int i = 0; i < 6; ++i)
        crates.push_back({0, 10.0f + 8.0f * static_cast<float>(i), false});
    std::vector<StandTarget> field = crates;
    field.push_back({1, 95.0f, true});
    StandSeen lost = RunStand({{1, 0, 10}, {2, 0, 1}}, field, bound);
    check(lost.refused[0] == X::STAND_REFUSALS, "WQ33: six refused crates: three refusals, then the need is closed");
    check(X::StandClosed(lost.open[0].refusals), "WQ33: the crates' need is closed");
    check(lost.heldClosed[0] == 0, "WQ33: no crate is held after the need closed");
    check(lost.failures[1] == 1 && lost.failures.size() == 1, "WQ33: exactly one no_stand failure row, its quest's");
    check(lost.open[1].left == 0, "WQ33: the order's other need goes on and is met");
    check(lost.looks < STAND_LOOKS, "WQ33: the look runs out of targets (the order's own end follows)");
    // A reachable crate behind two refused ones is taken before the bound, and its act clears the count (it brings no
    // credit: the act alone clears it)
    std::vector<StandTarget> const mixed = {{0, 10.0f, false}, {0, 18.0f, false}, {0, 26.0f, true, false},
                                            {0, 34.0f, false}, {0, 42.0f, false}, {0, 50.0f, false},
                                            {0, 58.0f, false}};
    StandSeen found = RunStand({{1, 0, 10}}, mixed, bound);
    check(found.firstWorked[0] == 2, "WQ33: the reachable crate is taken after two refusals, before the bound");
    check(found.refused[0] == 2 + X::STAND_REFUSALS, "WQ33: its act cleared the count: three more refusals close it");
    check(found.failures[1] == 1, "WQ33: one failure row after the later three");
    // Two objectives of one quest, both without a stand point: both close, the quest's failure row once
    std::vector<StandTarget> two = crates;
    for (int i = 0; i < 6; ++i)
        two.push_back({1, 12.0f + 8.0f * static_cast<float>(i), false});
    StandSeen both = RunStand({{1, 0, 10}, {1, 1, 5}}, two, bound);
    check(X::StandClosed(both.open[0].refusals) && X::StandClosed(both.open[1].refusals),
          "WQ33: both objectives of the quest close");
    check(both.failures[1] == 1, "WQ33: one failure row for the quest, not one per objective");
    // A credit of the need between refusals (another target counted) starts the count again
    std::vector<QuestOrderOpen> const before = {{1, 0, 10, 2}};
    std::vector<QuestOrderOpen> credited = {{1, 0, 9, 0}};
    std::vector<QuestOrderOpen> same = {{1, 0, 10, 0}};
    bound.carry(before, credited);
    bound.carry(before, same);
    check(credited[0].refusals == 0, "WQ33: a credit clears the need's count");
    check(same[0].refusals == 2, "WQ33: a recount without a credit keeps it");
    return all;
}

void StandBounds()
{
    StandCases(StandBound{}, true);
    StandBound none;  // today's code before WQ33: a refusal only passes the target over
    none.refuse = [](std::span<QuestOrderOpen>, std::size_t) { return false; };
    Check(!StandCases(none, false), "WQ33: mutant without the bound (the night's endless hold) is caught");
    StandBound forgets;
    forgets.carry = [](std::span<QuestOrderOpen const>, std::span<QuestOrderOpen>) {};
    Check(!StandCases(forgets, false), "WQ33: mutant that forgets the counts at a recount is caught");
    StandBound keeps;
    keeps.carry = [](std::span<QuestOrderOpen const> before, std::span<QuestOrderOpen> after)
    {
        for (std::size_t at = 0; at < after.size() && at < before.size(); ++at)
            after[at].refusals = before[at].refusals;
    };
    Check(!StandCases(keeps, false), "WQ33: mutant that keeps the count over a credit is caught");
    StandBound perQuest;
    perQuest.refuse = [](std::span<QuestOrderOpen> open, std::size_t need)
    { return X::StandRefused(open[need].refusals); };
    Check(!StandCases(perQuest, false), "WQ33: mutant writing a failure row per objective is caught");
    StandBound lasting;
    lasting.worked = [](std::uint8_t&) {};
    Check(!StandCases(lasting, false), "WQ33: mutant whose act keeps the count is caught");
}

void ShareAndAmounts()
{
    namespace F = Planner::QuestFailRows;
    using C = F::Credit;
    auto const share = [](std::vector<C> const& credits, bool complete)
    { return F::DoneShare(std::span<C const>(credits), complete); };
    Check(share({{3, 8}}, false) == 37 && share({{8, 8}, {0, 4}}, false) == 66 && share({{12, 8}, {0, 8}}, false) == 50,
          "share: credits done over credits required, each capped at its requirement, rounded down");
    Check(share({{8, 8}, {1, 1}}, true) == 100 && share({{0, 8}}, true) == 100 && share({}, true) == 0 &&
              share({}, false) == 0 && share({{0, 0}, {5, 0}}, false) == 0,
          "share: a complete quest with credits 100, a quest without any 0");
    Check(share({{7, 8}}, false) < 100 && share({{8, 8}}, false) == 100, "share: 100 only with every credit done");
    for (std::uint32_t const level : {0u, 1u, 12u, 83u})
        for (std::uint32_t const done : {0u, 1u, 50u, 99u, 100u})
        {
            std::int64_t const amount = F::AbandonAmount(level, done);
            Check(amount >= 1000 && amount / 1000 - 1 == done && amount % 1000 == level,
                  "abandon amount: share " + std::to_string(done) + " level " + std::to_string(level) + " reads back");
        }
    Check(F::AbandonAmount(12, std::nullopt) == 12 && F::AbandonAmount(83, std::nullopt) < 1000 &&
              F::AbandonAmount(12, 0) != F::AbandonAmount(12, std::nullopt) && F::AbandonAmount(12, 250) == 101012,
          "abandon amount: no share is the bare level (under 1000, as the old rows), a share over 100 counts 100");
    auto const signed10 = [](std::int64_t bits) { return bits >= 512 ? bits - 1024 : bits; };
    for (std::int32_t const x : {-91, 0, 5, 511, -512})
        for (std::uint32_t const minutes : {2u, 10u, 40u, 5000u})
        {
            std::uint64_t const cell = FieldCellKey(0, x, -x / 2);
            std::int64_t const amount = X::StallRowAmount(minutes, cell);
            bool const back = (amount & 0xFFF) == std::min<std::int64_t>(minutes, 0xFFF) &&
                              signed10((amount >> 12) & 0x3FF) == x && signed10((amount >> 22) & 0x3FF) == -x / 2;
            Check(amount >= 0 && back,
                  "stall amount: minutes " + std::to_string(minutes) + " cell " + std::to_string(x) + " reads back");
        }
}

int main(int argc, char** argv)
{
    std::uint32_t seed = DEFAULT_SEED;
    std::size_t show = NO_ORDER;
    if (argc > 1)
        seed = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    if (argc > 2)
        show = static_cast<std::size_t>(std::strtoul(argv[2], nullptr, 10));
    std::printf("quest-field-exec selftest (replay: <seed> [order to trace])\n");
    GeneratedOrders(seed, show);
    FieldAbove();
    FieldTwo();
    FieldDry();
    HandIns();
    WayCases();
    Bounds();
    SwitchRules();
    NpcOrders();
    EnderWalks(seed);
    FieldEnder();
    RefusedTakeCases();
    StallRows(seed);
    ShareAndAmounts();
    StandBounds();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
