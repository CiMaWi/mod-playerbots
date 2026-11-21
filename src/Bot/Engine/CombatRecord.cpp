/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatRecord.h"

#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "AllSpellScript.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "UnitScript.h"
#include "WorldLog.h"
#include "WorldLogTypes.h"

using CombatRecordRules::DealtMode;
using CombatRecordRules::KeyNumber;
using CombatRecordRules::RemoveMode;
using WorldLogFormat::DecisionKind;

static_assert(static_cast<uint8>(RemoveMode::Default) == AURA_REMOVE_BY_DEFAULT);
static_assert(static_cast<uint8>(RemoveMode::Cancel) == AURA_REMOVE_BY_CANCEL);
static_assert(static_cast<uint8>(RemoveMode::EnemySpell) == AURA_REMOVE_BY_ENEMY_SPELL);
static_assert(static_cast<uint8>(RemoveMode::Expire) == AURA_REMOVE_BY_EXPIRE);
static_assert(static_cast<uint8>(RemoveMode::Death) == AURA_REMOVE_BY_DEATH);
static_assert(DISPEL_MAGIC == 1 && DISPEL_CURSE == 2 && DISPEL_DISEASE == 3 && DISPEL_POISON == 4 &&
              DISPEL_ENRAGE == 9);

namespace
{
using CombatRecord::State;

// Threads: the hooks run on the map thread of the unit they are given; a traced bot's block is found in the registry
// (one mutex, taken only after Any and only for a player or a player's pet), its damage table is guarded by the
// block's own mutex, and WorldLog::Decision queues a row from any thread (it reads only the bot's guid).
std::mutex registryLock;
std::unordered_map<uint32, std::shared_ptr<State>> registry;

// An effect whose on row was written: off is written for the same bots, so the rows come in pairs (a stack's
// refresh calls the apply hook again on the same aura and writes nothing)
struct EffectKey
{
    Aura const* aura;
    Unit const* carrier;
    bool operator==(EffectKey const& other) const { return aura == other.aura && carrier == other.carrier; }
};
struct EffectKeyHash
{
    std::size_t operator()(EffectKey const& key) const
    {
        return std::hash<void const*>()(key.aura) ^ (std::hash<void const*>()(key.carrier) << 1);
    }
};
struct OpenEffect
{
    uint32 spell = 0;
    std::array<uint32, 2> bots{};
};
std::mutex effectLock;
std::unordered_map<EffectKey, OpenEffect, EffectKeyHash> openEffects;

// ModifyHealReceived's amount and spell for the OnHeal of the same call chain
thread_local CombatRecordRules::HealSlot healSlot;

constexpr std::string_view CHOICE_ON = "on";
constexpr std::string_view CHOICE_OFF = "off";
constexpr std::string_view CHOICE_START = "start";
constexpr std::string_view CHOICE_DONE = "done";
constexpr std::string_view CHOICE_STOPPED = "stopped";

struct Found
{
    std::shared_ptr<State> state;
    bool pet = false;

    explicit operator bool() const { return state != nullptr; }
};

uint32 KeyOf(Unit const* unit) { return unit ? unit->GetGUID().GetCounter() : 0; }

PerformanceData* HookPerf()
{
    return PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Combat hooks") : nullptr;
}

std::shared_ptr<State> ByKey(uint32 key)
{
    std::lock_guard<std::mutex> guard(registryLock);
    auto const it = registry.find(key);
    return it == registry.end() ? nullptr : it->second;
}

// A traced bot, or a unit a traced bot owns or charms (its pet)
Found Find(Unit* unit)
{
    if (!unit)
        return {};
    Player* const player = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!player)
        return {};
    std::shared_ptr<State> state = ByKey(player->GetGUID().GetCounter());
    if (!state)
        return {};
    return {std::move(state), unit != player};
}

