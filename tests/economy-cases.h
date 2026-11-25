/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The economy reference's cases as fixed expected values (professions plan WR29, DECISIONS WR.28): each value is
// copied with its case id and check name from the hub's docs/economy-reference/cases/craft_processing.json and never
// computed from the module's functions (the package's README, "Use as an offline oracle"). A harness feeds the case's
// inputs (the world database's loot rows, a recipe's colours) to the module and compares; the negative control reads
// each configured value at stock rates and must miss at least one. The package's probabilities of "at least one" and
// its joint tables are not module outputs (the module counts expected units) and are not checked here.
#ifndef _PLAYERBOT_TESTS_ECONOMY_CASES_H
#define _PLAYERBOT_TESTS_ECONOMY_CASES_H

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "ServerRates.h"

namespace EconomyCases
{
// config/worldserver.overrides.conf of the hub (lines 34-56): SkillGain.Crafting and .Gathering 2, every
// Rate.Drop.Item.<quality>, Referenced, ReferencedAmount, GroupAmount and Rate.Drop.Money 2; SkillChance.* default
inline Recipes::ServerRates Ours()
{
    Recipes::ServerRates ours;
    ours.craftStep = 2;
    ours.gatherStep = 2;
    ours.quality.fill(2.0f);
    ours.referenced = 2.0f;
    ours.referencedAmount = 2.0f;
    ours.groupAmount = 2.0f;
    ours.money = 2.0f;
    return ours;
}

enum class At
{
    Stock,  // the case's base value (stock rate 1)
    Ours,   // its configured value (the main configuration file)
    Both    // the case states it for both
};

struct Value
{
    char const* name;  // the case's check name
    char const* key;   // the module value it reads
    At at;
    double expected;  // copied from the case
};

struct Case
{
    char const* id;  // craft_processing.<id>
    std::vector<Value> values;
    std::function<std::map<std::string, double>(Recipes::ServerRates const&)> compute;  // key -> module value
};

inline bool Same(double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b)); }

// Every value met at its rates, and the control: a case with configured values computed at stock rates misses one
template <class Check>
void Run(std::vector<Case> const& cases, Check const& check)
{
    Recipes::ServerRates const stock;
    Recipes::ServerRates const ours = Ours();
    for (Case const& one : cases)
    {
        std::map<std::string, double> const atStock = one.compute(stock);
        std::map<std::string, double> const atOurs = one.compute(ours);
        bool configured = false;
        bool caught = false;
        for (Value const& value : one.values)
        {
            auto const read = [&value](std::map<std::string, double> const& values)
            {
                auto const found = values.find(value.key);
                return found == values.end() ? std::nan("") : found->second;
            };
            std::string const what = std::string(one.id) + ": " + value.name;
            if (value.at != At::Ours)
                check(Same(read(atStock), value.expected), what + " at stock rates is " +
                                                               std::to_string(value.expected) + " (got " +
                                                               std::to_string(read(atStock)) + ")");
            if (value.at != At::Stock)
            {
                check(Same(read(atOurs), value.expected), what + " at our rates is " + std::to_string(value.expected) +
                                                              " (got " + std::to_string(read(atOurs)) + ")");
                if (value.at == At::Ours)
                {
                    configured = true;
                    caught = caught || !Same(read(atStock), value.expected);
                }
            }
        }
        if (configured)
            check(caught, std::string(one.id) + ": control: stock rates against the configured values are caught");
        std::cout << "economy case craft_processing." << one.id << ": " << one.values.size() << " values\n";
    }
}
}  // namespace EconomyCases

#endif
