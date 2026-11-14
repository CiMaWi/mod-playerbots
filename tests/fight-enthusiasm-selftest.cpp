/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of a bot's enthusiasm as its AI holds it (src/Ai/World/Pvp/FightEnthusiasm.h, WP23): the seed, the
// engaged edges and a new balance settle with the old state first, a bot enters only when MayJoin and not MustLeave,
// the scan skip, the pick, the one-off edits, the entering row's terms and the leaving (WP24: engaged by a held target,
// the group hold, the leave check once per episode, the chase, the stalemate, who is left alone, no re-entry). Build
// (Git Bash, module root):
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
    NewGroup(bot, true, 0, v);
    Check(Near(bot.fleeNow, 0.0f) && !SkipsScan(bot, 0, v), "skip: a group member's fight lowers it, the bot scans");
    Check(!GroupDue(bot, BALANCE_REFRESH_MS - 1) && GroupDue(bot, BALANCE_REFRESH_MS),
          "skip: the group hold is read again at the balance's cadence");
    NewGroup(bot, false, 0, v);
    bot.state.value = 15.0;
    Check(!SkipsScan(bot, 0, v), "skip: at the flee threshold, above the lowest join, it scans");
    // Cautious: flee 35 (20 with the group hold) above an aggressive bot's lowest join; the skip uses the higher of
    // the lowest join and the flee threshold as it holds now
    BotEnthusiasm cautious = At(30.0, 0);
    NewTraits(cautious, 1.0f, 1.0f, 0.0f, 0.0f, 0, v);
    Check(cautious.lowestJoin < 30.0f && Near(cautious.flee, 35.0f) && Near(cautious.fleeLowest, 20.0f) &&
              SkipsScan(cautious, 0, v),
          "skip: the higher of the lowest join and the flee threshold");
    NewGroup(cautious, true, 0, v);
    Check(!SkipsScan(cautious, 0, v) && Enters(30.0, cautious.lowestJoin, cautious.fleeNow),
          "skip: with the group hold the same value scans and may enter");
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
BotEnthusiasm Engaged(double value, float caution, uint32_t nowMs, EnthusiasmValues const& v)
{
    BotEnthusiasm bot = At(value, nowMs);
    NewTraits(bot, 0.0f, caution, 0.0f, 0.0f, nowMs, v);
    Observe(bot, true, nowMs, v);
    return bot;
}

// Flagged first: a bot chasing a runner out of reach has no combat references, yet holding it as its victim keeps it
// engaged, so it drains, leaves once below its flee threshold and does not take the runner up again
void TestChase()
{
    EnthusiasmValues const v = Defaults();
    Check(EngagedBy(true, false, true) && EngagedBy(true, true, false) && !EngagedBy(true, false, false) &&
              !EngagedBy(false, true, true),
          "engaged: PvP combat or a held enemy player, open world only");
    BotEnthusiasm bot = At(18.0, 0);  // flee 15, 3 above
    NewTraits(bot, 0.0f, 0.0f, 0.0f, 0.0f, 0, v);
    Check(Observe(bot, EngagedBy(true, false, true), 0, v) && bot.engaged, "chase: holding the runner engages it");
    Check(!Leaves(bot, ValueAt(bot, 8 * SECOND, v), true, true), "chase: above the threshold, it chases");
    double const later = ValueAt(bot, 10 * SECOND, v);
    Check(later < 15.0 && Leaves(bot, later, true, true), "chase: drained below the flee threshold, it leaves");
    MarkLeft(bot, false, 10 * SECOND);
    Check(bot.left && !bot.attacked && bot.leftMs == 10 * SECOND && !FleesNow(bot),
          "chase: not attacked, it resumes (no flight)");
    Check(!Leaves(bot, ValueAt(bot, 11 * SECOND, v), true, true), "chase: once per engaged episode");
    Check(LeftAloneBy(bot, false), "chase: the runner that does not attack it is left alone");
    // The target dropped: nothing holds it engaged; the edge clears the leave
    Check(Observe(bot, EngagedBy(true, false, false), 12 * SECOND, v) && !bot.engaged && !bot.left,
          "chase: the disengaged edge clears the leave");
    Check(!Enters(ValueAt(bot, 13 * SECOND, v), LowestJoinThreshold(0.0f, 0.0f, v), bot.fleeNow) &&
              SkipsScan(bot, 13 * SECOND, v),
          "chase: a left bot is not re-picked by the scan (below its flee threshold)");
}

// Flagged first: two bots that take no damage both drain; the first to leave is still attacked and flees, the second
// leaves too, and then neither attacks or takes up the other
void TestStalemate()
{
    EnthusiasmValues const v = Defaults();
    BotEnthusiasm a = Engaged(17.0, 0.0f, 0, v);
    BotEnthusiasm b = Engaged(20.0, 0.0f, 0, v);
    uint32_t const aLeaves = 7 * SECOND;  // 17 - 20 x 7/60 = 14.67
    Check(Leaves(a, ValueAt(a, aLeaves, v), true, true) && !Leaves(b, ValueAt(b, aLeaves, v), true, true),
          "stalemate: the lower one leaves first");
    MarkLeft(a, true, aLeaves);  // b still attacks it
    Check(FleesNow(a) && !LeftAloneBy(a, true), "stalemate: attacked, it flees and still answers b");
    uint32_t const bLeaves = 16 * SECOND;  // 20 - 5.33 = 14.67
    Check(Leaves(b, ValueAt(b, bLeaves, v), true, true), "stalemate: the other one leaves at its threshold");
    MarkLeft(b, false, bLeaves);  // a dropped its target at its leave
    a.attacked = false;           // b dropped its target: the next refresh reads no attacker
    Check(!FleesNow(a) && !FleesNow(b) && LeftAloneBy(a, false) && LeftAloneBy(b, false),
          "stalemate: neither attacks, each leaves the other alone");
    Observe(a, false, bLeaves + 5 * SECOND, v);
    Observe(b, false, bLeaves + 5 * SECOND, v);
    float const lowestJoin = LowestJoinThreshold(0.0f, 0.0f, v);
    Check(!Enters(ValueAt(a, bLeaves + 25 * SECOND, v), lowestJoin, a.fleeNow) &&
              !Enters(ValueAt(b, bLeaves + 25 * SECOND, v), lowestJoin, b.fleeNow),
          "stalemate: 20 s later neither enters on the other again");
}

