/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcquaintanceMgr.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>

#include "CellImpl.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "GroupMgr.h"
#include "GroupScript.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "GuildScript.h"
#include "LadderMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "WorldSession.h"

namespace
{
constexpr uint32 MS_PER_SECOND = 1000;
constexpr uint32 SECONDS_PER_MINUTE = 60;
constexpr uint32 SECONDS_PER_HOUR = 3600;
constexpr size_t LIST_LIMIT = 50;
// Columns of PLAYERBOTS_INS_ACQUAINTANCE before the values that the ON DUPLICATE KEY UPDATE part repeats
constexpr uint8 INSERT_ONLY_COLUMNS = 3;

constexpr std::array<std::pair<AcquaintanceEvent, char const*>, 7> KIND_NAMES = {{
    {AcquaintanceEvent::Whisper, "whisper"},
    {AcquaintanceEvent::PartyChat, "partychat"},
    {AcquaintanceEvent::Trade, "trade"},
    {AcquaintanceEvent::Duel, "duel"},
    {AcquaintanceEvent::GroupJoin, "group"},
    {AcquaintanceEvent::GuildJoin, "guild"},
    {AcquaintanceEvent::PvpKill, "pvpkill"},
}};

constexpr std::array<std::pair<uint32, char const*>, 9> FLAG_NAMES = {{
    {AcquaintanceFlags::Group, "group"},
    {AcquaintanceFlags::Whisper, "whisper"},
    {AcquaintanceFlags::PartyChat, "partychat"},
    {AcquaintanceFlags::Trade, "trade"},
    {AcquaintanceFlags::Duel, "duel"},
    {AcquaintanceFlags::Guild, "guild"},
    {AcquaintanceFlags::PvpKill, "pvpkill"},
    {AcquaintanceFlags::SameBattleground, "battleground"},
    {AcquaintanceFlags::Nearby, "nearby"},
}};

constexpr std::array<char const*, 12> CLASS_NAMES = {"?",           "Warrior", "Paladin", "Hunter",  "Rogue", "Priest",
                                                     "DeathKnight", "Shaman",  "Mage",    "Warlock", "?",     "Druid"};

uint32 Now() { return static_cast<uint32>(GameTime::GetGameTime().count()); }

std::optional<AcquaintanceEvent> ParseKind(std::string const& word)
{
    for (auto const& [kind, name] : KIND_NAMES)
        if (word == name)
            return kind;

    return std::nullopt;
}

std::string KindList()
{
    std::string text;
    for (auto const& [kind, name] : KIND_NAMES)
        text += Acore::StringFormat("{}{}", text.empty() ? "" : "|", name);

    return text;
}

std::string FlagsText(uint32 flags)
{
    std::string text;
    for (auto const& [flag, name] : FLAG_NAMES)
        if (flags & flag)
            text += Acore::StringFormat("{}{}", text.empty() ? "" : ",", name);

    return text.empty() ? "-" : text;
}

uint32 FlagOf(AcquaintanceEvent kind)
{
    switch (kind)
    {
        case AcquaintanceEvent::Whisper:
            return AcquaintanceFlags::Whisper;
        case AcquaintanceEvent::PartyChat:
            return AcquaintanceFlags::PartyChat;
        case AcquaintanceEvent::Trade:
            return AcquaintanceFlags::Trade;
        case AcquaintanceEvent::Duel:
            return AcquaintanceFlags::Duel;
        case AcquaintanceEvent::GroupJoin:
            return AcquaintanceFlags::Group;
        case AcquaintanceEvent::GuildJoin:
            return AcquaintanceFlags::Guild;
        case AcquaintanceEvent::PvpKill:
            return AcquaintanceFlags::PvpKill;
    }

    return 0;
}

// Name or guid of any character; empty when there is none
ObjectGuid ResolveCharacter(std::string const& target, std::string& name)
{
    ObjectGuid guid = ObjectGuid::Empty;
    name = target;
    if (std::optional<uint32> const number = Acore::StringTo<uint32>(target))
        guid = ObjectGuid::Create<HighGuid::Player>(*number);
    else if (normalizePlayerName(name))
        guid = sCharacterCache->GetCharacterGuidByName(name);

    if (guid.IsEmpty() || !sCharacterCache->GetCharacterNameByGuid(guid, name))
        return ObjectGuid::Empty;

    return guid;
}

// Offline characters cannot be told apart from their owner's alt bots, so only online real players count
bool IsOnlineRealPlayer(uint32 low)
{
    return IsRealPlayer(ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(low)));
}

