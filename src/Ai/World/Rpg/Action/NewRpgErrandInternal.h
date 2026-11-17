/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGERRANDINTERNAL_H
#define PLAYERBOTS_NEWRPGERRANDINTERNAL_H

#include <cstdint>
#include <memory>
#include <vector>

#include "Define.h"
#include "NewRpgInfo.h"

class Creature;
class Player;
class PlayerbotAI;

namespace WorldTravel
{
struct ErrandPlace;
struct ServiceStop;
}  // namespace WorldTravel

namespace Planner
{
class PlannerIndexes;
class UpkeepIndex;
struct Service;
}  // namespace Planner

// The errand, trainer and stable helpers of the RPG action layer (split from NewRpgInternal.h, CS13). Private to
// src/Ai/World/Rpg/: a file that calls one has `using namespace NewRpgInternal;`
namespace NewRpgInternal
{
// Errands and trainers: the errand trip's helpers, called by the errand start, the errand visit and the trainer visits
bool BuysAtVendors(PlayerbotAI* botAI);
// The errand trip's needs (WU27, the visit list): the trip's kept list while the state is the one it was computed on,
// else one computed now (counted, kept with the trip); map thread only
std::shared_ptr<UpkeepNeeds::VisitNeeds const> ErrandNeeds(PlayerbotAI* botAI,
                                                           std::shared_ptr<Planner::UpkeepIndex const> const& index);
WorldTravel::ErrandPlace ErrandPlaceOf(Planner::Service const& spawn);

// Trainers, called by the errand start (the trip's stops) and the errand visit (the trainer visits)
void TrainAtTrainer(PlayerbotAI* botAI, Creature* creature);
void RideAtTrainer(PlayerbotAI* botAI, Creature* creature);
void LearnAtWeaponMaster(PlayerbotAI* botAI, Creature* creature);
std::vector<Planner::Service const*> TrainerSpawns(Player* bot, Planner::PlannerIndexes const& indexes,
                                                   Planner::UpkeepIndex const& index, uint32 key, float reachYards);
void AddTrainerStops(std::vector<Planner::Service const*> const& spawns, uint32 service,
                     std::vector<WorldTravel::ServiceStop>& stops);

// Stable, called by the errand visit: the tame and the stable parts of an errand stop
enum class TameStep : uint8
{
    Busy,      // a cast or its channel runs: the stop holds
    Rest,      // the bot rests before a new try: other actions (food) may run
    Approach,  // the picked beast is out of reach or sight: walk to it
    Done       // tamed or given up: the stop's tame is over
};

TameStep TameAtBeast(PlayerbotAI* botAI, NewRpgInfo::ErrandStop& stop);

enum class StableStep : uint8
{
    Wait,
    Stored,
    Skipped
};

StableStep StableAtMaster(PlayerbotAI* botAI, Creature* master, NewRpgInfo::ErrandStop& stop, uint32& stored);

// The shopping list at the errand's vendor, called by the errand visit (NewRpgVendor.cpp defines it)
void ShopAtVendor(PlayerbotAI* botAI, Creature* vendor, bool repaired);
}  // namespace NewRpgInternal

#endif
