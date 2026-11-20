-- Questing WT35e: the executor does objects and containers now, so the failure record forgets the give-ups it
-- counted for them as unsupported (readers skip a quest with two give-ups)
DELETE FROM `playerbots_bot_quest_fail` WHERE `last_cause` IN ('unsupported:object', 'unsupported:chest');
