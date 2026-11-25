/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The server side of the recipe index (professions plan WR3a): the world-thread gather of the core's profession
// spells, rank spells, trainers, recipe items, vendors and the loot stores' processing yields, the reagents' vendor
// prices and the gathering nodes' yields (WR9b), the worker build, the published index, its status lines and the
// `playerbots professions index` command.
//
// The shape of the gather follows PlannerUpkeep.cpp (the SPELL_EFFECT_LEARN_SPELL pattern of AddReagents and the
// trainer loop). The loot yields are read from the loot stores' tables, WR.9: no source index of our own.

#include "PlannerRecipes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "AuctionReference.h"
#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GatherNodeMgr.h"
#include "GitRevision.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PlannerIndexes.h"
#include "PlannerPool.h"
#include "PlannerService.h"
#include "ProfessionRules.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Trainer.h"

namespace Planner
{
namespace
{
enum class RecipeIndexState : uint8
{
    NotStarted,
    Building,
    Ready,
    Failed
};

constexpr std::size_t LISTED_RECIPES = 40;  // the lines `index <skill>` prints at most
constexpr float LONE_NODE_YARDS = 300.0f;   // [E] the spacing of a node kind with one spawn in its zone

std::atomic<RecipeIndexState> recipeState{RecipeIndexState::NotStarted};
SharedSlot<Recipes::RecipeIndex> recipeBook;
std::atomic<uint32> gatherMsValue{0};
std::atomic<uint32> buildMsValue{0};

uint32 RecipeMsSince(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

// The mean distance from each spawn to the nearest other spawn of its group
float MeanNearest(std::vector<GatherNodeSpawn const*> const& group)
{
    if (group.size() < 2)
        return LONE_NODE_YARDS;
    double sum = 0.0;
    for (GatherNodeSpawn const* spawn : group)
    {
        float nearest = std::numeric_limits<float>::max();
        for (GatherNodeSpawn const* other : group)
            if (other != spawn)
                nearest = std::min(nearest, spawn->pos.GetExactDist(&other->pos));
        sum += nearest;
    }
    return static_cast<float>(sum / static_cast<double>(group.size()));
}

}  // namespace

// One loot store's rows by entry (quest-only rows and rows that are never on in the normal mode left out)
Recipes::LootTable LoadLootTable(char const* table)
{
    Recipes::LootTable rows;
    QueryResult result = WorldDatabase.Query(
        "SELECT CAST(Entry AS UNSIGNED), CAST(Item AS SIGNED), CAST(Reference AS SIGNED), CAST(Chance AS DOUBLE), "
        "CAST(GroupId AS UNSIGNED), CAST(MinCount AS SIGNED), CAST(MaxCount AS UNSIGNED), "
        "CAST(QuestRequired AS UNSIGNED), CAST(LootMode AS UNSIGNED) FROM {}",
        table);
    if (!result)
        return rows;
    do
    {
        Field* fields = result->Fetch();
        if (fields[7].Get<uint64>() || !(fields[8].Get<uint64>() & 1))
            continue;
        Recipes::LootRow row;
        row.item = static_cast<uint32>(fields[1].Get<int64>());
        row.reference = static_cast<int32>(fields[2].Get<int64>());
        row.chance = static_cast<float>(fields[3].Get<double>());
        row.group = static_cast<uint32>(fields[4].Get<uint64>());
        row.minCount = static_cast<uint32>(std::max<int64>(fields[5].Get<int64>(), 1));
        row.maxCount = static_cast<uint32>(fields[6].Get<uint64>());
        rows[static_cast<uint32>(fields[0].Get<uint64>())].push_back(row);
    } while (result->NextRow());

    return rows;
}

namespace
{

Recipes::Gather GatherRecipes()
{
    PLAYERBOT_CHECK_WORLD_THREAD("Planner::GatherRecipes");
    Recipes::Gather gather;
    std::unordered_set<uint32> skills;
    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        skills.insert(static_cast<uint32>(info.Which));

    // The profession skill lines' abilities and the spells they name, plus every rank spell of the skills
    std::set<uint32> wanted;
    std::unordered_set<uint32> abilitySpells;
    for (uint32 row = 0; row < sSkillLineAbilityStore.GetNumRows(); ++row)
    {
        SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(row);
        if (!ability || !skills.count(ability->SkillLine))
            continue;
        Recipes::AbilityRow copy;
        copy.skill = static_cast<uint16>(ability->SkillLine);
        copy.spell = ability->Spell;
        copy.raceMask = ability->RaceMask;
        copy.classMask = ability->ClassMask;
        copy.minRank = ability->MinSkillLineRank;
        copy.yellow = ability->TrivialSkillLineRankLow;
        copy.grey = ability->TrivialSkillLineRankHigh;
        copy.acquire = ability->AcquireMethod;
        gather.abilities.push_back(copy);
        wanted.insert(ability->Spell);
        abilitySpells.insert(ability->Spell);
    }
    for (uint32 const skill : skills)
        for (uint32 const spell : sSpellMgr->GetSkillRankSpells(skill))
            wanted.insert(spell);

    std::set<uint32> items;  // every item the rows name, for their facts
    for (uint32 const spell : wanted)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info)
            continue;
        Recipes::SpellRow row;
        row.spell = spell;
        row.name = info->SpellName[0] ? info->SpellName[0] : "";
        row.castMs = info->CalcCastTime();
        row.focus = info->RequiresSpellFocus;
        for (std::size_t i = 0; i < info->Reagent.size(); ++i)
            if (info->Reagent[i] > 0)
            {
                row.reagents.push_back({static_cast<uint32>(info->Reagent[i]), info->ReagentCount[i]});
                items.insert(static_cast<uint32>(info->Reagent[i]));
            }
        for (uint32 const tool : info->Totem)
            if (tool)
            {
                row.tools.push_back(tool);
                items.insert(tool);
            }
        for (uint32 const category : info->TotemCategory)
            if (category)
                row.toolCategories.push_back(category);
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell && !row.taught)
                row.taught = effect.TriggerSpell;
            if (row.effect != Recipes::Effect::Other)
                continue;
            switch (effect.Effect)
            {
                case SPELL_EFFECT_CREATE_ITEM:
                case SPELL_EFFECT_CREATE_ITEM_2:
                    row.effect = Recipes::Effect::Make;
                    row.product = effect.ItemType;
                    row.productCount = static_cast<uint32>(std::max<int32>(effect.CalcValue(), 1));
                    if (row.product)
                        items.insert(row.product);
                    break;
                case SPELL_EFFECT_ENCHANT_ITEM:
                case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
                case SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC:
                    row.effect = Recipes::Effect::Enchant;
                    break;
                case SPELL_EFFECT_DISENCHANT:
                case SPELL_EFFECT_PROSPECTING:
                case SPELL_EFFECT_MILLING:
                    row.effect = Recipes::Effect::Process;
                    break;
                default:
                    break;
            }
        }
        if (SpellLearnSkillNode const* node = sSpellMgr->GetSpellLearnSkill(spell))
            if (skills.count(node->skill))
            {
                row.rankSkill = node->skill;
                row.rankCap = node->maxvalue;
                row.rankStep = node->step;
            }
        gather.spells.push_back(std::move(row));
    }

    // Trainers: one object per trainer id; only rows that teach a gathered spell
    std::unordered_set<Trainer::Trainer const*> seen;
    CreatureTemplateContainer const* templates = sObjectMgr->GetCreatureTemplates();
    std::vector<uint32> entries;
    entries.reserve(templates->size());
    for (auto const& [entry, creature] : *templates)
        entries.push_back(entry);
    std::sort(entries.begin(), entries.end());
    for (uint32 const entry : entries)
    {
        Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry);
        if (!trainer || !seen.insert(trainer).second)
            continue;
        for (Trainer::Spell const& spell : trainer->GetSpells())
        {
            Recipes::TrainerRow row;
            row.trainer = entry;
            row.listed = spell.SpellId;
            row.taught = spell.SpellId;
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId))
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                    {
                        row.taught = effect.TriggerSpell;
                        break;
                    }
            if (!wanted.count(row.taught) && !wanted.count(row.listed))
                continue;
            row.copper = spell.MoneyCost;
            row.reqSkill = spell.ReqSkillLine;
            row.reqRank = spell.ReqSkillRank;
            row.reqLevel = spell.ReqLevel;
            gather.trainers.push_back(row);
        }
    }

    // Recipe items (an on-use spell with a learn-spell effect for a gathered ability) and processing sources
    std::unordered_set<uint32> recipeItems;
    std::map<uint32, Recipes::ProcessRow> disenchants;
    std::map<uint32, std::vector<uint32>> disenchantSales;  // the vendor sales of a disenchant group's items (WR14d)
    std::vector<Recipes::ProcessRow> sources;
    for (ItemTemplate const* proto : *sObjectMgr->GetItemTemplateStoreFast())
    {
        if (!proto)
            continue;
        for (_Spell const& use : proto->Spells)
        {
            if (use.SpellId <= 0)
                continue;
            // A recipe item names its recipe in a learn-spell-id slot (beside the generic learning spell 483)
            if (use.SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID &&
                abilitySpells.count(static_cast<uint32>(use.SpellId)))
            {
                Recipes::RecipeItemRow row;
                row.item = proto->ItemId;
                row.taught = static_cast<uint32>(use.SpellId);
                row.buyCopper = static_cast<uint32>(std::max<int32>(proto->BuyPrice, 0));
                gather.recipeItems.push_back(row);
                recipeItems.insert(proto->ItemId);
                items.insert(proto->ItemId);
                continue;
            }
            if (use.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(static_cast<uint32>(use.SpellId));
            if (!info)
                continue;
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && abilitySpells.count(effect.TriggerSpell))
                {
                    Recipes::RecipeItemRow row;
                    row.item = proto->ItemId;
                    row.taught = effect.TriggerSpell;
                    row.buyCopper = static_cast<uint32>(std::max<int32>(proto->BuyPrice, 0));
                    gather.recipeItems.push_back(row);
                    recipeItems.insert(proto->ItemId);
                    items.insert(proto->ItemId);
                }
        }

        bool const prospect = proto->HasFlag(ITEM_FLAG_IS_PROSPECTABLE);
        bool const mill = proto->HasFlag(ITEM_FLAG_IS_MILLABLE);
        if (prospect || mill)
        {
            Recipes::ProcessRow row;
            row.kind = prospect ? Recipes::ProcessKind::Prospect : Recipes::ProcessKind::Mill;
            row.source = proto->ItemId;
            row.skill = proto->RequiredSkill;
            row.rank = proto->RequiredSkillRank;
            row.minLevel = row.maxLevel = proto->ItemLevel;
            row.qualityMask = 1u << std::min<uint32>(proto->Quality, 31);
            row.consumed = Recipes::PROSPECT_MILL_COUNT;
            sources.push_back(row);
            items.insert(proto->ItemId);
        }
        if (proto->DisenchantID && (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR))
        {
            auto const [at, added] = disenchants.try_emplace(proto->DisenchantID);
            Recipes::ProcessRow& row = at->second;
            if (added)
            {
                row.kind = Recipes::ProcessKind::Disenchant;
                row.source = proto->DisenchantID;
                row.skill = SKILL_ENCHANTING;
                row.rank = proto->RequiredDisenchantSkill;
                row.minLevel = row.maxLevel = proto->ItemLevel;
            }
            row.rank = std::min<uint32>(row.rank, proto->RequiredDisenchantSkill);
            row.minLevel = std::min<uint32>(row.minLevel, proto->ItemLevel);
            row.maxLevel = std::max<uint32>(row.maxLevel, proto->ItemLevel);
            row.qualityMask |= 1u << std::min<uint32>(proto->Quality, 31);
            disenchantSales[proto->DisenchantID].push_back(proto->SellPrice);
        }
    }
    for (auto& [id, row] : disenchants)
    {
        // The sale an enchanter gives up when it breaks down an item of the group: the group's median sale
        std::vector<uint32>& sales = disenchantSales[id];
        std::nth_element(sales.begin(), sales.begin() + sales.size() / 2, sales.end());
        row.saleCopper = sales.empty() ? 0 : sales[sales.size() / 2];
        sources.push_back(std::move(row));
    }

    // Vendors that sell a recipe item (every vendor creature entry with the item, spawned or not), and the spawned
    // vendors that sell a reagent or tool for gold, with or without a stock limit (WR9b, PLAN 3.6)
    std::unordered_set<uint32> materials;
    for (Recipes::SpellRow const& spell : gather.spells)
    {
        for (Recipes::ItemCount const& reagent : spell.reagents)
            materials.insert(reagent.item);
        for (uint32 const tool : spell.tools)
            materials.insert(tool);
    }
    std::unordered_set<uint32> spawned;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        for (uint32 const id : {data.id, data.id2, data.id3})
            if (id)
                spawned.insert(id);
    std::unordered_map<uint32, uint32> vendors;
    std::map<uint32, Recipes::VendorRow> materialVendors;
    for (uint32 const entry : entries)
    {
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
        if (!creature || !(creature->npcflag & UNIT_NPC_FLAG_VENDOR))
            continue;
        VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(entry);
        if (!list)
            continue;
        for (VendorItem const* sold : list->m_items)
        {
            if (!sold)
                continue;
            if (recipeItems.count(sold->item))
                ++vendors[sold->item];
            if (sold->ExtendedCost || !materials.count(sold->item) || !spawned.count(entry))
                continue;
            Recipes::VendorRow& row = materialVendors[sold->item];
            row.item = sold->item;
            ++(sold->maxcount ? row.limited : row.stocked);
        }
    }
    for (Recipes::RecipeItemRow& row : gather.recipeItems)
        row.vendors = vendors[row.item];
    for (auto& [id, row] : materialVendors)
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id))
        {
            row.buyCopper = static_cast<uint32>(std::max<int32>(proto->BuyPrice, 0));
            row.buyCount = std::max<uint32>(proto->BuyCount, 1);
            gather.vendors.push_back(row);
        }

    // The loot stores' yields per source; a source that yields nothing is not a process
    Recipes::LootTable const disenchantTable = LoadLootTable("disenchant_loot_template");
    Recipes::LootTable const prospectTable = LoadLootTable("prospecting_loot_template");
    Recipes::LootTable const millTable = LoadLootTable("milling_loot_template");
    Recipes::LootTable const referenceTable = LoadLootTable("reference_loot_template");
    for (Recipes::ProcessRow& row : sources)
    {
        Recipes::LootTable const& table = row.kind == Recipes::ProcessKind::Disenchant ? disenchantTable
                                          : row.kind == Recipes::ProcessKind::Prospect ? prospectTable
                                                                                       : millTable;
        row.yields = Recipes::ExpectedYield(table, row.source, &referenceTable);
        if (row.yields.empty())
            continue;
        for (Recipes::Yield const& yield : row.yields)
            items.insert(yield.item);
        gather.processes.push_back(std::move(row));
    }

    // Gathering nodes (WR9b): per zone and node entry the gather node index's spawns, their mean distance to the
    // nearest other spawn of the entry, and the node's expected loot (respawns are WR13a's)
    Recipes::LootTable const nodeTable = LoadLootTable("gameobject_loot_template");
    for (auto const& [mapId, zones] : sGatherNodeMgr.Spawns())
        for (auto const& [zoneId, spawns] : zones)
        {
            std::map<uint32, std::vector<GatherNodeSpawn const*>> byEntry;
            for (GatherNodeSpawn const& spawn : spawns)
                if (GameObjectData const* data = sObjectMgr->GetGameObjectData(spawn.spawnId))
                    byEntry[data->id].push_back(&spawn);
            for (auto const& [entry, group] : byEntry)
            {
                GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
                if (!info)
                    continue;
                // The standing nodes' spacing (WR13b): a spawn point holds a node its share of the time, so the
                // nodes that stand are sqrt(1 / share) farther apart
                double shares = 0.0;
                for (GatherNodeSpawn const* spawn : group)
                    shares += std::clamp(spawn->share, 0.0f, 1.0f);
                double const meanShare = shares / static_cast<double>(group.size());
                float const spacing =
                    meanShare > 0.0 ? static_cast<float>(MeanNearest(group) / std::sqrt(meanShare)) : 0.0f;
                std::vector<Recipes::Yield> const yields =
                    Recipes::ExpectedYield(nodeTable, info->GetLootId(), &referenceTable);
                for (Recipes::Yield const& yield : yields)
                {
                    gather.nodes.push_back({zoneId, static_cast<uint16>(group.front()->skillId),
                                            group.front()->reqSkillValue, entry, static_cast<uint32>(group.size()),
                                            spacing, yield.item, yield.expected});
                    items.insert(yield.item);
                }
            }
        }

    for (uint32 const id : items)
    {
        Recipes::ItemFact fact;
        fact.id = id;
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id))
        {
            fact.known = true;
            fact.name = proto->Name1;
            fact.itemClass = proto->Class;
            fact.subClass = proto->SubClass;
            fact.inventoryType = proto->InventoryType;
            fact.quality = proto->Quality;
            fact.itemLevel = proto->ItemLevel;
            fact.requiredLevel = proto->RequiredLevel;
            fact.allowableClass = static_cast<uint32>(proto->AllowableClass);
            for (uint32 stat = 0; stat < proto->StatsCount && stat < MAX_ITEM_PROTO_STATS; ++stat)
                if (proto->ItemStat[stat].ItemStatValue != 0 && proto->ItemStat[stat].ItemStatType < 64)
                    fact.stats |= std::uint64_t{1} << proto->ItemStat[stat].ItemStatType;
        }
        gather.items.push_back(std::move(fact));
    }

    return gather;
}

