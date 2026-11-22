/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The server side of the quest index (travel plan WT13a): the world-thread gather, the worker build, the published
// index and the GM query; and the population's quest statistics: the events, the load, the flush and the view.
// The questing circuit's job wrapper, its real-place harness and `planner circuit` (WT13b; the planner: QuestCircuit.h)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "BotKind.h"
#include "Chat.h"
#include "ClassQuestCompletion.h"
#include "ClassQuestJoin.h"
#include "ConditionMgr.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObjectData.h"
#include "GatherLive.h"
#include "GoalInterface.h"
#include "GoalModel.h"
#include "GoalModelHarness.h"
#include "GoalModelLog.h"
#include "Log.h"
#include "NeedGoalBridge.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerFishing.h"
#include "PlannerRouteCost.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "QuestCircuit.h"
#include "QuestCircuitHarness.h"
#include "QuestCircuitLog.h"
#include "QuestDef.h"
#include "QuestExecRules.h"
#include "QuestFailRecord.h"
#include "QuestIndex.h"
#include "QuestLevelRules.h"
#include "QuestPoiZStore.h"
#include "QuestStats.h"
#include "QuestUseRules.h"
#include "SkillPathBridge.h"
#include "SkillPathLive.h"
#include "SmartScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Upkeep.h"
#include "World.h"
#include "WorldLog.h"

namespace Planner
{
static_assert(QuestType::ELITE == QUEST_TYPE_ELITE && QuestType::PVP == QUEST_TYPE_PVP &&
                  QuestType::RAID == QUEST_TYPE_RAID && QuestType::DUNGEON == QUEST_TYPE_DUNGEON &&
                  QuestType::HEROIC == QUEST_TYPE_HEROIC && QuestType::RAID_10 == QUEST_TYPE_RAID_10 &&
                  QuestType::RAID_25 == QUEST_TYPE_RAID_25,
              "the planner's quest types are the core's");
static_assert(POI_FIRST_ITEM == QUEST_OBJECTIVES_COUNT, "item POIs follow the creature or object slots");
static_assert(Circuit::MAX_LEVELS_ABOVE == QUEST_MAX_LEVELS_ABOVE_BOT, "the circuit's level limit is today's");
static_assert(Circuit::SLOT_COMPLETE == static_cast<uint32>(QUEST_STATE_COMPLETE),
              "the circuit reads the log slot's complete flag");

namespace
{
enum class QuestIndexState : uint8
{
    NotStarted,
    Building,
    Ready,
    Failed
};

constexpr uint32 AREATRIGGER_ID_LIMIT = 0xFFFF;  // the area trigger ids looked up for exploration quests
constexpr std::size_t QUERY_PLACES = 4;          // givers and enders listed by `planner quest`
constexpr double BYTES_PER_MB = 1024.0 * 1024.0;

std::atomic<QuestIndexState> questState{QuestIndexState::NotStarted};
QuestBook questBook;

// The quest statistics (QuestStats.h): the book takes events from map threads; the rest is the world thread's
constexpr uint32 STATS_FLUSH_MS = 300 * IN_MILLISECONDS;  // the world thread writes the changed cells this often

std::atomic<bool> statsOn{false};  // set after the load; events before it are not counted
QuestStatsBook statsBook;
QuestStatsSlot statsSlot;
uint32 statsLoadedRows = 0;
uint32 statsBadRows = 0;  // rows whose minutes column did not parse: counts kept, samples dropped
uint32 statsFlushTimer = 0;
uint32 statsFlushes = 0;
uint32 statsLastRows = 0;
uint64 statsRowsWritten = 0;
std::optional<std::chrono::steady_clock::time_point> statsLastFlush;

uint32 MsSince(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

uint64 StatsNowMs()
{
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Map threads: the event with its scope check (persistent random bots, as the planner hook), timed as PerfMonitor
// row `planner quest event`
template <typename Event>
void TimedEvent(Player* bot, Event const& event)
{
    auto const started = std::chrono::steady_clock::now();
    if (BotKind::KeepsPossessions(bot))
        event(bot->GetGUID().GetCounter(), static_cast<uint32>(bot->GetLevel()));
    auto const us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "planner quest event"))
        row->Record(static_cast<uint64>(std::max<long long>(us, 0)));
}

bool StatsOn(Player* bot) { return bot && statsOn.load(std::memory_order_acquire); }

// World thread, at startup before any bot logs in (one query, as BotTraitMgr): the stored totals
void LoadQuestStats()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::LoadQuestStats");
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT quest_id, band, accepted, completed, abandoned, deaths, minutes FROM playerbots_quest_stats"))
    {
        do
        {
            Field* fields = result->Fetch();
            QuestStatCell cell;
            cell.accepted = fields[2].Get<uint32>();
            cell.completed = fields[3].Get<uint32>();
            cell.abandoned = fields[4].Get<uint32>();
            cell.deaths = fields[5].Get<uint32>();
            if (!DecodeMinutes(fields[6].Get<std::string>(), cell))
                ++statsBadRows;
            statsBook.Restore(fields[0].Get<uint32>(), fields[1].Get<uint8>(), cell);
            ++statsLoadedRows;
        } while (result->NextRow());
    }
    statsSlot.Publish(statsBook.View());
    statsOn.store(true, std::memory_order_release);
    LOG_INFO("playerbots", "Quest stats: loaded {} rows{}", statsLoadedRows,
             statsBadRows ? Acore::StringFormat(" ({} with a bad minutes column)", statsBadRows) : "");
}

std::string MinutesText(float minutes) { return minutes < 0.0f ? "-" : Acore::StringFormat("{:.1f}", minutes); }

bool HandleQuestStats(ChatHandler* handler)
{
    QuestBookCounts const c = statsBook.Counts();
    std::shared_ptr<QuestStatsView const> const view = QuestStats();
    std::string const load = statsOn.load(std::memory_order_acquire)
                                 ? Acore::StringFormat("loaded {} rows", statsLoadedRows)
                                 : std::string("not loaded");
    std::string const last =
        statsLastFlush
            ? Acore::StringFormat("last {} s ago ({} rows)", MsSince(*statsLastFlush) / IN_MILLISECONDS, statsLastRows)
            : std::string("none yet");
    handler->PSendSysMessage("QUEST stats: {}; {} rows in memory, {} dirty, {} open quests; view {} rows", load,
                             c.cells, c.dirty, c.open, view ? view->rows.size() : 0);
    handler->PSendSysMessage(
        "QUEST stats events: {} accepts, {} turn-ins ({} with minutes, {} without), {} abandons, {} drops, {} "
        "deaths; {} open quests pruned, {} accepts without a start (full)",
        c.accepts, c.turnIns, c.samples, c.unsampled, c.abandons, c.drops, c.deaths, c.pruned, c.full);
    handler->PSendSysMessage("QUEST stats flushes: {} every {} s, {}, {} rows written since startup", statsFlushes,
                             STATS_FLUSH_MS / IN_MILLISECONDS, last, statsRowsWritten);
    return true;
}

void SendQuestStats(ChatHandler* handler, uint32 questId)
{
    if (!statsOn.load(std::memory_order_acquire))
    {
        handler->PSendSysMessage("  statistics: not loaded");
        return;
    }
    std::vector<std::pair<uint8, QuestStatCell>> const cells = statsBook.CellsOf(questId);
    if (cells.empty())
        handler->PSendSysMessage("  statistics: none recorded yet");
    for (auto const& [band, cell] : cells)
    {
        QuestStatSummary const s = Summarize(cell);
        handler->PSendSysMessage(
            "  stats band {} (levels {}-{}): accepted {}, completed {}, median {} min ({} samples), deaths per "
            "completion {}, abandoned {}, deaths {}",
            band, band * LEVEL_BAND_LEVELS, band * LEVEL_BAND_LEVELS + LEVEL_BAND_LEVELS - 1, s.accepted, s.completed,
            MinutesText(s.medianMinutes), s.samples,
            s.deathsPerCompletion < 0.0f ? std::string("-") : Acore::StringFormat("{:.2f}", s.deathsPerCompletion),
            s.abandoned, s.deaths);
    }
}

uint16 FlagsOf(Quest const* quest)
{
    uint32 const type = quest->GetType();
    uint16 flags = 0;
    if (type == QuestType::ELITE)
        flags |= QuestFlag::ELITE;
    if (quest->GetSuggestedPlayers() > 1)
        flags |= QuestFlag::GROUP;
    if (type == QuestType::DUNGEON || type == QuestType::HEROIC)
        flags |= QuestFlag::DUNGEON;
    if (type == QuestType::RAID || type == QuestType::RAID_10 || type == QuestType::RAID_25)
        flags |= QuestFlag::RAID;
    if (type == QuestType::PVP || quest->HasFlag(QUEST_FLAGS_FLAGS_PVP))
        flags |= QuestFlag::PVP;
    if (quest->IsDaily())
        flags |= QuestFlag::DAILY;
    if (quest->IsWeekly())
        flags |= QuestFlag::WEEKLY;
    if (quest->IsRepeatable())
        flags |= QuestFlag::REPEATABLE;
    if (quest->IsSeasonal())
        flags |= QuestFlag::SEASONAL;
    return flags;
}

QuestRecord RecordOf(Quest const* quest)
{
    QuestRecord record;
    record.id = quest->GetQuestId();
    record.level = quest->GetQuestLevel();
    record.minLevel = quest->GetMinLevel();
    record.maxLevel = quest->GetMaxLevel();
    record.raceMask = quest->GetAllowableRaces();
    record.classMask = quest->GetRequiredClasses();
    record.zoneOrSort = quest->GetZoneOrSort();
    record.type = quest->GetType();
    record.suggestedPlayers = quest->GetSuggestedPlayers();
    record.flags = FlagsOf(quest);
    // A scaling quest (level -1) is valued at its minimum level
    int32 const level = record.level > 0 ? record.level : static_cast<int32>(record.minLevel);
    uint8 const at = static_cast<uint8>(std::clamp<int32>(level, 1, std::numeric_limits<uint8>::max()));
    record.xpBase = quest->XPValue(at);
    record.copperReward = quest->GetRewOrReqMoney(at);
    record.prev = quest->GetPrevQuestId();
    record.prevAny.assign(quest->prevQuests.begin(), quest->prevQuests.end());
    record.next = quest->GetNextQuestId();
    record.nextInChain = quest->GetNextQuestInChain();
    record.exclusiveGroup = quest->GetExclusiveGroup();
    record.breadcrumbFor = quest->GetBreadcrumbForQuestId();
    // WT35a: an event the gather names (an escort) or BuildObjectives clears (an area trigger's)
    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
        record.event = QuestEvent::Script;
    for (int32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;
        bool const object = npcOrGo < 0;
        record.objectives.push_back({object ? ObjectiveKind::Object : ObjectiveKind::Kill,
                                     static_cast<uint32>(std::abs(npcOrGo)),
                                     quest->RequiredNpcOrGoCount[i],
                                     i,
                                     {}});
    }
    // WT36a: an item the taking gives: the start item (at least one, Player::GiveQuestSourceItem), or a reward of a
    // quest that must be rewarded first
    auto const given = [quest](uint32 item, uint32 count)
    {
        if (item == quest->GetSrcItemId() && std::max<uint32>(quest->GetSrcItemCount(), 1) >= count)
            return true;
        for (int32 const prev : quest->prevQuests)
            if (Quest const* before = prev > 0 ? sObjectMgr->GetQuestTemplate(static_cast<uint32>(prev)) : nullptr)
                if (std::find(std::begin(before->RewardItemId), std::end(before->RewardItemId), item) !=
                        std::end(before->RewardItemId) ||
                    std::find(std::begin(before->RewardChoiceItemId), std::end(before->RewardChoiceItemId), item) !=
                        std::end(before->RewardChoiceItemId))
                    return true;
        return false;
    };
    for (int32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i])
        {
            record.objectives.push_back(
                {ObjectiveKind::Item, quest->RequiredItemId[i], quest->RequiredItemCount[i], POI_FIRST_ITEM + i, {}});
            record.objectives.back().given = given(quest->RequiredItemId[i], quest->RequiredItemCount[i]);
        }
    return record;
}

// What a spell can take: its explicit target mask and its effects' implicit targets
uint8 UseTargetsOf(SpellInfo const& info)
{
    uint8 targets = 0;
    uint32 const mask = info.GetExplicitTargetMask();
    if (mask & TARGET_FLAG_UNIT_MASK)
        targets |= UseTarget::UNIT;
    if (mask & TARGET_FLAG_GAMEOBJECT_MASK)
        targets |= UseTarget::OBJECT;
    if ((mask & TARGET_FLAG_CORPSE_MASK) || (info.AttributesEx2 & SPELL_ATTR2_ALLOW_DEAD_TARGET))
        targets |= UseTarget::DEAD;
    for (SpellEffectInfo const& effect : info.GetEffects())
    {
        if (!effect.IsEffect())
            continue;
        for (SpellImplicitTargetInfo const& target : {effect.TargetA, effect.TargetB})
            switch (target.GetObjectType())
            {
                case TARGET_OBJECT_TYPE_UNIT:
                case TARGET_OBJECT_TYPE_UNIT_AND_DEST:
                    targets |= target.GetTarget() == TARGET_UNIT_CASTER ? UseTarget::SELF : UseTarget::UNIT;
                    break;
                case TARGET_OBJECT_TYPE_GOBJ:
                case TARGET_OBJECT_TYPE_GOBJ_ITEM:
                    targets |= UseTarget::OBJECT;
                    break;
                case TARGET_OBJECT_TYPE_CORPSE:
                case TARGET_OBJECT_TYPE_CORPSE_ENEMY:
                case TARGET_OBJECT_TYPE_CORPSE_ALLY:
                    targets |= UseTarget::DEAD;
                    break;
                case TARGET_OBJECT_TYPE_SRC:
                case TARGET_OBJECT_TYPE_DEST:
                    targets |= UseTarget::SELF;
                    break;
                default:
                    break;
            }
    }
    return targets;
}

// A creature spawned lying dead (stand state or dynamic flag), as the corpses Symbol of Life and the vessels revive
bool LooksDead(uint32 entry)
{
    constexpr uint32 STAND_STATE_MASK = 0xFF;
    if (CreatureAddon const* addon = sObjectMgr->GetCreatureTemplateAddon(entry))
        if ((addon->bytes1 & STAND_STATE_MASK) == UNIT_STAND_STATE_DEAD)
            return true;
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
    return creature && (creature->dynamicflags & UNIT_DYNFLAG_DEAD);
}

void AddNamed(UseSpell& spell, Condition const* condition)
{
    if (!condition || condition->NegativeCondition)
        return;
    if (condition->ConditionType == CONDITION_OBJECT_ENTRY_GUID && condition->ConditionValue2 &&
        (condition->ConditionValue1 == TYPEID_UNIT || condition->ConditionValue1 == TYPEID_GAMEOBJECT))
    {
        bool const object = condition->ConditionValue1 == TYPEID_GAMEOBJECT;
        uint32 const entry = condition->ConditionValue2;
        spell.named.push_back({entry, object, !object && LooksDead(entry)});
    }
    else if (condition->ConditionType == CONDITION_NEAR_CREATURE && condition->ConditionValue1)
        spell.named.push_back({condition->ConditionValue1, false,
                               condition->ConditionValue3 != 0 || LooksDead(condition->ConditionValue1)});
}

