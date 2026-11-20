/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the moves the navmesh did not carry (src/Ai/World/Rpg/MeshMoveRules.h, WT71): every PathType
// bit combination to its class, the per-kind counters, the flush (only kinds with a miss, then a fresh start, once per
// interval), and "nothing counted twice" (a walk link's hop is counted in its hops, not here).
// Usage: mesh-move-selftest
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "MeshMoveRules.h"

using namespace MeshMove;
using namespace TravelWalk;

namespace
{
int checks = 0;
int failures = 0;

void check(bool ok, char const* what)
{
    ++checks;
    if (!ok)
    {
        ++failures;
        std::cout << "FAIL: " << what << "\n";
    }
}

struct P
{
    float x;
    float y;
    float z;
};

struct Row
{
    Kind kind;
    std::uint32_t missed;
    std::uint32_t ordered;
    Spot spot;
};

std::size_t Idx(Kind kind) { return static_cast<std::size_t>(kind); }
}  // namespace

int main()
{
    // The classes
    check(Classify(PATH_NORMAL, 0.0f) == Class::Carried, "NORMAL is carried");
    check(Classify(PATH_INCOMPLETE, 2.0f) == Class::Carried, "INCOMPLETE is carried");
    check(Classify(PATH_NORMAL | PATH_INCOMPLETE, HOP_STEP_YARDS) == Class::Carried,
          "a last step of the limit is none");
    check(Classify(PATH_NORMAL, HOP_STEP_YARDS + 0.1f) == Class::Hop, "NORMAL ending in a long step is a hop");
    check(Classify(PATH_INCOMPLETE, 40.0f) == Class::Hop, "INCOMPLETE ending in a long step is a hop");
    check(Classify(PATH_NOPATH, 0.0f) == Class::NoPath, "NOPATH");
    check(Classify(PATH_NOPATH | PATH_NORMAL, 0.0f) == Class::NoPath, "NOPATH wins over NORMAL");
    check(Classify(PATH_NOT_USING_PATH, 30.0f) == Class::NotUsingPath, "NOT_USING_PATH");
    check(Classify(PATH_SHORT | PATH_NORMAL, 0.0f) == Class::Short, "SHORT");
    check(Classify(PATH_SHORTCUT, 50.0f) == Class::Shortcut, "SHORTCUT");
    check(Classify(PATH_NORMAL | PATH_FARFROMPOLY_END, 0.0f) == Class::FarFromPoly, "NORMAL with an end far from poly");
    check(Classify(PATH_INCOMPLETE | PATH_FARFROMPOLY_START | PATH_FARFROMPOLY_END, 0.0f) == Class::FarFromPoly,
          "FARFROMPOLY start and end");
    check(Classify(0, 0.0f) == Class::NoPath, "no bits is no answer");
    check(Classify(PATH_FARFROMPOLY_END, 0.0f) == Class::FarFromPoly, "FARFROMPOLY_END alone");

    std::vector<P> const flat{{0, 0, 0}, {10, 0, 0}, {12, 0, 0}};
    std::vector<P> const hop{{0, 0, 0}, {10, 0, 0}, {40, 0, 0}};
    check(LastStep(flat) == 2.0f && LastStep(hop) == 30.0f && LastStep(std::vector<P>{{1, 1, 1}}) == 0.0f &&
              LastStep(std::vector<P>{}) == 0.0f,
          "the last step of a path");
    check(std::string(KindWord(Kind::TravelWalk)) == "travel_walk" &&
              std::string(KindWord(Kind::Boarding)) == "boarding" && std::string(KindWord(Kind::Deck)) == "deck" &&
              std::string(KindWord(Kind::Follow)) == "follow" && std::string(KindWord(Kind::Combat)) == "combat" &&
              std::string(KindWord(Kind::Other)) == "other",
          "kind words");

    // The counters
    Counters counters;
    check(!FlushDue(counters, 1000), "the first call only starts the interval");
    check(!Note(counters, Kind::Follow, Class::Carried, false), "a carried move is no miss");
    check(Note(counters, Kind::Follow, Class::Shortcut, false), "a shortcut is a miss");
    check(Note(counters, Kind::Follow, Class::NoPath, false), "no path is a miss");
    check(Note(counters, Kind::Other, Class::Short, false), "a short path is a miss");
    check(counters.ordered[Idx(Kind::Follow)] == 3 && counters.offmesh[Idx(Kind::Follow)] == 2 &&
              counters.ordered[Idx(Kind::Other)] == 1 && counters.offmesh[Idx(Kind::Other)] == 1 &&
              counters.ordered[Idx(Kind::Combat)] == 0,
          "the fold per kind");

    // Nothing counted twice: a hop the walk link counted is a move, not a miss
    check(!Note(counters, Kind::TravelWalk, Class::Hop, true) &&
              !Note(counters, Kind::TravelWalk, Class::Short, true) && counters.ordered[Idx(Kind::TravelWalk)] == 2 &&
              counters.offmesh[Idx(Kind::TravelWalk)] == 0,
          "a move whose class is counted elsewhere adds to ordered only");
    check(Note(counters, Kind::TravelWalk, Class::NoPath, false) && counters.offmesh[Idx(Kind::TravelWalk)] == 1,
          "a straight line at the far node is not in the link's hops: counted here");

    // WT64g: the roam's and the approach's judge is the class Carried; the old accept mask's FARFROMPOLY and a hop are
    // not, so a sample with either is dropped
    check(Classify(TravelWalk::PATH_NORMAL | TravelWalk::PATH_FARFROMPOLY_END, 3.0f) == Class::FarFromPoly &&
              Classify(TravelWalk::PATH_INCOMPLETE | PATH_FARFROMPOLY_START, 3.0f) == Class::FarFromPoly,
          "roam mask: an end or a start far from a polygon is not carried");
    check(Classify(TravelWalk::PATH_INCOMPLETE, 3.0f) == Class::Carried &&
              Classify(TravelWalk::PATH_INCOMPLETE, TravelWalk::HOP_STEP_YARDS + 1.0f) == Class::Hop,
          "roam mask: an incomplete path is carried by its class (its end is judged by the reach), a hop is not");

    // The flush
    counters.last[Idx(Kind::Follow)] = {1, 12, 34, 5.0f, 6.0f};
    check(!FlushDue(counters, 1000 + MISS_INTERVAL_MS - 1), "not due within the interval");
    check(FlushDue(counters, 1000 + MISS_INTERVAL_MS), "due after the interval");
    std::vector<Row> rows;
    auto const collect = [&](Kind kind, std::uint32_t missed, std::uint32_t ordered, Spot const& spot)
    { rows.push_back({kind, missed, ordered, spot}); };
    std::size_t const written = Flush(counters, 1000 + MISS_INTERVAL_MS, collect);
    check(written == 3 && rows.size() == 3, "one row per kind with a miss (TravelWalk, Follow, Other)");
    check(rows[0].kind == Kind::TravelWalk && rows[0].missed == 1 && rows[0].ordered == 3 &&
              rows[1].kind == Kind::Follow && rows[1].missed == 2 && rows[1].ordered == 3 && rows[1].spot.map == 1 &&
              rows[1].spot.zone == 12 && rows[1].spot.area == 34 && rows[1].spot.x == 5.0f && rows[1].spot.y == 6.0f &&
              rows[2].kind == Kind::Other && rows[2].missed == 1 && rows[2].ordered == 1,
          "the rows' missed, ordered and place");
    bool fresh = true;
    for (std::size_t k = 0; k < KINDS; ++k)
        fresh = fresh && counters.ordered[k] == 0 && counters.offmesh[k] == 0;
    check(fresh && !FlushDue(counters, 1000 + MISS_INTERVAL_MS + 1), "the flush resets every counter and the interval");

    // A kind with moves and no miss writes no row
    Note(counters, Kind::Combat, Class::Carried, false);
    Note(counters, Kind::Combat, Class::Carried, false);
    rows.clear();
    std::size_t const none = Flush(counters, 2 * MISS_INTERVAL_MS + 1000, collect);
    check(none == 0 && rows.empty() && counters.ordered[Idx(Kind::Combat)] == 0,
          "a kind with no miss writes no row and starts again");

    std::cout << "mesh-move-selftest: " << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
