/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTSOURCES_H
#define _PLAYERBOT_QUESTSOURCES_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Where an item comes from (questing plan WT32a, PLAN 3.1): the creatures that drop it, the creatures skinned for it
// and the objects (chests, herbs, veins, fishing holes) that hold it. The world thread copies the loot rows by SQL, as
// the core keeps its loot stores private, and the owners of each loot id; the build here folds the reference loot in.
// No game header: tests/quest-sources-selftest.cpp builds it from synthetic input. The traversal of creature, skinning,
// object and reference loot is adapted from mod-playerbots PR #3030 (LootSourceMgr, head d49d9007, by Avirar).
//
// A source's chance is the probability (0 to 1) that one loot (a kill, a skinning, an opened object) holds the item,
// rolled as LootMgr.cpp does at the default rates: an ungrouped row on its own; a group picks at most one row, its
// explicit chances first and the rest split evenly among its zero-chance rows; a reference is taken at its chance and
// processed MaxCount times. Approximations: ways to one item in one loot count as independent, a group whose chances
// pass 100 % is scaled to 100 %, and rate settings, loot conditions and the duplicate cap are left out.
namespace Planner
{
enum class SourceKind : std::uint8_t
{
    Corpse,  // creature_loot_template through creature_template.lootid
    Skin,    // skinning_loot_template through creature_template.skinloot
    Object,  // gameobject_loot_template through a chest's or fishing hole's loot id
    Count
};
constexpr std::size_t SOURCE_KINDS = static_cast<std::size_t>(SourceKind::Count);
constexpr std::array<char const*, SOURCE_KINDS> SOURCE_KIND_NAMES = {"corpse", "skin", "object"};

inline char const* SourceKindName(SourceKind kind) { return SOURCE_KIND_NAMES[static_cast<std::size_t>(kind)]; }

namespace LootRule
{
constexpr float FULL = 100.0f;             // a chance in percent that always hits
constexpr float LOWEST = 0.000001f;        // the core skips item rows with a smaller chance (LootStoreItem::IsValid)
constexpr std::uint8_t GROUP_LIMIT = 128;  // group ids are 7 bits
}  // namespace LootRule

// A loot table row, as the core reads it: Entry, Item, |Reference|, Chance, GroupId, MinCount, MaxCount, QuestRequired
struct LootRow
{
    std::uint32_t lootId{0};
    std::uint32_t item{0};       // 0 on a reference row
    std::uint32_t reference{0};  // 0 on an item row
    float chance{0.0f};          // percent
    std::uint8_t groupId{0};
    std::uint8_t minCount{1};
    std::uint8_t maxCount{1};  // the rolls of a reference row
    bool questRequired{false};
};

struct LootOwner
{
    SourceKind kind{SourceKind::Corpse};
    std::uint32_t lootId{0};
    std::uint32_t entry{0};  // creature or gameobject template
};

struct SourceSpawn
{
    std::uint32_t entry{0};
    bool isObject{false};
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    // A creature's own facts for the per-bot filters (WT32b): the skill that skins it (a SkillType id; the skinning
    // source only) and the teams it is friendly to, a bit per team (1 << team, 0 Alliance, 1 Horde): those cannot
    // attack it, so it is no place for them. 0 on objects
    std::uint16_t skill{0};
    std::uint8_t friendlyTo{0};
};
constexpr std::uint8_t TEAM_BOTH = 3;  // SourceSpawn::friendlyTo of a creature friendly to both sides

// The world thread's copy (GatherQuests)
struct SourceGather
{
    std::array<std::vector<LootRow>, SOURCE_KINDS> tables;  // by SourceKind
    std::vector<LootRow> references;                        // reference_loot_template
    std::vector<LootOwner> owners;
    std::vector<SourceSpawn> spawns;  // of the owners
};

struct ItemSource
{
    SourceKind kind{SourceKind::Corpse};
    std::uint32_t entry{0};
    float chance{0.0f};  // 0 to 1 per loot, see above
    std::uint8_t minCount{1};
    std::uint8_t maxCount{1};
    bool questOnly{false};  // drops only for a looter on the quest
    std::uint32_t spawns{0};
};

struct SourceStats
{
    std::array<std::uint64_t, SOURCE_KINDS> sources{};  // (item, owner) pairs
    std::uint32_t items{0};                             // items with a source
    std::uint32_t references{0};                        // reference templates expanded
    std::uint32_t rings{0};                             // reference rows that close a ring, ended
    std::uint32_t missing{0};                           // referenced ids without rows, skipped
    std::uint32_t invalid{0};                           // rows the core's load checks drop
    std::uint32_t spawns{0};
    std::uint32_t buildMs{0};
};

namespace SourceDetail
{
// The core's load checks (LootStoreItem::IsValid); the gather already dropped items without a template
inline bool Valid(LootRow const& row)
{
    if (row.groupId >= LootRule::GROUP_LIMIT || !row.minCount)
        return false;
    if (row.chance == 0.0f && !row.groupId)
        return false;
    if (row.reference)
        return true;
    return (row.chance == 0.0f || row.chance >= LootRule::LOWEST) && row.maxCount >= row.minCount;
}

inline auto RowKey(LootRow const& row)
{
    return std::tie(row.lootId, row.groupId, row.item, row.reference, row.chance, row.minCount, row.maxCount,
                    row.questRequired);
}

// An item or a reference of one template with its probability per processing of the template
struct Way
{
    std::uint32_t item{0};
    std::uint32_t reference{0};
    double p{0.0};
    std::uint8_t minCount{1};
    std::uint8_t maxCount{1};
    bool questOnly{false};
};

// The ways of one template's rows, sorted by group
inline std::vector<Way> WaysOf(LootRow const* first, LootRow const* last)
{
    std::vector<Way> ways;
    auto const add = [&ways](LootRow const& row, double p)
    {
        if (p > 0.0)
            // The core ignores QuestRequired on a reference row
            ways.push_back(
                {row.item, row.reference, p, row.minCount, row.maxCount, !row.reference && row.questRequired});
    };
    while (first != last)
    {
        LootRow const* end = first;
        while (end != last && end->groupId == first->groupId)
            ++end;
        if (!first->groupId)
            for (; first != end; ++first)
                add(*first, std::min<double>(first->chance, LootRule::FULL) / LootRule::FULL);
        else
        {
            double explicitSum = 0.0;
            std::size_t equal = 0;
            for (LootRow const* row = first; row != end; ++row)
            {
                if (row->chance > 0.0f)
                    explicitSum += row->chance;
                equal += row->chance == 0.0f ? 1 : 0;
            }
            double const scale = explicitSum > LootRule::FULL ? LootRule::FULL / explicitSum : 1.0;
            double const rest = equal ? std::max(0.0, LootRule::FULL - explicitSum) / LootRule::FULL / equal : 0.0;
            for (; first != end; ++first)
                add(*first, first->chance > 0.0f    ? first->chance * scale / LootRule::FULL
                            : first->chance == 0.0f ? rest
                                                    : 0.0);
        }
    }
    return ways;
}

// A reference processed `rolls` times holds the item at least once
inline double Rolled(double p, std::uint8_t rolls) { return 1.0 - std::pow(1.0 - p, rolls); }

struct Drop
{
    std::uint32_t item{0};
    double p{0.0};
    std::uint8_t minCount{1};
    std::uint8_t maxCount{1};
    bool questOnly{false};
};

// One drop per item: independent ways combine, counts widen, quest-only only when every way is
inline void Fold(std::vector<Drop>& drops)
{
    std::sort(drops.begin(), drops.end(),
              [](Drop const& a, Drop const& b)
              {
                  return std::tie(a.item, a.p, a.minCount, a.maxCount, a.questOnly) <
                         std::tie(b.item, b.p, b.minCount, b.maxCount, b.questOnly);
              });
    std::vector<Drop> folded;
    for (Drop const& drop : drops)
    {
        if (folded.empty() || folded.back().item != drop.item)
        {
            folded.push_back(drop);
            continue;
        }
        Drop& into = folded.back();
        into.p = 1.0 - (1.0 - into.p) * (1.0 - drop.p);
        into.minCount = std::min(into.minCount, drop.minCount);
        into.maxCount = std::max(into.maxCount, drop.maxCount);
        into.questOnly = into.questOnly && drop.questOnly;
    }
    drops = std::move(folded);
}

// Reference templates to drops, with a ring guard along the path: a reference reached twice from different branches is
// no ring and counts both times
class Expander
{
public:
    explicit Expander(std::vector<LootRow> const& rows) : _rows(rows)
    {
        for (std::size_t i = 0; i < rows.size();)
        {
            std::size_t j = i;
            while (j < rows.size() && rows[j].lootId == rows[i].lootId)
                ++j;
            _ranges[rows[i].lootId] = {i, j};
            i = j;
        }
    }

