/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestPlayer.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <sstream>

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
constexpr uint32 CheckIntervalMs = 500;       // route check cadence
constexpr uint32 PendingTimeoutMs = 30000;    // give up a creation or login that never completes
constexpr uint32 MaxAttemptsPerWaypoint = 3;  // re-issues of a leg that ended short before skipping the point
constexpr float ArrivalRadius = 5.0f;         // 2D distance that counts as "at the waypoint"
constexpr float TeleportDistance = 100.0f;    // farther than this from the first waypoint: teleport there
constexpr uint8 TestPlayerLevel = 20;         // mobs along the Elwynn route do not kill it

// Positions of NPCs from acore_world.creature (z + 0.5); each leg is a street or a field path, mmaps pathing
// handles the turns.
std::map<std::string, std::vector<TestPlayerWaypoint>> const Routes = {
    {"stormwind-loop",
     {
         {0, -8814.55f, 660.32f, 97.18f},   // Trade District, Auctioneer Jaxon
         {0, -8867.79f, 673.67f, 98.49f},   // Trade District, Gilded Rose (Innkeeper Allison)
         {0, -8931.23f, 605.80f, 100.11f},  // Stormwind bank, Olivia Burnside
         {0, -8845.68f, 566.49f, 95.27f},   // Trade District, Elaine Trias
         {0, -8831.75f, 596.35f, 96.26f},   // Trade District, Thurman Mullby
         {0, -8623.24f, 766.66f, 97.90f},   // Cathedral Square, Brother Kristoff
         {0, -8523.00f, 848.78f, 107.20f},  // Cathedral of Light, Archbishop Benedictus
         {0, -8577.00f, 881.70f, 107.02f},  // Cathedral of Light, Lord Grayson Shadowbreaker
     }},
    {"elwynn-questing",
     {
         {0, -9462.66f, 16.19f, 57.55f},   // Goldshire, Lion's Pride Inn (Innkeeper Farley)
         {0, -9465.52f, 74.01f, 57.28f},   // Goldshire, Marshal Dughan
         {0, -9456.65f, 87.86f, 58.93f},   // Goldshire, Smith Argus
         {0, -9552.61f, 151.45f, 59.91f},  // fields west of Goldshire
         {0, -9770.83f, 279.17f, 43.62f},  // near Fargodeep Mine
         {0, -9895.00f, 332.42f, 37.05f},  // Stonefield Farm, Ma Stonefield
         {0, -9930.05f, 499.73f, 33.02f},  // Stonefield fields, Tommy Joe Stonefield
         {0, -9944.80f, 376.16f, 35.49f},  // fields south of Stonefield Farm
         {0, -9986.97f, 156.06f, 35.34f},  // fields towards the Maclure Vineyards
         {0, -10014.00f, 37.60f, 35.75f},  // Maclure Vineyards, Maybell Maclure
         {0, -9741.90f, 31.89f, 38.75f},   // fields south of Goldshire
         {0, -9612.56f, 17.92f, 62.13f},   // road into Goldshire
     }},
};

bool Fail(ChatHandler* handler, std::string const& text)
{
    handler->SendSysMessage(text);
    handler->SetSentErrorMessage(true);
    return false;
}

bool CheckEnabled(ChatHandler* handler)
{
    if (sPlayerbotAIConfig.TestPlayerEnable)
        return true;

    return Fail(handler, "The test player is disabled (Playerbots.TestPlayer.Enable = 0)");
}
}  // namespace

std::vector<TestPlayerWaypoint> const* TestPlayerMgr::FindRoute(std::string const& name)
{
    auto const itr = Routes.find(name);
    return itr == Routes.end() ? nullptr : &itr->second;
}

std::string TestPlayerMgr::RouteNames()
{
    std::string names;
    for (auto const& route : Routes)
        names += (names.empty() ? "" : ", ") + route.first;

    return names;
}

