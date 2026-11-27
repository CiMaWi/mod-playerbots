/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NEEDGOALBRIDGE_H
#define _PLAYERBOT_NEEDGOALBRIDGE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "GoalModel.h"
#include "UpkeepNeedRules.h"

// Upkeep's needs list as the goal model reads it (upkeep plan WU16, §3.10): the reserve feeds WT14's need factor and
// WT2's bag rule, the shortfall names what the bot is short for. One list of needs (WU.4): money-making stays a goal,
// the shortfall only raises what copper is worth. Pure, std only: any thread
namespace Planner
{
namespace NeedBridge
{
// NR14 (NR.14): a need worth a trip of its own that the gold pays, by upkeep's rule (Goals::OwnStop)
struct OwnNeed
{
    bool due{false};
    std::uint32_t target{0};
    Place place;
};

struct NeedInputs
{
    std::uint64_t reserveCopper{0};  // unscaled: the goal model scales it by thrift as the needs rules do
    std::uint64_t shortfallCopper{0};
    std::string shortKind;          // the largest unpaid part's kind word (Upkeep::KindWord): NeedShort's src
    std::string shortText;          // "12g 0s 0c short for journeyman riding"
    bool trainingUnpayable{false};  // a due trainer spell whose cheapest waiting rank the funded gold does not pay
    OwnNeed training;               // TrainingErrandDue
    OwnNeed riding;                 // RidingErrandDue
    OwnNeed pet;                    // PetErrandDue
};

struct ReservePart
{
    std::uint64_t copper{0};  // unscaled
    UpkeepKind kind{UpkeepKind::Gold};
    std::string name;
};

// The reserve's parts in the fixed order the gold pays them (§3.3)
inline std::vector<ReservePart> PartsOf(UpkeepNeeds::NeedList const& list)
{
    using UpkeepNeeds::NeedOrder;
    std::vector<ReservePart> parts;
    if (list.reserveRepair)
        parts.push_back({list.reserveRepair, UpkeepKind::Repair, "repair"});
    for (UpkeepNeeds::Need const& need : list.needs)
        if (need.copper && need.order != NeedOrder::Repair && need.order != NeedOrder::Training &&
            need.order != NeedOrder::Riding && need.order != NeedOrder::Pet && need.order != NeedOrder::Use &&
            need.order != NeedOrder::Profession && need.order != NeedOrder::Market)
            parts.push_back({need.copper, need.kind, UpkeepNeeds::OrderWord(need.order)});
    if (list.reserveTraining)
        parts.push_back({list.reserveTraining, UpkeepKind::TrainerSpell, "training"});
    if (list.reserveRiding)
    {
        std::string name = "riding";
        for (UpkeepNeeds::RankValue const& rank : list.ranks)
            if (rank.reserved && rank.rank.name)
            {
                name = rank.rank.name;
                break;
            }
        parts.push_back({list.reserveRiding, UpkeepKind::Riding, name});
    }
    if (list.reserveProfession)
        parts.push_back({list.reserveProfession, UpkeepKind::Profession, "professions"});
    return parts;
}

inline NeedInputs InputsOf(UpkeepNeeds::NeedList const& list)
{
    NeedInputs inputs;
    inputs.reserveCopper =
        list.reserveRepair + list.reserveStock + list.reserveTraining + list.reserveRiding + list.reserveProfession;
    inputs.shortfallCopper = list.shortfall;
    // The questing test's money goal (questing WQ37): a due training the gold cannot pay (TrainingErrandDue's
    // opposite), whether or not the reserve is short
    for (UpkeepNeeds::Need const& need : list.needs)
    {
        if (need.kind == UpkeepKind::TrainerSpell && need.due && need.cheapest > 0 && need.funded < need.cheapest)
            inputs.trainingUnpayable = true;
        // The stops of their own (NR14): upkeep's rules decide, the goal model offers the trip
        if (need.kind == UpkeepKind::TrainerSpell && UpkeepNeeds::TrainingErrandDue(need) && !inputs.training.due)
            inputs.training = {true, need.trainer, {}};
        else if (UpkeepNeeds::RidingErrandDue(need) && !inputs.riding.due)
            inputs.riding = {true, need.trainer ? need.trainer : need.vendor, {}};
        else if (UpkeepNeeds::PetErrandDue(need) && !inputs.pet.due)
            inputs.pet = {true, need.beast, need.beastAt};
    }
    if (!list.shortfall)
        return inputs;

    // The gold pays the parts in order; the part it leaves most unpaid names the shortfall
    std::uint64_t left = list.money;
    std::uint64_t most = 0;
    ReservePart const* largest = nullptr;
    std::vector<ReservePart> const parts = PartsOf(list);
    for (ReservePart const& part : parts)
    {
        std::uint64_t const scaled = UpkeepNeeds::Scaled(list, part.copper);
        std::uint64_t const paid = std::min(scaled, left);
        left -= paid;
        if (scaled - paid > most)
        {
            most = scaled - paid;
            largest = &part;
        }
    }
    inputs.shortKind = largest ? Upkeep::KindWord(largest->kind) : Upkeep::KindWord(UpkeepKind::Gold);
    inputs.shortText = UpkeepNeeds::Money(list.shortfall) + " short for " + (largest ? largest->name : "the reserve");
    return inputs;
}

inline void Fill(Goals::Situation& situation, NeedInputs const& inputs)
{
    situation.reserveCopper = inputs.reserveCopper;
    situation.shortfallCopper = inputs.shortfallCopper;
    situation.shortKind = inputs.shortKind;
    situation.shortText = inputs.shortText;
    situation.trainingUnpayable = inputs.trainingUnpayable;
    // The walk and its safety are the map thread's (OwnStop)
    auto const own = [](Goals::OwnStop& stop, OwnNeed const& need)
    {
        stop.due = need.due;
        stop.target = need.target;
        stop.place = need.place;
    };
    own(situation.training, inputs.training);
    own(situation.riding, inputs.riding);
    own(situation.pet, inputs.pet);
}

// The walk to each due stop of its own (after Fill), to the stop the errand start plans for that need
// (NewRpgErrandStart): a class trainer of the bot's class with the need's key or one that teaches its highest waiting
// spell; a riding trainer that teaches the rank, else a vendor in reach that sells the rank's mount; the beast's spawn
// within the errand walk. From the map thread's stops seen (nearest first), a sick bot's nearest safe one before the
// others; none in reach leaves the walk unknown (-1), and no trip is offered
inline void PlaceOwnStops(Goals::Situation& situation, UpkeepNeeds::NeedList const& list, UpkeepIndex const& index,
                          std::uint8_t race, std::uint8_t cls, Place const& position, float runSpeed)
{
    auto const place = [](Goals::OwnStop& stop, std::vector<Goals::StopSeen> const& seen, auto const& serves)
    {
        Goals::StopSeen const* best = nullptr;
        for (Goals::StopSeen const& candidate : seen)
            if ((!best || (candidate.safe && !best->safe)) && serves(candidate.entry))
                best = &candidate;
        if (best)
        {
            stop.walkMinutes = best->walkMinutes;
            stop.safe = best->safe;
        }
    };
    auto const taughtBy = [&index](std::uint32_t entry, auto const& teaches)
    {
        return std::any_of(
            index.trainers.begin(), index.trainers.end(), [entry, &teaches](UpkeepTrainer const& trainer)
            { return teaches(trainer) && std::binary_search(trainer.entries.begin(), trainer.entries.end(), entry); });
    };
    UpkeepNeeds::Need const* training = nullptr;
    UpkeepNeeds::Need const* riding = nullptr;
    UpkeepNeeds::Need const* pet = nullptr;
    for (UpkeepNeeds::Need const& need : list.needs)
        if (need.kind == UpkeepKind::TrainerSpell && UpkeepNeeds::TrainingErrandDue(need) && !training)
            training = &need;
        else if (UpkeepNeeds::RidingErrandDue(need) && !riding)
            riding = &need;
        else if (UpkeepNeeds::PetErrandDue(need) && !pet)
            pet = &need;
    if (training && situation.training.due)
        place(situation.training, situation.trainersSeen,
              [&](std::uint32_t entry)
              {
                  return taughtBy(entry,
                                  [&](UpkeepTrainer const& trainer)
                                  {
                                      return trainer.kind == TrainerKind::Class && trainer.requirement == cls &&
                                             (trainer.key == training->trainer ||
                                              UpkeepNeeds::Teaches(index, trainer, training->lastSpell));
                                  });
              });
    if (riding && situation.riding.due && riding->trainer)
        place(situation.riding, situation.trainersSeen,
              [&](std::uint32_t entry)
              {
                  return taughtBy(entry,
                                  [&](UpkeepTrainer const& trainer)
                                  {
                                      return trainer.kind == TrainerKind::Mount &&
                                             UpkeepNeeds::Teaches(index, trainer, riding->item);
                                  });
              });
    else if (riding && situation.riding.due)
        if (UpkeepNeeds::RankValue const* const rank = UpkeepNeeds::BoughtRank(list, riding->item))
            place(situation.riding, situation.mountVendorsSeen,
                  [&](std::uint32_t entry) { return UpkeepNeeds::MountSoldBy(index, race, cls, *rank, entry) != 0; });
    if (pet && situation.pet.due && pet->beastAt.map == position.map)
    {
        float const dx = pet->beastAt.x - position.x;
        float const dy = pet->beastAt.y - position.y;
        float const dz = pet->beastAt.z - position.z;
        float const minutes = WorldTravel::WalkMinutes(std::sqrt(dx * dx + dy * dy + dz * dz), runSpeed);
        if (minutes <= WorldTravel::ERRAND_MAX_WALK_MINUTES)
            situation.pet.walkMinutes = minutes;
    }
}
}  // namespace NeedBridge
}  // namespace Planner

#endif
