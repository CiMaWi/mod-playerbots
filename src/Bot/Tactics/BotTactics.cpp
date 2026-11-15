/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotTactics.h"

#include <algorithm>

#include "ArenaCover.h"
#include "ArenaDefensive.h"
#include "ArenaDrink.h"
#include "ArenaInterrupt.h"
#include "ArenaJump.h"
#include "ArenaKite.h"
#include "Battleground.h"
#include "BotTacticsInternal.h"
#include "CcState.h"
#include "DefensiveTacticStrategy.h"
#include "FocusState.h"
#include "MapMgr.h"
#include "NaturalCast.h"
#include "NaturalMove.h"
#include "NaturalPath.h"
#include "NaturalReaction.h"
#include "NaturalTurn.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "ReactionEngine.h"

BotTactics::BotTactics(PlayerbotAI* botAI, Player* bot) : botAI(botAI), bot(bot) {}

BotTactics::~BotTactics() = default;

void BotTactics::NoteTacticUse(TacticId id) const { NoteTacticActivation(id, botAI->GetTacticContext()); }

void PlayerbotAI::SetPvpSkill(uint8 skill, uint8 firstRoll)
{
    pvpSkill = skill;
    pvpSkillFirstRoll = firstRoll ? firstRoll : skill;
    skillNoticeCounters = SkillNoticeCounters();
    tactics->SetSkill(skill, pvpSkillFirstRoll);
}

