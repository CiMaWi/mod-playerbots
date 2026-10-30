/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FocusStrategy.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "ArenaFocus.h"
#include "Battleground.h"
#include "FocusState.h"
#include "Group.h"
#include "NaturalReaction.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "SkillStrategy.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Timer.h"

namespace
{
// As DpsAssistStrategy's "dps assist" (50); the plan just above it, so the marks are written before the follow
constexpr float FocusMarkRelevance = ACTION_HIGH + 30.0f;
constexpr float ArenaPlanRelevance = ACTION_HIGH + 31.0f;

constexpr uint32 PenanceFirstRank = 47540;  // a channel whose heals are triggered spells

// Defensive auras a player sees on an enemy's buff bar (the immunities are ImmuneAll / ImmunePhys instead)
constexpr std::array<uint32, 16> DefensiveAuras = {
    33206,  // Pain Suppression
    47788,  // Guardian Spirit
    22812,  // Barkskin
    61336,  // Survival Instincts
    871,    // Shield Wall
    12975,  // Last Stand
    498,    // Divine Protection
    6940,   // Hand of Sacrifice
    5277,   // Evasion
    26669,  // Evasion
    31224,  // Cloak of Shadows
    48792,  // Icebound Fortitude
    48707,  // Anti-Magic Shell
    47585,  // Dispersion
    19263,  // Deterrence
    30823,  // Shamanistic Rage
};

uint8 FocusHealthPct(Unit const* unit)
{
    if (!unit->IsAlive())
        return 0;

    // Rounded up: a live unit is never at 0 (HoldDecision and Decide read 0 as dead)
    return static_cast<uint8>(std::clamp(std::ceil(unit->GetHealthPct()), 1.0f, 100.0f));
}

bool FocusImmuneAll(Unit const* unit)
{
    return unit->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) && unit->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC);
}

bool FocusDefensiveUp(Unit const* unit)
{
    return std::any_of(DefensiveAuras.begin(), DefensiveAuras.end(),
                       [unit](uint32 spellId) { return unit->HasAura(spellId); });
}

bool FocusHealSpell(SpellInfo const* info)
{
    if (info->HasEffect(SPELL_EFFECT_HEAL) || info->HasAura(SPELL_AURA_PERIODIC_HEAL))
        return true;

    SpellInfo const* const first = info->GetFirstRankSpell();
    return first && first->Id == PenanceFirstRank;
}

// A heal on the enemy's cast bar (what a player sees)
bool FocusCastingHeal(Unit const* unit)
{
    for (CurrentSpellTypes const type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
    {
        Spell const* const spell = unit->GetCurrentSpell(type);
        if (spell && spell->GetSpellInfo() && FocusHealSpell(spell->GetSpellInfo()))
            return true;
    }

    return false;
}

bool FocusHealerClass(uint8 playerClass)
{
    return playerClass == CLASS_PRIEST || playerClass == CLASS_PALADIN || playerClass == CLASS_DRUID ||
           playerClass == CLASS_SHAMAN;
}

// A class with a crowd-control option the bots use today (polymorph, cyclone/hibernate/roots, freezing trap,
// fear/banish, blind, psychic scream/shackle, repentance/hammer); G4a widens the test
bool FocusCanControl(uint8 playerClass)
{
    switch (playerClass)
    {
        case CLASS_MAGE:
        case CLASS_DRUID:
        case CLASS_HUNTER:
        case CLASS_WARLOCK:
        case CLASS_ROGUE:
        case CLASS_PRIEST:
        case CLASS_PALADIN:
            return true;
        default:
            return false;
    }
}

bool FocusPhysical(Player* player) { return PlayerbotAI::IsMelee(player) || player->getClass() == CLASS_HUNTER; }

// Immune to what this bot does: to everything, to physical damage for a physical bot, or forced by the GM command
bool FocusTargetImmune(Unit const* target, Player* bot, FocusBotState const& state, uint32 nowMs)
{
    return FocusForcedImmune(state, target->GetGUID(), nowMs) || FocusImmuneAll(target) ||
           (target->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) && FocusPhysical(bot));
}

bool FocusArenaInProgress(Battleground const* bg)
{
    return bg && bg->isArena() && bg->GetStatus() == STATUS_IN_PROGRESS;
}

