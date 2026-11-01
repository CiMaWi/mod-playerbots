/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAJUMP_H
#define _PLAYERBOT_ARENAJUMP_H

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "ArenaCover.h"
#include "Common.h"

class ChatHandler;
class Player;

// Arena jump spots (G2g, PLAN 3.15, DT.13), the pure parts. A server MoveJump is a parabolic spline that ignores
// collision in flight and ends exactly at the given point, so a tactical jump is safe only from a measured spot:
// a from-point on an upper floor, a landing point on a lower one and a direction. This header holds
// - JumpArc: the spline's arc (MoveSpline::computeParabolicElevation: the straight line from the bot to the landing
//   plus a bump of 4 apex s (1 - s), apex = speedZ^2 / 2g) and CheckJumpArc, which samples it every half yard and
//   tests each piece by rays at the body and the head (no wall, no ceiling) and the floor under each point (the arc
//   does not pass under a floor);
// - CheckJumpLanding: the landing stands on ground (within 1 yd below the planned point, UpdateAllowedPositionZ's
//   rule); the navmesh and the path to the arena centre are the caller's (PathGenerator);
// - JumpScan: candidate edges over the arena's box (a 1 yd grid of floor levels; the floor ends and drops more than
//   3 yd within 2 yd, the floor continues behind the from-point, a flat landing open to the sky 3-8 yd out), clustered
//   4 yd apart; plus the hand-entered rows (ARENA_JUMP_HINTS) and Ring of Valor's pillar tops listed as refused;
// - JumpTestMachine: `tactics jump test` (stage 6 yd behind the spot, walk in, jump, wait for the landing, settle,
//   measure, walk back on the first jump; n jumps per spot) with a record and a failure mode per jump.
// Rays and floors are injected callables (the map's in game, a synthetic arena in the selftest). The game side is
// ArenaJump.cpp (the commands, the static checks, PlayerbotAI::StepJumpTest on the bot's map thread).

constexpr float JUMP_GRAVITY = 19.291105f;          // Movement::gravity (MovementUtil.cpp)
constexpr float JUMP_SPEED_Z = 7.9555473f;          // a player's own jump (the client's jump velocity): apex 1.64 yd
constexpr float JUMP_SAMPLE_YARDS = 0.5f;           // the arc is sampled this often along its straight line
constexpr float JUMP_BODY_YARDS = 1.0f;             // the arc's rays at the body ...
constexpr float JUMP_HEAD_YARDS = 2.0f;             // ... and at the head (a ceiling)
constexpr float JUMP_STAND_YARDS = 1.0f;            // a landing needs ground within this below the planned point
constexpr float JUMP_UNDER_FLOOR_YARDS = 0.25f;     // an arc point this far under a floor passes through it ...
constexpr float JUMP_END_SKIP_YARDS = 0.75f;        // ... tested from this far from either end (a sloped floor)
constexpr float JUMP_CONNECT_FACTOR = 3.0f;         // a landing connects: a path to the centre at most 3 x straight
constexpr float JUMP_NO_FLOOR = -100000.0f;         // a scan cell's empty level
constexpr uint32 JUMP_SCAN_LEVELS = 3;              // floor levels per scan cell
constexpr uint32 JUMP_ID_REFUSED_PILLAR = 51;       // Ring of Valor's pillar tops: ids 51-54 (refused)
constexpr uint32 JUMP_ID_SCAN = 101;                // scan candidates: ids from 101 in scan order
constexpr uint32 JUMP_GATE_DS_WATERFALL = 1u << 8;  // the test's gate bits: Ring of Valor's pillars (1-4), the
                                                    // Dalaran waterfall on (8)

enum class JumpFail : uint8
{
    None,
    ArcBlocked,       // the arc crosses a wall, a ceiling or a floor
    LandingNoGround,  // no ground within 1 yd under the landing
    LandingOffMesh,   // the landing is off the navmesh
    LandingError,     // the bot ended further than the tolerance from the planned point
    UnderMap,         // the bot ended more than 2 yd under the landing
    NoPath,           // no navmesh path from the landing to the arena centre
    StuckWalk,        // the walk to the from-point did not arrive
    StuckBack,        // the walk back from the landing did not arrive (no progress)
    Timeout,          // the staging teleport or the flight did not end in time
    HealthLoss,       // the jump cost health (no fall damage is expected: bots send no fall packets)
    Count
};

inline char const* JumpFailName(JumpFail fail)
{
    switch (fail)
    {
        case JumpFail::None:
            return "none";
        case JumpFail::ArcBlocked:
            return "arcBlocked";
        case JumpFail::LandingNoGround:
            return "landingNoGround";
        case JumpFail::LandingOffMesh:
            return "landingOffMesh";
        case JumpFail::LandingError:
            return "landingError";
        case JumpFail::UnderMap:
            return "underMap";
        case JumpFail::NoPath:
            return "noPath";
        case JumpFail::StuckWalk:
            return "stuckWalk";
        case JumpFail::StuckBack:
            return "stuckBack";
        case JumpFail::Timeout:
            return "timeout";
        case JumpFail::HealthLoss:
            return "healthLoss";
        default:
            return "?";
    }
}

enum class JumpKind : uint8
{
    Escape = 1,
    Chase = 2,
    Both = 3
};

inline char const* JumpKindName(JumpKind kind)
{
    return kind == JumpKind::Escape ? "escape" : kind == JumpKind::Chase ? "chase" : "both";
}

// --- the arc --------------------------------------------------------------------------------------------------

struct JumpArc
{
    float SpeedXY = 0.0f;
    float SpeedZ = 0.0f;
    float Apex = 0.0f;        // the bump at mid-time above the straight line (speedZ^2 / 2g)
    float Length = 0.0f;      // the straight line's 3D length (the spline's length)
    float AirSeconds = 0.0f;  // Length / SpeedXY (the spline's duration)
};

inline JumpArc MakeJumpArc(CoverVec const& from, CoverVec const& land, float speedXY, float speedZ)
{
    JumpArc arc;
    arc.SpeedXY = speedXY;
    arc.SpeedZ = speedZ;
    arc.Apex = speedZ * speedZ / (2.0f * JUMP_GRAVITY);
    float const dx = land.X - from.X;
    float const dy = land.Y - from.Y;
    float const dz = land.Z - from.Z;
    arc.Length = std::sqrt(dx * dx + dy * dy + dz * dz);
    arc.AirSeconds = speedXY > 0.1f ? arc.Length / speedXY : 0.0f;
    return arc;
}

// The point at share s (0..1) of the air time: the straight line plus (T - t) * 0.5 * a * t with a = 8 apex / T^2
// (MoveSpline::computeParabolicElevation), i.e. 4 apex s (1 - s); zero at both ends, apex at mid-time
inline CoverVec JumpArcAt(CoverVec const& from, CoverVec const& land, JumpArc const& arc, float s)
{
    s = std::clamp(s, 0.0f, 1.0f);
    return {from.X + (land.X - from.X) * s, from.Y + (land.Y - from.Y) * s,
            from.Z + (land.Z - from.Z) * s + 4.0f * arc.Apex * s * (1.0f - s)};
}

inline CoverVec JumpLift(CoverVec const& point, float yards) { return {point.X, point.Y, point.Z + yards}; }

