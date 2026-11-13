/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECK_H
#define PLAYERBOTS_ROUTECHECK_H

// `playerbots route check links [map [count]] | status | stop` (NV3b): asks the engine for a path between the ends
// of every walk link of the loaded travel graph and whether every node stands on the mesh, with no bot online.
// No argument: maps 0, 1, 530 and 571; [count]: only the first count walk links of the map (a diagnosis).
// The check runs on the world thread (a map's navmesh query is not thread-safe and loading tiles changes the
// navmesh) in slices queued through PlayerbotWorldThreadProcessor, so the command returns at once; `status` prints
// the progress or the last result, the report goes to <LogsDir>/route-check-links-<time>.tsv. The classing and the
// report format are RouteCheckRules.h. Every walk link gets a class from both directions (ok, reverse-only,
// end-off-mesh, dead, capped).
// `playerbots route check pairs <file>` (GM; lines `map x1 y1 z1 x2 y2 z2 [label]`): the same two probes for listed
// pairs of points, the corners of each answer beside the class, to <LogsDir>/route-check-pairs-<time>.tsv.
// `playerbots route check pairs walk <file>` (NV13b): for each pair the forward class and the walk a bot makes, as its
// movement asks the engine (MoveFarTo, MovePoint; RouteCheckRules.h Walk), to <LogsDir>/route-check-walks-<time>.tsv
// as `pair.walk` rows: arrived or stuck, the kinds of moves (mesh, incomplete, farpoly, straight, cone) and the yards
// walked along the mesh and in straight lines.
// `status` and `stop` serve all three; one job at a time.

class ChatHandler;

namespace RouteCheck
{
bool HandleLinksCommand(ChatHandler* handler, char const* args);
bool HandlePairsCommand(ChatHandler* handler, char const* args);
}  // namespace RouteCheck

#endif
