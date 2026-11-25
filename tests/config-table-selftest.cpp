/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline selftest of the config tables (src/Bot/ConfigTable.h): every family's rows against conf/playerbots.conf.dist
// (argv[1] = the module root) and the walk driven by a fake reader.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "AcquaintanceConfig.h"
#include "ArenaCcConfig.h"
#include "ArenaCoverConfig.h"
#include "ArenaDefensiveConfig.h"
#include "ArenaFocusConfig.h"
#include "ArenaInterruptConfig.h"
#include "ArenaKiteConfig.h"
#include "ArenaOnlyConfig.h"
#include "ArenaRecruitConfig.h"
#include "ArenaTeamConfig.h"
#include "AuctionConfig.h"
#include "BankMailConfig.h"
#include "BotAttributeTypes.h"
#include "ConfigTable.h"
#include "FleeConfig.h"
#include "ItemUseConfig.h"
#include "LadderConfig.h"
#include "LoneKeys.h"
#include "MatchLogConfig.h"
#include "NaturalCastConfig.h"
#include "NaturalMoveConfig.h"
#include "NaturalPathConfig.h"
#include "NaturalReactionConfig.h"
#include "NaturalTraceConfig.h"
#include "NaturalTurnConfig.h"
#include "PopulationConfig.h"
#include "ProfessionConfig.h"
#include "ProfessionRules.h"
#include "PvpGearConfig.h"
#include "PvpTestConfig.h"
#include "SkillConfig.h"
#include "SkillGrowthConfig.h"
#include "TacticGateConfig.h"
#include "TacticIds.h"
#include "TacticScope.h"
#include "TestPlayerConfig.h"
#include "WorldLogConfig.h"
#include "WorldPvpConfig.h"
#include "WorldTravelConfig.h"

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::cout << "FAIL: " << what << "\n";
    }
}

// Key tails that two rows may share on purpose (read twice with two defaults)
std::set<std::string> const DuplicateAllowed = {"KnownFromSkill"};

std::map<std::string, std::string> DistKeys(std::string const& text)
{
    std::map<std::string, std::string> keys;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == '[')
            continue;
        std::size_t const equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        auto const trim = [](std::string value)
        {
            std::size_t const first = value.find_first_not_of(" \t\"");
            std::size_t const last = value.find_last_not_of(" \t\"");
            return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
        };
        keys[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
    }
    return keys;
}

bool SameValue(std::string const& dist, std::string const& value)
{
    if (dist == value)
        return true;
    char* distEnd = nullptr;
    char* valueEnd = nullptr;
    double const a = std::strtod(dist.c_str(), &distEnd);
    double const b = std::strtod(value.c_str(), &valueEnd);
    return distEnd != dist.c_str() && *distEnd == '\0' && valueEnd != value.c_str() && *valueEnd == '\0' && a == b;
}

// The dist check of one family; returns its problems (empty = clean)
template <typename Struct>
std::vector<std::string> DistProblems(ConfigFamily<Struct> const& family,
                                      std::map<std::string, std::string> const& dist,
                                      std::set<std::string> const& known = {})
{
    std::vector<std::string> problems;
    Struct const defaults = ConfigFamilyDefaults(family);
    std::set<std::string> rowKeys;
    for (ConfigRow<Struct> const& row : family.Rows)
    {
        std::string const key = std::string(family.Prefix) + row.Key;
        rowKeys.insert(key);
        auto const line = dist.find(key);
        if (IsListedConfigKind(row.Kind))
        {
            if (line == dist.end())
                problems.push_back(key + ": listed, no dist line");
            else if (!SameValue(line->second, ConfigRowValueText(row, defaults)))
                problems.push_back(key + ": dist " + line->second + ", default " + ConfigRowValueText(row, defaults));
        }
        else if (row.Kind == ConfigKind::Tuning && line != dist.end())
            problems.push_back(key + ": unlisted tuning value has a dist line");
    }
    for (auto const& [key, value] : dist)
        if (key.rfind(family.Prefix, 0) == 0 && !rowKeys.count(key) && !known.count(key))
            problems.push_back(key + ": dist key without a row");
    return problems;
}

template <typename Struct>
std::vector<std::string> RowProblems(ConfigFamily<Struct> const& family, std::map<std::string, int>& seen)
{
    std::vector<std::string> problems;
    Struct const defaults = ConfigFamilyDefaults(family);
    for (ConfigRow<Struct> const& row : family.Rows)
    {
        std::string const key = std::string(family.Prefix) + row.Key;
        std::string const tail = key.substr(key.rfind('.') + 1);
        if (++seen[key] > 1 && !DuplicateAllowed.count(tail))
            problems.push_back(key + ": duplicate key");
        if (row.Lower > row.Upper)
            problems.push_back(key + ": lower above upper");
        std::visit(
            [&](auto member)
            {
                using T = ConfigTargetType<Struct, decltype(member)>;
                if constexpr (!std::is_same_v<T, std::string> && !std::is_same_v<T, bool>)
                {
                    double const value = static_cast<double>(ConfigTargetValue(defaults, member));
                    if (value < row.Lower || value > row.Upper)
                        problems.push_back(key + ": default outside its bounds");
                }
            },
            row.Target);
    }
    return problems;
}

// A map-backed reader: a key it holds is parsed as the default's type; a missing key is noted when it should warn
struct FakeReader
{
    std::map<std::string, std::string> values;
    std::vector<std::string> missingWarnings;

    template <typename T>
    T operator()(std::string const& key, T fallback, bool warnIfMissing)
    {
        auto const found = values.find(key);
        if (found == values.end())
        {
            if (warnIfMissing)
                missingWarnings.push_back(key);
            return fallback;
        }
        if constexpr (std::is_same_v<T, std::string>)
            return found->second;
        else if constexpr (std::is_same_v<T, bool>)
            return found->second == "1" || found->second == "true";
        else
        {
            std::istringstream text(found->second);
            T value{};
            text >> value;
            return value;
        }
    }
};

struct TypesConfig
{
    int32_t Signed = -5;
    float Ratio = 0.5f;
    float Range = 40.0f;
    bool Flag = true;
    std::string Names = "a,b";
    uint32_t Old = 3;
    uint32_t Unbounded = 7;
};

bool ParseNames(std::string const& text, TypesConfig&) { return text.find(';') == std::string::npos; }

constexpr std::array<ConfigRow<TypesConfig>, 7> TypesRows = {{
    {"Signed", &TypesConfig::Signed, -10, 10, ConfigKind::TuningListed},
    {"Ratio", &TypesConfig::Ratio, 0.0f, 1.0f},
    {"Range", &TypesConfig::Range, 5.0f, 100.0f, ConfigKind::Tuning, ConfigOutOfRange::KeepDefault},
    {"Flag", &TypesConfig::Flag, 0, 0, ConfigKind::Test},
    {"Names", &TypesConfig::Names, 0, 0, ConfigKind::Tuning, ConfigOutOfRange::Clamp, &ParseNames},
    {"Old", &TypesConfig::Old, 0, 10, ConfigKind::Retired},
    {"Unbounded", &TypesConfig::Unbounded},
}};

ConfigFamily<TypesConfig> const TypesFamily{"Test.Types.", TypesRows};

void TestBankMailWalk()
{
    BankMailConfig config;
    config.MailsPerVisit = 7;
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };

    FakeReader empty;
    Check(LoadConfigFamily(BankMailFamily, config, empty, warn) == 0 && warnings.empty(),
          "BankMail: no key, no warning");
    BankMailConfig const defaults{};
    Check(config.MailsPerVisit == defaults.MailsPerVisit && config.MailDefaultWorthCopper == 100 &&
              config.KeepLevelHorizon == 5 && config.MailVisitDeadlineSeconds == 900 &&
              config.BankVisitDeadlineSeconds == 900,
          "BankMail: the walk resets to the initializers");
    Check(empty.missingWarnings.empty(), "BankMail: unlisted rows read without the missing-key warning");

    FakeReader probe;
    probe.values = {{"Playerbots.BankMail.Mail.DefaultWorthCopper", "250"},
                    {"Playerbots.BankMail.Mail.VisitDeadlineSeconds", "1200"},
                    {"Playerbots.BankMail.Bank.VisitDeadlineSeconds", "30"},
                    {"Playerbots.BankMail.Mail.PerVisit", "500"},
                    {"Playerbots.BankMail.Bank.KeepLevelHorizon", "99"}};
    Check(LoadConfigFamily(BankMailFamily, config, probe, warn) == 3 && warnings.size() == 3,
          "BankMail: three values outside their bounds, three warnings");
    Check(config.MailDefaultWorthCopper == 250 && config.MailVisitDeadlineSeconds == 1200,
          "BankMail: values in range are kept");
    Check(config.BankVisitDeadlineSeconds == 60, "BankMail: clamped up to the lower bound");
    Check(config.MailsPerVisit == 100 && config.KeepLevelHorizon == 20, "BankMail: clamped down to the upper bound");
    Check(std::find(warnings.begin(), warnings.end(),
                    "Playerbots.BankMail.Bank.VisitDeadlineSeconds = 30 is outside 60 to 3600, using 60") !=
              warnings.end(),
          "BankMail: the warning names key, value, bounds and result");
}

