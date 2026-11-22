/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDTRAVELRULES_H
#define _PLAYERBOT_WORLDTRAVELRULES_H

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "StatusEndRules.h"
#include "WorldTravelTypes.h"

// The Player-free rules of honest world travel for persistent bots: the sight rule for teleports, the busy test, the
// corpse-run limit and the Stuck event's cause and position packing. Header-only so tests/world-travel-selftest.cpp
// checks the same code the server runs.
namespace WorldTravel
{
// The auto release and the dead engine switch each count one death (ReleaseSpiritAction, PlayerbotAI::UpdateAI)
constexpr uint32_t DEATH_COUNT_PER_DEATH = 2;
// A door or elevator this close to a stuck bot names the cause
constexpr float STUCK_DOOR_YARDS = 15.0f;
// Each repeat of a failed attempt doubles its expiry, at most this many times (FailedAttemptRules.h)
constexpr uint32_t STUCK_REPEAT_MAX = 3;
// A ghost waiting at its grave looks for a spirit healer at most this often
constexpr uint32_t GHOST_HEALER_RETRY_MS = 30 * 1000;
// A ghost that cannot walk to its grave is teleported there at most this often
constexpr uint32_t GHOST_TELEPORT_RETRY_MS = 5 * 60 * 1000;

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
    bool Honest = false;      // the rules apply: a persistent bot
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

// A ghost's way back on record (WU33a): one `death` row per change of step or reason, and a repeat row this often
constexpr uint32_t GHOST_ROW_REPEAT_MS = 10 * 60 * 1000;

enum class GhostWay : uint8_t
{
    None,        // no row written yet for this death
    CorpseWalk,  // walking to the corpse
    HealerWalk,  // walking to the grave's spirit healer
    HealerWait,  // at the grave, no healer found yet
    Teleport,    // teleported to the grave (GhostFallback, or the wait's bound away from the grave)
    Blocked,     // no move started and no teleport allowed
    Revive       // the wait's bound at the grave: revived for the spirit healer's price (WU34; the revive's row)
};

enum class GhostReason : uint8_t
{
    None,
    Moving,         // the corpse walk under way
    NoPath,         // the corpse walk found no path
    DeadTooLong,    // dead 10 min or more, or five deaths
    RunsExhausted,  // the corpse runs are used up
    Camped,         // an enemy player at the corpse (world PvP)
    NoHealer,       // at the grave without a healer in range
    Seen,           // a teleport would be seen
    RetryWait,      // the teleport interval has not passed
    CorpseAway,     // the corpse is on another map (an instance, or the game's repop moved the ghost)
    WaitEnded       // the wait reached GHOST_WAIT_BOUND_MS (WU34)
};

constexpr char const* GhostWayWord(GhostWay way)
{
    switch (way)
    {
        case GhostWay::CorpseWalk:
            return "corpse_walk";
        case GhostWay::HealerWalk:
            return "healer_walk";
        case GhostWay::HealerWait:
            return "healer_wait";
        case GhostWay::Teleport:
            return "teleport";
        case GhostWay::Blocked:
            return "blocked";
        case GhostWay::Revive:
            return "revive";
        default:
            return "none";
    }
}

constexpr char const* GhostReasonWord(GhostReason reason)
{
    switch (reason)
    {
        case GhostReason::Moving:
            return "moving";
        case GhostReason::NoPath:
            return "no_path";
        case GhostReason::DeadTooLong:
            return "dead_too_long";
        case GhostReason::RunsExhausted:
            return "runs_exhausted";
        case GhostReason::Camped:
            return "corpse_camped";
        case GhostReason::NoHealer:
            return "no_healer";
        case GhostReason::Seen:
            return "seen";
        case GhostReason::RetryWait:
            return "retry_wait";
        case GhostReason::CorpseAway:
            return "corpse_away";
        case GhostReason::WaitEnded:
            return "wait_ended";
        default:
            return "none";
    }
}

struct GhostWayInput
{
    bool CorpseBranch = false;  // asked from the corpse walk (FindCorpseAction); else from the spirit healer action
    bool Moving = false;        // a walk is under way or was just started
    bool AtGrave = false;       // on the grave's map and within sight of it
    bool RunsExhausted = false;
    bool DeadTooLong = false;              // dead 10 min or more, or five deaths
    bool Camped = false;                   // an enemy player stands at the corpse
    bool CorpseAway = false;               // the corpse is on another map
    bool Seen = false;                     // a real player would see the teleport (valid when Fallback is not Wait)
    GhostStep Fallback = GhostStep::Wait;  // GhostFallback's answer
    uint32_t WaitedMs = 0;                 // since the death's first wait step (GhostWaitStart); 0 before it
};

struct GhostWayResult
{
    GhostWay Way = GhostWay::None;
    GhostReason Reason = GhostReason::None;
};

// The step of a ghost's way back, named from plain inputs. The reason of a walk to the healer or a teleport is why it
// does not walk to its corpse. A corpse-branch call without a walk names no step (the hand-off to the healer action
// writes its own).
// Every ghost wait ends (WU34, the hub's rule): a persistent ghost whose first wait step of this death (healer_wait
// or blocked) began this long ago is revived at its grave for the spirit healer's price the next time it waits; away
// from the grave it is first teleported there. The clock runs on through later walks, so a ghost whose walk keeps
// failing between waits is bounded too.
constexpr uint32_t GHOST_WAIT_BOUND_MS = 10 * 60 * 1000;

inline bool IsGhostWait(GhostWay way) { return way == GhostWay::HealerWait || way == GhostWay::Blocked; }

// The death's wait clock: it starts (at nowMs, never 0) at the first wait step and is kept after it
inline uint32_t GhostWaitStart(uint32_t startMs, GhostWay way, uint32_t nowMs)
{
    if (startMs || !IsGhostWait(way))
        return startMs;

    return nowMs ? nowMs : 1;
}

inline GhostWayResult GhostStepOf(GhostWayInput const& in)
{
    GhostReason const cause = in.Camped          ? GhostReason::Camped
                              : in.RunsExhausted ? GhostReason::RunsExhausted
                              : in.CorpseAway    ? GhostReason::CorpseAway
                              : in.DeadTooLong   ? GhostReason::DeadTooLong
                                                 : GhostReason::NoPath;
    if (in.CorpseBranch)
        return in.Moving ? GhostWayResult{GhostWay::CorpseWalk, GhostReason::Moving} : GhostWayResult{};

    GhostWayResult step;
    if (in.AtGrave)
        step = {GhostWay::HealerWait, GhostReason::NoHealer};
    else if (in.Moving)
        step = {GhostWay::HealerWalk, cause};
    else if (in.Fallback == GhostStep::Teleport)
        step = {GhostWay::Teleport, cause};
    else
        step = {GhostWay::Blocked, in.Seen ? GhostReason::Seen : GhostReason::RetryWait};

    if (IsGhostWait(step.Way) && in.WaitedMs >= GHOST_WAIT_BOUND_MS)
        return {in.AtGrave ? GhostWay::Revive : GhostWay::Teleport, GhostReason::WaitEnded};

    return step;
}

// A row is due when the step or reason differs from the last row's, or the repeat time has passed since it
inline bool GhostRowDue(GhostWayResult const& last, uint32_t sinceMs, GhostWayResult const& now)
{
    return now.Way != GhostWay::None &&
           (now.Way != last.Way || now.Reason != last.Reason || sinceMs >= GHOST_ROW_REPEAT_MS);
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

// A recovery that left the bot where it was: its spot goes to the record of failed attempts (off_mesh)
constexpr bool OffMeshStays(OffMeshRecovery recovery)
{
    return recovery == OffMeshRecovery::Seen || recovery == OffMeshRecovery::NoPoly;
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

// A failed attempt's expiry after `repeats` repeats: doubled per repeat, capped
inline uint32_t StuckAvoidMs(uint32_t baseMs, uint32_t repeats)
{
    uint64_t const ms = static_cast<uint64_t>(baseMs) << std::min(repeats, STUCK_REPEAT_MAX);
    return static_cast<uint32_t>(std::min<uint64_t>(ms, UINT32_MAX));
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
// The inn's bind spot: this far in front of its innkeeper, this much above his spawn (the bind trip walks there)
constexpr float INN_FRONT_YARDS = 5.0f;
constexpr float INN_FRONT_RISE = 0.5f;

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

// The inn errand's last steps. The innkeeper must be within interaction distance (about 5 yd) to bind, so the bot walks
// until it stands this close to the spot in front of him; a bind that fails there is tried again, at most
// INN_BIND_TRIES times, then the errand ends. After a bind out of his sight (WT63e-b: at a few inns the spot lies
// behind a wall or the counter) the bot walks up to the innkeeper himself (`toKeeper`: the yards are his) and tries
// there
constexpr float INN_BIND_YARDS = 4.5f;
constexpr float INN_KEEPER_YARDS = 3.0f;
constexpr float INN_REACH_YARDS = 12.0f;  // a stuck bot this close still tries the bind
constexpr uint32_t INN_BIND_TRIES = 3;
enum class InnStep
{
    Walk,  // not close enough yet
    Bind,  // try the bind now (the caller counts the try)
    Fail   // the errand ends unbound
};
inline InnStep InnNext(float yards, bool stuck, uint32_t triesDone, bool toKeeper)
{
    if (triesDone >= INN_BIND_TRIES)
        return InnStep::Fail;
    if (yards <= (toKeeper ? INN_KEEPER_YARDS : INN_BIND_YARDS))
        return InnStep::Bind;
    if (!stuck)
        return InnStep::Walk;
    return yards <= INN_REACH_YARDS ? InnStep::Bind : InnStep::Fail;
}

// The innkeeper's bind spell sets the bind point where the bot stands; a cast that fails sets nothing (planning
// WT63d-b: lab14 bots logged a bind that never took and were sent to the same inn again). True when the bind point now
// lies in reach of the bot on its map
inline bool BindTook(bool sameMap, float yardsToBind) { return sameMap && yardsToBind <= INN_REACH_YARDS; }

// How a bind trip that reached its inn ends (WT63d-b): bound, it arrives; a bot that tried there and could not bind (no
// usable innkeeper, a bind that did not take) avoids the spot and gives the trip up; a bot stuck out of reach blames no
// spot and ends stuck. An unbound end stamps the planner's hearth retry (GoalApply::NoteInterludeEnd, 3.19 (6))
struct InnEnd
{
    StatusEnd::Outcome outcome;
    bool avoid;  // the spot is refused: the trip's not_bound end writes the record of failed attempts
};
// The inn avoid row's word for the bind's failure (BindAtInn's `why`)
inline char const* InnAvoidWord(std::string_view why)
{
    return why == "no innkeeper in reach"        ? "no_innkeeper"
           : why == "innkeeper not interactable" ? "not_interactable"
           : why == "innkeeper out of sight"     ? "not_in_sight"
                                                 : "not_bound";
}

inline InnEnd InnEndOf(bool bound, InnStep step)
{
    if (bound)
        return {StatusEnd::Outcome::Arrived, false};
    return step == InnStep::Bind ? InnEnd{StatusEnd::Outcome::Abandoned, true}
                                 : InnEnd{StatusEnd::Outcome::Stuck, false};
}
}  // namespace WorldTravel

#endif
