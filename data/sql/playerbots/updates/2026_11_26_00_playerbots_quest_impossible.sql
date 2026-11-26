-- The quests known to be impossible (questing WQ38, WQ.48): one row per quest with its cause word and a free note
-- (for a fault of ours, its owner). Read once at startup with the quest index (a change needs a restart): a listed
-- quest gives the planner no need and no take, `planner quest <id>` prints its cause, and the readings leave it out.
-- Cause words, by WQ.48's classes: far_trip (needs a far trip), group (needs a group or is elite), dungeon (lies in a
-- dungeon), lone_objective (its objective lies alone), under_water, ours (a fault of ours), game_data (a fault of the
-- game data). Elite, group and dungeon quests are told by their flags and need no row. The rows come with the
-- questing coverage work; this file creates the table empty.
CREATE TABLE IF NOT EXISTS `playerbots_quest_impossible` (
  `Quest` INT UNSIGNED NOT NULL,
  `Cause` VARCHAR(32) NOT NULL,
  `Note` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`Quest`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