// The professions plan's family (WR1): the defaults, and a value outside its bounds clamped with a warning
void TestProfessionWalk()
{
    ProfessionConfig config;
    config.GathererLevel = 70;
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };

    FakeReader empty;
    Check(LoadConfigFamily(ProfessionFamily, config, empty, warn) == 0 && warnings.empty(),
          "Professions: no key, no warning");
    Check(config.GathererLevel == 5 && config.CraftVisitDeadlineSeconds == 900 &&
              config.FishTripDeadlineSeconds == 1200 && config.VisitStepSeconds == 10,
          "Professions: the walk resets to the initializers");

    FakeReader probe;
    probe.values = {{"Playerbots.Professions.Learn.GathererLevel", "10"},
                    {"Playerbots.Professions.Craft.VisitDeadlineSeconds", "5"}};
    Check(LoadConfigFamily(ProfessionFamily, config, probe, warn) == 1 && warnings.size() == 1 &&
              config.GathererLevel == 10 && config.CraftVisitDeadlineSeconds == 60,
          "Professions: a value in range kept, one below its bound clamped with a warning");
    Check(ProfessionFamily.Rows.size() == 72, "Professions: four plain rows, eight shares and sixty biases");
    Check(config.CraftShare[0] == 12.5f && config.ClassBias[ProfessionRules::Cell(2, 3)] == 2.0f,
          "Professions: the shares are equal and the hunters' engineering bias 2 by default");
    probe.values = {{"Playerbots.Professions.Share.Blacksmithing", "50"},
                    {"Playerbots.Professions.Bias.Hunter.Engineering", "0"}};
    uint32_t const before = static_cast<uint32_t>(warnings.size());
    Check(LoadConfigFamily(ProfessionFamily, config, probe, warn) == 1 && warnings.size() == before + 1 &&
              config.CraftShare[0] == 50.0f && config.ClassBias[ProfessionRules::Cell(2, 3)] == 0.1f,
          "Professions: a bias of 0 is outside its bounds (no option): clamped with a warning");
    Check(config.UnreachableCrafts == 1u && ProfessionUnreachableCrafts(config) == "blacksmithing",
          "Professions: a share no class can reach is named after the load");
    FakeReader none;
    LoadConfigFamily(ProfessionFamily, config, none, warn);
    Check(config.UnreachableCrafts == 0u && ProfessionUnreachableCrafts(config).empty() &&
              config.Chance[ProfessionRules::Cell(0, 0)] > 0.0 && config.Chance[ProfessionRules::Cell(0, 4)] > 0.0,
          "Professions: the defaults reach every share and fit chances for the served crafts");
}

// WA1a: the auction family's walk; the defaults are the initializers, a value outside its bounds clamped
void TestAuctionWalk()
{
    AuctionConfig config;
    config.PublishSeconds = 5;
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };

    FakeReader empty;
    Check(LoadConfigFamily(AuctionFamily, config, empty, warn) == 0 && warnings.empty(), "Auction: no key, no warning");
    Check(config.PublishSeconds == 60 && config.SaveSeconds == 300 && config.HistoryDays == 7 &&
              config.OpenBidsMax == 3 && config.UndercutPercent == 3 && config.RelistStepPercent == 10 &&
              config.RelistLimit == 3 && config.FirstDurationHours == 24 && config.RelistDurationHours == 48,
          "Auction: the walk resets to the initializers");

    FakeReader probe;
    probe.values = {{"Playerbots.Auction.Bid.OpenMax", "5"}, {"Playerbots.Auction.Post.FirstDurationHours", "72"}};
    Check(LoadConfigFamily(AuctionFamily, config, probe, warn) == 1 && warnings.size() == 1 &&
              config.OpenBidsMax == 5 && config.FirstDurationHours == 48,
          "Auction: a value in range kept, one above its bound clamped with a warning");
    Check(AuctionFamily.Rows.size() == 9, "Auction: nine rows");
    Check(AuctionWords::ChoiceWord(AuctionWords::Choice::Skip) == "skip" &&
              AuctionWords::ReasonWord(AuctionWords::Reason::DetourTooLong) == "detour_too_long",
          "Auction: the decision words");
}

void TestTypesWalk()
{
    TypesConfig config;
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };

    FakeReader empty;
    LoadConfigFamily(TypesFamily, config, empty, warn);
    Check(empty.missingWarnings == std::vector<std::string>{"Test.Types.Signed", "Test.Types.Flag"},
          "types: the missing-key warning is on for listed rows only, retired rows are not read");

    FakeReader probe;
    probe.values = {{"Test.Types.Signed", "-20"},          {"Test.Types.Ratio", "1.5"},
                    {"Test.Types.Range", "500"},           {"Test.Types.Flag", "0"},
                    {"Test.Types.Names", "x;y"},           {"Test.Types.Old", "9"},
                    {"Test.Types.Unbounded", "4000000000"}};
    uint32_t const count = LoadConfigFamily(TypesFamily, config, probe, warn);
    Check(count == 4, "types: int32 clamp, float clamp, keep-default and a parser refusal warn");
    Check(config.Signed == -10, "types: int32 clamped to its lower bound");
    Check(config.Ratio == 1.0f, "types: float clamped to its upper bound");
    Check(config.Range == 40.0f, "types: out of range keeps the default (KeepDefault)");
    Check(!config.Flag, "types: bool read");
    Check(config.Names == "a,b", "types: a parser refusal keeps the default");
    Check(config.Old == 3, "types: a retired row is ignored");
    Check(config.Unbounded == 4000000000u, "types: a row without bounds takes its type's range");

    probe.values["Test.Types.Range"] = "60";
    probe.values["Test.Types.Names"] = "x,y";
    LoadConfigFamily(TypesFamily, config, probe, warn);
    Check(config.Range == 60.0f && config.Names == "x,y", "types: in-range float and parsed string kept");
    Check(
        ConfigRowValueText(TypesRows[1], config) == std::string("1") && ConfigRowValueText(TypesRows[3], config) == "0",
        "types: the value as text");

    // a NaN keeps the default of every row (CS22j: a Clamp row no longer clamps it to the upper bound)
    auto const nanReader = [](std::string const&, auto fallback, bool)
    {
        if constexpr (std::is_same_v<decltype(fallback), float>)
            return std::numeric_limits<float>::quiet_NaN();
        else
            return fallback;
    };
    TypesConfig nanConfig;
    uint32_t const nanWarnings = LoadConfigFamily(TypesFamily, nanConfig, nanReader, warn);
    TypesConfig const nanDefaults{};
    Check(nanConfig.Range == nanDefaults.Range && nanWarnings >= 1,
          "types: a NaN keeps the default of a KeepDefault row, with a warning");
    Check(nanConfig.Ratio == nanDefaults.Ratio && nanWarnings >= 2,
          "types: a NaN keeps the default of a Clamp row too (no clamp to the upper bound), with a warning");
}

struct SampleConfig
{
    uint32_t A = 15;
    float Ratio = 0.5f;
    uint32_t Old = 3;
    bool Flag = false;
};

constexpr std::array<ConfigRow<SampleConfig>, 4> SampleRows = {{
    {"A", &SampleConfig::A, 0, 100},
    {"Ratio", &SampleConfig::Ratio},
    {"Old", &SampleConfig::Old, 0, 10, ConfigKind::Retired},
    {"Flag", &SampleConfig::Flag, 0, 0, ConfigKind::Test},
}};

ConfigFamily<SampleConfig> const SampleFamily{"Playerbots.Fake.", SampleRows};

void TestUnknownRetiredPrint()
{
    std::vector<std::string> const keys = {"Playerbots.Fake.A", "Playerbots.Fake.NoSuchKey", "Playerbots.Fake.Old",
                                           "Playerbots.Other.NoSuchKey", "Playerbots.Fake."};
    std::vector<std::string> const unknown = UnknownConfigKeys(SampleFamily, keys);
    Check(unknown == std::vector<std::string>{"Playerbots.Fake.NoSuchKey", "Playerbots.Fake."},
          "unknown: only the key under the prefix with no row is reported, a key outside the prefix is not");
    Check(UnknownConfigKeys(SampleFamily, {"Playerbots.Fake.Old"}).empty(), "retired: a retired row knows its key");

    SampleConfig config;
    FakeReader reader;
    reader.values = {{"Playerbots.Fake.Old", "9"}, {"Playerbots.Fake.A", "20"}};
    std::vector<std::string> warnings;
    uint32_t const count = LoadConfigFamily(SampleFamily, config, reader,
                                            [&warnings](std::string const& text) { warnings.push_back(text); });
    Check(count == 0 && warnings.empty(), "retired: a retired key set in the config gives no warning");
    Check(config.Old == 3 && config.A == 20, "retired: the member keeps its initializer, other rows are read");
    Check(reader.missingWarnings == std::vector<std::string>{"Playerbots.Fake.Flag"},
          "retired: the walk never reads the retired key");

    SampleConfig const defaults;
    Check(ConfigRowLine(SampleFamily, SampleRows[0], defaults) ==
              "Playerbots.Fake.A = 15 (default 15, bounds 0 to 100, tuning)",
          "print: a default value has no mark");
    Check(ConfigRowLine(SampleFamily, SampleRows[0], config) ==
              "Playerbots.Fake.A = 20 * (default 15, bounds 0 to 100, tuning)",
          "print: a changed value is marked");
    Check(ConfigRowLine(SampleFamily, SampleRows[1], defaults) ==
              "Playerbots.Fake.Ratio = 0.5 (default 0.5, bounds -inf to inf, tuning)",
          "print: a float prints as 0.5, an open bound as inf");
    Check(ConfigRowLine(SampleFamily, SampleRows[3], defaults) == "Playerbots.Fake.Flag = 0 (default 0, test)",
          "print: a bool has no bounds");
    Check(ConfigBoundText(0.25) == "0.25" && ConfigValueText(1.5f) == "1.5", "print: fractional text is the shortest");
}

