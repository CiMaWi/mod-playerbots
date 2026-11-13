/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGENAMES_H
#define PLAYERBOTS_BRIDGENAMES_H

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// How an order's name finds what the bot perceives, knows or carries (LC.7). Plain data and no game type: the
// ability layer collects the candidates, this picks one. Allocates: a command path, never per tick.
namespace PlayerbotBridge
{
// One thing a name may mean: its name, its id (guid, place index, quest id, item entry, spell id) and its distance
// from the bot (0 where distance means nothing).
struct NameCandidate
{
    std::string Name;
    uint64_t Id = 0;
    float Distance = 0.0f;
};

enum class NameMatch : uint8_t
{
    None,
    Exact,   // The same name, without case, spaces or punctuation
    Nearest  // One name holds the other
};

struct NameResult
{
    NameMatch Kind = NameMatch::None;
    std::size_t Index = 0;  // Into the candidates; 0 for None
};

// A part of a name matches only from this many letters on, so "an" finds no "Thane".
constexpr std::size_t MinPartialName = 3;

// The text's letters and digits in lower case: "Thunder Bluff" and "thunderbluff", "Lord Gregor's" and "lordgregors"
// are one name.
inline std::string NameKey(std::string_view text)
{
    std::string key;
    for (char const c : text)
        if (std::isalnum(static_cast<unsigned char>(c)))
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    return key;
}

// An exact name wins over a part of one; among equals the nearest wins, then the first. Two things of one name are
// never ambiguous: the nearer is meant. No candidate, an empty name or no match is None.
inline NameResult ResolveName(std::string_view wanted, std::span<NameCandidate const> candidates)
{
    NameResult best;
    std::string const key = NameKey(wanted);
    if (key.empty())
        return best;

    float bestDistance = 0.0f;
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        std::string const name = NameKey(candidates[index].Name);
        NameMatch kind = NameMatch::None;
        if (name == key)
            kind = NameMatch::Exact;
        else if (key.size() >= MinPartialName && name.size() >= MinPartialName &&
                 (name.find(key) != std::string::npos || key.find(name) != std::string::npos))
            kind = NameMatch::Nearest;

        if (kind == NameMatch::None)
            continue;

        float const distance = candidates[index].Distance;
        bool const better = best.Kind == NameMatch::None ||
                            (kind == NameMatch::Exact && best.Kind == NameMatch::Nearest) ||
                            (kind == best.Kind && distance < bestDistance);
        if (better)
        {
            best = {kind, index};
            bestDistance = distance;
        }
    }
    return best;
}
}  // namespace PlayerbotBridge

#endif
