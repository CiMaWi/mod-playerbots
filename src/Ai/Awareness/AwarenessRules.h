/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AWARENESSRULES_H
#define _PLAYERBOT_AWARENESSRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// The bot's picture of the units around it, both sides, and the small rules read off it. Game-free for the
// selftest (tests/awareness-rules-selftest.cpp); a rule here decides nothing, its caller chooses.
namespace AwarenessRules
{
// Tuning value: the units one picture holds; AW3's cost rows confirm it
constexpr std::size_t PICTURE_SIZE = 32;
// Tuning values: a picture older than this is rebuilt when read; a fight changes faster than a walk
constexpr std::uint32_t FRESH_IN_FIGHT_MS = 1000;
constexpr std::uint32_t FRESH_OUTSIDE_MS = 3000;

// A unit's link to a unit that is not in the array (fightsIndex)
constexpr std::int16_t NOT_IN_PICTURE = -1;
// A unit's link to the bot itself, which is not in the array: it is Picture::self
constexpr std::int16_t THE_BOT = -2;

// The side of a unit towards this bot. Each enum ends with Count, which is no value: the selftest loops up to it
enum class Side : std::uint8_t
{
    Enemy,    // attacks the bot on sight, or fights the bot's side
    Ally,     // the two cannot attack each other
    Neither,  // may be attacked, does not attack first
    Count
};

// An ally's kind; None for every other side
enum class Kind : std::uint8_t
{
    None,
    Group,   // a member of the bot's group
    Player,  // another player
    Guard,
    Creature,  // another creature
    Count
};

enum class Rank : std::uint8_t
{
    Normal,
    Elite,
    Rare,
    Boss,
    Count
};

enum class UnitType : std::uint8_t
{
    Creature,
    Player,
    Pet,  // Seen::ownerGuid holds its owner
    Count
};

enum class Busy : std::uint8_t
{
    Idle,
    OnPath,  // moving on a waypoint path
    Evading,
    Dead,
    Fighting,  // the only state whose link (fightsGuid, fightsIndex) is a fight
    Count
};

// How the bot knows of a unit; this plan fills only Seen (AW.4)
enum class Known : std::uint8_t
{
    Seen,        // the engine says the bot sees it now
    Remembered,  // seen and lost: its last place, a disc that grows with time
    Inferred,    // never seen, must exist
    Count
};

enum class Role : std::uint8_t
{
    Tank,
    Healer,
    Melee,
    Ranged,
    Other,
    Count
};

// Why an enemy is in a fight (the fight row's reason)
enum class Reason : std::uint8_t
{
    Target,    // the bot chose it
    Attacker,  // it struck first
    Call,      // an enemy fled and called it
    Assist,    // it stood in the assist radius of an enemy that entered
    Reach,     // the bot or its fight came into its reach
    Path,      // it was moving on a path
    New,       // it was not in the last picture
    Other,
    Count
};

// Literal row words (a log keeps the pointer); "?" for no value
constexpr char const* Word(Side side)
{
    switch (side)
    {
        case Side::Enemy:
            return "enemy";
        case Side::Ally:
            return "ally";
        case Side::Neither:
            return "neither";
        default:
            return "?";
    }
}

constexpr char const* Word(Kind kind)
{
    switch (kind)
    {
        case Kind::None:
            return "none";
        case Kind::Group:
            return "group";
        case Kind::Player:
            return "player";
        case Kind::Guard:
            return "guard";
        case Kind::Creature:
            return "creature";
        default:
            return "?";
    }
}

constexpr char const* Word(Rank rank)
{
    switch (rank)
    {
        case Rank::Normal:
            return "normal";
        case Rank::Elite:
            return "elite";
        case Rank::Rare:
            return "rare";
        case Rank::Boss:
            return "boss";
        default:
            return "?";
    }
}

constexpr char const* Word(UnitType type)
{
    switch (type)
    {
        case UnitType::Creature:
            return "creature";
        case UnitType::Player:
            return "player";
        case UnitType::Pet:
            return "pet";
        default:
            return "?";
    }
}

constexpr char const* Word(Busy busy)
{
    switch (busy)
    {
        case Busy::Idle:
            return "idle";
        case Busy::OnPath:
            return "path";
        case Busy::Evading:
            return "evading";
        case Busy::Dead:
            return "dead";
        case Busy::Fighting:
            return "fighting";
        default:
            return "?";
    }
}

constexpr char const* Word(Known known)
{
    switch (known)
    {
        case Known::Seen:
            return "seen";
        case Known::Remembered:
            return "remembered";
        case Known::Inferred:
            return "inferred";
        default:
            return "?";
    }
}

constexpr char const* Word(Role role)
{
    switch (role)
    {
        case Role::Tank:
            return "tank";
        case Role::Healer:
            return "healer";
        case Role::Melee:
            return "melee";
        case Role::Ranged:
            return "ranged";
        case Role::Other:
            return "other";
        default:
            return "?";
    }
}

constexpr char const* Word(Reason reason)
{
    switch (reason)
    {
        case Reason::Target:
            return "target";
        case Reason::Attacker:
            return "attacker";
        case Reason::Call:
            return "call";
        case Reason::Assist:
            return "assist";
        case Reason::Reach:
            return "reach";
        case Reason::Path:
            return "path";
        case Reason::New:
            return "new";
        case Reason::Other:
            return "other";
        default:
            return "?";
    }
}

// One unit the bot knows of, enemy or ally
struct Seen
{
    std::uint64_t guid = 0;       // the raw ObjectGuid: unique across creatures, players and pets
    std::uint64_t ownerGuid = 0;  // a pet's owner
    std::uint32_t entry = 0;
    UnitType type = UnitType::Creature;
    std::uint8_t playerClass = 0;  // a player's class, 0 for a creature
    Side side = Side::Neither;
    Kind kind = Kind::None;
    bool canAttackNow = false;   // a player of the other faction: the two may attack each other now
    std::int16_t levelDiff = 0;  // its level minus the bot's
    Rank rank = Rank::Normal;
    bool casts = false;
    std::uint32_t creatureType = 0;  // the engine's creature type
    float healthPct = 100.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float homeX = 0.0f;  // a creature's home
    float homeY = 0.0f;
    float homeZ = 0.0f;
    float reachYards = 0.0f;  // a creature's aggro reach against this bot; 0 when it would not attack (a player)
    Busy busy = Busy::Idle;
    std::uint64_t fightsGuid = 0;               // whom it fights, 0 for nobody
    std::int16_t fightsIndex = NOT_IN_PICTURE;  // that unit's place in the array, or THE_BOT
    bool taggedByOther = false;
    bool inSight = true;  // line of sight; a unit behind a wall stays in the picture
    Known known = Known::Seen;
    float ageSeconds = 0.0f;      // since it was last seen
    float estimateRadius = 0.0f;  // its place is somewhere within this disc around x, y
};

// The bot itself; it is not in the array, so a link to it is THE_BOT
struct Self
{
    std::uint64_t guid = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::uint8_t level = 0;
    std::uint8_t playerClass = 0;
    Role role = Role::Other;
    float healthPct = 100.0f;
    float powerPct = 100.0f;
    float avgItemLevel = 0.0f;  // of what it wears
    std::uint64_t fightsGuid = 0;
    std::int16_t fightsIndex = NOT_IN_PICTURE;
};

struct Picture
{
    std::array<Seen, PICTURE_SIZE> units{};
    std::uint8_t count = 0;
    std::array<std::uint16_t, static_cast<std::size_t>(Side::Count)> leftOut{};  // per side, when the array was full
    Self self;
    std::uint32_t builtMs = 0;
};

// A link is a fight only on a fighting unit; a dead or idle unit's stale target is no fight
constexpr bool Fights(Seen const& unit)
{
    return unit.busy == Busy::Fighting && (unit.fightsGuid != 0 || unit.fightsIndex != NOT_IN_PICTURE);
}

inline float PlanarDistance(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

// Every distance rule measures against the disc of the estimate, so a unit known and not seen needs no second rule
inline float DistanceToDisc(Seen const& unit, float x, float y)
{
    return std::max(0.0f, PlanarDistance(unit.x, unit.y, x, y) - unit.estimateRadius);
}

// Its reach covers this point with this margin; a unit without reach (a player) covers nothing
inline bool Covers(Seen const& unit, float x, float y, float margin)
{
    return unit.reachYards > 0.0f && DistanceToDisc(unit, x, y) <= unit.reachYards + margin;
}

// The two discs come within this radius of each other
inline bool Within(Seen const& a, Seen const& b, float radius)
{
    float gap = PlanarDistance(a.x, a.y, b.x, b.y) - a.estimateRadius - b.estimateRadius;
    return std::max(0.0f, gap) <= radius;
}

// Who would join a target, the place part (PLAN §3.1 Joining): when a creature enters a fight, the engine calls
// every creature within its assist radius, centre to centre, that would assist it (CallAssistance, CanAssistTo).
// Here: a creature hostile to the bot, free (not a pet), not dead, evading or engaged already, within `radius` of the
// target. Faction, civilians, flags and the line of sight from the target are the engine's (Awareness::JoinersOf)
inline bool WouldAssistByPlace(Seen const& neighbour, Seen const& target, float radius)
{
    bool const free = neighbour.busy == Busy::Idle || neighbour.busy == Busy::OnPath;
    return radius > 0.0f && neighbour.guid != target.guid && neighbour.type == UnitType::Creature &&
           neighbour.side == Side::Enemy && free && Within(neighbour, target, radius);
}

// A picture of this age is still good; equal to the limit is still fresh
constexpr bool IsFresh(std::uint32_t ageMs, bool inFight)
{
    return ageMs <= (inFight ? FRESH_IN_FIGHT_MS : FRESH_OUTSIDE_MS);
}

// What was known when an enemy entered the fight: from the last picture before it and the facts at that moment. The
// caller measures the radii (the engine's assist and call distances) with Within and Covers
struct EntryFacts
{
    bool inLastPicture = false;
    Busy lastBusy = Busy::Idle;  // its state in the last picture
    bool chosen = false;         // the bot chose it as its target
    bool struckFirst = false;    // it attacked before the bot attacked it
    bool called = false;         // a fleeing enemy of the fight called it
    bool assisted = false;       // an enemy that entered stood within the assist radius of it
    bool reached = false;        // the bot or its fight came within its reach
};

// The bot's own choice and the enemy's own act come first; then the engine's pulls, the specific call before the
// general assist and reach; a path and a newcomer only explain what nothing else does
constexpr Reason WhyInFight(EntryFacts const& facts)
{
    if (facts.chosen)
        return Reason::Target;
    if (facts.struckFirst)
        return Reason::Attacker;
    if (facts.called)
        return Reason::Call;
    if (facts.assisted)
        return Reason::Assist;
    if (facts.reached)
        return Reason::Reach;
    if (!facts.inLastPicture)
        return Reason::New;
    if (facts.lastBusy == Busy::OnPath)
        return Reason::Path;
    return Reason::Other;
}

// Of two units of one side the nearer to the bot: the smaller distance to its disc, at equal distance the lower guid
inline bool Nearer(Seen const& a, Seen const& b, float x, float y)
{
    float const da = DistanceToDisc(a, x, y);
    float const db = DistanceToDisc(b, x, y);
    return da != db ? da < db : a.guid < b.guid;
}

// The slot of the unit of this guid: THE_BOT for the bot, NOT_IN_PICTURE for nobody or a unit not kept
inline std::int16_t SlotOf(Picture const& picture, std::uint64_t guid)
{
    if (!guid)
        return NOT_IN_PICTURE;
    if (guid == picture.self.guid)
        return THE_BOT;
    for (std::size_t i = 0; i < picture.count; ++i)
        if (picture.units[i].guid == guid)
            return static_cast<std::int16_t>(i);
    return NOT_IN_PICTURE;
}

// Offers one unit to the picture; Picture::self (the bot's place) must be set first. Where more units stand than the
// array holds, the sides take turns: the nearest of each side, then the second nearest of each, and so on (a unit's
// rank is the number of units of its side that are nearer; at equal rank enemy before ally before neither). So a
// town's many allies never crowd out an approaching enemy, a side that runs out leaves its share to the others, and
// the kept units and counts do not depend on the order the units are offered in. When the array is full, the side
// with the most units among the array and the newcomer gives up its farthest one (at equal numbers neither before
// ally before enemy); that unit is counted in leftOut. Slots change, so Admit keeps no link: LinkUp after the last.
// `capacity` below PICTURE_SIZE is for the selftest's small arrays
inline void Admit(Picture& picture, Seen const& unit, std::size_t capacity = PICTURE_SIZE)
{
    if (picture.count < std::min(capacity, PICTURE_SIZE))
    {
        picture.units[picture.count++] = unit;
        return;
    }
    constexpr std::size_t SIDES = static_cast<std::size_t>(Side::Count);
    std::size_t const own = static_cast<std::size_t>(unit.side);
    std::array<std::size_t, SIDES> held{};
    std::array<Seen const*, SIDES> farthest{};
    held[own] = 1;
    farthest[own] = &unit;
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& kept = picture.units[i];
        std::size_t const side = static_cast<std::size_t>(kept.side);
        ++held[side];
        if (!farthest[side] || Nearer(*farthest[side], kept, picture.self.x, picture.self.y))
            farthest[side] = &kept;
    }
    std::size_t giver = 0;
    for (std::size_t side = 1; side < SIDES; ++side)
        if (held[side] >= held[giver])
            giver = side;
    ++picture.leftOut[giver];
    if (farthest[giver] != &unit)
        picture.units[static_cast<std::size_t>(farthest[giver] - picture.units.data())] = unit;
}

// Sets every link's slot from its guid, once after the last Admit: the bot is THE_BOT, a unit not kept NOT_IN_PICTURE
inline void LinkUp(Picture& picture)
{
    picture.self.fightsIndex = SlotOf(picture, picture.self.fightsGuid);
    for (std::size_t i = 0; i < picture.count; ++i)
        picture.units[i].fightsIndex = SlotOf(picture, picture.units[i].fightsGuid);
}

// Units of the picture: slots, or THE_BOT
struct UnitList
{
    std::array<std::int16_t, PICTURE_SIZE> refs{};
    std::uint8_t size = 0;
};

// The two sides of the fight around a unit; the unit itself is in neither list
struct Sides
{
    UnitList beside;   // its own side
    UnitList against;  // the other side
};

// The sides of the fight around a unit (A8): `centre` is a slot or THE_BOT. The members are the bot and every unit
// that is not dead. A link is a member's "fights whom" (Fights; the bot's Self::fightsIndex) and counts when it
// names another member. The fight around the centre is every member reached from it over counting links, each
// followed in either direction; a member an odd number of links away (the fewest) is against it, an even number
// beside it. Only this rule gives a unit and the unit it fights the same two groups, swapped, and has every link
// cross between them; in a ring of three or another odd number of units no split can, and "fewest" makes each
// ring member read its direct opponents as against. Side, kind and distance are not read: whether a unit beside is
// help is the caller's. Nothing is stored per fight; one walk over at most 33 members, each taken once
inline Sides SidesAround(Picture const& picture, std::int16_t centre)
{
    Sides sides;
    int const bot = picture.count;
    auto member = [&picture, bot](int ref)
    {
        if (ref == THE_BOT)
            return bot;
        if (ref >= 0 && ref < bot && picture.units[ref].busy != Busy::Dead)
            return ref;
        return -1;
    };
    int const start = member(centre);
    if (start < 0)
        return sides;
    // Each member's link as a member, -1 where it counts nowhere (the bot is member `bot`). A link to its own holder
    // needs no test: the walk has already taken the holder when it reads that link, so it joins nobody
    std::array<int, PICTURE_SIZE + 1> link{};
    for (int u = 0; u < bot; ++u)
    {
        Seen const& unit = picture.units[u];
        link[u] = unit.busy == Busy::Dead || !Fights(unit) ? -1 : member(unit.fightsIndex);
    }
    link[bot] = member(picture.self.fightsIndex);

    std::array<int, PICTURE_SIZE + 1> steps;
    steps.fill(-1);
    std::array<int, PICTURE_SIZE + 1> queue{};
    int head = 0;
    int tail = 0;
    steps[start] = 0;
    queue[tail++] = start;
    while (head < tail)
    {
        int const u = queue[head++];
        for (int v = 0; v <= bot; ++v)
        {
            if (steps[v] < 0 && (link[u] == v || link[v] == u))
            {
                steps[v] = steps[u] + 1;
                queue[tail++] = v;
            }
        }
    }
    for (int v = 0; v <= bot; ++v)
    {
        if (v == start || steps[v] < 0)
            continue;
        UnitList& list = steps[v] % 2 ? sides.against : sides.beside;
        list.refs[list.size++] = v == bot ? THE_BOT : static_cast<std::int16_t>(v);
    }
    return sides;
}
}  // namespace AwarenessRules

#endif