int8 FocusIndexOf(FocusMatchState const& match, ObjectGuid guid)
{
    if (guid.IsEmpty())
        return FOCUS_NONE;

    for (uint8 index = 0; index < match.EnemyCount; ++index)
    {
        if (match.Enemies[index].Guid == guid)
            return static_cast<int8>(index);
    }

    return FOCUS_NONE;
}

// A changed icon this bot did not write: in a group with a real player it counts as his (the group does not say who
// set an icon; another bot's strong-disagreement mark is counted the same way)
void ObserveMarks(FocusMatchState& match, ObjectGuid skull, ObjectGuid moon, bool realPlayer, uint32 nowMs)
{
    if (skull == match.SeenSkull && moon == match.SeenMoon)
        return;

    bool const own = skull == match.WrittenSkull && moon == match.WrittenMoon;
    match.SeenSkull = skull;
    match.SeenMoon = moon;
    if (skull.IsEmpty() && moon.IsEmpty())
        return;

    match.Marked = true;
    match.LastMarkMs = nowMs;
    if (!own && realPlayer)
    {
        match.HumanMarked = true;
        match.LastHumanMarkMs = nowMs;
        ++match.HumanMarksSeen;
    }
}

// Group::SetTargetIcon only on a change (it broadcasts, and it clears the same target from the other icons)
void WriteIcon(Group* group, uint8 icon, ObjectGuid who, ObjectGuid target)
{
    if (group->GetTargetIcon(icon) != target)
        group->SetTargetIcon(icon, who, target);
}

void StartMatch(FocusMatchState& match, uint32 matchId, uint32 nowMs)
{
    match = FocusMatchState();
    match.MatchId = matchId;
    match.StartMs = nowMs;
    match.NextPlanMs = nowMs;
    match.NextSampleMs = nowMs;
}

uint32 PlanIntervalMs(FocusBotState const& state)
{
    return std::max(state.Params.Team.PlanIntervalMs, FOCUS_MIN_PLAN_INTERVAL_MS);
}
}  // namespace

