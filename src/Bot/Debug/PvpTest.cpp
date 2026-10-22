/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpTest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <random>
#include <set>
#include <sstream>

#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
constexpr uint32 TickIntervalMs = 500;             // cadence of the O(roster) work
constexpr uint32 GatherTimeoutMs = 60000;          // logins, leaving queues, combat ending
constexpr uint32 AcceptFallbackMs = 15000;         // bots that have not entered by then get the accept packet sent
constexpr uint32 TearDownTimeoutMs = 60000;        // leaving the arena and the teleport back
constexpr uint16 BattlefieldPortUnknown = 0x1F90;  // the constant the client sends, as in BGStatusAction
constexpr uint8 BattlefieldPortAccept = 1;
constexpr uint32 MsPerSecond = 1000;
constexpr uint32 SecondsPerMinute = 60;

struct ArenaMap
{
    char const* Key;
    BattlegroundTypeId Type;
};

std::array<ArenaMap, 5> const ArenaMaps = {{
    {"na", BATTLEGROUND_NA},
    {"be", BATTLEGROUND_BE},
    {"rl", BATTLEGROUND_RL},
    {"ds", BATTLEGROUND_DS},
    {"rv", BATTLEGROUND_RV},
}};

constexpr char const* Usage =
    "Usage: .playerbots pvptest arena <teamA|name,name> <teamB|name,name> [map=na|be|rl|ds|rv|random] [rated=0|1] "
    "[seed=<n>] (quote team names with spaces)";

bool CommandFail(ChatHandler* handler, std::string const& text)
{
    handler->SendSysMessage(text);
    handler->SetSentErrorMessage(true);
    return false;
}

bool CheckEnabled(ChatHandler* handler)
{
    if (sPlayerbotAIConfig.PvpTestEnable)
        return true;

    return CommandFail(handler, "The PvP test harness is disabled (Playerbots.PvpTest.Enable = 0)");
}

