/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankMailState.h"

#include <algorithm>
#include <limits>

#include "BankRulesInput.h"
#include "GameTime.h"
#include "Mail.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ThreadCheck.h"
#include "Timer.h"

namespace BankMail
{
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
    RefreshBankKnowledge(botAI);
    known.bank.built = true;
    RefreshMailKnowledge(bot, known.mail);
}
}  // namespace BankMail
