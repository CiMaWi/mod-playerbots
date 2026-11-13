/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "AreaDefines.h"
#include "BroadcastHelper.h"
#include "CellImpl.h"
#include "CensusMgr.h"
#include "ChatHelper.h"
#include "DBCStores.h"
#include "ErrandRules.h"
#include "G3D/Vector2.h"
#include "GameObjectModel.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "IVMapMgr.h"
#include "InventorySlots.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
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
#include "RandomPlayerbotMgr.h"
#include "RouteService.h"
#include "SharedDefines.h"
#include "StartingHistoryRules.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "Transport.h"
#include "TravelGraph.h"
#include "TravelLegRules.h"
#include "TravelMgr.h"
#include "TravelTransportGeometry.h"
#include "WaypointMovementGenerator.h"
#include "WorldLog.h"
#include "WorldLogStatus.h"
#include "WorldPacket.h"
#include "WorldTravelRules.h"

namespace
{
// Ends the bot's errand: stamps the errand check, measures t from a visited vendor and records the run
void FinishErrands(PlayerbotAI* botAI, bool visited)
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
        EventLog::Emit(bot, "Inventory", "errand_abort", data->vendorEntry, int64(minutes));

    info.ChangeToIdle();
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
        botAI->CancelLootInteraction("order");
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
constexpr char const* CONSTRAINT_EVENT_TYPE = "Constraint";

std::mutex travelOrdersLock;
std::unordered_map<ObjectGuid, TravelOrder> travelOrders;  // guarded by travelOrdersLock

// nofight: creatures neither attack an immune-to-NPC unit nor are attacked by it (Unit::_IsValidAttackTarget); setting
// the flag ends the unit's PvE combat
void SetNoFight(Unit* unit, bool on)
{
    if (unit && unit->IsImmuneToNPC() != on)
        unit->SetImmuneToNPC(on);
}

void NoFight(Player& bot, bool on)
{
    SetNoFight(&bot, on);
    for (Unit* controlled : bot.m_Controlled)
        SetNoFight(controlled, on);
}

void ApplyNoFight(Player& bot) { NoFight(bot, true); }
void UndoNoFight(Player& bot) { NoFight(bot, false); }
void ApplyNoDamage(Player& bot) { bot.SetCommandStatusOn(CHEAT_GOD); }
void UndoNoDamage(Player& bot) { bot.SetCommandStatusOff(CHEAT_GOD); }

// The state rows' apply and undo (TestConstraints::ROWS)
constexpr TestConstraints::StateOps<Player> STATE_OPS[] = {
    {TestConstraints::NOFIGHT, ApplyNoFight, UndoNoFight},
    {TestConstraints::NODAMAGE, ApplyNoDamage, UndoNoDamage},
};
static_assert(TestConstraints::Covered(STATE_OPS) == TestConstraints::STATES, "every state row has apply and undo");

void PlaceAboard(PlayerbotAI* botAI, Player* bot, uint32 entry);  // beside the transport legs' deck helpers

// A GM order ends the bot's trip: the Travel event and the trip decision with the word (stopped, held)
void EndTripByOrder(Player* bot, NewRpgInfo& info, NewRpgInfo::Travel const& data, char const* word)
{
    LOG_INFO("playerbots", "[Route] {} gives the trip up: {}", bot->GetName(), word);
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    std::string const source = std::string("abort:") + word;
    EventLog::Emit(bot, "Travel", source.c_str(), data.tripId, seconds);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "abort", word, static_cast<float>(seconds),
                       {{"replans", static_cast<float>(data.trip.replans)}}, data.tripId);
    info.ChangeToIdle();
}

// A test's constraint change: the state rows applied or undone, one Constraint row whose amount is the ms from the
// command to the take (also on an idle bot). A choice row turning on ends the current status unless a trip comes with
// it, so the bot stands idle; the idle gate then keeps it there. The planner's next snapshot sees the hearth bit
void ApplyConstraints(Player* bot, NewRpgInfo& info, TravelOrder const& order, bool tripFollows)
{
    uint16 const before = info.constraints;
    uint16 const after = TestConstraints::Next(before, order.on, order.off);
    info.constraints = after;
    info.planner.snapshotDue = true;
    TestConstraints::ChangeStates(STATE_OPS, *bot, before, after);
    std::string const set = TestConstraints::Format(after);
    uint32 const latencyMs = GetMSTimeDiffToNow(order.postedMs);
    LOG_INFO("playerbots", "[Route] {} constraints: {} (taken after {} ms)", bot->GetName(), set, latencyMs);
    EventLog::Emit(bot, CONSTRAINT_EVENT_TYPE, set.c_str(), 0, latencyMs);
    if (!TestConstraints::EndsStatus(before, after) || tripFollows)
        return;
    WorldLog::StatusReason const reasonScope("hold");
    if (auto const* data = std::get_if<NewRpgInfo::Travel>(&info.data))
        EndTripByOrder(bot, info, *data, "held");
    else
        info.ChangeToIdle();
}

// A GM order taken on the bot's map thread: the state is checked again (the command saw it on the world thread)
bool ApplyTravelOrder(PlayerbotAI* botAI, Player* bot, TravelOrder const& order)
{
    NewRpgInfo& info = botAI->rpgInfo;
    bool const trip = TestConstraints::HasTrip(order);
    bool const constrained = order.on || order.off;
    if (constrained)
        ApplyConstraints(bot, info, order, trip);
    if (order.aboard)
        PlaceAboard(botAI, bot, order.aboard);
    bool const applied = constrained || order.aboard;
    if (!trip)
        return applied;
    WorldLog::StatusReason const reasonScope("order");
    if (order.stop)
    {
        auto const* data = std::get_if<NewRpgInfo::Travel>(&info.data);
        if (!data)
        {
            LOG_INFO("playerbots", "[Route] {} stop: not travelling", bot->GetName());
            return applied;
        }
        botAI->CancelLootInteraction("order");
        EndTripByOrder(bot, info, *data, "stopped");
        return true;
    }

    char const* refusal = nullptr;
    if (!bot->IsAlive())
        refusal = "dead";
    else if (bot->IsInFlight())
        refusal = "in flight";
    else if (bot->GetMap()->Instanceable())
        refusal = "in an instance";
    if (refusal)
    {
        LOG_INFO("playerbots", "[Route] {} travel to {} refused: {}", bot->GetName(), order.place, refusal);
        return applied;
    }
    LOG_INFO("playerbots", "[Route] {} travel to {} ({}, {:.0f}, {:.0f}, {:.0f}) ordered", bot->GetName(), order.place,
             order.dest.GetMapId(), order.dest.GetPositionX(), order.dest.GetPositionY(), order.dest.GetPositionZ());
    botAI->CancelLootInteraction("order");
    info.ChangeToTravel(order.dest, order.place);
    return true;
}

// The bot's order when it may take it now (TestConstraints::TakeNow); the bot's hint stays while its order waits.
// Orders nobody took in time are dropped on any take, and logged: the bot logged out or fought for a minute
bool TakeOrder(PlayerbotAI* botAI, ObjectGuid bot, bool inCombat, TravelOrder& order)
{
    std::lock_guard<std::mutex> guard(travelOrdersLock);
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
    bool const taken = found != travelOrders.end() && TestConstraints::TakeNow(found->second, inCombat);
    bool const waiting = found != travelOrders.end() && !taken;
    if (taken)
    {
        order = std::move(found->second);
        travelOrders.erase(found);
    }
    botAI->orderPosted.store(waiting, std::memory_order_release);
    return taken;
}
}  // namespace

void TravelOrders::Post(PlayerbotAI* botAI, TravelOrder order)
{
    std::lock_guard<std::mutex> guard(travelOrdersLock);
    // A hold and a trip posted before the bot's next update both stay (TestConstraints::Merge)
    auto const [it, added] = travelOrders.try_emplace(botAI->GetBot()->GetGUID(), order);
    if (!added)
        TestConstraints::Merge(it->second, std::move(order));
    botAI->orderPosted.store(true, std::memory_order_release);
}

bool TravelOrders::TakeAndApply(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI->orderPosted.load(std::memory_order_acquire))
        return false;
    TravelOrder order;
    if (!TakeOrder(botAI, bot->GetGUID(), bot->IsInCombat(), order))
        return false;
    if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
    {
        LOG_INFO("playerbots", "[Route] {} order '{}' refused: the bot does not run the new rpg strategy",
                 bot->GetName(), order.place);
        return false;
    }
    return ApplyTravelOrder(botAI, bot, order);
}

