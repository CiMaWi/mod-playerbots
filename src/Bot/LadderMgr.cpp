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
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "PvpGearMgr.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "World.h"

namespace
{
constexpr uint32 CheckIntervalMs = 1000;
constexpr uint8 ArenaSlotCount = 3;  // 2v2, 3v3, 5v5
constexpr uint32 PoolCheckIntervalMs = 30 * IN_MILLISECONDS;
constexpr uint32 PoolBuildPerCheck = 10;
constexpr uint32 RelogoutIntervalMs = 10 * IN_MILLISECONDS;  // a member still online after the logouts
constexpr size_t MaxContacts = 10000;
constexpr uint32 DefaultGmPinMinutes = 60;
constexpr uint32 MaxGmPinMinutes = 7 * 24 * 60;

uint32 UnixNow() { return static_cast<uint32>(GameTime::GetGameTime().count()); }

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
constexpr char const* PoolOffText =
    "Ladder pool is off (needs Playerbots.Ladder.Role = 1 and Playerbots.Ladder.PoolTeams)";

uint8 MaxLevel() { return static_cast<uint8>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)); }
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
    _poolOn = role == LADDER_ROLE_MAIN;
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
    {
        if (!_hostedOnly)
            NoteTeam(_moves[team->GetId()], team);

        DropTeam(team);
    }

    for (uint32 const teamId : _awayTeams)
    {
        Move& move = _moves[teamId];
        move.Current = State::Departed;  // not busy, so no SetState
        move.Note = "away (roster)";
    }

    // The bot arena team assignment asks PoolWants from the first logins on
    if (PoolOn())
    {
        PoolCount const pool = CountPool();
        _poolShort = pool.Resident + pool.Away < sPlayerbotAIConfig.LadderPoolTeams;
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
    // In a battleground the original group is the player's own one
    for (Group* group : {player->GetGroup(), player->GetOriginalGroup()})
    {
        if (!group)
            continue;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && !GET_PLAYERBOT_AI(member))
                return true;
        }
    }

    return false;
}

