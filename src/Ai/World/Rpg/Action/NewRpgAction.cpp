/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"
#include "AreaDefines.h"
#include "BroadcastHelper.h"
#include "CellImpl.h"
#include "CensusMgr.h"
#include "ChatHelper.h"
#include "DBCStores.h"
#include "ErrandRules.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "IVMapMgr.h"
#include "InventorySlots.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "NewRpgTriggers.h"
#include "Object.h"
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
#include "QuestIndex.h"
#include "Random.h"
#include "RouteService.h"
#include "SharedDefines.h"
#include "StartingHistoryRules.h"
#include "Timer.h"
#include "TravelGraph.h"
#include "TravelLegRules.h"
#include "TravelMgr.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "WaypointMovementGenerator.h"
#include "WorldLog.h"
#include "WorldLogStatus.h"
#include "WorldTravelRules.h"
#include "WorldPacket.h"
#include "G3D/Vector2.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace
{
// Ends the bot's errand: stamps the errand check, measures t from a visited vendor and records the run; an errand given
// up records abortSource (a literal: errand_abort, or errand_sick when the sickness ended it)
void FinishErrands(PlayerbotAI* botAI, bool visited, char const* abortSource = "errand_abort")
{
    WorldLog::StatusReason const reasonScope("finished");
    NewRpgInfo& info = botAI->rpgInfo;
    auto const* data = std::get_if<NewRpgInfo::Errands>(&info.data);
    if (!data)
        return;

    Player* bot = botAI->GetBot();
    WorldTravel::SlotMeasure& measure =
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    uint32 const now = getMSTime();
    float const minutes = static_cast<float>(getMSTimeDiff(info.startT, now)) / (MINUTE * IN_MILLISECONDS);
    WorldTravel::StampErrandCheck(measure, now);
    if (visited)
    {
        WorldTravel::AddTripMinutes(measure, minutes);
        EventLog::Emit(bot, "Inventory", "errand_run", data->vendorEntry,
                       int64(bot->GetMoney()) - int64(data->moneyAtStart));
    }
    else
        EventLog::Emit(bot, "Inventory", abortSource, data->vendorEntry, int64(minutes));

    info.ChangeToIdle();
}

// The Travel event sources of an abort ("abort:<reason>"); the trip's abort decision takes the reason
constexpr std::string_view TRAVEL_ABORT_PREFIX = "abort:";

// Gives the bot's trip up: the Travel event (source "abort:<reason>", a literal), the trip's abort decision, idle
void GiveTripUp(PlayerbotAI* botAI, NewRpgInfo::Travel const& data, char const* why, char const* source)
{
    Player* bot = botAI->GetBot();
    if (data.place.empty())
        LOG_DEBUG("playerbots", "[Route] {} gives the trip up: {}", bot->GetName(), why);
    else
        LOG_INFO("playerbots", "[Route] {} gives the trip to {} up: {}", bot->GetName(), data.place, why);
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    EventLog::Emit(bot, "Travel", source, data.tripId, seconds);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "abort",
                       WorldLogFormat::AfterPrefix(source, TRAVEL_ABORT_PREFIX), static_cast<float>(seconds),
                       {{"replans", static_cast<float>(data.trip.replans)}}, data.tripId);
    WorldLog::StatusReason const reasonScope("abort");
    botAI->rpgInfo.ChangeToIdle();
}

// The stock per upkeep kind now as the needs count it (one snapshot: the kept grade reads the level and the equipped
// ammo), for the use measure
UpkeepNeeds::StockLevels StockLevelsOf(Player* bot)
{
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    return UpkeepNeeds::LevelsOf(index.get(), Planner::FillSnapshot(bot, 0));
}

// Lots of one vendor row through the core's purchase (Player::BuyItemFromVendorSlot: count is lots, the price the
// game's with the reputation discount; it rechecks every cost and the bag room), judged by the item count because its
// return is no success flag. One call for all lots, then lot by lot when that fails (the bags hold fewer), up to the
// first failure. Returns the lots bought
uint32 BuyLots(Player* bot, Creature* vendor, uint32 slot, uint32 item, uint32 lots)
{
    uint32 const before = bot->GetItemCount(item, false);
    bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, item, static_cast<uint8>(lots), NULL_BAG, NULL_SLOT);
    if (bot->GetItemCount(item, false) > before)
        return lots;

    uint32 bought = 0;
    for (; lots > 1 && bought < lots; ++bought)
    {
        uint32 const count = bot->GetItemCount(item, false);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, item, 1, NULL_BAG, NULL_SLOT);
        if (bot->GetItemCount(item, false) <= count)
            break;
    }
    return bought;
}

// The shopping list at the errand's vendor (upkeep WU6, §3.6): the needs from a fresh snapshot (the sell and the
// repair changed the gold), each stock in Fund's order bought in whole lots within its funded share (PlanShopping),
// the vendor's own rows rechecked (gold only, a limited row's live count). An item the bot's item usage would sell
// (VENDOR, AH or NONE: the generic sell of the next visit) is never bought. A repair paid at this stop already spent
// what the gold pays; the gear left worn costs more than the rest, so it holds no gold back here. A failed purchase
// skips the item; the bot goes on. At most one row per stock kind
void ShopAtVendor(PlayerbotAI* botAI, Creature* vendor, bool repaired)
{
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    VendorItemData const* rows = vendor->GetVendorItems();
    if (!index || !rows || !rows->GetItemCount())
        return;

    Player* bot = botAI->GetBot();
    Planner::Snapshot snapshot = Planner::FillSnapshot(bot, 0);
    if (repaired)
        snapshot.repairCost = 0;
    UpkeepNeeds::NeedList const needs =
        UpkeepNeeds::ComputeNeeds(snapshot, *index, Planner::BotNeedRates(botAI, snapshot));
    std::vector<UpkeepNeeds::ShopLine> const lines =
        UpkeepNeeds::PlanShopping(needs, *index, snapshot, vendor->GetEntry());
    for (UpkeepNeeds::ShopLine const& line : lines)
    {
        char const* const word = UpkeepNeeds::ShopWord(line.kind);
        if (line.outcome == UpkeepNeeds::ShopOutcome::Budget)
        {
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "budget",
                               static_cast<float>(line.wanted),
                               {{word, static_cast<float>(line.units)},
                                {"funded", static_cast<float>(line.funded)},
                                {"gold", static_cast<float>(bot->GetMoney())}},
                               line.item);
            continue;
        }

        if (line.outcome == UpkeepNeeds::ShopOutcome::Buy)
        {
            ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", line.item)->Get();
            if (usage == ITEM_USAGE_NONE || usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH)
            {
                WorldLog::Decision(
                    bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "would_sell", static_cast<float>(line.copper),
                    {{word, static_cast<float>(line.units)}, {"usage", static_cast<float>(usage)}}, line.item);
                continue;
            }
        }

        uint32 lots = line.lots;
        uint32 slot = rows->GetItemCount();  // the vendor's own row of the item, payable in gold
        if (line.outcome == UpkeepNeeds::ShopOutcome::Buy)
            for (uint32 at = 0; at < rows->GetItemCount(); ++at)
            {
                VendorItem const* row = rows->GetItem(at);
                if (!row || row->item != line.item || row->ExtendedCost)
                    continue;
                if (row->maxcount)
                    lots = std::min(lots, vendor->GetVendorItemCurrentCount(row) / line.lotSize);
                slot = at;
                break;
            }
        if (slot == rows->GetItemCount() || !lots)
        {
            WorldLog::Decision(
                bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "not_sold", static_cast<float>(line.wanted),
                {{word, static_cast<float>(line.units)}, {"target", static_cast<float>(line.target)}}, line.item);
            continue;
        }

        uint32 const moneyBefore = bot->GetMoney();
        uint32 const bought = BuyLots(bot, vendor, slot, line.item, lots);
        uint32 const paid = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0;
        if (!bought)
        {
            // The core refused (the bags full, a reputation or vendor condition): skip the item
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "refused",
                               static_cast<float>(line.copper),
                               {{word, static_cast<float>(line.units)},
                                {"free_slots", static_cast<float>(WorldTravel::FreeGeneralSlots(bot))}},
                               line.item);
            continue;
        }

        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "buy", line.due ? "stock_low" : "top_up",
                           static_cast<float>(paid),
                           {{word, static_cast<float>(bought * line.lotSize)},
                            {"target", static_cast<float>(line.target)},
                            {"funded", static_cast<float>(line.funded)}},
                           line.item);
    }
}

WorldTravel::ErrandPlace ErrandPlaceOf(WorldLocation const& loc)
{
    return {loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ()};
}

// The equipped gear's durability in percent (as the planner snapshot counts it)
uint32 EquippedDurabilityPercent(Player* bot)
{
    uint64 durability = 0;
    uint64 maxDurability = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            durability += item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            maxDurability += item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        }
    return static_cast<uint32>(
        maxDurability ? std::min<uint64>(durability * WorldTravel::PERCENT / maxDurability, WorldTravel::PERCENT)
                      : WorldTravel::PERCENT);
}

