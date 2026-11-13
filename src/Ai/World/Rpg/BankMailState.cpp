/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankMailState.h"

#include <algorithm>
#include <limits>

#include "BankRulesInput.h"
#include "BankVisit.h"
#include "GameTime.h"
#include "Mail.h"
#include "MailVisit.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "StringFormat.h"
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

std::string DescribeVisitCounters(VisitKind kind, uint32 started, VisitEndCounters const& ended)
{
    auto const count = [&ended](VisitEnd end) { return ended[static_cast<size_t>(end)].load(); };
    uint32 interrupted = 0;
    std::string each;
    for (size_t i = 0; i < VISIT_END_COUNT; ++i)
    {
        VisitEnd const end = static_cast<VisitEnd>(i);
        if (!IsInterruption(end))
            continue;
        interrupted += count(end);
        each += Acore::StringFormat("{}{} {}", each.empty() ? "" : ", ", VisitEndWord(kind, end), count(end));
    }
    return Acore::StringFormat("started {}, done {}, timed out {}, {} {}, interrupted {} ({})", started,
                               count(VisitEnd::Done), count(VisitEnd::Timeout),
                               kind == VisitKind::Mail ? "no mailbox" : "no banker", count(VisitEnd::NoSpot),
                               interrupted, each);
}

VisitWatchIn WatchInOf(Player const* bot, uint32 mapId, uint32 startedMs, uint32 limitMs)
{
    return VisitWatchIn{bot->IsAlive(), bot->IsInCombat(), bot->GetMapId() == mapId,
                        getMSTimeDiff(startedMs, getMSTime()), limitMs};
}

void WatchVisits(PlayerbotAI* botAI)
{
    Player const* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot)
        return;

    BotBankMail const& state = botAI->rpgInfo.bankMail;
    if (state.mailVisit.phase != MailVisitPhase::None)
    {
        MailVisitState const& visit = state.mailVisit;
        if (std::optional<VisitEnd> const end = WatchVisit(WatchInOf(bot, visit.mapId, visit.startedMs, visit.limitMs)))
            EndMailVisit(botAI, *end);
    }
    if (state.bankVisit.phase != BankVisitPhase::None)
    {
        BankVisitState const& visit = state.bankVisit;
        if (std::optional<VisitEnd> const end = WatchVisit(WatchInOf(bot, visit.mapId, visit.startedMs, visit.limitMs)))
            EndBankVisit(botAI, *end);
    }
}

void EndVisits(PlayerbotAI* botAI, VisitEnd end)
{
    EndMailVisit(botAI, end);
    EndBankVisit(botAI, end);
}
}  // namespace BankMail
