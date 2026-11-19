/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTPOIZSTORE_H
#define _PLAYERBOT_QUESTPOIZSTORE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

// The baked floors of quest POI points (`playerbots_quest_poi_z`, WT33a's bake against the navmesh dataset the server
// runs). Adapted from mod-playerbots PR #3031 (head 52a77ddc, Avirar): its loader, statement and 16-bit reads of
// PoiIdx and MapId. WT33b: the quest index's gather reads it once on the world thread, before the index is built; it
// is not kept afterwards, so no map thread reads it
class QuestPoiZStore
{
public:
    // World thread, once at startup. An empty table means no floor for every lookup (one log line)
    void Load();

    struct Candidate
    {
        float z{0.0f};
        std::uint16_t pointIdx{0};  // quest_poi_points.Idx2
        std::uint16_t mapId{0};
        std::uint8_t rank{1};
        std::uint8_t confidence{0};
        bool navmeshOk{false};
        bool trigger{false};  // the baker's evidence names an area trigger
    };

    // Load's two halves, engine-free so tests/quest-places-selftest.cpp runs them: one table row, then the order once
    // every row is in
    void Add(std::uint32_t questId, std::uint32_t poiId, Candidate const& candidate)
    {
        _candidates[Key(questId, poiId)].push_back(candidate);
    }

    // Rank first; at equal rank the baker's confidence, then trigger evidence, so a surface row does not beat the
    // underground floor of the same point
    void Order()
    {
        for (auto& [key, list] : _candidates)
            std::stable_sort(list.begin(), list.end(),
                             [](Candidate const& a, Candidate const& b)
                             {
                                 if (a.rank != b.rank)
                                     return a.rank < b.rank;
                                 if (a.confidence != b.confidence)
                                     return a.confidence > b.confidence;
                                 return a.trigger && !b.trigger;
                             });
    }

    // The floor of one POI point on the map: the best candidate the baker found walkable there (rank first, then
    // confidence, then trigger evidence). None when the point is not baked, or only for another map or off the mesh
    std::optional<float> Floor(std::uint32_t questId, std::uint32_t poiId, std::uint32_t pointIdx,
                               std::uint32_t mapId) const
    {
        auto const it = _candidates.find(Key(questId, poiId));
        if (it == _candidates.end())
            return std::nullopt;
        // An invalid floor (off the mesh, another map) is the same as none
        for (Candidate const& candidate : it->second)
            if (candidate.pointIdx == pointIdx && candidate.mapId == mapId && candidate.navmeshOk)
                return candidate.z;
        return std::nullopt;
    }

    std::size_t Pois() const { return _candidates.size(); }

private:
    static std::uint64_t Key(std::uint32_t questId, std::uint32_t poiId)
    {
        return (static_cast<std::uint64_t>(questId) << 32) | poiId;
    }

    std::unordered_map<std::uint64_t, std::vector<Candidate>> _candidates;  // (quest, POI id), best first
};

#endif
