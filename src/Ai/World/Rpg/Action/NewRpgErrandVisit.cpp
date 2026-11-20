/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <list>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "BankMailState.h"
#include "BankRulesInput.h"
#include "BankVisit.h"
#include "CellImpl.h"
#include "CensusMgr.h"
#include "Creature.h"
#include "DBCStores.h"
#include "ErrandRules.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "InventorySlots.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "Log.h"
#include "MailVisit.h"
#include "ModelIgnoreFlags.h"
#include "NewRpgAction.h"
#include "NewRpgErrandInternal.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "NewRpgTriggers.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PlannerIndexes.h"
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
    auto const* errands = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data);
    if (!errands || errands->stop >= errands->stops.size())
        return false;
    WorldPosition const& place = errands->stops[errands->stop].pos;
    float const joinYards = WorldTravel::STOP_JOIN_YARDS;
    char const* reason = "busy";
    bool started = false;
    if (kind == WorldTravel::ErrandKind::Mail)
    {
        std::optional<BankMail::MailboxSpot> const spot = BankMail::MailboxSpotNear(
            bot, place.GetMapId(), place.GetPositionX(), place.GetPositionY(), place.GetPositionZ(), joinYards);
        if (!spot)
            reason = "no_mailbox";
        else
            started = BankMail::BeginMailVisit(botAI, *spot);
    }
    else
    {
        std::optional<BankMail::BankerSpot> const spot = BankMail::BankerSpotNear(
            bot, place.GetMapId(), place.GetPositionX(), place.GetPositionY(), place.GetPositionZ(), joinYards);
        if (!spot)
            reason = "no_banker";
        else
            started = BankMail::BeginBankVisit(botAI, *spot);
    }
    if (!started)
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, WorldTravel::ErrandKindName(kind), reason, 0.0f);
    return started;
}

// The planner's bag purchase at the vendor (Planner::OrderedBag): the bag its plan weighed is bought here, not by the
// generic buy's own worth test, which judges by the trigger's slot measure and may refuse what the plan chose (WT63c,
// lab14 seed 75779019: a trip to the planner's bag vendor sold and bought no bag). A test bot logs the outcome word
void BuyOrderedBag(PlayerbotAI* botAI, Creature* vendor)
{
    Player* bot = botAI->GetBot();
    uint32 const item = Planner::OrderedBag(botAI);
    ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item) : nullptr;
    VendorItemData const* items = vendor->GetVendorItems();
    uint32 slot = 0;
    while (proto && items && slot < items->GetItemCount() && items->GetItem(slot)->item != item)
        ++slot;
    uint32 const price = proto ? uint32(std::floor(proto->BuyPrice * bot->GetReputationPriceDiscount(vendor))) : 0;
    char const* word = "bought";
    if (!item)
        word = "no_order";
    else if (!proto || !items || slot >= items->GetItemCount())
        word = "vendor_lacks";
    else if (price > bot->GetMoney())
        word = "money";
    else
    {
        uint32 const before = bot->GetItemCount(item, false);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, item, 1, NULL_BAG, NULL_SLOT);
        if (bot->GetItemCount(item, false) > before)
        {
            EventLog::Emit(bot, EventLog::EventType::Inventory, "bag_bought", item, price);
            Planner::NoteBagBought(botAI);
            botAI->DoSpecificAction("equip upgrades packet action");
        }
        else
            word = "refused";  // the core's refusal: no free slot, a stock sold out
    }
    if (botAI->testConstraints)
        LOG_INFO("playerbots", "[Probe] {} bag buy {} item {} vendor {} price {} money {}", bot->GetName(), word, item,
                 vendor->GetEntry(), price, bot->GetMoney());
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
        {
            ShopAtVendor(botAI, vendor, repaired);
            BuyOrderedBag(botAI, vendor);
        }

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

bool NewRpgInternal::OrderedStop(PlayerbotAI* botAI, Planner::PlannerIndexes const& indexes, ErrandOrder const& order,
                                 std::vector<NewRpgInfo::ErrandStop>& stops)
{
    if (!order.order || !order.vendor)
        return false;
    // The order's vendor is the trip's purpose, so it comes first (WT63c run 2: last, behind a far stock stop, the
    // sale or the bag waited minutes beside the vendor)
    auto const named = std::find_if(stops.begin(), stops.end(), [&order](NewRpgInfo::ErrandStop const& stop)
                                    { return stop.vendorEntry == order.vendor; });
    if (named != stops.end())
    {
        std::rotate(stops.begin(), named, named + 1);
        return false;
    }
    Player* bot = botAI->GetBot();
    for (Planner::Nearby const& found :
         indexes.NearestAll(Planner::ServiceKind::Vendor, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                            bot->GetPositionZ()))
    {
        Planner::Service const& vendor = indexes.services[found.index];
        if (vendor.entry != order.vendor)
            continue;
        NewRpgInfo::ErrandStop stop;
        stop.pos = WorldPosition(vendor.mapId, vendor.x, vendor.y, vendor.z);
        stop.vendorEntry = vendor.entry;
        stops.insert(stops.begin(), std::move(stop));
        return true;
    }
    return false;
}