void SaturatingAdd(uint32& value, uint64 amount, uint64 cap)
{
    value = static_cast<uint32>(std::min<uint64>(static_cast<uint64>(value) + amount, cap));
}

// DE.3: a group the player formed is the strongest kind; a battleground team and a dungeon finder group, which the
// player did not choose, only count as the weak closeness
AcquaintanceTally TallyOf(Group const* group)
{
    if (group->isBGGroup() || group->isBFGroup())
        return AcquaintanceTally::Battleground;

    return group->isLFGGroup() ? AcquaintanceTally::Nearby : AcquaintanceTally::Group;
}

// The hooks run on map threads or the world thread: they only test the option and the two sides, then queue
class AcquaintancePlayerScript : public PlayerScript
{
public:
    AcquaintancePlayerScript()
        : PlayerScript(
              "AcquaintancePlayerScript",
              {PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT, PLAYERHOOK_CAN_INIT_TRADE,
               PLAYERHOOK_ON_DUEL_REQUEST, PLAYERHOOK_ON_DUEL_START, PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_LOGOUT})
    {
    }

    using PlayerScript::OnPlayerCanUseChat;

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& /*msg*/, Player* receiver) override
    {
        if (sPlayerbotAIConfig.AcquaintanceEnable && type == CHAT_MSG_WHISPER && lang != LANG_ADDON &&
            IsRealPlayer(player) && receiver && !IsRealPlayer(receiver))
            AcquaintanceMgr::Instance().Record(AcquaintanceEvent::Whisper, player->GetGUID().GetCounter(),
                                               receiver->GetGUID().GetCounter());

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& /*msg*/, Group* group) override
    {
        if (sPlayerbotAIConfig.AcquaintanceEnable && lang != LANG_ADDON && group && IsRealPlayer(player))
            AcquaintanceMgr::Instance().RecordPartyChat(player->GetGUID().GetCounter(), group->GetGUID().GetCounter());

        return true;
    }

    bool OnPlayerCanInitTrade(Player* player, Player* target) override
    {
        if (sPlayerbotAIConfig.AcquaintanceEnable)
            RecordPair(AcquaintanceEvent::Trade, player, target);

        return true;
    }

    void OnPlayerDuelRequest(Player* target, Player* challenger) override
    {
        if (sPlayerbotAIConfig.AcquaintanceEnable)
            RecordPair(AcquaintanceEvent::Duel, target, challenger);
    }

    void OnPlayerDuelStart(Player* player1, Player* player2) override
    {
        if (sPlayerbotAIConfig.AcquaintanceEnable)
            RecordPair(AcquaintanceEvent::Duel, player1, player2);
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (sPlayerbotAIConfig.AcquaintanceEnable)
            RecordPair(AcquaintanceEvent::PvpKill, killer, killed);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (sPlayerbotAIConfig.AcquaintanceEnable && IsRealPlayer(player))
            AcquaintanceMgr::Instance().RequestFlush();
    }

private:
    // One side a real player, the other a bot (the drain keeps random bots only)
    static void RecordPair(AcquaintanceEvent kind, Player* first, Player* second)
    {
        if (!first || !second)
            return;

        bool const firstReal = IsRealPlayer(first);
        if (firstReal == IsRealPlayer(second))
            return;

        Player* player = firstReal ? first : second;
        Player* bot = firstReal ? second : first;
        AcquaintanceMgr::Instance().Record(kind, player->GetGUID().GetCounter(), bot->GetGUID().GetCounter());
    }
};

