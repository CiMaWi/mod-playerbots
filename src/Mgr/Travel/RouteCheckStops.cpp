/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteCheckStops.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Chat.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RouteCheckStopsRules.h"
#include "Timer.h"
#include "Transport.h"
#include "TransportMgr.h"

namespace
{
constexpr float DIFFER_RADIANS = 0.01f;  // heading A and B count as apart beyond this

struct Heading
{
    float value;
    char const* source;  // spline, teleport, none
};

// heading B of the stop at frames[stopFrame]: the moving spline's direction at the end of the frame it is arrived
// from (Transport.cpp:354-358 at t = 1); a teleport arrives with the stop's InitialOrientation (TeleportTransport)
Heading HeadingB(KeyFrameVec const& frames, std::size_t stopFrame)
{
    KeyFrame const& frame = frames[stopFrame];
    KeyFrame const& from = frames[RouteCheckStopsRules::ArrivingFrame(frames.size(), stopFrame)];
    if (from.IsTeleportFrame())
        return {frame.InitialOrientation, "teleport"};
    int const index = static_cast<int>(from.Index);
    if (!from.Spline || index < from.Spline->first() || index >= from.Spline->last())
        return {frame.InitialOrientation, "none"};
    G3D::Vector3 dir;
    from.Spline->evaluate_derivative(index, 1.0f, dir);
    return {RouteCheckStopsRules::HeadingFromDirection(dir.x, dir.y), "spline"};
}

std::vector<std::uint32_t> MotionTransportEntries()
{
    std::vector<std::uint32_t> entries;
    for (auto const& [entry, goInfo] : *sObjectMgr->GetGameObjectTemplates())
        if (goInfo.type == GAMEOBJECT_TYPE_MO_TRANSPORT)
            entries.push_back(entry);
    std::sort(entries.begin(), entries.end());
    return entries;
}

struct DumpCounts
{
    std::uint32_t entries15 = 0;
    std::uint32_t stops15 = 0;
    std::uint32_t apart15 = 0;
    std::uint32_t noTemplate = 0;
    std::uint32_t spawns11 = 0;
    std::set<std::uint32_t> entries11;
    std::uint32_t stops11 = 0;
    std::uint32_t turning11 = 0;
    std::uint32_t noAnimation = 0;
    std::uint32_t noPose = 0;
};

void DumpMotionTransports(std::vector<std::string>& rows, DumpCounts& counts)
{
    for (std::uint32_t entry : MotionTransportEntries())
    {
        GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(entry);
        TransportTemplate const* tInfo = sTransportMgr->GetTransportTemplate(entry);
        if (!goInfo || !tInfo || tInfo->keyFrames.empty())
        {
            ++counts.noTemplate;
            continue;
        }
        ++counts.entries15;
        KeyFrameVec const& frames = tInfo->keyFrames;
        std::uint32_t stop = 0;
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            KeyFrame const& frame = frames[i];
            if (!frame.IsStopFrame())
                continue;
            Heading const b = HeadingB(frames, i);
            bool const apart = RouteCheckStopsRules::AngleDelta(frame.InitialOrientation, b.value) > DIFFER_RADIANS;
            rows.push_back(Acore::StringFormat(
                "{}\t15\t{}\t-\t{}\t{}\t{:.4f}\t{:.4f}\t{:.4f}\t{:.6f}\t{:.6f}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", entry,
                goInfo->displayId, stop, frame.Node->mapid, frame.Node->x, frame.Node->y, frame.Node->z,
                frame.InitialOrientation, b.value, b.source, frame.ArriveTime, frame.DepartureTime, tInfo->pathTime, i,
                frame.Node->index, apart ? 1 : 0, goInfo->name));
            ++stop;
            ++counts.stops15;
            if (apart)
                ++counts.apart15;
        }
    }
}

