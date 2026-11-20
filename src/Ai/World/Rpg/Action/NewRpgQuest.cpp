/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <list>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "BotTravel.h"
#include "BroadcastHelper.h"
#include "CensusMgr.h"
#include "ChatHelper.h"
#include "Creature.h"
#include "G3D/Vector2.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "NewRpgAction.h"
#include "NewRpgBaseAction.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestExecRules.h"
#include "QuestPackets.h"
#include "QuestPlaceRules.h"
#include "Random.h"
#include "StatsWeightCalculator.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    RpgStatus::End(botAI, StatusEnd::Outcome::Finished);
    return true;
}

namespace
{
// WT35a: the executor's seek and engagement (adapted from mod-playerbots PR #2632, head d4e5d110; tuning values)
constexpr uint32 QUEST_SCAN_MS = 2 * IN_MILLISECONDS;         // a seek at most this often
constexpr float QUEST_SCAN_YARDS = 150.0f;                    // and at most this far (and within sight)
constexpr uint32 QUEST_ENGAGE_MS = 45 * IN_MILLISECONDS;      // a held target out of combat this long is passed over
constexpr uint32 QUEST_ENGAGE_GAP_MS = 5 * IN_MILLISECONDS;   // a longer pause between engage ticks restarts it
constexpr float QUEST_ENGAGE_YARDS = 30.0f;                   // within this and in sight: the attack
constexpr float QUEST_TARGET_MOVED_YARDS = 10.0f;             // a held target that moved this far is walked to anew
constexpr float QUEST_ARRIVE_YARDS = 10.0f;                   // at the place
constexpr uint32 QUEST_PLACE_LOOK_MS = 60 * IN_MILLISECONDS;  // at a place this long without a target: it is tried

// Whether the objective in this quest-log slot (0..3 creature or object, QUEST_OBJECTIVES_COUNT + i item i) still
// needs progress
bool SlotOpen(Quest const* quest, QuestStatusData const& progress, int32 slot)
{
    if (slot >= 0 && slot < QUEST_OBJECTIVES_COUNT)
        return quest->RequiredNpcOrGo[slot] != 0 &&
               progress.CreatureOrGOCount[slot] < quest->RequiredNpcOrGoCount[slot];
    int32 const item = slot - QUEST_OBJECTIVES_COUNT;
    return item >= 0 && item < QUEST_ITEM_OBJECTIVES_COUNT && quest->RequiredItemId[item] != 0 &&
           progress.ItemCount[item] < quest->RequiredItemCount[item];
}

// The step function over the bot's log and the index's places (QuestExecRules.h); false while the index is still
// building. For a step that works an objective, `place` is where
bool NextQuestStep(PlayerbotAI* botAI, Player* bot, NewRpgInfo::DoQuest const& data, Planner::QuestExec::Step& step,
                   QuestDestination& place)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index)
        return false;
    uint32 const questId = data.questId;
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    Planner::QuestRecord const* record = quest ? index->Find(questId) : nullptr;
    if (!record)
    {
        step = {Planner::QuestExec::Act::NoPlace};
        return true;
    }
    QuestStatus const status = bot->GetQuestStatus(questId);
    auto const it = bot->getQuestStatusMap().find(questId);
    QuestStatusData const* progress = it != bot->getQuestStatusMap().end() ? &it->second : nullptr;
    Planner::QuestExec::LogState const log{
        progress && (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE),
        status == QUEST_STATUS_COMPLETE};
    auto const open = [quest, progress](int32 slot) { return progress && SlotOpen(quest, *progress, slot); };
    Player* const player = bot;
    auto const hasSkill = [player](uint16 skill) { return player->HasSkill(skill); };
    Planner::QuestPlaces::Skip skip;
    if (data.left != WorldPosition())
        skip = {true, data.left.GetPositionX(), data.left.GetPositionY()};
    // A planner bot goes to the place nearest its plan's tasks for the quest, others to the nearest
    auto const prefer = [botAI, questId](std::vector<Planner::QuestExec::Candidate> const& candidates)
    {
        std::vector<std::pair<float, float>> points;
        points.reserve(candidates.size());
        for (Planner::QuestExec::Candidate const& candidate : candidates)
            points.emplace_back(candidate.place->x, candidate.place->y);
        return Planner::PlannedPoi(botAI, questId, points);
    };
    uint32 const mapId = bot->GetMapId();
    step = Planner::QuestExec::Next(*record, log, open, mapId, bot->GetPositionX(), bot->GetPositionY(),
                                    static_cast<uint8>(bot->GetTeamId()), hasSkill, data.tried, skip, prefer);
    if (step.act == Planner::QuestExec::Act::Work)
        place = {WorldPosition(mapId, step.place->x, step.place->y, step.place->z),
                 record->objectives[step.objective].poiIndex, Planner::QuestPlaces::Roam(*step.place)};
    step.place = nullptr;  // it points into this index snapshot
    return true;
}
}  // namespace

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    using Planner::QuestExec::Act;
    using Planner::QuestExec::Work;
    uint32 const questId = data.questId;
    if (data.pos != WorldPosition())
    {
        // The step stands while its objective is open: the log is the state (a tried place was left when tried)
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        auto const it = bot->getQuestStatusMap().find(questId);
        if (!quest || it == bot->getQuestStatusMap().end() || !SlotOpen(quest, it->second, data.objectiveIdx))
            LeavePlace(data);
    }
    if (data.pos == WorldPosition())
    {
        Planner::QuestExec::Step step;
        QuestDestination place{WorldPosition(), 0, 0.0f};
        if (!NextQuestStep(botAI, bot, data, step, place))
            return false;
        switch (step.act)
        {
            case Act::Work:
                break;
            case Act::Refused:
                // Before any walk: the executor has no interaction for an open objective (PLAN 3.2)
                RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned,
                               {.word = Planner::QuestExec::CapabilityOf(step.work).refusal});
                return true;
            case Act::NoPlace:
                // No open objective has a place on this map (PLAN 3.1 rule 6): stop doing the quest for now
                RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = "no_place"});
                return true;
            case Act::NoTarget:
                RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = "no_target"});
                return true;
            default:
                return false;  // the turn-in or done: Execute follows the log's status
        }
        data.lastReachPOI = 0;
        data.pos = place.pos;
        data.objectiveIdx = place.objectiveIdx;
        data.roam = place.roam;
        data.left = WorldPosition();
        data.place = Planner::QuestExec::KeyOf(step);
        data.work = static_cast<uint8>(step.work);
    }

    // Kill: the bot seeks the objective's creature within sight, on the way and at the place, and holds the nearest
    bool const kill = data.work == static_cast<uint8>(Work::Kill);
    if (kill)
    {
        if (!data.targetGuid.IsEmpty())
            return EngageTarget(data);
        if (!data.lastScan || GetMSTimeDiffToNow(data.lastScan) >= QUEST_SCAN_MS)
        {
            data.lastScan = getMSTime();
            if (SeekTarget(data, QUEST_SCAN_YARDS))
                return true;
        }
    }

    if (bot->GetDistance(data.pos) > QUEST_ARRIVE_YARDS && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate: nudge the bot a short distance so the next tick retries from
        // a different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (!hasProgression)
        {
            // we has reach the poi for more than 5 mins but no progession
            // may not be able to complete this quest, marked as abandoned
            /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
            botAI->lowPriorityQuest.insert(questId);
            botAI->RecordQuestAbandoned(questId, QuestAbandonReason::NoProgress);
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = "abandon"});
            return true;
        }
        // Progress, but five minutes here: pick again and leave this place for the objective's next area, where the
        // targets are not thinned out (WT33b; the nearest-place rule would return here every time)
        WorldPosition const left = data.pos;
        LeavePlace(data);
        data.left = left;
        return true;
    }

    // A kill place without a target for a look round: tried, so the step function takes the objective's next place
    if (kill && GetMSTimeDiffToNow(data.lastReachPOI) >= QUEST_PLACE_LOOK_MS &&
        (!data.lastEngage || GetMSTimeDiffToNow(data.lastEngage) >= QUEST_PLACE_LOOK_MS))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} quest {}: no target at place {}, next place", bot->GetName(), questId,
                  data.place);
        data.tried.push_back(data.place);
        LeavePlace(data);
        return true;
    }

    // At the place: roam inside its area (the spawns' spread, 8 to 40 yd), so the bot meets the area's targets and
    // does not drift off to the next camp (an item objective: grind and loot do the work until its seek, WT35a part 2)
    return RoamInPlace(data.pos, data.roam);
}

