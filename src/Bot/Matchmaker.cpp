/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Matchmaker.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "AllBattlegroundScript.h"
#include "ArenaSeasonMgr.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DBCStores.h"
#include "DisableMgr.h"
#include "GameTime.h"
#include "Group.h"
#include "LFGMgr.h"
#include "LadderMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "WorldPacket.h"

namespace
{
constexpr uint32 CheckIntervalMs = 500;
constexpr uint32 NoCandidateRetryMs = 5 * IN_MILLISECONDS;
constexpr size_t MaxPickRecords = 20;

uint32 UnixNow() { return static_cast<uint32>(GameTime::GetGameTime().count()); }

uint32 Distance(uint32 a, uint32 b) { return a > b ? a - b : b - a; }

BattlegroundQueueTypeId ArenaQueueType(uint8 arenaType)
{
    return BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_AA, arenaType);
}

// In the arena queue and not invited yet
bool WaitsInQueue(Player* player, BattlegroundQueueTypeId queueTypeId)
{
    return player->InBattlegroundQueueForBattlegroundQueueType(queueTypeId) &&
           !player->IsInvitedForBattlegroundQueueType(queueTypeId);
}

bool InvitedOrPlaying(Player* player, BattlegroundQueueTypeId queueTypeId)
{
    return player->InBattleground() || player->IsInvitedForBattlegroundQueueType(queueTypeId);
}

uint32 AverageMmr(ArenaTeam* team)
{
    uint32 sum = 0;
    for (ArenaTeamMember const& member : team->GetMembers())
        sum += member.MatchMakerRating;

    return team->GetMembersSize() ? sum / team->GetMembersSize() : 0;
}

// Every member is a random bot: no real player's team, no team of a player's own bots
bool IsRandomBotTeam(ArenaTeam* team)
{
    for (ArenaTeamMember const& member : team->GetMembers())
    {
        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(member.Guid);
        if (!entry || !sPlayerbotAIConfig.IsInRandomAccountList(entry->AccountId))
            return false;
    }

    return team->GetMembersSize() > 0;
}

// Why an online member must not be taken away from what it does; nullptr: free
char const* BusyReason(Player* member)
{
    if (member->InBattleground() || member->InArena())
        return "is in a battleground or arena";

    if (member->InBattlegroundQueue())
        return "is in a battleground queue";

    if (Map* map = member->FindMap(); map && (map->IsDungeon() || map->IsRaid()))
        return "is in a dungeon or raid";

    switch (sLFGMgr->GetState(member->GetGUID()))
    {
        case lfg::LFG_STATE_ROLECHECK:
        case lfg::LFG_STATE_QUEUED:
        case lfg::LFG_STATE_PROPOSAL:
        case lfg::LFG_STATE_BOOT:
        case lfg::LFG_STATE_DUNGEON:
            return "is in the dungeon finder";
        default:
            break;
    }

    if (member->IsBeingTeleported())
        return "is teleporting";

    if (LadderMgr::Instance().IsQueueBlocked(member))
        return "may not queue (its team departs or the ladder is drained)";

    return nullptr;
}
}  // namespace

MatchmakerPick Matchmaker::Pick(uint32 mmr, bool rated, uint32 waitSeconds,
                                std::vector<MatchmakerCandidate> const& candidates, std::vector<uint32> const& recent,
                                MatchmakerSettings const& settings, float roll)
{
    MatchmakerPick result;
    // First without every recent opponent; when only those are left, without the newest one
    for (size_t const skipped : {recent.size(), std::min<size_t>(recent.size(), 1)})
    {
        auto const recentBegin = recent.end() - static_cast<std::ptrdiff_t>(skipped);
        std::vector<std::pair<size_t, uint32>> eligible;  // index, MMR distance
        uint32 nearest = std::numeric_limits<uint32>::max();
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            MatchmakerCandidate const& candidate = candidates[i];
            if (candidate.Busy || candidate.Pinned ||
                std::find(recentBegin, recent.end(), candidate.TeamId) != recent.end())
                continue;

            uint32 const distance = rated ? Distance(mmr, candidate.Mmr) : 0;
            eligible.emplace_back(i, distance);
            nearest = std::min(nearest, distance);
        }

        if (eligible.empty())
            continue;

        uint32 window = 0;
        if (rated)
        {
            window = std::max<uint32>(settings.Window + settings.WindowGrowth * waitSeconds, 1);
            while (window < nearest)
                window =
                    window > std::numeric_limits<uint32>::max() / 2 ? std::numeric_limits<uint32>::max() : window * 2;
        }

        std::vector<std::pair<size_t, double>> weighted;
        double total = 0.0;
        for (auto const& [index, distance] : eligible)
        {
            if (rated && distance > window)
                continue;

            double const scaled = static_cast<double>(distance) / std::max<uint32>(settings.MmrWidth, 1);
            double const weight = 1.0 / (1.0 + scaled * scaled);
            weighted.emplace_back(index, weight);
            total += weight;
        }

        result.Window = window;
        result.Eligible = static_cast<uint32>(weighted.size());
        result.Index = static_cast<int32>(weighted.back().first);
        double target = std::clamp(static_cast<double>(roll), 0.0, 1.0) * total;
        for (auto const& [index, weight] : weighted)
        {
            if (target < weight)
            {
                result.Index = static_cast<int32>(index);
                break;
            }

            target -= weight;
        }

        return result;
    }

    return result;
}

