/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the world log's Player-free part (src/Bot/Debug/WorldLogFormat.h): the checks the console's
// "playerbots worldlog selftest" runs (WorldLogSelftest.h), and four threads admitting rows through one QueueAccount
// under a mutex while a writer thread takes them. Optional argument: a folder that receives one sample segment per
// server stream (header row and rows: events, state, meta, decisions and a recorder case as the module formats them)
// for a cross-check through the hub's ingest.
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WorldLogSelftest.h"

using WorldLogFormat::QueueAccount;
using WorldLogFormat::Stream;

namespace
{
constexpr int THREADS = 4;
constexpr uint64_t PUSHES_PER_THREAD = 50000;
constexpr uint64_t CAP = 1000;
constexpr uint64_t SAMPLE_T = 1794231912000;
constexpr char const* SAMPLE_HOUR = "-2026110913.csv";
constexpr uint32_t SAMPLE_SERVER_MS = 5000000;
constexpr int16_t SAMPLE_RPG_DO_QUEST = 5;

int failures = 0;
int passes = 0;

void Report(bool ok, std::string const& what)
{
    if (ok)
    {
        ++passes;
        return;
    }

    if (++failures <= 20)
        std::cout << "FAIL " << what.substr(0, 200) << "\n";
}

void CheckShared()
{
    std::vector<std::string> lines;
    uint32_t const checks = WorldLogFormat::Selftest(lines);
    for (std::string const& line : lines)
        Report(false, line);

    passes += static_cast<int>(checks - lines.size());
}

void CheckConcurrentAdmit()
{
    QueueAccount account;
    std::mutex mutex;
    uint64_t taken = 0;
    bool done = false;

    std::thread writer(
        [&]
        {
            while (true)
            {
                std::lock_guard<std::mutex> guard(mutex);
                uint64_t const waiting = account.Of(Stream::Event).queued;
                account.Taken(Stream::Event, waiting);
                taken += waiting;
                if (done && !waiting)
                    return;
            }
        });

    std::vector<std::thread> producers;
    for (int t = 0; t < THREADS; ++t)
        producers.emplace_back(
            [&]
            {
                for (uint64_t push = 0; push < PUSHES_PER_THREAD; ++push)
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    account.Admit(Stream::Event, 1, CAP);
                    account.Admit(Stream::State, 1, CAP);
                }
            });

    for (std::thread& producer : producers)
        producer.join();

    {
        std::lock_guard<std::mutex> guard(mutex);
        done = true;
    }
    writer.join();

    uint64_t const total = THREADS * PUSHES_PER_THREAD;
    WorldLogFormat::StreamCounters const& event = account.Of(Stream::Event);
    WorldLogFormat::StreamCounters const& state = account.Of(Stream::State);
    Report(event.admitted + event.dropped == total, "event: every push admitted or dropped");
    Report(event.admitted == taken && event.queued == 0, "event: the writer took every admitted row");
    Report(event.peak <= CAP, "event: never more than the cap waiting");
    Report(state.admitted == CAP && state.dropped == total - CAP, "state (never taken): exactly the cap admitted");
    Report(account.Of(Stream::Meta).admitted == 0, "other streams untouched");
}

