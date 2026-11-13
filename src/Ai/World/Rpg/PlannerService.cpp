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
#include <functional>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "Bag.h"
#include "BankMailState.h"
#include "BotTraitMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "GoalInterface.h"
#include "GoalModelLog.h"
#include "Group.h"
#include "Log.h"
#include "NewRpgAction.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "QuestCircuitLog.h"
#include "RouteService.h"
#include "SpellAuras.h"
#include "StartingHistoryRules.h"
#include "StringFormat.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "TravelGraph.h"
#include "TravelPlaces.h"
#include "UpkeepNeedRules.h"
#include "World.h"
#include "WorldLog.h"

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
constexpr uint32 STATE_SCOPE_BIT = 1u << 31;  // in PlannerState::stateKey: the bot keeps its possessions
constexpr double NANOS_PER_MICRO = 1000.0;
static_assert(SNAPSHOT_SKILLS ==
                  std::array<std::uint16_t, 15>{SKILL_RIDING, SKILL_FIRST_AID, SKILL_COOKING, SKILL_FISHING,
                                                SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING,
                                                SKILL_HERBALISM, SKILL_INSCRIPTION, SKILL_JEWELCRAFTING,
                                                SKILL_LEATHERWORKING, SKILL_MINING, SKILL_SKINNING, SKILL_TAILORING},
              "PlannerPool.h's snapshot skills are the core's SkillType ids");

static_assert(TAXI_WORDS == TaxiMaskSize, "the snapshot's taxi mask has PlayerTaxi's layout");
static_assert(QUEST_LOG_SLOTS == MAX_QUEST_LOG_SIZE, "the snapshot holds the whole quest log");
static_assert(QUEST_COUNTERS == QUEST_OBJECTIVES_COUNT, "one counter per quest objective");

std::atomic<uint64> snapshotsBuilt{0};
std::atomic<uint64> pendingLost{0};  // jobs out longer than RESULT_KEEP_MS without a result: dropped or failed

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

void FillBags(Player* bot, Snapshot& snapshot)
{
    uint32 slots = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = bot->GetBagByPos(bagSlot);
        if (bag && bag->GetTemplate()->Class == ITEM_CLASS_CONTAINER &&
            bag->GetTemplate()->SubClass == ITEM_SUBCLASS_CONTAINER)
            slots += bag->GetBagSize();
    }
    snapshot.bagSlots = static_cast<uint16>(slots);
    uint32 free = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++free;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = bot->GetBagByPos(bagSlot);
        if (!bag || bag->GetTemplate()->Class != ITEM_CLASS_CONTAINER ||
            bag->GetTemplate()->SubClass != ITEM_SUBCLASS_CONTAINER)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            if (!bag->GetItemByPos(static_cast<uint8>(slot)))
                ++free;
    }
    snapshot.bagFree = static_cast<uint16>(free);

    GearWear const wear = ReadGearWear(bot);
    snapshot.durabilityPercent = wear.durabilityPercent;
    snapshot.lowestDurabilityPercent = wear.lowestPercent;
    snapshot.brokenItems = wear.broken;
    snapshot.repairCost = PriceRepair(bot).total;
}

void FillSlots(Player* bot, std::vector<QuestSlot>& quests)
{
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
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

void FillQuests(Player* bot, Snapshot& snapshot)
{
    FillSlots(bot, snapshot.quests);
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
    BankMail::BotBankMail& known = botAI->rpgInfo.bankMail;
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

// FillSnapshot with its PerfMonitor row (map-thread microseconds per snapshot)
Snapshot TimedSnapshot(Player* bot, uint32 stateVersion)
{
    auto const started = std::chrono::steady_clock::now();
    Snapshot snapshot = FillSnapshot(bot, stateVersion);
    auto const us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "planner snapshot"))
        row->Record(static_cast<uint64>(std::max<long long>(us, 0)));
    snapshotsBuilt.fetch_add(1, std::memory_order_relaxed);
    return snapshot;
}

