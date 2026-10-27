-- #########################################################
-- Playerbots - Fixed skill percentile per bot (Playerbots.Skill.Distribution)
-- percentile: the bot's rank in (0, 1); the skill is this percentile mapped through the configured distribution.
-- NULL until the server migrates the row once (the first roll's percentile, or the stored skill's under normal 50/15).
-- #########################################################

ALTER TABLE `playerbots_bot_profile` ADD COLUMN `percentile` DOUBLE NULL DEFAULT NULL AFTER `version`;