void BotTactics::SetSkill(uint8 skill, uint8 firstRoll)
{
    skillReactFactor = 1.0f;
    skillNoticeMissPercent = 0;
    skillNoticeDelayMs = 0;
    reactionTypes = ReactionTypeState();
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    // Called before the bot's first update (login) or on the world thread (GM commands, the harness, lab pins and
    // reapply): MapMgr::Update waits for the map threads before the commands and world scripts run, so no map thread
    // reads these values while they change one after another.
    // The hashes' guid: the bot's own, or a lab copy's template (G0j-b); they never see the percentile
    uint64 const guid = attributeTemplate ? attributeTemplate : (bot ? bot->GetGUID().GetCounter() : 0);
    // The attributes from the skill and the guid (BotAttributes.h; with the defaults every ability is the skill).
    // DT.14: the learnable ones read the (grown) skill, the fixed ones the first roll (G6b; the skill without growth)
    SkillDistributionParams const distribution{config.SkillDistributionUniform, config.SkillMean, config.SkillSd,
                                               config.SkillMin, config.SkillMax};
    attributes = ComputeBotAttributes(config.SkillAttributes, distribution, skill, firstRoll, guid);
    // The tactic gate from the knowledge, discipline, grit and style attributes (skill 0: full ability)
    tacticGate = ComputeTacticGate(config.SkillGate, config.TacticConfig,
                                   AttributeGateInputs(config.SkillAttributes, attributes, skill), guid);
    // Speed carries the reaction draw, D3's stretch and D4's notice delay; Focus D4's miss share. Cached whatever
    // Playerbots.Natural.Reaction.Enable says, so a .reload config can turn it on for logged-in bots.
    uint8 const speed = attributes.Get(BotAttribute::Speed);
    reactionState = ComputeReactionState(config.NaturalReaction, speed, config.SkillGate.CurveExponent);
    // The hold's and the team plan's numbers (G1b), cached whatever Playerbots.Tactics.Hold/Plan.Enable say; skill 0
    // gives the skill-99 values
    FocusBotState& focus = GetFocusState();
    focus.Params = ComputeFocusParams(skill, attributes, config.SkillGate.CurveExponent, config.TacticFocus);
    focus.AddonKnowledge = FocusAddonKnowledge(skill, attributes.Get(BotAttribute::Knowledge),
                                               config.TacticFocus.FocusAddonKnowledgeFromSkill);
    // Crowd control's numbers (G4b, G4a NOTE 8), cached whatever Playerbots.Tactics.Cc/OwnCc.Enable say
    GetCcState().Params =
        ComputeCcParams(skill, attributes, tacticGate.Effort, config.SkillGate.CurveExponent, config.TacticCc);
    // The Defensive and Protect tactics' numbers (G5d), cached whatever their Enable says; skill 0 gives skill 99's
    GetDefensiveState().Params = ComputeDefensiveParams(skill, attributes, tacticGate.Effort,
                                                        config.SkillGate.CurveExponent, config.TacticDefensive);
    // The Fake tactic's numbers (G5e), cached whatever its Enable says; skill 0 gives skill 99's
    GetFakeState().Params = ComputeFakeParams(skill, attributes, config.TacticFake);
    // The natural facing's numbers (G2a), cached whatever Natural.Facing.Enable says; skill 0 gives skill 99's
    GetTurnState().Params = ComputeTurnParams(skill, attributes, config.SkillGate.CurveExponent, config.NaturalFacing);
    // The natural tracking's numbers (G2b), cached whatever Natural.Track.Enable says; skill 0 gives skill 99's
    GetTrackState().Params = ComputeTrackParams(skill, attributes, config.SkillGate.CurveExponent, config.NaturalTrack);
    // The natural paths' numbers (G2c), cached whatever Natural.Path.Enable says; skill 0 gives skill 99's
    GetPathState().Params = ComputePathParams(skill, attributes, config.SkillGate.CurveExponent, config.NaturalPath);
    if (skill == 0)
        return;

    // 0 at 1, 1 at 99, bent by Playerbots.Skill.CurveExponent (1.0: the straight line, as before)
    float const speedPosition = SkillPosition(speed);
    float const focusPosition = SkillPosition(attributes.Get(BotAttribute::Focus));
    float const exponent = config.SkillGate.CurveExponent;
    // G0h3's prices from Speed, Wisdom, Focus, Dexterity, Foresight and Patience, also with the parts off
    ReactionTypeInputs typeInputs;
    typeInputs.Speed = speed;
    typeInputs.Wisdom = attributes.Get(BotAttribute::Wisdom);
    typeInputs.Focus = attributes.Get(BotAttribute::Focus);
    typeInputs.Dexterity = attributes.Get(BotAttribute::Dexterity);
    typeInputs.Foresight = attributes.Get(BotAttribute::Foresight);
    typeInputs.Patience = attributes.Get(BotAttribute::Patience);
    reactionTypes = ComputeReactionTypeState(config.NaturalReaction, typeInputs, exponent);
    if (config.SkillReactActive)
        skillReactFactor = SkillLerp(config.SkillReactFactorLow, config.SkillReactFactorHigh, speedPosition, exponent);

    if (config.SkillNoticeActive)
    {
        skillNoticeMissPercent = SkillLerpRounded(config.SkillNoticeMissPercentLow, config.SkillNoticeMissPercentHigh,
                                                  focusPosition, exponent);
        skillNoticeDelayMs =
            SkillLerpRounded(config.SkillNoticeDelayMsLow, config.SkillNoticeDelayMsHigh, speedPosition, exponent);
    }
}

FocusBotState& BotTactics::GetFocusState()
{
    if (!focusState)
    {
        // SetPvpSkill fills the numbers; until it runs (Skill off) they are the skill-0 ones, the skill-99 values
        PlayerbotAIConfig const& config = sPlayerbotAIConfig;
        focusState = std::make_unique<FocusBotState>();
        focusState->Params = ComputeFocusParams(0, attributes, config.SkillGate.CurveExponent, config.TacticFocus);
        focusState->AddonKnowledge = FocusAddonKnowledge(0, attributes.Get(BotAttribute::Knowledge),
                                                         config.TacticFocus.FocusAddonKnowledgeFromSkill);
    }

    return *focusState;
}

