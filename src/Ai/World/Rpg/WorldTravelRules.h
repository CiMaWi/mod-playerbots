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
#include <vector>

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
// A give-up this close to where the bot gave up last time is the same obstacle again
constexpr float STUCK_REPEAT_YARDS = 100.0f;
// Each repeat at the same obstacle doubles the avoid time and radius, at most this many times
constexpr uint32_t STUCK_REPEAT_MAX = 3;
// A ghost waiting at its grave looks for a spirit healer at most this often
constexpr uint32_t GHOST_HEALER_RETRY_MS = 30 * 1000;
// A ghost that cannot walk to its grave is teleported there at most this often
constexpr uint32_t GHOST_TELEPORT_RETRY_MS = 5 * 60 * 1000;
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

// A timed retry is due when it never ran (tsSet false) or retryMs have passed since
inline bool RetryDue(bool tsSet, uint32_t sinceMs, uint32_t retryMs) { return !tsSet || sinceMs >= retryMs; }

enum class GhostStep : uint8_t
{
    Wait,
    Teleport
};

struct GhostInput
{
    bool AtGrave = false;           // on the grave's map and within sight of it
    bool Seen = false;              // a real player would see the teleport (the sight rule)
    bool TeleportedBefore = false;  // SinceTeleportMs is valid
    uint32_t SinceTeleportMs = 0;
};

// A ghost that does not walk toward its grave. At the grave it waits for a spirit healer and never teleports (it
// used to teleport to the grave it stood at, again and again); a far one is teleported unseen, at most once per
// GHOST_TELEPORT_RETRY_MS.
inline GhostStep GhostFallback(GhostInput const& in)
{
    if (in.AtGrave || in.Seen || !RetryDue(in.TeleportedBefore, in.SinceTeleportMs, GHOST_TELEPORT_RETRY_MS))
        return GhostStep::Wait;

    return GhostStep::Teleport;
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

// The avoid time after `repeats` give-ups in a row at the same obstacle: doubled per repeat, capped
inline uint32_t StuckAvoidMs(uint32_t baseMs, uint32_t repeats)
{
    uint64_t const ms = static_cast<uint64_t>(baseMs) << std::min(repeats, STUCK_REPEAT_MAX);
    return static_cast<uint32_t>(std::min<uint64_t>(ms, UINT32_MAX));
}

inline float StuckAvoidYards(uint32_t repeats)
{
    return STUCK_AVOID_YARDS * static_cast<float>(1u << std::min(repeats, STUCK_REPEAT_MAX));
}

// A destination given up after a Stuck event stays avoided for the (escalated) avoid time within the (escalated)
// radius (sinceMs since the event; memoSet false = none)
inline bool IsAvoidedSpot(bool memoSet, uint32_t sinceMs, uint32_t avoidMs, SightPoint const& given,
                          SightPoint const& dest, uint32_t repeats = 0)
{
    return memoSet && sinceMs < StuckAvoidMs(avoidMs, repeats) && WithinSight(given, dest, StuckAvoidYards(repeats));
}

// The repeat count of a new give-up: one more than the last while that is still avoided and the bot gives up within
// STUCK_REPEAT_YARDS of where it gave up then (the same obstacle), else 0
inline uint32_t NextStuckRepeats(bool memoSet, uint32_t sinceMs, uint32_t avoidMs, uint32_t repeats,
                                 SightPoint const& lastBot, SightPoint const& bot)
{
    if (!memoSet || sinceMs >= StuckAvoidMs(avoidMs, repeats) || !WithinSight(lastBot, bot, STUCK_REPEAT_YARDS))
        return 0;

    return std::min(repeats + 1, STUCK_REPEAT_MAX);
}

// Teleports and the hearthstone (WT9). A manager teleport asked to "hearth" used to rebind the bot at the destination
// (a city or an rpg spot); a persistent bot (honest travel) keeps its bind: it moves only at an inn
inline bool TeleportRebinds(bool honest, bool hearth) { return hearth && !honest; }

// Inn binding as an errand. The bind is "far" past ShouldHomeBindValue's 1000 yd, or on another map than the bot's
constexpr float INN_BIND_FAR_YARDS = 1000.0f;
// A persistent bot checks at most this often (ms), the first check is due at once
constexpr uint32_t INN_RETRY_MS = 30 * 60 * 1000;
// The inn must lie within this much walking at run speed
constexpr float INN_MAX_WALK_SECONDS = 600.0f;

inline bool BindIsFar(bool sameMap, float yardsToBind) { return !sameMap || yardsToBind > INN_BIND_FAR_YARDS; }

// lastCheckMs 0 = never checked (the stamp is kept non-zero)
inline bool InnCheckDue(uint32_t lastCheckMs, uint32_t nowMs)
{
    return RetryDue(lastCheckMs != 0, nowMs - lastCheckMs, INN_RETRY_MS);
}

// One inn of the bot's team on the bot's map: the straight yards from the bot, and whether its zone fits the level
struct InnCandidate
{
    float yards{0.0f};
    bool fits{false};
};

// The nearest level-fitting inn within maxYards; -1 when none (no errand). A bind that already sits at a fitting inn
// near the bot never gets here: it is not far
inline int PickInn(std::vector<InnCandidate> const& inns, float maxYards)
{
    int pick = -1;
    for (size_t i = 0; i < inns.size(); ++i)
        if (inns[i].fits && inns[i].yards <= maxYards &&
            (pick < 0 || inns[i].yards < inns[static_cast<size_t>(pick)].yards))
            pick = static_cast<int>(i);

    return pick;
}

// The inn errand's last steps. The innkeeper must be within interaction distance (about 5 yd) to bind, so the bot walks
// until it stands this close; a bind that fails there is tried again, at most INN_BIND_TRIES times, then the errand
// ends
constexpr float INN_BIND_YARDS = 4.5f;
constexpr float INN_REACH_YARDS = 12.0f;  // a stuck bot this close still tries the bind
constexpr uint32_t INN_BIND_TRIES = 3;
enum class InnStep
{
    Walk,  // not close enough yet
    Bind,  // try the bind now (the caller counts the try)
    Fail   // the errand ends unbound
};
inline InnStep InnNext(float yards, bool stuck, uint32_t triesDone)
{
    if (triesDone >= INN_BIND_TRIES)
        return InnStep::Fail;
    if (yards <= INN_BIND_YARDS)
        return InnStep::Bind;
    if (!stuck)
        return InnStep::Walk;
    return yards <= INN_REACH_YARDS ? InnStep::Bind : InnStep::Fail;
}
}  // namespace WorldTravel

#endif
