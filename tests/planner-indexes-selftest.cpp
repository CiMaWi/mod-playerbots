/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the planner indexes and the crowding book (travel plan WT12b): zones, services and their nearest
// lookups against a brute-force scan, hubs, determinism, publication under threads, the memory estimate.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg
// tests/planner-indexes-selftest.cpp

#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <thread>

#include "PlannerIndexes.h"

using namespace Planner;

namespace
{
constexpr std::uint32_t MAP_A = 0;
constexpr std::uint32_t MAP_B = 1;
constexpr std::uint32_t MAP_EMPTY = 530;
constexpr std::uint32_t ZONE_TOWN = 12;
constexpr std::uint32_t ZONE_FIELDS = 40;
constexpr std::uint32_t AREA_TOWN = 87;
constexpr std::uint32_t AREA_HAMLET = 108;
constexpr std::uint32_t AREA_LONE = 109;
constexpr std::uint32_t FACTION_FRIEND = 12;
constexpr std::uint32_t FACTION_HOSTILE = 85;
constexpr std::uint8_t BAG_SLOTS = 10;
constexpr int QUERIES = 500;
constexpr int PUBLISHES = 2000;
constexpr int READERS = 4;
constexpr float NEAR_YARDS = 600.0f;
constexpr long long GUARD_MS = 100;

int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

SpawnRecord Spawn(std::uint32_t map, float x, float y, std::uint32_t entry, std::uint32_t flags, std::uint32_t zone,
                  std::uint32_t area, std::uint32_t faction = FACTION_FRIEND, std::uint8_t bag = 0,
                  bool mailbox = false)
{
    return {map, x, y, 10.0f, entry, flags, faction, zone, area, bag, mailbox};
}

// Map A: a town (vendor, repairer, innkeeper, flight master, mailbox, a bag vendor, a hostile vendor, quest givers),
// a hamlet of quest givers 1500 yd away, a lone vendor 4000 yd away. Map B: a second town
GatherInput Synthetic()
{
    GatherInput in;
    std::uint32_t entry = 100;
    auto add = [&in, &entry](std::uint32_t map, float x, float y, std::uint32_t flags, std::uint32_t zone,
                             std::uint32_t area, std::uint32_t faction = FACTION_FRIEND, std::uint8_t bag = 0,
                             bool mailbox = false)
    { in.spawns.push_back(Spawn(map, x, y, ++entry, flags, zone, area, faction, bag, mailbox)); };
    add(MAP_A, 100.0f, 100.0f, NpcFlag::VENDOR | NpcFlag::REPAIR, ZONE_TOWN, AREA_TOWN);
    add(MAP_A, 120.0f, 90.0f, NpcFlag::INNKEEPER | NpcFlag::VENDOR, ZONE_TOWN, AREA_TOWN);
    add(MAP_A, 150.0f, 130.0f, NpcFlag::FLIGHTMASTER, ZONE_TOWN, AREA_TOWN);
    add(MAP_A, 110.0f, 140.0f, 0, ZONE_TOWN, AREA_TOWN, FACTION_FRIEND, 0, true);
    add(MAP_A, 90.0f, 160.0f, NpcFlag::VENDOR, ZONE_TOWN, AREA_TOWN, FACTION_FRIEND, BAG_SLOTS);
    add(MAP_A, 170.0f, 80.0f, NpcFlag::VENDOR, ZONE_TOWN, AREA_TOWN, FACTION_HOSTILE);
    add(MAP_A, 130.0f, 110.0f, NpcFlag::TRAINER | NpcFlag::TRAINER_CLASS, ZONE_TOWN, AREA_TOWN);
    add(MAP_A, 135.0f, 115.0f, NpcFlag::BANKER, ZONE_TOWN, AREA_TOWN);
    add(MAP_A, 140.0f, 120.0f, NpcFlag::AUCTIONEER, ZONE_TOWN, AREA_TOWN);
    for (int i = 0; i < 8; ++i)
        add(MAP_A, 60.0f + 15.0f * static_cast<float>(i), 60.0f + 10.0f * static_cast<float>(i), NpcFlag::QUESTGIVER,
            ZONE_TOWN, AREA_TOWN);
    for (int i = 0; i < 6; ++i)
        add(MAP_A, 1600.0f + 20.0f * static_cast<float>(i), 100.0f, NpcFlag::QUESTGIVER, ZONE_FIELDS, AREA_HAMLET);
    add(MAP_A, 4100.0f, 100.0f, NpcFlag::VENDOR, ZONE_FIELDS, AREA_LONE);
    for (int i = 0; i < 12; ++i)
        add(MAP_B, -2000.0f + 12.0f * static_cast<float>(i), 500.0f + 9.0f * static_cast<float>(i % 4),
            i % 3 ? NpcFlag::QUESTGIVER : NpcFlag::VENDOR | NpcFlag::REPAIR, ZONE_FIELDS + 1, AREA_LONE + 1);
    in.zones.push_back({ZONE_TOWN, 1, 10, ZoneSide::Alliance, MAP_A});
    in.zones.push_back({ZONE_FIELDS, 10, 20, ZoneSide::Contested, MAP_A});
    in.zones.push_back({ZONE_FIELDS + 1, 0, 0, ZoneSide::Horde, MAP_B});
    in.unresolved = 3;
    return in;
}

bool SameIndexes(PlannerIndexes const& a, PlannerIndexes const& b)
{
    if (a.services.size() != b.services.size() || a.hubs.size() != b.hubs.size() || a.zones.size() != b.zones.size())
        return false;
    for (std::size_t i = 0; i < a.services.size(); ++i)
        if (a.services[i].entry != b.services[i].entry || a.services[i].hub != b.services[i].hub)
            return false;
    for (std::size_t i = 0; i < a.hubs.size(); ++i)
        if (a.hubs[i].x != b.hubs[i].x || a.hubs[i].y != b.hubs[i].y || a.hubs[i].members != b.hubs[i].members ||
            a.hubs[i].counts != b.hubs[i].counts)
            return false;
    return true;
}

void Zones(PlannerIndexes const& idx)
{
    ZoneRecord const* town = idx.FindZone(ZONE_TOWN);
    Check(town && town->low == 1 && town->high == 10 && town->side == ZoneSide::Alliance && town->mapId == MAP_A,
          "zone bracket, side and continent copied");
    ZoneRecord const* other = idx.FindZone(ZONE_FIELDS + 1);
    Check(other && other->side == ZoneSide::Horde && other->mapId == MAP_B, "second map's zone");
    Check(!idx.FindZone(9999), "an unknown zone is absent");
}

void Services(PlannerIndexes const& idx)
{
    IndexStats const& s = idx.stats;
    auto count = [&s](ServiceKind kind) { return s.perKind[static_cast<std::size_t>(kind)]; };
    Check(count(ServiceKind::Vendor) == 9, "vendors: 9 (town 5, lone 1, map B 4)");
    Check(count(ServiceKind::BagVendor) == 1, "one bag vendor");
    Check(count(ServiceKind::Repairer) == 5, "repairers: town 1, map B 4");
    Check(count(ServiceKind::Innkeeper) == 1 && count(ServiceKind::FlightMaster) == 1 &&
              count(ServiceKind::Mailbox) == 1 && count(ServiceKind::Trainer) == 1 && count(ServiceKind::Banker) == 1 &&
              count(ServiceKind::Auctioneer) == 1,
          "innkeeper, flight master, mailbox, trainer, banker, auctioneer: one each");
    Check(count(ServiceKind::QuestGiver) == 22, "quest givers: 8 + 6 + 8");
    Check(s.unresolved == 3 && s.spawns == 36, "spawn and unresolved counts carried");

    std::vector<Nearby> const bag = idx.Nearest(ServiceKind::BagVendor, MAP_A, 0.0f, 0.0f, 10.0f, 3);
    Check(bag.size() == 1 && idx.services[bag[0].index].bagSlots == BAG_SLOTS, "a bag vendor reports its slots");
    std::vector<Nearby> const hostile = idx.Nearest(ServiceKind::Vendor, MAP_A, 170.0f, 80.0f, 10.0f, 1);
    Check(hostile.size() == 1 && idx.services[hostile[0].index].factionTemplate == FACTION_HOSTILE,
          "the hostile vendor is kept with its faction");
    Check(idx.Nearest(ServiceKind::Vendor, MAP_EMPTY, 0.0f, 0.0f, 0.0f, 3).empty(), "a map without any: empty");
    Check(idx.Nearest(ServiceKind::Innkeeper, MAP_A, 0.0f, 0.0f, 0.0f, 50).size() == 1, "k above the count is safe");
    Check(idx.Nearest(ServiceKind::Vendor, MAP_A, 4100.0f, 900.0f, 10.0f, 5, 500.0f).empty(),
          "maxYards excludes far vendors");

    // Nearest against a brute-force scan
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> coord(-3000.0f, 5000.0f);
    std::uniform_int_distribution<int> pick(0, static_cast<int>(SERVICE_KINDS) - 1);
    int mismatches = 0;
    for (int q = 0; q < QUERIES; ++q)
    {
        std::uint32_t const map = q % 2 ? MAP_B : MAP_A;
        ServiceKind const kind = static_cast<ServiceKind>(pick(rng));
        float const x = coord(rng);
        float const y = coord(rng);
        std::size_t const k = 1 + static_cast<std::size_t>(q % 4);
        std::vector<float> brute;
        for (Service const& svc : idx.services)
        {
            if (svc.mapId != map)
                continue;
            std::vector<ServiceKind> const kinds = Detail::KindsOf(svc.npcFlags, svc.bagSlots, svc.mailbox);
            if (std::find(kinds.begin(), kinds.end(), kind) != kinds.end())
                brute.push_back(Detail::Distance(x, y, 10.0f, svc.x, svc.y, svc.z));
        }
        std::sort(brute.begin(), brute.end());
        std::vector<Nearby> const got = idx.Nearest(kind, map, x, y, 10.0f, k);
        bool ok = got.size() == std::min(k, brute.size());
        for (std::size_t i = 0; ok && i < got.size(); ++i)
            ok = std::fabs(got[i].distance - brute[i]) < 0.01f;
        if (!ok)
            ++mismatches;
    }
    Check(mismatches == 0,
          "Nearest equals a brute-force scan on 500 queries (" + std::to_string(mismatches) + " mismatches)");
}

void Hubs(PlannerIndexes const& idx, GatherInput const& input)
{
    Check(idx.hubs.size() == 3,
          "three hubs: the town, the hamlet, map B's town (" + std::to_string(idx.hubs.size()) + ")");
    std::optional<Nearby> const town = idx.NearestHub(MAP_A, 120.0f, 110.0f, 10.0f);
    Check(town.has_value(), "a hub near the town");
    if (town)
    {
        Hub const& hub = idx.hubs[town->index];
        Check(hub.x > 60.0f && hub.x < 175.0f && hub.y > 60.0f && hub.y < 165.0f, "the town's centroid is inside it");
        Check(hub.zoneId == ZONE_TOWN && hub.areaId == AREA_TOWN, "the town's zone and area");
        Check(hub.Count(ServiceKind::Vendor) == 4 && hub.Count(ServiceKind::BagVendor) == 1 &&
                  hub.Count(ServiceKind::Repairer) == 1 && hub.Count(ServiceKind::Innkeeper) == 1 &&
                  hub.Count(ServiceKind::FlightMaster) == 1 && hub.Count(ServiceKind::Mailbox) == 1 &&
                  hub.Count(ServiceKind::QuestGiver) == 8,
              "the town's counts per kind");
        Check(idx.ServicesInHub(town->index).size() == 17, "the town has 17 services");
    }
    std::optional<Nearby> const hamlet = idx.NearestHub(MAP_A, 1650.0f, 100.0f, 10.0f, 200.0f);
    Check(hamlet && idx.hubs[hamlet->index].Count(ServiceKind::QuestGiver) == 6 &&
              !idx.hubs[hamlet->index].Has(ServiceKind::Vendor),
          "the hamlet is a hub of quest givers only");
    Check(!idx.NearestHub(MAP_A, 4100.0f, 100.0f, 10.0f, 500.0f), "the lone vendor is no hub (maxYards honoured)");
    std::vector<Nearby> const lone = idx.Nearest(ServiceKind::Vendor, MAP_A, 4100.0f, 100.0f, 10.0f, 1);
    Check(lone.size() == 1 && idx.services[lone[0].index].hub == NO_HUB, "the lone vendor belongs to no hub");
    Check(idx.HubsInZone(ZONE_TOWN).size() == 1 && idx.HubsInZone(ZONE_FIELDS).size() == 1 &&
              idx.HubsInZone(9999).empty(),
          "hubs by zone");
    Check(idx.NearestHubs(MAP_A, 0.0f, 0.0f, 0.0f, 10).size() == 2, "all hubs of a map, k above the count");

    PlannerIndexes const again = PlannerIndexes::Build(input);
    Check(SameIndexes(idx, again), "two builds are equal");
    GatherInput shuffled = input;
    std::mt19937 rng(777);
    std::shuffle(shuffled.spawns.begin(), shuffled.spawns.end(), rng);
    std::reverse(shuffled.zones.begin(), shuffled.zones.end());
    Check(SameIndexes(idx, PlannerIndexes::Build(shuffled)), "the build does not depend on the input order");
}

void Crowding()
{
    CrowdingBook book;
    Check(!book.Current(), "an empty book reads null");
    auto first = std::make_shared<CrowdingSnapshot>();
    first->Add(MAP_A, ZONE_TOWN, AREA_TOWN, true);
    first->Add(MAP_A, ZONE_TOWN, AREA_TOWN, false);
    first->sequence = 1;
    book.Publish(first);
    std::shared_ptr<CrowdingSnapshot const> held = book.Current();
    auto second = std::make_shared<CrowdingSnapshot>();
    second->sequence = 2;
    book.Publish(second);
    Check(held && held->sequence == 1 && held->total.bots == 1 && held->total.players == 1 &&
              held->byArea.at(AreaKey(MAP_A, AREA_TOWN)).bots == 1,
          "a reader's old snapshot is unaffected by a later publish");
    Check(book.Current()->sequence == 2, "the new snapshot is current");

    // One publisher, four readers: every read is whole (the total equals the sum of its areas), never null
    CrowdingBook shared;
    shared.Publish(std::make_shared<CrowdingSnapshot>());
    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::atomic<long> reads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < READERS; ++r)
        readers.emplace_back(
            [&]
            {
                std::uint64_t last = 0;
                while (!done.load())
                {
                    std::shared_ptr<CrowdingSnapshot const> snap = shared.Current();
                    if (!snap)
                    {
                        ++torn;
                        continue;
                    }
                    std::uint32_t bots = 0;
                    std::uint32_t players = 0;
                    for (auto const& [key, counts] : snap->byArea)
                    {
                        bots += counts.bots;
                        players += counts.players;
                    }
                    if (bots != snap->total.bots || players != snap->total.players || snap->sequence < last)
                        ++torn;
                    last = snap->sequence;
                    ++reads;
                }
            });
    for (int i = 1; i <= PUBLISHES; ++i)
    {
        auto snap = std::make_shared<CrowdingSnapshot>();
        for (int b = 0; b < i % 50; ++b)
            snap->Add(b % 3, ZONE_TOWN + static_cast<std::uint32_t>(b % 5),
                      AREA_TOWN + static_cast<std::uint32_t>(b % 7), b % 4 != 0);
        snap->sequence = static_cast<std::uint64_t>(i);
        shared.Publish(std::move(snap));
    }
    done = true;
    for (std::thread& t : readers)
        t.join();
    Check(torn == 0 && reads > 0,
          "4 readers x 2000 publishes: no torn or null read (" + std::to_string(reads.load()) + " reads)");
}