    bool Has(std::uint32_t reference) const { return _ranges.count(reference) != 0; }

    std::vector<Drop> Drops(std::uint32_t reference)
    {
        bool cut = false;
        return Expand(reference, cut);
    }

    void Missing(std::uint32_t reference) { _missing.insert(reference); }

    std::uint32_t Expanded() const { return static_cast<std::uint32_t>(_expanded.size()); }
    std::uint32_t Rings() const { return static_cast<std::uint32_t>(_rings.size()); }
    std::uint32_t MissingCount() const { return static_cast<std::uint32_t>(_missing.size()); }

private:
    std::vector<LootRow> const& _rows;
    std::unordered_map<std::uint32_t, std::pair<std::size_t, std::size_t>> _ranges;
    std::unordered_map<std::uint32_t, std::vector<Drop>> _done;  // expansions no ring cut short
    std::vector<std::uint32_t> _path;
    std::unordered_set<std::uint32_t> _expanded;
    std::set<std::pair<std::uint32_t, std::uint32_t>> _rings;  // (reference, the one on the path it names)
    std::set<std::uint32_t> _missing;

    // cut: a ring ended below, so the result depends on the path and is not kept
    std::vector<Drop> Expand(std::uint32_t reference, bool& cut)
    {
        auto const done = _done.find(reference);
        if (done != _done.end())
            return done->second;
        auto const range = _ranges.find(reference);
        _expanded.insert(reference);
        _path.push_back(reference);
        std::vector<Drop> drops;
        bool cutHere = false;
        for (Way const& way : WaysOf(_rows.data() + range->second.first, _rows.data() + range->second.second))
        {
            if (!way.reference)
            {
                drops.push_back({way.item, way.p, way.minCount, way.maxCount, way.questOnly});
                continue;
            }
            if (!Has(way.reference))
            {
                _missing.insert(way.reference);
                continue;
            }
            if (std::find(_path.begin(), _path.end(), way.reference) != _path.end())
            {
                _rings.insert({reference, way.reference});
                cutHere = true;
                continue;
            }
            for (Drop const& inner : Expand(way.reference, cutHere))
                drops.push_back({inner.item, way.p * Rolled(inner.p, way.maxCount), inner.minCount, inner.maxCount,
                                 inner.questOnly});
        }
        _path.pop_back();
        Fold(drops);
        if (!cutHere)
            _done[reference] = drops;
        cut = cut || cutHere;
        return drops;
    }
};
}  // namespace SourceDetail

// Immutable once built: the loot templates' own item rows, their reference rows and the references' drops, joined to
// the owners per lookup. Expanding every reference into every owner would take millions of rows (world drops)
class SourceTable
{
public:
    SourceStats stats;

