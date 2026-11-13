/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "AreaDefines.h"
#include "BroadcastHelper.h"
#include "CellImpl.h"
#include "CensusMgr.h"
#include "ChatHelper.h"
#include "CheckMountStateAction.h"
#include "DBCStores.h"
#include "ErrandRules.h"
#include "G3D/Vector2.h"
#include "GameObjectModel.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "IVMapMgr.h"
#include "InventorySlots.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NewRpgHelpers.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "NewRpgTriggers.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestIndex.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "RouteService.h"
#include "RpgNavMeshSnap.h"
#include "SharedDefines.h"
#include "StartingHistoryRules.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "Trainer.h"
#include "Transport.h"
#include "TravelGraph.h"
#include "TravelLegRules.h"
#include "TravelMgr.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "WaypointMovementGenerator.h"
#include "WorldLog.h"
#include "WorldLogStatus.h"
#include "WorldPacket.h"
#include "WorldTravelRules.h"

namespace
{
// Ends the bot's errand: stamps the errand check, measures t from a visited vendor and records the run; an errand given
// up records abortSource (a literal: errand_abort, or errand_sick when the sickness ended it)
void FinishErrands(PlayerbotAI* botAI, bool visited, char const* abortSource = "errand_abort")
{
    WorldLog::StatusReason const reasonScope("finished");
    NewRpgInfo& info = botAI->rpgInfo;
    auto const* data = std::get_if<NewRpgInfo::Errands>(&info.data);
    if (!data)
        return;

    Player* bot = botAI->GetBot();
    WorldTravel::SlotMeasure& measure =
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    uint32 const now = getMSTime();
    float const minutes = static_cast<float>(getMSTimeDiff(info.startT, now)) / (MINUTE * IN_MILLISECONDS);
    WorldTravel::StampErrandCheck(measure, now);
    if (visited)
    {
        WorldTravel::AddTripMinutes(measure, minutes);
        EventLog::Emit(bot, "Inventory", "errand_run", data->vendorEntry,
                       int64(bot->GetMoney()) - int64(data->moneyAtStart));
    }
    else
        EventLog::Emit(bot, "Inventory", abortSource, data->vendorEntry, int64(minutes));

    info.ChangeToIdle();
}

// The Travel event sources of an abort ("abort:<reason>"); the trip's abort decision takes the reason
constexpr std::string_view TRAVEL_ABORT_PREFIX = "abort:";

// Gives the bot's trip up: the Travel event (source "abort:<reason>", a literal), the trip's abort decision, idle
void GiveTripUp(PlayerbotAI* botAI, NewRpgInfo::Travel const& data, char const* why, char const* source)
{
    Player* bot = botAI->GetBot();
    if (data.place.empty())
        LOG_DEBUG("playerbots", "[Route] {} gives the trip up: {}", bot->GetName(), why);
    else
        LOG_INFO("playerbots", "[Route] {} gives the trip to {} up: {}", bot->GetName(), data.place, why);
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    EventLog::Emit(bot, "Travel", source, data.tripId, seconds);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "abort",
                       WorldLogFormat::AfterPrefix(source, TRAVEL_ABORT_PREFIX), static_cast<float>(seconds),
                       {{"replans", static_cast<float>(data.trip.replans)}}, data.tripId);
    WorldLog::StatusReason const reasonScope("abort");
    botAI->rpgInfo.ChangeToIdle();
}

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

// A bot that buys at vendors (the shopping list, a riding rank and its mount): a random bot in honest travel without a
// real player as master. The riding errand is taken only for such a bot, so a rank is never taught where the mount
// could not be bought (a known rank without its mount keeps the need open)
bool BuysAtVendors(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    return RandomPlayerbotMgr::HonestTravel() && sRandomPlayerbotMgr.IsRandomBot(bot) &&
           !IsRealPlayer(botAI->GetMaster());
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
    for (UpkeepNeeds::Need const& each : needs.needs)
        if (each.kind == UpkeepKind::Riding && each.vendor == entry)
        {
            need = &each;
            break;
        }
    UpkeepNeeds::RankValue const* value = nullptr;
    for (UpkeepNeeds::RankValue const& each : needs.ranks)
        if (need && each.bought && each.rank.spellId == need->item)
            value = &each;
    if (!need || !value || !value->mountItem)
        return;

    uint32 const item = value->mountItem;
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

    // The riding need's mount, when this vendor sells it (WU14), with the gold the stock left
    uint32 const entry = vendor->GetEntry();
    if (std::any_of(needs.needs.begin(), needs.needs.end(), [entry](UpkeepNeeds::Need const& need)
                    { return need.kind == UpkeepKind::Riding && need.vendor == entry; }))
        MountAtVendor(botAI, vendor, *index);
}

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
    Planner::Snapshot const snapshot = Planner::FillSnapshot(bot, 0);
    Planner::TrainerList const* list = index ? index->ClassList(snapshot.cls) : nullptr;
    UpkeepNeeds::Need need;
    if (index && trainer && list && UpkeepNeeds::BitsAlign(*index, snapshot) && trainer->IsTrainerValidForPlayer(bot))
    {
        UpkeepNeeds::NeedList const needs =
            UpkeepNeeds::ComputeNeeds(snapshot, *index, Planner::BotNeedRates(botAI, snapshot));
        for (UpkeepNeeds::Need const& each : needs.needs)
            if (each.kind == UpkeepKind::TrainerSpell)
                need = each;
    }

    uint32 const moneyBefore = bot->GetMoney();
    uint32 learned = 0;
    UpkeepNeeds::TrainingBuy buy;
    if (need.units)
    {
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
        Planner::Snapshot const snapshot = Planner::FillSnapshot(bot, 0);
        UpkeepNeeds::NeedList const needs =
            UpkeepNeeds::ComputeNeeds(snapshot, *index, Planner::BotNeedRates(botAI, snapshot));
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

WorldTravel::ErrandPlace ErrandPlaceOf(WorldLocation const& loc)
{
    return {loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ()};
}

// The spawns on the bot's map of the trainer an upkeep need names (the index's entries of its key) that trade with
// the bot, the nearest TRAINER_STOPS within reach
std::vector<TravelMgr::TrainerInfo const*> TrainerSpawns(Player* bot, Planner::UpkeepIndex const& index, uint32 key,
                                                         float reachYards)
{
    constexpr std::size_t TRAINER_STOPS = 3;
    std::vector<TravelMgr::TrainerInfo const*> result;
    auto const trainer = std::find_if(index.trainers.begin(), index.trainers.end(),
                                      [key](Planner::UpkeepTrainer const& known) { return known.key == key; });
    if (!key || trainer == index.trainers.end())
        return result;

    std::vector<std::pair<float, TravelMgr::TrainerInfo const*>> spawns;
    for (TravelMgr::TrainerInfo const& spawn : TravelMgr::instance().GetTrainers(bot->GetMapId()))
    {
        // The vendors' interact test (NewRpgStartErrandsAction::Execute)
        if (!std::binary_search(trainer->entries.begin(), trainer->entries.end(), spawn.entry) ||
            bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(spawn.factionTemplate), bot) <= REP_UNFRIENDLY)
            continue;

        float const yards =
            bot->GetExactDist(spawn.loc.GetPositionX(), spawn.loc.GetPositionY(), spawn.loc.GetPositionZ());
        if (yards <= reachYards)
            spawns.emplace_back(yards, &spawn);
    }
    std::sort(spawns.begin(), spawns.end(),
              [](std::pair<float, TravelMgr::TrainerInfo const*> const& a,
                 std::pair<float, TravelMgr::TrainerInfo const*> const& b) { return a.first < b.first; });
    if (spawns.size() > TRAINER_STOPS)
        spawns.resize(TRAINER_STOPS);
    for (std::pair<float, TravelMgr::TrainerInfo const*> const& at : spawns)
        result.push_back(at.second);
    return result;
}

// The trainer spawns as the trip's stops with that service
void AddTrainerStops(std::vector<TravelMgr::TrainerInfo const*> const& spawns, uint32 service,
                     std::vector<WorldTravel::ServiceStop>& stops)
{
    for (TravelMgr::TrainerInfo const* spawn : spawns)
        stops.push_back({spawn->entry, ErrandPlaceOf(spawn->loc), service});
}

// The bot's completed quests with the nearest ender on its map (the planner's quest index; none before it is built)
void AddCompletedQuests(Player* bot, std::vector<WorldTravel::CompletedQuest>& completed)
{
    std::shared_ptr<Planner::QuestIndex const> const index = Planner::Quests();
    if (!index)
        return;

    uint32 const mapId = bot->GetMapId();
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;

        Planner::QuestRecord const* record = index->Find(questId);
        if (!record)
            continue;

        Planner::QuestPlace const* ender = nullptr;
        float enderYards = std::numeric_limits<float>::max();
        for (uint32 const placeIdx : record->enders)
        {
            if (placeIdx >= index->places.size() || index->places[placeIdx].mapId != mapId)
                continue;

            Planner::QuestPlace const& place = index->places[placeIdx];
            float const yards = bot->GetExactDist(place.x, place.y, place.z);
            if (yards < enderYards)
            {
                ender = &place;
                enderYards = yards;
            }
        }
        if (!ender)
            continue;

        WorldTravel::CompletedQuest quest;
        quest.QuestId = questId;
        quest.Ender = ender->entry;
        quest.EnderIsObject = ender->isObject;
        quest.Place = {ender->mapId, ender->x, ender->y, ender->z};
        quest.WorthCopper = static_cast<float>(std::max<int32>(record->copperReward, 0));
        completed.push_back(quest);
    }
}
}  // namespace

void TellRpgStatusAction::WhisperStatusChange(Player* owner, std::string const& statusName)
{
    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        RPG_STATUS_CHANGED_KEY, RPG_STATUS_CHANGED_DEFAULT,
        {{"%status", statusName}});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
}

bool TellRpgStatusAction::Execute(Event event)
{
    WorldLog::StatusReason const reasonScope("order");
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    if (text.empty())
    {
        std::string out = botAI->rpgInfo.ToString();
        bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
        return true;
    }

    Player* master = botAI->GetMaster();
    bool isMaster = master && master->GetGUID() == owner->GetGUID();
    bool isGM = owner->GetSession() && owner->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    if (!isMaster && !isGM)
    {
        std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "rpg_debug_permission_error",
            "Only your master or a GM can change my rpg status.", {});
        bot->Whisper(msg, LANG_UNIVERSAL, owner);
        return false;
    }

    std::string name = text;
    uint32 questId = 0;
    static std::string const doQuestPrefix = "do quest ";
    size_t doQuestPos = text.find(doQuestPrefix);
    if (doQuestPos != std::string::npos)
    {
        name = "do quest";
        std::string idStr = text.substr(doQuestPos + doQuestPrefix.length());
        try
        {
            questId = static_cast<uint32>(std::stoul(idStr));
        }
        catch (std::exception const&)
        {
            questId = 0;
        }
    }

    NewRpgStatus status = NewRpgInfo::StatusFromString(name);
    NewRpgInfo& info = botAI->rpgInfo;

    if (status == RPG_IDLE)
    {
        info.ChangeToIdle();
        WhisperStatusChange(owner, "IDLE");
        return true;
    }
    else if (status == RPG_REST)
    {
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        WhisperStatusChange(owner, "REST");
        return true;
    }
    else if (status == RPG_WANDER_RANDOM)
    {
        info.ChangeToWanderRandom();
        WhisperStatusChange(owner, "WANDER_RANDOM");
        return true;
    }
    else if (status == RPG_WANDER_NPC)
    {
        info.ChangeToWanderNpc();
        WhisperStatusChange(owner, "WANDER_NPC");
        return true;
    }
    else if (status == RPG_GO_GRIND)
    {
        WorldPosition pos = SelectRandomGrindPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_grind_pos_error", "No grind position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoGrind(pos);
        WhisperStatusChange(owner, "GO_GRIND");
        return true;
    }
    else if (status == RPG_GO_CAMP)
    {
        WorldPosition pos = SelectRandomCampPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_camp_pos_error", "No camp position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoCamp(pos);
        WhisperStatusChange(owner, "GO_CAMP");
        return true;
    }
    else if (status == RPG_TRAVEL_FLIGHT)
    {
        uint32 flightMasterEntry = 0;
        WorldPosition flightMasterPos;
        std::vector<uint32> path;
        if (!SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_flight_path_error", "No flight path available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, std::move(path));
        WhisperStatusChange(owner, "TRAVEL_FLIGHT");
        return true;
    }
    else if (status == RPG_OUTDOOR_PVP)
    {
        info.ChangeToOutdoorPvp();
        WhisperStatusChange(owner, "OUTDOOR_PVP");
        return true;
    }
    else if (status == RPG_DO_GATHER)
    {
        if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_gather_no_profession_error", "I have neither Herbalism nor Mining, so I can't gather.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoGather();
        WhisperStatusChange(owner, "DO_GATHER");
        return true;
    }
    else if (status == RPG_DO_QUEST)
    {
        if (!questId)
        {
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 qid = bot->GetQuestSlotQuestId(slot);
                if (!qid)
                    continue;
                std::vector<POIInfo> poi;
                if (GetQuestPOIPosAndObjectiveIdx(qid, poi, true))
                {
                    questId = qid;
                    break;
                }
            }
        }
        if (!questId)
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_quest_error", "No quest available; use 'do quest <id>'.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatus questStatus = bot->GetQuestStatus(questId);
        if (!quest || (questStatus != QUEST_STATUS_INCOMPLETE && questStatus != QUEST_STATUS_COMPLETE))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_invalid_quest_error", "Invalid quest %quest_id",
                {{"%quest_id", std::to_string(questId)}});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoQuest(questId, quest);
        WhisperStatusChange(owner, "DO_QUEST " + std::to_string(questId));
        return true;
    }

    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "rpg_unknown_status_error",
        "Unknown rpg status. Options: idle, rest, wander random, wander npc, "
        "go grind, go camp, do quest [<id>], travel flight, outdoor pvp, do gather.", {});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
    return false;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    WorldLog::StatusReason const reasonScope("order");
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