// CS22j: the parsed rows (the fight spot, the test name prefix) and the lone full-key families
void TestLastFamilies()
{
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };

    PvpTestConfig const spot{};
    PvpTestConfig fight;
    FakeReader empty;
    Check(
        LoadConfigFamily(PvpTestFamily, fight, empty, warn) == 0 && fight.FightMap == 0 && fight.FightX == spot.FightX,
        "PvpTest: no key keeps the default spot");
    FakeReader place;
    place.values = {{"Playerbots.PvpTest.FightLocation", "1 -10.5 20 30.25 0.5"}};
    Check(LoadConfigFamily(PvpTestFamily, fight, place, warn) == 0 && fight.FightMap == 1 && fight.FightX == -10.5f &&
              fight.FightY == 20.0f && fight.FightZ == 30.25f && fight.FightO == 0.5f,
          "PvpTest: FightLocation fills the five fight members");
    FakeReader noSpot;
    noSpot.values = {{"Playerbots.PvpTest.FightLocation", ""}};
    Check(
        LoadConfigFamily(PvpTestFamily, fight, noSpot, warn) == 0 && fight.FightMap == 0 && fight.FightX == spot.FightX,
        "PvpTest: an empty FightLocation is the default spot, without a warning");
    FakeReader badSpot;
    badSpot.values = {{"Playerbots.PvpTest.FightLocation", "1 2 three"}};
    fight.FightMap = 9;
    Check(LoadConfigFamily(PvpTestFamily, fight, badSpot, warn) == 1 && fight.FightMap == 0 &&
              fight.FightY == spot.FightY,
          "PvpTest: a bad FightLocation keeps the default spot, with a warning");

    LoneKeys lone;
    auto const prefixOf = [&](char const* text)
    {
        FakeReader reader;
        reader.values = {{"Playerbots.RandomBotTestNamePrefix", text}};
        lone = LoneKeys{};
        uint32_t const count = LoadConfigFamily(TestNamePrefixFamily, lone, reader, warn);
        return std::make_pair(lone.TestNamePrefix, count);
    };
    Check(prefixOf("tESTx") == std::make_pair(std::string("Testx"), 0u), "TestNamePrefix: lower case, capital first");
    Check(prefixOf("") == std::make_pair(std::string(), 0u), "TestNamePrefix: empty is the name pool");
    Check(prefixOf("ab1") == std::make_pair(std::string(), 1u),
          "TestNamePrefix: a digit keeps the pool, with a warning");
    Check(prefixOf("x") == std::make_pair(std::string(), 1u) &&
              prefixOf("abcdefghij") == std::make_pair(std::string(), 1u),
          "TestNamePrefix: 2 to 9 letters only");

    FakeReader sight;
    sight.values = {{"Playerbots.BotSightRangeCap", "-5"}, {"Playerbots.RandomBotAutoJoinRatedArenaQueueSlots", "0"}};
    lone = LoneKeys{};
    Check(LoadConfigFamily(SightRangeCapFamily, lone, sight, warn) == 1 && lone.SightRangeCap == 0.0f &&
              LoadConfigFamily(RatedQueueSlotsFamily, lone, sight, warn) == 1 && lone.RatedQueueSlots == 1,
          "LoneKeys: a negative sight cap and no queue slot clamp to their lower bounds, with a warning");
}

void TestDistChecks(std::map<std::string, std::string> const& dist)
{
    std::map<std::string, int> seen;
    std::vector<std::string> problems = RowProblems(BankMailFamily, seen);
    std::vector<std::string> const distProblems = DistProblems(BankMailFamily, dist);
    problems.insert(problems.end(), distProblems.begin(), distProblems.end());
    for (std::string const& problem : problems)
        std::cout << "  " << problem << "\n";
    Check(problems.empty(), "BankMail: rows unique, defaults in bounds, conf.dist agrees with the rows");
    Check(BankMailFamily.Rows.size() == 5, "BankMail: five rows");
    std::vector<std::string> professionProblems = RowProblems(ProfessionFamily, seen);
    std::vector<std::string> const professionDist = DistProblems(ProfessionFamily, dist);
    professionProblems.insert(professionProblems.end(), professionDist.begin(), professionDist.end());
    for (std::string const& problem : professionProblems)
        std::cout << "  " << problem << "\n";
    Check(professionProblems.empty(), "Professions: rows unique, defaults in bounds, conf.dist agrees with the rows");
    std::vector<std::string> auctionProblems = RowProblems(AuctionFamily, seen);
    std::vector<std::string> const auctionDist = DistProblems(AuctionFamily, dist);
    auctionProblems.insert(auctionProblems.end(), auctionDist.begin(), auctionDist.end());
    for (std::string const& problem : auctionProblems)
        std::cout << "  " << problem << "\n";
    Check(auctionProblems.empty(), "Auction: rows unique, defaults in bounds, conf.dist agrees with the rows");

    // CS22c-f: the tactic and skill families; rows unique, defaults in bounds, conf.dist agrees with the rows. Several
    // families share a prefix (the per-tactic table and a tactic's numbers), so a dist key is "without a row" only when
    // no family has it.
    auto const forEachFamily = [](auto&& visit)
    {
        visit(HoldFamily, 13, "Hold");
        visit(FocusFamily, 22, "Focus");
        visit(FocusTargetFamily, 3, "FocusTarget");
        visit(CcFamily, 15, "Cc");
        visit(AvoidFamily, 18, "Avoid");
        visit(InterruptFamily, 38, "Interrupt");
        visit(DefensiveFamily, 30, "Defensive");
        visit(FakeFamily, 20, "Fake");
        visit(LosFamily, 14, "Los");
        visit(PillarFamily, 7, "Pillar");
        visit(KiteFamily, 15, "Kite");
        visit(DrinkFamily, 12, "Drink");
        visit(JumpFamily, 10, "Jump");
        for (uint32 index = 0; index < TACTIC_COUNT; ++index)
            visit(TacticSettingsFamily(index), 5, TACTIC_DEFINITIONS[index].ConfigName);
        visit(TacticsEnableFamily, 1, "Tactics.Enable");
        visit(SkillFamily, 14, "Skill");
        visit(SkillGateFamily, 6, "Skill gate");
        visit(SkillGrowthFamily, 5, "Skill growth");
        visit(SkillAttributeLoadingFamily, 12, "Skill attribute loadings");
        visit(SkillAttributeSpreadFamily, 12, "Skill attribute spreads");
        visit(ReactionFamily, 16, "Reaction");
        visit(ReactionChoiceFamily, 3, "Reaction choice");
        visit(ReactionLoadFamily, 7, "Reaction load");
        visit(ReactionEaseFamily, 9, "Reaction ease");
        visit(ReactionThinkFamily, 8, "Reaction think");
        visit(ReactionAnticipateFamily, 2, "Reaction anticipate");
        visit(ReactionQueueFamily, 4, "Reaction queue");
        visit(ReactionReevalFamily, 4, "Reaction reeval");
        visit(ReactionMemoryFamily, 2, "Reaction memory");
        visit(CadenceFamily, 4, "Cadence");
        visit(NaturalFacingFamily, 24, "Natural facing");
        visit(NaturalTrackFamily, 24, "Natural track");
        visit(NaturalPathFamily, 25, "Natural path");
        visit(NaturalTraceFamily, 8, "Natural trace");
        visit(FleeFamily, 3, "Flee");
        visit(WorldLogFamily, 8, "World log");
        visit(PopulationRotationFamily, 6, "Population rotation");
        visit(PopulationRetireFamily, 4, "Population retire");
        visit(PopulationFollowFamily, 1, "Population follow");
        visit(WorldPvpFamily, 5, "World PvP");
        visit(WorldPvpEnthusiasmFamily, 34, "World PvP enthusiasm");
        visit(WorldTravelFamily, 19, "World travel");
        visit(PvpGearFamily, 8, "PvP gear");
        visit(ArenaRecruitFamily, 8, "Arena recruit");
        visit(ArenaTeamFamily, 28, "Arena team");
        visit(LadderFamily, 5, "Ladder");
        visit(MatchmakerFamily, 10, "Ladder matchmaker");
        visit(AcquaintanceFamily, 15, "Acquaintance");
        visit(MatchLogFamily, 3, "Match log");
        visit(PvpTestFamily, 3, "PvP test");
        visit(TestPlayerFamily, 2, "Test player");
        visit(ArenaOnlyFamily, 2, "Arena only");
        visit(ItemUseFamily, 1, "Item use");
        visit(SightRangeCapFamily, 1, "BotSightRangeCap");
        visit(RatedQueueSlotsFamily, 1, "RandomBotAutoJoinRatedArenaQueueSlots");
        visit(RatedQueueUncappedFamily, 1, "RandomBotAutoJoinRatedArenaUncapped");
        visit(TestNamePrefixFamily, 1, "RandomBotTestNamePrefix");
        visit(DebugThreadChecksFamily, 1, "DebugThreadChecks");
        visit(TestMissLogFamily, 1, "Test.MissLog");
    };
    std::set<std::string> known;
    // The keys code reads outside the tables
    for (auto const& [key, value] : dist)
        if (key.rfind("Playerbots.Skill.Attributes.Corr.", 0) == 0)
            known.insert(key);
    forEachFamily(
        [&known](auto const& family, std::size_t, char const*)
        {
            for (auto const& row : family.Rows)
                known.insert(std::string(family.Prefix) + row.Key);
        });
    forEachFamily(
        [&dist, &seen, &known](auto const& family, std::size_t rows, char const* name)
        {
            std::vector<std::string> found = RowProblems(family, seen);
            std::vector<std::string> const distFound = DistProblems(family, dist, known);
            found.insert(found.end(), distFound.begin(), distFound.end());
            for (std::string const& problem : found)
                std::cout << "  " << problem << "\n";
            Check(found.empty() && family.Rows.size() == rows,
                  std::string(name) + ": rows, defaults and conf.dist agree");
        });
    CcConfig masked;
    masked.AvoidAoeGuardExemptClasses = 0xFFFFFFFFu;
    masked.AvoidDotGuardExemptClasses = 272;
    AvoidFamily.Finish(masked);
    Check(masked.AvoidAoeGuardExemptClasses == 0x7FFu && masked.AvoidDotGuardExemptClasses == 272,
          "Avoid: the class masks keep bits 0-10 (the 11 classes), as the old read did");
    Check(dist.count("Playerbots.Population.Persistent") == 0,
          "retired: Playerbots.Population.Persistent has no dist line");
    for (char const* key :
         {"Playerbots.Population.Rotation.AcquaintedSharePct", "Playerbots.Population.Rotation.AcquaintedMinScore"})
        Check(dist.count(key) == 0, std::string("unlisted: ") + key + " has no dist line (console command only)");
    for (char const* key : {"Playerbots.RandomBotArenaGroupOnWorldThread", "Playerbots.RandomBotArenaTeamRatingMode",
                            "Playerbots.RandomBotArenaTeamMinRating", "Playerbots.RandomBotArenaTeamMaxRating",
                            "Playerbots.Ladder.Enable", "Playerbots.Natural.Cadence.NearRealPlayerYards",
                            "Playerbots.Ladder.Matchmaker.Enable"})
        Check(dist.count(key) == 0, std::string("retired: ") + key + " has no dist line");

    // The checks themselves, on a fixture
    std::map<std::string, std::string> const fixture = DistKeys(
        "Playerbots.BankMail.NoRow = 1\nPlayerbots.BankMail.Mail.PerVisit = 50\n# Playerbots.BankMail.Old = 2\n"
        "Test.Types.Signed = -4\n");
    std::vector<std::string> const bank = DistProblems(BankMailFamily, fixture);
    Check(bank.size() == 2 && bank[0].find("Mail.PerVisit: unlisted") != std::string::npos &&
              bank[1].find("NoRow: dist key without a row") != std::string::npos,
          "fixture: a dist key without a row and an unlisted row with a dist line are found");
    std::vector<std::string> const types = DistProblems(TypesFamily, fixture);
    Check(types.size() == 2 && types[0].find("Signed: dist -4, default -5") != std::string::npos &&
              types[1].find("Flag: listed, no dist line") != std::string::npos,
          "fixture: a listed row with a wrong default and one without a dist line are found");

    std::map<std::string, int> twice;
    RowProblems(TypesFamily, twice);
    Check(RowProblems(TypesFamily, twice).size() == 7, "fixture: a key read twice is a duplicate");
}

