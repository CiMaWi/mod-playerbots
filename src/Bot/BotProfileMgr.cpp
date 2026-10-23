/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotProfileMgr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "SkillStrategy.h"
#include "StringConvert.h"
#include "StringFormat.h"

namespace
{
constexpr uint64 SkillSeedSalt = 0x5B07A1E5C0FFEE11ULL;
constexpr double SkillMean = 50.0;
constexpr double SkillDeviation = 15.0;
constexpr double Pi = 3.14159265358979323846;

// splitmix64: a fixed integer mix, so a guid gives the same skill on every build and standard library
uint64 SplitMix64(uint64& state)
{
    state += 0x9E3779B97F4A7C15ULL;
    uint64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Uniform in [0, 1) from the top 53 bits
double Uniform(uint64& state) { return static_cast<double>(SplitMix64(state) >> 11) / 9007199254740992.0; }
}  // namespace

uint8 BotProfileMgr::Roll(uint32 guid, uint32 version)
{
    // Box-Muller on two seeded uniforms; std::normal_distribution differs between standard libraries
    uint64 state = ((static_cast<uint64>(guid) << 32) | version) ^ SkillSeedSalt;
    double const u1 = 1.0 - Uniform(state);  // (0, 1], so the log is finite
    double const u2 = Uniform(state);
    double const z = std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * Pi * u2);
    long const skill = std::lround(SkillMean + SkillDeviation * z);
    return static_cast<uint8>(std::clamp<long>(skill, MinSkill, MaxSkill));
}

void BotProfileMgr::Load()
{
    if (!sPlayerbotAIConfig.SkillEnable)
        return;

    std::lock_guard<std::mutex> guard(_lock);
    EnsureLoaded();
}

void BotProfileMgr::EnsureLoaded()
{
    if (_loaded)
        return;

    _loaded = true;
    // One query for all bots (startup or first use, world thread), so a bot login reads only the map
    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_BOT_PROFILE_ALL);
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            BotProfile profile;
            profile.Skill = std::clamp<uint8>(fields[1].Get<uint8>(), MinSkill, MaxSkill);
            profile.Version = fields[2].Get<uint32>();
            _profiles[fields[0].Get<uint32>()] = profile;
        } while (result->NextRow());
    }

    LOG_INFO("playerbots", "Bot profiles loaded: {}", SummaryLocked());
}

void BotProfileMgr::Save(uint32 guid, BotProfile const& profile)
{
    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_BOT_PROFILE);
    stmt->SetData(0, guid);
    stmt->SetData(1, profile.Skill);
    stmt->SetData(2, profile.Version);
    stmt->SetData(3, profile.Skill);
    stmt->SetData(4, profile.Version);
    PlayerbotsDatabase.Execute(stmt);
}

uint8 BotProfileMgr::LoadOrCreate(uint32 guid)
{
    std::lock_guard<std::mutex> guard(_lock);
    EnsureLoaded();
    auto const it = _profiles.find(guid);
    if (it != _profiles.end())
        return it->second.Skill;

    BotProfile profile;
    profile.Skill = Roll(guid, profile.Version);
    _profiles[guid] = profile;
    Save(guid, profile);
    // One summary line per 100 new profiles, so a first start shows the distribution in the log
    if (++_created % 100 == 0)
        LOG_INFO("playerbots", "Bot profiles after {} new this run: {}", _created, SummaryLocked());

    return profile.Skill;
}

std::optional<BotProfile> BotProfileMgr::Find(uint32 guid)
{
    std::lock_guard<std::mutex> guard(_lock);
    EnsureLoaded();
    auto const it = _profiles.find(guid);
    if (it == _profiles.end())
        return std::nullopt;

    return it->second;
}

BotProfile BotProfileMgr::SetSkill(uint32 guid, uint8 skill)
{
    std::lock_guard<std::mutex> guard(_lock);
    EnsureLoaded();
    BotProfile& profile = _profiles[guid];
    profile.Skill = std::clamp(skill, MinSkill, MaxSkill);
    Save(guid, profile);
    return profile;
}

BotProfile BotProfileMgr::Reroll(uint32 guid)
{
    std::lock_guard<std::mutex> guard(_lock);
    EnsureLoaded();
    auto const [it, inserted] = _profiles.try_emplace(guid);
    if (!inserted)
        ++it->second.Version;

    it->second.Skill = Roll(guid, it->second.Version);
    Save(guid, it->second);
    return it->second;
}

std::string BotProfileMgr::Summary()
{
    std::lock_guard<std::mutex> guard(_lock);
    EnsureLoaded();
    return SummaryLocked();
}

