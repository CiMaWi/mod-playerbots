/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Population.h"

#include <atomic>
#include <vector>

#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "PopulationPolicy.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"

namespace
{
std::atomic<bool> s_persistent{false};
}

bool Population::IsPersistent() { return s_persistent.load(std::memory_order_relaxed); }

void Population::LoadConfig(bool reload)
{
    bool const persistent = sConfigMgr->GetOption<bool>("Playerbots.Population.Persistent", false);
    s_persistent.store(persistent, std::memory_order_relaxed);

    LOG_INFO("playerbots", "[Population] {}Playerbots.Population.Persistent = {}{}", reload ? "reload: " : "",
             persistent ? 1 : 0, persistent ? " (bot levels never go down)" : "");

    // Read from sConfigMgr, not PlayerbotAIConfig: on ".reload config" the level-management values there are
    // reloaded by another script whose order against this one is not fixed.
    std::vector<char const*> const refused = PopulationPolicy::RefusedOptions(
        persistent, sConfigMgr->GetOption<bool>("Playerbots.DowngradeMaxLevelBot", false),
        sConfigMgr->GetOption<bool>("Playerbots.ResetBotLevel.Enabled", false),
        sConfigMgr->GetOption<bool>("Playerbots.LevelBrackets.Enabled", false));
    for (char const* option : refused)
        LOG_WARN("playerbots",
                 "[Population] {} = 1 is refused while Playerbots.Population.Persistent = 1: "
                 "it would lower bot levels; set it to 0",
                 option);
}

uint32 Population::GuardLevel(Player* bot, uint32 requestedLevel, char const* site)
{
    if (!bot || !IsPersistent())
        return requestedLevel;

    uint32 const currentLevel = bot->GetLevel();
    if (PopulationPolicy::KeepLevel(true, currentLevel, requestedLevel) == requestedLevel)
        return requestedLevel;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return requestedLevel;

    LOG_WARN("playerbots", "[Population] {}: refused to lower {} from level {} to {}, kept {}", site, bot->GetName(),
             currentLevel, requestedLevel, currentLevel);

    return currentLevel;
}

class PopulationWorldScript : public WorldScript
{
public:
    PopulationWorldScript()
        : WorldScript("PopulationWorldScript", {WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_AFTER_CONFIG_LOAD})
    {
    }

    void OnStartup() override { Population::LoadConfig(false); }

    // The initial (reload == false) call comes before the world is set up; OnStartup covers it.
    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            Population::LoadConfig(true);
    }
};

void AddSC_population() { new PopulationWorldScript(); }
