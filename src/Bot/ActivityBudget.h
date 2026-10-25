/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ACTIVITYBUDGET_H
#define _PLAYERBOT_ACTIVITYBUDGET_H

#include <atomic>

#include "Define.h"

class ChatHandler;
class Player;

// The situation of a real player, ordered from the loosest to the strictest budget. The values index the per-situation
// options (PlayerbotAIConfig::SituationDiffLimitFloor/Ceiling).
enum class BotSituation : uint8
{
    OpenWorld = 0,
    Dungeon = 1,
    Boss = 2,
    Pvp = 3,
    Count = 4
};

// The tick time (ms) smart scale (AutoScaleActivity) follows. With Playerbots.botActiveAloneSmartScalePercentile = 0 it
// is the core's maximum over its last ticks, read directly. Otherwise Update() stores that percentile of the core's
// last-500-tick table once per world tick: the core sorts a shared array without a lock, so only the world thread may
// call it, and map threads only read the atomic.
//
// Situation budgets (Playerbots.Situation.Enable): every Playerbots.Situation.IntervalMs, Update() classifies the real
// players online (PvP, boss fight, dungeon or raid, open world); the strictest situation present selects smart scale's
// floor and ceiling, published in atomics. With nobody online, or with the option off, the global values apply.
class ActivityBudget
{
public:
    static ActivityBudget& Instance()
    {
        static ActivityBudget instance;
        return instance;
    }

    // World thread only (world OnUpdate)
    void Update();

    // Any thread
    uint32 GetTickMs() const;
    uint32 GetDiffLimitFloor() const;
    uint32 GetDiffLimitCeiling() const;

    // World thread only: the situation of one player (PvP, boss fight, dungeon or raid, open world)
    static BotSituation Classify(Player* player);
    static char const* SituationName(BotSituation situation);

    // .playerbots situation status | force <pvp|boss|dungeon|open|off>
    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleForceCommand(ChatHandler* handler, char const* args);

private:
    ActivityBudget() = default;

    // World thread only: classifies the real players and publishes the effective floor and ceiling
    void ClassifyPlayers();
    // The pinned situation: the command's if one was given, else Playerbots.Situation.Force; -1 = detect
    int32 GetForce() const;

    static constexpr int32 NoSituation = -1;
    static constexpr int32 CommandForceUnset = -2;

    std::atomic<uint32> _tickMs{0};
    std::atomic<uint32> _floorMs{0};
    std::atomic<uint32> _ceilingMs{0};
    std::atomic<int32> _situation{NoSituation};

    // World thread only
    uint32 _lastClassifyMs = 0;
    bool _classified = false;
    int32 _commandForce = CommandForceUnset;
    uint32 _playerCount = 0;
};

#endif