// World thread only (GatherQuests): every quest's source items with an on-use spell; each spell's targets, the
// creatures and objects its conditions name (source types 17 and 13), its focus objects; the objects a key item
// unlocks; the spell-hit reactions (event 8) of kill targets and the gameobjects whose scripts, directly or through
// an action list, summon or credit one
void GatherUses(QuestGather& input)
{
    QuestUseGather& uses = input.uses;
    std::unordered_map<uint32, std::vector<uint32>> focusObjects;
    std::unordered_map<uint32, float> focusYards;
    std::unordered_map<uint32, std::vector<uint32>> unlockedBy;  // key item -> gameobject templates
    std::vector<uint32> scriptedObjects;
    for (auto const& [entry, info] : *sObjectMgr->GetGameObjectTemplates())
    {
        if (info.type == GAMEOBJECT_TYPE_SPELL_FOCUS && info.spellFocus.focusId)
        {
            focusObjects[info.spellFocus.focusId].push_back(entry);
            float const yards = static_cast<float>(info.spellFocus.dist);
            auto const [it, added] = focusYards.emplace(info.spellFocus.focusId, yards);
            if (!added)
                it->second = std::min(it->second, yards);
        }
        if (uint32 const lockId = info.GetLockId())
            if (LockEntry const* lock = sLockStore.LookupEntry(lockId))
                for (uint32 i = 0; i < MAX_LOCK_CASE; ++i)
                    if (lock->Type[i] == LOCK_KEY_ITEM && lock->Index[i])
                        unlockedBy[lock->Index[i]].push_back(entry);
        if (info.AIName == "SmartGameObjectAI")
            scriptedObjects.push_back(entry);
    }

    std::unordered_set<uint32> gathered;
    auto const addSpell = [&uses, &gathered, &focusObjects, &focusYards](uint32 spellId) -> bool
    {
        if (gathered.count(spellId))
            return true;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return false;
        gathered.insert(spellId);
        UseSpell spell;
        spell.spellId = spellId;
        spell.targets = UseTargetsOf(*info);
        spell.requiresDead = info->IsRequiringDeadTarget();
        spell.classFamily = info->SpellFamilyName != SPELLFAMILY_GENERIC;
        spell.focusId = info->RequiresSpellFocus;
        spell.rangeYards = info->GetMaxRange(info->IsPositive());
        if (spell.focusId)
            if (auto const it = focusObjects.find(spell.focusId); it != focusObjects.end())
            {
                spell.focusObjects = it->second;
                std::sort(spell.focusObjects.begin(), spell.focusObjects.end());
                spell.focusYards = focusYards[spell.focusId];
            }
        for (Condition const* condition :
             sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_SPELL, spellId))
            AddNamed(spell, condition);
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.ImplicitTargetConditions)
                for (Condition const* condition : *effect.ImplicitTargetConditions)
                    AddNamed(spell, condition);
        std::sort(spell.named.begin(), spell.named.end(), [](UseEntry const& a, UseEntry const& b)
                  { return std::tie(a.entry, a.isObject, a.dead) < std::tie(b.entry, b.isObject, b.dead); });
        spell.named.erase(std::unique(spell.named.begin(), spell.named.end(), [](UseEntry const& a, UseEntry const& b)
                                      { return a.entry == b.entry && a.isObject == b.isObject; }),
                          spell.named.end());
        // WT35g: what the spell brings forth (#2632's derivation: summon, kill credit and create-item effects)
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect == SPELL_EFFECT_SUMMON && effect.MiscValue > 0)
                spell.summons.push_back(static_cast<uint32>(effect.MiscValue));
            else if (effect.Effect == SPELL_EFFECT_SEND_EVENT && effect.MiscValue > 0)
            {
                // An event script's summon (q6001's Cenarion Moondust: event 5991 summons Lunaclaw)
                auto const script = sEventScripts.find(static_cast<uint32>(effect.MiscValue));
                if (script != sEventScripts.end())
                    for (auto const& [delay, step] : script->second)
                        if (step.command == SCRIPT_COMMAND_TEMP_SUMMON_CREATURE &&
                            step.TempSummonCreature.CreatureEntry)
                            spell.summons.push_back(step.TempSummonCreature.CreatureEntry);
            }
            else if ((effect.Effect == SPELL_EFFECT_KILL_CREDIT || effect.Effect == SPELL_EFFECT_KILL_CREDIT2) &&
                     effect.MiscValue > 0)
                spell.credits.push_back(static_cast<uint32>(effect.MiscValue));
            else if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType)
                spell.creates = effect.ItemType;
        }
        for (std::vector<uint32>* list : {&spell.summons, &spell.credits})
        {
            std::sort(list->begin(), list->end());
            list->erase(std::unique(list->begin(), list->end()), list->end());
        }
        for (std::size_t r = 0; spell.creates && r < info->Reagent.size(); ++r)
            if (info->Reagent[r] > 0 && info->ReagentCount[r] > 0)
                spell.reagents.emplace_back(static_cast<uint32>(info->Reagent[r]), info->ReagentCount[r]);
        uses.spells.push_back(std::move(spell));
        return true;
    };

    std::unordered_set<uint32> kills;
    for (QuestRecord const& record : input.quests)
        for (QuestObjective const& objective : record.objectives)
            if (objective.kind == ObjectiveKind::Kill)
                kills.insert(objective.target);
    for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
    {
        if (!quest)
            continue;
        std::array<uint32, 1 + QUEST_SOURCE_ITEM_IDS_COUNT> sources{quest->GetSrcItemId()};
        std::copy(std::begin(quest->ItemDrop), std::end(quest->ItemDrop), sources.begin() + 1);
        std::sort(sources.begin(), sources.end());
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            ItemTemplate const* item =
                sources[i] && (!i || sources[i] != sources[i - 1]) ? sObjectMgr->GetItemTemplate(sources[i]) : nullptr;
            if (!item)
                continue;
            for (auto const& use : item->Spells)
                if (use.SpellId > 0 && use.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE &&
                    addSpell(static_cast<uint32>(use.SpellId)))
                {
                    UseItem entry{id, item->ItemId, static_cast<uint32>(use.SpellId), {}};
                    if (auto const it = unlockedBy.find(item->ItemId); it != unlockedBy.end())
                        entry.lockObjects = it->second;
                    std::sort(entry.lockObjects.begin(), entry.lockObjects.end());
                    uses.items.push_back(std::move(entry));
                }
        }
    }
    for (uint32 const creature : kills)
        for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(creature, SMART_SCRIPT_TYPE_CREATURE))
            if (holder.event.type == SMART_EVENT_SPELLHIT && holder.event.spellHit.spell &&
                addSpell(holder.event.spellHit.spell))
                uses.hits.push_back({creature, holder.event.spellHit.spell});
    auto const credit = [&uses, &kills](uint32 object, SmartScriptHolder const& holder)
    {
        uint32 creature = 0;
        if (holder.action.type == SMART_ACTION_SUMMON_CREATURE)
            creature = holder.action.summonCreature.creature;
        else if (holder.action.type == SMART_ACTION_CALL_KILLEDMONSTER)
            creature = holder.action.killedMonster.creature;
        if (creature && kills.count(creature))
            uses.creators.push_back({object, creature});
    };
    for (uint32 const object : scriptedObjects)
        for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(object, SMART_SCRIPT_TYPE_GAMEOBJECT))
        {
            credit(object, holder);
            if (holder.action.type == SMART_ACTION_CALL_TIMED_ACTIONLIST)
                for (SmartScriptHolder const& step : sSmartScriptMgr->GetScript(
                         static_cast<int32>(holder.action.timedActionList.id), SMART_SCRIPT_TYPE_TIMED_ACTIONLIST))
                    credit(object, step);
        }

    // WT35g: a spell hit that turns a creature into a kill target (event 8 -> action 36), and the creatures whose
    // kill credits a kill target: their template's KillCredit, their death script's credit or cast of a credit spell
    auto const spellCredits = [](uint32 spellId)
    {
        std::vector<uint32> credited;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            for (SpellEffectInfo const& effect : info->GetEffects())
                if ((effect.Effect == SPELL_EFFECT_KILL_CREDIT || effect.Effect == SPELL_EFFECT_KILL_CREDIT2) &&
                    effect.MiscValue > 0)
                    credited.push_back(static_cast<uint32>(effect.MiscValue));
        return credited;
    };
    for (auto const& [entry, info] : *sObjectMgr->GetCreatureTemplates())
    {
        for (uint32 const credited : info.KillCredit)
            if (credited && kills.count(credited))
                uses.killCredits.push_back({entry, credited});
        if (info.AIName != "SmartAI")
            continue;
        for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(entry, SMART_SCRIPT_TYPE_CREATURE))
        {
            if (holder.event.type == SMART_EVENT_SPELLHIT && holder.event.spellHit.spell &&
                holder.action.type == SMART_ACTION_UPDATE_TEMPLATE &&
                kills.count(holder.action.updateTemplate.creature))
                uses.transforms.push_back({entry, holder.event.spellHit.spell, holder.action.updateTemplate.creature});
            if (holder.event.type != SMART_EVENT_DEATH)
                continue;
            std::vector<uint32> credited;
            if (holder.action.type == SMART_ACTION_CALL_KILLEDMONSTER)
                credited.push_back(holder.action.killedMonster.creature);
            else if (holder.action.type == SMART_ACTION_CAST)
                credited = spellCredits(holder.action.cast.spell);
            for (uint32 const creature : credited)
                if (kills.count(creature))
                    uses.killCredits.push_back({entry, creature});
        }
    }
}

// Whether a SmartAI row leads to an action `leads` names: the row itself, the rows it links or their timed action lists
template <typename Leads>
bool LeadsTo(SmartAIEventList const& script, SmartScriptHolder const& start, Leads leads)
{
    constexpr uint32 LINK_DEPTH = 4;  // links followed from the start row
    SmartScriptHolder const* row = &start;
    for (uint32 depth = 0; row && depth < LINK_DEPTH; ++depth)
    {
        if (leads(*row))
            return true;
        if (row->action.type == SMART_ACTION_CALL_TIMED_ACTIONLIST)
            for (SmartScriptHolder const& step : sSmartScriptMgr->GetScript(
                     static_cast<int32>(row->action.timedActionList.id), SMART_SCRIPT_TYPE_TIMED_ACTIONLIST))
                if (leads(step))
                    return true;
        uint32 const link = row->link;
        row = nullptr;
        for (SmartScriptHolder const& next : script)
            if (link && next.event_id == link)
                row = &next;
    }
    return false;
}

// World thread, in GatherQuests (WT35a): whether the creature's SmartAI starts a path or a follow when a player
// accepts the quest, in the accept row itself, the rows it links or its timed action list
bool StartsEscort(uint32 creature, uint32 questId)
{
    SmartAIEventList const script =
        sSmartScriptMgr->GetScript(static_cast<int32>(creature), SMART_SCRIPT_TYPE_CREATURE);
    auto const leads = [](SmartScriptHolder const& row)
    {
        return row.action.type == SMART_ACTION_ESCORT_START || row.action.type == SMART_ACTION_FOLLOW ||
               row.action.type == SMART_ACTION_START_CLOSEST_WAYPOINT;
    };
    for (SmartScriptHolder const& accept : script)
        if (accept.event.type == SMART_EVENT_ACCEPTED_QUEST &&
            (!accept.event.quest.quest || accept.event.quest.quest == questId) && LeadsTo(script, accept, leads))
            return true;
    return false;
}

// World thread, in GatherQuests (WT35f): how a creature gives a talk credit. A row of its SmartAI's gossip hello or
// of an option's select that credits the creature's own kill (the row, its links, a timed action list) tells it; a
// hello row wins, since the hello comes first. A gossip creature without such a row is Unknown
TalkCredit TalkOf(uint32 creature)
{
    constexpr uint32 GOSSIP_HELLO_REPORT_USE_ONLY = 2;  // a hello row's filter that runs on an object's use only
    TalkCredit talk;
    CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(creature);
    if (!info || !info->npcflag)
        return talk;
    if (info->AIName == "SmartAI")
    {
        SmartAIEventList const script =
            sSmartScriptMgr->GetScript(static_cast<int32>(creature), SMART_SCRIPT_TYPE_CREATURE);
        auto const credits = [creature](SmartScriptHolder const& row)
        { return row.action.type == SMART_ACTION_CALL_KILLEDMONSTER && row.action.killedMonster.creature == creature; };
        for (SmartScriptHolder const& row : script)
        {
            bool const hello = row.event.type == SMART_EVENT_GOSSIP_HELLO &&
                               row.event.gossipHello.filter != GOSSIP_HELLO_REPORT_USE_ONLY;
            // A row bound to an event phase is a scene's step another script starts (q10935): no plain talk credit
            if ((!hello && row.event.type != SMART_EVENT_GOSSIP_SELECT) || row.event.event_phase_mask ||
                !LeadsTo(script, row, credits))
                continue;
            if (hello)
                return {TalkHow::Hello, 0, 0, 0};
            if (talk.how == TalkHow::None)
                talk = {TalkHow::Select, row.event.gossip.sender, row.event.gossip.action, 0};
        }
    }
    if (talk.how == TalkHow::None && (info->npcflag & UNIT_NPC_FLAG_GOSSIP))
        talk.how = TalkHow::Unknown;
    return talk;
}

// World thread, in GatherQuests (WT35g): per quest, a creature whose gossip gives the quest's event: a SmartAI row
// of its hello or of an option's select that leads to the quest's event credit (q6001's Lunaclaw Spirit, menu 3862
// option 0). The first such row of the lowest entry wins
std::unordered_map<uint32, TalkCredit> EventTalks()
{
    constexpr uint32 GOSSIP_HELLO_REPORT_USE_ONLY = 2;
    std::unordered_map<uint32, TalkCredit> found;
    std::vector<uint32> entries;
    for (auto const& [entry, info] : *sObjectMgr->GetCreatureTemplates())
        if (info.npcflag && info.AIName == "SmartAI")
            entries.push_back(entry);
    std::sort(entries.begin(), entries.end());
    for (uint32 const creature : entries)
    {
        SmartAIEventList const script =
            sSmartScriptMgr->GetScript(static_cast<int32>(creature), SMART_SCRIPT_TYPE_CREATURE);
        for (SmartScriptHolder const& row : script)
        {
            bool const hello = row.event.type == SMART_EVENT_GOSSIP_HELLO &&
                               row.event.gossipHello.filter != GOSSIP_HELLO_REPORT_USE_ONLY;
            if ((!hello && row.event.type != SMART_EVENT_GOSSIP_SELECT) || row.event.event_phase_mask)
                continue;
            uint32 quest = 0;
            auto const credits = [&quest](SmartScriptHolder const& step)
            {
                if (step.action.type != SMART_ACTION_CALL_AREAEXPLOREDOREVENTHAPPENS || !step.action.quest.quest)
                    return false;
                quest = step.action.quest.quest;
                return true;
            };
            if (!LeadsTo(script, row, credits))
                continue;
            TalkCredit talk = hello ? TalkCredit{TalkHow::Hello, 0, 0, 0}
                                    : TalkCredit{TalkHow::Select, row.event.gossip.sender, row.event.gossip.action, 0};
            talk.creature = creature;
            found.try_emplace(quest, talk);
        }
    }
    return found;
}

