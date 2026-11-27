/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgHelpers.h"

#include <algorithm>
#include <optional>
#include <variant>

#include "Creature.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "GameEventMgr.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestFailRecord.h"
#include "QuestFields.h"
#include "QuestLevelRules.h"
#include "QuestLogRules.h"
#include "TestConstraints.h"
#include "World.h"

namespace NewRpgHelpers
{
bool OnOrder(PlayerbotAI* botAI)
{
    if (!botAI)
        return false;
    auto const* doing = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    return doing && doing->order;
}

std::shared_ptr<Planner::QuestLog::Record const> OrderRecord(Player* bot, Planner::QuestOrderRun& run)
{
    return QuestFail::Reread(QuestFailRecord::Rows(), bot->GetGUID().GetCounter(), run.fails, run.failsVersion,
                             [](std::vector<std::pair<std::uint32_t, QuestFail::Row>> const& rows)
                             { return Planner::QuestLog::Read(rows); });
}

std::shared_ptr<Planner::QuestLog::Record const> OrderRecord(PlayerbotAI* botAI, Player* bot)
{
    auto* const doing = botAI ? std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data) : nullptr;
    if (doing && doing->order)
        return OrderRecord(bot, *doing->order);
    return std::make_shared<Planner::QuestLog::Record const>(
        Planner::QuestLog::Read(QuestFailRecord::RowsOf(bot->GetGUID().GetCounter())));
}

bool KeepsLogRule(PlayerbotAI* botAI) { return botAI && (OnOrder(botAI) || Planner::Owns(botAI)); }

bool HolidayEnded(Quest const* quest)
{
    uint16 const event = quest->GetEventIdForQuest();
    return event && !sGameEventMgr->IsActiveEvent(event);
}

Planner::QuestLog::Held LogHeld(Player* bot, Quest const* quest)
{
    QuestStatus const status = bot->GetQuestStatus(quest->GetQuestId());
    return {.id = quest->GetQuestId(),
            .level = quest->GetQuestLevel(),
            .ready = status == QUEST_STATUS_COMPLETE,
            .failed = status == QUEST_STATUS_FAILED,
            .holidayEnded = HolidayEnded(quest)};
}

namespace
{
// The offer as the one accept reads it; its repeat kinds are the quest index's (PlannerQuests.cpp FlagsOf)
Planner::QuestLog::Offer OfferOf(Player* bot, Quest const* quest)
{
    return {.id = quest->GetQuestId(),
            .level = quest->GetQuestLevel(),
            .repeats = quest->IsDaily() || quest->IsWeekly() || quest->IsRepeatable(),
            .holidayEnded = HolidayEnded(quest),
            .gameAllows = bot->CanTakeQuest(quest, false)};
}
}  // namespace

bool LogAccept::Allows(Quest const* quest)
{
    if (!_built)
    {
        _built = true;
        _grayLevel = Acore::XP::GetGrayLevel(_bot->GetLevel());
        _givenUp = OrderRecord(GET_PLAYERBOT_AI(_bot), _bot)->givenUp;
    }
    Planner::QuestLog::Facts const facts{.grayLevel = _grayLevel, .givenUp = _givenUp};
    return Planner::QuestLog::Accepts(OfferOf(_bot, quest), facts);
}

bool LogAccepts(PlayerbotAI* botAI, Player* bot, Quest const* quest)
{
    if (!KeepsLogRule(botAI))
        return true;
    LogAccept take(bot);
    return take.Allows(quest);
}

bool FreeBotTakes(Player* bot, Quest const* quest)
{
    return bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) && IsQuestWorthDoing(bot, quest) &&
           IsQuestCapableDoing(bot, quest);
}

bool CanInteractWithQuestGiver(Player* bot, Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that removes the distance check and keeps all other checks
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:  // Player::GetNPCIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            // unit checks
            if (!guid)
                return false;

            if (!bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
                return false;

            if (bot->IsInFlight())
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            // Deathstate checks
            if (!bot->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_VISIBLE_TO_GHOSTS))
                return false;

            // alive or spirit healer
            if (!creature->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_INTERACT_WHILE_DEAD))
                return false;

            // appropriate npc type
            if (!creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                return false;

            // not allow interaction under control, but allow with own pets
            if (creature->GetCharmerGUID())
                return false;

            // xinef: perform better check
            if (creature->GetReactionTo(bot) <= REP_UNFRIENDLY)
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:  // Player::GetGameObjectIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
                {
                    // Players cannot interact with gameobjects that use the "Point" icon
                    if (go->GetGOInfo()->IconName == "Point")
                        return false;

                    return true;
                }
            }

            return false;
        }
        // unused for now
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool RoomRefusesReward(Player* bot, Quest const* quest, uint32 choice)
{
    bool full = false;
    auto const stores = [bot, &full](uint32 itemId, uint32 count)
    {
        if (!itemId)
            return true;
        ItemPosCountVec dest;
        InventoryResult const result = bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count);
        full = full || result == EQUIP_ERR_INVENTORY_FULL;
        return result == EQUIP_ERR_OK || result == EQUIP_ERR_INVENTORY_FULL;
    };
    if (quest->GetRewChoiceItemsCount() > 0 && choice < QUEST_REWARD_CHOICES_COUNT &&
        !stores(quest->RewardChoiceItemId[choice], quest->RewardChoiceItemCount[choice]))
        return false;
    for (uint32 i = 0; i < QUEST_REWARDS_COUNT; ++i)
        if (!stores(quest->RewardItemId[i], quest->RewardItemIdCount[i]))
            return false;
    return full;
}

