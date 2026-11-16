/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_TACTICSTRATEGYCHECKS_H
#define _PLAYERBOT_TACTICSTRATEGYCHECKS_H

#include <format>
#include <string>

#include "TacticIds.h"

// CS25a: TacticStrategyWanted (TacticIds.h) for every tactic id, the master, the tactic's own switch and the harness
// switch. The expected value is written here apart from the predicate: the system on, and the own switch or the
// harness. check(ok, what) and say(line) as TacticChecks.h's TacticCheck and TacticSay.
template <typename Check, typename Say>
void RunStrategyChecks(Check const& check, Say const& say)
{
    uint32 cells = 0;
    uint32 wanted = 0;
    std::string firstWrong;
    std::string firstMasterOff;
    std::string firstOwnOff;
    for (uint32 index = 0; index < TACTIC_COUNT; ++index)
    {
        TacticId const id = static_cast<TacticId>(index);
        for (uint32 bits = 0; bits < 8; ++bits)
        {
            bool const master = (bits & 1) != 0;
            bool const own = (bits & 2) != 0;
            bool const harness = (bits & 4) != 0;
            bool const expected = master && (own || harness);
            bool const got = TacticStrategyWanted(master, id, own, harness);
            ++cells;
            if (got)
                ++wanted;

            std::string const cell =
                std::format("{} master {} own {} harness {}", TACTIC_DEFINITIONS[index].Name, master, own, harness);
            if (got != expected && firstWrong.empty())
                firstWrong = cell;

            if (!master && got && firstMasterOff.empty())
                firstMasterOff = cell;

            if (master && own && !got && firstOwnOff.empty())
                firstOwnOff = cell;
        }
    }

    check(cells == TACTIC_COUNT * 8, "every tactic id x master x own x harness cell ran");
    check(firstWrong.empty(), "every cell is the master and (the own switch or the harness) " + firstWrong);
    check(firstMasterOff.empty(), "with the master off no tactic's strategy is wanted " + firstMasterOff);
    check(firstOwnOff.empty(), "with the master and the tactic's own switch on the strategy is wanted " + firstOwnOff);

    // The two-tactic sites (Hold or Plan, Cc or OwnCc) read the or of two tactics under the one master
    std::string firstPairWrong;
    for (uint32 bits = 0; bits < 32; ++bits)
    {
        bool const master = (bits & 1) != 0;
        bool const first = (bits & 2) != 0;
        bool const second = (bits & 4) != 0;
        bool const harness = (bits & 8) != 0;
        bool const old = master && (first || second || harness);
        bool const holdPlan = TacticStrategyWanted(master, TacticId::Hold, first, harness) ||
                              TacticStrategyWanted(master, TacticId::Plan, second, harness);
        bool const ccOwnCc = TacticStrategyWanted(master, TacticId::Cc, first, harness) ||
                             TacticStrategyWanted(master, TacticId::OwnCc, second, harness);
        if ((holdPlan != old || ccOwnCc != old) && firstPairWrong.empty())
            firstPairWrong = std::format("master {} first {} second {} harness {}", master, first, second, harness);
    }

    check(firstPairWrong.empty(),
          "the Hold or Plan and Cc or OwnCc sites keep their one-clause meaning " + firstPairWrong);
    say(std::format("{} cells over {} tactics, {} wanted; every cell equals master and (own or harness)", cells,
                    static_cast<uint32>(TACTIC_COUNT), wanted));
}

#endif
