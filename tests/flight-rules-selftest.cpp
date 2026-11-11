/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the flight rules (src/Ai/World/Rpg/FlightRules.h, upkeep plan WU9) on made-up terrain: the shared
// checks print each route's height profile (flat ground, a low ridge flown over, a high range around, a pass, a no-fly
// area, careful and daring bots, two guids, a plateau, rolling hills); then the same searches from several threads at
// once must match one thread's, and a continent-sized map times the search.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /O2 /W4 /WX /I src/Ai/World/Rpg
// tests/flight-rules-selftest.cpp

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "FlightRules.h"

using namespace FlightRules;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& what)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << what << "\n";
}

constexpr std::uint32_t MAP = 571;
constexpr float CELL = 533.33333f / 8.0f;
constexpr std::int32_t BIG_SIDE = 240;  // 16 km square: larger than Outland's or Northrend's open box
constexpr std::uint32_t SEARCHES = 200;
constexpr std::uint32_t THREADS = 4;

// A continent-like terrain: rolling ground, a few mountains, two no-fly discs (a city, a battle zone)
HeightMap BigMap()
{
    struct Peak
    {
        float x;
        float y;
        float height;
        float radius;
    };
    std::vector<Peak> const peaks = {{4000.0f, 4000.0f, 900.0f, 1500.0f},
                                     {11000.0f, 6000.0f, 700.0f, 2000.0f},
                                     {7000.0f, 12000.0f, 1100.0f, 1200.0f},
                                     {13000.0f, 13000.0f, 500.0f, 2500.0f}};
    auto const height = [&peaks](float x, float y)
    {
        float ground = 30.0f + 25.0f * std::sin(x / 300.0f) * std::cos(y / 410.0f);
        for (Peak const& peak : peaks)
        {
            float const d = std::hypot(x - peak.x, y - peak.y) / peak.radius;
            ground += peak.height * std::exp(-d * d);
        }
        return ground;
    };
    auto const open = [](float x, float y)
    { return std::hypot(x - 8000.0f, y - 8000.0f) > 600.0f && std::hypot(x - 3000.0f, y - 12000.0f) > 1500.0f; };
    return MakeMap(MAP, 0.0f, 0.0f, CELL, BIG_SIDE, BIG_SIDE, height, open);
}

std::vector<FlightRequest> Requests(HeightMap const& map)
{
    std::vector<FlightRequest> requests;
    std::uint32_t seed = 12345;
    auto const next = [&seed]()
    {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    float const side = CELL * static_cast<float>(BIG_SIDE);
    while (requests.size() < SEARCHES)
    {
        FlightRequest r;
        r.mapId = map.mapId;
        r.startX = next() * side;
        r.startY = next() * side;
        r.goalX = next() * side;
        r.goalY = next() * side;
        if (!map.IsOpen(r.startX, r.startY) || !map.IsOpen(r.goalX, r.goalY))
            continue;
        r.startZ = static_cast<float>(map.Ground(map.CellX(r.startX), map.CellY(r.startY)));
        r.goalZ = static_cast<float>(map.Ground(map.CellX(r.goalX), map.CellY(r.goalY)));
        r.speed = 26.6f;
        FlightStyle const style = BotFlightStyle(next() * 2.0f - 1.0f, static_cast<std::uint32_t>(requests.size()));
        r.clearanceYards = style.clearanceYards;
        r.offsetYards = style.offsetYards;
        requests.push_back(r);
    }
    return requests;
}

bool SameRoute(FlightRoute const& a, FlightRoute const& b)
{
    if (a.ok != b.ok || a.waypoints.size() != b.waypoints.size() || a.expandedCells != b.expandedCells)
        return false;
    for (std::size_t i = 0; i < a.waypoints.size(); ++i)
        if (a.waypoints[i].x != b.waypoints[i].x || a.waypoints[i].y != b.waypoints[i].y ||
            a.waypoints[i].z != b.waypoints[i].z)
            return false;
    return true;
}
}  // namespace

int main()
{
    std::vector<std::string> failures;
    std::vector<std::string> notes;
    std::uint32_t const checks = Selftest(failures, notes);
    for (std::string const& line : notes)
        std::cout << line << "\n";
    for (std::string const& line : failures)
        std::cout << "FAIL: " << line << "\n";
    passed += static_cast<int>(checks - failures.size());
    failed += static_cast<int>(failures.size());

    HeightMap const big = BigMap();
    std::cout << "big map: " << big.width << "x" << big.height << " cells, " << big.openCells << " open, "
              << big.closedCells << " closed, ground " << big.minGround << ".." << big.maxGround << " yd, "
              << big.Bytes() / 1024 << " KB\n";
    std::vector<FlightRequest> const requests = Requests(big);

    // One thread: the reference routes and the timing
    std::vector<FlightRoute> reference;
    std::vector<double> micros;
    std::uint64_t expanded = 0;
    std::uint32_t routed = 0;
    for (FlightRequest const& r : requests)
    {
        auto const started = std::chrono::steady_clock::now();
        reference.push_back(Search(big, r));
        auto const took = std::chrono::steady_clock::now() - started;
        micros.push_back(std::chrono::duration<double, std::micro>(took).count());
        expanded += reference.back().expandedCells;
        if (reference.back().ok)
            ++routed;
    }
    std::vector<double> sorted = micros;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double const us : micros)
        sum += us;
    std::cout << "search on the big map: " << routed << " of " << requests.size() << " routed; mean "
              << std::llround(sum / static_cast<double>(micros.size())) << " us, p50 "
              << std::llround(sorted[sorted.size() / 2]) << " us, p95 "
              << std::llround(sorted[sorted.size() * 95 / 100]) << " us, max " << std::llround(sorted.back())
              << " us; mean " << expanded / requests.size() << " cells expanded (first search includes the buffers)\n";
    Check(routed == requests.size(), "big map: every request between open points routes");
    std::cout
        << "big map, the slowest route: "
        << ProfileLine(
               reference[static_cast<std::size_t>(std::max_element(micros.begin(), micros.end()) - micros.begin())])
        << "\n";

    // The same searches from several threads at once give the same routes (per-thread scratch, immutable map)
    std::vector<std::vector<FlightRoute>> results(THREADS);
    std::vector<std::thread> workers;
    for (std::uint32_t t = 0; t < THREADS; ++t)
        workers.emplace_back(
            [&big, &requests, &results, t]()
            {
                for (std::size_t i = 0; i < requests.size(); ++i)
                    results[t].push_back(Search(big, requests[(i + t * 37) % requests.size()]));
            });
    for (std::thread& worker : workers)
        worker.join();
    bool same = true;
    for (std::uint32_t t = 0; t < THREADS; ++t)
        for (std::size_t i = 0; i < requests.size(); ++i)
            same = same && SameRoute(results[t][i], reference[(i + t * 37) % requests.size()]);
    Check(same, "threads: the same routes as one thread");

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
