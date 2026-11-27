/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "BotTravel.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "ClassQuestCompletion.h"
#include "Creature.h"
#include "EventLog.h"
#include "Formulas.h"
#include "G3D/Vector2.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GridTerrainData.h"
#include "Group.h"
#include "HandInRoomRules.h"
#include "InventorySlots.h"
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
#include "PvpGearMgr.h"
#include "QuestDef.h"
#include "QuestExecRules.h"
#include "QuestFailRecord.h"
#include "QuestFailRows.h"
#include "QuestFieldExec.h"
#include "QuestFields.h"
#include "QuestGiveUp.h"
#include "QuestLogRules.h"
#include "QuestPackets.h"
#include "QuestPlaceRules.h"
#include "QuestingConfig.h"
#include "Random.h"
#include "StatsWeightCalculator.h"
#include "TestConstraints.h"
#include "TestProbe.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "World.h"
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

// questing WQ19a: the open needs of the bot's log (QuestFields::Needs over its slots, at neutral prices), each item
// objective's credits left read from the log (a slot holds only the creature and object counters). WQ38: as the
// bot's group sees them (a lone bot works no elite, group or instance quest). WQ34: a given-up quest gives none
std::vector<Planner::QuestFields::Need> LogNeeds(Player* bot, Planner::QuestIndex const& index)
{
    std::vector<Planner::QuestSlot> slots;
    slots.reserve(bot->GetQuestLogSize());
    Planner::FillSlots(bot, slots);
    Planner::QuestFields::Pricing const pricing{};
    Group const* group = bot->GetGroup();
    std::shared_ptr<Planner::QuestLog::Record const> const record =
        NewRpgHelpers::OrderRecord(GET_PLAYERBOT_AI(bot), bot);
    std::vector<Planner::QuestFields::Need> needs =
        Planner::QuestFields::Needs(index, slots, static_cast<uint8>(bot->GetTeamId()), pricing,
                                    group ? group->GetMembersCount() : 1, record->givenUp);
    for (Planner::QuestFields::Need& need : needs)
    {
        if (need.objective == Planner::QuestFields::HAND_IN || need.objective == Planner::QuestFields::TAKE)
            continue;
        Planner::QuestRecord const* record = index.Find(need.quest);
        Quest const* quest = sObjectMgr->GetQuestTemplate(need.quest);
        auto const it = bot->getQuestStatusMap().find(need.quest);
        if (!record || !quest || it == bot->getQuestStatusMap().end())
            continue;
        int32 const item = Planner::QuestExec::ObjectiveAt(*record, need.objective).poiIndex - QUEST_OBJECTIVES_COUNT;
        if (item < 0 || item >= QUEST_ITEM_OBJECTIVES_COUNT || !quest->RequiredItemId[item])
            continue;
        uint32 const have = it->second.ItemCount[item];
        uint32 const wanted = quest->RequiredItemCount[item];
        need.left = have >= wanted ? 0 : wanted - have;
    }
    return needs;
}

// Whether the bot can still take a quest an order expects it to take (questing WQ18b): not in its log, not handed
// in, and not given up (QuestLogRules' Read)
bool Takeable(Player* bot, Planner::QuestLog::Record const& record, uint32 questId)
{
    return bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && !bot->GetQuestRewardStatus(questId) &&
           !std::binary_search(record.givenUp.begin(), record.givenUp.end(), questId);
}

// The order's count of the log (at its start and after each credit): the open needs, and what the looks seek for
// them: every source of an objective's need (hand-ins and takes are steps WQ19b's and WQ31's)
void CountOrder(Player* bot, Planner::QuestOrderRun& run, std::vector<Planner::QuestFields::Need> const& needs)
{
    std::vector<Planner::QuestOrderOpen> const before = std::move(run.open);
    run.open = Planner::QuestFieldExec::Open(needs);
    // The takes the order expects (questing WQ18b), after the log's needs: the seeks index those
    std::shared_ptr<Planner::QuestLog::Record const> const record = NewRpgHelpers::OrderRecord(bot, run);
    Planner::QuestFieldExec::OpenTakes(run.open, run.order,
                                       [&](uint32 questId) { return Takeable(bot, *record, questId); });
    // questing WQ33: a need's refused stand points stay counted until it gains a credit
    Planner::QuestFieldExec::CarryRefusals(before, run.open);
    run.seeks.clear();
    uint32 at = 0;
    for (Planner::QuestFields::Need const& need : needs)
    {
        if (!need.left)
            continue;
        if (need.objective != Planner::QuestFields::HAND_IN && need.objective != Planner::QuestFields::TAKE)
            for (Planner::QuestFields::NeedSource const& source : need.sources)
                run.seeks.push_back({source.entry, at, source.kind == Planner::FieldKind::Object});
        ++at;
    }
    run.lastCount = Planner::QuestFieldExec::Stamp(getMSTime());
}

