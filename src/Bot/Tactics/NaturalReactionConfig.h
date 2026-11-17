/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALREACTIONCONFIG_H
#define _PLAYERBOT_NATURALREACTIONCONFIG_H

#include <array>
#include <string>

#include "Define.h"
#include "TacticIds.h"

// The numbers of the natural reaction (Playerbots.Natural.Reaction.*, PlayerbotAIConfig::NaturalReaction), kept apart
// from NaturalReaction.h so that the model can change without rebuilding every PlayerbotAIConfig.h user.

// The structure of a reaction: one known answer, a choice between real options, or an event the bot predicted
enum class ReactionType : uint8
{
    Simple = 0,
    Choice,
    Anticipated
};

// How hard the answer is to give: a common button, a rare one (defensive cooldowns, crowd-control breaks), an action
// on the focus target (G4e) or on a new target (a unit other than the bot's target)
enum class ReactionEase : uint8
{
    Common = 0,
    Rare,
    Focus,
    NewTarget,
    Count
};

constexpr uint32 REACTION_EASE_COUNT = static_cast<uint32>(ReactionEase::Count);

// The weak bot's shortcut (it answers the first event it sees) or the skilled bot's deep check (it judges first)
enum class ReactionThinkDepth : uint8
{
    Shortcut = 0,
    Deep
};

// Reaction types (G0h3, in Playerbots.Natural.Reaction.Contexts): each part has prices at attribute 1
// and 99 (Playerbots.Skill.Attributes.*; with their defaults every ability is the skill), lerped along
// Playerbots.Skill.CurveExponent. Every cost lies inside the reaction (the trace's ready), so the end-to-end
//   target
// keeps its sum. The playerbots.reaction trace lines add type (0 simple, 1 choice, 2 anticipated), options,
//   load
// (other events pending), ease (0 common, 1 rare, 2 focus target, 3 new target), think (ms), cost (choice +
//   load +
// ease + think, ms), busy and own (see Reeval).
// Choice (Wisdom): Hick's law, MsPerDoubling per doubling of the real options (one option: none; at most
//   MaxOptions, 1-64). Counted from what the bot has already built: a target switch and crowd control from its
//   attackers list; an interrupt and a dispel are one option (no count without a new scan). 0-2000 ms.
// Load (Focus): per other pending event (at most MaxEvents, 1-5) MsPerEvent more and MissPercentPerEvent
//   (0-100) more miss share; an event that outranks every pending one (interrupt, defensive, cc break, dispel,
//   crowd control, target switch) keeps TopProtect (0-1) of both off.
// Ease (Dexterity): the price of the answer's button: Common (interrupts, dispels, crowd control) < Rare
//   (defensive cooldowns, cc breaks, strangulate, mass dispel) < Focus (an action on the focus target, G4e;
//   below FocusDexterity, 1-99, a new target's price) < NewTarget (a target switch, or an action on an enemy
//   other than the bot's target). Each price is raised to 1 ms above the one before at both ends if needed, so
//   the order holds at every Dexterity.
// Think (Wisdom): a decision's time, the Shortcut (the first event seen, no judgement: today's interrupt) or
//   the Deep check (charged by later tactics, which take it DeepPercent, 0-100, of the time); PatienceWaitMs
//   (Patience trait 0 and 100, straight line) is how long a bot waits for a better moment (read by later
//   tactics).
// Anticipate (Foresight): an interrupt the opponent memory predicts (the enemy cast the same spell the time
//   before) saves Share (0-0.95) of the draw, never below MinMs; it is no choice.
// Queue (Speed): a bot whose own cast or GCD ends before its next check checks MinMs-MaxMs after the end
//   instead (never later; its own timer, no wake-up), as a human mashing the button.
// Reeval (the user's notes of 2026-10-30): Enable stops the bot's own cast (MinOwnRemainingMs or more left) for
//   a kick of its target's cast of at least ImportantCastMs (a stand-in for judging the spell) that the own
//   cast would end too late for, once its reaction to the cast bar is over (draw and shortcut think cost, end
//   to end from the bar's start) and its interrupt is ready; the kick follows in the same check. Once per enemy
//   cast; after a stop the kick did not follow (its trigger or a higher action picked something else) none for
//   30 s. An opt-in stand-in until G5b's judgement: it never weighs the own cast (a heal on a dying partner is
//   stopped too). A kick that a bot busy with its own cast, the kick's cooldown or the GCD would land only
//   within BusyMarginMs of the cast's end or after it is dropped, when being busy is what makes it late (one
//   late from its reaction alone is tried). The trace's busy field: 1 stopped its own cast for the kick, 2
//   skipped; own: ms of own cast within the event.
// Memory (DT.12): per bot and match (cleared when it ends), the cast bars the bot sees on its target and
//   attackers (at most 8 enemies) at its own checks; a bar gone before its end without a visible interrupt
//   (school lock, silence, stun, fear, death) is a cancel. The fake rate counts from MinCasts (1-100) casts; a
//   bar gone within EndToleranceMs of its end finished. Anticipate reads it.
// Movement: avoid aoe as a reaction, stamped with the ground effect's start (a trap or trigger NPC: the first
//   look); only bots with the avoid aoe strategy have it (Playerbots.AutoAvoidAoe, bots with a real master).
// Times 0-10000 ms. On a running server .reload config then .playerbots rndbot reload: changed prices reach a
// logged-in bot at its next skill load. .playerbots tactics show <bot> lists the bot's prices.
// The prices are tuning values (Playerbots.Natural.Reaction.<Part>.*, config table rows in NaturalReactionConfig.cpp)
// without a conf.dist line; `playerbots config Playerbots.Natural.Reaction` prints each with its default and bounds.

