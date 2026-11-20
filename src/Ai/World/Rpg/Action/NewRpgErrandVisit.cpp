/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "BankMailState.h"
#include "BankVisit.h"
#include "CensusMgr.h"
#include "Creature.h"
#include "ErrandRules.h"
#include "InventorySlots.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "MailVisit.h"
#include "NewRpgAction.h"
#include "NewRpgErrandInternal.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "NewRpgTriggers.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Upkeep.h"
#include "UpkeepIndex.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "WorldLog.h"
#include "WorldTravelRules.h"

using namespace NewRpgInternal;

namespace
{
// The state the trip's needs list was computed on (UpkeepNeeds::VisitKey): O(1) reads of the bot and its measure
UpkeepNeeds::VisitKey VisitKeyOf(PlayerbotAI* botAI, Planner::UpkeepIndex const* index)
{
    Player* bot = botAI->GetBot();
    PetStable const* stable = bot->GetPetStable();
    UpkeepNeeds::VisitKey key;
    key.money = bot->GetMoney();
    key.level = bot->GetLevel();
    key.spells = static_cast<uint32>(bot->GetSpellMap().size());
    key.pet = stable && stable->CurrentPet ? stable->CurrentPet->PetNumber : 0;
    key.stableSlots = stable ? stable->MaxStabledPets : 0;
    UpkeepNeeds::KeyRates(key,
                          botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet());
    key.index = index;
    return key;
}

// The stock per upkeep kind now as the needs count it (one snapshot: the kept grade reads the level and the equipped
// ammo), for the use measure: the trip's kept snapshot while its state holds, else a snapshot alone (no needs)
UpkeepNeeds::StockLevels StockLevelsOf(PlayerbotAI* botAI)
{
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    auto const* errands = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data);
    UpkeepNeeds::VisitNeeds const* kept =
        errands && index ? UpkeepNeeds::VisitNeedsKept(errands->needs, VisitKeyOf(botAI, index.get())) : nullptr;
    if (kept)
        return UpkeepNeeds::LevelsOf(index.get(), kept->snapshot);
    return UpkeepNeeds::LevelsOf(index.get(), Planner::FillSnapshot(botAI->GetBot(), 0));
}

// A bank or mail errand at the stop: the visit's own action (relevance 13, any status) walks to the box and holds
// the bot until the visit ends with its row; the trip goes on after it. A box beyond the stop's reach or a refused
// start skips the errand with a row, so the trip never waits for it
bool StartStopVisit(PlayerbotAI* botAI, Player* bot, WorldTravel::ErrandKind kind)
{
    float const reach = WorldTravel::STOP_JOIN_YARDS + NewRpgErrandsAction::VENDOR_SEARCH_YARDS;
    char const* refusal = nullptr;
    char const* reason = "busy";
    bool started = false;
    if (kind == WorldTravel::ErrandKind::Mail)
    {
        std::optional<BankMail::MailboxSpot> const spot = BankMail::FindMailboxSpot(bot, refusal);
        if (!spot || spot->distance > reach)
            reason = "no_mailbox";
        else
            started = BankMail::BeginMailVisit(botAI, *spot);
    }
    else
    {
        std::optional<BankMail::BankerSpot> const spot = BankMail::FindBankerSpot(bot, refusal);
        if (!spot || spot->distance > reach)
            reason = "no_banker";
        else
            started = BankMail::BeginBankVisit(botAI, *spot);
    }
    if (!started)
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, WorldTravel::ErrandKindName(kind), reason, 0.0f);
    return started;
}
}  // namespace