namespace
{
constexpr uint32 HEARTHSTONE_SPELL = 8690;
constexpr uint32 TRAVEL_ORDER_TTL_MS = 60 * IN_MILLISECONDS;
constexpr char const* CONSTRAINT_EVENT_TYPE = "Constraint";

std::mutex travelOrdersLock;
std::unordered_map<ObjectGuid, TravelOrder> travelOrders;  // guarded by travelOrdersLock

// nofight: creatures neither attack an immune-to-NPC unit nor are attacked by it (Unit::_IsValidAttackTarget); setting
// the flag ends the unit's PvE combat
void SetNoFight(Unit* unit, bool on)
{
    if (unit && unit->IsImmuneToNPC() != on)
        unit->SetImmuneToNPC(on);
}

void NoFight(Player& bot, bool on)
{
    SetNoFight(&bot, on);
    for (Unit* controlled : bot.m_Controlled)
        SetNoFight(controlled, on);
}

void ApplyNoFight(Player& bot) { NoFight(bot, true); }
void UndoNoFight(Player& bot) { NoFight(bot, false); }
void ApplyNoDamage(Player& bot) { bot.SetCommandStatusOn(CHEAT_GOD); }
void UndoNoDamage(Player& bot) { bot.SetCommandStatusOff(CHEAT_GOD); }

// The state rows' apply and undo (TestConstraints::ROWS)
constexpr TestConstraints::StateOps<Player> STATE_OPS[] = {
    {TestConstraints::NOFIGHT, ApplyNoFight, UndoNoFight},
    {TestConstraints::NODAMAGE, ApplyNoDamage, UndoNoDamage},
};
static_assert(TestConstraints::Covered(STATE_OPS) == TestConstraints::STATES, "every state row has apply and undo");

void PlaceAboard(PlayerbotAI* botAI, Player* bot, uint32 entry);  // beside the transport legs' deck helpers

// A GM order ends the bot's trip: the Travel event and the trip decision with the word (stopped, held)
void EndTripByOrder(Player* bot, NewRpgInfo& info, NewRpgInfo::Travel const& data, char const* word)
{
    LOG_INFO("playerbots", "[Route] {} gives the trip up: {}", bot->GetName(), word);
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    std::string const source = std::string("abort:") + word;
    EventLog::Emit(bot, "Travel", source.c_str(), data.tripId, seconds);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "abort", word, static_cast<float>(seconds),
                       {{"replans", static_cast<float>(data.trip.replans)}}, data.tripId);
    info.ChangeToIdle();
}

// A test's constraint change: the state rows applied or undone, one Constraint row whose amount is the ms from the
// command to the take (also on an idle bot). A choice row turning on ends the current status unless a trip comes with
// it, so the bot stands idle; the idle gate then keeps it there. The planner's next snapshot sees the hearth bit; the
// errand row turning off forgets the last errand check (TestConstraints::FreesErrands)
void ApplyConstraints(PlayerbotAI* botAI, Player* bot, NewRpgInfo& info, TravelOrder const& order, bool tripFollows)
{
    uint16 const before = info.constraints;
    uint16 const after = TestConstraints::Next(before, order.on, order.off);
    info.constraints = after;
    info.planner.snapshotDue = true;
    if (TestConstraints::FreesErrands(before, after))
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet().ErrandChecked = false;
    TestConstraints::ChangeStates(STATE_OPS, *bot, before, after);
    std::string const set = TestConstraints::Format(after);
    uint32 const latencyMs = GetMSTimeDiffToNow(order.postedMs);
    LOG_INFO("playerbots", "[Route] {} constraints: {} (taken after {} ms)", bot->GetName(), set, latencyMs);
    EventLog::Emit(bot, CONSTRAINT_EVENT_TYPE, set.c_str(), 0, latencyMs);
    if (!TestConstraints::EndsStatus(before, after) || tripFollows)
        return;
    WorldLog::StatusReason const reasonScope("hold");
    if (auto const* data = std::get_if<NewRpgInfo::Travel>(&info.data))
        EndTripByOrder(bot, info, *data, "held");
    else
        info.ChangeToIdle();
}

// A GM order taken on the bot's map thread: the state is checked again (the command saw it on the world thread)
bool ApplyTravelOrder(PlayerbotAI* botAI, Player* bot, TravelOrder const& order)
{
    NewRpgInfo& info = botAI->rpgInfo;
    bool const trip = TestConstraints::HasTrip(order);
    bool const constrained = order.on || order.off;
    if (constrained)
        ApplyConstraints(botAI, bot, info, order, trip);
    if (order.aboard)
        PlaceAboard(botAI, bot, order.aboard);
    bool const applied = constrained || order.aboard;
    if (!trip)
        return applied;
    WorldLog::StatusReason const reasonScope("order");
    if (order.stop)
    {
        auto const* data = std::get_if<NewRpgInfo::Travel>(&info.data);
        if (!data)
        {
            LOG_INFO("playerbots", "[Route] {} stop: not travelling", bot->GetName());
            return applied;
        }
        EndTripByOrder(bot, info, *data, "stopped");
        return true;
    }

    char const* refusal = nullptr;
    if (!bot->IsAlive())
        refusal = "dead";
    else if (bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS))
        refusal = "sick";  // a sick bot rests (upkeep §3.5)
    else if (bot->IsInFlight())
        refusal = "in flight";
    else if (bot->GetMap()->Instanceable())
        refusal = "in an instance";
    if (refusal)
    {
        LOG_INFO("playerbots", "[Route] {} travel to {} refused: {}", bot->GetName(), order.place, refusal);
        return applied;
    }
    LOG_INFO("playerbots", "[Route] {} travel to {} ({}, {:.0f}, {:.0f}, {:.0f}) ordered", bot->GetName(), order.place,
             order.dest.GetMapId(), order.dest.GetPositionX(), order.dest.GetPositionY(), order.dest.GetPositionZ());
    info.ChangeToTravel(order.dest, order.place);
    return true;
}

// The bot's order when it may take it now (TestConstraints::TakeNow); the bot's hint stays while its order waits.
// Orders nobody took in time are dropped on any take, and logged: the bot logged out or fought for a minute
bool TakeOrder(PlayerbotAI* botAI, ObjectGuid bot, bool inCombat, TravelOrder& order)
{
    std::lock_guard<std::mutex> guard(travelOrdersLock);
    for (auto it = travelOrders.begin(); it != travelOrders.end();)
    {
        if (GetMSTimeDiffToNow(it->second.postedMs) <= TRAVEL_ORDER_TTL_MS)
        {
            ++it;
            continue;
        }
        LOG_INFO("playerbots", "[Route] order '{}' for bot {} dropped: not taken within {} s", it->second.place,
                 it->first.GetCounter(), TRAVEL_ORDER_TTL_MS / IN_MILLISECONDS);
        it = travelOrders.erase(it);
    }
    auto const found = travelOrders.find(bot);
    bool const taken = found != travelOrders.end() && TestConstraints::TakeNow(found->second, inCombat);
    bool const waiting = found != travelOrders.end() && !taken;
    if (taken)
    {
        order = std::move(found->second);
        travelOrders.erase(found);
    }
    botAI->orderPosted.store(waiting, std::memory_order_release);
    return taken;
}
}  // namespace

void TravelOrders::Post(PlayerbotAI* botAI, TravelOrder order)
{
    std::lock_guard<std::mutex> guard(travelOrdersLock);
    // A hold and a trip posted before the bot's next update both stay (TestConstraints::Merge)
    auto const [it, added] = travelOrders.try_emplace(botAI->GetBot()->GetGUID(), order);
    if (!added)
        TestConstraints::Merge(it->second, std::move(order));
    botAI->orderPosted.store(true, std::memory_order_release);
}

bool TravelOrders::TakeAndApply(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI->orderPosted.load(std::memory_order_acquire))
        return false;
    TravelOrder order;
    if (!TakeOrder(botAI, bot->GetGUID(), bot->IsInCombat(), order))
        return false;
    if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
    {
        LOG_INFO("playerbots", "[Route] {} order '{}' refused: the bot does not run the new rpg strategy",
                 bot->GetName(), order.place);
        return false;
    }
    return ApplyTravelOrder(botAI, bot, order);
}

