/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MESHMOVERULES_H
#define PLAYERBOTS_MESHMOVERULES_H

// The moves the navmesh did not carry (WT71, WT.51, DP.17), engine-free. Every move a bot orders is classified on its
// path's answer (the PathType bits and the path's last step): carried by the mesh, or not (no path, a player's "no
// polygon" line, a path cut to a straight line, a shortcut, an end far from the mesh, the engine's hop). The call site
// passes its kind; nothing is inferred. A bot counts per kind the moves it ordered and the ones not carried, and the
// travel maintenance (BotTravel::PreGate) flushes them once per interval as one "MeshMiss" event row per kind with a
// miss: source = the kind word, id = moves not carried, amount = moves ordered, the place = where the last one was
// ordered. Counting is not repairing: a move accepted before is accepted now.
// Already counted elsewhere, so not counted again here: the follower's hop at a walk link's end (walk_link rows' hops;
// at its call sites the helper is told the hop is counted), a trip's off-mesh give-up (Decision trip off_mesh) and
// the off-mesh snap (HonestyTeleport off_mesh_snap).
// tests/mesh-move-selftest.cpp drives it with made-up answers.

#include <cstddef>
#include <cstdint>

#include "TravelWalkRules.h"

namespace MeshMove
{
constexpr std::uint32_t PATH_SHORTCUT = 0x02;
constexpr std::uint32_t PATH_FARFROMPOLY_START = 0x40;
constexpr std::uint32_t MISS_INTERVAL_MS = 5 * 60 * 1000;  // the flush interval (WT.40: a plain value, revisable)
constexpr char const* EVENT_TYPE = "MeshMiss";

enum class Class : std::uint8_t
{
    Carried,       // NORMAL or INCOMPLETE, no hop
    NoPath,        // no path (or no answer)
    NotUsingPath,  // an end has no polygon: the engine's line
    Short,         // longer than the point limit: replaced by a straight line
    Shortcut,      // a straight line through obstacles
    FarFromPoly,   // an end more than 7 yd from a polygon
    Hop            // a carried path ending in a straight step longer than HOP_STEP_YARDS
};

enum class Kind : std::uint8_t
{
    TravelWalk,
    Boarding,  // boarding and leaving a transport
    Deck,      // a walk on a vessel's deck, on the vessel's own mesh (WT70c)
    Follow,    // follow and formation
    Combat,
    Other,
    Count
};
constexpr std::size_t KINDS = static_cast<std::size_t>(Kind::Count);

inline char const* KindWord(Kind kind)
{
    switch (kind)
    {
        case Kind::TravelWalk:
            return "travel_walk";
        case Kind::Boarding:
            return "boarding";
        case Kind::Deck:
            return "deck";
        case Kind::Follow:
            return "follow";
        case Kind::Combat:
            return "combat";
        case Kind::Other:
        case Kind::Count:
            break;
    }
    return "other";
}

// The class of one path answer
inline Class Classify(std::uint32_t type, float lastStep)
{
    using namespace TravelWalk;
    if (type & PATH_NOPATH)
        return Class::NoPath;
    if (type & PATH_NOT_USING_PATH)
        return Class::NotUsingPath;
    if (type & PATH_SHORT)
        return Class::Short;
    if (type & PATH_SHORTCUT)
        return Class::Shortcut;
    if (type & (PATH_FARFROMPOLY_END | PATH_FARFROMPOLY_START))
        return Class::FarFromPoly;
    if (!(type & (PATH_NORMAL | PATH_INCOMPLETE)))
        return Class::NoPath;
    // The hop is the engine copying an unreached target in as the path's last point, which it does for a partial
    // polygon path only (INCOMPLETE; the core's PathGenerator::BuildPointPath). A NORMAL path's last step over the
    // limit is a smooth 4 yd step measured on steep or rough ground (WT64g-2: 23 refused orders with last steps of
    // 5.0 to 6.0 yd and none longer, against a median of 78 yd for the INCOMPLETE hops)
    return (type & PATH_INCOMPLETE) && lastStep > HOP_STEP_YARDS ? Class::Hop : Class::Carried;
}

// The path's last segment (a shortcut's whole line); 0 for fewer than two points
template <class Points>
float LastStep(Points const& points)
{
    return points.size() >= 2 ? TravelWalk::Distance(points[points.size() - 1], points[points.size() - 2]) : 0.0f;
}

struct Spot
{
    std::uint32_t map{0};
    std::uint32_t zone{0};
    std::uint32_t area{0};
    float x{0.0f};
    float y{0.0f};
};

struct Counters
{
    std::uint32_t ordered[KINDS]{};
    std::uint32_t offmesh[KINDS]{};
    Spot last[KINDS];            // where the last move not carried was ordered
    std::uint32_t flushedMs{0};  // the last flush's time; 0 = none yet
};

// One ordered move. counted: the class is already counted elsewhere (a walk link's hop), so only the move is. True
// when a move not carried was counted (the caller then sets counters.last[kind])
inline bool Note(Counters& counters, Kind kind, Class cls, bool counted)
{
    std::size_t const k = static_cast<std::size_t>(kind);
    if (counters.ordered[k] < UINT32_MAX)
        ++counters.ordered[k];
    if (cls == Class::Carried || counted)
        return false;
    if (counters.offmesh[k] < UINT32_MAX)
        ++counters.offmesh[k];
    return true;
}

// The flush is due once per interval; the first call only starts the interval
inline bool FlushDue(Counters& counters, std::uint32_t nowMs)
{
    if (!counters.flushedMs)
    {
        counters.flushedMs = nowMs ? nowMs : 1;
        return false;
    }
    return nowMs - counters.flushedMs >= MISS_INTERVAL_MS;
}

// The flush: emit(kind, offmesh, ordered, spot) for every kind with a miss, then every counter starts again; the
// rows written
template <class Emit>
std::size_t Flush(Counters& counters, std::uint32_t nowMs, Emit const& emit)
{
    std::size_t rows = 0;
    for (std::size_t k = 0; k < KINDS; ++k)
    {
        if (counters.offmesh[k])
        {
            emit(static_cast<Kind>(k), counters.offmesh[k], counters.ordered[k], counters.last[k]);
            ++rows;
        }
        counters.ordered[k] = 0;
        counters.offmesh[k] = 0;
    }
    counters.flushedMs = nowMs ? nowMs : 1;
    return rows;
}
}  // namespace MeshMove

#endif
