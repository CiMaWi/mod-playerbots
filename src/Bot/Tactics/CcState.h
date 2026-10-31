/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CCSTATE_H
#define _PLAYERBOT_CCSTATE_H

#include <array>
#include <ctime>
#include <vector>

#include "ArenaCc.h"
#include "Common.h"
#include "ObjectGuid.h"

// One bot's crowd-control state (G4b, PLAN 3.15): the cached CcParams, what it remembers of the CC it saw land
// (DrMemory, fed by the aura hooks of the bots of the caster's group on the same map), its own rank-resolved spell
// table, the ready mask and the trigger's last answer. Touched by the bot's own update and by the aura hooks on the
// bot's map thread; the GM commands read and force it on the world thread while the maps wait (C3). GUIDs only.

constexpr uint32 CC_READY_REFRESH_MS = 250;     // the ready mask and the trigger's answer live this long
constexpr uint32 CC_TABLE_REFRESH_MS = 60000;   // the table is also rebuilt this often (a respec at the same level)
constexpr uint32 CC_TARGET_KEEP_MS = 3000;      // a CC target outside a moon is kept this long, so it does not flip
constexpr uint32 CC_FAILED_BACKOFF_MS = 1500;   // a row whose cast failed waits this long
constexpr uint32 CC_FORCE_MS = 10000;           // tactics force cc (G4b-b)
constexpr uint32 CC_EXCLUDE_REFRESH_MS = 250;   // the OwnCc exclusion list lives this long (G4b-b)
constexpr uint8 CC_KILL_WINDOW_PCT = 50;        // the bot's target at or below it opens the kill window (CcHolds)
constexpr uint32 CC_CHAIN_WINDOW_MS = 3000;     // a team CC landing at most this long after the last one ended chains
constexpr uint32 CC_CHAIN_LANDING_MS = 5000;    // G4c-d: the bot's team cc aura landing this soon after its cast is it
constexpr uint32 CC_CHAIN_REACT_CAP_MS = 2000;  // G4c-d: a chain's delay from its moment to the cast: longer is stale
constexpr uint32 CC_CHAIN_GUARD_KEEP_MS = 500;  // G4c-d: the busy guard lasts this long after cc ready's last look
constexpr uint32 CC_CHAIN_AFTER_MIN_MS = 50;    // G4c-d: a chain onto the team's cyclone lands this long after its end
constexpr uint32 CC_CHAIN_AFTER_SPREAD_MS = 100;  // plus up to this (noise per chain)
constexpr uint32 CC_BREAK_REFRESH_MS = 250;       // G5a: cc matters' answer lives this long
constexpr uint32 CC_DISPEL_REFRESH_MS = 250;      // G5a: dispel cc on party's pick lives this long
constexpr uint32 CC_DISPEL_MIN_LEFT_MS = 700;     // G5a: a CC with less left is not dispelled (DispelAuraDuration)
constexpr uint32 CC_FORCE_STUN_SPELL = 10308;     // G5a tactics force stun: hammer of justice rank 4 (6 s, magic)
constexpr uint32 CC_DISPEL_CONFIRM_MS = 1000;     // G5a review: a dispel's CC going this long after the cast counts