// questing WQ18b part 3: the spots of an order's expected hand-ins and takes on the bot's map: the enders and givers
// the quest index has for them, none hostile to the bot's side or outside the default phase
std::vector<Planner::QuestFieldExec::NpcSpot> OrderSpots(Player* bot, Planner::QuestOrder const& order,
                                                         Planner::QuestIndex const& index)
{
    std::vector<Planner::QuestFieldExec::NpcSpot> spots;
    uint32 const side = 1u << static_cast<uint8>(bot->GetTeamId());
    for (Planner::QuestOrderNeed const& need : order.expected)
    {
        bool const handIn = need.objective == Planner::QuestFields::HAND_IN;
        Planner::QuestRecord const* const record = index.Find(need.quest);
        if (!record || (!handIn && need.objective != Planner::QuestFields::TAKE))
            continue;
        for (uint32 const at : handIn ? record->enders : record->givers)
        {
            if (at >= index.places.size())
                continue;
            Planner::QuestPlace const& place = index.places[at];
            if (place.mapId == bot->GetMapId() && !place.otherPhase && !(place.hostileTo & side))
                spots.push_back({need.quest, need.objective, place.x, place.y, place.z});
        }
    }
    return spots;
}

constexpr float ORDER_FIELD_ROAM_YARDS = 40.0f;  // round the place as round an objective's widest area
constexpr float ORDER_NPC_ROAM_YARDS = 5.0f;     // at its NPC the bot stays beside it

// An order's place on the bot's map: its point on the ground (terrain, no buildings: a field lies in the open)
WorldPosition OrderPlace(Player* bot, Planner::QuestOrder const& order)
{
    float z = bot->GetMap()->GetHeight(bot->GetPhaseMask(), order.x, order.y, MAX_HEIGHT, false);
    if (z <= INVALID_HEIGHT)
        z = bot->GetPositionZ();
    return WorldPosition(bot->GetMapId(), order.x, order.y, z);
}

// questing WQ18b parts 3 and 4: an order walks to the NPC of an expected hand-in or take that is open
// (QuestFieldExec::AimOf: hand-ins first, the nearest), whatever its word, and a field order with none to its place:
// the giver search reaches less far than a field is wide. Asked at the order's start and after each count of the log,
// so the bot goes from one NPC to the next and then to the field. A new aim is a new walk: the arrival is taken again
// there, by the straight walk to it
void AimOrder(Player* bot, NewRpgInfo::DoQuest& data)
{
    constexpr float SAME_SPOT_YARDS = 1.0f;  // the aim the order already walks to
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index || !data.order)
        return;
    Planner::QuestOrderRun& run = *data.order;
    std::vector<Planner::QuestFieldExec::NpcSpot> const spots = OrderSpots(bot, run.order, *index);
    std::optional<Planner::QuestFieldExec::OrderAim> const aim =
        Planner::QuestFieldExec::AimOf(run.order, run.open, spots, bot->GetPositionX(), bot->GetPositionY());
    if (!aim)
        return;
    data.roam = aim->npc ? ORDER_NPC_ROAM_YARDS : ORDER_FIELD_ROAM_YARDS;
    if (data.pos.GetMapId() == bot->GetMapId() && data.pos.GetExactDist2d(aim->x, aim->y) <= SAME_SPOT_YARDS)
        return;
    data.pos = aim->npc ? WorldPosition(bot->GetMapId(), aim->x, aim->y, aim->z) : OrderPlace(bot, run.order);
    run.arrivedAt = 0;
    run.closerAt = 0;  // the new aim's distances start anew (QuestFieldExec::NoteCloser)
    run.bestYards = 0.0f;
    run.order.walkMinutes = bot->GetExactDist2d(aim->x, aim->y) / std::max(bot->GetSpeed(MOVE_RUN), 1.0f) / 60.0f;
}

// Every quest of the log at its progress (QuestFieldExec::Note): the credits since the marks were last taken, and
// whether a quest came into the log
uint32 NoteLog(Player* bot, std::vector<Planner::QuestOrderMark>& marks, bool& added)
{
    uint32 rise = 0;
    for (uint16 slot = 0; slot < bot->GetQuestLogSize(); ++slot)
        if (uint32 const questId = bot->GetQuestSlotQuestId(slot))
        {
            std::size_t const before = marks.size();
            rise += Planner::QuestFieldExec::Note(marks, questId, QuestProgress(bot, questId));
            added = added || marks.size() != before;
        }
    return rise;
}

