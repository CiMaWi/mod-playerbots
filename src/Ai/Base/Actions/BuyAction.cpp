/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BuyAction.h"

#include <algorithm>
#include <limits>
#include <optional>

#include "AuctionBuyList.h"
#include "BudgetValues.h"
#include "DBCStores.h"
#include "Event.h"
#include "EventLog.h"
#include "GearWorthInput.h"
#include "InventorySlots.h"
#include "ItemCountValue.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "ItemWorthRules.h"
#include "NewRpgErrandInternal.h"
#include "PlannerService.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpGearMgr.h"
#include "StatsWeightCalculator.h"
#include "Upkeep.h"
#include "UpkeepNeedRules.h"
#include "WorldLog.h"
#include "WorldTravelConfig.h"

struct BuyAction::GearVisit
{
    uint32 filled{0};            // one bit per equipment slot a purchase of this visit fills
    uint32 refused{0};           // one bit per replaced slot whose refusal is written
    std::vector<uint32> failed;  // items a vendor did not hand over
    std::optional<uint64> kept;  // the copper the needs keep in the purse, read at the first candidate
};

namespace
{
bool IsGear(ItemTemplate const* proto) { return proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR; }

bool IsEquipUsage(ItemUsage usage)
{
    return usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_BAD_EQUIP ||
           usage == ITEM_USAGE_BROKEN_EQUIP;
}

// The copper gear leaves in the purse, as the market does (AuctionVisit.cpp): from the trip's needs list; without a
// needs list nothing is spendable
uint64 KeptForNeeds(PlayerbotAI* botAI)
{
    std::shared_ptr<Planner::UpkeepIndex const> const upkeep =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    std::shared_ptr<UpkeepNeeds::VisitNeeds const> const needs =
        upkeep ? NewRpgInternal::ErrandNeeds(botAI, upkeep) : nullptr;
    return needs ? UpkeepNeeds::MarketKept(needs->needs) : std::numeric_limits<uint64>::max();
}

void LogGearOffer(Player* bot, std::string_view choice, std::string_view reason, GearWorth::Offer const& offer,
                  uint64 spendable)
{
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Gear, choice, reason, static_cast<float>(offer.worth),
                       {{"price", static_cast<float>(offer.price)},
                        {"ceiling", static_cast<float>(offer.ceiling)},
                        {"spare", static_cast<float>(spendable)}},
                       offer.item);
}

// A persistent bot buys a vendor bag when its price is at most its worth (the slots it adds over the bag it replaces,
// at the bot's need factor as the auction's bag line, AH.5) and fits the budget above the money reserve (WT.5)
bool ShouldBuyVendorBag(PlayerbotAI* botAI, Player* bot, AiObjectContext* context, ItemTemplate const* proto,
                        uint32 price)
{
    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    WorldTravel::SlotBand const& band = config.SlotBands[WorldTravel::LevelBand(bot->GetLevel())];
    WorldTravel::SlotRates const rates =
        WorldTravel::CurrentRates(context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet(), band);

    WorldTravel::BagBuyInput input;
    input.Price = price;
    input.Worth =
        AuctionRules::BagCeiling(WorldTravel::BagWorth(rates, WorldTravel::FreeGeneralSlots(bot), proto->ContainerSlots,
                                                       ItemUsageValue::SmallestBagSize(bot), band.BagHorizonHours),
                                 0, Planner::CopperWeightOf(botAI));
    input.Gold = bot->GetMoney();
    // Everything saved before gear: guild, repairs, ammo, training and travel
    input.Reserve = AI_VALUE2(uint32, "total money needed for", uint32(NeedMoneyFor::gear)) -
                    AI_VALUE2(uint32, "money needed for", uint32(NeedMoneyFor::gear));
    input.ThriftPercent = config.BagThriftPercent;
    input.BudgetPercent = config.BagBudgetPercent;
    return WorldTravel::ShouldBuyBag(input);
}
}  // namespace

