/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HunterActions.h"
#include "Bag.h"
#include "Event.h"
#include "GenericSpellActions.h"
#include "NaturalTurn.h"
#include "Pet.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Upkeep.h"

bool CastViperStingAction::isUseful()
{
    return CastAuraSpellAction::isUseful() && AI_VALUE2(uint8, "mana", "self target") < 50 &&
           AI_VALUE2(uint8, "mana", "current target") >= 30;
}

bool CastAspectOfTheHawkAction::isUseful()
{
    Unit* target = GetTarget();
    return target && !botAI->HasSpell("aspect of the dragonhawk");
}

bool CastArcaneShotAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !botAI->HasSpell("explosive shot"))
        return false;

    // Armor Penetration rating check - will not cast Arcane Shot above 435 ArP
    int32 armorPenRating =
        bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1) + bot->GetUInt32Value(CR_ARMOR_PENETRATION);
    if (armorPenRating > 435)
        return false;

    return true;
}

bool CastImmolationTrapAction::isUseful()
{
    Unit* target = GetTarget();
    return target && !botAI->HasSpell("explosive trap");
}

Value<Unit*>* CastFreezingTrap::GetTargetValue()
{
    return context->GetValue<Unit*>("cc target", "freezing trap");
}

namespace
{
constexpr uint32 SPELL_FEED_PET = 6991;
constexpr uint32 SPELL_FEED_PET_EFFECT = 1539;  // Feed Pet's trigger spell: the pet eats for 10 s
constexpr time_t FOOD_SEARCH_WAIT_SECONDS = 30;

// The first food in the bags the pet eats and still gains from (Pet::HaveInDiet, GetCurrentFoodBenefitLevel: the
// core's Feed Pet checks)
Item* FindPetFood(Player* bot, Pet* pet)
{
    auto const feeds = [pet](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        return proto && proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_FOOD &&
               pet->HaveInDiet(proto) && pet->GetCurrentFoodBenefitLevel(proto->ItemLevel) > 0;
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); feeds(item))
            return item;
    }

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
        {
            if (Item* item = bag->GetItemByPos(static_cast<uint8>(slot)); feeds(item))
                return item;
        }
    }

    return nullptr;
}
}  // namespace

// A bot granted Food keeps the old shortcut (happiness set to the maximum, no food). A persistent bot outside the
// handout share feeds its pet as a player does: Feed Pet on food from its bags that matches the pet's diet (upkeep
// WU7b); without such food the pet stays unhappy (less damage) until the bot buys some
bool FeedPetAction::isUseful()
{
    if (Upkeep::Granted(bot, UpkeepKind::Food))
        return true;

    Pet* pet = bot->GetPet();
    if (!pet || pet->getPetType() != HUNTER_PET || pet->GetHappinessState() == HAPPY ||
        pet->HasAura(SPELL_FEED_PET_EFFECT) || !bot->HasSpell(SPELL_FEED_PET))
        return false;

    time_t const now = time(nullptr);
    if (now < nextFoodSearch)
        return false;

    if (FindPetFood(bot, pet))
        return true;

    nextFoodSearch = now + FOOD_SEARCH_WAIT_SECONDS;
    return false;
}

bool FeedPetAction::Execute(Event /*event*/)
{
    if (Upkeep::Granted(bot, UpkeepKind::Food))
    {
        if (Pet* pet = bot->GetPet(); pet && pet->getPetType() == HUNTER_PET &&
            pet->GetHappinessState() != HAPPY)
        {
            pet->SetPower(POWER_HAPPINESS, pet->GetMaxPower(Powers(POWER_HAPPINESS)));
            Upkeep::Note(bot, UpkeepKind::Food, "pet_happiness");
        }

        return true;
    }

    Pet* pet = bot->GetPet();
    if (!pet || pet->getPetType() != HUNTER_PET)
        return false;

    Item* food = FindPetFood(bot, pet);
    return food && botAI->CastSpell(SPELL_FEED_PET, pet, food);
}

bool CastAutoShotAction::isUseful()
{
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
        return false;

    if (AI_VALUE(Unit*, "current target") && bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) &&
        bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL)->m_targets.GetUnitTargetGUID() ==
            AI_VALUE(Unit*, "current target")->GetGUID())
    {
        return false;
    }

    return AI_VALUE(uint32, "active spell") != AI_VALUE2(uint32, "spell id", getName());
}

bool CastDisengageAction::Execute(Event event)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    // can cast spell check passed in isUseful(); the leap goes the way the bot's back faces. G2a: where the bot turns
    // naturally it turns to the target first and leaps once the flick is over (the check counts as acted), else the
    // instant write as before
    if (!NaturalTurnForCast(botAI, target, 0.0f))
        return true;

    return CastSpellAction::Execute(event);
}

bool CastDisengageAction::isUseful()
{
    return !botAI->HasStrategy("trap weave", BOT_STATE_COMBAT);
}

Value<Unit*>* CastScareBeastCcAction::GetTargetValue()
{
    return context->GetValue<Unit*>("cc target", "scare beast");
}

bool CastScareBeastCcAction::Execute(Event /*event*/)
{
    return botAI->CastSpell("scare beast", GetTarget());
}

bool CastWingClipAction::isUseful()
{
    return CastSpellAction::isUseful() && !botAI->HasAura(spell, GetTarget());
}

std::vector<NextAction> CastWingClipAction::getPrerequisites()
{
    return {};
}
