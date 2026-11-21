/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "UseItemAction.h"

#include <set>
#include <string>

#include "AcceptQuestAction.h"
#include "BotTravel.h"
#include "ChatHelper.h"
#include "Event.h"
#include "ItemUsageValue.h"
#include "ItemUse.h"
#include "LootObjectStack.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "ServerFacade.h"

static constexpr uint32 SPELL_LEARNING_1 = 483;
static constexpr uint32 SPELL_LEARNING_2 = 55884;

bool UseItemAction::Execute(Event event)
{
    std::string name = event.getParam();
    if (name.empty())
        name = itemName;

    std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", name);
    GuidVector gos = chat->parseGameobjects(name);

    if (gos.empty())
    {
        if (!items.empty())
        {
            return UseItemAuto(*items.begin());
        }
    }
    else
    {
        if (items.empty())
            return UseGameObject(*gos.begin());
        else
            return UseItemOnGameObject(*items.begin(), *gos.begin());
    }

    botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault("use_item_none_available",
                                                                      "No items (or game objects) available", {}));
    return false;
}

bool UseItemAction::UseGameObject(ObjectGuid guid)
{
    auto fail = [this](char const* name, char const* defaultText)
    {
        botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(name, defaultText, {}));
        return false;
    };

    GameObject* go = botAI->GetGameObject(guid);
    if (!go || !go->isSpawned())
        return fail("gameobject_unavailable_error", "Game object is no longer available");

    if (sPlayerbotAIConfig.DisallowedGameObjects.contains(go->GetEntry()))
        return fail("gameobject_disallowed_error", "Game object is disallowed by configuration");

    if (sPlayerbotAIConfig.LootDistance && bot->GetDistance(go) > sPlayerbotAIConfig.LootDistance)
        return fail("gameobject_outside_loot_distance_error", "Game object is outside the configured loot distance");

    if (go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE) ||
        (go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND) && !go->ActivateToQuest(bot)))
        return fail("gameobject_not_eligible_error", "Game object is not currently eligible for interaction");

    if (!bot->IsAlive() || bot->IsInFlight() || bot->m_mover != bot || bot->IsNonMeleeSpellCast(false) ||
        bot->GetLootGUID())
        return fail("gameobject_cannot_interact_error",
                    "Cannot interact while dead, flying, remotely controlled, casting, or looting");

    if (go->GetGoType() == GAMEOBJECT_TYPE_CHEST || go->GetGOInfo()->GetLootId())
    {
        LootObject loot(bot, guid);
        if (!loot.IsLootPossible(bot))
            return fail("gameobject_cannot_loot_error",
                        "Cannot loot this object: check quest, skill, tools, key, and object state");

        bool inRange = go->IsAtInteractDistance(bot);
        if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT) && bot->GetDistance(go) > CONTACT_DISTANCE)
            return fail("gameobject_stay_out_of_range_error", "Game object is out of reach while staying");

        bool canContinue =
            botAI->HasStrategy("loot", BOT_STATE_NON_COMBAT) || botAI->HasStrategy("gather", BOT_STATE_NON_COMBAT);
        if (!inRange && !canContinue)
            return fail("gameobject_approach_unavailable_error",
                        "Move closer or enable the loot or gather strategy to approach this object");

        LootObjectStack* availableLoot = AI_VALUE(LootObjectStack*, "available loot");
        if (availableLoot->HasInteraction() && availableLoot->InteractionGuid() != guid)
            botAI->GetTravel().CancelLootInteraction("order");

        LootObject previous = AI_VALUE(LootObject, "loot target");
        bool added = availableLoot->Add(guid);
        context->GetValue<LootObject>("loot target")->Set(loot);

        std::string objectName = chat->FormatGameobject(go);
        bool const consumed = botAI->DoSpecificAction(inRange ? "open loot" : "move to loot", Event(), true);
        if (!consumed)
        {
            if (added && availableLoot->CanAttemptLoot(guid))
                availableLoot->Remove(guid);
            if (previous.guid != guid || availableLoot->CanAttemptLoot(previous.guid))
                context->GetValue<LootObject>("loot target")->Set(previous);
            else
                context->GetValue<LootObject>("loot target")->Set(LootObject());
            return fail("gameobject_open_failed_error", "Could not approach or open the game object");
        }

        bool const requested = inRange && availableLoot->IsLootPending() && availableLoot->InteractionGuid() == guid;
        botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            requested ? "gameobject_open_requested" : "gameobject_loot_queued",
            requested ? "Opening requested: %gameobject" : "Queued for looting: %gameobject",
            {{"%gameobject", objectName}}));
        return true;
    }

    if (go->GetGOInfo()->GetLockId() && go->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_LOCKED))
        return fail("gameobject_nonloot_locked_error", "This non-loot object requires an opening spell or key");

    if (!go->IsWithinDistInMap(bot, go->GetInteractionDistance()))
        return fail("gameobject_interact_out_of_range_error", "Move closer to interact with this game object");

    if (bot->isMoving())
        bot->StopMoving();
    ServerFacade::instance().SetFacingTo(bot, go);

    WorldPacket use(CMSG_GAMEOBJ_USE, 8);
    use << guid;
    bot->GetSession()->HandleGameObjectUseOpcode(use);

    go = botAI->GetGameObject(guid);
    if (go && go->isSpawned() && go->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
    {
        WorldPacket report(CMSG_GAMEOBJ_REPORT_USE, 8);
        report << guid;
        bot->GetSession()->HandleGameobjectReportUse(report);
    }

    botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "gameobject_interaction_requested", "Game object interaction requested", {}));
    return true;
}

