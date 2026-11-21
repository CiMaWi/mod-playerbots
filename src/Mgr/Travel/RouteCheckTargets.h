/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKTARGETS_H
#define PLAYERBOTS_ROUTECHECKTARGETS_H

// `playerbots route check targets <file>` (pathing plan PA8): what a bot's typed approach finds at each target.
// - The file: one target per line `map x y z reach [kind [entry [guid]]]` (reach in yards; kind, entry, guid pass
//   through to the report).
// - Writes <LogsDir>/route-check-targets-<time>.tsv, one row per usable input row in input order (columns and result
//   words: RouteCheckTargetsRules.h; the stand point is NavMeshSnap::StandPoint under the bots' filter, the fragment
//   column NavMeshSnap::IsFragment), a summary line and `# complete`.
// Read only (SEC_GAMEMASTER); runs on the world thread in slices (it creates the grids it asks, as `route check
// outdoor` does), so use it on a lab without bots. From a session the start is queued to the world thread.

class ChatHandler;

namespace RouteCheckTargets
{
bool HandleTargetsCommand(ChatHandler* handler, char const* args);
}  // namespace RouteCheckTargets

#endif
