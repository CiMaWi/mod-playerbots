/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TACTICFOLLOWERS_H
#define _PLAYERBOT_TACTICFOLLOWERS_H

#include <array>

#include "Define.h"
#include "TacticIds.h"

// The live test of BotTactics::OnCheck (CS5 design 6.4): the check hook runs its followers only for a bot one of
// them can act for. Game-free; the tactic selftests' "followers" suite proves that the test admits every follower
// whose own guard can pass.

// The four tactic-keyed followers of OnCheck (the opponent memory has no tactic bit: MemoryIn below)
enum class TacticFollower : uint8
{
    Natural,      // FeedNaturalTrack: Natural.Track.Enable and the Natural tactic here
    Interrupt,    // FollowInterruptFight: InterruptFollows and the Interrupt tactic here
    FocusTarget,  // FollowFocusTarget: the FocusTarget tactic here (or a pick left to clear: leftover)
    Defensive,    // FollowDefensive: the Defensive tactic here (or the harness counting: counting)
    Count
};

constexpr uint32 TACTIC_FOLLOWER_COUNT = static_cast<uint32>(TacticFollower::Count);
constexpr std::array<TacticId, TACTIC_FOLLOWER_COUNT> TACTIC_FOLLOWER_TACTICS = {
    TacticId::Natural, TacticId::Interrupt, TacticId::FocusTarget, TacticId::Defensive};
constexpr std::array<char const*, TACTIC_FOLLOWER_COUNT> TACTIC_FOLLOWER_NAMES = {
    "FeedNaturalTrack", "FollowInterruptFight", "FollowFocusTarget", "FollowDefensive"};

constexpr uint8 TacticFollowerBit(TacticFollower follower) { return 1u << static_cast<uint32>(follower); }

// One byte per TacticContext: the followers that can pass their guard in that (stored) context
using TacticFollowerTable = std::array<uint8, TACTIC_CONTEXT_COUNT>;

// The switches the table is built from (config load and reload)
struct TacticFollowerSwitches
{
    bool TacticsEnable = false;
    bool TrackEnable = false;                           // Playerbots.Natural.Track.Enable
    bool InterruptFollows = false;                      // InterruptFollows(TacticInterrupt)
    std::array<uint8, TACTIC_FOLLOWER_COUNT> Scopes{};  // the followers' tactics' scopes, in TacticFollower order
};

constexpr TacticFollowerTable BuildTacticFollowerTable(TacticFollowerSwitches const& switches)
{
    TacticFollowerTable table{};
    if (!switches.TacticsEnable)
        return table;

    for (uint32 context = 0; context < TACTIC_CONTEXT_COUNT; ++context)
    {
        for (uint32 index = 0; index < TACTIC_FOLLOWER_COUNT; ++index)
        {
            TacticFollower const follower = static_cast<TacticFollower>(index);
            bool const switchOn = (follower != TacticFollower::Natural || switches.TrackEnable) &&
                                  (follower != TacticFollower::Interrupt || switches.InterruptFollows);
            if (switchOn && ScopeAllows(switches.Scopes[index], static_cast<TacticContext>(context)))
                table[context] |= TacticFollowerBit(follower);
        }
    }

    return table;
}

// The bot's tactic mask folded to the followers' bits
constexpr uint8 TacticFollowersOfMask(uint32 mask)
{
    uint8 followers = 0;
    for (uint32 index = 0; index < TACTIC_FOLLOWER_COUNT; ++index)
    {
        if (mask & TacticBit(TACTIC_FOLLOWER_TACTICS[index]))
            followers |= TacticFollowerBit(static_cast<TacticFollower>(index));
    }

    return followers;
}

struct TacticCheckInputs
{
    bool Counting = false;  // the harness counts the bot (pvpTestCounting)
    bool Leftover = false;  // a follower left state a later check must close (an open memory, a focus pick)
    uint32 Mask = 0;        // the bot's tactic mask
    TacticContext Stored = TacticContext::Pve;  // the context the guards read (the last check's store)
    TacticContext Probed = TacticContext::Pve;  // this check's probe
};

// live = counting || leftover || (FollowersFor[stored] & botFollowers) != 0 || MemoryIn[probed]
constexpr bool TacticCheckLive(TacticFollowerTable const& table, TacticCheckInputs const& inputs)
{
    bool const memoryIn = inputs.Probed == TacticContext::Arena || inputs.Probed == TacticContext::Battleground;
    return inputs.Counting || inputs.Leftover ||
           (table[static_cast<uint32>(inputs.Stored)] & TacticFollowersOfMask(inputs.Mask)) != 0 || memoryIn;
}

#endif
