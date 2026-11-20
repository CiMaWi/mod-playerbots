-- #########################################################
-- Playerbots - Per-bot professions (professions plan WR2, BotProfessionMgr, ProfessionRules.h)
-- One row per random bot, made at its login: the professions it held then (origin 1, WR.7), else rolled from guid and
-- version (origin 0); origin 2: set by the GM command on a lab. main1, main2: skill line ids in learning order (the
-- gatherer first), 0 = none; first_aid, fishing, cooking: 1 when the bot has that secondary profession.
-- version: the roll counter (1 = first roll, a reroll adds one). A stored row is kept as it is: later rule changes
-- never reroll it.
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_bot_professions` (
  `guid` int unsigned NOT NULL,
  `version` int unsigned NOT NULL DEFAULT 1,
  `origin` tinyint unsigned NOT NULL DEFAULT 0,
  `main1` smallint unsigned NOT NULL DEFAULT 0,
  `main2` smallint unsigned NOT NULL DEFAULT 0,
  `first_aid` tinyint unsigned NOT NULL DEFAULT 0,
  `fishing` tinyint unsigned NOT NULL DEFAULT 0,
  `cooking` tinyint unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
