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
#include <optional>
#include <utility>

#include "StatusEndTypes.h"

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
constexpr std::uint32_t STATUS_DO_FISH = 12;
constexpr std::uint32_t STATUS_COUNT = 13;

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
    {EVERY_OUTCOME, false},     // do fish: the weapon back and the FishTrip row at every end
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

// The reset rule: a reset ends the status as Interrupted, except that a held trip keeps its holder's status across a
// far teleport.
constexpr bool KeepsStatus(bool farTeleport, bool holdsTrip) { return farTeleport && holdsTrip; }

// A reset's effect on one member group of the bot's RPG state: the group starts fresh or stays as it is
enum class ResetEffect : std::uint8_t
{
    Fresh,
    Kept
};

// The one list of what a reset (RpgStatus::Reset) does to the bot's RPG member groups: the status starts over unless
// KeepsStatus keeps it (a kept trip keeps its start); the planner's view and the RPG memos start fresh; a test's
// constraints stay. The bank and mail state has its own rule (BankMail::AfterReset: the visits end, the knowledge
// stays); the route request ids are BotTravel's, which a reset does not touch
struct ResetList
{
    ResetEffect status;
    ResetEffect planner;
    ResetEffect rpgMemos;
    ResetEffect testConstraints;
};

constexpr ResetList ResetOf(bool farTeleport, bool holdsTrip)
{
    return {KeepsStatus(farTeleport, holdsTrip) ? ResetEffect::Kept : ResetEffect::Fresh, ResetEffect::Fresh,
            ResetEffect::Fresh, ResetEffect::Kept};
}

// The errand status's deadline clock (WT64c): its age without the time its trips ran, the ended ones' (endedMs) and
// the one it holds now (heldMs): a trip held for the status counts nothing
constexpr std::uint32_t NoTripMs(std::uint32_t statusMs, std::uint32_t endedMs, std::uint32_t heldMs)
{
    std::uint64_t const tripsMs = static_cast<std::uint64_t>(endedMs) + heldMs;
    return statusMs > tripsMs ? static_cast<std::uint32_t>(statusMs - tripsMs) : 0;
}

// Applies one entry of the reset list to its group
template <class Group>
void ApplyReset(ResetEffect effect, Group& group)
{
    if (effect == ResetEffect::Fresh)
        group = Group{};
}

// The held trip (WT64f; PLAN 3.20 (3), INTERFACE 1): a status holds a trip that lives in the bot's travel block, not
// in the status data. Who acts first: the route follower while the bot's status holds the trip, the status's own
// executor otherwise (the trip has ended, or none was begun). O(1)
enum class FirstActor : std::uint8_t
{
    Holder,
    Follower
};

constexpr FirstActor WhoActsFirst(bool held, std::uint32_t holder, std::uint32_t status)
{
    return held && holder == status ? FirstActor::Follower : FirstActor::Holder;
}

// Whose end a held trip's own end is (WT64b): travel's status is its trip, so that status ends with it; any other
// holder's status goes on and reads the end once (TripHold::Finish, TakeEnd)
constexpr bool TripEndsHolder(std::uint32_t holder) { return holder == STATUS_TRAVEL; }
static_assert(TripEndsHolder(STATUS_TRAVEL) && !TripEndsHolder(STATUS_IDLE) && !TripEndsHolder(STATUS_GO_GRIND) &&
                  !TripEndsHolder(STATUS_ERRANDS),
              "only travel's status ends with its trip");

// How a held trip ended: its holder's outcome and the end's word (a literal; Note)
struct TripEnd
{
    Outcome outcome{Outcome::Interrupted};
    char const* word{nullptr};
};

// One held trip and its holder. The status core begins it with its holder's status and ends it before that status
// ends or is replaced, with the same outcome (End); a reset keeps it with its holder across a far teleport
// (KeepsStatus) and ends any other as Interrupted (Reset). A trip that ends by itself while its holder goes on
// (Finish: arrived, or a typed failure) leaves its end unread for that holder, who takes it once (TakeEnd, GoTo's
// step 3, GoToRules.h). The Player-side work (the passenger motion) is the core's.
template <class Trip>
class TripHold
{
public:
    Trip* Held() { return trip ? &*trip : nullptr; }
    Trip const* Held() const { return trip ? &*trip : nullptr; }
    std::uint32_t Holder() const { return holder; }
    TripEnd const& LastEnd() const { return lastEnd; }

    Trip& Begin(std::uint32_t status, Trip next)
    {
        trip = std::move(next);
        holder = status;
        unread = false;
        return *trip;
    }

    // The holder ends or is replaced: its trip ends with the same outcome, and nobody is left to read it. False: no
    // trip was held
    bool End(Outcome outcome, char const* word)
    {
        if (!trip)
            return false;
        lastEnd = {outcome, word};
        trip.reset();
        holder = STATUS_IDLE;
        unread = false;
        return true;
    }

    // The trip ended by itself (the follower's arrival or a failed leg); its holder's status goes on and reads the end
    // once. False: no trip was held
    bool Finish(Outcome outcome, char const* word)
    {
        std::uint32_t const by = holder;
        if (!End(outcome, word))
            return false;
        unread = true;
        endHolder = by;
        return true;
    }

    // The unread end of the trip this status held, once; nothing for another status or after the first read
    std::optional<TripEnd> TakeEnd(std::uint32_t status)
    {
        if (!unread || endHolder != status)
            return std::nullopt;
        unread = false;
        return lastEnd;
    }

    // A reset: Kept, the trip and its holder go on (a far teleport); Fresh, the holder starts over without its trip
    ResetEffect Reset(bool farTeleport)
    {
        if (KeepsStatus(farTeleport, trip.has_value()))
            return ResetEffect::Kept;
        End(Outcome::Interrupted, "reset");
        return ResetEffect::Fresh;
    }

private:
    std::optional<Trip> trip;
    std::uint32_t holder{STATUS_IDLE};
    TripEnd lastEnd;
    bool unread{false};                    // lastEnd waits for its holder (Finish)
    std::uint32_t endHolder{STATUS_IDLE};  // the status that held the finished trip
};

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
