/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_UPKEEPINDEX_H
#define _PLAYERBOT_UPKEEPINDEX_H

// The upkeep index (upkeep plan WU2, PLAN 3.3): what a bot buys and from whom, at the game's prices. Built once after
// startup on a planner worker from plain copies of the core's trainer and vendor stores (PlannerUpkeep.cpp gathers
// them on the world thread), immutable afterwards. Trainer spells per trainer type and requirement (the class for
// class trainers), each list sorted by spell id so a snapshot's known spells are a bit set over its positions; vendor
// offers per stock kind. Places come from PlannerIndexes' services by creature entry. Std only, so
// tests/upkeep-index-selftest.cpp builds it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Planner
{
// The core's item classes and fields the stock rules read (static_asserts in PlannerUpkeep.cpp)
namespace UpkeepItem
{
constexpr std::uint32_t CLASS_CONSUMABLE = 0;
constexpr std::uint32_t CLASS_PROJECTILE = 6;
constexpr std::uint32_t CLASS_MISC = 15;
constexpr std::uint32_t SUBCLASS_CONSUMABLE = 0;
constexpr std::uint32_t SUBCLASS_FOOD = 5;
constexpr std::uint32_t SUBCLASS_CONSUMABLE_OTHER = 8;  // rogue poisons
constexpr std::uint32_t SUBCLASS_ARROW = 2;
constexpr std::uint32_t SUBCLASS_BULLET = 3;
constexpr std::uint32_t SUBCLASS_MOUNT = 5;  // of CLASS_MISC
constexpr std::uint32_t CATEGORY_FOOD = 11;  // Spells[0].SpellCategory, as UseItemAction reads it
constexpr std::uint32_t CATEGORY_DRINK = 59;
constexpr std::uint32_t FOOD_TYPE_MAX = 8;  // FoodType 1..8; a pet eats it when its diet has bit FoodType - 1
constexpr std::uint32_t CLASS_MASK_ROGUE = 1u << 3;
constexpr std::uint32_t CLASS_MASK_PLAYABLE = 0x5FF;  // CLASSMASK_ALL_PLAYABLE
}  // namespace UpkeepItem

constexpr std::size_t UPKEEP_CLASS_SLOTS = 12;  // class ids 1..11 (MAX_CLASSES)

enum class UpkeepStock : std::uint8_t
{
    Food,
    Water,
    Ammo,
    Reagent,
    Poison,
    PetFood,
    Mount,
    Count
};
constexpr std::size_t UPKEEP_STOCK_COUNT = static_cast<std::size_t>(UpkeepStock::Count);
constexpr std::array<std::string_view, UPKEEP_STOCK_COUNT> UPKEEP_STOCK_NAMES = {
    "food", "water", "ammo", "reagents", "poisons", "pet food", "mounts"};

constexpr std::uint8_t StockBit(UpkeepStock kind)
{
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(kind));
}

// The core's Trainer::Type, in its order
enum class TrainerKind : std::uint8_t
{
    Class,
    Mount,
    Tradeskill,
    Pet,
    Count
};
constexpr std::size_t TRAINER_KIND_COUNT = static_cast<std::size_t>(TrainerKind::Count);
constexpr std::array<std::string_view, TRAINER_KIND_COUNT> TRAINER_KIND_NAMES = {"class", "mount", "tradeskill", "pet"};

// An item as the stock rules see it: an ItemTemplate's plain fields
struct UpkeepItemFacts
{
    std::uint32_t item{0};
    std::uint32_t itemClass{0};
    std::uint32_t subClass{0};
    std::uint32_t spellCategory{0};  // of Spells[0]
    std::uint32_t foodType{0};
    std::uint32_t allowableClass{0};
    std::uint32_t allowableRace{0};
    std::uint32_t buyPrice{0};  // copper for buyCount items
    std::uint32_t buyCount{1};
    std::uint32_t requiredLevel{0};
    std::uint32_t itemLevel{0};
    std::uint32_t requiredSkill{0};
    std::uint32_t requiredSkillRank{0};
    std::uint32_t requiredFaction{0};
    std::uint32_t requiredFactionRank{0};
};