// Two families fill one struct (Hold and Focus fill FocusConfig); a Finish rule masks a value after the walk
struct SharedConfig
{
    uint32_t HoldMs = 100;
    uint32_t FocusMs = 200;
    uint32_t Classes = 0;
};

void MaskSharedClasses(SharedConfig& out) { out.Classes &= 0x7FFu; }

constexpr std::array<ConfigRow<SharedConfig>, 1> SharedHoldRows = {{
    {"HoldMs", &SharedConfig::HoldMs, 0, 1000},
}};
constexpr std::array<ConfigRow<SharedConfig>, 2> SharedFocusRows = {{
    {"FocusMs", &SharedConfig::FocusMs, 0, 1000},
    {"Classes", &SharedConfig::Classes, 0, 4294967295.0},
}};

ConfigFamily<SharedConfig> const SharedHoldFamily{"Test.Hold.", SharedHoldRows};
ConfigFamily<SharedConfig> const SharedFocusFamily{"Test.Focus.", SharedFocusRows, &MaskSharedClasses};

void TestSharedStruct()
{
    SharedConfig config;
    auto const silent = [](std::string const&) {};
    FakeReader reader;
    reader.values = {{"Test.Hold.HoldMs", "300"}, {"Test.Focus.FocusMs", "400"}, {"Test.Focus.Classes", "4095"}};
    LoadConfigFamily(SharedHoldFamily, config, reader, silent);
    LoadConfigFamily(SharedFocusFamily, config, reader, silent);
    Check(config.HoldMs == 300 && config.FocusMs == 400, "shared: the second family keeps the first one's value");
    Check(config.Classes == 0x7FFu, "shared: Finish masks the value after the walk");

    FakeReader empty;
    config.Classes = 4095;
    LoadConfigFamily(SharedHoldFamily, config, empty, silent);
    Check(config.HoldMs == 100 && config.FocusMs == 400 && config.Classes == 4095,
          "shared: a walk resets its own rows to their initializers and leaves the other family's members");

    std::array<char const*, 2> const tails = {"Enable", "Scope"};
    std::vector<std::string> const keys = {"Test.Hold.HoldMs", "Test.Hold.Enable", "Test.Hold.Scope",
                                           "Test.Hold.EnableX", "Test.Hold.Typo"};
    Check(UnknownConfigKeys(SharedHoldFamily, keys, tails) ==
              std::vector<std::string>{"Test.Hold.EnableX", "Test.Hold.Typo"},
          "unknown: a code-owned tail is known, a longer key with the same start is not");
    Check(UnknownConfigKeys(SharedHoldFamily, keys).size() == 4,
          "unknown: without code tails every rowless key is unknown");
}
// CS22e: uint8 and double members, array elements, family defaults that are not the initializer, a parser that
// fills another member, a code tail that covers every key below it
struct WideConfig
{
    uint8_t Percent = 100;
    double Gain = 0.8f;  // a double the old code read as a float
    std::array<float, 3> Loading = {1.0f, 1.0f, 0.0f};
    std::string Scope = "a";
    uint32_t ScopeBits = 1;
};

float& WideLoading(WideConfig& out, uint32_t index) { return out.Loading[index]; }

bool ParseWideScope(std::string const& text, WideConfig& out)
{
    if (text != "a" && text != "b")
        return false;
    out.ScopeBits = text == "a" ? 1 : 2;
    return true;
}

constexpr std::array<ConfigRow<WideConfig>, 4> WideRows = {{
    {"Percent", &WideConfig::Percent, 0, 100},
    {"Gain", &WideConfig::Gain, 0.0f, 10.0f},
    {"Loading.B", ConfigElement<WideConfig, float>{&WideLoading, 1}, 0.0f, 1.0f},
    {"Scope", &WideConfig::Scope, 0, 0, ConfigKind::Tuning, ConfigOutOfRange::Clamp, &ParseWideScope},
}};

WideConfig const WideDefaults = {100, 0.8f, {1.0f, 1.0f, 0.0f}, "b", 2};
ConfigFamily<WideConfig> const WideFamily{"Test.Wide.", WideRows, nullptr, &WideDefaults};

void TestWideTypes()
{
    WideConfig config;
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };

    FakeReader empty;
    Check(LoadConfigFamily(WideFamily, config, empty, warn) == 0, "wide: no key, no warning");
    Check(config.Percent == 100 && config.Gain == static_cast<double>(0.8f) && config.Loading[1] == 1.0f,
          "wide: uint8, double and element take their defaults");
    Check(config.Scope == "b" && config.ScopeBits == 2,
          "wide: the family's Defaults, not the initializer, and the "
          "parser fills its member from the default text");

    FakeReader probe;
    probe.values = {{"Test.Wide.Percent", "300"},
                    {"Test.Wide.Gain", "2.5"},
                    {"Test.Wide.Loading.B", "0.25"},
                    {"Test.Wide.Scope", "zzz"}};
    config.ScopeBits = 7;
    Check(LoadConfigFamily(WideFamily, config, probe, warn) == 2,
          "wide: a uint8 above its bound and a refused "
          "scope warn");
    Check(config.Percent == 100 && config.Gain == 2.5 && config.Loading[1] == 0.25f && config.Loading[0] == 1.0f,
          "wide: uint8 clamped, double read, one element set and its neighbour left");
    Check(config.Scope == "b" && config.ScopeBits == 2, "wide: a refused text parses the default into its member");
    Check(std::find(warnings.begin(), warnings.end(), "Test.Wide.Percent = 300 is outside 0 to 100, using 100") !=
              warnings.end(),
          "wide: the uint8 warning names the read value");

    probe.values["Test.Wide.Percent"] = "50";
    probe.values["Test.Wide.Scope"] = "a";
    LoadConfigFamily(WideFamily, config, probe, warn);
    Check(config.Percent == 50 && config.ScopeBits == 1, "wide: in-range uint8 and a parsed scope");
    Check(ConfigRowLine(WideFamily, WideRows[0], config) ==
              "Test.Wide.Percent = 50 * (default 100, bounds 0 to 100, tuning)",
          "wide: a uint8 prints as a number");
    Check(ConfigRowLine(WideFamily, WideRows[1], WideDefaults) ==
              "Test.Wide.Gain = 0.8 (default 0.8, bounds 0 to 10, tuning)",
          "wide: a double prints as the float it was read as");
    Check(ConfigRowLine(WideFamily, WideRows[2], config) ==
              "Test.Wide.Loading.B = 0.25 * (default 1, bounds 0 to 1, tuning)",
          "wide: an element prints with its default");
    Check(ConfigRowLine(WideFamily, WideRows[3], config) == "Test.Wide.Scope = a * (default b, tuning)",
          "wide: the default comes from the family's Defaults");

    std::array<char const*, 1> const corr = {"Corr."};
    Check(KnowsConfigKey(WideFamily, "Test.Wide.Corr.A-B", corr) &&
              !KnowsConfigKey(WideFamily, "Test.Wide.CorrX", corr) &&
              KnowsConfigKey(WideFamily, "Test.Wide.Loading.B") && !KnowsConfigKey(WideFamily, "Test.Other.Gain"),
          "wide: a code tail ending in a dot covers the keys below it, rows know their keys");
}
}  // namespace

