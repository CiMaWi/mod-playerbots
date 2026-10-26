/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MatchLog.h"

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "AiFactory.h"
#include "ArenaScript.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundScore.h"
#include "Chat.h"
#include "Log.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpTestRecord.h"
#include "ScriptMgr.h"
#include "SkillStrategy.h"
#include "StringFormat.h"

namespace
{
using Clock = std::chrono::steady_clock;

// A capture, claim or hand-off older than this belongs to a match that never ended (closed arena, restart of the
// map) and is dropped; the longest arena is far shorter
constexpr std::chrono::hours MaxAge{3};
constexpr size_t MaxCaptures = 256;
constexpr size_t MaxKept = 64;
constexpr uint32 StampLength = 14;

// What the hook of a member update saw
struct MemberUpdate
{
    bool Won = false;
    int32 MmrChange = 0;
};

// A running arena match: the roster and the "before" values taken at the start, the member updates seen since
struct Capture
{
    PvpTestMatchRecord Record;
    std::unordered_map<uint64, MemberUpdate> Updates;
    // The bots count their casts for this match (not when a harness job counts); the casts of the bots that left
    // before the end, by guid: they stop counting when they leave
    bool Counted = false;
    std::unordered_map<uint64, std::array<uint32, 6>> Leavers;
    Clock::time_point Created = Clock::now();
};

struct HarnessClaim
{
    std::string MatchId;
    Clock::time_point Created = Clock::now();
};

struct Handoff
{
    PvpTestMatchRecord Record;
    Clock::time_point Created = Clock::now();
};

// Shared between the arena map threads and the world thread
std::mutex RegistryLock;
std::unordered_map<uint32, Capture> Captures;
std::unordered_map<uint32, HarnessClaim> Claims;
std::unordered_map<uint32, Handoff> Handoffs;

// Plain counters for .playerbots matchlog status
std::atomic<uint64> Started{0};
std::atomic<uint64> Ended{0};
std::atomic<uint64> LateRecords{0};
std::atomic<uint64> Written{0};
std::atomic<uint64> HandedOff{0};
std::atomic<uint64> Merged{0};
std::atomic<uint64> Dropped{0};
std::atomic<uint64> StartMicroseconds{0};
std::atomic<uint64> EndMicroseconds{0};
std::atomic<uint32> Counter{0};

// UTC YYYYMMDDHHMMSS of the process start (initialised with the module), so ids stay unique across restarts
std::string MakeStamp()
{
    std::time_t const now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &utc);
    return buffer;
}

std::string const ProcessStamp = MakeStamp();