void TravelOrders::KeepConstraints(Player* bot, uint16 constraints)
{
    TestConstraints::KeepStates(STATE_OPS, *bot, constraints);
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    Planner::PlannerHook(botAI, bot);
    // GM orders were taken before the engine ran (TravelOrders::TakeAndApply in PlayerbotAI::UpdateAI)
    if (NewRpgTravelAction::ConsumeScheduledWork(botAI))
        return true;
    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
        {
            // A test's hold (TestConstraints): a held choice is not made; an order was taken all the same
            TestConstraints::IdleGate const gate = TestConstraints::GateOf(info.constraints);
            if (!gate.planner && !gate.status)
                return false;
            // A planner bot's goal (WT16); the weighted pick for the A/B group and as the counted fallback
            Planner::IdleStep step;
            if (gate.planner)
                step = Planner::OnIdle(botAI, bot,
                                       [this](uint32 questId)
                                       {
                                           std::vector<POIInfo> poiInfo;
                                           return !botAI->lowPriorityQuest.contains(questId) &&
                                                  GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true);
                                       });
            if (step.started)
                return true;
            if (step.wait || !gate.status)
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
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
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
    WorldTravel::StampErrandCheck(measure, getMSTime());
    measure.HasVendor = false;

    // The vendors on the bot's map that trade with the bot (startup cache; no route to other maps yet): the nearest
    // one for the destroy rule, the nearest VENDOR_STOPS within the errand walk limit as the trip's service stops
    constexpr std::size_t VENDOR_STOPS = 8;
    uint32 const mapId = bot->GetMapId();
    float const runSpeed = bot->GetSpeed(MOVE_RUN);
    float const reachYards = WorldTravel::ERRAND_MAX_WALK_MINUTES * MINUTE * runSpeed;
    std::vector<TravelMgr::VendorInfo> const& vendors = TravelMgr::instance().GetVendors(mapId);
    TravelMgr::VendorInfo const* nearest = nullptr;
    float nearestYards = std::numeric_limits<float>::max();
    std::vector<std::pair<float, TravelMgr::VendorInfo const*>> inReach;
    uint32 refused = 0;
    for (TravelMgr::VendorInfo const& vendor : vendors)
    {
        float const yards =
            bot->GetExactDist(vendor.loc.GetPositionX(), vendor.loc.GetPositionY(), vendor.loc.GetPositionZ());
        if (yards >= nearestYards && yards > reachYards)
            continue;

        // The core's interact test (Player::GetNPCIfCanInteractWith): the vendor's reaction must be above Unfriendly.
        // A template without a reputation faction is judged by the template relation, not refused as Neutral.
        if (bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(vendor.factionTemplate), bot) <= REP_UNFRIENDLY)
        {
            ++refused;
            continue;
        }

        if (yards < nearestYards)
        {
            nearest = &vendor;
            nearestYards = yards;
        }
        if (yards <= reachYards)
            inReach.emplace_back(yards, &vendor);
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

    // The trip planner's view of the bot (ErrandRules.h): what each errand is worth now and where it can be done
    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    WorldTravel::SlotBand const& band = config.SlotBands[WorldTravel::LevelBand(bot->GetLevel())];
    WorldTravel::ErrandSnapshot snapshot;
    snapshot.Level = bot->GetLevel();
    snapshot.Money = bot->GetMoney();
    snapshot.Rates = WorldTravel::CurrentRates(measure, band);
    snapshot.BagHorizonHours = band.BagHorizonHours;
    snapshot.FreeSlots = WorldTravel::FreeGeneralSlots(bot);
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

    // The vendor visit of the npc wander: sell by worth, buy (a bag by worth) and repair, all paid. A vendor that is
    // gone leaves the stop's other errands
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

        constexpr uint32 MIN_VENDOR_BALANCE = 1000;  // 10 silver
        botAI->DoSpecificAction("sell", Event("sell", "vendor"), true);
        // A bag bought earlier may lie inside the bag it should replace, where the swap always fails
        if (WorldTravel::MoveSpareBagToBackpack(bot, ItemUsageValue::SmallestBagSize(bot)))
            botAI->DoSpecificAction("equip upgrades packet action");

        if (bot->GetMoney() >= MIN_VENDOR_BALANCE)
            botAI->DoSpecificAction("buy", Event("buy", "vendor"), true);

        if (vendor->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && AI_VALUE(bool, "can repair"))
            botAI->DoSpecificAction("repair", Event("repair"), true);

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

constexpr float HOP_LIFT_YARDS = 0.5f;  // the hop lands this far above the deck point (as the off-mesh snap does)
using TravelLifecycle::MotionKind;
using TravelLifecycle::Outcome;
using TravelLifecycle::PassengerMotion;
using TravelLifecycle::Point;
namespace TransportGeometry = TravelTransportGeometry;

// The current leg is a transport leg past its approach: the bot waits at the dock, boards, rides or steps off. The
// follower scans for the transport then, and the trip's deadline waits (the next departure may be minutes away)
bool AtTransportLeg(TravelLegs::Trip const& trip)
{
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport &&
           trip.boarding != TravelLegs::Boarding::Approach;
}

// The leg's transport, found anew on every scan (never kept across ticks): the one the bot rides, else the nearest
// of the leg's entry to its dock on the bot's map. O(transports on the map), once a scan
Transport* FindLegTransport(Player* bot, TravelRoute::Leg const& leg, uint64 expected)
{
    if (Transport* const ridden = bot->GetTransport())
        return ridden->GetEntry() == leg.object ? ridden : nullptr;
    if (expected)
    {
        Transport* const transport = bot->GetMap()->GetTransport(ObjectGuid(expected));
        if (transport && transport->GetEntry() == leg.object && TransportGeometry::Available(*bot, *transport))
            return transport;
    }
    Transport* best = nullptr;
    float bestYards = std::numeric_limits<float>::max();
    auto const consider = [&](Transport* transport)
    {
        if (!transport || transport->GetEntry() != leg.object || !TransportGeometry::Available(*bot, *transport))
            return;
        float const yards = transport->IsStaticTransport() ? transport->GetExactDist(leg.from.x, leg.from.y, leg.from.z)
                                                           : transport->GetExactDist2d(leg.from.x, leg.from.y);
        if (yards < bestYards)
        {
            best = transport;
            bestYards = yards;
        }
    };
    for (Transport* transport : bot->GetMap()->GetAllTransports())
        consider(transport);
    if (!best)
    {
        // Grid-loaded static transports may be absent from the motion transport set.
        std::list<GameObject*> objects;
        Acore::AllGameObjectsWithEntryInRange check(bot, leg.object, TravelLegs::STATIC_DOCK_YARDS * 2.0f);
        Acore::GameObjectListSearcher<Acore::AllGameObjectsWithEntryInRange> searcher(bot, objects, check);
        Cell::VisitObjects(bot, searcher, TravelLegs::STATIC_DOCK_YARDS * 2.0f);
        for (GameObject* object : objects)
            consider(object->ToTransport());
    }
    return best;
}

// What keeps a bot from a transport move this tick (the board gate's debug word), or nothing
char const* TransportMoveHold(Player const& bot)
{
    if (!bot.movespline->Finalized())
        return "spline";
    if (bot.HasUnitState(UNIT_STATE_NOT_MOVE))
        return "state";
    if (bot.HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
        return "flag";
    if (bot.IsMovementPreventedByCasting())
        return "cast";
    if (bot.IsFlying())
        return "flying";
    if (bot.Unit::IsFalling())
        return "falling";
    return nullptr;
}

bool ReadyForTransportMove(Player const& bot) { return !TransportMoveHold(bot); }

// A ship's or zeppelin's stop node is the vessel's own position (sea level, or a zeppelin's origin about 18 yd above
// its tower floor): the ground beside it within these is where a walk to the stop ends while no quay is known
constexpr float STOP_GROUND_ACROSS_YARDS = 25.0f;
constexpr float STOP_GROUND_VERTICAL_YARDS = 22.0f;

// The nearest ground beside a vessel's stop or position, looked for at the level of the bot that walks there (WT54-5):
// a rider's deck, or the point an approach walk comes from. Looked for at the stop's own height it is the sea floor
// under a berth, or a zeppelin tower's roof or upper ramp instead of the platform 18 yd below the hull's origin.
// Nothing when the tile is not loaded or none lies within reach
std::optional<Point> GroundBesideStop(Map* map, Point const& stop, float level)
{
    Point at{stop.X, stop.Y, level};
    if (NavMeshSnap::Nearest(map, at.X, at.Y, at.Z, STOP_GROUND_ACROSS_YARDS, STOP_GROUND_VERTICAL_YARDS, NAV_GROUND) !=
        NavMeshSnap::Result::Found)
        return std::nullopt;
    return at;
}

// A vessel's deck below its origin (WT54-7), measured on its model at the hull's centre: casts down from the origin,
// each from just below the surface the last one hit. The last surface is the hull's bottom (run 10: Northrend zeppelins
// -8.94/-17.88/-25.51, deck -17.88); the deck is the lowest surface above it with a body's headroom. The model is the
// same at every pose and on every map, so the yards are kept per entry for every map thread; zero for a vessel whose
// stop does not hang above its ground (a ship: its deck lies above its waterline origin) or with no deck found.
constexpr float DECK_HEADROOM_YARDS = 2.0f;
constexpr uint32 DECK_DROP_HITS = 8;

std::mutex deckDropLock;
std::unordered_map<uint32, float> deckDrops;

std::optional<float> KnownDeckDrop(uint32 entry)
{
    std::lock_guard<std::mutex> guard(deckDropLock);
    auto const known = deckDrops.find(entry);
    return known == deckDrops.end() ? std::nullopt : std::optional<float>(known->second);
}

std::optional<float> MeasureDeckDrop(Transport const& transport, uint32 phase)
{
    if (!transport.m_model)
        return std::nullopt;
    Point const centre = TransportGeometry::HullCentre(transport);
    float const low = transport.m_model->GetBounds().low().z;
    float top = centre.Z;
    std::vector<float> surfaces;
    std::string hits;
    while (surfaces.size() < DECK_DROP_HITS && top > low)
    {
        auto const surface =
            TransportGeometry::DeckHeight(transport, phase, {centre.X, centre.Y, top}, 0.0f, top - low + 1.0f);
        if (!surface)
            break;
        hits += Acore::StringFormat("{}{:.2f}", hits.empty() ? "" : "/", *surface - centre.Z);
        surfaces.push_back(*surface);
        top = *surface - 0.05f;
    }
    std::optional<float> deck;
    for (std::size_t i = 0; i + 1 < surfaces.size(); ++i)
        if ((i ? surfaces[i - 1] : centre.Z) - surfaces[i] >= DECK_HEADROOM_YARDS)
            deck = surfaces[i];
    LOG_DEBUG("playerbots", "[TravelLifecycle] deck_drop entry={} origin_z={:.2f} low={:.2f} hits={} deck={}",
              transport.GetEntry(), centre.Z, low, hits.empty() ? std::string("none") : hits,
              deck ? Acore::StringFormat("{:.2f}", *deck - centre.Z) : std::string("none"));
    if (!deck)
        return std::nullopt;
    return centre.Z - *deck;
}

// The approach walk to a ship's or zeppelin's stop ends on the ground beside it, not at the stop (in the berth's
// water, or in the air at a zeppelin's origin), where the arriving hull would take the bot aboard without a boarding.
// A stop that hangs above its ground (a zeppelin's) is waited for at its deck's level (WT54-7): the ground nearest the
// origin is the tower's roof or upper ramp (Tirisfal 113.09, Orgrimmar 62.41), 18 yd above the platform the gondola
// docks at. The deck is measured once the vessel is on the bot's map (here or by any bot); an end aimed before that is
// aimed again once it is known. The walk's point before the stop is no level (WT54-5: a graph node at the tower's foot)
void AimApproachEnd(Player* bot, TravelRoute::Point& end, TravelRoute::Leg const& leg, uint64 expected)
{
    if (end.mapId != leg.from.mapId)
        return;
    Point const stop{leg.from.x, leg.from.y, leg.from.z};
    bool const atStop = TravelRoute::Distance(end, leg.from) < 0.1f;
    auto drop = KnownDeckDrop(leg.object);
    if (!drop)
        if (Transport* const transport = FindLegTransport(bot, leg, expected))
            if (transport->ToMotionTransport())
            {
                auto const plain = GroundBesideStop(bot->GetMap(), stop, stop.Z);
                if (plain && transport->m_model)
                {
                    // A failed measurement over a hanging stop is not kept: the next scan measures again
                    bool const overGround = plain->Z < stop.Z - TravelLifecycle::DOCK_FLOOR_YARDS;
                    std::optional<float> const measured =
                        overGround ? MeasureDeckDrop(*transport, bot->GetPhaseMask()) : std::optional<float>(0.0f);
                    if (measured)
                    {
                        drop = measured;
                        std::lock_guard<std::mutex> guard(deckDropLock);
                        deckDrops.emplace(leg.object, *drop);
                    }
                }
            }
    bool const hangs = drop && *drop > TravelLifecycle::DOCK_FLOOR_YARDS;
    float const level = hangs ? stop.Z - *drop : stop.Z;
    if (!atStop && (!hangs || std::fabs(end.z - level) <= 2.0f * TravelLifecycle::DOCK_FLOOR_YARDS))
        return;
    auto const ground = GroundBesideStop(bot->GetMap(), stop, level);
    if (!ground || (!atStop && TravelRoute::Distance(end, {end.mapId, ground->X, ground->Y, ground->Z}) < 0.5f))
        return;
    end = {end.mapId, ground->X, ground->Y, ground->Z};
    LOG_DEBUG("playerbots", "[Route] {} walks to {:.2f}, {:.2f}, {:.2f} beside the stop of {} (deck {} below it)",
              bot->GetName(), ground->X, ground->Y, ground->Z, leg.object,
              hangs ? Acore::StringFormat("{:.2f}", *drop) : std::string("not"));
}

// The ground nearest the rider that lies off the docked hull, within a step of the deck's floor (WT54-3). A route that
// ends at the vessel's stop gives no ground off the hull: beside a stop lies the pier under the docked ship, or the
// ground below a zeppelin's tower. The bot then goes on from this ground; the exit's checks stay as they are.
constexpr float OFF_HULL_REACH_YARDS = 40.0f;
constexpr float OFF_HULL_RING_YARDS = 2.0f;
constexpr uint32 OFF_HULL_BEARINGS = 16;

std::optional<Point> GroundOffHull(Player const& bot, Transport const& ridden, bool trace)
{
    TransportGeometry::GroundGeometry const geometry{bot, &ridden};
    Point const from = TransportGeometry::PositionOf(bot);
    constexpr float FULL_TURN = 6.28318531f;
    // Why the candidates failed (traced bots, WT54-4): no ground mesh within a step of the deck's height, ground under
    // the hull, or a mesh point that is not standing ground (water, or the snap left the point)
    uint32 noMesh = 0;
    uint32 onHull = 0;
    uint32 notGround = 0;
    for (float radius = OFF_HULL_RING_YARDS; radius <= OFF_HULL_REACH_YARDS; radius += OFF_HULL_RING_YARDS)
        for (uint32 bearing = 0; bearing < OFF_HULL_BEARINGS; ++bearing)
        {
            float const angle = FULL_TURN * static_cast<float>(bearing) / static_cast<float>(OFF_HULL_BEARINGS);
            Point const seed{from.X + radius * std::cos(angle), from.Y + radius * std::sin(angle), from.Z};
            auto const ground = geometry.SnapGround(seed, OFF_HULL_RING_YARDS / 2.0f, TravelLifecycle::EXIT_STEP_YARDS);
            if (!ground)
                ++noMesh;
            else if (!geometry.OffHull(*ground))
                ++onHull;
            else if (!geometry.OnGround(*ground))
                ++notGround;
            else
                return ground;
        }
    if (trace)
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] ground_off_hull bot={} entry={} at={:.2f},{:.2f},{:.2f} origin_z={:.2f} "
                  "none: no_mesh={} on_hull={} not_ground={}",
                  bot.GetGUID().GetCounter(), ridden.GetEntry(), from.X, from.Y, from.Z, ridden.GetPositionZ(), noMesh,
                  onHull, notGround);
    return std::nullopt;
}

