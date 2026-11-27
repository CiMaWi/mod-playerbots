/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlannerFishing.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "PlannerIndexes.h"
#include "PlannerPool.h"
#include "PlannerRecipes.h"
#include "PlannerService.h"
#include "PlannerSnapshot.h"
#include "QueryResult.h"
#include "SkillPathLive.h"
#include "SkillPathTypes.h"
#include "ThreadCheck.h"
#include "UpkeepNeedRules.h"

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
    std::map<uint32, std::vector<Fish::Spot>> spots;         // zone -> pool spawn positions
    uint32 resolved{0};                                      // spawns without a zone id, resolved by position
    Recipes::ServerRates rates;                              // the server's, the yields are at them (WR29)
};

FishInput GatherFish()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::GatherFish");
    FishInput input;
    input.rates = ReadServerRates();
    if (QueryResult result = WorldDatabase.Query("SELECT CAST(entry AS UNSIGNED), CAST(skill AS SIGNED) FROM "
                                                 "skill_fishing_base_level"))
        do
        {
            Field* fields = result->Fetch();
            input.baseSkills[fields[0].Get<uint64>()] = static_cast<int32>(fields[1].Get<int64>());
        } while (result->NextRow());

    // Every pool spawn with its position (WR16b: the planner's places); a spawn the world DB gives no zone (a third
    // of them) gets its zone from the map's terrain at its position, as the core's own zone calculation does
    if (QueryResult result = WorldDatabase.Query(
            "SELECT CAST(g.zoneId AS UNSIGNED), CAST(g.id AS UNSIGNED), CAST(g.map AS UNSIGNED), g.position_x, "
            "g.position_y, g.position_z FROM gameobject g JOIN gameobject_template t ON t.entry = g.id WHERE t.type = "
            "25"))
        do
        {
            Field* fields = result->Fetch();
            uint32 zone = static_cast<uint32>(fields[0].Get<uint64>());
            uint32 const entry = static_cast<uint32>(fields[1].Get<uint64>());
            Fish::Spot const spot{static_cast<uint32>(fields[2].Get<uint64>()), fields[3].Get<float>(),
                                  fields[4].Get<float>(), fields[5].Get<float>()};
            if (!zone)
            {
                zone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, spot.map, spot.x, spot.y, spot.z);
                if (!zone)
                    continue;
                ++input.resolved;
            }
            ++input.pools[zone][entry];
            input.spots[zone].push_back(spot);
        } while (result->NextRow());

    Recipes::LootTable const references = LoadLootTable("reference_loot_template");
    Recipes::LootTable const openTable = LoadLootTable("fishing_loot_template");
    Recipes::LootTable const poolTable = LoadLootTable("gameobject_loot_template");
    for (auto const& [area, rows] : openTable)
        input.openFish[area] = Recipes::ExpectedYield(openTable, area, &references, input.rates);
    for (auto const& [zone, entries] : input.pools)
        for (auto const& [entry, spawns] : entries)
            if (!input.poolFish.count(entry))
                if (GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry))
                    input.poolFish[entry] =
                        Recipes::ExpectedYield(poolTable, info->GetLootId(), &references, input.rates);
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
        if (auto const spots = input.spots.find(zone); spots != input.spots.end())
            water.spots = spots->second;
        for (auto const& [entry, spawns] : entries)
            if (auto const fish = input.poolFish.find(entry); fish != input.poolFish.end())
                AddYields(water.poolYields, fish->second, static_cast<double>(spawns) / static_cast<double>(total));
    }
    Fish::Index index;
    for (auto& [area, water] : waters)
        index.waters.push_back(std::move(water));
    index.rates = input.rates;
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
                         "Fishing index built on a worker: {} waters ({} base skills), {} pools in {} zones ({} zones "
                         "resolved by position)",
                         index->waters.size(), input->baseSkills.size(), pools, withPools, input->resolved);
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

void FillFishWaters(Snapshot const& snapshot, SkillPath::Path const* path, Materials::Rates const& rates,
                    Goals::Situation& situation)
{
    auto const skill = std::find_if(snapshot.skills.begin(), snapshot.skills.end(),
                                    [](Skill const& held) { return held.id == Fish::FISHING; });
    if (skill == snapshot.skills.end() || !skill->value)
        return;
    std::shared_ptr<Fish::Index const> const index = CurrentFishIndex();
    Fish::Water const* zone = index ? index->Find(snapshot.zone) : nullptr;
    if (!zone || zone->spots.empty())
        return;
    Fish::Fisher fisher;
    fisher.skill = skill->value;
    fisher.maxSkill = skill->max;
    fisher.rates = rates;
    fisher.server = index->rates;
    if (snapshot.runSpeed > 0.0f)
        fisher.rates.runSpeed = snapshot.runSpeed;
    fisher.interest = UpkeepNeeds::HasInterest(snapshot, TraitRules::Interest::Fishing) ? 1.0 : 0.0;
    for (uint32 const tool : snapshot.toolsHeld)
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tool))
            fisher.poleHeld = fisher.poleHeld || (proto->Class == ITEM_CLASS_WEAPON &&
                                                  proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE);
    if (!fisher.poleHeld)
        return;
    // The fish at the bot's prices: a vendor's, the skill path's for a material it uses (a cook's fish) and a
    // listing's proceeds (WA7a)
    Market::View const market = MarketView();
    Fish::Candidate candidate{*zone, {}};
    for (std::vector<Gather::Yield>* yields : {&candidate.water.openYields, &candidate.water.poolYields})
        for (Gather::Yield& yield : *yields)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(yield.item);
            SkillPath::Material const* material = path ? SkillPath::MaterialOf(*path, yield.item) : nullptr;
            yield.vendorCopper = proto ? proto->SellPrice : 0;
            yield.pathCopper = material ? material->copperEach : 0;
            yield.otherCopper = MarketProceeds(market, yield.item);
        }
    candidate.water.spots.clear();
    bool pool = false;
    if (Fish::Pick({candidate}, fisher, pool) == Fish::NO_PICK)
        return;
    Fish::Spot const* nearest = nullptr;
    float best = 0.0f;
    for (Fish::Spot const& spot : zone->spots)
    {
        if (spot.map != snapshot.position.map)
            continue;
        float const dx = spot.x - snapshot.position.x;
        float const dy = spot.y - snapshot.position.y;
        float const yards = std::sqrt(dx * dx + dy * dy);
        if (!nearest || yards < best)
        {
            nearest = &spot;
            best = yards;
        }
    }
    if (!nearest)
        return;
    Goals::GatherNode option = Fish::TripOption(candidate.water, fisher, candidate.risk, pool, *nearest,
                                                static_cast<float>(Gather::WalkMinutes(best, fisher.rates)));
    if (option.id)
        situation.waters.push_back(std::move(option));
}
}  // namespace Planner
