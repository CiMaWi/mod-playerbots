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
// report format are RouteCheckRules.h.

class ChatHandler;

namespace RouteCheck
{
bool HandleLinksCommand(ChatHandler* handler, char const* args);
}

#endif
