/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PVPGEARCONFIG_H
#define _PLAYERBOT_PVPGEARCONFIG_H

#include <string>

#include "Define.h"

// PvP gear for ladder bots (Playerbots.PvpGear.*, PvpGearMgr.cpp, PvpGearWear.cpp; PlayerbotAIConfig::PvpGear), read
// once at config load. The members are named after their keys without the prefix (BuyEnable is Buy.Enable). The
// conf.dist text above the keys describes the catalogue, the loadout and the buying.
struct PvpGearConfig
{
    // The tiers, lowest first, comma-separated; a tier lists item name prefixes separated by "/". Parsed and checked by
    // PvpGearMgr::Load (unknown names are logged there)
    std::string TierOrder = "Hateful/Savage,Deadly,Furious,Relentless,Wrathful";
    // The tier every ladder bot starts a season with; empty = the first tier
    std::string BaseTier;
    // Ladder bots keep their PvP loadout on between matches, as on an arena server (lab fixtures)
    bool WearBetweenMatches = false;
    // Buying upgrades: on at Ladder.Role = 2 (PlayerbotAIConfig::ApplyLadderRole); elsewhere a test toggle
    bool BuyEnable = false;
    // The slots in the order the bots buy for them; slots left out follow in the default order. Parsed and checked by
    // PvpGearMgr::Load (unknown names are logged there)
    std::string BuySlotPriority =
        "weapon,chest,legs,head,shoulders,hands,feet,waist,wrists,back,neck,rings,trinkets,ranged";
    bool BuySaveTowardBetter = true;      // save toward the best upgrade the rating allows
    uint32 BuyCheckIntervalMinutes = 60;  // 1-1440: minutes between two checks of every online ladder bot
    uint32 BuyBatchPerTick = 5;           // 1-100: bots checked per world tick
};

#endif
