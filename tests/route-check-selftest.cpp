/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of `playerbots route check links` (src/Mgr/Travel/RouteCheckRules.h): the classing of every
// path type, the chaining of long links, the report keys and lines.
// Usage: route-check-selftest
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
    Vec const atEnd{99.0f, 1.0f, 12.0f};
    Vec const halfway{50.0f, 0.0f, 10.0f};
    Vec const nowhere{0.5f, 0.0f, 10.0f};

    // One answer, every path type
    check(ClassifyHop(PATH_NORMAL, start, atEnd, end) == HopOutcome::Arrived, "0x01 at the end: arrived");
    check(ClassifyHop(PATH_NORMAL, start, Vec{99.0f, 0.0f, 30.0f}, end) == HopOutcome::Continue,
          "0x01 at the end's x,y but another floor: not arrived");
    check(ClassifyHop(PATH_NORMAL, start, halfway, end) == HopOutcome::Continue, "0x01 short of the end: continue");
    check(ClassifyHop(PATH_NORMAL, start, nowhere, end) == HopOutcome::Incomplete, "0x01 without progress: incomplete");
    check(ClassifyHop(PATH_NORMAL | PATH_NOT_USING_PATH, start, end, end) == HopOutcome::Fallback,
          "0x11 (no tile, or no polygon near an end): fallback even at the end");
    check(ClassifyHop(PATH_SHORTCUT, start, end, end) == HopOutcome::Fallback, "0x02: fallback");
    check(ClassifyHop(PATH_NOPATH, start, end, end) == HopOutcome::Fallback, "0x08: fallback");
    check(ClassifyHop(PATH_SHORTCUT | PATH_NOPATH, start, end, end) == HopOutcome::Fallback, "0x0a: fallback");
    check(ClassifyHop(0, start, end, end) == HopOutcome::Fallback, "0x00 (invalid coordinates): fallback");
    check(ClassifyHop(PATH_INCOMPLETE, start, halfway, end) == HopOutcome::Continue, "0x04 with progress: continue");
    check(ClassifyHop(PATH_INCOMPLETE, start, nowhere, end) == HopOutcome::Incomplete,
          "0x04 without progress: incomplete");
    check(ClassifyHop(PATH_INCOMPLETE, start, atEnd, end) == HopOutcome::Continue,
          "0x04 even at the end is no arrival (the next answer decides)");
    check(ClassifyHop(PATH_INCOMPLETE | 0x80, start, halfway, end) == HopOutcome::Continue,
          "0x84 (end far from a polygon) with progress: continue");
    check(ClassifyHop(PATH_SHORTCUT | PATH_SHORT, start, end, end) == HopOutcome::Capped,
          "0x22 (too many points): capped");
    check(ClassifyHop(PATH_NORMAL | PATH_SHORT, start, end, end) == HopOutcome::Capped, "0x21: capped");
    check(ClassifyHop(PATH_NORMAL | 0x40, start, atEnd, end) == HopOutcome::Continue,
          "0x41 (start far from a polygon) is not exactly NORMAL: no arrival");

    // Chaining
    auto const script = [](std::vector<Hop> answers)
    {
        return [answers, next = std::size_t{0}](Vec const&, Vec const&) mutable
        { return next < answers.size() ? answers[next++] : Hop{PATH_NOPATH, {}}; };
    };
    LinkResult result = Chain(start, end, script({{PATH_NORMAL, atEnd}}));
    check(result.cls == LinkClass::Real && result.hops == 1, "one answer to the end: real, 1 hop");
    result =
        Chain(start, end,
              script({{PATH_INCOMPLETE, Vec{40, 0, 10}}, {PATH_INCOMPLETE, Vec{80, 0, 10}}, {PATH_NORMAL, atEnd}}));
    check(result.cls == LinkClass::Real && result.hops == 3, "long link chained over 3 answers: real");
    result = Chain(start, end, script({{PATH_INCOMPLETE, Vec{40, 0, 10}}, {PATH_INCOMPLETE, Vec{40.5f, 0, 10}}}));
    check(result.cls == LinkClass::Incomplete && result.hops == 2 && result.reached.x == 40.5f,
          "a chain that stops gaining: incomplete where it stuck");
    result = Chain(start, end, script({{PATH_INCOMPLETE, Vec{40, 0, 10}}, {PATH_NORMAL | PATH_NOT_USING_PATH, end}}));
    check(result.cls == LinkClass::Fallback && result.hops == 2 && result.lastType == 0x11,
          "a fallback in a later answer: fallback");
    result = Chain(start, end, script({{PATH_SHORTCUT | PATH_SHORT, end}}));
    check(result.cls == LinkClass::Capped, "too many points: capped");
    std::vector<Hop> many;
    for (int i = 1; i <= 50; ++i)
        many.push_back({PATH_INCOMPLETE, Vec{static_cast<float>(i), 0, 10}});
    result = Chain(start, end, script(many));
    check(result.cls == LinkClass::Capped && result.hops == MAX_HOPS, "MAX_HOPS answers without arrival: capped");
    std::vector<Vec> asked;
    Chain(start, end,
          [&](Vec const& at, Vec const&)
          {
              asked.push_back(at);
              return asked.size() == 1 ? Hop{PATH_INCOMPLETE, Vec{60, 5, 11}} : Hop{PATH_NORMAL, atEnd};
          });
    check(asked.size() == 2 && asked[1].x == 60.0f && asked[1].z == 11.0f, "the next answer starts at the last point");

    // Keys and lines
    check(NodeKey(530, -1234.4f, 5678.5f, -0.4f) == "530:node:-1234,5679,0", "node key rounds to a yard");
    check(NodeKey(0, -0.6f, 0.4f, 12.49f) == "0:node:-1,0,12", "node key rounds negative halves away");
    std::string const fromKey = NodeKey(1, 1, 2, 3);
    std::string const toKey = NodeKey(1, 4, 5, 6);
    check(LinkKey(fromKey, toKey) == "1:node:1,2,3>1:node:4,5,6:walk", "link key");
    LinkResult broken{LinkClass::Fallback, 2, 0x11, Vec{12.4f, -3.6f, 7.0f}};
    check(BrokenValue(broken, true, false) == "fallback type=0x11 hop=2 at=12,-4,7 notile", "broken row value");
    check(OffMeshValue(2.04f, true) == "2.0 steep" && OffMeshValue(-1.0f, false) == "none", "off-mesh row value");

    MapCounts a;
    a.mapId = 571;
    a.links = 3;
    a.byClass[0] = 1;
    a.byClass[1] = 1;
    a.byClass[3] = 1;
    a.nodes = 2;
    a.offMesh = 1;
    MapCounts b;
    b.mapId = 0;
    b.links = 2;
    b.byClass[0] = 2;
    b.chained = 1;
    b.nodes = 2;
    std::vector<std::string> rows = {Line("nodes.offmesh", NodeKey(571, 1, 1, 1), "1.5"),
                                     Line("links.broken", LinkKey(fromKey, toKey), "incomplete type=0x04")};
    std::vector<std::string> const lines = ReportLines({a, b}, rows);
    check(std::is_sorted(lines.begin(), lines.end()), "report sorted");
    auto const has = [&](std::string const& line)
    { return std::find(lines.begin(), lines.end(), line) != lines.end(); };
    check(has("3\tlinks.checked\tall\t5") && has("3\tlinks.checked\t0\t2") && has("3\tlinks.checked\t571\t3"),
          "links.checked per map and in total");
    check(has("3\tlinks.class.real\tall\t3") && has("3\tlinks.class.capped\t571\t1") &&
              has("3\tlinks.class.incomplete\tall\t1") && has("3\tlinks.chained\tall\t1"),
          "class counts");
    check(has("3\tnodes.class.offmesh\tall\t1") && has("3\tnodes.checked\tall\t4"), "node counts");
    check(has("3\tstatus\t-\tfail"), "status fails with a broken link or an off-mesh node");
    check(has(rows[0]) && has(rows[1]), "rows kept");
    MapCounts clean = b;
    std::vector<std::string> const passing = ReportLines({clean}, {});
    check(std::find(passing.begin(), passing.end(), "3\tstatus\t-\tpass") != passing.end(),
          "status passes; capped links do not fail it");
    a.Add(b);
    check(a.Broken() == 1 && a.links == 5, "a capped link is not broken");
    std::string const summary = SummaryLine("map 571", a);
    check(summary.find("5 links: 3 real (1 chained), 1 incomplete, 0 fallback, 1 capped") != std::string::npos,
          "summary line");
    std::cout << "  " << summary << "\n";

    std::cout << "route-check-selftest: " << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