// The live part of the pin: what the member does with a real player now, or nullptr
char const* LadderMgr::WithRealPlayer(Player* player)
{
    if (GroupedWithRealPlayer(player))
        return "is grouped with";

    if (player->duel && player->duel->Opponent && IsRealPlayer(player->duel->Opponent))
        return "duels";

    if (Player* trader = player->GetTrader(); trader && IsRealPlayer(trader))
        return "trades with";

    if (Battleground* bg = player->GetBattleground())
        for (auto const& [guid, other] : bg->GetPlayers())
            if (other && IsRealPlayer(other))
                return "is in a battleground or arena with";

    return nullptr;
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
    // Drained: every arena team member is blocked, the set is not needed
    if (_draining.load(std::memory_order_relaxed))
    {
        for (uint8 slot = 0; slot < ArenaSlotCount; ++slot)
            if (bot->GetArenaTeamId(slot))
                return true;

        return false;
    }

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
    if (_poolOn)
        UpdatePool(diff);

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

    // Also once the logouts have started: a contact noted for an offline member, a GM pin
    std::string reason;
    if (IsTeamPinned(team, &reason))
    {
        CancelDepart(teamId, move, State::Failed, Acore::StringFormat("depart cancelled: pinned ({})", reason));
        LOG_INFO("playerbots", "Ladder: arena team {} failed: {}", teamId, move.Note);
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
            if (player && (player->InBattleground() || player->InBattlegroundQueue()))
            {
                move.Note = Acore::StringFormat("waits: {} is queued or in a battleground", member.Name);
                return;
            }
        }

        // The PvP gear comes off here first (an arena server wears it all the time): the worn rows stay on this
        // server, and a character leaving in PvP gear would arrive with its PvE items in the bags
        for (ArenaTeamMember const& member : team->GetMembers())
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(member.Guid);
            if (player && PvpGearMgr::Instance().IsWorn(player) && !PvpGearMgr::Instance().Unwear(player, "depart"))
            {
                move.Note = Acore::StringFormat("waits: {} takes its PvP gear off", member.Name);
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
        move.LogoutIssuedMs = move.WaitedMs;
        move.Note = "waits for the logouts";
        LOG_INFO("playerbots", "Ladder: arena team {} <{}> logs out {} members", teamId, team->GetName(),
                 team->GetMembersSize());
        return;
    }

    // A login in flight when the logouts started can complete after them: such a member is logged out again
    bool const reissue = move.WaitedMs - move.LogoutIssuedMs >= RelogoutIntervalMs;
    bool online = false;
    for (ObjectGuid const& guid : move.Members)
    {
        if (!ObjectAccessor::FindConnectedPlayer(guid))
            continue;

        online = true;
        if (reissue)
            sRandomPlayerbotMgr.LadderLogout(guid.GetCounter());
    }

    if (online)
    {
        if (reissue)
        {
            move.LogoutIssuedMs = move.WaitedMs;
            move.Note = "waits for the logouts (issued again)";
        }

        return;
    }

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
    move.HostedSince = UnixNow();
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

        // The roster has one row per bot, and the away set one team per bot
        for (uint8 slot = 0; slot < ArenaSlotCount; ++slot)
        {
            uint32 const otherTeamId = sCharacterCache->GetCharacterArenaTeamIdByGuid(member.Guid, slot);
            if (slot != team->GetSlot() && otherTeamId)
                return Refuse(handler, Acore::StringFormat("Team {}: {} is also in arena team {}", *teamId, member.Name,
                                                           otherTeamId));
        }
    }

    std::string reason;
    if (mgr.IsTeamPinned(team, &reason))
        return Refuse(handler, Acore::StringFormat("Team {}: pinned ({})", *teamId, reason));

    Move& move = mgr._moves[*teamId];
    mgr.ReleaseMembers(*teamId, move);
    mgr.NoteTeam(move, team);
    move.WaitedMs = 0;
    move.LogoutIssuedMs = 0;
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
    handler->PSendSysMessage("Ladder role {} (server {}); moves {}; {} pending; queues {}; random bots online {} of {}",
                             RoleName(sPlayerbotAIConfig.LadderRole), serverId.empty() ? "unnamed" : serverId,
                             MovesOn() ? "on" : "off", mgr._busy,
                             mgr._draining.load(std::memory_order_relaxed) ? "drained" : "open",
                             sRandomPlayerbotMgr.GetPlayerbotsCount(), sRandomPlayerbotMgr.GetMaxAllowedBotCount());

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
    if (mgr.PoolOn())
    {
        PoolCount const pool = mgr.CountPool();
        handler->PSendSysMessage("Pool {} of {} teams ({} resident, {} away)", pool.Resident + pool.Away,
                                 sPlayerbotAIConfig.LadderPoolTeams, pool.Resident, pool.Away);
    }

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

    ArenaTeam* team = teamId ? sArenaTeamMgr->GetArenaTeamById(*teamId) : nullptr;
    if (teamId && !shown)
        handler->PSendSysMessage("#{} {}", *teamId, team ? "hosted" : "not loaded");

    if (team)
    {
        std::string reason;
        bool const pinned = mgr.IsTeamPinned(team, &reason);
        handler->PSendSysMessage("#{} {}, {}", *teamId, IsTeamBusy(team) ? "busy" : "not busy",
                                 pinned ? "pinned (" + reason + ")" : "not pinned");
    }

    return true;
}

void LadderMgr::NoteContact(uint32 botLowGuid, uint32 now)
{
    if (!sPlayerbotAIConfig.LadderContactMinutes || !MovesOn())
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::NoteContact");
    if (_contacts.size() >= MaxContacts && !_contacts.contains(botLowGuid))
    {
        PruneContacts(now);
        if (_contacts.size() >= MaxContacts)
            return;
    }

    _contacts[botLowGuid] = now;
}

