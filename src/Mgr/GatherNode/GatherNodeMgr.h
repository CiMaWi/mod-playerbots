/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOT_GATHERNODEMGR_H
#define PLAYERBOT_GATHERNODEMGR_H

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ObjectGuid.h"
#include "TravelMgr.h"

class GameObject;
class Map;
class Player;
class PlayerbotAI;

struct GatherNodeSpawn
{
    ObjectGuid::LowType spawnId{0};
    WorldPosition pos{};
    uint32 skillId{0};  // SKILL_HERBALISM or SKILL_MINING
    uint32 reqSkillValue{0};
    uint32 entry{0};        // the gameobject template
    uint32 respawnSecs{0};  // the spawn's respawn time (spawntimesecs)
    float share{1.0f};      // how often the spawn point holds a node: its pools' share (GatherRules.h PoolShare)
    // The place's danger (professions WR13b): the hostile creatures spawned within RISK_YARDS, their mean level and
    // elite share; none near: the zone's level and no elite
    float mobLevel{0.0f};
    float eliteShare{0.0f};
};

/// Index of gatherable gameobject spawns (herbs/veins) on continent maps,
/// built once from sObjectMgr spawn data at world init. Immutable afterwards.
class GatherNodeMgr
{
public:
    static GatherNodeMgr& instance()
    {
        static GatherNodeMgr instance;
        return instance;
    }

    void Load();

    // Cheap check for CheckRpgStatusAvailable: any node in the bot's current
    // zone harvestable with its current skills?
    bool HasUsableNodes(Player* bot);

    // Live (spawned + GO_STATE_READY) object for a spawn point, nullptr if
    // none. Only meaningful when the grid at the spawn position is loaded.
    static GameObject* FindLiveNode(Map* map, ObjectGuid::LowType spawnId);

    // True when the spawn point is verifiably empty right now: grid loaded
    // and no live object. An unloaded grid yields false ("unknown"). Never
    // record this verdict anywhere - nodes respawn, so liveness must be
    // re-evaluated at every decision.
    static bool IsVerifiablyDown(Map* map, WorldPosition const& pos, ObjectGuid::LowType spawnId);

    // Nearest unvisited usable node within `radius` that is verifiably up
    // right now, or nullptr. Deliberately not zone-filtered: a live node
    // just across a zone border still counts as "passing by".
    GatherNodeSpawn const* GetNearestLiveNode(Player* bot, std::unordered_set<ObjectGuid::LowType> const& visited,
                                              float radius);

    // Every spawn by map and zone, immutable after Load (the recipe index's node rows read it, professions WR9b)
    [[nodiscard]] std::unordered_map<uint32, std::unordered_map<uint32, std::vector<GatherNodeSpawn>>> const& Spawns()
        const
    {
        return _nodes;
    }

    // The spawn of a spawn id, null when it is no gather node; immutable after Load
    [[nodiscard]] GatherNodeSpawn const* Find(ObjectGuid::LowType spawnId) const
    {
        auto const at = _bySpawn.find(spawnId);
        return at != _bySpawn.end() ? at->second : nullptr;
    }

private:
    bool IsUsable(PlayerbotAI* botAI, Player* bot, GatherNodeSpawn const& node);

    std::vector<GatherNodeSpawn> const* GetZoneNodes(uint32 mapId, uint32 zoneId) const;

    // Bucketed per zone so the zone-scoped queries (HasUsableNodes, the
    // planner's circuit) touch only the bot's zone instead of the whole continent.
    std::unordered_map<uint32 /*mapId*/, std::unordered_map<uint32 /*zoneId*/, std::vector<GatherNodeSpawn>>> _nodes;
    std::unordered_map<ObjectGuid::LowType, GatherNodeSpawn const*> _bySpawn;  // into _nodes, built after it
};

#define sGatherNodeMgr GatherNodeMgr::instance()

#endif
