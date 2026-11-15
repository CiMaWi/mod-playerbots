/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGSTATUSEND_H
#define PLAYERBOTS_NEWRPGSTATUSEND_H

#include "StatusEndRules.h"

class PlayerbotAI;

// The status core (consolidation CS14, R6): an RPG status ends through one call, on the bot's map thread. It runs the
// status's own clean-up where StatusEnd::CleansUp says so (errands and trips have one), goes idle and writes the one
// Status row with the end's word. O(1), no lock, no database access, no allocation.
namespace RpgStatus
{
// Ends the bot's status as `outcome`: its clean-up, idle, one Status row (note.word, else the outcome's word)
void End(PlayerbotAI* botAI, StatusEnd::Outcome outcome, StatusEnd::Note note = {});
}  // namespace RpgStatus

#endif
