/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankMailState.h"

#include <algorithm>
#include <limits>

#include "GameTime.h"
#include "Mail.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ThreadCheck.h"
#include "Timer.h"

namespace BankMail
{
namespace
{
// The 28 bank item slots only: bank bags and bought bag slots are not this plan's (WB.3)
uint16 CountFreeBankSlots(Player const* bot)
{
    uint16 free = 0;
    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++free;
    return free;
}
}  // namespace

// Mail list read: the bot's own map thread or the world thread only (see BankMailState.h)
uint16 CountWaitingMail(Player const* bot)
{
    // Not Player::unReadMails: a uint8 that saturates and counts mail already deleted in this session
    time_t const now = GameTime::GetGameTime().count();
    uint32 waiting = 0;
    for (Mail const* mail : bot->GetMails())
    {
        if (mail->state == MAIL_STATE_DELETED || (mail->checked & MAIL_CHECK_MASK_READ))
            continue;
        if (now < mail->deliver_time || now > mail->expire_time)
            continue;
        ++waiting;
    }
    return static_cast<uint16>(std::min<uint32>(waiting, std::numeric_limits<uint16>::max()));
}

void RefreshMailKnowledge(Player const* bot, MailKnowledge& mail)
{
    mail.waiting = CountWaitingMail(bot);
    mail.refreshedMs = std::max<uint32>(getMSTime(), 1);
}

void BuildBankKnowledge(PlayerbotAI* botAI)
{
    PLAYERBOT_CHECK_WORLD_THREAD("BuildBankKnowledge");
    Player* bot = botAI->GetBot();
    if (!bot)
        return;

    BotBankMail& known = botAI->rpgInfo.bankMail;
    // The kept list stays empty until a keep rule decides which bank items are kept for a later level
    known.bank.kept.clear();
    known.bank.freeSlots = CountFreeBankSlots(bot);
    SumDueItems(known.bank, bot->GetLevel());
    known.bank.built = true;
    RefreshMailKnowledge(bot, known.mail);
}
}  // namespace BankMail