inline float JumpDist2d(CoverVec const& a, CoverVec const& b)
{
    float const dx = a.X - b.X;
    float const dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

struct JumpArcResult
{
    bool Pass = false;
    uint32 Samples = 0;
    uint32 Rays = 0;
    int32 BlockedSample = -1;  // the first blocked sample (-1: none)
    char const* Why = "";      // "wall" (the body ray), "ceiling" (the head ray), "floor" (an arc point under a floor)
    float TopZ = 0.0f;         // the arc's highest point
};

// los(a, b): true when the ray from a to b is clear; floorBelow(x, y, zTop, z): the highest floor at or below zTop
template <typename Los, typename Floor>
JumpArcResult CheckJumpArc(CoverVec const& from, CoverVec const& land, JumpArc const& arc, Los&& los,
                           Floor&& floorBelow, float sampleYards = JUMP_SAMPLE_YARDS)
{
    JumpArcResult result;
    if (arc.SpeedXY <= 0.1f || arc.Length < 0.01f || sampleYards <= 0.01f)
    {
        result.Why = "no speed";
        return result;
    }

    uint32 const steps = std::max<uint32>(2, static_cast<uint32>(std::ceil(arc.Length / sampleYards)));
    result.Samples = steps + 1;
    result.TopZ = std::max(from.Z, land.Z);
    CoverVec previous = from;
    for (uint32 index = 1; index <= steps; ++index)
    {
        float const s = static_cast<float>(index) / static_cast<float>(steps);
        CoverVec const point = JumpArcAt(from, land, arc, s);
        result.TopZ = std::max(result.TopZ, point.Z);
        ++result.Rays;
        if (!los(JumpLift(previous, JUMP_BODY_YARDS), JumpLift(point, JUMP_BODY_YARDS)))
        {
            result.BlockedSample = static_cast<int32>(index);
            result.Why = "wall";
            return result;
        }

        ++result.Rays;
        if (!los(JumpLift(previous, JUMP_HEAD_YARDS), JumpLift(point, JUMP_HEAD_YARDS)))
        {
            result.BlockedSample = static_cast<int32>(index);
            result.Why = "ceiling";
            return result;
        }

        float const along = s * arc.Length;
        float floorZ = 0.0f;
        if (index < steps && along > JUMP_END_SKIP_YARDS && arc.Length - along > JUMP_END_SKIP_YARDS &&
            floorBelow(point.X, point.Y, point.Z + JUMP_HEAD_YARDS, floorZ) &&
            floorZ > point.Z + JUMP_UNDER_FLOOR_YARDS)
        {
            result.BlockedSample = static_cast<int32>(index);
            result.Why = "floor";
            return result;
        }

        previous = point;
    }

    result.Pass = true;
    return result;
}

// MoveJump's arc is the straight line plus a symmetric bump, so off a high edge the natural jump (JUMP_SPEED_Z)
// cuts through the ledge it leaves; PlanJumpArc tries higher jumps (up to 2 x the speed, 4 x the apex) and takes the
// first that clears (none: the natural jump's result, for its reason)
constexpr std::array<float, 5> JUMP_SPEED_Z_STEPS = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};

struct JumpPlan
{
    JumpArc Arc;
    JumpArcResult Check;
};

template <typename Los, typename Floor>
JumpPlan PlanJumpArc(CoverVec const& from, CoverVec const& land, float speedXY, Los&& los, Floor&& floorBelow)
{
    JumpPlan first;
    for (size_t index = 0; index < JUMP_SPEED_Z_STEPS.size(); ++index)
    {
        JumpPlan plan;
        plan.Arc = MakeJumpArc(from, land, speedXY, JUMP_SPEED_Z * JUMP_SPEED_Z_STEPS[index]);
        plan.Check = CheckJumpArc(from, land, plan.Arc, los, floorBelow);
        if (plan.Check.Pass)
            return plan;

        if (!index)
            first = plan;
    }

    return first;
}

struct JumpLanding
{
    bool Ground = false;
    float GroundZ = 0.0f;
};

// The landing stands on ground: a floor within JUMP_STAND_YARDS below (or at) the planned point
template <typename Floor>
JumpLanding CheckJumpLanding(CoverVec const& land, Floor&& floorBelow)
{
    JumpLanding landing;
    float z = 0.0f;
    if (floorBelow(land.X, land.Y, land.Z + JUMP_STAND_YARDS, z) && std::fabs(land.Z - z) <= JUMP_STAND_YARDS)
    {
        landing.Ground = true;
        landing.GroundZ = z;
    }

    return landing;
}

// The static checks of a spot (the arc from the from-point, the landing's ground, navmesh and connection)
struct JumpStatic
{
    bool Checked = false;
    bool ArcPass = false;
    float SpeedZ = JUMP_SPEED_Z;  // the planned jump's vertical speed (PlanJumpArc)
    char const* ArcWhy = "";
    uint32 ArcRays = 0;
    float AirMs = 0.0f;
    bool Ground = false;
    float GroundZ = 0.0f;
    bool OnMesh = false;
    bool Connected = false;
    bool WayBack = false;  // a navmesh path leads from the landing back to the from-point (else a one-way drop)
    uint32 PathType = 0;   // the core's PathType bits of the path to the centre (the report reads them)
    float PathYards = 0.0f;
    float StraightYards = 0.0f;
};

inline JumpFail JumpStaticVerdict(JumpStatic const& check)
{
    if (!check.ArcPass)
        return JumpFail::ArcBlocked;

    if (!check.Ground)
        return JumpFail::LandingNoGround;

    if (!check.OnMesh)
        return JumpFail::LandingOffMesh;

    if (!check.Connected)
        return JumpFail::NoPath;

    return JumpFail::None;
}

// A path connects when it is a whole navmesh path no longer than JUMP_CONNECT_FACTOR x the straight distance
inline bool JumpPathConnects(bool normalPath, float pathYards, float straightYards)
{
    return normalPath && pathYards <= JUMP_CONNECT_FACTOR * std::max(straightYards, 1.0f);
}

// --- spots ----------------------------------------------------------------------------------------------------

struct JumpSpot
{
    uint32 MapId = 0;
    uint32 Id = 0;
    std::string Name;
    CoverVec From;
    CoverVec Land;
    float Yaw = 0.0f;
    float Drop = 0.0f;
    JumpKind Kind = JumpKind::Both;
    uint8 Gate = 0;                 // the cover gate the spot depends on (0: none)
    bool Hand = false;              // a hand-entered row (ARENA_JUMP_HINTS)
    char const* Refused = nullptr;  // why the spot is not tested (nullptr: tested)
    JumpStatic Static;
    CoverVec Staging;  // the test's staging point (6 yd behind the from-point; the from-point when no floor there)
};

// Hand-entered rows: a from-point (x, y and a height hint, resolved to the floor nearest the hint), a direction and
// how far out the landing lies (resolved to the first floor at least 2 yd under the from-point). Blade's Edge's
// rope bridge runs between the off-mesh links' inner ends (mmaps-config.yaml: (6234.47, 256.56, 11.06) and
// (6242.27, 266.70, 11.09)); the rows drop off its middle to either side. Dalaran's pipe mouths, Lordaeron's and
// Nagrand's ramps come from the scan (not from memory).
struct JumpHint
{
    uint32 MapId;
    uint32 Id;
    char const* Name;
    float X;
    float Y;
    float ZHint;
    float Yaw;
    float Out;
    JumpKind Kind;
};

inline constexpr std::array<JumpHint, 2> ARENA_JUMP_HINTS = {{
    {COVER_MAP_BLADES_EDGE, 1, "bridge drop south-west", 6238.37f, 261.63f, 11.1f, 2.486f, 5.0f, JumpKind::Both},
    {COVER_MAP_BLADES_EDGE, 2, "bridge drop north-east", 6238.37f, 261.63f, 11.1f, 5.627f, 5.0f, JumpKind::Both},
}};

constexpr char const* JUMP_REFUSED_RV_PILLAR =
    "a Ring of Valor pillar top: a transport that rises and sinks (no navmesh on it; it moves under the bot)";