bool PrepareTransportMove(Player& bot)
{
    if (!ReadyForTransportMove(bot))
        return false;
    if (!bot.IsMounted())
        return true;
    WorldPacket emptyPacket;
    bot.GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    return false;
}

PassengerMotion Intent(Player const& bot, Transport const& transport, MotionKind kind, Point const& endpoint)
{
    PassengerMotion motion;
    motion.Kind = kind;
    motion.Transport = transport.GetGUID().GetRawValue();
    motion.Entry = transport.GetEntry();
    motion.Map = bot.GetMapId();
    motion.Endpoint = endpoint;
    motion.Dock = TransportGeometry::PoseOf(transport);
    return motion;
}

// A boarding walks straight onto the deck; an exit walks across the deck by its deck walk's corners to the deck's edge
// (via) and on to the quay. Via points within 0.2 yd of the point before are left out (a spline refuses a segment
// shorter than 0.1 yd)
void IssueTransportMove(Player& bot, NewRpgInfo::Travel& data, PassengerMotion motion,
                        std::vector<Point> const& via = {})
{
    if (bot.IsSitState())
        bot.SetStandState(UNIT_STAND_STATE_STAND);
    bot.GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(&bot);
    if (!via.empty())
    {
        Movement::PointsArray path{{bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ()}};
        for (Point const& point : via)
            if ((G3D::Vector3{point.X, point.Y, point.Z} - path.back()).length() >= 0.2f)
                path.push_back({point.X, point.Y, point.Z});
        G3D::Vector3 const end{motion.Endpoint.X, motion.Endpoint.Y, motion.Endpoint.Z};
        if (path.size() > 1 && (end - path.back()).length() < 0.2f)
            path.back() = end;
        else
            path.push_back(end);
        init.MovebyPath(path);
    }
    else
        init.MoveTo(motion.Endpoint.X, motion.Endpoint.Y, motion.Endpoint.Z, false, false);
    if (init.Launch() <= 0)
        return;
    motion.Spline = bot.movespline->GetId();
    data.passengerMotion = motion;
}

// A ship or zeppelin at a stop it keeps for the attach's deadline (MotionTransport::IsMoving is private: its key
// frames, as MotionTransport::Update reads them)
bool Docked(MotionTransport const& ship)
{
    uint32 const period = ship.GetPeriod();
    if (!period)
        return false;
    uint32 const timer = ship.GetPathProgress() % period;
    for (KeyFrame const& frame : ship.GetKeyFrames())
        if (timer >= frame.ArriveTime && timer + PlayerbotAI::ABOARD_DEADLINE_MS < frame.DepartureTime)
            return true;
    return false;
}