bool NewRpgDoQuestAction::ValidTarget(NewRpgInfo::DoQuest const& data, Creature* creature) const
{
    if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->GetGUID() == data.passed)
        return false;
    // Tapped by another: its kill would bring no credit
    if (creature->hasLootRecipient() && !creature->isTappedBy(bot))
        return false;
    return !bot->IsFriendlyTo(creature) && bot->IsValidAttackTarget(creature);
}

bool NewRpgDoQuestAction::SeekTarget(NewRpgInfo::DoQuest& data, float maxYards)
{
    Quest const* quest = data.quest;
    int32 const slot = data.objectiveIdx;
    if (!quest || slot < 0 || slot >= QUEST_OBJECTIVES_COUNT || quest->RequiredNpcOrGo[slot] <= 0)
        return false;

    float const range = std::min({maxYards, QUEST_SCAN_YARDS, sPlayerbotAIConfig.SightDistance});
    std::list<Creature*> creatures;
    bot->GetCreatureListWithEntryInGrid(creatures, static_cast<uint32>(quest->RequiredNpcOrGo[slot]), range);
    Creature* nearest = nullptr;
    float nearestYards = range;
    for (Creature* creature : creatures)
    {
        if (!ValidTarget(data, creature))
            continue;
        float const yards = bot->GetDistance(creature);
        if (yards < nearestYards)
        {
            nearest = creature;
            nearestYards = yards;
        }
    }
    if (!nearest)
        return false;

    data.targetGuid = nearest->GetGUID();
    data.targetPos =
        WorldPosition(bot->GetMapId(), nearest->GetPositionX(), nearest->GetPositionY(), nearest->GetPositionZ());
    data.targetSince = 0;
    LOG_DEBUG("playerbots", "[New RPG] {} holds quest target {} ({} yd) for quest {} objective {}", bot->GetName(),
              data.targetGuid.ToString(), nearestYards, data.questId, data.objectiveIdx);
    return true;
}