// The order's tick before anything else (Execute; questing WQ19a): its credits since the last tick (place or way, by
// the arrival), which restart the dry clock. A credit, a quest new in the log or one that left it counts the log
// again; a quest whose objectives that count finds met (or already handed in) writes its QuestReady row here, as the
// one-quest executor does when it finds its quest complete
void NoteOrderCredits(PlayerbotAI* botAI, Player* bot, NewRpgInfo::DoQuest& data)
{
    Planner::QuestOrderRun& run = *data.order;
    bool changed = false;
    std::size_t const known = run.marks.size();
    uint32 const rise = NoteLog(bot, run.marks, changed);
    if (rise)
    {
        (run.arrivedAt ? run.placeCredits : run.wayCredits) += rise;
        run.lastFound = Planner::QuestFieldExec::Stamp(getMSTime());
    }
    // A quest taken on the order (questing WQ31, WQ18b): the place gave something, so the dry clock restarts; its
    // take need closes at the count below
    if (changed)
    {
        run.takes = static_cast<uint16>(std::min<std::size_t>(run.takes + (run.marks.size() - known), 0xFFFF));
        run.lastFound = Planner::QuestFieldExec::Stamp(getMSTime());
    }
    // A ready quest handed in on the order (the giver search, questing WQ19b): its hand-in need closes, the dry clock
    // restarts and the log is counted again
    auto const rewarded = [bot](uint32 questId) { return bot->GetQuestRewardStatus(questId); };
    changed = Planner::QuestFieldExec::NoteHandIns(run, getMSTime(), rewarded) > 0 || changed;
    std::vector<uint32> working;
    for (Planner::QuestOrderOpen const& open : run.open)
        if (open.objective != Planner::QuestFields::HAND_IN && open.objective != Planner::QuestFields::TAKE)
            working.push_back(open.quest);
    std::sort(working.begin(), working.end());
    working.erase(std::unique(working.begin(), working.end()), working.end());
    // A quest worked turned complete or left the log; a ready quest to hand in left it without its reward (WQ43, L2)
    auto const worked = [bot](uint32 questId)
    {
        QuestStatus const status = bot->GetQuestStatus(questId);
        return status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_FAILED;
    };
    auto const gone = [bot](uint32 questId) { return bot->GetQuestStatus(questId) == QUEST_STATUS_NONE; };
    changed = changed || Planner::QuestFieldExec::CountAgain(run.open, worked, gone);
    if (!rise && !changed)
        return;
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index)
        return;
    CountOrder(bot, run, LogNeeds(bot, *index));
    AimOrder(bot, data);  // an order at NPCs: on to its next NPC with something open
    for (uint32 const questId : working)
    {
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
            continue;
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
            BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->RecordQuestCompleted(questId);
        EventLog::Emit(bot, EventLog::EventType::QuestReady, "executor", questId, 0);
    }
}

// questing WQ38: the stall sum (QuestFailRows.h) of each quest the order expects an open objective of, once per quest
// and tick, after the tick's credits are noted. The sums live with the bot's goals, so they outlive the order; a quest
// gone from the log is forgotten. A sum that reaches 10 minutes (2 under shortgiveup) writes QuestStall: src the
// order's word, id the quest, amount the minutes and the order's cell (QuestFieldExec::StallRowAmount). O(expected
// needs x open needs)
void TickStall(PlayerbotAI* botAI, Player* bot, Planner::QuestOrderRun const& run)
{
    std::vector<uint32> worked;
    for (Planner::QuestOrderNeed const& need : run.order.expected)
        if (need.objective != Planner::QuestFields::HAND_IN && need.objective != Planner::QuestFields::TAKE &&
            std::any_of(run.open.begin(), run.open.end(), [&need](Planner::QuestOrderOpen const& open)
                        { return open.quest == need.quest && open.objective == need.objective && open.left; }))
            worked.push_back(need.quest);
    Planner::QuestFailRows::Sums& sums = Planner::QuestStallsOf(botAI, bot);
    if (worked.empty() && sums.quests.empty())
        return;
    Planner::QuestFailRows::KeepLogged(
        sums, [bot](uint32 questId) { return bot->FindQuestSlot(questId) < bot->GetQuestLogSize(); });
    std::sort(worked.begin(), worked.end());
    worked.erase(std::unique(worked.begin(), worked.end()), worked.end());
    uint32 const now = getMSTime();
    uint32 const limit = Planner::QuestFailRows::LimitMs(TestConstraints::GiveUpShortened(botAI->testConstraints));
    for (uint32 const questId : worked)
    {
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;
        if (std::optional<Planner::QuestFailRows::Row> const row =
                Planner::QuestFailRows::Tick(sums, questId, QuestProgress(bot, questId), now, limit))
            EventLog::Emit(bot, EventLog::EventType::QuestStall, Planner::QuestOrderWordName(run.order.word), questId,
                           Planner::QuestFieldExec::StallRowAmount(row->minutes, run.order.cell));
    }
}
}  // namespace

