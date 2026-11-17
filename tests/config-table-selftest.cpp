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
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ArenaCcConfig.h"
#include "ArenaCoverConfig.h"
#include "ArenaDefensiveConfig.h"
#include "ArenaFocusConfig.h"
#include "ArenaInterruptConfig.h"
#include "ArenaKiteConfig.h"
#include "BankMailConfig.h"
#include "ConfigTable.h"
#include "NaturalCastConfig.h"
#include "SkillConfig.h"
#include "TacticGateConfig.h"
#include "TacticIds.h"
#include "TacticScope.h"

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
    config.VisitStepSeconds = 99;
    std::vector<std::string> warnings;
    auto const warn = [&warnings](std::string const& text) { warnings.push_back(text); };

    FakeReader empty;
    Check(LoadConfigFamily(BankMailFamily, config, empty, warn) == 0 && warnings.empty(),
          "BankMail: no key, no warning");
    BankMailConfig const defaults{};
    Check(config.MailsPerVisit == defaults.MailsPerVisit && config.VisitStepSeconds == defaults.VisitStepSeconds &&
              config.MailDefaultWorthCopper == 100 && config.KeepLevelHorizon == 5 &&
              config.MailVisitDeadlineSeconds == 900 && config.BankVisitDeadlineSeconds == 900,
          "BankMail: the walk resets to the initializers");
    Check(empty.missingWarnings.empty(), "BankMail: unlisted rows read without the missing-key warning");

    FakeReader probe;
    probe.values = {{"Playerbots.BankMail.Mail.DefaultWorthCopper", "250"},
                    {"Playerbots.BankMail.Mail.VisitDeadlineSeconds", "1200"},
                    {"Playerbots.BankMail.Bank.VisitDeadlineSeconds", "30"},
                    {"Playerbots.BankMail.VisitStepSeconds", "0"},
                    {"Playerbots.BankMail.Mail.PerVisit", "500"},
                    {"Playerbots.BankMail.Bank.KeepLevelHorizon", "99"}};
    Check(LoadConfigFamily(BankMailFamily, config, probe, warn) == 4 && warnings.size() == 4,
          "BankMail: four values outside their bounds, four warnings");
    Check(config.MailDefaultWorthCopper == 250 && config.MailVisitDeadlineSeconds == 1200,
          "BankMail: values in range are kept");
    Check(config.BankVisitDeadlineSeconds == 60 && config.VisitStepSeconds == 1,
          "BankMail: clamped up to the lower bound");
    Check(config.MailsPerVisit == 100 && config.KeepLevelHorizon == 20, "BankMail: clamped down to the upper bound");
    Check(std::find(warnings.begin(), warnings.end(),
                    "Playerbots.BankMail.Bank.VisitDeadlineSeconds = 30 is outside 60 to 3600, using 60") !=
              warnings.end(),
          "BankMail: the warning names key, value, bounds and result");
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

void TestDistChecks(std::map<std::string, std::string> const& dist)
{
    std::map<std::string, int> seen;
    std::vector<std::string> problems = RowProblems(BankMailFamily, seen);
    std::vector<std::string> const distProblems = DistProblems(BankMailFamily, dist);
    problems.insert(problems.end(), distProblems.begin(), distProblems.end());
    for (std::string const& problem : problems)
        std::cout << "  " << problem << "\n";
    Check(problems.empty(), "BankMail: rows unique, defaults in bounds, conf.dist agrees with the rows");
    Check(BankMailFamily.Rows.size() == 6, "BankMail: six rows");

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
    };
    std::set<std::string> known;
    // The keys code reads outside the tables
    for (auto const& [key, value] : dist)
        for (char const* prefix : {"Playerbots.Skill.Attributes.", "Playerbots.Skill.Growth."})
            if (key.rfind(prefix, 0) == 0)
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
    for (char const* key : {"Playerbots.RandomBotArenaGroupOnWorldThread", "Playerbots.RandomBotArenaTeamRatingMode",
                            "Playerbots.RandomBotArenaTeamMinRating", "Playerbots.RandomBotArenaTeamMaxRating",
                            "Playerbots.Ladder.Enable", "Playerbots.Natural.Cadence.NearRealPlayerYards",
                            "Playerbots.Ladder.Matchmaker.Enable"})
        Check(dist.count(key) == 0, std::string("retired: ") + key + " has no dist line");

    // The checks themselves, on a fixture
    std::map<std::string, std::string> const fixture = DistKeys(
        "Playerbots.BankMail.NoRow = 1\nPlayerbots.BankMail.VisitStepSeconds = 10\n# Playerbots.BankMail.Old = 2\n"
        "Test.Types.Signed = -4\n");
    std::vector<std::string> const bank = DistProblems(BankMailFamily, fixture);
    Check(bank.size() == 2 && bank[0].find("VisitStepSeconds: unlisted") != std::string::npos &&
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
    TestTypesWalk();
    TestUnknownRetiredPrint();
    TestRetiredFullKey();
    TestSharedStruct();
    TestWideTypes();
    TestTacticFamilies();
    TestSkillFamilies();
    TestDistChecks(dist);

    std::cout << "config-table: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
