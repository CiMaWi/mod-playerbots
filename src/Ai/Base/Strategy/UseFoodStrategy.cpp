/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UseFoodStrategy.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Upkeep.h"

void UseFoodStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);
    // AW9: a risen bot recovers before it takes up its work (awareness INTERFACE.md §3.6), above every status action
    // of the RPG (`new rpg visit` 13.0 is the highest)
    triggers.push_back(new TriggerNode("recovering", {NextAction("recover", 14.0f)}));
    // Decided when the strategies are built; a master joining or leaving rebuilds them (ResetStrategies)
    if (Upkeep::FoodCheat(botAI))
    {
        triggers.push_back(new TriggerNode("medium health", { NextAction("eat", 4.1f) }));
        triggers.push_back(new TriggerNode("high mana", { NextAction("drink", 4.2f) }));
    }
    else
    {
        triggers.push_back(new TriggerNode("low health", { NextAction("eat", 4.1f) }));
        triggers.push_back(new TriggerNode("medium mana", { NextAction("drink", 4.2f) }));
    }
}
