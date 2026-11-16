/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FLIGHTSELFTEST_H
#define _PLAYERBOT_FLIGHTSELFTEST_H

#include <cstdint>
#include <string>
#include <vector>

#include "FlightRules.h"

// The flight search's checks (FlightRules.h) on made-up terrain, for tests/flight-rules-selftest.cpp and any
// in-server run; only they include this header.
namespace FlightRules
{
// The checks shared by tests/flight-rules-selftest.cpp and any in-server run, on made-up terrain. Returns the number
// of checks; failures get one line each, notes one profile per route
inline std::uint32_t Selftest(std::vector<std::string>& failures, std::vector<std::string>& notes)
{
    constexpr std::uint32_t MAP = 530;
    constexpr float CELL = 533.33333f / 8.0f;  // the server's cell: an eighth of a terrain grid
    constexpr std::int32_t SIDE = 96;          // 6.4 km square
    constexpr float SPEED = 26.6f;             // epic flying, yards per second
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back(what);
        return ok;
    };
    FlightParams const params;
    FlightStyle neutral;
    neutral.clearanceYards = 40.0f;
    neutral.offsetYards = 0.0f;
    auto const always = [](float, float) { return true; };
    auto const request = [](HeightMap const& map, float sx, float sy, float gx, float gy, FlightStyle const& style)
    {
        FlightRequest r;
        r.mapId = map.mapId;
        r.startX = sx;
        r.startY = sy;
        r.startZ = static_cast<float>(map.Ground(map.CellX(sx), map.CellY(sy)));
        r.goalX = gx;
        r.goalY = gy;
        r.goalZ = static_cast<float>(map.Ground(map.CellX(gx), map.CellY(gy)));
        r.speed = SPEED;
        r.clearanceYards = style.clearanceYards;
        r.offsetYards = style.offsetYards;
        return r;
    };
    auto const highest = [](FlightRoute const& route)
    {
        float z = -std::numeric_limits<float>::infinity();
        for (FlightPoint const& point : route.waypoints)
            z = std::max(z, point.z);
        return z;
    };
    auto const flatLength = [](FlightRoute const& route)
    {
        float length = 0.0f;
        for (std::size_t i = 1; i < route.waypoints.size(); ++i)
            length += Detail::Distance2d(route.waypoints[i - 1], route.waypoints[i]);
        return length;
    };
    auto const sideways = [](FlightRoute const& route, FlightRequest const& r)
    {
        float const dx = r.goalX - r.startX;
        float const dy = r.goalY - r.startY;
        float const length = std::hypot(dx, dy);
        float widest = 0.0f;
        for (FlightPoint const& point : route.waypoints)
            widest = std::max(widest, std::abs((point.x - r.startX) * dy - (point.y - r.startY) * dx) / length);
        return widest;
    };
    auto const innerLevel = [](FlightRoute const& route, bool& same)
    {
        same = true;
        float const level = route.waypoints.size() > 2 ? route.waypoints[1].z : 0.0f;
        for (std::size_t i = 1; i + 1 < route.waypoints.size(); ++i)
            same = same && route.waypoints[i].z == level;
        return level;
    };
    auto const upThenDown = [](FlightRoute const& route)
    {
        std::size_t i = 1;
        while (i < route.waypoints.size() && route.waypoints[i].z >= route.waypoints[i - 1].z)
            ++i;
        while (i < route.waypoints.size() && route.waypoints[i].z <= route.waypoints[i - 1].z)
            ++i;
        return i == route.waypoints.size();
    };
    auto const segmentsOpen = [](HeightMap const& map, FlightRoute const& route)
    {
        for (std::size_t i = 1; i < route.waypoints.size(); ++i)
            if (Detail::SegmentGround(map, route.waypoints[i - 1], route.waypoints[i]) == Detail::CLOSED_SEGMENT)
                return false;
        return true;
    };
    auto const note = [&notes](std::string const& what, FlightRoute const& route)
    { notes.push_back(what + ": " + ProfileLine(route)); };

