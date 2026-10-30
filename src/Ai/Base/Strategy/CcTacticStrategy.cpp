/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CcTacticStrategy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <span>
#include <string_view>

#include "ArenaCc.h"
#include "ArenaCcSpells.h"
#include "Bag.h"
#include "Battleground.h"
#include "CcState.h"
#include "FocusState.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpGearMgr.h"
#include "PvpTestCounters.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Tactics.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

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
// G4d fix: Action::getName() is the action's own name, not its creator's name. The CastCrowdControlSpellAction
// classes (CastPolymorphAction, CastCycloneCcAction, CastHibernateCcAction, CastEntanglingRootsCcAction,
// CastFearOnCcAction, CastBanishOnCcAction) are named by their spell; the hunter's CastScareBeastCcAction is named
// "scare beast on cc" and CastFreezingTrap (made by both "freezing trap" and "freezing trap on cc") "freezing trap".
// The druid's plain CastHibernateAction and CastEntanglingRootsAction (balance, kiting) share the spell names of the
// cc ones but are no CastCrowdControlSpellAction: they stay (CcOldActionName, IsOldCcAction)
constexpr std::array<std::string_view, 6> OldCcSpellActions = {"polymorph",        "cyclone", "hibernate",
                                                               "entangling roots", "fear",    "banish"};
constexpr std::array<std::string_view, 2> OldCcOtherActions = {"scare beast on cc", "freezing trap"};

// Known at the bot's skill (the gate's KnownFrom; full ability knows every tactic), whatever this window's use roll
bool CcKnownAtSkill(PlayerbotAI* botAI)
{
    return (botAI->GetTacticGate().KnownMask & TacticBit(TacticId::Cc)) != 0 || botAI->HasFullSkillAbility();
}

// OwnCc's skill model (G4b-b): below the tactic's known skill the bot does not look for its team's CC (it breaks it);
// above it the own-break window of low skill lets it break it now and then (CcParams::OwnBreakPct, 0 from
// OwnBreakNeverFromSkill)
bool CcBreaksOwnNow(PlayerbotAI* botAI, CcBotState const& state, uint32 nowMs)
{
    bool const known =
        (botAI->GetTacticGate().KnownMask & TacticBit(TacticId::OwnCc)) != 0 || botAI->HasFullSkillAbility();
    return !known || CcBreaksOwn(state.Params, botAI->GetBot()->GetGUID().GetCounter(), nowMs);
}

bool CcImmuneAll(Unit const* unit)
{
    return unit->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) && unit->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC);
}

// G4c-d F1: every school and damage immunity on the unit comes from a cyclone (no bubble or ice block besides), so
// the immunity ends with it; the caller asks only on a unit immune to all (CcImmuneAll)
bool CcFromCyclone(uint32 spellId)
{
    SpellInfo const* const info = sSpellMgr->GetSpellInfo(spellId);
    return info && GetDiminishingReturnsGroupForSpell(info, false) == DIMINISHING_CYCLONE;
}

bool CcImmuneOnlyByCyclone(Unit const* unit)
{
    for (uint32 const kind : {static_cast<uint32>(IMMUNITY_SCHOOL), static_cast<uint32>(IMMUNITY_DAMAGE)})
    {
        for (auto const& [mask, spellId] : unit->m_spellImmune[kind])
        {
            if (!CcFromCyclone(spellId))
                return false;
        }
    }

    return true;
}

// G4c-d review: the bot's real cast time of a row (haste: Icy Veins, gear), not the table's DBC base time
uint32 CcCastMsNow(Player* bot, CcBotState const& state, int8 row)
{
    SpellInfo const* const info = sSpellMgr->GetSpellInfo(state.KnownSpell[row]);
    return info ? info->CalcCastTime(bot) : state.Spells[row].CastMs;
}

