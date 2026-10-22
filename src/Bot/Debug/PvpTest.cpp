/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpTest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <limits>
#include <random>
#include <set>
#include <sstream>

#include "AiFactory.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
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

using namespace PvpTestUtil;

namespace
{
constexpr uint32 AcceptFallbackMs = 15000;         // bots that have not entered by then get the accept packet sent
constexpr uint16 BattlefieldPortUnknown = 0x1F90;  // the constant the client sends, as in BGStatusAction
constexpr uint8 BattlefieldPortAccept = 1;
constexpr uint32 MaxPlanConcurrency = 4;  // matches of one plan at once
constexpr uint32 MaxPlanTotal = 10000;
constexpr size_t RecentOutcomeCount = 3;  // outcomes shown by "plan status"

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

constexpr char const* PlanUsage =
    "Usage: .playerbots pvptest plan start total=<n> [concurrency=<n>] [seed=<base>] [map=na|be|rl|ds|rv|random] "
    "[pool=random|teams:<teamA>;<teamB>;...|bots:<name>,<name>,...] (quote a token that contains spaces)";

constexpr char const* Usage =
    "Usage: .playerbots pvptest arena <teamA|name,name> <teamB|name,name> [map=na|be|rl|ds|rv|random] [rated=0|1] "
    "[seed=<n>] (quote team names with spaces)";

std::string DescribeSide(PvpTestSide const& side)
{
    std::string names;
    for (std::string const& name : side.Names)
        names += names.empty() ? name : ", " + name;

    return Acore::StringFormat("{} [{}]", side.Label, names);
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
    // The record has every field from the start, so a failed or stopped match is still a complete line
    _record.Id = _params.MatchId;
    _record.PlanId = _params.PlanId;
    _record.Index = _params.PlanIndex;
    _record.StartTime = PvpTestRecordWriter::IsoTimeNow();
    _record.Seed = _params.Seed;
    _record.ArenaType = _params.ArenaType;
    _record.Rated = _params.Rated;
    if (Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(_params.Map))
        _record.Map = bgTemplate->GetName();

    for (size_t index = 0; index < _record.Sides.size(); ++index)
    {
        PvpTestSide const& side = index == 0 ? _sideA : _sideB;
        PvpTestSideRecord& sideRecord = _record.Sides[index];
        sideRecord.Label = side.Label;
        sideRecord.Side = index == 0 ? 'A' : 'B';
        sideRecord.TeamId = side.ArenaTeamId;
        sideRecord.Faction = index == 0 ? "alliance" : "horde";
        for (size_t member = 0; member < side.Members.size(); ++member)
        {
            PvpTestBotRecord bot;
            bot.Guid = side.Members[member].GetCounter();
            bot.Name = side.Names[member];
            if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(side.Members[member]))
            {
                bot.ClassId = entry->Class;
                bot.RaceId = entry->Race;
                bot.Level = entry->Level;
            }

            sideRecord.Bots.push_back(std::move(bot));
        }
    }

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

    // The scores die with the arena, so a stopped fight keeps what was counted so far
    if (_stage == Stage::Monitor)
    {
        if (Battleground* arena = sBattlegroundMgr->GetBattleground(_instanceId, _bgTypeId))
            CaptureScores(arena);

        _result.FightMs = _fightStarted ? _totalMs - _fightStartMs : 0;
    }

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
    _record.Map = _result.Map;
    CaptureRoster();
    LOG_INFO("playerbots", "PVPTEST arena {} vs {}: {} instance {} created, rated {}, seed {}", DescribeSide(_sideA),
             DescribeSide(_sideB), _result.Map, _instanceId, _params.Rated ? 1 : 0, _params.Seed);
    return true;
}

// Class, race, level, spec and gear score of the bots while all of them are online
void PvpTestArenaJob::CaptureRoster()
{
    for (PvpTestSideRecord& sideRecord : _record.Sides)
    {
        for (PvpTestBotRecord& bot : sideRecord.Bots)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(bot.Guid));
            if (!player)
                continue;

            bot.ClassId = player->getClass();
            bot.RaceId = player->getRace();
            bot.Level = player->GetLevel();
            bot.Spec = AiFactory::GetPlayerSpecName(player);
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(player))
                bot.GearScore = botAI->GetEquipGearScore(player);
        }
    }
}