// The bot's completed quests with the nearest ender on its map (the planner's quest index; none before it is built)
void AddCompletedQuests(Player* bot, std::vector<WorldTravel::CompletedQuest>& completed)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index)
        return;

    uint32 const mapId = bot->GetMapId();
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;

        Planner::QuestRecord const* record = index->Find(questId);
        if (!record)
            continue;

        Planner::QuestPlace const* ender = nullptr;
        float enderYards = std::numeric_limits<float>::max();
        for (uint32 const placeIdx : record->enders)
        {
            if (placeIdx >= index->places.size() || index->places[placeIdx].mapId != mapId)
                continue;

            Planner::QuestPlace const& place = index->places[placeIdx];
            float const yards = bot->GetExactDist(place.x, place.y, place.z);
            if (yards < enderYards)
            {
                ender = &place;
                enderYards = yards;
            }
        }
        if (!ender)
            continue;

        WorldTravel::CompletedQuest quest;
        quest.QuestId = questId;
        quest.Ender = ender->entry;
        quest.EnderIsObject = ender->isObject;
        quest.Place = {ender->mapId, ender->x, ender->y, ender->z};
        quest.WorthCopper = static_cast<float>(std::max<int32>(record->copperReward, 0));
        completed.push_back(quest);
    }
}
}  // namespace

void TellRpgStatusAction::WhisperStatusChange(Player* owner, std::string const& statusName)
{
    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        RPG_STATUS_CHANGED_KEY, RPG_STATUS_CHANGED_DEFAULT,
        {{"%status", statusName}});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
}

bool TellRpgStatusAction::Execute(Event event)
{
    WorldLog::StatusReason const reasonScope("order");
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    if (text.empty())
    {
        std::string out = botAI->rpgInfo.ToString();
        bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
        return true;
    }

    Player* master = botAI->GetMaster();
    bool isMaster = master && master->GetGUID() == owner->GetGUID();
    bool isGM = owner->GetSession() && owner->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    if (!isMaster && !isGM)
    {
        std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "rpg_debug_permission_error",
            "Only your master or a GM can change my rpg status.", {});
        bot->Whisper(msg, LANG_UNIVERSAL, owner);
        return false;
    }

    std::string name = text;
    uint32 questId = 0;
    static std::string const doQuestPrefix = "do quest ";
    size_t doQuestPos = text.find(doQuestPrefix);
    if (doQuestPos != std::string::npos)
    {
        name = "do quest";
        std::string idStr = text.substr(doQuestPos + doQuestPrefix.length());
        try
        {
            questId = static_cast<uint32>(std::stoul(idStr));
        }
        catch (std::exception const&)
        {
            questId = 0;
        }
    }

    NewRpgStatus status = NewRpgInfo::StatusFromString(name);
    NewRpgInfo& info = botAI->rpgInfo;

    if (status == RPG_IDLE)
    {
        info.ChangeToIdle();
        WhisperStatusChange(owner, "IDLE");
        return true;
    }
    else if (status == RPG_REST)
    {
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        WhisperStatusChange(owner, "REST");
        return true;
    }
    else if (status == RPG_WANDER_RANDOM)
    {
        info.ChangeToWanderRandom();
        WhisperStatusChange(owner, "WANDER_RANDOM");
        return true;
    }
    else if (status == RPG_WANDER_NPC)
    {
        info.ChangeToWanderNpc();
        WhisperStatusChange(owner, "WANDER_NPC");
        return true;
    }
    else if (status == RPG_GO_GRIND)
    {
        WorldPosition pos = SelectRandomGrindPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_grind_pos_error", "No grind position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoGrind(pos);
        WhisperStatusChange(owner, "GO_GRIND");
        return true;
    }
    else if (status == RPG_GO_CAMP)
    {
        WorldPosition pos = SelectRandomCampPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_camp_pos_error", "No camp position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoCamp(pos);
        WhisperStatusChange(owner, "GO_CAMP");
        return true;
    }
    else if (status == RPG_TRAVEL_FLIGHT)
    {
        uint32 flightMasterEntry = 0;
        WorldPosition flightMasterPos;
        std::vector<uint32> path;
        if (!SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_flight_path_error", "No flight path available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, std::move(path));
        WhisperStatusChange(owner, "TRAVEL_FLIGHT");
        return true;
    }
    else if (status == RPG_OUTDOOR_PVP)
    {
        info.ChangeToOutdoorPvp();
        WhisperStatusChange(owner, "OUTDOOR_PVP");
        return true;
    }
    else if (status == RPG_DO_GATHER)
    {
        if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_gather_no_profession_error", "I have neither Herbalism nor Mining, so I can't gather.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoGather();
        WhisperStatusChange(owner, "DO_GATHER");
        return true;
    }
    else if (status == RPG_DO_QUEST)
    {
        if (!questId)
        {
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 qid = bot->GetQuestSlotQuestId(slot);
                if (!qid)
                    continue;
                std::vector<POIInfo> poi;
                if (GetQuestPOIPosAndObjectiveIdx(qid, poi, true))
                {
                    questId = qid;
                    break;
                }
            }
        }
        if (!questId)
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_quest_error", "No quest available; use 'do quest <id>'.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatus questStatus = bot->GetQuestStatus(questId);
        if (!quest || (questStatus != QUEST_STATUS_INCOMPLETE && questStatus != QUEST_STATUS_COMPLETE))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_invalid_quest_error", "Invalid quest %quest_id",
                {{"%quest_id", std::to_string(questId)}});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoQuest(questId, quest);
        WhisperStatusChange(owner, "DO_QUEST " + std::to_string(questId));
        return true;
    }

    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "rpg_unknown_status_error",
        "Unknown rpg status. Options: idle, rest, wander random, wander npc, "
        "go grind, go camp, do quest [<id>], travel flight, outdoor pvp, do gather.", {});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
    return false;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    WorldLog::StatusReason const reasonScope("order");
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

namespace
{
constexpr uint32 HEARTHSTONE_SPELL = 8690;
constexpr uint32 TRAVEL_ORDER_TTL_MS = 60 * IN_MILLISECONDS;

std::mutex travelOrdersLock;
std::unordered_map<ObjectGuid, TravelOrder> travelOrders;  // guarded by travelOrdersLock
std::atomic<uint32> travelOrdersWaiting{0};                // travelOrders.size(): the map threads' cheap check

// A GM order taken on the bot's map thread: the state is checked again (the command saw it on the world thread)
bool ApplyTravelOrder(PlayerbotAI* botAI, Player* bot, TravelOrder const& order)
{
    WorldLog::StatusReason const reasonScope("order");
    NewRpgInfo& info = botAI->rpgInfo;
    if (order.stop)
    {
        auto const* data = std::get_if<NewRpgInfo::Travel>(&info.data);
        if (!data)
        {
            LOG_INFO("playerbots", "[Route] {} stop: not travelling", bot->GetName());
            return false;
        }
        LOG_INFO("playerbots", "[Route] {} gives the trip up: stopped", bot->GetName());
        uint32 const seconds = GetMSTimeDiffToNow(data->tripStartMs) / IN_MILLISECONDS;
        EventLog::Emit(bot, "Travel", "abort:stopped", data->tripId, seconds);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "abort", "stopped", static_cast<float>(seconds),
                           {{"replans", static_cast<float>(data->trip.replans)}}, data->tripId);
        info.ChangeToIdle();
        return true;
    }

    char const* refusal = nullptr;
    if (!bot->IsAlive())
        refusal = "dead";
    else if (bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS))
        refusal = "sick";  // a sick bot rests (upkeep §3.5)
    else if (bot->IsInFlight())
        refusal = "in flight";
    else if (bot->GetMap()->Instanceable())
        refusal = "in an instance";
    if (refusal)
    {
        LOG_INFO("playerbots", "[Route] {} travel to {} refused: {}", bot->GetName(), order.place, refusal);
        return false;
    }
    LOG_INFO("playerbots", "[Route] {} travel to {} ({}, {:.0f}, {:.0f}, {:.0f}) ordered", bot->GetName(), order.place,
             order.dest.GetMapId(), order.dest.GetPositionX(), order.dest.GetPositionY(), order.dest.GetPositionZ());
    info.ChangeToTravel(order.dest, order.place);
    return true;
}
}  // namespace

void TravelOrders::Post(ObjectGuid bot, TravelOrder order)
{
    std::lock_guard<std::mutex> guard(travelOrdersLock);
    travelOrders[bot] = std::move(order);
    travelOrdersWaiting.store(static_cast<uint32>(travelOrders.size()), std::memory_order_release);
}