bool BuyAction::Execute(Event event)
{
    bool buyUseful = false;
    ItemIds itemIds;
    std::string const link = event.getParam();

    if (link == "vendor")
        buyUseful = true;
    else
    {
        itemIds = chat->parseItems(link);
    }

    GuidVector vendors = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();

    bool vendored = false;
    bool result = false;
    // Persistent bots buy bags by worth, at most one per visit; a planner bot's bag is its planner's order alone
    // (BuyOrderedBag, planning WT63e: one owner per decision)
    bool const honestBags = buyUseful && botAI->KeepsPossessions();
    bool const orderedBags = Planner::Owns(botAI);
    bool boughtBag = false;
    // Their gear is bought by worth, one purchase per equipment slot over all vendors of the visit
    GearVisit gearVisit;
    // A bot not granted them gets its consumables, ammo and class reagents from its shopping list (upkeep §3.6, WU28)
    bool const shopsAmmo = !Upkeep::Granted(bot, UpkeepKind::Ammo);
    bool const shopsReagents = !Upkeep::Granted(bot, UpkeepKind::Reagent);
    bool const consumablesGranted = Upkeep::Granted(bot, UpkeepKind::Consumable);
    std::vector<uint32> const classReagents =
        shopsReagents ? ItemUsageValue::ClassSpellReagents(bot) : std::vector<uint32>();
    for (GuidVector::iterator i = vendors.begin(); i != vendors.end(); ++i)
    {
        ObjectGuid vendorguid = *i;
        Creature* pCreature = bot->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR);
        if (!pCreature)
            continue;

        vendored = true;

        if (buyUseful)
        {
            // Items are evaluated from high-level to low level.
            // For each item the bot checks again if an item is usefull.
            // Bot will buy until no usefull items are left.

            VendorItemData const* tItems = pCreature->GetVendorItems();
            if (!tItems)
                continue;

            if (honestBags)
                BuyGearByWorth(pCreature, vendorguid, tItems, gearVisit);

            VendorItemList m_items_sorted = tItems->m_items;

            m_items_sorted.erase(std::remove_if(m_items_sorted.begin(), m_items_sorted.end(),
                                                [](VendorItem* i)
                                                {
                                                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(i->item);
                                                    return !proto;
                                                }),
                                 m_items_sorted.end());

            if (m_items_sorted.empty())
                continue;

            StatsWeightCalculator calculator(bot);
            calculator.SetItemSetBonus(false);
            calculator.SetOverflowPenalty(false);

            // Score each item once; the comparator must be a strict weak ordering, and the old
            // "if either score is 0, compare item level" branch was not.
            std::unordered_map<uint32, std::pair<float, uint32>> itemScores;
            for (VendorItem const* vendorItem : m_items_sorted)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
                itemScores[vendorItem->item] = {calculator.CalculateItem(vendorItem->item),
                                                proto ? proto->ItemLevel : 0};
            }

            std::sort(m_items_sorted.begin(), m_items_sorted.end(),
                [&itemScores](VendorItem* i, VendorItem* j)
                {
                    auto const& score1 = itemScores.at(i->item);
                    auto const& score2 = itemScores.at(j->item);
                    if (score1.first != score2.first)
                        return score1.first > score2.first;
                    return score1.second > score2.second;
                });

            std::unordered_map<uint32, float> bestPurchasedItemScore;  // Track best item score per InventoryType

            for (auto& tItem : m_items_sorted)
            {
                uint32 maxPurchases = 1;  // Default to buying once
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tItem->item);
                if (!proto)
                    continue;

                if (proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_PROJECTILE)
                {
                    maxPurchases = 10;  // Allow up to 10 purchases if it's a consumable or projectile
                }

                for (uint32 i = 0; i < maxPurchases; i++)
                {
                    // Stock and currency costs change between purchases; recheck each time.
                    if (!CanAfford(tItem, proto, pCreature))
                        break;

                    if (honestBags && proto->Class == ITEM_CLASS_CONTAINER &&
                        proto->SubClass == ITEM_SUBCLASS_CONTAINER)
                    {
                        uint32 const bagPrice =
                            uint32(floor(proto->BuyPrice * bot->GetReputationPriceDiscount(pCreature)));
                        // A bag bought earlier that could not be equipped yet would be bought again
                        if (boughtBag || orderedBags ||
                            WorldTravel::CarriesSpareBag(bot, ItemUsageValue::SmallestBagSize(bot)) ||
                            !ShouldBuyVendorBag(botAI, bot, context, proto, bagPrice) ||
                            !BuyItem(tItems, vendorguid, proto))
                            break;

                        boughtBag = true;
                        EventLog::Emit(bot, EventLog::EventType::Inventory, "bag_bought", proto->ItemId, bagPrice);
                        botAI->DoSpecificAction("equip upgrades packet action");
                        break;
                    }

                    // Any usage: an ammo upgrade is EQUIP
                    if (shopsAmmo && proto->Class == ITEM_CLASS_PROJECTILE)
                        break;

                    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", tItem->item);

                    // A persistent bot's gear is bought by its worth alone (BuyGearByWorth), never here
                    if (GearWorth::RouteOf({honestBags, IsGear(proto), IsEquipUsage(usage)}) != GearWorth::Route::Old)
                        break;

                    uint32 invType = proto->InventoryType;

                    // Reuse the score computed for the sort.
                    float newScore = itemScores.at(proto->ItemId).first;

                    // Skip if we already bought a better item for this slot
                    if (bestPurchasedItemScore.find(invType) != bestPurchasedItemScore.end() &&
                        bestPurchasedItemScore[invType] > newScore)
                    {
                        break;  // Skip lower-scoring items
                    }

                    // Check the bot's currently equipped item for this slot
                    uint8 dstSlot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
                    Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot);

                    float oldScore = 0.0f;
                    if (oldItem)
                    {
                        ItemTemplate const* oldItemProto = oldItem->GetTemplate();
                        if (oldItemProto)
                            oldScore = calculator.CalculateItem(oldItemProto->ItemId);
                    }

                    // Skip if the bot already has a better or equal item equipped
                    if (oldScore > newScore)
                        break;

                    uint32 price = proto->BuyPrice;
                    price = uint32(floor(price * bot->GetReputationPriceDiscount(pCreature)));

                    NeedMoneyFor needMoneyFor = NeedMoneyFor::none;
                    switch (usage)
                    {
                        case ITEM_USAGE_REPLACE:
                        case ITEM_USAGE_EQUIP:
                        case ITEM_USAGE_BAD_EQUIP:
                        case ITEM_USAGE_BROKEN_EQUIP:
                            needMoneyFor = NeedMoneyFor::gear;
                            break;
                        case ITEM_USAGE_AMMO:
                            needMoneyFor = NeedMoneyFor::ammo;
                            break;
                        case ITEM_USAGE_QUEST:
                            needMoneyFor = NeedMoneyFor::anything;
                            break;
                        case ITEM_USAGE_USE:
                            if (consumablesGranted)
                                needMoneyFor = NeedMoneyFor::consumables;
                            break;
                        case ITEM_USAGE_SKILL:
                            // Profession materials keep the tradeskill budget
                            if (UpkeepNeeds::GenericBuysSkillItem(shopsReagents, classReagents, proto->ItemId))
                                needMoneyFor = NeedMoneyFor::tradeskill;
                            break;
                        default:
                            break;
                    }

                    if (needMoneyFor == NeedMoneyFor::none)
                        break;

                    if (AI_VALUE2(uint32, "free money for", uint32(needMoneyFor)) < price)
                        break;

                    if (!BuyItem(tItems, vendorguid, proto))
                        break;

                    // Store the best item score per InventoryType
                    bestPurchasedItemScore[invType] = newScore;

                    if (needMoneyFor == NeedMoneyFor::gear)
                    {
                        botAI->DoSpecificAction("equip upgrades packet action");
                    }
                }
            }
        }
        else
        {
            if (itemIds.empty())
                return false;

            for (ItemIds::iterator i = itemIds.begin(); i != itemIds.end(); i++)
            {
                uint32 itemId = *i;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto)
                    continue;

                result |= BuyItem(pCreature->GetVendorItems(), vendorguid, proto);

                if (!result)
                {
                    std::ostringstream out;
                    out << "Nobody sells " << ChatHelper::FormatItem(proto) << " nearby";
                    botAI->TellMaster(out.str());
                    continue;
                }

                ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemId);
                if (usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP ||
                    usage == ITEM_USAGE_BAD_EQUIP || usage == ITEM_USAGE_BROKEN_EQUIP)
                {
                    botAI->DoSpecificAction("equip upgrades packet action");
                    break;
                }
            }
        }
    }

    return vendored;
}