void TravelOrders::KeepConstraints(Player* bot, uint16 constraints)
{
    TestConstraints::KeepStates(STATE_OPS, *bot, constraints);
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    Planner::PlannerHook(botAI, bot);
    // GM orders were taken before the engine ran (TravelOrders::TakeAndApply in PlayerbotAI::UpdateAI)
    NewRpgStatus status = info.GetStatus();
    // Resurrection sickness (upkeep §3.5): a sick bot rests until the sickness ends. Any other status ends, an errand
    // and a trip as given up (errand_sick, abort:sick); a flight in the air lands first. The status pick, the errand
    // start and the travel orders keep it resting
    bool const sick = bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS);
    if (Upkeep::SickEndsStatus(status, sick, bot->IsInFlight()))
    {
        if (status == RPG_ERRANDS)
            FinishErrands(botAI, false, "errand_sick");
        else if (auto const* trip = std::get_if<NewRpgInfo::Travel>(&info.data))
            GiveTripUp(botAI, *trip, "sick", "abort:sick");
        WorldLog::StatusReason const reasonScope("sick");
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    switch (status)
    {
        case RPG_IDLE:
        {
            // A test's hold (TestConstraints): a held choice is not made; an order was taken all the same
            TestConstraints::IdleGate const gate = TestConstraints::GateOf(info.constraints);
            if (!gate.planner && !gate.status)
                return false;
            // A planner bot's goal (WT16); the weighted pick for the A/B group and as the counted fallback. A sick bot
            // takes no goal: the weighted pick lets it only rest (upkeep §3.5)
            Planner::IdleStep step;
            if (gate.planner && !sick)
                step = Planner::OnIdle(botAI, bot,
                                       [this](uint32 questId)
                                       {
                                           std::vector<POIInfo> poiInfo;
                                           return !botAI->lowPriorityQuest.contains(questId) &&
                                                  GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true);
                                       });
            if (step.started)
                return true;
            if (step.wait || !gate.status)
                return false;
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP, RPG_DO_GATHER},
                                      step.reason);
        }

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                WorldLog::StatusReason const reasonScope("arrived");
                info.ChangeToWanderRandom();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                WorldLog::StatusReason const reasonScope("arrived");
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                WorldLog::StatusReason const reasonScope("arrived");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE; a sick bot's rest lasts until the sickness ends
            if (Upkeep::RestEnds(info.HasStatusPersisted(statusRestDuration), sick))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_GATHER:
        {
            // DO_GATHER -> IDLE
            if (info.HasStatusPersisted(statusDoGatherDuration))
            {
                WorldLog::StatusReason const reasonScope("timeout");
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_ERRANDS:
        {
            // ERRANDS -> IDLE: a trip that has not ended in time is given up
            if (info.HasStatusPersisted(WorldTravel::ERRAND_DEADLINE_MS))
            {
                auto const* errands = std::get_if<NewRpgInfo::Errands>(&info.data);
                FinishErrands(botAI, errands && errands->visited);
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // Prefer a giver with something to accept or reward, else any nearby npc; none -> IDLE
        ObjectGuid npcOrGo = NewRpgHelpers::FindQuestGiver(botAI);
        if (npcOrGo.IsEmpty())
        {
            GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
            if (!possibleTargets.empty())
            {
                uint32 idx = urand(0, possibleTargets.size() - 1);
                WorldObject* object = ObjectAccessor::GetWorldObject(*bot, possibleTargets[idx]);
                if (object && object->IsInWorld())
                    npcOrGo = object->GetGUID();
            }
        }
        if (npcOrGo.IsEmpty())
        {
            WorldLog::StatusReason const reasonScope("no_target");
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);

            if (Creature* creature = object->ToCreature())
            {
                constexpr uint32 MIN_VENDOR_BALANCE = 1000;  // 10 silver

                if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) && AI_VALUE(bool, "can sell"))
                {
                    botAI->DoSpecificAction("sell", Event("sell", "vendor"), true);

                    // Buying sorts the vendor list through the weight calculator; skip it for
                    // bots without money for cheap consumables.
                    if (bot->GetMoney() >= MIN_VENDOR_BALANCE)
                        botAI->DoSpecificAction("buy", Event("buy", "vendor"), true);
                }

                if (creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) && AI_VALUE(bool, "can repair"))
                    botAI->DoSpecificAction("repair", Event("repair"), true);
            }

            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgStartErrandsAction::Execute(Event /*event*/)
{
    WorldLog::StatusReason const reasonScope("errand");
    WorldTravel::SlotMeasure& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    // The repair need the trigger saw (its wear test first)
    bool const repairWanted = WorldTravel::RepairErrandWanted(bot);
    WorldTravel::StampErrandCheck(measure, getMSTime());
    measure.HasVendor = false;

    // The needs (upkeep §3.3): the consumable stocks the gold pays a lot of, the due ones and the others, and the
    // vendors that cover them (the immutable index; one snapshot on the bot's map thread)
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    UpkeepNeeds::NeedList needs;
    std::unordered_map<uint32, uint8> cover;
    uint8 dueKinds = 0;
    uint8 unmetKinds = 0;
    if (index)
    {
        Planner::Snapshot const snapshot = Planner::FillSnapshot(bot, 0);
        needs = UpkeepNeeds::ComputeNeeds(snapshot, *index, Planner::BotNeedRates(botAI, snapshot));
        dueKinds = UpkeepNeeds::PayableKinds(needs, true);
        unmetKinds = UpkeepNeeds::PayableKinds(needs, false);
        if (dueKinds | unmetKinds)
            cover = UpkeepNeeds::VendorCover(*index, snapshot, needs, dueKinds | unmetKinds);
    }

    // The vendors on the bot's map that trade with the bot (startup cache; no route to other maps yet): each one an
    // upkeep candidate, the nearest one for the destroy rule, the nearest VENDOR_STOPS within the errand walk limit as
    // the trip's service stops
    constexpr std::size_t VENDOR_STOPS = 8;
    uint32 const mapId = bot->GetMapId();
    float const runSpeed = bot->GetSpeed(MOVE_RUN);
    float const reachYards = WorldTravel::ERRAND_MAX_WALK_MINUTES * MINUTE * runSpeed;
    std::vector<TravelMgr::VendorInfo> const& vendors = TravelMgr::instance().GetVendors(mapId);
    std::vector<UpkeepNeeds::VendorCandidate> candidates;
    TravelMgr::VendorInfo const* nearest = nullptr;
    float nearestYards = std::numeric_limits<float>::max();
    std::vector<std::pair<float, TravelMgr::VendorInfo const*>> inReach;
    uint32 refused = 0;
    for (TravelMgr::VendorInfo const& vendor : vendors)
    {
        // The core's interact test (Player::GetNPCIfCanInteractWith): the vendor's reaction must be above Unfriendly.
        // A template without a reputation faction is judged by the template relation, not refused as Neutral.
        if (bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(vendor.factionTemplate), bot) <= REP_UNFRIENDLY)
        {
            ++refused;
            continue;
        }

        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(vendor.entry);
        float const yards =
            bot->GetExactDist(vendor.loc.GetPositionX(), vendor.loc.GetPositionY(), vendor.loc.GetPositionZ());
        auto const covered = cover.find(vendor.entry);
        candidates.push_back({vendor.entry, WorldTravel::WalkMinutes(yards, runSpeed),
                              proto && (proto->npcflag & UNIT_NPC_FLAG_REPAIR),
                              covered != cover.end() ? covered->second : uint8(0)});
        if (yards < nearestYards)
        {
            nearest = &vendor;
            nearestYards = yards;
        }
        if (yards <= reachYards)
            inReach.emplace_back(yards, &vendor);
    }

    // The needs the trip planner cannot answer, once per errand check: the repair with no repairer on the map or none
    // within the walk limit; each due stock no vendor in reach sells, too far or on no vendor of this map
    float constexpr ANY_WALK = std::numeric_limits<float>::max();
    bool repairing = repairWanted;
    if (repairing)
    {
        std::size_t const nearestRepairer = UpkeepNeeds::PickVendor(candidates, true, 0, 0, ANY_WALK);
        if (nearestRepairer == UpkeepNeeds::NO_VENDOR)
        {
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "no_repairer", 0.0f);
            repairing = false;
        }
        else if (candidates[nearestRepairer].walkMinutes > WorldTravel::ERRAND_MAX_WALK_MINUTES)
        {
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "too_far",
                               static_cast<float>(Planner::PriceRepair(bot).total));
            repairing = false;
        }
    }

    uint8 dueInReach = 0;
    for (UpkeepNeeds::VendorCandidate const& candidate : candidates)
        if (candidate.walkMinutes <= WorldTravel::ERRAND_MAX_WALK_MINUTES)
            dueInReach |= static_cast<uint8>(candidate.covers & dueKinds);
    UpkeepNeeds::Need const* training = nullptr;
    UpkeepNeeds::Need const* riding = nullptr;  // one rank per trip, the lowest the funded gold pays
    bool const buysAtVendors = BuysAtVendors(botAI);
    for (UpkeepNeeds::Need const& need : needs.needs)
    {
        if (need.kind == UpkeepKind::TrainerSpell)
            training = &need;
        else if (!riding && buysAtVendors && UpkeepNeeds::RidingErrandDue(need))
            riding = &need;
        uint8 const bit = Planner::StockBit(need.stock);
        if (!need.due || !UpkeepNeeds::StockPayable(need) || (dueInReach & bit))
            continue;
        bool const onMap = std::any_of(candidates.begin(), candidates.end(),
                                       [bit](UpkeepNeeds::VendorCandidate const& c) { return c.covers & bit; });
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", onMap ? "too_far" : "no_vendor",
                           static_cast<float>(need.copper),
                           {{UpkeepNeeds::ShopWord(need.stock), static_cast<float>(need.units)},
                            {"funded", static_cast<float>(need.funded)}},
                           need.item);
    }
    // Due training makes a trip only when the gold pays a spell (UpkeepNeeds::TrainingErrandDue, as AnyErrandDue)
    bool const trainingDue = training && UpkeepNeeds::TrainingErrandDue(*training);
    bool const needDue = repairing || dueInReach || trainingDue || riding;

    // A map without a vendor that trades with the bot still has its trainers (WU12 NOTE 4): a due class training or a
    // riding rank to learn plans its trainer trip
    bool const trainerWanted = index && ((trainingDue && training->trainer) || (riding && riding->trainer));
    if (!WorldTravel::ErrandCheckPlans(nearest != nullptr, trainerWanted))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} errands: no vendor on map {} ({} vendors, {} refused by faction)",
                  bot->GetName(), mapId, vendors.size(), refused);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "skip", "no_vendor", 0.0f);
        return false;
    }

    float const walkMinutes = nearest ? WorldTravel::WalkMinutes(nearestYards, runSpeed) : 0.0f;
    if (nearest)
    {
        measure.HasVendor = true;
        measure.VendorMapId = mapId;
        measure.VendorX = nearest->loc.GetPositionX();
        measure.VendorY = nearest->loc.GetPositionY();
        measure.VendorZ = nearest->loc.GetPositionZ();
    }

    // The stacks the sell action would take from the general slots (as the destroy rule counts them)
    uint32 sellableSlots = 0;
    for (ItemUsage const usage : {ITEM_USAGE_VENDOR, ITEM_USAGE_AH})
        for (Item* item :
             AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(static_cast<uint32>(usage))))
            if (WorldTravel::InGeneralSlot(bot, item))
                ++sellableSlots;
    uint32 const freeSlots = WorldTravel::FreeGeneralSlots(bot);

    // Full bags alone do not end questing (the trigger let a questing bot through for a due need): a questing bot
    // plans its due needs only, without one it destroys by worth instead
    bool const questing = !WorldTravel::ErrandMayStart(botAI);
    if (questing && !needDue)
    {
        WorldLogFormat::Alternatives weighed;
        if (WorldLog::Active())
            weighed = {{"sellable", static_cast<float>(sellableSlots)}, {"free_slots", static_cast<float>(freeSlots)}};
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "skip", "questing", walkMinutes * MINUTE,
                           weighed);
        return false;
    }

    // The trip planner's view of the bot (ErrandRules.h): what each errand is worth now and where it can be done
    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    WorldTravel::SlotBand const& band = config.SlotBands[WorldTravel::LevelBand(bot->GetLevel())];
    WorldTravel::ErrandSnapshot snapshot;
    snapshot.Level = bot->GetLevel();
    snapshot.Money = bot->GetMoney();
    snapshot.Rates = WorldTravel::CurrentRates(measure, band);
    snapshot.BagHorizonHours = band.BagHorizonHours;
    snapshot.FreeSlots = freeSlots;
    snapshot.SellableSlots = sellableSlots;
    snapshot.MinutesToNextStop = static_cast<float>(config.ErrandNextStopMinutes);
    // The needs (one list, WU.4), each an errand provider's input; the stops that answer them carry its service
    snapshot.RepairDue = repairing;
    snapshot.DueStock = dueKinds;
    snapshot.OtherStock = unmetKinds;
    if (training)
        snapshot.Training = {training->units, training->levelsBehind, trainingDue, training->item};
    if (riding)
        snapshot.Riding = {riding->item, riding->vendor, riding->trainer != 0};
    // The nearest VENDOR_STOPS vendors within the walk limit, and as many more of those that answer a need
    std::sort(inReach.begin(), inReach.end(),
              [](std::pair<float, TravelMgr::VendorInfo const*> const& a,
                 std::pair<float, TravelMgr::VendorInfo const*> const& b) { return a.first < b.first; });
    std::size_t plainStops = 0;
    std::size_t needStops = 0;
    for (std::pair<float, TravelMgr::VendorInfo const*> const& vendor : inReach)
    {
        uint32 const entry = vendor.second->entry;
        auto const covered = cover.find(entry);
        uint8 const covers = covered != cover.end() ? covered->second : uint8(0);
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
        bool const repairs = creature && (creature->npcflag & UNIT_NPC_FLAG_REPAIR);
        // A stop answers a need only when it covers a due kind, repairs while a repair is due, or sells the mount
        bool const answers = (covers & dueKinds) || (repairing && repairs) ||
                             (snapshot.Riding.MountVendor && entry == snapshot.Riding.MountVendor);
        std::size_t& count = answers ? needStops : plainStops;
        if (count >= VENDOR_STOPS)
            continue;

        ++count;
        snapshot.Stops.push_back(
            {entry, ErrandPlaceOf(vendor.second->loc), WorldTravel::VendorServices(snapshot, entry, repairs, covers)});
    }
    if (index && training)
    {
        // A vendor stop with a class trainer within STOP_JOIN_YARDS takes training too, and the trainer part of the
        // stop goes to that trainer; the trainer's own spawns are stops as well
        std::vector<WorldTravel::ServiceStop> trainerStops;
        AddTrainerStops(TrainerSpawns(bot, *index, training->trainer, reachYards), WorldTravel::SERVICE_TRAINER,
                        trainerStops);
        WorldTravel::JoinTrainers(snapshot.Stops, trainerStops, WorldTravel::SERVICE_TRAINER);
        snapshot.Stops.insert(snapshot.Stops.end(), trainerStops.begin(), trainerStops.end());
    }
    if (index && riding && riding->trainer)
        AddTrainerStops(TrainerSpawns(bot, *index, riding->trainer, reachYards), WorldTravel::SERVICE_RIDING_TRAINER,
                        snapshot.Stops);
    AddCompletedQuests(bot, snapshot.Completed);

    // No stop the bot makes anyway yet (WT16's goals bring the route): every stop is a detour that must pay
    WorldTravel::TripRequest request;
    request.Start = {mapId, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    request.YardsPerSecond = runSpeed;
    request.Rates = snapshot.Rates;
    std::vector<WorldTravel::ErrandOffer> offers = WorldTravel::CollectOffers(snapshot, WorldTravel::TravelProviders());
    if (questing)
        offers.erase(std::remove_if(offers.begin(), offers.end(),
                                    [](WorldTravel::ErrandOffer const& offer) { return !offer.Due; }),
                     offers.end());
    WorldTravel::TripPlan const plan = WorldTravel::PlanTrip(request, snapshot.Stops, offers);

    std::vector<NewRpgInfo::ErrandStop> stops;
    float tripMinutes = 0.0f;
    std::size_t errandCount = 0;
    bool anyVendor = false;
    bool anyTrainer = false;
    std::array<bool, WorldTravel::ERRAND_KIND_COUNT> due{};  // a due need's errand is on the trip, by kind
    for (WorldTravel::TripStop const& tripStop : plan.Stops)
    {
        NewRpgInfo::ErrandStop stop;
        stop.pos = WorldPosition(tripStop.Place.MapId, tripStop.Place.X, tripStop.Place.Y, tripStop.Place.Z);
        if (tripStop.Services & WorldTravel::SERVICE_VENDOR)
        {
            stop.vendorEntry = tripStop.Id;
            anyVendor = true;
        }
        // The class trainer is visited at the one stop that took the training errand (the first trainer stop of the
        // trip); a riding trainer at each of its stops
        bool const trains =
            std::any_of(tripStop.Errands.begin(), tripStop.Errands.end(), [](WorldTravel::ErrandOffer const& errand)
                        { return errand.Kind == WorldTravel::ErrandKind::Train; });
        bool const classTrainer = (tripStop.Services & WorldTravel::SERVICE_TRAINER) && trains;
        if (classTrainer || (tripStop.Services & WorldTravel::SERVICE_RIDING_TRAINER))
        {
            stop.trainerEntry = tripStop.Trainer ? tripStop.Trainer : tripStop.Id;
            stop.trainerRiding = !classTrainer;
            anyTrainer = true;
        }
        for (WorldTravel::ErrandOffer const& errand : tripStop.Errands)
        {
            if (errand.Kind == WorldTravel::ErrandKind::TurnIn)
                stop.turnIns.push_back({errand.Id, errand.Target, errand.TargetIsObject});
            if (errand.Due)
                due[static_cast<std::size_t>(errand.Kind)] = true;
        }
        tripMinutes += tripStop.DetourMinutes;
        errandCount += tripStop.Errands.size();
        stops.push_back(std::move(stop));
    }

    // The world log's decision row: the sellable stacks and free slots, the trip's stops and errands it weighed
    WorldLogFormat::Alternatives weighed;
    if (WorldLog::Active())
        weighed = {{"sellable", static_cast<float>(sellableSlots)},
                   {"free_slots", static_cast<float>(snapshot.FreeSlots)},
                   {"stops", static_cast<float>(stops.size())},
                   {"errands", static_cast<float>(errandCount)}};
    if (stops.empty())
    {
        if (!nearest)
        {
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "skip", "no_vendor", 0.0f, weighed);
            return false;
        }

        LOG_DEBUG("playerbots",
                  "[New RPG] {} errands: trip not worth it (nearest vendor {} at {:.0f} yd, {:.1f} walk min, {} "
                  "sellable, {} free slots, {} errands refused)",
                  bot->GetName(), nearest->entry, nearestYards, walkMinutes, sellableSlots, snapshot.FreeSlots,
                  plan.Refused.size());
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "skip", "not_worth", walkMinutes * MINUTE,
                           weighed);
        return false;
    }

    botAI->rpgInfo.ChangeToErrands(std::move(stops), bot->GetMoney());
    auto const dueOnTrip = [&due](WorldTravel::ErrandKind kind) { return due[static_cast<std::size_t>(kind)]; };
    if (dueOnTrip(WorldTravel::ErrandKind::Repair))
    {
        uint32 const repairCopper = Planner::PriceRepair(bot).total;
        WorldLogFormat::Alternatives repairWeighed;
        if (WorldLog::Active())
            repairWeighed = {{"repair_copper", static_cast<float>(repairCopper)},
                             {"lowest_durability", static_cast<float>(Planner::ReadGearWear(bot).lowestPercent)},
                             {"sellable", static_cast<float>(sellableSlots)}};
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "repair", "due", static_cast<float>(repairCopper),
                           repairWeighed);
    }
    // The errand row's reason: the first due need on the trip in the fixed order
    char const* const reason = dueOnTrip(WorldTravel::ErrandKind::Repair)  ? "repair_due"
                               : dueOnTrip(WorldTravel::ErrandKind::Shop)  ? "stock_due"
                               : dueOnTrip(WorldTravel::ErrandKind::Train) ? "train_due"
                               : dueOnTrip(WorldTravel::ErrandKind::Ride)  ? "ride_due"
                                                                           : "worth_it";
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand,
                       anyVendor ? "vendor" : (anyTrainer ? "trainer" : "turn_in"), reason, tripMinutes * MINUTE,
                       weighed);
    // The errands the trip leaves out: the detour rule's refusals (detour_not_worth, too_far, no_stop)
    if (WorldLog::Active())
        for (WorldTravel::Refusal const& refusal : plan.Refused)
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand,
                               WorldTravel::ErrandKindName(refusal.Offer.Kind), refusal.Reason,
                               refusal.WalkMinutes * MINUTE,
                               {{"worth", refusal.Offer.WorthCopper}, {"cost", refusal.CostCopper}});
    return true;
}