bool NewRpgDoQuestAction::EngageTarget(NewRpgInfo::DoQuest& data)
{
    // A fight pauses the engagement and its clock; the combat actions do the fight
    if (bot->IsInCombat())
    {
        data.targetSince = getMSTime();
        return false;
    }
    if (data.lastEngage && GetMSTimeDiffToNow(data.lastEngage) > QUEST_ENGAGE_GAP_MS)
        data.targetSince = getMSTime();
    data.lastEngage = getMSTime();

    Unit* unit = botAI->GetUnit(data.targetGuid);
    Creature* creature = unit ? unit->ToCreature() : nullptr;
    if (!creature || !creature->IsAlive())
    {
        ClearTarget(data);  // killed (its corpse goes to the loot) or gone: the log says whether it counted
        return true;
    }
    if (!ValidTarget(data, creature))
    {
        PassOver(data);
        return true;
    }
    if (!data.targetSince)
        data.targetSince = getMSTime();
    else if (GetMSTimeDiffToNow(data.targetSince) >= QUEST_ENGAGE_MS)
    {
        LOG_DEBUG("playerbots", "[New RPG] {} passes over quest target {} for quest {}", bot->GetName(),
                  data.targetGuid.ToString(), data.questId);
        PassOver(data);
        return true;
    }

    if (creature->GetExactDist(data.targetPos) > QUEST_TARGET_MOVED_YARDS)
        data.targetPos = WorldPosition(bot->GetMapId(), creature->GetPositionX(), creature->GetPositionY(),
                                       creature->GetPositionZ());
    float const yards = bot->GetDistance(creature);

    // A switch needs a target twice as close, so equally near creatures do not take turns
    if (yards > QUEST_ENGAGE_YARDS && (!data.lastScan || GetMSTimeDiffToNow(data.lastScan) >= QUEST_SCAN_MS))
    {
        data.lastScan = getMSTime();
        ObjectGuid const held = data.targetGuid;
        if (SeekTarget(data, yards * 0.5f) && data.targetGuid != held)
            return true;
    }

    if (yards > QUEST_ENGAGE_YARDS || !bot->IsWithinLOSInMap(creature))
    {
        if (MoveFarTo(data.targetPos))
            return true;
        return MoveRandomNear(10.0f);
    }
    return botAI->DoSpecificAction("new rpg attack quest target", Event(), true);
}

