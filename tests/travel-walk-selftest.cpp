/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of a walk leg along graph links (src/Ai/World/Rpg/TravelWalkRules.h, WT58): the orders of a leg
// over three links in both directions and over a link without stored points, the fallback at a stored point off the
// mesh or without progress, the refusal on "no path", and the hop count; and the navmesh probe of a walk off the graph
// (WT56): the path types it accepts, the chain of partial paths, its orders.
// Usage: travel-walk-selftest
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "TravelWalkRules.h"

using namespace TravelWalk;

namespace
{
struct P
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct L
{
    std::uint32_t from{0};
    std::uint32_t to{0};
    bool walkable{true};
    std::uint32_t pathFirst{0};
    std::uint32_t pathCount{0};
};

// A made-up graph: nodes, links sorted by from then to, their stored points
struct Graph
{
    std::vector<P> nodes;
    std::vector<L> links;
    std::vector<P> points;

    void Link(std::uint32_t from, std::uint32_t to, std::vector<P> const& path, bool walkable = true)
    {
        L link{from, to, walkable, static_cast<std::uint32_t>(points.size()), static_cast<std::uint32_t>(path.size())};
        points.insert(points.end(), path.begin(), path.end());
        links.push_back(link);
        std::stable_sort(links.begin(), links.end(),
                         [](L const& a, L const& b) { return a.from != b.from ? a.from < b.from : a.to < b.to; });
    }

    std::span<L const> LinksOf(std::uint32_t node) const
    {
        auto const first = std::find_if(links.begin(), links.end(), [node](L const& l) { return l.from == node; });
        auto const last = std::find_if(first, links.end(), [node](L const& l) { return l.from != node; });
        return {links.data() + (first - links.begin()), static_cast<std::size_t>(last - first)};
    }

