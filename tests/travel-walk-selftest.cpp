/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of a walk leg along graph links (src/Ai/World/Rpg/TravelWalkRules.h, WT58): the orders of a leg
// over three links in both directions and over a link without stored points, the fallback at a stored point off the
// mesh or without progress, the refusal on "no path" near the far node or far (WT64g: no stepping stone), a refused
// link that takes no other move, an accepted order that moved nothing (StillOrder), and the hop count; and the navmesh
// probe of a walk off the graph (WT56): the answers it accepts, the chain of partial paths, its orders. WT64i: over
// generated answers (type bits x last step x end distance) the probe accepts a step exactly where the follower's judge
// carries it (no step GoTo::LegCarried would refuse as an order to its end), a fragment end refuses the piece without a
// path call, and the mutants (the old type mask; a fragment walked unprobed) are killed. Fix round: the step judged is
// the order's own smooth path (OrderStep), aimed by straight corners on a long walk; the engine's one line refuses
// the piece, and the mutant judging the straight path is killed. Usage: travel-walk-selftest
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "GoToRules.h"
#include "MeshMoveRules.h"
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
        if (Judge(walked.walk, Answer(NORMAL)) == Verdict::Refuse)
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
        check(Judge(walk, Answer(NORMAL | 0x10, 25.0f)) == Verdict::Fallback && walk.mode == Mode::Fallback,
              "off mesh: a shortcut answer (no polygon at the point) falls back");
        check(Target(walk, points, first, graph.nodes[3]) == NO_POINT, "off mesh: the fallback heads for the far node");
        check(Judge(walk, Answer(NORMAL)) == Verdict::Walk && walk.moves == 1,
              "off mesh: the fallback walks node to node, counted");
        check(std::string(RowWord(walk)) == "fallback", "off mesh: the row says fallback");
        LinkWalk noPath;
        Begin(noPath, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        check(Judge(noPath, Answer(0x08, 0.0f, 0.0f)) == Verdict::Fallback,
              "off mesh: no path to a stored point falls back");
        Begin(walk, 3, 2, 6, graph.PathOf(graph.links[6]), false, graph.nodes[3], graph.nodes[2]);
        check(walk.mode == Mode::Stored && walk.moves == 0 && walk.hops == 0,
              "off mesh: the next link starts on stored points, counts anew");

        // No path at the fallback: refused, never walked as a straight line
        check(Judge(noPath, Answer(0x08, 0.0f, 0.0f)) == Verdict::Refuse && noPath.mode == Mode::Refused,
              "no path: no path at the fallback refuses the link");
        check(std::string(RowWord(noPath)) == "refused", "no path: the row says refused");
        LinkWalk nodes;
        Begin(nodes, 1, 2, NO_LINK, std::span<P const>{}, false, graph.nodes[1], graph.nodes[2]);
        check(Judge(nodes, Answer(0x08, 0.0f, 0.0f, true)) == Verdict::Refuse && nodes.mode == Mode::Refused &&
                  nodes.moves == 0,
              "no path: far from the node the link is refused too (WT64g: no stepping stone), no move counted");
        LinkWalk failed;
        Begin(failed, 1, 2, NO_LINK, std::span<P const>{}, false, graph.nodes[1], graph.nodes[2]);
        check(Judge(failed, Answer(0, 0.0f, 0.0f, false)) == Verdict::Refuse,
              "no path: a failed path calculation refuses a link without rows");
        // A refused leg's order takes no other move: every later answer, a carried one too, is refused and uncounted
        check(Judge(failed, Answer(NORMAL)) == Verdict::Refuse &&
                  Judge(failed, Answer(INCOMPLETE, 12.0f)) == Verdict::Refuse && failed.moves == 0 && failed.hops == 0,
              "no path: a refused link takes no other move");
        // Every answer of "no path" gets Fallback or Refuse, near the order or far in stored points
        bool never = true;
        for (Mode const mode : {Mode::Stored, Mode::Nodes, Mode::Fallback})
        {
            LinkWalk one;
            one.mode = mode;
            one.from = 1;
            one.to = 2;
            Verdict const v = Judge(one, Answer(0x08, 40.0f, 0.0f));
            never = never && (v == Verdict::Fallback || v == Verdict::Refuse);
        }
        check(never, "no path: no mode walks a near no-path answer");
    }

    // An accepted order that moved nothing (WT64g): the same place ordered again from where the last order left the
    // bot counts as refused at the NO_MOTION_STRIKES-th time in a row; any advance, or another place, starts again
    {
        std::uint8_t strikes = 0;
        float const farOff = 40.0f;
        check(!StillOrder(strikes, false, 0.0f, farOff) && strikes == 0, "still: a new place is no strike");
        check(!StillOrder(strikes, true, 0.5f, farOff) && strikes == 1,
              "still: the same place from the same spot, strike 1");
        check(StillOrder(strikes, true, 1.9f, farOff),
              "still: the second order in a row that moved nothing is refused");
        std::uint8_t moved = 1;
        check(!StillOrder(moved, true, PROGRESS_YARDS, farOff) && moved == 0,
              "still: an advance of PROGRESS_YARDS resets");
        std::uint8_t first = 0;
        check(!StillOrder(first, true, 1.0e9f, farOff) && first == 0,
              "still: the first order of a place (none before) is free");
        // WT64g-2: a bot at the place (waiting at a dock point for its car) has nothing to move, however often it
        // orders
        std::uint8_t waiting = 0;
        check(!StillOrder(waiting, true, 0.0f, 1.9f) && !StillOrder(waiting, true, 0.0f, AT_PLACE_YARDS) &&
                  !StillOrder(waiting, true, 0.0f, 1.2f) && waiting == 0,
              "still: orders from within AT_PLACE_YARDS of the place are no strikes");
        std::uint8_t beside = NO_MOTION_STRIKES - 1;
        check(StillOrder(beside, true, 0.0f, AT_PLACE_YARDS + 6.0f),
              "still: 9 yd from the place and standing is refused");
        {
            // Mutant: the bot at its place counted as standing
            std::uint8_t atPlace = NO_MOTION_STRIKES - 1;
            bool const mutantRefuses = static_cast<std::uint8_t>(atPlace + 1) >= NO_MOTION_STRIKES;
            check(mutantRefuses != StillOrder(atPlace, true, 0.0f, 1.0f),
                  "mutant killed: a bot waiting at its place refused");
        }
        // Mutants: refused at the first still order; an advance not resetting the strikes
        std::uint8_t eager = 0;
        bool const eagerRefuses = ++eager >= 1;
        check(eagerRefuses != StillOrder(strikes = 0, true, 0.0f, farOff),
              "mutant killed: refused at the first still order");
        std::uint8_t sticky = NO_MOTION_STRIKES - 1;
        bool const stickyRefuses = static_cast<std::uint8_t>(sticky + 1) >= NO_MOTION_STRIKES;
        check(stickyRefuses != StillOrder(sticky, true, 10.0f, farOff),
              "mutant killed: an advance that keeps the strikes");
    }

    // The hop: a partial path whose last step is longer than a walking step is walked and counted, once per answer
    {
        LinkWalk walk;
        Begin(walk, 1, 2, NO_LINK, std::span<P const>{}, false, graph.nodes[1], graph.nodes[2]);
        check(Judge(walk, Answer(INCOMPLETE, 12.0f)) == Verdict::Hop && walk.hops == 1 && walk.moves == 1,
              "hop: a 12 yd last step is a hop");
        check(Judge(walk, Answer(NORMAL, 3.0f)) == Verdict::Walk && walk.hops == 1 && walk.moves == 2,
              "hop: a 3 yd last step is a walk");
    }

    // No progress: an order that gains less than PROGRESS_YARDS, or the same point ordered again, is a strike; the
    // third falls back
    {
        std::span<P const> const points = graph.PathOf(graph.links[5]);
        LinkWalk walk;
        Begin(walk, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        Target(walk, points, graph.nodes[2], graph.nodes[3]);
        Verdict const a = Judge(walk, Answer(NORMAL, 3.0f, 0.5f));
        Verdict const b = Judge(walk, Answer(NORMAL, 3.0f, 0.5f));
        Verdict const c = Judge(walk, Answer(NORMAL, 3.0f, 0.5f));
        check(a == Verdict::Walk && b == Verdict::Walk && c == Verdict::Fallback && walk.mode == Mode::Fallback,
              "no progress: three orders without gain fall back");
        LinkWalk again;
        Begin(again, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        Verdict const first = Judge(again, Answer(NORMAL));
        Verdict const second = Judge(again, Answer(NORMAL));
        Verdict const third = Judge(again, Answer(NORMAL));
        Verdict const fourth = Judge(again, Answer(NORMAL));
        check(
            first == Verdict::Walk && second == Verdict::Walk && third == Verdict::Walk && fourth == Verdict::Fallback,
            "no progress: the same point ordered three times again falls back");
        // A reached point resets the strikes
        LinkWalk reset;
        Begin(reset, 2, 3, 5, points, false, graph.nodes[2], graph.nodes[3]);
        std::uint32_t const at = Target(reset, points, graph.nodes[2], graph.nodes[3]);
        Judge(reset, Answer(NORMAL, 3.0f, 0.5f));
        Judge(reset, Answer(NORMAL, 3.0f, 0.5f));
        Target(reset, points, PointAt(points, false, at), graph.nodes[3]);
        check(reset.strikes == 0 && Judge(reset, Answer(NORMAL, 3.0f, 0.5f)) == Verdict::Walk,
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
    // The probe with the route's judge (MeshMove::ProbeAccepts)
    auto const probe = [](auto const& from, auto const& to, auto const& step)
    { return ProbePiece(from, to, step, MeshMove::ProbeAccepts); };
    {
        // A real path to the goal in one step: its corners thinned 25 yd apart, none beside the goal
        P const goal{300.0f, 0.0f, 0.0f};
        PieceWalk const piece = probe(origin, goal, [&](ProbePoint const& from, ProbePoint const& to)
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
        PieceWalk const longWay = probe(origin, P{6000.0f, 0.0f, 0.0f}, [&](ProbePoint const& f, ProbePoint const& t)
                                        { return answer(PATH_NORMAL, line(f, t, 30.0f, 9000.0f)); });
        check(longWay.probe == Probe::Walk && longWay.orders.size() == PROBE_ORDERS,
              "probe: a 6000 yd path keeps PROBE_ORDERS orders");
    }
    {
        // The engine's straight lines and broken answers are refused at the first step, as the judge reads them: no
        // path, the line, a cut path, a shortcut, an end far from a polygon and the engine's one-line partial answer
        // (its unreached target copied in as the end: a hop, WT64i)
        P const goal{60.0f, 0.0f, 0.0f};
        bool allRefused = true;
        for (std::uint32_t const type :
             {PATH_NOPATH, PATH_NORMAL | PATH_NOPATH, PATH_NORMAL | PATH_SHORT, PATH_NORMAL | PATH_NOT_USING_PATH,
              PATH_INCOMPLETE | PATH_NOT_USING_PATH, PATH_NORMAL | MeshMove::PATH_SHORTCUT,
              PATH_NORMAL | PATH_FARFROMPOLY_END, PATH_NORMAL | MeshMove::PATH_FARFROMPOLY_START, PATH_INCOMPLETE,
              std::uint32_t{0}})
        {
            PieceWalk const piece = probe(
                origin, goal, [&](ProbePoint const& from, ProbePoint const& to) { return answer(type, {from, to}); });
            allRefused = allRefused && piece.Refused() && piece.probe == Probe::PathType && piece.steps == 1 &&
                         piece.orders.empty();
        }
        check(allRefused,
              "probe: no path, the line, a cut path, a shortcut, an end off the mesh, the one-line partial "
              "answer and a blank answer are refused");
        PieceWalk const failed = probe(origin, goal, [&](ProbePoint const& from, ProbePoint const& to)
                                       { return answer(PATH_NORMAL, {from, to}, false); });
        PieceWalk const empty =
            probe(origin, goal, [&](ProbePoint const&, ProbePoint const&) { return answer(PATH_NORMAL, {}); });
        check(failed.probe == Probe::PathType && empty.probe == Probe::PathType &&
                  std::string(ProbeWord(failed.probe)) == "path_type",
              "probe: a failed path call and one without points are refused");
    }
    {
        // A partial path chained to the goal: each step from the last one's end; within REACH_YARDS is there. Its
        // last step is a short one (4 yd here), else the judge reads the end as the engine's hop. A goal more than
        // 7 yd off the mesh is refused at the first step (WT64i: the judge's end far from a polygon)
        P const goal{350.0f, 0.0f, 0.0f};
        auto const partial = [&](std::uint32_t type)
        {
            return [&, type](ProbePoint const& from, ProbePoint const& to)
            { return answer(type, line(from, to, 4.0f, std::min(100.0f, Distance(from, to) - 8.0f))); };
        };
        PieceWalk const chained = probe(origin, goal, partial(PATH_INCOMPLETE));
        PieceWalk const offMesh = probe(origin, goal, partial(PATH_INCOMPLETE | PATH_FARFROMPOLY_END));
        PieceWalk const hopped = probe(origin, goal, [&](ProbePoint const& from, ProbePoint const& to)
                                       { return answer(PATH_INCOMPLETE, line(from, to, 20.0f, 100.0f)); });
        check(chained.probe == Probe::Walk && chained.steps == 4 && !chained.orders.empty() &&
                  chained.orders.back().x < 350.0f - SPACING_YARDS / 2.0f,
              "probe: a partial path chained to within 10 yd of the goal in four steps");
        check(offMesh.probe == Probe::PathType && offMesh.steps == 1 && offMesh.orders.empty(),
              "probe: a goal more than 7 yd off the mesh is refused at the first step");
        check(hopped.probe == Probe::PathType && hopped.steps == 1,
              "probe: a partial answer whose last step is longer than HOP_STEP_YARDS is refused (the judge's hop)");
        // A partial path that ends at the same spot from its own end (a mesa top on its own mesh) stalls
        PieceWalk const stalled =
            probe(origin, goal,
                  [&](ProbePoint const& from, ProbePoint const&)
                  {
                      return from.x >= 119.0f
                                 ? answer(PATH_INCOMPLETE, {from, from})
                                 : answer(PATH_INCOMPLETE, line(from, ProbePoint{120.0f, 0.0f, 0.0f}, 4.0f, 1000.0f));
                  });
        PieceWalk const crawl = probe(origin, goal, [&](ProbePoint const& from, ProbePoint const&)
                                      { return answer(PATH_INCOMPLETE, {from, {from.x + 1.5f, 0.0f, 0.0f}}); });
        check(stalled.probe == Probe::Stall && stalled.steps == 2 && stalled.orders.empty() &&
                  crawl.probe == Probe::Stall && crawl.steps == 1 && std::string(ProbeWord(stalled.probe)) == "stall",
              "probe: a partial path that gains under 2 yd on the goal stalls");
        // A chain still short of the goal after PROBE_STEPS steps
        PieceWalk const endless = probe(
            origin, P{1000.0f, 0.0f, 0.0f}, [&](ProbePoint const& from, ProbePoint const&)
            { return answer(PATH_INCOMPLETE, {from, {from.x + 6.0f, 0.0f, 0.0f}, {from.x + 10.0f, 0.0f, 0.0f}}); });
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
    {
        // WT64i, one reader: over generated answers (every PathType bit combination x last step x end distance x the
        // call's result) the probe accepts its first step exactly where the judge's class is Carried, and so never
        // one that GoTo::LegCarried would refuse as an order to the step's own end. The step is judged as class only
        // (the order's end distance 0, no counted hop): the probe's own walk-end test reads the distance
        P const goal{400.0f, 0.0f, 0.0f};
        float const lastSteps[] = {0.0f, 1.0f, 4.0f, HOP_STEP_YARDS, HOP_STEP_YARDS + 0.5f, 6.0f, 20.0f, 78.0f};
        float const endDistances[] = {0.0f, 5.0f, REACH_YARDS, REACH_YARDS + 1.0f, 38.0f, 120.0f};
        auto const sweep = [&](auto const& accepts, int& accepted, int& acceptedPartial, auto const& judged)
        {
            int wrong = 0;
            for (std::uint32_t type = 0; type <= 0xFF; ++type)
                for (float const lastStep : lastSteps)
                    for (float const endDistance : endDistances)
                        for (bool const result : {true, false})
                        {
                            ProbePoint const last{goal.x - endDistance, 0.0f, 0.0f};
                            std::vector<ProbePoint> points{ProbePoint{}};
                            if (lastStep > 0.0f)
                                points.push_back({last.x - lastStep, 0.0f, 0.0f});
                            points.push_back(last);
                            if (lastStep == 0.0f)
                                points.push_back(last);
                            ProbeAnswer const generated = answer(type, points, result);
                            int calls = 0;
                            PieceWalk const piece = ProbePiece(
                                origin, goal, [&](ProbePoint const& from, ProbePoint const& to)
                                { return ++calls == 1 ? judged(generated, from, to) : answer(PATH_NOPATH, {from}); },
                                accepts);
                            bool const took = !(piece.steps == 1 && piece.probe == Probe::PathType);
                            float const lastMeasured = MeshMove::LastStep(generated.points);
                            bool const carried =
                                result && MeshMove::Classify(type, lastMeasured) == MeshMove::Class::Carried;
                            bool const order = GoTo::LegCarried(type, lastMeasured, false, 0.0f,
                                                                Distance(ProbePoint{}, last), REACH_YARDS);
                            if (took != carried || (took && !order))
                                ++wrong;
                            if (took)
                            {
                                ++accepted;
                                if (type & PATH_INCOMPLETE)
                                    ++acceptedPartial;
                            }
                        }
            return wrong;
        };
        // The step's answer as the engine gave it (plain), or as the probe asks it: the order's smooth path aimed by a
        // straight path that carries to the goal (TravelWalk::OrderStep; the generated answer is the smooth one)
        auto const plain = [](ProbeAnswer const& generated, ProbePoint const&, ProbePoint const&) { return generated; };
        auto const straightToGoal = [&](ProbePoint const& from, ProbePoint const& to)
        { return answer(PATH_NORMAL, line(from, to, 20.0f, 1000.0f)); };
        int aimedElsewhere = 0;
        auto const ordered = [&](ProbeAnswer const& generated, ProbePoint const& from, ProbePoint const& to)
        {
            auto const smooth = [&](ProbePoint const&, ProbePoint const& aim)
            {
                if (Distance(aim, to) > 0.0f)
                    ++aimedElsewhere;
                return generated;
            };
            return OrderStep(from, to, straightToGoal, smooth, 1000.0f);
        };
        int accepted = 0;
        int partials = 0;
        int const wrong = sweep(MeshMove::ProbeAccepts, accepted, partials, plain);
        check(wrong == 0 && accepted > 0 && partials > 0,
              "probe: over generated answers a step is taken exactly where the judge carries it (" +
                  std::to_string(wrong) + " wrong of " + std::to_string(accepted) + " taken)");
        // Mutant: the probe's old mask (#2747's type flags: an end far from a polygon, a shortcut and the one-line
        // partial answer pass)
        auto const oldMask = [](ProbeAnswer const& a)
        {
            return a.result && !a.points.empty() && (a.type & (PATH_NORMAL | PATH_INCOMPLETE)) &&
                   !(a.type & (PATH_NOT_USING_PATH | PATH_NOPATH | PATH_SHORT));
        };
        int mutantTaken = 0;
        int mutantPartials = 0;
        check(sweep(oldMask, mutantTaken, mutantPartials, plain) > 0,
              "mutant killed: the probe accepts by the old type mask");

        // WT64i fix round, the order's own call: over the same generated answers, now the smooth path's under a
        // straight path that carries to the goal, a step is taken exactly where the smooth answer is carried
        int orderedTaken = 0;
        int orderedPartials = 0;
        int const orderedWrong = sweep(MeshMove::ProbeAccepts, orderedTaken, orderedPartials, ordered);
        check(orderedWrong == 0 && orderedTaken == accepted && aimedElsewhere == 0,
              "probe: over generated smooth answers a step is taken exactly where the order's call is carried (" +
                  std::to_string(orderedWrong) + " wrong of " + std::to_string(orderedTaken) + " taken)");
        // Mutant: the probe judges the straight path that aimed the call (as before the fix round)
        auto const judgedStraight = [&](ProbeAnswer const&, ProbePoint const& from, ProbePoint const& to)
        { return straightToGoal(from, to); };
        int straightTaken = 0;
        int straightPartials = 0;
        check(sweep(MeshMove::ProbeAccepts, straightTaken, straightPartials, judgedStraight) > 0,
              "mutant killed: the probe judges the straight path, not the order's smooth one");
        // The engine's one line on the smooth path (a 2-point shortcut without a path, type 0xa; or the start polygon
        // alone with the target copied in, INCOMPLETE) where the straight path's corners carry: refused at the probe,
        // no point to order. Mutant: the straight answer judged walks it
        ProbePoint const near{300.0f, 0.0f, 0.0f};
        std::vector<ProbeAnswer> const oneLines{answer(MeshMove::PATH_SHORTCUT | PATH_NOPATH, {ProbePoint{}, near}),
                                                answer(PATH_INCOMPLETE, {ProbePoint{}, near})};
        for (ProbeAnswer const& oneLine : oneLines)
        {
            auto const smoothLine = [&](ProbePoint const&, ProbePoint const&) { return oneLine; };
            PieceWalk const refused = probe(origin, near, [&](ProbePoint const& from, ProbePoint const& to)
                                            { return OrderStep(from, to, straightToGoal, smoothLine, 1000.0f); });
            PieceWalk const mutant = probe(origin, near, straightToGoal);
            check(refused.probe == Probe::PathType && refused.steps == 1 && refused.orders.empty() &&
                      mutant.probe == Probe::Walk,
                  "probe: the order's smooth path as the engine's one line (type " + std::to_string(oneLine.type) +
                      ") refuses the piece the straight corners carry; mutant killed");
        }
        // A long walk: each step is asked to the last straight corner within the aim's length, the chain goes on
        // from the smooth answer's end, and the orders (4 yd smooth points, about 25 yd apart) cover it past the
        // old cap of 32
        P const far{1000.0f, 0.0f, 0.0f};
        std::vector<float> aims;
        auto const corners100 = [&](ProbePoint const& from, ProbePoint const& to)
        { return answer(PATH_NORMAL, line(from, to, 100.0f, 5000.0f)); };
        auto const smooth4 = [&](ProbePoint const& from, ProbePoint const& aim)
        {
            aims.push_back(aim.x);
            return answer(PATH_NORMAL, line(from, aim, 4.0f, 5000.0f));
        };
        PieceWalk const longWalk = probe(origin, far, [&](ProbePoint const& from, ProbePoint const& to)
                                         { return OrderStep(from, to, corners100, smooth4, 296.0f); });
        check(longWalk.probe == Probe::Walk && longWalk.steps == 5 &&
                  same(aims, {200.0f, 400.0f, 600.0f, 800.0f, 1000.0f}) && longWalk.orders.size() > 32,
              "probe: a 1000 yd walk is asked in 5 smooth steps aimed at corners within 296 yd (" +
                  std::to_string(longWalk.orders.size()) + " orders)");

        // A fragment end refuses the piece without a path call, whether or not the other end's tile is loaded; an
        // unloaded tile alone leaves the piece unprobed; else the probe runs
        int calls = 0;
        auto const counted = [&](ProbePoint const& from, ProbePoint const& to)
        {
            ++calls;
            return answer(PATH_NORMAL, line(from, to, 20.0f, 1000.0f));
        };
        PieceWalk const fragment = StartPiece(true, true, origin, goal, counted, MeshMove::ProbeAccepts);
        PieceWalk const fragmentUnloaded = StartPiece(true, false, origin, goal, counted, MeshMove::ProbeAccepts);
        check(fragment.probe == Probe::Fragment && fragment.Refused() && fragment.steps == 0 &&
                  fragment.orders.empty() && fragmentUnloaded.probe == Probe::Fragment && calls == 0 &&
                  std::string(ProbeWord(fragment.probe)) == "fragment",
              "probe: a fragment end refuses the piece, cause fragment, without a path call");
        PieceWalk const unloaded = StartPiece(false, false, origin, goal, counted, MeshMove::ProbeAccepts);
        PieceWalk const probed = StartPiece(false, true, origin, goal, counted, MeshMove::ProbeAccepts);
        check(unloaded.probe == Probe::None && !unloaded.Refused() && calls == 1 && probed.probe == Probe::Walk,
              "probe: an unloaded tile leaves the piece unprobed; loaded ends are probed");
        // Mutant: the fragment end walked unprobed as before WT64i
        PieceWalk const walkedAsBefore{};
        check(walkedAsBefore.Refused() != fragment.Refused(), "mutant killed: a fragment end walked unprobed");
    }

    std::cout << "travel-walk-selftest: " << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
