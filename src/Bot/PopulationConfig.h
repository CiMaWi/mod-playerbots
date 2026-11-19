/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_POPULATIONCONFIG_H
#define _PLAYERBOT_POPULATIONCONFIG_H

#include <cstdint>

// Playerbots.Population.*: the values as configured (Population::Options()); the initial values are the defaults.
// Population::LoadConfig turns them into the running settings (minutes become seconds, the session minimum is capped
// at the mean there). Player-free (the selftests include it).
struct PopulationOptions
{
    // Rotation.RosterSize: random bot characters in the roster of a fresh database while the rotation is on
    // (0 = the online cap alone)
    uint32_t RosterSize = 2500;
    // Rotation.IntervalSeconds (10..): seconds between two rotation ticks
    uint32_t IntervalSeconds = 300;
    // Rotation.MeanSessionMinutes (1..): mean length of an online session
    uint32_t MeanSessionMinutes = 120;
    // Rotation.MinSessionMinutes: a swapped bot stays at least this long (capped at the mean when applied)
    uint32_t MinSessionMinutes = 30;
    // Rotation.AcquaintedSharePct (..100): online share per day (percent) owed to a bot a real player knows. Never
    // below the fair share cap / roster; lowered when the acquainted bots alone would exceed the cap.
    uint32_t AcquaintedSharePct = 90;
    // Rotation.AcquaintedMinScore (1..): the acquaintance score from which a bot counts as known
    uint32_t AcquaintedMinScore = 1;
    // Retire.IntervalSeconds (10..): seconds between two retirement passes
    uint32_t RetireIntervalSeconds = 600;
    // Retire.MaxPerPass, Retire.MaxPerHour: bots retired at most per pass and per sliding hour
    uint32_t RetireMaxPerPass = 1;
    uint32_t RetireMaxPerHour = 10;
    // Retire.TestTarget (test): retire the max-level range down to this many bots; 0 = nothing is retired
    uint32_t RetireTestTarget = 0;
    // FollowTargets (role): this server's population follows the targets (rotation and retirement)
    bool FollowTargets = true;
};

template <typename Struct>
struct ConfigFamily;

// The config table families (PopulationConfig.cpp): Playerbots.Population.Rotation.*, .Retire.* and the one key
// Playerbots.Population.FollowTargets
extern ConfigFamily<PopulationOptions> const PopulationRotationFamily;
extern ConfigFamily<PopulationOptions> const PopulationRetireFamily;
extern ConfigFamily<PopulationOptions> const PopulationFollowFamily;

#endif
