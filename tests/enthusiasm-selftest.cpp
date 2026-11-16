/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the enthusiasm rules (src/Ai/World/Pvp/Enthusiasm.h): every row of the modifier table against
// the functions, the scale's bounds under random edits, reading against stepping, the trait extremes, the engaged
// bound, the local balance and the start's cost (a simulated starter in and outside the start-cost share, the entering
// floor after a start at every trait corner). Build (Git Bash, module root; MSYS_NO_PATHCONV=1 keeps /I):
//   clang-cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I src/Ai/World/Pvp tests/enthusiasm-selftest.cpp
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Enthusiasm.h"

using namespace WorldPvp;

static_assert(std::is_trivially_copyable_v<Enthusiasm> && std::is_trivially_copyable_v<EnthusiasmValues> &&
                  std::is_trivially_copyable_v<JoinFacts> && std::is_trivially_copyable_v<FleeFacts> &&
                  std::is_trivially_copyable_v<BanFacts> && std::is_trivially_copyable_v<BalanceEntry>,
              "rule inputs are plain values");

namespace
{
constexpr uint32_t SECOND = 1000;
constexpr uint32_t MINUTE = 60 * SECOND;
constexpr uint32_t HOUR = 60 * MINUTE;

int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
    {
        ++passed;
        return;
    }
    ++failed;
    std::printf("FAILED: %s\n", what.c_str());
}

bool Near(double a, double b, double tolerance = 1e-4) { return std::fabs(a - b) <= tolerance; }

std::string Number(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}

EnthusiasmValues Defaults() { return FromConfig(WorldPvpConfig{}); }

