/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of `playerbots route check links|pairs` (src/Mgr/Travel/RouteCheckRules.h): the classing of every
// path type, the chaining of long links, the verdict of a link from both directions, the report keys and lines.
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
    // A link's verdict from both directions
    LinkResult const real{LinkClass::Real, 1, PATH_NORMAL, Vec{99, 0, 10}};
    LinkResult const stuckAtA{LinkClass::Incomplete, 1, PATH_INCOMPLETE, Vec{0.5f, 0, 10}};
    LinkResult const stuckAtB{LinkClass::Fallback, 1, PATH_NORMAL | PATH_NOT_USING_PATH, Vec{100, 0, 10}};
    LinkResult const stuckMid{LinkClass::Incomplete, 2, PATH_INCOMPLETE, Vec{50, 0, 10}};
    LinkResult const capped{LinkClass::Capped, MAX_HOPS, PATH_INCOMPLETE, Vec{60, 0, 10}};
    Vec const nodeA{0, 0, 10};
    Vec const nodeB{100, 0, 10};
    check(ClassifyLink(real, nullptr, nodeA, nodeB, false, false).verdict == Verdict::Ok,
          "ok: the forward path arrives");
    check(ClassifyLink(real, nullptr, nodeA, nodeB, true, true).verdict == Verdict::Ok,
          "ok stays ok with off-mesh ends (the path arrived)");
    check(ClassifyLink(capped, nullptr, nodeA, nodeB, false, false).verdict == Verdict::Capped,
          "capped forward: capped, the reverse is not asked");
    check(ClassifyLink(stuckMid, &real, nodeA, nodeB, false, false).verdict == Verdict::ReverseOnly,
          "reverse-only: forward fails, reverse arrives");
    check(ClassifyLink(stuckMid, &real, nodeA, nodeB, true, true).verdict == Verdict::ReverseOnly,
          "reverse-only wins over off-mesh ends");
    check(ClassifyLink(stuckMid, &capped, nodeA, nodeB, false, false).verdict == Verdict::Capped,
          "a capped reverse: capped, not a fault");
    LinkVerdict v = ClassifyLink(stuckAtA, &stuckAtB, nodeA, nodeB, true, false);
    check(v.verdict == Verdict::EndOffMesh && v.cause == Cause::From, "end-off-mesh: the from end is the cause");
    v = ClassifyLink(stuckAtA, &stuckAtB, nodeA, nodeB, false, true);
    check(v.verdict == Verdict::EndOffMesh && v.cause == Cause::To, "end-off-mesh: the to end is the cause");
    v = ClassifyLink(stuckAtA, &stuckAtB, nodeA, nodeB, true, true);
    check(v.verdict == Verdict::EndOffMesh && v.cause == Cause::Both, "end-off-mesh: both ends");
    v = ClassifyLink(stuckAtA, &stuckAtB, nodeA, nodeB, false, false);
    check(v.verdict == Verdict::Dead && v.cause == Cause::None, "dead: neither arrives, both ends on the mesh");
    v = ClassifyLink(stuckMid, &stuckMid, nodeA, nodeB, true, true);
    check(v.verdict == Verdict::Dead, "an off-mesh end whose probes stopped over 20 yd away: dead");
    v = ClassifyLink(LinkResult{LinkClass::Incomplete, 1, PATH_INCOMPLETE, Vec{20, 0, 10}}, &stuckMid, nodeA, nodeB,
                     true, false);
    check(v.verdict == Verdict::EndOffMesh, "forward stopped exactly 20 yd from the off-mesh end: cause");
    v = ClassifyLink(LinkResult{LinkClass::Incomplete, 1, PATH_INCOMPLETE, Vec{21, 0, 10}}, &stuckMid, nodeA, nodeB,
                     true, false);
    check(v.verdict == Verdict::Dead, "forward stopped 21 yd from the off-mesh end: not the cause");
    v = ClassifyLink(stuckMid, &stuckAtB, nodeA, nodeB, false, true);
    check(v.verdict == Verdict::EndOffMesh && v.cause == Cause::To,
          "the reverse probe stopping at the to end blames it (the probe starts there)");
    check(!IsOffMesh(0.0f) && !IsOffMesh(4.0f) && IsOffMesh(4.1f) && IsOffMesh(-1.0f),
          "off the mesh: over 4.0 yd, or none");
    check(std::string(VerdictWord(Verdict::ReverseOnly)) == "reverse-only" &&
              std::string(VerdictWord(Verdict::EndOffMesh)) == "end-off-mesh",
          "class words");

    // Rows
    LinkResult broken{LinkClass::Fallback, 2, 0x11, Vec{12.4f, -3.6f, 7.0f}};
    check(LinkValue(LinkVerdict{Verdict::Dead, Cause::None}, broken, &stuckAtA, true) ==
              "dead fwd=fallback type=0x11 hop=2 at=12,-4,7 rev=incomplete type=0x04 hop=1 at=0,0,10 notile",
          "dead row value");
    check(LinkValue(LinkVerdict{Verdict::EndOffMesh, Cause::Both}, broken, &stuckAtA, false).find(" cause=both") !=
              std::string::npos,
          "end-off-mesh row names the cause");
    check(LinkValue(LinkVerdict{Verdict::Capped, Cause::None}, capped, nullptr, false).find(" rev=-") !=
              std::string::npos,
          "rev=- where the reverse was not asked");
    check(OffMeshValue(2.04f, true) == "2.0 steep" && OffMeshValue(-1.0f, false) == "none", "off-mesh row value");

    // Per node
    NodeLinkCounts perNode;
    CountNodeLink(perNode, Verdict::ReverseOnly, toKey, fromKey);
    CountNodeLink(perNode, Verdict::Dead, fromKey, toKey);
    CountNodeLink(perNode, Verdict::Dead, fromKey, NodeKey(1, 7, 8, 9));
    CountNodeLink(perNode, Verdict::Capped, fromKey, toKey);
    CountNodeLink(perNode, Verdict::Ok, fromKey, toKey);
    std::vector<std::string> nodeLines;
    AddNodeLinkLines(nodeLines, perNode);
    check(nodeLines.size() == 3 &&
              nodeLines[0] == "3\tnodes.links\t1:node:1,2,3\treverse-only=1 end-off-mesh=0 dead=2" &&
              nodeLines[1] == "3\tnodes.links\t1:node:4,5,6\treverse-only=1 end-off-mesh=0 dead=1",
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

    // The report
    MapCounts a;
    a.mapId = 571;
    a.links = 5;
    a.byClass[static_cast<int>(Verdict::Ok)] = 1;
    a.byClass[static_cast<int>(Verdict::ReverseOnly)] = 1;
    a.byClass[static_cast<int>(Verdict::EndOffMesh)] = 1;
    a.byClass[static_cast<int>(Verdict::Dead)] = 1;
    a.byClass[static_cast<int>(Verdict::Capped)] = 1;
    a.nodes = 2;
    a.offMesh = 2;
    a.offMesh4 = 1;
    MapCounts b;
    b.mapId = 0;
    b.links = 2;
    b.byClass[0] = 2;
    b.chained = 1;
    b.nodes = 2;
    check(a.Broken() == 3 && b.Broken() == 0, "broken = reverse-only + end-off-mesh + dead; capped is not broken");
    std::vector<std::string> rows = {Line("nodes.offmesh", NodeKey(571, 1, 1, 1), "1.5"),
                                     Line("links.class", LinkKey(fromKey, toKey), "dead fwd=fallback")};
    std::vector<std::string> const lines = ReportLines({a, b}, rows);
    check(std::is_sorted(lines.begin(), lines.end()), "report sorted");
    auto const has = [&](std::string const& line)
    { return std::find(lines.begin(), lines.end(), line) != lines.end(); };
    check(has("3\tlinks.checked\tall\t7") && has("3\tlinks.checked\t0\t2") && has("3\tlinks.checked\t571\t5"),
          "links.checked per map and in total");
    check(has("3\tlinks.class.ok\tall\t3") && has("3\tlinks.class.capped\t571\t1") &&
              has("3\tlinks.class.reverse-only\tall\t1") && has("3\tlinks.class.end-off-mesh\t571\t1") &&
              has("3\tlinks.class.dead\tall\t1") && has("3\tlinks.chained\tall\t1"),
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
    a.Add(b);
    check(a.links == 7 && a.byClass[0] == 3, "counts add");
    std::string const summary = SummaryLine("map 571", a);
    check(summary.find("7 links: 3 ok (1 chained), 1 reverse-only, 1 end-off-mesh, 1 dead, 1 capped") !=
              std::string::npos,
          "summary line");
    std::cout << "  " << summary << "\n";

    std::cout << "route-check-selftest: " << (checks - failures) << "/" << checks << " passed\n";
    return failures ? 1 : 0;
}
