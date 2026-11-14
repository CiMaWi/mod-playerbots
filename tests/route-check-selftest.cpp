/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of `playerbots route check links|pairs` (src/Mgr/Travel/RouteCheckRules.h): the bot's walk on
// engine answers from small made-up worlds, a direction's class by its walk, the verdict of a link from both
// directions, the report keys and lines.
// Usage: route-check-selftest
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "RouteCheckRules.h"

using namespace RouteCheck;

int main()
{
    int failures = 0;
    int checks = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
        {
            ++failures;
            std::cout << "FAIL: " << what << "\n";
        }
    };

    Vec const start{0.0f, 0.0f, 10.0f};
    Vec const end{100.0f, 0.0f, 10.0f};

    // The bot's walk: engine answers from small made-up worlds. A smooth path has a point every 4 yd (pointCount);
    // the trace keeps only its ends
    auto const answer = [](std::uint32_t type, Vec const& at, Vec const& last)
    {
        WalkAnswer one;
        one.result = true;
        one.type = type;
        float const reach = Dist2D(at, last);
        float const back = reach > 4.0f ? 1.0f - 4.0f / reach : 0.0f;  // the smooth step before the last point
        one.beforeLast = {at.x + (last.x - at.x) * back, at.y + (last.y - at.y) * back, at.z + (last.z - at.z) * back};
        one.last = last;
        one.actualEnd = last;
        one.length = Dist3D(at, last);
        one.pointCount = 1 + std::max(1u, static_cast<std::uint32_t>(std::ceil(Dist2D(at, last) / 4.0f)));
        one.points = {at, last};
        return one;
    };
    // Core v20's hop: the target appended to a one-point path, typed INCOMPLETE, actual end = last
    auto const hopAnswer = [](std::uint32_t type, Vec const& at, Vec const& target)
    {
        WalkAnswer one;
        one.result = true;
        one.type = type;
        one.beforeLast = at;
        one.last = target;
        one.actualEnd = target;
        one.length = Dist3D(at, target);
        one.pointCount = 2;
        one.points = {at, target};
        return one;
    };
    auto const half = []() { return 0.5f; };
    // The mesh everywhere: one order along the path to the end
    std::vector<Vec> trace;
    WalkResult walk = Walk(
        start, end, [&](Vec const& at, Vec const& target) { return answer(PATH_NORMAL, at, target); }, half, &trace);
    check(walk.end == WalkEnd::Arrived && walk.moves == 1 && walk.kinds == WALK_MESH && walk.straightYd == 0.0f,
          "walk on the mesh: arrived in 1 move, mesh only");
    check(DirectionOf(walk) == Direction::Ok, "a walk on the mesh is ok");
    check(trace.size() == 2 && trace[0].x == 0.0f && trace[1].x == 100.0f, "the trace: the start, then the path");
    // The areas under the walked path add up and show in the row, only when there are some
    WalkResult const wet = Walk(
        start, end,
        [&](Vec const& at, Vec const& target)
        {
            WalkAnswer one = answer(PATH_NORMAL, at, target);
            one.areas = AREA_FATIGUE | AREA_LAVA;
            return one;
        },
        half);
    check(wet.areas == (AREA_FATIGUE | AREA_LAVA) && walk.areas == 0 &&
              DirectionValue("fwd", wet).find(" areas=fatigue+lava") != std::string::npos &&
              DirectionValue("fwd", walk).find("areas=") == std::string::npos,
          "walk areas: summed over the moves, named only when present");
    // A hole from x 60 on: the path towards the end is INCOMPLETE to x 60, then within 70 yd the order is the end
    // itself and the engine answers the hole with a shortcut: a straight line to the end
    Vec const holeEdge{60.0f, 0.0f, 10.0f};
    auto const hole = [&](Vec const& at, Vec const& target)
    {
        if (target.x <= 60.0f)
            return answer(PATH_NORMAL, at, target);
        if (at.x < 60.0f)
            return answer(PATH_INCOMPLETE, at, holeEdge);
        return answer(PATH_NORMAL | PATH_NOT_USING_PATH, at, target);
    };
    walk = Walk(start, end, hole, half);
    check(walk.end == WalkEnd::Arrived && walk.moves == 2 &&
              walk.kinds == (WALK_MESH | WALK_INCOMPLETE | WALK_STRAIGHT) &&
              std::fabs(walk.straightYd - 40.0f) < 0.01f && std::fabs(walk.meshYd - 60.0f) < 0.01f &&
              walk.straightSeen && walk.firstStraight.x == 60.0f,
          "walk over a hole: arrives by a straight line from the hole's edge");
    check(DirectionOf(walk) == Direction::Shortcut, "arrived with a straight move: shortcut");
    check(DirectionValue("fwd", walk) ==
              "fwd=shortcut moves=2 kinds=mesh+incomplete+straight mesh=60 straight=40 at=100,0,10 from=60,0,10",
          "direction value");
    // NOPATH also moves the bot in a straight line (MoveSplineInit::MoveTo without a path)
    walk = Walk(
        start, Vec{50.0f, 0.0f, 10.0f}, [&](Vec const& at, Vec const&) { return answer(PATH_NOPATH, at, at); }, half);
    check(walk.end == WalkEnd::Arrived && walk.kinds == WALK_STRAIGHT && DirectionOf(walk) == Direction::Shortcut,
          "NOPATH within 70 yd: a straight line, a shortcut");
    // An INCOMPLETE mesh that ends at x 40 short of the end at x 60: the bot stops there and gives up
    auto const shortMesh = [&](Vec const& at, Vec const& target)
    { return target.x <= 40.0f ? answer(PATH_NORMAL, at, target) : answer(PATH_INCOMPLETE, at, Vec{40, 0, 10}); };
    walk = Walk(start, Vec{60.0f, 0.0f, 10.0f}, shortMesh, half);
    check(walk.end == WalkEnd::Stuck && walk.moves == 1 + WALK_STUCK_MOVES && walk.reached.x == 40.0f &&
              walk.kinds == (WALK_MESH | WALK_INCOMPLETE) && DirectionOf(walk) == Direction::Stuck,
          "walk stopped by an incomplete mesh: stuck where the mesh ends");
    // A long link whose end lies in a hole: the path towards it is a shortcut (not accepted), so the bot takes
    // stepping stones of the cone (rand 0.5: straight ahead, 52.5 yd) until within 70 yd, then a straight line
    Vec const far{200.0f, 0.0f, 10.0f};
    auto const farHole = [&](Vec const& at, Vec const& target)
    {
        if (target.x >= 200.0f)
            return answer(PATH_NORMAL | PATH_NOT_USING_PATH, at, target);
        return answer(PATH_NORMAL, at, target);
    };
    walk = Walk(start, far, farHole, half);
    check(
        walk.end == WalkEnd::Arrived && walk.moves == 4 && walk.kinds == (WALK_MESH | WALK_CONE_STONE | WALK_STRAIGHT),
        "long link into a hole: cone stones, then a straight line");
    // Stepping stones are navmesh paths: the far path is refused (NOPATH) but within 70 yd the end has a path
    auto const farStones = [&](Vec const& at, Vec const& target)
    {
        if (target.x >= 200.0f && Dist3D(at, target) >= WALK_ORDER_RANGE)
            return answer(PATH_NOPATH, at, at);
        return answer(PATH_NORMAL, at, target);
    };
    walk = Walk(start, far, farStones, half);
    check(walk.end == WalkEnd::Arrived && (walk.kinds & WALK_CONE_STONE) && !(walk.kinds & WALK_STRAIGHT) &&
              DirectionOf(walk) == Direction::Ok,
          "a walk over cone stones on the mesh is ok");
    // A very long link: every order gains 6 yd, WALK_MAX_MOVES moves end short of it
    Vec const veryFar{3000.0f, 0.0f, 10.0f};
    auto const creep = [&](Vec const& at, Vec const& target)
    {
        if (target.x >= 3000.0f)
            return answer(PATH_INCOMPLETE, at, Vec{at.x + 6.0f, 0.0f, 10.0f});
        return answer(PATH_NORMAL, at, target);
    };
    walk = Walk(start, veryFar, creep, half);
    check(walk.end == WalkEnd::Capped && walk.moves == WALK_MAX_MOVES && DirectionOf(walk) == Direction::Capped,
          "WALK_MAX_MOVES moves without arrival: capped");

    // The hop (G33): the mesh ends at x 40; from its edge core v20 appends the target (INCOMPLETE, two points, the
    // actual end = the last point). The old model took it as a mesh move (ok); it is a straight line: shortcut
    Vec const edge{40.0f, 0.0f, 10.0f};
    Vec const sixtyEnd{60.0f, 0.0f, 10.0f};
    auto const gap = [&](Vec const& at, Vec const& target)
    {
        if (target.x <= 40.0f)
            return answer(PATH_NORMAL, at, target);
        if (at.x < 40.0f)
            return answer(PATH_INCOMPLETE, at, edge);
        return hopAnswer(PATH_INCOMPLETE, at, target);
    };
    walk = Walk(start, sixtyEnd, gap, half);
    check(walk.end == WalkEnd::Arrived && walk.moves == 2 &&
              walk.kinds == (WALK_MESH | WALK_INCOMPLETE | WALK_STRAIGHT | WALK_HOP) &&
              std::fabs(walk.straightYd - 20.0f) < 0.01f && std::fabs(walk.meshYd - 40.0f) < 0.01f &&
              walk.straightSeen && walk.firstStraight.x == 40.0f && DirectionOf(walk) == Direction::Shortcut,
          "the hop over a gap: a straight line from the mesh's edge, shortcut");
    check(DirectionValue("fwd", walk) ==
              "fwd=shortcut moves=2 kinds=mesh+incomplete+straight+hop mesh=40 straight=20 at=60,0,10 from=40,0,10",
          "a hop's direction value names it");
    // The same world on core v21: NOPATH instead of the hop, the same class
    auto const gapV21 = [&](Vec const& at, Vec const& target)
    {
        if (target.x <= 40.0f || at.x < 40.0f)
            return gap(at, target);
        WalkAnswer one = answer(PATH_NOPATH, at, at);
        one.pointCount = 2;
        return one;
    };
    WalkResult const v21Gap = Walk(start, sixtyEnd, gapV21, half);
    check(DirectionOf(v21Gap) == Direction::Shortcut && !(v21Gap.kinds & WALK_HOP) &&
              std::fabs(v21Gap.straightYd - 20.0f) < 0.01f,
          "core v21 answers the gap NOPATH: the same shortcut, no hop");
    // Which answers are hops
    Vec const twenty{20.0f, 0.0f, 10.0f};
    check(IsHop(hopAnswer(PATH_INCOMPLETE, start, twenty)) &&
              IsHop(hopAnswer(PATH_INCOMPLETE | PATH_FARFROMPOLY, start, twenty)),
          "two points, INCOMPLETE (also far from a polygon), 20 yd: a hop");
    check(!IsHop(answer(PATH_INCOMPLETE, start, twenty)), "a smooth INCOMPLETE path of 6 points: no hop");
    check(!IsHop(hopAnswer(PATH_INCOMPLETE, start, Vec{4.2f, 0.0f, 13.0f})),
          "two points within one smooth step (4.2 yd 2D, 3 yd up): no hop");
    check(!IsHop(hopAnswer(PATH_NORMAL, start, twenty)), "a NORMAL two-point path: no hop");
    auto hopOnly = [&](Vec const& at, Vec const& target) { return hopAnswer(PATH_INCOMPLETE, at, target); };
    WalkStep step;
    std::uint32_t const hopKinds = WalkMove(start, twenty, hopOnly, step, nullptr);
    check(hopKinds == (WALK_STRAIGHT | WALK_HOP | WALK_INCOMPLETE) && step.land.x == 20.0f &&
              step.straightYd == 20.0f && step.meshYd == 0.0f && step.straightFrom.x == 0.0f,
          "a hop move of two points: straight, lands on its last point");
    // One rule on both cores (NV19d): core v21 makes the same line without the append, after smooth steps to the
    // start polygon's edge (FindSmoothPath copies the unclamped target in as the last point): x 0, 4, 8, 12, then 100
    auto const jumpAnswer = [](std::uint32_t type, Vec const& at, Vec const& target)
    {
        WalkAnswer one;
        one.result = true;
        one.type = type;
        one.points = {at, Vec{at.x + 4.0f, at.y, at.z}, Vec{at.x + 8.0f, at.y, at.z}, Vec{at.x + 12.0f, at.y, at.z},
                      target};
        one.pointCount = 5;
        one.beforeLast = one.points[3];
        one.last = target;
        one.actualEnd = target;
        one.length = 12.0f + Dist3D(one.beforeLast, target);
        return one;
    };
    Vec const hundred{100.0f, 0.0f, 10.0f};
    check(IsHop(jumpAnswer(PATH_INCOMPLETE, start, hundred)) &&
              IsHop(jumpAnswer(PATH_INCOMPLETE | PATH_FARFROMPOLY, start, hundred)),
          "smooth steps, then a last leg of 88 yd, INCOMPLETE (also far from a polygon): a hop");
    check(!IsHop(jumpAnswer(PATH_NORMAL, start, hundred)), "the same points on a NORMAL path: no hop");
    check(!IsHop(jumpAnswer(PATH_INCOMPLETE, start, Vec{16.2f, 0.0f, 13.0f})),
          "smooth steps, then a last leg of 4.2 yd (2D): no hop");
    auto jumpOnly = [&](Vec const& at, Vec const& target) { return jumpAnswer(PATH_INCOMPLETE, at, target); };
    std::vector<Vec> jumpTrace;
    std::uint32_t const jumpKinds = WalkMove(start, hundred, jumpOnly, step, &jumpTrace);
    check(jumpKinds == (WALK_MESH | WALK_STRAIGHT | WALK_HOP | WALK_INCOMPLETE) && step.land.x == 100.0f &&
              std::fabs(step.meshYd - 12.0f) < 0.01f && std::fabs(step.straightYd - 88.0f) < 0.01f &&
              step.straightFrom.x == 12.0f && jumpTrace.size() == 4 && jumpTrace.back().x == 100.0f,
          "a hop move after smooth steps: mesh to the edge, then straight from it");
    WalkResult const jumpWalk = Walk(start, end, jumpOnly, half);
    check(jumpWalk.end == WalkEnd::Arrived && jumpWalk.moves == 1 && DirectionOf(jumpWalk) == Direction::Shortcut &&
              jumpWalk.firstStraight.x == 12.0f && std::fabs(jumpWalk.meshYd - 12.0f) < 0.01f &&
              std::fabs(jumpWalk.straightYd - 88.0f) < 0.01f,
          "a walk whose one answer ends in that line: shortcut, the line from the mesh's edge");
    check(DirectionValue("fwd", jumpWalk) ==
              "fwd=shortcut moves=1 kinds=mesh+incomplete+straight+hop mesh=12 straight=88 at=100,0,10 from=12,0,10",
          "that walk's direction value");

    // mode=path: the stored points in order. A mesh with a hole between x 80 and 100: the walk node to node goes
    // around it (one mesh order), the stored points lead into it
    std::vector<Vec> const stored = {start, Vec{30, 0, 10}, Vec{60, 0, 10}, Vec{90, 0, 10}, Vec{120, 0, 10}};
    auto const around = [&](Vec const& at, Vec const& target)
    {
        if (target.x > 80.0f && target.x < 100.0f)
            return answer(PATH_NORMAL | PATH_NOT_USING_PATH, at, target);
        return answer(PATH_NORMAL, at, target);
    };
    walk = Walk(start, stored.back(), around, half);
    check(DirectionOf(walk) == Direction::Ok, "node to node around the hole: ok");
    std::vector<Vec> pathTrace;
    walk = WalkPath(stored, around, half, &pathTrace);
    check(walk.end == WalkEnd::Arrived && walk.moves == 4 && (walk.kinds & WALK_STRAIGHT) &&
              walk.firstStraight.x == 60.0f && std::fabs(walk.straightYd - 30.0f) < 0.01f &&
              DirectionOf(walk) == Direction::Shortcut,
          "over the stored points into the hole: a straight move, shortcut");
    check(pathTrace.size() == 5 && pathTrace[3].x == 90.0f, "the path trace: the start, then every point passed");
    std::vector<Vec> const backwards(stored.rbegin(), stored.rend());
    walk = WalkPath(backwards, around, half);
    check(walk.end == WalkEnd::Arrived && walk.moves == 4 && DirectionOf(walk) == Direction::Shortcut &&
              walk.firstStraight.x == 120.0f,
          "the reverse walks the points backwards");
    std::vector<Vec> const onMesh = {start, Vec{30, 0, 10}, Vec{60, 0, 10}, Vec{75, 0, 10}};
    walk = WalkPath(onMesh, around, half);
    check(walk.end == WalkEnd::Arrived && walk.moves == 3 && walk.kinds == WALK_MESH &&
              DirectionOf(walk) == Direction::Ok,
          "stored points on the mesh: ok, one move per point");
    // Points within 10 yd of the bot arrive at once (the follower's goal), as node to node
    std::vector<Vec> const dense = {start,          Vec{4, 0, 10},  Vec{8, 0, 10}, Vec{12, 0, 10},
                                    Vec{16, 0, 10}, Vec{20, 0, 10}, Vec{24, 0, 10}};
    walk = WalkPath(dense, around, half);
    check(walk.end == WalkEnd::Arrived && walk.moves == 2 && DirectionOf(walk) == Direction::Ok,
          "dense points: a point is ordered when over 10 yd away");
    // A stored point past the mesh's end: the leg gets stuck there, so does the walk
    std::vector<Vec> const past = {start, Vec{30, 0, 10}, Vec{60, 0, 10}, Vec{90, 0, 10}};
    walk = WalkPath(past, shortMesh, half);
    check(walk.end == WalkEnd::Stuck && walk.reached.x == 40.0f && DirectionOf(walk) == Direction::Stuck,
          "a stored point past the mesh's end: stuck there");
    WalkResult const twoPoints = WalkPath({start, end}, hole, half);
    WalkResult const nodeWalk = Walk(start, end, hole, half);
    check(twoPoints.moves == nodeWalk.moves && twoPoints.kinds == nodeWalk.kinds && twoPoints.end == nodeWalk.end,
          "a way of two points is the walk node to node");

    // Keys and lines
    check(NodeKey(530, -1234.4f, 5678.5f, -0.4f) == "530:node:-1234,5679,0", "node key rounds to a yard");
    check(NodeKey(0, -0.6f, 0.4f, 12.49f) == "0:node:-1,0,12", "node key rounds negative halves away");
    std::string const fromKey = NodeKey(1, 1, 2, 3);
    std::string const toKey = NodeKey(1, 4, 5, 6);
    check(LinkKey(fromKey, toKey) == "1:node:1,2,3>1:node:4,5,6:walk", "link key");

    // A link's verdict from both directions
    auto const made = [](WalkEnd how, std::uint32_t kinds, Vec const& reached, std::uint32_t moves)
    {
        WalkResult one;
        one.end = how;
        one.kinds = kinds;
        one.reached = reached;
        one.moves = moves;
        return one;
    };
    WalkResult const okWalk = made(WalkEnd::Arrived, WALK_MESH, Vec{99, 0, 10}, 1);
    WalkResult const shortcut = made(WalkEnd::Arrived, WALK_MESH | WALK_STRAIGHT, Vec{100, 0, 10}, 2);
    WalkResult const stuckAtA = made(WalkEnd::Stuck, WALK_MESH, Vec{0.5f, 0, 10}, 1);
    WalkResult const stuckAtB = made(WalkEnd::Stuck, WALK_MESH, Vec{100, 0, 10}, 6);
    WalkResult const stuckMid = made(WalkEnd::Stuck, WALK_MESH, Vec{50, 0, 10}, 6);
    WalkResult const capped = made(WalkEnd::Capped, WALK_MESH, Vec{60, 0, 10}, WALK_MAX_MOVES);
    Vec const nodeA{0, 0, 10};
    Vec const nodeB{100, 0, 10};
    check(ClassifyLink(okWalk, nullptr, nodeA, nodeB, false, false).verdict == Verdict::Ok,
          "ok: the forward walk arrives on the mesh");
    check(ClassifyLink(okWalk, nullptr, nodeA, nodeB, true, true).verdict == Verdict::Ok,
          "ok stays ok with off-mesh ends (the walk arrived)");
    check(ClassifyLink(capped, nullptr, nodeA, nodeB, false, false).verdict == Verdict::Capped,
          "capped forward: capped, the reverse is not walked");
    check(ClassifyLink(stuckMid, &okWalk, nodeA, nodeB, false, false).verdict == Verdict::ReverseOnly,
          "reverse-only: forward stuck, reverse arrives");
    check(ClassifyLink(shortcut, &okWalk, nodeA, nodeB, false, false).verdict == Verdict::ReverseOnly,
          "reverse-only: forward only by a shortcut, reverse on the mesh");
    check(ClassifyLink(stuckMid, &okWalk, nodeA, nodeB, true, true).verdict == Verdict::ReverseOnly,
          "reverse-only wins over off-mesh ends");
    check(ClassifyLink(stuckMid, &capped, nodeA, nodeB, false, false).verdict == Verdict::Capped,
          "a capped reverse: capped, not a fault");
    check(ClassifyLink(shortcut, &stuckMid, nodeA, nodeB, false, false).verdict == Verdict::Shortcut,
          "shortcut: forward arrives only by a straight move, reverse stuck");
    check(ClassifyLink(stuckMid, &shortcut, nodeA, nodeB, false, false).verdict == Verdict::Shortcut,
          "shortcut: reverse arrives only by a straight move");
    check(ClassifyLink(shortcut, &shortcut, nodeA, nodeB, false, false).verdict == Verdict::Shortcut,
          "shortcut: both arrive only by straight moves");
    check(ClassifyLink(stuckAtA, &shortcut, nodeA, nodeB, true, false).verdict == Verdict::Shortcut,
          "an arrival by a straight move wins over an off-mesh end");
    LinkVerdict v = ClassifyLink(stuckAtA, &stuckAtB, nodeA, nodeB, true, false);
    check(v.verdict == Verdict::EndOffMesh && v.cause == Cause::From, "end-off-mesh: the from end is the cause");
    v = ClassifyLink(stuckAtA, &stuckAtB, nodeA, nodeB, false, true);
    check(v.verdict == Verdict::EndOffMesh && v.cause == Cause::To, "end-off-mesh: the to end is the cause");
    v = ClassifyLink(stuckAtA, &stuckAtB, nodeA, nodeB, true, true);
    check(v.verdict == Verdict::EndOffMesh && v.cause == Cause::Both, "end-off-mesh: both ends");
    v = ClassifyLink(stuckAtA, &stuckAtB, nodeA, nodeB, false, false);
    check(v.verdict == Verdict::Dead && v.cause == Cause::None, "dead: both stuck, both ends on the mesh");
    v = ClassifyLink(stuckMid, &stuckMid, nodeA, nodeB, true, true);
    check(v.verdict == Verdict::Dead, "an off-mesh end whose walks stopped over 20 yd away: dead");
    v = ClassifyLink(made(WalkEnd::Stuck, WALK_MESH, Vec{20, 0, 10}, 6), &stuckMid, nodeA, nodeB, true, false);
    check(v.verdict == Verdict::EndOffMesh, "forward stopped exactly 20 yd from the off-mesh end: cause");
    v = ClassifyLink(made(WalkEnd::Stuck, WALK_MESH, Vec{21, 0, 10}, 6), &stuckMid, nodeA, nodeB, true, false);
    check(v.verdict == Verdict::Dead, "forward stopped 21 yd from the off-mesh end: not the cause");
    v = ClassifyLink(stuckMid, &stuckAtB, nodeA, nodeB, false, true);
    check(v.verdict == Verdict::EndOffMesh && v.cause == Cause::To,
          "the reverse walk stopping at the to end blames it (the walk starts there)");
    check(!IsOffMesh(0.0f) && !IsOffMesh(4.0f) && IsOffMesh(4.1f) && IsOffMesh(-1.0f),
          "off the mesh: over 4.0 yd, or none");
    check(std::string(VerdictWord(Verdict::ReverseOnly)) == "reverse-only" &&
              std::string(VerdictWord(Verdict::EndOffMesh)) == "end-off-mesh" &&
              std::string(VerdictWord(Verdict::Shortcut)) == "shortcut",
          "class words");
    // Both walks from one made-up world: the mesh ends at x 40 going forward, the way back is on the mesh
    Vec const sixty{60.0f, 0.0f, 10.0f};
    WalkResult const oneWayFwd = Walk(start, sixty, shortMesh, half);
    WalkResult const oneWayRev = Walk(sixty, start, shortMesh, half);
    check(ClassifyLink(oneWayFwd, &oneWayRev, start, sixty, false, false).verdict == Verdict::ReverseOnly,
          "a one-way mesh, walked both ways: reverse-only");

    // Rows
    WalkResult const broken = made(WalkEnd::Stuck, WALK_MESH | WALK_INCOMPLETE, Vec{12.4f, -3.6f, 7.0f}, 6);
    check(LinkValue(LinkVerdict{Verdict::Dead, Cause::None}, broken, &stuckAtA, true) ==
              "dead fwd=stuck moves=6 kinds=mesh+incomplete mesh=0 straight=0 at=12,-4,7 rev=stuck moves=1 "
              "kinds=mesh mesh=0 straight=0 at=0,0,10 notile",
          "dead row value");
    check(LinkValue(LinkVerdict{Verdict::EndOffMesh, Cause::Both}, broken, &stuckAtA, false).find(" cause=both") !=
              std::string::npos,
          "end-off-mesh row names the cause");
    check(LinkValue(LinkVerdict{Verdict::Capped, Cause::None}, capped, nullptr, false).find(" rev=-") !=
              std::string::npos,
          "rev=- where the reverse was not walked");
    check(
        LinkValue(LinkVerdict{Verdict::Shortcut, Cause::None}, shortcut, &stuckMid, false).rfind("shortcut fwd=", 0) ==
            0,
        "shortcut row value begins with the class word");
    check(OffMeshValue(2.04f, true) == "2.0 steep" && OffMeshValue(-1.0f, false) == "none", "off-mesh row value");

    // Per node
    std::string const thirdKey = NodeKey(1, 7, 8, 9);
    NodeLinkCounts perNode;
    CountNodeLink(perNode, Verdict::ReverseOnly, toKey, fromKey);
    CountNodeLink(perNode, Verdict::Dead, fromKey, toKey);
    CountNodeLink(perNode, Verdict::Dead, fromKey, thirdKey);
    CountNodeLink(perNode, Verdict::Shortcut, toKey, thirdKey);
    CountNodeLink(perNode, Verdict::Capped, fromKey, toKey);
    CountNodeLink(perNode, Verdict::Ok, fromKey, toKey);
    std::vector<std::string> nodeLines;
    AddNodeLinkLines(nodeLines, perNode);
    check(nodeLines.size() == 3 &&
              nodeLines[0] == "3\tnodes.links\t1:node:1,2,3\treverse-only=1 end-off-mesh=0 dead=2 shortcut=0" &&
              nodeLines[1] == "3\tnodes.links\t1:node:4,5,6\treverse-only=1 end-off-mesh=0 dead=1 shortcut=1" &&
              nodeLines[2] == "3\tnodes.links\t1:node:7,8,9\treverse-only=0 end-off-mesh=0 dead=1 shortcut=1",
          "nodes.links: both ends counted, capped and ok not counted, sorted by key");

    // Pairs
    PairSpec pair;
    check(ParsePairLine("1 -100.5 200 30   -90 210 31.5 ship dock A\r", pair) && pair.mapId == 1 &&
              pair.from.x == -100.5f && pair.to.z == 31.5f && pair.label == "ship dock A",
          "pair line with a label");
    check(ParsePairLine("0 1 2 3 4 5 6", pair) && pair.label.empty(), "pair line without a label");
    check(!ParsePairLine("# a comment", pair) && !ParsePairLine("   ", pair) && !ParsePairLine("1 2 3", pair) &&
              !ParsePairLine("x 1 2 3 4 5 6", pair),
          "comment, blank and malformed lines are skipped");
    check(PairKey(fromKey, toKey, "") == "1:node:1,2,3>1:node:4,5,6:pair" &&
              PairKey(fromKey, toKey, "a b") == "1:node:1,2,3>1:node:4,5,6:pair#a b",
          "pair key");
    check(PathValue({Vec{1, 2, 3}, Vec{4.5f, 5, 6}}) == "1.0,2.0,3.0;4.5,5.0,6.0" && PathValue({}).empty(),
          "pair path line");

    // The walk model's words
    CheckOptions options;
    check(options.Name() == "v21-nodes-40" && !options.SteepExcluded(), "default: v21, node to node, steep cost 40");
    auto const named = [](std::vector<std::string> const& words)
    {
        CheckOptions one;
        for (std::string const& word : words)
            if (!IsCheckWord(word) || !TakeCheckWord(word, one))
                return std::string("bad ") + word;
        return one.Name();
    };
    check(named({"model=v20"}) == "v20-nodes-excluded", "model=v20 leaves steep ground out");
    check(named({"steep=excluded"}) == "v21-nodes-excluded" && named({"steep=1", "mode=path"}) == "v21-path-1" &&
              named({"model=v20", "steep=40"}) == "v20-nodes-40" && named({"steep=2.5"}) == "v21-nodes-2.5" &&
              named({"mode=nodes", "model=v21"}) == "v21-nodes-40",
          "steep= and mode= words");
    check(named({"steep=0"}).rfind("bad", 0) == 0 && named({"steep=-1"}).rfind("bad", 0) == 0 &&
              named({"steep=abc"}).rfind("bad", 0) == 0 && named({"steep="}).rfind("bad", 0) == 0 &&
              named({"steep=4x"}).rfind("bad", 0) == 0 && named({"mode=pathx"}).rfind("bad", 0) == 0 &&
              named({"model=v19"}).rfind("bad", 0) == 0,
          "bad values are refused");
    check(named({"areas=all"}) == "v21-nodes-40-areas" && named({"areas=default"}) == "v21-nodes-40" &&
              named({"areas=lava"}).rfind("bad", 0) == 0 && named({"areas="}).rfind("bad", 0) == 0 &&
              IsCheckWord("areas=all") && CheckOptions{}.AreasOption() == "default",
          "areas= word");
    check(AreaBit(0x01) == 0 && AreaBit(0x08) == 0 && AreaBit(0x10) == 0 && AreaBit(0x02) == AREA_LAVA &&
              AreaBit(0x04) == AREA_SLIME && AreaBit(0x20) == AREA_FATIGUE && AreaBit(0x40) == AREA_OTHER &&
              AreasWord(AREA_FATIGUE | AREA_LAVA) == "fatigue+lava" && AreasWord(0) == "-",
          "area bits and words");
    check(!IsCheckWord("all") && !IsCheckWord("571") && !IsCheckWord("C:/x/pairs.txt") && IsCheckWord("model=v20") &&
              IsCheckWord("mode=path"),
          "only the three words are taken");

    // The report
    MapCounts a;
    a.mapId = 571;
    a.links = 6;
    a.byClass[static_cast<int>(Verdict::Ok)] = 1;
    a.byClass[static_cast<int>(Verdict::ReverseOnly)] = 1;
    a.byClass[static_cast<int>(Verdict::EndOffMesh)] = 1;
    a.byClass[static_cast<int>(Verdict::Dead)] = 1;
    a.byClass[static_cast<int>(Verdict::Shortcut)] = 1;
    a.byClass[static_cast<int>(Verdict::Capped)] = 1;
    a.nodes = 2;
    a.offMesh = 2;
    a.offMesh4 = 1;
    MapCounts b;
    b.mapId = 0;
    b.links = 2;
    b.byClass[static_cast<int>(Verdict::Ok)] = 2;
    b.chained = 1;
    b.nodes = 2;
    check(a.Broken() == 4 && b.Broken() == 0,
          "broken = reverse-only + end-off-mesh + dead + shortcut; capped is not broken");
    std::vector<std::string> rows = {Line("nodes.offmesh", NodeKey(571, 1, 1, 1), "1.5"),
                                     Line("links.class", LinkKey(fromKey, toKey), "dead fwd=stuck")};
    std::vector<std::string> const lines = ReportLines({a, b}, rows);
    check(std::is_sorted(lines.begin(), lines.end()), "report sorted");
    auto const has = [&](std::string const& line)
    { return std::find(lines.begin(), lines.end(), line) != lines.end(); };
    check(has("3\tlinks.checked\tall\t8") && has("3\tlinks.checked\t0\t2") && has("3\tlinks.checked\t571\t6"),
          "links.checked per map and in total");
    check(has("3\tlinks.class.ok\tall\t3") && has("3\tlinks.class.capped\t571\t1") &&
              has("3\tlinks.class.reverse-only\tall\t1") && has("3\tlinks.class.end-off-mesh\t571\t1") &&
              has("3\tlinks.class.dead\tall\t1") && has("3\tlinks.class.shortcut\t571\t1") &&
              has("3\tlinks.class.shortcut\t0\t0") && has("3\tlinks.chained\tall\t1"),
          "class counts");
    check(has("3\tnodes.class.offmesh\tall\t2") && has("3\tnodes.class.offmesh4\tall\t1") &&
              has("3\tnodes.checked\tall\t4"),
          "node counts");
    check(has("3\tstatus\t-\tfail"), "status fails with a non-ok link or an off-mesh node");
    check(has(rows[0]) && has(rows[1]), "rows kept");
    MapCounts clean = b;
    clean.offMesh = 3;  // nodes between 1 and 4 yd off the mesh do not fail the status
    std::vector<std::string> const passing = ReportLines({clean}, {});
    check(std::find(passing.begin(), passing.end(), "3\tstatus\t-\tpass") != passing.end(),
          "status passes with only ok links and no node over 4 yd");
    MapCounts onlyShortcut = b;
    onlyShortcut.links = 3;
    onlyShortcut.byClass[static_cast<int>(Verdict::Shortcut)] = 1;
    std::vector<std::string> const failing = ReportLines({onlyShortcut}, {});
    check(std::find(failing.begin(), failing.end(), "3\tstatus\t-\tfail") != failing.end(),
          "a shortcut link fails the status");
    MapCounts withNoPath = b;
    withNoPath.links = 3;
    withNoPath.byClass[static_cast<int>(Verdict::NoPath)] = 1;
    std::vector<std::string> const noPathLines = ReportLines({withNoPath}, {});
    check(
        withNoPath.Broken() == 0 &&
            std::find(noPathLines.begin(), noPathLines.end(), "3\tlinks.class.no-path\tall\t1") != noPathLines.end() &&
            std::find(noPathLines.begin(), noPathLines.end(), "3\tstatus\t-\tpass") != noPathLines.end(),
        "no-path: counted apart, not broken");
    check(std::string(VerdictWord(Verdict::NoPath)) == "no-path", "no-path word");
    check(has("3\tlinks.class.no-path\tall\t0"), "every report counts no-path");
    a.Add(b);
    check(a.links == 8 && a.byClass[static_cast<int>(Verdict::Ok)] == 3, "counts add");
    std::string const summary = SummaryLine("map 571", a);
    check(summary.find("8 links: 3 ok (1 chained), 1 reverse-only, 1 end-off-mesh, 1 dead, 1 shortcut, 1 capped, "
                       "0 no-path") != std::string::npos,
          "summary line");
    std::cout << "  " << summary << "\n";

    std::cout << "route-check-selftest: " << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
