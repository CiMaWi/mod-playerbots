/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_RECIPEINDEX_H
#define _PLAYERBOT_RECIPEINDEX_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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

// The spell a processing kind casts on an item (Disenchant, Prospecting, Milling)
constexpr std::uint32_t ProcessSpell(ProcessKind kind)
{
    return kind == ProcessKind::Disenchant ? 13262u : kind == ProcessKind::Prospect ? 31252u : 51005u;
}

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
    std::array<std::uint32_t, 3> reqAbility{};  // spells the learner must know (Trainer::Spell::ReqAbility, WR.27)
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
    std::uint32_t saleCopper{0};  // disenchant: the median vendor sale of the items that share the source
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

// ---- drop rows (WR20a): creatures as sources of a material ----
enum class DropKind : std::uint8_t
{
    Corpse,  // looted from the corpse
    Skin     // skinned (or mined or picked) from the corpse
};

// One creature kind in one zone as a source of one item: its open-world spawn points and their spacing, the
// creatures' level range and elite share, the sides it is friendly to, the units one kill is expected to give and,
// for a skin, the skill line that takes it. Built once from questing's source index (SourcesOf, SpawnsOf: WR.9, family
// rule 17) and the core's creature templates and spawns; instance maps, creatures friendly to both sides and sources
// under the chance floor are left out
struct DropRow
{
    std::uint32_t zone{0};
    DropKind kind{DropKind::Corpse};
    std::uint32_t entry{0};  // the creature template
    std::uint32_t item{0};
    std::uint32_t spawns{0};
    float spacing{0.0f};  // yards: the mean distance from a spawn to the nearest other spawn of the kind in the zone
    std::uint32_t minLevel{0};
    std::uint32_t maxLevel{0};
    float eliteShare{0.0f};        // 0 to 1
    std::uint8_t friendlyTo{0};    // a bit per side (1 << team) that cannot attack it
    std::uint16_t skill{0};        // a skin: the skill line that takes it (the level it asks is the creature's)
    double perKill{0.0};           // units one kill is expected to give: chance x mean count
    std::uint32_t respawnSecs{0};  // the spawns' mean respawn time
};

// The core's facts of a creature kind with loot (the world thread's gather)
struct DropCreature
{
    std::uint32_t entry{0};
    std::uint32_t minLevel{0};
    std::uint32_t maxLevel{0};
    bool elite{false};  // elite, rare elite or boss rank
};

// One open-world spawn point of a creature kind with loot, its zone resolved on the world thread. The rows' spawns
// stay beside the rows in the index for the farm circuits (WR20b/c), never in the dump; a stop is (creature, spawnId)
struct DropSpawn
{
    std::uint32_t spawnId{0};  // the creature table's guid
    std::uint32_t entry{0};
    std::uint32_t zone{0};
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::uint32_t respawnSecs{0};
};

struct DropWorld
{
    std::vector<DropCreature> creatures;
    std::vector<DropSpawn> spawns;  // open-world maps only, normal phase, no game event spawns
};

// One source of an item as the source index gives it (kind, creature, chance per loot, counts, quest-only) with the
// creature's skin skill and friendly sides from its spawns
struct DropSource
{
    DropKind kind{DropKind::Corpse};
    std::uint32_t entry{0};
    float chance{0.0f};  // 0 to 1 per loot
    std::uint8_t minCount{1};
    std::uint8_t maxCount{1};
    bool questOnly{false};
    std::uint16_t skill{0};
    std::uint8_t friendlyTo{0};
};

constexpr std::uint8_t DROP_BOTH_SIDES = 3;  // DropSource::friendlyTo of a creature friendly to both sides
constexpr float LONE_SPAWN_YARDS = 300.0f;   // [E] the spacing of a creature kind with one spawn in its zone

// What the drop build kept and left out, for the startup line
struct DropCounts
{
    std::uint32_t items{0};        // items with a row
    std::uint32_t creatures{0};    // creature kinds with a row
    std::uint32_t zones{0};        // zones with a row
    std::uint32_t underFloor{0};   // sources left out: under the chance floor
    std::uint32_t questOnly{0};    // sources left out: drop only for a looter on the quest
    std::uint32_t bothSides{0};    // sources left out: friendly to both sides
    std::uint32_t noOpenSpawn{0};  // sources left out: no open-world spawn (instances, other phases, events)
};

