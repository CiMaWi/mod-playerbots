/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_RECIPEINDEX_H
#define _PLAYERBOT_RECIPEINDEX_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

// The recipe index (professions plan WR3a, PLAN 3.5): what can be made, from what, at which skill, and how a recipe is
// learned. The server gathers plain rows from the core's stores on the world thread (PlannerRecipes.cpp), Build joins
// them on a planner worker, the result is immutable and published like the upkeep index. Header-only and free of core
// types, so tests/recipe-index-selftest.cpp checks the code the server runs and the dump a server wrote can be read
// back (WA's recipe graph, "what an item is made from", reads this index: family rule 17).
namespace Recipes
{
// What the first useful effect of a profession spell does
enum class Effect : std::uint8_t
{
    Other,    // gathering, fishing, passive and helper spells
    Make,     // creates an item
    Enchant,  // enchants an item
    Process   // disenchants, prospects or mills
};

enum class ProcessKind : std::uint8_t
{
    Disenchant,
    Prospect,
    Mill
};

// SkillLineAbility acquire methods (DBCEnums.h)
constexpr std::uint32_t ACQUIRE_ON_SKILL_VALUE = 1;  // learned at the recipe's minimum skill
constexpr std::uint32_t ACQUIRE_ON_SKILL_LEARN = 2;  // learned with the whole skill

// Prospecting and milling take five of the item, a disenchant one
constexpr std::uint32_t PROSPECT_MILL_COUNT = 5;

struct ItemCount
{
    std::uint32_t item{0};
    std::uint32_t count{0};
};

struct Yield
{
    std::uint32_t item{0};
    double expected{0.0};  // items per cast, loot chances as the store lists them
};

// ---- the gather: plain rows, one list per core store ----
struct AbilityRow  // a SkillLineAbility row of one of the profession skill lines
{
    std::uint16_t skill{0};
    std::uint32_t spell{0};
    std::uint32_t raceMask{0};
    std::uint32_t classMask{0};
    std::uint32_t minRank{0};  // orange
    std::uint32_t yellow{0};   // TrivialSkillLineRankLow
    std::uint32_t grey{0};     // TrivialSkillLineRankHigh
    std::uint32_t acquire{0};
};

struct SpellRow
{
    std::uint32_t spell{0};
    std::string name;
    Effect effect{Effect::Other};
    std::uint32_t product{0};
    std::uint32_t productCount{0};
    std::uint32_t castMs{0};
    std::uint32_t focus{0};  // the station: a spell focus object (forge, anvil, ...)
    std::vector<ItemCount> reagents;
    std::vector<std::uint32_t> tools;           // item ids that must be in the bags
    std::vector<std::uint32_t> toolCategories;  // totem categories the bag must hold an item of
    std::uint32_t taught{0};                    // the spell a LEARN_SPELL effect teaches
    std::uint16_t rankSkill{0};                 // a rank spell: the skill and the cap it raises to
    std::uint32_t rankCap{0};
    std::uint32_t rankStep{0};
};

struct TrainerRow
{
    std::uint32_t trainer{0};  // the first creature entry of the core's trainer
    std::uint32_t listed{0};   // the spell the trainer lists
    std::uint32_t taught{0};   // the spell it teaches (the listed one when it is not a learn spell)
    std::uint32_t copper{0};
    std::uint32_t reqSkill{0};
    std::uint32_t reqRank{0};
    std::uint32_t reqLevel{0};
};

struct RecipeItemRow  // an item whose use teaches a spell
{
    std::uint32_t item{0};
    std::uint32_t taught{0};
    std::uint32_t vendors{0};  // creature entries that sell it
    std::uint32_t buyCopper{0};
};

struct ProcessRow
{
    ProcessKind kind{ProcessKind::Disenchant};
    std::uint32_t source{0};  // the item (prospect, mill) or the disenchant loot id
    std::uint32_t skill{0};
    std::uint32_t rank{0};
    std::uint32_t minLevel{0};  // item levels of the items that share the source
    std::uint32_t maxLevel{0};
    std::uint32_t qualityMask{0};
    std::uint32_t consumed{1};
    std::vector<Yield> yields;
};

struct ItemFact
{
    std::uint32_t id{0};
    bool known{false};  // an item template exists
    std::string name;
    std::uint32_t itemClass{0};
    std::uint32_t subClass{0};
    std::uint32_t inventoryType{0};
    std::uint32_t quality{0};
    std::uint32_t itemLevel{0};
    std::uint32_t requiredLevel{0};
    std::uint32_t allowableClass{0};
    std::uint64_t stats{0};  // its stat profile: bit n set when the item carries a stat of type n (ItemModType, < 64)
};

// A vendor price of a reagent or tool item (WR9b): the item's gold price per buy stack and how many vendors list it
// without a stock limit (they always have it) or with one (not counted on, PLAN 3.6)
struct VendorRow
{
    std::uint32_t item{0};
    std::uint32_t buyCopper{0};  // the price of one buy stack
    std::uint32_t buyCount{1};   // items per buy stack
    std::uint32_t stocked{0};    // vendors that always stock it
    std::uint32_t limited{0};    // vendors with a limited stock
};

// The nodes of one gameobject entry in one zone and what each node gives of one item (WR9b): a mining or herbalism
// node kind, the skill it needs, how many spawns, their mean distance to the nearest other spawn of the entry
struct NodeRow
{
    std::uint32_t zone{0};
    std::uint16_t skill{0};
    std::uint32_t reqSkill{0};
    std::uint32_t entry{0};
    std::uint32_t nodes{0};
    float spacing{0.0f};  // yards
    std::uint32_t item{0};
    double perNode{0.0};  // items one node is expected to give
};

struct Gather
{
    std::vector<AbilityRow> abilities;
    std::vector<SpellRow> spells;
    std::vector<TrainerRow> trainers;
    std::vector<RecipeItemRow> recipeItems;
    std::vector<ProcessRow> processes;
    std::vector<ItemFact> items;
    std::vector<VendorRow> vendors;
    std::vector<NodeRow> nodes;
};

// ---- loot stores: the expected yield of a template ----
struct LootRow
{
    std::uint32_t item{0};
    std::int32_t reference{0};
    float chance{0.0f};  // percent
    std::uint32_t group{0};
    std::uint32_t minCount{1};
    std::uint32_t maxCount{1};
};

using LootTable = std::unordered_map<std::uint32_t, std::vector<LootRow>>;

namespace Detail
{
// A reference names a template of the reference table (the core's reference_loot_template), never of the table the
// row is in; a hand table without its own reference table passes itself
inline void Accumulate(LootTable const& table, LootTable const& references, std::uint32_t entry, double weight,
                       int depth, std::map<std::uint32_t, double>& out)
{
    auto const found = table.find(entry);
    if (found == table.end() || depth > 4)
        return;

    auto const add = [&](LootRow const& row, double share)
    {
        if (row.reference > 0)  // a reference rolls its template max-count times
            Accumulate(references, references, static_cast<std::uint32_t>(row.reference),
                       weight * share * std::max<std::uint32_t>(row.maxCount, 1), depth + 1, out);
        else
            out[row.item] += weight * share * (row.minCount + row.maxCount) / 2.0;
    };

    std::map<std::uint32_t, std::vector<LootRow const*>> groups;
    for (LootRow const& row : found->second)
        if (row.group == 0)
            add(row, std::min(row.chance, 100.0f) / 100.0);
        else
            groups[row.group].push_back(&row);

    // A group yields one of its items: the explicit chances first, the rest shared by the zero-chance items
    for (auto const& [id, rows] : groups)
    {
        (void)id;
        double explicitSum = 0.0;
        std::size_t equal = 0;
        for (LootRow const* row : rows)
            if (row->chance > 0.0f)
                explicitSum += row->chance / 100.0;
            else
                ++equal;
        double const rest = std::max(0.0, 1.0 - explicitSum);
        for (LootRow const* row : rows)
            add(*row, row->chance > 0.0f ? row->chance / 100.0 : (equal ? rest / static_cast<double>(equal) : 0.0));
    }
}
}  // namespace Detail

// The items one cast of the template is expected to give, by item id
inline std::vector<Yield> ExpectedYield(LootTable const& table, std::uint32_t entry,
                                        LootTable const* references = nullptr)
{
    std::map<std::uint32_t, double> sums;
    Detail::Accumulate(table, references ? *references : table, entry, 1.0, 0, sums);
    std::vector<Yield> yields;
    for (auto const& [item, expected] : sums)
        if (expected > 0.0)
            yields.push_back({item, expected});

    return yields;
}

// ---- the index ----
struct RecipeSource
{
    std::uint32_t item{0};
    std::uint32_t vendors{0};
    std::uint32_t buyCopper{0};
};

struct Recipe
{
    std::uint16_t skill{0};
    std::uint32_t spell{0};
    std::string name;
    std::uint32_t minRank{0};  // orange: the skill the recipe is learnable or first craftable at
    std::uint32_t yellow{0};
    std::uint32_t grey{0};
    std::uint32_t raceMask{0};
    std::uint32_t classMask{0};
    std::uint32_t acquire{0};
    Effect effect{Effect::Other};
    std::uint32_t product{0};
    std::uint32_t productCount{0};
    std::uint32_t castMs{0};
    std::uint32_t focus{0};
    std::vector<ItemCount> reagents;
    std::vector<std::uint32_t> tools;
    std::vector<std::uint32_t> toolCategories;
    // How it is learned: the cheapest trainer row (trainers is how many list it) and the items that teach it
    std::uint32_t trainers{0};
    std::uint32_t trainerCopper{0};
    std::uint32_t trainerRank{0};
    std::uint32_t trainerLevel{0};
    std::vector<RecipeSource> items;