// G0h3's parts with their prices at attribute 1 and 99 (lerped along Playerbots.Skill.CurveExponent)
struct ReactionChoiceParams  // Wisdom
{
    float MsPerDoublingAt1 = 120.0f;
    float MsPerDoublingAt99 = 50.0f;
    uint32 MaxOptions = 8;
};

struct ReactionLoadParams  // Focus
{
    uint32 MsPerEventAt1 = 200;
    uint32 MsPerEventAt99 = 80;
    uint32 MissPercentPerEventAt1 = 10;
    uint32 MissPercentPerEventAt99 = 2;
    // The share of the penalty and the miss share kept off an event that outranks every pending one
    float TopProtectAt1 = 0.0f;
    float TopProtectAt99 = 0.8f;
    uint32 MaxEvents = 3;
};

struct ReactionEaseParams  // Dexterity
{
    // By ReactionEase, strictly rising at both ends (so at every Dexterity)
    std::array<uint32, REACTION_EASE_COUNT> MsAt1 = {0, 250, 350, 450};
    std::array<uint32, REACTION_EASE_COUNT> MsAt99 = {0, 80, 120, 180};
    // Below this Dexterity an action on the focus target costs a new target's price
    uint8 FocusDexterity = 40;
};

struct ReactionThinkParams  // Wisdom (depth and cost), Patience (waiting)
{
    uint32 ShortcutMsAt1 = 40;
    uint32 ShortcutMsAt99 = 20;
    uint32 DeepMsAt1 = 400;
    uint32 DeepMsAt99 = 150;
    uint32 DeepPercentAt1 = 0;
    uint32 DeepPercentAt99 = 100;
    // How long a bot waits for a better moment at the Patience trait 0 and 100 (straight line)
    uint32 PatienceWaitMsAt0 = 0;
    uint32 PatienceWaitMsAt100 = 1500;
};

struct ReactionAnticipateParams  // Foresight
{
    // The share of the draw a predicted event saves (never below MinMs)
    float ShareAt1 = 0.1f;
    float ShareAt99 = 0.5f;
};

struct ReactionQueueParams  // Speed (G0h3-b)
{
    uint32 MinMsAt1 = 60;
    uint32 MaxMsAt1 = 250;
    uint32 MinMsAt99 = 10;
    uint32 MaxMsAt99 = 80;
};

// The busy bot (G0h3-b; the user's DT.11 addendum, 2026-10-30)
struct ReactionReevalParams
{
    // Stop the bot's own cast for a kick of an important enemy cast (at least ImportantCastMs: a stand-in for G5b's
    // judgement) when the own cast has MinOwnRemainingMs left and would end too late for the kick; once per enemy cast
    bool Enable = false;
    uint32 ImportantCastMs = 1500;
    uint32 MinOwnRemainingMs = 300;
    // No kick that a bot busy with its own cast or GCD could land only within BusyMarginMs of the enemy cast's end,
    // when being busy is what makes it late
    uint32 BusyMarginMs = 100;
};