void BuyAction::BuyGearByWorth(Creature* vendor, ObjectGuid vendorguid, VendorItemData const* tItems, GearVisit& visit)
{
    // A bought item is worn at once (R4): nothing is bought where the equip pass would refuse
    if ((!sPlayerbotAIConfig.AutoEquipUpgradeLoot && !sRandomPlayerbotMgr.IsRandomBot(bot)) ||
        PvpGearMgr::Instance().IsWorn(bot))
        return;

    std::vector<GearWorth::Offer> offers;
    std::vector<ItemTemplate const*> protos;  // the offers' items, in their order
    // Each purchase changes what is worn and the gold left, so the rule's answers, the worths and the copper
    // weight are read again after it
    while (true)
    {
        std::optional<AuctionRules::CopperWeight> weight;
        offers.clear();
        protos.clear();
        for (VendorItem const* vendorItem : tItems->m_items)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
            // A carried copy that could not be worn would be bought again
            if (!proto || !IsGear(proto) || bot->HasItemCount(proto->ItemId) ||
                std::find(visit.failed.begin(), visit.failed.end(), proto->ItemId) != visit.failed.end() ||
                !CanAfford(vendorItem, proto, vendor))
                continue;

            GearWorth::RouteInput route{true, true};
            route.equipUsage = IsEquipUsage(AI_VALUE2(ItemUsage, "item usage", proto->ItemId));
            if (route.equipUsage)
            {
                ItemUsage const upgrade = AI_VALUE2(ItemUsage, "item upgrade", std::to_string(proto->ItemId));
                route.upgrade = upgrade == ITEM_USAGE_EQUIP || upgrade == ITEM_USAGE_REPLACE;
            }
            route.goldPrice = !vendorItem->ExtendedCost;
            if (GearWorth::RouteOf(route) != GearWorth::Route::ByWorth)
                continue;

            std::optional<GearWorth::LiveInput> const input = GearWorth::ReadInput(botAI, proto, 0);
            if (!input || (input->slots & visit.filled))
                continue;

            if (!weight)
                weight = Planner::CopperWeightOf(botAI);

            GearWorth::Offer offer;
            offer.item = proto->ItemId;
            offer.slot = input->slot;
            offer.slots = input->slots;
            offer.worth = GearWorth::Worth(input->worth);
            offer.price = uint32(floor(proto->BuyPrice * bot->GetReputationPriceDiscount(vendor)));
            offer.ceiling = GearWorth::CeilingOf(offer.worth, 0, *weight);
            offers.push_back(offer);
            protos.push_back(proto);
        }

        if (offers.empty())
            return;

        if (!visit.kept)
            visit.kept = KeptForNeeds(botAI);

        uint64 const spendable = AuctionRules::Spendable(bot->GetMoney(), *visit.kept, 0);
        std::size_t const best = GearWorth::BestBuy(offers, visit.filled, spendable);
        if (best == GearWorth::NO_OFFER)
        {
            for (GearWorth::Refusal const& refusal : GearWorth::Refusals(offers, visit.filled, visit.refused))
            {
                GearWorth::Offer const& offer = offers[refusal.offer];
                visit.refused |= 1u << offer.slot;
                if (WorldLog::Active())
                    LogGearOffer(bot, "skip", refusal.noGold ? "no_gold" : "over_worth", offer, spendable);
            }
            return;
        }

        GearWorth::Offer const& offer = offers[best];
        if (!BuyItem(tItems, vendorguid, protos[best]))
        {
            visit.failed.push_back(offer.item);
            continue;
        }

        visit.filled |= offer.slots;
        if (WorldLog::Active())
            LogGearOffer(bot, "buy", "under_worth", offer, spendable);
        botAI->DoSpecificAction("equip upgrades packet action");
    }
}