    [[nodiscard]] std::uint32_t Green() const { return (yellow + grey) / 2; }
};

// One rung of a profession's rank ladder (Apprentice ... Grand Master)
struct Rung
{
    std::uint16_t skill{0};
    std::uint32_t spell{0};
    std::string name;
    std::uint32_t cap{0};
    std::uint32_t step{0};
    std::uint32_t trainers{0};
    std::uint32_t copper{0};
    std::uint32_t reqSkill{0};
    std::uint32_t reqRank{0};
    std::uint32_t reqLevel{0};
};

struct Process
{
    ProcessKind kind{ProcessKind::Disenchant};
    std::uint32_t source{0};
    std::uint32_t skill{0};
    std::uint32_t rank{0};
    std::uint32_t minLevel{0};
    std::uint32_t maxLevel{0};
    std::uint32_t qualityMask{0};
    std::uint32_t consumed{1};
    std::vector<Yield> yields;
};

// How a recipe is learned, as a word: the skill itself, a trainer, an item (sold or found), or no way found
inline std::string_view HowLearned(Recipe const& recipe)
{
    if (recipe.acquire == ACQUIRE_ON_SKILL_LEARN)
        return "skill";
    if (recipe.acquire == ACQUIRE_ON_SKILL_VALUE && !recipe.trainers && recipe.items.empty())
        return "rank";
    if (recipe.trainers)
        return "trainer";
    if (!recipe.items.empty())
    {
        for (RecipeSource const& source : recipe.items)
            if (source.vendors)
                return "item-vendor";
        return "item-found";
    }

    return "none";
}

inline char const* EffectWord(Effect effect)
{
    switch (effect)
    {
        case Effect::Make:
            return "make";
        case Effect::Enchant:
            return "enchant";
        case Effect::Process:
            return "process";
        default:
            return "other";
    }
}

inline char const* ProcessWord(ProcessKind kind)
{
    return kind == ProcessKind::Disenchant ? "disenchant" : kind == ProcessKind::Prospect ? "prospect" : "mill";
}

class RecipeIndex
{
public:
    static RecipeIndex Build(Gather&& gather)
    {
        RecipeIndex index;
        std::unordered_map<std::uint32_t, SpellRow const*> spells;
        for (SpellRow const& spell : gather.spells)
            spells[spell.spell] = &spell;

        // The cheapest trainer row per taught spell (a learn spell's trainer teaches the spell it learns)
        struct Best
        {
            TrainerRow row;
            std::uint32_t count{0};
        };
        std::unordered_map<std::uint32_t, Best> trainers;
        for (TrainerRow const& row : gather.trainers)
            for (std::uint32_t const key : {row.taught, row.listed == row.taught ? 0u : row.listed})
            {
                if (!key)
                    continue;
                Best& best = trainers[key];
                if (!best.count || row.copper < best.row.copper)
                    best.row = row;
                ++best.count;
            }

        std::unordered_map<std::uint32_t, std::vector<RecipeSource>> items;
        for (RecipeItemRow const& row : gather.recipeItems)
            items[row.taught].push_back({row.item, row.vendors, row.buyCopper});

        for (AbilityRow const& ability : gather.abilities)
        {
            auto const found = spells.find(ability.spell);
            if (found == spells.end())
                continue;
            SpellRow const& spell = *found->second;
            Recipe recipe;
            recipe.skill = ability.skill;
            recipe.spell = ability.spell;
            recipe.name = spell.name;
            recipe.minRank = ability.minRank;
            recipe.yellow = ability.yellow;
            recipe.grey = ability.grey;
            recipe.raceMask = ability.raceMask;
            recipe.classMask = ability.classMask;
            recipe.acquire = ability.acquire;
            recipe.effect = spell.effect;
            recipe.product = spell.product;
            recipe.productCount = spell.productCount;
            recipe.castMs = spell.castMs;
            recipe.focus = spell.focus;
            recipe.reagents = spell.reagents;
            recipe.tools = spell.tools;
            recipe.toolCategories = spell.toolCategories;
            if (auto const trainer = trainers.find(recipe.spell); trainer != trainers.end())
            {
                recipe.trainers = trainer->second.count;
                recipe.trainerCopper = trainer->second.row.copper;
                recipe.trainerRank = trainer->second.row.reqRank;
                recipe.trainerLevel = trainer->second.row.reqLevel;
            }
            if (auto const source = items.find(recipe.spell); source != items.end())
                recipe.items = source->second;
            index._recipes.push_back(std::move(recipe));
        }

        for (SpellRow const& spell : gather.spells)
        {
            if (!spell.rankSkill)
                continue;
            Rung rung;
            rung.skill = spell.rankSkill;
            rung.spell = spell.spell;
            rung.name = spell.name;
            rung.cap = spell.rankCap;
            rung.step = spell.rankStep;
            if (auto const trainer = trainers.find(rung.spell); trainer != trainers.end())
            {
                rung.trainers = trainer->second.count;
                rung.copper = trainer->second.row.copper;
                rung.reqSkill = trainer->second.row.reqSkill;
                rung.reqRank = trainer->second.row.reqRank;
                rung.reqLevel = trainer->second.row.reqLevel;
            }
            index._rungs.push_back(std::move(rung));
        }

        for (ProcessRow const& row : gather.processes)
            index._processes.push_back({row.kind, row.source, row.skill, row.rank, row.minLevel, row.maxLevel,
                                        row.qualityMask, row.consumed, row.yields});
        index._items = std::move(gather.items);
        index._vendors = std::move(gather.vendors);
        index._nodes = std::move(gather.nodes);
        index.Finish();
        return index;
    }