// `route aboard` (tests): the bot is stood on the deck of the docked ship or zeppelin of that entry nearest it on its
// map, at the boarding deck point from where it stands (the test puts it on the dock first). No explicit attach:
// AddPassenger takes the current position as the offset, so PlayerbotAI's passenger check attaches it after the
// teleport (BeginAboard). Refused: absent (none on the map), not_mo (an elevator), away (not at a stop), no_deck
void PlaceAboard(PlayerbotAI* botAI, Player* bot, uint32 entry)
{
    Transport* transport = nullptr;
    float nearest = std::numeric_limits<float>::max();
    for (Transport* candidate : bot->GetMap()->GetAllTransports())
    {
        if (!candidate->IsInWorld() || candidate->GetEntry() != entry)
            continue;
        float const yards = candidate->GetExactDist2d(bot);
        if (yards < nearest)
        {
            transport = candidate;
            nearest = yards;
        }
    }
    MotionTransport const* ship = transport ? transport->ToMotionTransport() : nullptr;
    std::optional<Point> deck;
    char const* refusal = nullptr;
    if (!transport)
        refusal = "absent";
    else if (!ship)
        refusal = "not_mo";
    else if (!Docked(*ship))
        refusal = "away";
    else
    {
        deck = TransportGeometry::GroundGeometry{*bot, transport}.DeckFrom(TransportGeometry::PositionOf(*bot));
        if (!deck ||
            bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), deck->X, deck->Y, deck->Z, bot) != transport)
            refusal = "no_deck";
    }
    if (refusal)
    {
        LOG_INFO("playerbots", "[Route] {} aboard {} refused: {}", bot->GetName(), entry, refusal);
        std::string const source = std::string("refused:") + refusal;
        EventLog::Emit(bot, PlayerbotAI::ABOARD_EVENT_TYPE, source.c_str(), entry);
        return;
    }
    LOG_INFO("playerbots", "[Route] {} aboard {}: deck {:.1f}, {:.1f}, {:.1f}", bot->GetName(), entry, deck->X, deck->Y,
             deck->Z);
    bot->NearTeleportTo(deck->X, deck->Y, deck->Z + HOP_LIFT_YARDS, bot->GetOrientation());
    botAI->BeginAboard(entry);
}

std::optional<Outcome> TakePassengerOutcome(Player& bot, NewRpgInfo::Travel& data)
{
    if (!data.passengerMotion)
        return std::nullopt;
    auto const result =
        TravelLifecycle::PassengerOutcome(*data.passengerMotion, TransportGeometry::ReadMembership(bot));
    if (!result)
        return std::nullopt;
    TransportGeometry::DiscardMotion(bot, data.passengerMotion);
    return result;
}

// The crossing checks' words (TravelLifecycle::CrossingCheck), for the debug log
constexpr std::array<char const*, TravelLifecycle::CrossingTally::REASONS> CROSSING_NAMES = {
    "accepted", "no_shore", "no_deck",  "on_hull",  "height",   "distance",
    "ground",   "onward",   "crossing", "corridor", "deck_walk"};

// A failed exit or quay search, for the debug log: the hull box, the count of each failed check and its first sample
void LogCrossingSearch(Player const& bot, NewRpgInfo::Travel const& data, Transport const& transport, char const* kind,
                       TravelLifecycle::CrossingTally const& tally)
{
    TravelLifecycle::Footprint const hull = TransportGeometry::Hull(bot, transport);
    TransportGeometry::GroundGeometry const geometry{bot, &transport};
    std::string reasons;
    for (std::size_t i = 0; i < CROSSING_NAMES.size(); ++i)
    {
        if (!tally.Counts[i])
            continue;
        auto const& sample = *tally.First[i];
        reasons +=
            Acore::StringFormat(" {}={}@{:.2f},{:.2f},{:.2f}>{:.2f},{:.2f},{:.2f}", CROSSING_NAMES[i], tally.Counts[i],
                                sample.From.X, sample.From.Y, sample.From.Z, sample.To.X, sample.To.Y, sample.To.Z);
        if (i == static_cast<std::size_t>(TravelLifecycle::CrossingCheck::Crossing))
            reasons += Acore::StringFormat(
                "/{}", TransportGeometry::GroundGeometry::BlockName(geometry.CrossingBlock(sample.From, sample.To)));
        if (i == static_cast<std::size_t>(TravelLifecycle::CrossingCheck::Corridor) && tally.Onward)
            reasons += Acore::StringFormat(
                "/{}", TransportGeometry::GroundGeometry::ReachName(geometry.GroundReach(sample.To, *tally.Onward)));
    }
    std::string const onward =
        tally.Onward ? Acore::StringFormat("{:.2f},{:.2f},{:.2f}", tally.Onward->X, tally.Onward->Y, tally.Onward->Z)
                     : std::string("none");
    Point const centre = TransportGeometry::HullCentre(transport);
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] crossing_search bot={} trip={} leg={} kind={} entry={} map={} at={:.2f},{:.2f},{:.2f} "
              "origin={:.2f},{:.2f},{:.2f} o={:.3f} hull={:.1f},{:.1f},{:.1f}..{:.1f},{:.1f},{:.1f} "
              "centre={:.2f},{:.2f} onward={} deck_cells={}{}",
              bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, kind, transport.GetEntry(), bot.GetMapId(),
              bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ(), transport.GetPositionX(),
              transport.GetPositionY(), transport.GetPositionZ(), transport.GetOrientation(), hull.Low.X, hull.Low.Y,
              hull.Low.Z, hull.High.X, hull.High.Y, hull.High.Z, centre.X, centre.Y, onward, tally.DeckCells, reasons);
    // The exit probe: per bearing (degrees from the route's side), the candidate that got furthest through the checks
    for (std::size_t turn = 0; turn < tally.Bearings.size(); ++turn)
    {
        auto const& furthest = tally.Bearings[turn];
        if (!furthest)
            continue;
        int const side = static_cast<int>((turn + 1) / 2) * (turn % 2 ? 1 : -1);
        std::string block;
        if (furthest->Check == TravelLifecycle::CrossingCheck::Crossing)
            block = Acore::StringFormat("/{}", TransportGeometry::GroundGeometry::BlockName(
                                                   geometry.CrossingBlock(furthest->From, furthest->To)));
        else if (furthest->Check == TravelLifecycle::CrossingCheck::Corridor && tally.Onward)
            block = Acore::StringFormat(
                "/{}", TransportGeometry::GroundGeometry::ReachName(geometry.GroundReach(furthest->To, *tally.Onward)));
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] exit_probe bot={} trip={} bearing={} at={:.0f} check={}{} "
                  "from={:.2f},{:.2f},{:.2f} to={:.2f},{:.2f},{:.2f}",
                  bot.GetGUID().GetCounter(), data.tripId,
                  side * static_cast<int>(360 / TravelLifecycle::CROSSING_BEARINGS), furthest->Distance,
                  CROSSING_NAMES[static_cast<std::size_t>(furthest->Check)], block, furthest->From.X, furthest->From.Y,
                  furthest->From.Z, furthest->To.X, furthest->To.Y, furthest->To.Z);
    }
}

// WT43b: the boarding crossing walked back where the transport stands as it did at the boarding (the passenger
// observer confirms it as any exit, ConfirmExit). Its shore may touch the hull (a zeppelin's gondola beside its tower):
// then the walk goes on from the shore, along the crossing's line, to the first point off the hull up to
// BACK_OUT_YARDS out, and that step is the exit's crossing (CheckExit from the shore; the deck-to-shore part is the
// boarding's, checked then at this pose). Onward ground: a step beyond
struct BackPath
{
    Point Deck;
    Point From;  // where the exit's crossing starts: the deck, or the shore when the walk goes on from it
    Point Ground;
    Point Onward;
    std::vector<Point> Walk;  // the deck walk's corners, ending at Deck
};

std::optional<BackPath> BackCrossing(Player const& bot, NewRpgInfo::Travel const& data, Transport const& ridden,
                                     bool trace)
{
    constexpr float ONWARD_YARDS = 4.0f;
    constexpr float BACK_OUT_YARDS = 3.0f;
    auto const walk = TravelLegs::CrossingBack(data.trip.boardCrossing, ridden.GetGUID().GetRawValue(),
                                               TransportGeometry::PoseOf(ridden));
    if (!walk)
        return std::nullopt;
    float const dx = walk->shore.X - walk->deck.X;
    float const dy = walk->shore.Y - walk->deck.Y;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.1f)
        return std::nullopt;
    TransportGeometry::GroundGeometry const geometry{bot, &ridden};
    auto const out = [&](Point const& from, float yards)
    {
        return geometry.SnapGround({from.X + dx / length * yards, from.Y + dy / length * yards, from.Z}, 1.5f,
                                   TravelLifecycle::DOCK_FLOOR_YARDS);
    };
    TravelLifecycle::CrossingCheck check = TravelLifecycle::CrossingCheck::NoShore;
    std::optional<BackPath> path;
    float extra = 0.0f;
    for (; extra <= BACK_OUT_YARDS && !path; extra += 1.0f)
    {
        Point const from = extra > 0.0f ? walk->shore : walk->deck;
        auto const ground = extra > 0.0f ? out(walk->shore, extra) : std::optional<Point>{walk->shore};
        auto const onward = ground ? out(*ground, ONWARD_YARDS) : std::nullopt;
        if (!ground || !onward)
        {
            check = ground ? TravelLifecycle::CrossingCheck::Onward : TravelLifecycle::CrossingCheck::NoShore;
            continue;
        }
        check = TravelLifecycle::CheckExit(geometry, from, *ground, *onward);
        if (check == TravelLifecycle::CrossingCheck::Accepted)
            path = BackPath{walk->deck, from, *ground, *onward};
    }
    if (path)
    {
        std::uint32_t expanded = 0;
        auto deckWalk = geometry.DeckPath(TransportGeometry::PositionOf(bot), walk->deck,
                                          TravelLifecycle::DECK_PATH_CELLS, expanded);
        if (deckWalk)
            path->Walk = std::move(*deckWalk);
        else
        {
            check = TravelLifecycle::CrossingCheck::DeckWalk;
            path.reset();
        }
    }
    if (trace)
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] exit_back bot={} trip={} leg={} entry={} check={} out={:.0f} "
                  "deck={:.2f},{:.2f},{:.2f} shore={:.2f},{:.2f},{:.2f}",
                  bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, ridden.GetEntry(),
                  CROSSING_NAMES[static_cast<std::size_t>(check)], extra - 1.0f, walk->deck.X, walk->deck.Y,
                  walk->deck.Z, walk->shore.X, walk->shore.Y, walk->shore.Z);
    return path;
}

