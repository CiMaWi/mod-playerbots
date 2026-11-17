/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ARENARECRUITCONFIG_H
#define _PLAYERBOT_ARENARECRUITCONFIG_H

#include "Define.h"

// The hidden rule for a real player's arena team invite to a random bot (Playerbots.ArenaRecruit.*, ArenaRecruit.cpp;
// PlayerbotAIConfig::ArenaRecruitOptions), read once at config load. The conf.dist text above the keys describes it.
struct ArenaRecruitConfig
{
    // On unless Ladder.Role = 2 (PlayerbotAIConfig::ApplyLadderRole); a test sets 0 for fixed teams
    bool Enable = true;
    uint32 RatingAtSkill1 = 0;               // 0-10000: the team rating a bot of skill 1 considers its own level
    uint32 RatingAtSkill99 = 2000;           // 0-10000: the same at skill 99 (linear between)
    uint32 DeltaBase = 300;                  // 0-10000: the rating gap every bot allows
    float DeltaPerAcquaintancePoint = 4.0f;  // 0-1000: extra gap per acquaintance score point
    uint32 DeltaMax = 1500;                  // 0-10000: the most gap that is ever allowed
    uint32 LoyaltyMinGames = 5;              // 1-1000: games from which a winning team's bot is loyal
    uint32 LoyaltyMinWinPercent = 55;        // 0-100: and its win share
};

#endif
