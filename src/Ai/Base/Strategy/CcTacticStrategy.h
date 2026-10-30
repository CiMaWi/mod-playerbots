/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CCTACTICSTRATEGY_H
#define PLAYERBOTS_CCTACTICSTRATEGY_H

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Action.h"
#include "ArenaCc.h"
#include "Multiplier.h"
#include "ObjectGuid.h"
#include "Strategy.h"
#include "Trigger.h"
#include "Value.h"

class PlayerbotAI;
class Unit;
struct ItemTemplate;

// The Cc tactic in play (G4b, PLAN 3.15; the policy in ArenaCc.h, per-bot state in CcState.h). Added to every combat
// engine while Playerbots.Tactics.Enable is on and the Cc or OwnCc tactic is enabled, or the PvP harness is on
// (AiFactory); each part tests its tactic at run time (the harness forces tactic masks per side), so with the tactic
// off nothing changes and the old "cc" strategies act as before.

// Mutes the old "cc" strategies' actions (polymorph and the "* on cc" ones) where the Cc tactic acts: enabled here
// (mask and scope) and known at the bot's skill. Two systems on one bot double-cast and fight over DR (DT.3). Named
// "pvp cc" so that the engine (strategies by name) asks it before the "skill" multiplier: a muted action does not
// start a notice or reaction event.
class TacticCcMultiplier : public Multiplier
{
public:
    TacticCcMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tactic cc") {}

    float GetValue(Action* action) override;

private:
    bool IsOldCcAction(Action* action);

    // Per bot: the actions of a bot's context live as long as the bot, so the pointer is a stable key
    std::unordered_map<Action*, bool> oldCcActions;
};

class PvpCcStrategy : public Strategy
{
public:
    PvpCcStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "pvp cc"; }
};

// "cc ready" -> "team cc": the bot's CC (PickCc over its known, ready rows) on the team's CC target: the moon in an
// arena; elsewhere, or in an arena without a moon, the strongest enemy of "fight enemies" other than the bot's own
// target and the skull
class TacticCcStrategy : public Strategy
{
public:
    TacticCcStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "tactic cc"; }
    // OwnCc (G4b-b): the team's breakable-CC'd enemies (and a forced one) stay out of the dps, debuff and AoE targets
    // in every context (scope All); the list is cached per bot (CcBotState::Excluded), so a call is a time compare
    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;
};

// The action name (Action::getName()) is one of the old "cc" strategies' actions that the Cc tactic mutes;
// crowdControlAction: the action is a CastCrowdControlSpellAction (named by its spell), else the hunter's two (named
// "scare beast on cc" and "freezing trap"). Pure, for the selftest.
bool CcOldActionName(std::string_view name, bool crowdControlAction);

// The Cc tactic is active (gate), the bot is not casting, a row is ready, the CC target is alive, not immune and not
// controlled (a chaining bot, G4c: or held by its team's CC, the chain due), the patient hold and the low-effort stop
// let it, PickCc has a row and the target is in sight. The answer is cached for CC_READY_REFRESH_MS (CcBotState;
// shorter when a chain is due sooner), so a tick costs one flag test and a time compare.
class CcReadyTrigger : public Trigger
{
public:
    CcReadyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cc ready") {}

    bool IsActive() override;
};

// Casts the picked row's highest known rank on the trigger's target (a CrowdControl action of the skill strategy's
// table: the notice and reaction model applies, DT.11)
class TeamCcAction : public Action
{
public:
    TeamCcAction(PlayerbotAI* botAI) : Action(botAI, "team cc") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
    Unit* GetTarget() override;
};

// OwnCc: the bot's current target is held in a team breakable CC (or forced), the bot's own-break window aside
class OwnCcTargetTrigger : public Trigger
{
public:
    OwnCcTargetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "own cc target") {}

    bool IsActive() override;
};

// Drops that target (a cast or channel at it, the swing, the pet's attack) so the next target pick, which leaves it
// out, takes another (a TargetSwitch of the skill strategy's table: the notice and reaction model applies, DT.11)
class CcRetargetAction : public Action
{
public:
    CcRetargetAction(PlayerbotAI* botAI) : Action(botAI, "cc retarget") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// The Trinket tactic (G5a, PLAN 3.15): "tactic trinket" ("cc matters" -> "pvp trinket" and the racials) and "pvp
// trinket" (the multiplier below) go into every combat engine while the Trinket tactic is enabled or the harness is
// on (AiFactory); "tactic dispel" ("dispel cc on party") into the cleansers' (priest, paladin, shaman) while the
// Dispel tactic is. Each part tests its tactic at run time: with it off nothing changes.

// Mutes the racial breaks' old triggers (loss of control, fear charm sleep, movement impaired) where the Trinket
// tactic acts (enabled here, known at the bot's skill): a racial then fires only as cc matters' answer. Named "pvp
// trinket" so that the engine asks it before the "skill" multiplier (a muted action starts no notice or reaction).
class TacticTrinketMultiplier : public Multiplier
{
public:
    TacticTrinketMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "tactic trinket") {}

    float GetValue(Action* action) override;

private:
    CcBreaker RacialOf(Action* action);

    // Per bot, as TacticCcMultiplier's
    std::unordered_map<Action*, CcBreaker> racialActions;
};

