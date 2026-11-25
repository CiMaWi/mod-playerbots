/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONREFERENCE_H
#define _PLAYERBOT_AUCTIONREFERENCE_H

#include <memory>

class ChatHandler;

namespace Recipes
{
class RecipeIndex;
}

namespace AuctionRules
{
struct Table;
}

// The reference value of every tradeable item (auction house plan WA2a, AuctionRules.h): the item templates and the
// vendors are read once at startup on the world thread, the table is built on the planner worker that built the
// recipe index, right after it, and published immutable. No reader yet besides the command (WA2b, WA7a read it).
namespace AuctionReference
{
// Startup, world thread, before Planner::StartRecipeIndex: the item templates, the vendors' stock and the bands' rates
void Gather();

// The recipe index's worker, after its publication: builds and publishes the table from the gathered rows
void OnRecipeIndexReady(std::shared_ptr<Recipes::RecipeIndex const> index);

// The table, null until built; any thread
std::shared_ptr<AuctionRules::Table const> Current();

// playerbots auction ref <item id or link> | sample | dump [<file name>] (read only)
bool HandleRefCommand(ChatHandler* handler, char const* args);
}  // namespace AuctionReference

#endif