void LadderMgr::PruneContacts(uint32 now)
{
    uint32 const window = sPlayerbotAIConfig.LadderContactMinutes * MINUTE;
    std::erase_if(_contacts, [now, window](auto const& contact) { return now - contact.second >= window; });
    std::erase_if(_gmPins, [now](auto const& pin) { return now >= pin.second; });
}

bool LadderMgr::IsTeamPinned(ArenaTeam* team, std::string* reason) const
{
    uint32 const now = UnixNow();
    if (auto const it = _gmPins.find(team->GetId()); it != _gmPins.end() && now < it->second)
    {
        if (reason)
            *reason = Acore::StringFormat("GM pin, {} s left", it->second - now);

        return true;
    }

    uint32 const window = sPlayerbotAIConfig.LadderContactMinutes * MINUTE;
    for (ArenaTeamMember const& member : team->GetMembers())
    {
        if (Player* player = ObjectAccessor::FindConnectedPlayer(member.Guid))
        {
            if (char const* what = WithRealPlayer(player))
            {
                if (reason)
                    *reason = Acore::StringFormat("{} {} a real player", member.Name, what);

                return true;
            }
        }

        auto const it = _contacts.find(member.Guid.GetCounter());
        if (it != _contacts.end() && now - it->second < window)
        {
            if (reason)
                *reason = Acore::StringFormat("{} had a direct contact with a real player {} s ago", member.Name,
                                              now - it->second);

            return true;
        }
    }

    return false;
}

bool LadderMgr::IsTeamBusy(ArenaTeam* team)
{
    for (ArenaTeamMember const& member : team->GetMembers())
        if (Player* player = ObjectAccessor::FindConnectedPlayer(member.Guid))
            if (player->InBattleground() || player->InBattlegroundQueue())
                return true;

    return false;
}

bool LadderMgr::HandleTeamsCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::HandleTeamsCommand");
    std::string_view const filter = args ? std::string_view(args) : std::string_view();
    bool const onlyPinned = filter == "pinned";
    bool const onlyBusy = filter == "busy";
    if (!filter.empty() && !onlyPinned && !onlyBusy)
        return Refuse(handler, "Usage: .playerbots ladder teams [pinned|busy]");

    LadderMgr& mgr = Instance();
    uint32 const now = UnixNow();
    uint32 const start = static_cast<uint32>(GameTime::GetStartTime().count());
    mgr.PruneContacts(now);
    uint32 busyTeams = 0;
    uint32 pinnedTeams = 0;
    for (auto const& [id, team] : sArenaTeamMgr->GetArenaTeams())
    {
        std::string reason;
        bool const pinned = mgr.IsTeamPinned(team, &reason);
        bool const busy = IsTeamBusy(team);
        if (pinned)
            ++pinnedTeams;

        if (busy)
            ++busyTeams;

        if ((onlyPinned && !pinned) || (onlyBusy && !busy))
            continue;

        uint32 online = 0;
        uint32 mmr = 0;
        for (ArenaTeamMember const& member : team->GetMembers())
        {
            mmr += member.MatchMakerRating;
            if (ObjectAccessor::FindConnectedPlayer(member.Guid))
                ++online;
        }

        uint32 const members = team->GetMembersSize();
        auto const it = mgr._moves.find(id);
        bool const moved = it != mgr._moves.end();
        uint32 const since = moved && it->second.HostedSince ? it->second.HostedSince : start;
        std::string const state =
            moved && IsBusy(it->second.Current) ? Acore::StringFormat(" {}", StateName(it->second.Current)) : "";
        handler->PSendSysMessage("#{} <{}> {}v{} rating {} mmr {} members {}/{} hosted {} min{}{}{}", id,
                                 team->GetName(), team->GetType(), team->GetType(), team->GetRating(),
                                 members ? mmr / members : 0, online, members, (now - since) / MINUTE, state,
                                 busy ? " busy" : "", pinned ? " pinned (" + reason + ")" : "");
    }

    handler->PSendSysMessage("{} teams in memory: {} busy, {} pinned; {} recent contacts; queues {}",
                             sArenaTeamMgr->GetArenaTeams().size(), busyTeams, pinnedTeams, mgr._contacts.size(),
                             mgr._draining.load(std::memory_order_relaxed) ? "drained" : "open");
    return true;
}

