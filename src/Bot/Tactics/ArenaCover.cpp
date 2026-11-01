/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ArenaCover.h"

#include <array>
#include <map>
#include <string>

#include "Battleground.h"
#include "BattlegroundRV.h"
#include "Chat.h"
#include "GameObject.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "StringFormat.h"
#include "Timer.h"

namespace
{
constexpr std::array<uint32, 4> RV_PILLAR_OBJECTS = {BG_RV_OBJECT_PILAR_1, BG_RV_OBJECT_PILAR_2, BG_RV_OBJECT_PILAR_3,
                                                     BG_RV_OBJECT_PILAR_4};

std::string RvPillarState(Battleground* bg, uint8 gate)
{
    if (gate == 0)
        return "static";

    if (!bg || bg->GetMapId() != COVER_MAP_RING_OF_VALOR || gate > RV_PILLAR_OBJECTS.size())
        return "no object";

    GameObject* pillar = bg->GetBGObject(RV_PILLAR_OBJECTS[gate - 1]);
    if (!pillar)
        return "no object";

    return Acore::StringFormat("go state {}, transport {}", static_cast<uint32>(pillar->GetGoState()),
                               pillar->IsTransport() ? "yes" : "no");
}
}  // namespace

uint32 ArenaCoverActiveGates(Battleground* bg)
{
    uint32 gates = COVER_GATE_ALWAYS;
    if (!bg || bg->GetMapId() != COVER_MAP_RING_OF_VALOR)
        return gates;

    // 13e-polish (G2d-b's scan): a pillar at GO_STATE_ACTIVE is raised and blocks the rays, at GO_STATE_READY it is
    // lowered and blocks none. Its LOS follows the transport's animation, so a swap is unreliable for up to about 5 s.
    for (uint32 gate = 1; gate <= RV_PILLAR_OBJECTS.size(); ++gate)
        if (GameObject* pillar = bg->GetBGObject(RV_PILLAR_OBJECTS[gate - 1]))
            if (pillar->GetGoState() == GO_STATE_ACTIVE)
                gates |= 1u << gate;

    return gates;
}

bool ArenaCoverLos(Map* map, uint32 phaseMask, CoverVec const& a, CoverVec const& b)
{
    // as the spell check (Spell::CheckCast -> WorldObject::IsWithinLOSInMap(target, M2, LINEOFSIGHT_ALL_CHECKS))
    return map->isInLineOfSight(a.X, a.Y, a.Z, b.X, b.Y, b.Z, phaseMask, LINEOFSIGHT_ALL_CHECKS,
                                VMAP::ModelIgnoreFlags::M2);
}