uint64 MicrosecondsSince(Clock::time_point start)
{
    return static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

// Drops the entries older than MaxAge, then the oldest ones until at most cap are left; the registry lock is held
template <typename Map>
void Prune(Map& map, size_t cap)
{
    Clock::time_point const now = Clock::now();
    for (auto itr = map.begin(); itr != map.end();)
    {
        if (now - itr->second.Created > MaxAge)
        {
            itr = map.erase(itr);
            ++Dropped;
        }
        else
            ++itr;
    }

    while (!map.empty() && map.size() > cap)
    {
        auto oldest = map.begin();
        for (auto itr = map.begin(); itr != map.end(); ++itr)
        {
            if (itr->second.Created < oldest->second.Created)
                oldest = itr;
        }

        map.erase(oldest);
        ++Dropped;
    }
}

// The harness gets the ratings, honor and the like from the organic view of its own match
void MergeExtras(PvpTestMatchRecord const& from, PvpTestMatchRecord& to)
{
    to.Server = from.Server;
    to.Real = from.Real;
    to.RatingsKnown = from.RatingsKnown;
    for (size_t index = 0; index < to.Sides.size(); ++index)
    {
        PvpTestSideRecord const& source = from.Sides[index];
        PvpTestSideRecord& target = to.Sides[index];
        target.TeamName = source.TeamName;
        target.TeamRatingBefore = source.TeamRatingBefore;
        target.TeamRatingAfter = source.TeamRatingAfter;
        target.MmrBefore = source.MmrBefore;
        target.MmrAfter = source.MmrAfter;
        for (PvpTestBotRecord& bot : target.Bots)
        {
            for (PvpTestBotRecord const& seen : source.Bots)
            {
                if (seen.Guid != bot.Guid)
                    continue;

                bot.Real = seen.Real;
                bot.Honor = seen.Honor;
                bot.PersonalBefore = seen.PersonalBefore;
                bot.PersonalAfter = seen.PersonalAfter;
                bot.MmrChangeKnown = seen.MmrChangeKnown;
                bot.MmrChange = seen.MmrChange;
                break;
            }
        }
    }
}

bool IsClaimed(uint32 instanceId)
{
    std::lock_guard<std::mutex> guard(RegistryLock);
    return Claims.find(instanceId) != Claims.end();
}

// The roster and the team facts of an arena as they are now. atStart: the "before" values are these; otherwise (a
// match whose start was not seen) they stay unknown. Bots count their casts from now on unless the harness counts.
PvpTestMatchRecord BuildRoster(Battleground* bg, bool atStart, bool count)
{
    PvpTestMatchRecord record;
    record.Kind = "arena";
    record.StartTime = PvpTestRecordWriter::IsoTimeNow();
    record.Server = MatchLog::ServerId();
    record.ArenaType = bg->GetArenaType();
    record.Rated = bg->isRated();
    record.Map = bg->GetName();
    std::array<ArenaTeam*, 2> teams{};
    for (size_t index = 0; index < record.Sides.size(); ++index)
    {
        TeamId const teamId = index == 0 ? TEAM_ALLIANCE : TEAM_HORDE;
        PvpTestSideRecord& side = record.Sides[index];
        side.Side = index == 0 ? 'A' : 'B';
        side.Faction = index == 0 ? "alliance" : "horde";
        side.Label = side.Faction;
        if (!record.Rated)
            continue;

        side.TeamId = bg->GetArenaTeamIdForTeam(teamId);
        ArenaTeam* team = side.TeamId ? sArenaTeamMgr->GetArenaTeamById(side.TeamId) : nullptr;
        teams[index] = team;
        if (!team)
            continue;

        side.TeamName = team->GetName();
        side.Label = team->GetName();
        if (atStart)
        {
            side.TeamRatingBefore = team->GetRating();
            side.MmrBefore = static_cast<int32>(bg->GetArenaMatchmakerRating(teamId));
        }
    }

    for (auto const& [guid, player] : bg->GetPlayers())
    {
        if (!player)
            continue;

        size_t const index = player->GetBgTeamId() == TEAM_HORDE ? 1 : 0;
        PvpTestBotRecord bot;
        bot.Guid = guid.GetCounter();
        bot.Name = player->GetName();
        bot.ClassId = player->getClass();
        bot.RaceId = player->getRace();
        bot.Level = player->GetLevel();
        bot.Spec = AiFactory::GetPlayerSpecName(player);
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        bot.Real = !botAI;
        if (botAI)
        {
            // The gear score after the PvP loadout is worn: the wear is queued when the player is added to the arena
            // and has had the preparation time to finish
            bot.GearScore = botAI->GetEquipGearScore(player);
            if (sPlayerbotAIConfig.SkillEnable && botAI->GetPvpSkill())
                bot.Skill = botAI->GetPvpSkill();

            if (count)
                botAI->SetPvpTestCounting(true);
        }

        if (atStart && teams[index])
        {
            if (ArenaTeamMember* member = teams[index]->GetMember(guid))
                bot.PersonalBefore = member->PersonalRating;
        }

        record.Real = record.Real || bot.Real;
        record.Sides[index].Bots.push_back(std::move(bot));
    }

    return record;
}

// The scores, the ratings after, the winner: everything the end of the arena adds to the roster
void FinishRecord(Battleground* bg, TeamId winnerTeam, bool counted, Capture& capture)
{
    PvpTestMatchRecord& record = capture.Record;
    record.Outcome = "ended";
    if (winnerTeam == TEAM_ALLIANCE || winnerTeam == TEAM_HORDE)
    {
        size_t const index = winnerTeam == TEAM_ALLIANCE ? 0 : 1;
        record.WinnerSide = std::string(1, record.Sides[index].Side);
        record.Winner = record.Sides[index].Label;
        record.Reason = "winner";
    }
    else
        record.Reason = "time limit";  // a draw only comes from the arena timer

    record.TotalMs = bg->GetStartTime();
    int32 const delay = bg->GetStartDelayTime();
    record.FightMs = delay >= 0 && record.TotalMs > static_cast<uint32>(delay) ? record.TotalMs - delay : 0;

    auto const* scores = bg->GetPlayerScores();
    bool ratingsKnown = record.Rated;
    for (size_t index = 0; index < record.Sides.size(); ++index)
    {
        PvpTestSideRecord& side = record.Sides[index];
        ArenaTeam* team = record.Rated && side.TeamId ? sArenaTeamMgr->GetArenaTeamById(side.TeamId) : nullptr;
        if (team)
        {
            // FinishGame has run: the team holds the new rating, the battleground the new matchmaker rating
            side.TeamRatingAfter = team->GetRating();
            side.MmrAfter = static_cast<int32>(bg->GetArenaMatchmakerRating(index == 0 ? TEAM_ALLIANCE : TEAM_HORDE));
        }

        ratingsKnown = ratingsKnown && team && side.TeamRatingBefore >= 0 && side.MmrBefore >= 0;
        for (PvpTestBotRecord& bot : side.Bots)
        {
            auto const score = scores->find(bot.Guid);
            if (score != scores->end() && score->second)
            {
                bot.Entered = true;
                bot.Damage = score->second->GetDamageDone();
                bot.Healing = score->second->GetHealingDone();
                bot.KillingBlows = score->second->GetKillingBlows();
                bot.Deaths = score->second->GetDeaths();
                bot.HonorableKills = score->second->GetHonorableKills();
                bot.Honor = score->second->GetBonusHonor();
            }

            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(bot.Guid);
            if (team)
            {
                if (ArenaTeamMember* member = team->GetMember(guid))
                    bot.PersonalAfter = member->PersonalRating;
            }

            auto const update = capture.Updates.find(bot.Guid);
            if (update != capture.Updates.end())
            {
                bot.MmrChangeKnown = true;
                bot.MmrChange = update->second.MmrChange;
            }

            // The bots are still in the arena here; the counting stops with the match (a harness match is read by
            // its job, which also stops it). A bot that left earlier stopped counting then (OnLeave).
            if (!counted)
                continue;

            auto const leaver = capture.Leavers.find(bot.Guid);
            if (leaver != capture.Leavers.end())
            {
                bot.Casts = leaver->second;
                continue;
            }

            auto const player = bg->GetPlayers().find(guid);
            PlayerbotAI* botAI = nullptr;
            if (player != bg->GetPlayers().end() && player->second)
                botAI = GET_PLAYERBOT_AI(player->second);

            if (botAI)
            {
                bot.Casts = botAI->GetPvpTestExecuted();
                botAI->SetPvpTestCounting(false);
            }
        }
    }

    record.RatingsKnown = ratingsKnown;
}

// A string-aware check that a JSON text is balanced and closes every string
bool WellFormed(std::string const& json)
{
    int depth = 0;
    bool inString = false;
    for (size_t i = 0; i < json.size(); ++i)
    {
        char const c = json[i];
        if (inString)
        {
            if (c == '\\')
                ++i;
            else if (c == '"')
                inString = false;

            continue;
        }

        if (c == '"')
            inString = true;
        else if (c == '{' || c == '[')
            ++depth;
        else if ((c == '}' || c == ']') && --depth < 0)
            return false;
    }

    return depth == 0 && !inString;
}

bool Contains(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }
}  // namespace