std::atomic<uint64> goalsApplied{0};    // a new goal started from a plan
std::atomic<uint64> goalsContinued{0};  // the circuit's next task started from the same plan
std::atomic<uint64> goalsResumed{0};    // an interlude ended and the goal it interrupted went on
std::atomic<uint64> goalSwitches{0};    // a replan left the current goal
std::atomic<uint64> planWaits{0};       // idle stretches that waited for a coming plan
std::atomic<uint64> strayResults{0};    // results of another job than the one waited for, not applied
std::atomic<uint64> scopeRefused{0};    // a bot found out of scope right before its job
std::atomic<uint64> stalledSkips{0};    // an option not started: its key stalled (GoalApply::MayStart)
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
    bool weighted{false};
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

bool Weighted(Player* bot) { return GoalApply::InWeightedGroup(bot->GetGUID().GetCounter()); }

// The scope right now: no real master, the bot keeps its possessions (rare: once per job and per applied answer)
bool InScopeNow(PlayerbotAI* botAI, Player* bot)
{
    return !botAI->HasGameClientMaster() && PlayerbotFactory::KeepsPossessions(bot);
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
    view.weighted = Weighted(bot);
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
    view.jobOut = goals.ticket != 0;
    view.questsStarted = goals.quest.started.size();
    view.lastFallback = goals.lastFallback;
    view.lastFallbackMs = goals.lastFallbackMs;
    view.paused = goals.paused;
    std::lock_guard<std::mutex> lock(viewsMutex);
    views[bot->GetGUID().GetRawValue()] = std::move(view);
}

