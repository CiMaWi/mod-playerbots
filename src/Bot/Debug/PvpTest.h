/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPTEST_H
#define _PLAYERBOT_PVPTEST_H

#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Common.h"
#include "ObjectGuid.h"
#include "PvpTestFight.h"
#include "PvpTestRecord.h"
#include "PvpTestUtil.h"
#include "SharedDefines.h"

class Battleground;
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
    // The record's identity: set by the manager; a single arena command has no plan and index 0
    std::string MatchId;
    std::string PlanId;
    uint32 PlanIndex = 0;
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
    std::string const& GetPlanId() const { return _params.PlanId; }
    uint32 GetPlanIndex() const { return _params.PlanIndex; }
    // The match record with every field filled from what the job knows now (also for failed and stopped matches)
    PvpTestMatchRecord BuildRecord() const;
    // Side A, then side B
    std::vector<ObjectGuid> AllMembers() const;

    static char const* StageName(Stage stage);

private:
    void EnterStage(Stage stage);
    void UpdateGather();
    bool GatherMember(PvpTestSide& side, size_t index);
    bool FormGroup(PvpTestSide& side);
    bool StartArena(std::string& error);
    void CaptureRoster();
    void CaptureScores(Battleground const* arena);
    void SetCounting(bool on);
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
    uint32 _instanceId = 0;
    BattlegroundTypeId _bgTypeId = BATTLEGROUND_TYPE_NONE;
    BattlegroundQueueTypeId _queueTypeId = BATTLEGROUND_QUEUE_NONE;
    PvpTestResult _result;
    PvpTestMatchRecord _record;
};

// Where a plan takes its bots from
enum class PvpTestPool
{
    Random,  // random disjoint pairs of maximum-level random bots
    Teams,   // the listed arena teams, two of them per match
    Bots     // the listed bots, drawn into disjoint sides
};

// N matches in a row with a concurrency cap; match k (1-based) replays seed SeedBase + k - 1. Modelled on
// mod-dungeon-clear's DcTestPlan.
struct PvpTestPlan
{
    std::string Id;
    uint32 Total = 0;
    uint32 Concurrency = 1;
    uint32 SeedBase = 0;
    PvpTestPool Pool = PvpTestPool::Random;
    std::string MapKey = "random";
    // Random and Bots: the candidates in guid order; Teams: the resolved sides
    std::vector<ObjectGuid> Candidates;
    std::vector<PvpTestSide> Teams;
    ObjectGuid Issuer;
    uint32 Launched = 0;
    uint32 Finished = 0;
    uint32 Failed = 0;
    uint32 Stopped = 0;
    bool StopRequested = false;
    std::string EndedEarly;  // why no more matches were launched, when the pool ran dry
    std::deque<std::string> Recent;
};

// Opt-in PvP harness (Playerbots.PvpTest.Enable): GM and console commands that force matches between bot teams.
// Single matches and plans of N matches (concurrent matches use disjoint bots); each finished match writes a record.
// Duels and brawls in the open world are single jobs of their own.
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
    // Writes the records of the unfinished jobs as stopped, drops the jobs and joins the record writer (server
    // shutdown logs the bots out anyway)
    void Shutdown();

    static bool HandleArenaCommand(ChatHandler* handler, char const* args);
    // Forced open-world fights (PvpTestFight.cpp): duel <a> <b> and brawl <a,b,...> vs <c,...>
    static bool HandleDuelCommand(ChatHandler* handler, char const* args);
    static bool HandleBrawlCommand(ChatHandler* handler, char const* args);
    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleStopCommand(ChatHandler* handler, char const* args);
    static bool HandlePlanStartCommand(ChatHandler* handler, char const* args);
    static bool HandlePlanStatusCommand(ChatHandler* handler, char const* args);
    static bool HandlePlanStopCommand(ChatHandler* handler, char const* args);

private:
    PvpTestMgr() = default;

    void Report(PvpTestArenaJob const& job);
    void ReportFight(PvpTestFightJob const& job);
    static bool HandleFightCommand(ChatHandler* handler, PvpTestFightParams& params);
    void UpdatePlan();
    bool LaunchPlanMatch(std::string& error);
    bool DrawRoster(PvpTestPlan const& plan, std::set<ObjectGuid> const& inUse, PvpTestArenaParams& params);
    std::set<ObjectGuid> MembersInUse() const;
    uint32 ActivePlanJobs() const;
    std::string DescribePlan() const;
    void FinishPlan();

    std::vector<std::unique_ptr<PvpTestArenaJob>> _jobs;
    // At most one open-world fight at a time (the damage counters are shared)
    std::vector<std::unique_ptr<PvpTestFightJob>> _fightJobs;
    std::unique_ptr<PvpTestPlan> _plan;
    uint32 _planTickMs = 0;
    uint32 _singleCounter = 0;
    std::string _lastPlan;
    std::string _lastResult;
    // The bots of the last finished job, listed by status to show they are out of groups and arenas
    std::vector<ObjectGuid> _lastMembers;
};

#endif