    static SourceTable Build(SourceGather input)
    {
        auto const start = std::chrono::steady_clock::now();
        SourceTable table;
        auto const prepare = [&table](std::vector<LootRow>& rows)
        {
            std::size_t const before = rows.size();
            rows.erase(
                std::remove_if(rows.begin(), rows.end(), [](LootRow const& row) { return !SourceDetail::Valid(row); }),
                rows.end());
            table.stats.invalid += static_cast<std::uint32_t>(before - rows.size());
            std::sort(rows.begin(), rows.end(), [](LootRow const& a, LootRow const& b)
                      { return SourceDetail::RowKey(a) < SourceDetail::RowKey(b); });
        };
        prepare(input.references);
        table._owners = std::move(input.owners);
        std::sort(table._owners.begin(), table._owners.end(), [](LootOwner const& a, LootOwner const& b)
                  { return std::tie(a.kind, a.lootId, a.entry) < std::tie(b.kind, b.lootId, b.entry); });
        table._owners.erase(
            std::unique(table._owners.begin(), table._owners.end(), [](LootOwner const& a, LootOwner const& b)
                        { return a.kind == b.kind && a.lootId == b.lootId && a.entry == b.entry; }),
            table._owners.end());
        table._owners.shrink_to_fit();

        SourceDetail::Expander expander(input.references);
        std::unordered_map<std::uint32_t, std::vector<SourceDetail::Drop>> used;  // references a loot table names
        std::unordered_set<std::uint32_t> items;
        for (std::size_t k = 0; k < SOURCE_KINDS; ++k)
        {
            SourceKind const kind = static_cast<SourceKind>(k);
            std::vector<LootRow>& rows = input.tables[k];
            prepare(rows);
            for (std::size_t i = 0; i < rows.size();)
            {
                std::size_t j = i;
                while (j < rows.size() && rows[j].lootId == rows[i].lootId)
                    ++j;
                std::uint32_t const lootId = rows[i].lootId;
                std::vector<std::uint32_t> held;  // for the statistics
                for (SourceDetail::Way const& way : SourceDetail::WaysOf(rows.data() + i, rows.data() + j))
                {
                    if (!way.reference)
                    {
                        table._direct.push_back({way.item, kind, lootId, static_cast<float>(way.p), way.minCount,
                                                 way.maxCount, way.questOnly});
                        held.push_back(way.item);
                        continue;
                    }
                    if (!expander.Has(way.reference))
                    {
                        expander.Missing(way.reference);
                        continue;
                    }
                    auto [drops, added] = used.try_emplace(way.reference);
                    if (added)
                        drops->second = expander.Drops(way.reference);
                    table._refUses.push_back({way.reference, kind, lootId, static_cast<float>(way.p), way.maxCount});
                    for (SourceDetail::Drop const& drop : drops->second)
                        held.push_back(drop.item);
                }
                std::uint64_t const owners = static_cast<std::uint64_t>(table.OwnersOf(kind, lootId).size());
                if (owners)
                {
                    std::sort(held.begin(), held.end());
                    held.erase(std::unique(held.begin(), held.end()), held.end());
                    table.stats.sources[k] += held.size() * owners;
                    items.insert(held.begin(), held.end());
                }
                i = j;
            }
        }
        for (auto const& [reference, drops] : used)
            for (SourceDetail::Drop const& drop : drops)
                table._refDrops.push_back(
                    {drop.item, reference, static_cast<float>(drop.p), drop.minCount, drop.maxCount, drop.questOnly});

        std::sort(table._direct.begin(), table._direct.end(),
                  [](Direct const& a, Direct const& b)
                  {
                      return std::tie(a.item, a.kind, a.lootId, a.p, a.minCount, a.maxCount, a.questOnly) <
                             std::tie(b.item, b.kind, b.lootId, b.p, b.minCount, b.maxCount, b.questOnly);
                  });
        std::sort(table._refUses.begin(), table._refUses.end(),
                  [](RefUse const& a, RefUse const& b)
                  {
                      return std::tie(a.reference, a.kind, a.lootId, a.p, a.rolls) <
                             std::tie(b.reference, b.kind, b.lootId, b.p, b.rolls);
                  });
        std::sort(table._refDrops.begin(), table._refDrops.end(),
                  [](RefDrop const& a, RefDrop const& b)
                  {
                      return std::tie(a.item, a.reference, a.p, a.minCount, a.maxCount, a.questOnly) <
                             std::tie(b.item, b.reference, b.p, b.minCount, b.maxCount, b.questOnly);
                  });
        table._direct.shrink_to_fit();
        table._refUses.shrink_to_fit();
        table._refDrops.shrink_to_fit();

        table._spawns = std::move(input.spawns);
        std::sort(table._spawns.begin(), table._spawns.end(), SpawnLess);
        table._spawns.erase(
            std::unique(table._spawns.begin(), table._spawns.end(), [](SourceSpawn const& a, SourceSpawn const& b)
                        { return !SpawnLess(a, b) && !SpawnLess(b, a); }),
            table._spawns.end());
        table._spawns.shrink_to_fit();

        table.stats.items = static_cast<std::uint32_t>(items.size());
        table.stats.references = expander.Expanded();
        table.stats.rings = expander.Rings();
        table.stats.missing = expander.MissingCount();
        table.stats.spawns = static_cast<std::uint32_t>(table._spawns.size());
        table.stats.buildMs = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        return table;
    }