bool UseItemAction::UseItemAuto(Item* item) { return UseItem(item, ObjectGuid::Empty); }

bool UseItemAction::UseItemOnGameObject(Item* item, ObjectGuid go) { return UseItem(item, go); }

bool UseItemAction::UseItem(Item* item, ObjectGuid goGuid, Unit* unitTarget)
{
    // the test's nouse: a bot that makes no request of its own reaches none of the side effects below either
    if (!ItemUse::Allowed(bot, caller))
        return false;

    if (bot->CanUseItem(item) != EQUIP_ERR_OK)
        return false;

    if (bot->IsNonMeleeSpellCast(false))
        return false;

    // nothing is used on a flight path: the stop below would end the taxi's spline and leave the bot hanging in the air
    if (bot->IsInFlight())
        return false;

    GameObject* goTarget = goGuid ? botAI->GetGameObject(goGuid) : nullptr;
    if (goGuid && (!goTarget || !goTarget->isSpawned()))
        return false;

    ItemTemplate const* itemProto = item->GetTemplate();
    bool const isGenericLearnItem =
        itemProto->Spells[0].SpellId == SPELL_LEARNING_1 || itemProto->Spells[0].SpellId == SPELL_LEARNING_2;

    if (isGenericLearnItem)
    {
        if (bot->HasSpell(itemProto->Spells[1].SpellId))
            return false;
    }
    else if (itemProto->Spells[0].SpellId)
    {
        // Older/direct layout: Spells[0] itself teaches the spell(s), via one or
        // more SPELL_EFFECT_LEARN_SPELL effects (not necessarily in effect slot 0).
        if (SpellInfo const* learnSpellInfo = sSpellMgr->GetSpellInfo(itemProto->Spells[0].SpellId))
        {
            bool foundLearnEffect = false;
            bool allKnown = true;
            for (auto const& effect : learnSpellInfo->Effects)
            {
                if (effect.Effect != SPELL_EFFECT_LEARN_SPELL || !effect.TriggerSpell)
                    continue;

                foundLearnEffect = true;
                if (!bot->HasSpell(effect.TriggerSpell))
                {
                    allKnown = false;
                    break;
                }
            }

            if (foundLearnEffect && allKnown)
                return false;
        }
    }

    std::string itemText = chat->FormatItem(itemProto);
    std::string targetText;

    if (itemProto->Stackable > 1)
    {
        uint32 count = item->GetCount();
        if (count > 1)
            itemText += " (" + std::to_string(count) + " available)";
        else
            itemText += " (the last one!)";
    }

    // The use is a request to the item use path (item-use WI5). Its target is the one the old packet named first,
    // which the game's handler read, in the old order: the object, the master's unit, the unit
    ItemUse::Request request = ItemUse::OnSelf(item, caller);
    bool targetSelected = false;

    if (goTarget)
    {
        request = ItemUse::OnObject(item, goTarget, caller);
        targetText = chat->FormatGameobject(goTarget);
        targetSelected = true;
    }

    Player* master = GetMaster();
    if (!targetSelected && itemProto->Class != ITEM_CLASS_CONSUMABLE && master && IsRealPlayer(botAI->GetMaster()) &&
        !selfOnly)
    {
        if (ObjectGuid masterSelection = master->GetTarget())
        {
            if (Unit* unit = botAI->GetUnit(masterSelection))
            {
                request = ItemUse::OnUnit(item, unit, caller);
                targetText = unit->GetName();
                targetSelected = true;
            }
        }
    }

    if (!targetSelected && itemProto->Class != ITEM_CLASS_CONSUMABLE && unitTarget)
    {
        request = ItemUse::OnUnit(item, unitTarget, caller);
        targetText = unitTarget->GetName();
        targetSelected = true;
    }

    // A quest-start item has no use spell: the bot accepts its quest directly, outside the item use path (WI5)
    if (uint32 questId = itemProto->StartQuest)
    {
        if (Quest const* qInfo = sObjectMgr->GetQuestTemplate(questId))
        {
            // The master-driven "use <item>" path accepts what the player could; the automatic
            // path filters with QuestAction::CanAcceptQuest before calling UseItem.
            AcceptQuestAction acceptQuestAction(botAI);
            return acceptQuestAction.AcceptQuest(qInfo, item->GetGUID());
        }
    }

    bot->ClearUnitState(UNIT_STATE_CHASE);
    bot->ClearUnitState(UNIT_STATE_FOLLOW);

    if (bot->isMoving())
    {
        bot->StopMoving();
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.GlobalCoolDown);
        return false;
    }

    // A use spell that takes an item: the bot's own item for it, or the trade window's slot while trading
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; i++)
    {
        uint32 spellId = itemProto->Spells[i].SpellId;
        if (!spellId || itemProto->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        if (spellInfo->Targets & TARGET_FLAG_ITEM)
        {
            Item* itemForSpell = AI_VALUE2(Item*, "item for spell", spellId);
            if (!itemForSpell)
                continue;

            if (itemForSpell->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                continue;

            if (bot->GetTrader())
            {
                if (selfOnly)
                    return false;

                if (!targetSelected)
                {
                    request = ItemUse::OnTradeSlot(item, caller);
                    targetText = "traded item";
                    targetSelected = true;
                }
            }
            else if (!targetSelected)
            {
                request = ItemUse::OnItem(item, itemForSpell, caller);
                targetText = chat->FormatItem(itemForSpell->GetTemplate());
                targetSelected = true;
            }
            uint32 castTime = spellInfo->CalcCastTime();
            botAI->SetNextCheckDelay(castTime + sPlayerbotAIConfig.ReactDelay);
        }

        break;
    }

    if (!targetSelected)
    {
        // a consumable on a unit (a soulstone on a friend): the unit is the request's target
        if (unitTarget)
        {
            request = ItemUse::OnUnit(item, unitTarget, caller);
            if (unitTarget == bot || !unitTarget->IsInWorld() || unitTarget->IsDuringRemoveFromWorld())
                targetText = "self";
            else if (unitTarget->IsHostileTo(bot))
                targetText = "self";
            else
                targetText = unitTarget->GetName();
        }
        else
            targetText = "self";
        targetSelected = true;
    }

    // Check first: a caller that polls (its trigger holds while a cast runs) writes no row per update
    if (ItemUse::Check(bot, request) != ItemUse::Cause::None)
        return false;

    bool isDrink = itemProto->Spells[0].SpellCategory == 59;
    bool isFood = itemProto->Spells[0].SpellCategory == 11;
    if (itemProto->Class == ITEM_CLASS_CONSUMABLE &&
        (itemProto->SubClass == ITEM_SUBCLASS_FOOD || itemProto->SubClass == ITEM_SUBCLASS_CONSUMABLE) &&
        (isFood || isDrink))
    {
        if (bot->IsInCombat())
            return false;

        // bot->SetStandState(UNIT_STAND_STATE_SIT);
        bot->CastStop();
        float hp = bot->GetHealthPct();
        float mp = bot->GetPower(POWER_MANA) * 100.0f / bot->GetMaxPower(POWER_MANA);
        float const p = isDrink && !isFood ? mp : std::min(hp, mp);
        // the line is made first: the use can consume the last one
        std::string const consumableLine =
            ConsumableLine(item, isDrink && isFood ? "Feasting" : (isDrink ? "Drinking" : "Eating"), p);
        if (ItemUse::Use(bot, request).outcome == ItemUse::Outcome::Refused)
            return false;

        if (!consumableLine.empty())
            botAI->TellMasterNoFacing(consumableLine);

        if (!bot->IsInCombat() && !bot->InBattleground())
            SetDuration((uint32)std::max(10.0f * IN_MILLISECONDS, 27.0f * IN_MILLISECONDS * (100 - p) / 100.0f));

        if (!bot->IsInCombat() && bot->InBattleground())
            SetDuration((uint32)std::max(10.0f * IN_MILLISECONDS, 20.0f * IN_MILLISECONDS * (100 - p) / 100.0f));

        return true;
    }

    std::string useText =
        targetSelected
            ? PlayerbotTextMgr::instance().GetBotTextOrDefault("use_item_on_target", "Using %item on %target",
                                                               {{"%item", itemText}, {"%target", targetText}})
            : PlayerbotTextMgr::instance().GetBotTextOrDefault("use_item", "Using %item", {{"%item", itemText}});
    botAI->TellMasterNoFacing(useText);
    return ItemUse::Use(bot, request).outcome != ItemUse::Outcome::Refused;
}

std::string UseItemAction::ConsumableLine(Item* item, std::string const action, float percent)
{
    if (!sPlayerbotAIConfig.AnnounceConsumableUse)
        return "";

    std::ostringstream out;
    out << action << " " << chat->FormatItem(item->GetTemplate());

    if (item->GetTemplate()->Stackable > 1)
        out << "/x" << item->GetCount();

    out << " (" << round(percent) << "%)";
    return out.str();
}

bool UseItemAction::isPossible() { return getName() == "use" || AI_VALUE2(uint32, "item count", itemName) > 0; }

bool UseSpellItemAction::isUseful() { return AI_VALUE2(bool, "spell cast useful", getName()); }

bool UseHealingPotion::isUseful() { return AI_VALUE2(bool, "combat", "self target"); }

bool UseManaPotion::isUseful() { return AI_VALUE2(bool, "combat", "self target"); }

bool UseHearthStone::Execute(Event event)
{
    if (bot->isMoving())
    {
        MotionMaster& mm = *bot->GetMotionMaster();
        bot->StopMoving();
        mm.Clear();
    }

    bool used = UseItemAction::Execute(event);
    if (used)
    {
        RESET_AI_VALUE(bool, "combat::self target");
        RESET_AI_VALUE(WorldPosition, "current position");
        botAI->SetNextCheckDelay(10 * IN_MILLISECONDS);
    }

    return used;
}

bool UseHearthStone::isUseful() { return !bot->InBattleground(); }

bool UseRandomRecipe::Execute(Event /*event*/)
{
    std::vector<Item*> recipes = AI_VALUE2(std::vector<Item*>, "inventory items", "recipe");

    // Every recipe the bot can learn now, each once by name (professions WR12; it took only the last one)
    std::set<std::string> names;
    for (Item* recipe : recipes)
    {
        ItemTemplate const* proto = recipe->GetTemplate();
        // The taught spell is the recipe's second spell (the first is the learning cast, 483)
        if (!bot->HasSpell(proto->Spells[1].SpellId) && bot->CanUseItem(proto) == EQUIP_ERR_OK)
            names.insert(proto->Name1);
    }

    bool used = false;
    for (std::string const& recipeName : names)
        used = UseItemAction::Execute(Event(name, recipeName)) || used;

    if (used)
        botAI->SetNextCheckDelay(3.0 * IN_MILLISECONDS);

    return used;
}

bool UseRandomRecipe::isUseful()
{
    return !bot->IsInCombat() && !IsRealPlayer(botAI->GetMaster()) && !bot->InBattleground();
}

bool UseRandomRecipe::isPossible() { return AI_VALUE2(uint32, "item count", "recipe") > 0; }

bool UseStartQuestItem::Execute(Event /*event*/)
{
    for (Item* questItem : AI_VALUE2(std::vector<Item*>, "inventory items", "start quest"))
    {
        Quest const* qInfo = sObjectMgr->GetQuestTemplate(questItem->GetTemplate()->StartQuest);
        if (!QuestAction::CanAcceptQuest(bot, qInfo))
            continue;

        // UseItem can fail before the start-quest accept (e.g. a transient cast state), so keep
        // looking at the other eligible items instead of stopping at the first one.
        if (UseItem(questItem, ObjectGuid::Empty, nullptr))
        {
            botAI->SetNextCheckDelay(sPlayerbotAIConfig.GlobalCoolDown);
            return true;
        }
    }

    return false;
}

bool UseStartQuestItem::isUseful()
{
    return !bot->IsInCombat() && !IsRealPlayer(botAI->GetMaster()) && !bot->InBattleground() &&
           !bot->HasUnitState(UNIT_STATE_IN_FLIGHT);
}

bool UseStartQuestItem::isPossible()
{
    for (Item* questItem : AI_VALUE2(std::vector<Item*>, "inventory items", "start quest"))
        if (QuestAction::CanAcceptQuest(bot, sObjectMgr->GetQuestTemplate(questItem->GetTemplate()->StartQuest)))
            return true;

    return false;
}
