/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDTRAVELRULES_H
#define _PLAYERBOT_WORLDTRAVELRULES_H

#include <algorithm>
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
    OffMesh,  // the bot itself stands off the walkable mesh: no move can start from there
    Other
};

// The bot's own spot is on the mesh when a poly a bot may walk lies within this box around it (PathGenerator's first
// search); the check runs at the second tick without progress, long before the stuck give-up
constexpr float OFF_MESH_ACROSS_YARDS = 3.0f;
constexpr float OFF_MESH_VERTICAL_YARDS = 5.0f;
constexpr uint32_t OFF_MESH_CHECK_ATTEMPT = 2;
// An off-mesh bot that cannot hearth moves to the nearest walkable poly within this box, unseen (WT.23)
constexpr float OFF_MESH_SNAP_YARDS = 30.0f;
// After a recovery that found no way (seen, no_poly) the bot gives its goals up quietly at that spot for a while: no
// new Stuck event and no new recovery while it stands this close to it (OffMeshBackoff)
constexpr uint32_t OFF_MESH_BACKOFF_MS = 10 * 60 * 1000;
constexpr float OFF_MESH_BACKOFF_YARDS = 5.0f;

struct StuckInput
{
    bool OtherMap = false;            // the destination is on another map
    bool OnTransport = false;         // boat, zeppelin or elevator under the bot
    bool FallingOrSwimming = false;   // falling or in water
    bool NearDoorOrElevator = false;  // a door or elevator gameobject within STUCK_DOOR_YARDS
    bool PathComputed = false;        // a path from the bot to the destination was asked for
    bool NoPath = false;              // no path at all
    bool DestinationOffMesh = false;  // the destination is far from the walkable mesh
    bool StartOffMesh = false;        // the bot's own spot has no walkable poly near it
};

// Precedence: the bot's own situation first (map, transport, falling or water, off the mesh, doors), then the path
// result. Off the mesh comes before doors: a door does not explain a bot that cannot start any move
inline StuckCause ClassifyStuck(StuckInput const& in)
{
    if (in.OtherMap)
        return StuckCause::Other;
    if (in.OnTransport)
        return StuckCause::OnTransport;
    if (in.FallingOrSwimming)
        return StuckCause::FellOrSwimming;
    if (in.StartOffMesh)
        return StuckCause::OffMesh;
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

// The source word of the Stuck event (the world log's event row); string literals, so they outlive the call
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
        case StuckCause::OffMesh:
            return "off_mesh";
        default:
            return "other";
    }
}

// The recovery of a bot off the mesh (WT.23): the hearthstone when it is ready (a player's own action), else a short
// move to the nearest walkable poly when no real player sees it (a last-resort teleport, counted as off_mesh_snap),
// else nothing (the bot gives its goal up and tries again at its next stuck check)
enum class OffMeshRecovery : uint8_t
{
    Hearth,
    Snap,
    Seen,   // a poly is near, but a real player would see the move
    NoPoly  // no walkable poly within OFF_MESH_SNAP_YARDS
};

inline OffMeshRecovery OffMeshNext(bool hearthCast, bool polyFound, bool seen)
{
    if (hearthCast)
        return OffMeshRecovery::Hearth;
    if (!polyFound)
        return OffMeshRecovery::NoPoly;
    return seen ? OffMeshRecovery::Seen : OffMeshRecovery::Snap;
}

// The reason word of the off-mesh decision row
constexpr char const* OffMeshRecoveryWord(OffMeshRecovery recovery)
{
    switch (recovery)
    {
        case OffMeshRecovery::Hearth:
            return "hearth";
        case OffMeshRecovery::Snap:
            return "snap";
        case OffMeshRecovery::Seen:
            return "seen";
        default:
            return "no_poly";
    }
}

// A recovery that left the bot where it was (it backs off there: OffMeshBackoff)
constexpr bool OffMeshStays(OffMeshRecovery recovery)
{
    return recovery == OffMeshRecovery::Seen || recovery == OffMeshRecovery::NoPoly;
}

// A give-up while backing off: the memo of a recovery that left the bot in place is set, younger than
// OFF_MESH_BACKOFF_MS (sinceMs: the wrap-safe difference to now), and the bot stands within OFF_MESH_BACKOFF_YARDS of
// it. Then the give-up writes no Stuck event and tries no recovery: one Stuck off_mesh per spot and 10 minutes, so two
// in a row at one spot mean that a recovery did not take
inline bool OffMeshBackoff(bool memoSet, uint32_t sinceMs, SightPoint const& at, SightPoint const& here)
{
    return memoSet && sinceMs < OFF_MESH_BACKOFF_MS && WithinSight(at, here, OFF_MESH_BACKOFF_YARDS);
}

