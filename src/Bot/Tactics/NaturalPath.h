/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALPATH_H
#define _PLAYERBOT_NATURALPATH_H

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "BotAttributes.h"
#include "Common.h"
#include "NaturalPathConfig.h"
#include "NaturalReaction.h"
#include "SplitMix64.h"
#include "TacticGate.h"

class Player;
class PlayerbotAI;

// Natural paths (G2c), the pure parts. Where MoveTo sent the bot along the straight mmap path, with
// Playerbots.Natural.Path.Enable, the Natural tactic and a PvP context (out of PvP only in a real player's sight) the
// bot wanders: the interior points of the path, plus anchor points every MinLegYards / 2 from the destination, move
// sideways by a smooth profile (a draw per anchor, linear between anchors, 0 at both ends; the amplitude falls with
// Dexterity, OffsetYardsAtSkill1 to OffsetYardsAtSkill99), each moved point checked by the game (the collision check
// from the point it came from, the ground under it, edge probes); a refused added point is left out, a refused path
// point stays where it was. Behind their own flags: curves (Curves.Enable: catmull-rom samples between the moved
// points on open ground, each sample checked; a refused sample leaves the polyline), strafes (Strafe.Enable: a ranged
// bot that is not casting steps 2-4 yards sideways facing its target, at most every StrafeEveryMs), hops
// (Hops.Enable: a short running jump along the first leg of a run, on flat ground, the arc ray-checked, never within
// EdgeYards of an edge, never within 3 s of a cast) and walking (Walk.Enable: short legs out of combat). Every draw
// comes from a hash (no RNG state). The game side is in MovementActions.cpp (MoveTo, FollowNaturalPath) and
// PlayerbotAI.cpp (NaturalPathHere, the params by SetPvpSkill); the numbers are PathConfig's.

constexpr float PATH_PI = 3.14159265358979323846f;
constexpr float PATH_GRAVITY = 19.291105f;        // Movement::gravity (MovementUtil.cpp)
constexpr float PATH_TAPER_YARDS = 3.0f;          // the offset grows from 0 at either end of the path over this
constexpr float PATH_MIN_OFFSET_SHARE = 0.25f;    // an offset draw's size: 0.25 to 2 amplitudes
constexpr float PATH_MAX_OFFSET_SHARE = 2.0f;     // (so every offset is at most 2 amplitudes)
constexpr float PATH_SHAPE_YARDS = 40.0f;         // only the first this many yards of a path wander
constexpr float PATH_ANCHOR_MIN_YARDS = 4.0f;     // the anchors: every MinLegYards / 2 from the end, at least this
constexpr float PATH_NEAR_POINT_SHARE = 0.34f;    // no anchor point within this share of the spacing of a path point
constexpr float PATH_MIN_MOVE_YARDS = 0.05f;      // a smaller offset leaves the point alone
constexpr uint32 PATH_SEED_MS = 3000;             // the wander's draws hold this long (a refreshed chase keeps them)
constexpr float PATH_SEED_CELL_YARDS = 10.0f;     // and while the destination stays in one cell of this size
constexpr float PATH_MAX_DZ = 1.5f;               // the ground under a moved point within this of where it came from
constexpr float PATH_EDGE_DROP = 2.0f;            // a probe this much lower than the point (or no ground) is an edge
constexpr float PATH_OFFSET_EDGE_YARDS = 3.0f;    // a moved point's edge probes (4 directions)
constexpr float PATH_CURVE_STEP_YARDS = 2.5f;     // a curve's samples between two points
constexpr float PATH_CURVE_MAX_TURN_DEG = 45.0f;  // a path turning more at one point is no open ground
constexpr uint32 PATH_MAX_POINTS = 48;            // a path with more points (or a curve that would get more) as found
constexpr float PATH_FRONT_YARDS = 2.0f;          // the path's first point this close to the bot, its last to the goal
constexpr float PATH_STRAFE_MIN_TARGET_YARDS = 8.0f;  // no strafe nearer to the target (melee range)
constexpr uint32 PATH_STRAFE_SWITCH_PCT = 60;         // a strafe goes to the other side than the last one this often
constexpr float PATH_STRAFE_EDGE_YARDS = 4.0f;        // a strafe's end: edge probes this far (8 directions)
constexpr float PATH_HOP_MIN_RUN_YARDS = 10.0f;       // hops only on a run this long
constexpr float PATH_HOP_MIN_YARDS = 2.5f;            // and when the first leg leaves room for a hop this long
constexpr uint32 PATH_HOP_AIR_MS = 700;               // a running jump's time in the air (length = run speed x this)
constexpr float PATH_HOP_FLAT_YARDS = 0.5f;           // the ground under a hop within this of the take-off
constexpr float PATH_HOP_BODY_YARDS = 1.0f;           // the arc's rays at this height above the feet
constexpr uint32 PATH_HOP_CAST_GAP_MS = 3000;         // no hop this soon after a cast
constexpr uint32 PATH_EDGE_DIRECTIONS = 8;
constexpr float PATH_WALK_MIN_YARDS = 1.5f;      // a shorter leg is not worth a walk
constexpr uint32 PATH_MAP_DALARAN_SEWERS = 617;  // no hops while its waterfall is on
constexpr uint32 PATH_MAP_RING_OF_VALOR = 618;   // no hops in its first Tactics.Pillar.RvIdleMs (the elevators)

constexpr uint64 PATH_SEED_SALT = 0x3C6EF372FE94F82BULL;
constexpr uint64 PATH_ANCHOR_SALT = 0xA54FF53A5F1D36F1ULL;
constexpr uint64 PATH_SIDE_SALT = 0x510E527FADE682D2ULL;
constexpr uint64 PATH_STRAFE_SALT = 0x9B05688C2B3E6C20ULL;
constexpr uint64 PATH_DUE_SALT = 0x1F83D9ABFB41BD6CULL;

struct PathVec
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

