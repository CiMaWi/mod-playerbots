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
constexpr size_t ExpectedLineBytes = 2800;
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

// A count that is null when unknown (-1)
std::string NumberOrNull(int32 value) { return value < 0 ? std::string("null") : std::to_string(value); }

// {"fear":2,...} with the non-zero DR groups only
std::string DrGroupsToJson(std::array<uint32, PVPTEST_DR_GROUP_COUNT> const& counts)
{
    std::string json = "{";
    for (uint32 group = 0; group < PVPTEST_DR_GROUP_COUNT; ++group)
    {
        if (!counts[group])
            continue;

        if (json.size() > 1)
            json += ',';

        json += Acore::StringFormat("\"{}\":{}", PVPTEST_DR_GROUP_NAMES[group], counts[group]);
    }

    json += '}';
    return json;
}

// The cc and tactics objects of a bot (G0b); the tactics object lists every named counter, zero included
std::string CountersToJson(PvpTestBotRecord const& bot)
{
    PvpTestCcCounters const& cc = bot.Cc;
    std::string json = Acore::StringFormat(
        "\"cc\":{{\"cast\":{},\"atImmune\":{},\"received\":{},\"ownBreaks\":{},\"trinket\":{},\"racialBreak\":{},"
        "\"partnerDispel\":{},\"lostEarly\":{}}},\"tactics\":{{",
        DrGroupsToJson(cc.Cast), cc.AtImmune, DrGroupsToJson(cc.Received), cc.OwnBreaks, cc.Trinket, cc.RacialBreak,
        cc.PartnerDispel, cc.LostEarly);
    for (uint32 index = 0; index < PVPTEST_TACTIC_COUNT; ++index)
    {
        if (index)
            json += ',';

        json += Acore::StringFormat("\"{}\":{}", PVPTEST_TACTIC_NAMES[index], bot.Tactics[index]);
    }

    json += '}';
    return json;
}

std::string BotToJson(PvpTestBotRecord const& bot)
{
    // skill is null when Playerbots.Skill.Enable is 0 or the player is not a bot
    return Acore::StringFormat(
        "{{\"guid\":{},\"name\":{},\"class\":{},\"classId\":{},\"race\":{},\"level\":{},\"spec\":{},\"gearScore\":{},"
        "\"skill\":{},\"entered\":{},\"damage\":{},\"healing\":{},\"killingBlows\":{},\"deaths\":{},"
        "\"honorableKills\":{},\"casts\":{{\"interrupt\":{},\"defensive\":{},\"dispel\":{},\"crowdControl\":{},"
        "\"ccBreak\":{},\"targetSwitch\":{}}},{},\"real\":{},\"honor\":{},\"personalBefore\":{},"
        "\"personalAfter\":{},\"mmrChange\":{}}}",
        bot.Guid, Quote(bot.Name), Quote(PvpTestRecordWriter::ClassName(bot.ClassId)), static_cast<uint32>(bot.ClassId),
        static_cast<uint32>(bot.RaceId), static_cast<uint32>(bot.Level), Quote(bot.Spec), bot.GearScore,
        bot.Skill < 0 ? std::string("null") : std::to_string(bot.Skill), bot.Entered ? "true" : "false", bot.Damage,
        bot.Healing, bot.KillingBlows, bot.Deaths, bot.HonorableKills, bot.Casts[0], bot.Casts[1], bot.Casts[2],
        bot.Casts[3], bot.Casts[4], bot.Casts[5], CountersToJson(bot), bot.Real ? "true" : "false", bot.Honor,
        NumberOrNull(bot.PersonalBefore), NumberOrNull(bot.PersonalAfter),
        bot.MmrChangeKnown ? std::to_string(bot.MmrChange) : std::string("null"));
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

    std::string forcedComp;
    for (std::string const& entry : side.ForcedComp)
        forcedComp += (forcedComp.empty() ? "" : ",") + Quote(entry);

    return Acore::StringFormat(
        "{{\"label\":{},\"side\":{},\"teamId\":{},\"faction\":{},\"comp\":[{}],\"bots\":[{}],"
        "\"teamName\":{},\"teamRatingBefore\":{},\"teamRatingAfter\":{},\"mmrBefore\":{},\"mmrAfter\":{},"
        "\"forced\":{{\"comp\":[{}],\"skill\":{}}}}}",
        Quote(side.Label), Quote(std::string(1, side.Side)), side.TeamId, Quote(side.Faction), compJson, bots,
        Quote(side.TeamName), NumberOrNull(side.TeamRatingBefore), NumberOrNull(side.TeamRatingAfter),
        NumberOrNull(side.MmrBefore), NumberOrNull(side.MmrAfter), forcedComp, NumberOrNull(side.ForcedSkill));
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
        "\"failedStage\":{},\"fightMs\":{},\"totalMs\":{},\"server\":{},\"matchId\":{},\"source\":{},"
        "\"real\":{},\"ratingsKnown\":{},\"sides\":[",
        SchemaVersion, Quote(record.Kind), Quote(record.Id), Quote(record.PlanId), record.Index,
        Quote(record.StartTime), record.Seed, Quote(record.Map), static_cast<uint32>(record.ArenaType),
        record.Rated ? "true" : "false", Quote(record.Outcome), Quote(record.Winner), Quote(record.WinnerSide),
        Quote(record.Reason), Quote(record.FailedStage), record.FightMs, record.TotalMs, Quote(record.Server),
        Quote(record.MatchId), Quote(record.Source), record.Real ? "true" : "false",
        record.RatingsKnown ? "true" : "false");
    json += SideToJson(record.Sides[0]) + "," + SideToJson(record.Sides[1]) + "]}";
    return json;
}

