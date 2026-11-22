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
#include "CombatTrace.h"
#include "EventWanted.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "Pet.h"
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

// combat_known's classifier reads the core's numbers through CombatKinds::Data
namespace KindData = CombatKinds::Data;
static_assert(KindData::EFFECT_HEAL == SPELL_EFFECT_HEAL && KindData::EFFECT_LEAP == SPELL_EFFECT_LEAP &&
              KindData::EFFECT_ENERGIZE == SPELL_EFFECT_ENERGIZE && KindData::EFFECT_DISPEL == SPELL_EFFECT_DISPEL &&
              KindData::EFFECT_JUMP == SPELL_EFFECT_JUMP && KindData::EFFECT_JUMP_DEST == SPELL_EFFECT_JUMP_DEST &&
              KindData::EFFECT_THREAT == SPELL_EFFECT_THREAT &&
              KindData::EFFECT_HEAL_MAX_HEALTH == SPELL_EFFECT_HEAL_MAX_HEALTH &&
              KindData::EFFECT_INTERRUPT_CAST == SPELL_EFFECT_INTERRUPT_CAST &&
              KindData::EFFECT_CHARGE == SPELL_EFFECT_CHARGE &&
              KindData::EFFECT_DISPEL_MECHANIC == SPELL_EFFECT_DISPEL_MECHANIC &&
              KindData::EFFECT_ATTACK_ME == SPELL_EFFECT_ATTACK_ME &&
              KindData::EFFECT_PULL_TOWARDS == SPELL_EFFECT_PULL_TOWARDS &&
              KindData::EFFECT_MODIFY_THREAT_PERCENT == SPELL_EFFECT_MODIFY_THREAT_PERCENT &&
              KindData::EFFECT_STEAL_BENEFICIAL_BUFF == SPELL_EFFECT_STEAL_BENEFICIAL_BUFF &&
              KindData::EFFECT_HEAL_PCT == SPELL_EFFECT_HEAL_PCT &&
              KindData::EFFECT_ENERGIZE_PCT == SPELL_EFFECT_ENERGIZE_PCT &&
              KindData::EFFECT_LEAP_BACK == SPELL_EFFECT_LEAP_BACK &&
              KindData::EFFECT_PULL_TOWARDS_DEST == SPELL_EFFECT_PULL_TOWARDS_DEST &&
              KindData::EFFECT_CHARGE_DEST == SPELL_EFFECT_CHARGE_DEST);
static_assert(KindData::AURA_MOD_POSSESS == SPELL_AURA_MOD_POSSESS &&
              KindData::AURA_MOD_CONFUSE == SPELL_AURA_MOD_CONFUSE &&
              KindData::AURA_MOD_CHARM == SPELL_AURA_MOD_CHARM && KindData::AURA_MOD_FEAR == SPELL_AURA_MOD_FEAR &&
              KindData::AURA_PERIODIC_HEAL == SPELL_AURA_PERIODIC_HEAL &&
              KindData::AURA_MOD_THREAT == SPELL_AURA_MOD_THREAT && KindData::AURA_MOD_TAUNT == SPELL_AURA_MOD_TAUNT &&
              KindData::AURA_MOD_STUN == SPELL_AURA_MOD_STUN &&
              KindData::AURA_OBS_MOD_HEALTH == SPELL_AURA_OBS_MOD_HEALTH &&
              KindData::AURA_OBS_MOD_POWER == SPELL_AURA_OBS_MOD_POWER &&
              KindData::AURA_PERIODIC_ENERGIZE == SPELL_AURA_PERIODIC_ENERGIZE &&
              KindData::AURA_MOD_ROOT == SPELL_AURA_MOD_ROOT && KindData::AURA_MOD_SILENCE == SPELL_AURA_MOD_SILENCE &&
              KindData::AURA_MOD_INCREASE_SPEED == SPELL_AURA_MOD_INCREASE_SPEED &&
              KindData::AURA_SCHOOL_IMMUNITY == SPELL_AURA_SCHOOL_IMMUNITY &&
              KindData::AURA_DAMAGE_IMMUNITY == SPELL_AURA_DAMAGE_IMMUNITY &&
              KindData::AURA_MOD_PARRY_PERCENT == SPELL_AURA_MOD_PARRY_PERCENT &&
              KindData::AURA_MOD_DODGE_PERCENT == SPELL_AURA_MOD_DODGE_PERCENT &&
              KindData::AURA_MOD_PACIFY_SILENCE == SPELL_AURA_MOD_PACIFY_SILENCE &&
              KindData::AURA_FEIGN_DEATH == SPELL_AURA_FEIGN_DEATH &&
              KindData::AURA_SCHOOL_ABSORB == SPELL_AURA_SCHOOL_ABSORB &&
              KindData::AURA_MECHANIC_IMMUNITY == SPELL_AURA_MECHANIC_IMMUNITY &&
              KindData::AURA_MOD_DAMAGE_PERCENT_TAKEN == SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN &&
              KindData::AURA_MOD_TOTAL_THREAT == SPELL_AURA_MOD_TOTAL_THREAT &&
              KindData::AURA_MOD_SPEED_ALWAYS == SPELL_AURA_MOD_SPEED_ALWAYS &&
              KindData::AURA_MOD_SPEED_NOT_STACK == SPELL_AURA_MOD_SPEED_NOT_STACK);
