/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMFIGHTTRIGGERS_H
#define _PLAYERBOT_ITEMFIGHTTRIGGERS_H

#include <array>
#include <string>

#include "ItemOfferRules.h"
#include "Trigger.h"

class PlayerbotAI;

// Item use plan WI15b: the "item fight" strategy's triggers, one per need a free entry (no cost but a cooldown) can
// answer. Gate O(1): the bot's free list has an entry of the need's kind (FreeKindsOf) and the bot fights; only then
// the shared fight offer (FightOffer: sampled, made at most once a second); active when the offer's need is this one.
// The thresholds are the offer's (WantedNeeds), none here.
namespace ItemUse
{
constexpr std::array<Need, 8> FIGHT_NEEDS = {Need::Heal,  Need::Mana,  Need::Protect,   Need::Dispel,
                                             Need::Speed, Need::Boost, Need::DamageOne, Need::DamageArea};

inline std::string FightTriggerName(Need need) { return "item fight " + std::string(NeedWord(need)); }
}  // namespace ItemUse

class ItemFightTrigger : public Trigger
{
public:
    ItemFightTrigger(PlayerbotAI* botAI, ItemUse::Need need)
        : Trigger(botAI, ItemUse::FightTriggerName(need), 1), need(need)
    {
    }

    bool IsActive() override;

private:
    ItemUse::Need need;
};

#endif
