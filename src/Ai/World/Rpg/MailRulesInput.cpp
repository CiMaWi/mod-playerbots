/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MailRulesInput.h"

#include <algorithm>

#include "AiObjectContext.h"
#include "GameTime.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "Mail.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"

namespace BankMail
{
namespace
{
bool IsPlainBag(Bag const* bag)
{
    return bag && bag->GetTemplate()->Class == ITEM_CLASS_CONTAINER &&
           bag->GetTemplate()->SubClass == ITEM_SUBCLASS_CONTAINER;
}

void AddStackRoom(BagSpace& space, Item const* item)
{
    if (!item)
    {
        ++space.freeSlots;
        return;
    }

    uint32 const stackMax = item->GetTemplate()->GetMaxStackSize();
    if (stackMax <= item->GetCount())
        return;

    uint32 const entry = item->GetEntry();
    auto stack = std::find_if(space.stackRoom.begin(), space.stackRoom.end(),
                              [entry](StackRoom const& room) { return room.entry == entry; });
    if (stack == space.stackRoom.end())
        space.stackRoom.push_back({entry, stackMax - item->GetCount()});
    else
        stack->room += stackMax - item->GetCount();
}

MailItemIn ReadMailItem(PlayerbotAI* botAI, ObjectGuid::LowType itemGuid, Item const* item, float sellMultiplier)
{
    ItemTemplate const* proto = item->GetTemplate();
    MailItemIn in;
    in.itemGuid = itemGuid;
    in.entry = proto->ItemId;
    in.count = std::max<uint32>(item->GetCount(), 1);
    in.stackMax = std::max<uint32>(proto->GetMaxStackSize(), 1);
    ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", proto->ItemId)->Get();
    in.worth = ItemUsageValue::ItemWorth(proto, usage, in.count, sellMultiplier);
    if (in.worth == WorldTravel::WORTH_PRICELESS)
    {
        // Upgrades, quest and kept items go first; their worth stays finite so the seen worth stays a real number
        in.priority = true;
        in.worth =
            WorldTravel::ItemWorth(WorldTravel::SpaceClass::Use, proto->SellPrice,
                                   static_cast<uint32>(std::max<int32>(proto->BuyPrice, 0)), in.count, sellMultiplier);
    }
    return in;
}
}  // namespace

// Mail list read: the bot's own map thread or the world thread only (see BankMailState.h)
std::vector<MailIn> BuildMailIn(PlayerbotAI* botAI)
{
    std::vector<MailIn> mails;
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return mails;

    time_t const now = GameTime::GetGameTime().count();
    float const sellMultiplier = static_cast<float>(sRandomPlayerbotMgr.GetSellMultiplier(bot));
    for (Mail const* mail : bot->GetMails())
    {
        if (mail->state == MAIL_STATE_DELETED || now > mail->expire_time)
            continue;

        MailIn in;
        in.id = mail->messageID;
        in.delivered = now >= mail->deliver_time;
        in.cod = mail->COD > 0;
        in.unread = !(mail->checked & MAIL_CHECK_MASK_READ);
        in.hasText = (mail->checked & MAIL_CHECK_MASK_HAS_BODY) != 0;
        in.money = mail->money;
        for (MailItemInfo const& info : mail->items)
            if (Item const* item = bot->GetMItem(info.item_guid))
                in.items.push_back(ReadMailItem(botAI, info.item_guid, item, sellMultiplier));
        mails.push_back(std::move(in));
    }
    return mails;
}

BagSpace BuildBagSpace(Player* bot)
{
    BagSpace space;
    if (!bot)
        return space;

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        AddStackRoom(space, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = bot->GetBagByPos(bagSlot);
        if (!IsPlainBag(bag))
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            AddStackRoom(space, bag->GetItemByPos(static_cast<uint8>(slot)));
    }
    return space;
}

MailOptions BuildMailOptions()
{
    MailOptions options;
    options.mailsPerVisit = sPlayerbotAIConfig.BankMail.MailsPerVisit;
    return options;
}
}  // namespace BankMail
