/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WorldLog.h"

#include <atomic>
#include <chrono>
#include <vector>

#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "WorldLogSelftest.h"

namespace
{
constexpr uint32 DEFAULT_REALM_ID = 1;

std::atomic<bool> s_active{false};
std::string s_dir;  // written once at startup, before any reader
std::atomic<uint32> s_stateSeconds{WorldLogFormat::Settings{}.stateSeconds};
std::atomic<uint32> s_segmentMinutes{WorldLogFormat::Settings{}.segmentMinutes};
std::atomic<uint32> s_queueRows{WorldLogFormat::Settings{}.queueRows};
std::atomic<uint32> s_realmId{DEFAULT_REALM_ID};
bool s_dirChangeLogged = false;  // world thread only
}  // namespace

void WorldLog::LoadConfig(bool reload)
{
    WorldLogFormat::Settings const defaults;
    WorldLogFormat::Settings const settings = WorldLogFormat::ClampSettings(
        sConfigMgr->GetOption<uint32>("Playerbots.WorldLog.StateSeconds", defaults.stateSeconds),
        sConfigMgr->GetOption<uint32>("Playerbots.WorldLog.SegmentMinutes", defaults.segmentMinutes),
        sConfigMgr->GetOption<uint32>("Playerbots.WorldLog.QueueRows", defaults.queueRows));
    s_stateSeconds.store(settings.stateSeconds, std::memory_order_relaxed);
    s_segmentMinutes.store(settings.segmentMinutes, std::memory_order_relaxed);
    s_queueRows.store(settings.queueRows, std::memory_order_relaxed);

    std::string const dir = sConfigMgr->GetOption<std::string>("Playerbots.WorldLog.Dir", "");
    if (!reload)
    {
        s_dir = dir;
        s_realmId.store(sConfigMgr->GetOption<uint32>("RealmID", DEFAULT_REALM_ID), std::memory_order_relaxed);
        s_active.store(!s_dir.empty(), std::memory_order_relaxed);
    }
    else if (dir != s_dir && !s_dirChangeLogged)
    {
        s_dirChangeLogged = true;
        LOG_WARN(
            "playerbots",
            "[WorldLog] reload: Playerbots.WorldLog.Dir = \"{}\" is ignored until restart; the log stays in \"{}\"",
            dir, s_dir);
    }

    LOG_INFO("playerbots", "[WorldLog] {}Dir = \"{}\"{}, StateSeconds = {}, SegmentMinutes = {}, QueueRows = {}",
             reload ? "reload: " : "", s_dir, Active() ? "" : " (off)", settings.stateSeconds, settings.segmentMinutes,
             settings.queueRows);
}

bool WorldLog::Active() { return s_active.load(std::memory_order_relaxed); }

std::string const& WorldLog::Dir() { return s_dir; }

WorldLogFormat::Settings WorldLog::CurrentSettings()
{
    WorldLogFormat::Settings settings;
    settings.stateSeconds = s_stateSeconds.load(std::memory_order_relaxed);
    settings.segmentMinutes = s_segmentMinutes.load(std::memory_order_relaxed);
    settings.queueRows = s_queueRows.load(std::memory_order_relaxed);
    return settings;
}

uint32 WorldLog::RealmId() { return s_realmId.load(std::memory_order_relaxed); }

uint64 WorldLog::NowMs()
{
    auto const sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count());
}

bool WorldLog::HandleSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    std::vector<std::string> failures;
    uint32 const checks = WorldLogFormat::Selftest(failures);
    for (std::string const& failure : failures)
        handler->PSendSysMessage("WORLDLOG selftest FAIL: {}", failure);

    handler->PSendSysMessage("WORLDLOG selftest: {} checks, {} failures{}", checks, failures.size(),
                             failures.empty() ? " (PASS)" : "");
    return true;
}

class WorldLogWorldScript : public WorldScript
{
public:
    WorldLogWorldScript() : WorldScript("WorldLogWorldScript", {WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_AFTER_CONFIG_LOAD})
    {
    }

    void OnStartup() override { WorldLog::LoadConfig(false); }

    // The initial (reload == false) call comes before the world is set up; OnStartup covers it.
    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            WorldLog::LoadConfig(true);
    }
};

void AddSC_world_log() { new WorldLogWorldScript(); }
