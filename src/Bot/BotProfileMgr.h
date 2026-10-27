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
    // Roll counter: 1 = the first roll, a reroll adds one (Playerbots.Skill.Distribution: a reroll keeps the
    // percentile)
    uint32 Version = 1;
    // The bot's fixed rank in (0, 1): the skill is this percentile mapped through Playerbots.Skill.Distribution
    double Percentile = 0.5;
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
    // Sets the skill (clamped to 1-99) and the percentile that maps to it, and saves the profile
    BotProfile SetSkill(uint32 guid, uint8 skill);
    // Re-applies the distribution to the bot's percentile (the rank stays), counts the version up, saves the profile
    BotProfile Reroll(uint32 guid);
    // The bot's percentile: its profile's, else the one its first roll gives (no profile needed)
    double Percentile(uint32 guid);
    // Count, mean, sd, min, max and a histogram in steps of 10
    std::string Summary();

    // The skill before percentiles: normal distribution, mean 50, sd 15, rounded and clamped to 1-99, seeded from
    // guid and version only
    static uint8 Roll(uint32 guid, uint32 version);
    // The percentile of the first roll (version 1): Phi(z) of Roll's z, so normal 50/15 maps it to Roll(guid, 1)
    static double LegacyPercentile(uint32 guid);
    // The standard normal CDF and its inverse (Acklam's rational approximation, two Halley steps; p clamped)
    static double NormalCdf(double z);
    static double InverseNormalCdf(double p);
    // Percentile -> skill 1-99 through a distribution (uniform: min..max; normal: mean, sd), and the current one
    static uint8 SkillFromPercentile(double percentile, bool uniform, double mean, double sd, double low, double high);
    static uint8 SkillFromPercentile(double percentile);
    // Skill -> the percentile that maps to it under the current distribution (the midpoint of the skill's step)
    static double PercentileFromSkill(uint8 skill);

    // .playerbots skill [<bot name|guid> [set <1-99> | reroll] | selftest]
    static bool HandleSkillCommand(ChatHandler* handler, char const* args);
    // .playerbots skill selftest: today's skills under the default, the order across distributions, the migration
    static bool HandleSelftest(ChatHandler* handler);

private:
    BotProfileMgr() = default;

    // The callers hold _lock
    void EnsureLoaded();
    void Save(uint32 guid, BotProfile const& profile);
    std::string SummaryLocked() const;

    std::mutex _lock;
    std::unordered_map<uint32, BotProfile> _profiles;
    bool _loaded = false;
    // playerbots_bot_profile.percentile exists (update 2026_10_27_00); without it percentiles live in memory only
    bool _percentileColumn = false;
    uint32 _created = 0;
};

#endif
