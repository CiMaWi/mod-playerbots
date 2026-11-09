/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERSERVICE_H
#define _PLAYERBOT_PLANNERSERVICE_H

#include <cstdint>
#include <string>
#include <vector>

#include "PlannerPool.h"

class ChatHandler;
class Player;
class PlayerbotAI;

// The server side of the planner pool (travel plan WT12a): the instance, the snapshot and the per-bot hook
namespace Planner
{
PlannerPool& Pool();
// World thread: starts the pool with WorldTravel.PlannerThreads workers and its PerfMonitor rows
void StartPlanner();
// Joins the workers; waiting jobs are dropped
void StopPlanner();
// Map thread only: the bot as the planner sees it (WT.11)
Snapshot FillSnapshot(Player* bot, std::uint32_t stateVersion);
// The bot's map thread, first in its new rpg status update, for persistent bots without a real master: takes and
// rechecks its result, tracks its state version and submits a job when one is due. O(1) unless a snapshot is built
void PlannerHook(PlayerbotAI* botAI, Player* bot);
// The lines of `playerbots planner status`
std::vector<std::string> PlannerStatus();

bool HandleStatusCommand(ChatHandler* handler, char const* args);
bool HandleSelftestCommand(ChatHandler* handler, char const* args);
}  // namespace Planner

#endif
