/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COMBATTRACE_H
#define _PLAYERBOT_COMBATTRACE_H

#include <array>
#include <cstddef>
#include <string>

#include "Define.h"

class Action;
class Player;
class PlayerbotAI;

// The fight tally's edges (combat-foundation CF1): the core's combat edges in WorldLogHooks.cpp, beside the bots'
// record. Bots only; a fight opens only while the world log is on, and its close writes the Fight row.
namespace CombatTally
{
void OnEnter(Player* player);
void OnLeave(Player* player, bool logout);
// The executed action is a spell cast (any CastSpellAction; melee and attack are not)
bool IsCast(Action* action);
}  // namespace CombatTally

// The detail trace of a casttrace bot (TestConstraints::CombatTraced; tests only): world-log decision rows
// combat_action, combat_reject and combat_known, and for a spec table combat_table and combat_rows
namespace CombatTrace
{
enum class RejectReason : uint8
{
    Unknown,  // no action object for the node
    Useless,
    Vetoed,  // a multiplier took the relevance to 0 or below
    Impossible,
    Failed,
    NoPower  // the execute failed with SPELL_FAILED_NO_POWER
};

constexpr std::size_t REJECTS_PER_TICK = 5;

struct Reject
{
    std::string name;
    std::string veto;  // vetoed: the multiplier's name
    float relevance = 0.0f;
    float vetoValue = 0.0f;
    uint32 spell = 0;
    RejectReason reason = RejectReason::Unknown;
};

// The first REJECTS_PER_TICK rejected candidates of one tick, in queue order (names copied: the node goes)
struct Rejects
{
    std::array<Reject, REJECTS_PER_TICK> rows;
    std::size_t count = 0;

    void Add(PlayerbotAI* botAI, Action* action, std::string const& name, RejectReason reason, float relevance,
             std::string const& veto = {}, float vetoValue = 0.0f);
};

// The spell id of a CastSpellAction ("spell id" of its spell), else 0
uint32 SpellOf(PlayerbotAI* botAI, Action* action);
// combat_action: an executed action, its target's kind after the execute
void Executed(PlayerbotAI* botAI, Action* action, float relevance);
// combat_reject: the rejects of an idle or no-power tick
void Rejected(PlayerbotAI* botAI, Rejects const& rejects);
// combat_known: the bot's active class abilities, highest ranks only; sets the bot's combatKnownTraced
void Known(PlayerbotAI* botAI);
// combat_table: the table's rows and standing state, at the first traced tick (first) and after a change
void Table(PlayerbotAI* botAI, bool first);
// combat_rows: at a fight's end, per table row its ticks live and times executed; clears the counts
void Rows(PlayerbotAI* botAI);
}  // namespace CombatTrace

#endif