void DumpStaticTransports(std::vector<std::string>& rows, DumpCounts& counts)
{
    GameObjectDataContainer const& store = sObjectMgr->GetAllGOData();
    std::vector<ObjectGuid::LowType> spawnIds;
    for (auto const& [spawnId, data] : store)
    {
        GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(data.id);
        if (goInfo && goInfo->type == GAMEOBJECT_TYPE_TRANSPORT)
            spawnIds.push_back(spawnId);
    }
    std::sort(spawnIds.begin(), spawnIds.end());

    for (ObjectGuid::LowType spawnId : spawnIds)
    {
        GameObjectData const& data = store.at(spawnId);
        GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(data.id);
        TransportAnimation const* anim = sTransportMgr->GetTransportAnimInfo(data.id);
        if (!anim || !anim->TotalTime)
        {
            ++counts.noAnimation;
            continue;
        }
        ++counts.spawns11;
        counts.entries11.insert(data.id);

        // StaticTransport::Create: the path rotation from gameobject_addon, else the spawn's rotation
        RouteCheckStopsRules::Spawn spawn{data.posX,        data.posY,       data.posZ,
                                          data.orientation, data.rotation.z, data.rotation.w};
        if (GameObjectAddon const* addon = sObjectMgr->GetGameObjectAddon(spawnId))
        {
            spawn.pathRotationZ = addon->ParentRotation.z;
            spawn.pathRotationW = addon->ParentRotation.w;
        }
        std::uint32_t const period = anim->TotalTime;
        std::uint32_t const pause = goInfo->transport.pauseAtTime;
        std::vector<RouteCheckStopsRules::Span> const spans =
            RouteCheckStopsRules::StopSpans(anim->Path, period, pause);
        for (std::size_t stop = 0; stop < spans.size(); ++stop)
        {
            RouteCheckStopsRules::Span const& span = spans[stop];
            RouteCheckStopsRules::Pose start{}, end{};
            if (!RouteCheckStopsRules::PoseAtProgress(anim->Path, anim->Rotations, period, spawn, span.start % period,
                                                      start) ||
                !RouteCheckStopsRules::PoseAtProgress(anim->Path, anim->Rotations, period, spawn,
                                                      RouteCheckStopsRules::SpanEndProgress(span, period), end))
            {
                ++counts.noPose;
                continue;
            }
            bool const apart = RouteCheckStopsRules::AngleDelta(start.o, end.o) > DIFFER_RADIANS;
            rows.push_back(Acore::StringFormat(
                "{}\t11\t{}\t{}\t{}\t{}\t{:.4f}\t{:.4f}\t{:.4f}\t{:.6f}\t{:.6f}\t{}\t{}\t{}\t{}\t-\t-\t{}\t{}", data.id,
                goInfo->displayId, spawnId, stop, data.mapid, start.x, start.y, start.z, start.o, end.o,
                pause ? "pause" : (span.wrap ? "span-end-wrap" : "span-end"), span.start, span.end, period,
                apart ? 1 : 0, goInfo->name));
            ++counts.stops11;
            if (apart)
                ++counts.turning11;
        }
    }
}

std::vector<std::string> RunDump()
{
    std::vector<std::string> rows;
    DumpCounts counts;
    DumpMotionTransports(rows, counts);
    DumpStaticTransports(rows, counts);

    std::string const stamp = Acore::Time::TimeToTimestampStr(GameTime::GetGameTime(), "%Y%m%d-%H%M%S");
    std::filesystem::path const file =
        std::filesystem::path(sLog->GetLogsDir()) / ("route-check-stops-" + stamp + ".tsv");
    std::ofstream out(file, std::ios::binary);
    if (out)
    {
        out << "# route check stops; type 15: a stop frame, heading A = InitialOrientation, B = the moving spline's "
            << "direction at the end of the segment it is arrived by (b_source teleport or none: A), arrive/depart = "
            << "the timer's ms; type 11: a span of progress (ms; span-end-wrap: through 0), the pose at its start, "
            << "A/B = the heading at its start/end; apart = A and B differ by more than " << DIFFER_RADIANS << " rad\n";
        out << "entry\ttype\tdisplay\tguid\tstop\tmap\tx\ty\tz\theading_a\theading_b\tb_source\tarrive_ms\tdepart_ms"
            << "\tperiod_ms\tframe\tnode\tapart\tname\n";
        for (std::string const& row : rows)
            out << row << "\n";
    }

    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat(
        "ROUTE check stops: type 15 {} entries, {} stops ({} with A and B apart), {} without a path; type 11 {} "
        "spawns of {} entries, {} stops ({} turning while they stand), {} without animation, {} without pose; {}",
        counts.entries15, counts.stops15, counts.apart15, counts.noTemplate, counts.spawns11, counts.entries11.size(),
        counts.stops11, counts.turning11, counts.noAnimation, counts.noPose,
        out ? file.string() : "the file could not be written"));
    return lines;
}