// G4c-d F3: the bot's expected delay from a chain's moment to its cast: what it learned of its chains, else the
// reaction model's median at its skill
uint32 CcChainReactMs(PlayerbotAI* botAI, CcBotState const& state)
{
    if (state.ChainReactLearned)
        return state.ChainReactMs;

    return static_cast<uint32>(std::exp(botAI->GetReactionState().Mu) + 0.5f);
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
// on some target (comboMask; CcComboRowsOn keeps it only with points on the CC target), a form spell in its form (G4d:
// bash in bear form, maim in cat form; the bot does not shift for a CC, and a failed cast would back the row off)
CcRowMask CcReadyRows(Player* bot, CcBotState& state, uint32 nowMs, CcRowMask& comboMask)
{
    comboMask = 0;
    if (state.FailedMask && !FocusBefore(nowMs, state.FailedUntilMs))
        state.FailedMask = 0;

    CcRowMask mask = 0;
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        CcRowMask const bit = CcOptionBit(index);
        if (!(state.KnownMask & bit) || (state.FailedMask & bit) || bot->HasSpellCooldown(state.KnownSpell[index]))
            continue;

        SpellInfo const* const info = sSpellMgr->GetSpellInfo(state.KnownSpell[index]);
        if (!info || (info->Stances && info->CheckShapeshift(bot->GetShapeshiftForm()) != SPELL_CAST_OK))
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
CcRowMask CcComboRowsOn(Player* bot, CcBotState& state, CcRowMask ready, CcRowMask comboMask, Unit* target)
{
    if (!(ready & comboMask))
        return ready;

    uint8 const points = bot->GetComboPoints(target);
    for (uint8 index = 0; index < CC_OPTION_COUNT; ++index)
    {
        CcRowMask const bit = CcOptionBit(index);
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

float CcLongestReach(CcBotState const& state, CcRowMask ready)
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

// G4c: a loss-of-control CC of the team landing on the enemy. The caster's gap to the team's previous one on it (that
// one still up or ended within CC_CHAIN_WINDOW_MS) is counted while the harness counts and traced for the spread
// (Logger.playerbots=6): as a chain only for the landing of the caster's own team cc cast that it picked as a chain
// (G4c-d F2), else as a re-CC (a freed target CC'd again, a rotation, racial or pet stun); then every team bot's
// clock takes the new end.
void CcNoteChainLanding(Unit* unit, Player* caster, Aura const* aura, uint8 group, uint32 nowMs, uint32 durationMs)
{
    uint64 const enemy = unit->GetGUID().GetRawValue();
    PlayerbotAI* const casterAI = GET_PLAYERBOT_AI(caster);
    if (casterAI && casterAI->TacticEnabledHere(TacticId::Cc) && !ccForcedCasts.load(std::memory_order_relaxed))
    {
        CcBotState& state = casterAI->GetCcState();
        bool const own = state.CastAuraSpell && state.CastAuraSpell == aura->GetId() &&
                         getMSTimeDiff(state.CastAtMs, nowMs) <= CC_CHAIN_LANDING_MS;
        bool const planned = own && state.CastWasChain;
        bool const busy = planned && state.CastWasBusy;
        if (own)
            state.CastAuraSpell = 0;

        int32 signedGapMs = 0;
        uint8 previousGroup = 0;
        if (state.Chain.Gap(enemy, nowMs, CC_CHAIN_WINDOW_MS, signedGapMs, previousGroup))
        {
            uint32 const gapMs = static_cast<uint32>(signedGapMs < 0 ? -signedGapMs : signedGapMs);
            if (planned)
            {
                ++state.ChainsLanded;
                state.LastChainGapMs = signedGapMs;
                casterAI->CountTactic(TacticCounter::ChainGapMs, gapMs);
                casterAI->CountTactic(TacticCounter::Chains);
                if (signedGapMs < 0)
                    casterAI->CountTactic(TacticCounter::ChainEarlyMs, gapMs);

                if (busy)
                    casterAI->CountTactic(TacticCounter::ChainBusy);
            }
            else
            {
                ++state.ReCcsLanded;
                casterAI->CountTactic(TacticCounter::ReCcGapMs, gapMs);
                casterAI->CountTactic(TacticCounter::ReCcs);
            }

            LOG_TRACE(
                "playerbots",
                "CC {}: {} (skill {}, lead {} ms) {} ({}) on {} after {}, gap {} ms, lasts {} ms, at {} ms, react "
                "{} ms, busy {}",
                planned ? "chain" : "re-cc", caster->GetName(), casterAI->GetPvpSkill(), state.Params.ChainLeadMs,
                aura->GetSpellInfo()->SpellName[0], PVPTEST_DR_GROUP_NAMES[group % PVPTEST_DR_GROUP_COUNT],
                unit->GetName(), PVPTEST_DR_GROUP_NAMES[previousGroup % PVPTEST_DR_GROUP_COUNT], signedGapMs,
                durationMs, nowMs, state.ChainReactMs, busy ? "yes" : "no");
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

// The last damage events of this map thread that hit an enemy in a team breakable CC (G4b-b), one slot per victim. A
// damage shield's reverse damage (Unit.cpp DealDamageShieldDamage, before the outer hit's aura removal) is a nested
// event on another victim and no longer wipes the outer one (G4b-b review NOTE 1); every team breakable aura on the
// victim is kept, so the hit's removal of a later one counts when the first is spared (NOTE 6). The break is counted
// in OnAuraRemove, only when that damage really removed the aura: the core spares the aura's own spell (frost nova's
// damage, a root's ticks) and spells with SPELL_ATTR4_DAMAGE_DOESNT_BREAK_AURAS, Unit.cpp DealDamage.
constexpr uint8 CC_PENDING_SLOTS = 2;  // the outer damage event and a nested one

struct CcPendingBreak
{
    ObjectGuid Source;             // the bot; looked up on the victim's map at the removal (no pointer is kept)
    Unit const* Victim = nullptr;  // compared, never dereferenced
    std::array<Aura const*, CC_PENDING_AURAS> Auras{};
    uint32 AtMs = 0;
    // For the cause and the break's log line: the damage's spell (0 melee) and whether it ticked, whether a pet dealt
    // it, what the bot aimed at
    SpellInfo const* DamageSpell = nullptr;
    bool Periodic = false;
    bool ByPet = false;
    bool OnTarget = false;
    bool KeptOut = false;
};

thread_local std::array<CcPendingBreak, CC_PENDING_SLOTS> ccPendingBreaks;
// The spell of the damage being dealt on this thread and whether it is a periodic tick (ModifySpellDamageTaken /
// ModifyPeriodicDamageAurasTick set it, ModifyMeleeDamage clears it); read only for a pending break's cause
thread_local SpellInfo const* ccDamageSpell = nullptr;
thread_local bool ccDamagePeriodic = false;

// G4c-b: what broke it, for the harness's tactics.ownBreakPet / ownBreakAoe / ownBreakDot (aimed: none of them)
char const* CcCountBreakCause(PlayerbotAI* botAI, CcPendingBreak const& pending)
{
    if (pending.ByPet)
    {
        botAI->CountTactic(TacticCounter::OwnBreakPet);
        return "pet";
    }

    if (pending.DamageSpell && CcResolveSpellArea(pending.DamageSpell).Centre != CcAreaCentre::None)
    {
        botAI->CountTactic(TacticCounter::OwnBreakAoe);
        return "area";
    }

    if (pending.Periodic)
    {
        botAI->CountTactic(TacticCounter::OwnBreakDot);
        return "periodic";
    }

    return "aimed";
}

// Whether a pending damage event removed the aura (counted when its bot counts)
bool CcConsumeOwnBreak(Unit const* unit, Aura const* aura, AuraRemoveMode mode)
{
    for (CcPendingBreak& pending : ccPendingBreaks)
    {
        if (pending.Victim != unit)
            continue;

        auto const hit = std::find(pending.Auras.begin(), pending.Auras.end(), aura);
        if (hit == pending.Auras.end())
            continue;

        *hit = nullptr;
        // RemoveAurasWithInterruptFlags removes with the default mode, in the same DealDamage call as the hook
        if (mode != AURA_REMOVE_BY_DEFAULT || getMSTimeDiff(pending.AtMs, getMSTime()) > 1)
            return false;

        Player* const source = ObjectAccessor::GetPlayer(*unit, pending.Source);
        PlayerbotAI* const botAI = source ? GET_PLAYERBOT_AI(source) : nullptr;
        if (!botAI || !botAI->IsPvpTestCounting())
            return true;

        CcBotState& state = botAI->GetCcState();
        if (state.OwnBreakAura == aura && state.OwnBreakApplyTime == aura->GetApplyTime())
            return true;

        state.OwnBreakAura = aura;
        state.OwnBreakApplyTime = aura->GetApplyTime();
        botAI->CountPvpTestOwnBreak();
        char const* const cause = CcCountBreakCause(botAI, pending);
        Unit const* const caster = aura->GetCaster();
        // Diagnostic only, at trace level (Logger.playerbots=6): the dist's level 5 keeps it out of the default logs
        LOG_TRACE(
            "playerbots",
            "CC own break: {} broke {} ({}) on {} by {} {} ({}), {}, aura {} ms old, bot target {}, kept out "
            "{}, cache {} ms",
            source->GetName(), aura->GetSpellInfo()->SpellName[0], caster ? caster->GetName() : "?", unit->GetName(),
            pending.ByPet ? "pet" : "self", pending.DamageSpell ? pending.DamageSpell->SpellName[0] : "melee",
            pending.DamageSpell ? pending.DamageSpell->Id : 0, cause, aura->GetMaxDuration() - aura->GetDuration(),
            pending.OnTarget ? "yes" : "no", pending.KeptOut ? "yes" : "no",
            FocusBefore(pending.AtMs, state.ExcludedUntilMs)
                ? static_cast<int32>(CC_EXCLUDE_REFRESH_MS - (state.ExcludedUntilMs - pending.AtMs))
                : -1);
        return true;
    }

    return false;
}

// G4c-b opponent memory (Cc.OpponentMemoryEnable): the team's loss of control on an enemy player left early and not by
// damage: a dispel (AURA_REMOVE_BY_ENEMY_SPELL; its delay from landing) or a trinket (CcSelfTrinketOn). Each DR-aware
// team bot that sees it remembers it for this match; the harness counts it per bot (oppDispels, oppTrinkets).
void CcNoteOpponentAnswer(Unit* unit, Player* caster, Aura const* aura, AuraRemoveMode mode, uint32 nowMs)
{
    bool const dispel = mode == AURA_REMOVE_BY_ENEMY_SPELL;
    if (!dispel && (mode != AURA_REMOVE_BY_DEFAULT || !CcSelfTrinketOn(unit)))
        return;

    uint64 const enemy = unit->GetGUID().GetRawValue();
    uint32 const matchKey = unit->GetInstanceId();
    uint32 const afterMs =
        static_cast<uint32>(std::max<int32>(aura->GetMaxDuration() - std::max<int32>(aura->GetDuration(), 0), 0));
    CcForEachObserver(unit, caster,
                      [&](Player* member, CcBotState& state)
                      {
                          // G5a: a trinket its immunity aura already showed (CcNoteEnemyTrinket) is not counted again
                          bool learned = true;
                          if (dispel)
                              state.Opponents.Dispelled(matchKey, enemy, afterMs, nowMs);
                          else
                              learned = state.Opponents.Trinketed(matchKey, enemy, nowMs);

                          PlayerbotAI* const memberAI = GET_PLAYERBOT_AI(member);
                          if (learned && memberAI && memberAI->IsPvpTestCounting())
                              memberAI->CountTactic(dispel ? TacticCounter::OppDispels : TacticCounter::OppTrinkets);
                      });
    LOG_TRACE("playerbots", "CC answer: {} {} {} ({}) after {} ms, at {} ms", unit->GetName(),
              dispel ? "dispelled" : "trinketed", aura->GetSpellInfo()->SpellName[0], caster->GetName(), afterMs,
              nowMs);
}

// G5a review: a CC that a group bot's dispel cc on party was cast for left the unit by a dispel within
// CC_DISPEL_CONFIRM_MS: that bot counts it (cc.partnerDispel, tactics.partnerDispelMs for a partner) and logs it. The
// unit's own bot first (a root on itself), then its group's bots on its map (the same map thread)
void CcNoteDispelRemoval(Unit* unit, Aura const* aura)
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    Player* const player = unit->ToPlayer();
    if (!player || !config.TacticsEnable ||
        !(config.TacticConfig[static_cast<uint32>(TacticId::Dispel)].Enable || config.PvpTestEnable))
        return;

    uint32 const nowMs = getMSTime();
    uint64 const target = unit->GetGUID().GetRawValue();
    auto const confirm = [&](Player* member)
    {
        PlayerbotAI* const memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            return false;

        CcBotState& state = memberAI->GetCcState();
        if (!CcDispelFreed(state.PendingDispel, target, aura->GetId(), true, nowMs, CC_DISPEL_CONFIRM_MS))
            return false;

        uint32 const ageMs =
            static_cast<uint32>(std::max<int32>(aura->GetMaxDuration() - std::max<int32>(aura->GetDuration(), 0), 0));
        if (member != player)
        {
            ++state.Dispels;
            state.LastDispelAgeMs = ageMs;
            memberAI->CountPvpTestPartnerDispel(ageMs);
        }

        LOG_TRACE("playerbots", "Dispel CC: {} freed {} {} ({}) after {} ms with {} cast {} ms before, at {} ms",
                  member->GetName(), member == player ? std::string("itself") : player->GetName(),
                  aura->GetSpellInfo()->SpellName[0], aura->GetId(), ageMs,
                  state.PendingDispel.Spell ? state.PendingDispel.Spell : "?", nowMs - state.PendingDispel.CastMs,
                  nowMs);
        state.PendingDispel = CcPendingDispel();
        return true;
    };

    if (confirm(player))
        return;

    Group* const group = player->GetGroup();
    if (!group)
        return;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* const member = ref->GetSource();
        if (member && member != player && member->IsInMap(player) && confirm(member))
            return;
    }
}

// G5a: the spells of a CC break (the PvP trinkets, every man for himself, will of the forsaken: a self-cast mechanic
// immunity; escape artist: a script)
constexpr uint32 SPELL_PVP_TRINKET = 42292;
constexpr uint32 SPELL_PVP_TRINKET_OTHER = 65547;
constexpr uint32 SPELL_EVERY_MAN = 59752;
constexpr uint32 SPELL_FORSAKEN = 7744;
constexpr uint32 SPELL_ESCAPE_ARTIST = 20589;

constexpr bool CcTrinketUseSpell(uint32 spellId)
{
    return spellId == SPELL_PVP_TRINKET || spellId == SPELL_PVP_TRINKET_OTHER || spellId == SPELL_EVERY_MAN ||
           spellId == SPELL_FORSAKEN;
}

// G5a opponent memory (Cc.OpponentMemoryEnable): an enemy player's trinket use seen by its immunity aura landing
// (DT.12: the combat log shows it), whether or not a CC of the team was on it. Every hostile bot on the battleground
// or arena map with the Cc tactic and DR awareness remembers it (Trinkets, LastTrinketMs: TrinketReadyMs); the same
// use seen again by the CC it removed counts once (CcOpponentMemory::Trinketed). The open world keeps the removal
// source only (no walk over a continent's players).
void CcNoteEnemyTrinket(Unit* unit, Aura const* aura)
{
    if (!unit->IsPlayer() || aura->GetCasterGUID() != unit->GetGUID())
        return;

    Map* const map = unit->GetMap();
    if (!map || !map->IsBattlegroundOrArena())
        return;

    uint64 const enemy = unit->GetGUID().GetRawValue();
    uint32 const matchKey = unit->GetInstanceId();
    uint32 const nowMs = getMSTime();
    uint32 seenBy = 0;
    Map::PlayerList const& players = map->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* const member = itr->GetSource();
        PlayerbotAI* const memberAI =
            member && member != unit && member->IsHostileTo(unit) ? GET_PLAYERBOT_AI(member) : nullptr;
        if (!memberAI || !memberAI->TacticEnabledHere(TacticId::Cc))
            continue;

        CcBotState& state = memberAI->GetCcState();
        if (!state.Params.DrAware || !state.Opponents.Trinketed(matchKey, enemy, nowMs))
            continue;

        ++seenBy;
        memberAI->CountTactic(TacticCounter::OppTrinkets);
    }

    LOG_TRACE("playerbots", "CC answer: {} used {} ({}), seen by {} bots, ready again at {} ms, at {} ms",
              unit->GetName(), aura->GetSpellInfo()->SpellName[0], aura->GetId(), seenBy,
              nowMs + CC_OPPONENT_TRINKET_COOLDOWN_MS, nowMs);
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
        ccDamagePeriodic = true;
    }

    void ModifyMeleeDamage(Unit* /*target*/, Unit* /*attacker*/, uint32& /*damage*/) override
    {
        ccDamageSpell = nullptr;
        ccDamagePeriodic = false;
    }

    void ModifySpellDamageTaken(Unit* /*target*/, Unit* /*attacker*/, int32& /*damage*/,
                                SpellInfo const* spellInfo) override
    {
        ccDamageSpell = spellInfo;
        ccDamagePeriodic = false;
    }

    // Fires for every aura of every unit: the config flags, then the aura's DR group, before anything else
    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!CcFeedOn())
            return;

        // G5a: an enemy's trinket use, seen by its immunity aura (a spell id test first; no DR group)
        if (CcTrinketUseSpell(aura->GetId()))
        {
            if (sPlayerbotAIConfig.TacticCc.CcOpponentMemoryEnable)
                CcNoteEnemyTrinket(unit, aura);

            return;
        }

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
    // aura of the group still on the unit; the team's control ends now, or with its other loss-of-control CC there;
    // a dispel or a trinket goes into the opponent memory (G4c-b)
    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        Aura const* const aura = aurApp->GetBase();
        bool const byDamage = CcConsumeOwnBreak(unit, aura, mode);
        // G5a review: a CC that dispel cc on party was cast for goes by a dispel (counted there, not at the cast)
        if (mode == AURA_REMOVE_BY_ENEMY_SPELL)
            CcNoteDispelRemoval(unit, aura);

        // An expiry feeds only the harness's ccHeldMs (G4c-a review NOTE 3: without the harness G4b's cost)
        if (!CcFeedOn() || (mode == AURA_REMOVE_BY_EXPIRE && !sPlayerbotAIConfig.PvpTestEnable))
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

        if (control && !byDamage && sPlayerbotAIConfig.TacticCc.CcOpponentMemoryEnable && unit->IsPlayer())
            CcNoteOpponentAnswer(unit, caster, aura, mode, nowMs);
    }
};

// A cone's arc for the AoE guard: wider than the spells' (the guard errs on the side of not casting)
constexpr float CcGuardConeArc = static_cast<float>(M_PI);
constexpr uint32 CC_AREA_RETRY_MS = 10000;  // a cast action whose spell the bot does not know: asked again after this

// The spell id of a cast action (0: no cast action, or a spell the bot does not know yet)
uint32 CcActionSpellId(PlayerbotAI* botAI, Action* action, bool& castAction)
{
    CastSpellAction* const cast = dynamic_cast<CastSpellAction*>(action);
    castAction = cast != nullptr;
    return cast ? botAI->GetAiObjectContext()->GetValue<uint32>("spell id", cast->getSpell())->Get() : 0;
}

// G4c-b AoE guard (Avoid.AoeGuardEnable): mutes a spell action whose damage area (CcResolveSpellArea: around the bot,
// at the action's target, a cone or a chain; its radius + Avoid.AoeGuardMarginYards) covers an enemy the bot keeps out
// (the OwnCc list: the skill model and the own-break window apply). The area is resolved once per action, and only
// while the list holds an enemy (G4c-d); an action without one ends at that lookup. In "pvp cc", so the engine asks it
// before "skill" (no reaction for a muted action); added with its switch only (G4c-d).
class OwnCcAoeGuardMultiplier : public Multiplier
{
public:
    OwnCcAoeGuardMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "own cc aoe guard") {}

    float GetValue(Action* action) override
    {
        CcConfig const& config = sPlayerbotAIConfig.TacticCc;
        if (!config.AvoidAoeGuardEnable || action == nullptr)
            return 1.0f;

        std::vector<ObjectGuid> const& held = CcOwnExclusions(botAI);
        if (held.empty())
            return 1.0f;

        CcSpellArea const* const area = AreaOf(action);
        if (!area || area->Centre == CcAreaCentre::None)
            return 1.0f;

        Player* const bot = botAI->GetBot();
        Unit* const target = action->GetTarget();
        Unit const* const centre = target && target->IsInMap(bot) ? target : bot;
        float const marginYards = static_cast<float>(config.AvoidAoeGuardMarginYards);
        for (ObjectGuid const& guid : held)
        {
            Unit* const unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsInMap(bot))
                continue;

            if (CcAreaCovers(*area, marginYards, bot->GetDistance(unit), centre->GetDistance(unit),
                             bot->HasInArc(CcGuardConeArc, unit)))
                return 0.0f;
        }

        return 1.0f;
    }

