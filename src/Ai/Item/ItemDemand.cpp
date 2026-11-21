/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ItemDemand.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>

#include "AiObjectContext.h"
#include "Bag.h"
#include "BotTraitMgr.h"
#include "Item.h"
#include "ItemBotFacts.h"
#include "ItemUse.h"
#include "ItemUseIndex.h"
#include "ItemUseKinds.h"
#include "ItemWorthRules.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerRecipes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "ProfessionRules.h"
#include "RecipeIndex.h"
#include "Timer.h"

namespace ItemUse
{
namespace
{
// The bags' consumables as demand's list: one entry per use record, the stacks' counts merged per record. WI9a's
// own builder (INTERFACE 2.3, D7); WI19 replaces it with lane 2's UsableOf
struct DemandList
{
    std::vector<UsableEntry> list;
    std::vector<Record> records;
    std::vector<std::uint8_t> canUse;
    std::vector<std::uint32_t> buy;
    std::vector<std::uint8_t> offered;  // WI9b: a vendor sells it to the bot; offered only: count 0
    std::vector<std::uint8_t> made;     // WR15: the bot makes it; made only: count 0
    std::vector<DemandHeld> held;
};

void AddHeld(std::vector<DemandHeld>& held, std::uint32_t entry, std::uint32_t count)
{
    auto const same =
        std::find_if(held.begin(), held.end(), [entry](DemandHeld const& item) { return item.entry == entry; });
    if (same != held.end())
        same->count += count;
    else
        held.push_back({entry, count});
}

void AddRecords(Player* bot, ItemTemplate const* proto, std::uint32_t count, std::uint64_t guid,
                ItemUseIndex const& index, DemandList& out, bool offered, bool made = false)
{
    std::uint8_t canUse = 2;  // asked once per item: 2 not yet asked
    for (Record const& record : index.OfItem(proto->ItemId))
    {
        std::uint32_t const id = index.IdOf(record);
        auto const same = std::find_if(out.list.begin(), out.list.end(),
                                       [id](UsableEntry const& entry) { return entry.record == id; });
        if (same != out.list.end())
        {
            same->count += count;
            continue;
        }
        if (canUse == 2)
            canUse = bot->CanUseItem(proto) == EQUIP_ERR_OK ? 1 : 0;

        Classified const classified = Classify(record);
        UsableEntry entry;
        entry.record = id;
        entry.item = ItemRef{proto->ItemId, guid, SLOT_BAGS};
        entry.kind = classified.kind;
        entry.sub = classified.sub;
        entry.rank = classified.rank;
        entry.count = count;
        entry.consumed = record.consumed;
        entry.potion = record.potion;
        entry.replaceCopper = record.replaceCopper;
        entry.ready = true;
        entry.shape = record.shape;
        entry.radiusYards = record.radiusYards;
        entry.rangeYards = record.rangeYards;
        entry.minRangeYards = record.minRangeYards;
        entry.rangeFriendYards = record.rangeFriendYards;
        out.list.push_back(entry);
        out.records.push_back(record);
        out.canUse.push_back(canUse);
        out.buy.push_back(static_cast<std::uint32_t>(std::max<std::int32_t>(proto->BuyPrice, 0)));
        out.offered.push_back(offered ? 1 : 0);
        out.made.push_back(made ? 1 : 0);
    }
}

void AddItem(Player* bot, Item* item, ItemUseIndex const& index, DemandList& out)
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE)
        return;
    AddHeld(out.held, proto->ItemId, item->GetCount());
    AddRecords(bot, proto, item->GetCount(), item->GetGUID().GetRawValue(), index, out, false);
}

// One use item vendors sell for gold with no reputation (UpkeepStock::Use offers), its fit facts
struct OfferedItem
{
    std::uint32_t item{0};
    std::uint32_t requiredLevel{0};
    std::uint32_t classMask{0};
    std::uint32_t raceMask{0};
    std::uint32_t requiredSkill{0};
    std::uint32_t requiredSkillRank{0};
};