void Publication(GatherInput const& input)
{
    IndexBook book;
    book.Publish(std::make_shared<PlannerIndexes const>(PlannerIndexes::Build(input)));
    std::shared_ptr<PlannerIndexes const> held = book.Current();
    std::size_t const services = held->services.size();
    book.Publish(std::make_shared<PlannerIndexes const>(PlannerIndexes::Build(GatherInput{})));
    Check(held->services.size() == services && !held->Nearest(ServiceKind::Vendor, MAP_A, 0, 0, 0, 1).empty(),
          "a reader keeps the old indexes alive after a new publish");
    Check(book.Current()->services.empty(), "the new (empty) indexes are current");
}

void Memory(PlannerIndexes const& idx)
{
    Check(idx.stats.estimatedBytes > 0, "the memory estimate is not zero");
    std::uint64_t previous = 0;
    bool monotone = true;
    for (int n = 1; n <= 4; ++n)
    {
        GatherInput in;
        for (int i = 0; i < n * 200; ++i)
            in.spawns.push_back(Spawn(MAP_A, static_cast<float>(i % 40) * 30.0f, static_cast<float>(i / 40) * 30.0f,
                                      static_cast<std::uint32_t>(i), NpcFlag::VENDOR, ZONE_TOWN, AREA_TOWN));
        std::uint64_t const bytes = PlannerIndexes::Build(in).stats.estimatedBytes;
        monotone = monotone && bytes > previous;
        previous = bytes;
    }
    Check(monotone, "the memory estimate grows with the spawns");
}

