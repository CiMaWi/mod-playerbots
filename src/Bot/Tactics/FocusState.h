/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FOCUSSTATE_H
#define _PLAYERBOT_FOCUSSTATE_H

#include <array>

#include "ArenaFocus.h"
#include "Common.h"
#include "ObjectGuid.h"

// One bot's state of the target hold and the team plan (G1b, PLAN 3.15), owned by its PlayerbotAI
// (PlayerbotAI::GetFocusState). Touched by the bot's own update on its map thread; the GM commands (tactics show,
// tactics force) read or write it on the world thread while the map threads wait, as SetPvpSkill does. There is no
// team object: the team's only shared channel is its group's target icons (DT.1), skull = kill, moon = control.

constexpr uint8 FOCUS_ICON_MOON = 4;                // RtiTargetValue::GetRtiIndex("moon")
constexpr uint8 FOCUS_ICON_SKULL = 7;               // RtiTargetValue::GetRtiIndex("skull")
constexpr uint32 FOCUS_FORCE_IMMUNE_MS = 10000;     // tactics force immune: how long the enemy counts as immune
constexpr uint32 FOCUS_MIN_PLAN_INTERVAL_MS = 100;  // floor of the plan cadence (a config of 0 plans every 100 ms)
constexpr uint32 FOCUS_SIGHT_CHECK_MS = 250;        // the hold's line-of-sight check on its target, at most this often
constexpr uint32 FOCUS_REACH_SAMPLE_MAX_MS = 500;   // G1b-d: the longest gap one skull-reach sample counts

// The hold multiplier's answers: the HoldReason values, then the team plan's own
constexpr uint8 FOCUS_HOLD_REASONS = static_cast<uint8>(HoldReason::Count);
constexpr uint8 FOCUS_VERDICT_MARK = FOCUS_HOLD_REASONS;           // allow: a switch to the skull
constexpr uint8 FOCUS_VERDICT_MARK_HELD = FOCUS_HOLD_REASONS + 1;  // refuse: the bot is on the skull
constexpr uint8 FOCUS_VERDICT_FOLLOW = FOCUS_HOLD_REASONS + 2;     // allow: the partner's target, no marks
constexpr uint8 FOCUS_HOLD_VERDICTS = FOCUS_HOLD_REASONS + 3;
constexpr uint8 FOCUS_REMARK_REASONS = static_cast<uint8>(RemarkReason::Count);

inline char const* FocusVerdictName(uint8 verdict)
{
    switch (verdict)
    {
        case FOCUS_VERDICT_MARK:
            return "skull";
        case FOCUS_VERDICT_MARK_HELD:
            return "on skull";
        case FOCUS_VERDICT_FOLLOW:
            return "follow";
        default:
            return HoldReasonName(static_cast<HoldReason>(verdict));
    }
}

// The hold (every PvP context in the Hold tactic's scope)
struct FocusHoldState
{
    ObjectGuid HeldGuid;  // the current target as last seen, taken at HeldSinceMs
    uint32 HeldSinceMs = 0;
    ObjectGuid PendingGuid;  // the last allowed switch: its time stamps HeldSinceMs once the target is taken
    uint32 PendingAtMs = 0;
    uint8 PendingVerdict = FOCUS_HOLD_VERDICTS;
    ObjectGuid SightGuid;  // the target whose sight LastSeenMs follows
    uint32 LastSeenMs = 0;
    uint32 SightCheckMs = 0;
    ObjectGuid FollowGuid;  // the partner's target the bot has noticed since FollowSinceMs
    uint32 FollowSinceMs = 0;
    ObjectGuid LastCandidate;  // the counts below change once per candidate and answer, not per update
    uint8 LastVerdict = FOCUS_HOLD_VERDICTS;
    std::array<uint32, FOCUS_HOLD_VERDICTS> Verdicts{};
    uint32 Switches = 0;  // allowed by the hold (any verdict, G1b-c) and taken: tactics.holdSwitch
    // G1b-d: the last skull-reach sample and the ms on a skull out of range or sight while the candidate was in reach
    // (tactics.skullOutOfReachMs)
    uint32 ReachSampleMs = 0;
    uint32 SkullOutOfReachMs = 0;
};

