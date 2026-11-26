/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_NEEDGOALBRIDGE_H
#define _PLAYERBOT_NEEDGOALBRIDGE_H

#include <algorithm>
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
struct NeedInputs
{
    std::uint64_t reserveCopper{0};  // unscaled: the goal model scales it by thrift as the needs rules do
    std::uint64_t shortfallCopper{0};
    std::string shortKind;          // the largest unpaid part's kind word (Upkeep::KindWord): NeedShort's src
    std::string shortText;          // "12g 0s 0c short for journeyman riding"
    bool trainingUnpayable{false};  // a due trainer spell whose cheapest waiting rank the funded gold does not pay
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
            need.order != NeedOrder::Riding && need.order != NeedOrder::Profession && need.order != NeedOrder::Market)
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
        if (need.kind == UpkeepKind::TrainerSpell && need.due && need.cheapest > 0 && need.funded < need.cheapest)
            inputs.trainingUnpayable = true;
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
}
}  // namespace NeedBridge
}  // namespace Planner

#endif