// The battleground's score map dies with the battleground, and a bot that leaves takes its score entry along, so this
// runs on every slow tick while the fight is on and again when the winner is first seen. It only ever fills in: a bot
// whose entry is already gone keeps the values of the last snapshot. A bot that never had a score entry never entered
// the arena; its scores stay zero.
void PvpTestArenaJob::CaptureScores(Battleground const* arena)
{
    auto const* scores = arena->GetPlayerScores();
    for (PvpTestSideRecord& sideRecord : _record.Sides)
    {
        for (PvpTestBotRecord& bot : sideRecord.Bots)
        {
            auto const itr = scores->find(bot.Guid);
            if (itr == scores->end() || !itr->second)
                continue;

            bot.Entered = true;
            bot.Damage = itr->second->GetDamageDone();
            bot.Healing = itr->second->GetHealingDone();
            bot.KillingBlows = itr->second->GetKillingBlows();
            bot.Deaths = itr->second->GetDeaths();
            bot.HonorableKills = itr->second->GetHonorableKills();
        }
    }
}

PvpTestMatchRecord PvpTestArenaJob::BuildRecord() const
{
    PvpTestMatchRecord record = _record;
    record.Outcome = _result.Outcome;
    record.Winner = _result.Winner;
    record.WinnerSide = _result.WinnerSide;
    record.Reason = _result.Reason;
    record.FailedStage = _result.FailedStage;
    record.FightMs = _result.FightMs;
    record.TotalMs = _result.TotalMs ? _result.TotalMs : _totalMs;
    if (record.Outcome.empty())
    {
        // Only reached for a job dropped by the server shutdown
        bool const unfinished = _stage != Stage::Done;
        record.Outcome = unfinished ? "stopped" : "ended";
        if (unfinished)
        {
            record.FailedStage = StageName(_stage);
            record.Reason = "server shutdown";
        }
    }

    return record;
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
        CaptureScores(arena);
        if (winner == PVP_TEAM_ALLIANCE)
        {
            _result.Outcome = "winner";
            _result.Winner = _sideA.Label;
            _result.WinnerSide = "A";
        }
        else if (winner == PVP_TEAM_HORDE)
        {
            _result.Outcome = "winner";
            _result.Winner = _sideB.Label;
            _result.WinnerSide = "B";
        }
        else
            _result.Outcome = "draw";

        EnterStage(Stage::TearDown);
        return;
    }

    if (!slowTick)
        return;

    // The rolling snapshot (C4, F1): the winning side's bots may leave before the end is seen
    if (status == STATUS_IN_PROGRESS)
        CaptureScores(arena);

    if (!_acceptFallbackSent && _stageMs >= AcceptFallbackMs)
        SendAcceptFallback();

    if (_stageMs >= sPlayerbotAIConfig.PvpTestTimeoutSeconds * MsPerSecond)
    {
        _result.Outcome = "timeout";
        _result.Reason =
            Acore::StringFormat("no winner after {} s, ended as a draw", sPlayerbotAIConfig.PvpTestTimeoutSeconds);
        _result.FightMs = _fightStarted ? _totalMs - _fightStartMs : 0;
        arena->EndBattleground(PVP_TEAM_NEUTRAL);
        CaptureScores(arena);
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
    return Acore::StringFormat(
        "PVPTEST arena {} vs {}: stage {} for {} s, total {}, map {}, rated {}, seed {}, match {}",
        DescribeSide(_sideA), DescribeSide(_sideB), StageName(_stage), _stageMs / MsPerSecond, FormatDuration(_totalMs),
        _result.Map.empty() ? "(not created)" : _result.Map, _params.Rated ? 1 : 0, _params.Seed, _params.MatchId);
}

std::string PvpTestArenaJob::ResultLine() const
{
    std::string const head = Acore::StringFormat("PVPTEST arena {} vs {}: ", _sideA.Label, _sideB.Label);
    std::string const tail =
        Acore::StringFormat(", map {}, rated {}, seed {}, match {}", _result.Map.empty() ? "-" : _result.Map,
                            _params.Rated ? 1 : 0, _params.Seed, _params.MatchId);
    std::string const reason = _result.Reason.empty() ? "" : " (" + _result.Reason + ")";
    if (_result.Outcome == "failed" || _result.Outcome == "stopped")
        return head + _result.Outcome + " in stage " + _result.FailedStage + reason + tail;

    std::string const durations = Acore::StringFormat(" after {} of fighting (total {})",
                                                      FormatDuration(_result.FightMs), FormatDuration(_result.TotalMs));
    if (_result.Outcome == "winner")
        return head + "Winner: " + _result.Winner + durations + reason + tail;

    return head + _result.Outcome + durations + reason + tail;
}

