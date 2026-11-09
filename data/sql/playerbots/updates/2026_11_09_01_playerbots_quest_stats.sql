-- #########################################################
-- Playerbots - Population quest statistics (travel plan WT13a, PlannerQuests.cpp, QuestStats.h)
-- One row per quest and level band (the bot's level at accept / 5), recorded from the persistent bots' quest events,
-- kept in memory and written back as totals every few minutes. minutes: the accept-to-turn-in histogram, 16 bucket
-- counts separated by commas (bucket tops 1, 2, 3, 5, 7, 10, 15, 20, 30, 45, 60, 90, 120, 150, 180 and 240 minutes).
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_quest_stats` (
  `quest_id` int unsigned NOT NULL,
  `band` tinyint unsigned NOT NULL,
  `accepted` int unsigned NOT NULL DEFAULT 0,
  `completed` int unsigned NOT NULL DEFAULT 0,
  `abandoned` int unsigned NOT NULL DEFAULT 0,
  `deaths` int unsigned NOT NULL DEFAULT 0,
  `minutes` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`quest_id`, `band`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