// World thread, in GatherQuests (WT32a): the loot rows of creatures, skinning and objects and the reference loot, one
// query per table with the core's column types (LootStore::LoadLootTable), and the templates owning each loot id
void GatherSources(SourceGather& sources)
{
    auto const load = [](char const* table, std::vector<LootRow>& rows)
    {
        // The default loot mode only: the other modes are hard-mode loot a boss script switches on
        QueryResult result = WorldDatabase.Query(
            "SELECT Entry, Item, Reference, Chance, QuestRequired, GroupId, MinCount, "
            "MaxCount FROM {} WHERE LootMode = 0 OR (LootMode & {}) <> 0",
            table, static_cast<uint32>(LOOT_MODE_DEFAULT));
        if (!result)
            return;
        rows.reserve(result->GetRowCount());
        do
        {
            Field* fields = result->Fetch();
            LootRow row;
            row.lootId = fields[0].Get<uint32>();
            row.reference = static_cast<uint32>(std::abs(fields[2].Get<int32>()));
            row.item = row.reference ? 0 : fields[1].Get<uint32>();
            row.chance = fields[3].Get<float>();
            row.questRequired = fields[4].Get<bool>();
            row.groupId = fields[5].Get<uint8>();
            row.minCount = fields[6].Get<uint8>();
            row.maxCount = fields[7].Get<uint8>();
            // The core never drops an item without a template (LootGroupInvalidSelector, Loot::AddItem)
            if (row.reference || sObjectMgr->GetItemTemplate(row.item))
                rows.push_back(row);
        } while (result->NextRow());
    };
    load("creature_loot_template", sources.tables[static_cast<std::size_t>(SourceKind::Corpse)]);
    load("skinning_loot_template", sources.tables[static_cast<std::size_t>(SourceKind::Skin)]);
    load("gameobject_loot_template", sources.tables[static_cast<std::size_t>(SourceKind::Object)]);
    load("reference_loot_template", sources.references);
    for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
    {
        if (creature.lootid)
            sources.owners.push_back({SourceKind::Corpse, creature.lootid, entry});
        if (creature.SkinLootId)
            sources.owners.push_back({SourceKind::Skin, creature.SkinLootId, entry});
    }
    // Chests (herbs and veins too) and fishing holes
    for (auto const& [entry, object] : *sObjectMgr->GetGameObjectTemplates())
        if (uint32 const lootId = object.GetLootId())
            sources.owners.push_back({SourceKind::Object, lootId, entry});
}

// World thread only: copies the quest templates, relations, POIs, area triggers and the spawns of givers, enders and
// kill or object targets (GM commands change these stores at runtime), so the worker reads only this copy. WT33b: each
// POI point carries its baked floor from `floors`
QuestGather GatherQuests(QuestPoiZStore const& floors)
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::GatherQuests");
    QuestGather input;
    std::unordered_set<uint64> targets;  // EntryKey of kill and object targets
    for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
    {
        if (!quest)
            continue;
        QuestRecord record = RecordOf(quest);
        for (QuestObjective const& objective : record.objectives)
            if (objective.kind == ObjectiveKind::Kill || objective.kind == ObjectiveKind::Object)
                targets.insert(Detail::EntryKey(objective.target, objective.kind == ObjectiveKind::Object));
        if (QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(id))
            for (QuestPOI const& poi : *pois)
            {
                PoiInput entry{id, poi.ObjectiveIndex, poi.MapId, {}, {}, poi.Id};
                entry.points.reserve(poi.points.size());
                entry.floors.reserve(poi.points.size());
                // The core loads the points ordered by Idx2, dense from 0: a point's index is the table's PointIdx
                for (std::size_t i = 0; i < poi.points.size(); ++i)
                {
                    entry.points.emplace_back(static_cast<float>(poi.points[i].x), static_cast<float>(poi.points[i].y));
                    std::optional<float> const floor = floors.Floor(id, poi.Id, static_cast<uint32>(i), poi.MapId);
                    entry.floors.push_back(floor ? *floor : NO_FLOOR);
                }
                input.pois.push_back(std::move(entry));
            }
        input.quests.push_back(std::move(record));
    }

    std::unordered_set<uint64> related;  // EntryKey of givers and enders
    auto const relate = [&input, &related](QuestRelations const* relations, bool isObject, bool ender)
    {
        if (!relations)
            return;
        for (auto const& [entry, questId] : *relations)
        {
            input.relations.push_back({questId, entry, isObject, ender});
            related.insert(Detail::EntryKey(entry, isObject));
        }
    };
    relate(sObjectMgr->GetCreatureQuestRelationMap(), false, false);
    relate(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), false, true);
    relate(sObjectMgr->GetGOQuestRelationMap(), true, false);
    relate(sObjectMgr->GetGOQuestInvolvedRelationMap(), true, true);

    // WT35a: an event quest a giver escorts or leads (QuestEvent::Escort)
    std::unordered_set<uint32> escorts;
    if (QuestRelations const* starters = sObjectMgr->GetCreatureQuestRelationMap())
        for (auto const& [entry, questId] : *starters)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (quest && quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) &&
                StartsEscort(entry, questId))
                escorts.insert(questId);
        }
    for (QuestRecord& record : input.quests)
        if (record.event == QuestEvent::Script && escorts.count(record.id))
            record.event = QuestEvent::Escort;

    GatherSources(input.sources);
    std::unordered_set<uint64> sourceEntries;  // EntryKey of loot owners
    for (LootOwner const& owner : input.sources.owners)
        sourceEntries.insert(Detail::EntryKey(owner.entry, owner.kind == SourceKind::Object));

    // WT32b: the facts of a loot-owning creature the per-bot filters need, once per template. The sides are the
    // player factions of a human (template 1) and an orc (2); a creature friendly to a side is no place for it
    FactionTemplateEntry const* const sideFactions[2] = {sFactionTemplateStore.LookupEntry(1),
                                                         sFactionTemplateStore.LookupEntry(2)};
    std::unordered_map<uint32, std::pair<uint16, uint8>> creatureFacts;
    auto const factsOf = [&creatureFacts, &sideFactions](uint32 entry)
    {
        auto const [cached, added] = creatureFacts.try_emplace(entry);
        if (!added)
            return cached->second;
        if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry))
        {
            cached->second.first = static_cast<uint16>(creature->GetRequiredLootSkill());
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creature->faction);
            for (uint32 team = 0; faction && team < 2; ++team)
                if (sideFactions[team] && faction->IsFriendlyTo(*sideFactions[team]) &&
                    !faction->IsHostileTo(*sideFactions[team]))
                    cached->second.second |= static_cast<uint8>(1u << team);
        }
        return cached->second;
    };
    // WT35f: a kill objective's creature that gives its credit through its gossip, for the sides it is friendly to
    std::unordered_map<uint32, TalkCredit> talks;
    for (QuestRecord& record : input.quests)
        for (QuestObjective& objective : record.objectives)
            if (objective.kind == ObjectiveKind::Kill)
            {
                auto const [cached, added] = talks.try_emplace(objective.target);
                if (added)
                {
                    cached->second = TalkOf(objective.target);
                    cached->second.friendlyTo = factsOf(objective.target).second;
                }
                objective.talk = cached->second;
            }
    // WT35g: an event quest's talker (EventTalks), the chain's last link after its use
    std::unordered_map<uint32, TalkCredit> const eventTalks = EventTalks();
    for (QuestRecord& record : input.quests)
        if (record.event == QuestEvent::Script)
            if (auto const it = eventTalks.find(record.id); it != eventTalks.end())
            {
                record.eventUse.talk = it->second;
                record.eventUse.talk.friendlyTo = factsOf(it->second.creature).second;
            }

    // WT35h: the sides a giver or ender creature is hostile to, once per template. Only a faction without reputation:
    // with one the reaction is the player's standing (at war or not), which the runtime hand-in test reads
    std::unordered_map<uint32, uint8> hostility;
    auto const hostileTo = [&hostility, &sideFactions](uint32 entry)
    {
        auto const [cached, added] = hostility.try_emplace(entry, uint8(0));
        CreatureTemplate const* creature = added ? sObjectMgr->GetCreatureTemplate(entry) : nullptr;
        FactionTemplateEntry const* faction = creature ? sFactionTemplateStore.LookupEntry(creature->faction) : nullptr;
        FactionEntry const* reputation = faction ? sFactionStore.LookupEntry(faction->faction) : nullptr;
        for (uint32 team = 0; faction && !(reputation && reputation->CanHaveReputation()) && team < 2; ++team)
            if (sideFactions[team] && faction->IsHostileTo(*sideFactions[team]))
                cached->second |= static_cast<uint8>(1u << team);
        return cached->second;
    };
    auto const spawn = [&input, &related, &targets, &sourceEntries, &factsOf, &hostileTo](
                           bool isObject, uint32 entry, uint32 mapId, float x, float y, float z, uint32 phaseMask)
    {
        uint64 const key = Detail::EntryKey(entry, isObject);
        if (related.count(key))
        {
            QuestPlace place{mapId, x, y, z, entry, isObject};
            if (!ResolveArea(mapId, x, y, z, place.zoneId, place.areaId))
                ++input.unresolved;
            place.hostileTo = isObject ? 0 : hostileTo(entry);
            place.otherPhase = !(phaseMask & PHASEMASK_NORMAL);
            input.places.push_back(place);
        }
        if (targets.count(key))
            input.targets.push_back({entry, isObject, mapId, x, y, z});
        if (sourceEntries.count(key))
        {
            SourceSpawn source{entry, isObject, mapId, x, y, z};
            if (!isObject)
                std::tie(source.skill, source.friendlyTo) = factsOf(entry);
            input.sources.spawns.push_back(source);
        }
    };
    // WT35b: the spawns of every creature or object a use rule may name (the index places a rule's objective there)
    GatherUses(input);
    for (UseSpell const& spell : input.uses.spells)
    {
        for (UseEntry const& named : spell.named)
            targets.insert(Detail::EntryKey(named.entry, named.isObject));
        for (uint32 const object : spell.focusObjects)
            targets.insert(Detail::EntryKey(object, true));
    }
    for (UseItem const& item : input.uses.items)
        for (uint32 const object : item.lockObjects)
            targets.insert(Detail::EntryKey(object, true));
    for (SpellHit const& hit : input.uses.hits)
        targets.insert(Detail::EntryKey(hit.creature, false));
    for (CreatorObject const& creator : input.uses.creators)
        targets.insert(Detail::EntryKey(creator.object, true));
    // Game event spawns exist only while their event runs, as for the services
    std::unordered_set<uint32> const creatureEvents = EventCreatureSpawns();
    for (auto const& [guid, data] : sObjectMgr->GetAllCreatureData())
        if (!creatureEvents.count(guid))
            spawn(false, data.id, data.mapid, data.posX, data.posY, data.posZ, data.phaseMask);
    std::unordered_set<uint32> const objectEvents = EventObjectSpawns();
    for (auto const& [guid, data] : sObjectMgr->GetAllGOData())
        if (!objectEvents.count(guid))
            spawn(true, data.id, data.mapid, data.posX, data.posY, data.posZ, data.phaseMask);

    for (uint32 trigger = 1; trigger < AREATRIGGER_ID_LIMIT; ++trigger)
    {
        uint32 const questId = sObjectMgr->GetQuestForAreaTrigger(trigger);
        AreaTrigger const* at = questId ? sObjectMgr->GetAreaTrigger(trigger) : nullptr;
        if (!at)
            continue;
        float const radius = at->radius > 0.0f ? at->radius : 0.5f * std::hypot(at->length, at->width);
        input.triggers.push_back({questId, trigger, at->map, at->x, at->y, at->z, radius});
    }
    return input;
}

std::string AreaName(ChatHandler* handler, uint32 id)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(id);
    return area ? area->area_name[handler->GetSessionDbcLocale()] : "unknown";
}

std::string EntryName(uint32 entry, bool isObject)
{
    if (isObject)
    {
        GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(entry);
        return object ? object->name : "unknown object";
    }
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
    return creature ? creature->Name : "unknown creature";
}

std::string TargetName(QuestObjective const& objective)
{
    switch (objective.kind)
    {
        case ObjectiveKind::Kill:
            return EntryName(objective.target, false);
        case ObjectiveKind::Object:
            return EntryName(objective.target, true);
        case ObjectiveKind::Item:
        {
            ItemTemplate const* item = sObjectMgr->GetItemTemplate(objective.target);
            return item ? item->Name1 : "unknown item";
        }
        case ObjectiveKind::Explore:
            return "area trigger";
        default:
            return "turn-in";
    }
}

std::string FlagText(QuestRecord const& quest)
{
    static constexpr std::array<std::pair<uint16, char const*>, 9> NAMES = {{{QuestFlag::ELITE, "elite"},
                                                                             {QuestFlag::GROUP, "group"},
                                                                             {QuestFlag::DUNGEON, "dungeon"},
                                                                             {QuestFlag::RAID, "raid"},
                                                                             {QuestFlag::PVP, "pvp"},
                                                                             {QuestFlag::DAILY, "daily"},
                                                                             {QuestFlag::WEEKLY, "weekly"},
                                                                             {QuestFlag::REPEATABLE, "repeatable"},
                                                                             {QuestFlag::SEASONAL, "seasonal"}}};
    std::string text;
    for (auto const& [flag, name] : NAMES)
        if (quest.Has(flag))
            text += Acore::StringFormat("{}{}", text.empty() ? "" : ", ", name);
    if (quest.event != QuestEvent::None)
        text += Acore::StringFormat("{}{}", text.empty() ? "" : ", ",
                                    quest.event == QuestEvent::Escort ? "escort" : "script event");
    return text.empty() ? "none" : text;
}

std::string ListText(std::vector<uint32> const& ids)
{
    std::string text;
    for (uint32 const id : ids)
        text += Acore::StringFormat("{}{}", text.empty() ? "" : ", ", id);
    return text.empty() ? "none" : text;
}

// The camp and the services hub it joins (QuestIndex.h, CampHub)
std::string CampText(QuestIndex const& quests, PlannerIndexes const* indexes, uint32 campIndex)
{
    if (campIndex >= quests.camps.size())
        return "no camp";
    QuestCamp const& camp = quests.camps[campIndex];
    std::string text = Acore::StringFormat("camp #{} ({} places, {} quests, {:.0f} yd)", campIndex, camp.places.size(),
                                           camp.quests.size(), camp.radius);
    if (!indexes)
        return text + ", hubs not ready";
    if (std::optional<Nearby> const hub = CampHub(camp, *indexes))
        return text + Acore::StringFormat(", hub #{} ({:.0f} yd from its centre)", hub->index, hub->distance);
    std::optional<Nearby> const nearest = indexes->NearestHub(camp.mapId, camp.x, camp.y, camp.z);
    return text + (nearest ? Acore::StringFormat(", in no hub's core (nearest hub #{} {:.0f} yd)", nearest->index,
                                                 nearest->distance)
                           : std::string(", no hub on the map"));
}

void SendPlaces(ChatHandler* handler, QuestIndex const& quests, PlannerIndexes const* indexes,
                std::vector<uint32> const& list, char const* role)
{
    if (list.empty())
        handler->PSendSysMessage("  {}: none", role);
    for (std::size_t i = 0; i < list.size() && i < QUERY_PLACES; ++i)
    {
        QuestPlace const& place = quests.places[list[i]];
        handler->PSendSysMessage("  {} {} {} at ({}, {:.0f}, {:.0f}, {:.0f}), {} in {}; {}", role, place.entry,
                                 EntryName(place.entry, place.isObject), place.mapId, place.x, place.y, place.z,
                                 AreaName(handler, place.areaId), AreaName(handler, place.zoneId),
                                 CampText(quests, indexes, place.camp));
    }
    if (list.size() > QUERY_PLACES)
        handler->PSendSysMessage("  ... and {} more {} spawns", list.size() - QUERY_PLACES, role);
}

