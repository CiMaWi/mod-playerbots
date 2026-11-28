/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGMULTIPLIERS_H
#define PLAYERBOTS_NEWRPGMULTIPLIERS_H

#include "Multiplier.h"

// Decides upstream's grind pick "attack anything" (GoalApply::GrindPickOf, questing WQ39, WQ.45). A bot the planner
// leads in the open world picks a mob only while its goal is a chosen grind. Every bot picks none under the test's
// nofight, with resurrection sickness (upkeep plan 3.5) or on a trip (RPG_TRAVEL). A bot the planner does not lead, and
// one in an instance, also picks none while a giver its status would visit is near or while the quest executor holds
// a target (questing WT35a; ported/adapted from mod-playerbots PR #2632, head d4e5d110). The combat actions that answer
// an attacker and the executor's attack on its target are other actions.
class NewRpgGrindMultiplier : public Multiplier
{
public:
    NewRpgGrindMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "new rpg grind") {}

    float GetValue(Action* action) override;
};

#endif