// A removed switch whose parent prefix holds live keys: the family's prefix is the full key, its one row's key empty
struct FakeSwitches
{
    bool Gone = false;
};

constexpr std::array<ConfigRow<FakeSwitches>, 1> FakeSwitchRows = {{
    {"", &FakeSwitches::Gone, 0, 0, ConfigKind::Retired},
}};

ConfigFamily<FakeSwitches> const FakeSwitchFamily{"Playerbots.Fake.Enable", FakeSwitchRows};

// CS22e: the per-tactic table (a family per tactic, its scope default from TACTIC_DEFINITIONS) and the system switch
void TestTacticFamilies()
{
    TacticsConfig config;
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };
    FakeReader reader;
    reader.values = {{"Playerbots.Tactics.Enable", "1"},
                     {"Playerbots.Tactics.Hold.Enable", "1"},
                     {"Playerbots.Tactics.Hold.Scope", " pve, Arena "},
                     {"Playerbots.Tactics.Plan.Enable", "0"},
                     {"Playerbots.Tactics.Plan.Scope", "Moon"},
                     {"Playerbots.Tactics.Cc.Scope", ""},
                     {"Playerbots.Tactics.Cc.KnownFromSkill", "150"},
                     {"Playerbots.Tactics.Cc.UseChanceAtSkill1", "40"}};
    uint32 warned = 0;
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
        warned += LoadConfigFamily(TacticSettingsFamily(index), config.Settings[index], reader, warn);
    warned += LoadConfigFamily(TacticsEnableFamily, config, reader, warn);
    TacticSettings const& hold = config.Settings[static_cast<uint32>(TacticId::Hold)];
    TacticSettings const& plan = config.Settings[static_cast<uint32>(TacticId::Plan)];
    TacticSettings const& cc = config.Settings[static_cast<uint32>(TacticId::Cc)];
    TacticSettings const& ownCc = config.Settings[static_cast<uint32>(TacticId::OwnCc)];
    auto const missing = [&reader](char const* key)
    { return std::count(reader.missingWarnings.begin(), reader.missingWarnings.end(), key); };
    Check(warned == 3, "tactics: a refused scope, an empty scope and a gate value above 100 warn");
    Check(hold.Enable && hold.Scope == (TACTIC_SCOPE_PVE | TACTIC_SCOPE_ARENA), "tactics: Hold's scope text parsed");
    Check(plan.Scope == (TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND) && plan.ScopeText == "Arena,Battleground",
          "tactics: a refused scope keeps the definition's default scope");
    Check(cc.Scope == (TACTIC_SCOPE_ARENA | TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_WORLD),
          "tactics: an empty scope keeps the default");
    Check(cc.KnownFromSkill == 100 && cc.UseChanceAtSkill1 == 40 && cc.UseChanceAtSkill99 == 100,
          "tactics: the gate values clamp to 0-100, the use chances default to 100");
    Check(ownCc.Scope == TACTIC_SCOPE_ALL && ownCc.ScopeText == "All" && !ownCc.Enable,
          "tactics: a tactic without keys takes its definition's scope");
    Check(config.ConfigMask == TacticBit(TacticId::Hold), "tactics: the mask holds the enabled tactic");
    Check(missing("Playerbots.Tactics.OwnCc.Enable") == 1 && missing("Playerbots.Tactics.OwnCc.Scope") == 0 &&
              missing("Playerbots.Tactics.OwnCc.KnownFromSkill") == 0,
          "tactics: a missing Enable warns (listed), a missing Scope or gate value does not");
    ConfigFamily<TacticSettings> const& drink = TacticSettingsFamily(static_cast<uint32>(TacticId::Drink));
    ConfigFamily<TacticSettings> const& jump = TacticSettingsFamily(static_cast<uint32>(TacticId::Jump));
    ConfigFamily<TacticSettings> const& kite = TacticSettingsFamily(static_cast<uint32>(TacticId::Kite));
    Check(std::string(drink.Prefix) == "Playerbots.Tactics.Drink." && drink.Rows[0].Kind == ConfigKind::NotYet &&
              jump.Rows[0].Kind == ConfigKind::NotYet && kite.Rows[0].Kind == ConfigKind::Test,
          "tactics: Drink and Jump's switches are not-yet switches, the others test switches");
    ConfigFamily<TacticSettings> const& trinket = TacticSettingsFamily(static_cast<uint32>(TacticId::Trinket));
    Check(trinket.Rows[1].Kind == ConfigKind::TuningListed && trinket.Rows[0].Kind == ConfigKind::Test &&
              kite.Rows[1].Kind == ConfigKind::Tuning,
          "tactics: Trinket's scope is listed (a hub test sets it), the other scopes unlisted tuning values");

    reader.values["Playerbots.Tactics.Enable"] = "0";
    LoadConfigFamily(TacticsEnableFamily, config, reader, warn);
    Check(config.ConfigMask == 0, "tactics: the system off empties the mask");

    uint8 scope = 0;
    std::string error;
    Check(!ParseTacticScope("Arena,", scope, error) && error.find("unknown context ''") == 0,
          "scope: a trailing comma is an empty entry");
    Check(FormatTacticScope(TACTIC_SCOPE_BATTLEGROUND | TACTIC_SCOPE_PVE) == "Battleground,Pve" &&
              FormatTacticScope(0) == "None",
          "scope: format");
}

// CS22e: the skill (its Finish: derived switches, lower-case distribution, Max at least Min) and the gate
void TestSkillFamilies()
{
    SkillConfig skill;
    TacticGateParams gate;
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };

    FakeReader empty;
    LoadConfigFamily(SkillFamily, skill, empty, warn);
    LoadConfigFamily(SkillGateFamily, gate, empty, warn);
    Check(!skill.ReactActive && !skill.NoticeActive && !skill.DistributionUniform && skill.Distribution == "normal" &&
              skill.Mean == 50.0f && skill.Max == 99.0f && gate.KnowledgeSpread == 15 && gate.EffortAtSkill1 == 100,
          "skill: the defaults, nothing derived on");
    Check(std::count(empty.missingWarnings.begin(), empty.missingWarnings.end(), "Playerbots.Skill.Mean") == 0 &&
              std::count(empty.missingWarnings.begin(), empty.missingWarnings.end(), "Playerbots.Skill.Min") == 1 &&
              std::count(empty.missingWarnings.begin(), empty.missingWarnings.end(),
                         "Playerbots.Skill.KnowledgeSpread") == 1,
          "skill: a missing listed key warns, Mean (unlisted) does not");

    FakeReader reader;
    reader.values = {{"Playerbots.Skill.Distribution", "Uniform"},
                     {"Playerbots.Skill.Min", "30"},
                     {"Playerbots.Skill.Max", "20"},
                     {"Playerbots.Skill.Mean", "120"},
                     {"Playerbots.Skill.ReactFactorAtSkill1", "2.5"},
                     {"Playerbots.Skill.NoticeDelayMsAtSkill99", "5"},
                     {"Playerbots.Skill.KnowledgeSpread", "80"},
                     {"Playerbots.Skill.CurveExponent", "0.01"}};
    uint32 const warned =
        LoadConfigFamily(SkillFamily, skill, reader, warn) + LoadConfigFamily(SkillGateFamily, gate, reader, warn);
    Check(warned == 3, "skill: Mean, KnowledgeSpread and CurveExponent outside their bounds warn");
    Check(skill.Distribution == "uniform" && skill.DistributionUniform && skill.Min == 30.0f && skill.Max == 30.0f &&
              skill.Mean == 99.0f && skill.ReactActive && skill.NoticeActive,
          "skill: lower-case uniform, Max raised to Min, Mean clamped, both switches derived");
    Check(gate.KnowledgeSpread == 50 && gate.CurveExponent == 0.1f, "gate: clamped as the old read");

    reader.values = {{"Playerbots.Skill.Distribution", "zipf"}};
    Check(LoadConfigFamily(SkillFamily, skill, reader, warn) == 1 && skill.Distribution == "normal" &&
              !skill.DistributionUniform && !skill.ReactActive && skill.Max == 99.0f,
          "skill: an unknown distribution keeps normal with a warning");
}

