/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKTARGETSRULES_H
#define PLAYERBOTS_ROUTECHECKTARGETSRULES_H

// Pure rules of `playerbots route check targets <file>` (pathing plan PA8): no core headers, so the selftest
// (tests/route-check-targets-selftest.cpp) compiles them alone. The stand point is the bots' own (pathing PA46:
// NavMeshSnap::StandPoint gathers the mesh's candidates, StandRules::Pick chooses); this file only reads its facts and
// words the answer.
//
// What a row says about a target (map, position, reach; max_yd: the stand rule's greatest distance, the core's own
// check for the spawn's kind and size less a margin, or the row's reach for a station or a control):
// - stand: the stand rule's point (sx sy sz, its side, stand_yd); fragment 1: no joined floor in the band, the point is
//   the nearest of the target's closed piece (the rule's `fragment`).
// - no_sight: joined floor in the band, none of it in sight of the target.
// - steep_only: no point in the band, but a polygon within max_yd (3D) is steep ground, which the bots' filter leaves
//   out.
// - other_floor: no point, no steep, but a bots' polygon lies within max_yd horizontally on another level (the probe's
//   own vertical box, OTHER_FLOOR_VERTICAL_YARDS).
// - no_poly: none of these. no_tile: no answer (no navmesh, the tile did not load, an instance map).

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace RouteCheckTargetsRules
{
// The probe's own number (no approach counterpart): how far above or below the target another floor is looked for
constexpr float OTHER_FLOOR_VERTICAL_YARDS = 100.0f;
// The nearest graph nodes listed per row (PA9 joins a stand point to its five nearest)
constexpr std::size_t NODES_LISTED = 5;
constexpr std::size_t COLUMNS = 20;

enum class Result : std::uint8_t
{
    Stand,
    NoSight,
    SteepOnly,
    OtherFloor,
    NoPoly,
    NoTile
};

struct Target
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float reach{0.0f};
    std::string kind{"-"};
    std::string entry{"-"};
    std::string guid{"-"};
};

// What the polygon queries found
struct Facts
{
    bool tileKnown{false};
    bool stand{false};       // the stand rule's point (or its fragment's)
    bool noSight{false};     // the stand rule's no_sight
    bool steepNear{false};   // a steep polygon within reach (3D)
    bool otherFloor{false};  // a bots' polygon within reach horizontally, any level of the tall box
};

inline Result Classify(Facts const& facts)
{
    if (!facts.tileKnown)
        return Result::NoTile;
    if (facts.stand)
        return Result::Stand;
    if (facts.noSight)
        return Result::NoSight;
    if (facts.steepNear)
        return Result::SteepOnly;
    if (facts.otherFloor)
        return Result::OtherFloor;
    return Result::NoPoly;
}

inline char const* Word(Result result)
{
    switch (result)
    {
        case Result::Stand:
            return "stand";
        case Result::NoSight:
            return "no_sight";
        case Result::SteepOnly:
            return "steep_only";
        case Result::OtherFloor:
            return "other_floor";
        case Result::NoPoly:
            return "no_poly";
        default:
            return "no_tile";
    }
}

// A target line: `map x y z reach [kind [entry [guid]]]`, whitespace separated, `#` starts a comment line
inline bool ParseTargetLine(std::string const& line, Target& out)
{
    std::istringstream in(line);
    std::string first;
    if (!(in >> first) || first[0] == '#')
        return false;
    Target target;
    try
    {
        std::size_t used = 0;
        unsigned long const map = std::stoul(first, &used);
        if (used != first.size())
            return false;
        target.mapId = static_cast<std::uint32_t>(map);
    }
    catch (...)
    {
        return false;
    }
    if (!(in >> target.x >> target.y >> target.z >> target.reach) || !(target.reach >= 0.0f))
        return false;
    std::string word;
    if (in >> word)
        target.kind = word;
    if (in >> word)
        target.entry = word;
    if (in >> word)
        target.guid = word;
    out = target;
    return true;
}