// A fixed-seed generator, so a failure replays
struct Lcg
{
    uint32_t state;
    uint32_t Next()
    {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    float Unit() { return static_cast<float>(Next() >> 8) / static_cast<float>(1u << 24); }
    float Between(float low, float high) { return low + (high - low) * Unit(); }
};

// One input for every category at once; the neutral one has every row off. Helpfulness -1 is neutral: it acts only
// through the faction row, and adds nothing there at -1
struct Probe
{
    EnthusiasmValues values = Defaults();
    JoinFacts join;
    FleeFacts flee;
    float depletionBalance = EVEN_BALANCE;
    EditCause cause = EditCause::None;
    float confidence = 0.0f;
    BanFacts ban;
};

Probe Neutral()
{
    Probe probe;
    probe.join.helpfulness = -1.0f;
    return probe;
}

// Each category's result: recovery and depletion as points per hour and minute, the thresholds, the edit, the ban
struct Results
{
    std::array<double, 5> numbers;  // index: ModifierCategory, Recovery..OneOffEdit
    BanReason ban;
};

Results Evaluate(Probe const& probe)
{
    EnthusiasmValues const& v = probe.values;
    Results results{};
    results.numbers[static_cast<size_t>(ModifierCategory::Recovery)] = Read({0.0, 0}, HOUR, false, 1.0f, v);
    float const factor = BalanceDepletionFactor(probe.depletionBalance, v);
    results.numbers[static_cast<size_t>(ModifierCategory::Depletion)] =
        v.max - Read({v.max, 0}, MINUTE, true, factor, v);
    results.numbers[static_cast<size_t>(ModifierCategory::JoinThreshold)] = JoinThreshold(probe.join, v);
    results.numbers[static_cast<size_t>(ModifierCategory::FleeThreshold)] = FleeThreshold(probe.flee, v);
    results.numbers[static_cast<size_t>(ModifierCategory::OneOffEdit)] = Edit(probe.cause, probe.confidence, v);
    results.ban = Banned(probe.ban);
    return results;
}

// A row's case: the input without it, the input with it, and the ban it gives
struct RowCase
{
    Probe without = Neutral();
    Probe with = Neutral();
    BanReason ban = BanReason::None;
};

RowCase CaseFor(Modifier row)
{
    RowCase c;
    EnthusiasmValues const& v = c.with.values;
    switch (row)
    {
        case Modifier::RecoveryRate:
            c.without.values.recoveryPerHour = 0.0f;
            break;
        case Modifier::DepletionRate:
            c.without.values.depletionPerMinute = 0.0f;
            break;
        case Modifier::DepletionBehind:
            c.with.depletionBalance = v.balanceBehind;
            break;
        case Modifier::DepletionLead:
            c.with.depletionBalance = v.balanceLead;
            break;
        case Modifier::JoinBase:
            c.without.values.joinBase = 0.0f;
            break;
        case Modifier::JoinAggression:
            c.with.join.aggression = 1.0f;
            break;
        case Modifier::JoinAggressionOngoing:
            c.without.join.targetAttacksFaction = true;
            c.with.join.targetAttacksFaction = true;
            c.with.join.aggression = 1.0f;
            break;
        case Modifier::JoinGreen:
            c.with.join.colour = Colour::Green;
            break;
        case Modifier::JoinFaction:
            c.with.join.targetAttacksFaction = true;
            break;
        case Modifier::JoinHelpfulness:
            c.without.join.targetAttacksFaction = true;
            c.with.join.targetAttacksFaction = true;
            c.with.join.helpfulness = 1.0f;
            break;
        case Modifier::JoinGroup:
            c.with.join.targetAttacksGroup = true;
            break;
        case Modifier::JoinGraveyard:
            c.with.join.nearGraveyard = true;
            break;
        case Modifier::JoinBehind:
            c.with.join.balance = v.balanceBehind;
            break;
        case Modifier::JoinLead:
            c.with.join.balance = v.balanceLead;
            break;
        case Modifier::FleeBase:
            c.without.values.fleeBase = 0.0f;
            break;
        case Modifier::FleeCaution:
            c.with.flee.caution = 1.0f;
            break;
        case Modifier::FleeGroupEngaged:
            c.with.flee.groupMemberEngaged = true;
            break;
        case Modifier::Death:
            c.with.cause = EditCause::Death;
            break;
        case Modifier::DeathConfident:
            c.with.cause = EditCause::Death;
            c.with.confidence = 1.0f;
            break;
        case Modifier::DeathUnconfident:
            c.with.cause = EditCause::Death;
            c.with.confidence = -1.0f;
            break;
        case Modifier::Kill:
            c.with.cause = EditCause::Kill;
            break;
        case Modifier::GroupDeath:
            c.with.cause = EditCause::GroupDeath;
            break;
        case Modifier::Start:
            c.with.cause = EditCause::Start;
            break;
        case Modifier::BanRed:
            c.with.ban.colour = Colour::Red;
            c.ban = BanReason::Red;
            break;
        case Modifier::BanGrey:
            c.with.ban.colour = Colour::Grey;
            c.ban = BanReason::Grey;
            break;
        case Modifier::BanZone:
            c.with.ban.prohibitedZone = true;
            c.ban = BanReason::ProhibitedZone;
            break;
        case Modifier::BanTransport:
            c.with.ban.onTransport = true;
            c.ban = BanReason::Transport;
            break;
        case Modifier::BanInstance:
            c.with.ban.inInstance = true;
            c.ban = BanReason::Instance;
            break;
        case Modifier::BanRealMaster:
            c.with.ban.realMaster = true;
            c.ban = BanReason::RealMaster;
            break;
        default:
            break;
    }
    return c;
}

void PrintTable()
{
    std::printf("Modifier table (defaults):\n");
    for (ModifierRow const& row : ModifierTable(Defaults()))
        std::printf("  %-15s %-6s %8.3g  %s\n", Word(row.category), Word(row.form), row.value, row.name);
}

// Exit 1: every row changes its category's result as its form says, and no other
void TestRowsIsolated()
{
    std::array<ModifierRow, MODIFIER_COUNT> const table = ModifierTable(Defaults());
    for (size_t i = 0; i < table.size(); ++i)
    {
        ModifierRow const& row = table[i];
        RowCase const c = CaseFor(static_cast<Modifier>(i));
        Results const without = Evaluate(c.without);
        Results const with = Evaluate(c.with);
        std::string problem;
        for (size_t k = 0; k < with.numbers.size(); ++k)
        {
            double const before = without.numbers[k];
            double const after = with.numbers[k];
            std::string const word = Word(static_cast<ModifierCategory>(k));
            if (k != static_cast<size_t>(row.category))
            {
                if (!Near(before, after))
                    problem += " changes " + word + " " + Number(before) + " -> " + Number(after) + ";";
                continue;
            }
            bool const asForm = row.form == ModifierForm::Factor ? before > 0.0 && Near(after / before, row.value)
                                                                 : Near(after - before, row.value);
            if (!asForm || row.form == ModifierForm::Ban)
                problem += " " + word + " " + Number(before) + " -> " + Number(after) + ", not by its " +
                           Word(row.form) + " " + Number(row.value) + ";";
        }
        if (row.category == ModifierCategory::Never)
        {
            if (without.ban != BanReason::None || with.ban != c.ban || c.ban == BanReason::None)
                problem += std::string(" ban ") + Word(without.ban) + " -> " + Word(with.ban) + ";";
        }
        else if (with.ban != without.ban)
            problem += std::string(" changes the ban to ") + Word(with.ban) + ";";
        if (row.form == ModifierForm::Ban && row.category != ModifierCategory::Never)
            problem += " a ban outside never;";
        Check(problem.empty(),
              "row '" + std::string(row.name) + "' (" + Word(row.category) + ", " + Word(row.form) + "):" + problem);
    }
    Check(table.size() == MODIFIER_COUNT, "the table has one row per modifier");

    // The two bans with an exception, and red, which has none
    BanFacts greyAttacker;
    greyAttacker.colour = Colour::Grey;
    greyAttacker.targetAttacksFaction = true;
    BanFacts redAttacker;
    redAttacker.colour = Colour::Red;
    redAttacker.targetAttacksFaction = true;
    BanFacts mastersFoe;
    mastersFoe.realMaster = true;
    mastersFoe.mastersFight = true;
    Check(Banned(greyAttacker) == BanReason::None && Banned(redAttacker) == BanReason::Red &&
              Banned(mastersFoe) == BanReason::None,
          std::string("ban exceptions: a grey target that attacks the bot's faction (") + Word(Banned(greyAttacker)) +
              ") and a real master's fight (" + Word(Banned(mastersFoe)) + ") are allowed, a red one never (" +
              Word(Banned(redAttacker)) + ")");

    // The join side: the master is in the bot's group, so its fight takes the group term off the faction's threshold
    EnthusiasmValues const v = Defaults();
    JoinFacts mastersFaction;
    mastersFaction.targetAttacksFaction = true;
    JoinFacts mastersGroup = mastersFaction;
    mastersGroup.targetAttacksGroup = true;
    float const factionJoin = JoinThreshold(mastersFaction, v);
    float const groupJoin = JoinThreshold(mastersGroup, v);
    Check(Banned(mastersFoe) == BanReason::None && Near(v.joinGroupAttacked, 50.0) &&
              Near(factionJoin - groupJoin, v.joinGroupAttacked),
          "a real master's fight: joined at " + Number(groupJoin) + ", the faction's " + Number(factionJoin) +
              " less the group term " + Number(v.joinGroupAttacked));
}

// Exit 2: the value stays within the scale under any sequence of edits, reads and time jumps
void TestScaleBounded()
{
    EnthusiasmValues extreme = Defaults();
    extreme.recoveryPerHour = 1000.0f;
    extreme.depletionPerMinute = 1000.0f;
    extreme.deathEdit = 1000.0f;
    extreme.deathEditConfident = 0.0f;
    extreme.deathEditUnconfident = 1000.0f;
    extreme.killEdit = 1000.0f;
    extreme.groupDeathShare = 1.0f;
    for (EnthusiasmValues const& v : {Defaults(), extreme})
    {
        Lcg random{20261114u};
        uint32_t now = 0xFFFF0000u;  // crosses the ms clock's wrap
        Enthusiasm state{StartValue(random.Next(), v), now};
        bool engaged = false;
        float factor = 1.0f;
        std::string problem;
        auto const inScale = [&v](double value) { return value >= 0.0 && value <= v.max; };
        for (int step = 0; step < 5000 && problem.empty(); ++step)
        {
            switch (random.Next() % 6)
            {
                case 0:  // a time jump, from nothing to weeks
                    now += (random.Next() % 4 == 0) ? random.Next() : random.Next() % (3 * HOUR);
                    break;
                case 1:
                    state = Settle(state, now, engaged, factor, v);
                    engaged = !engaged;
                    break;
                case 2:
                    state = Settle(state, now, engaged, factor, v);
                    factor = random.Between(0.0f, 5.0f);
                    break;
                case 3:
                {
                    EditCause const cause = static_cast<EditCause>(random.Next() % 4);
                    state = Apply(state, now, engaged, factor, cause, random.Between(-2.0f, 2.0f), v);
                    break;
                }
                case 4:  // the extreme edits, many in a row
                {
                    constexpr std::array<EditCause, 3> CAUSES = {EditCause::Death, EditCause::Kill, EditCause::Start};
                    EditCause const cause = CAUSES[random.Next() % CAUSES.size()];
                    for (int i = 0; i < 50; ++i)
                        state = Apply(state, now, engaged, factor, cause, -1.0f, v);
                    break;
                }
                default:
                    break;
            }
            double const read = Read(state, now, engaged, factor, v);
            if (!inScale(state.value) || !inScale(read))
                problem = " step " + std::to_string(step) + ": value " + Number(state.value) + ", read " + Number(read);
        }
        Check(problem.empty(), std::string("the value stays within 0..") + Number(v.max) +
                                   (v.killEdit > 100.0f ? " (extreme values)" : " (defaults)") + problem);
    }

    // An edit settles first, then applies, then holds to the scale
    EnthusiasmValues const v = Defaults();
    double const killed = Apply({50.0, 0}, HOUR, false, 1.0f, EditCause::Kill, 0.0f, v).value;
    double const died = Apply({50.0, 0}, MINUTE, true, 1.0f, EditCause::Death, 0.0f, v).value;
    double const lost = Apply({90.0, 0}, 0, false, 1.0f, EditCause::GroupDeath, -1.0f, v).value;
    double const topped = Apply({95.0, 0}, 0, false, 1.0f, EditCause::Kill, 0.0f, v).value;
    Check(Near(killed, 75.0) && Near(died, 0.0) && Near(lost, 67.5) && Near(topped, 100.0),
          "edits settle first: kill after 1 h 50 -> " + Number(killed) + " (75), death after 1 min engaged 50 -> " +
              Number(died) + " (0), group death unconfident 90 -> " + Number(lost) + " (67.5), kill at 95 -> " +
              Number(topped) + " (100)");
}

// Exit 3: reading the value after a time gives the same result as settling at every step through it
void TestLazyEqualsStepped()
{
    EnthusiasmValues const v = Defaults();
    struct Stepping
    {
        uint32_t stepMs;  // 0: random steps of 1 ms to 2 min
        uint32_t spanMs;
    };
    std::array<Stepping, 4> const steppings = {{{1, 3 * MINUTE}, {997, 6 * HOUR}, {MINUTE, 6 * HOUR}, {0, 6 * HOUR}}};
    std::array<float, 3> const factors = {v.depletionFactorLead, 1.0f, v.depletionFactorBehind};
    std::array<double, 4> const starts = {0.0, 37.25, v.max - 0.5, v.max};
    for (Stepping const& stepping : steppings)
    {
        std::string problem;
        for (bool engaged : {false, true})
            for (float factor : factors)
                for (double start : starts)
                {
                    Enthusiasm const first{start, 0xFFFF0000u};
                    double const lazy = Read(first, first.atMs + stepping.spanMs, engaged, factor, v);
                    Enthusiasm stepped = first;
                    Lcg random{7u};
                    for (uint32_t done = 0; done < stepping.spanMs;)
                    {
                        uint32_t const step = stepping.stepMs ? stepping.stepMs : 1 + random.Next() % (2 * MINUTE);
                        done += std::min(step, stepping.spanMs - done);
                        stepped = Settle(stepped, first.atMs + done, engaged, factor, v);
                    }
                    if (!Near(stepped.value, lazy, 1e-6))
                        problem += std::string(" ") + (engaged ? "engaged" : "recovering") + " factor " +
                                   Number(factor) + " from " + Number(start) + ": stepped " + Number(stepped.value) +
                                   ", read " + Number(lazy) + ";";
                }
        Check(problem.empty(), "reading equals stepping in " +
                                   (stepping.stepMs ? Number(stepping.stepMs) + " ms steps" : "random steps") +
                                   " over " + Number(stepping.spanMs / 1000.0) + " s" + problem);
    }
}

// A join threshold worked out from the table's numbers, apart from JoinThreshold: the colour, helpfulness, balance and
// graveyard rows added to `base`. The balance lean is 1 at balance <= 0.25 and -1 at >= 0.75, linear between
float ExpectedThreshold(float base, JoinFacts const& facts)
{
    float const lean = facts.balance >= 0.5f ? -std::min(1.0f, (facts.balance - 0.5f) / 0.25f)
                                             : std::min(1.0f, (0.5f - facts.balance) / 0.25f);
    float threshold = base + 30.0f * lean;
    if (facts.colour == Colour::Green)
        threshold += 20.0f;
    if (facts.targetAttacksFaction)
        threshold -= 30.0f + 15.0f * (facts.helpfulness + 1.0f) * 0.5f;
    if (facts.targetAttacksGroup)
        threshold -= 50.0f;
    if (facts.nearGraveyard)
        threshold += 40.0f;
    return threshold;
}

// Exit 4: a bot at Aggression -1 never starts a fight but joins one (the user, 2026-11-14); one at Caution -1 never
// falls below its flee threshold
void TestTraitExtremes()
{
    EnthusiasmValues const v = Defaults();
    std::string problem;
    std::string joinProblem;
    int joins = 0;
    int joining = 0;
    for (int colour = 0; colour < 5; ++colour)
        for (int flags = 0; flags < 8; ++flags)
            for (float helpfulness : {-1.0f, 0.0f, 1.0f})
                for (int b = 0; b <= 20; ++b)
                {
                    JoinFacts facts;
                    facts.aggression = -1.0f;
                    facts.helpfulness = helpfulness;
                    facts.colour = static_cast<Colour>(colour);
                    facts.targetAttacksFaction = (flags & 1) != 0;
                    facts.targetAttacksGroup = (flags & 2) != 0;
                    facts.nearGraveyard = (flags & 4) != 0;
                    facts.balance = 0.05f * static_cast<float>(b);
                    float const threshold = JoinThreshold(facts, v);
                    std::string const where = " threshold " + Number(threshold) + " (colour " + Word(facts.colour) +
                                              ", flags " + std::to_string(flags) + ", balance " +
                                              Number(facts.balance) + ");";
                    if ((flags & 3) == 0)
                    {
                        // Starting: the threshold is the scale's top, whatever else is on
                        if (MayJoin(v.max, threshold) || !Near(threshold, 100.0))
                            problem += where;
                        continue;
                    }
                    // Joining: the Aggression term at a quarter, base 70, the other rows as they are
                    float const expected = ExpectedThreshold(70.0f, facts);
                    if (!Near(threshold, expected))
                        joinProblem += where + " expected " + Number(expected) + ";";
                    ++joining;
                    joins += MayJoin(v.max, threshold) ? 1 : 0;
                }
    Check(problem.empty(), "Aggression -1 never starts a fight: no colour, balance or flag lowers 100" + problem);
    Check(joinProblem.empty() && joins > 0, "Aggression -1 joins a fight: base 70, then the faction and group rows (" +
                                                std::to_string(joins) + " of " + std::to_string(joining) +
                                                " joining situations pass at full enthusiasm)" + joinProblem);

    // The two fights it is for, in the ordinary situation (yellow target, even balance, away from graveyards)
    JoinFacts groupFight;
    groupFight.aggression = -1.0f;
    groupFight.targetAttacksGroup = true;
    JoinFacts factionFight;
    factionFight.aggression = -1.0f;
    factionFight.helpfulness = 0.0f;
    factionFight.targetAttacksFaction = true;
    JoinFacts startFight;
    startFight.aggression = -1.0f;
    float const inGroup = JoinThreshold(groupFight, v);
    float const inFaction = JoinThreshold(factionFight, v);
    Check(Near(inGroup, 20.0) && inGroup < v.max && MayJoin(25.0, inGroup) && !MayJoin(15.0, inGroup) &&
              Near(inFaction, 32.5) && inFaction < v.max && Near(JoinThreshold(startFight, v), 100.0),
          "Aggression -1: a group member's fight 20, a faction player's fight 32.5 (neutral Helpfulness), starting " +
              Number(JoinThreshold(startFight, v)) + "; got " + Number(inGroup) + " and " + Number(inFaction));
    Check(LowestJoinThreshold(-1.0f, 1.0f, v) < v.max && Near(LowestJoinThreshold(-1.0f, 1.0f, v), -55.0),
          "the scan's lowest threshold for Aggression -1 is its joining one, -55, not the top: " +
              Number(LowestJoinThreshold(-1.0f, 1.0f, v)));

    problem.clear();
    for (bool group : {false, true})
    {
        float const threshold = FleeThreshold({-1.0f, group}, v);
        for (int i = 0; i <= 200; ++i)
            if (MustLeave(v.max * i / 200.0, threshold))
                problem += " value " + Number(v.max * i / 200.0) + " below " + Number(threshold) + ";";
        // The lowest value the rules give: 0 after any drain
        if (MustLeave(Read({v.max, 0}, 10 * HOUR, true, v.depletionFactorBehind, v), threshold))
            problem += " drained to 0 and below " + Number(threshold) + ";";
    }
    Check(problem.empty(),
          "Caution -1 never falls below its flee threshold (-5, -20 with a group member engaged)" + problem);

    // The ends a trait gives, and the peaceable rule only at the very top
    JoinFacts eager;
    eager.aggression = 1.0f;
    JoinFacts mild;
    mild.aggression = -0.99f;
    mild.targetAttacksGroup = true;
    Check(Near(JoinThreshold(eager, v), 20.0) && Near(FleeThreshold({1.0f, false}, v), 35.0) &&
              Near(FleeThreshold({-1.0f, false}, v), -5.0) && JoinThreshold(mild, v) < 60.0f,
          "trait ends: join 20 at Aggression +1, flee 35 and -5 at Caution +1 and -1, Aggression -0.99 helps a group");

    // Both checks are strict: a value at its threshold neither joins nor leaves. A neutral bot beside an engaged
    // group member has a flee threshold of 0, so it stays at 0
    float const held = FleeThreshold({0.0f, true}, v);
    Check(!MayJoin(60.0, 60.0f) && MayJoin(60.001, 60.0f) && !MustLeave(15.0, 15.0f) && MustLeave(14.999, 15.0f) &&
              Near(held, 0.0) && !MustLeave(0.0, held),
          "strict checks: at the threshold a bot neither joins nor leaves; flee threshold " + Number(held) +
              " with a group member engaged holds a neutral bot at 0");
}

// The weakened Aggression term applies when the target already attacks the bot's faction or a group member, and in no
// other situation: the threshold's slope in Aggression is -40 per +1 when starting (colour, graveyard and balance
// make no difference) and -10 when joining (either row, both rows)
void TestAggressionWhenJoining()
{
    EnthusiasmValues const v = Defaults();
    std::string problem;
    for (int colour = 0; colour < 5; ++colour)
        for (int flags = 0; flags < 8; ++flags)
            for (int b = 0; b <= 20; ++b)
            {
                JoinFacts facts;
                facts.colour = static_cast<Colour>(colour);
                facts.targetAttacksFaction = (flags & 1) != 0;
                facts.targetAttacksGroup = (flags & 2) != 0;
                facts.nearGraveyard = (flags & 4) != 0;
                facts.balance = 0.05f * static_cast<float>(b);
                float const none = JoinThreshold(facts, v);
                bool const ongoing = (flags & 3) != 0;
                for (float aggression : {-0.5f, 0.25f, 1.0f})
                {
                    facts.aggression = aggression;
                    float const slope = (JoinThreshold(facts, v) - none) / aggression;
                    if (!Near(slope, ongoing ? -10.0 : -40.0))
                        problem += " slope " + Number(slope) + " (flags " + std::to_string(flags) + ", colour " +
                                   std::to_string(colour) + ", balance " + Number(facts.balance) + ");";
                }
            }
    Check(problem.empty(),
          "the Aggression term is -40 per +1 when starting and -10 when joining, nowhere else" + problem);

    // The factor is the option's: 1 gives the full term when joining, 0 none, and starting never reads it
    JoinFacts joining;
    joining.targetAttacksGroup = true;
    joining.aggression = 0.5f;
    JoinFacts starting;
    starting.aggression = 0.5f;
    EnthusiasmValues full = v;
    full.joinAggressionFactor = 1.0f;
    EnthusiasmValues none = v;
    none.joinAggressionFactor = 0.0f;
    Check(Near(JoinThreshold(joining, full), 60.0 - 20.0 - 50.0) && Near(JoinThreshold(joining, none), 60.0 - 50.0) &&
              Near(JoinThreshold(starting, full), 40.0) && Near(JoinThreshold(starting, none), 40.0),
          "factor 1: the full term when joining (-10), factor 0: none (10); starting reads neither (40)");

    // The scale-top rule is for starting: with the full term a joining Aggression -1 bot sits at 100 before the rows,
    // and they still lower it
    JoinFacts peaceable = joining;
    peaceable.aggression = -1.0f;
    JoinFacts peaceableStart = starting;
    peaceableStart.aggression = -1.0f;
    Check(Near(JoinThreshold(peaceable, full), 50.0) && Near(JoinThreshold(peaceableStart, full), 100.0),
          "factor 1, Aggression -1: base 100 does not stop the rows when joining (50), starting stays at 100");

    // FromConfig takes the factor from the config and holds it to 0..1
    WorldPvpConfig config;
    Check(Near(FromConfig(config).joinAggressionFactor, 0.25), "the factor's default is 0.25");
    config.Enthusiasm.AggressionJoinFactor = 7.0f;
    Check(Near(FromConfig(config).joinAggressionFactor, 1.0), "the factor is held to 1");

    // The values are built from the config alone (once per load): a changed config gives its rates
    WorldPvpConfig changed;
    changed.Enthusiasm.RecoveryPerHour = 33.0f;
    changed.Enthusiasm.DepletionPerMinute = 7.5f;
    Check(Near(FromConfig(WorldPvpConfig{}).recoveryPerHour, 15.0) && Near(FromConfig(changed).recoveryPerHour, 33.0) &&
              Near(FromConfig(changed).depletionPerMinute, 7.5) &&
              Near(FromConfig(WorldPvpConfig{}).recoveryPerHour, 15.0),
          "a config's rates reach its values, the default config's stay");
}

// The scan's floor: no situation gives a lower threshold than LowestJoinThreshold
void TestScanFloor()
{
    EnthusiasmValues const v = Defaults();
    std::string problem;
    for (float aggression : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f})
        for (float helpfulness : {-1.0f, 0.0f, 1.0f})
        {
            float const lowest = LowestJoinThreshold(aggression, helpfulness, v);
            for (int colour = 0; colour < 5; ++colour)
                for (int flags = 0; flags < 8; ++flags)
                    for (int b = 0; b <= 20; ++b)
                    {
                        JoinFacts facts{
                            aggression,       helpfulness,      static_cast<Colour>(colour),  (flags & 1) != 0,
                            (flags & 2) != 0, (flags & 4) != 0, 0.05f * static_cast<float>(b)};
                        if (JoinThreshold(facts, v) < lowest)
                            problem += " aggression " + Number(aggression) + " below " + Number(lowest) + ";";
                    }
        }
    Check(problem.empty(), "no situation lies below the scan's lowest threshold" + problem);
}