void QuestGiveUp::Noted(PlayerbotAI* botAI, NewRpgInfo::DoQuest const& data, char const* cause)
{
    Player* const bot = botAI->GetBot();
    if (!bot)
        return;
    // An order gives up its failed hand-ins (no_ender, questing WQ19b): one row for each expected ready quest still in
    // the log, none for one handed in; and, when a use really failed (WQ19a: the item missing, the use refused), the
    // quest of the need it worked. Pursuit::WritesFailure lets no other end of an order in
    if (data.order)
    {
        std::vector<uint32> quests;
        if (cause && std::string_view(cause) == Pursuit::NO_ENDER)
            quests = Planner::QuestFieldExec::NoEnderQuests(*data.order);
        else if (data.questId)
            quests.push_back(data.questId);
        for (uint32 const questId : quests)
        {
            NewRpgInfo::DoQuest one;
            one.questId = questId;
            Noted(botAI, one, cause);
        }
        return;
    }
    if (!data.questId)
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
    EventLog::Emit(bot, EventLog::EventType::QuestFail, cause, data.questId, minutes);
    QuestFailRecord::Note(bot, data.questId, cause, minutes);
    // A take the game refused (cannot_take) was never in the log: no abandon in the quest statistics (WQ43, L8)
    Pursuit::AbandonCount const counted = Pursuit::AbandonOf(cause);
    if (counted != Pursuit::AbandonCount::None)
        botAI->RecordQuestAbandoned(data.questId, counted == Pursuit::AbandonCount::CannotTurnIn
                                                      ? QuestAbandonReason::CannotTurnIn
                                                      : QuestAbandonReason::NoProgress);
    LOG_DEBUG("playerbots", "[Quest] {} gives quest {} up: {} after {} min", bot->GetName(), data.questId, cause,
              minutes);
    // Given up twice, a doable class quest or a step of its chain is failed: completed now (upkeep WU21a, WU.29)
    Planner::ClassQuestCompletion::Request(bot, Planner::ClassQuests::When::Check);
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

    // The order's give-up clock runs while its executor runs (questing WT35c). An order (questing WQ19a) has none:
    // it counts its credits instead, before the giver search can hand a quest in
    if (auto* doing = std::get_if<NewRpgInfo::DoQuest>(&info.data))
    {
        if (doing->order)
        {
            NoteOrderCredits(botAI, bot, *doing);
            // AimOrder may have ended the status: the order is read again
            if (auto* still = std::get_if<NewRpgInfo::DoQuest>(&info.data); still && still->order)
                TickStall(botAI, bot, *still->order);
        }
        else if (char const* const cause = TickPursuit(botAI, bot, *doing))
        {
            RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = cause});
            return true;
        }
    }

    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (data.order)
        return DoOrder(data);
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
// The world log's word of the one drop's reason (questing WQ34): grey is upstream's trivial
QuestDropReason DropReasonOf(Planner::QuestLog::Drop drop)
{
    switch (drop)
    {
        case Planner::QuestLog::Drop::Grey:
            return QuestDropReason::Trivial;
        case Planner::QuestLog::Drop::HolidayEnded:
            return QuestDropReason::HolidayEnded;
        default:
            return QuestDropReason::Failed;
    }
}

// The log's drop of a quest slot through the abandon packet, as a player would; its QuestAbandon row names the reason
void DropQuestSlot(Player* bot, uint16 slot, QuestDropReason reason)
{
    // The packet names its slot in 8 bits: a quest in a later slot of a log without a limit changes places first
    if (slot > std::numeric_limits<uint8>::max())
    {
        bot->SwapQuestSlot(slot, 0);
        slot = 0;
    }
    WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
    packet << static_cast<uint8>(slot);
    WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
    removeQuest.Read();
    WorldLog::QuestDropWord const word(QuestDropReasonName(reason), bot, bot->GetQuestSlotQuestId(slot));
    bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
}

// A bot the log's rule covers (NewRpgHelpers::KeepsLogRule) abandons only what QuestLogRules' Abandon says: a quest
// that is not complete and grey by the game's line, whose holiday has ended or that the game marks failed; never for
// a give-up, for what the executor cannot do, for a failed hand-in or for room (questing WQ34, WQ.46 item 4). Runs at
// every tick of the giver search: one pass over the log, no record and no database
bool OrganizeLogByRule(PlayerbotAI* botAI, Player* bot)
{
    int32 const grayLevel = Acore::XP::GetGrayLevel(bot->GetLevel());
    bool dropped = false;
    for (uint16 i = 0; i < bot->GetQuestLogSize(); ++i)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(i);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;
        Planner::QuestLog::Drop const drop = Planner::QuestLog::Abandon(NewRpgHelpers::LogHeld(bot, quest), grayLevel);
        if (drop == Planner::QuestLog::Drop::Keep)
            continue;
        QuestDropReason const reason = DropReasonOf(drop);
        LOG_DEBUG("playerbots", "[New RPG] {} drops quest {}: {}", bot->GetName(), questId,
                  Planner::QuestLog::Word(drop));
        DropQuestSlot(bot, i, reason);
        botAI->RecordQuestDropped(questId, reason);
        dropped = true;
    }
    return dropped;
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

    // A bot the log's rule covers takes what the one accept allows (questing WQ34). A planned take it refuses stays
    // open, and the order's end at this giver gives it up (QuestGiveUp::OrderEnded, WQ18c)
    bool const onOrder = NewRpgHelpers::OnOrder(botAI);
    bool const byRule = NewRpgHelpers::KeepsLogRule(botAI);
    NewRpgHelpers::LogAccept take(bot);
    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        QuestMenuItem const& item = menu.GetItem(idx);
        Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        QuestStatus const& status = bot->GetQuestStatus(item.QuestId);
        bool accept = false;
        if (status == QUEST_STATUS_NONE && TestConstraints::QuestChoicesAllowed(botAI->testConstraints))
        {
            if (!byRule)
                accept = NewRpgHelpers::FreeBotTakes(bot, quest);
            else  // the accept asks the core's CanTakeQuest (WQ41); CanAddQuest has the last word
                accept = take.Allows(quest) && bot->CanAddQuest(quest, false);
        }
        if (accept)
        {
            AcceptQuest(quest, guid);
            if (onOrder)
                Planner::NotePickup(botAI, true, true);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->RecordQuestAccepted(quest->GetQuestId());
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && MakeRoomForHandIn(quest))
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
    // An order writes its quests' Ready rows itself (NoteOrderCredits, questing WQ19a)
    if (auto const* doing = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
        doing && !doing->order && doing->questId == questID && doing->objectiveIdx != -1)
        EventLog::Emit(bot, EventLog::EventType::QuestReady, "executor", questID, 0);

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

