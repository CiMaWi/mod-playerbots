/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <atomic>
#include <functional>
#include <shared_mutex>

#include "Action.h"
#include "CensusMgr.h"
#include "Chat.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldLog.h"
#include "WorldLogBotState.h"

// WL8, the recorder's server side: the pushes from PlayerbotAI::UpdateAI (positions), Engine::ListenAndExecute
// (actions) and CensusMgr's world-log event rows (events), each on the bot's own thread (events also on the world
// thread, while no map update runs), and the dump of a due case into the writer's queue as one chunk of recorder rows.
// The rings, triggers and row format are Player-free in WorldLogRecorder.h. PerfMonitor rows: "WorldLog record" (one
// push), "WorldLog record dump" (one case), "WorldLog mark" (one .wlog mark).
namespace
{
using WorldLogFormat::Recorder;

static_assert(WorldLogFormat::RECORDER_RPG_IDLE == RPG_IDLE && WorldLogFormat::RECORDER_RPG_REST == RPG_REST,
              "the still watch's idle and rest statuses match NewRpgStatus");

constexpr char const* CONSOLE_CASE = "console";
constexpr char const* MARK_CASE = "user";

std::atomic<uint64> s_caseIds{0};
std::atomic<uint64> s_rings{0};
std::atomic<uint64> s_cases{0};
std::atomic<uint64> s_caseRows{0};
std::atomic<uint64> s_caseBytes{0};
std::atomic<uint64> s_casesDropped{0};

WorldLogFormat::WordTable& Words()
{
    static WorldLogFormat::WordTable words;
    return words;
}

// Holds the recorder's busy flag for one push or dump; a second thread finding it held skips its push
class RecorderEntry
{
public:
    explicit RecorderEntry(Recorder& recorder) : _recorder(recorder), _entered(recorder.Enter()) {}
    ~RecorderEntry()
    {
        if (_entered)
            _recorder.Leave();
    }

    RecorderEntry(RecorderEntry const&) = delete;
    RecorderEntry& operator=(RecorderEntry const&) = delete;

