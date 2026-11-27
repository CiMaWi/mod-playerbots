/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_SKILLPATHLIVE_H
#define _PLAYERBOT_SKILLPATHLIVE_H

#include <memory>
#include <string>

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

// Any thread: what one unit of an item is expected to fetch listed (Market::View::ProceedsOf at the item template's
// sell price), in copper; 0 without a market or a typical price, or when the vendor pays as much
std::uint32_t MarketProceeds(Market::View const& market, std::uint32_t item);

// Any thread, a pure function of its inputs: the needs list with the path's recipes and ranks (SkillPathBridge
// Compute), its own PerfMonitor row "profession path computed" and its parts' rows. Without a recipe index or a
// person: the needs without a path, and null
std::shared_ptr<SkillPath::Path const> NeedsWithPath(Snapshot const& snapshot, UpkeepIndex const& upkeep,
                                                     UpkeepNeeds::NeedRates const& rates,
                                                     SkillPath::PathInputs const& inputs, PathData const& data,
                                                     UpkeepNeeds::NeedList& needs);

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
