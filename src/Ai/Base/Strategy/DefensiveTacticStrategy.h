/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_DEFENSIVETACTICSTRATEGY_H
#define _PLAYERBOT_DEFENSIVETACTICSTRATEGY_H

#include <string>

#include "Action.h"
#include "Strategy.h"
#include "Trigger.h"

class Player;
class PlayerbotAI;
class Spell;
class Unit;

// The Defensive tactic (G5d, PLAN 3.15; policy in ArenaDefensive.h, code in CcTacticStrategy.cpp): "tactic
// defensive" ("burst" -> "self defensive" at ACTION_EMERGENCY - 5, below the classes' critical-health defensives;
// "burst potion" -> healthstone and healing potion just below) in every context with the Defensive tactic enabled
// or the harness on (AiFactory); the triggers test the tactic (mask, scope, gate) at run time. The old
// health-triggered defensives stay as they are.
class TacticDefensiveStrategy : public Strategy
{
public:
    TacticDefensiveStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "tactic defensive"; }
};

// The Defensive tactic is active and the bot's health, burst sampler and focus say press (SelfDefensiveReason, late
// by effort, MinGapMs after the last self-defensive); "burst potion": the same reason at or below PotionHpPct
class BurstTrigger : public Trigger
{
public:
    BurstTrigger(PlayerbotAI* botAI, bool potion) : Trigger(botAI, potion ? "burst potion" : "burst"), potion(potion) {}

    bool IsActive() override;

private:
    bool potion;
};

// The first of the class's self-defensives (ice block, divine shield, cloak of shadows and evasion, icebound
// fortitude, deterrence, barkskin and survival instincts, pain suppression or dispersion, Protection's shield wall)
// that is known and castable; none while one of the bot's defensives is up
class SelfDefensiveAction : public Action
{
public:
    SelfDefensiveAction(PlayerbotAI* botAI) : Action(botAI, "self defensive") {}

    bool isUseful() override;
    bool Execute(Event event) override;
};

// A defensive aura or an immunity a player sees on the unit's buff bar (ArenaDefensive.h's lists)
bool DefensiveAuraUp(Unit const* unit);
// At the bot's checks (PlayerbotAI::UpdateAI) with the Defensive tactic on here or the harness counting: the health
// sampler, the enemies focusing the bot, the match's opener and tactics.deathsDefReady
void FollowDefensive(PlayerbotAI* botAI, Player* bot, uint32 nowMs);
// ObserveOpponents: an enemy seen with a defensive up goes into the opponent memory with its health
void ObserveEnemyDefensive(PlayerbotAI* botAI, Unit* enemy, uint32 nowMs);
// PlayerScript::OnPlayerSpellCast (the caster's map thread): a bot's self or partner defensive is counted
// (tactics.defSelf, tactics.defPartner) and starts the self-defensive gap
void NoteDefensiveCast(Player* caster, Spell* spell);
// BoostTrigger and GenericBoostTrigger on an enemy player: the burst waits (DecideBurstHold) where the Defensive
// tactic is on here and the bot's Foresight knows the memory
bool DefensiveHoldsBurst(PlayerbotAI* botAI, Unit* target);
// PartyMemberToProtect: the partner to protect where the Protect tactic is active (PvP only, DT.3), else nullptr
Unit* DefensivePartnerToProtect(PlayerbotAI* botAI);

#endif