    // The sources of the item, by kind and entry
    std::vector<ItemSource> SourcesOf(std::uint32_t item) const
    {
        struct Hit
        {
            SourceKind kind;
            std::uint32_t lootId;
            double p;
            std::uint8_t minCount;
            std::uint8_t maxCount;
            bool questOnly;
        };
        std::vector<Hit> hits;
        auto const [directFirst, directLast] =
            std::equal_range(_direct.begin(), _direct.end(), item, ItemLess<Direct>{});
        for (auto it = directFirst; it != directLast; ++it)
            hits.push_back({it->kind, it->lootId, it->p, it->minCount, it->maxCount, it->questOnly});
        auto const [dropFirst, dropLast] =
            std::equal_range(_refDrops.begin(), _refDrops.end(), item, ItemLess<RefDrop>{});
        for (auto drop = dropFirst; drop != dropLast; ++drop)
        {
            auto const [useFirst, useLast] =
                std::equal_range(_refUses.begin(), _refUses.end(), drop->reference, ReferenceLess{});
            for (auto use = useFirst; use != useLast; ++use)
                hits.push_back({use->kind, use->lootId, use->p * SourceDetail::Rolled(drop->p, use->rolls),
                                drop->minCount, drop->maxCount, drop->questOnly});
        }
        std::sort(hits.begin(), hits.end(),
                  [](Hit const& a, Hit const& b)
                  {
                      return std::tie(a.kind, a.lootId, a.p, a.minCount, a.maxCount, a.questOnly) <
                             std::tie(b.kind, b.lootId, b.p, b.minCount, b.maxCount, b.questOnly);
                  });
        std::vector<ItemSource> sources;
        for (std::size_t i = 0; i < hits.size();)
        {
            Hit fold = hits[i];
            double miss = 1.0 - fold.p;
            std::size_t j = i + 1;
            for (; j < hits.size() && hits[j].kind == fold.kind && hits[j].lootId == fold.lootId; ++j)
            {
                miss *= 1.0 - hits[j].p;
                fold.minCount = std::min(fold.minCount, hits[j].minCount);
                fold.maxCount = std::max(fold.maxCount, hits[j].maxCount);
                fold.questOnly = fold.questOnly && hits[j].questOnly;
            }
            for (LootOwner const& owner : OwnersOf(fold.kind, fold.lootId))
            {
                auto const [first, last] = SpawnsOf(owner.entry, fold.kind == SourceKind::Object);
                sources.push_back({fold.kind, owner.entry, static_cast<float>(1.0 - miss), fold.minCount, fold.maxCount,
                                   fold.questOnly, static_cast<std::uint32_t>(last - first)});
            }
            i = j;
        }
        std::sort(sources.begin(), sources.end(), [](ItemSource const& a, ItemSource const& b)
                  { return std::tie(a.kind, a.entry) < std::tie(b.kind, b.entry); });
        return sources;
    }

