/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "CensusMgr.h"
#include "Creature.h"
#include "ErrandRules.h"
#include "NewRpgErrandInternal.h"
#include "NewRpgInternal.h"
#include "ObjectMgr.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "ProfessionConfig.h"
#include "ProfessionVisit.h"
#include "Trainer.h"
#include "UpkeepIndex.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "WorldLog.h"

// The profession errand of the RPG action layer (professions WR4b; split from NewRpgTrainers.cpp and
// NewRpgErrandStart.cpp, WR5c): the trip's profession need and its teachers, the trainer visit and its test command,
// and the trainer part of an errand stop (from NewRpgErrandVisit.cpp at landing WRL1, CS13's line ceiling)
using namespace NewRpgInternal;

namespace NewRpgInternal
{
// The profession trainer visit of an errand stop (professions §3.3, WR4b): each profession need (recomputed here) whose
// spell this trainer teaches, taught and paid through the core's trainer path at the reputation price when the need's
// funded share pays it. Every spell passes MayLearn (WR.11) and the factory's test of what a bot may learn from a
// trainer; the core's own CanTeachSpell runs before the teach and reads it back. One ProfessionVisit event (what the
// stop paid, in copper) and one profession decision row per need, its value in copper
void LearnAtProfessionTrainer(PlayerbotAI* botAI, Creature* creature)
{
    using Professions::Choice;
    using Professions::Reason;
    Player* bot = botAI->GetBot();
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    Trainer::Trainer* trainer = sObjectMgr->GetTrainer(creature->GetEntry());
    uint32 const moneyBefore = bot->GetMoney();
    uint32 wanted = 0;
    uint32 learns = 0;
    uint32 ranks = 0;
    if (index && trainer && trainer->GetTrainerType() == Trainer::Type::Tradeskill &&
        trainer->IsTrainerValidForPlayer(bot) && BuysAtVendors(botAI))
    {
        std::shared_ptr<UpkeepNeeds::VisitNeeds const> const visit = ErrandNeeds(botAI, index);
        float const discount = bot->GetReputationPriceDiscount(creature);
        for (UpkeepNeeds::Need const& need : visit->needs.needs)
        {
            bool const mine = need.kind == UpkeepKind::Profession && need.step != UpkeepNeeds::ProfessionStep::Tool;
            Trainer::Spell const* spell = mine ? trainer->GetSpell(need.item) : nullptr;
            if (!spell)
                continue;  // not a profession need, or another trainer teaches it
            ++wanted;
            Choice const choice = need.step == UpkeepNeeds::ProfessionStep::Rank ? Choice::Rank : Choice::Learn;
            uint32 const price = UpkeepNeeds::TrainerPrice(spell->MoneyCost, discount);
            WorldLogFormat::Alternatives const weighed = {{"price", static_cast<float>(price)},
                                                          {"funded", static_cast<float>(need.funded)},
                                                          {"gold", static_cast<float>(bot->GetMoney())}};
            Reason skip = Reason::GoldShort;
            if (!UpkeepNeeds::MayLearn(spell->SpellId) ||
                !PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer, spell))
                skip = Reason::Locked;
            else if (price > need.funded)
                skip = Reason::GoldShort;
            else if (!trainer->CanTeachSpell(bot, spell))
                skip = Reason::Access;
            else
            {
                uint32 const before = bot->GetMoney();
                trainer->TeachSpell(creature, bot, spell->SpellId);
                if (!trainer->CanTeachSpell(bot, spell))
                {
                    ++(choice == Choice::Rank ? ranks : learns);
                    uint32 const spent = before > bot->GetMoney() ? before - bot->GetMoney() : 0;
                    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession, Professions::ChoiceWord(choice),
                                       Professions::ReasonWord(Reason::Access), static_cast<float>(spent), weighed,
                                       spell->SpellId);
                    continue;
                }
                skip = Reason::Access;  // taught but the core still offers it: the teach did not take
            }
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession, Professions::ChoiceWord(Choice::Skip),
                               Professions::ReasonWord(skip), static_cast<float>(price), weighed, spell->SpellId);
        }
    }

    uint32 const paid = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0;
    EventLog::Emit(bot, EventLog::EventType::ProfessionVisit, learns ? "learn" : (ranks ? "rank" : "none"),
                   creature->GetEntry(), int64(paid));
    if (!wanted)
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession, Professions::ChoiceWord(Choice::Skip),
                           Professions::ReasonWord(Reason::NoSource), 0.0f, {}, creature->GetEntry());
}

// The trip's profession need: one learn or rank per trip, the first due one with a trainer, a funded one before an
// unfunded one (UpkeepNeeds::ProfessionErrandDue). A tool is a stock need, bought by the shop errand (WR5b)
UpkeepNeeds::Need const* ProfessionErrand(UpkeepNeeds::NeedList const& needs, bool buysAtVendors)
{
    UpkeepNeeds::Need const* profession = nullptr;
    for (UpkeepNeeds::Need const& need : needs.needs)
        if (buysAtVendors && need.kind == UpkeepKind::Profession && need.step != UpkeepNeeds::ProfessionStep::Tool &&
            need.due && need.trainer && need.copper > 0 &&
            (!profession || (!UpkeepNeeds::ProfessionErrandDue(*profession) && UpkeepNeeds::ProfessionErrandDue(need))))
            profession = &need;
    return profession;
}