void WalkOff(Player& bot, NewRpgInfo::Travel& data, Transport const& ridden, std::vector<Point> const& route)
{
    if (data.passengerMotion || data.trip.exitSought || !PrepareTransportMove(bot))
        return;
    bool const trace = !data.place.empty() && sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG);
    TravelLifecycle::CrossingTally tally;
    std::vector<Point> onward = route;
    if (!TravelLifecycle::OnwardPoint(TransportGeometry::GroundGeometry{bot, &ridden}, route))
        if (auto const ground = GroundOffHull(bot, ridden, trace))
            onward.push_back(*ground);
    auto const exit = TransportGeometry::FindExit(bot, ridden, onward, trace ? &tally : nullptr);
    if (exit)
    {
        if (trace)
        {
            std::string walk;
            for (std::size_t i = 0; i + 1 < exit->Walk.size(); ++i)
                walk += Acore::StringFormat("{}{:.1f},{:.1f},{:.1f}", walk.empty() ? "" : "/", exit->Walk[i].X,
                                            exit->Walk[i].Y, exit->Walk[i].Z);
            LOG_DEBUG("playerbots",
                      "[TravelLifecycle] exit_path bot={} trip={} leg={} entry={} map={} at={:.2f},{:.2f},{:.2f} "
                      "deck={:.2f},{:.2f},{:.2f} ground={:.2f},{:.2f},{:.2f} onward={:.2f},{:.2f},{:.2f} "
                      "corners={} cells={}",
                      bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, ridden.GetEntry(), bot.GetMapId(),
                      bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ(), exit->Deck.X, exit->Deck.Y,
                      exit->Deck.Z, exit->Ground.X, exit->Ground.Y, exit->Ground.Z, exit->Onward.X, exit->Onward.Y,
                      exit->Onward.Z, walk.empty() ? std::string("none") : walk, tally.DeckCells);
        }
        PassengerMotion motion = Intent(bot, ridden, MotionKind::Exit, exit->Ground);
        motion.ExitFrom = exit->Deck;
        motion.Onward = exit->Onward;
        IssueTransportMove(bot, data, motion, exit->Walk);
        return;
    }
    if (trace)
        LogCrossingSearch(bot, data, ridden, "exit", tally);
    if (auto const back = BackCrossing(bot, data, ridden, trace))
    {
        PassengerMotion motion = Intent(bot, ridden, MotionKind::Exit, back->Ground);
        motion.ExitFrom = back->From;
        motion.Onward = back->Onward;
        std::vector<Point> via = back->Walk;
        via.push_back(back->From);
        IssueTransportMove(bot, data, motion, via);
        return;
    }
    data.trip.exitSought = true;
    data.trip.exitless = true;
    EventLog::Emit(&bot, TravelLegs::TRANSPORT_EVENT_TYPE,
                   TravelLegs::TransportSource(TravelLegs::TransportEvent::NoExit), ridden.GetEntry(), 0);
}

// Where the bot goes after a transport's quay, for the exit search: the next walk leg's waypoints after its start
// (its graph nodes, then its end), or the start of the next leg of another kind; the trip's destination when the
// next leg lies on another map or there is none. deckZ: the rider's floor, the level the ground beside the stop is
// looked for at
std::vector<Point> OnwardRoute(TravelLegs::Trip const& trip, Map* map, uint32 mapId, Point const& dest, float deckZ)
{
    std::vector<Point> route;
    if (trip.legIdx + 1 < trip.legs.size())
    {
        TravelRoute::Leg const& onward = trip.legs[trip.legIdx + 1];
        if (onward.type == TravelRoute::LegType::Walk)
        {
            std::vector<TravelLegs::Waypoint> const points = TravelLegs::Waypoints(onward, GraphNodeAt);
            for (std::size_t i = 1; i < points.size(); ++i)
                if (points[i].at.mapId == mapId)
                    route.push_back({points[i].at.x, points[i].at.y, points[i].at.z});
        }
        else if (onward.from.mapId == mapId)
            route.push_back({onward.from.x, onward.from.y, onward.from.z});
    }
    if (route.empty())
        route.push_back(dest);
    // A point at the leg's end node is the vessel's own stop (in the berth's water, or in the air at a zeppelin's
    // origin), with no ground to walk on: the ground beside it instead, as for the approach walk (SeekQuay)
    TravelRoute::Point const& end = trip.legs[trip.legIdx].to;
    for (Point& point : route)
        if (end.mapId == mapId &&
            TravelRoute::Distance(end, {mapId, point.X, point.Y, point.Z}) <= TravelLegs::SAME_POINT_YARDS)
            point = GroundBesideStop(map, point, deckZ).value_or(point);
    return route;
}

// An aboard scan short of the leave, for the debug log: what the dock test at the leg's end saw
void LogAboardScan(Player* bot, TravelLegs::Trip const& trip, float moved)
{
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    TravelLegs::TransportSight const& sight = trip.sight;
    float const dx = sight.at.x - leg.to.x;
    float const dy = sight.at.y - leg.to.y;
    LOG_DEBUG("playerbots",
              "[Route] {} aboard {} on map {}: {:.1f} yd (2D) from the leg's end on map {}, still {}, moving {}, "
              "static {}, {:.1f} yd moved since the last scan (-1: none)",
              bot->GetName(), leg.object, sight.at.mapId, std::sqrt(dx * dx + dy * dy), leg.to.mapId, trip.still,
              sight.moving, sight.isStatic, moved);
}

float SeeTransport(Player* bot, Transport* transport, TravelLegs::Trip const& trip, TravelLegs::TransportSight& sight);

uint32 MotionQuayLeg(TravelLegs::Trip const& trip)
{
    uint32 index = trip.legIdx;
    if (index >= trip.legs.size())
        return TravelLegs::NO_NODE;
    TravelRoute::Leg const& current = trip.legs[index];
    if (current.type == TravelRoute::LegType::Walk && index + 1 < trip.legs.size())
    {
        TravelRoute::Point const& dock = trip.legs[++index].from;
        if (current.to.mapId != dock.mapId || TravelRoute::Distance(current.to, dock) > 1.0f)
            return TravelLegs::NO_NODE;
    }
    TravelRoute::Leg const& leg = trip.legs[index];
    if (leg.type != TravelRoute::LegType::Transport)
        return TravelLegs::NO_NODE;
    GameObjectTemplate const* const info = sObjectMgr->GetGameObjectTemplate(leg.object);
    return info && info->type == GAMEOBJECT_TYPE_MO_TRANSPORT ? index : TravelLegs::NO_NODE;
}

void SeekQuay(Player* bot, NewRpgInfo::Travel& data, TravelLegs::Observation& seen)
{
    TravelLegs::Trip& trip = data.trip;
    if (trip.legIdx >= trip.legs.size() || trip.boarding != TravelLegs::Boarding::Approach)
        return;
    // At its transport's dock the bot waits on foot (WT54-7; TravelKeepsOnFoot keeps it so): Thunder Bluff's zeppelin
    // left while the bot spent the scan after its docking on the dismount
    if (trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport && bot->IsMounted() && !bot->IsFlying() &&
        !bot->IsInCombat())
    {
        WorldPacket emptyPacket;
        bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    }

    if (trip.hasQuay || getMSTimeDiff(data.transportScanMs, seen.nowMs) < TravelLegs::TRANSPORT_SCAN_MS)
        return;
    if (seen.quayLeg == TravelLegs::NO_NODE)
    {
        if (trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport && !trip.quaySought)
            TravelLegs::SetQuay(trip, nullptr);
        return;
    }

    bool const approachWalk = seen.quayLeg != trip.legIdx;
    TravelRoute::Leg const& leg = trip.legs[seen.quayLeg];
    if (bot->GetMapId() != leg.from.mapId)
        return;

    data.transportScanMs = seen.nowMs;
    // The approach walk ends on the ground beside the stop (AimApproachEnd); once docked, the quay
    if (approachWalk && !trip.waypoints.empty())
        AimApproachEnd(bot, trip.waypoints.back().at, leg, trip.sight.guid);
    if (approachWalk && (trip.waypoints.empty() || trip.next + 1 < trip.waypoints.size()))
        return;
    TravelLegs::TransportSight sight;
    Transport* const transport = FindLegTransport(bot, leg, trip.sight.guid);
    if (transport)
        SeeTransport(bot, transport, trip, sight);
    TravelLegs::NoteSight(trip, sight);
    if (!transport || !TravelLegs::DockedAt(trip, leg.from))
    {
        trip.quaySought = false;
        return;
    }
    if (trip.quaySought || !ReadyForTransportMove(*bot))
        return;

    TransportGeometry::GroundGeometry const geometry{*bot, transport};
    Point from = TransportGeometry::PositionOf(*bot);
    if (!approachWalk)
    {
        // A bot whose approach walk ended before the deck's level was known waits off that level (WT54-10:
        // Orgrimmar's upper ramp 62.41, the platform 52.0): its quay is looked for from the ground beside the stop at
        // the deck's level, and the walk to the quay takes it there. Once per docking, so no cost per scan
        TravelRoute::Point seed{leg.from.mapId, from.X, from.Y, from.Z};
        AimApproachEnd(bot, seed, leg, trip.sight.guid);
        from = Point{seed.x, seed.y, seed.z};
    }
    bool const trace = !data.place.empty() && sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG);
    TravelLifecycle::CrossingTally tally;
    auto const shore = TravelLifecycle::FindQuay(geometry, from, TransportGeometry::PositionOf(*transport),
                                                 TransportGeometry::HullCentre(*transport), trace ? &tally : nullptr);
    if (!shore)
    {
        TravelLegs::SetQuay(trip, nullptr);
        LOG_DEBUG("playerbots", "[Route] {} found no reachable boarding floor for {}", bot->GetName(), leg.object);
        if (trace)
        {
            LogCrossingSearch(*bot, data, *transport, "quay", tally);
            // The first quay that failed only on the walk to it, and how the ground mesh joins the bot to it (WT54-7)
            auto const& corridor = tally.First[static_cast<std::size_t>(TravelLifecycle::CrossingCheck::Corridor)];
            if (corridor)
                LOG_DEBUG("playerbots",
                          "[TravelLifecycle] quay_reach bot={} at={:.2f},{:.2f},{:.2f} shore={:.2f},{:.2f},{:.2f} "
                          "reach={}",
                          bot->GetGUID().GetCounter(), from.X, from.Y, from.Z, corridor->From.X, corridor->From.Y,
                          corridor->From.Z,
                          TransportGeometry::GroundGeometry::ReachName(geometry.GroundReach(from, corridor->From)));
        }
        return;
    }

    TravelRoute::Point const quay{leg.from.mapId, shore->X, shore->Y, shore->Z};
    TravelLegs::SetQuay(trip, &quay);
    if (approachWalk)
        trip.waypoints.back().at = quay;
    LOG_DEBUG("playerbots", "[Route] {} waits for transport {} on ground at {:.2f}, {:.2f}, {:.2f}", bot->GetName(),
              leg.object, quay.x, quay.y, quay.z);
}