    std::span<P const> PathOf(L const& link) const { return {points.data() + link.pathFirst, link.pathCount}; }
};

std::vector<P> Line(float x0, float x1, float y, int count)
{
    std::vector<P> line;
    for (int i = 0; i < count; ++i)
        line.push_back({x0 + (x1 - x0) * static_cast<float>(i) / static_cast<float>(count - 1), y, 0.0f});
    return line;
}

EngineAnswer Answer(std::uint32_t type, float lastStep = 3.0f, float gain = 10.0f, bool result = true)
{
    EngineAnswer answer;
    answer.result = result;
    answer.type = type;
    answer.lastStep = lastStep;
    answer.gain = gain;
    return answer;
}

constexpr std::uint32_t NORMAL = 0x01;
constexpr std::uint32_t INCOMPLETE = 0x04;

// One link walked by a bot that reaches every order (the engine answers NORMAL): the x of every order, the far node's
// as the last. Every order is one of the link's stored points or its far node
struct Walked
{
    std::vector<float> xs;
    bool onlyLinkPoints{true};
    bool inWindow{true};  // every stored point ordered lies within LOOKAHEAD_YARDS of the bot
    LinkWalk walk;
};

Walked WalkLink(Graph const& graph, std::uint32_t from, std::uint32_t to)
{
    Walked walked;
    Rows<L> const rows = PickRows<L>([&graph](std::uint32_t node) { return graph.LinksOf(node); }, from, to,
                                     [](L const& link) { return link.walkable; });
    std::span<P const> const points = rows.link ? graph.PathOf(*rows.link) : std::span<P const>{};
    Begin(walked.walk, from, to, rows.link ? 0u : NO_LINK, points, rows.reversed, graph.nodes[from], graph.nodes[to]);
    P bot = graph.nodes[from];
    for (int moves = 0; moves < 100; ++moves)
    {
        std::uint32_t const point = Target(walked.walk, points, bot, graph.nodes[to]);
        P const order = point == NO_POINT ? graph.nodes[to] : PointAt(points, rows.reversed, point);
        bool const ofLink = point == NO_POINT || std::any_of(points.begin(), points.end(), [&order](P const& p)
                                                             { return p.x == order.x && p.y == order.y; });
        walked.onlyLinkPoints = walked.onlyLinkPoints && ofLink;
        walked.inWindow = walked.inWindow && (point == NO_POINT || Distance(bot, order) <= LOOKAHEAD_YARDS);
        walked.xs.push_back(order.x);
        if (Judge(walked.walk, Answer(NORMAL), true) == Verdict::Refuse)
            break;
        bot = order;
        if (point == NO_POINT)
            break;
    }
    return walked;
}
}  // namespace

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
    auto const same = [](std::vector<float> const& a, std::vector<float> const& b) { return a == b; };

    // A -> B -> C -> D: A->B 6 stored points (no rows back), B->C none either way, C->D 9 points and D->C its own 4.
    // A ship's link A->B (not walkable) with its own points comes first among A's links to B
    Graph graph;
    graph.nodes = {{0.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 0.0f}, {100.0f, 60.0f, 0.0f}, {200.0f, 60.0f, 0.0f}};
    graph.Link(0, 1, {{0.0f, 50.0f, 0.0f}, {50.0f, 50.0f, 0.0f}, {100.0f, 50.0f, 0.0f}}, false);
    graph.Link(0, 1, Line(0.0f, 100.0f, 0.0f, 6));
    graph.Link(1, 2, {});
    graph.Link(2, 1, {});
    graph.Link(2, 3, Line(100.0f, 200.0f, 60.0f, 9));
    graph.Link(3, 2, {{190.0f, 60.0f, 0.0f}, {160.0f, 60.0f, 0.0f}, {130.0f, 60.0f, 0.0f}, {105.0f, 60.0f, 0.0f}});
    graph.Link(1, 0, {});

    // Which rows: the traversed link's own, the opposite's reversed, none
    auto const linksOf = [&graph](std::uint32_t node) { return graph.LinksOf(node); };
    auto const walkable = [](L const& link) { return link.walkable; };
    Rows<L> const ab = PickRows<L>(linksOf, 0, 1, walkable);
    check(ab.link && ab.link->walkable && ab.link->pathCount == 6 && !ab.reversed,
          "rows: A->B uses its own walk link's 6 points, not the ship's");
    Rows<L> const ba = PickRows<L>(linksOf, 1, 0, walkable);
    check(ba.link && ba.link->from == 0 && ba.link->pathCount == 6 && ba.reversed,
          "rows: B->A without rows uses A->B's points reversed");
    Rows<L> const bc = PickRows<L>(linksOf, 1, 2, walkable);
    check(!bc.link, "rows: B->C without rows either way walks its nodes");
    Rows<L> const dc = PickRows<L>(linksOf, 3, 2, walkable);
    check(dc.link && dc.link->pathCount == 4 && !dc.reversed, "rows: D->C uses its own rows");

    // The leg A -> D: each link's thinned points in order, each order the furthest within LOOKAHEAD_YARDS of the bot,
    // the far node last; B->C by its nodes
    Walked const w1 = WalkLink(graph, 0, 1);
    Walked const w2 = WalkLink(graph, 1, 2);
    Walked const w3 = WalkLink(graph, 2, 3);
    check(same(w1.xs, {80.0f, 100.0f}) && w1.walk.mode == Mode::Stored, "leg A->D: A->B orders 80 (40 passed), B");
    check(same(w2.xs, {100.0f}) && w2.xs.size() == 1 && w2.walk.mode == Mode::Nodes,
          "leg A->D: B->C without rows orders its far node only");
    check(same(w3.xs, {175.0f, 200.0f}) && w3.walk.mode == Mode::Stored,
          "leg A->D: C->D orders 175 (the furthest of 125, 150, 175 within reach), D");
    check(w1.onlyLinkPoints && w2.onlyLinkPoints && w3.onlyLinkPoints,
          "leg A->D: every order is a stored point of its link or its far node");
    check(w1.walk.moves == 2 && w3.walk.moves == 2 && w1.walk.hops == 0, "leg A->D: one move per order, no hops");
    check(w1.inWindow && w3.inWindow, "leg A->D: every stored order lies within LOOKAHEAD_YARDS of the bot");
    std::vector<std::uint32_t> const thin = Thinned(graph.PathOf(*ab.link), false, graph.nodes[0], graph.nodes[1]);
    bool spaced = !thin.empty();
    P previous = graph.nodes[0];
    for (std::uint32_t const index : thin)
    {
        P const& at = PointAt(graph.PathOf(*ab.link), false, index);
        spaced = spaced && Distance(previous, at) >= SPACING_YARDS;
        previous = at;
    }
    check(spaced && Distance(previous, graph.nodes[1]) >= SPACING_YARDS / 2.0f,
          "thinned: orders at least SPACING_YARDS apart, the last clear of the far node");

    // The leg D -> A: D->C by its own rows, C->B by its nodes, B->A by A->B's points reversed; no node repeats
    Walked const r1 = WalkLink(graph, 3, 2);
    Walked const r2 = WalkLink(graph, 2, 1);
    Walked const r3 = WalkLink(graph, 1, 0);
    check(same(r1.xs, {130.0f, 100.0f}), "leg D->A: D->C orders its own 130 (160 passed), C");
    check(same(r2.xs, {100.0f}) && r2.walk.mode == Mode::Nodes, "leg D->A: C->B orders B");
    check(same(r3.xs, {20.0f, 0.0f}) && r3.walk.reversed, "leg D->A: B->A orders A->B's 20 (60 passed) reversed, A");
    check(r1.onlyLinkPoints && r3.onlyLinkPoints, "leg D->A: every order is a stored point of its link or its node");

    // A stored point off the mesh: the earlier points stay, the rest of the link goes to its far node, and the next
    // link is back on stored points
    {
        std::span<P const> const points = graph.PathOf(graph.links[5]);  // C->D, sorted after A's and B's links
        check(points.size() == 9 && graph.links[5].from == 2 && graph.links[5].to == 3, "off mesh: the fixture's C->D");
        LinkWalk walk;
        Begin(walk, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        P const first = graph.nodes[2];
        std::uint32_t const point = Target(walk, points, first, graph.nodes[3]);
        check(point != NO_POINT && PointAt(points, false, point).x == 175.0f,
              "off mesh: the order is the furthest point within reach, 175");
        check(Judge(walk, Answer(NORMAL | 0x10, 25.0f), true) == Verdict::Fallback && walk.mode == Mode::Fallback,
              "off mesh: a shortcut answer (no polygon at the point) falls back");
        check(Target(walk, points, first, graph.nodes[3]) == NO_POINT, "off mesh: the fallback heads for the far node");
        check(Judge(walk, Answer(NORMAL), false) == Verdict::Walk && walk.moves == 1,
              "off mesh: the fallback walks node to node, counted");
        check(std::string(RowWord(walk)) == "fallback", "off mesh: the row says fallback");
        LinkWalk noPath;
        Begin(noPath, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        check(Judge(noPath, Answer(0x08, 0.0f, 0.0f), true) == Verdict::Fallback,
              "off mesh: no path to a stored point falls back");
        Begin(walk, 3, 2, 6, graph.PathOf(graph.links[6]), false, graph.nodes[3], graph.nodes[2]);
        check(walk.mode == Mode::Stored && walk.moves == 0 && walk.hops == 0,
              "off mesh: the next link starts on stored points, counts anew");

        // No path at the fallback: refused, never walked as a straight line
        check(Judge(noPath, Answer(0x08, 0.0f, 0.0f), true) == Verdict::Refuse && noPath.mode == Mode::Refused,
              "no path: no path at the fallback refuses the link");
        check(std::string(RowWord(noPath)) == "refused", "no path: the row says refused");
        LinkWalk nodes;
        Begin(nodes, 1, 2, NO_LINK, std::span<P const>{}, false, graph.nodes[1], graph.nodes[2]);
        check(Judge(nodes, Answer(0x08, 0.0f, 0.0f, true), false) == Verdict::Walk && nodes.moves == 0,
              "no path: far from the node the stepping stones are tried as before, no move counted");
        check(Judge(nodes, Answer(0, 0.0f, 0.0f, false), true) == Verdict::Refuse,
              "no path: a failed path calculation near the node refuses a link without rows");
        // Every answer of "no path" gets Fallback or Refuse, near the order or far in stored points
        bool never = true;
        for (Mode const mode : {Mode::Stored, Mode::Nodes, Mode::Fallback})
        {
            LinkWalk one;
            one.mode = mode;
            one.from = 1;
            one.to = 2;
            Verdict const v = Judge(one, Answer(0x08, 40.0f, 0.0f), true);
            never = never && (v == Verdict::Fallback || v == Verdict::Refuse);
        }
        check(never, "no path: no mode walks a near no-path answer");
    }

    // The hop: a partial path whose last step is longer than a walking step is walked and counted, once per answer
    {
        LinkWalk walk;
        Begin(walk, 1, 2, NO_LINK, std::span<P const>{}, false, graph.nodes[1], graph.nodes[2]);
        check(Judge(walk, Answer(INCOMPLETE, 12.0f), true) == Verdict::Hop && walk.hops == 1 && walk.moves == 1,
              "hop: a 12 yd last step is a hop");
        check(Judge(walk, Answer(NORMAL, 3.0f), true) == Verdict::Walk && walk.hops == 1 && walk.moves == 2,
              "hop: a 3 yd last step is a walk");
    }

    // No progress: an order that gains less than PROGRESS_YARDS, or the same point ordered again, is a strike; the
    // third falls back
    {
        std::span<P const> const points = graph.PathOf(graph.links[5]);
        LinkWalk walk;
        Begin(walk, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        Target(walk, points, graph.nodes[2], graph.nodes[3]);
        Verdict const a = Judge(walk, Answer(NORMAL, 3.0f, 0.5f), true);
        Verdict const b = Judge(walk, Answer(NORMAL, 3.0f, 0.5f), true);
        Verdict const c = Judge(walk, Answer(NORMAL, 3.0f, 0.5f), true);
        check(a == Verdict::Walk && b == Verdict::Walk && c == Verdict::Fallback && walk.mode == Mode::Fallback,
              "no progress: three orders without gain fall back");
        LinkWalk again;
        Begin(again, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        Verdict const first = Judge(again, Answer(NORMAL), true);
        Verdict const second = Judge(again, Answer(NORMAL), true);
        Verdict const third = Judge(again, Answer(NORMAL), true);
        Verdict const fourth = Judge(again, Answer(NORMAL), true);
        check(
            first == Verdict::Walk && second == Verdict::Walk && third == Verdict::Walk && fourth == Verdict::Fallback,
            "no progress: the same point ordered three times again falls back");
        // A reached point resets the strikes
        LinkWalk reset;
        Begin(reset, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        std::uint32_t const at = Target(reset, points, graph.nodes[2], graph.nodes[3]);
        Judge(reset, Answer(NORMAL, 3.0f, 0.5f), true);
        Judge(reset, Answer(NORMAL, 3.0f, 0.5f), true);
        Target(reset, points, PointAt(points, false, at), graph.nodes[3]);
        check(reset.strikes == 0 && Judge(reset, Answer(NORMAL, 3.0f, 0.5f), true) == Verdict::Walk,
              "no progress: reaching a point clears the strikes");
        // A bot within LOOKAHEAD_YARDS of the far node heads for it
        LinkWalk skip;
        Begin(skip, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        check(Target(skip, points, P{150.0f, 60.0f, 0.0f}, graph.nodes[3]) == NO_POINT,
              "window: a bot 50 yd from the far node heads for it");
        // Short of the window the order stays: a bot 95 yd before the first point keeps it
        LinkWalk stay;
        Begin(stay, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        std::uint32_t const kept = Target(stay, points, P{30.0f, 60.0f, 0.0f}, graph.nodes[3]);
        check(kept != NO_POINT && PointAt(points, false, kept).x == 125.0f,
              "window: a bot 95 yd before the first point orders it");
    }

    // The row words and the inactive walk
    {
        LinkWalk none;
        check(!none.Active() && std::string(RowWord(none)) == "none", "words: an inactive walk");
        LinkWalk stored;
        Begin(stored, 2, 3, 5, graph.PathOf(graph.links[5]), false, graph.nodes[2], graph.nodes[3]);
        check(stored.Active() && stored.On(2, 3) && !stored.On(3, 2) && std::string(RowWord(stored)) == "stored",
              "words: stored, and the link's ends");
        LinkWalk nodes;
        Begin(nodes, 1, 2, NO_LINK, std::span<P const>{}, false, graph.nodes[1], graph.nodes[2]);
        check(std::string(RowWord(nodes)) == "nodes", "words: nodes");
        // A link whose points all lie near its ends orders its far node at once, still by stored points
        LinkWalk shortLink;
        std::vector<P> const nearEnds{{1.0f, 0.0f, 0.0f}, {99.0f, 0.0f, 0.0f}};
        std::span<P const> const nearSpan(nearEnds);
        Begin(shortLink, 0, 1, 9, nearSpan, false, P{0.0f, 0.0f, 0.0f}, P{100.0f, 0.0f, 0.0f});
        check(shortLink.mode == Mode::Stored &&
                  Target(shortLink, nearSpan, P{0.0f, 0.0f, 0.0f}, P{100.0f, 0.0f, 0.0f}) == NO_POINT,
              "short: a link whose points lie near its ends orders its far node, by stored points");
    }

    // WT56: a walk off the graph probed on the navmesh before the bot starts it (ProbePiece), on made-up engine
    // answers. A line of corner points every `every` yd from `from` towards `to`, ending `upTo` yd along it (or at
    // `to`)
    auto const line = [](ProbePoint const& from, ProbePoint const& to, float every, float upTo)
    {
        std::vector<ProbePoint> points{from};
        float const length = Distance(from, to);
        float const end = std::min(upTo, length);
        for (float along = every; along < end; along += every)
            points.push_back({from.x + (to.x - from.x) * along / length, from.y + (to.y - from.y) * along / length,
                              from.z + (to.z - from.z) * along / length});
        points.push_back({from.x + (to.x - from.x) * end / length, from.y + (to.y - from.y) * end / length,
                          from.z + (to.z - from.z) * end / length});
        return points;
    };
    auto const answer = [](std::uint32_t type, std::vector<ProbePoint> points, bool result = true)
    {
        ProbeAnswer out;
        out.result = result;
        out.type = type;
        out.points = std::move(points);
        return out;
    };
    P const origin{0.0f, 0.0f, 0.0f};
    {
        // A real path to the goal in one step: its corners thinned 25 yd apart, none beside the goal
        P const goal{300.0f, 0.0f, 0.0f};
        PieceWalk const piece = ProbePiece(origin, goal, [&](ProbePoint const& from, ProbePoint const& to)
                                           { return answer(PATH_NORMAL, line(from, to, 20.0f, 1000.0f)); });
        std::vector<float> xs;
        for (ProbePoint const& point : piece.orders)
            xs.push_back(point.x);
        check(piece.probe == Probe::Walk && !piece.Refused() && piece.steps == 1 &&
                  same(xs, {40.0f, 80.0f, 120.0f, 160.0f, 200.0f, 240.0f, 280.0f}),
              "probe: a real path to the goal orders its corners 25 yd apart (none beside the goal)");
        check(std::string(ProbeWord(piece.probe)) == "walk", "probe: the word walk");
        // The order: the furthest point within the lookahead past the one reached, then the goal
        PieceWalk ordered = piece;
        std::uint32_t const first = PieceTarget(ordered, P{0.0f, 0.0f, 0.0f}, goal);
        std::uint32_t const later = PieceTarget(ordered, P{75.0f, 0.0f, 0.0f}, goal);
        std::uint32_t const last = PieceTarget(ordered, P{250.0f, 0.0f, 0.0f}, goal);
        check(first != NO_POINT && ordered.orders[first].x == 80.0f && later != NO_POINT &&
                  ordered.orders[later].x == 160.0f && last == NO_POINT,
              "probe: orders 80, then 160 from 75, then the goal itself from 250");
        // A long path keeps at most PROBE_ORDERS orders
        PieceWalk const longWay =
            ProbePiece(origin, P{3000.0f, 0.0f, 0.0f}, [&](ProbePoint const& f, ProbePoint const& t)
                       { return answer(PATH_NORMAL, line(f, t, 30.0f, 5000.0f)); });
        check(longWay.probe == Probe::Walk && longWay.orders.size() == PROBE_ORDERS,
              "probe: a 3000 yd path keeps PROBE_ORDERS orders");
    }
    {
        // The engine's straight lines and broken answers are refused at the first step, by the path's type flags
        P const goal{60.0f, 0.0f, 0.0f};
        bool allRefused = true;
        for (std::uint32_t const type :
             {PATH_NOPATH, PATH_NORMAL | PATH_NOPATH, PATH_NORMAL | PATH_SHORT, PATH_NORMAL | PATH_NOT_USING_PATH,
              PATH_INCOMPLETE | PATH_NOT_USING_PATH, std::uint32_t{0}})
        {
            PieceWalk const piece = ProbePiece(
                origin, goal, [&](ProbePoint const& from, ProbePoint const& to) { return answer(type, {from, to}); });
            allRefused = allRefused && piece.Refused() && piece.probe == Probe::PathType && piece.steps == 1 &&
                         piece.orders.empty();
        }
        check(allRefused, "probe: no path, a shortcut, a too long path and a blank answer are refused");
        PieceWalk const failed = ProbePiece(origin, goal, [&](ProbePoint const& from, ProbePoint const& to)
                                            { return answer(PATH_NORMAL, {from, to}, false); });
        PieceWalk const empty =
            ProbePiece(origin, goal, [&](ProbePoint const&, ProbePoint const&) { return answer(PATH_NORMAL, {}); });
        check(failed.probe == Probe::PathType && empty.probe == Probe::PathType &&
                  std::string(ProbeWord(failed.probe)) == "path_type",
              "probe: a failed path call and one without points are refused");
    }
    {
        // A partial path chained to the goal: each step from the last one's end; within REACH_YARDS is there, also
        // when the goal lies more than 7 yd off the mesh
        P const goal{350.0f, 0.0f, 0.0f};
        auto const partial = [&](std::uint32_t type)
        {
            return [&, type](ProbePoint const& from, ProbePoint const& to)
            { return answer(type, line(from, to, 20.0f, std::min(100.0f, Distance(from, to) - 8.0f))); };
        };
        PieceWalk const chained = ProbePiece(origin, goal, partial(PATH_INCOMPLETE));
        PieceWalk const offMesh = ProbePiece(origin, goal, partial(PATH_INCOMPLETE | PATH_FARFROMPOLY_END));
        check(chained.probe == Probe::Walk && chained.steps == 4 && !chained.orders.empty() &&
                  chained.orders.back().x < 350.0f - SPACING_YARDS / 2.0f,
              "probe: a partial path chained to within 10 yd of the goal in four steps");
        check(offMesh.probe == Probe::Walk, "probe: a goal more than 7 yd off the mesh, reached within 10 yd");
        // A partial path that ends at the same spot from its own end (a mesa top on its own mesh) stalls
        PieceWalk const stalled =
            ProbePiece(origin, goal, [&](ProbePoint const& from, ProbePoint const&)
                       { return answer(PATH_INCOMPLETE, {from, ProbePoint{120.0f, 0.0f, 0.0f}}); });
        PieceWalk const crawl = ProbePiece(origin, goal, [&](ProbePoint const& from, ProbePoint const&)
                                           { return answer(PATH_INCOMPLETE, {from, {from.x + 1.5f, 0.0f, 0.0f}}); });
        check(stalled.probe == Probe::Stall && stalled.steps == 2 && stalled.orders.empty() &&
                  crawl.probe == Probe::Stall && crawl.steps == 1 && std::string(ProbeWord(stalled.probe)) == "stall",
              "probe: a partial path that gains under 2 yd on the goal stalls");
        // A chain still short of the goal after PROBE_STEPS steps
        PieceWalk const endless =
            ProbePiece(origin, P{1000.0f, 0.0f, 0.0f}, [&](ProbePoint const& from, ProbePoint const&)
                       { return answer(PATH_INCOMPLETE, {from, {from.x + 10.0f, 0.0f, 0.0f}}); });
        check(endless.probe == Probe::Steps && endless.steps == PROBE_STEPS && endless.orders.empty() &&
                  std::string(ProbeWord(endless.probe)) == "steps",
              "probe: a chain short of the goal after PROBE_STEPS steps is refused");
        // A piece for a leg and waypoint
        PieceWalk keyed = chained;
        keyed.leg = 2;
        keyed.next = 3;
        PieceWalk none;
        check(keyed.For(2, 3) && !keyed.For(2, 4) && !none.For(TravelWalk::NO_POINT, TravelWalk::NO_POINT) &&
                  !none.Refused() && std::string(ProbeWord(none.probe)) == "none",
              "probe: a piece belongs to its leg and waypoint; none is no refusal");
    }

    std::cout << "travel-walk-selftest: " << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
