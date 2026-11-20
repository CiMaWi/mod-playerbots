/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the travel graph (src/Mgr/Travel/TravelGraph.h) on the shipped data: parses the INSERT rows of
// data/sql/playerbots/base/playerbots_travelnode*.sql and of the updates under data/sql/playerbots/updates/ (or of a
// graph directory's files alone) into the same TravelGraphBuilder the server's loader feeds, then runs
// TravelGraph::Selftest (the counts, the per-node link ranges, the nearest-node grid and three routes) - the checks
// `playerbots route selftest` runs on the loaded graph.
// Usage: travel-graph-selftest [module root] [graph dir] (defaults: the current directory, the shipped graph)
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "RouteRules.h"
#include "TravelGraph.h"
#include "TravelGraphSelftest.h"
#include "TravelGraphSql.h"

namespace
{
// The route search from one node to another, as a bot with transports allowed (the graph's own positions)
TravelRoute::Result RouteBetween(TravelGraph const& graph, std::uint32_t from, std::uint32_t to)
{
    TravelGraphNode const& a = graph.GetNode(from);
    TravelGraphNode const& b = graph.GetNode(to);
    TravelRoute::Request request;
    request.snapshot.position = {a.mapId, a.x, a.y, a.z};
    request.snapshot.level = 80;
    request.snapshot.allowTransport = true;
    request.goal = {b.mapId, b.x, b.y, b.z};
    return TravelRoute::Search(graph, request, TravelRoute::StuckEdges{}, {});
}

TravelGraphBuilder::LinkRow Walk(std::uint32_t fromId, std::uint32_t toId, std::uint32_t type = 1)
{
    TravelGraphBuilder::LinkRow row;
    row.fromId = fromId;
    row.toId = toId;
    row.type = type;
    row.distance = 70.0f;
    return row;
}

// The builder's rules on a hand-made graph: bad links skipped, orphan points counted, the path length summed per map,
// out-of-order points and a duplicate node id refused; wait points and static portals
std::uint32_t BuilderChecks(std::vector<std::string>& failures)
{
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, char const* what)
    {
        ++checks;
        if (!ok)
            failures.push_back(std::string("builder: ") + what);
    };

    std::string error;
    TravelGraphBuilder builder;
    builder.AddNode(20, "c", 0, 140.0f, 0.0f, 0.0f);
    builder.AddNode(10, "a", 0, 0.0f, 0.0f, 0.0f);
    builder.AddNode(15, "b", 0, 70.0f, 0.0f, 0.0f);
    builder.AddLink(Walk(10, 15));
    builder.AddLink(Walk(15, 20));
    builder.AddLink(Walk(10, 10));     // to itself
    builder.AddLink(Walk(10, 99));     // unknown node
    builder.AddLink(Walk(15, 10, 8));  // unknown type
    builder.AddLink(Walk(15, 10, 5));  // unknown type
    check(builder.EndLinks(error), "a small graph ends its links");
    builder.AddPoint(10, 15, 0, 0, 0.0f, 0.0f, 0.0f);
    builder.AddPoint(10, 15, 1, 0, 3.0f, 4.0f, 0.0f);   // 5 yd
    builder.AddPoint(10, 15, 2, 1, 50.0f, 0.0f, 0.0f);  // another map: no length
    builder.AddPoint(10, 15, 3, 1, 50.0f, 0.0f, 2.0f);  // 2 yd
    builder.AddPoint(30, 40, 0, 0, 0.0f, 0.0f, 0.0f);   // no such link
    TravelGraph graph;
    check(builder.Finish(graph, error), "a small graph finishes");
    graph.Publish(0, 0);
    TravelGraphStats const& stats = graph.Stats();
    check(stats.links == 2 && stats.skippedLinks == 4 && stats.orphanPoints == 1, "bad links skipped, orphans counted");
    check(graph.IndexOfId(10) == 0 && graph.IndexOfId(20) == 2 && graph.IndexOfId(11) == TravelGraph::NO_NODE,
          "nodes ordered by id");
    TravelGraphLink const& link = graph.LinksOf(0)[0];
    check(link.pathCount == 4 && std::fabs(link.pathLength - 7.0f) < 0.001f, "path length summed on one map");
    TravelRoute::Result const route = RouteBetween(graph, 0, 2);
    check(route.ok && std::fabs(route.totalSeconds - 20.0f) < 0.5f, "a two-link route");
    TravelRoute::Result const back = RouteBetween(graph, 2, 0);
    check(!back.ok || (back.legs.size() == 1 && back.legs[0].type == TravelRoute::LegType::Walk),
          "no route against the links (only the straight walk off the graph)");