constexpr std::uint8_t DietBit(std::uint32_t foodType)
{
    return foodType && foodType <= UpkeepItem::FOOD_TYPE_MAX ? static_cast<std::uint8_t>(1u << (foodType - 1)) : 0;
}

// The stock kinds an item counts for, a bit per UpkeepStock; Reagent needs the class reagent list
// (UpkeepIndex::KindsOf)
constexpr std::uint8_t StockKinds(UpkeepItemFacts const& item)
{
    using namespace UpkeepItem;
    std::uint8_t kinds = 0;
    bool const consumable = item.itemClass == CLASS_CONSUMABLE;
    if (consumable && (item.subClass == SUBCLASS_FOOD || item.subClass == SUBCLASS_CONSUMABLE))
    {
        if (item.spellCategory == CATEGORY_FOOD)
            kinds |= StockBit(UpkeepStock::Food);
        else if (item.spellCategory == CATEGORY_DRINK)
            kinds |= StockBit(UpkeepStock::Water);
    }
    if (DietBit(item.foodType))
        kinds |= StockBit(UpkeepStock::PetFood);
    if (item.itemClass == CLASS_PROJECTILE && (item.subClass == SUBCLASS_ARROW || item.subClass == SUBCLASS_BULLET))
        kinds |= StockBit(UpkeepStock::Ammo);
    if (consumable && item.subClass == SUBCLASS_CONSUMABLE_OTHER &&
        (item.allowableClass & CLASS_MASK_PLAYABLE) == CLASS_MASK_ROGUE)
        kinds |= StockBit(UpkeepStock::Poison);
    if (item.itemClass == CLASS_MISC && item.subClass == SUBCLASS_MOUNT)
        kinds |= StockBit(UpkeepStock::Mount);
    return kinds;
}

// A Trainer::Spell's plain fields
struct TrainerSpell
{
    std::uint32_t spellId{0};
    std::uint32_t moneyCost{0};  // copper, the game's price
    std::uint32_t reqSkillLine{0};
    std::uint32_t reqSkillRank{0};
    std::array<std::uint32_t, 3> reqAbility{};
    std::uint8_t reqLevel{0};

    bool operator==(TrainerSpell const& other) const = default;
};

// One core trainer (many creature entries share one); key = the lowest creature entry that uses it
struct TrainerFacts
{
    std::uint32_t key{0};
    TrainerKind kind{TrainerKind::Class};
    std::uint32_t requirement{0};  // the class for class trainers, the race (0 = any) for riding trainers
    std::vector<TrainerSpell> spells;
};

struct VendorItemFacts
{
    std::uint32_t vendorEntry{0};
    std::uint32_t item{0};
    std::uint32_t maxCount{0};  // 0 = unlimited
    std::uint32_t incrTime{0};
    std::uint32_t extendedCost{0};
    bool goldRequired{true};  // VendorItem::IsGoldRequired; false = sold for currency only
};

struct ReagentUse
{
    std::uint32_t item{0};
    std::uint8_t cls{0};  // a class whose trainer spells use it
};

// Plain copies of the core's stores, taken on the world thread; the worker reads only these
struct UpkeepGather
{
    std::vector<TrainerFacts> trainers;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> trainerEntries;  // creature entry, trainer key
    std::vector<VendorItemFacts> vendorItems;
    std::vector<UpkeepItemFacts> items;  // the vendors' items, once each
    std::vector<ReagentUse> reagents;
    std::uint32_t emptyVendors{0};
    std::uint32_t missingItems{0};  // vendor rows whose item has no template
};

// The union of one trainer type and requirement's spells, sorted by spell id; a spell's position is its bit
struct TrainerList
{
    static constexpr std::size_t NOT_FOUND = std::numeric_limits<std::size_t>::max();

    TrainerKind kind{TrainerKind::Class};
    std::uint32_t requirement{0};
    std::vector<TrainerSpell> spells;

