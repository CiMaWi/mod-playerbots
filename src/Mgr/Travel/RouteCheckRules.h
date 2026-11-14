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
// shortcut = it arrives, but only with a straight move off the mesh (not a natural walk; the engine's hop counts as
// one, NV19); stuck = it gives up; capped = WALK_MAX_MOVES moves without arrival, a length cap and not a fault.
// The walk goes node to node (`mode=nodes`) or over the link's stored points in order (`mode=path`, WalkPath).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
// The engine's hop (G33, NV19, NV19d). When the polygon path towards the target is partial and holds only the start
// polygon (INCOMPLETE), PathGenerator::FindSmoothPath steers to the target unclamped ("the path is on the same
// poly", v21 PathGenerator.cpp:998-1000, v20 :966-968): its smooth steps go to the polygon's edge, and at the end of
// the corridor it copies that target in as the last point ("Reached end of path", v21 :1064-1065, v20 :1032-1033),
// which may lie far off the mesh. Core v20 also appends the target to a smooth path of one point whatever the type
// (:656, v21 for NORMAL paths only, :659). Both cores share the first; the bot walks the smooth part, then a straight
// line to the last point. A smooth path has a point at least every SMOOTH_PATH_STEP_SIZE (4 yd) plus
// SMOOTH_PATH_SLOP (0.3 yd), so an INCOMPLETE answer whose last leg is longer than this (2D) ends with the hop, on
// either core. (Off-mesh connections also make a long leg; they lie inside a path, not at its end.)
constexpr float HOP_MIN = 4.3f;

// What a walk's moves were, as bits
constexpr std::uint32_t WALK_MESH = 0x01;        // along a navmesh path
constexpr std::uint32_t WALK_INCOMPLETE = 0x02;  // that path, or the one that chose the order, was INCOMPLETE
constexpr std::uint32_t WALK_FARPOLY = 0x04;     // ... started or ended far from a polygon
constexpr std::uint32_t WALK_STRAIGHT = 0x08;    // a straight line (NOPATH, NOT_USING_PATH, SHORTCUT, SHORT or a hop)
constexpr std::uint32_t WALK_CONE_STONE = 0x10;  // the order was a stepping stone of the cone
constexpr std::uint32_t WALK_HOP = 0x20;         // the straight line was the engine's hop (IsHop)

// The polygon areas a walk touches (`areas=all` reports them): the bits `AreaBit` makes of the area ids
constexpr std::uint32_t AREA_LAVA = 0x01;
constexpr std::uint32_t AREA_SLIME = 0x02;
constexpr std::uint32_t AREA_FATIGUE = 0x04;  // fatigue water
constexpr std::uint32_t AREA_OTHER = 0x08;    // an area id other than ground, water, steep ground and the above

// The bit of a polygon's area id (the engine's NavTerrain values: ground 1, lava 2, slime 4, water 8, steep 0x10,
// fatigue water 0x20); ground, water, steep ground and no area are the plain ones and make no bit
inline std::uint32_t AreaBit(unsigned area)
{
    switch (area)
    {
        case 0x00:
        case 0x01:
        case 0x08:
        case 0x10:
            return 0;
        case 0x02:
            return AREA_LAVA;
        case 0x04:
            return AREA_SLIME;
        case 0x20:
            return AREA_FATIGUE;
        default:
            return AREA_OTHER;
    }
}

inline std::string AreasWord(std::uint32_t areas)
{
    static constexpr std::array<std::pair<std::uint32_t, char const*>, 4> NAMES{
        {{AREA_FATIGUE, "fatigue"}, {AREA_LAVA, "lava"}, {AREA_SLIME, "slime"}, {AREA_OTHER, "other"}}};
    std::string text;
    for (auto const& [bit, name] : NAMES)
        if (areas & bit)
            text += (text.empty() ? "" : "+") + std::string(name);
    return text.empty() ? "-" : text;
}

// One engine answer as the walk reads it
struct WalkAnswer
{
    bool result{false};  // CalculatePath's return
    std::uint32_t type{0};
    Vec beforeLast;               // the point before the last one (the first, for two points)
    Vec last;                     // the path's last point
    Vec actualEnd;                // GetActualEndPosition
    float length{0.0f};           // along the path's points
    std::uint32_t pointCount{0};  // the path's points
    std::vector<Vec> points;      // the path's points; only needed when the walk keeps a trace
    std::uint32_t areas{0};       // AREA_* bits of the polygons under the path (set only under `areas=all`)
};

// The engine's hop (HOP_MIN): the answer's last point was copied in, not reached on the mesh
inline bool IsHop(WalkAnswer const& answer)
{
    return (answer.type & PATH_INCOMPLETE) && answer.pointCount >= 2 &&
           Dist2D(answer.beforeLast, answer.last) > HOP_MIN;
}

