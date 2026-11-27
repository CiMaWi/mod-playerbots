/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlannerService.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "Awareness.h"
#include "Bag.h"
#include "BankMailService.h"
#include "BankMailState.h"
#include "BotTraitMgr.h"
#include "BotTravel.h"
#include "BridgeKnowledgeRules.h"
#include "Chat.h"
#include "ClassQuestCompletion.h"
#include "ClassQuestRules.h"
#include "CraftVisit.h"
#include "DBCStores.h"
#include "EventLog.h"
#include "Formulas.h"
#include "GatherLive.h"
#include "GoalInterface.h"
#include "GoalModelLog.h"
#include "Group.h"
#include "InterludeInputs.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "LoneKeys.h"
#include "NewRpgAction.h"
#include "NewRpgErrandInternal.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "NewRpgTriggers.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerIndexes.h"
#include "PlannerPool.h"
#include "PlannerPoolSelftest.h"
#include "PlannerRecipes.h"
#include "PlannerRouteCost.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ProfessionConfig.h"
#include "QuestCircuitLog.h"
#include "QuestFailRecord.h"
#include "ReputationMgr.h"
#include "RouteService.h"
#include "SkillPathLive.h"
#include "SpellAuras.h"
#include "StartingHistoryRules.h"
#include "StringFormat.h"
#include "TestConstraints.h"
#include "TestProbe.h"
#include "Timer.h"
#include "TravelGraph.h"
#include "TravelMgr.h"
#include "TravelPlaces.h"
#include "UpkeepNeedRules.h"
#include "World.h"
#include "WorldLog.h"
#include "WorldLogStatus.h"
#include "WorldTravelConfig.h"

namespace Planner
{
namespace
{
constexpr uint32 SPELL_HEARTHSTONE = 8690;
constexpr uint32 PERCENT = 100;
constexpr uint32 SELFTEST_BOTS_PER_THREAD = 50;
constexpr uint32 SELFTEST_FAILURES_SHOWN = 5;
constexpr uint32 STATE_ZONE_SHIFT = 8;
constexpr uint32 STATE_ALIVE_BIT = 1u << 24;
constexpr uint32 STATE_GROUP_BIT = 1u << 25;
constexpr uint32 STATE_SCOPE_BIT = 1u << 31;  // in Planner::BotState::stateKey: the bot keeps its possessions
constexpr double NANOS_PER_MICRO = 1000.0;
static_assert(SNAPSHOT_SKILLS ==
                  std::array<std::uint16_t, 15>{SKILL_RIDING, SKILL_FIRST_AID, SKILL_COOKING, SKILL_FISHING,
                                                SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
                                                SKILL_HERBALISM, SKILL_INSCRIPTION, SKILL_JEWELCRAFTING,
                                                SKILL_LEATHERWORKING, SKILL_MINING, SKILL_SKINNING, SKILL_TAILORING},
              "PlannerPool.h's snapshot skills are the core's SkillType ids");

static_assert(TAXI_WORDS == TaxiMaskSize, "the snapshot's taxi mask has PlayerTaxi's layout");
static_assert(QUEST_COUNTERS == QUEST_OBJECTIVES_COUNT, "one counter per quest objective");

std::atomic<uint64> snapshotsBuilt{0};
std::atomic<uint64> pendingLost{0};  // jobs out longer than RESULT_KEEP_MS without a result: dropped or failed
RouteCostTotals routeCosts;          // every goals job's route cost (JobTravel)

// On a worker: PerfMonitor rows record from any thread (one shard per thread)
void RecordPerf(std::uint64_t computeUs, std::uint64_t waitUs)
{
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "planner job"))
        row->Record(computeUs);
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "planner queue wait"))
        row->Record(waitUs);
}

// Level, zone, life and group: cached fields, no scan. A change bumps the bot's state version
uint32 StateKey(Player* bot)
{
    uint32 key = bot->GetLevel() | (bot->GetZoneId() << STATE_ZONE_SHIFT);
    if (bot->IsAlive())
        key |= STATE_ALIVE_BIT;
    if (bot->GetGroup())
        key |= STATE_GROUP_BIT;
    return key;
}

Limits ConfiguredLimits()
{
    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    return {config.PlannerStaleSeconds * IN_MILLISECONDS, static_cast<float>(config.PlannerRecheckYards)};
}

// How long a planner goal's key stays blocked (GoalApply::BlockKey, questing WQ6b)
uint32 BlockMs() { return sPlayerbotAIConfig.WorldTravel.PlannerBlockMinutes * MINUTE * IN_MILLISECONDS; }

// How long a bot stays near an inn before the planner binds it there (NR.5); 0 = no dwell
uint32 InnDwellMs() { return sPlayerbotAIConfig.WorldTravel.InnDwellMinutes * MINUTE * IN_MILLISECONDS; }

Place PlaceOf(Player* bot) { return {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()}; }

// What Player::DurabilityRepair would charge for the item, without repairing it (DurabilityRepairAll(false, ...)
// repairs for free, so the snapshot cannot call it): lost durability times the item level's multiplier for its class
// and subclass times the quality's modifier times Rate.RepairCost; no reputation discount (the vendor's faction)
uint32 RepairCost(Item const* item)
{
    if (!item)
        return 0;
    uint32 const most = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
    uint32 const current = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
    if (!most || current >= most)
        return 0;
    ItemTemplate const* proto = item->GetTemplate();
    DurabilityCostsEntry const* costs = proto ? sDurabilityCostsStore.LookupEntry(proto->ItemLevel) : nullptr;
    DurabilityQualityEntry const* quality =
        costs ? sDurabilityQualityStore.LookupEntry((proto->Quality + 1) * 2) : nullptr;
    if (!quality)
        return 0;
    uint32 const multiplier = costs->multiplier[ItemSubClassToDurabilityMultiplierId(proto->Class, proto->SubClass)];
    uint32 const copper = uint32((most - current) * multiplier * double(quality->quality_mod));
    return std::max<uint32>(uint32(copper * sWorld->getRate(RATE_REPAIRCOST)), 1);
}

void AddRepair(RepairPrice& price, Item const* item)
{
    uint32 const copper = RepairCost(item);
    if (!copper)
        return;
    price.total += copper;
    price.cheapest = price.cheapest ? std::min(price.cheapest, copper) : copper;
}

// The backpack's and the worn ordinary bags' slots: four lookups
uint32 BagSlots(Player* bot)
{
    uint32 slots = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = bot->GetBagByPos(bagSlot);
        if (bag && bag->GetTemplate()->Class == ITEM_CLASS_CONTAINER &&
            bag->GetTemplate()->SubClass == ITEM_SUBCLASS_CONTAINER)
            slots += bag->GetBagSize();
    }
    return slots;
}

void FillBags(Player* bot, Snapshot& snapshot)
{
    snapshot.bagSlots = static_cast<uint16>(BagSlots(bot));
    uint32 free = 0;
    uint32 questItems = 0;  // questing WQ44: by the template alone, no look at the log
    auto const count = [&free, &questItems](Item const* item)
    {
        if (!item)
            ++free;
        else if (item->GetTemplate()->Class == ITEM_CLASS_QUEST || item->GetTemplate()->Bonding == BIND_QUEST_ITEM)
            ++questItems;
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        count(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = bot->GetBagByPos(bagSlot);
        if (!bag || bag->GetTemplate()->Class != ITEM_CLASS_CONTAINER ||
            bag->GetTemplate()->SubClass != ITEM_SUBCLASS_CONTAINER)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            count(bag->GetItemByPos(static_cast<uint8>(slot)));
    }
    snapshot.bagFree = static_cast<uint16>(free);
    snapshot.questItemSlots = static_cast<uint16>(questItems);
    snapshot.unarmed = !bot->GetWeaponForAttack(BASE_ATTACK);

    GearWear const wear = ReadGearWear(bot);
    snapshot.durabilityPercent = wear.durabilityPercent;
    snapshot.lowestDurabilityPercent = wear.lowestPercent;
    snapshot.brokenItems = wear.broken;
    auto const repairPrice = PriceRepair(bot);
    snapshot.repairCost = repairPrice.total;
    snapshot.repairCheapest = static_cast<std::uint32_t>(repairPrice.cheapest);
}

}  // namespace

void FillSlots(Player* bot, std::vector<QuestSlot>& quests)
{
    for (uint16 slot = 0; slot < bot->GetQuestLogSize(); ++slot)
    {
        uint32 const quest = bot->GetQuestSlotQuestId(slot);
        if (!quest)
            continue;
        QuestSlot entry;
        entry.quest = quest;
        entry.state = bot->GetQuestSlotState(slot);
        for (uint8 counter = 0; counter < QUEST_OBJECTIVES_COUNT; ++counter)
            entry.counters[counter] = bot->GetQuestSlotCounter(slot, counter);
        quests.push_back(entry);
    }
}

namespace
{
void FillQuests(Player* bot, Snapshot& snapshot)
{
    FillSlots(bot, snapshot.quests);
    snapshot.noLogLimit = bot->HasUnlimitedQuestLog();
    RewardedQuestSet const& rewarded = bot->getRewardedQuests();
    snapshot.rewardedQuests.assign(rewarded.begin(), rewarded.end());
    std::sort(snapshot.rewardedQuests.begin(), snapshot.rewardedQuests.end());
}

// Mail list read: the bot's own map thread or the world thread only (FillSnapshot's callers; BankMailState.h)
void FillBankMail(Player* bot, Snapshot& snapshot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;
    BankMail::BotBankMail& known = botAI->bankMail;
    BankMail::RefreshMailKnowledge(bot, known.mail);
    BankMail::DueTotal const due = BankMail::DueAt(known.bank.kept, bot->GetLevel());
    snapshot.mailWaiting = known.mail.waiting;
    snapshot.mailSeenCopper = known.mail.seenCopper;
    snapshot.mailExpectedCopper = known.mail.expectedCopper;
    snapshot.bankKeptCount = static_cast<uint32>(known.bank.kept.size());
    snapshot.bankDueCount = due.count;
    snapshot.bankDueWorth = due.worth;
    snapshot.bankFreeSlots = known.bank.freeSlots;
}

void AnswerTakes(Player* bot, Snapshot& snapshot, GoalApply::BotGoals& goals);

// FillSnapshot and the game's answer about takes with its PerfMonitor row (map-thread microseconds per snapshot; rows
// `planner take key` and `planner take set` are the answer's share)
Snapshot TimedSnapshot(Player* bot, uint32 stateVersion, GoalApply::BotGoals& goals)
{
    auto const started = std::chrono::steady_clock::now();
    Snapshot snapshot = FillSnapshot(bot, stateVersion);
    AnswerTakes(bot, snapshot, goals);
    auto const us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "planner snapshot"))
        row->Record(static_cast<uint64>(std::max<long long>(us, 0)));
    snapshotsBuilt.fetch_add(1, std::memory_order_relaxed);
    return snapshot;
}

std::atomic<uint64> goalsApplied{0};      // a new goal started from a plan
std::atomic<uint64> goalsContinued{0};    // the circuit's next task started from the same plan
std::atomic<uint64> goalsResumed{0};      // an interlude ended and the goal it interrupted went on
std::atomic<uint64> goalSwitches{0};      // a replan left the current goal
std::atomic<uint64> planWaits{0};         // idle stretches that waited for a coming plan
std::atomic<uint64> scopeRefused{0};      // a bot found out of scope right before its job
std::atomic<uint64> stalledSkips{0};      // an option not started: its key stalled (GoalApply::MayStart)
std::atomic<uint64> switchNotStarted{0};  // a switch or a fallback's plan started nothing: no executor, stall, refusal
std::atomic<uint64> retrySkips{0};        // an interlude passed over: its kind failed within the retry time (WT63c)
std::atomic<uint64> routeResults{0};      // results taken by bots on a route goal (questing WQ18b)
std::atomic<uint64> routeRejectedMoved{0};  // of those, rejected because the bot had moved over the recheck yards
std::array<std::atomic<uint64>, static_cast<std::size_t>(GoalApply::Fallback::Count)> fallbacks{};

// What `planner goal <bot>` shows, published by the bot's map thread when it changes (a goal, a plan, a fallback)
struct OptionLine
{
    std::string choice;
    float valuePerMinute{0.0f};
    float minutes{0.0f};
    std::string reason;
};

struct GoalView
{
    GoalApply::Active goal;
    std::shared_ptr<Goals::GoalPlan const> plan;  // shared with the bot's BotGoals, immutable
    std::vector<OptionLine> options;
    std::string failure;
    uint32 planMs{0};
    bool jobOut{false};
    std::size_t questsStarted{0};
    GoalApply::Fallback lastFallback{GoalApply::Fallback::None};
    uint32 lastFallbackMs{0};
    bool paused{false};
};

std::mutex viewsMutex;
std::unordered_map<uint64, GoalView> views;  // by raw guid; a bot's entry is replaced, never erased (bounded)

// A PerfMonitor row's time for one scope (the hook, a bridge order's route search): two clock reads, only while the
// monitor is on
class RowTimer
{
public:
    explicit RowTimer(char const* name) : _row(sPerfMonitor.acquire(PERF_MON_TOTAL, name))
    {
        if (_row)
            _started = std::chrono::steady_clock::now();
    }
    ~RowTimer()
    {
        if (_row)
            _row->Record(static_cast<uint64>(std::max<long long>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _started)
                    .count(),
                0)));
    }
    RowTimer(RowTimer const&) = delete;
    RowTimer& operator=(RowTimer const&) = delete;

private:
    PerformanceData* _row;
    std::chrono::steady_clock::time_point _started;
};

// The scope right now: the bot keeps its possessions (no master at a game client)
bool InScopeNow(PlayerbotAI* botAI) { return botAI->KeepsPossessions(); }

constexpr char const* SWITCH_WORD = "switch";  // the Status row of a planner start over a running status
constexpr char const* RESUME_WORD = "resume";  // the Status row of the goal an interlude interrupted, going on
// The same once its interludes passed the bound (GoalApply::INTERLUDE_SHIFT_MAX_MS): the excess counted against it
constexpr char const* RESUME_BOUND_WORD = "resume_bound";

// The stall EndRun or a refused start (StartActive) just recorded, if any: its one event row
void NoteStalled(Player* bot, GoalApply::BotGoals& goals)
{
    std::optional<GoalApply::StallRow> const row = std::exchange(goals.stallRow, std::nullopt);
    if (!row)
        return;
    std::string const src = row->refused  ? "refused"
                            : row->judged ? std::string(StatusEnd::Word(row->outcome)) + "/" + row->note
                                          : "none";
    EventLog::Emit(bot, EventLog::EventType::GoalStalled, src.c_str(), static_cast<uint32>(row->kind));
    // A case bot's trace (WT63e)
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot); botAI && TestProbe::On(botAI->testConstraints))
        LOG_INFO("playerbots", "[Probe] {} stalled kind {} last {}", bot->GetName(), static_cast<int>(row->kind), src);
}

