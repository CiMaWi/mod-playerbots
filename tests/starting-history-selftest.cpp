/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the starting history rules (src/Ai/World/Rpg/StartingHistoryRules.h): flights only between
// discovered nodes, the history's flight points for fixed levels and races of both factions, the inn choice, the bag
// table by level and the bag slot steps that replace only factory bags.
#include <array>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "StartingHistoryRules.h"

using StartingHistory::BagStep;
using StartingHistory::Bracket;
using StartingHistory::NodeInZone;

namespace
{
// Race start nodes as PlayerTaxi::InitTaxiNodesForLevel sets them: human 2 + Northrend 100, orc 23 + Northrend 99
std::vector<uint32_t> const HUMAN_START = {2, 100};
std::vector<uint32_t> const ORC_START = {23, 99};

// Alliance nodes: Elwynn-like zone 1-10, Westfall-like 10-20, Duskwood-like 18-30, Stranglethorn-like (neutral)
// 30-45, an unbracketed zone and a Northrend zone 68-80
std::vector<NodeInZone> const ALLIANCE_NODES = {
    {2, true, {1, 10}},   {4, true, {10, 20}}, {12, true, {18, 30}},
    {43, true, {30, 45}}, {50, false, {}},     {245, true, {68, 80}},
};
// Horde nodes: Durotar-like 1-10, Barrens-like 10-25, the same neutral Stranglethorn node, Northrend 68-80
std::vector<NodeInZone> const HORDE_NODES = {
    {23, true, {1, 10}},
    {25, true, {10, 25}},
    {43, true, {30, 45}},
    {257, true, {68, 80}},
};

int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& name)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << name << "\n";
}

void CheckFlights()
{
    std::set<uint32_t> const discovered = {2, 4, 12};
    auto const known = [&discovered](uint32_t node) { return discovered.count(node) > 0; };

    Check(StartingHistory::KnownNodes(std::vector<uint32_t>{4, 7, 12, 43}, known) == std::vector<uint32_t>{4, 12},
          "only discovered candidates stay");
    Check(StartingHistory::KnownNodes(std::vector<uint32_t>{7, 43}, known).empty(), "no discovered candidate");
    Check(StartingHistory::KnownNodes(std::vector<uint32_t>{}, known).empty(), "empty candidates");

    Check(StartingHistory::PathKnown(std::vector<uint32_t>{2, 4}, known), "direct flight to a known node");
    Check(StartingHistory::PathKnown(std::vector<uint32_t>{7, 4, 12}, known), "the start node is learned there");
    Check(!StartingHistory::PathKnown(std::vector<uint32_t>{2, 43}, known), "undiscovered destination refused");
    Check(!StartingHistory::PathKnown(std::vector<uint32_t>{2, 43, 12}, known), "undiscovered stop refused");
    Check(!StartingHistory::PathKnown(std::vector<uint32_t>{}, known), "empty path refused");

    std::set<uint32_t> const none;
    auto const knowsNothing = [&none](uint32_t node) { return none.count(node) > 0; };
    Check(!StartingHistory::PathKnown(std::vector<uint32_t>{2, 4}, knowsNothing), "a bot that knows nothing");
}

