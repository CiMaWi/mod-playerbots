/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of applying the planner's goals (travel plan WT16, GoalInterface.h): who the planner leads (the pinned
// table, questing WQ6e), source precedence, the ticket match, the idle step's fallback reasons and bounded wait, the
// interlude's return, the executors (each quest option to what it names), the keep list, the circuit's next task, the
// one stall rule of every goal the planner starts (keys, the log signature without XP, the circuit and delivery loops
// it ends), the ends from outside it does not judge (change 2, CS14e), the planned point of interest, and that every
// block ends (questing WQ6b: a search over every state of the blocks, with its mutants).
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg tests/goal-apply-selftest.cpp

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>

#include "ClassQuestRules.h"
#include "GoalInterface.h"
#include "GoalModelLog.h"
#include "PlannerPoolTypes.h"
#include "QuestFailBook.h"
#include "TestConstraints.h"
#include "WorldTravelRules.h"

namespace
{
using namespace Planner;
using namespace Planner::GoalApply;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

Active GoalOf(Goals::GoalKind kind, std::uint32_t target, Source source)
{
    Active goal;
    goal.kind = kind;
    goal.target = target;
    goal.source = source;
    return goal;
}

// A planner goal as GoalModel makes it: the circuit (reason "circuit"), a delivery (target type quest), a move
Active Typed(Goals::GoalKind kind, Goals::TargetType targetType, char const* reason, std::uint32_t target = 1)
{
    Active goal = GoalOf(kind, target, Source::Planner);
    goal.targetType = targetType;
    goal.reason = reason;
    return goal;
}

CircuitPlan SampleCircuit()
{
    CircuitPlan plan;
    plan.ok = true;
    plan.take = {{101, 0, 1.0f, false}, {102, 0, 1.0f, true}};
    plan.deferred = {103};
    plan.carried = {102};
    CircuitStop first;
    first.x = 100.0f;
    first.y = 100.0f;
    first.tasks = {{201, 0, 100.0f, 100.0f, 0.0f}, {202, 1, 110.0f, 100.0f, 0.0f}};
    CircuitStop second;
    second.x = 500.0f;
    second.y = 500.0f;
    second.tasks = {{201, 1, 500.0f, 500.0f, 0.0f}, {203, 0, 505.0f, 500.0f, 0.0f}};
    plan.stops = {first, second};
    plan.turnIns = {{204, 0, 0}};
    plan.abandon = {{203, AbandonReason::Count}, {999, AbandonReason::Count}};
    return plan;
}

// The block time of the cases before WQ6b: Planner.BlockMinutes' default, 10 minutes
constexpr std::uint32_t BLOCK = 10 * 60 * 1000;

// The cases before WQ6b (the stall rule's keys and its release by the log): the clock stands still at 0, so a block
// holds until the log changes, which is what they test. WQ6b's search below runs the clock
bool MayStart(BotGoals const& goals, std::uint64_t key, std::uint64_t log)
{
    return GoalApply::MayStart(goals, key, log, 0, BLOCK);
}

void EndRun(BotGoals& goals, std::uint64_t log, std::uint64_t xp = 0) { GoalApply::EndRun(goals, log, xp, 0, BLOCK); }

// WQ6b's search (every block ends): what one option met at an idle
enum class Attempt : std::uint8_t
{
    Started,
    Refused,
    Blocked  // not attempted: its key is blocked
};

constexpr std::uint64_t SEARCH_LOG = 77;  // the quest log, which never changes in the search

// The rule under test: the header's StartActive and EndRun on one bot's goals, each goal ended by itself. Every
// block it sets must come with its GoalStalled row, a refused start's with the word refused
struct HeaderRule
{
    BotGoals goals;
    std::uint32_t blockMs{0};
    std::size_t rowFaults{0};

    Attempt Try(std::uint64_t key, bool refuses, std::uint32_t nowMs)
    {
        StartFacts const facts{key, SEARCH_LOG, 0, true, nowMs, blockMs};
        StartResult const result =
            StartActive(goals, Goals::GoalKind::Quest, Executor::Circuit, facts, [refuses] { return !refuses; });
        Attempt const got = result == StartResult::Started   ? Attempt::Started
                            : result == StartResult::Refused ? Attempt::Refused
                                                             : Attempt::Blocked;
        rowFaults +=
            got == Attempt::Refused ? !(goals.stallRow && goals.stallRow->refused) : goals.stallRow.has_value();
        goals.stallRow.reset();
        return got;
    }

    void End(std::uint64_t, std::uint32_t nowMs)
    {
        NoteStatusEnd(goals, StatusEnd::Outcome::Finished, true);
        GoalApply::EndRun(goals, SEARCH_LOG, 0, nowMs, blockMs);
        rowFaults += !(goals.stallRow && !goals.stallRow->refused && goals.stallRow->judged);
        goals.stallRow.reset();
    }

    std::size_t Size() const { return goals.stalled ? goals.stalled->blocks.size() : 0; }
};

// A copy of the rule to mutate, the mutants' proof that the search sees each fault. With every switch on it makes the
// header's decisions (checked)
struct ModelRule
{
    bool timed{true};          // false, the mutant endless: a block lifts only by the log (the rule before WQ6b)
    bool refusalBlocks{true};  // false, the mutant refused not blocked: a refused start leaves no block
    bool refreshes{true};      // false, the mutant not refreshed: a listed key is not blocked anew
    std::uint32_t blockMs{0};
    std::vector<Block> blocks;

    bool Held(std::uint64_t key, std::uint32_t nowMs) const
    {
        return std::any_of(blocks.begin(), blocks.end(), [&](Block const& block)
                           { return block.key == key && (!timed || nowMs - block.sinceMs < blockMs); });
    }

    void Set(std::uint64_t key, std::uint32_t nowMs)
    {
        auto const listed =
            std::find_if(blocks.begin(), blocks.end(), [key](Block const& block) { return block.key == key; });
        if (listed == blocks.end())
            blocks.push_back({key, nowMs});
        else if (refreshes)
            listed->sinceMs = nowMs;
    }

    Attempt Try(std::uint64_t key, bool refuses, std::uint32_t nowMs)
    {
        if (Held(key, nowMs))
            return Attempt::Blocked;
        if (!refuses)
            return Attempt::Started;
        if (refusalBlocks)
            Set(key, nowMs);
        return Attempt::Refused;
    }

    void End(std::uint64_t key, std::uint32_t nowMs) { Set(key, nowMs); }

    std::size_t Size() const { return blocks.size(); }
};

struct SearchOption
{
    std::uint64_t key{0};
    bool refuses{false};  // its start is refused; else its goal runs the goal time and ends without progress
};

struct SearchAttempt
{
    std::uint64_t atMs{0};  // since the search's start
    std::size_t option{0};
    Attempt got{Attempt::Started};
    bool operator==(SearchAttempt const&) const = default;
};

struct SearchTrace
{
    std::vector<SearchAttempt> attempts;
    bool live{true};     // A: every option attempted by the time the bot idled the block time and a fallback status
    bool rated{true};    // B: no option attempted again before the block time passed since its last block began
    bool bounded{true};  // the list never longer than MAX_STALLED
    std::uint64_t worstIdleMs{0};
};

constexpr std::int64_t NOT_BLOCKED = std::numeric_limits<std::int64_t>::min();

// One bot from one state, over several windows of the block time and a fallback status. At each idle it attempts
// the options in rank order until one starts; a started goal runs goalMs and ends without progress; an idle that
// starts nothing lasts one fallback status (the worst case: a plan may end it sooner). blockedAt: when each option's
// block began, ms since the start (negative: before it), NOT_BLOCKED for none
template <typename Rule>
SearchTrace Simulate(Rule& rule, std::vector<SearchOption> const& options, std::vector<std::int64_t> blockedAt,
                     std::uint32_t startMs, std::uint32_t blockMs, std::uint32_t goalMs)
{
    SearchTrace trace;
    std::uint64_t const horizon = 6ULL * (blockMs + FALLBACK_STATUS_MS) + goalMs;
    std::uint64_t const bound = static_cast<std::uint64_t>(blockMs) + FALLBACK_STATUS_MS;
    std::vector<std::uint64_t> idled(options.size(), 0);  // the fallback time since the option's last attempt
    std::uint64_t at = 0;
    while (at < horizon)
    {
        bool started = false;
        for (std::size_t i = 0; i < options.size() && !started; ++i)
        {
            Attempt const got = rule.Try(options[i].key, options[i].refuses, startMs + static_cast<std::uint32_t>(at));
            if (got == Attempt::Blocked)
                continue;
            trace.attempts.push_back({at, i, got});
            trace.live = trace.live && idled[i] <= bound;
            trace.rated = trace.rated && (blockedAt[i] == NOT_BLOCKED ||
                                          static_cast<std::int64_t>(at) - blockedAt[i] >= std::int64_t{blockMs});
            trace.worstIdleMs = std::max(trace.worstIdleMs, idled[i]);
            idled[i] = 0;
            if (got == Attempt::Started)
            {
                started = true;
                at += goalMs;
                rule.End(options[i].key, startMs + static_cast<std::uint32_t>(at));
            }
            blockedAt[i] = static_cast<std::int64_t>(at);
        }
        if (!started)
        {
            at += FALLBACK_STATUS_MS;
            for (std::uint64_t& idle : idled)
                idle += FALLBACK_STATUS_MS;
        }
        trace.bounded = trace.bounded && rule.Size() <= MAX_STALLED;
    }
    for (std::uint64_t const idle : idled)
    {
        trace.live = trace.live && idle <= bound;
        trace.worstIdleMs = std::max(trace.worstIdleMs, idle);
    }
    return trace;
}

// WQ6b (the stopgap of WQ.33): every block ends. A plan of one to three options, each with an executor, each either
// refused at its start or ending without progress after its goal's time; the quest log never changes. From every set
// of blocked keys with every remaining time (0, 1 ms, half, all but 1 ms, all of the block time), at both block times
// (WQ6d's 1 minute, the default 10) and goal times of 1 s, 5 and 30 minutes: (A) each option is attempted by the time
// the bot idled the block time and one fallback status since its last attempt, so a bot is never idle longer while
// any option has an executor; (B) an option whose goal ended without progress or whose start was refused is not
// attempted again before the block time passed. Long goals may hold a lower option back, but never idle
void EveryBlockEnds()
{
    std::uint32_t const blockTimes[] = {60 * 1000, BLOCK};
    std::uint32_t const goalTimes[] = {1000, 5 * 60 * 1000, 30 * 60 * 1000};
    // The reference start, one whose blocks began before the clock's 0, one that crosses 2^32 seven minutes in
    std::uint32_t const starts[] = {1000000, 1000, 0xFFFFFFFFu - 7 * 60 * 1000 + 1};
    std::size_t states = 0;
    std::size_t bad = 0;
    std::size_t wrapBad = 0;
    std::size_t modelBad = 0;
    std::size_t rowFaults = 0;
    std::size_t endlessBreaksA = 0;
    std::size_t refusedBreaksB = 0;
    std::size_t staleBreaksB = 0;
    std::uint64_t worstIdleMs = 0;
    for (std::uint32_t const blockMs : blockTimes)
        for (std::uint32_t const goalMs : goalTimes)
            for (std::size_t count = 1; count <= 3; ++count)
            {
                std::uint32_t const remaining[] = {0, 1, blockMs / 2, blockMs - 1, blockMs};
                std::size_t const choices = std::size(remaining) + 1;  // free, or blocked with one remaining time
                std::size_t inits = 1;
                for (std::size_t i = 0; i < count; ++i)
                    inits *= choices;
                for (std::size_t refusals = 0; refusals < (std::size_t{1} << count); ++refusals)
                    for (std::size_t init = 0; init < inits; ++init)
                    {
                        std::vector<SearchOption> options;
                        std::vector<std::int64_t> blockedAt;
                        std::size_t rest = init;
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            options.push_back({i + 1, ((refusals >> i) & 1) != 0});
                            std::size_t const choice = rest % choices;
                            rest /= choices;
                            blockedAt.push_back(choice ? -static_cast<std::int64_t>(blockMs - remaining[choice - 1])
                                                       : NOT_BLOCKED);
                        }
                        auto const blocksAt = [&](std::uint32_t startMs)
                        {
                            std::vector<Block> blocks;
                            for (std::size_t i = 0; i < count; ++i)
                                if (blockedAt[i] != NOT_BLOCKED)
                                    blocks.push_back(
                                        {options[i].key, startMs + static_cast<std::uint32_t>(blockedAt[i])});
                            return blocks;
                        };
                        auto const header = [&](std::uint32_t startMs)
                        {
                            HeaderRule rule;
                            rule.blockMs = blockMs;
                            std::vector<Block> blocks = blocksAt(startMs);
                            if (!blocks.empty())
                                rule.goals.stalled = Stalled{SEARCH_LOG, std::move(blocks)};
                            SearchTrace const trace = Simulate(rule, options, blockedAt, startMs, blockMs, goalMs);
                            rowFaults += rule.rowFaults;
                            return trace;
                        };
                        auto const model = [&](bool timed, bool refusalBlocks, bool refreshes)
                        {
                            ModelRule rule{timed, refusalBlocks, refreshes, blockMs, blocksAt(starts[0])};
                            return Simulate(rule, options, blockedAt, starts[0], blockMs, goalMs);
                        };
                        SearchTrace const real = header(starts[0]);
                        ++states;
                        bad += !(real.live && real.rated && real.bounded && !real.attempts.empty());
                        worstIdleMs = std::max(worstIdleMs, real.worstIdleMs);
                        wrapBad +=
                            header(starts[1]).attempts != real.attempts || header(starts[2]).attempts != real.attempts;
                        modelBad += model(true, true, true).attempts != real.attempts;
                        endlessBreaksA += !model(false, true, true).live;
                        refusedBreaksB += !model(true, false, true).rated;
                        staleBreaksB += !model(true, true, false).rated;
                    }
            }
    std::printf(
        "info: WQ6b every block ends: %zu states, %zu counterexamples, worst idle %llu s before an attempt; "
        "mutants: endless breaks A in %zu, refused not blocked breaks B in %zu, not refreshed breaks B in %zu\n",
        states, bad, static_cast<unsigned long long>(worstIdleMs / 1000), endlessBreaksA, refusedBreaksB, staleBreaksB);
    Check(states == 11304 && bad == 0 && rowFaults == 0,
          "WQ6b every block ends: from every set of blocked keys and remaining times, each option is attempted within "
          "the block time and one fallback status of idling, none again before its block time passed, the list "
          "bounded, every block with its GoalStalled row (refused for a refused start)");
    Check(wrapBad == 0, "the same decisions when the blocks began before the clock's 0 or the clock crosses 2^32");
    Check(modelBad == 0, "the mutants' model with every switch on makes the header's decisions in every state");
    Check(endlessBreaksA > 0,
          "(mutant: endless, a block lifts only by the log, the rule before WQ6b) breaks A: the bot idles on");
    Check(refusedBreaksB > 0, "(mutant: refused not blocked) breaks B: a refused option is tried at every idle");
    Check(staleBreaksB > 0,
          "(mutant: not refreshed, a listed key is not blocked anew) breaks B: a second end without progress after "
          "the block ended blocks nothing");

    // The release by the log, kept: a changed log lifts every block at once, before its time
    {
        BotGoals goals;
        for (std::uint64_t key = 1; key <= 3; ++key)
        {
            BeginRun(goals, key, 5);
            GoalApply::EndRun(goals, 5, 0, 1000, BLOCK);
        }
        bool const held = !GoalApply::MayStart(goals, 1, 5, 2000, BLOCK) &&
                          !GoalApply::MayStart(goals, 3, 5, 2000, BLOCK) &&
                          GoalApply::MayStart(goals, 3, 5, 1000 + BLOCK, BLOCK);
        bool const lifted =
            GoalApply::MayStart(goals, 1, 6, 2000, BLOCK) && GoalApply::MayStart(goals, 2, 6, 2000, BLOCK);
        BeginRun(goals, 4, 5);
        GoalApply::EndRun(goals, 6, 0, 2000, BLOCK);
        Check(held && lifted && !goals.stalled,
              "a block holds for its time, and a changed log lifts every block at once before it (the release by the "
              "log, kept)");
    }
    // The list stays bounded: blocks that ended leave it at the next block
    {
        BotGoals goals;
        bool within = true;
        for (std::uint32_t i = 0; i < 40; ++i)
        {
            BeginRun(goals, 100 + i, 5);
            GoalApply::EndRun(goals, 5, 0, i * 20000, 60 * 1000);
            within = within && goals.stalled && goals.stalled->blocks.size() <= MAX_STALLED;
        }
        Check(within && goals.stalled->blocks.size() == 3,
              "the block list stays within MAX_STALLED; blocks that ended leave it (20 s apart, 1 minute: 3 stay)");
    }
}
}  // namespace

// Questing WQ18a: an order of a route in a cell of map 0
QuestOrder OrderAt(std::int32_t cellX, std::int32_t cellY, QuestOrderWord word, std::vector<QuestOrderNeed> expected,
                   std::uint32_t mapId = 0)
{
    QuestOrder order;
    order.word = word;
    order.mapId = mapId;
    order.cell = FieldCellKey(mapId, cellX, cellY);
    order.x = (static_cast<float>(cellX) + 0.5f) * FIELD_CELL_YARDS;
    order.y = (static_cast<float>(cellY) + 0.5f) * FIELD_CELL_YARDS;
    order.expected = std::move(expected);
    order.minutes = 5.0f;
    return order;
}

std::shared_ptr<Goals::GoalPlan const> RouteOf(std::vector<QuestOrder> orders)
{
    auto plan = std::make_shared<Goals::GoalPlan>();
    plan->route.orders = std::move(orders);
    plan->route.worth = 100.0f;
    plan->route.minutes = 10.0f;
    return plan;
}