    // The spawns of a creature or object that is a source, by map and position
    std::pair<std::vector<SourceSpawn>::const_iterator, std::vector<SourceSpawn>::const_iterator> SpawnsOf(
        std::uint32_t entry, bool isObject) const
    {
        SourceSpawn const key{entry, isObject, 0, 0.0f, 0.0f, 0.0f};
        return std::equal_range(_spawns.begin(), _spawns.end(), key, [](SourceSpawn const& a, SourceSpawn const& b)
                                { return std::tie(a.isObject, a.entry) < std::tie(b.isObject, b.entry); });
    }

    std::uint64_t EstimateBytes() const
    {
        return Bytes(_direct) + Bytes(_refUses) + Bytes(_refDrops) + Bytes(_owners) + Bytes(_spawns);
    }

private:
    struct Direct
    {
        std::uint32_t item;
        SourceKind kind;
        std::uint32_t lootId;
        float p;
        std::uint8_t minCount;
        std::uint8_t maxCount;
        bool questOnly;
    };

    struct RefUse
    {
        std::uint32_t reference;
        SourceKind kind;
        std::uint32_t lootId;
        float p;  // the reference is taken
        std::uint8_t rolls;
    };

    struct RefDrop
    {
        std::uint32_t item;
        std::uint32_t reference;
        float p;  // per processing of the reference, nested references folded in
        std::uint8_t minCount;
        std::uint8_t maxCount;
        bool questOnly;
    };