template <typename Floor>
JumpSpot ResolveJumpHint(JumpHint const& hint, Floor&& floorBelow)
{
    JumpSpot spot;
    spot.MapId = hint.MapId;
    spot.Id = hint.Id;
    spot.Name = hint.Name;
    spot.Yaw = hint.Yaw;
    spot.Kind = hint.Kind;
    spot.Hand = true;
    spot.From = {hint.X, hint.Y, hint.ZHint};
    spot.Land = {hint.X + hint.Out * std::cos(hint.Yaw), hint.Y + hint.Out * std::sin(hint.Yaw), hint.ZHint};
    float fromZ = 0.0f;
    if (!floorBelow(hint.X, hint.Y, hint.ZHint + 1.5f, fromZ) || std::fabs(fromZ - hint.ZHint) > 2.5f)
    {
        spot.Refused = "no floor at the hint's from-point";
        return spot;
    }

    spot.From.Z = fromZ;
    float landZ = 0.0f;
    if (!floorBelow(spot.Land.X, spot.Land.Y, fromZ - 2.0f, landZ))
    {
        spot.Refused = "no floor under the hint's landing";
        return spot;
    }

    spot.Land.Z = landZ;
    spot.Drop = fromZ - landZ;
    return spot;
}

// Ring of Valor's pillar tops as refused rows (listed so that the report names them)
inline std::vector<JumpSpot> JumpRefusedPillarSpots(ArenaCoverTable const& table)
{
    std::vector<JumpSpot> spots;
    if (table.MapId != COVER_MAP_RING_OF_VALOR)
        return spots;

    for (uint32 index = 0; index < table.Count; ++index)
    {
        CoverPillar const& pillar = table.Pillars[index];
        JumpSpot spot;
        spot.MapId = table.MapId;
        spot.Id = JUMP_ID_REFUSED_PILLAR + index;
        spot.Name = "pillar top " + std::to_string(index);
        spot.Yaw = std::atan2(table.CentreY - pillar.Y, table.CentreX - pillar.X);
        spot.From = {pillar.X, pillar.Y, pillar.Z};
        spot.Land = {pillar.X + (pillar.R + 3.0f) * std::cos(spot.Yaw),
                     pillar.Y + (pillar.R + 3.0f) * std::sin(spot.Yaw), pillar.Z};
        spot.Gate = pillar.Gate;
        spot.Hand = true;
        spot.Refused = JUMP_REFUSED_RV_PILLAR;
        spots.push_back(spot);
    }

    return spots;
}

// --- the scan -------------------------------------------------------------------------------------------------

struct JumpScanParams
{
    float CentreX = 0.0f;
    float CentreY = 0.0f;
    float RefZ = 0.0f;
    float HalfSize = 44.0f;
    float Step = 1.0f;         // the grid step (yards)
    float ZUp = 20.0f;         // levels from RefZ + ZUp ...
    float ZDown = 12.0f;       // ... down to RefZ - ZDown
    float LevelGap = 2.0f;     // the next level lies at least this under the one above (a floor's thickness)
    float MinDrop = 3.0f;      // an edge drops more than this ...
    float EdgeYards = 2.0f;    // ... within this
    float StepTol = 1.0f;      // a neighbour level within this is the same floor (behind the from-point)
    float RiseTol = 1.5f;      // a neighbour level up to this above the edge's level continues the floor
    float LandMin = 3.0f;      // the landing lies this far out at least ...
    float LandMax = 8.0f;      // ... and at most
    float FlatTol = 0.6f;      // the landing cell's four neighbours have a level within this
    float Cluster = 4.0f;      // spots of the same level and direction lie this far apart
    float ClusterYaw = 1.05f;  // directions closer than this (60 degrees) are the same
    uint32 MaxSpots = 48;
};

struct JumpCandidate
{
    CoverVec From;
    CoverVec Land;
    float Yaw = 0.0f;
    float Drop = 0.0f;
};

struct JumpScanResult
{
    std::vector<JumpCandidate> Spots;  // clustered, nearest the centre first
    uint32 Cells = 0;
    uint32 FloorCells = 0;
    uint32 Levels = 0;
    uint32 Edges = 0;  // raw edge candidates before clustering
    bool Capped = false;
};

inline float JumpYawDiff(float a, float b)
{
    float diff = std::fmod(std::fabs(a - b), COVER_TWO_PI);
    return diff > COVER_PI ? COVER_TWO_PI - diff : diff;
}