bool LadderMgr::HandlePinCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::HandlePinCommand");
    if (!MovesOn())
        return Refuse(handler, MovesOffText);

    std::string_view const text = args ? std::string_view(args) : std::string_view();
    size_t const space = text.find(' ');
    Optional<uint32> const teamId = ParseTeamId(args);
    Optional<uint32> const minutes = space == std::string_view::npos ? Optional<uint32>(DefaultGmPinMinutes)
                                                                     : Acore::StringTo<uint32>(text.substr(space + 1));
    if (!teamId || !minutes || *minutes > MaxGmPinMinutes)
        return Refuse(handler, "Usage: .playerbots ladder pin <arenaTeamId> [minutes, default 60; 0 unpins]");

    LadderMgr& mgr = Instance();
    if (!*minutes)
    {
        mgr._gmPins.erase(*teamId);
        handler->PSendSysMessage("Team {}: GM pin removed", *teamId);
        return true;
    }

    mgr._gmPins[*teamId] = UnixNow() + *minutes * MINUTE;
    handler->PSendSysMessage(
        "Team {} pinned for {} min; a depart still waiting for its members is cancelled (one that is saving completes)",
        *teamId, *minutes);
    LOG_INFO("playerbots", "Ladder: arena team {} pinned by a GM for {} min", *teamId, *minutes);
    return true;
}

bool LadderMgr::HandleDrainCommand(ChatHandler* handler, char const* /*args*/)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::HandleDrainCommand");
    if (!MovesOn())
        return Refuse(handler, MovesOffText);

    Instance()._draining.store(true, std::memory_order_relaxed);
    handler->SendSysMessage("Ladder drained: no arena team member queues; queued members and matches carry on");
    LOG_INFO("playerbots", "Ladder: drained");
    return true;
}

bool LadderMgr::HandleUndrainCommand(ChatHandler* handler, char const* /*args*/)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::HandleUndrainCommand");
    if (!MovesOn())
        return Refuse(handler, MovesOffText);

    Instance()._draining.store(false, std::memory_order_relaxed);
    handler->SendSysMessage("Ladder undrained: arena team members queue again");
    LOG_INFO("playerbots", "Ladder: undrained");
    return true;
}

void LadderMgr::NoteTeam(Move& move, ArenaTeam* team)
{
    move.Type = static_cast<uint8>(team->GetType());
    move.Pool = PoolOn() && IsPoolTeam(team);
}

bool LadderMgr::PoolOn() const { return _poolOn && sPlayerbotAIConfig.LadderPoolTeams > 0; }

bool LadderMgr::IsPoolTeam(ArenaTeam* team)
{
    if (!team || team->GetType() != ARENA_TYPE_2v2 || team->GetMembersSize() != 2)
        return false;

    uint8 const maxLevel = MaxLevel();
    uint8 const slot = team->GetSlot();
    for (ArenaTeamMember const& member : team->GetMembers())
    {
        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(member.Guid);
        if (!entry || entry->Level < maxLevel || !sPlayerbotAIConfig.IsInRandomAccountList(entry->AccountId))
            return false;

        for (uint8 other = 0; other < ArenaSlotCount; ++other)
            if (other != slot && entry->ArenaTeamId[other])
                return false;
    }

    return true;
}

bool LadderMgr::PoolWants(Player* bot) const
{
    if (!_poolShort || !PoolOn())
        return false;

    uint8 const maxLevel = MaxLevel();
    return bot->GetLevel() >= maxLevel && IsFreePoolBot(bot->GetGUID(), maxLevel);
}

void LadderMgr::CollectAwayTeams(uint8 type, std::vector<uint32>& out) const
{
    if (_hostedOnly)
        return;

    for (uint32 const teamId : _awayTeams)
    {
        auto const it = _moves.find(teamId);
        if (it != _moves.end() && it->second.Type == type && !it->second.Pool &&
            !sArenaTeamMgr->GetArenaTeamById(teamId))
            out.push_back(teamId);
    }
}

