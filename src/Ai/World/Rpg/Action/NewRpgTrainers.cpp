/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "CensusMgr.h"
#include "Creature.h"
#include "DBCStores.h"
#include "ErrandRules.h"
#include "NewRpgErrandInternal.h"
#include "NewRpgInternal.h"
#include "ObjectMgr.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Trainer.h"
#include "TravelMgr.h"
#include "UpkeepIndex.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "WorldLog.h"

using namespace NewRpgInternal;

namespace NewRpgInternal
{
// The class trainer visit of an errand stop (upkeep §3.7): the class spells up to the bot's level that this trainer
// teaches the bot, in level order within the training need's funded share (recomputed here, so repair and stock keep
// theirs), each taught and paid through the core's trainer path at the reputation price. The core's own test
// (CanTeachSpell) runs per spell at the teach, so a second rank whose first this visit teaches is taught after it; each
// spell is taught at most once. One TrainerVisit event and one upkeep row per visit
void TrainAtTrainer(PlayerbotAI* botAI, Creature* creature)
{
    Player* bot = botAI->GetBot();
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    Trainer::Trainer* trainer = sObjectMgr->GetTrainer(creature->GetEntry());
    std::shared_ptr<UpkeepNeeds::VisitNeeds const> const visit =
        index && trainer && trainer->IsTrainerValidForPlayer(bot) ? ErrandNeeds(botAI, index) : nullptr;
    Planner::TrainerList const* list = visit ? index->ClassList(visit->snapshot.cls) : nullptr;
    UpkeepNeeds::Need need;
    if (list && UpkeepNeeds::BitsAlign(*index, visit->snapshot))
        for (UpkeepNeeds::Need const& each : visit->needs.needs)
            if (each.kind == UpkeepKind::TrainerSpell)
                need = each;

    uint32 const moneyBefore = bot->GetMoney();
    uint32 learned = 0;
    UpkeepNeeds::TrainingBuy buy;
    if (need.units)
    {
        Planner::Snapshot const& snapshot = visit->snapshot;
        std::vector<std::size_t> const learnable =
            UpkeepNeeds::Learnable(*list, index->RidingList(snapshot.race), snapshot, snapshot.knownClassSpells,
                                   snapshot.canLearnClassSpells, snapshot.level);
        std::vector<std::size_t> taught;
        for (std::size_t const position : learnable)
        {
            Trainer::Spell const* spell = trainer->GetSpell(list->spells[position].spellId);
            if (spell && PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer, spell))
                taught.push_back(position);
        }

        float const discount = bot->GetReputationPriceDiscount(creature);
        buy = UpkeepNeeds::TrainingPurchase(*list, taught, snapshot.level, need.funded, discount);
        std::vector<std::size_t> waiting = buy.positions;
        for (bool progress = true; progress && !waiting.empty();)
        {
            progress = false;
            for (auto it = waiting.begin(); it != waiting.end();)
            {
                Trainer::Spell const* spell = trainer->GetSpell(list->spells[*it].spellId);
                if (spell && !trainer->CanTeachSpell(bot, spell))
                {
                    ++it;
                    continue;
                }

                uint32 const paid = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0;
                if (spell && paid <= need.funded &&
                    UpkeepNeeds::TrainerPrice(spell->MoneyCost, discount) <= need.funded - paid)
                {
                    trainer->TeachSpell(creature, bot, spell->SpellId);
                    if (!trainer->CanTeachSpell(bot, spell))
                        ++learned;
                    progress = true;
                }
                it = waiting.erase(it);
            }
        }
    }

    uint32 const paid = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0;
    EventLog::Emit(bot, Upkeep::EVENT_TRAINER_VISIT, "class", creature->GetEntry(), int64(paid));
    if (learned)
    {
        char const* const reason = need.funded < need.copper ? "short" : (need.due ? "due" : "joined");
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "train", reason, static_cast<float>(paid),
                           {{"spells", static_cast<float>(learned)},
                            {"left", static_cast<float>(need.units > learned ? need.units - learned : 0)},
                            {"funded", static_cast<float>(need.funded)}},
                           creature->GetEntry());
        return;
    }

    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip",
                       need.units && buy.positions.empty() && need.funded < need.copper ? "train_short" : "train_none",
                       static_cast<float>(need.copper),
                       {{"spells", static_cast<float>(need.units)},
                        {"funded", static_cast<float>(need.funded)},
                        {"gold", static_cast<float>(bot->GetMoney())}},
                       creature->GetEntry());
}