    // Flat ground: a straight line at one height, also on a slant the grid does not follow
    HeightMap const flat = MakeMap(MAP, 0.0f, 0.0f, CELL, SIDE, SIDE, [](float, float) { return 0.0f; }, always);
    {
        FlightRequest const r = request(flat, 500.0f, 3000.0f, 5900.0f, 3000.0f, neutral);
        FlightRoute const route = Search(flat, r, params);
        note("flat", route);
        bool same = false;
        float const level = innerLevel(route, same);
        check(route.ok, "flat: a route");
        check(same && std::abs(level - 40.0f) < 0.5f, "flat: one height, the clearance above the ground");
        check(route.segments == 1 && sideways(route, r) < 1.0f, "flat: a straight line");
        check(route.heightChanges == 0, "flat: no height change");
        check(route.seconds > 0.0f && std::abs(route.seconds - route.distance / SPEED) < 0.01f, "flat: seconds");
        float widestGap = 0.0f;
        for (std::size_t i = 1; i < route.waypoints.size(); ++i)
            widestGap = std::max(widestGap, Detail::Distance2d(route.waypoints[i - 1], route.waypoints[i]));
        check(widestGap <= params.waypointMaxYards + 0.5f && widestGap >= 100.0f, "flat: waypoints 100-200 yd apart");
        FlightRoute const again = Search(flat, r, params);
        bool sameRoute = again.waypoints.size() == route.waypoints.size();
        for (std::size_t i = 0; sameRoute && i < route.waypoints.size(); ++i)
            sameRoute = again.waypoints[i].x == route.waypoints[i].x && again.waypoints[i].y == route.waypoints[i].y &&
                        again.waypoints[i].z == route.waypoints[i].z;
        check(sameRoute, "flat: the same request gives the same route");

        FlightRequest const slant = request(flat, 400.0f, 400.0f, 6000.0f, 3500.0f, neutral);
        FlightRoute const cut = Search(flat, slant, params);
        note("flat, slanted", cut);
        check(cut.ok && cut.segments == 1 && sideways(cut, slant) < 1.0f, "flat slant: the zigzag is cut straight");
    }

    // A low ridge across most of the map is flown over; a high one around its end
    HeightMap const lowRidge = MakeMap(
        MAP, 0.0f, 0.0f, CELL, SIDE, SIDE,
        [](float x, float y) { return x >= 3000.0f && x <= 3200.0f && y < 5500.0f ? 40.0f : 0.0f; }, always);
    {
        FlightRequest const r = request(lowRidge, 500.0f, 2000.0f, 5900.0f, 2000.0f, neutral);
        FlightRoute const route = Search(lowRidge, r, params);
        note("low ridge", route);
        check(route.ok && flatLength(route) < 5600.0f, "low ridge: flown over, not around");
        check(highest(route) >= 40.0f + 20.0f, "low ridge: high enough over the ridge");
        check(upThenDown(route), "low ridge: a climb, level and a descent");
    }
    HeightMap const highRange = MakeMap(
        MAP, 0.0f, 0.0f, CELL, SIDE, SIDE,
        [](float x, float y) { return x >= 3000.0f && x <= 3200.0f && y < 5000.0f ? 800.0f : 0.0f; }, always);
    {
        FlightRequest const r = request(highRange, 500.0f, 4000.0f, 5900.0f, 4000.0f, neutral);
        FlightRoute const route = Search(highRange, r, params);
        note("high range", route);
        check(route.ok && highest(route) < 400.0f, "high range: flown around, not over");
        check(route.ok && sideways(route, r) > 900.0f, "high range: around its end");
    }

