-- #########################################################
-- Playerbots - Quest failure record (questing WT35c, QuestFailRecord.cpp, QuestFailBook.h)
-- One row per persistent bot and quest it gave up: how often, the last cause word (timeout, no_place, no_target,
-- no_ender, unsupported:<kind>), the minutes pursued over every give-up and the last give-up's time (unix seconds).
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_bot_quest_fail` (
  `guid` int unsigned NOT NULL,
  `quest_id` int unsigned NOT NULL,
  `give_ups` int unsigned NOT NULL DEFAULT 0,
  `last_cause` varchar(32) NOT NULL DEFAULT '',
  `minutes` int unsigned NOT NULL DEFAULT 0,
  `last_time` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `quest_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
