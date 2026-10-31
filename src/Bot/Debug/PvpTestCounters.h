/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPTESTCOUNTERS_H
#define _PLAYERBOT_PVPTESTCOUNTERS_H

#include <array>

#include "Common.h"

// Counters of a bot while a harness match runs (PlayerbotAI::SetPvpTestCounting). The bot fills them, the record
// writer (PvpTestRecord) reads them; both include this header.

// Crowd control by diminishing-returns group: the indices are the core's DiminishingGroup values
constexpr uint32 PVPTEST_DR_GROUP_COUNT = 21;

constexpr std::array<char const*, PVPTEST_DR_GROUP_COUNT> PVPTEST_DR_GROUP_NAMES = {
    "none",   "banish",      "charge",     "openingStun", "controlledStun", "controlledRoot", "cyclone",
    "disarm", "disorient",   "entrapment", "fear",        "horror",         "mindControl",    "root",
    "stun",   "scatterShot", "silence",    "sleep",       "taunt",          "limitOnly",      "dragonsBreath"};

// The groups that count as crowd control: all but none, charge, taunt and the limit-only group
constexpr bool IsPvpTestCcGroup(uint32 group)
{
    return group > 0 && group < PVPTEST_DR_GROUP_COUNT && group != 2 && group != 18 && group != 19;
}

struct PvpTestCcCounters
{
    // CC casts that succeeded and CC auras of an enemy that landed on the bot, by DR group
    std::array<uint32, PVPTEST_DR_GROUP_COUNT> Cast{};
    std::array<uint32, PVPTEST_DR_GROUP_COUNT> Received{};
    // CC casts on a player target the core's DR state already held at immunity
    uint32 AtImmune = 0;
    // Damage of the bot or its pet on an enemy in a breakable CC of its team, one per CC aura (G4b)
    uint32 OwnBreaks = 0;
    // G5a: the bot's PvP trinket uses and racial breaks (every man for himself, will of the forsaken, escape artist),
    // and its dispels of a partner's CC (dispel cc on party); LostEarly is written as 0 until a later step
    uint32 Trinket = 0;
    uint32 RacialBreak = 0;
    uint32 PartnerDispel = 0;
    uint32 LostEarly = 0;
};

// Named counters each tactic adds through PlayerbotAI::CountTactic. Later steps add entries before Count and a name
// in the same place of the name table.
enum class TacticCounter : uint8
{
    FocusSame,     // G1b: per plan-interval sample, the bot's target is the skull (or, without marks, a partner's)
    FocusDiff,     // G1b: the same sample, another target
    PlanSwitch,    // G1b: a new skull after the match's first marks
    ImmuneSwapMs,  // G1b: ms from the skull seen immune to the new skull (summed)
    HoldSwitch,    // G1b: switches the hold allowed (any verdict, G1b-c) and the bot took
    CcOnTarget,    // G4b: CC casts (a CC DR group) that went on the planned CC target (arena: moon; else the kept one)
    ChainGapMs,    // G4c: |landing - end of the team's previous loss-of-control CC| of a CC the bot picked as a chain
    Chains,        // G4c: those landings (the mean gap is chainGapMs / chains; G4c-d: picked chains only)
    CcHeldMs,      // G4c: ms the bot's loss-of-control CC stayed on enemy players (the CC uptime)
    OwnBreakAoe,   // G4c-b: own breaks by an area, cone or chain spell
    OwnBreakDot,   // G4c-b: own breaks by a periodic damage tick
    OwnBreakPet,   // G4c-b: own breaks by the bot's pet
    OppTrinkets,   // G4c-b: enemy trinkets (early CC ends) the bot's opponent memory learned
    OppDispels,    // G4c-b: enemy dispels of the team's CC the bot's opponent memory learned
    ChainEarlyMs,  // G4c-d: the overlap of those chains (landing before the end), summed: the signed gap sum is
                   // chainGapMs - 2 * chainEarlyMs
    ChainBusy,     // G4c-d: chains whose bot was still casting at the chain's moment
    ReCcs,         // G4c-d: the bot's other loss-of-control landings that follow the team's CC (a re-CC of a freed
                   // target, a rotation, racial or pet stun)
    ReCcGapMs,     // G4c-d: their |gap|, summed
    BreakWasted,   // G5a: the bot's breaks of a CC on it (pvp trinket, racials) that did not matter (CcMatters false:
                   // the wasted-break roll of a low skill)
    BreakMs,       // G5a: the CC's age (ms since it landed) at those breaks and the ones that mattered, summed (the
                   // mean is breakMs / (cc.trinket + cc.racialBreak))
    PartnerDispelMs,    // G5a: the CC's age at the bot's dispels of a partner's CC (cc.partnerDispel), summed
    IntHeal,            // G5b: the bot's interrupts (a kick, a silence, a stun on a cast bar) of a heal
    IntCc,              // G5b: of a crowd control cast
    IntDamage,          // G5b: of a damage cast
    IntLate,            // G5b: heal interrupts at or after ArenaInterrupt::LATE_SHARE_PCT of the bar
    IntWasted,          // G5b: kicks at an enemy with no cast bar left (a fake or a finished cast), and damage
                        // interrupts while another enemy in spell range and sight cast a heal
    IntHealDone,        // G5b: enemy heals the bot saw finish (the opponent memory's casts seen to their end), counted
                        // once per heal by the lowest-guid bot of the group alive in the map
    SkullOutOfReachMs,  // G1b-d: ms the bot's target was the skull and out of its reach (melee range for a melee bot,
                        // spell range else) or out of sight, while Hold is active
    IntHold,            // G5c-c: damage casts the bot let go for an expected cast heal (HoldPolicy 1)
    IntHoldEpisodes,    // G5c-c: hold episodes (from the first held cast to its reset, a heal kick or the match end)
    IntHoldPaid,        // G5c-c: episodes that ended with a heal kicked
    IntHoldVain,        // G5c-c: episodes that ended with no heal bar of the healer seen (instants only or none)
    IntHoldExpired,     // G5c-c: episodes that ran past the bot's longest hold (its damage casts kicked again)
    FocusKick,          // G4e: interrupts on the bot's focus target (not its target)
    FocusCc,            // G4e: crowd control on the bot's focus target (not its target)
    SwitchKick,         // G4e: interrupts on an enemy that was neither the bot's target nor its focus
    Count
};

constexpr uint32 PVPTEST_TACTIC_COUNT = static_cast<uint32>(TacticCounter::Count);

constexpr std::array<char const*, PVPTEST_TACTIC_COUNT> PVPTEST_TACTIC_NAMES = {
    "focusSame",   "focusDiff",   "planSwitch",      "immuneSwapMs",      "holdSwitch",  "ccOnTarget",
    "chainGapMs",  "chains",      "ccHeldMs",        "ownBreakAoe",       "ownBreakDot", "ownBreakPet",
    "oppTrinkets", "oppDispels",  "chainEarlyMs",    "chainBusy",         "reCcs",       "reCcGapMs",
    "breakWasted", "breakMs",     "partnerDispelMs", "intHeal",           "intCc",       "intDamage",
    "intLate",     "intWasted",   "intHealDone",     "skullOutOfReachMs", "intHold",     "intHoldEpisodes",
    "intHoldPaid", "intHoldVain", "intHoldExpired",  "focusKick",         "focusCc",     "switchKick"};

#endif
