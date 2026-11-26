/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the auction price index (src/Ai/World/Rpg/AuctionIndexRules.h): planted hook events (adds,
// sales, expiries, cancels; stacks of different counts) against the expected offers, unit prices, medians and counts.
// A seeded search over hook streams checks the invariants against a plain model. Exit code 1 on a failed check.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <string>

#include "AuctionIndexRules.h"
#include "AuctionPricing.h"

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

bool Near(double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b)); }

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

    // Loaded ok (WA2a): a failed load keeps the session's records for the index but never saves over the stored rows
    Book lost;
    lost.LoadFailed();
    lost.Sale(Bought(Auction(600, 800, 2, 10, 300), 9), NOW);
    lost.Remove(600);
    lost.Expire(Auction(601, 800, 1, 10, 0), NOW);
    lost.Remove(601);
    Check(lost.HistoryDirty() && !lost.SaveDue(), "loaded ok: after a failed load a sale and an expiry are no save");
    Check(lost.ExportHistory().size() == 2, "loaded ok: after a failed load the session's two records are kept");
    Snapshot const sl = Take(lost);
    Check(sl.Find(800) && sl.Find(800)->typical.unitPrice == 150 && sl.Find(800)->typical.sample == 1 &&
              sl.Find(800)->typical.expiries == 1,
          "loaded ok: after a failed load the typical price reads the session's records");
    Book never;
    never.Sale(Bought(Auction(610, 800, 1, 10, 90), 9), NOW);
    never.Remove(610);
    Check(never.HistoryDirty() && !never.SaveDue(), "loaded ok: a book never loaded saves nothing");
    Book fresh;
    fresh.LoadHistory({}, NOW, WINDOW);
    Check(!fresh.SaveDue(), "loaded ok: after an empty load no save until a change");
    fresh.Sale(Bought(Auction(620, 800, 1, 10, 90), 9), NOW);
    fresh.Remove(620);
    Check(fresh.SaveDue(), "loaded ok: after an empty load a sale makes the save due");
    // Negative control: the old gate without the flag would write the failed book's two rows over the stored ones
    auto const gateWithoutFlag = [](Book const& book) { return book.HistoryDirty(); };
    Check(gateWithoutFlag(lost), "loaded ok control: a gate that ignores the flag saves after a failed load (caught)");
}

// A seeded search over hook streams (PLAN 0, open-ended inputs): adds, sales, expiries and cancels in a random mix
// against a plain model. Invariants: "offers" (the snapshot lists exactly the model's live auctions, no line of an
// ended one), "counters" (sales, expiries and cancels as the model counts them: a cancel never counts as a sale),
// "sample" (each item's sample is its sales in the window, up to the cap). Each fault is a negative control that one
// invariant must catch.
enum class StreamFault
{
    None,
    SaleAsCancel,  // one sale fed as a bare Remove: "counters"
    LostAdd,       // one listing never reaches the book: "offers"
    StaleSale,     // one sale older than the window, counted by the model: "sample"
    KindedAdd      // one listing given a kind, so its item's sales feed records the hooks must not feed yet: "records"
};

struct StreamModel
{
    std::map<std::uint32_t, std::uint32_t> live;     // auction id -> item
    std::map<std::uint32_t, std::uint32_t> salesOf;  // item -> sales in the window
    std::uint64_t sales = 0;
    std::uint64_t expiries = 0;
    std::uint64_t cancels = 0;
};

// The invariants the snapshot breaks against the model, by name; empty when it holds them all
std::string StreamMismatch(Snapshot const& snapshot, StreamModel const& model)
{
    std::map<std::uint32_t, std::uint32_t> offersOf;
    for (auto const& [id, item] : model.live)
        ++offersOf[item];
    bool offers = snapshot.auctions == model.live.size();
    bool sample = true;
    bool records = snapshot.kinds.empty();  // the stream's adds carry no kind (a plain Add)
    for (auto const& [item, entry] : snapshot.items)
    {
        records = records && entry.record.Empty() && !entry.kind.Known();
        auto const listed = offersOf.find(item);
        offers = offers && entry.offers.auctions == (listed == offersOf.end() ? 0u : listed->second);
        for (OfferLine const& line : entry.offers.cheapest)
            offers = offers && model.live.count(line.id) == 1;
        auto const sold = model.salesOf.find(item);
        std::size_t const want = sold == model.salesOf.end() ? 0 : std::min<std::size_t>(sold->second, SALES_MAX);
        sample = sample && entry.typical.sample == want;
    }
    for (auto const& [item, n] : offersOf)
        offers = offers && snapshot.Find(item) != nullptr;
    for (auto const& [item, n] : model.salesOf)
        sample = sample && snapshot.Find(item) != nullptr;
    bool const counters =
        snapshot.sales == model.sales && snapshot.expiries == model.expiries && snapshot.cancels == model.cancels;
    return std::string(offers ? "" : "offers ") + (counters ? "" : "counters ") + (sample ? "" : "sample ") +
           (records ? "" : "records ");
}