class AcquaintanceGroupScript : public GroupScript
{
public:
    AcquaintanceGroupScript() : GroupScript("AcquaintanceGroupScript", {GROUPHOOK_ON_ADD_MEMBER}) {}

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (sPlayerbotAIConfig.AcquaintanceEnable && group)
            AcquaintanceMgr::Instance().RecordGroupJoin(group->GetGUID().GetCounter(), guid.GetCounter());
    }
};

class AcquaintanceGuildScript : public GuildScript
{
public:
    AcquaintanceGuildScript() : GuildScript("AcquaintanceGuildScript", {GUILDHOOK_ON_ADD_MEMBER}) {}

    void OnAddMember(Guild* guild, Player* player, uint8& /*plRank*/) override
    {
        if (sPlayerbotAIConfig.AcquaintanceEnable && guild && player)
            AcquaintanceMgr::Instance().RecordGuildJoin(guild->GetId(), player->GetGUID().GetCounter());
    }
};
}  // namespace

void AcquaintanceMgr::Load()
{
    // Runs in OnBeforeWorldInitialized, before the world thread is known to the thread checks, or in Update after a
    // config reload turned the feature on
    if (!sPlayerbotAIConfig.AcquaintanceEnable || _loaded)
        return;

    _loaded = true;
    uint32 const start = getMSTime();
    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_ACQUAINTANCE_ALL);
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const player = fields[0].Get<uint32>();
            uint32 const bot = fields[1].Get<uint32>();
            uint64 const key = Key(player, bot);
            Entry& entry = _pairs[key];
            Acquaintance& data = entry.Data;
            data.FirstMet = fields[2].Get<uint32>();
            data.LastMet = fields[3].Get<uint32>();
            data.Score = fields[4].Get<uint32>();
            data.Flags = fields[5].Get<uint32>();
            data.GroupSeconds = fields[6].Get<uint32>();
            data.GuildSeconds = fields[7].Get<uint32>();
            data.NearbySeconds = fields[8].Get<uint32>();
            data.DirectEvents = fields[9].Get<uint32>();
            data.EventPoints = fields[10].Get<uint32>();
            _byPlayer[player].push_back(bot);

            // Retuned weights: the stored score follows at the next flush
            uint32 const stored = data.Score;
            Rescore(data);
            if (data.Score != stored)
                MarkDirty(key, entry);
        } while (result->NextRow());
    }

    LOG_INFO("server.loading", ">> Loaded {} acquaintances of {} players in {} ms", _pairs.size(), _byPlayer.size(),
             GetMSTimeDiffToNow(start));
}

void AcquaintanceMgr::Update(uint32 diff)
{
    bool const enabled = sPlayerbotAIConfig.AcquaintanceEnable;
    if (!enabled && !_active)
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("AcquaintanceMgr::Update");
    if (!enabled)
    {
        // A config reload turned the feature off: write what was counted once; the pairs stay in memory
        _active = false;
        Drain();
        Flush();
        return;
    }

    // Nothing is counted or written before the stored rows are in memory: an empty pair would overwrite its row
    Load();
    _active = true;
    Drain();
    Sample(diff);

    _flushElapsed += diff;
    bool const requested =
        _flushRequested.load(std::memory_order_relaxed) && _flushRequested.exchange(false, std::memory_order_relaxed);
    if (requested || _flushElapsed >= sPlayerbotAIConfig.AcquaintanceFlushSeconds * MS_PER_SECOND)
    {
        _flushElapsed = 0;
        Flush();
    }
}

void AcquaintanceMgr::Shutdown()
{
    if (!_loaded)
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("AcquaintanceMgr::Shutdown");
    Drain();
    // Asynchronous like every flush, so it runs after the earlier ones (one worker by default,
    // PlayerbotsDatabase.WorkerThreads; the upsert never lowers a tally either); closing the pool waits for the queue
    Flush();
    LOG_INFO("playerbots", "Acquaintances: {} pairs, {} rows written since startup", _pairs.size(), _rowsWritten);
}

void AcquaintanceMgr::Record(AcquaintanceEvent kind, uint32 player, uint32 bot)
{
    Push({kind, Source::Pair, player, bot, 0});
}