// Every tradeskill trainer that teaches the profession spell as the trip's stops; a vendor stop within
// STOP_JOIN_YARDS of one that has no trainer yet takes it too
void AddProfessionTeachers(Player* bot, Planner::PlannerIndexes const& indexes, Planner::UpkeepIndex const& index,
                           uint32 spellId, float reachYards, std::vector<WorldTravel::ServiceStop>& stops)
{
    std::vector<WorldTravel::ServiceStop> teacherStops;
    for (Planner::UpkeepTrainer const& trainer : index.trainers)
        if (trainer.kind == Planner::TrainerKind::Tradeskill && UpkeepNeeds::Teaches(index, trainer, spellId))
            AddTrainerStops(TrainerSpawns(bot, indexes, index, trainer.key, reachYards),
                            WorldTravel::SERVICE_PROFESSION_TRAINER, teacherStops);
    WorldTravel::JoinTrainers(stops, teacherStops, WorldTravel::SERVICE_PROFESSION_TRAINER);
    stops.insert(stops.end(), teacherStops.begin(), teacherStops.end());
}

// The trainer part of an errand stop, read by the errand visit (NewRpgErrandVisit.cpp): the class trainer at the one
// stop that took the training errand (the first trainer stop of the trip), a weapon master or a profession trainer at a
// stop that took its errand, a riding trainer at each of its stops. False when the stop has no trainer part
bool TrainerPart(WorldTravel::TripStop const& tripStop, NewRpgInfo::ErrandStop& stop)
{
    auto const takes = [&tripStop](uint32 service, WorldTravel::ErrandKind kind)
    {
        return (tripStop.Services & service) &&
               std::any_of(tripStop.Errands.begin(), tripStop.Errands.end(),
                           [kind](WorldTravel::ErrandOffer const& errand) { return errand.Kind == kind; });
    };
    bool const classTrainer = takes(WorldTravel::SERVICE_TRAINER, WorldTravel::ErrandKind::Train);
    bool const weaponMaster = takes(WorldTravel::SERVICE_WEAPON_MASTER, WorldTravel::ErrandKind::Weapon);
    bool const professionTrainer = takes(WorldTravel::SERVICE_PROFESSION_TRAINER, WorldTravel::ErrandKind::Profession);
    if (!classTrainer && !weaponMaster && !professionTrainer &&
        !(tripStop.Services & WorldTravel::SERVICE_RIDING_TRAINER))
        return false;

    stop.trainerEntry = tripStop.Trainer ? tripStop.Trainer : tripStop.Id;
    stop.trainerProfession = !classTrainer && !weaponMaster && professionTrainer;
    stop.trainerRiding = !classTrainer && !weaponMaster && !professionTrainer;
    stop.trainerWeapon = !classTrainer && weaponMaster;
    return true;
}
}  // namespace NewRpgInternal

namespace ProfessionVisit
{
// `playerbots professions visit <bot>` (a GM test command): the profession stop's handler run now on the nearest
// tradeskill trainer within VISIT_REACH_YARDS of the bot, as the bank visit command does; the walk to a far trainer is
// the errand's job. World thread, between the map updates
bool Run(Player* bot, std::string& report)
{
    constexpr float VISIT_REACH_YARDS = 60.0f;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        report = "the bot is not a playerbot";
    else if (!botAI->KeepsPossessions())
        report = "the bot does not keep its possessions (not a persistent bot)";
    else if (!bot->IsAlive())
        report = "the bot is dead";
    else if (!Planner::IndexesReady() || !Planner::UpkeepIndexReady())
        report = "the planner's indexes are not ready";
    if (!report.empty())
        return false;

    std::shared_ptr<Planner::PlannerIndexes const> const indexes = Planner::Indexes();
    for (Planner::Nearby const& found :
         indexes->NearestAll(Planner::ServiceKind::Trainer, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                             bot->GetPositionZ(), VISIT_REACH_YARDS))
    {
        uint32 const entry = indexes->services[found.index].entry;
        Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry);
        if (!trainer || trainer->GetTrainerType() != Trainer::Type::Tradeskill)
            continue;
        Creature* creature = bot->FindNearestCreature(entry, VISIT_REACH_YARDS);
        if (!creature)
            continue;
        uint32 const before = bot->GetMoney();
        NewRpgInternal::LearnAtProfessionTrainer(botAI, creature);
        report = "trainer " + std::to_string(entry) + " " + std::to_string(static_cast<uint32>(found.distance)) +
                 " yd away, paid " + std::to_string(before > bot->GetMoney() ? before - bot->GetMoney() : 0) +
                 " copper";
        return true;
    }

    report = "no tradeskill trainer within " + std::to_string(static_cast<uint32>(VISIT_REACH_YARDS)) + " yd";
    return false;
}
}  // namespace ProfessionVisit