// What the bot looked like when the hearthstone action refused (the reason word of the hearth_refused decision row).
// A mounted bot is the common case: the core refuses the cast (SPELL_FAILED_NOT_MOUNTED) where a player's client
// dismounts first, so the follower dismounts a bot on the ground before the cast and this word names what is left
struct HearthState
{
    bool alive = true;
    bool stone = true;  // the hearthstone is in the bags
    bool inCombat = false;
    bool battleground = false;
    bool flying = false;  // in the air: no dismount there
    bool mounted = false;
    bool flightForm = false;
    bool moving = false;
    bool cooldown = false;
};

constexpr char const* HearthRefusalWord(HearthState const& state)
{
    if (!state.alive)
        return "dead";
    if (!state.stone)
        return "no_stone";
    if (state.inCombat)
        return "combat";
    if (state.battleground)
        return "battleground";
    if (state.flying)
        return "flying";
    if (state.mounted)
        return "mounted";
    if (state.flightForm)
        return "flight_form";
    if (state.moving)
        return "moving";
    if (state.cooldown)
        return "cooldown";
    return "cast_failed";  // the core's cast check refused (the row's value is its SpellCastResult)
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

// The inn spots of the startup cache (TravelMgr::PrepareDestinationCache) hold only innkeepers a bot can use: one seen
// in the normal phase, friendly to players, and not phased out by its area. An area phase (a spell_area row with an
// automatic phase aura) hides a spawn when it applies to a bot without quest progress (no start quest, no required
// aura) and its phase mask leaves the spawn's phase out: The Shadow Vault's Phase Shift I (30181, phase 2, until quest
// 12896/12897) hides Initiate Brenners (phase 1)
constexpr uint32_t NORMAL_PHASE_MASK = 1;
inline bool PhaseRowHidesSpawn(bool autocast, uint32_t questStart, int32_t auraSpell, uint32_t auraPhaseMask,
                               uint32_t spawnPhaseMask)
{
    return autocast && questStart == 0 && auraSpell <= 0 && (auraPhaseMask & spawnPhaseMask) == 0;
}

// hostileToPlayers: the faction template's enemy mask holds every player (Lodge-Matron Embla's 2109: friendly only
// under a quest aura)
inline bool InnSpawnUsable(uint32_t spawnPhaseMask, bool hostileToPlayers, bool phasedOut)
{
    return (spawnPhaseMask & NORMAL_PHASE_MASK) != 0 && !hostileToPlayers && !phasedOut;
}

// A spot where the bind found no usable innkeeper is avoided by that bot for a while: its next inn errand picks
// another inn. A few spots per bot; the oldest gives way
constexpr uint32_t INN_AVOID_MS = 60 * 60 * 1000;
constexpr float INN_AVOID_YARDS = 15.0f;
constexpr size_t INN_AVOID_SPOTS = 4;
struct AvoidedSpot
{
    SightPoint at;
    uint32_t sinceMs = 0;  // getMSTime() when the bind failed there
};

inline bool InnAvoided(std::vector<AvoidedSpot> const& spots, uint32_t nowMs, SightPoint const& spot)
{
    for (AvoidedSpot const& avoided : spots)
        if (nowMs - avoided.sinceMs < INN_AVOID_MS && WithinSight(avoided.at, spot, INN_AVOID_YARDS))
            return true;

    return false;
}

inline void AvoidInn(std::vector<AvoidedSpot>& spots, uint32_t nowMs, SightPoint const& spot)
{
    for (AvoidedSpot& avoided : spots)
        if (nowMs - avoided.sinceMs >= INN_AVOID_MS || WithinSight(avoided.at, spot, INN_AVOID_YARDS))
        {
            avoided = {spot, nowMs};
            return;
        }

    if (spots.size() < INN_AVOID_SPOTS)
    {
        spots.push_back({spot, nowMs});
        return;
    }

    auto const oldest = std::max_element(spots.begin(), spots.end(), [nowMs](AvoidedSpot const& a, AvoidedSpot const& b)
                                         { return nowMs - a.sinceMs < nowMs - b.sinceMs; });
    *oldest = {spot, nowMs};
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
