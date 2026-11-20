/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <string_view>

#include "Creature.h"
#include "Item.h"
#include "ItemUse.h"
#include "ItemUseIndex.h"
#include "Log.h"
#include "NewRpgAction.h"
#include "NewRpgInfo.h"
#include "NewRpgStatusEnd.h"
#include "ObjectGuid.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestExecRules.h"
#include "QuestIndex.h"
#include "QuestUseExecRules.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

// WT35b: a quest item used at the target of the objective's use rule (QuestUseRules.h, the one source): the bot seeks
// a creature of the rule, alive or lying dead as it wants, walks into the spell's range and asks the item-use framework
// with the caller quest (the game's own use handler casts it). A timed use is waited for; the quest log says whether
// it counted; a refusal waiting cannot change ends the order with its cause word (QuestUseExecRules.h)
namespace
{
using Planner::QuestExec::UseAct;
using Planner::QuestExec::UseSeen;

constexpr uint32 QUEST_USE_HOLD_MS = 2500;   // after a use: the credit
constexpr uint32 QUEST_USE_RETRY_MS = 1500;  // between two requests at one target after a retriable refusal

// A quest_exec decision row (world-log.md): ref is the quest
void Trace(Player* bot, std::string_view choice, std::string_view reason, float value, uint32 questId,
           WorldLogFormat::Alternatives const& alternatives = {})
{
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::QuestExec, choice, reason, value, alternatives, questId);
}

// The quest_use decision row (world-log.md): one per request or refusal, with its result word
void TraceUse(Player* bot, Planner::QuestUseRule const& rule, std::string_view result, float yards, uint32 entry)
{
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::QuestUse, rule.itemId ? "item" : "spell", result, yards,
                       {{rule.itemId ? "item" : "spell", static_cast<float>(rule.itemId ? rule.itemId : rule.spellId)},
                        {"entry", static_cast<float>(entry)},
                        {"kind", static_cast<float>(static_cast<uint8>(rule.kind))}},
                       rule.questId);
}

// The rule this bot works the step's objective by; null with a word when it holds no rule's item (or knows no rule's
// spell), null without one while the index is not there. `index` keeps the rule alive for the tick
Planner::QuestUseRule const* RuleFor(Player* bot, NewRpgInfo::DoQuest const& data,
                                     std::shared_ptr<Planner::QuestIndex const>& index, char const*& word)
{
    word = nullptr;
    index = Planner::Quests();
    Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
    uint32 const objective = Planner::QuestExec::ObjectiveOf(data.place);
    if (!record || objective >= Planner::QuestExec::ObjectiveCount(*record))
        return nullptr;
    auto const [first, last] = index->uses.RulesFor(data.questId);
    Planner::QuestExec::UsePick const pick = Planner::QuestExec::PickRule(
        first, last, objective < record->objectives.size() ? objective : Planner::USE_COMPLETES_QUEST,
        Planner::QuestExec::ObjectiveAt(*record, objective).useKind, [bot](uint32 item)
        { return bot->HasItemCount(item, 1); }, [bot](uint32 spell) { return bot->HasSpell(spell); });
    word = pick.word;
    return pick.rule;
}

bool WantsDead(Planner::QuestUseRule const& rule) { return rule.kind == Planner::UseKind::UnitDead || rule.mustBeDead; }

// Dead, or lying as dead (a quest's corpse that is a living creature with the dead stand state or flag)
bool LiesDead(Creature const* creature)
{
    return !creature->IsAlive() || creature->getStandState() == UNIT_STAND_STATE_DEAD ||
           creature->HasDynamicFlag(UNIT_DYNFLAG_DEAD);
}

UseSeen See(Player* bot, NewRpgInfo::DoQuest const& data, Planner::QuestUseRule const& rule, Creature* creature)
{
    UseSeen seen;
    seen.pending = ItemUse::Pending(bot);
    if (!creature || !creature->IsInWorld())
        return seen;
    seen.present = true;
    seen.dead = LiesDead(creature);
    seen.wantDead = WantsDead(rule);
    seen.passed = Planner::QuestExec::Remembered(data.passed, creature->GetGUID());
    seen.inReach =
        bot->GetDistance(creature) <= Planner::QuestExec::UseReach(rule.rangeYards) && bot->IsWithinLOSInMap(creature);
    return seen;
}

