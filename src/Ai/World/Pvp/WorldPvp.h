/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDPVP_H
#define _PLAYERBOT_WORLDPVP_H

#include <cstdint>
#include <string_view>

#include "ObjectGuid.h"

class ChatHandler;
class Player;
class PlayerbotAI;
class Unit;

// World PvP (WP.19, WP23, WP24): persistent bots in the open world enter and leave fights by their enthusiasm
// (FightEnthusiasm.cpp, its rules in FightEnthusiasm.h and Enthusiasm.h); there is no fight object. Battlegrounds,
// arenas, dungeons, other bots and PvpTest's forced fighters keep today's attack on sight. The ghost's wait is in
// GhostWait.h
namespace WorldPvp
{
// The bot's map thread (cheap, cached): whether this bot starts fights by the rules: a persistent bot on an
// open-world map outside battlegrounds and arenas that PvpTest does not force. Such a bot attacks another player only
// in PvP combat it is in or as the enemy it entered on by enthusiasm
bool StartsByRules(PlayerbotAI* botAI);
// EnemyPlayerValue: whether `bot` can attack `enemy` from where it is (InReach: the height between them, then its
// line of sight)
bool Reaches(Unit const* bot, Unit const* enemy);

// EnemyPlayerValue::Calculate first, for a bot that starts by the rules: its enthusiasm (seeded at random on first
// use), the engaged edge, the traits and, at the scan's cadence while the bot could enter or is engaged, the local
// balance (PerfMonitor "WorldPvp balance")
void RefreshEnthusiasm(PlayerbotAI* botAI);
// EnemyPlayerValue::Calculate: true while the value can enter nothing (SkipsScan), so the bot skips the enemy-player
// scan; attackers still reach it by its combat references
bool ScanSkipped(PlayerbotAI* botAI);
// NearestEnemyPlayersValue::AcceptUnit for a bot that starts by the rules: its pick, or an alive enemy player within
// ScanYards it does not leave alone, not banned (Banned), whose join threshold and flee floor the value passes
// (Enters). `refusal`: today's target check that failed on the enemy, nullptr when it is attackable, PvP-flagged and
// seen
bool MayEnter(PlayerbotAI* botAI, Unit* unit, char const* refusal);
// EnemyPlayerValue::Calculate on the enemy it returns: a new pick writes its fight_start or fight_join row and holds
// for PICK_MS; false when the bot may not enter on it (any more)
bool NoteEntering(PlayerbotAI* botAI, Player* enemy);
// PossibleTargetsValue::AcceptUnit: whether `unit` is the enemy this bot entered on (its pick)
bool IsEnteredTarget(PlayerbotAI* botAI, Unit* unit);
// PlayerbotAI::UpdateAI, every bot tick while in PvP combat or engaged: the engaged edge (a PvpEnthusiasm engaged
// row on the rising one), while engaged the balance, the group hold and who attacks the bot (at the balance's
// cadence), then the leave check (WP24): an engaged bot below its flee threshold drops its target, calls its pet off
// and writes its fight_leave row, once per engaged episode
void NoteEngaged(PlayerbotAI* botAI, Player* bot, uint32_t nowMs);
// `playerbots fight enthusiasm <bot> [set <0..max>]` (world thread, between map updates): the value, the thresholds
// and the cached terms, one line each; `set` writes the value
bool HandleEnthusiasmCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
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

// PvpTest (world thread or any): a forced fighter keeps today's attack on sight and its rows are tagged pvptest
// (WP.6) until it is cleared or FORCED_TTL_MS after its last aim
void MarkForced(ObjectGuid guid);
void ClearForced(ObjectGuid guid);
}  // namespace WorldPvp

// The PvP-kill hooks (the killed player's map thread): the death edit of a bot victim, the kill edit of a bot killer
// and the group death edit of the victim's bot group members on its map; a PvpEnthusiasm row each
void AddWorldPvpScripts();

#endif
