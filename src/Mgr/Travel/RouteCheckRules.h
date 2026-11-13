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
// A link's class:
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
#include <cmath>
#include <cstdint>
#include <cstdio>
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
constexpr float OFFMESH_LIMIT = 1.0f;    // yd (3D) from a node to the nearest polygon bots may walk on

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

inline std::string BrokenValue(LinkResult const& result, bool startTile, bool endTile)
{
    char text[160];
    std::snprintf(text, sizeof(text), "%s type=0x%02x hop=%u at=%.0f,%.0f,%.0f%s", ClassWord(result.cls),
                  result.lastType, result.hops, result.reached.x, result.reached.y, result.reached.z,
                  !startTile || !endTile ? " notile" : "");
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
    std::uint32_t byClass[4]{};  // index = LinkClass
    std::uint32_t chained{0};    // real links that took more than one answer
    std::uint32_t nodes{0};
    std::uint32_t offMesh{0};
    std::uint32_t offMeshSteep{0};

    std::uint32_t Broken() const
    {
        return byClass[static_cast<int>(LinkClass::Incomplete)] + byClass[static_cast<int>(LinkClass::Fallback)];
    }

    void Add(MapCounts const& other)
    {
        links += other.links;
        for (int i = 0; i < 4; ++i)
            byClass[i] += other.byClass[i];
        chained += other.chained;
        nodes += other.nodes;
        offMesh += other.offMesh;
        offMeshSteep += other.offMeshSteep;
    }
};

// The count lines of one map (key = map id) or of all maps (key "all")
inline void AddCountLines(std::vector<std::string>& lines, std::string const& key, MapCounts const& counts)
{
    lines.push_back(Line("links.checked", key, std::to_string(counts.links)));
    for (int i = 0; i < 4; ++i)
        lines.push_back(Line(std::string("links.class.") + ClassWord(static_cast<LinkClass>(i)), key,
                             std::to_string(counts.byClass[i])));
    lines.push_back(Line("links.chained", key, std::to_string(counts.chained)));
    lines.push_back(Line("nodes.checked", key, std::to_string(counts.nodes)));
    lines.push_back(Line("nodes.class.offmesh", key, std::to_string(counts.offMesh)));
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
    rows.push_back(Line("status", "-", total.Broken() == 0 && total.offMesh == 0 ? "pass" : "fail"));
    std::sort(rows.begin(), rows.end());
    return rows;
}

// One console line per map
inline std::string SummaryLine(std::string const& name, MapCounts const& counts)
{
    char text[320];
    std::snprintf(
        text, sizeof(text),
        "%s: %u links: %u real (%u chained), %u incomplete, %u fallback, %u capped; %u nodes, %u off the mesh "
        "(%u by a steep polygon)",
        name.c_str(), counts.links, counts.byClass[0], counts.chained, counts.byClass[1], counts.byClass[2],
        counts.byClass[3], counts.nodes, counts.offMesh, counts.offMeshSteep);
    return text;
}
}  // namespace RouteCheck

#endif
