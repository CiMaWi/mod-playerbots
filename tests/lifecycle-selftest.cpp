/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the lifecycle rules (src/Bot/LifecycleRules.h): one case per cell of the table of events by
// kind of random bot (the consolidation plan's bot-kind design, 4.1, plus upstream's bot), the eight console commands
// for each kind, creation due for a new character, a bot made at a level and an established bot, the cohort by share
// and the kind by its inputs.
#include <iostream>
#include <string>

#include "LifecycleRules.h"

using LifecycleRules::ConsoleCommand;
using LifecycleRules::Event;
using LifecycleRules::Kind;
using LifecycleRules::Outcome;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << name << "\n";
}

// The outcome is the same whatever creation due and the level say
bool Always(Kind kind, Event event, Outcome outcome)
{
    for (bool creationDue : {false, true})
        for (bool belowMin : {false, true})
            if (LifecycleRules::WhenDue(kind, event, creationDue, belowMin) != outcome)
                return false;
    return true;
}

bool LoginRandomize(Kind kind, bool creationDue) { return LifecycleRules::AtLogin(kind, creationDue).randomize; }
bool LoginTeleport(Kind kind, bool creationDue) { return LifecycleRules::AtLogin(kind, creationDue).teleport; }
bool LoginCohort(Kind kind, bool creationDue) { return LifecycleRules::AtLogin(kind, creationDue).cohort; }

bool LoginNever(bool (*field)(Kind, bool), Kind kind) { return !field(kind, false) && !field(kind, true); }
bool LoginAlways(bool (*field)(Kind, bool), Kind kind) { return field(kind, false) && field(kind, true); }

void CheckTable()
{
    using LifecycleRules::WhenDue;

    // Persistent bot outside the cohort: randomized once, as its creation; never teleported, revived or refreshed
    Kind kind = Kind::PersistentOutsideCohort;
    Check(LoginRandomize(kind, true) && !LoginRandomize(kind, false) &&
              WhenDue(kind, Event::Randomize, true, false) == Outcome::Creation &&
              WhenDue(kind, Event::Randomize, false, false) == Outcome::None,
          "persistent outside: randomize once, as its creation");
    Check(LoginNever(LoginTeleport, kind) && Always(kind, Event::Teleport, Outcome::None),
          "persistent outside: teleport never");
    Check(Always(kind, Event::Death, Outcome::ReleaseAndRunBack), "persistent outside: dead and revive never");
    Check(LoginNever(LoginCohort, kind) && Always(kind, Event::Cohort, Outcome::None),
          "persistent outside: cohort never");

    // Persistent bot in the handout cohort: the same, and the cohort refresh at the teleport interval
    kind = Kind::PersistentInCohort;
    Check(LoginRandomize(kind, true) && !LoginRandomize(kind, false) &&
              WhenDue(kind, Event::Randomize, true, false) == Outcome::Creation &&
              WhenDue(kind, Event::Randomize, false, false) == Outcome::None,
          "persistent cohort: randomize once, as its creation");
    Check(LoginNever(LoginTeleport, kind) && Always(kind, Event::Teleport, Outcome::None),
          "persistent cohort: teleport never");
    Check(Always(kind, Event::Death, Outcome::ReleaseAndRunBack), "persistent cohort: dead and revive never");
    Check(LoginAlways(LoginCohort, kind) && Always(kind, Event::Cohort, Outcome::CohortRefresh),
          "persistent cohort: cohort refresh, no teleport");

    // Arena server: never randomized; teleport and revive as upstream
    kind = Kind::ArenaServer;
    Check(LoginNever(LoginRandomize, kind) && Always(kind, Event::Randomize, Outcome::None),
          "arena server: randomize never");
    Check(LoginAlways(LoginTeleport, kind) && Always(kind, Event::Teleport, Outcome::RefreshAndTeleport),
          "arena server: teleport as today");
    Check(Always(kind, Event::Death, Outcome::ReviveByEvents), "arena server: dead and revive as today");
    Check(LoginNever(LoginCohort, kind) && Always(kind, Event::Cohort, Outcome::None), "arena server: cohort never");

    // Arena lab: the login schedules as upstream; at or above the minimum level nothing runs but the revive in
    // place, below it a new character gets upstream's first randomize
    kind = Kind::ArenaLab;
    Check(LoginAlways(LoginRandomize, kind) && WhenDue(kind, Event::Randomize, false, true) == Outcome::Randomize &&
              WhenDue(kind, Event::Randomize, true, true) == Outcome::Randomize &&
              WhenDue(kind, Event::Randomize, false, false) == Outcome::None &&
              WhenDue(kind, Event::Randomize, true, false) == Outcome::None,
          "arena lab: randomize only a new character below the minimum level");
    Check(WhenDue(kind, Event::Teleport, false, false) == Outcome::None &&
              WhenDue(kind, Event::Teleport, true, false) == Outcome::None,
          "arena lab: teleport never at the minimum level");
    Check(WhenDue(kind, Event::Death, false, false) == Outcome::ReviveInPlace &&
              WhenDue(kind, Event::Death, false, true) == Outcome::ReviveByEvents,
          "arena lab: revived where it stands");
    Check(LoginNever(LoginCohort, kind) && Always(kind, Event::Cohort, Outcome::None), "arena lab: cohort never");

    // Upstream's bot: upstream's lifecycle
    kind = Kind::Upstream;
    Check(LoginAlways(LoginRandomize, kind) && Always(kind, Event::Randomize, Outcome::Randomize),
          "upstream: randomize and reschedule");
    Check(LoginAlways(LoginTeleport, kind) && Always(kind, Event::Teleport, Outcome::RefreshAndTeleport),
          "upstream: refresh and teleport for the level");
    Check(Always(kind, Event::Death, Outcome::ReviveByEvents), "upstream: dead and revive");
    Check(LoginNever(LoginCohort, kind) && Always(kind, Event::Cohort, Outcome::None), "upstream: cohort never");
}