std::string MatchLog::ServerId()
{
    return sPlayerbotAIConfig.LadderServerId.empty() ? std::string("main") : sPlayerbotAIConfig.LadderServerId;
}

std::string MatchLog::NewMatchId() { return Acore::StringFormat("{}-{}-{}", ServerId(), ProcessStamp, ++Counter); }

void MatchLog::Stamp(PvpTestMatchRecord& record)
{
    record.Server = ServerId();
    if (record.MatchId.empty())
        record.MatchId = NewMatchId();
}

void MatchLog::Claim(uint32 instanceId, std::string const& matchId)
{
    if (!sPlayerbotAIConfig.MatchLogEnable)
        return;

    std::lock_guard<std::mutex> guard(RegistryLock);
    Prune(Claims, MaxKept - 1);
    HarnessClaim claim;
    claim.MatchId = matchId;
    Claims[instanceId] = std::move(claim);
}

bool MatchLog::Merge(uint32 instanceId, PvpTestMatchRecord& record)
{
    std::lock_guard<std::mutex> guard(RegistryLock);
    auto const handoff = Handoffs.find(instanceId);
    if (handoff == Handoffs.end())
        return false;

    MergeExtras(handoff->second.Record, record);
    Handoffs.erase(handoff);
    ++Merged;
    return true;
}