void NewRpgDoQuestAction::ClearTarget(NewRpgInfo::DoQuest& data)
{
    if (!data.targetGuid.IsEmpty())
        if (Unit* unit = botAI->GetUnit(data.targetGuid))
            if (!unit->IsAlive())
                AI_VALUE(LootObjectStack*, "available loot")->Add(data.targetGuid);
    data.targetGuid = ObjectGuid::Empty;
    data.targetPos = WorldPosition();
    data.targetSince = 0;
}

void NewRpgDoQuestAction::PassOver(NewRpgInfo::DoQuest& data)
{
    data.passed = data.targetGuid;
    ClearTarget(data);
}

void NewRpgDoQuestAction::LeavePlace(NewRpgInfo::DoQuest& data)
{
    ClearTarget(data);
    data.lastReachPOI = 0;
    data.pos = WorldPosition();
    data.objectiveIdx = 0;
}

// Ported/adapted from mod-playerbots PR #2632 (head d4e5d110): the targeted attack
Unit* NewRpgAttackQuestTargetAction::GetTarget()
{
    auto* data = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (!data || !data->targetGuid.IsAnyTypeCreature())
        return nullptr;

    return botAI->GetUnit(data->targetGuid);
}

bool NewRpgAttackQuestTargetAction::Execute(Event /*event*/)
{
    verbose = false;

    Unit* target = GetTarget();
    if (!target || !target->IsInWorld() || !target->IsAlive())
        return false;

    if (bot->IsFriendlyTo(target) || !bot->IsValidAttackTarget(target))
        return false;

    bot->SetSelection(target->GetGUID());
    context->GetValue<Unit*>("current target")->Set(target);

    // Halt even when the attack is deferred: the in-flight spline ends on the mob itself.
    LastMovement& lastMovement = AI_VALUE(LastMovement&, "last movement");
    bool moveControlled = bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE;
    if (bot->isMoving() && lastMovement.priority < MovementPriority::MOVEMENT_COMBAT && !moveControlled)
    {
        lastMovement.clear();
        bot->GetMotionMaster()->Clear(false);
        bot->StopMoving();
    }

    if (bot->IsMounted())
    {
        bot->Dismount();
        return true;
    }

    bool result = Attack(target);
    LOG_DEBUG("playerbots", "[New RPG] {} attack quest target {} ({}): {}", bot->GetName(), target->GetName(),
              target->GetGUID().ToString(), result ? "engaged" : "refused");
    return result;
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    Quest const* quest = data.quest;

    if (data.objectiveIdx != -1)
    {
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->RecordQuestCompleted(questId);
        EventLog::Emit(bot, WorldLogFormat::EVENT_QUEST_READY, "executor", questId, 0);
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
    {
        // WT33b: the turn-in is at the nearest ender spawn on this map (its own height, also on a building's floor)
        std::vector<QuestDestination> places;
        PlaceAnswer const answer = QuestPlaces(questId, places);
        if (answer == PlaceAnswer::NotReady)
            return false;
        if (answer == PlaceAnswer::None)
        {
            RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = "no_place"});
            return false;
        }
        data.pos = places.front().pos;
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward
    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // e.g. Can not reward quest to gameobjects
        /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
        botAI->lowPriorityQuest.insert(questId);
        botAI->RecordQuestAbandoned(questId, QuestAbandonReason::CannotTurnIn);
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = "abandon"});
        return true;
    }
    return false;
}

