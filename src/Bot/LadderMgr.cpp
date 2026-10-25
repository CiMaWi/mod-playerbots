/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LadderMgr.h"

#include <algorithm>
#include <array>

#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "RandomPlayerbotMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Timer.h"

namespace
{
constexpr uint32 CheckIntervalMs = 1000;
constexpr uint8 ArenaSlotCount = 3;  // 2v2, 3v3, 5v5

Optional<uint32> ParseTeamId(char const* args)
{
    if (!args || !*args)
        return {};

    std::string_view text(args);
    text = text.substr(0, text.find(' '));
    return Acore::StringTo<uint32>(text);
}

bool Refuse(ChatHandler* handler, std::string const& message)
{
    handler->PSendSysMessage("{}", message);
    handler->SetSentErrorMessage(true);
    return false;
}

char const* RoleName(uint32 role)
{
    switch (role)
    {
        case LADDER_ROLE_MAIN:
            return "main";
        case LADDER_ROLE_ARENA:
            return "arena";
        default:
            return "off";
    }
}

constexpr char const* MovesOffText =
    "Ladder moves are off (Playerbots.Ladder.Enable = 0 and Playerbots.Ladder.Role = 0)";
}  // namespace

bool LadderMgr::MovesOn()
{
    return sPlayerbotAIConfig.LadderEnable || sPlayerbotAIConfig.LadderRole != LADDER_ROLE_OFF;
}

void LadderMgr::LoadRoster()
{
    // Runs in OnBeforeWorldInitialized, before the world thread is known to the thread checks; a config reload keeps
    // the first load
    uint32 const role = sPlayerbotAIConfig.LadderRole;
    if (role == LADDER_ROLE_OFF || _rosterLoaded)
        return;

    _rosterLoaded = true;
    _hostedOnly = role == LADDER_ROLE_ARENA;
    uint32 const start = getMSTime();
    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_LADDER_ROSTER_ALL);
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const guid = fields[0].Get<uint32>();
            uint32 const teamId = fields[1].Get<uint32>();
            if (fields[2].Get<uint8>() == ROSTER_AWAY)
            {
                _away.insert(guid);
                _awayTeams.insert(teamId);
            }
            else if (_hostedOnly)
            {
                _hosted.insert(guid);
                _hostedTeams.insert(teamId);
            }
        } while (result->NextRow());
    }

    // Main keeps its own teams but the away ones; an arena instance keeps only the hosted ones
    std::vector<ArenaTeam*> dropped;
    for (auto const& [id, team] : sArenaTeamMgr->GetArenaTeams())
        if (_hostedOnly ? !_hostedTeams.contains(id) : _awayTeams.contains(id))
            dropped.push_back(team);

    for (ArenaTeam* team : dropped)
        DropTeam(team);

    for (uint32 const teamId : _awayTeams)
    {
        Move& move = _moves[teamId];
        move.Current = State::Departed;  // not busy, so no SetState
        move.Note = "away (roster)";
    }

    uint32 missing = 0;
    for (uint32 const teamId : _hostedTeams)
        if (!sArenaTeamMgr->GetArenaTeamById(teamId))
            ++missing;

    LOG_INFO("server.loading",
             ">> Ladder role {}: roster hosts {} bots in {} teams, {} bots in {} teams away; {} teams dropped from "
             "memory in {} ms",
             RoleName(role), _hosted.size(), _hostedTeams.size(), _away.size(), _awayTeams.size(), dropped.size(),
             GetMSTimeDiffToNow(start));
    if (_hostedOnly && _hosted.empty())
        LOG_WARN("server.loading",
                 "Playerbots.Ladder.Role = 2 and the ladder roster hosts no bot: no random bot logs in");

    if (missing)
        LOG_WARN("server.loading", "Ladder roster: {} hosted teams have no arena_team row", missing);
}

void LadderMgr::LoginHosted()
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::LoginHosted");
    for (uint32 const low : _hosted)
        sRandomPlayerbotMgr.LadderLogin(low);
}

