/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FIGHTBOOK_H
#define _PLAYERBOT_FIGHTBOOK_H

#include "ObjectGuid.h"

#include <string_view>

class ChatHandler;
class Corpse;
class Player;
class PlayerbotAI;
class Unit;
class WorldObject;

// World PvP (WP1, WP4-WP6, WP23, WP24): the fight book of each open-world map (FightBookRules.h holds its rules), the
// bots' fight memories and meetings, entering and leaving a fight by enthusiasm (FightEnthusiasm.cpp, Enthusiasm.h),
// the book's fight ends (rows only since WP24: they move no bot), the fight events and the `playerbots fight`
// commands.
// Persistent bots in the open world; battlegrounds, arenas and dungeons have no book and keep today's attack on sight.
namespace WorldPvp
{
// The bot's map thread (cheap): whether this bot starts fights by the rules: a persistent bot on an open-world map
// outside battlegrounds and arenas that PvpTest does not force. Such a bot attacks another player only in a fight it
// is in or as the target the rules picked at a meeting
bool StartsByRules(PlayerbotAI* botAI);
// EnemyPlayerValue: whether `bot` can attack `enemy` from where it is (InReach: the height between them, then its
// line of sight)
bool Reaches(Unit const* bot, Unit const* enemy);

// Entering by enthusiasm (WP23, FightEnthusiasm.cpp), the bot's map thread. EnemyPlayerValue::Calculate first, for a
// bot that starts by the rules: its enthusiasm (seeded at random on first use), the engaged edge, the traits and, at
// the scan's cadence while the bot could enter or is engaged, the local balance (PerfMonitor "WorldPvp balance")
void RefreshEnthusiasm(PlayerbotAI* botAI);
// EnemyPlayerValue::Calculate: true while the value can enter nothing (SkipsScan), so the bot skips the enemy-player
// scan; attackers still reach it by its combat references
bool ScanSkipped(PlayerbotAI* botAI);
// NearestEnemyPlayersValue::AcceptUnit for a bot that starts by the rules: its pick, or an alive enemy player within
// ScanYards it does not leave alone, not banned (Banned), whose join threshold and flee floor the value passes
// (Enters). `refusal`:
// today's target check that failed on the enemy, nullptr when it is attackable, PvP-flagged and seen
bool MayEnter(PlayerbotAI* botAI, Unit* unit, char const* refusal);
// EnemyPlayerValue::Calculate on the enemy it returns: a new pick writes its fight_start or fight_join row and holds
// for PICK_MS; false when the bot may not enter on it (any more)
bool NoteEntering(PlayerbotAI* botAI, Player* enemy);
// PossibleTargetsValue::AcceptUnit: whether `unit` is the enemy this bot entered on (its pick)
bool IsEnteredTarget(PlayerbotAI* botAI, Unit* unit);
// OnBotTick (every bot tick while in PvP combat or engaged): the engaged edge, while engaged the balance, the group
// hold and who attacks the bot (at the balance's cadence), then the leave check (WP24): an engaged bot below its flee
// threshold drops its target, calls its pet off and writes its fight_leave row, once per engaged episode
void NoteEngaged(PlayerbotAI* botAI, Player* bot, uint32 nowMs);
// The PvP-kill hooks, the killed player's map thread: the death edit of a bot victim, the kill edit of a bot killer
// and the group death edit of the victim's bot group members on its map within GroupDeathYards; a PvpEnthusiasm event
// row each
void NoteEnthusiasmDeath(Player* killer, Player* killed);
// `playerbots fight enthusiasm <bot> [set <0..max>]` (world thread, between map updates): the value, the thresholds
// and the cached terms, one line each; `set` writes the value
bool HandleEnthusiasmCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// The yards from `object` to the nearest graveyard (spirit healers stand at them) of its map
float GraveyardYards(WorldObject const* object);
// EnemyPlayerValue::Calculate, MayEnter and InvalidTargetValue ("current target"): whether this bot, starting by the
// rules, leaves `unit` alone (neither picks nor keeps it as its target): it left its fight by its enthusiasm (WP24)
// and neither `unit` nor `unit`'s owner player attacks it (victim or selected target: the bot or its pet); it answers
// whoever does. O(1) for a bot that has not left
bool LeftAlone(PlayerbotAI* botAI, Unit* unit);
// DropTargetAction: whether this bot, dropping `target` because it leaves it alone, calls its pet off too (the pet
// attacks `target`'s side). O(1)
bool CallsPetOff(PlayerbotAI* botAI, Unit* target);
// The "world pvp retreat" trigger (O(1), cached): the bot left its fight by its enthusiasm and an enemy player attacks
// it, so it flees (FleeAttackersAction runs from its attackers)
bool LeavesFlee(PlayerbotAI* botAI);
// The leave (FightEnthusiasm.cpp): the bot stops attacking, no victim, no current target, no selection
void DropFightTarget(PlayerbotAI* botAI, Player* bot);

struct GhostPlan;
// FindCorpseAction and ReviveFromCorpseAction, the ghost's map thread (PlanGhost in FightRules.h): a persistent ghost
// in the open world with an enemy player (alive, of the other team, flagged or not) within GhostEnemyYards of its
// corpse waits as a ghost, runs to its corpse once the enemy has gone and takes the spirit healer
// GhostSpiritHealerSeconds after its release (the corpse's ghost time: a recheck never resets it). One corpse search
// per recheck, the last answer in between; a GhostWait row when a wait starts and when it ends. Step Corpse for every
// other ghost (a game-client master, a battleground, no book, not persistent, its corpse on another map)
GhostPlan PlanGhostFor(PlayerbotAI* botAI, Corpse const* corpse);

// NearestEnemyPlayersValue::AcceptUnit for the other bots, the bot's map thread: the enemy player when `unit` is one
// this persistent bot (a PvpTest fighter) meets now (in sight, not seen in the last MeetingMemorySeconds; remembered
// from now on), else nullptr. Cheap for anything but enemy players
Player* NewMeeting(PlayerbotAI* botAI, Unit* unit);
// The meeting's fight_start decision row: refusal is the word of today's rule that passed on the enemy, nullptr when
// the scan took it as an attack-on-sight target
void RecordMeeting(PlayerbotAI* botAI, Player* enemy, char const* refusal);

// PlayerbotAI::UpdateAI calls OnBotTick while BotTickDue (a relaxed load and one empty() check): the bot's PvP combat
// for the book (at most once a second) and the commands' requests for this bot
bool RequestsWaiting();
void OnBotTick(PlayerbotAI* botAI, Player* bot);

// PvpTest (world thread or any): a forced fighter's fights are tagged pvptest (WP.6) until it is cleared
void MarkForced(ObjectGuid guid);
void ClearForced(ObjectGuid guid);

// `.playerbots fight ...` (world thread): list (the GM's map, or every map from the console), bot (its memory, answered
// by its next tick)
bool HandleListCommand(ChatHandler* handler);
bool HandleBotCommand(ChatHandler* handler, PlayerbotAI* botAI);
}  // namespace WorldPvp

void AddFightBookScripts();

#endif
