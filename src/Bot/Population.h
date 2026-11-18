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

class ChatHandler;
class Player;
namespace PopulationPolicy
{
struct RotationSwap;
}

// Persistent population (living-azeroth phase H). H1: no code path lowers a random bot's level (the upstream
// level-lowering options are ignored). H2: where the population follows the targets (BotKind::FollowsTargets, the
// role value Playerbots.Population.FollowTargets) the roster of random bot characters is larger than the online cap
// and a schedule rotates who is online. Keys are read here through sConfigMgr (not PlayerbotAIConfig.h) and re-read
// on ".reload config". The rules themselves are in PopulationPolicy.h.
namespace Population
{
// Re-reads the keys and logs the state, plus one warning naming the ignored level-lowering options that are set.
// World thread.
void LoadConfig(bool reload);

// The level a level-setting path may give `bot`: `requestedLevel`, or the bot's current level when
// the bot is a random bot and `requestedLevel` is lower (logged as a refusal with
// `site`). Cheap when nothing is lowered; any thread that may touch the bot.
uint32 GuardLevel(Player* bot, uint32 requestedLevel, char const* site);

// Any thread (relaxed atomic): the population follows the targets (BotKind::FollowsTargets as LoadConfig last saw
// it), so the roster rotates
bool IsRotation();

// Startup (account creation and account types, the factory's target): the roster's count for an empty roster:
// `upstreamCount`, or Playerbots.Population.Rotation.RosterSize when the rotation is on and it is larger.
// Reads BotKind::FollowsTargets (it runs before LoadConfig, after PlayerbotAIConfig::Initialize set the role).
uint32 RosterBotCount(uint32 upstreamCount);

// Startup: the random bot characters the roster accounts are sized for: RosterBotCount; while the population follows
// the targets (BotKind::FollowsTargets) the larger of it and the characters the roster accounts held when a start
// first asked (read once), plus a tenth as spare slots (PopulationPolicy::RosterAccountBots). The factory then makes
// roster characters only while the roster accounts hold none (PopulationPolicy::RefillTarget); the roster's later
// growth is ".playerbots population resize"'s, and the roster accounts keep spare slots for it.
uint32 RosterAccountBotCount(uint32 upstreamCount);

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

// World thread. The cost of the login fill's account enumeration (AddRandomBots): one call's microseconds and
// characters found, added to the ramp's total; FlushEnumeration logs the ramp's line (and the LoadRoster totals)
// and zeroes it, doing nothing when no call was noted. It runs when the ramp ends and after every 10 calls.
void NoteEnumeration(uint64 micros, uint32 characters);
void FlushEnumeration();

// World thread. H4 (a bot retired or created): reload the roster at the next tick; a retired bot's schedule state
// (its owed online time) goes too. The rotation's per-guid caches are the roster and the schedule state.
void InvalidateRoster(uint32 retiredGuid = 0);

// Any thread (relaxed atomic): the retirement is on, which it is exactly where the roster rotates (IsRotation)
bool IsRetire();

// H4a. World thread (RandomPlayerbotMgr::UpdateAIInternal), every tick; returns at once while the retire is off and no
// query is in flight. Every Retire.IntervalSeconds, in the top level above Retire.TestTarget (0: not there), else in
// the level ranges persistently over target: offline roster bots no real player knows (DH.2) are deleted, at most
// Retire.MaxPerPass per pass and Retire.MaxPerHour per hour, never below a range's target (DH.3). `online` is the
// manager's online set.
void RetireTick(std::unordered_set<uint32> const& online);

// .playerbots population status
bool HandleStatusCommand(ChatHandler* handler, char const* args);

// World-log WL19, console only: ".playerbots population resize [<count> [plan]]" moves the roster toward `count`
// characters by WL.14's shares (PopulationPolicy::ResizePlan): complete deletes from the ranges most over theirs, the
// factory's new bots at levels of the ranges most under; spread over RetireTick's ticks. Without arguments: its state.
bool HandleResizeCommand(ChatHandler* handler, char const* args);
}  // namespace Population

void AddSC_population();

#endif
