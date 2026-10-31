/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENAINTERRUPTSPELLS_H
#define _PLAYERBOT_ARENAINTERRUPTSPELLS_H

#include <algorithm>

#include "ArenaCcSpells.h"
#include "ArenaInterrupt.h"
#include "PvpTestCounters.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

// The game side of the Interrupt tactic (G5b): what a player sees of a cast (its bar and its spell's tooltip, DT.12)
// as ArenaInterrupt's plain facts. Map thread only.

// The spell's kind by its effects: a loss-of-control aura (a CC DR group, as the harness counts CC), a heal (a heal
// effect, a heal over time, an absorb, or a periodic trigger of a helpful channel), damage (direct, periodic, a
// periodic trigger of a harmful channel)
inline ArenaInterrupt::SpellFacts InterruptSpellFactsOf(SpellInfo const* info)
{
    ArenaInterrupt::SpellFacts facts;
    if (!info)
        return facts;

    facts.Controls =
        IsPvpTestCcGroup(static_cast<uint32>(GetDiminishingReturnsGroupForSpell(GetCcAuraSpell(info), false)));
    bool const positive = info->IsPositive();
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        switch (effect.Effect)
        {
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_MECHANICAL:
            case SPELL_EFFECT_HEAL_PCT:
                facts.Heals = true;
                break;
            default:
                break;
        }

        if (CcAreaDetail::EffectDamages(effect))
            facts.Damages = true;

        if (!effect.IsAura())
            continue;

        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_PERIODIC_HEAL:
            case SPELL_AURA_OBS_MOD_HEALTH:
            case SPELL_AURA_SCHOOL_ABSORB:
                facts.Heals = true;
                break;
            case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                (positive ? facts.Heals : facts.Damages) = true;
                break;
            default:
                break;
        }
    }

    return facts;
}

// How the bot's own spell stops a cast: a kick (an interrupt effect or a silence), a stun, or not at all
enum class InterruptUse : uint8
{
    None,
    Kick,
    Stun
};

inline InterruptUse InterruptUseOf(SpellInfo const* info)
{
    if (!info)
        return InterruptUse::None;

    bool stun = false;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (effect.Effect == SPELL_EFFECT_INTERRUPT_CAST)
            return InterruptUse::Kick;

        if (effect.IsAura() &&
            (effect.ApplyAuraName == SPELL_AURA_MOD_SILENCE || effect.ApplyAuraName == SPELL_AURA_MOD_PACIFY_SILENCE))
            return InterruptUse::Kick;

        if (effect.IsAura() && effect.ApplyAuraName == SPELL_AURA_MOD_STUN)
            stun = true;
    }

    return stun ? InterruptUse::Stun : InterruptUse::None;
}

// G5c-c: a kick aimed at one enemy: its interrupt or silence effect hits the spell's enemy target (Kick, Pummel,
// Shield Bash, Counterspell, Mind Freeze, Strangulate, Silence, Silencing Shot, Wind Shear). Arcane Torrent silences
// around the caster and is cast as a self buff (RacialsStrategy), never at an enemy: no
inline bool InterruptKickAimed(SpellInfo const* info)
{
    if (!info)
        return false;

    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        bool const kick = effect.Effect == SPELL_EFFECT_INTERRUPT_CAST ||
                          (effect.IsAura() && (effect.ApplyAuraName == SPELL_AURA_MOD_SILENCE ||
                                               effect.ApplyAuraName == SPELL_AURA_MOD_PACIFY_SILENCE));
        if (kick && effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY)
            return true;
    }

    return false;
}

// The enemy's cast bar as a player sees it: a cast with a cast time or a channel with a duration
struct InterruptBar
{
    Spell const* Cast = nullptr;
    SpellInfo const* Info = nullptr;
    uint32 CastMs = 0;
    uint32 ElapsedMs = 0;
};

inline bool ReadInterruptBar(Unit* unit, InterruptBar& bar)
{
    if (!unit)
        return false;

    Spell* const cast = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (cast && cast->getState() == SPELL_STATE_PREPARING && cast->GetCastTime() > 0 && cast->GetSpellInfo())
    {
        bar.Cast = cast;
        bar.Info = cast->GetSpellInfo();
        bar.CastMs = static_cast<uint32>(cast->GetCastTime());
        bar.ElapsedMs =
            static_cast<uint32>(std::clamp(cast->GetCastTime() - cast->GetCastTimeRemaining(), 0, cast->GetCastTime()));
        return true;
    }

    Spell* const channel = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    int32 const duration = channel && channel->GetSpellInfo() ? channel->GetSpellInfo()->GetDuration() : 0;
    if (channel && channel->getState() == SPELL_STATE_CASTING && duration > 0)
    {
        bar.Cast = channel;
        bar.Info = channel->GetSpellInfo();
        bar.CastMs = static_cast<uint32>(duration);
        bar.ElapsedMs = static_cast<uint32>(std::clamp(duration - channel->GetCastTimeRemaining(), 0, duration));
        return true;
    }

    return false;
}

// The health of the cast's target (a heal's friend; the caster itself when it has none)
inline uint8 InterruptBarTargetHpPct(Unit* caster, InterruptBar const& bar)
{
    Unit* target = bar.Cast ? bar.Cast->m_targets.GetUnitTarget() : nullptr;
    if (!target)
        target = caster;

    return static_cast<uint8>(std::clamp(static_cast<int32>(target->GetHealthPct() + 0.5f), 0, 100));
}

// A healer class the bot can tell by sight (DT.12)
inline bool InterruptHealerClass(uint8 playerClass)
{
    return playerClass == CLASS_PRIEST || playerClass == CLASS_DRUID || playerClass == CLASS_SHAMAN ||
           playerClass == CLASS_PALADIN;
}

// G5c-c: the cast time a bot expects of a healer class's next cast heal before it has seen one (the class's usual
// heal: Flash Heal, Regrowth, Chain Heal, Flash of Light; 0: no healer class)
constexpr uint32 InterruptHealerCastPriorMs(uint8 playerClass)
{
    switch (playerClass)
    {
        case CLASS_PRIEST:
        case CLASS_PALADIN:
            return 1500;
        case CLASS_DRUID:
            return 2000;
        case CLASS_SHAMAN:
            return 2500;
        default:
            return 0;
    }
}

#endif
