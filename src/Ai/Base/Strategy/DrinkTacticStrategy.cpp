/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DrinkTacticStrategy.h"

#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

void TacticDrinkStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("prep drink", {NextAction("prep drink", ACTION_NORMAL + 2)}));
    triggers.push_back(new TriggerNode("safe to drink", {NextAction("safe drink", ACTION_NORMAL + 2)}));
}

void TacticDrinkStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TacticDrinkMultiplier(botAI));
}

// G3b fills the bodies below
float TacticDrinkMultiplier::GetValue(Action* /*action*/) { return 1.0f; }

bool PrepDrinkTrigger::IsActive() { return false; }

bool SafeToDrinkTrigger::IsActive() { return false; }

bool PrepDrinkAction::isUseful() { return false; }

bool PrepDrinkAction::Execute(Event /*event*/) { return false; }

bool SafeDrinkAction::isUseful() { return false; }

bool SafeDrinkAction::Execute(Event /*event*/) { return false; }
