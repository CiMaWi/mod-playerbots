/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CcTacticStrategy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <string_view>

#include "ArenaCc.h"
#include "ArenaCcSpells.h"
#include "Battleground.h"
#include "CcState.h"
#include "FocusState.h"
#include "Group.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTestCounters.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Tactics.h"
#include "Timer.h"

namespace
{
// Below the team plan's mark follow (ACTION_HIGH + 30) and the plan (+ 31), above the rotations: the old "cyclone on
// cc" relevance (42), the highest of the old cc strategies, so a class keeps its CC's rank against heals and
// interrupts. A healer's critical heal (ACTION_CRITICAL_HEAL, 30) still goes first: the trigger holds a healer's CC
// while a teammate is below CriticalHealth (CcHealerNeeded; G4b-a review NOTE 4). A relevance below 30 for healers
// would put the CC under every heal of the rotation, so a healer would never control.
constexpr float TeamCcRelevance = ACTION_HIGH + 22.0f;

// Above "team cc" and the rotations, below the plan's mark follow (+ 30): a held enemy is dropped before the next
// cast at it
constexpr float CcRetargetRelevance = ACTION_HIGH + 28.0f;

// The old "cc" strategies' actions (mage, druid, warlock, hunter; AiFactory adds "cc" for these classes). Rogue,
// paladin and priest CC (blind, sap, repentance, hammer of justice, psychic scream, ...) belong to class rotations and
// stay; "team cc" casts them on the CC target.
constexpr std::array<std::string_view, 8> OldCcActions = {
    "polymorph",  "cyclone on cc", "hibernate on cc",   "entangling roots on cc",
    "fear on cc", "banish on cc",  "scare beast on cc", "freezing trap on cc"};

// Known at the bot's skill (the gate's KnownFrom; full ability knows every tactic), whatever this window's use roll
bool CcKnownAtSkill(PlayerbotAI* botAI)
{
    return (botAI->GetTacticGate().KnownMask & TacticBit(TacticId::Cc)) != 0 || botAI->HasFullSkillAbility();
}

bool CcImmuneAll(Unit const* unit)
{
    return unit->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) && unit->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC);
}

// The table again when the level or the talents changed (a learned rank, a talent CC) and every CC_TABLE_REFRESH_MS
void CcRefreshTable(Player* bot, CcBotState& state, uint32 nowMs)
{
    uint8 const level = bot->GetLevel();
    uint32 const talentPoints = bot->GetFreeTalentPoints();
    if (state.TableBuilt && state.TableLevel == level && state.TableTalentPoints == talentPoints &&
        FocusBefore(nowMs, state.TableBuiltMs + CC_TABLE_REFRESH_MS))
        return;

    state.KnownMask = BuildCcBotTable(
        bot->getClass(), [bot](uint32 spellId) { return bot->HasSpell(spellId); }, state.KnownSpell, state.Spells);
    state.TableBuilt = true;
    state.TableLevel = level;
    state.TableTalentPoints = talentPoints;
    state.TableBuiltMs = nowMs;
}

// The rows the bot can start now: known, off cooldown, not waiting after a failed cast, a finisher with combo points
// on some target (comboMask; CcComboRowsOn keeps it only with points on the CC target)
uint32 CcReadyRows(Player* bot, CcBotState& state, uint32 nowMs, uint32& comboMask)
{
    comboMask = 0;
    if (state.FailedMask && !FocusBefore(nowMs, state.FailedUntilMs))
        state.FailedMask = 0;

    uint32 mask = 0;
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        uint32 const bit = CcOptionBit(index);
        if (!(state.KnownMask & bit) || (state.FailedMask & bit) || bot->HasSpellCooldown(state.KnownSpell[index]))
            continue;

        SpellInfo const* const info = sSpellMgr->GetSpellInfo(state.KnownSpell[index]);
        if (!info)
            continue;

        if (info->NeedsComboPoints())
        {
            if (!bot->GetComboPoints())
                continue;

            comboMask |= bit;
        }

        mask |= bit;
    }

    return mask;
}