static_assert(KindData::MECHANIC_CHARM == MECHANIC_CHARM && KindData::MECHANIC_DISORIENTED == MECHANIC_DISORIENTED &&
              KindData::MECHANIC_FEAR == MECHANIC_FEAR && KindData::MECHANIC_ROOT == MECHANIC_ROOT &&
              KindData::MECHANIC_SILENCE == MECHANIC_SILENCE && KindData::MECHANIC_SLEEP == MECHANIC_SLEEP &&
              KindData::MECHANIC_STUN == MECHANIC_STUN && KindData::MECHANIC_FREEZE == MECHANIC_FREEZE &&
              KindData::MECHANIC_KNOCKOUT == MECHANIC_KNOCKOUT && KindData::MECHANIC_POLYMORPH == MECHANIC_POLYMORPH &&
              KindData::MECHANIC_BANISH == MECHANIC_BANISH && KindData::MECHANIC_SHACKLE == MECHANIC_SHACKLE &&
              KindData::MECHANIC_TURN == MECHANIC_TURN && KindData::MECHANIC_HORROR == MECHANIC_HORROR &&
              KindData::MECHANIC_SAPPED == MECHANIC_SAPPED);
static_assert(KindData::MECHANIC_SNARE == MECHANIC_SNARE &&
              KindData::EFFECT_PERSISTENT_AREA_AURA == SPELL_EFFECT_PERSISTENT_AREA_AURA);
static_assert(KindData::DISPEL_TYPE_MAGIC == DISPEL_MAGIC && KindData::DISPEL_TYPE_CURSE == DISPEL_CURSE &&
              KindData::DISPEL_TYPE_DISEASE == DISPEL_DISEASE && KindData::DISPEL_TYPE_POISON == DISPEL_POISON);
static_assert(KindData::STATE_HEALTHLESS_20 == AURA_STATE_HEALTHLESS_20_PERCENT &&
              KindData::STATE_HEALTHLESS_35 == AURA_STATE_HEALTHLESS_35_PERCENT);
static_assert(POWER_MANA == 0 && POWER_RAGE == 1 && POWER_FOCUS == 2 && POWER_ENERGY == 3 && POWER_RUNIC_POWER == 6);
static_assert(CombatKinds::SHARE_WORDS.size() == CombatTally::SHARE_COUNT &&
              static_cast<std::size_t>(CombatTally::Share::Idle) == 0 &&
              static_cast<std::size_t>(CombatTally::Share::Acting) == 4);
static_assert(MAX_RUNES == 6);

namespace
{
using CombatRecord::State;

// Threads: the hooks run on the map thread of the unit they are given; a traced bot's block is found in the registry
// (one mutex, taken only after Any and only for a player or a player's pet), its damage table is guarded by the
// block's own mutex, and WorldLog::Decision queues a row from any thread by the bot's key: a hook keeps no Player of
// another thread. Lock order: registryLock before effectLock (Forget only); State::lock is taken alone.
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
    // the traced bot itself, the owner the hook's own unit names (alive on the hook's thread)
    Player* player = nullptr;

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
    return {std::move(state), unit != player, player};
}