private:
    // The action's spell (a cast action) and its area in the spell store; nullptr while the bot does not know the
    // spell (asked again CC_AREA_RETRY_MS later, a rank learned later; G4c-b review NOTE 2: no lookup per evaluation)
    CcSpellArea const* AreaOf(Action* action)
    {
        auto const it = areas.find(action);
        if (it != areas.end())
            return &it->second;

        uint32 const nowMs = getMSTime();
        auto const unknown = unknownSinceMs.find(action);
        if (unknown != unknownSinceMs.end() && getMSTimeDiff(unknown->second, nowMs) < CC_AREA_RETRY_MS)
            return nullptr;

        bool castAction = false;
        uint32 const spellId = CcActionSpellId(botAI, action, castAction);
        if (!castAction)
            return &areas.emplace(action, CcSpellArea{}).first->second;

        if (!spellId)
        {
            unknownSinceMs[action] = nowMs;
            return nullptr;
        }

        unknownSinceMs.erase(action);
        return &areas.emplace(action, CcResolveSpellArea(sSpellMgr->GetSpellInfo(spellId))).first->second;
    }

    // Per bot: the actions of a bot's context live as long as the bot, so the pointer is a stable key
    std::unordered_map<Action*, CcSpellArea> areas;
    std::unordered_map<Action*, uint32> unknownSinceMs;  // the lookup that found no spell id
};

// G4c-d F4 busy guard (Cc.ChainEnable): a chaining bot waiting for its chain's moment (cc ready's ChainDueAtMs, for
// CC_CHAIN_GUARD_KEEP_MS after its last look) starts no cast, channel or global cooldown of its own that ends after
// that moment; helpful spells (a heal) go. The team cc action is no cast action. The spell's busy time is read once
// per action. In "pvp cc", so the engine asks it before "skill"; added with its switch only.
class CcChainGuardMultiplier : public Multiplier
{
public:
    CcChainGuardMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "cc chain guard") {}

    float GetValue(Action* action) override
    {
        CcBotState& state = botAI->GetCcState();
        if (!state.ChainGuardUntilMs || action == nullptr)
            return 1.0f;

        uint32 const nowMs = getMSTime();
        if (!FocusBefore(nowMs, state.ChainGuardUntilMs))
        {
            state.ChainGuardUntilMs = 0;
            return 1.0f;
        }

        uint32 const busyMs = BusyOf(action, nowMs);
        if (!busyMs || !CcChainBlocksCast(nowMs, state.ChainDueAtMs, busyMs))
            return 1.0f;

        ++state.ChainGuardMutes;
        return 0.0f;
    }

