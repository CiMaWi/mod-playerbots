/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDTRAVELRULES_H
#define _PLAYERBOT_WORLDTRAVELRULES_H

#include <algorithm>
#include <cmath>
#include <cstdint>

// The Player-free rules of honest world travel for persistent bots: the sight rule for teleports, the busy test, the
// corpse-run limit and the Stuck event's cause and position packing. Header-only so tests/world-travel-selftest.cpp
// checks the same code the server runs.
namespace WorldTravel
{
// The auto release and the dead engine switch each count one death (ReleaseSpiritAction, PlayerbotAI::UpdateAI)
constexpr uint32_t DEATH_COUNT_PER_DEATH = 2;
// A door or elevator this close to a stuck bot names the cause
constexpr float STUCK_DOOR_YARDS = 15.0f;
// A new destination this close to the one given up is avoided too
constexpr float STUCK_AVOID_YARDS = 20.0f;
constexpr int32_t PACK_COORD_MIN = -32768;
constexpr int32_t PACK_COORD_MAX = 32767;
constexpr uint32_t PACK_COORD_BITS = 16;
constexpr uint64_t PACK_COORD_MASK = 0xFFFF;

// The rules apply to persistent bots; arena instances (ArenaOnly) keep their own cycle
inline bool AppliesTo(bool persistent, bool arenaOnly) { return persistent && !arenaOnly; }

struct SightPoint
{
    uint32_t MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

// A real player counts as a viewer unless it is an invisible game master
inline bool IsViewer(bool gameMaster, bool gmVisible) { return !gameMaster || gmVisible; }

// Strictly inside the radius, on the same map; radius 0 sees nothing
inline bool WithinSight(SightPoint const& viewer, SightPoint const& spot, float radius)
{
    if (radius <= 0.0f || viewer.MapId != spot.MapId)
        return false;

    float const dx = viewer.X - spot.X;
    float const dy = viewer.Y - spot.Y;
    float const dz = viewer.Z - spot.Z;
    return dx * dx + dy * dy + dz * dz < radius * radius;
}

// A teleport is seen when a viewer is within the radius of where the bot stands or where it lands
inline bool SeesTeleport(SightPoint const& viewer, SightPoint const& from, SightPoint const& to, float radius)
{
    return WithinSight(viewer, from, radius) || WithinSight(viewer, to, radius);
}

struct BusyInput
{
    bool Honest = false;      // the rules apply (AppliesTo)
    bool RealMaster = false;  // a real player's party bot, left to the master
    bool HasGoal = false;     // the bot pursues a goal (WT16)
    bool InCombat = false;
    bool Dead = false;
    bool Grouped = false;      // in a group
    bool InInstance = false;   // in a dungeon, raid, battleground or arena
    bool MidActivity = false;  // an RPG status that carries an activity in progress (flight, route, errands)
    bool LegacyIdle = true;    // other setups: the travel target is idle
};

// A bot the rules apply to is busy (no randomize, refresh or teleport) only while it is genuinely mid-activity. The
// short-lived wander, grind, camp, quest, rest, gather and outdoor PvP statuses do not count: the RPG leaves IDLE at
// the next status update, so testing for IDLE alone would all but stop the level and periodic teleports kept until
// WT20 (WT.7). Other bots keep the travel-target test.
inline bool IsBusy(BusyInput const& in)
{
    if (!in.Honest)
        return !in.LegacyIdle;

    return in.RealMaster || in.HasGoal || in.InCombat || in.Dead || in.Grouped || in.InInstance || in.MidActivity;
}

// The corpse runs are used up when more than `failures` runs ended in another death (deathCount counts each death
// DEATH_COUNT_PER_DEATH times and resets on XP gain or repair)
inline bool CorpseRunsExhausted(uint32_t deathCount, uint32_t failures)
{
    return deathCount / DEATH_COUNT_PER_DEATH > failures;
}

enum class StuckCause : uint8_t
{
    OnTransport,
    FellOrSwimming,
    DoorOrElevator,
    UnreachablePoi,
    NoPath,
    NoProgress,
    Other
};

struct StuckInput
{
    bool OtherMap = false;            // the destination is on another map
    bool OnTransport = false;         // boat, zeppelin or elevator under the bot
    bool FallingOrSwimming = false;   // falling or in water
    bool NearDoorOrElevator = false;  // a door or elevator gameobject within STUCK_DOOR_YARDS
    bool PathComputed = false;        // a path from the bot to the destination was asked for
    bool NoPath = false;              // no path at all
    bool DestinationOffMesh = false;  // the destination is far from the walkable mesh
};

// Precedence: the bot's own situation first (map, transport, falling or water, doors), then the path result
inline StuckCause ClassifyStuck(StuckInput const& in)
{
    if (in.OtherMap)
        return StuckCause::Other;
    if (in.OnTransport)
        return StuckCause::OnTransport;
    if (in.FallingOrSwimming)
        return StuckCause::FellOrSwimming;
    if (in.NearDoorOrElevator)
        return StuckCause::DoorOrElevator;
    if (!in.PathComputed)
        return StuckCause::Other;
    if (in.DestinationOffMesh)
        return StuckCause::UnreachablePoi;
    if (in.NoPath)
        return StuckCause::NoPath;
    return StuckCause::NoProgress;
}

// The source word of the Stuck event (bot_events.csv); string literals, so they outlive the call
constexpr char const* StuckCauseWord(StuckCause cause)
{
    switch (cause)
    {
        case StuckCause::OnTransport:
            return "on_transport";
        case StuckCause::FellOrSwimming:
            return "fell_or_swimming";
        case StuckCause::DoorOrElevator:
            return "door_or_elevator";
        case StuckCause::UnreachablePoi:
            return "unreachable_poi";
        case StuckCause::NoPath:
            return "no_path";
        case StuckCause::NoProgress:
            return "no_progress";
        default:
            return "other";
    }
}

inline uint64_t PackCoord(float value)
{
    long const rounded =
        std::lround(std::clamp(value, static_cast<float>(PACK_COORD_MIN), static_cast<float>(PACK_COORD_MAX)));
    return static_cast<uint64_t>(static_cast<uint16_t>(static_cast<int16_t>(rounded)));
}

// The Stuck event's amount: bot x, bot y, destination x, destination y as signed 16-bit yards, high to low
inline int64_t PackStuckPositions(float botX, float botY, float destX, float destY)
{
    uint64_t const packed = PackCoord(botX) << (3 * PACK_COORD_BITS) | PackCoord(botY) << (2 * PACK_COORD_BITS) |
                            PackCoord(destX) << PACK_COORD_BITS | PackCoord(destY);
    return static_cast<int64_t>(packed);
}

// index 0 = bot x, 1 = bot y, 2 = destination x, 3 = destination y
inline int32_t UnpackStuckCoord(int64_t amount, uint32_t index)
{
    uint32_t const shift = (3 - index) * PACK_COORD_BITS;
    return static_cast<int16_t>(static_cast<uint16_t>((static_cast<uint64_t>(amount) >> shift) & PACK_COORD_MASK));
}

// A destination given up after a Stuck event stays avoided for avoidMs (sinceMs since the event; memoSet false = none)
inline bool IsAvoidedSpot(bool memoSet, uint32_t sinceMs, uint32_t avoidMs, SightPoint const& given,
                          SightPoint const& dest)
{
    return memoSet && sinceMs < avoidMs && WithinSight(given, dest, STUCK_AVOID_YARDS);
}
}  // namespace WorldTravel

#endif
