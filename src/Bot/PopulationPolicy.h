/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_POPULATIONPOLICY_H
#define _PLAYERBOT_POPULATIONPOLICY_H

#include <cstdint>
#include <vector>

// Pure rules of the persistent population (Playerbots.Population.*), no game types, so a standalone
// harness can check them. Population.cpp and the call sites apply them to live bots.
namespace PopulationPolicy
{
// Why persistence turned a randomize into an incremental one at the bot's current level.
enum class KeptReason : uint8_t
{
    None,      // nothing kept: the upstream rule would not have changed the level either
    Reroll,    // upstream rerolls bots below level 3 (Death Knights below 56) to a random level
    Downgrade  // upstream sends a max-level bot to the min level (Playerbots.DowngradeMaxLevelBot)
};

struct RandomizeDecision
{
    bool first;       // true: RandomizeFirst (full reroll at a new level); false: incremental at the current level
    KeptReason kept;  // only set when persistent
};

// RandomPlayerbotMgr::Randomize's choice. Off: upstream's rule unchanged. On: always incremental at the current
// level, so no bot is rerolled (also not below level 3) and no max-level bot is downgraded.
inline RandomizeDecision DecideRandomize(bool persistent, uint32_t level, bool deathKnight, uint32_t maxLevel,
                                         bool downgradeMaxLevelBot)
{
    bool const reroll = level < 3 || (level < 56 && deathKnight);
    bool const downgrade = !reroll && level >= maxLevel && downgradeMaxLevelBot;

    if (!persistent)
        return {reroll || downgrade, KeptReason::None};

    return {false, reroll ? KeptReason::Reroll : (downgrade ? KeptReason::Downgrade : KeptReason::None)};
}

// RandomizeFirst's level: the rolled one, or the bot's current level when persistent (a re-initialisation at the
// same level; a new character so stays at its creation level).
inline uint32_t FirstLevel(bool persistent, uint32_t currentLevel, uint32_t rolledLevel)
{
    return persistent ? currentLevel : rolledLevel;
}

// The level a factory or reset path may set: never below the current one when persistent.
inline uint32_t KeepLevel(bool persistent, uint32_t currentLevel, uint32_t requestedLevel)
{
    return (persistent && requestedLevel < currentLevel) ? currentLevel : requestedLevel;
}

// RandomBotLevelMgr (level brackets and level reset) runs only without persistence.
inline bool LevelMgrAllowed(bool persistent) { return !persistent; }

// The level-lowering options persistence refuses, for the startup and reload warning.
inline std::vector<char const*> RefusedOptions(bool persistent, bool downgradeMaxLevelBot, bool resetBotLevel,
                                               bool levelBrackets)
{
    std::vector<char const*> refused;
    if (!persistent)
        return refused;

    if (downgradeMaxLevelBot)
        refused.push_back("Playerbots.DowngradeMaxLevelBot");
    if (resetBotLevel)
        refused.push_back("Playerbots.ResetBotLevel.Enabled");
    if (levelBrackets)
        refused.push_back("Playerbots.LevelBrackets.Enabled");

    return refused;
}

inline char const* KeptReasonName(KeptReason reason)
{
    switch (reason)
    {
        case KeptReason::Reroll:
            return "reroll";
        case KeptReason::Downgrade:
            return "downgrade";
        default:
            return "none";
    }
}
}  // namespace PopulationPolicy

#endif