    // Raw records back from a dump (RecipeIndex::Parse)
    static RecipeIndex FromRecords(std::vector<Recipe> recipes, std::vector<Rung> rungs, std::vector<Process> processes,
                                   std::vector<ItemFact> items, std::vector<VendorRow> vendors = {},
                                   std::vector<NodeRow> nodes = {})
    {
        RecipeIndex index;
        index._recipes = std::move(recipes);
        index._rungs = std::move(rungs);
        index._processes = std::move(processes);
        index._items = std::move(items);
        index._vendors = std::move(vendors);
        index._nodes = std::move(nodes);
        index.Finish();
        return index;
    }

    [[nodiscard]] std::vector<Recipe> const& Recipes() const { return _recipes; }
    [[nodiscard]] std::vector<Rung> const& Rungs() const { return _rungs; }
    [[nodiscard]] std::vector<Process> const& Processes() const { return _processes; }
    [[nodiscard]] std::vector<ItemFact> const& Items() const { return _items; }
    [[nodiscard]] std::vector<VendorRow> const& Vendors() const { return _vendors; }
    [[nodiscard]] std::vector<NodeRow> const& Nodes() const { return _nodes; }
    [[nodiscard]] std::size_t Records() const
    {
        return _recipes.size() + _rungs.size() + _processes.size() + _items.size() + _vendors.size() + _nodes.size();
    }