// A switch that started nothing ended nothing: counted, and one event row
void NoteNotStarted(Player* bot, GoalApply::StartResult result, Goals::GoalKind kind)
{
    switchNotStarted.fetch_add(1, std::memory_order_relaxed);
    char const* const word = kind == Goals::GoalKind::None                  ? "no_option"
                             : result == GoalApply::StartResult::NoExecutor ? "no_executor"
                             : result == GoalApply::StartResult::Stalled    ? "stalled"
                             : result == GoalApply::StartResult::Retry      ? "retry"
                                                                            : "refused";
    EventLog::Emit(bot, EventLog::EventType::SwitchNotStarted, word, static_cast<uint32>(kind));
}

// An interlude ended and the goal it interrupted could not go on: why (its circuit had nothing left, no executor,
// refused), its kind, and 1 when the newest plan is for another circuit than the goal's own
void NoteResumeNotStarted(Player* bot, GoalApply::BotGoals const& goals, GoalApply::Active const& goal,
                          GoalApply::Executor executor)
{
    char const* const word = executor == GoalApply::Executor::None      ? "no_executor"
                             : executor == GoalApply::Executor::Circuit ? "circuit_done"
                                                                        : "refused";
    bool const away = goals.plan && goals.circuitPlan &&
                      GoalApply::CircuitKey(*goals.plan) != GoalApply::CircuitKey(*goals.circuitPlan);
    EventLog::Emit(bot, EventLog::EventType::ResumeNotStarted, word, static_cast<uint32>(goal.kind), away ? 1 : 0);
}

std::string ChoiceOf(GoalApply::Active const& goal)
{
    Goals::Option option;
    option.kind = goal.kind;
    option.target = goal.target;
    option.targetType = goal.targetType;
    return Goals::ChoiceWord(option);
}

void Publish(Player* bot, GoalApply::BotGoals& goals)
{
    GoalApply::MarkPublished(goals);
    GoalView view;
    view.goal = goals.slot.Query();
    view.plan = goals.plan;
    if (goals.plan)
    {
        Goals::Ranked const& ranked = goals.plan->ranked;
        for (Goals::Option const& option : ranked.options)
            view.options.push_back({Goals::ChoiceWord(option), option.valuePerMinute, option.minutes, option.reason});
        view.failure = ranked.ok ? std::string() : ranked.failure;
    }
    view.planMs = goals.planMs;
    view.jobOut = goals.jobOut;
    view.questsStarted = goals.quest.started.size();
    view.lastFallback = goals.lastFallback;
    view.lastFallbackMs = goals.lastFallbackMs;
    view.paused = goals.paused;
    std::lock_guard<std::mutex> lock(viewsMutex);
    views[bot->GetGUID().GetRawValue()] = std::move(view);
}

GoalApply::BotGoals& GoalsOf(PlayerbotAI* botAI, Player* bot)
{
    std::shared_ptr<GoalApply::BotGoals>& goals = botAI->planner.goals;
    if (!goals)
    {
        goals = std::make_shared<GoalApply::BotGoals>();
        // The bag edge's first look is the goals' birth: a capacity first seen by the errand trigger (every 30 s)
        // after a change would be taken as the start and the change never reach the planner (WT63c, lab14 seed
        // 75779019: the bags lost 22 s after a test's release went unseen for five minutes)
        goals->bagSlotsSeen = static_cast<uint16>(BagSlots(bot));
        Publish(bot, *goals);
    }
    return *goals;
}

WorldPosition PositionOf(Place const& place) { return WorldPosition(place.map, place.x, place.y, place.z); }

bool Placed(Player* bot, Place const& place, bool sameMap)
{
    return Goals::Finite(place) && (place.x != 0.0f || place.y != 0.0f) && (!sameMap || place.map == bot->GetMapId());
}

// The bot's log quests a quest goal may start DO_QUEST on: incomplete ones to work, complete ones to turn in
struct QuestTests
{
    std::function<bool(uint32)> work;
    std::function<bool(uint32)> turnIn;
};

QuestTests TestsOf(Player* bot, std::function<bool(uint32)> const& workable)
{
    return {[bot, &workable](uint32 quest)
            { return bot->GetQuestStatus(quest) == QUEST_STATUS_INCOMPLETE && workable(quest); },
            [bot, &workable](uint32 quest)
            { return bot->GetQuestStatus(quest) == QUEST_STATUS_COMPLETE && workable(quest); }};
}

// The quest log and level now (GoalApply::LogSignature): at most 25 slots and their item counts, read once per plan
// applied and when a goal ends
uint64 LogNow(Player* bot)
{
    std::vector<QuestSlot> quests;
    quests.reserve(bot->GetQuestLogSize());
    FillSlots(bot, quests);
    std::vector<uint16> items;
    items.reserve(quests.size() * QUEST_ITEM_OBJECTIVES_COUNT);
    QuestStatusMap& statuses = bot->getQuestStatusMap();
    for (QuestSlot const& slot : quests)
    {
        auto const it = statuses.find(slot.quest);
        if (it != statuses.end())
            items.insert(items.end(), std::begin(it->second.ItemCount), std::end(it->second.ItemCount));
    }
    return GoalApply::LogSignature(quests, bot->GetLevel(), items);
}

GoalApply::BotSpot SpotOf(Player* bot) { return {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY()}; }

// The level and XP as one number that grows with every XP gained (GoalApply::EndRun's grind and gather progress)
uint64 XpNow(Player* bot) { return (static_cast<uint64>(bot->GetLevel()) << 32) | bot->GetUInt32Value(PLAYER_XP); }

// What a gather or money goal takes (professions WR20c, GoalApply::GainIsProgress): the bot's copper plus the count of
// the items in its bags. Read at a goal's start and end only
uint64 GainNow(Player* bot)
{
    uint64 items = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            items += item->GetCount();
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag const* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (Item const* item = bag->GetItemByPos(static_cast<uint8>(slot)))
                    items += item->GetCount();
    return static_cast<uint64>(bot->GetMoney()) + items;
}

// Where the goal's place and quest are for the bot (GoalApply::ExecutorFor). A delivery on the bot's map passes the
// circuit's turn-in test: no reward POI here or a turn-in that failed there (low priority) does not start
GoalApply::Reach ReachOf(Player* bot, GoalApply::Active const& goal, QuestTests const& tests)
{
    GoalApply::Reach reach;
    reach.sameMap = goal.place.map == bot->GetMapId();
    if (goal.targetType == Goals::TargetType::Quest)
        reach.deliverable =
            reach.sameMap ? tests.turnIn(goal.target) : bot->GetQuestStatus(goal.target) == QUEST_STATUS_COMPLETE;
    reach.there =
        Placed(bot, goal.place, true) && bot->GetDistance2d(goal.place.x, goal.place.y) <= GoalApply::GIVER_YARDS;
    return reach;
}

// The goal's own circuit (BotGoals::circuitPlan, GoalApply::AdvanceQuest): its next task or a completed quest's
// turn-in to DO_QUEST; without one, once per goal, its givers (walking to its start when far, else looking for them
// around). False when the circuit has nothing left
bool StartQuest(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, QuestTests const& tests,
                char const* word = WorldLog::STATUS_REASON_OTHER)
{
    if (!goals.circuitPlan)
        return false;
    CircuitPlan const& circuit = goals.circuitPlan->circuit;
    for (;;)  // ends: each quest starts at most once per goal
    {
        GoalApply::QuestNext const next = GoalApply::AdvanceQuest(goals.quest, circuit, tests.work, tests.turnIn);
        if (next.step == GoalApply::QuestStep::Done)
            return false;
        if (next.step == GoalApply::QuestStep::Givers)
            break;
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(next.quest))
        {
            RpgStatus::Start(botAI, NewRpgInfo::DoQuest{quest, next.quest}, word);
            return true;
        }
    }
    if (Placed(bot, circuit.start, true) &&
        bot->GetDistance2d(circuit.start.x, circuit.start.y) > GoalApply::GIVER_YARDS)
        RpgStatus::Start(botAI, NewRpgInfo::GoCamp{PositionOf(circuit.start)}, word);
    else
        RpgStatus::Start(botAI, NewRpgInfo::WanderNpc{}, word);
    return true;
}

// The route goal's next order (questing WQ18b; GoalApply::NextOrder over the newest plan's route) to DO_QUEST in
// order mode. An order its executor did not end (an interlude, a status ended from outside) is given again; an order
// of another map than the bot's is passed over. False when the route has no order left (the goal ends and the plan
// starts nothing more: GoalApply::AdvanceRoute) or the executor refuses (the quest index is building), which gives
// the order again at the next start. A bot a test holds on the circuit (constraint `circuit`, part 3) starts none: a
// route goal held from before the hold ends here, and a plan is asked for, which has no route (SituationOf)
bool StartRoute(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals,
                char const* word = WorldLog::STATUS_REASON_OTHER)
{
    bool const held = TestConstraints::HeldOnCircuit(botAI->testConstraints);
    if (held)
        botAI->planner.snapshotDue = true;
    std::function<bool(QuestOrderNeed const&)> const open = NewRpgInternal::OrderNeedOpen(botAI, bot);
    uint64 const log = LogNow(bot);
    std::vector<std::uint64_t> const closed = GoalApply::ClosedCells(goals, log, getMSTime(), BlockMs());
    GoalApply::RouteNext next = GoalApply::NextOrder(goals, open, closed, held);
    while (next.step == GoalApply::RouteStep::Order && next.order->mapId != bot->GetMapId())
        next = GoalApply::AdvanceRoute(goals, open, closed);  // ends: each order is given at most once
    if (next.step == GoalApply::RouteStep::Done)
        return false;
    if (NewRpgInternal::StartRouteOrder(botAI, bot, *next.order, word))
    {
        GoalApply::NoteRouteStart(goals, SpotOf(bot), log);
        return true;
    }
    GoalApply::OrderNotStarted(goals);
    return false;
}

// The plan's estimate of a gather circuit or fishing trip, written beside its start row (professions WR25a): value the
// minutes the plan gave it (a circuit: its laps' minutes; a trip: the walk and its time at the water), alternatives the
// takes expected in them (a node harvested, a creature killed, a fish caught), the units those give and the fight
// minutes of the kills; ref the goal's target as the start row's. A session then holds plan and outcome side by side.
// The takes' name says the rate's source (WPC13b): `takes_observed` when the minutes per take were observed, else
// `takes` (the static rule)
void NoteEstimate(Player* bot, Professions::Reason way, GoalApply::Active const& goal)
{
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession,
                       Professions::ChoiceWord(Professions::Choice::Estimate), Professions::ReasonWord(way),
                       goal.minutes,
                       {{goal.observed ? "takes_observed" : "takes", goal.takes},
                        {"units", goal.units},
                        {"fight_minutes", goal.fightMinutes}},
                       goal.target);
}

// Today's statuses are the executors (GoalApply::ExecutorFor). False when the goal cannot start here, having ended
// nothing. `word` is the Status row's word; a running status ends as interrupted
bool StartGoal(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, GoalApply::Active const& goal,
               GoalApply::Executor executor, QuestTests const& tests, char const* word = WorldLog::STATUS_REASON_OTHER)
{
    NewRpgInfo& info = botAI->rpgInfo;
    switch (executor)
    {
        case GoalApply::Executor::Circuit:
            return StartQuest(botAI, bot, goals, tests, word);
        case GoalApply::Executor::Route:
            return StartRoute(botAI, bot, goals, word);
        case GoalApply::Executor::Deliver:
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(goal.target))
            {
                RpgStatus::Start(botAI, NewRpgInfo::DoQuest{quest, goal.target}, word);
                return true;
            }
            return false;
        case GoalApply::Executor::Grind:
            if (!Placed(bot, goal.place, true))
                return false;
            RpgStatus::Start(botAI, NewRpgInfo::GoGrind{PositionOf(goal.place)}, word);
            return true;
        case GoalApply::Executor::Gather:
        {
            // The plan's circuit when the goal is its first stop (professions WR13b); else the nearest live node
            NewRpgInfo::DoGather gather;
            Goals::GoalPlan const* plan = goals.plan.get();
            bool const circuit = plan && !plan->gatherStops.empty() && plan->gatherStops.front() == goal.target;
            uint32 creatures = 0;
            if (circuit)
            {
                gather.circuit.assign(plan->gatherStops.begin(), plan->gatherStops.end());
                gather.laps = std::max<uint32>(plan->gatherLaps, 1);
                creatures = static_cast<uint32>(
                    std::count_if(plan->gatherStops.begin(), plan->gatherStops.end(), Gather::IsCreatureStop));
                // The span the observed rate counts (WPC13b): its zone and way as the circuit row names them
                gather.spanStartMs = std::max<uint32>(getMSTime(), 1);
                gather.span.zone = bot->GetZoneId();
                gather.span.way =
                    GatherRate::WayOf(static_cast<uint32>(plan->gatherStops.size()), plan->gatherHerbStops, creatures);
            }
            RpgStatus::Start(botAI, std::move(gather), word);
            if (circuit)
                WorldLog::Decision(
                    bot, WorldLogFormat::DecisionKind::Profession, Professions::ChoiceWord(Professions::Choice::Gather),
                    Professions::ReasonWord(Professions::Reason::Circuit), static_cast<float>(plan->gatherStops.size()),
                    {{"laps", static_cast<float>(std::max<uint32>(plan->gatherLaps, 1))},
                     {"herbs", static_cast<float>(plan->gatherHerbStops)},
                     {"creatures", static_cast<float>(creatures)}},
                    goal.target);
            if (circuit)
                NoteEstimate(bot, Professions::Reason::Circuit, goal);
            return true;
        }
        case GoalApply::Executor::Fish:
        {
            // The trip to the water's place (professions WR16b): the executor probes a pool or open water there
            if (!Placed(bot, goal.place, true))
                return false;
            NewRpgInfo::DoFish fish;
            fish.shore = PositionOf(goal.place);
            RpgStatus::Start(botAI, std::move(fish), word);
            Professions::Reason const reason = goal.reason == "skill_up"   ? Professions::Reason::Access
                                               : goal.reason == "interest" ? Professions::Reason::Interest
                                               : goal.reason == "use_soon" ? Professions::Reason::UseSoon
                                                                           : Professions::Reason::Income;
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession,
                               Professions::ChoiceWord(Professions::Choice::Fish), Professions::ReasonWord(reason),
                               goal.minutes, {}, goal.target);
            NoteEstimate(bot, Professions::Reason::Trip, goal);
            return true;
        }
        case GoalApply::Executor::Travel:
            // ApplyTravelOrder's refusals: the route follower starts alive, on the ground and in the open world
            if (!Placed(bot, goal.place, false) || !bot->IsAlive() || bot->IsInFlight() ||
                bot->GetMap()->Instanceable())
                return false;
            RpgStatus::Start(botAI, RpgStatus::TripTo(PositionOf(goal.place)), word);
            return true;
        case GoalApply::Executor::Rest:
            RpgStatus::Start(botAI, NewRpgInfo::Rest{}, word);
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        case GoalApply::Executor::Bind:
        {
            // The planner's order names its innkeeper (the hearth goal's target); the bridge's set-hearth names none:
            // the entry picks
            NewRpgInternal::BindOrder order{.order = true, .word = word};
            if (goal.source == GoalApply::Source::Planner)
                order.innkeeper = goal.target;
            return NewRpgInternal::StartBind(botAI, order);
        }
        case GoalApply::Executor::Errand:
        {
            // A sick bot outside every safe area goes in by its hearthstone (NR3); its errand at the inn comes next
            if (goal.source == GoalApply::Source::Planner && goal.reason == Goals::SICK_HEARTH_REASON)
                return NewRpgInternal::StartSickHearth(botAI, word);
            // The planner's order names its vendor (for a bag the nearest that sells it); the bridge's names none
            NewRpgInternal::ErrandOrder order{.order = true, .word = word};
            if (goal.source == GoalApply::Source::Planner)
            {
                order.reason = goal.reason.c_str();
                // A stop of its own (NR14) names a trainer key, a mount vendor or a beast: the trip plans it as the
                // due need it is, not as an ordered vendor
                if (!Goals::OwnStopReason(goal.reason))
                    order.vendor = goal.target;
                if (goal.kind == Goals::GoalKind::BuyBag)
                {
                    std::shared_ptr<PlannerIndexes const> const indexes = IndexesReady() ? Indexes() : nullptr;
                    order.item = goal.target;
                    order.vendor = indexes ? NewRpgInternal::BagVendorOf(bot, *indexes, goal.target) : 0;
                }
            }
            return NewRpgInternal::StartErrand(botAI, order);
        }
        default:
            return false;
    }
}

