/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GROUNDRULES_H
#define PLAYERBOTS_GROUNDRULES_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "TravelLegRules.h"

// A bot gets to ground before it plans (pathing PA31). A bot has no client gravity: one left in the air stays there,
// and every walk it asks for from that spot is refused (no_start), its hearth too (flying); the night of PA30 found
// 81 % of the goal-less no-route trips asked from the air, picked again every 3 s all night. One rule, three callers:
// a trip that ends with its bot in the air lands it (TripEnd: the cause, whatever leg or end gave the trip up), and
// before a status is picked (Pick) or a trip's route is asked for (Route) a bot in the air comes down first (the bound
// for what is left). The ways down are a player's: on a flying mount a straight flight down (Descend, the fly leg's
// own descent), without one the engine's fall (Fall), a hover on the flyer just above the floor lands (Land). A bot
// on its floor where the navmesh has no polygon makes no move (DP.17: no snap, no straight line): it writes its row
// `none` once per spot, the place is the mesh's to repair (PA42). A bot without a flying mount or flying flags that
// stands on the navmesh (the nearest walkable polygon's surface within STEP_YARDS of its feet) does not fall, whatever
// the floor below it reads: at a ledge's edge or over a gap in a city's floor the reading is the ground far below, and
// the fall dropped the bot through that floor (PA34, G229: Orgrimmar onto the terrain under the city). The walk's start
// box alone (3 yd across, 5 yd up and down, any polygon whose bounds touch it) is not standing: a bot 10 yd over a
// slope counted as on it and stayed in the air (G233). Every way down is a spline (the descent's flight, the engine's
// fall) and the bound waits while it runs, so no route request cuts it (G236: a fall cut every second dropped a bot
// 13 yd a read); a fall or a landing that left the bot above its floor is judged afresh at the next read, with a row
// (PA34, G225). Pure; the glue is NewRpgBaseAction::GetToGround
namespace Ground
{
using TravelRoute::Point;

constexpr float STEP_YARDS = 2.0f;        // more than this above its floor a bot does not stand on it
constexpr std::uint32_t CHECK_MS = 1000;  // a pick or a route request reads the floor at most once a second
constexpr float NONE_SPOT_YARDS = 10.0f;  // one `none` row per spot: again only this far (2D) from the last
constexpr std::uint32_t LANDING_MS = TravelLegs::DESCENT_TIMEOUT_MS;  // a descent this bound started ends by then

enum class Origin : std::uint8_t
{
    Pick,    // before the idle status pick (NewRpgStatusUpdateAction)
    Route,   // before a trip's route request (NewRpgTravelAction::StepTravel)
    TripEnd  // a held trip ends (the status core's Change and TripEnded): it lands its bot, it judges no mesh
};

enum class Act : std::uint8_t
{
    None,     // on the floor, on the mesh (or not this caller's to judge): plan on
    Wait,     // the descent this bound started is under way: no pick, no route request
    Descend,  // on a flying mount in the air: straight down to the floor (MoveTo, as the fly leg's descent)
    Fall,     // in the air without a flying mount: the engine's fall onto the floor (the fall's damage too)
    Land,     // on the flyer just above the floor, or down after the descent: off the mount (LandAndDismount)
    Stay      // on its floor without a polygon: no move (PA42's place), plan on
};

// What the gate reads before the floor: Skip (not this bound's: dead, in combat, a taxi flight, a transport or
// vehicle, in water, falling; or read within CHECK_MS), Wait (its landing is under way), Read (read the floor)
enum class Gate : std::uint8_t
{
    Skip,
    Wait,
    Read
};

// What the navmesh holds at the bot (NavMeshSnap::Nearest in the walk's start box): Unknown where it cannot judge (no
// mesh, no tile), None without a walkable polygon, Found with the nearest point's height on it
struct MeshRead
{
    enum class Kind : std::uint8_t
    {
        Unknown,
        None,
        Found
    };
    Kind kind{Kind::Unknown};
    float z{0.0f};
};

// Standing on the mesh: the nearest polygon's surface within STEP_YARDS of the bot's feet, above or below
inline bool StandsOn(MeshRead const& mesh, float botZ)
{
    return mesh.kind == MeshRead::Kind::Found && std::fabs(botZ - mesh.z) <= STEP_YARDS;
}

struct State
{
    bool skip{false};         // dead, in combat, on a taxi flight, aboard a transport or a vehicle, in water, falling
    bool moving{false};       // a movement spline runs
    bool flyingMount{false};  // mounted with a flight aura (TravelLegs::Observation::flyingMount)
    bool flyingFlag{false};   // the flying movement flags (a dismount in the air can leave them)
    Point bot;
    float groundZ{TravelLegs::FLY_NO_GROUND};  // CheckMountStateAction::FloorBelow
    float waterZ{TravelLegs::FLY_NO_GROUND};
};

// The bot's memory of this bound (BotTravel): the way down it started, the last floor read, the last `none` spot
struct Memo
{
    std::uint32_t downSinceMs{0};
    bool fell{false};  // the way down under way is the engine's fall (Fall, Land), not the descent's flight
    std::uint32_t checkedMs{0};
    bool noneSet{false};
    Point noneAt;
};

struct Step
{
    Act act{Act::None};
    Point target;        // Descend's floor
    float height{0.0f};  // above the floor (yd)
    bool row{false};     // the bound acted (or stays off the mesh at a new spot): one `grounded` row
};

inline bool Landing(Memo const& memo, std::uint32_t nowMs)
{
    return memo.downSinceMs && nowMs - memo.downSinceMs <= LANDING_MS;  // unsigned: wraps as getMSTimeDiff does
}

// O(1): flags only. A trip's end always reads (a trip ends once); a pick or a route request at most once per CHECK_MS
inline Gate GateOf(State const& state, Memo& memo, Origin origin, std::uint32_t nowMs)
{
    if (state.skip)
        return Gate::Skip;
    if (Landing(memo, nowMs))
    {
        if (state.moving)
            return Gate::Wait;  // its spline runs: no read, no pick, no route request that would cut it (G236)
        if (!memo.fell)
            return Gate::Read;  // the descent's end: read at once, its landing writes no row
    }
    memo.downSinceMs = 0;  // none started, a fall's spline ended, or it outlived LANDING_MS: judged afresh (a new row)
    memo.fell = false;
    if (origin != Origin::TripEnd)
    {
        if (memo.checkedMs && nowMs - memo.checkedMs < CHECK_MS)
            return Gate::Skip;
        memo.checkedMs = std::max<std::uint32_t>(nowMs, 1);
    }
    return Gate::Read;
}

// A `none` row only at a spot the last one did not name
inline bool NewNoneSpot(Memo& memo, Point const& bot)
{
    if (memo.noneSet && memo.noneAt.mapId == bot.mapId &&
        std::hypot(memo.noneAt.x - bot.x, memo.noneAt.y - bot.y) < NONE_SPOT_YARDS)
        return false;
    memo.noneSet = true;
    memo.noneAt = bot;
    return true;
}

// After GateOf's Read. meshAt() (a MeshRead) is read only for a bot on its floor at a pick or a route request (any
// polygon in the box: a walk starts there), or for one above its floor that would fall without a flying mount or flags
// (it stands on the polygon: StandsOn); one navmesh query
template <typename MeshAt>
Step Decide(State const& state, Memo& memo, Origin origin, std::uint32_t nowMs, MeshAt&& meshAt)
{
    bool const landing = Landing(memo, nowMs);
    float const floorZ = std::max(state.groundZ, state.waterZ);
    if (floorZ <= TravelLegs::FLY_NO_GROUND)  // no floor found below: nothing to come down to
    {
        memo.downSinceMs = 0;
        if (origin == Origin::TripEnd)
            return {};
        return {Act::Stay, state.bot, 0.0f, NewNoneSpot(memo, state.bot)};
    }
    float const height = state.bot.z - floorZ;
    Point const floor{state.bot.mapId, state.bot.x, state.bot.y, floorZ};
    if (height > STEP_YARDS)
    {
        Act const act = height > TravelLegs::AIRBORNE_YARDS ? (state.flyingMount ? Act::Descend : Act::Fall)
                        : state.flyingMount                 ? Act::Land
                                                            : Act::Fall;
        // On a polygon's surface it stands where a walk starts: the floor reading missed that surface (G229, G233)
        if (act == Act::Fall && !state.flyingFlag && StandsOn(meshAt(), state.bot.z))
        {
            memo.downSinceMs = 0;
            memo.fell = false;
            return {Act::None, floor, height, false};
        }
        if (act != Act::Descend)
        {
            // The engine's fall: the gate waits while its spline runs, then judges afresh (G236, G225)
            memo.downSinceMs = std::max<std::uint32_t>(nowMs, 1);
            memo.fell = true;
            return {act, floor, height, !landing};  // the end of its own descent writes no row
        }
        if (!landing)
            memo.downSinceMs = std::max<std::uint32_t>(nowMs, 1);
        memo.fell = false;
        return {act, floor, height, !landing};  // a descent under way that stopped short goes on without a row
    }
    if (landing)
    {
        memo.downSinceMs = 0;
        if (state.flyingMount || state.flyingFlag)
            return {Act::Land, floor, height, false};  // the end of its own descent
    }
    if (origin == Origin::TripEnd || meshAt().kind != MeshRead::Kind::None)
        return {Act::None, floor, height, false};
    return {Act::Stay, floor, height, NewNoneSpot(memo, state.bot)};
}

// The row's reason: what the bound did and where it was asked (`flew_down:pick`)
inline char const* ActWord(Act act)
{
    switch (act)
    {
        case Act::Descend:
            return "flew_down";
        case Act::Fall:
            return "fell";
        case Act::Land:
            return "landed";
        case Act::Stay:
            return "none";
        default:
            return "wait";
    }
}

inline char const* OriginWord(Origin origin)
{
    return origin == Origin::Pick ? "pick" : origin == Origin::Route ? "route" : "trip_end";
}

// Whether the caller waits this tick (no pick, no route request): the bound moves the bot or its descent is under way
inline bool Holds(Act act) { return act != Act::None && act != Act::Stay; }
}  // namespace Ground

#endif