std::vector<std::string> RunLive()
{
    std::set<std::uint32_t> mapIds;
    for (std::uint32_t entry : MotionTransportEntries())
        if (TransportTemplate const* tInfo = sTransportMgr->GetTransportTemplate(entry))
            if (!tInfo->inInstance)
                mapIds.insert(tInfo->mapsUsed.begin(), tInfo->mapsUsed.end());

    std::vector<std::string> lines;
    lines.push_back(
        "STOPLIVE\tentry\tdisplay\tguid\tmap\tx\ty\tz\to\ttimer\tperiod\tstop\tframe\tnode\tdpos"
        "\tda\tdb\tb_source");
    std::uint32_t transports = 0;
    std::uint32_t standing = 0;
    for (std::uint32_t mapId : mapIds)
    {
        Map* map = sMapMgr->FindBaseMap(mapId);
        if (!map)
            continue;
        std::vector<MotionTransport const*> vessels;
        for (Transport const* transport : map->GetAllTransports())
            if (MotionTransport const* vessel = transport->ToMotionTransport())
                vessels.push_back(vessel);
        std::sort(
            vessels.begin(), vessels.end(), [](MotionTransport const* a, MotionTransport const* b)
            { return a->GetEntry() != b->GetEntry() ? a->GetEntry() < b->GetEntry() : a->GetGUID() < b->GetGUID(); });

        for (MotionTransport const* vessel : vessels)
        {
            KeyFrameVec const& frames = vessel->GetKeyFrames();
            std::uint32_t const period = vessel->GetPeriod();
            if (frames.empty() || !period)
                continue;
            ++transports;
            std::uint32_t const timer = vessel->GetPathProgress() % period;
            std::size_t frameIndex = 0;
            int const stop = RouteCheckStopsRules::StopAt(frames, timer, frameIndex);
            std::string where = "moving\t-\t-\t-\t-\t-\t-";
            if (stop >= 0)
            {
                ++standing;
                KeyFrame const& frame = frames[frameIndex];
                Heading const b = HeadingB(frames, frameIndex);
                float const o = vessel->GetOrientation();
                where =
                    Acore::StringFormat("{}\t{}\t{}\t{:.4f}\t{:.6f}\t{:.6f}\t{}", stop, frameIndex, frame.Node->index,
                                        vessel->GetExactDist(frame.Node->x, frame.Node->y, frame.Node->z),
                                        RouteCheckStopsRules::AngleDelta(o, frame.InitialOrientation),
                                        RouteCheckStopsRules::AngleDelta(o, b.value), b.source);
            }
            lines.push_back(Acore::StringFormat(
                "STOPLIVE\t{}\t{}\t{}\t{}\t{:.4f}\t{:.4f}\t{:.4f}\t{:.6f}\t{}\t{}\t{}", vessel->GetEntry(),
                vessel->GetDisplayId(), vessel->GetGUID().GetCounter(), vessel->GetMapId(), vessel->GetPositionX(),
                vessel->GetPositionY(), vessel->GetPositionZ(), vessel->GetOrientation(), timer, period, where));
        }
    }
    lines.push_back(Acore::StringFormat("ROUTE check stops live: {} transports on {} maps, {} standing at a stop",
                                        transports, mapIds.size(), standing));
    return lines;
}

class StopsOperation final : public PlayerbotOperation
{
public:
    explicit StopsOperation(bool live) : _live(live) {}

    bool Execute() override
    {
        for (std::string const& line : _live ? RunLive() : RunDump())
            LOG_INFO("playerbots", "{}", line);
        return true;
    }

    std::string GetName() const override { return "route check stops"; }

private:
    bool _live;
};
}  // namespace

bool RouteCheckStops::HandleStopsCommand(ChatHandler* handler, char const* args)
{
    std::string const word = args ? args : "";
    std::string trimmed;
    for (char c : word)
        if (c != ' ')
            trimmed += c;
    if (!trimmed.empty() && trimmed != "live")
    {
        handler->SendErrorMessage("Usage: .playerbots route check stops [live]");
        return false;
    }
    bool const live = trimmed == "live";

    // a session's command runs on its map's thread: the read goes to the world thread and writes to the log
    if (handler->GetSession())
    {
        if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<StopsOperation>(live)))
        {
            handler->SendErrorMessage("ROUTE check stops: the world thread's queue is full");
            return false;
        }
        handler->PSendSysMessage("ROUTE check stops: queued; the result goes to the server log");
        return true;
    }

    for (std::string const& line : live ? RunLive() : RunDump())
        handler->PSendSysMessage("{}", line);
    return true;
}
