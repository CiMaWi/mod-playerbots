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
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
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
    std::int64_t added = 0;    // game time the book took it (a hook, the startup walk); 0 = long ago
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

// The standing price (AH8, PLAN 3.4): the lowest unit buyout among an item's offers that had stood the standing time,
// as of the last publication that saw one; it leaves with the window as a sale does
struct StandingPrice
{
    std::uint32_t unitPrice = 0;  // 0 = none
    std::int64_t time = 0;
};

struct ItemHistory
{
    std::vector<SaleRecord> sales;
    std::vector<ExpiryRecord> expiries;
    StandingPrice standing;
};

// One stored record of an item's history (WA1d), a row of playerbots_auction_history: the records themselves, not a
// summary, so a load puts the book where it was and the median keeps re-weighting as time passes. A standing price
// (AH8) is one row per item: Count 1, AuctionId 0
constexpr std::uint8_t HISTORY_SALE = 0;
constexpr std::uint8_t HISTORY_EXPIRY = 1;
constexpr std::uint8_t HISTORY_STANDING = 2;

struct HistoryRow
{
    std::uint32_t item = 0;
    std::uint8_t kind = HISTORY_SALE;
    std::int64_t time = 0;        // game time, s
    std::uint32_t unitPrice = 0;  // 0 for an expiry
    std::uint32_t count = 0;
    std::uint32_t auctionId = 0;
};

// What the book's history holds: the load's log line
struct HistoryCounts
{
    std::uint64_t sales = 0;
    std::uint64_t expiries = 0;
    std::uint32_t standing = 0;
    std::uint32_t items = 0;
};

// Where a typical price comes from (AH.2): recent sales, else the standing price, else there is none
enum class PriceBasis : std::uint8_t
{
    None,
    Sales,
    Offers
};

inline std::string_view BasisWord(PriceBasis basis)
{
    switch (basis)
    {
        case PriceBasis::Sales:
            return "sales";
        case PriceBasis::Offers:
            return "offers";
        default:
            return "none";
    }
}

// View 1 (WA.4, AH.2), read anywhere: an item's typical unit price, the recency-weighted median of its sales in the
// window, else its standing price in the window
struct TypicalPrice
{
    std::uint32_t unitPrice = 0;  // 0 = neither a sale nor a standing price in the window
    std::uint32_t sample = 0;     // sales in the window
    double weight = 0.0;          // their summed recency weights (1 = one sale of now)
    std::uint32_t expiries = 0;   // expiries in the window
    PriceBasis basis = PriceBasis::None;
};

// What the house saw of an item or a kind selling (PLAN 3.3, AH.4): lots sold and lot hours listed, each weighed by
// recency as the sales' prices are (half the window as half-life); a snapshot's are weighed to its publication.
// AuctionRules::SaleChance reads them
struct Record
{
    double sales = 0.0;  // lots
    double hours = 0.0;  // lot hours listed

    [[nodiscard]] bool Empty() const { return !(sales > 0.0) && !(hours > 0.0); }
};

// One stored record (AH6), a row of playerbots_auction_record: an item's or a kind's record as weighed to its time,
// so a load puts the book's records where they were and they go on decaying from there
constexpr std::uint8_t RECORD_ITEM = 0;
constexpr std::uint8_t RECORD_KIND = 1;

struct RecordRow
{
    std::uint8_t scope = RECORD_ITEM;
    std::uint32_t id = 0;  // the item entry, or the kind's KindKey::code
    double sales = 0.0;
    double hours = 0.0;
    std::int64_t time = 0;  // game time the values are weighed to, s
};

constexpr std::uint32_t KIND_BAND_LEVELS = 10;         // [E] levels per band
constexpr std::uint32_t KIND_KNOWN = 1u << 24;         // set in every resolved code
constexpr std::uint32_t KIND_BY_ITEM_LEVEL = 1u << 7;  // the band is the item level's
constexpr std::uint32_t KIND_BAND_MAX = KIND_BY_ITEM_LEVEL - 1;
constexpr double RECORD_FORGET = 0.01;  // a record below this many lots and lot hours is forgotten

// An item's kind (AH.4): class, quality and level band in one code. 0: not resolved, so it feeds no record (the
// server resolves every offer's kind from its template at the add; a plain Add of the harnesses has none)
struct KindKey
{
    std::uint32_t code = 0;

