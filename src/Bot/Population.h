/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_POPULATION_H
#define _PLAYERBOT_POPULATION_H

#include "Define.h"

class Player;

// Persistent population (living-azeroth phase H). H1: with Playerbots.Population.Persistent = 1 no code
// path lowers a random bot's level. Keys are read here through sConfigMgr (not PlayerbotAIConfig.h) and
// re-read on ".reload config". The rules themselves are in PopulationPolicy.h.
namespace Population
{
// Any thread (relaxed atomic). Written on the world thread at startup and on ".reload config".
bool IsPersistent();

// Re-reads the keys and logs the state, plus one warning per refused level-lowering option. World thread.
void LoadConfig(bool reload);

// The level a level-setting path may give `bot`: `requestedLevel`, or the bot's current level when
// persistence is on, the bot is a random bot and `requestedLevel` is lower (logged as a refusal with
// `site`). Cheap when nothing is lowered; any thread that may touch the bot.
uint32 GuardLevel(Player* bot, uint32 requestedLevel, char const* site);
}  // namespace Population

void AddSC_population();

#endif
