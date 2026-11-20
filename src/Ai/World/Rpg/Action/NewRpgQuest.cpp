/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <list>
#include <memory>
#include <string_view>
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
#include "PursuitRules.h"
#include "QuestDef.h"
#include "QuestExecRules.h"
#include "QuestFailRecord.h"
#include "QuestGiveUp.h"
#include "QuestPackets.h"
#include "QuestPlaceRules.h"
#include "Random.h"
#include "StatsWeightCalculator.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldLogStatus.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
// questing WT35c: the quest's progress in the bot's log, its objectives' counts summed (the give-up clock restarts
// when it rises)
uint32 QuestProgress(Player* bot, uint32 questId)
{
    auto const it = bot->getQuestStatusMap().find(questId);
    if (it == bot->getQuestStatusMap().end())
        return 0;
    QuestStatusData const& progress = it->second;
    uint32 sum = progress.Explored ? 1 : 0;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        sum += progress.CreatureOrGOCount[i];
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        sum += progress.ItemCount[i];
    return sum;
}

// The order's run stamp: one per order run and bot (the clock tells a new run from the one before by it)
uint32 PursuitRun(PlayerbotAI* botAI, NewRpgInfo::DoQuest& data)
{
    if (!data.run)
        data.run = ++botAI->pursuitClocks.uses;
    return data.run;
}

// The order's give-up clock, ticked once per executor tick (PursuitRules.h): the cause the order is given up for,
// nullptr while it goes on. A quest no longer in the log (done, dropped) forgets its clock
char const* TickPursuit(PlayerbotAI* botAI, Player* bot, NewRpgInfo::DoQuest& data)
{
    Pursuit::Key const key{Pursuit::Kind::Quest, data.questId};
    QuestStatus const status = bot->GetQuestStatus(data.questId);
    if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
    {
        Pursuit::Forget(botAI->pursuitClocks, key);
        return nullptr;
    }
    Pursuit::Clock& clock = Pursuit::Take(botAI->pursuitClocks, key);
    uint32 const run = PursuitRun(botAI, data);
    uint32 const now = getMSTime();
    uint32 const since = data.pursuitTick ? getMSTimeDiff(data.pursuitTick, now) : 0;
    data.pursuitTick = now;
    Pursuit::Tick(clock, true, run, since, QuestProgress(bot, data.questId));
    uint32 const limit = Pursuit::LimitMs(sPlayerbotAIConfig.Questing.GiveUpMinutes,
                                          TestConstraints::GiveUpShortened(botAI->testConstraints));
    // At the ender the log no longer moves: the limit passed there is no ender the bot can use
    return Pursuit::Decide(clock, limit, nullptr,
                           status == QUEST_STATUS_COMPLETE ? Pursuit::NO_ENDER : Pursuit::TIMEOUT);
}
}  // namespace