private:
    // How long the action's spell keeps the bot from casting: its cast time, channel or global cooldown; 0 for no
    // cast action, a helpful spell, or (asked again CC_AREA_RETRY_MS later) a spell the bot does not know
    uint32 BusyOf(Action* action, uint32 nowMs)
    {
        auto const it = busyMs.find(action);
        if (it != busyMs.end())
            return it->second;

        auto const unknown = unknownSinceMs.find(action);
        if (unknown != unknownSinceMs.end() && getMSTimeDiff(unknown->second, nowMs) < CC_AREA_RETRY_MS)
            return 0;

        bool castAction = false;
        uint32 const spellId = CcActionSpellId(botAI, action, castAction);
        if (castAction && !spellId)
        {
            unknownSinceMs[action] = nowMs;
            return 0;
        }

        unknownSinceMs.erase(action);
        SpellInfo const* const info = castAction ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        uint32 busy = 0;
        if (info && !info->IsPositive())
        {
            busy = std::max(info->CalcCastTime(), info->StartRecoveryTime);
            if (info->IsChanneled())
                busy = std::max<uint32>(busy, static_cast<uint32>(std::max(info->GetDuration(), 0)));
        }

        return busyMs.emplace(action, busy).first->second;
    }

    // Per bot: the actions of a bot's context live as long as the bot, so the pointer is a stable key
    std::unordered_map<Action*, uint32> busyMs;
    std::unordered_map<Action*, uint32> unknownSinceMs;  // the lookup that found no spell id
};

// G4c-b pet call-off (Avoid.PetCallOffEnable): the bot's pets and guardians stop attacking, and stop a cast at, an
// enemy the bot keeps out (at the list's refresh, on the bot's own update)
void CcCallOffPets(Player* bot, CcBotState& state)
{
    auto const held = [&](ObjectGuid const& guid)
    {
        return !guid.IsEmpty() && std::find(state.Excluded.begin(), state.Excluded.end(), guid) != state.Excluded.end();
    };
    for (Unit* const controlled : bot->m_Controlled)
    {
        if (!controlled || !controlled->IsInMap(bot))
            continue;

        bool calledOff = false;
        Unit* const victim = controlled->GetVictim();
        if (victim && held(victim->GetGUID()))
        {
            controlled->AttackStop();
            calledOff = true;
        }

        Spell const* const spell = controlled->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (spell && held(spell->m_targets.GetUnitTargetGUID()))
        {
            controlled->InterruptSpell(CURRENT_GENERIC_SPELL);
            calledOff = true;
        }

        if (calledOff)
            ++state.PetCallOffs;
    }
}
}  // namespace

bool CcOldActionName(std::string_view name, bool crowdControlAction)
{
    auto const& names = crowdControlAction ? std::span<std::string_view const>(OldCcSpellActions)
                                           : std::span<std::string_view const>(OldCcOtherActions);
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool CcImmuneBesidesCyclone(Unit const* target, SpellInfo const* spellInfo)
{
    auto const& allowList = target->m_spellImmune[IMMUNITY_ALLOW_ID];
    if (!allowList.empty() && !allowList.count(spellInfo->Id))
        return true;

    for (auto const& [id, spellId] : target->m_spellImmune[IMMUNITY_ID])
    {
        if (id == spellInfo->Id && !CcFromCyclone(spellId))
            return true;
    }

    for (auto const& [dispel, spellId] : target->m_spellImmune[IMMUNITY_DISPEL])
    {
        if (spellInfo->Dispel && dispel == spellInfo->Dispel && !CcFromCyclone(spellId))
            return true;
    }

    if (!spellInfo->HasAttribute(SPELL_ATTR0_CU_BYPASS_MECHANIC_IMMUNITY))
    {
        uint64 const mechanics = spellInfo->GetAllEffectsMechanicMask();
        for (auto const& [mechanic, spellId] : target->m_spellImmune[IMMUNITY_MECHANIC])
        {
            if (mechanic < 64 && (mechanics & (1ULL << mechanic)) && !CcFromCyclone(spellId))
                return true;
        }
    }

    for (auto const& [aura, spellId] : target->m_spellImmune[IMMUNITY_STATE])
    {
        if (CcFromCyclone(spellId))
            continue;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].IsEffect() && spellInfo->Effects[i].ApplyAuraName == aura)
                return true;
        }
    }

    // G4c-d re-review NOTE 5: an immunity to an effect type (Unit::IsImmunedToSpellEffect's effect list)
    for (auto const& [effect, spellId] : target->m_spellImmune[IMMUNITY_EFFECT])
    {
        if (CcFromCyclone(spellId))
            continue;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].IsEffect() && spellInfo->Effects[i].Effect == effect)
                return true;
        }
    }

    return false;
}

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
    bool const old = CcOldActionName(name, dynamic_cast<CastCrowdControlSpellAction*>(action) != nullptr);
    return oldCcActions.emplace(action, old).first->second;
}

void PvpCcStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TacticCcMultiplier(botAI));
    // G4c-d: the guards only with their switches (G4c-c review C2: the AoE guard cost as much as tactic cc); a switch
    // turned on later reaches a bot when its engine is built again
    CcConfig const& config = sPlayerbotAIConfig.TacticCc;
    if (config.AvoidAoeGuardEnable)
        multipliers.push_back(new OwnCcAoeGuardMultiplier(botAI));

    if (config.CcChainEnable)
        multipliers.push_back(new CcChainGuardMultiplier(botAI));
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

    // The skill model (CcBreaksOwnNow): below the known skill, or in the own-break window, the bot breaks it
    bool const breaksOwn = CcBreaksOwnNow(botAI, state, nowMs);
    if (breaksOwn)
        return state.Excluded;

    // What the bot fights: its attackers, its target and the skull (what the target picks read); G4c-b: with
    // Avoid.ExcludeFightEnemies every enemy of "fight enemies" (G4b-b review NOTE 7: a battleground's CC'd enemy that
    // is none of these), with Avoid.PetCallOffEnable its pets' victims
    CcConfig const& config = sPlayerbotAIConfig.TacticCc;
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
    if (config.AvoidExcludeFightEnemies)
    {
        for (ObjectGuid const& guid : context->GetValue<GuidVector>("fight enemies")->Get())
            consider(guid);
    }

    if (config.AvoidPetCallOffEnable)
    {
        for (Unit* const controlled : bot->m_Controlled)
        {
            Unit* const victim = controlled ? controlled->GetVictim() : nullptr;
            consider(victim ? victim->GetGUID() : ObjectGuid::Empty);
        }

        CcCallOffPets(bot, state);
    }

    return state.Excluded;
}

void CcSetForcedCasts(bool on) { ccForcedCasts.store(on, std::memory_order_relaxed); }

