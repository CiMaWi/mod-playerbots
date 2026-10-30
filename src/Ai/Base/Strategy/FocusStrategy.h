/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOCUSSTRATEGY_H
#define PLAYERBOTS_FOCUSSTRATEGY_H

#include <string>
#include <unordered_map>
#include <vector>

#include "Action.h"
#include "Multiplier.h"
#include "ObjectGuid.h"
#include "Strategy.h"
#include "Trigger.h"

class Battleground;
class PlayerbotAI;
class Unit;

// Target hold and team plan in play (G1b, PLAN 3.15; decisions in ArenaFocus.h, per-bot state in FocusState.h).
// Added to the combat engine only while Playerbots.Tactics.Enable is on (AiFactory); each part tests its tactic per
// match first (the harness forces tactic masks per side), so with the tactics off they change nothing.

// The Hold tactic: a target switch by one of the skill strategy's target-switch actions passes only when HoldDecision
// allows it; a switch to the group's skull always passes (the team plan), one off the skull only for a hard reason.
// Named "pvp hold" so that the engine (strategies by name) asks it before the "skill" multiplier: a held switch does
// not start a notice or reaction event.
class TacticHoldMultiplier : public Multiplier
{
public:
    TacticHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tactic hold") {}

    float GetValue(Action* action) override;

private:
    bool IsSwitchAction(Action* action);
    float Answer(ObjectGuid candidate, uint8 verdict, bool allow, uint32 nowMs);

    // Per bot: the actions of a bot's context live as long as the bot, so the pointer is a stable key
    std::unordered_map<Action*, bool> switchActions;
};

class PvpHoldStrategy : public Strategy
{
public:
    PvpHoldStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "pvp hold"; }
};

// The Plan tactic in arenas (DT.9): "plan due" -> "arena plan" marks skull and moon; "focus mark" sends a bot whose
// target is not the skull to it through "dps assist" (the rti target first) when the enemy-player exception of
// "not dps target active" would leave it where it is.
class ArenaFocusStrategy : public Strategy
{
public:
    ArenaFocusStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "arena focus"; }
};

// Due every PlanIntervalMs once the gates are open, and at once when the skull dies or turns immune. Also samples the
// focus counters (tactics.focusSame / focusDiff) at the same cadence, for every bot of the arena.
class PlanDueTrigger : public Trigger
{
public:
    PlanDueTrigger(PlayerbotAI* botAI) : Trigger(botAI, "plan due") {}

    bool IsActive() override;

private:
    void SampleFocus(Unit* skullUnit, bool skullSet);
};

class FocusMarkTrigger : public Trigger
{
public:
    FocusMarkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "focus mark") {}

    bool IsActive() override;
};

// The team plan's window: the snapshot of the arena's enemies (what the team sees), Decide, RemarkDecision, and the
// icons written only on a change. Bookkeeping: it returns false so that it never takes the bot's action of the tick.
class ArenaPlanAction : public Action
{
public:
    ArenaPlanAction(PlayerbotAI* botAI) : Action(botAI, "arena plan") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// Map thread, at the arena's end (Playerbots.Tactics.Enable): both teams' skull and moon are cleared
void FocusClearArenaMarks(Battleground* bg);

#endif
