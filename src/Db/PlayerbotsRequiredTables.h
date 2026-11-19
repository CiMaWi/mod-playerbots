/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOTS_REQUIRED_TABLES_H
#define _PLAYERBOTS_REQUIRED_TABLES_H

#include <array>
#include <set>
#include <string>
#include <vector>

// The tables and columns the module's stores read, each with the dated update (data/sql/playerbots/updates/) that
// makes it. A database that lacks one refuses the start. Game-free, so an offline selftest can feed it
namespace PlayerbotsRequiredTables
{
struct Requirement
{
    char const* Table;
    char const* Column;  // empty: the table itself
    char const* Update;  // the update file's name without .sql
};

inline constexpr std::array<Requirement, 10> Requirements = {{
    {"playerbots_bot_profile", "", "2026_10_23_00_playerbots_bot_profile"},
    {"playerbots_acquaintance", "", "2026_10_25_00_playerbots_acquaintance"},
    {"playerbots_ladder_roster", "", "2026_10_26_00_playerbots_ladder_roster"},
    {"playerbots_pvp_loadout", "", "2026_10_26_01_playerbots_pvp_gear"},
    {"playerbots_pvp_worn", "", "2026_10_26_01_playerbots_pvp_gear"},
    {"playerbots_bot_profile", "percentile", "2026_10_27_00_playerbots_bot_profile_percentile"},
    {"playerbots_bot_traits", "", "2026_11_09_00_playerbots_bot_traits"},
    {"playerbots_quest_stats", "", "2026_11_09_01_playerbots_quest_stats"},
    {"playerbots_bot_record", "", "2026_11_14_00_playerbots_bot_record"},
    {"playerbots_quest_poi_z", "", "2026_11_19_00_playerbots_quest_poi_z"},
}};

// A present table ("table") or column ("table.column"), as the startup query fills the set
inline std::string Key(std::string const& table, std::string const& column)
{
    return column.empty() ? table : table + "." + column;
}

// The requirements whose table or column is not present, in update order
inline std::vector<Requirement> Missing(std::set<std::string> const& present)
{
    std::vector<Requirement> missing;
    for (Requirement const& requirement : Requirements)
        if (!present.count(Key(requirement.Table, requirement.Column)))
            missing.push_back(requirement);

    return missing;
}

// Startup, before the statements are prepared: one information_schema query; logs one error per missing item with
// its update and returns false when any is missing
bool Check();
}  // namespace PlayerbotsRequiredTables

#endif