void LogGoal(Player* bot, Goals::Ranked const& ranked)
{
    if (!WorldLog::Active())
        return;
    for (GoalLog::Row const& row : GoalLog::Rows(ranked))
    {
        WorldLogFormat::Alternatives alternatives;
        for (GoalLog::Alt const& alt : row.alternatives)
            alternatives.Add({alt.choice, alt.value});
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Goal, row.choice, row.reason, row.value, alternatives);
    }
}

// The route goal's row again, for the route a new plan gave the running goal (questing WQ18b part 3): written where
// the plan replaces the running order (TakeGoals), so the log shows which goal the bot follows after a pickup's plan.
// A plan that keeps the running order writes none: the goal's last row still names what the bot does
void LogRouteGoal(Player* bot, Goals::Ranked const& ranked)
{
    for (std::size_t index = 0; index < ranked.options.size(); ++index)
        if (ranked.options[index].targetType == Goals::TargetType::Route)
        {
            LogGoal(bot, GoalApply::ChosenFirst(ranked, index));
            return;
        }
}

void LogCircuit(Player* bot, GoalApply::BotGoals& goals)
{
    if (goals.circuitLogged || !goals.plan || !WorldLog::Active())
        return;
    goals.circuitLogged = true;
    for (CircuitLog::Row const& row : CircuitLog::Rows(goals.plan->circuit))
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Circuit, row.choice, row.reason, row.value);
}

// One option started as the bot's goal (GoalApply::StartActive); its row shows the ranking with it first. A goal (no
// interlude) starts its run, with its circuit progress anew, and does not start while its key stalled (log: the
// signature at this plan). An interlude keeps the run and progress of the goal it interrupts
GoalApply::StartResult StartOption(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals,
                                   Goals::Option const& option, std::size_t index, uint32 now, uint64 log,
                                   QuestTests const& tests, char const* word)
{
    GoalApply::Active goal = GoalApply::FromOption(option, GoalApply::Source::Planner, now);
    if (!GoalApply::MayReplace(goals.slot.Query(), goal.source))
        return GoalApply::StartResult::Refused;
    if (GoalApply::RetryWaits(goals, goal.kind, now) || GoalApply::MissedWaits(goals, goal, bot->GetLevel(), now))
    {
        retrySkips.fetch_add(1, std::memory_order_relaxed);
        return GoalApply::StartResult::Retry;
    }
    GoalApply::Executor executor = GoalApply::ExecutorFor(goal, ReachOf(bot, goal, tests));
    bool const circuit = executor == GoalApply::Executor::Circuit;
    if (circuit && !goals.plan)
        executor = GoalApply::Executor::None;
    uint64 const key = GoalApply::GoalKey(goal, executor, goals.plan ? GoalApply::CircuitKey(*goals.plan) : 0);
    std::shared_ptr<Goals::GoalPlan const> held = goals.circuitPlan;  // a new circuit goal runs this plan's circuit
    if (circuit)
        goals.circuitPlan = goals.plan;
    GoalApply::StartResult const result =
        GoalApply::StartActive(goals, goal.kind, executor, {key, log, XpNow(bot), true, now, BlockMs(), GainNow(bot)},
                               [&] { return StartGoal(botAI, bot, goals, goal, executor, tests, word); });
    NoteStalled(bot, goals);
    if (result == GoalApply::StartResult::Stalled)
        stalledSkips.fetch_add(1, std::memory_order_relaxed);
    if (result == GoalApply::StartResult::Refused)
        GoalApply::NoteRefused(goals, goal, bot->GetLevel(), now);
    if (result != GoalApply::StartResult::Started)
    {
        goals.circuitPlan = std::move(held);
        return result;
    }
    goals.bagBought = false;  // a new order buys its own bag (OrderedBag)
    goals.ownStopDone = false;
    goals.slot.Set(std::move(goal));
    if (goals.plan)
        LogGoal(bot, GoalApply::ChosenFirst(goals.plan->ranked, index));
    if (circuit)
        LogCircuit(bot, goals);
    return result;
}

// What the plan's best option met when nothing started (NoteNotStarted); kind None: the plan had no option
struct FirstRefusal
{
    GoalApply::StartResult result{GoalApply::StartResult::Refused};
    Goals::GoalKind kind{Goals::GoalKind::None};
    std::size_t options{0};  // the plan's ranked options (WT63e: GoalApply::NotStartedReason)
    bool stalled{false};     // an option met its stalled key
};

// The plan's best option that can start; a plan starts at most one new goal
bool ApplyPlan(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, uint32 now, QuestTests const& tests,
               char const* word = WorldLog::STATUS_REASON_OTHER, FirstRefusal* first = nullptr)
{
    if (!goals.plan)
        return false;
    goals.planUsed = true;
    Goals::Ranked const& ranked = goals.plan->ranked;
    if (!ranked.ok)
        return false;
    uint64 const log = LogNow(bot);
    if (first)
        first->options = ranked.options.size();
    for (std::size_t i = 0; i < ranked.options.size(); ++i)
    {
        GoalApply::StartResult const result =
            StartOption(botAI, bot, goals, ranked.options[i], i, now, log, tests, word);
        if (result == GoalApply::StartResult::Started)
            return true;
        if (!first)
            continue;
        if (!i)
        {
            first->result = result;
            first->kind = ranked.options[i].kind;
        }
        first->stalled = first->stalled || result == GoalApply::StartResult::Stalled;
    }
    return false;
}

bool Preemptible(NewRpgStatus status)
{
    return status == RPG_DO_QUEST || status == RPG_GO_GRIND || status == RPG_GO_CAMP || status == RPG_DO_GATHER ||
           status == RPG_REST || status == RPG_WANDER_NPC || status == RPG_WANDER_RANDOM || status == RPG_DO_FISH;
}

// A fresh plan (GoalApply::OnFreshPlan): for a planner goal ReplanDecision says stay, switch or pick up, and a switch
// starts the chosen option at once; during a fallback status the plan's best option starts the same way. The start
// ends the running status as interrupted; an option that cannot start ends nothing (counted)
void ConsiderSwitch(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, uint32 now, QuestTests const& tests)
{
    bool const situation = std::exchange(goals.situationChanged, false);
    NewRpgStatus const status = botAI->rpgInfo.GetStatus();
    GoalApply::FreshPlan const fresh = GoalApply::OnFreshPlan(goals, Preemptible(status));
    // A test bot's trace (WT63c): what the fresh plan did; verdict -1 = no replan, result -1 = no start tried
    auto const probe = [&](int verdict, std::string const& reason, Goals::Option const* chosen, int result)
    {
        if (TestProbe::On(botAI->testConstraints))
            LOG_INFO("playerbots",
                     "[Probe] {} switch fresh {} status {} situation {} verdict {} reason {} chosen {}:{} "
                     "result {}",
                     bot->GetName(), static_cast<int>(fresh), static_cast<int>(status), situation, verdict, reason,
                     chosen ? static_cast<int>(chosen->kind) : -1, chosen ? chosen->target : 0, result);
    };
    if (fresh == GoalApply::FreshPlan::Take)
    {
        FirstRefusal first;
        bool const started = ApplyPlan(botAI, bot, goals, now, tests, SWITCH_WORD, &first);
        if (started)
            goalsApplied.fetch_add(1, std::memory_order_relaxed);
        else
            NoteNotStarted(bot, first.result, first.kind);  // the best option's refusal names why
        probe(-1, "take", nullptr, started ? 0 : static_cast<int>(first.result));
        return;
    }
    if (fresh != GoalApply::FreshPlan::Replan)
    {
        probe(-1, "keep", nullptr, -1);
        return;
    }
    GoalApply::Active const& current = goals.slot.Query();
    Goals::ReplanInput in;
    in.current = {current.kind, current.target, current.valuePerMinute, false, false, current.targetType};
    in.fresh = &goals.plan->ranked;
    in.delta.situation = situation;
    in.nowMs = now;
    in.lastPlanMs = goals.lastPlanMs;
    Snapshot traits;
    traits.traits = BotTraitMgr::instance().Find(bot->GetGUID().GetCounter());
    in.weights = Goals::FromTraits(traits);
    Goals::Replan const replan = Goals::ReplanDecision(in);
    if (replan.verdict == Goals::ReplanVerdict::Stay || !replan.chosen)
    {
        probe(static_cast<int>(replan.verdict), replan.reason, replan.chosen ? &*replan.chosen : nullptr, -1);
        return;
    }
    Goals::Ranked const& ranked = goals.plan->ranked;
    std::size_t index = 0;
    while (index < ranked.options.size() && !Goals::SameTarget(ranked.options[index], *replan.chosen))
        ++index;
    GoalApply::StartResult const result =
        StartOption(botAI, bot, goals, *replan.chosen, index, now, LogNow(bot), tests, SWITCH_WORD);
    probe(static_cast<int>(replan.verdict), replan.reason, &*replan.chosen, static_cast<int>(result));
    if (result != GoalApply::StartResult::Started)
    {
        NoteNotStarted(bot, result, replan.chosen->kind);
        return;
    }
    goals.planUsed = true;  // a plan starts at most one new goal
    goalSwitches.fetch_add(1, std::memory_order_relaxed);
}

// A checked answer of the job the bot waited for: kept as its plan (in scope only)
void TakeGoals(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, Result const& result, uint32 now,
               std::function<bool(uint32)> const& workable)
{
    GoalsAnswer const* answer = std::get_if<GoalsAnswer>(&result.payload);
    // A test's noplan row (WT63e) drops the answer as failed: the bot falls back
    if (!answer || !answer->plan || !TestConstraints::PlansTaken(botAI->testConstraints))
    {
        goals.lastFailed = true;  // a failed job answers without a plan (GoalsCompute)
        goals.lastRejected = false;
        Publish(bot, goals);
        return;
    }
    if (!InScopeNow(botAI))
    {
        scopeRefused.fetch_add(1, std::memory_order_relaxed);
        goals.lastRejected = true;
        goals.lastFailed = false;
        return;
    }
    bool const wasReplaced = GoalApply::OrderReplaced(goals);
    std::shared_ptr<Goals::GoalPlan const> const lastPlan = goals.plan;
    GoalApply::TakePlan(goals, answer->plan, result.takenMs);
    // A plan with no option starts no goal, so no start writes its row: written here, so every plan taken that ranked
    // nothing shows in the log (none:0)
    if (answer->plan->ranked.options.empty())
        LogGoal(bot, answer->plan->ranked);
    // A route goal's running order (questing WQ18b): its executor learns whether the new route still begins with it
    // (RefreshRoute). If not, its next tick ends the order as replaced and the new route's first order follows; the
    // goal's row is written again for the new route, once (part 3)
    if (goals.route.running)
    {
        bool const replaced = GoalApply::OrderReplaced(goals);
        NewRpgInternal::MarkOrderReplaced(botAI, *goals.route.running, replaced);
        if (replaced && !wasReplaced)
            LogRouteGoal(bot, answer->plan->ranked);
    }
    // A wanted material only creatures too far above the bot give (professions WR20c): one skip row per change
    Goals::GoalPlan const& taken = *answer->plan;
    // A due stop of its own whose list this plan's snapshot left out (NR14c): the next snapshot seeks it
    if (taken.ownStopsSkipped)
        botAI->planner.snapshotDue = true;
    if (taken.gatherRefusedItem && WorldLog::Active() &&
        (!lastPlan || lastPlan->gatherRefusedItem != taken.gatherRefusedItem))
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession,
                           Professions::ChoiceWord(Professions::Choice::Skip),
                           Professions::ReasonWord(Professions::Reason::TooDangerous), taken.gatherRefusedLevels, {},
                           taken.gatherRefusedItem);
    // Bags above the gather status's limit refused the circuit (professions WPC9): one skip row per change
    if (taken.gatherBagsFull && WorldLog::Active() && (!lastPlan || !lastPlan->gatherBagsFull))
        WorldLog::Decision(
            bot, WorldLogFormat::DecisionKind::Profession, Professions::ChoiceWord(Professions::Choice::Skip),
            Professions::ReasonWord(Professions::Reason::BagsFull), static_cast<float>(taken.gatherBagsFull));
    // A bot without a weapon was refused creature stops (professions WPC10): one skip row per change
    if (taken.gatherUnarmed && WorldLog::Active() && (!lastPlan || !lastPlan->gatherUnarmed))
        WorldLog::Decision(
            bot, WorldLogFormat::DecisionKind::Profession, Professions::ChoiceWord(Professions::Choice::Skip),
            Professions::ReasonWord(Professions::Reason::Unarmed), static_cast<float>(taken.gatherUnarmed));
    // A doable class quest the plan found failed (no way to its giver): completed here, on the bot's map thread (WU21a)
    if (!answer->plan->classFailed.empty())
        ClassQuestCompletion::CompleteFailed(bot, answer->plan->classFailed);
    std::shared_ptr<SkillPath::Path const> const before = std::exchange(goals.path, answer->path);  // WR11
    goals.pathMs = answer->path ? result.takenMs : 0;
    goals.gatherRates = answer->gatherRates;  // WR13c: the pass-by gate's rates
    if (goals.path)
        TakeSecondChoice(botAI, bot, before.get(), *goals.path);
    // A recipe item the bot can use is learned at once (professions WR14c, PLAN 3.9): each plan taken looks at the
    // bags, out of a fight, so a looted, bought or mailed recipe enters the next plan's path
    if (botAI->KeepsPossessions() && bot->IsAlive() && !bot->IsInCombat())
        botAI->DoSpecificAction("use random recipe", Event(), true);
    if (WorldLog::Active())
        for (CircuitLog::Row const& row : CircuitLog::PlanRows(answer->plan->routeGaps))
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Circuit, row.choice, row.reason, row.value);
    Goals::Ranked const& ranked = answer->plan->ranked;
    switch (GoalApply::NoteShort(goals, ranked.shortfallCopper, ranked.shortKind))
    {
        case GoalApply::ShortChange::Begin:
            EventLog::Emit(bot, EventLog::EventType::NeedShort, goals.shortKind.c_str(), 1,
                           static_cast<int64>(ranked.shortfallCopper));
            break;
        case GoalApply::ShortChange::End:
            EventLog::Emit(bot, EventLog::EventType::NeedShort, goals.shortKind.c_str(), 0, 0);
            break;
        default:
            break;
    }
    ConsiderSwitch(botAI, bot, goals, now, TestsOf(bot, workable));
    Publish(bot, goals);
}

