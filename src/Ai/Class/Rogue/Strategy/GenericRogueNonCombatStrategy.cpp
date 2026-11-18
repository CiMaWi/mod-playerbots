/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericRogueNonCombatStrategy.h"
#include "Playerbots.h"

GenericRogueNonCombatStrategy::GenericRogueNonCombatStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI)
{
}

void GenericRogueNonCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    NonCombatStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("player has flag",
                                       { NextAction("sprint", ACTION_EMERGENCY + 1) }));
    triggers.push_back(new TriggerNode("enemy flagcarrier near",
                                       { NextAction("sprint", ACTION_EMERGENCY + 2) }));
    triggers.push_back(new TriggerNode("often", { NextAction("unstealth", 30.0f) }));
}
