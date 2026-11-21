/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlannerFishing.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <utility>
#include <vector>

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PlannerRecipes.h"
#include "PlannerService.h"
#include "QueryResult.h"
#include "ThreadCheck.h"

namespace Planner
{
namespace
{
SharedSlot<Fish::Index> fishBook;
std::atomic<bool> fishStarted{false};

// What the world thread reads: the three stores' rows, as plain values
struct FishInput
{
    std::map<uint32, int32> baseSkills;                      // area -> skill_fishing_base_level
    std::map<uint32, std::map<uint32, uint32>> pools;        // zone -> pool entry -> spawns
    std::map<uint32, std::vector<Recipes::Yield>> poolFish;  // pool entry -> expected items per cast
    std::map<uint32, std::vector<Recipes::Yield>> openFish;  // area -> expected items per cast
};

FishInput GatherFish()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::GatherFish");
    FishInput input;
    if (QueryResult result = WorldDatabase.Query("SELECT CAST(entry AS UNSIGNED), CAST(skill AS SIGNED) FROM "
                                                 "skill_fishing_base_level"))
        do
        {
            Field* fields = result->Fetch();
            input.baseSkills[fields[0].Get<uint64>()] = static_cast<int32>(fields[1].Get<int64>());
        } while (result->NextRow());

    if (QueryResult result = WorldDatabase.Query(
            "SELECT CAST(g.zoneId AS UNSIGNED), CAST(g.id AS UNSIGNED), COUNT(*) FROM gameobject g JOIN "
            "gameobject_template t ON t.entry = g.id WHERE t.type = 25 AND g.zoneId <> 0 GROUP BY g.zoneId, g.id"))
        do
        {
            Field* fields = result->Fetch();
            input.pools[static_cast<uint32>(fields[0].Get<uint64>())][static_cast<uint32>(fields[1].Get<uint64>())] =
                static_cast<uint32>(fields[2].Get<uint64>());
        } while (result->NextRow());

    Recipes::LootTable const references = LoadLootTable("reference_loot_template");
    Recipes::LootTable const openTable = LoadLootTable("fishing_loot_template");
    Recipes::LootTable const poolTable = LoadLootTable("gameobject_loot_template");
    for (auto const& [area, rows] : openTable)
        input.openFish[area] = Recipes::ExpectedYield(openTable, area, &references);
    for (auto const& [zone, entries] : input.pools)
        for (auto const& [entry, spawns] : entries)
            if (!input.poolFish.count(entry))
                if (GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry))
                    input.poolFish[entry] = Recipes::ExpectedYield(poolTable, info->GetLootId(), &references);
    return input;
}

void AddYields(std::vector<Gather::Yield>& into, std::vector<Recipes::Yield> const& from, double weight)
{
    for (Recipes::Yield const& yield : from)
    {
        auto const at =
            std::find_if(into.begin(), into.end(), [&](Gather::Yield const& y) { return y.item == yield.item; });
        if (at == into.end())
            into.push_back({yield.item, yield.expected * weight, 0, 0, 0});
        else
            at->perNode += yield.expected * weight;
    }
}

// Worker: one row per area named by any of the stores; prices stay 0 (the bot's prices are the caller's)
Fish::Index BuildFish(FishInput const& input)
{
    std::map<uint32, Fish::Water> waters;
    auto const row = [&waters](uint32 area) -> Fish::Water&
    {
        Fish::Water& water = waters[area];
        water.area = area;
        return water;
    };
    for (auto const& [area, skill] : input.baseSkills)
        row(area).baseSkill = skill;
    for (auto const& [area, yields] : input.openFish)
        AddYields(row(area).openYields, yields, 1.0);
    for (auto const& [zone, entries] : input.pools)
    {
        Fish::Water& water = row(zone);
        uint32 total = 0;
        for (auto const& [entry, spawns] : entries)
            total += spawns;
        water.pools = total;
        for (auto const& [entry, spawns] : entries)
            if (auto const fish = input.poolFish.find(entry); fish != input.poolFish.end())
                AddYields(water.poolYields, fish->second, static_cast<double>(spawns) / static_cast<double>(total));
    }
    Fish::Index index;
    for (auto& [area, water] : waters)
        index.waters.push_back(std::move(water));
    return index;
}
}  // namespace

void StartFishIndex()
{
    if (fishStarted.exchange(true))
        return;  // once: the stores are static after startup
    auto input = std::make_shared<FishInput>(GatherFish());
    bool const posted = Pool().Post(
        [input]
        {
            try
            {
                auto index = std::make_shared<Fish::Index>(BuildFish(*input));
                uint32 pools = 0;
                uint32 withPools = 0;
                for (Fish::Water const& water : index->waters)
                {
                    pools += water.pools;
                    withPools += water.pools ? 1 : 0;
                }
                LOG_INFO("playerbots",
                         "Fishing index built on a worker: {} waters ({} base skills), {} pools in {} zones",
                         index->waters.size(), input->baseSkills.size(), pools, withPools);
                fishBook.Publish(std::move(index));
            }
            catch (std::exception const& e)
            {
                LOG_ERROR("playerbots", "Fishing index: the build failed: {}", e.what());
            }
        });
    if (!posted)
        LOG_ERROR("playerbots", "Fishing index: no planner worker took the build");
}

std::shared_ptr<Fish::Index const> CurrentFishIndex() { return fishBook.Current(); }
}  // namespace Planner