void Forget(uint32 key)
{
    bool empty = false;
    {
        std::lock_guard<std::mutex> guard(registryLock);
        registry.erase(key);
        CombatRecord::Traced.store(static_cast<uint32>(registry.size()), std::memory_order_relaxed);
        empty = registry.empty();
    }
    if (!empty)
        return;
    std::lock_guard<std::mutex> guard(effectLock);
    openEffects.clear();
}

void EffectRow(Player* bot, std::string_view choice, std::string_view reason, float value, Aura const* aura,
               Unit const* carrier)
{
    SpellInfo const* info = aura->GetSpellInfo();
    WorldLogFormat::Alternatives alternatives;
    alternatives.Add({"stacks", static_cast<float>(aura->GetStackAmount())});
    alternatives.Add({"unit", KeyNumber(KeyOf(carrier))});
    alternatives.Add({CombatRecordRules::DispelWord(info->Dispel), static_cast<float>(info->Mechanic)});
    WorldLog::Decision(bot, DecisionKind::CombatEffect, choice, reason, value, alternatives, info->Id);
}

// The game's answer whether an interrupt can stop this cast
bool CanStop(Unit const* caster, SpellInfo const* info)
{
    return (info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) &&
           info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE &&
           !caster->m_spellImmune[IMMUNITY_MECHANIC].count(MECHANIC_INTERRUPT);
}

// One row per cast: the caster's (a traced bot, its pet), else the traced bot (or its pet's owner) a hostile caster
// aims at or attacks
void CastRow(Spell* spell, Unit* caster, SpellInfo const* info, std::string_view choice, bool bySelf)
{
    if (!spell || !caster || !info || spell->IsTriggered())
        return;
    // OnSpellPrepare comes after an instant's cast: only a cast with a cast time has a start
    if (choice == CHOICE_START && spell->GetCastTime() <= 0)
        return;

    Unit* const target = spell->m_targets.GetUnitTarget();
    Found found = Find(caster);
    std::string_view reason = "enemy";
    if (found)
    {
        if (!found.pet && choice == CHOICE_DONE)
            return;  // the bot's own finished casts are its combat_action rows
        reason = found.pet ? "pet" : "self";
    }
    else
    {
        if (target && target != caster && caster->IsHostileTo(target))
            found = Find(target);
        Unit* const victim = caster->GetVictim();
        if (!found && victim && caster->IsHostileTo(victim))
            found = Find(victim);
        if (!found)
            return;
    }

    WorldLogFormat::Alternatives alternatives;
    alternatives.Add({"unit", KeyNumber(KeyOf(caster))});
    alternatives.Add({"can_stop", CanStop(caster, info) ? 1.0f : 0.0f});
    if (choice == CHOICE_STOPPED)
        alternatives.Add({"by_self", bySelf ? 1.0f : 0.0f});
    else
        alternatives.Add({"target", KeyNumber(KeyOf(target))});
    float const value = choice == CHOICE_START ? CombatRecordRules::Seconds(spell->GetCastTime()) : 0.0f;
    WorldLog::Decision(found.state->bot, DecisionKind::CombatCast, choice, reason, value, alternatives, info->Id);
}

// A damage hook's amount before absorb and resist into the attacker's table (a traced bot or its pet)
void Dealt(Unit* attacker, Unit* target, uint32 amount, uint32 spell, DealtMode mode, bool periodic)
{
    if (!amount || !target)
        return;
    Found const found = Find(attacker);
    if (!found || !CombatRecordRules::CountsAsDamage(periodic, attacker->IsHostileTo(target)))
        return;

    std::lock_guard<std::mutex> guard(found.state->lock);
    found.state->dealt.Add(found.pet ? DealtMode::Pet : mode, spell, amount, KeyOf(target), getMSTime());
}

class CombatRecordUnitScript : public UnitScript
{
public:
    CombatRecordUnitScript()
        : UnitScript("CombatRecordUnitScript", true,
                     {UNITHOOK_ON_HEAL, UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_ON_DAMAGE, UNITHOOK_ON_AURA_APPLY,
                      UNITHOOK_ON_AURA_REMOVE, UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
                      UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK})
    {
    }