void AcquaintanceMgr::RecordGroupJoin(uint32 group, uint32 member)
{
    Push({AcquaintanceEvent::GroupJoin, Source::Group, 0, member, group});
}

void AcquaintanceMgr::RecordPartyChat(uint32 player, uint32 group)
{
    Push({AcquaintanceEvent::PartyChat, Source::Group, player, 0, group});
}

void AcquaintanceMgr::RecordGuildJoin(uint32 guild, uint32 member)
{
    Push({AcquaintanceEvent::GuildJoin, Source::Guild, member, 0, guild});
}

void AcquaintanceMgr::Push(Event const& event)
{
    std::lock_guard<std::mutex> lock(_queueMutex);
    _queue.push_back(event);
}

void AcquaintanceMgr::Drain()
{
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (_queue.empty())
            return;

        _draining.swap(_queue);
    }

    uint32 const now = Now();
    for (Event const& event : _draining)
        Expand(event, now);

    _eventsDrained += _draining.size();
    _draining.clear();
}

// World thread: the groups, guilds and the random bot list are only read here, while no map update runs
void AcquaintanceMgr::Expand(Event const& event, uint32 now)
{
    switch (event.From)
    {
        case Source::Pair:
            if (sRandomPlayerbotMgr.IsRandomBot(event.Bot))
                Apply(event.Kind, event.Player, event.Bot, now);
            break;
        case Source::Group:
        {
            Group const* group = sGroupMgr->GetGroupByGUID(event.Context);
            if (!group || group->isBGGroup() || group->isBFGroup())
                break;

            if (event.Kind == AcquaintanceEvent::GroupJoin)
            {
                // Explicitly invited: the real player leads the group (dungeon finder groups are formed for them)
                uint32 const leader = group->GetLeaderGUID().GetCounter();
                if (!group->isLFGGroup() && leader != event.Bot && IsOnlineRealPlayer(leader) &&
                    sRandomPlayerbotMgr.IsRandomBot(event.Bot))
                    Apply(event.Kind, leader, event.Bot, now);
                break;
            }

            for (Group::MemberSlot const& slot : group->GetMemberSlots())
            {
                uint32 const member = slot.guid.GetCounter();
                if (member != event.Player && sRandomPlayerbotMgr.IsRandomBot(member))
                    Apply(event.Kind, event.Player, member, now);
            }
            break;
        }
        case Source::Guild:
        {
            Guild const* guild = sGuildMgr->GetGuildById(event.Context);
            if (!guild)
                break;

            bool const joinerIsBot = sRandomPlayerbotMgr.IsRandomBot(event.Player);
            if (!joinerIsBot && !IsOnlineRealPlayer(event.Player))
                break;

            for (auto const& pair : guild->GetUnorderedMembers())
            {
                uint32 const member = pair.first;
                if (member == event.Player)
                    continue;

                if (!joinerIsBot)
                {
                    if (sRandomPlayerbotMgr.IsRandomBot(member))
                        Apply(event.Kind, event.Player, member, now);
                }
                else if (!sRandomPlayerbotMgr.IsRandomBot(member) && IsOnlineRealPlayer(member))
                    Apply(event.Kind, member, event.Player, now);
            }
            break;
        }
    }
}