inline float PathDistance2d(PathVec const& a, PathVec const& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

inline PathVec PathLerp(PathVec const& a, PathVec const& b, float t)
{
    return PathVec{a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t};
}

inline float PathLength2d(std::vector<PathVec> const& points)
{
    float length = 0.0f;
    for (size_t index = 1; index < points.size(); ++index)
        length += PathDistance2d(points[index - 1], points[index]);

    return length;
}

// Why a hop did not go out (tactics show; the refusals of Slope, Arc, Edge and Arena count as pathHopsRefused)
enum class PathHopWhy : uint8
{
    None,
    Short,  // no run long enough, or no room on the first leg
    Slope,  // the ground under the hop is not flat (or missing)
    Arc,    // the arc's rays hit something
    Edge,   // an edge within EdgeYards of the take-off or the landing
    Arena   // Dalaran Sewers' waterfall on, Ring of Valor's first seconds, an arena not started
};

inline char const* PathHopWhyName(PathHopWhy why)
{
    switch (why)
    {
        case PathHopWhy::None:
            return "none";
        case PathHopWhy::Short:
            return "short";
        case PathHopWhy::Slope:
            return "slope";
        case PathHopWhy::Arc:
            return "arc";
        case PathHopWhy::Edge:
            return "edge";
        case PathHopWhy::Arena:
            return "arena";
    }

    return "?";
}

// The natural paths' numbers for one bot (ComputePathParams, cached by PlayerbotAI::SetPvpSkill)
struct PathParams
{
    float OffsetYards = 0.4f;  // the wander's amplitude (Dexterity)
    bool Strafe = true;        // Dexterity >= StrafeFromSkill
    uint32 StrafeEveryMs = 1500;
    bool Hops = true;  // Dexterity >= HopsFromSkill
    uint32 HopEveryMs = 5000;
};

// A style attribute's factor on an interval: 50 (the default) keeps it, 99 makes it 0.7 x (more often), 1 makes it
// 1.3 x; 0 (no attributes) keeps it
inline float PathStyleFactor(uint8 value)
{
    if (value == 0)
        return 1.0f;

    return std::clamp(1.0f + 0.3f * (50.0f - static_cast<float>(value)) / 49.0f, 0.7f, 1.3f);
}

// An interval from its value at the skill it starts at to its value at 99, times the style factor
inline uint32 PathRateMs(uint32 ability, uint32 fromSkill, uint32 atFrom, uint32 at99, float style)
{
    float const span = fromSkill >= 99 ? 1.0f : 99.0f - static_cast<float>(fromSkill);
    float const t = std::clamp((static_cast<float>(ability) - static_cast<float>(fromSkill)) / span, 0.0f, 1.0f);
    float const ms = (static_cast<float>(atFrom) + (static_cast<float>(at99) - static_cast<float>(atFrom)) * t) * style;
    return static_cast<uint32>(std::max(ms, 100.0f) + 0.5f);
}

// Dexterity carries the wander, the strafes and the hops (larger wander at low skill, strafes and hops at high
// skill); Caution makes strafes, Aggression hops more frequent. Skill 0 is full ability (the skill-99 values). With
// the attribute defaults Dexterity is the skill.
inline PathParams ComputePathParams(uint8 skill, BotAttributeValues const& attributes, float exponent,
                                    PathConfig const& config)
{
    uint8 const dexterity = attributes.Get(BotAttribute::Dexterity);
    bool const full = skill == 0 || dexterity == 0;
    uint32 const ability = full ? 99u : dexterity;
    float const position = full ? 1.0f : SkillPosition(dexterity);

    PathParams params;
    params.OffsetYards =
        std::max(SkillLerp(config.OffsetYardsAtSkill1, config.OffsetYardsAtSkill99, position, exponent), 0.0f);
    params.Strafe = ability >= config.StrafeFromSkill;
    params.StrafeEveryMs =
        PathRateMs(ability, config.StrafeFromSkill, config.StrafeEveryMsAtSkill50, config.StrafeEveryMsAtSkill99,
                   PathStyleFactor(attributes.Get(BotAttribute::Caution)));
    params.Hops = ability >= config.HopsFromSkill;
    params.HopEveryMs =
        PathRateMs(ability, config.HopsFromSkill, config.HopsEveryMsAtSkill60, config.HopsEveryMsAtSkill99,
                   PathStyleFactor(attributes.Get(BotAttribute::Aggression)));
    return params;
}

inline uint64 PathHash(uint64 botGuid, uint64 a, uint64 b)
{
    return SplitMix64(SplitMix64(SplitMix64(botGuid ^ PATH_SEED_SALT) ^ a) ^ b);
}

inline uint64 PathCell(float value, float size)
{
    return static_cast<uint64>(static_cast<int64>(std::floor(value / size)));
}

// The wander's seed: the bot, the time bucket and the destination's cell (a refreshed chase keeps its draws)
inline uint64 PathSeed(uint64 botGuid, uint32 nowMs, float x, float y)
{
    return PathHash(botGuid, nowMs / PATH_SEED_MS,
                    (PathCell(x, PATH_SEED_CELL_YARDS) << 32) ^ (PathCell(y, PATH_SEED_CELL_YARDS) & 0xFFFFFFFFULL));
}

// One anchor's lateral offset: amplitude x (1 + sigma x a normal draw), clamped to 0.25-2 amplitudes, either side
inline float PathOffsetDraw(float amplitude, uint32 sigmaPct, uint64 hash)
{
    float const sigma = static_cast<float>(sigmaPct) / 100.0f;
    float const normal = static_cast<float>(Probit(UnitFromHash(hash)));
    float const size = amplitude * std::clamp(1.0f + sigma * normal, PATH_MIN_OFFSET_SHARE, PATH_MAX_OFFSET_SHARE);
    return (SplitMix64(hash ^ PATH_SIDE_SALT) & 1) ? size : -size;
}

// A point of the path to shape: a path point or an added anchor point, with its distance from the end along the path
struct PathNode
{
    PathVec At;
    float FromEnd = 0.0f;
    bool Added = false;
};

// The path's points plus the anchor points (every spacing yards from the end, none within PATH_NEAR_POINT_SHARE of
// the spacing of a path point); total is the path's length
inline std::vector<PathNode> PathNodes(std::vector<PathVec> const& path, float spacing, float& total)
{
    std::vector<float> fromStart(path.size(), 0.0f);
    for (size_t index = 1; index < path.size(); ++index)
        fromStart[index] = fromStart[index - 1] + PathDistance2d(path[index - 1], path[index]);

    total = path.empty() ? 0.0f : fromStart.back();
    float const nearYards = spacing * PATH_NEAR_POINT_SHARE;
    std::vector<PathNode> nodes;
    nodes.reserve(path.size() + static_cast<size_t>(total / spacing) + 1);
    for (size_t index = 0; index < path.size(); ++index)
    {
        if (index > 0)
        {
            float const legStart = total - fromStart[index - 1];
            float const legEnd = total - fromStart[index];
            float const leg = legStart - legEnd;
            if (leg > 0.0f)
                for (int32 k = static_cast<int32>(std::floor((legStart - nearYards) / spacing));
                     k >= 1 && static_cast<float>(k) * spacing > legEnd + nearYards; --k)
                {
                    float const fromEnd = static_cast<float>(k) * spacing;
                    nodes.push_back(
                        PathNode{PathLerp(path[index - 1], path[index], (legStart - fromEnd) / leg), fromEnd, true});
                }
        }

        nodes.push_back(PathNode{path[index], total - fromStart[index], false});
    }

    return nodes;
}

// The largest turn of the path at one of its points, in degrees
inline float PathMaxTurnDeg(std::vector<PathVec> const& path)
{
    float worst = 0.0f;
    for (size_t index = 1; index + 1 < path.size(); ++index)
    {
        float const ax = path[index].X - path[index - 1].X;
        float const ay = path[index].Y - path[index - 1].Y;
        float const bx = path[index + 1].X - path[index].X;
        float const by = path[index + 1].Y - path[index].Y;
        float const lengths = std::hypot(ax, ay) * std::hypot(bx, by);
        if (lengths < 1e-4f)
            continue;

        float const cosine = std::clamp((ax * bx + ay * by) / lengths, -1.0f, 1.0f);
        worst = std::max(worst, std::acos(cosine) * 180.0f / PATH_PI);
    }

    return worst;
}

// Uniform catmull-rom between p1 and p2 (t 0 to 1), as MoveSplineInit::SetSmooth would draw it
inline PathVec PathCatmullRom(PathVec const& p0, PathVec const& p1, PathVec const& p2, PathVec const& p3, float t)
{
    float const t2 = t * t;
    float const t3 = t2 * t;
    auto const blend = [&](float a, float b, float c, float d)
    {
        return 0.5f * (2.0f * b + (c - a) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
                       (3.0f * b - a - 3.0f * c + d) * t3);
    };
    return PathVec{blend(p0.X, p1.X, p2.X, p3.X), blend(p0.Y, p1.Y, p2.Y, p3.Y), blend(p0.Z, p1.Z, p2.Z, p3.Z)};
}

struct PathShape
{
    std::vector<PathVec> Points;  // the path to send (the path as found when nothing changed)
    uint32 Offsets = 0;           // points moved sideways
    uint32 Dropped = 0;           // moves the checks refused (an added point left out, a path point left in place)
    bool Curved = false;
    bool Changed = false;
};

// The wandering path. valid(from, to) is the game's check of a moved point (the collision check from the point it
// came from, the ground under it: it may set to.Z; edge probes), clear(a, b) its open-ground ray between two points.
// A path shorter than MinLegYards, or with more than PATH_MAX_POINTS points, is left as found.
template <typename Valid, typename Clear>
PathShape ShapeNaturalPath(std::vector<PathVec> const& path, PathParams const& params, PathConfig const& config,
                           uint64 seed, bool curves, Valid&& valid, Clear&& clear)
{
    PathShape shape;
    shape.Points = path;
    if (path.size() < 2)
        return shape;

    float total = 0.0f;
    float const spacing = std::max(static_cast<float>(config.MinLegYards) * 0.5f, PATH_ANCHOR_MIN_YARDS);
    std::vector<PathNode> const nodes = PathNodes(path, spacing, total);
    if (total < static_cast<float>(config.MinLegYards) || nodes.size() > PATH_MAX_POINTS)
        return shape;

    bool const offsets = config.OffsetsEnable && params.OffsetYards > 0.0f;
    auto const anchor = [&](uint32 k)
    {
        return k == 0 ? 0.0f
                      : PathOffsetDraw(params.OffsetYards, config.OffsetSigmaPct,
                                       SplitMix64(seed ^ (PATH_ANCHOR_SALT * k)));
    };
    std::vector<PathVec> out;
    out.reserve(nodes.size());
    out.push_back(nodes.front().At);
    for (size_t index = 1; index + 1 < nodes.size(); ++index)
    {
        PathNode const& node = nodes[index];
        float const fromStart = total - node.FromEnd;
        float offset = 0.0f;
        if (offsets && fromStart <= PATH_SHAPE_YARDS)
        {
            float const position = node.FromEnd / spacing;
            uint32 const k = static_cast<uint32>(position);
            float const t = position - static_cast<float>(k);
            float const taper = std::min({1.0f, fromStart / PATH_TAPER_YARDS, node.FromEnd / PATH_TAPER_YARDS});
            float const from = anchor(k);
            offset = (from + (anchor(k + 1) - from) * t) * taper;
        }

        float const dx = nodes[index + 1].At.X - nodes[index - 1].At.X;
        float const dy = nodes[index + 1].At.Y - nodes[index - 1].At.Y;
        float const length = std::hypot(dx, dy);
        if (std::fabs(offset) < PATH_MIN_MOVE_YARDS || length < 0.01f)
        {
            if (!node.Added)
                out.push_back(node.At);

            continue;
        }

        PathVec moved{node.At.X - dy / length * offset, node.At.Y + dx / length * offset, node.At.Z};
        if (valid(node.At, moved))
        {
            out.push_back(moved);
            ++shape.Offsets;
            continue;
        }

        ++shape.Dropped;
        if (!node.Added)
            out.push_back(node.At);
    }
    out.push_back(nodes.back().At);

    // Curves on open ground: no sharp turn in the path, a clear ray between every two points, every sample checked
    if (curves && out.size() >= 3 && PathMaxTurnDeg(path) <= PATH_CURVE_MAX_TURN_DEG)
    {
        bool open = true;
        for (size_t index = 1; open && index < out.size(); ++index)
            open = clear(out[index - 1], out[index]);

        std::vector<PathVec> dense;
        dense.push_back(out.front());
        for (size_t index = 0; open && index + 1 < out.size(); ++index)
        {
            PathVec const& p0 = out[index == 0 ? 0 : index - 1];
            PathVec const& p1 = out[index];
            PathVec const& p2 = out[index + 1];
            PathVec const& p3 = out[std::min(index + 2, out.size() - 1)];
            uint32 const steps = static_cast<uint32>(std::ceil(PathDistance2d(p1, p2) / PATH_CURVE_STEP_YARDS));
            for (uint32 step = 1; open && step < steps; ++step)
            {
                float const t = static_cast<float>(step) / static_cast<float>(steps);
                PathVec const line = PathLerp(p1, p2, t);
                PathVec sample = PathCatmullRom(p0, p1, p2, p3, t);
                sample.Z = line.Z;
                if (PathDistance2d(line, sample) >= PATH_MIN_MOVE_YARDS)
                    open = valid(line, sample);

                dense.push_back(sample);
            }

            dense.push_back(p2);
        }

        if (open && dense.size() > out.size() && dense.size() <= PATH_MAX_POINTS)
        {
            out.swap(dense);
            shape.Curved = true;
        }
    }

    shape.Changed = shape.Offsets > 0 || shape.Curved;
    if (shape.Changed)
        shape.Points.swap(out);

    return shape;
}

// No edge round a point: height(x, y, zHint, z) gives the ground (false: none); every probe on rings rings of
// directions directions up to radius must find ground no more than maxDrop below the point
template <typename Height>
bool PathEdgeClear(PathVec const& at, float radius, uint32 directions, uint32 rings, float maxDrop, Height&& height)
{
    for (uint32 ring = 1; ring <= rings; ++ring)
    {
        float const r = radius * static_cast<float>(ring) / static_cast<float>(rings);
        for (uint32 direction = 0; direction < directions; ++direction)
        {
            float const angle = 2.0f * PATH_PI * static_cast<float>(direction) / static_cast<float>(directions);
            float z = 0.0f;
            if (!height(at.X + r * std::cos(angle), at.Y + r * std::sin(angle), at.Z, z) || at.Z - z > maxDrop)
                return false;
        }
    }

    return true;
}

struct PathHop
{
    PathHopWhy Why = PathHopWhy::None;
    PathVec Land;
    float Yards = 0.0f;
    float SpeedXY = 0.0f;
    float SpeedZ = 0.0f;
    uint32 AirMs = 0;

    bool Ok() const { return Why == PathHopWhy::None; }
};

// A cosmetic hop along the first leg of a run (path[0] the bot): as long as run speed x PATH_HOP_AIR_MS, at most
// HopsMaxLegYards and a yard short of the leg's end; the ground flat under it, the arc's two rays (take-off to apex,
// apex to landing) clear, no edge within HopsEdgeYards of the take-off or the landing
template <typename Height, typename Los>
PathHop PlanNaturalHop(std::vector<PathVec> const& path, float runSpeed, PathConfig const& config, Height&& height,
                       Los&& los)
{
    PathHop hop;
    if (path.size() < 2 || runSpeed <= 0.0f || PathLength2d(path) < PATH_HOP_MIN_RUN_YARDS)
    {
        hop.Why = PathHopWhy::Short;
        return hop;
    }

    PathVec const& from = path[0];
    float const leg = PathDistance2d(from, path[1]);
    float const yards = std::min({static_cast<float>(config.HopsMaxLegYards), leg - 1.0f,
                                  runSpeed * static_cast<float>(PATH_HOP_AIR_MS) / 1000.0f});
    if (yards < PATH_HOP_MIN_YARDS)
    {
        hop.Why = PathHopWhy::Short;
        return hop;
    }

    float const dx = (path[1].X - from.X) / leg;
    float const dy = (path[1].Y - from.Y) / leg;
    PathVec land{from.X + dx * yards, from.Y + dy * yards, from.Z};
    for (float share : {1.0f / 3.0f, 2.0f / 3.0f, 1.0f})
    {
        float z = 0.0f;
        if (!height(from.X + dx * yards * share, from.Y + dy * yards * share, from.Z, z) ||
            std::fabs(z - from.Z) > PATH_HOP_FLAT_YARDS)
        {
            hop.Why = PathHopWhy::Slope;
            return hop;
        }

        land.Z = z;
    }

    float const airSeconds = yards / runSpeed;
    hop.SpeedXY = runSpeed;
    hop.SpeedZ = PATH_GRAVITY * airSeconds * 0.5f;
    float const apex = hop.SpeedZ * hop.SpeedZ / (2.0f * PATH_GRAVITY);
    PathVec const start{from.X, from.Y, from.Z + PATH_HOP_BODY_YARDS};
    PathVec const top{(from.X + land.X) * 0.5f, (from.Y + land.Y) * 0.5f,
                      std::max(from.Z, land.Z) + apex + PATH_HOP_BODY_YARDS};
    PathVec const end{land.X, land.Y, land.Z + PATH_HOP_BODY_YARDS};
    if (!los(start, top) || !los(top, end))
    {
        hop.Why = PathHopWhy::Arc;
        return hop;
    }

    float const edge = static_cast<float>(config.HopsEdgeYards);
    if (!PathEdgeClear(from, edge, PATH_EDGE_DIRECTIONS, 2, PATH_EDGE_DROP, height) ||
        !PathEdgeClear(land, edge, PATH_EDGE_DIRECTIONS, 2, PATH_EDGE_DROP, height))
    {
        hop.Why = PathHopWhy::Edge;
        return hop;
    }

    hop.Land = land;
    hop.Yards = yards;
    hop.AirMs = static_cast<uint32>(airSeconds * 1000.0f + 0.5f);
    return hop;
}

struct PathStrafe
{
    PathVec End;
    float Yards = 0.0f;
    int32 Side = 0;  // 1 left of the line to the target, -1 right
};

// A strafe: 2-4 yards (LegMinYards to LegMaxYards) square to the line to the target; the other side than the last
// strafe PATH_STRAFE_SWITCH_PCT of the time
inline PathStrafe PlanNaturalStrafe(PathVec const& self, PathVec const& target, PathConfig const& config,
                                    int32 lastSide, uint64 hash)
{
    PathStrafe strafe;
    float const dx = target.X - self.X;
    float const dy = target.Y - self.Y;
    float const length = std::hypot(dx, dy);
    if (length < 0.01f)
        return strafe;

    if (lastSide == 0)
        strafe.Side = (hash & 1) ? 1 : -1;
    else
        strafe.Side =
            UniformFromHash(SplitMix64(hash ^ PATH_SIDE_SALT), 1, 100) <= PATH_STRAFE_SWITCH_PCT ? -lastSide : lastSide;

    float const low = static_cast<float>(std::min(config.StrafeLegMinYards, config.StrafeLegMaxYards));
    float const high = static_cast<float>(std::max(config.StrafeLegMinYards, config.StrafeLegMaxYards));
    strafe.Yards = low + (high - low) * static_cast<float>(UnitFromHash(SplitMix64(hash ^ PATH_STRAFE_SALT)));
    float const side = static_cast<float>(strafe.Side) * strafe.Yards / length;
    strafe.End = PathVec{self.X - dy * side, self.Y + dx * side, self.Z};
    return strafe;
}

inline bool PathStrafeAllowed(PathParams const& params, bool casting, bool moving, float targetYards, float maxYards)
{
    return params.Strafe && !casting && !moving && targetYards >= PATH_STRAFE_MIN_TARGET_YARDS &&
           targetYards <= maxYards;
}

inline bool PathReached(uint32 atMs, uint32 nowMs) { return static_cast<int32>(nowMs - atMs) >= 0; }

inline uint32 PathNextDueMs(uint32 nowMs, uint32 everyMs, uint64 hash)
{
    return std::max(nowMs + static_cast<uint32>(static_cast<double>(everyMs) * (0.75 + 0.5 * UnitFromHash(hash))), 1u);
}

// Whether a strafe or hop is due (dueMs: the next one's time); the first call only schedules it, so bots do not all
// strafe at the first check of a fight
inline bool PathDueNow(uint32& dueMs, uint32 nowMs, uint32 everyMs, uint64 hash)
{
    if (dueMs == 0)
    {
        dueMs = PathNextDueMs(nowMs, everyMs, hash ^ PATH_DUE_SALT);
        return false;
    }

    return PathReached(dueMs, nowMs);
}

inline bool PathHopCastGapOk(uint32 lastCastMs, uint32 nowMs)
{
    return lastCastMs == 0 || nowMs - lastCastMs >= PATH_HOP_CAST_GAP_MS;
}

// Walking: a short leg out of combat
inline bool PathWalkLeg(PathConfig const& config, bool inCombat, float yards)
{
    return config.WalkEnable && !inCombat && yards >= PATH_WALK_MIN_YARDS &&
           yards <= static_cast<float>(config.WalkMaxLegYards);
}

// The per-bot state (PlayerbotAI::GetPathState; the bot's own update, tactics show on the world thread)
struct PathBotState
{
    uint32 InSightCheckedMs = 0;  // NaturalPathHere's real-player-in-sight cache (OutsidePvpInSight)
    bool InSight = false;
    PathParams Params;
    uint32 NextStrafeMs = 0;
    uint32 NextHopMs = 0;
    uint32 LastCastMs = 0;  // the last check the bot was casting at (the hops' cast gap)
    int32 LastStrafeSide = 0;
    bool Walking = false;      // the bot walks a leg it was given (the walk flag goes when it stops)
    uint32 ArenaInstance = 0;  // the arena whose start this bot saw (Ring of Valor's wait)
    uint32 ArenaSinceMs = 0;
    // What it did (tactics show)
    uint32 Paths = 0;  // paths sent shaped
    uint32 OffsetPoints = 0;
    uint32 Dropped = 0;
    uint32 Curves = 0;
    uint32 Strafes = 0;
    uint32 StrafesRefused = 0;
    uint32 Hops = 0;
    uint32 HopsRefused = 0;
    uint32 Walks = 0;
    PathHopWhy LastHopWhy = PathHopWhy::None;
    uint32 LastShapeMs = 0;
};

// tactics selftest path (also the offline harness): the numbers by skill, the offset draws, wandering paths (bounded,
// ends kept, refused points dropped, short paths straight, straightness by skill), curves, edges, hops, strafes,
// timing and walking
template <typename Check>
void RunPathChecks(Check&& check)
{
    PathConfig config;
    config.Enable = true;
    auto const attributesAt = [](uint8 ability, uint8 style)
    {
        BotAttributeValues values;
        for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
            values.Value[index] = index >= BOT_ABILITY_COUNT ? style : ability;

        return values;
    };
    auto const paramsAt = [&](uint8 skill) { return ComputePathParams(skill, attributesAt(skill, 50), 1.0f, config); };
    auto const within = [](float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; };

    // The numbers by skill
    PathParams const p1 = paramsAt(1);
    PathParams const p99 = paramsAt(99);
    PathParams const p0 = ComputePathParams(0, attributesAt(0, 50), 1.0f, config);
    check(within(p1.OffsetYards, 1.5f, 1e-4f) && within(p99.OffsetYards, 0.4f, 1e-4f),
          "the wander runs from OffsetYardsAtSkill1 (1.5) to OffsetYardsAtSkill99 (0.4)");
    check(within(p0.OffsetYards, p99.OffsetYards, 1e-5f) && p0.Strafe && p0.Hops, "skill 0 is full ability");
    check(!p1.Strafe && !paramsAt(49).Strafe && paramsAt(50).Strafe && !paramsAt(59).Hops && paramsAt(60).Hops,
          "strafes from StrafeFromSkill (50), hops from HopsFromSkill (60)");
    check(paramsAt(50).StrafeEveryMs == 4000 && p99.StrafeEveryMs == 1500 && paramsAt(60).HopEveryMs == 12000 &&
              p99.HopEveryMs == 5000,
          "the intervals run from their FromSkill values to their skill-99 values");
    PathParams const keen = ComputePathParams(80, attributesAt(80, 99), 1.0f, config);
    PathParams const calm = ComputePathParams(80, attributesAt(80, 1), 1.0f, config);
    PathParams const p80 = paramsAt(80);
    check(keen.StrafeEveryMs < p80.StrafeEveryMs && calm.StrafeEveryMs > p80.StrafeEveryMs &&
              keen.HopEveryMs < p80.HopEveryMs && calm.HopEveryMs > p80.HopEveryMs,
          "style varies the rates (Caution the strafes, Aggression the hops)");

    // The offset draws
    {
        float smallest = 1e9f;
        float largest = 0.0f;
        double sum = 0.0;
        uint32 left = 0;
        uint32 const draws = 20000;
        for (uint32 index = 0; index < draws; ++index)
        {
            float const offset = PathOffsetDraw(1.5f, config.OffsetSigmaPct, SplitMix64(index + 1));
            smallest = std::min(smallest, std::fabs(offset));
            largest = std::max(largest, std::fabs(offset));
            sum += std::fabs(offset);
            left += offset > 0.0f ? 1 : 0;
        }

        check(largest <= 1.5f * PATH_MAX_OFFSET_SHARE + 1e-4f && smallest >= 1.5f * PATH_MIN_OFFSET_SHARE - 1e-4f,
              "offset draws are bounded (0.25 to 2 amplitudes)");
        check(within(static_cast<float>(sum / draws), 1.5f, 0.15f), "offset draws average the amplitude");
        check(left > draws * 45 / 100 && left < draws * 55 / 100, "offset draws go to both sides");
    }

    // Wandering paths on open ground: a smoothed mmap path (a point every 4 yards) and a bare one (no interior point)
    auto const line = [](float length, float step)
    {
        std::vector<PathVec> points;
        for (float x = 0.0f; x < length - 0.01f; x += step)
            points.push_back(PathVec{x, 0.0f, 0.0f});

        points.push_back(PathVec{length, 0.0f, 0.0f});
        return points;
    };
    auto const always = [](PathVec const&, PathVec&) { return true; };
    auto const never = [](PathVec const&, PathVec&) { return false; };
    auto const open = [](PathVec const&, PathVec const&) { return true; };
    auto const farthest = [](std::vector<PathVec> const& points)
    {
        float worst = 0.0f;
        for (PathVec const& point : points)
            worst = std::max(worst, std::fabs(point.Y));

        return worst;
    };
    auto const sameEnds = [](std::vector<PathVec> const& a, std::vector<PathVec> const& b)
    {
        return !a.empty() && !b.empty() && PathDistance2d(a.front(), b.front()) < 1e-5f &&
               PathDistance2d(a.back(), b.back()) < 1e-5f;
    };
    std::vector<PathVec> const run = line(40.0f, 4.0f);
    std::vector<PathVec> const bare = line(40.0f, 40.0f);
    {
        bool ends = true;
        bool bounded = true;
        bool moved = true;
        for (uint64 seed = 1; seed <= 200; ++seed)
            for (std::vector<PathVec> const* path : {&run, &bare})
            {
                PathShape const shape = ShapeNaturalPath(*path, p1, config, seed, false, always, open);
                ends = ends && sameEnds(shape.Points, *path);
                bounded = bounded && farthest(shape.Points) <= p1.OffsetYards * PATH_MAX_OFFSET_SHARE + 1e-3f;
                moved = moved && shape.Changed && shape.Offsets > 0;
            }

        check(ends, "a wandering path keeps its start and its destination");
        check(bounded, "offsets are bounded (at most 2 amplitudes from the path)");
        check(moved, "interior points (and anchors on a bare open-ground path) are moved sideways");
    }
    {
        PathShape const a = ShapeNaturalPath(run, p1, config, 77, false, always, open);
        PathShape const b = ShapeNaturalPath(run, p1, config, 77, false, always, open);
        PathShape const c = ShapeNaturalPath(run, p1, config, 78, false, always, open);
        bool same = a.Points.size() == b.Points.size();
        for (size_t index = 0; same && index < a.Points.size(); ++index)
            same = PathDistance2d(a.Points[index], b.Points[index]) < 1e-6f;

        bool differs = a.Points.size() != c.Points.size();
        for (size_t index = 0; !differs && index < a.Points.size(); ++index)
            differs = PathDistance2d(a.Points[index], c.Points[index]) > 1e-3f;

        check(same && differs, "the wander is a function of its seed (same seed same path, another seed another)");
    }
    {
        auto const straightness = [&](PathParams const& params)
        {
            double sum = 0.0;
            for (uint64 seed = 1; seed <= 200; ++seed)
            {
                PathShape const shape = ShapeNaturalPath(run, params, config, seed, false, always, open);
                sum += 40.0 / PathLength2d(shape.Points);
            }

            return static_cast<float>(sum / 200.0);
        };
        float const low = straightness(p1);
        float const high = straightness(p99);
        check(low >= 0.90f && low <= 0.99f,
              "straightness at skill 1 in the human band (0.90-0.99): " + std::to_string(low));
        check(high > low && high >= 0.99f,
              "straightness rises with skill (skill 99 at least 0.99): " + std::to_string(high));
    }
    {
        std::vector<PathVec> const shortPath = line(10.0f, 4.0f);
        PathShape const shape = ShapeNaturalPath(shortPath, p1, config, 5, false, always, open);
        check(!shape.Changed && shape.Points.size() == shortPath.size(),
              "a path shorter than MinLegYards stays straight");
        PathConfig off = config;
        off.OffsetsEnable = false;
        check(!ShapeNaturalPath(run, p1, off, 5, false, always, open).Changed, "Offsets.Enable 0 leaves the path");
    }

    // Refused points: a wall right of the path refuses every point moved there; a corner the checks refuse stays
    {
        auto const wall = [](PathVec const&, PathVec& to) { return to.Y <= 0.2f; };
        bool kept = true;
        uint32 dropped = 0;
        bool counted = true;
        for (uint64 seed = 1; seed <= 100; ++seed)
        {
            PathShape const shape = ShapeNaturalPath(bare, p1, config, seed, false, wall, open);
            kept = kept && farthest(shape.Points) <= p1.OffsetYards * PATH_MAX_OFFSET_SHARE + 1e-3f;
            for (PathVec const& point : shape.Points)
                kept = kept && point.Y <= 0.2f + 1e-4f;

            dropped += shape.Dropped;
            counted = counted && (!shape.Changed || shape.Points.size() == 2 + shape.Offsets);
        }

        check(kept && dropped > 0, "refused points are dropped (no point beyond the wall)");
        check(counted, "a refused anchor point is left out of the path");
        std::vector<PathVec> corner = line(20.0f, 4.0f);
        for (float y = 4.0f; y <= 20.0f; y += 4.0f)
            corner.push_back(PathVec{20.0f, y, 0.0f});

        PathShape const shape = ShapeNaturalPath(corner, p1, config, 9, false, never, open);
        check(!shape.Changed && shape.Points.size() == corner.size() && shape.Dropped > 0,
              "refused path points stay where the mmap put them");
    }

    // Curves
    {
        PathShape const polyline = ShapeNaturalPath(run, p1, config, 21, false, always, open);
        PathShape const curve = ShapeNaturalPath(run, p1, config, 21, true, always, open);
        bool through = true;
        for (PathVec const& point : polyline.Points)
        {
            bool found = false;
            for (PathVec const& sample : curve.Points)
                found = found || PathDistance2d(point, sample) < 1e-5f;

            through = through && found;
        }

        check(curve.Curved && curve.Points.size() > polyline.Points.size() && curve.Points.size() <= PATH_MAX_POINTS,
              "curves add samples on open ground");
        check(through && sameEnds(curve.Points, run), "a curve passes through the wandering points");
        check(farthest(curve.Points) <= p1.OffsetYards * 2.5f, "a curve stays near the path (2.5 amplitudes)");
        auto const shut = [](PathVec const&, PathVec const&) { return false; };
        PathShape const walled = ShapeNaturalPath(run, p1, config, 21, true, always, shut);
        check(!walled.Curved && walled.Points.size() == polyline.Points.size(), "no curve where a ray is blocked");
        uint32 calls = 0;
        auto const countCalls = [&calls](PathVec const&, PathVec&)
        {
            ++calls;
            return true;
        };
        ShapeNaturalPath(run, p1, config, 21, false, countCalls, open);
        uint32 const offsetCalls = calls;
        calls = 0;
        auto const refuseSample = [&calls, offsetCalls](PathVec const&, PathVec&) { return ++calls <= offsetCalls; };
        PathShape const refused = ShapeNaturalPath(run, p1, config, 21, true, refuseSample, open);
        check(!refused.Curved && refused.Offsets == polyline.Offsets, "a refused curve sample leaves the polyline");
        std::vector<PathVec> corner = line(20.0f, 4.0f);
        for (float y = 4.0f; y <= 20.0f; y += 4.0f)
            corner.push_back(PathVec{20.0f, y, 0.0f});

        check(!ShapeNaturalPath(corner, p1, config, 21, true, always, open).Curved, "no curve round a sharp corner");
    }

    // Edges
    auto const flat = [](float, float, float, float& z)
    {
        z = 0.0f;
        return true;
    };
    auto const cliff = [](float x, float, float, float& z)
    {
        z = x > 5.0f ? -10.0f : 0.0f;
        return true;
    };
    auto const voidGround = [](float, float, float, float&) { return false; };
    check(PathEdgeClear(PathVec{}, 8.0f, PATH_EDGE_DIRECTIONS, 2, PATH_EDGE_DROP, flat) &&
              !PathEdgeClear(PathVec{}, 8.0f, PATH_EDGE_DIRECTIONS, 2, PATH_EDGE_DROP, cliff) &&
              PathEdgeClear(PathVec{-10.0f, 0.0f, 0.0f}, 8.0f, PATH_EDGE_DIRECTIONS, 2, PATH_EDGE_DROP, cliff) &&
              !PathEdgeClear(PathVec{}, 8.0f, PATH_EDGE_DIRECTIONS, 2, PATH_EDGE_DROP, voidGround),
          "edge probes find a drop within the radius and no ground at all");

    // Hops
    {
        auto const clearRay = [](PathVec const&, PathVec const&) { return true; };
        auto const blockedRay = [](PathVec const&, PathVec const&) { return false; };
        std::vector<PathVec> const chase = line(20.0f, 20.0f);
        PathHop const hop = PlanNaturalHop(chase, 7.0f, config, flat, clearRay);
        check(hop.Ok() && within(hop.Yards, 4.9f, 1e-3f) && hop.Yards <= config.HopsMaxLegYards && hop.AirMs == 700 &&
                  within(hop.Land.X, 4.9f, 1e-3f) && within(hop.Land.Y, 0.0f, 1e-4f),
              "a hop on flat ground: run speed x 0.7 s along the leg, at most MaxLegYards");
        float const apex = hop.SpeedZ * hop.SpeedZ / (2.0f * PATH_GRAVITY);
        check(apex > 0.8f && apex < 1.6f, "a hop's apex is a jump's height (0.8-1.6 yd): " + std::to_string(apex));
        check(PlanNaturalHop(line(30.0f, 30.0f), 14.0f, config, flat, clearRay).Yards <= config.HopsMaxLegYards,
              "a fast runner's hop is capped at MaxLegYards");
        auto const slope = [](float x, float, float, float& z)
        {
            z = 0.3f * x;
            return true;
        };
        auto const cliffAhead = [](float x, float, float, float& z)
        {
            z = x > 10.0f ? -10.0f : 0.0f;
            return true;
        };
        auto const cliffBehind = [](float x, float, float, float& z)
        {
            z = x < -3.0f ? -10.0f : 0.0f;
            return true;
        };
        check(PlanNaturalHop(chase, 7.0f, config, slope, clearRay).Why == PathHopWhy::Slope, "no hop on a slope");
        check(PlanNaturalHop(chase, 7.0f, config, cliffAhead, clearRay).Why == PathHopWhy::Edge &&
                  PlanNaturalHop(chase, 7.0f, config, cliffBehind, clearRay).Why == PathHopWhy::Edge,
              "no hop within EdgeYards of an edge (ahead of the landing or behind the take-off)");
        check(PlanNaturalHop(chase, 7.0f, config, voidGround, clearRay).Why == PathHopWhy::Slope,
              "no hop where the ground is missing");
        check(PlanNaturalHop(chase, 7.0f, config, flat, blockedRay).Why == PathHopWhy::Arc, "no hop through a wall");
        std::vector<PathVec> const stub{PathVec{0.0f, 0.0f, 0.0f}, PathVec{3.0f, 0.0f, 0.0f},
                                        PathVec{20.0f, 0.0f, 0.0f}};
        check(PlanNaturalHop(line(8.0f, 8.0f), 7.0f, config, flat, clearRay).Why == PathHopWhy::Short &&
                  PlanNaturalHop(stub, 7.0f, config, flat, clearRay).Why == PathHopWhy::Short,
              "no hop on a short run or a short first leg");
        check(!PathHopCastGapOk(10000, 12999) && PathHopCastGapOk(10000, 13000) && PathHopCastGapOk(0, 500),
              "no hop within 3 s of a cast");
    }

    // Timing: the first check schedules, then about one event per interval
    {
        uint32 due = 0;
        uint32 events = 0;
        bool first = PathDueNow(due, 1000, 5000, 3);
        for (uint32 now = 1100; now <= 121000; now += 100)
            if (PathDueNow(due, now, 5000, SplitMix64(now)))
            {
                ++events;
                due = PathNextDueMs(now, 5000, SplitMix64(now + 7));
            }

        check(!first && events >= 19 && events <= 29,
              "a 5 s interval gives 19-29 events in 120 s (first check schedules): " + std::to_string(events));
    }

    // Strafes
    {
        PathVec const self{0.0f, 0.0f, 0.0f};
        PathVec const target{20.0f, 0.0f, 0.0f};
        bool square = true;
        uint32 switches = 0;
        int32 side = 0;
        for (uint64 index = 1; index <= 1000; ++index)
        {
            PathStrafe const strafe = PlanNaturalStrafe(self, target, config, side, SplitMix64(index));
            square = square && within(strafe.End.X, 0.0f, 1e-3f) && strafe.Yards >= 2.0f && strafe.Yards <= 4.0f &&
                     within(std::fabs(strafe.End.Y), strafe.Yards, 1e-3f) &&
                     std::fabs(PathDistance2d(strafe.End, target) - 20.0f) < 0.5f;
            switches += side != 0 && strafe.Side != side ? 1 : 0;
            side = strafe.Side;
        }

        check(square, "a strafe is 2-4 yd square to the line to the target");
        check(switches >= 520 && switches <= 680,
              "strafes switch sides about 60 % of the time: " + std::to_string(switches));
        check(PathStrafeAllowed(p99, false, false, 20.0f, 30.0f) &&
                  !PathStrafeAllowed(p99, true, false, 20.0f, 30.0f) &&
                  !PathStrafeAllowed(p99, false, true, 20.0f, 30.0f) &&
                  !PathStrafeAllowed(p99, false, false, 5.0f, 30.0f) &&
                  !PathStrafeAllowed(p99, false, false, 45.0f, 30.0f) &&
                  !PathStrafeAllowed(p1, false, false, 20.0f, 30.0f),
              "no strafe while casting, moving, in melee range, out of range or below StrafeFromSkill");
    }

    // Walking
    {
        PathConfig walk = config;
        walk.WalkEnable = true;
        check(PathWalkLeg(walk, false, 4.0f) && !PathWalkLeg(walk, true, 4.0f) && !PathWalkLeg(walk, false, 10.0f) &&
                  !PathWalkLeg(walk, false, 1.0f) && !PathWalkLeg(config, false, 4.0f),
              "walking on short legs out of combat only (Walk.Enable)");
    }
}

// Whether the bot walks natural paths now (PlayerbotAI.cpp; as NaturalTrackHere with the Path options)
bool NaturalPathHere(PlayerbotAI* botAI, uint32 nowMs);

// Per check after the bot's action (PlayerbotAI::UpdateAIInternal; MovementActions.cpp): ends a walked leg's walk
// flag, notes casts for the hops' gap, strafes a ranged bot between casts
void FollowNaturalPath(PlayerbotAI* botAI, Player* bot, uint32 nowMs);

#endif
