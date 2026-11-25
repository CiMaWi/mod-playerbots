/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONINDEXRULES_H
#define _PLAYERBOT_AUCTIONINDEXRULES_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// The auction house price index (PLAN 3.3, WA.4, WA.10): a book fed by the core's auction hooks on the world thread
// and the snapshot it publishes. Std only, so the selftest includes it as it is; AuctionIndex.cpp copies the core's
// AuctionEntry fields into AuctionFacts and keeps no core pointer.
namespace AuctionIndex
{
constexpr std::size_t OFFER_LINES_MAX = 5;  // the cheapest auctions per item a snapshot lists
constexpr std::size_t SALES_MAX = 48;       // sales kept per item, the newest
constexpr std::size_t EXPIRIES_MAX = 48;    // expiries kept per item, the newest
constexpr std::int64_t SECONDS_PER_DAY = 86400;

// Copper per unit of a stack, rounded half up; a count of 0 is read as 1
inline std::uint32_t UnitPrice(std::uint32_t copper, std::uint32_t count)
{
    if (count <= 1)
        return copper;
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(copper) + count / 2) / count);
}

// One auction as a hook or the publish's refresh read it; times are game time in seconds
struct AuctionFacts
{
    std::uint32_t id = 0;
    std::uint32_t item = 0;
    std::uint32_t count = 0;
    std::uint32_t startBid = 0;
    std::uint32_t bid = 0;     // the current bid, 0 = none
    std::uint32_t buyout = 0;  // 0 = no buyout
    std::int64_t endTime = 0;
    std::uint64_t bidder = 0;  // raw guid, 0 = none
    std::uint8_t houseId = 0;  // the core's AuctionHouseId as stored
};
using OfferLine = AuctionFacts;

// The trade history, plain data for WA1d's load and save; each list oldest first
struct SaleRecord
{
    std::int64_t time = 0;
    std::uint32_t unitPrice = 0;
    std::uint32_t count = 0;
    std::uint32_t auctionId = 0;
};

struct ExpiryRecord
{
    std::int64_t time = 0;
    std::uint32_t count = 0;
    std::uint32_t auctionId = 0;
};

struct ItemHistory
{
    std::vector<SaleRecord> sales;
    std::vector<ExpiryRecord> expiries;
};

// View 1 (WA.4), read anywhere: an item's typical unit price, the recency-weighted median of its sales in the window
struct TypicalPrice
{
    std::uint32_t unitPrice = 0;  // 0 = no sale in the window
    std::uint32_t sample = 0;     // sales in the window
    double weight = 0.0;          // their summed recency weights (1 = one sale of now)
    std::uint32_t expiries = 0;   // expiries in the window
};

// View 2 (WA.4), for the auction visit only: what the house lists now
struct CurrentOffers
{
    std::uint32_t auctions = 0;
    std::uint32_t units = 0;
    std::uint32_t cheapestUnitBuyout = 0;  // 0 = no auction with a buyout
    std::vector<OfferLine> cheapest;       // up to OFFER_LINES_MAX, cheapest unit buyout first, no buyout last
};

struct ItemEntry
{
    TypicalPrice typical;
    CurrentOffers offers;
};

// One publication: immutable once published, read through AuctionIndex::Current() on any thread
struct Snapshot
{
    std::unordered_map<std::uint32_t, ItemEntry> items;
    std::int64_t gameTime = 0;   // game time of the publication, s
    std::uint32_t takenMs = 0;   // getMSTime() of the publication
    std::uint64_t sequence = 0;  // 1 = the startup walk's
    std::uint32_t auctions = 0;  // offers in the book
    std::uint64_t sales = 0;     // events since start
    std::uint64_t expiries = 0;
    std::uint64_t cancels = 0;
    std::uint64_t vanished = 0;  // offers gone from the house without a hook, dropped by the publish's refresh
    std::uint32_t startupAuctions = 0;
    std::uint32_t startupHouses = 0;
    std::uint32_t walkMs = 0;

    ItemEntry const* Find(std::uint32_t item) const
    {
        auto const it = items.find(item);
        return it == items.end() ? nullptr : &it->second;
    }
};

// The world thread's book: offers keyed by auction id under their item, and each item's history. A Successful or an
// Expire records the outcome and drops the offer; the Remove that follows it is the same auction's end, a Remove
// with no outcome before it is a cancel.
class Book
{
public:
    void Add(AuctionFacts const& facts)
    {
        Drop(facts.id);
        _items[facts.item].offers[facts.id] = facts;
        _itemOfAuction[facts.id] = facts.item;
    }