// The finishers stay ready only with combo points on the target (they belong to one target, Unit::GetComboPoints);
// their duration by those points (G4a NOTE 1, G4b-a review NOTE 2)
uint32 CcComboRowsOn(Player* bot, CcBotState& state, uint32 ready, uint32 comboMask, Unit* target)
{
    if (!(ready & comboMask))
        return ready;

    uint8 const points = bot->GetComboPoints(target);
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        uint32 const bit = CcOptionBit(index);
        if (!(ready & comboMask & bit))
            continue;

        SpellInfo const* const info = sSpellMgr->GetSpellInfo(state.KnownSpell[index]);
        if (!points || !info)
        {
            ready &= ~bit;
            continue;
        }

        state.Spells[index].BaseMs = CcComboDurationMs(info->GetDuration(), info->GetMaxDuration(), points);
    }

    return ready;
}

// A healer holds its CC while a teammate on its map (or itself) is below CriticalHealth: the critical heal first
bool CcHealerNeeded(Player* bot)
{
    if (!PlayerbotAI::IsHeal(bot))
        return false;

    float const critical = static_cast<float>(sPlayerbotAIConfig.CriticalHealth);
    Group* const group = bot->GetGroup();
    if (!group)
        return bot->GetHealthPct() < critical;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* const member = ref->GetSource();
        if (member && member->IsInMap(bot) && member->IsAlive() && member->GetHealthPct() < critical)
            return true;
    }

    return false;
}

float CcLongestReach(CcBotState const& state, uint32 ready)
{
    float reach = 0.0f;
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        if (!(ready & CcOptionBit(index)))
            continue;

        CcOption const& option = CC_OPTIONS[index];
        reach =
            std::max(reach, option.ReachYards ? static_cast<float>(option.ReachYards) : state.Spells[index].RangeYards);
    }

    return reach;
}

// The team's CC target: the moon in an arena (DT.1: the group's icon; none while it is set but out of reach; onMoon);
// otherwise the strongest enemy (highest max health, a player or player-controlled) of "fight enemies" within the
// longest ready reach other than the bot's own target and the skull, kept CC_TARGET_KEEP_MS
Unit* CcFindTarget(PlayerbotAI* botAI, Player* bot, CcBotState& state, uint32 nowMs, float reach, bool& onMoon)
{
    onMoon = false;
    Group* const group = bot->GetGroup();
    if (group && botAI->GetTacticContext() == TacticContext::Arena)
    {
        ObjectGuid const moon = group->GetTargetIcon(FOCUS_ICON_MOON);
        if (!moon.IsEmpty())
        {
            Unit* const unit = botAI->GetUnit(moon);
            bool const valid = unit && unit->IsInMap(bot) && unit->IsAlive() && bot->IsValidAttackTarget(unit);
            onMoon = valid;
            return valid ? unit : nullptr;
        }
    }

    ObjectGuid const skull = group ? group->GetTargetIcon(FOCUS_ICON_SKULL) : ObjectGuid::Empty;
    Unit* const current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    ObjectGuid const own = current ? current->GetGUID() : ObjectGuid::Empty;
    if (!state.CcTarget.IsEmpty() && FocusBefore(nowMs, state.CcTargetUntilMs) && state.CcTarget != own &&
        state.CcTarget != skull)
    {
        Unit* const kept = botAI->GetUnit(state.CcTarget);
        if (kept && kept->IsInMap(bot) && kept->IsAlive())
            return kept;
    }

    state.CcTarget = ObjectGuid::Empty;
    Unit* best = nullptr;
    for (ObjectGuid const& guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("fight enemies")->Get())
    {
        if (guid == own || guid == skull)
            continue;

        Unit* const unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsInMap(bot) || !unit->IsAlive() || !unit->GetCharmerOrOwnerPlayerOrPlayerItself() ||
            !bot->IsWithinDistInMap(unit, reach))
            continue;

        if (!best || unit->GetMaxHealth() > best->GetMaxHealth())
            best = unit;
    }

    if (best)
    {
        state.CcTarget = best->GetGUID();
        state.CcTargetUntilMs = nowMs + CC_TARGET_KEEP_MS;
    }

    return best;
}

// A low-effort bot stops controlling when its arena team is behind (the arena frames' health; elsewhere never)
bool CcTeamBehind(Player* bot, CcParams const& params)
{
    if (!params.StopsWhenBehind)
        return false;

    Battleground* const bg = bot->GetBattleground();
    if (!bg || !bg->isArena())
        return false;

    uint32 ownSum = 0;
    uint32 ownCount = 0;
    uint32 enemySum = 0;
    uint32 enemyCount = 0;
    for (auto const& [guid, player] : bg->GetPlayers())
    {
        if (!player || !player->IsInMap(bot))
            continue;

        uint32 const pct = player->IsAlive() ? static_cast<uint32>(player->GetHealthPct()) : 0;
        bool const own = player->GetBgTeamId() == bot->GetBgTeamId();
        (own ? ownSum : enemySum) += pct;
        ++(own ? ownCount : enemyCount);
    }

    if (!ownCount || !enemyCount)
        return false;

    return CcStopsBehind(params, static_cast<uint8>(ownSum / ownCount), static_cast<uint8>(enemySum / enemyCount));
}