template <typename Floor>
JumpScanResult JumpScan(JumpScanParams const& params, Floor&& floorBelow)
{
    JumpScanResult result;
    if (params.Step <= 0.1f || params.HalfSize <= 0.0f)
        return result;

    int32 const half = static_cast<int32>(std::floor(params.HalfSize / params.Step));
    int32 const side = 2 * half + 1;
    using Levels = std::array<float, JUMP_SCAN_LEVELS>;
    std::vector<Levels> grid(static_cast<size_t>(side) * static_cast<size_t>(side));
    auto const cellX = [&](int32 i) { return params.CentreX + static_cast<float>(i - half) * params.Step; };
    auto const cellY = [&](int32 j) { return params.CentreY + static_cast<float>(j - half) * params.Step; };
    auto const cell = [&](int32 i, int32 j) -> Levels const*
    {
        if (i < 0 || j < 0 || i >= side || j >= side)
            return nullptr;

        return &grid[static_cast<size_t>(i) * static_cast<size_t>(side) + static_cast<size_t>(j)];
    };

    for (int32 i = 0; i < side; ++i)
    {
        for (int32 j = 0; j < side; ++j)
        {
            Levels& levels = grid[static_cast<size_t>(i) * static_cast<size_t>(side) + static_cast<size_t>(j)];
            levels.fill(JUMP_NO_FLOOR);
            ++result.Cells;
            float top = params.RefZ + params.ZUp;
            for (uint32 k = 0; k < JUMP_SCAN_LEVELS; ++k)
            {
                float z = 0.0f;
                if (!floorBelow(cellX(i), cellY(j), top, z) || z < params.RefZ - params.ZDown || z > top + 0.01f)
                    break;

                levels[k] = z;
                ++result.Levels;
                top = z - params.LevelGap;
            }

            result.FloorCells += levels[0] > JUMP_NO_FLOOR ? 1 : 0;
        }
    }

    auto const hasLevelIn = [](Levels const* levels, float low, float high)
    {
        if (!levels)
            return false;

        for (float z : *levels)
        {
            if (z > JUMP_NO_FLOOR && z >= low && z <= high)
                return true;
        }

        return false;
    };
    auto const highestBelow = [](Levels const* levels, float below)
    {
        float best = JUMP_NO_FLOOR;
        if (levels)
        {
            for (float z : *levels)
            {
                if (z > JUMP_NO_FLOOR && z < below)
                    best = std::max(best, z);
            }
        }

        return best;
    };

    static constexpr std::array<std::array<int32, 2>, 8> DIRECTIONS = {
        {{{1, 0}}, {{1, 1}}, {{0, 1}}, {{-1, 1}}, {{-1, 0}}, {{-1, -1}}, {{0, -1}}, {{1, -1}}}};
    std::vector<JumpCandidate> raw;
    for (int32 i = 0; i < side; ++i)
    {
        for (int32 j = 0; j < side; ++j)
        {
            Levels const& levels = *cell(i, j);
            for (float zU : levels)
            {
                if (zU <= JUMP_NO_FLOOR)
                    continue;

                for (std::array<int32, 2> const& direction : DIRECTIONS)
                {
                    int32 const dx = direction[0];
                    int32 const dy = direction[1];
                    float const cellYards = params.Step * ((dx && dy) ? 1.41421356f : 1.0f);
                    int32 const edgeCells = std::max(1, static_cast<int32>(std::lround(params.EdgeYards / cellYards)));
                    // the floor continues behind the from-point (not a railing or a thin ledge) ...
                    if (!hasLevelIn(cell(i - dx, j - dy), zU - params.StepTol, zU + params.StepTol))
                        continue;

                    // ... and ends ahead: no floor near its level, a floor more than MinDrop under it
                    Levels const* edge = cell(i + edgeCells * dx, j + edgeCells * dy);
                    if (!edge || hasLevelIn(edge, zU - params.MinDrop, zU + params.RiseTol) ||
                        highestBelow(edge, zU - params.MinDrop) <= JUMP_NO_FLOOR)
                        continue;

                    ++result.Edges;
                    int32 const firstCell =
                        std::max(edgeCells, static_cast<int32>(std::ceil(params.LandMin / cellYards)));
                    int32 const lastCell = static_cast<int32>(std::floor(params.LandMax / cellYards));
                    for (int32 t = firstCell; t <= lastCell; ++t)
                    {
                        int32 const li = i + t * dx;
                        int32 const lj = j + t * dy;
                        Levels const* landing = cell(li, lj);
                        float const zL = highestBelow(landing, zU - params.MinDrop);
                        if (zL <= JUMP_NO_FLOOR)
                            continue;

                        // open above up to the jumper's level (not under a bridge or an overhang) and flat
                        if (hasLevelIn(landing, zL + params.LevelGap, zU + params.RiseTol))
                            continue;

                        bool flat = true;
                        for (std::array<int32, 2> const& n :
                             {std::array<int32, 2>{{1, 0}}, std::array<int32, 2>{{-1, 0}}, std::array<int32, 2>{{0, 1}},
                              std::array<int32, 2>{{0, -1}}})
                            flat = flat &&
                                   hasLevelIn(cell(li + n[0], lj + n[1]), zL - params.FlatTol, zL + params.FlatTol);

                        if (!flat)
                            continue;

                        JumpCandidate candidate;
                        candidate.From = {cellX(i), cellY(j), zU};
                        candidate.Land = {cellX(li), cellY(lj), zL};
                        candidate.Yaw = std::atan2(static_cast<float>(dy), static_cast<float>(dx));
                        if (candidate.Yaw < 0.0f)
                            candidate.Yaw += COVER_TWO_PI;

                        candidate.Drop = zU - zL;
                        raw.push_back(candidate);
                        break;
                    }
                }
            }
        }
    }

    // nearest the centre first, the larger drop first among equals; then one spot per 4 yd, level and direction
    CoverVec const centre{params.CentreX, params.CentreY, params.RefZ};
    std::stable_sort(raw.begin(), raw.end(),
                     [&centre](JumpCandidate const& a, JumpCandidate const& b)
                     {
                         float const da = JumpDist2d(a.From, centre);
                         float const db = JumpDist2d(b.From, centre);
                         if (std::fabs(da - db) > 0.01f)
                             return da < db;

                         return a.Drop > b.Drop;
                     });
    for (JumpCandidate const& candidate : raw)
    {
        bool near_kept = false;
        for (JumpCandidate const& kept : result.Spots)
        {
            if (JumpDist2d(kept.From, candidate.From) < params.Cluster &&
                std::fabs(kept.From.Z - candidate.From.Z) < params.LevelGap &&
                JumpYawDiff(kept.Yaw, candidate.Yaw) < params.ClusterYaw)
            {
                near_kept = true;
                break;
            }
        }

        if (near_kept)
            continue;

        if (result.Spots.size() >= params.MaxSpots)
        {
            result.Capped = true;
            break;
        }

        result.Spots.push_back(candidate);
    }

    return result;
}

// --- the test machine -----------------------------------------------------------------------------------------

enum class JumpTestStage : uint8
{
    Idle,
    Stage,   // teleport to the staging point
    Arrive,  // wait for the teleport
    Walk,    // walk to the from-point
    Aim,     // jump
    Air,     // wait for the landing
    Settle,  // wait, then measure
    Back,    // walk back to the from-point (first jump only)
    Done
};

enum class JumpTestCmdKind : uint8
{
    None,
    Teleport,  // NearTeleportTo(Point)
    Walk,      // MovePoint(Point) on the navmesh
    Jump,      // MoveJump(Point) with the machine's speeds; the caller then checks the arc (NoteArc)
    Measure,   // fill a JumpTestMeasure and call NoteMeasure
    Finish     // the spot is done (Records, Passed)
};

struct JumpTestCmd
{
    JumpTestCmdKind Kind = JumpTestCmdKind::None;
    CoverVec Point;
    float Yaw = 0.0f;
    float SpeedZ = JUMP_SPEED_Z;  // a Jump's vertical speed (the spot's plan)
};

struct JumpTestObs
{
    uint32 NowMs = 0;
    CoverVec Pos;
    bool Airborne = false;  // the jump's spline runs (an effect movement)
    bool Moving = false;    // any spline runs
    uint32 Health = 0;
    uint32 Gates = 0;  // ArenaCoverActiveGates | JUMP_GATE_DS_WATERFALL
};

struct JumpTestMeasure
{
    bool GroundValid = false;
    float GroundZ = 0.0f;
    bool OnMesh = false;         // the path's start is on the navmesh (not PATHFIND_FARFROMPOLY_START)
    bool PathOk = false;         // a whole path to the arena centre, at most 3 x straight
    bool BackPath = true;        // a navmesh path back to the from-point (else a one-way drop: no walk back)
    float BackPathYards = 0.0f;  // the path back to the from-point (sizes the walk back's deadline)
};

struct JumpTestLimits
{
    float LandTol = 2.0f;    // Playerbots.Tactics.Jump.LandingTolX10 / 10
    float FallYards = 2.0f;  // further under the landing: under the map
    float StageBack = 6.0f;  // the staging point lies this far behind the from-point
    float ArriveYards = 0.75f;
    float RunSpeed = 7.0f;
    uint32 WalkTimeoutMs = 12000;
    uint32 AirTimeoutMs = 6000;
    uint32 SettleMs = 1500;
    uint32 MaxStuckMs = 8000;
    uint32 TeleportTimeoutMs = 5000;
    uint32 RewalkMs = 1000;  // a walk that stopped short is sent again after this
};

struct JumpTestRecord
{
    JumpFail Fail = JumpFail::None;
    uint32 WalkMs = 0;
    uint32 AirMs = 0;
    uint32 ExpectedAirMs = 0;
    uint32 BackMs = 0;  // the walk back (first jump; 0 otherwise)
    float Err2d = 0.0f;
    float ErrZ = 0.0f;
    float GroundDelta = 0.0f;  // the bot's z minus the ground under it after the landing
    int32 HealthDelta = 0;
    bool ArcPass = true;  // the arc from the real start (NoteArc)
    bool Connected = false;
    uint32 Gates = 0;
};

class JumpTestMachine
{
public:
    void Start(JumpSpot const& spot, CoverVec const& staging, uint32 jumps, JumpTestLimits const& limits, uint32 nowMs)
    {
        spot_ = spot;
        staging_ = staging;
        jumps_ = jumps;
        limits_ = limits;
        records_.clear();
        record_ = JumpTestRecord{};
        stage_ = jumps ? JumpTestStage::Stage : JumpTestStage::Done;
        stageMs_ = nowMs;
        finished_ = false;
    }