bool TestPlayerMgr::HandleLoginCommand(ChatHandler* handler, char const* args)
{
    if (!CheckEnabled(handler))
        return false;

    std::istringstream in(args ? args : "");
    std::string name;
    std::string route;
    std::string token;
    in >> name;
    while (in >> token)
    {
        if (token.rfind("route=", 0) == 0)
            route = token.substr(6);
        else
            return Fail(handler, "Usage: .playerbots testplayer login <character> [route=<name>]");
    }

    if (name.empty())
        return Fail(handler, "Usage: .playerbots testplayer login <character> [route=<name>]");

    return Instance().Login(handler, name, route);
}

bool TestPlayerMgr::HandleRouteCommand(ChatHandler* handler, char const* args)
{
    if (!CheckEnabled(handler))
        return false;

    std::istringstream in(args ? args : "");
    std::string route;
    in >> route;
    if (route.empty())
        return Fail(handler, "Usage: .playerbots testplayer route <name>|none (routes: " + RouteNames() + ")");

    return Instance().SetRoute(handler, route);
}

bool TestPlayerMgr::HandleLogoutCommand(ChatHandler* handler, char const* /*args*/)
{
    if (!CheckEnabled(handler))
        return false;

    return Instance().Logout(handler);
}

bool TestPlayerMgr::HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    if (!CheckEnabled(handler))
        return false;

    Instance().Status(handler);
    return true;
}

bool TestPlayerMgr::HandleWhisperCommand(ChatHandler* handler, char const* args)
{
    if (!CheckEnabled(handler))
        return false;

    std::istringstream in(args ? args : "");
    std::string target;
    std::string text;
    in >> target;
    std::getline(in >> std::ws, text);
    if (target.empty() || text.empty())
        return Fail(handler, "Usage: .playerbots testplayer whisper <character> <text>");

    return Instance().Whisper(handler, target, text);
}

bool TestPlayerMgr::HandleGroupCommand(ChatHandler* handler, char const* args)
{
    if (!CheckEnabled(handler))
        return false;

    std::istringstream in(args ? args : "");
    std::string target;
    in >> target;
    if (target.empty())
        return Fail(handler, "Usage: .playerbots testplayer group <character>|none");

    return Instance().GroupWith(handler, target);
}

Player* TestPlayerMgr::OnlinePlayer() const
{
    return _state == State::Online && _session ? _session->GetPlayer() : nullptr;
}

bool TestPlayerMgr::Whisper(ChatHandler* handler, std::string const& target, std::string const& text)
{
    Player* player = OnlinePlayer();
    if (!player)
        return Fail(handler, "No test player is online");

    Player* receiver = ObjectAccessor::FindPlayerByName(target);
    if (!receiver)
        return Fail(handler, Acore::StringFormat("{} is not online", target));

    // The faction's own language: the handler refuses universal from a player who is no GM
    uint32 const lang = player->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH;
    WorldPacket packet(CMSG_MESSAGECHAT, 4 + 4 + receiver->GetName().size() + 1 + text.size() + 1);
    packet << uint32(CHAT_MSG_WHISPER);
    packet << lang;
    packet << receiver->GetName();
    packet << text;
    _session->HandleMessagechatOpcode(packet);
    handler->PSendSysMessage("Test player {} whispered {}: {}", _name, receiver->GetName(), text);
    return true;
}

bool TestPlayerMgr::GroupWith(ChatHandler* handler, std::string const& target)
{
    Player* player = OnlinePlayer();
    if (!player)
        return Fail(handler, "No test player is online");

    if (target == "none")
    {
        Group* group = player->GetGroup();
        if (!group)
            return Fail(handler, "The test player is in no group");

        group->RemoveMember(player->GetGUID());
        handler->PSendSysMessage("Test player {} left its group", _name);
        return true;
    }

    Player* member = ObjectAccessor::FindPlayerByName(target);
    if (!member || member == player)
        return Fail(handler, Acore::StringFormat("{} is not online", target));

    if (member->GetGroup() || member->InBattleground())
        return Fail(handler, Acore::StringFormat("{} is in a group or a battleground", target));

    // No invite round trip: a headless session has no client to answer one
    Group* group = player->GetGroup();
    if (!group)
    {
        group = new Group();
        if (!group->Create(player))
        {
            delete group;
            return Fail(handler, "The group could not be created");
        }

        sGroupMgr->AddGroup(group);
    }

    if (group->IsFull() || !group->AddMember(member))
        return Fail(handler, Acore::StringFormat("{} could not join the test player's group", target));

    handler->PSendSysMessage("Test player {} groups with {}", _name, member->GetName());
    LOG_INFO("playerbots", "TestPlayer: {} groups with {}", _name, member->GetName());
    return true;
}

