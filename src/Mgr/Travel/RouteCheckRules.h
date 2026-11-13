/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKRULES_H
#define PLAYERBOTS_ROUTECHECKRULES_H

// `playerbots route check links [map]` (NV3b): the pure half. How one engine answer (a PathGenerator path type and
// its last point) is classed, how a walk link longer than one answer is chained, and the gate report's lines
// (navigation NV1 design §4: tab-separated `check metric key value`, sorted, keys by rounded position, never a node
// id). Header-only so tests/route-check-selftest.cpp runs it without the engine; RouteCheck.cpp asks the engine.
//
// A direction's answer (LinkClass); a link's verdict from both directions is Verdict, below:
// - real: the engine's path reaches the link's end (path type exactly NORMAL, last point within END_TOLERANCE);
// - incomplete: the engine finds polygons towards the end but not to it (INCOMPLETE without progress, or NORMAL
//   ending short of the end, e.g. an end node off the mesh);
// - fallback: no navmesh answer at all, only the straight line (SHORTCUT, NOPATH, or NORMAL|NOT_USING_PATH: no
//   tile, or no polygon near an end, for an owner that is no creature);
// - capped: the answer hit a length cap, not a navmesh fault. A path is at most MAX_PATH_LENGTH (148) polygons, so a
//   long link ends INCOMPLETE short of its end; such a link is chained: the next answer starts at the last point,
//   as a bot that walks the link re-paths from where it stands. Capped means MAX_HOPS answers did not get there, or
//   an answer had more points than the cap (SHORT). Capped links are listed apart and are not counted as broken.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace RouteCheck
{
// PathType bits (core PathGenerator.h); RouteCheck.cpp checks them against the engine's enum
constexpr std::uint32_t PATH_NORMAL = 0x01;
constexpr std::uint32_t PATH_SHORTCUT = 0x02;
constexpr std::uint32_t PATH_INCOMPLETE = 0x04;
constexpr std::uint32_t PATH_NOPATH = 0x08;
constexpr std::uint32_t PATH_NOT_USING_PATH = 0x10;
constexpr std::uint32_t PATH_SHORT = 0x20;

constexpr float END_TOLERANCE = 3.0f;    // yd, 2D: the last point is the link's end
constexpr float END_TOLERANCE_Z = 6.0f;  // yd: the engine sets each point to the ground height below it
constexpr float MIN_PROGRESS = 1.0f;     // yd a chained answer must gain on the end (2D)
constexpr std::uint32_t MAX_HOPS = 40;   // answers per link; the legacy getPathFromPath's attempt limit
constexpr float OFFMESH_LIMIT = 4.0f;    // yd (3D) from a node to the nearest polygon bots may walk on (NV5's measure)
constexpr float OFFMESH_LISTED = 1.0f;   // yd: the nodes.offmesh rows list nodes over this
constexpr float CAUSE_RADIUS = 20.0f;    // yd (2D): a probe that stopped this near an off-mesh end node blames it

// The gate's check number (PLAN §3.2 check 3)
constexpr char const* CHECK = "3";

struct Vec
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

inline float Dist2D(Vec const& a, Vec const& b) { return std::hypot(a.x - b.x, a.y - b.y); }

enum class LinkClass : std::uint8_t
{
    Real,
    Incomplete,
    Fallback,
    Capped
};

inline char const* ClassWord(LinkClass cls)
{
    switch (cls)
    {
        case LinkClass::Real:
            return "real";
        case LinkClass::Incomplete:
            return "incomplete";
        case LinkClass::Fallback:
            return "fallback";
        case LinkClass::Capped:
            return "capped";
    }
    return "?";
}

enum class HopOutcome : std::uint8_t
{
    Arrived,
    Continue,  // a chained link goes on from the answer's last point
    Incomplete,
    Fallback,
    Capped
};

// One engine answer from `at` towards `end`: its path type and its last point (`at` when it has none)
inline HopOutcome ClassifyHop(std::uint32_t type, Vec const& at, Vec const& last, Vec const& end)
{
    if (type & PATH_SHORT)
        return HopOutcome::Capped;
    if (!type || (type & (PATH_SHORTCUT | PATH_NOPATH | PATH_NOT_USING_PATH)))
        return HopOutcome::Fallback;
    if (type == PATH_NORMAL && Dist2D(last, end) <= END_TOLERANCE && std::fabs(last.z - end.z) <= END_TOLERANCE_Z)
        return HopOutcome::Arrived;
    if (Dist2D(at, end) - Dist2D(last, end) >= MIN_PROGRESS)
        return HopOutcome::Continue;
    return HopOutcome::Incomplete;
}

struct Hop
{
    std::uint32_t type{0};
    Vec last;
};

struct LinkResult
{
    LinkClass cls{LinkClass::Fallback};
    std::uint32_t hops{0};
    std::uint32_t lastType{0};  // the path type of the answer that decided
    Vec reached;                // the last point of that answer
};

// Chains answers along a link; path(at, end) returns one engine answer
template <class PathFn>
LinkResult Chain(Vec const& start, Vec const& end, PathFn&& path, std::uint32_t maxHops = MAX_HOPS)
{
    LinkResult result;
    Vec at = start;
    for (std::uint32_t hop = 1; hop <= maxHops; ++hop)
    {
        Hop const answer = path(at, end);
        result.hops = hop;
        result.lastType = answer.type;
        result.reached = answer.last;
        switch (ClassifyHop(answer.type, at, answer.last, end))
        {
            case HopOutcome::Arrived:
                result.cls = LinkClass::Real;
                return result;
            case HopOutcome::Continue:
                at = answer.last;
                break;
            case HopOutcome::Incomplete:
                result.cls = LinkClass::Incomplete;
                return result;
            case HopOutcome::Fallback:
                result.cls = LinkClass::Fallback;
                return result;
            case HopOutcome::Capped:
                result.cls = LinkClass::Capped;
                return result;
        }
    }
    result.cls = LinkClass::Capped;
    return result;
}

// A node's lasting name: `map:node:x,y,z` rounded to a yard. The loaded graph keeps no node kind, so the kind is
// always `node` (NV1 design §4)
inline std::string NodeKey(std::uint32_t mapId, float x, float y, float z)
{
    char text[96];
    std::snprintf(text, sizeof(text), "%u:node:%ld,%ld,%ld", mapId, std::lround(x), std::lround(y), std::lround(z));
    return text;
}

inline std::string LinkKey(std::string const& fromKey, std::string const& toKey)
{
    return fromKey + ">" + toKey + ":walk";
}

inline std::string Line(std::string const& metric, std::string const& key, std::string const& value)
{
    return std::string(CHECK) + "\t" + metric + "\t" + key + "\t" + value;
}

// What a link is, from both directions (PLAN §3.2 check 3); the word names the repair
enum class Verdict : std::uint8_t
{
    Ok,           // the forward path arrives
    ReverseOnly,  // arrives only the other way: this direction is left out
    EndOffMesh,   // neither arrives and an end node off the mesh is the cause: the node is moved
    Dead,         // neither arrives, both nodes on the mesh: the link is left out
    Capped        // a length cap, not a fault: listed apart
};

constexpr int VERDICTS = 5;

inline char const* VerdictWord(Verdict verdict)
{
    switch (verdict)
    {
        case Verdict::Ok:
            return "ok";
        case Verdict::ReverseOnly:
            return "reverse-only";
        case Verdict::EndOffMesh:
            return "end-off-mesh";
        case Verdict::Dead:
            return "dead";
        case Verdict::Capped:
            return "capped";
    }
    return "?";
}

enum class Cause : std::uint8_t
{
    None,
    From,
    To,
    Both
};

inline char const* CauseWord(Cause cause)
{
    switch (cause)
    {
        case Cause::From:
            return "from";
        case Cause::To:
            return "to";
        case Cause::Both:
            return "both";
        case Cause::None:
            break;
    }
    return "none";
}

// distance: from a node to the nearest polygon bots may walk on, < 0 when there is none in the search box
inline bool IsOffMesh(float distance) { return distance < 0.0f || distance > OFFMESH_LIMIT; }

struct LinkVerdict
{
    Verdict verdict{Verdict::Ok};
    Cause cause{Cause::None};
};

// rev: the answer of the reverse probe, null when it was not asked (the forward answer arrived or was capped).
// An end node is the cause when it is off the mesh (over OFFMESH_LIMIT yd from it, or none) and either probe stopped
// within CAUSE_RADIUS yd (2D) of it. A capped reverse probe makes the link capped: its fault is unknown.
inline LinkVerdict ClassifyLink(LinkResult const& fwd, LinkResult const* rev, Vec const& from, Vec const& to,
                                bool fromOff, bool toOff)
{
    LinkVerdict out;
    if (fwd.cls == LinkClass::Real)
        return out;
    if (fwd.cls == LinkClass::Capped)
    {
        out.verdict = Verdict::Capped;
        return out;
    }
    if (rev && rev->cls == LinkClass::Real)
    {
        out.verdict = Verdict::ReverseOnly;
        return out;
    }
    if (rev && rev->cls == LinkClass::Capped)
    {
        out.verdict = Verdict::Capped;
        return out;
    }
    auto const stoppedNear = [&](Vec const& node)
    { return Dist2D(fwd.reached, node) <= CAUSE_RADIUS || (rev && Dist2D(rev->reached, node) <= CAUSE_RADIUS); };
    bool const fromCause = fromOff && stoppedNear(from);
    bool const toCause = toOff && stoppedNear(to);
    if (fromCause || toCause)
    {
        out.verdict = Verdict::EndOffMesh;
        out.cause = fromCause && toCause ? Cause::Both : fromCause ? Cause::From : Cause::To;
    }
    else
        out.verdict = Verdict::Dead;
    return out;
}

inline std::string DirectionValue(char const* name, LinkResult const& result)
{
    char text[128];
    std::snprintf(text, sizeof(text), "%s=%s type=0x%02x hop=%u at=%.0f,%.0f,%.0f", name, ClassWord(result.cls),
                  result.lastType, result.hops, result.reached.x, result.reached.y, result.reached.z);
    return text;
}

// A row's value: the class word, then both answers (rev=- when not asked), the cause of an end-off-mesh link
inline std::string LinkValue(LinkVerdict const& verdict, LinkResult const& fwd, LinkResult const* rev, bool notile)
{
    std::string text = std::string(VerdictWord(verdict.verdict)) + " " + DirectionValue("fwd", fwd) + " " +
                       (rev ? DirectionValue("rev", *rev) : std::string("rev=-"));
    if (verdict.verdict == Verdict::EndOffMesh)
        text += std::string(" cause=") + CauseWord(verdict.cause);
    if (notile)
        text += " notile";
    return text;
}

// distance < 0: no polygon within the search box. steep: a polygon bots may not walk on (NAV_GROUND_STEEP) lies
// within OFFMESH_LIMIT
inline std::string OffMeshValue(float distance, bool steep)
{
    char text[64];
    if (distance < 0.0f)
        std::snprintf(text, sizeof(text), "none%s", steep ? " steep" : "");
    else
        std::snprintf(text, sizeof(text), "%.1f%s", distance, steep ? " steep" : "");
    return text;
}

struct MapCounts
{
    std::uint32_t mapId{0};
    std::uint32_t links{0};
    std::uint32_t byClass[VERDICTS]{};  // index = Verdict
    std::uint32_t chained{0};           // ok links that took more than one answer
    std::uint32_t nodes{0};
    std::uint32_t offMesh{0};   // nodes over OFFMESH_LISTED yd from the mesh (or none)
    std::uint32_t offMesh4{0};  // nodes over OFFMESH_LIMIT yd from the mesh (or none)
    std::uint32_t offMeshSteep{0};

    std::uint32_t Broken() const { return links - byClass[0] - byClass[static_cast<int>(Verdict::Capped)]; }

    void Add(MapCounts const& other)
    {
        links += other.links;
        for (int i = 0; i < VERDICTS; ++i)
            byClass[i] += other.byClass[i];
        chained += other.chained;
        nodes += other.nodes;
        offMesh += other.offMesh;
        offMesh4 += other.offMesh4;
        offMeshSteep += other.offMeshSteep;
    }
};

// The count lines of one map (key = map id) or of all maps (key "all")
inline void AddCountLines(std::vector<std::string>& lines, std::string const& key, MapCounts const& counts)
{
    lines.push_back(Line("links.checked", key, std::to_string(counts.links)));
    for (int i = 0; i < VERDICTS; ++i)
        lines.push_back(Line(std::string("links.class.") + VerdictWord(static_cast<Verdict>(i)), key,
                             std::to_string(counts.byClass[i])));
    lines.push_back(Line("links.chained", key, std::to_string(counts.chained)));
    lines.push_back(Line("nodes.checked", key, std::to_string(counts.nodes)));
    lines.push_back(Line("nodes.class.offmesh", key, std::to_string(counts.offMesh)));
    lines.push_back(Line("nodes.class.offmesh4", key, std::to_string(counts.offMesh4)));
}

// Per node (key = node key): its non-ok links by class, both ends counted. Capped links are not counted.
using NodeLinkCounts = std::map<std::string, std::array<std::uint32_t, 3>>;  // reverse-only, end-off-mesh, dead

inline void CountNodeLink(NodeLinkCounts& counts, Verdict verdict, std::string const& fromKey, std::string const& toKey)
{
    int const slot = verdict == Verdict::ReverseOnly  ? 0
                     : verdict == Verdict::EndOffMesh ? 1
                     : verdict == Verdict::Dead       ? 2
                                                      : -1;
    if (slot < 0)
        return;
    ++counts[fromKey][slot];
    ++counts[toKey][slot];
}

inline void AddNodeLinkLines(std::vector<std::string>& lines, NodeLinkCounts const& counts)
{
    for (auto const& [key, n] : counts)
        lines.push_back(Line("nodes.links", key,
                             "reverse-only=" + std::to_string(n[0]) + " end-off-mesh=" + std::to_string(n[1]) +
                                 " dead=" + std::to_string(n[2])));
}

// A pair of points checked like a link without a stored link (`route check pairs`)
struct PairSpec
{
    std::uint32_t mapId{0};
    Vec from;
    Vec to;
    std::string label;
};

// `map x1 y1 z1 x2 y2 z2 [label]`; false for a comment, a blank or a malformed line
inline bool ParsePairLine(std::string const& line, PairSpec& out)
{
    std::istringstream in(line);
    std::string first;
    if (!(in >> first) || first[0] == '#')
        return false;
    PairSpec pair;
    try
    {
        std::size_t used = 0;
        unsigned long const map = std::stoul(first, &used);
        if (used != first.size())
            return false;
        pair.mapId = static_cast<std::uint32_t>(map);
    }
    catch (...)
    {
        return false;
    }
    if (!(in >> pair.from.x >> pair.from.y >> pair.from.z >> pair.to.x >> pair.to.y >> pair.to.z))
        return false;
    std::getline(in >> std::ws, pair.label);
    while (!pair.label.empty() && (pair.label.back() == '\r' || pair.label.back() == ' '))
        pair.label.pop_back();
    out = pair;
    return true;
}

inline std::string PairKey(std::string const& fromKey, std::string const& toKey, std::string const& label)
{
    return fromKey + ">" + toKey + ":pair" + (label.empty() ? "" : "#" + label);
}

// The corners of a path, in order: `x,y,z;x,y,z;...`
inline std::string PathValue(std::vector<Vec> const& points)
{
    std::string text;
    char one[64];
    for (Vec const& point : points)
    {
        std::snprintf(one, sizeof(one), "%.1f,%.1f,%.1f", point.x, point.y, point.z);
        text += (text.empty() ? "" : ";") + std::string(one);
    }
    return text;
}

// The report: count lines per map and in total, the status, then the rows; sorted (by metric, then key)
inline std::vector<std::string> ReportLines(std::vector<MapCounts> const& maps, std::vector<std::string> rows)
{
    MapCounts total;
    for (MapCounts const& counts : maps)
    {
        AddCountLines(rows, std::to_string(counts.mapId), counts);
        total.Add(counts);
    }
    AddCountLines(rows, "all", total);
    rows.push_back(Line("status", "-", total.Broken() == 0 && total.offMesh4 == 0 ? "pass" : "fail"));
    std::sort(rows.begin(), rows.end());
    return rows;
}

// One console line per map
inline std::string SummaryLine(std::string const& name, MapCounts const& counts)
{
    char text[360];
    std::snprintf(
        text, sizeof(text),
        "%s: %u links: %u ok (%u chained), %u reverse-only, %u end-off-mesh, %u dead, %u capped; %u nodes, %u "
        "over %.0f yd from the mesh, %u over %.0f yd (%u by a steep polygon)",
        name.c_str(), counts.links, counts.byClass[0], counts.chained, counts.byClass[1], counts.byClass[2],
        counts.byClass[3], counts.byClass[4], counts.nodes, counts.offMesh, OFFMESH_LISTED, counts.offMesh4,
        OFFMESH_LIMIT, counts.offMeshSteep);
    return text;
}
}  // namespace RouteCheck

#endif
