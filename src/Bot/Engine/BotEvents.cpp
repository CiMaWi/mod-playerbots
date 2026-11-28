/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotEvents.h"

#include "Creature.h"
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "ThreatManager.h"
#include "Unit.h"

// The server's hooks that note events on bots (BotEvents.h). Each runs on the map thread of the unit it is about and
// only sets bits on a bot (PlayerbotAI::Notify, atomic): the bot's own update turns them into a wake. A bot on
// another map (a group member) is only ever noted, never read.
namespace
{
PlayerbotAI* BotOf(Unit const* unit)
{
    Player* const player = unit ? const_cast<Unit*>(unit)->ToPlayer() : nullptr;
    return player ? GET_PLAYERBOT_AI(player) : nullptr;
}

// The player a unit acts for: itself, or the owner of a pet, a totem, a guardian
PlayerbotAI* BotBehind(Unit const* unit)
{
    Player* const player = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    return player ? GET_PLAYERBOT_AI(player) : nullptr;
}

// The bots in the group of a player, the player left out
void NotifyGroup(Player const* player, uint32 events)
{
    Group const* const group = player->GetGroup();
    if (!group)
        return;
    for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* const member = ref->GetSource();
        if (!member || member == player)
            continue;
        if (PlayerbotAI* const botAI = GET_PLAYERBOT_AI(member))
            botAI->Notify(events);
    }
}

// The bots a creature fights (its threat list; a pet's or a totem's owner too)
void NotifyThreatened(Unit const* creature, uint32 events)
{
    for (ThreatReference const* ref : creature->GetThreatMgr().GetUnsortedThreatList())
        if (PlayerbotAI* const botAI = BotBehind(ref->GetVictim()))
            botAI->Notify(events);
}

class BotEventsUnitScript : public UnitScript
{
public:
    BotEventsUnitScript()
        : UnitScript("BotEventsUnitScript", true,
                     {UNITHOOK_ON_DAMAGE, UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE, UNITHOOK_ON_UNIT_DEATH})
    {
    }

    // Before the damage is taken: the victim's health is the health before it
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!victim || !damage)
            return;

        // A rage user's rage comes with its hit
        if (attacker && attacker != victim && attacker->IsPlayer() && attacker->getPowerType() == POWER_RAGE)
            if (PlayerbotAI* const botAI = BotOf(attacker))
                botAI->Notify(BotEvents::Hit);

        Player const* const player = victim->ToPlayer();
        if (!player)
            return;
        if (PlayerbotAI* const botAI = GET_PLAYERBOT_AI(const_cast<Player*>(player)))
            botAI->Notify(BotEvents::Damage);
        if (player->GetGroup() && BotEvents::CrossesHealthBand(victim->GetHealth(), damage, victim->GetMaxHealth()))
            NotifyGroup(player, BotEvents::AllyHurt);
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!unit || !aura || !unit->IsPlayer())
            return;
        if (PlayerbotAI* const botAI = BotOf(unit))
            botAI->Notify(aura->GetCasterGUID() == unit->GetGUID() ? BotEvents::AuraOwn : BotEvents::AuraOn);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (!unit || !aurApp || mode == AURA_REMOVE_BY_DEATH)
            return;
        if (unit->IsPlayer())
            if (PlayerbotAI* const botAI = BotOf(unit))
                botAI->Notify(BotEvents::AuraOff);

        // An aura a player had put on another unit: its caster on this map hears of its end
        Aura const* const aura = aurApp->GetBase();
        if (!aura || !aura->GetCasterGUID().IsPlayer() || aura->GetCasterGUID() == unit->GetGUID())
            return;
        if (PlayerbotAI* const botAI = BotOf(ObjectAccessor::GetUnit(*unit, aura->GetCasterGUID())))
            botAI->Notify(BotEvents::AuraGone);
    }

    // After the death: the dead unit's threat list is gone, so the killer and its group stand for who fought it (the
    // kill credit's hook in Playerbots.cpp notes the loot's recipients)
    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (!unit)
            return;

        if (Player const* const player = unit->ToPlayer())
        {
            if (PlayerbotAI* const botAI = GET_PLAYERBOT_AI(const_cast<Player*>(player)))
                botAI->Notify(BotEvents::Died);
            NotifyGroup(player, BotEvents::AllyHurt);
        }

        Player const* const killerPlayer = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!killerPlayer || killerPlayer == unit)
            return;
        if (PlayerbotAI* const botAI = GET_PLAYERBOT_AI(const_cast<Player*>(killerPlayer)))
            botAI->Notify(BotEvents::Kill);
        NotifyGroup(killerPlayer, BotEvents::Kill);
    }
};

class BotEventsSpellScript : public AllSpellScript
{
public:
    BotEventsSpellScript() : AllSpellScript("BotEventsSpellScript", {ALLSPELLHOOK_ON_PREPARE}) {}

    // A spell a unit begins of its own will (no triggered one): the bots a creature fights hear of it, and the bot a
    // hostile spell is aimed at
    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/) override
    {
        if (!spell || !caster || spell->IsTriggered())
            return;

        if (caster->IsCreature() && caster->IsInCombat() && !caster->GetCharmerOrOwnerPlayerOrPlayerItself())
        {
            NotifyThreatened(caster, BotEvents::EnemyCast);
            return;
        }

        Unit* const target = spell->m_targets.GetUnitTarget();
        if (!target || target == caster || !target->IsPlayer() || !caster->IsHostileTo(target))
            return;
        if (PlayerbotAI* const botAI = BotOf(target))
            botAI->Notify(BotEvents::EnemyCast);
    }
};
}  // namespace

void AddBotEventScripts()
{
    new BotEventsUnitScript();
    new BotEventsSpellScript();
}