    void Sale(AuctionFacts const& facts, std::int64_t now)
    {
        Drop(facts.id);
        SaleRecord record;
        record.time = now;
        record.unitPrice = UnitPrice(facts.bid ? facts.bid : facts.buyout, facts.count);
        record.count = facts.count;
        record.auctionId = facts.id;
        InsertCapped(_items[facts.item].history.sales, record, SALES_MAX);
        _outcomes.push_back(facts.id);
        ++_sales;
    }

    void Expire(AuctionFacts const& facts, std::int64_t now)
    {
        Drop(facts.id);
        ExpiryRecord record;
        record.time = now;
        record.count = facts.count;
        record.auctionId = facts.id;
        InsertCapped(_items[facts.item].history.expiries, record, EXPIRIES_MAX);
        _outcomes.push_back(facts.id);
        ++_expiries;
    }

    void Remove(std::uint32_t id)
    {
        auto const outcome = std::find(_outcomes.begin(), _outcomes.end(), id);
        if (outcome != _outcomes.end())
        {
            _outcomes.erase(outcome);
            Drop(id);
            return;
        }
        if (Drop(id))
            ++_cancels;
    }

    // At publish on the world thread: alive(line) copies the auction's current bid and bidder into the line, or
    // returns false when the house no longer holds it
    template <typename Alive>
    void Refresh(Alive&& alive)
    {
        for (auto& [item, book] : _items)
        {
            for (auto it = book.offers.begin(); it != book.offers.end();)
            {
                if (alive(it->second))
                {
                    ++it;
                    continue;
                }
                _itemOfAuction.erase(it->first);
                it = book.offers.erase(it);
                ++_vanished;
            }
        }
    }

    // History older than the window goes, then items with nothing left; an outcome whose Remove never came is
    // forgotten (the core removes right after the outcome hook, so none is pending between two publications)
    void Prune(std::int64_t now, std::int64_t windowSeconds)
    {
        std::int64_t const oldest = now - windowSeconds;
        for (auto it = _items.begin(); it != _items.end();)
        {
            ItemHistory& history = it->second.history;
            std::erase_if(history.sales, [oldest](SaleRecord const& sale) { return sale.time < oldest; });
            std::erase_if(history.expiries, [oldest](ExpiryRecord const& expiry) { return expiry.time < oldest; });
            if (it->second.offers.empty() && history.sales.empty() && history.expiries.empty())
                it = _items.erase(it);
            else
                ++it;
        }
        _outcomes.clear();
    }

    // Fills the item entries and counters; the caller sets the publication's times and the walk's figures
    void Fill(Snapshot& out, std::int64_t now, std::int64_t windowSeconds) const
    {
        out.gameTime = now;
        out.items.reserve(_items.size());
        out.auctions = static_cast<std::uint32_t>(_itemOfAuction.size());
        out.sales = _sales;
        out.expiries = _expiries;
        out.cancels = _cancels;
        out.vanished = _vanished;
        for (auto const& [item, book] : _items)
        {
            ItemEntry& entry = out.items[item];
            FillOffers(entry.offers, book.offers);
            FillTypical(entry.typical, book.history, now, windowSeconds);
        }
    }

    ItemHistory const* History(std::uint32_t item) const
    {
        auto const it = _items.find(item);
        return it == _items.end() ? nullptr : &it->second.history;
    }

private:
    struct ItemBook
    {
        std::map<std::uint32_t, OfferLine> offers;  // by auction id
        ItemHistory history;
    };

    // Ordered for a deterministic snapshot: cheapest unit buyout first, then by the unit of the next bid, then id
    static std::tuple<bool, std::uint32_t, std::uint32_t> LineKey(OfferLine const& line)
    {
        if (line.buyout)
            return {false, UnitPrice(line.buyout, line.count), line.id};
        return {true, UnitPrice(line.bid ? line.bid : line.startBid, line.count), line.id};
    }

    static void FillOffers(CurrentOffers& offers, std::map<std::uint32_t, OfferLine> const& lines)
    {
        offers.auctions = static_cast<std::uint32_t>(lines.size());
        std::vector<OfferLine const*> order;
        order.reserve(lines.size());
        for (auto const& [id, line] : lines)
        {
            offers.units += line.count;
            if (line.buyout)
            {
                std::uint32_t const unit = UnitPrice(line.buyout, line.count);
                if (!offers.cheapestUnitBuyout || unit < offers.cheapestUnitBuyout)
                    offers.cheapestUnitBuyout = unit;
            }
            order.push_back(&line);
        }
        std::size_t const keep = std::min(order.size(), OFFER_LINES_MAX);
        std::partial_sort(order.begin(), order.begin() + keep, order.end(),
                          [](OfferLine const* a, OfferLine const* b) { return LineKey(*a) < LineKey(*b); });
        offers.cheapest.reserve(keep);
        for (std::size_t i = 0; i < keep; ++i)
            offers.cheapest.push_back(*order[i]);
    }