LadderMgr::PoolCount LadderMgr::CountPool() const
{
    PoolCount count;
    for (auto const& [id, team] : sArenaTeamMgr->GetArenaTeams())
        if (IsPoolTeam(team))
            ++count.Resident;

    // Dropped from memory, so the startup drop or the depart noted whether it was a pool team
    for (uint32 const teamId : _awayTeams)
    {
        auto const it = _moves.find(teamId);
        if (it != _moves.end() && it->second.Pool && !sArenaTeamMgr->GetArenaTeamById(teamId))
            ++count.Away;
    }

    return count;
}

bool LadderMgr::IsFreePoolBot(ObjectGuid guid, uint8 maxLevel) const
{
    CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid);
    if (!entry || entry->Level < maxLevel || IsAway(guid.GetCounter()))
        return false;

    if (!sPlayerbotAIConfig.IsInRandomAccountList(entry->AccountId) ||
        sRandomPlayerbotMgr.IsAddClassAccount(entry->AccountId))
        return false;

    if (sPlayerbotAIConfig.DisableDeathKnightLogin && entry->Class == CLASS_DEATH_KNIGHT)
        return false;

    for (uint8 slot = 0; slot < ArenaSlotCount; ++slot)
        if (entry->ArenaTeamId[slot])
            return false;

    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    if (!player)
        return true;

    for (uint8 slot = 0; slot < ArenaSlotCount; ++slot)
        if (player->GetArenaTeamId(slot))
            return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    return botAI && !botAI->HasGameClientMaster();
}

// The core founds a team only with an online captain. For a pair of offline bots, an online random bot below the
// maximum level (never a pool bot) founds the team and leaves it at once
Player* LadderMgr::FindPoolFounder()
{
    uint8 const maxLevel = MaxLevel();
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld() || !bot->GetSession() || bot->GetLevel() >= maxLevel || bot->GetGroup() ||
            bot->InBattleground() || bot->InBattlegroundQueue())
            continue;

        if (!sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()) ||
            bot->GetArenaTeamId(ARENA_SLOT_2v2) ||
            sCharacterCache->GetCharacterArenaTeamIdByGuid(bot->GetGUID(), ARENA_SLOT_2v2))
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI && !botAI->HasGameClientMaster())
            return bot;
    }

    return nullptr;
}

void LadderMgr::UpdatePool(uint32 diff)
{
    if (_poolQueryPending)
    {
        _queryProcessor.ProcessReadyCallbacks();
        return;
    }

    _poolTimer += diff;
    if (_poolTimer < PoolCheckIntervalMs)
        return;

    _poolTimer = 0;
    if (!PoolOn())
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::UpdatePool");
    PoolCount const count = CountPool();
    _poolShort = count.Resident + count.Away < sPlayerbotAIConfig.LadderPoolTeams;
    if (_poolShort)
        QueryPoolCandidates();
}

void LadderMgr::QueryPoolCandidates()
{
    // Characters at the maximum level in no arena team; the world thread filters them by the character cache
    _poolQueryPending = true;
    std::string const sql = Acore::StringFormat(
        "SELECT c.guid FROM characters c LEFT JOIN arena_team_member m ON m.guid = c.guid "
        "WHERE c.level >= {} AND c.account <> 0 AND m.guid IS NULL",
        MaxLevel());
    _queryProcessor.AddCallback(
        CharacterDatabase.AsyncQuery(sql).WithCallback([this](QueryResult result) { OnPoolCandidates(result); }));
}

