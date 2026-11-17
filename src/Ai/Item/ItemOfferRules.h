/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMOFFERRULES_H
#define _PLAYERBOT_ITEMOFFERRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "ItemSituation.h"
#include "ItemSpendRules.h"
#include "ItemUseKinds.h"
#include "ItemUseTypes.h"

// Item use plan WI14, PLAN 3.5 and 3.7, INTERFACE 1.11: the offer and the aim. Pure and std only, any thread, no
// allocation, no clock and no global state: the same situation and list give the same offer.
//
// The offer. WantedNeeds says which needs the situation has (thresholds below). The needs are tried in their enum
// order; for each, the entries that answer it (EntryAnswers: the need's kind and sub, a fitting shape, never the
// PvP trinket, which stays its tactic's) and are usable (ready, uses left) and whose target can be built
// (AnswerTarget). A free entry (not consumed) wins at once over any consumed one (free before consumed). Among one
// class the stronger: higher rank, then the lower price, then the lower list index. Consumed entries are weighed by
// the spend rule (Spend) in that order; the first it spends answers the need, a weaker cheaper one only when the
// stronger is refused. The first need with an answer wins, except a potion: when the winning need's answer is a
// potion, every wanted need's spent potion is gathered and BestPotionNeed picks the need where it saves most (one
// potion per fight). A heal goes to the bot while it is below the heal bar, else (WI15b) to the most hurt friendly
// unit below it within the item's friend reach, by an item of the friend or any-unit shape. Nothing answers: any ==
// false; when a consumed candidate was weighed, need, usable, consumed and verdict hold the first wanted need's
// strongest weighed candidate and its keep verdict (the item_spend row's keep:<kind>, SpendDecision), else consumed ==
// false.
//
// WI16 (PLAN 3.8, the chain): among the consumed answers of one need a non-potion is weighed before a potion (a
// healthstone or a rune before the fight's one potion), each class strongest first. Two rules of usable (EntryUsable):
// in an arena a consumed heal is the PvP tactics' (CcTacticStrategy's potion gate holds the healthstone and the
// healing potion), never the offer's; a channelled use (a bandage) only while no enemy hits the bot and it stands, and
// a channelled heal not while the engine's Recently Bandaged is on the bot.
//
// WI18 (PLAN 3.9, the gadgets): control is also wanted against one enemy of the fight that flees (a chase) or casts
// (StopTarget), and that enemy is the one it holds; a casting target takes only an item that stops a cast
// (UsableEntry::stopsCast: a stun, a silence; a net's root does not), and a control item on an enemy that flees saves
// RUNNER_MINUTES of the spend rule. A ground decoy goes to the nearest enemy's place within its range, else the
// bot's own (DecoyTarget). Helper and decoy are wanted when overwhelmed, resurrection after the fight, repair out of
// it with broken gear and no vendor near; chases stays unset by the sampler (a fleeing enemy is NearUnit::flees).
//
// ready (UsableEntry) is CheapCheck's target-independent part, folded by the caller when the situation is sampled:
// cooldown, charges, potion, busy, dead, no control, the in-fight need, arena, form, place, level and class. The
// target-dependent part (shape against the target's kind, range, too close) is what Offer::target satisfies by
// construction; an area spot's sight is the sampler's (Situation::aimSight, WI17), a unit's the caller's (Check). A
// drunk potion and an empty stack are refused here as well.
//
// The aim (3.7). Candidates: the AIM_ENEMIES fight enemies nearest the bot (alive, in the fight, not friendly, not
// controlled; ties by index), the point between each two of them and their centre; an item on units (enemy shape)
// only the enemies; an item centred on the bot only the bot's place. Each spot covers the fight enemies (not
// controlled, all of them, not only the nearest) within the radius (3D, the edge inside). Forbidden: a unit inside
// the radius that is not friendly and not in the fight (nothing new is pulled), a controlled fight enemy inside, or
// for a bot-centred item health below RECOIL_HEALTH_PCT. A friendly unit inside is not forbidden (it is not pulled;
// PLAN 3.7 forbids units not in the fight, read as non-friendly ones). AimPick: the most covers among the spots that
// are allowed, in range and in sight; ties the lower index; none when none covers an enemy. The offer wants at least
// AREA_MIN_HITS, or one enemy of AREA_ELITE_MIN_RANK or higher (an elite, a rare, a boss; not a player) alone
// (AreaWorth, WI17). How many enemies make it worth it is the spend rule's answer (hits). A spot's sight (WI17):
// bit k of Situation::aimSight; the sampler asks the engine in the pick's order (NextSightCheck) until the best spot
// it sees is known, so the pick over those bits is the pick over every spot's sight.
//
// Tuning values [E], one declaration each (CS.16), no conf option; the thresholds mirror the config defaults
// (Playerbots.MediumHealth 65, MediumMana 40) without reading them.
namespace ItemUse
{
constexpr float HEAL_HEALTH_PCT = 65.0f;           // below it the bot wants a heal (MediumHealth)
constexpr float MANA_PCT = 40.0f;                  // below it a mana user wants mana (MediumMana)
constexpr std::uint8_t PROTECT_ATTACKERS = 2;      // this many on the bot want a protection at any health
constexpr std::uint8_t CONTROL_ATTACKERS = 2;      // this many on the bot want one controlled
constexpr std::uint8_t OVERWHELMED_ATTACKERS = 2;  // with OVERWHELMED_DANGER: a helper or a decoy
constexpr float OVERWHELMED_DANGER = 0.5f;         // the fight lasts as long as the bot lives
constexpr std::uint8_t AREA_MIN_HITS = 2;          // below it the need is damage to one
constexpr Rank AREA_ELITE_MIN_RANK = Rank::Elite;  // WI17: one enemy of this rank or higher is worth an area item
constexpr std::size_t AIM_ENEMIES = 6;             // nearest enemies that make candidates: 6 + 15 pairs + 1 centre
constexpr float RECOIL_HEALTH_PCT = 40.0f;         // a bot-centred item's recoil needs this much health
constexpr float MELEE_YARDS = 5.0f;                // the reach of a unit item whose range is 0

// Words: heal mana protect dispel speed boost damage_one damage_area control helper decoy resurrect repair
enum class Need : std::uint8_t
{
    Heal,
    Mana,
    Protect,
    Dispel,
    Speed,
    Boost,
    DamageOne,
    DamageArea,
    Control,
    Helper,
    Decoy,
    Resurrect,
    Repair,
    Count
};

constexpr std::size_t NEED_COUNT = static_cast<std::size_t>(Need::Count);
constexpr std::array<std::string_view, NEED_COUNT> NEED_WORDS = {
    "heal",        "mana",    "protect", "dispel", "speed",     "boost", "damage_one",
    "damage_area", "control", "helper",  "decoy",  "resurrect", "repair"};
static_assert(static_cast<std::size_t>(Need::Repair) + 1 == NEED_COUNT, "a word per need");

constexpr std::string_view NeedWord(Need need)
{
    std::size_t const index = static_cast<std::size_t>(need);
    return index < NEED_COUNT ? NEED_WORDS[index] : std::string_view("heal");
}

using NeedMask = std::uint16_t;
static_assert(NEED_COUNT <= 16, "a bit per need");

constexpr NeedMask NeedBit(Need need) { return static_cast<NeedMask>(1u << static_cast<unsigned>(need)); }

// The kind whose entries answer a need (lane 1's demand reads it, INTERFACE 2.3)
constexpr Kind NeedKind(Need need)
{
    switch (need)
    {
        case Need::Heal:
            return Kind::Heal;
        case Need::Mana:
            return Kind::Mana;
        case Need::Protect:
            return Kind::Protect;
        case Need::Dispel:
            return Kind::Dispel;
        case Need::Speed:
            return Kind::Move;
        case Need::Boost:
            return Kind::Boost;
        case Need::DamageOne:
            return Kind::DamageOne;
        case Need::DamageArea:
            return Kind::DamageArea;
        case Need::Control:
            return Kind::Control;
        case Need::Helper:
        case Need::Decoy:
        case Need::Repair:
            return Kind::Summon;
        case Need::Resurrect:
            return Kind::Resurrect;
        default:
            return Kind::Unclassified;
    }
}

struct Offer
{
    bool any{false};
    Need need{Need::Heal};
    std::uint8_t usable{0};  // index into the list
    Target target;
    bool consumed{false};
    SpendVerdict
        verdict;  // a consumed candidate was weighed: the spend that allowed the offer, or with any==false the keep
};

struct AimSpot
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::uint8_t covers{0};
    bool forbidden{false};
    bool inRange{false};
    bool inSight{true};    // the harness (or the caller's sight check) sets it
    std::int8_t unit{-1};  // WI14 append: the unit the spot is on, -1: a point between, the centre or the bot
};

