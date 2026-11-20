/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PROFESSIONVISIT_H
#define PLAYERBOTS_PROFESSIONVISIT_H

#include <string>

class Player;

namespace ProfessionVisit
{
// The test command's visit (WR4b): the profession trainer stop's handler run on the nearest tradeskill trainer in
// reach. `report` is the refusal or what the stop did; false = refused
bool Run(Player* bot, std::string& report);
}  // namespace ProfessionVisit

#endif
