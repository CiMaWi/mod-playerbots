/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the executor on an order (questing WQ19a, QuestFieldExec.h over QuestOrder.h's QuestOrderRun):
// generated orders and logs over made-up maps (the seed is printed; argv[1] replays it, argv[2] traces that order),
// each ticked every 2 s with generated candidates and credits and checked against the step's invariants by the
// harness's own rules, plus hand-built cases shaped as the three matrix cases (field-two, field-dry, field-above).
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

bool HarnessTakes(X::Candidate const& candidate, std::vector<QuestOrderOpen> const& open, QuestOrder const& order,
                  int botLevel)
{
    return HarnessOpen(open, candidate.need) && HarnessReach(candidate, botLevel) &&
           HarnessInField(order, candidate.mapId, candidate.x, candidate.y);
}

bool HarnessExpectedOpen(QuestOrder const& order, std::vector<QuestOrderOpen> const& open)
{
    for (QuestOrderNeed const& expected : order.expected)
        for (QuestOrderOpen const& need : open)
            if (need.quest == expected.quest && need.objective == expected.objective && need.left > 0)
                return true;
    return false;
}

// Whether a need is one the harness expects of a field: an open objective (no hand-in, no take) with a source that has
// a row in one of the nine cells
bool HarnessExpects(FieldMap const& map, Need const& need, std::uint32_t mapId, float x, float y)
{
    if (need.objective == HAND_IN || need.objective == TAKE || !need.left)
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
    Invariant noFailure{"an order's end is never Abandoned (no failure row), each end its outcome"};
    Invariant credits{"the row's credits and quests are the rises of the log's progress"};
    Invariant row{"the row's amount unpacks to its credits, minutes and cell; its source to word and end"};
    std::size_t withExpected{0};
    std::size_t ends[5]{};  // by QuestOrderEnd
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

// One generated order ticked to its end; show traces it
void RunOrder(Gen& gen, World const& w, Invariants& inv, std::size_t at, bool show)
{
    QuestOrder order = X::FieldOrder(w.map, w.needs, w.mapId, w.x, w.y, gen.Real(0.5f, 20.0f));
    order.walkMinutes = gen.Chance(0.3f) ? 0.0f : gen.Real(0.1f, 6.0f);
    if (gen.Chance(0.1f))
        order.end = QuestOrderEnd::Arrived;
    CheckExpect(w, order, inv, at);
    inv.withExpected += order.expected.empty() ? 0 : 1;

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
    std::map<std::uint32_t, std::uint32_t> progress;  // per quest
    std::map<std::uint32_t, std::uint32_t> gained;
    int lastFound = NEVER;
    if (show)
        std::printf("order %zu: map %u (%.1f, %.1f) level %d, %zu needs, %zu expected, %.2f min, walk %.2f min, %s\n",
                    at, w.mapId, w.x, w.y, w.level, w.needs.size(), order.expected.size(), order.minutes,
                    order.walkMinutes, arrival == NEVER ? "never arrives" : "arrives");
    std::optional<QuestOrderEnd> end;
    int k = 0;
    for (; k <= cap && !end; ++k)
    {
        std::uint32_t const now = base + static_cast<std::uint32_t>(k) * TICK_MS;
        bool const arrived = arrival != NEVER && k >= arrival;
        if (arrived && !run.arrivedAt)
            run.arrivedAt = X::Stamp(now);
        X::PickAsk ask;
        ask.x = arrived ? w.x + gen.Real(-40.0f, 40.0f) : w.x + 400.0f;
        ask.y = arrived ? w.y + gen.Real(-40.0f, 40.0f) : w.y;
        ask.level = static_cast<std::uint8_t>(w.level);
        std::vector<X::Candidate> candidates(static_cast<std::size_t>(gen.Int(0, arrived ? 5 : 2)));
        for (X::Candidate& candidate : candidates)
        {
            candidate.need = static_cast<std::uint32_t>(gen.Int(0, static_cast<int>(run.open.size())));
            candidate.kind = gen.Chance(0.25f) ? FieldKind::Object : FieldKind::Creature;
            candidate.level = candidate.kind == FieldKind::Object
                                  ? 0
                                  : static_cast<std::uint8_t>(std::clamp(w.level + gen.Int(-5, 12), 1, 83));
            candidate.mapId = gen.Chance(0.95f) ? w.mapId : MAP_B + 7;
            candidate.x = w.x + gen.Real(-260.0f, 260.0f);
            candidate.y = w.y + gen.Real(-260.0f, 260.0f);
        }
        std::optional<std::size_t> const pick = X::Pick(candidates, run.open, run.order, ask);
        std::optional<std::size_t> nearest;
        float nearestYards = 0.0f;
        for (std::size_t c = 0; c < candidates.size(); ++c)
        {
            if (!HarnessTakes(candidates[c], run.open, order, w.level))
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
            inv.pickField.Note(HarnessInField(order, picked.mapId, picked.x, picked.y), at, "out of the field");
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

        end = X::End(run, now, working);
        bool const expectedOpen = HarnessExpectedOpen(order, run.open);
        if (show)
            std::printf("  tick %d: %zu candidates, pick %s, %s\n", k, candidates.size(),
                        pick ? std::to_string(*pick).c_str() : "none", Name(end).c_str());
        if (end == QuestOrderEnd::Done)
            inv.neverDone.Note(!expectedOpen, at, "tick " + std::to_string(k));
        if (end == QuestOrderEnd::Dry)
            inv.neverDry.Note(!nearest, at, "tick " + std::to_string(k));
        if (order.end == QuestOrderEnd::Done && !order.expected.empty() && !expectedOpen && !run.replaced)
            inv.doneLive.Note(end == QuestOrderEnd::Done, at, "tick " + std::to_string(k) + ": " + Name(end));
        int const quietFrom = std::max(arrival, lastFound);
        if (arrived && !nearest && (k - quietFrom) * static_cast<int>(TICK_MS) >= static_cast<int>(X::LOOK_MS))
            inv.dryLive.Note(end.has_value(), at, "tick " + std::to_string(k));
        if (arrived && static_cast<double>(k - arrival) * TICK_MS >= stayMs)
            inv.timeLive.Note(end.has_value(), at, "tick " + std::to_string(k));
    }
    inv.bounded.Note(end.has_value() && static_cast<double>(k - 1) * TICK_MS <= boundMs, at,
                     "tick " + std::to_string(k - 1) + " of bound " + std::to_string(boundMs) + " ms");
    if (!end)
        return;
    ++inv.ends[static_cast<std::size_t>(*end)];
    inv.noFailure.Note(
        X::OutcomeOf(*end) != StatusEnd::Outcome::Abandoned && X::OutcomeOf(*end) == ExpectedOutcome(*end), at,
        Name(end));
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
    Invariants inv;
    for (std::size_t at = 0; at < ORDERS; ++at)
    {
        World const w = Generate(gen);
        RunOrder(gen, w, inv, at, at == show);
    }
    std::printf(
        "seed=%u orders=%zu with expected needs=%zu ends: done %zu, arrived %zu, time %zu, dry %zu, "
        "replaced %zu\n",
        seed, ORDERS, inv.withExpected, inv.ends[0], inv.ends[1], inv.ends[2], inv.ends[3], inv.ends[4]);
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
}

// Ticks an order at its place with the given candidates every tick (none: an empty place) until it ends; returns the
// end and the seconds after the arrival
std::pair<std::optional<QuestOrderEnd>, std::uint32_t> TickAtPlace(QuestOrderRun& run,
                                                                   std::vector<X::Candidate> const& candidates,
                                                                   X::PickAsk const& ask, std::uint32_t limitSeconds)
{
    std::uint32_t const base = 5000;
    run.startedAt = base;
    run.arrivedAt = base;
    for (std::uint32_t s = 0; s <= limitSeconds; s += TICK_MS / 1000)
    {
        std::uint32_t const now = base + s * 1000;
        std::optional<std::size_t> const pick = X::Pick(candidates, run.open, run.order, ask);
        if (pick)
            run.lastFound = now;
        if (std::optional<QuestOrderEnd> const end = X::End(run, now, pick.has_value()))
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
// cell (-91, -4); a ready quest's hand-in there is not expected (WQ19b's). The order works both and ends done with
// credits of two quests
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
    spawns.push_back(MakeSpawn(FieldKind::Creature, 197, MAP_A, x + 30.0f, y + 30.0f, 10, 10));  // an ender
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
    Bounds();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
