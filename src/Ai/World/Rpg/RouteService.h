/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ROUTESERVICE_H
#define _PLAYERBOT_ROUTESERVICE_H

// Route searches off the map threads. A map thread Submit()s a request (the bot's Snapshot, filled on that thread) and
// polls TryTake() on later ticks; it never waits. The workers run TravelRoute::Search on the immutable graph and the
// stuck edges as last published, so they read no game state. Each bot has a small cache of its recent answers: the
// same question (nearest start and goal nodes, the bot's taxi nodes, purse, hearth and the same stuck edges) within
// CACHE_KEEP_MS is answered without a search. Player-free like RouteRules.h, so tests/route-rules-selftest.cpp runs
// ServiceSelftest() on it; the server's instance, FillSnapshot and the PerfMonitor rows live in RouteService.cpp.
// WT12a's planner pool replaces the workers behind the same Submit/TryTake.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "RouteRules.h"

class Player;

namespace TravelRoute
{
constexpr std::size_t SERVICE_QUEUE_MAX = 4096;      // bots waiting for a search; the oldest is dropped beyond
constexpr std::uint32_t RESULT_KEEP_MS = 30 * 1000;  // an answer nobody takes is dropped after this
constexpr std::uint32_t CACHE_KEEP_MS = 60 * 1000;   // a bot's cached answer serves the same question this long
constexpr std::size_t CACHE_ENTRIES = 4;             // per bot; the least recently used goes
constexpr std::size_t TIMING_SAMPLES = 1024;         // recent searches behind the p50/p95 of `playerbots route status`
constexpr std::uint32_t SERVICE_THREADS_MAX = 8;
constexpr std::uint32_t SERVICE_SWEEP_MS = 1000;

struct Answer
{
    std::uint32_t requestId{0};  // the Snapshot's: an answer to an older request can arrive after a newer Submit
    bool cached{false};
    Result result;
};

struct ServiceStats
{
    std::uint32_t threads{0};
    std::uint64_t requests{0};  // accepted by Submit
    std::uint64_t replaced{0};  // a waiting request superseded by the same bot's newer one
    std::uint64_t dropped{0};   // the oldest waiting request, when the queue was full
    std::uint64_t searches{0};
    std::uint64_t cacheHits{0};
    std::uint64_t expanded{0};    // graph vertices over all searches
    std::uint64_t expired{0};     // answers nobody took within RESULT_KEEP_MS
    std::uint64_t superseded{0};  // answers not kept: the bot had asked again since its request was taken
    std::uint32_t waiting{0};
    std::uint32_t cachedBots{0};
    std::uint32_t searchP50Us{0};
    std::uint32_t searchP95Us{0};
    std::uint32_t waitP50Us{0};
    std::uint32_t waitP95Us{0};
};

class RouteService
{
public:
    using Clock = std::chrono::steady_clock;
    // Runs on a worker after each request: whether it searched (else the cache answered), the search's and the
    // queue wait's microseconds. The server feeds its PerfMonitor rows with it
    using Sink = std::function<void(bool searched, std::uint64_t searchUs, std::uint64_t waitUs)>;

    RouteService(TravelGraph const& graph, StuckBook const& stuck) : _graph(graph), _stuck(stuck) {}
    ~RouteService() { Stop(); }
    RouteService(RouteService const&) = delete;
    RouteService& operator=(RouteService const&) = delete;

    // The server's (RouteService.cpp): TravelGraph::Instance() and StuckBook::Instance()
    static RouteService& Instance();