    [[nodiscard]] bool Known() const { return code != 0; }
    bool operator==(KindKey const&) const = default;
};

// [E] The band is the required level's ten levels. An item without a required level (trade goods, most consumables,
// starting gear) is banded by its item level instead, in bands of their own: an item level is no character level
inline KindKey KindOf(std::uint32_t itemClass, std::uint32_t quality, std::uint32_t requiredLevel,
                      std::uint32_t itemLevel)
{
    bool const byItemLevel = !requiredLevel;
    std::uint32_t const band = std::min((byItemLevel ? itemLevel : requiredLevel) / KIND_BAND_LEVELS, KIND_BAND_MAX);
    return {KIND_KNOWN | (std::min<std::uint32_t>(itemClass, 0xFF) << 16) |
            (std::min<std::uint32_t>(quality, 0xFF) << 8) | (byItemLevel ? KIND_BY_ITEM_LEVEL : 0) | band};
}

// View 2 (WA.4), for the auction visit only: what the house lists now
struct CurrentOffers
{
    std::uint32_t auctions = 0;
    std::uint32_t units = 0;
    std::uint32_t cheapestUnitBuyout = 0;  // 0 = no auction with a buyout
    std::vector<OfferLine> cheapest;       // by LineKey: the OFFER_LINES_MAX cheapest, then the lots LotFrontier adds
};

struct ItemEntry
{
    TypicalPrice typical;
    CurrentOffers offers;
    Record record;  // the item's own
    KindKey kind;   // its kind as the book resolved it at an add or a credit; none for an item only loaded
};

// An item's evidence for an ask (PLAN 3.3, 3.10): its typical price, the cheapest standing buyout (the auction visit's
// only, R1), its record and its kind's
struct Evidence
{
    TypicalPrice typical;
    std::uint32_t cheapestUnitBuyout = 0;
    Record item;
    Record kind;
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
    // The kinds' records, by KindKey::code
    std::unordered_map<std::uint32_t, Record> kinds;

    ItemEntry const* Find(std::uint32_t item) const
    {
        auto const it = items.find(item);
        return it == items.end() ? nullptr : &it->second;
    }

    [[nodiscard]] Record KindRecord(KindKey kind) const
    {
        auto const it = kinds.find(kind.code);
        return it == kinds.end() ? Record{} : it->second;
    }

    // For a caller holding the item's template values: kind = KindOf(class, quality, required level, item level), so
    // an item the house never saw still has its kind's record. Two lookups
    [[nodiscard]] Evidence EvidenceOf(std::uint32_t item, KindKey kind) const
    {
        Evidence evidence;
        if (ItemEntry const* entry = Find(item))
        {
            evidence.typical = entry->typical;
            evidence.cheapestUnitBuyout = entry->offers.cheapestUnitBuyout;
            evidence.item = entry->record;
        }
        evidence.kind = KindRecord(kind);
        return evidence;
    }
};

