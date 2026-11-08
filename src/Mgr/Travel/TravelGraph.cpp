/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TravelGraph.h"

#include "Chat.h"
#include "DBCStores.h"
#include "Field.h"
#include "Log.h"
#include "PlayerbotsDatabase.h"
#include "QueryResult.h"
#include "RouteRules.h"
#include "StringFormat.h"
#include "Timer.h"

namespace
{
// Path points are read in node id ranges of this size (about 37,000 rows each): the loader never holds the 1.4 million
// row table as one result, and the pool's one synchronous connection (PlayerbotsDatabase.SynchThreads = 1) is free
// again for the world thread after each chunk
constexpr uint32 PATH_CHUNK_NODES = 100;
}  // namespace

TravelGraph& TravelGraph::Instance()
{
    static TravelGraph instance;
    return instance;
}

void TravelGraph::Start()
{
    State expected = State::Idle;
    if (!_state.compare_exchange_strong(expected, State::Loading))
        return;

    _loader = std::thread([this] { Load(); });
}

void TravelGraph::Shutdown()
{
    _stop.store(true);
    if (_loader.joinable())
        _loader.join();
}

// The loader thread: plain queries on the playerbots pool's synchronous connections, one at a time, nothing else
void TravelGraph::Load()
{
    uint32 const startMs = getMSTime();
    auto const fail = [this](std::string const& why)
    {
        _error = why;
        _state.store(State::Failed, std::memory_order_release);
        LOG_ERROR("playerbots", "[Route] travel graph not loaded: {}", why);
    };

    TravelGraphBuilder builder;
    uint32 rows[3]{};
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT id, name, map_id, x, y, z FROM playerbots_travelnode"))
    {
        do
        {
            Field* fields = result->Fetch();
            builder.AddNode(fields[0].Get<uint32>(), fields[1].Get<std::string>(), fields[2].Get<uint32>(),
                            fields[3].Get<float>(), fields[4].Get<float>(), fields[5].Get<float>());
            ++rows[0];
        } while (result->NextRow());
    }

    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT node_id, to_node_id, type, object, distance, swim_distance, extra_cost, max_creature_0, "
            "max_creature_1, max_creature_2 FROM playerbots_travelnode_link"))
    {
        do
        {
            Field* fields = result->Fetch();
            TravelGraphBuilder::LinkRow row;
            row.fromId = fields[0].Get<uint32>();
            row.toId = fields[1].Get<uint32>();
            row.type = fields[2].Get<uint8>();
            row.object = fields[3].Get<uint32>();
            row.distance = fields[4].Get<float>();
            row.swim = fields[5].Get<float>();
            row.extraCost = fields[6].Get<float>();
            for (uint32 i = 0; i < 3; ++i)
                row.maxCreature[i] = fields[7 + i].Get<uint8>();
            builder.AddLink(row);
            ++rows[1];
        } while (result->NextRow());
    }

    // The flight links' TaxiPaths and who may use them: the DBC stores are loaded before OnBeforeWorldInitialized
    // starts this thread and never change afterwards
    for (uint32 id = 0; id < sTaxiNodesStore.GetNumRows(); ++id)
        if (TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(id))
            builder.AddTaxiNode(id, node->MountCreatureID[1] != 0, node->MountCreatureID[0] != 0);
    for (uint32 id = 0; id < sTaxiPathStore.GetNumRows(); ++id)
        if (TaxiPathEntry const* path = sTaxiPathStore.LookupEntry(id))
            builder.AddTaxiPath(id, path->from, path->to, path->price);

    std::string error;
    if (!builder.EndLinks(error))
        return fail(error);

    // A failed chunk query looks like an empty node range: the point count read must match the table's
    uint64 expectedPoints = 0;
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT COUNT(*) FROM playerbots_travelnode_path"))
        expectedPoints = result->Fetch()[0].Get<uint64>();
    if (!expectedPoints)
        return fail("the path point count could not be read");
    builder.ReservePoints(expectedPoints);

    // Points of a node id past the last node would have no link anyway
    uint32 peakChunkRows = 0;
    for (uint32 first = 0; first <= builder.MaxNodeId(); first += PATH_CHUNK_NODES)
    {
        if (_stop.load())
        {
            _state.store(State::Stopped, std::memory_order_release);
            LOG_INFO("playerbots", "[Route] travel graph load stopped by the shutdown");
            return;
        }

        if (QueryResult result = PlayerbotsDatabase.Query(
                "SELECT node_id, to_node_id, nr, map_id, x, y, z FROM playerbots_travelnode_path WHERE node_id >= {} "
                "AND node_id < {} ORDER BY node_id, to_node_id, nr",
                first, first + PATH_CHUNK_NODES))
        {
            peakChunkRows = std::max<uint32>(peakChunkRows, static_cast<uint32>(result->GetRowCount()));
            do
            {
                Field* fields = result->Fetch();
                builder.AddPoint(fields[0].Get<uint32>(), fields[1].Get<uint32>(), fields[2].Get<uint32>(),
                                 fields[3].Get<uint32>(), fields[4].Get<float>(), fields[5].Get<float>(),
                                 fields[6].Get<float>());
                ++rows[2];
            } while (result->NextRow());
        }
        std::this_thread::yield();
    }

    if (rows[2] != expectedPoints)
        return fail(Acore::StringFormat("{} of {} path points read (a chunk query failed)", rows[2], expectedPoints));

    if (!builder.Finish(*this, error))
        return fail(error);

    uint32 const loadMs = GetMSTimeDiffToNow(startMs);
    LOG_INFO("playerbots", "[Route] travel graph: {} nodes, {} links, {} points loaded in {} ms, {:.1f} MB",
             _stats.nodes, _stats.links, _stats.points, loadMs,
             static_cast<double>(_stats.memoryBytes) / (1024.0 * 1024.0));
    LOG_INFO("playerbots",
             "[Route] travel graph rows read: {} nodes, {} links, {} points; largest path chunk {} rows; {} links "
             "skipped, {} points without a link; {} taxi paths, {} flight links without one",
             rows[0], rows[1], rows[2], peakChunkRows, _stats.skippedLinks, _stats.orphanPoints, _stats.taxiPaths,
             _stats.flightsWithoutTaxi);
    Publish(loadMs, peakChunkRows);
}

