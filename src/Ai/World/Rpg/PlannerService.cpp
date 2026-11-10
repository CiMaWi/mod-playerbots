/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlannerService.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "Bag.h"
#include "BotTraitMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Group.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "StartingHistoryRules.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"

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

// The items DurabilityRepairAll repairs: equipped, the backpack, the bags themselves and the items in them
uint32 RepairCostAll(Player* bot)
{
    uint32 total = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        total += RepairCost(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag const* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                total += RepairCost(bag->GetItemByPos(static_cast<uint8>(slot)));
    return total;
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
    snapshot.durabilityPercent =
        static_cast<uint8>(maxDurability ? std::min<uint64>(durability * PERCENT / maxDurability, PERCENT) : PERCENT);
    snapshot.lowestDurabilityPercent = static_cast<uint8>(lowest);
    snapshot.brokenItems = static_cast<uint8>(broken);
    snapshot.repairCost = RepairCostAll(bot);
}

void FillQuests(Player* bot, Snapshot& snapshot)
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
        snapshot.quests.push_back(entry);
    }
    RewardedQuestSet const& rewarded = bot->getRewardedQuests();
    snapshot.rewardedQuests.assign(rewarded.begin(), rewarded.end());
    std::sort(snapshot.rewardedQuests.begin(), snapshot.rewardedQuests.end());
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
}  // namespace

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
    // The scope (KeepsPossessions: lookups in the random bot lists) is checked again only when the state key
    // changes; a real master takes a bot through a group invite, which changes the key. Bit 31 keeps the verdict
    NewRpgInfo::PlannerState& state = botAI->rpgInfo.planner;
    uint32 const key = StateKey(bot);
    if (key != (state.stateKey & ~STATE_SCOPE_BIT))
    {
        state.stateKey = key | (PlayerbotFactory::KeepsPossessions(bot) ? STATE_SCOPE_BIT : 0);
        ++state.stateVersion;
        state.snapshotDue = true;
    }
    if (!(state.stateKey & STATE_SCOPE_BIT))
        return;

    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    uint32 const now = getMSTime();
    bool const idle = botAI->rpgInfo.GetStatus() == RPG_IDLE;
    if (idle && !state.wasIdle)
        state.snapshotDue = true;  // a goal ended
    state.wasIdle = idle;

    if (state.pending)
    {
        if (std::optional<Result> const result = Pool().TryTake(bot->GetGUID().GetRawValue()))
        {
            state.pending = false;
            Verdict const verdict =
                Recheck(*result, {bot->IsAlive(), state.stateVersion, PlaceOf(bot), now}, ConfiguredLimits());
            Pool().CountVerdict(verdict);
            // A used no-op result changes nothing; WT16 applies the planner's answers
        }
        else if (getMSTimeDiff(state.lastSnapshotMs, now) > RESULT_KEEP_MS)
        {
            state.pending = false;  // the bot keeps today's pick and asks again when due
            // An expired result is counted by the pool already: lost counts only jobs that left none
            if (!Pool().ForgetExpired(bot->GetGUID().GetRawValue()))
                pendingLost.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // The no-op job is the only kind yet: without it nothing is built (WT16 submits the planning job when due)
    if (!config.PlannerNoopSeconds || state.pending)
        return;
    uint32 const sinceSnapshot = getMSTimeDiff(state.lastSnapshotMs, now);
    bool const first = !state.lastSnapshotMs;
    bool const due =
        first || state.snapshotDue || sinceSnapshot >= config.PlannerSnapshotMinutes * MINUTE * IN_MILLISECONDS;
    if (!due || (!first && sinceSnapshot < config.PlannerNoopSeconds * IN_MILLISECONDS))
        return;

    Job job{JobKind::Noop, TimedSnapshot(bot, state.stateVersion), NoopCompute};
    state.lastSnapshotMs = std::max<uint32>(now, 1);
    state.snapshotDue = false;
    state.pending = Pool().Submit(bot->GetGUID().GetRawValue(), std::move(job));
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