// DE.3: direct chat and interactions and an explicitly invited group are the strongest, the same guild the second;
// cooldowns keep repeated events from inflating a pair
void AcquaintanceMgr::Apply(AcquaintanceEvent kind, uint32 player, uint32 bot, uint32 now)
{
    Entry& entry = Touch(player, bot, now);
    Acquaintance& data = entry.Data;
    data.Flags |= FlagOf(kind);

    uint32* last = &entry.LastDirect;
    uint32 cooldown = sPlayerbotAIConfig.AcquaintanceDirectCooldownSeconds;
    uint32 points = sPlayerbotAIConfig.AcquaintancePointsDirect;
    bool direct = false;
    switch (kind)
    {
        case AcquaintanceEvent::Whisper:
        case AcquaintanceEvent::PartyChat:
        case AcquaintanceEvent::Trade:
        case AcquaintanceEvent::Duel:
            direct = true;
            break;
        case AcquaintanceEvent::GroupJoin:
            last = &entry.LastGroupJoin;
            cooldown = sPlayerbotAIConfig.AcquaintanceGroupJoinCooldownSeconds;
            points = sPlayerbotAIConfig.AcquaintancePointsGroupJoin;
            break;
        case AcquaintanceEvent::GuildJoin:
            last = &entry.LastGuildJoin;
            cooldown = sPlayerbotAIConfig.AcquaintanceGroupJoinCooldownSeconds;
            points = sPlayerbotAIConfig.AcquaintancePointsGuildJoin;
            break;
        case AcquaintanceEvent::PvpKill:
            last = &entry.LastPvpKill;
            points = sPlayerbotAIConfig.AcquaintancePointsPvpKill;
            break;
    }

    if (!*last || now - *last >= cooldown)
    {
        *last = now;
        SaturatingAdd(data.EventPoints, points, sPlayerbotAIConfig.AcquaintanceMaxScore);
        if (direct)
            SaturatingAdd(data.DirectEvents, 1, std::numeric_limits<uint32>::max());
    }

    Rescore(data);
    MarkDirty(Key(player, bot), entry);

    // Playerbots.Ladder.ContactMinutes: a direct contact or an invited group pins the bot's arena team
    if (direct || kind == AcquaintanceEvent::GroupJoin)
        LadderMgr::Instance().NoteContact(bot, now);
}

void AcquaintanceMgr::AddTime(AcquaintanceTally tally, uint32 player, uint32 bot, uint32 seconds)
{
    if (!sPlayerbotAIConfig.AcquaintanceEnable || !_loaded || !seconds)
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("AcquaintanceMgr::AddTime");
    Entry& entry = Touch(player, bot, Now());
    Acquaintance& data = entry.Data;
    uint64 const cap = std::numeric_limits<uint32>::max();
    switch (tally)
    {
        case AcquaintanceTally::Group:
            data.Flags |= AcquaintanceFlags::Group;
            SaturatingAdd(data.GroupSeconds, seconds, cap);
            break;
        case AcquaintanceTally::Guild:
            data.Flags |= AcquaintanceFlags::Guild;
            SaturatingAdd(data.GuildSeconds, seconds, cap);
            break;
        case AcquaintanceTally::Battleground:
            data.Flags |= AcquaintanceFlags::SameBattleground;
            SaturatingAdd(data.NearbySeconds, seconds, cap);
            break;
        case AcquaintanceTally::Nearby:
            data.Flags |= AcquaintanceFlags::Nearby;
            SaturatingAdd(data.NearbySeconds, seconds, cap);
            break;
    }

    Rescore(data);
    MarkDirty(Key(player, bot), entry);
}

AcquaintanceMgr::Entry& AcquaintanceMgr::Touch(uint32 player, uint32 bot, uint32 now)
{
    auto const [it, inserted] = _pairs.try_emplace(Key(player, bot));
    if (inserted)
    {
        it->second.Data.FirstMet = now;
        _byPlayer[player].push_back(bot);
    }

    it->second.Data.LastMet = now;
    return it->second;
}

void AcquaintanceMgr::MarkDirty(uint64 key, Entry& entry)
{
    if (entry.Dirty)
        return;

    entry.Dirty = true;
    _dirty.push_back(key);
}

// The score only depends on the tallies, so it never falls while the weights stay
void AcquaintanceMgr::Rescore(Acquaintance& data)
{
    uint64 const score =
        static_cast<uint64>(data.EventPoints) +
        static_cast<uint64>(data.GroupSeconds / SECONDS_PER_MINUTE) *
            sPlayerbotAIConfig.AcquaintancePointsPerGroupMinute +
        static_cast<uint64>(data.GuildSeconds / SECONDS_PER_HOUR) * sPlayerbotAIConfig.AcquaintancePointsPerGuildHour +
        static_cast<uint64>(data.NearbySeconds / SECONDS_PER_MINUTE) *
            sPlayerbotAIConfig.AcquaintancePointsPerNearbyMinute;
    data.Score = static_cast<uint32>(std::min<uint64>(score, sPlayerbotAIConfig.AcquaintanceMaxScore));
}

