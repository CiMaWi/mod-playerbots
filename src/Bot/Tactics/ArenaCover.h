/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENACOVER_H
#define _PLAYERBOT_ARENACOVER_H

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "Common.h"

class Battleground;
class ChatHandler;
class Map;
class Player;

// Arena cover (G2d), the pure parts. Every arena has a table of pillars (discs on the floor: x, y, z, r). Ring of
// Valor's four come from the core (game objects that rise and sink in two sets: a pillar blocks only while its object
// is up, so every pillar carries a gate bit and the caller passes the gates that are up now); Nagrand, Blade's Edge,
// Lordaeron and Dalaran have pillars of map geometry, found by `tactics cover scan` (CoverScan below: rays over a
// grid, blocked pairs clustered into discs, every disc measured and confirmed by live rays) and pasted in by G2d-b.
// FindCoverPoint picks a point on a ring round a pillar that hides the bot from an enemy and/or sees a unit, ranked
// by the table's geometry and confirmed by rays (at most MaxRays, usually one or two); NextCirclePoint is the next step
// round a pillar away from a chaser; CoverIsStillValid re-tests a held point. Rays are an injected callable (the map's
// line of sight in game, the table's geometry in the selftest), between points Eye yards above the floor, as the
// core's spell check does (WorldObject::IsWithinLOSInMap adds the collision height). The game side is ArenaCover.cpp
// (the scan command, the gates of Ring of Valor, the map's rays); the Los and Pillar tactics (G2e, G2f) use it with
// LosConfig's and PillarConfig's numbers (ArenaCoverConfig.h).
//
// Why breaking LOS works (Spell.cpp, read for G2d): a spell with a cast time checks LOS when it starts (prepare ->
// CheckCast) and again when the bar completes (update: PREPARING, m_timer 0 -> cast(!m_casttime) -> _cast(false) ->
// CheckCast(false) -> IsWithinLOSInMap -> SPELL_FAILED_LINE_OF_SIGHT, SendInterrupted), not during the bar; so a target
// that steps behind a pillar at any time before the bar completes breaks the cast. Instants are checked once. A channel
// is not re-checked: in CASTING, UpdateChanneledTargetList tests range only, so a channel keeps ticking behind a
// pillar.

constexpr float COVER_PI = 3.14159265358979323846f;
constexpr float COVER_TWO_PI = 2.0f * COVER_PI;

constexpr uint32 COVER_MAP_NAGRAND = 559;
constexpr uint32 COVER_MAP_BLADES_EDGE = 562;
constexpr uint32 COVER_MAP_LORDAERON = 572;
constexpr uint32 COVER_MAP_DALARAN = 617;
constexpr uint32 COVER_MAP_RING_OF_VALOR = 618;

constexpr float COVER_EYE_YARDS = 2.0f;  // rays run this high above the floor (a player's collision height)

constexpr uint32 COVER_GATE_ALWAYS = 1;          // bit 0: map geometry, always solid
constexpr uint32 COVER_ALL_GATES = 0xFFFFFFFFu;  // every pillar solid (the selftest; a caller without gate data)

struct CoverVec
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

// A pillar: a disc on the floor (Z the floor players stand on next to it, R its radius). Gate 0: map geometry; gate
// k > 0: solid while the arena's game object k is up (bit k of the gates, ArenaCoverActiveGates)
struct CoverPillar
{
    float X;
    float Y;
    float Z;
    float R;
    uint8 Gate;
};

// An arena's table; the box (centre, half size, floor height) is where the scan looks
struct ArenaCoverTable
{
    uint32 MapId;
    char const* Name;
    float CentreX;
    float CentreY;
    float CentreZ;
    float HalfSize;
    CoverPillar const* Pillars;
    uint32 Count;
};

// Ring of Valor (core BattlegroundRV.cpp:239-243 positions, GetPillarIdForPos radii 1.75 and 2 x 1.75); gate k is
// BG_RV_OBJECT_PILAR_k, solid while GO_STATE_READY (GameObject::SetGoState); Z is the arena floor, not the object's
// origin. UpdatePillars swaps sets 1+3 and 2+4 every 25 s from about 45.5 s into the match.
inline constexpr std::array<CoverPillar, 4> ARENA_COVER_RV_PILLARS = {{
    {763.632385f, -306.162384f, 28.28f, 1.75f, 1},
    {723.644287f, -284.493256f, 28.28f, 3.5f, 2},
    {763.611145f, -261.856750f, 28.28f, 1.75f, 3},
    {802.211609f, -284.493256f, 28.28f, 3.5f, 4},
}};

// The five arenas. Nagrand, Blade's Edge, Lordaeron and Dalaran get their rows from G2d-b's scan (their pillars are
// map geometry, not game objects); the boxes centre on ArenaTactics::moveToCenter's points.
inline constexpr std::array<ArenaCoverTable, 5> ARENA_COVER_TABLES = {{
    {COVER_MAP_NAGRAND, "Nagrand Arena", 4056.0f, 2919.0f, 13.3f, 44.0f, nullptr, 0},
    {COVER_MAP_BLADES_EDGE, "Blade's Edge Arena", 6242.0f, 263.0f, 1.5f, 48.0f, nullptr, 0},
    {COVER_MAP_LORDAERON, "Ruins of Lordaeron", 1280.0f, 1666.0f, 34.0f, 46.0f, nullptr, 0},
    {COVER_MAP_DALARAN, "Dalaran Sewers", 1291.6f, 790.9f, 7.8f, 44.0f, nullptr, 0},
    {COVER_MAP_RING_OF_VALOR, "Ring of Valor", 763.5f, -284.0f, 28.28f, 44.0f, ARENA_COVER_RV_PILLARS.data(),
     static_cast<uint32>(ARENA_COVER_RV_PILLARS.size())},
}};

inline ArenaCoverTable const* FindArenaCoverTable(uint32 mapId)
{
    for (ArenaCoverTable const& table : ARENA_COVER_TABLES)
        if (table.MapId == mapId)
            return &table;

    return nullptr;
}