// Exit 5: an engaged bot with no kills reaches its flee threshold, or 0, within a bounded time for every balance
void TestEngagedBounded()
{
    EnthusiasmValues const v = Defaults();
    uint32_t const bound = EngagedBoundMs(v);
    Check(bound == 10 * MINUTE,
          "the bound is 100 points at 20 x 0.5 per minute: 10 min (got " + Number(bound / 1000.0) + " s)");
    std::vector<float> factors;
    for (int b = 0; b <= 20; ++b)
        factors.push_back(BalanceDepletionFactor(0.05f * static_cast<float>(b), v));
    factors.push_back(v.depletionFactorLead);
    factors.push_back(v.depletionFactorBehind);
    for (float factor : factors)
    {
        std::string problem;
        for (float caution : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f})
            for (bool group : {false, true})
            {
                float const threshold = FleeThreshold({caution, group}, v);
                double const value = Read({v.max, 0}, bound, true, factor, v);
                if (value > 0.0 && !MustLeave(value, threshold))
                    problem += " caution " + Number(caution) + ": " + Number(value) + " after the bound;";
            }
        Check(problem.empty(), "from the top, factor " + Number(factor) + ": below the flee threshold or 0 within " +
                                   Number(bound / 60000.0) + " min" + problem);
    }
    // The plan's two times, for the record: a neutral bot from its join threshold (60) to its flee threshold (15) in
    // an even fight, and from 0 back to 60 out of fights
    double const fightMinutes = (60.0 - 15.0) / v.depletionPerMinute;
    double const recoveryHours = 60.0 / v.recoveryPerHour;
    std::printf("  neutral bot: 60 -> 15 in %.2f min of an even fight; 0 -> 60 in %.1f h of play\n", fightMinutes,
                recoveryHours);
    Check(Near(Read({60.0, 0}, static_cast<uint32_t>(fightMinutes * MINUTE), true, 1.0f, v), 15.0, 1e-3) &&
              Near(Read({0.0, 0}, static_cast<uint32_t>(recoveryHours * HOUR), false, 1.0f, v), 60.0, 1e-3),
          "the plan's times: about 2 minutes of an even fight, about 4 h of play");
}

