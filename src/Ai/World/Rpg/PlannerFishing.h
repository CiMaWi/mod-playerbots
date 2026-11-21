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
namespace Planner
{
// World thread, once at startup after the stores loaded
void StartFishIndex();
// Any thread: the published fishing index (null before the build); a reader keeps its copy alive
std::shared_ptr<Fish::Index const> CurrentFishIndex();
}  // namespace Planner

#endif