bool CcFeedOn()
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    return config.TacticsEnable &&
           (config.TacticConfig[static_cast<uint32>(TacticId::Cc)].Enable || config.PvpTestEnable);
}

// The bots that see a CC land on or leave the unit (DT.12: the combat log of their own fight): the caster and the
// bots of its group on the unit's map (the same map thread: their state is safe to write, C3) with the Cc tactic
// enabled here
template <typename Fn>
void CcForEachTeamBot(Unit* unit, Player* caster, Fn const& fn)
{
    auto const visit = [&](Player* member)
    {
        if (!member || !member->IsInMap(unit))
            return;

        PlayerbotAI* const memberAI = GET_PLAYERBOT_AI(member);
        if (memberAI && memberAI->TacticEnabledHere(TacticId::Cc))
            fn(member, memberAI->GetCcState());
    };

    Group* const group = caster->GetGroup();
    if (!group)
    {
        visit(caster);
        return;
    }

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        visit(ref->GetSource());
}

// Those of them with DR awareness feed their DR memory (below it the bot believes every CC lands in full: PickCc
// ignores the levels)
template <typename Fn>
void CcForEachObserver(Unit* unit, Player* caster, Fn const& fn)
{
    CcForEachTeamBot(unit, caster,
                     [&](Player* member, CcBotState& state)
                     {
                         if (state.Params.DrAware)
                             fn(member, state);
                     });
}

// tactics force dr's casts are not chains (CcSetForcedCasts)
std::atomic<bool> ccForcedCasts{false};

// G4c: a loss-of-control CC of the team landing on the enemy. The caster's gap to the team's previous one on it (a
// chain: that one still up or ended within CC_CHAIN_WINDOW_MS) is counted while the harness counts and traced for the
// spread (Logger.playerbots=6); then every team bot's clock takes the new end.
void CcNoteChainLanding(Unit* unit, Player* caster, Aura const* aura, uint8 group, uint32 nowMs, uint32 durationMs)
{
    uint64 const enemy = unit->GetGUID().GetRawValue();
    PlayerbotAI* const casterAI = GET_PLAYERBOT_AI(caster);
    if (casterAI && casterAI->TacticEnabledHere(TacticId::Cc) && !ccForcedCasts.load(std::memory_order_relaxed))
    {
        CcBotState& state = casterAI->GetCcState();
        int32 signedGapMs = 0;
        uint8 previousGroup = 0;
        if (state.Chain.Gap(enemy, nowMs, CC_CHAIN_WINDOW_MS, signedGapMs, previousGroup))
        {
            ++state.ChainsLanded;
            state.LastChainGapMs = signedGapMs;
            casterAI->CountTactic(TacticCounter::ChainGapMs,
                                  static_cast<uint32>(signedGapMs < 0 ? -signedGapMs : signedGapMs));
            casterAI->CountTactic(TacticCounter::Chains);
            LOG_TRACE("playerbots",
                      "CC chain: {} (skill {}, lead {} ms) {} ({}) on {} after {}, gap {} ms, lasts {} ms, at {} ms",
                      caster->GetName(), casterAI->GetPvpSkill(), state.Params.ChainLeadMs,
                      aura->GetSpellInfo()->SpellName[0], PVPTEST_DR_GROUP_NAMES[group % PVPTEST_DR_GROUP_COUNT],
                      unit->GetName(), PVPTEST_DR_GROUP_NAMES[previousGroup % PVPTEST_DR_GROUP_COUNT], signedGapMs,
                      durationMs, nowMs);
        }
    }

    CcForEachTeamBot(unit, caster,
                     [&](Player* /*member*/, CcBotState& state) { state.Chain.Land(enemy, group, nowMs, durationMs); });
}

