/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GATHERPASSBY_H
#define _PLAYERBOT_GATHERPASSBY_H

#include <cstdint>
#include <optional>

#include "ObjectGuid.h"

class PlayerbotAI;

namespace Market
{
struct View;
}

// Passing by (professions plan WR13c, DECISIONS WR.14): the gather rules (GatherRules.h JudgeNode, JudgeHide) in front
// of the loot stack, for a persistent bot whose newest plan brought gather rates (in every status, a trip's too);
// every other bot adds as before. A planned circuit's stop (do_gather) passes ungated
namespace GatherPassBy
{
// AddAllLootAction::AddLoot, on the bot's map thread: false when the target stays off the loot stack; reach: the
// yards it may be fetched from beyond the loot distance (0: none); market: the scan's one view, taken here at the
// first target priced (professions WPC14b)
bool Admit(PlayerbotAI* botAI, ObjectGuid guid, float& reach, std::optional<Market::View>& market);

// OpenLootAction::RequestLoot, at a harvest or a skin cast: the take row of a target the gate let through
void NoteTake(PlayerbotAI* botAI, ObjectGuid guid, bool skin);

// StoreLootAction: true when the bot takes every item of the corpse, so that it can be skinned (the hide pays); a
// hide that does not pay is a pass
bool LootsEmpty(PlayerbotAI* botAI, ObjectGuid corpse);

// OpenLootAction::RequestLoot, before a skin cast: false when the hide does not pay (a pass)
bool Skins(PlayerbotAI* botAI, ObjectGuid corpse);

// Every status change (NewRpgStatusEnd.cpp Change): the ended status's passes as one row, by cause
void NoteStatusEnd(PlayerbotAI* botAI, std::uint32_t oldStatus);
}  // namespace GatherPassBy

#endif
