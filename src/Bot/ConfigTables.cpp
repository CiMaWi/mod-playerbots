/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ConfigTables.h"

#include <array>
#include <functional>
#include <vector>

#include "BankMailConfig.h"
#include "Chat.h"
#include "Config.h"
#include "ConfigTable.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"
#include "ScriptMgr.h"

namespace
{
// A family with the struct it fills, type-erased; one line in Registry() per family
struct ConfigTableEntry
{
    std::string Prefix;
    std::function<void()> Load;
    std::function<std::vector<std::string>(std::vector<std::string> const&)> Unknown;
    // the lines of the rows whose full key starts with the filter
    std::function<void(std::string const&, std::vector<std::string>&)> Print;
};

void WarnConfigRow(std::string const& text) { LOG_WARN("server.loading", "{}", text); }

template <typename Struct>
ConfigTableEntry MakeEntry(ConfigFamily<Struct> const& family, Struct& target)
{
    ConfigTableEntry entry;
    entry.Prefix = family.Prefix;
    entry.Load = [&family, &target]()
    {
        LoadConfigFamily(
            family, target, [](std::string const& key, auto fallback, bool warnIfMissing)
            { return sConfigMgr->GetOption<decltype(fallback)>(key, fallback, warnIfMissing); }, WarnConfigRow);
    };
    entry.Unknown = [&family](std::vector<std::string> const& keys) { return UnknownConfigKeys(family, keys); };
    entry.Print = [&family, &target](std::string const& filter, std::vector<std::string>& lines)
    {
        for (ConfigRow<Struct> const& row : family.Rows)
        {
            if (row.Kind == ConfigKind::Retired)
                continue;
            if ((std::string(family.Prefix) + row.Key).rfind(filter, 0) == 0)
                lines.push_back(ConfigRowLine(family, row, target));
        }
    };
    return entry;
}

// Removed switches: one family per full key with one empty-key row, so exactly that key is known and a line that
// still sets it is ignored without a warning (the parent prefixes hold live keys read elsewhere)
struct RetiredSwitches
{
    bool Acquaintance = false;
    bool SkipIdle = false;
    bool Goals = false;
};

RetiredSwitches RetiredSwitchValues;

constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredAcquaintanceRows = {{
    {"", &RetiredSwitches::Acquaintance, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredAcquaintanceFamily{"Playerbots.Acquaintance.Enable",
                                                              RetiredAcquaintanceRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredSkipIdleRows = {{
    {"", &RetiredSwitches::SkipIdle, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredSkipIdleFamily{"Playerbots.SkipIdleTriggerChecks", RetiredSkipIdleRows};
constexpr std::array<ConfigRow<RetiredSwitches>, 1> RetiredGoalsRows = {{
    {"", &RetiredSwitches::Goals, 0, 0, ConfigKind::Retired},
}};
ConfigFamily<RetiredSwitches> const RetiredGoalsFamily{"Playerbots.WorldTravel.Planner.Goals", RetiredGoalsRows};

// CS22c-j add one line per family
std::vector<ConfigTableEntry> const& Registry()
{
    static std::vector<ConfigTableEntry> const entries = {
        MakeEntry(BankMailFamily, sPlayerbotAIConfig.BankMail),
        MakeEntry(RetiredAcquaintanceFamily, RetiredSwitchValues),
        MakeEntry(RetiredSkipIdleFamily, RetiredSwitchValues),
        MakeEntry(RetiredGoalsFamily, RetiredSwitchValues),
    };
    return entries;
}

class ConfigTablesWorldScript : public WorldScript
{
public:
    ConfigTablesWorldScript() : WorldScript("ConfigTablesWorldScript", {WORLDHOOK_ON_AFTER_CONFIG_LOAD}) {}

    // The first load is covered by PlayerbotAIConfig::Initialize; `.reload config` does not call it
    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            LoadConfigTables();
    }
};
}  // namespace

void LoadConfigTables()
{
    for (ConfigTableEntry const& entry : Registry())
    {
        entry.Load();
        for (std::string const& key : entry.Unknown(sConfigMgr->GetKeysByString(entry.Prefix)))
            WarnConfigRow(key + " is set but not a known key of " + entry.Prefix + "*, ignored");
    }
}

void PrintConfigTables(ChatHandler* handler, std::string const& prefix)
{
    std::vector<std::string> lines;
    for (ConfigTableEntry const& entry : Registry())
        if (entry.Prefix.rfind(prefix, 0) == 0 || prefix.rfind(entry.Prefix, 0) == 0)
            entry.Print(prefix, lines);

    if (lines.empty())
    {
        handler->PSendSysMessage("CONFIG 0 rows: no config rows under '{}'", prefix);
        return;
    }
    handler->PSendSysMessage("CONFIG {} rows (* = differs from the default)", lines.size());
    for (std::string const& line : lines)
        handler->PSendSysMessage("{}", line);
}

void AddSC_config_tables() { new ConfigTablesWorldScript(); }
