/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DangerRules.h"
#include "Log.h"
#include "PlayerbotsDatabase.h"
#include "QueryResult.h"
#include "Timer.h"

bool DangerRules::Load()
{
    std::uint32_t const started = getMSTime();
    std::vector<BuildRow> builds;
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT build, cell_yards, height_reach_yards, row_count, max_layers FROM playerbots_known_danger_build"))
    {
        do
        {
            Field* fields = result->Fetch();
            BuildRow build;
            build.build = fields[0].Get<std::string>();
            build.cellYards = fields[1].Get<uint16>();
            build.heightReachYards = fields[2].Get<uint16>();
            build.rowCount = fields[3].Get<uint32>();
            build.maxLayers = fields[4].Get<uint8>();
            builds.push_back(build);
        } while (result->NextRow());
    }

    std::vector<Row> rows;
    if (builds.size() == 1)
        rows.reserve(builds.front().rowCount);
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT map, cx, cy, side, layer, z_lo, z_hi, lvl_n, lvl_e, "
                                                      "lvl_r, lvl_b, cnt_n, cnt_e, cnt_r, cnt_b, guards "
                                                      "FROM playerbots_known_danger"))
    {
        do
        {
            Field* fields = result->Fetch();
            Row row;
            row.map = fields[0].Get<uint16>();
            row.cx = fields[1].Get<int16>();
            row.cy = fields[2].Get<int16>();
            row.side = fields[3].Get<uint8>();
            row.layer = fields[4].Get<uint8>();
            row.value.zLo = fields[5].Get<int16>();
            row.value.zHi = fields[6].Get<int16>();
            for (std::size_t r = 0; r < RANK_COUNT; ++r)
            {
                row.value.facts.level[r] = fields[7 + r].Get<uint8>();
                row.value.facts.count[r] = fields[7 + RANK_COUNT + r].Get<uint8>();
            }
            row.value.facts.guards = fields[15].Get<uint8>() != 0;
            rows.push_back(row);
        } while (result->NextRow());
    }

    std::size_t const rowCount = rows.size();
    auto known = std::make_unique<Known>();
    std::string const refusal = Assemble(builds, std::move(rows), *known);
    if (!refusal.empty())
    {
        LOG_ERROR("server.playerbots",
                  "Playerbots: the known danger table is refused ({}). Build its rows with the hub's "
                  "tools/awareness/danger-build.js and apply them (on a lab: ac.js db apply <lab> <file> --db "
                  "playerbots, then remake its pool); the server does not start",
                  refusal);
        return false;
    }

    LOG_INFO("server.loading", ">> Loaded known danger {}: {} rows, {} cells and sides, {:.1f} MB in {} ms",
             known->build, rowCount, known->keys.size(), known->Bytes() / 1048576.0, GetMSTimeDiffToNow(started));
    Publish(std::move(known));
    return true;
}
