/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpTestRecord.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <utility>

#include "Config.h"
#include "Log.h"
#include "StringFormat.h"

namespace
{
constexpr uint32 SchemaVersion = 1;
constexpr size_t DateLength = 10;  // YYYY-MM-DD at the start of the ISO time
constexpr size_t ExpectedLineBytes = 1600;
constexpr uint8 MaxClassId = 11;
constexpr unsigned char FirstPrintable = 0x20;

std::array<char const*, MaxClassId + 1> const ClassNames = {
    "", "warrior", "paladin", "hunter", "rogue", "priest", "deathknight", "shaman", "mage", "warlock", "", "druid"};

// A JSON string literal: quotes, backslash and control characters escaped, UTF-8 passed through
std::string Quote(std::string const& text)
{
    std::string quoted = "\"";
    for (char const c : text)
    {
        switch (c)
        {
            case '"':
                quoted += "\\\"";
                break;
            case '\\':
                quoted += "\\\\";
                break;
            case '\n':
                quoted += "\\n";
                break;
            case '\r':
                quoted += "\\r";
                break;
            case '\t':
                quoted += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < FirstPrintable)
                    quoted += Acore::StringFormat("\\u{:04x}", static_cast<uint32>(static_cast<unsigned char>(c)));
                else
                    quoted += c;
        }
    }

    quoted += '"';
    return quoted;
}

std::string BotToJson(PvpTestBotRecord const& bot)
{
    // skill is null when Playerbots.Skill.Enable is 0 or the player is not a bot
    return Acore::StringFormat(
        "{{\"guid\":{},\"name\":{},\"class\":{},\"classId\":{},\"race\":{},\"level\":{},\"spec\":{},\"gearScore\":{},"
        "\"skill\":{},\"entered\":{},\"damage\":{},\"healing\":{},\"killingBlows\":{},\"deaths\":{},"
        "\"honorableKills\":{}}}",
        bot.Guid, Quote(bot.Name), Quote(PvpTestRecordWriter::ClassName(bot.ClassId)), static_cast<uint32>(bot.ClassId),
        static_cast<uint32>(bot.RaceId), static_cast<uint32>(bot.Level), Quote(bot.Spec), bot.GearScore,
        bot.Skill < 0 ? std::string("null") : std::to_string(bot.Skill), bot.Entered ? "true" : "false", bot.Damage,
        bot.Healing, bot.KillingBlows, bot.Deaths, bot.HonorableKills);
}

std::string SideToJson(PvpTestSideRecord const& side)
{
    // comp: the sorted classes and specs of the side, the key the summaries group by
    std::vector<std::string> comp;
    std::string bots;
    for (PvpTestBotRecord const& bot : side.Bots)
    {
        std::string entry = PvpTestRecordWriter::ClassName(bot.ClassId);
        if (!bot.Spec.empty())
            entry += "/" + bot.Spec;

        comp.push_back(std::move(entry));
        bots += (bots.empty() ? "" : ",") + BotToJson(bot);
    }

    std::sort(comp.begin(), comp.end());
    std::string compJson;
    for (std::string const& entry : comp)
        compJson += (compJson.empty() ? "" : ",") + Quote(entry);

    return Acore::StringFormat("{{\"label\":{},\"side\":{},\"teamId\":{},\"faction\":{},\"comp\":[{}],\"bots\":[{}]}}",
                               Quote(side.Label), Quote(std::string(1, side.Side)), side.TeamId, Quote(side.Faction),
                               compJson, bots);
}
}  // namespace

PvpTestRecordWriter::~PvpTestRecordWriter() { Shutdown(); }

std::string PvpTestRecordWriter::ClassName(uint8 classId) { return classId <= MaxClassId ? ClassNames[classId] : ""; }

std::string PvpTestRecordWriter::IsoTimeNow()
{
    std::time_t const now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string PvpTestRecordWriter::Directory()
{
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir.push_back('/');

    return dir + "pvptest/";
}

std::string PvpTestRecordWriter::TodayFile()
{
    std::string date = IsoTimeNow().substr(0, DateLength);
    date.erase(std::remove(date.begin(), date.end(), '-'), date.end());
    return Directory() + "matches-" + date + ".jsonl";
}

std::string PvpTestRecordWriter::ToJson(PvpTestMatchRecord const& record)
{
    std::string json;
    json.reserve(ExpectedLineBytes);
    json += Acore::StringFormat(
        "{{\"schema\":{},\"kind\":{},\"id\":{},\"plan\":{},\"index\":{},\"time\":{},\"seed\":{},\"map\":{},"
        "\"arenaType\":{},\"rated\":{},\"outcome\":{},\"winner\":{},\"winnerSide\":{},\"reason\":{},"
        "\"failedStage\":{},\"fightMs\":{},\"totalMs\":{},\"sides\":[",
        SchemaVersion, Quote(record.Kind), Quote(record.Id), Quote(record.PlanId), record.Index,
        Quote(record.StartTime), record.Seed, Quote(record.Map), static_cast<uint32>(record.ArenaType),
        record.Rated ? "true" : "false", Quote(record.Outcome), Quote(record.Winner), Quote(record.WinnerSide),
        Quote(record.Reason), Quote(record.FailedStage), record.FightMs, record.TotalMs);
    json += SideToJson(record.Sides[0]) + "," + SideToJson(record.Sides[1]) + "]}";
    return json;
}

void PvpTestRecordWriter::Write(PvpTestMatchRecord const& record)
{
    std::string date = record.StartTime.substr(0, DateLength);
    date.erase(std::remove(date.begin(), date.end(), '-'), date.end());
    std::string text = ToJson(record) + "\n";
    bool start = false;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_stop)
            return;

        start = !_writer.joinable();
        _queue.push_back(Line{std::move(date), std::move(text)});
    }

    if (start)
        _writer = std::thread(&PvpTestRecordWriter::WriterLoop, this, Directory());

    _wake.notify_one();
}

// Writer thread: appends the queued lines, one open per batch and file, and flushes
void PvpTestRecordWriter::WriterLoop(std::string directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        LOG_ERROR("playerbots", "PVPTEST: cannot create {}; match records may be lost", directory);

    std::vector<Line> batch;
    bool stop = false;
    while (!stop)
    {
        {
            std::unique_lock<std::mutex> guard(_mutex);
            _wake.wait(guard, [this] { return _stop || !_queue.empty(); });
            batch.swap(_queue);
            stop = _stop;
        }

        for (Line const& line : batch)
        {
            std::ofstream file(directory + "matches-" + line.Date + ".jsonl", std::ios::out | std::ios::app);
            if (file)
                file << line.Text;
            else
                LOG_ERROR("playerbots", "PVPTEST: cannot append to the match record file in {}", directory);
        }

        batch.clear();
    }
}

void PvpTestRecordWriter::Shutdown()
{
    {
        std::lock_guard<std::mutex> guard(_mutex);
        _stop = true;
    }
    _wake.notify_all();
    if (_writer.joinable())
        _writer.join();
}
