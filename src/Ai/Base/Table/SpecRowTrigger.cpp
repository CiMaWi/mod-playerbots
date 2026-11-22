/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SpecRowTrigger.h"

#include "AiObjectContext.h"
#include "CureTriggers.h"
#include "GenericSpellActions.h"
#include "GenericTriggers.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "SharedDefines.h"
#include "SpecTableStrategy.h"
#include "SpellAuras.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "Value.h"

// SpecTable.h is pure; its dispel types are the core's
static_assert(static_cast<uint32>(spec::DispelType::Magic) == DISPEL_MAGIC);
static_assert(static_cast<uint32>(spec::DispelType::Curse) == DISPEL_CURSE);
static_assert(static_cast<uint32>(spec::DispelType::Disease) == DISPEL_DISEASE);
static_assert(static_cast<uint32>(spec::DispelType::Poison) == DISPEL_POISON);

namespace
{
constexpr float TARGET_RENEWAL_LIFE_S = 0.0f;  // CF.18: the life time a cast needs is its action's

// The bot's active power: rage, energy and runic power in points, mana in percent (INTERFACE §4)
uint32 PowerOf(Player* bot)
{
    Powers const power = bot->getPowerType();
    return CombatTally::PowerPoints(static_cast<CombatTally::PowerKind>(power), bot->GetPower(power),
                                    bot->GetMaxPower(power));
}

char const* HealthWord(spec::HealthTier tier)
{
    switch (tier)
    {
        case spec::HealthTier::Critical:
            return "critical health";
        case spec::HealthTier::Low:
            return "low health";
        case spec::HealthTier::Medium:
            return "medium health";
        default:
            return "almost full health";
    }
}
}  // namespace

namespace KindRule
{
Rule Make(PlayerbotAI* botAI, spec::Row const& row, std::string const& spell)
{
    Rule rule;
    if (!row.instead.empty())
    {
        rule.registered = std::string(row.instead);
        return rule;
    }

    auto const owned = [&](Trigger* trigger)
    {
        if (spell.empty())
        {
            delete trigger;
            rule.missing = true;
        }
        else
            rule.owned.reset(trigger);
    };
    uint32 const dispelType = static_cast<uint32>(row.dispelType);

    switch (row.kind)
    {
        case spec::Kind::Defensive:
            rule.registered = HealthWord(row.health);
            break;
        case spec::Kind::Dispel:
            if (row.dispelOn == spec::DispelOn::Self)
                owned(new NeedCureTrigger(botAI, spell, dispelType));
            else if (row.dispelOn == spec::DispelOn::Party)
                owned(new PartyMemberNeedCureTrigger(botAI, spell, dispelType));
            else
                owned(new TargetAuraDispelTrigger(botAI, spell, dispelType));
            break;
        case spec::Kind::Interrupt:
            if (row.who == spec::Who::Target)
                owned(new InterruptSpellTrigger(botAI, spell));
            else
                owned(new InterruptEnemyHealerTrigger(botAI, spell));
            break;
        case spec::Kind::MoveTool:
            rule.registered = row.purpose == spec::Purpose::Close ? "enemy out of melee" : "enemy is close";
            break;
        case spec::Kind::Heal:
            if (row.scope == spec::HealScope::Group)
                rule.registered = "group heal setting";
            else if (row.scope == spec::HealScope::GroupHard)
                rule.registered = "medium group heal setting";
            else
                rule.registered = std::string("party member ") + HealthWord(row.health);
            break;
        case spec::Kind::Form:
            owned(new BuffTrigger(botAI, spell));
            break;
        case spec::Kind::Renewal:
            if (row.subject == spec::Subject::Self)
                owned(new BuffTrigger(botAI, spell));
            else if (row.subject == spec::Subject::Party)
                owned(new BuffOnPartyTrigger(botAI, spell));
            else
                owned(new DebuffTrigger(botAI, spell, 1, row.subject == spec::Subject::TargetMine,
                                        TARGET_RENEWAL_LIFE_S));
            break;
        case spec::Kind::Proc:
            if (row.stacks > 1)
                owned(new HasAuraStackTrigger(botAI, spell, static_cast<int>(row.stacks)));
            else
                owned(new HasAuraTrigger(botAI, spell));
            break;
        case spec::Kind::Recover:
            rule.registered = row.mana == spec::ManaTier::Low      ? "low mana"
                              : row.mana == spec::ManaTier::Medium ? "medium mana"
                                                                   : "high mana";
            break;
        case spec::Kind::Aoe:
            rule.registered = row.count <= 2 ? "light aoe" : (row.count == 3 ? "medium aoe" : "high aoe");
            break;
        default:  // Cooldown, Builder, Spender, Filler: no rule; Opener, Control, Pet carry Instead
            break;
    }
    return rule;
}
}  // namespace KindRule

void SpecRowTrigger::Qualify(std::string const qual)
{
    Qualified::Qualify(qual);
    std::string_view strategyView;
    parsed = spec::ParseRowQualifier(qualifier, strategyView, index);
    strategyName = parsed ? std::string(strategyView) : std::string();
    displayName = parsed ? spec::RowQualifier(strategyName, index) : std::string("spec row");
}