// The open effects go with the last traced bot, under the registry's lock: a Track in between cannot lose its entries,
// and an apply that passed Any() before finds Any() false under effectLock (OnAuraApply)
void Forget(uint32 key)
{
    std::lock_guard<std::mutex> guard(registryLock);
    registry.erase(key);
    CombatRecord::Traced.store(static_cast<uint32>(registry.size()), std::memory_order_relaxed);
    if (!registry.empty())
        return;
    std::lock_guard<std::mutex> effects(effectLock);
    openEffects.clear();
}

void EffectRow(uint32 bot, std::string_view choice, std::string_view reason, float value, Aura const* aura,
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

    // the target by its guid: the stored pointer can dangle when the target left the map before a cancel (a caster's
    // death interrupts a cast whose target is gone)
    Unit* const target = ObjectAccessor::GetUnit(*caster, spell->m_targets.GetUnitTargetGUID());
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
    WorldLog::Decision(found.state->key, DecisionKind::CombatCast, choice, reason, value, alternatives, info->Id);
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

        Player* const bot = found.player;
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
            if (!CombatRecord::Any())
                return;  // the last traced bot went (Forget) after this hook's first test: no stale entry
            OpenEffect& open = openEffects[{aura, unit}];
            if (open.spell == info->Id)
                return;  // a refresh or a new stack of an open effect
            open.spell = info->Id;
            open.bots = {};
            for (std::size_t i = 0; i < count; ++i)
                open.bots[i] = rows[i].first->key;
        }
        float const duration = CombatRecordRules::Seconds(aura->GetMaxDuration());
        for (std::size_t i = 0; i < count; ++i)
            EffectRow(rows[i].first->key, CHOICE_ON, rows[i].second, duration, aura, unit);
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
        // only a bot still traced (its key may name a bot of another map, even one logging out: no Player is read)
        for (uint32 const key : open.bots)
            if (key && ByKey(key))
                EffectRow(key, CHOICE_OFF, reason, left, aura, unit);
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

PerformanceData* RecordPerf()
{
    return PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "Combat record") : nullptr;
}

constexpr std::string_view CHOICE_SAMPLE_SELF = "self";
constexpr std::string_view CHOICE_ENEMY = "enemy";
constexpr std::string_view CHOICE_STACK = "stack";
constexpr std::string_view CHOICE_PET = "pet";
constexpr std::string_view CHOICE_PULL = "pull";
constexpr std::string_view CHOICE_HAS = "has";
constexpr std::string_view CHOICE_WEAPON = "weapon";

CombatKinds::Victim VictimOf(Player* bot, Unit* victim)
{
    using CombatKinds::Victim;
    if (!victim)
        return Victim::None;
    if (victim == bot)
        return Victim::Me;
    Player* const owner = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (owner == bot)
        return Victim::Pet;
    if (owner && bot->IsInSameGroupWith(owner))
        return Victim::Mate;
    return Victim::Other;
}

// An enemy row: its health, the yards to the bot, whom it attacks (a player's key) and its entry
void EnemyRow(Player* bot, Unit* enemy, bool target, bool attacker)
{
    Unit* const victim = enemy->GetVictim();
    WorldLogFormat::Alternatives alternatives;
    alternatives.Add({"dist", bot->GetDistance(enemy)});
    alternatives.Add({CombatKinds::VictimWord(VictimOf(bot, victim)),
                      victim && victim->IsPlayer() ? KeyNumber(KeyOf(victim)) : 0.0f});
    alternatives.Add({"entry", static_cast<float>(enemy->GetEntry())});
    WorldLog::Decision(bot, DecisionKind::CombatSample, CHOICE_ENEMY, CombatKinds::EnemyReason(target, attacker),
                       enemy->GetHealthPct(), alternatives, KeyOf(enemy));
}

bool AttacksBot(Unit* enemy, Player* bot, Pet* pet)
{
    Unit* const victim = enemy->GetVictim();
    return victim && (victim == bot || (pet && victim == pet));
}

