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
}  // namespace

int main()
{
    CheckRounding();
    CheckLifecycle();
    CheckBidAndVanish();
    CheckMedian();
    CheckDeterminism();
    std::cout << "auction-index: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
