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
#include <utility>
#include <vector>

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

// Percentiles stay inside (0, 1), so the inverse CDF is finite
constexpr double MinPercentile = 1e-15;
constexpr double MaxPercentile = 1.0 - 1e-15;

// Box-Muller on two seeded uniforms; std::normal_distribution differs between standard libraries
double RollZ(uint32 guid, uint32 version)
{
    uint64 state = ((static_cast<uint64>(guid) << 32) | version) ^ SkillSeedSalt;
    double const u1 = 1.0 - Uniform(state);  // (0, 1], so the log is finite
    double const u2 = Uniform(state);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * Pi * u2);
}

uint8 ClampSkill(double value)
{
    return static_cast<uint8>(std::clamp<long>(std::lround(value), BotProfileMgr::MinSkill, BotProfileMgr::MaxSkill));
}
}  // namespace

uint8 BotProfileMgr::Roll(uint32 guid, uint32 version)
{
    return ClampSkill(SkillMean + SkillDeviation * RollZ(guid, version));
}

double BotProfileMgr::LegacyPercentile(uint32 guid)
{
    return std::clamp(NormalCdf(RollZ(guid, 1)), MinPercentile, MaxPercentile);
}

double BotProfileMgr::NormalCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

double BotProfileMgr::InverseNormalCdf(double p)
{
    static constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,  -2.759285104469687e+02,
                                   1.383577518672690e+02,  -3.066479806614716e+01, 2.506628277459239e+00};
    static constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                                   6.680131188771972e+01, -1.328068155288572e+01};
    static constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                                   -2.549732539343734e+00, 4.374664141464968e+00,  2.938163982698783e+00};
    static constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                                   3.754408661907416e+00};
    constexpr double Low = 0.02425;

    p = std::clamp(p, MinPercentile, MaxPercentile);
    double x;
    if (p < Low || p > 1.0 - Low)
    {
        double const q = std::sqrt(-2.0 * std::log(p < Low ? p : 1.0 - p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
        if (p > 1.0 - Low)
            x = -x;
    }
    else
    {
        double const q = p - 0.5;
        double const r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
    }

    // Halley steps on the exact CDF bring the approximation (about 1e-9) to double precision
    for (int step = 0; step < 2; ++step)
    {
        double const e = NormalCdf(x) - p;
        double const u = e * std::sqrt(2.0 * Pi) * std::exp(x * x / 2.0);
        x -= u / (1.0 + x * u / 2.0);
    }

    return x;
}

uint8 BotProfileMgr::SkillFromPercentile(double percentile, bool uniform, double mean, double sd, double low,
                                         double high)
{
    double const p = std::clamp(percentile, MinPercentile, MaxPercentile);
    if (uniform)
        return ClampSkill(low + p * (high - low));

    return ClampSkill(mean + sd * InverseNormalCdf(p));
}

uint8 BotProfileMgr::SkillFromPercentile(double percentile)
{
    return SkillFromPercentile(percentile, sPlayerbotAIConfig.SkillDistributionUniform, sPlayerbotAIConfig.SkillMean,
                               sPlayerbotAIConfig.SkillSd, sPlayerbotAIConfig.SkillMin, sPlayerbotAIConfig.SkillMax);
}

double BotProfileMgr::PercentileFromSkill(uint8 skill)
{
    double p = 0.5;
    if (!sPlayerbotAIConfig.SkillDistributionUniform)
        p = NormalCdf((skill - sPlayerbotAIConfig.SkillMean) / sPlayerbotAIConfig.SkillSd);
    else if (sPlayerbotAIConfig.SkillMax > sPlayerbotAIConfig.SkillMin)
        p = (skill - sPlayerbotAIConfig.SkillMin) / (sPlayerbotAIConfig.SkillMax - sPlayerbotAIConfig.SkillMin);

    return std::clamp(p, 1e-6, 1.0 - 1e-6);
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
    // One query for all bots (startup or first use, world thread), so a bot login reads only the map. The percentile
    // column comes with an update file that a server with Playerbots.Updates.EnableDatabases = 0 may lack: the
    // prepared statements stay without it, the column is read and written only where it exists
    _percentileColumn = PlayerbotsDatabase.Query(
                            "SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = "
                            "DATABASE() AND TABLE_NAME = 'playerbots_bot_profile' AND "
                            "COLUMN_NAME = 'percentile'") != nullptr;
    std::unordered_map<uint32, double> percentiles;
    if (_percentileColumn)
    {
        if (QueryResult result = PlayerbotsDatabase.Query(
                "SELECT guid, percentile FROM playerbots_bot_profile WHERE percentile IS NOT NULL"))
        {
            do
            {
                Field* fields = result->Fetch();
                percentiles[fields[0].Get<uint32>()] = fields[1].Get<double>();
            } while (result->NextRow());
        }
    }

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_BOT_PROFILE_ALL);
    uint32 migrated = 0;
    uint32 remapped = 0;
    uint32 unsaved = 0;
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const guid = fields[0].Get<uint32>();
            BotProfile profile;
            profile.Skill = std::clamp<uint8>(fields[1].Get<uint8>(), MinSkill, MaxSkill);
            profile.Version = fields[2].Get<uint32>();
            auto const stored = percentiles.find(guid);
            bool const known = stored != percentiles.end() && stored->second > 0.0 && stored->second < 1.0;
            if (known)
                profile.Percentile = stored->second;
            else if (profile.Version == 1 && profile.Skill == Roll(guid, 1))
                profile.Percentile = LegacyPercentile(guid);  // the first roll, exactly
            else  // rerolled or set: the skill's percentile under the curve it was rolled on (normal 50/15)
                profile.Percentile =
                    std::clamp(NormalCdf((profile.Skill - SkillMean) / SkillDeviation), 1e-6, 1.0 - 1e-6);

            uint8 const skill = SkillFromPercentile(profile.Percentile);
            bool const changed = skill != profile.Skill;
            migrated += known ? 0 : 1;
            if (_percentileColumn || !changed)
            {
                remapped += changed ? 1 : 0;
                profile.Skill = skill;
            }
            else
                ++unsaved;  // no column: a remapped skill would be read back as another percentile at the next start

            _profiles[guid] = profile;
            if (_percentileColumn && (!known || changed))
                Save(guid, profile);
        } while (result->NextRow());
    }

    LOG_INFO("playerbots", "Bot profiles loaded: {}", SummaryLocked());
    std::string const shape =
        sPlayerbotAIConfig.SkillDistributionUniform
            ? Acore::StringFormat("uniform {:.0f}-{:.0f}", sPlayerbotAIConfig.SkillMin, sPlayerbotAIConfig.SkillMax)
            : Acore::StringFormat("normal mean {:.1f}, sd {:.1f}", sPlayerbotAIConfig.SkillMean,
                                  sPlayerbotAIConfig.SkillSd);
    LOG_INFO("playerbots", "Bot profiles: distribution {}, {} percentiles migrated, {} skills remapped", shape,
             _percentileColumn ? migrated : 0, remapped);
    if (!_percentileColumn)
        LOG_WARN("playerbots",
                 "Bot profiles: playerbots_bot_profile has no percentile column (update 2026_10_27_00 not applied): "
                 "percentiles are derived at each start, {} remapped skills kept at their stored value",
                 unsaved);
}