    // Once; later calls do nothing. threads is clamped to 1..SERVICE_THREADS_MAX
    void Start(std::uint32_t threads, Sink sink = {})
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_workers.empty() || _stopping)
            return;
        _sink = std::move(sink);
        std::uint32_t const count = std::clamp<std::uint32_t>(threads, 1, SERVICE_THREADS_MAX);
        for (std::uint32_t i = 0; i < count; ++i)
            _workers.emplace_back([this] { Work(); });
    }

    // Wakes and joins the workers; waiting requests are dropped. A stopped service accepts nothing
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stopping = true;
        }
        _wake.notify_all();
        for (std::thread& worker : _workers)
            if (worker.joinable())
                worker.join();
        std::lock_guard<std::mutex> lock(_mutex);
        _workers.clear();
        _order.clear();
        _waiting.clear();
    }

    // Any thread; never waits for a search. False = no route now: the graph is not ready or the service not running.
    // A bot has at most one waiting request: a newer one replaces it in place
    bool Submit(std::uint64_t bot, Request const& request, Params const& params)
    {
        if (!_graph.IsReady())
            return false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_workers.empty() || _stopping)
                return false;
            ++_stats.requests;
            auto const waiting = _waiting.find(bot);
            if (waiting != _waiting.end())
            {
                waiting->second = {request, params, Clock::now(), ++_sequence};
                ++_stats.replaced;
                return true;
            }
            if (_waiting.size() >= SERVICE_QUEUE_MAX)
            {
                _waiting.erase(_order.front());
                _order.pop_front();
                ++_stats.dropped;
            }
            _waiting.emplace(bot, Job{request, params, Clock::now(), ++_sequence});
            _order.push_back(bot);
        }
        _wake.notify_one();
        return true;
    }

    // Any thread: the bot's answer once it is done, while younger than RESULT_KEEP_MS
    std::optional<Answer> TryTake(std::uint64_t bot)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto const done = _done.find(bot);
        if (done == _done.end())
            return std::nullopt;
        bool const fresh = Clock::now() - done->second.at <= std::chrono::milliseconds(RESULT_KEEP_MS);
        std::optional<Answer> answer;
        if (fresh)
            answer = std::move(done->second.answer);
        else
            ++_stats.expired;
        _done.erase(done);
        return answer;
    }

    ServiceStats Stats() const
    {
        std::vector<std::uint32_t> searches;
        std::vector<std::uint32_t> waits;
        ServiceStats stats;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            stats = _stats;
            stats.threads = static_cast<std::uint32_t>(_workers.size());
            stats.waiting = static_cast<std::uint32_t>(_waiting.size());
            stats.cachedBots = static_cast<std::uint32_t>(_cache.size());
            searches = _searchUs;
            waits = _waitUs;
        }
        auto const percentile = [](std::vector<std::uint32_t>& samples, std::size_t percent) -> std::uint32_t
        {
            if (samples.empty())
                return 0;
            auto const at = samples.begin() + static_cast<std::ptrdiff_t>((samples.size() - 1) * percent / 100);
            std::nth_element(samples.begin(), at, samples.end());
            return *at;
        };
        stats.searchP50Us = percentile(searches, 50);
        stats.searchP95Us = percentile(searches, 95);
        stats.waitP50Us = percentile(waits, 50);
        stats.waitP95Us = percentile(waits, 95);
        return stats;
    }

