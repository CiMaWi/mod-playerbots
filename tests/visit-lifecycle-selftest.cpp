/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone lifecycle harness of the mailbox and bank visits (src/Ai/World/Rpg/VisitLifecycle.h): every state
// crossed with every event and a seeded search for a counterexample (VisitLifecycleSelftest.h), printed in full.
// Exit code 1 on a failed check.
#include <iostream>
#include <string>
#include <vector>

#include "VisitLifecycleSelftest.h"

int main()
{
    std::vector<std::string> lines;
    BankMail::VisitLifecycleDetail::Result const result = BankMail::RunVisitLifecycleSelftest(lines);
    for (std::string const& line : lines)
        std::cout << line << "\n";
    return result.failed ? 1 : 0;
}
