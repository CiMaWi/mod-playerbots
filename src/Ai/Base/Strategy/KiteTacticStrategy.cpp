/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "KiteTacticStrategy.h"

#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

void TacticKiteStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("melee on me", {NextAction("kite step", ACTION_MOVE + 4)}));
}

void TacticKiteStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TacticKiteMultiplier(botAI));
}

// G3a fills the bodies below
float TacticKiteMultiplier::GetValue(Action* /*action*/) { return 1.0f; }

bool MeleeOnMeTrigger::IsActive() { return false; }

bool KiteStepAction::isUseful() { return false; }

bool KiteStepAction::Execute(Event /*event*/) { return false; }
