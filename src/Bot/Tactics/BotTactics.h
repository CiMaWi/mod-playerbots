/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_BOTTACTICS_H
#define _PLAYERBOT_BOTTACTICS_H

#include <array>
#include <atomic>
#include <memory>

#include "BotAttributes.h"
#include "Define.h"
#include "NaturalReaction.h"
#include "ObjectGuid.h"
#include "TacticGate.h"
#include "Tactics.h"

class Player;
class PlayerbotAI;
class Unit;
struct CcBotState;
struct CoverBotState;
struct DefensiveBotState;
struct DrinkBotState;
struct FakeBotState;
struct FocusBotState;
struct InterruptBotState;
struct JumpBotState;
struct KiteBotState;
struct PathBotState;
struct TrackBotState;
struct TurnBotState;

// The bot's tactics (CS5 design 4.1): its skill profile, natural timing, opponent memory and the tactic states.
// PlayerbotAI makes it in its constructor and keeps it until logout, so the pointer is never null and the world
// thread's writers (SetPvpSkill, the GM commands) need no lock. The tactic gate itself (mask, context, counting flag)
// stays on PlayerbotAI. Reached through PlayerbotAI::GetTactics().
class BotTactics
{
public:
    BotTactics(PlayerbotAI* botAI, Player* bot);
    ~BotTactics();
    BotTactics(BotTactics const&) = delete;
    BotTactics& operator=(BotTactics const&) = delete;

    // The skill profile from the bot's (new) skill: PlayerbotAI::SetPvpSkill, after it stored the skill and the first
    // roll
    void SetSkill(uint8 skill, uint8 firstRoll);
    // Reaction-delay factor from the hidden skill (Playerbots.Skill.React*), cached by SetPvpSkill. 1.0 when the
    // skill system or the factor range is off, and in PvE fights when Playerbots.Skill.FullAbilityInPvE is on.
    // Also 1.0 where the natural cadence and the natural reaction both run (the model replaces the stretch).
    float GetEffectiveSkillReactFactor() const;
    // Natural timing (Playerbots.Natural.Cadence.*), refreshed at each AI check: the bot is in the cadence's contexts
    // (also tracked while the cadence is off and PerfMon on, for its rows), in combat in a started match there, and
    // the cadence's faster check applies
    bool IsInNaturalTimingScope() const { return timingScope; }
    bool IsInNaturalTimingCombat() const { return timingCombat; }
    bool IsNaturalCadenceActive() const { return cadenceCombat; }
    bool IsSkillStretchOff() const { return stretchOff; }
    // At each AI check, by the bot's own update; O(1): the context probe and the match status
    void RefreshNaturalTiming();
    // Skill knob 2, noticing (Playerbots.Skill.Notice*, SkillStrategy): chance to miss a window and longest notice
    // delay from the hidden skill, cached by SetPvpSkill; both 0 when the knob is off.
    uint32 GetSkillNoticeMissPercent() const { return skillNoticeMissPercent; }
    uint32 GetSkillNoticeDelayMs() const { return skillNoticeDelayMs; }
    // Natural reaction (Playerbots.Natural.Reaction.*, NaturalReaction.h): ln(median) and spread of the bot's reaction
    // time, cached by SetPvpSkill for every skilled bot (read only while the option is on)
    ReactionState const& GetReactionState() const { return reactionState; }
    // G0h3's reaction types: the choice, load, ease, think, anticipation and queue prices from the bot's attributes,
    // cached by SetPvpSkill (all 0 at skill 0)
    ReactionTypeState const& GetReactionTypes() const { return reactionTypes; }
    // What the bot saw its enemies cast and cancel in the current match (DT.12; bounded), and its own cast and GCD
    // ends for the queue-like follow-up and the busy-bot rules (G0h3-b). The bot's own update only.
    OpponentMemory& GetOpponentMemory() { return opponentMemory; }
    ReactionBusyState& GetReactionBusy() { return reactionBusy; }
    // Full ability for the tactic gate: skill 0, PvE with Playerbots.Skill.FullAbilityInPvE, or exempt
    bool HasFullSkillAbility();
    // The tactic gate (TacticGate.h), cached by SetPvpSkill: enabled here (mask and scope), known at the bot's skill
    // and used in the current window. Later tactics call this, then NoteTacticUse when they act.
    bool TacticActive(TacticId id);
    TacticGateState const& GetTacticGate() const { return tacticGate; }
    // Effort 0-100 (100 = tries everything) and style traits 0-100 (50 = neutral), hidden like the skill
    uint8 GetSkillEffort() { return HasFullSkillAbility() ? SKILL_GATE_FULL : tacticGate.Effort; }
    uint8 GetSkillStyle(SkillStyleTrait trait) const { return tacticGate.Style[static_cast<uint32>(trait)]; }
    // Attributes (Playerbots.Skill.Attributes.*, BotAttributes.h): eight abilities on the skill's 1-99 scale and four
    // traits (0-100, 50 neutral) drawn from the skill and the guid hash, cached by SetPvpSkill (abilities 0 and traits
    // 50 at skill 0). With the defaults every ability is the skill. Hidden like the skill: GM commands, records only.
    BotAttributeValues const& GetAttributes() const { return attributes; }
    uint8 GetAttribute(BotAttribute attribute) const { return attributes.Get(attribute); }
    // Lab copies (G0j-b): the guid whose hashes draw the attributes and the tactic gate (0 = the bot's own). Set on the
    // world thread, then SetPvpSkill with the template's skill.
    uint32 GetAttributeTemplate() const { return attributeTemplate; }
    void SetAttributeTemplate(uint32 templateGuid) { attributeTemplate = templateGuid; }
    // A tactic fired: counted per tactic and context for .playerbots tactics stats
    void NoteTacticUse(TacticId id) const;
    // Target hold and team plan (FocusState.h, G1b): the cached FocusParams and the hold's and the plan's state.
    // Touched by the bot's own update; the GM commands read and force it on the world thread while the maps wait.
    FocusBotState& GetFocusState();
    // Crowd control (CcState.h, G4b): the cached CcParams, the DR memory, the rank-resolved table and the trigger's
    // answer. The bot's own update and the aura hooks on its map thread; the GM commands on the world thread.
    CcBotState& GetCcState();
    // Interrupts (ArenaInterrupt.h, G5b): the bot's decisions on the enemy casts in view. The bot's own update.
    InterruptBotState& GetInterruptState();
    // Defensives on burst (ArenaDefensive.h, G5d), fake casts (NaturalCast.h, G5e), natural turning (NaturalTurn.h,
    // G2a) and tracking (NaturalMove.h, G2b): each bot's state, made at first use. The bot's own update (and the
    // aura or spell hooks on its map thread); the GM commands on the world thread while the maps wait.
    DefensiveBotState& GetDefensiveState();
    FakeBotState& GetFakeState();
    TurnBotState& GetTurnState();
    TrackBotState& GetTrackState();
    // Natural paths (NaturalPath.h, G2c) and the Los and Pillar tactics' cover state (ArenaCover.h, G2e, G2f): the same
    PathBotState& GetPathState();
    CoverBotState& GetCoverState();
    // The Kite, Drink and Jump tactics' state (ArenaKite.h, ArenaDrink.h, ArenaJump.h; G3a, G3b, G2g): the same
    KiteBotState& GetKiteState();
    DrinkBotState& GetDrinkState();
    JumpBotState& GetJumpState();
    // The jump test (G2g, `tactics jump test`): while set, UpdateAI runs StepJumpTest instead of the engines (a frozen
    // test bot); one bool test else
    void StepJumpTest(uint32 nowMs);
    void SetJumpTest(bool on) { jumpTest = on; }
    bool IsJumpTest() const { return jumpTest; }
    // The natural pre-gate stepper (G5e's cancel instant, G2a's turn steps): called from UpdateAI before the AI
    // gates on every Player::Update while SetNaturalPreGate(true), else one bool test. nowMs = getMSTime().
    void StepNaturalPreGate(uint32 nowMs);
    void SetNaturalPreGate(bool on) { naturalPreGate = on; }
    bool IsNaturalPreGate() const { return naturalPreGate; }
    // The Interrupt tactic's hook (enabled here): true when the bot's kick may go at the enemy's cast now. One
    // decision per cast (the shortcut or the judging path), its commit point from the bar's start.
    bool DecideInterrupt(Unit* enemy);
    // Debug snapshot for .playerbots tactics show (world thread): the last "fight enemies" result
    void SetFightEnemiesSnapshot(GuidVector const& enemies);
    uint32 GetFightEnemiesSnapshot(std::array<ObjectGuid, TACTIC_FIGHT_ENEMIES_MAX>& enemies) const;

private:
    // PlayerbotAI's update loop and cast path read and write the states in place until the hooks are BotTactics'
    // methods (CS16e, the hooks)
    friend class PlayerbotAI;

