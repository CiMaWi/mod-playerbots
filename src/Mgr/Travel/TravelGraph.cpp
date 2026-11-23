/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TravelGraph.h"

#include "Chat.h"
#include "DBCStores.h"
#include "Field.h"
#include "GameObjectData.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PlayerbotsDatabase.h"
#include "QueryResult.h"
#include "RouteCheckStopsRules.h"
#include "RouteSelftest.h"
#include "RouteService.h"
#include "StringFormat.h"
#include "Timer.h"
#include "TransportMgr.h"
#include "TravelGraphSelftest.h"
#include "TravelPlaces.h"

namespace
{
// Path points are read in node id ranges of this size (about 37,000 rows each): the loader never holds the 1.4 million
// row table as one result, and the pool's one synchronous connection (PlayerbotsDatabase.SynchThreads = 1) is free
// again for the world thread after each chunk
constexpr uint32 PATH_CHUNK_NODES = 100;

// A transport link's object must be a transport gameobject (a ship, zeppelin, elevator or tram car): the follower
// looks for it among the map's transports; object 0 is a wait point's link to its stop. The templates are loaded before
// OnBeforeWorldInitialized starts the loader and never change afterwards
bool IsTransportObject(uint32 entry)
{
    GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(entry);
    return object && (object->type == GAMEOBJECT_TYPE_TRANSPORT || object->type == GAMEOBJECT_TYPE_MO_TRANSPORT);
}

// A stop's position as the graph stores it, for matching a vessel's stop frames
struct StopPlace
{
    uint32 mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// How far a graph stop may be from the vessel's stop frame it is matched to (yards; a ship stops beside its dock)
constexpr float STOP_MATCH_YARDS = 300.0f;

// The vessel's stop frame nearest to a graph stop; null when none is within yards
KeyFrame const* NearestStopFrame(TransportTemplate const& motion, StopPlace const& place,
                                 float yards = STOP_MATCH_YARDS)
{
    KeyFrame const* best = nullptr;
    float bestSq = yards * yards;
    for (KeyFrame const& frame : motion.keyFrames)
    {
        if (!frame.Node || !frame.IsStopFrame() || frame.Node->mapid != place.mapId)
            continue;
        float const dx = frame.Node->x - place.x;
        float const dy = frame.Node->y - place.y;
        float const dz = frame.Node->z - place.z;
        float const distSq = dx * dx + dy * dy + dz * dz;
        if (distSq <= bestSq)
        {
            bestSq = distSq;
            best = &frame;
        }
    }
    return best;
}

// The vessel's period (the template's whole path time) and its ride from one graph stop to the next: the time from
// leaving the near stop frame to arriving at the far one. The transport templates are complete before
// OnBeforeWorldInitialized starts the loader (World::SetInitialWorldSettings loads them first) and never change.
// False: the core has no template for the entry, so it moves no vessel (an elevator keeps its stored seconds)
bool ReadVessel(uint32 entry, StopPlace const& from, StopPlace const& to, uint32& periodMs, float& rideSeconds)
{
    TransportTemplate const* motion = sTransportMgr->GetTransportTemplate(entry);
    if (!motion || !motion->pathTime)
        return false;
    periodMs = motion->pathTime;
    rideSeconds = 0.0f;
    KeyFrame const* leave = NearestStopFrame(*motion, from);
    KeyFrame const* arrive = NearestStopFrame(*motion, to);
    if (leave && arrive && leave != arrive)
    {
        uint32 const rideMs = (arrive->ArriveTime + motion->pathTime - leave->DepartureTime) % motion->pathTime;
        rideSeconds = static_cast<float>(rideMs) / 1000.0f;
    }
    return true;
}

// A stop record's window in its vessel's period (WT70c): the arrival and the departure of the core's stop frame at the
// record's pose. hasPath: the core moves the entry on a path (a ship or zeppelin; a lift or tram car has none). False:
// no path, or no stop frame within TravelCrossing::STOP_NODE_YARDS of the pose
bool ReadStopWindow(TravelGraphBuilder::CrossingRow& row, bool& hasPath)
{
    TransportTemplate const* motion = sTransportMgr->GetTransportTemplate(row.entry);
    hasPath = motion && motion->pathTime;
    if (!hasPath)
        return false;
    KeyFrame const* frame =
        NearestStopFrame(*motion, {row.mapId, row.x, row.y, row.z}, TravelCrossing::STOP_NODE_YARDS);
    if (!frame)
        return false;
    row.arriveMs = frame->ArriveTime;
    row.departMs = frame->DepartureTime;
    row.periodMs = motion->pathTime;
    return true;
}

// What the car windows of the load came to (the load line): stops of looping cars with a window and how many of them
// have the window's span number as their stop number (or that number plus one), stops whose pose matches no span or
// more than one, and stops of cars that wait for a state change (a pause time: no window)
struct CarWindows
{
    uint32 windows{0};
    uint32 shared{0};
    uint32 unmatched{0};
    uint32 paused{0};
};

// A lift's or the tram's car (WT70d, PLAN 3.21 (4)): a car that loops stands in the run of spans of its animation at
// the record's pose (RouteCheckStopsRules::StopWindowAt, G330), by the stop dump's rule (RouteCheckStops.cpp); of
// several such stands (one height passed going up and going down) the one holding the span numbered as the record's
// stop, as the dump numbers them; a car with a pause time waits for a state change and has no window. The spawn data,
// the object templates and the animations are complete before the loader starts and never change
bool ReadCarWindow(TravelGraphBuilder::CrossingRow& row, CarWindows& counts)
{
    GameObjectData const* const data = sObjectMgr->GetGameObjectData(row.guid);
    TransportAnimation const* const anim = data ? sTransportMgr->GetTransportAnimInfo(data->id) : nullptr;
    GameObjectTemplate const* const goInfo = data ? sObjectMgr->GetGameObjectTemplate(data->id) : nullptr;
    if (!anim || !anim->TotalTime || !goInfo)
        return false;
    if (goInfo->transport.pauseAtTime)
    {
        ++counts.paused;
        return false;
    }
    RouteCheckStopsRules::Spawn spawn{data->posX,        data->posY,       data->posZ,
                                      data->orientation, data->rotation.z, data->rotation.w};
    if (GameObjectAddon const* addon = sObjectMgr->GetGameObjectAddon(row.guid))
    {
        spawn.pathRotationZ = addon->ParentRotation.z;
        spawn.pathRotationW = addon->ParentRotation.w;
    }
    RouteCheckStopsRules::Span window{};
    std::size_t matched = 0;
    if (!RouteCheckStopsRules::StopWindowAt(anim->Path, anim->Rotations, anim->TotalTime, 0, spawn, row.x, row.y, row.z,
                                            TravelCrossing::STOP_NODE_YARDS, row.stop, window, matched))
    {
        ++counts.unmatched;
        return false;
    }
    ++counts.windows;
    counts.shared += matched > 1 ? 1 : 0;
    row.arriveMs = window.start;
    row.departMs = window.end;
    row.periodMs = anim->TotalTime;
    return true;
}
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
    uint32 notTransports = 0;
    uint32 noVessel = 0;
    uint32 vesselsMatched = 0;
    std::unordered_map<uint32, StopPlace> places;
    // The outdoor flag (navigation G91, WT72) comes with a guarded update: a database without the column loads as if
    // every node had no answer (NULL)
    bool const hasOutdoor = bool(PlayerbotsDatabase.Query("SHOW COLUMNS FROM playerbots_travelnode LIKE 'outdoor'"));
    if (QueryResult result =
            PlayerbotsDatabase.Query(hasOutdoor ? "SELECT id, name, map_id, x, y, z, outdoor FROM playerbots_travelnode"
                                                : "SELECT id, name, map_id, x, y, z, NULL FROM playerbots_travelnode"))
    {
        do
        {
            Field* fields = result->Fetch();
            int8 const outdoor = fields[6].IsNull() ? int8(-1) : (fields[6].Get<uint8>() ? int8(1) : int8(0));
            builder.AddNode(fields[0].Get<uint32>(), fields[1].Get<std::string>(), fields[2].Get<uint32>(),
                            fields[3].Get<float>(), fields[4].Get<float>(), fields[5].Get<float>(), outdoor);
            places[fields[0].Get<uint32>()] = {fields[2].Get<uint32>(), fields[3].Get<float>(), fields[4].Get<float>(),
                                               fields[5].Get<float>()};
            ++rows[0];
        } while (result->NextRow());
    }

