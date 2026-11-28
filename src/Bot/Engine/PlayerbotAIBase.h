/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTAIBASE_H
#define PLAYERBOTS_PLAYERBOTAIBASE_H

#include "Define.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"

class PlayerbotAIBase
{
public:
    PlayerbotAIBase(bool isBotAI);

    bool CanUpdateAI();
    void SetNextCheckDelay(uint32 const delay);
    void IncreaseNextCheckDelay(uint32 delay);
    void YieldThread(Player* bot, uint32 delay = sPlayerbotAIConfig.ReactDelay);
    virtual void UpdateAI(uint32 elapsed, bool minimal = false);
    virtual void UpdateAIInternal(uint32 elapsed, bool minimal = false) = 0;
    bool IsActive();
    bool IsBotAI() const;

    void SetActionDuration(uint32 duration);
    void ResetActionDuration();
    bool IsActionDurationActive() const { return nextAICheckDelay > 0; }
    // The scheduler (BotEvents.h): a bot's AI looks when an event, a timer or the regular check wakes it.
    // A wait for an event of at most this long: a wake ends it
    void SetPollDelay(uint32 delay);
    // The next check exactly then (a chosen time to think): the regular check's delay does not lengthen it
    void SetThinkDelay(uint32 delay);
    // An event came: the next check this long from now, never before the wait an action asked for is over and
    // never later than the check already due. True when it brought the check forward
    bool WakeIn(uint32 delay);
    void Wake() { WakeIn(0); }
    // A step that took no time: the same check goes on with the next step (PlayerbotAI::UpdateAI's bounded loop)
    void ChainOn() { chainOn = true; }
    // The delay YieldThread last set, its stagger included
    uint32 GetLastCheckDelay() const { return lastCheckDelay; }

protected:
    // The check delay's steps: every write of the delay outside these goes through them
    void TickDelay(uint32 elapsed);
    uint32 nextAICheckDelay;
    // The part of nextAICheckDelay an action asked for (SetNextCheckDelay and its kin); the rest is the regular check's
    // wait, which Wake() ends
    uint32 holdDelay = 0;
    bool thinkDelay = false;
    bool chainOn = false;
    // YieldThread's stagger from the guid (Playerbots.Natural.Cadence.StaggerMs narrows it for bots in its scope)
    uint32 checkStaggerMs = UPSTREAM_CHECK_STAGGER_MS;
    uint32 lastCheckDelay = 0;
    class PerfMonitorOperation* totalPmo = nullptr;

private:
    bool _isBotAI;
};

#endif
