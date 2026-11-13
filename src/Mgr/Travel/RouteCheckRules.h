/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKRULES_H
#define PLAYERBOTS_ROUTECHECKRULES_H

// `playerbots route check links|pairs` (NV3b, NV11, NV11b): the pure half. The walk a bot makes over a link, modelled
// on the engine's answers as its movement asks them (Walk), a link's class from both directions (ClassifyLink), and
// the gate report's lines (navigation NV1 design §4: tab-separated `check metric key value`, sorted, keys by rounded
// position, never a node id). Header-only so tests/route-check-selftest.cpp runs it without the engine;
// RouteCheck.cpp asks the engine.
//
// One direction (DirectionOf): ok = the walk arrives on navmesh moves only (stepping stones are navmesh paths too);
// shortcut = it arrives, but only with a straight move off the mesh (not a natural walk); stuck = it gives up;
// capped = WALK_MAX_MOVES moves without arrival, a length cap and not a fault.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <utility>
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
constexpr std::uint32_t PATH_FARFROMPOLY = 0xc0;  // FARFROMPOLY_START | FARFROMPOLY_END

constexpr float OFFMESH_LIMIT = 4.0f;   // yd (3D) from a node to the nearest polygon bots may walk on (NV5's measure)
constexpr float OFFMESH_LISTED = 1.0f;  // yd: the nodes.offmesh rows list nodes over this
constexpr float CAUSE_RADIUS = 20.0f;   // yd (2D): a walk that stopped this near an off-mesh end node blames it

// The gate's check number (PLAN §3.2 check 3)
constexpr char const* CHECK = "3";

struct Vec
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

inline float Dist2D(Vec const& a, Vec const& b) { return std::hypot(a.x - b.x, a.y - b.y); }

inline float Dist3D(Vec const& a, Vec const& b)
{
    return std::sqrt(Dist2D(a, b) * Dist2D(a, b) + (a.z - b.z) * (a.z - b.z));
}

// The walk (NV13b). A route follower orders its waypoint by NewRpgBaseAction::MoveFarTo: within WALK_ORDER_RANGE
// (3D) the waypoint itself; else the actual end of a path towards it, when the path type stays within WALK_TYPE_OK
// and that end gains WALK_GAIN on the waypoint; else a stepping stone in the forward cone (two samples). The order is
// a MotionMaster::MovePoint with a generated path and no forced destination: PointMovementGenerator and
// MoveSplineInit::MoveTo follow the path unless the answer is NOPATH (then a straight line), and a shortcut answer
// (NORMAL|NOT_USING_PATH for a mesh hole, as the owner is no creature; SHORT) is a straight line too. The trip
// arrives within WALK_GOAL (3D, TravelLegRules GOAL_YARDS) and gives up after WALK_STUCK_MOVES moves in a row that
// gain less than WALK_GAIN on the end.
constexpr std::uint32_t WALK_TYPE_OK = PATH_NORMAL | PATH_INCOMPLETE | PATH_FARFROMPOLY;
constexpr float WALK_ORDER_RANGE = 70.0f;  // yd: NewRpgBaseAction pathFinderDis
constexpr float WALK_GOAL = 10.0f;
constexpr float WALK_GAIN = 5.0f;
constexpr std::uint32_t WALK_STUCK_MOVES = 5;
constexpr std::uint32_t WALK_MAX_MOVES = 200;
constexpr float WALK_CONE = 3.14159265f;  // the cone's width: (rand - 0.5) x this

// What a walk's moves were, as bits
constexpr std::uint32_t WALK_MESH = 0x01;        // along a navmesh path
constexpr std::uint32_t WALK_INCOMPLETE = 0x02;  // that path, or the one that chose the order, was INCOMPLETE
constexpr std::uint32_t WALK_FARPOLY = 0x04;     // ... started or ended far from a polygon
constexpr std::uint32_t WALK_STRAIGHT = 0x08;    // a straight line (NOPATH, NOT_USING_PATH, SHORTCUT or SHORT)
constexpr std::uint32_t WALK_CONE_STONE = 0x10;  // the order was a stepping stone of the cone

