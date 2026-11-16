/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSEINDEX_H
#define _PLAYERBOT_ITEMUSEINDEX_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ItemUseTypes.h"

// The item use index (item-use plan WI2, INTERFACE.md 2.1): one Record per on-use spell of an item template and per
// use spell of an enchantment. Build() runs once on the world thread at startup, after items, spells, enchantments,
// spell groups and vendors are loaded, and reads only those loaded stores (no DB query); the index is immutable
// afterwards and any thread reads it without a lock.
class ItemUseIndex
{
public:
    struct Stats
    {
        std::uint32_t itemRecords{0};
        std::uint32_t enchantRecords{0};
        std::uint32_t cut{0};            // records with more effects than MAX_EFFECTS
        std::uint32_t missingSpells{0};  // spell rows whose spell is not in the spell store
        std::uint32_t buildMs{0};
    };

    // World thread, once; the only writer
    static void Build();
    // The immutable index; empty before Build
    static ItemUseIndex const& Get();

    // The item's own use effects in spell-index order; empty when it has none
    std::span<ItemUse::Record const> OfItem(std::uint32_t itemEntry) const;
    std::span<ItemUse::Record const> OfEnchant(std::uint32_t enchantId) const;
    // A record by its id (its position; stable for the server's run), null when out of range
    ItemUse::Record const* At(std::uint32_t id) const;
    std::uint32_t IdOf(ItemUse::Record const& record) const;
    // Every record: items first by entry, then enchantments by id
    std::span<ItemUse::Record const> All() const { return _records; }
    // The item's or enchantment's name, for prints
    std::string_view NameOf(std::uint32_t id) const;
    Stats const& GetStats() const { return _stats; }

private:
    using Span = std::pair<std::uint32_t, std::uint32_t>;  // first record id, count

    std::span<ItemUse::Record const> SpanOf(std::unordered_map<std::uint32_t, Span> const& spans,
                                            std::uint32_t key) const;

    std::vector<ItemUse::Record> _records;
    std::vector<std::string> _names;  // by record id
    std::unordered_map<std::uint32_t, Span> _items;
    std::unordered_map<std::uint32_t, Span> _enchants;
    Stats _stats;
};

#endif
