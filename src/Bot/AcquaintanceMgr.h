/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ACQUAINTANCEMGR_H
#define _PLAYERBOT_ACQUAINTANCEMGR_H

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Define.h"

class ChatHandler;
class Group;
class Player;

// Which kinds of meeting a pair had at least once (the `flags` column)
namespace AcquaintanceFlags
{
constexpr uint32 Group = 1 << 0;
constexpr uint32 Whisper = 1 << 1;
constexpr uint32 PartyChat = 1 << 2;
constexpr uint32 Trade = 1 << 3;
constexpr uint32 Duel = 1 << 4;
constexpr uint32 Guild = 1 << 5;
constexpr uint32 PvpKill = 1 << 6;
constexpr uint32 SameBattleground = 1 << 7;
constexpr uint32 Nearby = 1 << 8;
}  // namespace AcquaintanceFlags

enum class AcquaintanceEvent : uint8
{
    Whisper,
    PartyChat,
    Trade,
    Duel,
    GroupJoin,
    GuildJoin,
    PvpKill
};

// Time a pair spends together, added by the world-thread samplers
enum class AcquaintanceTally : uint8
{
    Group,
    Guild,
    // Same battleground or arena, same team: counts as nearby time
    Battleground,
    Nearby
};

struct Acquaintance
{
    // Unix seconds
    uint32 FirstMet = 0;
    uint32 LastMet = 0;
    // Monotone scalar for readers that compare it with a threshold (computed from the tallies below)
    uint32 Score = 0;
    uint32 Flags = 0;
    uint32 GroupSeconds = 0;
    uint32 GuildSeconds = 0;
    uint32 NearbySeconds = 0;
    uint32 DirectEvents = 0;
    // Points of the one-off events (direct, group join, guild join, PvP kill)
    uint32 EventPoints = 0;
};

// Which random bots each real player has met, and how well (Playerbots.Acquaintance.Enable). Hooks on any thread only
// queue an event under a mutex; the world thread drains the queue in Update and owns every pair. All rows are loaded
// with one query at startup; changed rows are written in batches by asynchronous transactions. Only real players'
// characters count (no PlayerbotAI), and only random bots on the other side.
class AcquaintanceMgr
{
public:
    static AcquaintanceMgr& Instance()
    {
        static AcquaintanceMgr instance;
        return instance;
    }

    // World thread, startup (or the first tick after a config reload turned the feature on): one synchronous query
    // for all rows; does nothing when the feature is off
    void Load();
    // World thread, once per world tick: drains the events, runs the time samplers, writes dirty rows every
    // FlushSeconds; after a config reload turned the feature off it writes the dirty rows once
    void Update(uint32 diff);
    // World thread, server shutdown: drains the events and queues every dirty row behind the earlier flushes
    void Shutdown();

    // Any thread; one lock, no I/O. The world thread checks that bot is a random bot when it drains the event.
    void Record(AcquaintanceEvent kind, uint32 player, uint32 bot);
    // Any thread. A member joined the group; counts for the group leader when the leader is a real player.
    void RecordGroupJoin(uint32 group, uint32 member);
    // Any thread. A real player spoke in the group chat; counts for each random bot of the group.
    void RecordPartyChat(uint32 player, uint32 group);
    // Any thread. A member joined the guild; pairs it with the guild's random bots or its online real players.
    void RecordGuildJoin(uint32 guild, uint32 member);
    // Any thread; the dirty rows are written at the next world tick (a real player logged out)
    void RequestFlush() { _flushRequested.store(true, std::memory_order_relaxed); }

    // World thread (samplers): time the pair spent together
    void AddTime(AcquaintanceTally tally, uint32 player, uint32 bot, uint32 seconds);

    // Readers, world thread only
    Acquaintance const* Find(uint32 player, uint32 bot) const;
    uint32 GetScore(uint32 player, uint32 bot) const;
    // The player's acquaintances, highest score first, at most limit entries
    std::vector<std::pair<uint32, Acquaintance>> GetAcquaintances(uint32 player, size_t limit) const;

    // .playerbots acquaintances [<player>] | stats | record <player> <bot> <kind>
    static bool HandleCommand(ChatHandler* handler, char const* args);

private:
    enum class Source : uint8
    {
        Pair,
        Group,
        Guild
    };

    struct Event
    {
        AcquaintanceEvent Kind;
        Source From;
        uint32 Player;
        uint32 Bot;
        // Group or guild id for the expanding sources
        uint32 Context;
    };

    struct Entry
    {
        Acquaintance Data;
        // Cooldowns, in memory only
        uint32 LastDirect = 0;
        uint32 LastGroupJoin = 0;
        uint32 LastGuildJoin = 0;
        uint32 LastPvpKill = 0;
        bool Dirty = false;
    };

    AcquaintanceMgr() = default;

    static uint64 Key(uint32 player, uint32 bot) { return (static_cast<uint64>(player) << 32) | bot; }

    void Push(Event const& event);
    void Drain();
    void Expand(Event const& event, uint32 now);
    void Apply(AcquaintanceEvent kind, uint32 player, uint32 bot, uint32 now);
    Entry& Touch(uint32 player, uint32 bot, uint32 now);
    void MarkDirty(uint64 key, Entry& entry);
    static void Rescore(Acquaintance& data);
    void Flush();

    void Sample(uint32 diff);
    void SampleGroup(Player* player, Group* group, AcquaintanceTally tally, uint32 seconds);
    void SampleGuild(Player* player, uint32 seconds);
    void SampleNearby(Player* player, uint32 seconds);
    // Whole seconds that elapsed reached the interval: returns them and keeps the remainder; 0 otherwise
    static uint32 TakeSeconds(uint32& elapsed, uint32 diff, uint32 intervalSeconds);

    // Shared with the hooks
    std::mutex _queueMutex;
    std::vector<Event> _queue;
    std::atomic<bool> _flushRequested{false};

    // World thread only
    std::unordered_map<uint64, Entry> _pairs;
    std::unordered_map<uint32, std::vector<uint32>> _byPlayer;
    std::vector<uint64> _dirty;
    std::vector<Event> _draining;
    uint32 _flushElapsed = 0;
    uint32 _groupElapsed = 0;
    uint32 _guildElapsed = 0;
    uint32 _nearbyElapsed = 0;
    // Scratch lists of one sampler pass, empty between ticks (no Player* is kept)
    std::vector<Player*> _realPlayers;
    std::vector<Player*> _nearby;
    uint64 _eventsDrained = 0;
    uint64 _rowsWritten = 0;
    uint64 _samples = 0;
    uint32 _lastSampleMicroseconds = 0;
    uint32 _lastSamplePlayers = 0;
    bool _loaded = false;
    // The feature was on at the last tick (a config reload may turn it off)
    bool _active = false;
};

#endif
