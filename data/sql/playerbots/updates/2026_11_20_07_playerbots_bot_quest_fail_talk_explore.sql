-- Questing WT35f: the executor walks into area triggers and talks to creatures through their gossip now, so the
-- failure record forgets the give-ups it counted for them: the exploring refusals, and the no-target, timeout and
-- abandoned rows of the quests whose creature gives its credit by gossip (a kill objective on a friendly creature,
-- never attacked)
DELETE FROM `playerbots_bot_quest_fail` WHERE `last_cause` IN ('unsupported:explore');
DELETE FROM `playerbots_bot_quest_fail`
WHERE `quest_id` IN (3520, 4921, 5211, 5721, 8746, 8762, 9164, 9437, 9663, 9738, 9785, 10097, 10368, 10980, 11221,
                     11479, 11983, 12204, 12245, 12592, 12980, 13926, 13927)
  AND `last_cause` IN ('no_target', 'timeout', 'abandoned');
