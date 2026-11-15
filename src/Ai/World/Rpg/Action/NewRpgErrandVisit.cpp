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
// The stock per upkeep kind now as the needs count it (one snapshot: the kept grade reads the level and the equipped
// ammo), for the use measure
UpkeepNeeds::StockLevels StockLevelsOf(Player* bot)
{
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    return UpkeepNeeds::LevelsOf(index.get(), Planner::FillSnapshot(bot, 0));
}

// Lots of one vendor row through the core's purchase (Player::BuyItemFromVendorSlot: count is lots, the price the
// game's with the reputation discount; it rechecks every cost and the bag room), judged by the item count because its
// return is no success flag. One call for all lots, then lot by lot when that fails (the bags hold fewer), up to the
// first failure. Returns the lots bought
uint32 BuyLots(Player* bot, Creature* vendor, uint32 slot, uint32 item, uint32 lots)
{
    uint32 const before = bot->GetItemCount(item, false);
    bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, item, static_cast<uint8>(lots), NULL_BAG, NULL_SLOT);
    if (bot->GetItemCount(item, false) > before)
        return lots;

    uint32 bought = 0;
    for (; lots > 1 && bought < lots; ++bought)
    {
        uint32 const count = bot->GetItemCount(item, false);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, item, 1, NULL_BAG, NULL_SLOT);
        if (bot->GetItemCount(item, false) <= count)
            break;
    }
    return bought;
}

// The riding need's mount at the vendor that sells it (upkeep §3.8, WU14): a fresh need list (the shopping spent
// gold), the rank bought by worth whose mount this vendor sells, bought through the core's purchase at its gold-only
// row within the need's funded share and learned as the item's own use learns it (the learning spell in its second
// spell; the item is used up). A rank not known yet waits, as the core's item test wants the riding skill: the next
// errand check sends the bot for the mount alone. A mount item the bot carries already is learned, not bought again
void MountAtVendor(PlayerbotAI* botAI, Creature* vendor, Planner::UpkeepIndex const& index)
{
    Player* bot = botAI->GetBot();
    Planner::Snapshot const snapshot = Planner::FillSnapshot(bot, 0);
    UpkeepNeeds::NeedList const needs =
        UpkeepNeeds::ComputeNeeds(snapshot, index, Planner::BotNeedRates(botAI, snapshot));
    uint32 const entry = vendor->GetEntry();
    UpkeepNeeds::Need const* need = nullptr;
    UpkeepNeeds::RankValue const* value = nullptr;
    uint32 item = 0;  // this vendor's own mount for the rank (any race's vendor that sells one at the price)
    for (UpkeepNeeds::Need const& each : needs.needs)
    {
        UpkeepNeeds::RankValue const* rank =
            each.kind == UpkeepKind::Riding ? UpkeepNeeds::BoughtRank(needs, each.item) : nullptr;
        item = rank ? UpkeepNeeds::MountSoldBy(index, snapshot.race, snapshot.cls, *rank, entry) : 0;
        if (item)
        {
            need = &each;
            value = rank;
            break;
        }
    }
    if (!need || !value || !item)
        return;

    auto const skip = [bot, need, item](char const* reason)
    {
        WorldLog::Decision(
            bot, WorldLogFormat::DecisionKind::Upkeep, "skip", reason, static_cast<float>(need->copper),
            {{"funded", static_cast<float>(need->funded)}, {"gold", static_cast<float>(bot->GetMoney())}}, item);
    };
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    uint32 const learn =
        proto && proto->Spells[1].SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID ? proto->Spells[1].SpellId : 0;
    if (!value->known || !learn || bot->CanUseItem(proto) != EQUIP_ERR_OK)
    {
        skip(value->known ? "mount_unusable" : "mount_no_rank");
        return;
    }

    // A mount the bot knows already is never bought again
    if (bot->HasSpell(learn))
    {
        skip("mount_known");
        return;
    }

    uint32 const moneyBefore = bot->GetMoney();
    if (!bot->HasItemCount(item, 1, false))
    {
        VendorItemData const* rows = vendor->GetVendorItems();
        uint32 const count = rows ? rows->GetItemCount() : 0;
        uint32 slot = count;  // the vendor's own row of the mount, payable in gold and in stock
        for (uint32 at = 0; at < count; ++at)
        {
            VendorItem const* row = rows->GetItem(at);
            if (row && row->item == item && !row->ExtendedCost &&
                (!row->maxcount || vendor->GetVendorItemCurrentCount(row)))
            {
                slot = at;
                break;
            }
        }
        // The core's price (Player::BuyItemFromVendorSlot: the buy price with the reputation discount)
        uint64 const price = static_cast<uint64>(
            std::floor(static_cast<float>(proto->BuyPrice) * bot->GetReputationPriceDiscount(vendor)));
        if (slot == count)
        {
            skip("not_sold");
            return;
        }
        if (price > need->funded)
        {
            skip("budget");
            return;
        }
        if (!BuyLots(bot, vendor, slot, item, 1))
        {
            skip("refused");
            return;
        }
    }

    uint32 const paid = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0;
    bot->learnSpell(learn);
    if (!bot->HasSpell(learn))
    {
        // The item stays in the bags: the next visit learns it without paying again
        skip("not_learned");
        return;
    }

    bot->DestroyItemCount(item, 1, true);
    EventLog::Emit(bot, Upkeep::EVENT_RIDING_BOUGHT, "mount", item, int64(paid));
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "buy",
                       static_cast<float>(value->Price()) <= value->worth ? "worth" : "want", static_cast<float>(paid),
                       {{"mount", static_cast<float>(value->rank.skill)}, {"funded", static_cast<float>(need->funded)}},
                       item);
}

