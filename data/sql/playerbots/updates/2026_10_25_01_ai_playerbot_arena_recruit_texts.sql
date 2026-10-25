-- #########################################################
-- Playerbots - Arena team recruiting texts (Playerbots.ArenaRecruit.*)
-- A bot answers a real player's arena team invite in character.
-- Localized for all WotLK locales (koKR, frFR, deDE, zhCN,
-- zhTW, esES, esMX, ruRU)
-- #########################################################

DELETE FROM ai_playerbot_texts
WHERE name IN ('arena_team_recruit_accept', 'arena_team_recruit_refuse_level', 'arena_team_recruit_refuse_loyal');

DELETE FROM ai_playerbot_texts_chance
WHERE name IN ('arena_team_recruit_accept', 'arena_team_recruit_refuse_level', 'arena_team_recruit_refuse_loyal');

INSERT INTO `ai_playerbot_texts`
(`id`, `name`, `text`, `say_type`, `reply_type`,
 `text_loc1`, `text_loc2`, `text_loc3`, `text_loc4`,
 `text_loc5`, `text_loc6`, `text_loc7`, `text_loc8`)
VALUES
(1929, 'arena_team_recruit_accept', 'Gladly. Let''s see what we can do together.', 0, 0,
'기꺼이요. 함께 무엇을 할 수 있는지 봅시다.',
'Avec plaisir. Voyons ce que nous pouvons faire ensemble.',
'Sehr gern. Mal sehen, was wir zusammen schaffen.',
'乐意之至。让我们看看能一起做些什么。',
'樂意之至。讓我們看看能一起做些什麼。',
'Con gusto. Veamos qué podemos hacer juntos.',
'Con gusto. Veamos qué podemos hacer juntos.',
'С удовольствием. Посмотрим, что мы сможем сделать вместе.'),
(1930, 'arena_team_recruit_refuse_level', 'I don''t think we''d be a good fit yet. Maybe if we spent some time together first.', 0, 0,
'아직은 우리가 잘 맞지 않을 것 같아요. 먼저 함께 시간을 보내면 달라질 수도 있겠죠.',
'Je ne pense pas que nous soyons encore faits pour jouer ensemble. Peut-être si nous passions d''abord du temps ensemble.',
'Ich glaube, wir passen noch nicht gut zusammen. Vielleicht, wenn wir erst einmal etwas Zeit miteinander verbringen.',
'我觉得我们现在还不太合适。也许先一起相处一段时间会好些。',
'我覺得我們現在還不太合適。也許先一起相處一段時間會好些。',
'No creo que encajemos bien todavía. Quizá si pasáramos algo de tiempo juntos primero.',
'No creo que encajemos bien todavía. Quizá si pasáramos algo de tiempo juntos primero.',
'Не думаю, что мы пока хорошо подходим друг другу. Может быть, если сначала проведём немного времени вместе.'),
(1931, 'arena_team_recruit_refuse_loyal', 'Thanks for asking, but I''m happy where I am.', 0, 0,
'물어봐 주셔서 고맙지만, 저는 지금 있는 곳에 만족해요.',
'Merci de me l''avoir proposé, mais je suis bien là où je suis.',
'Danke für die Frage, aber ich bin zufrieden, wo ich bin.',
'谢谢你的邀请，不过我现在这样挺好的。',
'謝謝你的邀請，不過我現在這樣挺好的。',
'Gracias por preguntar, pero estoy contento donde estoy.',
'Gracias por preguntar, pero estoy contento donde estoy.',
'Спасибо, что спросили, но мне хорошо там, где я есть.');

INSERT INTO ai_playerbot_texts_chance (name, probability)
VALUES
('arena_team_recruit_accept', 100),
('arena_team_recruit_refuse_level', 100),
('arena_team_recruit_refuse_loyal', 100);