    void ModifyHealReceived(Unit* /*target*/, Unit* /*healer*/, uint32& heal, SpellInfo const* spellInfo) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        healSlot.Store(heal, spellInfo ? spellInfo->Id : 0);
    }

    void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        CombatRecordRules::HealSlot const slot = healSlot.Take();
        Found const found = Find(healer);
        if (!found || !receiver)
            return;

        Player* const bot = found.state->bot;
        std::string_view reason = "ally";
        if (receiver == bot)
            reason = "self";
        else if (receiver->GetCharmerOrOwnerPlayerOrPlayerItself() == bot)
            reason = "pet";
        WorldLogFormat::Alternatives alternatives;
        alternatives.Add({"over", static_cast<float>(CombatRecordRules::Overheal(slot.amount, gain))});
        alternatives.Add({"unit", KeyNumber(KeyOf(receiver))});
        alternatives.Add({"hp", static_cast<float>(CombatRecordRules::HealthPctBefore(receiver->GetHealth(), gain,
                                                                                      receiver->GetMaxHealth()))});
        WorldLog::Decision(bot, DecisionKind::CombatHeal, "heal", reason, static_cast<float>(gain), alternatives,
                           slot.spell);

        std::lock_guard<std::mutex> guard(found.state->lock);
        found.state->dealt.totals.healed += gain;
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        if (!damage)
            return;
        if (Found const by = Find(attacker))
        {
            std::lock_guard<std::mutex> guard(by.state->lock);
            (by.pet ? by.state->dealt.totals.pet : by.state->dealt.totals.done) += damage;
        }
        Found const on = Find(victim);
        if (on && !on.pet)
        {
            std::lock_guard<std::mutex> guard(on.state->lock);
            on.state->dealt.totals.taken += damage;
        }
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        Dealt(attacker, target, damage, 0, DealtMode::Melee, false);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        if (damage > 0)
            Dealt(attacker, target, static_cast<uint32>(damage), spellInfo ? spellInfo->Id : 0, DealtMode::Direct,
                  false);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage,
                                       SpellInfo const* spellInfo) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        Dealt(attacker, target, damage, spellInfo ? spellInfo->Id : 0, DealtMode::Periodic, true);
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        SpellInfo const* info = aura ? aura->GetSpellInfo() : nullptr;
        if (!unit || !info || info->IsPassive())
            return;

        Unit* const caster = aura->GetCaster();
        Found const byCaster = Find(caster);
        Found onCarrier = Find(unit);
        // the bot's or its pet's effect on the bot or its pet
        bool const own = byCaster && onCarrier && onCarrier.state == byCaster.state;
        if (own)
            onCarrier = {};
        std::array<std::pair<std::shared_ptr<State>, std::string_view>, 2> rows;
        std::size_t count = 0;
        if (byCaster)
            rows[count++] = {byCaster.state, own ? "self" : (caster->IsHostileTo(unit) ? "enemy" : "ally")};
        if (onCarrier)
            rows[count++] = {onCarrier.state, caster && !caster->IsHostileTo(unit) ? "from_ally" : "from_enemy"};
        // an enemy of a traced bot (or of its pet) on itself
        if (!count && caster == unit && !unit->IsControlledByPlayer())
            if (Found const attacked = Find(unit->GetVictim()))
                rows[count++] = {attacked.state, "enemy_own"};
        if (!count)
            return;

        {
            std::lock_guard<std::mutex> guard(effectLock);
            OpenEffect& open = openEffects[{aura, unit}];
            if (open.spell == info->Id)
                return;  // a refresh or a new stack of an open effect
            open.spell = info->Id;
            open.bots = {};
            for (std::size_t i = 0; i < count; ++i)
                open.bots[i] = rows[i].first->bot->GetGUID().GetCounter();
        }
        float const duration = CombatRecordRules::Seconds(aura->GetMaxDuration());
        for (std::size_t i = 0; i < count; ++i)
            EffectRow(rows[i].first->bot, CHOICE_ON, rows[i].second, duration, aura, unit);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        Aura const* aura = aurApp ? aurApp->GetBase() : nullptr;
        if (!unit || !aura)
            return;

        OpenEffect open;
        {
            std::lock_guard<std::mutex> guard(effectLock);
            auto const it = openEffects.find({aura, unit});
            if (it == openEffects.end())
                return;
            open = it->second;
            openEffects.erase(it);
        }
        std::string_view const reason = CombatRecordRules::OffWord(static_cast<RemoveMode>(mode));
        float const left = CombatRecordRules::Seconds(aura->GetDuration());
        for (uint32 const key : open.bots)
            if (key)
                if (std::shared_ptr<State> const state = ByKey(key))
                    EffectRow(state->bot, CHOICE_OFF, reason, left, aura, unit);
    }
};

