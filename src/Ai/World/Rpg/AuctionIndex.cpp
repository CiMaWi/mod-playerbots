/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionIndex.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "AuctionConfig.h"
#include "AuctionHouseMgr.h"
#include "AuctionPricing.h"
#include "AuctionVisit.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerIndexes.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotsDatabase.h"
#include "QueryResult.h"
#include "ScriptMgr.h"
#include "ThreadCheck.h"
#include "Timer.h"

namespace AuctionIndex
{
namespace
{
Book book;                              // world thread only
Planner::SharedSlot<Snapshot> current;  // any thread
std::uint32_t publishTimer = 0;         // world thread only
std::uint32_t saveTimer = 0;            // world thread only
std::uint64_t sequence = 0;             // world thread only
std::uint32_t startupAuctions = 0;      // set once by the startup walk
std::uint32_t startupHouses = 0;
std::uint32_t walkMs = 0;
std::int64_t accruedAt = 0;  // world thread only: game time of the last listing hours accrual

std::int64_t GameNow() { return static_cast<std::int64_t>(GameTime::GetGameTime().count()); }

std::int64_t WindowSeconds()
{
    return static_cast<std::int64_t>(sPlayerbotAIConfig.Auction.HistoryDays) * SECONDS_PER_DAY;
}

// Copied inside the hook: the core deletes the entry right after OnAuctionRemove
AuctionFacts FactsOf(AuctionEntry const* entry)
{
    AuctionFacts facts;
    facts.id = entry->Id;
    facts.item = entry->item_template;
    facts.count = entry->itemCount;
    facts.startBid = entry->startbid;
    facts.bid = entry->bid;
    facts.buyout = entry->buyout;
    facts.endTime = static_cast<std::int64_t>(entry->expire_time);
    facts.bidder = entry->bidder.GetRawValue();
    facts.houseId = static_cast<std::uint8_t>(entry->houseId);
    return facts;
}
}  // namespace

std::vector<AuctionHouseObject*> Houses()
{
    std::vector<AuctionHouseObject*> houses;
    for (AuctionHouseId const id : {AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral})
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(id);
        if (house && std::find(houses.begin(), houses.end(), house) == houses.end())
            houses.push_back(house);
    }
    return houses;
}

namespace
{
// World thread: a bid that is not a buyout fires no hook, so the listed auctions' bid and bidder are read here; an
// auction the house no longer holds is dropped
void Publish()
{
    book.Refresh(
        [](OfferLine& line)
        {
            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(static_cast<AuctionHouseId>(line.houseId));
            AuctionEntry const* entry = house ? house->GetAuction(line.id) : nullptr;
            if (!entry)
                return false;
            line.bid = entry->bid;
            line.bidder = entry->bidder.GetRawValue();
            return true;
        });
    std::int64_t const now = GameNow();
    std::int64_t const window = WindowSeconds();
    // The seconds since the last accrual, not PublishSeconds: the fixture's PublishNow publishes between two timers
    book.AccrueListingHours(now, now - accruedAt, window);
    accruedAt = now;
    book.Prune(now, window);

    auto snapshot = std::make_shared<Snapshot>();
    book.Fill(*snapshot, now, window);
    snapshot->takenMs = getMSTime();
    snapshot->sequence = ++sequence;
    snapshot->startupAuctions = startupAuctions;
    snapshot->startupHouses = startupHouses;
    snapshot->walkMs = walkMs;
    current.Publish(std::move(snapshot));
}

// A table's rows read at startup. The pool returns null for a failed query and an empty result alike; the count always
// has one row when it ran, so a null count, or no rows when it counted some, means the table could not be read (WA2a)
struct StoredRows
{
    PreparedQueryResult result;
    std::uint64_t stored = 0;
    bool failed = false;
};

StoredRows ReadStored(PlayerbotsDatabaseStatements countStatement, PlayerbotsDatabaseStatements rowsStatement)
{
    StoredRows rows;
    PreparedQueryResult const count = PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(countStatement));
    rows.stored = count ? count->Fetch()[0].Get<uint64>() : 0;
    if (rows.stored)
        rows.result = PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(rowsStatement));
    rows.failed = !count || (rows.stored && !rows.result);
    return rows;
}