// Questing WQ18a: the rules that walk a bot through its route (AdvanceRoute, EndOrder, the block of a dry cell,
// TakePlan's keep rule)
void RouteRules()
{
    constexpr std::uint32_t BLOCK_MS = 10 * 60 * 1000;
    constexpr std::uint64_t LOG0 = 11;
    constexpr std::uint64_t LOG1 = 12;
    constexpr std::uint64_t LOG2 = 13;
    constexpr std::uint64_t HUB_KEY = 40;
    using Word = QuestOrderWord;
    Active const routeGoal = Typed(Goals::GoalKind::Quest, Goals::TargetType::Route, Goals::ROUTE_REASON, 0);
    std::uint64_t const routeKey = GoalKey(routeGoal, Executor::Route, HUB_KEY);
    auto const anyOpen = [](QuestOrderNeed const&) { return true; };
    auto const sameAs = [](RouteNext const& next, QuestOrder const& order)
    { return next.step == RouteStep::Order && next.order && SameOrder(*next.order, order); };

    {
        Reach const nowhere;
        Reach everywhere;
        everywhere.deliverable = everywhere.sameMap = everywhere.there = everywhere.workable = true;
        Check(
            ExecutorFor(routeGoal, nowhere) == Executor::Route &&
                ExecutorFor(routeGoal, everywhere) == Executor::Route && RunsRoute(routeGoal) &&
                !RunsCircuit(routeGoal) &&
                !RunsRoute(Typed(Goals::GoalKind::Quest, Goals::TargetType::Default, Goals::CIRCUIT_REASON)) &&
                !RunsRoute(Typed(Goals::GoalKind::Quest, Goals::TargetType::Field, Goals::CIRCUIT_REASON, 0)),
            "route: its option runs the route executor wherever the bot is; a circuit, also one named field, does not "
            "(mutant: RoleOf by the reason word)");
        std::size_t cells = 0;
        std::size_t meet = 0;
        for (std::uint32_t const map : {0u, 1u, 530u, 571u, 724u})
            for (std::int32_t x = -400; x <= 400; x += 37)
                for (std::int32_t y = -400; y <= 400; y += 41)
                {
                    std::uint64_t const cell = FieldCellKey(map, x, y);
                    std::uint64_t const key = CellBlockKey(cell);
                    ++cells;
                    meet += !IsCellBlock(key) || IsRouteKey(key) || CellOfBlock(key) != cell || IsCellBlock(cell);
                }
        for (std::uint32_t executor = 0; executor <= static_cast<std::uint32_t>(Executor::Route); ++executor)
            for (std::uint32_t kind = 0; kind < static_cast<std::uint32_t>(Goals::GoalKind::Count); ++kind)
                for (std::uint32_t type = 0; type < static_cast<std::uint32_t>(Goals::TargetType::Count); ++type)
                    for (std::uint32_t const target : {0u, 1u, 0xFFFFFFFFu})
                    {
                        Active goal = GoalOf(static_cast<Goals::GoalKind>(kind), target, Source::Planner);
                        goal.targetType = static_cast<Goals::TargetType>(type);
                        std::uint64_t const key = GoalKey(goal, static_cast<Executor>(executor), HUB_KEY);
                        meet += IsCellBlock(key);
                        meet += IsRouteKey(key) != (static_cast<Executor>(executor) == Executor::Route);
                    }
        for (std::uint64_t const circuitKey :
             {std::uint64_t{0}, std::uint64_t{40}, std::uint64_t{0xFFFFFFFFu}, CAMP_KEY | 7, CAMP_KEY | NO_CAMP})
            meet += IsCellBlock(circuitKey) || IsRouteKey(circuitKey);
        Check(cells > 1000 && meet == 0,
              "route: the block keys of cells, of goals, of routes and of circuits never meet, and a cell's key gives "
              "its cell back (mutant: the tag at bit 40)");
    }

    // AdvanceRoute over every set of open needs
    {
        QuestOrder const a = OrderAt(0, 0, Word::Field, {{1, 0, 3}, {1, 1, 2}});
        QuestOrder const b = OrderAt(5, 0, Word::HandIn, {{2, ORDER_HAND_IN, 1}, {3, ORDER_TAKE, 1}});
        QuestOrder const c = OrderAt(9, 9, Word::Field, {{4, 0, 5}});
        std::vector<QuestOrder> const orders = {a, b, c};
        std::vector<QuestOrderNeed> needs;
        for (QuestOrder const& order : orders)
            needs.insert(needs.end(), order.expected.begin(), order.expected.end());
        std::size_t wrong = 0;
        std::size_t notDue = 0;
        std::size_t started = 0;
        for (std::uint32_t mask = 0; mask < (1u << needs.size()); ++mask)
        {
            auto const open = [&needs, mask](QuestOrderNeed const& need)
            {
                for (std::size_t i = 0; i < needs.size(); ++i)
                    if (needs[i] == need)
                        return ((mask >> i) & 1) != 0;
                return false;
            };
            std::vector<std::uint64_t> expected;
            for (QuestOrder const& order : orders)
                if (std::any_of(order.expected.begin(), order.expected.end(), open))
                    expected.push_back(order.cell);
            BotGoals goals;
            TakePlan(goals, RouteOf(orders), 1000);
            BeginRun(goals, routeKey, LOG0);
            std::vector<std::uint64_t> walked;
            bool freshBefore = goals.plan && !goals.planUsed;
            for (std::size_t step = 0; step <= orders.size(); ++step)
            {
                RouteNext const next = AdvanceRoute(goals, open, {});
                if (next.step == RouteStep::Done)
                    break;
                wrong += !next.order || !goals.route.running || !SameOrder(*goals.route.running, *next.order) ||
                         std::none_of(next.order->expected.begin(), next.order->expected.end(), open);
                walked.push_back(next.order->cell);
                ++started;
                freshBefore = freshBefore && !goals.planUsed;
                EndOrder(goals, QuestOrderEnd::Done, LOG0, 2000, BLOCK_MS);
            }
            wrong += walked != expected || goals.route.running.has_value() ||
                     AdvanceRoute(goals, open, {}).step != RouteStep::Done;
            IdleInput idle;
            idle.ready = true;
            idle.freshPlan = goals.plan && !goals.planUsed;  // PlannerService's IdleInput
            idle.jobComing = true;
            notDue += !freshBefore || DecideIdle(idle).action == IdleAction::Apply || goals.stalled.has_value();
        }
        Check(
            wrong == 0 && started > 0,
            "route: the next order is the next one with an expected need still open; an order with none open is never "
            "started; after the last the answer is done (mutant: the next order whatever is open)");
        Check(notDue == 0,
              "route: a done route leaves its plan used, so the idle step waits for a new plan instead of starting the "
              "old one again, and blocks nothing (mutant: planUsed untouched)");
    }

    // A dry end closes the cell and its neighbours for the block time, and nothing else
    for (std::uint32_t const start : {1000000u, 0xFFFFFFFFu - 1000u})
    {
        QuestOrder const x = OrderAt(0, 0, Word::Field, {{1, 0, 3}});
        QuestOrder const beside = OrderAt(1, -1, Word::Field, {{2, 0, 3}});
        QuestOrder const far = OrderAt(2, 0, Word::Field, {{3, 0, 3}});
        QuestOrder const otherMap = OrderAt(0, 0, Word::Field, {{4, 0, 3}}, 1);
        BotGoals goals;
        TakePlan(goals, RouteOf({x, beside, far, otherMap}), 1000);
        BeginRun(goals, routeKey, LOG0);
        bool const first = sameAs(AdvanceRoute(goals, anyOpen, {}), x);
        BlockKey(goals, HUB_KEY, LOG1, start - 1000, BLOCK_MS);  // a goal's stall at the log the order ends at
        bool const fresh = EndOrder(goals, QuestOrderEnd::Dry, LOG1, start, BLOCK_MS);
        // WQ18b: the closed cell's row, which the service writes at once (Planner::OnOrderEnd, NoteStalled)
        bool const dryRow = goals.stallRow.has_value();
        goals.stallRow.reset();
        std::vector<std::uint64_t> const closed = ClosedCells(goals, LOG1, start, BLOCK_MS);
        std::size_t wrongCells = 0;
        for (std::int32_t dx = -3; dx <= 3; ++dx)
            for (std::int32_t dy = -3; dy <= 3; ++dy)
                wrongCells += CellClosed(closed, FieldCellKey(0, dx, dy)) != (std::abs(dx) <= 1 && std::abs(dy) <= 1);
        wrongCells += CellClosed(closed, FieldCellKey(1, 0, 0));
        Check(first && fresh && dryRow && closed == std::vector<std::uint64_t>{x.cell} && wrongCells == 0,
              "route: an order that ended dry closes its cell and the eight around it, on its map (mutant: EndOrder "
              "blocks nothing, or the route's key)");
        Check(goals.stalled && goals.stalled->blocks.size() == 2 && MayStart(goals, routeKey, LOG1, start, BLOCK_MS) &&
                  MayStart(goals, HUB_KEY + 1, LOG1, start, BLOCK_MS) &&
                  !MayStart(goals, HUB_KEY, LOG1, start, BLOCK_MS) &&
                  MayStart(goals, CellBlockKey(far.cell), LOG1, start, BLOCK_MS),
              "route: the dry end blocks nothing else: not the route, no other goal, no other cell");
        Check(ClosedCells(goals, LOG1, start + BLOCK_MS - 1, BLOCK_MS) == closed &&
                  ClosedCells(goals, LOG1, start + BLOCK_MS, BLOCK_MS).empty() &&
                  ClosedCells(goals, LOG1, start, 0).empty(),
              "route: the cell stays closed for the block time and no longer, across the clock's wrap");
        RouteNext const second = AdvanceRoute(goals, anyOpen, closed);
        bool const skipsBeside = sameAs(second, far);
        EndOrder(goals, QuestOrderEnd::Time, LOG1, start + 1, BLOCK_MS);
        bool const mapApart = sameAs(AdvanceRoute(goals, anyOpen, closed), otherMap);
        EndOrder(goals, QuestOrderEnd::Done, LOG1, start + 2, BLOCK_MS);
        Check(skipsBeside && mapApart && AdvanceRoute(goals, anyOpen, closed).step == RouteStep::Done &&
                  ClosedCells(goals, LOG1, start + 2, BLOCK_MS) == closed,
              "route: a closed cell or one beside it is never the next order's; the same cell of another map is "
              "(mutant: AdvanceRoute without the closed cells)");
        // The route's goal ends with the log changed since its start: every goal's stall is lifted, the cell closed
        // at this log stays closed
        EndRun(goals, LOG1, 0, start + 5000, BLOCK_MS);
        Check(
            ClosedCells(goals, LOG1, start + 5000, BLOCK_MS) == closed &&
                MayStart(goals, HUB_KEY, LOG1, start + 5000, BLOCK_MS) && !goals.stallRow,
            "route: the end of the route's goal after a changed log lifts the goals' stalls and keeps the cell closed "
            "(mutant: EndRun resets the list)");
        Check(ClosedCells(goals, LOG2, start + 5000, BLOCK_MS).empty() &&
                  MayStart(goals, CellBlockKey(x.cell), LOG2, start + 5000, BLOCK_MS),
              "route: a changed log opens the cell again, as it lifts every stall");
    }
    {
        std::size_t blocked = 0;
        for (QuestOrderEnd const end : {QuestOrderEnd::Done, QuestOrderEnd::Arrived, QuestOrderEnd::Time,
                                        QuestOrderEnd::Replaced, QuestOrderEnd::NoEnder})
        {
            BotGoals goals;
            TakePlan(goals, RouteOf({OrderAt(0, 0, Word::Field, {{1, 0, 3}})}), 1000);
            BeginRun(goals, routeKey, LOG0);
            AdvanceRoute(goals, anyOpen, {});
            blocked += EndOrder(goals, end, LOG0, 5000, BLOCK_MS) || goals.stalled.has_value() ||
                       goals.route.running.has_value();
            EndRun(goals, LOG0, 0, 6000, BLOCK_MS);
            blocked += goals.stalled.has_value() || goals.stallRow.has_value();
        }
        BotGoals circuit;
        BeginRun(circuit, HUB_KEY, LOG0);
        EndRun(circuit, LOG0, 0, 6000, BLOCK_MS);
        Check(blocked == 0 && circuit.stalled && circuit.stallRow,
              "route: no other end of an order blocks anything, and a route's goal that ends without progress is not "
              "stalled as a circuit's is (mutant: EndRun blocks the route's key)");
        BotGoals refused;
        StartFacts facts;
        facts.key = routeKey;
        facts.log = LOG0;
        facts.nowMs = 7000;
        facts.blockMs = BLOCK_MS;
        StartResult const result =
            StartActive(refused, Goals::GoalKind::Quest, Executor::Route, facts, []() { return false; });
        StartFacts hub = facts;
        hub.key = HUB_KEY;
        BotGoals refusedHub;
        StartActive(refusedHub, Goals::GoalKind::Quest, Executor::Circuit, hub, []() { return false; });
        Check(result == StartResult::Refused && !refused.stalled && !refused.stallRow && refusedHub.stalled,
              "route: a refused start of the route blocks nothing either; a circuit's still blocks its key");
    }

    // TakePlan: the order that is running goes on when the new route begins with it
    {
        QuestOrder const x = OrderAt(0, 0, Word::Field, {{1, 0, 3}});
        QuestOrder const y = OrderAt(4, 0, Word::Field, {{2, 0, 3}});
        QuestOrder const z = OrderAt(8, 0, Word::Field, {{3, 0, 3}});
        QuestOrder refreshed = x;
        refreshed.expected = {{1, 0, 1}, {5, 0, 4}};
        refreshed.minutes = 3.0f;
        BotGoals goals;
        TakePlan(goals, RouteOf({x, y}), 1000);
        BeginRun(goals, routeKey, LOG0);
        bool const runs = sameAs(AdvanceRoute(goals, anyOpen, {}), x);
        TakePlan(goals, RouteOf({refreshed, z}), 2000);
        bool const kept = !goals.route.replaced && goals.route.running && SameOrder(*goals.route.running, x) &&
                          goals.route.running->minutes == x.minutes;
        EndOrder(goals, QuestOrderEnd::Time, LOG0, 3000, BLOCK_MS);
        bool const walksOn = sameAs(AdvanceRoute(goals, anyOpen, {}), z);
        Check(runs && kept && walksOn,
              "route: a refreshed route that begins with the running order leaves it running and never starts it "
              "again; the walk goes on behind it (mutant: every plan starts the route from its first order)");

        BotGoals moved;
        TakePlan(moved, RouteOf({x, y}), 1000);
        BeginRun(moved, routeKey, LOG0);
        AdvanceRoute(moved, anyOpen, {});
        TakePlan(moved, RouteOf({y, x}), 2000);
        bool const replaced = moved.route.replaced;
        bool const givesFirst = sameAs(AdvanceRoute(moved, anyOpen, {}), y) && !moved.route.replaced;
        QuestOrder handIn = x;
        handIn.word = Word::HandIn;
        BotGoals worded;
        TakePlan(worded, RouteOf({x, y}), 1000);
        BeginRun(worded, routeKey, LOG0);
        AdvanceRoute(worded, anyOpen, {});
        TakePlan(worded, RouteOf({handIn, y}), 2000);
        BotGoals emptied;
        TakePlan(emptied, RouteOf({x, y}), 1000);
        BeginRun(emptied, routeKey, LOG0);
        AdvanceRoute(emptied, anyOpen, {});
        TakePlan(emptied, RouteOf({}), 2000);
        BotGoals idle;
        TakePlan(idle, RouteOf({x, y}), 1000);
        TakePlan(idle, RouteOf({y, x}), 2000);
        Check(replaced && givesFirst && worded.route.replaced && emptied.route.replaced &&
                  AdvanceRoute(emptied, anyOpen, {}).step == RouteStep::Done && !idle.route.replaced &&
                  sameAs(AdvanceRoute(idle, anyOpen, {}), y),
              "route: a new route that begins with another order, the same cell under another word or no order "
              "replaces the running one, and the next order is the new route's first; no order running, nothing is "
              "replaced (mutant: the running order always kept)");

        // Every pair of a held and a refreshed route over three cells
        std::vector<QuestOrder> const all = {x, y, z};
        std::size_t pairs = 0;
        std::size_t wrong = 0;
        for (std::uint32_t held = 1; held < 27; ++held)
            for (std::uint32_t fresh = 0; fresh < 27; ++fresh)
            {
                auto const routeFrom = [&all](std::uint32_t code)
                {
                    std::vector<QuestOrder> orders;
                    for (; code; code /= 3)
                        if (std::none_of(orders.begin(), orders.end(),
                                         [&](QuestOrder const& order) { return SameOrder(order, all[code % 3]); }))
                            orders.push_back(all[code % 3]);
                    return orders;
                };
                std::vector<QuestOrder> const before = routeFrom(held);
                std::vector<QuestOrder> const after = routeFrom(fresh);
                BotGoals pair;
                TakePlan(pair, RouteOf(before), 1000);
                BeginRun(pair, routeKey, LOG0);
                AdvanceRoute(pair, anyOpen, {});
                TakePlan(pair, RouteOf(after), 2000);
                bool const same = !after.empty() && SameOrder(after.front(), before.front());
                ++pairs;
                wrong += pair.route.replaced == same;
                if (!same)
                    continue;
                EndOrder(pair, QuestOrderEnd::Time, LOG0, 3000, BLOCK_MS);
                for (RouteNext next = AdvanceRoute(pair, anyOpen, {}); next.step == RouteStep::Order;
                     next = AdvanceRoute(pair, anyOpen, {}))
                    wrong += SameOrder(*next.order, before.front());
            }
        Check(pairs > 600 && wrong == 0,
              "route: over every pair of a held and a refreshed route the running order is replaced exactly when the "
              "new route begins with another, and a kept one is never given again");
    }

    // A new goal starts the route's progress anew; a refused start and an interlude keep it
    {
        QuestOrder const x = OrderAt(0, 0, Word::Field, {{1, 0, 3}});
        QuestOrder const y = OrderAt(4, 0, Word::Field, {{2, 0, 3}});
        BotGoals goals;
        TakePlan(goals, RouteOf({x, y}), 1000);
        BeginRun(goals, routeKey, LOG0);
        AdvanceRoute(goals, anyOpen, {});
        StartFacts facts;
        facts.key = HUB_KEY;
        facts.log = LOG0;
        facts.nowMs = 2000;
        facts.blockMs = BLOCK_MS;
        StartActive(goals, Goals::GoalKind::Grind, Executor::Grind, facts, []() { return false; });
        bool const keptOnRefusal = goals.route.running && goals.route.next == 1;
        StartActive(goals, Goals::GoalKind::Errand, Executor::Errand, facts, []() { return true; });
        bool const keptOnInterlude = goals.route.running && goals.route.next == 1;
        ResumeRoute(goals.route);
        bool const again = sameAs(AdvanceRoute(goals, anyOpen, {}), x);
        TakePlan(goals, RouteOf({y, x}), 3000);
        ResumeRoute(goals.route);
        bool const replacedFirst = sameAs(AdvanceRoute(goals, anyOpen, {}), y);
        facts.key = HUB_KEY + 2;
        StartActive(goals, Goals::GoalKind::Grind, Executor::Grind, facts, []() { return true; });
        Check(
            keptOnRefusal && keptOnInterlude && again && replacedFirst && !goals.route.running && goals.route.next == 0,
            "route: a refused start and an interlude keep the route's progress, the goal resumed after the interlude "
            "gets its interrupted order again (the new first one after a replacing plan), and a new goal starts the "
            "progress anew (mutant: StartActive leaves the route's progress)");
    }
}

// Questing WQ18b: the rules the service walks a bot through its route with: the order at each start of the route's
// executor (NextOrder), the executor's own end of an order (EndRouteOrder), the closed cell's row
void RouteSwitch()
{
    constexpr std::uint32_t BLOCK_MS = 10 * 60 * 1000;
    constexpr std::uint64_t LOG0 = 11;
    using Word = QuestOrderWord;
    auto const anyOpen = [](QuestOrderNeed const&) { return true; };
    auto const sameAs = [](RouteNext const& next, QuestOrder const& order)
    { return next.step == RouteStep::Order && next.order && SameOrder(*next.order, order); };
    QuestOrder const a = OrderAt(0, 0, Word::Field, {{1, 0, 3}});
    QuestOrder const b = OrderAt(5, 0, Word::HandIn, {{2, ORDER_HAND_IN, 1}});
    QuestOrder const c = OrderAt(9, 9, Word::Field, {{4, 0, 5}});
    std::vector<std::uint64_t> const none;
    // A bot whose route's first order runs
    auto const running = [&](std::vector<QuestOrder> orders)
    {
        BotGoals goals;
        TakePlan(goals, RouteOf(std::move(orders)), 1000);
        NextOrder(goals, anyOpen, none);
        return goals;
    };

    {
        BotGoals goals;
        TakePlan(goals, RouteOf({a, b, c}), 1000);
        bool const first = sameAs(NextOrder(goals, anyOpen, none), a);
        bool const again = sameAs(NextOrder(goals, anyOpen, none), a);  // the bot was taken from it: no end came
        bool const ended = !EndRouteOrder(goals, a, QuestOrderEnd::Time, LOG0, 2000, BLOCK_MS) && !goals.route.running;
        bool const second = sameAs(NextOrder(goals, anyOpen, none), b);
        EndRouteOrder(goals, b, QuestOrderEnd::Done, LOG0, 3000, BLOCK_MS);
        bool const third = sameAs(NextOrder(goals, anyOpen, none), c);
        EndRouteOrder(goals, c, QuestOrderEnd::Done, LOG0, 4000, BLOCK_MS);
        bool const done = NextOrder(goals, anyOpen, none).step == RouteStep::Done && goals.planUsed;
        Check(first && again && ended && second && third && done,
              "switch: an order its executor did not end is given again, its own end gives the next, the route's end "
              "uses the plan (mutant: NextOrder without ResumeRoute)");
    }
    {
        BotGoals goals = running({a, b});
        QuestOrder const way = OrderAt(0, 0, Word::Way, {});
        bool const elsewhere = !EndRouteOrder(goals, c, QuestOrderEnd::Dry, LOG0, 2000, BLOCK_MS);
        bool const otherWord = !EndRouteOrder(goals, way, QuestOrderEnd::Dry, LOG0, 2000, BLOCK_MS);
        BotGoals idle;
        TakePlan(idle, RouteOf({a, b}), 1000);
        bool const noneRunning = !EndRouteOrder(idle, a, QuestOrderEnd::Dry, LOG0, 2000, BLOCK_MS);
        Check(elsewhere && otherWord && noneRunning && goals.route.running && SameOrder(*goals.route.running, a) &&
                  !goals.stalled && !goals.stallRow && !idle.stalled,
              "switch: the end of an order that is not the route's running one (a test's order elsewhere, another "
              "word, none running) ends nothing and blocks nothing (mutant: EndRouteOrder without SameOrder)");
    }
    {
        std::size_t quiet = 0;
        for (QuestOrderEnd const end : {QuestOrderEnd::Done, QuestOrderEnd::Arrived, QuestOrderEnd::Time,
                                        QuestOrderEnd::Replaced, QuestOrderEnd::NoEnder, QuestOrderEnd::Bags})
        {
            BotGoals goals = running({a, b});
            quiet += !EndRouteOrder(goals, a, end, LOG0, 2000, BLOCK_MS) && !goals.route.running && !goals.stalled &&
                     !goals.stallRow && ClosedCells(goals, LOG0, 2000, BLOCK_MS).empty();
        }
        BotGoals goals = running({a, b});
        bool const blocked = EndRouteOrder(goals, a, QuestOrderEnd::Dry, LOG0, 2000, BLOCK_MS);
        bool const row = goals.stallRow && goals.stallRow->kind == Goals::GoalKind::Quest && goals.stallRow->judged &&
                         !goals.stallRow->refused && goals.stallRow->outcome == StatusEnd::Outcome::TimedOut &&
                         std::string(goals.stallRow->note) == "dry";
        std::vector<std::uint64_t> const closed = ClosedCells(goals, LOG0, 2000, BLOCK_MS);
        bool const cell = closed.size() == 1 && closed.front() == a.cell;
        goals.stallRow.reset();
        TakePlan(goals, RouteOf({a, b}), 3000);
        bool const given = sameAs(NextOrder(goals, anyOpen, none), a);
        bool const renewed = !EndRouteOrder(goals, a, QuestOrderEnd::Dry, LOG0, 4000, BLOCK_MS) && !goals.stallRow;
        TakePlan(goals, RouteOf({a, b}), 5000);
        bool const passedOver = sameAs(NextOrder(goals, anyOpen, ClosedCells(goals, LOG0, 5000, BLOCK_MS)), b);
        Check(quiet == 6 && blocked && row && cell && given && renewed && passedOver,
              "switch: only a dry end closes the order's cell, with one row (quest, timed_out/dry); the cell dry "
              "again within the block time gets no second row, and the route passes the closed cell over (mutant: "
              "EndOrder without the row)");
    }
    {
        BotGoals goals = running({a, b});
        bool const unmarked = !OrderReplaced(goals);
        TakePlan(goals, RouteOf({c, b}), 2000);
        bool const marked = OrderReplaced(goals);
        EndRouteOrder(goals, a, QuestOrderEnd::Replaced, LOG0, 3000, BLOCK_MS);
        bool const newFirst = sameAs(NextOrder(goals, anyOpen, none), c) && !OrderReplaced(goals);
        BotGoals taken = running({a, b});  // an interlude holds the bot while the new route comes: no end
        TakePlan(taken, RouteOf({c, b}), 2000);
        bool const takenFirst = sameAs(NextOrder(taken, anyOpen, none), c);
        BotGoals same = running({a, b});
        TakePlan(same, RouteOf({a, c}), 2000);
        bool const goesOn = !OrderReplaced(same);
        BotGoals sameTaken = same;
        EndRouteOrder(same, a, QuestOrderEnd::Done, LOG0, 3000, BLOCK_MS);
        bool const onward = sameAs(NextOrder(same, anyOpen, none), c);
        bool const sameAgain = sameAs(NextOrder(sameTaken, anyOpen, none), a);
        BotGoals back = running({a, b});  // replaced, then a plan that begins with the running order again
        TakePlan(back, RouteOf({c, b}), 2000);
        TakePlan(back, RouteOf({a, b}), 3000);
        bool const unmarkedAgain = !OrderReplaced(back);
        Check(unmarked && marked && newFirst && takenFirst && goesOn && onward && sameAgain && unmarkedAgain,
              "switch: a route that begins with another order marks the running one replaced, and its end or an "
              "interlude's end gives the new first order; a route that begins with the running order lets it go on "
              "and follows with its second");
    }
    {
        // WQ18b part 3 (fault F3): the place could not be reached. `time` before the arrival and a failed walk close
        // the cell as dry does; the order is over, and every later plan that still names it passes it over
        BotGoals late = running({a, b});
        bool const unreached = EndRouteOrder(late, a, QuestOrderEnd::Time, LOG0, 2000, BLOCK_MS, false) &&
                               !late.route.running && late.stallRow &&
                               late.stallRow->outcome == StatusEnd::Outcome::TimedOut &&
                               std::string(late.stallRow->note) == "time" &&
                               ClosedCells(late, LOG0, 2000, BLOCK_MS) == std::vector<std::uint64_t>{a.cell};
        std::size_t failed = 0;
        for (StatusEnd::Outcome const outcome : {StatusEnd::Outcome::Stuck, StatusEnd::Outcome::Abandoned,
                                                 StatusEnd::Outcome::TimedOut, StatusEnd::Outcome::Danger})
        {
            BotGoals goals = running({a});
            bool const closed = FailsOrder(outcome) &&
                                FailRouteOrder(goals, a, outcome, "no_walk", LOG0, 2000, BLOCK_MS) &&
                                !goals.route.running && !OrderReplaced(goals);
            bool const row = goals.stallRow && goals.stallRow->kind == Goals::GoalKind::Quest &&
                             goals.stallRow->judged && !goals.stallRow->refused && goals.stallRow->outcome == outcome &&
                             std::string(goals.stallRow->note) == "no_walk";
            goals.stallRow.reset();
            std::vector<std::uint64_t> const cells = ClosedCells(goals, LOG0, 2000, BLOCK_MS);
            bool const done = NextOrder(goals, anyOpen, cells).step == RouteStep::Done && goals.planUsed;
            // The live loop: a plan every 5 s that names the unreachable order again (lab13: 36 and 62 times)
            std::size_t given = 0;
            for (std::uint32_t at = 7000; at < BLOCK_MS; at += 5000)
            {
                TakePlan(goals, RouteOf({a}), at);
                given += NextOrder(goals, anyOpen, ClosedCells(goals, LOG0, at, BLOCK_MS)).step == RouteStep::Order;
            }
            // After the block time the place is tried again, once
            TakePlan(goals, RouteOf({a}), 2000 + BLOCK_MS);
            bool const later =
                sameAs(NextOrder(goals, anyOpen, ClosedCells(goals, LOG0, 2000 + BLOCK_MS, BLOCK_MS)), a);
            failed += closed && row && cells.size() == 1 && cells.front() == a.cell && done && given == 0 && later;
        }
        std::size_t takenOver = 0;
        for (StatusEnd::Outcome const outcome :
             {StatusEnd::Outcome::Interrupted, StatusEnd::Outcome::Ordered, StatusEnd::Outcome::Sick,
              StatusEnd::Outcome::Arrived, StatusEnd::Outcome::Finished})
            takenOver += !FailsOrder(outcome);
        BotGoals other = running({a, b});
        bool const notItsOrder =
            !FailRouteOrder(other, c, StatusEnd::Outcome::Stuck, "no_walk", LOG0, 2000, BLOCK_MS) &&
            other.route.running && !other.stalled && !other.stallRow;
        BotGoals stayed = running({a, b});
        bool const stayPassed = !EndRouteOrder(stayed, a, QuestOrderEnd::Time, LOG0, 2000, BLOCK_MS, true) &&
                                !stayed.stalled && sameAs(NextOrder(stayed, anyOpen, none), b);
        Check(unreached && failed == 4 && takenOver == 5 && notItsOrder && stayPassed,
              "switch: an order whose place could not be reached (time before the arrival; its status stuck, "
              "abandoned, timed out by a walk or given up for danger, AW38b) closes its cell with one row and is "
              "over: a route of that order "
              "alone is done, 119 further plans naming it start none, and it is tried again after the block time; a "
              "stay that passed at the place and a take-over close nothing (mutants: FailOrder without the block, "
              "ClosesCell for dry only)");
    }
    {
        // WQ18b part 3 (fault F2): a bot a test holds on the circuit gets no order of a route it held from before
        BotGoals goals = running({a, b});
        RouteNext const held = NextOrder(goals, anyOpen, none, true);
        bool const ended =
            held.step == RouteStep::Done && !goals.route.running && goals.route.next == 0 && goals.planUsed;
        BotGoals fresh;
        TakePlan(fresh, RouteOf({a, b}), 1000);
        bool const none2 = NextOrder(fresh, anyOpen, none, true).step == RouteStep::Done && !fresh.route.running;
        BotGoals free = running({a, b});
        bool const goesOn = sameAs(NextOrder(free, anyOpen, none, false), a);
        Check(ended && none2 && goesOn,
              "switch: a bot held on the circuit gets no order: the route goal it held ends at the order boundary "
              "and a new route starts none; a free bot is given its order again (mutant: NextOrder without the hold)");
    }
    {
        BotGoals goals = running({a, b});
        OrderNotStarted(goals);
        Check(!goals.route.running && sameAs(NextOrder(goals, anyOpen, none), a),
              "switch: an order its executor refused to start is given again");
    }
}

