/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "DangerFold.h"
#include "DangerRules.h"
#include "Log.h"
#include "PlayerbotsDatabase.h"
#include "QueryResult.h"
#include "Timer.h"
#include "TravelGraph.h"

namespace
{
// `aw term` (AW37a): the fold's store, set once by the fold; the copy without links, made at the first `off`
std::atomic<DangerRules::Known const*> foldedStore{nullptr};
std::mutex termLock;
DangerRules::Known const* unlinkedStore = nullptr;  // under termLock
}  // namespace

void DangerRules::FoldBeforeReady(TravelGraph const& graph)
{
    // The store of the load, never null after a start (the load refuses it); the fold's store is a copy with the links
    Known const* const facts = Published();
    if (!facts)
    {
        LOG_ERROR("server.playerbots", "Playerbots: no known danger to fold; the route search has no danger term");
        return;
    }
    std::uint32_t const started = getMSTime();
    std::vector<LinkDanger> links;
    FoldGraph(*facts, graph, links);
    std::size_t walks = 0;
    std::array<std::size_t, SIDE_COUNT> deadlyAtFirst{};  // deadly for a bot of level 1
    for (std::size_t index = 0; index < graph.LinkCount(); ++index)
    {
        if (graph.GetLink(static_cast<std::uint32_t>(index)).type != TravelLinkType::Walk)
            continue;
        ++walks;
        for (std::uint8_t side = 0; side < SIDE_COUNT; ++side)
            if (links[index * SIDE_COUNT + side].deadlyBelow > 1)
                ++deadlyAtFirst[side];
    }
    auto known = std::make_unique<Known>(*facts);
    known->graphLinks = graph.LinkCount();
    known->links = std::move(links);
    LOG_INFO("server.loading",
             ">> Folded known danger {} into {} walk links of {} ({} and {} deadly at level 1, Alliance and Horde), "
             "{:.1f} MB in {} ms",
             known->build, walks, known->graphLinks, deadlyAtFirst[0], deadlyAtFirst[1], known->Bytes() / 1048576.0,
             GetMSTimeDiffToNow(started));
    foldedStore.store(Publish(std::move(known)), std::memory_order_release);
}

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

bool DangerRules::SetTerm(bool on)
{
    Known const* const folded = foldedStore.load(std::memory_order_acquire);
    if (!folded)
        return false;
    std::lock_guard<std::mutex> const lock(termLock);
    if (on)
    {
        Store::published.store(folded, std::memory_order_release);
        return true;
    }
    if (unlinkedStore)
    {
        Store::published.store(unlinkedStore, std::memory_order_release);
        return true;
    }
    auto copy = std::make_unique<Known>();
    copy->build = folded->build;
    copy->maxLayers = folded->maxLayers;
    copy->keys = folded->keys;
    copy->firsts = folded->firsts;
    copy->layers = folded->layers;
    copy->graphLinks = folded->graphLinks;
    unlinkedStore = Publish(std::move(copy));
    return true;
}

bool DangerRules::TermOn()
{
    Known const* const folded = foldedStore.load(std::memory_order_acquire);
    return folded && Published() == folded;
}