// tactics.ccHeldMs (G4c, the CC uptime): how long the caster's loss of control held on an enemy player, whatever
// ended it (the aura's DR'd and capped duration less what was left)
void CcCountHeld(Unit const* unit, Player* caster, Aura const* aura)
{
    if (!sPlayerbotAIConfig.PvpTestEnable || !unit->IsPlayer())
        return;

    PlayerbotAI* const casterAI = GET_PLAYERBOT_AI(caster);
    int32 const maxMs = aura->GetMaxDuration();
    if (!casterAI || !casterAI->IsPvpTestCounting() || maxMs <= 0)
        return;

    casterAI->CountTactic(TacticCounter::CcHeldMs,
                          static_cast<uint32>(maxMs - std::clamp<int32>(aura->GetDuration(), 0, maxMs)));
}

// A CC aura of a hostile player (or his pet) on a player or a player's pet: its DR group, else DIMINISHING_NONE
DiminishingGroup CcObservedGroup(Unit* unit, Aura const* aura, Player*& casterPlayer)
{
    casterPlayer = nullptr;
    DiminishingGroup const group = GetDiminishingReturnsGroupForSpell(aura->GetSpellInfo(), false);
    if (!IsPvpTestCcGroup(static_cast<uint32>(group)) || !unit->GetCharmerOrOwnerPlayerOrPlayerItself())
        return DIMINISHING_NONE;

    Unit* const caster = aura->GetCaster();
    casterPlayer = caster ? caster->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!casterPlayer || !caster->IsHostileTo(unit))
        return DIMINISHING_NONE;

    return group;
}

// The last damage event of this map thread that hit an enemy in a team breakable CC (G4b-b). The break is counted
// in OnAuraRemove, only when that damage really removed the aura: the core spares the aura's own spell (frost nova's
// damage, a root's ticks) and spells with SPELL_ATTR4_DAMAGE_DOESNT_BREAK_AURAS, Unit.cpp DealDamage.
struct CcPendingBreak
{
    Player* Source = nullptr;
    Unit const* Victim = nullptr;
    Aura const* BrokenAura = nullptr;
    uint32 AtMs = 0;
    // For the break's log line: the damage's spell (0 melee), whether a pet dealt it, what the bot aimed at
    SpellInfo const* DamageSpell = nullptr;
    bool ByPet = false;
    bool OnTarget = false;
    bool KeptOut = false;
};

thread_local CcPendingBreak ccPendingBreak;
// The spell of the damage being dealt on this thread (ModifySpellDamageTaken / ModifyPeriodicDamageAurasTick set it,
// ModifyMeleeDamage clears it); read only for a pending break's log line
thread_local SpellInfo const* ccDamageSpell = nullptr;

void CcConsumeOwnBreak(Unit const* unit, Aura const* aura, AuraRemoveMode mode)
{
    if (ccPendingBreak.BrokenAura != aura || ccPendingBreak.Victim != unit)
        return;

    CcPendingBreak const pending = ccPendingBreak;
    ccPendingBreak = CcPendingBreak{};
    // RemoveAurasWithInterruptFlags removes with the default mode, in the same DealDamage call as the hook
    if (mode != AURA_REMOVE_BY_DEFAULT || getMSTimeDiff(pending.AtMs, getMSTime()) > 1)
        return;

    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(pending.Source);
    if (!botAI || !botAI->IsPvpTestCounting())
        return;

    CcBotState& state = botAI->GetCcState();
    if (state.OwnBreakAura == aura && state.OwnBreakApplyTime == aura->GetApplyTime())
        return;

    state.OwnBreakAura = aura;
    state.OwnBreakApplyTime = aura->GetApplyTime();
    botAI->CountPvpTestOwnBreak();
    Unit const* const caster = aura->GetCaster();
    // Diagnostic only, at trace level (Logger.playerbots=6): the dist's level 5 keeps it out of the default logs
    LOG_TRACE("playerbots",
              "CC own break: {} broke {} ({}) on {} by {} {} ({}), aura {} ms old, bot target {}, kept out {}, "
              "cache {} ms",
              pending.Source->GetName(), aura->GetSpellInfo()->SpellName[0], caster ? caster->GetName() : "?",
              unit->GetName(), pending.ByPet ? "pet" : "self",
              pending.DamageSpell ? pending.DamageSpell->SpellName[0] : "melee",
              pending.DamageSpell ? pending.DamageSpell->Id : 0, aura->GetMaxDuration() - aura->GetDuration(),
              pending.OnTarget ? "yes" : "no", pending.KeptOut ? "yes" : "no",
              FocusBefore(pending.AtMs, state.ExcludedUntilMs)
                  ? static_cast<int32>(CC_EXCLUDE_REFRESH_MS - (state.ExcludedUntilMs - pending.AtMs))
                  : -1);
}

