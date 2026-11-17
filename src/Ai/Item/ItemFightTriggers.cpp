/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemFightTriggers.h"

#include "AiObjectContext.h"
#include "FleeRules.h"
#include "ItemUsable.h"
#include "Player.h"
#include "PlayerbotAI.h"

bool ItemFightTrigger::IsActive()
{
    if (!ItemUse::HasEntryFor(ItemUse::FightKindsOf(botAI), need) || !bot->IsInCombat())
        return false;
    ItemUse::Offer const& offer = ItemUse::FightOffer(botAI);
    return offer.any && offer.need == need;
}

bool ItemFlightTrigger::IsActive()
{
    ItemUse::KindMask const kinds = ItemUse::FightKindsOf(botAI);
    if ((!ItemUse::HasEntryFor(kinds, ItemUse::Need::Speed) && !ItemUse::HasEntryFor(kinds, ItemUse::Need::Protect)) ||
        !bot->IsInCombat() || !context->GetValue<FleeRules::Flight&>("flee flight")->Get().active)
        return false;
    ItemUse::Offer const& offer = ItemUse::FightOffer(botAI);
    return offer.any && (offer.need == ItemUse::Need::Speed || offer.need == ItemUse::Need::Protect);
}