// Review note WT12b a: NaN, inf and huge points return at once (empty or from the nearest edge cell) and a point
// outside the occupied cells still gets the exact nearest services
void Guards(PlannerIndexes const& idx)
{
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();
    float const huge = 1e30f;
    Check(Detail::Cell(huge, SERVICE_CELL_YARDS) == CELL_OFFSET - 1 &&
              Detail::Cell(-huge, SERVICE_CELL_YARDS) == -CELL_OFFSET &&
              Detail::Cell(nan, SERVICE_CELL_YARDS) == -CELL_OFFSET && Detail::Cell(-1.0f, SERVICE_CELL_YARDS) == -1,
          "cells clamp into the key range");
    auto const start = std::chrono::steady_clock::now();
    Check(idx.Nearest(ServiceKind::Vendor, MAP_A, nan, 0.0f, 0.0f, 3).empty() &&
              idx.Nearest(ServiceKind::Vendor, MAP_A, 0.0f, inf, 0.0f, 3).empty() &&
              idx.Nearest(ServiceKind::Vendor, MAP_A, 0.0f, 0.0f, -inf, 3).empty(),
          "Nearest: NaN and inf give none");
    Check(idx.NearestHubs(MAP_A, nan, 0.0f, 0.0f, 3).empty() && !idx.NearestHub(MAP_A, 0.0f, inf, 0.0f),
          "NearestHubs: NaN and inf give none");
    Check(idx.Nearest(ServiceKind::Vendor, MAP_A, huge, 0.0f, 0.0f, 3).size() == 3 &&
              idx.Nearest(ServiceKind::Vendor, MAP_A, -huge, -huge, 0.0f, 3).size() == 3 &&
              idx.Nearest(ServiceKind::Vendor, MAP_A, huge, 0.0f, 0.0f, 3, 500.0f).empty(),
          "Nearest: 1e30 answers from the edge, none within 500 yd");
    long long const ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    Check(ms < GUARD_MS, "the guard queries return at once (" + std::to_string(ms) + " ms)");

    int mismatches = 0;
    for (float const x : {50000.0f, -50000.0f, 2000.0f})
        for (float const y : {50000.0f, -50000.0f, 300.0f})
        {
            std::vector<std::pair<float, std::uint32_t>> brute;
            for (std::uint32_t i = 0; i < idx.services.size(); ++i)
            {
                Service const& svc = idx.services[i];
                std::vector<ServiceKind> const kinds = Detail::KindsOf(svc.npcFlags, svc.bagSlots, svc.mailbox);
                if (svc.mapId == MAP_A && std::find(kinds.begin(), kinds.end(), ServiceKind::QuestGiver) != kinds.end())
                    brute.push_back({Detail::Distance(x, y, 10.0f, svc.x, svc.y, svc.z), i});
            }
            std::sort(brute.begin(), brute.end());
            std::vector<Nearby> const got = idx.Nearest(ServiceKind::QuestGiver, MAP_A, x, y, 10.0f, 3);
            bool ok = got.size() == 3;
            for (std::size_t i = 0; ok && i < got.size(); ++i)
                ok = got[i].index == brute[i].second;
            mismatches += ok ? 0 : 1;
        }
    Check(mismatches == 0,
          "points outside the occupied cells get the exact nearest (" + std::to_string(mismatches) + " mismatches)");
}

