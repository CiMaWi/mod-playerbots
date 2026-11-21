/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKVESSELRULES_H
#define PLAYERBOTS_ROUTECHECKVESSELRULES_H

// The pure rules of `route check vessel` (navigation plan NV30b, NV33a, NV33b): its arguments, the deck filter it
// searches with, a line of a pairs file and a pair's end gap. No engine headers, so the selftest compiles them alone.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "TravelWalkTypes.h"

namespace RouteCheckVesselRules
{
constexpr std::uint32_t MAX_PAIRS = 2000;        // random pairs per vessel
constexpr std::uint32_t MAX_FILE_PAIRS = 50000;  // pairs read from one file

// NavTerrain (MapDefines.h); RouteCheckVessel.cpp asserts they are equal
constexpr std::uint16_t GROUND = 0x01;
constexpr std::uint16_t MAGMA = 0x02;
constexpr std::uint16_t SLIME = 0x04;
constexpr std::uint16_t WATER = 0x08;
constexpr std::uint16_t GROUND_STEEP = 0x10;

struct FilterFlags
{
    std::uint16_t include = 0;
    std::uint16_t exclude = 0;
    float waterCost = 5.0f;
};

// The bots' polygons ashore, as PathGenerator::CreateFilter gives a headless player.
inline FilterFlags BotsFilter()
{
    FilterFlags flags;
    flags.include = GROUND | WATER;
    flags.exclude = TravelWalk::BOT_EXCLUDE_FLAGS;
    return flags;
}

// The deck's rule (navigation NV.25), as PathGenerator::CalculateVesselPath turns a unit's filter for its vessel
// search (Transport's DeckPlaceFilter is its result for a bot): a filter that takes ground takes steep ground too.
inline FilterFlags DeckRule(FilterFlags flags)
{
    if (flags.include & GROUND)
    {
        flags.include |= GROUND_STEEP;
        flags.exclude &= static_cast<std::uint16_t>(~GROUND_STEEP);
    }
    return flags;
}

// The probe's filter: what a bot aboard searches with (the deck's rule on the bots' filter), or with `bots` the
// bots' filter ashore, for a comparison.
inline FilterFlags DeckFilter(bool bots) { return bots ? BotsFilter() : DeckRule(BotsFilter()); }

inline char const* FilterName(bool bots) { return bots ? "bots" : "deck"; }

inline bool ParseUInt(std::string const& word, std::uint32_t& value)
{
    char* end = nullptr;
    unsigned long const parsed = std::strtoul(word.c_str(), &end, 10);
    value = static_cast<std::uint32_t>(parsed);
    return end && !*end && !word.empty() && word[0] != '-';
}

inline bool ParseFloat(std::string const& word, float& value)
{
    char* end = nullptr;
    value = std::strtof(word.c_str(), &end);
    return end && !*end && !word.empty() && std::isfinite(value);
}

// words split at spaces and tabs
inline std::vector<std::string> Split(std::string const& text)
{
    std::vector<std::string> words;
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t const start = text.find_first_not_of(" \t\r\n", at);
        if (start == std::string::npos)
            break;
        std::size_t const end = std::min(text.find_first_of(" \t\r\n", start), text.size());
        words.push_back(text.substr(start, end - start));
        at = end;
    }
    return words;
}

enum class Form
{
    Bad,
    Points,  // <display id> x1 y1 z1 x2 y2 z2
    Random,  // <display id>|all <count> <seed>
    Pairs    // pairs <file>
};

struct Command
{
    Form form = Form::Bad;
    bool all = false;
    bool bots = false;
    std::uint32_t displayId = 0;
    float offsets[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t count = 0;
    std::uint32_t seed = 0;
    std::string file;
};

// A last word `bots` on any form searches with the bots' filter ashore instead of the deck's rule.
inline Command ParseCommand(std::vector<std::string> words)
{
    Command command;
    if (!words.empty() && words.back() == "bots")
    {
        command.bots = true;
        words.pop_back();
    }
    if (words.size() == 2 && words[0] == "pairs")
    {
        command.file = words[1];
        command.form = Form::Pairs;
        return command;
    }
    if (words.size() != 3 && words.size() != 7)
        return command;
    command.all = words[0] == "all";
    if (!command.all && !ParseUInt(words[0], command.displayId))
        return command;
    if (words.size() == 7)
    {
        if (command.all)
            return command;
        for (std::size_t i = 0; i < 6; ++i)
            if (!ParseFloat(words[i + 1], command.offsets[i]))
                return command;
        command.form = Form::Points;
        return command;
    }
    if (ParseUInt(words[1], command.count) && ParseUInt(words[2], command.seed) && command.count &&
        command.count <= MAX_PAIRS)
        command.form = Form::Random;
    return command;
}

enum class LineKind
{
    Pair,
    Skipped,     // empty, a comment (#) or the header (first word `id` or `display`)
    FieldCount,  // not 7 or 8 words
    DisplayId,   // the display id is no number
    Offset       // an offset is no finite number
};

inline char const* LineKindName(LineKind kind)
{
    switch (kind)
    {
        case LineKind::Pair:
            return "pair";
        case LineKind::Skipped:
            return "skipped";
        case LineKind::FieldCount:
            return "not 7 or 8 fields";
        case LineKind::DisplayId:
            return "bad display id";
        case LineKind::Offset:
            return "bad offset";
    }
    return "?";
}

struct PairLine
{
    std::string id;
    std::uint32_t displayId = 0;
    float offsets[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

// A pairs file's line: [id] <display id> <x1 y1 z1> <x2 y2 z2>, tab or space separated; without an id the pair's id
// is its line number (1-based).
inline LineKind ParsePairLine(std::string const& line, std::size_t lineNumber, PairLine& pair)
{
    std::vector<std::string> const words = Split(line);
    if (words.empty() || words[0][0] == '#' || words[0] == "id" || words[0] == "display")
        return LineKind::Skipped;
    if (words.size() != 7 && words.size() != 8)
        return LineKind::FieldCount;
    std::size_t const first = words.size() == 8 ? 1 : 0;
    pair.id = first ? words[0] : std::to_string(lineNumber);
    if (!ParseUInt(words[first], pair.displayId))
        return LineKind::DisplayId;
    for (std::size_t i = 0; i < 6; ++i)
        if (!ParseFloat(words[first + 1 + i], pair.offsets[i]))
            return LineKind::Offset;
    return LineKind::Pair;
}

// yd from a path's last point to the requested end (x, y, z); negative when the search returned no point
inline float EndGap(std::size_t pointCount, float const* lastPoint, float const* end)
{
    if (!pointCount)
        return -1.0f;
    float const dx = lastPoint[0] - end[0];
    float const dy = lastPoint[1] - end[1];
    float const dz = lastPoint[2] - end[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace RouteCheckVesselRules

#endif