inline bool CoverPillarActive(CoverPillar const& pillar, uint32 activeGates)
{
    return pillar.Gate < 32 && ((activeGates >> pillar.Gate) & 1u) != 0;
}

inline CoverVec CoverEye(CoverVec const& point, float eye) { return {point.X, point.Y, point.Z + eye}; }

inline float CoverDist2d(CoverVec const& a, CoverVec const& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

// The angle in (-pi, pi]
inline float CoverWrapAngle(float angle)
{
    while (angle > COVER_PI)
        angle -= COVER_TWO_PI;

    while (angle <= -COVER_PI)
        angle += COVER_TWO_PI;

    return angle;
}

// Does the floor segment a-b pass through the disc (cx, cy, r)? (2-D: the segment's closest point to the centre)
inline bool CoverSegmentHitsDisc(CoverVec const& a, CoverVec const& b, float cx, float cy, float r)
{
    if (r <= 0.0f)
        return false;

    float const dx = b.X - a.X;
    float const dy = b.Y - a.Y;
    float const length2 = dx * dx + dy * dy;
    float const t = length2 > 0.0f ? std::clamp(((cx - a.X) * dx + (cy - a.Y) * dy) / length2, 0.0f, 1.0f) : 0.0f;
    float const px = a.X + t * dx - cx;
    float const py = a.Y + t * dy - cy;
    return px * px + py * py < r * r;
}

// Does a-b cross an active pillar of the table (radii shrunk by margin)? The table's own line of sight.
inline bool CoverSegmentBlocked(ArenaCoverTable const& table, uint32 activeGates, CoverVec const& a, CoverVec const& b,
                                float margin)
{
    for (uint32 index = 0; index < table.Count; ++index)
    {
        CoverPillar const& pillar = table.Pillars[index];
        if (CoverPillarActive(pillar, activeGates) && CoverSegmentHitsDisc(a, b, pillar.X, pillar.Y, pillar.R - margin))
            return true;
    }

    return false;
}

inline bool CoverInsidePillar(ArenaCoverTable const& table, uint32 activeGates, CoverVec const& point)
{
    for (uint32 index = 0; index < table.Count; ++index)
    {
        CoverPillar const& pillar = table.Pillars[index];
        if (CoverPillarActive(pillar, activeGates) && std::hypot(point.X - pillar.X, point.Y - pillar.Y) < pillar.R)
            return true;
    }

    return false;
}

struct CoverSearchParams
{
    float RingGap = 1.5f;        // candidates stand this far outside a pillar's radius
    uint32 AnglesPerPillar = 8;  // evenly round each pillar, from the side away from the enemy ...
    float FineStepDeg = 22.5f;   // ... plus one either side of that side, this far off
    bool RequireHidden = true;   // the point must hide the bot from the enemy (break LOS)
    bool PreferHidden = false;   // otherwise a hidden point wins unless a visible one is HiddenBonusYards nearer
    float HiddenBonusYards = 6.0f;
    float DetourFactor = 1.3f;  // a straight line through a pillar is this much longer on foot
    float Margin = 0.25f;       // the geometric ranking shrinks the radii by this (the rays decide)
    float Eye = COVER_EYE_YARDS;
    uint32 MaxRays = 40;
};

struct CoverSearchResult
{
    bool Found = false;
    CoverVec Point;
    int32 Pillar = -1;      // index into the table
    bool Hidden = false;    // the ray from the enemy is blocked (tested when RequireHidden or PreferHidden)
    bool Sees = false;      // the ray to mustSee is clear (tested when it is given)
    float Moved = 0.0f;     // the estimated walk (straight line, DetourFactor through a pillar)
    uint32 Candidates = 0;  // after the geometric filter
    uint32 Rays = 0;
};

// The cover point nearest to self (by the estimated walk, at most maxMove) on a ring round an active pillar that hides
// self from enemy (RequireHidden; PreferHidden ranks hidden points first) and sees mustSee (when given). Candidates
// come from the table's geometry, then rays confirm them in rank order; the search stops at MaxRays. The caller still
// checks the path to the point (pillars outside the table, walls).
template <typename Los>
CoverSearchResult FindCoverPoint(ArenaCoverTable const& map, uint32 activeGates, CoverVec const& self,
                                 CoverVec const& enemy, CoverVec const* mustSee, float maxMove, Los&& los,
                                 CoverSearchParams const& params = CoverSearchParams())
{
    struct Candidate
    {
        CoverVec Point;
        int32 Pillar;
        float Moved;
        float Score;
    };

    CoverSearchResult result;
    std::vector<Candidate> candidates;
    uint32 const evenCount = std::max<uint32>(params.AnglesPerPillar, 1);
    float const fine = params.FineStepDeg * COVER_PI / 180.0f;
    for (uint32 index = 0; index < map.Count; ++index)
    {
        CoverPillar const& pillar = map.Pillars[index];
        if (!CoverPillarActive(pillar, activeGates))
            continue;

        float const ring = pillar.R + params.RingGap;
        if (std::hypot(self.X - pillar.X, self.Y - pillar.Y) - ring > maxMove)
            continue;

        float const away = std::atan2(pillar.Y - enemy.Y, pillar.X - enemy.X);
        for (uint32 k = 0; k < evenCount + 2; ++k)
        {
            float const angle = k < evenCount ? away + COVER_TWO_PI * static_cast<float>(k) / evenCount
                                              : away + (k == evenCount ? fine : -fine);
            CoverVec const point{pillar.X + ring * std::cos(angle), pillar.Y + ring * std::sin(angle), pillar.Z};
            if (std::fabs(point.X - map.CentreX) > map.HalfSize || std::fabs(point.Y - map.CentreY) > map.HalfSize ||
                CoverInsidePillar(map, activeGates, point))
                continue;

            float moved = CoverDist2d(self, point);
            if (CoverSegmentBlocked(map, activeGates, self, point, params.Margin))
                moved *= params.DetourFactor;

            if (moved > maxMove)
                continue;

            bool const hidden = CoverSegmentBlocked(map, activeGates, enemy, point, params.Margin);
            if (params.RequireHidden && !hidden)
                continue;

            if (mustSee && CoverSegmentBlocked(map, activeGates, point, *mustSee, params.Margin))
                continue;

            float const score = moved + (params.PreferHidden && !hidden ? params.HiddenBonusYards : 0.0f);
            candidates.push_back({point, static_cast<int32>(index), moved, score});
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](Candidate const& a, Candidate const& b) { return a.Score < b.Score; });
    result.Candidates = static_cast<uint32>(candidates.size());
    bool const testHidden = params.RequireHidden || params.PreferHidden;
    uint32 const raysEach = (testHidden ? 1 : 0) + (mustSee ? 1 : 0);
    for (Candidate const& candidate : candidates)
    {
        if (result.Rays + raysEach > params.MaxRays)
            break;

        bool hidden = false;
        if (testHidden)
        {
            ++result.Rays;
            hidden = !los(CoverEye(enemy, params.Eye), CoverEye(candidate.Point, params.Eye));
            if (params.RequireHidden && !hidden)
                continue;
        }

        if (mustSee)
        {
            ++result.Rays;
            if (!los(CoverEye(candidate.Point, params.Eye), CoverEye(*mustSee, params.Eye)))
                continue;
        }

        result.Found = true;
        result.Point = candidate.Point;
        result.Pillar = candidate.Pillar;
        result.Hidden = hidden;
        result.Sees = mustSee != nullptr;
        result.Moved = candidate.Moved;
        break;
    }

    return result;
}

// The next point round the pillar (ring R + ringGap), an arc of at most stepYards away from the chaser, never past
// the point opposite it (where the pillar is between them); self off the ring is first put on it (G2f, a melee pulled
// round a pillar). Geometry only: the caller checks the path.
inline CoverVec NextCirclePoint(CoverPillar const& pillar, CoverVec const& self, CoverVec const& chaser,
                                float stepYards, float ringGap)
{
    float const ring = pillar.R + ringGap;
    float const selfAngle = std::atan2(self.Y - pillar.Y, self.X - pillar.X);
    float const chaserAngle = std::atan2(chaser.Y - pillar.Y, chaser.X - pillar.X);
    float const apart = CoverWrapAngle(selfAngle - chaserAngle);
    float const direction = apart >= 0.0f ? 1.0f : -1.0f;
    float const step = std::min(std::max(stepYards, 0.0f) / ring, COVER_PI - std::fabs(apart));
    float const angle = selfAngle + direction * step;
    return {pillar.X + ring * std::cos(angle), pillar.Y + ring * std::sin(angle), pillar.Z};
}

// A bot's cover plan (the value "cover plan"): the point it holds and why; GUIDs (raw) and numbers only
struct CoverPlan
{
    uint32 UpdatedMs = 0;
    bool Valid = false;
    uint32 MapId = 0;
    int32 Pillar = -1;  // index into the map's table; -1: a point of no pillar
    CoverVec Point;
    bool WantHidden = false;  // the point must keep the bot hidden from the enemy
    bool WantSee = false;     // the point must keep the unit of SeeGuid in sight
    uint64 EnemyGuid = 0;
    uint64 SeeGuid = 0;
};

// The per-bot state (PlayerbotAI::GetCoverState; the bot's own update, tactics show on the world thread): G2e and
// G2f add the partner-LOS-lost start and the ring of the last relocations
struct CoverBotState
{
    uint32 LastSearchMs = 0;
};

// Does the held point still do its job? Its pillar is still up (Ring of Valor's sets did not flip) and, by at most two
// rays, it still hides from the enemy and sees the unit it must see (see null when that unit is gone: invalid).
template <typename Los>
bool CoverIsStillValid(CoverPlan const& plan, ArenaCoverTable const& map, uint32 activeGates, CoverVec const& enemy,
                       CoverVec const* see, Los&& los, float eye = COVER_EYE_YARDS, uint32* rays = nullptr)
{
    if (!plan.Valid || plan.MapId != map.MapId)
        return false;

    if (plan.Pillar >= 0 &&
        (static_cast<uint32>(plan.Pillar) >= map.Count || !CoverPillarActive(map.Pillars[plan.Pillar], activeGates)))
        return false;

    if (plan.WantHidden)
    {
        if (rays)
            ++*rays;

        if (los(CoverEye(enemy, eye), CoverEye(plan.Point, eye)))
            return false;
    }

    if (plan.WantSee)
    {
        if (!see)
            return false;

        if (rays)
            ++*rays;

        if (!los(CoverEye(plan.Point, eye), CoverEye(*see, eye)))
            return false;
    }

    return true;
}

// How many of `diameters` rays through the pillar (from R + gap on one side to R + gap on the other, Eye above its
// floor) are blocked: the live check of a table row or a scanned disc
template <typename Los>
uint32 CoverCheckPillar(CoverPillar const& pillar, Los&& los, float gap, float eye, uint32 diameters)
{
    uint32 blocked = 0;
    float const reach = pillar.R + gap;
    for (uint32 k = 0; k < diameters; ++k)
    {
        float const angle = COVER_PI * static_cast<float>(k) / diameters;
        float const dx = reach * std::cos(angle);
        float const dy = reach * std::sin(angle);
        if (!los(CoverVec{pillar.X + dx, pillar.Y + dy, pillar.Z + eye},
                 CoverVec{pillar.X - dx, pillar.Y - dy, pillar.Z + eye}))
            ++blocked;
    }

    return blocked;
}

struct CoverScanParams
{
    float CentreX = 0.0f;
    float CentreY = 0.0f;
    float RefZ = 0.0f;
    float HalfSize = 44.0f;
    float Step = 2.0f;       // the grid step (yards)
    float ZWindow = 8.0f;    // a floor point lies within this of RefZ
    float MaxEdgeDz = 1.5f;  // pairs further apart in height are steps, not views
    float Eye = COVER_EYE_YARDS;
    uint32 MinBlocked = 3;      // a cluster of fewer blocked pairs is noise
    float MaxDiameter = 26.0f;  // a larger cluster is a wall or a building
    uint32 Directions = 16;     // radius probes per disc ...
    uint32 SearchSteps = 7;     // ... each a bisection of this many rays
    uint32 MinValidDirections = 12;
    float MinCoreRadius = 0.3f;  // a disc whose centre is not solid in every direction is no pillar
    float ConfirmGap = 2.0f;     // the confirming diameters reach this far past the radius ...
    uint32 ConfirmRays = 8;      // ... this many of them, at least ConfirmMinBlocked blocked
    uint32 ConfirmMinBlocked = 7;
    float RingGap = 1.5f;  // the standing ring checked for floor
};

struct CoverScanDisc
{
    CoverPillar Pillar{};
    float RMin = 0.0f;   // the smallest probed radius (RMin well below R: an elongated block)
    uint32 Blocked = 0;  // blocked pairs in its cluster
    uint32 ValidDirections = 0;
    uint32 DiametersBlocked = 0;
    uint32 RingStandable = 0;  // of 8 ring points at R + RingGap with floor
    bool Pass = false;
};

struct CoverScanRejected
{
    float X;
    float Y;
    float Extent;
    uint32 Blocked;
    char const* Reason;
};

struct CoverScanResult
{
    std::vector<CoverScanDisc> Discs;
    std::vector<CoverScanRejected> Rejected;
    uint32 GridPoints = 0;
    uint32 FloorPoints = 0;
    uint32 PairRays = 0;
    uint32 BlockedPairs = 0;
    uint32 RefineRays = 0;
    uint32 ConfirmRays = 0;
};

// The pillar scan: floor heights on a grid over the box (ground(x, y, z&) -> bool), rays between nearby floor points
// at eye height (pairs 4 to 8 yd apart, whose midpoints fall on grid points), blocked pairs counted at their
// midpoints and clustered (8-neighbours); a cluster at the box border, larger than MaxDiameter or of fewer than
// MinBlocked pairs is dropped. A kept cluster's centre (the weighted midpoint) is probed in Directions directions: a
// ray from far outside to the point t yards out is clear exactly when t is past the obstacle's surface, so a
// bisection finds the radius in that direction; the centre moves to the probes' best circle, R is the largest probed
// radius, and ConfirmRays diameters (CoverCheckPillar) confirm the disc.
template <typename Ground, typename Los>
CoverScanResult CoverScan(CoverScanParams const& params, Ground&& ground, Los&& los)
{
    CoverScanResult result;
    float const step = std::max(params.Step, 0.5f);
    int32 const n = static_cast<int32>(std::floor(2.0f * params.HalfSize / step)) + 1;
    float const x0 = params.CentreX - params.HalfSize;
    float const y0 = params.CentreY - params.HalfSize;
    size_t const cells = static_cast<size_t>(n) * static_cast<size_t>(n);
    auto const cellOf = [n](int32 i, int32 j) { return static_cast<size_t>(j) * static_cast<size_t>(n) + i; };
    std::vector<float> floorZ(cells, 0.0f);
    std::vector<uint8> isFloor(cells, 0);
    for (int32 j = 0; j < n; ++j)
    {
        for (int32 i = 0; i < n; ++i)
        {
            ++result.GridPoints;
            float z = 0.0f;
            if (ground(x0 + i * step, y0 + j * step, z) && std::fabs(z - params.RefZ) <= params.ZWindow)
            {
                isFloor[cellOf(i, j)] = 1;
                floorZ[cellOf(i, j)] = z;
                ++result.FloorPoints;
            }
        }
    }

    static constexpr int32 PAIRS[6][2] = {{2, 0}, {0, 2}, {2, 2}, {2, -2}, {4, 0}, {0, 4}};
    std::vector<uint32> blockedAt(cells, 0);
    std::vector<float> blockedZ(cells, 0.0f);
    for (int32 j = 0; j < n; ++j)
    {
        for (int32 i = 0; i < n; ++i)
        {
            size_t const a = cellOf(i, j);
            if (!isFloor[a])
                continue;

            for (auto const& pair : PAIRS)
            {
                int32 const bi = i + pair[0];
                int32 const bj = j + pair[1];
                if (bi < 0 || bi >= n || bj < 0 || bj >= n)
                    continue;

                size_t const b = cellOf(bi, bj);
                if (!isFloor[b] || std::fabs(floorZ[a] - floorZ[b]) > params.MaxEdgeDz)
                    continue;

                ++result.PairRays;
                if (los(CoverVec{x0 + i * step, y0 + j * step, floorZ[a] + params.Eye},
                        CoverVec{x0 + bi * step, y0 + bj * step, floorZ[b] + params.Eye}))
                    continue;

                ++result.BlockedPairs;
                size_t const mid = cellOf(i + pair[0] / 2, j + pair[1] / 2);
                ++blockedAt[mid];
                blockedZ[mid] += 0.5f * (floorZ[a] + floorZ[b]);
            }
        }
    }

    std::vector<uint8> seen(cells, 0);
    std::vector<size_t> stack;
    std::vector<size_t> members;
    for (size_t start = 0; start < cells; ++start)
    {
        if (!blockedAt[start] || seen[start])
            continue;

        members.clear();
        stack.assign(1, start);
        seen[start] = 1;
        while (!stack.empty())
        {
            size_t const cell = stack.back();
            stack.pop_back();
            members.push_back(cell);
            int32 const ci = static_cast<int32>(cell % static_cast<size_t>(n));
            int32 const cj = static_cast<int32>(cell / static_cast<size_t>(n));
            for (int32 dj = -1; dj <= 1; ++dj)
            {
                for (int32 di = -1; di <= 1; ++di)
                {
                    int32 const ni = ci + di;
                    int32 const nj = cj + dj;
                    if (ni < 0 || ni >= n || nj < 0 || nj >= n)
                        continue;

                    size_t const next = cellOf(ni, nj);
                    if (blockedAt[next] && !seen[next])
                    {
                        seen[next] = 1;
                        stack.push_back(next);
                    }
                }
            }
        }

        double weight = 0.0;
        double sumX = 0.0;
        double sumY = 0.0;
        double sumZ = 0.0;
        int32 minI = n;
        int32 maxI = -1;
        int32 minJ = n;
        int32 maxJ = -1;
        for (size_t const cell : members)
        {
            int32 const ci = static_cast<int32>(cell % static_cast<size_t>(n));
            int32 const cj = static_cast<int32>(cell / static_cast<size_t>(n));
            double const w = blockedAt[cell];
            weight += w;
            sumX += w * (x0 + ci * step);
            sumY += w * (y0 + cj * step);
            sumZ += blockedZ[cell];
            minI = std::min(minI, ci);
            maxI = std::max(maxI, ci);
            minJ = std::min(minJ, cj);
            maxJ = std::max(maxJ, cj);
        }

        uint32 const blocked = static_cast<uint32>(weight);
        float const extent = static_cast<float>(std::max(maxI - minI, maxJ - minJ) + 1) * step;
        CoverVec const centre{static_cast<float>(sumX / weight), static_cast<float>(sumY / weight),
                              static_cast<float>(sumZ / weight)};
        char const* reason = nullptr;
        if (minI <= 1 || minJ <= 1 || maxI >= n - 2 || maxJ >= n - 2)
            reason = "border";
        else if (extent > params.MaxDiameter)
            reason = "too large";
        else if (blocked < params.MinBlocked)
            reason = "noise";

        if (reason)
        {
            result.Rejected.push_back({centre.X, centre.Y, extent, blocked, reason});
            continue;
        }

        // the radius by direction: bisection between the centre (blocked from outside) and reach (clear)
        float const reach = 0.5f * extent + 1.0f;
        float const farDist = reach + 6.0f;
        float const eyeZ = centre.Z + params.Eye;
        uint32 const directions = std::max<uint32>(params.Directions, 4);
        std::vector<float> radii(directions, -1.0f);
        uint32 valid = 0;
        float minRadius = farDist;
        for (uint32 d = 0; d < directions; ++d)
        {
            float const angle = COVER_TWO_PI * static_cast<float>(d) / directions;
            float const dx = std::cos(angle);
            float const dy = std::sin(angle);
            CoverVec const outside{centre.X + farDist * dx, centre.Y + farDist * dy, eyeZ};
            auto const at = [&](float t) { return CoverVec{centre.X + t * dx, centre.Y + t * dy, eyeZ}; };
            ++result.RefineRays;
            if (!los(outside, at(reach)))
                continue;  // something else is in the way

            float lo = 0.0f;
            float hi = reach;
            ++result.RefineRays;
            if (los(outside, at(0.0f)))
                hi = 0.0f;  // the centre is open from here
            else
            {
                for (uint32 s = 0; s < params.SearchSteps; ++s)
                {
                    float const mid = 0.5f * (lo + hi);
                    ++result.RefineRays;
                    if (los(outside, at(mid)))
                        hi = mid;
                    else
                        lo = mid;
                }
            }

            radii[d] = hi;
            minRadius = std::min(minRadius, hi);
            ++valid;
        }

        if (valid < params.MinValidDirections)
        {
            result.Rejected.push_back({centre.X, centre.Y, extent, blocked, "rays disagree"});
            continue;
        }

        if (minRadius < params.MinCoreRadius)
        {
            result.Rejected.push_back({centre.X, centre.Y, extent, blocked, "hollow"});
            continue;
        }

        // the probes' best circle: for a circle off by delta, radius(d) ~ R + delta . d, so delta = 2 / D sum r(d) d
        CoverVec fitted = centre;
        if (valid == directions)
        {
            float shiftX = 0.0f;
            float shiftY = 0.0f;
            for (uint32 d = 0; d < directions; ++d)
            {
                float const angle = COVER_TWO_PI * static_cast<float>(d) / directions;
                shiftX += radii[d] * std::cos(angle);
                shiftY += radii[d] * std::sin(angle);
            }

            fitted.X += 2.0f * shiftX / directions;
            fitted.Y += 2.0f * shiftY / directions;
        }

        float radius = 0.0f;
        float radiusMin = farDist;
        for (uint32 d = 0; d < directions; ++d)
        {
            if (radii[d] < 0.0f)
                continue;

            float const angle = COVER_TWO_PI * static_cast<float>(d) / directions;
            float const surface = std::hypot(centre.X + radii[d] * std::cos(angle) - fitted.X,
                                             centre.Y + radii[d] * std::sin(angle) - fitted.Y);
            radius = std::max(radius, surface);
            radiusMin = std::min(radiusMin, surface);
        }

        CoverScanDisc disc;
        disc.Pillar = {fitted.X, fitted.Y, centre.Z, radius, 0};
        disc.RMin = radiusMin;
        disc.Blocked = blocked;
        disc.ValidDirections = valid;
        disc.DiametersBlocked = CoverCheckPillar(disc.Pillar, los, params.ConfirmGap, params.Eye, params.ConfirmRays);
        result.ConfirmRays += params.ConfirmRays;
        for (uint32 k = 0; k < 8; ++k)
        {
            float const angle = COVER_TWO_PI * static_cast<float>(k) / 8.0f;
            float const ring = radius + params.RingGap;
            float z = 0.0f;
            if (ground(fitted.X + ring * std::cos(angle), fitted.Y + ring * std::sin(angle), z) &&
                std::fabs(z - centre.Z) <= params.MaxEdgeDz)
                ++disc.RingStandable;
        }

        disc.Pass = disc.DiametersBlocked >= params.ConfirmMinBlocked;
        result.Discs.push_back(disc);
    }

    return result;
}

// Game side (ArenaCover.cpp): the gates up now (bit 0 always; Ring of Valor: bit k while BG_RV_OBJECT_PILAR_k is
// GO_STATE_READY), the map's ray between two eye points (the spell check's LOS), and the scan command's body
uint32 ArenaCoverActiveGates(Battleground* bg);
bool ArenaCoverLos(Map* map, uint32 phaseMask, CoverVec const& a, CoverVec const& b);
bool RunArenaCoverScan(ChatHandler* handler, Player* player);

// The selftest (tactics selftest cover; the offline harness runs the same checks): the tables, FindCoverPoint
// (a hide case, a sweep against a brute-force ring search, see-and-hide, unreachable, gates, the ray budget),
// CoverIsStillValid, NextCirclePoint, and the scan on a synthetic arena (four discs found, walls dropped)
template <typename Check>
void RunCoverChecks(Check&& check)
{
    check(ARENA_COVER_TABLES.size() == 5, "five arena tables");
    for (ArenaCoverTable const& table : ARENA_COVER_TABLES)
    {
        check(FindArenaCoverTable(table.MapId) == &table, std::string("lookup of ") + table.Name);
        bool inside = true;
        for (uint32 index = 0; index < table.Count; ++index)
        {
            CoverPillar const& pillar = table.Pillars[index];
            inside = inside && std::fabs(pillar.X - table.CentreX) <= table.HalfSize &&
                     std::fabs(pillar.Y - table.CentreY) <= table.HalfSize && pillar.R > 0.5f && pillar.R < 13.0f;
        }

        check(inside, std::string("pillars inside the box with a sane radius: ") + table.Name);
    }

    check(!FindArenaCoverTable(0) && !FindArenaCoverTable(30), "no table outside the arenas");
    ArenaCoverTable const* rv = FindArenaCoverTable(COVER_MAP_RING_OF_VALOR);
    check(rv && rv->Count == 4, "Ring of Valor has the core's four pillars");
    if (!rv || rv->Count != 4)
        return;

    check(rv->Pillars[0].R == 1.75f && rv->Pillars[1].R == 3.5f && rv->Pillars[2].R == 1.75f &&
              rv->Pillars[3].R == 3.5f && rv->Pillars[0].Gate == 1 && rv->Pillars[1].Gate == 2 &&
              rv->Pillars[2].Gate == 3 && rv->Pillars[3].Gate == 4,
          "Ring of Valor radii and gates of the core");

    uint32 rays = 0;
    uint32 gates = COVER_ALL_GATES;
    auto const los = [&](CoverVec const& a, CoverVec const& b)
    {
        ++rays;
        return !CoverSegmentBlocked(*rv, gates, a, b, 0.0f);
    };
    auto const truth = [&](CoverVec const& a, CoverVec const& b)
    { return !CoverSegmentBlocked(*rv, gates, a, b, 0.0f); };
    CoverSearchParams const params;
    float const floorZ = 28.28f;

    // hide: the bot by pillar 1, the enemy north-east of it
    CoverVec const self{758.0f, -300.0f, floorZ};
    CoverVec const enemy{775.0f, -275.0f, floorZ};
    CoverSearchResult const hide = FindCoverPoint(*rv, gates, self, enemy, nullptr, 12.0f, los, params);
    check(hide.Found && hide.Hidden && hide.Pillar == 0 && !truth(enemy, hide.Point) &&
              CoverDist2d(self, hide.Point) <= 12.0f && hide.Rays >= 1 && hide.Rays <= 6,
          "hide: a point behind pillar 1, within reach, by at most 6 rays");

    // the sweep: random bots and enemies, every point found hidden and in reach, the rays few, and nearly every
    // case a fine ring search (every 5 degrees) solves solved too
    uint64 seed = 0x2545F4914F6CDD1DULL;
    auto const next = [&seed]()
    {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>((seed >> 33) % 1000000) / 1000000.0f;
    };
    uint32 searches = 0;
    uint32 found = 0;
    uint32 bruteFound = 0;
    uint32 wrong = 0;
    uint32 maxRays = 0;
    uint32 totalRays = 0;
    float const reachYards = 15.0f;
    for (uint32 run = 0; run < 400; ++run)
    {
        CoverVec const s{rv->CentreX + (2.0f * next() - 1.0f) * 30.0f, rv->CentreY + (2.0f * next() - 1.0f) * 22.0f,
                         floorZ};
        CoverVec const e{rv->CentreX + (2.0f * next() - 1.0f) * 30.0f, rv->CentreY + (2.0f * next() - 1.0f) * 22.0f,
                         floorZ};
        if (CoverInsidePillar(*rv, gates, s) || CoverInsidePillar(*rv, gates, e))
            continue;

        ++searches;
        CoverSearchResult const r = FindCoverPoint(*rv, gates, s, e, nullptr, reachYards, los, params);
        totalRays += r.Rays;
        maxRays = std::max(maxRays, r.Rays);
        if (r.Found)
        {
            ++found;
            if (truth(e, r.Point) || CoverDist2d(s, r.Point) > reachYards + 0.01f)
                ++wrong;
        }

        bool any = false;
        for (uint32 index = 0; index < rv->Count && !any; ++index)
        {
            CoverPillar const& pillar = rv->Pillars[index];
            for (uint32 k = 0; k < 72 && !any; ++k)
            {
                float const angle = COVER_TWO_PI * static_cast<float>(k) / 72.0f;
                float const ring = pillar.R + params.RingGap;
                CoverVec const p{pillar.X + ring * std::cos(angle), pillar.Y + ring * std::sin(angle), floorZ};
                float moved = CoverDist2d(s, p);
                if (CoverSegmentBlocked(*rv, gates, s, p, params.Margin))
                    moved *= params.DetourFactor;

                any = moved <= reachYards && !CoverInsidePillar(*rv, gates, p) && !truth(e, p);
            }
        }

        if (any)
            ++bruteFound;
    }

    check(searches >= 300, "sweep: enough searches");
    check(wrong == 0, "sweep: every point found is hidden and within reach");
    check(maxRays <= params.MaxRays && totalRays <= 6 * searches, "sweep: at most 40 rays a search, 6 on average");
    check(found * 4 >= bruteFound * 3, "sweep: solves at least 3 of 4 cases the fine ring search solves (found " +
                                           std::to_string(found) + ", ring " + std::to_string(bruteFound) + ")");

    // see and hide: a healer hides from the caster behind pillar 2 and still sees its partner
    CoverVec const healer{735.0f, -290.0f, floorZ};
    CoverVec const partner{730.0f, -300.0f, floorZ};
    CoverVec const caster{760.0f, -284.0f, floorZ};
    CoverSearchResult const heal = FindCoverPoint(*rv, gates, healer, caster, &partner, 25.0f, los, params);
    check(heal.Found && heal.Hidden && heal.Sees && !truth(caster, heal.Point) && truth(heal.Point, partner),
          "see and hide: hidden from the caster, the partner in sight");
    CoverSearchResult const impossible = FindCoverPoint(*rv, gates, healer, caster, &caster, 25.0f, los, params);
    check(!impossible.Found, "see and hide: no point hides from and sees the same unit");

    // regain LOS without hiding (G2e low skill): the nearest point that sees the target
    CoverSearchParams seeOnly;
    seeOnly.RequireHidden = false;
    CoverVec const target{723.6f, -276.0f, floorZ};
    CoverVec const blind{723.6f, -293.5f, floorZ};
    CoverSearchResult const regain = FindCoverPoint(*rv, gates, blind, caster, &target, 14.0f, los, seeOnly);
    check(!truth(blind, target) && regain.Found && regain.Sees && truth(regain.Point, target) && regain.Rays <= 6,
          "regain: a point that sees the target behind pillar 2");

    CoverSearchResult const stuck = FindCoverPoint(*rv, gates, self, enemy, nullptr, 0.5f, los, params);
    check(!stuck.Found && stuck.Rays == 0 && stuck.Candidates == 0, "unreachable: no point, no ray");

    // gates: only pillars 2 and 4 up; the point uses one of them and is invalid once the sets flip
    uint32 const setB = COVER_GATE_ALWAYS | (1u << 2) | (1u << 4);
    uint32 const setA = COVER_GATE_ALWAYS | (1u << 1) | (1u << 3);
    gates = setB;
    CoverVec const middle{763.5f, -284.0f, floorZ};
    CoverSearchResult const gated = FindCoverPoint(*rv, gates, middle, enemy, nullptr, 45.0f, los, params);
    check(gated.Found && (gated.Pillar == 1 || gated.Pillar == 3), "gates: only an up pillar gives cover");
    CoverPlan plan;
    plan.Valid = gated.Found;
    plan.MapId = COVER_MAP_RING_OF_VALOR;
    plan.Pillar = gated.Pillar;
    plan.Point = gated.Point;
    plan.WantHidden = true;
    check(CoverIsStillValid(plan, *rv, setB, enemy, nullptr, truth), "valid while the set is up");
    check(!CoverIsStillValid(plan, *rv, setA, enemy, nullptr, truth), "invalid once the sets flip");

    // still valid: the hide plan holds while the enemy stays, and fails once the enemy walks round
    gates = COVER_ALL_GATES;
    plan.Valid = hide.Found;
    plan.Pillar = hide.Pillar;
    plan.Point = hide.Point;
    uint32 validRays = 0;
    check(CoverIsStillValid(plan, *rv, gates, enemy, nullptr, truth, COVER_EYE_YARDS, &validRays) && validRays == 1,
          "valid: the enemy still cannot see the point (one ray)");
    CoverPillar const& one = rv->Pillars[0];
    CoverVec const flank{hide.Point.X + 2.0f * (hide.Point.X - one.X), hide.Point.Y + 2.0f * (hide.Point.Y - one.Y),
                         floorZ};
    check(!CoverIsStillValid(plan, *rv, gates, flank, nullptr, truth), "invalid: the enemy walked round");
    plan.WantSee = true;
    check(!CoverIsStillValid(plan, *rv, gates, enemy, nullptr, truth), "invalid: the unit to see is gone");

    // the ray budget: nothing ever hides (all rays clear), so every candidate costs a ray up to the cap
    auto const open = [&](CoverVec const&, CoverVec const&)
    {
        ++rays;
        return true;
    };
    CoverSearchResult const all = FindCoverPoint(*rv, gates, middle, enemy, nullptr, 60.0f, open, params);
    check(!all.Found && all.Rays == std::min(all.Candidates, params.MaxRays), "budget: one ray per candidate");
    CoverSearchParams tight = params;
    tight.MaxRays = 5;
    CoverSearchResult const capped = FindCoverPoint(*rv, gates, middle, enemy, nullptr, 60.0f, open, tight);
    check(!capped.Found && capped.Rays == std::min<uint32>(capped.Candidates, 5) && capped.Candidates > 5,
          "budget: the search stops at MaxRays");
    CoverSearchParams both = params;
    both.MaxRays = 41;
    CoverSearchResult const pairs = FindCoverPoint(*rv, gates, middle, enemy, &partner, 60.0f, open, both);
    check(pairs.Rays <= 41 && pairs.Rays % 2 == 0, "budget: see-and-hide never starts a pair it cannot finish");

    // NextCirclePoint: on the ring, away from the chaser, at most the step, never past the opposite point
    CoverPillar const disc{0.0f, 0.0f, 0.0f, 3.0f, 0};
    CoverVec const onRing{4.5f, 0.0f, 0.0f};
    CoverVec const chaser{10.0f, 2.0f, 0.0f};
    CoverVec const step = NextCirclePoint(disc, onRing, chaser, 3.0f, 1.5f);
    float const before = std::fabs(CoverWrapAngle(std::atan2(onRing.Y, onRing.X) - std::atan2(chaser.Y, chaser.X)));
    float const after = std::fabs(CoverWrapAngle(std::atan2(step.Y, step.X) - std::atan2(chaser.Y, chaser.X)));
    check(std::fabs(std::hypot(step.X, step.Y) - 4.5f) < 0.01f && after > before &&
              std::fabs(CoverWrapAngle(std::atan2(step.Y, step.X)) + 3.0f / 4.5f) < 0.01f,
          "circle: one arc of 3 yd on the ring, away from the chaser");
    CoverVec const opposite{-4.5f, 0.0f, 0.0f};
    CoverVec const stay = NextCirclePoint(disc, opposite, CoverVec{9.0f, 0.0f, 0.0f}, 3.0f, 1.5f);
    check(CoverDist2d(stay, opposite) < 0.01f, "circle: opposite the chaser it stays");
    CoverVec const nearOpposite =
        NextCirclePoint(disc, CoverVec{-4.4f, 0.4f, 0.0f}, CoverVec{9.0f, 0.0f, 0.0f}, 6.0f, 1.5f);
    check(std::fabs(std::atan2(nearOpposite.Y, nearOpposite.X)) > COVER_PI - 0.01f,
          "circle: a long step stops at the opposite point");
    CoverVec const offRing = NextCirclePoint(disc, CoverVec{8.0f, 0.0f, 0.0f}, chaser, 0.0f, 1.5f);
    check(std::fabs(std::hypot(offRing.X, offRing.Y) - 4.5f) < 0.01f, "circle: a bot off the ring is put on it");

    // the scan on a synthetic arena: four discs, an outer wall (outside it there is floor too) and an inner wall
    struct Wall
    {
        float X1;
        float Y1;
        float X2;
        float Y2;
    };
    static constexpr std::array<CoverPillar, 4> SCAN_DISCS = {{
        {10.0f, 5.0f, 10.0f, 2.0f, 0},
        {-12.0f, -8.0f, 10.0f, 3.5f, 0},
        {0.0f, -20.0f, 10.0f, 1.75f, 0},
        {18.0f, -15.0f, 10.0f, 5.0f, 0},
    }};
    static constexpr std::array<Wall, 5> SCAN_WALLS = {{
        {-36.0f, -36.0f, 36.0f, -36.0f},
        {36.0f, -36.0f, 36.0f, 36.0f},
        {36.0f, 36.0f, -36.0f, 36.0f},
        {-36.0f, 36.0f, -36.0f, -36.0f},
        {-20.0f, 15.0f, -5.0f, 15.0f},
    }};
    ArenaCoverTable const synthetic{
        0, "synthetic", 0.0f, 0.0f, 10.0f, 40.0f, SCAN_DISCS.data(), static_cast<uint32>(SCAN_DISCS.size())};
    auto const cross = [](float ax, float ay, float bx, float by, float cx, float cy)
    { return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax); };
    uint32 scanRays = 0;
    auto const scanLos = [&](CoverVec const& a, CoverVec const& b)
    {
        ++scanRays;
        if (CoverSegmentBlocked(synthetic, COVER_ALL_GATES, a, b, 0.0f))
            return false;

        for (Wall const& wall : SCAN_WALLS)
        {
            float const d1 = cross(wall.X1, wall.Y1, wall.X2, wall.Y2, a.X, a.Y);
            float const d2 = cross(wall.X1, wall.Y1, wall.X2, wall.Y2, b.X, b.Y);
            float const d3 = cross(a.X, a.Y, b.X, b.Y, wall.X1, wall.Y1);
            float const d4 = cross(a.X, a.Y, b.X, b.Y, wall.X2, wall.Y2);
            if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
                ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f)))
                return false;
        }

        return true;
    };
    auto const flat = [](float, float, float& z)
    {
        z = 10.0f;
        return true;
    };
    CoverScanParams scanParams;
    scanParams.CentreX = 0.0f;
    scanParams.CentreY = 0.0f;
    scanParams.RefZ = 10.0f;
    scanParams.HalfSize = 40.0f;
    CoverScanResult const scan = CoverScan(scanParams, flat, scanLos);
    uint32 matched = 0;
    for (CoverPillar const& truthDisc : SCAN_DISCS)
    {
        for (CoverScanDisc const& found2 : scan.Discs)
        {
            if (std::hypot(found2.Pillar.X - truthDisc.X, found2.Pillar.Y - truthDisc.Y) < 0.5f &&
                found2.Pillar.R >= truthDisc.R - 0.05f && found2.Pillar.R <= truthDisc.R + 0.5f && found2.Pass &&
                found2.DiametersBlocked == scanParams.ConfirmRays && found2.RingStandable == 8)
            {
                ++matched;
                break;
            }
        }
    }

    bool innerWallDropped = true;
    for (CoverScanDisc const& found2 : scan.Discs)
        if (found2.Pillar.Y > 12.0f && found2.Pillar.Y < 18.0f && found2.Pillar.X > -22.0f && found2.Pillar.X < -3.0f)
            innerWallDropped = false;

    uint32 walls = 0;
    for (CoverScanRejected const& rejected : scan.Rejected)
        if (rejected.Extent > scanParams.MaxDiameter)
            ++walls;

    check(matched == SCAN_DISCS.size() && scan.Discs.size() == SCAN_DISCS.size(),
          "scan: the four discs found (centre within 0.5 yd, radius within 0.5 yd above), confirmed by 8 rays; " +
              std::to_string(scan.Discs.size()) + " discs");
    check(innerWallDropped, "scan: a thin inner wall is no pillar");
    check(walls >= 1, "scan: the outer wall is dropped (too large)");
    check(scan.FloorPoints == scan.GridPoints && scan.PairRays > 0 &&
              scanRays == scan.PairRays + scan.RefineRays + scan.ConfirmRays,
          "scan: every ray counted (" + std::to_string(scanRays) + " rays)");
    (void)rays;
}

#endif