/// @TODO: Fix redundant code
/// Quest related method refer to TalkToQuestGiverAction.h
bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(ObjectGuid guid)
{
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    if (!object || !bot->CanInteractWithQuestGiver(object))
        return false;

    // Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    // if (creature)
    // {
    //     WorldPacket packet(CMSG_GOSSIP_HELLO);
    //     packet << guid;
    //     bot->GetSession()->HandleGossipHelloOpcode(packet);
    // }

    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return true;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && TestConstraints::QuestChoicesAllowed(botAI->testConstraints) &&
            bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            NewRpgHelpers::IsQuestWorthDoing(bot, quest) && NewRpgHelpers::IsQuestCapableDoing(bot, quest))
        {
            AcceptQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->RecordQuestAccepted(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            TurnInQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_rewarded",
                    "Quest rewarded %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
            botAI->RecordQuestRewarded(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {}", bot->GetName(), quest->GetQuestId());
        }
    }
    return true;
}

bool NewRpgBaseAction::IsWithinInteractionDist(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that only keep the distance check
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            // unit checks
            if (!guid)
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->IsWithinDistInMap(bot))
                {
                    return true;
                }
            }
            return false;
        }
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::AcceptQuest(Quest const* quest, ObjectGuid guid)
{
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
    uint32 unk1 = 0;
    p << guid << quest->GetQuestId() << unk1;
    p.rpos(0);
    bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

    return true;
}

bool NewRpgBaseAction::TurnInQuest(Quest const* quest, ObjectGuid guid)
{
    uint32 questID = quest->GetQuestId();

    if (bot->GetQuestRewardStatus(questID))
    {
        return false;
    }

    if (!bot->CanRewardQuest(quest, false))
    {
        return false;
    }

    // WQ1's seam row: the giver search can turn in the executor's quest before the executor saw it complete (WT33b's
    // places bring a bot near the ender), so its Ready row is written here then
    if (auto const* doing = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
        doing && doing->questId == questID && doing->objectiveIdx != -1)
        EventLog::Emit(bot, WorldLogFormat::EVENT_QUEST_READY, "executor", questID, 0);

    bot->PlayDistanceSound(621);

    WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
    p << guid << quest->GetQuestId();
    if (quest->GetRewChoiceItemsCount() <= 1)
    {
        p << 0;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }
    else
    {
        uint32 bestId = BestRewardIndex(quest);
        p << bestId;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }

    return true;
}

uint32 NewRpgBaseAction::BestRewardIndex(Quest const* quest)
{
    ItemIds returnIds;
    ItemUsage bestUsage = ITEM_USAGE_NONE;
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;
    else
    {
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                bestUsage = ITEM_USAGE_EQUIP;
            else if (usage == ITEM_USAGE_BAD_EQUIP && bestUsage != ITEM_USAGE_EQUIP)
                bestUsage = usage;
            else if (usage != ITEM_USAGE_NONE && bestUsage == ITEM_USAGE_NONE)
                bestUsage = usage;
        }
        StatsWeightCalculator calc(bot);
        uint32 best = 0;
        float bestScore = 0;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == bestUsage || usage == ITEM_USAGE_REPLACE)
            {
                uint32 const itemId = quest->RewardChoiceItemId[i];
                ItemTemplate const* rewardProto = sObjectMgr->GetItemTemplate(itemId);
                uint8 rewardSlot = NULL_SLOT;
                if (rewardProto)
                    rewardSlot = botAI->FindEquipSlot(rewardProto, NULL_SLOT, true);
                float score = calc.CalculateItem(itemId, 0, rewardSlot);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
        return best;
    }
}