// The first mismatch of a seeded stream, "" when none; the seed and step say where to replay it
std::string PlayStream(std::uint32_t seed, StreamFault fault)
{
    std::mt19937 rng(seed);
    Book book;
    StreamModel model;
    std::uint32_t nextId = 1;
    bool faulted = false;
    constexpr int STEPS = 3000;
    for (int step = 0; step < STEPS; ++step)
    {
        bool const faultedBefore = faulted;
        std::uint32_t const roll = rng() % 100;
        if (roll < 40 || model.live.empty())
        {
            std::uint32_t const item = 1000 + rng() % 6;
            std::uint32_t const count = 1 + rng() % 20;
            std::uint32_t const startBid = 5 + rng() % 50;
            std::uint32_t const buyout = rng() % 4 ? 10 + rng() % 5000 : 0;
            if (fault == StreamFault::KindedAdd && !faulted)
                book.Add(Auction(nextId, item, count, startBid, buyout), KindOf(4, 2, 25, 30));
            else if (fault != StreamFault::LostAdd || faulted)
                book.Add(Auction(nextId, item, count, startBid, buyout));
            faulted = faulted || fault == StreamFault::LostAdd || fault == StreamFault::KindedAdd;
            model.live[nextId++] = item;
        }
        else
        {
            auto const it = std::next(model.live.begin(), static_cast<std::ptrdiff_t>(rng() % model.live.size()));
            std::uint32_t const id = it->first;
            std::uint32_t const item = it->second;
            AuctionFacts const facts = Auction(id, item, 1 + rng() % 20, 10, 100 + rng() % 900);
            std::int64_t const at = NOW - static_cast<std::int64_t>(rng() % DAY);
            if (roll < 65)
            {
                if (fault == StreamFault::SaleAsCancel && !faulted)
                    book.Remove(id);
                else if (fault == StreamFault::StaleSale && !faulted)
                    book.Sale(Bought(facts, 9), NOW - 2 * WINDOW);
                else
                    book.Sale(Bought(facts, 9), at);
                faulted = faulted || fault == StreamFault::SaleAsCancel || fault == StreamFault::StaleSale;
                book.Remove(id);
                ++model.sales;
                ++model.salesOf[item];
            }
            else if (roll < 85)
            {
                book.Expire(facts, at);
                book.Remove(id);
                ++model.expiries;
            }
            else
            {
                book.Remove(id);
                ++model.cancels;
            }
            model.live.erase(it);
        }
        // Every tenth step on average, the last, and the step a fault went in (before a later event can mask it)
        bool const sampled = rng() % 10 == 0;
        if (sampled || faulted != faultedBefore || step + 1 == STEPS)
        {
            std::string const why = StreamMismatch(Take(book), model);
            if (!why.empty())
                return "seed " + std::to_string(seed) + " step " + std::to_string(step) + ": " + why;
        }
    }
    return "";
}

void CheckSeededStreams()
{
    for (std::uint32_t const seed : {20261125u, 1u, 77u, 4242u})
    {
        std::string const found = PlayStream(seed, StreamFault::None);
        Check(found.empty(), "seeded stream holds offers, counters and sample (" + found + ")");
    }
    Check(PlayStream(20261125u, StreamFault::SaleAsCancel).find("counters") != std::string::npos,
          "negative control: a sale fed as a bare Remove breaks the counters");
    Check(PlayStream(20261125u, StreamFault::LostAdd).find("offers") != std::string::npos,
          "negative control: a listing the book never saw breaks the offers");
    Check(PlayStream(20261125u, StreamFault::StaleSale).find("sample") != std::string::npos,
          "negative control: a sale outside the window breaks the sample");
    Check(PlayStream(20261125u, StreamFault::KindedAdd).find("records") != std::string::npos,
          "negative control: a listing given a kind feeds records");
}

