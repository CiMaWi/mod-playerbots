/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatTally.h"

namespace CombatTally
{
void PowerGate::Note(std::uint32_t amount, std::uint32_t nowMs, std::uint16_t row)
{
    if (_thisTick && amount >= _amount)
        return;

    _amount = amount;
    _atMs = nowMs;
    _row = row;
    _thisTick = true;
}

bool PowerGate::Unmet(std::uint32_t nowMs, std::uint32_t points) const
{
    return _amount && nowMs - _atMs <= GATE_WINDOW_MS && points < _amount;
}

void Fight::Open(std::uint32_t nowMs, std::uint32_t groupSize, TickReading const& pull, bool ranged)
{
    if (_open)
        return;

    *this = Fight();
    _open = true;
    _ranged = ranged;
    Restart(nowMs, std::max<std::uint32_t>(groupSize, 1), pull);
}

void Fight::Tick(std::uint32_t nowMs, Share share, std::uint32_t enemies, bool cast, TickReading const& reading)
{
    if (!_open)
        return;

    if (!_ticked)
    {
        _ticked = true;
        _lastShare = share;
        _lastAtCap = reading.atCap;
    }
    Attribute(nowMs);

    _lastShare = share;
    _lastAtCap = reading.atCap;
    _sums.lowestHealthPct = std::min(_sums.lowestHealthPct, reading.healthPct);
    _sums.maxEnemies = std::max(_sums.maxEnemies, enemies);
    if (cast)
        ++_sums.casts;
}

bool Fight::SplitDue(std::uint32_t nowMs) const { return _open && nowMs - _startMs >= SPLIT_MS; }

std::optional<Summary> Fight::Split(std::uint32_t nowMs, TickReading const& pull)
{
    if (!SplitDue(nowMs))
        return std::nullopt;

    Summary const part = Part(nowMs);
    Restart(nowMs, part.groupSize, pull);
    _ticked = false;
    return part;
}

std::optional<Summary> Fight::Close(std::uint32_t nowMs)
{
    if (!_open)
        return std::nullopt;

    Summary const summary = Part(nowMs);
    *this = Fight();
    return summary;
}

Summary Fight::Part(std::uint32_t nowMs)
{
    if (_ticked)
        Attribute(nowMs);
    else
        _sums.shareMs[static_cast<std::size_t>(Share::Idle)] = nowMs - _startMs;
    Summary part = _sums;
    part.lengthMs = nowMs - _startMs;
    part.ticked = _ticked;
    return part;
}

void Fight::Restart(std::uint32_t nowMs, std::uint32_t groupSize, TickReading const& pull)
{
    _sums = Summary();
    _sums.groupSize = groupSize;
    _sums.pullHealthPct = pull.healthPct;
    _sums.pullPowerPct = pull.powerPct;
    _sums.lowestHealthPct = pull.healthPct;
    _startMs = nowMs;
    _lastTickMs = nowMs;
}

void Fight::Attribute(std::uint32_t nowMs)
{
    std::uint32_t const elapsed = nowMs - _lastTickMs;
    _sums.shareMs[static_cast<std::size_t>(_lastShare)] += elapsed;
    if (_lastAtCap)
        _sums.capMs += elapsed;
    _lastTickMs = nowMs;
}
}  // namespace CombatTally
