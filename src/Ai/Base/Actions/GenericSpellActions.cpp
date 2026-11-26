/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GenericSpellActions.h"

#include <ctime>
#include <unordered_set>

#include "ArenaCcSpells.h"
#include "CcTacticStrategy.h"
#include "Chat.h"
#include "Event.h"
#include "GenericBuffUtils.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "ItemUsable.h"
#include "ObjectDefines.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "WorldPacket.h"

using ai::buff::BuffBelowRefreshTarget;
using ai::buff::MakeAuraQualifierForBuff;
using ai::spell::HasSpellOrCategoryCooldown;

CastSpellAction::CastSpellAction(PlayerbotAI* botAI, std::string const spell)
    : Action(botAI, spell), spell(spell), range(botAI->GetRange("spell")) {}

bool CastSpellAction::Execute(Event /*event*/) { return botAI->CastSpell(spell, GetTarget()); }

bool CastKnockbackSpellAction::IsBlocked(Player* bot, uint32 noKnockbackGlyph)
{
    return bot->GetMap()->IsDungeon() && !bot->HasAura(noKnockbackGlyph);
}

bool CastKnockbackSpellAction::isUseful()
{
    return !IsBlocked(bot, _noKnockbackGlyph) && CastSpellAction::isUseful();
}

bool CastSpellAction::isUseful()
{
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
        return false;

    if (spell == "mount" && !bot->IsMounted() && !bot->IsInCombat())
        return true;

    if (spell == "mount" && bot->IsInCombat())
    {
        bot->Dismount();
        return false;
    }

    Unit* spellTarget = GetTarget();
    if (!spellTarget || !spellTarget->IsInWorld() || spellTarget->GetMapId() != bot->GetMapId())
        return false;

    // float combatReach = bot->GetCombatReach() + target->GetCombatReach();
    // if (!PlayerbotAI::IsRanged(bot))
    //     combatReach += 4.0f / 3.0f;

    return AI_VALUE2(bool, "spell cast useful", spell);
           // && ServerFacade::instance().GetDistance2d(bot, target) <= (range + combatReach);
}

bool CastSpellAction::isPossible()
{
    if (botAI->IsInVehicle() && !botAI->IsInVehicle(false, false, true))
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && botAI->HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Can cast spell failed. Vehicle. - bot name: {}", bot->GetName());

        return false;
    }

    if (spell == "mount" && !bot->IsMounted() && !bot->IsInCombat())
        return true;

    if (spell == "mount" && bot->IsInCombat())
    {
        if (!sPlayerbotAIConfig.LogInGroupOnly || (bot->GetGroup() && botAI->HasGameClientMaster()))
            LOG_DEBUG("playerbots", "Can cast spell failed. Mount. - bot name: {}", bot->GetName());

        bot->Dismount();
        return false;
    }

    // Spell* currentSpell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL); //not used, line marked for removal.
    return botAI->CanCastSpell(spell, GetTarget());
}

CastMeleeSpellAction::CastMeleeSpellAction(
    PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell)
{
    range = ATTACK_DISTANCE;
}

bool CastMeleeSpellAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !bot->IsWithinMeleeRange(target))
        return false;

    return CastSpellAction::isUseful();
}

CastMeleeDebuffSpellAction::CastMeleeDebuffSpellAction(
    PlayerbotAI* botAI, std::string const spell, bool isOwner, float needLifeTime) :
    CastDebuffSpellAction(botAI, spell, isOwner, needLifeTime)
{
    range = ATTACK_DISTANCE;
}

bool CastMeleeDebuffSpellAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !bot->IsWithinMeleeRange(target))
        return false;

    return CastDebuffSpellAction::isUseful();
}

bool CastAuraSpellAction::isUseful()
{
    if (!GetTarget() || !CastSpellAction::isUseful())
        return false;

    Aura* aura = botAI->GetAura(spell, GetTarget(), isOwner, checkDuration);
    return BuffBelowRefreshTarget(botAI, aura, beforeDuration);
}

bool CastBuffSpellAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !CastSpellAction::isUseful())
        return false;

    Aura* aura = botAI->GetAura(spell, target, isOwner, checkDuration);
    return BuffBelowRefreshTarget(botAI, aura, beforeDuration);
}