// A book with `items` items of one kind (pooled) or of a kind each, each listed once for 24 h and expired unsold
Book ListedUnsold(KindKey kind, std::uint32_t items, bool pooled)
{
    Book book;
    for (std::uint32_t i = 0; i < items; ++i)
        book.Add(Auction(100 + i, 5000 + i, 1, 10, 20), pooled ? kind : KindKey{KIND_KNOWN | (0xFF0000u - i)});
    for (std::int64_t hour = 1; hour <= 24; ++hour)
        book.AccrueListingHours(NOW + hour * 3600, 3600, WINDOW);
    for (std::uint32_t i = 0; i < items; ++i)
    {
        book.Expire(Auction(100 + i, 5000 + i, 1, 10, 20), NOW + 24 * 3600);
        book.Remove(100 + i);
    }
    return book;
}

// AH5: the records (PLAN 3.3). Only offers of a known kind feed them: listing hours at each accrual (lots standing
// times the interval), a sale at its hook, to the item and its kind. They outlive the history's prune, are weighed with
// half the window as half-life and forgotten at last. Items of one kind listed and unsold lower the next one's chance
// (control: a kind per item leaves it at the start), which drifts back to the start as the kind's record decays
void CheckRecords()
{
    AuctionConfig const config;
    KindKey const green = KindOf(4, 2, 25, 30);
    Check(green.Known() && green == KindOf(4, 2, 29, 40) && !(green == KindOf(4, 2, 30, 30)) &&
              !(green == KindOf(4, 3, 25, 30)) && !(green == KindOf(2, 2, 25, 30)) &&
              !(KindOf(7, 1, 0, 25) == KindOf(7, 1, 25, 25)) && KindOf(7, 1, 0, 25) == KindOf(7, 1, 0, 29),
          "kind: class, quality and the required level's ten-level band; none required: the item level's, apart");

    Book plain;
    plain.Add(Auction(1, 100, 1, 10, 20));
    plain.AccrueListingHours(NOW, 3600, WINDOW);
    plain.Sale(Bought(Auction(1, 100, 1, 10, 20), 9), NOW);
    plain.Remove(1);
    Snapshot s = Take(plain);
    Check(Item(s, 100) && Item(s, 100)->record.Empty() && s.kinds.empty(),
          "no kind (a plain Add): neither the hours nor the sale feed a record");

    Book book;
    book.Add(Auction(1, 100, 1, 10, 20), green);
    book.Add(Auction(2, 100, 1, 10, 20), green);
    book.AccrueListingHours(NOW, 3600, WINDOW);
    book.Sale(Bought(Auction(1, 100, 1, 10, 20), 9), NOW);
    book.Remove(1);
    s = Take(book);
    ItemEntry const* e = Item(s, 100);
    Record const kind = s.KindRecord(green);
    Check(e && e->kind == green && Near(e->record.hours, 2.0) && Near(e->record.sales, 1.0) && Near(kind.hours, 2.0) &&
              Near(kind.sales, 1.0),
          "two lots standing an hour: 2 lot hours, the sale 1 lot, to the item and its kind");
    double const halfHour = std::exp2(-1800.0 / static_cast<double>(WINDOW / 2));
    book.AccrueListingHours(NOW + 1800, 1800, WINDOW);
    s = Take(book, NOW + 1800);
    e = Item(s, 100);
    Check(e && Near(e->record.hours, 2.0 * halfHour + 0.5) && Near(e->record.sales, halfHour),
          "half an hour later: the sold lot adds no hours, the one left half an hour; both weighed by the half-life");
    book.Expire(Auction(2, 100, 1, 10, 20), NOW + 1800);
    book.Remove(2);
    s = Take(book, NOW + 2 * WINDOW);
    e = Item(s, 100);
    Check(e && e->typical.sample == 0 && e->typical.expiries == 0 && e->offers.auctions == 0 &&
              Near(e->record.hours, (2.0 * halfHour + 0.5) * std::exp2(-4.0 + 1800.0 / (WINDOW / 2))),
          "the record outlives the history's prune, weighed 1/16 two windows on");
    s = Take(book, NOW + 20 * WINDOW);
    Check(!Item(s, 100) && s.kinds.empty(), "forgotten 40 half-lives on: the item and its kind go");

    constexpr std::uint32_t ITEMS = 20;
    std::int64_t const ended = NOW + 24 * 3600;
    auto nextChance = [&](Book& listed, std::int64_t at)
    {
        Evidence const next = Take(listed, at).EvidenceOf(6000, green);
        return AuctionRules::SaleChance(next.item, next.kind, 24.0, config);
    };
    Book pooled = ListedUnsold(green, ITEMS, true);
    Book perItem = ListedUnsold(green, ITEMS, false);
    double const lowered = nextChance(pooled, ended);
    double const atStart = nextChance(perItem, ended);
    std::cout << "records: after " << ITEMS << " unsold 24 h listings of one kind the next item's chance is " << lowered
              << " (a kind per item: " << atStart << ")\n";
    Check(lowered < 0.5, "N items of one kind listed once and unsold lower the next one's chance below 1/2");
    Check(atStart == 0.5, "control: a kind per item leaves the next one at the start (caught)");
    double last = lowered;
    bool rises = true;
    for (std::int64_t const later : {WINDOW, 3 * WINDOW, 6 * WINDOW})
    {
        double const now = nextChance(pooled, ended + later);
        rises = rises && now > last && now <= 0.5;
        last = now;
    }
    Check(rises && last > 0.49 && nextChance(pooled, ended + 40 * WINDOW) == 0.5,
          "left alone the kind drifts back toward the start as its record decays, and is at it once forgotten");
}

