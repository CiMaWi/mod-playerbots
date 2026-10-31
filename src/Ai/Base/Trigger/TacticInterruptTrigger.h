/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TACTICINTERRUPTTRIGGER_H
#define PLAYERBOTS_TACTICINTERRUPTTRIGGER_H

#include <string>

#include "PlayerbotAI.h"
#include "Tactics.h"
#include "Trigger.h"

// A class interrupt trigger of the "tactic interrupt" strategy (G5c, TacticInterruptStrategy): the wrapped trigger
// (an InterruptSpellTrigger or InterruptEnemyHealerTrigger of the class) is asked only where the Interrupt tactic is
// enabled for the bot (mask and scope), so the strategy's extra kicks stay off for a masked side, outside the tactic's
// scope and, as the strategy is added only then, with the tactic off. The inner trigger is never registered in an
// engine: it only answers IsActive, as the class's own trigger would.
class TacticInterruptTrigger : public Trigger
{
public:
    TacticInterruptTrigger(PlayerbotAI* botAI, Trigger* inner)
        : Trigger(botAI, "tactic interrupt " + inner->getName()), inner(inner)
    {
    }

    ~TacticInterruptTrigger() override { delete inner; }

    bool IsActive() override { return botAI->TacticEnabledHere(TacticId::Interrupt) && inner->IsActive(); }

private:
    Trigger* inner;
};

#endif