    PlayerbotAI* const botAI;
    Player* const bot;
    float skillReactFactor = 1.0f;
    uint32 skillNoticeMissPercent = 0;
    uint32 skillNoticeDelayMs = 0;
    ReactionState reactionState;
    ReactionTypeState reactionTypes;
    OpponentMemory opponentMemory;
    ReactionBusyState reactionBusy;
    bool timingScope = false;
    bool timingCombat = false;
    bool cadenceCombat = false;
    bool stretchOff = false;
    TacticGateState tacticGate = ComputeTacticGate(TacticGateParams(), {}, 0, 0);
    std::unique_ptr<FocusBotState> focusState;          // made at first use (GetFocusState); complete in BotTactics.cpp
    std::unique_ptr<CcBotState> ccState;                // made at first use (GetCcState); complete in BotTactics.cpp
    std::unique_ptr<InterruptBotState> interruptState;  // made at first use (GetInterruptState); the same
    std::unique_ptr<DefensiveBotState> defensiveState;  // the same (GetDefensiveState)
    std::unique_ptr<FakeBotState> fakeState;            // the same (GetFakeState)
    std::unique_ptr<TurnBotState> turnState;            // the same (GetTurnState)
    std::unique_ptr<TrackBotState> trackState;          // the same (GetTrackState)
    std::unique_ptr<PathBotState> pathState;            // the same (GetPathState)
    std::unique_ptr<CoverBotState> coverState;          // the same (GetCoverState)
    std::unique_ptr<KiteBotState> kiteState;            // the same (GetKiteState)
    std::unique_ptr<DrinkBotState> drinkState;          // the same (GetDrinkState)
    std::unique_ptr<JumpBotState> jumpState;            // the same (GetJumpState)
    bool jumpTest = false;        // true while the jump test drives the bot (StepJumpTest); one bool test else
    bool naturalPreGate = false;  // true while a pre-gate stepper has work (a turn, a fake cast); one bool test else
    BotAttributeValues attributes = ComputeBotAttributes(BotAttributeParams(), SkillDistributionParams(), 0, 0, 0);
    uint32 attributeTemplate = 0;
    std::atomic<uint32> fightEnemyCount{0};
    std::array<std::atomic<uint64>, TACTIC_FIGHT_ENEMIES_MAX> fightEnemies{};
};

#endif
