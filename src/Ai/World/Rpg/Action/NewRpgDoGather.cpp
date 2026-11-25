/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgDoGather.h"

#include <mutex>
#include <unordered_map>

#include "GameObject.h"
#include "GatherNodeMgr.h"
#include "GatherRules.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "NewRpgInfo.h"
#include "NewRpgStatusEnd.h"
#include "ObjectDefines.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PoolMgr.h"
#include "ProfessionConfig.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogStatus.h"

// Base-rank gather spells, the same ids OpenLootAction::DoLoot casts.
enum GatherSpells
{
    HERB_GATHERING = 2366,
    MINING = 2575,
};

// The Status words of a gather status's own ends (professions WR13b)
constexpr char const* CIRCUIT_DONE = "circuit_done";
constexpr char const* MAP_CHANGED = "map_changed";
// Why a stop resolved (its row's third alternative, the bot's distance to the node in yd): its own harvest cast went
// at the node and the node went; down, seen empty within the bot's sight (SeenEmpty) or when the bot stood at it, by
// the words of WR13d (DownStop, Gather::DownCause); written off: no loot access, the harvest did not happen in time
constexpr char const* STOP_HARVESTED = "harvested";
constexpr char const* STOP_NO_LOOT = "no_loot";
constexpr char const* STOP_TIMED_OUT = GatherStopWords::TIMED_OUT;
// The bot walked toward the stop Gather::WALK_STALL_MS without moving (a fight or another pause does not count): the
// stop resolves and the status ends stuck, so the planner picks again (a trip it holds can't be dropped otherwise)
constexpr char const* STOP_UNREACHED = GatherStopWords::UNREACHED;

namespace
{
// The last gather cast at each node spawn by any bot (WR13d, the diagnosis): the bot's guid counter and when. Bots run
// on map threads, so the record is locked; one write per harvest cast, one read per stop found down. Players' takes
// are not in it
struct HarvestNote
{
    ObjectGuid::LowType bot{0};
    uint32 ms{0};
};
std::mutex harvestNotesMutex;
std::unordered_map<ObjectGuid::LowType, HarvestNote> harvestNotes;

// Why the spawn holds no live node, from what the bot can read now (the map's store and pool state, the record)
std::string_view DownStop(Player* bot, ObjectGuid::LowType spawnId)
{
    Gather::DownFacts facts;
    GatherNodeSpawn const* spawn = sGatherNodeMgr.Find(spawnId);
    uint32 const respawnMs = spawn ? spawn->respawnSecs * IN_MILLISECONDS : 0;
    {
        std::lock_guard<std::mutex> lock(harvestNotesMutex);
        auto const note = harvestNotes.find(spawnId);
        if (note != harvestNotes.end() && GetMSTimeDiffToNow(note->second.ms) < respawnMs)
        {
            facts.ownTake = note->second.bot == bot->GetGUID().GetCounter();
            facts.otherTake = !facts.ownTake;
        }
    }
    Map* map = bot->GetMap();
    auto const bounds = map->GetGameObjectBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
    {
        facts.held = true;
        if (it->second->isSpawned() && it->second->GetGoState() != GO_STATE_READY)
            facts.notReady = true;
    }
    if (uint32 const pool = sPoolMgr->IsPartOfAPool<GameObject>(spawnId))
    {
        SpawnedPoolData const& spawned = map->GetPoolData();
        facts.pooled = true;
        facts.held = spawned.IsSpawnedObject<GameObject>(spawnId);
        uint32 const up = spawned.GetSpawnedObjects(pool);
        uint32 const mother = sPoolMgr->IsPartOfAPool<Pool>(pool);
        facts.poolUp = up > (facts.held ? 1u : 0u) ||
                       (mother && spawned.GetSpawnedObjects(mother) > (spawned.IsSpawnedObject<Pool>(pool) ? 1u : 0u));
    }
    return Gather::DownCause(facts);
}

// The bot sees the spawn point empty (WR13d): it lies within the bot's sight, the map's visibility range (as a player
// sees a node on the minimap), its grid is loaded and it holds no live node. Beyond its sight the bot knows nothing
// and walks closer
bool SeenEmpty(Player* bot, WorldPosition const& pos, ObjectGuid::LowType spawnId)
{
    Map* map = bot->GetMap();
    return bot->GetExactDist(pos) <= map->GetVisibilityRange() && GatherNodeMgr::IsVerifiablyDown(map, pos, spawnId);
}
}  // namespace