    JumpTestCmd Step(JumpTestObs const& obs)
    {
        JumpTestCmd cmd;
        uint32 const now = obs.NowMs;
        switch (stage_)
        {
            case JumpTestStage::Idle:
                break;
            case JumpTestStage::Stage:
                record_ = JumpTestRecord{};
                cmd.Kind = JumpTestCmdKind::Teleport;
                cmd.Point = staging_;
                cmd.Yaw = spot_.Yaw;
                Enter(JumpTestStage::Arrive, now);
                break;
            case JumpTestStage::Arrive:
                if (JumpDist2d(obs.Pos, staging_) <= 1.5f && std::fabs(obs.Pos.Z - staging_.Z) <= 3.0f)
                {
                    cmd = WalkTo(spot_.From, now);
                    Enter(JumpTestStage::Walk, now);
                }
                else if (now - stageMs_ > limits_.TeleportTimeoutMs)
                    EndJump(JumpFail::Timeout, obs, cmd);

                break;
            case JumpTestStage::Walk:
                if (JumpDist2d(obs.Pos, spot_.From) <= limits_.ArriveYards &&
                    std::fabs(obs.Pos.Z - spot_.From.Z) <= 1.5f)
                {
                    record_.WalkMs = now - stageMs_;
                    Enter(JumpTestStage::Aim, now);
                }
                else if (now - stageMs_ > limits_.WalkTimeoutMs)
                {
                    record_.WalkMs = now - stageMs_;
                    EndJump(JumpFail::StuckWalk, obs, cmd);
                }
                else if (!obs.Moving && now - walkMs_ >= limits_.RewalkMs)
                    cmd = WalkTo(spot_.From, now);

                break;
            case JumpTestStage::Aim:
            {
                cmd.Kind = JumpTestCmdKind::Jump;
                cmd.Point = spot_.Land;
                cmd.Yaw = spot_.Yaw;
                cmd.SpeedZ = spot_.Static.SpeedZ;
                JumpArc const arc = MakeJumpArc(obs.Pos, spot_.Land, limits_.RunSpeed, cmd.SpeedZ);
                record_.ExpectedAirMs = static_cast<uint32>(arc.AirSeconds * 1000.0f);
                healthBefore_ = obs.Health;
                Enter(JumpTestStage::Air, now);
                break;
            }
            case JumpTestStage::Air:
                if (obs.Airborne)
                {
                    if (now - stageMs_ > limits_.AirTimeoutMs)
                    {
                        record_.AirMs = now - stageMs_;
                        EndJump(JumpFail::Timeout, obs, cmd);
                    }
                }
                else if (now > stageMs_)
                {
                    record_.AirMs = now - stageMs_;
                    Enter(JumpTestStage::Settle, now);
                }

                break;
            case JumpTestStage::Settle:
                if (now - stageMs_ >= limits_.SettleMs)
                    cmd.Kind = JumpTestCmdKind::Measure;

                break;
            case JumpTestStage::Back:
                if (JumpDist2d(obs.Pos, spot_.From) <= 1.5f && std::fabs(obs.Pos.Z - spot_.From.Z) <= 2.0f)
                {
                    record_.BackMs = now - stageMs_;
                    EndJump(JumpFail::None, obs, cmd);
                }
                else if (JumpDist2d(obs.Pos, progressPos_) >= 1.0f || std::fabs(obs.Pos.Z - progressPos_.Z) >= 1.0f)
                {
                    progressPos_ = obs.Pos;
                    progressMs_ = now;
                    if (!obs.Moving && now - walkMs_ >= limits_.RewalkMs)
                        cmd = WalkTo(spot_.From, now);
                }
                else if (now - progressMs_ > limits_.MaxStuckMs || now - stageMs_ > backLimitMs_)
                {
                    record_.BackMs = now - stageMs_;
                    EndJump(JumpFail::StuckBack, obs, cmd);
                }
                else if (!obs.Moving && now - walkMs_ >= limits_.RewalkMs)
                    cmd = WalkTo(spot_.From, now);

                break;
            case JumpTestStage::Done:
                if (!finished_)
                {
                    finished_ = true;
                    cmd.Kind = JumpTestCmdKind::Finish;
                }

                break;
        }

        return cmd;
    }

    // the caller's arc check from the real start, right after a Jump command
    void NoteArc(bool pass) { record_.ArcPass = pass; }

    // the caller's measurement after a Measure command; the next Step walks back (first jump) or goes on
    JumpTestCmd NoteMeasure(JumpTestMeasure const& measure, JumpTestObs const& obs)
    {
        JumpTestCmd cmd;
        if (stage_ != JumpTestStage::Settle)
            return cmd;

        record_.Err2d = JumpDist2d(obs.Pos, spot_.Land);
        record_.ErrZ = obs.Pos.Z - spot_.Land.Z;
        record_.GroundDelta = measure.GroundValid ? obs.Pos.Z - measure.GroundZ : 0.0f;
        record_.HealthDelta = static_cast<int32>(obs.Health) - static_cast<int32>(healthBefore_);
        record_.Connected = measure.PathOk;
        record_.Gates = obs.Gates;
        JumpFail fail = JumpFail::None;
        if (obs.Pos.Z < spot_.Land.Z - limits_.FallYards)
            fail = JumpFail::UnderMap;
        else if (record_.Err2d > limits_.LandTol || std::fabs(record_.ErrZ) > limits_.LandTol)
            fail = JumpFail::LandingError;
        else if (!measure.GroundValid || std::fabs(record_.GroundDelta) > JUMP_STAND_YARDS)
            fail = JumpFail::LandingNoGround;
        else if (!measure.OnMesh)
            fail = JumpFail::LandingOffMesh;
        else if (!measure.PathOk)
            fail = JumpFail::NoPath;
        else if (record_.HealthDelta < 0)
            fail = JumpFail::HealthLoss;
        else if (!record_.ArcPass)
            fail = JumpFail::ArcBlocked;

        if (fail == JumpFail::None && records_.empty() && measure.BackPath)
        {
            // the first jump also walks back up (the landing connects live, not only by the navmesh query)
            float const expectedMs = measure.BackPathYards / std::max(limits_.RunSpeed, 1.0f) * 1000.0f;
            backLimitMs_ = std::max(limits_.MaxStuckMs, static_cast<uint32>(expectedMs * 1.5f) + 2000);
            progressPos_ = obs.Pos;
            progressMs_ = obs.NowMs;
            cmd = WalkTo(spot_.From, obs.NowMs);
            Enter(JumpTestStage::Back, obs.NowMs);
            return cmd;
        }

        EndJump(fail, obs, cmd);
        return cmd;
    }

    JumpTestStage Stage() const { return stage_; }
    bool Done() const { return stage_ == JumpTestStage::Done; }
    JumpSpot const& Spot() const { return spot_; }
    std::vector<JumpTestRecord> const& Records() const { return records_; }
    uint32 Jumps() const { return jumps_; }

    uint32 Passes() const
    {
        uint32 passes = 0;
        for (JumpTestRecord const& record : records_)
            passes += record.Fail == JumpFail::None ? 1 : 0;

        return passes;
    }

    // a spot passes when every one of its n jumps passed
    bool Passed() const { return jumps_ > 0 && records_.size() == jumps_ && Passes() == jumps_; }

private:
    void Enter(JumpTestStage stage, uint32 nowMs)
    {
        stage_ = stage;
        stageMs_ = nowMs;
    }

    JumpTestCmd WalkTo(CoverVec const& point, uint32 nowMs)
    {
        walkMs_ = nowMs;
        JumpTestCmd cmd;
        cmd.Kind = JumpTestCmdKind::Walk;
        cmd.Point = point;
        cmd.Yaw = spot_.Yaw;
        return cmd;
    }

    void EndJump(JumpFail fail, JumpTestObs const& obs, JumpTestCmd& cmd)
    {
        record_.Fail = fail;
        if (fail == JumpFail::StuckWalk || fail == JumpFail::Timeout)
            record_.Gates = obs.Gates;

        records_.push_back(record_);
        record_ = JumpTestRecord{};
        if (records_.size() >= jumps_)
        {
            Enter(JumpTestStage::Done, obs.NowMs);
            finished_ = true;
            cmd.Kind = JumpTestCmdKind::Finish;
            return;
        }

        Enter(JumpTestStage::Stage, obs.NowMs);
    }