struct DropBuild
{
    std::vector<DropRow> rows;
    std::vector<DropSpawn> spawns;  // the spawns of the rows' creature kinds
    DropCounts counts;
};

// The mean distance from each point to the nearest other point (LONE_SPAWN_YARDS for one point)
template <typename Point>
float MeanNearestYards(std::vector<Point const*> const& group)
{
    if (group.size() < 2)
        return LONE_SPAWN_YARDS;
    double sum = 0.0;
    for (Point const* point : group)
    {
        double nearest = -1.0;
        for (Point const* other : group)
            if (other != point)
            {
                double const dx = point->x - other->x;
                double const dy = point->y - other->y;
                double const dz = point->z - other->z;
                double const distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (nearest < 0.0 || distance < nearest)
                    nearest = distance;
            }
        sum += nearest;
    }
    return static_cast<float>(sum / static_cast<double>(group.size()));
}

// The drop rows of the items (WR20a). sourcesOf(item) gives the item's sources (no objects); a source is left out
// under the floor, quest-only, friendly to both sides or without an open-world spawn of its creature
template <typename SourcesOf>
DropBuild BuildDrops(std::vector<std::uint32_t> items, SourcesOf const& sourcesOf, DropWorld const& world, float floor)
{
    DropBuild build;
    std::unordered_map<std::uint32_t, DropCreature const*> creatures;
    for (DropCreature const& creature : world.creatures)
        creatures[creature.entry] = &creature;
    // The spawns of a creature kind by zone, once per kind
    struct Place
    {
        std::uint32_t zone{0};
        std::uint32_t spawns{0};
        float spacing{0.0f};
        std::uint32_t respawnSecs{0};
    };
    std::unordered_map<std::uint32_t, std::vector<DropSpawn const*>> spawnsOf;
    for (DropSpawn const& spawn : world.spawns)
        spawnsOf[spawn.entry].push_back(&spawn);
    std::unordered_map<std::uint32_t, std::vector<Place>> placesOf;
    auto const places = [&spawnsOf, &placesOf](std::uint32_t entry) -> std::vector<Place> const&
    {
        auto const [cached, added] = placesOf.try_emplace(entry);
        if (!added)
            return cached->second;
        auto const found = spawnsOf.find(entry);
        if (found == spawnsOf.end())
            return cached->second;
        std::map<std::uint32_t, std::vector<DropSpawn const*>> byZone;
        for (DropSpawn const* spawn : found->second)
            byZone[spawn->zone].push_back(spawn);
        for (auto const& [zone, group] : byZone)
        {
            double respawn = 0.0;
            for (DropSpawn const* spawn : group)
                respawn += spawn->respawnSecs;
            cached->second.push_back({zone, static_cast<std::uint32_t>(group.size()), MeanNearestYards(group),
                                      static_cast<std::uint32_t>(respawn / static_cast<double>(group.size()))});
        }
        return cached->second;
    };

    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
    std::set<std::uint32_t> withRow;
    std::set<std::uint32_t> zones;
    std::set<std::uint32_t> kinds;
    for (std::uint32_t const item : items)
    {
        bool rowed = false;
        for (DropSource const& source : sourcesOf(item))
        {
            if (source.chance < floor)
            {
                ++build.counts.underFloor;
                continue;
            }
            if (source.questOnly)
            {
                ++build.counts.questOnly;
                continue;
            }
            if ((source.friendlyTo & DROP_BOTH_SIDES) == DROP_BOTH_SIDES)
            {
                ++build.counts.bothSides;
                continue;
            }
            auto const creature = creatures.find(source.entry);
            std::vector<Place> const& at = places(source.entry);
            if (creature == creatures.end() || at.empty())
            {
                ++build.counts.noOpenSpawn;
                continue;
            }
            double const meanCount = (source.minCount + std::max(source.minCount, source.maxCount)) / 2.0;
            for (Place const& place : at)
            {
                DropRow row;
                row.zone = place.zone;
                row.kind = source.kind;
                row.entry = source.entry;
                row.item = item;
                row.spawns = place.spawns;
                row.spacing = place.spacing;
                row.minLevel = creature->second->minLevel;
                row.maxLevel = std::max(creature->second->minLevel, creature->second->maxLevel);
                row.eliteShare = creature->second->elite ? 1.0f : 0.0f;
                row.friendlyTo = source.friendlyTo;
                row.skill = source.kind == DropKind::Skin ? source.skill : std::uint16_t{0};
                row.perKill = static_cast<double>(source.chance) * meanCount;
                row.respawnSecs = place.respawnSecs;
                build.rows.push_back(row);
                zones.insert(place.zone);
            }
            kinds.insert(source.entry);
            rowed = true;
        }
        if (rowed)
            withRow.insert(item);
    }
    for (std::uint32_t const entry : kinds)
        for (DropSpawn const* spawn : spawnsOf[entry])
            build.spawns.push_back(*spawn);
    build.counts.items = static_cast<std::uint32_t>(withRow.size());
    build.counts.creatures = static_cast<std::uint32_t>(kinds.size());
    build.counts.zones = static_cast<std::uint32_t>(zones.size());
    return build;
}

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
    // The profession spells the trainer row asks the learner to know (a specialisation such as Gnomish Engineer,
    // WR.27); a trainer row without any wins. Rank spells (the ladder's) and spells off the profession lines (riding)
    // are not listed
    std::vector<std::uint32_t> trainerAbilities;

    [[nodiscard]] std::uint32_t Green() const { return (yellow + grey) / 2; }
};

