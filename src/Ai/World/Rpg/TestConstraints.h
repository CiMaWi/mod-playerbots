/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TESTCONSTRAINTS_H
#define _PLAYERBOT_TESTCONSTRAINTS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// A test's constraints on a bot (`playerbots route constraint|hold|release`, functional tests): named per-bot
// switches, all rows of one table. A choice row is read where the bot already decides; a state row is applied on the
// bot's map thread when it is set and undone when it is cleared (TravelOrders, NewRpgAction.cpp). Kept in
// NewRpgInfo::constraints, not saved. Orders are taken before the idle gate, so a held bot runs an order and holds
// again when it ends. Player-free for the selftest. Adding a constraint: a row, its gate or apply/undo pair, and a
// selftest line
namespace TestConstraints
{
constexpr std::uint16_t PLANNER = 1;       // planner goals and jobs
constexpr std::uint16_t STATUS = 2;        // the weighted random status pick
constexpr std::uint16_t ERRAND = 4;        // vendor errands and inn binds
constexpr std::uint16_t HEARTH = 8;        // hearth legs, the off-mesh hearth and the planner's inn goal
constexpr std::uint16_t NOFIGHT = 16;      // starts no fight; creatures ignore the bot and its pets
constexpr std::uint16_t NODAMAGE = 32;     // takes no damage
constexpr std::uint16_t FLYFAIL = 64;      // a fly leg fails in the air past its middle (the way down, upkeep WU26b)
constexpr std::uint16_t FLYABORT = 128;    // as flyfail, as the trip's last failure: the trip ends once the bot is down
constexpr std::uint16_t NODESTROY = 256;   // destroys no bag item for space (bank-mail WB7d)
constexpr std::uint16_t FIGHTREADY = 512;  // its chances start a fight when the hard rules pass (world PvP)
constexpr std::uint16_t JOINROLL = 1024;   // a bystander's join roll always wins (world PvP)
constexpr std::uint16_t FLEE = 2048;       // in PvP combat it flees from the enemy players attacking it (world PvP)
constexpr std::uint16_t FLY_FAILS = FLYFAIL | FLYABORT;

enum class Kind : std::uint8_t
{
    Choice,
    State
};

struct Row
{
    char const* name;
    Kind kind;
    std::uint16_t bit;
    char const* where;  // the gate that reads it, or what apply and undo do
};

constexpr Row ROWS[] = {
    {"planner", Kind::Choice, PLANNER, "planner hook and idle gate"},
    {"status", Kind::Choice, STATUS, "idle status pick"},
    {"errand", Kind::Choice, ERRAND, "errand triggers"},
    {"hearth", Kind::Choice, HEARTH, "trip and planner hearth, off-mesh hearth"},
    {"nofight", Kind::State, NOFIGHT, "immune to NPCs (bot and its pets), no grind or enemy player pick"},
    {"nodamage", Kind::State, NODAMAGE, "god mode (CHEAT_GOD)"},
    {"flyfail", Kind::State, FLYFAIL, "fly follower: the fly leg fails in the air past its middle, the trip replans"},
    {"flyabort", Kind::State, FLYABORT, "fly follower: as flyfail with no replan left, the trip ends once down"},
    {"nodestroy", Kind::State, NODESTROY, "smart destroy: no bag item destroyed for bag space or for loot"},
    {"fightready", Kind::State, FIGHTREADY, "a chance starts a fight when the hard rules pass (p 1, never peaceable)"},
    {"joinroll", Kind::State, JOINROLL, "a bystander's join roll always wins (defence, group, master unchanged)"},
    {"flee", Kind::State, FLEE, "retreat trigger: in PvP combat it flees from the enemy players attacking it"},
};

constexpr std::uint16_t BitsOf(Kind kind)
{
    std::uint16_t bits = 0;
    for (Row const& row : ROWS)
        if (row.kind == kind)
            bits |= row.bit;
    return bits;
}

constexpr std::uint16_t CHOICES = BitsOf(Kind::Choice);
constexpr std::uint16_t STATES = BitsOf(Kind::State);
constexpr std::uint16_t HOLD = PLANNER | STATUS | ERRAND | HEARTH;  // `route hold`: every choice row
static_assert(HOLD == CHOICES && !(CHOICES & STATES), "hold is the choice rows; a bit is one row's");

inline char const* KindName(Kind kind) { return kind == Kind::Choice ? "choice" : "state"; }

// Every row's name, for refusals
inline std::string Names()
{
    std::string out;
    for (Row const& row : ROWS)
        out += (out.empty() ? "" : ", ") + std::string(row.name);
    return out;
}

inline std::string Format(std::uint16_t set)
{
    std::string out;
    for (Row const& row : ROWS)
    {
        if (!(set & row.bit))
            continue;
        if (!out.empty())
            out += ',';
        out += row.name;
    }
    return out.empty() ? "none" : out;
}

struct Parsed
{
    std::uint16_t set{0};
    std::string bad;  // the first unknown name; set is then 0
};

// Names separated by commas or spaces
inline Parsed Parse(std::string_view list)
{
    Parsed parsed;
    while (!list.empty())
    {
        std::size_t const start = list.find_first_not_of(", ");
        if (start == std::string_view::npos)
            break;
        list.remove_prefix(start);
        std::string_view const word = list.substr(0, list.find_first_of(", "));
        std::uint16_t bit = 0;
        for (Row const& row : ROWS)
            if (word == row.name)
                bit = row.bit;
        if (!bit)
            return {0, std::string(word)};
        parsed.set |= bit;
        list.remove_prefix(word.size());
    }
    return parsed;
}

// The set after an order: its off bits cleared, its on bits set
inline std::uint16_t Next(std::uint16_t set, std::uint16_t on, std::uint16_t off)
{
    return static_cast<std::uint16_t>((set & ~off) | on);
}

// A choice row turning on ends the bot's current status (it stands idle and the idle gate keeps it there); a state
// row changing never does
inline bool EndsStatus(std::uint16_t before, std::uint16_t after) { return (after & ~before & CHOICES) != 0; }

// The errand row turning off frees the errand checks at once: the retry wait after the last check (ERRAND_RETRY_MS,
// ItemWorthRules.h) starts over from no check, so a check from before the hold does not delay the released bot's first
// errand by up to five minutes
inline bool FreesErrands(std::uint16_t before, std::uint16_t after) { return (before & ~after & ERRAND) != 0; }

// The gates. What an idle bot's status update may do on its own
struct IdleGate
{
    bool planner{true};  // planner job submits and goals (PlannerHook, OnIdle)
    bool status{true};   // the weighted status pick
};

inline IdleGate GateOf(std::uint16_t set) { return {!(set & PLANNER), !(set & STATUS)}; }
inline bool ErrandAllowed(std::uint16_t set) { return !(set & ERRAND); }
inline bool HearthAllowed(std::uint16_t set) { return !(set & HEARTH); }
// "attack anything" picks no mob the bot could not hit anyway (the engine refuses both directions), and the enemy
// player scan picks no player on sight (one already fighting the bot is still answered)
inline bool StartsFights(std::uint16_t set) { return !(set & NOFIGHT); }
// flyfail and flyabort, read by the fly leg's follower on a tick in the air (NewRpgAction.cpp; nothing to apply or
// undo): the leg fails as its timeout would, once the bot has flown half the leg's straight 2D length and is in the
// air, so the failure lies where a test chose the leg's middle (over land, over water)
inline bool FlyLegFails(std::uint16_t set, float flownYards, float legYards, bool inTheAir)
{
    return (set & FLY_FAILS) && inTheAir && flownYards * 2.0f >= legYards;
}
// flyabort: the failure finds the trip's replans spent (TravelLegs::AfterFailure, MAX_REPLANS), so the trip aborts and
// comes down first (AbortDescent)
inline bool FlyFailAborts(std::uint16_t set) { return (set & FLYABORT) != 0; }
// Read where the fight book decides (FightBook.cpp); their apply and undo do nothing
inline bool FightReady(std::uint16_t set) { return (set & FIGHTREADY) != 0; }
inline bool JoinRoll(std::uint16_t set) { return (set & JOINROLL) != 0; }
// flee, read by the world PvP retreat trigger (WorldPvpStrategy.cpp); its apply and undo do nothing
inline bool Flees(std::uint16_t set) { return (set & FLEE) != 0; }
// nodestroy, read by the smart destroy action (DestroyItemAction.cpp; nothing to apply or undo): a held bot keeps
// the full bags a test gives it, neither the bag-space destroy nor the destroy for a loot frees a slot
inline bool DestroysForSpace(std::uint16_t set) { return !(set & NODESTROY); }

// A state row's apply and undo on the bot (Target: Player on the server, a model in the selftest). Each is idempotent
template <class Target>
struct StateOps
{
    std::uint16_t bit;
    void (*apply)(Target&);
    void (*undo)(Target&);
};

template <class Target, std::size_t N>
constexpr std::uint16_t Covered(StateOps<Target> const (&ops)[N])
{
    std::uint16_t bits = 0;
    for (StateOps<Target> const& op : ops)
        bits |= op.bit;
    return bits;
}

// The state rows turned on are applied, those turned off undone
template <class Target, std::size_t N>
void ChangeStates(StateOps<Target> const (&ops)[N], Target& target, std::uint16_t before, std::uint16_t after)
{
    for (StateOps<Target> const& op : ops)
    {
        bool const was = (before & op.bit) != 0;
        bool const is = (after & op.bit) != 0;
        if (is && !was)
            op.apply(target);
        else if (was && !is)
            op.undo(target);
    }
}

// Every set state row applied again: a pet summoned after the constraint was set gets it too
template <class Target, std::size_t N>
void KeepStates(StateOps<Target> const (&ops)[N], Target& target, std::uint16_t set)
{
    for (StateOps<Target> const& op : ops)
        if (set & op.bit)
            op.apply(target);
}

// A pending order (TravelOrder): a constraint change (on, off), a placement aboard and/or a trip. A second post merges
// with the first, later wins per bit; the later trip or stop replaces an earlier one, as does a later aboard
template <typename Order>
bool HasTrip(Order const& order)
{
    return order.stop || !order.place.empty();
}

// The bot's AI update takes its order at once (TravelOrders::TakeAndApply); only an order that moves the bot (a trip,
// a stop, aboard) waits until the bot is out of combat, a constraint change never waits
template <typename Order>
bool TakeNow(Order const& order, bool inCombat)
{
    return !inCombat || (!HasTrip(order) && !order.aboard);
}

template <typename Order>
void Merge(Order& pending, Order&& incoming)
{
    pending.on = Next(pending.on, incoming.on, incoming.off);
    pending.off = Next(pending.off, incoming.off, incoming.on);
    if (HasTrip(incoming))
    {
        pending.dest = incoming.dest;
        pending.place = std::move(incoming.place);
        pending.stop = incoming.stop;
    }
    if (incoming.aboard)
        pending.aboard = incoming.aboard;
    pending.postedMs = incoming.postedMs;
}
}  // namespace TestConstraints

#endif