// Exit 6: the local balance is 0.5 for mirrored sides, and a red enemy moves it more than a yellow one
void TestBalance()
{
    EnthusiasmValues const v = Defaults();
    Check(LocalBalance({}, v) == EVEN_BALANCE, "nobody counts: 0.5");

    Lcg random{42u};
    std::string problem;
    for (int list = 0; list < 200; ++list)
    {
        std::vector<BalanceEntry> entries;
        uint32_t const pairs = 1 + random.Next() % 8;
        for (uint32_t i = 0; i < pairs; ++i)
        {
            Colour const colour = static_cast<Colour>(random.Next() % 5);
            float const health = random.Unit();
            entries.push_back({true, colour, health});
            entries.push_back({false, colour, health});
        }
        for (size_t i = entries.size() - 1; i > 0; --i)
            std::swap(entries[i], entries[random.Next() % (i + 1)]);
        float const balance = LocalBalance(entries, v);
        if (!Near(balance, 0.5, 1e-6))
            problem += " list " + std::to_string(list) + ": " + Number(balance) + ";";
    }
    Check(problem.empty(), "mirrored sides: 0.5 (200 random lists, shuffled)" + problem);

    std::vector<BalanceEntry> const two = {
        {true, Colour::Yellow, 1.0f}, {true, Colour::Yellow, 1.0f}, {false, Colour::Yellow, 1.0f}};
    std::vector<BalanceEntry> yellow = two;
    yellow.push_back({false, Colour::Yellow, 1.0f});
    std::vector<BalanceEntry> red = two;
    red.push_back({false, Colour::Red, 1.0f});
    float const before = LocalBalance(two, v);
    float const withYellow = LocalBalance(yellow, v);
    float const withRed = LocalBalance(red, v);
    Check(withRed < withYellow && withYellow < before && Near(before, 2.0 / 3.0) && Near(withYellow, 0.5) &&
              Near(withRed, 2.0 / 7.0),
          "a red enemy moves it more than a yellow one: " + Number(before) + " -> " + Number(withYellow) +
              " (yellow), " + Number(withRed) + " (red)");

    std::vector<BalanceEntry> hurt = two;
    hurt[1].healthShare = 0.5f;
    std::vector<BalanceEntry> dead = two;
    dead[1].healthShare = 0.0f;
    Check(LocalBalance(hurt, v) < before && Near(LocalBalance(dead, v), 0.5),
          "a hurt ally counts less, a dead one not at all");

    bool monotone = true;
    for (int b = 0; b < 20; ++b)
    {
        float const low = 0.05f * static_cast<float>(b);
        float const high = 0.05f * static_cast<float>(b + 1);
        monotone = monotone && BalanceDepletionFactor(high, v) <= BalanceDepletionFactor(low, v) &&
                   BalanceJoinShift(high, v) <= BalanceJoinShift(low, v);
    }
    Check(monotone && Near(BalanceDepletionFactor(0.5f, v), 1.0) && Near(BalanceJoinShift(0.5f, v), 0.0) &&
              Near(BalanceDepletionFactor(0.0f, v), 3.0) && Near(BalanceDepletionFactor(1.0f, v), 0.5) &&
              Near(BalanceJoinShift(0.0f, v), 30.0) && Near(BalanceJoinShift(1.0f, v), -30.0) &&
              Near(BalanceDepletionFactor(0.375f, v), 2.0) && Near(BalanceJoinShift(0.625f, v), -15.0),
          "factor 3 -> 1 -> 0.5 and shift +30 -> 0 -> -30 from behind to leading, linear, monotone");
}