// The world thread's book: offers keyed by auction id under their item, and each item's history. A Successful or an
// Expire records the outcome and drops the offer; the Remove that follows it is the same auction's end, a Remove
// with no outcome before it is a cancel. The records (PLAN 3.3) are fed only for items added with a known kind: a sale
// at its hook, listing hours at each publish (AccrueListingHours)
class Book
{
public:
    void Add(AuctionFacts const& facts, KindKey kind = {})
    {
        Drop(facts.id);
        ItemBook& book = _items[facts.item];
        book.offers[facts.id] = facts;
        if (kind.Known())
            book.kind = kind;
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
        ItemBook& book = _items[facts.item];
        InsertCapped(book.history.sales, record, SALES_MAX);
        if (book.kind.Known())
        {
            Credit(book.record, now, 1.0, 0.0, _recordHalfLife);
            Credit(_kinds[book.kind.code], now, 1.0, 0.0, _recordHalfLife);
            _recordsDirty = true;
        }
        _outcomes.push_back(facts.id);
        ++_sales;
        _historyDirty = true;
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
        _historyDirty = true;
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

    // At a publish: every lot standing adds the interval's hours to its item's record and its kind's, for items of a
    // known kind; a lot sold or expired since the last publish adds none. The server's publish passes the seconds since
    // its last accrual (AH6), so a publish between two timers (the fixture's) counts no second twice
    void AccrueListingHours(std::int64_t now, std::int64_t seconds, std::int64_t windowSeconds)
    {
        constexpr double secondsPerHour = 3600.0;
        _recordHalfLife = HalfLife(windowSeconds);
        if (seconds <= 0)
            return;
        for (auto& [item, book] : _items)
        {
            if (!book.kind.Known() || book.offers.empty())
                continue;
            double const lotHours = static_cast<double>(seconds) / secondsPerHour * book.offers.size();
            Credit(book.record, now, 0.0, lotHours, _recordHalfLife);
            Credit(_kinds[book.kind.code], now, 0.0, lotHours, _recordHalfLife);
            _recordsDirty = true;
        }
    }

    // The test seam (AuctionFixture `listed`, AH6): hours of listing credited to an item's record and its kind's as if
    // its lots had stood that long unsold; the item takes the kind even if it was never listed
    void CreditListing(std::uint32_t item, KindKey kind, double hours, std::int64_t now, std::int64_t windowSeconds)
    {
        if (!kind.Known() || !(hours > 0.0))
            return;
        _recordHalfLife = HalfLife(windowSeconds);
        ItemBook& book = _items[item];
        book.kind = kind;
        Credit(book.record, now, 0.0, hours, _recordHalfLife);
        Credit(_kinds[kind.code], now, 0.0, hours, _recordHalfLife);
        _recordsDirty = true;
    }

    // At a publish, after the refresh (AH8, PLAN 3.4): each item's standing price is the lowest unit buyout among its
    // offers that have stood at least standingSeconds, a price a visitor would have seen and not a lot that flashed by;
    // it takes this publication's time. With none standing the last one stays with its time
    void TakeStanding(std::int64_t now, std::int64_t standingSeconds)
    {
        for (auto& [item, book] : _items)
        {
            std::uint32_t lowest = 0;
            for (auto const& [id, line] : book.offers)
            {
                if (!line.buyout || now - line.added < standingSeconds)
                    continue;
                std::uint32_t const unit = UnitPrice(line.buyout, line.count);
                if (!lowest || unit < lowest)
                    lowest = unit;
            }
            StandingPrice& standing = book.history.standing;
            if (!lowest || (standing.unitPrice == lowest && standing.time == now))
                continue;
            standing = {lowest, now};
            _historyDirty = true;
        }
    }

    // The test seam (AuctionFixture `stood`, AH8): the item's offers taken that many seconds earlier, so a case need
    // not wait out the standing time. The offers moved
    std::size_t Backdate(std::uint32_t item, std::int64_t seconds)
    {
        auto const it = _items.find(item);
        if (it == _items.end())
            return 0;
        for (auto& [id, line] : it->second.offers)
            line.added -= seconds;
        return it->second.offers.size();
    }

    // History older than the window goes, then items with nothing left (no offer, no history, a forgotten record) and
    // forgotten kinds; an outcome whose Remove never came is forgotten (the core removes right after the outcome hook,
    // so none is pending between two publications)
    void Prune(std::int64_t now, std::int64_t windowSeconds)
    {
        std::int64_t const oldest = now - windowSeconds;
        _recordHalfLife = HalfLife(windowSeconds);
        for (auto it = _items.begin(); it != _items.end();)
        {
            ItemHistory& history = it->second.history;
            std::size_t erased =
                std::erase_if(history.sales, [oldest](SaleRecord const& sale) { return sale.time < oldest; });
            erased +=
                std::erase_if(history.expiries, [oldest](ExpiryRecord const& expiry) { return expiry.time < oldest; });
            if (history.standing.unitPrice && history.standing.time < oldest)
            {
                history.standing = {};
                ++erased;
            }
            if (erased)
                _historyDirty = true;
            if (it->second.offers.empty() && history.sales.empty() && history.expiries.empty() &&
                !history.standing.unitPrice && Forgotten(it->second.record, now, _recordHalfLife))
                it = _items.erase(it);
            else
                ++it;
        }
        std::erase_if(_kinds, [this, now](auto const& kind) { return Forgotten(kind.second, now, _recordHalfLife); });
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
        double const halfLife = HalfLife(windowSeconds);
        for (auto const& [item, book] : _items)
        {
            ItemEntry& entry = out.items[item];
            FillOffers(entry.offers, book.offers);
            FillTypical(entry.typical, book.history, now, windowSeconds);  // the standing price taken before
            entry.record = WeighedAt(book.record, now, halfLife);
            entry.kind = book.kind;
        }
        out.kinds.reserve(_kinds.size());
        for (auto const& [code, recorded] : _kinds)
            out.kinds[code] = WeighedAt(recorded, now, halfLife);
    }

    ItemHistory const* History(std::uint32_t item) const
    {
        auto const it = _items.find(item);
        return it == _items.end() ? nullptr : &it->second.history;
    }

    // The stored history, read once at startup: a record later than now is set to now (a clock that went back), one
    // the book already holds is skipped and the caps keep the newest whatever the order; then the window prunes. The
    // book is clean afterwards unless a row needed a correction the table should get too.
    HistoryCounts LoadHistory(std::vector<HistoryRow> const& rows, std::int64_t now, std::int64_t windowSeconds)
    {
        bool corrected = false;
        for (HistoryRow const& row : rows)
            if (!LoadRow(row, now))
                corrected = true;
        _historyDirty = false;
        _loadOk = true;
        Prune(now, windowSeconds);
        _historyDirty = _historyDirty || corrected;
        return CountHistory();
    }

    // The stored records (AH6), read once at startup before LoadHistory, whose prune then keeps an item while its
    // record is not forgotten: a record weighed to a time later than now is set to now (a clock that went back), a row
    // of no known scope or kind, or with a negative value, is skipped; either leaves the table due a rewrite. The rows
    // taken
    std::size_t LoadRecords(std::vector<RecordRow> const& rows, std::int64_t now)
    {
        std::size_t taken = 0;
        for (RecordRow const& row : rows)
        {
            bool const known =
                row.scope == RECORD_ITEM ? row.id != 0 : row.scope == RECORD_KIND && (row.id & KIND_KNOWN);
            if (!known || !(row.sales >= 0.0) || !(row.hours >= 0.0))
            {
                _recordsDirty = true;
                continue;
            }
            Recorded& recorded = row.scope == RECORD_ITEM ? _items[row.id].record : _kinds[row.id];
            recorded.record = {row.sales, row.hours};
            recorded.time = std::min(row.time, now);
            _recordsDirty = _recordsDirty || row.time > now;
            ++taken;
        }
        return taken;
    }

    // Every record not empty as a row, items in id order, then kinds in code order: the save's full rewrite
    std::vector<RecordRow> ExportRecords() const
    {
        std::vector<RecordRow> rows;
        for (auto const& [item, book] : _items)
            if (!book.record.record.Empty())
                rows.push_back(
                    {RECORD_ITEM, item, book.record.record.sales, book.record.record.hours, book.record.time});
        std::map<std::uint32_t, Recorded const*> kinds;
        for (auto const& [code, recorded] : _kinds)
            kinds[code] = &recorded;
        for (auto const& [code, recorded] : kinds)
            if (!recorded->record.Empty())
                rows.push_back({RECORD_KIND, code, recorded->record.sales, recorded->record.hours, recorded->time});
        return rows;
    }

    // The stored history or records could not be read (WA2a, AH6): the session's own events still feed the index, but
    // no save may overwrite the stored rows it did not read
    void LoadFailed() { _loadOk = false; }

    // Every record as a row, items in id order and each list oldest first: the save's full rewrite
    std::vector<HistoryRow> ExportHistory() const
    {
        std::vector<HistoryRow> rows;
        for (auto const& [item, book] : _items)
        {
            for (SaleRecord const& sale : book.history.sales)
                rows.push_back({item, HISTORY_SALE, sale.time, sale.unitPrice, sale.count, sale.auctionId});
            for (ExpiryRecord const& expiry : book.history.expiries)
                rows.push_back({item, HISTORY_EXPIRY, expiry.time, 0, expiry.count, expiry.auctionId});
            StandingPrice const& standing = book.history.standing;
            if (standing.unitPrice)
                rows.push_back({item, HISTORY_STANDING, standing.time, standing.unitPrice, 1, 0});
        }
        return rows;
    }

    HistoryCounts CountHistory() const
    {
        HistoryCounts counts;
        for (auto const& [item, book] : _items)
        {
            counts.sales += book.history.sales.size();
            counts.expiries += book.history.expiries.size();
            bool const standing = book.history.standing.unitPrice != 0;
            counts.standing += standing ? 1 : 0;
            if (!book.history.sales.empty() || !book.history.expiries.empty() || standing)
                ++counts.items;
        }
        return counts;
    }

    // A sale, an expiry, a standing price taken or a prune that dropped records changed the history since the last save
    bool HistoryDirty() const { return _historyDirty; }
    // A sale, an accrual or a credit fed a record, or a load corrected one, since the last save (AH6)
    bool RecordsDirty() const { return _recordsDirty; }
    // A save is due when the stored rows were read and the history or the records changed since: never before a load
    // or after a failed one. One save writes both
    bool SaveDue() const { return _loadOk && (_historyDirty || _recordsDirty); }
    void MarkHistorySaved()
    {
        _historyDirty = false;
        _recordsDirty = false;
    }

private:
    // A record weighed to its time
    struct Recorded
    {
        Record record;
        std::int64_t time = 0;
    };

    struct ItemBook
    {
        std::map<std::uint32_t, OfferLine> offers;  // by auction id
        ItemHistory history;
        KindKey kind;
        Recorded record;
    };

    // Half the window, as the typical price weighs its sales
    static double HalfLife(std::int64_t windowSeconds)
    {
        return static_cast<double>(std::max<std::int64_t>(windowSeconds / 2, 1));
    }

    static double Weight(std::int64_t age, double halfLife)
    {
        return std::exp2(-static_cast<double>(std::max<std::int64_t>(age, 0)) / halfLife);
    }

    static Record WeighedAt(Recorded const& recorded, std::int64_t now, double halfLife)
    {
        double const weight = Weight(now - recorded.time, halfLife);
        return {recorded.record.sales * weight, recorded.record.hours * weight};
    }

    // Weighs the record to now, then adds; an event older than the record's time (hooks out of order) counts its age
    static void Credit(Recorded& recorded, std::int64_t now, double sales, double hours, double halfLife)
    {
        if (recorded.record.Empty())
            recorded.time = now;
        else if (now > recorded.time)
        {
            recorded.record = WeighedAt(recorded, now, halfLife);
            recorded.time = now;
        }
        double const weight = Weight(recorded.time - now, halfLife);
        recorded.record.sales += sales * weight;
        recorded.record.hours += hours * weight;
    }

    static bool Forgotten(Recorded const& recorded, std::int64_t now, double halfLife)
    {
        Record const record = WeighedAt(recorded, now, halfLife);
        return record.sales < RECORD_FORGET && record.hours < RECORD_FORGET;
    }

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
        auto const before = [](OfferLine const* a, OfferLine const* b) { return LineKey(*a) < LineKey(*b); };
        std::size_t const keep = std::min(order.size(), OFFER_LINES_MAX);
        std::partial_sort(order.begin(), order.begin() + keep, order.end(), before);
        std::vector<OfferLine const*> kept(order.begin(), order.begin() + keep);
        if (order.size() > keep)
        {
            for (OfferLine const* line : LotFrontier(order))
                if (std::find(order.begin(), order.begin() + keep, line) == order.begin() + keep)
                    kept.push_back(line);
            std::sort(kept.begin(), kept.end(), before);
        }
        offers.cheapest.reserve(kept.size());
        for (OfferLine const* line : kept)
            offers.cheapest.push_back(*line);
    }