struct ReactionMemoryParams  // G0h3-b
{
    // The fake-cast rate counts from MinCasts casts seen; a cast gone within EndToleranceMs of its bar's end finished
    uint32 MinCasts = 3;
    uint32 EndToleranceMs = 150;
};

struct ReactionParams
{
    // Median reaction time (ms) and log-space spread at skill 1 and 99 (lerped along Playerbots.Skill.CurveExponent)
    float MedianMsAtSkill1 = 690.0f;
    float MedianMsAtSkill99 = 220.0f;
    float SigmaAtSkill1 = 0.45f;
    float SigmaAtSkill99 = 0.25f;
    uint32 MinMs = 150;
    // Added once per event: target outside the view cone, and a change of focus within FocusTimeoutMs
    uint32 BehindMinMs = 150;
    uint32 BehindMaxMs = 400;
    uint32 SwitchMinMs = 100;
    uint32 SwitchMaxMs = 300;
    float ConeDegrees = 200.0f;
    uint32 FocusTimeoutMs = 2000;
    // End to end: PingMs is the modelled round trip (half to see the event, half for the answer to reach the server),
    // DisplayMs the client's frame and display delay, ServerTickMs the server ticks (-1: half the bot's map update)
    uint32 PingMs = 60;
    uint32 DisplayMs = 30;
    int32 ServerTickMs = -1;
    // G0h3's parts
    ReactionChoiceParams Choice;
    ReactionLoadParams Load;
    ReactionEaseParams Ease;
    ReactionThinkParams Think;
    ReactionAnticipateParams Anticipate;
    ReactionQueueParams Queue;
    ReactionReevalParams Reeval;
    ReactionMemoryParams Memory;
    // The Positioning category (G2e, G2f): regain los, break los and pillar step as reactions (a reaction only, like
    // Movement: no notice counter, no D4 delay)
    bool PositioningEnable = false;
    // The contexts the model acts in (Arena,Battleground,World,Pve, All, None); ContextsText is the key's text,
    // Contexts its mask (arena and battleground only by default: the model's arm cost measurably more per tick, G0h2-b)
    std::string ContextsText = "Arena,Battleground";
    uint8 Contexts =
        static_cast<uint8>(TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground));
};

template <typename Struct>
struct ConfigFamily;

// The config table families (NaturalReactionConfig.cpp): the base keys, then one family per part
extern ConfigFamily<ReactionParams> const ReactionFamily;
extern ConfigFamily<ReactionChoiceParams> const ReactionChoiceFamily;
extern ConfigFamily<ReactionLoadParams> const ReactionLoadFamily;
extern ConfigFamily<ReactionEaseParams> const ReactionEaseFamily;
extern ConfigFamily<ReactionThinkParams> const ReactionThinkFamily;
extern ConfigFamily<ReactionAnticipateParams> const ReactionAnticipateFamily;
extern ConfigFamily<ReactionQueueParams> const ReactionQueueFamily;
extern ConfigFamily<ReactionReevalParams> const ReactionReevalFamily;
extern ConfigFamily<ReactionMemoryParams> const ReactionMemoryFamily;

// Upstream's stagger of a bot's next AI check (PR #2250): 0-200 ms from the guid
constexpr uint32 UPSTREAM_CHECK_STAGGER_MS = 200;

// The cadence of the natural reaction (Playerbots.Natural.Cadence.*, PlayerbotAIConfig::NaturalCadence): a faster AI
// check and a smaller stagger for bots in its Contexts, in combat in a started match
struct CadenceParams
{
    // The check interval and the stagger of the next check while the cadence is active (stagger at most the upstream's)
    uint32 CheckMs = 100;
    uint32 StaggerMs = 20;
    std::string ContextsText = "Arena,Battleground";
    uint8 Contexts =
        static_cast<uint8>(TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground));
    // G6d-a: the skill's stretch (Skill.React*) stays where the cadence and the reaction model both run, so it scales
    // the cadence's check interval (0: factor 1 there, the reaction model carries the skill)
    bool SkillStretch = false;
};

// The config table family (NaturalReactionConfig.cpp)
extern ConfigFamily<CadenceParams> const CadenceFamily;

#endif