// One tick's inputs to upstream's grind pick "attack anything" (NewRpgGrindMultiplier)
struct GrindTick
{
    bool led{false};
    bool openWorld{false};
    Active goal;
    bool startsFights{false};
    bool sick{false};
    bool travelling{false};
    bool giverNear{false};    // a giver the bot's status would visit is near
    bool questTarget{false};  // the quest executor holds a target
};

// The multiplier: the pure rule, then upstream's two terms where it leaves the pick to them
bool GrindRuns(GrindTick const& t)
{
    GrindPick const pick = GrindPickOf(t.led, t.openWorld, t.goal, t.startsFights, t.sick, t.travelling);
    return pick == GrindPick::Allow || (pick == GrindPick::Upstream && !t.questTarget && !t.giverNear);
}

// The three multipliers before WQ39 (giver visit with nofight and trip, sickness, quest target), every bot alike
bool GrindRunsBefore(GrindTick const& t)
{
    return t.startsFights && !t.travelling && !t.giverNear && !t.sick && !t.questTarget;
}

// WQ.45's invariant, the count of ticks that break it: a bot the planner leads in the open world picks a mob only
// while its goal is a chosen grind (and never under nofight, sick or on a trip); every other bot picks as before
template <typename Rule>
std::size_t GrindBreaks(Rule rule, std::vector<Active> const& goals)
{
    std::size_t breaks = 0;
    for (int bits = 0; bits < 128; ++bits)
        for (Active const& goal : goals)
        {
            GrindTick t;
            t.led = (bits & 1) != 0;
            t.openWorld = (bits & 2) != 0;
            t.startsFights = (bits & 4) != 0;
            t.sick = (bits & 8) != 0;
            t.travelling = (bits & 16) != 0;
            t.giverNear = (bits & 32) != 0;
            t.questTarget = (bits & 64) != 0;
            t.goal = goal;
            bool const runs = rule(t);
            if (t.led && t.openWorld)
                breaks += runs != (GrindChosen(goal) && t.startsFights && !t.sick && !t.travelling);
            else
                breaks += runs != GrindRunsBefore(t);
        }
    return breaks;
}

void GrindRules()
{
    Active const none;
    Active const grind = GoalOf(Goals::GoalKind::Grind, 12, Source::Planner);
    Active const bridgeGrind = GoalOf(Goals::GoalKind::Grind, 12, Source::Bridge);
    Active const moneySpot = Typed(Goals::GoalKind::Money, Goals::TargetType::Spot, "money");
    Active const moneyNode = Typed(Goals::GoalKind::Money, Goals::TargetType::Node, "money");
    Active const circuit = Typed(Goals::GoalKind::Quest, Goals::TargetType::Default, "circuit");
    Active const route = Typed(Goals::GoalKind::Quest, Goals::TargetType::Route, "route", 0);
    Active const delivery = Typed(Goals::GoalKind::Quest, Goals::TargetType::Quest, "deliver");
    std::vector<Active> others = {none, moneyNode, circuit, route, delivery};
    for (Goals::GoalKind const kind :
         {Goals::GoalKind::MoveZone, Goals::GoalKind::Errand, Goals::GoalKind::BuyBag, Goals::GoalKind::Rest,
          Goals::GoalKind::Gather, Goals::GoalKind::Hearth, Goals::GoalKind::TravelElsewhere, Goals::GoalKind::Fish})
        others.push_back(GoalOf(kind, 7, Source::Planner));

    bool chosen = GrindChosen(grind) && GrindChosen(bridgeGrind) && GrindChosen(moneySpot);
    bool otherChosen = false;
    for (Active const& goal : others)
        otherChosen = otherChosen || GrindChosen(goal);
    Check(chosen && !otherChosen,
          "grind chosen (WQ.45): a grind goal (planner or bridge) and a money goal at a spot; no goal, a money goal at "
          "a node, every quest goal and every other kind are none");

    // An interlude in front of a grind goal: the goal waits behind it and picks no mob meanwhile
    BotGoals goals;
    goals.slot.Set(grind);
    goals.slot.Set(GoalOf(Goals::GoalKind::Errand, 3, Source::Planner));
    Check(goals.slot.Holds(Goals::GoalKind::Grind) && !GrindChosen(goals.slot.Query()),
          "grind chosen: an errand in front of a grind goal is none while it runs");

    std::vector<Active> all = others;
    all.insert(all.end(), {grind, bridgeGrind, moneySpot});
    std::size_t const breaks = GrindBreaks(GrindRuns, all);
    std::size_t const before = GrindBreaks(GrindRunsBefore, all);
    std::printf("  grind ticks: %zu breaks now, %zu with the three old multipliers\n", breaks, before);
    Check(breaks == 0 && before > 0,
          "grind pick (WQ.45): a planner-led open-world bot picks a mob only under a chosen grind, not under "
          "nofight, sick or on a trip; upstream's bots and instances pick as before (mutant: the three old "
          "multipliers break it)");

    // The ticks of the two cases kill-q7 and kill-among-others: a released bot on a test order, no goal, seeking
    GrindTick seeking;
    seeking.led = true;
    seeking.openWorld = true;
    seeking.startsFights = true;
    GrindTick held = seeking;
    held.led = false;
    Check(!GrindRuns(seeking) && GrindRunsBefore(seeking) && GrindRuns(held),
          "grind pick: a led bot seeking its order's next target picks no mob between two targets; a held bot still "
          "does (upstream's kinds keep their pick)");
}

// Questing WQ42 (3.11 part 5): route orders that end one after another where the first of them began, without a
// credit, an accept or a hand-in, end the route's goal as failed at the third such end, and its key is blocked as a
// goal's without progress is (WQ6b's block); any progress or a move counts anew
void RouteInPlace()
{
    constexpr std::uint32_t BLOCK_MS = 10 * 60 * 1000;
    constexpr std::uint64_t LOG0 = 11;
    constexpr std::uint64_t LOG1 = 12;
    constexpr std::uint64_t HUB_KEY = 40;
    using Word = QuestOrderWord;
    Active const routeGoal = Typed(Goals::GoalKind::Quest, Goals::TargetType::Route, Goals::ROUTE_REASON, 0);
    std::uint64_t const routeKey = GoalKey(routeGoal, Executor::Route, HUB_KEY);
    auto const anyOpen = [](QuestOrderNeed const&) { return true; };
    std::vector<std::uint64_t> const none;
    BotSpot const spot{0, 100.0f, 100.0f};
    BotSpot const near{0, 107.0f, 100.0f};  // 7 yd from spot
    BotSpot const away{0, 115.0f, 100.0f};  // 15 yd from spot
    BotSpot const otherMap{1, 100.0f, 100.0f};
    std::vector<QuestOrder> orders;
    for (std::int32_t cell = 0; cell < 8; ++cell)
        orders.push_back(OrderAt(cell * 3, 0, Word::Field, {{static_cast<std::uint32_t>(cell + 1), 0, 3}}));
    // A bot on its route's goal as the service walks it: each order starts where the bot stands (StartRoute) and ends
    // by its own end (replaced) or a failed walk (no_walk); the cell's row is written before the route's
    struct Bot
    {
        BotGoals goals;
        std::uint64_t log{LOG0};
        std::uint32_t nowMs{2000};
        std::vector<StallRow> rows;
    };
    auto const begin = [&]()
    {
        Bot bot;
        TakePlan(bot.goals, RouteOf(orders), 1000);
        BeginRun(bot.goals, routeKey, LOG0);
        return bot;
    };
    auto const order = [&](Bot& bot, BotSpot const& start, BotSpot const& end, bool fails, std::uint64_t endLog)
    {
        RouteNext const next = NextOrder(bot.goals, anyOpen, none);
        if (next.step != RouteStep::Order)
            return;
        QuestOrder const ended = *next.order;
        NoteRouteStart(bot.goals, start, bot.log);
        bot.nowMs += 10000;
        bool const route = IsRouteOrder(bot.goals, ended);
        bool const cell = fails ? FailRouteOrder(bot.goals, ended, StatusEnd::Outcome::Abandoned, "no_walk", endLog,
                                                 bot.nowMs, BLOCK_MS)
                                : EndRouteOrder(bot.goals, ended, QuestOrderEnd::Replaced, endLog, bot.nowMs, BLOCK_MS);
        if (cell)
            bot.rows.push_back(*std::exchange(bot.goals.stallRow, std::nullopt));
        if (route && EndInPlace(bot.goals, end, endLog, bot.nowMs, BLOCK_MS))
            bot.rows.push_back(*std::exchange(bot.goals.stallRow, std::nullopt));
        bot.log = endLog;
    };
    auto const going = [&](Bot const& bot)
    { return bot.goals.run.running && MayStart(bot.goals, routeKey, bot.log, bot.nowMs, BLOCK_MS); };

    {
        Bot bot = begin();
        order(bot, spot, spot, true, LOG0);
        order(bot, spot, near, false, LOG0);
        bool const afterTwo = going(bot) && bot.rows.size() == 1;
        order(bot, spot, spot, true, LOG0);
        StallRow const last = bot.rows.empty() ? StallRow{} : bot.rows.back();
        Check(afterTwo && !bot.goals.run.running && !MayStart(bot.goals, routeKey, LOG0, bot.nowMs, BLOCK_MS) &&
                  bot.rows.size() == 3 && std::string(bot.rows[1].note) == "no_walk" &&
                  last.kind == Goals::GoalKind::Quest && last.judged && last.outcome == StatusEnd::Outcome::Abandoned &&
                  std::string(last.note) == IN_PLACE_WORD,
              "route in place: the third order in a row that ends within 10 yd of where the first began, without "
              "progress and by any end (a failed walk, replaced), ends the route's run and blocks its key, with the "
              "row abandoned/in_place after the closed cell's own (mutant: no bound, EndInPlace ends nothing)");
        TakePlan(bot.goals, RouteOf(orders), 60000);
        StartFacts facts;
        facts.key = routeKey;
        facts.log = LOG0;
        facts.nowMs = bot.nowMs + 1000;
        facts.blockMs = BLOCK_MS;
        int starts = 0;
        StartResult const again = StartActive(bot.goals, Goals::GoalKind::Quest, Executor::Route, facts,
                                              [&]()
                                              {
                                                  ++starts;
                                                  return true;
                                              });
        Check(!bot.goals.run.running && again == StartResult::Stalled && starts == 0 &&
                  MayStart(bot.goals, HUB_KEY, LOG0, facts.nowMs, BLOCK_MS) &&
                  MayStart(bot.goals, routeKey, LOG0, bot.nowMs + BLOCK_MS + 1, BLOCK_MS) &&
                  MayStart(bot.goals, routeKey, LOG1, facts.nowMs, BLOCK_MS),
              "route in place: a fresh plan then starts no route order (the run no longer goes on, a new route goal "
              "stalls) while the bot's other goals may start; the block ends with the block time or a changed log "
              "(mutant: no bound)");
    }
    {
        Bot moved = begin();
        order(moved, spot, spot, true, LOG0);
        order(moved, spot, spot, true, LOG0);
        order(moved, spot, away, true, LOG0);
        bool const reset = going(moved);
        order(moved, away, away, true, LOG0);
        order(moved, away, away, false, LOG0);
        bool const counting = going(moved);
        order(moved, away, away, true, LOG0);
        Check(reset && counting && !going(moved),
              "route in place: an end 15 yd from where the first began counts anew, and three ends at the new spot "
              "fail the route (mutant: the distance is not checked, or the anchor stays at the first spot)");
    }
    {
        Bot progress = begin();
        order(progress, spot, spot, true, LOG0);
        order(progress, spot, spot, true, LOG0);
        order(progress, spot, spot, true, LOG1);
        bool const credit = going(progress) && progress.rows.size() == 3;
        order(progress, spot, spot, true, LOG1);
        order(progress, spot, spot, true, LOG1);
        order(progress, spot, otherMap, true, LOG1);
        bool const map = going(progress);
        Bot released = begin();
        order(released, spot, spot, true, LOG0);
        order(released, spot, spot, true, LOG0);
        Release(released.goals);
        BeginRun(released.goals, routeKey, LOG0);
        order(released, spot, spot, true, LOG0);
        Check(credit && map && going(released) && released.goals.inPlace.count == 1,
              "route in place: an end at another log (a credit, an accept, a hand-in) or on another map counts anew, "
              "and a test's release clears the count (mutant: the log or the map is not checked)");
    }
    {
        BotGoals goals;
        TakePlan(goals, RouteOf(orders), 1000);
        BeginRun(goals, routeKey, LOG0);
        std::uint32_t nowMs = 2000;
        for (int end = 0; end < 4; ++end)
        {
            NextOrder(goals, anyOpen, none);
            QuestOrder const ended = *goals.route.running;
            FailRouteOrder(goals, ended, StatusEnd::Outcome::Abandoned, "no_walk", LOG0, nowMs += 1000, BLOCK_MS);
        }
        Check(goals.run.running && MayStart(goals, routeKey, LOG0, nowMs, BLOCK_MS) && goals.inPlace.count == 0,
              "route in place: an order's end without the bot's spot (a test's order, EndRouteOrder alone) counts "
              "nothing");
    }
}

