/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPTEST_H
#define _PLAYERBOT_PVPTEST_H

#include <memory>
#include <string>
#include <vector>

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

class ChatHandler;
class Player;

// What a forced arena match is asked for; the arena type stays a parameter so 3v3 and 5v5 can follow
struct PvpTestArenaParams
{
    std::string SideA;
    std::string SideB;
    uint8 ArenaType = 2;
    // BATTLEGROUND_AA: the map is drawn from the seed
    BattlegroundTypeId Map = BATTLEGROUND_AA;
    bool Rated = false;
    uint32 Seed = 0;
};

// One side of a match: an arena team or an explicit roster of bot names
struct PvpTestSide
{
    std::string Label;
    uint32 ArenaTeamId = 0;
    std::vector<ObjectGuid> Members;
    std::vector<std::string> Names;
    std::vector<bool> LoginRequested;
    ObjectGuid::LowType GroupId = 0;
};

// The outcome of one job; C2 attaches its record to this
struct PvpTestResult
{
    std::string Outcome;  // winner, draw, timeout, ended, failed, stopped
    std::string Winner;
    std::string Map;
    std::string FailedStage;
    std::string Reason;
    uint32 FightMs = 0;
    uint32 TotalMs = 0;
};

// One forced arena match between two bot sides, ticked from the world thread. Stages: Resolve (in the command) ->
// Gather -> Start -> Monitor -> TearDown -> Done. Holds no Player* or Battleground* across ticks.
// The stage pattern is adapted from mod-dungeon-clear's DcTestRunJob; the gather and group steps from
// BGJoinAction::gatherArenaTeam (BattleGroundJoinAction.cpp).
class PvpTestArenaJob
{
public:
    enum class Stage
    {
        Resolve,
        Gather,
        Start,
        Monitor,
        TearDown,
        Done
    };

    PvpTestArenaJob(PvpTestArenaParams const& params, PvpTestSide sideA, PvpTestSide sideB, ObjectGuid issuer);

    // Builds the two sides from team names or name rosters; returns false with the reason in error
    static bool Resolve(PvpTestArenaParams const& params, PvpTestSide& sideA, PvpTestSide& sideB, std::string& error);

    void Update(uint32 diff);
    // Ends the job early: the bots leave the arena and their groups
    void Abort(std::string const& reason);

    bool IsDone() const { return _stage == Stage::Done; }
    Stage GetStage() const { return _stage; }
    PvpTestResult const& GetResult() const { return _result; }
    std::string Describe() const;
    std::string ResultLine() const;
    ObjectGuid GetIssuer() const { return _issuer; }
    // Side A, then side B
    std::vector<ObjectGuid> AllMembers() const;

    static char const* StageName(Stage stage);

private:
    void EnterStage(Stage stage);
    void UpdateGather();
    bool GatherMember(PvpTestSide& side, size_t index);
    bool FormGroup(PvpTestSide& side);
    bool StartArena(std::string& error);
    void UpdateMonitor(bool slowTick);
    void SendAcceptFallback();
    void UpdateTearDown();
    void FinishTearDown(bool timedOut);
    void Fail(std::string const& reason);

    PvpTestArenaParams _params;
    PvpTestSide _sideA;
    PvpTestSide _sideB;
    ObjectGuid _issuer;
    Stage _stage = Stage::Resolve;
    uint32 _stageMs = 0;
    uint32 _tickMs = 0;
    uint32 _totalMs = 0;
    uint32 _fightStartMs = 0;
    bool _fightStarted = false;
    bool _acceptFallbackSent = false;
    bool _ended = false;
    uint32 _instanceId = 0;
    BattlegroundTypeId _bgTypeId = BATTLEGROUND_TYPE_NONE;
    BattlegroundQueueTypeId _queueTypeId = BATTLEGROUND_QUEUE_NONE;
    PvpTestResult _result;
};

// Opt-in PvP harness (Playerbots.PvpTest.Enable): GM and console commands that force matches between bot teams.
// One job at a time for now; the job list is a vector so later steps can run several.
class PvpTestMgr
{
public:
    static PvpTestMgr& Instance()
    {
        static PvpTestMgr instance;
        return instance;
    }

    // World thread, once per world tick; returns at once while no job exists
    void Update(uint32 diff);
    // Drops the jobs (server shutdown logs the bots out anyway)
    void Shutdown();

    static bool HandleArenaCommand(ChatHandler* handler, char const* args);
    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleStopCommand(ChatHandler* handler, char const* args);

private:
    PvpTestMgr() = default;

    void Report(PvpTestArenaJob const& job);

    std::vector<std::unique_ptr<PvpTestArenaJob>> _jobs;
    std::string _lastResult;
    // The bots of the last finished job, listed by status to show they are out of groups and arenas
    std::vector<ObjectGuid> _lastMembers;
};

#endif