// From the bot's combat engine, never the context: an alias is a second object under the same name (INTERFACE §6)
void SpecRowTrigger::Resolve()
{
    SpecTableStrategy* const found =
        parsed ? dynamic_cast<SpecTableStrategy*>(botAI->GetStrategy(strategyName, BOT_STATE_COMBAT)) : nullptr;
    if (!found || index >= found->Table().count)
    {
        strategy = nullptr;
        return;
    }
    if (found == strategy)
        return;

    strategy = found;
    row = &found->Table().rows[index];
    AiObjectContext* const context = botAI->GetAiObjectContext();

    std::string spell(row->effect);
    if (spell.empty())
        if (CastSpellAction* const cast = dynamic_cast<CastSpellAction*>(context->GetAction(std::string(row->action))))
            spell = cast->getSpell();
    rule = KindRule::Make(botAI, *row, spell);
    shared = rule.registered.empty() ? nullptr : context->GetTrigger(rule.registered);
    if (!rule.registered.empty() && !shared)
        rule.missing = true;
    sharedAnswer = shared ? strategy->AnswerFor(shared) : nullptr;
    ownedAnswer = false;

    for (uint8 t = 0; t < row->whenCount; ++t)
    {
        spec::Term const& term = row->when[t];
        termNames[t] = std::string(term.name);
        termTriggers[t] = term.atom == spec::Atom::Trigger ? context->GetTrigger(termNames[t]) : nullptr;
        termAnswers[t] = termTriggers[t] ? strategy->AnswerFor(termTriggers[t]) : nullptr;
    }
    currentTarget = context->GetValue<Unit*>("current target");
    combo = context->GetValue<uint8>("combo", "current target");
    attackers = context->GetValue<uint8>("my attacker count");
    displayName = spec::RowQualifier(strategyName, index) + " " + std::string(row->action);
}

bool SpecRowTrigger::IsActive()
{
    uint32 const build = botAI->GetCombatEngineBuild();
    if (!resolvedOnce || build != resolvedBuild)
    {
        resolvedOnce = true;
        resolvedBuild = build;
        Resolve();
    }
    if (!strategy)
        return false;

    uint32 const now = getMSTime();
    bool const live = spec::IsLive(
        *row, strategy->StandingAt(index, now), [this, now]() { return RuleHolds(now); },
        [this, now](spec::Term const& term) { return TermHolds(term, now); });
    if (live && TestConstraints::CombatTraced(botAI->testConstraints))
        strategy->CountLive(index);
    return live;
}

// An owned instance keeps its own interval through needCheck; a registered trigger is never asked by needCheck here
// (it would move the object's lastCheckTime for its other users): the table's shared answer holds between intervals
bool SpecRowTrigger::RuleHolds(uint32 now)
{
    if (rule.missing)
        return false;
    if (rule.owned)
    {
        if (rule.owned->needCheck(now))
            ownedAnswer = !!rule.owned->Check();
        return ownedAnswer;
    }
    if (shared)
        return sharedAnswer->Read(now, shared->CheckIntervalMs(), [this]() { return !!shared->Check(); });
    return true;
}

bool SpecRowTrigger::TermHolds(spec::Term const& term, uint32 now)
{
    size_t const t = static_cast<size_t>(&term - row->when.data());
    switch (term.atom)
    {
        case spec::Atom::HealthBelow:
            return bot->GetHealthPct() < static_cast<float>(term.number);
        case spec::Atom::TargetHealthBelow:
        {
            Unit* const target = currentTarget->Get();
            return target && target->GetHealthPct() < static_cast<float>(term.number);
        }
        case spec::Atom::PowerAtLeast:
        {
            if (PowerOf(bot) >= term.number)
                return true;
            // The fight tally's waiting evidence (CombatTally::PowerGate); the row's own answer is unchanged
            botAI->powerGate.Note(term.number, now, static_cast<uint16>(index));
            return false;
        }
        case spec::Atom::PowerBelow:
            return PowerOf(bot) < term.number;
        case spec::Atom::Combo:
            return combo->Get() >= term.number;
        case spec::Atom::Has:
        {
            if (term.number <= 1)
                return botAI->HasAura(termNames[t], bot);
            Aura* const aura = botAI->GetAura(termNames[t], bot);
            return aura && aura->GetStackAmount() >= term.number;
        }
        case spec::Atom::Lacks:
            return !botAI->HasAura(termNames[t], bot);
        case spec::Atom::TargetHas:
        case spec::Atom::TargetLacks:
        {
            Unit* const target = currentTarget->Get();
            if (!target)
                return false;
            bool const has = botAI->HasAura(termNames[t], target, false, term.owner == spec::Owner::Mine);
            return term.atom == spec::Atom::TargetHas ? has : !has;
        }
        case spec::Atom::Attackers:
            return attackers->Get() >= term.number;
        case spec::Atom::Solo:
            return !bot->GetGroup();
        case spec::Atom::Grouped:
            return bot->GetGroup() != nullptr;
        case spec::Atom::Trigger:
        {
            Trigger* const trigger = termTriggers[t];
            return trigger &&
                   termAnswers[t]->Read(now, trigger->CheckIntervalMs(), [trigger]() { return !!trigger->Check(); });
        }
        default:
            return false;
    }
}