std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Trim(std::string const& text)
{
    size_t const first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";

    size_t const last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// Splits at blanks outside double quotes, so a team name with spaces can be quoted
std::vector<std::string> Tokenize(std::string const& text)
{
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    bool hasToken = false;
    for (char const c : text)
    {
        if (c == '"')
        {
            quoted = !quoted;
            hasToken = true;
            continue;
        }

        if (!quoted && std::isspace(static_cast<unsigned char>(c)))
        {
            if (hasToken)
                tokens.push_back(current);

            current.clear();
            hasToken = false;
            continue;
        }

        current += c;
        hasToken = true;
    }

    if (hasToken)
        tokens.push_back(current);

    return tokens;
}

std::string FormatDuration(uint32 ms)
{
    uint32 const seconds = ms / MsPerSecond;
    return Acore::StringFormat("{}:{:02}", seconds / SecondsPerMinute, seconds % SecondsPerMinute);
}

std::string DescribeSide(PvpTestSide const& side)
{
    std::string names;
    for (std::string const& name : side.Names)
        names += names.empty() ? name : ", " + name;

    return Acore::StringFormat("{} [{}]", side.Label, names);
}

// One status line per bot: where it is and whether it is still in a group, a battleground or a queue
std::string DescribeMember(ObjectGuid const& guid)
{
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    if (!player)
    {
        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid);
        return Acore::StringFormat("  {}: offline", entry ? entry->Name : guid.ToString());
    }

    uint32 queues = 0;
    for (uint32 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
    {
        if (player->GetBattlegroundQueueTypeId(slot) != BATTLEGROUND_QUEUE_NONE)
            ++queues;
    }

    Group const* group = player->GetGroup();
    return Acore::StringFormat("  {}: map {}, group {}, battleground {}, queues {}", player->GetName(),
                               player->GetMapId(), group ? std::to_string(group->GetGUID().GetCounter()) : "none",
                               player->InBattleground() ? std::to_string(player->GetBattlegroundId()) : "none", queues);
}

void LeaveQueues(Player* player)
{
    for (uint32 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
    {
        BattlegroundQueueTypeId const queueTypeId = player->GetBattlegroundQueueTypeId(slot);
        if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        sBattlegroundMgr->GetBattlegroundQueue(queueTypeId).RemovePlayer(player->GetGUID(), true);
        player->RemoveBattlegroundQueueId(queueTypeId);
    }
}

bool ResolveSide(std::string const& spec, uint8 arenaType, PvpTestSide& side, std::string& error)
{
    side = PvpTestSide();
    side.Label = spec;
    if (spec.find(',') == std::string::npos)
    {
        ArenaTeam* team = sArenaTeamMgr->GetArenaTeamByName(spec, arenaType);
        if (!team)
        {
            error = Acore::StringFormat("No {}v{} arena team named '{}' (a roster is name,name)", arenaType, arenaType,
                                        spec);
            return false;
        }

        side.Label = team->GetName();
        side.ArenaTeamId = team->GetId();
        for (ArenaTeamMember const& member : team->GetMembers())
            side.Members.push_back(member.Guid);
    }
    else
    {
        std::stringstream stream(spec);
        std::string name;
        while (std::getline(stream, name, ','))
        {
            name = Trim(name);
            if (name.empty())
                continue;

            ObjectGuid const guid =
                normalizePlayerName(name) ? sCharacterCache->GetCharacterGuidByName(name) : ObjectGuid::Empty;
            if (guid.IsEmpty())
            {
                error = Acore::StringFormat("No character named '{}'", name);
                return false;
            }

            side.Members.push_back(guid);
        }
    }

    if (side.Members.size() != arenaType)
    {
        error = Acore::StringFormat("Side '{}' has {} members, a {}v{} match needs {}", side.Label, side.Members.size(),
                                    arenaType, arenaType, arenaType);
        return false;
    }

    uint32 const requiredLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    for (ObjectGuid const& guid : side.Members)
    {
        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid);
        if (!entry)
        {
            error = Acore::StringFormat("Character {} of side '{}' is unknown", guid.ToString(), side.Label);
            return false;
        }

        if (!sPlayerbotAIConfig.IsInRandomAccountList(entry->AccountId))
        {
            error = Acore::StringFormat("{} is not a random bot", entry->Name);
            return false;
        }

        if (entry->Level != requiredLevel)
        {
            error = Acore::StringFormat("{} is level {}, the match needs level {}", entry->Name, entry->Level,
                                        requiredLevel);
            return false;
        }

        side.Names.push_back(entry->Name);
    }

    side.LoginRequested.assign(side.Members.size(), false);
    return true;
}

// Fills in the map: a named arena, or one drawn from the seed among the arenas this server has
bool ChooseMap(std::string const& key, PvpTestArenaParams& params, std::string& error)
{
    if (key != "random")
    {
        auto const itr =
            std::find_if(ArenaMaps.begin(), ArenaMaps.end(), [&key](ArenaMap const& map) { return key == map.Key; });
        if (itr == ArenaMaps.end())
        {
            error = Acore::StringFormat("Unknown map '{}' (na, be, rl, ds, rv, random)", key);
            return false;
        }

        if (!sBattlegroundMgr->GetBattlegroundTemplate(itr->Type))
        {
            error = Acore::StringFormat("Arena '{}' is not available on this server", key);
            return false;
        }

        params.Map = itr->Type;
        return true;
    }

    std::vector<BattlegroundTypeId> available;
    for (ArenaMap const& map : ArenaMaps)
    {
        if (sBattlegroundMgr->GetBattlegroundTemplate(map.Type))
            available.push_back(map.Type);
    }

    if (available.empty())
    {
        error = "No arena is available on this server";
        return false;
    }

    std::mt19937 generator(params.Seed);
    params.Map = available[generator() % available.size()];
    return true;
}
}  // namespace

PvpTestArenaJob::PvpTestArenaJob(PvpTestArenaParams const& params, PvpTestSide sideA, PvpTestSide sideB,
                                 ObjectGuid issuer)
    : _params(params), _sideA(std::move(sideA)), _sideB(std::move(sideB)), _issuer(issuer)
{
    EnterStage(Stage::Gather);
}

char const* PvpTestArenaJob::StageName(Stage stage)
{
    switch (stage)
    {
        case Stage::Resolve:
            return "resolve";
        case Stage::Gather:
            return "gather";
        case Stage::Start:
            return "start";
        case Stage::Monitor:
            return "monitor";
        case Stage::TearDown:
            return "tear-down";
        case Stage::Done:
            return "done";
    }

    return "unknown";
}