// No DB write; the caller saves first where the team changed in memory
void LadderMgr::DropTeam(ArenaTeam* team)
{
    uint32 const teamId = team->GetId();
    uint8 const slot = team->GetSlot();
    sArenaTeamMgr->RemoveArenaTeam(teamId);
    for (ArenaTeamMember const& member : team->GetMembers())
        if (sCharacterCache->GetCharacterArenaTeamIdByGuid(member.Guid, slot) == teamId)
            sCharacterCache->UpdateCharacterArenaTeamId(member.Guid, slot, 0);

    delete team;
}

bool LadderMgr::IsBusy(State state)
{
    return state == State::Departing || state == State::Saving || state == State::Arriving;
}

char const* LadderMgr::StateName(State state)
{
    switch (state)
    {
        case State::Departing:
            return "departing";
        case State::Saving:
            return "departing (saving)";
        case State::Departed:
            return "departed";
        case State::Arriving:
            return "arriving";
        case State::Hosted:
            return "hosted";
        case State::Failed:
            return "failed";
    }

    return "unknown";
}

bool LadderMgr::GroupedWithRealPlayer(Player* player)
{
    Group* group = player->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && !GET_PLAYERBOT_AI(member))
            return true;
    }

    return false;
}

void LadderMgr::SetState(Move& move, State state)
{
    bool const wasBusy = IsBusy(move.Current);
    bool const isBusy = IsBusy(state);
    move.Current = state;
    if (isBusy && !wasBusy)
        ++_busy;
    else if (wasBusy && !isBusy && _busy)
        --_busy;
}

void LadderMgr::SetBlocked(uint32 teamId, bool blocked)
{
    std::lock_guard<std::mutex> guard(_blockedLock);
    if (blocked)
        _blockedTeams.insert(teamId);
    else
        _blockedTeams.erase(teamId);

    _anyBlocked.store(!_blockedTeams.empty(), std::memory_order_relaxed);
}

bool LadderMgr::IsQueueBlocked(Player* bot)
{
    if (!_anyBlocked.load(std::memory_order_relaxed))
        return false;

    std::lock_guard<std::mutex> guard(_blockedLock);
    for (uint8 slot = 0; slot < ArenaSlotCount; ++slot)
    {
        uint32 const teamId = bot->GetArenaTeamId(slot);
        if (teamId && _blockedTeams.contains(teamId))
            return true;
    }

    return false;
}

// Lets the members of a move log in again through the random bot manager (arena role: hosts them)
void LadderMgr::ReleaseMembers(uint32 teamId, Move& move)
{
    for (ObjectGuid const& guid : move.Members)
    {
        uint32 const low = guid.GetCounter();
        bool released = _away.erase(low) > 0;
        if (_hostedOnly && _hosted.insert(low).second)
            released = true;

        if (released)
            sRandomPlayerbotMgr.LadderLogin(low);
    }

    _awayTeams.erase(teamId);
    if (_hostedOnly)
        _hostedTeams.insert(teamId);

    move.Members.clear();
    move.LoggedOut = false;
}

// The team is still in memory: its members come back and the roster says hosted again
void LadderMgr::CancelDepart(uint32 teamId, Move& move, State state, std::string const& note)
{
    if (!move.Members.empty())
        WriteRoster(teamId, move.Members, false);

    ReleaseMembers(teamId, move);
    SetBlocked(teamId, false);
    SetState(move, state);
    move.Note = note;
}

// Async playerbots DB writes; nothing with role 0, whose away set lives in memory only
void LadderMgr::WriteRoster(uint32 teamId, std::vector<ObjectGuid> const& members, bool away)
{
    if (!_rosterLoaded)
        return;

    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();
    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_LADDER_ROSTER_TEAM);
    stmt->SetData(0, teamId);
    trans->Append(stmt);
    // Main: no row means hosted
    if (away || _hostedOnly)
    {
        for (ObjectGuid const& guid : members)
        {
            stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_REP_LADDER_ROSTER);
            stmt->SetData(0, guid.GetCounter());
            stmt->SetData(1, teamId);
            stmt->SetData(2, static_cast<uint8>(away ? ROSTER_AWAY : ROSTER_HOSTED));
            trans->Append(stmt);
        }
    }

    PlayerbotsDatabase.CommitTransaction(trans);
}