class CombatRecordSpellScript : public AllSpellScript
{
public:
    CombatRecordSpellScript()
        : AllSpellScript("CombatRecordSpellScript",
                         {ALLSPELLHOOK_ON_PREPARE, ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL})
    {
    }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        CastRow(spell, caster, spellInfo, CHOICE_START, false);
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        CastRow(spell, caster, spellInfo, CHOICE_DONE, false);
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool bySelf) override
    {
        if (!CombatRecord::Any())
            return;
        PerfMonitorScope scope(HookPerf());
        CastRow(spell, caster, spellInfo, CHOICE_STOPPED, bySelf);
    }
};
}  // namespace

namespace CombatRecord
{
State& StateOf(PlayerbotAI* botAI)
{
    if (!botAI->combatRecord)
        botAI->combatRecord = std::make_shared<State>();
    return *botAI->combatRecord;
}

void Track(Player* bot)
{
    PlayerbotAI* const botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
        return;
    State& state = StateOf(botAI);
    if (state.tracked)
        return;

    state.bot = bot;
    state.tracked = true;
    std::lock_guard<std::mutex> guard(registryLock);
    registry[bot->GetGUID().GetCounter()] = botAI->combatRecord;
    Traced.store(static_cast<uint32>(registry.size()), std::memory_order_relaxed);
}

void Untrack(Player* bot)
{
    if (!bot)
        return;
    Forget(bot->GetGUID().GetCounter());
    if (PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot))
        botAI->combatRecord.reset();
}

void OnLogout(Player* bot) { Untrack(bot); }

void WriteDealt(Player* bot, State& state)
{
    CombatRecordRules::DealtTable table;
    {
        std::lock_guard<std::mutex> guard(state.lock);
        table = state.dealt;
        state.dealt.Clear();
    }
    auto const write = [bot](CombatRecordRules::DealtRow const& row)
    {
        WorldLog::Decision(
            bot, DecisionKind::CombatDealt, "dealt", CombatRecordRules::DEALT_WORDS[static_cast<std::size_t>(row.mode)],
            static_cast<float>(row.sum),
            {{"hits", static_cast<float>(row.hits)}, {"targets", static_cast<float>(row.targets.most)}}, row.spell);
    };
    for (std::size_t i = 0; i < table.used; ++i)
        write(table.rows[i]);
    if (table.HasRest())
        write(table.rows[CombatRecordRules::DEALT_SPELLS]);

    CombatRecordRules::Totals const& totals = table.totals;
    WorldLog::Decision(bot, DecisionKind::CombatDealt, "total", "", static_cast<float>(totals.done),
                       {{"pet", static_cast<float>(totals.pet)},
                        {"taken", static_cast<float>(totals.taken)},
                        {"healed", static_cast<float>(totals.healed)}});
}
}  // namespace CombatRecord

void AddSC_combat_record()
{
    new CombatRecordUnitScript();
    new CombatRecordSpellScript();
}
