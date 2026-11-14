-- #########################################################
-- Playerbots - Bot record (living-conversation LC6, BotRecordMgr.cpp, RecordRules.h)
-- One row per bot and major event of its real play, scored when it happens. A shared event (a group's kill, a fight)
-- has one event_id for every bot in it. kind: RecordRules::Kind's index; time and expires: unix seconds, expires 0 =
-- kept for good. others: tokens b<guid> (bot), p<guid> (player), n<entry> (creature); facts: key=value; pairs.
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_bot_record` (
  `guid` int unsigned NOT NULL,
  `event_id` int unsigned NOT NULL,
  `kind` tinyint unsigned NOT NULL,
  `time` int unsigned NOT NULL,
  `map` smallint unsigned NOT NULL DEFAULT 0,
  `zone` int unsigned NOT NULL DEFAULT 0,
  `area` int unsigned NOT NULL DEFAULT 0,
  `score` tinyint unsigned NOT NULL,
  `others` varchar(255) NOT NULL DEFAULT '',
  `facts` varchar(255) NOT NULL DEFAULT '',
  `expires` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `event_id`, `kind`),
  KEY `idx_guid_expires` (`guid`, `expires`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