constexpr std::size_t VENDORS_SEEN = 8;      // the nearest vendors the live Situation weighs
constexpr std::size_t BAG_VENDORS_SEEN = 4;  // the nearest bag vendors, each with its largest bag

// The core's interact test (Player::GetNPCIfCanInteractWith), as the errand entry's: a reaction above Unfriendly
bool Trades(Player* bot, Service const& service)
{
    return bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(service.factionTemplate), bot) > REP_UNFRIENDLY;
}

static_assert(REP_HOSTILE == Upkeep::HUB_HOSTILE_RANK, "the hub's hostile rank is the core's REP_HOSTILE");

// NR3 (NR.4, Upkeep::SafeAreas): the safe areas a point lies in for this bot. The city and the low zone from the
// point's zone and area; the hubs: every hub circle of the map that holds the point and is safe for the bot (not the
// other side's, a flight master or an innkeeper, no creature member hostile to the bot by its reputation), or with
// `among` only the hubs of that list (a stop's, against the bot's own). Only for a sick bot, once per situation and
// stop; map thread: the reactions read the bot's reputation
Upkeep::SafeAreas SafeAreasAt(Player* bot, PlannerIndexes const& indexes, uint32 mapId, float x, float y, uint32 zoneId,
                              uint32 areaId, std::vector<uint32_t> const* among)
{
    Upkeep::SafeAreas areas;
    bool const alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    AreaTableEntry const* zone = zoneId ? sAreaTableStore.LookupEntry(zoneId) : nullptr;
    AreaTableEntry const* area = areaId ? sAreaTableStore.LookupEntry(areaId) : nullptr;
    areas.city = Upkeep::SafeCity(zoneId, areaId, PlayerbotBridge::Knowledge::IsOwnCapital(zoneId, alliance),
                                  zone && zone->IsSanctuary(), area && area->IsSanctuary());
    if (ZoneRecord const* record = indexes.FindZone(zoneId))
        if (Upkeep::LowZoneSafe(record->high, record->side == (alliance ? ZoneSide::Alliance : ZoneSide::Horde),
                                Acore::XP::GetGrayLevel(bot->GetLevel())))
            areas.lowZone = zoneId;
    auto const holds = [mapId, x, y](Hub const& hub)
    { return hub.mapId == mapId && std::hypot(hub.x - x, hub.y - y) <= hub.radius; };
    if (among)
    {
        for (uint32_t const hub : *among)
            if (holds(indexes.hubs[hub]))
                areas.hubs.push_back(hub);
        return areas;
    }
    PlaceSide const other = alliance ? PlaceSide::Horde : PlaceSide::Alliance;
    for (std::size_t i = 0; i < indexes.hubs.size(); ++i)
    {
        Hub const& hub = indexes.hubs[i];
        if (!holds(hub))
            continue;
        bool const otherSide = hub.side == other;
        bool const flightOrInn = hub.Has(ServiceKind::FlightMaster) || hub.Has(ServiceKind::Innkeeper);
        bool const hostile =
            !otherSide && flightOrInn &&
            std::any_of(hub.members.begin(), hub.members.end(),
                        [bot, &indexes](uint32_t member)
                        {
                            Service const& service = indexes.services[member];
                            FactionTemplateEntry const* faction =
                                service.mailbox || service.focus
                                    ? nullptr
                                    : sFactionTemplateStore.LookupEntry(service.factionTemplate);
                            return faction &&
                                   Upkeep::HostileMember(static_cast<int32>(bot->GetFactionReactionTo(faction, bot)));
                        });
        if (Upkeep::HubSafe(otherSide, flightOrInn, hostile))
            areas.hubs.push_back(static_cast<uint32_t>(i));
    }
    return areas;
}

// NR3: the walk from a sick bot (its safe areas `here`) to a point stays in one safe area (Upkeep::SafeLeg on foot)
bool SafeWalk(Player* bot, PlannerIndexes const& indexes, Upkeep::SafeAreas const& here, uint32 mapId, float x, float y,
              uint32 zoneId, uint32 areaId)
{
    return Upkeep::SafeLeg(Upkeep::SafeLegKind::Foot, here,
                           SafeAreasAt(bot, indexes, mapId, x, y, zoneId, areaId, &here.hubs));
}

bool SafeWalk(Player* bot, PlannerIndexes const& indexes, Upkeep::SafeAreas const& here, Service const& stop)
{
    return SafeWalk(bot, indexes, here, stop.mapId, stop.x, stop.y, stop.zoneId, stop.areaId);
}

// The errand inputs (WT63c, InterludeInputs.h) within the errand walk: the nearest vendors, the nearest repairer and
// each near bag vendor's bags (the cheapest of each size), from the indexes and the vendor lists in memory;
// the sellable stacks and the minutes to the next vendor stop as the errand start counts them. A sick bot's (`sick`,
// its safe areas, NR3) vendors, repairer and station are only those its walk reaches inside its safe area
void FillErrandSeen(PlayerbotAI* botAI, Player* bot, PlannerIndexes const& indexes, Goals::Situation& situation,
                    Upkeep::SafeAreas const* sick)
{
    auto const usable = [&](Service const& service)
    { return Trades(bot, service) && (!sick || SafeWalk(bot, indexes, *sick, service)); };
    float const runSpeed = bot->GetSpeed(MOVE_RUN);
    float const reach = WorldTravel::ERRAND_MAX_WALK_MINUTES * MINUTE * runSpeed;
    uint32 const mapId = bot->GetMapId();
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    std::vector<Goals::VendorSeen> vendors;
    for (Nearby const& found : indexes.Nearest(ServiceKind::Vendor, mapId, x, y, z, VENDORS_SEEN, reach))
        if (Service const& vendor = indexes.services[found.index]; usable(vendor))
            vendors.push_back(
                {vendor.entry, found.distance, (vendor.npcFlags & UNIT_NPC_FLAG_REPAIR) != 0, sick != nullptr});
    for (Nearby const& found : indexes.Nearest(ServiceKind::Repairer, mapId, x, y, z, VENDORS_SEEN, reach))
        if (Service const& repairer = indexes.services[found.index]; usable(repairer))
        {
            Goals::VendorSeen const seen{repairer.entry, found.distance, true, sick != nullptr};
            vendors.insert(std::upper_bound(vendors.begin(), vendors.end(), seen,
                                            [](Goals::VendorSeen const& a, Goals::VendorSeen const& b)
                                            { return a.yards < b.yards; }),
                           seen);
            break;
        }
    std::vector<Goals::BagSeen> bags;
    for (Nearby const& found : indexes.Nearest(ServiceKind::BagVendor, mapId, x, y, z, BAG_VENDORS_SEEN, reach))
    {
        Service const& vendor = indexes.services[found.index];
        VendorItemData const* items = Trades(bot, vendor) ? sObjectMgr->GetNpcVendorItemList(vendor.entry) : nullptr;
        if (!items)
            continue;
        // Each size's cheapest bag: the largest alone hid the smaller bags a bot can afford (WT63c run 2)
        std::size_t const first = bags.size();
        for (VendorItem const* item : items->m_items)
        {
            ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
            if (!proto || proto->Class != ITEM_CLASS_CONTAINER || proto->SubClass != ITEM_SUBCLASS_CONTAINER)
                continue;
            uint32 const price = static_cast<uint32>(std::max<int32>(proto->BuyPrice, 0));
            Goals::BagSeen const seen{proto->ItemId, vendor.entry, found.distance, price, proto->ContainerSlots};
            auto const same = std::find_if(bags.begin() + first, bags.end(),
                                           [&seen](Goals::BagSeen const& bag) { return bag.slots == seen.slots; });
            if (same == bags.end())
                bags.push_back(seen);
            else if (price < same->price)
                *same = seen;
        }
    }
    Goals::FillErrandInputs(situation, vendors, bags, runSpeed, ItemUsageValue::SmallestBagSize(bot));
    situation.bags.sellableSlots = NewRpgInternal::SellableSlots(botAI);
    situation.bags.minutesToNextStop = static_cast<float>(sPlayerbotAIConfig.WorldTravel.ErrandNextStopMinutes);

    // The craft queue (professions WR14b): the kept path's first doable queued craft at a station and the nearest
    // station of its focus within the errand walk; a craft without a station brings the model nothing (it joins a trip
    // made anyway)
    float worthCopper = 0.0f;
    std::optional<BankMail::CraftOrder> const craft = BankMail::QueuedStationCraft(botAI, worthCopper);
    GoalApply::BotGoals const* goals = botAI->planner.goals.get();
    if (!craft || !goals || !goals->path)
        return;
    std::vector<Nearby> const stations = indexes.StationNearest(craft->focus, mapId, x, y, z, 1, reach);
    if (stations.empty())
        return;
    std::shared_ptr<Recipes::RecipeIndex const> const recipes = CurrentRecipeIndex();
    Recipes::Recipe const* recipe = recipes ? recipes->RecipeOf(craft->spell) : nullptr;
    situation.craft.worth = static_cast<float>(SkillPath::QueueWorth(*goals->path));
    situation.craft.walkMinutes = WorldTravel::WalkMinutes(stations.front().distance, runSpeed);
    situation.craft.safe = sick && SafeWalk(bot, indexes, *sick, indexes.services[stations.front().index]);
    situation.craft.minutes =
        recipe ? static_cast<float>(craft->casts * recipe->castMs) / static_cast<float>(MINUTE * IN_MILLISECONDS)
               : 0.0f;
}

// NR14: the stops of the planner's errands of their own within the errand walk that trade with the bot, nearest
// first, as the errand start finds them (TrainerSpawns, its mount vendors in reach): the class trainers of the bot's
// class and the riding trainers, the vendors that sell a mount for gold; a sick bot's (`sick`, NR3) safe when its
// walk stays in its safe area. Only the lists the last plan wants (GoalPlan::trainersWanted, mountVendorsWanted; both
// before a bot's first plan): the job finds the due needs, so a plan that finds a stop due whose list was skipped asks
// for the next plan at once (GoalPlan::ownStopsSkipped), whose snapshot seeks it.
// A pass over the trainers or the mount offers and a ring search that stops at STOPS_SEEN kept spawns per list; the
// job's NeedBridge::PlaceOwnStops picks the due stop's walk from them
void FillOwnStopsSeen(Player* bot, PlannerIndexes const& indexes, Goals::GoalPlan const* lastPlan,
                      Goals::Situation& situation, Upkeep::SafeAreas const* sick)
{
    bool const trainersWanted = !lastPlan || lastPlan->trainersWanted;
    bool const vendorsWanted = !lastPlan || lastPlan->mountVendorsWanted;
    situation.trainersSkipped = !trainersWanted;
    situation.mountVendorsSkipped = !vendorsWanted;
    if (!trainersWanted && !vendorsWanted)
        return;
    std::shared_ptr<UpkeepIndex const> const index = UpkeepIndexReady() ? CurrentUpkeepIndex() : nullptr;
    if (!index)
        return;
    constexpr std::size_t STOPS_SEEN = 16;
    float const runSpeed = bot->GetSpeed(MOVE_RUN);
    float const reach = WorldTravel::ERRAND_MAX_WALK_MINUTES * MINUTE * runSpeed;
    auto const seen = [&](ServiceKind kind, auto const& serves, std::vector<Goals::StopSeen>& out)
    {
        auto const keep = [&](Service const& spawn) { return serves(spawn.entry) && Trades(bot, spawn); };
        for (Nearby const& found : indexes.NearestWhere(kind, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                                        bot->GetPositionZ(), STOPS_SEEN, keep, reach))
        {
            Service const& spawn = indexes.services[found.index];
            out.push_back({spawn.entry, WorldTravel::WalkMinutes(found.distance, runSpeed),
                           sick && SafeWalk(bot, indexes, *sick, spawn)});
        }
    };
    if (trainersWanted)
    {
        std::vector<UpkeepTrainer const*> trainers;
        for (UpkeepTrainer const& trainer : index->trainers)
            if (trainer.kind == TrainerKind::Mount ||
                (trainer.kind == TrainerKind::Class && trainer.requirement == bot->getClass()))
                trainers.push_back(&trainer);
        seen(
            ServiceKind::Trainer,
            [&trainers](uint32 entry)
            {
                return std::any_of(
                    trainers.begin(), trainers.end(), [entry](UpkeepTrainer const* trainer)
                    { return std::binary_search(trainer->entries.begin(), trainer->entries.end(), entry); });
            },
            situation.trainersSeen);
    }
    if (!vendorsWanted)
        return;
    std::vector<uint32> mountVendors;
    for (VendorOffer const& offer : index->Offers(UpkeepStock::Mount))
        if (offer.GoldOnly())
            mountVendors.push_back(offer.vendorEntry);
    std::sort(mountVendors.begin(), mountVendors.end());
    mountVendors.erase(std::unique(mountVendors.begin(), mountVendors.end()), mountVendors.end());
    if (!mountVendors.empty())
        seen(
            ServiceKind::Vendor, [&mountVendors](uint32 entry)
            { return std::binary_search(mountVendors.begin(), mountVendors.end(), entry); },
            situation.mountVendorsSeen);
}

