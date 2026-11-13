/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERSERVICE_H
#define _PLAYERBOT_PLANNERSERVICE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "PlannerIndexes.h"
#include "PlannerPool.h"
#include "QuestStatEvents.h"

class ChatHandler;
class Player;
class PlayerbotAI;

namespace UpkeepNeeds
{
struct NeedRates;
}

// The server side of the planner pool (travel plan WT12a): the instance, the snapshot and the per-bot hook
namespace Planner
{
class QuestIndex;
class QuestStatsView;
class UpkeepIndex;

PlannerPool& Pool();
// World thread: starts the pool with WorldTravel.PlannerThreads workers and its PerfMonitor rows
void StartPlanner();
// Joins the workers; waiting jobs are dropped
void StopPlanner();
// Map thread only: the bot as the planner sees it (WT.11)
Snapshot FillSnapshot(Player* bot, std::uint32_t stateVersion);

// The wear of the bot's equipped items, as the snapshot and the repair errand read it (items that never wear skipped)
struct GearWear
{
    std::uint8_t durabilityPercent{100};  // equipped durability over its maximum
    std::uint8_t lowestPercent{100};      // the most worn equipped item
    std::uint8_t broken{0};               // equipped items at durability 0
};
// Map thread only: one pass over the equipment slots
GearWear ReadGearWear(Player* bot);

// What DurabilityRepairAll(true, 1.0f, false) charges, the core's formula without a reputation discount: the whole
// repair and its cheapest worn item (0 when nothing is worn)
struct RepairPrice
{
    std::uint32_t total{0};
    std::uint32_t cheapest{0};
};
// Map thread only: one pass over the items DurabilityRepairAll repairs
RepairPrice PriceRepair(Player* bot);
// The bot's map thread, first in its new rpg status update, for persistent bots without a real master: takes and
// rechecks its result, tracks its state version and submits a job when one is due. O(1) unless a snapshot is built
void PlannerHook(PlayerbotAI* botAI, Player* bot);

// The planner's goals applied (WT16, GoalInterface.h's rules). The bot's map thread, when its status is idle: the
// current goal goes on, a fresh plan starts its best goal that can start, the bot waits briefly for a coming plan, or
// it falls back to the weighted pick with the reason word for its rpg_status row. `workable` is today's test that a
// log quest has an objective to go to (its points of interest)
struct IdleStep
{
    bool started{false};
    bool wait{false};
    char const* reason{"weights"};
};
IdleStep OnIdle(PlayerbotAI* botAI, Player* bot, std::function<bool(std::uint32_t)> const& workable);
// The bot's map thread: the quest is on its plan's keep list (OrganizeQuestLog keeps it); false without a plan
bool KeepsQuest(PlayerbotAI* botAI, std::uint32_t quest);
// The bot's map thread: of a log quest's points of interest (x, y) the one nearest the plan's tasks for it; -1 = none
int PlannedPoi(PlayerbotAI* botAI, std::uint32_t quest, std::vector<std::pair<float, float>> const& points);
// Any thread: the bot (raw guid) pursues an external goal (bridge or group) or its plan is paused: the manager leaves
// it alone
bool HasExternalGoal(std::uint64_t bot);
namespace GoalApply
{
struct PlanView;
}
// Any thread: a copy of the bot's (raw guid) published goal and plan (the bridge's plan topics); false when the planner
// never saw the bot
bool PublishedPlan(std::uint64_t bot, GoalApply::PlanView& view);
// `playerbots planner goal <bot>`: the bot's group, goal, last ranking and fallback as its map thread published them
bool HandleGoalCommand(ChatHandler* handler, char const* args);
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
// World thread: the spawns that exist only while their game event runs (as TravelMgr::PrepareDestinationCache)
std::unordered_set<std::uint32_t> EventCreatureSpawns();
std::unordered_set<std::uint32_t> EventObjectSpawns();
// World thread: the point's zone and area on its loaded map; false when the map is not loaded or the area unknown
bool ResolveArea(std::uint32_t mapId, float x, float y, float z, std::uint32_t& zoneId, std::uint32_t& areaId);

// The quest index (WT13a, PlannerQuests.cpp; records, camps and the hub rule in QuestIndex.h)
// World thread, once after StartPlannerIndexes: gathers the quests and their spawns, builds the index on a worker
void StartQuestIndex();
// Any thread: the published quest index (null before the build); a reader keeps its copy alive
std::shared_ptr<QuestIndex const> Quests();
// False until the build finished
bool QuestsReady();
// The quest index line of `playerbots planner status|indexes`
std::string QuestIndexStatus();
// `playerbots planner quest <id> | stats | flush`: the quest's entry and statistics, the book's state, a flush now
bool HandleQuestCommand(ChatHandler* handler, char const* args);
// `playerbots planner questuse <quest>`: which item or spell the quest uses on which target (upkeep plan WU20b)
bool HandleQuestUseCommand(ChatHandler* handler, char const* args);

// The upkeep index (upkeep plan WU2, PlannerUpkeep.cpp; records and rules in UpkeepIndex.h)
// World thread, once after StartQuestIndex: gathers the trainers, the vendor stock and the class reagents, builds the
// index on a worker
void StartUpkeepIndex();
// Any thread: the published upkeep index (null before the build); a reader keeps its copy alive
std::shared_ptr<UpkeepIndex const> CurrentUpkeepIndex();
// False until the build finished
bool UpkeepIndexReady();
// The upkeep index lines of `playerbots planner status|indexes`
std::vector<std::string> UpkeepIndexStatus();
// Map thread, from FillSnapshot: the upkeep fields (stock, pet diet, known and learnable trainer spells); no DB, one
// pass over the bags and one HasSpell per trainer spell of the class and riding lists (the class-and-race fit and the
// off-list abilities are precomputed in the index)
void FillUpkeep(Player* bot, Snapshot& snapshot);
// The bot's thread: the rates its needs list reads (UpkeepNeedRules.h NeedRates): its earning rate and travel shares,
// the training reserve and its measured consumable use (the slot measure, WU6); the caller includes UpkeepNeedRules.h
UpkeepNeeds::NeedRates BotNeedRates(PlayerbotAI* botAI, Snapshot const& snapshot);
// `playerbots planner needs <bot>`: an online bot's needs, reserve and shortfall (UpkeepNeedRules.h), from a snapshot
// taken on the world thread while no map updates
bool HandleNeedsCommand(ChatHandler* handler, char const* args);
// The questing circuit (WT13b, PlannerQuests.cpp; the planner in QuestCircuit.h)
struct CircuitWeights;
struct QuestProgress;
namespace Goals
{
struct Situation;
}
// The calling thread: a circuit job over the published quest index, indexes, statistics and crowding, with the core's
// grey level difference taken here. Answers a CircuitAnswer; none when no plan was made or the index was not ready
Compute CircuitCompute(CircuitWeights const& weights, std::vector<std::uint32_t> const& protectedQuests,
                       std::vector<QuestProgress> const& progress);
// World thread: the real-place scenarios (Goldshire, the Crossroads, Grom'gol, each also with a dry hub); true = pass
bool RunCircuitHarness(ChatHandler* handler);
// `playerbots planner circuit selftest | <player name>`: the harness, or the plan for an online player (read only)
bool HandleCircuitCommand(ChatHandler* handler, char const* args);

// The goal model (WT14, GoalModel.h): the ranked goals with reasons
// World thread: the synthetic checks plus the real-data scenarios (the circuit harness's places, synthetic spots)
bool RunGoalsHarness(ChatHandler* handler);
// The calling thread: a goals job (PlanGoals) over the published data, with the plan's kept quests protected and the
// log quests' progress. Answers a GoalsAnswer, also for a failed ranking; none when the quest index was not ready.
// The rates (BotNeedRates, map thread) let the job compute the needs list for the need factor (upkeep WU16)
Compute GoalsCompute(std::vector<std::uint32_t> const& protectedQuests, std::vector<QuestProgress> const& progress,
                     Goals::Situation const& situation, UpkeepNeeds::NeedRates const& rates);
// `playerbots planner goals selftest | <player name>`: the harness, or the ranked goals of an online player (read
// only; writes a `goal` decision row when the world log is on)
bool HandleGoalsCommand(ChatHandler* handler, char const* args);

// The population's quest statistics (WT13a, PlannerQuests.cpp; rules and book in QuestStats.h). StartQuestIndex
// loads the stored totals; events before that are not counted
// The events QuestStatAccepted ... QuestStatDeath are declared in QuestStatEvents.h
// Any thread: the last published view (after the load and each flush); a reader keeps its copy alive
std::shared_ptr<QuestStatsView const> QuestStats();
// World thread: writes the changed cells asynchronously and publishes a new view; returns the rows taken
std::uint32_t QuestStatsFlushNow();
// World thread, every world tick: a timer add; a flush every 300 s
void UpdateQuestStats(std::uint32_t diff);
// World thread, at shutdown before the pools close: the last flush
void QuestStatsShutdown();
}  // namespace Planner

#endif