bool NewRpgErrandsAction::Execute(Event /*event*/)
{
    auto* data = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data);
    if (!data)
        return false;

    if (data->stop >= data->stops.size())
    {
        FinishErrands(botAI, data->visited);
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

    // The vendor part of the stop; a vendor that is gone leaves the stop's other errands
    if (stop.vendorEntry && !data->vendorDone)
    {
        if (data->vendor.IsEmpty())
        {
            Creature* found = bot->FindNearestCreature(stop.vendorEntry, VENDOR_SEARCH_YARDS);
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
        else if (trainer && IsWithinInteractionDist(trainer))
            TrainAtTrainer(botAI, trainer);
        else
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "no_trainer", 0.0f, {}, entry);
        stop.trainerEntry = 0;
        data->visited = true;
        return true;
    }

    // The stop is done: on to the next
    ++data->stop;
    data->arrived = false;
    data->vendorDone = false;
    data->vendor = ObjectGuid();
    return true;
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    WorldLog::StatusReason const reasonScope("finished");
    info.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        bool completed = true;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // can't find a poi pos to go, stop doing quest for now
            WorldLog::StatusReason const reasonScope("no_poi");
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // A planner bot goes to the point nearest its plan's tasks for the quest, others to a random one
        std::vector<std::pair<float, float>> points;
        points.reserve(poiInfo.size());
        for (POIInfo const& poi : poiInfo)
            points.emplace_back(poi.pos.x, poi.pos.y);
        int const planned = Planner::PlannedPoi(botAI, questId, points);
        uint32 rndIdx = planned >= 0 ? static_cast<uint32>(planned) : urand(0, poiInfo.size() - 1);
        G3D::Vector2 nearestPoi = poiInfo[rndIdx].pos;
        int32 objectiveIdx = poiInfo[rndIdx].objectiveIdx;

        float dx = nearestPoi.x, dy = nearestPoi.y;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate — nudge the
        // bot a short distance so the next tick retries from a
        // different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // kill mobs and looting quest should be done automatically by grind strategy

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (!hasProgression)
        {
            // we has reach the poi for more than 5 mins but no progession
            // may not be able to complete this quest, marked as abandoned
            /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
            botAI->lowPriorityQuest.insert(questId);
            botAI->RecordQuestAbandoned(questId, QuestAbandonReason::NoProgress);
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            WorldLog::StatusReason const reasonScope("abandon");
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
    }

    // At the POI: keep the bot actively placed but avoid large
    // random 20yd hops that look like pacing back and forth. A small
    // ~8yd wander reads as the bot looking around while grind/loot
    // strategies do their work.
    return MoveRandomNear(8.0f);
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    Quest const* quest = data.quest;

    if (data.objectiveIdx != -1)
    {
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->RecordQuestCompleted(questId);
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
            WorldLog::StatusReason const reasonScope("no_poi");
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        assert(poiInfo.size() > 0);
        // now we get the place to get rewarded
        float dx = poiInfo[0].pos.x, dy = poiInfo[0].pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
        return false;

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward
    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // e.g. Can not reward quest to gameobjects
        /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
        botAI->lowPriorityQuest.insert(questId);
        botAI->RecordQuestAbandoned(questId, QuestAbandonReason::CannotTurnIn);
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        WorldLog::StatusReason const reasonScope("abandon");
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
}

namespace
{
// A route exists only once the graph is ready (the route service answers only then), and it never changes after
TravelRoute::Point GraphNodeAt(std::uint32_t index)
{
    TravelGraphNode const& node = TravelGraph::Instance().GetNode(index);
    return {node.mapId, node.x, node.y, node.z};
}

// Transport legs (WT19): the deck point is probed in steps of DECK_STEP_YARDS from the bot towards the transport's
// centre, only within DECK_PROBE_YARDS of it; the bot heads DECK_INWARD_YARDS past the deck's edge. The deck is the
// model's top surface from DECK_RISE_YARDS above to DECK_DROP_YARDS below the bot (WT19-e)
constexpr float DECK_STEP_YARDS = 2.0f;
constexpr float DECK_PROBE_YARDS = 100.0f;
constexpr float DECK_INWARD_YARDS = 4.0f;
constexpr float DECK_RISE_YARDS = 6.0f;
constexpr float DECK_DROP_YARDS = 12.0f;
constexpr float HOP_LIFT_YARDS = 0.5f;  // the hop lands this far above the deck point (as the off-mesh snap does)
// The walk off at the leg's end heads for the nearest ground (no water) this far beside and above or below the bot
constexpr float EXIT_ACROSS_YARDS = 25.0f;
constexpr float EXIT_VERTICAL_YARDS = 10.0f;

// The current leg is a transport leg past its approach: the bot waits at the dock, boards, rides or steps off. The
// follower scans for the transport then, and the trip's deadline waits (the next departure may be minutes away)
bool AtTransportLeg(TravelLegs::Trip const& trip)
{
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport &&
           trip.boarding != TravelLegs::Boarding::Approach;
}

// The leg's transport, found anew on every scan (never kept across ticks): the one the bot rides, else the nearest
// of the leg's entry to its dock on the bot's map. O(transports on the map), once a scan
Transport* FindLegTransport(Player* bot, TravelRoute::Leg const& leg)
{
    Transport* const ridden = bot->GetTransport();
    if (ridden && ridden->GetEntry() == leg.object)
        return ridden;
    Transport* best = nullptr;
    float bestYards = std::numeric_limits<float>::max();
    for (Transport* transport : bot->GetMap()->GetAllTransports())
    {
        if (!transport->IsInWorld() || transport->GetEntry() != leg.object)
            continue;
        float const yards = transport->GetExactDist2d(leg.from.x, leg.from.y);
        if (yards < bestYards)
        {
            best = transport;
            bestYards = yards;
        }
    }
    return best;
}

// The top of the transport's model at (x, y) from DECK_RISE_YARDS above z down to DECK_DROP_YARDS below it: the
// surface a bot stands on there. PlayerbotAI's passenger check casts from 2 yd above the bot, so a point at the bot's
// own height under a higher deck, or in the water below one, never makes it a passenger
std::optional<float> DeckHeight(Transport* transport, uint32 phaseMask, float x, float y, float z)
{
    if (!transport->m_model)
        return std::nullopt;
    float distance = DECK_RISE_YARDS + DECK_DROP_YARDS;
    G3D::Ray const ray(G3D::Vector3(x, y, z + DECK_RISE_YARDS), G3D::Vector3(0.0f, 0.0f, -1.0f));
    if (!transport->m_model->intersectRay(ray, distance, false, phaseMask, VMAP::ModelIgnoreFlags::Nothing))
        return std::nullopt;
    return z + DECK_RISE_YARDS - distance;
}

// Where the bot boards: decks are off the mesh, so a ship's or zeppelin's deck is probed from the bot towards its
// centre (DeckHeight) and the bot heads DECK_INWARD_YARDS past the edge, at the deck's height there; an elevator or
// tram car is boarded towards its centre. Without a hit (also within DECK_STEP_YARDS of it) the centre, at the deck's
// height there when the model has one within reach
Position DeckPoint(Player* bot, Transport* transport)
{
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    float const dx = transport->GetPositionX() - botX;
    float const dy = transport->GetPositionY() - botY;
    float const yards = std::sqrt(dx * dx + dy * dy);
    Position deck(transport->GetPositionX(), transport->GetPositionY(), z, bot->GetOrientation());
    if (transport->IsStaticTransport() || yards > DECK_PROBE_YARDS)
        return deck;
    uint32 const phaseMask = bot->GetPhaseMask();
    for (float along = DECK_STEP_YARDS; along < yards; along += DECK_STEP_YARDS)
    {
        float const part = along / yards;
        std::optional<float> const edge = DeckHeight(transport, phaseMask, botX + dx * part, botY + dy * part, z);
        if (!edge)
            continue;
        float const inward = std::min((along + DECK_INWARD_YARDS) / yards, 1.0f);
        float const x = botX + dx * inward;
        float const y = botY + dy * inward;
        deck.Relocate(x, y, DeckHeight(transport, phaseMask, x, y, *edge).value_or(*edge));
        return deck;
    }
    deck.m_positionZ = DeckHeight(transport, phaseMask, deck.GetPositionX(), deck.GetPositionY(), z).value_or(z);
    return deck;
}

// A ship or zeppelin at a stop it keeps for the attach's deadline (MotionTransport::IsMoving is private: its key
// frames, as MotionTransport::Update reads them)
bool Docked(MotionTransport const& ship)
{
    uint32 const period = ship.GetPeriod();
    if (!period)
        return false;
    uint32 const timer = ship.GetPathProgress() % period;
    for (KeyFrame const& frame : ship.GetKeyFrames())
        if (timer >= frame.ArriveTime && timer + PlayerbotAI::ABOARD_DEADLINE_MS < frame.DepartureTime)
            return true;
    return false;
}

// `route aboard` (tests): the bot is stood on the deck of the docked ship or zeppelin of that entry nearest it on its
// map, at the boarding deck point from where it stands (the test puts it on the dock first). No explicit attach:
// AddPassenger takes the current position as the offset, so PlayerbotAI's passenger check attaches it after the
// teleport (BeginAboard). Refused: absent (none on the map), not_mo (an elevator), away (not at a stop), no_deck
void PlaceAboard(PlayerbotAI* botAI, Player* bot, uint32 entry)
{
    Transport* transport = nullptr;
    float nearest = std::numeric_limits<float>::max();
    for (Transport* candidate : bot->GetMap()->GetAllTransports())
    {
        if (!candidate->IsInWorld() || candidate->GetEntry() != entry)
            continue;
        float const yards = candidate->GetExactDist2d(bot);
        if (yards < nearest)
        {
            transport = candidate;
            nearest = yards;
        }
    }
    MotionTransport const* ship = transport ? transport->ToMotionTransport() : nullptr;
    Position deck;
    char const* refusal = nullptr;
    if (!transport)
        refusal = "absent";
    else if (!ship)
        refusal = "not_mo";
    else if (!Docked(*ship))
        refusal = "away";
    else
    {
        deck = DeckPoint(bot, transport);
        if (bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), deck.GetPositionX(), deck.GetPositionY(),
                                              deck.GetPositionZ(), bot) != transport)
            refusal = "no_deck";
    }
    if (refusal)
    {
        LOG_INFO("playerbots", "[Route] {} aboard {} refused: {}", bot->GetName(), entry, refusal);
        std::string const source = std::string("refused:") + refusal;
        EventLog::Emit(bot, PlayerbotAI::ABOARD_EVENT_TYPE, source.c_str(), entry);
        return;
    }
    LOG_INFO("playerbots", "[Route] {} aboard {}: deck {:.1f}, {:.1f}, {:.1f}", bot->GetName(), entry,
             deck.GetPositionX(), deck.GetPositionY(), deck.GetPositionZ());
    bot->NearTeleportTo(deck.GetPositionX(), deck.GetPositionY(), deck.GetPositionZ() + HOP_LIFT_YARDS,
                        bot->GetOrientation());
    botAI->BeginAboard(entry);
}

// Board and Disembark: a straight walk (no path: decks are off the mesh), as FollowAction boards a master's ship.
// PlayerbotAI makes the bot a passenger of the transport it stands on, or none, within a second
void StraightMove(Player* bot, float x, float y, float z)
{
    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    bot->CastStop();
    bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false, false);
}

// The leave at the leg's end: its end node is the transport's own stop (its centre, on the hull or over the water), so
// the bot walks straight off to the nearest ground beside the deck. The first look per leave goes to the debug log,
// or to the world log as `Transport no_exit` when there is no ground. entry: the transport's gameobject entry
void WalkOff(Player* bot, TravelLegs::Trip& trip, uint32 entry)
{
    float x = bot->GetPositionX();
    float y = bot->GetPositionY();
    float z = bot->GetPositionZ();
    bool const found = NavMeshSnap::Nearest(bot->GetMap(), x, y, z, EXIT_ACROSS_YARDS, EXIT_VERTICAL_YARDS,
                                            NAV_GROUND) == NavMeshSnap::Result::Found;
    if (found)
        StraightMove(bot, x, y, z);
    if (trip.exitSought)
        return;
    trip.exitSought = true;
    if (!found)
        EventLog::Emit(bot, TravelLegs::TRANSPORT_EVENT_TYPE,
                       TravelLegs::TransportSource(TravelLegs::TransportEvent::NoExit), entry, 0);
    if (!found || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    Transport* const ridden = bot->GetTransport();
    bool const overHull = ridden && bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), x, y, z, nullptr) == ridden;
    LOG_DEBUG("playerbots", "[Route] {} leaves transport {}: ground {:.1f} yd away ({:.1f} yd up), {}", bot->GetName(),
              entry, bot->GetExactDist(x, y, z), z - bot->GetPositionZ(), overHull ? "under the hull" : "off the hull");
}

// An aboard scan short of the leave, for the debug log: what the dock test at the leg's end saw
void LogAboardScan(Player* bot, TravelLegs::Trip const& trip, float moved)
{
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    TravelLegs::TransportSight const& sight = trip.sight;
    float const dx = sight.at.x - leg.to.x;
    float const dy = sight.at.y - leg.to.y;
    LOG_DEBUG("playerbots",
              "[Route] {} aboard {} on map {}: {:.1f} yd (2D) from the leg's end on map {}, still {}, moving {}, "
              "static {}, {:.1f} yd moved since the last scan (-1: none)",
              bot->GetName(), leg.object, sight.at.mapId, std::sqrt(dx * dx + dy * dy), leg.to.mapId, trip.still,
              sight.moving, sight.isStatic, moved);
}

// The quay of a ship's or zeppelin's leg (WT19-e, TravelLegs::SetQuay): the nearest ground (no water) beside its dock
// node, as the walk off at the end looks for it. Once a leg, on the leg's map while approaching; a nav mesh tile not
// loaded yet is looked at again a scan later. Elevators and the tram wait at their stop node
void SeekQuay(Player* bot, NewRpgInfo::Travel& data, uint32 nowMs)
{
    TravelLegs::Trip& trip = data.trip;
    if (trip.legIdx >= trip.legs.size() || trip.quaySought || trip.boarding != TravelLegs::Boarding::Approach)
        return;
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    if (leg.type != TravelRoute::LegType::Transport || bot->GetMapId() != leg.from.mapId ||
        getMSTimeDiff(data.transportScanMs, nowMs) < TravelLegs::TRANSPORT_SCAN_MS)
        return;
    data.transportScanMs = nowMs;
    GameObjectTemplate const* const info = sObjectMgr->GetGameObjectTemplate(leg.object);
    if (!info || info->type != GAMEOBJECT_TYPE_MO_TRANSPORT)
    {
        TravelLegs::SetQuay(trip, nullptr);
        return;
    }
    float x = leg.from.x;
    float y = leg.from.y;
    float z = leg.from.z;
    NavMeshSnap::Result const result =
        NavMeshSnap::Nearest(bot->GetMap(), x, y, z, EXIT_ACROSS_YARDS, EXIT_VERTICAL_YARDS, NAV_GROUND);
    if (result == NavMeshSnap::Result::Unknown)
    {
        TravelLegs::NoteQuayUnknown(trip);
        return;
    }
    TravelRoute::Point const quay{leg.from.mapId, x, y, z};
    TravelLegs::SetQuay(trip, result == NavMeshSnap::Result::Found ? &quay : nullptr);
    if (!sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    if (trip.hasQuay)
        LOG_DEBUG("playerbots",
                  "[Route] {} waits for transport {} on the ground {:.1f} yd from its stop ({:.1f} yd up)",
                  bot->GetName(), leg.object, TravelRoute::Distance(leg.from, quay), z - leg.from.z);
    else
        LOG_DEBUG("playerbots", "[Route] {} waits for transport {} at its stop: no ground beside it", bot->GetName(),
                  leg.object);
}

// A Board walk, for the debug log: the deck point, the bot's height and the distance
void LogBoardMove(Player* bot, uint32 entry, Position const& deck)
{
    if (!sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    LOG_DEBUG("playerbots",
              "[Route] {} boards transport {}: deck point ({:.1f}, {:.1f}, {:.1f}), bot z {:.1f}, {:.1f} yd",
              bot->GetName(), entry, deck.GetPositionX(), deck.GetPositionY(), deck.GetPositionZ(), bot->GetPositionZ(),
              bot->GetExactDist(deck));
}

// A miss this tick, for the debug log: whether the bot stood over the hull (the passenger check's test) or beside it
void LogMisses(Player* bot, TravelLegs::Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    for (TravelLegs::TransportNote const& note : trip.transportNotes)
    {
        if (note.event != TravelLegs::TransportEvent::Missed)
            continue;
        Transport* const transport = FindLegTransport(bot, trip.legs[trip.legIdx]);
        bool const onHull = transport && bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), bot->GetPositionX(),
                                                                           bot->GetPositionY(), bot->GetPositionZ(),
                                                                           nullptr) == transport;
        LOG_DEBUG("playerbots", "[Route] {} missed transport {} (miss {}): {}, {:.1f} yd from its centre, bot z {:.1f}",
                  bot->GetName(), note.entry, note.amount, onHull ? "over the hull" : "off the hull",
                  transport ? bot->GetExactDist2d(transport) : -1.0f, bot->GetPositionZ());
        return;
    }
}

