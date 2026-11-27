-- The auction price index's trade history (auction-house plan WA1d): the recent sales and expiries of each item,
-- one row per record, so a restart keeps what the market has learned. Written by the world thread's asynchronous
-- full rewrite every Playerbots.Auction.Market.SaveSeconds when changed and at shutdown, read once at startup.
-- Time is the server's game time in seconds; UnitPrice is copper per unit, 0 for an expiry.
CREATE TABLE IF NOT EXISTS `playerbots_auction_history` (
  `Item` INT UNSIGNED NOT NULL,
  `Kind` TINYINT UNSIGNED NOT NULL COMMENT '0 sale, 1 expiry',
  `Time` BIGINT NOT NULL,
  `UnitPrice` INT UNSIGNED NOT NULL DEFAULT 0,
  `Count` INT UNSIGNED NOT NULL DEFAULT 1,
  `AuctionId` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`Item`, `Kind`, `Time`, `AuctionId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