std::string DumpFilePath(std::string const& name)
{
    if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos)
        return {};
    // The core's own log folder option, as `playerbots item dump` reads it
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    return dir + name;
}
}  // namespace

void StartRecipeIndex()
{
    RecipeIndexState expected = RecipeIndexState::NotStarted;
    if (!recipeState.compare_exchange_strong(expected, RecipeIndexState::Building))
        return;  // once: spells, trainers and loot stores are static after startup

    auto const gatherStart = std::chrono::steady_clock::now();
    auto input = std::make_shared<Recipes::Gather>(GatherRecipes());
    uint32 const gatherMs = RecipeMsSince(gatherStart);
    gatherMsValue.store(gatherMs);
    LOG_INFO("server.loading",
             "Recipe index: gathered {} skill line abilities, {} spells, {} trainer rows, {} recipe items, {} "
             "processing sources, {} items in {} ms",
             input->abilities.size(), input->spells.size(), input->trainers.size(), input->recipeItems.size(),
             input->processes.size(), input->items.size(), gatherMs);

    bool const posted = Pool().Post(
        [input]
        {
            auto const buildStart = std::chrono::steady_clock::now();
            try
            {
                auto index = std::make_shared<Recipes::RecipeIndex>(Recipes::RecipeIndex::Build(std::move(*input)));
                buildMsValue.store(RecipeMsSince(buildStart));
                LOG_INFO("playerbots",
                         "Recipe index built in {} ms on a worker: {} recipes, {} rungs, {} processes, {} items",
                         buildMsValue.load(), index->Recipes().size(), index->Rungs().size(), index->Processes().size(),
                         index->Items().size());
                recipeBook.Publish(std::move(index));
                recipeState.store(RecipeIndexState::Ready, std::memory_order_release);
                AuctionReference::OnRecipeIndexReady(recipeBook.Current());  // auction WA2a: the reference table
            }
            catch (std::exception const& e)
            {
                recipeState.store(RecipeIndexState::Failed, std::memory_order_release);
                LOG_ERROR("playerbots", "Recipe index: the build failed: {}", e.what());
            }
        });
    if (!posted)
    {
        recipeState.store(RecipeIndexState::Failed, std::memory_order_release);
        LOG_ERROR("server.loading", "Recipe index: the planner pool is not running; no recipe index");
    }
}