// The circuit harness's real places (WT13b): at fixed map coordinates among the quest givers, checked against the
// quest index when the harness runs (the givers within the hub's reach, the nearest camp)
struct CircuitPlace
{
    char const* name;
    uint8 level;
    uint8 race;
    uint8 cls;
    uint8 team;  // 0 Alliance, 1 Horde
    uint32 mapId;
    float x;
    float y;
    float z;
};

// Goldshire's quests are levels 7-10: at level 12 they are grey (grey difference 4) and only deliveries are left, so
// the human is 9 (the coordinator's call after the WT13b review)
constexpr std::array<CircuitPlace, 3> CIRCUIT_PLACES{{
    {"goldshire-9-human", 9, RACE_HUMAN, CLASS_WARRIOR, 0, 0, -9465.0f, 40.0f, 57.0f},
    {"crossroads-22-orc", 22, RACE_ORC, CLASS_WARRIOR, 1, 1, -441.0f, -2596.0f, 92.0f},
    {"gromgol-35-troll", 35, RACE_TROLL, CLASS_HUNTER, 1, 0, -12370.0f, 175.0f, 4.0f},
}};
constexpr uint32 CIRCUIT_TIMING_RUNS = 50;      // plans per real scenario behind the job time's p50/p95
constexpr float CIRCUIT_CAMP_YARDS = 300.0f;    // the nearest quest camp to a place is looked for this far
constexpr std::size_t CIRCUIT_CAMPS_TRIED = 8;  // camps tried for one with quests, nearest first
constexpr uint16 CIRCUIT_BAG_SLOTS = 16;        // the synthetic bot's free bag slots

// The published data a circuit job reads, taken on the calling thread with the core's grey level difference
struct CircuitData
{
    std::shared_ptr<PlannerIndexes const> indexes;
    std::shared_ptr<QuestIndex const> quests;
    std::shared_ptr<QuestStatsView const> stats;
    std::shared_ptr<CrowdingSnapshot const> crowding;
    uint32 greyDiff{Circuit::GREY_DIFF};

    // The pointers stay valid while this data (or a copy of its shared pointers) lives; travel: the way's price
    // (a goals job's JobTravel), empty = straight yards
    CircuitContext Context(TravelCost travel = {}) const
    {
        CircuitContext context;
        context.travel = std::move(travel);
        context.quests = quests.get();
        context.indexes = indexes.get();
        context.stats = stats.get();
        context.crowding = crowding.get();
        context.greyDiff = greyDiff;
        return context;
    }
};

CircuitData CircuitDataNow()
{
    CircuitData data;
    data.indexes = IndexesReady() ? Indexes() : nullptr;
    data.quests = QuestsReady() ? Quests() : nullptr;
    data.stats = QuestStats();
    data.crowding = Crowding();
    data.greyDiff = sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF);
    return data;
}

Snapshot CircuitBot(CircuitPlace const& place, Place const& start)
{
    Snapshot bot;
    bot.level = place.level;
    bot.race = place.race;
    bot.cls = place.cls;
    bot.team = place.team;
    bot.position = start;
    bot.runSpeed = Circuit::DEFAULT_RUN_SPEED;
    bot.bagSlots = CIRCUIT_BAG_SLOTS;
    bot.bagFree = CIRCUIT_BAG_SLOTS;
    return bot;
}

// The place's start: its own position when a quest giver stands within the hub's reach, else the nearest camp that
// gives quests. False when neither has a giver within the hub's reach
bool CircuitStart(QuestIndex const& quests, CircuitPlace const& place, Place& start, std::string& text)
{
    start = {place.mapId, place.x, place.y, place.z};
    std::size_t givers = quests.GiversNear(place.mapId, place.x, place.y, Circuit::HUB_GIVER_YARDS).size();
    std::string where = Acore::StringFormat("({:.0f}, {:.0f}) on map {}", place.x, place.y, place.mapId);
    std::string camp = Acore::StringFormat("no camp with quests within {:.0f} yd", CIRCUIT_CAMP_YARDS);
    for (Nearby const& nearby :
         quests.NearestCamps(place.mapId, place.x, place.y, place.z, CIRCUIT_CAMPS_TRIED, CIRCUIT_CAMP_YARDS))
    {
        QuestCamp const& found = quests.camps[nearby.index];
        if (found.quests.empty())
            continue;
        camp =
            Acore::StringFormat("camp {} ({} quests, {} places, radius {:.0f} yd, zone {}) at {:.0f} yd", nearby.index,
                                found.quests.size(), found.places.size(), found.radius, found.zoneId, nearby.distance);
        if (!givers)
        {
            start = {place.mapId, found.x, found.y, found.z};
            givers = quests.GiversNear(place.mapId, found.x, found.y, Circuit::HUB_GIVER_YARDS).size();
            where = Acore::StringFormat("the camp's centre ({:.0f}, {:.0f})", found.x, found.y);
        }
        break;
    }
    text = Acore::StringFormat("{} quest givers within {:.0f} yd of {}; nearest {}", givers, Circuit::HUB_GIVER_YARDS,
                               where, camp);
    return givers > 0;
}

// The synthetic bot's history: every quest grey for its level, as a bot that levelled through the world has done
// (prerequisites of the hub's chains met; the grey ones skipped either way), ascending as the index
std::vector<uint32> GreyHistory(QuestIndex const& quests, uint8 level, uint32 greyDiff)
{
    std::vector<uint32> ids;
    for (QuestRecord const& quest : quests.quests)
        if (quest.level > 0 &&
            static_cast<int32>(level) > Circuit::QuestLevel(quest, level) + static_cast<int32>(greyDiff))
            ids.push_back(quest.id);
    return ids;
}

// Every quest the givers within the hub's reach offer, sorted: rewarded, they make the hub dry
std::vector<uint32> HubQuests(QuestIndex const& quests, Place const& at)
{
    std::vector<uint32> ids;
    for (uint32 const place : quests.GiversNear(at.map, at.x, at.y, Circuit::HUB_GIVER_YARDS))
        for (uint32 const id : quests.QuestsGivenAt(place))
            ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Log quests for the full-log fixture, ascending: every objective on another map than the place's (work the bot
// cannot do from the hub), grey for its level or not, never a repeatable, elite, group, instance or exclusive quest,
// and none that a hub quest names (prerequisite, breadcrumb), so the hub offers what it offers to an empty log
std::vector<uint32> FarQuests(QuestIndex const& quests, CircuitPlace const& place, std::vector<uint32> const& hub,
                              uint32 greyDiff, bool grey, std::size_t count)
{
    std::vector<uint32> named;
    for (uint32 const id : hub)
        if (QuestRecord const* quest = quests.Find(id))
        {
            named.push_back(static_cast<uint32>(quest->prev < 0 ? -quest->prev : quest->prev));
            named.push_back(quest->breadcrumbFor);
        }
    std::vector<uint32> ids;
    for (QuestRecord const& quest : quests.quests)
    {
        if (ids.size() >= count)
            break;
        bool const isGrey =
            static_cast<int32>(place.level) > Circuit::QuestLevel(quest, place.level) + static_cast<int32>(greyDiff);
        bool const above =
            Circuit::QuestLevel(quest, place.level) > static_cast<int32>(place.level) + Circuit::MAX_LEVELS_ABOVE;
        if (quest.level <= 0 || isGrey != grey || above || quest.exclusiveGroup ||
            (quest.flags & (Circuit::REPEAT_FLAGS | Circuit::GROUP_FLAGS | Circuit::INSTANCE_FLAGS)) ||
            std::binary_search(hub.begin(), hub.end(), quest.id) ||
            std::find(named.begin(), named.end(), quest.id) != named.end())
            continue;
        bool placed = false;
        bool elsewhere = true;
        for (QuestObjective const& objective : quest.objectives)
            for (AreaPoint const& area : objective.areas)
            {
                placed = placed || objective.kind != ObjectiveKind::Turnin;
                elsewhere = elsewhere && area.mapId != place.mapId;
            }
        if (placed && elsewhere)
            ids.push_back(quest.id);
    }
    return ids;
}

QuestSlot LogSlot(uint32 quest, uint32 state)
{
    QuestSlot slot;
    slot.quest = quest;
    slot.state = state;
    return slot;
}

void AddChecks(std::vector<CircuitHarness::Check>& checks, std::vector<CircuitHarness::Check> const& more)
{
    checks.insert(checks.end(), more.begin(), more.end());
}

void AddPlanLines(std::vector<std::string>& lines, std::string const& name, CircuitHarness::Report const& report)
{
    lines.push_back(Acore::StringFormat(
        "{} (hub camp {}):", name,
        report.plan.homeCamp == NO_CAMP ? std::string("none") : std::to_string(report.plan.homeCamp)));
    for (std::string const& line : CircuitHarness::PlanLines(report.plan, report.baselineYards))
        lines.push_back("  " + line);
}

std::string PlaceCensusText(std::array<uint32, PLACE_FROMS> const& counts)
{
    std::string text;
    for (std::size_t i = 0; i < PLACE_FROMS; ++i)
        text += Acore::StringFormat("{}{} {}", i ? ", " : "", counts[i], PLACE_FROM_NAMES[i]);
    return text;
}

// WT32b: objectives by where their places come from (PLAN 3.1's six; `use` and the baked floor do not exist yet, so
// `poi` is what rule 5 would serve), and what the item objectives had before the sources
void LogPlaceCensus(QuestIndexStats const& s)
{
    LOG_INFO("playerbots",
             "Quest index places: {} objectives ({} turn-ins apart) by place source: {}; item objectives: {}; before "
             "the sources they were {} poi, {} none",
             s.objectives - s.turnins, s.turnins, PlaceCensusText(s.placeFrom), PlaceCensusText(s.itemPlaceFrom),
             s.itemBeforePoi, s.itemBeforeNone);
}

// WT36a: what the planner's feed offers by the capability table (QuestExecRules.h): per side and level band (of the
// minimum level, QuestStats.h) the quests of the side, those offered (the executor does every objective), those not
// by their first refused work; and per side the offered quests by each work they hold
constexpr std::size_t CAPABILITY_BANDS = 17;  // levels 0-84
constexpr std::size_t REFUSAL_PREFIX = 12;    // "unsupported:" before the refused work's word
constexpr std::array<char const*, SIDES> SIDE_NAMES = {"Alliance", "Horde"};

struct CapabilityCensus
{
    std::array<std::array<uint32, CAPABILITY_BANDS>, SIDES> total{};
    std::array<std::array<uint32, CAPABILITY_BANDS>, SIDES> offered{};
    std::array<std::array<std::array<uint32, CAPABILITY_BANDS>, QuestExec::WORKS>, SIDES> refused{};
    std::array<std::array<uint32, QuestExec::WORKS>, SIDES> offeredWith{};
    uint32 offeredUnsupported{0};  // the invariant: a record the feed passes that the executor refuses (must be 0)
};

bool OfSide(QuestRecord const& quest, std::uint8_t side)
{
    return !quest.raceMask || (quest.raceMask & SIDE_RACE_MASKS[side]);
}

char const* RefusedWord(QuestExec::Work work) { return QuestExec::CapabilityOf(work).refusal + REFUSAL_PREFIX; }

// Whether the feed passes the quest for some character of the side: a race of the side, a class and a level the
// template allows (Eligible, with its capability refusal)
bool FeedPasses(QuestRecord const& quest, std::uint8_t side)
{
    uint8 const level = static_cast<uint8>(std::clamp<uint32>(quest.minLevel, 1, 255));
    uint8 cls = 1;
    while (quest.classMask && cls < 32 && !(quest.classMask & (1u << (cls - 1))))
        ++cls;
    for (uint8 race = 1; race <= 32; ++race)
        if ((SIDE_RACE_MASKS[side] & (1u << (race - 1))) && Eligible(quest, race, cls, level))
            return true;
    return false;
}

CapabilityCensus CensusOf(QuestIndex const& quests)
{
    CapabilityCensus census;
    for (QuestRecord const& quest : quests.quests)
        for (std::uint8_t side = 0; side < SIDES; ++side)
        {
            if (!OfSide(quest, side))
                continue;
            std::size_t const band = std::min<std::size_t>(LevelBandOf(quest.minLevel), CAPABILITY_BANDS - 1);
            ++census.total[side][band];
            QuestExec::Step const refusal = QuestExec::FirstRefusal(quest, side);
            if (refusal.act == QuestExec::Act::Refused)
            {
                ++census.refused[side][static_cast<std::size_t>(refusal.work)][band];
                if (FeedPasses(quest, side))
                    ++census.offeredUnsupported;
                continue;
            }
            ++census.offered[side][band];
            std::array<bool, QuestExec::WORKS> with{};
            for (uint32 i = 0; i < QuestExec::ObjectiveCount(quest); ++i)
                if (QuestExec::Work const work = QuestExec::WorkOf(QuestExec::ObjectiveAt(quest, i), side);
                    work != QuestExec::Work::Count)
                    with[static_cast<std::size_t>(work)] = true;
            for (std::size_t w = 0; w < QuestExec::WORKS; ++w)
                census.offeredWith[side][w] += with[w] ? 1 : 0;
        }
    return census;
}

uint32 SumOf(std::array<uint32, CAPABILITY_BANDS> const& bands)
{
    return std::accumulate(bands.begin(), bands.end(), uint32{0});
}

// "escort 12, event 30, ..." over the works with a count; "none" without one
template <typename CountOf>
std::string WorkCounts(CountOf countOf, bool refusals)
{
    std::string text;
    for (std::size_t w = 0; w < QuestExec::WORKS; ++w)
        if (uint32 const n = countOf(w))
            text += Acore::StringFormat(
                "{}{} {}", text.empty() ? "" : ", ",
                refusals ? RefusedWord(static_cast<QuestExec::Work>(w)) : QuestExec::CAPABILITIES[w].name, n);
    return text.empty() ? std::string("none") : text;
}

std::string SideLine(CapabilityCensus const& c, std::uint8_t side)
{
    uint32 const total = SumOf(c.total[side]);
    uint32 const offered = SumOf(c.offered[side]);
    return Acore::StringFormat("{}: {} quests, {} offered, {} not offered ({})", SIDE_NAMES[side], total, offered,
                               total - offered,
                               WorkCounts([&](std::size_t w) { return SumOf(c.refused[side][w]); }, true));
}
}  // namespace

