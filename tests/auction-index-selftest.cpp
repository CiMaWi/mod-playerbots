/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the auction price index (src/Ai/World/Rpg/AuctionIndexRules.h): planted hook events (adds,
// sales, expiries, cancels; stacks of different counts) against the expected offers, unit prices, medians and counts.
// Exit code 1 on a failed check.
#include <cstdint>
#include <iostream>
#include <string>

#include "AuctionIndexRules.h"

namespace
{
using namespace AuctionIndex;

int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::cout << "FAIL " << what << "\n";
    }
}

constexpr std::int64_t DAY = SECONDS_PER_DAY;
constexpr std::int64_t NOW = 1800000000;
constexpr std::int64_t WINDOW = 7 * DAY;

AuctionFacts Auction(std::uint32_t id, std::uint32_t item, std::uint32_t count, std::uint32_t startBid,
                     std::uint32_t buyout, std::uint8_t house = 7)
{
    AuctionFacts facts;
    facts.id = id;
    facts.item = item;
    facts.count = count;
    facts.startBid = startBid;
    facts.buyout = buyout;
    facts.endTime = NOW + DAY;
    facts.houseId = house;
    return facts;
}

// The facts as the hook sees them at a buyout: bid = buyout, a bidder set
AuctionFacts Bought(AuctionFacts facts, std::uint64_t buyer)
{
    facts.bid = facts.buyout;
    facts.bidder = buyer;
    return facts;
}

Snapshot Take(Book& book, std::int64_t now = NOW)
{
    book.Prune(now, WINDOW);
    Snapshot snapshot;
    book.Fill(snapshot, now, WINDOW);
    return snapshot;
}

ItemEntry const* Item(Snapshot const& snapshot, std::uint32_t item) { return snapshot.Find(item); }

void CheckRounding()
{
    Check(UnitPrice(100, 3) == 33, "unit 100/3 rounds to 33");
    Check(UnitPrice(101, 3) == 34, "unit 101/3 rounds to 34");
    Check(UnitPrice(5, 2) == 3, "unit 5/2 rounds half up to 3");
    Check(UnitPrice(500, 5) == 100, "unit 500/5 is 100");
    Check(UnitPrice(4294967295u, 1) == 4294967295u, "unit of the largest copper value does not overflow");
    Check(UnitPrice(7, 0) == 7, "a count of 0 is read as 1");
}

// Adds, one buyout sale, one cancel, one expiry: offers, units, cheapest unit buyout, lines, history counters
void CheckLifecycle()
{
    Book book;
    book.Add(Auction(1, 100, 5, 300, 500));   // unit buyout 100
    book.Add(Auction(2, 100, 1, 60, 90));     // unit buyout 90
    book.Add(Auction(3, 100, 20, 1000, 0));   // no buyout: an offer, not a cheapest buyout
    book.Add(Auction(4, 200, 1, 10, 20, 2));  // another item, another house id

    Snapshot s = Take(book);
    ItemEntry const* e = Item(s, 100);
    Check(e && e->offers.auctions == 3, "three offers of item 100");
    Check(e && e->offers.units == 26, "units 5+1+20");
    Check(e && e->offers.cheapestUnitBuyout == 90, "cheapest unit buyout 90 (the single, not the stack's total)");
    Check(e && e->offers.cheapest.size() == 3 && e->offers.cheapest[0].id == 2 && e->offers.cheapest[1].id == 1 &&
              e->offers.cheapest[2].id == 3,
          "lines ordered by unit buyout, the auction without buyout last");
    Check(e && e->typical.sample == 0 && e->typical.unitPrice == 0, "no sale yet: typical none");
    Check(s.auctions == 4, "four offers in the house");
    Check(Item(s, 200) && Item(s, 200)->offers.cheapest[0].houseId == 2, "the house id is kept per offer");

    // Buyout of the 5-stack: Successful then Remove is one sale at 500/5 = 100 per unit
    book.Sale(Bought(Auction(1, 100, 5, 300, 500), 77), NOW);
    book.Remove(1);
    s = Take(book);
    e = Item(s, 100);
    Check(e && e->offers.auctions == 2 && e->offers.units == 21, "the sold auction is no offer");
    Check(e && e->typical.sample == 1 && e->typical.unitPrice == 100, "one sale, unit price 100");
    Check(s.sales == 1 && s.cancels == 0, "Successful then Remove: one sale, no cancel");

    // Cancel of auction 2: Remove alone
    book.Remove(2);
    s = Take(book);
    e = Item(s, 100);
    Check(e && e->offers.auctions == 1 && e->offers.cheapestUnitBuyout == 0, "the cancelled auction is no offer");
    Check(e && e->offers.cheapest.size() == 1 && e->offers.cheapest[0].id == 3, "only auction 3 stays listed");
    Check(e && e->typical.sample == 1, "a cancel is never counted as a sale");
    Check(s.cancels == 1 && s.sales == 1, "Remove alone: one cancel");

    // Expiry of auction 3: Expire then Remove
    book.Expire(Auction(3, 100, 20, 1000, 0), NOW);
    book.Remove(3);
    s = Take(book);
    e = Item(s, 100);
    Check(e && e->offers.auctions == 0 && e->offers.units == 0 && e->offers.cheapest.empty(), "no offer left");
    Check(e && e->typical.expiries == 1 && e->typical.sample == 1, "Expire then Remove: one expiry, no sale");
    Check(s.expiries == 1 && s.cancels == 1, "an expiry is no cancel");

    // Negative control: a Remove of an id never seen changes nothing
    std::string const before = Dump(Take(book));
    book.Remove(999);
    Check(Dump(Take(book)) == before, "a Remove of an unknown id changes nothing");
}

