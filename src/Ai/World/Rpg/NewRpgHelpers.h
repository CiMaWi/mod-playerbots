/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGHELPERS_H
#define PLAYERBOTS_NEWRPGHELPERS_H

#include "ObjectGuid.h"

class Object;
class Player;
class PlayerbotAI;
class Quest;
class WorldObject;

// Quest checks shared by the new RPG actions and values. They encode new RPG questing policy
// (solo, non-repeatable, level window), not general quest rules.
namespace NewRpgHelpers
{
// Radius within which a quest giver with something to accept or reward pulls the bot over.
constexpr float QUEST_GIVER_SEARCH_DISTANCE = 80.0f;

// Player::CanInteractWithQuestGiver without the distance check.
bool CanInteractWithQuestGiver(Player* bot, Object* questGiver);
// constraints: the bot's test constraints (TestConstraints::GiverSought); 0 outside a test
bool HasQuestToAcceptOrReward(Player* bot, WorldObject* object, uint32 constraints = 0);
bool IsQuestWorthDoing(Player* bot, Quest const* quest);
bool IsQuestCapableDoing(Player* bot, Quest const* quest);

// Nearest giver in the new RPG target caches with something to accept or reward, skipping givers
// the bot gave up on. A distanceLimit of 0 searches the whole cache.
ObjectGuid FindQuestGiver(PlayerbotAI* botAI, float distanceLimit = 0.0f);

// A shapeshift a spell or an item use is refused in: any form but a stance (SpellInfo::CheckShapeshift; the same
// test as the item-use framework's view, ItemUse.cpp). The quest executor's uses and a craft leave it first (WQL2b
// note 1)
bool RealShapeshift(Player* bot);
}  // namespace NewRpgHelpers

#endif
