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
// medium heals. WI16: in a flight (FleeAttackersAction at ACTION_EMERGENCY + 1, every tick) the speed or protection
// item comes first, once: an instant use, the flight goes on at the next tick. The strategy is in both engines: a
// fleeing bot has dropped its target ("drop target", 99, above this) and runs the non-combat engine (lab12 WI16 e1)
// WI18b: a control item stops a runner or a cast, so it comes before the chase ("reach melee" ACTION_HIGH + 1, "reach
// spell" ACTION_HIGH) and the rotation, below the class's own interrupts (ACTION_INTERRUPT and up): at
// ACTION_NORMAL + 3 a warrior chasing a runner and a caster between its casts never reached it (lab12 r2)
constexpr float FLIGHT_RELEVANCE = ACTION_EMERGENCY + 2;

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
        case ItemUse::Need::Control:
            return ACTION_MOVE + 1;
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
    triggers.push_back(new TriggerNode("item flight", {NextAction("use item fight", FLIGHT_RELEVANCE)}));
}