    // A pass through a range that reaches nearly across: the pass, not the far end, not over
    HeightMap const pass = MakeMap(
        MAP, 0.0f, 0.0f, CELL, SIDE, SIDE,
        [](float x, float y)
        {
            if (x < 3000.0f || x > 3200.0f || y >= 5700.0f)
                return 0.0f;
            return y >= 2900.0f && y <= 3100.0f ? 100.0f : 800.0f;
        },
        always);
    {
        FlightRequest const r = request(pass, 500.0f, 2000.0f, 5900.0f, 2000.0f, neutral);
        FlightRoute const route = Search(pass, r, params);
        note("pass", route);
        check(route.ok && highest(route) < 400.0f, "pass: not over the range");
        check(route.ok && flatLength(route) < 7000.0f, "pass: through the pass, shorter than around");
        check(route.ok && highest(route) >= 100.0f + 20.0f, "pass: high enough over the pass");
    }

    // A no-fly area is flown around; closed ends and a closed ring fail
    auto const outsideDisc = [](float x, float y) { return std::hypot(x - 3200.0f, y - 3200.0f) > 800.0f; };
    HeightMap const noFly = MakeMap(MAP, 0.0f, 0.0f, CELL, SIDE, SIDE, [](float, float) { return 0.0f; }, outsideDisc);
    {
        FlightRequest const r = request(noFly, 500.0f, 3200.0f, 5900.0f, 3200.0f, neutral);
        FlightRoute const route = Search(noFly, r, params);
        note("no-fly area", route);
        check(route.ok && segmentsOpen(noFly, route), "no-fly area: the route stays out of it");
        check(route.ok && sideways(route, r) > 800.0f, "no-fly area: flown around");
        FlightStyle offset = neutral;
        offset.offsetYards = params.offsetMaxYards;
        FlightRoute const shifted = Search(noFly, request(noFly, 500.0f, 3200.0f, 5900.0f, 3200.0f, offset), params);
        check(shifted.ok && segmentsOpen(noFly, shifted), "no-fly area: an offset line stays out of it too");
        check(Search(noFly, request(noFly, 3200.0f, 3200.0f, 5900.0f, 3200.0f, neutral), params).failure ==
                  FlightFailure::StartClosed,
              "no-fly area: a start inside fails");
        check(Search(noFly, request(noFly, 500.0f, 3200.0f, 3300.0f, 3300.0f, neutral), params).failure ==
                  FlightFailure::GoalClosed,
              "no-fly area: a goal inside fails");
        FlightRequest other = r;
        other.mapId = MAP + 1;
        check(Search(noFly, other, params).failure == FlightFailure::NoMap, "a request for another map fails");
    }
    HeightMap const ring = MakeMap(
        MAP, 0.0f, 0.0f, CELL, SIDE, SIDE, [](float, float) { return 0.0f; },
        [](float x, float y)
        {
            float const r = std::hypot(x - 5000.0f, y - 5000.0f);
            return r < 300.0f || r > 500.0f;
        });
    {
        FlightRoute const route = Search(ring, request(ring, 500.0f, 500.0f, 5000.0f, 5000.0f, neutral), params);
        note("goal inside a closed ring", route);
        check(route.failure == FlightFailure::NoPath, "closed ring: no path");
    }

