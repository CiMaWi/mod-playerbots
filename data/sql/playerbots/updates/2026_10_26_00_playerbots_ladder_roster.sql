-- #########################################################
-- Playerbots - Ladder roster (Playerbots.Ladder.Role)
-- One row per random bot of an arena team that moved between servers: state 0 = hosted on this server, 1 = away (on
-- another server). Written only by .playerbots ladder depart and arrive, loaded at startup. Each server (main and
-- every arena instance) has its own playerbots DB and so its own roster. Main has rows only for away bots (no row =
-- hosted); an arena instance hosts exactly its rows with state 0.
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_ladder_roster` (
  `guid` int unsigned NOT NULL,
  `team_id` int unsigned NOT NULL,
  `state` tinyint unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`),
  KEY `idx_team_id` (`team_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
