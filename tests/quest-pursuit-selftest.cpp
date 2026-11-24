/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the order's give-up clock and the failure record (questing WT35c, PursuitRules.h, QuestFailBook.h):
// the clock runs only while the order's executor runs it (an errand, a rest, a death add nothing; a gap counts
// CLOCK_GAP_MS at most), progress restarts it and a falling count does not, it resumes after an interruption by the
// same order (another target gets its own clock, a fifth takes the oldest slot), a run after a give-up starts at zero,
// an immediate cause gives up at once, a give-up reported twice writes one row and one record update, and the limit
// passes exactly at it (the shortgiveup constraint's two minutes too); the record survives a save and a load, writes
// explicit totals and forgets a deleted bot with its pending writes. Mutants that break each rule must fail.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot
//   tests/quest-pursuit-selftest.cpp
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "ClassQuestRules.h"
#include "PursuitRules.h"
#include "QuestExecRules.h"
#include "QuestFailBook.h"
#include "TestConstraints.h"

using namespace Pursuit;

namespace
{
constexpr std::uint32_t TICK_MS = 1000;  // the executor's tick in the model
constexpr std::uint32_t QUEST_A = 7;
constexpr std::uint32_t QUEST_B = 33;
constexpr std::uint32_t BOT = 1234;
constexpr std::uint32_t NOW = 1700000000;

struct Tally
{
    int passed{0};
    int failed{0};
    bool print{true};
};

Tally total;
Tally* tally = &total;

void Check(bool pass, std::string const& text)
{
    if (pass)
        ++tally->passed;
    else
    {
        ++tally->failed;
        if (tally->print)
            std::printf("FAIL: %s\n", text.c_str());
    }
}

// The rules under test; a mutant replaces one
struct Rules
{
    std::function<void(Clock&, bool, std::uint32_t, std::uint32_t, std::uint32_t)> tick =
        [](Clock& clock, bool running, std::uint32_t run, std::uint32_t since, std::uint32_t progress)
    { Tick(clock, running, run, since, progress); };
    std::function<bool(Clock const&, std::uint32_t)> over = [](Clock const& clock, std::uint32_t limit)
    { return Over(clock, limit); };
    std::function<char const*(Clock const&, std::uint32_t, char const*)> decide =
        [](Clock const& clock, std::uint32_t limit, char const* immediate) { return Decide(clock, limit, immediate); };
    std::function<bool(Clock&, std::uint32_t)> report = [](Clock& clock, std::uint32_t run)
    { return Report(clock, run); };
    std::function<Clock&(Clocks&, Key)> take = [](Clocks& clocks, Key key) -> Clock& { return Take(clocks, key); };
    std::function<std::vector<std::uint32_t>(std::vector<std::pair<std::uint32_t, QuestFail::Row>> const&,
                                             std::uint32_t)>
        givenUp = [](std::vector<std::pair<std::uint32_t, QuestFail::Row>> const& rows, std::uint32_t limit)
    { return QuestFail::GivenUp(rows, limit); };
    std::function<bool(bool, StatusEnd::Outcome, char const*)> writes =
        [](bool order, StatusEnd::Outcome outcome, char const* word) { return WritesFailure(order, outcome, word); };
};

Key QuestKey(std::uint32_t quest) { return {Kind::Quest, quest}; }

// `count` running ticks of TICK_MS apart at a constant progress
void Run(Rules const& rules, Clock& clock, std::uint32_t run, int count, std::uint32_t progress,
         std::uint32_t since = TICK_MS)
{
    for (int i = 0; i < count; ++i)
        rules.tick(clock, true, run, since, progress);
}

// 1. The clock runs only while the order's executor runs it; a gap counts CLOCK_GAP_MS at most
void OnlyWhileRunning(Rules const& rules)
{
    Clock clock;
    clock.key = QuestKey(QUEST_A);
    Run(rules, clock, 1, 61, 0);
    Check(clock.idleMs == 60000 && clock.pursuedMs == 60000, "61 running ticks a second apart: 60 s (the first is 0)");
    for (int i = 0; i < 300; ++i)
        rules.tick(clock, false, 1, TICK_MS, 0);  // an errand, a rest, a death: five minutes
    Check(clock.idleMs == 60000 && clock.pursuedMs == 60000, "ticks the executor does not run add nothing");
    rules.tick(clock, true, 1, 300000, 0);
    Check(clock.idleMs == 60000, "the first running tick after a hold counts no pause");
    rules.tick(clock, true, 1, 20000, 0);
    Check(clock.idleMs == 60000 + CLOCK_GAP_MS, "a 20 s gap between running ticks counts CLOCK_GAP_MS");
    rules.tick(clock, true, 1, 3000, 0);
    Check(clock.idleMs == 60000 + CLOCK_GAP_MS + 3000, "a short gap counts in full");
}

// 2. Progress restarts it; a falling count does not
void ProgressRestarts(Rules const& rules)
{
    Clock clock;
    clock.key = QuestKey(QUEST_A);
    Run(rules, clock, 1, 100, 3);
    Check(clock.idleMs == 99000, "no progress: the clock runs");
    rules.tick(clock, true, 1, TICK_MS, 4);
    Check(clock.idleMs == 0 && clock.pursuedMs == 100000, "the count rose: idle time restarts, pursued time goes on");
    Run(rules, clock, 1, 30, 4);
    rules.tick(clock, true, 1, TICK_MS, 2);
    Check(clock.idleMs == 31000, "the count fell (an item gone): the clock goes on");
    Clock fresh;
    fresh.key = QuestKey(QUEST_A);
    rules.tick(fresh, true, 1, TICK_MS, 5);
    Check(fresh.idleMs == 0 && fresh.progress == 5, "the first tick sets the mark");
    Run(rules, fresh, 1, 10, 5);
    Check(fresh.idleMs == 10000, "the first tick's count is no progress");
}

// 3. It survives an interruption: the same order resumes its clock, another target gets its own
void SurvivesInterruption(Rules const& rules)
{
    Clocks clocks;
    {
        Clock& a = rules.take(clocks, QuestKey(QUEST_A));
        Run(rules, a, 1, 101, 0);
    }
    {
        Clock& b = rules.take(clocks, QuestKey(QUEST_B));
        Check(b.idleMs == 0 && b.key == QuestKey(QUEST_B), "another target: a clock of its own at zero");
        Run(rules, b, 2, 51, 0);
    }
    {
        Clock& a = rules.take(clocks, QuestKey(QUEST_A));
        Check(a.idleMs == 100000, "back to the first order: its clock kept");
        rules.tick(a, true, 3, 60000, 0);
        Check(a.idleMs == 100000, "the resumed run's first tick counts no pause");
        Run(rules, a, 3, 20, 0);
        Check(a.idleMs == 120000, "the resumed clock runs on");
    }
    {
        Clock& b = rules.take(clocks, QuestKey(QUEST_B));
        Check(b.idleMs == 50000, "the other order's clock kept too");
    }
    // Four slots: three more targets push the oldest out (B was used after A, so A goes)
    rules.take(clocks, QuestKey(100));
    rules.take(clocks, QuestKey(101));
    rules.take(clocks, QuestKey(102));
    Check(rules.take(clocks, QuestKey(QUEST_B)).idleMs == 50000,
          "four slots: the order used more lately keeps its clock");
    Check(rules.take(clocks, QuestKey(QUEST_A)).idleMs == 0,
          "a fifth target took the oldest slot: its order starts again");

    // A run after a give-up starts at zero; the given-up run itself stays given up
    Clocks again;
    Clock& c = rules.take(again, QuestKey(QUEST_A));
    Run(rules, c, 7, 200, 0);
    rules.report(c, 7);
    Clock& d = rules.take(again, QuestKey(QUEST_A));
    rules.tick(d, true, 8, TICK_MS, 0);
    Check(d.idleMs == 0 && d.pursuedMs == 0 && !d.givenUp, "a new order of a given-up quest starts its clock at zero");
    Forget(again, QuestKey(QUEST_A));
    Check(rules.take(again, QuestKey(QUEST_A)).idleMs == 0, "a forgotten order's clock is gone");
}

// 3b. The record survives a save and a load; its writes are explicit totals; a deleted bot leaves nothing to write
void RecordSurvivesSaveAndLoad()
{
    QuestFail::Book book;
    book.Note(BOT, QUEST_A, "timeout", 12, NOW);
    book.Note(BOT, QUEST_A, "no_target", 3, NOW + 60);
    book.Note(BOT, QUEST_B, "unsupported:escort", 0, NOW + 120);
    book.Note(BOT + 1, QUEST_A, "no_place", 0, NOW + 180);
    std::optional<QuestFail::Row> const a = book.Find(BOT, QUEST_A);
    Check(a && a->giveUps == 2 && a->lastCause == "no_target" && a->minutes == 15 && a->lastTime == NOW + 60,
          "two give-ups: counted, the last cause and time kept, minutes summed");
    std::vector<QuestFail::Written> const written = book.TakeDirty();
    Check(written.size() == 3, "the flush takes each changed row once");
    Check(book.TakeDirty().empty(), "nothing changed since: nothing to write");

    QuestFail::Book loaded;  // the next start: what the flush wrote, read back
    for (QuestFail::Written const& row : written)
        loaded.Restore(row.guid, row.quest, row.row);
    bool same = true;
    for (std::uint32_t const guid : {BOT, BOT + 1})
        for (std::uint32_t const quest : {QUEST_A, QUEST_B})
            same = same && book.Find(guid, quest) == loaded.Find(guid, quest);
    Check(same && loaded.RowsOf(BOT).size() == 2, "save and load: the same record");
    Check(loaded.TakeDirty().empty(), "a loaded row is not written again");

    QuestFail::Row row;
    row.giveUps = 2;
    row.lastCause = "no_target";
    row.minutes = 15;
    row.lastTime = NOW + 60;
    Check(QuestFail::UpsertText({BOT, QUEST_A, row}) ==
              "INSERT INTO playerbots_bot_quest_fail (guid, quest_id, give_ups, last_cause, minutes, last_time) "
              "VALUES (1234, 7, 2, 'no_target', 15, 1700000060) ON DUPLICATE KEY UPDATE give_ups = 2, last_cause = "
              "'no_target', minutes = 15, last_time = 1700000060",
          "the write carries explicit totals: a repeated or late write leaves the same row");
    Check(QuestFail::CleanCause("unsupported:escort") == "unsupported:escort" &&
              QuestFail::CleanCause("a'b; DROP") == "ab" &&
              QuestFail::CleanCause(std::string(40, 'x')).size() == QuestFail::CAUSE_MAX,
          "a cause is stored as a clean word of at most CAUSE_MAX");

    loaded.Note(BOT, QUEST_A, "timeout", 2, NOW + 240);
    loaded.Forget(BOT);
    Check(!loaded.Find(BOT, QUEST_A) && loaded.RowsOf(BOT).empty() && loaded.Find(BOT + 1, QUEST_A),
          "a deleted bot's rows go, another bot's stay");
    Check(loaded.TakeDirty().empty(), "a deleted bot's pending write goes too");
}

// 4. An immediate cause gives up at once
void ImmediateCause(Rules const& rules)
{
    for (char const* word : {"unsupported:escort", "no_place", "no_target", NO_ENDER})
    {
        Clock clock;
        clock.key = QuestKey(QUEST_A);
        rules.tick(clock, true, 1, 0, 0);
        char const* const cause = rules.decide(clock, LimitMs(10, false), word);
        Check(cause && !std::strcmp(cause, word), std::string("an immediate cause at tick 0: ") + word);
    }
    Clock clock;
    clock.key = QuestKey(QUEST_A);
    rules.tick(clock, true, 1, 0, 0);
    Check(!rules.decide(clock, LimitMs(10, false), nullptr), "no cause at tick 0: the order goes on");
}

// 5. A give-up reported twice (the end's hook and an executor path) writes one row and one record update
void OneRowPerGiveUp(Rules const& rules)
{
    QuestFail::Book book;
    Clocks clocks;
    int rows = 0;
    auto giveUp = [&](std::uint32_t run, char const* cause)
    {
        Clock& clock = rules.take(clocks, QuestKey(QUEST_A));
        if (!rules.report(clock, run))
            return;
        ++rows;
        book.Note(BOT, QUEST_A, cause, Minutes(clock), NOW);
    };
    Run(rules, rules.take(clocks, QuestKey(QUEST_A)), 1, 30, 0);
    giveUp(1, "no_target");  // the executor's end
    giveUp(1, "no_target");  // the end's hook sees the same give-up
    std::optional<QuestFail::Row> const once = book.Find(BOT, QUEST_A);
    Check(rows == 1 && once && once->giveUps == 1, "one give-up, two reports: one row, one record update");
    Run(rules, rules.take(clocks, QuestKey(QUEST_A)), 2, 30, 0);
    giveUp(2, TIMEOUT);
    std::optional<QuestFail::Row> const twice = book.Find(BOT, QUEST_A);
    Check(rows == 2 && twice && twice->giveUps == 2, "the next order's give-up writes its own row");
}

// 6. The limit passes exactly at it, never before; the shortgiveup constraint's two minutes are honoured
void Limit(Rules const& rules)
{
    using namespace TestConstraints;
    Check(LimitMs(10, false) == 600000 && LimitMs(10, true) == SHORT_LIMIT_MS && SHORT_LIMIT_MS == 120000,
          "the limit: the tuning value's minutes, two under shortgiveup");
    Check(LimitMs(10, GiveUpShortened(SHORTGIVEUP | NOFIGHT | NODAMAGE)) == 120000 &&
              LimitMs(10, GiveUpShortened(NOFIGHT | NODAMAGE | HOLD)) == 600000,
          "shortgiveup sets the short limit, no other row does");
    for (std::uint32_t const limit : {LimitMs(10, true), LimitMs(10, false)})
    {
        Clock clock;
        clock.key = QuestKey(QUEST_A);
        std::uint32_t const ticks = limit / TICK_MS;
        Run(rules, clock, 1, static_cast<int>(ticks), 0);  // limit - 1 s counted
        Check(!rules.over(clock, limit) && !rules.decide(clock, limit, nullptr),
              "one second before the limit: no give-up (" + std::to_string(limit) + " ms)");
        rules.tick(clock, true, 1, TICK_MS, 0);
        char const* const cause = rules.decide(clock, limit, nullptr);
        Check(rules.over(clock, limit) && cause && !std::strcmp(cause, TIMEOUT),
              "exactly at the limit: given up, cause timeout (" + std::to_string(limit) + " ms)");
        Check(Decide(clock, limit, nullptr, NO_ENDER) == NO_ENDER, "at the ender the limit gives up as no_ender");
    }

    // The giveup case's run: two minutes of running time without progress, broken by an errand and a fight, gives up
    // with two whole minutes on the row
    Clock clock;
    clock.key = QuestKey(QUEST_A);
    std::uint32_t const limit = LimitMs(10, true);
    std::uint32_t run = 1;
    int ticks = 0;
    char const* cause = nullptr;
    while (!cause && ticks < 1000)
    {
        ++ticks;
        if (ticks == 40)
            for (int i = 0; i < 60; ++i)
                rules.tick(clock, false, run, TICK_MS, 0);  // an errand
        if (ticks == 41)
            run = 2;  // the planner starts the order anew after the errand
        rules.tick(clock, true, run, ticks == 70 ? 30000 : TICK_MS, 0);  // a 30 s fight before tick 70
        cause = rules.decide(clock, limit, nullptr);
    }
    Check(cause && !std::strcmp(cause, TIMEOUT) && Minutes(clock) == 2,
          "the giveup case: timeout with two whole minutes pursued");
    // 120 one-second ticks, plus the three that count nothing (the first, after the errand, the new run's first),
    // less the four seconds the fight's tick counts beyond one
    Check(ticks == 120 + 3 - static_cast<int>(CLOCK_GAP_MS / TICK_MS - 1),
          "the errand and the resumed run's first tick add nothing, the fight adds CLOCK_GAP_MS");
    Clock longer;
    longer.pursuedMs = 179999;
    Clock shorter;
    shorter.pursuedMs = 59999;
    Check(Minutes(longer) == 2 && Minutes(shorter) == 0, "minutes are whole minutes rounded down");
}

// 7. The readers' rule (WT35d): a quest given up GIVE_UP_LIMIT times is no longer offered, once is; sorted, per bot,
// the same after a save and a load
void GivenUpReader(Rules const& rules)
{
    std::uint32_t const limit = Planner::ClassQuests::GIVE_UP_LIMIT;
    QuestFail::Book book;
    std::uint32_t const quests[] = {QUEST_B, QUEST_A, 5};
    for (std::uint32_t i = 0; i < limit; ++i)
        for (std::uint32_t const quest : quests)
            book.Note(BOT, quest, "timeout", 1, NOW + i);
    book.Note(BOT, 900, "no_target", 0, NOW);  // given up once
    book.Note(BOT + 1, 901, "no_place", 0, NOW);
    book.Note(BOT + 1, 901, "no_place", 0, NOW + 1);
    std::vector<std::uint32_t> const ids = rules.givenUp(book.RowsOf(BOT), limit);
    Check(ids == std::vector<std::uint32_t>{5, QUEST_A, QUEST_B}, "the quests given up at the limit, sorted");
    Check(!QuestFail::IsGivenUp(ids, 900) && QuestFail::IsGivenUp(ids, QUEST_A),
          "a quest given up once is offered again, at the limit it is not");
    Check(!QuestFail::IsGivenUp(ids, 901) &&
              rules.givenUp(book.RowsOf(BOT + 1), limit) == std::vector<std::uint32_t>{901},
          "another bot's give-ups stay its own");
    Check(rules.givenUp(book.RowsOf(BOT + 2), limit).empty(), "a bot without a record has nothing given up");

    QuestFail::Book loaded;
    for (QuestFail::Written const& row : book.TakeDirty())
        loaded.Restore(row.guid, row.quest, row.row);
    Check(rules.givenUp(loaded.RowsOf(BOT), limit) == ids, "the given-up quests survive a save and a load");
}

// 8. WQ19b: which quest status ends write the failure row at the door. A free quest status: every give-up, by its
// outcome (Abandoned) or by its word whatever the outcome (night lab5-20261122: 15 walk deadlines, timeout ending
// TimedOut, wrote no row); an order: only the failed hand-in no_ender (WQ.43 point 6)
void FailureRows(Rules const& rules)
{
    using StatusEnd::Outcome;
    Check(rules.writes(false, Outcome::TimedOut, TIMEOUT),
          "a quest's walk deadline (timeout, TimedOut) writes its row");
    Check(rules.writes(false, Outcome::Abandoned, "no_target") && rules.writes(false, Outcome::Abandoned, nullptr),
          "a quest given up writes its row, with or without a word");
    Check(rules.writes(false, Outcome::Stuck, "unsupported:escort") && rules.writes(false, Outcome::Stuck, NO_ENDER),
          "a give-up word writes its row whatever the outcome");
    Check(!rules.writes(false, Outcome::Finished, "finished") && !rules.writes(false, Outcome::Interrupted, nullptr) &&
              !rules.writes(false, Outcome::TimedOut, "deadline"),
          "a quest that ends otherwise writes none");
    Check(rules.writes(true, Outcome::Abandoned, NO_ENDER), "an order's failed hand-in writes its row");
    Check(!rules.writes(true, Outcome::Abandoned, "no_path") && !rules.writes(true, Outcome::TimedOut, TIMEOUT) &&
              !rules.writes(true, Outcome::TimedOut, "dry") && !rules.writes(true, Outcome::Finished, "done"),
          "an order writes no row for anything else");
    Check(IsGiveUpWord("refused:level") && IsGiveUpWord("locked") && !IsGiveUpWord("refuse") &&
              !IsGiveUpWord(nullptr) && !IsGiveUpWord("deadline"),
          "the give-up words are the check's");
}

void All(Rules const& rules)
{
    OnlyWhileRunning(rules);
    ProgressRestarts(rules);
    SurvivesInterruption(rules);
    ImmediateCause(rules);
    OneRowPerGiveUp(rules);
    Limit(rules);
    GivenUpReader(rules);
    FailureRows(rules);
}

void Mutant(char const* name, Rules const& rules)
{
    Tally local;
    local.print = false;
    tally = &local;
    All(rules);
    tally = &total;
    Check(local.failed > 0, std::string("mutant ") + name + " is caught");
}
}  // namespace