// The riding trainer visit of an errand stop (upkeep §3.8, WU14): the rank of the riding need (one per trip, the
// lowest), taught and paid through the core's trainer path at the reputation price when the need's funded share pays
// the rank and its mount (the purchase decided both; the mount follows at the mount vendor). The core's own test
// (CanTeachSpell) runs before the teach and reads it back; a visit teaches at most once. One TrainerVisit event per
// visit; a RidingBought event and an upkeep row for the rank
void RideAtTrainer(PlayerbotAI* botAI, Creature* creature)
{
    Player* bot = botAI->GetBot();
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    Trainer::Trainer* trainer = sObjectMgr->GetTrainer(creature->GetEntry());
    UpkeepNeeds::Need need;
    UpkeepNeeds::RankValue value;
    if (index && trainer && trainer->GetTrainerType() == Trainer::Type::Mount &&
        trainer->IsTrainerValidForPlayer(bot) && BuysAtVendors(botAI))
    {
        std::shared_ptr<UpkeepNeeds::VisitNeeds const> const visit = ErrandNeeds(botAI, index);
        UpkeepNeeds::NeedList const& needs = visit->needs;
        for (UpkeepNeeds::Need const& each : needs.needs)
            if (UpkeepNeeds::RidingErrandDue(each) && each.trainer)
            {
                need = each;
                break;
            }
        for (UpkeepNeeds::RankValue const& each : needs.ranks)
            if (need.item && each.bought && each.rank.spellId == need.item)
                value = each;
    }

    Trainer::Spell const* spell = need.item && value.bought ? trainer->GetSpell(need.item) : nullptr;
    uint32 const moneyBefore = bot->GetMoney();
    char const* refusal = "ride_none";  // this trainer teaches no rank the bot buys
    if (spell)
    {
        uint64 const price = UpkeepNeeds::TrainerPrice(spell->MoneyCost, bot->GetReputationPriceDiscount(creature));
        if (!trainer->CanTeachSpell(bot, spell))
            refusal = "ride_refused";
        else if (price + value.mount > need.funded)
            refusal = "ride_short";
        else
        {
            trainer->TeachSpell(creature, bot, spell->SpellId);
            refusal = trainer->CanTeachSpell(bot, spell) ? "ride_refused" : nullptr;
        }
    }

    uint32 const paid = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0;
    EventLog::Emit(bot, Upkeep::EVENT_TRAINER_VISIT, "riding", creature->GetEntry(), int64(paid));
    if (!refusal)
    {
        EventLog::Emit(bot, Upkeep::EVENT_RIDING_BOUGHT, "rank", spell->SpellId, int64(paid));
        WorldLog::Decision(
            bot, WorldLogFormat::DecisionKind::Upkeep, "buy",
            static_cast<float>(value.Price()) <= value.worth ? "worth" : "want", static_cast<float>(paid),
            {{"rank", static_cast<float>(value.rank.skill)}, {"funded", static_cast<float>(need.funded)}},
            spell->SpellId);
        return;
    }

    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", refusal, static_cast<float>(need.copper),
                       {{"funded", static_cast<float>(need.funded)}, {"gold", static_cast<float>(bot->GetMoney())}},
                       creature->GetEntry());
}

