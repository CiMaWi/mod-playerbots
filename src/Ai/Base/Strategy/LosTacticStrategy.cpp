/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LosTacticStrategy.h"

#include "Playerbots.h"

// 13f-0 stubs: nothing is active, so nothing changes. G2e and G2f fill these bodies.

void TacticLosStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above the plain movement, below the combat actions: a regain must not outrank a heal or an interrupt
    triggers.push_back(new TriggerNode("target out of los", {NextAction("regain los", ACTION_MOVE + 2)}));
    triggers.push_back(new TriggerNode("partner out of los", {NextAction("regain los", ACTION_MOVE + 2)}));
}

void TacticLosStrategy::InitMultipliers(std::vector<Multiplier*>& /*multipliers*/)
{
    // G2e adds TacticLosMultiplier here (13f-0: none, so no bot pays a virtual call per action)
}

void TacticPillarStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Below the interrupts (ACTION_INTERRUPT), above the movement
    triggers.push_back(new TriggerNode("cast at me", {NextAction("break los", ACTION_INTERRUPT - 1)}));
    triggers.push_back(new TriggerNode("pillar step", {NextAction("pillar step", ACTION_MOVE + 3)}));
}

float TacticLosMultiplier::GetValue(Action* /*action*/) { return 1.0f; }

bool TargetOutOfLosTrigger::IsActive() { return false; }

bool PartnerOutOfLosTrigger::IsActive() { return false; }

bool CastAtMeTrigger::IsActive() { return false; }

bool PillarStepTrigger::IsActive() { return false; }

bool RegainLosAction::isUseful() { return false; }

bool RegainLosAction::Execute(Event /*event*/) { return false; }

bool BreakLosAction::isUseful() { return false; }

bool BreakLosAction::Execute(Event /*event*/) { return false; }

bool PillarStepAction::isUseful() { return false; }

bool PillarStepAction::Execute(Event /*event*/) { return false; }