namespace
{
// Fisher-Yates with the seeded generator, so a seed draws the same order on every platform
template <typename T>
void Shuffle(std::vector<T>& items, uint32 seed)
{
    std::mt19937 generator(seed);
    for (size_t index = items.size(); index > 1; --index)
        std::swap(items[index - 1], items[generator() % index]);
}

char const* PoolName(PvpTestPool pool)
{
    switch (pool)
    {
        case PvpTestPool::Random:
            return "random";
        case PvpTestPool::Teams:
            return "teams";
        case PvpTestPool::Bots:
            return "bots";
    }

    return "unknown";
}

bool SidesOverlap(PvpTestSide const& first, PvpTestSide const& second)
{
    return std::any_of(
        first.Members.begin(), first.Members.end(), [&second](ObjectGuid const& guid)
        { return std::find(second.Members.begin(), second.Members.end(), guid) != second.Members.end(); });
}

// The record of a plan match that could not be set up: the roster as far as the draw gave one, outcome failed
PvpTestMatchRecord SetupFailureRecord(PvpTestArenaParams const& params, std::string const& error)
{
    PvpTestMatchRecord record;
    record.Id = params.MatchId;
    record.PlanId = params.PlanId;
    record.Index = params.PlanIndex;
    record.StartTime = PvpTestRecordWriter::IsoTimeNow();
    record.Seed = params.Seed;
    record.ArenaType = params.ArenaType;
    record.Rated = params.Rated;
    if (Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(params.Map))
        record.Map = bgTemplate->GetName();

    record.Outcome = "failed";
    record.Reason = error;
    record.FailedStage = "resolve";
    std::array<std::string const*, 2> const specs = {{&params.SideA, &params.SideB}};
    for (size_t index = 0; index < record.Sides.size(); ++index)
    {
        PvpTestSideRecord& side = record.Sides[index];
        side.Label = *specs[index];
        side.Side = index == 0 ? 'A' : 'B';
        side.Faction = index == 0 ? "alliance" : "horde";
        if (side.Label.find(',') == std::string::npos)
            continue;  // an arena team name: its members were not resolved

        for (std::string name : Split(side.Label, ','))
        {
            PvpTestBotRecord bot;
            bot.Name = name;
            ObjectGuid const guid =
                normalizePlayerName(name) ? sCharacterCache->GetCharacterGuidByName(name) : ObjectGuid::Empty;
            if (CharacterCacheEntry const* entry =
                    guid.IsEmpty() ? nullptr : sCharacterCache->GetCharacterCacheByGuid(guid))
            {
                bot.Guid = guid.GetCounter();
                bot.ClassId = entry->Class;
                bot.RaceId = entry->Race;
                bot.Level = entry->Level;
            }

            side.Bots.push_back(std::move(bot));
        }
    }

    return record;
}
}  // namespace

void PvpTestMgr::Update(uint32 diff)
{
    if (_jobs.empty() && _fightJobs.empty() && !_plan)
        return;

    for (std::unique_ptr<PvpTestArenaJob>& job : _jobs)
        job->Update(diff);

    for (std::unique_ptr<PvpTestFightJob>& job : _fightJobs)
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

    for (auto itr = _fightJobs.begin(); itr != _fightJobs.end();)
    {
        if (!(*itr)->IsDone())
        {
            ++itr;
            continue;
        }

        ReportFight(**itr);
        itr = _fightJobs.erase(itr);
    }

    if (!_plan)
        return;

    _planTickMs += diff;
    if (_planTickMs >= TickIntervalMs)
    {
        _planTickMs = 0;
        UpdatePlan();
    }
}

void PvpTestMgr::Shutdown()
{
    // A job that did not finish still leaves its (stopped) record
    for (std::unique_ptr<PvpTestArenaJob> const& job : _jobs)
        PvpTestRecordWriter::Instance().Write(job->BuildRecord());

    for (std::unique_ptr<PvpTestFightJob> const& job : _fightJobs)
        PvpTestRecordWriter::Instance().Write(job->BuildRecord());

    _jobs.clear();
    _fightJobs.clear();
    _plan.reset();
    PvpTestRecordWriter::Instance().Shutdown();
}