// A bid that is not a buyout fires no hook: the publish's refresh reads bid and bidder; a pure bid wins at expiry
void CheckBidAndVanish()
{
    Book book;
    book.Add(Auction(10, 300, 1, 50, 200));
    book.Add(Auction(11, 300, 4, 40, 400));
    book.Refresh(
        [](OfferLine& line)
        {
            if (line.id == 11)
                return false;  // gone from the house without a hook
            line.bid = 70;
            line.bidder = 42;
            return true;
        });
    Snapshot s = Take(book);
    ItemEntry const* e = Item(s, 300);
    Check(e && e->offers.auctions == 1 && e->offers.cheapest[0].bid == 70 && e->offers.cheapest[0].bidder == 42,
          "the refresh copies the current bid and bidder");
    Check(s.vanished == 1 && s.cancels == 0 && s.sales == 0, "a vanished auction is dropped, neither sale nor cancel");

    AuctionFacts won = Auction(10, 300, 1, 50, 200);
    won.bid = 70;
    won.bidder = 42;
    book.Sale(won, NOW);
    book.Remove(10);
    s = Take(book);
    e = Item(s, 300);
    Check(e && e->typical.unitPrice == 70 && e->typical.sample == 1, "a pure bid winning at expiry sells at its bid");
    Check(e && e->offers.auctions == 0, "the won auction is no offer");

    // A sale the book never saw added (an auction older than the index) still counts, nothing goes negative
    book.Sale(Bought(Auction(12, 301, 2, 10, 30), 5), NOW);
    book.Remove(12);
    s = Take(book);
    Check(Item(s, 301) && Item(s, 301)->typical.unitPrice == 15 && Item(s, 301)->offers.auctions == 0,
          "a sale of an unseen auction is recorded");
    Check(s.auctions == 0, "no offers left at all");

    // An Add of an id already listed replaces the line instead of counting twice
    book.Add(Auction(20, 302, 3, 10, 90));
    book.Add(Auction(20, 302, 2, 10, 60));
    s = Take(book);
    Check(Item(s, 302) && Item(s, 302)->offers.auctions == 1 && Item(s, 302)->offers.units == 2,
          "a repeated Add replaces its line");
}