uint32 NewRpgInternal::SellableSlots(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    AiObjectContext* context = botAI->GetAiObjectContext();
    BankMail::RefreshCarriedKeeps(botAI);
    uint32 slots = 0;
    for (ItemUsage const usage : {ITEM_USAGE_VENDOR, ITEM_USAGE_AH})
        for (Item* item :
             AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(static_cast<uint32>(usage))))
            if (WorldTravel::InGeneralSlot(bot, item) &&
                WorldTravel::CountsAsSellable(item->GetTemplate()->Quality == ITEM_QUALITY_POOR, true))
                ++slots;
    return slots;
}

uint32 NewRpgInternal::BagVendorOf(Player* bot, Planner::PlannerIndexes const& indexes, uint32 item)
{
    for (Planner::Nearby const& found :
         indexes.NearestAll(Planner::ServiceKind::BagVendor, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                            bot->GetPositionZ()))
    {
        Planner::Service const& vendor = indexes.services[found.index];
        if (bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(vendor.factionTemplate), bot) <= REP_UNFRIENDLY)
            continue;
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(vendor.entry);
        if (items && std::any_of(items->m_items.begin(), items->m_items.end(),
                                 [item](VendorItem const* sold) { return sold && sold->item == item; }))
            return vendor.entry;
    }
    return 0;
}

bool NewRpgInternal::BindAtInn(PlayerbotAI* botAI, char const*& why, ObjectGuid& outOfSight)
{
    constexpr uint32 SPELL_INN_BIND = 3286;  // the innkeeper's bind (WorldSession::SendBindPoint)
    Player* bot = botAI->GetBot();
    why = "no innkeeper in reach";
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(bot, WorldTravel::INN_REACH_YARDS);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, WorldTravel::INN_REACH_YARDS);
    for (Unit* unit : units)
    {
        if (!unit->IsCreature() || !unit->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            continue;
        Creature* innkeeper = bot->GetNPCIfCanInteractWith(unit->GetGUID(), UNIT_NPC_FLAG_INNKEEPER);
        if (!innkeeper)
        {
            why = "innkeeper not interactable";
            continue;
        }
        // As SendBindPoint, which drops the cast's result: the bind is cast here and judged by where it now lies
        uint32 const mapBefore = bot->m_homebindMapId;
        float const xBefore = bot->m_homebindX;
        float const yBefore = bot->m_homebindY;
        SpellCastResult const cast =
            bot->GetMap()->Instanceable() ? SPELL_FAILED_NOT_HERE : innkeeper->CastSpell(bot, SPELL_INN_BIND, true);
        float const yards = bot->GetExactDist(bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ);
        bool const took = WorldTravel::BindTook(bot->m_homebindMapId == bot->GetMapId(), yards);
        if (!took || botAI->testConstraints)  // a failed bind's cause; a test bot's every bind
            LOG_INFO("playerbots",
                     "[Route] {} bind at {} {} {}: cast result {}, bind {} ({:.0f}, {:.0f}) -> {} ({:.0f}, {:.0f}) "
                     "{:.1f} yd from the bot; the innkeeper {:.1f} yd away, in sight {}, phases {}/{}, mounted {}, "
                     "flying {}, in combat {}",
                     bot->GetName(), innkeeper->GetEntry(), innkeeper->GetName(), took ? "took" : "did not take",
                     static_cast<uint32>(cast), mapBefore, xBefore, yBefore, bot->m_homebindMapId, bot->m_homebindX,
                     bot->m_homebindY, yards, bot->GetExactDist(innkeeper),
                     innkeeper->IsWithinLOSInMap(bot, VMAP::ModelIgnoreFlags::M2), innkeeper->GetPhaseMask(),
                     bot->GetPhaseMask(), bot->IsMounted(), bot->IsFlying(), bot->IsInCombat());
        if (took)
            return true;
        why = "the bind did not take";
        if (cast == SPELL_FAILED_LINE_OF_SIGHT)  // the spot lies out of his sight: the next try beside him (WT63e-b)
        {
            why = "innkeeper out of sight";
            outOfSight = innkeeper->GetGUID();
            if (botAI->testConstraints)
                LOG_INFO("playerbots", "[Route] {} bind at {} out of sight: walks up to the innkeeper", bot->GetName(),
                         innkeeper->GetEntry());
        }
    }
    return false;
}