// CS22f: the natural reaction (Max at least Min, the ease prices strictly rising, the contexts text into a mask)
void TestReactionFamilies()
{
    ReactionParams reaction;
    ReactionEaseParams ease;
    ReactionQueueParams queue;
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };

    FakeReader empty;
    LoadConfigFamily(ReactionFamily, reaction, empty, warn);
    LoadConfigFamily(ReactionEaseFamily, ease, empty, warn);
    Check(reaction.MedianMsAtSkill1 == 690.0f && reaction.ServerTickMs == -1 && reaction.BehindMaxMs == 400 &&
              reaction.ContextsText == "Arena,Battleground" &&
              reaction.Contexts ==
                  (TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground)),
          "reaction: the defaults, the contexts mask from the text");
    Check(ease.MsAt1[3] == 450 && ease.MsAt99[1] == 80 && ease.FocusDexterity == 40, "reaction: the ease defaults");
    Check(std::count(empty.missingWarnings.begin(), empty.missingWarnings.end(), "Playerbots.Natural.Reaction.MinMs") ==
                  1 &&
              std::count(empty.missingWarnings.begin(), empty.missingWarnings.end(),
                         "Playerbots.Natural.Reaction.Ease.RareMsAtSkill1") == 0,
          "reaction: a missing listed key warns, an unlisted price does not");

    FakeReader reader;
    reader.values = {{"Playerbots.Natural.Reaction.BehindPenaltyMinMs", "500"},
                     {"Playerbots.Natural.Reaction.BehindPenaltyMaxMs", "300"},
                     {"Playerbots.Natural.Reaction.ServerTickMs", "-5"},
                     {"Playerbots.Natural.Reaction.MinMs", "20000"},
                     {"Playerbots.Natural.Reaction.Contexts", "World, Pve"}};
    uint32 const warned = LoadConfigFamily(ReactionFamily, reaction, reader, warn);
    Check(
        warned == 2 && reaction.BehindMinMs == 500 && reaction.BehindMaxMs == 500 && reaction.ServerTickMs == -1 &&
            reaction.MinMs == 10000 &&
            reaction.Contexts == (TacticContextBit(TacticContext::OpenWorldPvp) | TacticContextBit(TacticContext::Pve)),
        "reaction: Max raised to Min, two values clamped with a warning, the contexts parsed");
    reader.values = {{"Playerbots.Natural.Reaction.Contexts", "Arena,Moon"}};
    Check(LoadConfigFamily(ReactionFamily, reaction, reader, warn) == 1 &&
              reaction.ContextsText == "Arena,Battleground" &&
              reaction.Contexts ==
                  (TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground)),
          "reaction: a bad contexts text keeps the default text and mask with a warning");

    reader.values = {{"Playerbots.Natural.Reaction.Ease.CommonMsAtSkill1", "500"},
                     {"Playerbots.Natural.Reaction.Ease.RareMsAtSkill99", "5"},
                     {"Playerbots.Natural.Reaction.Ease.FocusDexterity", "120"}};
    LoadConfigFamily(ReactionEaseFamily, ease, reader, warn);
    Check(ease.MsAt1[0] == 500 && ease.MsAt1[1] == 501 && ease.MsAt1[2] == 502 && ease.MsAt1[3] == 503 &&
              ease.MsAt99[0] == 0 && ease.MsAt99[1] == 5 && ease.MsAt99[2] == 120 && ease.FocusDexterity == 99,
          "reaction: each ease price raised to 1 ms above the one before, the dexterity clamped");
    reader.values = {{"Playerbots.Natural.Reaction.Queue.MinMsAtSkill1", "400"},
                     {"Playerbots.Natural.Reaction.Queue.MinMsAtSkill99", "5"}};
    LoadConfigFamily(ReactionQueueFamily, queue, reader, warn);
    Check(queue.MinMsAt1 == 400 && queue.MaxMsAt1 == 400 && queue.MinMsAt99 == 5 && queue.MaxMsAt99 == 80,
          "reaction: the queue's Max raised to Min at one end only");

    CadenceParams cadence;
    FakeReader cadenceEmpty;
    LoadConfigFamily(CadenceFamily, cadence, cadenceEmpty, warn);
    Check(cadence.CheckMs == 100 && cadence.StaggerMs == 20 && !cadence.SkillStretch &&
              cadence.Contexts ==
                  (TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground)) &&
              cadenceEmpty.missingWarnings.size() == 4,
          "cadence: the defaults, all four keys listed (a missing one warns)");
    FakeReader cadenceReader;
    cadenceReader.values = {{"Playerbots.Natural.Cadence.CheckMs", "5"},
                            {"Playerbots.Natural.Cadence.StaggerMs", "500"},
                            {"Playerbots.Natural.Cadence.Contexts", "Arena,Moon"},
                            {"Playerbots.Natural.Cadence.SkillStretch", "1"}};
    Check(LoadConfigFamily(CadenceFamily, cadence, cadenceReader, warn) == 3 && cadence.CheckMs == 20 &&
              cadence.StaggerMs == UPSTREAM_CHECK_STAGGER_MS && cadence.SkillStretch &&
              cadence.ContextsText == "Arena,Battleground" &&
              cadence.Contexts ==
                  (TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground)),
          "cadence: two values clamped, a bad contexts text keeps the default");
}

// CS22g: the natural facing, track, path and trace
void TestNaturalMoveFamilies()
{
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };
    uint8 const worldPvp =
        static_cast<uint8>(TacticContextBit(TacticContext::Arena) | TacticContextBit(TacticContext::Battleground) |
                           TacticContextBit(TacticContext::OpenWorldPvp));

    TurnConfig turn;
    FakeReader turnEmpty;
    LoadConfigFamily(NaturalFacingFamily, turn, turnEmpty, warn);
    Check(turn.Enable == false && turn.Mode == 1 && turn.CorrectionDelayMaxMs == 250 && turn.InSightYards == 60 &&
              turn.Contexts == worldPvp && turnEmpty.missingWarnings.size() == 2,
          "facing: the defaults, only Enable and Mode listed (a missing one warns)");
    FakeReader turnReader;
    turnReader.values = {{"Playerbots.Natural.Facing.Enable", "1"},
                         {"Playerbots.Natural.Facing.Mode", "0"},
                         {"Playerbots.Natural.Facing.InSightYards", "900"},
                         {"Playerbots.Natural.Facing.CorrectionDelayMinMs", "400"},
                         {"Playerbots.Natural.Facing.KeyboardDegPerS", "70000"},
                         {"Playerbots.Natural.Facing.Contexts", "Arena,Moon"}};
    Check(LoadConfigFamily(NaturalFacingFamily, turn, turnReader, warn) == 4 && turn.Enable && turn.Mode == 1 &&
              turn.InSightYards == 500 && turn.KeyboardDegPerS == 60000 && turn.CorrectionDelayMinMs == 400 &&
              turn.CorrectionDelayMaxMs == 400 && turn.ContextsText == "Arena,Battleground,World" &&
              turn.Contexts == worldPvp,
          "facing: Mode 0 raised to 1, two values clamped, Max raised to Min, a bad contexts text keeps the default");

    TrackConfig track;
    FakeReader trackEmpty;
    LoadConfigFamily(NaturalTrackFamily, track, trackEmpty, warn);
    Check(track.Enable == false && track.RingSamples == 16 && track.SkipLeadRule == 1 && track.RefreshMs == 250 &&
              track.Contexts == worldPvp && trackEmpty.missingWarnings.size() == 1,
          "track: the defaults, only Enable listed (a missing one warns)");
    FakeReader trackReader;
    trackReader.values = {{"Playerbots.Natural.Track.RingSamples", "2"},
                          {"Playerbots.Natural.Track.SkipLeadRule", "5"},
                          {"Playerbots.Natural.Track.VelocityFromSkill", "250"},
                          {"Playerbots.Natural.Track.MaxLeadYards", "9"},
                          {"Playerbots.Natural.Track.Contexts", "None"}};
    Check(LoadConfigFamily(NaturalTrackFamily, track, trackReader, warn) == 3 && track.RingSamples == 4 &&
              track.SkipLeadRule == 1 && track.VelocityFromSkill == 100 && track.MaxLeadYards == 9 &&
              track.Contexts == 0 && track.ContextsText == "None",
          "track: RingSamples 4-32 and the other caps clamp with a warning, None is an empty mask");

    PathConfig path;
    FakeReader pathEmpty;
    LoadConfigFamily(NaturalPathFamily, path, pathEmpty, warn);
    Check(path.Enable == false && path.OffsetsEnable && path.OffsetYardsAtSkill1 == 1.5f &&
              path.StrafeLegMaxYards == 4 && path.Contexts == worldPvp && pathEmpty.missingWarnings.size() == 6,
          "path: the defaults, the six switches listed (a missing one warns)");
    FakeReader pathReader;
    pathReader.values = {{"Playerbots.Natural.Path.OffsetYardsAtSkill1", "25.5"},
                         {"Playerbots.Natural.Path.Strafe.LegMinYards", "9"},
                         {"Playerbots.Natural.Path.Hops.EdgeYards", "501"},
                         {"Playerbots.Natural.Path.Walk.Enable", "1"}};
    Check(LoadConfigFamily(NaturalPathFamily, path, pathReader, warn) == 2 && path.OffsetYardsAtSkill1 == 20.0f &&
              path.StrafeLegMinYards == 9 && path.StrafeLegMaxYards == 9 && path.HopsEdgeYards == 500 &&
              path.WalkEnable,
          "path: an offset and a distance clamp with a warning, the strafe leg's Max raised to Min");

    TraceConfig trace;
    FakeReader traceEmpty;
    LoadConfigFamily(NaturalTraceFamily, trace, traceEmpty, warn);
    Check(!trace.Enable && trace.MaxUnits == 8 && trace.IntervalMs == 100 && trace.MaxRowsPerUnit == 200000 &&
              trace.Dir.empty() && trace.FormatText == "jsonl" && !trace.Binary &&
              traceEmpty.missingWarnings.size() == 8,
          "trace: the defaults, all eight keys listed (a missing one warns)");
    FakeReader traceReader;
    traceReader.values = {{"Playerbots.Natural.Trace.MaxUnits", "5000"},
                          {"Playerbots.Natural.Trace.IntervalMs", "10"},
                          {"Playerbots.Natural.Trace.MaxRowsPerUnit", "50"},
                          {"Playerbots.Natural.Trace.Format", "BINARY"}};
    Check(LoadConfigFamily(NaturalTraceFamily, trace, traceReader, warn) == 3 && trace.MaxUnits == 1000 &&
              trace.IntervalMs == 50 && trace.MaxRowsPerUnit == 100 && trace.Binary,
          "trace: three values clamped with a warning, the format in any case");
    FakeReader traceBad;
    traceBad.values = {{"Playerbots.Natural.Trace.Format", "csv"}};
    Check(LoadConfigFamily(NaturalTraceFamily, trace, traceBad, warn) == 1 && !trace.Binary &&
              trace.FormatText == "jsonl",
          "trace: an unknown format warns and keeps jsonl");
}

