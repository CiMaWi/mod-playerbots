/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CensusMgr.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <shared_mutex>
#include <utility>

#include "Config.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "StringFormat.h"

namespace
{
constexpr uint32 MS_PER_SECOND = 1000;
constexpr float PERCENT = 100.0f;
constexpr size_t BYTES_PER_ROW = 160;
constexpr char const* LOCATIONS_FILE = "bot_locations.csv";
constexpr char const* EVENTS_FILE = "bot_events.csv";

// The cheap, state-free columns of one sample row (the part after the time and the type). Columns, in order: name,
// "POINT(y x)", orientation, race, class, map, level, health, power%, money, grouper, guilder, allowActivity,
// in-world, combat, safe (not evaluated: n/a), alive, then the appended zoneId, areaId, inCombat.
void AppendRow(std::string& out, std::string const& time, char const* type, Player* player, PlayerbotAI* ai)
{
    Powers const powerType = player->getPowerType();
    uint32 const maxPower = std::max<uint32>(1, player->GetMaxPower(powerType));
    float const powerPercent = PERCENT * static_cast<float>(player->GetPower(powerType)) / static_cast<float>(maxPower);
    uint32 const grouper = ai ? static_cast<uint32>(ai->GetGrouperType()) : 0;
    uint32 const guilder = ai ? static_cast<uint32>(ai->GetGuilderType()) : 0;
    bool const allow = ai ? ai->IsActivityAllowedCached() : true;
    bool const combat = player->IsInCombat();

    out += Acore::StringFormat(
        "{},{},{},\"POINT({:.2f} {:.2f})\",{:.2f},{},{},{},{},{},{:.2f},{},{},{},{},{},{},n/a,{},"
        "{},{},{}\n",
        time, type, player->GetName(), player->GetPositionY(), player->GetPositionX(), player->GetOrientation(),
        static_cast<uint32>(player->getRace()), static_cast<uint32>(player->getClass()), player->GetMapId(),
        static_cast<uint32>(player->GetLevel()), player->GetHealth(), powerPercent, player->GetMoney(), grouper,
        guilder, allow ? "active" : "inactive", player->IsInWorld() ? "active" : "delay",
        combat ? "combat" : "non-combat", player->IsAlive() ? "alive" : "dead", player->GetZoneId(),
        player->GetAreaId(), combat ? 1 : 0);
}

// Level-up hook; registered once, does nothing while the census is off
class CensusPlayerScript : public PlayerScript
{
public:
    CensusPlayerScript() : PlayerScript("CensusPlayerScript", {PLAYERHOOK_ON_LEVEL_CHANGED}) {}

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        CensusMgr::Instance().OnLevelChanged(player, oldLevel);
    }
};
}  // namespace

CensusMgr::~CensusMgr() { Shutdown(); }

std::string CensusMgr::TimeString()
{
    std::time_t const now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S+00", &utc);
    return buffer;
}

void CensusMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.CensusEnable)
        return;

    _elapsed += diff;
    if (_started && _elapsed < sPlayerbotAIConfig.CensusIntervalSeconds * MS_PER_SECOND)
        return;

    _elapsed = 0;
    if (!_started)
        StartWriter();

    Sample();
}

void CensusMgr::Sample()
{
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "Census");

    std::string const time = TimeString();
    std::string text;
    {
        // The players are only read here, on the world thread, while no map update runs
        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        auto const& players = ObjectAccessor::GetPlayers();
        text.reserve(players.size() * BYTES_PER_ROW);
        for (auto const& pair : players)
        {
            Player* player = pair.second;
            if (!player || !player->IsInWorld())
                continue;

            PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
            AppendRow(text, time, ai ? "RND" : "PLR", player, ai);
        }
    }

    if (!text.empty())
        Enqueue(false, std::move(text));

    if (pmo)
        pmo->finish();
}

void CensusMgr::OnLevelChanged(Player* player, uint8 oldLevel)
{
    if (!sPlayerbotAIConfig.CensusEnable || !player || !GET_PLAYERBOT_AI(player))
        return;

    // spp-map bot_events.csv: time, name, event, "POINT(y x)", race, class, level, "info 1", "info 2"
    Enqueue(true, Acore::StringFormat("{},{},LevelUp,\"POINT({:.2f} {:.2f})\",{},{},{:.2f},\"{}\",\"{}\"\n",
                                      TimeString(), player->GetName(), player->GetPositionY(), player->GetPositionX(),
                                      static_cast<uint32>(player->getRace()), static_cast<uint32>(player->getClass()),
                                      static_cast<float>(player->GetLevel()), static_cast<uint32>(oldLevel),
                                      static_cast<uint32>(player->GetLevel())));
}

void CensusMgr::Enqueue(bool events, std::string text)
{
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_stop)
            return;

        _queue.push_back(Chunk{events, std::move(text)});
    }
    _wake.notify_one();
}

void CensusMgr::StartWriter()
{
    _started = true;

    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir.push_back('/');
    dir += "census/";

    _writer = std::thread(&CensusMgr::WriterLoop, this, std::move(dir));
    LOG_INFO("playerbots", "Census on: every {} s into {}", sPlayerbotAIConfig.CensusIntervalSeconds, LOCATIONS_FILE);
}

// Writer thread: owns the two files and touches nothing but strings from the queue. Files are truncated at start.
void CensusMgr::WriterLoop(std::string dir)
{
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    std::ofstream locations(dir + LOCATIONS_FILE, std::ios::out | std::ios::trunc);
    std::ofstream events(dir + EVENTS_FILE, std::ios::out | std::ios::trunc);
    if (!locations || !events)
        LOG_ERROR("playerbots", "Census: cannot open the files in {}; the census is discarded", dir);

    std::vector<Chunk> batch;
    bool stop = false;
    while (!stop)
    {
        {
            std::unique_lock<std::mutex> guard(_mutex);
            _wake.wait(guard, [this] { return _stop || !_queue.empty(); });
            batch.swap(_queue);
            stop = _stop;
        }

        for (Chunk const& chunk : batch)
        {
            std::ofstream& file = chunk.events ? events : locations;
            if (file)
                file << chunk.text;
        }

        locations.flush();
        events.flush();
        batch.clear();
    }
}

void CensusMgr::Shutdown()
{
    {
        std::lock_guard<std::mutex> guard(_mutex);
        _stop = true;
    }
    _wake.notify_all();
    if (_writer.joinable())
        _writer.join();
}

void AddSC_census() { new CensusPlayerScript(); }
