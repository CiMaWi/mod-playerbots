/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TESTPLAYER_H
#define _PLAYERBOT_TESTPLAYER_H

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "Common.h"
#include "ObjectGuid.h"

class ChatHandler;
class LoginQueryHolder;
class Player;
class WorldPacket;
class WorldSession;

struct TestPlayerWaypoint
{
    uint32 mapId;
    float x;
    float y;
    float z;
};

// Headless stand-ins for real players in unattended tests: characters of a plain (non-bot) account, each logged in
// through its own client-less WorldSession without a PlayerbotAI, so playerbots counts them as real players. Each
// can walk a named waypoint route in a loop. A verb acts as the character a leading as=<character> token names, else
// as the first one logged in that is still present. Opt-in through Playerbots.TestPlayer.Enable; world thread only.
class TestPlayerMgr
{
public:
    static TestPlayerMgr& Instance()
    {
        static TestPlayerMgr instance;
        return instance;
    }

    // World thread, once per world tick; returns at once while no test player exists
    void Update(uint32 diff);
    // Logs every test player out (server shutdown)
    void Shutdown();
    // True for a test player from session creation to logout. Code that counts a headless session as a bot uses it
    // to count the test player as the real player it stands in for
    bool IsTestPlayer(Player const* player) const;

    static bool HandleLoginCommand(ChatHandler* handler, char const* args);
    static bool HandleRouteCommand(ChatHandler* handler, char const* args);
    static bool HandleLogoutCommand(ChatHandler* handler, char const* args);
    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    // Whispers a character through the chat opcode handler, as a client would (the chat hooks run)
    static bool HandleWhisperCommand(ChatHandler* handler, char const* args);
    // Sends party chat through the chat opcode handler so the normal group chat hooks run
    static bool HandlePartyCommand(ChatHandler* handler, char const* args);
    // Groups a character with the test player as the leader (as an accepted invite), or "none" leaves the group
    static bool HandleGroupCommand(ChatHandler* handler, char const* args);
    // Invites a character as a client would (CMSG_GROUP_INVITE through the test player's session): the character's
    // answer decides; a bot's own trigger answers at once
    static bool HandleInviteCommand(ChatHandler* handler, char const* args);
    // Queues a 2v2 arena (rated: in a 2v2 team with the character, founded if missing) grouped with a character;
    // "leave" leaves the queue. The test player accepts every battleground invite
    static bool HandleArenaQueueCommand(ChatHandler* handler, char const* args);
    // Opens a ready check as the leader of the test player's group, or of the named character's group (an empty
    // MSG_RAID_READY_CHECK through the leader's opcode handler)
    static bool HandleReadyCheckCommand(ChatHandler* handler, char const* args);
    // "READYANSWER <character>: <n> x ready|not ready" (n answers, the last one's state) or "none": the character's
    // answers to ready checks opened here
    static bool HandleReadyAnswerCommand(ChatHandler* handler, char const* args);
    // Any thread: a ready check answer sent to a player; kept when that player opened a check here
    void OnPacketSent(ObjectGuid receiver, WorldPacket const& packet);

private:
    // One test player: its session, its login state and its route
    class Character
    {
    public:
        bool Login(ChatHandler* handler, std::string const& name, std::string const& route, uint8 race);
        bool SetRoute(ChatHandler* handler, std::string const& route);
        void Status(ChatHandler* handler);
        bool Whisper(ChatHandler* handler, std::string const& target, std::string const& text);
        bool Party(ChatHandler* handler, std::string const& text);
        bool GroupWith(ChatHandler* handler, std::string const& target);
        bool Invite(ChatHandler* handler, std::string const& target);
        bool ArenaQueue(ChatHandler* handler, std::string const& partnerName, bool rated);
        bool OpenReadyCheck(ChatHandler* handler, std::string const& target);
        void Update(uint32 diff);
        void Reset();

        bool IsIdle() const { return _state == State::Idle; }
        bool IsOnline() const { return OnlinePlayer() != nullptr; }
        bool Owns(Player const* player) const;
        std::string const& Name() const { return _name; }

    private:
        enum class State
        {
            Idle,
            Creating,
            Loading,
            Online
        };

        Player* OnlinePlayer() const;
        void AcceptInvites(Player* player, uint32 diff);
        void StartLoad();
        void OnLoaded(LoginQueryHolder const& holder);
        void StartRoute(Player* player);
        void UpdateRoute(Player* player, uint32 diff);
        bool HandleTeleport(Player* player);

        State _state = State::Idle;
        // Set by a DB callback that failed; Update tears the session down after the callbacks have run
        bool _failed = false;
        WorldSession* _session = nullptr;
        ObjectGuid _guid;
        std::string _name;
        uint32 _accountId = 0;
        uint32 _pendingTimer = 0;
        uint32 _moveTimer = 0;
        std::string _routeName;
        std::vector<TestPlayerWaypoint> const* _route = nullptr;
        uint32 _waypoint = 0;
        uint32 _attempts = 0;
        uint32 _portTimer = 0;
    };

    TestPlayerMgr() = default;

    bool Login(ChatHandler* handler, std::string name, std::string const& route, uint8 race);
    bool Logout(ChatHandler* handler, std::string args);
    // The test player a verb acts as: the one a leading as=<character> token names (the token is taken off args),
    // else the first one logged in that is still present. Null after a failure message to the handler
    Character* Pick(ChatHandler* handler, std::string& args);
    Character* Find(std::string const& name) const;
    void ClearReadyChecks();

    static std::vector<TestPlayerWaypoint> const* FindRoute(std::string const& name);
    static std::string RouteNames();

    // In login order; the first is the one a verb without as= acts as
    std::vector<std::unique_ptr<Character>> _characters;
    // The ready check answers by member, sent from the members' map threads
    std::mutex _readyLock;
    struct ReadyAnswer
    {
        uint8 State = 0;
        uint32 Count = 0;
    };
    std::map<ObjectGuid, ReadyAnswer> _readyAnswers;
    std::set<ObjectGuid> _readyOpeners;
};

#endif