// The records' store and feed (AH6): a round trip, a clock that went back, rows of no scope, what makes a save due, a
// failed load, and the wa-index case's numbers (the seam's 48 listing hours and one sale)
bool SameRecordRows(std::vector<RecordRow> const& a, std::vector<RecordRow> const& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].scope != b[i].scope || a[i].id != b[i].id || a[i].sales != b[i].sales || a[i].hours != b[i].hours ||
            a[i].time != b[i].time)
            return false;
    return true;
}

void CheckRecordStore()
{
    AuctionConfig const config;
    KindKey const green = KindOf(4, 2, 25, 30);
    KindKey const grey = KindOf(15, 0, 0, 5);

    // Round trip: a sold item, a listed one, one only credited by the seam, one listed without a kind
    Book a;
    a.LoadHistory({}, NOW - DAY, WINDOW);
    a.Add(Auction(1, 100, 1, 10, 20), green);
    a.Add(Auction(2, 101, 1, 10, 20), grey);
    a.Add(Auction(3, 102, 1, 10, 20));
    a.AccrueListingHours(NOW - DAY, 3600, WINDOW);
    a.Sale(Bought(Auction(1, 100, 1, 10, 20), 9), NOW - DAY / 2);
    a.Remove(1);
    a.CreditListing(103, green, 5.0, NOW - DAY / 4, WINDOW);
    std::vector<RecordRow> const rows = a.ExportRecords();
    Check(rows.size() == 5 && rows[0].scope == RECORD_ITEM && rows[0].id == 100 && rows[1].id == 101 &&
              rows[2].id == 103 && rows[3].scope == RECORD_KIND && rows[3].id == green.code && rows[4].id == grey.code,
          "export: the three items' records in id order, then the two kinds'; an item without a kind has none");
    Book b;
    Check(b.LoadRecords(rows, NOW) == 5, "import: every row taken");
    b.LoadHistory(a.ExportHistory(), NOW, WINDOW);
    Snapshot const sa = Take(a);
    Snapshot const sb = Take(b);
    bool same = true;
    for (std::uint32_t const item : {100u, 101u, 103u})
        same = same && Item(sa, item) && Item(sb, item) &&
               Near(Item(sa, item)->record.sales, Item(sb, item)->record.sales) &&
               Near(Item(sa, item)->record.hours, Item(sb, item)->record.hours);
    for (KindKey const kind : {green, grey})
        same = same && Near(sa.KindRecord(kind).sales, sb.KindRecord(kind).sales) &&
               Near(sa.KindRecord(kind).hours, sb.KindRecord(kind).hours);
    Check(same && Near(Item(sb, 103)->record.hours, 5.0 * std::exp2(-0.25 * DAY / (WINDOW / 2))),
          "round trip: the same records, weighed on from their times");
    Check(!Item(sb, 103)->kind.Known() && Item(sb, 103)->offers.auctions == 0,
          "an item only loaded keeps its record through the history's prune, with no kind until an add");
    Check(SameRecordRows(rows, b.ExportRecords()) && !b.SaveDue(), "the import exports the same rows and is clean");
    std::vector<RecordRow> changed = rows;
    changed[0].hours += 1.0;
    Book c;
    c.LoadRecords(changed, NOW);
    Check(!Near(Take(c).Find(100)->record.hours, Item(sa, 100)->record.hours),
          "a changed row gives another record (the comparison can fail)");

    // A clock that went back: a record weighed to a later time is set to now, and decays from now on
    Book future;
    future.LoadRecords({{RECORD_KIND, green.code, 0.0, 10.0, NOW + DAY}}, NOW);
    future.LoadHistory({}, NOW, WINDOW);
    Check(
        Near(Take(future, NOW + WINDOW / 2).KindRecord(green).hours, 5.0) && future.RecordsDirty() && future.SaveDue(),
        "a record of a future time: set to now (half of it half a window on), the table due a rewrite");
    Book bad;
    Check(bad.LoadRecords({{7, 100, 1.0, 1.0, NOW},
                           {RECORD_KIND, 12, 1.0, 1.0, NOW},
                           {RECORD_ITEM, 0, 1.0, 1.0, NOW},
                           {RECORD_ITEM, 100, 1.0, -1.0, NOW}},
                          NOW) == 0 &&
              bad.RecordsDirty() && Take(bad).items.empty() && Take(bad).kinds.empty(),
          "rows of no scope, no kind, no item or a negative value: skipped, the table due a rewrite");

    // What makes a save due: a record fed, not an offer of no kind; one save clears both
    Book d;
    d.LoadHistory({}, NOW, WINDOW);
    d.Add(Auction(1, 100, 1, 10, 20));
    d.AccrueListingHours(NOW + 60, 60, WINDOW);
    Check(!d.SaveDue(), "an accrual over an offer of no kind: nothing to save");
    d.Add(Auction(2, 101, 1, 10, 20), green);
    d.AccrueListingHours(NOW + 120, 60, WINDOW);
    Check(d.SaveDue() && d.RecordsDirty() && !d.HistoryDirty(),
          "an accrual over a kinded offer: the records due a save");
    d.MarkHistorySaved();
    Check(!d.SaveDue(), "saved: nothing due");
    d.CreditListing(102, green, 1.0, NOW + 180, WINDOW);
    Check(d.SaveDue() && d.RecordsDirty(), "a credit: due");
    d.MarkHistorySaved();
    d.CreditListing(102, KindKey{}, 1.0, NOW + 240, WINDOW);
    d.CreditListing(102, green, 0.0, NOW + 240, WINDOW);
    Check(!d.SaveDue(), "a credit without a kind or hours does nothing");
    d.Sale(Bought(Auction(2, 101, 1, 10, 20), 9), NOW + 300);
    d.Remove(2);
    Check(d.SaveDue() && d.RecordsDirty() && d.HistoryDirty(), "a kinded sale: history and records due");

    // A failed load: the session's records still change, nothing is ever due
    Book f;
    f.LoadFailed();
    f.Add(Auction(1, 100, 1, 10, 20), green);
    f.AccrueListingHours(NOW, 3600, WINDOW);
    f.CreditListing(101, green, 2.0, NOW, WINDOW);
    Check(f.RecordsDirty() && !f.SaveDue() && !Take(f).KindRecord(green).Empty(),
          "a failed load: the records fill, no save may overwrite the stored rows");

    // The wa-index case: 48 listing hours by the seam, then one stand-in buyout, to the item and its kind
    Book g;
    g.LoadHistory({}, NOW, WINDOW);
    g.CreditListing(200, green, 48.0, NOW, WINDOW);
    g.Add(Auction(10, 200, 1, 10, 20), green);
    g.Sale(Bought(Auction(10, 200, 1, 10, 20), 9), NOW);
    g.Remove(10);
    Evidence const evidence = Take(g).EvidenceOf(200, green);
    double const start = std::log(2.0) / 24.0;
    double const kindRate = (1.0 + 48.0 * start) / (48.0 + 48.0);
    double const itemRate = (1.0 + 48.0 * kindRate) / (48.0 + 48.0);
    bool closed =
        config.ChanceEvenHours == 24 && config.ChanceKindPriorHours == 48 && config.ChanceItemPriorHours == 48;
    std::cout << "records: seam 48 h plus one sale, chance at 12/24/48 h:";
    for (double const hours : {12.0, 24.0, 48.0})
    {
        double const p = AuctionRules::SaleChance(evidence.item, evidence.kind, hours, config);
        closed = closed && Near(p, 1.0 - std::exp(-itemRate * hours));
        std::cout << " " << std::lround(p * 1000.0);
    }
    std::cout << " permille\n";
    Check(closed && Near(evidence.item.hours, 48.0) && Near(evidence.item.sales, 1.0) &&
              Near(evidence.kind.hours, 48.0) &&
              std::lround(1000.0 * AuctionRules::SaleChance(evidence.item, evidence.kind, 12.0, config)) == 240 &&
              std::lround(1000.0 * AuctionRules::SaleChance(evidence.item, evidence.kind, 24.0, config)) == 422 &&
              std::lround(1000.0 * AuctionRules::SaleChance(evidence.item, evidence.kind, 48.0, config)) == 666,
          "the case's records: 48 h and 1 sale on item and kind, 240 / 422 / 666 permille at 12 / 24 / 48 h");
    Evidence unsold = evidence;
    unsold.item.sales = 0.0;
    unsold.kind.sales = 0.0;
    Check(AuctionRules::SaleChance(unsold.item, unsold.kind, 24.0, config) <
              AuctionRules::SaleChance(evidence.item, evidence.kind, 24.0, config),
          "control: without the sale the chance is lower (caught)");
}

