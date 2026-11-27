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
#include <string_view>

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

// A unit's kind of movement: a creature's from the engine's current movement generator, so a wanderer or a patroller
// that pauses keeps its kind; a player or a pet stands or is Other while it moves (AW32b; for the record only)
enum class Motion : std::uint8_t
{
    Standing,
    Wandering,
    Path,     // a waypoint path, an escort or a formation
    Home,     // running home after a fight
    Chasing,  // a fight's chase
    Other,
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

constexpr char const* Word(Motion motion)
{
    switch (motion)
    {
        case Motion::Standing:
            return "standing";
        case Motion::Wandering:
            return "wandering";
        case Motion::Path:
            return "path";
        case Motion::Home:
            return "home";
        case Motion::Chasing:
            return "chasing";
        case Motion::Other:
            return "other";
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
    // A creature with the guard flag, of either side: an ally's readers take it from kind, known danger's live check
    // an enemy's (AW36b)
    bool guard = false;
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
    // A creature on a waypoint path: its velocity in yd/s as the engine has it at the fill; 0 and 0 when it stands
    // and for every unit not on a path (AW7)
    float moveX = 0.0f;
    float moveY = 0.0f;
    Motion motion = Motion::Standing;  // its kind of movement at the fill (AW32b)
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
// every creature within its assist radius that would assist it (CallAssistance, CanAssistTo), measured as the engine
// measures: the straight distance between centres in three dimensions, without the units' sizes (AW12). The discs of
// an estimate still widen it: the planar part is the gap between the two discs. Here: a creature hostile to the bot,
// free (not a pet), not dead, evading or engaged already, within `radius` of the target. Faction, civilians, flags and
// the line of sight from the target are the engine's (Awareness::JoinersOf)
inline bool WouldAssistByPlace(Seen const& neighbour, Seen const& target, float radius)
{
    bool const free = neighbour.busy == Busy::Idle || neighbour.busy == Busy::OnPath;
    if (radius <= 0.0f || neighbour.guid == target.guid || neighbour.type != UnitType::Creature ||
        neighbour.side != Side::Enemy || !free)
        return false;
    float const gap =
        PlanarDistance(neighbour.x, neighbour.y, target.x, target.y) - neighbour.estimateRadius - target.estimateRadius;
    return std::hypot(std::max(0.0f, gap), neighbour.z - target.z) <= radius;
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
// How a fight began (the FightScene row, AW5): the bot's own attack on a target it picked, or an enemy's attack
enum class Began : std::uint8_t
{
    Chosen,
    Attacked,
    Count
};

// Whose choice the bot's own attack was: the grind's, the quest work's or another caller's
enum class Caller : std::uint8_t
{
    Grind,
    Quest,
    Other,
    Count
};

// What is known at the fight's enter edge. The note is written by the bot's own attack call (AttackAction::Attack)
// and cleared at every enter and leave edge
struct BeginFacts
{
    std::uint64_t firstEnemy = 0;   // the unit the engine names at the enter edge
    std::uint64_t notedTarget = 0;  // the target of the bot's last own attack call since the last edge, 0 for none
    Caller notedCaller = Caller::Other;
    std::uint64_t victim = 0;     // the bot's attack target at the edge
    std::uint64_t selection = 0;  // the bot's selected unit (an attack the bot defers selects its target first)
};

struct Beginning
{
    Began began = Began::Attacked;
    Caller caller = Caller::Other;
};

// Chosen when the unit that entered is the one the bot's own attack call named and the bot still aims at it (a stale
// note of a target the bot left does not count); otherwise an enemy's attack began it
constexpr Beginning HowBegan(BeginFacts const& facts)
{
    bool const aimed = facts.notedTarget != 0 && facts.notedTarget == facts.firstEnemy &&
                       (facts.victim == facts.firstEnemy || facts.selection == facts.firstEnemy);
    if (!aimed)
        return Beginning{Began::Attacked, Caller::Other};
    return Beginning{Began::Chosen, facts.notedCaller};
}

// The FightScene row's word
constexpr char const* Word(Beginning const& beginning)
{
    if (beginning.began != Began::Chosen)
        return "attacked";
    switch (beginning.caller)
    {
        case Caller::Grind:
            return "chosen_grind";
        case Caller::Quest:
            return "chosen_quest";
        default:
            return "chosen_other";
    }
}

// The caller of an attack by the attack action's name
constexpr Caller CallerOf(std::string_view action)
{
    if (action == "attack anything")
        return Caller::Grind;
    if (action == "new rpg attack quest target")
        return Caller::Quest;
    return Caller::Other;
}

// The note's caller when an attack is noted again: on the same target a caller that chose nothing (melee follows the
// target a chooser set) keeps the first one; a new target or a named chooser takes the note
constexpr Caller KeptCaller(std::uint64_t notedTarget, Caller notedCaller, std::uint64_t target, Caller caller)
{
    return target == notedTarget && caller == Caller::Other ? notedCaller : caller;
}

// The rows' packing (AW5; the world log's id is 32 bits, its amount 64): `.claude/docs/world-log.md`, the rows
// FightScene, FightEnemy and DeathScene, and config/wlog-checks/awareness.sql unpack them
constexpr std::uint32_t ROSE_NONE = 32767;  // seconds since the bot rose: none since the login, or longer
constexpr std::uint32_t YARDS_NONE = 1023;  // yards from the rise place: no rise, another map, or farther
constexpr std::uint32_t HELPER_NONE = 255;  // yards to the nearest helper: none
constexpr std::uint32_t KILLER_NONE = 3;    // the killer's unit type beside UnitType's 0 to 2: none (environment)
constexpr std::uint32_t COUNT_MAX = 127;
constexpr std::uint32_t PERCENT_MAX = 100;

constexpr std::uint64_t Capped(std::uint64_t value, std::uint64_t cap) { return value < cap ? value : cap; }

// A level difference as 7 bits: -64..63 stored plus 64
constexpr std::uint64_t LevelField(int diff) { return static_cast<std::uint64_t>(std::clamp(diff, -64, 63) + 64); }

// The bot and its side at the fight's start
struct SceneRow
{
    std::uint32_t level = 0;
    std::uint32_t healthPct = 0;
    std::uint32_t powerPct = 0;
    std::uint32_t itemLevel = 0;
    std::uint32_t roseSeconds = ROSE_NONE;
    std::uint32_t group = 0;  // members of the bot's group and their pets
    std::uint32_t guards = 0;
    std::uint32_t allies = 0;  // other allies that fight the bot's enemies
    std::uint32_t nearby = 0;  // enemies beside the target whose reach covers the bot or the target
    bool moving = false;       // the bot moved as the fight opened (AW32b)
    bool onLeg = false;        // a leg of its trip was under way
};

// FightScene id: level | health % << 7 | power % << 14 | item level << 21 (7, 7, 7, 9 bits)
constexpr std::uint32_t SceneId(SceneRow const& row)
{
    return static_cast<std::uint32_t>(Capped(row.level, 127) | Capped(row.healthPct, PERCENT_MAX) << 7 |
                                      Capped(row.powerPct, PERCENT_MAX) << 14 | Capped(row.itemLevel, 511) << 21);
}

// FightScene amount: seconds since the bot rose | group << 15 | guards << 22 | allies << 29 | near << 36 |
// moving << 43 | on a leg << 44 (15, then 7, then 1 bit each)
constexpr std::uint64_t SceneAmount(SceneRow const& row)
{
    return Capped(row.roseSeconds, ROSE_NONE) | Capped(row.group, COUNT_MAX) << 15 |
           Capped(row.guards, COUNT_MAX) << 22 | Capped(row.allies, COUNT_MAX) << 29 |
           Capped(row.nearby, COUNT_MAX) << 36 | std::uint64_t{row.moving} << 43 | std::uint64_t{row.onLeg} << 44;
}

// One enemy as it entered the fight
struct EnemyRow
{
    std::uint32_t tenths = 0;  // since the fight's start
    int levelDiff = 0;         // its level minus the bot's
    Rank rank = Rank::Normal;
    bool casts = false;
    std::uint32_t creatureType = 0;
    std::uint32_t healthPct = 0;
    UnitType type = UnitType::Creature;
    Motion motion = Motion::Standing;  // its kind of movement as it joined
};

// The kind of movement an enemy joins a fight with: the engine's live kind, unless that is already the chase; then the
// kind in the last picture before it joined, when that holds the unit and is no older than a picture outside a fight
constexpr Motion MotionAtJoin(Motion live, Seen const* pictured, std::uint32_t pictureAgeMs)
{
    return live == Motion::Chasing && pictured && pictureAgeMs <= FRESH_OUTSIDE_MS ? pictured->motion : live;
}

// FightEnemy amount: tenths | level diff + 64 << 15 | rank << 22 | casts << 25 | creature type << 26 | health % << 31 |
// unit type << 38 | motion + 1 << 40 (15, 7, 3, 1, 5, 7, 2, 3 bits; a motion of 0 is a row of an older build)
constexpr std::uint64_t EnemyAmount(EnemyRow const& row)
{
    constexpr std::uint64_t lastMotion = static_cast<std::uint64_t>(Motion::Count) - 1;
    return Capped(row.tenths, 32767) | LevelField(row.levelDiff) << 15 |
           Capped(static_cast<std::uint64_t>(row.rank), 7) << 22 | std::uint64_t{row.casts} << 25 |
           Capped(row.creatureType, 31) << 26 | Capped(row.healthPct, PERCENT_MAX) << 31 |
           Capped(static_cast<std::uint64_t>(row.type), 3) << 38 |
           (Capped(static_cast<std::uint64_t>(row.motion), lastMotion) + 1) << 40;
}

// A death: the killer, the fight and the nearest helper
struct DeathRow
{
    int killerLevelDiff = 0;
    Rank killerRank = Rank::Normal;
    std::uint32_t killerType = KILLER_NONE;  // UnitType's value, or KILLER_NONE
    std::uint32_t enemies = 0;               // enemies of the fight still alive
    std::uint32_t roseSeconds = ROSE_NONE;
    std::uint32_t roseYards = YARDS_NONE;
    std::uint32_t helperYards = HELPER_NONE;
};

// DeathScene amount: killer diff + 64 | rank << 7 | enemies << 10 | rose s << 17 | rose yd << 32 | helper yd << 42 |
// killer type << 50 (7, 3, 7, 15, 10, 8, 2 bits)
constexpr std::uint64_t DeathAmount(DeathRow const& row)
{
    return LevelField(row.killerLevelDiff) | Capped(static_cast<std::uint64_t>(row.killerRank), 7) << 7 |
           Capped(row.enemies, COUNT_MAX) << 10 | Capped(row.roseSeconds, ROSE_NONE) << 17 |
           Capped(row.roseYards, YARDS_NONE) << 32 | Capped(row.helperYards, HELPER_NONE) << 42 |
           Capped(row.killerType, KILLER_NONE) << 50;
}

// A helper of the bot against these enemies: an ally of its group (not the bot's own pet), a guard, or another ally
// that fights one of the enemies or a unit that fights the bot
inline bool Helps(Picture const& picture, Seen const& unit, std::uint64_t const* enemies, std::size_t count)
{
    if (unit.side != Side::Ally || (unit.type == UnitType::Pet && unit.ownerGuid == picture.self.guid))
        return false;
    if (unit.kind == Kind::Group || unit.kind == Kind::Guard)
        return true;
    if (!Fights(unit))
        return false;
    for (std::size_t i = 0; i < count; ++i)
        if (enemies[i] != 0 && unit.fightsGuid == enemies[i])
            return true;
    return unit.fightsIndex >= 0 && unit.fightsIndex < picture.count && Fights(picture.units[unit.fightsIndex]) &&
           picture.units[unit.fightsIndex].fightsIndex == THE_BOT;
}

struct SceneCounts
{
    std::uint32_t group = 0;
    std::uint32_t guards = 0;
    std::uint32_t allies = 0;
    std::uint32_t nearby = 0;
};

// The FightScene's counts around the target (the unit that entered first)
inline SceneCounts CountScene(Picture const& picture, std::uint64_t target)
{
    SceneCounts counts;
    std::int16_t const slot = SlotOf(picture, target);
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        if (Helps(picture, unit, &target, 1))
        {
            if (unit.kind == Kind::Group)
                ++counts.group;
            else if (unit.kind == Kind::Guard)
                ++counts.guards;
            else
                ++counts.allies;
        }
        // An evading creature would not attack now: its reach is the one it has at home (AW7)
        else if (unit.side == Side::Enemy && unit.guid != target && unit.busy != Busy::Dead &&
                 unit.busy != Busy::Evading &&
                 (Covers(unit, picture.self.x, picture.self.y, 0.0f) ||
                  (slot >= 0 && Covers(unit, picture.units[slot].x, picture.units[slot].y, 0.0f))))
            ++counts.nearby;
    }
    return counts;
}

struct Helper
{
    Kind kind = Kind::None;
    float yards = 0.0f;
    bool found = false;
};

// The nearest helper of the bot against these enemies (the DeathScene's)
inline Helper NearestHelper(Picture const& picture, std::uint64_t const* enemies, std::size_t count)
{
    Helper nearest;
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        if (!Helps(picture, unit, enemies, count))
            continue;
        float const yards = DistanceToDisc(unit, picture.self.x, picture.self.y);
        if (!nearest.found || yards < nearest.yards)
            nearest = Helper{unit.kind, yards, true};
    }
    return nearest;
}

// The DeathScene row's word: group, guard, ally (another ally that fights the enemies) or none
constexpr char const* Word(Helper const& helper)
{
    if (!helper.found)
        return "none";
    if (helper.kind == Kind::Group)
        return "group";
    return helper.kind == Kind::Guard ? "guard" : "ally";
}

// AW12: the strength of a unit and of a side, and `sides` (INTERFACE.md §3.8). A strength is kept in thousandths as a
// whole number, 1000 being one ordinary creature of the bot's level at full health; each unit's is rounded once and
// the sums are whole numbers, so no answer depends on the order of the units

// The one table of the rule: no other place holds a number of it, so a later fit changes the table and no logic
struct StrengthTable
{
    double player = 2.5;          // a player's base, so what a bot at full health is worth (AW.18)
    double levelStep = 1.2;       // times this for every level a unit stands above the bot, divided for every one below
    std::int16_t levelSpan = 20;  // the level difference is read between -levelSpan and +levelSpan
    std::array<double, static_cast<std::size_t>(Rank::Count)> rank{1.0, 2.0, 2.0, 20.0};  // a creature's base
    double pet = 0.0;                                                                     // a pet is part of its owner
    double healthExponent = 0.5;  // the health share enters by its square root
    // Tuning value: how near an ally of a kind standsBy names must stand to count before it fights; planar, to the
    // far side of its disc
    float helpYards = 30.0f;
    std::array<bool, static_cast<std::size_t>(Kind::Count)> standsBy{false, true, false, false, false};  // group only
    std::uint32_t takeable = 1000;  // the most a risen bot takes at a point
};
constexpr StrengthTable STRENGTH{};

// A unit's strength: its base, times levelStep for every level it stands above the bot (divided for every one
// below), times its health share to healthExponent; rounded once
inline std::uint32_t Strength(double base, int levelDiff, float healthPct)
{
    int const span = STRENGTH.levelSpan;
    double const share = std::clamp(static_cast<double>(healthPct), 0.0, 100.0) / 100.0;
    double const value = 1000.0 * base * std::pow(STRENGTH.levelStep, std::clamp(levelDiff, -span, span)) *
                         std::pow(share, STRENGTH.healthExponent);
    return static_cast<std::uint32_t>(std::lround(value));
}

// The same rule for every unit, whichever side it is on: a creature by its rank, a player, a pet; a dead unit is 0
inline std::uint32_t StrengthOf(Seen const& unit)
{
    if (unit.busy == Busy::Dead)
        return 0;
    double base = STRENGTH.player;
    if (unit.type == UnitType::Pet)
        base = STRENGTH.pet;
    else if (unit.type == UnitType::Creature)
        base = STRENGTH.rank[std::min(static_cast<std::size_t>(unit.rank), STRENGTH.rank.size() - 1)];
    return Strength(base, unit.levelDiff, unit.healthPct);
}

// The bot is a unit like the others: a player's base, level difference 0, its own health
inline std::uint32_t StrengthOf(Self const& self) { return Strength(STRENGTH.player, 0, self.healthPct); }

// The strength of a slot or THE_BOT; 0 for a ref that names no unit
inline std::uint32_t StrengthAt(Picture const& picture, std::int16_t ref)
{
    if (ref == THE_BOT)
        return StrengthOf(picture.self);
    return ref >= 0 && ref < picture.count ? StrengthOf(picture.units[ref]) : 0;
}

// A side's strength: the sum of its units'
inline std::uint32_t StrengthOf(Picture const& picture, UnitList const& list)
{
    std::uint32_t sum = 0;
    for (std::uint8_t i = 0; i < list.size; ++i)
        sum += StrengthAt(picture, list.refs[i]);
    return sum;
}

// It stands by this place (§3.8 `target` step 3 and the point's support): a living ally of a kind standsBy names
// within helpYards, planar and to the far side of its disc, so an ally known less exactly never counts sooner
inline bool StandsBy(Seen const& unit, float x, float y)
{
    std::size_t const kind = static_cast<std::size_t>(unit.kind);
    return unit.side == Side::Ally && kind < STRENGTH.standsBy.size() && STRENGTH.standsBy[kind] &&
           unit.busy != Busy::Dead && PlanarDistance(unit.x, unit.y, x, y) + unit.estimateRadius <= STRENGTH.helpYards;
}

// The fight as it stands around a unit; strengths in thousandths
struct SidesAnswer
{
    bool holds = true;
    std::uint32_t ours = 0;    // the unit's strength and that of every unit beside it
    std::uint32_t theirs = 0;  // that of every unit against it
    UnitList helpers;          // the units beside it
    UnitList enemies;          // the units against it
};

// `sides` (§3.8): around a slot or THE_BOT, from the links alone (SidesAround). Whoever fights one of the unit's
// opponents is beside it, whatever its colour and distance, and nobody else is; a unit that only stands near is not
// in the fight. A unit in no fight holds with theirs 0. Reads nothing learned
inline SidesAnswer JudgeSides(Picture const& picture, std::int16_t unit)
{
    Sides const sides = SidesAround(picture, unit);
    SidesAnswer answer;
    answer.helpers = sides.beside;
    answer.enemies = sides.against;
    answer.ours = StrengthAt(picture, unit) + StrengthOf(picture, sides.beside);
    answer.theirs = StrengthOf(picture, sides.against);
    answer.holds = answer.ours >= answer.theirs;
    return answer;
}

constexpr char const* Word(SidesAnswer const& answer) { return answer.holds ? "holds" : "outweighed"; }

// AW7: the point judgement and the safe-point search (INTERFACE.md §3.2, §3.3). A judgement decides nothing; its
// caller chooses

// The picture's radius, planar: the fill's sight range, so a rule can say what the picture cannot vouch for
constexpr float PICTURE_YARDS = 100.0f;
// Tuning values (§3.2): what a walking creature covers in the picture's 1000 ms, rounded up; the time a risen bot
// needs to be on its feet; the reach of a creature the picture does not hold (5 levels above, detection range 20)
constexpr float POINT_MARGIN_YARDS = 3.0f;
constexpr float PATH_AHEAD_SECONDS = 10.0f;
constexpr float UNSEEN_REACH_YARDS = 25.0f;
// The clearance when no unit counts
constexpr float CLEARANCE_NONE = 1000.0f;
// Tuning values (§3.3): the rings' step, the room that makes every clear point equally good, the walkable tests of
// one call, the ground's height against the centre's, and how much longer than the straight line a path may be
constexpr float RING_STEP_YARDS = 7.75f;
constexpr float COMFORT_YARDS = 10.0f;
constexpr std::uint32_t WALK_TESTS_MAX = 8;
constexpr float WALK_Z_YARDS = 15.0f;
constexpr float WALK_DETOUR = 2.0f;
constexpr float WALK_DETOUR_YARDS = 5.0f;
// The most candidates one search judges: the centre and 6 rings, a radius up to 46.5 yd
constexpr std::size_t CANDIDATES_MAX = 127;

// Who reaches a point, and how far outside every reach it lies
struct PointAnswer
{
    bool clear = true;                 // no counting unit reaches it
    std::uint32_t reaching = 0;        // how many reach it
    std::uint64_t nearest = 0;         // the reaching unit with the smallest over, 0 for none
    float clearance = CLEARANCE_NONE;  // the smallest over of all counting units; negative inside a reach
    std::int16_t topLevelDiff = 0;     // the highest level difference among the reaching units
    Rank topRank = Rank::Normal;       // and the highest rank
    bool inView = false;               // the picture can vouch for the point
    std::uint32_t weight = 0;          // AW12: the strength of the units that reach it
    std::uint32_t support = 0;         // AW12: the strength of the allies that stand by it (StandsBy)
};

inline bool operator==(PointAnswer const& a, PointAnswer const& b)
{
    return a.clear == b.clear && a.reaching == b.reaching && a.nearest == b.nearest && a.clearance == b.clearance &&
           a.topLevelDiff == b.topLevelDiff && a.topRank == b.topRank && a.inView == b.inView && a.weight == b.weight &&
           a.support == b.support;
}

// It counts for a point: a living hostile creature that would attack this bot on sight, whatever it is busy with
constexpr bool CountsForPoint(Seen const& unit)
{
    return unit.side == Side::Enemy && unit.type == UnitType::Creature && unit.reachYards > 0.0f &&
           unit.busy != Busy::Dead;
}

// The planar distance from a unit's ground to a point: an evading creature's home, the stretch a creature on a path
// walks in PATH_AHEAD_SECONDS, every other unit's place
inline float GroundDistance(Seen const& unit, float x, float y)
{
    if (unit.busy == Busy::Evading)
        return PlanarDistance(unit.homeX, unit.homeY, x, y);
    if (unit.busy != Busy::OnPath || (unit.moveX == 0.0f && unit.moveY == 0.0f))
        return PlanarDistance(unit.x, unit.y, x, y);
    double const dx = double(unit.moveX) * PATH_AHEAD_SECONDS;
    double const dy = double(unit.moveY) * PATH_AHEAD_SECONDS;
    double const along = ((double(x) - unit.x) * dx + (double(y) - unit.y) * dy) / (dx * dx + dy * dy);
    // A point behind it measures from its place, exactly as for a unit that stands
    if (along <= 0.0)
        return PlanarDistance(unit.x, unit.y, x, y);
    double const t = std::min(along, 1.0);
    return static_cast<float>(std::hypot(unit.x + t * dx - x, unit.y + t * dy - y));
}

// How many yards the point lies outside the unit's reach, measured against its estimate's disc (negative inside)
inline float OverOf(Seen const& unit, float x, float y)
{
    return std::max(0.0f, GroundDistance(unit, x, y) - unit.estimateRadius) - unit.reachYards;
}

// Who reaches this point (§3.2): every counting unit whose over is at most POINT_MARGIN_YARDS. In view when the
// bot's distance to the point plus the larger of UNSEEN_REACH_YARDS and the largest counting reach plus the margin
// stays within what the picture covers: PICTURE_YARDS, or while enemies were left out the farthest kept enemy.
// Its strengths (§3.8): `weight` of the units that reach it, `support` of the allies that stand by it
inline PointAnswer JudgePoint(Picture const& picture, float x, float y)
{
    PointAnswer answer;
    float nearestOver = 0.0f;
    float unseenReach = UNSEEN_REACH_YARDS;
    float farthestEnemy = 0.0f;
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        if (unit.side == Side::Enemy)
            farthestEnemy = std::max(farthestEnemy, DistanceToDisc(unit, picture.self.x, picture.self.y));
        if (StandsBy(unit, x, y))
            answer.support += StrengthOf(unit);
        if (!CountsForPoint(unit))
            continue;
        unseenReach = std::max(unseenReach, unit.reachYards);
        float const over = OverOf(unit, x, y);
        answer.clearance = std::min(answer.clearance, over);
        if (over > POINT_MARGIN_YARDS)
            continue;
        if (!answer.reaching || over < nearestOver || (over == nearestOver && unit.guid < answer.nearest))
        {
            nearestOver = over;
            answer.nearest = unit.guid;
        }
        answer.topLevelDiff = answer.reaching ? std::max(answer.topLevelDiff, unit.levelDiff) : unit.levelDiff;
        answer.topRank = answer.reaching ? std::max(answer.topRank, unit.rank) : unit.rank;
        answer.weight += StrengthOf(unit);
        ++answer.reaching;
    }
    answer.clear = answer.reaching == 0;
    // The enemies left out are no nearer than the farthest kept one (Admit keeps each side's nearest)
    float const viewYards = picture.leftOut[static_cast<std::size_t>(Side::Enemy)] > 0
                                ? std::min(PICTURE_YARDS, farthestEnemy)
                                : PICTURE_YARDS;
    answer.inView =
        PlanarDistance(picture.self.x, picture.self.y, x, y) + unseenReach + POINT_MARGIN_YARDS <= viewYards;
    return answer;
}

// In view, at least one unit reaches the point and their weight is at most what a risen bot takes (§3.8): one
// ordinary creature of the bot's level or below, two five levels below, never an elite of its level
constexpr bool Takeable(PointAnswer const& answer)
{
    return answer.inView && answer.reaching > 0 && answer.weight <= STRENGTH.takeable;
}

// Why `target` leaves a target
enum class TargetReason : std::uint8_t
{
    None,     // it takes it
    Strong,   // the target alone is stronger than the bot's side
    Many,     // the fight with those that join is
    Unknown,  // the target is not a living unit of the picture
    Count
};

constexpr char const* Word(TargetReason reason)
{
    switch (reason)
    {
        case TargetReason::None:
            return "none";
        case TargetReason::Strong:
            return "strong";
        case TargetReason::Many:
            return "many";
        case TargetReason::Unknown:
            return "unknown";
        default:
            return "?";
    }
}

// The fight as it would stand; strengths in thousandths
struct TargetAnswer
{
    bool take = false;
    TargetReason reason = TargetReason::Unknown;
    std::uint32_t ours = 0;
    std::uint32_t weight = 0;  // the fight's strength: `theirs` of that picture
    UnitList joins;            // the units given the bot in step 2, slots of the picture asked
};

constexpr char const* Word(TargetAnswer const& answer) { return answer.take ? "take" : "leave"; }

// AW14: a caller's row about a candidate it leaves comes at most once per candidate and this many seconds
constexpr std::int64_t TARGET_ROW_SECONDS = 60;

// Whether that row is due: never written (last 0) or written at least TARGET_ROW_SECONDS before now (seconds)
constexpr bool RowDue(std::int64_t lastWrite, std::int64_t now)
{
    return lastWrite == 0 || now - lastWrite >= TARGET_ROW_SECONDS;
}

// `target` step 2 by reach: a unit that counts for a point (§3.2) whose reach plus POINT_MARGIN_YARDS covers this
// place, from its ground (an evading creature from its home, a creature on a path along its next stretch), measured
// as the engine measures a reach: straight in three dimensions, the planar part against the disc of its estimate
inline bool ReachesInSpace(Seen const& unit, float x, float y, float z)
{
    if (!CountsForPoint(unit))
        return false;
    float const planar = std::max(0.0f, GroundDistance(unit, x, y) - unit.estimateRadius);
    float const groundZ = unit.busy == Busy::Evading ? unit.homeZ : unit.z;
    return std::hypot(planar, groundZ - z) - unit.reachYards <= POINT_MARGIN_YARDS;
}

// `target` steps 1 to 3 (§3.8): the picture as it would be if `self` (THE_BOT or a slot) attacked `target`, a living
// unit of the picture other than `self`. Slots stay as they are; `joined` receives the units given `self` in step 2.
// 1. `self` fights the target; the target, when it fights nobody, fights `self`.
// 2. Unless the target is a player or a pet (what comes to a player's help is not read), each unit that fights nobody
//    is given `self`, once however often it is named: the units of `joins` (the engine's call at the pull,
//    Awareness::JoinersOf) and every unit whose reach covers the target's place or `self`'s (ReachesInSpace).
// 3. Each ally that stands by `self` (StandsBy) and fights nobody is given the target
inline Picture AsTaken(Picture const& picture, std::int16_t self, std::int16_t target, UnitList const& joins,
                       UnitList& joined)
{
    Picture taken = picture;
    bool const bot = self == THE_BOT;
    Seen const& aim = picture.units[target];
    std::uint64_t const selfGuid = bot ? picture.self.guid : picture.units[self].guid;
    float const selfX = bot ? picture.self.x : picture.units[self].x;
    float const selfY = bot ? picture.self.y : picture.units[self].y;
    float const selfZ = bot ? picture.self.z : picture.units[self].z;
    auto setFight = [&taken](std::size_t slot, std::uint64_t guid, std::int16_t ref)
    {
        taken.units[slot].busy = Busy::Fighting;
        taken.units[slot].fightsGuid = guid;
        taken.units[slot].fightsIndex = ref;
    };
    std::array<bool, PICTURE_SIZE> given{};
    given[static_cast<std::size_t>(target)] = true;
    if (bot)
    {
        taken.self.fightsGuid = aim.guid;
        taken.self.fightsIndex = target;
    }
    else
    {
        given[static_cast<std::size_t>(self)] = true;
        setFight(static_cast<std::size_t>(self), aim.guid, target);
    }
    if (!Fights(aim))
        setFight(static_cast<std::size_t>(target), selfGuid, self);
    if (aim.type == UnitType::Creature)
    {
        auto join = [&](std::size_t slot)
        {
            Seen const& unit = picture.units[slot];
            if (given[slot] || unit.busy == Busy::Dead || Fights(unit))
                return;
            given[slot] = true;
            setFight(slot, selfGuid, self);
            joined.refs[joined.size++] = static_cast<std::int16_t>(slot);
        };
        for (std::uint8_t i = 0; i < joins.size; ++i)
            if (joins.refs[i] >= 0 && joins.refs[i] < picture.count)
                join(static_cast<std::size_t>(joins.refs[i]));
        for (std::size_t i = 0; i < picture.count; ++i)
        {
            Seen const& unit = picture.units[i];
            if (ReachesInSpace(unit, aim.x, aim.y, aim.z) || ReachesInSpace(unit, selfX, selfY, selfZ))
                join(i);
        }
    }
    for (std::size_t i = 0; i < picture.count; ++i)
    {
        Seen const& unit = picture.units[i];
        if (!given[i] && !Fights(unit) && StandsBy(unit, selfX, selfY))
        {
            given[i] = true;
            setFight(i, aim.guid, target);
        }
    }
    return taken;
}

// `target` (§3.8): `sides` around `self` on the picture as it would be (AsTaken); there is no second way of weighing.
// `reason` is Strong when the target's own strength is above `ours`, else Many; Unknown (leave) when the target is
// not a living unit of the picture other than `self`. `joins` are slots of this picture
inline TargetAnswer JudgeTarget(Picture const& picture, std::int16_t self, std::int16_t target, UnitList const& joins)
{
    TargetAnswer answer;
    bool const selfLiving =
        self == THE_BOT || (self >= 0 && self < picture.count && picture.units[self].busy != Busy::Dead);
    if (!selfLiving || target < 0 || target >= picture.count || target == self ||
        picture.units[target].busy == Busy::Dead)
        return answer;
    SidesAnswer const sides = JudgeSides(AsTaken(picture, self, target, joins, answer.joins), self);
    answer.take = sides.holds;
    answer.ours = sides.ours;
    answer.weight = sides.theirs;
    if (!answer.take)
        answer.reason = StrengthAt(picture, target) > sides.ours ? TargetReason::Strong : TargetReason::Many;
    else
        answer.reason = TargetReason::None;
    return answer;
}

// A candidate of the search
struct Spot
{
    float x = 0.0f;
    float y = 0.0f;
};

// The search's candidates around a centre (§3.3): the centre, then rings every RING_STEP_YARDS out to the radius, ring
// k holding 6k points at even angles from angle 0; a ring that does not fit into `max` is left out whole
inline std::size_t RingSpots(float centreX, float centreY, float radius, Spot* out, std::size_t max)
{
    if (max == 0)
        return 0;
    std::size_t count = 0;
    out[count++] = Spot{centreX, centreY};
    for (int k = 1; static_cast<float>(k) * RING_STEP_YARDS <= radius; ++k)
    {
        std::size_t const points = static_cast<std::size_t>(6 * k);
        if (count + points > max)
            break;
        float const yards = static_cast<float>(k) * RING_STEP_YARDS;
        for (std::size_t j = 0; j < points; ++j)
        {
            double const angle = 6.283185307179586 * static_cast<double>(j) / static_cast<double>(points);
            out[count++] = Spot{centreX + static_cast<float>(yards * std::cos(angle)),
                                centreY + static_cast<float>(yards * std::sin(angle))};
        }
    }
    return count;
}

// A candidate's walkable test: Unknown when it was not tested and no test was allowed
enum class Walk : std::uint8_t
{
    Unknown,
    Yes,
    No
};

// The answer of the caller's walkable test: whether a new test was made for it, and the ground's height there
struct WalkAnswer
{
    Walk walk = Walk::Unknown;
    bool tested = false;
    float z = 0.0f;
};

// Where within a disc nobody reaches
struct SafePoint
{
    bool found = false;  // a walkable candidate in view
    bool clear = false;  // and it is clear
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;           // from the walkable test
    std::int16_t index = -1;  // the winner's candidate index
    PointAnswer answer;
    std::uint32_t unseen = 0;  // candidates not in view
    std::uint32_t tests = 0;   // new walkable tests this call made
    bool outOfTests = false;   // the call stopped at a candidate it could not test; the next call goes on
};

// The search's order (§3.3): a clear candidate by the larger clearance capped at COMFORT_YARDS, else the least
// reached (fewer reaching, lower top level, larger clearance); then the nearer to `from`, then the lower index
inline bool Safer(PointAnswer const& a, float aYards, std::size_t aIndex, PointAnswer const& b, float bYards,
                  std::size_t bIndex)
{
    if (a.clear != b.clear)
        return a.clear;
    if (a.clear)
    {
        float const aRoom = std::min(a.clearance, COMFORT_YARDS);
        float const bRoom = std::min(b.clearance, COMFORT_YARDS);
        if (aRoom != bRoom)
            return aRoom > bRoom;
    }
    else
    {
        if (a.reaching != b.reaching)
            return a.reaching < b.reaching;
        if (a.topLevelDiff != b.topLevelDiff)
            return a.topLevelDiff < b.topLevelDiff;
        if (a.clearance != b.clearance)
            return a.clearance > b.clearance;
    }
    if (aYards != bYards)
        return aYards < bYards;
    return aIndex < bIndex;
}

// The safe-point search (§3.3) over the given candidates (at most CANDIDATES_MAX): each judged by JudgePoint, one
// not in view never returned; `walkable(index, mayTest)` asked lazily in winning order until one passes, a new test
// allowed while fewer than `testsMax` were made; the first candidate it cannot answer ends the call (outOfTests)
template <typename Walkable>
SafePoint FindSafePoint(Picture const& picture, float fromX, float fromY, Spot const* candidates, std::size_t count,
                        Walkable&& walkable, std::uint32_t testsMax = WALK_TESTS_MAX)
{
    SafePoint safe;
    count = std::min(count, CANDIDATES_MAX);
    std::array<PointAnswer, CANDIDATES_MAX> answers{};
    std::array<float, CANDIDATES_MAX> yards{};
    std::array<std::uint8_t, CANDIDATES_MAX> order{};
    std::size_t inView = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        answers[i] = JudgePoint(picture, candidates[i].x, candidates[i].y);
        if (!answers[i].inView)
        {
            ++safe.unseen;
            continue;
        }
        yards[i] = PlanarDistance(fromX, fromY, candidates[i].x, candidates[i].y);
        order[inView++] = static_cast<std::uint8_t>(i);
    }
    std::sort(order.begin(), order.begin() + inView, [&answers, &yards](std::uint8_t a, std::uint8_t b)
              { return Safer(answers[a], yards[a], a, answers[b], yards[b], b); });
    for (std::size_t k = 0; k < inView; ++k)
    {
        std::size_t const i = order[k];
        WalkAnswer const walk = walkable(i, safe.tests < testsMax);
        if (walk.tested)
            ++safe.tests;
        if (walk.walk == Walk::Unknown)
        {
            safe.outOfTests = true;
            break;
        }
        if (walk.walk == Walk::No)
            continue;
        safe.found = true;
        safe.clear = answers[i].clear;
        safe.x = candidates[i].x;
        safe.y = candidates[i].y;
        safe.z = walk.z;
        safe.index = static_cast<std::int16_t>(i);
        safe.answer = answers[i];
        break;
    }
    return safe;
}

