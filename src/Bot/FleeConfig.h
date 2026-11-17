/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FLEECONFIG_H
#define _PLAYERBOT_FLEECONFIG_H

#include "Define.h"

// The flight from attacking enemy players (Playerbots.Flee.*, FleeAttackersAction; PlayerbotAIConfig::Flee), read once
// at config load. A bot that flees runs from the enemy players in PvP combat with it. Its flight ends as escaped when it
// is out of PvP combat or EscapeYards from every one of them; as cornered when its nearest attacker got no farther from
// it for CorneredSeconds (the best distance so far, in steps of 1 yd: a bot that cannot shake a pursuer turns and
// fights; the time it cannot move, rooted or stunned, does not count, and the measure starts again when it can move);
// or by its bound after BoundSeconds. After cornered or the bound it stands and fights and does not flee again in that
// PvP combat. A persistent bot in the open world flees when it left its fight by its enthusiasm and an enemy player
// attacks it (WorldPvpConfig.h).
struct FleeConfig
{
    uint32 EscapeYards = 40;     // 1-2000
    uint32 CorneredSeconds = 6;  // 1-600
    uint32 BoundSeconds = 30;    // 1-600
};

#endif
