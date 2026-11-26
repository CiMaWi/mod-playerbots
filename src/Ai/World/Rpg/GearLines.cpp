/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GearLines.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

#include "AiObjectContext.h"
#include "GameTime.h"
#include "GearWorthInput.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpGearMgr.h"
#include "RandomPlayerbotMgr.h"

static_assert(EQUIPMENT_SLOT_END == GearWorth::EQUIPMENT_SLOTS, "the pure rule numbers the core's equipment slots");
static_assert(EQUIPMENT_SLOT_BODY == GearWorth::SLOT_SHIRT && EQUIPMENT_SLOT_OFFHAND == GearWorth::SLOT_OFF_HAND &&
                  EQUIPMENT_SLOT_TABARD == GearWorth::SLOT_TABARD,
              "the pure rule's named slots are the core's");

namespace
{
bool IsGear(ItemTemplate const* proto)
{
    return (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) &&
           proto->InventoryType != INVTYPE_NON_EQUIP;
}

// The upgrade rule (R1) on the item with its suffix: the bot would wear it (R4: EQUIP or REPLACE). Asked only for the
// offers PickPerSlot names: each new qualifier keeps a value object
bool Wears(PlayerbotAI* botAI, uint32 entry, int32 randomPropertyId)
{
    std::string const qualifier =
        randomPropertyId ? std::to_string(entry) + "," + std::to_string(randomPropertyId) : std::to_string(entry);
    ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item upgrade", qualifier)->Get();
    return usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE;
}

// The slot of the no_offer row: the worn items' levels, the off hand blocked under a two-hander without titan grip
uint8 NoOfferSlotOf(Player* bot, uint32 filled)
{
    std::array<GearWorth::WornSlot, GearWorth::EQUIPMENT_SLOTS> worn{};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            worn[slot] = {true, item->GetTemplate()->ItemLevel};

    uint32 blocked = filled;
    Item const* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (mainHand && mainHand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON && !bot->CanTitanGrip())
        blocked |= 1u << EQUIPMENT_SLOT_OFFHAND;
    return GearWorth::NoOfferSlot(worn, blocked);
}

// One entry's reads by suffix: the lots of an entry share them
struct SuffixRead
{
    int32 randomPropertyId{0};
    std::optional<GearWorth::LiveInput> input;
};
}  // namespace

GearWorth::AuctionGear GearWorth::LinesAt(PlayerbotAI* botAI, AuctionIndex::Snapshot const& snapshot, uint8 houseId,
                                          std::vector<uint32> const& only, uint64 spendable)
{
    AuctionGear out;
    Player* bot = botAI->GetBot();
    // As the vendor (BuyAction::BuyGearByWorth): a bought item is worn at once (R4), so nothing is bought where the
    // equip pass would refuse
    if (!botAI->KeepsPossessions() ||
        (!sPlayerbotAIConfig.AutoEquipUpgradeLoot && !sRandomPlayerbotMgr.IsRandomBot(bot)) ||
        PvpGearMgr::Instance().IsWorn(bot))
        return out;
    // A test's filter naming no weapon or armour asks nothing of gear: no line and no no_offer row
    if (!only.empty() && std::none_of(only.begin(), only.end(),
                                      [](uint32 entry)
                                      {
                                          ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
                                          return proto && IsGear(proto);
                                      }))
        return out;

    uint64 const ownGuid = bot->GetGUID().GetRawValue();
    int64 const now = static_cast<int64>(GameTime::GetGameTime().count());
    std::optional<InputReader> reader;  // set up at the first candidate lot
    AuctionRules::CopperWeight const weight = Planner::CopperWeightOf(botAI);
    std::vector<Offer> offers;
    std::vector<AuctionIndex::OfferLine const*> lots;  // the offers' lots, in their order
    std::vector<ItemTemplate const*> protos;
    uint32 filled = 0;  // the slots of gear lots the bot leads a bid on
    std::vector<SuffixRead> reads;
    for (auto const& [item, entry] : snapshot.items)
    {
        if (!only.empty() && std::find(only.begin(), only.end(), item) == only.end())
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        if (!proto || !IsGear(proto))
            continue;

        reads.clear();
        std::optional<bool> usable;  // asked at the entry's first lot of this house
        for (AuctionIndex::OfferLine const& lot : entry.offers.cheapest)
        {
            if (lot.houseId != houseId || lot.endTime < now)
                continue;
            if (!usable)
                usable = !bot->HasItemCount(item) && bot->CanUseItem(proto) == EQUIP_ERR_OK;
            if (!*usable)
                break;

            auto read = std::find_if(reads.begin(), reads.end(), [&](SuffixRead const& known)
                                     { return known.randomPropertyId == lot.randomPropertyId; });
            if (read == reads.end())
            {
                if (!reader)
                    reader.emplace(botAI);
                reads.push_back({lot.randomPropertyId, reader->Read(proto, lot.randomPropertyId)});
                read = std::prev(reads.end());
            }
            if (!read->input)
                continue;
            if (lot.bidder == ownGuid)
            {
                filled |= read->input->slots;
                continue;
            }

            Offer offer;
            offer.item = item;
            offer.slot = read->input->slot;
            offer.slots = read->input->slots;
            offer.worth = Worth(read->input->worth);
            if (!offer.worth)
                continue;
            offer.price =
                static_cast<uint32>(std::min<uint64>(AuctionRules::RankPrice(lot), std::numeric_limits<uint32>::max()));
            offer.ceiling = CeilingOf(offer.worth, 0, weight);
            offers.push_back(offer);
            lots.push_back(&lot);
            protos.push_back(proto);
        }
    }

    std::vector<std::size_t> const picks =
        PickPerSlot(offers, spendable, [&](std::size_t i)
                    { return !(offers[i].slots & filled) && Wears(botAI, offers[i].item, lots[i]->randomPropertyId); });
    for (std::size_t const i : picks)
    {
        AuctionRules::BuyLine line = GearLine(offers[i].item, offers[i].worth, 0, weight);
        line.auctionId = lots[i]->id;
        line.excessUnitWorth = protos[i]->SellPrice;
        out.lines.push_back(line);
    }
    if (out.lines.empty())
        out.noOfferSlot = NoOfferSlotOf(bot, filled);
    return out;
}