void NewRpgDoGatherAction::ClearLootTargetForNode(ObjectGuid::LowType spawnId)
{
    LootObject lootTarget = AI_VALUE(LootObject, "loot target");
    if (!lootTarget.guid.IsGameObject())
        return;

    // GO instance guids are map-generated and do NOT equal spawn ids -
    // resolve the object and compare its spawn id. An unresolvable target
    // (despawned) is dead weight either way: release it too.
    GameObject* go = botAI->GetGameObject(lootTarget.guid);
    if (go && go->GetSpawnId() != spawnId)
        return;

    context->GetValue<LootObject>("loot target")->Set(LootObject());
}

void NewRpgDoGatherAction::AbandonNode(NewRpgInfo::DoGather& data, bool markVisited)
{
    ClearLootTargetForNode(data.nodeSpawnId);
    if (markVisited)
        data.visited.insert(data.nodeSpawnId);
    data.nodeSpawnId = 0;
    data.lastReach = 0;
}

bool NewRpgDoGatherAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoGather>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;

    // A dead bot can't interact with nodes: standing on them just burns the
    // timeout and wrongly writes live nodes into `visited`. Leave the state
    // untouched and resume after revive.
    if (!bot->IsAlive())
        return false;

    // (Nearly) full bags: the loot pipeline refuses to store new items
    // above this fill level (see StoreLootAction), so every harvest would
    // open the node, store nothing and stall until the timeout. Stop
    // farming and let the maintenance logic (sell/destroy) catch up.
    if (AI_VALUE(uint8, "bag space") > 80)
    {
        RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = "bags_full"});
        return true;
    }

    // Teleported or summoned away: the circuit's places are on the old map,
    // the status ends. Checked before anything that uses data.nodePos.
    if (data.nodeSpawnId && bot->GetMapId() != data.nodePos.GetMapId())
    {
        ClearLootTargetForNode(data.nodeSpawnId);
        RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = MAP_CHANGED});
        return true;
    }

    bool runPeriodicChecks = !data.lastPassiveCheck || GetMSTimeDiffToNow(data.lastPassiveCheck) > passiveCheckInterval;
    if (runPeriodicChecks)
        data.lastPassiveCheck = getMSTime();

    // A creature stop has its own steps (professions WR20c)
    if (Gather::IsCreatureStop(data.nodeSpawnId))
        return FarmStop(data, runPeriodicChecks);

    // Periodically resolve a target the bot sees empty instead of walking all
    // the way to it (taken when the bot was harvesting it): the respawn
    // memory keeps it out until it is back.
    if (runPeriodicChecks && data.nodeSpawnId && SeenEmpty(bot, data.nodePos, data.nodeSpawnId))
    {
        ResolveStop(data, data.harvestCast, false,
                    data.harvestCast ? std::string_view(STOP_HARVESTED) : DownStop(bot, data.nodeSpawnId));
        return true;
    }

    if (!data.nodeSpawnId)
    {
        TargetNextStop(data);
        return true;
    }

    // 10yd keeps the bot inside lootDistance (default 15) so the loot
    // pipeline's "loot available" trigger can fire on the node.
    if (std::optional<bool> const walking = WalkToStop(data, data.nodePos, 10.0f))
        return *walking;

    if (!data.lastReach)
        data.lastReach = getMSTime();

    GameObject* go = GatherNodeMgr::FindLiveNode(bot->GetMap(), data.nodeSpawnId);

    if (!go)
    {
        // Harvested (taken when the bot's own cast went at it) or gone. NOT
        // written off: the respawn memory brings it back on a later lap.
        ResolveStop(data, data.harvestCast, false,
                    data.harvestCast ? std::string_view(STOP_HARVESTED) : DownStop(bot, data.nodeSpawnId));
        return true;
    }

    // A harvest cast is already underway - do nothing that could clobber
    // it (movement cancels the cast).
    if (bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) || bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return false;

    // Write-off cases that do go into `visited` (this bot can't harvest
    // the node, now or later this session):
    // - IsEmpty: no access to the loot at all (quest-gated chests and
    //   anything else the index filter misses). Deliberately only the
    //   content check - positional/transient failures like the loot
    //   pipeline's Z-distance check on a slope are left to the timeout,
    //   so the bot doesn't bounce off harvestable nodes.
    // - nodeStayTime timeout: the loot pipeline failed to harvest a live
    //   node in time; without the write-off the bot would re-select it
    //   forever.
    LootObject lootObj(bot, go->GetGUID());
    bool timedOut = GetMSTimeDiffToNow(data.lastReach) > nodeStayTime;
    if (lootObj.IsEmpty() || timedOut)
    {
        ResolveStop(data, false, true, lootObj.IsEmpty() ? STOP_NO_LOOT : STOP_TIMED_OUT);
        return true;
    }

    // Keep the loot pipeline pointed at our node, not a stale previous one:
    // otherwise its move-to-loot (higher relevance) pulls the bot toward the
    // old target while our approach pulls toward this one, and they fight.
    // Don't override a valid target the pipeline is already busy with in loot
    // range (e.g. a corpse right next to us).
    LootObject currentTarget = AI_VALUE(LootObject, "loot target");
    bool keepCurrent = !currentTarget.IsEmpty() && currentTarget.IsLootPossible(bot) &&
                       AI_VALUE2(float, "distance", "loot target") <= sPlayerbotAIConfig.LootDistance;
    if (currentTarget.guid != go->GetGUID() && !keepCurrent)
        context->GetValue<LootObject>("loot target")->Set(LootObject(bot, go->GetGUID()));

    // Drive the close approach ourselves when out of interaction range. The
    // loot pipeline's own move-to-loot can't: it is gated on IsLootPossible
    // (whose Z check fails for nodes well above/below the bot - ledges,
    // underwater kelp), and it approaches via LOS-sampled offset points,
    // which find nothing when the node sits behind an obstacle (los=false
    // around a corner). A validated path (exact_waypoint false) climbs,
    // descends and routes around geometry to the node. This only runs when
    // the pipeline's higher-relevance approach already failed to move this
    // tick; a node with no reachable path is retired by the timeout above.
    if (!lootObj.IsLootPossible(bot) || bot->GetDistance(go) > INTERACTION_DISTANCE - 2.0f)
    {
        if (bot->isMoving())
            return false;
        return MoveTo(bot->GetMapId(), data.nodePos.GetPositionX(), data.nodePos.GetPositionY(),
                      data.nodePos.GetPositionZ(), false, false, false, false);
    }

    // The loot pipeline only dismounts, it never leaves shapeshift form. Drop
    // the form only when it actually blocks the gather cast (per the spell's
    // own stance rules: Herb Gathering is explicitly allowed in flight forms,
    // while e.g. Travel Form blocks it). We're in interaction range here (the
    // approach branch above returned otherwise), so the bot keeps its form
    // speed while approaching. No-op for non-druids (FORM_NONE).
    if (bot->GetShapeshiftForm() != FORM_NONE)
    {
        uint32 gatherSpellId = lootObj.skillId == SKILL_MINING ? MINING : HERB_GATHERING;
        SpellInfo const* gatherSpell = sSpellMgr->GetSpellInfo(gatherSpellId);
        if (gatherSpell && gatherSpell->CheckShapeshift(bot->GetShapeshiftForm()) != SPELL_CAST_OK)
        {
            bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            return true;
        }
    }

    // In range with the pipeline pointed at our node: it casts the gather
    // spell (NoteHarvestCast) and loots. While it works, Add() returns false
    // and this idles.
    return AI_VALUE(LootObjectStack*, "available loot")->Add(go->GetGUID());
}

