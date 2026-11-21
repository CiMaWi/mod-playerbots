/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERRECIPES_H
#define _PLAYERBOT_PLANNERRECIPES_H

#include <memory>
#include <string>
#include <vector>

#include "RecipeIndex.h"

class ChatHandler;

// The server side of the recipe index (professions plan WR3a; records and rules in RecipeIndex.h). Like the upkeep
// index: gathered on the world thread, built on a planner worker, published once as an immutable value.
namespace Planner
{
// World thread, once at startup after the stores loaded: gathers the profession spells, trainers, recipe items and
// loot yields, builds the index on a worker
void StartRecipeIndex();
// Any thread: the published recipe index (null before the build); a reader keeps its copy alive
std::shared_ptr<Recipes::RecipeIndex const> CurrentRecipeIndex();
// False until the build finished
bool RecipeIndexReady();
// World thread, at startup: one loot store's rows by entry (quest-only rows and rows never on in the normal mode left
// out), for Recipes::ExpectedYield
Recipes::LootTable LoadLootTable(char const* table);
// The recipe index lines of `playerbots planner status|indexes`
std::vector<std::string> RecipeIndexStatus();
// `playerbots professions index [<skill>] [dump [<file>]]`: the words after "index"
bool HandleRecipeIndexCommand(ChatHandler* handler, std::vector<std::string> const& words);
}  // namespace Planner

#endif
