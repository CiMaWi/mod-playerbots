/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_STANDRULES_H
#define _PLAYERBOT_STANDRULES_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Where a bot stands to use a creature or an object (a talk, a trade, a turn-in, a loot, a mailbox, a flight master):
// one rule (pathing PA.16) for the approach, the offline target check and the graph's target nodes. Pure: no core
// headers, so tests/stand-selftest.cpp compiles it alone. NavMeshSnap::StandPoint (RpgNavMeshSnap.h) gathers the
// candidate points on the mesh and their facts; this file picks one:
// 1. between the least and the greatest distance (the greatest: what the core's own check allows, less a margin);
// 2. on joined floor (not a closed piece of the mesh): `fragment` when no candidate in the band is joined;
// 3. in sight of the target first: when no free joined point is in sight, the best one out of sight, and the answer
//    says so (`inSight` false, the check's `sight 0`; pathing PA.14 settled it: the core's check asks no sight);
//    in front of it first, beside it next, behind it last;
// 4. spread: each bot its own point of the best side (by its key), none another bot stands on.
namespace StandRules
{
// The core's numbers (ObjectDefines.h INTERACTION_DISTANCE, DEFAULT_COMBAT_REACH; GameObject::GetInteractionDistance)
constexpr float INTERACTION_YARDS = 5.5f;
constexpr float BOT_SIZE_YARDS = 1.5f;  // a player's combat reach, which the core adds to a creature's reach
constexpr float MAILBOX_YARDS = 10.0f;
constexpr float DOOR_YARDS = 5.0f;
constexpr float QUESTGIVER_OBJECT_YARDS = 5.5555553f;
constexpr float BINDER_YARDS = 10.0f;
constexpr float CHAIR_YARDS = 3.0f;
// Tuning values (read from the nights)
constexpr float MARGIN_YARDS = 0.5f;  // the greatest distance stays this far inside the core's check
constexpr float LEAST_YARDS = 1.5f;   // keeps the bot's model off the target's (3D, centre to centre)
constexpr float BODY_YARDS = 1.0f;    // a body's width: two bots nearer than this stand in each other
// Two stand points keep a player's reach apart (the core's DEFAULT_PLAYER_COMBAT_REACH), more than a body's width, so
// a walk that ends up to half a yard off its point still leaves the two bodies apart (pathing PA47)
constexpr float APART_YARDS = 1.5f;
constexpr float FRONT_DEGREES = 60.0f;
constexpr float BEHIND_DEGREES = 120.0f;
// The mesh half's sampling (NavMeshSnap::StandPoint)
constexpr float RING_STEP_YARDS = 1.0f;
constexpr int RING_ANGLES = 24;
constexpr int MOST_RINGS = 16;  // a large target's band is sampled in at most this many rings
constexpr float SAME_POINT_YARDS = 0.25f;

constexpr float PI = 3.14159265358979f;

// Game object types the core gives their own interaction distance (SharedDefines.h GameobjectTypes)
enum ObjectType : std::uint32_t
{
    OBJECT_DOOR = 0,
    OBJECT_QUESTGIVER = 2,
    OBJECT_BINDER = 4,
    OBJECT_CHAIR = 7,
    OBJECT_TEXT = 9,
    OBJECT_MAILBOX = 19,
    OBJECT_FLAGSTAND = 24,
    OBJECT_FLAGDROP = 26,
    OBJECT_MINI_GAME = 27,
    OBJECT_BARBER_CHAIR = 32,
    OBJECT_GUILD_BANK = 34
};

enum class Side : std::uint8_t
{
    Front,
    Beside,
    Behind
};

enum class Why : std::uint8_t
{
    Found,
    Fragment,  // candidates in the band, none on joined floor
    NoPoly,    // no candidate in the band
    Occupied   // every joined candidate has a bot on it
};

// A game object's model box in its own frame, scaled (GameObjectDisplayInfo min/max times the object's scale)
struct Box
{
    float minX{0.0f};
    float minY{0.0f};
    float minZ{0.0f};
    float maxX{0.0f};
    float maxY{0.0f};
    float maxZ{0.0f};
};

struct Target
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float facing{0.0f};
    bool faced{true};      // false: the target has no front (every point counts as in front)
    float maxYards{0.0f};  // the greatest distance, measured as the core does (MaxYards)
    float minYards{0.0f};  // the least distance, centre to centre (MinYards)
    bool boxed{false};     // the core measures to the model box: maxYards is the box's margin, not a radius
    Box box;
    float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};  // the object's world rotation (x, y, z, w), for the box
    float capYards{0.0f};  // a centre-to-centre bound the caller's own reach check adds (CallerCap); 0: none
};

