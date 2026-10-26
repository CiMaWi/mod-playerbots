/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MATCHMAKER_H
#define _PLAYERBOT_MATCHMAKER_H

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"

class ArenaTeam;
class ChatHandler;
class Group;
class Player;

// One resident team the pick may draw
struct MatchmakerCandidate
{
    uint32 TeamId = 0;
    uint32 Mmr = 0;
    bool Busy = false;  // a member is offline, in a battleground or its queue, a dungeon or raid, the LFG or a move
    bool Pinned = false;
};

struct MatchmakerSettings
{
    uint32 MmrWidth = 150;
    uint32 Window = 200;
    uint32 WindowGrowth = 5;  // per second of wait
};

struct MatchmakerPick
{
    int32 Index = -1;     // into the candidates; -1: none could be drawn
    uint32 Window = 0;    // the MMR window drawn from (0: unrated, no window)
    uint32 Eligible = 0;  // candidates inside the window
};

// Main (Playerbots.Ladder.Matchmaker.Enable, DM.6): answers each 2v2 arena queue of a real player's group with one
// resident ladder team of the pool, queued a few seconds later, drawn around the group's MMR. The OnAddGroup hook only
// records the request; the pick, the group formation, the queue and the leftovers run in Update on the world thread.
// Nothing is persisted.
class Matchmaker
{
public:
    static Matchmaker& Instance()
    {
        static Matchmaker instance;
        return instance;
    }

    // Draws one candidate: never a busy, pinned or recent one (oldest first; when only recent ones are left, all but
    // the newest may be drawn), rated inside an MMR window that grows with the wait (an empty window doubles until it
    // holds a candidate), weighted by 1 / (1 + (distance / MmrWidth)^2); unrated uniformly. roll is in [0, 1). Pure
    static MatchmakerPick Pick(uint32 mmr, bool rated, uint32 waitSeconds,
                               std::vector<MatchmakerCandidate> const& candidates, std::vector<uint32> const& recent,
                               MatchmakerSettings const& settings, float roll);

    // World thread: queues the leader's group for an arena as the group branch of CMSG_BATTLEMASTER_JOIN_ARENA does,
    // without a battlemaster. Empty on success, else the reason
    static std::string JoinArenaQueue(Player* leader, uint8 arenaType, bool rated);

    // AllBattlegroundScript::OnAddGroup: records a real player's group queue (2v2); never touches the queue
    void OnAddGroup(Player* leader, Group* group, uint8 arenaType, bool rated, uint32 matchmakerRating);
    // World thread; returns at once while no request is pending
    void Update(uint32 diff);
    // World thread: a team the matchmaker picked, until its match ends or it left the queue
    bool IsReserved(uint32 teamId) const { return !_reserved.empty() && _reserved.contains(teamId); }

    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleCandidatesCommand(ChatHandler* handler, char const* args);
    // Checks Pick on made-up candidates (no game state)
    static bool HandleSelftestCommand(ChatHandler* handler, char const* args);

private:
    enum class Phase : uint8
    {
        Waiting,  // the delay, or no candidate yet
        Forming,  // the team's group is being formed
        Queued,
        Playing
    };

    struct Incoming
    {
        ObjectGuid Leader;
        std::vector<ObjectGuid> RealMembers;
        uint8 ArenaType = 0;
        bool Rated = false;
        uint32 Mmr = 0;
    };

    struct Request
    {
        Incoming Source;
        std::string LeaderName;
        uint32 QueuedAtMs = 0;
        uint32 DelayMs = 0;  // from PhaseSinceMs to the next pick
        uint32 PhaseSinceMs = 0;
        Phase Current = Phase::Waiting;
        uint32 Picks = 0;
        uint32 PickSerial = 0;
        uint32 TeamId = 0;
        ObjectGuid Captain;
        std::vector<ObjectGuid> Members;
        std::string Note;
    };

    struct PickRecord
    {
        uint32 Serial = 0;
        uint32 At = 0;  // Unix time
        std::string Player;
        uint32 TeamId = 0;
        std::string TeamName;
        bool Rated = false;
        uint32 PlayerMmr = 0;
        uint32 TeamMmr = 0;
        uint32 Window = 0;
        uint32 Eligible = 0;
        uint32 WaitSeconds = 0;
        std::string Outcome;
    };

    // A candidate's team and the members who would play: the captain first if free
    struct CandidateTeam
    {
        ArenaTeam* Team = nullptr;
        std::vector<ObjectGuid> Members;
    };

    Matchmaker() = default;

    static MatchmakerSettings Settings();
    static char const* PhaseName(Phase phase);
    // The 2v2 teams the matchmaker may draw (pool teams, or every random bot team with PoolOnly = 0) with their
    // state; reasons (optional) says why a team is busy or pinned
    static void BuildCandidates(Player* leader, uint8 arenaType, std::vector<MatchmakerCandidate>& candidates,
                                std::vector<CandidateTeam>& teams, std::vector<std::string>* reasons);

    void TakeIncoming(uint32 now);
    bool Step(Request& request, uint32 now);
    void TryPick(Request& request, Player* leader, uint32 now);
    void Release(Request& request, bool dequeue);
    void SetOutcome(uint32 serial, std::string const& outcome);
    std::vector<uint32> RecentFor(Request const& request) const;

    std::mutex _incomingLock;
    std::vector<Incoming> _incoming;  // guarded by _incomingLock
    std::atomic<bool> _hasIncoming{false};
    // World thread only
    std::vector<Request> _requests;
    std::unordered_set<uint32> _reserved;
    std::unordered_map<ObjectGuid, std::vector<uint32>> _recent;  // per real player, oldest first
    std::deque<PickRecord> _picks;
    uint32 _pickSerial = 0;
    uint32 _checkTimer = 0;
};

#endif
