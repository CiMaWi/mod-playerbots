/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_KITETACTICSTRATEGY_H
#define _PLAYERBOT_KITETACTICSTRATEGY_H

#include <string>
#include <vector>

#include "Action.h"
#include "ArenaKite.h"
#include "MovementActions.h"
#include "Strategy.h"
#include "Trigger.h"
#include "Value.h"

class PlayerbotAI;

// The Kite tactic (G3a, PLAN 3.15; numbers KiteConfig, policy ArenaKite.h). 13g-0: the names exist with stub bodies
// (the trigger is never active, the action never useful, the multiplier passes everything), so that G3a edits
// KiteTacticStrategy.cpp only.

// "tactic kite": "melee on me" -> "kite step", in every context with the Kite tactic enabled or the harness on
class TacticKiteStrategy : public Strategy
{
public:
    TacticKiteStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "tactic kite"; }
};

// Zeroes "flee" for a bot that knows the tactic, in scope (DT.3)
class TacticKiteMultiplier : public Multiplier
{
public:
    TacticKiteMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tactic kite") {}

    float GetValue(Action* action) override;
};

// A melee enemy that targets the bot is within KiteConfig::MeleeYards
class MeleeOnMeTrigger : public Trigger
{
public:
    MeleeOnMeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "melee on me") {}

    bool IsActive() override;
};

// One kite step: the class snare, a pillar step, a tested jump spot or a step towards the partner
class KiteStepAction : public MovementAction
{
public:
    KiteStepAction(PlayerbotAI* botAI) : MovementAction(botAI, "kite step") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// The Kite tactic's per-bot state (ArenaKite.h)
class KiteStateValue : public ManualSetValue<KiteBotState&>
{
public:
    KiteStateValue(PlayerbotAI* botAI) : ManualSetValue<KiteBotState&>(botAI, data, "kite state") {}

private:
    KiteBotState data = KiteBotState();
};

#endif
