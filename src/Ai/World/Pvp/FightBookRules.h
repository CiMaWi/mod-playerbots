/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_FIGHTBOOKRULES_H
#define _PLAYERBOT_FIGHTBOOKRULES_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "WorldPvpConfig.h"

// The fight book of world PvP (WP1): one map's open-world PvP fights, its recently ended fights and (WP7) its recent
// resurrections, and a bot's fight memory. Player-free: the glue (FightBook.cpp) turns players into Combatants and
// owns the threads; tests/fight-book-selftest.cpp proves the rules. A book is read and changed only on its map's
// update thread, a memory only on its bot's.
namespace WorldPvp
{
enum class Side : uint8_t
{
    Alliance = 0,
    Horde = 1
};

constexpr size_t SIDE_COUNT = 2;

// How a participant came into its fight (FightJoin's source word): the starter, the starter's first target, a player
// that attacked a participant, a player a participant attacked; group, master and bridge come with WP5/WP12
enum class CameIn : uint8_t
{
    Started,
    Defended,
    Joined,
    Targeted,
    Group,
    Master,
    Bridge
};

constexpr std::array<char const*, 7> CAME_IN_WORDS = {"started", "defended", "joined", "targeted",
                                                      "group",   "master",   "bridge"};

// Why a fight started (FightStart's source word). WP1 records attack_on_sight (a bot started it: today's rules),
// real_player (a real player started it) and pvptest (a forced PvpTest fighter took part); the others are reserved
enum class FightReason : uint8_t
{
    AttackOnSight,
    RealPlayer,
    PvpTest,
    Defence,
    Budget,
    Grudge,
    Objective,
    Bridge
};

constexpr std::array<char const*, 8> FIGHT_REASON_WORDS = {"attack_on_sight", "real_player", "pvptest",   "defence",
                                                           "budget",          "grudge",      "objective", "bridge"};

// Why a fight ended (FightEnd's source word). WP1 ends fights by combat_over and the safety cap stalemate_cap; the
// others come with WP6
enum class EndCause : uint8_t
{
    CombatOver,
    StalemateCap,
    Timeout,
    Stalemate,
    Wiped
};

constexpr std::array<char const*, 5> END_CAUSE_WORDS = {"combat_over", "stalemate_cap", "timeout", "stalemate",
                                                        "wiped"};

constexpr char const* Word(CameIn value) { return CAME_IN_WORDS[static_cast<size_t>(value)]; }
constexpr char const* Word(FightReason value) { return FIGHT_REASON_WORDS[static_cast<size_t>(value)]; }
constexpr char const* Word(EndCause value) { return END_CAUSE_WORDS[static_cast<size_t>(value)]; }

constexpr uint32_t MS_PER_SECOND = 1000;
constexpr uint32_t MS_PER_MINUTE = 60 * MS_PER_SECOND;
constexpr uint32_t COUNT_LIMIT = 255;

// One side of a combat as the glue sees it at the moment: a player (real or bot) with its place
struct Combatant
{
    uint64_t guid = 0;
    bool real = false;
    Side side = Side::Alliance;
    uint8_t level = 0;
    float x = 0.0f;
    float y = 0.0f;
    uint32_t zone = 0;
    uint32_t area = 0;
    // A PvpTest fighter (WP.6): its fights are tagged so the checks leave them out
    bool forced = false;
};

struct FightParticipant
{
    uint64_t guid = 0;
    bool real = false;
    Side side = Side::Alliance;
    uint8_t level = 0;
    CameIn cameIn = CameIn::Joined;
    uint32_t joinedMs = 0;
    bool retreated = false;  // WP6
    uint8_t deaths = 0;
    bool inCombat = true;  // at the last tick
    // Where the book last saw it on the fight's map (the end row's place when nobody is left there)
    float x = 0.0f;
    float y = 0.0f;
    uint32_t zone = 0;
    uint32_t area = 0;
};

struct Fight
{
    uint32_t id = 0;
    uint32_t startMs = 0;
    // The last time a participant was in PvP combat (or a combat or kill was noted)
    uint32_t lastHitMs = 0;
    float centreX = 0.0f;
    float centreY = 0.0f;
    float radius = 0.0f;
    uint64_t starter = 0;
    uint64_t target = 0;
    FightReason reason = FightReason::AttackOnSight;
    bool tagged = false;
    std::vector<FightParticipant> participants;
    uint32_t endMs = 0;
    EndCause cause = EndCause::CombatOver;

    FightParticipant* Find(uint64_t guid)
    {
        for (FightParticipant& participant : participants)
            if (participant.guid == guid)
                return &participant;
        return nullptr;
    }

    FightParticipant const* Find(uint64_t guid) const { return const_cast<Fight*>(this)->Find(guid); }

