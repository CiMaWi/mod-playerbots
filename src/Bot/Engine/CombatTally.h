/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COMBATTALLY_H
#define _PLAYERBOT_COMBATTALLY_H

#include "CombatTallyState.h"

// The fight tally's rules (combat-foundation CF1, second version CFM2; MEASURES §2): per bot and fight, the time of
// each combat-engine tick sorted into five shares, and one world-log pair Fight + FightMore at the fight's end
// (CombatTrace.cpp writes it). Pure: no Player, no clock of its own; the stamps are getMSTime() values, so every
// difference is an unsigned subtraction and survives its wrap. The state is in CombatTallyState.h.
namespace CombatTally
{
// What the engine knows of one combat tick; the target fields are read only when NeedsReach says so
struct TickFacts
{
    bool dead = false;
    bool lostControl = false;
    bool silenced = false;      // silenced or pacified
    bool executed = false;      // an action ran
    bool executedMove = false;  // the action that ran is a move (and not a cast)
    bool casting = false;       // a cast or channel in progress (an auto shot does not count)
    bool globalCooldown = false;
    bool hasTarget = false;
    bool targetInReach = false;
    bool sightFailed = false;    // a candidate's cast test failed for line of sight
    bool noPowerFailed = false;  // a candidate's cast failed with SPELL_FAILED_NO_POWER
    bool gateUnmet = false;      // a power gate unmet within GATE_WINDOW_MS and the power still below it
    bool otherFailed = false;    // a candidate that passed useful and possible failed for another reason
};

// Controlled and acting decide without the target: the one distance of a tick is read only when this is true
constexpr bool NeedsReach(TickFacts const& f)
{
    if (f.dead || f.lostControl || (!f.executed && f.silenced))
        return false;
    if (f.executed && !f.executedMove)
        return false;
    return !f.casting && !f.globalCooldown;
}

// MEASURES §2.2, first match wins
constexpr Share Classify(TickFacts const& f)
{
    if (f.dead || f.lostControl || (!f.executed && f.silenced))
        return Share::Controlled;
    if ((f.executed && !f.executedMove) || f.casting || f.globalCooldown)
        return Share::Acting;
    bool const inReach = f.hasTarget && f.targetInReach;
    if (f.executed)
        return inReach ? Share::Acting : Share::Reach;
    if (!inReach || f.sightFailed)
        return Share::Reach;
    if ((f.noPowerFailed || f.gateUnmet) && !f.otherFailed)
        return Share::Waiting;
    return Share::Idle;
}

// The core's Powers values (static_assert in CombatTrace.cpp)
enum class PowerKind : std::uint8_t
{
    Mana = 0,
    Rage = 1,
    Focus = 2,
    Energy = 3,
    Happiness = 4,
    Rune = 5,
    RunicPower = 6
};

constexpr std::uint32_t POWER_POINT_SCALE = 10;  // the core keeps rage and runic power in tenths of a point

constexpr std::uint32_t Percent(std::uint32_t value, std::uint32_t max)
{
    return max ? std::min<std::uint32_t>(static_cast<std::uint32_t>(std::uint64_t{value} * PERCENT / max), PERCENT) : 0;
}

// The power the gates compare: rage and runic power in points, mana in percent, the others as the core keeps them
constexpr std::uint32_t PowerPoints(PowerKind kind, std::uint32_t value, std::uint32_t max)
{
    if (kind == PowerKind::Mana)
        return Percent(value, max);
    if (kind == PowerKind::Rage || kind == PowerKind::RunicPower)
        return value / POWER_POINT_SCALE;
    return value;
}

// Power at the cap counts for rage, energy, runic power and focus; never for mana
constexpr bool AtCap(PowerKind kind, std::uint32_t value, std::uint32_t max)
{
    bool const counted = kind == PowerKind::Rage || kind == PowerKind::Energy || kind == PowerKind::RunicPower ||
                         kind == PowerKind::Focus;
    return counted && max && value >= max;
}

// An unmet power gate (a table row's PowerAtLeast term, the base's rage and energy triggers): one store per unmet
// check, the lowest amount of a tick wins; evidence of waiting while it is recent and the power still below it
constexpr std::uint32_t GATE_WINDOW_MS = 2000;

// The rows' fields: 15-bit times in tenths of a second, capped at TIME_MAX (54.6 minutes); a fight writes a running
// pair every SPLIT_MS (at the bot's update, CF.32) so no time reaches the cap
constexpr std::uint32_t TIME_BITS = 15;
constexpr std::uint64_t TIME_MAX = (std::uint64_t{1} << TIME_BITS) - 1;
constexpr std::uint32_t MS_PER_TENTH = 100;
constexpr std::uint32_t SPLIT_MS = 50 * 60 * 1000;
constexpr std::uint32_t VERSION = 1;
constexpr std::uint32_t VERSION_SHIFT = 30;
// FightMore id's mark (CF.31): the combat engine never ticked in the part
constexpr std::uint32_t UNTICKED_SHIFT = 30;
constexpr std::uint32_t PERCENT_BITS = 7;
constexpr std::uint32_t PERCENT_MASK = (1u << PERCENT_BITS) - 1;

constexpr std::uint64_t Tenths(std::uint32_t ms) { return std::min<std::uint64_t>(ms / MS_PER_TENTH, TIME_MAX); }

// A fight is the combat flag on from 0.1 s up (CF.30): a shorter part writes no pair, no combat_rows, no combat_dealt
constexpr bool Written(std::uint32_t lengthMs) { return Tenths(lengthMs) >= 1; }

// The shares in tenths: the first four rounded down, acting the rest, so the five sum to the length's tenths exactly
constexpr std::array<std::uint64_t, SHARE_COUNT> ShareTenths(Summary const& s)
{
    std::array<std::uint64_t, SHARE_COUNT> tenths{};
    std::uint64_t const length = Tenths(s.lengthMs);
    std::uint64_t used = 0;
    for (Share const share : {Share::Idle, Share::Waiting, Share::Reach, Share::Controlled})
    {
        std::uint64_t const part = std::min(Tenths(s.Of(share)), length - used);
        tenths[static_cast<std::size_t>(share)] = part;
        used += part;
    }
    tenths[static_cast<std::size_t>(Share::Acting)] = length - used;
    return tenths;
}

// Fight id: casts | most enemies << 10 | group size << 20 (the counts as PackVisitCounts packs them) | version << 30
constexpr std::uint32_t FightId(std::uint32_t counts)
{
    return (counts & ((1u << VERSION_SHIFT) - 1)) | VERSION << VERSION_SHIFT;
}

// Fight amount: length | idle << 15 | waiting << 30 | out of reach << 45, tenths
constexpr std::uint64_t FightAmount(Summary const& s)
{
    std::array<std::uint64_t, SHARE_COUNT> const t = ShareTenths(s);
    return Tenths(s.lengthMs) | t[static_cast<std::size_t>(Share::Idle)] << TIME_BITS |
           t[static_cast<std::size_t>(Share::Waiting)] << (2 * TIME_BITS) |
           t[static_cast<std::size_t>(Share::Reach)] << (3 * TIME_BITS);
}

// FightMore id: level | class << 7 | talent tab << 11 | power kind << 13 | lowest health % << 16 | final power % << 23
// | unticked << 30 (the mark; rows before it read as ticked)
constexpr std::uint32_t MoreId(std::uint32_t level, std::uint32_t cls, std::uint32_t tab, std::uint32_t power,
                               std::uint32_t lowestHealthPct, std::uint32_t finalPowerPct, bool unticked = false)
{
    return std::min<std::uint32_t>(level, 127) | (cls & 15) << 7 | (tab & 3) << 11 | (power & 7) << 13 |
           std::min(lowestHealthPct, PERCENT) << 16 | std::min(finalPowerPct, PERCENT) << 23 |
           (unticked ? 1u << UNTICKED_SHIFT : 0u);
}

// FightMore amount: controlled | acting << 15 | at the cap << 30 | health % at the pull << 45 | power % << 52
constexpr std::uint64_t MoreAmount(Summary const& s)
{
    std::array<std::uint64_t, SHARE_COUNT> const t = ShareTenths(s);
    return t[static_cast<std::size_t>(Share::Controlled)] | t[static_cast<std::size_t>(Share::Acting)] << TIME_BITS |
           std::min(Tenths(s.capMs), Tenths(s.lengthMs)) << (2 * TIME_BITS) |
           std::uint64_t{std::min(s.pullHealthPct, PERCENT)} << (3 * TIME_BITS) |
           std::uint64_t{std::min(s.pullPowerPct, PERCENT)} << (3 * TIME_BITS + PERCENT_BITS);
}

// One 15-bit time field of an amount (index 0-3), in tenths
constexpr std::uint32_t UnpackTime(std::uint64_t amount, std::uint32_t index)
{
    return static_cast<std::uint32_t>((amount >> (index * TIME_BITS)) & TIME_MAX);
}
}  // namespace CombatTally

#endif
