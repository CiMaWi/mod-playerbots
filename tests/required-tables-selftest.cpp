/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline selftest of the startup's required tables (src/Db/PlayerbotsRequiredTables.h): the missing-item decision on
// fake sets of present tables and columns, and every requirement against its update file (argv[1] = the module root).

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "PlayerbotsRequiredTables.h"

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
        ++passed;
    else
    {
        ++failed;
        std::cout << "FAIL: " << name << "\n";
    }
}

std::set<std::string> AllPresent()
{
    std::set<std::string> present;
    for (PlayerbotsRequiredTables::Requirement const& requirement : PlayerbotsRequiredTables::Requirements)
    {
        present.insert(requirement.Table);
        present.insert(PlayerbotsRequiredTables::Key(requirement.Table, requirement.Column));
    }
    return present;
}

std::vector<std::string> Updates(std::vector<PlayerbotsRequiredTables::Requirement> const& missing)
{
    std::vector<std::string> updates;
    for (PlayerbotsRequiredTables::Requirement const& requirement : missing)
        updates.push_back(requirement.Update);
    return updates;
}

// The present keys without a table (and all its columns) or one column
std::set<std::string> Without(std::set<std::string> present, std::string const& table, std::string const& column)
{
    if (!column.empty())
    {
        present.erase(table + "." + column);
        return present;
    }

    for (auto it = present.begin(); it != present.end();)
        if (*it == table || it->rfind(table + ".", 0) == 0)
            it = present.erase(it);
        else
            ++it;
    return present;
}
}  // namespace

int main(int argc, char** argv)
{
    using PlayerbotsRequiredTables::Missing;

    Check(Missing(AllPresent()).empty(), "a complete database lacks nothing");
    Check(Missing({}).size() == PlayerbotsRequiredTables::Requirements.size(), "an empty database lacks everything");

    // The CS27a exit's drop: the quest statistics table and the profile's percentile column, in update order
    std::set<std::string> const dropped =
        Without(Without(AllPresent(), "playerbots_quest_stats", ""), "playerbots_bot_profile", "percentile");
    Check(Updates(Missing(dropped)) == std::vector<std::string>{"2026_10_27_00_playerbots_bot_profile_percentile",
                                                                "2026_11_09_01_playerbots_quest_stats"},
          "the dropped table and column name their two updates");

    // A missing profile table lacks its column too: both updates are due
    Check(Updates(Missing(Without(AllPresent(), "playerbots_bot_profile", ""))) ==
              std::vector<std::string>{"2026_10_23_00_playerbots_bot_profile",
                                       "2026_10_27_00_playerbots_bot_profile_percentile"},
          "a missing profile table names the table's and the column's update");

    // A column of another table with the same name does not count
    std::set<std::string> other = Without(AllPresent(), "playerbots_bot_profile", "percentile");
    other.insert("playerbots_bot_traits.percentile");
    Check(Missing(other).size() == 1, "the percentile column counts only on the profile table");

    for (char const* table : {"playerbots_bot_traits", "playerbots_bot_record", "playerbots_acquaintance",
                              "playerbots_ladder_roster", "playerbots_pvp_loadout", "playerbots_pvp_worn"})
        Check(Missing(Without(AllPresent(), table, "")).size() == 1, std::string("one item for a missing ") + table);

    // Every requirement names an update file that creates its table or adds its column
    if (argc < 2)
        Check(false, "the module root is argv[1]");
    else
        for (PlayerbotsRequiredTables::Requirement const& requirement : PlayerbotsRequiredTables::Requirements)
        {
            std::filesystem::path const file = std::filesystem::path(argv[1]) / "data" / "sql" / "playerbots" /
                                               "updates" / (std::string(requirement.Update) + ".sql");
            std::ifstream in(file);
            std::stringstream text;
            text << in.rdbuf();
            std::string const sql = text.str();
            std::string const key = PlayerbotsRequiredTables::Key(requirement.Table, requirement.Column);
            Check(in.is_open(), key + ": the update file exists");
            Check(sql.find(requirement.Table) != std::string::npos, key + ": the update names the table");
            Check(!*requirement.Column || sql.find(requirement.Column) != std::string::npos,
                  key + ": the update names the column");
        }

    std::cout << "required-tables: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