namespace
{
constexpr uint32 HAND_IN_ROOM_HEARTHSTONE = 6948;
constexpr uint32 HAND_IN_ROOM_MANA_GEMS = 4;  // ItemLimitCategory.dbc: every mana gem (LootAction's NR6)

// Whether a quest in the log, open or ready, asks for the item or drops it as its source. Not the core's
// HasQuestForItem(id, 0, true): its turn-in branch answers yes for every item once any quest is ready
bool LogWantsItem(Player* bot, uint32 itemId)
{
    for (uint16 i = 0; i < bot->GetQuestLogSize(); ++i)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(i);
        Quest const* const quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        QuestStatus const status = quest ? bot->GetQuestStatus(questId) : QUEST_STATUS_NONE;
        if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
            continue;
        for (uint8 j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
            if (quest->RequiredItemId[j] == itemId && quest->RequiredItemCount[j])
                return true;
        for (uint8 j = 0; j < QUEST_SOURCE_ITEM_IDS_COUNT; ++j)
            if (quest->ItemDrop[j] == itemId)
                return true;
    }
    return false;
}

// questing WQ43: a stack the room before a hand-in never destroys: a quest item, a worn item, a bag, the hearthstone,
// a profession's tool, a key, a conjured thing the bot uses (its caster conjures it again; mana gems carry no conjured
// flag), and a thing without vendor value but an unusable conjured one (often one the bot cannot get back)
bool KeptAtHandIn(Player* bot, Item const* item)
{
    ItemTemplate const* const proto = item->GetTemplate();
    bool const conjured = proto->IsConjuredConsumable() || proto->ItemLimitCategory == HAND_IN_ROOM_MANA_GEMS;
    bool const tool = proto->TotemCategory != 0 ||
                      (proto->Class == ITEM_CLASS_WEAPON && (proto->SubClass == ITEM_SUBCLASS_WEAPON_MISC ||
                                                             proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE));
    bool const questItem = proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || proto->StartQuest ||
                           LogWantsItem(bot, proto->ItemId);
    bool const usable = bot->CanUseItem(proto) == EQUIP_ERR_OK;
    return questItem || tool || proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER ||
           proto->Class == ITEM_CLASS_KEY || proto->ItemId == HAND_IN_ROOM_HEARTHSTONE ||
           PvpGearMgr::Instance().IsWornItem(bot, item) || (conjured && usable) ||
           (proto->SellPrice == 0 && !(conjured && !usable));
}
}  // namespace