// A scan of the transport: where it is, whether a ship or zeppelin moved since the scan before (trip.sight), its
// period. Returns the yards moved since that scan on this map, -1 none
float SeeTransport(Player* bot, Transport* transport, TravelLegs::Trip const& trip, TravelLegs::TransportSight& sight)
{
    float moved = -1.0f;
    sight.found = true;
    sight.isStatic = transport->IsStaticTransport();
    sight.at = {bot->GetMapId(), transport->GetPositionX(), transport->GetPositionY(), transport->GetPositionZ()};
    TravelLegs::TransportSight const& before = trip.sight;
    if (before.found && before.at.mapId == sight.at.mapId)
        moved = TravelRoute::Distance(before.at, sight.at);
    if (MotionTransport const* ship = transport->ToMotionTransport())
    {
        // MotionTransport::IsMoving is private: a ship or zeppelin is under way when it moved since the scan before
        // (on the first scan it counts as under way, so no docking is read from one position)
        sight.moving = moved < 0.0f || moved > TravelLegs::STILL_YARDS;
        sight.periodMs = ship->GetPeriod();
    }
    else if (StaticTransport const* fixed = transport->ToStaticTransport())
        sight.periodMs = fixed->GetPeriod();
    return moved;
}

void SendTransportNotes(Player* bot, TravelLegs::Trip& trip)
{
    for (TravelLegs::TransportNote const& note : trip.transportNotes)
        EventLog::Emit(bot, TravelLegs::TRANSPORT_EVENT_TYPE, TravelLegs::TransportSource(note.event), note.entry,
                       note.amount);
    trip.transportNotes.clear();
}

// The gate before a trip's end, a route request or a walk (WT19-d, TravelLegs::BeginOffFirst): true when the bot
// rides a ship or zeppelin and gets off first (`Transport off_first`)
bool OffFirstBegins(Player* bot, TravelLegs::Trip& trip, TravelLegs::OffFirstEnd end = TravelLegs::OffFirstEnd::None,
                    char const* why = nullptr, char const* source = nullptr)
{
    Transport* const ridden = bot->GetTransport();
    MotionTransport const* const ship = ridden ? ridden->ToMotionTransport() : nullptr;
    if (!ship || !TravelLegs::BeginOffFirst(trip, ship->GetEntry(), ship->GetPeriod(), getMSTime(), end, why, source))
        return false;
    SendTransportNotes(bot, trip);
    char const* const waiting = end == TravelLegs::OffFirstEnd::Arrive  ? "arrival"
                                : end == TravelLegs::OffFirstEnd::Abort ? "abort"
                                                                        : "walk or route";
    LOG_DEBUG("playerbots", "[Route] {} gets off transport {} first (the {} waits)", bot->GetName(), ship->GetEntry(),
              waiting);
    return true;
}

// One tick of the get-off (TravelLegs::StepOffFirst): the ridden ship's scan once a TRANSPORT_SCAN_MS, the walk off
// to the nearest ground on a scan while it stands
TravelLegs::OffStep GetOffFirst(Player* bot, NewRpgInfo::Travel& data)
{
    TravelLegs::Trip& trip = data.trip;
    Transport* const ridden = bot->GetTransport();
    TravelLegs::Observation seen;
    seen.nowMs = getMSTime();
    seen.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    seen.aboard = ridden ? ridden->GetEntry() : 0;
    if (getMSTimeDiff(data.transportScanMs, seen.nowMs) >= TravelLegs::TRANSPORT_SCAN_MS)
    {
        data.transportScanMs = seen.nowMs;
        seen.transport.scanned = true;
        if (ridden && ridden->GetEntry() == trip.offFirst.entry)
            SeeTransport(bot, ridden, trip, seen.transport);
    }
    TravelLegs::OffStep const step = TravelLegs::StepOffFirst(trip, seen);
    SendTransportNotes(bot, trip);
    if (step == TravelLegs::OffStep::Disembark && seen.transport.scanned)
        WalkOff(bot, trip, trip.offFirst.entry);
    return step;
}

// The last-resort hop (WT.14): a few yards onto the docked deck, only unseen (the scan's sight check), counted (its
// Transport hop event went with the trip's notes; the honesty row for `wlog report honesty`)
void HopOnto(Player* bot, Position const& deck, uint32 entry, uint32 misses)
{
    bot->GetMotionMaster()->Clear();
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    bot->TeleportTo(bot->GetMapId(), deck.GetPositionX(), deck.GetPositionY(), deck.GetPositionZ() + HOP_LIFT_YARDS,
                    bot->GetOrientation());
    EventLog::Honesty(bot, TravelLegs::HOP_HONESTY_KIND, TravelLegs::HOP_HONESTY_SOURCE, entry, misses);
}

// A walk leg's end (WT.23): its trip decision row, walk_leg when done or walk_leg_failed (how the bot went: foot, part
// or mounted; its seconds in combat; its seconds against the estimate), and in the debug log its speed: its path
// through the waypoints, its straight line, the time taken
void LogWalkLeg(Player* bot, NewRpgInfo::Travel const& data, uint32 index, TravelLegs::LegRecord const& record,
                uint32 nowMs, uint32 elapsedMs, char const* how, bool done)
{
    TravelRoute::Leg const& leg = data.trip.legs[index];
    float const seconds = static_cast<float>(elapsedMs) / 1000.0f;
    float const combat = TravelLegs::CombatSeconds(record, nowMs);
    WorldLog::Decision(
        bot, WorldLogFormat::DecisionKind::Trip, done ? "walk_leg" : "walk_leg_failed", TravelLegs::MountWord(record),
        combat, {{"seconds", seconds}, {"estimate", leg.seconds}, {"leg", static_cast<float>(index + 1)}}, data.tripId);
    if (!sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    std::vector<TravelLegs::Waypoint> const points = TravelLegs::Waypoints(leg, GraphNodeAt);
    float path = 0.0f;
    for (size_t i = 1; i < points.size(); ++i)
        path += TravelRoute::Distance(points[i - 1].at, points[i].at);
    TravelRoute::Point const at{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    LOG_DEBUG("playerbots",
              "[Route] {} walk leg {}/{} {}: {:.0f} yd straight, {:.0f} yd path in {:.0f} s ({:.1f} yd/s), estimate "
              "{:.0f} s, {:.0f} yd from its end, {} ({} of {} ticks mounted), {:.0f} s in combat",
              bot->GetName(), index + 1, data.trip.legs.size(), how, TravelRoute::Distance(leg.from, leg.to), path,
              seconds, seconds > 0.0f ? path / seconds : 0.0f, leg.seconds, TravelRoute::Distance(at, leg.to),
              TravelLegs::MountWord(record), record.mountedTicks, record.ticks, combat);
}

constexpr float FLY_SAMPLE_YARDS = 50.0f;     // the real-height check samples the ground ahead this far apart
constexpr float LANDING_PROBE_YARDS = 5.0f;   // the landing check searches the ground down from this above the landing
constexpr float LANDING_PROBE_DEPTH = 40.0f;  // and this far down
constexpr uint32 FLY_SAMPLES_MAX = 8;         // and at most this often per waypoint

// A fly leg's end (WU10): its trip rows, fly_leg when done or fly_leg_failed (as walk_leg: how the bot went, its
// seconds in combat, its seconds against the estimate; a failed takeoff's row has why instead of how the bot went),
// then fly_height (raised when the real-height check lifted a waypoint, else planned): the highest z flown, against
// the plan's highest z, its levels and the raises
void LogFlyLeg(Player* bot, NewRpgInfo::Travel const& data, uint32 index, TravelLegs::LegRecord const& record,
               uint32 nowMs, uint32 elapsedMs, uint16 climbs, float peakZ, char const* how, bool done,
               char const* why = nullptr)
{
    TravelRoute::Leg const& leg = data.trip.legs[index];
    float const seconds = static_cast<float>(elapsedMs) / 1000.0f;
    float const combat = TravelLegs::CombatSeconds(record, nowMs);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, done ? "fly_leg" : "fly_leg_failed",
                       why ? why : TravelLegs::MountWord(record), combat,
                       {{"seconds", seconds}, {"estimate", leg.seconds}, {"leg", static_cast<float>(index + 1)}},
                       data.tripId);
    float plannedZ = leg.to.z;
    for (TravelRoute::Point const& point : leg.flightPoints)
        plannedZ = std::max(plannedZ, point.z);
    WorldLog::Decision(
        bot, WorldLogFormat::DecisionKind::Trip, "fly_height", climbs ? "raised" : "planned", peakZ,
        {{"max_z", plannedZ}, {"levels", static_cast<float>(leg.flightLevels)}, {"climbs", static_cast<float>(climbs)}},
        data.tripId);
    LOG_DEBUG("playerbots",
              "[Route] {} fly leg {}/{} {}: {:.0f} yd straight, {:.0f} yd planned at {} levels up to z {:.0f}, flown "
              "up to z {:.0f} with {} raises in {:.0f} s, estimate {:.0f} s, {:.0f} s in combat",
              bot->GetName(), index + 1, data.trip.legs.size(), how, TravelRoute::Distance(leg.from, leg.to),
              leg.flightYards, leg.flightLevels, plannedZ, peakZ, climbs, seconds, leg.seconds, combat);
}

// The real-height check's samples (WU10): the ground between the bot and its next waypoint in the air, buildings
// included (vmap), searched down from far above the flight line so a roof above it counts too
std::vector<float> SampleFlightGround(Player* bot, TravelRoute::Point const& target)
{
    float const fromX = bot->GetPositionX();
    float const fromY = bot->GetPositionY();
    float const yards = std::hypot(target.x - fromX, target.y - fromY);
    uint32 const count = std::clamp<uint32>(static_cast<uint32>(yards / FLY_SAMPLE_YARDS) + 1, 1, FLY_SAMPLES_MAX);
    float const top = std::max(target.z, bot->GetPositionZ()) + TravelLegs::FLY_RAISE_CAP_YARDS;
    std::vector<float> samples;
    samples.reserve(count);
    for (uint32 i = 1; i <= count; ++i)
    {
        float const share = static_cast<float>(i) / static_cast<float>(count);
        samples.push_back(bot->GetMapHeight(fromX + (target.x - fromX) * share, fromY + (target.y - fromY) * share, top,
                                            true, 2.0f * TravelLegs::FLY_RAISE_CAP_YARDS));
    }
    return samples;
}

// The landing's open-sky check (TravelLegs::NeedsLandingCheck): each point of the landing line on the ground below it,
// searched down from LANDING_PROBE_YARDS above the landing's height (a roof higher up is not the ground there), and
// outdoors there by the map's terrain status (WMO areas count). True when the landing moved (TravelLegs::MoveLanding)
bool CheckLanding(Player* bot, TravelLegs::Trip& trip)
{
    Map* map = bot->GetMap();
    std::vector<TravelRoute::Point> line = TravelLegs::LandingLine(trip);
    if (!map || line.empty())
        return TravelLegs::MoveLanding(trip, trip.waypoints.empty() ? TravelRoute::Point{} : trip.waypoints.back().at);
    uint32 const phase = bot->GetPhaseMask();
    for (TravelRoute::Point& point : line)
        point.z = map->GetHeight(phase, point.x, point.y, point.z + LANDING_PROBE_YARDS, true, LANDING_PROBE_DEPTH);
    float const collision = bot->GetCollisionHeight();
    auto const outdoors = [&](TravelRoute::Point const& point)
    {
        if (point.z <= INVALID_HEIGHT)
            return false;
        PositionFullTerrainStatus status;
        map->GetFullTerrainStatusForPosition(phase, point.x, point.y, point.z, collision, status);
        return status.outdoors;
    };
    std::size_t const index = TravelLegs::LandingIndex(line, outdoors);
    TravelRoute::Point const planned = trip.waypoints.back().at;
    if (!TravelLegs::MoveLanding(trip, index + 1 == line.size() ? planned : line[index]))
        return false;
    LOG_DEBUG("playerbots",
              "[Route] {} landing at ({:.0f}, {:.0f}, {:.0f}) is indoors: lands {:.0f} yd back at ({:.0f}, {:.0f}, "
              "{:.0f}) and walks in",
              bot->GetName(), planned.x, planned.y, planned.z, TravelRoute::Distance(planned, line[index]),
              line[index].x, line[index].y, line[index].z);
    return true;
}

// A fly leg's mount (WU10): off a ground mount, then the bot's fastest flying mount (CheckMountStateAction::
// CastFlyingMount; the engine's "check mount state" refused a bot standing still on open ground, WU11b). A try in
// combat, while casting or where the bot cannot take off is skipped and does not count (the follower asks again; the
// last fails the leg at once); so is a try while the bot moves or sits: the cast checks refuse it in the tick of the
// stop (WU11b: Edhoon's "cast result 255, moving true"), so it stops or stands up now and casts on a later tick. A
// counted try that starts no cast leaves its word for the fly_leg_failed row (no_strategy: no mount strategy;
// no_mount: no flying mount known; refused: the cast check refused every one)
bool MountToFly(Player* bot, PlayerbotAI* botAI, TravelLegs::Trip& trip, uint32 nowMs)
{
    if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(true) || CheckMountStateAction::NoTakeoffHere(bot))
        return true;
    if (bot->isMoving() || !bot->IsStandState())
    {
        if (bot->isMoving())
            bot->StopMoving();
        if (!bot->IsStandState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        return true;
    }
    TravelLegs::NoteMountTry(trip, nowMs);
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    if (!botAI->HasStrategy("mount", BOT_STATE_NON_COMBAT))
        trip.mountWhy = "no_strategy";
    else
        trip.mountWhy = CheckMountStateAction::CastFlyingMount(bot, botAI);
    return true;
}

// Why a fly leg's takeoff failed (the fly_leg_failed row's word): where the bot stands (NoTakeoffHere's words), else
// the last try's refusal, else what the tries left: a ground mount (ground_mount) or none after the cast (no_mount)
char const* TakeoffWord(Player* bot, TravelLegs::Trip const& trip)
{
    if (char const* here = CheckMountStateAction::NoTakeoffHere(bot))
        return here;
    if (*trip.mountWhy)
        return trip.mountWhy;
    return bot->IsMounted() ? "ground_mount" : "no_mount";
}

// The world log's route decision (only when it is on): the leg chain without no-op walks, why it was searched,
// its seconds; weighed against a straight walk at run speed (when the chain is not one walk on the same map) and its
// leg count. The route service returns one route, so no other route is searched for the row.
void LogRouteDecision(Player* bot, NewRpgInfo::Travel const& data, TravelRoute::Result const& route)
{
    WorldLogFormat::WordChain chain;
    uint32 legs = 0;
    bool walksOnly = true;
    for (TravelRoute::Leg const& leg : route.legs)
    {
        if (TravelLegs::IsNoOp(leg))
            continue;
        chain.Add(TravelRoute::LegTypeWord(leg.type));
        ++legs;
        walksOnly = walksOnly && leg.type == TravelRoute::LegType::Walk;
    }
    WorldLogFormat::Alternatives alternatives;
    if (!(legs == 1 && walksOnly) && bot->GetMapId() == data.dest.GetMapId())
    {
        float const yards =
            bot->GetExactDist(data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ());
        alternatives.Add({"walk_direct", WorldLogFormat::WalkSeconds(yards, bot->GetSpeed(MOVE_RUN))});
    }
    alternatives.Add({"legs", static_cast<float>(legs)});
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Route, legs ? chain.View() : "here",
                       data.trip.replans || data.trip.spotFails ? "replan" : "search", route.totalSeconds, alternatives,
                       data.tripId);
}

