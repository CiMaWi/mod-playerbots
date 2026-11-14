/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FLEEATTACKERSACTION_H
#define _PLAYERBOT_FLEEATTACKERSACTION_H

#include "FleeRules.h"
#include "MovementActions.h"
#include "Value.h"

class PlayerbotAI;

// The bot's flight from attacking enemy players ("flee flight"), reset when the bot dies
class FleeFlightValue : public ManualSetValue<FleeRules::Flight&>
{
public:
    FleeFlightValue(PlayerbotAI* botAI) : ManualSetValue<FleeRules::Flight&>(botAI, data) {}

private:
    FleeRules::Flight data = FleeRules::Flight();
};

// The bot flees from the enemy players in PvP combat with it, in 20 yd steps away from their centre (straight away,
// else the nearest free direction in a fan), until the flight's outcome (FleeRules: escaped, cornered or bound; a
// Flee world-log row). It reads the bot and its attackers only; the strategy that wants the flight holds a trigger
// that asks Due()
class FleeAttackersAction : public MovementAction
{
public:
    FleeAttackersAction(PlayerbotAI* botAI) : MovementAction(botAI, "flee attackers") {}

    bool isUseful() override;
    bool Execute(Event event) override;

    // For that trigger (O(1)); `wants`: the strategy's own reason to flee now (FleeRules::Due)
    static bool Due(PlayerbotAI* botAI, bool wants);

private:
    // What a step in one direction came to: taken; a wall (no path, or something hit right in front); an edge (the
    // walkable ground ends right in front, or the path winds); or the move was refused
    enum class Step : uint8
    {
        Taken,
        Wall,
        Edge,
        Refused
    };

    // The enemy players in PvP combat with the bot (a pet at its own place, no totems): their number, their centre
    // and the nearest's distance
    uint32 Foes(float& x, float& y, float& nearestYards);
    void End(FleeRules::Flight& flight, FleeRules::Outcome outcome, uint32 foes, float nearestYards, uint32 nowMs);
    // One turn of stepping away from (fromX, fromY): true while it holds the turn
    bool StepAway(FleeRules::Flight& flight, float fromX, float fromY, uint32 nowMs);
    // One step at `angle`; `endYards`: how far that way leads
    Step StepTo(FleeRules::Flight& flight, float angle, uint32 nowMs, float& endYards);
};

#endif
