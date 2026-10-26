/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FIGHTENEMIESVALUE_H
#define PLAYERBOTS_FIGHTENEMIESVALUE_H

#include <vector>

#include "Value.h"

class PlayerbotAI;
class Unit;

// The enemies of the bot's current fight for the tactics (Tactics.h), nearest first, at most TACTIC_FIGHT_ENEMIES_MAX:
// arena: the other team's players the bot can see; battleground: the players among "attackers" (within 40 yd);
// open-world PvP: the attacker a helper would take, the PvP combat references, the duel opponent and "enemy player
// target"; PvE: "attackers". Empty while the bot is not active.
class FightEnemiesValue : public ObjectGuidListCalculatedValue
{
public:
    // Milliseconds; shared by every tactic of the bot
    static constexpr int32 CheckIntervalMs = 500;

    FightEnemiesValue(PlayerbotAI* botAI) : ObjectGuidListCalculatedValue(botAI, "fight enemies", CheckIntervalMs) {}

    GuidVector Calculate() override;

private:
    void AddArenaEnemies(std::vector<Unit*>& candidates);
    void AddAttackers(std::vector<Unit*>& candidates, bool playersOnly);
    void AddOpenWorldEnemies(std::vector<Unit*>& candidates);
};

#endif