void CcCountOwnBreak(Unit* attacker, Unit* victim)
{
    // Every damage event of every unit: the victim's pending break goes first (a damage event that is not a counting
    // bot's leaves none, so its removal is never charged to a bot), then the harness switch, a player source, the aura
    // lists. Another victim's pending break stays (a damage shield's reverse damage nests in the outer hit).
    for (CcPendingBreak& pending : ccPendingBreaks)
    {
        if (pending.Victim == victim)
            pending.Victim = nullptr;
    }

    if (!sPlayerbotAIConfig.PvpTestEnable || !attacker || !victim || attacker == victim)
        return;

    Unit* const source = attacker->GetCharmerOrOwnerOrSelf();
    Player* const player = source ? source->ToPlayer() : nullptr;
    if (!player || source == victim)
        return;

    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(player);
    if (!botAI || !botAI->IsPvpTestCounting())
        return;

    // Counted only if this damage removes an aura (CcConsumeOwnBreak in OnAuraRemove)
    std::array<Aura const*, CC_PENDING_AURAS> auras{};
    if (!CcTeamBreakableAuras(victim, player, auras))
        return;

    // A free slot (none, or older than the removal's 1 ms), else the older one
    uint32 const nowMs = getMSTime();
    CcPendingBreak* slot = &ccPendingBreaks[0];
    for (CcPendingBreak& pending : ccPendingBreaks)
    {
        if (!pending.Victim || getMSTimeDiff(pending.AtMs, nowMs) > 1)
        {
            slot = &pending;
            break;
        }

        if (static_cast<int32>(pending.AtMs - slot->AtMs) < 0)
            slot = &pending;
    }

    std::vector<ObjectGuid> const& excluded = botAI->GetCcState().Excluded;
    *slot = CcPendingBreak{player->GetGUID(),
                           victim,
                           auras,
                           nowMs,
                           ccDamageSpell,
                           ccDamagePeriodic,
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
    state.PickIsChain = false;
    Player* const bot = botAI->GetBot();
    bool const guarding = state.ChainGuardUntilMs && FocusBefore(nowMs, state.ChainGuardUntilMs);
    if (!bot->IsAlive() || bot->IsNonMeleeSpellCast(false) || !botAI->TacticActive(TacticId::Cc))
    {
        // G4c-d F4: a cast of the bot's own still running at the chain's moment delays the chain (counted at its
        // landing, tactics.chainBusy); the guard stays while it runs
        if (guarding && bot->IsAlive() && bot->IsNonMeleeSpellCast(false))
        {
            state.ChainGuardUntilMs = nowMs + CC_CHAIN_GUARD_KEEP_MS;
            if (!FocusBefore(nowMs, state.ChainDueAtMs))
                state.ChainBusySeen = true;
        }

        state.ReadySinceMs = 0;
        return false;
    }

    CcRefreshTable(bot, state, nowMs);
    CcRowMask comboMask = 0;
    CcRowMask ready = CcReadyRows(bot, state, nowMs, comboMask);
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
    // the loss-of-control rows still go). G4c-d F1: a chaining bot goes on through its team's own cyclone (nothing
    // else keeps the target immune) and lands just after its end.
    ready = CcComboRowsOn(bot, state, ready, comboMask, target);
    CcConfig const& config = sPlayerbotAIConfig.TacticCc;
    bool const chains = params.Chains && config.CcChainEnable != 0;
    bool const immuneAll = CcImmuneAll(target);
    bool const throughCyclone = immuneAll && chains && CcImmuneOnlyByCyclone(target);
    if (!ready || (immuneAll && !throughCyclone) || CcTeamBehind(bot, params))
    {
        state.ReadySinceMs = 0;
        return false;
    }

    // A controlled target: only a chaining bot (G4c, Cc.ChainEnable) goes on, and only on its team's CC (the visible
    // aura that ends last: its remaining time and DR group); anyone else's control, or no chains, waits as in G4b. A
    // teammate's CC on his cast bar at the target counts as pending.
    CcChainView view;
    uint8 activeGroup = 0;
    if (CcUnitControlled(target) || throughCyclone)
    {
        uint32 elapsedMs = 0;
        view.AuraOnTarget = chains && CcTeamControlAura(target, bot, view.RemainingMs, elapsedMs, activeGroup);
        if (!view.AuraOnTarget)
        {
            state.ReadySinceMs = 0;
            return false;
        }

        // G4c-d F3: the bot starts its expected delay sooner; F1: after the cyclone's end by a margin with noise
        // (per bot and end)
        view.ReactMs = CcChainReactMs(botAI, state);
        view.ImmuneUntilEnd = throughCyclone;
        if (throughCyclone)
            view.AfterMs = CC_CHAIN_AFTER_MIN_MS +
                           static_cast<uint32>(SplitMix64(bot->GetGUID().GetRawValue() ^ CcSalt::ChainAfter ^
                                                          ((nowMs + view.RemainingMs) / CC_READY_REFRESH_MS)) %
                                               (CC_CHAIN_AFTER_SPREAD_MS + 1));

        // G4c-b: the opponent memory moves the chain's moment before the dispel this enemy's CC got so far (not a
        // cyclone's: it ends the immunity only at its end)
        if (config.CcOpponentMemoryEnable && !throughCyclone)
            view.RemainingMs =
                CcChainRemainingMs(view.RemainingMs, elapsedMs,
                                   state.Opponents.DispelMeanMs(bot->GetInstanceId(), target->GetGUID().GetRawValue(),
                                                                config.CcOpponentMinEvents),
                                   config.CcDispelMarginMs);
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
    // A chain adds control only (G4c-a review NOTE 1: a root's damage would break the team's CC and control nothing)
    input.ControlOnly = (onMoon && target == ccTarget) || view.AuraOnTarget;
    input.ActiveGroup = activeGroup;
    // G4c-b DotGuard: no CC that damage breaks on a target the team's periodic damage hits (a bot that keeps its
    // team's CC)
    input.TeamDotOn = config.AvoidDotGuardEnable && botAI->TacticEnabledHere(TacticId::OwnCc) &&
                      !CcBreaksOwnNow(botAI, state, nowMs) && CcTeamDotOn(target, bot);
    if (params.DrAware)
        input.Levels = state.Memory.Levels(target->GetGUID().GetRawValue(), nowMs);

    // A chaining bot whose memory leaves no group that lands (immune, too short) has no row and waits
    int8 const row = PickCc(input, state.Spells, params);
    if (row == CC_NO_ROW)
        return false;

    // G4c-d re-review NOTE 4: through the team's cyclone only when nothing else keeps the target immune to the row
    // (a cycloned warrior in berserker rage: no pick, so the busy guard does not hold the bot's casts for nothing)
    if (throughCyclone)
    {
        SpellInfo const* const info = sSpellMgr->GetSpellInfo(state.KnownSpell[row]);
        if (!info || CcImmuneBesidesCyclone(target, info))
            return false;
    }

    // The chain's moment: the CC lands ChainLeadMs before the team's CC ends; the answer is asked again at that
    // moment rather than a whole refresh later (the engine's cadence and the reaction model add the spread, DT.11)
    uint32 const castMs = CcCastMsNow(bot, state, row);
    if (chains && !ChainDue(view, castMs, params.ChainLeadMs, true))
    {
        ++state.ChainWaits;
        uint32 const waitMs = CcChainWaitMs(view, castMs, params.ChainLeadMs);
        if (waitMs && waitMs < CC_READY_REFRESH_MS)
            state.PickUntilMs = nowMs + waitMs;

        // G4c-d F4: until the moment the busy guard holds the bot's casts that would end after it
        if (waitMs)
        {
            if (!guarding)
                state.ChainBusySeen = false;

            state.ChainDueAtMs = nowMs + waitMs;
            state.ChainGuardUntilMs = nowMs + CC_CHAIN_GUARD_KEEP_MS;
        }

        return false;
    }

    if (!bot->IsWithinLOSInMap(target))
        return false;

    // G4c-d: a chain due now: the guard holds the bot's other casts until it goes, the moment is kept for the delay
    // the bot learns, and on the team's cyclone the cast waits until it lands after the end (TeamCcAction::isPossible)
    state.ChainImmuneEndMs = 0;
    if (view.AuraOnTarget)
    {
        if (!guarding)
            state.ChainBusySeen = false;

        state.ChainDueAtMs = nowMs;
        state.ChainGuardUntilMs = nowMs + CC_CHAIN_GUARD_KEEP_MS;
        if (!state.ChainDueSinceMs || getMSTimeDiff(state.ChainDueSinceMs, nowMs) > CC_CHAIN_REACT_CAP_MS)
        {
            state.ChainDueSinceMs = nowMs;
            state.ChainHeldImmune = false;
        }

        if (view.ImmuneUntilEnd)
            state.ChainImmuneEndMs = nowMs + view.RemainingMs + view.AfterMs;
    }

    state.PickRow = row;
    state.PickIsChain = view.AuraOnTarget;
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

    // G4c-d F1: a chain onto the team's cyclone goes only when it lands after its end (the hasted cast time)
    if (state.ChainImmuneEndMs &&
        CcLandsInImmunity(getMSTime(), CcCastMsNow(botAI->GetBot(), state, state.PickRow), state.ChainImmuneEndMs))
    {
        state.ChainHeldImmune = true;
        return false;
    }

    Unit* const target = GetTarget();
    uint32 const spellId = state.KnownSpell[state.PickRow];
    return target && spellId && botAI->CanCastSpell(spellId, target, true);
}

bool TeamCcAction::Execute(Event /*event*/)
{
    CcBotState& state = botAI->GetCcState();
    int8 const row = state.PickRow;
    bool const pickedChain = state.PickIsChain;
    Unit* const target = GetTarget();
    state.PickRow = CC_NO_ROW;
    state.PickIsChain = false;
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

    // G4c-d F2/F3: the cast's aura (only its landing counts as the chain the bot picked: noted at the pick) and the
    // delay from the chain's moment, learned unless a cast of the bot's own or the cyclone held it
    uint32 const nowMs = getMSTime();
    bool const chain = pickedChain;
    // G4d: one line per cast (the run counts the casts per spell; the records' cc.cast counts per DR group)
    if (sLog->ShouldLog("playerbots", LogLevel::LOG_LEVEL_TRACE))
    {
        SpellInfo const* const info = sSpellMgr->GetSpellInfo(state.KnownSpell[row]);
        uint8 const group = state.Spells[row].Group;
        LOG_TRACE("playerbots", "CC cast: {} {} ({}, row {} {}) on {} group {} ({}){}, ok at {} ms", bot->GetName(),
                  info && info->SpellName[0] ? info->SpellName[0] : "?", state.KnownSpell[row], CC_OPTIONS[row].Action,
                  CC_OPTIONS[row].SpellId, target->GetName(),
                  group < PVPTEST_DR_GROUP_COUNT ? PVPTEST_DR_GROUP_NAMES[group] : "?", group, chain ? " chain" : "",
                  nowMs);
    }

    if (chain && state.ChainDueSinceMs && getMSTimeDiff(state.ChainDueSinceMs, nowMs) <= CC_CHAIN_REACT_CAP_MS &&
        !state.ChainBusySeen && !state.ChainHeldImmune)
    {
        state.ChainReactMs = CcLearnChainReactMs(state.ChainReactMs, getMSTimeDiff(state.ChainDueSinceMs, nowMs),
                                                 state.ChainReactLearned);
        state.ChainReactLearned = true;
    }

    state.CastAuraSpell = state.Spells[row].AuraSpellId ? state.Spells[row].AuraSpellId : state.KnownSpell[row];
    state.CastAtMs = nowMs;
    state.CastWasChain = chain;
    state.CastWasBusy = chain && state.ChainBusySeen;
    state.ChainDueSinceMs = 0;
    state.ChainGuardUntilMs = 0;
    state.ChainImmuneEndMs = 0;
    state.ChainBusySeen = false;
    state.ChainHeldImmune = false;
    state.ReadySinceMs = 0;
    ++state.Casts;
    botAI->NoteTacticUse(TacticId::Cc);
    return true;
}

// ---- G5a: CC avoidance (the Trinket and Dispel tactics) -----------------------------------------------------------

namespace
{
// Above the racials (+ 1: the old racial triggers' relevance, and the racials' share of cc matters, DT.3)
constexpr float PvpTrinketRelevance = ACTION_EMERGENCY + 2.0f;
constexpr float RacialBreakRelevance = ACTION_EMERGENCY + 1.0f;
// Above the class cleanses (priest 40/41, paladin ACTION_DISPEL + 1/2, shaman 23/24) and the heals, below the breaks
constexpr float DispelCcRelevance = ACTION_EMERGENCY;
constexpr float CcDispelRangeYards = 30.0f;   // dispel magic, cleanse, cleanse spirit, cure toxins
constexpr float CcPartnerRangeYards = 40.0f;  // a partner further away does not make the bot's CC matter

// The CC aura types read on a unit and their CcKindOfAura bits
constexpr std::array<std::pair<AuraType, uint32>, 9> CcAuraTypes = {{{SPELL_AURA_MOD_STUN, CC_AURA_STUN},
                                                                     {SPELL_AURA_MOD_FEAR, CC_AURA_FEAR},
                                                                     {SPELL_AURA_MOD_CONFUSE, CC_AURA_CONFUSE},
                                                                     {SPELL_AURA_MOD_CHARM, CC_AURA_CHARM},
                                                                     {SPELL_AURA_AOE_CHARM, CC_AURA_CHARM},
                                                                     {SPELL_AURA_MOD_ROOT, CC_AURA_ROOT},
                                                                     {SPELL_AURA_MOD_DECREASE_SPEED, CC_AURA_SNARE},
                                                                     {SPELL_AURA_MOD_SILENCE, CC_AURA_SILENCE},
                                                                     {SPELL_AURA_MOD_PACIFY_SILENCE, CC_AURA_SILENCE}}};

// A harmful CC aura on a unit as CcKindOfAura reads it
struct CcAuraSeen
{
    Aura const* Base = nullptr;
    CcKind Kind = CcKind::Root;
    uint64 Mechanics = 0;
    uint32 LeftMs = 0;
    uint32 AgeMs = 0;
};

uint32 CcAuraBits(SpellInfo const* info)
{
    uint32 bits = CcFromCyclone(info->Id) ? CC_AURA_CYCLONE : 0;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (!info->Effects[i].IsAura())
            continue;

        for (auto const& entry : CcAuraTypes)
        {
            if (info->Effects[i].ApplyAuraName == entry.first)
                bits |= entry.second;
        }
    }

    return bits;
}

// The unit's harmful CC auras, read by aura type (no walk over all its auras); an aura with two CC effects comes twice
template <typename Fn>
void CcForEachCcAura(Unit const* unit, Fn const& fn)
{
    for (auto const& entry : CcAuraTypes)
    {
        for (AuraEffect const* effect : unit->GetAuraEffectsByType(entry.first))
        {
            Aura const* const aura = effect->GetBase();
            SpellInfo const* const info = aura->GetSpellInfo();
            if (info->IsPositive())
                continue;

            CcAuraSeen seen;
            seen.Base = aura;
            seen.Mechanics = info->GetAllEffectsMechanicMask();
            if (!CcKindOfAura(seen.Mechanics, CcAuraBits(info), seen.Kind))
                continue;

            int32 const leftMs = aura->GetDuration();
            seen.LeftMs = leftMs < 0 ? std::numeric_limits<uint32>::max() : static_cast<uint32>(leftMs);
            seen.AgeMs = static_cast<uint32>(std::max<int32>(aura->GetMaxDuration() - std::max<int32>(leftMs, 0), 0));
            fn(seen);
        }
    }
}

// Known at the bot's skill (the gate's KnownFrom; full ability knows every tactic), whatever this window's use roll
bool CcTrinketKnown(PlayerbotAI* botAI)
{
    return (botAI->GetTacticGate().KnownMask & TacticBit(TacticId::Trinket)) != 0 || botAI->HasFullSkillAbility();
}

// The worn PvP trinket (either trinket slot), or nullptr
Item* CcWornPvpTrinket(Player* bot)
{
    for (uint8 const slot : {EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2})
    {
        Item* const item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (item && CcIsPvpTrinket(item->GetTemplate()))
            return item;
    }

    return nullptr;
}

bool CcTrinketReady(Player* bot, CcBotState const& state)
{
    if (state.TrinketWithheld || bot->HasSpellCooldown(SPELL_PVP_TRINKET))
        return false;

    Item* const item = CcWornPvpTrinket(bot);
    return item && bot->CanUseItem(item) == EQUIP_ERR_OK;
}

bool CcRacialReady(Player* bot, uint32 spellId) { return bot->HasSpell(spellId) && !bot->HasSpellCooldown(spellId); }

// A partner (another group member on the bot's map, alive, within CcPartnerRangeYards) at or below the line
bool CcPartnerLow(Player* bot, uint8 lowHealthPct)
{
    Group* const group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* const member = ref->GetSource();
        if (member && member != bot && member->IsAlive() && member->IsInMap(bot) &&
            member->GetHealthPct() <= lowHealthPct && bot->IsWithinDistInMap(member, CcPartnerRangeYards))
            return true;
    }

    return false;
}

// The bot's worst CC: a loss of control before the rest, then the most time left
bool CcWorstOnSelf(Player* bot, CcAuraSeen& worst)
{
    bool found = false;
    CcForEachCcAura(bot,
                    [&](CcAuraSeen const& seen)
                    {
                        bool const loses = CcKindLosesControl(seen.Kind);
                        bool const worstLoses = found && CcKindLosesControl(worst.Kind);
                        if (!found || (loses && !worstLoses) || (loses == worstLoses && seen.LeftMs > worst.LeftMs))
                        {
                            worst = seen;
                            found = true;
                        }
                    });
    return found;
}

// dispel cc on party's pick (CcDispelPick's refresh): self first (only a root: a loss of control keeps the bot from
// casting), then the partners in group order (CcDispelBetter: a loss of control before a root), a CC the class's
// spells remove with CC_DISPEL_MIN_LEFT_MS or more left, a partner in range and sight
void CcRefreshDispelPick(PlayerbotAI* botAI, CcBotState& state, uint32 nowMs)
{
    state.DispelUntilMs = nowMs + CC_DISPEL_REFRESH_MS;
    state.DispelTarget.Clear();
    state.DispelAuraSpell = 0;
    state.DispelSpell = nullptr;
    Player* const bot = botAI->GetBot();
    if (!bot->IsAlive() || bot->IsNonMeleeSpellCast(false) || bot->HasUnitState(UNIT_STATE_CONTROLLED) ||
        !botAI->TacticActive(TacticId::Dispel))
        return;

    uint8 const classId = bot->getClass();
    CcCleanserSpells known;
    switch (classId)
    {
        case CLASS_PRIEST:
            known.DispelMagic = botAI->HasSpell("dispel magic");
            break;
        case CLASS_PALADIN:
            known.Cleanse = botAI->HasSpell("cleanse");
            known.Purify = botAI->HasSpell("purify");
            break;
        case CLASS_SHAMAN:
            known.CleanseSpirit = botAI->HasSpell("cleanse spirit");
            known.CureToxins = botAI->HasSpell("cure toxins");
            break;
        default:
            return;
    }

    CcDispelCandidate best;
    auto const offer = [&](Unit* unit, bool rootsOnly)
    {
        CcDispelCandidate candidate;
        uint32 auraSpell = 0;
        char const* cast = nullptr;
        CcForEachCcAura(unit,
                        [&](CcAuraSeen const& seen)
                        {
                            if (seen.LeftMs < CC_DISPEL_MIN_LEFT_MS || (rootsOnly && CcKindLosesControl(seen.Kind)))
                                return;

                            char const* const spell =
                                CcDispelSpellFor(classId, seen.Base->GetSpellInfo()->Dispel, known);
                            CcDispelCandidate const mine{spell != nullptr, seen.Kind};
                            if (CcDispelBetter(mine, candidate))
                            {
                                candidate = mine;
                                auraSpell = seen.Base->GetId();
                                cast = spell;
                            }
                        });
        if (!CcDispelBetter(candidate, best) ||
            (unit != bot && (!bot->IsWithinDistInMap(unit, CcDispelRangeYards) || !bot->IsWithinLOSInMap(unit))))
            return;

        best = candidate;
        state.DispelTarget = unit->GetGUID();
        state.DispelAuraSpell = auraSpell;
        state.DispelSpell = cast;
    };

    offer(bot, true);
    Group* const group = bot->GetGroup();
    if (!group)
        return;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* const member = ref->GetSource();
        if (member && member != bot && member->IsAlive() && member->IsInMap(bot))
            offer(member, false);
    }
}