std::string Matchmaker::JoinArenaQueue(Player* leader, uint8 arenaType, bool rated)
{
    PLAYERBOT_CHECK_WORLD_THREAD("Matchmaker::JoinArenaQueue");
    Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA);
    if (!bgt)
        return "no arena template";

    if (sDisableMgr->IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, BATTLEGROUND_AA, nullptr))
        return "arenas are disabled";

    if (leader->InBattleground() || leader->InBattlegroundQueue())
        return "the leader is in a battleground or a queue";

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgt->GetMapId(), leader->GetLevel());
    if (!bracketEntry)
        return "no arena bracket for the leader's level";

    Group* group = leader->GetGroup();
    if (!group || group->GetLeaderGUID() != leader->GetGUID())
        return "not a group leader";

    uint8 const slot = ArenaTeam::GetSlotByType(arenaType);
    uint32 teamId = 0;
    uint32 arenaRating = 0;
    uint32 matchmakerRating = 0;
    uint32 previousOpponents = 0;
    if (rated)
    {
        if (sArenaSeasonMgr->GetSeasonState() == ARENA_SEASON_STATE_DISABLED)
            return "no arena season in progress";

        teamId = leader->GetArenaTeamId(slot);
        ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
        if (!team)
            return "the leader has no arena team of this type";

        arenaRating = std::max<uint32>(team->GetRating(), 1);
        matchmakerRating = team->GetAverageMMR(group);
        previousOpponents = team->GetPreviousOpponents();
    }

    BattlegroundTypeId const bgTypeId = bgt->GetBgTypeID();
    BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, arenaType);
    BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
    GroupJoinBattlegroundResult err =
        group->CanJoinBattlegroundQueue(bgt, queueTypeId, arenaType, arenaType, rated, slot);
    if (err > 0)
    {
        group->DoForAllMembers(
            [&queue, &err](Player* member)
            {
                if (queue.IsPlayerInvitedToRatedArena(member->GetGUID()))
                    err = ERR_BATTLEGROUND_JOIN_FAILED;
            });
    }

    if (err <= 0)
        return Acore::StringFormat("the core refused the group (result {})", static_cast<int32>(err));

    bgt->SetRated(rated);
    GroupQueueInfo* ginfo = queue.AddGroup(leader, group, bgTypeId, bracketEntry, arenaType, rated, false, arenaRating,
                                           matchmakerRating, teamId, previousOpponents);
    uint32 const averageWait = queue.GetAverageQueueWaitTime(ginfo);
    WorldPacket data;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;

        uint32 const queueSlot = member->AddBattlegroundQueueId(queueTypeId);
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgt, queueSlot, STATUS_WAIT_QUEUE, averageWait, 0,
                                                        arenaType, TEAM_NEUTRAL, rated);
        member->SendDirectMessage(&data);
        sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, err);
        member->SendDirectMessage(&data);
        sScriptMgr->OnPlayerJoinArena(member);
    }

    sBattlegroundMgr->ScheduleQueueUpdate(matchmakerRating, arenaType, queueTypeId, bgTypeId,
                                          bracketEntry->GetBracketId());
    return {};
}

void Matchmaker::OnAddGroup(Player* leader, Group* group, uint8 arenaType, bool rated, uint32 matchmakerRating)
{
    if (!sPlayerbotAIConfig.LadderMatchmakerEnable || arenaType != ARENA_TYPE_2v2 || !IsRealPlayer(leader))
        return;

    if (!rated && !sPlayerbotAIConfig.LadderMatchmakerSkirmish)
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("Matchmaker::OnAddGroup");
    Incoming incoming;
    incoming.Leader = leader->GetGUID();
    incoming.ArenaType = arenaType;
    incoming.Rated = rated;
    incoming.Mmr = matchmakerRating;
    uint32 size = 1;
    if (group)
    {
        size = group->GetMembersCount();
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
            if (IsRealPlayer(ObjectAccessor::FindConnectedPlayer(slot.guid)))
                incoming.RealMembers.push_back(slot.guid);
    }
    else
        incoming.RealMembers.push_back(leader->GetGUID());

    char const* kind = rated ? "rated" : "skirmish";
    if (size < arenaType)
    {
        LOG_INFO("playerbots", "Matchmaker: {} queued {} {}v{} with {} of {} players: not answered", leader->GetName(),
                 kind, arenaType, arenaType, size, arenaType);
        return;
    }

    LOG_INFO("playerbots", "Matchmaker: {} queued {} {}v{} (mmr {})", leader->GetName(), kind, arenaType, arenaType,
             matchmakerRating);
    std::lock_guard<std::mutex> guard(_incomingLock);
    _incoming.push_back(std::move(incoming));
    _hasIncoming.store(true, std::memory_order_relaxed);
}

