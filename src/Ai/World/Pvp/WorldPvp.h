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
#include "WorldPvpConfig.h"

class ChatHandler;
class Player;
class PlayerbotAI;
class Unit;

namespace WorldPvp
{
struct GhostValues;
}

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

// ScanSkipped, MayEnter and NoteEntering are a rule bot's reads of its enthusiasm and refresh it first: made and
// seeded at random on first use, the engaged edge, the traits and, at the scan's cadence while the bot could enter or
// is engaged, the local balance (PerfMonitor "WorldPvp balance")
// EnemyPlayerValue::Calculate: true while the value can enter nothing (SkipsScan), so the bot skips the enemy-player
// scan; attackers still reach it by its combat references
bool ScanSkipped(PlayerbotAI* botAI);
// NearestEnemyPlayersValue::AcceptUnit for a bot that starts by the rules: its pick, or an alive enemy player within
// ScanYards it does not leave alone, not banned (Banned), whose join threshold and flee floor the value passes
// (Enters). `refusal`: today's target check that failed on the enemy, nullptr when it is attackable, PvP-flagged and
// seen. A read: no pick, no row, no cost (WP39)
bool MayEnter(PlayerbotAI* botAI, Unit* unit, char const* refusal);
// AttackEnemyPlayerAction, after every refusal of the attack, on an enemy the bot is not in combat with: a new pick
// writes its fight_start or fight_join row, in the start-cost share a start charges its cost, and the pick holds for
// PICK_MS; false when the bot may not enter on it (any more): no attack
bool NoteEntering(PlayerbotAI* botAI, Player* enemy);
// PossibleTargetsValue::AcceptUnit: whether `unit` is the enemy this bot entered on (its pick)
bool IsEnteredTarget(PlayerbotAI* botAI, Unit* unit);
// PlayerbotAI::UpdateAI, every bot tick while in PvP combat or engaged: the engaged edge (a PvpEnthusiasm engaged
// row on the rising one), while engaged the balance, the group hold and who attacks the bot (at the balance's
// cadence), then the leave check (WP24, WP38a): an engaged bot below its flee threshold drops its target, sets its pet
// passive until the disengaged edge and writes its fight_leave row, once per engaged episode
void NoteEngaged(PlayerbotAI* botAI, Player* bot, uint32_t nowMs);
// `playerbots fight enthusiasm <bot> [set <0..max>]` (world thread, between map updates): the value, the thresholds
// and the cached terms, one line each; `set` writes the value
bool HandleEnthusiasmCommand(ChatHandler* handler, PlayerbotAI* botAI, std::string_view rest);
// Where a player target comes from, PossibleTargetsValue::AcceptUnit (the target list) and EnemyPlayerValue (the PvP
// combat references), and where it is kept, InvalidTargetValue ("current target"): whether this bot, starting by the
// rules, leaves `unit` alone (neither picks nor keeps it as its target): it left its fight by its enthusiasm (WP24) and
// neither `unit` nor `unit`'s owner player attacks it (victim or selected target: the bot or its pet); it answers
// whoever does. O(1) for a bot that has not left
bool LeftAlone(PlayerbotAI* botAI, Unit* unit);
// The "world pvp retreat" trigger (O(1), cached): the bot left its fight by its enthusiasm and an enemy player attacks
// it, so it flees (FleeAttackersAction runs from its attackers)
bool LeavesFlee(PlayerbotAI* botAI);

// The enthusiasm and ghost values, built once from the config at each config load (PlayerbotAIConfig::
// LoadWorldPvpConfig: startup and `.reload config`, the world thread) and swapped in whole; a map thread reads the
// ones published last. PvpTest's fighters are not here: they carry the constraint row onsight (WP.6)
void PublishConfigValues(WorldPvpConfig const& config);
GhostValues const& GhostConfigValues();
}  // namespace WorldPvp

// The PvP-kill hooks (the killed player's map thread): the death edit of a bot victim, the kill edit of a bot killer
// and the group death edit of the victim's bot group members on its map; a PvpEnthusiasm row each
void AddWorldPvpScripts();

#endif
