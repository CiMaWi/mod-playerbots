/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_LIFECYCLERULES_H
#define _PLAYERBOT_LIFECYCLERULES_H

#include <cstdint>
#include <string_view>

#include "../Ai/World/Rpg/StartingHistoryRules.h"
#include "../Ai/World/ShareSlot.h"

// Which timed events of the random bot manager a random bot gets (randomize, teleport, dead and revive, cohort), as
// pure rules: the server (RandomPlayerbotMgr) and tests/lifecycle-selftest.cpp run the same code. A persistent bot is
// not given upstream's lifecycle: it is randomized once, as its creation, and a bot of the handout cohort is refreshed
// at the teleport interval without a teleport. The arena kinds keep today's behaviour.
namespace LifecycleRules
{
enum class Kind : uint8_t
{
    PersistentOutsideCohort,  // a random bot of a living world (BotKind::IsPersistentBot) outside the handout share
    PersistentInCohort,       // the same, inside Playerbots.Upkeep.HandoutShare (CS.14)
    ArenaServer,              // the ladder's arena server (LadderMgr::IsHostedOnly)
    ArenaLab,                 // Playerbots.ArenaOnly.Enable
    Upstream                  // a random bot no role of ours covers: upstream's lifecycle unchanged
};

enum class Event : uint8_t
{
    Randomize,
    Teleport,  // upstream's refresh and teleport for the level
    Death,     // the bot is dead: upstream's "dead" and "revive" events
    Cohort     // the handout cohort's refresh
};

// What the manager does when an event comes due (or, for Death, when it finds the bot dead)
enum class Outcome : uint8_t
{
    None,                // nothing: the event is never set for this kind, and an old row of it is ignored
    Creation,            // the creation randomize, once; the event is set again for the randomize time as the guard
    Randomize,           // upstream: Randomize, then reschedule for the randomize time
    RefreshAndTeleport,  // upstream: Refresh, RandomTeleportForLevel, then reschedule for the teleport interval
    CohortRefresh,       // Upkeep::RefreshCohort (Refresh and one decision row), reschedule for the teleport interval
    ReviveByEvents,      // upstream: mark "dead", set "revive" for the revive time, then Revive
    ReviveInPlace,       // resurrected where it stands
    ReleaseAndRunBack    // the manager leaves the bot alone; its dead strategy releases and runs back
};

// The events the login schedules
struct LoginSchedule
{
    bool randomize;
    bool teleport;
    bool cohort;
};

constexpr bool IsPersistent(Kind kind)
{
    return kind == Kind::PersistentOutsideCohort || kind == Kind::PersistentInCohort;
}

// The persistent bot's share of the handout cohort (Upkeep::InCohort over this)
constexpr bool InCohort(bool persistentBot, uint32_t guidCounter, uint32_t sharePercent)
{
    return persistentBot && BotShare::InShare(guidCounter, sharePercent);
}

// The kinds do not overlap on our instances (a living world is neither arena role nor ArenaOnly); the order is the
// one the manager tests them in
constexpr Kind KindOf(bool persistentBot, bool inCohort, bool arenaLab, bool arenaServer)
{
    if (persistentBot)
        return inCohort ? Kind::PersistentInCohort : Kind::PersistentOutsideCohort;
    if (arenaLab)
        return Kind::ArenaLab;
    if (arenaServer)
        return Kind::ArenaServer;
    return Kind::Upstream;
}

// A persistent bot still to be created: made at a level by the population resize (its made_at_level event is set
// until RandomizeFirst runs), or a new character by played time
inline bool CreationDue(bool madeAtLevel, uint32_t playedSeconds)
{
    return madeAtLevel || StartingHistory::NewCharacter(playedSeconds);
}

constexpr LoginSchedule AtLogin(Kind kind, bool creationDue)
{
    switch (kind)
    {
        case Kind::PersistentOutsideCohort:
            return {creationDue, false, false};
        case Kind::PersistentInCohort:
            return {creationDue, false, true};
        case Kind::ArenaServer:
            return {false, true, false};
        case Kind::ArenaLab:
        case Kind::Upstream:
            return {true, true, false};
    }
    return {false, false, false};
}

// belowMinLevel: the bot is below Playerbots.RandomBotMinLevel (only an arena lab reads it: such a new character
// still gets upstream's first randomize)
constexpr Outcome WhenDue(Kind kind, Event event, bool creationDue, bool belowMinLevel)
{
    if (kind == Kind::ArenaLab && belowMinLevel)
        kind = Kind::Upstream;

    switch (event)
    {
        case Event::Randomize:
            if (IsPersistent(kind))
                return creationDue ? Outcome::Creation : Outcome::None;
            return kind == Kind::Upstream ? Outcome::Randomize : Outcome::None;
        case Event::Teleport:
            return kind == Kind::Upstream || kind == Kind::ArenaServer ? Outcome::RefreshAndTeleport : Outcome::None;
        case Event::Death:
            if (IsPersistent(kind))
                return Outcome::ReleaseAndRunBack;
            return kind == Kind::ArenaLab ? Outcome::ReviveInPlace : Outcome::ReviveByEvents;
        case Event::Cohort:
            return kind == Kind::PersistentInCohort ? Outcome::CohortRefresh : Outcome::None;
    }
    return Outcome::None;
}

// The console's "playerbots rndbot <command>" commands that randomize, refresh, teleport or revive a bot
enum class ConsoleCommand : uint8_t
{
    Init,
    Level,
    LevelUp,
    Refresh,
    Teleport,
    Grind,
    Revive,
    ChangeStrategy
};

struct ConsoleName
{
    std::string_view name;
    ConsoleCommand command;
};

constexpr ConsoleName CONSOLE_NAMES[] = {
    {"init", ConsoleCommand::Init},         {"level", ConsoleCommand::Level},
    {"levelup", ConsoleCommand::LevelUp},   {"refresh", ConsoleCommand::Refresh},
    {"teleport", ConsoleCommand::Teleport}, {"grind", ConsoleCommand::Grind},
    {"revive", ConsoleCommand::Revive},     {"change_strategy", ConsoleCommand::ChangeStrategy}};

// By the handler table's exact key; false for a command these rules do not cover
constexpr bool ConsoleCommandOf(std::string_view name, ConsoleCommand& command)
{
    for (ConsoleName const& entry : CONSOLE_NAMES)
    {
        if (entry.name == name)
        {
            command = entry.command;
            return true;
        }
    }
    return false;
}

// A persistent bot refuses every one of them, except that refresh runs the cohort path for a bot of the cohort
constexpr bool ConsoleAllowed(ConsoleCommand command, Kind kind)
{
    if (!IsPersistent(kind))
        return true;
    return command == ConsoleCommand::Refresh && kind == Kind::PersistentInCohort;
}
}  // namespace LifecycleRules

#endif
