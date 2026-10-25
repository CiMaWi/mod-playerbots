/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityBudget.h"

#include "PlayerbotAIConfig.h"
#include "UpdateTime.h"

void ActivityBudget::Update()
{
    uint32 const percentile = sPlayerbotAIConfig.BotActiveAloneSmartScalePercentile;
    if (!percentile || !sPlayerbotAIConfig.BotActiveAloneSmartScale)
        return;

    // GetPercentile reads the size minus one: an empty table would wrap
    if (!sWorldUpdateTime.GetDatasetSize())
        return;

    _tickMs.store(sWorldUpdateTime.GetPercentile(static_cast<uint8>(percentile)), std::memory_order_relaxed);
}

uint32 ActivityBudget::GetTickMs() const
{
    if (!sPlayerbotAIConfig.BotActiveAloneSmartScalePercentile)
        return sWorldUpdateTime.GetMaxUpdateTimeOfCurrentTable();

    return _tickMs.load(std::memory_order_relaxed);
}