// One engine answer as the walk reads it
struct WalkAnswer
{
    bool result{false};  // CalculatePath's return
    std::uint32_t type{0};
    Vec last;                 // the path's last point
    Vec actualEnd;            // GetActualEndPosition
    float length{0.0f};       // along the path's points
    std::vector<Vec> points;  // the path's points; only needed when the walk keeps a trace
};

enum class WalkEnd : std::uint8_t
{
    Arrived,
    Stuck,
    Capped  // WALK_MAX_MOVES moves
};

struct WalkResult
{
    WalkEnd end{WalkEnd::Capped};
    std::uint32_t moves{0};
    std::uint32_t kinds{0};  // WALK_* bits of every move
    float meshYd{0.0f};      // walked along navmesh paths
    float straightYd{0.0f};  // walked in straight lines
    Vec reached;             // where the walk ended
    bool straightSeen{false};
    Vec firstStraight;  // where the first straight move began
};

inline std::uint32_t WalkTypeKinds(std::uint32_t type)
{
    return ((type & PATH_INCOMPLETE) ? WALK_INCOMPLETE : 0u) | ((type & PATH_FARFROMPOLY) ? WALK_FARPOLY : 0u);
}

// One MovePoint order to target: where the bot ends, how it went and how far (PointMovementGenerator's rule); the
// points it passes go to trace when given
template <class PathFn>
std::uint32_t WalkMove(Vec const& at, Vec const& target, PathFn& path, Vec& land, float& length,
                       std::vector<Vec>* trace)
{
    WalkAnswer const answer = path(at, target);
    if (!answer.result || (answer.type & (PATH_NOPATH | PATH_NOT_USING_PATH | PATH_SHORTCUT | PATH_SHORT)))
    {
        land = target;
        length = Dist3D(at, target);
        if (trace)
            trace->push_back(target);
        return WALK_STRAIGHT;
    }
    land = answer.last;
    length = answer.length;
    if (trace && answer.points.size() > 1)
        trace->insert(trace->end(), answer.points.begin() + 1, answer.points.end());
    else if (trace)
        trace->push_back(land);
    return WALK_MESH | WalkTypeKinds(answer.type);
}

// The walk from start to end; path(at, target) returns one engine answer, rand() a number in [0, 1). trace, when
// given, gets the start and then every point the walk passes
template <class PathFn, class RandFn>
WalkResult Walk(Vec const& start, Vec const& end, PathFn&& path, RandFn&& rand, std::vector<Vec>* trace = nullptr)
{
    WalkResult result;
    Vec at = start;
    float nearest = Dist3D(at, end);
    std::uint32_t idle = 0;
    if (trace)
        trace->push_back(start);
    while (result.moves < WALK_MAX_MOVES)
    {
        float const distance = Dist3D(at, end);
        if (distance <= WALK_GOAL)
        {
            result.end = WalkEnd::Arrived;
            break;
        }
        Vec target = end;
        std::uint32_t kinds = 0;
        bool order = true;
        if (distance >= WALK_ORDER_RANGE)
        {
            WalkAnswer const toward = path(at, end);
            if (toward.result && !(toward.type & ~WALK_TYPE_OK) && Dist3D(end, toward.actualEnd) + WALK_GAIN < distance)
            {
                target = toward.actualEnd;
                kinds = WalkTypeKinds(toward.type);
            }
            else
            {
                order = false;
                float minDelta = WALK_CONE;
                float const base = std::atan2(end.y - at.y, end.x - at.x);
                for (int attempt = 0; attempt < 2; ++attempt)
                {
                    float const delta = (rand() - 0.5f) * WALK_CONE;
                    float const reach = (0.5f + rand() * 0.5f) * WALK_ORDER_RANGE;
                    Vec const sample{at.x + std::cos(base + delta) * reach, at.y + std::sin(base + delta) * reach,
                                     at.z + 0.5f};
                    WalkAnswer const stone = path(at, sample);
                    if (stone.result && !(stone.type & ~WALK_TYPE_OK) && std::fabs(delta) <= minDelta)
                    {
                        order = true;
                        target = stone.actualEnd;
                        minDelta = std::fabs(delta);
                        kinds = WALK_CONE_STONE | WalkTypeKinds(stone.type);
                    }
                }
            }
        }
        ++result.moves;
        if (order)
        {
            Vec land;
            float length = 0.0f;
            kinds |= WalkMove(at, target, path, land, length, trace);
            if (kinds & WALK_STRAIGHT)
            {
                if (!result.straightSeen)
                    result.firstStraight = at;
                result.straightSeen = true;
                result.straightYd += length;
            }
            else
                result.meshYd += length;
            result.kinds |= kinds;
            at = land;
        }
        float const now = Dist3D(at, end);
        if (now + WALK_GAIN < nearest)
        {
            nearest = now;
            idle = 0;
        }
        else if (++idle >= WALK_STUCK_MOVES)
        {
            result.end = WalkEnd::Stuck;
            break;
        }
    }
    result.reached = at;
    return result;
}