// The recency-weighted median, the window and the caps
void CheckMedian()
{
    Book book;
    std::uint32_t id = 100;
    auto sell = [&](std::uint32_t unit, std::int64_t age)
    {
        AuctionFacts facts = Bought(Auction(++id, 400, 1, unit, unit), 1);
        book.Add(facts);
        book.Sale(facts, NOW - age);
        book.Remove(facts.id);
    };
    sell(10, 0);
    sell(1000, 6 * DAY);
    sell(1000, 6 * DAY + 1);
    Snapshot s = Take(book);
    ItemEntry const* e = Item(s, 400);
    Check(e && e->typical.sample == 3, "three sales in the window");
    Check(e && e->typical.unitPrice == 10, "the fresh sale outweighs two old ones (plain median would be 1000)");

    Book even;
    std::uint32_t evenId = 0;
    for (std::uint32_t unit : {30u, 10u, 20u})
    {
        AuctionFacts facts = Bought(Auction(++evenId, 401, 1, unit, unit), 1);
        even.Add(facts);
        even.Sale(facts, NOW);
        even.Remove(facts.id);
    }
    Check(Item(Take(even), 401) && Item(Take(even), 401)->typical.unitPrice == 20, "same ages: the plain median");

    sell(5, 8 * DAY);  // older than the window
    s = Take(book);
    e = Item(s, 400);
    Check(e && e->typical.sample == 3, "a sale older than HistoryDays is dropped");

    // Expiries age out the same way
    book.Expire(Auction(900, 400, 1, 1, 0), NOW - 8 * DAY);
    book.Remove(900);
    book.Expire(Auction(901, 400, 1, 1, 0), NOW - DAY);
    book.Remove(901);
    s = Take(book);
    Check(Item(s, 400) && Item(s, 400)->typical.expiries == 1, "an expiry older than HistoryDays is dropped");

    // History cap: the newest SALES_MAX sales stay
    Book many;
    for (std::uint32_t i = 1; i <= SALES_MAX + 12; ++i)
    {
        AuctionFacts facts = Bought(Auction(i, 500, 1, i, i), 1);
        many.Sale(facts, NOW - static_cast<std::int64_t>(SALES_MAX + 12 - i));
        many.Remove(i);
    }
    s = Take(many);
    Check(Item(s, 500) && Item(s, 500)->typical.sample == SALES_MAX, "history capped at SALES_MAX per item");
    Check(many.History(500) && many.History(500)->sales.front().unitPrice == 13, "the cap drops the oldest");

    // Line cap: the cheapest OFFER_LINES_MAX of eight, all eight counted
    Book lines;
    for (std::uint32_t i = 1; i <= 8; ++i)
        lines.Add(Auction(i, 600, 2, 1, (9 - i) * 10));  // unit buyouts 40, 35, ... 5
    s = Take(lines);
    e = Item(s, 600);
    Check(e && e->offers.auctions == 8 && e->offers.cheapest.size() == OFFER_LINES_MAX, "lines capped, all counted");
    Check(e && e->offers.cheapest.front().id == 8 && e->offers.cheapest.back().id == 4, "the cheapest lines kept");
    Check(e && e->offers.cheapestUnitBuyout == 5, "cheapest unit buyout of the 2-stacks");
}

// The same events in the same order give the same snapshot; one more event gives another (negative control)
void CheckDeterminism()
{
    auto play = [](Book& book, bool extraCancel)
    {
        for (std::uint32_t i = 1; i <= 20; ++i)
            book.Add(Auction(i, 700 + i % 3, 1 + i % 4, 10 * i, i % 5 ? 30 * i : 0, i % 2 ? 7 : 2));
        for (std::uint32_t i = 1; i <= 20; i += 3)
        {
            book.Sale(Bought(Auction(i, 700 + i % 3, 1 + i % 4, 10 * i, 30 * i), 9), NOW - i * 3600);
            book.Remove(i);
        }
        for (std::uint32_t i = 2; i <= 20; i += 6)
        {
            book.Expire(Auction(i, 700 + i % 3, 1 + i % 4, 10 * i, 0), NOW - i * 60);
            book.Remove(i);
        }
        book.Remove(3);  // a cancel
        if (extraCancel)
            book.Remove(5);
    };
    Book a;
    Book b;
    Book c;
    play(a, false);
    play(b, false);
    play(c, true);
    std::string const dumpA = Dump(Take(a));
    Check(!dumpA.empty() && dumpA == Dump(Take(b)), "same events, same order: same snapshot");
    Check(dumpA != Dump(Take(c)), "one more cancel: another snapshot (the comparison can fail)");
    Check(Dump(Take(a)) == dumpA, "taking a snapshot does not change the book");
}