    // The vendor price of an item, or null when no vendor sells it for gold
    [[nodiscard]] VendorRow const* Vendor(std::uint32_t item) const
    {
        auto const found = _vendorAt.find(item);
        return found == _vendorAt.end() ? nullptr : &_vendors[found->second];
    }

    // Positions in Nodes() of the node rows that give an item
    [[nodiscard]] std::vector<std::size_t> const& NodesOf(std::uint32_t item) const
    {
        return Positions(_nodesOf, item);
    }

    // Positions in Processes() of the processes that yield an item
    [[nodiscard]] std::vector<std::size_t> const& YieldedBy(std::uint32_t item) const
    {
        return Positions(_yieldedBy, item);
    }

    // The item facts of an id, or null when it is not in the index
    [[nodiscard]] ItemFact const* Item(std::uint32_t id) const
    {
        auto const found = _itemAt.find(id);
        return found == _itemAt.end() ? nullptr : &_items[found->second];
    }

    // The disenchant process of a disenchant loot id (an item template's DisenchantID), or null (professions WR12)
    [[nodiscard]] Process const* DisenchantOf(std::uint32_t lootId) const
    {
        auto const found = _disenchantAt.find(lootId);
        return found == _disenchantAt.end() ? nullptr : &_processes[found->second];
    }