    // The buyout lots no lot of at least their size undercuts in total (of equal lots the lowest id). Only these can
    // cost a need's line least (AuctionPricing.h LineCost: a larger lot covers as much, its excess units pay back), so
    // the short list keeps them beside the cheapest by unit, which a few big cheap stacks can fill (WA4a). Bounded by
    // the item's distinct lot sizes
    static std::vector<OfferLine const*> LotFrontier(std::vector<OfferLine const*> lots)
    {
        lots.erase(std::remove_if(lots.begin(), lots.end(), [](OfferLine const* line) { return !line->buyout; }),
                   lots.end());
        std::sort(lots.begin(), lots.end(),
                  [](OfferLine const* a, OfferLine const* b)
                  {
                      std::uint32_t const countA = std::max<std::uint32_t>(a->count, 1);
                      std::uint32_t const countB = std::max<std::uint32_t>(b->count, 1);
                      if (countA != countB)
                          return countA > countB;
                      return a->buyout != b->buyout ? a->buyout < b->buyout : a->id < b->id;
                  });
        std::vector<OfferLine const*> frontier;
        std::uint64_t least = std::numeric_limits<std::uint64_t>::max();
        for (OfferLine const* line : lots)
            if (line->buyout < least)
            {
                frontier.push_back(line);
                least = line->buyout;
            }
        return frontier;
    }

