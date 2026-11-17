-- #########################################################
-- Playerbots - Vessel crossings (navigation plan NV31, PLAN 3.8 5): the table only; its rows come from the graph
-- build (2026_11_18_06_playerbots_travelnode_crossing_merged-r5.sql, DELETE FROM + INSERT). Nothing reads it until
-- the travel plan's WT70. Text: the hub's measurements/nav/nv31/crossing-schema.sql.
-- One row per vessel (entry), spawn (guid), stop, direction and rank. No node id: the stop node lies at the pose.
-- guid: the spawn (gameobject guid) of a type 11 object, lifts and tram cars, whose one template stands at several
-- places; 0 for a type 15 object (one per entry). Stops are numbered per spawn in path order.
-- direction 0 boarding, 1 leaving; seq 0 = nearest the wait point; kind 0 walk, 1 jump; dock = world point on the
-- world's mesh, deck = transport offset on the vessel's mesh; gap, rise in yd (deck z minus dock z).
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_travelnode_crossing` (
    `entry` INT UNSIGNED NOT NULL,
    `guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `display_id` INT UNSIGNED NOT NULL,
    `stop` INT UNSIGNED NOT NULL,
    `map_id` INT UNSIGNED NOT NULL,
    `pose_x` FLOAT NOT NULL,
    `pose_y` FLOAT NOT NULL,
    `pose_z` FLOAT NOT NULL,
    `pose_o` FLOAT NOT NULL,
    `direction` TINYINT UNSIGNED NOT NULL,
    `seq` TINYINT UNSIGNED NOT NULL,
    `kind` TINYINT UNSIGNED NOT NULL,
    `dock_x` FLOAT NOT NULL,
    `dock_y` FLOAT NOT NULL,
    `dock_z` FLOAT NOT NULL,
    `deck_x` FLOAT NOT NULL,
    `deck_y` FLOAT NOT NULL,
    `deck_z` FLOAT NOT NULL,
    `gap` FLOAT NOT NULL,
    `rise` FLOAT NOT NULL,
    PRIMARY KEY (`entry`, `guid`, `stop`, `direction`, `seq`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;