class CcTacticUnitScript : public UnitScript
{
public:
    CcTacticUnitScript()
        : UnitScript("CcTacticUnitScript", true,
                     {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE, UNITHOOK_ON_DAMAGE,
                      UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_MODIFY_MELEE_DAMAGE,
                      UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN})
    {
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& /*damage*/) override { CcCountOwnBreak(attacker, victim); }

    // One store each; only the own-break log line reads it
    void ModifyPeriodicDamageAurasTick(Unit* /*target*/, Unit* /*attacker*/, uint32& /*damage*/,
                                       SpellInfo const* spellInfo) override
    {
        ccDamageSpell = spellInfo;
    }

    void ModifyMeleeDamage(Unit* /*target*/, Unit* /*attacker*/, uint32& /*damage*/) override
    {
        ccDamageSpell = nullptr;
    }

    void ModifySpellDamageTaken(Unit* /*target*/, Unit* /*attacker*/, int32& /*damage*/,
                                SpellInfo const* spellInfo) override
    {
        ccDamageSpell = spellInfo;
    }

    // Fires for every aura of every unit: the config flags, then the aura's DR group, before anything else
    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!CcFeedOn())
            return;

        Player* caster = nullptr;
        DiminishingGroup const group = CcObservedGroup(unit, aura, caster);
        int32 const durationMs = aura->GetDuration();
        if (group == DIMINISHING_NONE || durationMs <= 0)
            return;

        // The visible duration: the core has already applied DR and the PvP cap
        uint64 const enemy = unit->GetGUID().GetRawValue();
        uint32 const nowMs = getMSTime();
        CcForEachObserver(unit, caster,
                          [&](Player* member, CcBotState& state)
                          {
                              state.Memory.Observe(enemy, static_cast<uint8>(group), nowMs,
                                                   static_cast<uint32>(durationMs), state.Params.DrMissPct,
                                                   member->GetGUID().GetCounter());
                          });
        if (CcGroupLosesControl(static_cast<uint8>(group)))
            CcNoteChainLanding(unit, caster, aura, static_cast<uint8>(group), nowMs, static_cast<uint32>(durationMs));
    }

    // Removed before its end (damage, dispel, trinket, death): the reset counts from now, or from the end of another
    // aura of the group still on the unit; the team's control ends now, or with its other loss-of-control CC there
    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        Aura const* const aura = aurApp->GetBase();
        CcConsumeOwnBreak(unit, aura, mode);
        if (!CcFeedOn())
            return;

        Player* caster = nullptr;
        DiminishingGroup const group = CcObservedGroup(unit, aura, caster);
        if (group == DIMINISHING_NONE)
            return;

        bool const control = CcGroupLosesControl(static_cast<uint8>(group));
        if (control)
            CcCountHeld(unit, caster, aura);

        if (mode == AURA_REMOVE_BY_EXPIRE)
            return;

        uint32 otherLeftMs = 0;
        uint32 teamControlLeftMs = 0;
        for (auto const& [spellId, application] : unit->GetAppliedAuras())
        {
            Aura const* const other = application->GetBase();
            if (other == aura)
                continue;

            DiminishingGroup const otherGroup = GetDiminishingReturnsGroupForSpell(other->GetSpellInfo(), false);
            uint32 const leftMs = static_cast<uint32>(std::max<int32>(other->GetDuration(), 0));
            if (otherGroup == group)
                otherLeftMs = std::max(otherLeftMs, leftMs);

            if (control && leftMs > teamControlLeftMs && CcGroupLosesControl(static_cast<uint8>(otherGroup)) &&
                CcTeamCaster(other->GetCaster(), caster))
                teamControlLeftMs = leftMs;
        }

        uint64 const enemy = unit->GetGUID().GetRawValue();
        uint32 const nowMs = getMSTime();
        CcForEachObserver(unit, caster, [&](Player* /*member*/, CcBotState& state)
                          { state.Memory.Ended(enemy, static_cast<uint8>(group), nowMs, otherLeftMs); });
        if (control)
            CcForEachTeamBot(unit, caster, [&](Player* /*member*/, CcBotState& state)
                             { state.Chain.Ended(enemy, nowMs, teamControlLeftMs); });
    }
};
}  // namespace

float TacticCcMultiplier::GetValue(Action* action)
{
    // Most actions end at the cached lookup
    if (action == nullptr || !IsOldCcAction(action) || !botAI->TacticEnabledHere(TacticId::Cc) ||
        !CcKnownAtSkill(botAI))
        return 1.0f;

    return 0.0f;
}