void Matchmaker::Update(uint32 diff)
{
    if (_requests.empty() && !_hasIncoming.load(std::memory_order_relaxed))
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("Matchmaker::Update");
    _checkTimer += diff;
    if (_checkTimer < CheckIntervalMs)
        return;

    _checkTimer = 0;
    uint32 const now = getMSTime();
    TakeIncoming(now);
    std::erase_if(_requests, [this, now](Request& request) { return !Step(request, now); });
}

void Matchmaker::TakeIncoming(uint32 now)
{
    if (!_hasIncoming.load(std::memory_order_relaxed))
        return;

    std::vector<Incoming> incoming;
    {
        std::lock_guard<std::mutex> guard(_incomingLock);
        incoming.swap(_incoming);
        _hasIncoming.store(false, std::memory_order_relaxed);
    }

    uint32 const minDelay = sPlayerbotAIConfig.LadderMatchmakerDelayMinSeconds;
    uint32 const maxDelay = sPlayerbotAIConfig.LadderMatchmakerDelayMaxSeconds;
    for (Incoming& entry : incoming)
    {
        Request request;
        request.Source = std::move(entry);
        Player* leader = ObjectAccessor::FindConnectedPlayer(request.Source.Leader);
        request.LeaderName = leader ? leader->GetName() : request.Source.Leader.ToString();
        request.QueuedAtMs = now;
        request.PhaseSinceMs = now;
        request.DelayMs = urand(minDelay, maxDelay) * IN_MILLISECONDS;
        // At most one matchmade team per real group: a new queue replaces the earlier answer that has not played
        auto const it =
            std::find_if(_requests.begin(), _requests.end(), [&request](Request const& other)
                         { return other.Source.Leader == request.Source.Leader && other.Current != Phase::Playing; });
        if (it == _requests.end())
        {
            _requests.push_back(std::move(request));
            continue;
        }

        SetOutcome(it->PickSerial, "replaced by a new queue");
        Release(*it, it->Current == Phase::Queued);
        *it = std::move(request);
    }
}

bool Matchmaker::Step(Request& request, uint32 now)
{
    BattlegroundQueueTypeId const queueTypeId = ArenaQueueType(request.Source.ArenaType);
    Player* leader = ObjectAccessor::FindConnectedPlayer(request.Source.Leader);
    bool const playerWaits = leader && WaitsInQueue(leader, queueTypeId);
    bool const playerMatched = leader && InvitedOrPlaying(leader, queueTypeId);
    uint32 const inPhaseMs = getMSTimeDiff(request.PhaseSinceMs, now);
    uint32 const timeoutMs = sPlayerbotAIConfig.LadderMatchmakerTimeoutSeconds * IN_MILLISECONDS;
    Player* captain = request.Captain ? ObjectAccessor::FindConnectedPlayer(request.Captain) : nullptr;

    // The answer failed: the team leaves, the request waits for another pick (the team counts as a recent opponent)
    auto const retry = [this, &request, now](std::string const& reason)
    {
        LOG_INFO("playerbots", "Matchmaker: team #{} withdrawn for {}: {}", request.TeamId, request.LeaderName, reason);
        SetOutcome(request.PickSerial, reason);
        Release(request, true);
        if (request.Picks >= sPlayerbotAIConfig.LadderMatchmakerMaxPicks)
        {
            LOG_INFO("playerbots", "Matchmaker: {} gets no answer after {} picks", request.LeaderName, request.Picks);
            return false;
        }

        request.Current = Phase::Waiting;
        request.PhaseSinceMs = now;
        request.DelayMs = 0;
        return true;
    };

    switch (request.Current)
    {
        case Phase::Waiting:
            if (!playerWaits)
            {
                LOG_INFO("playerbots", "Matchmaker: {} left the queue before a pick", request.LeaderName);
                return false;
            }

            if (inPhaseMs >= request.DelayMs)
                TryPick(request, leader, now);

            return true;
        case Phase::Forming:
        {
            if (!playerWaits)
            {
                LOG_INFO("playerbots", "Matchmaker: {} left the queue; team #{} released", request.LeaderName,
                         request.TeamId);
                SetOutcome(request.PickSerial, "the player left the queue");
                Release(request, false);
                return false;
            }

            ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(request.TeamId);
            if (!captain || !team)
                return retry("the captain or the team is gone");

            if (LeadsCompleteArenaTeamGroup(captain, team, request.Source.ArenaType))
            {
                std::string const error = JoinArenaQueue(captain, request.Source.ArenaType, request.Source.Rated);
                if (!error.empty())
                    return retry("the queue refused the team: " + error);

                request.Current = Phase::Queued;
                request.PhaseSinceMs = now;
                SetOutcome(request.PickSerial, "queued");
                LOG_INFO("playerbots", "Matchmaker: team #{} <{}> queued {} for {}", request.TeamId, team->GetName(),
                         request.Source.Rated ? "rated" : "skirmish", request.LeaderName);
                return true;
            }

            if (inPhaseMs >= timeoutMs)
                return retry("its group did not form in time");

            return true;
        }
        case Phase::Queued:
            if (captain && InvitedOrPlaying(captain, queueTypeId))
            {
                uint32 const waited = getMSTimeDiff(request.QueuedAtMs, now) / IN_MILLISECONDS;
                request.Current = Phase::Playing;
                request.PhaseSinceMs = now;
                SetOutcome(request.PickSerial, Acore::StringFormat("match {} s after the player's queue", waited));
                LOG_INFO("playerbots", "Matchmaker: {} meets team #{} {} s after queuing", request.LeaderName,
                         request.TeamId, waited);
                return true;
            }

            if (playerMatched)
            {
                SetOutcome(request.PickSerial, "the player was paired with another group");
                Release(request, true);
                return false;
            }

            if (!playerWaits)
            {
                LOG_INFO("playerbots", "Matchmaker: {} left the queue; team #{} withdrawn", request.LeaderName,
                         request.TeamId);
                SetOutcome(request.PickSerial, "the player left the queue");
                Release(request, true);
                return false;
            }

            if (!captain || !WaitsInQueue(captain, queueTypeId))
                return retry("the team left the queue");

            if (inPhaseMs >= timeoutMs)
                return retry("no match within the timeout");

            return true;
        case Phase::Playing:
            for (ObjectGuid const& guid : request.Members)
                if (Player* member = ObjectAccessor::FindConnectedPlayer(guid))
                    if (InvitedOrPlaying(member, queueTypeId))
                        return true;

            LOG_INFO("playerbots", "Matchmaker: team #{} is free again (match over)", request.TeamId);
            Release(request, false);
            return false;
    }

    return false;
}