namespace NewRpgInternal
{
// A bot that buys at vendors (the shopping list, a riding rank and its mount): a random bot in honest travel without a
// real player as master. The riding errand is taken only for such a bot, so a rank is never taught where the mount
// could not be bought (a known rank without its mount keeps the need open)
bool BuysAtVendors(PlayerbotAI* botAI) { return botAI->KeepsPossessions(); }

std::shared_ptr<UpkeepNeeds::VisitNeeds const> ErrandNeeds(PlayerbotAI* botAI,
                                                           std::shared_ptr<Planner::UpkeepIndex const> const& index)
{
    // Outside a trip (the start action, before its status) the list is the caller's alone
    auto* errands = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data);
    std::shared_ptr<UpkeepNeeds::VisitNeeds const> own;
    std::shared_ptr<UpkeepNeeds::VisitNeeds const>& cache = errands ? errands->needs : own;
    return UpkeepNeeds::VisitNeedsFor(cache, VisitKeyOf(botAI, index.get()),
                                      [botAI, &index](UpkeepNeeds::VisitNeeds& fresh)
                                      {
                                          fresh.index = index;
                                          fresh.snapshot = Planner::FillSnapshot(botAI->GetBot(), 0);
                                          fresh.needs = Planner::ComputeErrandNeeds(botAI, fresh.snapshot, *index);
                                      });
}

// The trainer part of an errand stop, read by the visit below: the class trainer at the one stop that took the training
// errand (the first trainer stop of the trip), a weapon master or a profession trainer at a stop that took its errand,
// a riding trainer at each of its stops. False when the stop has no trainer part
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

