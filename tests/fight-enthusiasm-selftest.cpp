/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of a bot's enthusiasm as its AI holds it (src/Ai/World/Pvp/FightEnthusiasm.h, WP23): the seed, the
// engaged edges and a new balance settle with the old state first, a bot enters only when MayJoin and not MustLeave,
// the scan skip, the pick, the one-off edits and the entering row's terms. Build (Git Bash, module root):
//   clang-cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I src/Ai/World/Pvp tests/fight-enthusiasm-selftest.cpp
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "FightEnthusiasm.h"

using namespace WorldPvp;

namespace
{
constexpr uint32_t SECOND = 1000;
constexpr uint32_t MINUTE = 60 * SECOND;
constexpr uint32_t HOUR = 60 * MINUTE;
constexpr double CLOSE = 1e-6;

int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::printf("FAIL %s\n", what.c_str());
    }
}

bool Near(double a, double b) { return std::fabs(a - b) < CLOSE; }

EnthusiasmValues Defaults() { return FromConfig(WorldPvpConfig()); }

BotEnthusiasm At(double value, uint32_t nowMs)
{
    BotEnthusiasm bot;
    bot.seeded = true;
    bot.state = {value, nowMs};
    return bot;
}

void TestSeed()
{
    EnthusiasmValues const v = Defaults();
    BotEnthusiasm bot;
    Seed(bot, UINT32_MAX / 2, 100, v);
    Check(bot.seeded && Near(bot.seed, StartValue(UINT32_MAX / 2, v)) && Near(bot.state.value, bot.seed) &&
              bot.state.atMs == 100,
          "seed: the first use starts the value at the random draw");
    Seed(bot, 0, 200, v);
    Check(Near(bot.state.value, StartValue(UINT32_MAX / 2, v)) && bot.state.atMs == 100, "seed: only once");
}

void TestEdges()
{
    EnthusiasmValues const v = Defaults();
    // Not engaged for an hour (+15), then engaged for a minute at factor 1 (-20)
    BotEnthusiasm bot = At(40.0, 0);
    Check(!Observe(bot, false, HOUR, v) && Near(bot.state.value, 40.0), "edge: no change, no settle");
    Check(Observe(bot, true, HOUR, v) && Near(bot.state.value, 55.0) && bot.engaged && bot.engagedMs == HOUR,
          "edge: becoming engaged settles the recovery first");
    Check(Near(ValueAt(bot, HOUR + MINUTE, v), 35.0), "edge: engaged, the value drains");
    Check(Observe(bot, false, HOUR + MINUTE, v) && Near(bot.state.value, 35.0) && !bot.engaged,
          "edge: leaving settles the drain first");
    Check(Near(ValueAt(bot, 2 * HOUR + MINUTE, v), 50.0), "edge: not engaged, it recovers again");
    // Becoming engaged ends the pick
    SetPick(bot, 42, 2 * HOUR);
    Check(IsPick(bot, 42, 2 * HOUR + SECOND), "pick: open");
    Observe(bot, true, 2 * HOUR + SECOND, v);
    Check(!IsPick(bot, 42, 2 * HOUR + 2 * SECOND) && bot.pickGuid == 0, "pick: ended by the engaged edge");
}

// Review item 2: a new balance settles with the old factor up to now
void TestBalanceSettle()
{
    EnthusiasmValues const v = Defaults();
    BotEnthusiasm bot = At(80.0, 0);
    Observe(bot, true, 0, v);
    NewBalance(bot, 1.0f, false, 0, v);  // clearly leading: factor 0.5
    Check(Near(bot.factor, 0.5f) && Near(bot.joinShift, -30.0f), "balance: leading, factor and shift");
    NewBalance(bot, 0.0f, false, MINUTE, v);  // a minute at 0.5 (-10), then clearly behind: factor 3
    Check(Near(bot.state.value, 70.0) && bot.state.atMs == MINUTE && Near(bot.factor, 3.0f),
          "balance: settled with the old factor before the new one holds");
    Check(Near(ValueAt(bot, 2 * MINUTE, v), 10.0), "balance: old factor then new (80 - 10 - 60), not 3 throughout");
    // The single-rate reading the settle prevents
    Enthusiasm const unsettled{80.0, 0};
    Check(Read(unsettled, 2 * MINUTE, true, 3.0f, v) == 0.0, "balance: without the settle the new factor counts twice");
    // Not engaged: the factor plays no part
    BotEnthusiasm idle = At(20.0, 0);
    NewBalance(idle, 0.0f, true, HOUR, v);
    Check(Near(idle.state.value, 35.0) && idle.nearGraveyard && idle.balanceKnown && idle.balanceMs == HOUR,
          "balance: not engaged, the settle is the recovery; the graveyard row is cached with it");
    Check(!BalanceDue(idle, HOUR + BALANCE_REFRESH_MS - 1) && BalanceDue(idle, HOUR + BALANCE_REFRESH_MS),
          "balance: due again at the scan's cadence");
}