bool CastBuffSpellAction::Execute(Event /*event*/)
{
    botAI->forceRebuff.NoteBuffWork();

    return botAI->CastSpell(spell, GetTarget());
}

bool GroupBuffSpellAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !CastSpellAction::isUseful())
        return false;

    if (ai::buff::IsGroupVariantEnabled(bot, spell))
    {
        std::string const groupVariant = ai::buff::GroupVariantFor(spell);
        if (!groupVariant.empty() && !BuffBelowRefreshTarget(
                botAI, botAI->GetAura(groupVariant, target, isOwner, checkDuration), beforeDuration))
            return false;
    }

    Aura* aura = botAI->GetAura(spell, target, isOwner, checkDuration);
    return BuffBelowRefreshTarget(botAI, aura, beforeDuration);
}

bool GroupBuffSpellAction::Execute(Event /*event*/)
{
    botAI->forceRebuff.NoteBuffWork();

    std::string missingReagentGroupName;
    std::string const castName = ai::buff::UpgradeToGroupIfAppropriate(
        bot, botAI, spell, &missingReagentGroupName);

    if (!botAI->CastSpell(castName, GetTarget()))
        return false;

    if (!missingReagentGroupName.empty())
        ai::buff::TryAnnounceMissingBuffReagents(botAI, spell, missingReagentGroupName);

    return true;
}

CastEnchantItemMainHandAction::CastEnchantItemMainHandAction(
    PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell) {}

bool CastEnchantItemMainHandAction::Execute(Event /*event*/)
{
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    return item && botAI->CastSpell(spell, bot, item);
}