// The PvP trinkets of the item store (Avoid.EnsureTrinket's supply), the highest item level first; built once, on
// the world thread at the first harness match start that asks
std::vector<uint32> const& CcPvpTrinketItems()
{
    static std::vector<uint32> const items = []()
    {
        std::vector<std::pair<uint32, uint32>> found;
        for (auto const& [id, proto] : *sObjectMgr->GetItemTemplateStore())
        {
            if (proto.InventoryType == INVTYPE_TRINKET && CcIsPvpTrinket(&proto))
                found.emplace_back(proto.ItemLevel, id);
        }

        std::sort(found.begin(), found.end(), std::greater<>());
        std::vector<uint32> ids;
        for (auto const& entry : found)
            ids.push_back(entry.second);

        return ids;
    }();
    return items;
}
}  // namespace

bool CcIsPvpTrinket(ItemTemplate const* proto)
{
    if (!proto)
        return false;

    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (proto->Spells[i].SpellId == static_cast<int32>(SPELL_PVP_TRINKET))
            return true;
    }

    return false;
}

CcBreaker CcBreakAnswer(PlayerbotAI* botAI)
{
    if (!botAI->TacticEnabledHere(TacticId::Trinket))
        return CcBreaker::None;

    CcBotState& state = botAI->GetCcState();
    uint32 const nowMs = getMSTime();
    if (FocusBefore(nowMs, state.BreakUntilMs))
        return state.BreakWith;

    state.BreakUntilMs = nowMs + CC_BREAK_REFRESH_MS;
    state.BreakWith = CcBreaker::None;
    state.BreakAuraSpell = 0;
    // A bot that knows the tactic and forgot it this window (the gate's use roll) does nothing; below the known skill
    // a bot breaks without telling what matters (ShouldBreakCcUnknowing)
    Player* const bot = botAI->GetBot();
    bool const known = CcTrinketKnown(botAI);
    CcAuraSeen worst;
    if (!bot->IsAlive() || (known && !botAI->TacticActive(TacticId::Trinket)) || !CcWorstOnSelf(bot, worst))
        return CcBreaker::None;

    CcParams const& params = state.Params;
    CcBreakView view;
    view.Kind = worst.Kind;
    view.RemainingMs = worst.LeftMs;
    view.SelfLow = bot->GetHealthPct() <= params.BreakLowHealthPct;
    view.PartnerLow = !view.SelfLow && CcPartnerLow(bot, params.BreakLowHealthPct);
    view.Burst = false;  // G5d's "under burst" feeds it
    uint64 const seed = bot->GetGUID().GetCounter();
    if (!(known ? ShouldBreakCc(view, params, seed, nowMs) : ShouldBreakCcUnknowing(view, params, seed, nowMs)))
        return CcBreaker::None;

    CcBreakOptions ready;
    ready.Trinket = CcTrinketReady(bot, state);
    ready.EveryMan = CcRacialReady(bot, SPELL_EVERY_MAN);
    ready.Forsaken = CcRacialReady(bot, SPELL_FORSAKEN);
    ready.EscapeArtist = CcRacialReady(bot, SPELL_ESCAPE_ARTIST);
    state.BreakWith = PickCcBreaker(worst.Mechanics, ready);
    state.BreakMatters = known && CcMatters(view, params);
    state.BreakKind = worst.Kind;
    state.BreakAuraSpell = worst.Base->GetId();
    state.BreakAuraAgeMs = worst.AgeMs;
    state.BreakLeftMs = worst.LeftMs;
    return state.BreakWith;
}