// AW8: the rule of the dead (INTERFACE.md §3.4). How far from the corpse a leader counts (the core's reclaim radius
// of 39 yd less 5; less ARRIVE_YARDS the search's radius); tuning values: where a ghost starts to judge, when it
// stands at a point, the wait without a clear point, the bound of one return, the death in a row that takes the healer.
// AW30b: a search for the spirit healer that finds none counts as the grave's (graveWithoutHealer) only within
// GRAVE_SEARCH_YARDS of its point: the search is made around the ghost, which sees the grave from 100 yd off
constexpr float RISE_YARDS = 34.0f;
constexpr float GHOST_LOOK_YARDS = 60.0f;
constexpr float ARRIVE_YARDS = 3.0f;
constexpr float GRAVE_SEARCH_YARDS = 20.0f;
constexpr std::uint32_t UNCLEAR_WAIT_SECONDS = 30;
constexpr std::uint32_t GHOST_BOUND_SECONDS = 600;
constexpr std::uint32_t ROW_HEALER_AT = 3;

// World PvP's answer for a corpse an enemy player stands at (a fact here, AW.12)
enum class PlayerHold : std::uint8_t
{
    None,
    Wait,
    Healer
};

// What the rule knows of a dead bot (INTERFACE.md §3.4, the table of facts)
struct DeadFacts
{
    bool inBattleground = false;
    bool released = true;            // a ghost, not a body
    std::uint32_t ghostSeconds = 0;  // the smaller of the seconds since the death and since the login
    bool corpseHere = true;          // a corpse on the ghost's map
    float corpseYards = 0.0f;
    bool reclaimHere = false;       // the core's distance test of a reclaim passes here
    std::uint32_t deathsInRow = 1;  // 1 for a first death
    bool ownedByPlayer = false;     // the bot's master is a real player
    PlayerHold playerHold = PlayerHold::None;
    bool leaderAtCorpse = false;       // the living leader stands within RISE_YARDS of the corpse
    bool atLeader = false;             // within spell distance of it
    bool leaderDown = false;           // the real player leading the bot lies dead near it
    PointAnswer own;                   // the ghost's own place; filled within GHOST_LOOK_YARDS of the corpse
    SafePoint best;                    // the search around the corpse; filled within GHOST_LOOK_YARDS
    bool atBest = false;               // within ARRIVE_YARDS of best
    std::uint32_t unclearSeconds = 0;  // since the first look without a clear point, 0 for none
    bool corpseWalkFailed = false;     // the walk to the corpse or the point could not start this tick
    bool healerChosen = false;         // this death already took the healer's way
    bool graveKnown = true;
    bool atGrave = false;
    bool healerInRange = false;
    bool graveWithoutHealer = false;  // this death's ghost stood at its grave and a search there found no healer
    bool graveWalkFailed = false;
    bool teleportSeen = false;  // a real player would see a teleport to the grave
};