    std::size_t Find(std::uint32_t spellId) const
    {
        auto const it =
            std::lower_bound(spells.begin(), spells.end(), spellId,
                             [](TrainerSpell const& spell, std::uint32_t id) { return spell.spellId < id; });
        return it != spells.end() && it->spellId == spellId ? static_cast<std::size_t>(it - spells.begin()) : NOT_FOUND;
    }
};

struct UpkeepTrainer
{
    std::uint32_t key{0};
    TrainerKind kind{TrainerKind::Class};
    std::uint32_t requirement{0};
    std::uint32_t list{0};               // into UpkeepIndex::lists
    std::vector<std::uint32_t> entries;  // the creature entries that teach it, sorted
    std::vector<std::uint16_t> spells;   // the positions in its list it teaches, sorted
};

// One item at one vendor for one stock kind
struct VendorOffer
{
    std::uint32_t vendorEntry{0};
    std::uint32_t item{0};
    std::uint32_t buyPrice{0};  // copper per buyCount items, the game's price
    std::uint32_t buyCount{1};
    std::uint32_t requiredLevel{0};
    std::uint32_t itemLevel{0};
    std::uint32_t classMask{0};  // reagents: the classes whose spells use it; else the item's AllowableClass
    std::uint32_t raceMask{0};   // AllowableRace (mounts by race and faction)
    std::uint32_t requiredSkill{0};
    std::uint32_t requiredSkillRank{0};  // mounts: the riding rank
    std::uint32_t requiredFaction{0};
    std::uint32_t requiredFactionRank{0};
    std::uint8_t diet{0};  // pet food: its diet bit
    bool limited{false};   // the vendor restocks it (maxcount); stock is not tracked here

    // The copper for at least `units` items: whole lots of buyCount
    std::uint64_t PriceFor(std::uint32_t units) const
    {
        std::uint32_t const lot = std::max<std::uint32_t>(buyCount, 1);
        return static_cast<std::uint64_t>((units + lot - 1) / lot) * buyPrice;
    }
};

struct UpkeepStats
{
    std::array<std::uint32_t, TRAINER_KIND_COUNT> trainers{};
    std::array<std::uint32_t, TRAINER_KIND_COUNT> trainerSpells{};  // over the kind's lists
    std::array<std::uint32_t, UPKEEP_CLASS_SLOTS> classSpells{};    // the class list's size per class id
    std::array<std::uint32_t, UPKEEP_STOCK_COUNT> vendors{};        // distinct vendor entries per kind
    std::array<std::uint32_t, UPKEEP_STOCK_COUNT> items{};          // distinct items per kind
    std::array<std::uint32_t, UPKEEP_STOCK_COUNT> offers{};
    std::uint32_t trainerEntries{0};
    std::uint32_t duplicateTrainers{0};
    std::uint32_t orphanEntries{0};   // creature entries naming no gathered trainer
    std::uint32_t spellConflicts{0};  // a spell two trainers of one list teach at different terms; the first is kept
    std::uint32_t reagentItems{0};
    std::uint32_t currencyOnly{0};  // vendor rows sold for currency only: skipped
    std::uint32_t unknownItems{0};
    std::uint32_t emptyVendors{0};
    std::uint32_t missingItems{0};
    std::uint32_t gatherMs{0};
    std::uint32_t buildMs{0};
    std::size_t estimatedBytes{0};
};

class UpkeepIndex
{
public:
    std::uint32_t version{0};             // set by the publisher; a snapshot's bit sets name the index they align to
    std::vector<TrainerList> lists;       // by kind, then requirement
    std::vector<UpkeepTrainer> trainers;  // by key
    std::array<std::vector<VendorOffer>, UPKEEP_STOCK_COUNT> offers;      // by required level, item, vendor
    std::vector<std::pair<std::uint32_t, std::uint32_t>> reagentClasses;  // item, class mask; by item
    UpkeepStats stats;

