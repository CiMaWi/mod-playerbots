/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgDoGather.h"

#include "GameObject.h"
#include "GatherNodeMgr.h"
#include "LootObjectStack.h"
#include "NewRpgInfo.h"
#include "NewRpgStatusEnd.h"
#include "ObjectDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
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

    // Periodically resolve a target that is verifiably empty (grid loaded, no
    // live object) instead of walking all the way to it (taken when the bot
    // was harvesting it): the respawn memory keeps it out until it is back.
    if (runPeriodicChecks && data.nodeSpawnId &&
        GatherNodeMgr::IsVerifiablyDown(bot->GetMap(), data.nodePos, data.nodeSpawnId))
    {
        ResolveStop(data, data.sawLive, false);
        return true;
    }

    if (!data.nodeSpawnId)
    {
        TargetNextStop(data);
        return true;
    }

    // 10yd keeps the bot inside lootDistance (default 15) so the loot
    // pipeline's "loot available" trigger can fire on the node.
    if (bot->GetExactDist(data.nodePos) > 10.0f)
        return MoveFarTo(data.nodePos);

    if (!data.lastReach)
        data.lastReach = getMSTime();

    GameObject* go = GatherNodeMgr::FindLiveNode(bot->GetMap(), data.nodeSpawnId);

    if (!go)
    {
        // Harvested (taken when the bot had it in front of it) or gone. NOT
        // written off: the respawn memory brings it back on a later lap.
        ResolveStop(data, data.sawLive, false);
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
        ResolveStop(data, false, true);
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
    // spell and loots. While it works, Add() returns false and this idles.
    data.sawLive = true;
    return AI_VALUE(LootObjectStack*, "available loot")->Add(go->GetGUID());
}

bool NewRpgDoGatherAction::TargetNextStop(NewRpgInfo::DoGather& data)
{
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
        if (data.takenMs.size() != data.circuit.size())
            data.takenMs.resize(data.circuit.size(), 0);
        // The stops in walk order, lap after lap; a stop taken or found gone
        // waits out its respawn, a written-off one is skipped
        while (!target && data.lap < data.laps)
        {
            for (; data.next < data.circuit.size(); ++data.next)
            {
                GatherNodeSpawn const* spawn = sGatherNodeMgr.Find(data.circuit[data.next]);
                if (!spawn || data.visited.contains(spawn->spawnId))
                    continue;
                uint32 const taken = data.takenMs[data.next];
                if (taken && GetMSTimeDiffToNow(taken) < spawn->respawnSecs * IN_MILLISECONDS)
                    continue;
                target = spawn;
                break;
            }
            if (target)
                break;
            data.next = 0;
            ++data.lap;
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
    data.sawLive = false;
    return true;
}

void NewRpgDoGatherAction::ResolveStop(NewRpgInfo::DoGather& data, bool taken, bool writeOff)
{
    using namespace Professions;
    GatherNodeSpawn const* spawn = sGatherNodeMgr.Find(data.nodeSpawnId);
    bool const planned = data.next < data.circuit.size() && data.circuit[data.next] == data.nodeSpawnId &&
                         data.takenMs.size() > data.next;
    // The stop's row (WR13b): taken or not, its number in walk order (0 off a circuit), the lap and the node's entry
    WorldLog::Decision(
        bot, WorldLogFormat::DecisionKind::Profession, ChoiceWord(Choice::Gather),
        ReasonWord(taken ? Reason::Supply : Reason::NoSource), planned ? static_cast<float>(data.next + 1) : 0.0f,
        {{"lap", static_cast<float>(data.lap + 1)}, {"entry", spawn ? static_cast<float>(spawn->entry) : 0.0f}},
        data.nodeSpawnId);
    if (planned)
    {
        data.takenMs[data.next] = std::max<uint32>(getMSTime(), 1);
        ++data.next;
    }
    // Off a circuit every node is taken once; on it only a written-off one stays out
    AbandonNode(data, writeOff || data.circuit.empty());
    data.sawLive = false;
}
