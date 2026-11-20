/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatherNodeMgr.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GatherRules.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Object.h"
#include "ObjectMgr.h"
#include "PlannerRecipes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PoolMgr.h"
#include "SharedDefines.h"
#include "Timer.h"

namespace
{
// The place risk (professions WR13b) [E]: the hostile creatures spawned this near a spawn point, found through a grid;
// with none that near, those of the wider ring (open zones space their creatures about 90 yd apart, so a node with
// none within 40 yd still lies among them: run scenario-1795179494905, Un'Goro)
constexpr float RISK_YARDS = 40.0f;
constexpr float RISK_WIDE_YARDS = 120.0f;
constexpr float RISK_CELL_YARDS = 64.0f;
constexpr uint32 CELL_MASK = 0xFFFFFF;

struct Hostile
{
    float x;
    float y;
    float z;
    float level;
    bool elite;
};

uint64 CellKey(uint32 mapId, int32 cellX, int32 cellY)
{
    return (static_cast<uint64>(mapId) << 48) | (static_cast<uint64>(static_cast<uint32>(cellX) & CELL_MASK) << 24) |
           (static_cast<uint32>(cellY) & CELL_MASK);
}

int32 CellOf(float coordinate) { return static_cast<int32>(std::floor(coordinate / RISK_CELL_YARDS)); }

// The continents' creature spawns hostile to players (no critters, triggers or unattackable ones) by grid cell, from
// the spawn data in memory
std::unordered_map<uint64, std::vector<Hostile>> HostilesByCell()
{
    std::unordered_map<uint64, std::vector<Hostile>> cells;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        MapEntry const* mapEntry = sMapStore.LookupEntry(data.mapid);
        if (!mapEntry || !mapEntry->IsContinent())
            continue;
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(data.id);
        if (!info || info->type == CREATURE_TYPE_CRITTER || (info->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER) ||
            (info->unit_flags & (UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE)))
            continue;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction);
        if (!faction || !faction->IsHostileToPlayers())
            continue;
        bool const elite = info->rank == CREATURE_ELITE_ELITE || info->rank == CREATURE_ELITE_RAREELITE ||
                           info->rank == CREATURE_ELITE_WORLDBOSS;
        cells[CellKey(data.mapid, CellOf(data.posX), CellOf(data.posY))].push_back(
            {data.posX, data.posY, data.posZ, (info->minlevel + info->maxlevel) / 2.0f, elite});
    }
    return cells;
}

// The hostile creatures within `yards` of the spawn: their count, summed level and elites
struct Near
{
    uint32 count = 0;
    double levels = 0.0;
    uint32 elites = 0;
};

Near HostilesNear(GatherNodeSpawn const& node, float yards,
                  std::unordered_map<uint64, std::vector<Hostile>> const& cells)
{
    float const x = node.pos.GetPositionX();
    float const y = node.pos.GetPositionY();
    float const z = node.pos.GetPositionZ();
    int32 const reach = static_cast<int32>(std::ceil(yards / RISK_CELL_YARDS));
    Near found;
    for (int32 cellX = CellOf(x) - reach; cellX <= CellOf(x) + reach; ++cellX)
        for (int32 cellY = CellOf(y) - reach; cellY <= CellOf(y) + reach; ++cellY)
        {
            auto const cell = cells.find(CellKey(node.pos.GetMapId(), cellX, cellY));
            if (cell == cells.end())
                continue;
            for (Hostile const& hostile : cell->second)
            {
                float const dx = hostile.x - x;
                float const dy = hostile.y - y;
                float const dz = hostile.z - z;
                if (dx * dx + dy * dy + dz * dz > yards * yards)
                    continue;
                found.levels += hostile.level;
                ++found.count;
                found.elites += hostile.elite ? 1 : 0;
            }
        }
    return found;
}

// The spawn's danger: the hostile creatures within RISK_YARDS, else within RISK_WIDE_YARDS, their mean level and
// elite share; none: 0 (Load gives it its zone's). True when a hostile one is within RISK_YARDS
bool FillRisk(GatherNodeSpawn& node, std::unordered_map<uint64, std::vector<Hostile>> const& cells)
{
    Near found = HostilesNear(node, RISK_YARDS, cells);
    bool const close = found.count > 0;
    if (!close)
        found = HostilesNear(node, RISK_WIDE_YARDS, cells);
    node.mobLevel = found.count ? static_cast<float>(found.levels / found.count) : 0.0f;
    node.eliteShare = found.count ? static_cast<float>(found.elites) / static_cast<float>(found.count) : 0.0f;
    return close;
}

