/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERFISHING_H
#define _PLAYERBOT_PLANNERFISHING_H

#include <memory>

#include "FishRules.h"

// The server side of the fishing index (professions plan WR16a; the rules are in FishRules.h): the fishing pools by
// zone, the areas' base skills and the fish of open water and of a pool, gathered on the world thread at startup and
// built on a planner worker, published once as an immutable value (like the recipe index).
namespace SkillPath
{
struct Path;
}

namespace Planner
{
struct Snapshot;
namespace Goals
{
struct Situation;
}

// World thread, once at startup after the stores loaded
void StartFishIndex();
// Any thread: the published fishing index (null before the build); a reader keeps its copy alive
std::shared_ptr<Fish::Index const> CurrentFishIndex();

// The planner worker (professions WR16b): a fishing trip in the bot's zone as the situation's one water option, for a
// bot with fishing skill and a pole: the zone's water at the path's prices (Fish::Pick: a pool or open water), its
// place the pool spawn nearest the bot; nothing when the zone has no pool spawn or the water does not pay
void FillFishWaters(Snapshot const& snapshot, SkillPath::Path const* path, Materials::Rates const& rates,
                    Goals::Situation& situation);
}  // namespace Planner

#endif
