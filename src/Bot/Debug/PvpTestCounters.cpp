/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PvpTestCounters.h"

#include "PlayerbotAI.h"

static_assert(PVPTEST_NOTICE_CATEGORY_COUNT == SkillNoticeCounters::Categories,
              "the executed counts have one slot per notice category");

PvpTestCounterBlock const& PlayerbotAI::GetPvpTestCounters() const
{
    static PvpTestCounterBlock const none{};
    return pvpTestCounters ? *pvpTestCounters : none;
}

void PlayerbotAI::AddPvpTestOwnBreak()
{
    if (pvpTestCounters)
        ++pvpTestCounters->Cc.OwnBreaks;
}

void PlayerbotAI::AddPvpTestPartnerDispel(uint32 ageMs)
{
    if (!pvpTestCounters)
        return;

    ++pvpTestCounters->Cc.PartnerDispel;
    pvpTestCounters->Tactics[static_cast<uint32>(TacticCounter::PartnerDispelMs)] += ageMs;
}

void PlayerbotAI::AddPvpTestTactic(TacticCounter counter, uint32 amount)
{
    if (pvpTestCounters)
        pvpTestCounters->Tactics[static_cast<uint32>(counter)] += amount;
}
