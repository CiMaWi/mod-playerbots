/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemFightStrategy.h"

#include "ItemFightTriggers.h"

namespace
{
// The old trinket's boost kept its relevance (ACTION_NORMAL + 4, RacialsStrategy); damage below it. A free instant
// heal comes before a class's cast heals (a healer between two casts uses it first: lab12 wi-gear, a priest that kept
// casting its own heals on the hurt mate never reached the item at ACTION_LIGHT_HEAL + 5); a protection before the
// medium heals
float RelevanceOf(ItemUse::Need need)
{
    switch (need)
    {
        case ItemUse::Need::Heal:
            return ACTION_CRITICAL_HEAL + 5;
        case ItemUse::Need::Protect:
            return ACTION_MEDIUM_HEAL + 5;
        case ItemUse::Need::Dispel:
            return ACTION_DISPEL;
        case ItemUse::Need::Mana:
        case ItemUse::Need::Speed:
            return ACTION_NORMAL + 5;
        case ItemUse::Need::Boost:
            return ACTION_NORMAL + 4;
        default:
            return ACTION_NORMAL + 3;
    }
}
}  // namespace

void ItemFightStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    for (ItemUse::Need const need : ItemUse::FIGHT_NEEDS)
        triggers.push_back(
            new TriggerNode(ItemUse::FightTriggerName(need), {NextAction("use item fight", RelevanceOf(need))}));
}
