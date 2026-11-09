-- #########################################################
-- Playerbots - Per-bot personality (travel plan WT11a, BotTraitMgr)
-- One row per bot, rolled at its first login from guid and version (TraitRules.h): 30 traits, -100..100, named as
-- the GM command names them (faction zeal is `zeal`), and one to three interests (Interest + 1, 0 = none).
-- version: the roll counter (1 = first roll, a reroll adds one). A stored row is kept as it is: later rule changes
-- never reroll it.
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_bot_traits` (
  `guid` int unsigned NOT NULL,
  `version` int unsigned NOT NULL DEFAULT 1,
  `interest1` tinyint unsigned NOT NULL DEFAULT 0,
  `interest2` tinyint unsigned NOT NULL DEFAULT 0,
  `interest3` tinyint unsigned NOT NULL DEFAULT 0,
  `drive` tinyint NOT NULL DEFAULT 0,
  `curiosity` tinyint NOT NULL DEFAULT 0,
  `caution` tinyint NOT NULL DEFAULT 0,
  `aggression` tinyint NOT NULL DEFAULT 0,
  `sociability` tinyint NOT NULL DEFAULT 0,
  `helpfulness` tinyint NOT NULL DEFAULT 0,
  `leadership` tinyint NOT NULL DEFAULT 0,
  `loyalty` tinyint NOT NULL DEFAULT 0,
  `generosity` tinyint NOT NULL DEFAULT 0,
  `thrift` tinyint NOT NULL DEFAULT 0,
  `greed` tinyint NOT NULL DEFAULT 0,
  `orderliness` tinyint NOT NULL DEFAULT 0,
  `thoroughness` tinyint NOT NULL DEFAULT 0,
  `competitiveness` tinyint NOT NULL DEFAULT 0,
  `patience` tinyint NOT NULL DEFAULT 0,
  `courtesy` tinyint NOT NULL DEFAULT 0,
  `rootedness` tinyint NOT NULL DEFAULT 0,
  `talkativeness` tinyint NOT NULL DEFAULT 0,
  `warmth` tinyint NOT NULL DEFAULT 0,
  `humour` tinyint NOT NULL DEFAULT 0,
  `register` tinyint NOT NULL DEFAULT 0,
  `confidence` tinyint NOT NULL DEFAULT 0,
  `outlook` tinyint NOT NULL DEFAULT 0,
  `piety` tinyint NOT NULL DEFAULT 0,
  `zeal` tinyint NOT NULL DEFAULT 0,
  `lore` tinyint NOT NULL DEFAULT 0,
  `suspicion` tinyint NOT NULL DEFAULT 0,
  `theatricality` tinyint NOT NULL DEFAULT 0,
  `mischief` tinyint NOT NULL DEFAULT 0,
  `worldliness` tinyint NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