// WR.27: a trainer teaches the recipe only to a learner that knows every ability its row asks for; knows(spell) says
// whether the learner knows a profession spell
template <typename Knows>
bool TrainerTeaches(Recipe const& recipe, Knows const& knows)
{
    return std::all_of(recipe.trainerAbilities.begin(), recipe.trainerAbilities.end(),
                       [&knows](std::uint32_t ability) { return knows(ability); });
}

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
    std::uint32_t saleCopper{0};  // disenchant: the median vendor sale of the items that share the source; 0: unknown
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

        // The abilities a trainer row may ask for that a learner's profession spells answer: the profession lines'
        // spells that are no rank (WR.27)
        std::unordered_set<std::uint32_t> answerable;
        for (AbilityRow const& ability : gather.abilities)
            if (auto const found = spells.find(ability.spell); found != spells.end() && !found->second->rankSkill)
                answerable.insert(ability.spell);

        // The best trainer row per taught spell (a learn spell's trainer teaches the spell it learns): the fewest
        // abilities asked, then the cheapest
        struct Best
        {
            TrainerRow row;
            std::vector<std::uint32_t> abilities;
            std::uint32_t count{0};
        };
        std::unordered_map<std::uint32_t, Best> trainers;
        for (TrainerRow const& row : gather.trainers)
        {
            std::vector<std::uint32_t> abilities;
            for (std::uint32_t const ability : row.reqAbility)
                if (ability && answerable.count(ability))
                    abilities.push_back(ability);
            for (std::uint32_t const key : {row.taught, row.listed == row.taught ? 0u : row.listed})
            {
                if (!key)
                    continue;
                Best& best = trainers[key];
                if (!best.count || std::make_pair(abilities.size(), row.copper) <
                                       std::make_pair(best.abilities.size(), best.row.copper))
                {
                    best.row = row;
                    best.abilities = abilities;
                }
                ++best.count;
            }
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
                recipe.trainerAbilities = trainer->second.abilities;
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
                                        row.qualityMask, row.consumed, row.yields, row.saleCopper});
        index._items = std::move(gather.items);
        index._vendors = std::move(gather.vendors);
        index._nodes = std::move(gather.nodes);
        index.Finish();
        return index;
    }

    // Raw records back from a dump (RecipeIndex::Parse)
    static RecipeIndex FromRecords(std::vector<Recipe> recipes, std::vector<Rung> rungs, std::vector<Process> processes,
                                   std::vector<ItemFact> items, std::vector<VendorRow> vendors = {},
                                   std::vector<NodeRow> nodes = {}, std::vector<DropRow> drops = {})
    {
        RecipeIndex index;
        index._recipes = std::move(recipes);
        index._rungs = std::move(rungs);
        index._processes = std::move(processes);
        index._items = std::move(items);
        index._vendors = std::move(vendors);
        index._nodes = std::move(nodes);
        index._drops = std::move(drops);
        index.Finish();
        return index;
    }

    // The drop rows and their creatures' spawns, built once after the source index (WR20a's second stage): set on a
    // copy before it is published
    void SetDrops(std::vector<DropRow> drops, std::vector<DropSpawn> spawns)
    {
        _drops = std::move(drops);
        _dropSpawns = std::move(spawns);
        FinishDrops();
    }

    [[nodiscard]] std::vector<Recipe> const& Recipes() const { return _recipes; }
    // The recipe of a spell, or null (a hash lookup: a craft visit reads it at every step)
    [[nodiscard]] Recipe const* RecipeOf(std::uint32_t spell) const
    {
        auto const found = _recipeAt.find(spell);
        return found != _recipeAt.end() ? &_recipes[found->second] : nullptr;
    }
    [[nodiscard]] std::vector<Rung> const& Rungs() const { return _rungs; }
    [[nodiscard]] std::vector<Process> const& Processes() const { return _processes; }
    [[nodiscard]] std::vector<ItemFact> const& Items() const { return _items; }
    [[nodiscard]] std::vector<VendorRow> const& Vendors() const { return _vendors; }
    [[nodiscard]] std::vector<NodeRow> const& Nodes() const { return _nodes; }
    [[nodiscard]] std::vector<DropRow> const& Drops() const { return _drops; }
    [[nodiscard]] std::vector<DropSpawn> const& DropSpawns() const { return _dropSpawns; }
    [[nodiscard]] std::size_t Records() const
    {
        return _recipes.size() + _rungs.size() + _processes.size() + _items.size() + _vendors.size() + _nodes.size() +
               _drops.size();
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

    // Positions in Drops() of the drop rows that give an item
    [[nodiscard]] std::vector<std::size_t> const& DropsOf(std::uint32_t item) const
    {
        return Positions(_dropsOf, item);
    }

    // The spawn points of a creature kind in a zone (by spawn id; empty on an index read from a dump)
    [[nodiscard]] std::span<DropSpawn const> DropSpawnsOf(std::uint32_t zone, std::uint32_t entry) const
    {
        DropSpawn key;
        key.zone = zone;
        key.entry = entry;
        auto const [first, last] =
            std::equal_range(_dropSpawns.begin(), _dropSpawns.end(), key, [](DropSpawn const& a, DropSpawn const& b)
                             { return std::tie(a.zone, a.entry) < std::tie(b.zone, b.entry); });
        return {first, last};
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
        out << "# recipe-index-dump v6 module " << moduleRev << " core " << coreRev << " records " << Records() << '\n';
        out << "#R\tskill\tspell\tname\tminRank\tyellow\tgrey\traceMask\tclassMask\tacquire\teffect\tproduct\t"
               "productCount\tcastMs\tfocus\treagents\ttools\ttoolCategories\ttrainers\ttrainerCopper\ttrainerRank\t"
               "trainerLevel\titems\ttrainerAbilities\n";
        out << "#L\tskill\tspell\tname\tcap\tstep\ttrainers\tcopper\treqSkill\treqRank\treqLevel\n";
        out << "#P\tkind\tsource\tskill\trank\tminLevel\tmaxLevel\tqualityMask\tconsumed\tyields\tsaleCopper\n";
        out << "#I\tid\tknown\tname\titemClass\tsubClass\tinventoryType\tquality\titemLevel\trequiredLevel\t"
               "allowableClass\tstats\n";
        out << "#V\titem\tbuyCopper\tbuyCount\tstocked\tlimited\n";
        out << "#N\tzone\tskill\treqSkill\tentry\tnodes\tspacing\titem\tperNode\n";
        out << "#D\tzone\tkind\tentry\titem\tspawns\tspacing\tminLevel\tmaxLevel\teliteShare\tfriendlyTo\tskill\t"
               "perKill\trespawnSecs\n";
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
            out << '\t';
            WriteList(out, r.trainerAbilities, [](std::uint32_t id) { return std::to_string(id); });
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
            out << '\t' << p.saleCopper << '\n';
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
        for (DropRow const& d : _drops)
        {
            std::ostringstream numbers;
            numbers << std::fixed << std::setprecision(2) << d.spacing << '\t' << d.minLevel << '\t' << d.maxLevel
                    << '\t' << d.eliteShare << '\t' << static_cast<int>(d.friendlyTo) << '\t' << d.skill << '\t'
                    << std::setprecision(4) << d.perKill << '\t' << d.respawnSecs;
            out << "D\t" << d.zone << '\t' << static_cast<int>(d.kind) << '\t' << d.entry << '\t' << d.item << '\t'
                << d.spawns << '\t' << numbers.str() << '\n';
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
        std::vector<DropRow> drops;
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
                if (line.rfind("# recipe-index-dump v2", 0) == 0 || line.rfind("# recipe-index-dump v3", 0) == 0 ||
                    line.rfind("# recipe-index-dump v4", 0) == 0 || line.rfind("# recipe-index-dump v5", 0) == 0 ||
                    line.rfind("# recipe-index-dump v6", 0) == 0)
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
                if (f[0] == "R" && (f.size() == 23 || f.size() == 24))  // v6 adds the trainer's abilities
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
                    if (f.size() == 24)
                        for (std::string const& part : Split(f[23], ','))
                            if (!part.empty())
                                r.trainerAbilities.push_back(U(part));
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
                else if (f[0] == "P" && (f.size() == 10 || f.size() == 11))  // v5 adds the sale
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
                    p.saleCopper = f.size() == 11 ? U(f[10]) : 0u;
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
                else if (f[0] == "D" && f.size() == 14)
                {
                    DropRow d;
                    d.zone = U(f[1]);
                    d.kind = U(f[2]) ? DropKind::Skin : DropKind::Corpse;
                    d.entry = U(f[3]);
                    d.item = U(f[4]);
                    d.spawns = U(f[5]);
                    d.spacing = std::stof(f[6]);
                    d.minLevel = U(f[7]);
                    d.maxLevel = U(f[8]);
                    d.eliteShare = std::stof(f[9]);
                    d.friendlyTo = static_cast<std::uint8_t>(U(f[10]));
                    d.skill = static_cast<std::uint16_t>(U(f[11]));
                    d.perKill = std::stod(f[12]);
                    d.respawnSecs = U(f[13]);
                    drops.push_back(d);
                }
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

        std::size_t const total = recipes.size() + rungs.size() + processes.size() + items.size() + vendors.size() +
                                  nodes.size() + drops.size();
        if (!header || total != declared)
        {
            error = header ? "record count " + std::to_string(total) + " differs from the header's " +
                                 std::to_string(declared)
                           : "no recipe-index-dump v2 to v6 header";
            return false;
        }
        index = FromRecords(std::move(recipes), std::move(rungs), std::move(processes), std::move(items),
                            std::move(vendors), std::move(nodes), std::move(drops));
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
        _recipeAt.clear();
        for (std::size_t i = 0; i < _recipes.size(); ++i)
        {
            _recipeAt.emplace(_recipes[i].spell, i);  // the first of a spell, as the linear scan found it
            if (_recipes[i].effect == Effect::Make && _recipes[i].product)
                _madeBy[_recipes[i].product].push_back(i);
        }
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
        FinishDrops();
    }

    void FinishDrops()
    {
        std::sort(_drops.begin(), _drops.end(), [](DropRow const& a, DropRow const& b)
                  { return std::tie(a.item, a.zone, a.kind, a.entry) < std::tie(b.item, b.zone, b.kind, b.entry); });
        _dropsOf.clear();
        for (std::size_t i = 0; i < _drops.size(); ++i)
            _dropsOf[_drops[i].item].push_back(i);
        std::sort(_dropSpawns.begin(), _dropSpawns.end(), [](DropSpawn const& a, DropSpawn const& b)
                  { return std::tie(a.zone, a.entry, a.spawnId) < std::tie(b.zone, b.entry, b.spawnId); });
    }

    std::vector<Recipe> _recipes;
    std::vector<Rung> _rungs;
    std::vector<Process> _processes;
    std::vector<ItemFact> _items;
    std::unordered_map<std::uint32_t, std::size_t> _itemAt;
    std::vector<VendorRow> _vendors;
    std::vector<NodeRow> _nodes;
    std::vector<DropRow> _drops;
    std::vector<DropSpawn> _dropSpawns;  // not in the dump
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> _madeBy;
    std::unordered_map<std::uint32_t, std::size_t> _recipeAt;
    std::unordered_map<std::uint32_t, std::size_t> _vendorAt;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> _nodesOf;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> _dropsOf;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> _yieldedBy;
    std::unordered_map<std::uint32_t, std::size_t> _disenchantAt;
};
}  // namespace Recipes

#endif