// The inns (WT63d, InterludeInputs.h) within the inn walk, only while the bind is far (as the inn trigger sees it): the
// model weighs a bind at an inn only then (GoalModel AddHearth). The stays near inns (NR.5) are noted either way. A
// sick bot's (`sick`, NR3) inn is safe when its walk there stays in its safe area
void FillInnSeen(PlayerbotAI* botAI, Player* bot, PlannerIndexes const& indexes, Goals::Situation& situation,
                 Upkeep::SafeAreas const* sick)
{
    bool const sameMap = bot->m_homebindMapId == bot->GetMapId();
    float const yards = sameMap ? bot->GetExactDist(bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ) : 0.0f;
    bool const bindFar = WorldTravel::BindIsFar(sameMap, yards);
    GoalApply::BotGoals* const goals = botAI->planner.goals.get();
    if (!bindFar && (!goals || !InnDwellMs()))
        return;
    float const reach = bindFar ? WorldTravel::INN_MAX_WALK_SECONDS * std::max(bot->GetSpeed(MOVE_RUN), 1.0f)
                                : WorldTravel::INN_BIND_FAR_YARDS;
    std::vector<Goals::InnSeen> const seen = InnsNear(bot, indexes, PlaceOf(bot), reach);
    uint32 const now = getMSTime();
    if (goals)
        Goals::NoteInnStays(goals->innStays, seen, PlaceOf(bot), now);
    if (!bindFar)
        return;
    Place const bind{bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ};
    FailedAttempts::Record const& record = botAI->GetTravel().Failures();
    std::vector<FailedAttempts::Entry> const live = FailedAttempts::LiveEntries(record, now);
    situation.inns = Goals::InnInputs(seen, bind, live);
    std::vector<Goals::InnStay> const none;
    std::vector<Goals::InnStay> const& stays = goals ? goals->innStays : none;
    Goals::MarkUnstayed(situation.inns, stays, now, InnDwellMs());
    if (sick)
        for (Goals::Inn& inn : situation.inns)
        {
            auto const it = std::find_if(seen.begin(), seen.end(),
                                         [&inn](Goals::InnSeen const& candidate) { return candidate.entry == inn.id; });
            inn.safe = it != seen.end() &&
                       SafeWalk(bot, indexes, *sick, inn.place.map, inn.place.x, inn.place.y, it->zoneId, 0);
        }
    if (!TestProbe::On(botAI->testConstraints))
        return;
    // A test bot's trace (WT63d); per seen inn (NE3, the inn list's causes B and C): usual, the refusing entry's cause,
    // the nearest live place entry's yards and cause (-1 none) and the record's entry count (full at MAX_ENTRIES)
    LOG_INFO("playerbots", "[Probe] {} inns seen {} weighed {} first {}", bot->GetName(), seen.size(),
             situation.inns.size(), situation.inns.empty() ? 0 : situation.inns.front().id);
    for (Goals::InnSeen const& inn : seen)
    {
        float nearest = -1.0f;
        std::string_view nearestCause = "none";
        for (FailedAttempts::Entry const& entry : live)
        {
            if (entry.key.kind != FailedAttempts::Kind::Place || entry.key.from.MapId != inn.place.map)
                continue;
            float const yards = std::sqrt((entry.key.from.X - inn.place.x) * (entry.key.from.X - inn.place.x) +
                                          (entry.key.from.Y - inn.place.y) * (entry.key.from.Y - inn.place.y) +
                                          (entry.key.from.Z - inn.place.z) * (entry.key.from.Z - inn.place.z));
            if (nearest < 0.0f || yards < nearest)
            {
                nearest = yards;
                nearestCause = entry.cause;
            }
        }
        FailedAttempts::Entry const* refusal = Goals::InnRefusal(live, inn.place);
        Goals::InnStay const* stay = Goals::FindStay(stays, inn.entry);
        LOG_INFO("playerbots", "[Probe] {} inn {} usual {} refused {} nearest {:.1f} {} record {} stayS {}",
                 bot->GetName(), inn.entry, Goals::UsualInn(bind, inn.place) ? 1 : 0, refusal ? refusal->cause : "none",
                 nearest, nearestCause, record.count,
                 stay ? static_cast<int32>((now - stay->sinceMs) / IN_MILLISECONDS) : -1);
    }
}

Goals::Situation SituationOf(PlayerbotAI* botAI, Player* bot)
{
    Goals::Situation situation;
    situation.here.hub = NO_HUB;  // the job finds it from its circuit (Goals::CircuitHub)
    // Every planner bot gets its quest route, and the circuit only while the route has no order (questing WQ18b); a
    // test holds a bot on the circuit (constraint circuit)
    situation.onCircuit = TestConstraints::HeldOnCircuit(botAI->testConstraints);
    situation.healthPercent = bot->GetHealthPct();
    uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
    situation.manaPercent = maxMana > 0 ? static_cast<float>(PERCENT) * static_cast<float>(bot->GetPower(POWER_MANA)) /
                                              static_cast<float>(maxMana)
                                        : -1.0f;
    // Class quests (upkeep WU20a): a governed bot's (one that keeps its possessions) facts for the unlock bonus; the
    // job adds the trainer's spells
    situation.questsHeld = !TestConstraints::QuestChoicesAllowed(botAI->testConstraints);  // a test's noquest
    situation.fightsHeld = !TestConstraints::StartsFights(botAI->testConstraints);         // a test's nofight
    situation.questingOnly = sPlayerbotAIConfig.Lone.TestQuestingOnly;  // the questing test's setting (WQ37)
    situation.governed = botAI->KeepsPossessions();
    if (situation.governed)
    {
        // At the level cap the next level's XP is 0: the last level's is the bonus's basis (DECISIONS WU.30)
        uint32 const level = bot->GetLevel();
        situation.levelXp = ClassQuests::LevelXpBasis(bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP),
                                                      level > 1 ? sObjectMgr->GetXPForLevel(level - 1) : 0);
        situation.knownClassQuestSpells = ClassQuestCompletion::KnownSpells(bot);
    }
    // WQ34: as the log's accept reads it (a failed hand-in too): a held given-up quest gives no need
    situation.givenUpQuests = QuestLog::Read(QuestFailRecord::RowsOf(bot->GetGUID().GetCounter())).givenUp;
    if (std::shared_ptr<PlannerIndexes const> const indexes = IndexesReady() ? Indexes() : nullptr)
    {
        // NR3 (NR.4): a sick bot's safe areas, where it stands and where its hearthstone is bound
        std::optional<Upkeep::SafeAreas> sick;
        if (bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS))
        {
            sick = SafeAreasAt(bot, *indexes, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                               bot->GetZoneId(), bot->GetAreaId(), nullptr);
            situation.sickSafeHere = Upkeep::IsSafe(*sick);
            AreaTableEntry const* bindArea = sAreaTableStore.LookupEntry(bot->m_homebindAreaId);
            uint32 const bindZone = bindArea ? (bindArea->zone ? bindArea->zone : bindArea->ID) : 0;
            situation.sickHearthSafe =
                Upkeep::IsSafe(SafeAreasAt(bot, *indexes, bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY,
                                           bindZone, bot->m_homebindAreaId, nullptr));
        }
        FillErrandSeen(botAI, bot, *indexes, situation, sick ? &*sick : nullptr);
        GoalApply::BotGoals const* const goals = botAI->planner.goals.get();
        FillOwnStopsSeen(bot, *indexes, goals ? goals->plan.get() : nullptr, situation, sick ? &*sick : nullptr);
        FillInnSeen(botAI, bot, *indexes, situation, sick ? &*sick : nullptr);
    }
    return situation;
}

// The upkeep row sick_area at recover's hand-over of a sick bot (NR3): the safe area it stands in, the sickness left
void WriteSickArea(PlayerbotAI* botAI)
{
    if (!WorldLog::Active())
        return;
    Player* bot = botAI->GetBot();
    std::shared_ptr<PlannerIndexes const> const indexes = IndexesReady() ? Indexes() : nullptr;
    Aura const* sickness = bot->GetAura(Upkeep::SPELL_RESURRECTION_SICKNESS);
    float const seconds =
        sickness ? static_cast<float>(std::max<int32>(sickness->GetDuration(), 0)) / IN_MILLISECONDS : 0.0f;
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "sick_area",
                       indexes ? Upkeep::SafeAreaWord(SafeAreasHere(bot, *indexes)) : "unknown", seconds);
}

// The goals job: the snapshot with the current goal, the plan's kept quests and the log quests' ages
void SubmitGoals(PlayerbotAI* botAI, Player* bot, Planner::BotState& state, uint32 now)
{
    GoalApply::BotGoals& goals = GoalsOf(botAI, bot);
    Snapshot snapshot = TimedSnapshot(bot, state.stateVersion, goals);
    // NR3c: the job plans from the snapshot's hand-over (the sick gate), so the edge is the snapshot's as much as the
    // errand trigger's (NoteSickOpen): an edge seen here writes its row only, the job is the plan; a hand-over this
    // snapshot found closed is the trigger's edge again (run 1: a plan that rested on a closed snapshot after the
    // trigger had seen it open waited 290 s for another change; an errand begun on the snapshot's hand-over wrote its
    // row only at its end)
    if (snapshot.sickMs && GoalApply::SickOpenChanged(goals, snapshot.sickErrandOpen))
        WriteSickArea(botAI);
    // A test keeping the hearth off: no hearth or inn goal either (GoalModel AddHearth)
    if (!TestConstraints::HearthAllowed(botAI->testConstraints))
        snapshot.hasHearthstone = false;
    GoalApply::Active const& current = goals.slot.Query();
    snapshot.currentGoal = {static_cast<uint32>(current.kind), current.target, current.startedMs};
    std::vector<uint32> log;
    log.reserve(snapshot.quests.size());
    for (QuestSlot const& slot : snapshot.quests)
        log.push_back(slot.quest);
    GoalApply::UpdateSeen(goals.seen, std::move(log), now);
    // The needs rates read the bot's slot measure here; the job computes the needs list from them (upkeep WU16)
    // Item use's answer is a map-thread cache: the path's share of it is copied into the job here (WR11)
    Goals::Situation situation = SituationOf(botAI, bot);
    // The cells a dry order closed to the bot (questing WQ18b): its route passes them and their neighbours over
    situation.closedCells = GoalApply::ClosedCells(goals, LogNow(bot), now, BlockMs());
    Compute compute = GoalsCompute(goals.keep, GoalApply::ProgressOf(goals.seen, now), situation,
                                   BotNeedRates(botAI, snapshot), PathInputsOf(botAI));
    Job job{JobKind::Goals, std::move(snapshot), std::move(compute)};
    state.lastSnapshotMs = std::max<uint32>(now, 1);
    state.snapshotDue = false;
    goals.jobOut = Pool().Submit(bot->GetGUID().GetRawValue(), std::move(job));
    state.pending = goals.jobOut;
}
}  // namespace

Goals::Situation LiveSituation(PlayerbotAI* botAI, Player* bot) { return SituationOf(botAI, bot); }

std::vector<Goals::InnSeen> InnsNear(Player* bot, PlannerIndexes const& indexes, Place const& at, float maxYards)
{
    std::vector<Goals::InnSeen> inns;
    uint32 const level = bot->GetLevel();
    for (Nearby const& found : indexes.NearestAll(ServiceKind::Innkeeper, at.map, at.x, at.y, at.z, maxYards))
    {
        Service const& innkeeper = indexes.services[found.index];
        if (!Trades(bot, innkeeper))
            continue;
        // The bind spot in front of him, as TravelMgr's inn list has it
        Place const spot{innkeeper.mapId, innkeeper.x + std::cos(innkeeper.orientation) * WorldTravel::INN_FRONT_YARDS,
                         innkeeper.y + std::sin(innkeeper.orientation) * WorldTravel::INN_FRONT_YARDS,
                         innkeeper.z + WorldTravel::INN_FRONT_RISE};
        ZoneRecord const* zone = indexes.FindZone(innkeeper.zoneId);
        float const dx = spot.x - at.x;
        float const dy = spot.y - at.y;
        float const dz = spot.z - at.z;
        inns.push_back({innkeeper.entry, spot, std::sqrt(dx * dx + dy * dy + dz * dz),
                        zone && StartingHistory::BracketFits(level, {zone->low, zone->high}), innkeeper.zoneId});
    }
    return inns;
}

bool HandleInnsCommand(ChatHandler* handler, char const* args)
{
    std::string name = args ? args : "";
    Player* bot = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    std::shared_ptr<PlannerIndexes const> const indexes = IndexesReady() ? Indexes() : nullptr;
    if (!bot || !indexes)
    {
        handler->SendErrorMessage("Usage: playerbots planner inns <online player> (with the indexes ready)");
        return false;
    }
    // At every hub (the sample positions of each map): the old pick, the nearest inn of the team's startup list on
    // the map (each has a level bracket), and the new, the nearest innkeeper of the index the bot can use whose zone
    // has a level bracket. Both lists put the spot in front of the innkeeper; a spot within a yard is the same inn
    constexpr float SAME_YARDS = 1.0f;
    std::vector<TravelMgr::InnInfo> const& old = sTravelMgr.GetInns(bot->GetTeamId());
    auto const yardsTo = [](Place const& a, float x, float y, float z)
    { return std::sqrt((a.x - x) * (a.x - x) + (a.y - y) * (a.y - y) + (a.z - z) * (a.z - z)); };
    auto const yardsToOld = [&yardsTo](Place const& a, TravelMgr::InnInfo const& inn)
    { return yardsTo(a, inn.loc.GetPositionX(), inn.loc.GetPositionY(), inn.loc.GetPositionZ()); };
    auto const bracketed = [&indexes](uint32 zoneId)
    {
        ZoneRecord const* zone = indexes->FindZone(zoneId);
        return zone && (zone->low || zone->high);
    };
    std::map<uint32, std::array<uint32, 3>> perMap;  // samples, agree, differ
    std::map<std::string, uint32> differences;       // a difference's line, the samples that showed it
    for (Hub const& hub : indexes->hubs)
    {
        Place const at{hub.mapId, hub.x, hub.y, hub.z};
        TravelMgr::InnInfo const* oldPick = nullptr;
        float oldYards = std::numeric_limits<float>::max();
        for (TravelMgr::InnInfo const& inn : old)
            if (inn.loc.GetMapId() == at.map && yardsToOld(at, inn) < oldYards)
            {
                oldYards = yardsToOld(at, inn);
                oldPick = &inn;
            }
        std::vector<Goals::InnSeen> const seen = InnsNear(bot, *indexes, at, 0.0f);
        Goals::InnSeen const* newPick = nullptr;
        for (Goals::InnSeen const& inn : seen)
            if (bracketed(inn.zoneId) && (!newPick || inn.yards < newPick->yards))
                newPick = &inn;
        std::array<uint32, 3>& counts = perMap[at.map];
        ++counts[0];
        if ((!oldPick && !newPick) || (oldPick && newPick && yardsToOld(newPick->place, *oldPick) <= SAME_YARDS))
        {
            ++counts[1];
            continue;
        }
        ++counts[2];
        // The cause: the old pick missing from the new list, else the new pick missing from the old list, else a tie
        Goals::InnSeen const* oldInNew = nullptr;
        if (oldPick)
            for (Goals::InnSeen const& inn : seen)
                if (yardsToOld(inn.place, *oldPick) <= SAME_YARDS)
                    oldInNew = &inn;
        bool newInOld = false;
        if (newPick)
            for (TravelMgr::InnInfo const& inn : old)
                newInOld = newInOld || (inn.loc.GetMapId() == at.map && yardsToOld(newPick->place, inn) <= SAME_YARDS);
        std::string cause = "tie";
        if (oldPick && !oldInNew)
            cause = indexes->Nearest(ServiceKind::Innkeeper, at.map, oldPick->loc.GetPositionX(),
                                     oldPick->loc.GetPositionY(), oldPick->loc.GetPositionZ(), 1,
                                     WorldTravel::INN_FRONT_YARDS + SAME_YARDS)
                            .empty()
                        ? "old_only:not_an_index_innkeeper"
                        : "old_only:no_trade_with_the_bot";
        else if (oldPick && !bracketed(oldInNew->zoneId))
            cause = Acore::StringFormat("old_only:index_zone_{}_unbracketed_old_zone_{}", oldInNew->zoneId,
                                        oldPick->zoneId);
        else if (newPick && !newInOld)
            cause = "new_only:not_in_the_old_list";
        std::string const oldText = oldPick
                                        ? Acore::StringFormat("({:.0f}, {:.0f}) zone {}", oldPick->loc.GetPositionX(),
                                                              oldPick->loc.GetPositionY(), oldPick->zoneId)
                                        : std::string("none");
        std::string const newText = newPick ? Acore::StringFormat("{} ({:.0f}, {:.0f}) zone {}", newPick->entry,
                                                                  newPick->place.x, newPick->place.y, newPick->zoneId)
                                            : std::string("none");
        ++differences[Acore::StringFormat("map {} old {} new {}: {}", at.map, oldText, newText, cause)];
    }
    std::array<uint32, 3> total{};
    for (auto const& [map, counts] : perMap)
    {
        handler->PSendSysMessage("INNS {} map {}: samples {}, agree {}, differ {}", bot->GetName(), map, counts[0],
                                 counts[1], counts[2]);
        for (std::size_t i = 0; i < total.size(); ++i)
            total[i] += counts[i];
    }
    for (auto const& [line, samples] : differences)
        handler->PSendSysMessage("INNS {} differ {} ({} samples)", bot->GetName(), line, samples);
    handler->PSendSysMessage("INNS {} total: samples {}, agree {}, differ {}, distinct differences {}", bot->GetName(),
                             total[0], total[1], total[2], differences.size());
    return true;
}