void PvpTestRecordWriter::Write(PvpTestMatchRecord const& record)
{
    std::string date = record.StartTime.substr(0, DateLength);
    date.erase(std::remove(date.begin(), date.end(), '-'), date.end());
    WriteLine("matches", std::move(date), ToJson(record) + "\n");
}

std::string PvpTestRecordWriter::JsonQuote(std::string const& text) { return Quote(text); }

bool PvpTestRecordWriter::Append(std::string const& directory, Line& line)
{
    if (line.Make)
        line.Text = line.Make();

    std::ofstream file(directory + line.Prefix + "-" + line.Date + ".jsonl", std::ios::out | std::ios::app);
    if (!file)
    {
        ++_failed;
        return false;
    }

    file << line.Text;
    ++_written;
    return true;
}

void PvpTestRecordWriter::WriteLine(std::string prefix, std::string date, std::string text,
                                    std::function<std::string()> make)
{
    Line line{std::move(prefix), std::move(date), std::move(text), std::move(make)};
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_stop)
        {
            // A deliberate exception to "file I/O only on the writer thread" (C2): the arenas that are still running
            // when the server stops are ended by the shutdown, after Shutdown has stopped the writer, and their
            // records would be lost. Once per such arena, at the stop only. _fileMutex keeps this append and the
            // writer thread's last batch (it may still be draining) from writing the file at the same time.
            std::lock_guard<std::mutex> fileGuard(_fileMutex);
            std::string const directory = Directory();
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            Append(directory, line);
            return;
        }

        // Started under the lock: arena map threads can write the first records at the same time
        if (!_writer.joinable())
            _writer = std::thread(&PvpTestRecordWriter::WriterLoop, this, Directory());

        _queue.push_back(std::move(line));
        if (_queue.size() > _queueHigh.load())
            _queueHigh.store(_queue.size());
    }

    _wake.notify_one();
}

size_t PvpTestRecordWriter::QueueSize()
{
    std::lock_guard<std::mutex> guard(_mutex);
    return _queue.size();
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

        // Held for the batch: a record written after the stop appends under the same lock (see Write)
        std::lock_guard<std::mutex> fileGuard(_fileMutex);
        for (Line& line : batch)
        {
            if (!Append(directory, line))
                LOG_ERROR("playerbots", "PVPTEST: cannot append to the {} file in {}", line.Prefix, directory);
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