void LadderMgr::OnPoolCandidates(QueryResult result)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::OnPoolCandidates");
    _poolQueryPending = false;
    uint32 const target = sPlayerbotAIConfig.LadderPoolTeams;
    PoolCount const before = CountPool();
    _poolShort = before.Resident + before.Away < target;
    if (!_poolShort)
        return;

    uint8 const maxLevel = MaxLevel();
    std::array<std::vector<PoolBot>, 2> freeBots;
    if (result)
    {
        do
        {
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(result->Fetch()[0].Get<uint32>());
            if (!IsFreePoolBot(guid, maxLevel))
                continue;

            TeamId const faction = Player::TeamIdForRace(sCharacterCache->GetCharacterCacheByGuid(guid)->Race);
            if (faction == TEAM_ALLIANCE || faction == TEAM_HORDE)
                freeBots[faction].push_back({guid.GetCounter(), ObjectAccessor::FindConnectedPlayer(guid) != nullptr});
        } while (result->NextRow());
    }

    uint32 const repaired = RepairPoolTeams(freeBots);
    uint32 const missing = target - before.Resident - before.Away;
    uint32 const wanted = std::min(missing > repaired ? missing - repaired : 0, PoolBuildPerCheck);
    Player* founder = nullptr;
    uint32 created = 0;
    uint32 failed = 0;
    uint32 noFounder = 0;
    for (auto const& [first, second] : PairPoolBots(freeBots, wanted))
    {
        ObjectGuid const captain = ObjectGuid::Create<HighGuid::Player>(first.Guid);
        ObjectGuid const partner = ObjectGuid::Create<HighGuid::Player>(second.Guid);
        Player* teamFounder = ObjectAccessor::FindConnectedPlayer(captain);
        if (!teamFounder)
        {
            if (!founder)
                founder = FindPoolFounder();

            teamFounder = founder;
        }

        if (!teamFounder)
            ++noFounder;
        else if (RandomPlayerbotFactory::CreatePairArenaTeam(teamFounder, captain, partner))
            ++created;
        else
            ++failed;
    }

    _poolCreated += created;
    PoolCount const after = CountPool();
    _poolShort = after.Resident + after.Away < target;
    _poolNote = Acore::StringFormat(
        "last check: {} free bots at level {} ({} alliance, {} horde), {} teams repaired, {} created, {} failed, {} "
        "without a founder",
        freeBots[TEAM_ALLIANCE].size() + freeBots[TEAM_HORDE].size(), maxLevel, freeBots[TEAM_ALLIANCE].size(),
        freeBots[TEAM_HORDE].size(), repaired, created, failed, noFounder);
    if (repaired || created || failed || noFounder)
        LOG_INFO("playerbots", "Ladder pool: {} of {} teams ({} resident, {} away); {}", after.Resident + after.Away,
                 target, after.Resident, after.Away, _poolNote);
}

// A pool team a formation leave left with one member: outside the pool count and the bot arena team registry, so
// nothing else fills it
bool LadderMgr::IsBrokenPoolTeam(ArenaTeam* team) const
{
    if (team->GetType() != ARENA_TYPE_2v2 || team->GetMembersSize() != 1 ||
        RandomPlayerbotFactory::IsRegisteredBotArenaTeam(ARENA_TYPE_2v2, team->GetId()))
        return false;

    if (auto const it = _moves.find(team->GetId()); it != _moves.end() && IsBusy(it->second.Current))
        return false;

    CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(team->GetMembers().front().Guid);
    if (!entry || entry->Level < MaxLevel() || !sPlayerbotAIConfig.IsInRandomAccountList(entry->AccountId))
        return false;

    for (uint8 slot = 0; slot < ArenaSlotCount; ++slot)
        if (slot != team->GetSlot() && entry->ArenaTeamId[slot])
            return false;

    return true;
}