// The bot's place as the travel rules see it
TravelRoute::Point BotPoint(Player* bot)
{
    return {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
}

// Mounted with a flight aura (TravelLegs::Observation::flyingMount)
bool OnFlyingMount(Player* bot)
{
    return bot->IsMounted() && (bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura());
}

// A done or failed walk or fly leg's time into the bot's travel shares (upkeep WU14, the riding ranks' worth); the
// area where it ended tells whether a flying mount would fly there (FlightMap's open areas)
void NoteTravelShares(PlayerbotAI* botAI, Player* bot, TravelRoute::LegType type, uint32 legMs, uint32 nowMs)
{
    constexpr uint32 SPELL_COLD_WEATHER_FLYING = 54197;
    WorldTravel::LegKind const kind = type == TravelRoute::LegType::Walk  ? WorldTravel::LegKind::Walk
                                      : type == TravelRoute::LegType::Fly ? WorldTravel::LegKind::Fly
                                                                          : WorldTravel::LegKind::Other;
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId());
    bool const open = area && area->IsFlyable() && !(area->flags & AREA_FLAG_NO_FLY_ZONE) &&
                      area->ID != AREA_WINTERGRASP && area->zone != AREA_WINTERGRASP;
    uint8 const shares = WorldTravel::LegShares(kind, bot->GetMapId(), open, bot->HasSpell(SPELL_COLD_WEATHER_FLYING));
    WorldTravel::SlotMeasure& measure =
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    WorldTravel::NoteTravelLeg(measure, shares, legMs, nowMs);
}

}  // namespace

bool NewRpgTravelAction::Execute(Event /*event*/)
{
    auto* data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (!data)
        return false;
    // A trip end, route request or walk that came while the bot rode a ship or zeppelin waits until it is off (or a
    // period passed); then it goes on (WT19-d)
    if (data->trip.offFirst.sinceMs)
    {
        TravelLegs::OffStep const step = GetOffFirst(bot, *data);
        if (step == TravelLegs::OffStep::Wait || step == TravelLegs::OffStep::Disembark)
            return true;
        TravelLegs::OffFirst const& off = data->trip.offFirst;
        if (off.end == TravelLegs::OffFirstEnd::Arrive)
            return Arrive(*data);
        if (off.end == TravelLegs::OffFirstEnd::Abort)
            EndTrip(*data, off.why, off.source);
        return true;
    }
    // A trip never ends in the air: on a taxi, on its own fly leg or on the flight down after one it goes past the
    // deadline. Aboard or at a transport's dock the deadline waits; the leg's end takes that time off the route (below)
    bool const flyLeg = data->trip.legIdx < data->trip.legs.size() &&
                        data->trip.legs[data->trip.legIdx].type == TravelRoute::LegType::Fly;
    Transport* const ridden = bot->GetTransport();
    bool const riding = bot->IsInFlight() || (flyLeg && OnFlyingMount(bot)) || ridden || AtTransportLeg(data->trip) ||
                        data->trip.descending;
    bool const replanPending = data->failedMs && data->trip.legs.empty();
    if (TravelLegs::PastDeadline(GetMSTimeDiffToNow(data->routeStartMs), data->routeSeconds, riding, replanPending))
    {
        EndTrip(*data, "deadline", "abort:deadline");
        return true;
    }
    // Killed twice on the leg or three times on the trip (PlayerbotAI::ChangeEngine counts it, the follower runs
    // again once the bot is back): the leg fails, its edge is avoided; the third death ends the trip
    if (TravelLegs::DiedTooOften(data->trip))
        return LegFailed(*data, TravelLegs::FailReason::Death);
    // A fly leg failed in the air: down to the floor before the replan
    if (data->trip.descending)
        return FlyDown(*data, TravelLegs::StepDescent(data->trip, BotPoint(bot), OnFlyingMount(bot), getMSTime()));
    if (data->route.waiting)
        return TakeRoute(*data);
    if (data->trip.legs.empty())
    {
        // The trip's start, or a new route once the failed leg's stuck report has reached the published book
        if (data->failedMs && GetMSTimeDiffToNow(data->failedMs) < TravelLegs::REPLAN_DELAY_MS)
            return true;
        // Never planned from a ship's deck: the bot gets off first
        if (OffFirstBegins(bot, data->trip))
            return true;
        return RequestRoute(*data);
    }
    // MoveFarTo found the bot off the mesh and started its recovery (hearth or a short move): the trip ends
    if (data->offMesh)
        return LegFailed(*data, TravelLegs::FailReason::OffMesh);

    TravelLegs::Observation seen;
    seen.bot = BotPoint(bot);
    seen.inFlight = bot->IsInFlight();
    seen.flyingMount = OnFlyingMount(bot);
    // Flying flags count only on the flying mount: a dismount in the air can leave them (no client sends the landing)
    seen.flying = seen.flyingMount && bot->IsFlying() && !seen.inFlight;
    // Before a fly leg's takeoff: may the bot mount a flyer and lift off here (a no-fly spot fails the leg at once).
    // Read also on the walk leg before it, so the tick that ends the walk and starts the flight already knows
    auto const isFly = [&](size_t idx)
    { return idx < data->trip.legs.size() && data->trip.legs[idx].type == TravelRoute::LegType::Fly; };
    if (!data->trip.seenInFlight && (isFly(data->trip.legIdx) || isFly(data->trip.legIdx + 1)))
        seen.takeoffHere = !CheckMountStateAction::NoTakeoffHere(bot);
    seen.money = bot->GetMoney();
    // A passenger does not walk: no give-up of an earlier walk counts against the ride
    if (ridden)
        data->stuck = false;
    seen.stuck = data->stuck;
    seen.nowMs = getMSTime();
    seen.aboard = ridden ? ridden->GetEntry() : 0;
    TravelLegs::Trip const& trip = data->trip;
    SeekQuay(bot, *data, seen.nowMs);
    // The transport scan (WT19): once a TRANSPORT_SCAN_MS while a transport leg is past its approach, on the bot's
    // map only. The deck point and the Transport* live for this tick only
    std::optional<Position> deck;
    float moved = -1.0f;  // the transport's yards since the scan before on this map; -1 none
    if (AtTransportLeg(trip) && getMSTimeDiff(data->transportScanMs, seen.nowMs) >= TravelLegs::TRANSPORT_SCAN_MS)
    {
        data->transportScanMs = seen.nowMs;
        TravelLegs::TransportSight& sight = seen.transport;
        sight.scanned = true;
        if (Transport* const transport = FindLegTransport(bot, trip.legs[trip.legIdx]))
        {
            moved = SeeTransport(bot, transport, trip, sight);
            if (!ridden && !sight.moving)
            {
                deck = DeckPoint(bot, transport);
                if (TravelLegs::WantsSightCheck(trip))
                    sight.unseen = !sRandomPlayerbotMgr.IsTeleportSeen(
                        bot, WorldPosition(bot->GetMapId(), deck->GetPositionX(), deck->GetPositionY(),
                                           deck->GetPositionZ()));
            }
        }
    }
    TravelLegs::NoteTick(data->trip, bot->IsMounted());
    TravelLegs::LegRecord const recordBefore = trip.record;  // Advance starts a new leg's record
    uint32 const legBefore = trip.legIdx;
    uint32 const legStartBefore = trip.legStartMs;
    uint16 const climbsBefore = trip.climbs;
    float const peakBefore = trip.peakZ;
    uint32 const dockSinceBefore = trip.waitSinceMs;  // a transport leg: the bot's arrival at the dock
    TravelLegs::Decision next = TravelLegs::Advance(data->trip, seen, GraphNodeAt);
    LogMisses(bot, trip);
    SendTransportNotes(bot, data->trip);
    if (seen.transport.scanned && ridden && trip.legIdx == legBefore && trip.boarding == TravelLegs::Boarding::Ride &&
        sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        LogAboardScan(bot, trip, moved);
    // The legs passed this tick (no-op walks are no legs to a reader): id the trip, amount the leg's seconds
    for (uint32 i = legBefore; i < trip.legIdx && i < trip.legs.size(); ++i)
    {
        if (TravelLegs::IsNoOp(trip.legs[i]))
            continue;
        uint32 const seconds = i == legBefore ? (seen.nowMs - legStartBefore) / IN_MILLISECONDS : 0;
        EventLog::Emit(bot, "Travel", TravelLegs::LegDoneSource(trip.legs[i].type), data->tripId, seconds);
        if (i == legBefore &&
            (trip.legs[i].type == TravelRoute::LegType::Walk || trip.legs[i].type == TravelRoute::LegType::Fly))
            NoteTravelShares(botAI, bot, trip.legs[i].type, seen.nowMs - legStartBefore, seen.nowMs);
        if (i == legBefore && trip.legs[i].type == TravelRoute::LegType::Walk)
            LogWalkLeg(bot, *data, i, recordBefore, seen.nowMs, seen.nowMs - legStartBefore, "done", true);
        if (i == legBefore && trip.legs[i].type == TravelRoute::LegType::Fly)
        {
            LogFlyLeg(bot, *data, i, recordBefore, seen.nowMs, seen.nowMs - legStartBefore, climbsBefore, peakBefore,
                      "done", true);
            CheckMountStateAction::LandAndDismount(bot);
        }
        // The deadline waited at the dock and aboard: that time does not count against the route's estimate
        if (i == legBefore && trip.legs[i].type == TravelRoute::LegType::Transport)
            data->routeStartMs += seen.nowMs - dockSinceBefore;
    }
    // Once per waypoint in the air: the real ground ahead (the flight map has no buildings) may lift it
    if (next.act == TravelLegs::Act::Fly && TravelLegs::NeedsHeightCheck(trip))
    {
        TravelLegs::CheckHeight(data->trip, SampleFlightGround(bot, trip.waypoints[trip.next].at));
        next.target = trip.waypoints[trip.next].at;
    }
    // Once per fly leg near its end: an indoor landing (an inn's floor under its roof) moves to the outdoor spot
    // nearest to it back along the flight, and the bot walks in from there
    if (next.act == TravelLegs::Act::Fly && TravelLegs::NeedsLandingCheck(trip, seen.bot) &&
        CheckLanding(bot, data->trip))
        next.target = trip.waypoints[trip.next].at;
    // For the case's hold line: what the follower did last, and when
    data->trip.lastAct = next.act;
    data->trip.lastTarget = next.target;
    data->trip.lastActMs = seen.nowMs;
    switch (next.act)
    {
        case TravelLegs::Act::Arrived:
            return Arrive(*data);
        case TravelLegs::Act::Walk:
            // Not from a ship's deck: the bot gets off first
            if (OffFirstBegins(bot, data->trip))
                return true;
            return MoveFarTo(WorldPosition(next.target.mapId, next.target.x, next.target.y, next.target.z));
        case TravelLegs::Act::UseTrigger:
            return UsePortal(*data);
        case TravelLegs::Act::TalkFlightMaster:
            return TalkFlightMaster(*data);
        case TravelLegs::Act::UseHearth:
            return UseHearth(*data);
        case TravelLegs::Act::Mount:
            return MountToFly(bot, botAI, data->trip, seen.nowMs);
        case TravelLegs::Act::Fly:
            // On the flying mount MoveTo gives the bot the flying flags and a straight line to the point, no path;
            // a move still running keeps the follower in charge
            MoveTo(next.target.mapId, next.target.x, next.target.y, next.target.z, false, false, false, true,
                   MovementPriority::MOVEMENT_NORMAL, true);
            return true;
        case TravelLegs::Act::Ride:
            // As the random flight status does: nothing else to do in the air
            ContinueCrossMapTaxi();
            return false;
        case TravelLegs::Act::Board:
            // Towards the deck point of this tick's scan; a new walk only once the last one ended
            if (deck && TravelLegs::WantsBoardMove(trip, !bot->movespline->Finalized()))
            {
                data->trip.boardMoveSent = true;
                LogBoardMove(bot, trip.legs[trip.legIdx].object, *deck);
                StraightMove(bot, deck->GetPositionX(), deck->GetPositionY(), deck->GetPositionZ());
            }
            return true;
        case TravelLegs::Act::Hop:
            // Only on a scan tick (it needs the sight check), so the deck point is this tick's
            if (deck)
                HopOnto(bot, *deck, trip.legs[trip.legIdx].object, trip.misses);
            return true;
        case TravelLegs::Act::Disembark:
            // Once a scan: the transport stands at the leg's end, the bot walks straight off onto the ground beside it
            if (seen.transport.scanned)
                WalkOff(bot, data->trip, trip.legs[trip.legIdx].object);
            return true;
        case TravelLegs::Act::Fail:
            return LegFailed(*data, next.reason);
        case TravelLegs::Act::Descend:  // StartDescent's and StepDescent's only (FlyDown)
        case TravelLegs::Act::Fall:
        case TravelLegs::Act::Wait:
        case TravelLegs::Act::Done:
            return true;
    }
    return false;
}

bool NewRpgTravelAction::RequestRoute(NewRpgInfo::Travel& data)
{
    TravelRoute::Snapshot snapshot = TravelRoute::FillSnapshot(bot, ++botAI->rpgInfo.routeRequests);
    // Transports run (WT19; a timed-out transport leg's link is left out through avoidEdges). Flights only to
    // discovered nodes (the snapshot's taxi mask), their fares within the purse (the search's fare budget). The
    // hearthstone is a leg while its stone is in the bags and it is ready or cooling for at most the wait cap (the
    // search); not again after a hearth leg of this trip failed
    snapshot.hasHearthstone =
        snapshot.hasHearthstone && !data.noHearth && TestConstraints::HearthAllowed(botAI->rpgInfo.constraints);
    snapshot.allowTransport = true;
    TravelRoute::Request request;
    request.snapshot = snapshot;
    request.goal = {data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ()};
    // The trip's stuck walks: their start or goal hops are left out (TravelLegs::NoteStuck)
    request.avoidNodes = data.trip.avoid;
    request.noDirect = data.trip.noDirect;
    // The walks this bot died on twice (TravelLegs::NoteDeath): their graph edges are left out, for its trip only
    request.avoidEdges = data.trip.avoidEdges;
    // After a failed fly leg the trip's later routes walk (TravelLegs::AfterFailure)
    request.noFly = data.trip.noFly;
    if (!TravelRoute::RouteService::Instance().Submit(bot->GetGUID().GetRawValue(), request,
                                                      TravelRoute::ConfiguredParams()))
    {
        EndTrip(data, "no route service", "abort:no_service");  // the graph is not loaded yet: never wait
        return true;
    }
    data.route = {snapshot.requestId, snapshot.position.mapId, snapshot.takenMs, true};
    data.stuck = false;
    return true;
}

bool NewRpgTravelAction::TakeRoute(NewRpgInfo::Travel& data)
{
    uint32 const now = getMSTime();
    std::optional<TravelRoute::Answer> answer =
        TravelRoute::RouteService::Instance().TryTake(bot->GetGUID().GetRawValue());
    // A late or lost answer asks again from here, a few times per trip, before it fails the leg
    auto const askAgain = [&](char const* why)
    {
        if (!TravelLegs::Resubmit(data.trip))
            return LegFailed(data, TravelLegs::FailReason::NoAnswer);
        LOG_DEBUG("playerbots", "[Route] {} {} ({} ms since the request), asked again", bot->GetName(), why,
                  now - data.route.requestedMs);
        return RequestRoute(data);
    };
    if (!answer)
        return TravelLegs::WaitExpired(data.route, now) ? askAgain("got no answer") : true;
    switch (TravelLegs::Judge(data.route, answer->requestId, bot->GetMapId(), now))
    {
        case TravelLegs::Verdict::Drop:
            return true;  // an older request's answer: the latest is still on its way
        case TravelLegs::Verdict::Refresh:
            return askAgain(bot->GetMapId() != data.route.mapId ? "changed maps" : "took the answer too late");
        case TravelLegs::Verdict::Apply:
            break;
    }

    data.route.waiting = false;
    TravelRoute::Result& route = answer->result;
    if (!route.ok)
    {
        LOG_DEBUG("playerbots",
                  "[Route] {} has no route: {} from ({}, {:.0f}, {:.0f}, {:.0f}) to ({}, {:.0f}, {:.0f}, {:.0f}), "
                  "noHearth {}, replans {}",
                  bot->GetName(), TravelRoute::FailureWord(route.failure), bot->GetMapId(), bot->GetPositionX(),
                  bot->GetPositionY(), bot->GetPositionZ(), data.dest.GetMapId(), data.dest.GetPositionX(),
                  data.dest.GetPositionY(), data.dest.GetPositionZ(), data.noHearth, data.trip.replans);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Route, "none", TravelRoute::FailureWord(route.failure),
                           0.0f, {}, data.tripId);
        return LegFailed(data, TravelLegs::FailReason::NoRoute);
    }
    if (data.estimateSeconds <= 0.0f)
    {
        // The trip's clock starts at its start row, so the ends' seconds span the logged trip (the wait for the
        // first route is not counted; a trip that ends before this row counts from its pick)
        data.tripStartMs = now;
        data.estimateSeconds = route.totalSeconds;
        EventLog::Emit(bot, "Travel", "start", data.tripId, static_cast<int64>(route.totalSeconds));
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "start", data.bindInn ? "inn" : "order",
                           route.totalSeconds, {{"legs", static_cast<float>(route.legs.size())}}, data.tripId);
        if (!data.place.empty())
            LOG_INFO("playerbots", "[Route] {} travel to {}: {} legs, {:.0f} s", bot->GetName(), data.place,
                     route.legs.size(), route.totalSeconds);
    }
    if (WorldLog::Active())
        LogRouteDecision(bot, data, route);
    // The deadline runs against the current route (a replan's own estimate)
    data.routeStartMs = now;
    data.routeSeconds = route.totalSeconds;
    LOG_DEBUG("playerbots",
              "[Route] {} to ({}, {:.0f}, {:.0f}, {:.0f}): {} legs, {:.0f} s; taken {} ms after the request (in combat "
              "{}, active {})",
              bot->GetName(), data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(),
              data.dest.GetPositionZ(), route.legs.size(), route.totalSeconds, now - data.route.requestedMs,
              bot->IsInCombat(), botAI->AllowActivity());
    if (route.legs.empty())
        return Arrive(data);  // already there
    data.trip.legs = std::move(route.legs);
    TravelLegs::StartLeg(data.trip, 0, now, GraphNodeAt);
    return true;
}

