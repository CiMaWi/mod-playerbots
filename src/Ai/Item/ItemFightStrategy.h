/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMFIGHTSTRATEGY_H
#define _PLAYERBOT_ITEMFIGHTSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// Item use plan WI15b: what the bot wears and carries that costs only a cooldown, used in a fight on itself, a friend
// or an enemy, for every class (beside "racials"). One trigger per need (ItemFightTriggers.h), one action.
class ItemFightStrategy : public Strategy
{
public:
    ItemFightStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "item fight"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