// The steps: none; the corpse's way; the healer's way; the bound's
enum class DeadAct : std::uint8_t
{
    None,
    WalkCorpse,
    WalkPoint,
    Wait,
    Rise,
    WalkGrave,
    WaitGrave,
    HealerRevive,
    TeleportGrave,
    ReviveGrave,
    ReviveHere,
    Count
};

enum class DeadReason : std::uint8_t
{
    None,
    Clear,
    ReachedOne,
    Leader,
    Player,
    LeaderDown,
    NoClearPoint,
    CorpseAway,
    CorpseRunsExhausted,
    CorpseCamped,
    NoPath,
    NoGrave,
    NoHealer,
    Seen,
    WaitEnded,
    Count
};

struct DeadStep
{
    DeadAct act = DeadAct::None;
    DeadReason reason = DeadReason::None;
};

constexpr bool operator==(DeadStep const& a, DeadStep const& b) { return a.act == b.act && a.reason == b.reason; }

constexpr char const* Word(DeadAct act)
{
    switch (act)
    {
        case DeadAct::None:
            return "none";
        case DeadAct::WalkCorpse:
            return "walk_corpse";
        case DeadAct::WalkPoint:
            return "walk_point";
        case DeadAct::Wait:
            return "wait";
        case DeadAct::Rise:
            return "rise";
        case DeadAct::WalkGrave:
            return "walk_grave";
        case DeadAct::WaitGrave:
            return "wait_grave";
        case DeadAct::HealerRevive:
            return "healer_revive";
        case DeadAct::TeleportGrave:
            return "teleport_grave";
        case DeadAct::ReviveGrave:
            return "revive_grave";
        case DeadAct::ReviveHere:
            return "revive_here";
        default:
            return "?";
    }
}

