/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERROUTECOST_H
#define _PLAYERBOT_PLANNERROUTECOST_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "QuestCircuit.h"
#include "RouteRules.h"

// The planner's price of a way (planning WN1a, PLAN 3.20): the seconds of the route a trip would take there, from one
// TravelRoute::Search per place asked, over the bot's route snapshot (Snapshot::route, filled on its map thread), the
// stuck edges and the route tuning taken when the job was made. Pure: a planner worker runs it, no Player, no game
// state. An answer is kept for the job, so a place asked twice is searched once; past the cap a place is left out
// (negative seconds), never priced by yards. tests/goal-model-selftest.cpp and planner-pool-selftest.cpp run it on
// fixture graphs.
namespace Planner
{
// Searches one goals job may run. A job asks a few dozen places at most (four next-zone hubs, the deliveries' enders,
// the circuit's move targets, the class quests' givers); a search over the shipped graph takes some 0.1-0.4 ms, so 24
// keep a job within a few ms. WN1c sets it from the measured cost
constexpr std::uint32_t ROUTE_SEARCHES_PER_JOB = 24;
constexpr float NO_ROUTE_SECONDS = -1.0f;

inline TravelRoute::Point RoutePoint(Place const& place) { return {place.map, place.x, place.y, place.z}; }

inline bool SamePlace(Place const& a, Place const& b)
{
    return a.map == b.map && a.x == b.x && a.y == b.y && a.z == b.z;
}

// What a job's cost takes at its start, on the bot's map thread
struct RouteCostInputs
{
    std::shared_ptr<TravelRoute::StuckEdges const> stuck;
    TravelRoute::Params params;
};

struct RouteCostCounts
{
    std::uint32_t asked{0};     // hook calls
    std::uint32_t searches{0};  // route searches run
    std::uint32_t noRoute{0};   // searches that found no route
    std::uint32_t capped{0};    // places left out past the cap
    std::uint64_t micros{0};    // the searches' time
};

// One job's cost: its memo, cap and counts. The hook's copies share it; the last one gone reports the counts (onEnd)
class RouteCost
{
public:
    using OnEnd = std::function<void(RouteCostCounts const&)>;

    RouteCost(TravelGraph const& graph, TravelRoute::Snapshot const& bot, RouteCostInputs inputs,
              std::uint32_t cap = ROUTE_SEARCHES_PER_JOB, OnEnd onEnd = {})
        : _graph(graph), _bot(bot), _inputs(std::move(inputs)), _cap(cap), _onEnd(std::move(onEnd))
    {
        if (!_inputs.stuck)
            _inputs.stuck = std::make_shared<TravelRoute::StuckEdges const>();
    }
    RouteCost(RouteCost const&) = delete;
    RouteCost& operator=(RouteCost const&) = delete;
    ~RouteCost()
    {
        if (_onEnd)
            _onEnd(_counts);
    }

    // CircuitContext::travel over this cost; empty while the graph is not ready (the callers' straight yards)
    static TravelCost Hook(std::shared_ptr<RouteCost> const& cost)
    {
        if (!cost || !cost->_graph.IsReady())
            return {};
        return [cost](Place const& from, Place const& to) { return cost->Seconds(from, to); };
    }

    float Seconds(Place const& from, Place const& to)
    {
        ++_counts.asked;
        if (std::optional<float> const known = Answer(from, to))
            return *known;
        float seconds = NO_ROUTE_SECONDS;
        if (_counts.searches >= _cap)
            ++_counts.capped;
        else
        {
            ++_counts.searches;
            auto const started = std::chrono::steady_clock::now();
            TravelRoute::Request request(_bot, RoutePoint(to));
            request.snapshot.position = RoutePoint(from);
            TravelRoute::Result const route = TravelRoute::Search(_graph, request, *_inputs.stuck, _inputs.params);
            _counts.micros += static_cast<std::uint64_t>(std::max<long long>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
                    .count(),
                0));
            if (route.ok && std::isfinite(route.totalSeconds) && route.totalSeconds >= 0.0f)
                seconds = route.totalSeconds;
            else
                ++_counts.noRoute;
        }
        _answers.push_back({from, to, seconds});
        return seconds;
    }

    // The job's answer for a way asked already: the route's seconds, negative for no route or past the cap
    std::optional<float> Answer(Place const& from, Place const& to) const
    {
        for (Asked const& asked : _answers)
            if (SamePlace(asked.to, to) && SamePlace(asked.from, from))
                return asked.seconds;
        return std::nullopt;
    }

    RouteCostCounts const& Counts() const { return _counts; }

private:
    struct Asked
    {
        Place from;
        Place to;
        float seconds{NO_ROUTE_SECONDS};
    };

    TravelGraph const& _graph;
    TravelRoute::Snapshot const _bot;
    RouteCostInputs _inputs;
    std::uint32_t const _cap;
    OnEnd _onEnd;
    RouteCostCounts _counts;
    std::vector<Asked> _answers;  // a few dozen at most: a scan beats a hash
};

// The server's totals over every job (`playerbots planner status`); any thread adds
struct RouteCostTotals
{
    std::atomic<std::uint64_t> jobs{0};
    std::atomic<std::uint64_t> asked{0};
    std::atomic<std::uint64_t> searches{0};
    std::atomic<std::uint64_t> noRoute{0};
    std::atomic<std::uint64_t> capped{0};

    void Add(RouteCostCounts const& counts)
    {
        jobs.fetch_add(1, std::memory_order_relaxed);
        asked.fetch_add(counts.asked, std::memory_order_relaxed);
        searches.fetch_add(counts.searches, std::memory_order_relaxed);
        noRoute.fetch_add(counts.noRoute, std::memory_order_relaxed);
        capped.fetch_add(counts.capped, std::memory_order_relaxed);
    }
};

// Server side (PlannerService.cpp). Map thread: the stuck book's current edges and the route tuning for a goals job.
// Worker: the job's travel hook over the published graph from the bot's route snapshot; its end records the counts
// (RouteCostTotals, the PerfMonitor row "planner route cost"); empty while the graph is not ready
RouteCostInputs RouteCostInputsNow();
TravelCost JobTravel(RouteCostInputs const& inputs, TravelRoute::Snapshot const& bot);
}  // namespace Planner

#endif