    TravelGraphBuilder disordered;
    disordered.AddNode(1, "a", 0, 0.0f, 0.0f, 0.0f);
    disordered.AddNode(2, "b", 0, 0.0f, 0.0f, 0.0f);
    disordered.AddLink(Walk(1, 2));
    disordered.AddLink(Walk(2, 1));
    disordered.EndLinks(error);
    disordered.AddPoint(1, 2, 0, 0, 0.0f, 0.0f, 0.0f);
    disordered.AddPoint(2, 1, 0, 0, 0.0f, 0.0f, 0.0f);
    disordered.AddPoint(1, 2, 1, 0, 0.0f, 0.0f, 0.0f);  // the link's points come in two groups
    TravelGraph refused;
    check(!disordered.Finish(refused, error), "points out of order refused");

    TravelGraphBuilder twice;
    twice.AddNode(1, "a", 0, 0.0f, 0.0f, 0.0f);
    twice.AddNode(1, "b", 0, 0.0f, 0.0f, 0.0f);
    twice.AddLink(Walk(1, 1));
    check(!twice.EndLinks(error), "a node id twice refused");

    // Land 1 - wait point 2 - stop 3 = ride = stop 4 - wait point 5 - land 6, a walk 1 - 3 onto the stop, a static
    // portal 1 - 6 and a dangling wait-point link 6 - 7 (7 has no ride). Node id 0 is a node like any other
    TravelGraphBuilder waits;
    // Places for the route search, which prices a walk off the links by its yards: the far land (4, 5) lies 20000 yd
    // east, the portal's landing (6, 7) 50000 yd north, so only the crossing and the portal reach them quickly
    for (std::uint32_t id = 0; id <= 7; ++id)
    {
        float const x = 100.0f * static_cast<float>(id) + (id == 4 || id == 5 ? 20000.0f : 0.0f);
        float const y = id >= 6 ? 50000.0f : 0.0f;
        waits.AddNode(id, "n", 0, x, y, 0.0f);
    }
    auto const row = [](std::uint32_t from, std::uint32_t to, std::uint32_t type, std::uint32_t object)
    {
        TravelGraphBuilder::LinkRow made = Walk(from, to, type);
        made.object = object;
        made.distance = type == 1 || !object ? 30.0f : 0.1f;
        made.extraCost = type == 6 ? 3.0f : 0.0f;          // a static portal's seconds
        made.periodMs = type == 3 && object ? 240000 : 0;  // a ship's period (the loader reads the core's)
        return made;
    };
    for (auto const& [from, to] : {std::pair{0u, 1u}, std::pair{1u, 2u}, std::pair{1u, 3u}, std::pair{5u, 6u}})
        waits.AddLink(row(from, to, 1, 0));
    waits.AddLink(row(2, 3, 3, 0));
    waits.AddLink(row(3, 4, 3, 164871));
    waits.AddLink(row(4, 5, 3, 0));
    waits.AddLink(row(6, 7, 3, 0));
    waits.AddLink(row(1, 6, 6, 193604));
    // The ride's stored crossings (WT70c: no crossing, no ride): boarding at stop 3, leaving at stop 4
    for (std::uint32_t stop = 0; stop < 2; ++stop)
    {
        TravelGraphBuilder::CrossingRow crossing;
        crossing.entry = 164871;
        crossing.stop = stop;
        crossing.x = stop ? 20400.0f : 300.0f;
        crossing.direction = stop;
        waits.AddCrossing(crossing);
    }
    check(waits.EndLinks(error), "a graph with wait points ends its links");
    waits.AddPoint(0, 1, 0, 0, 0.0f, 0.0f, 0.0f);
    TravelGraph waited;
    check(waits.Finish(waited, error), "a graph with wait points finishes");
    waited.Publish(0, 0);
    TravelGraphStats const& waitStats = waited.Stats();
    check(waitStats.links == 9 && !waitStats.skippedLinks && waitStats.linksByType[6] == 1 &&
              waitStats.waitPointLinks == 3 && waitStats.waitStops == 2,
          "static portals and wait-point links load, the two stops are marked");
    check(waited.GetNode(waited.IndexOfId(3)).waitStop && waited.GetNode(waited.IndexOfId(4)).waitStop &&
              !waited.GetNode(waited.IndexOfId(2)).waitStop && !waited.GetNode(waited.IndexOfId(7)).waitStop,
          "a stop is a wait-point link's end with a ride");
    auto const passedIds = [&waited](TravelRoute::Result const& route)
    {
        std::vector<std::uint32_t> passed;
        for (TravelRoute::Leg const& leg : route.legs)
            for (std::uint32_t const node : leg.nodes)
                if (passed.empty() || passed.back() != waited.GetNode(node).id)
                    passed.push_back(waited.GetNode(node).id);
        return passed;
    };
    TravelRoute::Result const crossing = RouteBetween(waited, waited.IndexOfId(0), waited.IndexOfId(5));
    check(crossing.ok && passedIds(crossing) == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5},
          "the route crosses through both wait points, not by the walk onto the stop");
    TravelRoute::Result const portal = RouteBetween(waited, waited.IndexOfId(0), waited.IndexOfId(6));
    check(portal.ok && passedIds(portal) == std::vector<std::uint32_t>{0, 1, 6},
          "the route takes the static portal (WT55d)");
    auto const arrivals = waited.ArrivalsOnto(0);
    check(arrivals.size() == 2 && arrivals[0].node == waited.IndexOfId(4) && arrivals[1].node == waited.IndexOfId(6) &&
              arrivals[1].fixedSeconds == 3.0f,
          "the ride and the static portal arrive (its seconds fixed): no arrival by a wait-point link on one map");
    return checks;
}

