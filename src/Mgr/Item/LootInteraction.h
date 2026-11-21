/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LOOTINTERACTION_H
#define PLAYERBOTS_LOOTINTERACTION_H

#include <cstdint>

namespace LootInteraction
{
constexpr uint32_t PhaseMs = 15000;

enum class Phase : uint8_t
{
    Idle,
    Preparing,
    Requested,
    AwaitingRelease
};

struct Transaction
{
    uint64_t Guid = 0;
    uint32_t Generation = 0;
    Phase Current = Phase::Idle;
    bool Expired = false;
    bool Worked = false;
    bool Deferred = false;
    bool Left = false;

    bool Pending() const { return Current == Phase::Requested || Current == Phase::AwaitingRelease; }
    bool AllowsAttempt(uint64_t guid) const
    {
        return !Guid || (Guid == guid && Current == Phase::Preparing && !Expired && !_paused);
    }

    void Observe(uint32_t now, bool paused)
    {
        if (!Guid || Expired)
            return;

        uint32_t const elapsed = now - _sampledAt;
        if (!_paused)
        {
            if (elapsed >= _remainingMs)
                Expired = true;
            else
                _remainingMs -= elapsed;
        }
        _sampledAt = now;
        _paused = paused;
    }

    bool Prepare(uint64_t guid, uint32_t now)
    {
        if (!guid || !AllowsAttempt(guid))
            return false;
        if (Guid)
            return true;

        Guid = guid;
        ++Generation;
        Current = Phase::Preparing;
        _sampledAt = now;
        _remainingMs = PhaseMs;
        return true;
    }

    bool Begin(uint64_t guid, uint32_t now)
    {
        Observe(now, _paused);
        if (!Prepare(guid, now))
            return false;
        Current = Phase::Requested;
        return true;
    }

    bool Open(uint64_t guid, uint32_t now)
    {
        Observe(now, _paused);
        if (Guid != guid || Current != Phase::Requested || Expired)
            return false;
        Current = Phase::AwaitingRelease;
        _remainingMs = PhaseMs;
        return true;
    }

    void Clear()
    {
        Guid = 0;
        Current = Phase::Idle;
        Expired = false;
        Worked = false;
        Deferred = false;
        Left = false;
        _paused = false;
    }

private:
    uint32_t _sampledAt = 0;
    uint32_t _remainingMs = 0;
    bool _paused = false;
};

enum class Attempt : uint8_t
{
    Failed,
    Prepared,
    Requested
};

// Both preparation steps consume the scheduled tick, without reporting a request to direct callers.
template <class Adapter>
Attempt Execute(Adapter& adapter)
{
    if (!adapter.Admit())
        return Attempt::Failed;
    if (adapter.PrepareMotion())
        return Attempt::Prepared;
    if (!adapter.Request())
    {
        adapter.Defer();
        return Attempt::Failed;
    }

    adapter.Begin();
    return Attempt::Requested;
}

// A loot target the bot opened and left an item in for lack of room (upkeep WU36): it stays in the bot's loot list as
// left and is not attempted again until the bot's free bag slots rise above their count at the leave (a slot freed,
// a bag added or an item destroyed for space); it leaves the list when its object is gone. The count is read only
// for a left target, so a target that was never left costs nothing more.
struct LeftForRoom
{
    bool Left = false;
    uint32_t FreeAtLeave = 0;

    void Leave(uint32_t freeSlots)
    {
        Left = true;
        FreeAtLeave = freeSlots;
    }
    bool Blocks(uint32_t freeSlots) const { return Left && freeSlots <= FreeAtLeave; }
};

// The free slots a response leaves: the items it stored are still queued packets when it ends, and each may take a
// slot (one that stacks errs towards an earlier attempt, never a later one)
inline uint32_t FreeAfterStore(uint32_t freeSlots, uint32_t stored)
{
    return freeSlots > stored ? freeSlots - stored : 0;
}

// The response disposition and matching core close share this seam with the offline adapter. A target left for room
// stays as left; a game object with nothing stored keeps its time backoff; anything else is done and removed.
template <class Defer, class Leave, class Remove, class Release>
void CompleteResponse(Transaction& state, uint64_t guid, uint32_t gold, uint32_t stored, bool gameObject,
                      bool leftForRoom, Defer&& defer, Leave&& leave, Remove&& remove, Release&& release)
{
    bool const deferred = !leftForRoom && !stored && gameObject;
    if (state.Guid == guid && state.Current == Phase::AwaitingRelease)
    {
        state.Worked = gold != 0 || stored != 0;
        state.Deferred = deferred;
        state.Left = leftForRoom;
    }
    if (leftForRoom)
        leave();
    else if (deferred)
        defer();
    else
        remove();
    release();
}

template <class Finish>
bool ConsumeClosed(Transaction const& state, uint64_t window, uint32_t& completed, Finish&& finish)
{
    if (state.Current != Phase::AwaitingRelease || window == state.Guid)
        return false;
    if (state.Worked && !state.Expired)
        ++completed;
    finish(state.Expired    ? "timeout"
           : state.Worked   ? "closed"
           : state.Left     ? "left"
           : state.Deferred ? "deferred"
                            : "empty");
    return true;
}

template <class Adapter>
bool ConsumeExpiry(Transaction const& state, Adapter& adapter)
{
    if (!state.Expired)
        return false;
    adapter.Cancel("timeout");
    return true;
}
}  // namespace LootInteraction

#endif
