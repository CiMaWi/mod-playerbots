/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_POPULATIONROSTER_H
#define _PLAYERBOT_POPULATIONROSTER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// The random bot characters on the roster accounts, sorted by guid (no game types: the offline harness
// tests/population-selftest.cpp checks it). Population owns the one instance and keeps it equal to the characters
// table: one load, then an insert per confirmed new character and an erase per deleted one.
class PopulationRoster
{
public:
    struct Entry
    {
        uint32_t guid;
        uint32_t account;
        uint8_t race;
        uint8_t classId;
    };

    // Replaces the content; a guid listed twice is kept once
    void Load(std::vector<Entry> entries)
    {
        std::sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) { return a.guid < b.guid; });
        entries.erase(std::unique(entries.begin(), entries.end(),
                                  [](Entry const& a, Entry const& b) { return a.guid == b.guid; }),
                      entries.end());
        _entries = std::move(entries);
    }

    // False when the guid is there already (nothing changes)
    bool Insert(Entry const& entry)
    {
        auto const at = Find(entry.guid);
        if (at != _entries.end() && at->guid == entry.guid)
            return false;

        _entries.insert(at, entry);
        return true;
    }

    // False when the guid is not there
    bool Erase(uint32_t guid)
    {
        auto const at = Find(guid);
        if (at == _entries.end() || at->guid != guid)
            return false;

        _entries.erase(at);
        return true;
    }

    bool Contains(uint32_t guid) const
    {
        auto const at = Find(guid);
        return at != _entries.end() && at->guid == guid;
    }

    std::size_t Size() const { return _entries.size(); }
    bool Empty() const { return _entries.empty(); }
    std::vector<Entry> const& Entries() const { return _entries; }

private:
    std::vector<Entry>::iterator Find(uint32_t guid)
    {
        return std::lower_bound(_entries.begin(), _entries.end(), guid,
                                [](Entry const& entry, uint32_t value) { return entry.guid < value; });
    }

    std::vector<Entry>::const_iterator Find(uint32_t guid) const
    {
        return std::lower_bound(_entries.begin(), _entries.end(), guid,
                                [](Entry const& entry, uint32_t value) { return entry.guid < value; });
    }

    std::vector<Entry> _entries;
};

#endif