struct Candidate
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    bool joined{false};    // on joined floor: not a closed piece of the mesh (NavMeshSnap::IsFragment's flood)
    bool inSight{false};   // the target is in sight from the point
    bool occupied{false};  // another bot stands or has claimed its point within APART_YARDS of it (Taken)
};

struct Answer
{
    Why why{Why::NoPoly};
    std::size_t index{0};  // the candidate; on Fragment the nearest in the band (the closed piece's point)
    Side side{Side::Front};
    float yards{0.0f};   // centre to centre, 3D
    bool inSight{true};  // false: no free joined point in sight, the best one out of sight
};

// The core's check for a creature: 5.5 plus both objects' sizes, 3D (Player::GetNPCIfCanInteractWith, Object.cpp
// _IsWithinDist); `targetSize` is the creature's combat reach (its model's, times its scale)
inline float CreatureReach(float targetSize, float botSize = BOT_SIZE_YARDS)
{
    return INTERACTION_YARDS + botSize + targetSize;
}

// The core's check for a game object (GameObject::GetInteractionDistance), measured to its model box where it has one
inline float ObjectReach(std::uint32_t type)
{
    switch (type)
    {
        case OBJECT_DOOR:
            return DOOR_YARDS;
        case OBJECT_QUESTGIVER:
        case OBJECT_TEXT:
        case OBJECT_FLAGSTAND:
        case OBJECT_FLAGDROP:
        case OBJECT_MINI_GAME:
            return QUESTGIVER_OBJECT_YARDS;
        case OBJECT_BINDER:
            return BINDER_YARDS;
        case OBJECT_CHAIR:
        case OBJECT_BARBER_CHAIR:
            return CHAIR_YARDS;
        case OBJECT_MAILBOX:
        case OBJECT_GUILD_BANK:
            return MAILBOX_YARDS;
        default:
            return INTERACTION_YARDS;
    }
}

inline float MaxYards(float coreReach) { return coreReach > MARGIN_YARDS ? coreReach - MARGIN_YARDS : 0.0f; }

inline float MinYards(float maxYards) { return maxYards * 0.5f < LEAST_YARDS ? maxYards * 0.5f : LEAST_YARDS; }

inline float Distance(float ax, float ay, float az, float bx, float by, float bz)
{
    float const dx = ax - bx;
    float const dy = ay - by;
    float const dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// How far a point lies outside the model box, per axis as the core's test (the box grown by the radius on each axis,
// GameObject::IsAtInteractDistance): the point passes a radius r exactly when this is at most r
inline float BoxYards(Target const& target, float x, float y, float z)
{
    float qx = -target.rotation[0];  // the inverse rotation takes the point into the box's frame
    float qy = -target.rotation[1];
    float qz = -target.rotation[2];
    float qw = target.rotation[3];
    float const length = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (length > 0.0f)
    {
        qx /= length;
        qy /= length;
        qz /= length;
        qw /= length;
    }
    else
        qw = 1.0f;
    float const vx = x - target.x;
    float const vy = y - target.y;
    float const vz = z - target.z;
    float const tx = 2.0f * (qy * vz - qz * vy);
    float const ty = 2.0f * (qz * vx - qx * vz);
    float const tz = 2.0f * (qx * vy - qy * vx);
    float const lx = vx + qw * tx + (qy * tz - qz * ty);
    float const ly = vy + qw * ty + (qz * tx - qx * tz);
    float const lz = vz + qw * tz + (qx * ty - qy * tx);
    auto const outside = [](float value, float low, float high)
    { return value < low ? low - value : (value > high ? value - high : 0.0f); };
    float const ox = outside(lx, target.box.minX, target.box.maxX);
    float const oy = outside(ly, target.box.minY, target.box.maxY);
    float const oz = outside(lz, target.box.minZ, target.box.maxZ);
    return ox > oy ? (ox > oz ? ox : oz) : (oy > oz ? oy : oz);
}

// The distance the greatest is measured in: to the model box for a boxed object, else centre to centre (3D)
inline float CoreYards(Target const& target, float x, float y, float z)
{
    return target.boxed ? BoxYards(target, x, y, z) : Distance(x, y, z, target.x, target.y, target.z);
}

inline bool InBand(Target const& target, float x, float y, float z)
{
    float const yards = Distance(x, y, z, target.x, target.y, target.z);
    return yards >= target.minYards && CoreYards(target, x, y, z) <= target.maxYards &&
           (target.capYards <= 0.0f || yards <= target.capYards);
}

// Front within FRONT_DEGREES of the target's facing, behind beyond BEHIND_DEGREES, beside between
inline Side SideOf(Target const& target, float x, float y)
{
    if (!target.faced)
        return Side::Front;
    float const dx = x - target.x;
    float const dy = y - target.y;
    if (dx * dx + dy * dy < 1e-6f)
        return Side::Front;
    float off = std::atan2(dy, dx) - target.facing;
    while (off > PI)
        off -= 2.0f * PI;
    while (off < -PI)
        off += 2.0f * PI;
    float const degrees = std::fabs(off) * 180.0f / PI;
    if (degrees <= FRONT_DEGREES)
        return Side::Front;
    return degrees > BEHIND_DEGREES ? Side::Behind : Side::Beside;
}

// A bot's share of a side's points: the same bot repeats its point, two bots mostly differ (the rest: occupied)
inline std::size_t Spread(std::uint64_t key, std::size_t count)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return count ? static_cast<std::size_t>(key % count) : 0;
}