uint32 AcquaintanceMgr::TakeSeconds(uint32& elapsed, uint32 diff, uint32 intervalSeconds)
{
    if (!intervalSeconds)
        return 0;

    elapsed += diff;
    if (elapsed < intervalSeconds * MS_PER_SECOND)
        return 0;

    uint32 const seconds = elapsed / MS_PER_SECOND;
    elapsed -= seconds * MS_PER_SECOND;
    return seconds;
}

// World thread, while no map update runs: players, groups, guilds and grids are only read here. The passes cover the
// online real players only, each bounded by its group, guild or a radius search; no pass scans the bots.
void AcquaintanceMgr::Sample(uint32 diff)
{
    uint32 const groupSeconds = TakeSeconds(_groupElapsed, diff, sPlayerbotAIConfig.AcquaintanceGroupSampleSeconds);
    uint32 const guildSeconds = TakeSeconds(_guildElapsed, diff, sPlayerbotAIConfig.AcquaintanceGuildSampleSeconds);
    uint32 const nearbySeconds =
        TakeSeconds(_nearbyElapsed, diff, sPlayerbotAIConfig.AcquaintanceNearbyIntervalSeconds);
    if (!groupSeconds && !guildSeconds && !nearbySeconds)
        return;

    PLAYERBOT_CHECK_WORLD_THREAD("AcquaintanceMgr::Sample");
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "Acquaintance");
    auto const start = std::chrono::steady_clock::now();
    {
        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* player = pair.second;
            if (player && player->IsInWorld() && IsRealPlayer(player))
                _realPlayers.push_back(player);
        }
    }

    for (Player* player : _realPlayers)
    {
        if (groupSeconds)
        {
            Group* group = player->GetGroup();
            if (group)
                SampleGroup(player, group, TallyOf(group), groupSeconds);

            // In a battleground the player's own group is kept as the original group
            Group* original = player->GetOriginalGroup();
            if (original && original != group)
                SampleGroup(player, original, TallyOf(original), groupSeconds);
        }

        if (guildSeconds)
            SampleGuild(player, guildSeconds);

        // In a battleground only the own team counts (the group pass)
        if (nearbySeconds && !player->InBattleground())
            SampleNearby(player, nearbySeconds);
    }

    _lastSamplePlayers = static_cast<uint32>(_realPlayers.size());
    _realPlayers.clear();
    ++_samples;
    _lastSampleMicroseconds = static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    if (pmo)
        pmo->finish();
}

void AcquaintanceMgr::SampleGroup(Player* player, Group* group, AcquaintanceTally tally, uint32 seconds)
{
    uint32 const low = player->GetGUID().GetCounter();
    for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
    {
        Player* member = reference->GetSource();
        if (member && member != player && sRandomPlayerbotMgr.IsRandomBot(member))
            AddTime(tally, low, member->GetGUID().GetCounter(), seconds);
    }
}

void AcquaintanceMgr::SampleGuild(Player* player, uint32 seconds)
{
    uint32 const guildId = player->GetGuildId();
    if (!guildId)
        return;

    Guild const* guild = sGuildMgr->GetGuildById(guildId);
    if (!guild)
        return;

    uint32 const low = player->GetGUID().GetCounter();
    for (auto const& pair : guild->GetUnorderedMembers())
    {
        uint32 const member = pair.first;
        if (member == low || !sRandomPlayerbotMgr.IsRandomBot(member))
            continue;

        Player const* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(member));
        if (bot && bot->IsInWorld())
            AddTime(AcquaintanceTally::Guild, low, member, seconds);
    }
}

