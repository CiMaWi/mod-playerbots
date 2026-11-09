/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERSERVICE_H
#define _PLAYERBOT_PLANNERSERVICE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "PlannerIndexes.h"
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

// The planner's indexes and crowding (WT12b, PlannerIndexes.cpp)
// World thread, once after the pool started: gathers the spawns and zones, builds the indexes on a worker
void StartPlannerIndexes();
// Any thread: the published indexes (null before the first build); a reader keeps its copy alive
std::shared_ptr<PlannerIndexes const> Indexes();
// False until the build finished: planners keep today's pick meanwhile
bool IndexesReady();
// Any thread: the last crowding snapshot (null before the first refresh)
std::shared_ptr<CrowdingSnapshot const> Crowding();
// World thread, every world tick: a timer add; every Crowding.RefreshSeconds one pass over the online players
void UpdateCrowding(std::uint32_t diff);
// The index and crowding lines of `playerbots planner status|indexes`
std::vector<std::string> IndexStatus();
bool HandleIndexesCommand(ChatHandler* handler, char const* args);
bool HandleHubCommand(ChatHandler* handler, char const* args);
bool HandleServiceCommand(ChatHandler* handler, char const* args);
bool HandleCrowdCommand(ChatHandler* handler, char const* args);
}  // namespace Planner

#endif