    // The participant whose end row it is when nobody of the fight is on its map any more: the starter, else the first
    FightParticipant const* Anchor() const
    {
        if (FightParticipant const* const starterParticipant = Find(starter))
            return starterParticipant;
        return participants.empty() ? nullptr : &participants.front();
    }

    uint32_t Count(Side side) const
    {
        return static_cast<uint32_t>(std::count_if(participants.begin(), participants.end(),
                                                   [side](FightParticipant const& p) { return p.side == side; }));
    }

    uint32_t Deaths(Side side) const
    {
        uint32_t deaths = 0;
        for (FightParticipant const& participant : participants)
            if (participant.side == side)
                deaths += participant.deaths;
        return deaths;
    }
};

// An ended fight kept for the calm time: the calm areas (WP7 reads them)
struct EndedFight
{
    uint32_t id = 0;
    uint32_t endMs = 0;
    float x = 0.0f;
    float y = 0.0f;
    float radius = 0.0f;
    EndCause cause = EndCause::CombatOver;
    bool tagged = false;
};

// A recent resurrection on the map (WP7 fills them; declared now)
struct Resurrection
{
    uint64_t guid = 0;
    uint32_t ms = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct FightBounds
{
    uint32_t maxFights = 32;
    uint32_t maxParticipants = 40;
    float radius = 40.0f;
    float radiusMax = 150.0f;
    uint32_t maxFightMs = 30 * MS_PER_MINUTE;
    uint32_t endGraceMs = 10 * MS_PER_SECOND;
    uint32_t calmMs = 15 * MS_PER_MINUTE;

    static FightBounds From(WorldPvpConfig const& config)
    {
        FightBounds bounds;
        bounds.maxFights = std::max<uint32_t>(config.MaxActiveFights, 1);
        bounds.maxParticipants = std::max<uint32_t>(config.MaxParticipants, 2);
        bounds.radius = static_cast<float>(std::max<uint32_t>(config.FightRadiusYards, 1));
        bounds.radiusMax = std::max(bounds.radius, static_cast<float>(config.FightRadiusMaxYards));
        bounds.maxFightMs = std::max<uint32_t>(config.MaxFightMinutes, 1) * MS_PER_MINUTE;
        bounds.endGraceMs = config.EndGraceSeconds * MS_PER_SECOND;
        bounds.calmMs = config.CalmMinutes * MS_PER_MINUTE;
        return bounds;
    }
};

// The reason of a fight its aggressor starts: a forced PvpTest fighter on either side makes it pvptest, a real
// aggressor real_player, else a bot started it by today's attack on sight (WP4 adds the budget and the others)
inline FightReason StartReason(Combatant const& aggressor, Combatant const& other)
{
    if (aggressor.forced || other.forced)
        return FightReason::PvpTest;
    return aggressor.real ? FightReason::RealPlayer : FightReason::AttackOnSight;
}

struct JoinNote
{
    uint64_t guid = 0;
    Side side = Side::Alliance;
    CameIn cameIn = CameIn::Joined;
};

// What one noted combat changed: the fight (0 when nothing was recorded), whether it is new, and who joined it
struct CombatNote
{
    uint32_t fightId = 0;
    bool started = false;
    FightReason reason = FightReason::AttackOnSight;
    uint8_t joinCount = 0;
    std::array<JoinNote, 2> joins{};
};

class FightBook
{
public:
    FightBook(FightBounds const& bounds, std::atomic<uint32_t>& ids) : _bounds(bounds), _ids(ids) {}

    void SetBounds(FightBounds const& bounds) { _bounds = bounds; }
    FightBounds const& Bounds() const { return _bounds; }

    // PvP combat between two players of opposite sides, aggressor the one that attacked. Both in the same fight:
    // nothing new. One in a fight: the other joins it (joined when it attacked, targeted when it was attacked).
    // Neither: both join the nearest active fight whose radius holds their midpoint, else they start a new fight (the
    // aggressor started, the other defended). A full book or fight records nothing more (counted).
    CombatNote OnCombat(Combatant const& aggressor, Combatant const& other, uint32_t nowMs)
    {
        CombatNote note;
        Fight* const aggressorFight = FightOf(aggressor.guid);
        Fight* const otherFight = FightOf(other.guid);
        if (aggressorFight && otherFight)
        {
            aggressorFight->lastHitMs = nowMs;
            otherFight->lastHitMs = nowMs;
            note.fightId = aggressorFight->id;
            return note;
        }

        if (aggressorFight || otherFight)
        {
            Fight& fight = aggressorFight ? *aggressorFight : *otherFight;
            Combatant const& newcomer = aggressorFight ? other : aggressor;
            Join(fight, newcomer, aggressorFight ? CameIn::Targeted : CameIn::Joined, nowMs, note);
            return note;
        }

        float const midX = (aggressor.x + other.x) * 0.5f;
        float const midY = (aggressor.y + other.y) * 0.5f;
        if (Fight* const nearest = NearestWithin(midX, midY))
        {
            Join(*nearest, aggressor, CameIn::Joined, nowMs, note);
            Join(*nearest, other, CameIn::Targeted, nowMs, note);
            return note;
        }

        if (_active.size() >= _bounds.maxFights)
        {
            ++_refusedFights;
            return note;
        }

        Fight fight;
        fight.id = _ids.fetch_add(1, std::memory_order_relaxed) + 1;
        fight.startMs = nowMs;
        fight.lastHitMs = nowMs;
        fight.radius = _bounds.radius;
        fight.starter = aggressor.guid;
        fight.target = other.guid;
        fight.reason = StartReason(aggressor, other);
        _active.push_back(std::move(fight));
        note.started = true;
        Join(_active.back(), aggressor, CameIn::Started, nowMs, note);
        Join(_active.back(), other, CameIn::Defended, nowMs, note);
        return note;
    }