struct CcBotState
{
    CcParams Params;  // cached by PlayerbotAI::SetPvpSkill (ComputeCcParams)
    DrMemory Memory;
    // The bot's table (ArenaCcSpells.h BuildCcBotTable): per row the highest rank it knows (0 = none) and its numbers;
    // rebuilt when the level or the free talent points change, and every CC_TABLE_REFRESH_MS
    std::array<uint32, CC_OPTION_COUNT> KnownSpell{};
    CcSpellTable Spells{};
    CcRowMask KnownMask = 0;
    bool TableBuilt = false;
    uint8 TableLevel = 0;
    uint32 TableTalentPoints = 0;
    uint32 TableBuiltMs = 0;
    // Known, off cooldown, combo points for a finisher; FailedMask's rows wait until FailedUntilMs
    CcRowMask ReadyMask = 0;
    CcRowMask FailedMask = 0;
    uint32 FailedUntilMs = 0;
    // The CC target outside a moon (battleground, world, an arena without one): the strongest other enemy
    ObjectGuid CcTarget;
    uint32 CcTargetUntilMs = 0;
    // cc ready's answer until PickUntilMs: the row, the unit it goes on and the planned CC target (they differ on a
    // wrong-target window)
    int8 PickRow = CC_NO_ROW;
    bool PickIsChain = false;  // the pick is a chain (the team's CC on the target): only its landing counts as one
    ObjectGuid PickTarget;
    ObjectGuid PickCcTarget;
    uint32 PickUntilMs = 0;
    uint32 ReadySinceMs = 0;  // a CC ready with a target since (the patient hold); 0 = not ready
    uint32 Casts = 0;         // team cc casts started (tactics show)
    // tactics force cc (G4b-b): this enemy counts as held in a team breakable CC until ForcedCcUntilMs
    ObjectGuid ForcedCcGuid;
    uint32 ForcedCcUntilMs = 0;
    // OwnCc (G4b-b): the enemies the bot keeps out of its dps, debuff and AoE targets (the team's breakable CC on
    // them, or forced) until ExcludedUntilMs; the bot's own update only
    std::vector<ObjectGuid> Excluded;
    uint32 ExcludedUntilMs = 0;
    // cc.ownBreaks: the team CC aura the bot's damage last hit (one count per aura; compared, never dereferenced)
    void const* OwnBreakAura = nullptr;
    time_t OwnBreakApplyTime = 0;
    // G4c chains: when the team's CC on each enemy ends (the aura hooks of the team's bots on the map), the bot's
    // chained landings and the last one's signed gap (tactics show; the harness counts chainGapMs and chains)
    CcChainClock Chain;
    uint32 ChainsLanded = 0;
    int32 LastChainGapMs = 0;
    uint32 ChainWaits = 0;  // cc ready answers that waited for the chain's moment
    // G4c-d chain timing: the bot's learned delay from a chain's moment to its cast (CcLearnChainReactMs; the
    // reaction model's median until the first sample), the moment its chain pick became due, the moment cc ready
    // waits for and how long the busy guard (cc chain guard) holds no cast that ends after it, the end of the team's
    // cyclone (plus AfterMs) the pick lands after; whether a cast of its own delayed the chain or the cyclone held it
    uint32 ChainReactMs = 0;
    bool ChainReactLearned = false;
    uint32 ChainDueSinceMs = 0;
    uint32 ChainDueAtMs = 0;
    uint32 ChainGuardUntilMs = 0;
    uint32 ChainImmuneEndMs = 0;
    bool ChainBusySeen = false;
    bool ChainHeldImmune = false;
    uint32 ChainGuardMutes = 0;  // action evaluations the busy guard muted (tactics show)
    // G4c-d: the bot's last team cc cast (its aura spell, start, and whether it was a chain pick and was delayed by a
    // cast of the bot's own): only its landing counts as a chain (tactics.chains); other landings are re-CCs
    uint32 CastAuraSpell = 0;
    uint32 CastAtMs = 0;
    bool CastWasChain = false;
    bool CastWasBusy = false;
    bool TeamCcCasting = false;  // G4d-c: inside team cc's CastSpell (the CC immune line names the tactic's casts)
    uint32 ReCcsLanded = 0;
    // G4c-b: what the bot saw of each enemy's dispels and trinkets of the team's CC this match (the aura hooks of the
    // team's bots on the map, Cc.OpponentMemoryEnable); the pets and guardians called off a kept-out enemy
    CcOpponentMemory Opponents;
    uint32 PetCallOffs = 0;
    // G5a Trinket: cc matters' answer until BreakUntilMs: the breaker (None: no CC on the bot to break now), whether
    // the CC mattered (CcMatters; else a wasted break), its kind, aura spell, age and time left; the bot's breaks and
    // wasted ones, the last break's CC age (tactics show)
    CcBreaker BreakWith = CcBreaker::None;
    bool BreakMatters = false;
    CcKind BreakKind = CcKind::Stun;
    uint32 BreakAuraSpell = 0;
    uint32 BreakAuraAgeMs = 0;
    uint32 BreakLeftMs = 0;
    uint32 BreakUntilMs = 0;
    uint32 Breaks = 0;
    uint32 WastedBreaks = 0;
    uint32 LastBreakAgeMs = 0;
    // G5a Dispel: dispel cc on party's pick until DispelUntilMs: the party member (self first), the CC aura's spell,
    // the spell that removes it; the bot's dispels of a partner's CC and the last one's CC age (tactics show)
    ObjectGuid DispelTarget;
    uint32 DispelAuraSpell = 0;
    char const* DispelSpell = nullptr;
    uint32 DispelUntilMs = 0;
    uint32 Dispels = 0;
    uint32 LastDispelAgeMs = 0;
    // The last cast, until its CC goes by a dispel (CcNoteDispelRemoval counts and logs it there) or a later cast
    CcPendingDispel PendingDispel;
    // Avoid.EnsureTrinket (harness matches, the world thread at the fight start): the PvP trinket the bot was given
    // and the one it replaced in that slot (both back by CcRestoreTrinket: counting stops, the battleground leave, the
    // logout; a dead bot waits until it is alive); or the PvP trinket withheld for the match (2)
    ObjectGuid EnsuredTrinket;
    ObjectGuid EnsuredReplaced;
    uint8 EnsuredSlot = 0;
    bool TrinketWithheld = false;
};

// Whether an enemy stays out of the bot's targets (OwnCc): a forced CC always; a team breakable CC unless the bot's
// own-break window (CcBreaksOwn, low skill) lets it break it
inline bool CcKeepsOut(bool heldByTeam, bool forced, bool breaksOwn) { return forced || (heldByTeam && !breaksOwn); }

class SpellInfo;
class Unit;

// G4c-d review: whether the target is immune to the spell by anything besides its cyclone's immunities (a spell id,
// dispel type, mechanic or aura state immunity: Berserker Rage, Lichborne, Icebound Fortitude); the chain through
// the team's cyclone (PlayerbotAI::CanCastSpell) still refuses then. CcTacticStrategy.cpp
bool CcImmuneBesidesCyclone(Unit const* target, SpellInfo const* spellInfo);

#endif