bool PvpTestArenaJob::Resolve(PvpTestArenaParams const& params, PvpTestSide& sideA, PvpTestSide& sideB,
                              std::string& error)
{
    if (!ResolveSide(params.SideA, params.ArenaType, sideA, error) ||
        !ResolveSide(params.SideB, params.ArenaType, sideB, error))
        return false;

    std::set<ObjectGuid> distinct(sideA.Members.begin(), sideA.Members.end());
    distinct.insert(sideB.Members.begin(), sideB.Members.end());
    if (distinct.size() != sideA.Members.size() + sideB.Members.size())
    {
        error = "A bot is listed twice (the sides must not overlap)";
        return false;
    }

    if (params.Rated && (!sideA.ArenaTeamId || !sideB.ArenaTeamId))
    {
        error = "rated=1 needs two arena teams, not name rosters";
        return false;
    }

    return true;
}

std::vector<ObjectGuid> PvpTestArenaJob::AllMembers() const
{
    std::vector<ObjectGuid> members(_sideA.Members);
    members.insert(members.end(), _sideB.Members.begin(), _sideB.Members.end());
    return members;
}

void PvpTestArenaJob::EnterStage(Stage stage)
{
    _stage = stage;
    _stageMs = 0;
    LOG_INFO("playerbots", "PVPTEST arena {} vs {}: stage {}", _sideA.Label, _sideB.Label, StageName(stage));
}

void PvpTestArenaJob::Fail(std::string const& reason)
{
    _result.Outcome = "failed";
    _result.FailedStage = StageName(_stage);
    _result.Reason = reason;
    EnterStage(Stage::TearDown);
}

void PvpTestArenaJob::Abort(std::string const& reason)
{
    if (_stage == Stage::Done || _stage == Stage::TearDown)
        return;

    _result.Outcome = "stopped";
    _result.FailedStage = StageName(_stage);
    _result.Reason = reason;
    EnterStage(Stage::TearDown);
}

void PvpTestArenaJob::Update(uint32 diff)
{
    if (_stage == Stage::Done)
        return;

    _stageMs += diff;
    _totalMs += diff;
    _tickMs += diff;
    bool const slowTick = _tickMs >= TickIntervalMs;
    if (slowTick)
        _tickMs = 0;

    switch (_stage)
    {
        case Stage::Gather:
            if (slowTick)
                UpdateGather();
            break;
        case Stage::Monitor:
            UpdateMonitor(slowTick);
            break;
        case Stage::TearDown:
            if (slowTick)
                UpdateTearDown();
            break;
        default:
            break;
    }
}

bool PvpTestArenaJob::GatherMember(PvpTestSide& side, size_t index)
{
    ObjectGuid const guid = side.Members[index];
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    if (!player)
    {
        if (!side.LoginRequested[index])
        {
            // Asynchronous: the bot appears on a later tick
            side.LoginRequested[index] = true;
            sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
        }

        return false;
    }

    if (!player->IsInWorld() || player->IsBeingTeleported())
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
    {
        Fail(Acore::StringFormat("{} is online without bot AI", side.Names[index]));
        return false;
    }

    if (botAI->HasGameClientMaster())
    {
        Fail(Acore::StringFormat("{} is with a real player", side.Names[index]));
        return false;
    }

    if (player->InBattleground())
    {
        player->LeaveBattleground();
        return false;
    }

    if (player->InBattlegroundQueue())
    {
        LeaveQueues(player);
        return false;
    }

    if (!player->IsAlive())
    {
        player->ResurrectPlayer(1.0f);
        player->SpawnCorpseBones();
        return false;
    }

    return !player->IsInCombat();
}

