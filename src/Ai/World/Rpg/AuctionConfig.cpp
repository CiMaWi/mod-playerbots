/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionConfig.h"

#include <array>

#include "ConfigTable.h"

namespace
{
using Row = ConfigRow<AuctionConfig>;

constexpr std::array<Row, 9> AuctionRows = {{
    {"Market.PublishSeconds", &AuctionConfig::PublishSeconds, 10, 600},
    {"Market.SaveSeconds", &AuctionConfig::SaveSeconds, 60, 3600},
    {"Market.HistoryDays", &AuctionConfig::HistoryDays, 1, 60},
    {"Bid.OpenMax", &AuctionConfig::OpenBidsMax, 0, 20},
    {"Post.UndercutPercent", &AuctionConfig::UndercutPercent, 0, 50},
    {"Post.RelistStepPercent", &AuctionConfig::RelistStepPercent, 1, 50},
    {"Post.RelistLimit", &AuctionConfig::RelistLimit, 0, 20},
    {"Post.FirstDurationHours", &AuctionConfig::FirstDurationHours, 12, 48},
    {"Post.RelistDurationHours", &AuctionConfig::RelistDurationHours, 12, 48},
}};
}  // namespace

ConfigFamily<AuctionConfig> const AuctionFamily{"Playerbots.Auction.", AuctionRows};

AuctionConfig& AuctionConfigStorage()
{
    static AuctionConfig values;
    return values;
}