bool GetGatherSkillFromTemplate(GameObjectTemplate const* goInfo, uint32& skillId, uint32& reqSkillValue)
{
    if (!goInfo || goInfo->type != GAMEOBJECT_TYPE_CHEST)
        return false;

    uint32 lockId = goInfo->GetLockId();
    if (!lockId)
        return false;

    LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
    if (!lockInfo)
        return false;

    for (uint8 j = 0; j < 8; ++j)
    {
        if (lockInfo->Type[j] != LOCK_KEY_SKILL)
            continue;

        uint32 skill = SkillByLockType(LockType(lockInfo->Index[j]));
        if (skill == SKILL_HERBALISM || skill == SKILL_MINING)
        {
            skillId = skill;
            reqSkillValue = std::max(1u, lockInfo->Skill[j]);
            return true;
        }
    }

    return false;
}

// A member's level of the pool chain: the pool's limit, the member's explicit chance and the pool's chances
template <class T>
Gather::PoolLevel LevelOf(PoolGroup<T> const& group, uint32 maxLimit, uint32 member)
{
    Gather::PoolLevel level;
    level.maxLimit = maxLimit;
    level.equalMembers = static_cast<uint32>(group.GetEqualChanced().size());
    for (PoolObject const& object : group.GetExplicitlyChanced())
    {
        level.explicitSum += object.chance;
        if (object.guid == member)
            level.chance = object.chance;
    }
    return level;
}

// The spawn's pools (PoolMgr, loaded before this index): its own pool, then the mother pools above it; empty
// outside a pool
std::vector<Gather::PoolLevel> PoolChainOf(ObjectGuid::LowType spawnId)
{
    std::vector<Gather::PoolLevel> chain;
    uint32 poolId = sPoolMgr->IsPartOfAPool<GameObject>(spawnId);
    PoolGroup<GameObject> const* group = poolId ? sPoolMgr->GetPoolGameObjectGroup(poolId) : nullptr;
    PoolTemplateData const* data = poolId ? sPoolMgr->GetPoolTemplate(poolId) : nullptr;
    if (!group || !data)
        return chain;
    chain.push_back(LevelOf(*group, data->MaxLimit, spawnId));
    while (chain.size() < Gather::POOL_DEPTH)
    {
        uint32 const mother = sPoolMgr->IsPartOfAPool<Pool>(poolId);
        PoolGroup<Pool> const* motherGroup = mother ? sPoolMgr->GetPoolPoolGroup(mother) : nullptr;
        PoolTemplateData const* motherData = mother ? sPoolMgr->GetPoolTemplate(mother) : nullptr;
        if (!motherGroup || !motherData)
            break;
        chain.push_back(LevelOf(*motherGroup, motherData->MaxLimit, poolId));
        poolId = mother;
    }
    return chain;
}
}  // namespace