    bool Entered() const { return _entered; }

private:
    Recorder& _recorder;
    bool const _entered;
};

WorldLogFormat::RecorderPosition PositionOf(Player* bot, uint32 nowMs, int32 rpgStatus)
{
    WorldLogFormat::RecorderPosition position;
    position.ms = nowMs;
    position.x = bot->GetPositionX();
    position.y = bot->GetPositionY();
    position.map = static_cast<uint16>(bot->GetMapId());
    position.rpg = static_cast<int16>(rpgStatus);
    return position;
}

// The due case as one chunk on the recorder stream (dropped whole and counted when it would pass the queue cap). A case
// counts as written only once the queue took it; a refused one counts as dropped.
void DumpCase(Player* bot, Recorder& recorder, uint32 nowMs)
{
    if (!recorder.CaseDue())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog record dump");

    uint64 const caseId = s_caseIds.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64 const wallMs = WorldLog::NowMs();
    std::string rows;
    uint32 const count = recorder.AppendCase(rows, Words(), caseId, bot->GetGUID().GetCounter(), nowMs, wallMs);
    size_t const bytes = rows.size();
    bool const queued =
        CensusMgr::Instance().EnqueueWorldLog(WorldLogFormat::Stream::Recorder, std::move(rows), count, wallMs);
    if (queued)
    {
        s_cases.fetch_add(1, std::memory_order_relaxed);
        s_caseRows.fetch_add(count, std::memory_order_relaxed);
        s_caseBytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    else
        s_casesDropped.fetch_add(1, std::memory_order_relaxed);

    LOG_DEBUG("playerbots", "[WorldLog] case {} of {}: {} rows, {} bytes{}", caseId, bot->GetName(), count, bytes,
              queued ? "" : ", dropped (queue full)");

    if (pmo)
        pmo->finish();
}
}  // namespace

void WorldLog::NotePosition(Player* bot, std::unique_ptr<BotLog>& log, int32_t rpgStatus)
{
    if (!bot)
        return;

    // The state and the rings are allocated (and the recorder's state set) before the busy flag is taken: safe only
    // because the world thread's events (NoteEvent) never run during a map update, so they cannot overlap this push
    if (!log)
        log = std::make_unique<BotLog>();

    Recorder& recorder = log->recorder;
    uint32 const nowMs = getMSTime();
    if (!recorder.PositionDue(nowMs))
        return;

    if (recorder.Unset())
    {
        if (!recorder.Allocate(CurrentRecorderSettings()))
            return;

        s_rings.fetch_add(1, std::memory_order_relaxed);
    }

    RecorderEntry const entry(recorder);
    if (!entry.Entered())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog record");
    bool const watched =
        WorldLogFormat::StillWatched(bot->IsAlive(), bot->IsInFlight(), bot->GetTransport() != nullptr, rpgStatus);
    bool const due = recorder.PushPosition(Words(), PositionOf(bot, nowMs, rpgStatus), watched);
    if (pmo)
        pmo->finish();

    if (due)
        DumpCase(bot, recorder, nowMs);
}

void WorldLog::NoteAction(Player* bot, std::unique_ptr<BotLog>& log, Action* action, std::string const& trigger,
                          bool result)
{
    if (!bot || !action || !log || !log->recorder.On())
        return;

    Recorder& recorder = log->recorder;
    RecorderEntry const entry(recorder);
    if (!entry.Entered())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog record");
    size_t const triggerHash = std::hash<std::string_view>{}(trigger);
    Recorder::ActionWords words;
    if (!recorder.CachedAction(action, triggerHash, words))
    {
        // A miss: getName() copies the name once per action object and trigger
        words.name = Words().Intern(action->getName());
        words.trigger = Words().Intern(trigger);
        recorder.CacheAction(action, triggerHash, words);
    }

    uint32 const nowMs = getMSTime();
    bool const due = recorder.PushAction(nowMs, words, result);
    if (pmo)
        pmo->finish();

    if (due)
        DumpCase(bot, recorder, nowMs);
}

void WorldLog::NoteEvent(Player* player, std::string_view type, std::string_view source, uint32_t id, int64_t amount)
{
    PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
    if (!botAI || !botAI->worldLog || !botAI->worldLog->recorder.On())
        return;

    Recorder& recorder = botAI->worldLog->recorder;
    RecorderEntry const entry(recorder);
    if (!entry.Entered())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog record");
    uint32 const nowMs = getMSTime();
    bool const due = recorder.PushEvent(Words(), nowMs, type, source, id, amount);
    if (pmo)
        pmo->finish();

    if (due)
        DumpCase(player, recorder, nowMs);
}

// Console commands run on the world thread while no map update runs, so reading the bot's state here does not race
// its thread; the request itself is an atomic the bot's next position push takes.
bool WorldLog::HandleCaseCommand(ChatHandler* handler, char const* args)
{
    if (!Active())
    {
        handler->PSendSysMessage("WORLDLOG off: Playerbots.WorldLog.Dir was empty at startup");
        return true;
    }

    std::string name = args ? args : "";
    name = name.substr(0, name.find(' '));
    if (name.empty() || !normalizePlayerName(name))
    {
        handler->PSendSysMessage("WORLDLOG case: usage: playerbots worldlog case <bot name>");
        return true;
    }

    Player* bot = ObjectAccessor::FindPlayerByName(name, false);
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
    {
        handler->PSendSysMessage("WORLDLOG case: no bot named {} is online", name);
        return true;
    }

    if (!botAI->worldLog || !botAI->worldLog->recorder.On())
    {
        handler->PSendSysMessage("WORLDLOG case: {} has no recorder ring (not updated yet, or RecorderSeconds = 0)",
                                 name);
        return true;
    }

    botAI->worldLog->recorder.RequestCase(CONSOLE_CASE);
    handler->PSendSysMessage(
        "WORLDLOG case: requested for {} (guid {}): written at its next position push (trigger mark, detail {})", name,
        bot->GetGUID().GetCounter(), CONSOLE_CASE);
    return true;
}

// In game, so on the world thread while no map update runs, like the console commands: the bots' places are read
// here and each request is an atomic the bot's next position push takes
bool WorldLog::HandleMarkCommand(ChatHandler* handler, char const* args)
{
    if (!Active())
    {
        handler->PSendSysMessage("WORLDLOG off: Playerbots.WorldLog.Dir was empty at startup");
        return true;
    }

    Player* marker = handler->GetPlayer();
    if (!marker || !marker->IsInWorld())
    {
        handler->PSendSysMessage("WORLDLOG mark: only from a character in the world");
        return true;
    }

    std::string_view const text = WorldLogFormat::MarkUserText(args ? args : "");
    if (text.empty())
    {
        handler->PSendSysMessage("WORLDLOG mark: usage: .wlog mark <text> (a case of each bot within {} yd)",
                                 static_cast<uint32>(WorldLogFormat::MARK_YARDS));
        return true;
    }

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "WorldLog mark");
    uint64 const t = NowMs();
    WorldLogFormat::FocusPoint const here = FocusPointOf(marker);
    uint32 bots = 0;
    {
        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* player = pair.second;
            if (!player || !player->IsInWorld() ||
                !WorldLogFormat::Near(here, FocusPointOf(player), WorldLogFormat::MARK_YARDS))
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
            if (!botAI || !botAI->worldLog || !botAI->worldLog->recorder.On())
                continue;

            botAI->worldLog->recorder.RequestCase(MARK_CASE, static_cast<int64>(t));
            ++bots;
        }
    }

    WorldLogFormat::Place place;
    place.map = marker->GetMapId();
    place.zone = marker->GetZoneId();
    place.area = marker->GetAreaId();
    place.x = marker->GetPositionX();
    place.y = marker->GetPositionY();
    std::string const markText = WorldLogFormat::MarkText(place, text);
    bool const queued = CensusMgr::Instance().EnqueueWorldLog(
        WorldLogFormat::Stream::Meta,
        WorldLogFormat::FormatMetaRow(
            {t, WorldLogFormat::META_MARK, static_cast<int64>(marker->GetGUID().GetCounter()), bots, markText}),
        1, t);
    if (pmo)
        pmo->finish();

    handler->PSendSysMessage(
        "WORLDLOG mark t={}: a case requested for {} bots within {} yd (each at its next position push){}", t, bots,
        static_cast<uint32>(WorldLogFormat::MARK_YARDS),
        queued ? "" : "; the mark row was not queued (writer stopping)");
    return true;
}

std::string WorldLog::RecorderStatus()
{
    WorldLogFormat::RecorderSettings const settings = CurrentRecorderSettings();
    return Acore::StringFormat(
        "WORLDLOG recorder: {} s, {} actions, {} events, one case per {} s per bot, {} bytes per bot; rings {}, cases "
        "{} ({} rows, {} bytes), cases dropped {}, words {}/{} ({} refused)",
        settings.seconds, settings.actions, settings.events, settings.cooldownSeconds, BotLogBytes(settings),
        s_rings.load(std::memory_order_relaxed), s_cases.load(std::memory_order_relaxed),
        s_caseRows.load(std::memory_order_relaxed), s_caseBytes.load(std::memory_order_relaxed),
        s_casesDropped.load(std::memory_order_relaxed), Words().Count(), WorldLogFormat::WORD_CAPACITY,
        Words().Overflow());
}
