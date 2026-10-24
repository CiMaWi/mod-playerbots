-- #########################################################
-- Playerbots - Acquaintances between real players and random bots (Playerbots.Acquaintance.Enable)
-- One row per (real player, random bot) pair that met: score, which kinds of meeting happened (flags) and the raw
-- tallies the score is computed from, so the weights can be retuned without losing history. Times are unix seconds.
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_acquaintance` (
  `player_guid` int unsigned NOT NULL,
  `bot_guid` int unsigned NOT NULL,
  `first_met` int unsigned NOT NULL DEFAULT 0,
  `last_met` int unsigned NOT NULL DEFAULT 0,
  `score` int unsigned NOT NULL DEFAULT 0,
  `flags` int unsigned NOT NULL DEFAULT 0,
  `group_seconds` int unsigned NOT NULL DEFAULT 0,
  `guild_seconds` int unsigned NOT NULL DEFAULT 0,
  `nearby_seconds` int unsigned NOT NULL DEFAULT 0,
  `direct_events` int unsigned NOT NULL DEFAULT 0,
  `event_points` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`player_guid`, `bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