// The command returns before the match ends, so the result goes to the log, to the issuing player if still online
// (single matches), and to the status command; the record goes to the writer thread
void PvpTestMgr::Report(PvpTestArenaJob const& job)
{
    _lastResult = job.ResultLine();
    _lastMembers = job.AllMembers();
    PvpTestRecordWriter::Instance().Write(job.BuildRecord());
    LOG_INFO("playerbots", "{}", _lastResult);

    if (_plan && !job.GetPlanId().empty() && job.GetPlanId() == _plan->Id)
    {
        PvpTestResult const& result = job.GetResult();
        ++_plan->Finished;
        if (result.Outcome == "failed")
            ++_plan->Failed;
        else if (result.Outcome == "stopped")
            ++_plan->Stopped;

        _plan->Recent.push_back(Acore::StringFormat("#{} {}{}", job.GetPlanIndex(), result.Outcome,
                                                    result.Winner.empty() ? "" : " " + result.Winner));
        while (_plan->Recent.size() > RecentOutcomeCount)
            _plan->Recent.pop_front();

        return;
    }

    if (Player* issuer = ObjectAccessor::FindConnectedPlayer(job.GetIssuer()))
        ChatHandler(issuer->GetSession()).SendSysMessage(_lastResult);
}

std::set<ObjectGuid> PvpTestMgr::MembersInUse() const
{
    std::set<ObjectGuid> members;
    for (std::unique_ptr<PvpTestArenaJob> const& job : _jobs)
    {
        for (ObjectGuid const& guid : job->AllMembers())
            members.insert(guid);
    }

    for (std::unique_ptr<PvpTestFightJob> const& job : _fightJobs)
    {
        for (ObjectGuid const& guid : job->AllMembers())
            members.insert(guid);
    }

    return members;
}

uint32 PvpTestMgr::ActivePlanJobs() const
{
    return static_cast<uint32>(std::count_if(_jobs.begin(), _jobs.end(), [](std::unique_ptr<PvpTestArenaJob> const& job)
                                             { return !job->GetPlanId().empty(); }));
}

// Fills the two sides of the next match from the pool, leaving out the bots in use and any bot with a real player.
// Returns false when the pool cannot give a roster now.
bool PvpTestMgr::DrawRoster(PvpTestPlan const& plan, std::set<ObjectGuid> const& inUse, PvpTestArenaParams& params)
{
    if (plan.Pool == PvpTestPool::Teams)
    {
        std::vector<size_t> order;
        for (size_t index = 0; index < plan.Teams.size(); ++index)
        {
            PvpTestSide const& team = plan.Teams[index];
            if (std::none_of(team.Members.begin(), team.Members.end(),
                             [&inUse](ObjectGuid const& guid) { return inUse.count(guid) > 0; }))
                order.push_back(index);
        }

        Shuffle(order, params.Seed);
        for (size_t first = 0; first < order.size(); ++first)
        {
            for (size_t second = first + 1; second < order.size(); ++second)
            {
                if (SidesOverlap(plan.Teams[order[first]], plan.Teams[order[second]]))
                    continue;

                params.SideA = plan.Teams[order[first]].Label;
                params.SideB = plan.Teams[order[second]].Label;
                return true;
            }
        }

        return false;
    }

    std::vector<ObjectGuid> available;
    for (ObjectGuid const& guid : plan.Candidates)
    {
        if (inUse.count(guid))
            continue;

        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
            if (!botAI || botAI->HasGameClientMaster())
                continue;
        }

        available.push_back(guid);
    }

    size_t const perSide = params.ArenaType;
    if (available.size() < perSide * 2)
        return false;

    Shuffle(available, params.Seed);
    for (size_t index = 0; index < perSide * 2; ++index)
    {
        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(available[index]);
        if (!entry)
            return false;

        std::string& side = index < perSide ? params.SideA : params.SideB;
        side += side.empty() ? entry->Name : "," + entry->Name;
    }

    return true;
}

