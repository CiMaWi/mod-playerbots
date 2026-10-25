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

class ArenaTeam;
class ChatHandler;
class Player;

// Runtime moves of one bot arena team (Playerbots.Ladder.Enable or Playerbots.Ladder.Role). Depart drops the team from
// memory once no member is queued or in a battleground and every member has logged out; arrive loads it back from its
// DB rows with two async queries. Never disbands and never deletes a team row. With a ladder role the members' state
// (hosted or away) is kept in the playerbots table playerbots_ladder_roster, written only by depart and arrive and read
// at startup. World thread only, except IsQueueBlocked (bot AI, map threads).
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

    // playerbots_ladder_roster.state
    enum RosterState : uint8
    {
        ROSTER_HOSTED = 0,
        ROSTER_AWAY = 1
    };

    static LadderMgr& Instance()
    {
        static LadderMgr instance;
        return instance;
    }

    // The moves' commands work (Playerbots.Ladder.Enable or a ladder role)
    static bool MovesOn();

    // Startup (PlayerbotAIConfig::Initialize, before the bot arena team registry is built): with a ladder role, loads
    // the roster and drops the teams this server does not host from memory (no DB writes). Once; role 0 does nothing
    void LoadRoster();

    // Returns at once when no move is pending
    void Update(uint32 diff);

    // Map threads: a member of a departing or departed team does not queue
    bool IsQueueBlocked(Player* bot);
    // World thread: an away bot never logs in here. Main: a member of a departed team; arena role: every bot the
    // roster does not host
    bool IsAway(uint32 botLowGuid) const
    {
        if (_hostedOnly)
            return !_hosted.contains(botLowGuid);

        return !_away.empty() && _away.contains(botLowGuid);
    }

    // Arena role (Playerbots.Ladder.Role = 2): only the hosted roster bots log in
    bool IsHostedOnly() const { return _hostedOnly; }
    uint32 GetHostedBotCount() const { return _hosted.size(); }
    // World thread, arena role: lets every hosted bot that is not in the random bot manager's set log in
    void LoginHosted();

    static bool HandleDepartCommand(ChatHandler* handler, char const* args);
    static bool HandleArriveCommand(ChatHandler* handler, char const* args);
    static bool HandleStatusCommand(ChatHandler* handler, char const* args);

private:
    struct Move
    {
        State Current = State::Hosted;
        // The members logged out by the depart (filled when the logouts start) or loaded by the arrive
        std::vector<ObjectGuid> Members;
        bool LoggedOut = false;
        uint32 WaitedMs = 0;
        std::string Note;
    };

    LadderMgr() = default;

    static bool IsBusy(State state);
    static char const* StateName(State state);
    static bool GroupedWithRealPlayer(Player* player);
    static void DropTeam(ArenaTeam* team);

    void SetState(Move& move, State state);
    void SetBlocked(uint32 teamId, bool blocked);
    void ReleaseMembers(uint32 teamId, Move& move);
    void CancelDepart(uint32 teamId, Move& move, State state, std::string const& note);
    void WriteRoster(uint32 teamId, std::vector<ObjectGuid> const& members, bool away);
    void Fail(uint32 teamId, std::string const& reason);
    void UpdateDeparting(uint32 teamId, Move& move);
    void OnTeamRow(QueryCallback& callback, uint32 teamId, QueryResult result);
    void OnMemberRows(uint32 teamId, QueryResult result);

    std::unordered_map<uint32, Move> _moves;
    // Arrive: the team row while the member query runs
    std::unordered_map<uint32, QueryResult> _teamRows;
    // Members of departed teams (low guids) and their team ids; world thread only
    std::unordered_set<uint32> _away;
    std::unordered_set<uint32> _awayTeams;
    // Arena role: the bots and teams this server hosts; world thread only
    std::unordered_set<uint32> _hosted;
    std::unordered_set<uint32> _hostedTeams;
    std::unordered_set<uint32> _blockedTeams;  // guarded by _blockedLock
    std::mutex _blockedLock;
    std::atomic<bool> _anyBlocked{false};
    QueryCallbackProcessor _queryProcessor;
    uint32 _busy = 0;  // moves in Departing, Saving or Arriving
    uint32 _checkTimer = 0;
    bool _hostedOnly = false;
    bool _rosterLoaded = false;
};

#endif
