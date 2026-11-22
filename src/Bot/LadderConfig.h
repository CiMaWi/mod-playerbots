/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LADDERCONFIG_H
#define _PLAYERBOT_LADDERCONFIG_H

#include <string>

#include "Define.h"

// The ladder's matchmaker (Playerbots.Ladder.Matchmaker.*, Matchmaker.cpp; PlayerbotAIConfig::Ladder.Matchmaker), read
// once at config load. It answers each 2v2 arena queue of a real player's group with one resident pool team, queued a
// few seconds later, so the core pairs the two (the conf.dist text of Playerbots.Ladder.Matchmaker.* describes it).
struct MatchmakerConfig
{
    // Derived, not a key: on exactly at Ladder.Role = 1 (PlayerbotAIConfig::ApplyLadderRole)
    bool Enable = false;
    uint32 DelayMinSeconds = 3;  // 0-600; the pick comes a random time in DelayMin..DelayMax after the queue
    uint32 DelayMaxSeconds = 8;  // DelayMin-600
    uint32 MmrWidth = 150;       // 1-5000: the MMR distance at which a team is half as likely
    uint32 Window = 200;         // 1-5000: the MMR window around the group at its queue
    uint32 WindowGrowth = 5;     // 0-1000: its growth per second of wait
    uint32 RecentOpponents = 3;  // 0-20: teams a player faced last that are not drawn again
    uint32 TimeoutSeconds = 60;  // 5-3600: a drawn team forms its group and is paired within this
    uint32 MaxPicks = 3;         // 1-100: teams drawn at most for one queue
    bool Skirmish = true;        // answer skirmish queues too
    bool PoolOnly = true;        // draw only pool teams
};

// The ladder (Playerbots.Ladder.*; PlayerbotAIConfig::Ladder), read once at config load
struct LadderConfig
{
    uint32 Role = 0;                    // 0 off, 1 main, 2 arena server (enum LadderRole)
    std::string ServerId;               // the instance's name; "lab..." marks a lab
    uint32 DepartTimeoutSeconds = 600;  // 5-86400: a depart that waits longer is cancelled
    uint32 ContactMinutes = 15;         // 0-1440: a real player's contact pins a team (0 = none pin)
    uint32 PoolTeams = 0;               // 0-5000: the 2v2 teams of the ladder pool at main
    MatchmakerConfig Matchmaker;
};

template <typename Struct>
struct ConfigFamily;

// The config table family (LadderConfig.cpp)
extern ConfigFamily<LadderConfig> const LadderFamily;

// The config table family (LadderConfig.cpp)
extern ConfigFamily<MatchmakerConfig> const MatchmakerFamily;

// The one instance (LadderConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.Ladder.
LadderConfig& LadderConfigStorage();

#endif