    // Positions in Recipes() of the recipes that make an item (what an item is made from)
    [[nodiscard]] std::vector<std::size_t> const& MadeBy(std::uint32_t item) const { return Positions(_madeBy, item); }

    // A skill's rank ladder, lowest cap first
    [[nodiscard]] std::vector<Rung> Ladder(std::uint16_t skill) const
    {
        std::vector<Rung> ladder;
        for (Rung const& rung : _rungs)
            if (rung.skill == skill)
                ladder.push_back(rung);
        return ladder;
    }

    // One summary line per profession skill
    [[nodiscard]] std::string Summary(std::uint16_t skill, std::string_view name) const
    {
        std::size_t recipes = 0;
        std::size_t make = 0;
        std::size_t trainer = 0;
        std::size_t item = 0;
        std::size_t rank = 0;
        std::size_t withSkill = 0;
        std::size_t none = 0;
        for (Recipe const& recipe : _recipes)
        {
            if (recipe.skill != skill)
                continue;
            ++recipes;
            make += recipe.effect == Effect::Make;
            std::string_view const how = HowLearned(recipe);
            trainer += how == "trainer";
            item += how == "item-vendor" || how == "item-found";
            rank += how == "rank";
            withSkill += how == "skill";
            none += how == "none";
        }

        std::ostringstream out;
        out << name << " (" << skill << "): " << recipes << " spells, " << make << " make an item; learned: " << trainer
            << " trainer, " << item << " item, " << rank << " at rank, " << withSkill << " with the skill, " << none
            << " no source; ladder";
        for (Rung const& rung : Ladder(skill))
            out << ' ' << rung.name << '=' << rung.cap;
        return out.str();
    }