    JumpSpot spot_;
    CoverVec staging_;
    uint32 jumps_ = 0;
    JumpTestLimits limits_;
    JumpTestStage stage_ = JumpTestStage::Idle;
    uint32 stageMs_ = 0;
    uint32 walkMs_ = 0;
    uint32 healthBefore_ = 0;
    uint32 backLimitMs_ = 0;
    CoverVec progressPos_;
    uint32 progressMs_ = 0;
    bool finished_ = false;
    JumpTestRecord record_;
    std::vector<JumpTestRecord> records_;
};

// Per bot (PlayerbotAI::GetJumpState): the test's queue of spots, armed by `tactics jump test` on the world thread
// (while no map updates) and stepped by PlayerbotAI::StepJumpTest on the bot's map thread. Frozen: the jump test
// drives the bot, so UpdateAI runs no engine (PlayerbotAI::jumpTest).
struct JumpBotState
{
    bool Frozen = false;
    bool Running = false;  // Machine holds Queue[Next]
    uint32 MapId = 0;
    uint32 InstanceId = 0;
    uint32 Jumps = 0;
    uint32 Next = 0;
    uint32 Passed = 0;
    uint32 ArmedMs = 0;
    CoverVec Centre;  // the arena centre's floor (the landing's path target)
    std::vector<JumpSpot> Queue;
    JumpTestLimits Limits;
    JumpTestMachine Machine;
};

// The game side (ArenaJump.cpp, world thread): `tactics jump scan` and `tactics jump test`
bool RunArenaJumpScan(ChatHandler* handler, Player* player);
bool RunArenaJumpTest(ChatHandler* handler, char const* args);

// --- the selftest ---------------------------------------------------------------------------------------------

