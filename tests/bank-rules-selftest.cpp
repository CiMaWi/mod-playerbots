/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the bank rules (src/Ai/World/Rpg/BankRules.h): the scenarios the console's
// "playerbots bank selftest" runs (BankRulesSelftest.h), printed with every decision. Exit code 1 on a failed check.
#include <iostream>
#include <string>
#include <vector>

#include "BankRulesSelftest.h"

int main()
{
    std::vector<std::string> lines;
    BankMail::BankSelftestDetail::Result const result = BankMail::RunBankRulesSelftest(lines);
    for (std::string const& line : lines)
        std::cout << line << "\n";
    return result.failed ? 1 : 0;
}
