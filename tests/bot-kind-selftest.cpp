/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the bot-kind facts (src/Bot/BotKindRules.h): the truth table of IsPersistentBot and
// KeepsPossessions over the instance's role, whether the bot is a random bot, and who its master is.
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
    bool persistentKey;
    bool arenaRole;
    bool arenaOnly;
    bool living;  // the expected LivingWorld
};

// Every role the facts see: a living world, the arena role, an ArenaOnly lab, and a world without the key (until CS23)
Role const ROLES[] = {
    {"living world", true, false, false, true},         {"arena role", true, true, false, false},
    {"ArenaOnly lab", true, false, true, false},        {"arena role and ArenaOnly", true, true, true, false},
    {"no persistence key", false, false, false, false},
};

struct Master
{
    char const* name;
    MasterKind kind;
    bool keeps;  // the expected KeepsPossessions of a random bot of a living world
};

// The selfbot row is the one CS17d flips: a person sits at a selfbot's client, so it will count as a master
Master const MASTERS[] = {
    {"no master", MasterKind::None, true},
    {"a bot", MasterKind::Bot, true},
    {"a real player", MasterKind::RealPlayer, false},
    {"a selfbot", MasterKind::SelfBot, true},
};
}  // namespace

int main()
{
    for (Role const& role : ROLES)
    {
        bool const living = BotKindRules::LivingWorld(role.persistentKey, role.arenaRole, role.arenaOnly);
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

    Check(!BotKindRules::MasterCounts(MasterKind::None), "no master does not count");
    Check(!BotKindRules::MasterCounts(MasterKind::Bot), "a bot master does not count");
    Check(BotKindRules::MasterCounts(MasterKind::RealPlayer), "a real player counts");
    Check(!BotKindRules::MasterCounts(MasterKind::SelfBot), "a selfbot master does not count yet (CS17d)");

    std::cout << "bot-kind selftest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
