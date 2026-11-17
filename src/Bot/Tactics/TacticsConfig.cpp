/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <array>
#include <string>

#include "ConfigTable.h"
#include "TacticIds.h"
#include "TacticScope.h"

namespace
{
using SettingsRow = ConfigRow<TacticSettings>;

constexpr uint32 MaxPercent = 100;

// The scope text into the mask; a refused text keeps the default (the definition's scope) with a warning
bool ParseScopeRow(std::string const& text, TacticSettings& out)
{
    std::string error;
    return ParseTacticScope(text, out.Scope, error);
}

// The five rows of every tactic; Enable is a test switch, or a not-yet switch for the tactics that do not work yet
constexpr std::array<SettingsRow, 5> TacticRows(ConfigKind enableKind)
{
    return {{
        {"Enable", &TacticSettings::Enable, 0, 0, enableKind},
        {"Scope", &TacticSettings::ScopeText, 0, 0, ConfigKind::Tuning, ConfigOutOfRange::Clamp, &ParseScopeRow},
        {"KnownFromSkill", &TacticSettings::KnownFromSkill, 0, MaxPercent},
        {"UseChanceAtSkill1", &TacticSettings::UseChanceAtSkill1, 0, MaxPercent},
        {"UseChanceAtSkill99", &TacticSettings::UseChanceAtSkill99, 0, MaxPercent},
    }};
}

constexpr std::array<SettingsRow, 5> TestTacticRows = TacticRows(ConfigKind::Test);
constexpr std::array<SettingsRow, 5> NotYetTacticRows = TacticRows(ConfigKind::NotYet);

// Drink (prep drinks flap) and Jump (no reliable jump spot) stay off until PvP refinement (config-classes.csv)
constexpr bool EnableNotYet(TacticId id) { return id == TacticId::Drink || id == TacticId::Jump; }

// The prefixes, defaults and families, built once from TACTIC_DEFINITIONS (the families point into them)
struct TacticFamilies
{
    std::array<std::string, TACTIC_COUNT> Prefixes;
    std::array<TacticSettings, TACTIC_COUNT> Defaults;
    std::array<ConfigFamily<TacticSettings>, TACTIC_COUNT> Families;

    TacticFamilies()
    {
        for (uint32 index = 0; index < TACTIC_COUNT; ++index)
        {
            TacticDefinition const& definition = TACTIC_DEFINITIONS[index];
            Prefixes[index] = std::string("Playerbots.Tactics.") + definition.ConfigName + ".";
            Defaults[index].Scope = definition.DefaultScope;
            Defaults[index].ScopeText = FormatTacticScope(definition.DefaultScope);
            Families[index].Prefix = Prefixes[index].c_str();
            Families[index].Rows = EnableNotYet(static_cast<TacticId>(index)) ? NotYetTacticRows : TestTacticRows;
            Families[index].Defaults = &Defaults[index];
        }
    }
};

// Playerbots.Tactics.Enable: the system switch (not yet, CS.10); the mask of the tactics enabled in config
constexpr std::array<ConfigRow<TacticsConfig>, 1> TacticsEnableRows = {{
    {"", &TacticsConfig::Enable, 0, 0, ConfigKind::NotYet},
}};

void FinishTacticsEnable(TacticsConfig& out)
{
    out.ConfigMask = 0;
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
        if (out.Enable && out.Settings[index].Enable)
            out.ConfigMask |= TacticBit(static_cast<TacticId>(index));
}
}  // namespace

ConfigFamily<TacticSettings> const& TacticSettingsFamily(uint32 index)
{
    static TacticFamilies const families;
    return families.Families[index];
}

ConfigFamily<TacticsConfig> const TacticsEnableFamily{"Playerbots.Tactics.Enable", TacticsEnableRows,
                                                      &FinishTacticsEnable};
