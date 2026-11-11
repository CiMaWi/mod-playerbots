/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDPVPSTRATEGY_H
#define _PLAYERBOT_WORLDPVPSTRATEGY_H

#include <string>
#include <vector>

#include "MovementActions.h"
#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;

// World PvP (WP6): "world pvp" holds "world pvp retreat" -> "world pvp retreat" above the rotation
// (ACTION_EMERGENCY + 1; the defensive cooldowns above it still fire), in the combat and non-combat engines of every
// bot outside battlegrounds. Only a persistent bot that retreats from an open-world fight by the rules, or disengages
// at its timeout or stalemate, has the trigger on (FightBook.cpp)
class WorldPvpStrategy : public Strategy
{
public:
    WorldPvpStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "world pvp"; }
};

// The bot retreats or disengages (O(1): its fight memory's clock)
class WorldPvpRetreatTrigger : public Trigger
{
public:
    WorldPvpRetreatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world pvp retreat") {}

    bool IsActive() override;
};

// A step away from the enemies the bot retreats from (straight away, else the nearest free direction in a fan); the
// turn is held while the step runs, so the bot does not attack meanwhile
class WorldPvpRetreatAction : public MovementAction
{
public:
    WorldPvpRetreatAction(PlayerbotAI* botAI) : MovementAction(botAI, "world pvp retreat") {}

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    bool StepTo(float angle, uint32 nowMs);

    uint32 _stepMs = 0;
    float _stepX = 0.0f;
    float _stepY = 0.0f;
};

#endif