void Matchmaker::TryPick(Request& request, Player* leader, uint32 now)
{
    std::vector<MatchmakerCandidate> candidates;
    std::vector<CandidateTeam> teams;
    BuildCandidates(leader, request.Source.ArenaType, candidates, teams, nullptr);
    uint32 const waitSeconds = getMSTimeDiff(request.QueuedAtMs, now) / IN_MILLISECONDS;
    MatchmakerPick const pick = Pick(request.Source.Mmr, request.Source.Rated, waitSeconds, candidates,
                                     RecentFor(request), Settings(), frand(0.0f, 1.0f));
    request.PhaseSinceMs = now;
    if (pick.Index < 0)
    {
        if (request.Note.empty())
            LOG_INFO("playerbots", "Matchmaker: no free team for {} ({} pool teams of the type); retrying",
                     request.LeaderName, candidates.size());

        request.Note = "no free team";
        request.DelayMs = NoCandidateRetryMs;
        return;
    }

    ArenaTeam* team = teams[pick.Index].Team;
    uint32 const teamMmr = candidates[pick.Index].Mmr;
    request.Note.clear();
    ++request.Picks;
    request.TeamId = team->GetId();
    request.Members = teams[pick.Index].Members;
    request.Captain = request.Members.front();
    std::vector<ObjectGuid> const others(request.Members.begin() + 1, request.Members.end());

    _reserved.insert(request.TeamId);
    uint32 const keep = sPlayerbotAIConfig.LadderMatchmakerRecentOpponents;
    for (ObjectGuid const& guid : request.Source.RealMembers)
    {
        std::vector<uint32>& recent = _recent[guid];
        recent.push_back(request.TeamId);
        if (recent.size() > keep)
            recent.erase(recent.begin(), recent.end() - keep);
    }

    PickRecord record;
    record.Serial = ++_pickSerial;
    record.At = UnixNow();
    record.Player = request.LeaderName;
    record.TeamId = request.TeamId;
    record.TeamName = team->GetName();
    record.Rated = request.Source.Rated;
    record.PlayerMmr = request.Source.Mmr;
    record.TeamMmr = teamMmr;
    record.Window = pick.Window;
    record.Eligible = pick.Eligible;
    record.WaitSeconds = waitSeconds;
    record.Outcome = "forming its group";
    request.PickSerial = record.Serial;
    _picks.push_front(std::move(record));
    if (_picks.size() > MaxPickRecords)
        _picks.pop_back();

    request.Current = Phase::Forming;
    PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<ArenaGroupFormationOperation>(
        request.Captain, others, static_cast<uint32>(request.Source.ArenaType), request.TeamId, team->GetName()));
    LOG_INFO("playerbots",
             "Matchmaker: picked team #{} <{}> for {} ({}, mmr distance {}, window {}, {} eligible, waited {} s)",
             request.TeamId, team->GetName(), request.LeaderName, request.Source.Rated ? "rated" : "skirmish",
             request.Source.Rated ? Distance(request.Source.Mmr, teamMmr) : 0, pick.Window, pick.Eligible, waitSeconds);
}