    // ---- the dump ----
    [[nodiscard]] std::string Dump(std::string const& moduleRev, std::string const& coreRev) const
    {
        std::ostringstream out;
        out << "# recipe-index-dump v3 module " << moduleRev << " core " << coreRev << " records " << Records() << '\n';
        out << "#R\tskill\tspell\tname\tminRank\tyellow\tgrey\traceMask\tclassMask\tacquire\teffect\tproduct\t"
               "productCount\tcastMs\tfocus\treagents\ttools\ttoolCategories\ttrainers\ttrainerCopper\ttrainerRank\t"
               "trainerLevel\titems\n";
        out << "#L\tskill\tspell\tname\tcap\tstep\ttrainers\tcopper\treqSkill\treqRank\treqLevel\n";
        out << "#P\tkind\tsource\tskill\trank\tminLevel\tmaxLevel\tqualityMask\tconsumed\tyields\n";
        out << "#I\tid\tknown\tname\titemClass\tsubClass\tinventoryType\tquality\titemLevel\trequiredLevel\t"
               "allowableClass\tstats\n";
        out << "#V\titem\tbuyCopper\tbuyCount\tstocked\tlimited\n";
        out << "#N\tzone\tskill\treqSkill\tentry\tnodes\tspacing\titem\tperNode\n";
        for (Recipe const& r : _recipes)
        {
            out << "R\t" << r.skill << '\t' << r.spell << '\t' << Clean(r.name) << '\t' << r.minRank << '\t' << r.yellow
                << '\t' << r.grey << '\t' << r.raceMask << '\t' << r.classMask << '\t' << r.acquire << '\t'
                << static_cast<int>(r.effect) << '\t' << r.product << '\t' << r.productCount << '\t' << r.castMs << '\t'
                << r.focus << '\t';
            WriteList(out, r.reagents,
                      [](ItemCount const& c) { return std::to_string(c.item) + ":" + std::to_string(c.count); });
            out << '\t';
            WriteList(out, r.tools, [](std::uint32_t id) { return std::to_string(id); });
            out << '\t';
            WriteList(out, r.toolCategories, [](std::uint32_t id) { return std::to_string(id); });
            out << '\t' << r.trainers << '\t' << r.trainerCopper << '\t' << r.trainerRank << '\t' << r.trainerLevel
                << '\t';
            WriteList(out, r.items,
                      [](RecipeSource const& s)
                      {
                          return std::to_string(s.item) + ":" + std::to_string(s.vendors) + ":" +
                                 std::to_string(s.buyCopper);
                      });
            out << '\n';
        }
        for (Rung const& l : _rungs)
            out << "L\t" << l.skill << '\t' << l.spell << '\t' << Clean(l.name) << '\t' << l.cap << '\t' << l.step
                << '\t' << l.trainers << '\t' << l.copper << '\t' << l.reqSkill << '\t' << l.reqRank << '\t'
                << l.reqLevel << '\n';
        for (Process const& p : _processes)
        {
            out << "P\t" << static_cast<int>(p.kind) << '\t' << p.source << '\t' << p.skill << '\t' << p.rank << '\t'
                << p.minLevel << '\t' << p.maxLevel << '\t' << p.qualityMask << '\t' << p.consumed << '\t';
            WriteList(out, p.yields,
                      [](Yield const& y)
                      {
                          std::ostringstream one;
                          one << y.item << ':' << std::fixed << std::setprecision(4) << y.expected;
                          return one.str();
                      });
            out << '\n';
        }
        for (ItemFact const& i : _items)
            out << "I\t" << i.id << '\t' << (i.known ? 1 : 0) << '\t' << Clean(i.name) << '\t' << i.itemClass << '\t'
                << i.subClass << '\t' << i.inventoryType << '\t' << i.quality << '\t' << i.itemLevel << '\t'
                << i.requiredLevel << '\t' << i.allowableClass << '\t' << i.stats << '\n';
        for (VendorRow const& v : _vendors)
            out << "V\t" << v.item << '\t' << v.buyCopper << '\t' << v.buyCount << '\t' << v.stocked << '\t'
                << v.limited << '\n';
        for (NodeRow const& n : _nodes)
        {
            std::ostringstream numbers;
            numbers << std::fixed << std::setprecision(2) << n.spacing << '\t' << n.item << '\t' << std::setprecision(4)
                    << n.perNode;
            out << "N\t" << n.zone << '\t' << n.skill << '\t' << n.reqSkill << '\t' << n.entry << '\t' << n.nodes
                << '\t' << numbers.str() << '\n';
        }
        return out.str();
    }