bool TravelOrders::Take(ObjectGuid bot, TravelOrder& order)
{
    if (!travelOrdersWaiting.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> guard(travelOrdersLock);
    // Orders nobody took in time are dropped on any take, also a miss (so no order keeps every bot at the lock), and
    // logged: the bot is not in the new rpg strategy, logged out or in combat
    for (auto it = travelOrders.begin(); it != travelOrders.end();)
    {
        if (GetMSTimeDiffToNow(it->second.postedMs) <= TRAVEL_ORDER_TTL_MS)
        {
            ++it;
            continue;
        }
        LOG_INFO("playerbots", "[Route] order '{}' for bot {} dropped: not taken within {} s", it->second.place,
                 it->first.GetCounter(), TRAVEL_ORDER_TTL_MS / IN_MILLISECONDS);
        it = travelOrders.erase(it);
    }
    auto const found = travelOrders.find(bot);
    bool taken = false;
    if (found != travelOrders.end())
    {
        order = std::move(found->second);
        travelOrders.erase(found);
        taken = true;
    }
    travelOrdersWaiting.store(static_cast<uint32>(travelOrders.size()), std::memory_order_release);
    return taken;
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    Planner::PlannerHook(botAI, bot);
    {
        TravelOrder order;
        if (TravelOrders::Take(bot->GetGUID(), order) && ApplyTravelOrder(botAI, bot, order))
            return true;
    }
    NewRpgStatus status = info.GetStatus();
    // Resurrection sickness (upkeep §3.5): a sick bot rests until the sickness ends. Any other status ends, an errand
    // and a trip as given up (errand_sick, abort:sick); a flight in the air lands first. The status pick, the errand
    // start and the travel orders keep it resting
    bool const sick = bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS);
    if (Upkeep::SickEndsStatus(status, sick, bot->IsInFlight()))
    {
        if (status == RPG_ERRANDS)
            FinishErrands(botAI, false, "errand_sick");
        else if (auto const* trip = std::get_if<NewRpgInfo::Travel>(&info.data))
            GiveTripUp(botAI, *trip, "sick", "abort:sick");
        WorldLog::StatusReason const reasonScope("sick");
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    switch (status)
    {
        case RPG_IDLE:
        {
            // A planner bot's goal (WT16); the weighted pick for the A/B group and as the counted fallback. A sick bot
            // takes no goal: the weighted pick lets it only rest (upkeep §3.5)
            Planner::IdleStep const step =
                sick ? Planner::IdleStep{}
                     : Planner::OnIdle(botAI, bot,
                                       [this](uint32 questId)
                                       {
                                           std::vector<POIInfo> poiInfo;
                                           return !botAI->lowPriorityQuest.contains(questId) &&
                                                  GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true);
                                       });
            if (step.started)
                return true;
            if (step.wait)
                return false;
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP, RPG_DO_GATHER},
                                      step.reason);
        }

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                WorldLog::StatusReason const reasonScope("arrived");
                info.ChangeToWanderRandom();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                WorldLog::StatusReason const reasonScope("arrived");
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                WorldLog::StatusReason const reasonScope("arrived");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE; a sick bot's rest lasts until the sickness ends
            if (Upkeep::RestEnds(info.HasStatusPersisted(statusRestDuration), sick))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_GATHER:
        {
            // DO_GATHER -> IDLE
            if (info.HasStatusPersisted(statusDoGatherDuration))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_ERRANDS:
        {
            // ERRANDS -> IDLE: a trip that has not ended in time is given up
            if (info.HasStatusPersisted(WorldTravel::ERRAND_DEADLINE_MS))
            {
                auto const* errands = std::get_if<NewRpgInfo::Errands>(&info.data);
                FinishErrands(botAI, errands && errands->visited);
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // Prefer a giver with something to accept or reward, else any nearby npc; none -> IDLE
        ObjectGuid npcOrGo = NewRpgHelpers::FindQuestGiver(botAI);
        if (npcOrGo.IsEmpty())
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            if (!possibleTargets.empty())
            {
                uint32 idx = urand(0, possibleTargets.size() - 1);
                WorldObject* object = ObjectAccessor::GetWorldObject(*bot, possibleTargets[idx]);
                if (object && object->IsInWorld())
                    npcOrGo = object->GetGUID();
            }
        }
        if (npcOrGo.IsEmpty())
        {
            WorldLog::StatusReason const reasonScope("no_target");
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);

            if (Creature* creature = object->ToCreature())
            {
                constexpr uint32 MIN_VENDOR_BALANCE = 1000;  // 10 silver

                if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) && AI_VALUE(bool, "can sell"))
                {
                    botAI->DoSpecificAction("sell", Event("sell", "vendor"), true);

                    // Buying sorts the vendor list through the weight calculator; skip it for
                    // bots without money for cheap consumables.
                    if (bot->GetMoney() >= MIN_VENDOR_BALANCE)
                        botAI->DoSpecificAction("buy", Event("buy", "vendor"), true);
                }

                if (creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && AI_VALUE(bool, "can repair"))
                    botAI->DoSpecificAction("repair", Event("repair"), true);
            }

            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgStartErrandsAction::Execute(Event /*event*/)
{
    WorldLog::StatusReason const reasonScope("errand");
    WorldTravel::SlotMeasure& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    // The causes the trigger saw: full bags (read before the stamp), the repair, a due stock
    bool const bagsFull = AI_VALUE(uint8, "bag space") >= WorldTravel::ERRAND_PRESSURE_PERCENT;
    bool const repairWanted = WorldTravel::RepairErrandWanted(bot);
    WorldTravel::StampErrandCheck(measure, getMSTime());
    measure.HasVendor = false;

    // The needs (upkeep §3.3): the consumable stocks the gold pays a lot of, the due ones and the others, and the
    // vendors that cover them (the immutable index; one snapshot on the bot's map thread)
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    UpkeepNeeds::NeedList needs;
    std::unordered_map<uint32, uint8> cover;
    uint8 dueKinds = 0;
    uint8 unmetKinds = 0;
    if (index)
    {
        Planner::Snapshot const snapshot = Planner::FillSnapshot(bot, 0);
        needs = UpkeepNeeds::ComputeNeeds(snapshot, *index, Planner::BotNeedRates(botAI, snapshot));
        dueKinds = UpkeepNeeds::PayableKinds(needs, true);
        unmetKinds = UpkeepNeeds::PayableKinds(needs, false);
        if (dueKinds | unmetKinds)
            cover = UpkeepNeeds::VendorCover(*index, snapshot, needs, dueKinds | unmetKinds);
    }

    // The vendors on the bot's map that trade with the bot (startup cache; no route to other maps yet): each one an
    // upkeep candidate, the nearest one for the destroy rule, the nearest VENDOR_STOPS within the errand walk limit as
    // the trip's service stops
    constexpr std::size_t VENDOR_STOPS = 8;
    uint32 const mapId = bot->GetMapId();
    float const runSpeed = bot->GetSpeed(MOVE_RUN);
    float const reachYards = WorldTravel::ERRAND_MAX_WALK_MINUTES * MINUTE * runSpeed;
    std::vector<TravelMgr::VendorInfo> const& vendors = TravelMgr::instance().GetVendors(mapId);
    std::vector<UpkeepNeeds::VendorCandidate> candidates;
    std::vector<TravelMgr::VendorInfo const*> candidateInfo;
    TravelMgr::VendorInfo const* nearest = nullptr;
    float nearestYards = std::numeric_limits<float>::max();
    std::vector<std::pair<float, TravelMgr::VendorInfo const*>> inReach;
    uint32 refused = 0;
    for (TravelMgr::VendorInfo const& vendor : vendors)
    {
        // The core's interact test (Player::GetNPCIfCanInteractWith): the vendor's reaction must be above Unfriendly.
        // A template without a reputation faction is judged by the template relation, not refused as Neutral.
        if (bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(vendor.factionTemplate), bot) <= REP_UNFRIENDLY)
        {
            ++refused;
            continue;
        }

        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(vendor.entry);
        float const yards =
            bot->GetExactDist(vendor.loc.GetPositionX(), vendor.loc.GetPositionY(), vendor.loc.GetPositionZ());
        auto const covered = cover.find(vendor.entry);
        candidates.push_back({vendor.entry, WorldTravel::WalkMinutes(yards, runSpeed),
                              proto && (proto->npcflag & UNIT_NPC_FLAG_REPAIR),
                              covered != cover.end() ? covered->second : uint8(0)});
        candidateInfo.push_back(&vendor);
        if (yards < nearestYards)
        {
            nearest = &vendor;
            nearestYards = yards;
        }
        if (yards <= reachYards)
            inReach.emplace_back(yards, &vendor);
    }

    // The repair need: the repairers within the walk limit, the one covering the most stock needs first; a due stock:
    // the vendor within the walk limit covering the most due stock. Both are worth a trip by themselves; else the
    // trip planner weighs the bags and the other errands
    float constexpr ANY_WALK = std::numeric_limits<float>::max();
    std::size_t pick = UpkeepNeeds::NO_VENDOR;
    bool repairing = repairWanted;
    if (repairing)
    {
        std::size_t const nearestRepairer = UpkeepNeeds::PickVendor(candidates, true, 0, 0, ANY_WALK);
        if (nearestRepairer == UpkeepNeeds::NO_VENDOR)
        {
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "no_repairer", 0.0f);
            repairing = false;
        }
        else if (candidates[nearestRepairer].walkMinutes > WorldTravel::ERRAND_MAX_WALK_MINUTES)
        {
            // Too far for a repair trip: the plain vendor choice, as for a bot without a repairer
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "too_far",
                               static_cast<float>(Planner::PriceRepair(bot).total));
            repairing = false;
        }
        else
            pick =
                UpkeepNeeds::PickVendor(candidates, true, dueKinds, unmetKinds, WorldTravel::ERRAND_MAX_WALK_MINUTES);
    }

    bool stocking = false;
    if (!repairing && dueKinds)
    {
        pick = UpkeepNeeds::PickVendor(candidates, false, dueKinds, unmetKinds, WorldTravel::ERRAND_MAX_WALK_MINUTES);
        stocking = pick != UpkeepNeeds::NO_VENDOR && (candidates[pick].covers & dueKinds);
        if (!stocking)
        {
            // Once per errand check: each due stock no vendor in reach sells, too far or on no vendor of this map
            pick = UpkeepNeeds::NO_VENDOR;
            for (UpkeepNeeds::Need const& need : needs.needs)
            {
                if (!need.due || !UpkeepNeeds::StockPayable(need))
                    continue;
                uint8 const bit = Planner::StockBit(need.stock);
                bool const onMap = std::any_of(candidates.begin(), candidates.end(),
                                               [bit](UpkeepNeeds::VendorCandidate const& c) { return c.covers & bit; });
                WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", onMap ? "too_far" : "no_vendor",
                                   static_cast<float>(need.copper),
                                   {{UpkeepNeeds::ShopWord(need.stock), static_cast<float>(need.units)},
                                    {"funded", static_cast<float>(need.funded)}},
                                   need.item);
            }
            // The due stock was the only cause: no vendor in reach sells it for the gold, so no trip (it would buy
            // nothing and repeat after the retry time)
            if (!bagsFull && !repairWanted)
                return false;
        }
    }

    if (!nearest)
    {
        LOG_DEBUG("playerbots", "[New RPG] {} errands: no vendor on map {} ({} vendors, {} refused by faction)",
                  bot->GetName(), mapId, vendors.size(), refused);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "skip", "no_vendor", 0.0f);
        return false;
    }

    float const walkMinutes = WorldTravel::WalkMinutes(nearestYards, runSpeed);
    measure.HasVendor = true;
    measure.VendorMapId = mapId;
    measure.VendorX = nearest->loc.GetPositionX();
    measure.VendorY = nearest->loc.GetPositionY();
    measure.VendorZ = nearest->loc.GetPositionZ();

    // The stacks the sell action would take from the general slots (as the destroy rule counts them)
    uint32 sellableSlots = 0;
    for (ItemUsage const usage : {ITEM_USAGE_VENDOR, ITEM_USAGE_AH})
        for (Item* item :
             AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(static_cast<uint32>(usage))))
            if (WorldTravel::InGeneralSlot(bot, item))
                ++sellableSlots;
    uint32 const freeSlots = WorldTravel::FreeGeneralSlots(bot);

    // The repair need and a due stock are worth a trip by themselves within the walk limit, whatever the bags hold:
    // one stop at the vendor the pick chose (WU12 makes them the trip planner's errand providers)
    if (pick != UpkeepNeeds::NO_VENDOR && (repairing || stocking))
    {
        UpkeepNeeds::VendorCandidate const& chosen = candidates[pick];
        uint32 const repairCopper = repairing ? Planner::PriceRepair(bot).total : 0;
        // The world log's decision row: walk seconds to the vendor and what the need weighed
        WorldLogFormat::Alternatives weighed;
        if (WorldLog::Active() && repairing)
            weighed = {{"repair_copper", static_cast<float>(repairCopper)},
                       {"lowest_durability", static_cast<float>(Planner::ReadGearWear(bot).lowestPercent)},
                       {"sellable", static_cast<float>(sellableSlots)}};
        else if (WorldLog::Active())
            weighed = {{"due_kinds", static_cast<float>(std::popcount(static_cast<unsigned>(dueKinds)))},
                       {"covered", static_cast<float>(std::popcount(static_cast<unsigned>(chosen.covers)))},
                       {"sellable", static_cast<float>(sellableSlots)}};

        std::vector<NewRpgInfo::ErrandStop> stops(1);
        stops.front().pos = WorldPosition(candidateInfo[pick]->loc);
        stops.front().vendorEntry = chosen.entry;
        botAI->rpgInfo.ChangeToErrands(std::move(stops), bot->GetMoney());
        if (repairing)
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "repair", "due",
                               static_cast<float>(repairCopper), weighed);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "vendor", repairing ? "repair_due" : "stock_due",
                           chosen.walkMinutes * MINUTE, weighed);
        return true;
    }

    // Full bags alone do not end questing (the trigger let a questing bot through for a due need that the vendor pick
    // did not keep): a questing bot destroys by worth instead
    if (!WorldTravel::ErrandMayStart(botAI))
    {
        WorldLogFormat::Alternatives weighed;
        if (WorldLog::Active())
            weighed = {{"sellable", static_cast<float>(sellableSlots)}, {"free_slots", static_cast<float>(freeSlots)}};
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "skip", "questing", walkMinutes * MINUTE,
                           weighed);
        return false;
    }

    // The trip planner's view of the bot (ErrandRules.h): what each errand is worth now and where it can be done
    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    WorldTravel::SlotBand const& band = config.SlotBands[WorldTravel::LevelBand(bot->GetLevel())];
    WorldTravel::ErrandSnapshot snapshot;
    snapshot.Level = bot->GetLevel();
    snapshot.Money = bot->GetMoney();
    snapshot.Rates = WorldTravel::CurrentRates(measure, band);
    snapshot.BagHorizonHours = band.BagHorizonHours;
    snapshot.FreeSlots = freeSlots;
    snapshot.SellableSlots = sellableSlots;
    snapshot.DurabilityPercent = EquippedDurabilityPercent(bot);
    snapshot.MinutesToNextStop = static_cast<float>(config.ErrandNextStopMinutes);
    std::sort(inReach.begin(), inReach.end(),
              [](std::pair<float, TravelMgr::VendorInfo const*> const& a,
                 std::pair<float, TravelMgr::VendorInfo const*> const& b) { return a.first < b.first; });
    if (inReach.size() > VENDOR_STOPS)
        inReach.resize(VENDOR_STOPS);
    for (std::pair<float, TravelMgr::VendorInfo const*> const& vendor : inReach)
    {
        WorldTravel::ServiceStop stop;
        stop.Id = vendor.second->entry;
        stop.Place = ErrandPlaceOf(vendor.second->loc);
        stop.Services = WorldTravel::SERVICE_VENDOR;
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(stop.Id);
        if (creature && (creature->npcflag & UNIT_NPC_FLAG_REPAIR))
            stop.Services |= WorldTravel::SERVICE_REPAIR;
        snapshot.Stops.push_back(stop);
    }
    AddCompletedQuests(bot, snapshot.Completed);

    // No stop the bot makes anyway yet (WT16's goals bring the route): every stop is a detour that must pay
    WorldTravel::TripRequest request;
    request.Start = {mapId, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    request.YardsPerSecond = runSpeed;
    request.Rates = snapshot.Rates;
    WorldTravel::TripPlan const plan = WorldTravel::PlanTrip(
        request, snapshot.Stops, WorldTravel::CollectOffers(snapshot, WorldTravel::TravelProviders()));

    std::vector<NewRpgInfo::ErrandStop> stops;
    float tripMinutes = 0.0f;
    std::size_t errandCount = 0;
    bool anyVendor = false;
    for (WorldTravel::TripStop const& tripStop : plan.Stops)
    {
        NewRpgInfo::ErrandStop stop;
        stop.pos = WorldPosition(tripStop.Place.MapId, tripStop.Place.X, tripStop.Place.Y, tripStop.Place.Z);
        if (tripStop.Services & WorldTravel::SERVICE_VENDOR)
        {
            stop.vendorEntry = tripStop.Id;
            anyVendor = true;
        }
        for (WorldTravel::ErrandOffer const& errand : tripStop.Errands)
            if (errand.Kind == WorldTravel::ErrandKind::TurnIn)
                stop.turnIns.push_back({errand.Id, errand.Target, errand.TargetIsObject});
        tripMinutes += tripStop.DetourMinutes;
        errandCount += tripStop.Errands.size();
        stops.push_back(std::move(stop));
    }

    // The world log's decision row: the sellable stacks and free slots, the trip's stops and errands it weighed
    WorldLogFormat::Alternatives weighed;
    if (WorldLog::Active())
        weighed = {{"sellable", static_cast<float>(sellableSlots)},
                   {"free_slots", static_cast<float>(snapshot.FreeSlots)},
                   {"stops", static_cast<float>(stops.size())},
                   {"errands", static_cast<float>(errandCount)}};
    if (stops.empty())
    {
        LOG_DEBUG("playerbots",
                  "[New RPG] {} errands: trip not worth it (nearest vendor {} at {:.0f} yd, {:.1f} walk min, {} "
                  "sellable, {} free slots, {} errands refused)",
                  bot->GetName(), nearest->entry, nearestYards, walkMinutes, sellableSlots, snapshot.FreeSlots,
                  plan.Refused.size());
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "skip", "not_worth", walkMinutes * MINUTE,
                           weighed);
        return false;
    }

    botAI->rpgInfo.ChangeToErrands(std::move(stops), bot->GetMoney());
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, anyVendor ? "vendor" : "turn_in", "worth_it",
                       tripMinutes * MINUTE, weighed);
    // The errands the trip leaves out: the detour rule's refusals (detour_not_worth, too_far, no_stop)
    if (WorldLog::Active())
        for (WorldTravel::Refusal const& refusal : plan.Refused)
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand,
                               WorldTravel::ErrandKindName(refusal.Offer.Kind), refusal.Reason,
                               refusal.WalkMinutes * MINUTE,
                               {{"worth", refusal.Offer.WorthCopper}, {"cost", refusal.CostCopper}});
    return true;
}