void MatchLog::OnArenaStart(Battleground* bg)
{
    if (!sPlayerbotAIConfig.MatchLogEnable || !bg || !bg->isArena())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "MatchLogStart");
    Clock::time_point const start = Clock::now();
    uint32 const instanceId = bg->GetInstanceID();
    Capture capture;
    // A harness job counts the casts itself (from the start of its fight)
    capture.Counted = !IsClaimed(instanceId);
    capture.Record = BuildRoster(bg, true, capture.Counted);
    {
        std::lock_guard<std::mutex> guard(RegistryLock);
        Prune(Captures, MaxCaptures - 1);
        Captures[instanceId] = std::move(capture);
    }

    ++Started;
    StartMicroseconds += MicrosecondsSince(start);
    if (pmo)
        pmo->finish();
}

void MatchLog::OnMemberUpdate(ArenaTeam* /*team*/, Player* player, bool won, int32 matchmakerChange)
{
    if (!sPlayerbotAIConfig.MatchLogEnable || !player)
        return;

    Battleground* bg = player->GetBattleground();
    if (!bg)
        return;

    std::lock_guard<std::mutex> guard(RegistryLock);
    auto const capture = Captures.find(bg->GetInstanceID());
    if (capture != Captures.end())
        capture->second.Updates[player->GetGUID().GetCounter()] = MemberUpdate{won, matchmakerChange};
}

void MatchLog::OnLeave(Battleground* bg, Player* player)
{
    if (!sPlayerbotAIConfig.MatchLogEnable || !bg || !player || !bg->isArena())
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI || !botAI->IsPvpTestCounting())
        return;

    // After the end the capture is gone and the end has already stopped the counting; a harness job counts itself
    std::lock_guard<std::mutex> guard(RegistryLock);
    auto const capture = Captures.find(bg->GetInstanceID());
    if (capture == Captures.end() || !capture->second.Counted)
        return;

    capture->second.Leavers[player->GetGUID().GetCounter()] = botAI->GetPvpTestExecuted();
    botAI->SetPvpTestCounting(false);
}

