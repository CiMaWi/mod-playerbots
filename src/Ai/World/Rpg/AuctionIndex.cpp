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
#include "Chat.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerIndexes.h"
#include "PlayerbotAIConfig.h"
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
std::uint64_t sequence = 0;             // world thread only
std::uint32_t startupAuctions = 0;      // set once by the startup walk
std::uint32_t startupHouses = 0;
std::uint32_t walkMs = 0;

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

// The distinct house objects: with AllowTwoSide.Interaction.Auction every id gives the neutral one
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
            book.Add(FactsOf(entry));
            ++found;
        }
    }
    startupAuctions = found;
    startupHouses = static_cast<std::uint32_t>(houses.size());
    walkMs = GetMSTimeDiffToNow(start);
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
                              AUCTIONHOUSEHOOK_ON_AUCTION_SUCCESSFUL, AUCTIONHOUSEHOOK_ON_AUCTION_EXPIRE})
    {
    }

    void OnAuctionAdd(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex add");
        if (entry)
            book.Add(FactsOf(entry));
    }

    // After Successful or Expire, the same auction's end; alone, a cancel
    void OnAuctionRemove(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex remove");
        if (entry)
            book.Remove(entry->Id);
    }

    // A buyout (bid set to the buyout) or the leading bid at expiry
    void OnAuctionSuccessful(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex sale");
        if (entry)
            book.Sale(FactsOf(entry), GameNow());
    }

    void OnAuctionExpire(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex expire");
        if (entry)
            book.Expire(FactsOf(entry), GameNow());
    }
};

class AuctionIndexWorldScript : public WorldScript
{
public:
    AuctionIndexWorldScript() : WorldScript("AuctionIndexWorldScript", {WORLDHOOK_ON_STARTUP}) {}

    void OnStartup() override { Walk(); }
};
}  // namespace

std::shared_ptr<Snapshot const> Current() { return current.Current(); }

void Update(std::uint32_t diff)
{
    publishTimer += diff;
    if (publishTimer < sPlayerbotAIConfig.Auction.PublishSeconds * IN_MILLISECONDS)
        return;
    publishTimer = 0;
    PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex::Update");
    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "auction index");
    Publish();
    if (pmo)
        pmo->finish();
}

void PublishNow()
{
    PLAYERBOT_CHECK_WORLD_THREAD("AuctionIndex::PublishNow");
    Publish();
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