bool NewRpgErrandsAction::Execute(Event /*event*/)
{
    auto* data = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data);
    if (!data)
        return false;

    if (data->stop >= data->stops.size())
    {
        FinishErrands(botAI, data->visited);
        return true;
    }

    NewRpgInfo::ErrandStop& stop = data->stops[data->stop];
    if (!data->arrived)
    {
        if (bot->GetExactDist(stop.pos) > VENDOR_SEARCH_YARDS)
        {
            if (MoveFarTo(stop.pos))
                return true;
            return MoveRandomNear(10.0f);
        }
        data->arrived = true;
    }

    // The vendor part of the stop; a vendor that is gone leaves the stop's other errands
    if (stop.vendorEntry && !data->vendorDone)
    {
        if (data->vendor.IsEmpty())
        {
            Creature* found = bot->FindNearestCreature(stop.vendorEntry, VENDOR_SEARCH_YARDS);
            if (!found)
            {
                data->vendorDone = true;
                return true;
            }
            data->vendor = found->GetGUID();
        }

        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data->vendor);
        Creature* vendor = object ? object->ToCreature() : nullptr;
        if (!vendor || !vendor->IsAlive())
        {
            data->vendorDone = true;
            return true;
        }

        if (!IsWithinInteractionDist(vendor))
        {
            if (MoveWorldObjectTo(data->vendor))
                return true;
            return MoveRandomNear(15.0f);
        }

        // The vendor visit of the npc wander: the use measure before anything changes the bags, then sell by worth,
        // repair, a persistent bot's shopping list (upkeep §3.3: repair first, then the stock in its order) and the
        // generic buy (gear, a bag by worth; consumables and a hunter's ammo only for a bot granted them), all paid
        constexpr uint32 MIN_VENDOR_BALANCE = 1000;  // 10 silver
        bool const shopping = RandomPlayerbotMgr::HonestTravel() && sRandomPlayerbotMgr.IsRandomBot(bot) &&
                              !IsRealPlayer(botAI->GetMaster());
        WorldTravel::SlotMeasure& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
        if (shopping)
            UpkeepNeeds::SampleUse(measure, StockLevelsOf(bot), bot->getClass(), bot->GetLevel(), getMSTime());
        botAI->DoSpecificAction("sell", Event("sell", "vendor"), true);
        // A bag bought earlier may lie inside the bag it should replace, where the swap always fails
        if (WorldTravel::MoveSpareBagToBackpack(bot, ItemUsageValue::SmallestBagSize(bot)))
            botAI->DoSpecificAction("equip upgrades packet action");

        // "can repair" wants the whole repair within the money kept for it; gear due for repair gets what the gold
        // pays
        bool repaired = false;
        if (vendor->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) &&
            (AI_VALUE(bool, "can repair") || WorldTravel::RepairErrandWanted(bot)))
        {
            botAI->DoSpecificAction("repair", Event("repair"), true);
            repaired = true;
        }

        if (shopping)
            ShopAtVendor(botAI, vendor, repaired);

        if (bot->GetMoney() >= MIN_VENDOR_BALANCE)
            botAI->DoSpecificAction("buy", Event("buy", "vendor"), true);

        if (shopping)
            UpkeepNeeds::NoteStock(measure, StockLevelsOf(bot), getMSTime());
        data->vendorDone = true;
        data->visited = true;
        return true;
    }

    // The completed quests whose ender stands at the stop: the quest talk turns in every quest the ender takes and
    // accepts the fitting ones it offers (WT.6). An ender out of reach or a quest no longer complete is left out
    while (!stop.turnIns.empty())
    {
        NewRpgInfo::ErrandTurnIn const turnIn = stop.turnIns.back();
        if (bot->GetQuestStatus(turnIn.questId) != QUEST_STATUS_COMPLETE)
        {
            stop.turnIns.pop_back();
            continue;
        }

        float const searchYards = WorldTravel::STOP_JOIN_YARDS + VENDOR_SEARCH_YARDS;
        WorldObject* ender = nullptr;
        if (turnIn.enderIsObject)
            ender = bot->FindNearestGameObject(turnIn.ender, searchYards);
        else
            ender = bot->FindNearestCreature(turnIn.ender, searchYards);
        if (!ender)
        {
            stop.turnIns.pop_back();
            continue;
        }

        if (!IsWithinInteractionDist(ender))
        {
            if (MoveWorldObjectTo(ender->GetGUID()))
                return true;
            stop.turnIns.pop_back();
            continue;
        }

        if (bot->CanInteractWithQuestGiver(ender))
        {
            InteractWithNpcOrGameObjectForQuest(ender->GetGUID());
            data->visited = true;
        }
        stop.turnIns.pop_back();
        return true;
    }

    // The stop is done: on to the next
    ++data->stop;
    data->arrived = false;
    data->vendorDone = false;
    data->vendor = ObjectGuid();
    return true;
}

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
    WorldLog::StatusReason const reasonScope("finished");
    info.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        bool completed = true;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // can't find a poi pos to go, stop doing quest for now
            WorldLog::StatusReason const reasonScope("no_poi");
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // A planner bot goes to the point nearest its plan's tasks for the quest, others to a random one
        std::vector<std::pair<float, float>> points;
        points.reserve(poiInfo.size());
        for (POIInfo const& poi : poiInfo)
            points.emplace_back(poi.pos.x, poi.pos.y);
        int const planned = Planner::PlannedPoi(botAI, questId, points);
        uint32 rndIdx = planned >= 0 ? static_cast<uint32>(planned) : urand(0, poiInfo.size() - 1);
        G3D::Vector2 nearestPoi = poiInfo[rndIdx].pos;
        int32 objectiveIdx = poiInfo[rndIdx].objectiveIdx;

        float dx = nearestPoi.x, dy = nearestPoi.y;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate — nudge the
        // bot a short distance so the next tick retries from a
        // different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // kill mobs and looting quest should be done automatically by grind strategy

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
            WorldLog::StatusReason const reasonScope("abandon");
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
    }

    // At the POI: keep the bot actively placed but avoid large
    // random 20yd hops that look like pacing back and forth. A small
    // ~8yd wander reads as the bot looking around while grind/loot
    // strategies do their work.
    return MoveRandomNear(8.0f);
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
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
            WorldLog::StatusReason const reasonScope("no_poi");
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        assert(poiInfo.size() > 0);
        // now we get the place to get rewarded
        float dx = poiInfo[0].pos.x, dy = poiInfo[0].pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
        return false;

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
        WorldLog::StatusReason const reasonScope("abandon");
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
}