void TestLeave()
{
    EnthusiasmValues const v = Defaults();
    BotEnthusiasm bot = Engaged(10.0, 0.0f, 0, v);
    Check(Leaves(bot, 10.0, true, true), "leave: engaged below the flee threshold");
    Check(!Leaves(bot, 10.0, false, true), "leave: not dead");
    Check(!Leaves(bot, 10.0, true, false), "leave: not a bot that does not start by the rules");
    Check(!Leaves(bot, 15.0, true, true), "leave: at the threshold it stays (MustLeave is strict)");
    bot.realMaster = true;
    Check(!Leaves(bot, 10.0, true, true), "leave: a bot with a real master never leaves");
    bot.realMaster = false;
    BotEnthusiasm idle = At(10.0, 0);
    NewTraits(idle, 0.0f, 0.0f, 0.0f, 0.0f, 0, v);
    Check(!Leaves(idle, 10.0, true, true), "leave: not engaged, nothing to leave");
    MarkLeft(bot, true, SECOND);
    Check(bot.left && FleesNow(bot) && !Leaves(bot, 0.0, true, true), "leave: attacked it flees; once per episode");
    Check(LeftAloneBy(bot, false) && !LeftAloneBy(bot, true) && !LeftAloneBy(idle, false),
          "leave: left alone exactly who does not attack a bot that left");
    Check(!Observe(bot, true, 2 * SECOND, v) && bot.left, "leave: no edge, the leave holds");
    Check(Observe(bot, false, 3 * SECOND, v) && !bot.left && !bot.attacked && !FleesNow(bot),
          "leave: the disengaged edge clears it");
    Observe(bot, true, 4 * SECOND, v);
    Check(Leaves(bot, ValueAt(bot, 4 * SECOND, v), true, true), "leave: a new engaged episode checks again");
    // A kill while left does not undo the leave
    MarkLeft(bot, false, 4 * SECOND);
    EditBy(bot, EditCause::Kill, 5 * SECOND, v);
    Check(bot.left, "leave: a kill edit leaves the leave alone");
    // A flee threshold of 0 or below never fires: the value stops at 0
    BotEnthusiasm daring = Engaged(0.0, -1.0f, 0, v);
    Check(Near(daring.fleeNow, -5.0f) && !Leaves(daring, ValueAt(daring, HOUR, v), true, true),
          "leave: Caution -1 (threshold -5) never leaves");
    BotEnthusiasm held = Engaged(0.0, 0.0f, 0, v);
    NewGroup(held, true, 0, v);
    Check(Near(held.fleeNow, 0.0f) && !Leaves(held, ValueAt(held, HOUR, v), true, true),
          "leave: Caution 0 with the group hold (threshold 0) never leaves");
}

void TestGroup()
{
    EnthusiasmValues const v = Defaults();
    GroupMember member;
    member.alive = true;
    member.engaged = true;
    member.yards = 40.0f;
    Check(GroupMemberHolds(member, v), "group: a living engaged member within 40 yd holds");
    member.yards = 40.5f;
    Check(!GroupMemberHolds(member, v), "group: farther than 40 yd it does not");
    member.yards = 10.0f;
    member.alive = false;
    Check(!GroupMemberHolds(member, v), "group: a dead member does not (its death ends the hold at once)");
    member.alive = true;
    member.engaged = false;
    Check(!GroupMemberHolds(member, v), "group: a member whose PvP combat lapsed does not");
    // The bot stays while its member fights, and leaves once the hold fades
    BotEnthusiasm bot = Engaged(8.0, 0.0f, 0, v);
    Check(Near(bot.flee, 15.0f) && Near(bot.fleeLowest, 0.0f) && Near(bot.fleeNow, 15.0f),
          "group: plain 15, held 0, now plain");
    NewGroup(bot, true, 0, v);
    Check(Near(bot.fleeNow, 0.0f) && !Leaves(bot, 8.0, true, true), "group: held, below the plain threshold it stays");
    float const lowestJoin = LowestJoinThreshold(0.0f, 0.0f, v);
    Check(Enters(8.0, lowestJoin, bot.fleeNow) && !Enters(8.0, lowestJoin, bot.flee),
          "group: the entering check uses the threshold as it holds now");
    NewGroup(bot, false, SECOND, v);
    Check(Near(bot.fleeNow, 15.0f) && Leaves(bot, 8.0, true, true), "group: the hold faded, it leaves");
    // A trait change keeps the hold
    NewGroup(bot, true, 2 * SECOND, v);
    NewTraits(bot, 0.0f, 1.0f, 0.0f, 0.0f, 0, v);
    Check(Near(bot.flee, 35.0f) && Near(bot.fleeNow, 20.0f), "group: new traits keep the cached hold");
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
    TestChase();
    TestStalemate();
    TestLeave();
    TestGroup();
    std::printf("fight-enthusiasm-selftest: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