// CS22h: the flee, world travel, world PvP and world log families
void TestWorldFamilies()
{
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };

    FleeConfig flee;
    FakeReader fleeEmpty;
    LoadConfigFamily(FleeFamily, flee, fleeEmpty, warn);
    Check(flee.EscapeYards == 40 && flee.CorneredSeconds == 6 && flee.BoundSeconds == 30 &&
              fleeEmpty.missingWarnings.empty(),
          "flee: the defaults, unlisted rows (a missing key is silent)");
    FakeReader fleeReader;
    fleeReader.values = {{"Playerbots.Flee.EscapeYards", "0"},
                         {"Playerbots.Flee.CorneredSeconds", "5000"},
                         {"Playerbots.Flee.BoundSeconds", "45"}};
    Check(LoadConfigFamily(FleeFamily, flee, fleeReader, warn) == 2 && flee.EscapeYards == 1 &&
              flee.CorneredSeconds == 600 && flee.BoundSeconds == 45,
          "flee: 1 to 2000 and 1 to 600 clamp with a warning (the old read clamped silently)");

    LadderConfig ladder;
    MatchmakerConfig matchmaker;
    FakeReader ladderEmpty;
    LoadConfigFamily(LadderFamily, ladder, ladderEmpty, warn);
    LoadConfigFamily(MatchmakerFamily, matchmaker, ladderEmpty, warn);
    Check(ladder.Role == 0 && ladder.ServerId.empty() && ladder.DepartTimeoutSeconds == 600 &&
              ladder.ContactMinutes == 15 && ladder.PoolTeams == 0 && !matchmaker.Enable &&
              matchmaker.DelayMinSeconds == 3 && matchmaker.DelayMaxSeconds == 8 && matchmaker.MmrWidth == 150 &&
              matchmaker.Skirmish && matchmaker.PoolOnly && ladderEmpty.missingWarnings.size() == 8,
          "ladder: the defaults; the role, the server id and the five listed tuning keys warn when missing");
    FakeReader ladderReader;
    ladderReader.values = {{"Playerbots.Ladder.Role", "7"},
                           {"Playerbots.Ladder.PoolTeams", "9999"},
                           {"Playerbots.Ladder.Matchmaker.DelayMinSeconds", "20"},
                           {"Playerbots.Ladder.Matchmaker.DelayMaxSeconds", "5"},
                           {"Playerbots.Ladder.Matchmaker.MaxPicks", "0"}};
    Check(
        LoadConfigFamily(LadderFamily, ladder, ladderReader, warn) == 2 && ladder.Role == 2 && ladder.PoolTeams == 5000,
        "ladder: Role 0 to 2 and PoolTeams 0 to 5000 clamp with a warning");
    Check(LoadConfigFamily(MatchmakerFamily, matchmaker, ladderReader, warn) == 1 && matchmaker.MaxPicks == 1 &&
              matchmaker.DelayMinSeconds == 20 && matchmaker.DelayMaxSeconds == 20,
          "ladder matchmaker: a delay maximum below the minimum rises to it (the Finish rule), MaxPicks clamps");

    WorldTravelConfig travel;
    FakeReader travelEmpty;
    LoadConfigFamily(WorldTravelFamily, travel, travelEmpty, warn);
    bool defaultBands = true;
    for (uint32_t band = 0; band < WorldTravel::LEVEL_BAND_COUNT; ++band)
    {
        WorldTravel::SlotBand const& got = travel.SlotBands[band];
        WorldTravel::SlotBand const& want = WorldTravel::DEFAULT_SLOT_BANDS[band];
        defaultBands = defaultBands && got.LootSlotsPerHour == want.LootSlotsPerHour &&
                       got.TripMinutes == want.TripMinutes && got.EarnCopperPerMinute == want.EarnCopperPerMinute &&
                       got.BagHorizonHours == want.BagHorizonHours;
    }
    Check(defaultBands && travel.RouteThreads == 1 && travelEmpty.missingWarnings.size() == 1,
          "travel: the default texts parse to DEFAULT_SLOT_BANDS; only Traits.AllowEdit is listed");
    FakeReader travelReader;
    travelReader.values = {{"Playerbots.WorldTravel.Slots.TripMinutes", "1,2,3,4,5"},
                           {"Playerbots.WorldTravel.Bags.HorizonHours", "1,2,3"},
                           {"Playerbots.WorldTravel.Slots.LootPerHour", "9999999,2,3,4,5"},
                           {"Playerbots.WorldTravel.RouteThreads", "0"},
                           {"Playerbots.WorldTravel.Planner.ResultStaleSeconds", "1"},
                           {"Playerbots.WorldTravel.Traits.AllowEdit", "1"}};
    uint32_t const travelWarnings = LoadConfigFamily(WorldTravelFamily, travel, travelReader, warn);
    Check(travelWarnings == 3 && travel.SlotBands[4].TripMinutes == 5 && travel.SlotBands[0].TripMinutes == 1 &&
              travel.SlotBands[0].LootSlotsPerHour == 1000000 && travel.SlotBands[1].LootSlotsPerHour == 2 &&
              travel.SlotBands[0].BagHorizonHours == 20 && travel.SlotBands[4].BagHorizonHours == 100 &&
              travel.RouteThreads == 1 && travel.PlannerStaleSeconds == 5 && travel.TraitsAllowEdit,
          "travel: a band list sets each band (a value above 1000000 is cut), a list of another length keeps the "
          "defaults, the clamps lift to 1 and 5");

    WorldPvpConfig pvp;
    FakeReader pvpEmpty;
    LoadConfigFamily(WorldPvpFamily, pvp, pvpEmpty, warn);
    LoadConfigFamily(WorldPvpEnthusiasmFamily, pvp.Enthusiasm, pvpEmpty, warn);
    Check(pvp.ScanYards == 40 && pvp.GhostSpiritHealerSeconds == 300 && pvp.Enthusiasm.Max == 100.0f &&
              pvp.Enthusiasm.StartCost == 15.0f && pvp.Enthusiasm.StartCostShare == 100 &&
              pvp.Enthusiasm.BalanceBehind == 0.25f && pvpEmpty.missingWarnings.size() == 4,
          "world pvp: the defaults; the two hub-set ghost keys, StartCostShare (scenario overlays set it) and "
          "RecoveryPerHour (the reload test edits it) are listed");
    FakeReader pvpReader;
    pvpReader.values = {{"Playerbots.WorldPvp.ScanYards", "5000"},
                        {"Playerbots.WorldPvp.GhostRecheckSeconds", "0"},
                        {"Playerbots.WorldPvp.Enthusiasm.Max", "5000"},
                        {"Playerbots.WorldPvp.Enthusiasm.JoinBase", "80"},
                        {"Playerbots.WorldPvp.Enthusiasm.BalanceLead", "0.2"},
                        {"Playerbots.WorldPvp.Enthusiasm.StartCostShare", "250"}};
    uint32_t const pvpWarnings = LoadConfigFamily(WorldPvpFamily, pvp, pvpReader, warn) +
                                 LoadConfigFamily(WorldPvpEnthusiasmFamily, pvp.Enthusiasm, pvpReader, warn);
    Check(pvpWarnings == 5 && pvp.ScanYards == 200 && pvp.GhostRecheckSeconds == 1 && pvp.Enthusiasm.Max == 100.0f &&
              pvp.Enthusiasm.JoinBase == 80.0f && pvp.Enthusiasm.BalanceLead == 0.75f &&
              pvp.Enthusiasm.StartCostShare == 100,
          "world pvp: whole numbers clamp; a float out of range keeps its default (as pvpRange did); each warns");

    WorldLogOptions log;
    FakeReader logEmpty;
    LoadConfigFamily(WorldLogFamily, log, logEmpty, warn);
    Check(log.Dir.empty() && log.StateSeconds == 60 && log.SegmentMinutes == 60 && log.QueueRows == 200000 &&
              log.RecorderSeconds == 300 && log.RecorderActions == 128 && log.RecorderEvents == 64 &&
              log.RecorderCooldownSeconds == 600 && logEmpty.missingWarnings.size() == 4,
          "world log: the defaults; Dir, StateSeconds, SegmentMinutes and QueueRows (the drops test edits it) are "
          "listed");
    FakeReader logReader;
    logReader.values = {
        {"Playerbots.WorldLog.Dir", "C:/logs"},         {"Playerbots.WorldLog.StateSeconds", "1"},
        {"Playerbots.WorldLog.SegmentMinutes", "7"},    {"Playerbots.WorldLog.QueueRows", "99999999"},
        {"Playerbots.WorldLog.RecorderSeconds", "0"},   {"Playerbots.WorldLog.RecorderActions", "0"},
        {"Playerbots.WorldLog.RecorderEvents", "9000"}, {"Playerbots.WorldLog.RecorderCooldownSeconds", "999999"}};
    uint32_t const logWarnings = LoadConfigFamily(WorldLogFamily, log, logReader, warn);
    Check(logWarnings == 5 && log.Dir == "C:/logs" && log.StateSeconds == 5 && log.SegmentMinutes == 7 &&
              log.QueueRows == 10000000 && log.RecorderSeconds == 0 && log.RecorderActions == 1 &&
              log.RecorderEvents == 4096 && log.RecorderCooldownSeconds == 86400,
          "world log: the rows clamp with a warning; SegmentMinutes 7 stays a row value (the round-down to 6 is "
          "WorldLogFormat's, checked by world-log)");

    // CS26i: the population's values (the minutes-to-seconds step and the session cap are Population::LoadConfig's)
    PopulationOptions population;
    FakeReader populationEmpty;
    LoadConfigFamily(PopulationRotationFamily, population, populationEmpty, warn);
    LoadConfigFamily(PopulationRetireFamily, population, populationEmpty, warn);
    LoadConfigFamily(PopulationFollowFamily, population, populationEmpty, warn);
    Check(population.RosterSize == 2500 && population.IntervalSeconds == 300 && population.MeanSessionMinutes == 120 &&
              population.MinSessionMinutes == 30 && population.AcquaintedSharePct == 90 &&
              population.AcquaintedMinScore == 1 && population.RetireIntervalSeconds == 600 &&
              population.RetireMaxPerPass == 1 && population.RetireMaxPerHour == 10 &&
              population.RetireTestTarget == 0 && population.FollowTargets &&
              populationEmpty.missingWarnings.size() == 9,
          "population: the defaults; the nine listed rows (TestTarget and the role value too) warn when missing, "
          "the unlisted two do not");
    FakeReader populationReader;
    populationReader.values = {{"Playerbots.Population.Rotation.RosterSize", "77"},
                               {"Playerbots.Population.Rotation.IntervalSeconds", "1"},
                               {"Playerbots.Population.Rotation.MeanSessionMinutes", "0"},
                               {"Playerbots.Population.Rotation.MinSessionMinutes", "0"},
                               {"Playerbots.Population.Rotation.AcquaintedSharePct", "500"},
                               {"Playerbots.Population.Rotation.AcquaintedMinScore", "0"},
                               {"Playerbots.Population.Retire.IntervalSeconds", "1"},
                               {"Playerbots.Population.Retire.MaxPerPass", "4"},
                               {"Playerbots.Population.Retire.MaxPerHour", "0"},
                               {"Playerbots.Population.Retire.TestTarget", "12"},
                               {"Playerbots.Population.FollowTargets", "0"}};
    uint32_t const populationWarnings = LoadConfigFamily(PopulationRotationFamily, population, populationReader, warn) +
                                        LoadConfigFamily(PopulationRetireFamily, population, populationReader, warn) +
                                        LoadConfigFamily(PopulationFollowFamily, population, populationReader, warn);
    Check(populationWarnings == 5 && population.RosterSize == 77 && population.IntervalSeconds == 10 &&
              population.MeanSessionMinutes == 1 && population.MinSessionMinutes == 0 &&
              population.AcquaintedSharePct == 100 && population.AcquaintedMinScore == 1 &&
              population.RetireIntervalSeconds == 10 && population.RetireMaxPerPass == 4 &&
              population.RetireMaxPerHour == 0 && population.RetireTestTarget == 12 && !population.FollowTargets,
          "population: IntervalSeconds, MeanSessionMinutes, AcquaintedSharePct, AcquaintedMinScore and "
          "Retire.IntervalSeconds clamp with one warning each; the others keep their values");
}