// PA.16's order over the candidates; `key` spreads the bots (the offline check passes 0)
inline Answer Pick(Target const& target, std::vector<Candidate> const& candidates, std::uint64_t key)
{
    Answer answer;
    std::vector<std::size_t> band;
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (InBand(target, candidates[i].x, candidates[i].y, candidates[i].z))
            band.push_back(i);
    if (band.empty())
        return answer;

    std::vector<std::size_t> joined;
    for (std::size_t const i : band)
        if (candidates[i].joined)
            joined.push_back(i);
    if (joined.empty())
    {
        answer.why = Why::Fragment;
        float best = 0.0f;
        for (std::size_t const i : band)
        {
            Candidate const& c = candidates[i];
            float const yards = Distance(c.x, c.y, c.z, target.x, target.y, target.z);
            if (i == band.front() || yards < best)
            {
                best = yards;
                answer.index = i;
            }
        }
        answer.yards = best;
        answer.side = SideOf(target, candidates[answer.index].x, candidates[answer.index].y);
        return answer;
    }

    std::vector<std::size_t> free;
    for (std::size_t const i : joined)
        if (!candidates[i].occupied)
            free.push_back(i);
    if (free.empty())
    {
        answer.why = Why::Occupied;
        return answer;
    }

    std::vector<std::size_t> seen;
    for (std::size_t const i : free)
        if (candidates[i].inSight)
            seen.push_back(i);
    answer.inSight = !seen.empty();
    if (answer.inSight)
        free = seen;

    Side best = Side::Behind;
    for (std::size_t const i : free)
    {
        Side const side = SideOf(target, candidates[i].x, candidates[i].y);
        if (side < best)
            best = side;
    }
    std::vector<std::size_t> pool;
    for (std::size_t const i : free)
        if (SideOf(target, candidates[i].x, candidates[i].y) == best)
            pool.push_back(i);
    answer.why = Why::Found;
    answer.index = pool[Spread(key, pool.size())];
    answer.side = best;
    Candidate const& chosen = candidates[answer.index];
    answer.yards = Distance(chosen.x, chosen.y, chosen.z, target.x, target.y, target.z);
    return answer;
}

inline char const* Word(Why why)
{
    switch (why)
    {
        case Why::Found:
            return "found";
        case Why::Fragment:
            return "fragment";
        case Why::Occupied:
            return "occupied";
        default:
            return "no_poly";
    }
}

inline char const* Word(Side side)
{
    switch (side)
    {
        case Side::Front:
            return "front";
        case Side::Beside:
            return "beside";
        default:
            return "behind";
    }
}

// The rings the mesh half samples: horizontal radii from the least distance out to `outer`, at most MOST_RINGS
inline std::vector<float> Rings(float least, float outer)
{
    std::vector<float> rings;
    if (outer < least)
        return rings;
    float step = RING_STEP_YARDS;
    if ((outer - least) / step > static_cast<float>(MOST_RINGS - 1))
        step = (outer - least) / static_cast<float>(MOST_RINGS - 1);
    for (float r = least; r < outer; r += step)
        rings.push_back(r);
    rings.push_back(outer);
    return rings;
}

