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
// the piece, and the mutant judging the straight path is killed. Round 4: each step is judged as the follower's order
// to the walk's end from the step's start (GoTo::ProbeCarried: its end within the local range too), and the chain is
// done at the waypoint's reach, where the follower stops ordering; over generated closing answers the probe walks a
// piece exactly where the follower's last order is carried, and the mutants (the class alone; done within
// REACH_YARDS) are killed. Round 5: a step aimed at a corner is judged as the order to that corner, so a winding walk
// whose first corners lead away from its end is walked; the mutant judging it against the walk's end is killed.
// Usage: travel-walk-selftest
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "GoToRules.h"
#include "MeshMoveRules.h"
#include "TravelLegRules.h"
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
    // The probe with the route's judge (GoTo::ProbeCarried), done within the reach of the route's last waypoint
    auto const probe = [](auto const& from, auto const& to, auto const& step)
    { return ProbePiece(from, to, TravelLegs::GOAL_YARDS, step, GoTo::ProbeCarried); };
    // Round 3's judge: the answer's class alone (a mutant)
    auto const classOnly = [](ProbeAnswer const& a, ProbePoint const&, ProbePoint const&)
    {
        return a.result && !a.points.empty() &&
               MeshMove::Classify(a.type, MeshMove::LastStep(a.points)) == MeshMove::Class::Carried;
    };
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
        // A partial path that ends at the same spot from its own end (a mesa top on its own mesh), or crawls, far from
        // the goal: the follower's order there gains under LEG_GAIN_YARDS, refused (a stall near the waypoint below)
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
        check(stalled.probe == Probe::PathType && stalled.steps == 2 && stalled.orders.empty() &&
                  crawl.probe == Probe::PathType && crawl.steps == 1,
              "probe: a partial path that gains under LEG_GAIN_YARDS far from the goal is refused as the order");
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
        // Round 4: the step judged as the follower's order to the goal from its start, at 400 yd (the gain on it) and
        // at 60 yd (within the local range: its end within REACH_YARDS of the goal)
        P const near60{60.0f, 0.0f, 0.0f};
        auto const sweepAt =
            [&](P const& target, auto const& accepts, int& accepted, int& acceptedPartial, auto const& judged)
        {
            int wrong = 0;
            for (std::uint32_t type = 0; type <= 0xFF; ++type)
                for (float const lastStep : lastSteps)
                    for (float const endDistance : endDistances)
                        for (bool const result : {true, false})
                        {
                            ProbePoint const last{target.x - endDistance, 0.0f, 0.0f};
                            std::vector<ProbePoint> points{ProbePoint{}};
                            if (lastStep > 0.0f)
                                points.push_back({last.x - lastStep, 0.0f, 0.0f});
                            points.push_back(last);
                            if (lastStep == 0.0f)
                                points.push_back(last);
                            ProbeAnswer const generated = answer(type, points, result);
                            int calls = 0;
                            PieceWalk const piece = ProbePiece(
                                origin, target, TravelLegs::GOAL_YARDS,
                                [&](ProbePoint const& from, ProbePoint const& to)
                                { return ++calls == 1 ? judged(generated, from, to) : answer(PATH_NOPATH, {from}); },
                                accepts);
                            bool const took = !(piece.steps == 1 && piece.probe == Probe::PathType);
                            bool const order = GoTo::LegCarried(type, MeshMove::LastStep(generated.points), false,
                                                                endDistance, Distance(origin, target), REACH_YARDS);
                            if (took != (result && order))
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
        auto const sweep = [&](auto const& accepts, int& accepted, int& acceptedPartial, auto const& judged)
        { return sweepAt(goal, accepts, accepted, acceptedPartial, judged); };
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
        int const wrong = sweep(GoTo::ProbeCarried, accepted, partials, plain);
        check(wrong == 0 && accepted > 0 && partials > 0,
              "probe: over generated answers a step is taken exactly where the judge carries it (" +
                  std::to_string(wrong) + " wrong of " + std::to_string(accepted) + " taken)");
        // Mutant: the probe's old mask (#2747's type flags: an end far from a polygon, a shortcut and the one-line
        // partial answer pass)
        auto const oldMask = [](ProbeAnswer const& a, ProbePoint const&, ProbePoint const&)
        {
            return a.result && !a.points.empty() && (a.type & (PATH_NORMAL | PATH_INCOMPLETE)) &&
                   !(a.type & (PATH_NOT_USING_PATH | PATH_NOPATH | PATH_SHORT));
        };
        int mutantTaken = 0;
        int mutantPartials = 0;
        check(sweep(oldMask, mutantTaken, mutantPartials, plain) > 0,
              "mutant killed: the probe accepts by the old type mask");
        // Within the local range: a step is taken exactly where the follower's order from its start is carried, its
        // end within REACH_YARDS of the goal. Mutant: the class alone (round 3) takes an end 11 to 120 yd short
        int localTaken = 0;
        int localPartials = 0;
        int classTaken = 0;
        int classPartials = 0;
        int const localWrong = sweepAt(near60, GoTo::ProbeCarried, localTaken, localPartials, plain);
        check(localWrong == 0 && localTaken > 0 && localPartials > 0 && localTaken < accepted,
              "probe: within the local range a step is taken exactly where the follower's order is carried (" +
                  std::to_string(localWrong) + " wrong of " + std::to_string(localTaken) + " taken)");
        check(sweepAt(near60, classOnly, classTaken, classPartials, plain) > 0,
              "mutant killed: within the local range the probe judges the class alone");

        // WT64i fix round, the order's own call: over the same generated answers, now the smooth path's under a
        // straight path that carries to the goal, a step is taken exactly where the smooth answer is carried
        int orderedTaken = 0;
        int orderedPartials = 0;
        int const orderedWrong = sweep(GoTo::ProbeCarried, orderedTaken, orderedPartials, ordered);
        check(orderedWrong == 0 && orderedTaken == accepted && aimedElsewhere == 0,
              "probe: over generated smooth answers a step is taken exactly where the order's call is carried (" +
                  std::to_string(orderedWrong) + " wrong of " + std::to_string(orderedTaken) + " taken)");
        // Mutant: the probe judges the straight path that aimed the call (as before the fix round)
        auto const judgedStraight = [&](ProbeAnswer const&, ProbePoint const& from, ProbePoint const& to)
        { return straightToGoal(from, to); };
        int straightTaken = 0;
        int straightPartials = 0;
        check(sweep(GoTo::ProbeCarried, straightTaken, straightPartials, judgedStraight) > 0,
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

        // WT64i round 5: a winding walk whose corners first lead away from its end (the end 150 yd off, the path
        // 550 yd: 200 yd away, 150 yd across, 200 yd back). Each step is asked to a corner within 296 yd and judged as
        // the order to that corner, its progress measured on it, so the carried NORMAL path is walked in three steps.
        // Mutant: an aimed step judged against the walk's end refuses the piece at its first step
        P const across{150.0f, 0.0f, 0.0f};
        std::vector<ProbePoint> const bends{{0.0f, 200.0f, 0.0f}, {150.0f, 200.0f, 0.0f}, {150.0f, 0.0f, 0.0f}};
        auto const winding = [&](ProbePoint const& from, ProbePoint const&)
        {
            std::size_t next = 0;
            for (std::size_t i = 0; i < bends.size(); ++i)
                if (Distance(from, bends[i]) < 1.0f)
                    next = i + 1;
            std::vector<ProbePoint> points{from};
            points.insert(points.end(), bends.begin() + static_cast<std::ptrdiff_t>(next), bends.end());
            return answer(PATH_NORMAL, points);
        };
        auto const smoothTo = [&](ProbePoint const& from, ProbePoint const& aim)
        { return answer(PATH_NORMAL, line(from, aim, 4.0f, 5000.0f)); };
        auto const windingStep = [&](ProbePoint const& from, ProbePoint const& to)
        { return OrderStep(from, to, winding, smoothTo, 296.0f); };
        PieceWalk const wound = probe(origin, across, windingStep);
        auto const toTheEnd = [&](ProbeAnswer const& a, ProbePoint const& start, ProbePoint const&)
        { return GoTo::ProbeCarried(a, start, ProbePoint{across.x, across.y, across.z}); };
        PieceWalk const woundMutant = ProbePiece(origin, across, TravelLegs::GOAL_YARDS, windingStep, toTheEnd);
        check(wound.probe == Probe::Walk && wound.steps == 3 && !wound.orders.empty() &&
                  woundMutant.probe == Probe::PathType && woundMutant.steps == 1,
              "probe: a winding walk whose first corners lead away from its end is walked, each step judged as the "
              "order to its corner; mutant killed");

        // A fragment end refuses the piece without a path call, whether or not the other end's tile is loaded; an
        // unloaded tile alone leaves the piece unprobed; else the probe runs
        int calls = 0;
        auto const counted = [&](ProbePoint const& from, ProbePoint const& to)
        {
            ++calls;
            return answer(PATH_NORMAL, line(from, to, 20.0f, 1000.0f));
        };
        PieceWalk const fragment =
            StartPiece(true, true, origin, goal, TravelLegs::GOAL_YARDS, counted, GoTo::ProbeCarried);
        PieceWalk const fragmentUnloaded =
            StartPiece(true, false, origin, goal, TravelLegs::GOAL_YARDS, counted, GoTo::ProbeCarried);
        check(fragment.probe == Probe::Fragment && fragment.Refused() && fragment.steps == 0 &&
                  fragment.orders.empty() && fragmentUnloaded.probe == Probe::Fragment && calls == 0 &&
                  std::string(ProbeWord(fragment.probe)) == "fragment",
              "probe: a fragment end refuses the piece, cause fragment, without a path call");
        PieceWalk const unloaded =
            StartPiece(false, false, origin, goal, TravelLegs::GOAL_YARDS, counted, GoTo::ProbeCarried);
        PieceWalk const probed =
            StartPiece(false, true, origin, goal, TravelLegs::GOAL_YARDS, counted, GoTo::ProbeCarried);
        check(unloaded.probe == Probe::None && !unloaded.Refused() && calls == 1 && probed.probe == Probe::Walk,
              "probe: an unloaded tile leaves the piece unprobed; loaded ends are probed");
        // Mutant: the start as before WT64i, a fragment end walked unprobed (StartPiece without its fragment arm)
        auto const refusesFragment = [&](auto const& start)
        {
            PieceWalk const piece = start(true, true);
            return piece.probe == Probe::Fragment && piece.Refused() && piece.orders.empty();
        };
        auto const startNow = [&](bool fragmentEnd, bool tilesLoaded)
        {
            return StartPiece(fragmentEnd, tilesLoaded, origin, goal, TravelLegs::GOAL_YARDS, counted,
                              GoTo::ProbeCarried);
        };
        auto const startAsBefore = [&](bool fragmentEnd, bool tilesLoaded)
        {
            return fragmentEnd || !tilesLoaded
                       ? PieceWalk{}
                       : ProbePiece(origin, goal, TravelLegs::GOAL_YARDS, counted, GoTo::ProbeCarried);
        };
        check(refusesFragment(startNow) && !refusesFragment(startAsBefore),
              "mutant killed: a fragment end walked unprobed");
    }
    {
        // WT64i round 4, the chain's end as the follower reads it. A partial path from 57 yd that stops 12.3 yd short,
        // where a next step from its end would reach: the follower orders the waypoint from the start itself (it lies
        // within the lookahead) and is refused, so the probe refuses the piece. Mutant: the class alone walks it
        P const goal57{57.0f, 0.0f, 0.0f};
        auto const shortThenReach = [&](ProbePoint const& from, ProbePoint const& to)
        {
            return from.x < 1.0f ? answer(PATH_INCOMPLETE, line(from, to, 4.0f, Distance(from, to) - 12.3f))
                                 : answer(PATH_NORMAL, line(from, to, 4.0f, 1000.0f));
        };
        PieceWalk const shortStep = probe(origin, goal57, shortThenReach);
        PieceWalk const shortMutant = ProbePiece(origin, goal57, TravelLegs::GOAL_YARDS, shortThenReach, classOnly);
        check(shortStep.probe == Probe::PathType && shortStep.steps == 1 && shortStep.orders.empty() &&
                  shortMutant.probe == Probe::Walk && shortMutant.steps == 2,
              "probe: a step from within the local range that stops 12.3 yd short refuses the piece; mutant killed");

        // A partial path that stops 8 yd short of a waypoint the follower walks to within WAYPOINT_YARDS: the probe
        // asks the follower's next order, from that spot to the waypoint, judged as the order. Over generated closing
        // answers (type bits x last step x end distance) the piece is walked exactly where that order is carried and
        // ends within the waypoint's reach (or the first end already lies within it: the route's last waypoint)
        P const waypoint{60.0f, 0.0f, 0.0f};
        ProbePoint const shortEnd{52.0f, 0.0f, 0.0f};
        auto const firstShort = [&](ProbePoint const& from, ProbePoint const& to)
        { return answer(PATH_INCOMPLETE, line(from, to, 4.0f, Distance(from, to) - 8.0f)); };
        float const closingSteps[] = {0.0f, 1.0f, 4.0f, HOP_STEP_YARDS + 0.5f, 8.0f};
        float const closingEnds[] = {0.0f, 3.0f, TravelLegs::WAYPOINT_YARDS, 6.0f, 8.0f, REACH_YARDS + 1.0f};
        auto const chainEnd = [&](float probeReach, float followerReach, int& walked)
        {
            int wrong = 0;
            for (std::uint32_t type = 0; type <= 0xFF; ++type)
                for (float const lastStep : closingSteps)
                    for (float const endDistance : closingEnds)
                    {
                        ProbePoint const last{waypoint.x - endDistance, 0.0f, 0.0f};
                        std::vector<ProbePoint> points{shortEnd};
                        if (lastStep > 0.0f)
                            points.push_back({last.x - lastStep, 0.0f, 0.0f});
                        points.push_back(last);
                        if (lastStep == 0.0f)
                            points.push_back(last);
                        int calls = 0;
                        PieceWalk const piece = ProbePiece(
                            origin, waypoint, probeReach,
                            [&](ProbePoint const& from, ProbePoint const& to)
                            {
                                ++calls;
                                return calls == 1   ? firstShort(from, to)
                                       : calls == 2 ? answer(type, points)
                                                    : answer(PATH_NOPATH, {from});
                            },
                            GoTo::ProbeCarried);
                        bool const order = GoTo::LegCarried(type, MeshMove::LastStep(points), false, endDistance,
                                                            Distance(shortEnd, waypoint), REACH_YARDS);
                        bool const followerWalks =
                            Distance(shortEnd, waypoint) <= followerReach || (order && endDistance <= followerReach);
                        if ((piece.probe == Probe::Walk) != followerWalks)
                            ++wrong;
                        if (piece.probe == Probe::Walk)
                            ++walked;
                    }
            return wrong;
        };
        int walked = 0;
        int walkedLast = 0;
        int walkedMutant = 0;
        int const chainWrong = chainEnd(TravelLegs::WAYPOINT_YARDS, TravelLegs::WAYPOINT_YARDS, walked);
        int const lastWrong = chainEnd(TravelLegs::GOAL_YARDS, TravelLegs::GOAL_YARDS, walkedLast);
        check(chainWrong == 0 && walked > 0 && lastWrong == 0 && walkedLast > walked,
              "probe: over generated closing answers a piece is walked exactly where the follower's last order is "
              "carried within the waypoint's reach (" +
                  std::to_string(chainWrong + lastWrong) + " wrong, " + std::to_string(walked) + " walked)");
        // Mutant: done within REACH_YARDS (round 3), where the follower walks to within WAYPOINT_YARDS
        check(chainEnd(REACH_YARDS, TravelLegs::WAYPOINT_YARDS, walkedMutant) > 0,
              "mutant killed: the probe is done within REACH_YARDS of a waypoint the follower walks to within 5 yd");
        // The cases seen live (WT64i-3): the engine's one line from the short end (2 points, the unreached waypoint
        // copied in: an 8 yd hop), which the follower's own order there refuses, and an end that stays where it is
        // (the follower's still strikes refuse it): the piece is refused, no point ordered
        auto const closing = [&](ProbeAnswer const& second)
        {
            int calls = 0;
            return ProbePiece(
                origin, waypoint, TravelLegs::WAYPOINT_YARDS, [&](ProbePoint const& from, ProbePoint const& to)
                { return ++calls == 1 ? firstShort(from, to) : second; }, GoTo::ProbeCarried);
        };
        ProbeAnswer const oneLine = answer(PATH_INCOMPLETE, {shortEnd, ProbePoint{60.0f, 0.0f, 0.0f}});
        PieceWalk const hopEnd = closing(oneLine);
        PieceWalk const stayEnd = closing(answer(PATH_INCOMPLETE, {shortEnd, shortEnd}));
        std::uint8_t strikes = 0;
        bool const firstStill = StillOrder(strikes, true, 0.0f, 8.0f);
        bool const secondStill = StillOrder(strikes, true, 0.0f, 8.0f);
        check(hopEnd.probe == Probe::PathType && hopEnd.steps == 2 && hopEnd.orders.empty() &&
                  !GoTo::LegCarried(oneLine.type, MeshMove::LastStep(oneLine.points), false, 0.0f, 8.0f, REACH_YARDS) &&
                  stayEnd.probe == Probe::Stall && stayEnd.steps == 2 && stayEnd.orders.empty() && !firstStill &&
                  secondStill && std::string(ProbeWord(stayEnd.probe)) == "stall",
              "probe: the one line or a standing end 8 yd short of a waypoint refuses the piece, as the follower's "
              "order and its still strikes refuse it there");
    }
    {
        // Pathing PA4: the probe searches with a pool that spans the walk, and a refusal says why (SearchWhy). Made-up
        // engine answers on a made-up mesh: the walk from the origin to G, 600 yd, which the mesh joins. A search of a
        // small pool runs out of nodes and ends in a pocket P off the way, from which it finds no point nearer; the
        // wide pool reaches G. A second mesh does not join: its search spends the bot's piece and ends at its edge E
        P const goal{600.0f, 0.0f, 0.0f};
        ProbePoint const pocket{200.0f, 80.0f, 0.0f};
        ProbePoint const edge{250.0f, 0.0f, 0.0f};
        std::uint32_t const outOfNodes = SEARCH_OUT_OF_NODES | SEARCH_PARTIAL;
        auto const engine =
            [&](std::uint32_t type, std::vector<ProbePoint> points, std::uint32_t status, bool startPoly = true)
        {
            StepAnswer out;
            out.result = true;
            out.type = type;
            out.points = std::move(points);
            out.why = SearchWhy(type, status, startPoly);
            return out;
        };
        // A search to `to` that ends at `stop` (the pocket or the edge) with `status`; from `stop` it gains nothing
        auto const fallsShort = [&](ProbePoint const& stop, std::uint32_t status, bool smooth)
        {
            return [&, stop, status, smooth](ProbePoint const& from, ProbePoint const&)
            {
                if (Distance(from, stop) < 1.0f)
                    return engine(PATH_INCOMPLETE, {from, from}, status);
                return engine(PATH_INCOMPLETE, smooth ? line(from, stop, 4.0f, 1000.0f) : std::vector{from, stop},
                              status);
            };
        };
        auto const reaches = [&](bool smooth)
        {
            return [&, smooth](ProbePoint const& from, ProbePoint const& to)
            { return engine(PATH_NORMAL, smooth ? line(from, to, 4.0f, 1000.0f) : std::vector{from, to}, 0); };
        };
        auto const chain = [&](auto const& shape, auto const& order)
        {
            return [&shape, &order](ProbePoint const& from, ProbePoint const& to)
            { return OrderStep(from, to, shape, order, 296.0f); };
        };
        auto const smallShape = fallsShort(pocket, outOfNodes, false);
        auto const smallOrder = fallsShort(pocket, outOfNodes, true);
        auto const cutShape = fallsShort(edge, SEARCH_PARTIAL, false);
        auto const cutOrder = fallsShort(edge, SEARCH_PARTIAL, true);
        auto const wideShape = reaches(false);
        auto const wideOrder = reaches(true);
        PieceWalk const walked = probe(origin, goal, chain(wideShape, wideOrder));
        PieceWalk const nodes = probe(origin, goal, chain(smallShape, smallOrder));
        PieceWalk const cut = probe(origin, goal, chain(cutShape, cutOrder));
        // A refusal as the probe leaves it: refused, its reason, no order to walk
        auto const refusedWith = [](PieceWalk const& piece, ProbeWhy why)
        { return piece.Refused() && piece.why == why && piece.orders.empty(); };
        check(
            walked.probe == Probe::Walk && walked.steps == 1 && !walked.orders.empty() && walked.why == ProbeWhy::None,
            "PA4: the walk whose small-pool search ends in a pocket is one walk with the pool that spans it");
        check(refusedWith(nodes, ProbeWhy::Nodes) && nodes.probe == Probe::PathType && nodes.steps == 1 &&
                  std::string(WhyWord(nodes.why)) == "nodes",
              "PA4: an exhausted pool (the pocket) is refused as nodes at its first step and leaves no order");
        check(refusedWith(cut, ProbeWhy::NoJoin) && cut.steps == 1 && std::string(WhyWord(cut.why)) == "no_join",
              "PA4: a pair the mesh does not join is refused as no_join at its first step and leaves no order");
        // PA4c: a step aimed at a corner on the way whose search to the end ran out of nodes goes on, and the next
        // step from that corner reaches the end (a long walk, as offline: 21 walks pass a step out of nodes). The
        // search to G from the origin runs out at F, 400 yd along, so the step asks that corner; from F it reaches G
        ProbePoint const far{400.0f, 0.0f, 0.0f};
        auto const longShape = [&](ProbePoint const& from, ProbePoint const& to)
        {
            return Distance(from, far) < 1.0f ? engine(PATH_NORMAL, {from, to}, 0)
                                              : engine(PATH_INCOMPLETE, {from, far}, outOfNodes);
        };
        PieceWalk const longWalk = probe(origin, goal, chain(longShape, wideOrder));
        // Mutant: refused at the first search out of nodes, whatever the step asked (offline it refuses 9 walks)
        auto const anyShort = [&](auto const& stepOf)
        {
            return [&stepOf](ProbePoint const& from, ProbePoint const& to)
            {
                StepAnswer answer = stepOf(from, to);
                if (answer.why == ProbeWhy::Nodes)
                    answer.points = {from, from};
                return answer;
            };
        };
        auto const longChain = chain(longShape, wideOrder);
        PieceWalk const longMutant = probe(origin, goal, anyShort(longChain));
        check(longWalk.probe == Probe::Walk && longWalk.steps == 2 && !longWalk.orders.empty() && longMutant.Refused(),
              "PA4c: a step aimed at a corner whose search to the end ran out of nodes goes on to the end; mutant "
              "(refused at any search out of nodes) killed");
        // An end without a polygon: the engine's straight line (0x11) from the first call; which end, by the start's
        // polygon; an end over 7 yd from its polygon by the FARFROMPOLY bit
        auto const line11 = [&](bool startPoly)
        {
            return [&, startPoly](ProbePoint const& from, ProbePoint const& to)
            { return engine(PATH_NORMAL | PATH_NOT_USING_PATH, {from, to}, 0, startPoly); };
        };
        auto const noEndShape = line11(true);
        auto const noStartShape = line11(false);
        PieceWalk const noEnd = probe(origin, goal, chain(noEndShape, noEndShape));
        PieceWalk const noStart = probe(origin, goal, chain(noStartShape, noStartShape));
        check(refusedWith(noEnd, ProbeWhy::NoEnd) && noEnd.steps == 1 && std::string(WhyWord(noEnd.why)) == "no_end" &&
                  refusedWith(noStart, ProbeWhy::NoStart) && noStart.steps == 1 &&
                  std::string(WhyWord(noStart.why)) == "no_start",
              "PA4: an end without a polygon is refused as no_end or no_start and leaves no order");
        check(SearchWhy(PATH_INCOMPLETE | PATH_FARFROMPOLY_START, 0, true) == ProbeWhy::NoStart &&
                  SearchWhy(PATH_INCOMPLETE | PATH_FARFROMPOLY_END, outOfNodes, true) == ProbeWhy::NoEnd &&
                  SearchWhy(PATH_NORMAL, 0, true) == ProbeWhy::None && std::string(WhyWord(ProbeWhy::None)) == "none",
              "PA4: an end over 7 yd from its polygon names that end; a search that reached its end names none");
        // A fragment end stays refused without a search, with no reason of the search
        std::uint32_t calls = 0;
        auto const counted = [&](ProbePoint const& from, ProbePoint const& to)
        {
            ++calls;
            return OrderStep(from, to, wideShape, wideOrder, 296.0f);
        };
        PieceWalk const fragment =
            StartPiece(true, true, origin, goal, TravelLegs::GOAL_YARDS, counted, GoTo::ProbeCarried);
        check(fragment.probe == Probe::Fragment && fragment.why == ProbeWhy::None && calls == 0,
              "PA4: a fragment end is refused without a search");
        // Mutants: the chain taking the pocket's end as the walk's (an arrival anywhere); a refusal that keeps its
        // orders; the reason read partial before out of nodes (the word swapped); the straight line's end not asked
        PieceWalk const pocketMutant =
            ProbePiece(origin, goal, 1000.0f, chain(smallShape, smallOrder), GoTo::ProbeCarried);
        PieceWalk keptMutant = nodes;
        keptMutant.orders = walked.orders;
        auto const partialFirst = [](std::uint32_t status)
        {
            return (status & SEARCH_PARTIAL)        ? ProbeWhy::NoJoin
                   : (status & SEARCH_OUT_OF_NODES) ? ProbeWhy::Nodes
                                                    : ProbeWhy::None;
        };
        auto const lineIsEnd = [](std::uint32_t type)
        { return (type & PATH_NOT_USING_PATH) ? ProbeWhy::NoEnd : ProbeWhy::None; };
        // PA4c mutant: the chain without the stop at a step that asked the end goes on from the pocket (a second
        // search, the flood again) before it refuses
        PieceWalk const goesOn = ProbePiece(
            origin, goal, TravelLegs::GOAL_YARDS,
            [&](ProbePoint const& from, ProbePoint const& to)
            {
                StepAnswer answer = chain(smallShape, smallOrder)(from, to);
                answer.why = ProbeWhy::None;
                return answer;
            },
            GoTo::ProbeCarried);
        check(pocketMutant.probe == Probe::Walk && !refusedWith(keptMutant, ProbeWhy::Nodes) &&
                  partialFirst(outOfNodes) != nodes.why &&
                  lineIsEnd(PATH_NORMAL | PATH_NOT_USING_PATH) != noStart.why && goesOn.steps != nodes.steps,
              "PA4: mutants killed: the pocket walked, a refusal with orders, the word swapped, the end not asked, the "
              "chain going on past a search that fell short of the end it asked");
    }

    std::cout << "travel-walk-selftest: " << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
