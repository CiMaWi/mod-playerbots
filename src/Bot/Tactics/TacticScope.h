/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TACTICSCOPE_H
#define _PLAYERBOT_TACTICSCOPE_H

#include <string>
#include <vector>

#include "Define.h"

// The scope text of the tactics and the natural layers (Arena,Battleground,World,Pve, All, None) and the small text
// helpers of the tactic commands. Game-free: the config tables and their selftest compile it.

namespace TacticText
{
std::string Lower(std::string text);
std::string Trim(std::string const& text);
// A comma list, each entry trimmed; a trailing comma gives an empty last entry
std::vector<std::string> SplitList(std::string const& text);
}  // namespace TacticText

// "Arena,Battleground,World,Pve" (any case, also All and None) into a scope mask; false on an unknown or empty entry
bool ParseTacticScope(std::string const& text, uint8& scope, std::string& error);
std::string FormatTacticScope(uint8 scope);

#endif
