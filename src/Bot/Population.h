/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_POPULATION_H
#define _PLAYERBOT_POPULATION_H

#include <unordered_set>
#include <vector>

#include "Define.h"
#include "PopulationPolicy.h"

class ChatHandler;
class Player;

// Persistent population (living-azeroth phase H). H1: with Playerbots.Population.Persistent = 1 no code
// path lowers a random bot's level. H2: with Playerbots.Population.Rotation.Enable = 1 the roster of random
// bot characters is larger than the online cap and a schedule rotates who is online. Keys are read here
// through sConfigMgr (not PlayerbotAIConfig.h) and re-read on ".reload config". The rules themselves are in
// PopulationPolicy.h.
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

// Any thread (relaxed atomic): Playerbots.Population.Rotation.Enable
bool IsRotation();

// Startup (account creation and account types): how many random bot characters the accounts are sized for:
// `upstreamCount`, or Playerbots.Population.Rotation.RosterSize when the rotation is on and the roster is larger.
// Reads the config itself (it runs before LoadConfig).
uint32 RosterBotCount(uint32 upstreamCount);

// World thread (RandomPlayerbotMgr::UpdateAIInternal), every Rotation.IntervalSeconds, else nothing: the swaps of
// online bots for offline ones (`in` 0: only a logout of a bot outside the roster). `accounts` are the random bot
// accounts, `online` the manager's online set, `cap` its bot count. The caller logs the `out` bots out and lets the
// `in` bots log in, both before the next call.
std::vector<PopulationPolicy::RotationSwap> RotationTick(std::vector<uint32> const& accounts,
                                                         std::unordered_set<uint32> const& online, uint32 cap);

// World thread, the login fill (AddRandomBots): roster membership (true while no roster is loaded), the login order,
// and a login the schedule should know of
bool InRoster(uint32 guid);
bool LoginBefore(uint32 a, uint32 b);
void NoteLogin(uint32 guid);

// World thread. H4 (a bot retired or created): reload the roster at the next tick; a retired bot's schedule state
// (its owed online time) goes too. The rotation's per-guid caches are the roster and the schedule state.
void InvalidateRoster(uint32 retiredGuid = 0);

// Any thread (relaxed atomic): Playerbots.Population.Balance.Enable
bool IsBalance();

// H3. Any thread, O(1), no lock, no allocation (the XP hook): the XP multiplier of a random bot of `level` from the
// last balance update; 1 while the balance is off. OnPlayerGiveXP multiplies it into Playerbots.RandomBotXPRate.
float XPMultiplier(uint32 level);

// World thread (RandomPlayerbotMgr::UpdateAIInternal), every Balance.IntervalMinutes, else nothing: counts the
// population per level range (the rotation roster while the rotation has one, else `online`; levels from the
// character cache, no DB query), steps the controller and publishes the XP multipliers and the rotation weights.
void BalanceTick(std::unordered_set<uint32> const& online);

// World thread. H4 (retires bots in ranges persistently over target, by these targets): the level ranges as last
// measured: levels, target, count, XP multiplier, online weight and overSince (game time since when the range is
// over its target beyond the deadband; 0 = not over). Empty while the balance is off or before its first update.
// H3 keeps no per-guid cache (counts come from the roster or the online set at each update).
std::vector<PopulationPolicy::BalanceRange> BalanceRanges();

// World thread: the index into BalanceRanges() of a level
uint32 BalanceRangeOf(uint32 level);

// .playerbots population status | selftest
bool HandleStatusCommand(ChatHandler* handler, char const* args);
bool HandleSelftestCommand(ChatHandler* handler, char const* args);
}  // namespace Population

void AddSC_population();

#endif