constexpr std::size_t MAX_SPOTS = 24;
static_assert(AIM_ENEMIES + AIM_ENEMIES * (AIM_ENEMIES - 1) / 2 + 1 <= MAX_SPOTS, "the candidates fit");

struct AimSpots
{
    std::array<AimSpot, MAX_SPOTS> spots{};
    std::uint8_t count{0};
};

// What the aim needs of an item
struct AimItem
{
    float radius{0.0f};
    float range{0.0f};
    float minRange{0.0f};
    bool selfCentred{false};  // the area is around the bot
    bool onUnits{false};      // the area is around an enemy it targets
};

namespace OfferDetail
{
inline float Distance(float ax, float ay, float az, float bx, float by, float bz)
{
    float const dx = ax - bx;
    float const dy = ay - by;
    float const dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline std::size_t UnitCount(Situation const& situation)
{
    return std::min<std::size_t>(situation.unitCount, MAX_NEAR);
}

inline bool Alive(NearUnit const& unit) { return unit.healthPct > 0.0f; }

inline bool FightEnemy(NearUnit const& unit) { return !unit.friendly && unit.inFight && Alive(unit); }

inline bool Hittable(NearUnit const& unit) { return FightEnemy(unit) && !unit.controlled; }

inline bool Fighting(Situation const& situation) { return situation.inFight && !situation.fightEnded; }

// WI17: an enemy worth an area item alone (rank AREA_ELITE_MIN_RANK or higher, not a player)
inline bool StrongEnemy(NearUnit const& unit) { return unit.rank >= AREA_ELITE_MIN_RANK && unit.rank != Rank::Player; }

inline float BotDistance(Situation const& situation, NearUnit const& unit)
{
    return Distance(situation.x, situation.y, situation.z, unit.x, unit.y, unit.z);
}

inline float UnitReach(float range) { return range > 0.0f ? range : MELEE_YARDS; }

inline bool InRange(float distance, float minRange, float range) { return distance >= minRange && distance <= range; }

inline std::uint8_t Attackers(Situation const& situation)
{
    std::uint8_t count = 0;
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
        if (Hittable(situation.units[i]) && situation.units[i].onBot)
            ++count;
    return count;
}

// WI16: a held foe within melee reach still threatens a channel: a root does not stop its blows, and the hold
// breaking ends the channel at once
inline bool HeldInReach(Situation const& situation)
{
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
    {
        NearUnit const& unit = situation.units[i];
        if (FightEnemy(unit) && unit.controlled && BotDistance(situation, unit) <= MELEE_YARDS)
            return true;
    }
    return false;
}

inline std::uint8_t Hittables(Situation const& situation)
{
    std::uint8_t count = 0;
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
        if (Hittable(situation.units[i]))
            ++count;
    return count;
}

inline bool Won(Situation const& situation)
{
    if (situation.fightEnded)
        return true;
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
        if (FightEnemy(situation.units[i]))
            return false;
    return true;
}

inline bool ValidTarget(Situation const& situation)
{
    return situation.target >= 0 && static_cast<std::size_t>(situation.target) < UnitCount(situation);
}

// The enemy a damage item hits: the bot's target, else the nearest one on the bot; -1: none
inline std::int8_t DamageTarget(Situation const& situation)
{
    if (ValidTarget(situation) && Hittable(situation.units[static_cast<std::size_t>(situation.target)]))
        return situation.target;
    std::int8_t best = -1;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
    {
        NearUnit const& unit = situation.units[i];
        if (!Hittable(unit) || !unit.onBot)
            continue;
        float const distance = BotDistance(situation, unit);
        if (best < 0 || distance < bestDistance)
        {
            best = static_cast<std::int8_t>(i);
            bestDistance = distance;
        }
    }
    return best;
}

// WI18: the enemy of the fight a control item stops: the nearest that flees (a chase) or casts; -1: none
inline std::int8_t StopTarget(Situation const& situation)
{
    std::int8_t best = -1;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
    {
        NearUnit const& unit = situation.units[i];
        if (!Hittable(unit) || !(unit.flees || unit.casts))
            continue;
        float const distance = BotDistance(situation, unit);
        if (best < 0 || distance < bestDistance)
        {
            best = static_cast<std::int8_t>(i);
            bestDistance = distance;
        }
    }
    return best;
}

// The enemy a control item holds: the one StopTarget names, else the nearest one on the bot that is not its
// target, else its target; -1: none
inline std::int8_t ControlTarget(Situation const& situation)
{
    std::int8_t const stop = StopTarget(situation);
    if (stop >= 0)
        return stop;
    std::int8_t best = -1;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
    {
        NearUnit const& unit = situation.units[i];
        if (!Hittable(unit) || !unit.onBot || static_cast<std::int8_t>(i) == situation.target)
            continue;
        float const distance = BotDistance(situation, unit);
        if (best < 0 || distance < bestDistance)
        {
            best = static_cast<std::int8_t>(i);
            bestDistance = distance;
        }
    }
    if (best < 0 && ValidTarget(situation) && Hittable(situation.units[static_cast<std::size_t>(situation.target)]))
        best = situation.target;
    return best;
}

// The nearest dead friendly unit within reach; -1: none
inline std::int8_t DeadFriend(Situation const& situation, float minRange, float reach)
{
    std::int8_t best = -1;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
    {
        NearUnit const& unit = situation.units[i];
        if (!unit.friendly || Alive(unit))
            continue;
        float const distance = BotDistance(situation, unit);
        if (InRange(distance, minRange, reach) && (best < 0 || distance < bestDistance))
        {
            best = static_cast<std::int8_t>(i);
            bestDistance = distance;
        }
    }
    return best;
}

// The most hurt living friendly unit below the heal bar within reach (ties: the lower index); -1: none
inline std::int8_t HurtFriend(Situation const& situation, float minRange, float reach)
{
    std::int8_t best = -1;
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
    {
        NearUnit const& unit = situation.units[i];
        if (!unit.friendly || !Alive(unit) || unit.healthPct >= HEAL_HEALTH_PCT)
            continue;
        if (!InRange(BotDistance(situation, unit), minRange, reach))
            continue;
        if (best < 0 || unit.healthPct < situation.units[static_cast<std::size_t>(best)].healthPct)
            best = static_cast<std::int8_t>(i);
    }
    return best;
}

inline bool SelfNeed(Need need)
{
    return need != Need::DamageOne && need != Need::DamageArea && need != Need::Control && need != Need::Resurrect;
}

inline bool StrongerThan(UsableEntry const& a, std::size_t ia, UsableEntry const& b, std::size_t ib)
{
    if (a.rank != b.rank)
        return a.rank > b.rank;
    if (a.replaceCopper != b.replaceCopper)
        return a.replaceCopper < b.replaceCopper;
    return ia < ib;
}

// The order consumed answers of one need are weighed in (WI16, the chain): a non-potion first, then the stronger
inline bool ConsumedBefore(UsableEntry const& a, std::size_t ia, UsableEntry const& b, std::size_t ib)
{
    if (a.potion != b.potion)
        return !a.potion;
    return StrongerThan(a, ia, b, ib);
}

inline Target SelfTarget(Situation const& situation, Shape shape)
{
    Target target;
    if (shape == Shape::None)
        return target;
    if (shape == Shape::Ground)
    {
        target.kind = TargetKind::Spot;
        target.x = situation.x;
        target.y = situation.y;
        target.z = situation.z;
        return target;
    }
    target.kind = TargetKind::Self;
    return target;
}

inline Target UnitTarget(NearUnit const& unit)
{
    Target target;
    target.kind = TargetKind::Unit;
    target.guid = unit.guid;
    target.dead = !Alive(unit);
    return target;
}

inline Target SpotTarget(float x, float y, float z)
{
    Target target;
    target.kind = TargetKind::Spot;
    target.x = x;
    target.y = y;
    target.z = z;
    return target;
}

// WI18: a ground decoy's spot, beside the enemies: the nearest fight enemy's place within the item's range, else the
// bot's own spot when the range allows it; false: neither
inline bool DecoyTarget(Situation const& situation, UsableEntry const& entry, Target& target)
{
    std::int8_t best = -1;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
    {
        NearUnit const& unit = situation.units[i];
        if (!FightEnemy(unit))
            continue;
        float const distance = BotDistance(situation, unit);
        if (InRange(distance, entry.minRangeYards, entry.rangeYards) && (best < 0 || distance < bestDistance))
        {
            best = static_cast<std::int8_t>(i);
            bestDistance = distance;
        }
    }
    if (best >= 0)
    {
        NearUnit const& unit = situation.units[static_cast<std::size_t>(best)];
        target = SpotTarget(unit.x, unit.y, unit.z);
        return true;
    }
    if (!InRange(0.0f, entry.minRangeYards, entry.rangeYards))
        return false;
    target = SelfTarget(situation, Shape::Ground);
    return true;
}
}  // namespace OfferDetail

// The needs the situation has, as bits (NeedBit); danger is Danger(situation)
inline NeedMask WantedNeeds(Situation const& situation, float danger)
{
    using namespace OfferDetail;
    NeedMask mask = 0;
    bool const fighting = Fighting(situation);
    bool const fightOrAfter = situation.inFight || situation.fightEnded;
    std::uint8_t const attackers = Attackers(situation);
    bool const hurt = situation.healthPct < HEAL_HEALTH_PCT;
    // WI15b: a hurt friend in the situation (a group member near) wants a heal too; reach is the item's (AnswerTarget)
    bool const friendHurt = HurtFriend(situation, 0.0f, std::numeric_limits<float>::max()) >= 0;

    if (fightOrAfter && (hurt || friendHurt))
        mask |= NeedBit(Need::Heal);
    if (fightOrAfter && situation.usesMana && situation.manaPct < MANA_PCT)
        mask |= NeedBit(Need::Mana);
    // plan 3.8: a flight asks for protection as well as speed, whoever has the bot as victim at that moment
    if (fighting &&
        ((hurt && situation.healthLossPerSecond > 0.0f) || attackers >= PROTECT_ATTACKERS || situation.flees))
        mask |= NeedBit(Need::Protect);
    if (situation.dispelMask != 0)
        mask |= NeedBit(Need::Dispel);
    if (situation.flees || situation.chases || ((situation.rooted || situation.snared) && attackers > 0))
        mask |= NeedBit(Need::Speed);
    if (fighting && situation.bursts && !situation.holdsBurst)
        mask |= NeedBit(Need::Boost);
    if (fighting && DamageTarget(situation) >= 0)
        mask |= NeedBit(Need::DamageOne);
    bool strongHittable = false;
    for (std::size_t i = 0; i < UnitCount(situation) && !strongHittable; ++i)
        strongHittable = Hittable(situation.units[i]) && StrongEnemy(situation.units[i]);
    if (fighting && (Hittables(situation) >= AREA_MIN_HITS || strongHittable))
        mask |= NeedBit(Need::DamageArea);
    if (fighting && (attackers >= CONTROL_ATTACKERS || situation.cornered || StopTarget(situation) >= 0) &&
        ControlTarget(situation) >= 0)
        mask |= NeedBit(Need::Control);
    bool const overwhelmed = fighting && danger >= OVERWHELMED_DANGER && attackers >= OVERWHELMED_ATTACKERS;
    if (overwhelmed)
        mask |= NeedBit(Need::Helper);
    if (overwhelmed || (fighting && (situation.flees || situation.cornered)))
        mask |= NeedBit(Need::Decoy);
    if (!fighting && situation.groupDead > 0)
        mask |= NeedBit(Need::Resurrect);
    if (situation.gearBroken && !situation.inFight && !situation.vendorNear)
        mask |= NeedBit(Need::Repair);
    return mask;
}

inline NeedMask WantedNeeds(Situation const& situation) { return WantedNeeds(situation, Danger(situation)); }

// The entry can answer the need: its kind and sub, a shape that fits; never the PvP trinket. Not its readiness.
inline bool EntryAnswers(UsableEntry const& entry, Need need)
{
    if (entry.pvpTrinket || entry.kind != NeedKind(need))
        return false;

    std::uint8_t const sub = entry.sub;
    switch (need)
    {
        case Need::Mana:
            if (sub != static_cast<std::uint8_t>(Sub::Mana))
                return false;
            break;
        case Need::Speed:
            if (sub != static_cast<std::uint8_t>(Sub::Speed))
                return false;
            break;
        case Need::Helper:
            if (sub != static_cast<std::uint8_t>(Sub::Helper))
                return false;
            break;
        case Need::Decoy:
            if (sub != static_cast<std::uint8_t>(Sub::Decoy))
                return false;
            break;
        case Need::Repair:
            if (sub != static_cast<std::uint8_t>(Sub::Repair))
                return false;
            break;
        default:
            break;
    }

    Shape const shape = entry.shape;
    switch (need)
    {
        case Need::DamageOne:
        case Need::Control:
            return shape == Shape::Enemy || shape == Shape::AnyUnit || shape == Shape::Ground;
        case Need::DamageArea:
            return entry.radiusYards > 0.0f && (shape == Shape::Self || shape == Shape::None || shape == Shape::Enemy ||
                                                shape == Shape::AnyUnit || shape == Shape::Ground);
        case Need::Resurrect:
            return shape == Shape::Friend || shape == Shape::AnyUnit;
        default:
            return shape == Shape::Self || shape == Shape::Friend || shape == Shape::AnyUnit || shape == Shape::None ||
                   shape == Shape::Ground;
    }
}

// WI16: a consumed heal in an arena is the PvP tactics' (the potion gate), never the offer's
inline bool ArenaHeld(Situation const& situation, UsableEntry const& entry)
{
    return situation.inArena && entry.consumed && entry.kind == Kind::Heal;
}

// WI16: a channel (a bandage) breaks on a hit or a step: none while an enemy hits the bot, a held one stands within
// its melee reach or the bot moves, and a channelled heal not while Recently Bandaged is on the bot
inline bool ChannelHeld(Situation const& situation, UsableEntry const& entry)
{
    if (!entry.channeled)
        return false;
    return situation.moving || OfferDetail::Attackers(situation) > 0 || OfferDetail::HeldInReach(situation) ||
           (entry.kind == Kind::Heal && situation.recentlyBandaged);
}

// Usable now: ready, uses left, not the PvP trinket, not a potion after this fight's potion, not held (WI16)
inline bool EntryUsable(Situation const& situation, UsableEntry const& entry)
{
    return entry.ready && entry.count > 0 && !entry.pvpTrinket && !(entry.potion && situation.potionDrunk) &&
           !ArenaHeld(situation, entry) && !ChannelHeld(situation, entry);
}

inline AimItem AimItemOf(Shape shape, float radius, float range, float minRange)
{
    AimItem item;
    item.radius = radius;
    item.range = range;
    item.minRange = minRange;
    item.selfCentred =
        radius > 0.0f && (shape == Shape::Self || shape == Shape::None || (shape == Shape::Ground && range <= 0.0f));
    item.onUnits = !item.selfCentred && (shape == Shape::Enemy || shape == Shape::AnyUnit);
    return item;
}

inline AimItem AimItemOf(UsableEntry const& entry)
{
    return AimItemOf(entry.shape, entry.radiusYards, entry.rangeYards, entry.minRangeYards);
}

inline AimItem AimItemOf(Record const& record)
{
    return AimItemOf(record.shape, record.radiusYards, record.rangeYards, record.minRangeYards);
}

// The candidate spots with their covers, forbidden and range facts, pure
inline AimSpots AimCandidates(Situation const& situation, AimItem const& item)
{
    using namespace OfferDetail;
    AimSpots out;
    if (!(item.radius > 0.0f))
        return out;

    auto const add = [&out](float x, float y, float z, std::int8_t unit)
    {
        AimSpot& spot = out.spots[out.count++];
        spot.x = x;
        spot.y = y;
        spot.z = z;
        spot.unit = unit;
    };

    if (item.selfCentred)
        add(situation.x, situation.y, situation.z, -1);
    else
    {
        // the nearest hittable enemies, sorted by distance, ties by index
        std::array<std::uint8_t, AIM_ENEMIES> chosen{};
        std::array<float, AIM_ENEMIES> distances{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < UnitCount(situation); ++i)
        {
            if (!Hittable(situation.units[i]))
                continue;
            float const distance = BotDistance(situation, situation.units[i]);
            std::size_t at = count;
            while (at > 0 && distances[at - 1] > distance)
                --at;
            if (at >= AIM_ENEMIES)
                continue;
            std::size_t const last = std::min(count, AIM_ENEMIES - 1);
            for (std::size_t k = last; k > at; --k)
            {
                chosen[k] = chosen[k - 1];
                distances[k] = distances[k - 1];
            }
            chosen[at] = static_cast<std::uint8_t>(i);
            distances[at] = distance;
            count = std::min(count + 1, AIM_ENEMIES);
        }
        for (std::size_t k = 0; k < count; ++k)
        {
            NearUnit const& unit = situation.units[chosen[k]];
            add(unit.x, unit.y, unit.z, static_cast<std::int8_t>(chosen[k]));
        }
        if (!item.onUnits)
        {
            for (std::size_t a = 0; a < count; ++a)
                for (std::size_t b = a + 1; b < count; ++b)
                {
                    NearUnit const& ua = situation.units[chosen[a]];
                    NearUnit const& ub = situation.units[chosen[b]];
                    add((ua.x + ub.x) * 0.5f, (ua.y + ub.y) * 0.5f, (ua.z + ub.z) * 0.5f, -1);
                }
            if (count >= 3)
            {
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                for (std::size_t k = 0; k < count; ++k)
                {
                    x += situation.units[chosen[k]].x;
                    y += situation.units[chosen[k]].y;
                    z += situation.units[chosen[k]].z;
                }
                float const n = static_cast<float>(count);
                add(x / n, y / n, z / n, -1);
            }
        }
    }

    bool const recoilBorne = situation.healthPct >= RECOIL_HEALTH_PCT;
    float const reach = item.onUnits ? UnitReach(item.range) : item.range;
    for (std::size_t s = 0; s < out.count; ++s)
    {
        AimSpot& spot = out.spots[s];
        std::uint8_t covers = 0;
        bool forbidden = item.selfCentred && !recoilBorne;
        for (std::size_t i = 0; i < UnitCount(situation); ++i)
        {
            NearUnit const& unit = situation.units[i];
            if (unit.friendly || !Alive(unit) || Distance(spot.x, spot.y, spot.z, unit.x, unit.y, unit.z) > item.radius)
                continue;
            if (!unit.inFight || unit.controlled)
                forbidden = true;
            else
                ++covers;
        }
        spot.covers = covers;
        spot.forbidden = forbidden;
        spot.inRange = item.selfCentred
                           ? item.minRange <= 0.0f
                           : InRange(Distance(situation.x, situation.y, situation.z, spot.x, spot.y, spot.z),
                                     item.minRange, reach);
        spot.inSight = item.selfCentred || s >= 32 || ((situation.aimSight >> s) & 1u) != 0;
    }
    return out;
}

inline AimSpots AimCandidates(Situation const& situation, Record const& record)
{
    return AimCandidates(situation, AimItemOf(record));
}

inline AimSpots AimCandidates(Situation const& situation, UsableEntry const& entry)
{
    return AimCandidates(situation, AimItemOf(entry));
}

// The best spot allowed, in range and in sight: the most covers, ties the lower index; -1: none covers an enemy
inline std::int8_t AimPick(AimSpots const& spots)
{
    std::int8_t best = -1;
    std::uint8_t bestCovers = 0;
    std::size_t const count = std::min<std::size_t>(spots.count, MAX_SPOTS);
    for (std::size_t i = 0; i < count; ++i)
    {
        AimSpot const& spot = spots.spots[i];
        if (spot.forbidden || !spot.inRange || !spot.inSight)
            continue;
        if (spot.covers > bestCovers)
        {
            best = static_cast<std::int8_t>(i);
            bestCovers = spot.covers;
        }
    }
    return best;
}

// WI17: the sampler's next sight question for an area item: the spot whose sight the engine must answer so that
// AimPick over the bits answered (known; sight: the answers, other bits 0) equals AimPick over every spot's sight.
// The order is the pick's: allowed, in range and covering an enemy, the most covers first, ties the lower index; the
// first known to be in sight settles it. -1: settled (none left, or a bot-centred item, which needs no sight)
inline std::int8_t NextSightCheck(AimSpots const& spots, bool selfCentred, std::uint32_t known, std::uint32_t sight)
{
    if (selfCentred)
        return -1;
    std::size_t const count = std::min<std::size_t>(spots.count, std::min<std::size_t>(MAX_SPOTS, 32));
    std::uint32_t passed = 0;
    while (true)
    {
        std::int8_t best = -1;
        for (std::size_t i = 0; i < count; ++i)
        {
            AimSpot const& spot = spots.spots[i];
            if (spot.forbidden || !spot.inRange || spot.covers == 0 || ((passed >> i) & 1u) != 0)
                continue;
            if (best < 0 || spot.covers > spots.spots[static_cast<std::size_t>(best)].covers)
                best = static_cast<std::int8_t>(i);
        }
        if (best < 0)
            return -1;
        std::uint32_t const bit = std::uint32_t(1) << best;
        if ((known & bit) == 0)
            return best;
        if ((sight & bit) != 0)
            return -1;
        passed |= bit;
    }
}

// WI17: the spot is worth the area item: AREA_MIN_HITS enemies covered, or one StrongEnemy among those covered
inline bool AreaWorth(Situation const& situation, AimItem const& item, AimSpot const& spot)
{
    using namespace OfferDetail;
    if (spot.covers >= AREA_MIN_HITS)
        return true;
    if (spot.covers == 0)
        return false;
    for (std::size_t i = 0; i < UnitCount(situation); ++i)
    {
        NearUnit const& unit = situation.units[i];
        if (Hittable(unit) && StrongEnemy(unit) &&
            Distance(spot.x, spot.y, spot.z, unit.x, unit.y, unit.z) <= item.radius)
            return true;
    }
    return false;
}

// The target the entry would be used on for the need, and how many enemies it hits; false: none fits
inline bool AnswerTarget(Situation const& situation, UsableEntry const& entry, Need need, Target& target,
                         std::uint8_t& hits)
{
    using namespace OfferDetail;
    target = Target{};
    hits = 1;
    Shape const shape = entry.shape;

    // a heal while the bot is whole goes to the most hurt friend in reach, by an item that can be aimed at one
    if (need == Need::Heal && situation.healthPct >= HEAL_HEALTH_PCT)
    {
        if (shape != Shape::Friend && shape != Shape::AnyUnit)
            return false;
        std::int8_t const index = HurtFriend(situation, entry.minRangeYards, UnitReach(entry.rangeFriendYards));
        if (index < 0)
            return false;
        target = UnitTarget(situation.units[static_cast<std::size_t>(index)]);
        return true;
    }

    if (SelfNeed(need))
    {
        if (need == Need::Decoy && shape == Shape::Ground)
            return DecoyTarget(situation, entry, target);
        if (shape == Shape::Ground && !InRange(0.0f, entry.minRangeYards, entry.rangeYards))
            return false;
        target = SelfTarget(situation, shape);
        return true;
    }

    if (need == Need::DamageOne || need == Need::Control)
    {
        std::int8_t const index = need == Need::DamageOne ? DamageTarget(situation) : ControlTarget(situation);
        if (index < 0)
            return false;
        NearUnit const& unit = situation.units[static_cast<std::size_t>(index)];
        if (need == Need::Control && unit.casts && !entry.stopsCast)
            return false;  // WI18: a root holds a runner, never a caster
        float const distance = BotDistance(situation, unit);
        if (shape == Shape::Ground)
        {
            if (!InRange(distance, entry.minRangeYards, entry.rangeYards))
                return false;
            target = SpotTarget(unit.x, unit.y, unit.z);
            return true;
        }
        if (!InRange(distance, entry.minRangeYards, UnitReach(entry.rangeYards)))
            return false;
        target = UnitTarget(unit);
        return true;
    }

    if (need == Need::DamageArea)
    {
        AimItem const item = AimItemOf(entry);
        AimSpots const spots = AimCandidates(situation, item);
        std::int8_t const pick = AimPick(spots);
        if (pick < 0)
            return false;
        AimSpot const& spot = spots.spots[static_cast<std::size_t>(pick)];
        if (!AreaWorth(situation, item, spot))
            return false;
        hits = spot.covers;
        if (item.selfCentred)
            target = SelfTarget(situation, shape);
        else if (item.onUnits)
            target = UnitTarget(situation.units[static_cast<std::size_t>(spot.unit)]);
        else
            target = SpotTarget(spot.x, spot.y, spot.z);
        return true;
    }

    // Resurrect: the nearest dead group member in reach
    std::int8_t const index = DeadFriend(situation, entry.minRangeYards, UnitReach(entry.rangeFriendYards));
    if (index < 0)
        return false;
    target = UnitTarget(situation.units[static_cast<std::size_t>(index)]);
    return true;
}

// The spend rule's case for a consumed entry in this situation
inline SpendCase SpendCaseFor(Situation const& situation, UsableEntry const& entry, float danger, std::uint8_t hits,
                              bool freeReadySameNeed)
{
    SpendCase spendCase;
    spendCase.kind = entry.kind;
    spendCase.rank = entry.rank;
    spendCase.replaceCopper = entry.replaceCopper;
    spendCase.potion = entry.potion;
    spendCase.potionDrunk = situation.potionDrunk;
    spendCase.freeReadySameNeed = freeReadySameNeed;
    spendCase.danger = danger;
    spendCase.minutesSaved = 0.0f;  // the verdict is death and stake, but for a chase spared (WI18)
    if (entry.kind == Kind::Control)
    {
        std::int8_t const index = OfferDetail::ControlTarget(situation);
        if (index >= 0 && situation.units[static_cast<std::size_t>(index)].flees)
            spendCase.minutesSaved = RUNNER_MINUTES;
    }
    spendCase.hits = hits;
    spendCase.stake = situation.stake;
    spendCase.won = OfferDetail::Won(situation);
    spendCase.deathCopper = DeathCopper(situation.earnCopperPerMinute, situation.repairCopper);
    spendCase.earnCopperPerMinute = situation.earnCopperPerMinute;
    spendCase.thrift = situation.thrift;
    spendCase.daring = situation.daring;
    spendCase.control = situation.control;
    return spendCase;
}

// Which kind is wanted now and which item answers it best; see the header comment. Entries past index 255 are not
// read (usable is one byte).
inline Offer MakeOffer(Situation const& situation, std::span<UsableEntry const> list)
{
    using namespace OfferDetail;
    Offer offer;
    float const danger = Danger(situation);
    NeedMask const wanted = WantedNeeds(situation, danger);
    if (wanted == 0)
        return offer;

    std::size_t const size = std::min<std::size_t>(list.size(), 256);
    bool keepSet = false;
    std::array<SpendCase, NEED_COUNT> potionCases{};
    std::array<Offer, NEED_COUNT> potionOffers{};
    std::size_t potions = 0;

    for (std::size_t n = 0; n < NEED_COUNT; ++n)
    {
        Need const need = static_cast<Need>(n);
        if (!(wanted & NeedBit(need)))
            continue;

        // free first: the strongest free ready answer
        std::size_t freeBest = size;
        Target freeTarget;
        for (std::size_t i = 0; i < size; ++i)
        {
            UsableEntry const& entry = list[i];
            if (entry.consumed || !EntryUsable(situation, entry) || !EntryAnswers(entry, need))
                continue;
            if (freeBest < size && !StrongerThan(entry, i, list[freeBest], freeBest))
                continue;
            Target target;
            std::uint8_t hits = 1;
            if (!AnswerTarget(situation, entry, need, target, hits))
                continue;
            freeBest = i;
            freeTarget = target;
        }
        if (freeBest < size)
        {
            if (potions > 0)
                continue;  // an earlier need's potion takes the turn
            offer.any = true;
            offer.need = need;
            offer.usable = static_cast<std::uint8_t>(freeBest);
            offer.target = freeTarget;
            offer.consumed = false;
            offer.verdict = SpendVerdict{};
            return offer;
        }

        // consumed, a non-potion first, each strongest first: the first the spend rule spends
        std::size_t previous = size;
        while (true)
        {
            std::size_t next = size;
            for (std::size_t i = 0; i < size; ++i)
            {
                UsableEntry const& entry = list[i];
                if (!entry.consumed || !EntryUsable(situation, entry) || !EntryAnswers(entry, need))
                    continue;
                if (previous < size && !ConsumedBefore(list[previous], previous, entry, i))
                    continue;  // already weighed, or before the one weighed last
                if (next < size && !ConsumedBefore(entry, i, list[next], next))
                    continue;
                next = i;
            }
            if (next >= size)
                break;
            previous = next;

            UsableEntry const& entry = list[next];
            Target target;
            std::uint8_t hits = 1;
            if (!AnswerTarget(situation, entry, need, target, hits))
                continue;
            SpendCase const spendCase = SpendCaseFor(situation, entry, danger, hits, false);
            SpendVerdict const verdict = Spend(spendCase);
            if (!verdict.spend)
            {
                if (!keepSet)
                {
                    keepSet = true;
                    offer.need = need;
                    offer.usable = static_cast<std::uint8_t>(next);
                    offer.consumed = true;
                    offer.verdict = verdict;
                }
                continue;
            }

            Offer found;
            found.any = true;
            found.need = need;
            found.usable = static_cast<std::uint8_t>(next);
            found.target = target;
            found.consumed = true;
            found.verdict = verdict;
            if (entry.potion)
            {
                potionCases[potions] = spendCase;
                potionOffers[potions] = found;
                ++potions;
            }
            else if (potions == 0)
                return found;
            break;
        }
    }

    if (potions > 0)
    {
        std::size_t const best = BestPotionNeed(std::span<SpendCase const>(potionCases.data(), potions));
        return potionOffers[best < potions ? best : 0];
    }
    offer.any = false;
    offer.target = Target{};
    if (!keepSet)
    {
        offer.need = Need::Heal;
        offer.usable = 0;
        offer.consumed = false;
        offer.verdict = SpendVerdict{};
    }
    return offer;
}
}  // namespace ItemUse

#endif
