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
    // and its dispels of a partner's CC (dispel cc on party)
    uint32 Trinket = 0;
    uint32 RacialBreak = 0;
    uint32 PartnerDispel = 0;
};

// Named counters each tactic adds through PlayerbotAI::CountTactic, one list for the enum and the record's names:
// X(enumerator, record name). Adding a counter: one entry at the end of the list.
// clang-format off
#define PVPTEST_TACTIC_COUNTERS(X) \
    /* G1b: per plan-interval sample, the bot's target is the skull (or, without marks, a partner's) */ \
    X(FocusSame, "focusSame") \
    /* G1b: the same sample, another target */ \
    X(FocusDiff, "focusDiff") \
    /* G1b: a new skull after the match's first marks */ \
    X(PlanSwitch, "planSwitch") \
    /* G1b: ms from the skull seen immune to the new skull (summed) */ \
    X(ImmuneSwapMs, "immuneSwapMs") \
    /* G1b: switches the hold allowed (any verdict, G1b-c) and the bot took */ \
    X(HoldSwitch, "holdSwitch") \
    /* G4b: CC casts (a CC DR group) that went on the planned CC target (arena: moon; else the kept one) */ \
    X(CcOnTarget, "ccOnTarget") \
    /* G4c: |landing - end of the team's previous loss-of-control CC| of a CC the bot picked as a chain */ \
    X(ChainGapMs, "chainGapMs") \
    /* G4c: those landings (the mean gap is chainGapMs / chains; G4c-d: picked chains only) */ \
    X(Chains, "chains") \
    /* G4c: ms the bot's loss-of-control CC stayed on enemy players (the CC uptime) */ \
    X(CcHeldMs, "ccHeldMs") \
    /* G4c-b: own breaks by an area, cone or chain spell */ \
    X(OwnBreakAoe, "ownBreakAoe") \
    /* G4c-b: own breaks by a periodic damage tick */ \
    X(OwnBreakDot, "ownBreakDot") \
    /* G4c-b: own breaks by the bot's pet */ \
    X(OwnBreakPet, "ownBreakPet") \
    /* G4c-b: enemy trinkets (early CC ends) the bot's opponent memory learned */ \
    X(OppTrinkets, "oppTrinkets") \
    /* G4c-b: enemy dispels of the team's CC the bot's opponent memory learned */ \
    X(OppDispels, "oppDispels") \
    /* G4c-d: the overlap of those chains (landing before the end), summed: the signed gap sum is chainGapMs - 2 * \
       chainEarlyMs */ \
    X(ChainEarlyMs, "chainEarlyMs") \
    /* G4c-d: chains whose bot was still casting at the chain's moment */ \
    X(ChainBusy, "chainBusy") \
    /* G4c-d: the bot's other loss-of-control landings that follow the team's CC (a re-CC of a freed target, a \
       rotation, racial or pet stun) */ \
    X(ReCcs, "reCcs") \
    /* G4c-d: their |gap|, summed */ \
    X(ReCcGapMs, "reCcGapMs") \
    /* G5a: the bot's breaks of a CC on it (pvp trinket, racials) that did not matter (CcMatters false: the \
       wasted-break roll of a low skill) */ \
    X(BreakWasted, "breakWasted") \
    /* G5a: the CC's age (ms since it landed) at those breaks and the ones that mattered, summed (the mean is \
       breakMs / (cc.trinket + cc.racialBreak)) */ \
    X(BreakMs, "breakMs") \
    /* G5a: the CC's age at the bot's dispels of a partner's CC (cc.partnerDispel), summed */ \
    X(PartnerDispelMs, "partnerDispelMs") \
    /* G5b: the bot's interrupts (a kick, a silence, a stun on a cast bar) of a heal */ \
    X(IntHeal, "intHeal") \
    /* G5b: of a crowd control cast */ \
    X(IntCc, "intCc") \
    /* G5b: of a damage cast */ \
    X(IntDamage, "intDamage") \
    /* G5b: heal interrupts at or after ArenaInterrupt::LATE_SHARE_PCT of the bar */ \
    X(IntLate, "intLate") \
    /* G5b: kicks at an enemy with no cast bar left (a fake or a finished cast), and damage interrupts while \
       another enemy in spell range and sight cast a heal */ \
    X(IntWasted, "intWasted") \
    /* G5b: enemy heals the bot saw finish (the opponent memory's casts seen to their end), counted once per heal \
       by the lowest-guid bot of the group alive in the map */ \
    X(IntHealDone, "intHealDone") \
    /* G1b-d: ms the bot's target was the skull and out of its reach (melee range for a melee bot, spell range \
       else) or out of sight, while Hold is active */ \
    X(SkullOutOfReachMs, "skullOutOfReachMs") \
    /* G5c-c: damage casts the bot let go for an expected cast heal (HoldPolicy 1) */ \
    X(IntHold, "intHold") \
    /* G5c-c: hold episodes (from the first held cast to its reset, a heal kick or the match end) */ \
    X(IntHoldEpisodes, "intHoldEpisodes") \
    /* G5c-c: episodes that ended with a heal kicked */ \
    X(IntHoldPaid, "intHoldPaid") \
    /* G5c-c: episodes that ended with no heal bar of the healer seen (instants only or none) */ \
    X(IntHoldVain, "intHoldVain") \
    /* G5c-c: episodes that ran past the bot's longest hold (its damage casts kicked again) */ \
    X(IntHoldExpired, "intHoldExpired") \
    /* G4e: interrupts on the bot's focus target (not its target) */ \
    X(FocusKick, "focusKick") \
    /* G4e: crowd control on the bot's focus target (not its target) */ \
    X(FocusCc, "focusCc") \
    /* G4e: interrupts on an enemy that was neither the bot's target nor its focus */ \
    X(SwitchKick, "switchKick") \
    /* G5d: self-defensive cooldowns pressed by the Defensive tactic */ \
    X(DefSelf, "defSelf") \
    /* G5d: defensives pressed on a partner (blessing of protection, pain suppression, ...) */ \
    X(DefPartner, "defPartner") \
    /* G5d: bursts the sampler detected */ \
    X(DefBurst, "defBurst") \
    /* G5d: the bot died with a self-defensive ready (known, not on cooldown) */ \
    X(DeathsDefReady, "deathsDefReady") \
    /* G5e: casts the bot cancelled on purpose as a bait */ \
    X(Fakes, "fakes") \
    /* G5e: an enemy kick within the baited window after a fake cancel */ \
    X(KicksBaited, "kicksBaited") \
    /* G5e: enemy kicks seen used (the kick memory) */ \
    X(KickSeen, "kickSeen") \
    /* G2a: natural turn flicks started */ \
    X(TurnFlicks, "turnFlicks") \
    /* G2a: ms a cast or swing waited for the bot's facing arc to hold */ \
    X(TurnWaitMs, "turnWaitMs") \
    /* G2a: casts that failed for the facing arc */ \
    X(FaceFails, "faceFails") \
    /* G2b: chase destinations set from the lagged, predicted point */ \
    X(TrackChases, "trackChases") \
    /* G2b: the lag applied, summed over the chases (mean = trackLagMs / trackChases) */ \
    X(TrackLagMs, "trackLagMs") \
    /* G2c: paths whose interior waypoints were offset sideways */ \
    X(PathOffsets, "pathOffsets") \
    /* G2c: strafe legs started */ \
    X(PathStrafes, "pathStrafes") \
    /* G2c: cosmetic hops made */ \
    X(PathHops, "pathHops") \
    /* G2c: hops refused by the guards (edge, void, arc ray, landing) */ \
    X(PathHopsRefused, "pathHopsRefused") \
    /* G2e: regains of LOS started */ \
    X(LosRegains, "losRegains") \
    /* G2e: regains not finished within TimeoutMs */ \
    X(LosTimeouts, "losTimeouts") \
    /* G2e: ms a healer could not see an alive partner (both sides count it) */ \
    X(PartnerLosLostMs, "partnerLosLostMs") \
    /* G2f: LOS breaks from an enemy cast started */ \
    X(LosBreaks, "losBreaks") \
    /* G2f: enemy casts that failed for the lost LOS */ \
    X(CastsLosBroken, "castsLosBroken") \
    /* G2f: pillar hops made */ \
    X(PillarSteps, "pillarSteps") \
    /* G2e/G2f: relocations the guards refused (hold time, relocations per window) */ \
    X(RelocGuards, "relocGuards") \
    /* G3a: kite steps issued */ \
    X(KiteSteps, "kiteSteps") \
    /* G3a: sum of the distance x10 from a healer/caster to the nearest melee on it (1 sample/s) */ \
    X(MeleeDistX10, "meleeDistX10") \
    /* G3a: those samples */ \
    X(MeleeDistN, "meleeDistN") \
    /* G3a: ms a melee was within the melee distance */ \
    X(MeleeNearMs, "meleeNearMs") \
    /* G3a: kite steps that were a snare */ \
    X(KiteSnares, "kiteSnares") \
    /* G3a: kite steps that were a pillar step */ \
    X(KitePillars, "kitePillars") \
    /* G3a: kite steps towards the partner */ \
    X(KitePartner, "kitePartner") \
    /* G3a: kite steps that were a jump */ \
    X(KiteJumps, "kiteJumps") \
    /* G3a: FleeAction executions of a bot in a counted match */ \
    X(FleeRuns, "fleeRuns") \
    /* G3b: drinks started */ \
    X(Drinks, "drinks") \
    /* G3b: of them in preparation */ \
    X(PrepDrinks, "prepDrinks") \
    /* G3b: drinks broken (enemy, combat, partner low) */ \
    X(DrinkBroken, "drinkBroken") \
    /* G3b: a drink wanted, none to use */ \
    X(DrinkNoItem, "drinkNoItem") \
    /* G2g: tactical jumps made */ \
    X(Jumps, "jumps") \
    /* G2g: of them to escape */ \
    X(JumpsEscape, "jumpsEscape") \
    /* G2g: of them to chase */ \
    X(JumpsChase, "jumpsChase") \
    /* G2g: jumps refused by the guards */ \
    X(JumpsRefused, "jumpsRefused") \
    /* G2g: landings below the spot's floor - 2 yd */ \
    X(JumpFalls, "jumpFalls")