constexpr char const* Word(DeadReason reason)
{
    switch (reason)
    {
        case DeadReason::None:
            return "none";
        case DeadReason::Clear:
            return "clear";
        case DeadReason::ReachedOne:
            return "reached_one";
        case DeadReason::Leader:
            return "leader";
        case DeadReason::Player:
            return "player";
        case DeadReason::LeaderDown:
            return "leader_down";
        case DeadReason::NoClearPoint:
            return "no_clear_point";
        case DeadReason::CorpseAway:
            return "corpse_away";
        case DeadReason::CorpseRunsExhausted:
            return "corpse_runs_exhausted";
        case DeadReason::CorpseCamped:
            return "corpse_camped";
        case DeadReason::NoPath:
            return "no_path";
        case DeadReason::NoGrave:
            return "no_grave";
        case DeadReason::NoHealer:
            return "no_healer";
        case DeadReason::Seen:
            return "seen";
        case DeadReason::WaitEnded:
            return "wait_ended";
        default:
            return "?";
    }
}

// A step of the corpse's way; of the healer's way (a step of the bound has the reason wait_ended); a revive
constexpr bool CorpseWay(DeadAct act)
{
    return act == DeadAct::WalkCorpse || act == DeadAct::WalkPoint || act == DeadAct::Wait || act == DeadAct::Rise;
}