GearWear ReadGearWear(Player* bot)
{
    uint64 durability = 0;
    uint64 maxDurability = 0;
    uint64 lowest = PERCENT;
    uint32 broken = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            uint32 const current = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            uint32 const most = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            durability += current;
            maxDurability += most;
            if (!most)
                continue;  // never wears
            lowest = std::min<uint64>(lowest, static_cast<uint64>(current) * PERCENT / most);
            if (!current)
                ++broken;
        }
    GearWear wear;
    wear.durabilityPercent =
        static_cast<uint8>(maxDurability ? std::min<uint64>(durability * PERCENT / maxDurability, PERCENT) : PERCENT);
    wear.lowestPercent = static_cast<uint8>(lowest);
    wear.broken = static_cast<uint8>(broken);
    return wear;
}

// The items DurabilityRepairAll repairs: equipped, the backpack, the bags themselves and the items in them
RepairPrice PriceRepair(Player* bot)
{
    RepairPrice price;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        AddRepair(price, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag const* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                AddRepair(price, bag->GetItemByPos(static_cast<uint8>(slot)));
    return price;
}

PlannerPool& Pool()
{
    static PlannerPool pool;
    return pool;
}

void StartPlanner()
{
    Pool().Start(sPlayerbotAIConfig.WorldTravel.PlannerThreads, RecordPerf);
    LOG_INFO("server.loading", "Planner pool started: {} worker thread(s)", Pool().Stats().threads);
}

void StopPlanner() { Pool().Stop(); }

// The game's answer (questing WQ41a, decision WQ.47): the planner reads it at any distance, on every map. The filter
// before the ask only saves work: the core refuses a quest of the other side or below its minimum level itself, and
// a quest at or below GetGrayLevel is grey to Take::IsGrey too (one line since WQ34). A quest whose availability reads
// the world's events or states is not asked (QuestRecord::worldState, CONTRACT C3); the index walks in id order, so the
// answer is sorted
void FillTakeable(Player* bot, Snapshot& snapshot)
{
    if (PlannerPool::IsWorkerThread())
        LOG_ERROR("playerbots", "Thread check: planner take set built on a worker");
    RowTimer const timer("planner take set");
    snapshot.takeable.clear();
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
        return;
    std::uint8_t const level = bot->GetLevel();
    std::int32_t const grey = Acore::XP::GetGrayLevel(level);
    std::uint8_t const side = SideOfRace(bot->getRace(true));
    std::uint32_t const sideMask = side < SIDES ? SIDE_RACE_MASKS[side] : 0;
    for (QuestRecord const& record : quests->quests)
    {
        if (record.minLevel > level || record.worldState ||
            (sideMask && record.raceMask && !(record.raceMask & sideMask)))
            continue;
        if (record.level > 0 && record.level <= grey)
            continue;
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(record.id); quest && bot->CanTakeQuest(quest, false))
            snapshot.takeable.push_back(record.id);
    }
}

namespace
{
// What the game's answer depends on, as the plan text names it (WQ41a): the level, the log (its quests and their
// states), the rewarded quests, the skills (every skill a quest asks for is a snapshot skill) and the standings, and
// the index it walks. The full pass costs more than the snapshot's other gathers together (lab13, run
// scenario-1795758758745: 628 of 839 us per snapshot), so it runs only when this key changed. What else the game
// reads (items, auras, the bot's zone or area, spells, achievements, the daily and weekly resets) waits for the next
// change, at most TAKE_ANSWER_MS (WQ41b: every answer has an exit), or an order's end at a giver (AskTakesAgain); the
// accept at the giver stays the authority
constexpr uint32 TAKE_ANSWER_MS = 5 * MINUTE * IN_MILLISECONDS;

std::uint64_t TakeKey(Player* bot, Snapshot const& snapshot, QuestIndex const* quests)
{
    std::uint64_t key = 14695981039346656037ull;  // FNV-1a over 64-bit words
    auto const mix = [&key](std::uint64_t value)
    {
        key ^= value;
        key *= 1099511628211ull;
    };
    mix(reinterpret_cast<std::uintptr_t>(quests));
    mix(snapshot.level);
    for (QuestSlot const& slot : snapshot.quests)
        mix((static_cast<std::uint64_t>(slot.quest) << 32) | slot.state);
    mix(snapshot.rewardedQuests.size());
    for (std::uint32_t const quest : snapshot.rewardedQuests)
        mix(quest);
    for (Skill const& skill : snapshot.skills)
        mix((static_cast<std::uint64_t>(skill.id) << 16) | skill.value);
    for (auto const& [listId, faction] : bot->GetReputationMgr().GetStateList())
        mix((static_cast<std::uint64_t>(faction.ID) << 32) | static_cast<std::uint32_t>(faction.Standing));
    return key;
}

void AnswerTakes(Player* bot, Snapshot& snapshot, GoalApply::BotGoals& goals)
{
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    std::uint64_t key = 0;
    {
        RowTimer const timer("planner take key");
        key = TakeKey(bot, snapshot, quests.get());
    }
    uint32 const nowMs = getMSTime();
    if (key == goals.takeKey && getMSTimeDiff(goals.takeMs, nowMs) < TAKE_ANSWER_MS)
    {
        snapshot.takeable = goals.takeable;
        return;
    }
    FillTakeable(bot, snapshot);
    goals.takeKey = key;
    goals.takeMs = nowMs;
    goals.takeable = snapshot.takeable;
}
}  // namespace

void AskTakesAgain(PlayerbotAI* botAI)
{
    if (botAI && botAI->planner.goals)
        botAI->planner.goals->takeKey = 0;
}

Snapshot FillSnapshot(Player* bot, std::uint32_t stateVersion)
{
    if (PlannerPool::IsWorkerThread())
        LOG_ERROR("playerbots", "Thread check: planner snapshot built on a worker");

    Snapshot snapshot;
    snapshot.bot = bot->GetGUID().GetRawValue();
    snapshot.stateVersion = stateVersion;
    snapshot.takenMs = getMSTime();
    snapshot.level = bot->GetLevel();
    snapshot.race = bot->getRace(true);
    snapshot.cls = bot->getClass();
    snapshot.team = bot->GetTeamId() == TEAM_HORDE ? 1 : 0;
    snapshot.xp = bot->GetUInt32Value(PLAYER_XP);
    snapshot.position = PlaceOf(bot);
    snapshot.zone = bot->GetZoneId();
    snapshot.area = bot->GetAreaId();
    snapshot.money = bot->GetMoney();
    FillBags(bot, snapshot);
    FillBankMail(bot, snapshot);
    for (std::uint16_t const skill : SNAPSHOT_SKILLS)
        if (uint16 const value = bot->GetPureSkillValue(skill))
            snapshot.skills.push_back(
                {static_cast<uint16>(skill), value, static_cast<uint16>(bot->GetPureMaxSkillValue(skill))});
    FillGatherSight(bot, snapshot);
    FillQuests(bot, snapshot);
    snapshot.hasHearthstone = bot->HasItemCount(StartingHistory::ITEM_HEARTHSTONE, 1);
    snapshot.hearth = {bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ};
    snapshot.hearthCooldownMs = bot->GetSpellCooldownDelay(SPELL_HEARTHSTONE);
    for (uint32 node = 1; node <= TAXI_WORDS * 32; ++node)
        if (bot->m_taxi.IsTaximaskNodeKnown(node))
            snapshot.taxiMask[(node - 1) / 32] |= 1u << ((node - 1) % 32);
    snapshot.traits = BotTraitMgr::instance().Find(bot->GetGUID().GetCounter());
    snapshot.runSpeed = bot->GetSpeed(MOVE_RUN);
    snapshot.mounted = bot->IsMounted();
    // Resurrection sickness: the planner ranks Rest first until it ends (upkeep WU16), or once recover has handed the
    // bot over a goal its sick gate passes (NR3). The rest is at a clear point: the bot's own, else the one recover
    // walks it to (FindSafePoint around the bot within RISE_YARDS, as ReviveFromCorpseAction asks it)
    if (Aura const* sickness = bot->GetAura(Upkeep::SPELL_RESURRECTION_SICKNESS))
    {
        snapshot.sickMs = static_cast<std::uint32_t>(std::max<int32>(sickness->GetDuration(), 0));
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        snapshot.sickErrandOpen = botAI && WorldTravel::SickErrandOpen(botAI);
        if (botAI && !snapshot.sickErrandOpen && !Awareness::JudgePoint(botAI, *bot).clear)
        {
            AwarenessRules::SafePoint const safe = Awareness::FindSafePoint(botAI, *bot, AwarenessRules::RISE_YARDS);
            if (safe.found && safe.clear)
                snapshot.sickRest = Place{bot->GetMapId(), safe.x, safe.y, safe.z};
        }
    }
    if (Group const* group = bot->GetGroup())
    {
        snapshot.groupSize = static_cast<uint8>(group->GetMembersCount());
    }
    FillUpkeep(bot, snapshot);
    // The route search's view as a trip asks it (NewRpgTravelAction::RequestRoute): transports on, the hearthstone
    // while it is held, a test's constraints. The trip's freeride is a request field, not priced here (WN1a)
    snapshot.route = TravelRoute::FillSnapshot(bot, 0);
    snapshot.route.allowTransport = true;
    if (PlayerbotAI const* botAI = GET_PLAYERBOT_AI(bot))
    {
        snapshot.route.hasHearthstone =
            snapshot.route.hasHearthstone && TestConstraints::HearthAllowed(botAI->testConstraints);
        if (!TestConstraints::TaxiAllowed(botAI->testConstraints))
            snapshot.route.knownTaxi.fill(0);
        if (!TestConstraints::OwnFlyingAllowed(botAI->testConstraints))
            snapshot.route.flyOutland = snapshot.route.flyNorthrend = false;
    }
    return snapshot;
}

RouteCostInputs RouteCostInputsNow()
{
    RouteCostInputs inputs;
    inputs.stuck = TravelRoute::StuckBook::Instance().Current();
    inputs.params = TravelRoute::ConfiguredParams();
    // A fly leg is priced by its straight bound, not flown: no flight search in a job, so its searches stay short
    inputs.params.flightMaps = nullptr;
    return inputs;
}

TravelCost JobTravel(RouteCostInputs const& inputs, TravelRoute::Snapshot const& bot)
{
    if (!TravelGraph::Instance().IsReady())
        return {};
    auto const record = [](RouteCostCounts const& counts)
    {
        routeCosts.Add(counts);
        if (counts.searches)
            if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "planner route cost"))
                row->Record(counts.micros);
    };
    return RouteCost::Hook(
        std::make_shared<RouteCost>(TravelGraph::Instance(), bot, inputs, ROUTE_SEARCHES_PER_JOB, record));
}

void OnStatusEnd(PlayerbotAI* botAI, StatusEnd::Outcome outcome, bool toIdle, char const* note)
{
    Planner::BotState& state = botAI->planner;
    if (toIdle)
        state.snapshotDue = true;  // a goal ended
    if (state.goals)
    {
        GoalApply::NoteStatusEnd(*state.goals, outcome, toIdle, note);
        // The interlude that holds the slot ended: a failure stamps its kind's retry time (3.19 (6)); an errand that
        // finished reads whether its repair is still due (the gear once, only at that end)
        Goals::GoalKind const kind = state.goals->slot.Query().kind;
        bool const repairDue = kind == Goals::GoalKind::Errand && outcome == StatusEnd::Outcome::Finished &&
                               WorldTravel::RepairErrandWanted(botAI->GetBot());
        GoalApply::NoteInterludeEnd(*state.goals, kind, outcome, getMSTime(), repairDue, botAI->GetBot()->GetLevel());
    }
}

void OnOrderEnd(PlayerbotAI* botAI, QuestOrder const& order, QuestOrderEnd end, bool reached)
{
    Planner::BotState& state = botAI->planner;
    Player* const bot = botAI->GetBot();
    if (!state.goals || !bot)
        return;
    GoalApply::BotGoals& goals = *state.goals;
    bool const route = GoalApply::IsRouteOrder(goals, order);
    uint64 const log = LogNow(bot);
    uint32 const now = getMSTime();
    if (GoalApply::EndRouteOrder(goals, order, end, log, now, BlockMs(), reached))
        NoteStalled(bot, goals);  // the closed cell's row
    if (route && GoalApply::EndInPlace(goals, SpotOf(bot), log, now, BlockMs()))
        NoteStalled(bot, goals);  // the route goal's row
}

void OnOrderFailed(PlayerbotAI* botAI, QuestOrder const& order, StatusEnd::Outcome outcome, char const* note)
{
    Planner::BotState& state = botAI->planner;
    Player* const bot = botAI->GetBot();
    if (!state.goals || !bot || !GoalApply::FailsOrder(outcome))
        return;
    GoalApply::BotGoals& goals = *state.goals;
    bool const route = GoalApply::IsRouteOrder(goals, order);
    uint64 const log = LogNow(bot);
    uint32 const now = getMSTime();
    if (GoalApply::FailRouteOrder(goals, order, outcome, note, log, now, BlockMs()))
        NoteStalled(bot, goals);  // the closed cell's row
    if (route && GoalApply::EndInPlace(goals, SpotOf(bot), log, now, BlockMs()))
        NoteStalled(bot, goals);  // the route goal's row
}

bool Owns(PlayerbotAI* botAI)
{
    return GoalApply::Owns(botAI->KeepsPossessions(), TestConstraints::GateOf(botAI->testConstraints).planner);
}

void SituationChanged(PlayerbotAI* botAI)
{
    Planner::BotState& state = botAI->planner;
    ++state.stateVersion;  // a job out from before the change would answer the old situation and spend the flag
    state.snapshotDue = true;
    if (state.goals)
        state.goals->situationChanged = true;
}

Upkeep::SafeAreas SafeAreasHere(Player* bot, PlannerIndexes const& indexes)
{
    return SafeAreasAt(bot, indexes, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetZoneId(),
                       bot->GetAreaId(), nullptr);
}

bool SafeWalkTo(Player* bot, PlannerIndexes const& indexes, Upkeep::SafeAreas const& here, std::uint32_t mapId, float x,
                float y, float z)
{
    if (mapId != bot->GetMapId())
        return false;
    uint32 zoneId = 0;
    uint32 areaId = 0;
    bot->GetMap()->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, x, y, z);
    return SafeWalk(bot, indexes, here, mapId, x, y, zoneId, areaId);
}

