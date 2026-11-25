/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_AUCTIONCONFIG_H
#define _PLAYERBOT_AUCTIONCONFIG_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

template <typename Struct>
struct ConfigFamily;

// The numbers of persistent bots' auction house trade (Playerbots.Auction.*), read once at config load
// (PlayerbotAIConfig::Auction) through the rows in AuctionConfig.cpp; the initial values are the defaults. Kept apart
// from PlayerbotAIConfig.h so that later members do not rebuild every user of that header. Std only, so the selftests
// include it as it is.
struct AuctionConfig
{
    // Market.PublishSeconds (10-600): seconds between two snapshots of the house the world thread publishes for the
    // bots' prices
    std::uint32_t PublishSeconds = 60;
    // Market.HistoryDays (1-60): days of sales and expiries an item's price history keeps
    std::uint32_t HistoryDays = 7;
    // Bid.OpenMax (0-20): bids a bot holds at once on auctions it does not lead to buy out
    std::uint32_t OpenBidsMax = 3;
    // Post.UndercutPercent (0-50): percent a bot's ask lies below the cheapest competing buyout of the same item
    std::uint32_t UndercutPercent = 3;
    // Post.RelistStepPercent (1-50): percent an expiry lowers the next ask (down to the floor) and a quick sale raises
    // it
    std::uint32_t RelistStepPercent = 10;
    // Post.RelistLimit (0-20): times an item that expired is listed again before the bot gives it to a vendor
    std::uint32_t RelistLimit = 3;
    // Post.FirstDurationHours, Post.RelistDurationHours (12-48): hours of a first listing and of a listing after an
    // expiry; the core takes 12, 24 or 48, the poster uses the longest of them not above the value
    std::uint32_t FirstDurationHours = 24;
    std::uint32_t RelistDurationHours = 48;
};

extern ConfigFamily<AuctionConfig> const AuctionFamily;

// The one instance (AuctionConfig.cpp), filled by the config walk. Only PlayerbotAIConfig's constructor calls this:
// readers use sPlayerbotAIConfig.Auction.
AuctionConfig& AuctionConfigStorage();

// The auction plan's words (PLAN 3.10), fixed here once; .claude/docs/world-log.md lists them
namespace AuctionWords
{
// Event types: AuctionPost, AuctionSale, AuctionExpire, AuctionBid, AuctionBuy, AuctionOutbid, AuctionWon and one
// AuctionVisit per visit (EventTypes.h). Gold and items move through GoldIn/GoldOut and ItemIn/ItemOut with the
// source `auction`

// The `auction` decision row's choices and reasons (WorldLogFormat::DecisionKind::Auction); amounts in copper
enum class Choice : std::uint8_t
{
    Post,
    Hold,
    Relist,
    Buy,
    Bid,
    Skip
};
constexpr std::size_t CHOICE_COUNT = 6;
constexpr std::array<std::string_view, CHOICE_COUNT> CHOICE_WORDS = {"post", "hold", "relist", "buy", "bid", "skip"};

enum class Reason : std::uint8_t
{
    AboveVendor,
    BelowFloor,
    Expired,
    NoOffer,
    OverCeiling,
    Budget,
    BidCap,
    DetourTooLong
};
constexpr std::size_t REASON_COUNT = 8;
constexpr std::array<std::string_view, REASON_COUNT> REASON_WORDS = {
    "above_vendor", "below_floor", "expired", "no_offer", "over_ceiling", "budget", "bid_cap", "detour_too_long"};

constexpr std::string_view ChoiceWord(Choice choice)
{
    std::size_t const index = static_cast<std::size_t>(choice);
    return index < CHOICE_COUNT ? CHOICE_WORDS[index] : std::string_view("other");
}

constexpr std::string_view ReasonWord(Reason reason)
{
    std::size_t const index = static_cast<std::size_t>(reason);
    return index < REASON_COUNT ? REASON_WORDS[index] : std::string_view("other");
}

static_assert(static_cast<std::size_t>(Choice::Skip) + 1 == CHOICE_COUNT, "a word for every auction choice");
static_assert(static_cast<std::size_t>(Reason::DetourTooLong) + 1 == REASON_COUNT, "a word for every auction reason");
}  // namespace AuctionWords

#endif