bool HandInRewardable(Player* bot, Quest const* quest, uint32 choice, uint64 constraints)
{
    if (bot->CanRewardQuest(quest, choice, false))
        return true;
    return KeepsLogRule(GET_PLAYERBOT_AI(bot)) && TestConstraints::DestroysForSpace(constraints) &&
           bot->CanRewardQuest(quest, false) && RoomRefusesReward(bot, quest, choice);
}

bool HasQuestToAcceptOrReward(Player* bot, WorldObject* object, TestConstraints::Set constraints, LogAccept* take)
{
    ObjectGuid guid = object->GetGUID();
    bot->PrepareQuestMenu(guid);
    QuestMenu const& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return false;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;
        // A full bot's ender is sought too when the room rule makes room there (questing WQ43)
        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_COMPLETE && HandInRewardable(bot, quest, 0, constraints))
        {
            return true;
        }
    }
    std::optional<LogAccept> own;
    if (!take && KeepsLogRule(GET_PLAYERBOT_AI(bot)))
        take = &own.emplace(bot);
    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest || bot->GetQuestStatus(item.QuestId) != QUEST_STATUS_NONE)
            continue;

        bool const offered = take ? take->Allows(quest) && bot->CanAddQuest(quest, false) : FreeBotTakes(bot, quest);
        if (offered)
            return TestConstraints::GiverSought(false, true, constraints);
    }
    return false;
}

bool IsQuestWorthDoing(Player* bot, Quest const* quest)
{
    bool isLowLevelQuest =
        bot->GetLevel() > (bot->GetQuestLevel(quest) + sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));

    if (isLowLevelQuest)
        return false;

    if (quest->IsRepeatable())
        return false;

    if (quest->IsSeasonal())
        return false;

    return true;
}

bool IsQuestCapableDoing(Player* bot, Quest const* quest)
{
    bool highLevelQuest = bot->GetLevel() + QUEST_MAX_LEVELS_ABOVE_BOT < bot->GetQuestLevel(quest);
    if (highLevelQuest)
        return false;

    // Elite quest and dungeon quest etc
    if (quest->GetType() != 0)
        return false;

    // now we only capable of doing solo quests
    if (quest->GetSuggestedPlayers() >= 2)
        return false;

    return true;
}

ObjectGuid FindQuestGiver(PlayerbotAI* botAI, float distanceLimit)
{
    Player* bot = botAI->GetBot();
    GuidSet const& ignored = botAI->rpgMemos.ignoredQuestGivers;
    WorldObject* nearestObject = nullptr;
    float nearest = 0.0f;
    // A bot on an order takes the nearest giver of both caches; one accept's facts serve the whole search of a bot the
    // log's rule covers (WQ34)
    bool const onOrder = OnOrder(botAI);
    std::optional<LogAccept> take;
    if (KeepsLogRule(botAI))
        take.emplace(bot);

    for (char const* candidates : {"possible new rpg targets", "possible new rpg game objects"})
    {
        GuidVector const guids = botAI->GetAiObjectContext()->GetValue<GuidVector>(candidates)->Get();
        for (ObjectGuid const& guid : guids)
        {
            if (ignored.count(guid))
                continue;

            WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);

            if (!object || !object->IsInWorld())
                continue;

            if (distanceLimit && bot->GetDistance(object) > distanceLimit)
                continue;

            float const distance = bot->GetExactDist(object);
            if (onOrder && nearestObject && distance >= nearest)
                continue;

            if (CanInteractWithQuestGiver(bot, object) &&
                HasQuestToAcceptOrReward(bot, object, botAI->testConstraints, take ? &*take : nullptr))
            {
                if (!nearestObject || nearest > distance)
                {
                    nearestObject = object;
                    nearest = distance;
                }
                if (!onOrder)
                    break;
            }
        }
    }

    return nearestObject ? nearestObject->GetGUID() : ObjectGuid();
}
}  // namespace NewRpgHelpers

bool NewRpgHelpers::RealShapeshift(Player* bot)
{
    ShapeshiftForm const form = bot->GetShapeshiftForm();
    SpellShapeshiftFormEntry const* shape = form != FORM_NONE ? sSpellShapeshiftFormStore.LookupEntry(form) : nullptr;
    return form != FORM_NONE && !(shape && (shape->flags1 & SHAPESHIFT_FLAG_STANCE));
}