void WriteSample(std::string const& dir)
{
    WorldLogFormat::EventRow event;
    event.t = SAMPLE_T;
    event.bot = 42;
    event.type = WorldLogFormat::EVENT_LEVEL;
    event.id = 12;
    event.amount = 11;
    event.place = WorldLogFormat::Place{0, 12, 87, -8913.2f, 554.0f};
    std::ofstream(dir + "/event" + SAMPLE_HOUR, std::ios::binary) << WorldLogFormat::StreamHeader(Stream::Event) << '\n'
                                                                  << WorldLogFormat::FormatEventRow(event);

    WorldLogFormat::StateRow state;
    state.t = SAMPLE_T;
    state.bot = 42;
    state.place = event.place;
    state.level = 12;
    state.flags = WorldLogFormat::STATE_FLAG_HUMAN;
    std::string stateRows;
    WorldLogFormat::AppendStateRow(stateRows, state);
    std::ofstream(dir + "/state" + SAMPLE_HOUR, std::ios::binary) << WorldLogFormat::StreamHeader(Stream::State) << '\n'
                                                                  << stateRows;

    WorldLogFormat::RecorderSettings const recorderSettings;
    std::string const startText =
        WorldLogFormat::StartText(1, WorldLogFormat::Settings{},
                                  sizeof(WorldLogFormat::Recorder) + WorldLogFormat::RingArrayBytes(recorderSettings));
    std::string metaRows = WorldLogFormat::FormatMetaRow(
        WorldLogFormat::MetaRow{SAMPLE_T, WorldLogFormat::META_START, WorldLogFormat::CONTRACT_VERSION, 0, startText});
    metaRows += WorldLogFormat::FormatMetaRow(WorldLogFormat::MetaRow{
        SAMPLE_T, WorldLogFormat::META_MARK, 7, 1, WorldLogFormat::MarkText(event.place, "the bot at the gate")});
    QueueAccount account;
    account.AppendDropsRows(metaRows, SAMPLE_T + 1);
    metaRows +=
        WorldLogFormat::FormatMetaRow(WorldLogFormat::MetaRow{SAMPLE_T + 2, WorldLogFormat::META_STOP, 2, 0, ""});
    std::ofstream(dir + "/meta" + SAMPLE_HOUR, std::ios::binary) << WorldLogFormat::StreamHeader(Stream::Meta) << '\n'
                                                                 << metaRows;

    WorldLogFormat::DecisionRow route;
    route.t = SAMPLE_T;
    route.bot = 42;
    route.kind = WorldLogFormat::DecisionKind::Route;
    route.choice = "walk>flight>walk";
    route.reason = "search";
    route.value = 312.5f;
    route.alternatives = {{"walk_direct", 901.25f}, {"legs", 3.0f}};
    route.ref = 7;
    WorldLogFormat::DecisionRow status;
    status.t = SAMPLE_T + 1;
    status.bot = 42;
    status.kind = WorldLogFormat::DecisionKind::RpgStatus;
    status.choice = "do quest";
    status.reason = "no_target";
    status.value = 30.0f;
    std::ofstream(dir + "/decision" + SAMPLE_HOUR, std::ios::binary)
        << WorldLogFormat::StreamHeader(Stream::Decision) << '\n'
        << WorldLogFormat::FormatDecisionRow(route) << WorldLogFormat::FormatDecisionRow(status);

    // A recorder case as a bot's ring dumps it: a position, a failed action, a Stuck event, then the trigger row
    WorldLogFormat::WordTable words;
    WorldLogFormat::Recorder recorder;
    recorder.Allocate(recorderSettings);
    recorder.PushPosition(
        words, WorldLogFormat::RecorderPosition{SAMPLE_SERVER_MS, -8913.2f, 554.0f, 0, SAMPLE_RPG_DO_QUEST}, true);
    recorder.PushAction(SAMPLE_SERVER_MS + 400, {words.Intern("go"), words.Intern("rpg")}, false);
    recorder.PushEvent(words, SAMPLE_SERVER_MS + 900, "Stuck", "offmesh", SAMPLE_RPG_DO_QUEST, 77);
    std::string caseRows;
    recorder.AppendCase(caseRows, words, 1, 42, SAMPLE_SERVER_MS + 1000, SAMPLE_T);
    std::ofstream(dir + "/recorder" + SAMPLE_HOUR, std::ios::binary)
        << WorldLogFormat::StreamHeader(Stream::Recorder) << '\n'
        << caseRows;
}
}  // namespace

int main(int argc, char** argv)
{
    CheckShared();
    CheckConcurrentAdmit();

    if (argc > 1)
        WriteSample(argv[1]);

    std::cout << "world-log-selftest: " << passes << " passed, " << failures << " failed\n";
    return failures ? 1 : 0;
}