// Launches matches up to the concurrency cap; a pool that cannot give a free roster waits for a running match
bool PvpTestMgr::LaunchPlanMatch(std::string& error)
{
    PvpTestArenaParams params;
    params.Seed = _plan->SeedBase + _plan->Launched;
    if (!DrawRoster(*_plan, MembersInUse(), params))
        return false;

    params.PlanId = _plan->Id;
    params.PlanIndex = _plan->Launched + 1;
    params.MatchId = Acore::StringFormat("m-{}-{}", _plan->Id, params.PlanIndex);
    PvpTestSide sideA;
    PvpTestSide sideB;
    if (!ChooseMap(_plan->MapKey, params, error) || !PvpTestArenaJob::Resolve(params, sideA, sideB, error))
    {
        // C4, F2: the match counts as launched, finished and failed, leaves a record and the plan goes on
        ++_plan->Launched;
        ++_plan->Finished;
        ++_plan->Failed;
        PvpTestRecordWriter::Instance().Write(SetupFailureRecord(params, error));
        _lastResult = Acore::StringFormat("PVPTEST plan {} match {}: failed in stage resolve ({}), seed {}", _plan->Id,
                                          params.PlanIndex, error, params.Seed);
        LOG_INFO("playerbots", "{}", _lastResult);
        _plan->Recent.push_back(Acore::StringFormat("#{} failed", params.PlanIndex));
        while (_plan->Recent.size() > RecentOutcomeCount)
            _plan->Recent.pop_front();

        error.clear();
        return true;
    }

    ++_plan->Launched;
    std::unique_ptr<PvpTestArenaJob> job =
        std::make_unique<PvpTestArenaJob>(params, std::move(sideA), std::move(sideB), _plan->Issuer);
    LOG_INFO("playerbots", "{}", job->Describe());
    _jobs.push_back(std::move(job));
    return true;
}

// The plan tick, every 500 ms
void PvpTestMgr::UpdatePlan()
{
    PvpTestPlan& plan = *_plan;
    if (!plan.StopRequested && plan.EndedEarly.empty())
    {
        while (plan.Launched < plan.Total && ActivePlanJobs() < plan.Concurrency)
        {
            std::string error;
            uint32 const failedBefore = plan.Failed;
            if (LaunchPlanMatch(error))
            {
                // A launch error leaves a failed record; the next match waits for the next tick, so an error that
                // repeats cannot write the whole plan at once
                if (plan.Failed != failedBefore)
                    break;

                continue;
            }

            // No error means the free bots are short, which a running match will end
            if (!error.empty())
                plan.EndedEarly = error;
            else if (_jobs.empty())
                plan.EndedEarly = "the pool has no free roster for another match";

            break;
        }
    }

    bool const nothingMoreToLaunch = plan.StopRequested || !plan.EndedEarly.empty() || plan.Launched >= plan.Total;
    if (nothingMoreToLaunch && ActivePlanJobs() == 0)
        FinishPlan();
}

std::string PvpTestMgr::DescribePlan() const
{
    PvpTestPlan const& plan = *_plan;
    std::string recent;
    for (std::string const& outcome : plan.Recent)
        recent += recent.empty() ? outcome : "; " + outcome;

    return Acore::StringFormat(
        "PVPTEST plan {}: total {}, launched {}, finished {} (failed {}, stopped {}), active {}, concurrency {}, seed "
        "base {}, pool {}, map {}{}{}; last: {}; records: {}",
        plan.Id, plan.Total, plan.Launched, plan.Finished, plan.Failed, plan.Stopped, ActivePlanJobs(),
        plan.Concurrency, plan.SeedBase, PoolName(plan.Pool), plan.MapKey, plan.StopRequested ? " (stopped)" : "",
        plan.EndedEarly.empty() ? "" : " (ended early: " + plan.EndedEarly + ")", recent.empty() ? "-" : recent,
        PvpTestRecordWriter::TodayFile());
}

void PvpTestMgr::FinishPlan()
{
    _lastPlan = DescribePlan();
    LOG_INFO("playerbots", "{}", _lastPlan);
    if (Player* issuer = ObjectAccessor::FindConnectedPlayer(_plan->Issuer))
        ChatHandler(issuer->GetSession()).SendSysMessage(_lastPlan);

    _plan.reset();
}

