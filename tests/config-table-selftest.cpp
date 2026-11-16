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

#include "BankMailConfig.h"
#include "ConfigTable.h"

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
                                      std::map<std::string, std::string> const& dist)
{
    std::vector<std::string> problems;
    Struct const defaults{};
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
        if (key.rfind(family.Prefix, 0) == 0 && !rowKeys.count(key))
            problems.push_back(key + ": dist key without a row");
    return problems;
}

template <typename Struct>
std::vector<std::string> RowProblems(ConfigFamily<Struct> const& family, std::map<std::string, int>& seen)
{
    std::vector<std::string> problems;
    Struct const defaults{};
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
                using T = std::remove_cvref_t<decltype(defaults.*member)>;
                if constexpr (!std::is_same_v<T, std::string> && !std::is_same_v<T, bool>)
                {
                    double const value = static_cast<double>(defaults.*member);
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

struct FakeConfig
{
    uint32_t A = 15;
    float Ratio = 0.5f;
    uint32_t Old = 3;
    bool Flag = false;
};

constexpr std::array<ConfigRow<FakeConfig>, 4> FakeRows = {{
    {"A", &FakeConfig::A, 0, 100},
    {"Ratio", &FakeConfig::Ratio},
    {"Old", &FakeConfig::Old, 0, 10, ConfigKind::Retired},
    {"Flag", &FakeConfig::Flag, 0, 0, ConfigKind::Test},
}};

ConfigFamily<FakeConfig> const FakeFamily{"Playerbots.Fake.", FakeRows};

void TestUnknownRetiredPrint()
{
    std::vector<std::string> const keys = {"Playerbots.Fake.A", "Playerbots.Fake.NoSuchKey", "Playerbots.Fake.Old",
                                           "Playerbots.Other.NoSuchKey", "Playerbots.Fake."};
    std::vector<std::string> const unknown = UnknownConfigKeys(FakeFamily, keys);
    Check(unknown == std::vector<std::string>{"Playerbots.Fake.NoSuchKey", "Playerbots.Fake."},
          "unknown: only the key under the prefix with no row is reported, a key outside the prefix is not");
    Check(UnknownConfigKeys(FakeFamily, {"Playerbots.Fake.Old"}).empty(), "retired: a retired row knows its key");

    FakeConfig config;
    FakeReader reader;
    reader.values = {{"Playerbots.Fake.Old", "9"}, {"Playerbots.Fake.A", "20"}};
    std::vector<std::string> warnings;
    uint32_t const count = LoadConfigFamily(FakeFamily, config, reader,
                                            [&warnings](std::string const& text) { warnings.push_back(text); });
    Check(count == 0 && warnings.empty(), "retired: a retired key set in the config gives no warning");
    Check(config.Old == 3 && config.A == 20, "retired: the member keeps its initializer, other rows are read");
    Check(reader.missingWarnings == std::vector<std::string>{"Playerbots.Fake.Flag"},
          "retired: the walk never reads the retired key");

    FakeConfig const defaults;
    Check(ConfigRowLine(FakeFamily, FakeRows[0], defaults) ==
              "Playerbots.Fake.A = 15 (default 15, bounds 0 to 100, tuning)",
          "print: a default value has no mark");
    Check(ConfigRowLine(FakeFamily, FakeRows[0], config) ==
              "Playerbots.Fake.A = 20 * (default 15, bounds 0 to 100, tuning)",
          "print: a changed value is marked");
    Check(ConfigRowLine(FakeFamily, FakeRows[1], defaults) ==
              "Playerbots.Fake.Ratio = 0.5 (default 0.5, bounds -inf to inf, tuning)",
          "print: a float prints as 0.5, an open bound as inf");
    Check(ConfigRowLine(FakeFamily, FakeRows[3], defaults) == "Playerbots.Fake.Flag = 0 (default 0, test)",
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
    Check(dist.count("Playerbots.Population.Persistent") == 0,
          "retired: Playerbots.Population.Persistent has no dist line");

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
    TestDistChecks(dist);

    std::cout << "config-table: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
