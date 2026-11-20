/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatherNodeMgr.h"

#include <algorithm>
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
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PoolMgr.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"

namespace
{
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
        _nodes[goData.mapid][zoneId].push_back(std::move(node));
        ++count;
    }

    LOG_INFO("playerbots", ">> Loaded {} gather node spawns ({} pooled, mean share {:.2f}) in {} ms", count, pooled,
             count ? shares / count : 0.0, GetMSTimeDiffToNow(oldMSTime));
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

GatherNodeSpawn const* GatherNodeMgr::GetNextNode(Player* bot, std::unordered_set<ObjectGuid::LowType> const& visited)
{
    std::vector<GatherNodeSpawn> const* nodes = GetZoneNodes(bot->GetMapId(), bot->GetZoneId());
    if (!nodes)
        return nullptr;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;

    Map* map = bot->GetMap();
    WorldPosition botPos(bot);

    std::vector<std::pair<float, GatherNodeSpawn const*>> candidates;
    for (GatherNodeSpawn const& node : *nodes)
    {
        if (visited.contains(node.spawnId) || !IsUsable(botAI, bot, node))
            continue;

        // Don't route to spawn points we can already see to be empty.
        // Evaluated fresh on every selection so respawns come back into
        // the pool naturally.
        if (IsVerifiablyDown(map, node.pos, node.spawnId))
            continue;

        candidates.push_back({botPos.sqDistance(node.pos), &node});
    }

    if (candidates.empty())
        return nullptr;

    uint32 nearestCount = std::min<uint32>(4, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + nearestCount, candidates.end(),
                      [](auto const& a, auto const& b) { return a.first < b.first; });

    return candidates[urand(0, nearestCount - 1)].second;
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