void MatchLog::OnEnd(Battleground* bg, TeamId winnerTeam)
{
    if (!sPlayerbotAIConfig.MatchLogEnable || !bg || !bg->isArena())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "MatchLogEnd");
    Clock::time_point const start = Clock::now();
    uint32 const instanceId = bg->GetInstanceID();
    Capture capture;
    bool seenStart = false;
    bool claimed = false;
    std::string claimedId;
    {
        std::lock_guard<std::mutex> guard(RegistryLock);
        auto const found = Captures.find(instanceId);
        if (found != Captures.end())
        {
            capture = std::move(found->second);
            Captures.erase(found);
            seenStart = true;
        }

        auto const claim = Claims.find(instanceId);
        if (claim != Claims.end())
        {
            claimed = true;
            claimedId = claim->second.MatchId;
            Claims.erase(claim);
        }
    }

    if (!seenStart)
    {
        // The start was not seen (the option was turned on during the match): the roster as it is, no "before"
        capture.Record = BuildRoster(bg, false, false);
        ++LateRecords;
    }

    FinishRecord(bg, winnerTeam, seenStart && !claimed, capture);
    PvpTestMatchRecord& record = capture.Record;
    record.MatchId = claimed ? claimedId : NewMatchId();
    record.Id = record.MatchId;
    ++Ended;
    if (claimed)
    {
        // The harness job writes the record (it also covers failed and stopped matches); it merges these fields in
        record.Source = "harness";
        std::lock_guard<std::mutex> guard(RegistryLock);
        Prune(Handoffs, MaxKept - 1);
        Handoff handoff;
        handoff.Record = std::move(record);
        Handoffs[instanceId] = std::move(handoff);
        ++HandedOff;
    }
    else
    {
        record.Source = record.Real ? "player" : "organic";
        PvpTestRecordWriter::Instance().Write(record);
        ++Written;
        LOG_INFO("playerbots", "MATCHLOG: {} {}v{} {}: {} vs {}, winner {}, ratings {}->{} / {}->{}, source {}",
                 record.MatchId, static_cast<uint32>(record.ArenaType), static_cast<uint32>(record.ArenaType),
                 record.Map, record.Sides[0].Label, record.Sides[1].Label,
                 record.WinnerSide.empty() ? std::string("draw") : record.WinnerSide, record.Sides[0].TeamRatingBefore,
                 record.Sides[0].TeamRatingAfter, record.Sides[1].TeamRatingBefore, record.Sides[1].TeamRatingAfter,
                 record.Source);
    }

    EndMicroseconds += MicrosecondsSince(start);
    if (pmo)
        pmo->finish();
}

bool MatchLog::HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    size_t captures = 0;
    size_t claims = 0;
    size_t handoffs = 0;
    {
        std::lock_guard<std::mutex> guard(RegistryLock);
        captures = Captures.size();
        claims = Claims.size();
        handoffs = Handoffs.size();
    }

    PvpTestRecordWriter& writer = PvpTestRecordWriter::Instance();
    uint64 const started = Started.load();
    uint64 const ended = Ended.load();
    handler->PSendSysMessage("MATCHLOG: Playerbots.MatchLog.Enable {}, server {}, next id {}-{}-{}",
                             sPlayerbotAIConfig.MatchLogEnable ? 1 : 0, ServerId(), ServerId(), ProcessStamp,
                             Counter.load() + 1);
    handler->PSendSysMessage(
        "MATCHLOG: arena starts {}, ends {} ({} without a seen start), records written by MatchLog {}, handed to the "
        "harness {} (merged {}), dropped {}",
        started, ended, LateRecords.load(), Written.load(), HandedOff.load(), Merged.load(), Dropped.load());
    handler->PSendSysMessage("MATCHLOG: running captures {}, harness claims {}, kept hand-offs {}", captures, claims,
                             handoffs);
    handler->PSendSysMessage(
        "MATCHLOG: writer queue {} (high water {}), lines written to the file {}, lost {}; mean capture {} us, mean "
        "end "
        "{} us",
        writer.QueueSize(), writer.QueueHighWater(), writer.Written(), writer.Failed(),
        started ? StartMicroseconds.load() / started : 0, ended ? EndMicroseconds.load() / ended : 0);

    // The counting: every executed action of a counting bot looks its name up (BeginExecutedCount)
    PlayerbotAI::CountingStats const counting = PlayerbotAI::GetCountingStats();
    uint64 const meanNs = counting.Samples ? counting.SampledNs / counting.Samples : 0;
    handler->PSendSysMessage(
        "MATCHLOG: counting: {} actions went through the name lookup, {} lookups timed, mean {} ns (clock included), "
        "estimated total {} ms",
        counting.Calls, counting.Samples, meanNs, counting.Calls * meanNs / 1000000);
    return true;
}