// The samples of one second: self, the enemies (the target first, then those attacking the bot or its pet, 8 rows at
// most, from the bot's combat references: no search of the map), the bot's own stacking effects, its pet
void Sample(Player* bot, State& state, CombatTally::Share share)
{
    Unit* target = bot->GetVictim();
    if (!target)
        target = bot->GetSelectedUnit();
    if (target == bot)
        target = nullptr;

    std::uint64_t taken = 0;
    {
        std::lock_guard<std::mutex> guard(state.lock);
        taken = state.dealt.totals.taken;
    }
    std::uint64_t const since = CombatKinds::Since(taken, state.takenAtSample);
    state.takenAtSample = taken;

    Powers const power = bot->getPowerType();
    WorldLogFormat::Alternatives self;
    self.Add({CombatKinds::PowerWord(power), static_cast<float>(CombatRecord::PowerOf(bot))});
    self.Add({"taken", static_cast<float>(since)});
    if (bot->getClass() == CLASS_DEATH_KNIGHT)
    {
        std::array<uint32, MAX_RUNES> cooldowns{};
        for (uint8 i = 0; i < MAX_RUNES; ++i)
            cooldowns[i] = bot->GetRuneCooldown(i);
        self.Add({"extra", static_cast<float>(CombatKinds::RuneMask(cooldowns))});
    }
    else if (bot->getClass() == CLASS_ROGUE || bot->getClass() == CLASS_DRUID)
        self.Add({"extra", static_cast<float>(bot->GetComboPoints())});
    WorldLog::Decision(bot, DecisionKind::CombatSample, CHOICE_SAMPLE_SELF,
                       CombatKinds::SHARE_WORDS[static_cast<std::size_t>(share)], bot->GetHealthPct(), self,
                       KeyOf(target));

    Pet* const pet = bot->GetPet();
    std::size_t rows = 0;
    if (target && target->IsAlive() && bot->IsHostileTo(target))
    {
        EnemyRow(bot, target, true, AttacksBot(target, bot, pet));
        ++rows;
    }
    auto const attackers = [&](auto const& refs)
    {
        for (auto const& [guid, ref] : refs)
        {
            if (rows >= CombatKinds::ENEMY_ROWS)
                return;
            Unit* const enemy = ref->GetOther(bot);
            if (!enemy || enemy == target || !enemy->IsAlive() || !AttacksBot(enemy, bot, pet))
                continue;
            EnemyRow(bot, enemy, false, true);
            ++rows;
        }
    };
    CombatManager const& combat = bot->GetCombatManager();
    attackers(combat.GetPvECombatRefs());
    attackers(combat.GetPvPCombatRefs());

    ObjectGuid const own = bot->GetGUID();
    for (auto const& [spellId, application] : bot->GetAppliedAuras())
    {
        Aura const* const aura = application->GetBase();
        if (aura->GetStackAmount() <= 1 || aura->GetCasterGUID() != own)
            continue;
        WorldLog::Decision(bot, DecisionKind::CombatSample, CHOICE_STACK, "",
                           static_cast<float>(aura->GetStackAmount()),
                           {{"left", CombatRecordRules::Seconds(aura->GetDuration())}}, spellId);
    }

    if (pet && pet->IsInWorld())
        WorldLog::Decision(bot, DecisionKind::CombatSample, CHOICE_PET, "", pet->GetHealthPct(),
                           {{"power", static_cast<float>(CombatRecord::PowerOf(pet))}}, pet->GetEntry());
}

// Whom an effect lands on, from its first implicit target
CombatKinds::Aim AimOf(SpellEffectInfo const& effect)
{
    using CombatKinds::Aim;
    if (effect.TargetA.GetTarget() == TARGET_UNIT_CASTER)
        return Aim::Self;
    switch (effect.TargetA.GetCheckType())
    {
        case TARGET_CHECK_ENEMY:
            return Aim::Enemy;
        case TARGET_CHECK_ALLY:
        case TARGET_CHECK_PARTY:
        case TARGET_CHECK_RAID:
        case TARGET_CHECK_RAID_CLASS:
            return Aim::Ally;
        default:
            return effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_ANY ? Aim::Any : Aim::Other;
    }
}