// Review item 1: MayJoin and not MustLeave
void TestEnters()
{
    Check(Enters(50.0, 40.0f, 15.0f), "enters: above both");
    Check(!Enters(40.0, 40.0f, 15.0f), "enters: MayJoin is strict");
    Check(!Enters(30.0, 20.0f, 35.0f), "enters: between a join threshold below the flee threshold, not entered");
    Check(Enters(35.0, 20.0f, 35.0f), "enters: at the flee threshold (MustLeave is strict)");
    Check(!Enters(10.0, -7.5f, 15.0f), "enters: below the flee threshold whatever the join threshold");
}

void TestSkip()
{
    EnthusiasmValues const v = Defaults();
    // Neutral: lowest join = min(start 60 - 30, join (60) - 30 - 7.5 - 50 - 30) = -57.5; flee 15
    BotEnthusiasm bot = At(10.0, 0);
    NewTraits(bot, 0.0f, 0.0f, 0.0f, 0.0f, 0, v);
    Check(Near(bot.lowestJoin, LowestJoinThreshold(0.0f, 0.0f, v)) && Near(bot.flee, 15.0f),
          "skip: the cached thresholds of the traits");
    Check(SkipsScan(bot, 0, v), "skip: below the flee threshold the bot can enter nothing");
    bot.state.value = 15.0;
    Check(!SkipsScan(bot, 0, v), "skip: at the flee threshold, above the lowest join, it scans");
    // Cautious: flee 35 above an aggressive bot's lowest join; the skip uses the higher
    BotEnthusiasm cautious = At(30.0, 0);
    NewTraits(cautious, 1.0f, 1.0f, 0.0f, 0.0f, 0, v);
    Check(cautious.lowestJoin < 30.0f && Near(cautious.flee, 35.0f) && SkipsScan(cautious, 0, v),
          "skip: the higher of the lowest join and the flee threshold");
    // An open pick keeps the scan
    SetPick(bot, 7, 0);
    bot.state.value = 0.0;
    Check(!SkipsScan(bot, SECOND, v) && SkipsScan(bot, PICK_MS, v), "skip: not while a pick is open");
    Check(!ContextDue(bot, ENTHUSIASM_CONTEXT_MS - 1) && ContextDue(bot, ENTHUSIASM_CONTEXT_MS),
          "context: read again after its interval");
}

void TestEdits()
{
    EnthusiasmValues const v = Defaults();
    BotEnthusiasm bot = At(60.0, 0);
    NewTraits(bot, 0.0f, 0.0f, 0.0f, 1.0f, 0, v);
    double const before = EditBy(bot, EditCause::Death, HOUR, v);
    Check(Near(before, 75.0) && Near(ValueAt(bot, HOUR, v), 50.0), "edit: a confident bot's death settles, -25");
    Check(Near(EditBy(bot, EditCause::Kill, HOUR, v), 50.0) && Near(ValueAt(bot, HOUR, v), 60.0), "edit: kill +10");
    NewTraits(bot, 0.0f, 0.0f, 0.0f, 0.0f, HOUR, v);
    EditBy(bot, EditCause::GroupDeath, HOUR, v);
    Check(Near(ValueAt(bot, HOUR, v), 45.0), "edit: group death, 30% of 50");
    EditBy(bot, EditCause::Death, HOUR, v);
    Check(ValueAt(bot, HOUR, v) == 0.0, "edit: held at 0");
}

