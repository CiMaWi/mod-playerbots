/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_COMBATRECORD_H
#define _PLAYERBOT_COMBATRECORD_H

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

class PlayerbotAI;

// The combat record of a casttrace bot (combat-foundation, MEASURES §1-§3): its per-bot block, made at the bot's
// first traced tick and kept on PlayerbotAI (combatRecord). Written by the bot's own map thread only.
namespace CombatRecord
{
constexpr std::size_t REJECT_MARKS = 256;
constexpr std::uint32_t REJECT_REPEAT_MS = 10000;

struct State
{
    // combat_reject: when a pair of action and reason last wrote its row in this fight, by hash; two pairs on one
    // mark share a row
    std::array<std::uint32_t, REJECT_MARKS> rejectMs{};
    std::bitset<REJECT_MARKS> rejectSeen;

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
}  // namespace CombatRecord

#endif
