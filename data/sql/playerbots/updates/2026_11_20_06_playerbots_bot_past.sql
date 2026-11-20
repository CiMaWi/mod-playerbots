-- #########################################################
-- Playerbots - A bot's past (living-conversation LC20a, BotPastMgr.cpp, PastRules.h)
-- playerbots_bot_background: the statements of a bot's life before its arrival, by ordinal (1 to 10): the text, the
-- version of the writer's rules it passed, a shared id for a statement several bots hold (0: the bot's own) and its
-- source (0 the writer, 1 a talk).
-- playerbots_bot_colour: one short line per participant of a major record row (playerbots_bot_record.event_id): the
-- text and the version of the rules it passed.
-- A deleted bot's rows go with BotStores::Delete (BotStores.h).
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_bot_background` (
  `guid` int unsigned NOT NULL,
  `ordinal` tinyint unsigned NOT NULL,
  `text` varchar(240) NOT NULL DEFAULT '',
  `version` tinyint unsigned NOT NULL DEFAULT 0,
  `shared_id` int unsigned NOT NULL DEFAULT 0,
  `source` tinyint unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `ordinal`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `playerbots_bot_colour` (
  `event_id` int unsigned NOT NULL,
  `guid` int unsigned NOT NULL,
  `text` varchar(160) NOT NULL DEFAULT '',
  `version` tinyint unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`event_id`, `guid`),
  KEY `idx_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
