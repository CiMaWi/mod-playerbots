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
#include "GameObject.h"
#include "Item.h"
#include "ItemUse.h"
#include "ItemUseIndex.h"
#include "Log.h"
#include "NewRpgAction.h"
#include "NewRpgHelpers.h"
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
#include "UpkeepNeedRules.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

// WT35b: a quest item used at the target of the objective's use rule (QuestUseRules.h, the one source): the bot seeks
// a creature of the rule, alive or lying dead as it wants, or an object of it (an item on an object, a lock's key, a
// spell focus), walks into the spell's range and asks the item-use framework with the caller quest (the game's own use
// handler casts it). A class spell rule is the bot's own cast at the creature. A timed use is waited for, the bot's AI
// held through its channel; the quest log says whether it counted; a refusal waiting cannot change ends the order with
// its cause word (QuestUseExecRules.h)
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

// WT35g: what the use of the step's objective brings forth (QuestObjective::brings); Credit without the index
Planner::UseBrings BringsOf(NewRpgInfo::DoQuest const& data)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
    uint32 const objective = Planner::QuestExec::ObjectiveOf(data.place);
    return record && objective < Planner::QuestExec::ObjectiveCount(*record)
               ? Planner::QuestExec::ObjectiveAt(*record, objective).brings
               : Planner::UseBrings::Credit;
}

// WT35g: a chain whose use spent its item waits a while for what the use brings forth instead of ending no_item
// (QuestExecRules.h AwaitsBroughtForth); without the index it ends
bool AwaitsBroughtForth(NewRpgInfo::DoQuest const& data, char const* word)
{
    if (!word || std::string_view(word) != Planner::QuestExec::NO_ITEM)
        return false;
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
    uint32 const objective = Planner::QuestExec::ObjectiveOf(data.place);
    if (!record || objective >= Planner::QuestExec::ObjectiveCount(*record))
        return false;
    uint32 const since = data.lastEngage ? GetMSTimeDiffToNow(data.lastEngage) : UINT32_MAX;
    return Planner::QuestExec::AwaitsBroughtForth(Planner::QuestExec::ObjectiveAt(*record, objective), since);
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
    // WT35b: q9303's inoculation credits the caster whose aura ends first; a second bot's use on the same owlkin
    // read as used and never counted. A player does not use a quest item on a target another one is working on
    seen.taken = rule.itemId && rule.spellId && creature->HasAura(rule.spellId) &&
                 !creature->HasAura(rule.spellId, bot->GetGUID());
    seen.inReach =
        bot->GetDistance(creature) <= Planner::QuestExec::UseReach(rule.rangeYards) && bot->IsWithinLOSInMap(creature);
    return seen;
}

// An object of an object or focus rule: spawned; never dead. Reach by distance only (the small reach of an object rule;
// an object's own model may stand between the bot and its centre)
UseSeen SeeObject(Player* bot, NewRpgInfo::DoQuest const& data, Planner::QuestUseRule const& rule, GameObject* go)
{
    UseSeen seen;
    seen.pending = ItemUse::Pending(bot);
    if (!go || !go->IsInWorld() || !go->isSpawned())
        return seen;
    seen.present = true;
    seen.passed = Planner::QuestExec::Remembered(data.passed, go->GetGUID());
    seen.inReach = bot->GetDistance(go) <= Planner::QuestExec::UseReach(rule.rangeYards);
    return seen;
}