// The weapon master visit of an errand stop (upkeep §3.7, WU15a): each weapon skill need (recomputed here) this master
// teaches, taught and paid through the core's trainer path at the reputation price when the need's funded share pays
// it; the core's own test (CanTeachSpell) runs before the teach and reads it back. The skill then rises by use, as the
// core does. One TrainerVisit event and one upkeep row per visit
void LearnAtWeaponMaster(PlayerbotAI* botAI, Creature* creature)
{
    Player* bot = botAI->GetBot();
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    Trainer::Trainer* trainer = sObjectMgr->GetTrainer(creature->GetEntry());
    uint32 const moneyBefore = bot->GetMoney();
    uint32 wanted = 0;
    uint32 learned = 0;
    bool refused = false;
    uint64 copper = 0;
    uint64 funded = 0;
    if (index && trainer && trainer->GetTrainerType() == Trainer::Type::Tradeskill &&
        trainer->IsTrainerValidForPlayer(bot) && BuysAtVendors(botAI))
    {
        std::shared_ptr<UpkeepNeeds::VisitNeeds const> const visit = ErrandNeeds(botAI, index);
        UpkeepNeeds::NeedList const& needs = visit->needs;
        float const discount = bot->GetReputationPriceDiscount(creature);
        for (UpkeepNeeds::Need const& need : needs.needs)
        {
            Trainer::Spell const* spell = need.kind == UpkeepKind::WeaponSkill ? trainer->GetSpell(need.item) : nullptr;
            if (!spell)
                continue;  // not a weapon need, or another master teaches it
            ++wanted;
            copper += need.copper;
            funded += need.funded;
            if (UpkeepNeeds::TrainerPrice(spell->MoneyCost, discount) > need.funded)
                continue;
            if (!trainer->CanTeachSpell(bot, spell))
            {
                refused = true;
                continue;
            }
            trainer->TeachSpell(creature, bot, spell->SpellId);
            if (trainer->CanTeachSpell(bot, spell))
                refused = true;
            else
                ++learned;
        }
    }

    uint32 const paid = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0;
    EventLog::Emit(bot, Upkeep::EVENT_TRAINER_VISIT, "weapon", creature->GetEntry(), int64(paid));
    WorldLogFormat::Alternatives const weighed = {{"skills", static_cast<float>(learned)},
                                                  {"left", static_cast<float>(wanted - learned)},
                                                  {"funded", static_cast<float>(funded)},
                                                  {"gold", static_cast<float>(bot->GetMoney())}};
    if (learned)
    {
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "learn", funded < copper ? "short" : "due",
                           static_cast<float>(paid), weighed, creature->GetEntry());
        return;
    }

    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip",
                       !wanted ? "weapon_none" : (refused ? "weapon_refused" : "weapon_short"),
                       static_cast<float>(copper), weighed, creature->GetEntry());
}

// The spawns on the bot's map of the trainer an upkeep need names (the index's entries of its key) that trade with
// the bot, the nearest TRAINER_STOPS within reach
std::vector<Planner::Service const*> TrainerSpawns(Player* bot, Planner::PlannerIndexes const& indexes,
                                                   Planner::UpkeepIndex const& index, uint32 key, float reachYards)
{
    constexpr std::size_t TRAINER_STOPS = 3;
    std::vector<Planner::Service const*> result;
    auto const trainer = std::find_if(index.trainers.begin(), index.trainers.end(),
                                      [key](Planner::UpkeepTrainer const& known) { return known.key == key; });
    if (!key || trainer == index.trainers.end())
        return result;

    for (Planner::Nearby const& found :
         indexes.NearestAll(Planner::ServiceKind::Trainer, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                            bot->GetPositionZ(), reachYards))
    {
        Planner::Service const& spawn = indexes.services[found.index];
        // The vendors' interact test (NewRpgStartErrandsAction::Execute)
        if (!std::binary_search(trainer->entries.begin(), trainer->entries.end(), spawn.entry) ||
            bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(spawn.factionTemplate), bot) <= REP_UNFRIENDLY)
            continue;
        result.push_back(&spawn);
        if (result.size() == TRAINER_STOPS)
            break;
    }
    return result;
}

WorldTravel::ErrandPlace ErrandPlaceOf(Planner::Service const& spawn)
{
    return {spawn.mapId, spawn.x, spawn.y, spawn.z};
}

// The trainer spawns as the trip's stops with that service
void AddTrainerStops(std::vector<Planner::Service const*> const& spawns, uint32 service,
                     std::vector<WorldTravel::ServiceStop>& stops)
{
    for (Planner::Service const* spawn : spawns)
        stops.push_back({spawn->entry, ErrandPlaceOf(*spawn), service});
}
}  // namespace NewRpgInternal
