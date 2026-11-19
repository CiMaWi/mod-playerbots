/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotsRequiredTables.h"

#include "Log.h"
#include "PlayerbotsDatabase.h"
#include "QueryResult.h"
#include "StringFormat.h"

bool PlayerbotsRequiredTables::Check()
{
    std::set<std::string> tables;
    for (Requirement const& requirement : Requirements)
        tables.insert(requirement.Table);

    std::string list;
    for (std::string const& table : tables)
        list += Acore::StringFormat("{}'{}'", list.empty() ? "" : ", ", table);

    std::set<std::string> present;
    if (QueryResult result = PlayerbotsDatabase.Query(Acore::StringFormat(
            "SELECT TABLE_NAME, COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND "
            "TABLE_NAME IN ({})",
            list)))
    {
        do
        {
            Field* fields = result->Fetch();
            std::string const table = fields[0].Get<std::string>();
            present.insert(table);
            present.insert(Key(table, fields[1].Get<std::string>()));
        } while (result->NextRow());
    }

    std::vector<Requirement> const missing = Missing(present);
    for (Requirement const& requirement : missing)
        LOG_ERROR("server.playerbots",
                  "Playerbots database lacks {} {}: apply data/sql/playerbots/updates/{}.sql (on a lab: ac.js db apply "
                  "<lab> <file> --db playerbots, then remake its pool)",
                  *requirement.Column ? "column" : "table", Key(requirement.Table, requirement.Column),
                  requirement.Update);

    if (!missing.empty())
        LOG_ERROR("server.playerbots",
                  "Playerbots: {} required tables or columns are missing; the server does not start", missing.size());

    return missing.empty();
}