    // A PvP kill: noted as combat first (the killer the aggressor), then the death counts for the killed
    CombatNote OnKill(Combatant const& killer, Combatant const& killed, uint32_t nowMs)
    {
        CombatNote note = OnCombat(killer, killed, nowMs);
        if (Fight* const fight = FightOf(killed.guid))
            if (FightParticipant* const participant = fight->Find(killed.guid))
                if (participant->deaths < COUNT_LIMIT)
                    ++participant->deaths;
        return note;
    }

    // Once a second on the map thread. probe(participant) tells whether the participant is still in PvP combat on
    // this map (false when gone) and may update its x and y. A fight none of whose participants has been in PvP
    // combat for the end grace ends as combat_over, its length running to the last PvP combat (combat within the
    // grace continues it); one older than the safety cap ends as stalemate_cap. Ended fights are moved to `ended`
    // and kept as calm areas for the calm time.
    template <typename Probe>
    void Tick(uint32_t nowMs, Probe&& probe, std::vector<Fight>& ended)
    {
        for (size_t index = 0; index < _active.size();)
        {
            Fight& fight = _active[index];
            bool anyInCombat = false;
            for (FightParticipant& participant : fight.participants)
            {
                participant.inCombat = probe(participant);
                anyInCombat = anyInCombat || participant.inCombat;
            }
            if (anyInCombat)
                fight.lastHitMs = nowMs;
            Reshape(fight);

            bool const capped = nowMs - fight.startMs >= _bounds.maxFightMs;
            bool const over = !anyInCombat && nowMs - fight.lastHitMs >= _bounds.endGraceMs;
            if (!over && !capped)
            {
                ++index;
                continue;
            }

            fight.endMs = over ? fight.lastHitMs : nowMs;
            fight.cause = over ? EndCause::CombatOver : EndCause::StalemateCap;
            // The calm area counts from the close, so the ring stays in closing order
            EndedFight calm;
            calm.id = fight.id;
            calm.endMs = nowMs;
            calm.x = fight.centreX;
            calm.y = fight.centreY;
            calm.radius = fight.radius;
            calm.cause = fight.cause;
            calm.tagged = fight.tagged;
            _ended.push_back(calm);
            ended.push_back(std::move(fight));
            if (index + 1 < _active.size())
                _active[index] = std::move(_active.back());
            _active.pop_back();
        }

        while (!_ended.empty() && (_ended.size() > MAX_ENDED_FIGHTS || nowMs - _ended.front().endMs > _bounds.calmMs))
            _ended.pop_front();
    }

    Fight const* FindFightOf(uint64_t guid) const { return const_cast<FightBook*>(this)->FightOf(guid); }
    std::vector<Fight> const& Active() const { return _active; }
    std::deque<EndedFight> const& Ended() const { return _ended; }
    std::vector<Resurrection> const& Resurrections() const { return _resurrections; }
    uint32_t RefusedFights() const { return _refusedFights; }
    uint32_t RefusedParticipants() const { return _refusedParticipants; }

private:
    Fight* FightOf(uint64_t guid)
    {
        for (Fight& fight : _active)
            if (fight.Find(guid))
                return &fight;
        return nullptr;
    }

    Fight* NearestWithin(float x, float y)
    {
        Fight* nearest = nullptr;
        float best = 0.0f;
        for (Fight& fight : _active)
        {
            float const distance = std::hypot(fight.centreX - x, fight.centreY - y);
            if (distance <= fight.radius && (!nearest || distance < best))
            {
                nearest = &fight;
                best = distance;
            }
        }
        return nearest;
    }