float TacticHoldMultiplier::GetValue(Action* action)
{
    // Cheap exits first: most actions end at the cached lookup; never block engaging (no live target)
    if (action == nullptr || !IsSwitchAction(action) || botAI->GetTacticContext() == TacticContext::Pve)
        return 1.0f;

    Unit* const current = AI_VALUE(Unit*, "current target");
    if (!current || !current->IsAlive() || !current->IsInWorld())
        return 1.0f;

    Unit* const candidate = action->GetTarget();
    if (!candidate || candidate == current || !botAI->TacticActive(TacticId::Hold))
        return 1.0f;

    Player* const bot = botAI->GetBot();
    FocusBotState& state = botAI->GetFocusState();
    FocusHoldState& hold = state.Hold;
    uint32 const nowMs = getMSTime();
    ObjectGuid const currentGuid = current->GetGUID();
    ObjectGuid const candidateGuid = candidate->GetGUID();

    // The target as taken: HeldSinceMs from the allowed switch that led to it, else from now
    if (hold.HeldGuid != currentGuid)
    {
        bool const switched = hold.PendingGuid == currentGuid;
        hold.HeldSinceMs = switched ? hold.PendingAtMs : nowMs;
        if (switched && (hold.PendingVerdict == static_cast<uint8>(HoldReason::Free) ||
                         hold.PendingVerdict == static_cast<uint8>(HoldReason::Execute)))
        {
            ++hold.Switches;
            botAI->CountTactic(TacticCounter::HoldSwitch);
        }

        hold.HeldGuid = currentGuid;
        hold.PendingGuid = ObjectGuid::Empty;
    }

    // Sight of the target, checked at most every FOCUS_SIGHT_CHECK_MS
    if (hold.SightGuid != currentGuid)
    {
        hold.SightGuid = currentGuid;
        hold.LastSeenMs = nowMs;
        hold.SightCheckMs = nowMs;
    }

    if (!FocusBefore(nowMs, hold.SightCheckMs))
    {
        hold.SightCheckMs = nowMs + FOCUS_SIGHT_CHECK_MS;
        if (bot->IsWithinLOSInMap(current))
            hold.LastSeenMs = nowMs;
    }

    // The team plan: the skull always passes; a bot on the skull leaves it only for a hard reason
    Group* const group = bot->GetGroup();
    ObjectGuid const skull = group ? group->GetTargetIcon(FOCUS_ICON_SKULL) : ObjectGuid::Empty;
    if (!skull.IsEmpty() && candidateGuid == skull)
        return Answer(candidateGuid, FOCUS_VERDICT_MARK, true, nowMs);

    HoldParams const& params = state.Params.Hold;
    HoldInput input;
    input.HasTarget = true;
    input.TargetAlive = true;
    input.TargetInRange = bot->IsWithinDistInMap(current, sPlayerbotAIConfig.SpellDistance);
    input.TargetImmune = FocusTargetImmune(current, bot, state, nowMs);
    input.TargetLosLostMs = nowMs - hold.LastSeenMs;
    input.TargetHpPct = FocusHealthPct(current);
    input.HeldSinceMs = hold.HeldSinceMs;
    input.CandidateHpPct = FocusHealthPct(candidate);
    // The aura lookups only where the execute rule reads them
    input.CandidateDefensiveUp = input.CandidateHpPct <= params.ExecPct && FocusDefensiveUp(candidate);
    // Health only (G1a review NOTE 6): skill 1's tunnel vision rests on a health advantage never reaching its
    // hysteresis of 100; no healer or trinket terms here
    input.Advantage = static_cast<int32>(input.TargetHpPct) - static_cast<int32>(input.CandidateHpPct);
    input.SwitchCostMs = EaseCostMs(botAI->GetReactionTypes(), ReactionEase::NewTarget);
    HoldResult const result = HoldDecision(input, nowMs, params, bot->GetGUID().GetCounter());

    if (result.Allow && result.Reason == HoldReason::Free && currentGuid == skull)
        return Answer(candidateGuid, FOCUS_VERDICT_MARK_HELD, false, nowMs);

    if (!result.Allow && skull.IsEmpty() && group && group->GetMembersCount() <= FOCUS_MAX_TEAM)
    {
        // Without marks: the partner's visible target, late and by chance (FollowPartnerDecision)
        bool partnerOn = false;
        for (GroupReference* ref = group->GetFirstMember(); ref && !partnerOn; ref = ref->next())
        {
            Player* const member = ref->GetSource();
            partnerOn = member && member != bot && member->IsAlive() && member->GetTarget() == candidateGuid;
        }

        if (partnerOn)
        {
            if (hold.FollowGuid != candidateGuid)
            {
                hold.FollowGuid = candidateGuid;
                hold.FollowSinceMs = nowMs;
            }

            if (FollowPartnerDecision(hold.FollowSinceMs, nowMs, params, bot->GetGUID().GetCounter()))
                return Answer(candidateGuid, FOCUS_VERDICT_FOLLOW, true, nowMs);
        }
    }

    return Answer(candidateGuid, static_cast<uint8>(result.Reason), result.Allow, nowMs);
}

float TacticHoldMultiplier::Answer(ObjectGuid candidate, uint8 verdict, bool allow, uint32 nowMs)
{
    FocusHoldState& hold = botAI->GetFocusState().Hold;
    if (candidate != hold.LastCandidate || verdict != hold.LastVerdict)
    {
        hold.LastCandidate = candidate;
        hold.LastVerdict = verdict;
        if (verdict < FOCUS_HOLD_VERDICTS)
            ++hold.Verdicts[verdict];

        botAI->NoteTacticUse(TacticId::Hold);
    }

    if (!allow)
        return 0.0f;

    hold.PendingGuid = candidate;
    hold.PendingAtMs = nowMs;
    hold.PendingVerdict = verdict;
    return 1.0f;
}

bool TacticHoldMultiplier::IsSwitchAction(Action* action)
{
    auto const it = switchActions.find(action);
    if (it != switchActions.end())
        return it->second;

    // getName() builds a string; resolved once per action and bot, from the skill strategy's table
    bool const isSwitch = SkillNoticeCategoryOf(action->getName()) == SkillNoticeCategory::TargetSwitch;
    return switchActions.emplace(action, isSwitch).first->second;
}

void PvpHoldStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new TacticHoldMultiplier(botAI));
}

void ArenaFocusStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("plan due", {NextAction("arena plan", ArenaPlanRelevance)}));
    triggers.push_back(new TriggerNode("focus mark", {NextAction("dps assist", FocusMarkRelevance)}));
}

bool PlanDueTrigger::IsActive()
{
    // DT.9: the team plan is arena only (the Plan tactic's default scope also names Battleground)
    if (botAI->GetTacticContext() != TacticContext::Arena)
        return false;

    Player* const bot = botAI->GetBot();
    Battleground* const bg = bot->GetBattleground();
    Group* const group = bot->GetGroup();
    if (!FocusArenaInProgress(bg) || !group)
        return false;

    FocusBotState& state = botAI->GetFocusState();
    FocusMatchState& match = state.Match;
    uint32 const nowMs = getMSTime();
    if (match.MatchId != bg->GetInstanceID())
        StartMatch(match, bg->GetInstanceID(), nowMs);

    ObjectGuid const skull = group->GetTargetIcon(FOCUS_ICON_SKULL);
    Unit* const skullUnit = skull.IsEmpty() ? nullptr : botAI->GetUnit(skull);
    if (!FocusBefore(nowMs, match.NextSampleMs))
    {
        match.NextSampleMs = nowMs + PlanIntervalMs(state);
        SampleFocus(skullUnit, !skull.IsEmpty());
    }

    if (!botAI->TacticActive(TacticId::Plan))
        return false;

    // At once (once per event) when the marked kill target dies, leaves or turns immune
    bool const skullAlive = skullUnit && skullUnit->IsAlive() && skullUnit->IsInWorld();
    bool const immune = skullAlive && (FocusForcedImmune(state, skull, nowMs) || FocusImmuneAll(skullUnit));
    if (immune && match.ImmuneGuid != skull)
    {
        match.ImmuneGuid = skull;
        match.ImmuneSinceMs = FocusForcedImmune(state, skull, nowMs) ? state.ForcedImmuneAtMs : nowMs;
    }

    bool const urgent = !skull.IsEmpty() && (!skullAlive || immune);
    if (urgent != match.UrgentSeen)
    {
        match.UrgentSeen = urgent;
        if (urgent)
            return true;
    }

    return !FocusBefore(nowMs, match.NextPlanMs);
}

// tactics.focusSame / focusDiff: the bot's live target against the skull, or without marks against a partner's
// visible target (a team without a plan); no sample while the skull is set but dead
void PlanDueTrigger::SampleFocus(Unit* skullUnit, bool skullSet)
{
    if (!botAI->IsPvpTestCounting())
        return;

    Unit* const current = AI_VALUE(Unit*, "current target");
    if (!current || !current->IsAlive())
        return;

    Unit* reference = nullptr;
    if (skullSet)
        reference = skullUnit && skullUnit->IsAlive() ? skullUnit : nullptr;
    else if (Group* const group = botAI->GetBot()->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref && !reference; ref = ref->next())
        {
            Player* const member = ref->GetSource();
            if (!member || member == botAI->GetBot() || !member->IsAlive() || member->GetTarget().IsEmpty())
                continue;

            Unit* const target = botAI->GetUnit(member->GetTarget());
            reference = target && target->IsAlive() ? target : nullptr;
        }
    }

    if (!reference)
        return;

    botAI->CountTactic(reference == current ? TacticCounter::FocusSame : TacticCounter::FocusDiff);
}

bool FocusMarkTrigger::IsActive()
{
    if (botAI->GetTacticContext() != TacticContext::Arena || !botAI->TacticEnabledHere(TacticId::Plan))
        return false;

    Group* const group = botAI->GetBot()->GetGroup();
    ObjectGuid const skull = group ? group->GetTargetIcon(FOCUS_ICON_SKULL) : ObjectGuid::Empty;
    if (skull.IsEmpty())
        return false;

    Unit* const current = AI_VALUE(Unit*, "current target");
    if (!current || !current->IsAlive() || current->GetGUID() == skull)
        return false;

    // "dps target" takes the rti target (skull) first when the bot can attack it
    Unit* const dps = AI_VALUE(Unit*, "dps target");
    return dps && dps->GetGUID() == skull;
}

