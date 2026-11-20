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
#include "Group.h"
#include "MapMgr.h"
#include "NaturalCast.h"
#include "NaturalMove.h"
#include "NaturalPath.h"
#include "NaturalReaction.h"
#include "NaturalTrace.h"
#include "NaturalTurn.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PvpTestCounters.h"
#include "ReactionEngine.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "TacticFollowers.h"

namespace
{
// A tactic state's numbers from the skill, the attributes and the gate's effort (G1b, G4b, G5d, G5e, G2a, G2b, G2c):
// SetSkill for the states that exist, an accessor for a state it makes (CS16f: the states are made at first use)
void FillParams(FocusBotState& state, uint8 skill, BotAttributeValues const& attributes, uint8 /*effort*/)
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    state.Params = ComputeFocusParams(skill, attributes, config.SkillGate.CurveExponent, config.TacticFocus);
    state.AddonKnowledge = FocusAddonKnowledge(skill, attributes.Get(BotAttribute::Knowledge),
                                               config.TacticFocus.FocusAddonKnowledgeFromSkill);
}

void FillParams(CcBotState& state, uint8 skill, BotAttributeValues const& attributes, uint8 effort)
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    state.Params = ComputeCcParams(skill, attributes, effort, config.SkillGate.CurveExponent, config.TacticCc);
}

void FillParams(DefensiveBotState& state, uint8 skill, BotAttributeValues const& attributes, uint8 effort)
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    state.Params =
        ComputeDefensiveParams(skill, attributes, effort, config.SkillGate.CurveExponent, config.TacticDefensive);
}

void FillParams(FakeBotState& state, uint8 skill, BotAttributeValues const& attributes, uint8 /*effort*/)
{
    state.Params = ComputeFakeParams(skill, attributes, sPlayerbotAIConfig.TacticFake);
}

void FillParams(TurnBotState& state, uint8 skill, BotAttributeValues const& attributes, uint8 /*effort*/)
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    state.Params = ComputeTurnParams(skill, attributes, config.SkillGate.CurveExponent, config.NaturalFacing);
}

void FillParams(TrackBotState& state, uint8 skill, BotAttributeValues const& attributes, uint8 /*effort*/)
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    state.Params = ComputeTrackParams(skill, attributes, config.SkillGate.CurveExponent, config.NaturalTrack);
}

void FillParams(PathBotState& state, uint8 skill, BotAttributeValues const& attributes, uint8 /*effort*/)
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    state.Params = ComputePathParams(skill, attributes, config.SkillGate.CurveExponent, config.NaturalPath);
}
}  // namespace

BotTactics::BotTactics(PlayerbotAI* botAI, Player* bot) : botAI(botAI), bot(bot) {}

uint32 BotTactics::GetCheckStaggerMs() const
{
    return CheckStaggerMs(bot->GetGUID().GetCounter(), botAI->checkStaggerMs);
}

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
    SkillDistributionParams const distribution{config.Skill.DistributionUniform, config.Skill.Mean, config.Skill.Sd,
                                               config.Skill.Min, config.Skill.Max};
    attributes = ComputeBotAttributes(config.SkillAttributes, distribution, skill, firstRoll, guid);
    // The tactic gate from the knowledge, discipline, grit and style attributes (skill 0: full ability)
    tacticGate = ComputeTacticGate(config.SkillGate, config.Tactics.Settings,
                                   AttributeGateInputs(config.SkillAttributes, attributes, skill), guid);
    // Speed carries the reaction draw, D3's stretch and D4's notice delay; Focus D4's miss share. Cached whether
    // the natural reaction is active or not, so a .reload config can turn it on for logged-in bots.
    uint8 const speed = attributes.Get(BotAttribute::Speed);
    reactionState = ComputeReactionState(config.NaturalReaction, speed, config.SkillGate.CurveExponent);
    // The tactic states' numbers (hold and plan, crowd control, defensive, fake, facing, tracking, paths), whatever
    // their Enable says; skill 0 gives the skill-99 values. Only the states that exist: an accessor that makes one
    // later computes them from the same skill (FillParams)
    if (focusState)
        FillParams(*focusState, skill, attributes, tacticGate.Effort);
    if (ccState)
        FillParams(*ccState, skill, attributes, tacticGate.Effort);
    if (defensiveState)
        FillParams(*defensiveState, skill, attributes, tacticGate.Effort);
    if (fakeState)
        FillParams(*fakeState, skill, attributes, tacticGate.Effort);
    if (turnState)
        FillParams(*turnState, skill, attributes, tacticGate.Effort);
    if (trackState)
        FillParams(*trackState, skill, attributes, tacticGate.Effort);
    if (pathState)
        FillParams(*pathState, skill, attributes, tacticGate.Effort);
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
    if (config.Skill.ReactActive)
        skillReactFactor =
            SkillLerp(config.Skill.ReactFactorLow, config.Skill.ReactFactorHigh, speedPosition, exponent);

    if (config.Skill.NoticeActive)
    {
        skillNoticeMissPercent = SkillLerpRounded(config.Skill.NoticeMissPercentLow, config.Skill.NoticeMissPercentHigh,
                                                  focusPosition, exponent);
        skillNoticeDelayMs =
            SkillLerpRounded(config.Skill.NoticeDelayMsLow, config.Skill.NoticeDelayMsHigh, speedPosition, exponent);
    }
}

