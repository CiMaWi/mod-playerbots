/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LADDERMGR_H
#define _PLAYERBOT_LADDERMGR_H

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AsyncCallbackProcessor.h"
#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "ObjectGuid.h"

class ChatHandler;
class Player;

// Runtime moves of one bot arena team (Playerbots.Ladder.Enable). Depart drops the team from memory once no member is
// queued or in a battleground and every member has logged out; arrive loads it back from its DB rows with two async
// queries. Never disbands and never deletes a row. World thread only, except IsQueueBlocked (bot AI, map threads).
class LadderMgr
{
public:
    enum class State : uint8
    {
        Departing,  // waits until no member is queued or in a battleground, then for the logouts
        Saving,     // dropped from memory; waits for the async barrier behind the writes
        Departed,
        Arriving,  // async load in flight
        Hosted,
        Failed
    };

    static LadderMgr& Instance()
    {
        static LadderMgr instance;
        return instance;
    }

    // Returns at once when no move is pending
    void Update(uint32 diff);

    // Map threads: a member of a departing or departed team does not queue
    bool IsQueueBlocked(Player* bot);
    // World thread: a member of a departed team stays logged out
    bool IsAway(uint32 botLowGuid) const { return !_away.empty() && _away.contains(botLowGuid); }

    static bool HandleDepartCommand(ChatHandler* handler, char const* args);
    static bool HandleArriveCommand(ChatHandler* handler, char const* args);
    static bool HandleStatusCommand(ChatHandler* handler, char const* args);

private:
    struct Move
    {
        State Current = State::Hosted;
        // The members logged out by the depart (filled when the logouts start)
        std::vector<ObjectGuid> Members;
        bool LoggedOut = false;
        uint32 WaitedMs = 0;
        std::string Note;
    };

    LadderMgr() = default;

    static bool IsBusy(State state);
    static char const* StateName(State state);
    static bool GroupedWithRealPlayer(Player* player);

    void SetState(Move& move, State state);
    void SetBlocked(uint32 teamId, bool blocked);
    void ReleaseMembers(Move& move);
    void Fail(uint32 teamId, std::string const& reason);
    void UpdateDeparting(uint32 teamId, Move& move);
    void OnTeamRow(QueryCallback& callback, uint32 teamId, QueryResult result);
    void OnMemberRows(uint32 teamId, QueryResult result);

    std::unordered_map<uint32, Move> _moves;
    // Arrive: the team row while the member query runs
    std::unordered_map<uint32, QueryResult> _teamRows;
    // Members of departed teams (low guids); world thread only
    std::unordered_set<uint32> _away;
    std::unordered_set<uint32> _blockedTeams;  // guarded by _blockedLock
    std::mutex _blockedLock;
    std::atomic<bool> _anyBlocked{false};
    QueryCallbackProcessor _queryProcessor;
    uint32 _busy = 0;  // moves in Departing, Saving or Arriving
    uint32 _checkTimer = 0;
};

#endif