CcBotState& BotTactics::GetCcState()
{
    if (!ccState)
    {
        // SetPvpSkill fills the numbers; until it runs (Skill off) they are the skill-0 ones (full ability)
        PlayerbotAIConfig const& config = sPlayerbotAIConfig;
        ccState = std::make_unique<CcBotState>();
        ccState->Params =
            ComputeCcParams(0, attributes, SKILL_GATE_FULL, config.SkillGate.CurveExponent, config.TacticCc);
    }

    return *ccState;
}

InterruptBotState& BotTactics::GetInterruptState()
{
    if (!interruptState)
        interruptState = std::make_unique<InterruptBotState>();

    return *interruptState;
}

DefensiveBotState& BotTactics::GetDefensiveState()
{
    if (!defensiveState)
    {
        // SetPvpSkill fills the numbers; until it runs (Skill off) they are the skill-0 ones (full ability)
        PlayerbotAIConfig const& config = sPlayerbotAIConfig;
        defensiveState = std::make_unique<DefensiveBotState>();
        defensiveState->Params = ComputeDefensiveParams(0, attributes, SKILL_GATE_FULL, config.SkillGate.CurveExponent,
                                                        config.TacticDefensive);
    }

    return *defensiveState;
}

FakeBotState& BotTactics::GetFakeState()
{
    if (!fakeState)
    {
        // SetPvpSkill fills the numbers; until it runs (Skill off) they are the current skill's (0: full ability)
        fakeState = std::make_unique<FakeBotState>();
        fakeState->Params = ComputeFakeParams(botAI->GetPvpSkill(), attributes, sPlayerbotAIConfig.TacticFake);
    }

    return *fakeState;
}

TurnBotState& BotTactics::GetTurnState()
{
    if (!turnState)
    {
        // SetPvpSkill fills the numbers; until it runs (Skill off) they are the current skill's (0: full ability)
        PlayerbotAIConfig const& config = sPlayerbotAIConfig;
        turnState = std::make_unique<TurnBotState>();
        turnState->Params =
            ComputeTurnParams(botAI->GetPvpSkill(), attributes, config.SkillGate.CurveExponent, config.NaturalFacing);
    }

    return *turnState;
}

TrackBotState& BotTactics::GetTrackState()
{
    if (!trackState)
    {
        // SetPvpSkill fills the numbers; until it runs (Skill off) they are the current skill's (0: full ability)
        PlayerbotAIConfig const& config = sPlayerbotAIConfig;
        trackState = std::make_unique<TrackBotState>();
        trackState->Params =
            ComputeTrackParams(botAI->GetPvpSkill(), attributes, config.SkillGate.CurveExponent, config.NaturalTrack);
    }

    return *trackState;
}

PathBotState& BotTactics::GetPathState()
{
    if (!pathState)
    {
        // SetPvpSkill fills the numbers; until it runs (Skill off) they are the current skill's (0: full ability)
        PlayerbotAIConfig const& config = sPlayerbotAIConfig;
        pathState = std::make_unique<PathBotState>();
        pathState->Params =
            ComputePathParams(botAI->GetPvpSkill(), attributes, config.SkillGate.CurveExponent, config.NaturalPath);
    }

    return *pathState;
}

CoverBotState& BotTactics::GetCoverState()
{
    if (!coverState)
        coverState = std::make_unique<CoverBotState>();

    return *coverState;
}

KiteBotState& BotTactics::GetKiteState()
{
    if (!kiteState)
        kiteState = std::make_unique<KiteBotState>();

    return *kiteState;
}

DrinkBotState& BotTactics::GetDrinkState()
{
    if (!drinkState)
        drinkState = std::make_unique<DrinkBotState>();

    return *drinkState;
}

JumpBotState& BotTactics::GetJumpState()
{
    if (!jumpState)
        jumpState = std::make_unique<JumpBotState>();

    return *jumpState;
}

