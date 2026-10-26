/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MATCHLOG_H
#define _PLAYERBOT_MATCHLOG_H

#include <string>

#include "Common.h"
#include "SharedDefines.h"

class ArenaTeam;
class Battleground;
class ChatHandler;
class Player;
struct PvpTestMatchRecord;

// Playerbots.MatchLog.Enable: every arena match that ends on this server writes exactly one JSON record (the C2
// schema 1 of PvpTestRecord, additive) to <LogsDir>/pvptest/matches-<date>.jsonl, whoever played it: organic bot
// matches, harness matches (the harness job owns its record, MatchLog hands it the extra fields) and matches with a
// real player. Ids are <server>-<ts>-<n>. The arena hooks run on the map thread of the arena instance; the captures
// of the running matches sit in one mutex-guarded registry keyed by the battleground instance id, with plain values
// only (never a Player* or Battleground* across calls). The file I/O is the writer thread's.
class MatchLog
{
public:
    // Map thread, when the arena gates open: rosters, ratings before, gear score, counting on for the bots
    static void OnArenaStart(Battleground* bg);
    // Map thread, before the core updates a member's rating: remembers the matchmaker rating change
    static void OnMemberUpdate(ArenaTeam* team, Player* player, bool won, int32 matchmakerChange);
    // Map thread, when a player leaves the arena: a counting bot stops counting and its casts are kept for the record
    // (a bot that stays logged in must not count in the open world; an early leaver keeps its casts)
    static void OnLeave(Battleground* bg, Player* player);
    // Map thread, from AllBattlegroundScript::OnBattlegroundEnd: builds and writes the record (arenas only)
    static void OnEnd(Battleground* bg, TeamId winnerTeam);

    // The harness: its records carry the server and a match id too
    static void Stamp(PvpTestMatchRecord& record);
    // The harness owns the match in this arena instance (call when the arena is created): at its end MatchLog keeps
    // the extra fields for the harness job instead of writing a record of its own
    static void Claim(uint32 instanceId, std::string const& matchId);
    // The harness, when it builds its record: copies the ratings, honor and so on in; false when nothing was kept
    static bool Merge(uint32 instanceId, PvpTestMatchRecord& record);

    // Playerbots.Ladder.ServerId, "main" when empty
    static std::string ServerId();
    // <server>-<process start YYYYMMDDHHMMSS UTC>-<n>
    static std::string NewMatchId();
    // A string-aware check that a JSON text is balanced and closes every string (the selftests)
    static bool JsonWellFormed(std::string const& json);

    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleSelftestCommand(ChatHandler* handler, char const* args);
};

#endif
