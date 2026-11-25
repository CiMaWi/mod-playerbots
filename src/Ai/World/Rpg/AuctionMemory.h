/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONMEMORY_H
#define _PLAYERBOT_AUCTIONMEMORY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// A seller's memory of its own listings (auction house WA3b): per item, the expiries since its last sale and whether
// that sale came quickly (AuctionRules::AskInput's expiredSteps and quickSale), and its open listings' times. Fed by
// the core's auction hooks on the world thread (AuctionVisit.cpp keeps one per bot); in memory only, so a restart
// forgets it and a relist starts again at step 0. Std only, so the selftests include it as it is.
namespace AuctionMemory
{
struct ItemMemory
{
    std::uint32_t entry{0};
    std::uint32_t expiredSteps{0};
    bool quickSale{false};
};

struct Listing
{
    std::uint32_t auctionId{0};
    std::int64_t listedAt{0};  // game time, seconds
    std::int64_t endsAt{0};
};

// A sale is quick when it came within this share of its listing's time (one quarter)
constexpr std::int64_t QUICK_SALE_DIVISOR = 4;
// The items a seller remembers at most; the oldest goes first (a bounded memory over a long run)
constexpr std::size_t ITEMS_MAX = 64;

struct SellerMemory
{
    std::vector<ItemMemory> items;  // items with an expiry or a quick sale to remember
    std::vector<Listing> listings;  // the seller's open listings

    ItemMemory Of(std::uint32_t entry) const
    {
        auto found =
            std::find_if(items.begin(), items.end(), [entry](ItemMemory const& item) { return item.entry == entry; });
        return found != items.end() ? *found : ItemMemory{entry};
    }

    void Listed(std::uint32_t auctionId, std::int64_t now, std::int64_t endsAt)
    {
        listings.push_back({auctionId, now, endsAt});
    }

    // A listing ended unsold: one step more, and its quick sale is forgotten
    void Expired(std::uint32_t auctionId, std::uint32_t entry)
    {
        Drop(auctionId);
        ItemMemory& item = Item(entry);
        ++item.expiredSteps;
        item.quickSale = false;
    }

    // The visit held the stack (its ask under the floor, or no deposit): the market had no room for it, a step as an
    // expiry is, so an item held again and again leaves the stock past the relist limit instead of drawing a trip
    // to the auctioneer at every plan
    void Held(std::uint32_t entry)
    {
        ItemMemory& item = Item(entry);
        ++item.expiredSteps;
        item.quickSale = false;
    }

    // A listing sold: the steps start again, quick when it sold within its time's first share
    void Sold(std::uint32_t auctionId, std::uint32_t entry, std::int64_t now)
    {
        auto listing = std::find_if(listings.begin(), listings.end(),
                                    [auctionId](Listing const& open) { return open.auctionId == auctionId; });
        bool const quick = listing != listings.end() && listing->endsAt > listing->listedAt &&
                           (now - listing->listedAt) * QUICK_SALE_DIVISOR < listing->endsAt - listing->listedAt;
        Drop(auctionId);
        items.erase(
            std::remove_if(items.begin(), items.end(), [entry](ItemMemory const& item) { return item.entry == entry; }),
            items.end());
        if (quick)
            items.push_back({entry, 0, true});
    }

    // Cancelled, or the end after a sale or an expiry
    void Drop(std::uint32_t auctionId)
    {
        listings.erase(std::remove_if(listings.begin(), listings.end(),
                                      [auctionId](Listing const& open) { return open.auctionId == auctionId; }),
                       listings.end());
    }

    bool Empty() const { return items.empty() && listings.empty(); }

private:
    ItemMemory& Item(std::uint32_t entry)
    {
        auto found =
            std::find_if(items.begin(), items.end(), [entry](ItemMemory const& item) { return item.entry == entry; });
        if (found != items.end())
            return *found;
        if (items.size() >= ITEMS_MAX)
            items.erase(items.begin());
        items.push_back({entry});
        return items.back();
    }
};
}  // namespace AuctionMemory

#endif