    // The vessels' stored crossings (WT70c, WT.53), after the nodes and before the links that meet them. A row measured
    // on another model than the object template's is not loaded; a ship's or zeppelin's stop takes its window from the
    // core's stop frame at its pose, a looping car's from its animation (WT70d). The object templates and the transport
    // templates are complete before OnBeforeWorldInitialized starts this thread and never change afterwards
    uint32 crossingRows = 0;
    uint32 stopsWithoutFrame = 0;
    CarWindows carWindows;
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT entry, guid, display_id, stop, map_id, pose_x, pose_y, pose_z, pose_o, direction, seq, kind, "
            "dock_x, dock_y, dock_z, deck_x, deck_y, deck_z, gap, rise FROM playerbots_travelnode_crossing "
            "ORDER BY entry, guid, stop, direction, seq"))
    {
        TravelGraphBuilder::CrossingRow last;
        bool any = false;
        do
        {
            Field* fields = result->Fetch();
            TravelGraphBuilder::CrossingRow row;
            row.entry = fields[0].Get<uint32>();
            row.guid = fields[1].Get<uint32>();
            uint32 const displayId = fields[2].Get<uint32>();
            row.stop = fields[3].Get<uint32>();
            row.mapId = fields[4].Get<uint32>();
            row.x = fields[5].Get<float>();
            row.y = fields[6].Get<float>();
            row.z = fields[7].Get<float>();
            row.o = fields[8].Get<float>();
            row.direction = fields[9].Get<uint8>();
            row.seq = fields[10].Get<uint8>();
            row.kind = fields[11].Get<uint8>();
            row.dock = {fields[12].Get<float>(), fields[13].Get<float>(), fields[14].Get<float>()};
            row.deck = {fields[15].Get<float>(), fields[16].Get<float>(), fields[17].Get<float>()};
            row.gap = fields[18].Get<float>();
            row.rise = fields[19].Get<float>();
            ++crossingRows;
            GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(row.entry);
            if (!object || object->displayId != displayId)
            {
                builder.CountOtherDisplay();
                continue;
            }
            if (any && last.entry == row.entry && last.guid == row.guid && last.stop == row.stop)
            {
                row.hasWindow = last.hasWindow;
                row.arriveMs = last.arriveMs;
                row.departMs = last.departMs;
                row.periodMs = last.periodMs;
            }
            else
            {
                bool hasPath = false;
                row.hasWindow = row.guid ? ReadCarWindow(row, carWindows) : ReadStopWindow(row, hasPath);
                if (hasPath && !row.hasWindow)
                    ++stopsWithoutFrame;
            }
            last = row;
            any = true;
            builder.AddCrossing(row);
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
            ++rows[1];
            if (row.type == static_cast<uint8>(TravelLinkType::Transport) && row.object &&
                !IsTransportObject(row.object))
            {
                ++notTransports;
                continue;
            }
            // A ship or zeppelin (stored cost no seconds) takes its period and ride from the core; one the core
            // moves on no path cannot be priced and is dropped. Elevators keep their stored seconds
            if (row.type == static_cast<uint8>(TravelLinkType::Transport) && row.object && row.extraCost < 1.0f)
            {
                auto const from = places.find(row.fromId);
                auto const to = places.find(row.toId);
                if (from == places.end() || to == places.end() ||
                    !ReadVessel(row.object, from->second, to->second, row.periodMs, row.rideSeconds))
                {
                    ++noVessel;
                    continue;
                }
                if (row.rideSeconds > 0.0f)
                    ++vesselsMatched;
            }
            builder.AddLink(row);
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
    LOG_INFO(
        "playerbots",
        "[Route] travel graph rows read: {} nodes, {} links, {} points; largest path chunk {} rows; {} links "
        "skipped, {} points without a link; {} taxi paths, {} flight links without one; {} transport links "
        "whose object is no transport dropped; {} wait-point links, {} stops with a wait point; {} static portals; {} "
        "air links; {} vessel links priced by the core's period ({} with a ride read from its stop frames), {} "
        "dropped for having no transport path",
        rows[0], rows[1], rows[2], peakChunkRows, _stats.skippedLinks, _stats.orphanPoints, _stats.taxiPaths,
        _stats.flightsWithoutTaxi, notTransports, _stats.waitPointLinks, _stats.waitStops,
        _stats.linksByType[static_cast<std::size_t>(TravelLinkType::StaticPortal)],
        _stats.linksByType[static_cast<std::size_t>(TravelLinkType::Air)],
        _stats.linksByType[static_cast<std::size_t>(TravelLinkType::Transport)] - _stats.waitPointLinks, vesselsMatched,
        noVessel);
    // No crossing, no ride (WT.53): what the crossing table gave, and each ride it left out, for the navigation plan
    LOG_INFO("playerbots", "[Route] {} ({} table rows read)", CrossingLoadLine(_stats.crossings), crossingRows);
    if (stopsWithoutFrame)
        LOG_WARN("playerbots",
                 "[Route] crossings: {} stops of a ship or zeppelin have no stop frame at their pose (no window)",
                 stopsWithoutFrame);
    LOG_INFO("playerbots",
             "[Route] crossings: {} stops of looping cars have a window ({} at a pose of several spans, by the "
             "record's number), {} match no stop span, or several and none by the record's number, {} stops of cars "
             "with a pause time have none",
             carWindows.windows, carWindows.shared, carWindows.unmatched, carWindows.paused);
    for (TravelRideLeftOut const& ride : _ridesLeftOut)
        LOG_INFO("playerbots", "[Route] {}", RideLeftOutLine(ride));
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
        handler->PSendSysMessage("{}", TravelRoute::RouteServiceStatus());
        return true;
    }

    TravelGraphStats const& stats = graph.Stats();
    handler->PSendSysMessage(
        "ROUTE status: travel graph ready: {} nodes, {} links (walk {}, portal {}, transport {} with {} wait-point "
        "links, flight {}, static portal {}, air {}), {} points, {:.1f} MB, loaded in {} ms, largest path chunk {} "
        "rows",
        stats.nodes, stats.links, stats.linksByType[1], stats.linksByType[2], stats.linksByType[3],
        stats.waitPointLinks, stats.linksByType[4], stats.linksByType[6], stats.linksByType[7], stats.points,
        static_cast<double>(stats.memoryBytes) / (1024.0 * 1024.0), stats.loadMs, stats.peakChunkRows);
    handler->PSendSysMessage("{}", TravelRoute::RouteServiceStatus());
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
    // The graph's own checks, the route search's fixed snapshots (RouteRules.h), a four-worker route service of its
    // own under load (RouteService.h; the server's service keeps running), then the named places of `route go`
    uint32 const checks = graph.Selftest(failures, notes) + TravelRoute::Selftest(graph, failures, notes) +
                          TravelRoute::ServiceSelftest(graph, failures, notes) +
                          TravelPlaces::Selftest(graph, failures, notes);
    for (std::string const& note : notes)
        handler->PSendSysMessage("ROUTE {}", note);
    for (std::string const& failure : failures)
        handler->PSendSysMessage("ROUTE selftest FAIL: {}", failure);

    handler->PSendSysMessage("ROUTE selftest: {} checks, {} failures{}", checks, failures.size(),
                             failures.empty() ? " (PASS)" : "");
    return true;
}