bool NewRpgBaseAction::OrganizeQuestLog()
{
    if (!TestConstraints::QuestChoicesAllowed(botAI->testConstraints))
        return false;

    int32 freeSlotNum = 0;

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            freeSlotNum++;
    }

    // it's ok if we have two more free slots
    if (freeSlotNum >= 2)
        return false;

    int32 dropped = 0;
    // remove quests that not worth doing or not capable of doing
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        bool const worthDoing = NewRpgHelpers::IsQuestWorthDoing(bot, quest);
        bool const capableDoing = NewRpgHelpers::IsQuestCapableDoing(bot, quest);
        bool const failed = bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED;
        if (!worthDoing || !capableDoing || failed)
        {
            QuestDropReason const reason = !worthDoing    ? QuestDropReason::NotWorth
                                           : !capableDoing ? QuestDropReason::NotCapable
                                                           : QuestDropReason::Failed;
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->RecordQuestDropped(questId, reason);
            dropped++;
        }
    }

    // drop more than 8 quests at once to avoid repeated accept and drop
    if (dropped >= 8)
        return true;

    // remove festival/class quests and quests in different zone
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        // A planner bot keeps its plan's quests: a carried quest is delivered in another zone (WT16)
        if (Planner::KeepsQuest(botAI, questId))
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const int64_t botZoneId = this->bot->GetZoneId();

        int32 const zoneOrSort = quest->GetZoneOrSort();
        if (zoneOrSort < 0 || (zoneOrSort > 0 && zoneOrSort != botZoneId))
        {
            QuestDropReason const reason = zoneOrSort < 0 ? QuestDropReason::ClassOrHoliday
                                                          : QuestDropReason::OtherZone;
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
            removeQuest.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->RecordQuestDropped(questId, reason);
            dropped++;
        }
    }

    if (dropped >= 8)
        return true;

    // clear quests log (but a planner bot's planned quests)
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId || Planner::KeepsQuest(botAI, questId))
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << (uint8)i;
        WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
        removeQuest.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
        if (botAI->GetMaster())
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "new_rpg_quest_dropped",
                "Quest dropped %quest",
                {{"%quest", ChatHelper::FormatQuest(quest)}}));
        botAI->RecordQuestDropped(questId, QuestDropReason::LogFull);
    }

    return true;
}

bool NewRpgBaseAction::SearchQuestGiverAndAcceptOrReward()
{
    OrganizeQuestLog();
    ObjectGuid npcOrGo = AI_VALUE(ObjectGuid, "new rpg quest giver");
    if (!npcOrGo)
        return false;

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, npcOrGo);
    if (!object)
        return false;

    RpgMemos& memo = botAI->rpgMemos;
    if (memo.questGiver != npcOrGo || GetMSTimeDiffToNow(memo.questGiverLastTry) > questGiverRetryGap)
    {
        memo.questGiver = npcOrGo;
        memo.questGiverSince = getMSTime();
    }
    memo.questGiverLastTry = getMSTime();

    // Unreachable, or an offer the interaction never clears: let the status action and grinding resume.
    if (GetMSTimeDiffToNow(memo.questGiverSince) > questGiverGiveUpTime)
    {
        LOG_DEBUG("playerbots", "[New RPG] {} gives up on quest giver {}", bot->GetName(), npcOrGo.ToString());
        memo.ignoredQuestGivers.insert(npcOrGo);
        memo.questGiver.Clear();
        return false;
    }

    if (bot->CanInteractWithQuestGiver(object))
    {
        InteractWithNpcOrGameObjectForQuest(npcOrGo);
        ForceToWait(5000);
        return true;
    }
    return MoveWorldObjectTo(npcOrGo);
}

