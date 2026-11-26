/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GEARSWEEP_H
#define _PLAYERBOT_GEARSWEEP_H

class ChatHandler;

// The gear plan's sweep (WG1), a console command on the world thread: asks the upgrade rule (`item upgrade`) about
// every weapon and armour template a bot's class may use within ten levels below its level (random-property and suffix
// items once per suffix of a small sample), scores each with the rule's own score setup, and records the answers, the
// fit tests and the worn items in a tab-separated file in the log folder; it counts the rows that break the plan's
// invariants S1-S6 (and X1, X2). Read only; off every bot's tick. Test-only: nothing runs unless the command is typed.
namespace GearSweep
{
// playerbots gear sweep [<bot>]      one bot, or every online random bot and a total line
//   per bot: `[Route] <bot> gear sweep done S1 n S2 n S3 n S4 n S5 n S6 n X1 n X2 n rows n ms n`
//   a death knight of level 57-60 also gets `[Route] <bot> gear sweep pair ...` (PLAN item 3: 34659 and 18509)
bool HandleSweepCommand(ChatHandler* handler, char const* args);
}  // namespace GearSweep

#endif
