/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <list>
#include <memory>
#include <string_view>
#include <vector>

#include "GameObject.h"
#include "Log.h"
#include "LootObjectStack.h"
#include "NewRpgAction.h"
#include "NewRpgInfo.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestExecRules.h"
#include "QuestIndex.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

// WT35e: the objects of a quest's objectives. A container that holds an item the bot needs goes to the loot path,
// which opens and loots it; an object the objective names is used once through CMSG_GAMEOBJ_USE and the core's
// handler. The log says whether it counted. Ported/adapted from mod-playerbots PR #2632 (head d4e5d110): its
// EngageGameObject and HoldForLoot, without its visited-object map and timers (the passed ring and the hold do that)
namespace
{
constexpr float QUEST_OBJECT_REACH_YARDS = INTERACTION_DISTANCE - 2.0f;  // within this: the use or the loot
constexpr uint32 QUEST_OBJECT_USE_HOLD_MS = 2 * IN_MILLISECONDS;         // after a use: the handler's credit
constexpr uint32 QUEST_OBJECT_LOOT_HOLD_MS = 15 * IN_MILLISECONDS;       // after a hand-over: the loot path's turn

// A quest_exec decision row (world-log.md): ref is the quest
void Trace(Player* bot, std::string_view choice, std::string_view reason, float value, uint32 questId,
           WorldLogFormat::Alternatives const& alternatives = {})
{
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::QuestExec, choice, reason, value, alternatives, questId);
}

Planner::QuestExec::ObjectSeen See(Player* bot, NewRpgInfo::DoQuest const& data, GameObject* go)
{
    Planner::QuestExec::ObjectSeen seen;
    if (!go || !go->IsInWorld())
        return seen;
    seen.present = go->isSpawned();
    // In use (a goober's or a door's active state), not selectable, or a conditional object the bot's quests do not
    // activate (the server's call for a client, as LootObject::IsLootPossible makes it)
    seen.ready = go->GetGoState() == GO_STATE_READY && !go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE) &&
                 (!go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND) || go->ActivateToQuest(bot));
    seen.passed = Planner::QuestExec::Remembered(data.passed, go->GetGUID());
    seen.chest = go->GetGoType() == GAMEOBJECT_TYPE_CHEST;
    seen.inReach = bot->GetDistance(go) <= QUEST_OBJECT_REACH_YARDS;
    if (seen.chest)
    {
        // LootObject reads the container's quest items and its lock; its IsLootPossible the bot's key or skill
        LootObject loot(bot, go->GetGUID());
        seen.needed = !loot.IsEmpty();
        seen.opens = seen.needed && seen.inReach && loot.IsLootPossible(bot);
    }
    return seen;
}
}  // namespace

bool NewRpgDoQuestAction::SeekObject(NewRpgInfo::DoQuest& data, float maxYards)
{
    Quest const* quest = data.quest;
    int32 const slot = data.objectiveIdx;
    std::vector<uint32> entries;
    // An object objective names its gameobject (negative in the quest's slot); a chest objective's containers are its
    // item's object sources (QuestIndex.h SeekObjects)
    if (quest && slot >= 0 && slot < QUEST_OBJECTIVES_COUNT && quest->RequiredNpcOrGo[slot] < 0)
        entries.push_back(static_cast<uint32>(-quest->RequiredNpcOrGo[slot]));
    else if (data.work == static_cast<uint8>(Planner::QuestExec::Work::Chest))
    {
        std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
        Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
        uint32 const objective = Planner::QuestExec::ObjectiveOf(data.place);
        if (record && objective < record->objectives.size())
            entries = record->objectives[objective].seekObjects;
    }
    if (entries.empty())
        return false;

    float const range = std::min(maxYards, sPlayerbotAIConfig.SightDistance);
    std::list<GameObject*> objects;
    bot->GetGameObjectListWithEntryInGrid(objects, entries, range);
    GameObject* nearest = nullptr;
    float nearestYards = range;
    for (GameObject* go : objects)
    {
        Planner::QuestExec::ObjectAct const act = Planner::QuestExec::ObjectVerdict(See(bot, data, go));
        // An object in use is waited for: the look round of the place is held while one is in sight
        if (act == Planner::QuestExec::ObjectAct::Wait)
            data.lastEngage = getMSTime();
        if (act == Planner::QuestExec::ObjectAct::Skip || act == Planner::QuestExec::ObjectAct::Wait)
            continue;
        float const yards = bot->GetDistance(go);
        if (yards < nearestYards)
        {
            nearest = go;
            nearestYards = yards;
        }
    }
    if (!nearest)
        return false;

    data.targetGuid = nearest->GetGUID();
    data.targetPos =
        WorldPosition(bot->GetMapId(), nearest->GetPositionX(), nearest->GetPositionY(), nearest->GetPositionZ());
    data.engagedMs = 0;
    data.ticking = false;
    data.actedAt = 0;
    LOG_DEBUG("playerbots", "[New RPG] {} holds quest object {} ({} yd) for quest {} objective {}", bot->GetName(),
              data.targetGuid.ToString(), nearestYards, data.questId, data.objectiveIdx);
    Trace(bot, "hold", "object", nearestYards, data.questId,
          {{"entry", static_cast<float>(nearest->GetEntry())}, {"candidates", static_cast<float>(objects.size())}});
    return true;
}

