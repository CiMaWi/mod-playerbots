/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPTESTFIGHT_H
#define _PLAYERBOT_PVPTESTFIGHT_H

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "Common.h"
#include "ObjectGuid.h"
#include "PvpTestRecord.h"
#include "PvpTestUtil.h"

class Player;
class Unit;

// What a forced open-world fight is asked for
struct PvpTestFightParams
{
    std::string Kind;   // duel or brawl
    std::string SideA;  // bot names, comma separated
    std::string SideB;
    std::string MatchId;
};

// Damage, healing, killing blows and deaths of the bots of the running fight (the battleground scores do not exist in
// the open world). The hooks run on map threads: they read and write nothing but the fixed atomic slots below, and a
// single relaxed load ends them while no fight runs. The world thread owns Begin and End.
class PvpTestFightStats
{
public:
    static constexpr size_t MaxSlots = 16;
    static constexpr size_t NoSlot = MaxSlots;

    struct Totals
    {
        uint32 Damage = 0;
        uint32 Healing = 0;
        uint32 KillingBlows = 0;
        uint32 Deaths = 0;
    };

    // How a duel ended, as the duel-end hook saw it
    struct DuelEnd
    {
        bool Seen = false;
        size_t Winner = NoSlot;
        size_t Loser = NoSlot;
        uint32 Type = 0;  // DuelCompleteType
    };

    static PvpTestFightStats& Instance();

    // Slots are the members in order, the first sideASize of them on side A
    void Begin(std::vector<ObjectGuid> const& members, size_t sideASize);
    void End();
    Totals Get(size_t slot) const;
    DuelEnd GetDuelEnd() const;

    // Map threads
    void OnDamage(Unit* attacker, Unit* victim, uint32 damage);
    void OnHeal(Unit* healer, Unit* receiver, uint32 gain);
    void OnKill(Player* killer, Player* killed);
    void OnDuelEnd(Player* winner, Player* loser, uint32 type);

private:
    PvpTestFightStats();

    size_t FindSlot(uint64 rawGuid) const;
    bool SameSide(size_t first, size_t second) const;

    std::atomic<bool> _active{false};
    std::atomic<size_t> _sideASize{0};
    std::array<std::atomic<uint64>, MaxSlots> _guids;
    std::array<std::atomic<uint32>, MaxSlots> _damage;
    std::array<std::atomic<uint32>, MaxSlots> _healing;
    std::array<std::atomic<uint32>, MaxSlots> _kills;
    std::array<std::atomic<uint32>, MaxSlots> _deaths;
    std::atomic<uint32> _duelEnd{0};
};

// One forced fight between bots of two sides in the open world, ticked from the world thread. Stages: Gather (log in,
// out of groups, queues and combat; the positions are saved) -> Fight (teleport to the fight spot, full health, then
// the duel is challenged or the brawlers are pointed at each other) -> TearDown (teleport back, strategies reset) ->
// Done. Holds no Player* across ticks. Duels need the area flag that allows them (checked); fights are unrated.
class PvpTestFightJob
{
public:
    enum class Stage
    {
        Gather,
        Fight,
        TearDown,
        Done
    };

    PvpTestFightJob(PvpTestFightParams const& params, PvpTestSide sideA, PvpTestSide sideB, ObjectGuid issuer);
    ~PvpTestFightJob();

    // Builds the two sides from name rosters; returns false with the reason in error
    static bool Resolve(PvpTestFightParams const& params, PvpTestSide& sideA, PvpTestSide& sideB, std::string& error);

    void Update(uint32 diff);
    // Ends the job early: the bots are sent back
    void Abort(std::string const& reason);

    bool IsDone() const { return _stage == Stage::Done; }
    PvpTestResult const& GetResult() const { return _result; }
    std::string Describe() const;
    std::string ResultLine() const;
    ObjectGuid GetIssuer() const { return _issuer; }
    PvpTestMatchRecord BuildRecord() const;
    // Side A, then side B
    std::vector<ObjectGuid> AllMembers() const { return _members; }

    static char const* StageName(Stage stage);

private:
    enum class Phase
    {
        Arrive,     // teleported, waiting until every bot stands at the spot
        Challenge,  // duel: cast, wait for the accept and the countdown
        Active      // the fight runs
    };

    struct Origin
    {
        bool Saved = false;
        bool SentBack = false;
        bool Back = false;
        bool Pvp = false;
        uint32 MapId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
    };

    void EnterStage(Stage stage);
    void Fail(std::string const& reason);
    void UpdateGather();
    bool GatherMember(size_t index);
    void SaveOriginsAndTeleport();
    void UpdateArrive();
    void UpdateChallenge();
    void UpdateDuel();
    void UpdateBrawl();
    void EndFight();
    void CaptureStats();
    void UpdateTearDown();
    void FinishTearDown(bool timedOut);
    void SetWinner(size_t winnerSide, std::string const& reason);
    size_t SideOf(size_t index) const { return index < _sideASize ? 0 : 1; }
    bool IsDuel() const { return _params.Kind == "duel"; }

    PvpTestFightParams _params;
    PvpTestSide _sideA;
    PvpTestSide _sideB;
    ObjectGuid _issuer;
    std::vector<ObjectGuid> _members;
    std::vector<std::string> _names;
    std::vector<bool> _loginRequested;
    std::vector<bool> _arrived;
    std::vector<Origin> _origins;
    size_t _sideASize = 0;
    Stage _stage = Stage::Gather;
    Phase _phase = Phase::Arrive;
    uint32 _stageMs = 0;
    uint32 _tickMs = 0;
    uint32 _totalMs = 0;
    uint32 _fightStartMs = 0;
    uint32 _challengeMs = 0;
    bool _statsBegun = false;
    bool _duelSeen = false;
    bool _acceptSent = false;
    PvpTestResult _result;
    PvpTestMatchRecord _record;
};

#endif