// The request as the item record's shape aims it (INTERFACE 6.1), with the rule's spell: at the bot (an item used
// near its target, at a focus), at the target's spot, else at the target itself, the creature or the object
ItemUse::Request RequestFor(Item* item, Creature* creature, GameObject* go, Planner::QuestUseRule const& rule)
{
    ItemUse::Shape shape = go ? ItemUse::Shape::Object : ItemUse::Shape::AnyUnit;
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
            request = go ? ItemUse::AtSpot(item, *go, ItemUse::Caller::Quest)
                         : ItemUse::AtSpot(item, *creature, ItemUse::Caller::Quest);
            break;
        default:
            if (go)
                request = ItemUse::OnObject(item, go, ItemUse::Caller::Quest);
            else
            {
                request = ItemUse::OnUnit(item, creature, ItemUse::Caller::Quest);
                request.target.dead = WantsDead(rule);
            }
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
        // A chain's spent item: the place is kept while what the use brought forth comes (the scan finds it)
        if (!word || AwaitsBroughtForth(data, word))
            return false;
        // Nothing to use it with: the order ends before any walk (no item is handed out, PLAN 4 WT35b)
        LOG_DEBUG("playerbots", "[New RPG] {} quest {}: {}", bot->GetName(), data.questId, word);
        RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = word});
        return true;
    }

    // The nearest target the verdict does not skip; a pending use of another caller does not hide one
    float const range = std::min(maxYards, sPlayerbotAIConfig.SightDistance);
    WorldObject* nearest = nullptr;
    float nearestYards = range;
    auto const consider = [&](WorldObject* target, UseSeen seen)
    {
        if (!OnOrder(data, target))
            return;  // questing WQ20: one the order does not take (off the field, off the way)
        seen.pending = false;
        float const yards = bot->GetDistance(target);
        if (Planner::QuestExec::UseVerdict(seen) != UseAct::Skip && yards < nearestYards)
        {
            nearest = target;
            nearestYards = yards;
        }
    };
    std::size_t candidates = 0;
    if (Planner::QuestExec::UsesObject(rule->kind))
    {
        std::list<GameObject*> objects;
        bot->GetGameObjectListWithEntryInGrid(objects, rule->entries, range);
        for (GameObject* go : objects)
            consider(go, SeeObject(bot, data, *rule, go));
        candidates = objects.size();
    }
    else
    {
        std::list<Creature*> creatures;
        bot->GetCreatureListWithEntryInGrid(creatures, rule->entries, range);
        for (Creature* creature : creatures)
            consider(creature, See(bot, data, *rule, creature));
        candidates = creatures.size();
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
          {{"entry", static_cast<float>(nearest->GetEntry())}, {"candidates", static_cast<float>(candidates)}});
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
        if (since < QUEST_USE_HOLD_MS || bot->IsNonMeleeSpellCast(false))
            return true;
        // the target as the hold leaves it: a use whose credit transforms its target shows the script ran
        Unit const* held = botAI->GetUnit(data.targetGuid);
        LOG_DEBUG("playerbots", "[New RPG] {} quest {}: use on {} held {} ms, target now {} {} at {:.1f} yd",
                  bot->GetName(), data.questId, entry, since, held ? held->GetEntry() : 0,
                  held ? (held->IsAlive() ? "alive" : "dead") : "gone", held ? bot->GetDistance(held) : 0.0f);
        uint32 const now = held ? held->GetEntry() : 0;
        Trace(bot, "use_done", "use", since / 1000.0f, data.questId,
              {{"entry", static_cast<float>(entry)}, {"now", static_cast<float>(now)}});
        if (Planner::QuestExec::PassesUsed(BringsOf(data), entry, now))
            Planner::QuestExec::Remember(data.passed, data.passedNext, data.targetGuid);
        ClearTarget(data);
        return true;
    }

    std::shared_ptr<Planner::QuestIndex const> index;
    char const* word = nullptr;
    Planner::QuestUseRule const* rule = RuleFor(bot, data, index, word);
    if (!rule)
    {
        if (word && !AwaitsBroughtForth(data, word))
            RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = word});
        else
            ClearTarget(data);
        return true;
    }
    GameObject* go = data.targetGuid.IsGameObject() ? botAI->GetGameObject(data.targetGuid) : nullptr;
    Unit* unit = go ? nullptr : botAI->GetUnit(data.targetGuid);
    Creature* creature = unit ? unit->ToCreature() : nullptr;
    WorldObject* target = go ? static_cast<WorldObject*>(go) : creature;
    float const yards = target ? bot->GetDistance(target) : 0.0f;

    UseSeen const seen = go ? SeeObject(bot, data, *rule, go) : See(bot, data, *rule, creature);
    UseAct const act = Planner::QuestExec::UseVerdict(seen);
    if (act == UseAct::Wait)
        return true;
    if (act == UseAct::Skip)
    {
        // Gone, taken by another player's use or not (yet) in the life state the rule wants: not remembered, the
        // seek takes it again once it is free
        Trace(bot, "pass_over",
              !target      ? "gone"
              : seen.taken ? "taken"
                           : "life",
              data.engagedMs / 1000.0f, data.questId, {{"entry", static_cast<float>(entry)}});
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
        data.targetPos =
            WorldPosition(bot->GetMapId(), target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
        return WalkOn(data, data.targetPos, yards, true, entry, Planner::QuestExec::UseReach(rule->rangeYards));
    }

    ResetWalk(data);
    if (bot->isMoving())
        bot->StopMoving();
    if (bot->IsMounted())
    {
        bot->Dismount();
        return true;
    }
    if (NewRpgHelpers::RealShapeshift(bot))
    {
        bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);  // the framework refuses a use in one (form)
        return true;
    }
    if (data.useAskedAt && GetMSTimeDiffToNow(data.useAskedAt) < QUEST_USE_RETRY_MS)
        return true;
    data.useAskedAt = getMSTime();
    if (!rule->itemId)
    {
        // A class spell: the bot's own cast at the creature (custom logic, item-use INTERFACE 6.6); the hold waits out
        // its cast time, the log says whether it counted
        bool const cast = creature && botAI->CastSpell(rule->spellId, creature);
        LOG_DEBUG("playerbots", "[New RPG] {} casts {} on {} for quest {}: {}", bot->GetName(), rule->spellId, entry,
                  data.questId, cast ? "cast" : "refused");
        TraceUse(bot, *rule, cast ? "used" : Planner::QuestExec::CAST_REFUSED, yards, entry);
        if (cast)
            data.actedAt = getMSTime();
        else if (Planner::QuestExec::AfterCast(data.useTries) == Planner::QuestExec::Refusal::PassOver)
            PassOver(data, "cast");
        else
            ++data.useTries;
        return true;
    }
    Item* item = bot->GetItemByEntry(rule->itemId);
    if (!item)
    {
        RpgStatus::End(botAI, StatusEnd::Outcome::Abandoned, {.word = Planner::QuestExec::NO_ITEM});
        return true;
    }
    ItemUse::UseResult const result = ItemUse::Use(bot, RequestFor(item, creature, go, *rule));
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

bool NewRpgDoQuestAction::HoldUseChannel(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    auto const* data = std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data);
    if (!Planner::QuestExec::HoldsUseChannel(data && data->usePending,
                                             bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr,
                                             UpkeepNeeds::TameBreaksOff(bot->GetHealthPct())))
        return false;
    // The bot stands and channels: no swing or shot at the creature it uses the item on
    if (bot->GetVictim())
        bot->AttackStop();
    if (bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
    return true;
}

bool NewRpgDoQuestAction::StallTriesPlace(NewRpgInfo::DoQuest const& data) const
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    Planner::QuestRecord const* record = index ? index->Find(data.questId) : nullptr;
    Player* const player = bot;
    return !record || Planner::QuestExec::PlaceStallTries(
                          *record, bot->GetMapId(), static_cast<uint8>(bot->GetTeamId()),
                          [player](uint16 skill) { return player->HasSkill(skill); }, data.tried, data.place);
}