void TestTerms()
{
    EnthusiasmValues const v = Defaults();
    BotEnthusiasm bot = At(50.0, 0);
    NewTraits(bot, 0.0f, 0.0f, 0.0f, 0.0f, 0, v);
    Check(JoinTerms(FactsFor(bot, Colour::Yellow, false, false), v) == 0, "terms: a plain start has none");
    JoinFacts const plain = FactsFor(bot, Colour::Yellow, false, false);
    Check(Near(JoinThreshold(plain, v), 60.0f), "terms: the plain threshold 60");
    Check(JoinTerms(FactsFor(bot, Colour::Green, false, false), v) == TERM_GREEN, "terms: green");
    Check(JoinTerms(FactsFor(bot, Colour::Yellow, true, false), v) == (TERM_FACTION | TERM_HELPFULNESS),
          "terms: faction with a neutral bot's helpfulness, aggression 0 not weakened");
    JoinFacts const group = FactsFor(bot, Colour::Yellow, false, true);
    Check(group.targetAttacksFaction && JoinTerms(group, v) == (TERM_FACTION | TERM_HELPFULNESS | TERM_GROUP),
          "terms: a group member's attacker also attacks the faction");
    bot.nearGraveyard = true;
    bot.balance = 0.0f;
    Check(JoinTerms(FactsFor(bot, Colour::Yellow, false, false), v) == (TERM_GRAVEYARD | TERM_BALANCE),
          "terms: graveyard and balance");
    BotEnthusiasm peaceable = At(100.0, 0);
    NewTraits(peaceable, -1.0f, 0.0f, -1.0f, 0.0f, 0, v);
    Check(JoinTerms(FactsFor(peaceable, Colour::Green, false, false), v) == 0 &&
              !Enters(100.0, JoinThreshold(FactsFor(peaceable, Colour::Yellow, false, false), v), peaceable.flee),
          "terms: Aggression -1 never starts and applies no terms");
    JoinFacts const helping = FactsFor(peaceable, Colour::Yellow, true, false);
    Check(JoinTerms(helping, v) == (TERM_FACTION | TERM_WEAK_AGGRESSION) &&
              Near(JoinThreshold(helping, v), 70.0f - 30.0f) && Enters(41.0, JoinThreshold(helping, v), 15.0f),
          "terms: Aggression -1 joins its faction's fight at the weakened term (least helpful: no helpfulness)");
    // Each term's bit is on exactly when its fact changes the threshold
    for (int mask = 0; mask < 32; ++mask)
    {
        BotEnthusiasm any = At(50.0, 0);
        NewTraits(any, 0.5f, 0.0f, 0.5f, 0.0f, 0, v);
        any.nearGraveyard = (mask & 8) != 0;
        any.balance = (mask & 16) ? 0.9f : EVEN_BALANCE;
        JoinFacts const facts = FactsFor(any, (mask & 1) ? Colour::Green : Colour::Yellow, (mask & 2) != 0,
                                         (mask & 4) != 0);
        uint32_t const terms = JoinTerms(facts, v);
        JoinFacts off = facts;
        off.colour = Colour::Yellow;
        bool const greenOk = ((terms & TERM_GREEN) != 0) == (JoinThreshold(off, v) != JoinThreshold(facts, v));
        off = facts;
        off.nearGraveyard = false;
        bool const graveOk = ((terms & TERM_GRAVEYARD) != 0) == (JoinThreshold(off, v) != JoinThreshold(facts, v));
        off = facts;
        off.balance = EVEN_BALANCE;
        bool const balanceOk = ((terms & TERM_BALANCE) != 0) == (JoinThreshold(off, v) != JoinThreshold(facts, v));
        bool const ongoingOk = ((terms & TERM_WEAK_AGGRESSION) != 0) == FightOngoing(facts);
        Check(greenOk && graveOk && balanceOk && ongoingOk, "terms: bits match the rows, case " + std::to_string(mask));
    }
}
}  // namespace

int main()
{
    TestSeed();
    TestEdges();
    TestBalanceSettle();
    TestEnters();
    TestSkip();
    TestEdits();
    TestTerms();
    std::printf("fight-enthusiasm-selftest: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
