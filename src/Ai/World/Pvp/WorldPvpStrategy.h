/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDPVPSTRATEGY_H
#define _PLAYERBOT_WORLDPVPSTRATEGY_H

#include <string>
#include <vector>

#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;

// World PvP (WP6): "world pvp" holds "world pvp retreat" -> "flee attackers" above the rotation (ACTION_EMERGENCY + 1;
// the defensive cooldowns above it still fire), in the combat and non-combat engines of every bot outside
// battlegrounds. Only a persistent bot that left an open-world fight by its enthusiasm while an enemy player attacks
// it has the trigger on (WP24, FightEnthusiasm.cpp)
class WorldPvpStrategy : public Strategy
{
public:
    WorldPvpStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "world pvp"; }
};

// The bot left its fight and is attacked (O(1): WorldPvp::LeavesFlee), or the test constraint flee is on; the flight
// itself, its outcome and the stand after it are the flee action's (FleeAttackersAction::Due)
class WorldPvpRetreatTrigger : public Trigger
{
public:
    WorldPvpRetreatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world pvp retreat") {}

    bool IsActive() override;
};

#endif
