/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MageActions.h"
#include "NaturalTurn.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include "UseItemAction.h"
#include <cmath>

std::vector<NextAction> CastMoltenArmorAction::getAlternatives()
{
    if (!botAI->HasSpell("molten armor"))
        return NextAction::merge({ NextAction("mage armor") }, CastBuffSpellAction::getAlternatives());

    return CastBuffSpellAction::getAlternatives();
}

std::vector<NextAction> CastMageArmorAction::getAlternatives()
{
    if (!botAI->HasSpell("mage armor"))
        return NextAction::merge({ NextAction("ice armor") }, CastBuffSpellAction::getAlternatives());

    return CastBuffSpellAction::getAlternatives();
}

bool UseManaSapphireAction::isUseful()
{
    Player* bot = botAI->GetBot();
    return AI_VALUE2(bool, "combat", "self target") && bot->GetItemCount(33312, false) > 0;  // Mana Sapphire
}

bool UseManaEmeraldAction::isUseful()
{
    Player* bot = botAI->GetBot();
    return AI_VALUE2(bool, "combat", "self target") && bot->GetItemCount(22044, false) > 0;  // Mana Emerald
}

bool UseManaRubyAction::isUseful()
{
    Player* bot = botAI->GetBot();
    return AI_VALUE2(bool, "combat", "self target") && bot->GetItemCount(8008, false) > 0;  // Mana Ruby
}

bool UseManaCitrineAction::isUseful()
{
    Player* bot = botAI->GetBot();
    return AI_VALUE2(bool, "combat", "self target") && bot->GetItemCount(8007, false) > 0;  // Mana Citrine
}

bool UseManaJadeAction::isUseful()
{
    Player* bot = botAI->GetBot();
    return AI_VALUE2(bool, "combat", "self target") && bot->GetItemCount(5513, false) > 0;  // Mana Jade
}

bool UseManaAgateAction::isUseful()
{
    Player* bot = botAI->GetBot();
    return AI_VALUE2(bool, "combat", "self target") && bot->GetItemCount(5514, false) > 0;  // Mana Agate
}

bool CastFrostNovaAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsInWorld() || target->isFrozen() ||
        (target->ToCreature() &&
         target->ToCreature()->HasMechanicTemplateImmunity(1 << (MECHANIC_FREEZE - 1))))
    {
        return false;
    }

    return ServerFacade::instance().IsDistanceLessOrEqualThan(
        AI_VALUE2(float, "distance", GetTargetName()), 10.f);
}

bool CastConeOfColdAction::isUseful()
{
    bool facingTarget = AI_VALUE2(bool, "facing", "current target");
    bool targetClose = ServerFacade::instance().IsDistanceLessOrEqualThan(
        AI_VALUE2(float, "distance", GetTargetName()), 10.f);

    return facingTarget && targetClose;
}

bool CastDragonsBreathAction::isUseful()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    bool facingTarget = AI_VALUE2(bool, "facing", "current target");
    bool targetClose = bot->IsWithinCombatRange(target, 10.0f);
    return facingTarget && targetClose;
}

bool CastBlastWaveAction::isUseful()
{
    if (!CastKnockbackSpellAction::isUseful())
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    bool targetClose = bot->IsWithinCombatRange(target, 10.0f);
    return targetClose;
}

Unit* CastFocusMagicOnPartyAction::GetTarget()
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    Unit* casterDps = nullptr;
    Unit* healer = nullptr;
    Unit* target = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsAlive() || member->GetMap() != bot->GetMap() ||
            bot->GetDistance(member) > sPlayerbotAIConfig.SpellDistance || member->HasAura(54646))  // Focus Magic
        {
            continue;
        }

        if (member->getClass() == CLASS_MAGE)
            return member;

        if (!casterDps && PlayerbotAI::IsCaster(member) && PlayerbotAI::IsDps(member))
            casterDps = member;

        if (!healer && PlayerbotAI::IsHeal(member))
            healer = member;

        if (!target)
            target = member;
    }

    if (casterDps)
        return casterDps;

    if (healer)
        return healer;

    return target;
}

bool CastBlinkBackAction::Execute(Event event)
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target)
        return false;

    // G2a: where the bot turns naturally it turns its back on the target first and blinks once the flick is over (the
    // check counts as acted), else the instant write as before
    if (!NaturalTurnForCast(botAI, target, static_cast<float>(M_PI)))
        return true;

    return CastSpellAction::Execute(event);
}

// The highest known rank, from the "spell id" pick down, whose item the bot can use, or 0. A mage levelled down keeps
// its higher ranks: their items need a level it lacks, the trigger (FindFoodVisitor's CanUseItem) does not count
// them, and it would conjure them again at every trigger (NR6)
uint32 CastConjureConsumableAction::UsableRank()
{
    for (uint32 spellId = AI_VALUE2(uint32, "spell id", spell); spellId;
         spellId = sSpellMgr->GetPrevSpellInChain(spellId))
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || !bot->HasSpell(spellId))
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(spellInfo->Effects[0].ItemType);
        if (proto && bot->CanUseItem(proto) == EQUIP_ERR_OK)
            return spellId;
    }

    return 0;
}

// CastBuffSpellAction::Execute with the usable rank in place of the highest
bool CastConjureConsumableAction::Execute(Event /*event*/)
{
    botAI->forceRebuff.NoteBuffWork();

    uint32 const spellId = UsableRank();
    if (!spellId || !botAI->CastSpell(spellId, bot))
        return false;

    context->GetValue<time_t>("last spell cast time", spell)->Set(time(nullptr));
    return true;
}

bool CastConjureConsumableAction::isPossible()
{
    if (!CastBuffSpellAction::isPossible())
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(UsableRank());
    return spellInfo &&
           static_cast<int32>(bot->GetPower(POWER_MANA)) >= spellInfo->CalcPowerCost(bot, spellInfo->GetSchoolMask());
}