// Refills each broken pool team with a free bot of its member's faction (taken out of freeBots)
uint32 LadderMgr::RepairPoolTeams(std::array<std::vector<PoolBot>, 2>& freeBots)
{
    std::vector<ArenaTeam*> broken;
    for (auto const& [id, team] : sArenaTeamMgr->GetArenaTeams())
        if (IsBrokenPoolTeam(team))
            broken.push_back(team);

    uint32 repaired = 0;
    for (ArenaTeam* team : broken)
    {
        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(team->GetMembers().front().Guid);
        TeamId const faction = Player::TeamIdForRace(entry->Race);
        if ((faction != TEAM_ALLIANCE && faction != TEAM_HORDE) || freeBots[faction].empty())
            continue;

        ObjectGuid const partner = ObjectGuid::Create<HighGuid::Player>(freeBots[faction].back().Guid);
        freeBots[faction].pop_back();
        if (!team->AddMember(partner))
            continue;

        if (sPlayerbotAIConfig.RandomBotArenaTeamRatingMode == ARENA_RATING_MODE_RANDOM)
        {
            // As in a new pool team: the personal rating and the MMR follow the artificial team rating
            for (ArenaTeamMember& member : team->GetMembers())
            {
                if (member.Guid != partner)
                    continue;

                member.PersonalRating = static_cast<uint16>(team->GetRating());
                member.MatchMakerRating = member.PersonalRating;
                member.MaxMMR = std::max(member.MaxMMR, member.PersonalRating);
            }
        }

        team->SaveToDB(true);
        ++repaired;
        LOG_INFO("playerbots", "Ladder pool: arena team {} <{}> refilled with {}", team->GetId(), team->GetName(),
                 partner.ToString());
    }

    return repaired;
}

std::vector<std::pair<LadderMgr::PoolBot, LadderMgr::PoolBot>> LadderMgr::PairPoolBots(
    std::array<std::vector<PoolBot>, 2> const& freeBots, uint32 wanted)
{
    std::array<std::vector<std::pair<PoolBot, PoolBot>>, 2> byFaction;
    for (size_t faction = 0; faction < freeBots.size(); ++faction)
    {
        std::vector<PoolBot> online;
        std::vector<PoolBot> offline;
        for (PoolBot const& bot : freeBots[faction])
            (bot.Online ? online : offline).push_back(bot);

        std::vector<std::pair<PoolBot, PoolBot>>& pairs = byFaction[faction];
        size_t on = 0;
        size_t off = 0;
        for (; on < online.size() && off < offline.size(); ++on, ++off)
            pairs.emplace_back(online[on], offline[off]);

        for (; on + 1 < online.size(); on += 2)
            pairs.emplace_back(online[on], online[on + 1]);

        for (; off + 1 < offline.size(); off += 2)
            pairs.emplace_back(offline[off], offline[off + 1]);
    }

    std::vector<std::pair<PoolBot, PoolBot>> out;
    std::array<size_t, 2> next{};
    bool added = true;
    while (added && out.size() < wanted)
    {
        added = false;
        for (size_t faction = 0; faction < byFaction.size() && out.size() < wanted; ++faction)
        {
            if (next[faction] < byFaction[faction].size())
            {
                out.push_back(byFaction[faction][next[faction]++]);
                added = true;
            }
        }
    }

    return out;
}

bool LadderMgr::HandlePoolStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::HandlePoolStatusCommand");
    LadderMgr const& mgr = Instance();
    if (!mgr.PoolOn())
        return Refuse(handler, PoolOffText);

    PoolCount const count = mgr.CountPool();
    handler->PSendSysMessage(
        "Ladder pool: {} of {} teams ({} resident, {} away); {} teams created since the start; {}{}",
        count.Resident + count.Away, sPlayerbotAIConfig.LadderPoolTeams, count.Resident, count.Away, mgr._poolCreated,
        mgr._poolNote, mgr._poolQueryPending ? " (a check is running)" : "");
    return true;
}

bool LadderMgr::HandlePoolBuildCommand(ChatHandler* handler, char const* /*args*/)
{
    PLAYERBOT_CHECK_WORLD_THREAD("LadderMgr::HandlePoolBuildCommand");
    LadderMgr& mgr = Instance();
    if (!mgr.PoolOn())
        return Refuse(handler, PoolOffText);

    if (mgr._poolQueryPending)
        return Refuse(handler, "Ladder pool: a check is running");

    PoolCount const count = mgr.CountPool();
    mgr._poolTimer = 0;
    mgr.QueryPoolCandidates();
    handler->PSendSysMessage(
        "Ladder pool: {} of {} teams; looking for free bots at level {} (see .playerbots ladder "
        "pool status)",
        count.Resident + count.Away, sPlayerbotAIConfig.LadderPoolTeams, MaxLevel());
    return true;
}