GoalApply::BotGoals& GoalsOf(PlayerbotAI* botAI, Player* bot)
{
    std::shared_ptr<GoalApply::BotGoals>& goals = botAI->rpgInfo.planner.goals;
    if (!goals)
    {
        goals = std::make_shared<GoalApply::BotGoals>();
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

// The quest log and level now (GoalApply::LogSignature): at most 25 slots and their item counts, read once per plan
// applied and when a goal ends
uint64 LogNow(Player* bot)
{
    std::vector<QuestSlot> quests;
    quests.reserve(MAX_QUEST_LOG_SIZE);
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

// The level and XP as one number that grows with every XP gained (GoalApply::EndRun's grind and gather progress)
uint64 XpNow(Player* bot) { return (static_cast<uint64>(bot->GetLevel()) << 32) | bot->GetUInt32Value(PLAYER_XP); }

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

// The local circuit (GoalApply::AdvanceQuest): its next task or a completed quest's turn-in to DO_QUEST; without
// one, once per goal, its givers (walking to its start when far, else looking for them around). False when the
// circuit has nothing left
bool StartQuest(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, QuestTests const& tests)
{
    if (!goals.plan)
        return false;
    CircuitPlan const& circuit = goals.plan->circuit;
    for (;;)  // ends: each quest starts at most once per goal
    {
        GoalApply::QuestNext const next = GoalApply::AdvanceQuest(goals.quest, circuit, tests.work, tests.turnIn);
        if (next.step == GoalApply::QuestStep::Done)
            return false;
        if (next.step == GoalApply::QuestStep::Givers)
            break;
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(next.quest))
        {
            botAI->rpgInfo.ChangeToDoQuest(next.quest, quest);
            return true;
        }
    }
    if (Placed(bot, circuit.start, true) &&
        bot->GetDistance2d(circuit.start.x, circuit.start.y) > GoalApply::GIVER_YARDS)
        botAI->rpgInfo.ChangeToGoCamp(PositionOf(circuit.start));
    else
        botAI->rpgInfo.ChangeToWanderNpc();
    return true;
}

// Today's statuses are the executors (GoalApply::ExecutorFor). False when the goal cannot start here
bool StartGoal(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, GoalApply::Active const& goal,
               GoalApply::Executor executor, QuestTests const& tests)
{
    NewRpgInfo& info = botAI->rpgInfo;
    switch (executor)
    {
        case GoalApply::Executor::Circuit:
            return StartQuest(botAI, bot, goals, tests);
        case GoalApply::Executor::Deliver:
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(goal.target))
            {
                info.ChangeToDoQuest(goal.target, quest);
                return true;
            }
            return false;
        case GoalApply::Executor::Grind:
            if (!Placed(bot, goal.place, true))
                return false;
            info.ChangeToGoGrind(PositionOf(goal.place));
            return true;
        case GoalApply::Executor::Gather:
            info.ChangeToDoGather();
            return true;
        case GoalApply::Executor::Travel:
            // ApplyTravelOrder's refusals: the route follower starts alive, on the ground and in the open world
            if (!Placed(bot, goal.place, false) || !bot->IsAlive() || bot->IsInFlight() ||
                bot->GetMap()->Instanceable())
                return false;
            info.ChangeToTravel(PositionOf(goal.place));
            return true;
        case GoalApply::Executor::Rest:
            info.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
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

void LogCircuit(Player* bot, GoalApply::BotGoals& goals)
{
    if (goals.circuitLogged || !goals.plan || !WorldLog::Active())
        return;
    goals.circuitLogged = true;
    for (CircuitLog::Row const& row : CircuitLog::Rows(goals.plan->circuit))
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Circuit, row.choice, row.reason, row.value);
}

// One option started as the bot's goal; its row shows the ranking with it first. A goal (no interlude) starts its
// run, with its circuit progress anew, and does not start while its key stalled (GoalApply's stall rule; log: the
// signature at this plan). An interlude keeps the run and progress of the goal it interrupts
bool StartOption(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, Goals::Option const& option,
                 std::size_t index, uint32 now, uint64 log, QuestTests const& tests)
{
    GoalApply::Source const source =
        option.pinned && goals.external ? goals.externalSource : GoalApply::Source::Planner;
    GoalApply::Active goal = GoalApply::FromOption(option, source, now);
    if (!GoalApply::MayReplace(goals.slot.Query(), source))
        return false;
    GoalApply::Executor const executor = GoalApply::ExecutorFor(goal, ReachOf(bot, goal, tests));
    bool const circuit = executor == GoalApply::Executor::Circuit;
    if (executor == GoalApply::Executor::None || (circuit && !goals.plan))
        return false;
    std::optional<std::pair<GoalApply::Run, GoalApply::QuestGoal>> before;
    if (!Goals::IsInterlude(goal.kind))
    {
        uint64 const key = GoalApply::GoalKey(goal, executor, goals.plan ? GoalApply::CircuitKey(*goals.plan) : 0);
        if (!GoalApply::MayStart(goals, key, log))
        {
            stalledSkips.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        before.emplace(goals.run, std::move(goals.quest));
        GoalApply::BeginRun(goals, key, log, XpNow(bot), GoalApply::XpIsProgress(goal.kind));
    }
    if (!StartGoal(botAI, bot, goals, goal, executor, tests))
    {
        if (before)
        {
            goals.run = before->first;
            goals.quest = std::move(before->second);
        }
        return false;
    }
    goals.slot.Set(std::move(goal));
    if (goals.plan)
        LogGoal(bot, GoalApply::ChosenFirst(goals.plan->ranked, index));
    if (circuit)
        LogCircuit(bot, goals);
    return true;
}

// A replan's choice, else the plan's best option that can start; a plan starts at most one new goal
bool ApplyPlan(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, uint32 now, QuestTests const& tests)
{
    std::optional<Goals::Option> const switchTo = std::move(goals.switchTo);
    goals.switchTo.reset();
    if (!goals.plan)
        return false;
    goals.planUsed = true;
    Goals::Ranked const& ranked = goals.plan->ranked;
    uint64 const log = LogNow(bot);
    if (switchTo)
    {
        std::size_t index = 0;
        while (index < ranked.options.size() && !Goals::SameTarget(ranked.options[index], *switchTo))
            ++index;
        if (StartOption(botAI, bot, goals, *switchTo, index, now, log, tests))
            return true;
    }
    if (!ranked.ok)
        return false;
    for (std::size_t i = 0; i < ranked.options.size(); ++i)
        if (StartOption(botAI, bot, goals, ranked.options[i], i, now, log, tests))
            return true;
    return false;
}

bool Preemptible(NewRpgStatus status)
{
    return status == RPG_DO_QUEST || status == RPG_GO_GRIND || status == RPG_GO_CAMP || status == RPG_DO_GATHER ||
           status == RPG_REST || status == RPG_WANDER_NPC || status == RPG_WANDER_RANDOM;
}

// A fresh plan while the bot pursues a planner goal: ReplanDecision says stay, switch or pick up; a switch ends the
// current status so that the idle step in the same update starts the new goal
void ConsiderSwitch(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, uint32 now)
{
    GoalApply::Active const& current = goals.slot.Query();
    bool const situation = goals.situationChanged;
    goals.situationChanged = false;
    NewRpgStatus const status = botAI->rpgInfo.GetStatus();
    if (!current.Is() || GoalApply::IsExternal(current.source) || !Preemptible(status))
        return;
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
        return;
    goals.switchTo = replan.chosen;
    goalSwitches.fetch_add(1, std::memory_order_relaxed);
    botAI->rpgInfo.ChangeToIdle();
}

// A checked answer of the job the bot waited for: kept as its plan (in scope only)
void TakeGoals(PlayerbotAI* botAI, Player* bot, GoalApply::BotGoals& goals, Result const& result, uint32 now)
{
    GoalsAnswer const* answer = std::get_if<GoalsAnswer>(&result.payload);
    if (!answer || !answer->plan)
    {
        goals.lastFailed = true;  // a failed job answers without a plan (GoalsCompute)
        goals.lastRejected = false;
        Publish(bot, goals);
        return;
    }
    if (!InScopeNow(botAI, bot))
    {
        scopeRefused.fetch_add(1, std::memory_order_relaxed);
        goals.lastRejected = true;
        goals.lastFailed = false;
        return;
    }
    GoalApply::TakePlan(goals, answer->plan, result.takenMs);
    ConsiderSwitch(botAI, bot, goals, now);
    Publish(bot, goals);
}

Goals::Situation SituationOf(Player* bot, GoalApply::BotGoals const& goals)
{
    Goals::Situation situation;
    situation.here.hub = NO_HUB;  // the job finds it from its circuit (Goals::CircuitHub)
    situation.healthPercent = bot->GetHealthPct();
    uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
    situation.manaPercent = maxMana > 0 ? static_cast<float>(PERCENT) * static_cast<float>(bot->GetPower(POWER_MANA)) /
                                              static_cast<float>(maxMana)
                                        : -1.0f;
    situation.external = goals.external;
    return situation;
}

// The goals job: the snapshot with the current goal, the plan's kept quests and the log quests' ages
void SubmitGoals(PlayerbotAI* botAI, Player* bot, NewRpgInfo::PlannerState& state, uint32 now)
{
    GoalApply::BotGoals& goals = GoalsOf(botAI, bot);
    Snapshot snapshot = TimedSnapshot(bot, state.stateVersion);
    // A test keeping the hearth off: no hearth or inn goal either (GoalModel AddHearth)
    if (!TestConstraints::HearthAllowed(botAI->rpgInfo.constraints))
        snapshot.hasHearthstone = false;
    GoalApply::Active const& current = goals.slot.Query();
    snapshot.currentGoal = {static_cast<uint32>(current.kind), current.target, current.startedMs};
    std::vector<uint32> log;
    log.reserve(snapshot.quests.size());
    for (QuestSlot const& slot : snapshot.quests)
        log.push_back(slot.quest);
    GoalApply::UpdateSeen(goals.seen, std::move(log), now);
    // The needs rates read the bot's slot measure here; the job computes the needs list from them (upkeep WU16)
    Compute compute = GoalsCompute(goals.keep, GoalApply::ProgressOf(goals.seen, now), SituationOf(bot, goals),
                                   BotNeedRates(botAI, snapshot));
    Job job{JobKind::Goals, std::move(snapshot), std::move(compute)};
    state.lastSnapshotMs = std::max<uint32>(now, 1);
    state.snapshotDue = false;
    goals.ticket = Pool().Submit(bot->GetGUID().GetRawValue(), std::move(job));
    state.pending = goals.ticket != 0;
}
}  // namespace

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
    // Resurrection sickness: the planner ranks Rest first until it ends (upkeep WU16)
    if (Aura const* sickness = bot->GetAura(Upkeep::SPELL_RESURRECTION_SICKNESS))
        snapshot.sickMs = static_cast<std::uint32_t>(std::max<int32>(sickness->GetDuration(), 0));
    if (Group const* group = bot->GetGroup())
    {
        snapshot.groupSize = static_cast<uint8>(group->GetMembersCount());
        snapshot.groupLeader = group->GetLeaderGUID().GetRawValue();
    }
    FillUpkeep(bot, snapshot);
    return snapshot;
}

void PlannerHook(PlayerbotAI* botAI, Player* bot)
{
    RowTimer const timer("planner hook");
    // The scope (KeepsPossessions: lookups in the random bot lists) is checked again only when the state key
    // changes; a real master takes a bot through a group invite, which changes the key. Bit 31 keeps the verdict
    NewRpgInfo::PlannerState& state = botAI->rpgInfo.planner;
    uint32 const key = StateKey(bot);
    if (key != (state.stateKey & ~STATE_SCOPE_BIT))
    {
        state.stateKey = key | (PlayerbotFactory::KeepsPossessions(bot) ? STATE_SCOPE_BIT : 0);
        ++state.stateVersion;
        state.snapshotDue = true;
        if (state.goals)
            state.goals->situationChanged = true;
    }
    if (!(state.stateKey & STATE_SCOPE_BIT))
        return;
    // A test's hold on the planner (TestConstraints): no jobs, no goals taken
    if (!TestConstraints::GateOf(botAI->rpgInfo.constraints).planner)
        return;

    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    uint32 const now = getMSTime();
    bool const idle = botAI->rpgInfo.GetStatus() == RPG_IDLE;
    if (idle && !state.wasIdle)
    {
        state.snapshotDue = true;  // a goal ended
        // A wait that another action ended (an errand, an inn, a travel order) leaves no stale start behind
        if (state.goals)
        {
            state.goals->idleSinceMs = 0;
            state.goals->waitCounted = false;
        }
    }
    state.wasIdle = idle;

    if (state.pending)
    {
        GoalApply::BotGoals* const goals = state.goals.get();
        if (std::optional<Result> const result = Pool().TryTake(bot->GetGUID().GetRawValue()))
        {
            // A stray result (an older job's, after a reset or a lost wait) is not this job's answer: the bot keeps
            // waiting for its own
            if (goals && goals->ticket && !GoalApply::TicketMatches(result->ticket, goals->ticket))
                strayResults.fetch_add(1, std::memory_order_relaxed);
            else
            {
                state.pending = false;
                Verdict const verdict =
                    Recheck(*result, {bot->IsAlive(), state.stateVersion, PlaceOf(bot), now}, ConfiguredLimits());
                Pool().CountVerdict(verdict);
                if (goals && goals->ticket)
                {
                    goals->ticket = 0;
                    if (verdict == Verdict::Use)
                        TakeGoals(botAI, bot, *goals, *result, now);
                    else
                    {
                        goals->lastRejected = true;
                        goals->lastFailed = false;
                    }
                }
            }
        }
        else if (getMSTimeDiff(state.lastSnapshotMs, now) > RESULT_KEEP_MS)
        {
            state.pending = false;  // the bot keeps today's pick and asks again when due
            if (goals)
            {
                goals->ticket = 0;
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
    if (config.PlannerGoals && !Weighted(bot) && IndexesReady() && QuestsReady())
    {
        if (!due || (!first && sinceSnapshot < GoalApply::MIN_SNAPSHOT_MS))
            return;
        // The cached scope misses a real player's invite that changed nothing in the key: checked again here
        if (!InScopeNow(botAI, bot))
        {
            scopeRefused.fetch_add(1, std::memory_order_relaxed);
            state.stateKey &= ~STATE_SCOPE_BIT;
            return;
        }
        SubmitGoals(botAI, bot, state, now);
        return;
    }

    // The no-op job (a test switch) proves the machinery for bots without planner goals
    if (!config.PlannerNoopSeconds)
        return;
    if (!due || (!first && sinceSnapshot < config.PlannerNoopSeconds * IN_MILLISECONDS))
        return;

    Job job{JobKind::Noop, TimedSnapshot(bot, state.stateVersion), NoopCompute};
    state.lastSnapshotMs = std::max<uint32>(now, 1);
    state.snapshotDue = false;
    state.pending = Pool().Submit(bot->GetGUID().GetRawValue(), std::move(job)) != 0;
}

IdleStep OnIdle(PlayerbotAI* botAI, Player* bot, std::function<bool(uint32)> const& workable)
{
    NewRpgInfo::PlannerState& state = botAI->rpgInfo.planner;
    if (!(state.stateKey & STATE_SCOPE_BIT))
        return {};
    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    uint32 const now = getMSTime();
    bool const weighted = Weighted(bot);
    GoalApply::BotGoals& goals = GoalsOf(botAI, bot);
    QuestTests const tests{[bot, &workable](uint32 quest)
                           { return bot->GetQuestStatus(quest) == QUEST_STATUS_INCOMPLETE && workable(quest); },
                           [bot, &workable](uint32 quest)
                           { return bot->GetQuestStatus(quest) == QUEST_STATUS_COMPLETE && workable(quest); }};

    // A plan the bridge paused: the bot stands idle until resumed (GoalApply::StillPaused)
    if (GoalApply::StillPaused(goals, now))
        return {false, true, nullptr};

    // The current goal goes on: the circuit's next task or turn-in, or the goal an interlude interrupted. A goal whose
    // executor ended ends its run (GoalApply::EndRun): its key stalls when the log is as it was at its start. An
    // external goal goes on in the weighted group too: it has no planner goals, but the bridge may set one
    if ((!weighted || goals.slot.HasExternal()) && config.PlannerGoals && goals.slot.Query().Is() && !goals.switchTo)
    {
        GoalApply::Active const current = goals.slot.Query();
        if (GoalApply::RunsCircuit(current) && goals.run.running && StartQuest(botAI, bot, goals, tests))
        {
            goalsContinued.fetch_add(1, std::memory_order_relaxed);
            Publish(bot, goals);
            return {true, false, nullptr};
        }
        if (goals.run.running && !Goals::IsInterlude(current.kind))
            GoalApply::EndRun(goals, LogNow(bot), XpNow(bot));
        if (goals.slot.End())
        {
            GoalApply::Active const resumed = goals.slot.Query();
            GoalApply::Executor const executor = GoalApply::ExecutorFor(resumed, ReachOf(bot, resumed, tests));
            if (StartGoal(botAI, bot, goals, resumed, executor, tests))
            {
                goalsResumed.fetch_add(1, std::memory_order_relaxed);
                Publish(bot, goals);
                return {true, false, nullptr};
            }
            if (goals.run.running)
                GoalApply::EndRun(goals, LogNow(bot), XpNow(bot));
            goals.slot.End();
        }
    }

    // A goal's or a pause's end above shows now: a weighted bot answers Weights below and has no job to publish it
    if (GoalApply::ViewStale(goals))
        Publish(bot, goals);

    GoalApply::IdleInput in;
    in.weighted = weighted;
    in.switchOn = config.PlannerGoals;
    in.ready = IndexesReady() && QuestsReady();
    in.freshPlan = goals.switchTo || (goals.plan && !goals.planUsed &&
                                      getMSTimeDiff(goals.planMs, now) <= config.PlannerStaleSeconds * IN_MILLISECONDS);
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
    if (decision.action == GoalApply::IdleAction::Weights)
        return {};

    GoalApply::Fallback reason = decision.reason;
    if (decision.action == GoalApply::IdleAction::Apply)
    {
        if (ApplyPlan(botAI, bot, goals, now, tests))
        {
            goalsApplied.fetch_add(1, std::memory_order_relaxed);
            Publish(bot, goals);
            return {true, false, nullptr};
        }
        reason = goals.plan && !goals.plan->ranked.ok ? GoalApply::Fallback::Failed : GoalApply::Fallback::NoPlan;
    }
    fallbacks[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    goals.lastFallback = reason;
    goals.lastFallbackMs = now;
    Publish(bot, goals);
    return {false, false, GoalApply::FallbackWord(reason)};
}

bool KeepsQuest(PlayerbotAI* botAI, uint32 quest)
{
    GoalApply::BotGoals const* goals = botAI->rpgInfo.planner.goals.get();
    return sPlayerbotAIConfig.WorldTravel.PlannerGoals && goals && goals->plan && GoalApply::Keeps(goals->keep, quest);
}

int PlannedPoi(PlayerbotAI* botAI, uint32 quest, std::vector<std::pair<float, float>> const& points)
{
    GoalApply::BotGoals const* goals = botAI->rpgInfo.planner.goals.get();
    if (!sPlayerbotAIConfig.WorldTravel.PlannerGoals || !goals || !goals->plan)
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
    NewRpgInfo::PlannerState& state = botAI->rpgInfo.planner;
    GoalApply::BridgeFacts facts;
    // The planner's scope as OnIdle sees it: only there does the goal go on and end
    facts.inScope =
        sPlayerbotAIConfig.WorldTravel.PlannerGoals && (state.stateKey & STATE_SCOPE_BIT) && InScopeNow(botAI, bot);
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
        // The ground route answers "is there a route" (fly legs never change it): no flight search on a map thread
        return TravelRoute::GroundSearch(TravelGraph::Instance(), request, *stuck, TravelRoute::ConfiguredParams()).ok;
    };
    BridgeAnswer const answer = GoalApply::BridgeCheck(order, facts);
    if (answer != BridgeAnswer::Done)
        return answer;

    uint32 const now = getMSTime();
    GoalApply::BotGoals& goals = GoalsOf(botAI, bot);
    if (order == BridgeOrder::Pause || order == BridgeOrder::Resume)
    {
        if (GoalApply::BridgePause(goals, order == BridgeOrder::Pause, now))
            botAI->rpgInfo.ChangeToIdle();
        if (order == BridgeOrder::Resume)
            state.snapshotDue = true;
        Publish(bot, goals);
        return BridgeAnswer::Done;
    }

    Place const here = PlaceOf(bot);
    GoalApply::Active goal =
        GoalApply::BridgeGoalFor(order, here, at ? Place{at->mapId, at->x, at->y, at->z} : here, quest, now);
    // An errand trip and an inn trip are planned by their own start actions, as their triggers start them
    if (order == BridgeOrder::Errands || order == BridgeOrder::SetHearth)
    {
        bool const started = order == BridgeOrder::Errands ? NewRpgStartErrandsAction(botAI).Execute(Event())
                                                           : NewRpgStartInnAction(botAI).Execute(Event());
        if (!started)
            return BridgeAnswer::NothingHere;
        GoalApply::TakeBridgeGoal(goals, std::move(goal));
        Publish(bot, goals);
        return BridgeAnswer::Done;
    }

    // The bridge's order is the partner's wish: no stall check (GoalApply::MayStart), but its run is judged at its end
    QuestTests const tests{[](uint32) { return false; }, [](uint32) { return false; }};
    GoalApply::Reach reach;
    reach.sameMap = true;
    reach.deliverable = status == QUEST_STATUS_COMPLETE;
    reach.workable = status == QUEST_STATUS_INCOMPLETE;
    GoalApply::Executor const executor = GoalApply::ExecutorFor(goal, reach);
    GoalApply::Run const before = goals.run;
    GoalApply::QuestGoal progress = std::move(goals.quest);
    GoalApply::BeginRun(goals, GoalApply::GoalKey(goal, executor, 0), LogNow(bot), XpNow(bot),
                        GoalApply::XpIsProgress(goal.kind));
    if (executor == GoalApply::Executor::None || !StartGoal(botAI, bot, goals, goal, executor, tests))
    {
        goals.run = before;
        goals.quest = std::move(progress);
        return BridgeAnswer::Failed;
    }
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
    char const* const group = Weighted(player) ? "weighted" : "planner";
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
        handler->PSendSysMessage("GOAL {}: group {}, not seen by the planner (no persistent bot in scope yet)",
                                 player->GetName(), group);
        return true;
    }
    uint32 const now = getMSTime();
    auto const seconds = [now](uint32 ms) { return static_cast<float>(getMSTimeDiff(ms, now)) / IN_MILLISECONDS; };
    GoalApply::Active const& goal = view.goal;
    if (goal.Is())
        handler->PSendSysMessage(
            "GOAL {}: group {}, goal {} from {} ({}), {:.1f} min in, {:.1f}/min over {:.1f} min, "
            "{} quests started by the goal",
            player->GetName(), group, ChoiceOf(goal), GoalApply::SourceWord(goal.source), goal.reason,
            seconds(goal.startedMs) / MINUTE, goal.valuePerMinute, goal.minutes, view.questsStarted);
    else
        handler->PSendSysMessage("GOAL {}: group {}, no goal", player->GetName(), group);
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
    std::size_t weightedBots = 0;
    {
        std::lock_guard<std::mutex> lock(viewsMutex);
        for (auto const& [guid, view] : views)
            ++(view.weighted ? weightedBots : plannerBots);
    }
    auto const fallback = [](GoalApply::Fallback reason)
    { return fallbacks[static_cast<std::size_t>(reason)].load(std::memory_order_relaxed); };
    auto const count = [](std::atomic<uint64> const& counter) { return counter.load(std::memory_order_relaxed); };
    lines.push_back(Acore::StringFormat(
        "PLANNER goals: switch {}, bots {} planner, {} weighted; goals {} applied, {} continued, {} resumed, {} "
        "switched; {} waits; fallbacks off {}, not_ready {}, failed {}, no_plan {}, late {}, rejected {}; {} stray "
        "results, {} out of scope, {} stalled goals skipped",
        sPlayerbotAIConfig.WorldTravel.PlannerGoals ? "on" : "off", plannerBots, weightedBots, count(goalsApplied),
        count(goalsContinued), count(goalsResumed), count(goalSwitches), count(planWaits),
        fallback(GoalApply::Fallback::Off), fallback(GoalApply::Fallback::NotReady),
        fallback(GoalApply::Fallback::Failed), fallback(GoalApply::Fallback::NoPlan),
        fallback(GoalApply::Fallback::Late), fallback(GoalApply::Fallback::Rejected), count(strayResults),
        count(scopeRefused), count(stalledSkips)));
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