void Matchmaker::Release(Request& request, bool dequeue)
{
    if (!request.TeamId)
        return;

    if (dequeue)
        for (ObjectGuid const& guid : request.Members)
            if (Player* member = ObjectAccessor::FindConnectedPlayer(guid))
                LadderMgr::LeaveBattlegroundQueues(member, "the matchmaker withdrew its team");

    // Dissolve the group the matchmaker formed, if it still stands as formed
    Player* captain = ObjectAccessor::FindConnectedPlayer(request.Captain);
    Group* group = captain && !captain->InBattleground() ? captain->GetGroup() : nullptr;
    if (group && group->GetLeaderGUID() == request.Captain &&
        std::all_of(
            group->GetMemberSlots().begin(), group->GetMemberSlots().end(), [&request](Group::MemberSlot const& slot)
            { return std::find(request.Members.begin(), request.Members.end(), slot.guid) != request.Members.end(); }))
        group->Disband();

    _reserved.erase(request.TeamId);
    request.TeamId = 0;
    request.Captain.Clear();
    request.Members.clear();
}

void Matchmaker::SetOutcome(uint32 serial, std::string const& outcome)
{
    for (PickRecord& record : _picks)
    {
        if (record.Serial == serial)
        {
            record.Outcome = outcome;
            return;
        }
    }
}

std::vector<uint32> Matchmaker::RecentFor(Request const& request) const
{
    // The leader's list last: its newest entry is the opponent of the leader's last match
    std::vector<uint32> recent;
    for (ObjectGuid const& guid : request.Source.RealMembers)
        if (guid != request.Source.Leader)
            if (auto const it = _recent.find(guid); it != _recent.end())
                recent.insert(recent.end(), it->second.begin(), it->second.end());

    if (auto const it = _recent.find(request.Source.Leader); it != _recent.end())
        recent.insert(recent.end(), it->second.begin(), it->second.end());

    return recent;
}

MatchmakerSettings Matchmaker::Settings()
{
    MatchmakerSettings settings;
    settings.MmrWidth = sPlayerbotAIConfig.LadderMatchmakerMmrWidth;
    settings.Window = sPlayerbotAIConfig.LadderMatchmakerWindow;
    settings.WindowGrowth = sPlayerbotAIConfig.LadderMatchmakerWindowGrowth;
    return settings;
}

char const* Matchmaker::PhaseName(Phase phase)
{
    switch (phase)
    {
        case Phase::Waiting:
            return "waiting";
        case Phase::Forming:
            return "forming";
        case Phase::Queued:
            return "queued";
        case Phase::Playing:
            return "playing";
    }

    return "?";
}

void Matchmaker::BuildCandidates(Player* leader, uint8 arenaType, std::vector<MatchmakerCandidate>& candidates,
                                 std::vector<CandidateTeam>& teams, std::vector<std::string>* reasons)
{
    Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA);
    PvPDifficultyEntry const* leaderBracket =
        leader && bgt ? GetBattlegroundBracketByLevel(bgt->GetMapId(), leader->GetLevel()) : nullptr;
    LadderMgr& ladder = LadderMgr::Instance();
    bool const poolOnly = sPlayerbotAIConfig.LadderMatchmakerPoolOnly;
    for (auto const& [id, team] : sArenaTeamMgr->GetArenaTeams())
    {
        if (team->GetType() != arenaType || (poolOnly ? !LadderMgr::IsPoolTeam(team) : !IsRandomBotTeam(team)))
            continue;

        MatchmakerCandidate candidate;
        candidate.TeamId = id;
        CandidateTeam entry;
        entry.Team = team;
        std::string reason;
        if (Instance().IsReserved(id))
            reason = "picked by the matchmaker";
        else if (ladder.IsMoving(id))
            reason = "moving between servers";
        else if (LadderMgr::IsTeamBusy(team))
            reason = "a member is in a battleground or its queue";
        else
        {
            std::vector<ArenaTeamMember const*> order;
            for (ArenaTeamMember const& member : team->GetMembers())
            {
                if (member.Guid == team->GetCaptain())
                    order.insert(order.begin(), &member);
                else
                    order.push_back(&member);
            }

            uint32 mmrSum = 0;
            for (ArenaTeamMember const* member : order)
            {
                if (entry.Members.size() >= arenaType)
                    break;

                Player* player = ObjectAccessor::FindConnectedPlayer(member->Guid);
                char const* busy = !player || !player->IsInWorld() ? "is offline" : BusyReason(player);
                if (!busy && leaderBracket)
                {
                    PvPDifficultyEntry const* bracket =
                        GetBattlegroundBracketByLevel(bgt->GetMapId(), player->GetLevel());
                    if (!bracket || bracket->GetBracketId() != leaderBracket->GetBracketId())
                        busy = "is in another level bracket";
                }

                if (busy)
                {
                    if (reason.empty())
                        reason = Acore::StringFormat("{} {}", member->Name, busy);

                    continue;
                }

                entry.Members.push_back(member->Guid);
                mmrSum += member->MatchMakerRating;
            }

            if (entry.Members.size() >= arenaType)
            {
                reason.clear();
                candidate.Mmr = mmrSum / arenaType;
            }
            else if (reason.empty())
                reason = "too few members";
        }

        if (!reason.empty())
            candidate.Mmr = AverageMmr(team);

        candidate.Busy = !reason.empty();
        std::string pinReason;
        candidate.Pinned = ladder.IsTeamPinned(team, reasons ? &pinReason : nullptr);
        if (reasons)
            reasons->push_back(candidate.Busy ? "busy: " + reason : candidate.Pinned ? "pinned: " + pinReason : "free");

        candidates.push_back(candidate);
        teams.push_back(std::move(entry));
    }
}