// The selftest (tactics selftest jump; the offline harness runs the same checks): the arc's maths (ends, apex, air
// time), the arc's rays (clear, wall, ceiling, under a floor), the landing rules, the static verdict, the hand rows,
// the scan on a synthetic arena (a raised platform's edge found, a low step not, clustering) and the test machine
// with a fake bot and clock (a pass, every failure mode)
template <typename Check>
void RunJumpChecks(Check&& check)
{
    auto const near_eq = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

    // the arc
    CoverVec const from{0.0f, 0.0f, 6.0f};
    CoverVec const land{4.0f, 0.0f, 0.0f};
    JumpArc const arc = MakeJumpArc(from, land, 7.0f, JUMP_SPEED_Z);
    check(near_eq(arc.Apex, 1.640f, 0.01f),
          "the player's jump velocity gives a 1.64 yd apex: " + std::to_string(arc.Apex));
    check(near_eq(arc.Length, std::sqrt(52.0f), 0.001f), "the arc's length is the straight line's");
    check(near_eq(arc.AirSeconds, std::sqrt(52.0f) / 7.0f, 0.001f), "air time = length / speedXY");
    CoverVec const start = JumpArcAt(from, land, arc, 0.0f);
    CoverVec const end = JumpArcAt(from, land, arc, 1.0f);
    CoverVec const mid = JumpArcAt(from, land, arc, 0.5f);
    check(near_eq(start.X, 0.0f, 1e-4f) && near_eq(start.Z, 6.0f, 1e-4f), "the arc starts at the from-point");
    check(near_eq(end.X, 4.0f, 1e-4f) && near_eq(end.Z, 0.0f, 1e-4f), "the arc ends exactly at the landing");
    check(near_eq(mid.Z, 3.0f + arc.Apex, 1e-4f), "the apex is at mid-time, apex above the straight line");
    CoverVec const quarter = JumpArcAt(from, land, arc, 0.25f);
    check(near_eq(quarter.Z, 4.5f + 0.75f * arc.Apex, 1e-4f), "the bump is 4 apex s (1 - s)");
    check(MakeJumpArc(from, land, 0.05f, JUMP_SPEED_Z).AirSeconds == 0.0f, "no speed: no air time (MoveJump refuses)");

    // a synthetic arena: a platform at z 6 for x < 0 and y < 0, a step at z 2 for x < 0 and y >= 0, the floor at z 0
    auto const floorBelow = [](float x, float y, float zTop, float& z)
    {
        float best = -1.0e9f;
        if (x < -0.01f && y < -0.01f && zTop >= 6.0f)
            best = 6.0f;
        else if (x < -0.01f && y >= -0.01f && zTop >= 2.0f)
            best = 2.0f;
        else if (zTop >= 0.0f)
            best = 0.0f;

        if (best < -1.0e8f)
            return false;

        z = best;
        return true;
    };
    auto const clear = [](CoverVec const&, CoverVec const&) { return true; };
    JumpArcResult const clearArc = CheckJumpArc(from, land, arc, clear, floorBelow);
    check(clearArc.Pass && clearArc.Rays >= 2 * 14 && clearArc.Samples >= 15,
          "a clear arc passes with two rays per half yard: " + std::to_string(clearArc.Rays));
    check(clearArc.TopZ >= from.Z && clearArc.TopZ < from.Z + arc.Apex, "a 6 yd drop's top lies just past the start");
    uint32 calls = 0;
    auto const wall = [&calls](CoverVec const& a, CoverVec const& b)
    {
        ++calls;
        return (a.X - 2.0f) * (b.X - 2.0f) > 0.0f || std::max(a.Z, b.Z) > 20.0f;  // a wall at x = 2, 20 yd high
    };
    JumpArcResult const walled = CheckJumpArc(from, land, arc, wall, floorBelow);
    check(!walled.Pass && std::string(walled.Why) == "wall" && walled.BlockedSample > 0 && walled.Rays == calls,
          "a wall across the arc blocks it at the body ray, rays counted");
    auto const slab = [](CoverVec const& a, CoverVec const& b)
    {
        // a ceiling slab z 7.6-8.0 over x 0.5-3.5: a ray piece inside it is blocked
        for (uint32 k = 0; k <= 8; ++k)
        {
            float const s = static_cast<float>(k) / 8.0f;
            float const x = a.X + (b.X - a.X) * s;
            float const z = a.Z + (b.Z - a.Z) * s;
            if (x > 0.5f && x < 3.5f && z > 7.6f && z < 8.0f)
                return false;
        }

        return true;
    };
    JumpArcResult const ceiling = CheckJumpArc(from, land, arc, slab, floorBelow);
    check(!ceiling.Pass && std::string(ceiling.Why) == "ceiling", "a low ceiling blocks the arc at the head ray");
    CoverVec const lowFrom{-6.0f, -2.0f, 6.0f};
    CoverVec const lowLand{6.0f, -2.0f, 0.0f};
    JumpArc const flat = MakeJumpArc(lowFrom, lowLand, 7.0f, 1.0f);  // a low arc: under the platform's floor
    JumpArcResult const under = CheckJumpArc(lowFrom, lowLand, flat, clear, floorBelow);
    check(!under.Pass && std::string(under.Why) == "floor", "an arc that dips under a floor is blocked");
    JumpPlan const nearEdge =
        PlanJumpArc(CoverVec{-1.0f, -3.0f, 6.0f}, CoverVec{3.0f, -3.0f, 0.0f}, 7.0f, clear, floorBelow);
    check(nearEdge.Check.Pass && nearEdge.Arc.SpeedZ == JUMP_SPEED_Z, "1 yd from the edge the natural jump clears");
    JumpPlan const backEdge =
        PlanJumpArc(CoverVec{-3.0f, -3.0f, 6.0f}, CoverVec{3.0f, -3.0f, 0.0f}, 7.0f, clear, floorBelow);
    check(backEdge.Check.Pass && backEdge.Arc.SpeedZ > 1.2f * JUMP_SPEED_Z,
          "3 yd from the edge a higher jump clears the ledge: speedZ " + std::to_string(backEdge.Arc.SpeedZ));
    JumpPlan const blockedPlan = PlanJumpArc(from, land, 7.0f, wall, floorBelow);
    check(!blockedPlan.Check.Pass && blockedPlan.Arc.SpeedZ == JUMP_SPEED_Z &&
              std::string(blockedPlan.Check.Why) == "wall",
          "no height clears a wall: the natural jump's reason");

    // the landing
    check(CheckJumpLanding(land, floorBelow).Ground, "a landing on the floor stands");
    check(!CheckJumpLanding(CoverVec{4.0f, 0.0f, 1.5f}, floorBelow).Ground,
          "a landing 1.5 yd over the floor: no ground");
    auto const none = [](float, float, float, float&) { return false; };
    check(!CheckJumpLanding(land, none).Ground, "a landing over nothing: no ground");

    // the static verdict, in order
    JumpStatic good;
    good.ArcPass = good.Ground = good.OnMesh = good.Connected = true;
    check(JumpStaticVerdict(good) == JumpFail::None, "a spot that passes every static check");
    JumpStatic bad = good;
    bad.Connected = false;
    check(JumpStaticVerdict(bad) == JumpFail::NoPath, "no connection: noPath");
    bad.OnMesh = false;
    check(JumpStaticVerdict(bad) == JumpFail::LandingOffMesh, "off the mesh before noPath");
    bad.Ground = false;
    check(JumpStaticVerdict(bad) == JumpFail::LandingNoGround, "no ground before off the mesh");
    bad.ArcPass = false;
    check(JumpStaticVerdict(bad) == JumpFail::ArcBlocked, "the arc first");
    check(JumpPathConnects(true, 20.0f, 10.0f) && !JumpPathConnects(true, 31.0f, 10.0f) &&
              !JumpPathConnects(false, 5.0f, 10.0f),
          "a path connects when whole and at most 3 x straight");
    bool names = true;
    for (uint32 index = 0; index < static_cast<uint32>(JumpFail::Count); ++index)
        names = names && std::string(JumpFailName(static_cast<JumpFail>(index))) != "?";

    check(names, "every failure mode has a name");

    // the hand rows: on arena maps, unique ids below the refused pillar ids
    bool hints = true;
    for (JumpHint const& hint : ARENA_JUMP_HINTS)
    {
        hints = hints && FindArenaCoverTable(hint.MapId) && hint.Id > 0 && hint.Id < JUMP_ID_REFUSED_PILLAR &&
                hint.Out > 1.0f;
        for (JumpHint const& other : ARENA_JUMP_HINTS)
            hints = hints && (&other == &hint || other.MapId != hint.MapId || other.Id != hint.Id);
    }

    check(hints, "the hand rows sit on arena maps with unique ids");
    JumpHint const synthetic{COVER_MAP_NAGRAND, 1, "test", -2.0f, -2.0f, 6.0f, 0.0f, 5.0f, JumpKind::Escape};
    JumpSpot const resolved = ResolveJumpHint(synthetic, floorBelow);
    check(!resolved.Refused && near_eq(resolved.From.Z, 6.0f, 1e-4f) && near_eq(resolved.Land.X, 3.0f, 1e-4f) &&
              near_eq(resolved.Land.Z, 0.0f, 1e-4f) && near_eq(resolved.Drop, 6.0f, 1e-4f),
          "a hint resolves to the floors under it");
    JumpHint const nowhere{COVER_MAP_NAGRAND, 2, "test", -2.0f, -2.0f, 20.0f, 0.0f, 5.0f, JumpKind::Escape};
    check(ResolveJumpHint(nowhere, floorBelow).Refused != nullptr, "a hint far from any floor is refused");
    ArenaCoverTable const* rv = FindArenaCoverTable(COVER_MAP_RING_OF_VALOR);
    std::vector<JumpSpot> const pillars = rv ? JumpRefusedPillarSpots(*rv) : std::vector<JumpSpot>{};
    check(rv && pillars.size() == rv->Count && !pillars.empty() && pillars.front().Refused &&
              pillars.front().Id == JUMP_ID_REFUSED_PILLAR,
          "Ring of Valor's pillar tops are listed as refused");
    ArenaCoverTable const* na = FindArenaCoverTable(COVER_MAP_NAGRAND);
    check(na && JumpRefusedPillarSpots(*na).empty(), "no refused pillar rows elsewhere");

    // the scan on the synthetic arena
    JumpScanParams params;
    params.HalfSize = 10.0f;
    params.ZUp = 10.0f;
    params.ZDown = 2.0f;
    JumpScanResult const scan = JumpScan(params, floorBelow);
    check(scan.Cells == 21u * 21u && scan.FloorCells == scan.Cells, "the scan covers its box");
    check(
        !scan.Spots.empty() && scan.Edges >= scan.Spots.size(),
        "the platform's edge gives spots: " + std::to_string(scan.Spots.size()) + " of " + std::to_string(scan.Edges));
    bool platformOnly = true;
    bool landings = true;
    bool towardFloor = false;
    for (JumpCandidate const& spot : scan.Spots)
    {
        platformOnly = platformOnly && near_eq(spot.From.Z, 6.0f, 1e-4f) && spot.From.X < 0.0f && spot.From.Y < 0.0f;
        landings = landings && (spot.Land.X > 0.0f || spot.Land.Y > 0.0f) && spot.Drop > params.MinDrop &&
                   near_eq(spot.Drop, spot.From.Z - spot.Land.Z, 1e-4f) &&
                   JumpDist2d(spot.From, spot.Land) >= params.LandMin - 0.01f &&
                   JumpDist2d(spot.From, spot.Land) <= params.LandMax * 1.42f;
        towardFloor = towardFloor || (spot.Yaw < 0.01f && near_eq(spot.Drop, 6.0f, 1e-4f));
    }

    check(platformOnly, "spots only off the 6 yd platform, none off the 2 yd step");
    check(landings, "landings off the platform, more than 3 yd down, 3-8 yd out");
    check(towardFloor, "a spot drops 6 yd straight out (+x) to the floor");
    bool apart = true;
    for (size_t a = 0; a < scan.Spots.size(); ++a)
    {
        for (size_t b = a + 1; b < scan.Spots.size(); ++b)
        {
            if (JumpYawDiff(scan.Spots[a].Yaw, scan.Spots[b].Yaw) < params.ClusterYaw)
                apart = apart && JumpDist2d(scan.Spots[a].From, scan.Spots[b].From) >= params.Cluster;
        }
    }

    check(apart, "spots of one direction lie 4 yd apart");
    JumpScanParams capped = params;
    capped.MaxSpots = 1;
    check(JumpScan(capped, floorBelow).Spots.size() == 1 && JumpScan(capped, floorBelow).Capped, "the spot cap holds");
    check(JumpScan(params, none).Spots.empty(), "no floor: no spots");
    check(near_eq(JumpYawDiff(0.1f, COVER_TWO_PI - 0.1f), 0.2f, 1e-4f), "yaw differences wrap");

    // the test machine with a fake bot and clock (100 ms ticks)
    struct FakeBot
    {
        CoverVec Pos{-10.0f, -10.0f, 6.0f};
        CoverVec Target;
        bool Walking = false;
        bool Airborne = false;
        uint32 LandAtMs = 0;
        CoverVec LandPoint;
        uint32 Health = 1000;
    };
    struct Faults
    {
        bool WalkBlocked = false;
        bool BackBlocked = false;
        bool NeverLands = false;
        float LandOffsetX = 0.0f;
        float LandOffsetZ = 0.0f;
        uint32 HealthLoss = 0;
        bool NoGround = false;
        bool OffMesh = false;
        bool NoPath = false;
        bool ArcBlocked = false;
        bool OneWay = false;
    };
    JumpSpot testSpot;
    testSpot.MapId = COVER_MAP_NAGRAND;
    testSpot.Id = 101;
    testSpot.From = {-1.0f, -3.0f, 6.0f};
    testSpot.Land = {3.0f, -3.0f, 0.0f};
    testSpot.Yaw = 0.0f;
    CoverVec const staging{-7.0f, -3.0f, 6.0f};
    auto const run = [&](Faults const& faults, uint32 jumps) -> JumpTestMachine
    {
        JumpTestLimits limits;
        JumpTestMachine machine;
        FakeBot bot;
        uint32 now = 1000;
        machine.Start(testSpot, staging, jumps, limits, now);
        for (uint32 tick = 0; tick < 6000 && !machine.Done(); ++tick)
        {
            now += 100;
            // move the fake bot
            if (bot.Airborne && !faults.NeverLands && now >= bot.LandAtMs)
            {
                bot.Airborne = false;
                bot.Pos = bot.LandPoint;
            }

            if (bot.Walking)
            {
                bool const back = bot.Pos.Z < 3.0f;
                bool const blocked = back ? faults.BackBlocked : faults.WalkBlocked;
                float const dist = JumpDist2d(bot.Pos, bot.Target);
                if (!blocked && dist <= 0.7f)
                {
                    bot.Pos = bot.Target;
                    bot.Walking = false;
                }
                else if (!blocked)
                {
                    bot.Pos.X += (bot.Target.X - bot.Pos.X) / dist * 0.7f;
                    bot.Pos.Y += (bot.Target.Y - bot.Pos.Y) / dist * 0.7f;
                    if (back && bot.Pos.X < 0.0f)
                        bot.Pos.Z = 6.0f;  // the way back up (a ramp in the fake)
                }
            }

            JumpTestObs obs;
            obs.NowMs = now;
            obs.Pos = bot.Pos;
            obs.Airborne = bot.Airborne;
            obs.Moving = bot.Walking || bot.Airborne;
            obs.Health = bot.Health;
            JumpTestCmd cmd = machine.Step(obs);
            if (cmd.Kind == JumpTestCmdKind::Teleport)
            {
                bot.Pos = cmd.Point;
                bot.Walking = false;
            }
            else if (cmd.Kind == JumpTestCmdKind::Walk)
            {
                bot.Target = cmd.Point;
                bot.Walking = true;
            }
            else if (cmd.Kind == JumpTestCmdKind::Jump)
            {
                JumpArc const jumpArc = MakeJumpArc(bot.Pos, cmd.Point, limits.RunSpeed, cmd.SpeedZ);
                bot.Walking = false;
                bot.Airborne = true;
                bot.LandAtMs = now + static_cast<uint32>(jumpArc.AirSeconds * 1000.0f);
                bot.LandPoint = {cmd.Point.X + faults.LandOffsetX, cmd.Point.Y, cmd.Point.Z + faults.LandOffsetZ};
                bot.Health -= std::min(bot.Health, faults.HealthLoss);
                machine.NoteArc(!faults.ArcBlocked);
            }
            else if (cmd.Kind == JumpTestCmdKind::Measure)
            {
                JumpTestMeasure measure;
                measure.GroundValid = !faults.NoGround;
                measure.GroundZ = 0.0f;
                measure.OnMesh = !faults.OffMesh;
                measure.PathOk = !faults.NoPath;
                measure.BackPath = !faults.OneWay;
                measure.BackPathYards = 8.0f;
                obs.Pos = bot.Pos;
                cmd = machine.NoteMeasure(measure, obs);
                if (cmd.Kind == JumpTestCmdKind::Walk)
                {
                    bot.Target = cmd.Point;
                    bot.Walking = true;
                }
            }
        }

        return machine;
    };
    auto const failsWith = [](JumpTestMachine const& machine, JumpFail fail)
    {
        for (JumpTestRecord const& record : machine.Records())
        {
            if (record.Fail == fail)
                return true;
        }

        return false;
    };

    JumpTestMachine const ok = run(Faults{}, 3);
    check(ok.Done() && ok.Records().size() == 3 && ok.Passed() && ok.Passes() == 3, "three clean jumps pass the spot");
    bool timings = !ok.Records().empty();
    for (JumpTestRecord const& record : ok.Records())
    {
        timings = timings && record.WalkMs > 0 && record.AirMs + 200 >= record.ExpectedAirMs &&
                  record.AirMs <= record.ExpectedAirMs * 6 / 5 + 200 && record.Err2d < 0.01f && record.Connected;
    }

    check(timings, "walk and air times recorded, air within 20 % of the spline's");
    check(
        !ok.Records().empty() && ok.Records()[0].BackMs > 0 && (ok.Records().size() < 2 || ok.Records()[1].BackMs == 0),
        "only the first jump walks back");
    Faults offset;
    offset.LandOffsetX = 3.0f;
    JumpTestMachine const missed = run(offset, 2);
    check(!missed.Passed() && failsWith(missed, JumpFail::LandingError) && missed.Records().size() == 2,
          "a landing 3 yd off is a landing error");
    Faults sunk;
    sunk.LandOffsetZ = -5.0f;
    check(failsWith(run(sunk, 1), JumpFail::UnderMap), "a landing 5 yd under the point is under the map");
    Faults blocked;
    blocked.WalkBlocked = true;
    JumpTestMachine const stuck = run(blocked, 2);
    check(failsWith(stuck, JumpFail::StuckWalk) && stuck.Done() && stuck.Records().size() == 2,
          "a blocked walk fails each jump and the machine still ends");
    Faults hanging;
    hanging.NeverLands = true;
    check(failsWith(run(hanging, 1), JumpFail::Timeout), "a flight that never ends times out");
    Faults hurt;
    hurt.HealthLoss = 50;
    check(failsWith(run(hurt, 1), JumpFail::HealthLoss), "a jump that costs health fails");
    Faults backStuck;
    backStuck.BackBlocked = true;
    JumpTestMachine const back = run(backStuck, 2);
    check(
        failsWith(back, JumpFail::StuckBack) && back.Records().size() == 2 && back.Records()[1].Fail == JumpFail::None,
        "a stuck walk back fails the first jump only");
    Faults ground;
    ground.NoGround = true;
    check(failsWith(run(ground, 1), JumpFail::LandingNoGround), "no ground under the landing");
    Faults mesh;
    mesh.OffMesh = true;
    check(failsWith(run(mesh, 1), JumpFail::LandingOffMesh), "a landing off the mesh");
    Faults path;
    path.NoPath = true;
    check(failsWith(run(path, 1), JumpFail::NoPath), "a landing without a path to the centre");
    Faults arcBlocked;
    arcBlocked.ArcBlocked = true;
    check(failsWith(run(arcBlocked, 1), JumpFail::ArcBlocked), "a blocked arc from the real start fails the jump");
    Faults oneWay;
    oneWay.OneWay = true;
    oneWay.BackBlocked = true;
    JumpTestMachine const drop = run(oneWay, 2);
    check(drop.Passed() && drop.Records()[0].BackMs == 0, "a one-way drop passes without the walk back");
    JumpTestMachine none_run;
    none_run.Start(testSpot, staging, 0, JumpTestLimits{}, 0);
    check(none_run.Done() && !none_run.Passed(), "no jumps: done, not passed");
}

#endif