    // Each sale weighs 2^(-age / half the window): a sale half the window old counts half a fresh one. The median is
    // the lowest price at which the sorted weights reach half the total; ties sort by time and auction id. Without a
    // sale in the window the standing price in the window is the price (AH.2)
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
        {
            if (history.standing.unitPrice && history.standing.time >= oldest)
            {
                typical.unitPrice = history.standing.unitPrice;
                typical.basis = PriceBasis::Offers;
            }
            return;
        }
        typical.basis = PriceBasis::Sales;
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

    template <typename Record>
    static bool Earlier(Record const& a, Record const& b)
    {
        return std::tie(a.time, a.auctionId) < std::tie(b.time, b.auctionId);
    }

    // Kept sorted by (time, auction id) whatever order the events come in; the oldest go past the cap. True when the
    // cap dropped a record
    template <typename Record>
    static bool InsertCapped(std::vector<Record>& records, Record const& record, std::size_t cap)
    {
        records.insert(std::upper_bound(records.begin(), records.end(), record, Earlier<Record>), record);
        if (records.size() <= cap)
            return false;
        records.erase(records.begin(), records.begin() + (records.size() - cap));
        return true;
    }

    // A stored record into the history unless the list holds the same (time, auction id); a standing price unless the
    // item holds a later one. False when the row was skipped, set to now, or the cap dropped a record
    bool LoadRow(HistoryRow const& row, std::int64_t now)
    {
        if (row.kind != HISTORY_SALE && row.kind != HISTORY_EXPIRY && row.kind != HISTORY_STANDING)
            return false;
        if (row.kind == HISTORY_STANDING && !row.unitPrice)
            return false;
        bool const future = row.time > now;
        std::int64_t const time = future ? now : row.time;
        ItemHistory& history = _items[row.item].history;
        if (row.kind == HISTORY_STANDING)
        {
            if (history.standing.unitPrice && history.standing.time >= time)
                return false;
            bool const replaced = history.standing.unitPrice != 0;
            history.standing = {row.unitPrice, time};
            return !replaced && !future;
        }
        if (row.kind == HISTORY_SALE)
        {
            SaleRecord record;
            record.time = time;
            record.unitPrice = row.unitPrice;
            record.count = row.count;
            record.auctionId = row.auctionId;
            return InsertNew(history.sales, record, SALES_MAX) && !future;
        }
        ExpiryRecord record;
        record.time = time;
        record.count = row.count;
        record.auctionId = row.auctionId;
        return InsertNew(history.expiries, record, EXPIRIES_MAX) && !future;
    }