    void Join(Fight& fight, Combatant const& combatant, CameIn cameIn, uint32_t nowMs, CombatNote& note)
    {
        fight.lastHitMs = nowMs;
        note.fightId = fight.id;
        note.reason = fight.reason;
        if (fight.Find(combatant.guid))
            return;
        if (fight.participants.size() >= _bounds.maxParticipants)
        {
            ++_refusedParticipants;
            return;
        }

        FightParticipant participant;
        participant.guid = combatant.guid;
        participant.real = combatant.real;
        participant.side = combatant.side;
        participant.level = combatant.level;
        participant.cameIn = cameIn;
        participant.joinedMs = nowMs;
        participant.x = combatant.x;
        participant.y = combatant.y;
        participant.zone = combatant.zone;
        participant.area = combatant.area;
        fight.participants.push_back(participant);
        fight.tagged = fight.tagged || combatant.forced;
        Reshape(fight);
        if (note.joinCount < note.joins.size())
            note.joins[note.joinCount++] = {combatant.guid, combatant.side, cameIn};
    }

    // The centre follows the participants' mean place, the radius the farthest of them (within the bounds)
    void Reshape(Fight& fight) const
    {
        if (fight.participants.empty())
            return;

        float sumX = 0.0f;
        float sumY = 0.0f;
        for (FightParticipant const& participant : fight.participants)
        {
            sumX += participant.x;
            sumY += participant.y;
        }
        float const count = static_cast<float>(fight.participants.size());
        fight.centreX = sumX / count;
        fight.centreY = sumY / count;
        float farthest = 0.0f;
        for (FightParticipant const& participant : fight.participants)
            farthest = std::max(farthest, std::hypot(participant.x - fight.centreX, participant.y - fight.centreY));
        fight.radius = std::clamp(farthest, _bounds.radius, _bounds.radiusMax);
    }

    FightBounds _bounds;
    std::atomic<uint32_t>& _ids;
    std::vector<Fight> _active;
    std::deque<EndedFight> _ended;
    std::vector<Resurrection> _resurrections;
    uint32_t _refusedFights = 0;
    uint32_t _refusedParticipants = 0;
};

struct RecentEnemy
{
    uint64_t guid = 0;
    uint32_t seenMs = 0;
    uint32_t rolledMs = 0;  // WP3/WP4: the last roll for this enemy
};

// A bot's fight memory (in its PlayerbotAI, touched only on its map thread): the enemies it saw recently and its
// fight clock. Each later step adds what it needs (avoided areas WP7, grudges WP8).
class FightMemory
{
public:
    // True when `guid` was seen within the last windowMs; then its time is refreshed (an enemy that stays in sight
    // is one meeting)
    bool SeenRecently(uint64_t guid, uint32_t nowMs, uint32_t windowMs)
    {
        for (uint32_t index = 0; index < _count; ++index)
        {
            RecentEnemy& enemy = _enemies[index];
            if (enemy.guid != guid)
                continue;
            if (nowMs - enemy.seenMs > windowMs)
                return false;
            enemy.seenMs = nowMs;
            return true;
        }
        return false;
    }

    // Remembers `guid` as seen now; a full memory (capacity, at most MAX_RECENT_ENEMIES) forgets its oldest enemy
    void Remember(uint64_t guid, uint32_t nowMs, uint32_t capacity)
    {
        capacity = std::clamp<uint32_t>(capacity, 1, MAX_RECENT_ENEMIES);
        uint32_t oldest = 0;
        for (uint32_t index = 0; index < _count; ++index)
        {
            if (_enemies[index].guid == guid)
            {
                _enemies[index].seenMs = nowMs;
                return;
            }
            if (nowMs - _enemies[index].seenMs > nowMs - _enemies[oldest].seenMs)
                oldest = index;
        }

        RecentEnemy const enemy{guid, nowMs, 0};
        if (_count < capacity)
            _enemies[_count++] = enemy;
        else
            _enemies[oldest] = enemy;
    }

    uint32_t Count() const { return _count; }
    RecentEnemy const& operator[](uint32_t index) const { return _enemies[index]; }

    // The fight the bot is in (0: none), and when its last fight ended
    uint32_t fightId = 0;
    bool hasFought = false;
    uint32_t lastFightEndMs = 0;
    // WP4: the last chance and the lab's "ready" flag (`.playerbots fight ready`: the next chance starts a fight)
    bool hasChance = false;
    uint32_t lastChanceMs = 0;
    bool ready = false;
    // The glue's own clocks: whether the bot is persistent (rechecked now and then) and the last combat check
    bool persistent = false;
    bool persistentKnown = false;
    uint32_t persistentCheckMs = 0;
    uint32_t lastCombatCheckMs = 0;

private:
    std::array<RecentEnemy, MAX_RECENT_ENEMIES> _enemies{};
    uint32_t _count = 0;
};
}  // namespace WorldPvp

#endif