void StartQuestIndex()
{
    QuestIndexState expected = QuestIndexState::NotStarted;
    if (!questState.compare_exchange_strong(expected, QuestIndexState::Building))
        return;  // once: quest data is static after startup (WT.12)

    auto const gatherStart = std::chrono::steady_clock::now();
    // WT33b: the floors are read here on the world thread (a startup query, as the gather's WorldDatabase reads) and
    // dropped after the gather: the index carries them
    std::shared_ptr<QuestGather> input;
    {
        QuestPoiZStore floors;
        floors.Load();
        input = std::make_shared<QuestGather>(GatherQuests(floors));
    }
    uint32 const gatherMs = MsSince(gatherStart);
    LOG_INFO(
        "server.loading",
        "Quest index: gathered {} quests in {} ms ({} relations, {} giver and ender spawns, {} target spawns, {} "
        "POIs, {} area triggers; {} spawns unresolved; {} source items with an on-use spell, {} use spells, {} spell "
        "hits, {} creator objects; {} corpse, {} skin, {} object and {} reference loot rows, {} loot owners, {} "
        "source spawns)",
        input->quests.size(), gatherMs, input->relations.size(), input->places.size(), input->targets.size(),
        input->pois.size(), input->triggers.size(), input->unresolved, input->uses.items.size(),
        input->uses.spells.size(), input->uses.hits.size(), input->uses.creators.size(),
        input->sources.tables[0].size(), input->sources.tables[1].size(), input->sources.tables[2].size(),
        input->sources.references.size(), input->sources.owners.size(), input->sources.spawns.size());

    bool const posted = Pool().Post(
        [input, gatherMs]
        {
            auto const buildStart = std::chrono::steady_clock::now();
            try
            {
                auto index = std::make_shared<QuestIndex>(QuestIndex::Build(std::move(*input)));
                QuestExec::MarkRefusals(index->quests);  // WT36a: before the publish, so every reader sees it
                ClassQuests::JoinInto(*index);           // WU38: after the refusals; the planner job reads the rows
                index->stats.gatherMs = gatherMs;
                index->stats.buildMs = MsSince(buildStart);
                QuestIndexStats const& s = index->stats;
                LOG_INFO("playerbots",
                         "Quest index built in {} ms on a worker: {} quests, {} givers, {} enders, {} camps, {} "
                         "objectives ({} without area), {} quests without a giver, about {:.1f} MB (estimate)",
                         s.buildMs, s.quests, s.givers, s.enders, s.camps, s.objectives, s.withoutArea, s.withoutGiver,
                         static_cast<double>(s.estimatedBytes) / BYTES_PER_MB);
                LogPlaceCensus(s);
                ClassQuests::JoinCounts const classCounts = ClassQuests::CountsOf(index->classRows);
                LOG_INFO("playerbots",
                         "Quest index class quests (planner feed): {} class quests, {} steps, {} refused on a side, "
                         "{} not in the index",
                         classCounts.classQuests, classCounts.steps, classCounts.refused, classCounts.missing);
                CapabilityCensus const capability = CensusOf(*index);
                LOG_INFO("playerbots", "Quest index capability (planner feed): {}; {}; offered and unsupported: {}",
                         SideLine(capability, 0), SideLine(capability, 1), capability.offeredUnsupported);
                QuestUseStats const& u = index->uses.stats;
                LOG_INFO("playerbots",
                         "Quest index: {} use rules for {} quests ({} summon, {} transform), {} source spells name "
                         "nothing, {} violations",
                         u.rules, u.quests, u.summons, u.transforms, u.unnamed, u.violations);
                SourceStats const& src = index->sources.stats;
                LOG_INFO("playerbots",
                         "Quest index: {} item sources ({} corpse, {} skin, {} object) for {} items, {} source "
                         "spawns; {} references expanded, {} rings ended, {} missing references skipped, {} invalid "
                         "loot rows; built in {} ms, about {:.1f} MB (estimate)",
                         src.sources[0] + src.sources[1] + src.sources[2], src.sources[0], src.sources[1],
                         src.sources[2], src.items, src.spawns, src.references, src.rings, src.missing, src.invalid,
                         src.buildMs, static_cast<double>(index->sources.EstimateBytes()) / BYTES_PER_MB);
                if (u.violations)
                {
                    std::vector<std::string> const violations = index->uses.Violations();
                    constexpr std::size_t SHOWN = 5;
                    for (std::size_t i = 0; i < std::min(SHOWN, violations.size()); ++i)
                        LOG_ERROR("playerbots", "Quest index: use rule violation: {}", violations[i]);
                }
                questBook.Publish(std::move(index));
                questState.store(QuestIndexState::Ready, std::memory_order_release);
            }
            catch (std::exception const& e)
            {
                questState.store(QuestIndexState::Failed, std::memory_order_release);
                LOG_ERROR("playerbots", "Quest index: the build failed: {}", e.what());
            }
        });
    if (!posted)
    {
        questState.store(QuestIndexState::Failed, std::memory_order_release);
        LOG_ERROR("server.loading", "Quest index: the planner pool is not running; no quest index");
    }
    LoadQuestStats();
}

std::shared_ptr<QuestStatsView const> QuestStats() { return statsSlot.Current(); }

void QuestStatAccepted(Player* bot, uint32 questId)
{
    if (StatsOn(bot))
        TimedEvent(bot, [questId](uint32 guid, uint32 level) { statsBook.Accept(guid, questId, level, StatsNowMs()); });
}

void QuestStatRewarded(Player* bot, uint32 questId)
{
    if (StatsOn(bot))
        TimedEvent(bot, [questId](uint32 guid, uint32 level) { statsBook.Reward(guid, questId, level, StatsNowMs()); });
}

void QuestStatAbandoned(Player* bot, uint32 questId)
{
    if (StatsOn(bot))
        TimedEvent(bot,
                   [questId](uint32 guid, uint32 level) { statsBook.Abandon(guid, questId, level, StatsNowMs()); });
}

void QuestStatDropped(Player* bot, uint32 questId)
{
    if (StatsOn(bot))
        TimedEvent(bot, [questId](uint32 guid, uint32 level) { statsBook.Drop(guid, questId, level); });
}

void QuestStatDeath(Player* bot)
{
    if (!StatsOn(bot))
        return;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;
    // The death counts for the quest the bot's rpg status is doing (its map thread, like the status update)
    auto const* doing = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (!doing || !doing->questId)
        return;
    uint32 const questId = doing->questId;
    TimedEvent(bot, [questId](uint32 guid, uint32 level) { statsBook.Death(guid, questId, level); });
}

uint32 QuestStatsFlushNow()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::QuestStatsFlushNow");
    if (!statsOn.load(std::memory_order_acquire))
        return 0;
    statsFlushTimer = 0;
    std::vector<std::pair<uint64, QuestStatCell>> const rows = statsBook.TakeDirty(StatsNowMs());
    if (!rows.empty())
    {
        // Totals with explicit values: a repeated or late write leaves the same row
        PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
        for (auto const& [key, cell] : rows)
        {
            std::string const minutes = EncodeMinutes(cell);
            transaction->Append(Acore::StringFormat(
                "INSERT INTO playerbots_quest_stats (quest_id, band, accepted, completed, abandoned, deaths, minutes) "
                "VALUES ({}, {}, {}, {}, {}, {}, '{}') ON DUPLICATE KEY UPDATE accepted = {}, completed = {}, "
                "abandoned = {}, deaths = {}, minutes = '{}'",
                StatQuest(key), StatBand(key), cell.accepted, cell.completed, cell.abandoned, cell.deaths, minutes,
                cell.accepted, cell.completed, cell.abandoned, cell.deaths, minutes));
        }
        PlayerbotsDatabase.CommitTransaction(transaction);
        statsRowsWritten += rows.size();
    }
    statsSlot.Publish(statsBook.View());
    ++statsFlushes;
    statsLastRows = static_cast<uint32>(rows.size());
    statsLastFlush = std::chrono::steady_clock::now();
    return statsLastRows;
}

void UpdateQuestStats(uint32 diff)
{
    if (!statsOn.load(std::memory_order_relaxed))
        return;
    statsFlushTimer += diff;
    if (statsFlushTimer >= STATS_FLUSH_MS)
        QuestStatsFlushNow();
}

void QuestStatsShutdown()
{
    if (!statsOn.load(std::memory_order_acquire))
        return;
    uint32 const rows = QuestStatsFlushNow();
    statsOn.store(false, std::memory_order_release);
    LOG_INFO("playerbots", "Quest stats: {} rows written at shutdown, {} since startup", rows, statsRowsWritten);
}

std::shared_ptr<QuestIndex const> Quests() { return questBook.Current(); }

bool QuestsReady() { return questState.load(std::memory_order_acquire) == QuestIndexState::Ready; }

std::string QuestIndexStatus()
{
    QuestIndexState const state = questState.load(std::memory_order_acquire);
    std::shared_ptr<QuestIndex const> const quests = state == QuestIndexState::Ready ? Quests() : nullptr;
    if (!quests)
        return Acore::StringFormat("PLANNER quest index: {}", state == QuestIndexState::Building ? "building"
                                                              : state == QuestIndexState::Failed ? "failed"
                                                                                                 : "not started");
    QuestIndexStats const& s = quests->stats;
    QuestUseStats const& u = quests->uses.stats;
    ClassQuests::JoinCounts const classCounts = ClassQuests::CountsOf(quests->classRows);
    return Acore::StringFormat(
        "PLANNER quest index: ready, {} quests, {} places ({} givers, {} enders, {} unresolved), {} camps, {} "
        "objectives "
        "({} without area), {} quests without a giver, gathered in {} ms, built in {} ms on a worker, about {:.1f} MB "
        "(estimate); use rules: {} for {} quests ({} unit, {} dead, {} object, {} focus), {} source spells name "
        "nothing, {} violations; item sources: {} corpse, {} skin, {} object for {} items; class quests: {} ({} steps, "
        "{} refused on a "
        "side, {} not in the index)",
        s.quests, s.places, s.givers, s.enders, s.unresolved, s.camps, s.objectives, s.withoutArea, s.withoutGiver,
        s.gatherMs, s.buildMs, static_cast<double>(s.estimatedBytes) / BYTES_PER_MB, u.rules, u.quests, u.byKind[0],
        u.byKind[1], u.byKind[2], u.byKind[3], u.unnamed, u.violations, quests->sources.stats.sources[0],
        quests->sources.stats.sources[1], quests->sources.stats.sources[2], quests->sources.stats.items,
        classCounts.classQuests, classCounts.steps, classCounts.refused, classCounts.missing);
}

// WT36a: `planner capability`: the census of what the feed offers (CensusOf): per side its quests, the offered ones by
// the works they hold and per level band; then the invariant and <LogsDir>/questcapability.txt, one line per quest and
// side not offered: quest, min level, side, first refused work, objective index
bool HandleCapabilityCommand(ChatHandler* handler, char const* /*args*/)
{
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    CapabilityCensus const c = CensusOf(*quests);
    for (std::uint8_t side = 0; side < SIDES; ++side)
    {
        handler->PSendSysMessage("CAPABILITY {}", SideLine(c, side));
        handler->PSendSysMessage("CAPABILITY {} offered quests by work: {}", SIDE_NAMES[side],
                                 WorkCounts([&](std::size_t w) { return c.offeredWith[side][w]; }, false));
        for (std::size_t band = 0; band < CAPABILITY_BANDS; ++band)
            if (c.total[side][band])
                handler->PSendSysMessage(
                    "CAPABILITY {} levels {}-{}: {} quests, {} offered, not offered: {}", SIDE_NAMES[side],
                    band * LEVEL_BAND_LEVELS, band * LEVEL_BAND_LEVELS + LEVEL_BAND_LEVELS - 1, c.total[side][band],
                    c.offered[side][band], WorkCounts([&](std::size_t w) { return c.refused[side][w][band]; }, true));
    }
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "questcapability.txt";
    uint32 lines = 0;
    if (std::ofstream file(path, std::ios::trunc); file)
        for (QuestRecord const& quest : quests->quests)
            for (std::uint8_t side = 0; side < SIDES; ++side)
            {
                QuestExec::Step const refusal = QuestExec::FirstRefusal(quest, side);
                if (!OfSide(quest, side) || refusal.act != QuestExec::Act::Refused)
                    continue;
                file << quest.id << '\t' << quest.minLevel << '\t' << SIDE_NAMES[side] << '\t'
                     << RefusedWord(refusal.work) << '\t' << refusal.objective << '\n';
                ++lines;
            }
    handler->PSendSysMessage("CAPABILITY offered and unsupported: {}; {} quests by side not offered, list {}",
                             c.offeredUnsupported, lines, path);
    return true;
}

// WU38: `planner classquests [<quest>]`: the class-quest table joined with the quest index: per class quest and race
// path its chain, each step with its level, whether its giver is indexed and the executor's refusal per side; the
// counts. Without an argument it also writes <LogsDir>/classquests.txt, one line per table row as the index holds it:
// quest, in index, class mask, race mask, min level, prev, prevAny (signed, comma list, - none), refused Alliance,
// refused Horde, giver indexed (the data of tests/class-quest-selftest.cpp's comparison)
bool HandleClassQuestsCommand(ChatHandler* handler, char const* args)
{
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    uint32 const only = args && *args ? static_cast<uint32>(std::strtoul(args, nullptr, 10)) : 0;
    ClassQuests::Rows const& rows = quests->classRows;
    ClassQuests::JoinCounts const counts = ClassQuests::CountsOf(rows);
    handler->PSendSysMessage("CLASSQUESTS {} class quests, {} steps, {} refused on a side, {} not in the index",
                             counts.classQuests, counts.steps, counts.refused, counts.missing);
    auto const list = [](auto const& values)
    {
        std::string out;
        for (auto const value : values)
            out += (out.empty() ? "" : ",") + std::to_string(value);
        return out.empty() ? std::string("-") : out;
    };
    for (ClassQuests::Row const& row : rows)
    {
        if (!ClassQuests::IsClassQuest(row) || (only && row.quest() != only))
            continue;
        handler->PSendSysMessage("CLASSQUEST {} {} learns {}{}", row.quest(), ClassQuests::CauseWord(row.own->cause),
                                 row.own->learns, row.inIndex ? "" : ": not in the index");
        std::map<std::string, std::string> paths;  // the chain's text -> its races
        for (std::uint32_t race = 1; race <= 11; ++race)
        {
            if (race == 9 || (row.raceMask && !((row.raceMask >> (race - 1)) & 1u)))
                continue;
            std::string chain;
            for (ClassQuests::Row const* step : ClassQuests::ChainOf(rows, row, race))
            {
                chain += Acore::StringFormat("{}{} L{} giver {}", chain.empty() ? "" : " > ", step->quest(),
                                             step->minLevel, step->giverIndexed ? "indexed" : "not indexed");
                if (step->refused[0] || step->refused[1])
                    chain +=
                        Acore::StringFormat(" refused {}{}", step->refused[0] ? "A" : "", step->refused[1] ? "H" : "");
                if (!step->active.empty())
                    chain += " needs active " + list(step->active);
                if (!step->inIndex)
                    chain += " (not in the index)";
            }
            std::string& races = paths[chain];
            races += (races.empty() ? "" : ",") + std::to_string(race);
        }
        for (auto const& [chain, races] : paths)
            handler->PSendSysMessage("CLASSQUEST {} races {}: {}", row.quest(), races, chain);
    }
    if (only)
        return true;
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "classquests.txt";
    if (std::ofstream file(path, std::ios::trunc); file)
    {
        for (ClassQuests::Row const& row : rows)
        {
            QuestRecord const* record = quests->Find(row.quest());
            file << row.quest() << '\t' << (record ? 1 : 0) << '\t' << (record ? record->classMask : 0) << '\t'
                 << (record ? record->raceMask : 0) << '\t' << (record ? record->minLevel : 0) << '\t'
                 << (record ? record->prev : 0) << '\t' << (record ? list(record->prevAny) : std::string("-")) << '\t'
                 << row.refused[0] << '\t' << row.refused[1] << '\t' << row.giverIndexed << '\n';
        }
        handler->PSendSysMessage("CLASSQUESTS {} rows written to {}", rows.size(), path);
    }
    return true;
}

