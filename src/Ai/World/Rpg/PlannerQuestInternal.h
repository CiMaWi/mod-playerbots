/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERQUESTINTERNAL_H
#define _PLAYERBOT_PLANNERQUESTINTERNAL_H

// What the quest index's server side (PlannerQuests.cpp) shares with its GM commands and harnesses
// (PlannerQuestCommands.cpp) and the field map's (PlannerFields.cpp): the circuit job's published data, the field map's
// gather and build, and a few statistics and census helpers. Included by those .cpp files only, never by a header

#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "Define.h"
#include "FieldMap.h"
#include "ObjectGuid.h"
#include "QuestCircuit.h"

class ChatHandler;
struct CreatureData;
struct GameObjectData;

namespace Planner
{
class UpkeepIndex;

// The published data a circuit job reads, taken on the calling thread with the core's grey level difference
struct CircuitData
{
    std::shared_ptr<PlannerIndexes const> indexes;
    std::shared_ptr<QuestIndex const> quests;
    std::shared_ptr<QuestStatsView const> stats;
    std::shared_ptr<CrowdingSnapshot const> crowding;
    std::shared_ptr<FieldMap const> fields;  // WQ13: held by a planner job like the other indexes; WQ14 reads it
    uint32 greyDiff{Circuit::GREY_DIFF};

    // The pointers stay valid while this data (or a copy of its shared pointers) lives; travel: the way's price
    // (a goals job's JobTravel), empty = straight yards
    CircuitContext Context(TravelCost travel = {}) const
    {
        CircuitContext context;
        context.travel = std::move(travel);
        context.quests = quests.get();
        context.indexes = indexes.get();
        context.stats = stats.get();
        context.crowding = crowding.get();
        context.greyDiff = greyDiff;
        return context;
    }
};

// Taken on the calling thread: the published indexes, quests, statistics and crowding, each null while not built
CircuitData CircuitDataNow();

// The class-quest table's spells the bot's class trainer teaches (upkeep WU20a): their quests are ordinary ones
std::vector<uint32> ClassTrainerTaught(UpkeepIndex const& upkeep, uint8 cls);

inline uint32 MsSince(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

// Whether the quest statistics are loaded; events before the load are not counted
bool QuestStatsLoaded();
// `planner quest stats`: the statistics' counts, load and flushes
bool HandleQuestStats(ChatHandler* handler);
// The quest's statistics per level band, under `planner quest <id>`
void SendQuestStats(ChatHandler* handler, uint32 questId);
// WT36a: the capability census's log line at the index build (`planner capability` prints it in full)
void LogCapabilityCensus(QuestIndex const& quests);

// WQ13, PlannerFields.cpp: a continent's creature or object spawn as a field map row (world thread, in the gather);
// none off the continents or without a template
std::optional<FieldSpawn> FieldCreature(ObjectGuid::LowType guid, CreatureData const& data);
std::optional<FieldSpawn> FieldObject(ObjectGuid::LowType guid, GameObjectData const& data);
// The gather has started: FieldsReady stays false until the build publishes
void FieldMapGatherStarted();
// Posts the field map's build of the gathered spawns to the planner pool; logs its size and time, then publishes it
void PostFieldMapBuild(std::shared_ptr<std::vector<FieldSpawn>> spawns, uint32 gatherMs);
}  // namespace Planner

#endif
