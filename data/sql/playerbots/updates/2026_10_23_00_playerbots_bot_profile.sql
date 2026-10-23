-- #########################################################
-- Playerbots - Hidden per-bot profile (Playerbots.Skill.Enable)
-- pvp_skill: 1-99, rolled at the bot's first login
-- version: the roll counter (1 = first roll, a reroll adds one)
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_bot_profile` (
  `guid` int unsigned NOT NULL,
  `pvp_skill` tinyint unsigned NOT NULL,
  `created` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `version` int unsigned NOT NULL DEFAULT 1,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
