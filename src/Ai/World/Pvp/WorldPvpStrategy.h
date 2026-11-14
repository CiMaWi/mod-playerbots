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

protected:
    WorldPvpRetreatAction(PlayerbotAI* botAI, std::string const name) : MovementAction(botAI, name) {}

    // One turn of stepping away from (fromX, fromY), true while it holds the turn; `gaveUp`: every direction is
    // blocked. `what` names the movement in the debug lines
    bool StepAway(float fromX, float fromY, char const* what, bool& gaveUp);
    std::string RefusalCause(float x, float y, float z, bool exact);

private:
    // What a step in one direction came to: taken; a wall right in front (the collision check hit something there);
    // an edge right in front (the walkable ground ends there with nothing hit); or the move was refused
    enum class Step : uint8
    {
        Taken,
        Wall,
        Edge,
        Refused
    };

    // One step at `angle`; `endYards`: how far the straight line that way goes before it is cut short
    Step StepTo(float angle, uint32 nowMs, float& endYards);

    uint32 _stepMs = 0;
    float _stepX = 0.0f;
    float _stepY = 0.0f;
};

// Test hook (world-pvp WP20): with the constraint fleestep or fleemgr on, the bot in PvP combat flees from the players
// attacking it (O(1): the constraint bit first); the old retreat's trigger is off meanwhile
class WorldPvpFleeTrigger : public Trigger
{
public:
    WorldPvpFleeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "world pvp flee") {}

    bool IsActive() override;

private:
    uint32 _noteMs = 0;
};

// The flight from the attacking players' centre at its start, without the fight memory: fleestep by the retreat's
// steps, fleemgr by repeated FleeManager points of the same length. It ends by its time bound, by giving up (steps
// blocked all round) or when the trigger stays off for a while (out of PvP combat)
class WorldPvpFleeAction : public WorldPvpRetreatAction
{
public:
    WorldPvpFleeAction(PlayerbotAI* botAI) : WorldPvpRetreatAction(botAI, "world pvp flee") {}

    bool isUseful() override;
    bool Execute(Event event) override;

private:
    bool Begins(uint32 nowMs, char const* what);
    uint32 Foes(float& x, float& y);
    bool Hop(uint32 nowMs);
    void Note(uint32 nowMs, char const* outcome);

    uint32 _flightMs = 0;
    uint32 _lastMs = 0;
    uint32 _hopMs = 0;
    uint32 _noteMs = 0;
    uint32 _pulseMs = 0;
    float _startX = 0.0f;
    float _startY = 0.0f;
    bool _over = false;
    float _fromX = 0.0f;
    float _fromY = 0.0f;
    float _hopX = 0.0f;
    float _hopY = 0.0f;
};

#endif