// The request as the item record's shape aims it (INTERFACE 6.1), with the rule's spell
ItemUse::Request RequestFor(Item* item, Creature* creature, Planner::QuestUseRule const& rule)
{
    ItemUse::Shape shape = ItemUse::Shape::AnyUnit;
    for (ItemUse::Record const& record : ItemUseIndex::Get().OfItem(rule.itemId))
        if (record.spellId == rule.spellId)
            shape = record.shape;
    ItemUse::Request request;
    switch (Planner::QuestExec::AimOf(shape))
    {
        case Planner::QuestExec::UseAim::Self:
            request = ItemUse::OnSelf(item, ItemUse::Caller::Quest);
            break;
        case Planner::QuestExec::UseAim::Spot:
            request = ItemUse::AtSpot(item, *creature, ItemUse::Caller::Quest);
            break;
        default:
            request = ItemUse::OnUnit(item, creature, ItemUse::Caller::Quest);
            request.target.dead = WantsDead(rule);
            break;
    }
    request.spellId = rule.spellId;
    return request;
}

// The result word of the bot's last ItemUse row (the framework's ring), empty when it has none
std::string LastUseResult(PlayerbotAI* botAI)
{
    if (!botAI->botItems || !botAI->botItems->lastCount)
        return {};
    ItemUse::BotItems const& items = *botAI->botItems;
    return items.lastUses[(items.lastHead + ItemUse::LAST_USES - 1) % ItemUse::LAST_USES].result;
}
}  // namespace

bool NewRpgDoQuestAction::SeekUse(NewRpgInfo::DoQuest& data, float maxYards)
{
    std::shared_ptr<Planner::QuestIndex const> index;
    char const* word = nullptr;
    Planner::QuestUseRule const* rule = RuleFor(bot, data, index, word);
    if (!rule)
    {
        if (!word)
            return false;
        // Nothing to use it with: the order ends before any walk (no item is handed out, PLAN 4 WT35b)
        LOG_DEBUG("playerbots", "[New RPG] {} quest {}: {}", bot->GetName(), data.questId, word);
        RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = word});
        return true;
    }

    float const range = std::min(maxYards, sPlayerbotAIConfig.SightDistance);
    std::list<Creature*> creatures;
    bot->GetCreatureListWithEntryInGrid(creatures, rule->entries, range);
    Creature* nearest = nullptr;
    float nearestYards = range;
    for (Creature* creature : creatures)
    {
        UseSeen seen = See(bot, data, *rule, creature);
        seen.pending = false;  // a pending use of another caller does not hide a target
        if (Planner::QuestExec::UseVerdict(seen) == UseAct::Skip)
            continue;
        float const yards = bot->GetDistance(creature);
        if (yards < nearestYards)
        {
            nearest = creature;
            nearestYards = yards;
        }
    }
    if (!nearest)
        return false;

    data.targetGuid = nearest->GetGUID();
    data.targetPos =
        WorldPosition(bot->GetMapId(), nearest->GetPositionX(), nearest->GetPositionY(), nearest->GetPositionZ());
    data.engagedMs = 0;
    data.ticking = false;
    data.actedAt = 0;
    data.useTries = 0;
    data.useAskedAt = 0;
    data.usePending = false;
    Trace(bot, "hold", "use", nearestYards, data.questId,
          {{"entry", static_cast<float>(nearest->GetEntry())}, {"candidates", static_cast<float>(creatures.size())}});
    return true;
}