FocusBotState& BotTactics::GetFocusState()
{
    // Made at first use with the numbers of the bot's skill (SetSkill recomputes them when the skill changes)
    if (!focusState)
    {
        focusState = std::make_unique<FocusBotState>();
        FillParams(*focusState, botAI->GetPvpSkill(), attributes, tacticGate.Effort);
    }

    return *focusState;
}

CcBotState& BotTactics::GetCcState()
{
    if (!ccState)
    {
        ccState = std::make_unique<CcBotState>();
        FillParams(*ccState, botAI->GetPvpSkill(), attributes, tacticGate.Effort);
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
        defensiveState = std::make_unique<DefensiveBotState>();
        FillParams(*defensiveState, botAI->GetPvpSkill(), attributes, tacticGate.Effort);
    }

    return *defensiveState;
}

FakeBotState& BotTactics::GetFakeState()
{
    if (!fakeState)
    {
        fakeState = std::make_unique<FakeBotState>();
        FillParams(*fakeState, botAI->GetPvpSkill(), attributes, tacticGate.Effort);
    }

    return *fakeState;
}

TurnBotState& BotTactics::GetTurnState()
{
    if (!turnState)
    {
        turnState = std::make_unique<TurnBotState>();
        FillParams(*turnState, botAI->GetPvpSkill(), attributes, tacticGate.Effort);
    }

    return *turnState;
}

TrackBotState& BotTactics::GetTrackState()
{
    if (!trackState)
    {
        trackState = std::make_unique<TrackBotState>();
        FillParams(*trackState, botAI->GetPvpSkill(), attributes, tacticGate.Effort);
    }

    return *trackState;
}

