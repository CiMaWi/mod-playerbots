/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LADDERMGR_H
#define _PLAYERBOT_LADDERMGR_H

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
// at startup. On main (Playerbots.Ladder.Role = 1) with Playerbots.Ladder.PoolTeams it also keeps up the pool: pairs of
// random bots at the maximum level, each in exactly one 2v2 team. World thread only, except IsQueueBlocked (bot AI, map
// threads).
class LadderMgr
{
public:
    enum class State : uint8
    {
        Departing,  // waits until no member is invited to or in a battleground, then for the logouts
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

    // Map threads: a member of a departing or departed team does not queue, nor any arena team member while drained
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

    // World thread (AcquaintanceMgr's event drain): a real player whispered, talked in a party, traded, dueled or
    // grouped with the bot. Pins the bot's team for Playerbots.Ladder.ContactMinutes
    void NoteContact(uint32 botLowGuid, uint32 now);
    // World thread: a member is with a real player now (group, battleground or arena, duel, trade), had a direct
    // contact with one within Playerbots.Ladder.ContactMinutes, or a GM pinned the team. A pinned team never departs
    bool IsTeamPinned(ArenaTeam* team, std::string* reason = nullptr) const;
    // World thread: a member is queued or in a battleground or arena, or the matchmaker picked the team
    static bool IsTeamBusy(ArenaTeam* team);
    // World thread: a depart or arrive of the team is in progress
    bool IsMoving(uint32 teamId) const;
    // Invited from a queue (STATUS_WAIT_JOIN)
    static bool HasBattlegroundInvite(Player* player);
    // World thread: the core's leave-queue path for every queue the player is in but not invited from; reason goes
    // into the log line
    static void LeaveBattlegroundQueues(Player* player, char const* reason);

    // Main role with Playerbots.Ladder.PoolTeams > 0
    bool PoolOn() const;
    // A pool team: a 2v2 team of exactly two random bots at the maximum level that are in no other arena team
    static bool IsPoolTeam(ArenaTeam* team);
    // World thread: true while the pool is short, so the bot arena team assignment leaves free bots at the maximum
    // level to the pool
    bool PoolWants(Player* bot) const;
    // World thread: the away teams of this type that are not pool teams and not in memory. The bot arena team registry
    // counts them, so team creation does not replace a team that is away (also after a restart)
    void CollectAwayTeams(uint8 type, std::vector<uint32>& out) const;

    // A free bot for the pool and whether it is online
    struct PoolBot
    {
        uint32 Guid = 0;
        bool Online = false;
    };
    // Pairs free bots per faction (index TEAM_ALLIANCE, TEAM_HORDE) into at most wanted (captain, partner) pairs, the
    // factions in turn. Two online bots pair first (the matchmaker picks only a team whose members are online), then
    // an online bot captains an offline one (it founds the team itself), then offline pairs (founded by another online
    // bot). Each bot is in one pair at most
    static std::vector<std::pair<PoolBot, PoolBot>> PairPoolBots(std::array<std::vector<PoolBot>, 2> const& freeBots,
                                                                 uint32 wanted);

    static bool HandleDepartCommand(ChatHandler* handler, char const* args);
    static bool HandleArriveCommand(ChatHandler* handler, char const* args);
    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleTeamsCommand(ChatHandler* handler, char const* args);
    // Test aid: pins a team for some minutes (0 unpins)
    static bool HandlePinCommand(ChatHandler* handler, char const* args);
    static bool HandleDrainCommand(ChatHandler* handler, char const* args);
    static bool HandleUndrainCommand(ChatHandler* handler, char const* args);
    static bool HandlePoolStatusCommand(ChatHandler* handler, char const* args);
    static bool HandlePoolBuildCommand(ChatHandler* handler, char const* args);
    // Checks PairPoolBots on made-up inputs (no game state)
    static bool HandlePoolSelftestCommand(ChatHandler* handler, char const* args);

private:
    struct Move
    {
        State Current = State::Hosted;
        // The members logged out by the depart (filled when the logouts start) or loaded by the arrive
        std::vector<ObjectGuid> Members;
        bool LoggedOut = false;
        uint32 WaitedMs = 0;
        uint32 LogoutIssuedMs = 0;  // WaitedMs when the logouts were last issued
        uint32 HostedSince = 0;     // Unix time of the last arrive; 0: since the server start
        std::string Note;
        // Set by the depart (or the startup drop of an away team): the team's type and whether it was a pool team
        uint8 Type = 0;
        bool Pool = false;
    };

    struct PoolCount
    {
        uint32 Resident = 0;
        uint32 Away = 0;
    };

    LadderMgr() = default;

    static bool IsBusy(State state);
    static char const* StateName(State state);
    static bool GroupedWithRealPlayer(Player* player);
    static char const* WithRealPlayer(Player* player);
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
    void FinishArrive(uint32 teamId, ArenaTeam* team);
    void NoteTeam(Move& move, ArenaTeam* team);
    void PruneContacts(uint32 now);
    PoolCount CountPool() const;
    bool IsBrokenPoolTeam(ArenaTeam* team) const;
    uint32 RepairPoolTeams(std::array<std::vector<PoolBot>, 2>& freeBots);
    bool IsFreePoolBot(ObjectGuid guid, uint8 maxLevel) const;
    static Player* FindPoolFounder();
    void UpdatePool(uint32 diff);
    void QueryPoolCandidates();
    void OnPoolCandidates(QueryResult result);

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
    std::atomic<bool> _draining{false};
    // Bots a real player had a direct contact with (low guid, Unix time) and GM pins (team id, end time); world
    // thread only
    std::unordered_map<uint32, uint32> _contacts;
    std::unordered_map<uint32, uint32> _gmPins;
    QueryCallbackProcessor _queryProcessor;
    uint32 _busy = 0;  // moves in Departing, Saving or Arriving
    uint32 _checkTimer = 0;
    bool _hostedOnly = false;
    bool _rosterLoaded = false;
    // Pool (main role); world thread only
    bool _poolOn = false;
    bool _poolShort = true;  // counted at startup and at every check
    bool _poolQueryPending = false;
    uint32 _poolTimer = 0;
    uint32 _poolCreated = 0;
    std::string _poolNote = "not checked yet";
};

#endif