void LadderMgr::Fail(uint32 teamId, std::string const& reason)
{
    Move& move = _moves[teamId];
    SetState(move, State::Failed);
    move.Note = reason;
    LOG_WARN("playerbots", "Ladder: arena team {} failed: {}", teamId, reason);
}

void LadderMgr::Update(uint32 diff)
{
    if (!_busy)
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::Update");
    _queryProcessor.ProcessReadyCallbacks();

    _checkTimer += diff;
    if (_checkTimer < CheckIntervalMs)
        return;

    uint32 const elapsed = _checkTimer;
    _checkTimer = 0;
    uint32 const timeoutMs = sPlayerbotAIConfig.LadderDepartTimeoutSeconds * IN_MILLISECONDS;
    for (auto& [teamId, move] : _moves)
    {
        if (move.Current != State::Departing)
            continue;

        move.WaitedMs += elapsed;
        if (move.WaitedMs >= timeoutMs)
        {
            // E.g. a member held online by a player's bot manager never leaves ObjectAccessor
            std::string const note =
                Acore::StringFormat("depart cancelled after {} s ({})", move.WaitedMs / IN_MILLISECONDS, move.Note);
            CancelDepart(teamId, move, State::Failed, note);
            LOG_WARN("playerbots", "Ladder: arena team {} failed: {}", teamId, note);
            continue;
        }

        UpdateDeparting(teamId, move);
    }
}

void LadderMgr::UpdateDeparting(uint32 teamId, Move& move)
{
    ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
    if (!team)
    {
        CancelDepart(teamId, move, State::Failed, "the team left memory while departing");
        LOG_WARN("playerbots", "Ladder: arena team {} failed: {}", teamId, move.Note);
        return;
    }

    // A member who joined after the logouts started (recruit) restarts the wait
    if (move.LoggedOut)
    {
        for (ArenaTeamMember const& member : team->GetMembers())
        {
            if (std::find(move.Members.begin(), move.Members.end(), member.Guid) == move.Members.end())
            {
                move.LoggedOut = false;
                break;
            }
        }
    }

    if (!move.LoggedOut)
    {
        for (ArenaTeamMember const& member : team->GetMembers())
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(member.Guid);
            if (!player)
                continue;

            if (player->InBattleground() || player->InBattlegroundQueue())
            {
                move.Note = Acore::StringFormat("waits: {} is queued or in a battleground", member.Name);
                return;
            }

            if (GroupedWithRealPlayer(player))
            {
                move.Note = Acore::StringFormat("waits: {} is grouped with a real player", member.Name);
                return;
            }
        }

        for (ArenaTeamMember const& member : team->GetMembers())
        {
            uint32 const low = member.Guid.GetCounter();
            if (std::find(move.Members.begin(), move.Members.end(), member.Guid) == move.Members.end())
                move.Members.push_back(member.Guid);

            _away.insert(low);
            if (_hostedOnly)
                _hosted.erase(low);

            sRandomPlayerbotMgr.LadderLogout(low);
        }

        _awayTeams.insert(teamId);
        _hostedTeams.erase(teamId);
        WriteRoster(teamId, move.Members, true);
        move.LoggedOut = true;
        move.Note = "waits for the logouts";
        LOG_INFO("playerbots", "Ladder: arena team {} <{}> logs out {} members", teamId, team->GetName(),
                 team->GetMembersSize());
        return;
    }

    for (ObjectGuid const& guid : move.Members)
        if (ObjectAccessor::FindConnectedPlayer(guid))
            return;

    // Every member is offline and saved (their logout saves are queued before the barrier below)
    team->SaveToDB(true);
    std::string const name = team->GetName();
    uint32 const rating = team->GetRating();
    DropTeam(team);
    SetState(move, State::Saving);
    move.Note = Acore::StringFormat("<{}> rating {} dropped; waits for the save barrier", name, rating);

    // One async character DB worker (CharacterDatabase.WorkerThreads = 1): FIFO, so this answer follows every write
    _queryProcessor.AddCallback(CharacterDatabase.AsyncQuery("SELECT 1")
                                    .WithCallback(
                                        [this, teamId, name](QueryResult /*result*/)
                                        {
                                            auto const it = _moves.find(teamId);
                                            if (it == _moves.end() || it->second.Current != State::Saving)
                                                return;

                                            SetState(it->second, State::Departed);
                                            it->second.Note =
                                                Acore::StringFormat("<{}> saved and dropped, {} members away", name,
                                                                    it->second.Members.size());
                                            LOG_INFO("playerbots", "Ladder: arena team {} <{}> departed", teamId, name);
                                        }));
}