bool TravelGraph::HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
{
    TravelGraph const& graph = Instance();
    State const state = graph._state.load(std::memory_order_acquire);
    if (state != State::Ready)
    {
        char const* text = "loading";
        if (state == State::Idle)
            text = "not started";
        else if (state == State::Stopped)
            text = "stopped";
        else if (state == State::Failed)
            text = "failed";
        handler->PSendSysMessage("ROUTE status: travel graph {}{}", text,
                                 state == State::Failed ? " (" + graph._error + ")" : "");
        return true;
    }

    TravelGraphStats const& stats = graph.Stats();
    handler->PSendSysMessage(
        "ROUTE status: travel graph ready: {} nodes, {} links (walk {}, portal {}, transport {}, "
        "flight {}), {} points, {:.1f} MB, loaded in {} ms, largest path chunk {} rows",
        stats.nodes, stats.links, stats.linksByType[1], stats.linksByType[2], stats.linksByType[3],
        stats.linksByType[4], stats.points, static_cast<double>(stats.memoryBytes) / (1024.0 * 1024.0), stats.loadMs,
        stats.peakChunkRows);
    return true;
}

bool TravelGraph::HandleSelftestCommand(ChatHandler* handler, char const* /*args*/)
{
    TravelGraph const& graph = Instance();
    if (!graph.IsReady())
    {
        handler->PSendSysMessage("ROUTE selftest: the travel graph is not ready (see playerbots route status)");
        return true;
    }

    std::vector<std::string> failures;
    std::vector<std::string> notes;
    // The graph's own checks, then the route search's fixed snapshots (RouteRules.h)
    uint32 const checks = graph.Selftest(failures, notes) + TravelRoute::Selftest(graph, failures, notes);
    for (std::string const& note : notes)
        handler->PSendSysMessage("ROUTE {}", note);
    for (std::string const& failure : failures)
        handler->PSendSysMessage("ROUTE selftest FAIL: {}", failure);

    handler->PSendSysMessage("ROUTE selftest: {} checks, {} failures{}", checks, failures.size(),
                             failures.empty() ? " (PASS)" : "");
    return true;
}