bool NewRpgTravelAction::UsePortal(NewRpgInfo::Travel& data)
{
    uint32 const triggerId = data.trip.legs[data.trip.legIdx].object;
    AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
    if (!trigger || !sObjectMgr->GetAreaTriggerTeleport(triggerId) || trigger->map != bot->GetMapId())
        return LegFailed(data, TravelLegs::FailReason::Portal);
    if (!bot->IsInAreaTriggerRadius(trigger))
        return MoveFarTo(WorldPosition(trigger->map, trigger->x, trigger->y, trigger->z));

    // What a player's client sends on entering the trigger: the core's own handler checks it and teleports
    WorldPacket packet(CMSG_AREATRIGGER);
    packet << triggerId;
    packet.rpos(0);
    bot->GetSession()->HandleAreaTriggerOpcode(packet);
    data.trip.triggered = true;
    return true;
}

bool NewRpgTravelAction::TalkFlightMaster(NewRpgInfo::Travel& data)
{
    TravelRoute::Leg const& leg = data.trip.legs[data.trip.legIdx];
    if (leg.taxiNodes.size() < 2)
        return LegFailed(data, TravelLegs::FailReason::Taxi);
    // The master of the leg's first taxi node nearest the leg's start (startup cache, read only; scanned only on the
    // few ticks the bot stands at the master)
    TravelMgr::FlightMasterInfo const* master = nullptr;
    float nearest = TravelLegs::FLIGHT_MASTER_YARDS * 2.0f;
    for (auto const& [dbGuid, info] : sTravelMgr.GetFlightMasters(bot->GetTeamId()))
    {
        if (info.taxiNodeId != leg.taxiNodes.front() || info.pos.GetMapId() != leg.from.mapId)
            continue;
        float const yards = TravelRoute::Distance(
            {info.pos.GetMapId(), info.pos.GetPositionX(), info.pos.GetPositionY(), info.pos.GetPositionZ()}, leg.from);
        if (yards < nearest)
        {
            nearest = yards;
            master = &info;
        }
    }
    if (!master)
        return LegFailed(data, TravelLegs::FailReason::Taxi);

    switch (StartTaxi(master->templateEntry, master->pos, leg.taxiNodes))
    {
        case TaxiStart::Moving:
            return true;
        case TaxiStart::Stalled:
            return false;
        case TaxiStart::Started:
            data.trip.taxiActivated = true;
            return true;
        case TaxiStart::Refused:
        {
            // The core's fare differs from the snapshot's: a purse short of the chain's price fails as the fare
            uint32 cost = 0;
            for (size_t i = 0; i + 1 < leg.taxiNodes.size(); ++i)
            {
                uint32 path = 0;
                uint32 price = 0;
                sObjectMgr->GetTaxiPath(leg.taxiNodes[i], leg.taxiNodes[i + 1], path, price);
                cost += price;
            }
            return LegFailed(data,
                             bot->GetMoney() < cost ? TravelLegs::FailReason::Fare : TravelLegs::FailReason::Taxi);
        }
        case TaxiStart::NoMaster:
        case TaxiStart::Undiscovered:
            break;
    }
    return LegFailed(data, TravelLegs::FailReason::Taxi);
}

bool NewRpgTravelAction::UseHearth(NewRpgInfo::Travel& data)
{
    // The cast takes 10 s and a cooling stone is waited for (the route's seconds hold both; the leg's timeout bounds
    // them). The teleport is the stone's own spell: no bot teleport, no sight check
    if (bot->IsNonMeleeSpellCast(false) || bot->HasSpellCooldown(HEARTHSTONE_SPELL))
        return true;
    char const* refusal = nullptr;
    float castResult = 0.0f;
    if (!CastHearth(refusal, castResult))
    {
        // The world log's row names why (WorldTravel::HearthRefusalWord; cast_failed: the value is the core's result)
        LOG_DEBUG("playerbots", "[Route] {} hearth refused: {} (cast result {:.0f})", bot->GetName(),
                  refusal ? refusal : "?", castResult);
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "hearth_refused", refusal ? refusal : "?",
                           castResult, {}, data.tripId);
        return LegFailed(data, TravelLegs::FailReason::Hearth);
    }
    return true;
}

bool NewRpgTravelAction::LegFailed(NewRpgInfo::Travel& data, TravelLegs::FailReason reason)
{
    TravelLegs::Trip& trip = data.trip;
    LOG_DEBUG("playerbots", "[Route] {} leg {}/{} failed: {}", bot->GetName(), trip.legIdx + 1, trip.legs.size(),
              TravelLegs::FailWord(reason));
    uint32 const legSeconds =
        trip.legIdx < trip.legs.size() ? GetMSTimeDiffToNow(trip.legStartMs) / IN_MILLISECONDS : 0;
    EventLog::Emit(bot, "Travel", TravelLegs::LegFailedSource(reason), data.tripId, legSeconds);
    if (trip.legIdx < trip.legs.size() && (trip.legs[trip.legIdx].type == TravelRoute::LegType::Walk ||
                                           trip.legs[trip.legIdx].type == TravelRoute::LegType::Fly))
    {
        uint32 const now = getMSTime();
        NoteTravelShares(botAI, bot, trip.legs[trip.legIdx].type, now - trip.legStartMs, now);
    }
    // A failed walk's row before AfterFailure starts the next record
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Walk)
    {
        uint32 const now = getMSTime();
        LogWalkLeg(bot, data, trip.legIdx, trip.record, now, now - trip.legStartMs,
                   reason == TravelLegs::FailReason::Timeout ? "timed out" : TravelLegs::FailWord(reason), false);
    }
    // A failed fly leg's rows; down away from its end after the takeoff is a fall (or a dismount in the air): its
    // height above the ground now. A bot that fails in the air comes down before the replan (StartDescent, below)
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Fly)
    {
        uint32 const now = getMSTime();
        LogFlyLeg(bot, data, trip.legIdx, trip.record, now, now - trip.legStartMs, trip.climbs, trip.peakZ,
                  reason == TravelLegs::FailReason::Timeout ? "timed out" : TravelLegs::FailWord(reason), false,
                  reason == TravelLegs::FailReason::Takeoff ? TakeoffWord(bot, trip) : nullptr);
        if (reason == TravelLegs::FailReason::Landing)
        {
            float const ground = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            float const height = ground > INVALID_HEIGHT ? std::max(0.0f, bot->GetPositionZ() - ground) : 0.0f;
            char const* const word = TravelLegs::LandingWord(trip, height);
            EventLog::Emit(bot, Upkeep::EVENT_FLIGHT_FALL, word, data.tripId, std::lround(height));
            // What brought it down (WU11b fix 7: Elyvia and Meldel, both in Zul'Drak's area 4310): a fight after the
            // takeoff (combat) or not (fall), the mount, its flight aura and the flying flag now, the core's indoor
            // state (its vmap indoor check takes an outdoor-only mount: fix 10's lead for area 4310), the height and area
            LOG_INFO("playerbots",
                     "[Route] {} came down on fly leg {}/{} ({}): mounted {}, flight aura {}, flying {}, outdoors {}, "
                     "{:.0f} yd above the ground in area {} at ({:.0f}, {:.0f}, {:.0f})",
                     bot->GetName(), trip.legIdx + 1, trip.legs.size(), word, bot->IsMounted(),
                     bot->HasIncreaseMountedFlightSpeedAura() || bot->HasFlyAura(), bot->IsFlying(), bot->IsOutdoors(),
                     height, bot->GetAreaId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        }
    }
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Hearth)
        data.noHearth = true;
    // A portal that did not take the bot through, or a flight that could not be taken: the search avoids that edge
    // for a while (a stuck walk was reported by MoveFarTo's give-up already)
    if (reason == TravelLegs::FailReason::Portal || reason == TravelLegs::FailReason::Taxi)
    {
        auto const [from, to] =
            reason == TravelLegs::FailReason::Portal ? TravelLegs::StuckEdge(trip) : TravelLegs::FirstFlightHop(trip);
        if (from != TravelLegs::NO_NODE)
        {
            TravelRoute::StuckBook::Report report;
            report.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
            report.destination = report.bot;
            report.fromNode = from;
            report.toNode = to;
            TravelRoute::StuckBook::Instance().Push(report);
        }
    }

    // A takeoff refused where the bot stands keeps flying: the replan starts no flight at this spot (AddFlyLegs)
    bool const spotClosed =
        reason == TravelLegs::FailReason::Takeoff && CheckMountStateAction::NoTakeoffHere(bot) != nullptr;
    bool const aborts = TravelLegs::AfterFailure(trip, reason, spotClosed) == TravelLegs::Recovery::Abort;
    // A fly leg failed in the air (no client gravity holds a bot up where it failed, and every walk replan failed at
    // that spot): on its flying mount it flies straight down to the floor, without one it falls; then the replan. An
    // aborting trip comes down too and ends once it is down (FlyDown; WU12b: it hovered where the leg failed)
    if (trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Fly)
    {
        auto const [groundZ, waterZ] = CheckMountStateAction::FloorBelow(bot);
        bool const flyingMount = OnFlyingMount(bot);
        TravelLegs::Decision const down =
            aborts ? TravelLegs::AbortDescent(trip, reason, BotPoint(bot), groundZ, waterZ, flyingMount, getMSTime())
                   : TravelLegs::StartDescent(trip, BotPoint(bot), groundZ, waterZ, flyingMount, getMSTime());
        if (down.act != TravelLegs::Act::Done)
        {
            LOG_INFO("playerbots",
                     "[Route] {} fly leg {}/{} failed ({}) {:.0f} yd above the floor in area {} at ({:.0f}, {:.0f}, "
                     "{:.0f}): {}",
                     bot->GetName(), trip.legIdx + 1, trip.legs.size(), TravelLegs::FailWord(reason),
                     bot->GetPositionZ() - down.target.z, bot->GetAreaId(), bot->GetPositionX(), bot->GetPositionY(),
                     bot->GetPositionZ(), down.act == TravelLegs::Act::Descend ? "flies down" : "falls");
            FlyDown(data, down);
            if (aborts)
                return true;  // the trip ends in FlyDown, now or once it is down
        }
    }
    if (aborts)
    {
        EndTrip(data, TravelLegs::FailWord(reason), TravelLegs::AbortSource(reason));
        return true;
    }
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, "replan", TravelLegs::FailWord(reason),
                       static_cast<float>(legSeconds), {{"leg", static_cast<float>(trip.legIdx + 1)}}, data.tripId);
    trip.legs.clear();
    trip.legIdx = 0;
    trip.waypoints.clear();
    data.route.waiting = false;
    data.stuck = false;
    data.failedMs = std::max<uint32>(getMSTime(), 1);  // 0 = no failed leg
    return true;
}

