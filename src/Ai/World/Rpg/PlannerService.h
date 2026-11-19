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
#include "StatusEndRules.h"

enum NewRpgStatus : int;

class ChatHandler;
class Player;
struct CreatureTemplate;
struct ItemTemplate;
class PlayerbotAI;
class SpellInfo;

namespace UpkeepNeeds
{
struct NeedRates;
struct NeedList;
}  // namespace UpkeepNeeds

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
// A planner bot (GoalApply::Owns): it keeps its possessions, is outside the weighted group and no test holds its
// planner. Only the planner starts what such a bot does. O(1)
bool Owns(PlayerbotAI* botAI);
// The errand trigger's edge for a planner bot (full bags, gear due for repair): a snapshot is due, and the fresh plan
// is weighed against the running goal as a changed situation (Goals::ReplanDecision); a job out from before the change
// is not used (the state version moves on). O(1)
void SituationChanged(PlayerbotAI* botAI);
// The errand trigger's bag edge for a planner bot: a change of the bag capacity (a bag lost, sold, bought or put on)
// changes what the bag offers are worth, so it is a changed situation (GoalApply::BagSlotsChanged). O(bag slots)
void NoteBagSlots(PlayerbotAI* botAI);
// The inn trigger's edge for a bot with planner goals (planning WT63d): a bind seen near that turned far
// (GoalApply::BindFarChanged) is a changed situation; the planner weighs the bind at an inn. O(1)
void NoteBindFar(PlayerbotAI* botAI, bool bindFar);
// The bag the planner's running bag purchase (GoalKind::BuyBag in the slot) is for, 0 = none: the errand's vendor
// visit buys that bag, whose worth and price the plan weighed (an order skips the worth rules, 3.19 (7)); once bought
// (NoteBagBought) 0, so the trip's later stops buy no second one (WT63e). O(1)
std::uint32_t OrderedBag(PlayerbotAI* botAI);
// The errand's vendor visit bought the ordered bag: the order is done, and its trip's end clears the retry stamp
// (GoalApply::NoteInterludeEnd: a bag trip that finished without its bag stamps it). O(1)
void NoteBagBought(PlayerbotAI* botAI);
// The status core's end check of a status a planner goal may run (GO_GRIND, WANDER_RANDOM, DO_GATHER, REST), on the
// bot's map thread every update: a planned status ends by its order (GoalApply::PlannedEnd, its outcome and word),
// any other one when `timerOver` (upstream's timer; a sick bot's Upkeep::RestEnds). True when it ended. O(1)
bool StatusOver(PlayerbotAI* botAI, std::uint32_t status, bool timerOver, bool sick = false);
// The bot's map thread, first in its new rpg status update, for persistent bots without a real master: takes and
// rechecks its result, tracks its state version and submits a job when one is due; a taken plan may start its goal
// at once (a switch, ConsiderSwitch). `workable` as for OnIdle. O(1) unless a snapshot is built or a plan is taken
void PlannerHook(PlayerbotAI* botAI, Player* bot, std::function<bool(std::uint32_t)> const& workable);
// The status core (RpgStatus::End) on the bot's map thread, for every status that ends: an end to idle makes a
// snapshot due and ends a wait for a plan; the goal's run learns the outcome and its note (a literal: the trip's
// source or the Status row's word; GoalApply::NoteStatusEnd). O(1)
void OnStatusEnd(PlayerbotAI* botAI, StatusEnd::Outcome outcome, bool toIdle, char const* note = nullptr);

