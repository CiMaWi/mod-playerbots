/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENATEAMFORMATION_H
#define _PLAYERBOT_ARENATEAMFORMATION_H

#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"

class ArenaTeam;
class ChatHandler;
class Player;

// Bot arena teams formed by partner score (Playerbots.ArenaTeam.*, earned rating mode only): a roster may hold more
// members than the team size, each queue takes a random pair of it, after a loss the member with the longest loss
// streak may leave, and teams short of members recruit bots without a team (close skill, complementary class). Teams
// low on the ladder are forgiving (rare leaves, wide skill tolerance, weighted-random picks), teams at the top strict.
// Every membership change runs on the world thread (the periodic pass, or the login assignment operation).
class ArenaTeamFormation
{
public:
    static ArenaTeamFormation& Instance()
    {
        static ArenaTeamFormation instance;
        return instance;
    }

    // Formation.Enable on, the earned rating mode, bot arena teams not being deleted
    static bool IsActive();
    // Most members a bot team of this type holds (the core allows type * 2)
    static uint32 MaxRoster(uint32 type);
    // Fewest members the pass keeps (never below the team size): fewer recruit, more may lose one
    static uint32 MinRoster(uint32 type);

    // Pure: how well a candidate fits a team, 0 (poor) .. 1 (good). skillDiff is the candidate's skill minus the team's
    // mean, used only when skillKnown (unknown counts as neutral); tolerance > 0. teamClasses: the members' classes.
    static float FitScore(bool skillKnown, float skillDiff, float tolerance, std::vector<uint8> const& teamClasses,
                          uint8 candidateClass, float synergyWeight);
    // Pure: class synergy of a candidate with the classes already in a team, 0 .. 1
    static float ClassSynergy(std::vector<uint8> const& teamClasses, uint8 candidateClass);

    // World thread, every tick: the pass runs every UpdateSeconds
    void Update(uint32 diff);

    // World thread (login assignment, a leaver): adds the bot to the best-fitting candidate team other than
    // excludedTeamId; true when it joined one
    bool JoinBestTeam(Player* bot, std::vector<ArenaTeam*> const& candidates, uint32 excludedTeamId);
    // World thread: a new team the bot founded (log and tracking)
    void OnFounded(Player* bot, ArenaTeam const* team);

    // Captain (map thread, read only): the standing group should get a fresh random pair before this queue
    bool ShouldRepick(Player* captain, ArenaTeam const* team) const;
    // The group formed for the team's next match has not played yet (any thread)
    bool IsPairFresh(uint32 teamId, uint32 teamSeasonGames) const;
    // World thread (ArenaGroupFormationOperation): a pair was formed for the next match
    void OnPairFormed(uint32 teamId, uint32 teamSeasonGames);

    // .playerbots arena teams: every bot team with its roster (never a skill value)
    static bool HandleTeamsCommand(ChatHandler* handler, char const* args);

private:
    ArenaTeamFormation() = default;

    // Per member, keyed by guid (members may be offline). In memory only: a restart starts every streak at 0.
    struct MemberTrack
    {
        uint32 TeamId = 0;
        uint16 SeasonGames = 0;
        uint16 SeasonWins = 0;
        uint32 LossStreak = 0;
        uint32 LossesSinceJoin = 0;
        uint32 SeenPass = 0;
    };

    struct Candidate
    {
        ObjectGuid Guid;
        bool Taken = false;
    };

    void RunPass();
    // Polls the members' season counters; true when a member lost a match since the last pass
    bool TrackLosses(ArenaTeam* team);
    void TryLeave(ArenaTeam* team, time_t now);
    void Recruit(ArenaTeam* team, std::vector<Candidate>& pool, uint32& joinsLeft, time_t now);
    void BuildPool(std::vector<Candidate>& pool) const;
    void OnJoined(Player* bot, ArenaTeam* team, time_t now);
    // Fit of a candidate for a team, or a negative value when its skill is outside the team's tolerance
    float TeamFit(ArenaTeam* team, Player* candidate) const;
    float LadderPercentile(uint32 teamId) const;
    uint32 LossStreakOf(ObjectGuid guid, uint32 teamId) const;

    uint32 _timer = 0;
    uint32 _pass = 0;
    uint32 _leaves = 0;
    uint32 _joins = 0;
    std::unordered_map<ObjectGuid::LowType, MemberTrack> _tracks;
    std::unordered_map<uint32, time_t> _teamChangedAt;
    std::unordered_map<uint32, float> _percentiles;
    // team id -> the team's season games when the pair for its next match was formed (written on the world thread,
    // read by captains on map threads)
    std::unordered_map<uint32, uint32> _pairFormedAtGames;
    mutable std::mutex _pairLock;
};

#endif