// One direction of a link, by its walk
enum class Direction : std::uint8_t
{
    Ok,        // arrives on navmesh moves only
    Shortcut,  // arrives, but only with a straight move
    Stuck,     // gives up
    Capped     // WALK_MAX_MOVES moves without arrival
};

inline Direction DirectionOf(WalkResult const& walk)
{
    if (walk.end == WalkEnd::Capped)
        return Direction::Capped;
    if (walk.end == WalkEnd::Stuck)
        return Direction::Stuck;
    return (walk.kinds & WALK_STRAIGHT) ? Direction::Shortcut : Direction::Ok;
}

inline char const* DirectionWord(Direction direction)
{
    switch (direction)
    {
        case Direction::Ok:
            return "ok";
        case Direction::Shortcut:
            return "shortcut";
        case Direction::Stuck:
            return "stuck";
        case Direction::Capped:
            return "capped";
    }
    return "?";
}

inline std::string WalkKindsWord(std::uint32_t kinds)
{
    static constexpr std::array<std::pair<std::uint32_t, char const*>, 5> NAMES{{{WALK_MESH, "mesh"},
                                                                                 {WALK_INCOMPLETE, "incomplete"},
                                                                                 {WALK_FARPOLY, "farpoly"},
                                                                                 {WALK_STRAIGHT, "straight"},
                                                                                 {WALK_CONE_STONE, "cone"}}};
    std::string text;
    for (auto const& [bit, name] : NAMES)
        if (kinds & bit)
            text += (text.empty() ? "" : "+") + std::string(name);
    return text.empty() ? "-" : text;
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
    Ok,           // the forward walk arrives on the mesh
    ReverseOnly,  // only the other way does: this direction is left out
    EndOffMesh,   // neither walk arrives and an end node off the mesh is the cause: the node is moved
    Dead,         // neither walk arrives, no end node is the cause: the link is left out
    Shortcut,     // neither arrives on the mesh, one only with a straight move: left out like dead
    Capped        // a length cap, not a fault: listed apart
};

constexpr int VERDICTS = 6;

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
        case Verdict::Shortcut:
            return "shortcut";
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

