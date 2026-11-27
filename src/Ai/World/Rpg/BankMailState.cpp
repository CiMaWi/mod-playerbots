/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankMailState.h"

#include <algorithm>
#include <limits>

#include "AuctionIndex.h"
#include "BankMailService.h"
#include "BankRulesInput.h"
#include "GameTime.h"
#include "Item.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "Timer.h"
#include "Visit.h"

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

namespace
{
bool Listed(std::vector<uint32> const& ids, uint32 id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

// A mail with no money whose every item the core would still refuse the bot (R6)
bool HoldsOnlyHeldCopies(Player* bot, Mail const& mail)
{
    if (mail.money || mail.items.empty())
        return false;
    return std::all_of(mail.items.begin(), mail.items.end(),
                       [bot](MailItemInfo const& info)
                       {
                           Item* item = bot->GetMItem(info.item_guid);
                           return item && bot->CanTakeMoreSimilarItems(item) != EQUIP_ERR_OK;
                       });
}
}  // namespace

void ReadMailUnits(Player* bot, MailKnowledge& known)
{
    time_t const now = GameTime::GetGameTime().count();
    known.units.clear();
    for (Mail const* mail : bot->GetMails())
    {
        if (mail->state == MAIL_STATE_DELETED || mail->COD || now > mail->expire_time)
            continue;
        for (MailItemInfo const& info : mail->items)
            if (Item const* item = bot->GetMItem(info.item_guid))
                AddUnits(known.units, info.item_template, item->GetCount());
    }
}

// The copper of delivered auction house mail the bot has not taken (auction WA3b, WA4b): what the house's notices of
// its sales and of bids given back told it, and the items a won auction or an unsold listing sent back, each counted
// once at its typical price, else its vendor price (a stack is counted as one unit: the mail lists its items without
// their counts). The trip planner sends the bot to a mailbox for them. Mail a visit saw and left is in the seen
// worth and left out, as is a mail left for unique copies the bot still holds (needs NR.7)
uint32 AuctionCopperWaiting(Player* bot, MailKnowledge const& known)
{
    time_t const now = GameTime::GetGameTime().count();
    std::shared_ptr<AuctionIndex::Snapshot const> const index = AuctionIndex::Current();
    std::vector<WaitingAuctionMail> waiting;
    for (Mail const* mail : bot->GetMails())
    {
        if (mail->state == MAIL_STATE_DELETED || mail->messageType != MAIL_AUCTION || now < mail->deliver_time ||
            now > mail->expire_time)
            continue;
        if (Listed(known.uniqueMails, mail->messageID) && HoldsOnlyHeldCopies(bot, *mail))
            continue;
        uint64 copper = mail->money;
        for (MailItemInfo const& info : mail->items)
        {
            AuctionIndex::ItemEntry const* entry = index ? index->Find(info.item_template) : nullptr;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(info.item_template);
            copper += entry && entry->typical.unitPrice ? entry->typical.unitPrice : proto ? proto->SellPrice : 0;
        }
        waiting.push_back({mail->messageID, copper});
    }
    return UnseenCopper(waiting, known.seenMails);
}

void RefreshMailKnowledge(Player* bot, MailKnowledge& mail)
{
    mail.waiting = CountWaitingMail(bot);
    ReadMailUnits(bot, mail);
    mail.expectedCopper = AuctionCopperWaiting(bot, mail);
    mail.refreshedMs = std::max<uint32>(getMSTime(), 1);
}

void BuildBankKnowledge(PlayerbotAI* botAI)
{
    PLAYERBOT_CHECK_WORLD_THREAD("BuildBankKnowledge");
    Player* bot = botAI->GetBot();
    if (!bot)
        return;

    BotBankMail& known = botAI->bankMail;
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
    // The craft visit's own ends; its no spot is no station of the focus on the map, its no station the one found gone
    std::string const own =
        kind != VisitKind::Craft
            ? std::string()
            : Acore::StringFormat(", no materials {}, no station {}, bags full {}", count(VisitEnd::NoMaterials),
                                  count(VisitEnd::NoStation), count(VisitEnd::BagsFull));
    char const* const noSpot = kind == VisitKind::Mail      ? "no mailbox"
                               : kind == VisitKind::Bank    ? "no banker"
                               : kind == VisitKind::Auction ? "no auctioneer"
                                                            : "no spot";
    return Acore::StringFormat("started {}, done {}, timed out {}, {} {}, unreached {}{}, interrupted {} ({})", started,
                               count(VisitEnd::Done), count(VisitEnd::Timeout), noSpot, count(VisitEnd::NoSpot),
                               count(VisitEnd::Unreached), own, interrupted, each);
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

    VisitState const& visit = botAI->bankMail.visit;
    if (visit.phase == VisitPhase::None)
        return;
    VisitWatchIn in = WatchInOf(bot, visit.mapId, visit.startedMs, visit.limitMs);
    // An auction visit came into reach once it acted: only its act writes `tried` and `buying`
    AuctionVisitData const& auction = botAI->bankMail.auctionData;
    in.reached = visit.kind != VisitKind::Auction || !auction.tried.empty() || auction.buying;
    if (std::optional<VisitEnd> const end = WatchVisit(in))
        EndVisit(botAI, *end);
}

void EndVisits(PlayerbotAI* botAI, VisitEnd end) { EndVisit(botAI, end); }
}  // namespace BankMail
