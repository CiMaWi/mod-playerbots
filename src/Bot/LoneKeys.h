/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LONEKEYS_H
#define _PLAYERBOT_LONEKEYS_H

#include <string>

#include "Define.h"

// The keys that stand alone under Playerbots. (no family prefix), one row each in LoneKeys.cpp
// (PlayerbotAIConfig::Lone), read once at config load.

struct LoneKeys
{
    // Playerbots.BotSightRangeCap (0..): each bot's own sight range on world maps (core Player::SetSightRangeCap), set
    // at login; 0 = off
    float SightRangeCap = 0.0f;
    // Playerbots.RandomBotAutoJoinRatedArenaQueueSlots (1..): up to that many matches' worth of captains may queue
    int32 RatedQueueSlots = 1;
    // Playerbots.RandomBotAutoJoinRatedArenaUncapped (test): every free bot team queues, no slot or count limit
    bool RatedQueueUncapped = false;
    // Playerbots.RandomBotTestNamePrefix (test): new random bots are named prefix + a letters-only counter (2-9
    // letters, stored with a capital first letter; empty = the name pool)
    std::string TestNamePrefix;
    // Playerbots.DebugThreadChecks (test): detect world-thread-only code running on another thread
    bool DebugThreadChecks = false;
    // Playerbots.Test.MissLog (test): log a skipped pre-gate step or check follower whose own guard passes
    // ("PreGate miss: <step>", "Live miss: <follower>")
    bool TestMissLog = false;
};

template <typename Struct>
struct ConfigFamily;

// Playerbots.BotSightRangeCap (LoneKeys.cpp)
extern ConfigFamily<LoneKeys> const SightRangeCapFamily;
// Playerbots.RandomBotAutoJoinRatedArenaQueueSlots
extern ConfigFamily<LoneKeys> const RatedQueueSlotsFamily;
// Playerbots.RandomBotAutoJoinRatedArenaUncapped
extern ConfigFamily<LoneKeys> const RatedQueueUncappedFamily;
// Playerbots.RandomBotTestNamePrefix
extern ConfigFamily<LoneKeys> const TestNamePrefixFamily;
// Playerbots.DebugThreadChecks
extern ConfigFamily<LoneKeys> const DebugThreadChecksFamily;
// Playerbots.Test.MissLog
extern ConfigFamily<LoneKeys> const TestMissLogFamily;

// The one instance (LoneKeys.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this: readers
// use sPlayerbotAIConfig.Lone.
LoneKeys& LoneKeysStorage();

#endif
