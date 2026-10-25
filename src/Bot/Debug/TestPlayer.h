/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TESTPLAYER_H
#define _PLAYERBOT_TESTPLAYER_H

#include <string>
#include <vector>

#include "Common.h"
#include "ObjectGuid.h"

class ChatHandler;
class LoginQueryHolder;
class Player;
class WorldSession;

struct TestPlayerWaypoint
{
    uint32 mapId;
    float x;
    float y;
    float z;
};

// A headless stand-in for a real player in unattended tests: a character of a plain (non-bot) account, logged in
// through a client-less WorldSession without a PlayerbotAI, so playerbots counts it as a real player. It walks a
// named waypoint route in a loop. Opt-in through Playerbots.TestPlayer.Enable; world thread only.
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
    // Logs the test player out (server shutdown)
    void Shutdown();

    static bool HandleLoginCommand(ChatHandler* handler, char const* args);
    static bool HandleRouteCommand(ChatHandler* handler, char const* args);
    static bool HandleLogoutCommand(ChatHandler* handler, char const* args);
    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    // Whispers a character through the chat opcode handler, as a client would (the chat hooks run)
    static bool HandleWhisperCommand(ChatHandler* handler, char const* args);
    // Groups a character with the test player as the leader (as an accepted invite), or "none" leaves the group
    static bool HandleGroupCommand(ChatHandler* handler, char const* args);

private:
    enum class State
    {
        Idle,
        Creating,
        Loading,
        Online
    };

    TestPlayerMgr() = default;

    bool Login(ChatHandler* handler, std::string name, std::string const& route);
    bool SetRoute(ChatHandler* handler, std::string const& route);
    bool Logout(ChatHandler* handler);
    void Status(ChatHandler* handler);
    Player* OnlinePlayer() const;
    bool Whisper(ChatHandler* handler, std::string const& target, std::string const& text);
    bool GroupWith(ChatHandler* handler, std::string const& target);

    void StartLoad();
    void OnLoaded(LoginQueryHolder const& holder);
    void StartRoute(Player* player);
    void UpdateRoute(Player* player, uint32 diff);
    bool HandleTeleport(Player* player);
    void Reset();

    static std::vector<TestPlayerWaypoint> const* FindRoute(std::string const& name);
    static std::string RouteNames();

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
};

#endif