void PvpTestArenaJob::UpdateGather()
{
    std::vector<std::string> waiting;
    for (PvpTestSide* side : {&_sideA, &_sideB})
    {
        for (size_t index = 0; index < side->Members.size(); ++index)
        {
            if (!GatherMember(*side, index))
                waiting.push_back(side->Names[index]);

            if (_stage != Stage::Gather)
                return;  // a member failed the job
        }
    }

    if (!waiting.empty())
    {
        if (_stageMs >= GatherTimeoutMs)
        {
            std::string names;
            for (std::string const& name : waiting)
                names += names.empty() ? name : ", " + name;

            Fail(Acore::StringFormat("not ready after {} s: {}", GatherTimeoutMs / MsPerSecond, names));
        }

        return;
    }

    // Groups and the arena in the same tick, so the bots' AI cannot leave the new groups in between
    EnterStage(Stage::Start);
    if (!FormGroup(_sideA) || !FormGroup(_sideB))
    {
        Fail("could not form the groups");
        return;
    }

    std::string error;
    if (!StartArena(error))
    {
        Fail(error);
        return;
    }

    EnterStage(Stage::Monitor);
}

bool PvpTestArenaJob::FormGroup(PvpTestSide& side)
{
    std::vector<Player*> players;
    for (ObjectGuid const& guid : side.Members)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player)
            return false;

        if (Group* group = player->GetGroup())
            group->RemoveMember(guid);

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(player))
            botAI->Reset();

        players.push_back(player);
    }

    Group* group = new Group();
    if (!group->Create(players.front()))
    {
        delete group;
        return false;
    }

    sGroupMgr->AddGroup(group);
    side.GroupId = group->GetGUID().GetCounter();
    for (size_t index = 1; index < players.size(); ++index)
    {
        if (!group->AddMember(players[index]))
            return false;
    }

    return group->GetMembersCount() == side.Members.size();
}

// The forced-match recipe of BattlegroundQueue::BattlegroundQueueUpdate (rated arenas) and the group join of
// WorldSession::HandleBattlemasterJoinArena, without the queue wait
bool PvpTestArenaJob::StartArena(std::string& error)
{
    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_AA);
    Group* groupA = sGroupMgr->GetGroupByGUID(_sideA.GroupId);
    Group* groupB = sGroupMgr->GetGroupByGUID(_sideB.GroupId);
    Player* leaderA = ObjectAccessor::FindConnectedPlayer(_sideA.Members.front());
    Player* leaderB = ObjectAccessor::FindConnectedPlayer(_sideB.Members.front());
    if (!bgTemplate || !groupA || !groupB || !leaderA || !leaderB)
    {
        error = "the arena template, a group or a leader is missing";
        return false;
    }

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), leaderA->GetLevel());
    if (!bracketEntry)
    {
        error = Acore::StringFormat("no arena bracket for level {}", leaderA->GetLevel());
        return false;
    }

    _queueTypeId = BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_AA, _params.ArenaType);
    BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(_queueTypeId);
    for (ObjectGuid const& guid : AllMembers())
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player)
        {
            error = Acore::StringFormat("{} went offline", guid.ToString());
            return false;
        }

        PvPDifficultyEntry const* entry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
        if (!entry || entry->GetBracketId() != bracketEntry->GetBracketId())
        {
            error = Acore::StringFormat("{} is in another level bracket", player->GetName());
            return false;
        }

        // AddGroup asserts that no member is queued already
        GroupQueueInfo queued;
        if (queue.GetPlayerGroupInfoData(guid, &queued))
        {
            error = Acore::StringFormat("{} is still in the arena queue", player->GetName());
            return false;
        }
    }

    uint32 ratingA = 0;
    uint32 ratingB = 0;
    uint32 matchmakerRatingA = 0;
    uint32 matchmakerRatingB = 0;
    if (_params.Rated)
    {
        ArenaTeam* teamA = sArenaTeamMgr->GetArenaTeamById(_sideA.ArenaTeamId);
        ArenaTeam* teamB = sArenaTeamMgr->GetArenaTeamById(_sideB.ArenaTeamId);
        if (!teamA || !teamB)
        {
            error = "an arena team is gone";
            return false;
        }

        ratingA = teamA->GetRating();
        ratingB = teamB->GetRating();
        matchmakerRatingA = teamA->GetAverageMMR(groupA);
        matchmakerRatingB = teamB->GetAverageMMR(groupB);
    }

    GroupQueueInfo* infoA =
        queue.AddGroup(leaderA, groupA, BATTLEGROUND_AA, bracketEntry, _params.ArenaType, _params.Rated, false, ratingA,
                       matchmakerRatingA, _params.Rated ? _sideA.ArenaTeamId : 0);
    GroupQueueInfo* infoB =
        queue.AddGroup(leaderB, groupB, BATTLEGROUND_AA, bracketEntry, _params.ArenaType, _params.Rated, false, ratingB,
                       matchmakerRatingB, _params.Rated ? _sideB.ArenaTeamId : 0);
    infoA->OpponentsTeamRating = ratingB;
    infoA->OpponentsMatchmakerRating = matchmakerRatingB;
    infoB->OpponentsTeamRating = ratingA;
    infoB->OpponentsMatchmakerRating = matchmakerRatingA;
    for (ObjectGuid const& guid : AllMembers())
    {
        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            player->AddBattlegroundQueueId(_queueTypeId);
    }

    // The tear-down takes the queued players out again when this fails
    Battleground* arena =
        sBattlegroundMgr->CreateNewBattleground(_params.Map, bracketEntry, _params.ArenaType, _params.Rated);
    if (!arena)
    {
        error = "could not create the arena";
        return false;
    }

    // The invite decides the sides, so two teams of one faction can meet
    arena->SetArenaMatchmakerRating(TEAM_ALLIANCE, matchmakerRatingA);
    arena->SetArenaMatchmakerRating(TEAM_HORDE, matchmakerRatingB);
    queue.InviteGroupToBG(infoA, arena, TEAM_ALLIANCE);
    queue.InviteGroupToBG(infoB, arena, TEAM_HORDE);
    arena->StartBattleground();

    _instanceId = arena->GetInstanceID();
    _bgTypeId = arena->GetBgTypeID();
    _result.Map = arena->GetName();
    LOG_INFO("playerbots", "PVPTEST arena {} vs {}: {} instance {} created, rated {}, seed {}", DescribeSide(_sideA),
             DescribeSide(_sideB), _result.Map, _instanceId, _params.Rated ? 1 : 0, _params.Seed);
    return true;
}

