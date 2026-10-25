-- #########################################################
-- Playerbots - PvP gear (Playerbots.PvpGear.Enable)
-- playerbots_pvp_loadout: the stored PvP loadout of every ladder bot, one item entry per equipment slot. Plain rows
-- without item GUIDs, so they copy between servers whose item counters have diverged; a season start
-- (.playerbots pvpgear reset) sets every loadout to the base tier. Loaded at startup, written asynchronously.
-- playerbots_pvp_worn: per server and never copied: which real item is worn for the loadout in a slot and where the PvE
-- item went (for the swap back and the repair after a crash). Created here, written once bots wear their PvP gear.
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_pvp_loadout` (
  `guid` int unsigned NOT NULL,
  `slot` tinyint unsigned NOT NULL,
  `item_entry` int unsigned NOT NULL,
  `tier` tinyint unsigned NOT NULL DEFAULT 0,
  `season` int unsigned NOT NULL DEFAULT 0,
  `bought_at` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `playerbots_pvp_worn` (
  `guid` int unsigned NOT NULL,
  `slot` tinyint unsigned NOT NULL,
  `pvp_item_guid` int unsigned NOT NULL DEFAULT 0,
  `pve_item_guid` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