void QuestGiveUp::Noted(PlayerbotAI* botAI, NewRpgInfo::DoQuest const& data, char const* cause)
{
    Player* const bot = botAI->GetBot();
    if (!bot || !data.questId)
        return;
    Pursuit::Clock& clock = Pursuit::Take(botAI->pursuitClocks, {Pursuit::Kind::Quest, data.questId});
    uint32 run = data.run;
    if (!run)
    {
        // Given up before the executor's first tick (the start's refusal): the run's clock starts here, at zero
        run = ++botAI->pursuitClocks.uses;
        Pursuit::Tick(clock, true, run, 0, QuestProgress(bot, data.questId));
    }
    if (!Pursuit::Report(clock, run))
        return;
    uint32 const minutes = Pursuit::Minutes(clock);
    EventLog::Emit(bot, WorldLogFormat::EVENT_QUEST_FAIL, cause, data.questId, minutes);
    QuestFailRecord::Note(bot, data.questId, cause, minutes);
    botAI->RecordQuestAbandoned(data.questId, std::string_view(cause) == Pursuit::NO_ENDER
                                                  ? QuestAbandonReason::CannotTurnIn
                                                  : QuestAbandonReason::NoProgress);
    LOG_DEBUG("playerbots", "[Quest] {} gives quest {} up: {} after {} min", bot->GetName(), data.questId, cause,
              minutes);
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    if (auto* ordered = std::get_if<NewRpgInfo::DoQuest>(&info.data); ordered && ordered->ordered)
    {
        // How long after the order the executor first ran (what ran before it shows in the rows between)
        ordered->ordered = false;
        uint32 const ms = GetMSTimeDiffToNow(info.startT);
        LOG_INFO("playerbots", "[Quest] {} quest {} first tick {} ms after the order", bot->GetName(), ordered->questId,
                 ms);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::QuestExec, "first_tick", "order", ms / 1000.0f, {},
                           ordered->questId);
    }

    // The order's give-up clock runs while its executor runs (questing WT35c)
    if (auto* doing = std::get_if<NewRpgInfo::DoQuest>(&info.data))
        if (char const* const cause = TickPursuit(botAI, bot, *doing))
        {
            RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = cause});
            return true;
        }

    if (SearchQuestGiverAndAcceptOrReward())
        return true;

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
// The log's drop of a quest slot through the abandon packet, as a player would; its QuestAbandon row names the reason
void DropQuestSlot(Player* bot, uint16 slot, QuestDropReason reason)
{
    WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
    packet << static_cast<uint8>(slot);
    WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
    removeQuest.Read();
    WorldLog::QuestDropWord const word(QuestDropReasonName(reason));
    bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
}
}  // namespace

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
            DropQuestSlot(bot, i, reason);
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
            DropQuestSlot(bot, i, reason);
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
        DropQuestSlot(bot, i, QuestDropReason::LogFull);
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
    if (Approach(npcOrGo) != ::GoTo::Answer::Failed)
        return true;
    // Its stand point is not carried (the approach's Stuck row): skipped as a giver given up on
    memo.ignoredQuestGivers.insert(npcOrGo);
    memo.questGiver.Clear();
    return false;
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
        if (Planner::AreaPoint const* ender =
                Planner::QuestPlaces::TurnInPlace(*record, mapId, x, y, static_cast<uint8>(bot->GetTeamId())))
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
    // walks to its place at any distance on the map (a long walk is a trip, GoTo)
    std::vector<QuestDestination> places;
    return QuestPlaces(questId, places) == PlaceAnswer::Found &&
           bot->GetDistance2d(places.front().pos.GetPositionX(), places.front().pos.GetPositionY()) < QUEST_PICK_YARDS;
}

bool NewRpgBaseAction::RoamInPlace(WorldPosition const& centre, float roam)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return false;

    // As MoveRandomNear, but the point is spread evenly over the area's disc around its centre, at the place's height
    // (a building's floor), so the bot stays in the area however long it roams
    for (int attempt = 0; attempt < ROAM_SAMPLES; ++attempt)
    {
        float const distance = roam * std::sqrt(static_cast<float>(rand_norm()));
        float const angle = static_cast<float>(rand_norm()) * 2.0f * static_cast<float>(M_PI);
        if (OrderRoamSample(centre.GetPositionX() + distance * std::cos(angle),
                            centre.GetPositionY() + distance * std::sin(angle), centre.GetPositionZ(),
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
    // What the bot was doing when the order came: the engine, its target, its move
    AiObjectContext* const context = botAI->GetAiObjectContext();
    Unit* const target = context->GetValue<Unit*>("current target")->Get();
    LastMovement const& lastMove = context->GetValue<LastMovement&>("last movement")->Get();
    LOG_INFO("playerbots", "[Route] {} quest {} ordered: engine {}, target {} {:.1f} yd, moving {}, move prio {}",
             bot->GetName(), questId, botAI->GetState() == BOT_STATE_COMBAT ? "combat" : "noncombat",
             target ? target->GetEntry() : 0, target ? bot->GetDistance(target) : 0.0f, bot->isMoving() ? 1 : 0,
             static_cast<int>(lastMove.priority));
    botAI->GetTravel().CancelLootInteraction("order");
    NewRpgInfo::DoQuest order{quest, questId};
    order.ordered = true;
    RpgStatus::Start(botAI, std::move(order), "order", StatusEnd::Outcome::Ordered);
    EndQuestAtStart(botAI, bot);
    return true;
}