    template <typename Row>
    struct ItemLess
    {
        bool operator()(Row const& row, std::uint32_t item) const { return row.item < item; }
        bool operator()(std::uint32_t item, Row const& row) const { return item < row.item; }
    };

    struct ReferenceLess
    {
        bool operator()(RefUse const& use, std::uint32_t reference) const { return use.reference < reference; }
        bool operator()(std::uint32_t reference, RefUse const& use) const { return reference < use.reference; }
    };

    std::vector<Direct> _direct;       // by item
    std::vector<RefUse> _refUses;      // by reference
    std::vector<RefDrop> _refDrops;    // by item
    std::vector<LootOwner> _owners;    // by kind, loot id, entry
    std::vector<SourceSpawn> _spawns;  // by kind, entry, place

    static bool SpawnLess(SourceSpawn const& a, SourceSpawn const& b)
    {
        return std::tie(a.isObject, a.entry, a.mapId, a.x, a.y, a.z) <
               std::tie(b.isObject, b.entry, b.mapId, b.x, b.y, b.z);
    }

    template <typename T>
    static std::uint64_t Bytes(std::vector<T> const& v)
    {
        return static_cast<std::uint64_t>(v.capacity()) * sizeof(T);
    }

    struct OwnerRange
    {
        std::vector<LootOwner>::const_iterator first;
        std::vector<LootOwner>::const_iterator last;
        std::vector<LootOwner>::const_iterator begin() const { return first; }
        std::vector<LootOwner>::const_iterator end() const { return last; }
        std::size_t size() const { return static_cast<std::size_t>(last - first); }
    };

    OwnerRange OwnersOf(SourceKind kind, std::uint32_t lootId) const
    {
        auto const [first, last] = std::equal_range(
            _owners.begin(), _owners.end(), LootOwner{kind, lootId, 0}, [](LootOwner const& a, LootOwner const& b)
            { return std::tie(a.kind, a.lootId) < std::tie(b.kind, b.lootId); });
        return {first, last};
    }
};
}  // namespace Planner

#endif