void LadderMgr::OnTeamRow(QueryCallback& callback, uint32 teamId, QueryResult result)
{
    if (!result)
    {
        Fail(teamId, "no arena_team row");
        return;
    }

    // Checked before the loader runs: LoadMembersFromDB deletes the row of a member without a characters row
    Field* fields = result->Fetch();
    uint64 const members = fields[15].Get<uint64>();
    uint64 const named = fields[16].Get<uint64>();
    uint64 const captain = fields[17].Get<uint64>();
    if (!members)
    {
        Fail(teamId, "no arena_team_member rows");
        return;
    }

    if (named != members)
    {
        Fail(teamId, Acore::StringFormat("{} of {} members have no characters row", members - named, members));
        return;
    }

    if (!captain)
    {
        Fail(teamId, "the captain is no member");
        return;
    }

    _teamRows[teamId] = result;
    callback.SetNextQuery(CharacterDatabase.AsyncQuery(Acore::StringFormat(
        "SELECT atm.arenaTeamId, atm.guid, atm.weekGames, atm.weekWins, atm.seasonGames, atm.seasonWins, c.name, "
        "class, personalRating, matchMakerRating, maxMMR FROM arena_team_member atm"
        " INNER JOIN arena_team ate USING (arenaTeamId)"
        " LEFT JOIN characters AS c ON atm.guid = c.guid"
        " LEFT JOIN character_arena_stats AS cas ON c.guid = cas.guid AND (cas.slot = 0 AND ate.type = 2 OR "
        "cas.slot = 1 AND ate.type = 3 OR cas.slot = 2 AND ate.type = 5)"
        " WHERE atm.arenaTeamId = {}",
        teamId)));
}

void LadderMgr::OnMemberRows(uint32 teamId, QueryResult result)
{
    QueryResult teamRow;
    if (auto const it = _teamRows.find(teamId); it != _teamRows.end())
    {
        teamRow = it->second;
        _teamRows.erase(it);
    }

    if (!teamRow || !result)
    {
        Fail(teamId, "no member rows");
        return;
    }

    if (ArenaTeam* stale = sArenaTeamMgr->GetArenaTeamById(teamId))
    {
        for (ArenaTeamMember const& member : stale->GetMembers())
        {
            if (ObjectAccessor::FindConnectedPlayer(member.Guid))
            {
                Fail(teamId, Acore::StringFormat("the loaded team's member {} is online", member.Name));
                return;
            }
        }

        sArenaTeamMgr->RemoveArenaTeam(teamId);
        delete stale;
    }

    ArenaTeam* team = new ArenaTeam;
    if (!team->LoadArenaTeamFromDB(teamRow) || !team->LoadMembersFromDB(result))
    {
        delete team;
        Fail(teamId, "the loader refused the rows (no captain among the members?)");
        return;
    }

    sArenaTeamMgr->AddArenaTeam(team);
    // Read and restored: the manager has no getter for the next id
    uint32 const next = sArenaTeamMgr->GenerateArenaTeamId();
    sArenaTeamMgr->SetNextArenaTeamId(std::max(next, teamId + 1));

    // Every loaded member is hosted here now, also when main restarted while the team was away
    Move& move = _moves[teamId];
    for (ArenaTeamMember const& member : team->GetMembers())
        if (std::find(move.Members.begin(), move.Members.end(), member.Guid) == move.Members.end())
            move.Members.push_back(member.Guid);

    WriteRoster(teamId, move.Members, false);
    ReleaseMembers(teamId, move);
    SetBlocked(teamId, false);
    SetState(move, State::Hosted);
    move.Note = Acore::StringFormat("<{}> rating {}, {} members loaded", team->GetName(), team->GetRating(),
                                    team->GetMembersSize());
    LOG_INFO("playerbots", "Ladder: arena team {} arrived: {}", teamId, move.Note);
}