    static UpkeepIndex Build(UpkeepGather&& input)
    {
        UpkeepIndex index;
        index.stats.emptyVendors = input.emptyVendors;
        index.stats.missingItems = input.missingItems;
        index.BuildTrainers(input);
        index.BuildReagents(input);
        index.BuildOffers(input);
        index.stats.estimatedBytes = index.ApproxBytes();
        return index;
    }

    TrainerList const* List(TrainerKind kind, std::uint32_t requirement) const
    {
        auto const it = std::lower_bound(lists.begin(), lists.end(), std::make_pair(kind, requirement),
                                         [](TrainerList const& list, std::pair<TrainerKind, std::uint32_t> const& key)
                                         { return std::make_pair(list.kind, list.requirement) < key; });
        return it != lists.end() && it->kind == kind && it->requirement == requirement ? &*it : nullptr;
    }

    TrainerList const* ClassList(std::uint8_t cls) const { return List(TrainerKind::Class, cls); }

    // Riding trainers name a race or none (any race)
    TrainerList const* RidingList(std::uint8_t race) const
    {
        TrainerList const* list = List(TrainerKind::Mount, race);
        return list ? list : List(TrainerKind::Mount, 0);
    }

    std::uint32_t ReagentClassMask(std::uint32_t item) const
    {
        auto const it = std::lower_bound(reagentClasses.begin(), reagentClasses.end(), item,
                                         [](std::pair<std::uint32_t, std::uint32_t> const& entry, std::uint32_t id)
                                         { return entry.first < id; });
        return it != reagentClasses.end() && it->first == item ? it->second : 0;
    }

    // StockKinds plus Reagent when the class's trainer spells use the item
    std::uint8_t KindsOf(UpkeepItemFacts const& item, std::uint8_t cls) const
    {
        std::uint8_t kinds = StockKinds(item);
        if (cls && (ReagentClassMask(item.item) & ClassBit(cls)))
            kinds |= StockBit(UpkeepStock::Reagent);
        return kinds;
    }

    std::vector<VendorOffer> const& Offers(UpkeepStock kind) const { return offers[static_cast<std::size_t>(kind)]; }

    std::size_t ApproxBytes() const
    {
        std::size_t bytes = sizeof(UpkeepIndex) + lists.capacity() * sizeof(TrainerList) +
                            trainers.capacity() * sizeof(UpkeepTrainer) +
                            reagentClasses.capacity() * sizeof(std::pair<std::uint32_t, std::uint32_t>);
        for (TrainerList const& list : lists)
            bytes += list.spells.capacity() * sizeof(TrainerSpell);
        for (UpkeepTrainer const& trainer : trainers)
            bytes +=
                trainer.entries.capacity() * sizeof(std::uint32_t) + trainer.spells.capacity() * sizeof(std::uint16_t);
        for (std::vector<VendorOffer> const& kind : offers)
            bytes += kind.capacity() * sizeof(VendorOffer);
        return bytes;
    }

    static constexpr std::uint32_t ClassBit(std::uint8_t cls) { return cls ? 1u << (cls - 1) : 0; }

private:
    void BuildTrainers(UpkeepGather& input)
    {
        std::stable_sort(input.trainers.begin(), input.trainers.end(),
                         [](TrainerFacts const& a, TrainerFacts const& b) { return a.key < b.key; });
        std::vector<TrainerFacts> unique;
        unique.reserve(input.trainers.size());
        for (TrainerFacts& facts : input.trainers)
        {
            if (!unique.empty() && unique.back().key == facts.key)
            {
                ++stats.duplicateTrainers;
                continue;
            }
            if (facts.kind < TrainerKind::Count)
                unique.push_back(std::move(facts));
        }

        // The lists: every spell of the type and requirement, the first trainer's terms kept (trainers by key)
        std::vector<std::pair<std::pair<TrainerKind, std::uint32_t>, TrainerSpell>> all;
        for (TrainerFacts const& facts : unique)
            for (TrainerSpell const& spell : facts.spells)
                all.push_back({{facts.kind, facts.requirement}, spell});
        std::stable_sort(all.begin(), all.end(), [](auto const& a, auto const& b)
                         { return std::tie(a.first, a.second.spellId) < std::tie(b.first, b.second.spellId); });
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            if (lists.empty() || lists.back().kind != all[i].first.first ||
                lists.back().requirement != all[i].first.second)
                lists.push_back({all[i].first.first, all[i].first.second, {}});
            std::vector<TrainerSpell>& spells = lists.back().spells;
            if (!spells.empty() && spells.back().spellId == all[i].second.spellId)
            {
                if (!(spells.back() == all[i].second))
                    ++stats.spellConflicts;
                continue;
            }
            spells.push_back(all[i].second);
        }
        for (TrainerList& list : lists)
        {
            list.spells.shrink_to_fit();
            stats.trainerSpells[static_cast<std::size_t>(list.kind)] += static_cast<std::uint32_t>(list.spells.size());
            if (list.kind == TrainerKind::Class && list.requirement < UPKEEP_CLASS_SLOTS)
                stats.classSpells[list.requirement] = static_cast<std::uint32_t>(list.spells.size());
        }

