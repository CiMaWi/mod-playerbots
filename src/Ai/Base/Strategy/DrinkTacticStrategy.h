/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_DRINKTACTICSTRATEGY_H
#define _PLAYERBOT_DRINKTACTICSTRATEGY_H

#include <string>
#include <vector>

#include "Action.h"
#include "ArenaDrink.h"
#include "NonCombatActions.h"
#include "Strategy.h"
#include "Trigger.h"
#include "Value.h"

class PlayerbotAI;

// The Drink tactic (G3b, PLAN 3.15; numbers DrinkConfig, policy ArenaDrink.h). 13g-0: the names exist with stub bodies
// (the triggers are never active, the actions never useful, the multiplier passes everything), so that G3b edits
// DrinkTacticStrategy.cpp only.

// "tactic drink": "prep drink" and "safe to drink" -> the drinks, in the battleground non-combat engine and the
// combat engine (the safe-window break), with the Drink tactic enabled or the harness on
class TacticDrinkStrategy : public Strategy
{
public:
    TacticDrinkStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "tactic drink"; }
};

class TacticDrinkMultiplier : public Multiplier
{
public:
    TacticDrinkMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tactic drink") {}

    float GetValue(Action* action) override;
};

// The gates are closed, more than PrepMinLeftMs are left and the mana is below PrepBelowPct
class PrepDrinkTrigger : public Trigger
{
public:
    PrepDrinkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "prep drink") {}

    bool IsActive() override;
};

// Out of combat, mana below the threshold, no enemy near or in LOS, the partner healthy
class SafeToDrinkTrigger : public Trigger
{
public:
    SafeToDrinkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "safe to drink") {}

    bool IsActive() override;
};

class PrepDrinkAction : public DrinkAction
{
public:
    PrepDrinkAction(PlayerbotAI* botAI) : DrinkAction(botAI, "prep drink") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

class SafeDrinkAction : public DrinkAction
{
public:
    SafeDrinkAction(PlayerbotAI* botAI) : DrinkAction(botAI, "safe drink") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// The Drink tactic's per-bot state (ArenaDrink.h)
class DrinkStateValue : public ManualSetValue<DrinkBotState&>
{
public:
    DrinkStateValue(PlayerbotAI* botAI) : ManualSetValue<DrinkBotState&>(botAI, data, "drink state") {}

private:
    DrinkBotState data = DrinkBotState();
};

#endif