bool NewRpgDoQuestAction::EngageUse(NewRpgInfo::DoQuest& data)
{
    // A fight holds the engagement; the combat actions fight. The pursuit clock bounds the walk and the waits
    if (bot->IsInCombat())
        return false;
    uint32 const sinceLast = data.lastEngage ? GetMSTimeDiffToNow(data.lastEngage) : 0;
    Planner::QuestExec::Pursue(data.engagedMs, data.ticking, false, sinceLast);
    data.lastEngage = getMSTime();
    uint32 const entry = data.targetGuid.GetEntry();

    // The pending and held uses need no rule: a use may spend its item (a vessel), the hold still waits for the credit
    if (data.usePending)
    {
        // A timed use: no second request while it runs; its end is the framework's ItemUse row
        if (ItemUse::Pending(bot))
            return true;
        data.usePending = false;
        if (LastUseResult(botAI).rfind("interrupted", 0) == 0 && ++data.useTries < Planner::QuestExec::USE_RETRIES)
        {
            data.useAskedAt = getMSTime();  // stands still and asks again
            return true;
        }
        data.actedAt = getMSTime();
        return true;
    }
    if (data.actedAt)
    {
        // Used: held for the credit, then written off; the log says whether it counted, the seek takes the next
        uint32 const since = GetMSTimeDiffToNow(data.actedAt);
        if (since < QUEST_USE_HOLD_MS)
            return true;
        // the target as the hold leaves it: a use whose credit transforms its target shows the script ran
        Unit const* held = botAI->GetUnit(data.targetGuid);
        LOG_DEBUG("playerbots", "[New RPG] {} quest {}: use on {} held {} ms, target now {} {} at {:.1f} yd",
                  bot->GetName(), data.questId, entry, since, held ? held->GetEntry() : 0,
                  held ? (held->IsAlive() ? "alive" : "dead") : "gone", held ? bot->GetDistance(held) : 0.0f);
        Trace(bot, "use_done", "use", since / 1000.0f, data.questId, {{"entry", static_cast<float>(entry)}});
        Planner::QuestExec::Remember(data.passed, data.passedNext, data.targetGuid);
        ClearTarget(data);
        return true;
    }

    std::shared_ptr<Planner::QuestIndex const> index;
    char const* word = nullptr;
    Planner::QuestUseRule const* rule = RuleFor(bot, data, index, word);
    if (!rule)
    {
        if (word)
            RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = word});
        else
            ClearTarget(data);
        return true;
    }
    Unit* unit = botAI->GetUnit(data.targetGuid);
    Creature* creature = unit ? unit->ToCreature() : nullptr;
    float const yards = creature ? bot->GetDistance(creature) : 0.0f;

    UseAct const act = Planner::QuestExec::UseVerdict(See(bot, data, *rule, creature));
    if (act == UseAct::Wait)
        return true;
    if (act == UseAct::Skip)
    {
        // Gone or not (yet) in the life state the rule wants: not remembered, the seek takes it again once it is
        Trace(bot, "pass_over", creature ? "life" : "gone", data.engagedMs / 1000.0f, data.questId,
              {{"entry", static_cast<float>(entry)}});
        ClearTarget(data);
        return true;
    }
    if (Planner::QuestExec::PursuitOver(data.engagedMs))
    {
        PassOver(data, "clock");
        return true;
    }
    if (act == UseAct::Walk)
    {
        data.targetPos = WorldPosition(bot->GetMapId(), creature->GetPositionX(), creature->GetPositionY(),
                                       creature->GetPositionZ());
        return WalkOn(data, data.targetPos, yards, true, entry);
    }

    ResetWalk(data);
    if (bot->isMoving())
        bot->StopMoving();
    if (bot->IsMounted())
    {
        bot->Dismount();
        return true;
    }
    if (data.useAskedAt && GetMSTimeDiffToNow(data.useAskedAt) < QUEST_USE_RETRY_MS)
        return true;
    Item* item = bot->GetItemByEntry(rule->itemId);
    if (!item)
    {
        RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = Planner::QuestExec::NO_ITEM});
        return true;
    }
    data.useAskedAt = getMSTime();
    ItemUse::UseResult const result = ItemUse::Use(bot, RequestFor(item, creature, *rule));
    LOG_DEBUG("playerbots", "[New RPG] {} uses quest item {} on {} for quest {}: outcome {} cause {}", bot->GetName(),
              rule->itemId, entry, data.questId, static_cast<uint32>(result.outcome), ItemUse::CauseWord(result.cause));
    TraceUse(bot, *rule, Planner::QuestExec::UseRowWord(result.outcome, result.cause), yards, entry);
    switch (result.outcome)
    {
        case ItemUse::Outcome::Used:
            data.actedAt = getMSTime();
            return true;
        case ItemUse::Outcome::Pending:
            data.usePending = true;
            Trace(bot, "use_pending", "use", yards, data.questId, {{"entry", static_cast<float>(entry)}});
            return true;
        default:
            break;
    }
    switch (Planner::QuestExec::AfterRefusal(result.cause, data.useTries))
    {
        case Planner::QuestExec::Refusal::GiveUp:
            RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned,
                           {.word = Planner::QuestExec::GiveUpWord(result.cause)});
            return true;
        case Planner::QuestExec::Refusal::PassOver:
            PassOver(data, Planner::QuestExec::GiveUpWord(result.cause));
            return true;
        default:
            ++data.useTries;
            return true;
    }
}
