/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTEXECRULES_H
#define _PLAYERBOT_QUESTEXECRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "QuestIndex.h"
#include "QuestPlaceRules.h"

// What the quest executor does next (questing WT35a, PLAN 3.2): one step function over the bot's quest log, the
// index's places, the capability table and the places tried in this run. The log is the state: the function keeps
// nothing, so a fresh start after a fight, a death, an errand or a logout returns what the run would have done next.
// No game header: tests/quest-exec-selftest.cpp checks it over generated quests and logs
namespace Planner
{
namespace QuestExec
{
// The work an objective asks for, as the capability table names it
enum class Work : std::uint8_t
{
    Kill,
    Loot,
    Object,
    Explore,
    Escort,
    Count
};
constexpr std::size_t WORKS = static_cast<std::size_t>(Work::Count);

// A row of the capability table: the work's name, the Status word that refuses it, and whether the executor does it
struct Capability
{
    char const* name;
    char const* refusal;
    bool done;
};

constexpr std::array<Capability, WORKS> CAPABILITIES = {{
    {"kill", "unsupported:kill", true},         // attacks the objective's creature it sought, not whatever stands near
    {"loot", "unsupported:loot", true},         // kills a source creature of the item and loots it
    {"object", "unsupported:object", false},    // WT35e
    {"explore", "unsupported:explore", false},  // WT35f
    {"escort", "unsupported:escort", false},    // unsupported by name (PLAN 3.2)
}};

inline Capability const& CapabilityOf(Work work) { return CAPABILITIES[static_cast<std::size_t>(work)]; }

// The work of an objective of the index; Count for one the executor does not work toward (the turn-in)
inline Work WorkOf(ObjectiveKind kind)
{
    switch (kind)
    {
        case ObjectiveKind::Kill:
            return Work::Kill;
        case ObjectiveKind::Item:
            return Work::Loot;
        case ObjectiveKind::Object:
            return Work::Object;
        case ObjectiveKind::Explore:
            return Work::Explore;
        default:
            return Work::Count;
    }
}

enum class Act : std::uint8_t
{
    Work,      // work this objective at this place
    TurnIn,    // every objective is met
    Refused,   // an open objective has no capability row: the order ends with its refusal word
    NoPlace,   // no open objective has a usable place on this map
    NoTarget,  // every place of the open objectives was tried in this run without a target
    Done       // the quest is not in the log
};

struct Step
{
    Act act{Act::Done};
    Work work{Work::Count};
    std::uint32_t objective{0};  // index in QuestRecord::objectives
    std::uint32_t area{0};       // index in that objective's areas
    AreaPoint const* place{nullptr};

    bool operator==(Step const& other) const
    {
        return act == other.act && work == other.work && objective == other.objective && area == other.area;
    }
};

// A place of the run, by its objective and area; the places tried are kept only while the run lasts
using PlaceKey = std::uint16_t;
constexpr PlaceKey Key(std::uint32_t objective, std::uint32_t area)
{
    return static_cast<PlaceKey>((objective << 8) | (area & 0xFFu));
}
constexpr PlaceKey KeyOf(Step const& step) { return Key(step.objective, step.area); }

inline bool WasTried(std::vector<PlaceKey> const& tried, PlaceKey key)
{
    return std::find(tried.begin(), tried.end(), key) != tried.end();
}

// What the log says of the quest as a whole
struct LogState
{
    bool inLog{false};
    bool complete{false};
};

// A place the run may go to next
struct Candidate
{
    std::uint32_t objective{0};
    std::uint32_t area{0};
    AreaPoint const* place{nullptr};
    float yards{0.0f};  // from the bot, in the plane
};

// The next step. `open(poiIndex)` says whether the objective in that quest-log slot still needs progress;
// `hasSkill(id)` whether the bot has a skill; `prefer(candidates)` is a planner's pick among the candidates (nearest
// first), -1 for the nearest. `skip` is the place a stay with progress left (WT33b), passed over while the objective
// has another; `tried` the places tried in this run without a target
template <typename Open, typename HasSkill, typename Prefer>
Step Next(QuestRecord const& quest, LogState log, Open open, std::uint32_t mapId, float x, float y, std::uint8_t team,
          HasSkill hasSkill, std::vector<PlaceKey> const& tried, QuestPlaces::Skip const& skip, Prefer prefer)
{
    if (!log.inLog)
        return {Act::Done};
    if (log.complete)
        return {Act::TurnIn};

    // Refused at the order's start, before any walk: an open objective the executor has no interaction for
    for (std::uint32_t i = 0; i < quest.objectives.size(); ++i)
    {
        QuestObjective const& objective = quest.objectives[i];
        Work const work = WorkOf(objective.kind);
        if (work != Work::Count && !CapabilityOf(work).done && open(objective.poiIndex))
            return {Act::Refused, work, i};
    }

    std::vector<Candidate> candidates;
    bool placed = false;  // an open objective has a usable place on this map, tried or not
    for (std::uint32_t i = 0; i < quest.objectives.size(); ++i)
    {
        QuestObjective const& objective = quest.objectives[i];
        if (WorkOf(objective.kind) == Work::Count || !open(objective.poiIndex))
            continue;
        std::size_t const first = candidates.size();
        std::size_t left = SIZE_MAX;
        for (std::uint32_t a = 0; a < objective.areas.size(); ++a)
        {
            AreaPoint const& area = objective.areas[a];
            if (area.mapId != mapId || !AreaUsable(area, team, hasSkill))
                continue;
            placed = true;
            if (WasTried(tried, Key(i, a)))
                continue;
            if (skip.on && std::fabs(area.x - skip.x) < 1.0f && std::fabs(area.y - skip.y) < 1.0f)
                left = candidates.size();
            candidates.push_back({i, a, &area, std::hypot(area.x - x, area.y - y)});
        }
        // The place a stay left is passed over while the objective has another (QuestPlaces::PlaceOf)
        if (left != SIZE_MAX && candidates.size() - first > 1)
            candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(left));
    }
    if (candidates.empty())
        return {placed ? Act::NoTarget : Act::NoPlace};

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](Candidate const& a, Candidate const& b) { return a.yards < b.yards; });
    int const preferred = prefer(static_cast<std::vector<Candidate> const&>(candidates));
    Candidate const& chosen = candidates[preferred >= 0 && static_cast<std::size_t>(preferred) < candidates.size()
                                             ? static_cast<std::size_t>(preferred)
                                             : 0];
    return {Act::Work, WorkOf(quest.objectives[chosen.objective].kind), chosen.objective, chosen.area, chosen.place};
}

// Whether a step being worked still stands: its objective is open and its place not tried. A step that does not
// stand is taken again by Next
template <typename Open>
bool Stands(QuestRecord const& quest, Step const& step, Open open, std::vector<PlaceKey> const& tried)
{
    return step.act == Act::Work && step.objective < quest.objectives.size() &&
           open(quest.objectives[step.objective].poiIndex) && !WasTried(tried, KeyOf(step));
}
}  // namespace QuestExec
}  // namespace Planner

#endif