// --- Starting costs enthusiasm (WP34) ---

// A won fight's length in the simulation: the first night's starters won in about 21 s
constexpr uint32_t WON_FIGHT_MS = 21 * SECOND;
constexpr std::array<float, 3> TRAIT_CORNERS = {-1.0f, 0.0f, 1.0f};

// What a simulated starter did: its starts by hour and their times, and the lowest value a start left beside the
// entering floor
struct StartRun
{
    std::vector<int> perHour;
    std::vector<uint32_t> startsMs;
    double lowestAfterStart = 0.0;
    double floor = 0.0;
};

// A bot that starts whenever the check allows (a yellow target that attacks nobody, even balance), checked every
// second from a full scale, and wins every fight in WON_FIGHT_MS: the start's cost when it pays, the fight's drain,
// then the kill edit outside the share
StartRun SimulateStarter(float aggression, float caution, bool groupHold, bool inShare, uint32_t hours,
                         EnthusiasmValues const& v)
{
    JoinFacts facts;
    facts.aggression = aggression;
    float const join = JoinThreshold(facts, v);
    float const flee = FleeThreshold({caution, groupHold}, v);
    StartRun run;
    run.perHour.assign(hours, 0);
    run.floor = EnterFloor(flee, v);
    run.lowestAfterStart = v.max;
    Enthusiasm state{v.max, 0};
    bool engaged = false;
    uint32_t engagedMs = 0;
    for (uint32_t nowMs = 0; nowMs < hours * HOUR; nowMs += SECOND)
    {
        if (engaged)
        {
            if (nowMs - engagedMs < WON_FIGHT_MS)
                continue;
            state = Apply(state, nowMs, true, 1.0f, inShare ? EditCause::None : EditCause::Kill, 0.0f, v);
            engaged = false;
            continue;
        }
        bool const pays = PaysStart(!FightOngoing(facts), engaged, inShare);
        if (!Enters(Read(state, nowMs, false, 1.0f, v), join, flee, v, pays))
            continue;
        state = Apply(state, nowMs, false, 1.0f, pays ? EditCause::Start : EditCause::None, 0.0f, v);
        run.lowestAfterStart = std::min(run.lowestAfterStart, state.value);
        ++run.perHour[nowMs / HOUR];
        run.startsMs.push_back(nowMs);
        engaged = true;
        engagedMs = nowMs;
    }
    return run;
}