// The least line cost per unit of a need among the buyout lines (excess units worth nothing): a fraction cost/covered
std::pair<std::uint64_t, std::uint64_t> BestPerUnit(std::vector<OfferLine> const& lines, std::uint32_t need)
{
    std::pair<std::uint64_t, std::uint64_t> best{0, 0};
    for (OfferLine const& line : lines)
    {
        std::uint64_t const covered = std::min(std::max<std::uint32_t>(line.count, 1), need);
        if (line.buyout && (!best.second || line.buyout * best.second < best.first * covered))
            best = {line.buyout, covered};
    }
    return best;
}

// The short list keeps a smaller lot that costs a line less (WA4a): six 20-stacks at 15 a unit crowd the cheapest five
// by unit; the 4-stack at 72 and the 10-stack at 250 stay, as no larger lot undercuts them; the dearer 4-stack, the
// sixth 20-stack (the same lot as the first) and the lot without a buyout do not
void CheckLotFrontier()
{
    std::vector<OfferLine> all;
    for (std::uint32_t i = 1; i <= 6; ++i)
        all.push_back(Auction(i, 900, 20, 100, 300));
    all.push_back(Auction(7, 900, 4, 50, 72));
    all.push_back(Auction(8, 900, 4, 50, 80));
    all.push_back(Auction(9, 900, 10, 100, 250));
    all.push_back(Auction(10, 900, 3, 10, 0));
    Book book;
    for (OfferLine const& line : all)
        book.Add(line);
    Snapshot const s = Take(book);
    ItemEntry const* e = Item(s, 900);
    std::vector<std::uint32_t> ids;
    if (e)
        for (OfferLine const& line : e->offers.cheapest)
            ids.push_back(line.id);
    Check(ids == std::vector<std::uint32_t>{1, 2, 3, 4, 5, 7, 9},
          "the cheapest five by unit, then the lots no larger lot undercuts, in unit order");
    std::vector<OfferLine> const listed = e ? e->offers.cheapest : std::vector<OfferLine>{};
    std::vector<OfferLine> const unitFive(listed.begin(), listed.begin() + std::min(listed.size(), OFFER_LINES_MAX));
    bool same = true;
    for (std::uint32_t const need : {1u, 4u, 7u, 10u, 20u})
        same = same && BestPerUnit(listed, need) == BestPerUnit(all, need);
    Check(same, "for needs of 1 to 20 the list's best line cost per unit is the whole book's");
    Check(BestPerUnit(unitFive, 4) != BestPerUnit(all, 4),
          "control: the cheapest five by unit miss the 4-stack a need of 4 buys for 72 (caught)");
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
    CheckSeededStreams();
    CheckRecords();
    CheckRecordStore();
    CheckLotFrontier();
    std::cout << "auction-index: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