// Where a boarding starts (WT54-10): the quay the bot waits at, whose crossing FindQuay accepted, else where it
// stands. A bot within QUAY_REACH_YARDS of its quay does not walk to it, and from its own spot the crossing can differ
// (run 12, Orgrimmar's platform: accepted from the quay, blocked 0.6 yd beside it), so the board check, its deck point
// and the walk all start at the quay
Point BoardStart(Player const& bot, TravelLegs::Trip const& trip)
{
    if (trip.hasQuay && trip.quay.mapId == bot.GetMapId())
        return Point{trip.quay.x, trip.quay.y, trip.quay.z};
    return TransportGeometry::PositionOf(bot);
}

// A Board walk, for the debug log: the deck point, the bot's height and the distance
void LogBoardMove(Player* bot, uint32 entry, Point const& deck)
{
    if (!sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    LOG_DEBUG(
        "playerbots", "[Route] {} boards transport {}: deck point ({:.1f}, {:.1f}, {:.1f}), bot z {:.1f}, {:.1f} yd",
        bot->GetName(), entry, deck.X, deck.Y, deck.Z, bot->GetPositionZ(), bot->GetExactDist(deck.X, deck.Y, deck.Z));
}

// A scan at a still transport that does not board a traced bot, for the debug log: the first gate that holds it back.
// wait_z: off the wait point's floor; no_deck: no deck point; deck_z: the deck point beyond a floor's step from the
// bot; ground: not on ground; prepare: PrepareTransportMove (mounted, casting); accept: the crossing check.
void LogBoardGate(Player* bot, NewRpgInfo::Travel const& data, Transport const& transport,
                  std::optional<Point> const& deck, bool ready, bool accept)
{
    TravelLegs::Trip const& trip = data.trip;
    if (data.place.empty() || trip.legIdx >= trip.legs.size() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    float const waitDz = std::fabs(bot->GetPositionZ() - TravelLegs::WaitPoint(trip, trip.legs[trip.legIdx]).z);
    char const* gate = "accept";
    if (!deck)
        gate = waitDz > TravelLifecycle::DOCK_FLOOR_YARDS ? "wait_z" : "no_deck";
    else if (!ready)
        gate = std::fabs(deck->Z - bot->GetPositionZ()) > TravelLifecycle::DOCK_FLOOR_YARDS ? "deck_z" : "ground";
    else if (!accept)
        gate = "prepare";
    std::string const point =
        deck ? Acore::StringFormat("{:.2f},{:.2f},{:.2f}", deck->X, deck->Y, deck->Z) : std::string("none");
    char const* const hold = TransportMoveHold(*bot);
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] board_gate bot={} trip={} leg={} entry={} at={:.2f},{:.2f},{:.2f} gate={} "
              "wait_dz={:.2f} deck={} mounted={} hold={}",
              bot->GetGUID().GetCounter(), data.tripId, trip.legIdx, transport.GetEntry(), bot->GetPositionX(),
              bot->GetPositionY(), bot->GetPositionZ(), gate, waitDz, point, bot->IsMounted() ? 1 : 0,
              hold ? hold : "none");
}

// A miss this tick, for the debug log: whether the bot stood over the hull (the passenger check's test) or beside it
void LogMisses(Player* bot, TravelLegs::Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    for (TravelLegs::TransportNote const& note : trip.transportNotes)
    {
        if (note.event != TravelLegs::TransportEvent::Missed)
            continue;
        Transport* const transport = FindLegTransport(bot, trip.legs[trip.legIdx], trip.sight.guid);
        bool const onHull = transport && bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(),
                                                                           bot->GetPositionY(), bot->GetPositionZ(),
                                                                           nullptr) == transport;
        LOG_DEBUG("playerbots", "[Route] {} missed transport {} (miss {}): {}, {:.1f} yd from its centre, bot z {:.1f}",
                  bot->GetName(), note.entry, note.amount, onHull ? "over the hull" : "off the hull",
                  transport ? bot->GetExactDist2d(transport) : -1.0f, bot->GetPositionZ());
        return;
    }
}

// A scan of the transport: where it is, whether a ship or zeppelin moved since the scan before (trip.sight), its
// period. Returns the yards moved since that scan on this map, -1 none
float SeeTransport(Player* bot, Transport* transport, TravelLegs::Trip const& trip, TravelLegs::TransportSight& sight)
{
    float moved = -1.0f;
    sight.found = true;
    sight.guid = transport->GetGUID().GetRawValue();
    sight.isStatic = transport->IsStaticTransport();
    sight.at = {bot->GetMapId(), transport->GetPositionX(), transport->GetPositionY(), transport->GetPositionZ()};
    TravelLegs::TransportSight const& before = trip.sight;
    if (before.found && before.guid == sight.guid && before.at.mapId == sight.at.mapId)
        moved = TravelRoute::Distance(before.at, sight.at);
    if (MotionTransport const* ship = transport->ToMotionTransport())
    {
        // MotionTransport::IsMoving is private: a ship or zeppelin is under way when it moved since the scan before
        // (on the first scan it counts as under way, so no docking is read from one position)
        sight.moving = moved < 0.0f || moved > TravelLegs::STILL_YARDS;
        sight.periodMs = ship->GetPeriod();
    }
    else if (StaticTransport const* fixed = transport->ToStaticTransport())
        sight.periodMs = fixed->GetPeriod();
    return moved;
}

void SendTransportNotes(Player* bot, TravelLegs::Trip& trip)
{
    for (TravelLegs::TransportNote const& note : trip.transportNotes)
        EventLog::Emit(bot, TravelLegs::TRANSPORT_EVENT_TYPE, TravelLegs::TransportSource(note.event), note.entry,
                       note.amount);
    trip.transportNotes.clear();
}

// The gate before a trip's end, a route request or a walk (WT19-d, TravelLegs::BeginOffFirst): true when the bot
// rides a ship or zeppelin and gets off first (`Transport off_first`)
bool OffFirstBegins(Player* bot, NewRpgInfo::Travel& data, TravelLegs::OffFirstEnd end = TravelLegs::OffFirstEnd::None,
                    char const* why = nullptr, char const* source = nullptr)
{
    Transport const* const ridden = bot->GetTransport();
    if (!ridden)
        return false;
    TravelLegs::Trip& trip = data.trip;
    if (trip.offFirst.sinceMs)
        return true;
    uint32 const period = TransportGeometry::Period(*ridden);
    if (!TravelLegs::BeginOffFirst(trip, ridden->GetEntry(), ridden->GetGUID().GetRawValue(), period, getMSTime(), end,
                                   why, source))
        return false;
    TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
    SendTransportNotes(bot, trip);
    char const* const waiting = end == TravelLegs::OffFirstEnd::Arrive  ? "arrival"
                                : end == TravelLegs::OffFirstEnd::Abort ? "abort"
                                                                        : "walk or route";
    LOG_DEBUG("playerbots", "[Route] {} gets off transport {} first (the {} waits)", bot->GetName(), ridden->GetEntry(),
              waiting);
    return true;
}