// The shopping list at the errand's vendor (upkeep WU6, §3.6): the needs from a fresh snapshot (the sell and the
// repair changed the gold), each stock in Fund's order bought in whole lots within its funded share (PlanShopping),
// the vendor's own rows rechecked (gold only, a limited row's live count). An item the bot's item usage would sell
// (VENDOR, AH or NONE: the generic sell of the next visit) is never bought. A repair paid at this stop already spent
// what the gold pays; the gear left worn costs more than the rest, so it holds no gold back here. A failed purchase
// skips the item; the bot goes on. At most one row per stock kind
void ShopAtVendor(PlayerbotAI* botAI, Creature* vendor, bool repaired)
{
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    VendorItemData const* rows = vendor->GetVendorItems();
    if (!index || !rows || !rows->GetItemCount())
        return;

    Player* bot = botAI->GetBot();
    Planner::Snapshot snapshot = Planner::FillSnapshot(bot, 0);
    if (repaired)
        snapshot.repairCost = 0;
    UpkeepNeeds::NeedList const needs =
        UpkeepNeeds::ComputeNeeds(snapshot, *index, Planner::BotNeedRates(botAI, snapshot));
    std::vector<UpkeepNeeds::ShopLine> const lines =
        UpkeepNeeds::PlanShopping(needs, *index, snapshot, vendor->GetEntry());
    for (UpkeepNeeds::ShopLine const& line : lines)
    {
        char const* const word = UpkeepNeeds::ShopWord(line.kind);
        if (line.outcome == UpkeepNeeds::ShopOutcome::Budget)
        {
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "budget",
                               static_cast<float>(line.wanted),
                               {{word, static_cast<float>(line.units)},
                                {"funded", static_cast<float>(line.funded)},
                                {"gold", static_cast<float>(bot->GetMoney())}},
                               line.item);
            continue;
        }

        if (line.outcome == UpkeepNeeds::ShopOutcome::Buy)
        {
            ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", line.item)->Get();
            if (usage == ITEM_USAGE_NONE || usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH)
            {
                WorldLog::Decision(
                    bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "would_sell", static_cast<float>(line.copper),
                    {{word, static_cast<float>(line.units)}, {"usage", static_cast<float>(usage)}}, line.item);
                continue;
            }
        }

        uint32 lots = line.lots;
        uint32 slot = rows->GetItemCount();  // the vendor's own row of the item, payable in gold
        if (line.outcome == UpkeepNeeds::ShopOutcome::Buy)
            for (uint32 at = 0; at < rows->GetItemCount(); ++at)
            {
                VendorItem const* row = rows->GetItem(at);
                if (!row || row->item != line.item || row->ExtendedCost)
                    continue;
                if (row->maxcount)
                    lots = std::min(lots, vendor->GetVendorItemCurrentCount(row) / line.lotSize);
                slot = at;
                break;
            }
        if (slot == rows->GetItemCount() || !lots)
        {
            WorldLog::Decision(
                bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "not_sold", static_cast<float>(line.wanted),
                {{word, static_cast<float>(line.units)}, {"target", static_cast<float>(line.target)}}, line.item);
            continue;
        }

        uint32 const moneyBefore = bot->GetMoney();
        uint32 const bought = BuyLots(bot, vendor, slot, line.item, lots);
        uint32 const paid = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0;
        if (!bought)
        {
            // The core refused (the bags full, a reputation or vendor condition): skip the item
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "refused",
                               static_cast<float>(line.copper),
                               {{word, static_cast<float>(line.units)},
                                {"free_slots", static_cast<float>(WorldTravel::FreeGeneralSlots(bot))}},
                               line.item);
            continue;
        }

        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "buy", line.due ? "stock_low" : "top_up",
                           static_cast<float>(paid),
                           {{word, static_cast<float>(bought * line.lotSize)},
                            {"target", static_cast<float>(line.target)},
                            {"funded", static_cast<float>(line.funded)}},
                           line.item);
    }

    // The riding need's mount, when this vendor sells it (WU14), with the gold the stock left: MountAtVendor takes
    // this vendor's own mount for the bought rank, so any vendor of the errand may be the one
    if (std::any_of(needs.needs.begin(), needs.needs.end(),
                    [](UpkeepNeeds::Need const& need) { return need.kind == UpkeepKind::Riding; }))
        MountAtVendor(botAI, vendor, *index);
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
bool BuysAtVendors(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    return RandomPlayerbotMgr::HonestTravel() && sRandomPlayerbotMgr.IsRandomBot(bot) &&
           !IsRealPlayer(botAI->GetMaster());
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
            UpkeepNeeds::SampleUse(measure, StockLevelsOf(bot), bot->getClass(), bot->GetLevel(), getMSTime());
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
            UpkeepNeeds::NoteStock(measure, StockLevelsOf(bot), getMSTime());
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
    return true;
}