private:
    struct Job
    {
        Request request;
        Params params;
        Clock::time_point submitted;
        std::uint64_t sequence{0};  // Submit's order over all bots: the bot's later request has a higher one
    };

    struct Done
    {
        Answer answer;
        Clock::time_point at;
        std::uint64_t sequence{0};
    };

    // What decides a route besides the exact positions; one of them differs, the cached answer does not serve
    struct Question
    {
        std::uint32_t startNode{TravelGraph::NO_NODE};
        std::uint32_t goalNode{TravelGraph::NO_NODE};
        std::uint32_t stuckGeneration{0};
        Snapshot snapshot;
        Params params;

        bool Same(Question const& other) const
        {
            Snapshot const& a = snapshot;
            Snapshot const& b = other.snapshot;
            return startNode == other.startNode && goalNode == other.goalNode &&
                   stuckGeneration == other.stuckGeneration && a.team == b.team && a.level == b.level &&
                   std::bit_width(a.money) == std::bit_width(b.money) && a.knownTaxi == b.knownTaxi &&
                   a.hasHearthstone == b.hasHearthstone && (a.hearthCooldownMs == 0) == (b.hearthCooldownMs == 0) &&
                   a.hearth.mapId == b.hearth.mapId && a.hearth.x == b.hearth.x && a.hearth.y == b.hearth.y &&
                   a.runSpeed == b.runSpeed && a.swimSpeed == b.swimSpeed && a.thrift == b.thrift &&
                   a.allowTransport == b.allowTransport && params.stuckPenalty == other.params.stuckPenalty &&
                   params.transportWaitSeconds == other.params.transportWaitSeconds &&
                   params.flightSpeed == other.params.flightSpeed && params.heuristic == other.params.heuristic;
        }
    };

    struct CacheEntry
    {
        Question question;
        Result result;
        std::uint32_t routeFare{0};  // all the route's fares (RouteFare): a purse below this misses
        Clock::time_point at;
        Clock::time_point used;
    };

    using Cache = std::vector<CacheEntry>;

    static std::uint32_t Micros(Clock::duration duration)
    {
        auto const count = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        return static_cast<std::uint32_t>(std::clamp<long long>(count, 0, 0xFFFFFFFFLL));
    }

    static void Sample(std::vector<std::uint32_t>& samples, std::size_t& next, std::uint32_t value)
    {
        if (samples.size() < TIMING_SAMPLES)
            samples.push_back(value);
        else
            samples[next] = value;
        next = (next + 1) % TIMING_SAMPLES;
    }

    std::uint32_t NearestNode(Point const& at) const
    {
        std::vector<std::uint32_t> const nearest = _graph.NearestNodes(at.mapId, at.x, at.y, at.z, 1);
        return nearest.empty() ? TravelGraph::NO_NODE : nearest.front();
    }

    // Under _mutex: answers older than RESULT_KEEP_MS and cache entries older than CACHE_KEEP_MS go
    void Sweep(Clock::time_point now)
    {
        if (now - _swept < std::chrono::milliseconds(SERVICE_SWEEP_MS))
            return;
        _swept = now;
        for (auto it = _done.begin(); it != _done.end();)
        {
            if (now - it->second.at > std::chrono::milliseconds(RESULT_KEEP_MS))
            {
                ++_stats.expired;
                it = _done.erase(it);
            }
            else
                ++it;
        }
        for (auto it = _cache.begin(); it != _cache.end();)
        {
            Cache& entries = it->second;
            std::erase_if(entries, [now](CacheEntry const& entry)
                          { return now - entry.at > std::chrono::milliseconds(CACHE_KEEP_MS); });
            it = entries.empty() ? _cache.erase(it) : std::next(it);
        }
    }

    void Work()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        while (true)
        {
            _wake.wait(lock, [this] { return _stopping || !_order.empty(); });
            if (_stopping)
                return;
            std::uint64_t const bot = _order.front();
            _order.pop_front();
            auto const waiting = _waiting.find(bot);
            Job job = std::move(waiting->second);
            _waiting.erase(waiting);
            lock.unlock();

            Clock::time_point const picked = Clock::now();
            std::shared_ptr<StuckEdges const> const stuck = _stuck.Current();  // one copy: fixed for this search
            Question question{NearestNode(job.request.snapshot.position), NearestNode(job.request.goal),
                              stuck->generation, job.request.snapshot, job.params};
            bool const cacheable =
                question.startNode != TravelGraph::NO_NODE && question.goalNode != TravelGraph::NO_NODE;

            Answer answer;
            answer.requestId = job.request.snapshot.requestId;
            lock.lock();
            if (cacheable)
            {
                auto const cached = _cache.find(bot);
                if (cached != _cache.end())
                {
                    for (CacheEntry& entry : cached->second)
                    {
                        if (picked - entry.at > std::chrono::milliseconds(CACHE_KEEP_MS) ||
                            job.request.snapshot.money < entry.routeFare || !entry.question.Same(question))
                            continue;
                        entry.used = picked;
                        answer.result = entry.result;
                        answer.cached = true;
                        break;
                    }
                }
            }
            lock.unlock();

            std::uint32_t searchUs = 0;
            if (answer.cached)
            {
                // Same nodes, nearby positions: the cached legs run from where the bot stands to where it goes now
                if (!answer.result.legs.empty())
                {
                    answer.result.legs.front().from = job.request.snapshot.position;
                    answer.result.legs.back().to = job.request.goal;
                }
            }
            else
            {
                answer.result = Search(_graph, job.request, *stuck, job.params);
                searchUs = Micros(Clock::now() - picked);
            }
            std::uint32_t const waitUs = Micros(picked - job.submitted);

            lock.lock();
            Clock::time_point const now = Clock::now();
            if (answer.cached)
                ++_stats.cacheHits;
            else
            {
                ++_stats.searches;
                _stats.expanded += answer.result.expandedNodes;
                Sample(_searchUs, _nextSearch, searchUs);
                if (cacheable && answer.result.failure != Failure::GraphNotReady)
                {
                    CacheEntry entry{question, answer.result, RouteFare(answer.result), now, now};
                    Cache& entries = _cache[bot];
                    auto const same = std::find_if(entries.begin(), entries.end(), [&question](CacheEntry const& old)
                                                   { return old.question.Same(question); });
                    if (same != entries.end())
                        *same = std::move(entry);  // the purse fell below its fare: the new route replaces it
                    else
                    {
                        if (entries.size() >= CACHE_ENTRIES)
                            entries.erase(std::min_element(entries.begin(), entries.end(),
                                                           [](CacheEntry const& a, CacheEntry const& b)
                                                           { return a.used < b.used; }));
                        entries.push_back(std::move(entry));
                    }
                }
            }
            Sample(_waitUs, _nextWait, waitUs);
            bool const searched = !answer.cached;
            // With several workers the bot's later request can finish first, or still wait: only the latest answer
            // is kept, else the bot would drop the older one and never see the newer
            auto const done = _done.find(bot);
            if (_waiting.contains(bot) || (done != _done.end() && done->second.sequence > job.sequence))
                ++_stats.superseded;
            else
                _done[bot] = {std::move(answer), now, job.sequence};
            Sweep(now);
            lock.unlock();

            if (_sink)
                _sink(searched, searchUs, waitUs);
            lock.lock();
        }
    }

    TravelGraph const& _graph;
    StuckBook const& _stuck;
    Sink _sink;  // set before the workers start, read-only afterwards

    mutable std::mutex _mutex;
    std::condition_variable _wake;
    bool _stopping{false};
    std::vector<std::thread> _workers;
    std::deque<std::uint64_t> _order;  // bots with a waiting request, oldest first
    std::unordered_map<std::uint64_t, Job> _waiting;
    std::uint64_t _sequence{0};
    std::unordered_map<std::uint64_t, Done> _done;
    std::unordered_map<std::uint64_t, Cache> _cache;
    ServiceStats _stats;
    std::vector<std::uint32_t> _searchUs;
    std::vector<std::uint32_t> _waitUs;
    std::size_t _nextSearch{0};
    std::size_t _nextWait{0};
    Clock::time_point _swept{};
};