// WT70c, harness case 2: a player's jump over a crossing. The spline's height (a straight line plus the parabola of
// the amplitude) is the ballistic z = vt - gt^2/2 for a rise of 0, +1.0 and -5 yd; a rise above the apex and a gap
// past the run's reach are no player's jump
std::uint32_t JumpChecks(std::vector<std::string>& failures)
{
    using namespace TravelCrossing;
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back("jump: " + what);
    };

    for (float const rise : {0.0f, 1.0f, -5.0f})
    {
        Jump jump;
        bool ok = JumpOf(rise, jump);
        float worst = 0.0f;
        for (std::uint32_t step = 0; ok && step <= 20; ++step)
        {
            float const t = jump.seconds * static_cast<float>(step) / 20.0f;
            float const ballistic = JUMP_SPEED * t - JUMP_GRAVITY * t * t / 2.0f;
            worst = std::max(worst, std::fabs(JumpHeight(jump, rise, t) - ballistic));
        }
        check(ok && worst < 0.001f && std::fabs(JumpHeight(jump, rise, jump.seconds) - rise) < 0.001f &&
                  std::fabs(jump.amplitude - JUMP_GRAVITY * jump.seconds * jump.seconds / 8.0f) < 0.0001f,
              "a rise of " + std::to_string(rise) + " yd flies as z = vt - gt^2/2 (off by " + std::to_string(worst) +
                  " yd)");
    }
    Jump level;
    Jump up;
    Jump down;
    Jump drop;
    Jump over;
    check(JumpOf(0.0f, level) && std::fabs(level.seconds - 0.825f) < 0.005f &&
              std::fabs(JumpHeight(level, 0.0f, level.seconds / 2.0f) - JumpApex()) < 0.001f &&
              std::fabs(JumpApex() - 1.642f) < 0.005f,
          "a level jump lasts 0.83 s and peaks at the apex, 1.64 yd");
    check(JumpOf(1.0f, up) && std::fabs(up.seconds - 0.671f) < 0.005f && JumpOf(-5.0f, down) &&
              std::fabs(down.seconds - 1.243f) < 0.005f && JumpOf(-10.0f, drop) &&
              std::fabs(drop.seconds - 1.511f) < 0.005f,
          "a jump up 1 yd lasts 0.67 s, down 5 yd 1.24 s, down 10 yd 1.51 s");
    check(!JumpOf(1.7f, over) && JumpOf(1.64f, over), "a rise of 1.7 yd is above the apex, 1.64 yd is not");
    check(IsPlayerJump(0.0f, 5.7f) && !IsPlayerJump(0.0f, 5.9f) && IsPlayerJump(1.64f, 2.9f) &&
              !IsPlayerJump(1.64f, 3.3f) && !IsPlayerJump(1.7f, 0.5f) && IsPlayerJump(-5.0f, 8.0f),
          "the run carries a level jump 5.8 yd, one to the apex 2.9 yd, a drop further");
    Row row;
    row.rise = 2.0f;
    check(RiseOf(row, Direction::Board) == 2.0f && RiseOf(row, Direction::Leave) == -2.0f &&
              EventAmount(3, Direction::Leave, 2) == 312 && EventAmount(0, Direction::Board, 1) == 1,
          "a row's rise is the deck's above the dock: leaving jumps down it; an event's amount names the row");
    return checks;
}

