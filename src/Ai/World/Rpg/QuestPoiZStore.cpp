/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestPoiZStore.h"

#include <string>

#include "DatabaseEnv.h"
#include "Log.h"
#include "PlayerbotsDatabase.h"
#include "QueryResult.h"

void QuestPoiZStore::Load()
{
    _candidates.clear();
    PreparedQueryResult result =
        PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_QUEST_POI_Z));
    if (!result)
    {
        LOG_INFO("server.loading",
                 "Quest POI floors: playerbots_quest_poi_z is empty; objectives with only a POI have no place");
        return;
    }
    uint32 rows = 0;
    do
    {
        Field* fields = result->Fetch();
        Candidate candidate;
        candidate.pointIdx = fields[2].Get<uint16>();
        candidate.rank = fields[3].Get<uint8>();
        candidate.z = fields[4].Get<float>();
        candidate.confidence = fields[5].Get<uint8>();
        candidate.navmeshOk = fields[6].Get<uint8>() != 0;
        // PoiIdx and MapId are SMALLINT: a 32-bit read would take the neighbouring field's bytes (#3031's fix)
        candidate.mapId = fields[7].Get<uint16>();
        candidate.trigger = fields[8].Get<std::string>().find("trigger") != std::string::npos;
        Add(fields[0].Get<uint32>(), fields[1].Get<uint16>(), candidate);
        ++rows;
    } while (result->NextRow());
    Order();
    LOG_INFO("server.loading", "Quest POI floors: loaded {} candidate rows for {} quest POIs", rows,
             _candidates.size());
}
