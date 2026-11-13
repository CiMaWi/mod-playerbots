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
#include <limits>
#include <utility>
#include <vector>

#include "WorldPvpConfig.h"

// The fight book of world PvP (WP1): one map's open-world PvP fights, its recently ended fights and (WP7) its recent
// resurrections, and a bot's fight memory. Player-free: the glue (FightBook.cpp) turns players into Combatants and
// owns the threads; tests/fight-book-selftest.cpp proves the rules. A book is read and changed only on its map's
// update thread, a memory only on its bot's.
// Its clocks (WP6): lastHitMs is the last tick a participant was in PvP combat, which the core keeps up about 5 s after
// the last hit; lastSideHitMs the last noted combat between the sides (or a tick with two enemy participants in PvP
// combat with each other). A fight ends wiped as soon as a side has nobody here, alive and not retreated; combat_over
// after EndGraceSeconds without PvP combat (length to the last PvP combat, so the 5 s tail is in it, the grace not);
// timeout after NoHitSeconds without a hit between the sides (length to that hit, tail included), which ends only
// fights whose participants stay in PvP combat with others; stalemate at StalemateSeconds; stalemate_cap at
// MaxFightMinutes.
namespace WorldPvp
{
enum class Side : uint8_t
{
    Alliance = 0,
    Horde = 1
};

constexpr size_t SIDE_COUNT = 2;

// How a participant came into its fight (FightJoin's source word): the starter, the starter's first target, a player
// that attacked a participant, a player a participant attacked, a bot that joined its group's or real master's fight
// (WP5); bridge comes with WP12
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
// "Never" for the time-since inputs, and "none in reach" for the distance inputs
constexpr uint32_t NEVER_MS = std::numeric_limits<uint32_t>::max();
constexpr float FAR_YARDS = 1.0e9f;
// The bystanders of one fight whose join answer the fight keeps (WP5, WP.7), and a map's kept resurrections (WP7)
constexpr uint32_t MAX_DECIDED = 64;
constexpr uint32_t MAX_RESURRECTIONS = 256;

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
    // The reason of a fight this bot starts on the other: the rules' (budget, grudge) when they picked the other
    FightReason start = FightReason::AttackOnSight;
    // How this bot comes into a fight by attacking one of it: group or master when it joined that fight for them
    CameIn joinedAs = CameIn::Joined;
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
    uint32_t retreatedMs = 0;
    uint8_t deaths = 0;
    bool inCombat = true;  // at the last tick
    // The last tick's probe (WP6): on the fight's map, alive, in PvP combat with an enemy participant of the fight
    bool here = true;
    bool alive = true;
    bool sideCombat = false;
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
    // The last hit between the sides (WP6's no-hit timeout)
    uint32_t lastSideHitMs = 0;
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
    // The bystanders that decided whether to join it, once each (WP5); a full list takes no more
    std::array<uint64_t, MAX_DECIDED> decided{};
    uint32_t decidedCount = 0;
    // The bystanders beyond a full list, each counted once
    uint32_t refusedDecisions = 0;

    bool HasDecided(uint64_t guid) const
    {
        return std::find(decided.begin(), decided.begin() + decidedCount, guid) != decided.begin() + decidedCount;
    }

    bool NoteDecided(uint64_t guid)
    {
        if (HasDecided(guid))
            return true;
        if (decidedCount >= MAX_DECIDED)
            return false;
        decided[decidedCount++] = guid;
        return true;
    }

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

// A recent resurrection on the map (WP7 notes them)
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
    uint32_t resurrectedMs = 300 * MS_PER_SECOND;

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
        bounds.resurrectedMs = config.ResurrectedSeconds * MS_PER_SECOND;
        return bounds;
    }
};