void CheckNodes()
{
    Check(StartingHistory::HistoryNodes(1, HUMAN_START, ALLIANCE_NODES).empty(), "level 1 human gets nothing");
    Check(StartingHistory::HistoryNodes(1, ORC_START, HORDE_NODES).empty(), "level 1 orc gets nothing");
    Check(StartingHistory::HistoryNodes(10, HUMAN_START, ALLIANCE_NODES) == std::vector<uint32_t>{2, 100},
          "level 10: no bracket ends below it, start nodes only");
    Check(StartingHistory::HistoryNodes(11, HUMAN_START, ALLIANCE_NODES) == std::vector<uint32_t>{2, 100},
          "level 11: Elwynn's node equals the start node");
    Check(StartingHistory::HistoryNodes(31, HUMAN_START, ALLIANCE_NODES) == std::vector<uint32_t>{2, 4, 12, 100},
          "level 31 human: zones ending below 31");
    Check(StartingHistory::HistoryNodes(46, HUMAN_START, ALLIANCE_NODES) == std::vector<uint32_t>{2, 4, 12, 43, 100},
          "level 46 human: the neutral zone too, never an unbracketed one");
    Check(StartingHistory::HistoryNodes(80, HUMAN_START, ALLIANCE_NODES) == std::vector<uint32_t>{2, 4, 12, 43, 100},
          "level 80: Northrend's bracket does not end below 80");
    Check(StartingHistory::HistoryNodes(26, ORC_START, HORDE_NODES) == std::vector<uint32_t>{23, 25, 99},
          "level 26 orc: Barrens ends at 25");
    Check(StartingHistory::HistoryNodes(55, ORC_START, HORDE_NODES) == std::vector<uint32_t>{23, 25, 43, 99},
          "level 55 orc (a new death knight): its faction's and the neutral zones");

    std::vector<uint32_t> const once = StartingHistory::HistoryNodes(46, HUMAN_START, ALLIANCE_NODES);
    std::vector<uint32_t> const twice = StartingHistory::HistoryNodes(46, once, ALLIANCE_NODES);
    Check(once == twice, "a second pass adds nothing");
}

void CheckInns()
{
    // Goldshire-like 1-10, Sentinel Hill-like 10-20, Darkshire-like 18-30, Booty Bay-like 30-45
    std::vector<Bracket> const inns = {{1, 10}, {10, 20}, {18, 30}, {30, 45}};

    Check(StartingHistory::FittingInns(5, inns) == std::vector<size_t>{0}, "level 5: the starting zone's inn");
    Check(StartingHistory::FittingInns(10, inns) == std::vector<size_t>{0, 1}, "level 10: both bracket edges");
    Check(StartingHistory::FittingInns(19, inns) == std::vector<size_t>{1, 2}, "level 19: overlapping zones");
    Check(StartingHistory::FittingInns(50, inns).empty(), "level 50: no fitting inn");
    Check(StartingHistory::FittingInns(19, {}).empty(), "no inns of the team");

    Check(!StartingHistory::BindFits(30, true, {1, 10}), "a level 30 bot bound at its start rebinds");
    Check(StartingHistory::BindFits(30, true, {18, 30}), "a fitting bind is kept");
    Check(!StartingHistory::BindFits(30, false, {}), "a bind outside every bracket rebinds");
}

void CheckBags()
{
    Check(StartingHistory::BagForLevel(1) == 0, "level 1: no bag");
    Check(StartingHistory::BagForLevel(9) == 0, "level 9: no bag");
    Check(StartingHistory::BagForLevel(10) == StartingHistory::ITEM_SMALL_BROWN_POUCH, "level 10: 6-slot pouch");
    Check(StartingHistory::BagForLevel(59) == StartingHistory::ITEM_SMALL_BROWN_POUCH, "level 59: 6-slot pouch");
    Check(StartingHistory::BagForLevel(60) == StartingHistory::ITEM_RUNECLOTH_BAG, "level 60: 14-slot bag");
    Check(StartingHistory::BagForLevel(74) == StartingHistory::ITEM_RUNECLOTH_BAG, "level 74: 14-slot bag");
    Check(StartingHistory::BagForLevel(75) == StartingHistory::ITEM_FROSTWEAVE_BAG, "level 75: 20-slot bag");
    Check(StartingHistory::BagForLevel(80) == StartingHistory::ITEM_FROSTWEAVE_BAG, "level 80: 20-slot bag");

    uint32_t const pouch = StartingHistory::ITEM_SMALL_BROWN_POUCH;
    uint32_t const factory = StartingHistory::ITEM_FACTORY_BAG;
    Check(StartingHistory::DecideBagSlot(0, pouch) == BagStep::Equip, "empty slot gets the level's bag");
    Check(StartingHistory::DecideBagSlot(0, 0) == BagStep::Keep, "empty slot below level 10 stays empty");
    Check(StartingHistory::DecideBagSlot(factory, pouch) == BagStep::Replace, "factory bag replaced");
    Check(StartingHistory::DecideBagSlot(factory, 0) == BagStep::Remove, "factory bag below level 10 removed");
    Check(StartingHistory::DecideBagSlot(StartingHistory::ITEM_RUNECLOTH_BAG, pouch) == BagStep::Keep,
          "a natural bag is kept");
    Check(StartingHistory::DecideBagSlot(pouch, pouch) == BagStep::Keep, "a second pass keeps the history bag");
}