// questing WQ43 (WQ.49): room before a hand-in (HandInRoomRules.h). Run only at a hand-in the game refuses; a bot of
// the living world (KeepsLogRule) whose bags alone refuse the reward destroys the cheapest stacks it may lose in its
// general slots until the reward's items fit, one Inventory destroyed_for_hand_in row each (id the item, amount the
// stack's vendor value). Nothing goes when the stacks that may go cannot make the room: the order then ends `bags`
bool NewRpgBaseAction::MakeRoomForHandIn(Quest const* quest)
{
    uint32 const choice = quest->GetRewChoiceItemsCount() > 1 ? BestRewardIndex(quest) : 0;
    if (bot->CanRewardQuest(quest, choice, false))
        return true;
    // A case bot's trace: why a full bot's hand-in makes room or not
    if (TestProbe::On(botAI->testConstraints))
    {
        auto const store = [this](uint32 itemId, uint32 count)
        {
            ItemPosCountVec dest;
            return itemId ? static_cast<int>(bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count)) : -1;
        };
        LOG_INFO("playerbots",
                 "[Probe] {} hand-in room quest {} choice {} rewardable {} log rule {} destroys {} refuses {} "
                 "store choice {} fixed {} {} {} {}, free {}",
                 bot->GetName(), quest->GetQuestId(), choice, bot->CanRewardQuest(quest, false),
                 NewRpgHelpers::KeepsLogRule(botAI), TestConstraints::DestroysForSpace(botAI->testConstraints),
                 NewRpgHelpers::RoomRefusesReward(bot, quest, choice),
                 choice < QUEST_REWARD_CHOICES_COUNT
                     ? store(quest->RewardChoiceItemId[choice], quest->RewardChoiceItemCount[choice])
                     : -2,
                 store(quest->RewardItemId[0], quest->RewardItemIdCount[0]),
                 store(quest->RewardItemId[1], quest->RewardItemIdCount[1]),
                 store(quest->RewardItemId[2], quest->RewardItemIdCount[2]),
                 store(quest->RewardItemId[3], quest->RewardItemIdCount[3]), WorldTravel::FreeGeneralSlots(bot));
    }
    if (!NewRpgHelpers::HandInRewardable(bot, quest, choice, botAI->testConstraints))
        return false;
    uint32 needed = 0;
    auto const takes = [this, &needed](uint32 itemId, uint32 count)
    {
        ItemTemplate const* const proto = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
        if (proto)
            needed += Planner::HandInRoom::SlotsFor(count, proto->GetMaxStackSize(), bot->GetItemCount(itemId, false));
    };
    if (quest->GetRewChoiceItemsCount() > 0 && choice < QUEST_REWARD_CHOICES_COUNT)
        takes(quest->RewardChoiceItemId[choice], quest->RewardChoiceItemCount[choice]);
    for (uint32 i = 0; i < QUEST_REWARDS_COUNT; ++i)
        takes(quest->RewardItemId[i], quest->RewardItemIdCount[i]);
    // The game said the bags are full: at least one slot is short, whatever the count of stacks found
    uint32 const slotsShort =
        std::max<uint32>(1, Planner::HandInRoom::SlotsShort(needed, WorldTravel::FreeGeneralSlots(bot)));

    std::vector<Item*> items;
    std::vector<Planner::HandInRoom::Stack> stacks;
    auto const visit = [this, &items, &stacks](Item* item)
    {
        if (!item)
            return;
        items.push_back(item);
        stacks.push_back(
            {static_cast<uint64>(item->GetTemplate()->SellPrice) * item->GetCount(), KeptAtHandIn(bot, item)});
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        visit(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* const bag = bot->GetBagByPos(bagSlot); WorldTravel::IsGeneralBag(bag))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                visit(bag->GetItemByPos(slot));

    std::vector<std::size_t> const victims = Planner::HandInRoom::Victims(stacks, slotsShort);
    if (TestProbe::On(botAI->testConstraints))
    {
        std::size_t const kept =
            std::count_if(stacks.begin(), stacks.end(), [](Planner::HandInRoom::Stack const& s) { return s.kept; });
        LOG_INFO("playerbots", "[Probe] {} hand-in room needed {} short {} stacks {} kept {} victims {}",
                 bot->GetName(), needed, slotsShort, stacks.size(), kept, victims.size());
    }
    for (std::size_t const at : victims)
    {
        Item* const item = items[at];
        uint32 const itemId = item->GetEntry();
        // Destroyed units are no use: the use measure does not count them (NE8a)
        uint16 const nonUse = Planner::NonUseKindsOf(botAI, item->GetTemplate());
        uint32 const units = item->GetCount();
        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        Planner::NoteNonUse(botAI, nonUse, units);
        EventLog::Emit(bot, EventLog::EventType::Inventory, "destroyed_for_hand_in", itemId,
                       static_cast<int64>(stacks[at].value));
    }
    if (!victims.empty())
        LOG_DEBUG("playerbots", "[Quest] {} destroyed {} stacks for the reward of quest {}", bot->GetName(),
                  victims.size(), quest->GetQuestId());
    return bot->CanRewardQuest(quest, choice, false);
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

    if (NewRpgHelpers::KeepsLogRule(botAI))
        return OrganizeLogByRule(botAI, bot);

    // A log without a limit drops nothing for room (questing WQ40, WQ.46 item 1): upstream's upkeep below is that of
    // a character with a client's 25 slots
    if (bot->HasUnlimitedQuestLog())
        return false;

    int32 freeSlotNum = 0;

    for (uint16 i = 0; i < bot->GetQuestLogSize(); ++i)
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
    for (uint16 i = 0; i < bot->GetQuestLogSize(); ++i)
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
    for (uint16 i = 0; i < bot->GetQuestLogSize(); ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
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

    // clear quests log
    for (uint16 i = 0; i < bot->GetQuestLogSize(); ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
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
    // A bot on an order: its pickups' plan is due once no giver in reach offers more (questing WQ31)
    if (NewRpgHelpers::OnOrder(botAI))
        Planner::NotePickup(botAI, false, !npcOrGo.IsEmpty());
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
        if (TestProbe::On(botAI->testConstraints))
            LOG_INFO("playerbots", "[Probe] {} giver {} given up: interacts {} at {:.1f} yd", bot->GetName(),
                     object->GetEntry(), bot->CanInteractWithQuestGiver(object), bot->GetDistance(object));
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

namespace
{
// How an order's status starts: the Status row's word, what a running status ends as, and whether the start is
// written to the server log at info level (a test's order; a route's orders only at debug level)
struct OrderStart
{
    char const* word{"order"};
    StatusEnd::Outcome why{StatusEnd::Outcome::Ordered};
    bool traced{true};
};

// An order's start (questing WQ19a field, WQ20 way): the planned walk, the log's needs counted and marked, then
// DoQuest in order mode at the order's point of the bot's map
bool StartOrder(PlayerbotAI* botAI, Player* bot, Planner::QuestOrder order,
                std::vector<Planner::QuestFields::Need> const& needs, char const* word, OrderStart const& start = {})
{
    float const x = order.x;
    float const y = order.y;
    WorldPosition const place = OrderPlace(bot, order);
    Planner::QuestOrderRun run;
    run.order = std::move(order);
    // The planned walk: straight yards at the bot's run speed
    run.order.walkMinutes = bot->GetExactDist2d(x, y) / std::max(bot->GetSpeed(MOVE_RUN), 1.0f) / 60.0f;
    CountOrder(bot, run, needs);
    bool added = false;
    NoteLog(bot, run.marks, added);
    run.startedAt = Planner::QuestFieldExec::Stamp(getMSTime());
    if (start.traced)
    {
        LOG_INFO("playerbots",
                 "[Route] {} {} order at {:.0f} {:.0f} ({:.0f} min, walk {:.1f} min, allowance {:.0f} yd): {} needs "
                 "open, {} expected",
                 bot->GetName(), word, x, y, run.order.minutes, run.order.walkMinutes, run.order.allowanceYards,
                 run.open.size(), run.order.expected.size());
    }
    else
    {
        LOG_DEBUG("playerbots", "[Route] {} {} order of its route at {:.0f} {:.0f}: {} needs open, {} expected",
                  bot->GetName(), word, x, y, run.open.size(), run.order.expected.size());
    }
    botAI->GetTravel().CancelLootInteraction("order");
    NewRpgInfo::DoQuest quest;
    quest.pos = place;
    quest.roam = ORDER_FIELD_ROAM_YARDS;
    quest.ordered = true;
    quest.order = std::move(run);
    AimOrder(bot, quest);  // first to the NPC of an expected hand-in or take (WQ18b parts 3 and 4)
    RpgStatus::Start(botAI, std::move(quest), start.word, start.why);
    return true;
}
}  // namespace

std::function<bool(uint32)> NewRpgInternal::OrderEnderElsewhere(Player* bot, Planner::QuestOrder const& order)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index)
        return [](uint32) { return false; };
    auto const spots =
        std::make_shared<std::vector<Planner::QuestFieldExec::NpcSpot> const>(OrderSpots(bot, order, *index));
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    return [order, spots, x, y](uint32 questId)
    { return Planner::QuestFieldExec::EnderElsewhere(order, *spots, questId, x, y); };
}

bool NewRpgInternal::StartFieldOrder(PlayerbotAI* botAI, Player* bot, float x, float y, float minutes)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    std::shared_ptr<Planner::FieldMap const> const fields = Planner::Fields();
    if (!index || !fields)
    {
        LOG_INFO("playerbots", "[Route] {} field order refused: the quest index or the field map is building",
                 bot->GetName());
        return false;
    }
    std::vector<Planner::QuestFields::Need> const needs = LogNeeds(bot, *index);
    return StartOrder(botAI, bot, Planner::QuestFieldExec::FieldOrder(*fields, needs, bot->GetMapId(), x, y, minutes),
                      needs, "field");
}