bool TestPlayerMgr::Login(ChatHandler* handler, std::string name, std::string const& route)
{
    if (_state != State::Idle)
        return Fail(handler, "A test player is already logged in or loading; use .playerbots testplayer logout first");

    std::vector<TestPlayerWaypoint> const* waypoints = nullptr;
    if (!route.empty())
    {
        waypoints = FindRoute(route);
        if (!waypoints)
            return Fail(handler, "Unknown route '" + route + "' (routes: " + RouteNames() + ")");
    }

    std::string const& accountName = sPlayerbotAIConfig.TestPlayerAccount;
    uint32 const accountId = AccountMgr::GetId(accountName);
    if (!accountId)
        return Fail(handler, "Account '" + accountName + "' (Playerbots.TestPlayer.Account) does not exist");

    // The random-bot rotation logs its own accounts' characters in and out
    if (sPlayerbotAIConfig.IsInRandomAccountList(accountId))
        return Fail(handler, "Account '" + accountName + "' is a random-bot account; use a plain account");

    if (!normalizePlayerName(name))
        return Fail(handler, "'" + name + "' is not a usable character name");

    WorldSession* session = new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING,
                                             time_t(0), sWorld->GetDefaultDbcLocale(), 0, false, false, 0);

    ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
    bool const create = guid.IsEmpty();
    if (!create)
    {
        if (sCharacterCache->GetCharacterAccountIdByGuid(guid) != accountId)
        {
            delete session;
            return Fail(handler, "Character '" + name + "' belongs to another account than '" + accountName + "'");
        }

        if (ObjectAccessor::FindConnectedPlayer(guid))
        {
            delete session;
            return Fail(handler, "Character '" + name + "' is already online");
        }
    }
    else
    {
        if (ObjectMgr::CheckPlayerName(name, true) != CHAR_NAME_SUCCESS)
        {
            delete session;
            return Fail(handler, "'" + name + "' is not a usable character name");
        }

        // Character creation as in mod-dungeon-clear's DcTestDriver::TryProvision
        CharacterCreateInfo createInfo(name, RACE_HUMAN, CLASS_WARRIOR, GENDER_MALE, 0, 0, 0, 0, 0);
        Player* player = new Player(session);
        player->GetMotionMaster()->Initialize();
        if (!player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &createInfo))
        {
            player->CleanupsBeforeDelete();
            delete player;
            delete session;
            return Fail(handler, "Could not create character '" + name + "'");
        }

        player->setCinematic(2);  // skip the intro movie on login
        player->SetAtLoginFlag(AT_LOGIN_NONE);
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        player->SaveToDB(trans, true, false);
        sCharacterCache->AddCharacterCacheEntry(player->GetGUID(), accountId, player->GetName(), player->getGender(),
                                                player->getRace(), player->getClass(), player->GetLevel());
        guid = player->GetGUID();
        player->CleanupsBeforeDelete();
        delete player;

        // The login loads the character from the database, so it waits for the commit
        session->AddTransactionCallback(CharacterDatabase.AsyncCommitTransaction(trans))
            .AfterComplete(
                [](bool success)
                {
                    TestPlayerMgr& mgr = Instance();
                    if (mgr._state != State::Creating)
                        return;

                    if (!success)
                    {
                        LOG_ERROR("playerbots", "TestPlayer: saving the new character {} failed", mgr._name);
                        mgr._failed = true;
                        return;
                    }

                    mgr.StartLoad();
                });
    }

    _session = session;
    _guid = guid;
    _name = name;
    _accountId = accountId;
    _routeName = route;
    _route = waypoints;
    _waypoint = 0;
    _attempts = 0;
    _pendingTimer = 0;
    _failed = false;
    _state = create ? State::Creating : State::Loading;
    if (!create)
        StartLoad();

    handler->PSendSysMessage("Test player {} ({}) {} on account {}{}", name, guid.ToString(),
                             create ? "is being created and logged in" : "is logging in", accountName,
                             route.empty() ? "" : ", route " + route);
    LOG_INFO("playerbots", "TestPlayer: login {} ({}) account {} route '{}'{}", name, guid.ToString(), accountName,
             route, create ? " (new character)" : "");
    return true;
}

