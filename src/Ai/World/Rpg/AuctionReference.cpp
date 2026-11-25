/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionReference.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "AuctionHouseMgr.h"
#include "AuctionPricing.h"
#include "AuctionRules.h"
#include "Chat.h"
#include "Config.h"
#include "GitRevision.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PlannerIndexes.h"
#include "PlannerRecipes.h"
#include "PlayerbotAIConfig.h"
#include "ThreadCheck.h"
#include "World.h"
#include "WorldTravelConfig.h"

namespace AuctionReference
{
namespace
{
// What the startup gather hands the worker: written once on the world thread before the recipe index's task is
// posted, taken once by that task
struct Gathered
{
    std::vector<AuctionRules::ItemInput> inputs;
    WorldTravel::SlotBandTable bands = WorldTravel::DEFAULT_SLOT_BANDS;
    std::string coreRevision;
};

std::shared_ptr<Gathered> gathered;
Planner::SharedSlot<AuctionRules::Table> current;

// The houses' fees: written once on the world thread at startup, then read only
constexpr std::array<std::uint8_t, 3> HOUSES = {AuctionRules::HOUSE_ALLIANCE, AuctionRules::HOUSE_HORDE,
                                                AuctionRules::HOUSE_NEUTRAL};
std::array<AuctionRules::Fees, 3> houseFees{};
std::atomic<bool> feesRead{false};

std::size_t FeeSlot(std::uint8_t houseId)
{
    return houseId == AuctionRules::HOUSE_ALLIANCE ? 0 : houseId == AuctionRules::HOUSE_HORDE ? 1 : 2;
}

void ReadFees()
{
    AuctionRules::FeeRates rates;
    rates.deposit = sWorld->getRate(RATE_AUCTION_DEPOSIT);
    rates.cut = sWorld->getRate(RATE_AUCTION_CUT);
    rates.time = sWorld->getRate(RATE_AUCTION_TIME);
    for (std::uint8_t const house : HOUSES)
    {
        AuctionRules::Fees& fees = houseFees[FeeSlot(house)];
        fees.rates = rates;
        if (AuctionHouseEntry const* entry =
                AuctionHouseMgr::GetAuctionHouseEntryFromHouse(static_cast<AuctionHouseId>(house)))
            fees.row = {entry->depositPercent, entry->cutPercent};
        else
            LOG_ERROR("server.loading", "Auction fees: no AuctionHouse.dbc row for house {}, the core's defaults kept",
                      static_cast<std::uint32_t>(house));
    }
    feesRead.store(true, std::memory_order_release);
    auto row = [](std::uint8_t house) { return houseFees[FeeSlot(house)].row; };
    LOG_INFO(
        "server.loading",
        "Auction fees: house 2 deposit {} cut {}, house 6 deposit {} cut {}, house 7 deposit {} cut {}, rates d {} "
        "c {} t {}, shared house {}",
        row(AuctionRules::HOUSE_ALLIANCE).depositPercent, row(AuctionRules::HOUSE_ALLIANCE).cutPercent,
        row(AuctionRules::HOUSE_HORDE).depositPercent, row(AuctionRules::HOUSE_HORDE).cutPercent,
        row(AuctionRules::HOUSE_NEUTRAL).depositPercent, row(AuctionRules::HOUSE_NEUTRAL).cutPercent, rates.deposit,
        rates.cut, rates.time, sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION) ? "yes" : "no");
}

std::uint32_t MsSince(std::chrono::steady_clock::time_point start)
{
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

// A bare file name in the core's logs folder, as `playerbots professions index dump` takes it
std::string DumpPath(std::string const& name)
{
    if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos)
        return {};
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "", false);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    return dir + name;
}

void SendLine(ChatHandler* handler, AuctionRules::Table const& table, std::uint32_t id, std::string const& group)
{
    AuctionRules::ItemReference const* reference = table.Find(id);
    AuctionRules::ItemInput const* item = table.Input(id);
    if (!item)
    {
        handler->PSendSysMessage("AUCTION ref {}: no item", id);
        return;
    }
    if (!reference)
    {
        handler->PSendSysMessage("AUCTION ref {} {}: left out ({})", id, item->name,
                                 AuctionRules::LeftOutWord(AuctionRules::LeftOutOf(*item)));
        return;
    }
    std::string line = AuctionRules::Line(*reference, *item);
    if (!group.empty())
        line += " [" + group + "]";
    handler->SendSysMessage(line);
}
}  // namespace

