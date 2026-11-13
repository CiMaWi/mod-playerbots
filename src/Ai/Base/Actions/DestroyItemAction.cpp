/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DestroyItemAction.h"
#include "CensusMgr.h"
#include "Event.h"
#include "InventorySlots.h"
#include "ItemCountValue.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "NewRpgTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PvpGearMgr.h"
#include "TestConstraints.h"
#include <algorithm>

namespace
{
// Percent of bag slots used at or above which smart destroy keeps going.
constexpr uint8 BAG_SPACE_DESTROY_THRESHOLD = 90;

std::vector<uint32> DistinctItemIds(std::vector<Item*> const& items)
{
    std::vector<uint32> itemIds;
    for (Item* item : items)
    {
        uint32 const itemId = item->GetEntry();
        if (std::find(itemIds.begin(), itemIds.end(), itemId) == itemIds.end())
            itemIds.push_back(itemId);
    }

    return itemIds;
}

}  // namespace

bool DestroyItemAction::Execute(Event event)
{
    std::string const text = event.getParam();
    ItemIds ids = chat->parseItems(text);

    for (ItemIds::iterator i = ids.begin(); i != ids.end(); i++)
    {
        FindItemByIdVisitor visitor(*i);
        DestroyItem(&visitor);
    }

    return true;
}

void DestroyItemAction::DestroyItem(FindItemVisitor* visitor)
{
    IterateItems(visitor);
    std::vector<Item*> items = visitor->GetResult();
    for (Item* item : items)
        DestroyOne(item);
}

bool DestroyItemAction::DestroyOne(Item* item)
{
    if (PvpGearMgr::Instance().IsWornItem(bot, item))  // a worn PvP item or a displaced PvE item
        return false;

    std::ostringstream out;
    out << chat->FormatItem(item->GetTemplate()) << " destroyed";
    botAI->TellMaster(out);

    bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
    return true;
}

// A test's nodestroy bot destroys nothing on its own (TestConstraints::DestroysForSpace)
bool SmartDestroyItemAction::isUseful()
{
    return !IsRealPlayer(botAI->GetMaster()) && TestConstraints::DestroysForSpace(botAI->rpgInfo.constraints);
}

bool SmartDestroyItemAction::Execute(Event /*event*/)
{
    uint8 bagSpace = AI_VALUE(uint8, "bag space");

    if (bagSpace < BAG_SPACE_DESTROY_THRESHOLD)
        return false;

    // Only destroy grey items when the master is a real player or SelfBot, and the bot is in a real guild.
    if (botAI->HasGameClientMaster() && botAI->IsInRealGuild())
    {
        FindItemsToTradeByQualityVisitor visitor(ITEM_QUALITY_POOR, 5);
        IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);
        DestroyUntilBagSpace(DistinctItemIds(visitor.GetResult()));
        return true;
    }

    std::vector<uint32> bestToDestroy = {ITEM_USAGE_NONE};  // First destroy anything useless.

    if (!AI_VALUE(bool, "can sell") &&
        AI_VALUE(
            bool,
            "should get money"))  // We need money so quest items are less important since they can't directly be sold.
        bestToDestroy.push_back(ITEM_USAGE_QUEST);
    else  // We don't need money so destroy the cheapest stuff.
    {
        bestToDestroy.push_back(ITEM_USAGE_VENDOR);
        bestToDestroy.push_back(ITEM_USAGE_AH);
    }

    // If we still need room
    bestToDestroy.push_back(
        ITEM_USAGE_SKILL);  // Items that might help tradeskill are more important than above but still expenable.
    bestToDestroy.push_back(ITEM_USAGE_USE);  // These are more likely to be usefull 'soon' but still expenable.

    for (auto& usage : bestToDestroy)
    {
        std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(usage));
        std::reverse(items.begin(), items.end());

        if (DestroyUntilBagSpace(DistinctItemIds(items)))
            return true;
    }

    return false;
}

bool SmartDestroyItemAction::DestroyForLoot(uint32 lootWorth)
{
    if (!TestConstraints::DestroysForSpace(botAI->rpgInfo.constraints))
        return false;

    CollectItemsVisitor visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);

    float const sellMultiplier = static_cast<float>(sRandomPlayerbotMgr.GetSellMultiplier(bot));
    std::vector<WorldTravel::SpaceCandidate> candidates;
    candidates.reserve(visitor.items.size());
    uint32 sellableSlots = 0;
    for (Item* item : visitor.items)
    {
        WorldTravel::SpaceCandidate candidate;
        candidate.Protected = !WorldTravel::InGeneralSlot(bot, item) || PvpGearMgr::Instance().IsWornItem(bot, item);
        if (!candidate.Protected)
        {
            ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
            candidate.Worth = ItemUsageValue::ItemWorth(item->GetTemplate(), usage, item->GetCount(), sellMultiplier);
            if (usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH)
                ++sellableSlots;
        }

        candidates.push_back(candidate);
    }

    int32 const victim = WorldTravel::LowestWorthVictim(candidates);
    if (victim == WorldTravel::NO_VICTIM)
        return false;

    WorldTravel::SlotMeasure& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    WorldTravel::DestroyForSpaceInput input;
    input.InInstance = bot->GetMap()->Instanceable();
    // The errand rule with the vendor of the bot's last errand check (when on this map), walked from here
    if (!input.InInstance && measure.HasVendor && measure.VendorMapId == bot->GetMapId())
    {
        WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
        float const walkMinutes = WorldTravel::WalkMinutes(
            bot->GetExactDist(measure.VendorX, measure.VendorY, measure.VendorZ), bot->GetSpeed(MOVE_RUN));
        input.TripWorthIt = WorldTravel::LootWaitsForTrip(
            WorldTravel::ErrandMayStart(botAI), measure, AI_VALUE(uint8, "bag space"), getMSTime(),
            WorldTravel::MakeTripInput(measure, config.SlotBands[WorldTravel::LevelBand(bot->GetLevel())],
                                       WorldTravel::FreeGeneralSlots(bot), sellableSlots, walkMinutes,
                                       config.ErrandNextStopMinutes));
    }
    input.LootWorth = lootWorth;
    input.VictimWorth = candidates[victim].Worth;
    input.MarginPercent = sPlayerbotAIConfig.WorldTravel.DestroyMarginPercent;
    if (!WorldTravel::ShouldDestroyFor(input))
        return false;

    uint32 const itemId = visitor.items[victim]->GetEntry();
    if (!DestroyOne(visitor.items[victim]))
        return false;

    EventLog::Emit(bot, "Inventory", "destroyed_for_space", itemId, input.VictimWorth);
    WorldTravel::AddLostWorth(measure, getMSTime(), input.VictimWorth);
    return true;
}

bool SmartDestroyItemAction::DestroyUntilBagSpace(std::vector<uint32> const& itemIds)
{
    // Ids, not Item*: one destroy frees every stack of an id, including later entries.
    for (uint32 const itemId : itemIds)
    {
        FindItemByIdVisitor visitor(itemId);
        DestroyItem(&visitor);

        if (AI_VALUE(uint8, "bag space") < BAG_SPACE_DESTROY_THRESHOLD)
            return true;
    }

    return false;
}
