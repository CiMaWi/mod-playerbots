/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COMBATRECORD_H
#define _PLAYERBOT_COMBATRECORD_H

#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "CombatKinds.h"
#include "CombatRecordRules.h"
#include "CombatTallyState.h"

class Player;
class PlayerbotAI;
class SpellInfo;
class Unit;

namespace CombatTally
{
enum class Share : std::uint8_t;
}

// The combat record of a casttrace bot (combat-foundation, MEASURES §1-§3): its per-bot block, kept on PlayerbotAI
// (combatRecord) and, while the bot carries the constraint, in the record's registry, which the hooks of
// CombatRecord.cpp (any map thread) search by the bot's guid. Every hook returns after one relaxed load (Any) while
// no traced bot is online.
namespace CombatRecord
{
constexpr std::size_t REJECT_MARKS = 256;
constexpr std::uint32_t REJECT_REPEAT_MS = 10000;

// Online bots carrying casttrace (the registry's size)
inline std::atomic<std::uint32_t> Traced{0};
inline bool Any() { return Traced.load(std::memory_order_relaxed) != 0; }

struct State
{
    // combat_reject: when a pair of action and reason last wrote its row in this fight, by hash; two pairs on one
    // mark share a row. The bot's own thread only.
    std::array<std::uint32_t, REJECT_MARKS> rejectMs{};
    std::bitset<REJECT_MARKS> rejectSeen;
    // in the registry (the bot's own thread only)
    bool tracked = false;
    // the rows' bot (its guid's counter, WorldLog::Decision by key): set before the registry holds the block, so a
    // hook on another map thread never reads a Player that may be logging out
    std::uint32_t key = 0;

    // a hook on another map thread fills the table, the bot's fight end writes and clears it
    std::mutex lock;
    CombatRecordRules::DealtTable dealt;

    // combat_sample: the self rows' clock and the damage table's taken at the last self row (the bot's own thread)
    CombatKinds::SampleClock sample;
    std::uint64_t takenAtSample = 0;
    // the share of the bot's last combat tick (the engine does not tick during a cast; the self row reads this)
    CombatTally::Share lastShare{};

    void NewFight() { rejectSeen.reset(); }

    // The first time a pair meets in a fight, then once every REJECT_REPEAT_MS while it keeps meeting
    bool RejectDue(std::size_t hash, std::uint32_t nowMs)
    {
        std::size_t const mark = hash % REJECT_MARKS;
        if (rejectSeen[mark] && nowMs - rejectMs[mark] < REJECT_REPEAT_MS)
            return false;

        rejectSeen[mark] = true;
        rejectMs[mark] = nowMs;
        return true;
    }
};

// The bot's block, made on first use (traced bots only)
State& StateOf(PlayerbotAI* botAI);
// casttrace's apply (repeated every tick, so a no-op once tracked) and undo; a logout drops the bot too
void Track(Player* bot);
void Untrack(Player* bot);
// A logout: the open fight's end (Fight pair, then the damage rows), then Untrack
void OnLogout(Player* bot);
// The fight's end (CombatTally::OnLeave, after the Fight pair): the combat_dealt rows (written: it lasted 0.1 s,
// CF.30), then the table is cleared
void WriteDealt(Player* bot, State& state, bool written = true);
// A traced bot's combat tick (Engine::DoNextAction): keeps the tick's share for the self row
void OnTick(PlayerbotAI* botAI, CombatTally::Share share);
// A traced bot's update while its fight is open (PlayerbotAI::UpdateAI, also through its casts, when the engine does
// not tick): the sample rows once a second (self, enemy, stack, pet; MEASURES §1), and combat_offengine's still row
// while the bot's state is not the combat engine's
void OnUpdate(PlayerbotAI* botAI, std::uint32_t nowMs);
// A traced bot's fight opens (CombatTally::OnEnter): the state at the pull (pull, has, weapon); the clock starts
void OnEnter(PlayerbotAI* botAI, std::uint32_t nowMs);

// combat_known's numbers of a spell for the bot: kinds (CombatKinds::Classify of its spell data), cd (seconds),
// cost (points)
struct Known
{
    std::uint32_t kinds = 0;
    float cd = 0.0f;
    std::uint32_t cost = 0;
};
Known KnownOf(Player* bot, SpellInfo const* info);
// A unit's power as a player reads it (CombatKinds::Points of its own power kind)
std::uint32_t PowerOf(Unit const* unit);
}  // namespace CombatRecord

#endif
