/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSECONFIG_H
#define _PLAYERBOT_ITEMUSECONFIG_H

#include "Define.h"

// The tuning values of the item use framework (Playerbots.ItemUse.*), read once at config load
// (PlayerbotAIConfig::ItemUse); the initial values are the conf.dist defaults. Kept apart from PlayerbotAIConfig.h so
// that later members do not rebuild every user of that header.
struct ItemUseConfig
{
    // Percent (0..100) of persistent bots in the control cohort (ItemUse::InControlCohort): they spend by the old
    // rules. 0: none
    uint32 ControlShare = 0;
    // Milliseconds (0..10000) a timed use waits past its cast time for a sign of the cast before it ends as bound
    uint32 UseEndGraceMs = 1500;
};

template <typename Struct>
struct ConfigFamily;

// The config table family (ItemUseConfig.cpp)
extern ConfigFamily<ItemUseConfig> const ItemUseFamily;

// The one instance (ItemUseConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.ItemUse.
ItemUseConfig& ItemUseConfigStorage();

#endif