// How far out (horizontally) and up and down the band can reach from the target's place
inline float OuterYards(Target const& target)
{
    auto const capped = [&target](float outer)
    { return target.capYards > 0.0f && target.capYards < outer ? target.capYards : outer; };
    if (!target.boxed)
        return capped(target.maxYards);
    float const hx = std::fabs(target.box.minX) > std::fabs(target.box.maxX) ? std::fabs(target.box.minX)
                                                                             : std::fabs(target.box.maxX);
    float const hy = std::fabs(target.box.minY) > std::fabs(target.box.maxY) ? std::fabs(target.box.minY)
                                                                             : std::fabs(target.box.maxY);
    float const hz = std::fabs(target.box.minZ) > std::fabs(target.box.maxZ) ? std::fabs(target.box.minZ)
                                                                             : std::fabs(target.box.maxZ);
    return capped(std::sqrt(hx * hx + hy * hy + hz * hz) + target.maxYards);  // the box may be turned any way
}

// Taken (pathing PA47): another bot stands, or has claimed its stand point, within apartYards of the point
inline bool Taken(float x, float y, float z, std::vector<std::array<float, 3>> const& occupants,
                  float apartYards = APART_YARDS)
{
    for (std::array<float, 3> const& occupant : occupants)
        if (Distance(occupant[0], occupant[1], occupant[2], x, y, z) < apartYards)
            return true;
    return false;
}

// Whose points count for a target (pathing PA47): every bot and claim within the band's reach plus APART_YARDS of the
// target's place, keyed by place and not by object, so neighbouring targets (two bankers side by side) and a trip's
// end at the place see each other's points
inline float OccupantYards(Target const& target) { return OuterYards(target) + APART_YARDS; }

inline bool CountsFor(Target const& target, float x, float y, float z)
{
    return Distance(target.x, target.y, target.z, x, y, z) <= OccupantYards(target);
}

// The approach's target (pathing PA47), built from the live creature or object as the offline check builds it from the
// stores. The caller's reach is measured as its own check measures it, between the two bodies' edges
// (Unit::IsWithinDistInMap, WorldObject::GetDistance): it bounds the centre at that reach plus both sizes, less the
// margin, beside the kind's own greatest distance
inline float CallerCap(float callerYards, float botSize, float targetSize)
{
    return MaxYards(callerYards + botSize + targetSize);
}

inline Target Placed(float x, float y, float z, float facing)
{
    Target target;
    target.x = x;
    target.y = y;
    target.z = z;
    target.facing = facing;
    return target;
}

// A creature: the core's reach for its size, within the caller's
inline Target ForCreature(float x, float y, float z, float facing, float size, float botSize, float callerYards)
{
    Target target = Placed(x, y, z, facing);
    float const kind = MaxYards(CreatureReach(size, botSize));
    float const caller = CallerCap(callerYards, botSize, size);
    target.maxYards = kind < caller ? kind : caller;
    target.minYards = MinYards(target.maxYards);
    return target;
}

// A game object: the core's reach for its type, measured to its model box where it has one, within the caller's
inline Target ForObject(float x, float y, float z, float facing, std::uint32_t type, Box const* box,
                        float const (&rotation)[4], float objectSize, float botSize, float callerYards)
{
    Target target = Placed(x, y, z, facing);
    float const kind = MaxYards(ObjectReach(type));
    float const caller = CallerCap(callerYards, botSize, objectSize);
    if (box)
    {
        target.boxed = true;
        target.box = *box;
        for (int i = 0; i < 4; ++i)
            target.rotation[i] = rotation[i];
        target.maxYards = kind;
        target.capYards = caller;
        target.minYards = MinYards(caller);
        return target;
    }
    target.maxYards = kind < caller ? kind : caller;
    target.minYards = MinYards(target.maxYards);
    return target;
}

// A spell focus (a craft station) or a place: the caller's reach centre to centre, as the spell's focus check or the
// trip's goal measures it; a place has no front
inline Target ForFocus(float x, float y, float z, float facing, float yards)
{
    Target target = Placed(x, y, z, facing);
    target.maxYards = yards;
    target.minYards = MinYards(yards);
    return target;
}

inline Target ForPlace(float x, float y, float z, float reachYards)
{
    Target target = ForFocus(x, y, z, 0.0f, MaxYards(reachYards));
    target.faced = false;
    return target;
}

// The reach the walk's judge reads (GoTo::Decide): the band's outer bound, centre to centre
inline float CentreReach(Target const& target) { return OuterYards(target); }

// The world log's `stand` row (WL.9): the side, or none; ok, no_sight (the best joined point out of sight) or the
// rule's word for no point
inline char const* RowChoice(Answer const& answer) { return answer.why == Why::Found ? Word(answer.side) : "none"; }
inline char const* RowReason(Answer const& answer)
{
    if (answer.why != Why::Found)
        return Word(answer.why);
    return answer.inSight ? "ok" : "no_sight";
}
}  // namespace StandRules

#endif