void PvpTestArenaJob::UpdateMonitor(bool slowTick)
{
    // One lookup per tick, so the end status is not missed before the arena closes
    Battleground* arena = sBattlegroundMgr->GetBattleground(_instanceId, _bgTypeId);
    if (!arena)
    {
        _result.Outcome = "ended";
        _result.Reason = "the arena closed before a winner was seen";
        EnterStage(Stage::TearDown);
        return;
    }

    BattlegroundStatus const status = arena->GetStatus();
    if (status == STATUS_IN_PROGRESS && !_fightStarted)
    {
        _fightStarted = true;
        _fightStartMs = _totalMs;
    }

    if (status == STATUS_WAIT_LEAVE)
    {
        PvPTeamId const winner = arena->GetWinner();
        _result.FightMs = _fightStarted ? _totalMs - _fightStartMs : 0;
        if (winner == PVP_TEAM_ALLIANCE)
        {
            _result.Outcome = "winner";
            _result.Winner = _sideA.Label;
        }
        else if (winner == PVP_TEAM_HORDE)
        {
            _result.Outcome = "winner";
            _result.Winner = _sideB.Label;
        }
        else
            _result.Outcome = "draw";

        EnterStage(Stage::TearDown);
        return;
    }

    if (!slowTick)
        return;

    if (!_acceptFallbackSent && _stageMs >= AcceptFallbackMs)
        SendAcceptFallback();

    if (_stageMs >= sPlayerbotAIConfig.PvpTestTimeoutSeconds * MsPerSecond)
    {
        _result.Outcome = "timeout";
        _result.Reason =
            Acore::StringFormat("no winner after {} s, ended as a draw", sPlayerbotAIConfig.PvpTestTimeoutSeconds);
        _result.FightMs = _fightStarted ? _totalMs - _fightStartMs : 0;
        arena->EndBattleground(PVP_TEAM_NEUTRAL);
        EnterStage(Stage::TearDown);
    }
}

