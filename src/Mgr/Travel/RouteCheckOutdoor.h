/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKOUTDOOR_H
#define PLAYERBOTS_ROUTECHECKOUTDOOR_H

// `playerbots route check outdoor [<file>]` (navigation plan NV32, G88/G91): the engine's answer at points, for the
// graph build's outdoor column and its rule for air link ends (what the columns mean: RouteCheckOutdoorRules.h).
// - No argument: every node of the loaded travel graph. A file: one point per line `map x y z [label]`, so a
//   candidate's new and moved nodes are asked without loading it.
// - Writes <LogsDir>/route-check-outdoor-<time>.tsv (rows key map x y z outdoor area zone flyable liquid floor_z
//   label) and a summary line. A point on a map the server answers only per instance is a `nomap` row.
// Read only (SEC_PLAYER); runs on the world thread in slices (it creates the grids it asks, as `route check pairs`
// does), so use it on a lab without bots. From a session the start is queued to the world thread; the result goes
// to the server log.

class ChatHandler;

namespace RouteCheckOutdoor
{
bool HandleOutdoorCommand(ChatHandler* handler, char const* args);
}  // namespace RouteCheckOutdoor

#endif