// One move as the walk counts it
struct WalkStep
{
    Vec land;                // where the bot ends
    float meshYd{0.0f};      // walked along the path
    float straightYd{0.0f};  // walked in a straight line
    Vec straightFrom;        // where that line began
    std::uint32_t areas{0};  // AREA_* bits of the path the bot walked
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
    std::uint32_t areas{0};  // AREA_* bits of every move's path
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
// points it passes go to trace when given. A hop's answer is the path to its point before the last, then a
// straight line
template <class PathFn>
std::uint32_t WalkMove(Vec const& at, Vec const& target, PathFn& path, WalkStep& step, std::vector<Vec>* trace)
{
    WalkAnswer const answer = path(at, target);
    step = WalkStep{};
    if (!answer.result || (answer.type & (PATH_NOPATH | PATH_NOT_USING_PATH | PATH_SHORTCUT | PATH_SHORT)))
    {
        step.land = target;
        step.straightYd = Dist3D(at, target);
        step.straightFrom = at;
        if (trace)
            trace->push_back(target);
        return WALK_STRAIGHT;
    }
    step.land = answer.last;
    step.areas = answer.areas;
    if (trace && answer.points.size() > 1)
        trace->insert(trace->end(), answer.points.begin() + 1, answer.points.end());
    else if (trace)
        trace->push_back(step.land);
    if (IsHop(answer))
    {
        step.straightYd = Dist3D(answer.beforeLast, answer.last);
        step.meshYd = std::max(0.0f, answer.length - step.straightYd);
        step.straightFrom = answer.beforeLast;
        return WALK_STRAIGHT | WALK_HOP | (answer.pointCount > 2 ? WALK_MESH : 0u) | WalkTypeKinds(answer.type);
    }
    step.meshYd = answer.length;
    return WALK_MESH | WalkTypeKinds(answer.type);
}

// One leg of a walk, from at towards end; path(at, target) returns one engine answer, rand() a number in [0, 1).
// Its moves are added to result (moves, kinds, yards, the first straight move) and at becomes where the bot
// stands; the leg arrives, gets stuck, or is capped after WALK_MAX_MOVES moves of its own
template <class PathFn, class RandFn>
WalkEnd WalkLeg(Vec& at, Vec const& end, PathFn& path, RandFn& rand, std::vector<Vec>* trace, WalkResult& result)
{
    float nearest = Dist3D(at, end);
    std::uint32_t idle = 0;
    for (std::uint32_t legMoves = 0; legMoves < WALK_MAX_MOVES;)
    {
        float const distance = Dist3D(at, end);
        if (distance <= WALK_GOAL)
            return WalkEnd::Arrived;
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
        ++legMoves;
        ++result.moves;
        if (order)
        {
            WalkStep step;
            kinds |= WalkMove(at, target, path, step, trace);
            if (kinds & WALK_STRAIGHT)
            {
                if (!result.straightSeen)
                    result.firstStraight = step.straightFrom;
                result.straightSeen = true;
                result.straightYd += step.straightYd;
            }
            result.meshYd += step.meshYd;
            result.kinds |= kinds;
            result.areas |= step.areas;
            at = step.land;
        }
        float const now = Dist3D(at, end);
        if (now + WALK_GAIN < nearest)
        {
            nearest = now;
            idle = 0;
        }
        else if (++idle >= WALK_STUCK_MOVES)
            return WalkEnd::Stuck;
    }
    return WalkEnd::Capped;
}

// A walk over waypoints (`mode=path`: the link's stored points between its nodes): from way[0] towards each next
// one in turn, each leg by the same orders, moves, arrival and stuck rules (WalkLeg); it ends with the first leg
// that does not arrive. trace, when given, gets the start and then every point the walk passes
template <class PathFn, class RandFn>
WalkResult WalkPath(std::vector<Vec> const& way, PathFn&& path, RandFn&& rand, std::vector<Vec>* trace = nullptr)
{
    WalkResult result;
    result.end = WalkEnd::Arrived;
    Vec at = way.empty() ? Vec{} : way.front();
    if (trace)
        trace->push_back(at);
    for (std::size_t i = 1; i < way.size() && result.end == WalkEnd::Arrived; ++i)
        result.end = WalkLeg(at, way[i], path, rand, trace, result);
    result.reached = at;
    return result;
}

// The walk from start to end (`mode=nodes`): one leg
template <class PathFn, class RandFn>
WalkResult Walk(Vec const& start, Vec const& end, PathFn&& path, RandFn&& rand, std::vector<Vec>* trace = nullptr)
{
    return WalkPath(std::vector<Vec>{start, end}, path, rand, trace);
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
    static constexpr std::array<std::pair<std::uint32_t, char const*>, 6> NAMES{{{WALK_MESH, "mesh"},
                                                                                 {WALK_INCOMPLETE, "incomplete"},
                                                                                 {WALK_FARPOLY, "farpoly"},
                                                                                 {WALK_STRAIGHT, "straight"},
                                                                                 {WALK_CONE_STONE, "cone"},
                                                                                 {WALK_HOP, "hop"}}};
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
    Capped,       // a length cap, not a fault: listed apart
    NoPath        // `mode=path` and the link has no stored points: not walked, listed apart (NV.15)
};

constexpr int VERDICTS = 7;

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
        case Verdict::NoPath:
            return "no-path";
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
// [from=x,y,z] [areas=a+b]` (at: where the walk ended; from: where its first straight move began; areas: the
// hazard areas its paths touched, only when there are some)
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
    if (walk.areas)
        value += " areas=" + AreasWord(walk.areas);
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

