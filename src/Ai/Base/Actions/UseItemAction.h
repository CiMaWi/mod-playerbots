/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_USEITEMACTION_H
#define PLAYERBOTS_USEITEMACTION_H

#include "Action.h"
#include "ItemUseTypes.h"

class Item;
class ObjectGuid;
class PlayerbotAI;
class Unit;

class UseItemAction : public Action
{
public:
    // caller: the word of the use's row (item-use INTERFACE 6.2); the chat command `use` keeps chat
    UseItemAction(PlayerbotAI* botAI, std::string const name = "use", bool selfOnly = false,
                  std::string const itemName = "", ItemUse::Caller caller = ItemUse::Caller::Chat)
        : Action(botAI, name), itemName(itemName.empty() ? name : itemName), caller(caller), selfOnly(selfOnly)
    {
    }

    bool Execute(Event event) override;
    bool isPossible() override;
    bool UseItemOnGameObject(Item* item, ObjectGuid go);

protected:
    std::string itemName;
    ItemUse::Caller caller;
    bool UseItemAuto(Item* item);
    bool UseItem(Item* item, ObjectGuid go, Unit* unitTarget = nullptr);
    bool UseGameObject(ObjectGuid guid);
    std::string ConsumableLine(Item* item, std::string const action, float percent);

private:
    bool selfOnly;
};

class UseSpellItemAction : public UseItemAction
{
public:
    UseSpellItemAction(PlayerbotAI* botAI, std::string const name, bool selfOnly = false,
                       ItemUse::Caller caller = ItemUse::Caller::Fight)
        : UseItemAction(botAI, name, selfOnly, "", caller)
    {
    }

    bool isUseful() override;
};

class UseHealingPotion : public UseItemAction
{
public:
    UseHealingPotion(PlayerbotAI* botAI) : UseItemAction(botAI, "healing potion", false, "", ItemUse::Caller::Fight) {}

    bool isUseful() override;
};

class UseManaPotion : public UseItemAction
{
public:
    UseManaPotion(PlayerbotAI* botAI) : UseItemAction(botAI, "mana potion", false, "", ItemUse::Caller::Fight) {}

    bool isUseful() override;
};

class UseHearthStone : public UseItemAction
{
public:
    UseHearthStone(PlayerbotAI* botAI) : UseItemAction(botAI, "hearthstone", true, "", ItemUse::Caller::Errand) {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class UseRandomRecipe : public UseItemAction
{
public:
    UseRandomRecipe(PlayerbotAI* botAI) : UseItemAction(botAI, "random recipe", true, "", ItemUse::Caller::Upkeep) {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
};

class UseStartQuestItem : public UseItemAction
{
public:
    UseStartQuestItem(PlayerbotAI* botAI) : UseItemAction(botAI, "start quest item", true, "", ItemUse::Caller::Quest)
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
};

#endif