// Exit: a starter in the share makes at most RecoveryPerHour / StartCost starts per hour after its first hour (on
// average over hours 2..49, and no two starts after the first hour closer than StartCost / RecoveryPerHour hours),
// for every Aggression, Caution and group hold, at the default cost and at 60 (WP.2's one start per 4 h). The same
// starter outside the share never slows down: a won fight drains 7 and the kill gives 10, so the value refills and it
// starts again at once (the first night's fault, kept as the counter-case)
void TestStartRate()
{
    EnthusiasmValues const defaults = Defaults();
    EnthusiasmValues tuned = defaults;
    tuned.startCost = 60.0f;
    constexpr uint32_t HOURS = 49;
    for (EnthusiasmValues const& v : {defaults, tuned})
    {
        double const allowed = v.recoveryPerHour / v.startCost;
        uint32_t const gapMs = static_cast<uint32_t>(HOUR / allowed);
        std::string rate;
        std::string slows;
        std::string floor;
        int starters = 0;
        double most = 0.0;
        int fewestOutside = std::numeric_limits<int>::max();
        for (float aggression : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f})
            for (float caution : TRAIT_CORNERS)
                for (bool const group : {false, true})
                {
                    std::string const corner = " (aggression " + Number(aggression) + " caution " + Number(caution) +
                                               (group ? " hold" : "") + ")";
                    StartRun const share = SimulateStarter(aggression, caution, group, true, HOURS, v);
                    StartRun const outside = SimulateStarter(aggression, caution, group, false, HOURS, v);
                    int later = 0;
                    for (uint32_t hour = 1; hour < HOURS; ++hour)
                        later += share.perHour[hour];
                    double const average = static_cast<double>(later) / static_cast<double>(HOURS - 1);
                    most = std::max(most, average);
                    bool spaced = true;
                    for (size_t i = 1; i < share.startsMs.size(); ++i)
                        if (share.startsMs[i - 1] >= HOUR && share.startsMs[i] - share.startsMs[i - 1] < gapMs)
                            spaced = false;
                    if (average > allowed || !spaced)
                        rate +=
                            corner + " " + Number(average) + " per hour" + (spaced ? "" : ", two starts close") + ";";
                    if (share.lowestAfterStart < share.floor)
                        floor += corner + " " + Number(share.lowestAfterStart) + " < " + Number(share.floor) + ";";
                    if (outside.startsMs.empty())
                    {
                        if (!share.startsMs.empty())
                            slows += corner + " starts only in the share;";
                        continue;
                    }
                    ++starters;
                    for (uint32_t hour = 1; hour < HOURS; ++hour)
                    {
                        fewestOutside = std::min(fewestOutside, outside.perHour[hour]);
                        if (outside.perHour[hour] + 1 < outside.perHour[0] || outside.perHour[hour] <= allowed)
                        {
                            slows += corner + " hour " + std::to_string(hour + 1) + " " +
                                     std::to_string(outside.perHour[hour]) + " after " +
                                     std::to_string(outside.perHour[0]) + ";";
                            break;
                        }
                    }
                }
        std::string const cost = "start cost " + Number(v.startCost) + ": ";
        std::printf("  %sshare at most %.3g starts per hour after the first (allowed %.3g); outside at least %d\n",
                    cost.c_str(), most, allowed, fewestOutside);
        Check(rate.empty() && most > 0.0, cost + "a starter in the share makes at most " + Number(allowed) +
                                              " starts per hour after its first hour (most " + Number(most) + ")" +
                                              rate);
        Check(floor.empty(), cost + "no simulated start leaves the value below the entering floor" + floor);
        Check(slows.empty() && starters > 0, cost + "outside the share the same starter never slows (" +
                                                 std::to_string(starters) + " corners start, fewest " +
                                                 std::to_string(fewestOutside) + " per hour)" + slows);
    }
}