// Server side (RouteService.cpp), map thread: the bot as the search sees it, and the tuning values from the config
// (WorldTravel.StuckPenalty, TransportWaitSeconds; worldserver TaxiFlightSpeed)
Snapshot FillSnapshot(Player const* bot, std::uint32_t requestId);
Params ConfiguredParams();
// World thread: starts RouteService::Instance() with WorldTravel.RouteThreads workers and its PerfMonitor rows
void StartRouteService();
// One line for `playerbots route status`: the service's counters and the stuck edges flagged now
std::string RouteServiceStatus();

// g) The service under load: four threads submit 250 requests each (50 random pairs, 3 rounds while this thread
//    republishes a stuck flag on one of their walk edges, then each pair twice in a row once it stopped) to a
//    four-worker service. Every answer must be the single-thread search with or without that flag, each repeated
//    question a cache hit. Then each thread submits a search and, without waiting, a question its cache answers at
//    once, 49 times: the second's answer must arrive though it often finishes first. Last, on a one-worker service, a
//    purse of the same bit width that no longer pays a cached route's fare must get a fresh search. Returns the
//    number of checks
inline std::uint32_t ServiceSelftest(TravelGraph const& graph, std::vector<std::string>& failures,
                                     std::vector<std::string>& notes)
{
    constexpr std::uint32_t THREADS = 4;
    constexpr std::uint32_t PAIRS = 50;
    constexpr std::uint32_t TOGGLED_ROUNDS = 3;
    constexpr auto ANSWER_DEADLINE = std::chrono::seconds(30);

    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back(what);
        return ok;
    };
    if (!check(graph.IsReady(), "g) graph ready"))
        return checks;

    StuckBook book;
    {
        TravelGraph const idle;
        RouteService unready(idle, book);
        unready.Start(1);
        check(!unready.Submit(1, Request{}, Params{}), "g) a service on a graph that is not ready accepts a request");
        RouteService stopped(graph, book);
        check(!stopped.Submit(1, Request{}, Params{}), "g) a service that was never started accepts a request");
    }

    // The pairs of f) with the hearth ready, and a walk edge one of their routes uses
    std::mt19937 random(20261109u);
    std::uniform_int_distribution<std::uint32_t> anyNode(0, static_cast<std::uint32_t>(graph.NodeCount() - 1));
    std::vector<Request> requests;
    for (std::uint32_t i = 0; i < PAIRS; ++i)
    {
        TravelGraphNode const& a = graph.GetNode(anyNode(random));
        TravelGraphNode const& b = graph.GetNode(anyNode(random));
        Snapshot traveller;
        traveller.position = {a.mapId, a.x, a.y, a.z};
        traveller.team = static_cast<std::uint8_t>(i % 2);
        traveller.level = static_cast<std::uint8_t>(10 + (i * 7) % 71);
        traveller.money = 1000000;
        traveller.knownTaxi.fill(0xFFFFFFFFu);
        traveller.allowTransport = true;
        requests.push_back({traveller, {b.mapId, b.x, b.y, b.z}});
    }
    Params const params;
    std::vector<Result> plain;
    StuckBook::Report report{};
    bool edge = false;
    for (Request const& request : requests)
    {
        plain.push_back(Search(graph, request, StuckEdges{}, params));
        for (Leg const& leg : plain.back().legs)
        {
            if (edge || leg.type != LegType::Walk || leg.nodes.size() < 2)
                continue;
            TravelGraphNode const& from = graph.GetNode(leg.nodes[0]);
            TravelGraphNode const& to = graph.GetNode(leg.nodes[1]);
            report = {{from.mapId, from.x, from.y, from.z}, {to.mapId, to.x, to.y, to.z}};
            edge = true;
        }
    }
    std::uint32_t const avoidMs = 600 * 1000;
    std::uint32_t nowMs = 1000;
    book.Push(report);
    if (!check(edge && book.Update(graph, nowMs, avoidMs) == 1, "g) no walk edge to flag"))
        return checks;
    std::shared_ptr<StuckEdges const> const flaggedEdges = book.Current();
    std::vector<Result> flagged;
    std::uint32_t differ = 0;
    for (std::size_t i = 0; i < requests.size(); ++i)
    {
        flagged.push_back(Search(graph, requests[i], *flaggedEdges, params));
        differ += flagged.back().totalSeconds != plain[i].totalSeconds ? 1 : 0;
    }

    std::atomic<std::uint32_t> sinkCalls{0};
    std::atomic<std::uint32_t> togglesSeen{0};
    std::atomic<std::uint32_t> inToggledRounds{THREADS};
    std::atomic<bool> toggling{true};
    std::atomic<std::uint32_t> wrong{0};
    std::atomic<std::uint32_t> lost{0};
    std::atomic<std::uint32_t> withFlag{0};
    std::atomic<std::uint32_t> repeatHits{0};
    std::atomic<std::uint32_t> staleTaken{0};
    RouteService service(graph, book);
    service.Start(THREADS, [&sinkCalls](bool, std::uint64_t, std::uint64_t) { ++sinkCalls; });

    // The consumer's loop (WT8): poll, drop an answer to an older request of the bot, keep polling
    auto const answerFor = [&](RouteService& on, std::uint64_t bot, std::uint32_t requestId) -> std::optional<Answer>
    {
        auto const deadline = RouteService::Clock::now() + ANSWER_DEADLINE;
        while (RouteService::Clock::now() < deadline)
        {
            std::optional<Answer> answer = on.TryTake(bot);
            if (answer && answer->requestId == requestId)
                return answer;
            if (answer)
                ++staleTaken;
            else
                std::this_thread::yield();
        }
        return std::nullopt;
    };
    auto const ask = [&](std::uint64_t bot, std::uint32_t requestId, std::size_t pair) -> std::optional<Answer>
    {
        Request request = requests[pair];
        request.snapshot.requestId = requestId;
        if (!service.Submit(bot, request, params))
            return std::nullopt;
        return answerFor(service, bot, requestId);
    };
    auto const same = [](Result const& a, Result const& b)
    {
        if (a.ok != b.ok || a.totalSeconds != b.totalSeconds || a.legs.size() != b.legs.size())
            return false;
        for (std::size_t leg = 0; leg < a.legs.size(); ++leg)
            if (a.legs[leg].type != b.legs[leg].type || a.legs[leg].nodes != b.legs[leg].nodes)
                return false;
        return true;
    };
    auto const submitter = [&](std::uint32_t thread)
    {
        std::uint64_t const bot = thread + 1;
        std::uint32_t requestId = 0;
        auto const judge = [&](std::optional<Answer> const& answer, std::size_t pair) -> bool
        {
            if (!answer)
            {
                ++lost;
                return false;
            }
            bool const asPlain = same(answer->result, plain[pair]);
            bool const asFlagged = same(answer->result, flagged[pair]);
            wrong += asPlain || asFlagged ? 0 : 1;
            withFlag += asFlagged && !asPlain ? 1 : 0;
            return answer->cached;
        };
        auto const verify = [&](std::size_t pair) -> bool { return judge(ask(bot, ++requestId, pair), pair); };
        for (std::uint32_t round = 0; round < TOGGLED_ROUNDS; ++round)
            for (std::uint32_t pair = 0; pair < PAIRS; ++pair)
                verify(pair);
        --inToggledRounds;
        toggling.wait(true);  // the flags stay as they are from here: the same question twice is one search
        for (std::uint32_t pair = 0; pair < PAIRS; ++pair)
        {
            verify(pair);
            repeatHits += verify(pair) ? 1 : 0;
        }
        // Two requests without waiting: a search, then the last pair, cached just now and answered at once. A gap of
        // 0-75 us lets a worker take the first before the second replaces it, so the second often finishes first
        for (std::uint32_t pair = 0; pair + 1 < PAIRS; ++pair)
        {
            Request first = requests[pair];
            first.snapshot.requestId = ++requestId;
            Request second = requests[PAIRS - 1];
            second.snapshot.requestId = ++requestId;
            bool submitted = service.Submit(bot, first, params);
            auto const gapEnd = RouteService::Clock::now() + std::chrono::microseconds(25 * (pair % 4));
            while (RouteService::Clock::now() < gapEnd)
                std::this_thread::yield();
            submitted = service.Submit(bot, second, params) && submitted;
            if (!submitted)
            {
                ++lost;
                continue;
            }
            judge(answerFor(service, bot, requestId), PAIRS - 1);
        }
    };
    std::vector<std::thread> threads;
    for (std::uint32_t thread = 0; thread < THREADS; ++thread)
        threads.emplace_back(submitter, thread);
    // The world thread's part: expire the flag, flag the edge again, each a new publication, while searches run
    while (inToggledRounds.load() != 0)
    {
        nowMs += avoidMs + 1000;
        book.Update(graph, nowMs, avoidMs);
        book.Push(report);
        book.Update(graph, nowMs, avoidMs);
        togglesSeen += 2;
        std::this_thread::yield();
    }
    toggling = false;
    toggling.notify_all();
    for (std::thread& thread : threads)
        thread.join();
    service.Stop();  // joins the workers: an unwaited first request may still run
    service.Stop();  // twice is harmless
    ServiceStats const stats = service.Stats();

    // The last round asks each pair twice; the unwaited round submits two requests and awaits one answer, 49 times
    std::uint32_t const awaited = THREADS * PAIRS * (TOGGLED_ROUNDS + 2) + THREADS * (PAIRS - 1);
    std::uint32_t const asked = awaited + THREADS * (PAIRS - 1);
    check(lost == 0, "g) " + std::to_string(lost.load()) + " of " + std::to_string(awaited) +
                         " awaited answers did not arrive within the deadline");
    check(wrong == 0, "g) " + std::to_string(wrong.load()) +
                          " answers differ from the single-thread search with and without the flag");
    check(repeatHits == THREADS * PAIRS, "g) " + std::to_string(repeatHits.load()) + " of " +
                                             std::to_string(THREADS * PAIRS) +
                                             " repeated questions were answered from the cache");
    // A request replaced while it waited is never searched
    check(stats.requests == asked && stats.searches + stats.cacheHits + stats.replaced == asked,
          "g) the counters do not add up: " + std::to_string(stats.requests) + " requests, " +
              std::to_string(stats.searches) + " searches, " + std::to_string(stats.cacheHits) + " cache hits, " +
              std::to_string(stats.replaced) + " replaced");
    check(sinkCalls == asked - stats.replaced, "g) the sink ran " + std::to_string(sinkCalls.load()) + " times");

    // A purse of the cached question's bit width that no longer pays the route's fare: a fresh search. Thrift 0 keeps
    // the flight on a purse just above its fare
    RouteService fares(graph, book);
    fares.Start(1);
    bool fareChecked = false;
    for (std::size_t pair = 0; pair < requests.size() && !fareChecked; ++pair)
    {
        std::uint32_t const fare = RouteFare(plain[pair]);
        if (fare < 3 || std::has_single_bit(fare))
            continue;
        std::uint64_t const bot = 1000 + pair;
        Request rich = requests[pair];
        rich.snapshot.money = std::bit_ceil(fare) - 1;
        rich.snapshot.thrift = 0;
        rich.snapshot.requestId = 1;
        Request poor = rich;
        poor.snapshot.money = std::bit_floor(fare);
        poor.snapshot.requestId = 2;
        std::optional<Answer> const flown = fares.Submit(bot, rich, params) ? answerFor(fares, bot, 1) : std::nullopt;
        if (!flown || RouteFare(flown->result) <= poor.snapshot.money)
            continue;
        std::optional<Answer> const after = fares.Submit(bot, poor, params) ? answerFor(fares, bot, 2) : std::nullopt;
        check(after && !after->cached, "g) a route with a fare above the purse came from the cache");
        fareChecked = true;
    }
    check(fareChecked, "g) no route with a flight leg to check the cached fare against a smaller purse");
    fares.Stop();
    notes.push_back("g) " + std::to_string(asked) + " requests on " + std::to_string(THREADS) + " workers: " +
                    std::to_string(stats.searches) + " searches, " + std::to_string(stats.cacheHits) + " cache hits, " +
                    std::to_string(withFlag.load()) + " answers with the flag (" + std::to_string(differ) + " of " +
                    std::to_string(PAIRS) + " pairs change with it), " + std::to_string(togglesSeen.load()) +
                    " publications, unwaited pairs: " + std::to_string(stats.replaced) + " replaced, " +
                    std::to_string(stats.superseded) + " older answers not kept, " + std::to_string(staleTaken.load()) +
                    " taken and dropped; search p50/p95 " + std::to_string(stats.searchP50Us) + "/" +
                    std::to_string(stats.searchP95Us) + " us");
    return checks;
}
}  // namespace TravelRoute

#endif