void AcquaintanceMgr::SampleNearby(Player* player, uint32 seconds)
{
    float const radius = sPlayerbotAIConfig.AcquaintanceNearbyRadius;
    Acore::AnyPlayerInObjectRangeCheck check(player, radius, false);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(player, _nearby, check);
    Cell::VisitObjects(player, searcher, radius);

    uint32 const low = player->GetGUID().GetCounter();
    Group const* group = player->GetGroup();
    for (Player* other : _nearby)
    {
        // Group members get the stronger group time instead
        if (other == player || (group && other->GetGroup() == group) || !sRandomPlayerbotMgr.IsRandomBot(other))
            continue;

        AddTime(AcquaintanceTally::Nearby, low, other->GetGUID().GetCounter(), seconds);
    }

    _nearby.clear();
}

void AcquaintanceMgr::Flush()
{
    if (!_loaded || _dirty.empty())
        return;

    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    for (uint64 const key : _dirty)
    {
        auto const it = _pairs.find(key);
        if (it == _pairs.end())
            continue;

        it->second.Dirty = false;
        Acquaintance const& data = it->second.Data;
        std::array<uint32, 8> const values = {data.LastMet,      data.Score,        data.Flags,
                                              data.GroupSeconds, data.GuildSeconds, data.NearbySeconds,
                                              data.DirectEvents, data.EventPoints};

        PlayerbotsDatabasePreparedStatement* stmt =
            PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_ACQUAINTANCE);
        stmt->SetData(0, static_cast<uint32>(key >> 32));
        stmt->SetData(1, static_cast<uint32>(key));
        stmt->SetData(2, data.FirstMet);
        for (uint8 i = 0; i < values.size(); ++i)
        {
            stmt->SetData(static_cast<uint8>(INSERT_ONLY_COLUMNS + i), values[i]);
            stmt->SetData(static_cast<uint8>(INSERT_ONLY_COLUMNS + values.size() + i), values[i]);
        }

        transaction->Append(stmt);
    }

    _rowsWritten += _dirty.size();
    _dirty.clear();
    PlayerbotsDatabase.CommitTransaction(transaction);
}

Acquaintance const* AcquaintanceMgr::Find(uint32 player, uint32 bot) const
{
    PLAYERBOT_CHECK_WORLD_THREAD("AcquaintanceMgr::Find");
    auto const it = _pairs.find(Key(player, bot));
    return it == _pairs.end() ? nullptr : &it->second.Data;
}

uint32 AcquaintanceMgr::GetScore(uint32 player, uint32 bot) const
{
    Acquaintance const* data = Find(player, bot);
    return data ? data->Score : 0;
}

std::vector<std::pair<uint32, Acquaintance>> AcquaintanceMgr::GetAcquaintances(uint32 player, size_t limit) const
{
    PLAYERBOT_CHECK_WORLD_THREAD("AcquaintanceMgr::GetAcquaintances");
    std::vector<std::pair<uint32, Acquaintance>> list;
    auto const it = _byPlayer.find(player);
    if (it == _byPlayer.end())
        return list;

    list.reserve(it->second.size());
    for (uint32 const bot : it->second)
        if (Acquaintance const* data = Find(player, bot))
            list.emplace_back(bot, *data);

    auto const better = [](std::pair<uint32, Acquaintance> const& a, std::pair<uint32, Acquaintance> const& b)
    {
        return a.second.Score != b.second.Score ? a.second.Score > b.second.Score : a.second.LastMet > b.second.LastMet;
    };
    size_t const count = std::min(limit, list.size());
    std::partial_sort(list.begin(), list.begin() + count, list.end(), better);
    list.resize(count);
    return list;
}

