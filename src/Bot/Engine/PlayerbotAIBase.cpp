/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotAIBase.h"

#include "BotEvents.h"
#include "NaturalReaction.h"
#include "PerfMonitor.h"
#include "Playerbots.h"

PlayerbotAIBase::PlayerbotAIBase(bool isBotAI) : nextAICheckDelay(0), _isBotAI(isBotAI) {}

void PlayerbotAIBase::UpdateAI(uint32 elapsed, bool minimal)
{
    if (totalPmo)
        totalPmo->finish();

    totalPmo = sPerfMonitor.start(PERF_MON_TOTAL, "PlayerbotAIBase::FullTick");

    TickDelay(elapsed);

    if (!CanUpdateAI())
        return;

    UpdateAIInternal(elapsed, minimal);
    YieldThread(nullptr);
}

void PlayerbotAIBase::SetNextCheckDelay(uint32 const delay)
{
    // if (nextAICheckDelay < delay)
    // LOG_DEBUG("playerbots", "Setting lesser delay {} -> {}", nextAICheckDelay, delay);

    nextAICheckDelay = delay;
    holdDelay = delay;
    thinkDelay = false;

    // if (nextAICheckDelay > sPlayerbotAIConfig.GlobalCoolDown)
    // LOG_DEBUG("playerbots",  "std::set next check delay: {}", nextAICheckDelay);
}

void PlayerbotAIBase::TickDelay(uint32 elapsed)
{
    nextAICheckDelay = nextAICheckDelay > elapsed ? nextAICheckDelay - elapsed : 0;
    holdDelay = holdDelay > elapsed ? holdDelay - elapsed : 0;
    if (!nextAICheckDelay)
        thinkDelay = false;
}

void PlayerbotAIBase::SetPollDelay(uint32 delay)
{
    nextAICheckDelay = delay;
    holdDelay = 0;
    thinkDelay = true;  // the regular check does not lengthen the wait
}

void PlayerbotAIBase::SetThinkDelay(uint32 delay)
{
    nextAICheckDelay = delay;
    holdDelay = delay;
    thinkDelay = true;
}

bool PlayerbotAIBase::WakeIn(uint32 delay)
{
    uint32 const next = BotEvents::WakeDelayMs(nextAICheckDelay, holdDelay, delay);
    if (next >= nextAICheckDelay)
        return false;
    nextAICheckDelay = next;
    return true;
}

void PlayerbotAIBase::IncreaseNextCheckDelay(uint32 delay)
{
    nextAICheckDelay += delay;
    holdDelay += delay;

    // if (nextAICheckDelay > sPlayerbotAIConfig.GlobalCoolDown)
    //     LOG_DEBUG("playerbots",  "increase next check delay: {}", nextAICheckDelay);
}

bool PlayerbotAIBase::CanUpdateAI() { return nextAICheckDelay == 0; }

void PlayerbotAIBase::YieldThread(Player* bot, uint32 delay)
{
    if (nextAICheckDelay < delay && !thinkDelay)
    {
        // Adding a deterministic per-bot slight offset (0–200 ms) to stagger updates and prevent cpu spikes.
        uint32 offset = bot ? CheckStaggerMs(bot->GetGUID().GetCounter(), checkStaggerMs) : 0;
        nextAICheckDelay = delay + offset;
        lastCheckDelay = nextAICheckDelay;
    }
}

bool PlayerbotAIBase::IsActive() { return nextAICheckDelay < sPlayerbotAIConfig.MaxWaitForMove; }

bool PlayerbotAIBase::IsBotAI() const { return _isBotAI; }

void PlayerbotAIBase::SetActionDuration(uint32 duration)
{
    nextAICheckDelay = duration;
    holdDelay = duration;
    thinkDelay = false;
}

void PlayerbotAIBase::ResetActionDuration()
{
    nextAICheckDelay = 0;
    holdDelay = 0;
    thinkDelay = false;
}
