/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The field map's server side (questing WQ13): a continent spawn's row, taken in the quest index's gather
// (PlannerQuests.cpp), the worker build and the published map. Apart from PlannerQuests.cpp since landing WQL5a
// (consolidation CS37's line ceiling).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "DBCStores.h"
#include "GatherRules.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PlannerIndexes.h"
#include "PlannerPool.h"
#include "PlannerQuestInternal.h"
#include "PlannerService.h"
#include "PoolMgr.h"

namespace Planner
{
namespace
{
enum class FieldMapState : uint8
{
    NotStarted,
    Building,
    Ready,
    Failed
};

constexpr double BYTES_PER_MB = 1024.0 * 1024.0;

std::atomic<FieldMapState> fieldState{FieldMapState::NotStarted};
SharedSlot<FieldMap> fieldBook;

// WQ13: one level of a spawn's pool chain (GatherNodeMgr.cpp's LevelOf, for creatures too)
template <class T>
Gather::PoolLevel FieldPoolLevel(PoolGroup<T> const& group, uint32 maxLimit, uint32 member)
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

// WQ13: the share of the time a spawn stands, over its pool and the mother pools above it (Gather::PoolShare, as the
// gather nodes count it); 1 outside a pool
template <class T>
float FieldPoolShare(ObjectGuid::LowType spawnId)
{
    uint32 poolId = sPoolMgr->IsPartOfAPool<T>(spawnId);
    if (!poolId)
        return 1.0f;
    PoolGroup<T> const* group = nullptr;
    if constexpr (std::is_same_v<T, Creature>)
        group = sPoolMgr->GetPoolCreatureGroup(poolId);
    else
        group = sPoolMgr->GetPoolGameObjectGroup(poolId);
    PoolTemplateData const* data = sPoolMgr->GetPoolTemplate(poolId);
    if (!group || !data)
        return 1.0f;
    std::vector<Gather::PoolLevel> chain{FieldPoolLevel(*group, data->MaxLimit, spawnId)};
    while (chain.size() < Gather::POOL_DEPTH)
    {
        uint32 const mother = sPoolMgr->IsPartOfAPool<::Pool>(poolId);
        PoolGroup<::Pool> const* motherGroup = mother ? sPoolMgr->GetPoolPoolGroup(mother) : nullptr;
        PoolTemplateData const* motherData = mother ? sPoolMgr->GetPoolTemplate(mother) : nullptr;
        if (!motherGroup || !motherData)
            break;
        chain.push_back(FieldPoolLevel(*motherGroup, motherData->MaxLimit, poolId));
        poolId = mother;
    }
    return Gather::PoolShare(chain);
}

bool OnContinent(uint32 mapId)
{
    MapEntry const* map = sMapStore.LookupEntry(mapId);
    return map && map->IsContinent();
}
}  // namespace

// WQ13: a continent's creature spawn for the field map; hostile as GatherNodeMgr's risk cells count it (hostile to
// players, no critter, trigger or unattackable creature)
std::optional<FieldSpawn> FieldCreature(ObjectGuid::LowType guid, CreatureData const& data)
{
    CreatureTemplate const* info = OnContinent(data.mapid) ? sObjectMgr->GetCreatureTemplate(data.id) : nullptr;
    if (!info)
        return std::nullopt;
    FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction);
    FieldSpawn spawn;
    spawn.kind = FieldKind::Creature;
    spawn.entry = data.id;
    spawn.mapId = data.mapid;
    spawn.x = data.posX;
    spawn.y = data.posY;
    spawn.z = data.posZ;
    spawn.minLevel = info->minlevel;
    spawn.maxLevel = std::max(info->minlevel, info->maxlevel);
    spawn.elite = info->rank == CREATURE_ELITE_ELITE || info->rank == CREATURE_ELITE_RAREELITE ||
                  info->rank == CREATURE_ELITE_WORLDBOSS;
    spawn.hostile = faction && faction->IsHostileToPlayers() && info->type != CREATURE_TYPE_CRITTER &&
                    !(info->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER) &&
                    !(info->unit_flags & (UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE));
    spawn.respawnSecs = data.spawntimesecs;
    spawn.share = FieldPoolShare<Creature>(guid);
    return spawn;
}

// WQ13: a continent's object spawn for the field map; a respawn of 0 or less (spawned by a script) is none
std::optional<FieldSpawn> FieldObject(ObjectGuid::LowType guid, GameObjectData const& data)
{
    if (!OnContinent(data.mapid) || !sObjectMgr->GetGameObjectTemplate(data.id))
        return std::nullopt;
    FieldSpawn spawn;
    spawn.kind = FieldKind::Object;
    spawn.entry = data.id;
    spawn.mapId = data.mapid;
    spawn.x = data.posX;
    spawn.y = data.posY;
    spawn.z = data.posZ;
    spawn.respawnSecs = data.spawntimesecs > 0 ? static_cast<uint32>(data.spawntimesecs) : 0;
    spawn.share = FieldPoolShare<GameObject>(guid);
    return spawn;
}

void FieldMapGatherStarted() { fieldState.store(FieldMapState::Building, std::memory_order_release); }

void PostFieldMapBuild(std::shared_ptr<std::vector<FieldSpawn>> spawns, uint32 gatherMs)
{
    bool const posted = Pool().Post(
        [spawns = std::move(spawns), gatherMs]
        {
            auto const buildStart = std::chrono::steady_clock::now();
            try
            {
                auto fields = std::make_shared<FieldMap>(FieldMap::Build(std::move(*spawns)));
                FieldMapStats& s = fields->MutableStats();
                s.gatherMs = gatherMs;
                s.buildMs = MsSince(buildStart);
                LOG_INFO("playerbots",
                         "Field map built in {} ms on a worker: {} rows ({} creature, {} object), {} cells, {} "
                         "spawns ({} hostile), about {:.1f} MB (estimate)",
                         s.buildMs, s.rows, s.creatureRows, s.objectRows, s.cells, s.spawns, s.hostiles,
                         static_cast<double>(fields->ApproxBytes()) / BYTES_PER_MB);
                fieldBook.Publish(std::move(fields));
                fieldState.store(FieldMapState::Ready, std::memory_order_release);
            }
            catch (std::exception const& e)
            {
                fieldState.store(FieldMapState::Failed, std::memory_order_release);
                LOG_ERROR("playerbots", "Field map: the build failed: {}", e.what());
            }
        });
    if (!posted)
    {
        fieldState.store(FieldMapState::Failed, std::memory_order_release);
        LOG_ERROR("server.loading", "Field map: the planner pool is not running; no field map");
    }
}

std::shared_ptr<FieldMap const> Fields() { return fieldBook.Current(); }

bool FieldsReady() { return fieldState.load(std::memory_order_acquire) == FieldMapState::Ready; }
}  // namespace Planner
