/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTORDERSTATUS_H
#define PLAYERBOTS_TESTORDERSTATUS_H

#include <string>

class Player;
class PlayerbotAI;
struct TravelOrder;

// `route start` (tests, WT64e): a status started at the order's place with what its own pick would give it
namespace TravelOrders
{
bool KnownStatus(std::string const& word);  // a word `route start` takes
std::string StatusWords();                  // those words, for the usage line
// On the bot's map thread (TestOrders.cpp's ApplyTravelOrder): the order's status started, or refused and logged
bool StartStatus(PlayerbotAI* botAI, Player* bot, TravelOrder const& order);
}  // namespace TravelOrders

#endif
