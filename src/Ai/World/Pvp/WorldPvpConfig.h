/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDPVPCONFIG_H
#define _PLAYERBOT_WORLDPVPCONFIG_H

#include <cstdint>

// The numbers of world PvP for persistent bots in the open world (Playerbots.WorldPvp.*), read once at config load
// (PlayerbotAIConfig::WorldPvp); the initial values are the defaults, conf.dist documents them. Player-free (the
// selftests include it).

// Enthusiasm (WP22, read; Enthusiasm.h; WP23/WP24 decide by it): starting values the nights tune. Points are on the
// scale 0..Max; edits and threshold terms are sizes, Enthusiasm.h gives them their signs
struct WorldPvpEnthusiasmConfig
{
    float Max = 100.0f;
    // Points per hour of play while not engaged, per minute while engaged (times the local balance's factor)
    float RecoveryPerHour = 15.0f;
    float DepletionPerMinute = 20.0f;
    // A PvP death costs DeathEdit at confidence 0, DeathEditConfident at +1, DeathEditUnconfident at -1; a kill of an
    // enemy player gives KillEdit; a group member's PvP death within GroupDeathYards costs GroupDeathShare of the
    // bot's own death edit
    float DeathEdit = 50.0f;
    float DeathEditConfident = 25.0f;
    float DeathEditUnconfident = 75.0f;
    float KillEdit = 10.0f;
    uint32_t GroupDeathYards = 60;
    float GroupDeathShare = 0.3f;
    // Join threshold: JoinBase - JoinAggression x aggression; +JoinGreen for a green target; -JoinFactionAttacked and
    // up to -JoinFactionHelpfulness more by helpfulness when the target attacks a player of the bot's faction;
    // -JoinGroupAttacked when it attacks a group member; +JoinGraveyard within GraveyardYards of a graveyard;
    // -JoinBalanceMax..+JoinBalanceMax by the local balance. When the target already attacks a player of the bot's
    // faction or a group member, the Aggression term counts at AggressionJoinFactor of its size (0..1)
    float JoinBase = 60.0f;
    float JoinAggression = 40.0f;
    float AggressionJoinFactor = 0.25f;
    float JoinGreen = 20.0f;
    float JoinFactionAttacked = 30.0f;
    float JoinFactionHelpfulness = 15.0f;
    float JoinGroupAttacked = 50.0f;
    float JoinGraveyard = 40.0f;
    float JoinBalanceMax = 30.0f;
    // Flee threshold: FleeBase + FleeCaution x caution, FleeGroupHold lower while a living group member within
    // GroupHoldYards is engaged
    float FleeBase = 15.0f;
    float FleeCaution = 20.0f;
    float FleeGroupHold = 15.0f;
    uint32_t GroupHoldYards = 40;
    // The local balance: living players within BalanceYards, weighted by level colour to the bot times health share;
    // the bot's side clearly leads at a share of BalanceLead or more and is clearly behind at BalanceBehind or less,
    // where the depletion factor reaches DepletionFactorLead and DepletionFactorBehind (1 when even)
    uint32_t BalanceYards = 40;
    float WeightGrey = 0.25f;
    float WeightGreen = 0.5f;
    float WeightYellow = 1.0f;
    float WeightOrange = 2.0f;
    float WeightRed = 4.0f;
    float DepletionFactorLead = 0.5f;
    float DepletionFactorBehind = 3.0f;
    float BalanceLead = 0.75f;
    float BalanceBehind = 0.25f;
};

struct WorldPvpConfig
{
    // An enemy player within ScanYards is one the bot may enter a fight on; within GraveyardYards of a graveyard the
    // join threshold's graveyard term applies
    uint32_t ScanYards = 40;
    uint32_t GraveyardYards = 80;

    // --- The ghost's wait (WP7, GhostWait.h) ---
    // A ghost waits while an enemy is within GhostEnemyYards of its corpse, checks again every GhostRecheckSeconds and
    // takes the spirit healer after GhostSpiritHealerSeconds
    uint32_t GhostEnemyYards = 40;
    uint32_t GhostRecheckSeconds = 15;
    uint32_t GhostSpiritHealerSeconds = 300;

    // --- Enthusiasm (WP22, read; WP23/WP24 decide by it) ---
    WorldPvpEnthusiasmConfig Enthusiasm;
};

namespace WorldPvp
{
// The yards of "nothing there" (no enemy at a corpse, no graveyard on a map)
constexpr float FAR_YARDS = 1.0e9f;
}  // namespace WorldPvp

#endif