bool ArenaPlanAction::isUseful()
{
    return botAI->GetTacticContext() == TacticContext::Arena && botAI->GetBot()->GetGroup() &&
           FocusArenaInProgress(botAI->GetBot()->GetBattleground());
}

bool ArenaPlanAction::Execute(Event /*event*/)
{
    Player* const bot = botAI->GetBot();
    Battleground* const bg = bot->GetBattleground();
    Group* const group = bot->GetGroup();
    if (!bg || !group)
        return false;

    FocusBotState& state = botAI->GetFocusState();
    FocusMatchState& match = state.Match;
    uint32 const nowMs = getMSTime();
    match.NextPlanMs = nowMs + PlanIntervalMs(state);

    // The team on this map (at most FOCUS_MAX_TEAM): who plans; a real player marks himself (ElectPlanner)
    std::array<PlannerCandidate, FOCUS_MAX_TEAM> candidates{};
    std::array<Player*, FOCUS_MAX_TEAM> members{};
    uint8 memberCount = 0;
    bool realPlayer = false;
    for (GroupReference* ref = group->GetFirstMember(); ref && memberCount < FOCUS_MAX_TEAM; ref = ref->next())
    {
        Player* const member = ref->GetSource();
        if (!member || !member->IsInMap(bot))
            continue;

        PlayerbotAI* const memberAI = GET_PLAYERBOT_AI(member);
        PlannerCandidate& candidate = candidates[memberCount];
        candidate.Guid = member->GetGUID().GetCounter();
        candidate.Skill = memberAI ? memberAI->GetPvpSkill() : 0;
        candidate.Alive = member->IsAlive();
        candidate.RealPlayer = memberAI == nullptr;
        realPlayer = realPlayer || candidate.RealPlayer;
        members[memberCount++] = member;
    }

    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    int8 const elected = ElectPlanner(candidates.data(), memberCount,
                                      config.TacticConfig[static_cast<uint32>(TacticId::Plan)].KnownFromSkill);
    match.Planner = elected != FOCUS_NONE && members[elected] == bot;

    ObjectGuid const skull = group->GetTargetIcon(FOCUS_ICON_SKULL);
    ObjectGuid const moon = group->GetTargetIcon(FOCUS_ICON_MOON);
    ObserveMarks(match, skull, moon, realPlayer, nowMs);
    // A bot other than the planner only re-marks a strong disagreement: without marks there is none
    if (!match.Planner && skull.IsEmpty())
        return false;

    if (!match.TeamReady)
    {
        // The planner's own numbers with what its team can do, once per match (the gates are open: all are in)
        match.Team = state.Params.Team;
        uint8 physical = 0;
        for (uint8 index = 0; index < memberCount; ++index)
        {
            match.Team.CcCapable = match.Team.CcCapable || FocusCanControl(members[index]->getClass());
            physical += FocusPhysical(members[index]) ? 1 : 0;
        }

        match.Team.PhysicalTeam = physical * 2 > memberCount;
        match.TeamReady = true;
    }

    // The enemies as the arena frames list them: the other team's players, by guid (stable in the match), the first
    // FOCUS_MAX_ENEMIES (G1a review NOTE 4: a 5v5's last two are left out; the tests are 2v2)
    std::array<Player*, FOCUS_MAX_ENEMIES> foes{};
    uint8 count = 0;
    for (auto const& [guid, player] : bg->GetPlayers())
    {
        if (count >= FOCUS_MAX_ENEMIES)
            break;

        if (player && player->GetBgTeamId() != bot->GetBgTeamId() && player->IsInMap(bot))
            foes[count++] = player;
    }

    if (count == 0)
        return false;

    // The snapshot: what the team sees now, plus what it has seen in this match (DT.12)
    std::array<EnemyInfo, FOCUS_MAX_ENEMIES> enemies{};
    match.EnemyCount = count;
    for (uint8 index = 0; index < count; ++index)
    {
        Player* const foe = foes[index];
        FocusEnemyMemory& memory = match.Enemies[index];
        if (memory.Guid != foe->GetGUID())
        {
            memory = FocusEnemyMemory();
            memory.Guid = foe->GetGUID();
            memory.LastSeenMs = match.StartMs;
        }

        EnemyInfo& info = enemies[index];
        info.Alive = foe->IsAlive();
        info.HpPct = FocusHealthPct(foe);
        if (!info.Alive)
            continue;

        memory.HealSeen = memory.HealSeen || FocusCastingHeal(foe);
        // The healer test (a player cannot see talents): a healer class seen casting a heal in this match
        info.Healer = FocusHealerClass(foe->getClass()) && memory.HealSeen;
        info.ImmuneAll = FocusForcedImmune(state, memory.Guid, nowMs) || FocusImmuneAll(foe);
        info.ImmunePhys = foe->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL);
        info.CcBreakable = foe->HasBreakableByDamageCrowdControlAura();
        info.StunnedOrFeared = foe->HasAuraType(SPELL_AURA_MOD_STUN) || foe->HasAuraType(SPELL_AURA_MOD_FEAR);
        info.DefensiveUp = FocusDefensiveUp(foe);
        memory.DefensiveSeen = memory.DefensiveSeen || info.DefensiveUp;
        info.DefensiveUsed = state.AddonKnowledge && memory.DefensiveSeen && !info.DefensiveUp;
        // TrinketUsed and DrLevel stay unknown until the CC step observes breaks and its own CC
        for (uint8 member = 0; member < memberCount; ++member)
        {
            Player* const own = members[member];
            if (!own->IsAlive())
                continue;

            if (own->IsWithinDistInMap(foe, config.SpellDistance))
                ++info.InRangeCount;

            if (own->IsWithinLOSInMap(foe) && own->CanSeeOrDetect(foe))
                ++info.LosCount;
        }

        if (info.LosCount)
            memory.LastSeenMs = nowMs;

        info.LosLostMs = info.LosCount ? 0 : nowMs - memory.LastSeenMs;
    }

    // The plan as marked: the icons are the truth (another planner, a real player, a planner that died)
    PlanState prev = match.Plan;
    if (skull != match.PlanKillGuid)
    {
        prev.HeldUntilMs = match.LastMarkMs + match.Team.HoldMs;
        prev.Reason = PlanReason::None;
    }

    prev.Kill = FocusIndexOf(match, skull);
    prev.Cc = FocusIndexOf(match, moon);
    if (prev.Kill != FOCUS_NONE && enemies[prev.Kill].ImmuneAll && match.ImmuneGuid != skull)
    {
        match.ImmuneGuid = skull;
        match.ImmuneSinceMs = FocusForcedImmune(state, skull, nowMs) ? state.ForcedImmuneAtMs : nowMs;
    }

    uint32 const switchCostMs = EaseCostMs(botAI->GetReactionTypes(), ReactionEase::NewTarget);
    PlanState const next =
        Decide(enemies.data(), count, prev, nowMs, match.Team, bot->GetGUID().GetCounter(), switchCostMs);
    bool const killChange = next.Kill != prev.Kill;
    bool const ccChange = match.Team.CcCapable && next.Cc != prev.Cc;
    RemarkEvent event = RemarkEvent::None;
    if (killChange && next.Kill != FOCUS_NONE)
    {
        event = EventOfPlanReason(next.Reason);
        // G1a review NOTE 5: a Start after the match's first marks is a re-pick (the kill target was dead or gone)
        if (next.Reason == PlanReason::Start && match.FirstPlanDone)
            event = RemarkEvent::KillDead;
    }
    else if (!killChange && ccChange)
    {
        bool const moonGone = prev.Cc == FOCUS_NONE || !FocusAlive(enemies[prev.Cc]) || enemies[prev.Cc].ImmuneAll;
        event = moonGone ? RemarkEvent::Opening : RemarkEvent::None;
    }

    if (!killChange)
    {
        match.Plan = next;
        match.PlanKillGuid = skull;
    }

    if (event == RemarkEvent::None)
    {
        match.PendingEvent = RemarkEvent::None;
        match.PendingKill = FOCUS_NONE;
        return false;
    }

    if (event != match.PendingEvent || next.Kill != match.PendingKill)
    {
        match.PendingEvent = event;
        match.PendingKill = next.Kill;
        match.EventAtMs = event == RemarkEvent::KillImmune && match.ImmuneGuid == skull ? match.ImmuneSinceMs : nowMs;
        match.LastRemark = FOCUS_REMARK_REASONS;
    }

    // The bot's own best against the marked target (a dead, immune or controlled one counts nothing)
    bool const physicalTeam = match.Team.PhysicalTeam;
    int32 margin = 0;
    if (prev.Kill != FOCUS_NONE && next.Kill != FOCUS_NONE)
        margin = FocusKillScore(enemies[next.Kill], physicalTeam) -
                 (FocusKillCandidate(enemies[prev.Kill]) ? FocusKillScore(enemies[prev.Kill], physicalTeam) : 0);

    RemarkInput input;
    input.Event = event;
    input.EventAtMs = match.EventAtMs;
    input.Planner = match.Planner;
    input.Margin = margin;
    input.Marked = match.Marked;
    input.LastMarkMs = match.LastMarkMs;
    input.HumanMarked = match.HumanMarked;
    input.LastHumanMarkMs = match.LastHumanMarkMs;
    RemarkResult const remark = RemarkDecision(input, nowMs, match.Team);
    uint8 const remarkIndex = static_cast<uint8>(remark.Reason);
    if (remarkIndex != match.LastRemark && remarkIndex < FOCUS_REMARK_REASONS)
    {
        match.LastRemark = remarkIndex;
        ++match.Remarks[remarkIndex];
    }

    if (!remark.Allow)
    {
        // Back exactly when the human delay has passed
        uint32 const dueMs = match.EventAtMs + match.Team.RemarkDelayMs;
        if (remark.Reason == RemarkReason::Delay && FocusBefore(dueMs, match.NextPlanMs))
            match.NextPlanMs = dueMs;

        return false;
    }

    ObjectGuid const killGuid = next.Kill != FOCUS_NONE ? match.Enemies[next.Kill].Guid : ObjectGuid::Empty;
    ObjectGuid const ccGuid =
        match.Team.CcCapable && next.Cc != FOCUS_NONE ? match.Enemies[next.Cc].Guid : ObjectGuid::Empty;
    WriteIcon(group, FOCUS_ICON_SKULL, bot->GetGUID(), killGuid);
    // Moon only when the team can control; a moon this bot wrote goes when there is none
    if (!ccGuid.IsEmpty() || group->GetTargetIcon(FOCUS_ICON_MOON) == match.WrittenMoon)
        WriteIcon(group, FOCUS_ICON_MOON, bot->GetGUID(), ccGuid);

    match.WrittenSkull = group->GetTargetIcon(FOCUS_ICON_SKULL);
    match.WrittenMoon = group->GetTargetIcon(FOCUS_ICON_MOON);
    match.SeenSkull = match.WrittenSkull;
    match.SeenMoon = match.WrittenMoon;
    match.Marked = true;
    match.LastMarkMs = nowMs;
    ++match.MarkWrites;
    if (killChange)
    {
        // The match's first marks are no switch (NOTE 5)
        if (match.FirstPlanDone)
        {
            ++match.PlanSwitches;
            botAI->CountTactic(TacticCounter::PlanSwitch);
        }

        if (next.Reason == PlanReason::Immune && match.ImmuneGuid == skull)
            botAI->CountTactic(TacticCounter::ImmuneSwapMs, nowMs - match.ImmuneSinceMs);

        match.FirstPlanDone = true;
    }

    if (!match.Planner)
        ++match.MarginRemarks;

    match.Plan = next;
    match.PlanKillGuid = killGuid;
    match.PendingEvent = RemarkEvent::None;
    match.PendingKill = FOCUS_NONE;
    botAI->NoteTacticUse(TacticId::Plan);
    return false;
}

void FocusClearArenaMarks(Battleground* bg)
{
    if (!sPlayerbotAIConfig.TacticsEnable || !bg || !bg->isArena())
        return;

    for (TeamId const team : {TEAM_ALLIANCE, TEAM_HORDE})
    {
        Group* const group = bg->GetBgRaid(team);
        if (!group)
            continue;

        for (uint8 const icon : {FOCUS_ICON_SKULL, FOCUS_ICON_MOON})
            WriteIcon(group, icon, ObjectGuid::Empty, ObjectGuid::Empty);
    }
}