// WT70c, harness case 16: the loader's rules on a hand-made graph. The stop records and their rows (a jump no player
// makes and a fourth row are not loaded and counted); a ride link meets its two stops; a link without a stop, without
// a boarding or a leaving row, and one whose two ends are stops of different cars are left out and counted; the route
// carries the two records
std::uint32_t CrossingChecks(std::vector<std::string>& failures)
{
    using namespace TravelCrossing;
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, char const* what)
    {
        ++checks;
        if (!ok)
            failures.push_back(std::string("crossings: ") + what);
    };

    constexpr std::uint32_t SHIP = 100;        // boards at node 1, leaves at node 2; no way back
    constexpr std::uint32_t NO_LEAVING = 200;  // boards at node 3, no leaving row at node 4
    constexpr std::uint32_t LIFT = 300;        // cars 7 and 8 at different ends; cars 9 and 10 both between 7 and 8
    constexpr std::uint32_t UNKNOWN = 400;     // no crossing at all
    constexpr std::uint32_t FAR_OFF = 500;     // a stop 3.5 yd from its node
    TravelGraphBuilder builder;
    // Nodes 20000 yd apart on a line (a ride beats the walk), node 9 the wait point of node 7
    for (std::uint32_t id = 1; id <= 8; ++id)
        builder.AddNode(id, "n", 0, 20000.0f * static_cast<float>(id), 0.0f, 0.0f);
    builder.AddNode(9, "wait", 0, 20000.0f * 7.0f - 30.0f, 0.0f, 0.0f);
    auto const ride = [&builder](std::uint32_t from, std::uint32_t to, std::uint32_t object)
    {
        TravelGraphBuilder::LinkRow row = Walk(from, to, 3);
        row.object = object;
        row.distance = object ? 0.1f : 30.0f;
        row.periodMs = object ? 240000 : 0;
        builder.AddLink(row);
    };
    // A crossing row of a stop at node `node`, `off` yards beside it
    auto const crossing = [&builder](std::uint32_t entry, std::uint32_t guid, std::uint32_t stop, std::uint32_t node,
                                     float off, Direction direction, std::uint32_t seq, Kind kind = Kind::Walk,
                                     float gap = 1.0f, float rise = 0.0f)
    {
        TravelGraphBuilder::CrossingRow row;
        row.entry = entry;
        row.guid = guid;
        row.stop = stop;
        row.x = 20000.0f * static_cast<float>(node);
        row.y = off;
        row.o = 1.0f;
        row.direction = static_cast<std::uint32_t>(direction);
        row.seq = seq;
        row.kind = static_cast<std::uint32_t>(kind);
        row.dock = {row.x, off + 5.0f, static_cast<float>(seq)};
        row.deck = {1.0f, 2.0f, 3.0f};
        row.gap = gap;
        row.rise = rise;
        builder.AddCrossing(row);
    };

    // The lift first and the ship's rows against their seq order: the builder sorts by the table's key
    for (std::uint32_t const car : {10u, 9u})
        for (std::uint32_t stop = 0; stop < 2; ++stop)
            for (Direction const direction : {Direction::Board, Direction::Leave})
                crossing(LIFT, car, stop, 7 + stop, car == 9 ? 2.0f : 0.5f, direction, 0);
    crossing(LIFT, 7, 0, 5, 0.0f, Direction::Board, 0);
    crossing(LIFT, 7, 1, 1, 9000.0f, Direction::Leave, 0);
    crossing(LIFT, 8, 0, 6, 0.0f, Direction::Leave, 0);
    crossing(LIFT, 8, 1, 2, 9000.0f, Direction::Board, 0);
    // The ship's from-stop, 2 yd beside node 1. Boarding: seq 0 a jump up 1.7 yd (refused), seq 1 a level jump of
    // 6 yd (refused), seq 2 a jump up 1 yd, seq 3 a walk, seq 4 a jump down, seq 5 a walk (the fourth kept: over 3).
    // Leaving: a jump whose deck lies 1.7 yd above the dock (a drop: kept) and one 1.7 yd below it (refused)
    crossing(SHIP, 0, 0, 1, 2.0f, Direction::Board, 5);
    crossing(SHIP, 0, 0, 1, 2.0f, Direction::Board, 4, Kind::Jump, 2.0f, -3.0f);
    crossing(SHIP, 0, 0, 1, 2.0f, Direction::Board, 3);
    crossing(SHIP, 0, 0, 1, 2.0f, Direction::Board, 2, Kind::Jump, 2.0f, 1.0f);
    crossing(SHIP, 0, 0, 1, 2.0f, Direction::Board, 1, Kind::Jump, 6.0f, 0.0f);
    crossing(SHIP, 0, 0, 1, 2.0f, Direction::Board, 0, Kind::Jump, 1.0f, 1.7f);
    crossing(SHIP, 0, 0, 1, 2.0f, Direction::Leave, 0, Kind::Jump, 1.0f, 1.7f);
    crossing(SHIP, 0, 0, 1, 2.0f, Direction::Leave, 1, Kind::Jump, 1.0f, -1.7f);
    crossing(SHIP, 0, 1, 2, 0.0f, Direction::Leave, 0);
    crossing(NO_LEAVING, 0, 0, 3, 0.0f, Direction::Board, 0);
    crossing(NO_LEAVING, 0, 1, 4, 0.0f, Direction::Board, 0);
    crossing(FAR_OFF, 0, 0, 1, 3.5f, Direction::Board, 0);
    crossing(FAR_OFF, 0, 1, 2, 0.0f, Direction::Leave, 0);

    ride(1, 2, SHIP);        // kept
    ride(2, 1, SHIP);        // no boarding row at node 2's stop
    ride(3, 4, NO_LEAVING);  // no leaving row at node 4's stop
    ride(5, 6, LIFT);        // car 7 stands at node 5, car 8 at node 6: no pair of one car
    ride(7, 8, LIFT);        // kept: cars 9 and 10 both stop at 7 and 8, car 10 nearer
    ride(1, 3, UNKNOWN);     // no stop record of the entry
    ride(1, 2, FAR_OFF);     // the from-stop lies 3.5 yd from node 1
    ride(9, 7, 0);           // the wait point of node 7
    builder.AddLink(Walk(8, 1));
    std::string error;
    check(builder.EndLinks(error), "a graph with crossings ends its links");
    builder.AddPoint(8, 1, 0, 0, 0.0f, 0.0f, 0.0f);
    TravelGraph graph;
    check(builder.Finish(graph, error), "a graph with crossings finishes");
    graph.Publish(0, 0);

    TravelCrossingStats const& stats = graph.Stats().crossings;
    Index const& index = graph.Crossings();
    check(stats.rows == 21 && stats.stops == 14 && stats.vessels == 7 && stats.noPlayerJump == 3 &&
              stats.overThree == 1 && !stats.otherDisplay && index.Size() == 14,
          "the rows not loaded are counted: three jumps no player makes, one row over 3");
    check(stats.ridesKept == 2 && stats.ridesLeftOut == 5 && stats.ridesWithoutWait == 1 &&
              graph.Stats().linksByType[3] == 3 && !graph.Stats().skippedLinks,
          "two rides kept (one without a wait node), five left out and not counted as skipped links");
    check(CrossingLoadLine(stats) ==
              "crossings: 21 rows at 14 stops of 7 vessels (0 other display, 3 no player jump, "
              "1 over 3); rides: 2 kept, 5 left out for no crossing, 1 without a wait node",
          "the load line");

    std::span<Stop const> const ship = index.StopsOf(SHIP, 0);
    bool shipRows = ship.size() == 2 && ship[0].stop == 0 && ship[0].boardCount == 3 && ship[0].leaveCount == 1 &&
                    ship[1].boardCount == 0 && ship[1].leaveCount == 1 && !ship[0].hasWindow;
    if (shipRows)
    {
        std::span<Row const> const board = ship[0].Rows(Direction::Board);
        shipRows = board.size() == 3 && board[0].seq == 2 && board[0].kind == Kind::Jump && board[1].seq == 3 &&
                   board[1].kind == Kind::Walk && board[2].seq == 4 && board[2].dock.z == 4.0f &&
                   board[2].deck.z == 3.0f && ship[0].Rows(Direction::Leave)[0].seq == 0 && ship[0].o == 1.0f;
    }
    check(shipRows, "a stop keeps up to 3 rows per direction in seq order, a refused jump takes no place");
    check(index.StopsOf(LIFT).size() == 8 && index.StopsOf(LIFT, 9).size() == 2 && index.StopsOf(LIFT, 11).empty() &&
              index.StopsOf(UNKNOWN).empty() && !index.At(NO_STOP) && !index.At(14) && index.At(0) &&
              index.At(0)->entry == SHIP && index.IndexOf(*index.At(5)) == 5,
          "the index finds an entry's stops, one car's stops and a record by its index");
    Stop const* const stoodAt = index.StopAt(LIFT, 10, [](Stop const& stop) { return stop.stop == 1; });
    check(stoodAt && stoodAt->guid == 10 && stoodAt->stop == 1 &&
              !index.StopAt(LIFT, 10, [](Stop const&) { return false; }),
          "the stop a car stands at, by the caller's pose test");

    TravelGraphLink const* shipRide = nullptr;
    TravelGraphLink const* liftRide = nullptr;
    for (TravelGraphLink const& link : graph.LinksOf(graph.IndexOfId(1)))
        if (link.object == SHIP)
            shipRide = &link;
    for (TravelGraphLink const& link : graph.LinksOf(graph.IndexOfId(7)))
        if (link.object == LIFT)
            liftRide = &link;
    Stop const* const shipFrom = shipRide ? index.At(shipRide->crossFrom) : nullptr;
    Stop const* const shipTo = shipRide ? index.At(shipRide->crossTo) : nullptr;
    check(shipFrom && shipTo && shipFrom->entry == SHIP && shipFrom->stop == 0 && shipTo->entry == SHIP &&
              shipTo->stop == 1,
          "a ride link meets its two stops (the from-stop 2 yd beside its node)");
    Stop const* const liftFrom = liftRide ? index.At(liftRide->crossFrom) : nullptr;
    Stop const* const liftTo = liftRide ? index.At(liftRide->crossTo) : nullptr;
    check(liftFrom && liftTo && liftFrom->guid == 10 && liftTo->guid == 10 && liftFrom->stop == 0 && liftTo->stop == 1,
          "among several cars the pair with the smallest sum");
    bool walksClean = true;
    for (std::uint32_t node = 0; node < graph.NodeCount(); ++node)
        for (TravelGraphLink const& link : graph.LinksOf(node))
            if (!(link.type == TravelLinkType::Transport && link.object))
                walksClean = walksClean && link.crossFrom == NO_STOP && link.crossTo == NO_STOP;
    check(walksClean, "a link that is no ride carries no stop record");

    auto const leftOut = [&graph](std::uint32_t entry, std::uint32_t fromId, TravelRideGap gap, std::uint32_t guid)
    {
        for (TravelRideLeftOut const& out : graph.RidesLeftOut())
            if (out.entry == entry && out.fromId == fromId)
                return out.gap == gap && out.guid == guid;
        return false;
    };
    check(graph.RidesLeftOut().size() == 5 && leftOut(SHIP, 2, TravelRideGap::BoardRow, 0) &&
              leftOut(NO_LEAVING, 3, TravelRideGap::LeaveRow, 0),
          "a link without a boarding row or without a leaving row is left out");
    check(leftOut(LIFT, 5, TravelRideGap::ToStop, 7), "a link whose two ends are stops of different cars is left out");
    check(leftOut(UNKNOWN, 1, TravelRideGap::FromStop, 0) && leftOut(FAR_OFF, 1, TravelRideGap::FromStop, 0),
          "a link without a stop record within 3 yd of its from node is left out");
    check(RideLeftOutLine(graph.RidesLeftOut()[0]) ==
              "ride left out: entry 100 guid 0 nodes 2 -> 1 (no boarding row at the from-stop)",
          "a ride left out has its line");

    TravelRoute::Result const route = RouteBetween(graph, graph.IndexOfId(1), graph.IndexOfId(2));
    TravelRoute::Leg const* rideLeg = nullptr;
    for (TravelRoute::Leg const& leg : route.legs)
        if (leg.type == TravelRoute::LegType::Transport)
            rideLeg = &leg;
    check(route.ok && rideLeg && shipRide && rideLeg->object == SHIP && rideLeg->crossFrom == shipRide->crossFrom &&
              rideLeg->crossTo == shipRide->crossTo && rideLeg->crossFrom != NO_STOP,
          "the route's transport leg carries the ride's two stop records");
    TravelRoute::Leg const walk;
    check(walk.crossFrom == NO_STOP && walk.crossTo == NO_STOP, "a leg without a ride carries none");
    return checks;
}
}  // namespace