// rev: the reverse walk, null when it was not walked (the forward walk was ok or capped). A walk that arrives with a
// straight move makes the link a shortcut unless the other direction is ok. When both get stuck, an end node is the
// cause when it is off the mesh (over OFFMESH_LIMIT yd from it, or none) and either walk stopped within CAUSE_RADIUS
// yd (2D) of it. A capped reverse makes the link capped: its fault is unknown.
inline LinkVerdict ClassifyLink(WalkResult const& fwd, WalkResult const* rev, Vec const& from, Vec const& to,
                                bool fromOff, bool toOff)
{
    LinkVerdict out;
    Direction const forward = DirectionOf(fwd);
    if (forward == Direction::Ok)
        return out;
    if (forward == Direction::Capped)
    {
        out.verdict = Verdict::Capped;
        return out;
    }
    Direction const reverse = rev ? DirectionOf(*rev) : Direction::Stuck;
    if (reverse == Direction::Ok)
    {
        out.verdict = Verdict::ReverseOnly;
        return out;
    }
    if (reverse == Direction::Capped)
    {
        out.verdict = Verdict::Capped;
        return out;
    }
    if (forward == Direction::Shortcut || reverse == Direction::Shortcut)
    {
        out.verdict = Verdict::Shortcut;
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

// One direction in a row: `<name>=<ok|shortcut|stuck|capped> moves=N kinds=a+b mesh=Y straight=Y at=x,y,z
// [from=x,y,z]` (at: where the walk ended; from: where its first straight move began)
inline std::string DirectionValue(char const* name, WalkResult const& walk)
{
    char text[256];
    std::snprintf(text, sizeof(text), "%s=%s moves=%u kinds=%s mesh=%.0f straight=%.0f at=%.0f,%.0f,%.0f", name,
                  DirectionWord(DirectionOf(walk)), walk.moves, WalkKindsWord(walk.kinds).c_str(), walk.meshYd,
                  walk.straightYd, walk.reached.x, walk.reached.y, walk.reached.z);
    std::string value = text;
    if (walk.straightSeen)
    {
        std::snprintf(text, sizeof(text), " from=%.0f,%.0f,%.0f", walk.firstStraight.x, walk.firstStraight.y,
                      walk.firstStraight.z);
        value += text;
    }
    return value;
}

// A row's value: the class word, then both walks (rev=- when not walked), the cause of an end-off-mesh link
inline std::string LinkValue(LinkVerdict const& verdict, WalkResult const& fwd, WalkResult const* rev, bool notile)
{
    std::string text = std::string(VerdictWord(verdict.verdict)) + " " + DirectionValue("fwd", fwd) + " " +
                       (rev ? DirectionValue("rev", *rev) : std::string("rev=-"));
    if (verdict.verdict == Verdict::EndOffMesh)
        text += std::string(" cause=") + CauseWord(verdict.cause);
    if (notile)
        text += " notile";
    return text;
}

// distance < 0: no polygon within the search box. steep: the nearest polygon of any kind is steep (NAV_GROUND_STEEP)
// and lies within OFFMESH_LIMIT
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
    std::uint32_t chained{0};           // ok links walked in more than one move
    std::uint32_t nodes{0};
    std::uint32_t offMesh{0};   // nodes over OFFMESH_LISTED yd from the mesh (or none)
    std::uint32_t offMesh4{0};  // nodes over OFFMESH_LIMIT yd from the mesh (or none)
    std::uint32_t offMeshSteep{0};

    std::uint32_t Broken() const
    {
        return links - byClass[static_cast<int>(Verdict::Ok)] - byClass[static_cast<int>(Verdict::Capped)];
    }

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
using NodeLinkCounts = std::map<std::string, std::array<std::uint32_t, 4>>;  // reverse-only, end-off-mesh, dead,
                                                                             // shortcut

inline void CountNodeLink(NodeLinkCounts& counts, Verdict verdict, std::string const& fromKey, std::string const& toKey)
{
    int const slot = verdict == Verdict::ReverseOnly  ? 0
                     : verdict == Verdict::EndOffMesh ? 1
                     : verdict == Verdict::Dead       ? 2
                     : verdict == Verdict::Shortcut   ? 3
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
                                 " dead=" + std::to_string(n[2]) + " shortcut=" + std::to_string(n[3])));
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

// The points of a walk, in order: `x,y,z;x,y,z;...`
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
    char text[400];
    std::snprintf(text, sizeof(text),
                  "%s: %u links: %u ok (%u chained), %u reverse-only, %u end-off-mesh, %u dead, %u shortcut, %u "
                  "capped; %u nodes, %u over %.0f yd from the mesh, %u over %.0f yd (%u by a steep polygon)",
                  name.c_str(), counts.links, counts.byClass[static_cast<int>(Verdict::Ok)], counts.chained,
                  counts.byClass[static_cast<int>(Verdict::ReverseOnly)],
                  counts.byClass[static_cast<int>(Verdict::EndOffMesh)],
                  counts.byClass[static_cast<int>(Verdict::Dead)], counts.byClass[static_cast<int>(Verdict::Shortcut)],
                  counts.byClass[static_cast<int>(Verdict::Capped)], counts.nodes, counts.offMesh, OFFMESH_LISTED,
                  counts.offMesh4, OFFMESH_LIMIT, counts.offMeshSteep);
    return text;
}
}  // namespace RouteCheck

#endif