bool NewRpgInternal::StartWayOrder(PlayerbotAI* botAI, Player* bot, float x, float y, float allowance)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index)
    {
        LOG_INFO("playerbots", "[Route] {} way order refused: the quest index is building", bot->GetName());
        return false;
    }
    std::vector<Planner::QuestFields::Need> const needs = LogNeeds(bot, *index);
    return StartOrder(botAI, bot, Planner::QuestFieldExec::WayOrder(bot->GetMapId(), x, y, allowance), needs, "way");
}

std::function<bool(Planner::QuestOrderNeed const&)> NewRpgInternal::OrderNeedOpen(PlayerbotAI* botAI, Player* bot)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index)
        return [](Planner::QuestOrderNeed const&) { return false; };
    auto const open = std::make_shared<std::vector<Planner::QuestOrderOpen> const>(
        Planner::QuestFieldExec::Open(LogNeeds(bot, *index)));
    std::shared_ptr<Planner::QuestLog::Record const> const record = NewRpgHelpers::OrderRecord(botAI, bot);
    return [bot, open, record](Planner::QuestOrderNeed const& need)
    {
        return Planner::QuestFieldExec::NeedOpen(*open, need,
                                                 [&](uint32 questId) { return Takeable(bot, *record, questId); });
    };
}

bool NewRpgInternal::StartRouteOrder(PlayerbotAI* botAI, Player* bot, Planner::QuestOrder const& order,
                                     char const* word)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index || order.mapId != bot->GetMapId())
        return false;
    // The planner's start: the Status row's word is its own (a switch, a resume), a running status ends as interrupted
    return StartOrder(botAI, bot, order, LogNeeds(bot, *index), Planner::QuestOrderWordName(order.word),
                      {word, StatusEnd::Outcome::Interrupted, false});
}

void NewRpgInternal::MarkOrderReplaced(PlayerbotAI* botAI, Planner::QuestOrder const& order, bool replaced)
{
    auto* const doing = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (doing && doing->order && Planner::SameOrder(doing->order->order, order))
        doing->order->replaced = replaced;
}

