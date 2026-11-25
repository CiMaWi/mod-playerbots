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
// bot's map thread when it is set and undone when it is cleared (TravelOrders, TestOrders.cpp). Kept in
// PlayerbotAI::testConstraints, not saved. Orders are taken before the idle gate, so a held bot runs an order and holds
// again when it ends. Player-free for the selftest. Adding a constraint: a row, its gate or apply/undo pair, and a
// selftest line
namespace TestConstraints
{
constexpr std::uint32_t PLANNER = 1;      // planner goals and jobs
constexpr std::uint32_t STATUS = 2;       // the weighted random status pick
constexpr std::uint32_t ERRAND = 4;       // vendor errands and inn binds
constexpr std::uint32_t HEARTH = 8;       // hearth legs, the off-mesh hearth and the planner's inn goal
constexpr std::uint32_t NOFIGHT = 16;     // starts no fight; creatures ignore the bot and its pets
constexpr std::uint32_t NODAMAGE = 32;    // takes no damage
constexpr std::uint32_t FLYFAIL = 64;     // a fly leg fails in the air past its middle (the way down, upkeep WU26b)
constexpr std::uint32_t FLYABORT = 128;   // as flyfail, as the trip's last failure: the trip ends once the bot is down
constexpr std::uint32_t NODESTROY = 256;  // destroys no bag item for space (bank-mail WB7d)
constexpr std::uint32_t NOTAXI = 512;     // plans no flight leg (travel WT55a: walk-only trips)
// mounts on its own no more (living-conversation LC17a; 1024, not 512, which travel's notaxi takes)
constexpr std::uint32_t NOMOUNT = 1024;
constexpr std::uint32_t FLEE = 2048;     // in PvP combat it flees from the enemy players attacking it (world PvP)
constexpr std::uint32_t NOUSE = 4096;    // uses no item on its own; only the console's requests pass (item use WI2)
constexpr std::uint32_t NOSPEND = 8192;  // counts as a member of the item use control cohort (item use WI10)
// world PvP's rules count its master, in a bot group its leader, as a game client (world-pvp WP37a)
constexpr std::uint32_t REALMASTER = 16384;
// a PvpTest fighter: world PvP's rules leave the bot out (today's attack on sight) and its engaged rows carry the
// pvptest tag (world-pvp WP37b)
constexpr std::uint32_t ONSIGHT = 32768;
// plans no own flying, it flies only on a taxi (travel WT61b; the first bit past 16, the set is 32 bits
// since travel landing 6)
constexpr std::uint32_t NOFLY = 65536;
// the bot's own AI starts no spell (living-conversation LC17c; the bridge's ordered cast is exempt)
constexpr std::uint32_t NOCAST = 131072;
// a choice row past the state bits (questing WQ1): the bot accepts no quest and drops none by itself
constexpr std::uint32_t NOQUEST = 262144;
// a wander status's NPC stop sells, buys and repairs at no vendor it meets (planning WT63c; 262144 is questing's
// noquest, landed with WQL1; 1048576 clashes with no other row)
constexpr std::uint32_t NOVENDOR = 1048576;
// the planner's job answers are dropped as failed, so the idle step falls back (planning WT63e; 524288 is used by no
// other plan's row)
constexpr std::uint32_t NOPLAN = 524288;
// every ride of a transport link costs the route no time, so a test's case rides (travel WT70c, WT.55;
// 2097152 since travel landing 7: 262144 is questing's noquest)
constexpr std::uint32_t FREERIDE = 2097152;
// a quest order without progress is given up after PursuitRules.h's SHORT_LIMIT_MS (questing WT35c; 2097152 is
// travel's freeride on the main line, so the next bit)
constexpr std::uint32_t SHORTGIVEUP = 4194304;
// its MeshMiss rows are written where a walk is ordered and where it ends, so a walk's rows are its own (travel
// WT64b; 8388608: 4194304 is questing's shortgiveup)
constexpr std::uint32_t WALKFLUSH = 8388608;
// the combat engine writes the bot's combat actions, its idle ticks' rejected candidates and once its known abilities
// as world-log decision rows (combat-foundation CF1; bit 24: 4194304 is questing's shortgiveup on the main line and
// 8388608 travel planning's walkflush on its branch)
constexpr std::uint32_t CASTTRACE = 16777216;
// the skill path weighs every learnable recipe and takes the fastest climb whatever its worth, so a test's craft has a
// queue (professions WR14b; bit 25: 8388608 is travel's walkflush, 16777216 combat's casttrace)
constexpr std::uint32_t ANYPATH = 33554432;
// the record of failed attempts expires from a 20 s base instead of WorldTravel.StuckAvoidSeconds, the repeat rule and
// its cap unchanged (travel WT64c; bit 26: 33554432 is professions' anypath)
constexpr std::uint32_t SHORTAVOID = 67108864;
constexpr std::uint32_t SHORT_AVOID_MS = 20 * 1000;
// the ghost code reads the bot's death as 65 minutes older than it is, as the rotation's hour out of the world aged
// the night's deaths (upkeep WU33b; the core sets no ghost time; bit 27: 33554432 is professions' anypath, 67108864
// travel's shortavoid)
constexpr std::uint32_t AGEDEATH = 134217728;
constexpr std::int64_t AGED_DEATH_SECONDS = 65 * 60;
// the bot's own AI runs no action and it stays attackable: a test's patient or puller whose health and threat a script
// drives (combat-foundation CFM4; bit 28: 67108864 is the main line's shortavoid, 134217728 upkeep's agedeath)
constexpr std::uint32_t PASSIVE = 268435456;
// the random-bot manager's randomize and refresh skip the bot: a case bot is held still between its setup and its
// verdict (pathing PA64; bit 29)
constexpr std::uint32_t NOFACTORY = 536870912;
// the planner holds the bot on the questing circuit: its plans build and offer no quest route (questing WQ18b; bit
// 30). It goes with the circuit (WQ28)
constexpr std::uint32_t CIRCUIT = 1073741824;
// each walk leg of the bot's own route ends by one teleport to its last waypoint, its rows a walk's: a test skips the
// walks it does not test (test design, the user 2026-11-25; bit 31: bit 30 is questing's circuit)
constexpr std::uint32_t FREEWALK = 2147483648u;
constexpr std::uint32_t FLY_FAILS = FLYFAIL | FLYABORT;

enum class Kind : std::uint8_t
{
    Hold,   // a gate reads the bit where the bot decides (`route hold` sets every one)
    Other,  // any other row: a bit its reader checks, or one with an apply and undo (APPLIED)
    Choice = Hold,
    State = Other
};

struct Row
{
    char const* name;
    Kind kind;
    std::uint32_t bit;
    char const* where;  // the gate that reads it, or what apply and undo do
};

constexpr Row ROWS[] = {
    {"planner", Kind::Hold, PLANNER, "planner hook and idle gate"},
    {"status", Kind::Hold, STATUS, "idle status pick"},
    {"errand", Kind::Hold, ERRAND, "errand and inn triggers (an order passes)"},
    {"hearth", Kind::Hold, HEARTH, "trip and planner hearth, off-mesh hearth"},
    {"noquest", Kind::Hold, NOQUEST,
     "rpg quest pick: no quest accepted, none dropped by the log upkeep; planner: no quest goal offered or kept"},
    {"nofight", Kind::Other, NOFIGHT, "immune to NPCs (bot and its pets), no grind or enemy player pick"},
    {"nodamage", Kind::Other, NODAMAGE, "god mode (CHEAT_GOD)"},
    {"flyfail", Kind::Other, FLYFAIL, "fly follower: the fly leg fails in the air past its middle, the trip replans"},
    {"flyabort", Kind::Other, FLYABORT, "fly follower: as flyfail with no replan left, the trip ends once down"},
    {"nodestroy", Kind::Other, NODESTROY, "smart destroy: no bag item destroyed for bag space or for loot"},
    {"notaxi", Kind::Other, NOTAXI, "route request: no taxi known, the route and the bot take no taxi"},
    {"nomount", Kind::Other, NOMOUNT, "check mount state: the bot mounts on its own no more"},
    {"flee", Kind::Other, FLEE, "retreat trigger: in PvP combat it flees from the enemy players attacking it"},
    {"nouse", Kind::Other, NOUSE, "use path: the bot uses no item on its own; only the console's requests pass"},
    {"nospend", Kind::Other, NOSPEND, "spend rule and buff picker: the bot counts as a member of the control cohort"},
    {"realmaster", Kind::Other, REALMASTER, "world PvP rules: its master, in a bot group its leader, is a game client"},
    {"onsight", Kind::Other, ONSIGHT,
     "world PvP rules: the bot is outside them (attack on sight), its engaged rows tagged"},
    {"nofly", Kind::Other, NOFLY, "route request: no own flying, the bot flies only on a taxi"},
    {"nocast", Kind::Other, NOCAST,
     "PlayerbotAI::CastSpell: the bot's own AI starts no spell (an ordered cast is exempt)"},
    {"novendor", Kind::Other, NOVENDOR, "wander npc status: no vendor stop (sell, buy, repair)"},
    {"noplan", Kind::Other, NOPLAN, "planner job answer: dropped as failed, the bot falls back (planning WT63e)"},
    {"freeride", Kind::Other, FREERIDE, "route request: every ride of a transport link costs no time"},
    {"shortgiveup", Kind::Other, SHORTGIVEUP, "quest executor: the give-up limit is two minutes without progress"},
    {"walkflush", Kind::Other, WALKFLUSH,
     "route start, a held trip's end, GoTo's local piece: the MeshMiss rows written where a walk starts and ends"},
    {"casttrace", Kind::Other, CASTTRACE,
     "combat engine and combat record: the bot's actions, rejects, known abilities, effects, casts, heals and damage"},
    {"anypath", Kind::Other, ANYPATH,
     "skill path inputs: every learnable recipe weighed, the fastest climb taken whatever its worth"},
    {"shortavoid", Kind::Other, SHORTAVOID,
     "failed-attempt record: the expiry base is 20 s instead of StuckAvoidSeconds (the repeat rule and the cap "
     "unchanged)"},
    {"agedeath", Kind::Other, AGEDEATH,
     "ghost way (corpse walk, spirit healer, repop): the death reads 65 minutes older than it is"},
    {"passive", Kind::Other, PASSIVE,
     "PlayerbotAI::DoNextAction: no engine runs an action (the bot stays attackable; the record's samples go on)"},
    {"nofactory", Kind::Other, NOFACTORY,
     "RandomPlayerbotMgr::Randomize and Refresh: return before they change the bot (gold, gear, level, revive); a dead "
     "bot stays dead"},
    {"circuit", Kind::Other, CIRCUIT,
     "planner situation (PlannerService SituationOf): the bot is held on the questing circuit, its plans build and "
     "offer no quest route"},
    {"freewalk", Kind::Other, FREEWALK,
     "route follower: a walk leg's rest is one teleport to its last waypoint, its rows a walk's"},
};

constexpr std::uint32_t BitsOf(Kind kind)
{
    std::uint32_t bits = 0;
    for (Row const& row : ROWS)
        if (row.kind == kind)
            bits |= row.bit;
    return bits;
}

constexpr std::uint32_t CHOICES = BitsOf(Kind::Hold);
constexpr std::uint32_t STATES = BitsOf(Kind::Other);
// The rows with an apply and undo in STATE_OPS (TestConstraintStates.cpp); a row that only sets a bit its reader
// checks needs no entry there
constexpr std::uint32_t APPLIED = NOFIGHT | NODAMAGE | CASTTRACE;
constexpr std::uint32_t HOLD = PLANNER | STATUS | ERRAND | HEARTH | NOQUEST;  // `route hold`: every hold row
static_assert(HOLD == CHOICES && !(CHOICES & STATES), "hold is the hold rows; a bit is one row's");

inline char const* KindName(Kind kind) { return kind == Kind::Hold ? "hold" : "other"; }

// Every row's name, for refusals
inline std::string Names()
{
    std::string out;
    for (Row const& row : ROWS)
        out += (out.empty() ? "" : ", ") + std::string(row.name);
    return out;
}

inline std::string Format(std::uint32_t set)
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
    std::uint32_t set{0};
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
        std::uint32_t bit = 0;
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
inline std::uint32_t Next(std::uint32_t set, std::uint32_t on, std::uint32_t off)
{
    return static_cast<std::uint32_t>((set & ~off) | on);
}

// A hold row turning on ends the bot's current status (it stands idle and the idle gate keeps it there); any other
// row changing never does
inline bool EndsStatus(std::uint32_t before, std::uint32_t after) { return (after & ~before & CHOICES) != 0; }

// The errand row turning off frees the errand checks at once: the retry wait after the last check (ERRAND_RETRY_MS,
// ItemWorthRules.h) starts over from no check, so a check from before the hold does not delay the released bot's first
// errand by up to five minutes
inline bool FreesErrands(std::uint32_t before, std::uint32_t after) { return (before & ~after & ERRAND) != 0; }

// A release: a hold row turning off (`route release`, `route constraint <row> off`). The planner goals the bot holds
// end there (GoalApply::Release), so its first goal after it is chosen under the rows the release keeps (WR30)
inline bool Releases(std::uint32_t before, std::uint32_t after) { return (before & ~after & CHOICES) != 0; }

// The gates. What an idle bot's status update may do on its own
struct IdleGate
{
    bool planner{true};  // planner job submits and goals (PlannerHook, OnIdle)
    bool status{true};   // the weighted status pick
};

inline IdleGate GateOf(std::uint32_t set) { return {!(set & PLANNER), !(set & STATUS)}; }
inline bool ErrandAllowed(std::uint32_t set) { return !(set & ERRAND); }
inline bool HearthAllowed(std::uint32_t set) { return !(set & HEARTH); }
// the rpg quest pick accepts and the quest log upkeep drops only while this holds (a turn-in stays allowed)
inline bool QuestChoicesAllowed(std::uint32_t set) { return !(set & NOQUEST); }
// the giver search (NewRpgHelpers::FindQuestGiver) walks to a giver with a turn-in, or with a quest to accept only
// while the bot may accept one: otherwise it stands at a giver it will take nothing from
inline bool GiverSought(bool canReward, bool canAccept, std::uint32_t set)
{
    return canReward || (canAccept && QuestChoicesAllowed(set));
}
// "attack anything" picks no mob the bot could not hit anyway (the engine refuses both directions), and the enemy
// player scan picks no player on sight (one already fighting the bot is still answered)
inline bool StartsFights(std::uint32_t set) { return !(set & NOFIGHT); }
// flyfail and flyabort, read by the fly leg's follower on a tick in the air (NewRpgRoute.cpp; nothing to apply or
// undo): the leg fails as its timeout would, once the bot has flown half the leg's straight 2D length and is in the
// air, so the failure lies where a test chose the leg's middle (over land, over water)
inline bool FlyLegFails(std::uint32_t set, float flownYards, float legYards, bool inTheAir)
{
    return (set & FLY_FAILS) && inTheAir && flownYards * 2.0f >= legYards;
}
// flyabort: the failure finds the trip's replans spent (TravelLegs::AfterFailure, MAX_REPLANS), so the trip aborts and
// comes down first (AbortDescent)
inline bool FlyFailAborts(std::uint32_t set) { return (set & FLYABORT) != 0; }
// flee, read by the world PvP retreat trigger (WorldPvpStrategy.cpp); its apply and undo do nothing
inline bool Flees(std::uint32_t set) { return (set & FLEE) != 0; }
// realmaster, read by world PvP's real master (WorldPvp::RealMasterOf, GhostWait.cpp; nothing to apply or undo): the
// bot never starts, joins its master's fights, never leaves by enthusiasm and its ghost waits for no enemy
inline bool HasRealMaster(std::uint32_t set) { return (set & REALMASTER) != 0; }
// onsight, read by world PvP's place of a bot (FightEnthusiasm.cpp PlaceOf, the engaged rows; nothing to apply or
// undo): PvpTest's fighters keep today's attack on sight and their engaged rows carry the pvptest tag
inline bool OnSight(std::uint32_t set) { return (set & ONSIGHT) != 0; }
// nodestroy, read by the smart destroy action (DestroyItemAction.cpp; nothing to apply or undo): a held bot keeps
// the full bags a test gives it, neither the bag-space destroy nor the destroy for a loot frees a slot
inline bool DestroysForSpace(std::uint32_t set) { return !(set & NODESTROY); }
// notaxi, read by the trip's route request (NewRpgRoute.cpp; nothing to apply or undo): the search knows no flight
// master, so the route and the bot take no taxi (also after the bot learns a flight node on the way)
inline bool TaxiAllowed(std::uint32_t set) { return !(set & NOTAXI); }
// nofly, read by the trip's route request (NewRpgRoute.cpp; nothing to apply or undo): the search knows no own flying
// (no fly leg, no air link), so a bot with a flying mount flies only on a taxi and walks where a walker walks
inline bool OwnFlyingAllowed(std::uint32_t set) { return !(set & NOFLY); }
// freeride, read by the trip's route request (NewRpgRoute.cpp; nothing to apply or undo): the search prices every ride
// of a transport link at no time, so a transport case's bot rides where a walk would be quicker
inline bool RidesFree(std::uint32_t set) { return (set & FREERIDE) != 0; }
// freewalk, read by the route follower's walk step (NewRpgTravelAction::StepTravel, TravelLegs::FreeWalkEnd; nothing
// to apply or undo): the walk leg's rest is one teleport, and the next step ends the leg as a walk's
inline bool WalksFree(std::uint32_t set) { return (set & FREEWALK) != 0; }
// walkflush, read where a walk is ordered and where it ends (TravelOrders::FlushWalk: `route start` ahead of the
// order's Status row, a held trip's end after its Travel row, the end of GoTo's local piece; nothing to apply or
// undo): the bot's MeshMiss rows are written there and not at the travel block's interval, so a test reads a walk's
// rows as that walk's own and not mixed with the moves before it and the status's work after it
inline bool FlushesWalks(std::uint32_t set) { return (set & WALKFLUSH) != 0; }
// shortavoid, read by the record's one writer (BotTravel::NoteFailure; nothing to apply or undo): an entry expires
// within a test's run, so a case sees the refusal and the new attempt after it
inline bool ShortAvoid(std::uint32_t set) { return (set & SHORTAVOID) != 0; }
// nomount, read by the check mount state action (CheckMountStateAction::Mount; nothing to apply or undo): an idle
// random bot mounts when it has no target, so a test that orders a fight to a bot on foot keeps it on foot
inline bool MountsOnItsOwn(std::uint32_t set) { return !(set & NOMOUNT); }
// nouse, read by the item use gates (UseItemAction, UseItemFightAction, PlayerbotAI::ImbueItem; nothing to apply or
// undo): the bot uses no item on its own, so a test's console request is the only use
inline bool UsesItemsOnItsOwn(std::uint32_t set) { return !(set & NOUSE); }
// nospend, read by the item spend rule and the buff picker (nothing to apply or undo): true when the bot spends
// normally; the control cohort reads !SpendsOnItsOwn
inline bool SpendsOnItsOwn(std::uint32_t set) { return !(set & NOSPEND); }
// nocast, read by PlayerbotAI::CastSpell (nothing to apply or undo): the bot's own AI casts nothing, so no cast or
// global cooldown of its own meets a test's ordered cast; the bridge's cast clears the bit around its call
inline bool CastsOnItsOwn(std::uint32_t set) { return !(set & NOCAST); }
// casttrace, read by Engine::DoNextAction (undo clears the known-abilities mark, so a later set writes them again) and
// the combat record's hooks (apply and undo put the bot in and out of its registry): the detail trace (CombatTrace.h)
inline bool CombatTraced(std::uint32_t set) { return (set & CASTTRACE) != 0; }
// novendor, read by the wander NPC status's stop (NewRpgStatus.cpp; nothing to apply or undo): a met vendor sells,
// buys and repairs nothing, so a test's errand is the bot's only trade
inline bool VendorStops(std::uint32_t set) { return !(set & NOVENDOR); }
// noplan, read where the bot takes its planner job's answer (PlannerService.cpp TakeGoals; nothing to apply or undo):
// every answer counts as failed, so the idle step falls back (fallback_failed); route release keeps it
inline bool PlansTaken(std::uint32_t set) { return !(set & NOPLAN); }
// shortgiveup, read by the quest executor's give-up clock (PursuitRules.h LimitMs; nothing to apply or undo): a test
// sees a give-up for want of progress in minutes, without a server-wide limit that would cut its other cases
inline bool GiveUpShortened(std::uint32_t set) { return (set & SHORTGIVEUP) != 0; }
// anypath, read where the skill path's inputs are copied (SkillPathLive.cpp PathInputsOf; nothing to apply or undo): a
// test's craft from skill 1 has a chosen stretch and a queue whatever the bot's gold, traits and uses make of its worth
inline bool TakesAnyPath(std::uint32_t set) { return (set & ANYPATH) != 0; }
// agedeath, read where the ghost code takes its death's age (ReviveFromCorpseAction.cpp: the corpse walk, the spirit
// healer and the death row's dead_seconds; ReleaseSpiritAction.cpp: the repop's grave; nothing to apply or undo): a
// test ghost's death counts AGED_DEATH_SECONDS older, so a case sees the way back of a ghost that was out of the world
inline std::int64_t DeathAgeSeconds(std::int64_t seconds, std::uint32_t set)
{
    return (set & AGEDEATH) ? seconds + AGED_DEATH_SECONDS : seconds;
}
// passive, read by PlayerbotAI::DoNextAction before the engine's call (nothing to apply or undo): no combat, non-combat
// or dead engine runs an action, so a test's patients and pullers stand while a script drives their health and threat
inline bool ActsOnItsOwn(std::uint32_t set) { return !(set & PASSIVE); }
// false: the random-bot manager's Randomize and Refresh leave the bot alone
inline bool FactoryMayRun(std::uint32_t set) { return !(set & NOFACTORY); }
// circuit, read where the planner fills a goals job's situation (PlannerService.cpp SituationOf; nothing to apply or
// undo): the bot is held on the questing circuit, so its plans build and offer no quest route (questing WQ18b)
inline bool HeldOnCircuit(std::uint32_t set) { return (set & CIRCUIT) != 0; }

// A state row's apply and undo on the bot (Target: Player on the server, a model in the selftest). Each is idempotent
template <class Target>
struct StateOps
{
    std::uint32_t bit;
    void (*apply)(Target&);
    void (*undo)(Target&);
};

template <class Target, std::size_t N>
constexpr std::uint32_t Covered(StateOps<Target> const (&ops)[N])
{
    std::uint32_t bits = 0;
    for (StateOps<Target> const& op : ops)
        bits |= op.bit;
    return bits;
}

// The state rows turned on are applied, those turned off undone
template <class Target, std::size_t N>
void ChangeStates(StateOps<Target> const (&ops)[N], Target& target, std::uint32_t before, std::uint32_t after)
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
void KeepStates(StateOps<Target> const (&ops)[N], Target& target, std::uint32_t set)
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
    return order.stop || !order.place.empty() || order.quest || !order.status.empty();
}

// The bot's AI update takes its order at once (TravelOrders::TakeAndApply); only an order that moves the bot (a trip,
// a stop, aboard) waits until the bot is out of combat, a constraint change never waits
template <typename Order>
bool TakeNow(Order const& order, bool inCombat)
{
    return !inCombat || (!HasTrip(order) && !order.aboard);
}

// Whether the part of an order that is not a constraint change needs the new rpg strategy: a trip, aboard or flush
// does. A bot in a party runs the group's AI instead, and a test switches its state rows there (combat-foundation CFM5:
// a bench's members turn passive on and off between the segments of one formed party); tests only, no order outside one
template <typename Order>
bool NeedsNewRpg(Order const& order)
{
    return HasTrip(order) || order.aboard || order.flush;
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
        pending.quest = incoming.quest;
        pending.field = incoming.field;
        pending.minutes = incoming.minutes;
        pending.status = std::move(incoming.status);
        pending.entry = incoming.entry;
    }
    if (incoming.aboard)
        pending.aboard = incoming.aboard;
    pending.flush = pending.flush || incoming.flush;  // a flush is no trip: it neither waits nor is replaced
    pending.postedMs = incoming.postedMs;
}
}  // namespace TestConstraints

#endif