    template <typename Record>
    static bool InsertNew(std::vector<Record>& records, Record const& record, std::size_t cap)
    {
        auto const at = std::lower_bound(records.begin(), records.end(), record, Earlier<Record>);
        if (at != records.end() && at->time == record.time && at->auctionId == record.auctionId)
            return false;
        return !InsertCapped(records, record, cap);
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
    std::unordered_map<std::uint32_t, Recorded> _kinds;               // by KindKey::code
    double _recordHalfLife = HalfLife(7 * SECONDS_PER_DAY);  // the last Prune's or accrual's; the default window's
    std::uint64_t _sales = 0;
    std::uint64_t _expiries = 0;
    std::uint64_t _cancels = 0;
    std::uint64_t _vanished = 0;
    bool _historyDirty = false;  // the history changed since the last save or load
    bool _recordsDirty = false;  // a record changed since the last save or load
    bool _loadOk = false;        // the stored history was read (LoadHistory), not failed or never tried
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
            << " basis " << BasisWord(entry->typical.basis) << "\n";
        for (OfferLine const& line : entry->offers.cheapest)
            out << "  " << line.id << " x" << line.count << " bid " << line.bid << "/" << line.startBid << " buyout "
                << line.buyout << " end " << line.endTime << " bidder " << line.bidder << " house "
                << static_cast<std::uint32_t>(line.houseId) << "\n";
    }
    return out.str();
}
}  // namespace AuctionIndex

#endif