// The pre-gate stepper (G5e's fake cast, G2a's turn): UpdateAI calls it on every Player::Update while naturalPreGate,
// before the AI check delay and CanUpdateAI, so a fake's cancel lands within half a map tick of its instant whatever
// nextAICheckDelay is, and a turn moves every update. naturalPreGate stays on while anything is pending.
void BotTactics::StepNaturalPreGate(uint32 nowMs)
{
    bool pending = false;
    if (fakeState && fakeState->Pending())
    {
        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalCast::Step")
                                                        : nullptr);
        FakeStep const step = StepFakeCast(botAI, bot, *fakeState, nowMs, botAI->GetLastUpdateDiff());
        pending = step == FakeStep::Pending;
        // After a fake the bot acts at its next ordinary check, not at the end of the bar it waited for
        if (step == FakeStep::Cancelled)
            botAI->nextAICheckDelay = std::min(botAI->nextAICheckDelay, botAI->GetReactDelay());
    }

    // G2a: the turn's step (the server orientation every update, heartbeats by the packet rule)
    if (turnState && turnState->Active())
    {
        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalTurn::Step")
                                                        : nullptr);
        pending = StepNaturalTurn(bot, *turnState, nowMs) || pending;
    }

    naturalPreGate = pending;
}

// The pre-gate hook (CS5 design 4.1): UpdateAI calls it on every Player::Update before the AI check delay. G5e and
// G2a: the natural pre-gate stepper (a fake cast's cancel instant, a turn's steps); G2g: the jump test drives a frozen
// test bot (no engine runs). One bool test each idle; true when the jump test took the update.
bool BotTactics::PreGate()
{
    if (naturalPreGate)
        StepNaturalPreGate(getMSTime());

    if (jumpTest)
    {
        StepJumpTest(getMSTime());
        return true;
    }

    return false;
}

