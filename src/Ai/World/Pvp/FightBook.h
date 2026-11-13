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

// World PvP's records (WP1): the fight book of each open-world map (FightBookRules.h holds its rules), the bots'
// fight memories and meetings, the fight events and the `playerbots fight` commands. Records only: nothing here
// changes whom a bot attacks. Persistent bots in the open world; battlegrounds, arenas and dungeons have no book.
namespace WorldPvp
{
// NearestEnemyPlayersValue::AcceptUnit, the bot's map thread: the enemy player when `unit` is one this persistent bot
// meets now (in sight, not seen in the last MeetingMemorySeconds; remembered from now on), else nullptr. Cheap for
// anything but enemy players
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