constexpr bool HealerWay(DeadAct act)
{
    return act == DeadAct::WalkGrave || act == DeadAct::WaitGrave || act == DeadAct::HealerRevive ||
           act == DeadAct::TeleportGrave;
}

constexpr bool Revives(DeadAct act)
{
    return act == DeadAct::HealerRevive || act == DeadAct::ReviveGrave || act == DeadAct::ReviveHere;
}

// The step that sets the memo's healerChosen (rule line 3): a step of the healer's way, not of the bound
constexpr bool ChoosesHealer(DeadStep const& step)
{
    return HealerWay(step.act) && step.reason != DeadReason::WaitEnded;
}

// The healer's way (rule line 3): wait without a grave; at the grave the healer's revive or a wait for one; else the
// walk there, the teleport when the walk cannot start, a wait while a player would see the teleport
inline DeadStep HealerWayStep(DeadFacts const& facts, DeadReason why)
{
    if (!facts.graveKnown)
        return {DeadAct::WaitGrave, DeadReason::NoGrave};
    if (facts.atGrave)
        return facts.healerInRange ? DeadStep{DeadAct::HealerRevive, why}
                                   : DeadStep{DeadAct::WaitGrave, DeadReason::NoHealer};
    if (!facts.graveWalkFailed)
        return {DeadAct::WalkGrave, why};
    if (!facts.teleportSeen)
        return {DeadAct::TeleportGrave, why};
    return {DeadAct::WaitGrave, DeadReason::Seen};
}

