-- Known danger (awareness): hostile creatures per cell of 25 yd, side and height layer of the open-world maps.
-- The rows are generated (the hub's tools/awareness/danger-build.js, *_playerbots_known_danger_rows.sql) and
-- applied apart; the server refuses to start while the table is empty
CREATE TABLE IF NOT EXISTS `playerbots_known_danger` (
  `map` SMALLINT UNSIGNED NOT NULL,
  `cx` SMALLINT NOT NULL,
  `cy` SMALLINT NOT NULL,
  `side` TINYINT UNSIGNED NOT NULL,
  `layer` TINYINT UNSIGNED NOT NULL,
  `z_lo` SMALLINT NOT NULL,
  `z_hi` SMALLINT NOT NULL,
  `lvl_n` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `lvl_e` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `lvl_r` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `lvl_b` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `cnt_n` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `cnt_e` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `cnt_r` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `cnt_b` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `guards` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`map`, `cx`, `cy`, `side`, `layer`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_known_danger_build` (
  `build` VARCHAR(32) NOT NULL,
  `cell_yards` SMALLINT UNSIGNED NOT NULL,
  `height_reach_yards` SMALLINT UNSIGNED NOT NULL,
  `row_count` INT UNSIGNED NOT NULL,
  `max_layers` TINYINT UNSIGNED NOT NULL,
  `world_fingerprint` CHAR(64) NOT NULL,
  `built_at` DATETIME NOT NULL,
  PRIMARY KEY (`build`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