bool PvpTestMgr::HandleArenaCommand(ChatHandler* handler, char const* args)
{
    if (!CheckEnabled(handler))
        return false;

    PvpTestMgr& manager = Instance();
    if (manager._plan)
        return CommandFail(handler, "A pvptest plan runs (.playerbots pvptest plan status | stop)");

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

    std::set<ObjectGuid> const inUse = manager.MembersInUse();
    for (PvpTestSide const* side : {&sideA, &sideB})
    {
        for (ObjectGuid const& guid : side->Members)
        {
            if (inUse.count(guid))
                return CommandFail(handler, "A bot of this match is in another pvptest job already");
        }
    }

    params.MatchId = Acore::StringFormat("m-single-{}-{}", std::time(nullptr), ++manager._singleCounter);
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
    if (manager._plan)
        handler->SendSysMessage(manager.DescribePlan());

    if (manager._jobs.empty() && manager._fightJobs.empty())
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

    for (std::unique_ptr<PvpTestFightJob> const& job : manager._fightJobs)
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
    if (manager._jobs.empty() && manager._fightJobs.empty() && !manager._plan)
        return CommandFail(handler, "No pvptest job runs");

    if (manager._plan)
        manager._plan->StopRequested = true;

    for (std::unique_ptr<PvpTestArenaJob>& job : manager._jobs)
    {
        job->Abort("stopped by a GM");
        handler->PSendSysMessage("Stopping: {}", job->Describe());
    }

    for (std::unique_ptr<PvpTestFightJob>& job : manager._fightJobs)
    {
        job->Abort("stopped by a GM");
        handler->PSendSysMessage("Stopping: {}", job->Describe());
    }

    return true;
}

