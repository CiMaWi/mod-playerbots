/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The server side of the quest index (travel plan WT13a): the world-thread gather, the worker build, the published
// index; and the population's quest statistics: the events, the load, the flush and the view.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "BotKind.h"
#include "Chat.h"
#include "ClassQuestJoin.h"
#include "ConditionMgr.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObjectData.h"
#include "GatherLive.h"
#include "GoalModel.h"
#include "Log.h"
#include "NeedGoalBridge.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerFishing.h"
#include "PlannerIndexes.h"
#include "PlannerPool.h"
#include "PlannerQuestInternal.h"
#include "PlannerRecipes.h"
#include "PlannerRouteCost.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "QuestCircuit.h"
#include "QuestDef.h"
#include "QuestExecRules.h"
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
#include "World.h"

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

}  // namespace

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

namespace
{
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
// POI point carries its baked floor from `floors`. WQ13: `fields` gets every continent spawn for the field map
QuestGather GatherQuests(QuestPoiZStore const& floors, std::vector<FieldSpawn>& fields)
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
        {
            spawn(false, data.id, data.mapid, data.posX, data.posY, data.posZ, data.phaseMask);
            if (std::optional<FieldSpawn> const field = FieldCreature(guid, data))
                fields.push_back(*field);
        }
    std::unordered_set<uint32> const objectEvents = EventObjectSpawns();
    for (auto const& [guid, data] : sObjectMgr->GetAllGOData())
        if (!objectEvents.count(guid))
        {
            spawn(true, data.id, data.mapid, data.posX, data.posY, data.posZ, data.phaseMask);
            if (std::optional<FieldSpawn> const field = FieldObject(guid, data))
                fields.push_back(*field);
        }

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

}  // namespace

CircuitData CircuitDataNow()
{
    CircuitData data;
    data.indexes = IndexesReady() ? Indexes() : nullptr;
    data.quests = QuestsReady() ? Quests() : nullptr;
    data.stats = QuestStats();
    data.crowding = Crowding();
    data.fields = FieldsReady() ? Fields() : nullptr;
    data.greyDiff = sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF);
    return data;
}

namespace
{
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
    auto fieldSpawns = std::make_shared<std::vector<FieldSpawn>>();  // WQ13: apart from `input`, which a worker moves
    FieldMapGatherStarted();
    {
        QuestPoiZStore floors;
        floors.Load();
        input = std::make_shared<QuestGather>(GatherQuests(floors, *fieldSpawns));
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
                LogCapabilityCensus(*index);
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
                StartDropRows();  // WR20a: the recipe index's drop rows read the source index
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
    // WQ13: the field map's own build, posted after the quest index's so that one's ready time stays as it was
    PostFieldMapBuild(std::move(fieldSpawns), gatherMs);
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

bool QuestStatsLoaded() { return statsOn.load(std::memory_order_acquire); }

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

// The class-quest table's spells the bot's class trainer teaches (upkeep WU20a): their quests are ordinary ones
std::vector<uint32> ClassTrainerTaught(UpkeepIndex const& upkeep, uint8 cls)
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
}  // namespace Planner
