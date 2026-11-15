/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STATUSENDRULES_H
#define PLAYERBOTS_STATUSENDRULES_H

// How an RPG status ends (consolidation CS14, R6), game-free: the eight outcomes of an end, the Status row's word of
// each, which status runs its own clean-up for which outcome, and the reset rule. The status core (RpgStatus::End)
// carries the decisions out on the bot's map thread; tests/status-end-selftest.cpp checks every cell. No game types
// here: the status numbers are kept as plain constants and the game side static_asserts them against RPG_*.

#include <cstdint>

namespace StatusEnd
{
// The NewRpgStatus values (PlayerbotAIConfig.h, RPG_IDLE ... RPG_ERRANDS, RPG_STATUS_END).
constexpr std::uint32_t STATUS_IDLE = 0;
constexpr std::uint32_t STATUS_GO_GRIND = 1;
constexpr std::uint32_t STATUS_GO_CAMP = 2;
constexpr std::uint32_t STATUS_WANDER_RANDOM = 3;
constexpr std::uint32_t STATUS_WANDER_NPC = 4;
constexpr std::uint32_t STATUS_DO_QUEST = 5;
constexpr std::uint32_t STATUS_TRAVEL_FLIGHT = 6;
constexpr std::uint32_t STATUS_REST = 7;
constexpr std::uint32_t STATUS_OUTDOOR_PVP = 8;
constexpr std::uint32_t STATUS_DO_GATHER = 9;
constexpr std::uint32_t STATUS_TRAVEL = 10;
constexpr std::uint32_t STATUS_ERRANDS = 11;
constexpr std::uint32_t STATUS_COUNT = 12;

// What happened, not a judgement. Interrupted: another status, the planner or a reset took the bot over. Ordered: a
// GM, test or bridge order ended it. Sick: resurrection sickness ended it and the bot rests.
enum class Outcome : std::uint8_t
{
    Arrived,
    Finished,
    TimedOut,
    Stuck,
    Abandoned,
    Interrupted,
    Ordered,
    Sick
};
constexpr std::uint32_t OUTCOME_COUNT = 8;

// What an end's rows say beyond the outcome. Literals only (an end allocates nothing); every field is optional.
struct Note
{
    char const* word{nullptr};    // the Status row's reason; null: the outcome's default word
    char const* source{nullptr};  // the status's own event source: a trip's "abort:<reason>"
    char const* text{nullptr};    // the server log's phrase for it ("stuck aboard")
};

// The Status row's default word of an outcome.
constexpr char const* Word(Outcome outcome)
{
    switch (outcome)
    {
        case Outcome::Arrived:
            return "arrived";
        case Outcome::Finished:
            return "finished";
        case Outcome::TimedOut:
            return "timeout";
        case Outcome::Stuck:
            return "stuck";
        case Outcome::Abandoned:
            return "abandoned";
        case Outcome::Interrupted:
            return "interrupted";
        case Outcome::Ordered:
            return "order";
        case Outcome::Sick:
            return "sick";
    }
    return "";
}

// Whether the planner judges the run that ended (progress compared with the run's start): not when something outside
// the goal ended it.
constexpr bool Judged(Outcome outcome)
{
    return outcome != Outcome::Interrupted && outcome != Outcome::Ordered && outcome != Outcome::Sick;
}

constexpr std::uint8_t OutcomeBit(Outcome outcome)
{
    return static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(outcome));
}

constexpr std::uint8_t NO_OUTCOME = 0;
constexpr std::uint8_t EVERY_OUTCOME = 0xFF;

// The clean-up runs at every end of a status that has one (only errands and travel do), except for today's gaps,
// kept so that CS14 changes behaviour only where its plan names it. Errands: not on Interrupted (gap E1) or Ordered
// (gap E2); a stuck errand walk cleans up (CS14d's change 1). Travel: not on Interrupted (gap T1).
constexpr std::uint8_t ERRANDS_CLEAN_UP =
    EVERY_OUTCOME & ~(OutcomeBit(Outcome::Interrupted) | OutcomeBit(Outcome::Ordered));
constexpr std::uint8_t TRAVEL_CLEAN_UP = EVERY_OUTCOME & ~OutcomeBit(Outcome::Interrupted);

// One row per status. durationIsNormalEnd: TimedOut is the status's normal end, not a failure to do its work.
struct StatusRow
{
    std::uint8_t cleansUp;
    bool durationIsNormalEnd;
};

inline constexpr StatusRow STATUS_ROWS[] = {
    {NO_OUTCOME, false},        // idle
    {NO_OUTCOME, false},        // go grind
    {NO_OUTCOME, false},        // go camp
    {NO_OUTCOME, true},         // wander random
    {NO_OUTCOME, true},         // wander npc
    {NO_OUTCOME, false},        // do quest
    {NO_OUTCOME, false},        // travel flight
    {NO_OUTCOME, true},         // rest
    {NO_OUTCOME, true},         // outdoor PvP
    {NO_OUTCOME, false},        // do gather
    {TRAVEL_CLEAN_UP, false},   // travel
    {ERRANDS_CLEAN_UP, false},  // errands
};
static_assert(sizeof(STATUS_ROWS) / sizeof(STATUS_ROWS[0]) == STATUS_COUNT, "every status has a row");

// Whether ending `status` with `outcome` runs the status's own clean-up before the status is replaced.
constexpr bool CleansUp(std::uint32_t status, Outcome outcome)
{
    return status < STATUS_COUNT && (STATUS_ROWS[status].cleansUp & OutcomeBit(outcome)) != 0;
}

// Whether a TimedOut end of `status` is its normal end (wander random, wander npc, rest, outdoor PvP).
constexpr bool DurationIsNormalEnd(std::uint32_t status)
{
    return status < STATUS_COUNT && STATUS_ROWS[status].durationIsNormalEnd;
}

// The reset rule: a reset ends the status as Interrupted, except that a trip keeps its status across a far teleport.
constexpr bool KeepsStatus(bool farTeleport, bool isTravel) { return farTeleport && isTravel; }

// A trip's abort source when its end names none: "abort:" and the outcome's word (the Travel event's source; after the
// prefix, the reason of the trip's abort decision and of the server-log line)
constexpr char const* TripAbortSource(Outcome outcome)
{
    switch (outcome)
    {
        case Outcome::Arrived:
            return "abort:arrived";
        case Outcome::Finished:
            return "abort:finished";
        case Outcome::TimedOut:
            return "abort:timeout";
        case Outcome::Stuck:
            return "abort:stuck";
        case Outcome::Abandoned:
            return "abort:abandoned";
        case Outcome::Interrupted:
            return "abort:interrupted";
        case Outcome::Ordered:
            return "abort:order";
        case Outcome::Sick:
            return "abort:sick";
    }
    return "abort:abandoned";
}

// Whether a trip's abort line goes to the server log at INFO: a trip to a named place, and every ordered end (a test
// reads a GM's hold or stop from the log); any other at DEBUG
constexpr bool TripAbortLogsInfo(bool hasPlace, Outcome outcome) { return hasPlace || outcome == Outcome::Ordered; }
}  // namespace StatusEnd

#endif
