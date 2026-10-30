/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CCTACTICSTRATEGY_H
#define PLAYERBOTS_CCTACTICSTRATEGY_H

#include <string>
#include <unordered_map>
#include <vector>

#include "Action.h"
#include "Multiplier.h"
#include "ObjectGuid.h"
#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;
class Unit;

// The Cc tactic in play (G4b, PLAN 3.15; the policy in ArenaCc.h, per-bot state in CcState.h). Added to every combat
// engine while Playerbots.Tactics.Enable is on and the Cc or OwnCc tactic is enabled, or the PvP harness is on
// (AiFactory); each part tests its tactic at run time (the harness forces tactic masks per side), so with the tactic
// off nothing changes and the old "cc" strategies act as before.

// Mutes the old "cc" strategies' actions (polymorph and the "* on cc" ones) where the Cc tactic acts: enabled here
// (mask and scope) and known at the bot's skill. Two systems on one bot double-cast and fight over DR (DT.3). Named
// "pvp cc" so that the engine (strategies by name) asks it before the "skill" multiplier: a muted action does not
// start a notice or reaction event.
class TacticCcMultiplier : public Multiplier
{
public:
    TacticCcMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tactic cc") {}

    float GetValue(Action* action) override;

private:
    bool IsOldCcAction(Action* action);

    // Per bot: the actions of a bot's context live as long as the bot, so the pointer is a stable key
    std::unordered_map<Action*, bool> oldCcActions;
};

class PvpCcStrategy : public Strategy
{
public:
    PvpCcStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "pvp cc"; }
};

// "cc ready" -> "team cc": the bot's CC (PickCc over its known, ready rows) on the team's CC target: the moon in an
// arena; elsewhere, or in an arena without a moon, the strongest enemy of "fight enemies" other than the bot's own
// target and the skull
class TacticCcStrategy : public Strategy
{
public:
    TacticCcStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "tactic cc"; }
    // OwnCc (G4b-b): the team's breakable-CC'd enemies (and a forced one) stay out of the dps, debuff and AoE targets
    // in every context (scope All); the list is cached per bot (CcBotState::Excluded), so a call is a time compare
    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;
};

// The Cc tactic is active (gate), the bot is not casting, a row is ready, the CC target is alive, not immune and not
// controlled (a chaining bot, G4c: or held by its team's CC, the chain due), the patient hold and the low-effort stop
// let it, PickCc has a row and the target is in sight. The answer is cached for CC_READY_REFRESH_MS (CcBotState;
// shorter when a chain is due sooner), so a tick costs one flag test and a time compare.
class CcReadyTrigger : public Trigger
{
public:
    CcReadyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cc ready") {}

    bool IsActive() override;
};

// Casts the picked row's highest known rank on the trigger's target (a CrowdControl action of the skill strategy's
// table: the notice and reaction model applies, DT.11)
class TeamCcAction : public Action
{
public:
    TeamCcAction(PlayerbotAI* botAI) : Action(botAI, "team cc") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
    Unit* GetTarget() override;
};

// OwnCc: the bot's current target is held in a team breakable CC (or forced), the bot's own-break window aside
class OwnCcTargetTrigger : public Trigger
{
public:
    OwnCcTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "own cc target") {}

    bool IsActive() override;
};

// Drops that target (a cast or channel at it, the swing, the pet's attack) so the next target pick, which leaves it
// out, takes another (a TargetSwitch of the skill strategy's table: the notice and reaction model applies, DT.11)
class CcRetargetAction : public Action
{
public:
    CcRetargetAction(PlayerbotAI* botAI) : Action(botAI, "cc retarget") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// The OwnCc list of the bot (refreshed at most every CC_EXCLUDE_REFRESH_MS; empty with OwnCc not enabled here)
std::vector<ObjectGuid> const& CcOwnExclusions(PlayerbotAI* botAI);

// The target is in that list: the bot keeps out of it (the hold lets the bot leave it as it leaves an immune one)
bool CcTargetKeptOut(PlayerbotAI* botAI, Unit const* target);

// cc.ownBreaks: the bot's (or its pet's) damage hit an enemy in a breakable CC of its team (the harness counts it)
void CcCountOwnBreak(Unit* attacker, Unit* victim);

// tactics force dr (G4c): while on, a team CC landing is not counted as a chain (the forced casts land at once, one
// after another); set on the world thread while the maps wait
void CcSetForcedCasts(bool on);

// The DR memory's feed (unit hooks: a CC aura landing on and leaving a player or a player's pet), registered by
// Playerbots.cpp
void AddCcTacticScripts();

#endif