void CheckFactory()
{
    Check(StartingHistory::FactoryApplies(true, 55, 0), "a new death knight gets its history");
    Check(!StartingHistory::FactoryApplies(true, 1, 0), "a new level 1 bot gets none");
    Check(!StartingHistory::FactoryApplies(true, 30, StartingHistory::NEW_CHARACTER_PLAYED_SECONDS),
          "an existing bot waits for the console pass");
    Check(!StartingHistory::FactoryApplies(false, 55, 0), "not a persistent random bot");
}

// The riding ranks and mount types by level, with the default thresholds (conf: 20, 40, 60, 70)
void CheckRiding()
{
    using StartingHistory::RIDING_HISTORY_SPELLS;
    using StartingHistory::RidingTier;
    std::array<uint32_t, 4> const thresholds = {20, 40, 60, 70};
    Check(RidingTier(19, thresholds) == 0, "riding: none at 19");
    Check(RidingTier(20, thresholds) == 1 && RIDING_HISTORY_SPELLS[0] == 33388,
          "riding: apprentice and the slow mount at 20");
    Check(RidingTier(39, thresholds) == 1, "riding: still apprentice at 39");
    Check(RidingTier(40, thresholds) == 2 && RIDING_HISTORY_SPELLS[1] == 33391,
          "riding: journeyman and the fast mount at 40");
    Check(RidingTier(60, thresholds) == 3 && RIDING_HISTORY_SPELLS[2] == 34090, "riding: expert and a flyer at 60");
    Check(RidingTier(70, thresholds) == 4 && RIDING_HISTORY_SPELLS[3] == 34091,
          "riding: artisan and the fast flyer at 70");
    Check(RidingTier(80, thresholds) == 4, "riding: nothing above artisan at 80");
    Check(RidingTier(50, {20, 60, 40, 70}) == 1, "riding: a threshold below an unmet earlier one gives nothing");
}

void CheckWipe()
{
    uint32_t const played = StartingHistory::NEW_CHARACTER_PLAYED_SECONDS;
    Check(!StartingHistory::RandomizeWipes(false, true, played), "a persistent bot keeps its bags and gold");
    Check(!StartingHistory::RandomizeWipes(false, true, played * 100), "also after a long life");
    Check(StartingHistory::RandomizeWipes(false, true, 0), "creation gives the fresh kit");
    Check(StartingHistory::RandomizeWipes(false, true, played - 1), "the first hour still counts as creation");
    Check(StartingHistory::RandomizeWipes(true, true, played), "a full init (operator command) clears");
    Check(StartingHistory::RandomizeWipes(false, false, played), "other setups keep the old cycle");
}
}  // namespace

void CheckCleanup()
{
    using StartingHistory::CleanupDeletesMisc;
    Check(!CleanupDeletesMisc(15, 0, StartingHistory::ITEM_HEARTHSTONE), "cleanup keeps the hearthstone (misc, junk)");
    Check(!CleanupDeletesMisc(15, 1, StartingHistory::ITEM_HEARTHSTONE), "cleanup keeps the hearthstone by id");
    Check(!CleanupDeletesMisc(15, 0, 12345), "cleanup keeps other junk");
    Check(CleanupDeletesMisc(15, 1, 12345), "cleanup deletes misc reagents");
    Check(CleanupDeletesMisc(5, 0, 12345), "cleanup deletes reagents");
    Check(!CleanupDeletesMisc(0, 0, 12345), "cleanup leaves other classes to its other rules");
}

int main()
{
    CheckCleanup();
    CheckFlights();
    CheckNodes();
    CheckInns();
    CheckBags();
    CheckFactory();
    CheckWipe();
    CheckRiding();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