void NoteSickOpen(PlayerbotAI* botAI, bool open)
{
    Planner::BotState& state = botAI->planner;
    if (!state.goals || !GoalApply::SickOpenChanged(*state.goals, open))
        return;
    SituationChanged(botAI);
    WriteSickArea(botAI);
}

QuestFailRows::Sums& QuestStallsOf(PlayerbotAI* botAI, Player* bot) { return GoalsOf(botAI, bot).questStalls; }

void ResetPlanner(PlayerbotAI* botAI, StatusEnd::ResetList const& list)
{
    // The path is the bot's profession plan, neither its place nor its AI state: dropped with the goals, it took the
    // keep of every material with it, and a due material withdrawn from the bank was sold at the next vendor (NR15a)
    GoalApply::BotGoals const* const goals = botAI->planner.goals.get();
    std::shared_ptr<SkillPath::Path const> path =
        goals && list.skillPath == StatusEnd::ResetEffect::Kept ? goals->path : nullptr;
    uint32 const pathMs = goals ? goals->pathMs : 0;
    StatusEnd::ApplyReset(list.planner, botAI->planner);
    Player* const bot = botAI->GetBot();
    if (!path || !bot)
        return;
    GoalApply::BotGoals& fresh = GoalsOf(botAI, bot);
    fresh.path = std::move(path);
    fresh.pathMs = pathMs;
}

void NotePickup(PlayerbotAI* botAI, bool accepted, bool giverStillOffers)
{
    Planner::BotState& state = botAI->planner;
    if (!state.goals || !Owns(botAI))
        return;
    QuestLog::Round& round = state.goals->pickups;
    uint32 const now = getMSTime();
    if (accepted)
        QuestLog::NoteAccept(round, now);
    if (!QuestLog::PlanDue(round, now, giverStillOffers))
        return;
    round = {};
    SituationChanged(botAI);
}

void NoteBagSlots(PlayerbotAI* botAI)
{
    Planner::BotState& state = botAI->planner;
    if (!state.goals)
        return;
    uint16 const seen = state.goals->bagSlotsSeen;
    uint16 const slots = static_cast<uint16>(BagSlots(botAI->GetBot()));
    bool const changed = GoalApply::BagSlotsChanged(*state.goals, slots);
    if (TestProbe::On(botAI->testConstraints) && seen != slots)  // a test bot's trace (WT63c)
        LOG_INFO("playerbots", "[Probe] {} bag slots {} -> {} edge {}", botAI->GetBot()->GetName(), seen, slots,
                 changed);
    if (changed)
        SituationChanged(botAI);
}

void NoteBindFar(PlayerbotAI* botAI, bool bindFar)
{
    Planner::BotState& state = botAI->planner;
    if (!state.goals)
        return;
    uint8 const seen = state.goals->bindSeen;
    bool const changed = GoalApply::BindFarChanged(*state.goals, bindFar);
    if (TestProbe::On(botAI->testConstraints) && seen != state.goals->bindSeen)  // a test bot's trace (WT63d)
        LOG_INFO("playerbots", "[Probe] {} bind seen {} -> {} edge {}", botAI->GetBot()->GetName(), seen,
                 state.goals->bindSeen, changed);
    // Snapshots can be minutes apart, so a stay the bot left ends here too, and one that reached the dwell under a far
    // bind is a changed situation: the planner may now bind there (NR.5)
    Goals::DropLeftStays(state.goals->innStays, PlaceOf(botAI->GetBot()));
    bool const ripened = bindFar && Goals::StayRipened(state.goals->innStays, getMSTime(), InnDwellMs());
    if (ripened && TestProbe::On(botAI->testConstraints))
        LOG_INFO("playerbots", "[Probe] {} inn stay reached the dwell", botAI->GetBot()->GetName());
    if (changed || ripened)
        SituationChanged(botAI);
}

uint32 OrderedBag(PlayerbotAI* botAI)
{
    // A bot whose planner a test holds keeps its goals from before the hold: its errands are the trigger's
    GoalApply::BotGoals const* goals = botAI->planner.goals.get();
    return goals && Owns(botAI) ? GoalApply::OrderedBagOf(*goals) : 0;
}

void NoteBagBought(PlayerbotAI* botAI)
{
    if (GoalApply::BotGoals* goals = botAI->planner.goals.get())
        goals->bagBought = true;
}

void NoteOwnStopDone(PlayerbotAI* botAI)
{
    if (GoalApply::BotGoals* goals = botAI->planner.goals.get())
        goals->ownStopDone = true;
}

bool StatusOver(PlayerbotAI* botAI, uint32 status, bool timerOver, bool sick)
{
    Player* bot = botAI->GetBot();
    GoalApply::BotGoals const* goals = botAI->planner.goals.get();
    GoalApply::PlannedAnswer answer;
    if (goals &&
        (goals->inFallback || (goals->slot.Query().source == GoalApply::Source::Planner && goals->slot.Query().Is())))
    {
        GoalApply::PlannedFacts facts;
        facts.status = status;
        facts.nowMs = getMSTime();
        facts.owns = Owns(botAI);
        facts.inFallback = goals->inFallback;
        facts.sick = sick;
        facts.statusMs = GetMSTimeDiffToNow(botAI->rpgInfo.startT);
        facts.healthPercent = bot->GetHealthPct();
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        facts.manaPercent = maxMana ? static_cast<float>(PERCENT) * static_cast<float>(bot->GetPower(POWER_MANA)) /
                                          static_cast<float>(maxMana)
                                    : -1.0f;
        constexpr float AT_BIND_YARDS = 10.0f;  // the stone sets the bot down at its bind
        facts.atBind = bot->GetMapId() == bot->m_homebindMapId &&
                       bot->GetExactDist(bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ) <= AT_BIND_YARDS;
        answer = GoalApply::PlannedEnd(goals->slot.Query(), facts);
    }
    if (answer.planned ? !answer.ends : !timerOver)
        return false;
    if (TestProbe::On(botAI->testConstraints))  // a test bot's trace (WT63e): what ended the status
        LOG_INFO("playerbots", "[Probe] {} status {} end {} after {} ms", bot->GetName(), status,
                 answer.planned ? answer.word : "timer", GetMSTimeDiffToNow(botAI->rpgInfo.startT));
    if (answer.planned)
    {
        RpgStatus::End(botAI, answer.outcome, {answer.word});
        // The stone moved the bot (NR3: a sick bot's hearth_bind): the next plan is made from its new place
        if (answer.outcome == StatusEnd::Outcome::Arrived)
            SituationChanged(botAI);
    }
    else
        RpgStatus::End(botAI, StatusEnd::Outcome::TimedOut);
    return true;
}