void CcNoteBreakCast(PlayerbotAI* botAI, uint32 spellId)
{
    // A break the tactic answered (a racial of the old triggers outside its scope is not counted)
    CcBotState& state = botAI->GetCcState();
    if (!botAI->TacticEnabledHere(TacticId::Trinket) || state.BreakWith == CcBreaker::None || !state.BreakAuraSpell)
        return;

    uint32 const nowMs = getMSTime();
    uint32 const answeredMs = state.BreakUntilMs - CC_BREAK_REFRESH_MS;
    uint32 const ageMs = state.BreakAuraAgeMs + (FocusBefore(answeredMs, nowMs) ? nowMs - answeredMs : 0);
    ++state.Breaks;
    state.LastBreakAgeMs = ageMs;
    if (!state.BreakMatters)
    {
        ++state.WastedBreaks;
        botAI->CountTactic(TacticCounter::BreakWasted);
    }

    botAI->CountTactic(TacticCounter::BreakMs, ageMs);
    Player* const bot = botAI->GetBot();
    SpellInfo const* const auraInfo = sSpellMgr->GetSpellInfo(state.BreakAuraSpell);
    LOG_TRACE("playerbots",
              "CC break: {} {} ({}) broke {} ({}, kind {}) after {} ms, {} ms left, {}, health {:.0f} %, at {} ms",
              bot->GetName(), CcBreakerAction(state.BreakWith), spellId, auraInfo ? auraInfo->SpellName[0] : "?",
              state.BreakAuraSpell, static_cast<uint32>(state.BreakKind), ageMs, state.BreakLeftMs,
              state.BreakMatters ? "mattered" : "wasted", bot->GetHealthPct(), nowMs);
    state.BreakWith = CcBreaker::None;
    state.BreakAuraSpell = 0;
    state.BreakUntilMs = 0;
}

Unit* CcDispelPick(PlayerbotAI* botAI)
{
    if (!botAI->TacticEnabledHere(TacticId::Dispel))
        return nullptr;

    CcBotState& state = botAI->GetCcState();
    uint32 const nowMs = getMSTime();
    if (!FocusBefore(nowMs, state.DispelUntilMs))
        CcRefreshDispelPick(botAI, state, nowMs);

    if (state.DispelTarget.IsEmpty())
        return nullptr;

    Unit* const unit = botAI->GetUnit(state.DispelTarget);
    return unit && unit->IsInWorld() && unit->IsAlive() ? unit : nullptr;
}

namespace
{
// The given trinket goes, the item it replaced back into its slot when that is free (the restore, the login cleanup;
// as Player::SwapItem moves an item from a bag into an equipment slot, no combat check: the bot's own item)
void CcPutBackTrinket(Player* bot, Item* given, Item* replaced, uint8 slot)
{
    if (given)
        bot->DestroyItem(given->GetBagSlot(), given->GetSlot(), true);

    if (!replaced || replaced->IsEquipped() || slot >= EQUIPMENT_SLOT_END ||
        bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        return;

    bot->RemoveItem(replaced->GetBagSlot(), replaced->GetSlot(), true);
    bot->EquipItem((static_cast<uint16>(INVENTORY_SLOT_BAG_0) << 8) | slot, replaced, true);
}

// A trinket Avoid.EnsureTrinket gave (CcEnsureTrinket's mark: a PvP trinket whose creator is the bot itself)
bool CcEnsuredMark(Player* bot, Item* item)
{
    return item && CcIsPvpTrinket(item->GetTemplate()) && item->GetGuidValue(ITEM_FIELD_CREATOR) == bot->GetGUID();
}
}  // namespace

bool CcEnsureTrinket(PlayerbotAI* botAI, uint32 mode)
{
    CcRestoreTrinket(botAI);  // a leftover of an earlier match
    CcBotState& state = botAI->GetCcState();
    if (mode == 2)
    {
        state.TrinketWithheld = true;
        return false;
    }

    // A dead bot's leftover (restored once it is alive) keeps its slot
    Player* const bot = botAI->GetBot();
    if (mode != 1 || !bot->IsAlive() || !state.EnsuredTrinket.IsEmpty() || CcWornPvpTrinket(bot) ||
        bot->getRace() == RACE_HUMAN || bot->getRace() == RACE_UNDEAD_PLAYER)
        return false;

    // The highest item level the bot can wear and use (G5a review: level, class, race, faction; none that needs a
    // reputation or an honor rank, which CanUseItem(ItemTemplate) does not test)
    uint32 itemId = 0;
    for (uint32 const id : CcPvpTrinketItems())
    {
        ItemTemplate const* const proto = sObjectMgr->GetItemTemplate(id);
        if (proto && proto->RequiredLevel <= bot->GetLevel() && !proto->RequiredReputationFaction &&
            !proto->RequiredHonorRank && bot->CanUseItem(proto) == EQUIP_ERR_OK)
        {
            itemId = id;
            break;
        }
    }

    if (!itemId)
    {
        LOG_DEBUG("playerbots", "Avoid.EnsureTrinket: no PvP trinket {} can wear", bot->GetName());
        return false;
    }

    // G5a review: PvP gear's wear (at the arena entry, before this) tracks its loadout items by slot; a slot without
    // one is taken first (CcEnsureTrinketSlot), a tracked item comes back before PvP gear looks again (the leave)
    PvpGearMgr const& gear = PvpGearMgr::Instance();
    Item* const first = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1);
    Item* const second = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET2);
    uint8 const slot = CcEnsureTrinketSlot(!first, gear.IsWornItem(bot, first), !second, gear.IsWornItem(bot, second))
                           ? EQUIPMENT_SLOT_TRINKET2
                           : EQUIPMENT_SLOT_TRINKET1;
    Item* const old = slot == EQUIPMENT_SLOT_TRINKET1 ? first : second;
    if (old)
    {
        ItemPosCountVec dest;
        if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, old, false) != EQUIP_ERR_OK)
        {
            LOG_DEBUG("playerbots", "Avoid.EnsureTrinket: {} has no bag room for its trinket", bot->GetName());
            return false;
        }

        state.EnsuredReplaced = old->GetGUID();
        bot->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
        bot->StoreItem(dest, old, true);
    }

    state.EnsuredSlot = slot;
    Item* const given = bot->EquipNewItem((static_cast<uint16>(INVENTORY_SLOT_BAG_0) << 8) | slot, itemId, true);
    if (!given)
    {
        CcRestoreTrinket(botAI);
        return false;
    }

    // The mark a save keeps (a crash, a save before the restore): CcCleanEnsuredTrinket finds it at the login. The
    // creator is the bot (no PvP trinket is crafted), the gift creator field holds the replaced item's guid (no gift
    // flag: nothing reads it as a gift)
    given->SetGuidValue(ITEM_FIELD_CREATOR, bot->GetGUID());
    given->SetGuidValue(ITEM_FIELD_GIFTCREATOR,
                        ObjectGuid::Create<HighGuid::Player>(old ? old->GetGUID().GetCounter() : 0));
    // The equip cooldown (30 s, Player::ApplyEquipCooldown; the arena entry's cooldown reset ran before it): the
    // trinket is ready at the fight start
    bot->RemoveSpellCooldown(SPELL_PVP_TRINKET, true);
    state.EnsuredTrinket = given->GetGUID();
    LOG_DEBUG("playerbots", "Avoid.EnsureTrinket: {} wears {} ({}) for the match in slot {}", bot->GetName(),
              given->GetTemplate()->Name1, itemId, slot);
    return true;
}

