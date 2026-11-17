/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECHECKSTOPS_H
#define PLAYERBOTS_ROUTECHECKSTOPS_H

// `playerbots route check stops [live]` (navigation plan NV31, §3.8 5-6): where every vessel stands still for boarding.
// - No argument: one row per entry and stop from the in-memory stores, at any time after start, to
//   <LogsDir>/route-check-stops-<time>.tsv, and a summary line. Type 15 (ships, zeppelins, turtles): every
//   MO_TRANSPORT template with a transport template; a stop is a stop frame, with heading A (InitialOrientation) and
//   heading B (the moving spline's direction at the end of the segment it is arrived by, or A when it is arrived at by
//   a teleport), arrive and depart ms. Type 11 (lifts, tram cars): every spawn of a TRANSPORT template with an
//   animation; a stop is a span of progress (RouteCheckStopsRules::StopSpans), its pose RelocateToProgress' at the
//   span's start (heading A) and end (heading B).
// - `live`: every MotionTransport on the continents now: its position, heading, timer and the stop it stands at (else
//   `moving`), with the distance to that stop's position and the angles to its heading A and B. A line each.
// Read only (SEC_PLAYER). From the console it runs in the call (the world thread); from a session it is queued to
// the world thread and its lines go to the server log.

class ChatHandler;

namespace RouteCheckStops
{
bool HandleStopsCommand(ChatHandler* handler, char const* args);
}  // namespace RouteCheckStops

#endif
