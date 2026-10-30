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

constexpr uint32 CC_READY_REFRESH_MS = 250;    // the ready mask and the trigger's answer live this long
constexpr uint32 CC_TABLE_REFRESH_MS = 60000;  // the table is also rebuilt this often (a respec at the same level)
constexpr uint32 CC_TARGET_KEEP_MS = 3000;     // a CC target outside a moon is kept this long, so it does not flip
constexpr uint32 CC_FAILED_BACKOFF_MS = 1500;  // a row whose cast failed waits this long
constexpr uint32 CC_FORCE_MS = 10000;          // tactics force cc (G4b-b)
constexpr uint32 CC_EXCLUDE_REFRESH_MS = 250;  // the OwnCc exclusion list lives this long (G4b-b)
constexpr uint8 CC_KILL_WINDOW_PCT = 50;       // the bot's target at or below it opens the kill window (CcHolds)

struct CcBotState
{
    CcParams Params;  // cached by PlayerbotAI::SetPvpSkill (ComputeCcParams)
    DrMemory Memory;
    // The bot's table (ArenaCcSpells.h BuildCcBotTable): per row the highest rank it knows (0 = none) and its numbers;
    // rebuilt when the level or the free talent points change, and every CC_TABLE_REFRESH_MS
    std::array<uint32, CC_OPTION_COUNT> KnownSpell{};
    CcSpellTable Spells{};
    uint32 KnownMask = 0;
    bool TableBuilt = false;
    uint8 TableLevel = 0;
    uint32 TableTalentPoints = 0;
    uint32 TableBuiltMs = 0;
    // Known, off cooldown, combo points for a finisher; FailedMask's rows wait until FailedUntilMs
    uint32 ReadyMask = 0;
    uint32 FailedMask = 0;
    uint32 FailedUntilMs = 0;
    // The CC target outside a moon (battleground, world, an arena without one): the strongest other enemy
    ObjectGuid CcTarget;
    uint32 CcTargetUntilMs = 0;
    // cc ready's answer until PickUntilMs: the row, the unit it goes on and the planned CC target (they differ on a
    // wrong-target window)
    int8 PickRow = CC_NO_ROW;
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
};

// Whether an enemy stays out of the bot's targets (OwnCc): a forced CC always; a team breakable CC unless the bot's
// own-break window (CcBreaksOwn, low skill) lets it break it
inline bool CcKeepsOut(bool heldByTeam, bool forced, bool breaksOwn) { return forced || (heldByTeam && !breaksOwn); }

#endif
