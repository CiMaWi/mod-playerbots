/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENATEAMCONFIG_H
#define _PLAYERBOT_ARENATEAMCONFIG_H

#include <string>

#include "Define.h"

// The bot arena teams' formation and seeding (Playerbots.ArenaTeam.*, ArenaTeamFormation.cpp,
// RandomPlayerbotFactory.cpp; PlayerbotAIConfig::ArenaTeamOptions), read once at config load. The members are named
// after their keys without the prefix: SeedEnable is Seed.Enable, CompLearningFile is CompLearning.File. The conf.dist
// text above the keys describes the formation and the comp learning.
struct ArenaTeamConfig
{
    // Seed.*: a new bot team's rating and MMR from its members' skill percentiles plus noise (test servers)
    bool SeedEnable = false;
    float SeedSpread = 800.0f;   // 0-3000
    float SeedNoiseSd = 100.0f;  // 0-1000
    // Formation: on unless Ladder.Role = 2 (PlayerbotAIConfig::ApplyLadderRole); a test sets 0 for fixed teams
    bool FormationEnable = true;
    uint32 UpdateSeconds = 60;         // 5-3600: the pass interval
    uint32 MaxRoster2v2 = 4;           // 2-4: the most members a team holds
    uint32 MaxRoster3v3 = 6;           // 3-6
    uint32 MaxRoster5v5 = 10;          // 5-10
    uint32 MinMembers = 2;             // 2-10, never below the team size: fewer members recruit
    bool RandomPair = true;            // the queuing members are a random set of the roster
    bool RepickEachQueue = true;       // with RandomPair, a larger roster re-forms its group after every match
    uint32 LossStreakToConsider = 3;   // 1-50: the loss streak from which a member may leave
    uint32 LeaveChanceBottom = 20;     // 0-100 percent per lost match, at the bottom of the ladder
    uint32 LeaveChanceTop = 50;        // 0-100, at the top
    uint32 MinTeamStaySeconds = 600;   // 0-86400: no member leaves this soon after the team's last change
    uint32 ToleranceBottom = 40;       // 1-98 skill points: the largest gap to the team's mean a recruit may have
    uint32 ToleranceTop = 8;           // 1-98
    float SynergyWeight = 0.4f;        // 0-1: the share of class synergy in the score
    uint32 RecruitsPerPass = 4;        // 0-100: the most recruits per pass, over all teams
    uint32 RecruitAboveMinChance = 0;  // 0-100 percent per pass: a team above MinMembers takes one more member
    // CompLearning.*: the partner score learns comp strength from comp-strength.json (G6f)
    bool CompLearningEnable = false;
    std::string CompLearningFile;                 // empty = comp-strength.json in the configs folder
    float CompLearningWeightBottom = 0.2f;        // 0-1: the share of the learned value in the score
    float CompLearningWeightTop = 0.5f;           // 0-1
    float CompLearningExploreBottom = 0.3f;       // 0-2: the exploration factor
    float CompLearningExploreTop = 0.05f;         // 0-2
    float CompLearningTemperatureBottom = 0.25f;  // 0.01-10: the softmax temperature of the recruit pick
    float CompLearningTemperatureTop = 0.05f;     // 0.01-10
};

#endif
