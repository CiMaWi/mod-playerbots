/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COMBATTALLYSTATE_H
#define _PLAYERBOT_COMBATTALLYSTATE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// The fight tally's state (combat-foundation CF1, CFM10): the types PlayerbotAI holds per bot. The rules (the
// classification, the packing) are in CombatTally.h, the bodies of Fight and PowerGate in CombatTally.cpp, so an edit
// of either does not recompile what includes PlayerbotAI.h.
namespace CombatTally
{
// Why PlayerbotAI::CanCastSpell said no, one byte on the bot (MEASURES §2.3); None: not asked since the engine cleared
// it, or it said yes
enum class CastRefusal : std::uint8_t
{
    None,
    NotKnown,
    Cooldown,
    Range,
    Sight,
    Target,
    Impossible
};

// The five shares of a fight's time, in the order the rule tests them last to first
enum class Share : std::uint8_t
{
    Idle,
    Waiting,
    Reach,
    Controlled,
    Acting
};

constexpr std::size_t SHARE_COUNT = 5;

constexpr std::uint32_t PERCENT = 100;

constexpr std::uint16_t NO_ROW = 0xFFFF;

// A closed fight (or a running part of one): its length and the time of each share, which sum to the length
struct Summary
{
    std::uint32_t lengthMs = 0;
    std::array<std::uint32_t, SHARE_COUNT> shareMs{};
    std::uint32_t capMs = 0;
    std::uint32_t casts = 0;
    std::uint32_t maxEnemies = 0;
    std::uint32_t groupSize = 1;
    std::uint32_t lowestHealthPct = PERCENT;
    std::uint32_t pullHealthPct = 0;
    std::uint32_t pullPowerPct = 0;
    // the combat engine ticked in the part; false is FightMore's mark (CF.31)
    bool ticked = false;

    std::uint32_t Of(Share share) const { return shareMs[static_cast<std::size_t>(share)]; }
};

// The reads of every combat tick: health, power and the cap
struct TickReading
{
    std::uint32_t healthPct = PERCENT;
    std::uint32_t powerPct = 0;
    bool atCap = false;
};

// An unmet power gate (a table row's PowerAtLeast term, the base's rage and energy triggers): one store per unmet
// check, the lowest amount of a tick wins; evidence of waiting while it is recent and the power still below it
class PowerGate
{
public:
    // The engine's tick begins (before its triggers)
    void NewTick() { _thisTick = false; }

    // row: the table row whose term was unmet, NO_ROW for a base trigger
    void Note(std::uint32_t amount, std::uint32_t nowMs, std::uint16_t row = NO_ROW);

    bool Unmet(std::uint32_t nowMs, std::uint32_t points) const;

    bool NotedThisTick() const { return _thisTick; }
    // The amount the tick's gate asked for (the scheduler's wait for the bot's energy)
    std::uint32_t Amount() const { return _amount; }
    std::uint16_t Row() const { return _row; }

private:
    std::uint32_t _amount = 0;
    std::uint32_t _atMs = 0;
    std::uint16_t _row = NO_ROW;
    bool _thisTick = false;
};

// The open fight of one bot. The time from a tick to the next tick, or to the close, belongs to the earlier tick's
// share (and to the cap when that tick saw the power at its cap); the time before the first tick to the first tick's.
class Fight
{
public:
    bool IsOpen() const { return _open; }
    // Read at the enter edge: a ranged bot's reach is the spell distance, a melee bot's its melee reach
    bool Ranged() const { return _ranged; }

    // The enter edge; ignored while a fight is open
    void Open(std::uint32_t nowMs, std::uint32_t groupSize, TickReading const& pull, bool ranged);

    // A combat-engine tick of the open fight; cast: the action that ran was a spell cast. It never splits.
    void Tick(std::uint32_t nowMs, Share share, std::uint32_t enemies, bool cast, TickReading const& reading);

    // The part has lasted SPLIT_MS (one comparison; the bot's update tests it before it reads anything)
    bool SplitDue(std::uint32_t nowMs) const;

    // The bot's update (CF.32): once SplitDue, the running part returned (as Close attributes it) and the sums start
    // again, unticked, with pull as the next part's pull; else nothing
    std::optional<Summary> Split(std::uint32_t nowMs, TickReading const& pull);

    // The leave edge: the last stretch attributed (a fight without a tick is all idle), the summary returned and the
    // fight reset. The core can repeat the leave edge, so a close without an open fight returns nothing.
    std::optional<Summary> Close(std::uint32_t nowMs);

private:
    // The part up to nowMs: the time since the last tick to its share, a part without a tick all idle; the mark
    Summary Part(std::uint32_t nowMs);
    void Restart(std::uint32_t nowMs, std::uint32_t groupSize, TickReading const& pull);
    void Attribute(std::uint32_t nowMs);

    Summary _sums;
    std::uint32_t _startMs = 0;
    std::uint32_t _lastTickMs = 0;
    Share _lastShare = Share::Idle;
    bool _open = false;
    bool _ticked = false;
    bool _lastAtCap = false;
    bool _ranged = false;
};
}  // namespace CombatTally

#endif