namespace
{
// A route exists only once the graph is ready (the route service answers only then), and it never changes after
TravelRoute::Point GraphNodeAt(std::uint32_t index)
{
    TravelGraphNode const& node = TravelGraph::Instance().GetNode(index);
    return {node.mapId, node.x, node.y, node.z};
}

// A walk leg's end (WT.23): its trip decision row, walk_leg when done or walk_leg_failed (how the bot went: foot, part
// or mounted; its seconds in combat; its seconds against the estimate), and in the debug log its speed: its path
// through the waypoints, its straight line, the time taken
void LogWalkLeg(Player* bot, NewRpgInfo::Travel const& data, uint32 index, TravelLegs::LegRecord const& record,
                uint32 nowMs, uint32 elapsedMs, char const* how, bool done)
{
    TravelRoute::Leg const& leg = data.trip.legs[index];
    float const seconds = static_cast<float>(elapsedMs) / 1000.0f;
    float const combat = TravelLegs::CombatSeconds(record, nowMs);
    WorldLog::Decision(
        bot, WorldLogFormat::DecisionKind::Trip, done ? "walk_leg" : "walk_leg_failed", TravelLegs::MountWord(record),
        combat, {{"seconds", seconds}, {"estimate", leg.seconds}, {"leg", static_cast<float>(index + 1)}}, data.tripId);
    if (!sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    std::vector<TravelLegs::Waypoint> const points = TravelLegs::Waypoints(leg, GraphNodeAt);
    float path = 0.0f;
    for (size_t i = 1; i < points.size(); ++i)
        path += TravelRoute::Distance(points[i - 1].at, points[i].at);
    TravelRoute::Point const at{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    LOG_DEBUG("playerbots",
              "[Route] {} walk leg {}/{} {}: {:.0f} yd straight, {:.0f} yd path in {:.0f} s ({:.1f} yd/s), estimate "
              "{:.0f} s, {:.0f} yd from its end, {} ({} of {} ticks mounted), {:.0f} s in combat",
              bot->GetName(), index + 1, data.trip.legs.size(), how, TravelRoute::Distance(leg.from, leg.to), path,
              seconds, seconds > 0.0f ? path / seconds : 0.0f, leg.seconds, TravelRoute::Distance(at, leg.to),
              TravelLegs::MountWord(record), record.mountedTicks, record.ticks, combat);
}

// The world log's route decision (only when it is on): the leg chain without no-op walks, why it was searched,
// its seconds; weighed against a straight walk at run speed (when the chain is not one walk on the same map) and its
// leg count. The route service returns one route, so no other route is searched for the row.
void LogRouteDecision(Player* bot, NewRpgInfo::Travel const& data, TravelRoute::Result const& route)
{
    WorldLogFormat::WordChain chain;
    uint32 legs = 0;
    bool walksOnly = true;
    for (TravelRoute::Leg const& leg : route.legs)
    {
        if (TravelLegs::IsNoOp(leg))
            continue;
        chain.Add(TravelRoute::LegTypeWord(leg.type));
        ++legs;
        walksOnly = walksOnly && leg.type == TravelRoute::LegType::Walk;
    }
    WorldLogFormat::Alternatives alternatives;
    if (!(legs == 1 && walksOnly) && bot->GetMapId() == data.dest.GetMapId())
    {
        float const yards =
            bot->GetExactDist(data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ());
        alternatives.Add({"walk_direct", WorldLogFormat::WalkSeconds(yards, bot->GetSpeed(MOVE_RUN))});
    }
    alternatives.Add({"legs", static_cast<float>(legs)});
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Route, legs ? chain.View() : "here",
                       data.trip.replans ? "replan" : "search", route.totalSeconds, alternatives, data.tripId);
}
}  // namespace

bool NewRpgTravelAction::Execute(Event /*event*/)
{
    auto* data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (!data)
        return false;
    if (TravelLegs::PastDeadline(GetMSTimeDiffToNow(data->routeStartMs), data->routeSeconds, bot->IsInFlight()))
    {
        EndTrip(*data, "deadline", "abort:deadline");
        return true;
    }
    // Killed twice on the leg or three times on the trip (PlayerbotAI::ChangeEngine counts it, the follower runs
    // again once the bot is back): the leg fails, its edge is avoided; the third death ends the trip
    if (TravelLegs::DiedTooOften(data->trip))
        return LegFailed(*data, TravelLegs::FailReason::Death);
    if (data->route.waiting)
        return TakeRoute(*data);
    if (data->trip.legs.empty())
    {
        // The trip's start, or a new route once the failed leg's stuck report has reached the published book
        if (data->failedMs && GetMSTimeDiffToNow(data->failedMs) < TravelLegs::REPLAN_DELAY_MS)
            return true;
        return RequestRoute(*data);
    }
    // MoveFarTo found the bot off the mesh and started its recovery (hearth or a short move): the trip ends
    if (data->offMesh)
        return LegFailed(*data, TravelLegs::FailReason::OffMesh);

    TravelLegs::Observation seen;
    seen.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    seen.inFlight = bot->IsInFlight();
    seen.money = bot->GetMoney();
    seen.stuck = data->stuck;
    seen.nowMs = getMSTime();
    TravelLegs::Trip const& trip = data->trip;
    TravelLegs::NoteTick(data->trip, bot->IsMounted());
    TravelLegs::LegRecord const recordBefore = trip.record;  // Advance starts a new leg's record
    uint32 const legBefore = trip.legIdx;
    uint32 const legStartBefore = trip.legStartMs;
    TravelLegs::Decision const next = TravelLegs::Advance(data->trip, seen, GraphNodeAt);
    // The legs passed this tick (no-op walks are no legs to a reader): id the trip, amount the leg's seconds
    for (uint32 i = legBefore; i < trip.legIdx && i < trip.legs.size(); ++i)
    {
        if (TravelLegs::IsNoOp(trip.legs[i]))
            continue;
        uint32 const seconds = i == legBefore ? (seen.nowMs - legStartBefore) / IN_MILLISECONDS : 0;
        EventLog::Emit(bot, "Travel", TravelLegs::LegDoneSource(trip.legs[i].type), data->tripId, seconds);
        if (i == legBefore && trip.legs[i].type == TravelRoute::LegType::Walk)
            LogWalkLeg(bot, *data, i, recordBefore, seen.nowMs, seen.nowMs - legStartBefore, "done", true);
    }
    switch (next.act)
    {
        case TravelLegs::Act::Arrived:
            return Arrive(*data);
        case TravelLegs::Act::Walk:
            return MoveFarTo(WorldPosition(next.target.mapId, next.target.x, next.target.y, next.target.z));
        case TravelLegs::Act::UseTrigger:
            return UsePortal(*data);
        case TravelLegs::Act::TalkFlightMaster:
            return TalkFlightMaster(*data);
        case TravelLegs::Act::UseHearth:
            return UseHearth(*data);
        case TravelLegs::Act::Ride:
            // As the random flight status does: nothing else to do in the air
            ContinueCrossMapTaxi();
            return false;
        case TravelLegs::Act::Fail:
            return LegFailed(*data, next.reason);
        case TravelLegs::Act::Wait:
        case TravelLegs::Act::Done:
            return true;
    }
    return false;
}

bool NewRpgTravelAction::RequestRoute(NewRpgInfo::Travel& data)
{
    TravelRoute::Snapshot snapshot = TravelRoute::FillSnapshot(bot, ++botAI->rpgInfo.routeRequests);
    // Transports are not run yet (WT19). Flights only to discovered nodes (the snapshot's taxi mask), their fares
    // within the purse (the search's fare budget). The hearthstone is a leg while its stone is in the bags and it is
    // ready or cooling for at most the wait cap (the search); not again after a hearth leg of this trip failed
    snapshot.hasHearthstone = snapshot.hasHearthstone && !data.noHearth;
    snapshot.allowTransport = false;
    TravelRoute::Request request;
    request.snapshot = snapshot;
    request.goal = {data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ()};
    // The trip's stuck walks: their start or goal hops are left out (TravelLegs::NoteStuck)
    request.avoidNodes = data.trip.avoid;
    request.noDirect = data.trip.noDirect;
    // The walks this bot died on twice (TravelLegs::NoteDeath): their graph edges are left out, for its trip only
    request.avoidEdges = data.trip.avoidEdges;
    if (!TravelRoute::RouteService::Instance().Submit(bot->GetGUID().GetRawValue(), request,
                                                      TravelRoute::ConfiguredParams()))
    {
        EndTrip(data, "no route service", "abort:no_service");  // the graph is not loaded yet: never wait
        return true;
    }
    data.route = {snapshot.requestId, snapshot.position.mapId, snapshot.takenMs, true};
    data.stuck = false;
    return true;
}

bool NewRpgTravelAction::TakeRoute(NewRpgInfo::Travel& data)
{
    uint32 const now = getMSTime();
    std::optional<TravelRoute::Answer> answer =
        TravelRoute::RouteService::Instance().TryTake(bot->GetGUID().GetRawValue());
    // A late or lost answer asks again from here, a few times per trip, before it fails the leg
    auto const askAgain = [&](char const* why)
    {
        if (!TravelLegs::Resubmit(data.trip))
            return LegFailed(data, TravelLegs::FailReason::NoAnswer);
        LOG_DEBUG("playerbots", "[Route] {} {} ({} ms since the request), asked again", bot->GetName(), why,
                  now - data.route.requestedMs);
        return RequestRoute(data);
    };
    if (!answer)
        return TravelLegs::WaitExpired(data.route, now) ? askAgain("got no answer") : true;
    switch (TravelLegs::Judge(data.route, answer->requestId, bot->GetMapId(), now))
    {
        case TravelLegs::Verdict::Drop:
            return true;  // an older request's answer: the latest is still on its way
        case TravelLegs::Verdict::Refresh:
            return askAgain(bot->GetMapId() != data.route.mapId ? "changed maps" : "took the answer too late");
        case TravelLegs::Verdict::Apply:
            break;
    }

    data.route.waiting = false;
    TravelRoute::Result& route = answer->result;
    if (!route.ok)
    {
        LOG_DEBUG("playerbots",
                  "[Route] {} has no route: {} from ({}, {:.0f}, {:.0f}, {:.0f}) to ({}, {:.0f}, {:.0f}, {:.0f}), "
                  "noHearth {}, replans {}",
                  bot->GetName(), TravelRoute::FailureWord(route.failure), bot->GetMapId(), bot->GetPositionX(),
                  bot->GetPositionY(), bot->GetPositionZ(), data.dest.GetMapId(), data.dest.GetPositionX(),
                  data.dest.GetPositionY(), data.dest.GetPositionZ(), data.noHearth, data.trip.replans);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Route, "none", TravelRoute::FailureWord(route.failure),
                           0.0f, {}, data.tripId);
        return LegFailed(data, TravelLegs::FailReason::NoRoute);
    }
    if (data.estimateSeconds <= 0.0f)
    {
        // The trip's clock starts at its start row, so the ends' seconds span the logged trip (the wait for the
        // first route is not counted; a trip that ends before this row counts from its pick)
        data.tripStartMs = now;
        data.estimateSeconds = route.totalSeconds;
        EventLog::Emit(bot, "Travel", "start", data.tripId, static_cast<int64>(route.totalSeconds));
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "start", data.bindInn ? "inn" : "order",
                           route.totalSeconds, {{"legs", static_cast<float>(route.legs.size())}}, data.tripId);
        if (!data.place.empty())
            LOG_INFO("playerbots", "[Route] {} travel to {}: {} legs, {:.0f} s", bot->GetName(), data.place,
                     route.legs.size(), route.totalSeconds);
    }
    if (WorldLog::Active())
        LogRouteDecision(bot, data, route);
    // The deadline runs against the current route (a replan's own estimate)
    data.routeStartMs = now;
    data.routeSeconds = route.totalSeconds;
    LOG_DEBUG("playerbots",
              "[Route] {} to ({}, {:.0f}, {:.0f}, {:.0f}): {} legs, {:.0f} s; taken {} ms after the request (in combat "
              "{}, active {})",
              bot->GetName(), data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(),
              data.dest.GetPositionZ(), route.legs.size(), route.totalSeconds, now - data.route.requestedMs,
              bot->IsInCombat(), botAI->AllowActivity());
    if (route.legs.empty())
        return Arrive(data);  // already there
    data.trip.legs = std::move(route.legs);
    TravelLegs::StartLeg(data.trip, 0, now, GraphNodeAt);
    return true;
}