// Startup, world thread, before the first publication: the stored records and trade history into the book (WA1d,
// AH6), the records first so the history's prune keeps an item whose record is not forgotten. A table that could not
// be read leaves both unloaded: the save stays off and the stored rows of both stay as they are
void LoadHistory()
{
    std::uint32_t const start = getMSTime();
    StoredRows const history = ReadStored(PLAYERBOTS_SEL_AUCTION_HISTORY_COUNT, PLAYERBOTS_SEL_AUCTION_HISTORY);
    StoredRows const records = ReadStored(PLAYERBOTS_SEL_AUCTION_RECORD_COUNT, PLAYERBOTS_SEL_AUCTION_RECORD);
    if (history.failed || records.failed)
    {
        book.LoadFailed();
        LOG_ERROR("playerbots",
                  "Auction index: history load FAILED (history {} stored rows {}, records {} stored rows {}); saves "
                  "are off for this session, the stored rows stay as they are",
                  history.stored, history.failed ? "unread" : "read", records.stored,
                  records.failed ? "unread" : "read");
        return;
    }
    std::vector<RecordRow> recordRows;
    if (records.result)
    {
        recordRows.reserve(records.result->GetRowCount());
        do
        {
            Field* fields = records.result->Fetch();
            RecordRow row;
            row.scope = fields[0].Get<uint8>();
            row.id = fields[1].Get<uint32>();
            row.sales = fields[2].Get<double>();
            row.hours = fields[3].Get<double>();
            row.time = fields[4].Get<int64>();
            recordRows.push_back(row);
        } while (records.result->NextRow());
    }
    std::size_t const recordsTaken = book.LoadRecords(recordRows, GameNow());
    std::vector<HistoryRow> rows;
    if (history.result)
    {
        rows.reserve(history.result->GetRowCount());
        do
        {
            Field* fields = history.result->Fetch();
            HistoryRow row;
            row.item = fields[0].Get<uint32>();
            row.kind = fields[1].Get<uint8>();
            row.time = fields[2].Get<int64>();
            row.unitPrice = fields[3].Get<uint32>();
            row.count = fields[4].Get<uint32>();
            row.auctionId = fields[5].Get<uint32>();
            rows.push_back(row);
        } while (history.result->NextRow());
    }
    HistoryCounts const counts = book.LoadHistory(rows, GameNow(), WindowSeconds());
    LOG_INFO("server.loading",
             "Auction index: history loaded {} sales, {} expiries of {} items ({} rows read), {} records of {} rows "
             "read ({} ms)",
             counts.sales, counts.expiries, counts.items, rows.size(), recordsTaken, recordRows.size(),
             GetMSTimeDiffToNow(start));
}

// World thread: the whole history and the records (AH6) rewritten in one asynchronous transaction when either
// changed since the last save; the world thread only builds the statements
void SaveHistory(bool atShutdown)
{
    if (!book.SaveDue())
        return;
    std::vector<HistoryRow> const rows = book.ExportHistory();
    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    transaction->Append(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_AUCTION_HISTORY));
    for (HistoryRow const& row : rows)
    {
        PlayerbotsDatabasePreparedStatement* stmt =
            PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_AUCTION_HISTORY);
        stmt->SetData(0, row.item);
        stmt->SetData(1, row.kind);
        stmt->SetData(2, row.time);
        stmt->SetData(3, row.unitPrice);
        stmt->SetData(4, row.count);
        stmt->SetData(5, row.auctionId);
        transaction->Append(stmt);
    }
    std::vector<RecordRow> const records = book.ExportRecords();
    transaction->Append(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_AUCTION_RECORD));
    for (RecordRow const& row : records)
    {
        PlayerbotsDatabasePreparedStatement* stmt =
            PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_AUCTION_RECORD);
        stmt->SetData(0, row.scope);
        stmt->SetData(1, row.id);
        // The core binds no double (PreparedStatement.cpp); a float's seven digits are more than a chance needs
        stmt->SetData(2, static_cast<float>(row.sales));
        stmt->SetData(3, static_cast<float>(row.hours));
        stmt->SetData(4, row.time);
        transaction->Append(stmt);
    }
    PlayerbotsDatabase.CommitTransaction(transaction);
    book.MarkHistorySaved();
    if (atShutdown)
        LOG_INFO("playerbots", "Auction index: history saved at shutdown, {} rows, {} records", rows.size(),
                 records.size());
}