bool PvpTestMgr::HandlePlanStartCommand(ChatHandler* handler, char const* args)
{
    if (!CheckEnabled(handler))
        return false;

    PvpTestMgr& manager = Instance();
    if (manager._plan)
        return CommandFail(handler, "A pvptest plan runs already (.playerbots pvptest plan status | stop)");

    std::unique_ptr<PvpTestPlan> plan = std::make_unique<PvpTestPlan>();
    std::string poolSpec = "random";
    bool seedGiven = false;
    uint32 requestedConcurrency = 1;
    for (std::string const& token : Tokenize(args ? args : ""))
    {
        size_t const equals = token.find('=');
        std::string const key = equals == std::string::npos ? "" : ToLower(token.substr(0, equals));
        std::string const value = equals == std::string::npos ? "" : token.substr(equals + 1);
        Optional<uint32> const number = Acore::StringTo<uint32>(value);
        if (key == "total" && number && *number >= 1 && *number <= MaxPlanTotal)
            plan->Total = *number;
        else if (key == "concurrency" && number && *number >= 1)
            requestedConcurrency = *number;
        else if (key == "seed" && number)
        {
            plan->SeedBase = *number;
            seedGiven = true;
        }
        else if (key == "pool" && !value.empty())
            poolSpec = value;
        else if (key == "map" && !value.empty())
            plan->MapKey = ToLower(value);
        else
            return CommandFail(handler, Acore::StringFormat("Bad option '{}'. {}", token, PlanUsage));
    }

    if (!plan->Total)
        return CommandFail(handler, PlanUsage);

    if (!seedGiven)
        plan->SeedBase = urand(1, std::numeric_limits<int32>::max());

    std::string error;
    PvpTestArenaParams scratch;
    scratch.Seed = plan->SeedBase;
    if (!ChooseMap(plan->MapKey, scratch, error))
        return CommandFail(handler, error);

    // The pool: "random", "teams:<team>;<team>;..." or "bots:<name>,<name>,..."
    size_t const colon = poolSpec.find(':');
    std::string const poolKind = ToLower(colon == std::string::npos ? poolSpec : poolSpec.substr(0, colon));
    std::string const poolList = colon == std::string::npos ? "" : poolSpec.substr(colon + 1);
    uint32 capacity = 0;
    if (poolKind == "random" && colon == std::string::npos)
    {
        // One query when the plan starts, none while it runs
        plan->Pool = PvpTestPool::Random;
        QueryResult result = CharacterDatabase.Query("SELECT guid FROM characters WHERE level = {}",
                                                     sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        if (result)
        {
            do
            {
                ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>((*result)[0].Get<uint32>());
                std::string ignored;
                if (CheckBot(guid, ignored))
                    plan->Candidates.push_back(guid);
            } while (result->NextRow());
        }
    }
    else if (poolKind == "bots" && !poolList.empty())
    {
        plan->Pool = PvpTestPool::Bots;
        for (std::string name : Split(poolList, ','))
        {
            ObjectGuid const guid =
                normalizePlayerName(name) ? sCharacterCache->GetCharacterGuidByName(name) : ObjectGuid::Empty;
            if (guid.IsEmpty())
                return CommandFail(handler, Acore::StringFormat("No character named '{}'", name));

            if (!CheckBot(guid, error))
                return CommandFail(handler, error);

            if (std::find(plan->Candidates.begin(), plan->Candidates.end(), guid) == plan->Candidates.end())
                plan->Candidates.push_back(guid);
        }
    }
    else if (poolKind == "teams" && !poolList.empty())
    {
        plan->Pool = PvpTestPool::Teams;
        for (std::string const& spec : Split(poolList, ';'))
        {
            PvpTestSide team;
            if (spec.find(',') != std::string::npos || !ResolveSide(spec, scratch.ArenaType, team, error))
                return CommandFail(handler, error.empty() ? "A teams pool lists arena team names" : error);

            if (std::any_of(plan->Teams.begin(), plan->Teams.end(),
                            [&team](PvpTestSide const& other) { return other.Label == team.Label; }))
                return CommandFail(handler, Acore::StringFormat("Team '{}' is listed twice", team.Label));

            plan->Teams.push_back(std::move(team));
        }
    }
    else
        return CommandFail(handler, Acore::StringFormat("Bad pool '{}'. {}", poolSpec, PlanUsage));

    std::sort(plan->Candidates.begin(), plan->Candidates.end());
    if (plan->Pool == PvpTestPool::Teams)
        capacity = static_cast<uint32>(plan->Teams.size() / 2);
    else
        capacity = static_cast<uint32>(plan->Candidates.size() / (2u * scratch.ArenaType));

    if (!capacity)
        return CommandFail(handler, Acore::StringFormat("The {} pool cannot fill one {}v{} match", PoolName(plan->Pool),
                                                        scratch.ArenaType, scratch.ArenaType));

    plan->Concurrency = std::min({requestedConcurrency, capacity, MaxPlanConcurrency});
    if (plan->Concurrency != requestedConcurrency)
        handler->PSendSysMessage("Concurrency {} is reduced to {} (the pool fits {} matches at once, the cap is {})",
                                 requestedConcurrency, plan->Concurrency, capacity, MaxPlanConcurrency);

    WorldSession* session = handler->GetSession();
    plan->Issuer = session && session->GetPlayer() ? session->GetPlayer()->GetGUID() : ObjectGuid::Empty;
    plan->Id = Acore::StringFormat("p-{}", std::time(nullptr));
    manager._plan = std::move(plan);
    manager._planTickMs = TickIntervalMs;  // the first matches launch on the next tick
    handler->PSendSysMessage("{}", manager.DescribePlan());
    handler->SendSysMessage("Matches launch from the next tick; results in .playerbots pvptest plan status");
    return true;
}

bool PvpTestMgr::HandlePlanStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    if (!CheckEnabled(handler))
        return false;

    PvpTestMgr& manager = Instance();
    if (!manager._plan)
    {
        handler->SendSysMessage("No pvptest plan runs");
        if (!manager._lastPlan.empty())
            handler->PSendSysMessage("Last plan: {}", manager._lastPlan);

        return true;
    }

    handler->SendSysMessage(manager.DescribePlan());
    for (std::unique_ptr<PvpTestArenaJob> const& job : manager._jobs)
    {
        if (!job->GetPlanId().empty())
            handler->SendSysMessage(job->Describe());
    }

    return true;
}

// Aborts the running matches of the plan (their records are written as stopped) and launches no more
bool PvpTestMgr::HandlePlanStopCommand(ChatHandler* handler, char const* /*args*/)
{
    if (!CheckEnabled(handler))
        return false;

    PvpTestMgr& manager = Instance();
    if (!manager._plan)
        return CommandFail(handler, "No pvptest plan runs");

    manager._plan->StopRequested = true;
    for (std::unique_ptr<PvpTestArenaJob>& job : manager._jobs)
    {
        if (job->GetPlanId().empty())
            continue;

        job->Abort("plan stopped by a GM");
        handler->PSendSysMessage("Stopping: {}", job->Describe());
    }

    handler->SendSysMessage("The plan launches no more matches and ends when the running ones are torn down");
    return true;
}