    // Bots differ: a careful bot flies higher than a daring one; two guids do not share a line
    {
        FlightStyle const careful = BotFlightStyle(1.0f, 1001, params);
        FlightStyle const daring = BotFlightStyle(-1.0f, 1001, params);
        FlightStyle const middle = BotFlightStyle(0.0f, 1001, params);
        check(careful.clearanceYards == params.clearanceMaxYards && daring.clearanceYards == params.clearanceMinYards &&
                  std::abs(middle.clearanceYards - 40.0f) < 0.01f,
              "style: the clearance follows caution");
        FlightRoute const high =
            Search(lowRidge, request(lowRidge, 500.0f, 2000.0f, 5900.0f, 2000.0f, careful), params);
        FlightRoute const low = Search(lowRidge, request(lowRidge, 500.0f, 2000.0f, 5900.0f, 2000.0f, daring), params);
        note("low ridge, careful", high);
        note("low ridge, daring", low);
        bool same = false;
        check(high.ok && low.ok && innerLevel(high, same) > innerLevel(low, same) && highest(high) > highest(low),
              "style: a careful bot flies higher than a daring one");

        FlightStyle const first = BotFlightStyle(0.0f, 1001, params);
        FlightStyle const second = BotFlightStyle(0.0f, 1002, params);
        check(std::abs(first.offsetYards) <= params.offsetMaxYards &&
                  std::abs(second.offsetYards) <= params.offsetMaxYards,
              "style: offsets within the widest");
        FlightRoute const one = Search(flat, request(flat, 500.0f, 3000.0f, 5900.0f, 3000.0f, first), params);
        FlightRoute const two = Search(flat, request(flat, 500.0f, 3000.0f, 5900.0f, 3000.0f, second), params);
        std::size_t const middleIndex = one.waypoints.size() / 2;
        check(one.ok && two.ok && one.waypoints.size() == two.waypoints.size() &&
                  std::abs(one.waypoints[middleIndex].y - two.waypoints[middleIndex].y) > 5.0f,
              "style: two bots do not share one line");
        check(one.ok && one.waypoints.front().y == 3000.0f && one.waypoints.back().y == 3000.0f,
              "style: the offset leaves the takeoff and the landing in place");
        notes.push_back("offsets: guid 1001 " + std::to_string(std::lround(first.offsetYards)) + " yd, guid 1002 " +
                        std::to_string(std::lround(second.offsetYards)) + " yd");
    }

    // Few height changes: a plateau is one climb and one descent; rolling hills are one level
    HeightMap const plateau = MakeMap(
        MAP, 0.0f, 0.0f, CELL, SIDE, SIDE, [](float x, float) { return x >= 2500.0f && x <= 3900.0f ? 200.0f : 0.0f; },
        always);
    {
        FlightRoute const route = Search(plateau, request(plateau, 500.0f, 3200.0f, 5900.0f, 3200.0f, neutral), params);
        note("plateau", route);
        check(route.ok && upThenDown(route), "plateau: a climb, level stretches and a descent");
        check(route.ok && route.heightChanges <= 2, "plateau: two height changes at most");
        check(route.ok && highest(route) >= 200.0f + 20.0f, "plateau: high enough over it");
    }
    HeightMap const hills = MakeMap(
        MAP, 0.0f, 0.0f, CELL, SIDE, SIDE,
        [](float x, float y) { return 20.0f + 20.0f * std::sin(x / 70.0f) * std::cos(y / 90.0f); }, always);
    {
        FlightRoute const route = Search(hills, request(hills, 500.0f, 600.0f, 5900.0f, 5800.0f, neutral), params);
        note("rolling hills", route);
        check(route.ok && route.heightChanges <= 1, "rolling hills: the height is held");
        check(route.ok && upThenDown(route), "rolling hills: no up and down");
    }

    // The cheap rules
    check(IsGroundHop(149.9f, params) && !IsGroundHop(150.1f, params), "hop: below 150 yd stays on the ground");
    check(CanFlyBetween(noFly, {500.0f, 3200.0f, 0.0f}, {5900.0f, 3200.0f, 0.0f}) &&
              !CanFlyBetween(noFly, {500.0f, 3200.0f, 0.0f}, {3200.0f, 3200.0f, 0.0f}),
          "CanFlyBetween: open ends only");
    check(flat.Bytes() < 32 * 1024 && flat.openCells == SIDE * SIDE && noFly.closedCells > 0, "map: sizes and counts");
    // Voltarus' lowest point 334 over Zeramas' ground near 239; a building's foot below its hillside; a bridge 20 yd
    // over a river's surface; the edge itself
    check(FloatsOver(334.0f, 239.0f) && !FloatsOver(230.0f, 239.0f) && !FloatsOver(20.0f, 0.0f) &&
              !FloatsOver(FLOATING_MODEL_YARDS, 0.0f) && FloatsOver(FLOATING_MODEL_YARDS + 0.5f, 0.0f),
          "FloatsOver: a floating model only");
    return checks;
}
}  // namespace FlightRules

#endif