// OffFirst shares the normal local-frame exit, including membership acknowledgment.
TravelLegs::OffStep GetOffFirst(Player* bot, NewRpgInfo::Travel& data)
{
    TravelLegs::Trip& trip = data.trip;
    Transport* const ridden = bot->GetTransport();
    TravelLegs::Observation seen;
    seen.nowMs = getMSTime();
    seen.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    auto const membership = TransportGeometry::ReadMembership(*bot);
    seen.aboardGuid = membership.Guid;
    seen.membershipAvailable = membership.Available;
    if (membership.Available)
        seen.passengerOutcome = TakePassengerOutcome(*bot, data);
    if (getMSTimeDiff(data.transportScanMs, seen.nowMs) >= TravelLegs::TRANSPORT_SCAN_MS)
    {
        data.transportScanMs = seen.nowMs;
        seen.transport.scanned = true;
        if (ridden && membership.Available && membership.Guid == trip.offFirst.guid)
            SeeTransport(bot, ridden, trip, seen.transport);
    }
    bool const wasStill = trip.still;
    TravelLegs::OffStep const step = TravelLegs::StepOffFirst(trip, seen);
    if (seen.transport.scanned && ridden && membership.Available)
    {
        if (step == TravelLegs::OffStep::Disembark)
        {
            // Off where it stands, onto the ground beside the vessel: the trip's goal may be another vessel stop
            // (no ground) or on another map, and the pending operation plans from the shore
            Point const onward =
                GroundBesideStop(bot->GetMap(), TransportGeometry::PositionOf(*ridden), bot->GetPositionZ())
                    .value_or(TransportGeometry::PositionOf(*bot));
            WalkOff(*bot, data, *ridden, std::vector<Point>{onward});
        }
        else if (step == TravelLegs::OffStep::Wait && wasStill)
        {
            trip.transportNotes.push_back({TravelLegs::TransportEvent::StuckAboard, ridden->GetEntry(), 0});
            trip.exitSought = false;
            TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
        }
    }
    SendTransportNotes(bot, trip);
    return step;
}

// The last-resort hop (WT.14): a few yards onto the docked deck, only unseen (the scan's sight check), counted (its
// Transport hop event went with the trip's notes; the honesty row for `wlog report honesty`)
void HopOnto(Player* bot, NewRpgInfo::Travel& data, Transport const& transport, Point deck, uint32 misses)
{
    deck.Z += HOP_LIFT_YARDS;
    PassengerMotion motion = Intent(*bot, transport, MotionKind::Board, deck);
    motion.ContactOnly = true;
    bot->GetMotionMaster()->Clear();
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    if (bot->TeleportTo(bot->GetMapId(), deck.X, deck.Y, deck.Z, bot->GetOrientation()))
    {
        data.passengerMotion = motion;
        EventLog::Honesty(bot, TravelLegs::HOP_HONESTY_KIND, TravelLegs::HOP_HONESTY_SOURCE, motion.Entry, misses);
    }
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

// The Travel event sources of an abort ("abort:<reason>"); the trip's abort decision takes the reason
constexpr std::string_view TRAVEL_ABORT_PREFIX = "abort:";

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
bool PassengerOwnsTravel(Player const& bot, NewRpgInfo::Travel const& data)
{
    return data.PassengerOwnsProgress() || bot.GetTransport() || bot.HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) ||
           bot.IsInFlight() || bot.GetVehicle();
}
}  // namespace

bool TravelKeepsOnFoot(PlayerbotAI* botAI)
{
    auto const* const data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (!data)
        return false;
    TravelLegs::Trip const& trip = data->trip;
    if (trip.offFirst.sinceMs)
        return true;
    // A transport leg goes on foot from its dock on (WT54-7: also while it waits there, SeekQuay)
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport;
}

bool NewRpgTravelAction::ConsumeScheduledWork(PlayerbotAI* botAI)
{
    using TravelProgress::Overdue;
    using TravelProgress::Work;
    auto* const data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    Overdue const reason = data ? data->progress.Reason() : Overdue::None;
    bool const lootExpired = botAI->LootInteractionExpired();
    if (reason == Overdue::None && !lootExpired)
        return false;

    Player* const bot = botAI->GetBot();
    bool const walkFailure = reason == Overdue::Walk || reason == Overdue::OffMesh || reason == Overdue::RouteDeadline;
    if (reason != Overdue::None &&
        ((walkFailure && PassengerOwnsTravel(*bot, *data)) || !TransportGeometry::ReadMembership(*bot).Available))
        return false;

    bool const canMove = bot->IsAlive() && !bot->IsInCombat() && botAI->CanMove();
    Work const work = TravelProgress::ScheduledWork(canMove, reason, botAI->HasLootInteraction(), lootExpired);
    if (work == Work::Recover)
        return NewRpgTravelAction(botAI).StepTravel(data, data->progress.TakeReason());
    return work == Work::DeferLoot && botAI->ConsumeLootExpiry();
}

bool NewRpgTravelAction::Execute(Event /*event*/)
{
    auto* const data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (!data || !bot->IsAlive() || bot->IsInCombat() || (!bot->IsInFlight() && !botAI->CanMove()))
        return false;
    if (ConsumeScheduledWork(botAI))
        return true;
    if (botAI->HasLootInteraction() && !PassengerOwnsTravel(*bot, *data))
        return false;
    return StepTravel(data, TravelProgress::Overdue::None);
}