void CcRestoreTrinket(PlayerbotAI* botAI)
{
    CcBotState& state = botAI->GetCcState();
    state.TrinketWithheld = false;
    if (state.EnsuredTrinket.IsEmpty() && state.EnsuredReplaced.IsEmpty())
        return;

    // An item equipped by a dead player misses its stats (Player::EquipItem): a later call (the battleground leave
    // resurrects first; the logout's save keeps the mark for the login)
    Player* const bot = botAI->GetBot();
    if (!bot->IsAlive())
        return;

    CcPutBackTrinket(bot, bot->GetItemByGuid(state.EnsuredTrinket), bot->GetItemByGuid(state.EnsuredReplaced),
                     state.EnsuredSlot);
    state.EnsuredTrinket.Clear();
    state.EnsuredReplaced.Clear();
}

void CcCleanEnsuredTrinket(PlayerbotAI* botAI)
{
    // The trinket slots, the backpack and the bags (once per login, about a hundred slots)
    Player* const bot = botAI->GetBot();
    std::vector<Item*> marked;
    auto const look = [&](Item* item)
    {
        if (CcEnsuredMark(bot, item))
            marked.push_back(item);
    };
    look(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1));
    look(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET2));
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        look(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* const bag = bot->GetBagByPos(bagSlot);
        for (uint32 slot = 0; bag && slot < bag->GetBagSize(); ++slot)
            look(bag->GetItemByPos(static_cast<uint8>(slot)));
    }

    // Each goes through the restore; a dead bot keeps the first one in its state until it is alive
    CcBotState& state = botAI->GetCcState();
    for (Item* const given : marked)
    {
        uint32 const replacedCounter = given->GetGuidValue(ITEM_FIELD_GIFTCREATOR).GetCounter();
        Item* const replaced =
            replacedCounter ? bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(replacedCounter)) : nullptr;
        uint8 slot = given->IsEquipped() ? given->GetSlot() : static_cast<uint8>(EQUIPMENT_SLOT_END);
        for (uint8 const trinketSlot : {EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2})
        {
            if (slot == EQUIPMENT_SLOT_END && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, trinketSlot))
                slot = trinketSlot;
        }

        LOG_INFO("playerbots", "Avoid.EnsureTrinket: {} logs in with a match's trinket {} (slot {}), item {} back",
                 bot->GetName(), given->GetEntry(), slot, replaced ? replaced->GetEntry() : 0);
        state.EnsuredTrinket = given->GetGUID();
        state.EnsuredReplaced = replaced ? replaced->GetGUID() : ObjectGuid::Empty;
        state.EnsuredSlot = slot;
        CcRestoreTrinket(botAI);
        if (!state.EnsuredTrinket.IsEmpty())
            break;
    }
}

float TacticTrinketMultiplier::GetValue(Action* action)
{
    // Most actions end at the cached lookup
    if (action == nullptr)
        return 1.0f;

    CcBreaker const racial = RacialOf(action);
    if (racial == CcBreaker::None || !botAI->TacticEnabledHere(TacticId::Trinket))
        return 1.0f;

    return CcBreakAnswer(botAI) == racial ? 1.0f : 0.0f;
}

CcBreaker TacticTrinketMultiplier::RacialOf(Action* action)
{
    auto const it = racialActions.find(action);
    if (it != racialActions.end())
        return it->second;

    // getName() builds a string; resolved once per action and bot
    std::string const name = action->getName();
    CcBreaker racial = CcBreaker::None;
    for (CcBreaker const breaker : {CcBreaker::EveryMan, CcBreaker::Forsaken, CcBreaker::EscapeArtist})
    {
        if (name == CcBreakerAction(breaker))
            racial = breaker;
    }

    return racialActions.emplace(action, racial).first->second;
}

void PvpTrinketStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TacticTrinketMultiplier(botAI));
}

void TacticTrinketStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Player* const bot = botAI->GetBot();
    std::vector<NextAction> actions = {NextAction("pvp trinket", PvpTrinketRelevance)};
    for (CcBreaker const breaker : {CcBreaker::EveryMan, CcBreaker::Forsaken, CcBreaker::EscapeArtist})
    {
        uint32 const spellId = breaker == CcBreaker::EveryMan   ? SPELL_EVERY_MAN
                               : breaker == CcBreaker::Forsaken ? SPELL_FORSAKEN
                                                                : SPELL_ESCAPE_ARTIST;
        if (bot->HasSpell(spellId))
            actions.push_back(NextAction(CcBreakerAction(breaker), RacialBreakRelevance));
    }

    triggers.push_back(new TriggerNode("cc matters", actions));
}

bool CcMattersTrigger::IsActive() { return CcBreakAnswer(botAI) != CcBreaker::None; }

bool PvpTrinketAction::isUseful() { return CcBreakAnswer(botAI) == CcBreaker::Trinket; }

bool PvpTrinketAction::Execute(Event /*event*/)
{
    CcBotState& state = botAI->GetCcState();
    Item* const item = state.TrinketWithheld ? nullptr : CcWornPvpTrinket(bot);
    if (!item)
        return false;

    // As UseTrinketAction::UseTrinket: the client's item use, on the bot itself
    uint8 const castCount = 1;
    uint32 const glyphIndex = 0;
    uint8 const castFlags = 0;
    uint32 const targetFlag = TARGET_FLAG_NONE;
    WorldPacket packet(CMSG_USE_ITEM);
    packet << item->GetBagSlot() << item->GetSlot() << castCount << SPELL_PVP_TRINKET << item->GetGUID() << glyphIndex
           << castFlags;
    packet << targetFlag << bot->GetPackGUID();
    bot->GetSession()->HandleUseItemOpcode(packet);
    // A failed use keeps the cached answer (G5a review: no refresh every tick while it fails; it lives
    // CC_BREAK_REFRESH_MS)
    if (!bot->HasSpellCooldown(SPELL_PVP_TRINKET) && !bot->HasAura(SPELL_PVP_TRINKET))
        return false;

    botAI->NoteTacticUse(TacticId::Trinket);
    // cc.trinket (then CcNoteBreakCast) while the harness counts; the item use is no PlayerbotAI::CastSpell
    if (botAI->IsPvpTestCounting())
        botAI->CountPvpTestCast(SPELL_PVP_TRINKET, 0, false);
    else
        CcNoteBreakCast(botAI, SPELL_PVP_TRINKET);

    return true;
}

void TacticDispelStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("dispel cc on party", {NextAction("dispel cc on party", DispelCcRelevance)}));
}

bool DispelCcOnPartyTrigger::IsActive() { return CcDispelPick(botAI) != nullptr; }

Unit* DispelCcOnPartyAction::GetTarget() { return CcDispelPick(botAI); }

bool DispelCcOnPartyAction::isUseful() { return CcDispelPick(botAI) != nullptr; }

bool DispelCcOnPartyAction::Execute(Event /*event*/)
{
    CcBotState& state = botAI->GetCcState();
    Unit* const target = CcDispelPick(botAI);
    char const* const spell = state.DispelSpell;
    uint32 const auraSpell = state.DispelAuraSpell;
    if (!target || !spell)
        return false;

    // Counted and logged when the CC goes by the dispel (CcNoteDispelRemoval), not here: dispel magic removes a random
    // magic debuff, so the CC may stay. Filled before the cast: an instant dispel removes the aura inside CastSpell
    // (Spell::prepare casts at once, handle_immediate) (G5a review 2)
    state.PendingDispel.Target = target->GetGUID().GetRawValue();
    state.PendingDispel.AuraSpell = auraSpell;
    state.PendingDispel.CastMs = getMSTime();
    state.PendingDispel.Spell = spell;

    // A failed cast keeps the cached pick (G5a review: no refresh every tick while it fails, GCD or silence)
    if (!botAI->CastSpell(spell, target))
    {
        state.PendingDispel = CcPendingDispel();
        return false;
    }

    state.DispelUntilMs = 0;  // asked again after the cast
    botAI->NoteTacticUse(TacticId::Dispel);
    return true;
}

Unit* PartyMemberCcToDispelValue::Calculate() { return CcDispelPick(botAI); }

void AddCcTacticScripts() { new CcTacticUnitScript(); }