bool NewRpgTravelAction::UsePortal(NewRpgInfo::Travel& data)
{
    uint32 const triggerId = data.trip.legs[data.trip.legIdx].object;
    AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
    if (!trigger || !sObjectMgr->GetAreaTriggerTeleport(triggerId) || trigger->map != bot->GetMapId())
        return LegFailed(data, TravelLegs::FailReason::Portal);
    if (!bot->IsInAreaTriggerRadius(trigger))
        return MoveFarTo(WorldPosition(trigger->map, trigger->x, trigger->y, trigger->z));

    // What a player's client sends on entering the trigger: the core's own handler checks it and teleports
    WorldPacket packet(CMSG_AREATRIGGER);
    packet << triggerId;
    packet.rpos(0);
    bot->GetSession()->HandleAreaTriggerOpcode(packet);
    data.trip.triggered = true;
    return true;
}

bool NewRpgTravelAction::TalkFlightMaster(NewRpgInfo::Travel& data)
{
    TravelRoute::Leg const& leg = data.trip.legs[data.trip.legIdx];
    if (leg.taxiNodes.size() < 2)
        return LegFailed(data, TravelLegs::FailReason::Taxi);
    // The master of the leg's first taxi node nearest the leg's start (startup cache, read only; scanned only on the
    // few ticks the bot stands at the master)
    TravelMgr::FlightMasterInfo const* master = nullptr;
    float nearest = TravelLegs::FLIGHT_MASTER_YARDS * 2.0f;
    for (auto const& [dbGuid, info] : sTravelMgr.GetFlightMasters(bot->GetTeamId()))
    {
        if (info.taxiNodeId != leg.taxiNodes.front() || info.pos.GetMapId() != leg.from.mapId)
            continue;
        float const yards = TravelRoute::Distance(
            {info.pos.GetMapId(), info.pos.GetPositionX(), info.pos.GetPositionY(), info.pos.GetPositionZ()}, leg.from);
        if (yards < nearest)
        {
            nearest = yards;
            master = &info;
        }
    }
    if (!master)
        return LegFailed(data, TravelLegs::FailReason::Taxi);

    switch (StartTaxi(master->templateEntry, master->pos, leg.taxiNodes))
    {
        case TaxiStart::Moving:
            return true;
        case TaxiStart::Stalled:
            return false;
        case TaxiStart::Started:
            data.trip.taxiActivated = true;
            return true;
        case TaxiStart::Refused:
        {
            // The core's fare differs from the snapshot's: a purse short of the chain's price fails as the fare
            uint32 cost = 0;
            for (size_t i = 0; i + 1 < leg.taxiNodes.size(); ++i)
            {
                uint32 path = 0;
                uint32 price = 0;
                sObjectMgr->GetTaxiPath(leg.taxiNodes[i], leg.taxiNodes[i + 1], path, price);
                cost += price;
            }
            return LegFailed(data,
                             bot->GetMoney() < cost ? TravelLegs::FailReason::Fare : TravelLegs::FailReason::Taxi);
        }
        case TaxiStart::NoMaster:
        case TaxiStart::Undiscovered:
            break;
    }
    return LegFailed(data, TravelLegs::FailReason::Taxi);
}