bool NewRpgTravelAction::StepTravel(NewRpgInfo::Travel* data, TravelProgress::Overdue overdue)
{
    using TravelProgress::Overdue;
    auto const membership = TransportGeometry::ReadMembership(*bot);
    if (!membership.Available && !bot->IsInFlight())
        return true;
    // A trip operation requested aboard waits for an acknowledged exit within one period.
    if (data->trip.offFirst.sinceMs)
    {
        TravelLegs::OffStep const step = GetOffFirst(bot, *data);
        if (step == TravelLegs::OffStep::Wait || step == TravelLegs::OffStep::Disembark)
            return true;
        if (step == TravelLegs::OffStep::GiveUp || step == TravelLegs::OffStep::Lost)
        {
            bool const expired = step == TravelLegs::OffStep::GiveUp;
            botAI->TraceTravelInteraction("route_recovery", expired ? "stuck_aboard" : "transport_lost");
            EndTrip(*data, expired ? "stuck aboard" : "transport lost",
                    expired ? "abort:stuck_aboard" : "abort:transport_lost");
            return true;
        }
        TravelLegs::OffFirst const& off = data->trip.offFirst;
        if (off.end == TravelLegs::OffFirstEnd::Arrive)
            return Arrive(*data);
        if (off.end == TravelLegs::OffFirstEnd::Abort)
            EndTrip(*data, off.why, off.source);
        return true;
    }
    Transport* const ridden = bot->GetTransport();
    TravelLegs::Trip const& trip = data->trip;
    // A route may have been superseded aboard. Only the route with this ride or its own boarding result may use it.
    bool const ownsBoard = data->passengerMotion && data->passengerMotion->Kind == MotionKind::Board &&
                           data->passengerMotion->Transport == membership.Guid;
    // Taken aboard by the leg's vessel at the leg's start (its deck docked under the waiting bot) is the boarding
    TravelRoute::Point const standing{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    // or on the walk to that leg's start (the walk is done, WT54-6); not by an elevator or the tram
    bool const takenAboard =
        ridden && (TravelLegs::TakenAboard(trip, standing, membership.Entry) ||
                   (!ridden->IsStaticTransport() && TravelLegs::WalkTakenAboard(trip, standing, membership.Entry)));
    if (ridden && !takenAboard && (!AtTransportLeg(trip) || (!trip.passengerGuid && !ownsBoard)))
    {
        OffFirstBegins(bot, *data);
        return true;
    }
    uint32 const quayLeg = trip.boarding == TravelLegs::Boarding::Approach ? MotionQuayLeg(trip) : TravelLegs::NO_NODE;
    if (overdue == Overdue::RouteDeadline)
    {
        botAI->TraceTravelInteraction("route_recovery", "deadline");
        EndTrip(*data, "deadline", "abort:deadline");
        return true;
    }
    if (overdue == Overdue::Walk || overdue == Overdue::OffMesh)
    {
        if (!data->progress.AttemptedWalk())
        {
            botAI->TraceTravelInteraction("route_recovery", "stalled");
            EndTrip(*data, "stalled", "abort:stalled");
            return true;
        }

        if (data->progress.Owns(bot->movespline->GetId()) && !bot->movespline->Finalized())
            bot->StopMoving();
        bool const offMesh = GiveUpMove(botAI->rpgInfo.moveFarPos);
        if (!offMesh && !data->CurrentLeg() && data->bindInn)
        {
            botAI->TraceTravelInteraction("route_recovery", "stuck");
            return Arrive(*data, true);
        }
        return LegFailed(*data, offMesh ? TravelLegs::FailReason::OffMesh : TravelLegs::FailReason::Stuck);
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
    TravelLegs::Observation seen;
    seen.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    seen.inFlight = bot->IsInFlight();
    seen.money = bot->GetMoney();
    seen.nowMs = getMSTime();
    seen.activeLegMs = data->progress.LegMs();
    seen.aboard = membership.Entry;
    seen.aboardGuid = membership.Guid;
    seen.membershipAvailable = membership.Available;
    seen.boardMotion = ownsBoard;
    seen.passengerOutcome = TakePassengerOutcome(*bot, *data);
    seen.quayLeg = quayLeg;
    SeekQuay(bot, *data, seen);
    // The transport scan (WT19): once a TRANSPORT_SCAN_MS while a transport leg is past its approach, on the bot's
    // map only. The deck point and the Transport* live for this tick only
    std::optional<Point> deck;
    Transport* transport = nullptr;
    float moved = -1.0f;  // the transport's yards since the scan before on this map; -1 none
    if (AtTransportLeg(trip) && getMSTimeDiff(data->transportScanMs, seen.nowMs) >= TravelLegs::TRANSPORT_SCAN_MS)
    {
        data->transportScanMs = seen.nowMs;
        TravelLegs::TransportSight& sight = seen.transport;
        sight.scanned = true;
        uint64 const expected = data->passengerMotion ? data->passengerMotion->Transport : trip.sight.guid;
        transport = FindLegTransport(bot, trip.legs[trip.legIdx], expected);
        if (transport)
        {
            moved = SeeTransport(bot, transport, trip, sight);
            TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
            if (!ridden && !sight.moving && !data->passengerMotion &&
                std::fabs(seen.bot.z - TravelLegs::WaitPoint(trip, leg).z) <= TravelLifecycle::DOCK_FLOOR_YARDS)
            {
                Point const from = BoardStart(*bot, trip);
                deck = TransportGeometry::GroundGeometry{*bot, transport}.DeckFrom(from);
                if (deck)
                {
                    seen.boardingReady = std::fabs(deck->Z - from.Z) <= TravelLifecycle::DOCK_FLOOR_YARDS &&
                                         TransportGeometry::GroundGeometry{*bot}.OnGround(from);
                    if (seen.boardingReady && TravelLegs::WantsSightCheck(trip) && !bot->IsMounted() &&
                        ReadyForTransportMove(*bot))
                        sight.unseen = !sRandomPlayerbotMgr.IsTeleportSeen(
                            bot, WorldPosition(bot->GetMapId(), deck->X, deck->Y, deck->Z));
                }
            }
        }
    }
    TravelLegs::NoteTick(data->trip, bot->IsMounted());
    TravelLegs::LegRecord const recordBefore = trip.record;  // Advance starts a new leg's record
    uint32 const legBefore = trip.legIdx;
    uint32 const legStartBefore = trip.legStartMs;
    uint32 const waypointBefore = trip.next;
    bool const wasWalk = data->CurrentLeg() && data->CurrentLeg()->type == TravelRoute::LegType::Walk &&
                         !TravelLegs::IsNoOp(*data->CurrentLeg());
    TravelLegs::Decision const next = TravelLegs::Advance(data->trip, seen, GraphNodeAt);
    bool const legChanged = trip.legIdx != legBefore;
    bool const waypointReached =
        wasWalk && (legChanged || trip.next > waypointBefore) && data->progress.WaypointReached();
    if (waypointReached)
        botAI->TraceTravelInteraction("route_progress", "waypoint", ObjectGuid::Empty, 0, "none", 0.0f,
                                      std::pair{legBefore, legStartBefore});
    if (legChanged)
        data->progress.StartLeg(!wasWalk || waypointReached);
    if (data->passengerMotion &&
        (trip.legIdx != legBefore || (next.act != TravelLegs::Act::Board && next.act != TravelLegs::Act::Disembark)))
        TransportGeometry::DiscardMotion(*bot, data->passengerMotion);
    LogMisses(bot, trip);
    SendTransportNotes(bot, data->trip);
    if (seen.transport.scanned && ridden && trip.legIdx == legBefore && trip.boarding == TravelLegs::Boarding::Ride &&
        sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        LogAboardScan(bot, trip, moved);
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
            if (trip.legIdx != legBefore && seen.quayLeg != trip.legIdx && MotionQuayLeg(trip) != TravelLegs::NO_NODE)
                return true;  // the new approach gets a quay observation before walking
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
        case TravelLegs::Act::Board:
            if (transport && deck && seen.boardingReady && !data->passengerMotion && PrepareTransportMove(*bot))
            {
                Point const from = BoardStart(*bot, data->trip);
                if (TravelLifecycle::AcceptBoard(TransportGeometry::GroundGeometry{*bot, transport}, from, *deck))
                {
                    LogBoardMove(bot, transport->GetEntry(), *deck);
                    IssueTransportMove(*bot, *data, Intent(*bot, *transport, MotionKind::Board, *deck), {from});
                    if (data->passengerMotion)
                        data->trip.boardCrossing = TravelLegs::StoreCrossing(
                            transport->GetGUID().GetRawValue(), TransportGeometry::PoseOf(*transport), *deck, from);
                }
                else
                    LogBoardGate(bot, *data, *transport, deck, true, true);
            }
            else if (transport && !data->passengerMotion && !seen.transport.moving)
                LogBoardGate(bot, *data, *transport, deck, seen.boardingReady, false);
            return true;
        case TravelLegs::Act::Hop:
            data->trip.boardCrossing = {};  // a hop is no crossing to walk back
            if (transport && deck)
                HopOnto(bot, *data, *transport, *deck, trip.misses);
            return true;
        case TravelLegs::Act::Disembark:
            if (ridden && seen.transport.scanned && seen.transport.guid == membership.Guid)
            {
                Point const dest{data->dest.GetPositionX(), data->dest.GetPositionY(), data->dest.GetPositionZ()};
                if (trip.boarding == TravelLegs::Boarding::Back)  // the quay it waited on
                    WalkOff(*bot, *data, *ridden,
                            std::vector<Point>{Point{next.target.x, next.target.y, next.target.z}});
                else
                    WalkOff(*bot, *data, *ridden,
                            OnwardRoute(trip, bot->GetMap(), bot->GetMapId(), dest, bot->GetPositionZ()));
            }
            return true;
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
    // Transports run (WT19; a timed-out transport leg's link is left out through avoidEdges). Flights only to
    // discovered nodes (the snapshot's taxi mask), their fares within the purse (the search's fare budget). The
    // hearthstone is a leg while its stone is in the bags and it is ready or cooling for at most the wait cap (the
    // search); not again after a hearth leg of this trip failed
    snapshot.hasHearthstone =
        snapshot.hasHearthstone && !data.noHearth && TestConstraints::HearthAllowed(botAI->rpgInfo.constraints);
    snapshot.allowTransport = true;
    TravelRoute::Request request;
    request.snapshot = snapshot;
    request.goal = {data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ()};
    // The trip's stuck walks: their start or goal hops are left out (TravelLegs::NoteStuck)
    request.avoidNodes = data.trip.avoid;
    request.noDirect = data.trip.noDirect;
    // The walks this bot died on twice (TravelLegs::NoteDeath): their graph edges are left out, for its trip only
    request.avoidEdges = data.trip.avoidEdges;
    TravelLegs::RouteWait const waiting{snapshot.requestId, snapshot.position.mapId, snapshot.takenMs, true,
                                        snapshot.position};
    auto const submit = [&]
    {
        return TravelRoute::RouteService::Instance().Submit(bot->GetGUID().GetRawValue(), request,
                                                            TravelRoute::ConfiguredParams());
    };
    if (!TravelProgress::SubmitRoute(data.route, waiting, submit))
    {
        EndTrip(data, "no route service", "abort:no_service");  // the graph is not loaded yet: never wait
        return true;
    }
    botAI->TraceTravelInteraction("route_request", "issued");
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
        botAI->TraceTravelInteraction("route_recovery", "resubmit");
        LOG_DEBUG("playerbots", "[Route] {} {} ({} ms since the request), asked again", bot->GetName(), why,
                  now - data.route.requestedMs);
        return RequestRoute(data);
    };
    if (!answer)
        return TravelLegs::WaitExpired(data.route, now) ? askAgain("got no answer") : true;
    TravelRoute::Point const current{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    switch (TravelLegs::Judge(data.route, answer->requestId, current, now))
    {
        case TravelLegs::Verdict::Drop:
            return TravelLegs::WaitExpired(data.route, now) ? askAgain("got only a stale answer") : true;
        case TravelLegs::Verdict::Refresh:
            return askAgain(bot->GetMapId() != data.route.mapId ? "changed maps" : "moved or took the answer too late");
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
    data.progress.AcceptRoute();
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
    botAI->TraceTravelInteraction("route_recovery", TravelLegs::FailWord(reason));
    TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
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
    data.progress.Recover();
    data.failedMs = std::max<uint32>(getMSTime(), 1);  // 0 = no failed leg
    return true;
}

bool NewRpgTravelAction::Arrive(NewRpgInfo::Travel& data, bool stuck)
{
    // Not on a ship's deck: the bot gets off first, then arrives (WT19-d)
    if (OffFirstBegins(bot, data, TravelLegs::OffFirstEnd::Arrive))
        return true;
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    uint32 const replans = data.trip.replans;
    if (data.bindInn)
    {
        // The route ends in front of the innkeeper (InnInfo::loc). Walk until close enough to talk to him, then bind
        // directly (not through the "home" action, which needs a cleared target and a cached npc list); a failed bind
        // is tried again a few times, a stuck bot out of reach gives up
        float const yards =
            bot->GetExactDist(data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ());
        WorldTravel::InnStep const step = WorldTravel::InnNext(yards, stuck, data.bindTries);
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

void NewRpgTravelAction::EndTrip(NewRpgInfo::Travel& data, char const* why, char const* source)
{
    // Not on a ship's deck: the bot gets off first, then the trip ends (WT19-d)
    if (OffFirstBegins(bot, data, TravelLegs::OffFirstEnd::Abort, why, source))
        return;
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
