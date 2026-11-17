/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemFightTriggers.h"

#include "ItemUsable.h"
#include "Player.h"
#include "PlayerbotAI.h"

bool ItemFightTrigger::IsActive()
{
    if (!ItemUse::HasEntryFor(ItemUse::FreeKindsOf(botAI), need) || !bot->IsInCombat())
        return false;
    ItemUse::Offer const& offer = ItemUse::FightOffer(botAI);
    return offer.any && offer.need == need;
}