// What the team has seen of one enemy in this match
struct FocusEnemyMemory
{
    ObjectGuid Guid;
    uint32 LastSeenMs = 0;       // the last snapshot in which an own bot saw it
    bool HealSeen = false;       // seen casting a heal (the healer test: a healer class and a heal seen)
    bool DefensiveSeen = false;  // a defensive aura seen on it (addon-level: spent once gone)
};

// The team plan in one arena match (reset when the bot's arena instance changes)
struct FocusMatchState
{
    uint32 MatchId = 0;  // Battleground::GetInstanceID
    uint32 StartMs = 0;
    uint32 NextPlanMs = 0;
    uint32 NextSampleMs = 0;
    bool UrgentSeen = false;  // the skull's death or immunity has made the plan due once
    bool TeamReady = false;
    TeamParams Team;  // the bot's own TeamParams with the team's CcCapable and PhysicalTeam
    std::array<FocusEnemyMemory, FOCUS_MAX_ENEMIES> Enemies{};  // the arena's enemies by guid, cut to 3 (NOTE 4)
    uint8 EnemyCount = 0;
    PlanState Plan;  // the plan as marked (Kill/Cc: indexes into Enemies)
    ObjectGuid PlanKillGuid;
    bool Planner = false;
    bool FirstPlanDone = false;
    ObjectGuid WrittenSkull;  // the icons as this bot left them
    ObjectGuid WrittenMoon;
    ObjectGuid SeenSkull;  // the icons as this bot last saw them
    ObjectGuid SeenMoon;
    bool Marked = false;
    uint32 LastMarkMs = 0;
    bool HumanMarked = false;  // a mark this bot did not write, in a group with a real player
    uint32 LastHumanMarkMs = 0;
    RemarkEvent PendingEvent = RemarkEvent::None;  // kept until RemarkDecision allows it or the plan drops it
    int8 PendingKill = FOCUS_NONE;
    uint32 EventAtMs = 0;
    uint8 LastRemark = FOCUS_REMARK_REASONS;
    std::array<uint32, FOCUS_REMARK_REASONS> Remarks{};  // RemarkDecision answers, once per event and answer
    ObjectGuid ImmuneGuid;  // the skull seen immune since ImmuneSinceMs (tactics.immuneSwapMs)
    uint32 ImmuneSinceMs = 0;
    uint32 PlanSwitches = 0;  // tactics.planSwitch
    uint32 MarkWrites = 0;
    uint32 MarginRemarks = 0;   // marks a bot other than the planner wrote (strong disagreement)
    uint32 HumanMarksSeen = 0;  // mark changes counted as a real player's
    // The moon (G1b-c): the mode read at the match's start, whether the team may have one, and in mode 2 the moon
    // this bot wrote, when a CC last landed on it or was cast at it, and until when a released moon stays off
    uint8 MoonMode = FOCUS_MOON_ALWAYS;
    bool MoonCapable = false;
    ObjectGuid MoonGuid;
    uint32 MoonLastLandMs = 0;
    uint32 MoonBlockedUntilMs = 0;
    uint32 MoonReleases = 0;
};

struct FocusBotState
{
    FocusParams Params;           // cached by PlayerbotAI::SetPvpSkill (ComputeFocusParams)
    bool AddonKnowledge = false;  // FocusAddonKnowledge at SetPvpSkill
    FocusHoldState Hold;
    FocusMatchState Match;
    // tactics force immune: this enemy counts as immune to everything until ForcedImmuneUntilMs
    ObjectGuid ForcedImmuneGuid;
    uint32 ForcedImmuneAtMs = 0;
    uint32 ForcedImmuneUntilMs = 0;
};

inline bool FocusForcedImmune(FocusBotState const& state, ObjectGuid guid, uint32 nowMs)
{
    return !guid.IsEmpty() && guid == state.ForcedImmuneGuid && FocusBefore(nowMs, state.ForcedImmuneUntilMs);
}

#endif