// Exit: no start leaves the value below the entering floor, for every trait corner (Aggression, Caution, Helpfulness
// and Confidence at -1, 0 and +1, with and without the group hold), every colour a start may have and every balance
// end, over the whole scale and at the edges; a start the bot does not pay for enters as before; only a start while
// not engaged in the share pays
void TestStartFloor()
{
    EnthusiasmValues const defaults = Defaults();
    EnthusiasmValues tuned = defaults;
    tuned.startCost = 60.0f;
    std::string problem;
    int starts = 0;
    int edges = 0;
    for (EnthusiasmValues const& v : {defaults, tuned})
        for (float aggression : TRAIT_CORNERS)
            for (float caution : TRAIT_CORNERS)
                for (float helpfulness : TRAIT_CORNERS)
                    for (float confidence : TRAIT_CORNERS)
                        for (bool const group : {false, true})
                            for (Colour colour : {Colour::Green, Colour::Yellow, Colour::Orange})
                                for (float balance : {v.balanceBehind, EVEN_BALANCE, v.balanceLead})
                                {
                                    JoinFacts facts;
                                    facts.aggression = aggression;
                                    facts.helpfulness = helpfulness;
                                    facts.colour = colour;
                                    facts.balance = balance;
                                    float const join = JoinThreshold(facts, v);
                                    float const flee = FleeThreshold({caution, group}, v);
                                    double const floor = EnterFloor(flee, v);
                                    std::vector<double> points;
                                    for (int step = 0; step <= 400; ++step)
                                        points.push_back(v.max * step / 400.0);
                                    // The lowest start that pays, a hair above floor plus cost (the sum rounds)
                                    double const edge = floor + v.startCost + 1e-9;
                                    points.insert(points.end(), {edge, edge - 1e-6, join + 1e-6});
                                    for (double const value : points)
                                    {
                                        // A value lies within the scale (Read holds it there)
                                        if (value < 0.0 || value > v.max)
                                            continue;
                                        bool const plain = Enters(value, join, flee, v);
                                        if (plain != (MayJoin(value, join) && value >= floor))
                                            problem += " a start not paid for changed at " + Number(value) + ";";
                                        if (!Enters(value, join, flee, v, true))
                                            continue;
                                        ++starts;
                                        double const after =
                                            Apply({value, 0}, 0, false, 1.0f, EditCause::Start, confidence, v).value;
                                        if (!plain || after < floor)
                                            problem += " aggression " + Number(aggression) + " caution " +
                                                       Number(caution) + " value " + Number(value) + " -> " +
                                                       Number(after) + " floor " + Number(floor) + ";";
                                    }
                                    if (edge < v.max && edge > join + 1e-6)
                                    {
                                        ++edges;
                                        if (!Enters(edge, join, flee, v, true) ||
                                            Enters(edge - 1e-6, join, flee, v, true))
                                            problem += " the edge " + Number(edge) + " is not floor plus cost;";
                                    }
                                }
    Check(problem.empty() && starts > 0 && edges > 0, "no start leaves the value below the entering floor (" +
                                                          std::to_string(starts) + " starts, " + std::to_string(edges) +
                                                          " edges at floor plus cost, every trait corner)" + problem);
    Check(PaysStart(true, false, true) && !PaysStart(false, false, true) && !PaysStart(true, true, true) &&
              !PaysStart(true, false, false),
          "only a start (not a join) while not engaged by a bot in the share pays");
}
}  // namespace

int main()
{
    PrintTable();
    int lastPassed = 0;
    int lastFailed = 0;
    auto const report = [&lastPassed, &lastFailed](char const* name)
    {
        std::printf("%s: %d passed, %d failed\n", name, passed - lastPassed, failed - lastFailed);
        lastPassed = passed;
        lastFailed = failed;
    };
    TestRowsIsolated();
    report("TestRowsIsolated");
    TestScaleBounded();
    report("TestScaleBounded");
    TestLazyEqualsStepped();
    report("TestLazyEqualsStepped");
    TestTraitExtremes();
    report("TestTraitExtremes");
    TestAggressionWhenJoining();
    report("TestAggressionWhenJoining");
    TestScanFloor();
    report("TestScanFloor");
    TestEngagedBounded();
    report("TestEngagedBounded");
    TestBalance();
    report("TestBalance");
    TestStartRate();
    report("TestStartRate");
    TestStartFloor();
    report("TestStartFloor");
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
