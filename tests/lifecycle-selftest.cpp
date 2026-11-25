/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the lifecycle rules (src/Bot/LifecycleRules.h): one case per cell of the table of events by
// kind of random bot (the consolidation plan's bot-kind design, 4.1, plus upstream's bot), the eight console commands
// for each kind, creation due for a new character, a bot made at a level and an established bot, and the kind by its
// inputs.
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

bool LoginNever(bool (*field)(Kind, bool), Kind kind) { return !field(kind, false) && !field(kind, true); }
bool LoginAlways(bool (*field)(Kind, bool), Kind kind) { return field(kind, false) && field(kind, true); }

void CheckTable()
{
    using LifecycleRules::WhenDue;

    // Persistent bot: randomized once, as its creation; never teleported, revived or refreshed
    Kind kind = Kind::Persistent;
    Check(LoginRandomize(kind, true) && !LoginRandomize(kind, false) &&
              WhenDue(kind, Event::Randomize, true, false) == Outcome::Creation &&
              WhenDue(kind, Event::Randomize, false, false) == Outcome::None,
          "persistent: randomize once, as its creation");
    Check(LoginNever(LoginTeleport, kind) && Always(kind, Event::Teleport, Outcome::None),
          "persistent: teleport never");
    Check(Always(kind, Event::Death, Outcome::ReleaseAndRunBack), "persistent: dead and revive never");

    // Arena server: never randomized; teleport and revive as upstream
    kind = Kind::ArenaServer;
    Check(LoginNever(LoginRandomize, kind) && Always(kind, Event::Randomize, Outcome::None),
          "arena server: randomize never");
    Check(LoginAlways(LoginTeleport, kind) && Always(kind, Event::Teleport, Outcome::RefreshAndTeleport),
          "arena server: teleport as today");
    Check(Always(kind, Event::Death, Outcome::ReviveByEvents), "arena server: dead and revive as today");

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

    // Upstream's bot: upstream's lifecycle
    kind = Kind::Upstream;
    Check(LoginAlways(LoginRandomize, kind) && Always(kind, Event::Randomize, Outcome::Randomize),
          "upstream: randomize and reschedule");
    Check(LoginAlways(LoginTeleport, kind) && Always(kind, Event::Teleport, Outcome::RefreshAndTeleport),
          "upstream: refresh and teleport for the level");
    Check(Always(kind, Event::Death, Outcome::ReviveByEvents), "upstream: dead and revive");
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
    static KindName const KINDS[] = {{"persistent", Kind::Persistent},
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
            // every command refused for a persistent bot (refresh too), every one run for the other kinds
            bool const expected = kind.kind != Kind::Persistent;
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
    using LifecycleRules::CreationStep;
    using LifecycleRules::CreationStepOf;
    Check(LifecycleRules::CreationDue(false, false, 0), "creation due: a new character");
    Check(LifecycleRules::CreationDue(false, false, 3599), "creation due: a character below an hour played");
    Check(LifecycleRules::CreationDue(false, true, 0), "creation due: a bot made at a level");
    Check(!LifecycleRules::CreationDue(false, false, 3600), "no creation: an hour played");
    Check(!LifecycleRules::CreationDue(false, false, 500000), "no creation: an established bot");

    // Upkeep WU35, the night's candidates, each against CreationDue and WhenDue (before WU35 the first two gave
    // Creation: the mark held whatever the played time, and a new character's creation repeated when its randomize
    // event ran out)
    auto const outcome = [](bool due)
    { return LifecycleRules::WhenDue(LifecycleRules::Kind::Persistent, LifecycleRules::Event::Randomize, due, false); };
    // marked made_at_level, 2 h played: the mark is dropped (a decision row), no creation, at once
    Check(CreationStepOf(false, true, 7200) == CreationStep::DropMark, "WU35: a played marked bot drops its mark");
    Check(outcome(LifecycleRules::CreationDue(false, true, 7200)) == LifecycleRules::Outcome::None,
          "WU35: no Creation for a played character with the mark");
    Check(LifecycleRules::StepsAtOnce(CreationStepOf(false, true, 7200), true), "WU35: the drop does not wait");
    // created 10 min ago, the randomize event run out: the created mark holds, whatever the played time
    Check(CreationStepOf(true, false, 600) == CreationStep::None, "WU35: a created bot is not created again");
    Check(outcome(LifecycleRules::CreationDue(true, false, 600)) == LifecycleRules::Outcome::None,
          "WU35: no Creation for a created one");
    Check(CreationStepOf(true, true, 0) == CreationStep::None, "WU35: a created bot's stale mark creates nothing");
    // no mark, played time under an hour after hours of play (lost played time): the created mark holds
    Check(!LifecycleRules::CreationDue(true, false, 1200), "WU35: lost played time does not create a created bot");
    // a played character without the mark: nothing
    Check(CreationStepOf(false, false, 7200) == CreationStep::None, "WU35: a played unmarked bot: nothing");
    Check(outcome(LifecycleRules::CreationDue(false, false, 7200)) == LifecycleRules::Outcome::None,
          "WU35: no Creation for a played character without the mark");
    // an unplayed marked bot: Creation at once (before IsBusy and the randomize event)
    Check(CreationStepOf(false, true, 0) == CreationStep::Create, "WU35: an unplayed marked bot is created");
    Check(outcome(LifecycleRules::CreationDue(false, true, 0)) == LifecycleRules::Outcome::Creation,
          "WU35: Creation for an unplayed marked one");
    Check(LifecycleRules::StepsAtOnce(CreationStepOf(false, true, 0), true), "WU35: its creation does not wait");
    // an unmarked new character waits for an idle pass (the held test bots are left alone)
    Check(!LifecycleRules::StepsAtOnce(CreationStepOf(false, false, 0), false), "WU35: an unmarked creation waits");

    // Invariant over every input: a creation only for a bot without the created mark and under an hour played; a
    // dropped mark only for a marked, played, uncreated bot; nothing at once without the mark; the factory's
    // incremental randomize of a keeper only where the manager would create it
    uint32_t const playedValues[] = {0, 1, 599, 3599, 3600, 3601, 7200, 45756, 500000};
    bool invariant = true;
    for (bool created : {false, true})
        for (bool marked : {false, true})
            for (uint32_t played : playedValues)
            {
                CreationStep const step = CreationStepOf(created, marked, played);
                if (step == CreationStep::Create && (created || played >= 3600))
                    invariant = false;
                if (step == CreationStep::DropMark && (created || !marked || played < 3600))
                    invariant = false;
                if (LifecycleRules::StepsAtOnce(step, marked) && !marked)
                    invariant = false;
                bool const factory = LifecycleRules::RandomizeAllowed(false, true, created, played);
                if (factory != (CreationStepOf(created, false, played) == CreationStep::Create))
                    invariant = false;
            }
    Check(invariant, "WU35: creation happens once (every created, mark and played input)");
}

void CheckRandomizeAllowed()
{
    uint32_t const played = StartingHistory::NEW_CHARACTER_PLAYED_SECONDS;
    Check(!LifecycleRules::RandomizeAllowed(false, true, false, played), "randomize: not for an established keeper");
    Check(!LifecycleRules::RandomizeAllowed(false, true, false, played * 100), "randomize: also not after a long life");
    Check(LifecycleRules::RandomizeAllowed(false, true, false, 0), "randomize: a keeper's creation");
    Check(LifecycleRules::RandomizeAllowed(false, true, false, played - 1),
          "randomize: the first hour counts as creation");
    Check(!LifecycleRules::RandomizeAllowed(false, true, true, 0), "randomize: not again for a created keeper (WU35)");
    Check(LifecycleRules::RandomizeAllowed(true, true, true, played), "randomize: a full init (operator command)");
    Check(LifecycleRules::RandomizeAllowed(false, false, true, played), "randomize: a bot that keeps nothing");
}

void CheckKind()
{
    Check(LifecycleRules::KindOf(true, false, false) == Kind::Persistent, "kind: persistent");
    Check(LifecycleRules::KindOf(true, true, true) == Kind::Persistent, "kind: persistent first");
    Check(LifecycleRules::KindOf(false, false, true) == Kind::ArenaServer, "kind: arena server");
    Check(LifecycleRules::KindOf(false, true, false) == Kind::ArenaLab, "kind: arena lab");
    Check(LifecycleRules::KindOf(false, true, true) == Kind::ArenaLab, "kind: arena lab before arena server");
    Check(LifecycleRules::KindOf(false, false, false) == Kind::Upstream, "kind: upstream");
}
}  // namespace

int main()
{
    CheckTable();
    CheckConsole();
    CheckCreation();
    CheckRandomizeAllowed();
    CheckKind();

    std::cout << "lifecycle selftest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
