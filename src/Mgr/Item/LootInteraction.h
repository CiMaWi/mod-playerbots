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

// The response disposition and matching core close share this seam with the offline adapter.
template <class Defer, class Remove, class Release>
void CompleteResponse(Transaction& state, uint64_t guid, uint32_t gold, uint32_t stored, bool gameObject, Defer&& defer,
                      Remove&& remove, Release&& release)
{
    bool const deferred = !stored && gameObject;
    if (state.Guid == guid && state.Current == Phase::AwaitingRelease)
    {
        state.Worked = gold != 0 || stored != 0;
        state.Deferred = deferred;
    }
    if (deferred)
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
    finish(state.Expired ? "timeout" : state.Worked ? "closed" : state.Deferred ? "deferred" : "empty");
    return true;
}

inline bool MountAllowed(bool hasInteraction, bool hasOpenWindow) { return !hasInteraction && !hasOpenWindow; }

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