int main()
{
    Check(CLOCK_GAP_MS == Planner::QuestExec::CLOCK_GAP_MS, "the order clock counts a pause as the executor's clocks");
    All(Rules{});
    RecordSurvivesSaveAndLoad();

    Rules runsWhileHeld;
    runsWhileHeld.tick = [](Clock& clock, bool, std::uint32_t run, std::uint32_t since, std::uint32_t progress)
    { Tick(clock, true, run, since, progress); };
    Mutant("runs while the order does not", runsWhileHeld);

    Rules gapCountsInFull;
    gapCountsInFull.tick =
        [](Clock& clock, bool running, std::uint32_t run, std::uint32_t since, std::uint32_t progress)
    {
        bool const counts = running && clock.ticking && clock.run == run && since > CLOCK_GAP_MS;
        Tick(clock, running, run, since, progress);
        if (counts)
            clock.idleMs += since - CLOCK_GAP_MS;
    };
    Mutant("counts a gap in full", gapCountsInFull);

    Rules progressIgnored;
    progressIgnored.tick = [](Clock& clock, bool running, std::uint32_t run, std::uint32_t since, std::uint32_t)
    { Tick(clock, running, run, since, clock.progress); };
    Mutant("progress does not restart", progressIgnored);

    Rules fallRestarts;
    fallRestarts.tick = [](Clock& clock, bool running, std::uint32_t run, std::uint32_t since, std::uint32_t progress)
    {
        bool const fell = running && clock.started && progress < clock.progress;
        Tick(clock, running, run, since, progress);
        if (fell)
            clock.idleMs = 0;
    };
    Mutant("a falling count restarts", fallRestarts);

    Rules interruptionResets;
    interruptionResets.tick =
        [](Clock& clock, bool running, std::uint32_t run, std::uint32_t since, std::uint32_t progress)
    {
        if (running && clock.started && clock.run != run)
            clock.idleMs = 0;
        Tick(clock, running, run, since, progress);
    };
    Mutant("an interruption resets", interruptionResets);

    Rules oneSlot;
    oneSlot.take = [](Clocks& clocks, Key key) -> Clock&
    {
        if (!(clocks.slots[0].key == key))
        {
            clocks.slots[0] = Clock{};
            clocks.slots[0].key = key;
        }
        return clocks.slots[0];
    };
    Mutant("keeps one clock only", oneSlot);

    Rules immediateWaits;
    immediateWaits.decide = [](Clock const& clock, std::uint32_t limit, char const* immediate)
    { return Over(clock, limit) ? (immediate ? immediate : TIMEOUT) : nullptr; };
    Mutant("an immediate cause waits for the limit", immediateWaits);

    Rules doubleRow;
    doubleRow.report = [](Clock& clock, std::uint32_t run)
    {
        Report(clock, run);
        return true;
    };
    Mutant("a give-up reported twice writes two rows", doubleRow);

    Rules pastLimit;
    pastLimit.over = [](Clock const& clock, std::uint32_t limit) { return clock.idleMs > limit; };
    pastLimit.decide = [](Clock const& clock, std::uint32_t limit, char const* immediate)
    { return immediate ? immediate : (clock.idleMs > limit ? TIMEOUT : nullptr); };
    Mutant("gives up one tick past the limit", pastLimit);

    Rules limitOffByOne;
    limitOffByOne.givenUp = [](std::vector<std::pair<std::uint32_t, QuestFail::Row>> const& rows, std::uint32_t limit)
    { return QuestFail::GivenUp(rows, limit - 1); };
    Mutant("the given-up limit off by one", limitOffByOne);

    Rules orderWritesAll;
    orderWritesAll.writes = [](bool, StatusEnd::Outcome outcome, char const* word)
    { return WritesFailure(false, outcome, word); };
    Mutant("an order writes a row for every give-up", orderWritesAll);

    Rules outcomeOnly;
    outcomeOnly.writes = [](bool order, StatusEnd::Outcome outcome, char const* word)
    { return order ? WritesFailure(true, outcome, word) : outcome == StatusEnd::Outcome::Abandoned; };
    Mutant("a walk's timeout writes no row (the 15 rows)", outcomeOnly);

    std::printf("%d passed, %d failed\n", total.passed, total.failed);
    return total.failed ? 1 : 0;
}