void BotProfileMgr::Save(uint32 guid, BotProfile const& profile)
{
    if (_percentileColumn)
    {
        uint32 const skill = profile.Skill;
        PlayerbotsDatabase.Execute(
            "INSERT INTO playerbots_bot_profile (guid, pvp_skill, version, percentile) VALUES "
            "({}, {}, {}, {:.17g}) ON DUPLICATE KEY UPDATE pvp_skill = {}, version = {}, "
            "percentile = {:.17g}",
            guid, skill, profile.Version, profile.Percentile, skill, profile.Version, profile.Percentile);
        return;
    }

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
    profile.Percentile = LegacyPercentile(guid);
    profile.Skill = SkillFromPercentile(profile.Percentile);
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
    profile.Percentile = PercentileFromSkill(profile.Skill);
    Save(guid, profile);
    return profile;
}

double BotProfileMgr::Percentile(uint32 guid)
{
    if (!sPlayerbotAIConfig.SkillEnable)
        return LegacyPercentile(guid);

    std::lock_guard<std::mutex> guard(_lock);
    EnsureLoaded();
    auto const it = _profiles.find(guid);
    return it != _profiles.end() ? it->second.Percentile : LegacyPercentile(guid);
}

BotProfile BotProfileMgr::Reroll(uint32 guid)
{
    std::lock_guard<std::mutex> guard(_lock);
    EnsureLoaded();
    auto const [it, inserted] = _profiles.try_emplace(guid);
    if (inserted)
        it->second.Percentile = LegacyPercentile(guid);
    else
        ++it->second.Version;

    // DD.7: the rank stays; a reroll only re-applies the current distribution
    it->second.Skill = SkillFromPercentile(it->second.Percentile);
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

bool BotProfileMgr::HandleSelftest(ChatHandler* handler)
{
    uint32 checks = 0;
    uint32 failures = 0;
    auto check = [&](bool ok, std::string const& what)
    {
        ++checks;
        if (!ok)
        {
            ++failures;
            handler->PSendSysMessage("SKILL selftest FAIL: {}", what);
        }
    };

    // 1. Today's skills: the default (normal 50/15) maps a bot's percentile to exactly its old first roll
    constexpr uint32 Sample = 20000;
    uint32 differ = 0;
    for (uint32 guid = 1; guid <= Sample; ++guid)
        differ += SkillFromPercentile(LegacyPercentile(guid), false, 50.0, 15.0, 1.0, 99.0) != Roll(guid, 1) ? 1 : 0;

    check(differ == 0,
          Acore::StringFormat("normal 50/15 gives Roll(guid, 1) for guids 1-{} ({} differ)", Sample, differ));

    // 2. The order: sorted by percentile, the skills never fall under any distribution
    std::vector<std::pair<double, uint32>> ranked;
    ranked.reserve(Sample);
    for (uint32 guid = 1; guid <= Sample; ++guid)
        ranked.emplace_back(LegacyPercentile(guid), guid);

    std::sort(ranked.begin(), ranked.end());
    struct Shape
    {
        char const* Name;
        bool Uniform;
        double Mean;
        double Sd;
        double Low;
        double High;
    };
    std::array<Shape, 5> const shapes{{{"normal 50/15", false, 50.0, 15.0, 1.0, 99.0},
                                       {"normal 60/5", false, 60.0, 5.0, 1.0, 99.0},
                                       {"normal 50/40", false, 50.0, 40.0, 1.0, 99.0},
                                       {"uniform 1-99", true, 50.0, 15.0, 1.0, 99.0},
                                       {"uniform 30-70", true, 50.0, 15.0, 30.0, 70.0}}};
    for (Shape const& shape : shapes)
    {
        uint32 inversions = 0;
        uint8 previous = MinSkill;
        for (auto const& entry : ranked)
        {
            uint8 const skill =
                SkillFromPercentile(entry.first, shape.Uniform, shape.Mean, shape.Sd, shape.Low, shape.High);
            inversions += skill < previous ? 1 : 0;
            previous = skill;
        }

        check(inversions == 0,
              Acore::StringFormat("{}: the skills keep the percentile order ({} inversions)", shape.Name, inversions));
    }

    // Uniform 1-99 spreads evenly: p 0.1 and 0.9 land near skill 11 and 89
    uint32 const tenth = SkillFromPercentile(0.1, true, 50.0, 15.0, 1.0, 99.0);
    uint32 const ninth = SkillFromPercentile(0.9, true, 50.0, 15.0, 1.0, 99.0);
    check(tenth >= 10 && tenth <= 12 && ninth >= 88 && ninth <= 90,
          Acore::StringFormat("uniform 1-99: p 0.1 -> {}, p 0.9 -> {}", tenth, ninth));

    // 3. The migration: a skill's percentile under the old curve maps back to the same skill
    uint32 back = 0;
    for (uint32 skill = MinSkill; skill <= MaxSkill; ++skill)
    {
        double const p = std::clamp(NormalCdf((skill - SkillMean) / SkillDeviation), 1e-6, 1.0 - 1e-6);
        back += SkillFromPercentile(p, false, 50.0, 15.0, 1.0, 99.0) == skill ? 1 : 0;
    }

    check(back == MaxSkill - MinSkill + 1,
          Acore::StringFormat("migration maps {} of 99 skills back to themselves", back));

    // 4. The inverse CDF is the CDF's inverse
    double worst = 0.0;
    for (int step = -600; step <= 600; ++step)
    {
        double const z = step / 100.0;
        worst = std::max(worst, std::abs(InverseNormalCdf(NormalCdf(z)) - z));
    }

    check(worst < 1e-8, Acore::StringFormat("InverseNormalCdf(NormalCdf(z)) = z within {:.2e} for z in -6..6", worst));
    handler->PSendSysMessage("SKILL selftest: {} checks, {} failed", checks, failures);
    return failures == 0;
}

bool BotProfileMgr::HandleSkillCommand(ChatHandler* handler, char const* args)
{
    if (args && std::string(args) == "selftest")
        return HandleSelftest(handler);

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
        // An actual skill change resets the notice counters; a plain read must not
        if (botAI)
            botAI->SetPvpSkill(profile.Skill);
    }
    else if (verb == "reroll")
    {
        profile = mgr.Reroll(low);
        if (botAI)
            botAI->SetPvpSkill(profile.Skill);
    }
    else
    {
        handler->PSendSysMessage(
            "Usage: .playerbots skill [<bot> [set <{}-{}> | reroll] | selftest] (a reroll "
            "keeps the bot's percentile and re-applies Playerbots.Skill.Distribution)",
            MinSkill, MaxSkill);
        return false;
    }

    handler->PSendSysMessage("{} (guid {}, {}): skill {}, percentile {:.4f}, roll {}", name, low,
                             botAI ? "online" : "offline", static_cast<uint32>(profile.Skill), profile.Percentile,
                             profile.Version);
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