bool NewRpgTravelAction::FlyDown(NewRpgInfo::Travel& data, TravelLegs::Decision const& down)
{
    TravelLegs::Trip& trip = data.trip;
    uint32 const now = getMSTime();
    trip.lastAct = down.act;
    trip.lastTarget = down.target;
    trip.lastActMs = now;
    // An aborting trip ends once the descent is over, whatever ended it (down, fell, gave up)
    bool const endsTrip = TravelLegs::EndsAfterDescent(trip, down);
    TravelLegs::FailReason const endReason = trip.endReason;
    switch (down.act)
    {
        case TravelLegs::Act::Descend:
            // Straight down on the flying mount, as the fly leg's follower moves (no path generation in the air)
            MoveTo(down.target.mapId, down.target.x, down.target.y, down.target.z, false, false, false, true,
                   MovementPriority::MOVEMENT_NORMAL, true);
            return true;
        case TravelLegs::Act::Done:
            LOG_INFO("playerbots", "[Route] {} flew down to the floor in {} s, area {} at ({:.0f}, {:.0f}, {:.0f})",
                     bot->GetName(), getMSTimeDiff(trip.descentStartMs, now) / IN_MILLISECONDS, bot->GetAreaId(),
                     bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            CheckMountStateAction::LandAndDismount(bot);
            break;
        case TravelLegs::Act::Fall:
            // As a player falls (the fall's damage too): the mount's aura and the flying flags go first
            LOG_INFO("playerbots", "[Route] {} falls {:.0f} yd without a flying mount in area {} at ({:.0f}, {:.0f})",
                     bot->GetName(), bot->GetPositionZ() - down.target.z, bot->GetAreaId(), bot->GetPositionX(),
                     bot->GetPositionY());
            CheckMountStateAction::LandAndDismount(bot);
            break;
        case TravelLegs::Act::Fail:
            // The replan starts where the bot is
            LOG_INFO("playerbots", "[Route] {} gives the flight down up ({}) at z {:.0f}, floor {:.0f}, in area {}",
                     bot->GetName(), TravelLegs::FailWord(down.reason), bot->GetPositionZ(), trip.descentTo.z,
                     bot->GetAreaId());
            break;
        default:
            break;
    }
    if (endsTrip)
        EndTrip(data, TravelLegs::FailWord(endReason), TravelLegs::AbortSource(endReason));
    return true;
}

bool NewRpgTravelAction::Arrive(NewRpgInfo::Travel& data)
{
    // Not on a ship's deck: the bot gets off first, then arrives (WT19-d)
    if (OffFirstBegins(bot, data.trip, TravelLegs::OffFirstEnd::Arrive))
        return true;
    uint32 const seconds = GetMSTimeDiffToNow(data.tripStartMs) / IN_MILLISECONDS;
    uint32 const replans = data.trip.replans;
    if (data.bindInn)
    {
        // The route ends in front of the innkeeper (InnInfo::loc). Walk until close enough to talk to him, then bind
        // directly (not through the "home" action, which needs a cleared target and a cached npc list); a failed bind
        // is tried again a few times, a stuck bot out of reach gives up
        float const yards =
            bot->GetExactDist(data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ());
        WorldTravel::InnStep const step = WorldTravel::InnNext(yards, data.stuck, data.bindTries);
        if (step == WorldTravel::InnStep::Walk)
            return MoveFarTo(data.dest);
        bool bound = false;
        char const* why = "stuck out of reach";
        if (step == WorldTravel::InnStep::Bind)
        {
            ++data.bindTries;
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
                bot->GetSession()->SendBindPoint(innkeeper);
                bound = true;
                break;
            }
            if (!bound && data.bindTries < WorldTravel::INN_BIND_TRIES)
            {
                LOG_DEBUG("playerbots", "[Route] {} bind try {} failed ({}), {:.1f} yd from the inn spot",
                          bot->GetName(), static_cast<uint32>(data.bindTries), why, yards);
                return true;
            }
        }
        LOG_INFO("playerbots", "[Route] {} {} at the inn after {} s ({} replans, {} binds, {:.1f} yd{}{})",
                 bot->GetName(), bound ? "bound its hearthstone" : "could not bind", seconds, replans,
                 static_cast<uint32>(data.bindTries), yards, bound ? "" : ": ", bound ? "" : why);
        EventLog::Emit(bot, "Travel", bound ? "inn_bound" : "inn_failed", data.tripId, seconds);
        // A spot where the bot stood but found no usable innkeeper (phased out, dead, hostile) is avoided by its next
        // inn errands for a while (WorldTravel::InnAvoided); a bot stuck out of reach blames no spot
        bool const noInnkeeper = std::string_view(why) == "no innkeeper in reach";
        if (!bound && (noInnkeeper || std::string_view(why) == "innkeeper not interactable"))
        {
            WorldTravel::AvoidInn(
                botAI->rpgInfo.innAvoid, getMSTime(),
                {data.dest.GetMapId(), data.dest.GetPositionX(), data.dest.GetPositionY(), data.dest.GetPositionZ()});
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "avoid",
                               noInnkeeper ? "no_innkeeper" : "not_interactable", yards, {}, data.tripId);
        }
    }
    if (data.place.empty())
        LOG_DEBUG("playerbots", "[Route] {} arrived after {} s (estimate {:.0f} s, {} replans)", bot->GetName(),
                  seconds, data.estimateSeconds, replans);
    else
        LOG_INFO("playerbots", "[Route] {} arrived at {} after {} s (estimate {:.0f} s, {} replans)", bot->GetName(),
                 data.place, seconds, data.estimateSeconds, replans);
    EventLog::Emit(bot, "Travel", "arrived", data.tripId, seconds);
    WorldLog::StatusReason const reasonScope("arrived");
    botAI->rpgInfo.ChangeToIdle();
    return true;
}

void NewRpgTravelAction::EndTrip(NewRpgInfo::Travel& data, char const* why, char const* source)
{
    // Not on a ship's deck: the bot gets off first, then the trip ends (WT19-d)
    if (OffFirstBegins(bot, data.trip, TravelLegs::OffFirstEnd::Abort, why, source))
        return;
    GiveTripUp(botAI, data, why, source);
}

bool NewRpgStartInnAction::Execute(Event /*event*/)
{
    WorldLog::StatusReason const reasonScope("inn");
    if (!WorldTravel::ErrandMayStart(botAI))
        return false;

    // Checked at most every INN_RETRY_MS, whatever comes of it
    NewRpgInfo& info = botAI->rpgInfo;
    info.lastInnMs = std::max<uint32>(getMSTime(), 1);

    // The bot's team's inns on its map (startup cache): the nearest one whose zone fits its level, within 10 minutes of
    // walking (straight line at run speed)
    std::vector<TravelMgr::InnInfo> const& inns = sTravelMgr.GetInns(bot->GetTeamId());
    std::vector<WorldTravel::InnCandidate> candidates;
    std::vector<size_t> innIndex;
    uint32 const level = bot->GetLevel();
    for (size_t i = 0; i < inns.size(); ++i)
    {
        TravelMgr::InnInfo const& inn = inns[i];
        if (inn.loc.GetMapId() != bot->GetMapId())
            continue;
        // A spot whose bind found no usable innkeeper lately (Arrive)
        if (!info.innAvoid.empty() && WorldTravel::InnAvoided(info.innAvoid, getMSTime(),
                                                              {inn.loc.GetMapId(), inn.loc.GetPositionX(),
                                                               inn.loc.GetPositionY(), inn.loc.GetPositionZ()}))
            continue;

        StartingHistory::Bracket bracket;
        bool const fits = sTravelMgr.GetZoneBracket(inn.zoneId, bracket.low, bracket.high) &&
                          StartingHistory::BracketFits(level, bracket);
        candidates.push_back(
            {bot->GetExactDist(inn.loc.GetPositionX(), inn.loc.GetPositionY(), inn.loc.GetPositionZ()), fits});
        innIndex.push_back(i);
    }
    float const runSpeed = bot->GetSpeed(MOVE_RUN);
    float const maxYards = WorldTravel::INN_MAX_WALK_SECONDS * std::max(runSpeed, 1.0f);
    int const pick = WorldTravel::PickInn(candidates, maxYards);
    // The world log's decision row: the three nearest other inns in walk seconds (inn_unfit: not for the bot's level)
    WorldLogFormat::Alternatives others;
    if (WorldLog::Active())
        for (size_t i = 0; i < candidates.size(); ++i)
            if (static_cast<int>(i) != pick)
                others.Offer({candidates[i].fits ? "inn" : "inn_unfit",
                              WorldLogFormat::WalkSeconds(candidates[i].yards, runSpeed)},
                             true);
    if (pick < 0)
    {
        LOG_DEBUG("playerbots", "[Route] {} inn errand: no fitting inn within reach ({} inns on the map)",
                  bot->GetName(), candidates.size());
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "none", "no_fitting_inn", 0.0f, others);
        return false;
    }

    TravelMgr::InnInfo const& inn = inns[innIndex[static_cast<size_t>(pick)]];
    LOG_DEBUG("playerbots", "[Route] {} inn errand: bind far, inn at ({:.0f}, {:.0f}) in zone {}", bot->GetName(),
              inn.loc.GetPositionX(), inn.loc.GetPositionY(), inn.zoneId);
    info.ChangeToTravel(WorldPosition(inn.loc));
    uint32 tripId = 0;
    if (auto* travel = std::get_if<NewRpgInfo::Travel>(&info.data))
    {
        travel->bindInn = true;
        tripId = travel->tripId;
    }
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "bind_far", "inn_picked",
                       WorldLogFormat::WalkSeconds(candidates[static_cast<size_t>(pick)].yards, runSpeed), others,
                       tripId);
    return true;
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        ContinueCrossMapTaxi();
        return false;
    }

    switch (StartTaxi(data.flightMasterEntry, data.flightMasterPos, data.path))
    {
        case TaxiStart::Moving:
        case TaxiStart::Started:
            return true;
        case TaxiStart::Stalled:
            return false;
        case TaxiStart::NoMaster:
        case TaxiStart::Undiscovered:
        case TaxiStart::Refused:
            break;
    }
    WorldLog::StatusReason const reasonScope("failed");
    info.ChangeToIdle();
    return true;
}

NewRpgBaseAction::TaxiStart NewRpgBaseAction::StartTaxi(uint32 masterEntry, WorldPosition const& masterPos,
                                                        std::vector<uint32> const& nodes)
{
    if (bot->GetDistance(masterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(masterPos) ? TaxiStart::Moving : TaxiStart::Stalled;

    Creature* flightMaster = bot->FindNearestCreature(masterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
        return TaxiStart::NoMaster;
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster) ? TaxiStart::Moving : TaxiStart::Stalled;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    // Learning the master's own node is natural: a player learns it here too
    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    auto const known = [this](uint32 node) { return bot->m_taxi.IsTaximaskNodeKnown(node); };
    if (!StartingHistory::PathKnown(nodes, known))
    {
        // Persistent bots pick only discovered routes; other setups still fly and the flight is counted
        if (RandomPlayerbotMgr::HonestTravel() && sRandomPlayerbotMgr.IsRandomBot(bot))
            return TaxiStart::Undiscovered;

        EventLog::Honesty(bot, "Flight", "undiscovered_flight", nodes.empty() ? 0 : nodes.back());
    }

    // Pays the fare; fails when a node is not usable or the purse is short
    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes.empty() ? 0 : nodes.front(), nodes.empty() ? 0 : nodes.back());
        return TaxiStart::Refused;
    }
    return TaxiStart::Started;
}

void NewRpgBaseAction::ContinueCrossMapTaxi()
{
    if (bot->IsBeingTeleported())
        return;

    if (!bot->movespline->Finalized())
        return;

    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm || mm->GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE)
        return;

    // Check if we are at our destination.
    uint32 nextDest = bot->m_taxi.GetTaxiDestination();
    if (!nextDest)
        return;

    // Confirm next node needs different map.
    TaxiNodesEntry const* nextNode = sTaxiNodesStore.LookupEntry(nextDest);
    if (!nextNode || nextNode->map_id == bot->GetMapId())
        return;

    FlightPathMovementGenerator* flight = dynamic_cast<FlightPathMovementGenerator*>(mm->top());
    if (!flight)
        return;

    LOG_DEBUG("playerbots", "[New RPG] {} continuing taxi across map boundary (next node {} on map {})",
              bot->GetName(), nextDest, nextNode->map_id);

    flight->SetCurrentNodeAfterTeleport();

    if (flight->HasArrived())
        return;

    TaxiPathNodeEntry const* node = flight->GetPath()[flight->GetCurrentNode()];
    flight->SkipCurrentNode();

    bot->TeleportTo(nextNode->map_id, node->x, node->y, node->z, bot->GetOrientation(), TELE_TO_NOT_LEAVE_TAXI);
}
