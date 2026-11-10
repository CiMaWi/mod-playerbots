/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTSTATEVENTS_H
#define _PLAYERBOT_QUESTSTATEVENTS_H

#include <cstdint>

class Player;

// The bot's quest events for the population's quest statistics (travel plan WT13a, PlannerQuests.cpp). A header of
// its own, so the bot's update and the census include neither the planner pool nor its indexes
namespace Planner
{
// Map threads: the bot's quest events, persistent random bots only; O(1) under one short lock, no DB
void QuestStatAccepted(Player* bot, std::uint32_t questId);
void QuestStatRewarded(Player* bot, std::uint32_t questId);
void QuestStatAbandoned(Player* bot, std::uint32_t questId);
void QuestStatDropped(Player* bot, std::uint32_t questId);
// A death counts for the quest the bot's rpg status is doing
void QuestStatDeath(Player* bot);
}  // namespace Planner

#endif
