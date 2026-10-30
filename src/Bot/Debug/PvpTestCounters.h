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
    // Filled by later steps (G4/G5), written as 0 until then
    uint32 OwnBreaks = 0;
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
    Count
};

constexpr uint32 PVPTEST_TACTIC_COUNT = static_cast<uint32>(TacticCounter::Count);

constexpr std::array<char const*, PVPTEST_TACTIC_COUNT> PVPTEST_TACTIC_NAMES = {"focusSame", "focusDiff", "planSwitch",
                                                                                "immuneSwapMs", "holdSwitch"};

#endif
