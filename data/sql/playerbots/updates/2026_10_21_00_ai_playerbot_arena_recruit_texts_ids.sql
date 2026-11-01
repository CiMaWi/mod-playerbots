-- #########################################################
-- Playerbots - move the arena team recruiting texts off ids 1929-1931
-- Upstream's 2026_10_22_00 (calc_item_breakdown, id 1929) and
-- 2026_10_27_00 (class_role_*, ids 1930-1944) insert ids that this
-- fork's 2026_10_25_01 had used. This file sorts before both, so a
-- database that already holds the recruiting texts frees the ids
-- first; 2026_10_25_01 (changed, so reapplied) inserts them again at
-- 30001-30003. A no-op on a fresh database.
-- #########################################################

DELETE FROM ai_playerbot_texts
WHERE id IN (1929, 1930, 1931)
  AND name IN ('arena_team_recruit_accept', 'arena_team_recruit_refuse_level', 'arena_team_recruit_refuse_loyal');
