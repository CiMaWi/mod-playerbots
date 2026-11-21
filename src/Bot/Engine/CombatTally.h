/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COMBATTALLY_H
#define _PLAYERBOT_COMBATTALLY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// The fight tally (combat-foundation CF1): per bot and fight, numbers taken at the combat engine's ticks and one
// world-log Fight row at the fight's end (CombatTrace.cpp writes it). Pure: no Player, no clock of its own; the
// stamps are getMSTime() values, so every difference is an unsigned subtraction and survives its wrap.
namespace CombatTally
{
// One combat-engine tick: an action ran, nothing ran and a cast failed for lack of power, nothing ran though the bot
// could act, or it could not act (dead, casting, lost control, global cooldown)
enum class TickKind : std::uint8_t
{
    Acted,
    NoPower,
    Idle,
    Busy
};

constexpr std::size_t TICK_KIND_COUNT = 4;

constexpr TickKind Classify(bool executed, bool noPower, bool couldAct)
{
    if (executed)
        return TickKind::Acted;
    if (noPower)
        return TickKind::NoPower;
    return couldAct ? TickKind::Idle : TickKind::Busy;
}

// The Fight row's amount: length | idle << 15 | nopower << 30 | moving << 45, each in tenths of a second and capped
// at TIME_MAX (54.6 minutes)
constexpr std::uint32_t TIME_BITS = 15;
constexpr std::uint64_t TIME_MAX = (std::uint64_t{1} << TIME_BITS) - 1;
constexpr std::uint32_t MS_PER_TENTH = 100;

enum class TimeField : std::uint32_t
{
    Length,
    Idle,
    NoPower,
    Moving
};

constexpr std::uint64_t Tenths(std::uint32_t ms) { return std::min<std::uint64_t>(ms / MS_PER_TENTH, TIME_MAX); }

constexpr std::uint64_t PackTimes(std::uint32_t lengthMs, std::uint32_t idleMs, std::uint32_t noPowerMs,
                                  std::uint32_t movingMs)
{
    return Tenths(lengthMs) | Tenths(idleMs) << TIME_BITS | Tenths(noPowerMs) << (2 * TIME_BITS) |
           Tenths(movingMs) << (3 * TIME_BITS);
}

// One field of a packed amount, in tenths of a second
constexpr std::uint32_t UnpackTime(std::uint64_t amount, TimeField field)
{
    return static_cast<std::uint32_t>((amount >> (static_cast<std::uint32_t>(field) * TIME_BITS)) & TIME_MAX);
}

// A closed fight: its length and the time each tick kind covered; the time before the first tick is uncovered
struct Summary
{
    std::uint32_t lengthMs = 0;
    std::array<std::uint32_t, TICK_KIND_COUNT> kindMs{};
    std::uint32_t movingMs = 0;
    std::uint32_t casts = 0;
    std::uint32_t maxEnemies = 0;
    std::uint32_t groupSize = 1;

    std::uint32_t Of(TickKind kind) const { return kindMs[static_cast<std::size_t>(kind)]; }

    std::uint32_t Covered() const
    {
        std::uint32_t covered = 0;
        for (std::uint32_t const ms : kindMs)
            covered += ms;
        return covered;
    }

    std::uint32_t Uncovered() const
    {
        std::uint32_t const covered = Covered();
        return covered < lengthMs ? lengthMs - covered : 0;
    }
};

// The open fight of one bot. The time from a tick to the next tick, or to the close, belongs to the earlier tick's
// kind, and to moving when that tick saw the bot move.
class Fight
{
public:
    bool IsOpen() const { return _open; }

    // The enter edge; ignored while a fight is open
    void Open(std::uint32_t nowMs, std::uint32_t groupSize)
    {
        if (_open)
            return;

        *this = Fight();
        _open = true;
        _startMs = nowMs;
        _sums.groupSize = std::max<std::uint32_t>(groupSize, 1);
    }

    // A combat-engine tick of the open fight; cast: the executed action was a spell cast
    void Tick(std::uint32_t nowMs, TickKind kind, bool moving, std::uint32_t enemies, bool cast)
    {
        if (!_open)
            return;

        Attribute(nowMs);
        _ticked = true;
        _lastTickMs = nowMs;
        _lastKind = kind;
        _lastMoving = moving;
        _sums.maxEnemies = std::max(_sums.maxEnemies, enemies);
        if (cast)
            ++_sums.casts;
    }

    // The leave edge: the last stretch attributed, the summary returned and the fight reset. The core can repeat the
    // leave edge, so a close without an open fight returns nothing.
    std::optional<Summary> Close(std::uint32_t nowMs)
    {
        if (!_open)
            return std::nullopt;

        Attribute(nowMs);
        Summary summary = _sums;
        summary.lengthMs = nowMs - _startMs;
        *this = Fight();
        return summary;
    }

private:
    void Attribute(std::uint32_t nowMs)
    {
        if (!_ticked)
            return;

        std::uint32_t const elapsed = nowMs - _lastTickMs;
        _sums.kindMs[static_cast<std::size_t>(_lastKind)] += elapsed;
        if (_lastMoving)
            _sums.movingMs += elapsed;
    }

    Summary _sums;
    std::uint32_t _startMs = 0;
    std::uint32_t _lastTickMs = 0;
    TickKind _lastKind = TickKind::Busy;
    bool _open = false;
    bool _ticked = false;
    bool _lastMoving = false;
};
}  // namespace CombatTally

#endif
