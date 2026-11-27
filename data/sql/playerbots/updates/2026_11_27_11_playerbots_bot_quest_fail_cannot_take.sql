-- Questing WQ41: the plan reads the game's own answer about takes (the core's CanTakeQuest on the bot's map thread)
-- and the live accept asks it too, so a take the game refuses (a skill, a standing, a holiday the old copy of its rule
-- did not know) is no longer planned nor given up. The failure record forgets the give-ups it counted for planned
-- takes (cannot_take): most were such refusals. A giver that still does not offer its quest is written again
DELETE FROM `playerbots_bot_quest_fail` WHERE `last_cause` = 'cannot_take';