// CS22e: the attribute loadings and spreads, one element row per attribute
void TestAttributeFamilies()
{
    BotAttributeParams attributes;
    attributes.Loading.fill(0.5f);
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };
    FakeReader reader;
    reader.values = {{"Playerbots.Skill.Attributes.Loading.Focus", "0.6"},
                     {"Playerbots.Skill.Attributes.Spread.Grit", "5"},
                     {"Playerbots.Skill.Attributes.Spread.Teamwork", "1"}};
    uint32 const warned = LoadConfigFamily(SkillAttributeLoadingFamily, attributes, reader, warn) +
                          LoadConfigFamily(SkillAttributeSpreadFamily, attributes, reader, warn);
    uint32 const focus = static_cast<uint32>(BotAttribute::Focus);
    uint32 const grit = static_cast<uint32>(BotAttribute::Grit);
    uint32 const teamwork = static_cast<uint32>(BotAttribute::Teamwork);
    bool defaults = true;
    for (uint32 index = 0; index < BOT_ATTRIBUTE_COUNT; ++index)
    {
        std::string const loading =
            std::string(SkillAttributeLoadingFamily.Prefix) + SkillAttributeLoadingFamily.Rows[index].Key;
        defaults = defaults && loading == std::string("Playerbots.Skill.Attributes.Loading.") +
                                              BOT_ATTRIBUTE_DEFINITIONS[index].Name;
        if (index != focus)
            defaults = defaults && attributes.Loading[index] == (IsBotPersonality(index) ? 0.0f : 1.0f);
        if (index != grit && index != teamwork)
            defaults = defaults && attributes.Spread[index] == 0.0f;
    }
    Check(defaults, "attributes: rows keyed by name, defaults 1 (abilities), 0 (traits) and spread 0");
    Check(warned == 1 && attributes.Loading[focus] == 0.6f && attributes.Spread[grit] == 3.0f &&
              attributes.Spread[teamwork] == 1.0f,
          "attributes: a loading and a spread read, a spread above 3 clamped with a warning");
    Check(reader.missingWarnings.empty(), "attributes: missing keys do not warn");
    std::array<char const*, 1> const corr = {"Attributes.Corr."};
    ConfigFamily<BotAttributeParams> const skillPrefix{"Playerbots.Skill.", SkillAttributeLoadingFamily.Rows};
    Check(KnowsConfigKey(skillPrefix, "Playerbots.Skill.Attributes.Corr.Wisdom-Teamwork", corr),
          "attributes: a Corr pair is a code-owned key");
}

// CS22e: skill growth (doubles read as floats, PotentialMax at least PotentialMin)
void TestGrowthFamily()
{
    SkillGrowthParams growth;
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };
    FakeReader empty;
    LoadConfigFamily(SkillGrowthFamily, growth, empty, warn);
    Check(!growth.Enable && growth.GainPerMatch == static_cast<double>(0.8f) && growth.RealPlayerFactor == 3.0 &&
              growth.PotentialMin == 10 && growth.PotentialMax == 40,
          "growth: the defaults, the gain as the old float read held it");
    Check(empty.missingWarnings.size() == 1 && empty.missingWarnings[0] == "Playerbots.Skill.Growth.Enable",
          "growth: only the listed Enable warns when missing");

    FakeReader reader;
    reader.values = {{"Playerbots.Skill.Growth.Enable", "1"},
                     {"Playerbots.Skill.Growth.GainPerMatch", "2.5"},
                     {"Playerbots.Skill.Growth.RealPlayerFactor", "0.5"},
                     {"Playerbots.Skill.Growth.PotentialMin", "50"},
                     {"Playerbots.Skill.Growth.PotentialMax", "20"}};
    Check(LoadConfigFamily(SkillGrowthFamily, growth, reader, warn) == 1 && growth.Enable &&
              growth.GainPerMatch == 2.5 && growth.RealPlayerFactor == 1.0 && growth.PotentialMax == 50,
          "growth: a factor below 1 clamped with a warning, PotentialMax raised to PotentialMin");
    reader.values["Playerbots.Skill.Growth.PotentialMax"] = "200";
    Check(LoadConfigFamily(SkillGrowthFamily, growth, reader, warn) == 2 && growth.PotentialMax == 98,
          "growth: PotentialMax above 98 clamped");
}

void TestRetiredFullKey()
{
    std::vector<std::string> const keys = {"Playerbots.Fake.Enable", "Playerbots.Fake.EnableX",
                                           "Playerbots.Fake.FlushSeconds"};
    Check(UnknownConfigKeys(FakeSwitchFamily, keys).empty(),
          "retired full key: exactly the key is in the family, a longer key and a sibling key are outside it");

    FakeSwitches switches;
    FakeReader reader;
    reader.values = {{"Playerbots.Fake.Enable", "0"}};
    std::vector<std::string> warnings;
    uint32_t const count = LoadConfigFamily(FakeSwitchFamily, switches, reader,
                                            [&warnings](std::string const& text) { warnings.push_back(text); });
    Check(count == 0 && warnings.empty() && reader.missingWarnings.empty() && !switches.Gone,
          "retired full key: a set line is ignored without a warning");

    // CS23d: a removed key whose parent prefix still holds live keys
    ConfigFamily<FakeSwitches> const dryRunFamily{"Playerbots.Population.Retire.DryRun", FakeSwitchRows};
    std::vector<std::string> const retireKeys = {"Playerbots.Population.Retire.DryRun",
                                                 "Playerbots.Population.Retire.DryRunX",
                                                 "Playerbots.Population.Retire.TestTarget"};
    Check(UnknownConfigKeys(dryRunFamily, retireKeys).empty(),
          "retired Retire.DryRun: the key is known, DryRunX and the live Retire.TestTarget are outside the family");

    // CS25d review: the live WearBetweenMatches starts with the retired PvpGear.Wear and is not its family's key
    ConfigFamily<FakeSwitches> const wearFamily{"Playerbots.PvpGear.Wear", FakeSwitchRows};
    std::vector<std::string> const wearKeys = {"Playerbots.PvpGear.Wear", "Playerbots.PvpGear.WearBetweenMatches"};
    Check(UnknownConfigKeys(wearFamily, wearKeys).empty(),
          "retired PvpGear.Wear: the key is known, the live WearBetweenMatches is outside the family");
}

int main(int argc, char** argv)
{
    std::filesystem::path const root = argc > 1 ? argv[1] : ".";
    std::ifstream file(root / "conf" / "playerbots.conf.dist", std::ios::binary);
    if (!file)
    {
        std::cout << "config-table: cannot read conf/playerbots.conf.dist under " << root.string() << "\n";
        return 1;
    }
    std::stringstream text;
    text << file.rdbuf();
    std::map<std::string, std::string> const dist = DistKeys(text.str());
    Check(dist.size() > 100, "conf.dist parsed");

    TestBankMailWalk();
    TestProfessionWalk();
    TestAuctionWalk();
    TestTypesWalk();
    TestLastFamilies();
    TestUnknownRetiredPrint();
    TestRetiredFullKey();
    TestSharedStruct();
    TestWideTypes();
    TestTacticFamilies();
    TestSkillFamilies();
    TestAttributeFamilies();
    TestGrowthFamily();
    TestReactionFamilies();
    TestNaturalMoveFamilies();
    TestWorldFamilies();
    TestDistChecks(dist);

    std::cout << "config-table: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