    // capped and no-path links are listed apart, not faults
    std::uint32_t Broken() const
    {
        return links - byClass[static_cast<int>(Verdict::Ok)] - byClass[static_cast<int>(Verdict::Capped)] -
               byClass[static_cast<int>(Verdict::NoPath)];
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

// The walk model's command words (NV19), beside the map words: `model=v20|v21` (v20: the v20 bots' polygons,
// fatigue water allowed and steep ground excluded unless steep= says otherwise), `mode=nodes|path` (node to node,
// or over the link's stored points), `steep=excluded|<cost>` (steep ground left out, or allowed at that area cost
// over 0; the v21 default STEEP_COST)
constexpr float STEEP_COST = 40.0f;  // core v21's headless player filter (PathGenerator::CreateFilter)

struct CheckOptions
{
    bool v20Model{false};
    bool pathMode{false};
    bool steepGiven{false};
    bool steepExcluded{false};
    float steepCost{STEEP_COST};
    bool areasAll{false};  // `areas=all`: lava and fatigue water allowed, and the walks' areas reported

    bool SteepExcluded() const { return steepGiven ? steepExcluded : v20Model; }
    std::string ModelWord() const { return v20Model ? "v20" : "v21"; }
    std::string ModeWord() const { return pathMode ? "path" : "nodes"; }

    std::string SteepWord() const
    {
        if (SteepExcluded())
            return "excluded";
        char text[32];
        std::snprintf(text, sizeof(text), "%g", steepCost);
        return text;
    }

    // The report's name part: `v21-nodes-40`, `v20-path-excluded`
    std::string AreasOption() const { return areasAll ? "all" : "default"; }

    std::string Name() const { return ModelWord() + "-" + ModeWord() + "-" + SteepWord() + (areasAll ? "-areas" : ""); }
};

inline bool IsCheckWord(std::string const& word)
{
    return word.rfind("model=", 0) == 0 || word.rfind("mode=", 0) == 0 || word.rfind("steep=", 0) == 0 ||
           word.rfind("areas=", 0) == 0;
}

// Applies one IsCheckWord word; false when its value is none of the above
inline bool TakeCheckWord(std::string const& word, CheckOptions& options)
{
    std::string const value = word.substr(word.find('=') + 1);
    if (word.rfind("model=", 0) == 0)
    {
        if (value != "v20" && value != "v21")
            return false;
        options.v20Model = value == "v20";
        return true;
    }
    if (word.rfind("mode=", 0) == 0)
    {
        if (value != "nodes" && value != "path")
            return false;
        options.pathMode = value == "path";
        return true;
    }
    if (word.rfind("areas=", 0) == 0)
    {
        if (value != "all" && value != "default")
            return false;
        options.areasAll = value == "all";
        return true;
    }
    if (value == "excluded")
    {
        options.steepGiven = true;
        options.steepExcluded = true;
        return true;
    }
    char* end = nullptr;
    float const cost = std::strtof(value.c_str(), &end);
    if (value.empty() || !end || *end || !std::isfinite(cost) || cost <= 0.0f)
        return false;
    options.steepGiven = true;
    options.steepExcluded = false;
    options.steepCost = cost;
    return true;
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
                  "capped, %u no-path; %u nodes, %u over %.0f yd from the mesh, %u over %.0f yd (%u by a steep "
                  "polygon)",
                  name.c_str(), counts.links, counts.byClass[static_cast<int>(Verdict::Ok)], counts.chained,
                  counts.byClass[static_cast<int>(Verdict::ReverseOnly)],
                  counts.byClass[static_cast<int>(Verdict::EndOffMesh)],
                  counts.byClass[static_cast<int>(Verdict::Dead)], counts.byClass[static_cast<int>(Verdict::Shortcut)],
                  counts.byClass[static_cast<int>(Verdict::Capped)], counts.byClass[static_cast<int>(Verdict::NoPath)],
                  counts.nodes, counts.offMesh, OFFMESH_LISTED, counts.offMesh4, OFFMESH_LIMIT, counts.offMeshSteep);
    return text;
}
}  // namespace RouteCheck

#endif
