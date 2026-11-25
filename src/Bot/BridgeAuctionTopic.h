/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEAUCTIONTOPIC_H
#define PLAYERBOTS_BRIDGEAUCTIONTOPIC_H

// WA9: the `auction` topic (BridgeBotState.h), Player-free. BridgeBotState.cpp reads, on the world thread, what the bot
// has listed (a scan of the core's auctions by owner), its buy list with the ceilings (Planner::BuyListOf) and its open
// bids into the plain input below, and this header words it. The prices are the bot's own: its listings' prices and
// its ceilings; a current offer of a house is never named (WA.4: only at the auctioneer). Money comes ready worded
// (UpkeepNeeds::Money, as the needs topic words it) and rides in the text and in extra as copper; no other digit.
// Only BridgeBotState.cpp and the selftest include this header. tests/bridge-bot-topics-selftest.cpp checks it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "BridgeBotState.h"
#include "BridgeTexts.h"

namespace PlayerbotBridge::Topics
{
// The row kinds (BotStateItem::code) of the auction topic
enum class AuctionRow : std::uint8_t
{
    Provided,  // a bot the rules do not govern: it trades nothing of its own (the only entry)
    Nothing,   // nothing listed, nothing looked for, no bid open
    Listed,    // name = the item, value = its count, extra = the price in copper (the buyout, else the start bid)
    Wants,     // name = the item, value = units wanted, extra = the most it would pay per unit, in copper
    Bids       // value = the bids open, extra = the most it holds at once
};

inline char const* RowWord(AuctionRow row)
{
    namespace L = BridgeTexts::Lookups;
    switch (row)
    {
        case AuctionRow::Provided:
            return L::ROW_AUCTION_PROVIDED;
        case AuctionRow::Nothing:
            return L::ROW_AUCTION_NOTHING;
        case AuctionRow::Listed:
            return L::ROW_AUCTION_LISTED;
        case AuctionRow::Wants:
            return L::ROW_AUCTION_WANTS;
        case AuctionRow::Bids:
            return L::ROW_AUCTION_BIDS;
    }
    return "";
}

// The shares of the topic row's limit: a part never starves another
constexpr std::size_t AUCTION_LISTED_SHOWN = 4;
constexpr std::size_t AUCTION_WANTS_SHOWN = 3;
constexpr std::size_t AUCTION_BIDS_SHOWN = 1;
constexpr std::size_t AUCTION_SHOWN = AUCTION_LISTED_SHOWN + AUCTION_WANTS_SHOWN + AUCTION_BIDS_SHOWN;

struct AuctionListing
{
    std::string name;
    std::uint32_t count{1};
    std::uint64_t copper{0};  // the buyout, else the start bid
    std::string money;        // the copper, worded
    bool buyout{true};
};

struct AuctionWant
{
    std::string name;
    std::uint32_t units{0};
    std::uint64_t ceiling{0};  // copper per unit
    std::string money;         // the ceiling, worded
};

struct AuctionInput
{
    bool governed{false};  // a persistent bot (BotKind::KeepsPossessions)
    std::vector<AuctionListing> listings;
    std::vector<AuctionWant> wants;
    std::uint32_t openBids{0};
    std::uint32_t bidsMax{0};  // the most it holds at once
};

inline BotStateItem AuctionLineItem(AuctionRow row, std::string name, std::string text, std::uint64_t value = 0,
                                    std::uint64_t extra = 0)
{
    auto const clamp = [](std::uint64_t number)
    { return static_cast<std::uint32_t>(std::min<std::uint64_t>(number, UINT32_MAX)); };
    return {std::move(name), clamp(value), clamp(extra), static_cast<std::uint8_t>(row), std::move(text), RowWord(row)};
}

// Provided; else a row per listing, per want and one for the open bids, each part cut at its share; none of them: one
// row saying so
inline void AuctionItems(AuctionInput const& in, std::vector<BotStateItem>& items)
{
    namespace L = BridgeTexts::Lookups;
    items.clear();
    if (!in.governed)
    {
        items.push_back(AuctionLineItem(AuctionRow::Provided, L::ROW_AUCTION_PROVIDED, L::AUCTION_PROVIDED));
        return;
    }
    std::size_t shown = 0;
    for (AuctionListing const& listing : in.listings)
    {
        if (shown++ >= AUCTION_LISTED_SHOWN)
            break;
        std::string text = std::string(L::AUCTION_LISTED_LEAD) + (listing.count > 1 ? L::AUCTION_STACK_OF : "") +
                           listing.name + (listing.buyout ? L::AUCTION_AT_BUYOUT : L::AUCTION_AT_BID) + listing.money +
                           L::LOOKUP_END;
        items.push_back(
            AuctionLineItem(AuctionRow::Listed, listing.name, std::move(text), listing.count, listing.copper));
    }
    shown = 0;
    for (AuctionWant const& want : in.wants)
    {
        if (shown++ >= AUCTION_WANTS_SHOWN)
            break;
        std::string text = std::string(L::AUCTION_WANTS_LEAD) + want.name + L::AUCTION_WANTS_PAY + want.money +
                           L::AUCTION_WANTS_EACH + L::LOOKUP_END;
        items.push_back(AuctionLineItem(AuctionRow::Wants, want.name, std::move(text), want.units, want.ceiling));
    }
    if (in.openBids)
    {
        std::string text = L::AUCTION_BIDS_OPEN;
        if (in.bidsMax && in.openBids >= in.bidsMax)
            text += L::AUCTION_BIDS_FULL;
        items.push_back(
            AuctionLineItem(AuctionRow::Bids, L::ROW_AUCTION_BIDS, std::move(text), in.openBids, in.bidsMax));
    }
    if (items.empty())
        items.push_back(AuctionLineItem(AuctionRow::Nothing, L::ROW_AUCTION_NOTHING, L::AUCTION_NOTHING));
}
}  // namespace PlayerbotBridge::Topics

#endif