int main()
{
    RouteRules();
    RouteSwitch();
    RouteInPlace();
    GrindRules();
    // The pinned table of who the planner leads (questing WQ6e, WQ.39): every bot that keeps its possessions and
    // whose planner no test holds, whatever its guid. A bot that keeps none or whose planner a test holds keeps
    // upstream's pick; a bot the planner leads never gets upstream's pick from the idle step
    {
        std::size_t rows = 0;
        std::size_t wrong = 0;
        for (bool const keeps : {true, false})
            for (bool const held : {true, false})
                for (std::uint64_t const guid : {0ull, 10ull, 1230ull, 1ull, 15ull, 1239ull})
                {
                    bool const led = Owns(keeps, !held);
                    ++rows;
                    if (led == (keeps && !held))
                        continue;
                    ++wrong;
                    std::printf("info: pinned row keeps %d held %d guid %llu: led %d\n", keeps, held,
                                static_cast<unsigned long long>(guid), led);
                }
        Check(rows == 24 && wrong == 0,
              "pinned: the planner leads exactly a bot that keeps its possessions and whose planner no test holds; "
              "the guid changes no row (mutant: guid % 10 back in Owns)");
        std::size_t inputs = 0;
        std::size_t upstream = 0;
        std::uint32_t const waits[] = {0, PLAN_WAIT_MS - 1, PLAN_WAIT_MS};
        for (std::uint32_t bits = 0; bits < 32; ++bits)
            for (std::uint32_t const wait : waits)
            {
                IdleInput in;
                in.ready = (bits & 1) != 0;
                in.freshPlan = (bits & 2) != 0;
                in.jobComing = (bits & 4) != 0;
                in.lastRejected = (bits & 8) != 0;
                in.lastFailed = (bits & 16) != 0;
                in.idleForMs = wait;
                IdleDecision const decision = DecideIdle(in);
                bool const ours = decision.action == IdleAction::Apply || decision.action == IdleAction::Wait ||
                                  (decision.action == IdleAction::Fallback && decision.reason != Fallback::None);
                ++inputs;
                upstream += !ours;
            }
        Check(inputs == 96 && upstream == 0,
              "pinned: the idle step of a led bot gives Apply, Wait or a fallback with its reason over all its inputs, "
              "never upstream's pick");
    }

    // Precedence: an external goal outranks the planner's until done or cancelled
    {
        Slot slot;
        Check(slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner)), "the planner sets an empty slot");
        Check(slot.Set(GoalOf(Goals::GoalKind::Grind, 2, Source::Planner)), "the planner replaces its own goal");
        Check(slot.Set(GoalOf(Goals::GoalKind::TravelElsewhere, 3, Source::Bridge)), "the bridge replaces the planner");
        Check(!slot.Set(GoalOf(Goals::GoalKind::Quest, 4, Source::Planner)) && slot.Query().target == 3,
              "the planner cannot replace the bridge's goal");
        Check(slot.HasExternal(), "an external goal makes the bot busy");
        Check(!slot.Cancel(Source::Planner) && slot.Query().Is(), "the planner cannot cancel an external goal");
        Check(slot.Cancel(Source::Bridge) && !slot.Query().Is(), "cancel ends the goal");
        Check(slot.Set(GoalOf(Goals::GoalKind::Quest, 6, Source::Planner)) && !slot.HasExternal(),
              "after the cancel the planner sets again");
    }

    // Interludes: a rest during a quest goal gives the slot back to the quest
    {
        Slot slot;
        slot.Set(GoalOf(Goals::GoalKind::Quest, 7, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(slot.End(0) && slot.Query().kind == Goals::GoalKind::Quest && slot.Query().target == 7,
              "the quest goal goes on after the rest");
        Check(!slot.End(0) && !slot.Query().Is(), "the quest goal then ends for good");
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(!slot.End(0) && !slot.Query().Is(), "an interlude without a goal before it just ends");
        slot.Set(GoalOf(Goals::GoalKind::Quest, 8, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Grind, 9, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(slot.End(0) && slot.Query().kind == Goals::GoalKind::Grind,
              "a new goal replaces the interrupted one: the grind goes on, not the quest");
        slot.Set(GoalOf(Goals::GoalKind::Errand, 0, Source::Planner));
        slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(slot.End(0) && slot.Query().kind == Goals::GoalKind::Grind, "two interludes return to the goal");
    }

    // A test's release (professions WR30): ApplyConstraints ends the planner goals at a hold row turning off, so a
    // released bot's first goal is chosen after the release; a constraint change that releases nothing keeps the goal
    {
        using namespace TestConstraints;
        auto const apply = [](BotGoals& goals, Set before, Set after)
        { return Releases(before, after) && Release(goals); };
        // The control: WR20c-3's old rule (a quest goal under noquest only) leaves every other kind standing
        auto const questOnly = [](BotGoals& goals, Set, Set after)
        {
            if (QuestChoicesAllowed(after) || !goals.slot.Holds(Goals::GoalKind::Quest))
                return false;
            return goals.slot.Cancel(Source::Planner);
        };
        auto const endsEveryKind = [](auto const& rule)
        {
            int ended = 0;
            int kinds = 0;
            for (std::size_t k = 1; k < static_cast<std::size_t>(Goals::GoalKind::Count); ++k)
            {
                BotGoals goals;
                goals.slot.Set(GoalOf(static_cast<Goals::GoalKind>(k), 7, Source::Planner));
                goals.run.running = true;
                ++kinds;
                if (rule(goals, HOLD, Next(HOLD, 0, HOLD & ~NOQUEST)) && !goals.slot.Query().Is() &&
                    !goals.run.running && !goals.stalled)
                    ++ended;
            }
            return ended == kinds;
        };
        Check(endsEveryKind(apply), "a release ends the held goal of every kind, with its run and without a stall");
        Check(!endsEveryKind(questOnly), "control: the quest-only rule leaves the other kinds standing (caught)");

        BotGoals goals;
        goals.slot.Set(GoalOf(Goals::GoalKind::Quest, 8, Source::Planner));
        goals.slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(apply(goals, HOLD, NOQUEST) && !goals.slot.End(0) && !goals.slot.Query().Is(),
              "a quest goal under a kept noquest ends, also behind an interlude (WR20c-3's case)");
        BotGoals routed;
        routed.slot.Set(Typed(Goals::GoalKind::Quest, Goals::TargetType::Route, Goals::ROUTE_REASON));
        routed.slot.Set(GoalOf(Goals::GoalKind::Rest, 0, Source::Planner));
        Check(apply(routed, HOLD, NOQUEST) && !routed.slot.End(0) && !routed.slot.Query().Is(),
              "a route goal (kind quest, target route) under a kept noquest ends too, behind an interlude (WQL5b)");
        goals.plan = std::make_shared<Goals::GoalPlan>();
        goals.planUsed = false;
        goals.slot.Set(GoalOf(Goals::GoalKind::MoveZone, 9, Source::Planner));
        Check(apply(goals, HOLD, PLANNER | STATUS) && goals.planUsed,
              "the plan taken before the release starts no goal after it");
        goals.slot.Set(GoalOf(Goals::GoalKind::Gather, 10, Source::Planner));
        Set const released = NOQUEST;
        Check(!apply(goals, released, Next(released, NOFIGHT, 0)) &&
                  goals.slot.Query().kind == Goals::GoalKind::Gather && !apply(goals, released, released) &&
                  goals.slot.Query().target == 10,
              "a goal chosen after the release stays at a change that releases nothing");
        BotGoals never;
        never.slot.Set(GoalOf(Goals::GoalKind::MoveZone, 11, Source::Planner));
        Check(!apply(never, 0, Next(0, 0, HOLD)) && !apply(never, NOFIGHT, 0) && never.slot.Query().target == 11,
              "a bot never held keeps its goal (a state row turning off is no release)");
        goals.slot.Set(GoalOf(Goals::GoalKind::Quest, 12, Source::Bridge));
        Check(!apply(goals, HOLD, 0) && goals.slot.HasExternal(), "the bridge's quest order stays at a release");

        // A start refused before the hold left only its block: nothing in the slot, so Release ends no goal
        BotGoals refused;
        BlockKey(refused, 13, 5, 0, BLOCK);
        bool const blocked = !MayStart(refused, 13, 5);
        Check(blocked && !Release(refused) && MayStart(refused, 13, 5),
              "a release with an empty slot drops the stall list, so the refused start may start");

        Goals::Ranked empty;
        empty.ok = true;
        std::vector<GoalLog::Row> const rows = GoalLog::Rows(empty);
        Check(rows.size() == 1 && rows[0].choice == "none:0" && rows[0].reason == "no_options",
              "an empty plan gives one goal row, none with no_options");
    }

    // The gain rule (professions WR20c): a gather or money goal that took items or copper made progress without XP;
    // the same gain is none for a grind or a quest goal, and no gain stalls a gather goal
    {
        auto const stalls = [](Goals::GoalKind kind, std::uint64_t gain)
        {
            BotGoals goals;
            goals.run = {77, 5, 100, XpIsProgress(kind), true, false, kind};
            goals.run.startGain = 10;
            goals.run.gainCounts = GainIsProgress(kind);
            GoalApply::EndRun(goals, 5, 100, 0, BLOCK, gain);
            return goals.stalled.has_value();
        };
        Check(!stalls(Goals::GoalKind::Gather, 11) && !stalls(Goals::GoalKind::Money, 11),
              "a gather or money goal that gained items or copper made progress");
        Check(stalls(Goals::GoalKind::Gather, 10) && stalls(Goals::GoalKind::Money, 9),
              "a gather or money goal without a gain stalls (mutant: any end is progress)");
        Check(stalls(Goals::GoalKind::Grind, 11) && stalls(Goals::GoalKind::Quest, 11),
              "a grind or quest goal's gain is no progress (mutant: gain counts for every kind)");
    }

    // The idle step
    {
        IdleInput in;
        in.ready = false;
        in.freshPlan = true;
        Check(DecideIdle(in).reason == Fallback::NotReady, "indexes not ready: fallback_not_ready");
        in.ready = true;
        Check(DecideIdle(in).action == IdleAction::Apply, "a fresh plan is applied");
        in.freshPlan = false;
        in.jobComing = true;
        in.idleForMs = PLAN_WAIT_MS - 1;
        Check(DecideIdle(in).action == IdleAction::Wait, "a coming plan is waited for");
        in.idleForMs = PLAN_WAIT_MS;
        Check(DecideIdle(in).reason == Fallback::Late, "the wait is bounded: fallback_late");
        in.jobComing = false;
        Check(DecideIdle(in).reason == Fallback::NoOption,
              "no plan and none coming: fallback_no_option (WT63e: fallback_no_plan is written no more)");
        in.lastRejected = true;
        Check(DecideIdle(in).reason == Fallback::Rejected, "a rejected answer: fallback_rejected");
        in.lastRejected = false;
        in.lastFailed = true;
        Check(DecideIdle(in).reason == Fallback::Failed, "a failed or lost job: fallback_failed at once");
        in.jobComing = true;
        in.idleForMs = 0;
        Check(DecideIdle(in).action == IdleAction::Wait, "a new job coming after a failure is waited for");
        in.jobComing = false;
        in.lastFailed = false;
        Check(std::string(FallbackWord(Fallback::None)) == "weights" &&
                  std::string(FallbackWord(Fallback::Failed)) == "fallback_failed",
              "the reason words");
    }

    // WT63e: the planner bot's fallback (3.19 (5)): its words, its list and its length
    {
        std::string words;
        for (std::size_t i = 0; i <= static_cast<std::size_t>(Fallback::Count); ++i)
            words += std::string(FallbackWord(static_cast<Fallback>(i))) + " ";
        Check(words ==
                  "weights fallback_not_ready fallback_failed fallback_late fallback_rejected fallback_no_option "
                  "fallback_cannot_start fallback_stalled fallback_no_option_stuck weights ",
              "every reason word, the new ones appended (WT63e three, WN4 one), out of range weights; fallback_no_plan "
              "gone");
        Check(NotStartedReason(0, false) == Fallback::NoOption && NotStartedReason(0, true) == Fallback::NoOption &&
                  NotStartedReason(3, false) == Fallback::CannotStart && NotStartedReason(3, true) == Fallback::Stalled,
              "a plan that started nothing: no option, none could start, every startable one stalled");
        using namespace StatusEnd;
        std::vector<std::uint32_t> const narrow = IdleList(true, false);
        std::vector<std::uint32_t> const full = IdleList(false, false);
        auto const has = [](std::vector<std::uint32_t> const& list, std::uint32_t status)
        { return std::find(list.begin(), list.end(), status) != list.end(); };
        Check(narrow.size() == 3 && has(narrow, STATUS_WANDER_RANDOM) && has(narrow, STATUS_WANDER_NPC) &&
                  has(narrow, STATUS_REST),
              "the planner bot's fallback list: wander random, wander npc, rest");
        bool never = true;
        for (std::uint32_t status : {STATUS_TRAVEL_FLIGHT, STATUS_OUTDOOR_PVP, STATUS_GO_GRIND, STATUS_GO_CAMP,
                                     STATUS_DO_QUEST, STATUS_DO_GATHER, STATUS_TRAVEL, STATUS_ERRANDS})
            never = never && !has(narrow, status) && !FallbackStatus(status);
        Check(never, "the fallback never flies, takes outdoor PvP, a grind or camp walk, a quest or a gather");
        Check(full.size() == 9 && has(full, STATUS_TRAVEL_FLIGHT) && has(full, STATUS_OUTDOOR_PVP) &&
                  has(full, STATUS_GO_GRIND) && has(full, STATUS_GO_CAMP) && has(full, STATUS_DO_QUEST) &&
                  has(full, STATUS_DO_GATHER) && has(full, STATUS_REST),
              "the weighted pick (a bot that keeps no possessions, a held planner) keeps upstream's full list");
        bool same = true;
        for (std::uint32_t status = 0; status < STATUS_COUNT; ++status)
            same = same && FallbackStatus(status) == has(narrow, status);
        Check(same, "FallbackStatus is the list");

        // its length: a goal-less fallback status of the list ends at FALLBACK_STATUS_MS, no other goal-less one
        Check(FALLBACK_STATUS_MS > PLAN_WAIT_MS && FALLBACK_STATUS_MS < 5 * 60000,
              "the fallback is short: longer than the wait for a plan, shorter than upstream's wander timers");
        auto const fallback = [](std::uint32_t status, std::uint32_t statusMs)
        {
            PlannedFacts in;
            in.status = status;
            in.owns = true;
            in.inFallback = true;
            in.statusMs = statusMs;
            return in;
        };
        bool length = true;
        for (std::uint32_t status : narrow)
        {
            PlannedAnswer const before = PlannedEnd(Active{}, fallback(status, FALLBACK_STATUS_MS - 1));
            PlannedAnswer const at = PlannedEnd(Active{}, fallback(status, FALLBACK_STATUS_MS));
            length = length && before.planned && !before.ends && at.planned && at.ends &&
                     at.outcome == Outcome::TimedOut && at.word && std::string(at.word) == "fallback_time";
        }
        Check(length, "each status of the list ends timed out, fallback_time, at its length (not upstream's timer)");
        PlannedFacts noFallback = fallback(STATUS_WANDER_NPC, FALLBACK_STATUS_MS);
        noFallback.inFallback = false;
        PlannedFacts notLed = fallback(STATUS_WANDER_RANDOM, FALLBACK_STATUS_MS);
        notLed.owns = false;
        PlannedFacts sick = fallback(STATUS_REST, FALLBACK_STATUS_MS);
        sick.sick = true;
        Check(!PlannedEnd(Active{}, noFallback).planned && !PlannedEnd(Active{}, notLed).planned &&
                  !PlannedEnd(Active{}, sick).planned &&
                  !PlannedEnd(Active{}, fallback(STATUS_TRAVEL_FLIGHT, FALLBACK_STATUS_MS)).planned &&
                  !PlannedEnd(Active{}, fallback(STATUS_DO_QUEST, FALLBACK_STATUS_MS)).planned,
              "no other goal-less status ends by the fallback's length: a sickness rest, a command, a test's order, "
              "a bot that keeps no possessions or whose planner a test holds, a status off the list keep their "
              "timers");
        Active quest = GoalOf(Goals::GoalKind::Quest, 1, Source::Planner);
        Check(!PlannedEnd(quest, fallback(STATUS_WANDER_NPC, FALLBACK_STATUS_MS)).planned,
              "a status with a goal is no fallback's (mutant: a goal's wander ends at the fallback's length)");

        // NR3: a sick bot's stone in (the errand sick_hearth, its cast in a rest) arrives at the bind, is overrun when
        // the cast never took it there, and runs on until then; a sick bot's other errand has no planned end
        Active hearth = GoalOf(Goals::GoalKind::Errand, 0, Source::Planner);
        hearth.reason = Goals::SICK_HEARTH_REASON;
        hearth.minutes = 1.0f;
        hearth.startedMs = 1000;
        PlannedFacts cast;
        cast.status = STATUS_REST;
        cast.owns = true;
        cast.sick = true;
        cast.nowMs = 1000 + 30000;
        PlannedAnswer const casting = PlannedEnd(hearth, cast);
        PlannedFacts home = cast;
        home.atBind = true;
        PlannedAnswer const arrived = PlannedEnd(hearth, home);
        PlannedFacts late = cast;
        late.nowMs = 1000 + static_cast<std::uint32_t>(ORDER_OVERRUN_FACTOR * 60000.0f);
        PlannedAnswer const overrun = PlannedEnd(hearth, late);
        Active errand = hearth;
        errand.reason = "repair";
        Check(casting.planned && !casting.ends && arrived.ends && arrived.outcome == StatusEnd::Outcome::Arrived &&
                  std::string(arrived.word) == "hearth_bind" && overrun.ends &&
                  overrun.outcome == StatusEnd::Outcome::TimedOut && !PlannedEnd(errand, home).planned,
              "NR3: the sick hearth's rest ends at the bind (mutant: it rests until the sickness ends)");
    }

    // WN4: the no-option loop's bound (WN3's G3: rest, ask, rank nothing, rest again, all night at one spot). OnIdle's
    // answer as PlannerService composes the pure steps: DecideIdle; an applied plan starts (StartActive) or names why
    // not (NotStartedReason); BoundNoOption. Searched, first one answer from every idle input at every count, then
    // every sequence of NO_OPTION_BOUND answers of eleven kinds from every count
    {
        using namespace StatusEnd;
        enum class Applied : std::uint8_t
        {
            Started,
            Interlude,
            NoOptions,
            CannotStart,
            Stalled,
            Count
        };
        struct Answer
        {
            IdleAction action{IdleAction::Fallback};
            Fallback reason{Fallback::None};
            bool started{false};
        };
        auto const answer = [](BotGoals& goals, IdleInput const& in, Applied applied)
        {
            IdleDecision const decision = DecideIdle(in);
            if (decision.action == IdleAction::Wait)
                return Answer{decision.action, Fallback::None, false};
            Fallback reason = decision.reason;
            if (decision.action == IdleAction::Apply)
            {
                if (applied == Applied::Started || applied == Applied::Interlude)
                {
                    Goals::GoalKind const kind =
                        applied == Applied::Started ? Goals::GoalKind::Grind : Goals::GoalKind::Rest;
                    bool const started = StartActive(goals, kind, Executor::Grind, {7, 1, 0, false},
                                                     [] { return true; }) == StartResult::Started;
                    return Answer{IdleAction::Apply, Fallback::None, started};
                }
                reason = applied == Applied::NoOptions ? NotStartedReason(0, false)
                                                       : NotStartedReason(3, applied == Applied::Stalled);
            }
            return Answer{IdleAction::Fallback, BoundNoOption(goals, reason), false};
        };
        std::vector<std::uint32_t> const moving = IdleList(true, true);
        bool movingList = !moving.empty();
        for (std::uint32_t const status : moving)
            movingList = movingList && status != STATUS_REST && FallbackStatus(status);
        Check(movingList && moving.size() == 2 && IdleList(false, true).size() == 9,
              "past the bound the fallback list leaves out rest, keeps wander random and npc (each ends at the "
              "fallback's length); the weighted list is unchanged");
        // The questing test's setting (questing WQ37): a planner bot's fallback is a rest, past the bound too; the
        // weighted pick (a bot whose planner a test holds, or that keeps no possessions) is unchanged
        std::vector<std::uint32_t> const restOnly = {STATUS_REST};
        Check(IdleList(true, false, true) == restOnly && IdleList(true, true, true) == restOnly &&
                  IdleList(false, true, true).size() == 9 && IdleList(false, false, true).size() == 9,
              "questing only: the fallback list is rest alone, also past the bound; the weighted list is unchanged");

        // One answer from every state: 2^5 inputs x 3 waits x counts 0..bound x 5 outcomes of an applied plan
        std::size_t states = 0;
        std::size_t bad = 0;
        std::size_t stuckSeen = 0;
        std::uint32_t const waits[] = {0, PLAN_WAIT_MS - 1, PLAN_WAIT_MS};
        for (std::uint32_t bits = 0; bits < 32; ++bits)
            for (std::uint32_t const wait : waits)
                for (std::uint8_t start = 0; start <= NO_OPTION_BOUND; ++start)
                    for (std::uint8_t a = 0; a < static_cast<std::uint8_t>(Applied::Count); ++a)
                    {
                        IdleInput in;
                        in.ready = (bits & 1) != 0;
                        in.freshPlan = (bits & 2) != 0;
                        in.jobComing = (bits & 4) != 0;
                        in.lastRejected = (bits & 8) != 0;
                        in.lastFailed = (bits & 16) != 0;
                        in.idleForMs = wait;
                        BotGoals goals;
                        goals.noOptionRun = start;
                        Answer const got = answer(goals, in, static_cast<Applied>(a));
                        std::uint8_t const after = goals.noOptionRun;
                        bool ok = true;
                        if (got.action == IdleAction::Wait)  // 1: every wait ends
                            ok = ok && in.jobComing && wait < PLAN_WAIT_MS && after == start;
                        if (got.reason == Fallback::NoOption)
                            ok = ok && start + 1 < NO_OPTION_BOUND && after == start + 1;
                        else if (got.reason == Fallback::NoOptionStuck)
                            ok = ok && start + 1 >= NO_OPTION_BOUND && after == NO_OPTION_BOUND;
                        else if (got.started)  // 3: a started goal resets the count
                            ok = ok && after == 0;
                        else
                            ok = ok && after == start;
                        stuckSeen += got.reason == Fallback::NoOptionStuck;
                        ++states;
                        bad += !ok;
                    }
        std::printf("info: WN4 one answer: %zu states, %zu at the bound, %zu counterexamples\n", states, stuckSeen,
                    bad);
        Check(states == 2880 && bad == 0 && stuckSeen > 0,
              "one answer from every idle state: no-option answers count to the bound and stay there, the bound's "
              "answer is fallback_no_option_stuck, a start resets, nothing else counts");

        // 2: every sequence of NO_OPTION_BOUND answers from every count. A bot that gets no work answers
        // fallback_no_option_stuck by its NO_OPTION_BOUND-th no-option answer at the latest, and from then on every
        // no-option answer is stuck until a goal starts
        struct Kind
        {
            IdleInput in;
            Applied applied{Applied::NoOptions};
        };
        auto const kindOf =
            [](bool fresh, bool coming, std::uint32_t wait, bool rejected, bool failed, bool ready, Applied applied)
        {
            Kind kind;
            kind.in.ready = ready;
            kind.in.freshPlan = fresh;
            kind.in.jobComing = coming;
            kind.in.idleForMs = wait;
            kind.in.lastRejected = rejected;
            kind.in.lastFailed = failed;
            kind.applied = applied;
            return kind;
        };
        std::vector<Kind> const kinds = {
            kindOf(false, false, 0, false, false, true, Applied::NoOptions),            // no plan, none coming
            kindOf(true, false, 0, false, false, true, Applied::NoOptions),             // a plan that ranked nothing
            kindOf(false, true, 0, false, false, true, Applied::NoOptions),             // a wait
            kindOf(false, true, PLAN_WAIT_MS, false, false, true, Applied::NoOptions),  // late
            kindOf(false, false, 0, true, false, true, Applied::NoOptions),             // rejected
            kindOf(false, false, 0, false, true, true, Applied::NoOptions),             // failed
            kindOf(false, false, 0, false, false, false, Applied::NoOptions),           // not ready
            kindOf(true, false, 0, false, false, true, Applied::CannotStart),           // cannot start
            kindOf(true, false, 0, false, false, true, Applied::Stalled),               // stalled
            kindOf(true, false, 0, false, false, true, Applied::Started),               // a goal starts
            kindOf(true, false, 0, false, false, true, Applied::Interlude)};            // an interlude starts
        std::size_t sequences = 0;
        std::size_t badSequences = 0;
        std::size_t total = 1;
        for (std::uint8_t i = 0; i < NO_OPTION_BOUND; ++i)
            total *= kinds.size();
        for (std::uint8_t start = 0; start <= NO_OPTION_BOUND; ++start)
            for (std::size_t code = 0; code < total; ++code)
            {
                BotGoals goals;
                goals.noOptionRun = start;
                std::uint32_t counted = start;  // no-option answers in a row, as the bound should see them
                bool stuck = start >= NO_OPTION_BOUND;
                bool ok = true;
                std::size_t rest = code;
                for (std::uint8_t step = 0; step < NO_OPTION_BOUND; ++step)
                {
                    Kind const& kind = kinds[rest % kinds.size()];
                    rest /= kinds.size();
                    Answer const got = answer(goals, kind.in, kind.applied);
                    bool const noOption = got.reason == Fallback::NoOption || got.reason == Fallback::NoOptionStuck;
                    if (got.started)
                    {
                        counted = 0;
                        stuck = false;
                    }
                    else if (noOption)
                    {
                        ++counted;
                        // within the bound: the bound-th no-option answer in a row is stuck at the latest
                        ok = ok && (got.reason == Fallback::NoOptionStuck) == (counted >= NO_OPTION_BOUND);
                        // and once stuck, stuck until a start
                        ok = ok && (!stuck || got.reason == Fallback::NoOptionStuck);
                        stuck = stuck || got.reason == Fallback::NoOptionStuck;
                    }
                    ok = ok && goals.noOptionRun == std::min<std::uint32_t>(counted, NO_OPTION_BOUND);
                }
                ++sequences;
                badSequences += !ok;
            }
        std::printf("info: WN4 sequences: %zu sequences of %u answers from counts 0..%u, %zu counterexamples\n",
                    sequences, static_cast<unsigned>(NO_OPTION_BOUND), static_cast<unsigned>(NO_OPTION_BOUND),
                    badSequences);
        Check(sequences == total * (NO_OPTION_BOUND + 1) && badSequences == 0,
              "every sequence: the bound's word comes by the bound-th no-option answer in a row, stays until a goal "
              "or an interlude starts; late, failed, rejected, not ready, cannot start and stalled neither count nor "
              "reset");
        BotGoals fromZero;
        Fallback last = Fallback::None;
        for (std::uint8_t i = 0; i < NO_OPTION_BOUND; ++i)
            last = BoundNoOption(fromZero, Fallback::NoOption);
        Check(last == Fallback::NoOptionStuck && std::string(FallbackWord(last)) == "fallback_no_option_stuck" &&
                  BoundNoOption(fromZero, Fallback::Rejected) == Fallback::Rejected,
              "from a fresh bot the bound-th no-option answer is fallback_no_option_stuck; a rejected answer stays");
    }
    // The executors: each quest option goes to what it names
    Reach const away;
    Reach onMap;
    onMap.deliverable = true;
    onMap.sameMap = true;
    Reach otherMap;
    otherMap.deliverable = true;
    Reach there;
    there.sameMap = true;
    there.there = true;
    Active const circuit = Typed(Goals::GoalKind::Quest, Goals::TargetType::Default, Goals::CIRCUIT_REASON, 40);
    Active const delivery = Typed(Goals::GoalKind::Quest, Goals::TargetType::Quest, "delivery", 102);
    Active const hubMove = Typed(Goals::GoalKind::Quest, Goals::TargetType::Default, "circuit_dry", 41);
    Active const campMove = Typed(Goals::GoalKind::Quest, Goals::TargetType::Camp, "hub", 9);
    Active const campCircuit = Typed(Goals::GoalKind::Quest, Goals::TargetType::Camp, Goals::CIRCUIT_REASON, 9);
    Check(ExecutorFor(circuit, away) == Executor::Circuit && ExecutorFor(campCircuit, there) == Executor::Circuit,
          "the plan's own circuit (at a hub or a camp) runs the local circuit");
    Check(ExecutorFor(delivery, onMap) == Executor::Deliver,
          "a delivery on the bot's map: DO_QUEST on the carried quest, which walks to its ender");
    Check(ExecutorFor(delivery, otherMap) == Executor::Travel, "a delivery on another map travels to the ender");
    Check(ExecutorFor(delivery, away) == Executor::None, "a delivery without a complete quest has nothing to do");
    {
        // WT35h: a turn-in given up (no_ender, QuestGiveUp::Noted) GIVE_UP_LIMIT times is not delivered again. A pin of
        // the chain: the record's feed (QuestFail::GivenUp) in the turn-in test the hook passes (PlannerService
        // TestsOf: complete and the picker's workable, !IsGivenUp) gives the delivery no executor on its map
        std::uint32_t constexpr BOT = 7;
        QuestFail::Book record;
        auto const reach = [&record, &delivery]()
        {
            Reach complete;
            complete.sameMap = true;
            complete.deliverable = !QuestFail::IsGivenUp(
                QuestFail::GivenUp(record.RowsOf(BOT), ClassQuests::GIVE_UP_LIMIT), delivery.target);
            return complete;
        };
        Check(ExecutorFor(delivery, reach()) == Executor::Deliver, "a complete quest never given up is delivered");
        record.Note(BOT, delivery.target, "no_ender", 4, 1);
        Check(ExecutorFor(delivery, reach()) == Executor::Deliver,
              "given up once (no_ender), it is delivered once more");
        record.Note(BOT, delivery.target, "no_ender", 3, 2);
        Check(ExecutorFor(delivery, reach()) == Executor::None,
              "given up twice, the delivery is not started again (mutant: the feed without its limit)");
    }
    Check(ExecutorFor(hubMove, away) == Executor::Travel && ExecutorFor(campMove, away) == Executor::Travel,
          "a move to the next hub or camp in the zone travels there (Northshire to Goldshire)");
    Check(ExecutorFor(hubMove, there) == Executor::None,
          "a move that is there already does not start: the next plan's circuit stands for that hub");
    {
        int circuits = 0;
        for (Active const& goal : {delivery, hubMove, campMove})
            for (Reach const& reach : {away, onMap, otherMap, there})
                circuits += ExecutorFor(goal, reach) == Executor::Circuit ? 1 : 0;
        Check(circuits == 0, "no delivery or move runs the local circuit (mutant: every quest option runs it)");
    }
    Check(ExecutorFor(Typed(Goals::GoalKind::Money, Goals::TargetType::Node, "gold"), away) == Executor::Gather &&
              ExecutorFor(Typed(Goals::GoalKind::Money, Goals::TargetType::Spot, "gold"), away) == Executor::Grind,
          "money: gather at a node, grind at a spot");
    Active const zoneMove = Typed(Goals::GoalKind::MoveZone, Goals::TargetType::Camp, "next_zone");
    Active const elsewhere = Typed(Goals::GoalKind::TravelElsewhere, Goals::TargetType::Default, "x");
    Check(ExecutorFor(zoneMove, away) == Executor::Travel && ExecutorFor(elsewhere, away) == Executor::Travel,
          "moves to another zone travel by route");
    Check(ExecutorFor(Typed(Goals::GoalKind::Errand, Goals::TargetType::Default, "x"), away) == Executor::Errand &&
              ExecutorFor(Typed(Goals::GoalKind::BuyBag, Goals::TargetType::Default, "x"), away) == Executor::Errand &&
              ExecutorFor(Typed(Goals::GoalKind::Hearth, Goals::TargetType::Default, "x"), away) == Executor::Bind,
          "errands and bags start through the errand entry, a hearth through the bind entry (WT63d)");

    // The plan's keep list, next task and planned point of interest
    {
        CircuitPlan const plan = SampleCircuit();
        std::vector<std::uint32_t> const keep = KeepList(plan);
        Check(Keeps(keep, 101) && Keeps(keep, 102) && Keeps(keep, 103) && Keeps(keep, 201) && Keeps(keep, 204),
              "taken, carried, deferred, worked and turned-in quests are kept");
        Check(!Keeps(keep, 203) && !Keeps(keep, 999) && !Keeps(keep, 300), "abandons and other quests are not");
        CircuitTask const* task = NextTask(plan, [](std::uint32_t quest) { return quest != 201; });
        Check(task && task->quest == 202, "the next task skips what the bot cannot work");
        Check(!NextTask(plan, [](std::uint32_t) { return false; }), "no workable task: none");
        std::vector<std::pair<float, float>> const points = {{900.0f, 900.0f}, {490.0f, 505.0f}, {120.0f, 90.0f}};
        Check(NearestToTasks(plan, 201, points) == 1, "the point nearest any of the quest's tasks");
        Check(NearestToTasks(plan, 201, {{900.0f, 900.0f}, {120.0f, 90.0f}}) == 1, "a task's nearest point");
        Check(NearestToTasks(plan, 203, points) == 1, "another quest's tasks");
        Check(NearestToTasks(plan, 300, points) == -1, "a quest without tasks: the executor's own pick");
    }

    // Turn-ins: complete log quests are walked to in the ender camps' order, after the incomplete tasks
    {
        CircuitPlan plan = SampleCircuit();
        plan.enderCamps = {7, 3};
        plan.turnIns = {{301, 0, 3}, {302, 0, 7}, {303, 0, 7}, {304, 0, 9}};
        auto const completeOf = [](std::vector<std::uint32_t> quests)
        {
            return [quests](std::uint32_t quest)
            { return std::find(quests.begin(), quests.end(), quest) != quests.end(); };
        };
        CircuitTurnIn const* turnIn = NextTurnIn(plan, completeOf({301, 303, 304}));
        // Mutant: the list's own order would answer 301 (camp 3, the second camp)
        CircuitTurnIn const* listOrder = nullptr;
        for (CircuitTurnIn const& candidate : plan.turnIns)
            if (!listOrder && completeOf({301, 303, 304})(candidate.quest))
                listOrder = &candidate;
        Check(turnIn && turnIn->quest == 303 && listOrder && listOrder->quest != turnIn->quest,
              "the first ender camp's complete quest first, not the list's order (mutant: list order)");
        turnIn = NextTurnIn(plan, completeOf({301, 304}));
        Check(turnIn && turnIn->quest == 301, "then the next camp's");
        turnIn = NextTurnIn(plan, completeOf({304}));
        Check(turnIn && turnIn->quest == 304, "a camp outside enderCamps comes last");
        Check(!NextTurnIn(plan, completeOf({})), "nothing complete: no turn-in");

        // A quest goal's steps: tasks, then turn-ins, then once its givers, then done
        auto const none = [](std::uint32_t) { return false; };
        auto const all = [](std::uint32_t) { return true; };
        QuestGoal goal;
        QuestNext next = AdvanceQuest(goal, plan, all, completeOf({302}));
        Check(next.step == QuestStep::Task && next.quest == 201, "a workable task comes first");
        next = AdvanceQuest(goal, plan, none, completeOf({302}));
        Check(next.step == QuestStep::TurnIn && next.quest == 302,
              "no task left: a complete quest's turn-in (mutant: incomplete quests only)");
        next = AdvanceQuest(goal, plan, none, completeOf({302}));
        Check(next.step == QuestStep::Givers, "each turn-in once per goal, then the givers");
        next = AdvanceQuest(goal, plan, none, completeOf({302}));
        Check(next.step == QuestStep::Done, "the givers once per goal, then the goal ends");
        CircuitPlan bare = SampleCircuit();
        bare.take.clear();
        bare.turnIns.clear();
        QuestGoal idle;
        Check(AdvanceQuest(idle, bare, none, none).step == QuestStep::Done,
              "no takes and no turn-ins: no givers visit");
        // An interlude cut the givers visit short (WT63c, live: an errand trip to a far bank): the resumed goal goes
        // to its givers again instead of ending with nothing left
        ResumeQuest(goal);
        Check(AdvanceQuest(goal, plan, none, none).step == QuestStep::Givers,
              "a resumed goal visits its givers again (mutant: the visit stays spent, the goal ends)");
        Check(AdvanceQuest(goal, plan, none, none).step == QuestStep::Done, "once per resume");

        // The bag capacity edge (WT63c): the first look records, a change is a changed situation, the same is none
        {
            BotGoals seen;
            bool const firstLook = BagSlotsChanged(seen, 16);
            bool const same = BagSlotsChanged(seen, 16);
            bool const grown = BagSlotsChanged(seen, 40);
            bool const lost = BagSlotsChanged(seen, 16);
            Check(!firstLook && !same && grown && lost && seen.bagSlotsSeen == 16,
                  "the bag capacity's change only is an edge (mutant: every look, or never)");
        }

        // NR3: recover's hand-over of a sick bot is an edge only where it turns open; a healthy look closes it, so the
        // next death's hand-over is an edge again
        {
            BotGoals seen;
            bool const closed = SickOpenChanged(seen, false);
            bool const opened = SickOpenChanged(seen, true);
            bool const still = SickOpenChanged(seen, true);
            bool const healthy = SickOpenChanged(seen, false);
            bool const again = SickOpenChanged(seen, true);
            Check(!closed && opened && !still && !healthy && again,
                  "NR3: the sick hand-over is told the planner once per opening (mutant: every check, or once ever)");
        }

        // The limit holds per goal, not per plan: a new plan at every idle keeps the goal's progress
        BotGoals goals;
        auto const shared = std::make_shared<Goals::GoalPlan>();
        shared->circuit = plan;
        TakePlan(goals, shared, 1000);
        BeginRun(goals, 40, 5);
        Check(AdvanceQuest(goals.quest, goals.plan->circuit, all, none).quest == 201, "the goal's first task");
        Check(AdvanceQuest(goals.quest, goals.plan->circuit, none, none).step == QuestStep::Givers, "then its givers");
        TakePlan(goals, shared, 6000);
        Check(goals.planMs == 6000 && goals.lastPlanMs == 1000 && !goals.planUsed, "the new plan is taken");
        Check(AdvanceQuest(goals.quest, goals.plan->circuit, none, none).step == QuestStep::Done,
              "a new plan does not send the bot to the givers again");
        Check(AdvanceQuest(goals.quest, goals.plan->circuit, all, none).quest == 202,
              "nor repeat the goal's started task");
        // Mutant: a plan that resets the goal's progress (WT16-a's per-plan limit) visits the givers again
        QuestGoal perPlan;
        Check(AdvanceQuest(perPlan, goals.plan->circuit, none, none).step == QuestStep::Givers,
              "(mutant: per-plan reset) the loop the per-goal limit ends");
        BeginRun(goals, 41, 5);
        Check(goals.quest.started.empty() && !goals.quest.giversVisited && goals.run.startLog == 5 &&
                  goals.run.key == 41 && goals.run.running,
              "a new goal starts its run and its circuit progress anew");

        // A running circuit goal keeps its own circuit (3.19 (6)): a plan taken away from its hub (an interlude's trip
        // to a far vendor or bank) does not replace it, one for the same circuit refreshes it, and once the goal ends
        // the newest plan's circuit counts again
        BotGoals away;
        auto const home = std::make_shared<Goals::GoalPlan>();
        home->hub = 7;
        auto const city = std::make_shared<Goals::GoalPlan>();
        city->hub = 9;
        TakePlan(away, home, 1000);
        BeginRun(away, CircuitKey(*home), 5);
        TakePlan(away, city, 2000);
        bool const kept = away.circuitPlan == home && away.plan == city;
        auto const homeAgain = std::make_shared<Goals::GoalPlan>(*home);
        TakePlan(away, homeAgain, 3000);
        bool const refreshed = away.circuitPlan == homeAgain;
        EndRun(away, 6);
        TakePlan(away, city, 4000);
        Check(kept && refreshed && away.circuitPlan == city,
              "a circuit goal runs its own hub's circuit while a plan for another comes (mutant: the newest plan's "
              "always, or never refreshed)");
    }

    // The stall rule: a goal that ended without progress does not start again under its key while the log, level and
    // quest items stay the same
    {
        std::vector<QuestSlot> log(2);
        log[0].quest = 201;
        log[1].quest = 302;
        log[1].state = 1;
        std::vector<std::uint16_t> const items(12, 0);
        std::uint64_t const before = LogSignature(log, 10, items);
        Check(before == LogSignature(log, 10, items), "the same log and level: the same signature");
        std::vector<QuestSlot> counted = log;
        counted[0].counters[1] = 3;
        std::vector<QuestSlot> taken = log;
        taken.push_back({});
        taken.back().quest = 205;
        Check(LogSignature(counted, 10, items) != before && LogSignature(taken, 10, items) != before &&
                  LogSignature(log, 11, items) != before,
              "an objective counted, a quest taken or a level gained changes it");
        std::vector<std::uint16_t> collected = items;
        collected[7] = 1;
        Check(LogSignature(log, 10, collected) != before,
              "a quest item collected changes it (mutant: level and slots only); XP is no input, kills lift nothing");

        Goals::GoalPlan hubbed;
        hubbed.hub = 40;
        hubbed.circuit.homeCamp = 40;
        Goals::GoalPlan hubless;
        hubless.circuit.homeCamp = 40;
        Check(CircuitKey(hubbed) == 40 && CircuitKey(hubless) == (CAMP_KEY | 40),
              "a circuit's stall key: its hub, else its home camp, never a hub's key");

        BotGoals goals;
        Check(MayStart(goals, 40, before), "no stall yet: the circuit runs");
        BeginRun(goals, 40, before);
        EndRun(goals, before);
        Check(!goals.run.running && !MayStart(goals, 40, before), "the hub stalled: no goal runs its circuit again");
        // Mutant: a stall that ignores the log would also refuse after progress
        Check(MayStart(goals, 40, LogSignature(taken, 10, items)) && MayStart(goals, 40, LogSignature(log, 11, items)),
              "a changed log or level lifts the stall (mutant: stall by hub only)");
        EndRun(goals, LogSignature(counted, 10, items));
        Check(!MayStart(goals, 40, before), "a goal whose run already ended marks and lifts nothing");
        // A move between two hubs leaves the log as it was: each hub's circuit runs once, then both stay stalled
        Check(MayStart(goals, 41, before), "after a move the next hub's circuit runs once");
        BeginRun(goals, 41, before);
        EndRun(goals, before);
        Check(!MayStart(goals, 41, before) && !MayStart(goals, 40, before),
              "back at the first hub both stay stalled (mutant: one stall entry, the bot bounces between them)");
        BeginRun(goals, 40, before);
        EndRun(goals, LogSignature(counted, 10, items));
        Check(!goals.stalled && MayStart(goals, 40, before), "a goal that made progress lifts every stall");
        for (std::uint64_t key = 100; key < 100 + MAX_STALLED + 1; ++key)
        {
            BeginRun(goals, key, before);
            EndRun(goals, before);
        }
        Check(goals.stalled && goals.stalled->blocks.size() == MAX_STALLED && MayStart(goals, 100, before) &&
                  !MayStart(goals, 101, before),
              "the stall list is bounded: the oldest key starts again first");
    }

    // Change 2 (consolidation CS14e): the status core tells the run why its status ended (NoteStatusEnd). An end the
    // goal did not cause leaves its run unjudged; an end by itself is judged by the log as before
    {
        using StatusEnd::Outcome;
        constexpr std::uint64_t KEY = 50;
        constexpr std::uint64_t LOG = 7;
        auto const begun = [](bool xpCounts = false)
        {
            BotGoals goals;
            BeginRun(goals, KEY, LOG, 100, xpCounts);
            return goals;
        };
        // The run ends with the log unchanged (and the given XP): does its key stall?
        auto const stalls = [](BotGoals& goals, std::uint64_t xp = 100)
        {
            EndRun(goals, LOG, xp);
            return !goals.run.running && !MayStart(goals, KEY, LOG);
        };
        auto const endedBy = [&begun, &stalls](Outcome outcome)
        {
            BotGoals goals = begun();
            NoteStatusEnd(goals, outcome, true);
            return stalls(goals);
        };
        Check(!endedBy(Outcome::Interrupted), "ended from outside, the goal is not stalled: a planner switch");
        Check(!endedBy(Outcome::Ordered), "ended from outside, the goal is not stalled: a hold");
        Check(!endedBy(Outcome::Ordered), "ended from outside, the goal is not stalled: a pause");
        Check(!endedBy(Outcome::Ordered), "ended from outside, the goal is not stalled: an order");
        Check(!endedBy(Outcome::Sick), "ended from outside, the goal is not stalled: sickness");
        Check(endedBy(Outcome::Arrived) && endedBy(Outcome::Finished) && endedBy(Outcome::TimedOut) &&
                  endedBy(Outcome::Stuck) && endedBy(Outcome::Abandoned),
              "arrived, finished, timeout, stuck, abandoned with the log unchanged: stalled as before");
        {
            BotGoals goals = begun();
            NoteStatusEnd(goals, Outcome::Interrupted, true);
            NoteStatusEnd(goals, Outcome::Finished, true);
            Check(!stalls(goals), "the interrupter's own end does not judge the goal (the mark is sticky)");
        }
        {
            BotGoals goals = begun();
            NoteStatusEnd(goals, Outcome::Ordered, true);
            ContinueRun(goals);
            NoteStatusEnd(goals, Outcome::Finished, true);
            Check(stalls(goals), "interrupted, resumed, then ended by itself: stalled (mutant: the mark never clears)");
        }
        {
            BotGoals goals = begun(true);
            NoteStatusEnd(goals, Outcome::Arrived, true);
            NoteStatusEnd(goals, Outcome::TimedOut, true);
            Check(stalls(goals), "go grind arrived, then the wander timed out without XP: stalled as before");
            goals = begun(true);
            NoteStatusEnd(goals, Outcome::Arrived, true);
            NoteStatusEnd(goals, Outcome::TimedOut, true);
            Check(!stalls(goals, 101), "the same chain with XP gained: progress, no stall");
        }
        {
            BotGoals goals = begun();
            EndRun(goals, LOG);
            BeginRun(goals, KEY + 1, LOG);
            NoteStatusEnd(goals, Outcome::Interrupted, true);
            EndRun(goals, LOG + 1);
            Check(!goals.stalled && MayStart(goals, KEY, LOG), "an outside end with a changed log lifts every stall");
            NoteStatusEnd(goals, Outcome::Interrupted, true);
            Check(!goals.run.outside, "an end while no run runs marks nothing");
            BeginRun(goals, KEY, LOG);
            goals.idleSinceMs = 5;
            goals.waitCounted = true;
            NoteStatusEnd(goals, Outcome::Finished, false);
            Check(goals.idleSinceMs == 5 && goals.waitCounted && !goals.run.outside,
                  "a judged end to a next status keeps the wait as it was");
            NoteStatusEnd(goals, Outcome::Finished, true);
            Check(goals.idleSinceMs == 0 && !goals.waitCounted, "an end to idle ends a wait for a plan");
        }
        // A start that replaces the goal's status (CS14f): an errand or inn start ends it as Interrupted, a sick start
        // as Sick, both to a next status: the run is marked, the wait stays as it was
        for (Outcome const outcome : {Outcome::Interrupted, Outcome::Sick})
        {
            BotGoals goals = begun();
            goals.idleSinceMs = 5;
            goals.waitCounted = true;
            NoteStatusEnd(goals, outcome, false);
            bool const sick = outcome == Outcome::Sick;
            Check(goals.run.outside && goals.idleSinceMs == 5 && goals.waitCounted,
                  sick ? "a sick start over the goal's status marks the run and keeps the wait"
                       : "an errand start over the goal's status marks the run and keeps the wait");
            NoteStatusEnd(goals, Outcome::Finished, true);
            Check(!stalls(goals), sick ? "the rest's own end then does not stall the goal"
                                       : "the errand trip's own end then does not stall the goal");
        }
    }

    // Keys: one per goal and executor, never a circuit's
    {
        std::uint64_t const circuitKey = 40;
        std::uint64_t const deliverKey = GoalKey(delivery, Executor::Deliver, circuitKey);
        std::uint64_t const tripKey = GoalKey(delivery, Executor::Travel, circuitKey);
        Check(GoalKey(circuit, Executor::Circuit, circuitKey) == circuitKey &&
                  GoalKey(campCircuit, Executor::Circuit, circuitKey) == circuitKey,
              "every goal that runs the local circuit has the circuit's key, whatever its option named");
        Check(deliverKey != tripKey,
              "a delivery's trip to the ender's map and its turn-in there stall apart (mutant: key without executor)");
        Active const otherQuest = Typed(Goals::GoalKind::Quest, Goals::TargetType::Quest, "delivery", 103);
        Active const sameHubMove = Typed(Goals::GoalKind::Quest, Goals::TargetType::Default, "circuit_dry", 40);
        Active const grind = Typed(Goals::GoalKind::Grind, Goals::TargetType::Spot, "grind", 102);
        std::uint64_t const keys[] = {deliverKey,
                                      tripKey,
                                      GoalKey(otherQuest, Executor::Deliver, circuitKey),
                                      GoalKey(sameHubMove, Executor::Travel, circuitKey),
                                      GoalKey(grind, Executor::Grind, circuitKey),
                                      circuitKey,
                                      CAMP_KEY | 40};
        bool distinct = true;
        for (std::size_t i = 0; i < std::size(keys); ++i)
            for (std::size_t j = i + 1; j < std::size(keys); ++j)
                distinct = distinct && keys[i] != keys[j];
        Check(distinct, "deliveries, moves, grinds and circuits never share a key (a move to hub 40: not its circuit)");
    }

    // Review 2's and 3's loops, one rule: at each idle the bot starts the first option that may start (its key not
    // stalled), else the counted fallback runs; every run here ends with the log unchanged (takes the givers refuse,
    // a delivery that fails at the ender)
    {
        auto const plan = std::make_shared<Goals::GoalPlan>();
        plan->hub = 40;
        plan->circuit = SampleCircuit();
        struct Tally
        {
            int runs = 0;
            int deliveries = 0;
            int trips = 0;
            int fallbacks = 0;
        };
        // circuitOnly: the mutant of fix 2's rule, where only circuit goals have a run
        auto const simulate = [&plan](std::vector<Active> const& ranked, Reach const& reach, std::uint64_t log,
                                      int idles, bool circuitOnly)
        {
            BotGoals goals;
            TakePlan(goals, plan, 1000);
            Tally tally;
            for (int idle = 0; idle < idles; ++idle)
            {
                bool started = false;
                for (Active const& option : ranked)
                {
                    Executor const executor = ExecutorFor(option, reach);
                    if (executor == Executor::None)
                        continue;
                    bool const tracked = !circuitOnly || executor == Executor::Circuit;
                    std::uint64_t const key = GoalKey(option, executor, CircuitKey(*goals.plan));
                    if (tracked && !MayStart(goals, key, log))
                        continue;
                    if (tracked)
                        BeginRun(goals, key, log);
                    tally.runs += executor == Executor::Circuit ? 1 : 0;
                    tally.deliveries += executor == Executor::Deliver ? 1 : 0;
                    tally.trips += executor == Executor::Travel ? 1 : 0;
                    EndRun(goals, log);
                    started = true;
                    break;
                }
                tally.fallbacks += started ? 0 : 1;
            }
            return tally;
        };
        std::vector<Active> const ranked = {circuit, delivery};
        Tally const one = simulate(ranked, onMap, 77, 6, false);
        Check(one.runs == 1 && one.deliveries == 1 && one.fallbacks == 4,
              "the stalled circuit runs once, the failing delivery once, then the counted fallback");
        Tally const old = simulate(ranked, onMap, 77, 6, true);
        Check(old.runs == 1 && old.deliveries == 5 && old.fallbacks == 0,
              "(mutant: only circuits stall) the failing delivery starts at every idle and never falls back");
        // A delivery on another map without a route: the trip aborts at once, stalls, the fallback runs
        Tally const trip = simulate({delivery}, otherMap, 77, 4, false);
        Check(trip.trips == 1 && trip.fallbacks == 3, "a trip that aborts stalls (mutant: travel, abort, again)");
        // A trip that arrived (on the ender's map now, the log unchanged): the turn-in there still starts
        BotGoals goals;
        TakePlan(goals, plan, 1000);
        BeginRun(goals, GoalKey(delivery, ExecutorFor(delivery, otherMap), 40), 77);
        EndRun(goals, 77);
        Check(MayStart(goals, GoalKey(delivery, ExecutorFor(delivery, onMap), 40), 77),
              "after the trip the turn-in at the ender starts (mutant: one key per goal blocks it)");
        // Progress lifts every stall: the circuit and the delivery run again
        Check(MayStart(goals, 40, 78) && MayStart(goals, GoalKey(delivery, Executor::Travel, 40), 78),
              "a changed log lets every stalled goal start again");
        // Interludes have no run: a rest during a run keeps it, so the run ends (and is judged) once
        Reach const restReach;
        Check(ExecutorFor(Typed(Goals::GoalKind::Rest, Goals::TargetType::Default, "rest"), restReach) ==
                      Executor::Rest &&
                  Goals::IsInterlude(Goals::GoalKind::Rest),
              "a rest is an interlude: it starts no run of its own");
        // Mutant: review 2's rules, every quest option runs the circuit and one stall keyed by the option's target
        std::optional<std::uint32_t> stalledTarget;
        int mutantRuns = 0;
        for (int idle = 0; idle < 6; ++idle)
            for (Active const& option : ranked)
            {
                if (stalledTarget && *stalledTarget == option.target)
                    continue;
                ++mutantRuns;
                stalledTarget = option.target;
                break;
            }
        Check(mutantRuns == 6, "(mutant: route by kind, stall by target) the circuit runs at every idle");
    }

    // WQ6b: the same loops with the clock running, over every state: every block ends
    EveryBlockEnds();

    // WT18a: the XP exemption. A grind, gather or money run that gained XP ends without a stall; a quest run's XP is no
    // progress (a kill on the way), and a grind with neither XP nor a log change stalls
    {
        BotGoals goals;
        BeginRun(goals, 50, 7, 1000, XpIsProgress(Goals::GoalKind::Grind));
        Check(goals.run.startXp == 1000 && goals.run.xpCounts, "a grind's run keeps its XP at the start");
        EndRun(goals, 7, 1400);
        Check(!goals.run.running && MayStart(goals, 50, 7),
              "a grind that gained XP does not stall (mutant: XP ignored)");
        BeginRun(goals, 50, 7, 1400, true);
        EndRun(goals, 7, 1400);
        Check(!MayStart(goals, 50, 7), "a grind without XP and without a log change stalls");
        BeginRun(goals, 51, 7, 1400, XpIsProgress(Goals::GoalKind::Gather));
        EndRun(goals, 7, 1500);
        Check(MayStart(goals, 51, 7) && !MayStart(goals, 50, 7),
              "a gather that gained XP does not stall, the grind's stall stays (mutant: XP lifts every stall)");
        BeginRun(goals, 52, 7, 1500, XpIsProgress(Goals::GoalKind::Quest));
        EndRun(goals, 7, 1900);
        Check(!MayStart(goals, 52, 7),
              "a quest goal's XP is no progress: it stalls (mutant: XP counts for every kind)");
        Check(XpIsProgress(Goals::GoalKind::Money) && !XpIsProgress(Goals::GoalKind::TravelElsewhere) &&
                  !XpIsProgress(Goals::GoalKind::Rest),
              "money counts its XP, a trip and a rest do not");
        std::uint64_t const levelUp = std::uint64_t{11} << 32;
        BeginRun(goals, 53, 7, (std::uint64_t{10} << 32) | 9000, true);
        EndRun(goals, 7, levelUp);
        Check(MayStart(goals, 53, 7), "a level gained (XP back to 0) is still XP gained in the level and XP number");
    }

    // WT18a: the bridge's goal orders through the plausibility gate
    {
        std::array<BridgeOrder, 9> const orders = {
            BridgeOrder::GoTo,      BridgeOrder::Quest,     BridgeOrder::Errands,
            BridgeOrder::SetHearth, BridgeOrder::GrindHere, BridgeOrder::GatherHere,
            BridgeOrder::MeetAt,    BridgeOrder::Pause,     BridgeOrder::Resume};
        int searches = 0;
        bool found = true;
        BridgeFacts open;
        open.inScope = true;
        open.placeKnown = true;
        open.questInLog = true;
        open.canGather = true;
        open.route = [&searches, &found]
        {
            ++searches;
            return found;
        };
        bool allPass = true;
        for (BridgeOrder order : orders)
            allPass = allPass && BridgeCheck(order, open) == BridgeAnswer::Done;
        Check(allPass && searches == 2, "every order passes in a plain case; only the two trips search a route");
        auto const answer = [&](BridgeOrder order, auto change)
        {
            BridgeFacts facts = open;
            change(facts);
            searches = 0;
            return BridgeCheck(order, facts);
        };
        bool scope = true;
        for (BridgeOrder order : orders)
        {
            scope = scope && answer(order, [](BridgeFacts& f) { f.inScope = false; }) == BridgeAnswer::NotPlanner &&
                    searches == 0;
        }
        Check(scope, "outside the planner's scope every order is not_planner, before a search (mutant: scope ignored)");
        Check(answer(BridgeOrder::GoTo, [](BridgeFacts& f)
                     { f.current = GoalOf(Goals::GoalKind::Grind, 3, Source::Bridge); }) == BridgeAnswer::Done &&
                  answer(BridgeOrder::GrindHere, [](BridgeFacts& f)
                         { f.current = GoalOf(Goals::GoalKind::Quest, 3, Source::Planner); }) == BridgeAnswer::Done,
              "the bridge's own goal and the planner's give way");
        for (BridgeOrder trip : {BridgeOrder::GoTo, BridgeOrder::MeetAt})
        {
            Check(answer(trip, [](BridgeFacts& f) { f.placeKnown = false; }) == BridgeAnswer::UnknownPlace &&
                      searches == 0,
                  "an unknown place is refused before a search (mutant: no unknown_place)");
            Check(answer(trip, [](BridgeFacts& f) { f.there = true; }) == BridgeAnswer::AlreadyThere && searches == 0,
                  "a bot at the place is already there (mutant: no already_there)");
            Check(answer(trip, [](BridgeFacts& f) { f.openWorld = false; }) == BridgeAnswer::CannotNow && searches == 0,
                  "no trip from inside an instance");
            found = false;
            Check(answer(trip, [](BridgeFacts&) {}) == BridgeAnswer::NoRoute && searches == 1,
                  "no route: no_route after one search (mutant: no no_route)");
            found = true;
            Check(answer(trip, [](BridgeFacts& f) { f.route = nullptr; }) == BridgeAnswer::NoRoute,
                  "without a route search no trip passes");
        }
        Check(answer(BridgeOrder::Quest, [](BridgeFacts& f) { f.questInLog = false; }) == BridgeAnswer::NoSuchQuest,
              "a quest not in the log is no_such_quest (mutant: no no_such_quest)");
        Check(answer(BridgeOrder::GoTo, [](BridgeFacts& f) { f.questInLog = false; }) == BridgeAnswer::Done &&
                  answer(BridgeOrder::Quest, [](BridgeFacts& f) { f.placeKnown = false; }) == BridgeAnswer::Done,
              "each order checks only its own facts");
        Check(answer(BridgeOrder::GrindHere, [](BridgeFacts& f) { f.canGrind = false; }) == BridgeAnswer::NothingHere &&
                  answer(BridgeOrder::GatherHere, [](BridgeFacts& f) { f.canGather = false; }) ==
                      BridgeAnswer::NothingHere,
              "nothing to grind in a town, nothing to gather without a skill (mutant: no nothing_here)");
        Check(
            answer(BridgeOrder::GrindHere, [](BridgeFacts& f) { f.openWorld = false; }) == BridgeAnswer::CannotNow &&
                answer(BridgeOrder::GatherHere, [](BridgeFacts& f) { f.openWorld = false; }) == BridgeAnswer::CannotNow,
            "no grind or gather inside an instance");

        Place const here{0, 1.0f, 2.0f, 3.0f};
        Place const at{1, 10.0f, 20.0f, 30.0f};
        Active const go = BridgeGoalFor(BridgeOrder::GoTo, here, at, 0, 500);
        Active const meet = BridgeGoalFor(BridgeOrder::MeetAt, here, at, 0, 500);
        Check(go.kind == Goals::GoalKind::TravelElsewhere && go.source == Source::Bridge && go.place.map == 1 &&
                  go.place.x == 10.0f && go.reason == "go_to" && go.startedMs == 500 &&
                  ExecutorFor(go, Reach{}) == Executor::Travel,
              "go_to: a bridge trip to the place (mutant: planner source)");
        Check(meet.kind == Goals::GoalKind::TravelElsewhere && meet.reason == "meet_at" && meet.place.map == 1,
              "meet_at: the same trip with the meeting as its reason");
        Active const quest = BridgeGoalFor(BridgeOrder::Quest, here, at, 77, 500);
        Reach working;
        working.workable = true;
        working.sameMap = true;
        Reach done;
        done.deliverable = true;
        done.sameMap = true;
        Reach stuck;
        stuck.sameMap = true;
        Active plannerQuest = quest;
        plannerQuest.source = Source::Planner;
        Check(quest.kind == Goals::GoalKind::Quest && quest.target == 77 &&
                  quest.targetType == Goals::TargetType::Quest && quest.place.map == 0 &&
                  ExecutorFor(quest, working) == Executor::Deliver && ExecutorFor(quest, done) == Executor::Deliver &&
                  ExecutorFor(plannerQuest, working) == Executor::None && ExecutorFor(quest, stuck) == Executor::None,
              "quest: an external quest goal works or turns in its quest; a planner delivery only delivers");
        Active const grind = BridgeGoalFor(BridgeOrder::GrindHere, here, at, 0, 500);
        Active const gather = BridgeGoalFor(BridgeOrder::GatherHere, here, at, 0, 500);
        Check(grind.kind == Goals::GoalKind::Grind && grind.place.map == 0 && grind.place.x == 1.0f &&
                  ExecutorFor(grind, Reach{}) == Executor::Grind && gather.kind == Goals::GoalKind::Gather &&
                  ExecutorFor(gather, Reach{}) == Executor::Gather && grind.reason == "grind_here",
              "grind_here and gather_here: around the bot's own position");
        Check(Goals::IsInterlude(BridgeGoalFor(BridgeOrder::Errands, here, at, 0, 0).kind) &&
                  BridgeGoalFor(BridgeOrder::SetHearth, here, at, 0, 0).kind == Goals::GoalKind::Hearth &&
                  !BridgeGoalFor(BridgeOrder::Pause, here, at, 0, 0).Is() &&
                  !BridgeGoalFor(BridgeOrder::Resume, here, at, 0, 0).Is(),
              "errands and set_hearth are interludes; a pause and a resume set no goal");

        BotGoals goals;
        goals.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        goals.paused = true;
        Check(TakeBridgeGoal(goals, grind) && goals.slot.Query().source == Source::Bridge && !goals.paused,
              "a bridge goal replaces the planner's and ends a pause");
        Check(!goals.slot.Cancel(Source::Planner) && goals.slot.Query().Is(), "the planner never cancels it");
        Check(TakeBridgeGoal(goals, go) && goals.slot.Query().kind == Goals::GoalKind::TravelElsewhere,
              "a new bridge goal replaces the bridge's");
        BotGoals errand;
        errand.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        Check(TakeBridgeGoal(errand, BridgeGoalFor(BridgeOrder::Errands, here, at, 0, 0)) && errand.slot.End(0) &&
                  errand.slot.Query().source == Source::Planner,
              "an errand trip interrupts the planner's goal, which goes on after it");

        BotGoals pausing;
        pausing.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        BeginRun(pausing, 40, 5);
        Check(BridgePause(pausing, true, 1000) && !pausing.slot.Query().Is() && pausing.paused &&
                  !pausing.run.running && !pausing.stalled,
              "a pause ends the planner's goal without a stall");
        Check(StillPaused(pausing, 1000 + PAUSE_MS - 1) && pausing.paused, "a paused bot waits");
        Check(!StillPaused(pausing, 1000 + PAUSE_MS) && !pausing.paused, "a pause ends by itself after PAUSE_MS");
        BotGoals wrapped;
        BridgePause(wrapped, true, 0xFFFFFF00u);
        Check(StillPaused(wrapped, 0xFFFFFF80u) && StillPaused(wrapped, 0x100u),
              "the pause's age survives the clock's wrap");

        // The published view: OnIdle publishes a stale one, also for a bot that then waits for its plan
        BotGoals viewed;
        Check(!ViewStale(viewed), "a fresh bot's view is current");
        TakeBridgeGoal(viewed, grind);
        Check(ViewStale(viewed), "a bridge goal makes the view stale");
        MarkPublished(viewed);
        Check(!ViewStale(viewed) && !viewed.slot.Cancel(Source::Planner) && !ViewStale(viewed),
              "a published view is current; a refused cancel changes nothing");
        Check(!viewed.slot.End(0) && ViewStale(viewed),
              "a bridge goal's end makes the view stale (mutant: a waiting bot keeps showing its ended goal)");
        MarkPublished(viewed);
        viewed.slot.End(0);
        Check(!ViewStale(viewed), "an empty slot's end changes nothing");
        BridgePause(viewed, true, 100);
        MarkPublished(viewed);
        Check(StillPaused(viewed, 100 + PAUSE_MS - 1) && !ViewStale(viewed), "a pause that goes on keeps the view");
        Check(!StillPaused(viewed, 100 + PAUSE_MS) && ViewStale(viewed),
              "a pause's end by itself makes the view stale (mutant: the bot stays busy)");
        BotGoals resumed;
        TakeBridgeGoal(resumed, grind);
        MarkPublished(resumed);
        Check(BridgePause(resumed, false, 0) && ViewStale(resumed), "a resume's cancel of the bridge goal shows");
        BotGoals interlude;
        interlude.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        TakeBridgeGoal(interlude, BridgeGoalFor(BridgeOrder::Errands, here, at, 0, 0));
        MarkPublished(interlude);
        Check(interlude.slot.End(0) && ViewStale(interlude), "an interlude's end, its goal going on, shows");
        BotGoals resuming;
        TakeBridgeGoal(resuming, grind);
        BridgePause(resuming, true, 10);
        TakeBridgeGoal(resuming, grind);
        resuming.situationChanged = false;
        Check(BridgePause(resuming, false, 20) && !resuming.slot.Query().Is() && !resuming.paused &&
                  resuming.situationChanged,
              "a resume ends the bridge's goal and asks for a fresh plan");
        BotGoals own;
        own.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        Check(!BridgePause(own, false, 0) && own.slot.Query().Is(), "a resume keeps the planner's own goal");
    }

    // The accept times the snapshot lacks
    {
        SeenQuests seen;
        UpdateSeen(seen, {5, 3}, 1000);
        UpdateSeen(seen, {3, 7}, 61000);
        std::vector<QuestProgress> const progress = ProgressOf(seen, 121000);
        Check(progress.size() == 2 && progress[0].quest == 3 && progress[0].elapsedMinutes == 2.0f &&
                  progress[1].quest == 7 && progress[1].elapsedMinutes == 1.0f,
              "a log quest's age runs from when it was first seen; a dropped one is forgotten");
    }

    // The goal row: the started option first
    {
        Goals::Ranked ranked;
        ranked.ok = true;
        for (std::uint32_t i = 0; i < 3; ++i)
        {
            Goals::Option option;
            option.kind = Goals::GoalKind::Grind;
            option.target = i;
            ranked.options.push_back(option);
        }
        Goals::Ranked const rotated = ChosenFirst(ranked, 2);
        Check(rotated.options[0].target == 2 && rotated.options[1].target == 0 && rotated.options[2].target == 1,
              "the started option leads the row, the others keep their order");
    }

    // NeedShort: a window opens at the first short plan, stays open while plans stay short, closes at 0
    {
        BotGoals goals;
        bool const quiet = NoteShort(goals, 0, "") == ShortChange::None;
        bool const begins = NoteShort(goals, 120000, "Riding") == ShortChange::Begin && goals.shortOpen;
        bool const stays = NoteShort(goals, 90000, "Repair") == ShortChange::None && goals.shortKind == "Riding";
        bool const ends = NoteShort(goals, 0, "") == ShortChange::End && !goals.shortOpen;
        bool const endKind = goals.shortKind == "Riding";
        bool const again = NoteShort(goals, 0, "") == ShortChange::None &&
                           NoteShort(goals, 500, "Food") == ShortChange::Begin && goals.shortKind == "Food";
        Check(quiet && begins && stays && ends && endKind && again,
              "NeedShort: a begin at the first shortfall, none while it lasts (kind kept), an end at 0, then anew");
    }

    // A switch is one start (3.5, WT63b): StartActive with a fake executor. The fake plays the status core: a start
    // that works ends the running status as interrupted (OnStatusEnd -> NoteStatusEnd), one that fails ends nothing
    {
        using StatusEnd::Outcome;
        constexpr std::uint64_t OLD_KEY = 40;
        constexpr std::uint64_t NEW_KEY = 50;
        constexpr std::uint64_t LOG = 5;
        int ends = 0;
        int calls = 0;
        auto const running = []
        {
            BotGoals goals;
            goals.slot.Set(GoalOf(Goals::GoalKind::Grind, 1, Source::Planner));
            BeginRun(goals, OLD_KEY, LOG);
            goals.quest.started = {7};
            return goals;
        };
        auto const startsOver = [&ends, &calls](BotGoals& goals) -> std::function<bool()>
        {
            return [&ends, &calls, &goals]
            {
                ++calls;
                ++ends;
                NoteStatusEnd(goals, Outcome::Interrupted, false);
                return true;
            };
        };
        std::function<bool()> const refuses = [&calls]
        {
            ++calls;
            return false;
        };
        auto const unchanged = [](BotGoals const& goals)
        {
            return goals.run.running && goals.run.key == OLD_KEY && !goals.run.outside &&
                   goals.quest.started.size() == 1 && goals.slot.Query().kind == Goals::GoalKind::Grind &&
                   !goals.stalled;
        };

        BotGoals refused = running();
        bool const refusedOnce = StartActive(refused, Goals::GoalKind::Quest, Executor::Circuit,
                                             {NEW_KEY, LOG, 0, true, 0, BLOCK}, refuses) == StartResult::Refused;
        bool const refusedBlocked = refused.stalled && refused.stalled->blocks.size() == 1 &&
                                    refused.stalled->blocks[0].key == NEW_KEY && !MayStart(refused, NEW_KEY, LOG) &&
                                    refused.stallRow && refused.stallRow->refused &&
                                    refused.stallRow->kind == Goals::GoalKind::Quest;
        refused.stalled.reset();
        refused.stallRow.reset();
        Check(refusedOnce && calls == 1 && ends == 0 && unchanged(refused) && refusedBlocked,
              "a refused start ends nothing: the run, its progress and the slot as before (mutant: progress lost); "
              "it blocks its key, with a refused row (WQ6b)");
        calls = 0;
        BotGoals none = running();
        Check(StartActive(none, Goals::GoalKind::Errand, Executor::None, {NEW_KEY, LOG}, startsOver(none)) ==
                      StartResult::NoExecutor &&
                  calls == 0 && ends == 0 && unchanged(none),
              "an option without an executor is not started and ends nothing");
        BotGoals stalled = running();
        stalled.stalled = Stalled{LOG, {{NEW_KEY, 0}}};
        Check(StartActive(stalled, Goals::GoalKind::Grind, Executor::Grind, {NEW_KEY, LOG, 0, true, 0, BLOCK},
                          startsOver(stalled)) == StartResult::Stalled &&
                  calls == 0 && ends == 0 && stalled.run.key == OLD_KEY && !stalled.run.outside,
              "a stalled option is not started and ends nothing");
        Check(StartActive(stalled, Goals::GoalKind::Grind, Executor::Grind, {NEW_KEY, LOG, 0, false},
                          startsOver(stalled)) == StartResult::Started,
              "the bridge's order skips the stall rule");

        ends = 0;
        BotGoals switched = running();
        bool const started = StartActive(switched, Goals::GoalKind::Grind, Executor::Grind, {NEW_KEY, LOG, 0},
                                         startsOver(switched)) == StartResult::Started;
        Check(started && ends == 1 && switched.run.running && switched.run.key == NEW_KEY && !switched.run.outside &&
                  switched.quest.started.empty() && !switched.stalled,
              "a switch that starts ends one status, closes the old run without a stall and begins the new one "
              "unmarked (mutant: the run begun before the start)");
        NoteStatusEnd(switched, Outcome::Finished, true);
        EndRun(switched, LOG);
        Check(switched.stalled && switched.stalled->blocks.size() == 1 && switched.stalled->blocks[0].key == NEW_KEY,
              "the switched-to goal's own end is judged: its key stalls, the old one does not");

        BotGoals changed = running();
        changed.stalled = Stalled{LOG, {{99, 0}}};
        StartActive(changed, Goals::GoalKind::Grind, Executor::Grind, {NEW_KEY, LOG + 1, 0}, startsOver(changed));
        Check(!changed.stalled && changed.run.startLog == LOG + 1,
              "closing the old run at a changed log still lifts the stalls");

        // An errand order wins over a running goal: no run of its own, the goal resumes with its run
        BotGoals errand = running();
        Active const order = Typed(Goals::GoalKind::Errand, Goals::TargetType::Default, "stock_due");
        bool const errandStarted = StartActive(errand, order.kind, ExecutorFor(order, Reach{}), {NEW_KEY, LOG},
                                               startsOver(errand)) == StartResult::Started;
        errand.slot.Set(order);
        Check(errandStarted && errand.run.running && errand.run.key == OLD_KEY && errand.run.outside &&
                  errand.quest.started.size() == 1 && errand.slot.Query().kind == Goals::GoalKind::Errand,
              "an errand order starts as an interlude: the goal keeps its run (marked outside) and progress");
        NoteStatusEnd(errand, Outcome::Arrived, true);  // the trip's own end
        bool const resumed = errand.slot.End(0) && errand.slot.Query().kind == Goals::GoalKind::Grind;
        ContinueRun(errand);
        Check(resumed && errand.run.running && errand.run.key == OLD_KEY && !errand.run.outside,
              "after the errand the interrupted goal resumes with its run");
        BotGoals refusedErrand = running();
        Check(
            StartActive(refusedErrand, order.kind, Executor::Errand, {NEW_KEY, LOG}, refuses) == StartResult::Refused &&
                unchanged(refusedErrand),
            "an errand the entry refuses changes nothing");

        // The bridge's errand goal starts through the entry, as an interlude
        Place const here{0, 1.0f, 2.0f, 3.0f};
        Active const bridged = BridgeGoalFor(BridgeOrder::Errands, here, here, 0, 0);
        BotGoals bridge = running();
        ends = 0;
        bool const bridgeStarted = ExecutorFor(bridged, Reach{}) == Executor::Errand &&
                                   StartActive(bridge, bridged.kind, ExecutorFor(bridged, Reach{}),
                                               {NEW_KEY, LOG, 0, false}, startsOver(bridge)) == StartResult::Started;
        Check(bridgeStarted && ends == 1 && TakeBridgeGoal(bridge, bridged) && bridge.run.key == OLD_KEY &&
                  bridge.slot.End(0) && bridge.slot.Query().kind == Goals::GoalKind::Grind,
              "the bridge's errand goal starts through the errand entry and the planner's goal goes on after it");

        // WT63d: a bind order that wins starts through the bind entry as an interlude; the goal resumes after it
        BotGoals bind = running();
        Active const hearth = Typed(Goals::GoalKind::Hearth, Goals::TargetType::Default, "bind_far", 1405);
        ends = 0;
        bool const bindStarted =
            ExecutorFor(hearth, Reach{}) == Executor::Bind &&
            StartActive(bind, hearth.kind, Executor::Bind, {NEW_KEY, LOG}, startsOver(bind)) == StartResult::Started;
        bind.slot.Set(hearth);
        Check(bindStarted && ends == 1 && bind.run.running && bind.run.key == OLD_KEY && bind.run.outside &&
                  bind.quest.started.size() == 1 && bind.slot.Query().kind == Goals::GoalKind::Hearth &&
                  bind.slot.Query().target == 1405,
              "a bind order that wins starts as an interlude: the goal keeps its run and progress, the slot its inn");
        NoteStatusEnd(bind, Outcome::Arrived, true);  // the bind trip's own end (RPG_TRAVEL)
        bool const bindResumed = bind.slot.End(0) && bind.slot.Query().kind == Goals::GoalKind::Grind;
        ContinueRun(bind);
        Check(bindResumed && bind.run.running && bind.run.key == OLD_KEY && !bind.run.outside && !bind.stalled,
              "after the bind the interrupted goal resumes with its run, unstalled");
        BotGoals refusedBind = running();
        Check(StartActive(refusedBind, hearth.kind, Executor::Bind, {NEW_KEY, LOG}, refuses) == StartResult::Refused &&
                  unchanged(refusedBind),
              "a bind the entry refuses (no inn in reach) changes nothing");

        // The bridge's set-hearth starts through the same bind entry; the goal is taken only when it started, a refusal
        // finds nothing to do here
        Active const setHearth = BridgeGoalFor(BridgeOrder::SetHearth, here, here, 0, 0);
        BotGoals bridgeBind = running();
        ends = 0;
        bool const setStarted = ExecutorFor(setHearth, Reach{}) == Executor::Bind && setHearth.target == 0 &&
                                StartActive(bridgeBind, setHearth.kind, ExecutorFor(setHearth, Reach{}),
                                            {NEW_KEY, LOG, 0, false}, startsOver(bridgeBind)) == StartResult::Started;
        Check(setStarted && ends == 1 && TakeBridgeGoal(bridgeBind, setHearth) && bridgeBind.run.key == OLD_KEY &&
                  bridgeBind.slot.End(0) && bridgeBind.slot.Query().kind == Goals::GoalKind::Grind,
              "the bridge's set-hearth starts through the bind entry (no inn named: the entry picks) and the goal goes "
              "on");
        BotGoals bridgeRefused = running();
        Check(StartActive(bridgeRefused, setHearth.kind, Executor::Bind, {NEW_KEY, LOG, 0, false}, refuses) ==
                      StartResult::Refused &&
                  unchanged(bridgeRefused) && BridgeRefused(BridgeOrder::SetHearth) == BridgeAnswer::NothingHere &&
                  BridgeRefused(BridgeOrder::Errands) == BridgeAnswer::NothingHere &&
                  BridgeRefused(BridgeOrder::GoTo) == BridgeAnswer::Failed,
              "a set-hearth the entry refuses takes no goal and answers nothing here (mutant: failed)");
        Check(!Owns(true, false) && Owns(true, true),
              "a bot whose planner a test holds is not asked: its trigger binds as upstream's (mutant: Owns ignores "
              "the hold)");
        BotGoals edge;
        bool const firstLook = BindFarChanged(edge, true);
        bool const stillFar = BindFarChanged(edge, true);
        bool const turnedNear = BindFarChanged(edge, false);
        bool const stillNear = BindFarChanged(edge, false);
        bool const turnedFar = BindFarChanged(edge, true);
        BotGoals nearFirst;
        bool const firstNear = BindFarChanged(nearFirst, false);
        Check(!firstLook && !stillFar && turnedNear && !stillNear && turnedFar && !firstNear,
              "the inn trigger's edge: a bind that turns far or near reports once, the first look records (mutant: "
              "only the far edge reports)");
        // NE3 cause A: the near edge bumps the state version (Planner::SituationChanged), so a plan taken while the
        // bind was far fails its recheck after the bind
        Planner::Result taken;
        taken.stateVersion = 7;
        taken.takenMs = 1000;
        Planner::CurrentState now;
        now.stateVersion = taken.stateVersion + (turnedNear ? 1 : 0);
        now.nowMs = 4000;
        Check(Planner::Recheck(taken, now, Planner::Limits{}) == Planner::Verdict::Version,
              "a plan taken before the bind fails its recheck after it (mutant: the near edge reports nothing)");
    }

    // A failed interlude stamps its kind's retry time (WT63c, 3.19 (6)): the planner passes the kind over until then, a
    // success clears the stamp, and the goal it interrupted resumes without a stall
    {
        using StatusEnd::Outcome;
        constexpr std::uint64_t QUEST_KEY = 11;
        constexpr std::uint64_t ERRAND_KEY = 12;
        constexpr std::uint64_t LOG = 22;
        constexpr std::uint32_t NOW = 1000;
        constexpr std::uint32_t RETRY = WorldTravel::ERRAND_RETRY_MS;
        auto const starts = [] { return true; };
        BotGoals goals;
        Active const quest = GoalOf(Goals::GoalKind::Quest, 7, Source::Planner);
        bool const questStarted =
            StartActive(goals, quest.kind, Executor::Circuit, {QUEST_KEY, LOG}, starts) == StartResult::Started;
        goals.slot.Set(quest);
        Active const errand = GoalOf(Goals::GoalKind::Errand, 77, Source::Planner);
        bool const errandStarted =
            StartActive(goals, errand.kind, Executor::Errand, {ERRAND_KEY, LOG}, starts) == StartResult::Started;
        NoteStatusEnd(goals, Outcome::Interrupted, false);  // the quest's status, cut by the errand's start
        goals.slot.Set(errand);
        NoteStatusEnd(goals, Outcome::TimedOut, true);
        NoteInterludeEnd(goals, errand.kind, Outcome::TimedOut, NOW);
        Check(questStarted && errandStarted && RetryWaits(goals, Goals::GoalKind::Errand, NOW + 1) &&
                  RetryWaits(goals, Goals::GoalKind::Errand, NOW + RETRY - 1) &&
                  !RetryWaits(goals, Goals::GoalKind::Errand, NOW + RETRY),
              "a failed errand is passed over for the retry time, then weighed again (mutant: no stamp)");
        Check(
            !RetryWaits(goals, Goals::GoalKind::BuyBag, NOW + 1) && !RetryWaits(goals, Goals::GoalKind::Quest, NOW + 1),
            "the stamp holds back its own kind only");
        bool const resumed = goals.slot.End(0) && goals.slot.Query().kind == Goals::GoalKind::Quest;
        ContinueRun(goals);
        Check(resumed && goals.run.running && goals.run.key == QUEST_KEY && !goals.run.outside &&
                  MayStart(goals, QUEST_KEY, LOG),
              "after the failed errand the interrupted goal resumes with its run and does not stall");
        NoteInterludeEnd(goals, Goals::GoalKind::Errand, Outcome::Interrupted, NOW + 5);
        Check(RetryWaits(goals, Goals::GoalKind::Errand, NOW + 6), "an end from outside leaves the stamp as it was");
        NoteInterludeEnd(goals, Goals::GoalKind::Errand, Outcome::Finished, NOW + 10);
        Check(!RetryWaits(goals, Goals::GoalKind::Errand, NOW + 11), "a finished errand clears the stamp");
        BotGoals other;
        NoteInterludeEnd(other, Goals::GoalKind::BuyBag, Outcome::Interrupted, NOW);
        NoteInterludeEnd(other, Goals::GoalKind::Quest, Outcome::TimedOut, NOW);
        Check(
            !RetryWaits(other, Goals::GoalKind::BuyBag, NOW + 1) && !RetryWaits(other, Goals::GoalKind::Quest, NOW + 1),
            "an interrupted interlude and a goal that is no interlude stamp nothing");
        NoteInterludeEnd(other, Goals::GoalKind::Rest, Outcome::TimedOut, NOW);
        Check(!RetryWaits(other, Goals::GoalKind::Rest, NOW + 1),
              "a rest that ran its time stamps nothing (mutant: every rest waits out the retry time)");
        constexpr std::uint32_t LATE = 0xFFFFFF00u;
        NoteInterludeEnd(other, Goals::GoalKind::BuyBag, Outcome::Stuck, LATE);
        Check(RetryWaits(other, Goals::GoalKind::BuyBag, 0x100u), "the wait holds across the clock's wrap");
        // WT63d: a bind that failed (the trip timed out, got stuck or was abandoned) stamps the hearth kind; the goal
        // it interrupted resumes without a stall; a bind set (arrived) clears the stamp
        BotGoals hearth;
        Active const quest2 = GoalOf(Goals::GoalKind::Quest, 7, Source::Planner);
        StartActive(hearth, quest2.kind, Executor::Circuit, {QUEST_KEY, LOG}, starts);
        hearth.slot.Set(quest2);
        Active const bind = GoalOf(Goals::GoalKind::Hearth, 1405, Source::Planner);
        bool const bindStarted =
            StartActive(hearth, bind.kind, Executor::Bind, {ERRAND_KEY, LOG}, starts) == StartResult::Started;
        NoteStatusEnd(hearth, Outcome::Interrupted, false);
        hearth.slot.Set(bind);
        bool allFailures = true;
        for (Outcome const outcome : {Outcome::TimedOut, Outcome::Stuck, Outcome::Abandoned})
        {
            BotGoals each;
            NoteInterludeEnd(each, Goals::GoalKind::Hearth, outcome, NOW);
            allFailures = allFailures && RetryWaits(each, Goals::GoalKind::Hearth, NOW + RETRY - 1) &&
                          !RetryWaits(each, Goals::GoalKind::Hearth, NOW + RETRY) &&
                          !RetryWaits(each, Goals::GoalKind::Errand, NOW + 1);
        }
        NoteStatusEnd(hearth, Outcome::Stuck, true);
        NoteInterludeEnd(hearth, hearth.slot.Query().kind, Outcome::Stuck, NOW);
        bool const passedOver = RetryWaits(hearth, Goals::GoalKind::Hearth, NOW + 1);
        bool const bindResumed = hearth.slot.End(0) && hearth.slot.Query().kind == Goals::GoalKind::Quest;
        ContinueRun(hearth);
        Check(bindStarted && allFailures && passedOver && bindResumed && hearth.run.running &&
                  hearth.run.key == QUEST_KEY && MayStart(hearth, QUEST_KEY, LOG),
              "a failed bind stamps the hearth's retry time and the interrupted goal resumes unstalled (mutant: no "
              "stamp for hearth)");
        NoteInterludeEnd(hearth, Goals::GoalKind::Hearth, Outcome::Arrived, NOW + 10);
        Check(!RetryWaits(hearth, Goals::GoalKind::Hearth, NOW + 11), "a bind set at the inn clears the stamp");
        // WT63d-b: an arrival at the inn whose bind did not take is no arrival: it stamps the hearth kind, so the
        // planner does not send the bot to an inn again within the retry time (lab14: a bot ordered the same inn six
        // times in 54 s while its bind never took); a bind that took clears it
        BotGoals loop;
        loop.slot.Set(bind);
        NoteInterludeEnd(loop, loop.slot.Query().kind, WorldTravel::InnEndOf(false, WorldTravel::InnStep::Bind).outcome,
                         NOW);
        bool const unboundStamps = RetryWaits(loop, Goals::GoalKind::Hearth, NOW + RETRY - 1);
        BotGoals stuckOut;
        NoteInterludeEnd(stuckOut, Goals::GoalKind::Hearth,
                         WorldTravel::InnEndOf(false, WorldTravel::InnStep::Fail).outcome, NOW);
        BotGoals took;
        NoteInterludeEnd(took, Goals::GoalKind::Hearth, Outcome::Abandoned, NOW);
        NoteInterludeEnd(took, Goals::GoalKind::Hearth, WorldTravel::InnEndOf(true, WorldTravel::InnStep::Bind).outcome,
                         NOW + 1);
        Check(unboundStamps && RetryWaits(stuckOut, Goals::GoalKind::Hearth, NOW + 1) &&
                  !RetryWaits(took, Goals::GoalKind::Hearth, NOW + 2),
              "an unbound arrival at the inn stamps the hearth's retry time, a bind that took clears it (mutant: the "
              "unbound end arrives)");
    }

    // A plan that arrives during a fallback status is taken at once; other goal-less statuses stay
    {
        BotGoals goals;
        goals.plan = std::make_shared<Goals::GoalPlan>();
        std::const_pointer_cast<Goals::GoalPlan>(goals.plan)->ranked.ok = true;
        Check(OnFreshPlan(goals, true) == FreshPlan::Keep,
              "a goal-less status that is no fallback (sickness, a command) keeps going");
        NoteFallback(goals, Fallback::NoOption, 100);
        Check(goals.lastFallback == Fallback::NoOption && goals.lastFallbackMs == 100 &&
                  OnFreshPlan(goals, true) == FreshPlan::Take,
              "during a fallback status a usable plan is taken at once (mutant: it waits for the status's end)");
        Check(OnFreshPlan(goals, false) == FreshPlan::Keep, "a fallback status that may not be cut stays");
        BotGoals unusable = goals;
        unusable.plan = std::make_shared<Goals::GoalPlan>();
        Check(OnFreshPlan(unusable, true) == FreshPlan::Keep, "a plan without a ranking is not taken");
        NoteStatusEnd(goals, StatusEnd::Outcome::Interrupted, false);
        Check(!goals.inFallback && OnFreshPlan(goals, true) == FreshPlan::Keep,
              "any end of the fallback status ends the fallback (mutant: the flag sticks)");
        goals.slot.Set(GoalOf(Goals::GoalKind::Quest, 1, Source::Planner));
        Check(OnFreshPlan(goals, true) == FreshPlan::Replan && OnFreshPlan(goals, false) == FreshPlan::Keep,
              "a planner goal is replanned only in a status a switch may cut");
        goals.slot.Set(GoalOf(Goals::GoalKind::Grind, 1, Source::Bridge));
        Check(OnFreshPlan(goals, true) == FreshPlan::Keep, "an external goal is never switched");
    }

    // NR3: a goal-less sickness rest yields to a plan whose best option passed the sick gate (Ranked::sickGoal, set by
    // the goal model: GoalModelHarness SickSafeAreas); any other plan keeps it, as it keeps a command
    {
        BotGoals goals;
        std::shared_ptr<Goals::GoalPlan> const plan = std::make_shared<Goals::GoalPlan>();
        plan->ranked.ok = true;
        Goals::Option errand;
        errand.kind = Goals::GoalKind::Errand;
        errand.due = true;
        errand.reason = "repair";
        plan->ranked.options.push_back(errand);
        goals.plan = plan;
        Check(OnFreshPlan(goals, true) == FreshPlan::Keep,
              "a due errand of a plan not made for a sick bot does not cut a goal-less status (a command)");
        plan->ranked.sickGoal = true;
        Check(OnFreshPlan(goals, true) == FreshPlan::Take,
              "NR3: the sickness rest yields to the option the sick gate passed (mutant: kept, the bot rests out the "
              "sickness)");
        Check(OnFreshPlan(goals, false) == FreshPlan::Keep, "a status a switch may not cut stays");
        plan->ranked.options.front().due = false;
        Check(OnFreshPlan(goals, true) == FreshPlan::Take,
              "NR3: an errand that is not due (full bags in a capital) replaces the sickness rest too");
        plan->ranked.sickGoal = false;
        plan->ranked.options.front().kind = Goals::GoalKind::Rest;
        Check(OnFreshPlan(goals, true) == FreshPlan::Keep, "a sick plan whose best option is a rest keeps the rest");
    }

    // WT63e: a planned status ends by its order's condition (3.19 (4)): the end table per kind and the overrun bound
    {
        using Goals::GoalKind;
        using StatusEnd::Outcome;
        constexpr std::uint32_t START = 1000;
        constexpr std::uint32_t MINUTE = 60000;
        constexpr float MINUTES = 10.0f;
        constexpr std::uint32_t AT = START + 10 * MINUTE;
        std::uint32_t const OVER = START + static_cast<std::uint32_t>(ORDER_OVERRUN_FACTOR * MINUTES * MINUTE);
        auto const order = [](GoalKind kind, Source source = Source::Planner)
        {
            Active goal = GoalOf(kind, 1, source);
            goal.minutes = MINUTES;
            goal.startedMs = START;
            return goal;
        };
        auto const facts = [](std::uint32_t status, std::uint32_t nowMs, float health = 100.0f, float mana = 100.0f)
        {
            PlannedFacts in;
            in.status = status;
            in.nowMs = nowMs;
            in.owns = true;
            in.healthPercent = health;
            in.manaPercent = mana;
            return in;
        };
        auto const goes = [](PlannedAnswer const& a) { return a.planned && !a.ends; };
        auto const ends = [](PlannedAnswer const& a, Outcome outcome, char const* word)
        { return a.planned && a.ends && a.outcome == outcome && a.word && std::string(a.word) == word; };
        using namespace StatusEnd;

        // grind, money at a spot: the minutes at the spot, the overrun on the way
        for (GoalKind kind : {GoalKind::Grind, GoalKind::Money})
        {
            Active const goal = order(kind);
            Check(goes(PlannedEnd(goal, facts(STATUS_WANDER_RANDOM, AT - 1))) &&
                      ends(PlannedEnd(goal, facts(STATUS_WANDER_RANDOM, AT)), Outcome::Finished, "order_time"),
                  "a planned grind or money spot ends finished order_time when the order's minutes are over (mutant: "
                  "upstream's timer)");
            Check(goes(PlannedEnd(goal, facts(STATUS_GO_GRIND, AT))) &&
                      goes(PlannedEnd(goal, facts(STATUS_GO_GRIND, OVER - 1))) &&
                      ends(PlannedEnd(goal, facts(STATUS_GO_GRIND, OVER)), Outcome::TimedOut, "order_overrun"),
                  "on the way to the spot only the overrun ends it, timed out order_overrun at the factor times the "
                  "minutes");
        }
        // gather, money at a node: the executor's own ends (circuit_done, a stop's ends) and the overrun (WR13d-2)
        for (GoalKind kind : {GoalKind::Gather, GoalKind::Money})
            Check(goes(PlannedEnd(order(kind), facts(STATUS_DO_GATHER, AT))) &&
                      goes(PlannedEnd(order(kind), facts(STATUS_DO_GATHER, OVER - 1))) &&
                      ends(PlannedEnd(order(kind), facts(STATUS_DO_GATHER, OVER)), Outcome::TimedOut, "order_overrun"),
                  "a planned gather goes on past its minutes (its circuit ends it) and ends timed out order_overrun at "
                  "the factor times the minutes (mutant: order_time at the minutes)");
        Check(!PlannedEnd(order(GoalKind::Gather), facts(STATUS_WANDER_RANDOM, OVER)).planned &&
                  !PlannedEnd(order(GoalKind::Grind), facts(STATUS_DO_GATHER, OVER)).planned &&
                  !PlannedEnd(order(GoalKind::Grind), facts(STATUS_WANDER_NPC, OVER)).planned,
              "a status that does not carry the goal's kind out is not planned: upstream's timer");
        // rest: rested at the model's top threshold, the overrun, the sick rest's own end
        Active const rest = order(GoalKind::Rest);
        Check(ends(PlannedEnd(rest, facts(STATUS_REST, START, RESTED_PERCENT, RESTED_PERCENT)), Outcome::Finished,
                   "rested") &&
                  ends(PlannedEnd(rest, facts(STATUS_REST, START, 100.0f, -1.0f)), Outcome::Finished, "rested"),
              "a planned rest ends finished rested once health and mana (a bot without mana: health) are back at the "
              "rested threshold");
        Check(goes(PlannedEnd(rest, facts(STATUS_REST, OVER - 1, RESTED_PERCENT - 1.0f, 100.0f))) &&
                  goes(PlannedEnd(rest, facts(STATUS_REST, OVER - 1, 100.0f, RESTED_PERCENT - 1.0f))) &&
                  ends(PlannedEnd(rest, facts(STATUS_REST, OVER, 50.0f, 100.0f)), Outcome::TimedOut, "order_overrun"),
              "a rest not yet rested goes on (its minutes are no end) until the overrun bound");
        Check(RESTED_PERCENT > Goals::REST_MAX * Goals::PERCENT_F - 0.01f,
              "the rested threshold is at or above every bot's rest threshold: the option does not re-fire at once");
        PlannedFacts sick = facts(STATUS_REST, START, 100.0f, 100.0f);
        sick.sick = true;
        Check(!PlannedEnd(rest, sick).planned, "a sick bot's rest is no planned end: Upkeep::RestEnds decides");
        // the kinds whose executors end them: never PlannedEnd, in any status
        bool none = true;
        for (GoalKind kind : {GoalKind::Quest, GoalKind::MoveZone, GoalKind::TravelElsewhere, GoalKind::Errand,
                              GoalKind::BuyBag, GoalKind::Hearth})
            for (std::uint32_t status = STATUS_IDLE; status < STATUS_COUNT; ++status)
                none = none && !PlannedEnd(order(kind), facts(status, OVER)).planned;
        Check(none,
              "quest, circuit, delivery, move, travel, errand, buy_bag and hearth keep their executors' ends "
              "and timers (mutant: PlannedEnd answers them)");
        // not planned: a bot the planner does not lead, a bridge goal, a fallback status, an order without minutes
        PlannedFacts notLed = facts(STATUS_WANDER_RANDOM, OVER);
        notLed.owns = false;
        PlannedFacts fallback = facts(STATUS_WANDER_RANDOM, OVER);
        fallback.inFallback = true;
        Active noMinutes = order(GoalKind::Grind);
        noMinutes.minutes = 0.0f;
        Check(!PlannedEnd(order(GoalKind::Grind), notLed).planned &&
                  !PlannedEnd(order(GoalKind::Grind, Source::Bridge), facts(STATUS_WANDER_RANDOM, OVER)).planned &&
                  !PlannedEnd(order(GoalKind::Grind), fallback).planned &&
                  !PlannedEnd(noMinutes, facts(STATUS_WANDER_RANDOM, OVER)).planned &&
                  !PlannedEnd(Active{}, facts(STATUS_WANDER_RANDOM, OVER)).planned,
              "a bot the planner does not lead, a bridge goal, a fallback status and an order without minutes keep "
              "upstream's timer");
    }

    // WT63e review: an interlude's time does not count against the order it interrupts (3.19 (6)): the resumed goal's
    // clock moves on by the interlude's length (Slot::End), so a resumed order ends by its own minutes
    {
        using Goals::GoalKind;
        using namespace StatusEnd;
        constexpr std::uint32_t MINUTE = 60000;
        constexpr std::uint32_t START = 1000;
        auto const grind = [](std::uint32_t startedMs)
        {
            Active goal = GoalOf(GoalKind::Grind, 1, Source::Planner);
            goal.minutes = 10.0f;
            goal.startedMs = startedMs;
            return goal;
        };
        auto const interlude = [](GoalKind kind, std::uint32_t startedMs)
        {
            Active goal = GoalOf(kind, 0, Source::Planner);
            goal.startedMs = startedMs;
            return goal;
        };
        auto const facts = [](std::uint32_t status, std::uint32_t nowMs)
        {
            PlannedFacts in;
            in.status = status;
            in.nowMs = nowMs;
            in.owns = true;
            return in;
        };
        auto const word = [](PlannedAnswer const& a, char const* w)
        { return a.planned && a.ends && a.word && std::string(a.word) == w; };
        auto const goes = [](PlannedAnswer const& a) { return a.planned && !a.ends; };

        // a walk of 3 minutes, a 20-minute errand trip, the walk resumed
        Slot walk;
        walk.Set(grind(START));
        walk.Set(interlude(GoalKind::Errand, START + 3 * MINUTE));
        std::uint32_t const back = START + 23 * MINUTE;
        bool const resumed = walk.End(back) && walk.Query().kind == GoalKind::Grind;
        Check(resumed && walk.Query().startedMs == START + 20 * MINUTE &&
                  goes(PlannedEnd(walk.Query(), facts(STATUS_GO_GRIND, back))),
              "a walk resumed after a 20-minute errand counts only its own 3 minutes: no order_overrun at once "
              "(mutant: the clock counts the interlude)");
        std::uint32_t const over =
            START + 20 * MINUTE + static_cast<std::uint32_t>(ORDER_OVERRUN_FACTOR * 10.0f * MINUTE);
        Check(goes(PlannedEnd(walk.Query(), facts(STATUS_GO_GRIND, over - 1))) &&
                  word(PlannedEnd(walk.Query(), facts(STATUS_GO_GRIND, over)), "order_overrun"),
              "its overrun comes the interlude's length later");

        // 9 of its 10 minutes at the spot, then a bag trip and the inn in a row (20 minutes), resumed at the spot
        Slot spot;
        spot.Set(grind(START));
        spot.Set(interlude(GoalKind::BuyBag, START + 9 * MINUTE));
        spot.Set(interlude(GoalKind::Hearth, START + 15 * MINUTE));
        std::uint32_t const again = START + 29 * MINUTE;
        Check(spot.End(again) && goes(PlannedEnd(spot.Query(), facts(STATUS_WANDER_RANDOM, again))) &&
                  goes(PlannedEnd(spot.Query(), facts(STATUS_WANDER_RANDOM, again + MINUTE - 1))) &&
                  word(PlannedEnd(spot.Query(), facts(STATUS_WANDER_RANDOM, again + MINUTE)), "order_time"),
              "an order resumed after 9 of its 10 minutes ends order_time after its remaining minute; two interludes "
              "in a row count from the first one's start");

        // no interlude: the clock as set; a new goal after an interlude starts its own clock
        Slot plain;
        plain.Set(grind(START));
        Check(plain.Query().startedMs == START &&
                  word(PlannedEnd(plain.Query(), facts(STATUS_WANDER_RANDOM, START + 10 * MINUTE)), "order_time"),
              "a goal no interlude cut keeps its clock");
        plain.Set(interlude(GoalKind::Errand, START + MINUTE));
        plain.Set(grind(START + 2 * MINUTE));
        plain.Set(interlude(GoalKind::Rest, START + 3 * MINUTE));
        Check(plain.End(START + 5 * MINUTE) && plain.Query().startedMs == START + 4 * MINUTE,
              "a new goal after an interlude: only the interlude that cut it moves its clock");
        Check(!walk.Bounded() && !spot.Bounded() && !plain.Bounded(),
              "interludes within the bound (20 minutes in all, the errand deadline) hit no bound");

        // WT63e-d, the general bound: an order's interludes move its clock at most INTERLUDE_SHIFT_MAX_MS in all
        Slot past;
        past.Set(grind(START));
        past.Set(interlude(GoalKind::Errand, START + 3 * MINUTE));
        std::uint32_t const late = START + 3 * MINUTE + INTERLUDE_SHIFT_MAX_MS + 5 * MINUTE;
        Check(past.End(late) && past.Bounded() && past.Query().startedMs == START + INTERLUDE_SHIFT_MAX_MS &&
                  goes(PlannedEnd(past.Query(), facts(STATUS_GO_GRIND, late))),
              "an interlude 5 minutes past the bound: those 5 minutes count against the order (own 8 of 10 minutes)");
        Slot many;
        many.Set(grind(START));
        std::uint32_t at = START + MINUTE;
        bool hit = false;
        for (int i = 0; i < 1000; ++i)
        {
            many.Set(interlude(i % 2 ? GoalKind::Rest : GoalKind::Errand, at));
            at += 20 * MINUTE;
            bool const resumed = many.End(at);
            hit = hit || (resumed && many.Bounded());
        }
        Check(hit && many.Query().kind == GoalKind::Grind && many.MinutesSince(at) >= ORDER_OVERRUN_FACTOR * 10.0f &&
                  word(PlannedEnd(many.Query(), facts(STATUS_WANDER_RANDOM, at)), "order_time") &&
                  word(PlannedEnd(many.Query(), facts(STATUS_GO_GRIND, at)), "order_overrun"),
              "1000 interludes of 20 minutes with no own time between them: the order ends, order_time at the spot "
              "and order_overrun on the walk (mutant: no bound, 1.00 own minute, never ends)");
        many.Set(grind(at));
        many.Set(interlude(GoalKind::Errand, at + MINUTE));
        Check(many.End(at + 21 * MINUTE) && !many.Bounded() && many.Query().startedMs == at + 20 * MINUTE,
              "a new goal gets the whole bound again");

        // the interlude does not stall the goal it interrupts: the resumed walk runs on
        BotGoals goals;
        goals.slot.Set(grind(START));
        BeginRun(goals, 40, 5, 100, true);
        NoteStatusEnd(goals, Outcome::Interrupted, false);  // the planner's switch ends the walk for the errand
        goals.slot.Set(interlude(GoalKind::Errand, START + 3 * MINUTE));
        NoteStatusEnd(goals, Outcome::Arrived, true);  // the errand trip's own end
        goals.slot.End(back);
        ContinueRun(goals);
        PlannedAnswer const first = PlannedEnd(goals.slot.Query(), facts(STATUS_GO_GRIND, back));
        if (first.ends)  // as the status core would: a judged end, then the run's end
        {
            NoteStatusEnd(goals, first.outcome, true, first.word);
            EndRun(goals, 5, 100);
        }
        Check(!first.ends && goals.run.running && !goals.stalled && !goals.stallRow,
              "the interrupted walk resumes and runs on: no judged end at once, no stall without XP (mutant: "
              "order_overrun at the resume stalls the key)");
    }

    // WT63e: the hearth's ends (WT63d-b) and the retry stamps per interlude kind (3.19 (6)); a bag trip without its bag
    {
        using Goals::GoalKind;
        using StatusEnd::Outcome;
        constexpr std::uint32_t NOW = 5000;
        WorldTravel::InnEnd const bound = WorldTravel::InnEndOf(true, WorldTravel::InnStep::Bind);
        WorldTravel::InnEnd const unbound = WorldTravel::InnEndOf(false, WorldTravel::InnStep::Bind);
        WorldTravel::InnEnd const stuck = WorldTravel::InnEndOf(false, WorldTravel::InnStep::Fail);
        Check(bound.outcome == Outcome::Arrived && !bound.avoid && unbound.outcome == Outcome::Abandoned &&
                  unbound.avoid && stuck.outcome == Outcome::Stuck && !stuck.avoid,
              "the hearth's ends: bound arrived, unbound abandoned (the inn avoided), stuck stuck (not avoided)");
        auto const stamps = [](GoalKind kind, Outcome outcome, bool bagBought)
        {
            BotGoals goals;
            goals.bagBought = bagBought;
            NoteInterludeEnd(goals, kind, outcome, NOW);
            return RetryWaits(goals, kind, NOW + 1);
        };
        auto const clears = [](GoalKind kind, Outcome outcome, bool bagBought)
        {
            BotGoals goals;
            goals.failedMs[static_cast<std::size_t>(kind)] = NOW;
            goals.bagBought = bagBought;
            NoteInterludeEnd(goals, kind, outcome, NOW + 1);
            return !RetryWaits(goals, kind, NOW + 2);
        };
        bool table = true;
        for (GoalKind kind : {GoalKind::Errand, GoalKind::BuyBag, GoalKind::Hearth})
        {
            for (Outcome failure : {Outcome::TimedOut, Outcome::Stuck, Outcome::Abandoned})
                table = table && stamps(kind, failure, true);
            table = table && clears(kind, Outcome::Arrived, true) && clears(kind, Outcome::Finished, true);
            for (Outcome outside : {Outcome::Interrupted, Outcome::Ordered, Outcome::Sick})
                table = table && !stamps(kind, outside, false) && !clears(kind, outside, true);
        }
        Check(table,
              "errand, buy_bag and hearth stamp on timed out, stuck or abandoned, clear on arrived or "
              "finished; an end from outside changes nothing");
        Check(stamps(GoalKind::BuyBag, Outcome::Finished, false) && !stamps(GoalKind::Errand, Outcome::Finished, false),
              "a bag trip that finished without its bag stamps the buy_bag retry (mutant: any finish clears)");
        // NE2 review: an errand that finished with its repair still due (a repairer dead or gone, a stand point off
        // the mesh) waits out the retry time, so the due repair, which ranks first, does not start the same trip again
        // at once; with the repair done it clears; an end from outside and a bag trip read nothing of it
        auto const repairEnd = [](GoalKind kind, Outcome outcome, bool repairDue, std::uint32_t at)
        {
            BotGoals goals;
            goals.bagBought = true;
            NoteInterludeEnd(goals, kind, outcome, NOW, repairDue);
            return RetryWaits(goals, kind, at);
        };
        constexpr std::uint32_t RETRY = WorldTravel::ERRAND_RETRY_MS;
        BotGoals repaired;
        repaired.failedMs[static_cast<std::size_t>(GoalKind::Errand)] = NOW;
        NoteInterludeEnd(repaired, GoalKind::Errand, Outcome::Finished, NOW + 1, false);
        Check(repairEnd(GoalKind::Errand, Outcome::Finished, true, NOW + RETRY - 1) &&
                  !repairEnd(GoalKind::Errand, Outcome::Finished, true, NOW + RETRY) &&
                  !RetryWaits(repaired, GoalKind::Errand, NOW + 2),
              "an errand that finished with its repair still due waits out the retry time, one that repaired clears "
              "it (mutant: any finished errand clears, the due repair restarts the trip at once)");
        Check(!repairEnd(GoalKind::Errand, Outcome::Interrupted, true, NOW + 1) &&
                  !repairEnd(GoalKind::BuyBag, Outcome::Finished, true, NOW + 1),
              "a due repair stamps only a finished errand: an end from outside and a bag trip with its bag stamp "
              "nothing");
        // NR14: an errand to a stop of its own (riding, training, the tame) that finished without what it went for
        // waits out the retry time as the due repair does: the due stop ranks first and would start the trip again
        auto const ownEnd = [](char const* reason, bool done, Outcome outcome)
        {
            BotGoals goals;
            Active errand = GoalOf(GoalKind::Errand, 4732, Source::Planner);
            errand.reason = reason;
            goals.slot.Set(std::move(errand));
            goals.ownStopDone = done;
            NoteInterludeEnd(goals, GoalKind::Errand, outcome, NOW);
            return RetryWaits(goals, GoalKind::Errand, NOW + 1);
        };
        bool ownMissed = true;
        for (char const* reason : {Goals::TRAINING_REASON, Goals::RIDING_REASON, Goals::PET_REASON})
            ownMissed = ownMissed && ownEnd(reason, false, Outcome::Finished) &&
                        !ownEnd(reason, true, Outcome::Finished) && !ownEnd(reason, false, Outcome::Interrupted);
        Check(ownMissed && !ownEnd("bags_full", false, Outcome::Finished),
              "an errand to a stop of its own that finished without its result waits out the retry time, one that got "
              "it clears; an end from outside and a sale errand read nothing of it (mutant: any finish clears, the "
              "due stop restarts the trip at once)");
        // NR14c: the missed goal's own wait doubles per miss in a row (5, 10, 20, then 40 min): a due stop that keeps
        // failing, or whose start keeps refusing, is tried less and less often; its success, a miss of another goal
        // or a new level starts the count anew, and another goal of the kind waits only the kind's retry time
        {
            constexpr std::uint8_t LEVEL = 20;
            Active riding = GoalOf(GoalKind::Errand, 4732, Source::Planner);
            riding.reason = Goals::RIDING_REASON;
            Active training = GoalOf(GoalKind::Errand, 911, Source::Planner);
            training.reason = Goals::TRAINING_REASON;
            BotGoals goals;
            goals.slot.Set(Active{riding});
            auto const missAt = [&goals](std::uint32_t at)
            {
                goals.ownStopDone = false;
                NoteInterludeEnd(goals, GoalKind::Errand, Outcome::Finished, at, false, LEVEL);
            };
            // waits exactly `times` retry times from `at`
            auto const waitsFor = [&goals](Active const& goal, std::uint32_t at, std::uint32_t times)
            {
                return MissedWaits(goals, goal, LEVEL, at + times * RETRY - 1) &&
                       !MissedWaits(goals, goal, LEVEL, at + times * RETRY);
            };
            std::uint32_t at = NOW;
            bool grows = true;
            for (std::uint32_t const times : {1u, 2u, 4u, 8u, 8u})
            {
                missAt(at);
                grows = grows && waitsFor(riding, at, times);
                at += times * RETRY;
            }
            Check(grows && !MissedWaits(goals, training, LEVEL, NOW + 1) &&
                      !RetryWaits(goals, GoalKind::Errand, at - RETRY),
                  "a goal missed in a row waits 5, 10, 20, 40 and again 40 minutes; another goal of the kind waits "
                  "only the kind's 5 minutes (mutant: the wait stays 5 minutes, the due stop loops for ever)");
            Check(!MissedWaits(goals, riding, LEVEL + 1, at - 1),
                  "a new level starts the count anew: its due needs are new (mutant: the level is not compared)");
            missAt(at);
            goals.ownStopDone = true;
            NoteInterludeEnd(goals, GoalKind::Errand, Outcome::Finished, at + 1, false, LEVEL);
            bool const cleared = !MissedWaits(goals, riding, LEVEL, at + 2);
            missAt(at + 2);
            bool const anew = waitsFor(riding, at + 2, 1);
            goals.slot.Set(Active{training});
            missAt(at + 3);
            goals.slot.Set(Active{riding});
            missAt(at + 4);
            Check(cleared && anew && waitsFor(riding, at + 4, 1),
                  "the goal's success ends its count, and so does a miss of another goal (mutant: the count never "
                  "resets)");
            BotGoals refused;
            NoteRefused(refused, riding, LEVEL, NOW);
            bool const once = MissedWaits(refused, riding, LEVEL, NOW + RETRY - 1) &&
                              !MissedWaits(refused, riding, LEVEL, NOW + RETRY);
            NoteRefused(refused, riding, LEVEL, NOW + RETRY);
            bool const twice = MissedWaits(refused, riding, LEVEL, NOW + 3 * RETRY - 1) &&
                               !MissedWaits(refused, riding, LEVEL, NOW + 3 * RETRY);
            BotGoals rested;
            NoteRefused(rested, GoalOf(GoalKind::Rest, 0, Source::Planner), LEVEL, NOW);
            Check(once && twice && !RetryWaits(refused, GoalKind::Errand, NOW + 1) &&
                      !MissedWaits(rested, GoalOf(GoalKind::Rest, 0, Source::Planner), LEVEL, NOW + 1),
                  "a refused start waits as a miss and grows the same way; it holds back only that goal, a rest's "
                  "refusal nothing (mutant: refusals stamp nothing, every plan runs the errand start again)");
        }
        bool rest = true;
        for (Outcome outcome : {Outcome::TimedOut, Outcome::Stuck, Outcome::Abandoned, Outcome::Finished})
            rest = rest && !stamps(GoalKind::Rest, outcome, false);
        Check(rest, "a rest stamps nothing: its order_overrun is no failure to retry");
    }

    // WT63e: the run keeps its last judged outcome and note; a stall's row carries them (3.19 (6))
    {
        using Goals::GoalKind;
        using StatusEnd::Outcome;
        constexpr std::uint64_t KEY = 9;
        constexpr std::uint64_t LOG = 7;
        auto const start = [](BotGoals& goals, GoalKind kind)
        {
            goals.slot.Set(GoalOf(kind, 1, Source::Planner));
            return StartActive(goals, kind, Executor::Grind, {KEY, LOG, 100}, [] { return true; });
        };
        BotGoals judged;
        start(judged, GoalKind::Grind);
        NoteStatusEnd(judged, Outcome::Arrived, false);
        NoteStatusEnd(judged, Outcome::Finished, true, "order_time");
        EndRun(judged, LOG, 100);
        Check(judged.stallRow && judged.stallRow->kind == GoalKind::Grind && judged.stallRow->judged &&
                  judged.stallRow->outcome == Outcome::Finished && std::string(judged.stallRow->note) == "order_time",
              "a judged end with the log as at the start stalls, and the stall's row carries the kind, the last "
              "outcome and its note");
        BotGoals defaulted;
        start(defaulted, GoalKind::Grind);
        NoteStatusEnd(defaulted, Outcome::Stuck, true);
        EndRun(defaulted, LOG, 100);
        Check(defaulted.stallRow && std::string(defaulted.stallRow->note) == "stuck",
              "an end without a note carries the outcome's word");
        bool outside = true;
        for (Outcome from : {Outcome::Interrupted, Outcome::Ordered, Outcome::Sick})
        {
            BotGoals goals;
            start(goals, GoalKind::Grind);
            NoteStatusEnd(goals, Outcome::TimedOut, false, "timeout");
            NoteStatusEnd(goals, from, true);
            outside = outside && goals.run.judged && goals.run.lastOutcome == Outcome::TimedOut;
            EndRun(goals, LOG, 100);
            outside = outside && !goals.stallRow && !goals.stalled;
        }
        Check(outside,
              "an end from outside (interrupted, ordered, sick) stalls nothing and keeps the earlier judged "
              "outcome");
        BotGoals interlude;
        start(interlude, GoalKind::Grind);
        NoteStatusEnd(interlude, Outcome::Arrived, false, "arrived");
        interlude.slot.Set(GoalOf(GoalKind::Errand, 2, Source::Planner));
        NoteStatusEnd(interlude, Outcome::Finished, true, "finished");
        Check(interlude.run.lastOutcome == Outcome::Arrived && std::string(interlude.run.lastNote) == "arrived",
              "an interlude's end is not the interrupted goal's outcome");
        BotGoals progressed;
        start(progressed, GoalKind::Grind);
        NoteStatusEnd(progressed, Outcome::Finished, true, "order_time");
        EndRun(progressed, LOG, 150);
        BotGoals unjudged;
        start(unjudged, GoalKind::Quest);
        EndRun(unjudged, LOG);
        Check(!progressed.stallRow && unjudged.stallRow && !unjudged.stallRow->judged,
              "XP gained writes no stall; a run that ended with no judged end stalls with no outcome");
    }

    // AW38b (awareness, Batch K3): a run whose status ended with Danger (a risen bot gave up its goal and left) blocks
    // its goal for the block time, whatever it gained and whatever ended its later statuses; the mark ends with the run
    {
        using Goals::GoalKind;
        using StatusEnd::Outcome;
        constexpr std::uint64_t KEY = 9;
        constexpr std::uint64_t NEXT = 10;
        constexpr std::uint64_t LOG = 7;
        auto const start = [](BotGoals& goals, GoalKind kind, std::uint64_t key, std::uint64_t gain = 0)
        {
            goals.slot.Set(GoalOf(kind, static_cast<std::uint32_t>(key), Source::Planner));
            StartFacts facts{key, LOG, 100};
            facts.blockMs = BLOCK;
            facts.gain = gain;
            return StartActive(goals, kind, Executor::Grind, facts, [] { return true; });
        };
        // The leaving as live (Jaidan, lab10): the work ends with danger, the walk out's start interrupts the next
        // status, the walk arrives; on the way the bot gained XP
        auto const left = [](BotGoals& goals)
        {
            NoteStatusEnd(goals, Outcome::Danger, false, "danger");
            NoteStatusEnd(goals, Outcome::Interrupted, false);
            NoteStatusEnd(goals, Outcome::Arrived, true);
        };
        BotGoals grind;
        start(grind, GoalKind::Grind, KEY);
        left(grind);
        EndRun(grind, LOG, 150);
        bool const grindBlocked = !MayStart(grind, KEY, LOG) && grind.stallRow && grind.stallRow->judged &&
                                  grind.stallRow->outcome == Outcome::Danger &&
                                  std::string(grind.stallRow->note) == "danger";
        BotGoals gather;
        start(gather, GoalKind::Gather, KEY, 20);
        left(gather);
        GoalApply::EndRun(gather, LOG, 150, 0, BLOCK, 25);
        Check(grindBlocked && !MayStart(gather, KEY, LOG),
              "a run ended by danger blocks its goal despite XP and loot gained and a later interrupted and arrived "
              "end; its row carries danger and its note (mutants: outside or progress read first, the later end "
              "overwrites the outcome)");
        BotGoals later;
        start(later, GoalKind::Grind, KEY);
        NoteStatusEnd(later, Outcome::Danger, false, "abort:danger");
        NoteStatusEnd(later, Outcome::TimedOut, true, "timeout");
        EndRun(later, LOG, 150);
        BotGoals resumed;
        start(resumed, GoalKind::Grind, KEY);
        NoteStatusEnd(resumed, Outcome::Danger, false);
        ContinueRun(resumed);
        NoteStatusEnd(resumed, Outcome::Finished, true);
        EndRun(resumed, LOG, 150);
        BotGoals relogged;
        start(relogged, GoalKind::Grind, KEY);
        NoteStatusEnd(relogged, Outcome::Danger, true);
        EndRun(relogged, LOG + 1, 150);
        Check(!MayStart(later, KEY, LOG) && later.stallRow && std::string(later.stallRow->note) == "abort:danger" &&
                  !MayStart(resumed, KEY, LOG) && !MayStart(relogged, KEY, LOG + 1),
              "the danger mark holds through a later judged end and a resumed executor (ContinueRun), and a "
              "changed log still blocks the goal at the new log (mutant: ContinueRun clears it)");
        BotGoals plain;
        start(plain, GoalKind::Grind, KEY);
        NoteStatusEnd(plain, Outcome::Interrupted, false);
        NoteStatusEnd(plain, Outcome::Arrived, true);
        EndRun(plain, LOG, 150);
        BotGoals interlude;
        start(interlude, GoalKind::Grind, KEY);
        interlude.slot.Set(GoalOf(GoalKind::Errand, 2, Source::Planner));
        NoteStatusEnd(interlude, Outcome::Danger, true);
        Check(!plain.stalled && !plain.stallRow && !interlude.run.danger,
              "without a danger end of its own a run ends as before: XP gained, no stall; an interlude's danger "
              "end marks nothing");
        BotGoals next;
        start(next, GoalKind::Grind, KEY);
        left(next);
        start(next, GoalKind::Grind, NEXT);
        bool const fresh = next.run.running && next.run.key == NEXT && !next.run.danger && !MayStart(next, KEY, LOG);
        next.stallRow.reset();
        NoteStatusEnd(next, Outcome::Arrived, true);
        EndRun(next, LOG, 150);
        bool const nextFree = MayStart(next, NEXT, LOG) && !next.stallRow;
        BotGoals route;
        std::uint64_t const routeKey = (static_cast<std::uint64_t>(Executor::Route) << 56) | 1;
        start(route, GoalKind::Quest, routeKey);
        NoteStatusEnd(route, Outcome::Danger, true);
        EndRun(route, LOG);
        Check(fresh && nextFree && MayStart(route, routeKey, LOG),
              "the mark does not carry into the next run (blocked goal stays blocked, the next one gains and is "
              "free); a route's run never stalls, its order fails instead (FailsOrder)");
    }

    // WT63e: the planner's bag order buys once (WT63c review: an order bought at every stop) and alone
    {
        BotGoals goals;
        Active bag = GoalOf(Goals::GoalKind::BuyBag, 4498, Source::Planner);
        goals.slot.Set(bag);
        bool const ordered = OrderedBagOf(goals) == 4498;
        goals.bagBought = true;
        Check(ordered && OrderedBagOf(goals) == 0,
              "after its purchase the order names no bag (mutant: every stop "
              "buys)");
        BotGoals bridge;
        bridge.slot.Set(GoalOf(Goals::GoalKind::BuyBag, 4498, Source::Bridge));
        BotGoals grind;
        grind.slot.Set(GoalOf(Goals::GoalKind::Grind, 4498, Source::Planner));
        Check(OrderedBagOf(bridge) == 0 && OrderedBagOf(grind) == 0, "only the planner's bag purchase orders a bag");
    }

    std::printf("goal apply selftest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
