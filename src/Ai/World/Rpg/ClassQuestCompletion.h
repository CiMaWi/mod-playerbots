/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_CLASSQUESTCOMPLETION_H
#define _PLAYERBOT_CLASSQUESTCOMPLETION_H

#include <cstdint>
#include <vector>

#include "ClassQuestRules.h"

class Player;

// The automatic class-quest completion (upkeep WU21a): the class quests WU39's rules call due (DueQuests) are marked
// rewarded with the bot's chain steps, a step in the log leaves it, the reward spell is cast; no XP, gold or items.
// One HonestyClassQuest row per quest (id the quest, src the source below or class_quest_failed). Map-thread only;
// the rules read the quest index and the upkeep index's trainer lists, both in memory (PlannerQuests.cpp)
namespace Planner::ClassQuestCompletion
{
constexpr char const* SOURCE_CREATION = "creation";
constexpr char const* SOURCE_LEVELUP = "levelup_maintenance";  // the level-up maintenance's source word

// A bot that keeps its possessions (BotKind::KeepsPossessions): its doable class quests are its own to do
bool Governed(Player* bot);
// Completes now what is due at `when`; before the quest and upkeep indexes are ready the bot waits for its first
// update after (Drain). Check asks only for a governed bot's failed doable quests
void Request(Player* bot, ClassQuests::When when);
// The planner's definite no-route verdicts (GoalPlan::classFailed), taken with the plan on the bot's map thread
void CompleteFailed(Player* bot, std::vector<ClassQuests::Due> const& due);
// A waiting request of this bot, once both indexes are ready: one atomic load while none waits (PlayerbotAI::UpdateAI)
void Drain(Player* bot);
// The bot logs out: a request it still waits with goes (the login asks again)
void Forget(Player* bot);
// The class-quest table's spells the bot knows (learns and reward spells), sorted: O(table)
std::vector<std::uint32_t> KnownSpells(Player* bot);
}  // namespace Planner::ClassQuestCompletion

#endif
