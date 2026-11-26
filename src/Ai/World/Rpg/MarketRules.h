/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_MARKETRULES_H
#define _PLAYERBOT_MARKETRULES_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>

#include "AuctionConfig.h"
#include "AuctionIndexRules.h"
#include "AuctionPricing.h"

// The market a decision away from the auctioneer sees (auction house plan WA7a, PLAN 3.7, WA.4): per item the index's
// typical price, which a bot knows anywhere, never the current offers, which only the auction visit reads. A path,
// gathering or second-profession computation takes one view when its inputs are built (the published snapshot's
// shared pointer, read on any thread) and asks it per item. No view, or an item without a typical price, is no market:
// the decision is then the vendor-only one (the reference value is not a price anyone paid and is not read here).
// Pure and std only: the selftests build views from their own snapshots.
namespace Market
{
constexpr std::uint32_t MAX_LOT = 20;  // [E] the units a bot lists at once at most: a common material stack

// What one unit is expected to fetch when the bot lists a lot of `lot` units at the typical price (PLAN 3.4, WA.17):
// the ask's expected value, the cut and the deposit taken off and the vendor's price after an expiry counted, over the
// units (AuctionRules::StockWorth on the evidence alone: the sale chance from the item's and its kind's records, no
// current offers, no expiries of the bot's own). 0 without a typical price, or when such a listing is not worth more
// than the vendor: the vendor is then the outlet, so a caller takes the larger of this and the vendor's price
inline double ProceedsEach(AuctionIndex::TypicalPrice const& typical, AuctionIndex::Record const& itemRecord,
                           AuctionIndex::Record const& kindRecord, std::uint32_t sellPrice, std::uint32_t lot,
                           AuctionRules::Fees const& fees, AuctionConfig const& config)
{
    if (!typical.unitPrice)
        return 0.0;
    AuctionRules::AskInput in;
    in.count = std::max<std::uint32_t>(lot, 1);
    in.sellPrice = sellPrice;
    in.vendorPayout = static_cast<std::uint64_t>(sellPrice) * in.count;
    in.typical = typical;
    in.itemRecord = itemRecord;
    in.kindRecord = kindRecord;
    in.fees = fees;
    return static_cast<double>(AuctionRules::StockWorth(in, config)) / in.count;
}

struct View
{
    std::shared_ptr<AuctionIndex::Snapshot const> snapshot;  // null: no market
    AuctionRules::Fees fees;  // the house a bot would sell at: the shared house's row (7, WA.9)
    AuctionConfig config;     // the poster's durations and steps
    std::function<std::uint32_t(std::uint32_t)> lotOf;  // the units of an item listed at once (its stack); empty: 1

    [[nodiscard]] bool Empty() const { return !snapshot || snapshot->items.empty(); }

    [[nodiscard]] AuctionIndex::TypicalPrice Typical(std::uint32_t item) const
    {
        AuctionIndex::ItemEntry const* entry = snapshot ? snapshot->Find(item) : nullptr;
        return entry ? entry->typical : AuctionIndex::TypicalPrice{};
    }

    // The material rules' auction source (Materials::Bot::auctionCopper): per unit, the typical price a buyer pays;
    // 0 for an item without one; empty without a market
    [[nodiscard]] std::function<std::uint32_t(std::uint32_t)> BuyCopperOf() const
    {
        if (Empty())
            return {};
        return [prices = snapshot](std::uint32_t item)
        {
            AuctionIndex::ItemEntry const* entry = prices->Find(item);
            return entry ? entry->typical.unitPrice : 0u;
        };
    }

    // The sale side (PLAN 3.7): what one unit of an item is expected to fetch listed (ProceedsEach) in a lot of its
    // stack, at most MAX_LOT; 0 without a market or a typical price, or when the vendor pays as much. Yields carry it
    // as Gather::Yield::otherCopper, the disenchant outlet as its other buyer's price
    [[nodiscard]] double ProceedsOf(std::uint32_t item, std::uint32_t sellPrice) const
    {
        AuctionIndex::ItemEntry const* entry = Empty() ? nullptr : snapshot->Find(item);
        if (!entry)
            return 0.0;
        std::uint32_t const lot = lotOf ? std::clamp<std::uint32_t>(lotOf(item), 1, MAX_LOT) : 1;
        return ProceedsEach(entry->typical, entry->record, snapshot->KindRecord(entry->kind), sellPrice, lot, fees,
                            config);
    }

    // What one unit fetches at best: the vendor's price or a listing's proceeds (the skill path's income term)
    [[nodiscard]] double SaleOf(std::uint32_t item, double vendorCopper) const
    {
        if (Empty())
            return vendorCopper;
        return std::max(vendorCopper, ProceedsOf(item, static_cast<std::uint32_t>(std::max(vendorCopper, 0.0))));
    }
};
}  // namespace Market

#endif