// Review note WT12b b: the spread of the hub radii
void Radii(PlannerIndexes const& idx)
{
    HubRadii const r = idx.RadiusStats();
    std::vector<float> radii;
    for (Hub const& hub : idx.hubs)
        radii.push_back(hub.radius);
    std::sort(radii.begin(), radii.end());
    Check(r.count == 3 && r.max == radii.back() && r.p50 == radii[1] && r.p95 == radii.back() && r.wide == 0,
          "radius percentiles of three hubs (nearest rank)");
    Check(r.widest.size() == 3 && idx.hubs[r.widest.front()].radius == radii.back() &&
              idx.hubs[r.widest.back()].radius == radii.front(),
          "the widest hubs, widest first");
    GatherInput wide;
    for (int i = 0; i < 12; ++i)
        wide.spawns.push_back(
            Spawn(MAP_A, 80.0f * static_cast<float>(i), 0.0f, static_cast<std::uint32_t>(i), NpcFlag::VENDOR, 1, 1));
    HubRadii const one = PlannerIndexes::Build(wide).RadiusStats();
    Check(one.count == 1 && one.wide == 1 && one.max > HUB_WIDE_YARDS && one.p50 == one.max,
          "a chain of services 80 yd apart is one wide hub");
    Check(PlannerIndexes().RadiusStats().count == 0, "no hubs: empty stats");
}
}  // namespace

int main()
{
    GatherInput const input = Synthetic();
    PlannerIndexes const idx = PlannerIndexes::Build(input);
    Zones(idx);
    Services(idx);
    Hubs(idx, input);
    Crowding();
    Publication(input);
    Memory(idx);
    Guards(idx);
    Radii(idx);
    std::vector<Nearby> const near = idx.Nearest(ServiceKind::Vendor, MAP_A, 100.0f, 100.0f, 10.0f, 3, NEAR_YARDS);
    std::printf("PLANNER indexes selftest: %zu services, %zu hubs, about %llu bytes; nearest vendor %.1f yd\n",
                idx.services.size(), idx.hubs.size(), static_cast<unsigned long long>(idx.stats.estimatedBytes),
                near.empty() ? -1.0 : static_cast<double>(near[0].distance));
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
