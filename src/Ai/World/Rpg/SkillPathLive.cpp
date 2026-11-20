/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SkillPathLive.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "DBCStores.h"
#include "GoalInterface.h"
#include "ItemDemand.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerIndexes.h"
#include "PlannerRecipes.h"
#include "PlannerService.h"
#include "PlayerbotAI.h"
#include "SkillPathBridge.h"

namespace Planner
{
namespace
{
constexpr std::uint8_t TEAM_ALLIANCE_SNAPSHOT = 0;  // Snapshot::team

std::vector<SkillPathBridge::ZoneBracket> BracketsOf(PlannerIndexes const* indexes, std::uint8_t team)
{
    std::vector<SkillPathBridge::ZoneBracket> zones;
    if (!indexes)
        return zones;
    zones.reserve(indexes->zones.size());
    for (ZoneRecord const& zone : indexes->zones)
    {
        bool const alliance = team == TEAM_ALLIANCE_SNAPSHOT;
        bool const open = zone.side == ZoneSide::Contested || zone.side == ZoneSide::Sanctuary ||
                          (zone.side == ZoneSide::Alliance) == alliance;
        zones.push_back({zone.zoneId, zone.low, zone.high, zone.mapId, open});
    }
    return zones;
}

// The totem categories the held tools cover: the core's test without its skill check (Player::
// IsTotemCategoryCompatiableWith). Item templates and the store are static data, read only
std::vector<std::uint32_t> CategoriesOf(std::vector<std::uint32_t> const& tools)
{
    std::vector<std::uint32_t> categories;
    for (std::uint32_t const tool : tools)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tool);
        TotemCategoryEntry const* held =
            proto && proto->TotemCategory ? sTotemCategoryStore.LookupEntry(proto->TotemCategory) : nullptr;
        if (!held)
            continue;
        for (std::uint32_t id = 0; id < sTotemCategoryStore.GetNumRows(); ++id)
            if (TotemCategoryEntry const* entry = sTotemCategoryStore.LookupEntry(id))
                if (entry->categoryType == held->categoryType &&
                    (held->categoryMask & entry->categoryMask) == entry->categoryMask)
                    categories.push_back(id);
    }
    std::sort(categories.begin(), categories.end());
    categories.erase(std::unique(categories.begin(), categories.end()), categories.end());
    return categories;
}

// What one unit fetches at a vendor: the item template's sell price (static data)
double SellCopper(std::uint32_t item)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    return proto ? static_cast<double>(proto->SellPrice) : 0.0;
}
}  // namespace

PathData PathDataNow()
{
    PathData data;
    if (RecipeIndexReady())
        data.recipes = CurrentRecipeIndex();
    if (IndexesReady())
        data.indexes = Indexes();
    return data;
}

SkillPath::PathInputs PathInputsOf(PlayerbotAI* botAI)
{
    SkillPath::PathInputs inputs;
    if (botAI && ItemUse::DemandAnswers(botAI))
        inputs.consumables = SkillPathBridge::ConsumablesOf(ItemUse::DemandOf(botAI));
    return inputs;
}

std::shared_ptr<SkillPath::Path const> NeedsWithPath(Snapshot const& snapshot, UpkeepIndex const& upkeep,
                                                     UpkeepNeeds::NeedRates const& rates,
                                                     SkillPath::PathInputs const& inputs, PathData const& data,
                                                     UpkeepNeeds::NeedList& needs)
{
    if (!data.recipes || !snapshot.person)
    {
        needs = UpkeepNeeds::ComputeNeeds(snapshot, upkeep, SkillPathBridge::WithoutPath(rates));
        return nullptr;
    }
    auto const started = std::chrono::steady_clock::now();
    SkillPath::PathWorld const world{
        *data.recipes, [](std::uint32_t spell) { return UpkeepNeeds::MayLearn(spell); }, SellCopper, {}, {}};
    SkillPathBridge::Computed computed =
        SkillPathBridge::Compute(snapshot, upkeep, rates, inputs, world, BracketsOf(data.indexes.get(), snapshot.team),
                                 CategoriesOf(snapshot.toolsHeld));
    needs = std::move(computed.needs);
    auto path = std::make_shared<SkillPath::Path const>(std::move(computed.path));
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "profession path computed"))
        row->Record(static_cast<uint64>(std::max<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count(),
            0)));
    return path;
}

void AddKeptPath(PlayerbotAI* botAI, UpkeepNeeds::NeedRates& rates)
{
    GoalApply::BotGoals const* goals = botAI ? botAI->planner.goals.get() : nullptr;
    if (goals && goals->path)
        rates = SkillPathBridge::WithPath(rates, *goals->path);
}
}  // namespace Planner