// The reason of a fight its aggressor starts: a forced PvpTest fighter on either side makes it pvptest, a real
// aggressor real_player, else the bot's own (the rules' budget or grudge, attack_on_sight for today's scan)
inline FightReason StartReason(Combatant const& aggressor, Combatant const& other)
{
    if (aggressor.forced || other.forced)
        return FightReason::PvpTest;
    return aggressor.real ? FightReason::RealPlayer : aggressor.start;
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

// A fight's clock (WP6): its age, the time since the last hit between the sides, and the participants per side still in
// it: here, alive and not retreated (a retreated one counts while it is still in PvP combat: the chase is the fight's)
struct FightClock
{
    uint32_t ageMs = 0;
    uint32_t sinceHitMs = 0;
    std::array<uint32_t, SIDE_COUNT> active{};
};

// The rules' end of a fight (FightRules.h's CheckFightEnd)
struct FightEndCheck
{
    bool ends = false;
    EndCause cause = EndCause::CombatOver;
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
            if (aggressorFight == otherFight)
                aggressorFight->lastSideHitMs = nowMs;
            note.fightId = aggressorFight->id;
            return note;
        }

        if (aggressorFight || otherFight)
        {
            Fight& fight = aggressorFight ? *aggressorFight : *otherFight;
            Combatant const& newcomer = aggressorFight ? other : aggressor;
            Join(fight, newcomer, aggressorFight ? CameIn::Targeted : aggressor.joinedAs, nowMs, note);
            return note;
        }

        float const midX = (aggressor.x + other.x) * 0.5f;
        float const midY = (aggressor.y + other.y) * 0.5f;
        if (Fight* const nearest = NearestWithin(midX, midY))
        {
            Join(*nearest, aggressor, aggressor.joinedAs, nowMs, note);
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
        fight.lastSideHitMs = nowMs;
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
    // this map (false when gone) and may update its place and its here, alive and sideCombat flags; judge(clock)
    // gives the rules' end (CheckFightEnd). A fight ends wiped as soon as the judge says so; else as combat_over once
    // none of its participants has been in PvP combat for the end grace, its length running to the last PvP combat
    // (combat within the grace continues it); else by the judge's timeout (length to the last hit between the sides)
    // or stalemate; else as stalemate_cap at the safety cap. Ended fights are moved to `ended` and kept as calm
    // areas for the calm time.
    template <typename Probe, typename Judge>
    void Tick(uint32_t nowMs, Probe&& probe, Judge&& judge, std::vector<Fight>& ended)
    {
        for (size_t index = 0; index < _active.size();)
        {
            Fight& fight = _active[index];
            bool anyInCombat = false;
            bool sideCombat = false;
            for (FightParticipant& participant : fight.participants)
            {
                participant.inCombat = probe(participant);
                anyInCombat = anyInCombat || participant.inCombat;
                sideCombat = sideCombat || (participant.inCombat && participant.sideCombat);
            }
            if (anyInCombat)
                fight.lastHitMs = nowMs;
            if (sideCombat)
                fight.lastSideHitMs = nowMs;
            Reshape(fight);

            FightEndCheck const rules = judge(ClockOf(fight, nowMs));
            bool const wiped = rules.ends && rules.cause == EndCause::Wiped;
            bool const over = !anyInCombat && nowMs - fight.lastHitMs >= _bounds.endGraceMs;
            bool const capped = nowMs - fight.startMs >= _bounds.maxFightMs;
            if (!rules.ends && !over && !capped)
            {
                ++index;
                continue;
            }

            if (wiped || (!over && rules.ends))
            {
                fight.cause = rules.cause;
                fight.endMs = rules.cause == EndCause::Timeout ? fight.lastSideHitMs : nowMs;
            }
            else
            {
                fight.endMs = over ? fight.lastHitMs : nowMs;
                fight.cause = over ? EndCause::CombatOver : EndCause::StalemateCap;
            }
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

    // Without the rules' ends: the end grace and the safety cap only
    template <typename Probe>
    void Tick(uint32_t nowMs, Probe&& probe, std::vector<Fight>& ended)
    {
        Tick(nowMs, std::forward<Probe>(probe), [](FightClock const&) { return FightEndCheck{}; }, ended);
    }

    static FightClock ClockOf(Fight const& fight, uint32_t nowMs)
    {
        FightClock clock;
        clock.ageMs = nowMs - fight.startMs;
        clock.sinceHitMs = nowMs - fight.lastSideHitMs;
        for (FightParticipant const& participant : fight.participants)
            if (participant.here && participant.alive && (!participant.retreated || participant.inCombat))
                ++clock.active[static_cast<size_t>(participant.side)];
        return clock;
    }

    Fight const* FindFightOf(uint64_t guid) const { return const_cast<FightBook*>(this)->FightOf(guid); }
    Fight const* FindFight(uint32_t fightId) const
    {
        for (Fight const& fight : _active)
            if (fight.id == fightId)
                return &fight;
        return nullptr;
    }
    std::vector<Fight> const& Active() const { return _active; }
    std::deque<EndedFight> const& Ended() const { return _ended; }
    std::vector<Resurrection> const& Resurrections() const { return _resurrections; }

    // A player resurrected on this map now (its latest only); the oldest beyond MAX_RESURRECTIONS and those older
    // than the bounds' resurrected time are dropped
    void NoteResurrection(uint64_t guid, float x, float y, uint32_t nowMs)
    {
        std::erase_if(_resurrections, [&](Resurrection const& resurrection)
                      { return resurrection.guid == guid || nowMs - resurrection.ms >= _bounds.resurrectedMs; });
        if (_resurrections.size() >= MAX_RESURRECTIONS)
            _resurrections.erase(_resurrections.begin());
        _resurrections.push_back({guid, nowMs, x, y});
    }

    // How long ago `guid` was resurrected here, NEVER_MS when not within the resurrected time
    uint32_t ResurrectedMsAgo(uint64_t guid, uint32_t nowMs) const
    {
        for (Resurrection const& resurrection : _resurrections)
            if (resurrection.guid == guid && nowMs - resurrection.ms < _bounds.resurrectedMs)
                return nowMs - resurrection.ms;
        return NEVER_MS;
    }
    // A bystander of active fight `fightId` gave its join answer (WP5); false when the fight is gone or its decided
    // list is full (that fight takes no more answers; the glue counts the bystander once, CountRefusedDecision)
    bool NoteDecided(uint32_t fightId, uint64_t guid)
    {
        for (Fight& fight : _active)
            if (fight.id == fightId)
                return fight.NoteDecided(guid);
        return false;
    }

    void CountRefusedDecision(uint32_t fightId)
    {
        ++_refusedDecisions;
        for (Fight& fight : _active)
            if (fight.id == fightId)
                ++fight.refusedDecisions;
    }

    // A participant retreats from its fight (WP6): from now on it counts for its side only while it is still in PvP
    // combat. The fight's id, 0 when the guid is in none
    uint32_t MarkRetreated(uint64_t guid, uint32_t nowMs)
    {
        Fight* const fight = FightOf(guid);
        FightParticipant* const participant = fight ? fight->Find(guid) : nullptr;
        if (!participant)
            return 0;
        if (!participant->retreated)
        {
            participant->retreated = true;
            participant->retreatedMs = nowMs;
        }
        return fight->id;
    }

    uint32_t RefusedFights() const { return _refusedFights; }
    uint32_t RefusedParticipants() const { return _refusedParticipants; }
    uint32_t RefusedDecisions() const { return _refusedDecisions; }

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
        fight.lastSideHitMs = nowMs;
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
    uint32_t _refusedDecisions = 0;
};

struct RecentEnemy
{
    uint64_t guid = 0;
    uint32_t seenMs = 0;
    uint32_t rolledMs = 0;  // WP4: the last roll for this enemy
    bool rolled = false;
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

    // Since this bot last rolled for `guid` (NEVER_MS: not remembered or never rolled for)
    uint32_t SinceRolled(uint64_t guid, uint32_t nowMs) const
    {
        for (uint32_t index = 0; index < _count; ++index)
            if (_enemies[index].guid == guid)
                return _enemies[index].rolled ? nowMs - _enemies[index].rolledMs : NEVER_MS;
        return NEVER_MS;
    }

    // Marks a remembered enemy as rolled for now
    void MarkRolled(uint64_t guid, uint32_t nowMs)
    {
        for (uint32_t index = 0; index < _count; ++index)
        {
            if (_enemies[index].guid != guid)
                continue;
            _enemies[index].rolledMs = nowMs;
            _enemies[index].rolled = true;
            return;
        }
    }

    // The fight the bot is in (0: none), and when its last fight ended
    uint32_t fightId = 0;
    bool hasFought = false;
    uint32_t lastFightEndMs = 0;
    // WP4: the last chance and the test constraint fightready (MemoryOf copies it: its chances start a fight)
    bool hasChance = false;
    uint32_t lastChanceMs = 0;
    bool ready = false;
    // The glue's own clocks: whether the bot is persistent (rechecked now and then) and the last combat check
    bool persistent = false;
    bool persistentKnown = false;
    uint32_t persistentCheckMs = 0;
    uint32_t lastCombatCheckMs = 0;
    // When the memory was made (the bot's login): the first chance's play time counts from there
    uint32_t bornMs = 0;

    // WP4: the target the rules picked at a chance (0: none), when and why; the scan attacks it while it is fresh
    uint64_t startGuid = 0;
    uint32_t startMs = 0;
    FightReason startReason = FightReason::Budget;
    // WP4: the bot-side gate values, refreshed every few seconds: outside the prohibited zones, a real player as its
    // master or in its group, a bot group's member that is not its leader, the nearest graveyard, the traits (-1..+1)
    bool gateKnown = false;
    uint32_t gateMs = 0;
    bool openWorld = false;
    bool realPlayer = false;
    bool groupMember = false;
    float graveyardYards = FAR_YARDS;
    float aggression = 0.0f;
    float caution = 0.0f;
    float helpfulness = 0.0f;
    float courtesy = 0.0f;
    float patience = 0.0f;

    // WP5: the fight the bot decided to join, its target, when and how it comes in
    uint64_t joinGuid = 0;
    uint32_t joinFightId = 0;
    uint32_t joinMs = 0;
    CameIn joinCameIn = CameIn::Joined;

    // A full fight this bystander was counted for once (WP5: the fight's refused answers)
    uint32_t refusedFightId = 0;

    // WP6: the retreat (from which fight, since and until when, away from where the threat is; a disengage is a
    // timeout's or stalemate's end, not a retreat by the rules), the chase of a retreated enemy (its target, start,
    // the fight's centre and radius then; once over the bot leaves it alone for a while) and the calm-area answer
    bool retreating = false;
    bool disengaging = false;
    uint32_t retreatStartMs = 0;
    uint32_t retreatUntilMs = 0;
    uint32_t retreatFightId = 0;
    float retreatX = 0.0f;
    float retreatY = 0.0f;
    uint64_t chaseGuid = 0;
    uint32_t chaseStartMs = 0;
    float chaseX = 0.0f;
    float chaseY = 0.0f;
    float chaseRadius = 0.0f;
    bool chaseOver = false;
    uint32_t chaseOverMs = 0;
    bool calmKnown = false;
    bool calm = false;
    uint32_t calmCheckMs = 0;
    float calmX = 0.0f;
    float calmY = 0.0f;

    // WP7: the ghost's wait near its corpse
    bool ghostWaiting = false;
    uint32_t ghostSinceMs = 0;
    uint32_t ghostNextCheckMs = 0;

private:
    std::array<RecentEnemy, MAX_RECENT_ENEMIES> _enemies{};
    uint32_t _count = 0;
};
}  // namespace WorldPvp

#endif