std::optional<Planner::QuestOrderEnd> QuestGiveUp::OrderEnded(PlayerbotAI* botAI, NewRpgInfo::DoQuest const& data,
                                                              StatusEnd::Outcome outcome, char const* word)
{
    Player* const bot = botAI->GetBot();
    if (!bot || !data.order)
        return std::nullopt;
    Planner::QuestOrderRun const& run = *data.order;
    // The order's own ends come with their word and outcome (DoOrder); anything else took the bot over, which the row
    // counts as replaced
    std::optional<Planner::QuestOrderEnd> const own = Planner::QuestFieldExec::OwnEnd(word, outcome);
    Planner::QuestOrderEnd const end = own.value_or(Planner::QuestOrderEnd::Replaced);
    // WQ18c (WQ.44 item 2): the one writer of a refused take. A planned take the order's own end leaves open at its
    // giver is one give-up of the quest (cannot_take) when the take test, the game's answer with it (CanTakeQuest,
    // WQ41), still allows it: the giver did not offer it. The second puts it out for the bot as any cause does. A take
    // the game refuses is none: the plan read an answer that is old now, so the next snapshot asks the game again. A
    // bot the test keeps from choosing quests refused nothing
    bool const givesUp = own && Planner::QuestFieldExec::GivesUpTakes(*own) &&
                         TestConstraints::QuestChoicesAllowed(botAI->testConstraints);
    if (givesUp)
        Planner::AskTakesAgain(botAI);
    if (std::shared_ptr<Planner::QuestIndex const> const index = givesUp ? Planner::Quests() : nullptr)
    {
        std::vector<Planner::QuestFieldExec::NpcSpot> const spots = OrderSpots(bot, run.order, *index);
        NewRpgHelpers::LogAccept take(bot);
        auto const atGiver = [bot, &spots](uint32 questId)
        { return Planner::QuestFieldExec::AtGiver(spots, questId, bot->GetPositionX(), bot->GetPositionY()); };
        auto const mayTake = [&take](uint32 questId)
        {
            Quest const* const quest = sObjectMgr->GetQuestTemplate(questId);
            return quest && take.Allows(quest);
        };
        for (uint32 const questId : Planner::QuestFieldExec::RefusedTakes(run, *own, atGiver, mayTake))
        {
            NewRpgInfo::DoQuest one;
            one.questId = questId;
            Noted(botAI, one, Planner::QuestLog::CANNOT_TAKE.data());
        }
    }
    // The credits since the last tick count too
    std::vector<Planner::QuestOrderMark> marks = run.marks;
    bool added = false;
    uint32 const rise = NoteLog(bot, marks, added);
    uint32 const placeCredits = run.placeCredits + (run.arrivedAt ? rise : 0);
    uint32 const wayCredits = run.wayCredits + (run.arrivedAt ? 0 : rise);
    uint32 const minutes = run.startedAt ? getMSTimeDiff(run.startedAt, getMSTime()) / (MINUTE * IN_MILLISECONDS) : 0;
    uint32 const quests = Planner::QuestFieldExec::QuestsCredited(marks);
    EventLog::Emit(bot, EventLog::EventType::QuestOrder, Planner::QuestFieldExec::RowSource(run.order.word, end),
                   quests, Planner::QuestFieldExec::RowAmount(placeCredits, wayCredits, minutes, run.order.cell));
    // The QuestOrder row holds every field of this line, so it is a debug line (WQ18b part 3: 2.6 % of a lab's log)
    LOG_DEBUG("playerbots", "[Quest] {} order {} ended {}: {} credits at the place, {} on the way, {} quests, {} min",
              bot->GetName(), Planner::QuestOrderWordName(run.order.word), Planner::QuestOrderEndName(end),
              placeCredits, wayCredits, quests, minutes);
    // A case bot's trace (WQ18b part 3, fault F4): an order at NPCs that the clock ended, with what tells a bound
    // too short from an arrival that never came
    bool const expectsHandIn =
        std::any_of(run.order.expected.begin(), run.order.expected.end(), [](Planner::QuestOrderNeed const& need)
                    { return need.objective == Planner::QuestFields::HAND_IN; });
    if ((end == Planner::QuestOrderEnd::Time || end == Planner::QuestOrderEnd::Dry ||
         end == Planner::QuestOrderEnd::NoEnder) &&
        (expectsHandIn || run.order.word == Planner::QuestOrderWord::HandIn) && TestProbe::On(botAI->testConstraints))
    {
        float nearest = -1.0f;
        if (std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests())
            for (Planner::QuestFieldExec::NpcSpot const& spot : OrderSpots(bot, run.order, *index))
            {
                float const yards = bot->GetExactDist2d(spot.x, spot.y);
                nearest = nearest < 0.0f ? yards : std::min(nearest, yards);
            }
        uint32 const seconds = run.startedAt ? getMSTimeDiff(run.startedAt, getMSTime()) / IN_MILLISECONDS : 0;
        LOG_INFO("playerbots",
                 "[Probe] {} order {} ended {}: walk {:.2f} min, stay {:.2f} min, {} s since its start, {}, "
                 "{:.0f} yd from its place, nearest expected NPC {:.0f} yd, {} needs open, {} taken, {} handed in",
                 bot->GetName(), Planner::QuestOrderWordName(run.order.word), Planner::QuestOrderEndName(end),
                 run.order.walkMinutes, run.order.minutes, seconds, run.arrivedAt ? "arrived" : "not arrived",
                 bot->GetDistance(data.pos), nearest, run.open.size(), run.takes, run.handIns);
    }
    return own;
}
