-- The quests a lone bot cannot do on today's build (questing WQ46): read from questing test T1
-- (lab3-20261127-200248), with T0 (lab3-20261126-122242) and lab5's 22 sessions, in none of which a bot handed one
-- in. The evidence per quest is in the hub's measurements/2026-11-28-questing-failing-quests.md.
DELETE FROM `playerbots_quest_impossible` WHERE `Quest` IN (3123, 3741, 5096, 5441, 11169);
INSERT INTO `playerbots_quest_impossible` (`Quest`, `Cause`, `Note`) VALUES
(3123, 'ours', 'questing, item-use framework: the vessel''s spell needs a dead Ironfur Bear or Longtooth Runner; the executor holds living ones and the use is refused (refused:target, pass_over life)'),
(3741, 'under_water', 'Glinting Mud lies on the floor of Lake Everstill: stand none/no_poly, walks refused; pathing owns the stand point (ToDo 2026-11-27-questing-underwater-quest-objects-have-no-stand-point)'),
(5096, 'ours', 'questing, item-use framework: the Scourge Banner''s use at the camp''s spell focus 176088 is refused by the core''s handler (refused:handler); which check refuses it is not known'),
(5441, 'ours', 'questing, item-use framework: the Blackjack''s use needs a sleeping Lazy Peon (aura 17743); the executor never holds a peon'),
(11169, 'ours', 'questing, item-use framework: the Captured Totem is never used; bots kill the crocolisks without it, so no kill gives credit');