bool AcquaintanceMgr::HandleCommand(ChatHandler* handler, char const* args)
{
    if (!sPlayerbotAIConfig.AcquaintanceEnable)
    {
        handler->PSendSysMessage("Acquaintances are off (Playerbots.Acquaintance.Enable = 0).");
        return true;
    }

    PLAYERBOT_CHECK_WORLD_THREAD("AcquaintanceMgr::HandleCommand");
    AcquaintanceMgr& mgr = Instance();
    std::istringstream in(args ? args : "");
    std::string first;
    in >> first;

    if (first == "stats")
    {
        size_t queued = 0;
        {
            std::lock_guard<std::mutex> lock(mgr._queueMutex);
            queued = mgr._queue.size();
        }

        handler->PSendSysMessage(
            "Acquaintances: {} pairs of {} players, {} dirty, {} queued, {} events drained, {} "
            "rows written; {} sampler passes, the last {} us over {} real players",
            mgr._pairs.size(), mgr._byPlayer.size(), mgr._dirty.size(), queued, mgr._eventsDrained, mgr._rowsWritten,
            mgr._samples, mgr._lastSampleMicroseconds, mgr._lastSamplePlayers);
        return true;
    }

    if (first == "record")
    {
        std::string playerTarget;
        std::string botTarget;
        std::string kindName;
        in >> playerTarget >> botTarget >> kindName;
        std::optional<AcquaintanceEvent> const kind = ParseKind(kindName);
        if (botTarget.empty() || !kind)
        {
            handler->PSendSysMessage("Usage: .playerbots acquaintances record <player> <bot> <{}>", KindList());
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string playerName;
        std::string botName;
        ObjectGuid const player = ResolveCharacter(playerTarget, playerName);
        ObjectGuid const bot = ResolveCharacter(botTarget, botName);
        if (player.IsEmpty() || bot.IsEmpty())
        {
            handler->PSendSysMessage("No character {}.", player.IsEmpty() ? playerTarget : botTarget);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* online = ObjectAccessor::FindConnectedPlayer(player);
        if ((online && !IsRealPlayer(online)) || sRandomPlayerbotMgr.IsRandomBot(player.GetCounter()))
        {
            handler->PSendSysMessage("{} is a bot, not a real player.", playerName);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!sRandomPlayerbotMgr.IsRandomBot(bot.GetCounter()))
        {
            handler->PSendSysMessage("{} is not a random bot.", botName);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // The same path as the hooks: queued now, counted at the next world tick
        mgr.Record(*kind, player.GetCounter(), bot.GetCounter());
        handler->PSendSysMessage("Recorded {} between {} and {}.", kindName, playerName, botName);
        return true;
    }

    ObjectGuid guid = ObjectGuid::Empty;
    std::string name;
    if (first.empty())
    {
        Player* self = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!self)
        {
            handler->PSendSysMessage("Usage: .playerbots acquaintances [<player>] | stats | record <player> <bot> <{}>",
                                     KindList());
            return true;
        }

        guid = self->GetGUID();
        name = self->GetName();
    }
    else
    {
        guid = ResolveCharacter(first, name);
        if (guid.IsEmpty())
        {
            handler->PSendSysMessage("No character {}.", first);
            handler->SetSentErrorMessage(true);
            return false;
        }
    }

    auto const known = mgr._byPlayer.find(guid.GetCounter());
    size_t const total = known == mgr._byPlayer.end() ? 0 : known->second.size();
    std::vector<std::pair<uint32, Acquaintance>> const list = mgr.GetAcquaintances(guid.GetCounter(), LIST_LIMIT);
    handler->PSendSysMessage("{} has met {} random bots{}", name, total,
                             total > list.size() ? Acore::StringFormat(", the best {}:", list.size()) : ":");
    for (auto const& [bot, data] : list)
    {
        CharacterCacheEntry const* cache =
            sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(bot));
        std::string const botName = cache ? cache->Name : "?";
        uint32 const level = cache ? cache->Level : 0;
        char const* className = cache && cache->Class < CLASS_NAMES.size() ? CLASS_NAMES[cache->Class] : "?";
        handler->PSendSysMessage(
            "  {} (guid {}, level {} {}): score {}, {}; met {} to {}; group {} s, guild {} s, "
            "nearby {} s, direct {}",
            botName, bot, level, className, data.Score, FlagsText(data.Flags),
            Acore::Time::TimeToTimestampStr(Seconds(data.FirstMet)),
            Acore::Time::TimeToTimestampStr(Seconds(data.LastMet)), data.GroupSeconds, data.GuildSeconds,
            data.NearbySeconds, data.DirectEvents);
    }

    return true;
}

void AddSC_acquaintance()
{
    new AcquaintancePlayerScript();
    new AcquaintanceGroupScript();
    new AcquaintanceGuildScript();
}