bool NewRpgDoQuestAction::EngageObject(NewRpgInfo::DoQuest& data)
{
    using Planner::QuestExec::ObjectAct;
    // A fight holds the engagement; the combat actions fight. The pursuit clock bounds the walk to the object
    if (bot->IsInCombat())
        return false;
    uint32 const sinceLast = data.lastEngage ? GetMSTimeDiffToNow(data.lastEngage) : 0;
    Planner::QuestExec::Pursue(data.engagedMs, data.ticking, false, sinceLast);
    data.lastEngage = getMSTime();

    GameObject* go = botAI->GetGameObject(data.targetGuid);
    if (data.actedAt)
    {
        // Acted once: a container stays held while the loot path loots it (it still holds a needed item), bounded;
        // a used object for the handler's credit. Then it is written off and the log says whether it counted
        bool const chest = go && go->GetGoType() == GAMEOBJECT_TYPE_CHEST;
        uint32 const since = GetMSTimeDiffToNow(data.actedAt);
        if (chest ? since < QUEST_OBJECT_LOOT_HOLD_MS && !LootObject(bot, data.targetGuid).IsEmpty()
                  : since < QUEST_OBJECT_USE_HOLD_MS)
            return true;
        Trace(bot, "object_done", chest ? "loot" : "use", since / 1000.0f, data.questId,
              {{"entry", static_cast<float>(data.targetGuid.GetEntry())}});
        Planner::QuestExec::Remember(data.passed, data.passedNext, data.targetGuid);
        ClearTarget(data);
        return true;
    }

    Planner::QuestExec::ObjectSeen const seen = See(bot, data, go);
    ObjectAct const act = Planner::QuestExec::ObjectVerdict(seen);
    if (act == ObjectAct::Skip || act == ObjectAct::Wait)
    {
        // Gone or in use: not remembered, so the seek takes it again once it is back or ready
        Trace(bot, "pass_over", act == ObjectAct::Wait ? "in_use" : "gone", data.engagedMs / 1000.0f, data.questId,
              {{"entry", static_cast<float>(data.targetGuid.GetEntry())}});
        ClearTarget(data);
        return true;
    }
    if (act == ObjectAct::Locked)
    {
        data.lockedSeen = true;
        PassOver(data, "locked");
        return true;
    }
    if (Planner::QuestExec::PursuitOver(data.engagedMs))
    {
        PassOver(data, "clock");
        return true;
    }
    float const yards = bot->GetDistance(go);
    if (act == ObjectAct::Walk)
        return WalkOn(data, data.targetPos, yards, true, go->GetEntry(), QUEST_OBJECT_REACH_YARDS);

    ResetWalk(data);
    if (bot->isMoving())
        bot->StopMoving();
    if (bot->IsMounted())
    {
        bot->Dismount();
        return true;
    }
    if (bot->GetShapeshiftForm() != FORM_NONE)
    {
        bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        return true;
    }

    data.actedAt = getMSTime();
    if (act == ObjectAct::Loot)
    {
        // The core grants a container's items through its loot: the loot path opens it (its lock's spell) and loots
        context->GetValue<LootObject>("loot target")->Set(LootObject(bot, data.targetGuid));
        AI_VALUE(LootObjectStack*, "available loot")->Add(data.targetGuid);
    }
    else
    {
        // One use through the handler a client's click reaches: a goober's credit, its script or its linked trap
        WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
        packet << data.targetGuid;
        bot->GetSession()->HandleGameObjectUseOpcode(packet);
    }
    LOG_DEBUG("playerbots", "[New RPG] {} {} quest object {} for quest {}", bot->GetName(),
              act == ObjectAct::Loot ? "loots" : "uses", go->GetEntry(), data.questId);
    Trace(bot, "object_used", act == ObjectAct::Loot ? "loot" : "use", yards, data.questId,
          {{"entry", static_cast<float>(go->GetEntry())}});
    return true;
}
