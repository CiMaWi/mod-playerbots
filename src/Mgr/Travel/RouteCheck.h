/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECK_H
#define PLAYERBOTS_ROUTECHECK_H

// `playerbots route check links [map [count]] | status | stop` (NV3b, NV11b): walks every walk link of the loaded
// travel graph as a bot would and checks whether every node stands on the mesh, with no bot online.
// No argument: maps 0, 1, 530 and 571; [count]: only the first count walk links of the map (a diagnosis).
// The check runs on the world thread (a map's navmesh query is not thread-safe and loading tiles changes the
// navmesh) in slices queued through PlayerbotWorldThreadProcessor, so the command returns at once; `status` prints
// the progress or the last result, the report goes to <LogsDir>/route-check-links-<time>.tsv. The probe is the walk
// a bot makes over the link, as its movement asks the engine (MoveFarTo, MovePoint; RouteCheckRules.h Walk); every
// walk link gets a class from both directions (ok, reverse-only, end-off-mesh, dead, shortcut, capped).
// `playerbots route check pairs <file>` (GM; lines `map x1 y1 z1 x2 y2 z2 [label]`): the same walks for listed
// pairs of points, the points each walk passed beside the class, to <LogsDir>/route-check-pairs-<time>.tsv.
// `status` and `stop` serve both; one job at a time.

class ChatHandler;

namespace RouteCheck
{
bool HandleLinksCommand(ChatHandler* handler, char const* args);
bool HandlePairsCommand(ChatHandler* handler, char const* args);
}  // namespace RouteCheck

#endif