    // Reads a dump back; false with the reason in error when a line is malformed
    static bool Parse(std::string const& text, RecipeIndex& index, std::string& error)
    {
        std::vector<Recipe> recipes;
        std::vector<Rung> rungs;
        std::vector<Process> processes;
        std::vector<ItemFact> items;
        std::vector<VendorRow> vendors;
        std::vector<NodeRow> nodes;
        std::istringstream in(text);
        std::string line;
        std::size_t number = 0;
        std::size_t declared = 0;
        bool header = false;
        while (std::getline(in, line))
        {
            ++number;
            if (line.empty())
                continue;
            if (line[0] == '#')
            {
                if (line.rfind("# recipe-index-dump v2", 0) == 0 || line.rfind("# recipe-index-dump v3", 0) == 0)
                {
                    header = true;
                    std::size_t const at = line.rfind("records ");
                    declared = at == std::string::npos ? 0 : std::stoul(line.substr(at + 8));
                }
                continue;
            }
            std::vector<std::string> const f = Split(line, '\t');
            try
            {
                if (f[0] == "R" && f.size() == 23)
                {
                    Recipe r;
                    r.skill = static_cast<std::uint16_t>(U(f[1]));
                    r.spell = U(f[2]);
                    r.name = f[3];
                    r.minRank = U(f[4]);
                    r.yellow = U(f[5]);
                    r.grey = U(f[6]);
                    r.raceMask = U(f[7]);
                    r.classMask = U(f[8]);
                    r.acquire = U(f[9]);
                    r.effect = static_cast<Effect>(U(f[10]));
                    r.product = U(f[11]);
                    r.productCount = U(f[12]);
                    r.castMs = U(f[13]);
                    r.focus = U(f[14]);
                    for (std::string const& part : Split(f[15], ','))
                        if (!part.empty())
                        {
                            std::vector<std::string> const c = Split(part, ':');
                            r.reagents.push_back({U(c.at(0)), U(c.at(1))});
                        }
                    for (std::string const& part : Split(f[16], ','))
                        if (!part.empty())
                            r.tools.push_back(U(part));
                    for (std::string const& part : Split(f[17], ','))
                        if (!part.empty())
                            r.toolCategories.push_back(U(part));
                    r.trainers = U(f[18]);
                    r.trainerCopper = U(f[19]);
                    r.trainerRank = U(f[20]);
                    r.trainerLevel = U(f[21]);
                    for (std::string const& part : Split(f[22], ','))
                        if (!part.empty())
                        {
                            std::vector<std::string> const c = Split(part, ':');
                            r.items.push_back({U(c.at(0)), U(c.at(1)), U(c.at(2))});
                        }
                    recipes.push_back(std::move(r));
                }
                else if (f[0] == "L" && f.size() == 11)
                {
                    Rung l;
                    l.skill = static_cast<std::uint16_t>(U(f[1]));
                    l.spell = U(f[2]);
                    l.name = f[3];
                    l.cap = U(f[4]);
                    l.step = U(f[5]);
                    l.trainers = U(f[6]);
                    l.copper = U(f[7]);
                    l.reqSkill = U(f[8]);
                    l.reqRank = U(f[9]);
                    l.reqLevel = U(f[10]);
                    rungs.push_back(std::move(l));
                }
                else if (f[0] == "P" && f.size() == 10)
                {
                    Process p;
                    p.kind = static_cast<ProcessKind>(U(f[1]));
                    p.source = U(f[2]);
                    p.skill = U(f[3]);
                    p.rank = U(f[4]);
                    p.minLevel = U(f[5]);
                    p.maxLevel = U(f[6]);
                    p.qualityMask = U(f[7]);
                    p.consumed = U(f[8]);
                    for (std::string const& part : Split(f[9], ','))
                        if (!part.empty())
                        {
                            std::vector<std::string> const c = Split(part, ':');
                            p.yields.push_back({U(c.at(0)), std::stod(c.at(1))});
                        }
                    processes.push_back(std::move(p));
                }
                else if (f[0] == "I" && f.size() == 12)
                {
                    ItemFact i;
                    i.id = U(f[1]);
                    i.known = U(f[2]) != 0;
                    i.name = f[3];
                    i.itemClass = U(f[4]);
                    i.subClass = U(f[5]);
                    i.inventoryType = U(f[6]);
                    i.quality = U(f[7]);
                    i.itemLevel = U(f[8]);
                    i.requiredLevel = U(f[9]);
                    i.allowableClass = U(f[10]);
                    i.stats = std::stoull(f[11]);
                    items.push_back(std::move(i));
                }
                else if (f[0] == "V" && f.size() == 6)
                    vendors.push_back({U(f[1]), U(f[2]), U(f[3]), U(f[4]), U(f[5])});
                else if (f[0] == "N" && f.size() == 9)
                    nodes.push_back({U(f[1]), static_cast<std::uint16_t>(U(f[2])), U(f[3]), U(f[4]), U(f[5]),
                                     std::stof(f[6]), U(f[7]), std::stod(f[8])});
                else
                {
                    error = "line " + std::to_string(number) + ": unknown record or field count";
                    return false;
                }
            }
            catch (std::exception const& e)
            {
                error = "line " + std::to_string(number) + ": " + e.what();
                return false;
            }
        }

        std::size_t const total =
            recipes.size() + rungs.size() + processes.size() + items.size() + vendors.size() + nodes.size();
        if (!header || total != declared)
        {
            error = header ? "record count " + std::to_string(total) + " differs from the header's " +
                                 std::to_string(declared)
                           : "no recipe-index-dump v2 or v3 header";
            return false;
        }
        index = FromRecords(std::move(recipes), std::move(rungs), std::move(processes), std::move(items),
                            std::move(vendors), std::move(nodes));
        return true;
    }

private:
    static std::string Clean(std::string text)
    {
        std::replace_if(text.begin(), text.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
        return text;
    }

    template <typename List, typename Format>
    static void WriteList(std::ostringstream& out, List const& list, Format format)
    {
        bool first = true;
        for (auto const& each : list)
        {
            out << (first ? "" : ",") << format(each);
            first = false;
        }
    }

    static std::vector<std::string> Split(std::string const& text, char separator)
    {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (true)
        {
            std::size_t const at = text.find(separator, start);
            if (at == std::string::npos)
            {
                parts.push_back(text.substr(start));
                break;
            }
            parts.push_back(text.substr(start, at - start));
            start = at + 1;
        }
        return parts;
    }

    static std::uint32_t U(std::string const& text) { return static_cast<std::uint32_t>(std::stoul(text)); }

    static std::vector<std::size_t> const& Positions(
        std::unordered_map<std::uint32_t, std::vector<std::size_t>> const& map, std::uint32_t item)
    {
        static std::vector<std::size_t> const none;
        auto const found = map.find(item);
        return found == map.end() ? none : found->second;
    }

    void Finish()
    {
        std::sort(_recipes.begin(), _recipes.end(), [](Recipe const& a, Recipe const& b)
                  { return std::tie(a.skill, a.minRank, a.spell) < std::tie(b.skill, b.minRank, b.spell); });
        std::sort(_rungs.begin(), _rungs.end(), [](Rung const& a, Rung const& b)
                  { return std::tie(a.skill, a.cap, a.spell) < std::tie(b.skill, b.cap, b.spell); });
        std::sort(_processes.begin(), _processes.end(), [](Process const& a, Process const& b)
                  { return std::tie(a.kind, a.source) < std::tie(b.kind, b.source); });
        std::sort(_items.begin(), _items.end(), [](ItemFact const& a, ItemFact const& b) { return a.id < b.id; });
        _itemAt.clear();
        for (std::size_t i = 0; i < _items.size(); ++i)
            _itemAt[_items[i].id] = i;
        _madeBy.clear();
        for (std::size_t i = 0; i < _recipes.size(); ++i)
            if (_recipes[i].effect == Effect::Make && _recipes[i].product)
                _madeBy[_recipes[i].product].push_back(i);
        std::sort(_vendors.begin(), _vendors.end(),
                  [](VendorRow const& a, VendorRow const& b) { return a.item < b.item; });
        _vendorAt.clear();
        for (std::size_t i = 0; i < _vendors.size(); ++i)
            _vendorAt[_vendors[i].item] = i;
        std::sort(_nodes.begin(), _nodes.end(), [](NodeRow const& a, NodeRow const& b)
                  { return std::tie(a.item, a.zone, a.entry) < std::tie(b.item, b.zone, b.entry); });
        _nodesOf.clear();
        for (std::size_t i = 0; i < _nodes.size(); ++i)
            _nodesOf[_nodes[i].item].push_back(i);
        _yieldedBy.clear();
        _disenchantAt.clear();
        for (std::size_t i = 0; i < _processes.size(); ++i)
        {
            for (Yield const& yield : _processes[i].yields)
                _yieldedBy[yield.item].push_back(i);
            if (_processes[i].kind == ProcessKind::Disenchant)
                _disenchantAt.emplace(_processes[i].source, i);
        }
    }

    std::vector<Recipe> _recipes;
    std::vector<Rung> _rungs;
    std::vector<Process> _processes;
    std::vector<ItemFact> _items;
    std::unordered_map<std::uint32_t, std::size_t> _itemAt;
    std::vector<VendorRow> _vendors;
    std::vector<NodeRow> _nodes;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> _madeBy;
    std::unordered_map<std::uint32_t, std::size_t> _vendorAt;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> _nodesOf;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> _yieldedBy;
    std::unordered_map<std::uint32_t, std::size_t> _disenchantAt;
};
}  // namespace Recipes

#endif