bool BuyAction::CanAfford(VendorItem const* tItem, ItemTemplate const* proto, Creature* vendor) const
{
    // Limited stock: skip if the vendor has fewer than one purchase remaining.
    if (tItem->maxcount != 0 && vendor->GetVendorItemCurrentCount(tItem) < proto->BuyCount)
        return false;

    // Non-gold costs (honor / arena points, token items, personal arena rating).
    if (tItem->ExtendedCost)
    {
        ItemExtendedCostEntry const* iece = sItemExtendedCostStore.LookupEntry(tItem->ExtendedCost);
        if (!iece)
            return false;

        if (bot->GetHonorPoints() < iece->reqhonorpoints)
            return false;
        if (bot->GetArenaPoints() < iece->reqarenapoints)
            return false;

        for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
            if (iece->reqitem[i] && !bot->HasItemCount(iece->reqitem[i], iece->reqitemcount[i]))
                return false;

        if (bot->GetMaxPersonalArenaRatingRequirement(iece->reqarenaslot) < iece->reqpersonalarenarating)
            return false;
    }

    return true;
}

bool BuyAction::BuyItem(VendorItemData const* tItems, ObjectGuid vendorguid, ItemTemplate const* proto)
{
    if (!tItems || !proto)
        return false;

    uint32 itemId = proto->ItemId;
    uint32 oldCount = bot->GetItemCount(itemId, false);

    for (uint32 slot = 0; slot < tItems->GetItemCount(); ++slot)
    {
        if (tItems->GetItem(slot)->item != itemId)
            continue;

        uint32 botMoney = bot->GetMoney();
        if (botAI->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(10000000);
            EventLog::Honesty(bot, EventLog::HonestyKind::Gold, "cheat_gold", 0, int64(10000000) - int64(botMoney));
        }

        bot->BuyItemFromVendorSlot(vendorguid, slot, itemId, 1, NULL_BAG, NULL_SLOT);

        if (botAI->HasCheat(BotCheatMask::gold))
            bot->SetMoney(botMoney);

        uint32 newCount = bot->GetItemCount(itemId, false);
        if (newCount > oldCount)
        {
            std::ostringstream out;
            out << "Buying " << ChatHelper::FormatItem(proto);
            botAI->TellMaster(out.str());
            return true;
        }

        return false;
    }

    return false;
}