bool HandleSourcesCommand(ChatHandler* handler, char const* args)
{
    uint32 item = 0;
    if (!(std::istringstream(args ? args : "") >> item) || !item)
    {
        handler->SendErrorMessage("Usage: playerbots planner sources <item id>");
        return false;
    }
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    std::vector<ItemSource> const sources = quests->SourcesOf(item);
    std::array<std::size_t, SOURCE_KINDS> byKind{};
    for (ItemSource const& source : sources)
        ++byKind[static_cast<std::size_t>(source.kind)];
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    handler->PSendSysMessage("SOURCES {} \"{}\": {} sources ({} corpse, {} skin, {} object)", item,
                             proto ? proto->Name1 : "?", sources.size(), byKind[0], byKind[1], byKind[2]);
    constexpr std::size_t SHOWN = 40;  // world drops have thousands
    for (std::size_t i = 0; i < std::min(SHOWN, sources.size()); ++i)
    {
        ItemSource const& source = sources[i];
        GameObjectTemplate const* object =
            source.kind == SourceKind::Object ? sObjectMgr->GetGameObjectTemplate(source.entry) : nullptr;
        CreatureTemplate const* creature =
            source.kind == SourceKind::Object ? nullptr : sObjectMgr->GetCreatureTemplate(source.entry);
        handler->PSendSysMessage("  {} {} \"{}\": {:.2f}%, count {}-{}, {} spawns{}", SourceKindName(source.kind),
                                 source.entry,
                                 object     ? object->name
                                 : creature ? creature->Name
                                            : "?",
                                 source.chance * 100.0f, source.minCount, source.maxCount, source.spawns,
                                 source.questOnly ? ", quest only" : "");
    }
    if (sources.size() > SHOWN)
        handler->PSendSysMessage("  ... {} more", sources.size() - SHOWN);
    return true;
}

// `playerbots planner places poionly [all]`: the objectives whose only place is a POI polygon (what a baked floor would
// serve, WT33a). Prints the count and a sample; the full list goes to <LogsDir>/poionly.txt, one row per objective
bool HandlePlacesCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string what;
    std::string how;
    in >> what >> how;
    if (what != "poionly")
    {
        handler->SendErrorMessage("Usage: playerbots planner places poionly [all]");
        return false;
    }
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    std::vector<std::string> rows;
    for (QuestRecord const& quest : quests->quests)
        for (std::size_t i = 0; i < quest.objectives.size(); ++i)
        {
            QuestObjective const& objective = quest.objectives[i];
            // The objectives rule 5 serves: a POI polygon, floored (poi) or not (none, WT33b)
            bool const poiOnly =
                objective.placeFrom == PlaceFrom::Poi || (objective.placeFrom == PlaceFrom::None && objective.polygon);
            if (objective.kind == ObjectiveKind::Turnin || !poiOnly)
                continue;
            uint32 points = 0;
            for (AreaPoint const& area : objective.areas)
                points += area.points;
            rows.push_back(Acore::StringFormat(
                "quest {} objective {} {} {} \"{}\" x{} poi_points {} areas {} map {} zone {}", quest.id, i,
                ObjectiveName(objective.kind), objective.target, TargetName(objective), objective.count, points,
                objective.areas.size(), objective.areas.empty() ? 0 : objective.areas.front().mapId, quest.zoneOrSort));
        }
    handler->PSendSysMessage("PLACES poionly: {} objectives have only a POI polygon", rows.size());
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "poionly.txt";
    if (std::ofstream file(path, std::ios::trunc); file)
    {
        for (std::string const& row : rows)
            file << row << '\n';
        handler->PSendSysMessage("  full list: {}", path);
    }
    std::size_t const shown = how == "all" ? rows.size() : std::min<std::size_t>(rows.size(), 15);
    for (std::size_t i = 0; i < shown; ++i)
        handler->PSendSysMessage("  {}", rows[i]);
    if (shown < rows.size())
        handler->PSendSysMessage("  ... {} more (`places poionly all` prints every row)", rows.size() - shown);
    return true;
}

// WT35g: `questuse facts` lists the four facts of every quest (QuestUseTable::FactsOf: summon, transform, made,
// credit) and each kill objective a use brings forth (QuestObjective::brings), one line each in
// <LogsDir>/questfacts.txt: quest, min level, race mask, objective, word, entry, via, detail
static bool QuestUseFacts(ChatHandler* handler, QuestIndex const& quests)
{
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "questfacts.txt";
    std::ofstream file(path, std::ios::trunc);
    std::array<uint32, static_cast<std::size_t>(FactKind::Count)> counts{};
    uint32 chains = 0;
    for (QuestRecord const& quest : quests.quests)
    {
        auto const line =
            [&](uint32 objective, std::string_view word, uint32 entry, uint32 via, std::string const& detail)
        {
            file << quest.id << '\t' << quest.minLevel << '\t' << quest.raceMask << '\t' << objective << '\t' << word
                 << '\t' << entry << '\t' << via << '\t' << detail << '\n';
        };
        for (UseFact const& fact : quests.uses.FactsOf(quest.id))
        {
            ++counts[static_cast<std::size_t>(fact.kind)];
            std::string detail = fact.viaObject ? "object" : (fact.via ? "item" : "kill");
            for (auto const& [item, count] : fact.reagents)
                detail += Acore::StringFormat(" {}x{}", item, count);
            line(fact.objective, FACT_KIND_NAMES[static_cast<std::size_t>(fact.kind)], fact.entry, fact.via, detail);
        }
        for (std::size_t i = 0; i < quest.objectives.size(); ++i)
            if (quest.objectives[i].brings != UseBrings::Credit)
            {
                ++chains;
                QuestObjective const& objective = quest.objectives[i];
                line(static_cast<uint32>(i), objective.brings == UseBrings::Summon ? "chain:summon" : "chain:transform",
                     objective.target, 0, Acore::StringFormat("{} areas", objective.areas.size()));
            }
    }
    handler->PSendSysMessage(
        "QUESTUSE facts: {} summon, {} transform, {} made, {} credit; {} kill objectives a use brings forth; list {}",
        counts[0], counts[1], counts[2], counts[3], chains, path);
    return true;
}

// WT35b: `questuse all` lists every quest the executor can use an item or a spell for: an objective (or the quest's
// event) with a use rule and a place, one id per line in <LogsDir>/questuse.txt (the failure record's cleanup reads it)
static bool QuestUseAll(ChatHandler* handler, QuestIndex const& quests)
{
    std::vector<uint32> ids;
    for (QuestRecord const& quest : quests.quests)
    {
        auto const placed = [](QuestObjective const& objective)
        { return objective.use != UseHow::None && !objective.areas.empty(); };
        if (placed(quest.eventUse) || std::any_of(quest.objectives.begin(), quest.objectives.end(), placed))
            ids.push_back(quest.id);
    }
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    std::string const path = dir + "questuse.txt";
    if (std::ofstream file(path, std::ios::trunc); file)
        for (uint32 const id : ids)
            file << id << '\n';
    handler->PSendSysMessage("QUESTUSE all: {} quests have a use rule and a place; list {}", ids.size(), path);
    return true;
}

bool HandleQuestUseCommand(ChatHandler* handler, char const* args)
{
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (args && std::string_view(args) == "all")
    {
        if (!quests)
        {
            handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
            return false;
        }
        return QuestUseAll(handler, *quests);
    }
    if (args && std::string_view(args) == "facts")
    {
        if (!quests)
        {
            handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
            return false;
        }
        return QuestUseFacts(handler, *quests);
    }
    uint32 id = 0;
    if (!(std::istringstream(args ? args : "") >> id) || !id)
    {
        handler->SendErrorMessage("Usage: playerbots planner questuse <quest id>|all|facts");
        return false;
    }
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    QuestRecord const* quest = quests->Find(id);
    if (!quest)
    {
        handler->SendErrorMessage("PLANNER: no quest {}", id);
        return false;
    }
    auto const [first, last] = quests->uses.RulesFor(id);
    std::vector<UseItem const*> const items = quests->uses.ItemsOf(id);
    Quest const* proto = sObjectMgr->GetQuestTemplate(id);
    handler->PSendSysMessage("QUESTUSE {} \"{}\": {} rules, {} source items with an on-use spell", id,
                             proto ? proto->GetTitle() : "?", std::distance(first, last), items.size());
    for (auto it = first; it != last; ++it)
    {
        QuestUseRule const& rule = *it;
        bool const objects = rule.kind == UseKind::Object || rule.kind == UseKind::Focus;
        std::string targets;
        for (uint32 const entry : rule.entries)
        {
            GameObjectTemplate const* object = objects ? sObjectMgr->GetGameObjectTemplate(entry) : nullptr;
            CreatureTemplate const* creature = objects ? nullptr : sObjectMgr->GetCreatureTemplate(entry);
            targets += Acore::StringFormat("{}{} \"{}\"", targets.empty() ? "" : ", ", entry,
                                           object     ? object->name
                                           : creature ? creature->Name
                                                      : "?");
        }
        std::string const what = rule.spellId == 0 ? std::string("use the object")
                                 : rule.itemId     ? Acore::StringFormat("item {} spell {}", rule.itemId, rule.spellId)
                                                   : Acore::StringFormat("class spell {}", rule.spellId);
        std::string const goal =
            rule.objective < quest->objectives.size()
                ? Acore::StringFormat("{} {}", ObjectiveName(quest->objectives[rule.objective].kind),
                                      quest->objectives[rule.objective].target)
                : std::string("completes the quest");
        handler->PSendSysMessage("  {}: {} on {}; range {:.0f} yd{}{}; -> {}", UseKindName(rule.kind), what, targets,
                                 rule.rangeYards, rule.mustBeDead ? ", target lies dead" : "",
                                 rule.standAtFocus ? Acore::StringFormat(", stand at focus {}", rule.focusId) : "",
                                 goal);
    }
    for (UseItem const* item : items)
    {
        if (std::any_of(first, last, [item](QuestUseRule const& rule)
                        { return rule.itemId == item->itemId && rule.spellId == item->spellId; }))
            continue;
        UseSpell const* spell = quests->uses.Spell(item->spellId);
        handler->PSendSysMessage("  no rule: item {} spell {} names no target (targets {:#x}, focus {})", item->itemId,
                                 item->spellId, spell ? spell->targets : 0, spell ? spell->focusId : 0);
    }
    return true;
}

bool HandleQuestCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string word;
    in >> word;
    if (word == "stats")
        return HandleQuestStats(handler);
    if (word == "flush")
    {
        if (!statsOn.load(std::memory_order_acquire))
        {
            handler->SendErrorMessage("PLANNER: the quest statistics are not loaded");
            return false;
        }
        uint32 const rows = QuestStatsFlushNow();
        handler->PSendSysMessage("QUEST stats flush: {} rows written (asynchronous)", rows);
        return true;
    }
    uint32 id = 0;
    if (!(std::istringstream(word) >> id) || !id)
    {
        handler->SendErrorMessage("Usage: playerbots planner quest <id> | stats | flush");
        return false;
    }
    std::shared_ptr<QuestIndex const> const quests = QuestsReady() ? Quests() : nullptr;
    if (!quests)
    {
        handler->SendErrorMessage("PLANNER: the quest index is not ready yet (see `playerbots planner indexes`)");
        return false;
    }
    QuestRecord const* quest = quests->Find(id);
    if (!quest)
    {
        handler->SendErrorMessage("PLANNER: no quest {}", id);
        return false;
    }
    std::shared_ptr<PlannerIndexes const> const indexes = IndexesReady() ? Indexes() : nullptr;
    Quest const* proto = sObjectMgr->GetQuestTemplate(id);
    std::string const zone = quest->zoneOrSort > 0 ? AreaName(handler, static_cast<uint32>(quest->zoneOrSort))
                                                   : Acore::StringFormat("quest sort {}", -quest->zoneOrSort);
    handler->PSendSysMessage(
        "QUEST {} \"{}\": level {}, min {}, max {}, type {}, {} suggested, flags {}, races {:#x}, classes {:#x}, {}, "
        "xp {}, money {}",
        id, proto ? proto->GetTitle() : "?", quest->level, quest->minLevel, quest->maxLevel, quest->type,
        quest->suggestedPlayers, FlagText(*quest), quest->raceMask, quest->classMask, zone, quest->xpBase,
        quest->copperReward);
    handler->PSendSysMessage(
        "  chain: prev {}, next {}, next in chain {}, exclusive group {}, breadcrumb for {}; "
        "unlocks {}",
        quest->prev, quest->next, quest->nextInChain, quest->exclusiveGroup, quest->breadcrumbFor,
        ListText(quests->Unlocks(id)));
    SendPlaces(handler, *quests, indexes.get(), quest->givers, "giver");
    SendPlaces(handler, *quests, indexes.get(), quest->enders, "ender");
    for (QuestObjective const& objective : quest->objectives)
    {
        std::string areas;
        for (AreaPoint const& area : objective.areas)
            areas += Acore::StringFormat("{}({}, {:.0f}, {:.0f}) {:.0f} yd, {} points", areas.empty() ? "" : "; ",
                                         area.mapId, area.x, area.y, area.radius, area.points);
        handler->PSendSysMessage("  {} {} {} x{}: {} areas{}{}", ObjectiveName(objective.kind), objective.target,
                                 TargetName(objective), objective.count, objective.areas.size(),
                                 areas.empty() ? "" : ": ", areas);
    }
    SendQuestStats(handler, id);
    return true;
}

// The class-quest table's spells the bot's class trainer teaches (upkeep WU20a): their quests are ordinary ones
static std::vector<uint32> ClassTrainerTaught(UpkeepIndex const& upkeep, uint8 cls)
{
    std::vector<uint32> taught;
    TrainerList const* list = upkeep.ClassList(cls);
    if (!list)
        return taught;
    for (ClassQuests::Entry const& entry : ClassQuests::TABLE)
        if (entry.learns && (list->Find(entry.learns) != TrainerList::NOT_FOUND ||
                             list->Teacher(entry.learns) != TrainerList::NOT_FOUND))
            taught.push_back(entry.learns);
    std::sort(taught.begin(), taught.end());
    taught.erase(std::unique(taught.begin(), taught.end()), taught.end());
    return taught;
}

