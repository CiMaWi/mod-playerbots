/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGENAMEINDEX_H
#define PLAYERBOTS_BRIDGENAMEINDEX_H

// LC18b: the whole world's item and quest names by exact key, for the `what` topic. Built once at startup from the
// loaded item and quest templates (BridgeNameIndex.cpp) and never changed after, so the world thread reads it without a
// lock. Its own index: the startup key readers of Population.cpp and RandomBotLevelMgr.cpp stay as they are. The data
// structure is Player-free so a selftest includes it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "BridgeNames.h"

namespace PlayerbotBridge
{
// Names by NameKey (letters and digits, lower case), the lowest id of two of one name; an exact key only, never a part
class NameKeyIndex
{
public:
    void Add(std::string_view name, std::uint32_t id)
    {
        std::string key = NameKey(name);
        if (key.empty() || !id)
            return;
        auto const found = _ids.find(key);
        if (found == _ids.end())
        {
            _bytes += key.size() + ENTRY_BYTES;
            _ids.emplace(std::move(key), id);
        }
        else if (id < found->second)
            found->second = id;
    }

    // The id of the name's key; 0 for none (also for an empty name or a part of a name)
    std::uint32_t Find(std::string_view name) const
    {
        std::string const key = NameKey(name);
        auto const found = key.empty() ? _ids.end() : _ids.find(key);
        return found == _ids.end() ? 0 : found->second;
    }

    std::size_t Size() const { return _ids.size(); }
    std::size_t EstimatedBytes() const { return _bytes; }

private:
    static constexpr std::size_t ENTRY_BYTES = 64;  // node, string header and id, roughly
    std::unordered_map<std::string, std::uint32_t> _ids;
    std::size_t _bytes{0};
};

struct NameIndex
{
    NameKeyIndex items;
    NameKeyIndex quests;
    std::uint32_t buildMs{0};
};

// World thread, once at startup after the item and quest templates are loaded: builds the index and logs its size and
// build time. Idempotent.
void StartNameIndex();

// Null until StartNameIndex has finished; the index never changes after.
NameIndex const* NameIndexNow();
}  // namespace PlayerbotBridge

#endif
