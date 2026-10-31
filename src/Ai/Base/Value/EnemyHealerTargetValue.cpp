/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EnemyHealerTargetValue.h"

#include <algorithm>

#include "ArenaInterrupt.h"
#include "ArenaInterruptSpells.h"
#include "FocusState.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Timer.h"

Unit* EnemyHealerTargetValue::Calculate()
{
    std::string const spell = qualifier;
    // G5b: a bot that knows the Interrupt tactic weighs the casts of the fight's enemies; else the first helpful cast
    if (botAI->TacticEnabledHere(TacticId::Interrupt) && botAI->TacticActive(TacticId::Interrupt))
        return BestInterruptTarget(spell);

    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit == target)
            continue;

        if (ServerFacade::instance().GetDistance2d(bot, unit) > botAI->GetRange("spell"))
            continue;

        if (!botAI->IsInterruptableSpellCasting(unit, spell))
            continue;

        Spell* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (spell && spell->m_spellInfo->IsPositive())
            return unit;

        spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (spell && spell->m_spellInfo->IsPositive())
            return unit;
    }

    return nullptr;
}

Unit* EnemyHealerTargetValue::BestInterruptTarget(std::string const& spell)
{
    Unit* const target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    uint32 const nowMs = getMSTime();
    InterruptConfig const& config = sPlayerbotAIConfig.TacticInterrupt;
    if (static_cast<int32>(nowMs - bestUntilMs) >= 0)
    {
        // At most TACTIC_FIGHT_ENEMIES_MAX units, every HealerCacheMs; the target's casts are the plain trigger's
        bestUntilMs = nowMs + std::max<uint32>(1, config.HealerCacheMs);
        bestGuid = ObjectGuid::Empty;
        uint32 bestScore = 0;
        float const range = botAI->GetRange("spell");
        // G4e: a cast on the bot's focus target scores FocusTargetKickBonus over the others of its kind
        FocusTargetState const& focus = botAI->GetFocusState().Target;
        uint32 const focusBonus = sPlayerbotAIConfig.TacticFocus.FocusTargetKickBonus;
        for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("fight enemies")->Get())
        {
            Unit* const unit = botAI->GetUnit(guid);
            if (!unit || unit == target || ServerFacade::instance().GetDistance2d(bot, unit) > range)
                continue;

            InterruptBar bar;
            if (!ReadInterruptBar(unit, bar) || !botAI->IsInterruptableSpellCasting(unit, spell))
                continue;

            uint32 const score = FocusKickScore(
                ArenaInterrupt::Score(ArenaInterrupt::Classify(InterruptSpellFactsOf(bar.Info)),
                                      InterruptBarTargetHpPct(unit, bar), static_cast<uint8>(config.UrgentHpPct)),
                focus.Is(guid), focusBonus);
            if (score > bestScore)
            {
                bestScore = score;
                bestGuid = guid;
            }
        }
    }

    if (bestGuid.IsEmpty())
        return nullptr;

    Unit* const unit = botAI->GetUnit(bestGuid);
    if (!unit || unit == target || !botAI->IsInterruptableSpellCasting(unit, spell))
        return nullptr;

    return botAI->DecideInterrupt(unit) ? unit : nullptr;
}

Unit* FocusTargetValue::Calculate()
{
    FocusTargetState const& focus = botAI->GetFocusState().Target;
    if (!focus.Pick.Guid)
        return nullptr;

    Unit* const unit = botAI->GetUnit(ObjectGuid(focus.Pick.Guid));
    return unit && unit->IsInWorld() && unit->IsAlive() ? unit : nullptr;
}