std::string BotProfileMgr::SummaryLocked() const
{
    if (_profiles.empty())
        return "count 0";

    std::array<uint32, 10> buckets{};
    double sum = 0.0;
    double sumSquares = 0.0;
    uint32 low = MaxSkill;
    uint32 high = MinSkill;
    for (auto const& [guid, profile] : _profiles)
    {
        uint32 const skill = profile.Skill;
        sum += skill;
        sumSquares += static_cast<double>(skill) * skill;
        low = std::min(low, skill);
        high = std::max(high, skill);
        ++buckets[std::min<size_t>(skill / 10, buckets.size() - 1)];
    }

    double const count = static_cast<double>(_profiles.size());
    double const mean = sum / count;
    double const deviation = std::sqrt(std::max(0.0, sumSquares / count - mean * mean));
    std::string histogram;
    for (size_t i = 0; i < buckets.size(); ++i)
        histogram += Acore::StringFormat("{}{}-{}: {}", i ? ", " : "", i ? i * 10 : 1, i * 10 + 9, buckets[i]);

    return Acore::StringFormat("count {}, mean {:.1f}, sd {:.1f}, min {}, max {}; {}", _profiles.size(), mean,
                               deviation, low, high, histogram);
}

bool BotProfileMgr::HandleSkillCommand(ChatHandler* handler, char const* args)
{
    if (!sPlayerbotAIConfig.SkillEnable)
    {
        handler->PSendSysMessage("Bot skill is off (Playerbots.Skill.Enable = 0).");
        return true;
    }

    std::istringstream in(args ? args : "");
    std::string target;
    std::string verb;
    std::string value;
    in >> target >> verb >> value;
    BotProfileMgr& mgr = instance();
    if (target.empty())
    {
        handler->PSendSysMessage("Bot profiles: {}", mgr.Summary());
        return true;
    }

    ObjectGuid guid = ObjectGuid::Empty;
    std::string name = target;
    if (std::optional<uint32> const number = Acore::StringTo<uint32>(target))
        guid = ObjectGuid::Create<HighGuid::Player>(*number);
    else if (normalizePlayerName(name))
        guid = sCharacterCache->GetCharacterGuidByName(name);

    if (guid.IsEmpty() || !sCharacterCache->GetCharacterNameByGuid(guid, name))
    {
        handler->PSendSysMessage("No character {}.", target);
        return false;
    }

    // Looked up per command; the pointers are not kept
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
    if (player && !botAI)
    {
        handler->PSendSysMessage("{} is not a bot.", name);
        return false;
    }

    uint32 const low = guid.GetCounter();
    if (botAI)
        mgr.LoadOrCreate(low);
    else if (!mgr.Find(low))
    {
        handler->PSendSysMessage("{} has no profile yet (it is created at the bot's login).", name);
        return false;
    }

    BotProfile profile;
    if (verb.empty())
        profile = *mgr.Find(low);
    else if (verb == "set")
    {
        std::optional<uint32> const skill = Acore::StringTo<uint32>(value);
        if (!skill || *skill < MinSkill || *skill > MaxSkill)
        {
            handler->PSendSysMessage("Usage: .playerbots skill <bot> set <{}-{}>", MinSkill, MaxSkill);
            return false;
        }

        profile = mgr.SetSkill(low, static_cast<uint8>(*skill));
    }
    else if (verb == "reroll")
        profile = mgr.Reroll(low);
    else
    {
        handler->PSendSysMessage("Usage: .playerbots skill [<bot> [set <{}-{}> | reroll]]", MinSkill, MaxSkill);
        return false;
    }

    if (botAI)
        botAI->SetPvpSkill(profile.Skill);

    handler->PSendSysMessage("{} (guid {}, {}): skill {}, roll {}", name, low, botAI ? "online" : "offline",
                             static_cast<uint32>(profile.Skill), profile.Version);
    if (botAI && sPlayerbotAIConfig.SkillNoticeActive)
    {
        // Multiplier verdicts since the skill was last set, not executed casts
        SkillNoticeCounters const& counters = botAI->GetSkillNoticeCounters();
        std::string verdicts;
        for (size_t i = 0; i < SkillNoticeCounters::Categories; ++i)
            verdicts += Acore::StringFormat("{}{} {}/{}", i ? ", " : "",
                                            SkillNoticeCategoryName(static_cast<SkillNoticeCategory>(i + 1)),
                                            counters.blocked[i], counters.allowed[i]);

        handler->PSendSysMessage("Notice: miss {}%, delay up to {} ms; blocked/allowed: {}",
                                 botAI->GetSkillNoticeMissPercent(), botAI->GetSkillNoticeDelayMs(), verdicts);
    }

    return true;
}
