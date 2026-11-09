/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTTRAITMGR_H
#define PLAYERBOTS_BOTTRAITMGR_H

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "Define.h"
#include "TraitRules.h"

class ChatHandler;

// Every bot's personality (travel plan WT11a, TraitRules.h), cached beside the bot profile. All rows are loaded with
// one query at startup (world thread), a bot login reads only the map, new and changed rows are written
// asynchronously. Find and Trait read the cache from any thread (map threads, planner workers) and return copies; they
// never load or query.
class BotTraitMgr
{
public:
    static BotTraitMgr& instance()
    {
        static BotTraitMgr instance;
        return instance;
    }

    // Startup, world thread: loads every row (memory only, with one WARN, when the table is missing)
    void Load();
    // Bot login, world thread: the bot's traits; rolls and saves version 1 when the bot has none
    TraitRules::BotTraits LoadOrCreate(uint32 guid, uint8 race, uint8 cls);
    // Any thread: a copy of the bot's traits, none when it has not logged in since startup
    std::optional<TraitRules::BotTraits> Find(uint32 guid);
    // Any thread: the AI's view of one trait, -1..+1; 0 when the bot has no traits
    float Trait(uint32 guid, TraitRules::Trait trait);
    // World thread. A retired bot: drops its traits from memory (the retire deletes the row)
    void Forget(uint32 guid);
    // World thread: rolls the next version (the version counts up) and saves it
    TraitRules::BotTraits Reroll(uint32 guid, uint8 race, uint8 cls);
    // World thread: sets one trait (clamped to -100..100) and saves; none when the bot has no traits yet
    std::optional<TraitRules::BotTraits> SetTrait(uint32 guid, TraitRules::Trait trait, int value);

    // .playerbots traits [<bot name|guid> [set <trait> <value> | reroll] | sample <N> | selftest]
    static bool HandleTraitsCommand(ChatHandler* handler, char const* args);

private:
    BotTraitMgr() = default;

    // The callers hold _lock exclusively
    void EnsureLoaded();
    void Save(uint32 guid, TraitRules::BotTraits const& traits) const;

    std::shared_mutex _lock;
    std::unordered_map<uint32, TraitRules::BotTraits> _traits;
    bool _loaded = false;
    // playerbots_bot_traits exists (update 2026_11_09_00); without it traits live in memory only
    bool _table = false;
};

#endif