        std::sort(input.trainerEntries.begin(), input.trainerEntries.end(), [](auto const& a, auto const& b)
                  { return std::tie(a.second, a.first) < std::tie(b.second, b.first); });
        input.trainerEntries.erase(std::unique(input.trainerEntries.begin(), input.trainerEntries.end()),
                                   input.trainerEntries.end());
        std::size_t next = 0;
        trainers.reserve(unique.size());
        for (TrainerFacts const& facts : unique)
        {
            UpkeepTrainer trainer;
            trainer.key = facts.key;
            trainer.kind = facts.kind;
            trainer.requirement = facts.requirement;
            TrainerList const* list = List(facts.kind, facts.requirement);
            trainer.list = list ? static_cast<std::uint32_t>(list - lists.data()) : 0;
            for (TrainerSpell const& spell : facts.spells)
                if (std::size_t const position = list ? list->Find(spell.spellId) : TrainerList::NOT_FOUND;
                    position != TrainerList::NOT_FOUND)
                    trainer.spells.push_back(static_cast<std::uint16_t>(position));
            std::sort(trainer.spells.begin(), trainer.spells.end());
            trainer.spells.erase(std::unique(trainer.spells.begin(), trainer.spells.end()), trainer.spells.end());
            while (next < input.trainerEntries.size() && input.trainerEntries[next].second < facts.key)
            {
                ++stats.orphanEntries;
                ++next;
            }
            for (; next < input.trainerEntries.size() && input.trainerEntries[next].second == facts.key; ++next)
                trainer.entries.push_back(input.trainerEntries[next].first);
            std::sort(trainer.entries.begin(), trainer.entries.end());
            stats.trainerEntries += static_cast<std::uint32_t>(trainer.entries.size());
            ++stats.trainers[static_cast<std::size_t>(trainer.kind)];
            trainers.push_back(std::move(trainer));
        }
        stats.orphanEntries += static_cast<std::uint32_t>(input.trainerEntries.size() - next);
    }

    void BuildReagents(UpkeepGather const& input)
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> uses;
        uses.reserve(input.reagents.size());
        for (ReagentUse const& use : input.reagents)
            if (use.item && use.cls && use.cls < UPKEEP_CLASS_SLOTS)
                uses.push_back({use.item, ClassBit(use.cls)});
        std::sort(uses.begin(), uses.end());
        for (auto const& [item, mask] : uses)
            if (!reagentClasses.empty() && reagentClasses.back().first == item)
                reagentClasses.back().second |= mask;
            else
                reagentClasses.push_back({item, mask});
        reagentClasses.shrink_to_fit();
        stats.reagentItems = static_cast<std::uint32_t>(reagentClasses.size());
    }

    void BuildOffers(UpkeepGather const& input)
    {
        std::unordered_map<std::uint32_t, UpkeepItemFacts const*> items;
        items.reserve(input.items.size());
        for (UpkeepItemFacts const& item : input.items)
            items.emplace(item.item, &item);
        for (VendorItemFacts const& sold : input.vendorItems)
        {
            auto const found = items.find(sold.item);
            if (found == items.end())
            {
                ++stats.unknownItems;
                continue;
            }
            UpkeepItemFacts const& item = *found->second;
            std::uint32_t const reagentMask = ReagentClassMask(item.item);
            std::uint8_t kinds = StockKinds(item);
            if (reagentMask)
                kinds |= StockBit(UpkeepStock::Reagent);
            if (!kinds)
                continue;
            if (!sold.goldRequired)
            {
                ++stats.currencyOnly;
                continue;
            }
            for (std::size_t kind = 0; kind < UPKEEP_STOCK_COUNT; ++kind)
            {
                if (!(kinds & StockBit(static_cast<UpkeepStock>(kind))))
                    continue;
                VendorOffer offer;
                offer.vendorEntry = sold.vendorEntry;
                offer.item = item.item;
                offer.buyPrice = item.buyPrice;
                offer.buyCount = std::max<std::uint32_t>(item.buyCount, 1);
                offer.requiredLevel = item.requiredLevel;
                offer.itemLevel = item.itemLevel;
                offer.classMask =
                    static_cast<UpkeepStock>(kind) == UpkeepStock::Reagent ? reagentMask : item.allowableClass;
                offer.raceMask = item.allowableRace;
                offer.requiredSkill = item.requiredSkill;
                offer.requiredSkillRank = item.requiredSkillRank;
                offer.requiredFaction = item.requiredFaction;
                offer.requiredFactionRank = item.requiredFactionRank;
                offer.diet = DietBit(item.foodType);
                offer.limited = sold.maxCount != 0;
                offers[kind].push_back(offer);
            }
        }
        for (std::size_t kind = 0; kind < UPKEEP_STOCK_COUNT; ++kind)
        {
            std::vector<VendorOffer>& list = offers[kind];
            auto const order = [](VendorOffer const& offer)
            { return std::make_tuple(offer.requiredLevel, offer.item, offer.vendorEntry); };
            std::stable_sort(list.begin(), list.end(),
                             [&order](VendorOffer const& a, VendorOffer const& b) { return order(a) < order(b); });
            list.erase(std::unique(list.begin(), list.end(), [](VendorOffer const& a, VendorOffer const& b)
                                   { return a.item == b.item && a.vendorEntry == b.vendorEntry; }),
                       list.end());
            list.shrink_to_fit();
            std::vector<std::uint32_t> vendors;
            std::vector<std::uint32_t> itemIds;
            for (VendorOffer const& offer : list)
            {
                vendors.push_back(offer.vendorEntry);
                itemIds.push_back(offer.item);
            }
            for (std::vector<std::uint32_t>* ids : {&vendors, &itemIds})
            {
                std::sort(ids->begin(), ids->end());
                ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
            }
            stats.vendors[kind] = static_cast<std::uint32_t>(vendors.size());
            stats.items[kind] = static_cast<std::uint32_t>(itemIds.size());
            stats.offers[kind] = static_cast<std::uint32_t>(list.size());
        }
    }
};

// Bit sets over a trainer list's positions (Snapshot::knownClassSpells, knownRidingSpells)
constexpr std::size_t BITS_PER_WORD = 64;

inline std::vector<std::uint64_t> SpellBits(std::size_t spells)
{
    return std::vector<std::uint64_t>((spells + BITS_PER_WORD - 1) / BITS_PER_WORD, 0);
}

inline void SetSpellBit(std::vector<std::uint64_t>& bits, std::size_t position)
{
    if (position / BITS_PER_WORD < bits.size())
        bits[position / BITS_PER_WORD] |= std::uint64_t{1} << (position % BITS_PER_WORD);
}

inline bool HasSpellBit(std::vector<std::uint64_t> const& bits, std::size_t position)
{
    return position / BITS_PER_WORD < bits.size() &&
           (bits[position / BITS_PER_WORD] >> (position % BITS_PER_WORD)) & 1u;
}
}  // namespace Planner

#endif