void Gather()
{
    PLAYERBOT_CHECK_WORLD_THREAD("AuctionReference::Gather");
    ReadFees();
    auto const start = std::chrono::steady_clock::now();
    auto data = std::make_shared<Gathered>();

    // The items a spawned vendor sells for gold without a stock limit (the recipe index's rule for materials)
    std::unordered_set<uint32> spawned;
    for (auto const& [spawnId, creature] : sObjectMgr->GetAllCreatureData())
        for (uint32 const id : {creature.id, creature.id2, creature.id3})
            if (id)
                spawned.insert(id);
    std::unordered_set<uint32> stocked;
    for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
    {
        if (!(creature.npcflag & UNIT_NPC_FLAG_VENDOR) || !spawned.count(entry))
            continue;
        VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(entry);
        if (!list)
            continue;
        for (VendorItem const* sold : list->m_items)
            if (sold && !sold->ExtendedCost && !sold->maxcount)
                stocked.insert(sold->item);
    }

    for (ItemTemplate const* proto : *sObjectMgr->GetItemTemplateStoreFast())
    {
        if (!proto)
            continue;
        AuctionRules::ItemInput item;
        item.id = proto->ItemId;
        item.name = proto->Name1;
        item.classId = proto->Class;
        item.subClass = proto->SubClass;
        item.inventoryType = proto->InventoryType;
        item.quality = proto->Quality;
        item.itemLevel = proto->ItemLevel;
        item.requiredLevel = proto->RequiredLevel;
        item.bonding = proto->Bonding;
        item.flags = static_cast<uint32>(proto->Flags);
        item.duration = proto->Duration;
        item.buyPrice = static_cast<uint32>(std::max<int32>(proto->BuyPrice, 0));
        item.sellPrice = proto->SellPrice;
        item.buyCount = std::max<uint32>(proto->BuyCount, 1);
        item.vendorStocked = stocked.count(proto->ItemId) != 0;
        data->inputs.push_back(std::move(item));
    }
    data->bands = sPlayerbotAIConfig.WorldTravel.SlotBands;
    data->coreRevision = GitRevision::GetHash();
    LOG_INFO("server.loading", "Auction reference: gathered {} item templates, {} items vendors always stock, in {} ms",
             data->inputs.size(), stocked.size(), MsSince(start));
    gathered = std::move(data);
}

AuctionRules::Fees HouseFees(std::uint8_t houseId)
{
    if (!feesRead.load(std::memory_order_acquire))
        return {};
    return houseFees[FeeSlot(houseId)];
}

void OnRecipeIndexReady(std::shared_ptr<Recipes::RecipeIndex const> index)
{
    std::shared_ptr<Gathered> data = std::move(gathered);
    if (!index || !data)
    {
        LOG_ERROR("playerbots", "Auction reference: no table ({} missing)", index ? "the gathered items" : "the index");
        return;
    }
    try
    {
        auto const start = std::chrono::steady_clock::now();
        auto table =
            std::make_shared<AuctionRules::Table>(AuctionRules::Build(std::move(data->inputs), *index, data->bands));
        table->coreRevision = data->coreRevision;
        table->sequence = 1;
        table->buildMs = MsSince(start);
        AuctionRules::Counts const& n = table->counts;
        using AuctionRules::Basis;
        LOG_INFO("server.loading",
                 "Auction reference: {} items ({} gathered, {} made, {} processed, {} vendor-capped, {} shaped, {} "
                 "floor; {} left out) in {} ms on a worker, table seq {}",
                 table->rows.size(), n.Of(Basis::Gathered), n.Of(Basis::Made), n.Of(Basis::Processed),
                 n.Of(Basis::VendorCap), n.Of(Basis::Shaped), n.Of(Basis::Floor), n.LeftOutTotal(), table->buildMs,
                 table->sequence);
        current.Publish(std::move(table));
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("playerbots", "Auction reference: the build failed: {}", e.what());
    }
}

std::shared_ptr<AuctionRules::Table const> Current() { return current.Current(); }

bool HandleRefCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("AuctionReference::HandleRefCommand");
    std::shared_ptr<AuctionRules::Table const> const table = Current();
    if (!table)
    {
        handler->SendErrorMessage("AUCTION ref: no table yet ({})", Planner::RecipeIndexStatus().front());
        return false;
    }
    std::string text = args ? args : "";
    std::string const word = text.substr(0, text.find(' '));
    if (word == "sample")
    {
        std::shared_ptr<Recipes::RecipeIndex const> const index = Planner::CurrentRecipeIndex();
        if (!index)
        {
            handler->SendErrorMessage("AUCTION ref: no recipe index");
            return false;
        }
        AuctionRules::Counts const& n = table->counts;
        handler->PSendSysMessage(
            "AUCTION ref table seq {}: {} items, {} left out, {} cut off by the recipe bounds, "
            "core {}, built in {} ms",
            table->sequence, table->rows.size(), n.LeftOutTotal(), n.bound, table->coreRevision, table->buildMs);
        for (AuctionRules::SampleEntry const& entry : AuctionRules::Sample(*table, *index))
            SendLine(handler, *table, entry.id, entry.group);
        return true;
    }
    if (word == "dump")
    {
        std::string const rest = text.size() > word.size() ? text.substr(word.size() + 1) : std::string();
        std::string const path = DumpPath(rest.empty() ? "auction-reference-inputs.txt" : rest);
        if (path.empty())
        {
            handler->SendErrorMessage("AUCTION ref dump: give a bare file name; the dump goes into the logs folder");
            return false;
        }
        std::string const dump = AuctionRules::DumpInputs(table->inputs, table->coreRevision);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(dump.data(), static_cast<std::streamsize>(dump.size()));
        out.flush();
        if (!out)
        {
            handler->SendErrorMessage("AUCTION ref dump: cannot write {}", path);
            return false;
        }
        handler->PSendSysMessage("AUCTION ref dump: {} item rows to {}", table->inputs.size(), path);
        return true;
    }
    char const* key = text.empty() ? nullptr : handler->extractKeyFromLink(text.data(), "Hitem");
    std::uint32_t const id = key ? static_cast<std::uint32_t>(std::strtoul(key, nullptr, 10)) : 0;
    if (!id || !table->Input(id))
    {
        handler->SendErrorMessage(
            "AUCTION ref: no item '{}'; usage: playerbots auction ref <item id or link> | sample | dump [<file name>]",
            args ? args : "");
        return false;
    }
    SendLine(handler, *table, id, "");
    return true;
}
}  // namespace AuctionReference