std::shared_ptr<Recipes::RecipeIndex const> CurrentRecipeIndex() { return recipeBook.Current(); }

bool RecipeIndexReady() { return recipeState.load(std::memory_order_acquire) == RecipeIndexState::Ready; }

std::vector<std::string> RecipeIndexStatus()
{
    RecipeIndexState const state = recipeState.load(std::memory_order_acquire);
    std::shared_ptr<Recipes::RecipeIndex const> const index =
        state == RecipeIndexState::Ready ? CurrentRecipeIndex() : nullptr;
    if (!index)
        return {Acore::StringFormat("PLANNER recipe index: {}", state == RecipeIndexState::Building ? "building"
                                                                : state == RecipeIndexState::Failed ? "failed"
                                                                                                    : "not started")};
    return {
        Acore::StringFormat("PLANNER recipe index: ready, {} recipes, {} rungs, {} processes, {} items, gathered "
                            "in {} ms, built in {} ms on a worker",
                            index->Recipes().size(), index->Rungs().size(), index->Processes().size(),
                            index->Items().size(), gatherMsValue.load(), buildMsValue.load())};
}

bool HandleRecipeIndexCommand(ChatHandler* handler, std::vector<std::string> const& words)
{
    std::shared_ptr<Recipes::RecipeIndex const> const index = RecipeIndexReady() ? CurrentRecipeIndex() : nullptr;
    if (!index)
    {
        handler->SendSysMessage(RecipeIndexStatus().front());
        return false;
    }

    ProfessionRules::ProfessionInfo const* only = nullptr;
    bool dump = false;
    std::string file = "recipe-index-dump.txt";
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        if (words[i] == "dump")
        {
            dump = true;
            if (i + 1 < words.size())
                file = words[++i];
            continue;
        }
        // One profession word at most; any other word is a usage error
        ProfessionRules::ProfessionInfo const* named = nullptr;
        for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
            if (info.Name == words[i])
                named = &info;
        if (!named || only)
        {
            handler->SendSysMessage("Usage: .playerbots professions index [<profession>] [dump [<file name>]]");
            return false;
        }
        only = named;
    }

    if (dump)
    {
        std::string const path = DumpFilePath(file);
        if (path.empty())
        {
            handler->SendErrorMessage("RECIPES dump: give a bare file name; the dump goes into the logs folder");
            return false;
        }
        std::string const text = index->Dump("unknown", GitRevision::GetHash());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out)
        {
            handler->SendErrorMessage("RECIPES dump: cannot write {}", path);
            return false;
        }
        handler->PSendSysMessage("RECIPES dump: {} records to {}", index->Records(), path);
    }

    for (ProfessionRules::ProfessionInfo const& info : ProfessionRules::PROFESSIONS)
        if (!only || only == &info)
            handler->SendSysMessage(index->Summary(static_cast<uint16>(info.Which), info.Name));
    if (!only)
    {
        std::size_t kinds[3] = {0, 0, 0};
        for (Recipes::Process const& process : index->Processes())
            ++kinds[static_cast<std::size_t>(process.kind)];
        handler->PSendSysMessage("Processing: {} disenchant sources, {} prospecting, {} milling", kinds[0], kinds[1],
                                 kinds[2]);
        return true;
    }

    std::size_t listed = 0;
    for (Recipes::Recipe const& recipe : index->Recipes())
    {
        if (recipe.skill != static_cast<uint16>(only->Which))
            continue;
        if (++listed > LISTED_RECIPES)
        {
            handler->SendSysMessage("... (the dump has the rest)");
            break;
        }
        std::string reagents;
        for (Recipes::ItemCount const& reagent : recipe.reagents)
            reagents += Acore::StringFormat("{}{}x{}", reagents.empty() ? "" : " ", reagent.count, reagent.item);
        handler->PSendSysMessage("{} {} | {}/{}/{}/{} | {} {}x{} | {} | {}", recipe.spell, recipe.name, recipe.minRank,
                                 recipe.yellow, recipe.Green(), recipe.grey, Recipes::EffectWord(recipe.effect),
                                 recipe.productCount, recipe.product, reagents, Recipes::HowLearned(recipe));
    }

    return true;
}
}  // namespace Planner