// The planner's goals applied (WT16, GoalInterface.h's rules). The bot's map thread, when its status is idle: the
// current goal goes on, a fresh plan starts its best goal that can start, the bot waits briefly for a coming plan, or
// it falls back to the weighted pick with the reason word for its rpg_status row. `workable` is today's test that a
// log quest has an objective to go to (its points of interest)
struct IdleStep
{
    bool started{false};
    bool wait{false};
    char const* reason{"weights"};
    bool fallback{false};   // a planner bot's fallback: the narrow list (GoalApply::IdleList)
    std::uint32_t note{0};  // the rpg_status row's ref: cannot_start, stalled: the plan's first option's goal kind
};
IdleStep OnIdle(PlayerbotAI* botAI, Player* bot, std::function<bool(std::uint32_t)> const& workable);
// The idle pick's candidates (3.19 (5)): a planner bot's fallback the narrow list, the weighted pick the full one
std::vector<NewRpgStatus> IdleStatuses(IdleStep const& step);
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
// The bot's thread: a weapon of a skill line the bot lacks and would learn at a weapon master (upkeep §3.7, WU15a): its
// class may wield it, and the index's weapon skill fits its class, race and level. Item usage keeps such a weapon
// instead of selling it; one binary search in the index
bool WeaponToLearn(Player* bot, ItemTemplate const* proto);
// The stock rule (WU28, UpkeepNeeds::KeepOf) on a persistent bot's bags, for an item of a stock kind it is not granted
// (no real master): buy below the target, else how many of it to keep; the rest is surplus and sold. One bag walk
struct StockDecision
{
    bool decided{false};    // false: no stock kind of the bot's decides the item, the generic rules do
    bool buy{false};        // below the target: keep all, buy more
    bool kept{false};       // every one the bot carries is kept
    std::uint32_t keep{0};  // how many of it the bot keeps; the rest is surplus
};
StockDecision StockFor(Player* bot, PlayerbotAI* botAI, ItemTemplate const* proto);
// A class spell's reagent: the shopping list buys it for a bot not granted reagents
bool ClassReagent(std::uint32_t item);
// A spell the server charges no reagents for (Glyph of Renewed Life): its reagents are neither kept nor bought
bool CastsWithoutReagent(Player* bot, SpellInfo const* info);
// The bot's thread: the rates its needs list reads (UpkeepNeedRules.h NeedRates): its earning rate and travel shares,
// the training reserve and its measured consumable use (the slot measure, WU6); the caller includes UpkeepNeedRules.h
UpkeepNeeds::NeedRates BotNeedRates(PlayerbotAI* botAI, Snapshot const& snapshot);
// The bot's thread: ComputeNeeds with the bot's rates, timed into the PerfMonitor row "errand needs computed" (calls =
// computations, WU27). The errand files' sites call it; the planner's own paths do not. The caller includes
// UpkeepNeedRules.h.
UpkeepNeeds::NeedList ComputeErrandNeeds(PlayerbotAI* botAI, Snapshot const& snapshot, UpkeepIndex const& index);
// World thread (PLAYERBOT_CHECK_WORLD_THREAD, no map updates): the bot's snapshot, rates and needs list, as `planner
// needs` and the bridge's `needs` topic read them (one source); false while the upkeep index is not ready. The caller
// includes UpkeepNeedRules.h.
bool BotNeedsNow(Player* bot, PlayerbotAI* botAI, Snapshot& snapshot, UpkeepNeeds::NeedRates& rates,
                 UpkeepNeeds::NeedList& list);
// `playerbots planner needs <bot>`: an online bot's needs, reserve and shortfall (UpkeepNeedRules.h), from a snapshot
// taken on the world thread while no map updates
bool HandleNeedsCommand(ChatHandler* handler, char const* args);
// The pet fixture of the functional tests (WU21b, a test cheat): playerbots planner pet <bot> [has|none|unpet|give
// <entry>]
bool HandlePetCommand(ChatHandler* handler, char const* args);
// Any thread: the beast's own spells break a tame's channel (UpkeepNeedRules.h BreaksTameChannel over the effects of
// its creature_template_spell spells and its SmartAI's casts); such a beast is no tame target (WU.4): the index
// gathers no spawn of it and the search for a beast at the stop passes it over. Reads the core's loaded stores only
bool BreaksTameChannel(CreatureTemplate const& beast);
// The questing circuit (WT13b, PlannerQuests.cpp; the planner in QuestCircuit.h)
struct CircuitWeights;
struct QuestProgress;
namespace Goals
{
struct InnSeen;
struct Situation;
}  // namespace Goals
// The live Situation a goals job gets (health, mana, vendor, repairer, bag offers, the inns when the bind is far); map
// thread, or the world thread while no map updates (`playerbots planner goals <bot>`)
Goals::Situation LiveSituation(PlayerbotAI* botAI, Player* bot);
// The innkeepers of the index the bot can use (the core's interact test) on the point's map within maxYards (0: any),
// nearest first, each with its bind spot and whether its zone fits the bot's level: the planner's inns and the bind
// entry's pick (planning WT63d). Map thread
std::vector<Goals::InnSeen> InnsNear(Player* bot, PlannerIndexes const& indexes, Place const& at, float maxYards);
// `playerbots planner inns <bot>`: the old inn list (TravelMgr::GetInns) against the index's innkeepers, the nearest
// of each at every hub of every map, the differences with their causes (the WT63d lookup harness). World thread
bool HandleInnsCommand(ChatHandler* handler, char const* args);
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
