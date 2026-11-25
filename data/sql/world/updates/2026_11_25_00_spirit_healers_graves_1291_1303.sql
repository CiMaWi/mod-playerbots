-- Spirit Healer (entry 6491) at two graveyards that send ghosts to a spot without one:
-- 1291 Terokkar Forest, Razorthorn Rise GY and 1303 Grizzly Hills, Eastern GY.
-- Sources: TrinityCore TDB 335.26091 (creature guids 78808 and 12459 at these graves) and Wowhead's
-- spawn maps of npc 6491 (TBC Classic: Terokkar 50.6, 11.6; WotLK Classic: Grizzly Hills 70.6, 40.2).
-- Graveyard 1298 (Lake Jorune) is left as it is: only TrinityCore has a healer there.
DELETE FROM `creature` WHERE `guid` IN (6491001, 6491002);
INSERT INTO `creature` (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `equipment_id`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`, `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`, `ScriptName`, `VerifiedBuild`, `CreateObject`, `Comment`) VALUES
(6491001, 6491, 530, 0, 0, 1, 4294967295, 0, -1423.5, 4358.49, 241.566, 5.18548, 60, 0, 0, 4120, 0, 0, 0, 0, 0, '', 0, 0, 'Terokkar Forest, Razorthorn Rise GY: Spirit Healer'),
(6491002, 6491, 571, 0, 0, 1, 4294967295, 0, 4105.36, -4818.6, 76.074, 0.149361, 60, 0, 0, 4120, 0, 0, 0, 0, 0, '', 0, 0, 'Grizzly Hills, Eastern GY: Spirit Healer');