void GatherNodeMgr::Load()
{
    uint32 oldMSTime = getMSTime();
    uint32 count = 0;
    uint32 pooled = 0;
    double shares = 0.0;
    uint32 guarded = 0;
    double mobLevels = 0.0;
    std::unordered_map<uint64, std::vector<Hostile>> const hostiles = HostilesByCell();

    // Loot ids with at least one freely lootable row. Quest-gated
    // "gathering" chests (Cactus Apple, Serpentbloom, ...) carry a
    // herbalism/mining lock but ALL their loot rows are QuestRequired, so
    // bots without the quest can't harvest them - and they never despawn,
    // which makes them permanent live-node bait. Note that filtering on
    // gameobject_questitem instead would be wrong: standard herbs carry a
    // conditional quest item (Root Sample) on top of their normal loot.
    std::unordered_set<uint32> openLootIds;
    if (QueryResult result =
            WorldDatabase.Query("SELECT DISTINCT Entry FROM gameobject_loot_template WHERE QuestRequired = 0"))
    {
        do
        {
            openLootIds.insert((*result)[0].Get<uint32>());
        } while (result->NextRow());
    }

    std::unordered_map<ObjectGuid::LowType, uint32> dbZoneIds;
    if (QueryResult result = WorldDatabase.Query("SELECT guid, zoneId FROM gameobject WHERE zoneId <> 0"))
    {
        do
        {
            dbZoneIds[(*result)[0].Get<uint32>()] = (*result)[1].Get<uint32>();
        } while (result->NextRow());
    }

    for (auto const& [spawnId, goData] : sObjectMgr->GetAllGOData())
    {
        // Instances are excluded: the state targets outdoor zone roaming.
        MapEntry const* mapEntry = sMapStore.LookupEntry(goData.mapid);
        if (!mapEntry || !mapEntry->IsContinent())
            continue;

        GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(goData.id);

        uint32 skillId = 0;
        uint32 reqSkillValue = 0;
        if (!GetGatherSkillFromTemplate(goInfo, skillId, reqSkillValue))
            continue;

        if (!openLootIds.contains(goInfo->GetLootId()))
            continue;

        GatherNodeSpawn node;
        node.spawnId = spawnId;
        node.pos = WorldPosition(goData.mapid, goData.posX, goData.posY, goData.posZ);
        node.skillId = skillId;
        node.reqSkillValue = reqSkillValue;
        node.entry = goData.id;
        node.respawnSecs = static_cast<uint32>(std::abs(goData.spawntimesecs));
        std::vector<Gather::PoolLevel> const chain = PoolChainOf(spawnId);
        node.share = Gather::PoolShare(chain);
        pooled += chain.empty() ? 0 : 1;
        shares += node.share;
        auto zoneItr = dbZoneIds.find(spawnId);
        uint32 zoneId = zoneItr != dbZoneIds.end()
                            ? zoneItr->second
                            : sMapMgr->GetZoneId(PHASEMASK_NORMAL, goData.mapid, goData.posX, goData.posY, goData.posZ);
        guarded += FillRisk(node, hostiles) ? 1 : 0;
        _nodes[goData.mapid][zoneId].push_back(std::move(node));
        ++count;
    }
    // A spawn with no hostile creature in either ring takes the mean danger of its zone's spawns that have one: the
    // zone's creatures lie on the way to it (lab12 run scn-261120153254: two quiet spots in Un'Goro made a level-12
    // bot's circuit)
    for (auto& [mapId, zones] : _nodes)
        for (auto& [zoneId, spawns] : zones)
        {
            double zoneLevels = 0.0;
            double zoneElites = 0.0;
            uint32 zoneGuarded = 0;
            for (GatherNodeSpawn const& spawn : spawns)
                if (spawn.mobLevel > 0.0f)
                {
                    zoneLevels += spawn.mobLevel;
                    zoneElites += spawn.eliteShare;
                    ++zoneGuarded;
                }
            for (GatherNodeSpawn& spawn : spawns)
            {
                if (zoneGuarded && spawn.mobLevel <= 0.0f)
                {
                    spawn.mobLevel = static_cast<float>(zoneLevels / zoneGuarded);
                    spawn.eliteShare = static_cast<float>(zoneElites / zoneGuarded);
                }
                mobLevels += spawn.mobLevel;
            }
        }
    _bySpawn.clear();
    for (auto const& [mapId, zones] : _nodes)
        for (auto const& [zoneId, spawns] : zones)
            for (GatherNodeSpawn const& spawn : spawns)
                _bySpawn[spawn.spawnId] = &spawn;
    LoadYields();

    LOG_INFO("playerbots",
             ">> Loaded {} gather node spawns ({} pooled, mean share {:.2f}; {} near hostile creatures, mean danger "
             "level {:.1f}) in {} ms",
             count, pooled, count ? shares / count : 0.0, guarded, count ? mobLevels / count : 0.0,
             GetMSTimeDiffToNow(oldMSTime));
}

bool GatherNodeMgr::IsUsable(PlayerbotAI* botAI, Player* bot, GatherNodeSpawn const& node)
{
    if (!botAI->HasSkill(SkillType(node.skillId)))
        return false;

    return bot->GetSkillValue(node.skillId) >= node.reqSkillValue;
}