bool Matchmaker::HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    PLAYERBOT_CHECK_WORLD_THREAD("Matchmaker::HandleStatusCommand");
    Matchmaker& matchmaker = Instance();
    handler->PSendSysMessage(
        "Matchmaker {}: skirmish {}, delay {}-{} s, window {} + {}/s, width {}, {} recent opponents skipped, timeout "
        "{} s, {} picks per queue, {}; {} requests, {} teams picked",
        sPlayerbotAIConfig.LadderMatchmakerEnable ? "on" : "off",
        sPlayerbotAIConfig.LadderMatchmakerSkirmish ? "on" : "off", sPlayerbotAIConfig.LadderMatchmakerDelayMinSeconds,
        sPlayerbotAIConfig.LadderMatchmakerDelayMaxSeconds, sPlayerbotAIConfig.LadderMatchmakerWindow,
        sPlayerbotAIConfig.LadderMatchmakerWindowGrowth, sPlayerbotAIConfig.LadderMatchmakerMmrWidth,
        sPlayerbotAIConfig.LadderMatchmakerRecentOpponents, sPlayerbotAIConfig.LadderMatchmakerTimeoutSeconds,
        sPlayerbotAIConfig.LadderMatchmakerMaxPicks,
        sPlayerbotAIConfig.LadderMatchmakerPoolOnly ? "pool teams" : "every random bot team",
        matchmaker._requests.size(), matchmaker._reserved.size());

    uint32 const now = getMSTime();
    for (Request const& request : matchmaker._requests)
        handler->PSendSysMessage("  {} {} {}v{} mmr {}: {} for {} s, queued {} s ago, team {}, {} picks{}",
                                 request.LeaderName, request.Source.Rated ? "rated" : "skirmish",
                                 request.Source.ArenaType, request.Source.ArenaType, request.Source.Mmr,
                                 PhaseName(request.Current), getMSTimeDiff(request.PhaseSinceMs, now) / IN_MILLISECONDS,
                                 getMSTimeDiff(request.QueuedAtMs, now) / IN_MILLISECONDS,
                                 request.TeamId ? Acore::StringFormat("#{}", request.TeamId) : "-", request.Picks,
                                 request.Note.empty() ? "" : " (" + request.Note + ")");

    if (matchmaker._picks.empty())
    {
        handler->SendSysMessage("No picks yet");
        return true;
    }

    uint32 const unixNow = UnixNow();
    std::set<uint32> distinct;
    uint32 ratedPicks = 0;
    uint32 distanceSum = 0;
    uint32 distanceMax = 0;
    handler->SendSysMessage("Last picks, newest first:");
    for (PickRecord const& record : matchmaker._picks)
    {
        uint32 const distance = Distance(record.PlayerMmr, record.TeamMmr);
        distinct.insert(record.TeamId);
        if (record.Rated)
        {
            ++ratedPicks;
            distanceSum += distance;
            distanceMax = std::max(distanceMax, distance);
        }

        handler->PSendSysMessage(
            "  {} s ago: {} vs #{} <{}> {}, mmr {} vs {} (distance {}, window {}, {} eligible, waited {} s): {}",
            unixNow - record.At, record.Player, record.TeamId, record.TeamName, record.Rated ? "rated" : "skirmish",
            record.PlayerMmr, record.TeamMmr, record.Rated ? distance : 0, record.Window, record.Eligible,
            record.WaitSeconds, record.Outcome);
    }

    handler->PSendSysMessage("{} picks listed, {} distinct teams; rated mmr distance mean {}, max {}",
                             matchmaker._picks.size(), distinct.size(), ratedPicks ? distanceSum / ratedPicks : 0,
                             distanceMax);
    return true;
}