std::optional<bool> NewRpgDoGatherAction::WalkToStop(NewRpgInfo::DoGather& data, WorldPosition const& pos, float reach)
{
    if (bot->GetExactDist(pos) <= reach)
        return std::nullopt;
    bool const first = !data.walkTickMs;
    uint32 const sinceLast = first ? 0 : GetMSTimeDiffToNow(data.walkTickMs);
    data.walkTickMs = std::max<uint32>(getMSTime(), 1);
    if (Gather::WalkStalled(data.walkAnchor, data.walkStalledMs, first, sinceLast,
                            {bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()}))
    {
        if (Gather::IsCreatureStop(data.nodeSpawnId))
            ResolveFarmStop(data, false, GatherStopWords::UNREACHED, false, true);
        else
            ResolveStop(data, false, true, STOP_UNREACHED);
        RpgStatus::End(botAI, StatusEnd::Outcome::Stuck, {.word = STOP_UNREACHED});
        return true;
    }
    return Away(pos, reach).value_or(true);
}

bool NewRpgDoGatherAction::TargetNextStop(NewRpgInfo::DoGather& data)
{
    NewRpgInfo& info = botAI->rpgInfo;
    GatherNodeSpawn const* target = nullptr;
    if (data.circuit.empty())
    {
        // No circuit (a bridge order): the nearest live node, one after the
        // other, until none is near
        target = sGatherNodeMgr.GetNearestLiveNode(bot, data.visited, nodeSwitchDistance);
        if (!target)
        {
            RpgStatus::End(botAI, StatusEnd::Outcome::Finished);
            return false;
        }
    }
    else
    {
        // The stops in walk order, lap after lap: one the bot took or found
        // empty waits out its respawn (its gather memory, which outlives the
        // status), a written-off one is skipped, one it sees empty from here
        // resolves at once (WR13d)
        uint32 const now = getMSTime();
        while (!target && data.lap < data.laps)
        {
            if (data.next >= data.circuit.size())
            {
                data.next = 0;
                ++data.lap;
                continue;
            }
            // A creature stop (WR20c): its spawn point from the world's creature data; it resolves when the bot
            // comes there (FarmStop)
            uint32 const key = data.circuit[data.next];
            if (Gather::IsCreatureStop(key))
            {
                CreatureData const* creature = sObjectMgr->GetCreatureData(key & ~Gather::CREATURE_STOP);
                if (!creature || data.visited.contains(key) || info.gatherMemory.Away(key, now))
                {
                    ++data.next;
                    continue;
                }
                if (creature->mapid != bot->GetMapId())
                {
                    RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = MAP_CHANGED});
                    return false;
                }
                data.nodeSpawnId = key;
                data.nodePos = WorldPosition(creature->mapid, creature->posX, creature->posY, creature->posZ);
                data.creatureEntry = creature->id;
                data.targetGuid.Clear();
                data.killedMs = 0;
                data.skinSeenMs = 0;
                data.lastReach = 0;
                data.harvestCast = false;
                data.walkAnchor = {};
                data.walkStalledMs = 0;
                data.walkTickMs = 0;
                return true;
            }
            GatherNodeSpawn const* spawn = sGatherNodeMgr.Find(key);
            if (!spawn || data.visited.contains(spawn->spawnId) || info.gatherMemory.Away(spawn->spawnId, now))
            {
                ++data.next;
                continue;
            }
            if (spawn->pos.GetMapId() == bot->GetMapId() && SeenEmpty(bot, spawn->pos, spawn->spawnId))
            {
                data.nodeSpawnId = spawn->spawnId;
                data.nodePos = spawn->pos;
                ResolveStop(data, false, false, DownStop(bot, spawn->spawnId));  // the stop is due: next moves on
                continue;
            }
            target = spawn;
        }
        if (!target)
        {
            RpgStatus::End(botAI, StatusEnd::Outcome::Finished, {.word = CIRCUIT_DONE});
            return false;
        }
        if (target->pos.GetMapId() != bot->GetMapId())
        {
            RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = MAP_CHANGED});
            return false;
        }
    }
    data.nodeSpawnId = target->spawnId;
    data.nodePos = target->pos;
    data.lastReach = 0;
    data.harvestCast = false;
    data.walkAnchor = {};
    data.walkStalledMs = 0;
    data.walkTickMs = 0;
    return true;
}