// The bound (rule line 2): a revive here without a grave, at the grave the healer's or the grave's, else the teleport
// there, seen or not
inline DeadStep BoundStep(DeadFacts const& facts)
{
    if (!facts.graveKnown)
        return {DeadAct::ReviveHere, DeadReason::WaitEnded};
    if (facts.atGrave)
        return {facts.healerInRange ? DeadAct::HealerRevive : DeadAct::ReviveGrave, DeadReason::WaitEnded};
    return {DeadAct::TeleportGrave, DeadReason::WaitEnded};
}

// A Rise of lines 5, 7 and 8, held while the real player who leads the bot lies dead near it
inline DeadStep RiseUnlessLeaderDown(DeadFacts const& facts, DeadReason why)
{
    return facts.leaderDown ? DeadStep{DeadAct::Wait, DeadReason::LeaderDown} : DeadStep{DeadAct::Rise, why};
}

// What a dead bot does next (INTERFACE.md §3.4): the first line that matches
inline DeadStep NextOfDead(DeadFacts const& facts)
{
    // 1. Not the rule's: a battleground, a body, a player's ghost whose corpse is on another map
    if (facts.inBattleground || !facts.released || (facts.ownedByPlayer && !facts.corpseHere))
        return {};
    // 2. The bound
    if (facts.ghostSeconds >= GHOST_BOUND_SECONDS)
        return BoundStep(facts);
    // 3. The healer's way, kept once chosen. Closed for this death when its grave had no healer and the corpse lies
    // here (AW30b): an enemy player's hold that asks for the healer waits as line 4 does, a walk that cannot start
    // waits at the grave, and the corpse's way follows
    if (facts.graveWithoutHealer && facts.corpseHere)
    {
        if (facts.playerHold == PlayerHold::Healer)
            return {DeadAct::Wait, DeadReason::Player};
        if (facts.corpseWalkFailed)
            return {DeadAct::WaitGrave, DeadReason::NoHealer};
    }
    else
    {
        DeadReason healer = DeadReason::None;
        if (!facts.corpseHere)
            healer = DeadReason::CorpseAway;
        else if (facts.deathsInRow >= ROW_HEALER_AT && !facts.ownedByPlayer)
            healer = DeadReason::CorpseRunsExhausted;
        else if (facts.playerHold == PlayerHold::Healer)
            healer = DeadReason::CorpseCamped;
        else if (facts.corpseWalkFailed)
            healer = DeadReason::NoPath;
        if (facts.healerChosen || healer != DeadReason::None)
            return HealerWayStep(facts, healer);
    }
    // 4. An enemy player at the corpse
    if (facts.playerHold == PlayerHold::Wait)
        return {DeadAct::Wait, DeadReason::Player};
    // 5. The leader at the corpse: the group is its support
    if (facts.leaderAtCorpse)
        return facts.atLeader && facts.reclaimHere ? RiseUnlessLeaderDown(facts, DeadReason::Leader)
                                                   : DeadStep{DeadAct::WalkPoint, DeadReason::Leader};
    // 6. Too far to see what stands at the corpse
    if (facts.corpseYards > GHOST_LOOK_YARDS)
        return {DeadAct::WalkCorpse, DeadReason::None};
    // 7. A clear point: rise at the first place with room to spare on the way, at a place without only at the best
    if (facts.best.found && facts.best.clear)
    {
        bool const rises = facts.reclaimHere && facts.own.clear && facts.own.inView &&
                           (facts.own.clearance >= COMFORT_YARDS || facts.atBest);
        return rises ? RiseUnlessLeaderDown(facts, DeadReason::Clear) : DeadStep{DeadAct::WalkPoint, DeadReason::Clear};
    }
    // 8. No clear point: look from the corpse, wait at the best, then beside one ordinary creature or the healer (a
    // closed healer's way: wait there and look again; the corpse lies here past line 3)
    if (facts.best.unseen > 0 && facts.corpseYards > ARRIVE_YARDS)
        return {DeadAct::WalkCorpse, DeadReason::NoClearPoint};
    if (facts.unclearSeconds < UNCLEAR_WAIT_SECONDS)
        return facts.best.found && !facts.atBest ? DeadStep{DeadAct::WalkPoint, DeadReason::NoClearPoint}
                                                 : DeadStep{DeadAct::Wait, DeadReason::NoClearPoint};
    if (facts.best.found && Takeable(facts.best.answer))
    {
        bool const ownTaken = (facts.own.clear && facts.own.inView) || Takeable(facts.own);
        return facts.atBest && facts.reclaimHere && ownTaken ? RiseUnlessLeaderDown(facts, DeadReason::ReachedOne)
                                                             : DeadStep{DeadAct::WalkPoint, DeadReason::ReachedOne};
    }
    if (facts.ownedByPlayer)
        return {DeadAct::Wait, DeadReason::NoClearPoint};
    if (facts.graveWithoutHealer)
        return {DeadAct::Wait, DeadReason::NoHealer};
    return HealerWayStep(facts, DeadReason::NoClearPoint);
}

