/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDPVPCONFIG_H
#define _PLAYERBOT_WORLDPVPCONFIG_H

#include <cstdint>

// The numbers of world PvP for persistent bots in the open world (Playerbots.WorldPvp.*), read once at config load
// (PlayerbotAIConfig::WorldPvp); the initial values are the defaults. Every option of the world PvP plan is declared
// here at once so that later steps never touch PlayerbotAIConfig.h; the values are the plan's estimates until WP3
// tunes them. Only the members marked "read" are read from the config now (conf.dist documents those); the others
// keep their defaults until the step named beside them reads them. Player-free (the selftests include it).
struct WorldPvpConfig
{
    // --- The fight book and the fight memory (WP1, read) ---
    // An enemy player in sight is a new meeting when this bot has not seen it for this long
    uint32_t MeetingMemorySeconds = 600;
    // Enemies one bot's memory holds (the oldest is forgotten first); at most WorldPvp::MAX_RECENT_ENEMIES
    uint32_t RecentEnemies = 16;
    // Active fights per map and participants per fight; a fight or participant beyond them is not recorded
    uint32_t MaxActiveFights = 32;
    uint32_t MaxParticipants = 40;
    // A new fight's radius; the radius follows the participants up to the maximum. Combat within it joins the fight
    uint32_t FightRadiusYards = 40;
    uint32_t FightRadiusMaxYards = 150;
    // The safety cap: a fight still in PvP combat after this long ends as stalemate_cap (none runs forever)
    uint32_t MaxFightMinutes = 30;
    // A fight ends as combat_over once none of its participants has been in PvP combat for this long; combat within
    // the grace continues the same fight (a short pause, a re-engage)
    uint32_t EndGraceSeconds = 10;
    // Test switch for `.playerbots fight ready|meet` (labs only)
    bool TestCommands = false;

    // --- Meetings and chances (WP3/WP4 read) ---
    // The yards within which an enemy is a meeting and the odds are counted
    uint32_t ScanYards = 40;
    // An enemy rolled for is not rolled for again for this long
    uint32_t RollAgainSeconds = 600;
    // Minimum time since this bot's last fight before its next chance can start one
    uint32_t MinGapMinutes = 30;
    // Aggression to rate: no starts below AggressionOffBelow; one start per HoursAtZero hours at aggression 0, one per
    // HoursAtOne at +1; the chance's probability never exceeds ChanceCap
    float AggressionOffBelow = -0.5f;
    float HoursAtZero = 4.0f;
    float HoursAtOne = 1.0f;
    float ChanceCap = 1.0f / 3.0f;
    // Colour factors of the chance (grey and red never start a fight); orange goes from OrangeDaring for a daring bot
    // to OrangeCareful for a careful one
    float GreenFactor = 0.5f;
    float YellowFactor = 1.0f;
    float OrangeDaring = 1.0f;
    float OrangeCareful = 0.0f;
    // Courtesy: a bot leaves an enemy alone that is busy with something else (fighting a mob, low health, mounted on
    // a flight path); the exact rules are WP3's
    bool Courtesy = true;
    // A grudge multiplies the chance against its enemy
    float GrudgeFactor = 3.0f;

    // --- No camping and calm areas (WP7 read) ---
    // A player resurrected this recently, or within GraveyardYards of a graveyard or spirit healer, is left alone
    uint32_t ResurrectedSeconds = 300;
    uint32_t GraveyardYards = 80;
    // No new fight within CalmYards of a fight that ended in the last CalmMinutes (read now: the ended fights' ring)
    uint32_t CalmYards = 200;
    uint32_t CalmMinutes = 15;

    // --- Joining, retreating and ending (WP5/WP6 read) ---
    // A bystander within JoinYards of a fight may join it
    uint32_t JoinYards = 60;
    // Retreat below this health percent when its side's odds are below RetreatOdds (placeholders until WP3)
    uint32_t RetreatHealthPercent = 30;
    float RetreatOdds = 0.5f;
    // A chase ends after ChaseSeconds or ChaseYards beyond the fight's radius
    uint32_t ChaseSeconds = 15;
    uint32_t ChaseYards = 40;
    // A fight ends after NoHitSeconds without a hit between the sides, or as a stalemate after StalemateSeconds
    uint32_t NoHitSeconds = 30;
    uint32_t StalemateSeconds = 300;

    // --- Death (WP7 read) ---
    // A ghost waits while an enemy is within GhostEnemyYards of its corpse, checks again every GhostRecheckSeconds and
    // takes the spirit healer after GhostSpiritHealerSeconds
    uint32_t GhostEnemyYards = 40;
    uint32_t GhostRecheckSeconds = 15;
    uint32_t GhostSpiritHealerSeconds = 300;
    // After a death the planner avoids AvoidYards around the place for AvoidMinMinutes to AvoidMaxMinutes
    uint32_t AvoidYards = 300;
    uint32_t AvoidMinMinutes = 30;
    uint32_t AvoidMaxMinutes = 60;

    // --- Grudges and duels (WP8/WP11 read) ---
    uint32_t MaxGrudges = 5;
    uint32_t GrudgeHalfLifeDays = 3;
    // A duel is offered to a player within this many levels
    uint32_t DuelLevelRange = 3;
};

namespace WorldPvp
{
// The fixed capacity of a bot's recent enemies (WorldPvpConfig::RecentEnemies is clamped to it)
constexpr uint32_t MAX_RECENT_ENEMIES = 32;
// The capacity of a map's ring of ended fights (the calm areas)
constexpr uint32_t MAX_ENDED_FIGHTS = 128;

// Event types (EventLog::Emit's type; the words are stable, world-log.md lists them)
constexpr char const* EVENT_FIGHT_START = "FightStart";
constexpr char const* EVENT_FIGHT_JOIN = "FightJoin";
constexpr char const* EVENT_FIGHT_RETREAT = "FightRetreat";  // WP6
constexpr char const* EVENT_FIGHT_END = "FightEnd";
constexpr char const* EVENT_DUEL = "Duel";      // WP11: challenge, answer, result
constexpr char const* EVENT_GRUDGE = "Grudge";  // WP8: formed, ended by revenge or fading

// FightEnd's second row (participants and deaths per side) has this source word
constexpr char const* SOURCE_SIDES = "sides";
// FightRetreat's source word (WP6)
constexpr char const* SOURCE_RETREAT = "retreat";

// The fight_start decision row's choices
constexpr char const* CHOICE_START = "start";
constexpr char const* CHOICE_PASS = "pass";
}  // namespace WorldPvp

#endif
