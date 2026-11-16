/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the bot-kind facts (src/Bot/BotKindRules.h): the truth table of IsPersistentBot and
// KeepsPossessions over the instance's role, whether the bot is a random bot, and who its master is; and
// FollowsTargets for every kind of instance we run.
#include <iostream>
#include <string>

#include "BotKindRules.h"

using BotKindRules::MasterKind;

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

struct Role
{
    char const* name;
    bool arenaRole;
    bool arenaOnly;
    bool living;  // the expected LivingWorld
};

// Every role the facts see: a living world, the arena role and an ArenaOnly lab
Role const ROLES[] = {
    {"living world", false, false, true},
    {"arena role", true, false, false},
    {"ArenaOnly lab", false, true, false},
    {"arena role and ArenaOnly", true, true, false},
};

// The instances and what their role values give. followKey is Playerbots.Population.FollowTargets as the instance's
// files leave it (conf.dist: 1)
struct Instance
{
    char const* name;
    bool arenaRole;
    bool arenaOnly;
    bool followKey;
    bool living;   // the expected LivingWorld
    bool follows;  // the expected FollowsTargets
};

Instance const INSTANCES[] = {
    {"main (no line: the dist default 1)", false, false, true, true, true},
    {"lab5 (lab5.conf sets 1 over world.conf's 0)", false, false, true, true, true},
    {"a world lab (world.conf sets 0)", false, false, false, true, false},
    {"the arena server (the arena role, no line: forced off)", true, false, true, false, false},
    {"an arena lab (ArenaOnly, lab.conf sets 0)", false, true, false, false, false},
    {"an arena lab running a world scenario (ArenaOnly 0, lab.conf's 0)", false, false, false, true, false},
    {"an arena lab running a world scenario that sets 1 (h23-func)", false, false, true, true, true},
    {"an ArenaOnly lab with the key 1 (forced off)", false, true, true, false, false},
    {"the arena role and ArenaOnly with the key 1 (forced off)", true, true, true, false, false},
    {"the arena role with the key 0", true, false, false, false, false},
};

struct Master
{
    char const* name;
    MasterKind kind;
    bool keeps;  // the expected KeepsPossessions of a random bot of a living world
};

// The selfbot row is the one CS17d flipped: a person sits at a selfbot's client, so it counts as a master
Master const MASTERS[] = {
    {"no master", MasterKind::None, true},
    {"a bot", MasterKind::Bot, true},
    {"a real player", MasterKind::RealPlayer, false},
    {"a selfbot", MasterKind::SelfBot, false},
};
}  // namespace

int main()
{
    for (Role const& role : ROLES)
    {
        bool const living = BotKindRules::LivingWorld(role.arenaRole, role.arenaOnly);
        Check(living == role.living, std::string(role.name) + ": LivingWorld");

        for (bool const randomBot : {true, false})
        {
            bool const persistent = BotKindRules::IsPersistentBot(living, randomBot);
            std::string const cell = std::string(role.name) + (randomBot ? ", random bot" : ", not a random bot");
            // A and B: no master term
            Check(persistent == (role.living && randomBot), cell + ": IsPersistentBot");

            for (Master const& master : MASTERS)
            {
                bool const keeps = BotKindRules::KeepsPossessions(persistent, master.kind);
                bool const expected = role.living && randomBot && master.keeps;
                Check(keeps == expected, cell + ", master " + master.name + ": KeepsPossessions");
            }
        }
    }

    for (Instance const& instance : INSTANCES)
    {
        bool const living = BotKindRules::LivingWorld(instance.arenaRole, instance.arenaOnly);
        Check(living == instance.living, std::string(instance.name) + ": LivingWorld");
        Check(BotKindRules::FollowsTargets(living, instance.followKey) == instance.follows,
              std::string(instance.name) + ": FollowsTargets");
    }

    Check(!BotKindRules::MasterCounts(MasterKind::None), "no master does not count");
    Check(!BotKindRules::MasterCounts(MasterKind::Bot), "a bot master does not count");
    Check(BotKindRules::MasterCounts(MasterKind::RealPlayer), "a real player counts");
    Check(BotKindRules::MasterCounts(MasterKind::SelfBot), "a selfbot master counts (CS17d)");

    std::cout << "bot-kind selftest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
