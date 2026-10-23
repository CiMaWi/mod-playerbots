/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTPROFILEMGR_H
#define PLAYERBOTS_BOTPROFILEMGR_H

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "Define.h"

class ChatHandler;

struct BotProfile
{
    // Hidden PvP skill, 1-99; 0 only in a profile that was never rolled
    uint8 Skill = 0;
    // Roll counter: 1 = the first roll, a reroll adds one (also salts the roll)
    uint32 Version = 1;
};

// The hidden per-bot profile (Playerbots.Skill.Enable). All rows are loaded with one query at startup (or at the first
// use), bot logins read only the in-memory map, new and changed rows are written asynchronously. Only the GM command
// `.playerbots skill` and the PvP harness records show a bot's skill.
class BotProfileMgr
{
public:
    static BotProfileMgr& instance()
    {
        static BotProfileMgr instance;
        return instance;
    }

    static constexpr uint8 MinSkill = 1;
    static constexpr uint8 MaxSkill = 99;

    // Startup: loads every profile (nothing when Playerbots.Skill.Enable is 0)
    void Load();
    // Bot login: the bot's skill; rolls and saves a new profile when the bot has none
    uint8 LoadOrCreate(uint32 guid);
    std::optional<BotProfile> Find(uint32 guid);
    // Sets the skill (clamped to 1-99) and saves the profile
    BotProfile SetSkill(uint32 guid, uint8 skill);
    // Rolls again with the next version and saves the profile
    BotProfile Reroll(uint32 guid);
    // Count, mean, sd, min, max and a histogram in steps of 10
    std::string Summary();

    // Normal distribution, mean 50, sd 15, rounded and clamped to 1-99, seeded from guid and version only
    static uint8 Roll(uint32 guid, uint32 version);

    // .playerbots skill [<bot name|guid> [set <1-99> | reroll]]
    static bool HandleSkillCommand(ChatHandler* handler, char const* args);

private:
    BotProfileMgr() = default;

    // The callers hold _lock
    void EnsureLoaded();
    void Save(uint32 guid, BotProfile const& profile);
    std::string SummaryLocked() const;

    std::mutex _lock;
    std::unordered_map<uint32, BotProfile> _profiles;
    bool _loaded = false;
    uint32 _created = 0;
};

#endif