// tactics cover scan [name]: the arena the player stands in, scanned for pillars (CoverScan), and the table's rows
// checked by live rays. Prints the discs as table rows (to paste into ArenaCover.h, G2d-b), the drops by reason, the
// table check and one JSON line (also in the Playerbots log) for the harness.
bool RunArenaCoverScan(ChatHandler* handler, Player* player)
{
    Map* map = player->GetMap();
    if (!map || !map->IsBattleArena())
    {
        handler->PSendSysMessage("TACTICS cover scan: {} is not in an arena", player->GetName());
        return false;
    }

    ArenaCoverTable const* table = FindArenaCoverTable(map->GetId());
    if (!table)
    {
        handler->PSendSysMessage("TACTICS cover scan: no cover table for map {}", map->GetId());
        return false;
    }

    uint32 const phaseMask = player->GetPhaseMask();
    CoverScanParams params;
    params.CentreX = table->CentreX;
    params.CentreY = table->CentreY;
    params.RefZ = table->CentreZ;
    params.HalfSize = table->HalfSize;
    auto const ground = [map, phaseMask, &params](float x, float y, float& z)
    {
        z = map->GetHeight(phaseMask, x, y, params.RefZ + params.ZWindow, true, 2.0f * params.ZWindow);
        return z > INVALID_HEIGHT + 1.0f;
    };
    auto const los = [map, phaseMask](CoverVec const& a, CoverVec const& b)
    { return ArenaCoverLos(map, phaseMask, a, b); };

    uint32 const startMs = getMSTime();
    CoverScanResult const scan = CoverScan(params, ground, los);
    uint32 const scanMs = GetMSTimeDiffToNow(startMs);
    uint32 passed = 0;
    for (CoverScanDisc const& disc : scan.Discs)
        passed += disc.Pass ? 1 : 0;

    handler->PSendSysMessage(
        "TACTICS cover scan {} (map {}, instance {}): {} discs, {} pass the ray check; floor {} "
        "of {} grid points; rays: {} pairs ({} blocked), {} radius probes, {} confirming; {} ms",
        table->Name, map->GetId(), map->GetInstanceId(), scan.Discs.size(), passed, scan.FloorPoints, scan.GridPoints,
        scan.PairRays, scan.BlockedPairs, scan.RefineRays, scan.ConfirmRays, scanMs);
    std::string json = Acore::StringFormat(
        "{{\"map\":{},\"name\":\"{}\",\"instance\":{},\"ms\":{},\"floor\":{},"
        "\"grid\":{},\"pairRays\":{},\"blockedPairs\":{},\"discs\":[",
        map->GetId(), table->Name, map->GetInstanceId(), scanMs, scan.FloorPoints, scan.GridPoints, scan.PairRays,
        scan.BlockedPairs);
    for (size_t index = 0; index < scan.Discs.size(); ++index)
    {
        CoverScanDisc const& disc = scan.Discs[index];
        bool const elongated = disc.RMin < 0.5f * disc.Pillar.R;
        handler->PSendSysMessage(
            "TACTICS cover scan disc {}: {{{:.2f}f, {:.2f}f, {:.2f}f, {:.2f}f, 0}}, rmin {:.2f}, "
            "pairs {}, directions {}/{}, diameters blocked {}/{}, ring floor {}/8: {}{}",
            index, disc.Pillar.X, disc.Pillar.Y, disc.Pillar.Z, disc.Pillar.R, disc.RMin, disc.Blocked,
            disc.ValidDirections, params.Directions, disc.DiametersBlocked, params.ConfirmRays, disc.RingStandable,
            disc.Pass ? "PASS" : "FAIL", elongated ? " (elongated)" : "");
        json += Acore::StringFormat("{}[{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{},{},{}]", index ? "," : "", disc.Pillar.X,
                                    disc.Pillar.Y, disc.Pillar.Z, disc.Pillar.R, disc.RMin, disc.DiametersBlocked,
                                    disc.RingStandable, disc.Pass ? 1 : 0);
    }

    std::map<std::string, uint32> drops;
    for (CoverScanRejected const& rejected : scan.Rejected)
        ++drops[rejected.Reason];

    std::string dropText;
    for (auto const& [reason, count] : drops)
        dropText += Acore::StringFormat("{}{} {}", dropText.empty() ? "" : ", ", count, reason);

    handler->PSendSysMessage("TACTICS cover scan dropped clusters: {}", dropText.empty() ? "none" : dropText);
    uint32 shown = 0;
    for (CoverScanRejected const& rejected : scan.Rejected)
    {
        if (std::string(rejected.Reason) == "noise" || ++shown > 12)
            continue;

        handler->PSendSysMessage("TACTICS cover scan dropped: ({:.1f}, {:.1f}) extent {:.1f}, pairs {}: {}", rejected.X,
                                 rejected.Y, rejected.Extent, rejected.Blocked, rejected.Reason);
    }

    // the table's rows by live rays (Ring of Valor: each pillar's object state too; a pillar that is up should block,
    // one that is down should not)
    BattlegroundMap* bgMap = map->ToBattlegroundMap();
    Battleground* bg = bgMap ? bgMap->GetBG() : nullptr;
    uint32 const gates = ArenaCoverActiveGates(bg);
    json += "],\"table\":[";
    uint32 tablePass = 0;
    for (uint32 index = 0; index < table->Count; ++index)
    {
        CoverPillar const& pillar = table->Pillars[index];
        bool const up = CoverPillarActive(pillar, gates);
        uint32 const blocked = CoverCheckPillar(pillar, los, params.ConfirmGap, params.Eye, params.ConfirmRays);
        bool const pass = up ? blocked >= params.ConfirmMinBlocked : blocked == 0;
        tablePass += pass ? 1 : 0;
        handler->PSendSysMessage(
            "TACTICS cover table pillar {}: ({:.2f}, {:.2f}) r {:.2f}, gate {} {} ({}), diameters "
            "blocked {}/{}: {}",
            index, pillar.X, pillar.Y, pillar.R, pillar.Gate, up ? "up" : "down", RvPillarState(bg, pillar.Gate),
            blocked, params.ConfirmRays, pass ? "PASS" : "FAIL");
        json +=
            Acore::StringFormat("{}[{},{},{},{}]", index ? "," : "", pillar.Gate, up ? 1 : 0, blocked, pass ? 1 : 0);
    }

    handler->PSendSysMessage("TACTICS cover table {}: {} rows, {} pass", table->Name, table->Count, tablePass);
    json += Acore::StringFormat("],\"discsPass\":{},\"tablePass\":{}}}", passed, tablePass);
    handler->PSendSysMessage("TACTICS cover scan json {}", json);
    LOG_INFO("playerbots", "TACTICS cover scan json {}", json);
    return true;
}
