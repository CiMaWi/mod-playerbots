/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemFightActions.h"

#include "ItemUsable.h"
#include "ItemUse.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"

bool UseItemFightAction::Execute(Event /*event*/)
{
    // the test's nouse: no request of the bot's own
    if (!ItemUse::Allowed(bot, ItemUse::Caller::Fight) || ItemUse::Pending(bot) || bot->IsNonMeleeSpellCast(true))
        return false;

    ItemUse::Offer const offer = ItemUse::FightOffer(botAI);
    ItemUse::UsableEntry const* entry = ItemUse::OfferedEntry(botAI, offer);
    if (!entry)
        return false;

    ItemUse::Request const request = ItemUse::FightRequest(bot, offer, *entry);
    ItemUse::Cause const cause = ItemUse::Check(bot, request);
    if (cause != ItemUse::Cause::None)
    {
        // no row (the old trinket action's rule); a refusal that holds lets the next answer have its turn
        if (cause != ItemUse::Cause::Busy)
            ItemUse::BlockOffered(botAI, *entry, cause);
        return false;
    }

    LOG_TRACE("playerbots", "Use item fight: {} uses {} spell {} for {} (target kind {})", bot->GetName(),
              entry->item.entry, request.spellId, ItemUse::NeedWord(offer.need), static_cast<int>(offer.target.kind));
    // the entry's keys before the use: the list is not touched by it, but nothing after it reads the list
    ItemUse::UsableEntry const used = *entry;
    ItemUse::UseResult const result = ItemUse::Use(bot, request);
    // a refusal after Check said yes (the game's handler: Cause::Handler) blocks the entry as Check's would, so the
    // offer does not name it again at the next tick with a rebuild and a refused row each time
    if (result.outcome == ItemUse::Outcome::Refused && result.cause != ItemUse::Cause::None &&
        result.cause != ItemUse::Cause::Busy)
        ItemUse::BlockOffered(botAI, used, result.cause);
    // the count, the cooldown and the offer change with the use: the list and the offer are made again
    ItemUse::MarkUsableDirty(botAI);
    return result.outcome != ItemUse::Outcome::Refused;
}

bool UseItemFightAction::Boosts()
{
    ItemUse::Offer const& offer = ItemUse::FightOffer(botAI);
    return offer.any && offer.need == ItemUse::Need::Boost;
}
