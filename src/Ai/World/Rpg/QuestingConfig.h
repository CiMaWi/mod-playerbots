/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTINGCONFIG_H
#define _PLAYERBOT_QUESTINGCONFIG_H

#include "Define.h"

template <typename Struct>
struct ConfigFamily;

// The numbers of the quest executor (Playerbots.Questing.*), read once at config load (PlayerbotAIConfig::Questing)
// through the rows in QuestingConfig.cpp; the initial values are the defaults. Tuning values the nights read
struct QuestingConfig
{
    // GiveUpMinutes (1-120): minutes of a quest order without progress before it is given up (PursuitRules.h); the
    // clock runs only while the executor runs the order and restarts at every progress
    uint32 GiveUpMinutes = 10;
};

extern ConfigFamily<QuestingConfig> const QuestingFamily;

// The one instance (QuestingConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.Questing.
QuestingConfig& QuestingConfigStorage();

#endif
