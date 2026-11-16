/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TACTICFOLLOWERSCHECKS_H
#define _PLAYERBOT_TACTICFOLLOWERSCHECKS_H

#include <format>
#include <string>

#include "TacticFollowers.h"

// CS5 design 6.4: the check hook's live test admits a follower whenever that follower's own guard can pass, for every
// combination of the switches, scopes, masks and contexts. The guards are stated here apart from the table, as
// OnCheck's callees read them at their entry. check(ok, what) as TacticChecks.h's TacticCheck.
template <typename Check>
void RunFollowerChecks(Check const& check)
{
    uint32 const scopeAll = TACTIC_SCOPE_ALL;
    uint32 const contexts = TACTIC_CONTEXT_COUNT;
    std::string firstUnsound;
    std::string firstInexact;
    uint32 cases = 0;
    for (uint32 switchBits = 0; switchBits < 16; ++switchBits)
    {
        bool const tacticsEnable = (switchBits & 1) != 0;
        bool const trackEnable = (switchBits & 2) != 0;
        bool const interruptFollows = (switchBits & 4) != 0;
        bool const memoryEnable = (switchBits & 8) != 0;
        for (uint32 scopeSeed = 0; scopeSeed <= scopeAll; ++scopeSeed)
        {
            TacticFollowerSwitches switches;
            switches.TacticsEnable = tacticsEnable;
            switches.TrackEnable = trackEnable;
            switches.InterruptFollows = interruptFollows;
            for (uint32 index = 0; index < TACTIC_FOLLOWER_COUNT; ++index)
                switches.Scopes[index] = static_cast<uint8>((scopeSeed + 5 * index) & scopeAll);

            TacticFollowerTable const table = BuildTacticFollowerTable(switches);
            for (uint32 context = 0; context < contexts; ++context)
            {
                for (uint32 index = 0; index < TACTIC_FOLLOWER_COUNT; ++index)
                {
                    TacticFollower const follower = static_cast<TacticFollower>(index);
                    bool const switchOn = (follower != TacticFollower::Natural || trackEnable) &&
                                          (follower != TacticFollower::Interrupt || interruptFollows);
                    bool const expected = tacticsEnable && switchOn &&
                                          ScopeAllows(switches.Scopes[index], static_cast<TacticContext>(context));
                    bool const inTable = (table[context] & TacticFollowerBit(follower)) != 0;
                    if (expected != inTable && firstInexact.empty())
                        firstInexact = std::format("switches {} scope seed {} context {} follower {}", switchBits,
                                                   scopeSeed, context, TACTIC_FOLLOWER_NAMES[index]);
                }
            }

            // The four followers' tactics and Fake, which no follower reads
            for (uint32 maskBits = 0; maskBits < 32; ++maskBits)
            {
                uint32 mask = (maskBits & 16) ? TacticBit(TacticId::Fake) : 0;
                for (uint32 index = 0; index < TACTIC_FOLLOWER_COUNT; ++index)
                {
                    if (maskBits & (1u << index))
                        mask |= TacticBit(TACTIC_FOLLOWER_TACTICS[index]);
                }

                for (uint32 caseBits = 0; caseBits < 8 * contexts * contexts; ++caseBits)
                {
                    bool const counting = (caseBits & 1) != 0;
                    bool const memoryOpen = (caseBits & 2) != 0;
                    bool const focusPick = (caseBits & 4) != 0;
                    TacticContext const stored = static_cast<TacticContext>((caseBits >> 3) % contexts);
                    TacticContext const probed = static_cast<TacticContext>((caseBits >> 3) / contexts);
                    auto const here = [&](TacticFollower follower)
                    {
                        uint32 const index = static_cast<uint32>(follower);
                        return IsTacticAllowed(tacticsEnable, mask, TACTIC_FOLLOWER_TACTICS[index],
                                               switches.Scopes[index], stored);
                    };
                    // ObserveOpponents opens or closes the memory against the running match: an open memory or a
                    // probe of a match
                    bool const observe = memoryEnable && (memoryOpen || probed == TacticContext::Arena ||
                                                          probed == TacticContext::Battleground);
                    bool const natural = trackEnable && here(TacticFollower::Natural);
                    bool const interrupt = interruptFollows && here(TacticFollower::Interrupt);
                    bool const focus = here(TacticFollower::FocusTarget) || focusPick;
                    bool const defensive = counting || here(TacticFollower::Defensive);
                    TacticCheckInputs inputs;
                    inputs.Counting = counting;
                    inputs.Leftover = memoryOpen || focusPick;
                    inputs.MemoryEnable = memoryEnable;
                    inputs.Mask = mask;
                    inputs.Stored = stored;
                    inputs.Probed = probed;
                    ++cases;
                    if ((observe || natural || interrupt || focus || defensive) && !TacticCheckLive(table, inputs) &&
                        firstUnsound.empty())
                        firstUnsound = std::format("switches {} scope seed {} mask {} case {}", switchBits, scopeSeed,
                                                   maskBits, caseBits);
                }
            }
        }
    }

    check(firstInexact.empty(), "the follower table is the followers' guards per context " + firstInexact);
    check(firstUnsound.empty(), "the live test admits every follower whose guard passes " + firstUnsound);
    check(cases == 16 * (scopeAll + 1) * 32 * 8 * contexts * contexts, "the follower cases ran in full");

    // Main's switches (tactics off, memory on): a bot outside a match is not live, one in a battleground is
    TacticFollowerTable const offTable = BuildTacticFollowerTable(TacticFollowerSwitches());
    TacticCheckInputs plain;
    plain.MemoryEnable = true;
    plain.Mask = TACTIC_ALL_MASK;
    check(!TacticCheckLive(offTable, plain), "tactics off, memory on: a bot in PvE is not live");
    plain.Probed = TacticContext::Battleground;
    check(TacticCheckLive(offTable, plain), "tactics off, memory on: a bot in a battleground is live");
    plain.Probed = TacticContext::OpenWorldPvp;
    check(!TacticCheckLive(offTable, plain), "tactics off, memory on: open-world PvP is not live");
    check(TacticFollowersOfMask(TacticBit(TacticId::Fake) | TacticBit(TacticId::Hold)) == 0,
          "no follower reads the Fake or Hold bits");
}

#endif
