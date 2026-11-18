/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BridgeNameIndex.h"

#include <atomic>
#include <chrono>
#include <memory>

#include "Log.h"
#include "ObjectMgr.h"
#include "QuestDef.h"

namespace PlayerbotBridge
{
namespace
{
std::unique_ptr<NameIndex> built;
std::atomic<NameIndex const*> current{nullptr};
constexpr double BYTES_PER_MB = 1024.0 * 1024.0;
}  // namespace

void StartNameIndex()
{
    if (current.load(std::memory_order_acquire))
        return;

    auto const start = std::chrono::steady_clock::now();
    auto index = std::make_unique<NameIndex>();
    ItemTemplateContainer const* items = sObjectMgr->GetItemTemplateStore();
    if (items)
        for (auto const& [entry, itemTemplate] : *items)
            index->items.Add(itemTemplate.Name1, entry);
    for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
        if (quest)
            index->quests.Add(quest->GetTitle(), id);
    index->buildMs = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    LOG_INFO("server.loading", "Name index: {} items, {} quests, built in {} ms, about {:.1f} MB (estimate)",
             index->items.Size(), index->quests.Size(), index->buildMs,
             static_cast<double>(index->items.EstimatedBytes() + index->quests.EstimatedBytes()) / BYTES_PER_MB);
    built = std::move(index);
    current.store(built.get(), std::memory_order_release);
}

NameIndex const* NameIndexNow() { return current.load(std::memory_order_acquire); }
}  // namespace PlayerbotBridge