PathBotState& BotTactics::GetPathState()
{
    if (!pathState)
    {
        pathState = std::make_unique<PathBotState>();
        FillParams(*pathState, botAI->GetPvpSkill(), attributes, tacticGate.Effort);
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

// The pre-gate stepper (G5e's fake cast, G2a's turn): UpdateAI calls it on every Player::Update while its work bit,
// before the AI check delay and CanUpdateAI, so a fake's cancel lands within half a map tick of its instant whatever
// nextAICheckDelay is, and a turn moves every update. The bit stays on while anything is pending.
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

    if (!pending)
        botAI->EndWork(BotWork::PreGateStep);
}

// The pre-gate hook (CS5 design 4.1): UpdateAI calls it on a Player::Update before the AI check delay while work
// (its load of the work mask) has either bit. G5e and G2a: the natural pre-gate stepper (a fake cast's cancel instant,
// a turn's steps); G2g: the jump test drives a frozen test bot (no engine runs). True when the jump test took the
// update.
bool BotTactics::PreGate(uint32 work)
{
    if (work & BotWork::PreGateStep)
        StepNaturalPreGate(getMSTime());

    if (work & BotWork::JumpTest)
    {
        StepJumpTest(getMSTime());
        return true;
    }

    return false;
}

void BotTactics::SetJumpTest(bool on) { botAI->SetWork(BotWork::JumpTest, on); }

void BotTactics::SetNaturalPreGate(bool on) { botAI->SetWork(BotWork::PreGateStep, on); }

void BotTactics::LogPreGateMisses(uint32 work) const
{
    if (timingCombat && !(work & BotWork::TimingRow))
        LOG_INFO("playerbots", "PreGate miss: TimingRow {}", bot->GetName());

    bool const pending = (fakeState && fakeState->Pending()) || (turnState && turnState->Active());
    if (pending && !(work & BotWork::PreGateStep))
        LOG_INFO("playerbots", "PreGate miss: PreGateStep {}", bot->GetName());
}

// The check hook (CS5 design 4.1): UpdateAI calls it at each AI check after RefreshNaturalTiming. busyModel: the
// natural reaction's busy bot runs here (UpdateAI's own-cast rules read it too). The followers run only for a bot
// the live test admits (design 6.4, TacticFollowers.h): the harness counts it, a follower left state to close, one
// of the followers' tactics is on in the stored context, or the opponent memory runs in a match. True when live.
bool BotTactics::OnCheck(uint32 nowMs, bool busyModel, TacticContext probed)
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    TacticCheckInputs inputs;
    inputs.Counting = botAI->IsPvpTestCounting();
    inputs.Leftover = opponentMemory.MatchId != 0 || (focusState && focusState->Target.Pick.Guid != 0);
    inputs.Mask = botAI->GetTacticMask();
    inputs.Stored = botAI->GetTacticContext();
    inputs.Probed = probed;
    if (!TacticCheckLive(config.TacticFollowersFor, inputs))
    {
        if (config.Lone.TestMissLog)
            LogLiveMiss();

        return false;
    }

    if (PerfMonitor::IsEnabled())
    {
        PerfMonitorScope scope(sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalReaction::ObserveOpponents"));
        ObserveOpponents(botAI, bot, busyModel, nowMs);
    }
    else
        ObserveOpponents(botAI, bot, busyModel, nowMs);

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

    return true;
}

// The miss switch (Playerbots.Test.MissLog; design 6.3, 6.4): a check the live test skipped while a follower's own
// guard passes, which would mean a term the live test lacks. The followers' guards as they read them at their entry
void BotTactics::LogLiveMiss() const
{
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    Battleground* const battleground = bot->GetBattleground();
    bool const match = battleground && battleground->GetStatus() == STATUS_IN_PROGRESS;
    if (match || opponentMemory.MatchId != 0)
        LOG_INFO("playerbots", "Live miss: ObserveOpponents {}", bot->GetName());

    std::array<bool, TACTIC_FOLLOWER_COUNT> const passes = {
        config.NaturalTrack.Enable && botAI->TacticEnabledHere(TacticId::Natural),
        InterruptFollows(config.TacticInterrupt) && botAI->TacticEnabledHere(TacticId::Interrupt),
        botAI->TacticEnabledHere(TacticId::FocusTarget) || (focusState && focusState->Target.Pick.Guid != 0),
        botAI->IsPvpTestCounting() || botAI->TacticEnabledHere(TacticId::Defensive)};
    for (uint32 index = 0; index < TACTIC_FOLLOWER_COUNT; ++index)
    {
        if (passes[index])
            LOG_INFO("playerbots", "Live miss: {} {}", TACTIC_FOLLOWER_NAMES[index], bot->GetName());
    }
}

// The cast hooks (CS5 design 4.1): CastSpell calls BeforeCast before its stand check and AfterCast once the cast
// is prepared
void BotTactics::BeforeCast(Unit* target, SpellInfo const* spellInfo)
{
    // G2a: a cast that waited for the bot's turn goes ahead (tactics.turnWaitMs; casts that need the facing only)
    if (turnState && turnState->CastWaitSinceMs != 0 && (spellInfo->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT))
    {
        // the span ends here whatever it was (a wait at another target, or one the bot gave up, counts nothing)
        uint32 const waitMs = turnState->TakeCastWait(target->GetGUID().GetRawValue(), getMSTime());
        if (waitMs != 0)
        {
            ++turnState->CastWaits;
            turnState->CastWaitMs += waitMs;
            botAI->CountTactic(TacticCounter::TurnWaitMs, waitMs);
        }
    }
}

void BotTactics::AfterCast(uint32 spellId, SpellInfo const* spellInfo, Spell* spell, Unit* target, CastNote const& note)
{
    if (note.followsKick && note.interruptHit.Use == InterruptUse::Kick)
        NoteInterruptKick(botAI, bot, spellInfo, note.interruptHit, getMSTime());

    // 13e-decide fix (Interrupt.SeeCancelLag): the bot pressed an interrupt at the enemy's cast; the ended bar it still
    // sees is then no bait (no second interrupt on it, InterruptSpellTrigger)
    if (interruptState && target && target != bot && sPlayerbotAIConfig.TacticInterrupt.SeeCancelLag &&
        InterruptUseOf(spellInfo) != InterruptUse::None)
    {
        if (InterruptCastSlot* const slot = interruptState->Latest(target->GetGUID().GetCounter()))
            slot->PressedMs = std::max(getMSTime(), 1u);
    }

    if (note.counting)
    {
        botAI->CountPvpTestCast(spellId, note.drGroup, note.atImmune);
        // G4d-c: a cast into the core's DR immunity (cc.atImmune), beside the CC cast lines: what the bot's memory
        // held of the group (none: never seen or its slot went to another group) against the core's level
        if (note.atImmune && sLog->ShouldLog("playerbots", LogLevel::LOG_LEVEL_TRACE))
        {
            uint32 const nowMs = getMSTime();
            uint8 memoryLevel = 0;
            uint32 sinceSeenMs = 0;
            int32 endsInMs = 0;
            bool const held =
                ccState && ccState->Memory.Recall(target->GetGUID().GetRawValue(), static_cast<uint8>(note.drGroup),
                                                  nowMs, memoryLevel, sinceSeenMs, endsInMs);
            LOG_TRACE("playerbots",
                      "CC immune: {} {} ({}) on {} group {} ({}) by {}, memory {} (seen {} ms ago, aura end {} ms; "
                      "aware {}, miss {}%), core {}, at {} ms",
                      bot->GetName(), spellInfo->SpellName[0] ? spellInfo->SpellName[0] : "?", spellId,
                      target->GetName(),
                      note.drGroup < PVPTEST_DR_GROUP_COUNT ? PVPTEST_DR_GROUP_NAMES[note.drGroup] : "?", note.drGroup,
                      ccState && ccState->TeamCcCasting ? "team cc" : "other",
                      held ? std::to_string(memoryLevel) : std::string("none"), held ? sinceSeenMs : 0,
                      held ? endsInMs : 0, ccState && ccState->Params.DrAware ? 1 : 0,
                      ccState ? ccState->Params.DrMissPct : 0, static_cast<uint32>(DIMINISHING_LEVEL_IMMUNE), nowMs);
        }

        CountInterruptHit(botAI, target, note.interruptHit, getMSTime());
        // tactics.ccOnTarget (G4b): the planned CC target is the group's moon in an arena, else the bot's kept one
        if (IsPvpTestCcGroup(note.drGroup) && target != bot)
        {
            Group* const group = bot->GetGroup();
            ObjectGuid planned = group && botAI->GetTacticContext() == TacticContext::Arena
                                     ? group->GetTargetIcon(FOCUS_ICON_MOON)
                                     : ObjectGuid::Empty;
            if (planned.IsEmpty() && ccState)
                planned = ccState->CcTarget;

            if (!planned.IsEmpty() && planned == target->GetGUID())
                botAI->CountTactic(TacticCounter::CcOnTarget);
        }
    }

    // G4e: focus and switch actions (the harness's counters, tactics show, the trace line)
    if (note.notesFocus)
        NoteFocusAct(botAI, bot, spellInfo, target, note.oldSel, note.ownBefore, note.counting, note.drGroup);

    // G5e: a long cast may be a fake (one tactic test; a pending fake ends with a new long cast)
    if (botAI->TacticEnabledHere(TacticId::Fake) || (fakeState && fakeState->Pending()))
        DecideFakeCast(botAI, bot, spellId, getMSTime());

    // G0e: a cast with a cast time begins (an instant one was cast inside prepare: its finish row is enough)
    if (NaturalTrace::Any())
        NaturalTrace::OnBotCastStart(bot, spell, target);
}

// The own cast at a check (UpdateAI, a spell still preparing): its window as the bot sees it, and the busy stop
// for a kick; true when the bot stops the cast
bool BotTactics::OnOwnCast(Spell* currentSpell, uint32 ownLeft, bool busyModel, uint32 nowMs)
{
    ReactionParams const& reaction = sPlayerbotAIConfig.NaturalReaction;
    bool stoppedForKick = false;
    reactionBusy.CastStartMs =
        nowMs - static_cast<uint32>(std::max(0, currentSpell->GetCastTime() - static_cast<int32>(ownLeft)));
    reactionBusy.CastEndMs = nowMs + ownLeft;
    if (busyModel && reaction.Reeval.Enable)
    {
        if (PerfMonitor::IsEnabled())
        {
            PerfMonitorScope scope(sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalReaction::BusyStop"));
            stoppedForKick = StopOwnCastForKickDue(botAI, bot, ownLeft, nowMs);
        }
        else
            stoppedForKick = StopOwnCastForKickDue(botAI, bot, ownLeft, nowMs);
    }

    return stoppedForKick;
}

// After the engines ran (UpdateAI): a busy stop the bot's kick did not follow
void BotTactics::NoteUnfollowedStop(uint32 nowMs)
{
    if (!bot->HasSpellCooldown(reactionBusy.KickSpellId))
    {
        reactionBusy.NoteStopUnfollowed(nowMs);
        LOG_TRACE("playerbots.reaction", "busy stop unfollowed skill {} kick {} at {}", botAI->GetPvpSkill(),
                  reactionBusy.KickSpellId, nowMs);
        if (PerfMonitor::IsEnabled())
            PerfMonitor::CountBlock(sPerfMonitor.acquire(PERF_MON_TOTAL, "NaturalReaction::UnfollowedStop"));
    }
}

bool BotTactics::HasFullSkillAbility()
{
    if (botAI->GetPvpSkill() == 0)
        return true;

    if (sPlayerbotAIConfig.Skill.FullAbilityInPvE && !botAI->IsPvpFight())
        return true;

    return botAI->IsSkillExempt();
}

bool BotTactics::TacticActive(TacticId id)
{
    if (!botAI->TacticEnabledHere(id))
        return false;

    return TacticGatePasses(tacticGate, id, HasFullSkillAbility(), bot->GetGUID().GetCounter(),
                            sPlayerbotAIConfig.Skill.NoticeWindowMs, getMSTime());
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

void BotTactics::RefreshNaturalTiming(TacticContext context)
{
    checkProbe = context;
    PlayerbotAIConfig const& config = sPlayerbotAIConfig;
    bool const measured = PerfMonitor::IsEnabled();
    // The timing row's work bit follows timingCombat (one atomic write at its change)
    bool const timedBefore = timingCombat;
    timingScope = ScopeAllows(config.NaturalCadence.Contexts, context);
    timingCombat = timingScope && (bot->IsInCombat() || botAI->currentState == BOT_STATE_COMBAT);
    if (timingCombat && (bot->InBattleground() || bot->InArena()))
    {
        // A match counts once its gates are open
        Battleground* const battleground = bot->GetBattleground();
        timingCombat = battleground && battleground->GetStatus() == STATUS_IN_PROGRESS;
    }

    if (timingCombat != timedBefore)
        botAI->SetWork(BotWork::TimingRow, timingCombat);

    cadenceCombat = timingCombat;
    stretchOff = timingScope && ScopeAllows(config.NaturalReaction.Contexts, context);
    botAI->checkStaggerMs = cadenceCombat ? config.NaturalCadence.StaggerMs : UPSTREAM_CHECK_STAGGER_MS;
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
    if (skillReactFactor == 1.0f || !sPlayerbotAIConfig.Skill.ReactActive ||
        !SkillStretchApplies(stretchOff, sPlayerbotAIConfig.NaturalCadence.SkillStretch))
        return 1.0f;
    if (sPlayerbotAIConfig.Skill.FullAbilityInPvE && !botAI->IsPvpFight())
        return 1.0f;
    return skillReactFactor;
}