    // Each sale weighs 2^(-age / half the window): a sale half the window old counts half a fresh one. The median is
    // the lowest price at which the sorted weights reach half the total; ties sort by time and auction id.
    static void FillTypical(TypicalPrice& typical, ItemHistory const& history, std::int64_t now,
                            std::int64_t windowSeconds)
    {
        std::int64_t const oldest = now - windowSeconds;
        double const halfLife = static_cast<double>(std::max<std::int64_t>(windowSeconds / 2, 1));
        std::vector<std::pair<SaleRecord const*, double>> weighted;
        weighted.reserve(history.sales.size());
        for (SaleRecord const& sale : history.sales)
        {
            if (sale.time < oldest)
                continue;
            double const age = static_cast<double>(std::max<std::int64_t>(now - sale.time, 0));
            double const weight = std::exp2(-age / halfLife);
            weighted.emplace_back(&sale, weight);
            typical.weight += weight;
        }
        for (ExpiryRecord const& expiry : history.expiries)
            if (expiry.time >= oldest)
                ++typical.expiries;
        typical.sample = static_cast<std::uint32_t>(weighted.size());
        if (weighted.empty())
            return;
        std::sort(weighted.begin(), weighted.end(),
                  [](auto const& a, auto const& b)
                  {
                      return std::tie(a.first->unitPrice, a.first->time, a.first->auctionId) <
                             std::tie(b.first->unitPrice, b.first->time, b.first->auctionId);
                  });
        double reached = 0.0;
        for (auto const& [sale, weight] : weighted)
        {
            reached += weight;
            if (reached * 2.0 >= typical.weight)
            {
                typical.unitPrice = sale->unitPrice;
                break;
            }
        }
        if (!typical.unitPrice)
            typical.unitPrice = weighted.back().first->unitPrice;  // rounding left the last sum short
    }

    // Kept sorted by (time, auction id) whatever order the events come in; the oldest go past the cap
    template <typename Record>
    static void InsertCapped(std::vector<Record>& records, Record const& record, std::size_t cap)
    {
        auto const at = std::upper_bound(records.begin(), records.end(), record, [](Record const& a, Record const& b)
                                         { return std::tie(a.time, a.auctionId) < std::tie(b.time, b.auctionId); });
        records.insert(at, record);
        if (records.size() > cap)
            records.erase(records.begin(), records.begin() + (records.size() - cap));
    }

    // Drops an auction's offer; true when it was listed
    bool Drop(std::uint32_t id)
    {
        auto const it = _itemOfAuction.find(id);
        if (it == _itemOfAuction.end())
            return false;
        auto const book = _items.find(it->second);
        if (book != _items.end())
            book->second.offers.erase(id);
        _itemOfAuction.erase(it);
        return true;
    }

    std::map<std::uint32_t, ItemBook> _items;
    std::unordered_map<std::uint32_t, std::uint32_t> _itemOfAuction;  // listed auctions only
    std::vector<std::uint32_t> _outcomes;                             // Successful or Expire seen, its Remove not yet
    std::uint64_t _sales = 0;
    std::uint64_t _expiries = 0;
    std::uint64_t _cancels = 0;
    std::uint64_t _vanished = 0;
};

// The snapshot as text, items in id order: the selftest's comparison
inline std::string Dump(Snapshot const& snapshot)
{
    std::map<std::uint32_t, ItemEntry const*> sorted;
    for (auto const& [item, entry] : snapshot.items)
        sorted[item] = &entry;
    std::ostringstream out;
    out << std::fixed << std::setprecision(9) << "auctions " << snapshot.auctions << " sales " << snapshot.sales
        << " expiries " << snapshot.expiries << " cancels " << snapshot.cancels << " vanished " << snapshot.vanished
        << "\n";
    for (auto const& [item, entry] : sorted)
    {
        out << item << ": offers " << entry->offers.auctions << " units " << entry->offers.units << " cheapest "
            << entry->offers.cheapestUnitBuyout << " typical " << entry->typical.unitPrice << " sample "
            << entry->typical.sample << " weight " << entry->typical.weight << " expiries " << entry->typical.expiries
            << "\n";
        for (OfferLine const& line : entry->offers.cheapest)
            out << "  " << line.id << " x" << line.count << " bid " << line.bid << "/" << line.startBid << " buyout "
                << line.buyout << " end " << line.endTime << " bidder " << line.bidder << " house "
                << static_cast<std::uint32_t>(line.houseId) << "\n";
    }
    return out.str();
}
}  // namespace AuctionIndex

#endif