bool Matchmaker::HandleCandidatesCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("Matchmaker::HandleCandidatesCommand");
    Optional<uint32> const mmr = args && *args ? Acore::StringTo<uint32>(args) : Optional<uint32>();
    if (args && *args && !mmr)
    {
        handler->SendSysMessage("Usage: .playerbots ladder matchmaker candidates [mmr]");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::vector<MatchmakerCandidate> candidates;
    std::vector<CandidateTeam> teams;
    std::vector<std::string> reasons;
    BuildCandidates(nullptr, ARENA_TYPE_2v2, candidates, teams, &reasons);
    std::vector<size_t> order(candidates.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;

    uint32 const center = mmr.value_or(0);
    std::sort(order.begin(), order.end(), [&candidates, center](size_t a, size_t b)
              { return Distance(candidates[a].Mmr, center) < Distance(candidates[b].Mmr, center); });

    uint32 freeTeams = 0;
    for (size_t i : order)
    {
        MatchmakerCandidate const& candidate = candidates[i];
        if (!candidate.Busy && !candidate.Pinned)
            ++freeTeams;

        handler->PSendSysMessage("  #{} <{}> mmr {}{}: {}", candidate.TeamId, teams[i].Team->GetName(), candidate.Mmr,
                                 mmr ? Acore::StringFormat(" (distance {})", Distance(candidate.Mmr, center)) : "",
                                 reasons[i]);
    }

    handler->PSendSysMessage("{} 2v2 teams, {} free (no level bracket check without a player)", candidates.size(),
                             freeTeams);
    return true;
}

bool Matchmaker::HandleSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    uint32 checks = 0;
    uint32 failures = 0;
    auto const check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("MATCHMAKER selftest FAILED: {}", what);
    };

    constexpr uint32 Draws = 1000;
    constexpr uint32 PlayerMmr = 1500;
    MatchmakerSettings settings;
    settings.MmrWidth = 100;
    settings.Window = 200;
    settings.WindowGrowth = 10;
    // Teams 100-109 at MMR 1000-1900 (team 105 at the player's MMR), a busy team at 1500, a pinned one at 1510
    std::vector<MatchmakerCandidate> candidates;
    for (uint32 i = 0; i < 10; ++i)
        candidates.push_back({100 + i, 1000 + i * 100, false, false});

    candidates.push_back({200, 1500, true, false});
    candidates.push_back({201, 1510, false, true});

    // Evenly spread rolls: the counts are the weights' shares
    auto const draw = [&](uint32 mmr, bool rated, uint32 wait, std::vector<uint32> const& recent,
                          std::vector<MatchmakerCandidate> const& pool, uint32& maxDistance)
    {
        std::map<uint32, uint32> counts;
        maxDistance = 0;
        for (uint32 i = 0; i < Draws; ++i)
        {
            MatchmakerPick const pick =
                Pick(mmr, rated, wait, pool, recent, settings, (static_cast<float>(i) + 0.5f) / Draws);
            if (pick.Index < 0)
                continue;

            MatchmakerCandidate const& picked = pool[pick.Index];
            ++counts[picked.TeamId];
            maxDistance = std::max(maxDistance, Distance(mmr, picked.Mmr));
        }

        return counts;
    };

    uint32 maxDistance = 0;
    std::map<uint32, uint32> counts = draw(PlayerMmr, true, 0, {}, candidates, maxDistance);
    check(!counts.contains(200) && !counts.contains(201), "a busy or pinned team was picked");
    check(counts[105] > counts[104] && counts[105] > counts[106], "the nearest team is not the most likely");
    check(counts[104] > counts[103] && counts[106] > counts[107], "the weight does not fall off with the distance");
    check(maxDistance <= 200, Acore::StringFormat("a pick at distance {} outside the window 200", maxDistance));
    check(Pick(PlayerMmr, true, 0, candidates, {}, settings, 0.5f).Window == 200, "window at wait 0 is not 200");
    check(Pick(PlayerMmr, true, 30, candidates, {}, settings, 0.5f).Window == 500, "window at wait 30 s is not 500");
    uint32 maxDistanceLater = 0;
    draw(PlayerMmr, true, 30, {}, candidates, maxDistanceLater);
    check(maxDistanceLater > 200 && maxDistanceLater <= 500,
          Acore::StringFormat("after 30 s the farthest pick is at {} (want 201-500)", maxDistanceLater));
    handler->PSendSysMessage(
        "MATCHMAKER selftest weights, mmr {} wait 0: #103 {} #104 {} #105 {} #106 {} #107 {}; "
        "wait 30 s: farthest pick at distance {}",
        PlayerMmr, counts[103], counts[104], counts[105], counts[106], counts[107], maxDistanceLater);

    // An empty window doubles until it holds a team: 200 -> 1600 reaches 1400-1900 from 3000
    MatchmakerPick const distant = Pick(3000, true, 0, candidates, {}, settings, 0.5f);
    check(distant.Index >= 0 && distant.Window == 1600 && distant.Eligible == 6,
          Acore::StringFormat("distant player: index {} window {} eligible {}", distant.Index, distant.Window,
                              distant.Eligible));

    counts = draw(PlayerMmr, true, 0, {105, 104, 106}, candidates, maxDistance);
    check(!counts.contains(104) && !counts.contains(105) && !counts.contains(106) && counts.size() == 2,
          "a recent opponent was picked");

    // Only recent teams free: every one but the newest may be drawn
    std::vector<MatchmakerCandidate> const recentOnly = {
        {1, 1500, false, false}, {2, 1500, false, false}, {3, 1500, false, false}, {4, 1500, true, false}};
    counts = draw(PlayerMmr, true, 0, {1, 2, 3}, recentOnly, maxDistance);
    check(!counts.contains(3) && !counts.contains(4) && counts[1] > 0 && counts[2] > 0,
          "with only recent teams free the newest was picked, or none");

    std::vector<MatchmakerCandidate> busyOnly = candidates;
    for (MatchmakerCandidate& candidate : busyOnly)
        candidate.Busy = true;

    check(Pick(PlayerMmr, true, 600, busyOnly, {}, settings, 0.5f).Index < 0, "a pick among busy teams only");

    // Skirmish: no MMR weight, every free team equally likely
    counts = draw(0, false, 0, {}, candidates, maxDistance);
    bool even = counts.size() == 10;
    for (auto const& [teamId, count] : counts)
        even = even && count >= 90 && count <= 110;

    check(even, "skirmish picks are not uniform over the free teams");

    // Variety: picks in a row with the last 3 opponents skipped, random rolls (fixed seed)
    uint32 seed = 12345;
    auto const roll = [&seed]()
    {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };

    std::vector<uint32> recent;
    std::map<uint32, uint32> variety;
    uint32 repeats = 0;
    uint32 distanceSum = 0;
    for (uint32 i = 0; i < Draws; ++i)
    {
        MatchmakerPick const pick = Pick(PlayerMmr, true, 0, candidates, recent, settings, roll());
        if (pick.Index < 0)
            continue;

        uint32 const teamId = candidates[pick.Index].TeamId;
        if (!recent.empty() && recent.back() == teamId)
            ++repeats;

        ++variety[teamId];
        distanceSum += Distance(PlayerMmr, candidates[pick.Index].Mmr);
        recent.push_back(teamId);
        if (recent.size() > 3)
            recent.erase(recent.begin());
    }

    uint32 most = 0;
    std::string listing;
    for (auto const& [teamId, count] : variety)
    {
        most = std::max(most, count);
        listing += Acore::StringFormat(" #{} (mmr {}): {}", teamId, 1000 + (teamId - 100) * 100, count);
    }

    check(repeats == 0, Acore::StringFormat("{} picks repeated the previous opponent", repeats));
    check(variety.size() >= 5 && most <= Draws / 2,
          Acore::StringFormat("{} distinct opponents, the most frequent {} times", variety.size(), most));
    handler->PSendSysMessage(
        "MATCHMAKER selftest variety, {} picks in a row (mmr {}, 3 recent skipped, mean distance "
        "{}):{}",
        Draws, PlayerMmr, distanceSum / Draws, listing);
    handler->PSendSysMessage("MATCHMAKER selftest: {} checks, {} failed", checks, failures);
    return true;
}

// Records a real player's group queue; the matchmaker's own teams pass through the filter (bot leaders)
class MatchmakerBattlegroundScript : public AllBattlegroundScript
{
public:
    MatchmakerBattlegroundScript()
        : AllBattlegroundScript("MatchmakerBattlegroundScript", {ALLBATTLEGROUNDHOOK_ON_ADD_GROUP})
    {
    }

    void OnAddGroup(BattlegroundQueue* /*queue*/, GroupQueueInfo* /*ginfo*/, uint32& /*index*/, Player* leader,
                    Group* group, BattlegroundTypeId /*bgTypeId*/, PvPDifficultyEntry const* /*bracketEntry*/,
                    uint8 arenaType, bool isRated, bool /*isPremade*/, uint32 /*arenaRating*/, uint32 matchmakerRating,
                    uint32 /*arenaTeamId*/, uint32 /*opponentsArenaTeamId*/) override
    {
        Matchmaker::Instance().OnAddGroup(leader, group, arenaType, isRated, matchmakerRating);
    }
};

void AddSC_matchmaker() { new MatchmakerBattlegroundScript(); }