void TestPlayerMgr::StartLoad()
{
    std::shared_ptr<LoginQueryHolder> holder = std::make_shared<LoginQueryHolder>(_accountId, _guid);
    if (!holder->Initialize())
    {
        LOG_ERROR("playerbots", "TestPlayer: could not prepare the login of {}", _name);
        _failed = true;
        return;
    }

    _state = State::Loading;
    _pendingTimer = 0;
    _session->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder))
        .AfterComplete([](SQLQueryHolderBase const& queryHolder)
                       { Instance().OnLoaded(static_cast<LoginQueryHolder const&>(queryHolder)); });
}

void TestPlayerMgr::OnLoaded(LoginQueryHolder const& holder)
{
    if (_state != State::Loading)
        return;

    _session->HandlePlayerLoginFromDB(holder);
    Player* player = _session->GetPlayer();
    if (!player)
    {
        LOG_ERROR("playerbots", "TestPlayer: login of {} failed", _name);
        _failed = true;
        return;
    }

    _state = State::Online;
    // PlayerbotsPlayerScript::OnPlayerLogin skips headless sessions; this puts the test player into the real-player
    // list that AllowActive and HasPlayerNearby read. No AddPlayerbotData: the player must stay without a PlayerbotAI.
    sRandomPlayerbotMgr.OnPlayerLogin(player);

    if (player->GetLevel() < TestPlayerLevel)
    {
        player->GiveLevel(TestPlayerLevel);
        player->InitTalentForLevel();
        player->SetUInt32Value(PLAYER_XP, 0);
    }

    LOG_INFO("playerbots", "TestPlayer: {} is online on map {} zone {} ({:.1f}, {:.1f}, {:.1f})", _name,
             player->GetMapId(), player->GetZoneId(), player->GetPositionX(), player->GetPositionY(),
             player->GetPositionZ());

    if (_route)
        StartRoute(player);
}

void TestPlayerMgr::Update(uint32 diff)
{
    if (_state == State::Idle)
        return;

    // Nothing else drives a headless session's async query, transaction and holder callbacks
    _session->ProcessQueryCallbacks();
    if (_failed)
    {
        Reset();
        return;
    }

    if (_state != State::Online)
    {
        _pendingTimer += diff;
        if (_pendingTimer >= PendingTimeoutMs)
        {
            LOG_ERROR("playerbots", "TestPlayer: the login of {} did not complete, giving up", _name);
            Reset();
        }
        return;
    }

    Player* player = _session->GetPlayer();
    if (!player)
    {
        LOG_ERROR("playerbots", "TestPlayer: {} lost its player object", _name);
        Reset();
        return;
    }

    if (HandleTeleport(player) || !player->IsInWorld())
        return;

    if (!player->IsAlive())
    {
        player->ResurrectPlayer(1.0f);
        player->SpawnCorpseBones();
    }

    if (_route)
        UpdateRoute(player, diff);
}

bool TestPlayerMgr::HandleTeleport(Player* player)
{
    // No client acknowledges a teleport, so the session does it, as PlayerbotAI::HandleTeleportAck does for bots
    if (player->IsBeingTeleportedFar())
    {
        _session->HandleMoveWorldportAck();
        return true;
    }

    if (!player->IsBeingTeleportedNear())
        return false;

    Player* mover = player->m_mover ? player->m_mover->ToPlayer() : nullptr;
    if (!player->IsInWorld() || !mover)
        return true;

    WorldPacket packet(MSG_MOVE_TELEPORT_ACK, 20);
    packet << mover->GetPackGUID();
    packet << uint32(0);  // flags
    packet << uint32(0);  // time
    _session->HandleMoveTeleportAck(packet);
    return true;
}