bool LadderMgr::HandleDepartCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::HandleDepartCommand");
    if (!MovesOn())
        return Refuse(handler, MovesOffText);

    Optional<uint32> const teamId = ParseTeamId(args);
    if (!teamId)
        return Refuse(handler, "Usage: .playerbots ladder depart <arenaTeamId>");

    LadderMgr& mgr = Instance();
    if (auto const it = mgr._moves.find(*teamId); it != mgr._moves.end() && IsBusy(it->second.Current))
        return Refuse(handler, Acore::StringFormat("Team {} is {}", *teamId, StateName(it->second.Current)));

    ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(*teamId);
    if (!team)
        return Refuse(handler, Acore::StringFormat("Team {} is not loaded", *teamId));

    for (ArenaTeamMember const& member : team->GetMembers())
    {
        // IsRandomBot is false for an offline bot (not in currentBots): test the account
        if (!sPlayerbotAIConfig.IsInRandomAccountList(sCharacterCache->GetCharacterAccountIdByGuid(member.Guid)))
            return Refuse(handler, Acore::StringFormat("Team {}: {} is no random bot", *teamId, member.Name));

        Player* player = ObjectAccessor::FindConnectedPlayer(member.Guid);
        if (player && GroupedWithRealPlayer(player))
            return Refuse(handler,
                          Acore::StringFormat("Team {}: {} is grouped with a real player", *teamId, member.Name));
    }

    Move& move = mgr._moves[*teamId];
    mgr.ReleaseMembers(*teamId, move);
    move.WaitedMs = 0;
    move.Note = "waits until no member is queued or in a battleground";
    mgr.SetState(move, State::Departing);
    mgr.SetBlocked(*teamId, true);
    handler->PSendSysMessage("Team {} <{}> rating {} departing: {}", *teamId, team->GetName(), team->GetRating(),
                             move.Note);
    LOG_INFO("playerbots", "Ladder: arena team {} <{}> departing", *teamId, team->GetName());
    return true;
}

bool LadderMgr::HandleArriveCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::HandleArriveCommand");
    if (!MovesOn())
        return Refuse(handler, MovesOffText);

    Optional<uint32> const teamId = ParseTeamId(args);
    if (!teamId)
        return Refuse(handler, "Usage: .playerbots ladder arrive <arenaTeamId>");

    LadderMgr& mgr = Instance();
    if (auto const it = mgr._moves.find(*teamId); it != mgr._moves.end() && IsBusy(it->second.Current))
    {
        Move& move = it->second;
        if (move.Current != State::Departing)
            return Refuse(handler,
                          Acore::StringFormat("Team {} is {}: {}", *teamId, StateName(move.Current), move.Note));

        // Still in memory, also once the logouts have started: the arrive cancels the depart
        mgr.CancelDepart(*teamId, move, State::Hosted, "depart cancelled");
        handler->PSendSysMessage("Team {}: depart cancelled", *teamId);
        LOG_INFO("playerbots", "Ladder: arena team {}: depart cancelled", *teamId);
        return true;
    }

    if (ArenaTeam* loaded = sArenaTeamMgr->GetArenaTeamById(*teamId))
        for (ArenaTeamMember const& member : loaded->GetMembers())
            if (ObjectAccessor::FindConnectedPlayer(member.Guid))
                return Refuse(handler, Acore::StringFormat("Team {} is loaded and {} is online", *teamId, member.Name));

    Move& move = mgr._moves[*teamId];
    move.WaitedMs = 0;
    move.Note = "loading the rows";
    mgr.SetState(move, State::Arriving);

    // The startup loader's queries (ArenaTeamMgr::LoadArenaTeams) filtered by the team, plus the member checks
    uint32 const id = *teamId;
    std::string const teamSql = Acore::StringFormat(
        "SELECT ate.arenaTeamId, ate.name, ate.captainGuid, ate.type, ate.backgroundColor, ate.emblemStyle, "
        "ate.emblemColor, ate.borderStyle, ate.borderColor, ate.rating, ate.weekGames, ate.weekWins, ate.seasonGames, "
        "ate.seasonWins, ate.`rank`, "
        "(SELECT COUNT(*) FROM arena_team_member m WHERE m.arenaTeamId = ate.arenaTeamId), "
        "(SELECT COUNT(*) FROM arena_team_member m INNER JOIN characters c ON c.guid = m.guid "
        "WHERE m.arenaTeamId = ate.arenaTeamId AND c.name <> ''), "
        "(SELECT COUNT(*) FROM arena_team_member m WHERE m.arenaTeamId = ate.arenaTeamId AND m.guid = ate.captainGuid) "
        "FROM arena_team ate WHERE ate.arenaTeamId = {}",
        id);
    mgr._queryProcessor.AddCallback(
        CharacterDatabase.AsyncQuery(teamSql)
            .WithChainingCallback([&mgr, id](QueryCallback& callback, QueryResult result)
                                  { mgr.OnTeamRow(callback, id, result); })
            .WithCallback([&mgr, id](QueryResult result) { mgr.OnMemberRows(id, result); }));
    handler->PSendSysMessage("Team {} arriving: loading its rows", id);
    LOG_INFO("playerbots", "Ladder: arena team {} arriving", id);
    return true;
}