// The automatic class-quest completion (upkeep WU21a, ClassQuestCompletion.h)
namespace ClassQuestCompletion
{
namespace
{
std::mutex waitLock;
std::unordered_map<uint32, ClassQuests::When> waits;  // guid counter -> the widest When asked (Creation first)
std::atomic<uint32> waiting{0};

bool Ready() { return QuestsReady() && UpkeepIndexReady(); }

void Wait(Player* bot, ClassQuests::When when)
{
    std::lock_guard<std::mutex> const guard(waitLock);
    auto const [it, added] = waits.emplace(bot->GetGUID().GetCounter(), when);
    if (added)
        waiting.fetch_add(1, std::memory_order_release);
    else if (when < it->second)
        it->second = when;
    LOG_DEBUG("playerbots", "[ClassQuest] {} waits for the quest index (when {})", bot->GetName(),
              static_cast<uint32>(it->second));
}

ClassQuests::BotFacts FactsOf(Player* bot, bool governed, ClassQuests::When when)
{
    ClassQuests::BotFacts facts;
    facts.cls = bot->getClass();
    facts.race = bot->getRace();
    facts.level = bot->GetLevel();
    facts.governed = governed;
    for (uint32 const quest : bot->getRewardedQuests())
        facts.rewarded.push_back(quest);
    std::sort(facts.rewarded.begin(), facts.rewarded.end());
    if (governed)
        facts.givenUp = QuestFailRecord::GivenUpQuests(bot->GetGUID().GetCounter(), ClassQuests::GIVE_UP_LIMIT);
    // A new character did its lower class quests: a spell the factory taught beside them (InitSpecialSpells' Death
    // Gate) settles none of them
    if (when != ClassQuests::When::Creation)
        facts.knownSpells = KnownSpells(bot);
    return facts;
}

// One due quest: the chain's steps leave the log and are rewarded with the class quest, its reward spell is cast; no
// XP, gold or items (Player::RewardQuest is not called)
bool CompleteOne(Player* bot, ClassQuests::Rows const& rows, ClassQuests::BotFacts const& facts,
                 ClassQuests::Due const& due, bool creation, char const* source)
{
    ClassQuests::Row const* const row = ClassQuests::Find(rows, due.quest);
    if (!row || !ClassQuests::IsClassQuest(*row) || bot->IsQuestRewarded(due.quest) ||
        (!creation && due.spell && bot->HasSpell(due.spell)))
        return false;
    std::vector<uint32> const steps = ClassQuests::CompletionSteps(rows, *row, facts);
    for (uint32 const step : steps)
    {
        uint16 const slot = bot->FindQuestSlot(step);
        if (slot < MAX_QUEST_LOG_SIZE)
        {
            bot->SetQuestSlot(slot, 0);
            bot->TakeQuestSourceItem(step, false);
        }
        if (bot->GetQuestStatus(step) != QUEST_STATUS_NONE)
            bot->RemoveActiveQuest(step, false);
        bot->SetRewardedQuest(step);
    }
    if (Quest const* quest = sObjectMgr->GetQuestTemplate(due.quest))
        bot->learnQuestRewardedSpells(quest);
    // The reward spell's cast teaches its spells; a cast that does not (its result is logged) leaves the quest's
    // defined effect to direct learns: every spell the reward spell teaches (Path of Defense: three), then the due one
    int32 cast = -1;  // not cast
    if (due.spell && !bot->HasSpell(due.spell) && row->own->rewardSpell)
        cast = static_cast<int32>(bot->CastSpell(bot, row->own->rewardSpell, true));
    bool const learned = due.spell && !bot->HasSpell(due.spell);
    if (learned)
    {
        if (SpellInfo const* const reward = sSpellMgr->GetSpellInfo(row->own->rewardSpell))
        {
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                uint32 const taught = reward->Effects[i].TriggerSpell;
                if (reward->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL && taught && !bot->HasSpell(taught))
                    bot->learnSpell(taught);
            }
        }
        if (!bot->HasSpell(due.spell))
            bot->learnSpell(due.spell);
    }
    Upkeep::Note(bot, UpkeepKind::ClassQuest, source, due.quest);
    LOG_INFO("playerbots",
             "[ClassQuest] {} completes class quest {} ({}): {} quests rewarded, spell {} {} (cast {} result {}, "
             "alive {}, in world {}, learned directly {})",
             bot->GetName(), due.quest, source, steps.size(), due.spell,
             due.spell && bot->HasSpell(due.spell) ? "known" : "not known", row->own->rewardSpell, cast, bot->IsAlive(),
             bot->IsInWorld(), learned);
    return true;
}

void Complete(Player* bot, std::vector<ClassQuests::Due> const& due, ClassQuests::Rows const& rows,
              ClassQuests::BotFacts const& facts, ClassQuests::When when)
{
    bool const creation = when == ClassQuests::When::Creation;
    for (ClassQuests::Due const& one : due)
        CompleteOne(bot, rows, facts, one, creation,
                    one.failed ? Upkeep::SOURCE_CLASS_QUEST_FAILED : (creation ? SOURCE_CREATION : SOURCE_LEVELUP));
}

void Run(Player* bot, ClassQuests::When when)
{
    std::shared_ptr<QuestIndex const> const quests = Quests();
    std::shared_ptr<UpkeepIndex const> const upkeep = CurrentUpkeepIndex();
    bool const governed = Governed(bot);
    if (!quests || !upkeep || (when == ClassQuests::When::Check && !governed))
        return;
    ClassQuests::BotFacts const facts = FactsOf(bot, governed, when);
    std::vector<uint32> const taught = ClassTrainerTaught(*upkeep, bot->getClass());
    auto const teaches = [&taught](uint32 spell) { return std::binary_search(taught.begin(), taught.end(), spell); };
    // The map thread asks no way: a failed way is the planner's verdict (CompleteFailed)
    auto const route = [](uint32) { return ClassQuests::Route::Unknown; };
    Complete(bot, ClassQuests::DueQuests(quests->classRows, facts, when, teaches, route), quests->classRows, facts,
             when);
}
}  // namespace

bool Governed(Player* bot) { return GoalApply::Governed(BotKind::KeepsPossessions(bot), bot->GetGUID().GetCounter()); }

void Request(Player* bot, ClassQuests::When when)
{
    if (!bot || (when == ClassQuests::When::Check && !Governed(bot)))
        return;
    if (!Ready())
        Wait(bot, when);
    else
        Run(bot, when);
}

void CompleteFailed(Player* bot, std::vector<ClassQuests::Due> const& due)
{
    std::shared_ptr<QuestIndex const> const quests = Ready() ? Quests() : nullptr;
    if (!bot || due.empty() || !quests || !Governed(bot))
        return;
    std::vector<ClassQuests::Due> failed;
    for (ClassQuests::Due const& one : due)
        if (one.failed)
            failed.push_back(one);
    Complete(bot, failed, quests->classRows, FactsOf(bot, true, ClassQuests::When::Check), ClassQuests::When::Check);
}

void Drain(Player* bot)
{
    if (!waiting.load(std::memory_order_acquire) || !Ready())
        return;
    ClassQuests::When when;
    {
        std::lock_guard<std::mutex> const guard(waitLock);
        auto const it = waits.find(bot->GetGUID().GetCounter());
        if (it == waits.end())
            return;
        when = it->second;
        waits.erase(it);
        waiting.fetch_sub(1, std::memory_order_release);
    }
    Run(bot, when);
}

void Forget(Player* bot)
{
    if (!bot || !waiting.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> const guard(waitLock);
    if (waits.erase(bot->GetGUID().GetCounter()))
        waiting.fetch_sub(1, std::memory_order_release);
}

std::vector<uint32> KnownSpells(Player* bot)
{
    std::vector<uint32> known;
    for (ClassQuests::Entry const& entry : ClassQuests::TABLE)
    {
        for (uint32 const spell : {entry.learns, entry.rewardSpell})
            if (spell && bot->HasSpell(spell))
                known.push_back(spell);
    }
    std::sort(known.begin(), known.end());
    known.erase(std::unique(known.begin(), known.end()), known.end());
    return known;
}
}  // namespace ClassQuestCompletion

Compute GoalsCompute(std::vector<uint32> const& protectedQuests, std::vector<QuestProgress> const& progress,
                     Goals::Situation const& situation, UpkeepNeeds::NeedRates const& rates,
                     SkillPath::PathInputs pathInputs)
{
    CircuitData data = CircuitDataNow();
    return [data = std::move(data), route = RouteCostInputsNow(), protectedQuests, progress, situation, rates,
            pathInputs = std::move(pathInputs), pathData = PathDataNow(),
            upkeep = CurrentUpkeepIndex()](Snapshot const& snapshot) -> std::optional<Payload>
    {
        // A failed job answers without a plan: its bot falls back at once (fallback_failed) instead of waiting out
        // the pool's keep time for a result that never comes
        if (!data.quests)
            return Payload{GoalsAnswer{}};
        auto const started = std::chrono::steady_clock::now();
        std::shared_ptr<Goals::GoalPlan const> plan;
        std::shared_ptr<SkillPath::Path const> path;
        std::shared_ptr<Materials::Rates const> gatherRates;
        try
        {
            // The needs list on the worker: its reserve and shortfall feed the need factor (upkeep WU16, §3.10); the
            // skill path between the needs without it and with its recipes and ranks (professions WR11)
            Goals::Situation filled = situation;
            if (upkeep && filled.governed)
                filled.trainerTaught = ClassTrainerTaught(*upkeep, snapshot.cls);
            // The way's price from the bot's route snapshot, this job's own (planning WN1a)
            CircuitContext context = data.Context(JobTravel(route, snapshot.route));
            context.protectedQuests = protectedQuests;
            context.progress = progress;
            if (upkeep)
            {
                UpkeepNeeds::NeedList needs;
                path = NeedsWithPath(snapshot, *upkeep, rates, pathInputs, pathData, needs);
                NeedBridge::Fill(filled, NeedBridge::InputsOf(needs));
                // The gather circuit at the path's prices (professions WR13b), against the zone's other gatherers
                uint32 competing = 0;
                if (context.crowding)
                    if (auto const zone = context.crowding->byZone.find(snapshot.zone);
                        zone != context.crowding->byZone.end())
                        competing = zone->second.gatherers;
                if (competing && snapshot.currentGoal.kind == static_cast<uint32>(Goals::GoalKind::Gather))
                    --competing;
                gatherRates = std::make_shared<Materials::Rates const>(
                    SkillPathBridge::RatesOf(snapshot, SkillPathBridge::ReserveBeforeProfession(needs)));
                FillGatherCircuit(snapshot, path.get(), *gatherRates, pathData, competing, filled);
                // A fishing trip in the zone at the same prices (professions WR16b)
                FillFishWaters(snapshot, path.get(), *gatherRates, filled);
            }
            plan = std::make_shared<Goals::GoalPlan const>(Goals::PlanGoals(snapshot, std::move(context), filled));
        }
        catch (...)
        {
            return Payload{GoalsAnswer{}};
        }
        if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "planner goals compute"))
            row->Record(static_cast<uint64>(std::max<long long>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
                    .count(),
                0)));
        return Payload{GoalsAnswer{std::move(plan), std::move(path), std::move(gatherRates)}};
    };
}

bool RunCircuitHarness(ChatHandler* handler)
{
    if (!IndexesReady() || !QuestsReady())
    {
        handler->SendErrorMessage("PLANNER circuit selftest: not ready (see `playerbots planner indexes`)");
        return false;
    }
    auto const started = std::chrono::steady_clock::now();
    CircuitData const data = CircuitDataNow();
    CircuitContext const context = data.Context();
    QuestIndex const& quests = *data.quests;
    std::vector<CircuitHarness::Check> checks;
    std::vector<std::string> plans;
    for (CircuitPlace const& place : CIRCUIT_PLACES)
    {
        Place start;
        std::string text;
        bool const found = CircuitStart(quests, place, start, text);
        checks.push_back({found, std::string(place.name) + ": " + text});
        if (!found)
            continue;

        // An empty log at the hub after a levelled bot's history: most fitting quests taken, only fitting ones, the log
        // within its limit, one turn-in batch
        CircuitHarness::Scenario scenario{place.name, CircuitBot(place, start), context, {}};
        scenario.bot.rewardedQuests = GreyHistory(quests, place.level, data.greyDiff);
        CircuitHarness::Report const report = CircuitHarness::RunScenario(scenario, CIRCUIT_TIMING_RUNS);
        AddChecks(checks, report.checks);
        AddPlanLines(plans, scenario.name, report);
        if (&place == &CIRCUIT_PLACES.front())
        {
            AddChecks(checks, CircuitHarness::GuardChecks(scenario));
        }
        std::vector<uint32> const hub = HubQuests(quests, start);

        // The next circuit with the carried quests done in the log (review 2, finding A): they stay out of the loop,
        // which visits the same ender camps
        if (!report.plan.carried.empty())
        {
            CircuitHarness::Scenario carrying{scenario.name + "-carrying", scenario.bot, context, {}};
            for (uint32 const id : report.plan.carried)
                carrying.bot.quests.push_back(LogSlot(id, Circuit::SLOT_COMPLETE));
            carrying.expect.minTake = 0;
            carrying.expect.carried = report.plan.carried;
            carrying.expect.checkEnderCamps = true;
            carrying.expect.enderCamps = report.plan.enderCamps;
            CircuitHarness::Report const carryingReport = CircuitHarness::RunScenario(carrying, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, carryingReport.checks);
            AddPlanLines(plans, carrying.name, carryingReport);
        }

        // A full log of quests from another map and a grey one: the grey one goes, far ones make room for every quest
        // the empty log took to work here (a delivery only when worth more than the far quests it pushes out, so it
        // may wait for log space), a protected far one stays
        std::vector<uint32> const fillers = FarQuests(quests, place, hub, data.greyDiff, false, QUEST_LOG_SLOTS - 1);
        std::vector<uint32> const grey = FarQuests(quests, place, hub, data.greyDiff, true, 1);
        bool const fixture = fillers.size() == QUEST_LOG_SLOTS - 1 && grey.size() == 1;
        checks.push_back({fixture, Acore::StringFormat("{}-full-log: {} far quests and {} grey one fill the log",
                                                       scenario.name, fillers.size(), grey.size())});
        if (fixture)
        {
            CircuitHarness::Scenario full{scenario.name + "-full-log", scenario.bot, context, {}};
            std::erase(full.bot.rewardedQuests, grey.front());
            for (uint32 const id : fillers)
                full.bot.quests.push_back(LogSlot(id, 0));
            full.bot.quests.push_back(LogSlot(grey.front(), 0));
            full.context.protectedQuests = {fillers.front()};
            std::vector<uint32> worked;  // the empty log's takes worked here
            for (CircuitTake const& take : report.plan.take)
                if (!take.carried)
                    worked.push_back(take.quest);
            std::sort(worked.begin(), worked.end());
            full.expect.minTake = worked.size();
            full.expect.checkAbandon = true;
            full.expect.abandonFrom.assign(fillers.begin() + 1, fillers.end());
            full.expect.abandonFrom.push_back(grey.front());
            full.expect.abandonMust = grey;
            full.expect.keep = {fillers.front()};
            CircuitHarness::Report const fullReport = CircuitHarness::RunScenario(full, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, fullReport.checks);
            std::vector<uint32> const was = CircuitHarness::TakenIds(report.plan);
            std::vector<uint32> const fullTaken = CircuitHarness::TakenIds(fullReport.plan);
            bool const keepsWorked = std::includes(fullTaken.begin(), fullTaken.end(), worked.begin(), worked.end());
            bool const onlyEmpty = std::includes(was.begin(), was.end(), fullTaken.begin(), fullTaken.end());
            checks.push_back({keepsWorked && onlyEmpty,
                              Acore::StringFormat("{}-full-log: takes {}: each the empty log works here, none it left",
                                                  scenario.name, CircuitHarness::IdList(fullTaken))});
            AddPlanLines(plans, full.name, fullReport);

            // The log full of those quests done (review 3, finding 1): complete quests are never abandoned and none is
            // turned in here, so the hub's quests wait for log space that only delivering frees. No slot frees at a
            // chain or camp, so the bot moves on only to deliver (reviews 4 and 5); with every ender on another map no
            // delivery is in reach, so it finds nowhere. The generic checks hold it to that and follow its moves
            CircuitHarness::Scenario done{scenario.name + "-full-log-done", scenario.bot, context, {}};
            for (std::size_t i = 0; i + Circuit::LOG_RESERVE < QUEST_LOG_SLOTS; ++i)
                done.bot.quests.push_back(LogSlot(fillers[i], Circuit::SLOT_COMPLETE));
            done.expect.exactTake = true;
            done.expect.minTake = 0;
            CircuitHarness::ExactAbandon(done.expect, {});
            CircuitHarness::Report const doneReport = CircuitHarness::RunScenario(done, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, doneReport.checks);
            AddPlanLines(plans, done.name, doneReport);
        }

        // The same hub with every quest its givers offer rewarded: the bot moves on
        CircuitHarness::Scenario dry{scenario.name + "-dry", scenario.bot, context, {}};
        std::vector<uint32> done;
        std::set_union(scenario.bot.rewardedQuests.begin(), scenario.bot.rewardedQuests.end(), hub.begin(), hub.end(),
                       std::back_inserter(done));
        dry.bot.rewardedQuests = std::move(done);
        dry.expect.minTake = 0;
        dry.expect.moveInBracket = true;
        CircuitHarness::Report const dryReport = CircuitHarness::RunScenario(dry, CIRCUIT_TIMING_RUNS);
        AddChecks(checks, dryReport.checks);
        AddPlanLines(plans, dry.name, dryReport);

        // The dry hub with the log full of the far quests, none protected (review 5, finding 1): any hub that wants a
        // slot gives one of them up, so the bot still moves on, and the generic checks see it take there
        if (fixture)
        {
            CircuitHarness::Scenario dryFull{scenario.name + "-dry-full-log", dry.bot, context, {}};
            for (uint32 const id : fillers)
            {
                std::erase(dryFull.bot.rewardedQuests, id);
                dryFull.bot.quests.push_back(LogSlot(id, 0));
            }
            dryFull.expect.minTake = 0;
            dryFull.expect.moveInBracket = true;
            CircuitHarness::ExactAbandon(dryFull.expect, {});
            CircuitHarness::Report const dryFullReport = CircuitHarness::RunScenario(dryFull, CIRCUIT_TIMING_RUNS);
            AddChecks(checks, dryFullReport.checks);
            AddPlanLines(plans, dryFull.name, dryFullReport);
        }
    }

    std::size_t failed = 0;
    for (CircuitHarness::Check const& check : checks)
    {
        failed += check.pass ? 0 : 1;
        handler->PSendSysMessage("{} {}", check.pass ? "PASS" : "FAIL", check.text);
    }
    for (std::string const& line : plans)
        handler->PSendSysMessage("{}", line);
    std::string const summary =
        Acore::StringFormat("PLANNER circuit selftest: {}, {} checks, {} failed, in {} ms (grey difference {})",
                            failed ? "FAIL" : "PASS", checks.size(), failed, MsSince(started), data.greyDiff);
    handler->PSendSysMessage("{}", summary);
    LOG_INFO("playerbots", "{}", summary);
    return !failed;
}