// The check hook (CS5 design 4.1): UpdateAI calls it at each AI check after RefreshNaturalTiming. busyModel: the
// natural reaction's busy bot runs here (UpdateAI's own-cast rules read it too)
void BotTactics::OnCheck(uint32 nowMs, bool busyModel)
{
    ReactionParams const& reaction = sPlayerbotAIConfig.NaturalReaction;
    if (reaction.Memory.Enable)
    {
        if (PerfMonitor::IsEnabled())
        {
            PerfMonitorScope scope(sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalReaction::ObserveOpponents"));
            ObserveOpponents(botAI, bot, busyModel, nowMs);
        }
        else
            ObserveOpponents(botAI, bot, busyModel, nowMs);
    }

    // G2b: the tracking ring follows the bot's target at every check of a fight, not only at its chase calls
    if (sPlayerbotAIConfig.NaturalTrack.Enable)
    {
        PerfMonitorScope scope(PerfMonitor::IsEnabled() ? sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalMove::Feed")
                                                        : nullptr);
        FeedNaturalTrack(botAI, bot, nowMs);
    }

    // G5c-c: the Interrupt tactic's match and the enemies' health trend, only while it follows the kick
    if (InterruptFollows(sPlayerbotAIConfig.TacticInterrupt))
        FollowInterruptFight(botAI, bot, nowMs);

    // G4e: the focus target, while the FocusTarget tactic is on here or a focus is left to clear
    if (botAI->TacticEnabledHere(TacticId::FocusTarget) || (focusState && focusState->Target.Pick.Guid != 0))
        FollowFocusTarget(botAI, bot, nowMs);

    // G5d: the Defensive tactic's health sampler, focus and opener while it is on here; deaths with a self-defensive
    // ready on both sides of a harness match
    if (botAI->IsPvpTestCounting() || botAI->TacticEnabledHere(TacticId::Defensive))
    {
        if (PerfMonitor::IsEnabled())
        {
            PerfMonitorScope scope(sPerfMonitor.acquire(PERF_MON_TOTAL, "Defensive::Follow"));
            FollowDefensive(botAI, bot, nowMs);
        }
        else
            FollowDefensive(botAI, bot, nowMs);
    }
}

bool BotTactics::HasFullSkillAbility()
{
    if (botAI->GetPvpSkill() == 0)
        return true;

    if (sPlayerbotAIConfig.SkillFullAbilityInPvE && !botAI->IsPvpFight())
        return true;

    return botAI->IsSkillExempt();
}

bool BotTactics::TacticActive(TacticId id)
{
    if (!botAI->TacticEnabledHere(id))
        return false;

    return TacticGatePasses(tacticGate, id, HasFullSkillAbility(), bot->GetGUID().GetCounter(),
                            sPlayerbotAIConfig.SkillNoticeWindowMs, getMSTime());
}

void BotTactics::SetFightEnemiesSnapshot(GuidVector const& enemies)
{
    uint32 const count = std::min<uint32>(enemies.size(), TACTIC_FIGHT_ENEMIES_MAX);
    for (uint32 index = 0; index < count; ++index)
        fightEnemies[index].store(enemies[index].GetRawValue(), std::memory_order_relaxed);

    fightEnemyCount.store(count, std::memory_order_release);
}

// Debug only: entries of two consecutive results may mix
uint32 BotTactics::GetFightEnemiesSnapshot(std::array<ObjectGuid, TACTIC_FIGHT_ENEMIES_MAX>& enemies) const
{
    uint32 const count = std::min(fightEnemyCount.load(std::memory_order_acquire), TACTIC_FIGHT_ENEMIES_MAX);
    for (uint32 index = 0; index < count; ++index)
        enemies[index] = ObjectGuid(fightEnemies[index].load(std::memory_order_relaxed));

    return count;
}

void BotTactics::RefreshNaturalTiming()
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    bool const measured = PerfMonitor::IsEnabled();
    if (!config.NaturalCadenceEnable && !measured)
    {
        // Off: upstream's check interval and stagger, the stretch everywhere
        timingScope = false;
        timingCombat = false;
        cadenceCombat = false;
        stretchOff = false;
        botAI->checkStaggerMs = UPSTREAM_CHECK_STAGGER_MS;
        return;
    }

    TacticContext const context = PlayerbotAI::ProbeTacticContext(bot);
    timingScope = ScopeAllows(config.NaturalCadenceContexts, context);
    timingCombat = timingScope && (bot->IsInCombat() || botAI->currentState == BOT_STATE_COMBAT);
    if (timingCombat && (bot->InBattleground() || bot->InArena()))
    {
        // A match counts once its gates are open
        Battleground* const battleground = bot->GetBattleground();
        timingCombat = battleground && battleground->GetStatus() == STATUS_IN_PROGRESS;
    }

    cadenceCombat = config.NaturalCadenceEnable && timingCombat;
    stretchOff = config.NaturalCadenceEnable && timingScope && config.NaturalReactionActive &&
                 ScopeAllows(config.NaturalReactionContexts, context);
    botAI->checkStaggerMs = cadenceCombat ? config.NaturalCadenceStaggerMs : UPSTREAM_CHECK_STAGGER_MS;
    if (!measured || !timingScope)
        return;

    PerfMonitor::CountBlock(sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalTiming::Checks (scope)"));
    if (timingCombat)
        PerfMonitor::CountBlock(sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalTiming::Checks (combat)"));
}

float BotTactics::GetEffectiveSkillReactFactor() const
{
    // Where the natural cadence and reaction both run, the reaction model carries the skill, unless
    // Natural.Cadence.SkillStretch keeps the stretch (G6d-a)
    if (skillReactFactor == 1.0f || !sPlayerbotAIConfig.SkillReactActive ||
        !SkillStretchApplies(stretchOff, sPlayerbotAIConfig.NaturalCadenceSkillStretch))
        return 1.0f;
    if (sPlayerbotAIConfig.SkillFullAbilityInPvE && !botAI->IsPvpFight())
        return 1.0f;
    return skillReactFactor;
}
