/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LOSTACTICSTRATEGY_H
#define _PLAYERBOT_LOSTACTICSTRATEGY_H

#include <string>
#include <vector>

#include "Action.h"
#include "ArenaCover.h"
#include "LosTactic.h"
#include "MovementActions.h"
#include "Strategy.h"
#include "Trigger.h"
#include "Value.h"

class PlayerbotAI;

// The Los and Pillar tactics (G2e, G2f, PLAN 3.15; policy in ArenaCover.h, numbers LosConfig and PillarConfig). 13f-0:
// the names exist with stub bodies (the triggers are never active, the actions never useful, the multiplier passes
// everything), so that G2e and G2f edit LosTacticStrategy.cpp only.

// "tactic los": "target out of los" and "partner out of los" -> "regain los", in every context with the Los tactic
// enabled or the harness on (AiFactory); the triggers test the tactic (mask, scope, gate) at run time
class TacticLosStrategy : public Strategy
{
public:
    TacticLosStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "tactic los"; }
};

// "tactic pillar": "cast at me" -> "break los" and "pillar step" -> "pillar step", below the interrupts (arena only)
class TacticPillarStrategy : public Strategy
{
public:
    TacticPillarStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "tactic pillar"; }
};

// Parks ArenaTactics' walk-into-target for a ranged bot that knows the regain (G2e, LosParksArenaTactics)
class TacticLosMultiplier : public Multiplier
{
public:
    TacticLosMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tactic los") {}

    float GetValue(Action* action) override;
};

// A ranged bot cannot see its target
class TargetOutOfLosTrigger : public Trigger
{
public:
    TargetOutOfLosTrigger(PlayerbotAI* botAI) : Trigger(botAI, "target out of los") {}

    bool IsActive() override;
};

// A healer cannot see an alive partner (read from the group directly, not from "party member to heal")
class PartnerOutOfLosTrigger : public Trigger
{
public:
    PartnerOutOfLosTrigger(PlayerbotAI* botAI) : Trigger(botAI, "partner out of los") {}

    bool IsActive() override;
};

// An enemy player casts at the bot a cast no interrupt can stop (the visible cast bar only)
class CastAtMeTrigger : public Trigger
{
public:
    CastAtMeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cast at me") {}

    bool IsActive() override;
};

// A melee chaser can be pulled round a pillar
class PillarStepTrigger : public Trigger
{
public:
    PillarStepTrigger(PlayerbotAI* botAI) : Trigger(botAI, "pillar step") {}

    bool IsActive() override;
};

// Moves to a point that sees the target or the partner again (the cover plan)
class RegainLosAction : public MovementAction
{
public:
    RegainLosAction(PlayerbotAI* botAI) : MovementAction(botAI, "regain los") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// Steps behind cover from an enemy cast at the bot
class BreakLosAction : public MovementAction
{
public:
    BreakLosAction(PlayerbotAI* botAI) : MovementAction(botAI, "break los") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// One hop round a pillar (at most StepYards)
class PillarStepAction : public MovementAction
{
public:
    PillarStepAction(PlayerbotAI* botAI) : MovementAction(botAI, "pillar step") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// The bot's cover plan, refreshed at most once per second (G2e)
class CoverPlanValue : public ManualSetValue<CoverPlan>
{
public:
    CoverPlanValue(PlayerbotAI* botAI) : ManualSetValue<CoverPlan>(botAI, CoverPlan(), "cover plan") {}
};

// The Los tactic's per-bot state (LosTactic.h: the sight refresh, the episode, the relocation guards, the counts)
class LosStateValue : public ManualSetValue<LosBotState&>
{
public:
    LosStateValue(PlayerbotAI* botAI) : ManualSetValue<LosBotState&>(botAI, data, "los state") {}

private:
    LosBotState data = LosBotState();
};

#endif
