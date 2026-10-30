/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CcTacticStrategy.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "ArenaCc.h"
#include "ArenaCcSpells.h"
#include "Battleground.h"
#include "CcState.h"
#include "FocusState.h"
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTestCounters.h"
#include "ScriptMgr.h"
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
// enabled here and DR awareness (below it the bot believes every CC lands in full: PickCc ignores the levels)
template <typename Fn>
void CcForEachObserver(Unit* unit, Player* caster, Fn const& fn)
{
    auto const visit = [&](Player* member)
    {
        if (!member || !member->IsInMap(unit))
            return;

        PlayerbotAI* const memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI || !memberAI->TacticEnabledHere(TacticId::Cc))
            return;

        CcBotState& state = memberAI->GetCcState();
        if (state.Params.DrAware)
            fn(member, state);
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

class CcTacticUnitScript : public UnitScript
{
public:
    CcTacticUnitScript() : UnitScript("CcTacticUnitScript", true, {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE}) {}

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
    }

    // Removed before its end (damage, dispel, trinket, death): the reset counts from now, or from the end of another
    // aura of the group still on the unit
    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (mode == AURA_REMOVE_BY_EXPIRE || !CcFeedOn())
            return;

        Aura const* const aura = aurApp->GetBase();
        Player* caster = nullptr;
        DiminishingGroup const group = CcObservedGroup(unit, aura, caster);
        if (group == DIMINISHING_NONE)
            return;

        uint32 otherLeftMs = 0;
        for (auto const& [spellId, application] : unit->GetAppliedAuras())
        {
            Aura const* const other = application->GetBase();
            if (other != aura && GetDiminishingReturnsGroupForSpell(other->GetSpellInfo(), false) == group)
                otherLeftMs =
                    std::max<uint32>(otherLeftMs, static_cast<uint32>(std::max<int32>(other->GetDuration(), 0)));
        }

        uint64 const enemy = unit->GetGUID().GetRawValue();
        uint32 const nowMs = getMSTime();
        CcForEachObserver(unit, caster, [&](Player* /*member*/, CcBotState& state)
                          { state.Memory.Ended(enemy, static_cast<uint8>(group), nowMs, otherLeftMs); });
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

    // No CC on a controlled target (G4c chains) or one immune to all (cyclone, banish, a bubble); a root on it is
    // seen in PickCc (no root on top, the loss-of-control rows still go)
    ready = CcComboRowsOn(bot, state, ready, comboMask, target);
    if (!ready || CcUnitControlled(target) || CcImmuneAll(target) || CcTeamBehind(bot, params))
    {
        state.ReadySinceMs = 0;
        return false;
    }

    // A patient bot holds a ready CC for the kill window: its own target low
    if (!state.ReadySinceMs)
        state.ReadySinceMs = nowMs;

    bool const killWindow =
        current && current != target && current->IsAlive() && current->GetHealthPct() <= CC_KILL_WINDOW_PCT;
    if (CcHolds(params, nowMs - state.ReadySinceMs, killWindow) || CcHealerNeeded(bot))
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
    if (params.DrAware)
        input.Levels = state.Memory.Levels(target->GetGUID().GetRawValue(), nowMs);

    int8 const row = PickCc(input, state.Spells, params);
    if (row == CC_NO_ROW || !bot->IsWithinLOSInMap(target))
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
