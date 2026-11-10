/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_UPKEEPCONFIG_H
#define _PLAYERBOT_UPKEEPCONFIG_H

#include "Define.h"

// The numbers of honest upkeep for persistent bots (Playerbots.Upkeep.*), read once at config load
// (PlayerbotAIConfig::Upkeep); the initial values are the conf.dist defaults. The kinds, the honest-kind table and the
// share rule are in UpkeepRules.h. Members without a conf.dist key yet are read by the step that uses them.
struct UpkeepConfig
{
    // Percent of persistent bots, chosen by guid (UpkeepRules.h), that keep every handout: the cohort the nights
    // compare with honest bots, and a switch for forced checks
    uint32 HandoutShare = 0;
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

#endif
