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

#include "CombatTallyState.h"
#include "Define.h"

class Action;
class Player;
class PlayerbotAI;

// The fight tally's edges (combat-foundation CF1, CFM2): the core's combat edges in WorldLogHooks.cpp, beside the
// bots' record. Bots only; a fight opens only while the world log is on, and its close writes the Fight pair.
namespace CombatTally
{
void OnEnter(Player* player);
void OnLeave(Player* player, bool logout);
// The bot's update while its fight is open (PlayerbotAI::UpdateAI, CF.32): once the part has lasted SPLIT_MS, the
// running pair, and the next part starts unticked with this update's health and power as its pull
void OnUpdate(PlayerbotAI* botAI);
// The pair Fight (version 1) + FightMore of a closed fight or a running part (outcome "running"); FightMore's bit 30
// marks a part the combat engine never ticked in
void Write(Player* player, Summary const& fight, char const* outcome);
// The executed action is a spell cast (any CastSpellAction; melee and attack are not)
bool IsCast(Action* action);
// The executed action is a move (any MovementAction)
bool IsMove(Action* action);
}  // namespace CombatTally

// The detail trace of a casttrace bot (TestConstraints::CombatTraced; tests only): world-log decision rows
// combat_action, combat_reject and combat_known, and for a spec table combat_table and combat_rows
namespace CombatTrace
{
// The words of combat_reject's reason, in this order (MEASURES §2.3)
enum class RejectReason : uint8
{
    Unknown,  // no action object for the node
    Useless,
    Vetoed,    // a multiplier took the relevance to 0 or below
    NotKnown,  // the cast test: the spell is not known
    Cooldown,
    Range,
    Sight,
    Target,
    Impossible,  // any other failed test
    Failed,
    NoPower,   // the execute failed with SPELL_FAILED_NO_POWER
    PowerGate  // a table row's power term was unmet (choice = the row's action)
};

// The cast test's refusal as a reject reason (no refusal: impossible)
RejectReason ReasonOf(CombatTally::CastRefusal refusal);

constexpr std::size_t REJECTS_PER_TICK = 5;       // an idle, waiting or out-of-reach tick
constexpr std::size_t REJECTS_BEFORE_ACTION = 3;  // a tick that acted: the abilities passed over

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
// The tick's unmet table power gate as a power_gate reject (before the candidates)
void AddPowerGate(PlayerbotAI* botAI, Rejects& rejects);
// combat_reject: the first limit rejects of a tick, each pair of action and reason the first time in a fight and then
// once every CombatRecord::REJECT_REPEAT_MS while it keeps meeting
void Rejected(PlayerbotAI* botAI, Rejects const& rejects, std::size_t limit, uint32 nowMs);
// combat_known: the bot's active class abilities, highest ranks only; sets the bot's combatKnownTraced
void Known(PlayerbotAI* botAI);
// combat_table: the table's rows and standing state, at the first traced tick (first) and after a change
void Table(PlayerbotAI* botAI, bool first);
// combat_rows: at a fight's end, per table row its ticks live and times executed (write: the part lasted 0.1 s, CF.30);
// clears the counts
void Rows(PlayerbotAI* botAI, bool write = true);
// combat_offengine (CF.31, MEASURES §1): a casttrace bot's open fight while its state is not BOT_STATE_COMBAT.
// open: the enter edge when the tally newly opens (fields only, no AI value: the edge can run in the core's damage
// call); still: the record's sample clock, also the size of the attackers value (Get()). choice: the state's word,
// value: the core's combat references, numbers: attackers (still), mounted, moving.
void OffEngineOpen(PlayerbotAI* botAI);
void OffEngineStill(PlayerbotAI* botAI);
// A tick of the current engine (Engine::DoNextAction, the flag read before its actions run): ran, an action that ran
// (ref: the spell of a cast); a candidate rejected before it with combat_reject's words. Each pair of action and reason
// the first time in the fight, then once every CombatRecord::REJECT_REPEAT_MS. Tests nothing a second time.
void OffEngineRan(PlayerbotAI* botAI, Action* action, float relevance);
void OffEngineRejected(PlayerbotAI* botAI, Action* action, std::string const& name, RejectReason reason,
                       float relevance, std::string const& veto = {}, float vetoValue = 0.0f);
}  // namespace CombatTrace

#endif