// The distinct offered use items, by required level: once per index version and map thread (not per build)
std::vector<OfferedItem> const& OfferedItems(Planner::UpkeepIndex const& upkeep)
{
    thread_local std::uint32_t version = 0;
    thread_local bool built = false;
    thread_local std::vector<OfferedItem> items;
    if (built && version == upkeep.version)
        return items;
    items.clear();
    // The offers are sorted by required level, item, vendor: one item's offers are neighbours
    for (Planner::VendorOffer const& offer : upkeep.Offers(Planner::UpkeepStock::Use))
        if (offer.GoldOnly() && !offer.requiredFaction && (items.empty() || items.back().item != offer.item))
            items.push_back({offer.item, offer.requiredLevel, offer.classMask, offer.raceMask, offer.requiredSkill,
                             offer.requiredSkillRank});
    version = upkeep.version;
    built = true;
    return items;
}

// The use items a vendor would sell the bot (upkeep's OfferFits tests: gold only, level, class, race, skill, no
// reputation): a held one is flagged offered, the others join the list with a count of 0
void AddOffers(Player* bot, DemandList& out)
{
    std::shared_ptr<Planner::UpkeepIndex const> const upkeep =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    if (!upkeep)
        return;
    ItemUseIndex const& index = ItemUseIndex::Get();
    std::uint32_t const level = bot->GetLevel();
    for (OfferedItem const& offer : OfferedItems(*upkeep))
    {
        if (offer.requiredLevel > level)
            break;
        if ((offer.classMask && !(offer.classMask & bot->getClassMask())) ||
            (offer.raceMask && !(offer.raceMask & bot->getRaceMask())) ||
            (offer.requiredSkill && bot->GetSkillValue(offer.requiredSkill) < offer.requiredSkillRank))
            continue;
        bool held = false;
        for (std::size_t i = 0; i < out.list.size(); ++i)
            if (out.list[i].item.entry == offer.item)
            {
                out.offered[i] = 1;
                held = true;
            }
        ItemTemplate const* proto = held ? nullptr : sObjectMgr->GetItemTemplate(offer.item);
        if (proto)
            AddRecords(bot, proto, 0, 0, index, out, true);
    }
}

// One recipe of a supply profession (professions WR15, PLAN 3.11: cooking and first aid feed the bot itself)
struct SupplyRecipe
{
    std::uint16_t skill{0};
    std::uint32_t spell{0};
    std::uint32_t product{0};
};

// The supply professions' recipes with a product: once per recipe index and map thread (not per build)
std::vector<SupplyRecipe> const& SupplyRecipes(std::shared_ptr<Recipes::RecipeIndex const> const& recipes)
{
    thread_local std::weak_ptr<Recipes::RecipeIndex const> source;
    thread_local std::vector<SupplyRecipe> rows;
    if (source.lock() == recipes)
        return rows;
    rows.clear();
    for (Recipes::Recipe const& recipe : recipes->Recipes())
        if (recipe.product && (recipe.skill == static_cast<std::uint16_t>(ProfessionRules::Skill::Cooking) ||
                               recipe.skill == static_cast<std::uint16_t>(ProfessionRules::Skill::FirstAid)))
            rows.push_back({recipe.skill, recipe.spell, recipe.product});
    source = recipes;
    return rows;
}

