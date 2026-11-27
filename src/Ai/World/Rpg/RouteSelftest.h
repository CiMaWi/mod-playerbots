/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ROUTESELFTEST_H
#define _PLAYERBOT_ROUTESELFTEST_H

#include <cstdint>
#include <string>
#include <vector>

#include "RouteRules.h"

// The route search's checks (RouteRules.h), shared by tests/route-rules-selftest.cpp and
// `playerbots route selftest` (TravelGraph.cpp); only they include this header.
namespace TravelRoute
{
// The checks shared by tests/route-rules-selftest.cpp and `playerbots route selftest` on the shipped graph with its
// TaxiPath data. Returns the number of checks; failures get one line each, notes the route summaries
inline std::uint32_t Selftest(TravelGraph const& graph, std::vector<std::string>& failures,
                              std::vector<std::string>& notes)
{
    std::uint32_t checks = 0;
    auto const check = [&checks, &failures](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
            failures.push_back(what);
        return ok;
    };
    auto const num = [](double value) { return std::to_string(std::llround(value)); };

    if (!check(graph.IsReady(), "graph ready"))
        return checks;
    if (!check(graph.Stats().taxiPaths > 0, "the graph has TaxiPath data (DBC)"))
        return checks;
    check(graph.Stats().flightsWithoutTaxi == 0,
          std::to_string(graph.Stats().flightsWithoutTaxi) + " flight links without a TaxiPath");

    Request notReady;
    TravelGraph const idle;
    check(Search(idle, notReady, StuckEdges{}, Params{}).failure == Failure::GraphNotReady,
          "a graph that is not ready gives no route");

    // Places by name nearest a known position (names are not unique), as in TravelGraph::Selftest
    auto const place = [&graph](char const* name, std::uint32_t mapId, float x, float y) -> Point
    {
        std::uint32_t const index = graph.FindNear(name, mapId, x, y);
        if (index == TravelGraph::NO_NODE)
            return {TravelGraph::ANY_MAP, 0.0f, 0.0f, 0.0f};
        TravelGraphNode const& node = graph.GetNode(index);
        return {node.mapId, node.x, node.y, node.z};
    };
    // The taxi node of a flight master node: the origin of its flight links
    auto const taxiNodeAt = [&graph](Point const& at) -> std::uint32_t
    {
        std::vector<std::uint32_t> const closest = graph.NearestNodes(at.mapId, at.x, at.y, at.z, 1);
        if (closest.empty())
            return 0;
        for (TravelGraphLink const& link : graph.LinksOf(closest.front()))
            if (TravelTaxiPath const* path = graph.TaxiPathOf(link))
                return path->from;
        return 0;
    };

    Point const goldshire = place("Goldshire innkeeper", 0, -9462.7f, 16.2f);
    Point const stormwindInn = place("Stormwind City innkeeper", 0, -8867.8f, 673.7f);
    Point const stormwindFlight = place("flightMaster", 0, -8835.8f, 490.1f);
    Point const ironforgeFlight = place("flightMaster", 0, -4821.8f, -1152.4f);
    Point const darnassus = place("Darnassus", 1, 9947.0f, 2482.0f);
    std::uint32_t const stormwindTaxi = taxiNodeAt(stormwindFlight);
    std::uint32_t const ironforgeTaxi = taxiNodeAt(ironforgeFlight);
    bool const placed = goldshire.mapId == 0 && stormwindInn.mapId == 0 && stormwindFlight.mapId == 0 &&
                        ironforgeFlight.mapId == 0 && darnassus.mapId == 1;
    if (!check(placed && stormwindTaxi && ironforgeTaxi, "fixture places and taxi nodes found"))
        return checks;

    // Every route: legs chain from the start to the goal and their seconds add up
    auto const describe = [&num](Result const& route)
    {
        std::string text = num(route.totalSeconds) + " s, " + std::to_string(route.expandedNodes) + " expanded:";
        for (Leg const& leg : route.legs)
            text += std::string(" ") + LegTypeWord(leg.type) + " " + num(leg.seconds) + " s" +
                    (leg.fareCopper ? " " + std::to_string(leg.fareCopper) + "c" : "") +
                    (leg.object ? " #" + std::to_string(leg.object) : "") + ";";
        return text;
    };
    auto const legCount = [](Result const& route, LegType type)
    {
        return static_cast<std::size_t>(
            std::count_if(route.legs.begin(), route.legs.end(), [type](Leg const& leg) { return leg.type == type; }));
    };
    auto const same = [](Point const& a, Point const& b) { return a.mapId == b.mapId && Distance(a, b) < 0.01f; };
    auto const sound = [&](Result const& route, Request const& request, std::string const& label)
    {
        if (!check(route.ok, label + ": no route (" + FailureWord(route.failure) + ")"))
            return false;
        bool chained = !route.legs.empty() && same(route.legs.front().from, request.snapshot.position) &&
                       same(route.legs.back().to, request.goal);
        float sum = 0.0f;
        for (std::size_t i = 0; i < route.legs.size(); ++i)
        {
            sum += route.legs[i].seconds;
            chained = chained && (i == 0 || same(route.legs[i - 1].to, route.legs[i].from));
        }
        check(chained, label + ": legs do not chain from start to goal");
        check(std::fabs(sum - route.totalSeconds) <= 0.001f * route.totalSeconds + 0.05f,
              label + ": legs sum to " + num(sum) + " s, not " + num(route.totalSeconds) + " s");
        notes.push_back(label + ": " + describe(route));
        return true;
    };
    Params const aStar;
    Params dijkstraParams;
    dijkstraParams.heuristic = false;
    auto const optimal =
        [&](Result const& route, Request const& request, StuckEdges const& stuck, std::string const& label)
    {
        Result const reference = Search(graph, request, stuck, dijkstraParams);
        check(reference.ok == route.ok &&
                  std::fabs(reference.totalSeconds - route.totalSeconds) <= 0.001f * reference.totalSeconds + 0.05f,
              label + ": A* " + num(route.totalSeconds) + " s, Dijkstra " + num(reference.totalSeconds) + " s");
        return reference;
    };

    // a) and c) stay on their own continent: the portals between maps are closed for them. Their subjects are the walk
    //    and the hearthstone against the flight. By time alone the cheapest way of a) is through the Dark Portal: the
    //    offline graph of tests/route-rules-selftest.cpp has no portal gates, the core lets every level through that
    //    portal, and the capitals' portals to the Blasted Lands are gated at 58 (PA38c). In a server the danger term
    //    (awareness AW37a) keeps bots off it, and the graph is ready only with the term folded; this selftest does not
    //    test the term
    StuckEdges homeMap;
    // A route's seconds less the danger term of its walk links (the term only adds): for each consecutive node pair of
    // a walk leg the shortest walk link between them, as DangerFold.h's WalkLinksOf picks it
    auto const withoutTerm = [&graph](Result const& route, Snapshot const& bot)
    {
        float seconds = route.totalSeconds;
        for (Leg const& leg : route.legs)
        {
            if (leg.type != LegType::Walk)
                continue;
            for (std::size_t i = 1; i < leg.nodes.size(); ++i)
            {
                TravelGraphLink const* best = nullptr;
                if (leg.nodes[i - 1] < graph.NodeCount())
                    for (TravelGraphLink const& link : graph.LinksOf(leg.nodes[i - 1]))
                        if (link.to == leg.nodes[i] && link.type == TravelLinkType::Walk &&
                            (!best || link.distance < best->distance))
                            best = &link;
                if (best)
                    seconds -= DangerRules::LinkTerm(graph.LinkCount(), graph.IndexOfLink(*best), bot.team, bot.level);
            }
        }
        return seconds;
    };
    for (std::uint32_t index = 0; index < graph.NodeCount(); ++index)
        for (TravelGraphLink const& link : graph.LinksOf(index))
            if ((link.type == TravelLinkType::Portal || link.type == TravelLinkType::StaticPortal) &&
                graph.GetNode(link.from).mapId != graph.GetNode(link.to).mapId)
                homeMap.flags[EdgeKey(link.from, link.to)] = STUCK_SKIP_FLAGS;

    // a) A level 20 human at the Goldshire inn, no flight points, bound in Stormwind (ready), to Ironforge. With no
    //    flights and no transports (the Deeprun Tram is one) the way is on foot through Redridge, the Burning Steppes,
    //    the Searing Gorge, the Badlands and Loch Modan or Dun Morogh: slow
    Snapshot human;
    human.position = goldshire;
    human.team = 0;
    human.level = 20;
    human.money = 10000;
    human.hasHearthstone = true;
    human.hearth = stormwindInn;
    Request toIronforge{human, ironforgeFlight};
    Result const walked = Search(graph, toIronforge, StuckEdges{}, aStar);  // d) flags an edge of it
    Result const onFoot = Search(graph, toIronforge, homeMap, aStar);
    if (sound(onFoot, toIronforge, "a) Goldshire -> Ironforge, no flight points"))
    {
        Result const reference = optimal(onFoot, toIronforge, homeMap, "a)");
        check(legCount(onFoot, LegType::Flight) == 0, "a) a flight leg without known flight points");
        check(legCount(onFoot, LegType::Transport) == 0, "a) a transport leg without allowTransport");
        check(legCount(onFoot, LegType::Hearth) == legCount(reference, LegType::Hearth),
              "a) A* and Dijkstra disagree on the hearth");
        float const onFootPlain = withoutTerm(onFoot, human);
        check(onFootPlain >= 1500.0f && onFootPlain <= 20000.0f,
              "a) " + num(onFootPlain) + " s without the danger term (" + num(onFoot.totalSeconds) +
                  " s with it) outside 1500..20000 s");
        check(onFoot.expandedNodes <= reference.expandedNodes,
              "a) A* expanded " + std::to_string(onFoot.expandedNodes) + " nodes, Dijkstra " +
                  std::to_string(reference.expandedNodes));
    }

    // b) The same bot at the Stormwind flight master, Stormwind and Ironforge known: one flight of about 6500 yd
    //    (200 s, 3.5 min in the game) beats any walk; without money the flight is gone
    Snapshot flyer = human;
    flyer.position = stormwindFlight;
    flyer.LearnTaxiNode(stormwindTaxi);
    flyer.LearnTaxiNode(ironforgeTaxi);
    Request flight{flyer, ironforgeFlight};
    Result const flown = Search(graph, flight, StuckEdges{}, aStar);
    if (sound(flown, flight, "b) Stormwind -> Ironforge, both known"))
    {
        optimal(flown, flight, StuckEdges{}, "b)");
        auto const leg = std::find_if(flown.legs.begin(), flown.legs.end(),
                                      [](Leg const& candidate) { return candidate.type == LegType::Flight; });
        check(legCount(flown, LegType::Flight) == 1, "b) not exactly one flight leg");
        check(leg != flown.legs.end() && leg->seconds < 400.0f && leg->fareCopper > 0,
              "b) the flight leg is not under 400 s with a fare");
        check(leg != flown.legs.end() && leg->taxiNodes.size() == 2 && leg->taxiNodes.front() == stormwindTaxi &&
                  leg->taxiNodes.back() == ironforgeTaxi,
              "b) the flight leg's taxi nodes are not Stormwind -> Ironforge");
        check(flown.totalSeconds < 400.0f, "b) " + num(flown.totalSeconds) + " s, not under 400 s");
    }
    Request broke = flight;
    broke.snapshot.money = 0;
    Result const poor = Search(graph, broke, StuckEdges{}, aStar);
    check(legCount(poor, LegType::Flight) == 0, "b) a flight leg without money");
    check(!poor.ok || poor.totalSeconds > flown.totalSeconds, "b) the route without money is not dearer");
    // The follower's time-value rule and the search agree: the flown route (fare weighed in) is chosen exactly when
    // FlightWorthIt says the flight saves more than its fare weighs
    if (poor.ok && flown.ok)
    {
        std::uint32_t const fare = RouteFare(flown);
        float const pure = flown.totalSeconds - FareSeconds(fare, flyer.money, flyer.thrift);
        check(FlightWorthIt(poor.totalSeconds, pure, fare, flyer.money, flyer.thrift) ==
                  (flown.totalSeconds < poor.totalSeconds),
              "b) FlightWorthIt and the search disagree on Stormwind -> Ironforge");
    }
    check(FlightWorthIt(1000.0f, 200.0f, 100, 100000, 50), "b) FlightWorthIt: a cheap fare saves 800 s");
    check(!FlightWorthIt(1000.0f, 200.0f, 100000, 100000, 100), "b) FlightWorthIt: the whole purse weighs an hour");
    check(!FlightWorthIt(1000.0f, 200.0f, 500, 100, 50), "b) FlightWorthIt: an unaffordable fare");
    check(!FlightWorthIt(200.0f, 190.0f, 50000, 100000, 100), "b) FlightWorthIt: 10 s saved, half a purse spent");
    Request unknown = flight;
    unknown.snapshot.knownTaxi = {};
    unknown.snapshot.LearnTaxiNode(stormwindTaxi);
    check(legCount(Search(graph, unknown, StuckEdges{}, aStar), LegType::Flight) == 0,
          "b) a flight to an unknown node");
    Request learned = flight;
    learned.snapshot.knownTaxi = {};
    learned.snapshot.LearnTaxiNode(ironforgeTaxi);
    check(legCount(Search(graph, learned, StuckEdges{}, aStar), LegType::Flight) == 1,
          "b) the origin's node is not learned at its flight master");
    Request horde = flight;
    horde.snapshot.team = 1;
    check(legCount(Search(graph, horde, StuckEdges{}, aStar), LegType::Flight) == 0,
          "b) a Horde bot flies from an Alliance flight master");

    // c) At the Ironforge flight master, home in Stormwind: a ready hearth (10 s) beats the flight; with 20 minutes
    //    of cooldown the flight wins
    Request home{flyer, stormwindInn};
    home.snapshot.position = ironforgeFlight;
    Result const hearthed = Search(graph, home, StuckEdges{}, aStar);
    if (sound(hearthed, home, "c) Ironforge -> Stormwind inn, hearth ready"))
    {
        optimal(hearthed, home, StuckEdges{}, "c) ready");
        check(!hearthed.legs.empty() && hearthed.legs.front().type == LegType::Hearth,
              "c) the ready hearth is not the first leg");
        check(hearthed.totalSeconds < 60.0f, "c) " + num(hearthed.totalSeconds) + " s with a ready hearth");
    }
    Request cooling = home;
    cooling.snapshot.hearthCooldownMs = 20 * 60 * 1000;
    Result const flownHome = Search(graph, cooling, homeMap, aStar);
    if (sound(flownHome, cooling, "c) Ironforge -> Stormwind inn, hearth in 20 min"))
    {
        optimal(flownHome, cooling, homeMap, "c) cooling");
        check(legCount(flownHome, LegType::Hearth) == 0, "c) the cooling hearth is used");
        check(legCount(flownHome, LegType::Flight) == 1, "c) no flight home");
        check(flownHome.totalSeconds > hearthed.totalSeconds, "c) the cooling hearth's route is not dearer");
    }
    // A short cooldown is waited for (60 s + 10 s beats the 200 s flight); one past the cap is no edge at all
    Request shortWait = home;
    shortWait.snapshot.hearthCooldownMs = 60 * 1000;
    Result const waited = Search(graph, shortWait, StuckEdges{}, aStar);
    if (sound(waited, shortWait, "c) Ironforge -> Stormwind inn, hearth in 60 s"))
        check(legCount(waited, LegType::Hearth) == 1 && waited.totalSeconds < 120.0f,
              "c) a 60 s cooldown is not waited for (" + num(waited.totalSeconds) + " s)");
    Request pastCap = home;
    pastCap.snapshot.hearthCooldownMs = HEARTH_WAIT_CAP_MS + 1000;
    check(legCount(Search(graph, pastCap, StuckEdges{}, aStar), LegType::Hearth) == 0,
          "c) a cooldown past the wait cap is used");
    Request noStone = home;
    noStone.snapshot.hasHearthstone = false;
    check(legCount(Search(graph, noStone, StuckEdges{}, aStar), LegType::Hearth) == 0,
          "c) a hearth leg without a hearthstone");

    // d) Stuck edges: flag the first graph edge of the walk in a); one flag makes it dearer or avoided, three skip it,
    //    and the book's flags expire after the avoid window
    std::uint32_t flaggedFrom = TravelGraph::NO_NODE;
    std::uint32_t flaggedTo = TravelGraph::NO_NODE;
    for (Leg const& leg : walked.legs)
    {
        if (leg.type == LegType::Walk && leg.nodes.size() >= 2)
        {
            flaggedFrom = leg.nodes[0];
            flaggedTo = leg.nodes[1];
            break;
        }
    }
    auto const uses = [&](Result const& route)
    {
        for (Leg const& leg : route.legs)
            for (std::size_t i = 1; i < leg.nodes.size(); ++i)
                if (leg.nodes[i - 1] == flaggedFrom && leg.nodes[i] == flaggedTo)
                    return true;
        return false;
    };
    if (check(walked.ok && flaggedFrom != TravelGraph::NO_NODE, "d) a walk edge to flag"))
    {
        StuckEdges once;
        once.flags[EdgeKey(flaggedFrom, flaggedTo)] = 1;
        Result const penalised = Search(graph, toIronforge, once, aStar);
        optimal(penalised, toIronforge, once, "d) one flag");
        check(penalised.ok && (penalised.totalSeconds > walked.totalSeconds || !uses(penalised)),
              "d) one flag neither costs more nor changes the route");
        StuckEdges thrice;
        thrice.flags[EdgeKey(flaggedFrom, flaggedTo)] = STUCK_SKIP_FLAGS;
        Result const avoided = Search(graph, toIronforge, thrice, aStar);
        check(!avoided.ok || !uses(avoided), "d) a skipped edge is used");

        StuckBook book;
        TravelGraphNode const& a = graph.GetNode(flaggedFrom);
        TravelGraphNode const& b = graph.GetNode(flaggedTo);
        StuckBook::Report const report{{a.mapId, a.x, a.y, a.z}, {b.mapId, b.x, b.y, b.z}};
        std::uint32_t const avoidMs = 600 * 1000;
        std::uint32_t const nowMs = 0xFFFFF000u;  // the clock wraps during the window
        std::shared_ptr<StuckEdges const> const before = book.Current();
        book.Push(report);
        check(book.Update(graph, nowMs, avoidMs) == 1, "d) the report flags no edge");
        std::shared_ptr<StuckEdges const> const flagged = book.Current();
        check(before->flags.empty() && flagged != before && flagged->flags.size() == 1 &&
                  flagged->flags.count(EdgeKey(flaggedFrom, flaggedTo)),
              "d) the book does not publish the flagged edge (the old snapshot must stay as it was)");
        Result const fromBook = Search(graph, toIronforge, *flagged, aStar);
        check(std::fabs(fromBook.totalSeconds - penalised.totalSeconds) <= 0.05f,
              "d) the book's flag costs differ from a hand-made flag");
        book.Update(graph, nowMs + avoidMs - 1000, avoidMs);
        check(book.Current()->flags.size() == 1, "d) the flag expires before the avoid window ends");
        book.Update(graph, nowMs + avoidMs + 1000, avoidMs);
        check(book.Current()->flags.empty(), "d) the flag does not expire after the avoid window");
        check(Search(graph, toIronforge, *book.Current(), aStar).totalSeconds == walked.totalSeconds,
              "d) the route is not back after the flag expired");
        book.Push({{0, 0.0f, 0.0f, 0.0f}, {0, 1.0f, 1.0f, 0.0f}});
        check(book.Update(graph, nowMs, avoidMs) == 0, "d) a destination off the graph flags an edge");
        // A follower's report names the edge: flagged although the positions match no node
        StuckBook exact;
        StuckBook::Report named;
        named.bot = {0, 0.0f, 0.0f, 0.0f};
        named.destination = {0, 1.0f, 1.0f, 0.0f};
        named.fromNode = flaggedFrom;
        named.toNode = flaggedTo;
        exact.Push(named);
        check(exact.Update(graph, nowMs, avoidMs) == 1 && exact.Current()->flags.size() == 1 &&
                  exact.Current()->flags.count(EdgeKey(flaggedFrom, flaggedTo)),
              "d) a report naming its edge does not flag exactly that edge");
        notes.push_back("d) flagged " + std::string(graph.GetName(flaggedFrom)) + " -> " +
                        std::string(graph.GetName(flaggedTo)) + ": " + num(penalised.totalSeconds) +
                        " s with one flag");
    }

    // e) Darnassus -> Stormwind leaves Kalimdor only by ship: no transport legs by default, one or more with
    //    allowTransport
    Snapshot elf = human;
    elf.position = darnassus;
    elf.hearth = darnassus;
    Request crossing{elf, stormwindInn};
    Result const stayed = Search(graph, crossing, StuckEdges{}, aStar);
    check(legCount(stayed, LegType::Transport) == 0, "e) a transport leg without allowTransport");
    notes.push_back(std::string("e) Darnassus -> Stormwind inn without transports: ") +
                    (stayed.ok ? describe(stayed) : FailureWord(stayed.failure)));
    crossing.snapshot.allowTransport = true;
    Result const sailed = Search(graph, crossing, StuckEdges{}, aStar);
    if (sound(sailed, crossing, "e) Darnassus -> Stormwind inn with transports"))
    {
        optimal(sailed, crossing, StuckEdges{}, "e)");
        // The merged graph's static portals (round 2c, WT55d) lead Kalimdor -> Stormwind without a vessel
        check(legCount(sailed, LegType::Transport) + legCount(sailed, LegType::StaticPortal) +
                      legCount(sailed, LegType::Portal) >=
                  1,
              "e) no transport or portal leg with allowTransport");
    }

    // f) Determinism and A* = Dijkstra on 50 random pairs (fixed seed), every flight node known, transports allowed,
    //    both teams, the hearth ready or cooling
    std::mt19937 random(20261109u);
    std::uniform_int_distribution<std::uint32_t> anyNode(0, static_cast<std::uint32_t>(graph.NodeCount() - 1));
    std::uint32_t pairs = 0;
    std::uint32_t mismatches = 0;
    std::uint32_t unstable = 0;
    std::uint64_t expandedAStar = 0;
    std::uint64_t expandedDijkstra = 0;
    double microsAStar = 0.0;
    double microsDijkstra = 0.0;
    using Clock = std::chrono::steady_clock;
    auto const micros = [](Clock::time_point since)
    { return std::chrono::duration<double, std::micro>(Clock::now() - since).count(); };
    for (std::uint32_t i = 0; i < 50; ++i)
    {
        TravelGraphNode const& a = graph.GetNode(anyNode(random));
        TravelGraphNode const& b = graph.GetNode(anyNode(random));
        Snapshot traveller = human;
        traveller.position = {a.mapId, a.x, a.y, a.z};
        traveller.team = static_cast<std::uint8_t>(i % 2);
        traveller.level = static_cast<std::uint8_t>(10 + (i * 7) % 71);
        traveller.money = 1000000;
        traveller.knownTaxi.fill(0xFFFFFFFFu);
        traveller.allowTransport = true;
        traveller.hearthCooldownMs = (i % 3) ? 0 : 20 * 60 * 1000;
        Request const pair{traveller, {b.mapId, b.x, b.y, b.z}};
        Clock::time_point const started = Clock::now();
        Result const first = Search(graph, pair, StuckEdges{}, aStar);
        microsAStar += micros(started);
        Result const again = Search(graph, pair, StuckEdges{}, aStar);
        Clock::time_point const reset = Clock::now();
        Result const reference = Search(graph, pair, StuckEdges{}, dijkstraParams);
        microsDijkstra += micros(reset);
        bool stable =
            first.ok == again.ok && first.totalSeconds == again.totalSeconds && first.legs.size() == again.legs.size();
        for (std::size_t leg = 0; stable && leg < first.legs.size(); ++leg)
            stable = first.legs[leg].nodes == again.legs[leg].nodes && first.legs[leg].type == again.legs[leg].type;
        unstable += stable ? 0 : 1;
        if (first.ok != reference.ok ||
            std::fabs(first.totalSeconds - reference.totalSeconds) > 0.001f * reference.totalSeconds + 0.05f)
        {
            ++mismatches;
            notes.push_back("f) mismatch: pair " + std::to_string(i) + " " + std::to_string(a.id) + " -> " +
                            std::to_string(b.id) + ": A* " + num(first.totalSeconds) + " s, Dijkstra " +
                            num(reference.totalSeconds) + " s");
        }
        pairs += first.ok ? 1 : 0;
        expandedAStar += first.expandedNodes;
        expandedDijkstra += reference.expandedNodes;
    }
    check(unstable == 0, "f) " + std::to_string(unstable) + " pairs gave different routes twice");
    check(mismatches == 0, "f) " + std::to_string(mismatches) + " A*/Dijkstra mismatches (see the notes)");
    check(pairs >= 25, "f) only " + std::to_string(pairs) + " of 50 random pairs have a route");
    notes.push_back("f) 50 random pairs: " + std::to_string(pairs) + " routed, " + std::to_string(mismatches) +
                    " A*/Dijkstra mismatches, expanded A* " + std::to_string(expandedAStar / 50) + " / Dijkstra " +
                    std::to_string(expandedDijkstra / 50) + " per search, " + num(microsAStar / 50.0) + " / " +
                    num(microsDijkstra / 50.0) + " us per search");

    // h) The fare budget. The core charges a taxi chain's total up front and every flight of a route draws on the same
    //    purse: find a route with a flight leg of several hops (every flight node known, thrift 0 so fares weigh no
    //    time and only the purse decides), then give the bot one copper less than the route's fares. Every hop alone
    //    is still affordable, yet the new route's flights must fit the purse. A route with two flight legs gets the
    //    same check when the random pairs find one
    auto const dearestHop = [&graph](Leg const& leg)
    {
        std::uint32_t dearest = 0;
        for (std::size_t i = 1; leg.type == LegType::Flight && i < leg.nodes.size(); ++i)
            for (TravelGraphLink const& link : graph.LinksOf(leg.nodes[i - 1]))
                if (link.to == leg.nodes[i])
                    if (TravelTaxiPath const* path = graph.TaxiPathOf(link))
                        dearest = std::max(dearest, path->price);
        return dearest;
    };
    Snapshot spender = human;
    spender.money = 1000000;
    spender.thrift = 0;
    spender.knownTaxi.fill(0xFFFFFFFFu);
    spender.hasHearthstone = false;
    std::mt19937 fareRandom(20261110u);
    bool chained = false;
    bool twoLegs = false;
    for (std::uint32_t i = 0; i < 600 && !(chained && twoLegs); ++i)
    {
        TravelGraphNode const& a = graph.GetNode(anyNode(fareRandom));
        TravelGraphNode const& b = graph.GetNode(anyNode(fareRandom));
        Request request{spender, {b.mapId, b.x, b.y, b.z}};
        request.snapshot.position = {a.mapId, a.x, a.y, a.z};
        request.snapshot.team = static_cast<std::uint8_t>(i % 2);
        Result const free = Search(graph, request, StuckEdges{}, aStar);
        std::uint32_t const total = RouteFare(free);
        std::uint32_t hop = 0;
        bool severalHops = false;
        for (Leg const& leg : free.legs)
        {
            hop = std::max(hop, dearestHop(leg));
            severalHops = severalHops || (leg.type == LegType::Flight && leg.taxiNodes.size() >= 3);
        }
        bool const several = legCount(free, LegType::Flight) >= 2;
        if (!free.ok || total <= hop || ((!severalHops || chained) && (!several || twoLegs)))
            continue;
        request.snapshot.money = total - 1;
        Result const tight = Search(graph, request, StuckEdges{}, aStar);
        bool legsFit = true;
        for (Leg const& leg : tight.legs)
            legsFit = legsFit && leg.fareCopper <= request.snapshot.money;
        std::string const label = std::string("h) ") + (severalHops ? "a chain of hops" : "") +
                                  (severalHops && several ? ", " : "") + (several ? "two flight legs" : "");
        check(legsFit && RouteFare(tight) <= request.snapshot.money,
              label + ": a route paying " + std::to_string(RouteFare(tight)) + "c from a purse of " +
                  std::to_string(request.snapshot.money) + "c");
        notes.push_back(label + " " + std::to_string(a.id) + " -> " + std::to_string(b.id) + ", dearest hop " +
                        std::to_string(hop) + "c, fares " + std::to_string(total) + "c: " + describe(free) +
                        " | purse " + std::to_string(request.snapshot.money) +
                        "c: " + (tight.ok ? describe(tight) : FailureWord(tight.failure)));
        chained = chained || severalHops;
        twoLegs = twoLegs || several;
    }
    check(chained, "h) no route with a flight leg of several hops among 600 random pairs");
    if (!twoLegs)
        notes.push_back("h) no route with two separate flight legs among the random pairs");
    // j) Start areas and avoided edges (WT10e-2): a level-1 bot in each race's start area walks to its first town (a
    //    route the graph lacks is noted), and an edge the trip avoids after deaths is not used
    struct StartArea
    {
        char const* name;
        std::uint8_t team;
        Point start;
        Point town;
    };
    StartArea const starts[] = {
        {"Northshire -> Goldshire", 0, {0, -8949.95f, -132.49f, 83.53f}, goldshire},
        {"Coldridge Valley -> Kharanos", 0, {0, -6240.32f, 331.03f, 382.76f}, {0, -5601.6f, -531.2f, 399.65f}},
        {"Shadowglen -> Dolanaar", 0, {1, 10311.3f, 832.46f, 1326.41f}, {1, 9886.0f, 974.0f, 1313.0f}},
        {"Ammen Vale -> Azure Watch", 0, {530, -3961.64f, -13931.2f, 100.62f}, {530, -4190.0f, -12470.0f, 44.0f}},
        {"Valley of Trials -> Razor Hill", 1, {1, -618.52f, -4251.67f, 38.72f}, {1, 320.0f, -4740.0f, 10.0f}},
        {"Camp Narache -> Bloodhoof Village", 1, {1, -2917.58f, -257.98f, 53.0f}, {1, -2300.0f, -400.0f, -9.0f}},
        {"Deathknell -> Brill", 1, {0, 1676.71f, 1678.31f, 121.67f}, {0, 2260.0f, 290.0f, 34.0f}},
        {"Sunstrider Isle -> Falconwing Square", 1, {530, 10349.6f, -6357.29f, 33.4f}, {530, 9500.0f, -6830.0f, 17.0f}},
    };
    for (StartArea const& area : starts)
    {
        Snapshot fresh;
        fresh.position = area.start;
        fresh.team = area.team;
        fresh.level = 1;
        Request const request{fresh, area.town};
        Result const route = Search(graph, request, StuckEdges{}, aStar);
        std::string const label = std::string("j) level 1, ") + area.name;
        if (route.ok)
            sound(route, request, label);
        else
            notes.push_back(label + ": no route (" + FailureWord(route.failure) + ")");
    }
    Snapshot veteran;
    veteran.position = {0, 2.7f, -857.9f, 58.9f};  // Tarren Mill
    veteran.team = 1;
    veteran.level = 80;
    veteran.money = 10000;
    Request const veteranTrip{veteran, {530, 9738.0f, -7454.0f, 13.6f}};  // Silvermoon
    Result const unbounded = Search(graph, veteranTrip, StuckEdges{}, aStar);
    if (sound(unbounded, veteranTrip, "j) level 80, Tarren Mill -> Silvermoon"))
    {
        Request avoiding = veteranTrip;
        for (Leg const& leg : unbounded.legs)
            if (leg.type == LegType::Walk && leg.nodes.size() >= 2 && avoiding.refused.empty())
                RefuseLink(avoiding, leg.nodes[1], leg.nodes[0]);  // given the other way round
        Result const around = Search(graph, avoiding, StuckEdges{}, aStar);
        Refusals const refusals = RefusalsOf(avoiding);
        bool used = false;
        for (Leg const& leg : around.legs)
            for (std::size_t i = 1; leg.type == LegType::Walk && i < leg.nodes.size(); ++i)
                used = used || refusals.AvoidsEdge(leg.nodes[i - 1], leg.nodes[i]);
        check(!avoiding.refused.empty() && !used, "j) an avoided edge is used");
        notes.push_back("j) avoided edge: " +
                        (around.ok ? describe(around) : std::string(FailureWord(around.failure))));
    }

    // k) Fly legs (WU10) over made-up flat height maps (ground 100 yd, open but where named) around Hellfire Peninsula
    //    (Honor Hold -> Thrallmar) and the Borean Tundra (Valiance Keep -> Fizzcrank Airstrip); the same request's
    //    ground route is the reference
    auto const nodeNear = [&graph](std::uint32_t mapId, float x, float y, float z) -> Point
    {
        std::vector<std::uint32_t> const nearest = graph.NearestNodes(mapId, x, y, z, 1);
        if (nearest.empty())
            return {TravelGraph::ANY_MAP, 0.0f, 0.0f, 0.0f};
        TravelGraphNode const& node = graph.GetNode(nearest.front());
        return {node.mapId, node.x, node.y, node.z};
    };
    Point const honorHold = nodeNear(FLY_MAP_OUTLAND, -683.0f, 2657.0f, 92.0f);
    Point const thrallmar = nodeNear(FLY_MAP_OUTLAND, 228.0f, 2633.0f, 87.0f);
    Point const valianceKeep = nodeNear(FLY_MAP_NORTHREND, 2272.0f, 5200.0f, 11.0f);
    Point const fizzcrank = nodeNear(FLY_MAP_NORTHREND, 4126.0f, 5303.0f, 28.0f);
    if (!check(honorHold.mapId == FLY_MAP_OUTLAND && thrallmar.mapId == FLY_MAP_OUTLAND &&
                   valianceKeep.mapId == FLY_MAP_NORTHREND && fizzcrank.mapId == FLY_MAP_NORTHREND,
               "k) fixture nodes on Outland and Northrend"))
        return checks;
    auto const groundMap = [](Point const& a, Point const& b, std::function<bool(float, float)> const& openAt,
                              std::function<float(float, float)> const& groundAt)
    {
        float const margin = 1500.0f;
        float const cell = 67.0f;
        std::int32_t const width = static_cast<std::int32_t>(std::ceil((std::fabs(a.x - b.x) + 2.0f * margin) / cell));
        std::int32_t const height = static_cast<std::int32_t>(std::ceil((std::fabs(a.y - b.y) + 2.0f * margin) / cell));
        return FlightRules::MakeMap(a.mapId, std::min(a.x, b.x) - margin, std::min(a.y, b.y) - margin, cell, width,
                                    height, groundAt, openAt);
    };
    auto const flatMap = [&groundMap](Point const& a, Point const& b, std::function<bool(float, float)> const& openAt)
    { return groundMap(a, b, openAt, [](float, float) { return 100.0f; }); };
    auto const mapSet = [](std::vector<FlightRules::HeightMap> maps)
    {
        auto set = std::make_shared<FlightRules::FlightMapSet>();
        set->maps = std::move(maps);
        return std::shared_ptr<FlightRules::FlightMapSet const>(std::move(set));
    };
    auto const flyLeg = [](Result const& route) -> Leg const*
    {
        for (Leg const& leg : route.legs)
            if (leg.type == LegType::Fly)
                return &leg;
        return nullptr;
    };
    auto const open = [](float, float) { return true; };
    auto const awayFrom = [](Point const& at, float x, float y) { return std::hypot(x - at.x, y - at.y); };

    Snapshot rider;
    rider.position = honorHold;
    rider.team = 0;
    rider.level = 70;
    rider.flyOutland = true;
    rider.flySpeed = 17.5f;  // 150% flying (riding 225)
    rider.flightStyle = FlightRules::BotFlightStyle(0.0f, 1001);
    Request const hellfire{rider, thrallmar};
    Params flying;
    flying.flightMaps = mapSet({flatMap(honorHold, thrallmar, open)});
    Request groundOnly = hellfire;  // the reference: the same bot with flying closed
    RefuseFlying(groundOnly);
    Result const ground = Search(graph, groundOnly, StuckEdges{}, aStar);
    Result const flownOver = Search(graph, hellfire, StuckEdges{}, flying);
    bool const routed = sound(ground, groundOnly, "k) Honor Hold -> Thrallmar on the ground");
    if (!routed || !sound(flownOver, hellfire, "k) Honor Hold -> Thrallmar flying"))
        return checks;
    Leg const* fly = flyLeg(flownOver);
    check(fly && legCount(flownOver, LegType::Fly) == 1, "k) no single fly leg on an open map");
    check(flownOver.totalSeconds < ground.totalSeconds, "k) flying " + num(flownOver.totalSeconds) +
                                                            " s is not quicker than walking " +
                                                            num(ground.totalSeconds) + " s");
    check(fly && fly->flightPoints.size() >= 3 && same(fly->flightPoints.front(), fly->from) &&
              same(fly->flightPoints.back(), fly->to) && fly->flightLevels >= 1 && fly->flightYards > 0.0f &&
              fly->flightClearance == rider.flightStyle.clearanceYards,
          "k) the fly leg's flight does not run from its start to its end");
    check(flownOver.flightSearches == 1, "k) " + std::to_string(flownOver.flightSearches) + " flight searches");
    notes.push_back("k) " + std::to_string(flownOver.flightSearches) + " flight searches in " +
                    num(flownOver.flightMicros) + " us");
    Params flyingDijkstra = flying;
    flyingDijkstra.heuristic = false;
    Result const flownReference = Search(graph, hellfire, StuckEdges{}, flyingDijkstra);
    check(std::fabs(flownReference.totalSeconds - flownOver.totalSeconds) <= 0.001f * flownOver.totalSeconds + 0.05f,
          "k) flying: A* " + num(flownOver.totalSeconds) + " s, Dijkstra " + num(flownReference.totalSeconds) + " s");
    // g) for fly legs: the same request twice gives the same legs and flights
    Result const flownAgain = Search(graph, hellfire, StuckEdges{}, flying);
    bool stable = flownAgain.legs.size() == flownOver.legs.size() && flownAgain.totalSeconds == flownOver.totalSeconds;
    for (std::size_t i = 0; stable && i < flownOver.legs.size(); ++i)
    {
        Leg const& a = flownOver.legs[i];
        Leg const& b = flownAgain.legs[i];
        stable = a.type == b.type && a.nodes == b.nodes && a.flightPoints.size() == b.flightPoints.size();
        for (std::size_t p = 0; stable && p < a.flightPoints.size(); ++p)
            stable = a.flightPoints[p].x == b.flightPoints[p].x && a.flightPoints[p].y == b.flightPoints[p].y &&
                     a.flightPoints[p].z == b.flightPoints[p].z;
    }
    check(stable, "k) the same request twice gave different fly legs");

    // WT61b, fly edges in the search. Without flight maps a flyer's route is priced with its fly legs, unflown
    Result const unflownOver = Search(graph, hellfire, StuckEdges{}, aStar);
    Leg const* unflownLeg = flyLeg(unflownOver);
    check(unflownOver.ok && legCount(unflownOver, LegType::Fly) == 1 && unflownLeg &&
              unflownLeg->flightPoints.empty() && unflownOver.flightSearches == 0,
          "k) without flight maps a flyer's route is not one unflown fly leg");
    // Between two outdoor points (Hellfire, west of Honor Hold -> the Temple of Telhamat, every flight master known)
    // the walker's route takes a taxi or a detour; a 280% flyer's is one fly leg, and a walker's is the one with flying
    // closed
    Snapshot swift = rider;
    swift.flySpeed = 7.0f * 3.8f;
    swift.money = 1000000;
    swift.knownTaxi.fill(0xFFFFFFFFu);
    Point const outdoorStart{FLY_MAP_OUTLAND, -815.9f, 2724.6f, 111.4f};
    Point const telhamat{FLY_MAP_OUTLAND, 199.1f, 4238.4f, 121.8f};
    swift.position = outdoorStart;
    Request const overTaxi{swift, telhamat};
    Request closedTaxi = overTaxi;
    RefuseFlying(closedTaxi);
    Snapshot walkerBot = swift;
    walkerBot.flyOutland = false;
    walkerBot.flySpeed = 0.0f;
    Request const walkerTrip{walkerBot, telhamat};
    Params wide;
    wide.flightMaps = mapSet({flatMap(outdoorStart, telhamat, open)});
    Result const taxiWalk = Search(graph, closedTaxi, StuckEdges{}, wide);
    Result const taxiFly = Search(graph, overTaxi, StuckEdges{}, wide);
    Result const walkerRoute = Search(graph, walkerTrip, StuckEdges{}, wide);
    if (sound(taxiWalk, closedTaxi, "k) Hellfire -> Telhamat, flying closed") &&
        sound(taxiFly, overTaxi, "k) Hellfire -> Telhamat flying") &&
        sound(walkerRoute, walkerTrip, "k) Hellfire -> Telhamat, a walker"))
    {
        float const straightWalk = Distance(outdoorStart, telhamat) / swift.runSpeed;
        check(legCount(taxiWalk, LegType::Flight) >= 1 || taxiWalk.totalSeconds > 1.3f * straightWalk,
              "k) fixture: the walk to Telhamat takes no taxi and no detour");
        check(taxiFly.legs.size() == 1 && legCount(taxiFly, LegType::Fly) == 1,
              "k) a flyer between two outdoor points: not one fly leg without a taxi or a walk");
        bool equal = walkerRoute.legs.size() == taxiWalk.legs.size() &&
                     std::fabs(walkerRoute.totalSeconds - taxiWalk.totalSeconds) < 0.001f;
        for (std::size_t i = 0; equal && i < walkerRoute.legs.size(); ++i)
            equal = walkerRoute.legs[i].type == taxiWalk.legs[i].type &&
                    walkerRoute.legs[i].nodes == taxiWalk.legs[i].nodes;
        check(equal, "k) a walker's route to Telhamat is not the flyer's with flying closed");
    }

    // A* = Dijkstra for flyers on 30 random pairs of Outland and Northrend nodes (no flight maps: fly edges priced,
    // unflown), and the search's cost for a flyer and a walker on the same pairs (expanded nodes, microseconds; the
    // walkers' seconds and legs summed, to compare builds)
    std::vector<std::uint32_t> flyNodes;
    for (std::uint32_t index = 0; index < graph.NodeCount(); ++index)
        if (graph.GetNode(index).mapId == FLY_MAP_OUTLAND || graph.GetNode(index).mapId == FLY_MAP_NORTHREND)
            flyNodes.push_back(index);
    if (check(!flyNodes.empty(), "k) no nodes on Outland and Northrend"))
    {
        std::mt19937 flyRandom(20261117u);
        std::uniform_int_distribution<std::size_t> anyFlyNode(0, flyNodes.size() - 1);
        std::uint32_t flyMismatches = 0;
        std::uint32_t flyRouted = 0;
        std::uint64_t flyExpanded = 0;
        std::uint64_t walkExpanded = 0;
        double flyMicros = 0.0;
        double walkMicros = 0.0;
        double walkSeconds = 0.0;
        std::size_t walkLegs = 0;
        for (std::uint32_t i = 0; i < 30; ++i)
        {
            TravelGraphNode const& a = graph.GetNode(flyNodes[anyFlyNode(flyRandom)]);
            TravelGraphNode const& b = graph.GetNode(flyNodes[anyFlyNode(flyRandom)]);
            Snapshot pairBot = swift;
            pairBot.position = {a.mapId, a.x, a.y, a.z};
            pairBot.team = static_cast<std::uint8_t>(i % 2);
            pairBot.level = 80;
            pairBot.flyNorthrend = true;
            pairBot.allowTransport = true;
            Snapshot pairWalker = pairBot;
            pairWalker.flySpeed = 0.0f;
            Request const flyPair{pairBot, {b.mapId, b.x, b.y, b.z}};
            Request const walkPair{pairWalker, {b.mapId, b.x, b.y, b.z}};
            Clock::time_point const started = Clock::now();
            Result const flyRoute = Search(graph, flyPair, StuckEdges{}, aStar);
            flyMicros += micros(started);
            Clock::time_point const walkStarted = Clock::now();
            Result const walkRoute = Search(graph, walkPair, StuckEdges{}, aStar);
            walkMicros += micros(walkStarted);
            Result const flyReference = Search(graph, flyPair, StuckEdges{}, dijkstraParams);
            if (flyRoute.ok != flyReference.ok || std::fabs(flyRoute.totalSeconds - flyReference.totalSeconds) >
                                                      0.001f * flyReference.totalSeconds + 0.05f)
            {
                ++flyMismatches;
                notes.push_back("k) flyer mismatch: pair " + std::to_string(i) + " " + std::to_string(a.id) + " -> " +
                                std::to_string(b.id) + ": A* " + num(flyRoute.totalSeconds) + " s, Dijkstra " +
                                num(flyReference.totalSeconds) + " s");
            }
            flyRouted += flyRoute.ok ? 1 : 0;
            flyExpanded += flyRoute.expandedNodes;
            walkExpanded += walkRoute.expandedNodes;
            walkSeconds += walkRoute.ok ? walkRoute.totalSeconds : 0.0f;
            walkLegs += walkRoute.legs.size();
        }
        check(flyMismatches == 0, "k) " + std::to_string(flyMismatches) + " flyer A*/Dijkstra mismatches");
        notes.push_back("k) 30 flyer pairs: " + std::to_string(flyRouted) + " routed, " +
                        std::to_string(flyMismatches) + " mismatches, expanded flyer " +
                        std::to_string(flyExpanded / 30) + " / walker " + std::to_string(walkExpanded / 30) +
                        " per search, " + num(flyMicros / 30.0) + " / " + num(walkMicros / 30.0) +
                        " us per search; walkers " + num(walkSeconds) + " s, " + std::to_string(walkLegs) + " legs");
    }

    // A bot that cannot take off where it stands (indoors, in water, a WMO no-fly area over open cells) walks to the
    // leg's first graph node and flies from there
    Snapshot indoors = rider;
    indoors.takeoffHere = false;
    Request const fromInside{indoors, thrallmar};
    Result const walkedOut = Search(graph, fromInside, StuckEdges{}, flying);
    if (sound(walkedOut, fromInside, "k) Honor Hold indoors -> Thrallmar flying"))
    {
        Leg const* out = flyLeg(walkedOut);
        check(out && walkedOut.legs.front().type == LegType::Walk && same(walkedOut.legs.front().from, honorHold) &&
                  awayFrom(honorHold, out->from.x, out->from.y) > FLY_OWN_SPOT_YARDS &&
                  same(out->flightPoints.front(), out->from),
              "k) no takeoff here: the fly leg does not start at the first graph node after a walk");
        if (out)
            notes.push_back("k) no takeoff at Honor Hold: walk " + num(walkedOut.legs.front().seconds) +
                            " s, then fly " + num(out->seconds) + " s from " +
                            num(awayFrom(honorHold, out->from.x, out->from.y)) + " yd out");
    }

    // The replan after a takeoff refused at a node (WU11b: Dalaran's portal node lies over open cells): the bot
    // stands where its walk ended, 4 yd from the node, and no flight starts there. The trip runs from Thrallmar
    // indoors, whose walk passes two graph nodes after the refused one: the walk from Honor Hold has only one left,
    // 110 yd short of Thrallmar, a ground hop (since l)'s priced climb it no longer ends at the node 193 yd out)
    Snapshot inThrallmar = indoors;
    inThrallmar.position = thrallmar;
    Request const fromThrallmar{inThrallmar, honorHold};
    Result const refusedAt = Search(graph, fromThrallmar, StuckEdges{}, flying);
    Leg const* refused =
        sound(refusedAt, fromThrallmar, "k) Thrallmar indoors -> Honor Hold flying") ? flyLeg(refusedAt) : nullptr;
    check(refused && awayFrom(thrallmar, refused->from.x, refused->from.y) > FLY_OWN_SPOT_YARDS,
          "k) no takeoff in Thrallmar: no fly leg after a walk");
    if (refused)
    {
        Snapshot atNode = inThrallmar;
        atNode.position = {refused->from.mapId, refused->from.x + 4.0f, refused->from.y, refused->from.z};
        Request const replan{atNode, honorHold};
        Result const onward = Search(graph, replan, StuckEdges{}, flying);
        Leg const* again = sound(onward, replan, "k) replan at the refused node") ? flyLeg(onward) : nullptr;
        check(again && awayFrom(refused->from, again->from.x, again->from.y) > FLY_OWN_SPOT_YARDS,
              "k) a replan at a node where the takeoff was refused flies from it again");
        if (again)
            notes.push_back("k) replan at the refused node: fly " + num(again->seconds) + " s from " +
                            num(awayFrom(refused->from, again->from.x, again->from.y)) + " yd on");
    }

    // No-fly ground at each end: the start's own cell (the start 40 yd west and 40 yd south of Honor Hold's node, so
    //    the walk's next node, 50 yd off, lies outside it: the map's origin lies 1500 yd west of that node, so the
    //    start falls in the next cell west) and 60 yd around Thrallmar's node (the merged graph's walk passes one node
    //    90 yd out). Walk to the first open node, fly, walk in
    Point const westOfHold{FLY_MAP_OUTLAND, honorHold.x - 40.0f, honorHold.y - 40.0f, honorHold.z};
    Params towns;
    towns.flightMaps = mapSet({flatMap(honorHold, thrallmar,
                                       [&](float x, float y)
                                       {
                                           bool const startCell = std::fabs(x - westOfHold.x) <= 33.5f &&
                                                                  std::fabs(y - westOfHold.y) <= 33.5f;
                                           return !startCell && awayFrom(thrallmar, x, y) > 60.0f;
                                       })});
    Snapshot townRider = rider;
    townRider.position = westOfHold;
    Request const fromWest{townRider, thrallmar};
    Request westOnGround = fromWest;
    RefuseFlying(westOnGround);
    Result const groundWest = Search(graph, westOnGround, StuckEdges{}, aStar);
    Result const fromTown = Search(graph, fromWest, StuckEdges{}, towns);
    if (sound(groundWest, westOnGround, "k) west of Honor Hold -> Thrallmar on the ground") &&
        sound(fromTown, fromWest, "k) no-fly ground at both ends"))
    {
        Leg const* townFly = flyLeg(fromTown);
        FlightRules::HeightMap const& townMap = towns.flightMaps->maps.front();
        check(fromTown.legs.size() >= 3 && fromTown.legs.front().type == LegType::Walk &&
                  fromTown.legs.back().type == LegType::Walk && legCount(fromTown, LegType::Fly) == 1 && townFly &&
                  townMap.IsOpen(townFly->from.x, townFly->from.y) && townMap.IsOpen(townFly->to.x, townFly->to.y),
              "k) no-fly ends: not walk, fly, walk with open fly ends");
        check(fromTown.totalSeconds < groundWest.totalSeconds, "k) no-fly ends: flying is not quicker");
    }

    // A floating platform (WU11b, Dalaran): a point more than FLY_PLATFORM_YARDS above its cell's ground is no fly
    // end. The ground lies 400 yd lower within platformYards of Thrallmar (cell centres: the walk's last graph node,
    // 90 yd out in the merged graph, stays on the plain since l)'s priced climb), so its nodes there float: the
    // flight ends before them and the bot walks the rest
    float const platformYards = 60.0f;
    // Whether a point lies on the platform is read back from the map (a sunk cell), not from the radius: the cells
    // are 67 yd wide, so wherever the graph's nodes lie a fly end is never on the platform (FlyEnd is false there)
    auto const sunkAt = [](FlightRules::HeightMap const& map, float x, float y)
    { return map.IsOpen(x, y) && map.Ground(map.CellX(x), map.CellY(y)) < 0; };
    FlightRules::HeightMap const plain = flatMap(honorHold, thrallmar, open);
    check(FlyEnd(plain, {FLY_MAP_OUTLAND, honorHold.x, honorHold.y, 120.0f}) &&
              !FlyEnd(plain, {FLY_MAP_OUTLAND, honorHold.x, honorHold.y, 100.0f + FLY_PLATFORM_YARDS + 1.0f}),
          "k) platform: a point 20 yd above the ground is no fly end, or one 101 yd above is one");
    Params platform;
    platform.flightMaps =
        mapSet({groundMap(honorHold, thrallmar, open, [&](float x, float y)
                          { return awayFrom(thrallmar, x, y) <= platformYards ? -400.0f : 100.0f; })});
    Result const toPlatform = Search(graph, hellfire, StuckEdges{}, platform);
    if (sound(toPlatform, hellfire, "k) Honor Hold -> Thrallmar on a platform"))
    {
        Leg const* high = flyLeg(toPlatform);
        FlightRules::HeightMap const& sunk = platform.flightMaps->maps.front();
        check(high && toPlatform.legs.back().type == LegType::Walk && FlyEnd(sunk, high->from) &&
                  FlyEnd(sunk, high->to) && !sunkAt(sunk, high->to.x, high->to.y) &&
                  sunkAt(sunk, thrallmar.x, thrallmar.y),
              "k) platform: the fly leg does not end below the platform before a walk up");
        if (high)
            notes.push_back("k) platform at Thrallmar: the flight ends " +
                            num(awayFrom(thrallmar, high->to.x, high->to.y)) + " yd out, " +
                            num(toPlatform.legs.back().seconds) + " s walk");
    }

    // Takeoff spots on that platform (WU11b fix 7, Krasus' Landing): a bot on the platform walks straight to the spot
    // 40 yd off Thrallmar and flies from it; a flight to the platform lands there and walks in. Without a spot, or with
    // the bot standing at the spot unable to take off, the flight starts beyond the platform. The spot lies on the
    // way to Honor Hold, so that no graph node just beyond the platform is a nearer way to the flight than the spot
    float const toSpot = 40.0f / awayFrom(thrallmar, honorHold.x, honorHold.y);
    Point const spotAt{FLY_MAP_OUTLAND, thrallmar.x + (honorHold.x - thrallmar.x) * toSpot,
                       thrallmar.y + (honorHold.y - thrallmar.y) * toSpot, thrallmar.z};
    Params spotted;
    {
        FlightRules::HeightMap withSpot = platform.flightMaps->maps.front();
        withSpot.takeoffs.push_back({spotAt.x, spotAt.y, spotAt.z});
        spotted.flightMaps = mapSet({withSpot});
    }
    check(sunkAt(platform.flightMaps->maps.front(), spotAt.x, spotAt.y),
          "k) takeoff spot: the fixture's spot lies on the platform");
    Snapshot onPlatform = rider;
    onPlatform.position = thrallmar;
    onPlatform.takeoffHere = false;
    Request const fromPlatform{onPlatform, honorHold};
    auto const viaSpot = [&](Result const& route, std::size_t walkIdx, std::size_t flyIdx)
    {
        return route.legs.size() > std::max(walkIdx, flyIdx) && route.legs[walkIdx].type == LegType::Walk &&
               route.legs[walkIdx].nodes.empty() && route.legs[flyIdx].type == LegType::Fly;
    };
    Result const offSpot = Search(graph, fromPlatform, StuckEdges{}, spotted);
    if (sound(offSpot, fromPlatform, "k) Thrallmar on a platform with a takeoff spot -> Honor Hold"))
        check(viaSpot(offSpot, 0, 1) && same(offSpot.legs[0].from, thrallmar) && same(offSpot.legs[0].to, spotAt) &&
                  same(offSpot.legs[1].from, spotAt),
              "k) takeoff spot: the bot does not walk straight to the spot and fly from it");
    Result const ontoSpot = Search(graph, hellfire, StuckEdges{}, spotted);
    if (sound(ontoSpot, hellfire, "k) Honor Hold -> Thrallmar on a platform with a takeoff spot"))
    {
        std::size_t const n = ontoSpot.legs.size();
        check(n >= 2 && viaSpot(ontoSpot, n - 1, n - 2) && same(ontoSpot.legs[n - 2].to, spotAt) &&
                  same(ontoSpot.legs[n - 1].from, spotAt) && same(ontoSpot.legs[n - 1].to, thrallmar),
              "k) takeoff spot: the flight onto the platform does not land at the spot and walk in");
    }
    auto const offBeyond = [&](Request const& request, Params const& params, std::string const& label)
    {
        Result const walked = Search(graph, request, StuckEdges{}, params);
        if (!sound(walked, request, "k) " + label))
            return;
        Leg const* off = flyLeg(walked);
        check(off && walked.legs.front().type == LegType::Walk && !walked.legs.front().nodes.empty() &&
                  !sunkAt(platform.flightMaps->maps.front(), off->from.x, off->from.y),
              "k) " + label + ": the flight does not start beyond the platform");
    };
    Request mayTakeOff = fromPlatform;
    mayTakeOff.snapshot.takeoffHere = true;
    offBeyond(mayTakeOff, platform, "a platform without a spot");
    Request atSpot = fromPlatform;
    atSpot.snapshot.position = {FLY_MAP_OUTLAND, spotAt.x + 3.0f, spotAt.y, spotAt.z};
    offBeyond(atSpot, spotted, "the bot at the spot, unable to take off");

    // Dalaran on the shipped graph (WU11b fix 7): the walk from Runeweaver Square to Wintergarde Keep steps down from
    // the city to Crystalsong Forest, which no bot can walk. Over a flat map (ground 150 yd) with the takeoff spot of
    // Krasus' Landing (taxi node 310) the bot walks there straight and flies from it; without the spot no fly leg
    // starts on the city's platform
    Point const runeweaver{FLY_MAP_NORTHREND, 5818.7f, 642.9f, 647.9f};
    Point const wintergarde{FLY_MAP_NORTHREND, 3712.4f, -694.9f, 215.4f};
    Point const krasus{FLY_MAP_NORTHREND, 5813.9f, 449.1f, 658.8f};
    Snapshot dalaran = rider;
    dalaran.position = runeweaver;
    dalaran.level = 77;
    dalaran.flyNorthrend = true;
    dalaran.takeoffHere = false;
    Request const fromDalaran{dalaran, wintergarde};
    FlightRules::HeightMap crystalsongMap =
        groundMap(runeweaver, wintergarde, open, [](float, float) { return 150.0f; });
    Params crystalsong;
    crystalsong.flightMaps = mapSet({crystalsongMap});
    crystalsongMap.takeoffs.push_back({krasus.x, krasus.y, krasus.z});
    Params landing;
    landing.flightMaps = mapSet({crystalsongMap});
    Result const dalaranBare = Search(graph, fromDalaran, StuckEdges{}, crystalsong);
    Result const dalaranFlown = Search(graph, fromDalaran, StuckEdges{}, landing);
    // The merged graph has no walk off Dalaran's platform to Crystalsong (WT61b); since round 6 (navigation NV32, G88)
    // an air link leaves Krasus' Landing, where the engine allows flight, so without the spot a fly leg may start there
    // and nowhere else on the platform
    Leg const* bare = flyLeg(dalaranBare);
    check(!bare || FlyEnd(crystalsongMap, bare->from) || awayFrom(krasus, bare->from.x, bare->from.y) <= 10.0f,
          "k) Dalaran without a takeoff spot: a fly leg starts on the city's platform");
    notes.push_back(std::string("k) Runeweaver Square -> Wintergarde without a takeoff spot: ") +
                    (dalaranBare.ok ? describe(dalaranBare) : FailureWord(dalaranBare.failure)));
    if (sound(dalaranFlown, fromDalaran, "k) Runeweaver Square -> Wintergarde via Krasus' Landing"))
    {
        check(viaSpot(dalaranFlown, 0, 1) && same(dalaranFlown.legs[0].from, runeweaver) &&
                  same(dalaranFlown.legs[0].to, krasus) && same(dalaranFlown.legs[1].from, krasus),
              "k) Dalaran: the bot does not walk to Krasus' Landing and fly from there");
        if (viaSpot(dalaranFlown, 0, 1))
            notes.push_back("k) Dalaran: walk " + num(dalaranFlown.legs[0].seconds) + " s to Krasus' Landing, fly " +
                            num(dalaranFlown.legs[1].seconds) + " s, " + num(dalaranFlown.totalSeconds) + " s in all");
    }

    // A hop under 150 yd stays on the ground, straight (90 yd) or through nodes (140 yd), without a flight search
    for (float const yards : {90.0f, 140.0f})
    {
        Request const hop{rider, {FLY_MAP_OUTLAND, honorHold.x + yards, honorHold.y, honorHold.z}};
        Result const hopped = Search(graph, hop, StuckEdges{}, flying);
        check(hopped.ok && legCount(hopped, LegType::Fly) == 0 && hopped.flightSearches == 0,
              "k) a hop of " + num(yards) + " yd is flown or searched");
    }

    // The fallbacks keep the ground route: flying refused, no flying speed, no height map of the bot's map, a closed
    // map, and a goal on a void-ringed island (open within 150 yd, closed from 150 to 450 yd: the flight search finds
    // no path)
    //    (each failed flight closes its fly edge's ends, FLY_TRIES searches, then the last one walks)
    auto const walksAsGround = [&](Request const& request, Params const& params, std::string const& label, bool failing)
    {
        Result const kept = Search(graph, request, StuckEdges{}, params);
        bool const searches =
            failing ? kept.flightSearches >= 1 && kept.flightSearches <= FLY_TRIES : kept.flightSearches == 0;
        check(kept.ok && legCount(kept, LegType::Fly) == 0 && searches &&
                  std::fabs(kept.totalSeconds - ground.totalSeconds) < 0.01f,
              "k) " + label + ": not the ground route (" + std::to_string(kept.flightSearches) + " searches)");
    };
    Request grounded = hellfire;
    RefuseFlying(grounded);
    walksAsGround(grounded, flying, "flying refused", false);
    Request slow = hellfire;
    slow.snapshot.flySpeed = 0.0f;
    walksAsGround(slow, flying, "no flying speed", false);
    Params elsewhere;
    elsewhere.flightMaps = mapSet({flatMap(valianceKeep, fizzcrank, open)});
    walksAsGround(hellfire, elsewhere, "no Outland height map", false);
    Params closed;
    closed.flightMaps = mapSet({flatMap(honorHold, thrallmar, [](float, float) { return false; })});
    walksAsGround(hellfire, closed, "a closed map", false);
    Params ringed;
    ringed.flightMaps = mapSet({flatMap(honorHold, thrallmar,
                                        [&](float x, float y)
                                        {
                                            float const yards = awayFrom(thrallmar, x, y);
                                            return yards < 150.0f || yards > 450.0f;
                                        })});
    walksAsGround(hellfire, ringed, "a void-ringed goal", true);

    // Northrend: without Cold Weather Flying the snapshot allows no flying there; with it the tundra is flown
    Params both;
    both.flightMaps = mapSet({flatMap(honorHold, thrallmar, open), flatMap(valianceKeep, fizzcrank, open)});
    Snapshot northerner = rider;
    northerner.position = valianceKeep;
    Request const borean{northerner, fizzcrank};
    Result const noColdWeather = Search(graph, borean, StuckEdges{}, both);
    if (sound(noColdWeather, borean, "k) Valiance Keep -> Fizzcrank without Cold Weather Flying"))
        check(legCount(noColdWeather, LegType::Fly) == 0 && noColdWeather.flightSearches == 0,
              "k) Northrend without Cold Weather Flying: a fly leg");
    Request coldWeather = borean;
    coldWeather.snapshot.flyNorthrend = true;
    Result const flownNorth = Search(graph, coldWeather, StuckEdges{}, both);
    if (sound(flownNorth, coldWeather, "k) Valiance Keep -> Fizzcrank with Cold Weather Flying") && noColdWeather.ok)
        check(legCount(flownNorth, LegType::Fly) == 1 && flownNorth.totalSeconds < noColdWeather.totalSeconds,
              "k) Northrend with Cold Weather Flying: no quicker fly leg");

    // l) Walks off the graph are proposed by distance alone (WT56): from Thunder Bluff's foot to its mesa (53 yd
    //    across, 68 yd up) and back, the route is one direct walk priced as a run over its distance. From the
    //    elevator scenario's start (74 yd across, 66 yd up) both ends are at nodes: the route is graph links. The bot's
    //    map thread probes it on the navmesh and a refusal replans without it (route-rules-selftest n). Without the
    //    direct walk the route runs through nodes or there is none; a goal beyond DIRECT_YARDS is no direct walk
    Point const bluffFoot{1, -1290.7f, 172.0f, 67.4f};
    Point const liftStart{1, -1279.9f, 195.38f, 69.44f};
    Point const bluffTop{1, -1274.74f, 121.51f, 134.98f};
    Snapshot climber;
    climber.position = bluffFoot;
    climber.team = 1;
    climber.level = 10;
    climber.money = 10000;
    climber.allowTransport = true;
    auto const directWalk = [](Result const& route)
    {
        return route.ok && route.legs.size() == 1 && route.legs.front().type == LegType::Walk &&
               route.legs.front().nodes.empty();
    };
    Result const climbed = Search(graph, Request{climber, bluffTop}, StuckEdges{}, aStar);
    check(directWalk(climbed), "l) Thunder Bluff foot -> mesa is not one direct walk");
    notes.push_back("l) Thunder Bluff foot -> mesa: " +
                    (climbed.ok ? describe(climbed) : std::string(FailureWord(climbed.failure))));
    Snapshot onTop = climber;
    onTop.position = bluffTop;
    check(directWalk(Search(graph, Request{onTop, bluffFoot}, StuckEdges{}, aStar)),
          "l) Thunder Bluff mesa -> foot is not one direct walk");
    climber.position = liftStart;
    Result const lifted = Search(graph, Request{climber, bluffTop}, StuckEdges{}, aStar);
    check(lifted.ok && !directWalk(lifted), "l) the elevator scenario's start -> mesa, both at nodes: a direct walk");
    check(std::fabs(WalkSeconds(liftStart, bluffTop, 7.0f) - Distance(liftStart, bluffTop) / 7.0f) < 0.01f,
          "l) a walk off the graph is priced by more than its distance");
    climber.position = bluffFoot;
    Request refusedDirect{climber, bluffTop};
    RefuseWalk(refusedDirect, bluffFoot, bluffTop);  // the record's walk entry of the direct walk
    Result const around = Search(graph, refusedDirect, StuckEdges{}, aStar);
    check(!directWalk(around), "l) without the direct walk the route still walks directly");
    notes.push_back("l) Thunder Bluff foot -> mesa without the direct walk: " +
                    (around.ok ? describe(around) : std::string(FailureWord(around.failure))));
    check(!directWalk(Search(graph, Request{climber, {1, bluffFoot.x + 150.0f, bluffFoot.y, bluffFoot.z}}, StuckEdges{},
                             aStar)),
          "l) a goal 150 yd off is one direct walk");
    return checks;
}
}  // namespace TravelRoute

#endif