struct Node
{
    std::uint32_t id{0};
    float yards{0.0f};
};

struct Row
{
    std::uint32_t index{0};  // 1-based among the usable input rows
    Target target;
    Result result{Result::NoTile};
    float sx{0.0f};
    float sy{0.0f};
    float sz{0.0f};
    float standYards{0.0f};
    bool fragment{false};
    std::string side{"-"};    // front, beside, behind (StandRules::Word)
    float maxYards{0.0f};     // the stand rule's greatest distance
    std::vector<Node> nodes;  // nearest first
};

inline std::string Num(float value)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.4f", static_cast<double>(value));
    return buffer;
}

inline std::string ColumnLine()
{
    return "row\tmap\tx\ty\tz\treach\tkind\tentry\tguid\tresult\tsx\tsy\tsz\tstand_yd\tfragment\tnode\tnode_"
           "yd\tnodes5\tside"
           "\tmax_yd";
}

inline std::string FormatRow(Row const& row)
{
    bool const stand = row.result == Result::Stand;
    std::string out = std::to_string(row.index) + "\t" + std::to_string(row.target.mapId) + "\t" + Num(row.target.x) +
                      "\t" + Num(row.target.y) + "\t" + Num(row.target.z) + "\t" + Num(row.target.reach) + "\t" +
                      row.target.kind + "\t" + row.target.entry + "\t" + row.target.guid + "\t" + Word(row.result);
    if (stand)
        out += "\t" + Num(row.sx) + "\t" + Num(row.sy) + "\t" + Num(row.sz) + "\t" + Num(row.standYards) + "\t" +
               (row.fragment ? "1" : "0");
    else
        out += "\t-\t-\t-\t-\t-";
    if (row.nodes.empty())
        out += "\t-\t-\t-";
    else
    {
        out += "\t" + std::to_string(row.nodes[0].id) + "\t" + Num(row.nodes[0].yards) + "\t";
        for (std::size_t i = 0; i < row.nodes.size() && i < NODES_LISTED; ++i)
            out += (i ? "," : "") + std::to_string(row.nodes[i].id) + ":" + Num(row.nodes[i].yards);
    }
    out += "\t" + (stand ? row.side : std::string("-")) + "\t" + Num(row.maxYards);
    return out;
}

struct Counts
{
    std::uint32_t rows{0};
    std::uint32_t stand{0};
    std::uint32_t fragment{0};  // of the stand rows
    std::uint32_t noSight{0};
    std::uint32_t steepOnly{0};
    std::uint32_t otherFloor{0};
    std::uint32_t noPoly{0};
    std::uint32_t noTile{0};
};

inline void Count(Counts& counts, Row const& row)
{
    ++counts.rows;
    switch (row.result)
    {
        case Result::Stand:
            ++counts.stand;
            if (row.fragment)
                ++counts.fragment;
            break;
        case Result::NoSight:
            ++counts.noSight;
            break;
        case Result::SteepOnly:
            ++counts.steepOnly;
            break;
        case Result::OtherFloor:
            ++counts.otherFloor;
            break;
        case Result::NoPoly:
            ++counts.noPoly;
            break;
        default:
            ++counts.noTile;
            break;
    }
}

inline std::string SummaryLine(Counts const& counts)
{
    return "ROUTE check targets: " + std::to_string(counts.rows) + " rows: stand " + std::to_string(counts.stand) +
           " (fragment " + std::to_string(counts.fragment) + "), no_sight " + std::to_string(counts.noSight) +
           ", steep_only " + std::to_string(counts.steepOnly) + ", other_floor " + std::to_string(counts.otherFloor) +
           ", no_poly " + std::to_string(counts.noPoly) + ", no_tile " + std::to_string(counts.noTile);
}
}  // namespace RouteCheckTargetsRules

#endif
