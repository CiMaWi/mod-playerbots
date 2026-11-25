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
// Build (Git Bash, module root):
//   clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot /I src/Mgr/Travel
//       tests/quest-field-exec-selftest.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
