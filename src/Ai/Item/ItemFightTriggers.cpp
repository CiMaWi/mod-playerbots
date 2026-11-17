/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemFightTriggers.h"

#include "AiObjectContext.h"
#include "FleeRules.h"
#include "Group.h"
#include "ItemUsable.h"
#include "Player.h"
#include "PlayerbotAI.h"

namespace
{
// WI18: a group member's body on the bot's map (the sampler adds those near); a walk over the group, no world scan
bool BodyInGroup(Player* bot)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != bot && member->IsInWorld() && member->GetMap() == bot->GetMap() &&
            member->getDeathState() == DeathState::Corpse)
            return true;
    }
    return false;
}
}  // namespace

bool ItemFightTrigger::IsActive()
{
    // combat first: the strategy is in the non-combat engine too (a fleeing bot), where most ticks are out of combat;
    // resurrection and repair the other way round (WI18), each behind its own cheap gate before the offer
    bool const afterFight = ItemUse::AfterFightNeed(need);
    if (bot->IsInCombat() == afterFight || !ItemUse::HasEntryFor(ItemUse::FightKindsOf(botAI), need))
        return false;
    if (need == ItemUse::Need::Resurrect && !BodyInGroup(bot))
        return false;
    if (need == ItemUse::Need::Repair && !ItemUse::FightHoldsRepair(botAI))
        return false;
    ItemUse::Offer const& offer = ItemUse::FightOffer(botAI);
    return offer.any && offer.need == need;
}

bool ItemFlightTrigger::IsActive()
{
    if (!bot->IsInCombat())
        return false;
    ItemUse::KindMask const kinds = ItemUse::FightKindsOf(botAI);
    if ((!ItemUse::HasEntryFor(kinds, ItemUse::Need::Speed) && !ItemUse::HasEntryFor(kinds, ItemUse::Need::Protect)) ||
        !context->GetValue<FleeRules::Flight&>("flee flight")->Get().active)
        return false;
    ItemUse::Offer const& offer = ItemUse::FightOffer(botAI);
    return offer.any && (offer.need == ItemUse::Need::Speed || offer.need == ItemUse::Need::Protect);
}