bool MatchLog::HandleSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    uint32 failures = 0;
    uint32 checks = 0;
    auto check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("MATCHLOG selftest FAIL: {}", what);
    };

    // The selftest leaves the counters of the real matches as they were
    uint32 const counter0 = Counter.load();
    uint64 const dropped0 = Dropped.load();
    uint64 const merged0 = Merged.load();

    // Ids: <server>-<14 digits>-<n>, counting up, unique
    std::string const first = NewMatchId();
    std::string const second = NewMatchId();
    std::string const prefix = ServerId() + "-" + ProcessStamp + "-";
    check(ProcessStamp.size() == StampLength, "the process stamp has 14 digits");
    check(first.compare(0, prefix.size(), prefix) == 0, "an id starts with <server>-<stamp>-");
    check(first != second && std::stoul(second.substr(prefix.size())) == std::stoul(first.substr(prefix.size())) + 1,
          "ids count up");

    // A full organic record: the new keys are there, null where unknown, balanced
    PvpTestMatchRecord organic;
    organic.Id = first;
    organic.MatchId = first;
    organic.Server = ServerId();
    organic.Source = "organic";
    organic.StartTime = "2026-10-26T12:00:00Z";
    organic.Rated = true;
    organic.RatingsKnown = true;
    organic.Outcome = "ended";
    organic.Winner = "Team \"A\"";
    organic.WinnerSide = "A";
    organic.Reason = "winner";
    for (size_t index = 0; index < organic.Sides.size(); ++index)
    {
        PvpTestSideRecord& side = organic.Sides[index];
        side.Side = index == 0 ? 'A' : 'B';
        side.Label = index == 0 ? "Team \"A\"" : "Team B";
        side.TeamName = side.Label;
        side.TeamId = 10 + static_cast<uint32>(index);
        side.TeamRatingBefore = 1500;
        side.TeamRatingAfter = index == 0 ? 1516 : 1484;
        side.MmrBefore = 1500;
        side.MmrAfter = index == 0 ? 1516 : 1484;
        PvpTestBotRecord bot;
        bot.Guid = 1000 + index;
        bot.Name = "Bot";
        bot.ClassId = 8;
        bot.PersonalBefore = 1500;
        bot.PersonalAfter = index == 0 ? 1516 : 1484;
        bot.MmrChangeKnown = true;
        bot.MmrChange = index == 0 ? 16 : -16;
        bot.Honor = 0;
        bot.Casts[2] = 3;
        side.Bots.push_back(bot);
    }

    std::string const line = PvpTestRecordWriter::ToJson(organic);
    check(WellFormed(line), "the organic record is well-formed JSON");
    check(Contains(line, "\"schema\":1"), "schema stays 1");
    check(
        Contains(line, "\"server\":\"") && Contains(line, "\"matchId\":\"") && Contains(line, "\"source\":\"organic\""),
        "server, matchId, source are written");
    check(Contains(line, "\"teamRatingBefore\":1500") && Contains(line, "\"teamRatingAfter\":1516") &&
              Contains(line, "\"mmrAfter\":1484") && Contains(line, "\"personalAfter\":1484") &&
              Contains(line, "\"mmrChange\":-16"),
          "ratings, personal ratings and the MMR change are written");
    check(Contains(line, "\"casts\":{\"interrupt\":0,\"defensive\":0,\"dispel\":3"), "the casts are written");
    check(Contains(line, "\"winner\":\"Team \\\"A\\\"\""), "a quote in a team name is escaped");

    // A harness record that was never merged: ratings and the MMR change are null
    PvpTestMatchRecord harness;
    harness.Id = "m-selftest-1";
    harness.StartTime = "2026-10-26T12:00:00Z";
    harness.Sides[0].Bots.push_back(PvpTestBotRecord());
    harness.Sides[0].Bots[0].Guid = 1000;
    harness.Sides[1].Bots.push_back(PvpTestBotRecord());
    harness.Sides[1].Bots[0].Guid = 1001;
    Stamp(harness);
    std::string const harnessLine = PvpTestRecordWriter::ToJson(harness);
    check(WellFormed(harnessLine), "the harness record is well-formed JSON");
    check(harness.Id == "m-selftest-1" && !harness.MatchId.empty() && harness.Source == "harness",
          "a harness record keeps its id and gains a match id");
    check(Contains(harnessLine, "\"teamRatingBefore\":null") && Contains(harnessLine, "\"mmrChange\":null") &&
              Contains(harnessLine, "\"ratingsKnown\":false") && Contains(harnessLine, "\"personalBefore\":null"),
          "unknown ratings are null");

    // Exactly one record per match: a claimed instance hands its fields to the harness and writes nothing itself
    uint32 const instance = 0xFFFFFF01;
    {
        std::lock_guard<std::mutex> guard(RegistryLock);
        Handoff handoff;
        handoff.Record = organic;
        Handoffs[instance] = std::move(handoff);
    }

    check(!Merge(instance + 1, harness), "no merge for an instance without a hand-off");
    check(Merge(instance, harness), "the hand-off merges into the harness record");
    check(!Merge(instance, harness), "a hand-off merges once");
    check(harness.Sides[0].TeamRatingAfter == 1516 && harness.Sides[1].Bots[0].PersonalAfter == 1484 &&
              harness.Sides[0].Bots[0].MmrChange == 16 && harness.RatingsKnown && harness.Id == "m-selftest-1" &&
              harness.MatchId != organic.MatchId,
          "the merge copies the ratings by side and bot, keeps the harness id and match id");

    // The registry stays bounded (on a map of its own: the live hand-offs are not touched)
    {
        std::unordered_map<uint32, Handoff> local;
        for (uint32 i = 0; i < MaxKept + 20; ++i)
        {
            Prune(local, MaxKept - 1);
            local[0xFFFF0000 + i] = Handoff();
        }

        check(local.size() <= MaxKept, "the hand-off map is capped");
    }

    {
        // Only the selftest's own hand-off key goes (a failed merge check could leave it)
        std::lock_guard<std::mutex> guard(RegistryLock);
        Handoffs.erase(instance);
    }

    // The counting: the name lookup of an executed action finds the categories, and what it costs
    {
        std::array<std::string, 8> const names = {"attack",    "dps assist",    "fire bolt", "check mount state",
                                                  "polymorph", "silence on cc", "flee",      "spell reflection"};
        check(SkillNoticeCategoryOf("dps assist") == SkillNoticeCategory::TargetSwitch &&
                  SkillNoticeCategoryOf("polymorph") == SkillNoticeCategory::CrowdControl &&
                  SkillNoticeCategoryOf("silence on cc") == SkillNoticeCategory::CrowdControl &&
                  SkillNoticeCategoryOf("attack") == SkillNoticeCategory::None,
              "the counting finds the categories by action name");

        constexpr uint32 rounds = 250000;
        uint32 found = 0;
        Clock::time_point const begin = Clock::now();
        for (uint32 i = 0; i < rounds; ++i)
            found += SkillNoticeCategoryOf(names[i % names.size()]) != SkillNoticeCategory::None ? 1 : 0;

        uint64 const nanoseconds = MicrosecondsSince(begin) * 1000 / rounds;
        check(found > 0, "the lookup loop ran");
        handler->PSendSysMessage("MATCHLOG selftest: name lookup of an executed action {} ns (mean of {} lookups)",
                                 nanoseconds, rounds);
    }

    Counter = counter0;
    Dropped = dropped0;
    Merged = merged0;
    handler->PSendSysMessage("MATCHLOG selftest: {} checks, {} failed", checks, failures);
    handler->PSendSysMessage("MATCHLOG sample record: {}", line);
    return true;
}

// Hooks of the arena. Returns true from OnBeforeArenaTeamMemberUpdate: ScriptMgr stops the core's MemberWon/MemberLost
// when a script returns false (CALL_ENABLED_BOOLEAN_HOOKS negates the value), so true leaves the outcome unchanged.
class MatchLogArenaScript : public ArenaScript
{
public:
    MatchLogArenaScript()
        : ArenaScript("MatchLogArenaScript", {ARENAHOOK_ON_ARENA_START, ARENAHOOK_ON_BEFORE_TEAM_MEMBER_UPDATE})
    {
    }

    void OnArenaStart(Battleground* bg) override { MatchLog::OnArenaStart(bg); }

    [[nodiscard]] bool OnBeforeArenaTeamMemberUpdate(ArenaTeam* team, Player* player, bool won,
                                                     uint32 /*opponentMatchmakerRating*/,
                                                     int32 matchmakerChange) override
    {
        MatchLog::OnMemberUpdate(team, player, won, matchmakerChange);
        return true;
    }
};

void AddSC_match_log() { new MatchLogArenaScript(); }