bool NewRpgTravelAction::UseHearth(NewRpgInfo::Travel& data)
{
    // The cast takes 10 s and a cooling stone is waited for (the route's seconds hold both; the leg's timeout bounds
    // them). The teleport is the stone's own spell: no bot teleport, no sight check
    if (bot->IsNonMeleeSpellCast(false) || bot->HasSpellCooldown(HEARTHSTONE_SPELL))
        return true;
    char const* refusal = nullptr;
    float castResult = 0.0f;
    if (!CastHearth(refusal, castResult))
    {
        // The world log's row names why (WorldTravel::HearthRefusalWord; cast_failed: the value is the core's result)
        LOG_DEBUG("playerbots", "[Route] {} hearth refused: {} (cast result {:.0f})", bot->GetName(),
                  refusal ? refusal : "?", castResult);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "hearth_refused", refusal ? refusal : "?",
                           castResult, {}, data.tripId);
        return LegFailed(data, TravelLegs::FailReason::Hearth);
    }
    return true;
}

bool NewRpgTravelAction::LegFailed(NewRpgInfo::Travel& data, TravelLegs::FailReason reason)
{
    TravelLegs::Trip& trip = data.trip;
    LOG_DEBUG("playerbots", "[Route] {} leg {}/{} failed: {}", bot->GetName(), trip.legIdx + 1, trip.legs.size(),
              TravelLegs::FailWord(reason));
    uint32 const legSeconds =
        trip.legIdx < trip.legs.size() ? GetMSTimeDiffToNow(trip.legStartMs) / IN_MILLISECONDS : 0;
    EventLog::Emit(bot, "Travel", TravelLegs::LegFailedSource(reason), data.tripId, legSeconds);
    // A failed walk's row before AfterFailure starts the next record
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Walk)
    {
        uint32 const now = getMSTime();
        LogWalkLeg(bot, data, trip.legIdx, trip.record, now, now - trip.legStartMs,
                   reason == TravelLegs::FailReason::Timeout ? "timed out" : TravelLegs::FailWord(reason), false);
    }
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Hearth)
        data.noHearth = true;
    // A portal that did not take the bot through, or a flight that could not be taken: the search avoids that edge
    // for a while (a stuck walk was reported by MoveFarTo's give-up already)
    if (reason == TravelLegs::FailReason::Portal || reason == TravelLegs::FailReason::Taxi)
    {
        auto const [from, to] =
            reason == TravelLegs::FailReason::Portal ? TravelLegs::StuckEdge(trip) : TravelLegs::FirstFlightHop(trip);
        if (from != TravelLegs::NO_NODE)
        {
            TravelRoute::StuckBook::Report report;
            report.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
            report.destination = report.bot;
            report.fromNode = from;
            report.toNode = to;
            TravelRoute::StuckBook::Instance().Push(report);
        }
    }

    if (TravelLegs::AfterFailure(trip, reason) == TravelLegs::Recovery::Abort)
    {
        EndTrip(data, TravelLegs::FailWord(reason), TravelLegs::AbortSource(reason));
        return true;
    }
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "replan", TravelLegs::FailWord(reason),
                       static_cast<float>(legSeconds), {{"leg", static_cast<float>(trip.legIdx + 1)}}, data.tripId);
    trip.legs.clear();
    trip.legIdx = 0;
    trip.waypoints.clear();
    data.route.waiting = false;
    data.stuck = false;
    data.failedMs = std::max<uint32>(getMSTime(), 1);  // 0 = no failed leg
    return true;
}