// The use items the bot makes (WR15): the products of the cooking and first aid recipes it knows. A held one is
// flagged made, the others join the list with a count of 0, as an offer does; demand's usable test still applies
void AddMade(Player* bot, DemandList& out)
{
    std::shared_ptr<Recipes::RecipeIndex const> const recipes =
        Planner::RecipeIndexReady() ? Planner::CurrentRecipeIndex() : nullptr;
    if (!recipes)
        return;
    ItemUseIndex const& index = ItemUseIndex::Get();
    bool const cooks = bot->HasSkill(static_cast<std::uint32_t>(ProfessionRules::Skill::Cooking));
    bool const firstAid = bot->HasSkill(static_cast<std::uint32_t>(ProfessionRules::Skill::FirstAid));
    if (!cooks && !firstAid)
        return;
    // Its own PerfMonitor row, a bot with a supply skill only (WR15: the pass's cost per demand build)
    auto const started = std::chrono::steady_clock::now();
    for (SupplyRecipe const& recipe : SupplyRecipes(recipes))
    {
        bool const skilled =
            recipe.skill == static_cast<std::uint16_t>(ProfessionRules::Skill::Cooking) ? cooks : firstAid;
        if (!skilled || !bot->HasSpell(recipe.spell))
            continue;
        bool held = false;
        for (std::size_t i = 0; i < out.list.size(); ++i)
            if (out.list[i].item.entry == recipe.product)
            {
                out.made[i] = 1;
                held = true;
            }
        ItemTemplate const* proto = held ? nullptr : sObjectMgr->GetItemTemplate(recipe.product);
        if (proto && proto->Class == ITEM_CLASS_CONSUMABLE)
            AddRecords(bot, proto, 0, 0, index, out, false, true);
    }
    // Rounded to the microsecond, not cut: the pass may take less than one
    auto const ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "item demand made"))
        row->Record(static_cast<uint64>(std::max<long long>((ns + 500) / 1000, 0)));
}

void ReadBags(Player* bot, DemandList& out, bool withBank)
{
    ItemUseIndex const& index = ItemUseIndex::Get();
    for (std::uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        AddItem(bot, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), index, out);
    for (std::uint8_t bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* pack = bot->GetBagByPos(bag))
            for (std::uint32_t slot = 0; slot < pack->GetBagSize(); ++slot)
                AddItem(bot, pack->GetItemByPos(static_cast<std::uint8_t>(slot)), index, out);
    // The bank's 28 item slots, as the bank rules read them (bank bags are not theirs)
    if (withBank)
        for (std::uint8_t slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
            AddItem(bot, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), index, out);
}

// Demand asks what the bot would use, not whether to coat now: the views carry no coating
TargetView WornView(Player* bot, std::uint8_t slot)
{
    TargetView view = ViewOfWeapon(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    view.itemHasCoating = false;
    return view;
}

DemandAnswer const& NoDemand()
{
    static DemandAnswer const none;
    return none;
}

std::uint32_t HeldCount(std::vector<DemandHeld> const& held, std::uint32_t entry)
{
    for (DemandHeld const& item : held)
        if (item.entry == entry)
            return item.count;
    return 0;
}
}  // namespace

bool DemandAnswers(PlayerbotAI* botAI)
{
    return botAI && botAI->GetBot() && botAI->KeepsPossessions() && !ItemUseIndex::Get().All().empty();
}