// A rise on record: the seconds as a ghost, how many reach the place, the yards from the corpse, the death's number
// in a row
struct RiseRow
{
    std::uint32_t ghostSeconds = 0;
    std::uint32_t reaching = 0;
    std::uint32_t corpseYards = YARDS_NONE;
    std::uint32_t deathsInRow = 1;
};

// Rise amount: ghost s | reaching << 15 | corpse yd << 22 | death in a row << 32 (15, 7, 10, 7 bits)
constexpr std::uint64_t RiseAmount(RiseRow const& row)
{
    return Capped(row.ghostSeconds, ROSE_NONE) | Capped(row.reaching, COUNT_MAX) << 15 |
           Capped(row.corpseYards, YARDS_NONE) << 22 | Capped(row.deathsInRow, COUNT_MAX) << 32;
}

// The Rise row's word: for a rise at the corpse the reason of the rule's Rise (clear, reached_one, leader), for one by
// the spirit healer `healer` or `bound` (a step of the bound); `other` for a rise the rule did not order (a spell, a
// step older than the caller's limit, a rise at the corpse after a healer's step)
constexpr char const* RiseWord(DeadStep const& last, bool ordered, bool atCorpse)
{
    if (!ordered)
        return "other";
    if (last.act == DeadAct::Rise)
        return atCorpse ? Word(last.reason) : "other";
    if (atCorpse || (!HealerWay(last.act) && !Revives(last.act)))
        return "other";
    return last.reason == DeadReason::WaitEnded ? "bound" : "healer";
}