bool LadderMgr::HandlePoolSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    using Pairs = std::vector<std::pair<PoolBot, PoolBot>>;
    uint32 failures = 0;
    auto check = [&](bool ok, std::string const& what)
    {
        if (ok)
            return;

        ++failures;
        handler->PSendSysMessage("Ladder pool selftest FAIL: {}", what);
    };

    // Every bot once, both of a pair of one faction, an online captain where the pair has an online bot, as many
    // pairs as the factions allow up to wanted
    auto checkPairs = [&](std::string const& name, std::array<std::vector<PoolBot>, 2> const& freeBots, uint32 wanted,
                          Pairs const& pairs)
    {
        std::unordered_map<uint32, size_t> factionOf;
        for (size_t faction = 0; faction < freeBots.size(); ++faction)
            for (PoolBot const& bot : freeBots[faction])
                factionOf[bot.Guid] = faction;

        std::unordered_set<uint32> used;
        for (auto const& [captain, partner] : pairs)
        {
            check(used.insert(captain.Guid).second && used.insert(partner.Guid).second, name + ": a bot in two pairs");
            check(factionOf.contains(captain.Guid) && factionOf.contains(partner.Guid) &&
                      factionOf[captain.Guid] == factionOf[partner.Guid],
                  name + ": a pair of two factions or an unknown bot");
            check(captain.Online || !partner.Online, name + ": an offline captain with an online partner");
        }

        size_t const possible = freeBots[TEAM_ALLIANCE].size() / 2 + freeBots[TEAM_HORDE].size() / 2;
        size_t const expected = std::min<size_t>(possible, wanted);
        check(pairs.size() == expected, Acore::StringFormat("{}: {} pairs, expected {}", name, pairs.size(), expected));
    };

    std::array<std::vector<PoolBot>, 2> mixed;
    mixed[TEAM_ALLIANCE] = {{1, false}, {2, false}, {3, false}};
    mixed[TEAM_HORDE] = {{10, true}, {11, true}, {12, false}, {13, false}, {14, false}};
    Pairs const all = PairPoolBots(mixed, 10);
    checkPairs("mixed", mixed, 10, all);
    check(all.size() == 3 && all[0].first.Guid == 1 && all[0].second.Guid == 2 && all[1].first.Guid == 10 &&
              all[1].second.Guid == 12 && all[2].first.Guid == 11 && all[2].second.Guid == 13,
          "mixed: factions in turn, online captains with offline partners first");
    checkPairs("mixed, wanted 1", mixed, 1, PairPoolBots(mixed, 1));
    checkPairs("mixed, wanted 0", mixed, 0, PairPoolBots(mixed, 0));
    std::array<std::vector<PoolBot>, 2> const none;
    checkPairs("empty", none, 5, PairPoolBots(none, 5));

    std::array<std::vector<PoolBot>, 2> online;
    online[TEAM_HORDE] = {{20, true}, {21, true}, {22, true}, {23, true}, {24, true}};
    checkPairs("online horde", online, 5, PairPoolBots(online, 5));

    std::array<std::vector<PoolBot>, 2> odd;
    odd[TEAM_ALLIANCE] = {{30, false}, {31, true}, {32, true}};
    odd[TEAM_HORDE] = {{40, false}};
    Pairs const oddPairs = PairPoolBots(odd, 5);
    checkPairs("odd", odd, 5, oddPairs);
    check(oddPairs.size() == 1 && oddPairs[0].first.Guid == 31 && oddPairs[0].second.Guid == 30,
          "odd: the online bot captains the offline one");

    handler->PSendSysMessage("Ladder pool selftest: {} ({} failures)", failures ? "FAIL" : "PASS", failures);
    return true;
}
