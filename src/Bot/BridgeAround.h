/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEAROUND_H
#define PLAYERBOTS_BRIDGEAROUND_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// LC8: what is around a bot, for its state (P §3.3 "around you"). Plain data and no game type: ReadBotState collects
// the candidates from the bot's values, this picks the entries. Allocates: a request path, never per tick.
namespace PlayerbotBridge
{
enum class AroundKind : uint8_t
{
    Hostile,  // Detail: how dangerous (HostileDetail)
    Service,  // Detail: the service (innkeeper, flightmaster, banker, auctioneer, trainer, repair, vendor)
    Task,     // Detail: quest (one to take) or reward (one to hand in)
    Person,   // Detail: group, bot or player
    Object    // Detail: mailbox, anvil, forge or fire
};

constexpr std::size_t MaxAround = 12;  // [E] entries in a state

inline char const* AroundKindWord(AroundKind kind)
{
    switch (kind)
    {
        case AroundKind::Hostile:
            return "hostile";
        case AroundKind::Service:
            return "service";
        case AroundKind::Task:
            return "task";
        case AroundKind::Person:
            return "person";
        case AroundKind::Object:
            return "object";
    }
    return "object";
}

// One unit or object the bot perceives, at its distance (0 for the bot's own victim and selection, which come first).
struct AroundCandidate
{
    std::string Name;
    AroundKind Kind = AroundKind::Object;
    std::string Detail;
    float Distance = 0.0f;
};

// One name: the nearest candidate's kind and detail, how many candidates bear the name, the nearest distance.
struct AroundEntry
{
    std::string Name;
    AroundKind Kind = AroundKind::Object;
    std::string Detail;
    uint32_t Count = 0;
    float Distance = 0.0f;
};

// Nearest first (ties keep the candidates' order), one entry per name with its count, at most cap names; the bot's
// own name and empty names are never listed. A name past the cap is left out, a listed name still counts every
// candidate of it.
inline std::vector<AroundEntry> SelectAround(std::vector<AroundCandidate> candidates, std::string_view self,
                                             std::size_t cap = MaxAround)
{
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](AroundCandidate const& a, AroundCandidate const& b) { return a.Distance < b.Distance; });
    std::vector<AroundEntry> entries;
    for (AroundCandidate& candidate : candidates)
    {
        if (candidate.Name.empty() || candidate.Name == self)
            continue;
        auto const listed = std::find_if(entries.begin(), entries.end(), [&candidate](AroundEntry const& entry)
                                         { return entry.Name == candidate.Name; });
        if (listed != entries.end())
            ++listed->Count;
        else if (entries.size() < cap)
            entries.push_back(
                {std::move(candidate.Name), candidate.Kind, std::move(candidate.Detail), 1, candidate.Distance});
    }
    return entries;
}

// Creature ranks as the core's CreatureEliteType numbers them.
constexpr uint32_t RankElite = 1;
constexpr uint32_t RankRareElite = 2;
constexpr uint32_t RankWorldBoss = 3;
constexpr int32_t HardLevels = 3;    // [E] this many levels above the bot: hard
constexpr int32_t DeadlyLevels = 5;  // [E] this many: deadly
constexpr int32_t EasyLevels = 3;    // [E] this many below: easy

// How dangerous a hostile unit is to the bot, in one word: trivial (at or below the gray level), easy, even, hard,
// deadly by the level difference; an elite that is not trivial says elite, a world boss says boss.
inline char const* HostileDetail(uint32_t botLevel, uint32_t unitLevel, uint32_t grayLevel, uint32_t rank)
{
    if (rank == RankWorldBoss)
        return "boss";
    if (unitLevel <= grayLevel)
        return "trivial";
    if (rank == RankElite || rank == RankRareElite)
        return "elite";
    int32_t const above = static_cast<int32_t>(unitLevel) - static_cast<int32_t>(botLevel);
    if (above >= DeadlyLevels)
        return "deadly";
    if (above >= HardLevels)
        return "hard";
    if (above <= -EasyLevels)
        return "easy";
    return "even";
}
}  // namespace PlayerbotBridge

#endif
