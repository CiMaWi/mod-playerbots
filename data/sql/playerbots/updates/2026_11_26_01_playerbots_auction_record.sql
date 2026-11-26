-- The auction price index's sale records (auction-house-2 plan AH6): each item's and each kind's lots sold and lot
-- hours listed, weighed by recency to Time, so a restart keeps what the house learned of how fast things sell.
-- Written with the trade history by the world thread's asynchronous full rewrite every
-- Playerbots.Auction.Market.SaveSeconds when changed and at shutdown, read once at startup. Scope 0: Id is an item
-- entry; 1: Id is a kind code (class, quality and level band, above 2^24). Time is the server's game time in seconds.
CREATE TABLE IF NOT EXISTS `playerbots_auction_record` (
  `Scope` TINYINT UNSIGNED NOT NULL COMMENT '0 item, 1 kind',
  `Id` INT UNSIGNED NOT NULL,
  `Sales` DOUBLE NOT NULL DEFAULT 0,
  `Hours` DOUBLE NOT NULL DEFAULT 0,
  `Time` BIGINT NOT NULL,
  PRIMARY KEY (`Scope`, `Id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