enum class TacticCounter : uint8
{
#define PVPTEST_TACTIC_ENUM(name, key) name,
    PVPTEST_TACTIC_COUNTERS(PVPTEST_TACTIC_ENUM)
#undef PVPTEST_TACTIC_ENUM
    Count
};

constexpr uint32 PVPTEST_TACTIC_COUNT = static_cast<uint32>(TacticCounter::Count);

constexpr std::array<char const*, PVPTEST_TACTIC_COUNT> PVPTEST_TACTIC_NAMES = {
#define PVPTEST_TACTIC_NAME(name, key) key,
    PVPTEST_TACTIC_COUNTERS(PVPTEST_TACTIC_NAME)
#undef PVPTEST_TACTIC_NAME
};
// clang-format on

// The executed actions of the notice categories, in the order of PlayerbotAI.h's SkillNoticeCounters
constexpr uint32 PVPTEST_NOTICE_CATEGORY_COUNT = 6;

// One bot's counters while a harness match runs: made by PlayerbotAI::SetPvpTestCounting(true), zeroed at each start
// and kept until logout, so a bot that never counts carries only the pointer. Read through
// PlayerbotAI::GetPvpTestCounters.
struct PvpTestCounterBlock
{
    std::array<uint32, PVPTEST_NOTICE_CATEGORY_COUNT> Executed{};
    PvpTestCcCounters Cc;
    std::array<uint32, PVPTEST_TACTIC_COUNT> Tactics{};
};

#endif