// Bots accept through BGStatusAction when the invite packet reaches them; this sends the same accept for any bot
// whose AI has not done so in time
void PvpTestArenaJob::SendAcceptFallback()
{
    _acceptFallbackSent = true;
    for (ObjectGuid const& guid : AllMembers())
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player || player->InBattleground() || !player->IsInvitedForBattlegroundQueueType(_queueTypeId))
            continue;

        WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
        packet << _params.ArenaType << uint8(0) << uint32(_bgTypeId) << BattlefieldPortUnknown << BattlefieldPortAccept;
        player->GetSession()->QueuePacket(new WorldPacket(packet));
        LOG_INFO("playerbots", "PVPTEST arena {} vs {}: {} had not entered after {} s, accept sent", _sideA.Label,
                 _sideB.Label, player->GetName(), AcceptFallbackMs / MsPerSecond);
    }
}

void PvpTestArenaJob::UpdateTearDown()
{
    bool pending = false;
    for (ObjectGuid const& guid : AllMembers())
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player)
            continue;

        if (player->InBattleground())
        {
            player->LeaveBattleground();
            pending = true;
            continue;
        }

        // Invited but never entered
        if (_queueTypeId != BATTLEGROUND_QUEUE_NONE &&
            player->InBattlegroundQueueForBattlegroundQueueType(_queueTypeId))
        {
            sBattlegroundMgr->GetBattlegroundQueue(_queueTypeId).RemovePlayer(guid, true);
            player->RemoveBattlegroundQueueId(_queueTypeId);
        }

        if (!player->IsInWorld() || player->IsBeingTeleported())
            pending = true;
    }

    if (pending && _stageMs < TearDownTimeoutMs)
        return;

    FinishTearDown(pending);
}

void PvpTestArenaJob::FinishTearDown(bool timedOut)
{
    for (PvpTestSide* side : {&_sideA, &_sideB})
    {
        if (side->GroupId)
        {
            if (Group* group = sGroupMgr->GetGroupByGUID(side->GroupId))
                group->Disband(true);
        }

        side->GroupId = 0;
    }

    for (ObjectGuid const& guid : AllMembers())
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
        if (!botAI)
            continue;

        botAI->Reset();
        botAI->ResetStrategies(!sRandomPlayerbotMgr.IsRandomBot(player));
    }

    if (timedOut)
        _result.Reason += Acore::StringFormat("{}tear-down timed out after {} s", _result.Reason.empty() ? "" : "; ",
                                              TearDownTimeoutMs / MsPerSecond);

    if (_result.Outcome.empty())
        _result.Outcome = "ended";

    _result.TotalMs = _totalMs;
    EnterStage(Stage::Done);
}

std::string PvpTestArenaJob::Describe() const
{
    return Acore::StringFormat("PVPTEST arena {} vs {}: stage {} for {} s, total {}, map {}, rated {}, seed {}",
                               DescribeSide(_sideA), DescribeSide(_sideB), StageName(_stage), _stageMs / MsPerSecond,
                               FormatDuration(_totalMs), _result.Map.empty() ? "(not created)" : _result.Map,
                               _params.Rated ? 1 : 0, _params.Seed);
}

std::string PvpTestArenaJob::ResultLine() const
{
    std::string const head = Acore::StringFormat("PVPTEST arena {} vs {}: ", _sideA.Label, _sideB.Label);
    std::string const tail = Acore::StringFormat(", map {}, rated {}, seed {}", _result.Map.empty() ? "-" : _result.Map,
                                                 _params.Rated ? 1 : 0, _params.Seed);
    std::string const reason = _result.Reason.empty() ? "" : " (" + _result.Reason + ")";
    if (_result.Outcome == "failed" || _result.Outcome == "stopped")
        return head + _result.Outcome + " in stage " + _result.FailedStage + reason + tail;

    std::string const durations = Acore::StringFormat(" after {} of fighting (total {})",
                                                      FormatDuration(_result.FightMs), FormatDuration(_result.TotalMs));
    if (_result.Outcome == "winner")
        return head + "Winner: " + _result.Winner + durations + reason + tail;

    return head + _result.Outcome + durations + reason + tail;
}

void PvpTestMgr::Update(uint32 diff)
{
    if (_jobs.empty())
        return;

    for (std::unique_ptr<PvpTestArenaJob>& job : _jobs)
        job->Update(diff);

    for (auto itr = _jobs.begin(); itr != _jobs.end();)
    {
        if (!(*itr)->IsDone())
        {
            ++itr;
            continue;
        }

        Report(**itr);
        itr = _jobs.erase(itr);
    }
}

