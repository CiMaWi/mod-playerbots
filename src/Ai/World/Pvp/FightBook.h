/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FIGHTBOOK_H
#define _PLAYERBOT_FIGHTBOOK_H

#include "ObjectGuid.h"

class ChatHandler;
class Corpse;
class Player;
class PlayerbotAI;
class Unit;

// World PvP (WP1, WP4-WP6): the fight book of each open-world map (FightBookRules.h holds its rules), the bots' fight
// memories and meetings, the start, the joining and the end of fights by the fight rules (FightRules.h, fed by
// FightWiring.h: retreats, chases, timeouts, calm areas), the fight events and the `playerbots fight` commands.
// Persistent bots in the open world; battlegrounds, arenas and dungeons have no book and keep today's attack on sight.
namespace WorldPvp
{
// The bot's map thread (cheap): whether this bot starts fights by the rules: a persistent bot on an open-world map
// outside battlegrounds and arenas that PvpTest does not force. Such a bot attacks another player only in a fight it
// is in or as the target the rules picked at a meeting
bool StartsByRules(PlayerbotAI* botAI);
// EnemyPlayerValue::Calculate: true when a bot that starts by the rules cannot start a fight now (StartGate: no open
// world, a real player as its master or in its group, not its bot group's leader, in a fight, near a graveyard,
// peaceable, in its gap), so it skips the enemy-player scan; attackers and fights still reach it
bool ScanGated(PlayerbotAI* botAI);
// NearestEnemyPlayersValue::AcceptUnit for a bot that starts by the rules: whether the enemy player `unit` is the
// target the rules picked. A new meeting (within ScanYards, in sight and in reach, not seen for MeetingMemorySeconds)
// while the
// gate is open is decided here once (DecideStart; its fight_start row); `refusal` is the word of today's target check
// that failed on the enemy, nullptr when it is an attackable, PvP-flagged enemy the bot can see
bool AcceptByRules(PlayerbotAI* botAI, Unit* unit, char const* refusal);
// EnemyPlayerValue and AcceptByRules: whether `bot` can attack `enemy` from where it is (InReach: the height between
// them, then its line of sight)
bool Reaches(Unit const* bot, Unit const* enemy);
// PossibleTargetsValue::AcceptUnit: whether `unit` is the player the rules picked for this bot to attack, at a meeting
// or as the target of a fight it decided to join
bool IsPickedTarget(PlayerbotAI* botAI, Unit* unit);
// EnemyPlayerValue::Calculate: the enemy of the fight this bot decided to join (the fight book's tick asks the
// bystanders of its fights once each, WP5), while fresh, alive, on its map and in the fight; when it is gone and the
// bot is in that fight, the nearest enemy participant it sees and may pick. Never in a fight it retreated from;
// else nullptr
Player* JoinTarget(PlayerbotAI* botAI);
// InvalidTargetValue ("current target"): whether this bot stops chasing `target`, a retreated enemy of its fight it
// attacks: the chase's time since the enemy's retreat or its yards beyond the fight passed the bot's limit
// (ChaseLimitFor: aggression lengthens it, courtesy shortens it), also when the enemy hits back (it fights only who
// attacks it). O(1) unless `target` is the chased one
bool ChaseEnds(PlayerbotAI* botAI, Unit* target);
// EnemyPlayerValue::Calculate and InvalidTargetValue ("current target"): whether this bot, starting by the rules,
// leaves `unit` alone (neither picks nor keeps it as its target): it retreated from the fight it is in, it disengages,
// or its chase of `unit` just ended; never an enemy attacking it (self-defence). A player's pet counts as its owner's
// side: leaving the player alone leaves its pets alone unless they attack the bot
bool LeftAlone(PlayerbotAI* botAI, Unit* unit);
// DropTargetAction: whether this bot, dropping `target` because it leaves it alone or its chase of it ended, calls its
// pet off too (the pet attacks `target`'s side). O(1)
bool CallsPetOff(PlayerbotAI* botAI, Unit* target);
// The "world pvp retreat" trigger: whether this bot retreats (or disengages) by the rules now; (x, y): the place its
// fight memory keeps for it (the flight itself, FleeAttackersAction, runs from its attackers)
bool RetreatFrom(PlayerbotAI* botAI, float& x, float& y);

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
// by its next tick); `fight ready` is the test constraint fightready (PlayerbotCommandScript.cpp)
bool HandleListCommand(ChatHandler* handler);
bool HandleBotCommand(ChatHandler* handler, PlayerbotAI* botAI);
}  // namespace WorldPvp

void AddFightBookScripts();

#endif