bool SameHistory(ItemHistory const* a, ItemHistory const* b)
{
    if (!a || !b)
        return a == b;
    if (a->sales.size() != b->sales.size() || a->expiries.size() != b->expiries.size())
        return false;
    for (std::size_t i = 0; i < a->sales.size(); ++i)
    {
        SaleRecord const& x = a->sales[i];
        SaleRecord const& y = b->sales[i];
        if (x.time != y.time || x.unitPrice != y.unitPrice || x.count != y.count || x.auctionId != y.auctionId)
            return false;
    }
    for (std::size_t i = 0; i < a->expiries.size(); ++i)
    {
        ExpiryRecord const& x = a->expiries[i];
        ExpiryRecord const& y = b->expiries[i];
        if (x.time != y.time || x.count != y.count || x.auctionId != y.auctionId)
            return false;
    }
    return true;
}

bool SameTypical(Snapshot const& a, Snapshot const& b, std::uint32_t item)
{
    ItemEntry const* x = a.Find(item);
    ItemEntry const* y = b.Find(item);
    return x && y && x->typical.unitPrice == y->typical.unitPrice && x->typical.sample == y->typical.sample &&
           x->typical.weight == y->typical.weight && x->typical.expiries == y->typical.expiries;
}

bool SameRows(std::vector<HistoryRow> const& a, std::vector<HistoryRow> const& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].item != b[i].item || a[i].kind != b[i].kind || a[i].time != b[i].time ||
            a[i].unitPrice != b[i].unitPrice || a[i].count != b[i].count || a[i].auctionId != b[i].auctionId)
            return false;
    return true;
}