void PvpTestMgr::Shutdown() { _jobs.clear(); }

// The command returns before the match ends, so the result goes to the log, to the issuing player if still online,
// and to the status command
void PvpTestMgr::Report(PvpTestArenaJob const& job)
{
    _lastResult = job.ResultLine();
    _lastMembers = job.AllMembers();
    LOG_INFO("playerbots", "{}", _lastResult);
    if (Player* issuer = ObjectAccessor::FindConnectedPlayer(job.GetIssuer()))
        ChatHandler(issuer->GetSession()).SendSysMessage(_lastResult);
}

bool PvpTestMgr::HandleArenaCommand(ChatHandler* handler, char const* args)
{
    if (!CheckEnabled(handler))
        return false;

    PvpTestMgr& manager = Instance();
    if (!manager._jobs.empty())
        return CommandFail(handler, "A pvptest job runs already (.playerbots pvptest status | stop)");

    PvpTestArenaParams params;
    std::string mapKey = "random";
    bool seedGiven = false;
    std::vector<std::string> sides;
    for (std::string const& token : Tokenize(args ? args : ""))
    {
        size_t const equals = token.find('=');
        if (equals == std::string::npos)
        {
            sides.push_back(token);
            continue;
        }

        std::string const key = ToLower(token.substr(0, equals));
        std::string const value = token.substr(equals + 1);
        if (key == "map")
            mapKey = ToLower(value);
        else if (key == "rated" && (value == "0" || value == "1"))
            params.Rated = value == "1";
        else if (key == "seed" && Acore::StringTo<uint32>(value))
        {
            params.Seed = *Acore::StringTo<uint32>(value);
            seedGiven = true;
        }
        else
            return CommandFail(handler, Acore::StringFormat("Bad option '{}'. {}", token, Usage));
    }

    if (sides.size() != 2)
        return CommandFail(handler, Usage);

    params.SideA = sides[0];
    params.SideB = sides[1];
    if (!seedGiven)
        params.Seed = urand(1, std::numeric_limits<int32>::max());

    std::string error;
    PvpTestSide sideA;
    PvpTestSide sideB;
    if (!ChooseMap(mapKey, params, error) || !PvpTestArenaJob::Resolve(params, sideA, sideB, error))
        return CommandFail(handler, error);

    WorldSession* session = handler->GetSession();
    ObjectGuid const issuer = session && session->GetPlayer() ? session->GetPlayer()->GetGUID() : ObjectGuid::Empty;
    std::unique_ptr<PvpTestArenaJob> job =
        std::make_unique<PvpTestArenaJob>(params, std::move(sideA), std::move(sideB), issuer);
    handler->PSendSysMessage("{} (the result follows in the log as PVPTEST and in .playerbots pvptest status)",
                             job->Describe());
    manager._jobs.push_back(std::move(job));
    return true;
}

bool PvpTestMgr::HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    if (!CheckEnabled(handler))
        return false;

    PvpTestMgr& manager = Instance();
    if (manager._jobs.empty())
    {
        handler->SendSysMessage("No pvptest job runs");
        if (!manager._lastResult.empty())
            handler->PSendSysMessage("Last result: {}", manager._lastResult);

        for (ObjectGuid const& guid : manager._lastMembers)
            handler->SendSysMessage(DescribeMember(guid));

        return true;
    }

    for (std::unique_ptr<PvpTestArenaJob> const& job : manager._jobs)
    {
        handler->SendSysMessage(job->Describe());
        for (ObjectGuid const& guid : job->AllMembers())
            handler->SendSysMessage(DescribeMember(guid));
    }

    return true;
}

bool PvpTestMgr::HandleStopCommand(ChatHandler* handler, char const* /*args*/)
{
    if (!CheckEnabled(handler))
        return false;

    PvpTestMgr& manager = Instance();
    if (manager._jobs.empty())
        return CommandFail(handler, "No pvptest job runs");

    for (std::unique_ptr<PvpTestArenaJob>& job : manager._jobs)
    {
        job->Abort("stopped by a GM");
        handler->PSendSysMessage("Stopping: {}", job->Describe());
    }

    return true;
}