bool HandleCircuitCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string word;
    in >> word;
    if (word.empty())
    {
        handler->SendErrorMessage("Usage: playerbots planner circuit selftest | <player name>");
        return false;
    }
    // The summary line carries the verdict; false here would only add the usage text
    if (word == "selftest")
    {
        RunCircuitHarness(handler);
        return true;
    }
    if (!IndexesReady() || !QuestsReady())
    {
        handler->SendErrorMessage("PLANNER circuit: not ready (see `playerbots planner indexes`)");
        return false;
    }
    std::string name = word;
    Player* player = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    if (!player || !player->IsInWorld())
    {
        handler->SendErrorMessage("PLANNER circuit: no player {} online", word);
        return false;
    }
    // Read only, on the world thread between map updates; the log quests' elapsed minutes come with WT16
    CircuitData const data = CircuitDataNow();
    Snapshot const snapshot = FillSnapshot(player, 0);
    CircuitPlan const plan = PlanCircuit(snapshot, data.Context());
    // The world log's decision rows of this plan (kind circuit); WT16 emits the same rows where it applies plans
    if (WorldLog::Active())
        for (CircuitLog::Row const& row : CircuitLog::Rows(plan))
            WorldLog::Decision(player, WorldLogFormat::DecisionKind::Circuit, row.choice, row.reason, row.value);
    handler->PSendSysMessage("CIRCUIT {}: level {}, map {} ({:.0f}, {:.0f}), {} log quests, hub camp {}",
                             player->GetName(), snapshot.level, snapshot.position.map, snapshot.position.x,
                             snapshot.position.y, snapshot.quests.size(),
                             plan.homeCamp == NO_CAMP ? std::string("none") : std::to_string(plan.homeCamp));
    float const baseline = plan.ok ? BaselineYards(plan, *data.quests) : 0.0f;
    for (std::string const& line : CircuitHarness::PlanLines(plan, baseline))
        handler->PSendSysMessage("  {}", line);
    return true;
}

// WT14-2: the goal model on real data. The circuit, hubs, quests and services are the real indexes at the circuit
// harness's places; the grind spots and the herb node are synthetic (the world has no spot or node index yet, WT16)
namespace
{
constexpr float GOALS_SPOT_XP = 60.0f;  // synthetic grind spots: XP and copper per kill
constexpr float GOALS_SPOT_COPPER = 30.0f;
constexpr float GOALS_NODE_COPPER = 400.0f;  // synthetic herb node: what its yield sells for
constexpr uint32 GOALS_SPOT_ID = 9001;
constexpr uint32 GOALS_SPOT_GREY_ID = 9002;
constexpr uint32 GOALS_NODE_ID = 9101;
constexpr uint32 GOALS_ZONE_SPAN = 5;  // the synthetic zone bracket around the bot's level
constexpr uint32 GOALS_FLUSH = 1000000;

Goals::Situation GoalsSituation(Snapshot const& bot, Place const& start)
{
    Goals::Situation situation = GoalHarness::Base(bot.level);
    situation.here.zoneLow = bot.level > GOALS_ZONE_SPAN ? bot.level - GOALS_ZONE_SPAN : 1;
    situation.here.zoneHigh = bot.level + GOALS_ZONE_SPAN;
    uint32 const level = bot.level;
    Goals::GrindSpot spot =
        GoalHarness::Spot(GOALS_SPOT_ID, GOALS_SPOT_XP, GOALS_SPOT_COPPER, level > 1 ? level - 1 : 1, level + 1);
    spot.place = start;
    Goals::GrindSpot grey = spot;
    grey.id = GOALS_SPOT_GREY_ID;
    grey.minLevel = 1;
    grey.maxLevel = std::max(Goals::GreyLevel(level), 1u);
    situation.spots = {spot, grey};
    Goals::GatherNode node;
    node.id = GOALS_NODE_ID;
    node.skill = GoalHarness::HERBALISM;
    node.place = start;
    node.walkMinutes = 1.0f;
    node.copperWorth = GOALS_NODE_COPPER;
    situation.nodes = {node};
    return situation;
}

std::string GoalsLine(Goals::Ranked const& ranked)
{
    return Acore::StringFormat("  ranked: {}", GoalHarness::Summary(ranked));
}

// A real option names a hub, camp, spot, node, quest, vendor, item or inn, never the no-hub or no-camp marker, and a
// move to a camp leads there, not back to where the circuit ends
bool GoalTargetsOk(Goals::GoalPlan const& plan)
{
    for (Goals::Option const& option : plan.ranked.options)
    {
        if (option.target == NO_HUB || option.target == NO_CAMP)
            return false;
        if (option.targetType == Goals::TargetType::Camp && option.place.x == plan.circuit.returnTo.x &&
            option.place.y == plan.circuit.returnTo.y)
            return false;
    }
    return true;
}
}  // namespace

bool RunGoalsHarness(ChatHandler* handler)
{
    auto const started = std::chrono::steady_clock::now();
    std::vector<GoalHarness::Check> checks = GoalHarness::SyntheticChecks();
    std::size_t const synthetic = checks.size();
    std::vector<std::string> lines;
    if (!IndexesReady() || !QuestsReady())
        checks.push_back({"real: data", false, "indexes or quests not ready (see `playerbots planner indexes`)"});
    else
    {
        CircuitData const data = CircuitDataNow();
        CircuitContext const context = data.Context();
        for (CircuitPlace const& place : CIRCUIT_PLACES)
        {
            Place start;
            std::string text;
            bool const found = CircuitStart(*data.quests, place, start, text);
            std::string const name = std::string("real ") + place.name;
            if (!found)
            {
                checks.push_back({name + ": start", false, text});
                continue;
            }
            Snapshot bot = CircuitBot(place, start);
            bot.rewardedQuests = GreyHistory(*data.quests, place.level, data.greyDiff);
            bot.money = GOALS_FLUSH;
            Goals::GoalPlan const flush = Goals::PlanGoals(bot, context, GoalsSituation(bot, start));
            Goals::GoalPlan const again = Goals::PlanGoals(bot, context, GoalsSituation(bot, start));
            lines.push_back(name + " flush");
            lines.push_back(GoalsLine(flush.ranked));
            checks.push_back(
                {name + ": a flush bot's ranking is made, with the circuit among its options",
                 flush.ranked.ok && (!flush.circuit.ok || GoalHarness::Has(flush.ranked, Goals::GoalKind::Quest)),
                 GoalHarness::Summary(flush.ranked)});
            checks.push_back({name + ": a flush bot does not choose money-making",
                              flush.ranked.ok && !flush.ranked.options.empty() &&
                                  flush.ranked.options.front().kind != Goals::GoalKind::Money,
                              GoalHarness::Summary(flush.ranked)});
            checks.push_back({name + ": the same input gives the same ranking",
                              GoalHarness::SameList(flush.ranked, again.ranked), "determinism"});
            bot.money = 0;
            Goals::GoalPlan const broke = Goals::PlanGoals(bot, context, GoalsSituation(bot, start));
            lines.push_back(name + " broke");
            lines.push_back(GoalsLine(broke.ranked));
            checks.push_back({name + ": a broke bot weighs money-making",
                              broke.ranked.ok && GoalHarness::Has(broke.ranked, Goals::GoalKind::Money),
                              GoalHarness::Summary(broke.ranked)});
            std::vector<GoalLog::Row> const rows = GoalLog::Rows(broke.ranked);
            bool const rowOk = rows.size() == 1 && !broke.ranked.options.empty() &&
                               rows[0].choice == Goals::ChoiceWord(broke.ranked.options.front());
            checks.push_back({name + ": the ranking maps to one goal row", rowOk,
                              rows.empty() ? std::string("no row") : rows[0].choice + " " + rows[0].reason});
            // A dry hub: every quest of the hub done; the moves are the circuit harness's proof, here the ranking
            Snapshot dry = bot;
            std::vector<uint32> const hub = HubQuests(*data.quests, start);
            std::vector<uint32> done;
            std::set_union(dry.rewardedQuests.begin(), dry.rewardedQuests.end(), hub.begin(), hub.end(),
                           std::back_inserter(done));
            dry.rewardedQuests = std::move(done);
            dry.money = GOALS_FLUSH;
            Goals::GoalPlan const dryPlan = Goals::PlanGoals(dry, context, GoalsSituation(dry, start));
            lines.push_back(name + " dry hub");
            lines.push_back(GoalsLine(dryPlan.ranked));
            checks.push_back({name + ": a dry hub still ranks goals",
                              dryPlan.ranked.ok && !dryPlan.ranked.options.empty(),
                              GoalHarness::Summary(dryPlan.ranked)});
            checks.push_back({name + ": every option names a real target",
                              GoalTargetsOk(flush) && GoalTargetsOk(broke) && GoalTargetsOk(dryPlan),
                              GoalHarness::Summary(dryPlan.ranked)});
        }
    }
    std::size_t failed = 0;
    for (GoalHarness::Check const& check : checks)
    {
        failed += check.pass ? 0 : 1;
        handler->PSendSysMessage("{} {}: {}", check.pass ? "PASS" : "FAIL", check.name, check.text);
    }
    for (std::string const& line : lines)
        handler->PSendSysMessage("{}", line);
    std::string const summary = Acore::StringFormat(
        "PLANNER goals selftest: {}, {} checks ({} synthetic), {} failed, in {} ms, rank {:.1f} us",
        failed ? "FAIL" : "PASS", checks.size(), synthetic, failed, MsSince(started), GoalHarness::RankMicroseconds());
    handler->PSendSysMessage("{}", summary);
    LOG_INFO("playerbots", "{}", summary);
    return !failed;
}

bool HandleGoalsCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string word;
    in >> word;
    if (word.empty())
    {
        handler->SendErrorMessage("Usage: playerbots planner goals selftest | <player name>");
        return false;
    }
    if (word == "selftest")
    {
        RunGoalsHarness(handler);
        return true;
    }
    if (!IndexesReady() || !QuestsReady())
    {
        handler->SendErrorMessage("PLANNER goals: not ready (see `playerbots planner indexes`)");
        return false;
    }
    std::string name = word;
    Player* player = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    if (!player || !player->IsInWorld())
    {
        handler->SendErrorMessage("PLANNER goals: no player {} online", word);
        return false;
    }
    // Read only, on the world thread: the circuit and the Situation inputs the world gives now; a bot's are the live
    // job's (vendor, repairer, bag offers: LiveSituation), a player's only health and mana. A caller offers a hearth
    // option only when WorldTravel::InnCheckDue says so; no inns are given here, so none is offered
    CircuitData const data = CircuitDataNow();
    Snapshot const snapshot = FillSnapshot(player, 0);
    Goals::Situation situation;
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        situation = LiveSituation(ai, player);
    else
    {
        situation.here.hub = NO_HUB;  // as the live job: found from the circuit
        situation.healthPercent = player->GetHealthPct();
        uint32 const maxMana = player->GetMaxPower(POWER_MANA);
        situation.manaPercent =
            maxMana > 0 ? 100.0f * static_cast<float>(player->GetPower(POWER_MANA)) / static_cast<float>(maxMana)
                        : -1.0f;
    }
    // One line naming the bot (a scenario's log keeps only those): the errand inputs the model weighs
    handler->PSendSysMessage(
        "GOALS {} inputs: vendor {} {:.1f}, repairer {} {:.1f}, sellable {}, offers {}, durability {}, free {}/{}, "
        "inns {} first {}, hearthstone {}",
        player->GetName(), situation.bags.vendor.id, situation.bags.vendor.walkMinutes, situation.bags.repairer.id,
        situation.bags.repairer.walkMinutes, situation.bags.sellableSlots, situation.bagOffers.size(),
        snapshot.durabilityPercent, snapshot.bagFree, snapshot.bagSlots, situation.inns.size(),
        situation.inns.empty() ? 0 : situation.inns.front().id, snapshot.hasHearthstone);
    Goals::GoalPlan const plan = Goals::PlanGoals(snapshot, data.Context(), std::move(situation));
    if (WorldLog::Active())
        for (GoalLog::Row const& row : GoalLog::Rows(plan.ranked))
        {
            WorldLogFormat::Alternatives alternatives;
            for (GoalLog::Alt const& alt : row.alternatives)
                alternatives.Add({alt.choice, alt.value});
            WorldLog::Decision(player, WorldLogFormat::DecisionKind::Goal, row.choice, row.reason, row.value,
                               alternatives);
        }
    handler->PSendSysMessage("GOALS {}: level {}, map {} ({:.0f}, {:.0f}), money {}, need factor {:.2f}, {} considered",
                             player->GetName(), snapshot.level, snapshot.position.map, snapshot.position.x,
                             snapshot.position.y, snapshot.money, plan.ranked.need.factor, plan.ranked.considered);
    if (!plan.ranked.ok)
        handler->PSendSysMessage("  no ranking: {}", plan.ranked.failure);
    for (Goals::Option const& option : plan.ranked.options)
        handler->PSendSysMessage("  {} {} {:.1f}/min over {:.1f} min: {}", player->GetName(), Goals::ChoiceWord(option),
                                 option.valuePerMinute, option.minutes, option.reason);
    return true;
}
}  // namespace Planner