bool LadderMgr::HandleStatusCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::HandleStatusCommand");
    LadderMgr const& mgr = Instance();
    Optional<uint32> const teamId = ParseTeamId(args);
    std::string const& serverId = sPlayerbotAIConfig.LadderServerId;
    handler->PSendSysMessage("Ladder role {} (server {}); moves {}; {} pending; random bots online {} of {}",
                             RoleName(sPlayerbotAIConfig.LadderRole), serverId.empty() ? "unnamed" : serverId,
                             MovesOn() ? "on" : "off", mgr._busy, sRandomPlayerbotMgr.GetPlayerbotsCount(),
                             sRandomPlayerbotMgr.GetMaxAllowedBotCount());

    uint32 arenas = 0;
    uint32 rated = 0;
    for (Battleground const* bg : sBattlegroundMgr->GetActiveBattlegrounds())
    {
        if (!bg->isArena())
            continue;

        ++arenas;
        if (bg->isRated())
            ++rated;
    }

    std::string const hosted = mgr._hostedOnly ? Acore::StringFormat("roster hosts {} teams ({} bots); ",
                                                                     mgr._hostedTeams.size(), mgr._hosted.size())
                                               : "";
    handler->PSendSysMessage("Arena teams in memory {}; {}away {} teams ({} members); arenas running {} ({} rated)",
                             sArenaTeamMgr->GetArenaTeams().size(), hosted, mgr._awayTeams.size(), mgr._away.size(),
                             arenas, rated);

    std::array<uint32, static_cast<size_t>(State::Failed) + 1> counts{};
    for (auto const& [id, move] : mgr._moves)
        ++counts[static_cast<size_t>(move.Current)];

    handler->PSendSysMessage("Moves: {} departing, {} saving, {} departed, {} arriving, {} hosted, {} failed",
                             counts[static_cast<size_t>(State::Departing)], counts[static_cast<size_t>(State::Saving)],
                             counts[static_cast<size_t>(State::Departed)], counts[static_cast<size_t>(State::Arriving)],
                             counts[static_cast<size_t>(State::Hosted)], counts[static_cast<size_t>(State::Failed)]);
    uint32 shown = 0;
    for (auto const& [id, move] : mgr._moves)
    {
        if (teamId && id != *teamId)
            continue;

        handler->PSendSysMessage("#{} {}: {} ({} s)", id, StateName(move.Current), move.Note, move.WaitedMs / 1000);
        ++shown;
    }

    if (teamId && !shown)
        handler->PSendSysMessage("#{} {}", *teamId, sArenaTeamMgr->GetArenaTeamById(*teamId) ? "hosted" : "not loaded");

    return true;
}