bool TacticCcMultiplier::IsOldCcAction(Action* action)
{
    auto const it = oldCcActions.find(action);
    if (it != oldCcActions.end())
        return it->second;

    // getName() builds a string; resolved once per action and bot
    std::string const name = action->getName();
    bool const old = std::find(OldCcActions.begin(), OldCcActions.end(), std::string_view(name)) != OldCcActions.end();
    return oldCcActions.emplace(action, old).first->second;
}

void PvpCcStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TacticCcMultiplier(botAI));
}

void TacticCcStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("cc ready", {NextAction("team cc", TeamCcRelevance)}));
    triggers.push_back(new TriggerNode("own cc target", {NextAction("cc retarget", CcRetargetRelevance)}));
}

void TacticCcStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::TankTarget)
        return;

    for (ObjectGuid const& guid : CcOwnExclusions(botAI))
        exclusions.insert(guid);
}

std::vector<ObjectGuid> const& CcOwnExclusions(PlayerbotAI* botAI)
{
    CcBotState& state = botAI->GetCcState();
    uint32 const nowMs = getMSTime();
    if (FocusBefore(nowMs, state.ExcludedUntilMs))
        return state.Excluded;

    state.ExcludedUntilMs = nowMs + CC_EXCLUDE_REFRESH_MS;
    state.Excluded.clear();
    if (!botAI->TacticEnabledHere(TacticId::OwnCc))
        return state.Excluded;

    Player* const bot = botAI->GetBot();
    bool const forced = !state.ForcedCcGuid.IsEmpty() && FocusBefore(nowMs, state.ForcedCcUntilMs);
    if (forced)
        state.Excluded.push_back(state.ForcedCcGuid);
    else
        state.ForcedCcGuid = ObjectGuid::Empty;

    // Below the tactic's known skill the bot does not look for its team's CC (it breaks it); above it the own-break
    // window of low skill lets it break it now and then (CcParams::OwnBreakPct, 0 from OwnBreakNeverFromSkill)
    bool const known =
        (botAI->GetTacticGate().KnownMask & TacticBit(TacticId::OwnCc)) != 0 || botAI->HasFullSkillAbility();
    bool const breaksOwn = !known || CcBreaksOwn(state.Params, bot->GetGUID().GetCounter(), nowMs);
    if (breaksOwn)
        return state.Excluded;

    // What the bot fights: its attackers, its target and the skull (what the target picks read)
    AiObjectContext* const context = botAI->GetAiObjectContext();
    Unit* const current = context->GetValue<Unit*>("current target")->Get();
    Group* const group = bot->GetGroup();
    auto const consider = [&](ObjectGuid const& guid)
    {
        if (guid.IsEmpty() || std::find(state.Excluded.begin(), state.Excluded.end(), guid) != state.Excluded.end())
            return;

        Unit* const unit = botAI->GetUnit(guid);
        if (unit && unit->IsInMap(bot) && unit->IsAlive() &&
            CcKeepsOut(CcTeamBreakableAura(unit, bot) != nullptr, false, breaksOwn))
            state.Excluded.push_back(guid);
    };

    for (ObjectGuid const& guid : context->GetValue<GuidVector>("attackers")->Get())
        consider(guid);

    consider(current ? current->GetGUID() : ObjectGuid::Empty);
    consider(group ? group->GetTargetIcon(FOCUS_ICON_SKULL) : ObjectGuid::Empty);
    return state.Excluded;
}

void CcSetForcedCasts(bool on) { ccForcedCasts.store(on, std::memory_order_relaxed); }

void CcCountOwnBreak(Unit* attacker, Unit* victim)
{
    // Every damage event of every unit: the harness switch, then a player source, then the aura lists. A damage
    // event that is not a counting bot's clears the pending break, so its removal is never charged to a bot
    ccPendingBreak.BrokenAura = nullptr;
    if (!sPlayerbotAIConfig.PvpTestEnable || !attacker || !victim || attacker == victim)
        return;

    Unit* const source = attacker->GetCharmerOrOwnerOrSelf();
    Player* const player = source ? source->ToPlayer() : nullptr;
    if (!player || source == victim)
        return;

    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(player);
    if (!botAI || !botAI->IsPvpTestCounting())
        return;

    // Counted only if this damage removes the aura (CcConsumeOwnBreak in OnAuraRemove)
    Aura const* const aura = CcTeamBreakableAura(victim, player);
    if (!aura)
    {
        ccPendingBreak = CcPendingBreak{};
        return;
    }

    std::vector<ObjectGuid> const& excluded = botAI->GetCcState().Excluded;
    ccPendingBreak = CcPendingBreak{player,
                                    victim,
                                    aura,
                                    getMSTime(),
                                    ccDamageSpell,
                                    attacker != player,
                                    player->GetTarget() == victim->GetGUID() || player->GetVictim() == victim,
                                    std::find(excluded.begin(), excluded.end(), victim->GetGUID()) != excluded.end()};
}