// AW9: the cause a step of the healer's way or of the bound names (the `death` rows and the healer's revive): the
// bound's own word, else the cause the memo kept when the healer's way was chosen, else the step's reason
constexpr DeadReason HealerCause(DeadStep const& step, DeadReason healerReason)
{
    if (step.reason == DeadReason::WaitEnded)
        return DeadReason::WaitEnded;
    return healerReason != DeadReason::None ? healerReason : step.reason;
}

// AW9, the return's memo (game side Awareness::NextOfDead). The death a return belongs to: the death edge's stamp,
// else the memo's own once it has one, else the corpse's ghost time, else 1. A corpse that comes onto the ghost's map
// does not change it, so the healer once chosen stays chosen (P4)
constexpr std::uint32_t DeathKey(std::uint32_t edge, std::uint32_t memo, std::uint32_t corpse)
{
    return edge ? edge : memo ? memo : corpse ? corpse : 1;
}

// The seconds as a ghost for the bound, before the test's aging: since the corpse's ghost time when the corpse lies on
// the ghost's map, else since the death edge, else since the memo's first ask of this death; never more than since
// the login. Negative inputs (a clock that went back) count as 0
struct GhostClock
{
    bool corpse = false;
    std::int64_t sinceCorpse = 0;
    bool edge = false;
    std::int64_t sinceEdge = 0;
    std::int64_t sinceMemo = 0;
    std::int64_t sinceLogin = 0;
};

constexpr std::int64_t GhostSecondsOf(GhostClock const& clock)
{
    std::int64_t const since = clock.corpse ? clock.sinceCorpse : clock.edge ? clock.sinceEdge : clock.sinceMemo;
    return std::max<std::int64_t>(0, std::min(since, clock.sinceLogin));
}

// The unclear clock: cleared by a look that finds a clear point, started (at now, never 0) by the first look without
// one; a search that stopped for its walkable tests before it found anything is no look and changes nothing
constexpr std::uint32_t UnclearStart(std::uint32_t startMs, SafePoint const& best, std::uint32_t nowMs)
{
    if (best.found && best.clear)
        return 0;
    if (!startMs && (best.found || !best.outOfTests))
        return nowMs ? nowMs : 1;
    return startMs;
}

// The ghost stands at the best point: one was found and it is within ARRIVE_YARDS of it
inline bool AtBest(SafePoint const& best, float x, float y)
{
    return best.found && PlanarDistance(x, y, best.x, best.y) <= ARRIVE_YARDS;
}

// A walk under way goes on while the rule names the same step and its point moved at most ARRIVE_YARDS; otherwise a
// new walk starts towards where the step points now (AW8's review, item 1)
constexpr bool WalkGoesOn(bool moving, DeadStep const& before, DeadStep const& now, float pointMovedYards)
{
    return moving && before == now && pointMovedYards <= ARRIVE_YARDS;
}

// AW30b, the memo's graveWithoutHealer (game side: the spirit healer action's search, Awareness::NextOfDeadAtGrave). A
// search for the spirit healer counts as the grave's when the ghost made it within GRAVE_SEARCH_YARDS of the grave's
// point (planar); the walk of WaitGrave (no_healer) ends within ARRIVE_YARDS of that point, inside it
constexpr bool GraveSearchCounts(float graveYards) { return graveYards <= GRAVE_SEARCH_YARDS; }

// The fact is set by a ghost at its grave whose search of this tick counted and found no living spirit healer, and
// it stays for this death (the memo is cleared at the rise and at a new death)
constexpr bool GraveWithoutHealer(bool already, bool atGrave, bool searched, bool healerInRange)
{
    return already || (atGrave && searched && !healerInRange);
}

// AW9: deaths in a row (INTERFACE.md §3.5). A death within ROW_SECONDS of the bot's last rise, on its map and within
// ROW_YARDS of the rise's place is in a row with the one before it; tuning values
constexpr std::uint32_t ROW_SECONDS = 300;
constexpr float ROW_YARDS = 40.0f;

// A death against the bot's last rise: whether it rose since the login, the seconds since that rise, the same map,
// the planar yards from its place
struct RowDeath
{
    bool rose = false;
    std::uint32_t sinceRiseSeconds = 0;
    bool sameMap = false;
    float riseYards = 0.0f;
};

// The count after this death: one more inside the three limits, else 1. Nothing else is input: a kill, experience, a
// repair or the spirit healer between two deaths changes nothing
constexpr std::uint32_t RowAfterDeath(std::uint32_t row, RowDeath const& death)
{
    bool const inRow =
        death.rose && death.sinceRiseSeconds < ROW_SECONDS && death.sameMap && death.riseYards <= ROW_YARDS;
    return inRow ? row + 1 : 1;
}

// The count a rise leaves: a rise follows a death, which counts 1 at least (a bot dead at login has no death edge)
constexpr std::uint32_t RowAtRise(std::uint32_t row) { return std::max<std::uint32_t>(row, 1); }

// AW9: after the rise (INTERFACE.md §3.6). Restored at these shares of health and of a mana user's mana; the bound of
// the recovery after the rise; tuning values
constexpr std::uint32_t RECOVER_HEALTH_PCT = 80;
constexpr std::uint32_t RECOVER_POWER_PCT = 80;
constexpr std::uint32_t RECOVER_BOUND_SECONDS = 60;

struct RecoverFacts
{
    std::uint32_t sinceRiseSeconds = 0;
    std::uint32_t healthPct = 100;
    std::uint32_t powerPct = 100;
    bool usesMana = false;
    bool sick = false;  // resurrection sickness
    PointAnswer own;    // the bot's own place
};

enum class RecoverAct : std::uint8_t
{
    Done,
    Move,
    Rest
};

constexpr bool Restored(RecoverFacts const& facts)
{
    return facts.healthPct >= RECOVER_HEALTH_PCT && (!facts.usesMana || facts.powerPct >= RECOVER_POWER_PCT);
}

// Recovering: sick, or within the bound of the rise and not restored. The trigger asks only this (no judgement)
constexpr bool Recovering(RecoverFacts const& facts)
{
    return facts.sick || (facts.sinceRiseSeconds < RECOVER_BOUND_SECONDS && !Restored(facts));
}

// A bot the recovery holds keeps on foot (the mount policy's on-foot fact): mounted it can neither eat nor drink, and
// the rise's speed change asks for a mount at once (AWL2: every risen bot with a mount rode the recovery's minute out
// unfed). A sick bot is not held (upkeep's rest), so the sickness alone keeps none on foot
constexpr bool KeepsOnFoot(RecoverFacts const& facts) { return !facts.sick && Recovering(facts); }

// What a risen bot does next: not recovering `Done`; recovering on a place someone reaches `Move` (to a clear point
// around it); else `Rest` (stop, eat and drink what it has, sit)
constexpr RecoverAct RecoverStep(RecoverFacts const& facts)
{
    if (!Recovering(facts))
        return RecoverAct::Done;
    return facts.own.clear ? RecoverAct::Rest : RecoverAct::Move;
}
}  // namespace AwarenessRules

#endif
