/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_USEBUFFSSTRATEGY_H
#define _PLAYERBOT_USEBUFFSSTRATEGY_H

#include <array>
#include <cstdint>

#include "Action.h"
#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;

// Item use plan WI8a, PLAN 3.6: before something worth it lies ahead (a battleground, a dungeon, an elite, a group) a
// bot drinks a flask or two elixirs, eats one buff food and reads one scroll, each the best it holds for its role
// (ItemBuffRules.h decides; the framework's ItemUse::Check and Use, caller buff, use them). One non-combat strategy
// of every bot. Its trigger is O(1): what is ahead changed, the queue's next pick is due, or the bags and auras are
// due to be read again (BUFF_CHECK_MS); the action reads the bags and auras then and runs the picker only when they
// or what is ahead changed, with one item_buff row per pick (after a change of the bags or auras only when a pick is
// used, so the bot's own uses write no rows of what they excluded).
class UseBuffsStrategy : public Strategy
{
public:
    UseBuffsStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "use buffs"; }
};

class BuffAheadTrigger : public Trigger
{
public:
    BuffAheadTrigger(PlayerbotAI* botAI);

    bool IsActive() override;
};

class UseBuffItemsAction : public Action
{
public:
    UseBuffItemsAction(PlayerbotAI* botAI) : Action(botAI, "use buff items") {}

    bool Execute(Event event) override;
};

namespace ItemUse
{
// The coating the buff strategy picks from the bags for the main and the off hand, as item entries (0: none): the one
// owner of the best stone, oil or poison (ItemUsageValue keeps what it picks). Reads the bags and auras: not per tick
std::array<std::uint32_t, 2> CoatingPicksOf(PlayerbotAI* botAI);
}  // namespace ItemUse

#endif