// After World::SetInitialWorldSettings loaded the auctions, which fires no OnAuctionAdd
void Walk()
{
    PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex walk");
    std::uint32_t const start = getMSTime();
    std::vector<AuctionHouseObject*> const houses = Houses();
    std::uint32_t found = 0;
    for (AuctionHouseObject* house : houses)
    {
        for (auto const& [id, entry] : house->GetAuctions())
        {
            if (!entry)
                continue;
            book.Add(FactsOf(entry), KindOfEntry(entry->item_template));
            ++found;
        }
    }
    startupAuctions = found;
    startupHouses = static_cast<std::uint32_t>(houses.size());
    walkMs = GetMSTimeDiffToNow(start);
    accruedAt = GameNow();  // the listing hours count from the walk on: the down time adds none
    LoadHistory();
    Publish();
    std::shared_ptr<Snapshot const> const snapshot = current.Current();
    LOG_INFO("server.loading", "Auction index: {} auctions of {} items in {} houses found, walked in {} ms", found,
             snapshot ? snapshot->items.size() : 0, startupHouses, walkMs);
}

class AuctionIndexScript : public AuctionHouseScript
{
public:
    AuctionIndexScript()
        : AuctionHouseScript("AuctionIndexScript",
                             {AUCTIONHOUSEHOOK_ON_AUCTION_ADD, AUCTIONHOUSEHOOK_ON_AUCTION_REMOVE,
                              AUCTIONHOUSEHOOK_ON_AUCTION_SUCCESSFUL, AUCTIONHOUSEHOOK_ON_AUCTION_EXPIRE,
                              AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_WON_MAIL,
                              AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_SEND_AUCTION_OUTBIDDED_MAIL})
    {
    }

    void OnAuctionAdd(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex add");
        if (!entry)
            return;
        book.Add(FactsOf(entry), KindOfEntry(entry->item_template));
        AuctionMemory::Listed(entry);
    }

    // After Successful or Expire, the same auction's end; alone, a cancel
    void OnAuctionRemove(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex remove");
        if (!entry)
            return;
        book.Remove(entry->Id);
        AuctionMemory::Removed(entry);
    }

    // A buyout (bid set to the buyout) or the leading bid at expiry
    void OnAuctionSuccessful(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex sale");
        if (!entry)
            return;
        book.Sale(FactsOf(entry), GameNow());
        AuctionMemory::Sold(entry);
    }

    void OnAuctionExpire(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex expire");
        if (!entry)
            return;
        book.Expire(FactsOf(entry), GameNow());
        AuctionMemory::Expired(entry);
    }

    // A buyout or the leading bid at the end: the buyer's row, online buyers only (WA4b)
    void OnBeforeAuctionHouseMgrSendAuctionWonMail(AuctionHouseMgr* /*mgr*/, AuctionEntry* auction, Player* bidder,
                                                   uint32& /*bidderAccId*/, bool& /*sendNotification*/,
                                                   bool& /*updateAchievementCriteria*/, bool& /*sendMail*/) override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex won mail");
        if (auction && bidder)
            AuctionMemory::Won(auction, bidder);
    }

    // A higher bid gives the old bidder's copper back by mail: its row, online bidders only (WA4b)
    void OnBeforeAuctionHouseMgrSendAuctionOutbiddedMail(AuctionHouseMgr* /*mgr*/, AuctionEntry* auction,
                                                         Player* oldBidder, uint32& /*oldBidderAccId*/,
                                                         Player* /*newBidder*/, uint32& newPrice,
                                                         bool& /*sendNotification*/, bool& /*sendMail*/) override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex outbid mail");
        if (auction && oldBidder)
            AuctionMemory::Outbid(auction, oldBidder, newPrice);
    }
};

class AuctionIndexWorldScript : public WorldScript
{
public:
    AuctionIndexWorldScript() : WorldScript("AuctionIndexWorldScript", {WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_SHUTDOWN}) {}

    void OnStartup() override { Walk(); }

    // After the world loop, before the databases close; the pool's close drains the asynchronous queue, so the
    // last transaction is written
    void OnShutdown() override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex shutdown");
        SaveHistory(true);
    }
};
}  // namespace

std::shared_ptr<Snapshot const> Current() { return current.Current(); }

void Update(std::uint32_t diff)
{
    publishTimer += diff;
    saveTimer += diff;
    if (publishTimer >= sPlayerbotAIConfig.Auction.PublishSeconds * IN_MILLISECONDS)
    {
        publishTimer = 0;
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex::Update");
        PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "auction index");
        Publish();
        if (pmo)
            pmo->finish();
    }
    if (saveTimer >= sPlayerbotAIConfig.Auction.SaveSeconds * IN_MILLISECONDS)
    {
        saveTimer = 0;
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex::Update save");
        PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "auction history save");
        SaveHistory(false);
        if (pmo)
            pmo->finish();
    }
}

