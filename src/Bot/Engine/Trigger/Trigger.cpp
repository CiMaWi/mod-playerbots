/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Trigger.h"

#include "AiObjectContext.h"
#include "BotTactics.h"
#include "Event.h"
#include "NaturalReaction.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"

Trigger::Trigger(PlayerbotAI* botAI, std::string const name, int32 checkInterval)
    : AiNamedObject(botAI, name),
      checkInterval(checkInterval == 1 ? 1 : (checkInterval < 100 ? checkInterval * IN_MILLISECONDS : checkInterval)),
      lastCheckTime(0)
{
}

Event Trigger::Check()
{
    if (IsActive())
    {
        Event event(getName());
        return event;
    }

    Event event;
    return event;
}

Value<Unit*>* Trigger::GetTargetValue() { return context->GetValue<Unit*>(GetTargetName()); }

Unit* Trigger::GetTarget() { return GetTargetValue()->Get(); }

uint32 Trigger::CheckIntervalMs()
{
    // During an out-of-combat force-rebuff, evaluate every buff trigger each tick
    if (IsBuffTrigger() && !IsDebuffTrigger() && botAI->forceRebuff.IsPending() && !bot->IsInCombat())
        return 0;

    if (checkInterval < 2)
        return 0;

    // A reaction trigger checks at the natural cadence's faster interval while it runs (the bool first: O(1)); the
    // cadence's CheckMs is at least 20, so this is never 0
    return ReactionCheckIntervalMs(uint32(checkInterval),
                                   botAI->GetTactics().IsNaturalCadenceActive() && IsReactionTrigger(),
                                   sPlayerbotAIConfig.NaturalCadence.CheckMs);
}

bool Trigger::needCheck(uint32 now)
{
    uint32 const interval = CheckIntervalMs();
    if (!interval)
        return true;

    if (!lastCheckTime || now - lastCheckTime >= interval)
    {
        lastCheckTime = now;
        return true;
    }

    return false;
}
