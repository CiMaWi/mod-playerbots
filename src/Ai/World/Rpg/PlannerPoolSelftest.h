/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERPOOLSELFTEST_H
#define _PLAYERBOT_PLANNERPOOLSELFTEST_H

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "GoalModel.h"
#include "PlannerPool.h"
#include "PlannerRouteCost.h"
#include "TravelGraph.h"

// The planner pool's checks (PlannerPool.h), shared by tests/planner-pool-selftest.cpp and
// `playerbots planner selftest` (PlannerService.cpp); only they include this header.
namespace Planner
{
// The test workload: a few microseconds that depend only on the snapshot (FNV-1a over its main fields), carried in a
// plan's hub field so a result says whose snapshot it came from
inline std::optional<Payload> PoolTestCompute(Snapshot const& snapshot)
{
    constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;
    std::uint64_t hash = FNV_OFFSET;
    auto const mix = [&hash](std::uint64_t value) { hash = (hash ^ value) * FNV_PRIME; };
    mix(snapshot.bot);
    mix(snapshot.stateVersion);
    mix(snapshot.level);
    mix(snapshot.zone);
    mix(snapshot.money);
    for (QuestSlot const& slot : snapshot.quests)
        mix(slot.quest);
    for (std::uint32_t const quest : snapshot.rewardedQuests)
        mix(quest);
    for (std::uint32_t const word : snapshot.taxiMask)
        mix(word);
    auto plan = std::make_shared<Goals::GoalPlan>();
    plan->hub = static_cast<std::uint32_t>(hash ^ (hash >> 32));
    return Payload{GoalsAnswer{std::move(plan), nullptr, nullptr}};
}

// The pool's checks (tests/planner-pool-selftest.cpp, `playerbots planner selftest`): queue semantics with a gated
// worker, expiry, failures, Stop, every Recheck verdict and its limits, the counter sums, then 4 submitter threads x
// botsPerThread bots on 2 workers (every result equals the single-thread computation, none lost, none for the wrong
// bot, an older job never overwrites a newer result), Post tasks run once, IsWorkerThread, the snapshot sizes.
// Returns the number of checks
inline std::uint32_t PoolSelftest(std::vector<std::string>& failures, std::vector<std::string>& notes,
                                  std::uint32_t botsPerThread)
{
    using namespace std::chrono_literals;
    static constexpr auto DEADLINE = std::chrono::seconds(30);
    constexpr std::uint32_t SUBMITTERS = 4;
    constexpr std::uint32_t WORKERS = 2;
    constexpr std::uint32_t RACE_ROUNDS = 49;
    constexpr std::uint32_t TASKS = 100;
    constexpr std::uint32_t SHORT_KEEP_MS = 50;
    constexpr std::size_t SMALL_QUEUE = 4;
    constexpr std::size_t WORST_BYTES = 16 * 1024;
    constexpr std::size_t TYPICAL_BYTES = 8 * 1024;

    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back(what);
        return ok;
    };
    auto const sums = [&check](PoolStats const& s, std::string const& where)
    {
        check(s.submitted == s.run + s.replaced + s.dropped + s.waiting,
              where + ": submitted = run + replaced + dropped + waiting");
        check(s.run == s.made + s.failures, where + ": run = made + failures");
        check(s.made == s.taken + s.expired + s.superseded + s.results,
              where + ": made = taken + expired + superseded + results");
    };
    auto const makeSnapshot = [](std::uint64_t bot, std::uint32_t version)
    {
        Snapshot snapshot;
        snapshot.bot = bot;
        snapshot.stateVersion = version;
        snapshot.takenMs = static_cast<std::uint32_t>(bot * 7);
        snapshot.level = static_cast<std::uint8_t>(1 + bot % 80);
        snapshot.zone = static_cast<std::uint32_t>(bot % 4000);
        snapshot.position = {static_cast<std::uint32_t>(bot % 2), static_cast<float>(bot % 1000), 1.0f, 2.0f};
        for (std::uint32_t quest = 0; quest < bot % 50; ++quest)
            snapshot.rewardedQuests.push_back(quest * 3 + static_cast<std::uint32_t>(bot % 7));
        return snapshot;
    };
    auto const job = [&makeSnapshot](std::uint64_t bot, std::uint32_t version, Compute compute = PoolTestCompute)
    { return Job{JobKind::Goals, makeSnapshot(bot, version), std::move(compute)}; };
    auto const checksum = [](Result const& result) -> std::uint64_t
    {
        GoalsAnswer const* answer = std::get_if<GoalsAnswer>(&result.payload);
        return answer && answer->plan ? answer->plan->hub : 0;
    };
    auto const expected = [&makeSnapshot, &checksum](std::uint64_t bot, std::uint32_t version) -> std::uint64_t
    { return checksum(Result{0, 0, 0, {}, JobKind::Goals, *PoolTestCompute(makeSnapshot(bot, version))}); };
    auto const waitFor = [](auto const& condition)
    {
        auto const until = std::chrono::steady_clock::now() + DEADLINE;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() > until)
                return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    };

    // a) Never started, stopped: nothing is accepted; Stop twice; Start after Stop does nothing
    {
        PlannerPool pool;
        check(!pool.Submit(1, job(1, 1)), "a never-started pool refuses Submit");
        check(!pool.Post([] {}), "a never-started pool refuses Post");
        check(!pool.TryTake(1), "a never-started pool has no result");
        pool.Stop();
        pool.Stop();
        pool.Start(WORKERS);
        check(pool.Stats().threads == 0, "Start after Stop starts no worker");
        check(!pool.Submit(1, job(1, 1)), "a stopped pool refuses Submit");
        check(!PlannerPool::IsWorkerThread(), "IsWorkerThread is false on the test thread");
    }

    // b) One gated worker: replace in place, the cap drops the oldest waiting job, the fast path, the sums
    {
        PlannerPool pool(SMALL_QUEUE);
        pool.Start(1);
        std::atomic<bool> entered{false};
        std::atomic<bool> open{false};
        Compute const gated = [&entered, &open](Snapshot const& snapshot) -> std::optional<Payload>
        {
            entered = true;
            while (!open)
                std::this_thread::yield();
            return PoolTestCompute(snapshot);
        };
        check(pool.Submit(1, job(1, 1, gated)), "Submit to a running pool accepts the job");
        check(waitFor([&entered] { return entered.load(); }), "the gated job starts");
        check(!pool.TryTake(99), "no result: the fast path");
        pool.Submit(2, job(2, 1));
        check(pool.Submit(2, job(2, 2)), "the replacing job is accepted");
        for (std::uint64_t bot = 3; bot <= 6; ++bot)
            pool.Submit(bot, job(bot, 1));
        PoolStats const queued = pool.Stats();
        check(queued.replaced == 1, "a newer job replaces the bot's waiting one");
        check(queued.dropped == 1 && queued.waiting == SMALL_QUEUE, "the cap drops the oldest waiting job");
        check(queued.peakWaiting == SMALL_QUEUE, "the peak queue depth");
        open = true;
        std::vector<std::uint64_t> const kept = {1, 3, 4, 5, 6};
        std::size_t got = 0;
        check(waitFor(
                  [&]
                  {
                      for (std::uint64_t const bot : kept)
                          if (std::optional<Result> const result = pool.TryTake(bot))
                          {
                              ++got;
                              check(result->bot == bot && checksum(*result) == expected(bot, 1),
                                    "a gated-queue result is the bot's own");
                          }
                      return got == kept.size();
                  }),
              "every kept job's result arrives");
        check(!pool.TryTake(2), "the dropped bot gets no result");
        sums(pool.Stats(), "gated queue");
    }

    // b2) Submit drops what the newer job makes stale: the finished result nobody took, and the late result of a job
    // still running; TryTake answers only the newest job
    {
        PlannerPool pool;
        pool.Start(1);
        pool.Submit(1, job(1, 1));
        check(waitFor([&pool] { return pool.Stats().made == 1; }), "b2: the first job finishes");
        pool.Submit(1, job(1, 2));
        std::optional<Result> second;
        check(waitFor([&] { return (second = pool.TryTake(1)).has_value(); }), "b2: the second job's result arrives");
        check(second && second->stateVersion == 2 && checksum(*second) == expected(1, 2),
              "b2: only the second job's result is taken after a new Submit");
        check(!pool.TryTake(1), "b2: the first job's result is gone, not served later");
        PoolStats const finished = pool.Stats();
        check(finished.superseded == 1 && finished.taken == 1, "b2: the dropped result counts as superseded");
        sums(finished, "submit drops a finished result");
    }
    {
        PlannerPool pool;
        pool.Start(1);
        std::atomic<bool> entered{false};
        std::atomic<bool> open{false};
        pool.Submit(1, job(1, 1,
                           [&entered, &open](Snapshot const& snapshot) -> std::optional<Payload>
                           {
                               entered = true;
                               while (!open)
                                   std::this_thread::yield();
                               return PoolTestCompute(snapshot);
                           }));
        check(waitFor([&entered] { return entered.load(); }), "b2: the gated first job starts");
        pool.Submit(1, job(1, 2));
        open = true;
        std::optional<Result> late;
        check(waitFor([&] { return (late = pool.TryTake(1)).has_value(); }), "b2: a result arrives after the gate");
        check(late && late->stateVersion == 2, "b2: a running job's late result is not the one taken");
        check(!pool.TryTake(1), "b2: no second result is kept for the bot");
        sums(pool.Stats(), "submit while a job runs");
    }

    // c) Expiry, failures and Stop dropping waiting jobs
    {
        PlannerPool pool(QUEUE_MAX, SHORT_KEEP_MS);
        pool.Start(1);
        pool.Submit(7, job(7, 1));
        pool.Submit(8, job(8, 1, [](Snapshot const&) -> std::optional<Payload> { return std::nullopt; }));
        pool.Submit(9, job(9, 1, [](Snapshot const&) -> std::optional<Payload> { throw std::exception(); }));
        check(waitFor([&pool] { return pool.Stats().run == 3; }), "three jobs run");
        std::this_thread::sleep_for(std::chrono::milliseconds(SHORT_KEEP_MS * 3));
        check(!pool.TryTake(7), "a result older than the keep time is not taken");
        PoolStats const stats = pool.Stats();
        check(stats.expired == 1, "the old result counts as expired");
        check(pool.ForgetExpired(7) && !pool.ForgetExpired(7), "the expiry is told to its bot once");
        check(!pool.ForgetExpired(8), "a failed job's bot learns of no expired result");
        check(stats.failures == 2, "a job returning none and one throwing count as failures");
        sums(stats, "expiry and failures");
    }
    {
        PlannerPool pool;
        pool.Start(1);
        std::atomic<bool> entered{false};
        std::atomic<bool> open{false};
        pool.Submit(1, job(1, 1,
                           [&entered, &open](Snapshot const& snapshot) -> std::optional<Payload>
                           {
                               entered = true;
                               while (!open)
                                   std::this_thread::yield();
                               return PoolTestCompute(snapshot);
                           }));
        waitFor([&entered] { return entered.load(); });
        for (std::uint64_t bot = 2; bot <= 4; ++bot)
            pool.Submit(bot, job(bot, 1));
        pool.Post([] {});
        std::thread opener(
            [&open]
            {
                std::this_thread::sleep_for(50ms);
                open = true;
            });
        pool.Stop();
        opener.join();
        PoolStats const stats = pool.Stats();
        check(stats.dropped == 3 && stats.tasksDropped == 1, "Stop drops the waiting jobs and tasks");
        check(!pool.Submit(5, job(5, 1)), "after Stop the pool accepts nothing");
        pool.Stop();
        sums(pool.Stats(), "stop");
    }

    // d) Recheck: every verdict and the limits
    {
        Result result;
        result.stateVersion = 3;
        result.takenMs = 1000;
        result.position = {0, 0.0f, 0.0f, 0.0f};
        Limits const limits{STALE_MS, RECHECK_YARDS};
        CurrentState now{true, 3, {0, 0.0f, 0.0f, 0.0f}, 1000};
        check(Recheck(result, now, limits) == Verdict::Use, "recheck: unchanged is used");
        CurrentState dead = now;
        dead.alive = false;
        check(Recheck(result, dead, limits) == Verdict::Dead, "recheck: dead");
        CurrentState version = now;
        version.stateVersion = 4;
        check(Recheck(result, version, limits) == Verdict::Version, "recheck: state version changed");
        CurrentState edge = now;
        edge.position = {0, 60.0f, 80.0f, 0.0f};  // exactly RECHECK_YARDS
        check(Recheck(result, edge, limits) == Verdict::Use, "recheck: exactly at the yards limit is used");
        edge.position.y = 80.5f;
        check(Recheck(result, edge, limits) == Verdict::Position, "recheck: beyond the yards limit");
        CurrentState otherMap = now;
        otherMap.position.map = 1;
        check(Recheck(result, otherMap, limits) == Verdict::Position, "recheck: another map");
        CurrentState age = now;
        age.nowMs = 1000 + STALE_MS;
        check(Recheck(result, age, limits) == Verdict::Use, "recheck: exactly at the age limit is used");
        age.nowMs = 1001 + STALE_MS;
        check(Recheck(result, age, limits) == Verdict::Age, "recheck: older than the age limit");
        Result wrapped = result;
        wrapped.takenMs = 0xFFFFFF00u;
        CurrentState afterWrap = now;
        afterWrap.nowMs = 0x100u;
        check(Recheck(wrapped, afterWrap, limits) == Verdict::Use, "recheck: across the clock's wrap");
    }

    // e) Threads: SUBMITTERS threads x botsPerThread bots on WORKERS workers, then the older/newer race, Post tasks
    {
        PlannerPool pool;
        pool.Start(WORKERS);
        std::atomic<std::uint32_t> outsideWorker{0};
        Compute const noted = [&outsideWorker](Snapshot const& snapshot)
        {
            if (!PlannerPool::IsWorkerThread())
                ++outsideWorker;
            return PoolTestCompute(snapshot);
        };
        std::vector<std::vector<std::string>> threadFailures(SUBMITTERS);
        std::vector<std::thread> submitters;
        for (std::uint32_t t = 0; t < SUBMITTERS; ++t)
            submitters.emplace_back(
                [&, t]
                {
                    std::vector<std::string>& mine = threadFailures[t];
                    std::uint64_t const base = (t + 1) * 1000000ULL;
                    for (std::uint64_t i = 0; i < botsPerThread; ++i)
                        if (!pool.Submit(base + i, job(base + i, 1, noted)))
                            mine.push_back("a running pool refused a Submit");
                    std::vector<bool> got(botsPerThread, false);
                    std::uint32_t left = botsPerThread;
                    auto const until = std::chrono::steady_clock::now() + DEADLINE;
                    while (left && std::chrono::steady_clock::now() < until)
                    {
                        for (std::uint64_t i = 0; i < botsPerThread; ++i)
                        {
                            if (got[i])
                                continue;
                            std::optional<Result> const result = pool.TryTake(base + i);
                            if (!result)
                                continue;
                            got[i] = true;
                            --left;
                            if (result->bot != base + i || checksum(*result) != expected(base + i, 1))
                                mine.push_back("a threaded result differs from the single-thread one");
                        }
                        std::this_thread::yield();
                    }
                    if (left)
                        mine.push_back(std::to_string(left) + " threaded results lost");
                    // The race: an older job must never overwrite the newer one's result
                    for (std::uint64_t i = 0; i < RACE_ROUNDS; ++i)
                    {
                        std::uint64_t const bot = base + botsPerThread + i;
                        pool.Submit(bot, job(bot, 1, noted));
                        pool.Submit(bot, job(bot, 2, noted));
                        bool newer = false;
                        auto const raceUntil = std::chrono::steady_clock::now() + DEADLINE;
                        while (!newer && std::chrono::steady_clock::now() < raceUntil)
                        {
                            if (std::optional<Result> const result = pool.TryTake(bot))
                                newer = result->stateVersion == 2;
                            else
                                std::this_thread::yield();
                        }
                        if (!newer)
                            mine.push_back("the newer job's result never arrived");
                        else if (pool.TryTake(bot))
                            mine.push_back("an older result arrived after the newer one");
                    }
                });
        std::vector<std::atomic<std::uint32_t>> ran(TASKS);
        for (std::uint32_t i = 0; i < TASKS; ++i)
            pool.Post([&ran, i] { ++ran[i]; });
        for (std::thread& submitter : submitters)
            submitter.join();
        for (std::vector<std::string> const& mine : threadFailures)
            check(mine.empty(), mine.empty() ? "threads" : "threads: " + mine.front());
        check(waitFor([&pool] { return pool.Stats().tasksRun == TASKS; }), "every posted task runs");
        bool once = true;
        for (std::atomic<std::uint32_t> const& count : ran)
            once = once && count.load() == 1;
        check(once, "every posted task runs exactly once");
        check(outsideWorker.load() == 0, "IsWorkerThread is true inside every job");
        PoolStats const stats = pool.Stats();
        sums(stats, "threads");
        check(stats.threads == WORKERS && stats.busyNs > 0 && stats.computeNs > 0, "worker count and busy time");
        notes.push_back("threads: " + std::to_string(stats.run) + " jobs, compute p50/p95 " +
                        std::to_string(stats.computeP50Ns) + "/" + std::to_string(stats.computeP95Ns) +
                        " ns, queue wait p50/p95 " + std::to_string(stats.waitP50Ns) + "/" +
                        std::to_string(stats.waitP95Ns) + " ns, " + std::to_string(stats.superseded) + " superseded");
        pool.Stop();
    }

    // f) Snapshot sizes: worst case (25 quests, 2000 rewarded quests, every skill, the recent goals) and a typical bot
    {
        constexpr std::size_t WORST_REWARDED = 2000;
        constexpr std::size_t TYPICAL_QUESTS = 15;
        constexpr std::size_t TYPICAL_REWARDED = 300;
        constexpr std::size_t ALL_SKILLS = 15;
        constexpr std::size_t TYPICAL_SKILLS = 4;
        // Two crafts, cooking and first aid at their caps, and a typical bot's few; the tools of all of them
        constexpr std::size_t WORST_RECIPES = 900;
        constexpr std::size_t TYPICAL_RECIPES = 60;
        constexpr std::size_t WORST_TOOLS = 8;
        constexpr std::size_t TYPICAL_TOOLS = 2;
        Snapshot worst;
        worst.quests.resize(QUEST_LOG_SLOTS);
        worst.rewardedQuests.resize(WORST_REWARDED);
        worst.skills.resize(ALL_SKILLS);
        worst.knownRecipes.resize(WORST_RECIPES);
        worst.toolsHeld.resize(WORST_TOOLS);
        worst.traits = TraitRules::BotTraits{};
        Snapshot typical;
        typical.quests.resize(TYPICAL_QUESTS);
        typical.rewardedQuests.resize(TYPICAL_REWARDED);
        typical.skills.resize(TYPICAL_SKILLS);
        typical.knownRecipes.resize(TYPICAL_RECIPES);
        typical.toolsHeld.resize(TYPICAL_TOOLS);
        typical.traits = TraitRules::BotTraits{};
        check(worst.ApproxBytes() < WORST_BYTES, "the worst-case snapshot stays under 16 KB");
        check(typical.ApproxBytes() < TYPICAL_BYTES, "a typical snapshot stays under 8 KB");
        notes.push_back("snapshot bytes: worst " + std::to_string(worst.ApproxBytes()) + ", typical " +
                        std::to_string(typical.ApproxBytes()));

        Snapshot known;
        check(!known.mailWaiting && !known.mailSeenCopper && !known.mailExpectedCopper && !known.bankKeptCount &&
                  !known.bankDueCount && !known.bankDueWorth && !known.bankFreeSlots,
              "a new snapshot knows no mail and no bank");
        known.mailWaiting = 3;
        known.bankDueWorth = 4500;
        known.bankFreeSlots = 28;
        Snapshot const copied = known;
        check(copied.mailWaiting == 3 && copied.bankDueWorth == 4500 && copied.bankFreeSlots == 28,
              "a copied snapshot keeps the mail and bank fields");
    }

    // g) The route cost on a worker from the snapshot alone (planning WN1a): a job prices a way over a fixture road
    //    from its snapshot's route view, and the price is the same search run on this thread
    {
        constexpr std::uint32_t ROAD_NODES = 6;
        constexpr float ROAD_STEP = 100.0f;
        constexpr float MILLIS = 1000.0f;
        TravelGraph graph;
        TravelGraphBuilder builder;
        for (std::uint32_t node = 1; node <= ROAD_NODES; ++node)
        {
            builder.AddNode(node, "road", 0, ROAD_STEP * static_cast<float>(node), 0.0f, 0.0f);
            for (std::uint32_t const other : {node - 1, node + 1})
                if (other >= 1 && other <= ROAD_NODES)
                {
                    TravelGraphBuilder::LinkRow row;
                    row.fromId = node;
                    row.toId = other;
                    row.type = 1;
                    row.distance = ROAD_STEP;
                    builder.AddLink(row);
                }
        }
        std::string error;
        bool built = builder.EndLinks(error);
        builder.AddPoint(1, 2, 0, 0, ROAD_STEP, 0.0f, 0.0f);
        built = built && builder.Finish(graph, error);
        graph.Publish(0, 0);
        check(built, "g: the fixture road builds " + error);
        Place const goal{0, ROAD_STEP * ROAD_NODES, 0.0f, 0.0f};
        std::atomic<bool> onWorker{false};
        Compute const priced = [&graph, &goal, &onWorker](Snapshot const& snapshot) -> std::optional<Payload>
        {
            onWorker = PlannerPool::IsWorkerThread();
            TravelCost const travel =
                RouteCost::Hook(std::make_shared<RouteCost>(graph, snapshot.route, RouteCostInputs{}));
            float const seconds = travel ? travel(snapshot.position, goal) : NO_ROUTE_SECONDS;
            auto plan = std::make_shared<Goals::GoalPlan>();
            plan->hub = seconds < 0.0f ? 0 : static_cast<std::uint32_t>(std::lround(seconds * MILLIS));
            return Payload{GoalsAnswer{std::move(plan), nullptr, nullptr}};
        };
        Snapshot snapshot = makeSnapshot(1, 1);
        snapshot.position = {0, 0.0f, 0.0f, 0.0f};
        snapshot.route.position = RoutePoint(snapshot.position);
        TravelRoute::Result const here = TravelRoute::Search(
            graph, TravelRoute::Request(snapshot.route, RoutePoint(goal)), TravelRoute::StuckEdges{}, {});
        PlannerPool pool;
        pool.Start(1);
        pool.Submit(1, Job{JobKind::Goals, snapshot, priced});
        std::optional<Result> answer;
        check(waitFor([&] { return (answer = pool.TryTake(1)).has_value(); }), "g: the priced job's result arrives");
        check(onWorker.load(), "g: the route cost runs on the worker");
        check(here.ok && answer &&
                  checksum(*answer) == static_cast<std::uint64_t>(std::lround(here.totalSeconds * MILLIS)),
              "g: the worker's price from the snapshot is the route's seconds");
        notes.push_back("route cost on a worker: " + std::to_string(answer ? checksum(*answer) : 0) + " ms, " +
                        "the same search here " + std::to_string(here.totalSeconds) + " s");
        pool.Stop();
    }
    return checks;
}
}  // namespace Planner

#endif
