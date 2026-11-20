/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTPLACERULES_H
#define _PLAYERBOT_QUESTPLACERULES_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "QuestIndex.h"

// Where a bot does an objective or turns a quest in (questing WT33b, PLAN 3.1): the index's places, which carry a real
// height (a spawn's, or a baked floor for an objective with only a POI). One rule for the executor and the planner:
// an objective's place is its nearest usable area on the bot's map, measured in the plane as the circuit's Locate
// does (the first of equally near areas, in the index's order); a turn-in's place is the nearest ender spawn. No game
// header: tests/quest-places-selftest.cpp checks it against the circuit over a fixture index
namespace Planner
{
namespace QuestPlaces
{
// An objective's place for the executor: the area and the objective's index in QuestRecord::objectives
struct Place
{
    AreaPoint const* area{nullptr};
    std::uint32_t objective{0};
    float yards{0.0f};  // from the bot, in the plane
};

// The place a bot just stayed at (WT33b): after a stay with progress the executor picks again and leaves it for the
// objective's next area, where the targets are not thinned out; an objective with one area keeps it
struct Skip
{
    bool on{false};
    float x{0.0f};
    float y{0.0f};
};

// At its place a bot roams inside the area (the spawns' spread), at least a short look around and at most a camp's
// width, so it meets the targets the area holds without drifting off to the next one
constexpr float ROAM_MIN_YARDS = 8.0f;
constexpr float ROAM_MAX_YARDS = 40.0f;

inline float Roam(AreaPoint const& area) { return std::clamp(area.radius, ROAM_MIN_YARDS, ROAM_MAX_YARDS); }

// The objectives the executor walks to: kill, object and item objectives (an area trigger and the turn-in are not
// an objective it works toward; the turn-in has TurnInPlace)
inline bool Walked(ObjectiveKind kind)
{
    return kind == ObjectiveKind::Kill || kind == ObjectiveKind::Object || kind == ObjectiveKind::Item;
}

// The objective's nearest area this bot can use on the map; nullptr when it has none there
template <typename HasSkill>
AreaPoint const* PlaceOf(QuestObjective const& objective, std::uint32_t mapId, float x, float y, std::uint8_t team,
                         HasSkill hasSkill, float* yardsOut = nullptr, Skip const& skip = Skip{})
{
    AreaPoint const* best = nullptr;
    AreaPoint const* left = nullptr;
    float bestYards = 0.0f;
    for (AreaPoint const& area : objective.areas)
    {
        if (area.mapId != mapId || !AreaUsable(area, team, hasSkill))
            continue;
        if (skip.on && std::fabs(area.x - skip.x) < 1.0f && std::fabs(area.y - skip.y) < 1.0f)
        {
            left = &area;
            continue;
        }
        float const yards = std::hypot(area.x - x, area.y - y);
        if (!best || yards < bestYards)
        {
            best = &area;
            bestYards = yards;
        }
    }
    if (!best && left)
    {
        best = left;
        bestYards = std::hypot(left->x - x, left->y - y);
    }
    if (best && yardsOut)
        *yardsOut = bestYards;
    return best;
}

// The turn-in's place on the map: the nearest ender spawn (the Turnin objective's areas, built from the enders);
// nullptr when the quest has none there
inline AreaPoint const* TurnInPlace(QuestRecord const& quest, std::uint32_t mapId, float x, float y,
                                    float* yardsOut = nullptr)
{
    for (QuestObjective const& objective : quest.objectives)
        if (objective.kind == ObjectiveKind::Turnin)
            return PlaceOf(objective, mapId, x, y, 0, [](std::uint16_t) { return true; }, yardsOut);
    return nullptr;
}

// The places of the objectives still open, nearest first (ties by objective index). `open(poiIndex)` says whether
// the objective in that quest-log slot still needs progress (0..3 kill or object, POI_FIRST_ITEM + i item i)
template <typename Open, typename HasSkill>
std::vector<Place> OpenPlaces(QuestRecord const& quest, std::uint32_t mapId, float x, float y, std::uint8_t team,
                              Open open, HasSkill hasSkill, Skip const& skip = Skip{})
{
    std::vector<Place> places;
    for (std::uint32_t i = 0; i < quest.objectives.size(); ++i)
    {
        QuestObjective const& objective = quest.objectives[i];
        if (!Walked(objective.kind) || !open(objective.poiIndex))
            continue;
        float yards = 0.0f;
        if (AreaPoint const* area = PlaceOf(objective, mapId, x, y, team, hasSkill, &yards, skip))
            places.push_back({area, i, yards});
    }
    std::stable_sort(places.begin(), places.end(), [](Place const& a, Place const& b) { return a.yards < b.yards; });
    return places;
}
}  // namespace QuestPlaces
}  // namespace Planner

#endif