bool NewRpgErrandsAction::Execute(Event /*event*/)
{
    auto* data = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data);
    // A mailbox or banker visit of the stop runs under its own action; the trip goes on once it has ended
    if (!data || BankMail::VisitRuns(botAI->bankMail))
        return false;

    if (data->stop >= data->stops.size())
    {
        RpgStatus::End(botAI, StatusEnd::Outcome::Finished);
        return true;
    }

    NewRpgInfo::ErrandStop& stop = data->stops[data->stop];
    if (!data->arrived)
    {
        if (bot->GetExactDist(stop.pos) > VENDOR_SEARCH_YARDS)
        {
            // The walk uses stock and wears gear, which the needs list's key does not hold
            data->needs.reset();
            if (MoveFarTo(stop.pos))
                return true;
            return MoveRandomNear(10.0f);
        }
        data->arrived = true;
    }

    // WB8's order at a stop: the banker first when it withdraws (the items may be worn or sold), then the vendor and
    // the turn-ins, then a deposit alone and the mailbox
    if (stop.bankBefore)
    {
        stop.bankBefore = false;
        data->needs.reset();  // the withdrawal moves gear and items without gold, which the list's key does not hold
        if (StartStopVisit(botAI, bot, WorldTravel::ErrandKind::BankWithdraw))
        {
            data->visited = true;
            return true;
        }
    }

    // The vendor visit of the npc wander: sell by worth, buy (a bag by worth) and repair, all paid. A vendor that is
    // gone leaves the stop's other errands. Searched within the stop's reach, as the turn-ins: a banker visit before
    // it leaves the bot up to STOP_JOIN_YARDS from the stop (WB8b run 3: Dalaran's vendor 39 yd from the banker)
    if (stop.vendorEntry && !data->vendorDone)
    {
        if (data->vendor.IsEmpty())
        {
            Creature* found =
                bot->FindNearestCreature(stop.vendorEntry, WorldTravel::STOP_JOIN_YARDS + VENDOR_SEARCH_YARDS);
            if (!found)
            {
                data->vendorDone = true;
                return true;
            }
            data->vendor = found->GetGUID();
        }

        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data->vendor);
        Creature* vendor = object ? object->ToCreature() : nullptr;
        if (!vendor || !vendor->IsAlive())
        {
            data->vendorDone = true;
            return true;
        }

        if (!IsWithinInteractionDist(vendor))
        {
            if (MoveWorldObjectTo(data->vendor))
                return true;
            return MoveRandomNear(15.0f);
        }

        // The sale finds its vendor only in sight. In reach, the walk up to the vendor ends first, and one still out
        // of sight (Undercity's ramp above Jeremiah Payson, Dalaran's stairs: WB8b runs 5 and 8) gets a few steps
        // toward it at most; then the bot sells as it stands
        if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
            return false;

        constexpr uint8 MAX_SIGHT_STEPS = 3;
        if (data->sightSteps < MAX_SIGHT_STEPS && !bot->IsWithinLOSInMap(vendor))
        {
            ++data->sightSteps;
            if (MoveWorldObjectTo(data->vendor, 1.0f))
                return true;
        }

        // The vendor visit of the npc wander: the use measure before anything changes the bags, then sell by worth,
        // repair, a persistent bot's shopping list (upkeep §3.3: repair first, then the stock in its order) and the
        // generic buy (gear, a bag by worth; consumables and a hunter's ammo only for a bot granted them), all paid
        constexpr uint32 MIN_VENDOR_BALANCE = 1000;  // 10 silver
        bool const shopping = BuysAtVendors(botAI);
        WorldTravel::SlotMeasure& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
        if (shopping)
            UpkeepNeeds::SampleUse(measure, StockLevelsOf(botAI), bot->getClass(), bot->GetLevel(), getMSTime());
        botAI->DoSpecificAction("sell", Event("sell", "vendor"), true);
        // A bag bought earlier may lie inside the bag it should replace, where the swap always fails
        if (WorldTravel::MoveSpareBagToBackpack(bot, ItemUsageValue::SmallestBagSize(bot)))
            botAI->DoSpecificAction("equip upgrades packet action");

        // "can repair" wants the whole repair within the money kept for it; gear due for repair gets what the gold
        // pays
        bool repaired = false;
        if (vendor->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) &&
            (AI_VALUE(bool, "can repair") || WorldTravel::RepairErrandWanted(bot)))
        {
            botAI->DoSpecificAction("repair", Event("repair"), true);
            repaired = true;
        }

        if (shopping)
            ShopAtVendor(botAI, vendor, repaired);

        if (bot->GetMoney() >= MIN_VENDOR_BALANCE)
            botAI->DoSpecificAction("buy", Event("buy", "vendor"), true);

        if (shopping)
            UpkeepNeeds::NoteStock(measure, StockLevelsOf(botAI), getMSTime());
        data->vendorDone = true;
        data->visited = true;
        return true;
    }

    // The completed quests whose ender stands at the stop: the quest talk turns in every quest the ender takes and
    // accepts the fitting ones it offers (WT.6). An ender out of reach or a quest no longer complete is left out
    while (!stop.turnIns.empty())
    {
        NewRpgInfo::ErrandTurnIn const turnIn = stop.turnIns.back();
        if (bot->GetQuestStatus(turnIn.questId) != QUEST_STATUS_COMPLETE)
        {
            stop.turnIns.pop_back();
            continue;
        }

        float const searchYards = WorldTravel::STOP_JOIN_YARDS + VENDOR_SEARCH_YARDS;
        WorldObject* ender = nullptr;
        if (turnIn.enderIsObject)
            ender = bot->FindNearestGameObject(turnIn.ender, searchYards);
        else
            ender = bot->FindNearestCreature(turnIn.ender, searchYards);
        if (!ender)
        {
            stop.turnIns.pop_back();
            continue;
        }

        if (!IsWithinInteractionDist(ender))
        {
            if (MoveWorldObjectTo(ender->GetGUID()))
                return true;
            stop.turnIns.pop_back();
            continue;
        }

        if (bot->CanInteractWithQuestGiver(ender))
        {
            InteractWithNpcOrGameObjectForQuest(ender->GetGUID());
            data->visited = true;
        }
        stop.turnIns.pop_back();
        return true;
    }

    // The trainer part of the stop: a class or riding trainer stands at the stop or up to STOP_JOIN_YARDS from it (a
    // vendor stop that passes it): the bot walks into reach and learns. Either way the trip reached its trainer: not an
    // errand_abort
    if (stop.trainerEntry)
    {
        uint32 const entry = stop.trainerEntry;
        Creature* trainer = bot->FindNearestCreature(entry, WorldTravel::STOP_JOIN_YARDS + VENDOR_SEARCH_YARDS);
        // A walk that is still going on keeps the stop (MoveWorldObjectTo is false while it waits); only a path that
        // fails gives the trainer up
        if (trainer && !IsWithinInteractionDist(trainer) &&
            (MoveWorldObjectTo(trainer->GetGUID()) || IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL)))
            return true;
        if (trainer && IsWithinInteractionDist(trainer) && stop.trainerRiding)
            RideAtTrainer(botAI, trainer);
        else if (trainer && IsWithinInteractionDist(trainer) && stop.trainerWeapon)
            LearnAtWeaponMaster(botAI, trainer);
        else if (trainer && IsWithinInteractionDist(trainer) && stop.trainerProfession)
            LearnAtProfessionTrainer(botAI, trainer);
        else if (trainer && IsWithinInteractionDist(trainer))
            TrainAtTrainer(botAI, trainer);
        else
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "no_trainer", 0.0f, {}, entry);
        stop.trainerEntry = 0;
        data->visited = true;
        return true;
    }

    // The stable part of the stop (WU21c): the stable master stands at the stop; the bot walks into reach and stores
    // its unwanted pet. Once it is stored, the tame of the wanted beast follows as the trip's next stop
    if (stop.stableEntry)
    {
        Creature* master =
            bot->FindNearestCreature(stop.stableEntry, WorldTravel::STOP_JOIN_YARDS + VENDOR_SEARCH_YARDS);
        if (master && !IsWithinInteractionDist(master) &&
            (MoveWorldObjectTo(master->GetGUID()) || IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL)))
            return true;
        uint32 stored = 0;
        StableStep step = StableStep::Skipped;
        if (master && IsWithinInteractionDist(master))
            step = StableAtMaster(botAI, master, stop, stored);
        else
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "stable_master", 0.0f, {},
                               stop.stableEntry);
        if (step == StableStep::Wait)
            return true;
        NewRpgInfo::ErrandStop tame;
        bool const follows = step == StableStep::Stored && stop.stableBeast;
        if (follows)
        {
            tame.pos = stop.stableBeastAt;
            tame.tameEntry = stop.stableBeast;
            tame.tameStabled = stored;
        }
        stop.stableEntry = 0;
        data->visited = true;
        if (follows)
            data->stops.insert(data->stops.begin() + data->stop + 1, tame);  // stop is not read after this
        return true;
    }

    // The tame part of the stop: the stop is the beast's spawn (WU21b). A walk to the picked beast that fails gives
    // the tame up
    if (stop.tameEntry)
    {
        TameStep const step = TameAtBeast(botAI, stop);
        if (step == TameStep::Approach)
        {
            if (MoveWorldObjectTo(stop.tameBeast, UpkeepNeeds::TAME_CAST_YARDS / 2) ||
                IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
                return true;
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "tame_path",
                               static_cast<float>(stop.tameTries), {{"tries", static_cast<float>(stop.tameTries)}},
                               stop.tameEntry);
        }
        else if (step != TameStep::Done)
            return step == TameStep::Busy;
        stop.tameEntry = 0;
        data->visited = true;
        return true;
    }

    if (stop.bankAfter || stop.mail)
    {
        WorldTravel::ErrandKind const kind =
            stop.bankAfter ? WorldTravel::ErrandKind::BankDeposit : WorldTravel::ErrandKind::Mail;
        (stop.bankAfter ? stop.bankAfter : stop.mail) = false;
        if (StartStopVisit(botAI, bot, kind))
            data->visited = true;
        return true;
    }

    // The stop is done: on to the next
    ++data->stop;
    data->arrived = false;
    data->vendorDone = false;
    data->vendor = ObjectGuid();
    data->sightSteps = 0;
    data->needs.reset();  // the stop's turn-ins, deposit and mail moved items without gold: the next stop computes anew
    return true;
}