void PublishNow()
{
    PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex::PublishNow");
    Publish();
}

KindKey KindOfEntry(std::uint32_t item)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    return proto ? KindOf(proto->Class, proto->Quality, proto->RequiredLevel, proto->ItemLevel) : KindKey{};
}

bool CreditListing(std::uint32_t item, double hours)
{
    PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex::CreditListing");
    KindKey const kind = KindOfEntry(item);
    if (!kind.Known() || !(hours > 0.0))
        return false;
    book.CreditListing(item, kind, hours, GameNow(), WindowSeconds());
    Publish();
    return true;
}

bool HandleIndexCommand(ChatHandler* handler, char const* args)
{
    std::string text = args ? args : "";
    char const* key = text.empty() ? nullptr : handler->extractKeyFromLink(text.data(), "Hitem");
    std::uint32_t const itemId = key ? static_cast<std::uint32_t>(std::strtoul(key, nullptr, 10)) : 0;
    ItemTemplate const* proto = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
    if (!proto)
    {
        handler->SendErrorMessage("AUCTION index: no item '{}'; usage: playerbots auction index <item id or link>",
                                  args ? args : "");
        return false;
    }

    std::shared_ptr<Snapshot const> const snapshot = Current();
    if (!snapshot)
    {
        handler->SendErrorMessage("AUCTION index: nothing published yet");
        return false;
    }

    handler->PSendSysMessage(
        "AUCTION index: seq {} age {} ms, {} auctions listed; since start {} sales, {} expiries, {} cancels, {} "
        "vanished; startup walk {} auctions in {} houses, {} ms",
        snapshot->sequence, getMSTimeDiff(snapshot->takenMs, getMSTime()), snapshot->auctions, snapshot->sales,
        snapshot->expiries, snapshot->cancels, snapshot->vanished, snapshot->startupAuctions, snapshot->startupHouses,
        snapshot->walkMs);

    static ItemEntry const EMPTY;
    ItemEntry const* found = snapshot->Find(itemId);
    ItemEntry const& entry = found ? *found : EMPTY;
    std::string const typical = entry.typical.sample ? std::to_string(entry.typical.unitPrice) : std::string("none");
    handler->PSendSysMessage(
        "AUCTION index {} {}: offers {} (units {}, cheapest unit buyout {}), typical {} (sample {}, weight {:.2f}), "
        "sales {}, expiries {}",
        itemId, proto->Name1, entry.offers.auctions, entry.offers.units, entry.offers.cheapestUnitBuyout, typical,
        entry.typical.sample, entry.typical.weight, entry.typical.sample, entry.typical.expiries);
    // The records (AH.4) through the one sale chance rule; the kind from the template, so an unseen item shows its
    // kind's
    KindKey const kind = KindOfEntry(itemId);
    Evidence const evidence = snapshot->EvidenceOf(itemId, kind);
    AuctionConfig const& config = sPlayerbotAIConfig.Auction;
    handler->PSendSysMessage(
        "AUCTION record {}: item sales {:.3f} hours {:.3f}; kind {:#x} sales {:.3f} hours {:.3f}; chance at 12 h "
        "{:.3f}, 24 h {:.3f}, 48 h {:.3f}",
        itemId, evidence.item.sales, evidence.item.hours, kind.code, evidence.kind.sales, evidence.kind.hours,
        AuctionRules::SaleChance(evidence.item, evidence.kind, 12.0, config),
        AuctionRules::SaleChance(evidence.item, evidence.kind, 24.0, config),
        AuctionRules::SaleChance(evidence.item, evidence.kind, 48.0, config));
    for (OfferLine const& line : entry.offers.cheapest)
        handler->PSendSysMessage("AUCTION offer {}: x{} bid {} (start {}) buyout {} ends in {} s, bidder {}, house {}",
                                 line.id, line.count, line.bid, line.startBid, line.buyout,
                                 line.endTime - snapshot->gameTime,
                                 line.bidder ? ObjectGuid(line.bidder).ToString() : std::string("none"),
                                 static_cast<std::uint32_t>(line.houseId));
    return true;
}
}  // namespace AuctionIndex

void AddSC_auction_index()
{
    new AuctionIndex::AuctionIndexScript();
    new AuctionIndex::AuctionIndexWorldScript();
}