// The classifier's input from the spell data (the bot as the caster, for an amount's sign)
CombatKinds::SpellFacts FactsOf(Player* bot, SpellInfo const* info)
{
    CombatKinds::SpellFacts facts;
    facts.mechanic = info->Mechanic;
    facts.durationMs = info->GetDuration();
    facts.cooldownMs = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
    facts.targetAuraState = info->TargetAuraState;
    for (std::size_t i = 0; i < std::min<std::size_t>(MAX_SPELL_EFFECTS, CombatKinds::EFFECTS); ++i)
    {
        SpellEffectInfo const& effect = info->Effects[i];
        if (!effect.IsEffect())
            continue;
        CombatKinds::Effect& out = facts.effects[i];
        out.effect = effect.Effect;
        out.aura = effect.ApplyAuraName;
        out.mechanic = effect.Mechanic;
        out.misc = effect.MiscValue;
        out.value = effect.CalcValue(bot);
        out.aim = AimOf(effect);
        out.area = effect.IsTargetingArea();
    }
    return facts;
}
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

    state.key = bot->GetGUID().GetCounter();
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

void OnLogout(Player* bot)
{
    // the logout's fight end first (its Fight pair, then the damage rows): the world log's own logout hook runs after
    // this one (registered later, Playerbots.cpp) and would find the block gone; its second close returns nothing
    PlayerbotAI* const botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (botAI && botAI->combatRecord && EventLog::Wanted())
        CombatTally::OnLeave(bot, true);
    Untrack(bot);
}

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

std::uint32_t PowerOf(Unit const* unit)
{
    Powers const power = unit->getPowerType();
    return CombatKinds::Points(power, unit->GetPower(power));
}

void OnTick(PlayerbotAI* botAI, CombatTally::Share share) { StateOf(botAI).lastShare = share; }

void OnUpdate(PlayerbotAI* botAI, uint32 nowMs)
{
    State& state = StateOf(botAI);
    if (!state.sample.Due(nowMs) || !WorldLog::Active())
        return;
    PerfMonitorScope scope(RecordPerf());
    Sample(botAI->GetBot(), state, state.lastShare);
}

void OnEnter(PlayerbotAI* botAI, uint32 nowMs)
{
    State& state = StateOf(botAI);
    state.sample.Start(nowMs);
    {
        std::lock_guard<std::mutex> guard(state.lock);
        state.takenAtSample = state.dealt.totals.taken;
    }
    if (!WorldLog::Active())
        return;

    PerfMonitorScope scope(RecordPerf());
    Player* const bot = botAI->GetBot();
    Powers const power = bot->getPowerType();
    Pet* const pet = bot->GetPet();
    WorldLog::Decision(bot, DecisionKind::CombatSample, CHOICE_PULL, CombatKinds::PowerWord(power), bot->GetHealthPct(),
                       {{"power", static_cast<float>(PowerOf(bot))},
                        {"max", static_cast<float>(CombatKinds::Points(power, bot->GetMaxPower(power)))},
                        {"pet", pet ? static_cast<float>(pet->GetEntry()) : 0.0f}},
                       bot->GetMaxHealth());

    // every effect on the bot that is not passive: buffs, forms, what an enemy put on it before the pull
    for (auto const& [spellId, application] : bot->GetAppliedAuras())
    {
        Aura const* const aura = application->GetBase();
        if (aura->IsPassive())
            continue;
        WorldLog::Decision(bot, DecisionKind::CombatSample, CHOICE_HAS, "",
                           CombatRecordRules::Seconds(aura->GetDuration()),
                           {{"stacks", static_cast<float>(aura->GetStackAmount())}}, spellId);
    }

    Item* const main = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* const off = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    WorldLog::Decision(bot, DecisionKind::CombatSample, CHOICE_WEAPON, "",
                       main ? static_cast<float>(main->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT)) : 0.0f,
                       {{"off", off ? static_cast<float>(off->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT)) : 0.0f}},
                       main ? main->GetEntry() : 0);
}

Known KnownOf(Player* bot, SpellInfo const* info)
{
    Known known;
    known.kinds = CombatKinds::Classify(FactsOf(bot, info));
    known.cd = static_cast<float>(std::max(info->RecoveryTime, info->CategoryRecoveryTime)) / 1000.0f;
    known.cost = CombatKinds::Points(info->PowerType, info->CalcPowerCost(bot, info->GetSchoolMask()));
    return known;
}
}  // namespace CombatRecord

void AddSC_combat_record()
{
    new CombatRecordUnitScript();
    new CombatRecordSpellScript();
}