class PvpTrinketStrategy : public Strategy
{
public:
    PvpTrinketStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "pvp trinket"; }
};

// "cc matters" -> "pvp trinket" (ACTION_EMERGENCY + 2) and the racials the bot knows (+ 1)
class TacticTrinketStrategy : public Strategy
{
public:
    TacticTrinketStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "tactic trinket"; }
};

// The Trinket tactic is active (gate), a CC is on the bot, ShouldBreakCc says break (it matters: MinLeftMs or more
// left while the bot or its partner is at or below its low-health line; else the wasted-break roll of low skill) and
// a breaker that frees its mechanics is ready (CcBreakAnswer, cached CC_BREAK_REFRESH_MS)
class CcMattersTrigger : public Trigger
{
public:
    CcMattersTrigger(PlayerbotAI* botAI) : Trigger(botAI, "cc matters") {}

    bool IsActive() override;
};

// Uses the worn PvP trinket (an item with spell 42292; a CrowdControlBreak of the skill strategy's table: the notice
// and reaction model applies, DT.11); the harness counts cc.trinket
class PvpTrinketAction : public Action
{
public:
    PvpTrinketAction(PlayerbotAI* botAI) : Action(botAI, "pvp trinket") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// "dispel cc on party" -> "dispel cc on party" (ACTION_EMERGENCY: above the class cleanses and the heals)
class TacticDispelStrategy : public Strategy
{
public:
    TacticDispelStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "tactic dispel"; }
};

// The Dispel tactic is active and "party member cc to dispel" has a member: self first, then the partners (a loss
// of control before a root), a CC the bot's class removes, in range and sight (CcDispelPick, cached)
class DispelCcOnPartyTrigger : public Trigger
{
public:
    DispelCcOnPartyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dispel cc on party") {}

    bool IsActive() override;
};

// Casts the class's spell for the CC's dispel type on that member (a Dispel of the skill strategy's table); the
// harness counts cc.partnerDispel and tactics.partnerDispelMs for a partner
class DispelCcOnPartyAction : public Action
{
public:
    DispelCcOnPartyAction(PlayerbotAI* botAI) : Action(botAI, "dispel cc on party") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    Unit* GetTarget() override;
};

// The member dispel cc on party frees (CcDispelPick's cached answer)
class PartyMemberCcToDispelValue : public UnitCalculatedValue
{
public:
    PartyMemberCcToDispelValue(PlayerbotAI* botAI) : UnitCalculatedValue(botAI, "party member cc to dispel") {}

    Unit* Calculate() override;
};

// The bot's answer to the CC on it (None outside the Trinket tactic; cached CC_BREAK_REFRESH_MS)
CcBreaker CcBreakAnswer(PlayerbotAI* botAI);

// The member whose CC the bot dispels now (nullptr outside the Dispel tactic; cached CC_DISPEL_REFRESH_MS)
Unit* CcDispelPick(PlayerbotAI* botAI);

// The item is a PvP trinket (one of its spells is 42292)
bool CcIsPvpTrinket(ItemTemplate const* proto);

// A break cast of the bot (the trinket's or a racial's spell; PlayerbotAI::CountPvpTestCast while counting): the
// harness's tactics.breakWasted and breakMs, the state's counts and the "CC break:" trace line
void CcNoteBreakCast(PlayerbotAI* botAI, uint32 spellId);

// Avoid.EnsureTrinket at a harness fight start (world thread, after PvP gear's wear at the arena entry): 1 gives a
// bot without a PvP trinket one (true: given; its equip cooldown cleared), 2 withholds it. The given item is marked
// (its creator the bot, its gift creator the item it replaced) for CcCleanEnsuredTrinket. CcRestoreTrinket undoes it
// when counting stops, at the battleground leave and before the logout's save; a dead bot keeps it until it is alive
// (an item equipped by a dead player misses its stats)
bool CcEnsureTrinket(PlayerbotAI* botAI, uint32 mode);
void CcRestoreTrinket(PlayerbotAI* botAI);

// The bot's login (before PvP gear's repair): a marked trinket left by a crash or a save before the restore goes, the
// item it replaced back into its slot
void CcCleanEnsuredTrinket(PlayerbotAI* botAI);

// The OwnCc list of the bot (refreshed at most every CC_EXCLUDE_REFRESH_MS; empty with OwnCc not enabled here)
std::vector<ObjectGuid> const& CcOwnExclusions(PlayerbotAI* botAI);

// The target is in that list: the bot keeps out of it (the hold lets the bot leave it as it leaves an immune one)
bool CcTargetKeptOut(PlayerbotAI* botAI, Unit const* target);

// cc.ownBreaks: the bot's (or its pet's) damage hit an enemy in a breakable CC of its team (the harness counts it)
void CcCountOwnBreak(Unit* attacker, Unit* victim);

// tactics force dr (G4c): while on, a team CC landing is not counted as a chain (the forced casts land at once, one
// after another); set on the world thread while the maps wait
void CcSetForcedCasts(bool on);

// The DR memory's feed (unit hooks: a CC aura landing on and leaving a player or a player's pet), registered by
// Playerbots.cpp
void AddCcTacticScripts();

#endif
