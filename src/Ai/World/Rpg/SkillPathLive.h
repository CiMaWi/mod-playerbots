/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SKILLPATHLIVE_H
#define _PLAYERBOT_SKILLPATHLIVE_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "GatherRateRules.h"
#include "MarketRules.h"
#include "SkillPathTypes.h"

class ChatHandler;
class Player;
class PlayerbotAI;

namespace Recipes
{
class RecipeIndex;
}

namespace UpkeepNeeds
{
struct NeedRates;
struct NeedList;
}  // namespace UpkeepNeeds

// The skill path live (professions plan WR11; the pure half is SkillPathBridge.h). The path is computed on the planner
// worker with each goals job, after the needs and before the plan (PlannerQuests.cpp GoalsCompute), rides the answer
// to the bot and is kept in its BotGoals until the next plan; the bot's thread reads only the kept copy.
namespace Planner
{
struct Snapshot;
class UpkeepIndex;
class PlannerIndexes;

// The published data a job's path reads: immutable values, kept alive by the job
struct PathData
{
    std::shared_ptr<Recipes::RecipeIndex const> recipes;
    std::shared_ptr<PlannerIndexes const> indexes;
    GatherRate::View gatherRates;  // the observed minutes per take (WPC13b); empty before the first publication
};

// What one goals job's path computation read, by value, and its result (professions WPC14a): the bot keeps it with
// the path, so the next computation counts which inputs changed and whether its result is the same. Counters only
struct PathRecord
{
    struct Level
    {
        std::uint32_t id{0};
        std::uint32_t value{0};
        std::uint32_t max{0};

        bool operator==(Level const&) const = default;
    };

    std::shared_ptr<SkillPath::Path const> path;  // the result
    std::uint32_t level{0};
    std::array<std::uint16_t, 2> mains{};  // the person's main professions
    std::vector<Level> skills;             // the professions: skill, value, max
    std::vector<Level> held;               // the materials held, by item: bags + bank units as value
    std::vector<std::uint32_t> knownRecipes;
    std::vector<std::uint32_t> toolsHeld;
    std::uint32_t money{0};
    std::uint64_t reserve{0};      // before the professions
    std::uint64_t spendable{0};    // the path bot's money: above the reserve
    std::array<float, 4> rates{};  // RatesOf: exchange, factor, minute value, caution weight
    bool shortOfGold{false};
    std::uint32_t gathererLevel{0};
    SkillPath::PathInputs inputs;
    std::weak_ptr<Recipes::RecipeIndex const> recipes;
    std::weak_ptr<PlannerIndexes const> indexes;
    std::shared_ptr<AuctionIndex::Snapshot const> market;
    GatherRate::View gatherRates;
};

// The worker, after a goals job's computation (WPC14a): the PerfMonitor rows of the inputs that changed since the
// bot's last computation (null: its first) and whether the result is the same; also the market's and the rate book's
// sizes
void CountPathChanges(PathRecord const* last, PathRecord const& now);

// Any thread: the published recipe and planner indexes (null before their builds)
PathData PathDataNow();

// Map thread: what a goals job copies for the path (item use's cached answer, mapped, and the upgrades the bot's
// professions make that it would wear, WR12); empty for a bot item use does not answer for
SkillPath::PathInputs PathInputsOf(PlayerbotAI* botAI);

// The bot's thread: the path the bot keeps (BotGoals::path), or null before its first plan; valid during the call
SkillPath::Path const* KeptPath(PlayerbotAI* botAI);

// Any thread: the market a decision away from the auctioneer sees (auction house WA7a, WA.4): the auction index's
// published snapshot (typical prices only, through its lock-free pointer), the shared house's fee row (7, WA.9), the
// poster's numbers and the item templates' stacks as the lot. Take one per computation, not one per item
Market::View MarketView();
// The same with a PerfMonitor row of its own per view, its microseconds (WPC14a: the views per bot hour by caller)
Market::View CountedMarketView(char const* row);

// Any thread: what one unit of an item is expected to fetch listed (Market::View::ProceedsOf at the item template's
// sell price), in copper; 0 without a market or a typical price, or when the vendor pays as much
std::uint32_t MarketProceeds(Market::View const& market, std::uint32_t item);

// Any thread, a pure function of its inputs: the needs list with the path's recipes and ranks (SkillPathBridge
// Compute), its own PerfMonitor row "profession path computed" and its parts' rows. Without a recipe index or a
// person: the needs without a path, and null. record: filled with what the computation read (WPC14a), outside its row
std::shared_ptr<SkillPath::Path const> NeedsWithPath(Snapshot const& snapshot, UpkeepIndex const& upkeep,
                                                     UpkeepNeeds::NeedRates const& rates,
                                                     SkillPath::PathInputs const& inputs, PathData const& data,
                                                     UpkeepNeeds::NeedList& needs, PathRecord* record = nullptr);

// Map thread: the needs rates with the path the bot keeps (BotNeedRates); unchanged while it keeps none
void AddKeptPath(PlayerbotAI* botAI, UpkeepNeeds::NeedRates& rates);

// The bot's thread, as it takes a plan's path: the second choice applied (a second gatherer stored as its person,
// BotProfessionMgr::ChooseSecond) and a `profession` decision row when it differs from the plan before's
void TakeSecondChoice(PlayerbotAI* botAI, Player* bot, SkillPath::Path const* before, SkillPath::Path const& path);

// World thread: `playerbots professions path <bot>`, the path computed now from a fresh snapshot (the stretches
// weighed, the one chosen, its next steps, the second choice) beside the path the bot keeps. `path <bot> stock` (a GM
// test fixture, WR14b): the chosen stretch's wants put into the bags, the path computed again and kept as the plan's
bool HandlePathCommand(ChatHandler* handler, std::string name, std::string const& verb);
}  // namespace Planner

#endif