bool NewRpgTravelAction::Arrive(NewRpgInfo::Travel& data)
{
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    uint32 const replans = data.trip.replans;
    if (data.bindInn)
    {
        // The route ends in front of the innkeeper (InnInfo::loc). Walk until close enough to talk to him, then bind
        // directly (not through the "home" action, which needs a cleared target and a cached npc list); a failed bind
        // is tried again a few times, a stuck bot out of reach gives up
        float const yards =
            bot->GetExactDist(data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ());
        WorldTravel::InnStep const step = WorldTravel::InnNext(yards, data.stuck, data.bindTries);
        if (step == WorldTravel::InnStep::Walk)
            return MoveFarTo(data.dest);
        bool bound = false;
        char const* why = "stuck out of reach";
        if (step == WorldTravel::InnStep::Bind)
        {
            ++data.bindTries;
            why = "no innkeeper in reach";
            std::list<Unit*> units;
            Acore::AnyUnitInObjectRangeCheck check(bot, WorldTravel::INN_REACH_YARDS);
            Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
            Cell::VisitObjects(bot, searcher, WorldTravel::INN_REACH_YARDS);
            for (Unit* unit : units)
            {
                if (!unit->IsCreature() || !unit->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
                    continue;
                Creature* innkeeper = bot->GetNPCIfCanInteractWith(unit->GetGUID(), UNIT_NPC_FLAG_INNKEEPER);
                if (!innkeeper)
                {
                    why = "innkeeper not interactable";
                    continue;
                }
                bot->GetSession()->SendBindPoint(innkeeper);
                bound = true;
                break;
            }
            if (!bound && data.bindTries < WorldTravel::INN_BIND_TRIES)
            {
                LOG_DEBUG("playerbots", "[Route] {} bind try {} failed ({}), {:.1f} yd from the inn spot",
                          bot->GetName(), static_cast<uint32>(data.bindTries), why, yards);
                return true;
            }
        }
        LOG_INFO("playerbots", "[Route] {} {} at the inn after {} s ({} replans, {} binds, {:.1f} yd{}{})",
                 bot->GetName(), bound ? "bound its hearthstone" : "could not bind", seconds, replans,
                 static_cast<uint32>(data.bindTries), yards, bound ? "" : ": ", bound ? "" : why);
        EventLog::Emit(bot, "Travel", bound ? "inn_bound" : "inn_failed", data.tripId, seconds);
        // A spot where the bot stood but found no usable innkeeper (phased out, dead, hostile) is avoided by its next
        // inn errands for a while (WorldTravel::InnAvoided); a bot stuck out of reach blames no spot
        bool const noInnkeeper = std::string_view(why) == "no innkeeper in reach";
        if (!bound && (noInnkeeper || std::string_view(why) == "innkeeper not interactable"))
        {
            WorldTravel::AvoidInn(
                botAI->rpgInfo.innAvoid, getMSTime(),
                {data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ()});
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "avoid",
                               noInnkeeper ? "no_innkeeper" : "not_interactable", yards, {}, data.tripId);
        }
    }
    if (data.place.empty())
        LOG_DEBUG("playerbots", "[Route] {} arrived after {} s (estimate {:.0f} s, {} replans)", bot->GetName(),
                  seconds, data.estimateSeconds, replans);
    else
        LOG_INFO("playerbots", "[Route] {} arrived at {} after {} s (estimate {:.0f} s, {} replans)", bot->GetName(),
                 data.place, seconds, data.estimateSeconds, replans);
    EventLog::Emit(bot, "Travel", "arrived", data.tripId, seconds);
    WorldLog::StatusReason const reasonScope("arrived");
    botAI->rpgInfo.ChangeToIdle();
    return true;
}

void NewRpgTravelAction::EndTrip(NewRpgInfo::Travel const& data, char const* why, char const* source)
{
    GiveTripUp(botAI, data, why, source);
}

bool NewRpgStartInnAction::Execute(Event /*event*/)
{
    WorldLog::StatusReason const reasonScope("inn");
    if (!WorldTravel::ErrandMayStart(botAI))
        return false;

    // Checked at most every INN_RETRY_MS, whatever comes of it
    NewRpgInfo& info = botAI->rpgInfo;
    info.lastInnMs = std::max<uint32>(getMSTime(), 1);

    // The bot's team's inns on its map (startup cache): the nearest one whose zone fits its level, within 10 minutes of
    // walking (straight line at run speed)
    std::vector<TravelMgr::InnInfo> const& inns = sTravelMgr.GetInns(bot->GetTeamId());
    std::vector<WorldTravel::InnCandidate> candidates;
    std::vector<size_t> innIndex;
    uint32 const level = bot->GetLevel();
    for (size_t i = 0; i < inns.size(); ++i)
    {
        TravelMgr::InnInfo const& inn = inns[i];
        if (inn.loc.GetMapId() != bot->GetMapId())
            continue;
        // A spot whose bind found no usable innkeeper lately (Arrive)
        if (!info.innAvoid.empty() && WorldTravel::InnAvoided(info.innAvoid, getMSTime(),
                                                              {inn.loc.GetMapId(), inn.loc.GetPositionX(),
                                                               inn.loc.GetPositionY(), inn.loc.GetPositionZ()}))
            continue;

        StartingHistory::Bracket bracket;
        bool const fits = sTravelMgr.GetZoneBracket(inn.zoneId, bracket.low, bracket.high) &&
                          StartingHistory::BracketFits(level, bracket);
        candidates.push_back(
            {bot->GetExactDist(inn.loc.GetPositionX(), inn.loc.GetPositionY(), inn.loc.GetPositionZ()), fits});
        innIndex.push_back(i);
    }
    float const runSpeed = bot->GetSpeed(MOVE_RUN);
    float const maxYards = WorldTravel::INN_MAX_WALK_SECONDS * std::max(runSpeed, 1.0f);
    int const pick = WorldTravel::PickInn(candidates, maxYards);
    // The world log's decision row: the three nearest other inns in walk seconds (inn_unfit: not for the bot's level)
    WorldLogFormat::Alternatives others;
    if (WorldLog::Active())
        for (size_t i = 0; i < candidates.size(); ++i)
            if (static_cast<int>(i) != pick)
                others.Offer({candidates[i].fits ? "inn" : "inn_unfit",
                              WorldLogFormat::WalkSeconds(candidates[i].yards, runSpeed)},
                             true);
    if (pick < 0)
    {
        LOG_DEBUG("playerbots", "[Route] {} inn errand: no fitting inn within reach ({} inns on the map)",
                  bot->GetName(), candidates.size());
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "none", "no_fitting_inn", 0.0f, others);
        return false;
    }

    TravelMgr::InnInfo const& inn = inns[innIndex[static_cast<size_t>(pick)]];
    LOG_DEBUG("playerbots", "[Route] {} inn errand: bind far, inn at ({:.0f}, {:.0f}) in zone {}", bot->GetName(),
              inn.loc.GetPositionX(), inn.loc.GetPositionY(), inn.zoneId);
    info.ChangeToTravel(WorldPosition(inn.loc));
    uint32 tripId = 0;
    if (auto* travel = std::get_if<NewRpgInfo::Travel>(&info.data))
    {
        travel->bindInn = true;
        tripId = travel->tripId;
    }
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "bind_far", "inn_picked",
                       WorldLogFormat::WalkSeconds(candidates[static_cast<size_t>(pick)].yards, runSpeed), others,
                       tripId);
    return true;
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        ContinueCrossMapTaxi();
        return false;
    }

    switch (StartTaxi(data.flightMasterEntry, data.flightMasterPos, data.path))
    {
        case TaxiStart::Moving:
        case TaxiStart::Started:
            return true;
        case TaxiStart::Stalled:
            return false;
        case TaxiStart::NoMaster:
        case TaxiStart::Undiscovered:
        case TaxiStart::Refused:
            break;
    }
    WorldLog::StatusReason const reasonScope("failed");
    info.ChangeToIdle();
    return true;
}

NewRpgBaseAction::TaxiStart NewRpgBaseAction::StartTaxi(uint32 masterEntry, WorldPosition const& masterPos,
                                                        std::vector<uint32> const& nodes)
{
    if (bot->GetDistance(masterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(masterPos) ? TaxiStart::Moving : TaxiStart::Stalled;

    Creature* flightMaster = bot->FindNearestCreature(masterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
        return TaxiStart::NoMaster;
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster) ? TaxiStart::Moving : TaxiStart::Stalled;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    // Learning the master's own node is natural: a player learns it here too
    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    auto const known = [this](uint32 node) { return bot->m_taxi.IsTaximaskNodeKnown(node); };
    if (!StartingHistory::PathKnown(nodes, known))
    {
        // Persistent bots pick only discovered routes; other setups still fly and the flight is counted
        if (RandomPlayerbotMgr::HonestTravel() && sRandomPlayerbotMgr.IsRandomBot(bot))
            return TaxiStart::Undiscovered;

        EventLog::Honesty(bot, "Flight", "undiscovered_flight", nodes.empty() ? 0 : nodes.back());
    }

    // Pays the fare; fails when a node is not usable or the purse is short
    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes.empty() ? 0 : nodes.front(), nodes.empty() ? 0 : nodes.back());
        return TaxiStart::Refused;
    }
    return TaxiStart::Started;
}

void NewRpgBaseAction::ContinueCrossMapTaxi()
{
    if (bot->IsBeingTeleported())
        return;

    if (!bot->movespline->Finalized())
        return;

    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm || mm->GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE)
        return;

    // Check if we are at our destination.
    uint32 nextDest = bot->m_taxi.GetTaxiDestination();
    if (!nextDest)
        return;

    // Confirm next node needs different map.
    TaxiNodesEntry const* nextNode = sTaxiNodesStore.LookupEntry(nextDest);
    if (!nextNode || nextNode->map_id == bot->GetMapId())
        return;

    FlightPathMovementGenerator* flight = dynamic_cast<FlightPathMovementGenerator*>(mm->top());
    if (!flight)
        return;

    LOG_DEBUG("playerbots", "[New RPG] {} continuing taxi across map boundary (next node {} on map {})",
              bot->GetName(), nextDest, nextNode->map_id);

    flight->SetCurrentNodeAfterTeleport();

    if (flight->HasArrived())
        return;

    TaxiPathNodeEntry const* node = flight->GetPath()[flight->GetCurrentNode()];
    flight->SkipCurrentNode();

    bot->TeleportTo(nextNode->map_id, node->x, node->y, node->z, bot->GetOrientation(), TELE_TO_NOT_LEAVE_TAXI);
}
