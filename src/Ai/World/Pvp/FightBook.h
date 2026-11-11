/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FIGHTBOOK_H
#define _PLAYERBOT_FIGHTBOOK_H

#include "ObjectGuid.h"

class ChatHandler;
class Player;
class PlayerbotAI;
class Unit;

// World PvP (WP1, WP4): the fight book of each open-world map (FightBookRules.h holds its rules), the bots' fight
// memories and meetings, the start of fights by the fight rules (FightRules.h, fed by FightWiring.h), the fight events
// and the `playerbots fight` commands. Persistent bots in the open world; battlegrounds, arenas and dungeons have no
// book and keep today's attack on sight.
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
// target the rules picked. A new meeting (within ScanYards, in sight, not seen for MeetingMemorySeconds) while the
// gate is open is decided here once (DecideStart; its fight_start row); `refusal` is the word of today's target check
// that failed on the enemy, nullptr when it is an attackable, PvP-flagged enemy the bot can see
bool AcceptByRules(PlayerbotAI* botAI, Unit* unit, char const* refusal);
// PossibleTargetsValue::AcceptUnit: whether `unit` is the player the rules picked for this bot to attack
bool IsPickedTarget(PlayerbotAI* botAI, Unit* unit);

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
// by its next tick), ready and meet (labs only: Playerbots.WorldPvp.TestCommands)
bool HandleListCommand(ChatHandler* handler);
bool HandleBotCommand(ChatHandler* handler, PlayerbotAI* botAI);
bool HandleReadyCommand(ChatHandler* handler, PlayerbotAI* botAI);
bool HandleMeetCommand(ChatHandler* handler, PlayerbotAI* botAI, PlayerbotAI* enemyAI);
}  // namespace WorldPvp

void AddFightBookScripts();

#endif