DemandBuilt BuildDemand(PlayerbotAI* botAI, Player* bot, bool withBank)
{
    auto const started = std::chrono::steady_clock::now();
    DemandBuilt built;
    DemandList bags;
    ReadBags(bot, bags, withBank);
    built.held = bags.held;
    built.role = RoleOf(bot);

    std::optional<TraitRules::BotTraits> const traits = BotTraitMgr::instance().Find(bot->GetGUID().GetCounter());
    WorldTravel::SlotMeasure const& measure =
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    WorldTravel::SlotRates const rates = WorldTravel::CurrentRates(
        measure, sPlayerbotAIConfig.WorldTravel.SlotBands[WorldTravel::LevelBand(bot->GetLevel())]);
    built.earnCopperPerMinute = static_cast<std::uint32_t>(std::clamp(rates.EarnCopperPerMinute, 0.0f, 4294967295.0f));

    BuffWeights const weights = WeightsOf(bot, built.role, bags.list.empty() ? 0 : bags.list.front().item.entry);
    AddOffers(bot, bags);
    AddMade(bot, bags);
    std::vector<BuffWeights> coatStats;
    std::vector<PoisonKind> poisonKinds;
    std::vector<DemandSkill> skills;
    coatStats.reserve(bags.records.size());
    poisonKinds.reserve(bags.records.size());
    for (Record const& record : bags.records)
    {
        bool const coating = record.exclude == Exclude::Coating;
        coatStats.push_back(coating ? CoatingStats(record) : BuffWeights{});
        poisonKinds.push_back(coating && record.needs.classMask ? PoisonKindOf(record) : PoisonKind::None);
        if (record.needs.skill && std::none_of(skills.begin(), skills.end(), [&](DemandSkill const& skill)
                                               { return skill.skill == record.needs.skill; }))
            skills.push_back({record.needs.skill, static_cast<std::uint16_t>(bot->GetSkillValue(record.needs.skill))});
    }

    DemandInput input;
    input.cls = static_cast<std::uint8_t>(bot->getClass());
    input.race = static_cast<std::uint8_t>(bot->getRace(true));
    input.level = static_cast<std::uint8_t>(std::min<std::uint32_t>(bot->GetLevel(), 255));
    input.role = built.role;
    input.classBit = 1u << (bot->getClass() - 1);
    input.raceBit = bot->getRaceMask();
    input.usesMana = bot->GetMaxPower(POWER_MANA) > 0;
    input.skills = skills;
    input.thrift = traits ? SpendThrift(*traits) : 0.0f;
    input.daring = traits ? SpendDaring(*traits) : 0.0f;
    input.earnCopperPerMinute = built.earnCopperPerMinute;
    input.deathCopper = DeathCopper(built.earnCopperPerMinute, 0);
    input.persistent = botAI->KeepsPossessions();
    input.control = InControl(botAI, bot);
    for (std::size_t i = 0; i < input.upkeepUsePerHour.size() && i < measure.UsePerHour.size(); ++i)
        input.upkeepUsePerHour[i] = measure.UsePerHour[i];
    input.list = bags.list;
    input.records = bags.records;
    input.canUse = bags.canUse;
    input.offered = bags.offered;
    input.made = bags.made;
    input.buyCopper = bags.buy;
    input.coatStats = coatStats;
    input.poisonKinds = poisonKinds;
    input.statWeights = weights;
    input.mainHand = WornView(bot, EQUIPMENT_SLOT_MAINHAND);
    input.offHand = WornView(bot, EQUIPMENT_SLOT_OFFHAND);
    built.answer = Demand(input);

    // Its own PerfMonitor row (microseconds on the thread that builds: the bot's map thread, or the world thread
    // for the command)
    auto const us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    if (PerformanceData* row = sPerfMonitor.acquire(PERF_MON_TOTAL, "item demand"))
        row->Record(static_cast<uint64>(std::max<long long>(us, 0)));
    return built;
}

DemandAnswer const& DemandOf(PlayerbotAI* botAI, std::uint32_t entry)
{
    if (!DemandAnswers(botAI))
        return NoDemand();

    Player* bot = botAI->GetBot();
    BotItems& items = ItemsOf(botAI);
    if (!items.demand)
        items.demand = std::make_shared<DemandCache>();
    DemandCache& cache = *items.demand;
    std::uint32_t const now = getMSTime();
    bool stale = !cache.valid || getMSTimeDiff(cache.builtMs, now) >= DEMAND_REFRESH_MS;
    if (!stale && entry)
        stale = bot->GetItemCount(entry, false) != HeldCount(cache.built.held, entry);
    if (stale)
    {
        cache.built = BuildDemand(botAI, bot);
        cache.builtMs = now;
        cache.valid = true;
    }
    return cache.built.answer;
}

DemandVerdict DemandVerdictOf(PlayerbotAI* botAI, std::uint32_t entry)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || !DemandAnswers(botAI))
        return DemandVerdict::None;
    return VerdictOf(DemandOf(botAI, entry), entry);
}

std::uint32_t DemandWorthEach(PlayerbotAI* botAI, std::uint32_t entry)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || !DemandAnswers(botAI))
        return 0;
    return WorthEachOf(DemandOf(botAI, entry), entry);
}
}  // namespace ItemUse
