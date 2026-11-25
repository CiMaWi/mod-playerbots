/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_UPKEEPCONFIG_H
#define _PLAYERBOT_UPKEEPCONFIG_H

#include "Define.h"

// The numbers of honest upkeep for persistent bots (PlayerbotAIConfig::Upkeep); the initial values are the ones the
// readers use. None of them is a conf.dist key yet: the step that makes one adds a config table family. The kinds,
// the honest-kind table and the grant rule are in UpkeepRules.h.
struct UpkeepConfig
{
    // Money kept for training: the trainer spells of the next this many levels
    uint32 TrainingReserveLevels = 2;
    // Flying: the height-map cell, how far a flight profile looks ahead, the clearance kept above the ground, and the
    // hop below which a bot stays on the ground
    uint32 HeightCellYards = 67;
    uint32 FlightStretchYards = 500;
    uint32 FlightClearanceMinYards = 30;
    uint32 FlightClearanceMaxYards = 50;
    uint32 GroundHopYards = 150;
};

// The one instance (UpkeepConfig.cpp). Only PlayerbotAIConfig's constructor calls this: readers use
// sPlayerbotAIConfig.Upkeep.
UpkeepConfig& UpkeepConfigStorage();

#endif