// WA1d: the table as plain rows, written by the book's export and read by its import
void CheckHistoryStore()
{
    // Round trip: stacks, both caps hit, an item with offers only
    Book a;
    for (std::uint32_t i = 1; i <= 60; ++i)
    {
        a.Sale(Bought(Auction(i, 800, 1 + i % 5, 10, 100 + 7 * i), 9), NOW - static_cast<std::int64_t>(i) * 600);
        a.Remove(i);
    }
    for (std::uint32_t i = 101; i <= 110; ++i)
    {
        a.Sale(Bought(Auction(i, 801, 20, 10, 2000 + i), 9), NOW - static_cast<std::int64_t>(i) * 300);
        a.Remove(i);
        a.Expire(Auction(i + 100, 801, 5, 10, 0), NOW - static_cast<std::int64_t>(i) * 200);
        a.Remove(i + 100);
    }
    for (std::uint32_t i = 301; i <= 350; ++i)
    {
        a.Expire(Auction(i, 802, 1, 10, 50), NOW - static_cast<std::int64_t>(i) * 60);
        a.Remove(i);
    }
    a.Add(Auction(400, 803, 1, 10, 20));
    std::vector<HistoryRow> const rows = a.ExportHistory();
    Check(rows.size() == SALES_MAX + 10 + 10 + EXPIRIES_MAX, "export: every record one row, the caps held");
    Book b;
    HistoryCounts const loaded = b.LoadHistory(rows, NOW, WINDOW);
    Check(loaded.sales == SALES_MAX + 10 && loaded.expiries == 10 + EXPIRIES_MAX && loaded.items == 3,
          "import: the counts of the log line");
    Check(SameHistory(a.History(800), b.History(800)) && SameHistory(a.History(801), b.History(801)) &&
              SameHistory(a.History(802), b.History(802)),
          "round trip: each history equal field by field");
    Check(!b.History(803), "round trip: an item with offers only stores nothing");
    Snapshot const sa = Take(a);
    Snapshot const sb = Take(b);
    Check(SameTypical(sa, sb, 800) && SameTypical(sa, sb, 801) && SameTypical(sa, sb, 802) &&
              sb.Find(800)->typical.sample == SALES_MAX,
          "round trip: the same typical prices");
    Check(SameRows(rows, b.ExportHistory()), "round trip: the import exports the same rows");

    // Negative control: one changed price is seen
    std::vector<HistoryRow> changed = rows;
    changed.back().count += 1;
    changed.front().unitPrice += 1;
    Book c;
    c.LoadHistory(changed, NOW, WINDOW);
    Check(!SameHistory(a.History(800), c.History(800)) && !SameHistory(a.History(802), c.History(802)),
          "a changed row gives another history (the comparison can fail)");

    // Order does not matter, duplicates do not double
    std::vector<HistoryRow> reversed(rows.rbegin(), rows.rend());
    reversed.push_back(rows.front());
    reversed.push_back(rows.back());
    Book d;
    d.LoadHistory(reversed, NOW, WINDOW);
    Check(SameRows(rows, d.ExportHistory()), "reversed order and duplicate rows: the same history");

    // The cap holds the newest whatever the order: 60 stored sales of one item keep the newest 48
    std::vector<HistoryRow> many;
    for (std::uint32_t i = 1; i <= 60; ++i)
        many.push_back({900, HISTORY_SALE, NOW - static_cast<std::int64_t>(i) * 60, i, 1, i});
    Book capped;
    capped.LoadHistory(many, NOW, WINDOW);
    ItemHistory const* h = capped.History(900);
    Check(h && h->sales.size() == SALES_MAX && h->sales.front().auctionId == SALES_MAX &&
              h->sales.back().auctionId == 1 && capped.HistoryDirty(),
          "more stored than the cap: the newest kept, the table due a rewrite");

    // Window: older than the window goes, the edge stays, an item with only old rows leaves no entry
    std::vector<HistoryRow> const old = {
        {910, HISTORY_SALE, NOW - WINDOW - 1, 100, 1, 1},
        {910, HISTORY_SALE, NOW - WINDOW, 120, 1, 2},
        {911, HISTORY_EXPIRY, NOW - WINDOW - 5, 0, 1, 3},
        {911, HISTORY_SALE, NOW - 2 * WINDOW, 90, 1, 4},
    };
    Book w;
    HistoryCounts const windowed = w.LoadHistory(old, NOW, WINDOW);
    h = w.History(910);
    Check(h && h->sales.size() == 1 && h->sales.front().time == NOW - WINDOW && h->sales.front().unitPrice == 120,
          "window: the older sale dropped, the one at the edge kept");
    Check(!w.History(911) && windowed.items == 1 && windowed.sales == 1 && windowed.expiries == 0,
          "window: an item with only old rows leaves no entry");
    Check(w.HistoryDirty(), "window: a load that dropped rows leaves the table due a rewrite");

    // Empty table: an empty book, nothing to write
    Book e;
    HistoryCounts const none = e.LoadHistory({}, NOW, WINDOW);
    Check(none.sales == 0 && none.expiries == 0 && none.items == 0, "empty: counts 0");
    Check(Take(e).items.empty() && !e.History(800) && e.ExportHistory().empty() && !e.HistoryDirty(),
          "empty: no items, no history, nothing to save");

    // Future times are set to now; an unknown kind is skipped
    std::vector<HistoryRow> const future = {
        {920, HISTORY_SALE, NOW + 3600, 50, 2, 1},
        {920, HISTORY_EXPIRY, NOW + 60, 0, 1, 2},
        {921, 7, NOW - 60, 10, 1, 3},
    };
    Book f;
    f.LoadHistory(future, NOW, WINDOW);
    h = f.History(920);
    Check(h && h->sales.size() == 1 && h->sales.front().time == NOW && h->expiries.size() == 1 &&
              h->expiries.front().time == NOW,
          "future: records set to now");
    Check(!f.History(921) && f.HistoryDirty(), "future: an unknown kind skipped, the table due a rewrite");

    // Dirty: clean after a clean import, set by a sale, an expiry and a pruning erase, cleared by the save
    Book g;
    g.LoadHistory(rows, NOW, WINDOW);
    Check(!g.HistoryDirty(), "dirty: false after a clean import");
    g.Sale(Bought(Auction(500, 800, 1, 10, 99), 9), NOW);
    g.Remove(500);
    Check(g.HistoryDirty(), "dirty: true after a sale");
    g.MarkHistorySaved();
    Check(!g.HistoryDirty(), "dirty: false after the save");
    g.Expire(Auction(501, 800, 1, 10, 99), NOW);
    g.Remove(501);
    Check(g.HistoryDirty(), "dirty: true after an expiry");
    g.MarkHistorySaved();
    g.Prune(NOW, WINDOW);
    Check(!g.HistoryDirty(), "dirty: a prune that drops nothing leaves it clean");
    g.Prune(NOW + WINDOW, WINDOW);
    Check(g.HistoryDirty(), "dirty: true after a prune that dropped records");
}
}  // namespace

int main()
{
    CheckRounding();
    CheckLifecycle();
    CheckBidAndVanish();
    CheckMedian();
    CheckDeterminism();
    CheckHistoryStore();
    std::cout << "auction-index: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