int main(int argc, char** argv)
{
    std::filesystem::path const root = argc > 1 ? argv[1] : ".";
    std::filesystem::path const graphDir = argc > 2 ? argv[2] : "";
    TravelGraph graph;
    TravelGraphSql::LoadInfo info;
    std::string error;
    if (!TravelGraphSql::Load(root, {}, graph, info, error, graphDir))
    {
        std::cout << "travel-graph-selftest: build failed: " << error << "\n";
        return error.rfind("no shipped graph", 0) == 0 ? 2 : 1;
    }
    std::size_t const* rows = info.rows;
    std::uint32_t const loadMs = info.loadMs;

    TravelGraphStats const& stats = graph.Stats();
    std::cout << "rows read: " << rows[0] << " nodes, " << rows[1] << " links, " << rows[2] << " points\n";
    std::cout << "graph: " << stats.nodes << " nodes, " << stats.links << " links, " << stats.points << " points, "
              << static_cast<double>(stats.memoryBytes) / (1024.0 * 1024.0) << " MB, parsed in " << loadMs << " ms\n";

    // The loader's lines (the server logs the same text; offline: no display check, no stop windows)
    std::cout << CrossingLoadLine(stats.crossings) << " (" << info.crossingRows << " table rows read)\n";
    for (TravelRideLeftOut const& ride : graph.RidesLeftOut())
        std::cout << "  " << RideLeftOutLine(ride) << "\n";

    std::vector<std::string> failures;
    std::vector<std::string> notes;
    auto const routeStart = std::chrono::steady_clock::now();
    std::uint32_t const checks =
        graph.Selftest(failures, notes) + BuilderChecks(failures) + JumpChecks(failures) + CrossingChecks(failures);
    auto const selftestMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - routeStart).count();
    for (std::string const& note : notes)
        std::cout << "  " << note << "\n";
    for (std::string const& failure : failures)
        std::cout << "FAIL: " << failure << "\n";
    std::cout << "travel-graph-selftest: " << (checks - failures.size()) << "/" << checks << " passed (checks in "
              << selftestMs << " ms)\n";
    return failures.empty() ? 0 : 1;
}