// WT33b: the places come from the quest index (spawn heights; a baked floor where only a POI gives one), by the rule
// the planner's circuit uses (QuestPlaceRules.h). One snapshot lookup, only when the executor needs a destination
NewRpgBaseAction::PlaceAnswer NewRpgBaseAction::QuestPlaces(uint32 questId, std::vector<QuestDestination>& places,
                                                            WorldPosition const* left)
{
    places.clear();
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return PlaceAnswer::None;
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index)
        return PlaceAnswer::NotReady;
    Planner::QuestRecord const* record = index->Find(questId);
    if (!record)
        return PlaceAnswer::None;
    uint32 const mapId = bot->GetMapId();
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    QuestStatus const status = bot->GetQuestStatus(questId);
    if (status == QUEST_STATUS_COMPLETE)
    {
        if (Planner::AreaPoint const* ender = Planner::QuestPlaces::TurnInPlace(*record, mapId, x, y))
            places.push_back({WorldPosition(mapId, ender->x, ender->y, ender->z), -1, 0.0f});
    }
    else if (status == QUEST_STATUS_INCOMPLETE)
    {
        QuestStatusData const& progress = bot->getQuestStatusMap().at(questId);
        auto const open = [quest, &progress](int32 slot)
        {
            if (slot >= 0 && slot < QUEST_OBJECTIVES_COUNT)
                return quest->RequiredNpcOrGo[slot] != 0 &&
                       progress.CreatureOrGOCount[slot] < quest->RequiredNpcOrGoCount[slot];
            int32 const item = slot - QUEST_OBJECTIVES_COUNT;
            return item >= 0 && item < QUEST_ITEM_OBJECTIVES_COUNT && quest->RequiredItemId[item] != 0 &&
                   progress.ItemCount[item] < quest->RequiredItemCount[item];
        };
        Player* const player = bot;
        auto const hasSkill = [player](uint16 skill) { return player->HasSkill(skill); };
        Planner::QuestPlaces::Skip skip;
        if (left)
            skip = {true, left->GetPositionX(), left->GetPositionY()};
        for (Planner::QuestPlaces::Place const& place : Planner::QuestPlaces::OpenPlaces(
                 *record, mapId, x, y, static_cast<uint8>(bot->GetTeamId()), open, hasSkill, skip))
            places.push_back({WorldPosition(mapId, place.area->x, place.area->y, place.area->z),
                              record->objectives[place.objective].poiIndex, Planner::QuestPlaces::Roam(*place.area)});
    }
    return places.empty() ? PlaceAnswer::None : PlaceAnswer::Found;
}

bool NewRpgBaseAction::HasQuestPlace(uint32 questId)
{
    // A pick keeps to quests with a place within QUEST_PICK_YARDS, as the POI filter did; once ordered, the executor
    // walks to its place at any distance on the map (a long walk is travel's MoveFarTo)
    std::vector<QuestDestination> places;
    return QuestPlaces(questId, places) == PlaceAnswer::Found &&
           bot->GetDistance2d(places.front().pos.GetPositionX(), places.front().pos.GetPositionY()) < QUEST_PICK_YARDS;
}

bool NewRpgBaseAction::RoamInPlace(WorldPosition const& centre, float roam)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return false;

    Map* map = bot->GetMap();
    // As MoveRandomNear, but the point is spread evenly over the area's disc around its centre, at the place's height
    // (a building's floor), so the bot stays in the area however long it roams
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        float const distance = roam * std::sqrt(static_cast<float>(rand_norm()));
        float const angle = static_cast<float>(rand_norm()) * 2.0f * static_cast<float>(M_PI);
        float dx = centre.GetPositionX() + distance * std::cos(angle);
        float dy = centre.GetPositionY() + distance * std::sin(angle);
        float dz = centre.GetPositionZ();

        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        uint32 const typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        if (path.GetPathType() & ~typeOk)
            continue;
        if (!map->CanReachPositionAndGetValidCoords(bot, dx, dy, dz))
            continue;
        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
            continue;
        if (MoveCounted(bot->GetMapId(), dx, dy, dz, MeshMove::Kind::Other, path, false,
                        MovementPriority::MOVEMENT_NORMAL))
            return true;
    }
    return false;
}

bool NewRpgInternal::StartOrderedQuest(PlayerbotAI* botAI, Player* bot, uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    QuestStatus const questStatus = bot->GetQuestStatus(questId);
    if (!quest || (questStatus != QUEST_STATUS_INCOMPLETE && questStatus != QUEST_STATUS_COMPLETE))
    {
        LOG_INFO("playerbots", "[Route] {} quest {} not in the log", bot->GetName(), questId);
        return false;
    }
    LOG_INFO("playerbots", "[Route] {} quest {} ordered", bot->GetName(), questId);
    botAI->GetTravel().CancelLootInteraction("order");
    RpgStatus::Start(botAI, NewRpgInfo::DoQuest{quest, questId}, "order", StatusEnd::Outcome::Ordered);
    return true;
}