bool CcTargetKeptOut(PlayerbotAI* botAI, Unit const* target)
{
    if (!target)
        return false;

    std::vector<ObjectGuid> const& excluded = CcOwnExclusions(botAI);
    return std::find(excluded.begin(), excluded.end(), target->GetGUID()) != excluded.end();
}

bool OwnCcTargetTrigger::IsActive()
{
    return botAI->TacticEnabledHere(TacticId::OwnCc) && CcTargetKeptOut(botAI, AI_VALUE(Unit*, "current target"));
}

bool CcRetargetAction::isUseful() { return CcTargetKeptOut(botAI, AI_VALUE(Unit*, "current target")); }

bool CcRetargetAction::Execute(Event /*event*/)
{
    Unit* const current = AI_VALUE(Unit*, "current target");
    if (!current)
        return false;

    ObjectGuid const guid = current->GetGUID();
    for (CurrentSpellTypes const type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
    {
        Spell* const spell = bot->GetCurrentSpell(type);
        if (spell && spell->m_targets.GetUnitTargetGUID() == guid)
            bot->InterruptSpell(type);
    }

    if (bot->GetVictim() == current)
        bot->AttackStop();

    Pet* const pet = bot->GetPet();
    if (pet && pet->GetVictim() == current)
        pet->AttackStop();

    context->GetValue<Unit*>("current target")->Set(nullptr);
    bot->SetSelection(ObjectGuid::Empty);
    // The target picks run again at once, without the held enemy
    context->GetValue<Unit*>("dps target")->Reset();
    context->GetValue<Unit*>("enemy player target")->Reset();
    return true;
}

bool CcReadyTrigger::IsActive()
{
    if (!botAI->TacticEnabledHere(TacticId::Cc))
        return false;

    CcBotState& state = botAI->GetCcState();
    uint32 const nowMs = getMSTime();
    if (FocusBefore(nowMs, state.PickUntilMs))
        return state.PickRow != CC_NO_ROW;

    state.PickUntilMs = nowMs + CC_READY_REFRESH_MS;
    state.PickRow = CC_NO_ROW;
    Player* const bot = botAI->GetBot();
    if (!bot->IsAlive() || bot->IsNonMeleeSpellCast(false) || !botAI->TacticActive(TacticId::Cc))
    {
        state.ReadySinceMs = 0;
        return false;
    }

    CcRefreshTable(bot, state, nowMs);
    uint32 comboMask = 0;
    uint32 ready = CcReadyRows(bot, state, nowMs, comboMask);
    state.ReadyMask = ready;
    bool onMoon = false;
    Unit* const ccTarget =
        ready ? CcFindTarget(botAI, bot, state, nowMs, CcLongestReach(state, ready), onMoon) : nullptr;
    if (!ccTarget)
    {
        state.ReadySinceMs = 0;
        return false;
    }

    // A wrong-target window: the CC goes on the bot's own target instead (whoever is near; low skill)
    CcParams const& params = state.Params;
    Unit* const current = AI_VALUE(Unit*, "current target");
    Unit* target = ccTarget;
    if (CcWrongTarget(params, bot->GetGUID().GetCounter(), nowMs) && current && current != ccTarget &&
        current->IsAlive() && bot->IsValidAttackTarget(current))
        target = current;

    // No CC on a target immune to all (cyclone, banish, a bubble); a root on it is seen in PickCc (no root on top,
    // the loss-of-control rows still go)
    ready = CcComboRowsOn(bot, state, ready, comboMask, target);
    if (!ready || CcImmuneAll(target) || CcTeamBehind(bot, params))
    {
        state.ReadySinceMs = 0;
        return false;
    }

    // A controlled target: only a chaining bot (G4c, Cc.ChainEnable) goes on, and only on its team's CC (the visible
    // aura that ends last: its remaining time and DR group); anyone else's control, or no chains, waits as in G4b. A
    // teammate's CC on his cast bar at the target counts as pending.
    bool const chains = params.Chains && sPlayerbotAIConfig.TacticCc.CcChainEnable != 0;
    CcChainView view;
    uint8 activeGroup = 0;
    if (CcUnitControlled(target))
    {
        view.AuraOnTarget = chains && CcTeamControlAura(target, bot, view.RemainingMs, activeGroup);
        if (!view.AuraOnTarget)
        {
            state.ReadySinceMs = 0;
            return false;
        }
    }

    if (chains)
        view.PartnerCastPending = CcTeamCastingCcAt(bot, target->GetGUID());

    // A patient bot holds a ready CC for the kill window: its own target low; a chain under way is not held
    if (!state.ReadySinceMs)
        state.ReadySinceMs = nowMs;

    bool const killWindow =
        current && current != target && current->IsAlive() && current->GetHealthPct() <= CC_KILL_WINDOW_PCT;
    if ((!view.AuraOnTarget && CcHolds(params, nowMs - state.ReadySinceMs, killWindow)) || CcHealerNeeded(bot))
        return false;

    CcPickInput input;
    input.ReadyMask = ready;
    input.TargetIsPlayer = target->IsPlayer();
    input.DrAsPlayer = target->GetCharmerOrOwnerPlayerOrPlayerItself() != nullptr;
    input.CreatureType = static_cast<uint8>(target->GetCreatureType());
    input.DistanceYards = bot->GetDistance(target);
    input.Stealthed = bot->HasStealthAura();
    input.TargetInCombat = target->IsInCombat();
    input.Rooted = target->HasUnitState(UNIT_STATE_ROOT);
    input.ControlOnly = onMoon && target == ccTarget;
    input.ActiveGroup = activeGroup;
    if (params.DrAware)
        input.Levels = state.Memory.Levels(target->GetGUID().GetRawValue(), nowMs);

    // A chaining bot whose memory leaves no group that lands (immune, too short) has no row and waits
    int8 const row = PickCc(input, state.Spells, params);
    if (row == CC_NO_ROW)
        return false;

    // The chain's moment: the CC lands ChainLeadMs before the team's CC ends; the answer is asked again at that
    // moment rather than a whole refresh later (the engine's cadence and the reaction model add the spread, DT.11)
    if (chains && !ChainDue(view, state.Spells[row].CastMs, params.ChainLeadMs, true))
    {
        ++state.ChainWaits;
        uint32 const waitMs = CcChainWaitMs(view, state.Spells[row].CastMs, params.ChainLeadMs);
        if (waitMs && waitMs < CC_READY_REFRESH_MS)
            state.PickUntilMs = nowMs + waitMs;

        return false;
    }

    if (!bot->IsWithinLOSInMap(target))
        return false;

    state.PickRow = row;
    state.PickTarget = target->GetGUID();
    state.PickCcTarget = ccTarget->GetGUID();
    return true;
}

Unit* TeamCcAction::GetTarget()
{
    CcBotState& state = botAI->GetCcState();
    return state.PickTarget.IsEmpty() ? nullptr : botAI->GetUnit(state.PickTarget);
}

bool TeamCcAction::isUseful() { return botAI->GetCcState().PickRow != CC_NO_ROW; }

bool TeamCcAction::isPossible()
{
    CcBotState& state = botAI->GetCcState();
    if (state.PickRow == CC_NO_ROW)
        return false;

    Unit* const target = GetTarget();
    uint32 const spellId = state.KnownSpell[state.PickRow];
    return target && spellId && botAI->CanCastSpell(spellId, target, true);
}

bool TeamCcAction::Execute(Event /*event*/)
{
    CcBotState& state = botAI->GetCcState();
    int8 const row = state.PickRow;
    Unit* const target = GetTarget();
    state.PickRow = CC_NO_ROW;
    state.PickUntilMs = 0;
    if (row == CC_NO_ROW || !target)
        return false;

    // The trap row casts the trap spell (it goes down at the hunter's feet; the row's reach is 5 yd)
    if (!botAI->CastSpell(state.KnownSpell[row], target))
    {
        state.FailedMask |= CcOptionBit(static_cast<uint8>(row));
        state.FailedUntilMs = getMSTime() + CC_FAILED_BACKOFF_MS;
        return false;
    }

    state.ReadySinceMs = 0;
    ++state.Casts;
    botAI->NoteTacticUse(TacticId::Cc);
    return true;
}

void AddCcTacticScripts() { new CcTacticUnitScript(); }
