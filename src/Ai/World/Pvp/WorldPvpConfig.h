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
// here at once so that later steps never touch PlayerbotAIConfig.h; WP3b's encounter harness
// (tests/fight-encounter-selftest.cpp) checked the defaults, the fight rules (FightRules.h) take theirs from here.
// Only the members marked "read" are read from the config now (conf.dist documents those); the others keep their
// defaults until the step named beside them reads them. Player-free (the selftests include it).

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

    // --- Meetings and chances (WP4, read; GrudgeFactor WP9) ---
    // The yards within which an enemy is a meeting and the odds are counted
    uint32_t ScanYards = 40;
    // An enemy rolled for is not rolled for again for this long
    uint32_t RollAgainSeconds = 600;
    // Minimum time since this bot's last fight before its next chance can start one
    uint32_t MinGapMinutes = 30;
    // Aggression to rate: no starts below AggressionOffBelow; one start per HoursAtZero hours at aggression 0, one per
    // HoursAtOne at +1 (HoursAtZero / 4: the budget curve is exactly 4^a times the rate at 0, WP.2, WP.16); the
    // chance's probability never exceeds ChanceCap. "Once per 4 h" and "four times" describe this budget curve
    // before the soft factors: a chance starts at most one fight, the cap trims long quiet times and the colour,
    // courtesy and odds factors lower it, so real starts are rarer (WP.16; WP3b's harness: about once per 4 h at
    // aggression 0 before the soft factors, +1 about twice as often as 0)
    float AggressionOffBelow = -0.5f;
    float HoursAtZero = 3.4f;
    float HoursAtOne = 0.85f;
    float ChanceCap = 1.0f / 3.0f;
    // Colour factors of the chance (grey and red never start a fight); orange goes from OrangeDaring for a daring bot
    // to OrangeCareful for a careful one
    float GreenFactor = 0.5f;
    float YellowFactor = 1.0f;
    float OrangeDaring = 1.0f;
    float OrangeCareful = 0.0f;
    // Courtesy: a courteous bot leaves an enemy alone that is busy with something else (fighting a mob, low health);
    // a rule factor, not an option: only the encounter harness turns it off for its old-rules arm
    bool Courtesy = true;
    // A grudge multiplies the chance against its enemy
    float GrudgeFactor = 3.0f;
    // The odds a bot needs for its full chance (its side's share of the strength it sees): 0.5 + OddsCaution x its
    // caution (-1..+1); the chance falls to 0 a little below that (read: the nights tune it)
    float OddsCaution = 0.12f;

    // --- No camping and calm areas (WP7 read) ---
    // A player resurrected this recently, or within GraveyardYards of a graveyard or spirit healer, is left alone
    uint32_t ResurrectedSeconds = 300;
    uint32_t GraveyardYards = 80;
    // No new fight within CalmYards of a fight that ended in the last CalmMinutes (read: the bot's own place)
    uint32_t CalmYards = 200;
    uint32_t CalmMinutes = 15;

    // --- Joining, retreating and ending (read) ---
    // A bystander within JoinYards of a fight's nearest participant may join it (read)
    uint32_t JoinYards = 60;
    // A bystander's join chance (read: the nights tune them): JoinBase x (1 + JoinAggression x aggression) x (1 +
    // JoinHelpfulness x helpfulness when its side is losing or a player of its faction is the target) x (1 - JoinGoal
    // x its goal's value, 0..1); bad odds lower it by caution
    float JoinBase = 0.4f;
    float JoinAggression = 0.75f;
    float JoinHelpfulness = 0.75f;
    float JoinGoal = 0.75f;
    // Retreat below RetreatHealthPercent x (1 + RetreatCautionHealth x caution) health while its side's odds are
    // below RetreatOdds + RetreatCautionOdds x caution, never above even odds (read: the nights tune them)
    uint32_t RetreatHealthPercent = 30;
    float RetreatCautionHealth = 0.5f;
    float RetreatOdds = 0.5f;
    float RetreatCautionOdds = 0.1f;
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

    // --- Enthusiasm (WP22, read; WP23/WP24 decide by it) ---
    WorldPvpEnthusiasmConfig Enthusiasm;
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

// The fight_start and fight_join decision rows' choices
constexpr char const* CHOICE_START = "start";
constexpr char const* CHOICE_JOIN = "join";
constexpr char const* CHOICE_PASS = "pass";
}  // namespace WorldPvp

#endif
