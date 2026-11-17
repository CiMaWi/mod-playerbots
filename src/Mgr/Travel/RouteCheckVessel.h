/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKVESSEL_H
#define PLAYERBOTS_ROUTECHECKVESSEL_H

// `playerbots route check vessel <display id>|all <x1 y1 z1 x2 y2 z2> | <count> <seed>` (navigation plan NV30b,
// §3.8 6): paths on a vessel's deck mesh (<DataDir>/mmaps/vessels/<display id>.mmtile) with no bot and no
// transport, through the core's one vessel search (PathGenerator::FindVesselPath) on a query of its own; the mesh
// comes from the core's store, never a map's query. Points are passenger offsets (the model's coordinates).
// - Two offsets: the path's class, length, point count, the surface of each end, the points off the mesh, the time.
// - A count and a seed: per vessel (`all`: every file of the dataset) that many random pairs of deck points inside
//   one connected surface, and per pair a third random point that counts as a pair across surfaces when it lies on
//   another surface; plus four pairs with one end off the deck (beside the hull, under it), which must be no path.
//   A line per vessel and per surface; a row per pair to <LogsDir>/route-check-vessel-<time>.tsv.
// Read only (SEC_PLAYER). From the console it runs in the call (the world thread); from a session it is queued to
// the world thread and its lines go to the server log.

class ChatHandler;

namespace RouteCheckVessel
{
bool HandleVesselCommand(ChatHandler* handler, char const* args);
}  // namespace RouteCheckVessel

#endif