void TestPlayerMgr::StartRoute(Player* player)
{
    _waypoint = 0;
    _attempts = 0;
    _moveTimer = CheckIntervalMs;  // the next tick issues the first move
    player->GetMotionMaster()->Clear(true);
    player->StopMoving();

    TestPlayerWaypoint const& first = _route->front();
    if (player->GetMapId() != first.mapId || player->GetExactDist2d(first.x, first.y) > TeleportDistance)
        player->TeleportTo(first.mapId, first.x, first.y, first.z, player->GetOrientation());
}

void TestPlayerMgr::UpdateRoute(Player* player, uint32 diff)
{
    // O(1): one waypoint index, one distance check every CheckIntervalMs
    _moveTimer += diff;
    if (_moveTimer < CheckIntervalMs)
        return;

    _moveTimer = 0;
    MotionMaster* motion = player->GetMotionMaster();
    if (motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        return;  // still walking the leg

    TestPlayerWaypoint const* point = &(*_route)[_waypoint];
    if (player->GetMapId() != point->mapId)
    {
        StartRoute(player);
        return;
    }

    bool const arrived = player->GetExactDist2d(point->x, point->y) <= ArrivalRadius;
    if (arrived || ++_attempts > MaxAttemptsPerWaypoint)
    {
        if (!arrived)
            LOG_DEBUG("playerbots", "TestPlayer: {} skips unreachable waypoint {} of {}", _name, _waypoint, _routeName);

        _waypoint = (_waypoint + 1) % _route->size();
        _attempts = 1;
        point = &(*_route)[_waypoint];
    }

    motion->MovePoint(_waypoint, point->x, point->y, point->z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true);
}

bool TestPlayerMgr::SetRoute(ChatHandler* handler, std::string const& route)
{
    Player* player = _state == State::Online ? _session->GetPlayer() : nullptr;
    if (!player)
        return Fail(handler, "No test player is online");

    if (player->IsBeingTeleported())
        return Fail(handler, "The test player is being teleported; try again");

    if (route == "none")
    {
        _route = nullptr;
        _routeName.clear();
        player->GetMotionMaster()->Clear(true);
        player->StopMoving();
        handler->PSendSysMessage("Test player {} stopped", _name);
        return true;
    }

    std::vector<TestPlayerWaypoint> const* waypoints = FindRoute(route);
    if (!waypoints)
        return Fail(handler, "Unknown route '" + route + "' (routes: " + RouteNames() + ")");

    _route = waypoints;
    _routeName = route;
    StartRoute(player);
    handler->PSendSysMessage("Test player {} walks route {} ({} waypoints)", _name, route, waypoints->size());
    LOG_INFO("playerbots", "TestPlayer: {} walks route {}", _name, route);
    return true;
}

bool TestPlayerMgr::Logout(ChatHandler* handler)
{
    if (_state == State::Idle)
        return Fail(handler, "No test player is logged in");

    std::string const name = _name;
    Reset();
    handler->PSendSysMessage("Test player {} logged out", name);
    return true;
}

void TestPlayerMgr::Shutdown()
{
    if (_state != State::Idle)
        Reset();
}

void TestPlayerMgr::Reset()
{
    if (_session)
    {
        if (_session->GetPlayer())
        {
            // Fires OnPlayerBeforeLogout, which takes the player out of the real-player list again
            _session->LogoutPlayer(true);
            LOG_INFO("playerbots", "TestPlayer: {} logged out", _name);
        }

        delete _session;
    }

    _state = State::Idle;
    _failed = false;
    _session = nullptr;
    _guid.Clear();
    _name.clear();
    _accountId = 0;
    _pendingTimer = 0;
    _moveTimer = 0;
    _routeName.clear();
    _route = nullptr;
    _waypoint = 0;
    _attempts = 0;
}

void TestPlayerMgr::Status(ChatHandler* handler)
{
    if (_state == State::Idle)
    {
        handler->SendSysMessage("No test player is logged in");
        return;
    }

    Player* player = _state == State::Online ? _session->GetPlayer() : nullptr;
    if (!player)
    {
        handler->PSendSysMessage("Test player {} ({}) is {}", _name, _guid.ToString(),
                                 _state == State::Creating ? "being created" : "loading");
        return;
    }

    bool registered = false;
    for (Player* const realPlayer : sRandomPlayerbotMgr.GetPlayers())
    {
        if (realPlayer == player)
            registered = true;
    }

    // A console command, so a pass over the random bots is fine here
    float const radius = static_cast<float>(sPlayerbotAIConfig.BotActiveAloneForceWhenInRadius);
    uint32 inZone = 0;
    uint32 inRadius = 0;
    uint32 active = 0;
    uint32 total = 0;
    uint32 totalActive = 0;
    // Visibility per distance band (0-100, 100-150, 150-250, 250+ yards): bots at the player's client, and bots that
    // have the player at their client and see it within their own sight range (Playerbots.BotSightRangeCap)
    std::array<uint32, 4> playerSees = {};
    std::array<uint32, 4> seePlayer = {};
    uint32 playerSeesTotal = 0;
    float farthestSeen = 0.0f;
    Player const* sampleBot = nullptr;
    for (auto itr = sRandomPlayerbotMgr.GetPlayerBotsBegin(); itr != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++itr)
    {
        Player* const bot = itr->second;
        PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!botAI || !bot->IsInWorld())
            continue;

        bool const isActive = botAI->AllowActivity();
        ++total;
        totalActive += isActive ? 1 : 0;
        if (bot->GetMapId() != player->GetMapId())
            continue;

        float const dist = player->GetExactDist(bot);
        std::size_t const band = dist < 100.0f ? 0 : (dist < 150.0f ? 1 : (dist < 250.0f ? 2 : 3));
        if (player->HaveAtClient(bot))
        {
            ++playerSees[band];
            ++playerSeesTotal;
            farthestSeen = std::max(farthestSeen, dist);
        }
        if (bot->HaveAtClient(player) && bot->CanSeeOrDetect(player, false, true))
            ++seePlayer[band];
        if (!sampleBot)
            sampleBot = bot;

        bool const sameZone = bot->GetZoneId() == player->GetZoneId();
        bool const nearby = bot->IsWithinDistInMap(player, radius);
        if (!sameZone && !nearby)
            continue;

        inZone += sameZone ? 1 : 0;
        inRadius += nearby ? 1 : 0;
        active += isActive ? 1 : 0;
    }

    std::string const route =
        _route ? Acore::StringFormat("{} waypoint {}/{}", _routeName, _waypoint + 1, _route->size()) : "none";
    handler->PSendSysMessage(
        "Test player {} ({}) level {} map {} zone {} area {} pos {:.1f} {:.1f} {:.1f} moving {}", _name,
        _guid.ToString(), player->GetLevel(), player->GetMapId(), player->GetZoneId(), player->GetAreaId(),
        player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
        player->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE ? "yes" : "no");
    handler->PSendSysMessage("  route {}; real player {}; in real-player list {}; gm {}", route,
                             IsRealPlayer(player) ? "yes" : "no", registered ? "yes" : "no",
                             player->IsGameMaster() ? "on" : "off");
    handler->PSendSysMessage(
        "  random bots in zone {}, within {:.0f} yd {}, of these active {}; all random bots {}, active {}", inZone,
        radius, inRadius, active, total, totalActive);
    handler->PSendSysMessage(
        "  visibility: player sees {} bots (farthest {:.0f} yd; 0-100 {}, 100-150 {}, 150-250 {}, "
        "250+ {}); bots that see the player: 0-100 {}, 100-150 {}, 150-250 {}, 250+ {}",
        playerSeesTotal, farthestSeen, playerSees[0], playerSees[1], playerSees[2], playerSees[3], seePlayer[0],
        seePlayer[1], seePlayer[2], seePlayer[3]);
    std::string botSight = "none";
    if (sampleBot)
    {
        Optional<float> const cap = sampleBot->GetSightRangeCap();
        botSight = Acore::StringFormat("{} {:.0f} (cap {})", sampleBot->GetName(), sampleBot->GetSightRange(),
                                       cap ? Acore::StringFormat("{:.0f}", *cap) : std::string("off"));
    }
    handler->PSendSysMessage("  sight range: player {:.0f}, bot {}", player->GetSightRange(), botSight);
}
