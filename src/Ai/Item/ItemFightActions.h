/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMFIGHTACTIONS_H
#define _PLAYERBOT_ITEMFIGHTACTIONS_H

#include "Action.h"

class PlayerbotAI;

// Item use plan WI15b: the "item fight" strategy's one action. It runs the fight offer's request (the free entry and
// the target MakeOffer chose: the bot, a friend, an enemy or a spot) through the item use path, Check first: a
// refusal then writes no row and the offer is made again a second later; Use writes the row (fight:used,
// fight:refused:<cause>). Replaces the old trinket action (WI15b).
class UseItemFightAction : public Action
{
public:
    UseItemFightAction(PlayerbotAI* botAI) : Action(botAI, "use item fight") {}

    bool Execute(Event event) override;
    // The offer it runs answers the boost need (the encounters' DPS cooldown test, EncounterHelpers)
    bool Boosts();
};

#endif