void PlannerHook(PlayerbotAI* botAI, Player* bot, std::function<bool(uint32)> const& workable)
{
    RowTimer const timer("planner hook");
    // The scope (KeepsPossessions: two flags and the master) is compared on every update, beside the state key, so a
    // master that changes nothing in the key still takes the bot out of scope. Bit 31 keeps the verdict
    Planner::BotState& state = botAI->planner;
    uint32 const key = StateKey(bot) | (botAI->KeepsPossessions() ? STATE_SCOPE_BIT : 0);
    if (key != state.stateKey)
    {
        state.stateKey = key;
        ++state.stateVersion;
        state.snapshotDue = true;
        if (state.goals)
            state.goals->situationChanged = true;
    }
    if (!(state.stateKey & STATE_SCOPE_BIT))
        return;
    // A test's hold on the planner (TestConstraints): no jobs, no goals taken
    if (!TestConstraints::GateOf(botAI->testConstraints).planner)
        return;

    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    uint32 const now = getMSTime();

    if (state.pending)
    {
        GoalApply::BotGoals* const goals = state.goals.get();
        if (std::optional<Result> const result = Pool().TryTake(bot->GetGUID().GetRawValue()))
        {
            // The pool keeps only the newest job's result (Submit), so this is the job the bot waits for; after a
            // reset of the AI (no job out) it is taken and counted but applied to nothing
            state.pending = false;
            Verdict const verdict =
                Recheck(*result, {bot->IsAlive(), state.stateVersion, PlaceOf(bot), now}, ConfiguredLimits());
            Pool().CountVerdict(verdict);
            // A bot on a route walks: how often its results are rejected for the position (questing WQ18b)
            if (goals && GoalApply::RunsRoute(goals->slot.Query()))
            {
                routeResults.fetch_add(1, std::memory_order_relaxed);
                if (verdict == Verdict::Position)
                {
                    routeRejectedMoved.fetch_add(1, std::memory_order_relaxed);
                    // The night list plans_rejected_moved counts these rows (part 3): one per rejected result
                    if (WorldLog::Active())
                        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Circuit, "plan:rejected", "moved", 0.0f);
                }
            }
            if (TestProbe::On(botAI->testConstraints))  // a test bot's trace (WT63c): a result not used is no plan
                LOG_INFO("playerbots", "[Probe] {} plan result verdict {}", bot->GetName(), static_cast<int>(verdict));
            if (goals && goals->jobOut)
            {
                goals->jobOut = false;
                if (verdict == Verdict::Use)
                    TakeGoals(botAI, bot, *goals, *result, now, workable);
                else
                {
                    goals->lastRejected = true;
                    goals->lastFailed = false;
                }
            }
        }
        else if (getMSTimeDiff(state.lastSnapshotMs, now) > RESULT_KEEP_MS)
        {
            state.pending = false;  // the bot keeps today's pick and asks again when due
            if (goals)
            {
                goals->jobOut = false;
                goals->lastFailed = true;  // a lost job (dropped at the queue cap or at Stop)
                goals->lastRejected = false;
            }
            // An expired result is counted by the pool already: lost counts only jobs that left none
            if (!Pool().ForgetExpired(bot->GetGUID().GetRawValue()))
                pendingLost.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (state.pending)
        return;

    uint32 const sinceSnapshot = getMSTimeDiff(state.lastSnapshotMs, now);
    bool const first = !state.lastSnapshotMs;
    bool const due =
        first || state.snapshotDue || sinceSnapshot >= config.PlannerSnapshotMinutes * MINUTE * IN_MILLISECONDS;
    if (IndexesReady() && QuestsReady())
    {
        if (!due || (!first && sinceSnapshot < GoalApply::MIN_SNAPSHOT_MS))
            return;
        if (TestProbe::On(botAI->testConstraints))  // a test bot's trace (WT63c): why a job goes out
            LOG_INFO("playerbots", "[Probe] {} snapshot submitted flag {} since {} key {:x}", bot->GetName(),
                     state.snapshotDue, sinceSnapshot / IN_MILLISECONDS, state.stateKey);
        SubmitGoals(botAI, bot, state, now);
        return;
    }
}

IdleStep OnIdle(PlayerbotAI* botAI, Player* bot, std::function<bool(uint32)> const& workable)
{
    Planner::BotState& state = botAI->planner;
    if (!(state.stateKey & STATE_SCOPE_BIT))
        return {};
    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    uint32 const now = getMSTime();
    GoalApply::BotGoals& goals = GoalsOf(botAI, bot);
    QuestTests const tests = TestsOf(bot, workable);

    // A plan the bridge paused: the bot stands idle until resumed (GoalApply::StillPaused)
    if (GoalApply::StillPaused(goals, now))
        return {false, true, nullptr};

    // The current goal goes on: the circuit's next task or turn-in, the route's next order (questing WQ18b: StartRoute
    // gives the order again when its executor did not end it), or the goal an interlude interrupted. A goal whose
    // executor ended ends its run (GoalApply::EndRun): its key stalls for the block time (BlockMs) when the log is as
    // it was at its start, unless its status was ended from outside (GoalApply::NoteStatusEnd); its executor starting
    // again clears that mark
    if (goals.slot.Query().Is())
    {
        GoalApply::Active const current = goals.slot.Query();
        bool const goesOn =
            goals.run.running && ((GoalApply::RunsCircuit(current) && StartQuest(botAI, bot, goals, tests)) ||
                                  (GoalApply::RunsRoute(current) && StartRoute(botAI, bot, goals)));
        if (goesOn)
        {
            GoalApply::ContinueRun(goals);
            goalsContinued.fetch_add(1, std::memory_order_relaxed);
            Publish(bot, goals);
            return {true, false, nullptr};
        }
        if (goals.run.running && !Goals::IsInterlude(current.kind))
            GoalApply::EndRun(goals, LogNow(bot), XpNow(bot), now, BlockMs(), GainNow(bot));
        NoteStalled(bot, goals);
        if (goals.slot.End(now))
        {
            GoalApply::Active const resumed = goals.slot.Query();
            if (TestProbe::On(botAI->testConstraints))  // a test bot's trace (WT63e): the clock without the interlude
                LOG_INFO("playerbots", "[Probe] {} resumes kind {}: its order counts {:.1f} of {:.1f} min{}",
                         bot->GetName(), static_cast<int>(resumed.kind), goals.slot.MinutesSince(now), resumed.minutes,
                         goals.slot.Bounded() ? " (interludes over the bound)" : "");
            GoalApply::ResumeQuest(goals.quest);
            GoalApply::Executor const executor = GoalApply::ExecutorFor(resumed, ReachOf(bot, resumed, tests));
            if (StartGoal(botAI, bot, goals, resumed, executor, tests,
                          goals.slot.Bounded() ? RESUME_BOUND_WORD : RESUME_WORD))
            {
                GoalApply::ContinueRun(goals);
                goalsResumed.fetch_add(1, std::memory_order_relaxed);
                Publish(bot, goals);
                return {true, false, nullptr};
            }
            NoteResumeNotStarted(bot, goals, resumed, executor);
            if (goals.run.running)
                GoalApply::EndRun(goals, LogNow(bot), XpNow(bot), now, BlockMs(), GainNow(bot));
            NoteStalled(bot, goals);
            goals.slot.End(now);
        }
    }

    // A goal's or a pause's end above shows now: a bot that waits below publishes nothing until its job's answer
    if (GoalApply::ViewStale(goals))
        Publish(bot, goals);

    GoalApply::IdleInput in;
    in.ready = IndexesReady() && QuestsReady();
    in.freshPlan = goals.plan && !goals.planUsed &&
                   getMSTimeDiff(goals.planMs, now) <= config.PlannerStaleSeconds * IN_MILLISECONDS;
    in.jobComing = state.pending || state.snapshotDue;
    if (!goals.idleSinceMs)
        goals.idleSinceMs = std::max<uint32>(now, 1);
    in.idleForMs = getMSTimeDiff(goals.idleSinceMs, now);
    in.lastRejected = goals.lastRejected;
    in.lastFailed = goals.lastFailed;
    GoalApply::IdleDecision const decision = GoalApply::DecideIdle(in);
    if (decision.action == GoalApply::IdleAction::Wait)
    {
        if (!goals.waitCounted)
            planWaits.fetch_add(1, std::memory_order_relaxed);
        goals.waitCounted = true;
        return {false, true, nullptr};
    }
    goals.idleSinceMs = 0;
    goals.waitCounted = false;

    GoalApply::Fallback reason = decision.reason;
    FirstRefusal first;
    if (decision.action == GoalApply::IdleAction::Apply)
    {
        if (ApplyPlan(botAI, bot, goals, now, tests, WorldLog::STATUS_REASON_OTHER, &first))
        {
            goalsApplied.fetch_add(1, std::memory_order_relaxed);
            Publish(bot, goals);
            return {true, false, nullptr};
        }
        reason = goals.plan && !goals.plan->ranked.ok ? GoalApply::Fallback::Failed
                                                      : GoalApply::NotStartedReason(first.options, first.stalled);
    }
    // The no-option loop's bound (WN4): from its NO_OPTION_BOUND-th answer in a row the fallback leaves out rest
    reason = GoalApply::BoundNoOption(goals, reason);
    bool const noted = reason == GoalApply::Fallback::CannotStart || reason == GoalApply::Fallback::Stalled;
    uint32 const note = noted ? static_cast<uint32>(first.kind) : 0;
    if (TestProbe::On(botAI->testConstraints))  // a test bot's trace (WT63e): why the plan started nothing
        LOG_INFO("playerbots", "[Probe] {} fallback {} options {} stalled {} first {}:{}", bot->GetName(),
                 GoalApply::FallbackWord(reason), first.options, first.stalled, static_cast<int>(first.kind),
                 static_cast<int>(first.result));
    fallbacks[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    GoalApply::NoteFallback(goals, reason, now);
    Publish(bot, goals);
    return {false, false, GoalApply::FallbackWord(reason), true, note, reason == GoalApply::Fallback::NoOptionStuck};
}

std::vector<NewRpgStatus> IdleStatuses(IdleStep const& step)
{
    std::vector<NewRpgStatus> statuses;
    for (uint32 const status :
         GoalApply::IdleList(step.fallback, step.moving, sPlayerbotAIConfig.Lone.TestQuestingOnly))
        statuses.push_back(static_cast<NewRpgStatus>(status));
    return statuses;
}

int PlannedPoi(PlayerbotAI* botAI, uint32 quest, std::vector<std::pair<float, float>> const& points)
{
    GoalApply::BotGoals const* goals = botAI->planner.goals.get();
    if (!goals || !goals->plan)
        return -1;
    return GoalApply::NearestToTasks(goals->plan->circuit, quest, points);
}

bool HasExternalGoal(uint64 bot)
{
    std::lock_guard<std::mutex> lock(viewsMutex);
    auto const view = views.find(bot);
    return view != views.end() &&
           (view->second.paused || (view->second.goal.Is() && GoalApply::IsExternal(view->second.goal.source)));
}

GoalApply::BridgeAnswer BridgeGoal(PlayerbotAI* botAI, Player* bot, GoalApply::BridgeOrder order,
                                   std::string const& place, uint32 quest)
{
    using GoalApply::BridgeAnswer;
    using GoalApply::BridgeOrder;
    Planner::BotState& state = botAI->planner;
    GoalApply::BridgeFacts facts;
    // The planner's scope as OnIdle sees it: only there does the goal go on and end
    facts.inScope = (state.stateKey & STATE_SCOPE_BIT) && InScopeNow(botAI);
    if (facts.inScope && state.goals)
        facts.current = state.goals->slot.Query();
    bool const trip = order == BridgeOrder::GoTo || order == BridgeOrder::MeetAt;
    std::optional<TravelRoute::Point> const at = trip ? TravelPlaces::Resolve(place) : std::nullopt;
    facts.placeKnown = at.has_value();
    facts.there = at && at->mapId == bot->GetMapId() && bot->GetDistance2d(at->x, at->y) <= GoalApply::GIVER_YARDS;
    facts.openWorld = !bot->GetMap()->Instanceable();
    QuestStatus const status = quest ? bot->GetQuestStatus(quest) : QUEST_STATUS_NONE;
    facts.questInLog = status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE;
    facts.canGrind = !bot->HasPlayerFlag(PLAYER_FLAGS_RESTING);
    facts.canGather = botAI->HasSkill(SKILL_HERBALISM) || botAI->HasSkill(SKILL_MINING);
    // One search on this thread, as the route topic runs it; its PerfMonitor row is "bridge route order"
    facts.route = [bot, &at]
    {
        RowTimer const timer("bridge route order");
        TravelRoute::Request const request(TravelRoute::FillSnapshot(bot, 0), *at);
        std::shared_ptr<TravelRoute::StuckEdges const> const stuck = TravelRoute::StuckBook::Instance().Current();
        // Without flight maps a flyer's fly legs are priced, not flown: no flight search on a map thread (contract C3)
        TravelRoute::Params params = TravelRoute::ConfiguredParams();
        params.flightMaps = nullptr;
        return TravelRoute::Search(TravelGraph::Instance(), request, *stuck, params).ok;
    };
    BridgeAnswer const answer = GoalApply::BridgeCheck(order, facts);
    if (answer != BridgeAnswer::Done)
        return answer;

    uint32 const now = getMSTime();
    GoalApply::BotGoals& goals = GoalsOf(botAI, bot);
    if (order == BridgeOrder::Pause || order == BridgeOrder::Resume)
    {
        // An ordered end: a trip writes its abort rows (abort:order, the status core's trip arm)
        if (GoalApply::BridgePause(goals, order == BridgeOrder::Pause, now))
            RpgStatus::End(botAI, StatusEnd::Outcome::Ordered);
        if (order == BridgeOrder::Resume)
            state.snapshotDue = true;
        Publish(bot, goals);
        return BridgeAnswer::Done;
    }

    Place const here = PlaceOf(bot);
    GoalApply::Active goal =
        GoalApply::BridgeGoalFor(order, here, at ? Place{at->mapId, at->x, at->y, at->z} : here, quest, now);
    // The bridge's order is the partner's wish: no stall check (GoalApply::MayStart), but its run is judged at its end.
    // Its errands and its set-hearth go through their entries as an order, an interlude that keeps the interrupted
    // goal's run; one the entry refuses finds nothing to do here
    QuestTests const tests{[](uint32) { return false; }, [](uint32) { return false; }};
    GoalApply::Reach reach;
    reach.sameMap = true;
    reach.deliverable = status == QUEST_STATUS_COMPLETE;
    reach.workable = status == QUEST_STATUS_INCOMPLETE;
    GoalApply::Executor const executor = GoalApply::ExecutorFor(goal, reach);
    char const* const word = order == BridgeOrder::Errands     ? "errand"
                             : order == BridgeOrder::SetHearth ? "inn"
                                                               : WorldLog::STATUS_REASON_OTHER;
    GoalApply::StartResult const result =
        GoalApply::StartActive(goals, goal.kind, executor,
                               {GoalApply::GoalKey(goal, executor, 0), LogNow(bot), XpNow(bot), false, now, BlockMs()},
                               [&] { return StartGoal(botAI, bot, goals, goal, executor, tests, word); });
    NoteStalled(bot, goals);
    if (result != GoalApply::StartResult::Started)
        return GoalApply::BridgeRefused(order);
    GoalApply::TakeBridgeGoal(goals, std::move(goal));
    Publish(bot, goals);
    return BridgeAnswer::Done;
}

bool PublishedPlan(uint64 bot, GoalApply::PlanView& out)
{
    std::lock_guard<std::mutex> lock(viewsMutex);
    auto const view = views.find(bot);
    if (view == views.end())
        return false;
    out.goal = view->second.goal;
    out.plan = view->second.plan;
    out.planMs = view->second.planMs;
    out.jobOut = view->second.jobOut;
    out.lastFallback = view->second.lastFallback;
    out.paused = view->second.paused;
    return true;
}

bool HandleGoalCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string word;
    in >> word;
    std::string name = word;
    Player* player = !name.empty() && normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    if (word.empty())
    {
        handler->SendErrorMessage("Usage: playerbots planner goal <bot name>");
        return false;
    }
    if (!player || !player->IsInWorld())
    {
        handler->SendErrorMessage("PLANNER goal: no player {} online", word);
        return false;
    }
    GoalView view;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(viewsMutex);
        auto const it = views.find(player->GetGUID().GetRawValue());
        if (it != views.end())
        {
            view = it->second;
            found = true;
        }
    }
    if (!found)
    {
        handler->PSendSysMessage("GOAL {}: not seen by the planner (no persistent bot in scope yet)",
                                 player->GetName());
        return true;
    }
    uint32 const now = getMSTime();
    auto const seconds = [now](uint32 ms) { return static_cast<float>(getMSTimeDiff(ms, now)) / IN_MILLISECONDS; };
    GoalApply::Active const& goal = view.goal;
    if (goal.Is())
        handler->PSendSysMessage(
            "GOAL {}: goal {} from {} ({}), {:.1f} min in, {:.1f}/min over {:.1f} min, "
            "{} quests started by the goal",
            player->GetName(), ChoiceOf(goal), GoalApply::SourceWord(goal.source), goal.reason,
            seconds(goal.startedMs) / MINUTE, goal.valuePerMinute, goal.minutes, view.questsStarted);
    else
        handler->PSendSysMessage("GOAL {}: no goal", player->GetName());
    if (view.planMs)
        handler->PSendSysMessage("  plan {:.0f} s old, {} options{}{}{}", seconds(view.planMs), view.options.size(),
                                 view.failure.empty() ? "" : ", no ranking: ", view.failure,
                                 view.jobOut ? "; a job is out" : "");
    else
        handler->PSendSysMessage("  no plan yet{}", view.jobOut ? "; a job is out" : "");
    if (view.plan && !view.plan->ranked.shortText.empty())
        handler->PSendSysMessage("  {}", view.plan->ranked.shortText);
    for (OptionLine const& option : view.options)
        handler->PSendSysMessage("  {} {:.1f}/min over {:.1f} min: {}", option.choice, option.valuePerMinute,
                                 option.minutes, option.reason);
    if (view.lastFallbackMs)
        handler->PSendSysMessage("  last fallback {} {:.0f} s ago", GoalApply::FallbackWord(view.lastFallback),
                                 seconds(view.lastFallbackMs));
    return true;
}

std::vector<std::string> PlannerStatus()
{
    PoolStats const stats = Pool().Stats();
    double const busy = stats.threads && stats.uptimeNs ? 100.0 * static_cast<double>(stats.busyNs) /
                                                              (static_cast<double>(stats.threads) * stats.uptimeNs)
                                                        : 0.0;
    auto const us = [](std::uint64_t ns) { return static_cast<double>(ns) / NANOS_PER_MICRO; };
    auto const verdict = [&stats](Verdict v) { return stats.verdicts[static_cast<std::size_t>(v)]; };
    std::vector<std::string> lines = {
        Acore::StringFormat("PLANNER pool: {} worker(s), busy {:.4f}%, jobs {} submitted, {} replaced, {} dropped, {} "
                            "run, {} failed, {} waiting (peak {}), compute p50/p95 {:.2f}/{:.2f} us, mean {:.2f} us, "
                            "queue wait p50/p95 {:.2f}/{:.2f} us, tasks {} posted, {} run",
                            stats.threads, busy, stats.submitted, stats.replaced, stats.dropped, stats.run,
                            stats.failures, stats.waiting, stats.peakWaiting, us(stats.computeP50Ns),
                            us(stats.computeP95Ns), stats.run ? us(stats.computeNs) / stats.run : 0.0,
                            us(stats.waitP50Ns), us(stats.waitP95Ns), stats.posted, stats.tasksRun),
        Acore::StringFormat("PLANNER results: {} made, {} taken ({} used; rejected {} dead, {} version, {} age, {} "
                            "position), {} expired, {} superseded, {} waiting; {} snapshots built, {} lost (no "
                            "result: dropped or failed)",
                            stats.made, stats.taken, verdict(Verdict::Use), verdict(Verdict::Dead),
                            verdict(Verdict::Version), verdict(Verdict::Age), verdict(Verdict::Position), stats.expired,
                            stats.superseded, stats.results, snapshotsBuilt.load(std::memory_order_relaxed),
                            pendingLost.load(std::memory_order_relaxed)),
    };
    std::size_t plannerBots = 0;
    {
        std::lock_guard<std::mutex> lock(viewsMutex);
        plannerBots = views.size();
    }
    auto const fallback = [](GoalApply::Fallback reason)
    { return fallbacks[static_cast<std::size_t>(reason)].load(std::memory_order_relaxed); };
    auto const count = [](std::atomic<uint64> const& counter) { return counter.load(std::memory_order_relaxed); };
    lines.push_back(Acore::StringFormat(
        "PLANNER goals: bots {} planner; goals {} applied, {} continued, {} resumed, {} switched; {} "
        "waits; fallbacks not_ready {}, failed {}, late {}, rejected {}, no_option {}, cannot_start {}, stalled {}, "
        "no_option_stuck {}; {} out of scope, {} stalled goals skipped, {} switches started nothing, {} interludes "
        "passed over for their "
        "retry",
        plannerBots, count(goalsApplied), count(goalsContinued), count(goalsResumed), count(goalSwitches),
        count(planWaits), fallback(GoalApply::Fallback::NotReady), fallback(GoalApply::Fallback::Failed),
        fallback(GoalApply::Fallback::Late), fallback(GoalApply::Fallback::Rejected),
        fallback(GoalApply::Fallback::NoOption), fallback(GoalApply::Fallback::CannotStart),
        fallback(GoalApply::Fallback::Stalled), fallback(GoalApply::Fallback::NoOptionStuck), count(scopeRefused),
        count(stalledSkips), count(switchNotStarted), count(retrySkips)));
    uint64 const routeJobs = count(routeCosts.jobs);
    uint64 const routeSearches = count(routeCosts.searches);
    lines.push_back(
        Acore::StringFormat("PLANNER route cost: {} jobs, {} ways asked, {} searches ({:.1f} a job, cap {}), "
                            "{} without a route, {} capped",
                            routeJobs, count(routeCosts.asked), routeSearches,
                            routeJobs ? static_cast<double>(routeSearches) / routeJobs : 0.0, ROUTE_SEARCHES_PER_JOB,
                            count(routeCosts.noRoute), count(routeCosts.capped)));
    lines.push_back(
        Acore::StringFormat("PLANNER route goals: {} results of bots on a route, {} rejected for the "
                            "position (moved over the recheck yards)",
                            count(routeResults), count(routeRejectedMoved)));
    for (std::string& line : IndexStatus())
        lines.push_back(std::move(line));
    return lines;
}

bool HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    for (std::string const& line : PlannerStatus())
        handler->PSendSysMessage("{}", line);
    return true;
}

bool HandleSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    std::vector<std::string> failures;
    std::vector<std::string> notes;
    uint32 const checks = PoolSelftest(failures, notes, SELFTEST_BOTS_PER_THREAD);
    handler->PSendSysMessage("PLANNER selftest: {}, {} checks, {} failed", failures.empty() ? "PASS" : "FAIL", checks,
                             failures.size());
    for (std::string const& note : notes)
        handler->PSendSysMessage("  {}", note);
    for (std::size_t i = 0; i < failures.size() && i < SELFTEST_FAILURES_SHOWN; ++i)
        handler->PSendSysMessage("FAIL: {}", failures[i]);
    return true;
}
}  // namespace Planner
