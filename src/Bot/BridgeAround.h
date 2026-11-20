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
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "BridgeTexts.h"

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
            return BridgeTexts::Around::HOSTILE;
        case AroundKind::Service:
            return BridgeTexts::Around::SERVICE;
        case AroundKind::Task:
            return BridgeTexts::Around::TASK;
        case AroundKind::Person:
            return BridgeTexts::Around::PERSON;
        case AroundKind::Object:
            return BridgeTexts::Around::OBJECT;
    }
    return BridgeTexts::Around::OBJECT;
}

// One unit or object the bot perceives, at its distance (0 for the bot's own victim and selection, which come first).
struct AroundCandidate
{
    std::string Name;
    AroundKind Kind = AroundKind::Object;
    std::string Detail;
    float Distance = 0.0f;
    std::string Type;  // The creature's type word (TypeWord); empty for anything else
};

// LCB4: one more key of a bot's state or of an around entry, which the bridge module forwards to the wire with no
// edit of its own (bridge/game/README.md, *Bot state*). A reader adds one with `open.push_back({"key", value})`. The
// module keeps a key of [a-z][a-z_]{0,23} that it does not write itself, at most 8 per object; text by the 64-byte
// fact rule, whole numbers within +-1e9. The bridge must know a key before a reader sends it.
struct StateKey
{
    std::string key;
    std::variant<bool, int64_t, std::string> value;
};

// One name: the nearest candidate's kind and detail, how many candidates bear the name, the nearest distance.
struct AroundEntry
{
    std::string Name;
    AroundKind Kind = AroundKind::Object;
    std::string Detail;
    uint32_t Count = 0;
    float Distance = 0.0f;
    std::vector<StateKey> open;  // LCB4: a hostile creature's `type`
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
        {
            std::vector<StateKey> open;
            if (!candidate.Type.empty())
                open.push_back({"type", std::move(candidate.Type)});
            entries.push_back({std::move(candidate.Name), candidate.Kind, std::move(candidate.Detail), 1,
                               candidate.Distance, std::move(open)});
        }
    }
    return entries;
}

// Creature types as the core's CreatureType numbers them.
constexpr uint32_t TypeHumanoid = 7;

// The game's word of a creature type; empty for a type with none to say.
inline char const* TypeWord(uint32_t type)
{
    return type < std::size(BridgeTexts::Around::TYPE_WORDS) ? BridgeTexts::Around::TYPE_WORDS[type] : "";
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
        return BridgeTexts::Around::BOSS;
    if (unitLevel <= grayLevel)
        return BridgeTexts::Around::TRIVIAL;
    if (rank == RankElite || rank == RankRareElite)
        return BridgeTexts::Around::ELITE;
    int32_t const above = static_cast<int32_t>(unitLevel) - static_cast<int32_t>(botLevel);
    if (above >= DeadlyLevels)
        return BridgeTexts::Around::DEADLY;
    if (above >= HardLevels)
        return BridgeTexts::Around::HARD;
    if (above <= -EasyLevels)
        return BridgeTexts::Around::EASY;
    return BridgeTexts::Around::EVEN;
}
}  // namespace PlayerbotBridge

#endif