void CheckConsole()
{
    struct Name
    {
        char const* name;
        ConsoleCommand command;
    };
    static Name const NAMES[] = {
        {"init", ConsoleCommand::Init},         {"level", ConsoleCommand::Level},
        {"levelup", ConsoleCommand::LevelUp},   {"refresh", ConsoleCommand::Refresh},
        {"teleport", ConsoleCommand::Teleport}, {"grind", ConsoleCommand::Grind},
        {"revive", ConsoleCommand::Revive},     {"change_strategy", ConsoleCommand::ChangeStrategy}};
    struct KindName
    {
        char const* name;
        Kind kind;
    };
    static KindName const KINDS[] = {{"persistent outside", Kind::PersistentOutsideCohort},
                                     {"persistent cohort", Kind::PersistentInCohort},
                                     {"arena server", Kind::ArenaServer},
                                     {"arena lab", Kind::ArenaLab},
                                     {"upstream", Kind::Upstream}};

    for (Name const& entry : NAMES)
    {
        ConsoleCommand command = ConsoleCommand::Init;
        bool const found = LifecycleRules::ConsoleCommandOf(entry.name, command);
        Check(found && command == entry.command, std::string("console name ") + entry.name);

        for (KindName const& kind : KINDS)
        {
            bool expected = !LifecycleRules::IsPersistent(kind.kind);
            if (entry.command == ConsoleCommand::Refresh && kind.kind == Kind::PersistentInCohort)
                expected = true;
            Check(LifecycleRules::ConsoleAllowed(entry.command, kind.kind) == expected,
                  std::string("console ") + entry.name + ", " + kind.name);
        }
    }

    ConsoleCommand command = ConsoleCommand::Init;
    Check(!LifecycleRules::ConsoleCommandOf("clear", command), "console clear is not a lifecycle command");
    Check(!LifecycleRules::ConsoleCommandOf("p", command), "console prefix p is not a command");
}

void CheckCreation()
{
    Check(LifecycleRules::CreationDue(false, 0), "creation due: a new character");
    Check(LifecycleRules::CreationDue(false, 3599), "creation due: a character below an hour played");
    Check(LifecycleRules::CreationDue(true, 0), "creation due: a bot made at a level");
    Check(LifecycleRules::CreationDue(true, 7200), "creation due: a bot made at a level, played time aside");
    Check(!LifecycleRules::CreationDue(false, 3600), "no creation: an hour played");
    Check(!LifecycleRules::CreationDue(false, 500000), "no creation: an established bot");
}

void CheckRandomizeAllowed()
{
    uint32_t const played = StartingHistory::NEW_CHARACTER_PLAYED_SECONDS;
    Check(!LifecycleRules::RandomizeAllowed(false, true, played), "randomize: not for an established keeper");
    Check(!LifecycleRules::RandomizeAllowed(false, true, played * 100), "randomize: also not after a long life");
    Check(LifecycleRules::RandomizeAllowed(false, true, 0), "randomize: a keeper's creation");
    Check(LifecycleRules::RandomizeAllowed(false, true, played - 1), "randomize: the first hour counts as creation");
    Check(LifecycleRules::RandomizeAllowed(true, true, played), "randomize: a full init (operator command)");
    Check(LifecycleRules::RandomizeAllowed(false, false, played), "randomize: a bot that keeps nothing");
}

void CheckCohortAndKind()
{
    uint32_t in = 0;
    uint32_t out = 0;
    for (uint32_t guid = 1; guid < 2000; ++guid)
    {
        if (BotShare::ShareSlot(guid) < 50)
            in = in ? in : guid;
        else
            out = out ? out : guid;
    }

    Check(in && LifecycleRules::InCohort(true, in, 50), "cohort: a persistent bot in the share");
    Check(out && !LifecycleRules::InCohort(true, out, 50), "cohort: a persistent bot outside the share");
    Check(!LifecycleRules::InCohort(false, in, 50), "cohort: no bot that is not persistent");
    Check(!LifecycleRules::InCohort(true, in, 0), "cohort: share 0 holds none");
    Check(LifecycleRules::InCohort(true, out, 100), "cohort: share 100 holds every persistent bot");

    Check(LifecycleRules::KindOf(true, false, false, false) == Kind::PersistentOutsideCohort, "kind: persistent");
    Check(LifecycleRules::KindOf(true, true, false, false) == Kind::PersistentInCohort, "kind: persistent, cohort");
    Check(LifecycleRules::KindOf(false, true, false, true) == Kind::ArenaServer, "kind: arena server");
    Check(LifecycleRules::KindOf(false, false, true, false) == Kind::ArenaLab, "kind: arena lab");
    Check(LifecycleRules::KindOf(false, false, false, false) == Kind::Upstream, "kind: upstream");
}
}  // namespace

int main()
{
    CheckTable();
    CheckConsole();
    CheckCreation();
    CheckRandomizeAllowed();
    CheckCohortAndKind();

    std::cout << "lifecycle selftest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