bool CastEnchantItemMainHandAction::isPossible()
{
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (!item || item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_MISC ||
        item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE ||
        item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
    {
        return false;
    }

    return botAI->CanCastSpell(spell, bot, item);
}

CastEnchantItemOffHandAction::CastEnchantItemOffHandAction(
    PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell) {}

bool CastEnchantItemOffHandAction::Execute(Event /*event*/)
{
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    return item && botAI->CastSpell(spell, bot, item);
}

bool CastEnchantItemOffHandAction::isPossible()
{
    Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!item || item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_MISC ||
        item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
    {
        return false;
    }

    return botAI->CanCastSpell(spell, bot, item);
}

CastHealingSpellAction::CastHealingSpellAction(PlayerbotAI* botAI, std::string const spell, uint8 estAmount,
                                               HealingManaEfficiency manaEfficiency, bool isOwner)
    : CastAuraSpellAction(botAI, spell, isOwner), manaEfficiency(manaEfficiency), estAmount(estAmount)
{
    range = botAI->GetRange("heal");
}

bool CastHealingSpellAction::isUseful() { return CastAuraSpellAction::isUseful(); }

bool CastAoeHealSpellAction::isUseful() { return CastSpellAction::isUseful(); }

CastCureSpellAction::CastCureSpellAction(
    PlayerbotAI* botAI, std::string const spell) : CastSpellAction(botAI, spell)
{
    range = botAI->GetRange("heal");
}

Value<Unit*>* CurePartyMemberAction::GetTargetValue()
{
    return context->GetValue<Unit*>("party member to dispel", dispelType);
}

Value<Unit*>* BuffOnPartyAction::GetTargetValue()
{
    return context->GetValue<Unit*>("party member without aura", spell);
}

Value<Unit*>* GroupBuffOnPartyAction::GetTargetValue()
{
    return context->GetValue<Unit*>("party member without aura", MakeAuraQualifierForBuff(spell));
}

CastShootAction::CastShootAction(
    PlayerbotAI* botAI) : CastSpellAction(botAI, "shoot"), shootSpellId(0)
{
    if (Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
    {
        spell = "shoot";

        switch (pItem->GetTemplate()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_GUN:
                spell += " gun";
                shootSpellId = 3018;
                break;
            case ITEM_SUBCLASS_WEAPON_BOW:
                spell += " bow";
                shootSpellId = 3018;
                break;
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                spell += " crossbow";
                shootSpellId = 3018;
                break;
            case ITEM_SUBCLASS_WEAPON_THROWN:
                spell = "throw";
                shootSpellId = 2764;
                break;
        }
    }
}

bool CastShootAction::isPossible()
{
    if (shootSpellId)
        return botAI->CanCastSpell(shootSpellId, GetTarget(), false);

    return CastSpellAction::isPossible();
}

bool CastShootAction::Execute(Event /*event*/)
{
    if (shootSpellId)
        return botAI->CastSpell(shootSpellId, GetTarget());

    return botAI->CastSpell(spell, GetTarget());
}

Value<Unit*>* CastDebuffSpellOnAttackerAction::GetTargetValue()
{
    return context->GetValue<Unit*>("attacker without aura", spell);
}

Value<Unit*>* CastDebuffSpellOnMeleeAttackerAction::GetTargetValue()
{
    return context->GetValue<Unit*>("melee attacker without aura", spell);
}

CastBuffSpellAction::CastBuffSpellAction(
    PlayerbotAI* botAI, std::string const spell, bool checkIsOwner, uint32 beforeDuration)
    : CastAuraSpellAction(botAI, spell, checkIsOwner, false, beforeDuration)
{
    range = botAI->GetRange("spell");
}

Value<Unit*>* CastSpellOnEnemyHealerAction::GetTargetValue()
{
    return context->GetValue<Unit*>("enemy healer target", spell);
}

Value<Unit*>* CastSnareSpellAction::GetTargetValue() { return context->GetValue<Unit*>("snare target", spell); }

Value<Unit*>* CastCrowdControlSpellAction::GetTargetValue() { return context->GetValue<Unit*>("cc target", getName()); }

bool CastCrowdControlSpellAction::Execute(Event /*event*/) { return botAI->CastSpell(getName(), GetTarget()); }

bool CastCrowdControlSpellAction::isPossible() { return botAI->CanCastSpell(getName(), GetTarget()); }

bool CastCrowdControlSpellAction::isUseful() { return true; }

std::string const CastProtectSpellAction::GetTargetName() { return "party member to protect"; }

bool CastProtectSpellAction::isUseful() { return GetTarget() && !botAI->HasAura(spell, GetTarget()); }

bool CastVehicleSpellAction::isPossible()
{
    uint32 spellId = AI_VALUE2(uint32, "vehicle spell id", spell);
    return botAI->CanCastVehicleSpell(spellId, GetTarget());
}

bool CastVehicleSpellAction::isUseful() { return botAI->IsInVehicle(false, true); }

bool CastVehicleSpellAction::Execute(Event /*event*/)
{
    uint32 spellId = AI_VALUE2(uint32, "vehicle spell id", spell);
    return botAI->CastVehicleSpell(spellId, GetTarget());
}

bool CastEveryManForHimselfAction::isPossible()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && !HasSpellOrCategoryCooldown(bot, spellId);
}

bool CastWillOfTheForsakenAction::isPossible()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && !HasSpellOrCategoryCooldown(bot, spellId);
}

Value<Unit*>* BuffOnMainTankAction::GetTargetValue() { return context->GetValue<Unit*>("main tank", spell); }

bool CastDebuffSpellAction::isUseful()
{
    Unit* target = GetTarget();
    if (!target || !target->IsAlive() || !target->IsInWorld())
        return false;

    if (!CastAuraSpellAction::isUseful())
        return false;

    // G4d: a CC that takes control (freezing trap, wyvern sting) on a player's unit controls it whatever its health;
    // the life time is for damage over time, roots, silences and PvE crowd control. G4d-c: only where the Cc tactic
    // acts (Playerbots.Tactics.Enable, Tactics.Cc.Scope); elsewhere, and with the tactics off (the default), the life
    // time applies to every debuff as before G4d
    if (target->GetCharmerOrOwnerPlayerOrPlayerItself() && botAI->TacticEnabledHere(TacticId::Cc))
    {
        SpellInfo const* const info = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", spell));
        if (info &&
            CcGroupLosesControl(static_cast<uint8>(GetDiminishingReturnsGroupForSpell(GetCcAuraSpell(info), false))))
            return true;
    }

    return (target->GetHealth() / AI_VALUE(float, "estimated group dps")) >= needLifeTime;
}