void NewRpgDoGatherAction::NoteHarvestCast(PlayerbotAI* botAI, ObjectGuid node)
{
    GameObject* go = node.IsGameObject() ? botAI->GetGameObject(node) : nullptr;
    if (!go)
        return;
    if (go->GetSpawnId())
    {
        std::lock_guard<std::mutex> lock(harvestNotesMutex);
        harvestNotes[go->GetSpawnId()] = {botAI->GetBot()->GetGUID().GetCounter(), std::max<uint32>(getMSTime(), 1)};
    }
    auto* data = std::get_if<NewRpgInfo::DoGather>(&botAI->rpgInfo.data);
    if (data && data->nodeSpawnId && go->GetSpawnId() == data->nodeSpawnId)
        data->harvestCast = true;
}

void NewRpgDoGatherAction::ResolveStop(NewRpgInfo::DoGather& data, bool taken, bool writeOff, std::string_view cause)
{
    using namespace Professions;
    GatherNodeSpawn const* spawn = sGatherNodeMgr.Find(data.nodeSpawnId);
    bool const planned = data.next < data.circuit.size() && data.circuit[data.next] == data.nodeSpawnId;
    // The stop's row (WR13b): taken or not, its number in walk order (0 off a circuit), the lap, the node's entry and
    // the cause with the bot's distance to the node
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession, ChoiceWord(Choice::Gather),
                       ReasonWord(taken ? Reason::Supply : Reason::NoSource),
                       planned ? static_cast<float>(data.next + 1) : 0.0f,
                       {{"lap", static_cast<float>(data.lap + 1)},
                        {"entry", spawn ? static_cast<float>(spawn->entry) : 0.0f},
                        {cause, bot->GetExactDist(data.nodePos)}},
                       data.nodeSpawnId);
    // The respawn memory: taken, found empty or written off, the bot comes back after the respawn at the earliest
    if (spawn)
        botAI->rpgInfo.gatherMemory.Note(spawn->spawnId, getMSTime(), spawn->respawnSecs * IN_MILLISECONDS);
    if (planned)
        ++data.next;
    // Off a circuit every node is taken once; on it only a written-off one stays out
    AbandonNode(data, writeOff || data.circuit.empty());
    data.harvestCast = false;
}
