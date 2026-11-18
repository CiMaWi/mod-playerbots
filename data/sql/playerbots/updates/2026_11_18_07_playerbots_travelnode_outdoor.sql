-- The node table's outdoor column of graph-merged-r6-20261118: built by tools/nav/graph-build.js (recipe merged-r6; sources in precedence order: regenerated, loaded); built, never hand-edited.
-- outdoor: the engine's answer at the node (1 outdoors, 0 indoors, NULL no answer), written by the graph build (NV32, G91).
SET @have := (SELECT COUNT(*) FROM `information_schema`.`COLUMNS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'playerbots_travelnode' AND `COLUMN_NAME` = 'outdoor');
SET @ddl := IF(@have = 0, 'ALTER TABLE `playerbots_travelnode` ADD COLUMN `outdoor` TINYINT(1) DEFAULT NULL AFTER `linked`', 'DO 0');
PREPARE `stmt` FROM @ddl;
EXECUTE `stmt`;
DEALLOCATE PREPARE `stmt`;