std::vector<GatherNodeSpawn> const* GatherNodeMgr::GetZoneNodes(uint32 mapId, uint32 zoneId) const
{
    auto mapItr = _nodes.find(mapId);
    if (mapItr == _nodes.end())
        return nullptr;

    auto zoneItr = mapItr->second.find(zoneId);
    return zoneItr != mapItr->second.end() ? &zoneItr->second : nullptr;
}

bool GatherNodeMgr::HasUsableNodes(Player* bot)
{
    std::vector<GatherNodeSpawn> const* nodes = GetZoneNodes(bot->GetMapId(), bot->GetZoneId());
    if (!nodes)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    for (GatherNodeSpawn const& node : *nodes)
        if (IsUsable(botAI, bot, node))
            return true;

    return false;
}

GameObject* GatherNodeMgr::FindLiveNode(Map* map, ObjectGuid::LowType spawnId)
{
    auto bounds = map->GetGameObjectBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
        if (it->second->isSpawned() && it->second->GetGoState() == GO_STATE_READY)
            return it->second;

    return nullptr;
}

bool GatherNodeMgr::IsVerifiablyDown(Map* map, WorldPosition const& pos, ObjectGuid::LowType spawnId)
{
    // An unloaded grid means "unknown", not "not up".
    if (!map->IsGridLoaded(pos.GetPositionX(), pos.GetPositionY()))
        return false;

    return !FindLiveNode(map, spawnId);
}

GatherNodeSpawn const* GatherNodeMgr::GetNearestLiveNode(Player* bot,
                                                         std::unordered_set<ObjectGuid::LowType> const& visited,
                                                         float radius)
{
    auto mapItr = _nodes.find(bot->GetMapId());
    if (mapItr == _nodes.end())
        return nullptr;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;

    Map* map = bot->GetMap();
    WorldPosition botPos(bot);
    GatherNodeSpawn const* nearest = nullptr;
    float nearestDistSq = radius * radius;
    // All zone buckets of the map: this query is deliberately not
    // zone-scoped, and a radius this small prunes nearly everything on the
    // cheap distance check.
    for (auto const& [zoneId, nodes] : mapItr->second)
    {
        for (GatherNodeSpawn const& node : nodes)
        {
            float distSq = botPos.sqDistance(node.pos);
            if (distSq > nearestDistSq)
                continue;

            if (visited.contains(node.spawnId) || !IsUsable(botAI, bot, node))
                continue;

            if (!map->IsGridLoaded(node.pos.GetPositionX(), node.pos.GetPositionY()))
                continue;

            if (!FindLiveNode(map, node.spawnId))
                continue;

            nearest = &node;
            nearestDistSq = distSq;
        }
    }

    return nearest;
}

// The expected loot of every gather node template with spawns and of every skinning loot id (professions WR13c): the
// pass-by gate prices a node or a hide it sees from these, immutable after the load
void GatherNodeMgr::LoadYields()
{
    _nodeYields.clear();
    _hideYields.clear();
    Recipes::LootTable const references = Planner::LoadLootTable("reference_loot_template");
    auto const yieldsOf = [&references](Recipes::LootTable const& table, uint32 entry)
    {
        std::vector<GatherYield> yields;
        for (Recipes::Yield const& yield : Recipes::ExpectedYield(table, entry, &references))
            yields.push_back({yield.item, yield.expected});
        return yields;
    };
    Recipes::LootTable const nodeTable = Planner::LoadLootTable("gameobject_loot_template");
    for (auto const& [mapId, zones] : _nodes)
        for (auto const& [zoneId, spawns] : zones)
            for (GatherNodeSpawn const& spawn : spawns)
                if (!_nodeYields.contains(spawn.entry))
                    if (GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(spawn.entry))
                        _nodeYields[spawn.entry] = yieldsOf(nodeTable, info->GetLootId());
    Recipes::LootTable const skinTable = Planner::LoadLootTable("skinning_loot_template");
    for (auto const& [lootId, rows] : skinTable)
        _hideYields[lootId] = yieldsOf(skinTable, lootId);
    LOG_INFO("playerbots", ">> Gather yields: {} node templates, {} skinning loot ids", _nodeYields.size(),
             _hideYields.size());
}
