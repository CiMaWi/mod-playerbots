/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NATURALREACTIONCONFIG_H
#define _PLAYERBOT_NATURALREACTIONCONFIG_H

#include <array>

#include "Define.h"

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
};

// Upstream's stagger of a bot's next AI check (PR #2250): 0-200 ms from the guid
constexpr uint32 UPSTREAM_CHECK_STAGGER_MS = 200;

#endif
