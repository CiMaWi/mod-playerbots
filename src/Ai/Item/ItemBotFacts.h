/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMBOTFACTS_H
#define _PLAYERBOT_ITEMBOTFACTS_H

#include <cstdint>

#include "ItemBuffRules.h"

class Item;
class Player;
class PlayerbotAI;

// Item use WI8a, WI8b, WI9a: the bot's facts the buff strategy and demand both read, one definition each. The bot's
// map thread (or the world thread while the map threads wait, as a command does)
namespace ItemUse
{
struct BotItems;

// The bot's item use state, made on first use
BotItems& ItemsOf(PlayerbotAI* botAI);
// The role as the stat weights' calculator reads it (StatsWeightCalculator's constructor)
Role RoleOf(Player* bot);
// The role's fixed weights; for melee and ranged damage the strength and agility weights against attack power come
// from the bot's spec (StatsWeightCalculator), which the fixed table cannot know. anyEntry: any item to value, 0: none
BuffWeights WeightsOf(Player* bot, Role role, std::uint32_t anyEntry);
// WI.10: the control cohort (a persistent bot by its guid and the share) or the test's nospend
bool InControl(PlayerbotAI* botAI, Player* bot);
// WI8b: a poison's type from its enchantment's combat spell (direct damage: instant, a periodic damage aura: deadly)
PoisonKind PoisonKindOf(Record const& record);
// WI8b: the stats of a coating's enchantment, scored with the bot's buff weights
BuffWeights CoatingStats(Record const& record);
// A worn weapon as a coating's target; itemHasCoating says whether it carries a temporary enchantment now
TargetView ViewOfWeapon(Item* weapon);
}  // namespace ItemUse

#endif
