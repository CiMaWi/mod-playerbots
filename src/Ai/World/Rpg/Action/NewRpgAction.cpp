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
#include <unordered_set>

#include "AreaDefines.h"
#include "BankRulesInput.h"
#include "BankVisit.h"
#include "BotTraitMgr.h"
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
#include "MailVisit.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
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
#include "PlannerIndexes.h"
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
#include "SharedDefines.h"
#include "StartingHistoryRules.h"
#include "TestConstraints.h"
#include "Timer.h"
#include "Trainer.h"
#include "Transport.h"
#include "TravelGraph.h"
#include "TravelLegRules.h"
#include "TravelMgr.h"
#include "TravelTransportGeometry.h"
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

// Mailboxes and bankers (each kind's nearest few) that join the trip's stops by position
constexpr std::size_t BANK_MAIL_STOPS = 4;

// The bank and mail part of the trip planner's view (WB8): only a bot the bank rule governs (no real master, keeps its
// possessions). With an errand to offer, the nearest mailboxes and bankers serving the bot within the walk limit go
// onto the vendor stops within STOP_JOIN_YARDS, or stand as stops of their own (MergeServiceStops)
void AddBankMail(PlayerbotAI* botAI, Player* bot, float reachYards, WorldTravel::ErrandSnapshot& snapshot)
{
    snapshot.Governed = !botAI->HasGameClientMaster() && PlayerbotFactory::KeepsPossessions(bot);
    if (!snapshot.Governed)
        return;

    BankMail::BotBankMail& known = botAI->rpgInfo.bankMail;
    BankMail::RefreshMailKnowledge(bot, known.mail);
    BankMail::DueTotal const due = BankMail::DueAt(known.bank.kept, bot->GetLevel());
    uint32 carried = 0;
    for (BankMail::KeptEntry const& item : known.bank.carried.items)
        carried += item.count;
    snapshot.Orderliness = BotTraitMgr::instance().Trait(bot->GetGUID().GetCounter(), TraitRules::Trait::Orderliness);
    snapshot.MailWaiting = known.mail.waiting;
    snapshot.MailDefaultWorth = sPlayerbotAIConfig.BankMail.MailDefaultWorthCopper;
    snapshot.MailSeenCopper = known.mail.seenCopper;
    snapshot.MailExpectedCopper = known.mail.expectedCopper;
    snapshot.BankDueCount = due.count;
    snapshot.BankDueWorth = due.worth;
    snapshot.BankDepositSlots = std::min<uint32>(carried, known.bank.freeSlots);

    bool const mail = snapshot.MailWaiting || snapshot.MailSeenCopper || snapshot.MailExpectedCopper;
    bool const bank = snapshot.BankDueCount || snapshot.BankDepositSlots;
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!indexes || (!mail && !bank))
        return;

    std::vector<WorldTravel::ServiceStop> extras;
    auto add = [&](Planner::ServiceKind kind, uint32 service)
    {
        for (Planner::Nearby const& found :
             indexes->Nearest(kind, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                              BANK_MAIL_STOPS, reachYards))
        {
            Planner::Service const& spot = indexes->services[found.index];
            // The core's interact test for a banker (Player::GetNPCIfCanInteractWith), as FindBankerSpot makes it
            if (kind == Planner::ServiceKind::Banker &&
                bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(spot.factionTemplate), bot) <=
                    REP_UNFRIENDLY)
                continue;
            extras.push_back(
                {kind == Planner::ServiceKind::Banker ? spot.entry : 0, {spot.mapId, spot.x, spot.y, spot.z}, service});
        }
    };
    if (mail)
        add(Planner::ServiceKind::Mailbox, WorldTravel::SERVICE_MAILBOX);
    if (bank)
        add(Planner::ServiceKind::Banker, WorldTravel::SERVICE_BANKER);
    WorldTravel::MergeServiceStops(snapshot.Stops, extras);
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
        botAI->CancelLootInteraction("order");
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
// flyfail and flyabort change nothing on the bot: the fly leg's follower reads the bit (TestConstraints::FlyLegFails)
void ReadByFollower(Player&) {}
// nodestroy changes nothing on the bot: the smart destroy action reads the bit (TestConstraints::DestroysForSpace)
void ReadByDestroy(Player&) {}
// notaxi changes nothing on the bot: the route request reads the bit (TestConstraints::TaxiAllowed)
void ReadByRoute(Player&) {}

// The state rows' apply and undo (TestConstraints::ROWS)
constexpr TestConstraints::StateOps<Player> STATE_OPS[] = {
    {TestConstraints::NOFIGHT, ApplyNoFight, UndoNoFight},
    {TestConstraints::NODAMAGE, ApplyNoDamage, UndoNoDamage},
    {TestConstraints::FLYFAIL, ReadByFollower, ReadByFollower},
    {TestConstraints::FLYABORT, ReadByFollower, ReadByFollower},
    {TestConstraints::NODESTROY, ReadByDestroy, ReadByDestroy},
    {TestConstraints::NOTAXI, ReadByRoute, ReadByRoute},
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
    uint16 const before = info.constraints;
    if (constrained)
        ApplyConstraints(botAI, bot, info, order, trip);
    // A released errand choice is made at once: an errand check stamped before the hold (an inn trip, the login)
    // would otherwise keep the bot from its errands for the retry time (WB8b run 1: up to 5 minutes)
    if (!TestConstraints::ErrandAllowed(before) && TestConstraints::ErrandAllowed(info.constraints))
        botAI->GetAiObjectContext()->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet().ErrandChecked = false;
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
        botAI->CancelLootInteraction("order");
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
    botAI->CancelLootInteraction("order");
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
    if (NewRpgTravelAction::ConsumeScheduledWork(botAI))
        return true;
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
    // A trip for the mail or the bank needs no vendor either (WB8b)
    bool const bankMailWanted =
        !botAI->HasGameClientMaster() && BankMail::ErrandWaits(botAI->rpgInfo.bankMail, bot->GetLevel());
    if (!WorldTravel::ErrandCheckPlans(nearest != nullptr, trainerWanted || bankMailWanted))
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

    // The stacks the sell action would take from the general slots (as the destroy rule counts them); gear kept for a
    // later level is not among them
    BankMail::RefreshCarriedKeeps(botAI);
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
    {
        // The mount at the nearest vendor in reach that sells one for the rank at its price (each race's vendor sells
        // its own mounts at the one price; the need names the vendor its pricing met first)
        uint32 mountVendor = riding->vendor;
        UpkeepNeeds::RankValue const* const rank = UpkeepNeeds::BoughtRank(needs, riding->item);
        float mountYards = std::numeric_limits<float>::max();
        if (index && rank && riding->vendor)
            for (std::pair<float, TravelMgr::VendorInfo const*> const& vendor : inReach)
                if (vendor.first < mountYards &&
                    UpkeepNeeds::MountSoldBy(*index, bot->getRace(), bot->getClass(), *rank, vendor.second->entry))
                {
                    mountYards = vendor.first;
                    mountVendor = vendor.second->entry;
                }
        snapshot.Riding = {riding->item, mountVendor, riding->trainer != 0};
    }
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
        // stop goes to that trainer; the trainer's own spawns are stops as well. Every class trainer that teaches the
        // highest waiting spell is one: a starter trainer that teaches only the first levels is none
        std::vector<WorldTravel::ServiceStop> trainerStops;
        for (Planner::UpkeepTrainer const& trainer : index->trainers)
            if (trainer.kind == Planner::TrainerKind::Class && trainer.requirement == bot->getClass() &&
                (trainer.key == training->trainer || UpkeepNeeds::Teaches(*index, trainer, training->lastSpell)))
                AddTrainerStops(TrainerSpawns(bot, *index, trainer.key, reachYards), WorldTravel::SERVICE_TRAINER,
                                trainerStops);
        WorldTravel::JoinTrainers(snapshot.Stops, trainerStops, WorldTravel::SERVICE_TRAINER);
        snapshot.Stops.insert(snapshot.Stops.end(), trainerStops.begin(), trainerStops.end());
    }
    // Every riding trainer that teaches the rank: each race's riding trainer is a trainer of its own
    if (index && riding && riding->trainer)
        for (Planner::UpkeepTrainer const& trainer : index->trainers)
            if (trainer.kind == Planner::TrainerKind::Mount && UpkeepNeeds::Teaches(*index, trainer, riding->item))
                AddTrainerStops(TrainerSpawns(bot, *index, trainer.key, reachYards),
                                WorldTravel::SERVICE_RIDING_TRAINER, snapshot.Stops);
    AddCompletedQuests(bot, snapshot.Completed);
    AddBankMail(botAI, bot, reachYards, snapshot);

    // No stop the bot makes anyway yet (WT16's goals bring the route): every stop is a detour that must pay
    WorldTravel::TripRequest request;
    request.Start = {mapId, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    request.YardsPerSecond = runSpeed;
    request.Rates = snapshot.Rates;
    std::vector<WorldTravel::ErrandOffer> offers = WorldTravel::CollectOffers(snapshot, WorldTravel::TripProviders());
    if (questing)
        offers.erase(std::remove_if(offers.begin(), offers.end(),
                                    [](WorldTravel::ErrandOffer const& offer) { return !offer.Due; }),
                     offers.end());
    WorldTravel::TripPlan const plan = WorldTravel::PlanTrip(request, snapshot.Stops, offers);

    std::vector<NewRpgInfo::ErrandStop> stops;
    float tripMinutes = 0.0f;
    std::size_t errandCount = 0;
    bool anyVendor = false;
    // The bank and mail errands' rows: joined when the stop's other errands pay its detour, else detour (WB8b)
    std::vector<std::pair<WorldTravel::ErrandOffer, char const*>> bankMail;
    bool anyTrainer = false;
    std::array<bool, WorldTravel::ERRAND_KIND_COUNT> due{};  // a due need's errand is on the trip, by kind
    for (WorldTravel::TripStop const& tripStop : plan.Stops)
    {
        NewRpgInfo::ErrandStop stop;
        stop.pos = WorldPosition(tripStop.Place.MapId, tripStop.Place.X, tripStop.Place.Y, tripStop.Place.Z);
        bool otherErrand = false;
        float otherWorth = 0.0f;
        for (WorldTravel::ErrandOffer const& errand : tripStop.Errands)
        {
            if (errand.Due)
                due[static_cast<std::size_t>(errand.Kind)] = true;
            switch (errand.Kind)
            {
                case WorldTravel::ErrandKind::BankWithdraw:
                    stop.bankBefore = true;
                    continue;
                case WorldTravel::ErrandKind::BankDeposit:
                    stop.bankAfter = true;
                    continue;
                case WorldTravel::ErrandKind::Mail:
                    stop.mail = true;
                    continue;
                case WorldTravel::ErrandKind::TurnIn:
                    stop.turnIns.push_back({errand.Id, errand.Target, errand.TargetIsObject});
                    break;
                default:
                    break;
            }
            otherErrand = true;
            otherWorth += errand.WorthCopper;
        }
        stop.bankAfter = stop.bankAfter && !stop.bankBefore;
        // The vendor part runs where the stop has errands besides the bank and the mail (a mailbox beside a vendor
        // does not send the bot to that vendor)
        if (otherErrand && (tripStop.Services & WorldTravel::SERVICE_VENDOR))
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
        if (WorldLog::Active())
        {
            bool const joined =
                tripStop.OnRoute || otherWorth > WorldTravel::DetourCost(request.Rates, tripStop.DetourMinutes);
            for (WorldTravel::ErrandOffer const& errand : tripStop.Errands)
                if (errand.Kind == WorldTravel::ErrandKind::Mail ||
                    errand.Kind == WorldTravel::ErrandKind::BankWithdraw ||
                    errand.Kind == WorldTravel::ErrandKind::BankDeposit)
                    bankMail.emplace_back(errand, joined ? "joined" : "detour");
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
                  bot->GetName(), nearest ? nearest->entry : 0, nearest ? nearestYards : 0.0f, walkMinutes,
                  sellableSlots, snapshot.FreeSlots, plan.Refused.size());
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "skip", "not_worth", walkMinutes * MINUTE,
                           weighed);
        return false;
    }

    // The trip's row names the vendor, else the trainer, else its first errand (turn_in, mail, bank_withdraw)
    char const* tripWord = anyVendor ? "vendor" : "trainer";
    if (!anyVendor && !anyTrainer)
        tripWord = plan.Stops.front().Errands.empty()
                       ? "turn_in"
                       : WorldTravel::ErrandKindName(plan.Stops.front().Errands.front().Kind);
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
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, tripWord, reason, tripMinutes * MINUTE, weighed);
    // The bank and mail errands it takes, then the errands it leaves out: the detour rule's refusals
    // (detour_not_worth, too_far, no_stop)
    if (WorldLog::Active())
        for (std::pair<WorldTravel::ErrandOffer, char const*> const& errand : bankMail)
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand,
                               WorldTravel::ErrandKindName(errand.first.Kind), errand.second, 0.0f,
                               {{"worth", errand.first.WorthCopper}});
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
    // A mailbox or banker visit of the stop runs under its own action; the trip goes on once it has ended
    if (!data || BankMail::VisitRuns(botAI->rpgInfo.bankMail))
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
        else if (trainer && IsWithinInteractionDist(trainer))
            TrainAtTrainer(botAI, trainer);
        else
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip", "no_trainer", 0.0f, {}, entry);
        stop.trainerEntry = 0;
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

// WT58: the graph link a walk leg walks now, between waypoints[next - 1] and waypoints[next]; none on the walks
// between the bot, the graph and the goal, and on every other leg
std::pair<std::uint32_t, std::uint32_t> WalkLinkEnds(TravelLegs::Trip const& trip)
{
    if (trip.legIdx >= trip.legs.size() || trip.legs[trip.legIdx].type != TravelRoute::LegType::Walk)
        return {TravelLegs::NO_NODE, TravelLegs::NO_NODE};
    return TravelLegs::StuckEdge(trip);
}

// The links a walk leg walks along: walk links and a wait point's link to its stop (a ride's points are its voyage)
bool WalkableLink(TravelGraphLink const& link)
{
    return link.type == TravelLinkType::Walk || (link.type == TravelLinkType::Transport && !link.object);
}

// A walk leg's order along its graph link (TravelWalkRules.h): the link's next stored point, else target (the far
// node, or a waypoint off the graph). The points stay in the graph; the trip holds an index
TravelRoute::Point WalkLinkTarget(TravelLegs::Trip& trip, TravelRoute::Point const& bot,
                                  TravelRoute::Point const& target)
{
    TravelWalk::LinkWalk& walk = trip.walk;
    auto const [from, to] = WalkLinkEnds(trip);
    if (from == TravelLegs::NO_NODE)
    {
        walk = {};
        return target;
    }
    TravelGraph const& graph = TravelGraph::Instance();
    if (!walk.On(from, to))
    {
        TravelWalk::Rows<TravelGraphLink> const rows = TravelWalk::PickRows<TravelGraphLink>(
            [&graph](std::uint32_t node) { return graph.LinksOf(node); }, from, to, WalkableLink);
        std::span<TravelGraphPoint const> const points =
            rows.link ? graph.PathOf(*rows.link) : std::span<TravelGraphPoint const>{};
        TravelWalk::Begin(walk, from, to, rows.link ? graph.IndexOfLink(*rows.link) : TravelWalk::NO_LINK, points,
                          rows.reversed, graph.GetNode(from), graph.GetNode(to));
    }
    if (walk.mode != TravelWalk::Mode::Stored)
        return target;
    std::span<TravelGraphPoint const> const points = graph.PathOf(graph.GetLink(walk.link));
    std::uint32_t const point = TravelWalk::Target(walk, points, bot, graph.GetNode(to));
    if (point == TravelWalk::NO_POINT)
        return target;
    TravelGraphPoint const& at = TravelWalk::PointAt(points, walk.reversed, point);
    return {at.mapId, at.x, at.y, at.z};
}

constexpr float HOP_LIFT_YARDS = 0.5f;  // the hop lands this far above the deck point (as the off-mesh snap does)
using TravelLifecycle::MotionKind;
using TravelLifecycle::Outcome;
using TravelLifecycle::PassengerMotion;
using TravelLifecycle::Point;
namespace TransportGeometry = TravelTransportGeometry;

// The current leg is a transport leg past its approach: the bot waits at the dock, boards, rides or steps off. The
// follower scans for the transport then, and the trip's deadline waits (the next departure may be minutes away)
bool AtTransportLeg(TravelLegs::Trip const& trip)
{
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport &&
           trip.boarding != TravelLegs::Boarding::Approach;
}

// The leg's transport, found anew on every scan (never kept across ticks): the one the bot rides, else the nearest
// of the leg's entry to its dock on the bot's map. O(transports on the map), once a scan
Transport* FindLegTransport(Player* bot, TravelRoute::Leg const& leg, uint64 expected)
{
    if (Transport* const ridden = bot->GetTransport())
        return ridden->GetEntry() == leg.object ? ridden : nullptr;
    if (expected)
    {
        Transport* const transport = bot->GetMap()->GetTransport(ObjectGuid(expected));
        if (transport && transport->GetEntry() == leg.object && TransportGeometry::Available(*bot, *transport))
            return transport;
    }
    Transport* best = nullptr;
    float bestYards = std::numeric_limits<float>::max();
    auto const consider = [&](Transport* transport)
    {
        if (!transport || transport->GetEntry() != leg.object || !TransportGeometry::Available(*bot, *transport))
            return;
        TravelRoute::Point const& stop = TravelLegs::StopFrom(leg);
        float const yards = transport->IsStaticTransport() ? transport->GetExactDist(stop.x, stop.y, stop.z)
                                                           : transport->GetExactDist2d(stop.x, stop.y);
        if (yards < bestYards)
        {
            best = transport;
            bestYards = yards;
        }
    };
    for (Transport* transport : bot->GetMap()->GetAllTransports())
        consider(transport);
    if (!best)
    {
        // Grid-loaded static transports may be absent from the motion transport set.
        std::list<GameObject*> objects;
        Acore::AllGameObjectsWithEntryInRange check(bot, leg.object, TravelLegs::STATIC_DOCK_YARDS * 2.0f);
        Acore::GameObjectListSearcher<Acore::AllGameObjectsWithEntryInRange> searcher(bot, objects, check);
        Cell::VisitObjects(bot, searcher, TravelLegs::STATIC_DOCK_YARDS * 2.0f);
        for (GameObject* object : objects)
            consider(object->ToTransport());
    }
    return best;
}

// What keeps a bot from a transport move this tick (the board gate's debug word), or nothing
char const* TransportMoveHold(Player const& bot)
{
    if (!bot.movespline->Finalized())
        return "spline";
    if (bot.HasUnitState(UNIT_STATE_NOT_MOVE))
        return "state";
    if (bot.HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
        return "flag";
    if (bot.IsMovementPreventedByCasting())
        return "cast";
    if (bot.IsFlying())
        return "flying";
    if (bot.Unit::IsFalling())
        return "falling";
    return nullptr;
}

bool ReadyForTransportMove(Player const& bot) { return !TransportMoveHold(bot); }

// A ship's or zeppelin's stop node is the vessel's own position (sea level, or a zeppelin's origin about 18 yd above
// its tower floor): the ground beside it within these is where a walk to the stop ends while no quay is known
constexpr float STOP_GROUND_ACROSS_YARDS = 25.0f;
constexpr float STOP_GROUND_VERTICAL_YARDS = 22.0f;

// The nearest ground beside a vessel's stop or position, looked for at the level of the bot that walks there (WT54-5):
// a rider's deck, or the point an approach walk comes from. Looked for at the stop's own height it is the sea floor
// under a berth, or a zeppelin tower's roof or upper ramp instead of the platform 18 yd below the hull's origin.
// Nothing when the tile is not loaded or none lies within reach
std::optional<Point> GroundBesideStop(Map* map, Point const& stop, float level)
{
    Point at{stop.X, stop.Y, level};
    if (NavMeshSnap::Nearest(map, at.X, at.Y, at.Z, STOP_GROUND_ACROSS_YARDS, STOP_GROUND_VERTICAL_YARDS, NAV_GROUND) !=
        NavMeshSnap::Result::Found)
        return std::nullopt;
    return at;
}

// A vessel's deck below its origin (WT54-7), measured on its model at the hull's centre: casts down from the origin,
// each from just below the surface the last one hit. The last surface is the hull's bottom (run 10: Northrend zeppelins
// -8.94/-17.88/-25.51, deck -17.88); the deck is the lowest surface above it with a body's headroom. The model is the
// same at every pose and on every map, so the yards are kept per entry for every map thread; zero for a vessel whose
// stop does not hang above its ground (a ship: its deck lies above its waterline origin) or with no deck found.
constexpr float DECK_HEADROOM_YARDS = 2.0f;
constexpr uint32 DECK_DROP_HITS = 8;

std::mutex deckDropLock;
std::unordered_map<uint32, float> deckDrops;

std::optional<float> KnownDeckDrop(uint32 entry)
{
    std::lock_guard<std::mutex> guard(deckDropLock);
    auto const known = deckDrops.find(entry);
    return known == deckDrops.end() ? std::nullopt : std::optional<float>(known->second);
}

std::optional<float> MeasureDeckDrop(Transport const& transport, uint32 phase)
{
    if (!transport.m_model)
        return std::nullopt;
    Point const centre = TransportGeometry::HullCentre(transport);
    float const low = transport.m_model->GetBounds().low().z;
    float top = centre.Z;
    std::vector<float> surfaces;
    std::string hits;
    while (surfaces.size() < DECK_DROP_HITS && top > low)
    {
        auto const surface =
            TransportGeometry::DeckHeight(transport, phase, {centre.X, centre.Y, top}, 0.0f, top - low + 1.0f);
        if (!surface)
            break;
        hits += Acore::StringFormat("{}{:.2f}", hits.empty() ? "" : "/", *surface - centre.Z);
        surfaces.push_back(*surface);
        top = *surface - 0.05f;
    }
    std::optional<float> deck;
    for (std::size_t i = 0; i + 1 < surfaces.size(); ++i)
        if ((i ? surfaces[i - 1] : centre.Z) - surfaces[i] >= DECK_HEADROOM_YARDS)
            deck = surfaces[i];
    LOG_DEBUG("playerbots", "[TravelLifecycle] deck_drop entry={} origin_z={:.2f} low={:.2f} hits={} deck={}",
              transport.GetEntry(), centre.Z, low, hits.empty() ? std::string("none") : hits,
              deck ? Acore::StringFormat("{:.2f}", *deck - centre.Z) : std::string("none"));
    if (!deck)
        return std::nullopt;
    return centre.Z - *deck;
}

// The approach walk to a ship's or zeppelin's stop ends on the ground beside it, not at the stop (in the berth's
// water, or in the air at a zeppelin's origin), where the arriving hull would take the bot aboard without a boarding.
// A stop that hangs above its ground (a zeppelin's) is waited for at its deck's level (WT54-7): the ground nearest the
// origin is the tower's roof or upper ramp (Tirisfal 113.09, Orgrimmar 62.41), 18 yd above the platform the gondola
// docks at. The deck is measured once the vessel is on the bot's map (here or by any bot); an end aimed before that is
// aimed again once it is known. The walk's point before the stop is no level (WT54-5: a graph node at the tower's foot)
void AimApproachEnd(Player* bot, TravelRoute::Point& end, TravelRoute::Leg const& leg, uint64 expected)
{
    TravelRoute::Point const& stopAt = TravelLegs::StopFrom(leg);
    if (end.mapId != stopAt.mapId)
        return;
    Point const stop{stopAt.x, stopAt.y, stopAt.z};
    bool const atStop = TravelRoute::Distance(end, stopAt) < 0.1f;
    auto drop = KnownDeckDrop(leg.object);
    if (!drop)
        if (Transport* const transport = FindLegTransport(bot, leg, expected))
            if (transport->ToMotionTransport())
            {
                auto const plain = GroundBesideStop(bot->GetMap(), stop, stop.Z);
                if (plain && transport->m_model)
                {
                    // A failed measurement over a hanging stop is not kept: the next scan measures again
                    bool const overGround = plain->Z < stop.Z - TravelLifecycle::DOCK_FLOOR_YARDS;
                    std::optional<float> const measured =
                        overGround ? MeasureDeckDrop(*transport, bot->GetPhaseMask()) : std::optional<float>(0.0f);
                    if (measured)
                    {
                        drop = measured;
                        std::lock_guard<std::mutex> guard(deckDropLock);
                        deckDrops.emplace(leg.object, *drop);
                    }
                }
            }
    bool const hangs = drop && *drop > TravelLifecycle::DOCK_FLOOR_YARDS;
    float const level = hangs ? stop.Z - *drop : stop.Z;
    if (!atStop && (!hangs || std::fabs(end.z - level) <= 2.0f * TravelLifecycle::DOCK_FLOOR_YARDS))
        return;
    auto const ground = GroundBesideStop(bot->GetMap(), stop, level);
    if (!ground || (!atStop && TravelRoute::Distance(end, {end.mapId, ground->X, ground->Y, ground->Z}) < 0.5f))
        return;
    end = {end.mapId, ground->X, ground->Y, ground->Z};
    LOG_DEBUG("playerbots", "[Route] {} walks to {:.2f}, {:.2f}, {:.2f} beside the stop of {} (deck {} below it)",
              bot->GetName(), ground->X, ground->Y, ground->Z, leg.object,
              hangs ? Acore::StringFormat("{:.2f}", *drop) : std::string("not"));
}

// The ground nearest the rider that lies off the docked hull, within a step of the deck's floor (WT54-3). A route that
// ends at the vessel's stop gives no ground off the hull: beside a stop lies the pier under the docked ship, or the
// ground below a zeppelin's tower. The bot then goes on from this ground; the exit's checks stay as they are.
constexpr float OFF_HULL_REACH_YARDS = 40.0f;
constexpr float OFF_HULL_RING_YARDS = 2.0f;
constexpr uint32 OFF_HULL_BEARINGS = 16;

std::optional<Point> GroundOffHull(Player const& bot, Transport const& ridden, bool trace)
{
    TransportGeometry::GroundGeometry const geometry{bot, &ridden};
    Point const from = TransportGeometry::PositionOf(bot);
    constexpr float FULL_TURN = 6.28318531f;
    // Why the candidates failed (traced bots, WT54-4): no ground mesh within a step of the deck's height, ground under
    // the hull, or a mesh point that is not standing ground (water, or the snap left the point)
    uint32 noMesh = 0;
    uint32 onHull = 0;
    uint32 notGround = 0;
    for (float radius = OFF_HULL_RING_YARDS; radius <= OFF_HULL_REACH_YARDS; radius += OFF_HULL_RING_YARDS)
        for (uint32 bearing = 0; bearing < OFF_HULL_BEARINGS; ++bearing)
        {
            float const angle = FULL_TURN * static_cast<float>(bearing) / static_cast<float>(OFF_HULL_BEARINGS);
            Point const seed{from.X + radius * std::cos(angle), from.Y + radius * std::sin(angle), from.Z};
            auto const ground = geometry.SnapGround(seed, OFF_HULL_RING_YARDS / 2.0f, TravelLifecycle::EXIT_STEP_YARDS);
            if (!ground)
                ++noMesh;
            else if (!geometry.OffHull(*ground))
                ++onHull;
            else if (!geometry.OnGround(*ground))
                ++notGround;
            else
                return ground;
        }
    if (trace)
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] ground_off_hull bot={} entry={} at={:.2f},{:.2f},{:.2f} origin_z={:.2f} "
                  "none: no_mesh={} on_hull={} not_ground={}",
                  bot.GetGUID().GetCounter(), ridden.GetEntry(), from.X, from.Y, from.Z, ridden.GetPositionZ(), noMesh,
                  onHull, notGround);
    return std::nullopt;
}

bool PrepareTransportMove(Player& bot)
{
    if (!ReadyForTransportMove(bot))
        return false;
    if (!bot.IsMounted())
        return true;
    WorldPacket emptyPacket;
    bot.GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    return false;
}

PassengerMotion Intent(Player const& bot, Transport const& transport, MotionKind kind, Point const& endpoint)
{
    PassengerMotion motion;
    motion.Kind = kind;
    motion.Transport = transport.GetGUID().GetRawValue();
    motion.Entry = transport.GetEntry();
    motion.Map = bot.GetMapId();
    motion.Endpoint = endpoint;
    motion.Dock = TransportGeometry::PoseOf(transport);
    return motion;
}

// A boarding walks straight onto the deck; an exit walks across the deck by its deck walk's corners to the deck's edge
// (via) and on to the quay. Via points within 0.2 yd of the point before are left out (a spline refuses a segment
// shorter than 0.1 yd)
void IssueTransportMove(Player& bot, NewRpgInfo::Travel& data, PassengerMotion motion,
                        std::vector<Point> const& via = {})
{
    if (bot.IsSitState())
        bot.SetStandState(UNIT_STAND_STATE_STAND);
    bot.GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(&bot);
    if (!via.empty())
    {
        Movement::PointsArray path{{bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ()}};
        for (Point const& point : via)
            if ((G3D::Vector3{point.X, point.Y, point.Z} - path.back()).length() >= 0.2f)
                path.push_back({point.X, point.Y, point.Z});
        G3D::Vector3 const end{motion.Endpoint.X, motion.Endpoint.Y, motion.Endpoint.Z};
        if (path.size() > 1 && (end - path.back()).length() < 0.2f)
            path.back() = end;
        else
            path.push_back(end);
        init.MovebyPath(path);
    }
    else
        init.MoveTo(motion.Endpoint.X, motion.Endpoint.Y, motion.Endpoint.Z, false, false);
    if (init.Launch() <= 0)
        return;
    motion.Spline = bot.movespline->GetId();
    data.passengerMotion = motion;
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
    std::optional<Point> deck;
    char const* refusal = nullptr;
    if (!transport)
        refusal = "absent";
    else if (!ship)
        refusal = "not_mo";
    else if (!Docked(*ship))
        refusal = "away";
    else
    {
        deck = TransportGeometry::GroundGeometry{*bot, transport}.DeckFrom(TransportGeometry::PositionOf(*bot));
        if (!deck ||
            bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), deck->X, deck->Y, deck->Z, bot) != transport)
            refusal = "no_deck";
    }
    if (refusal)
    {
        LOG_INFO("playerbots", "[Route] {} aboard {} refused: {}", bot->GetName(), entry, refusal);
        std::string const source = std::string("refused:") + refusal;
        EventLog::Emit(bot, PlayerbotAI::ABOARD_EVENT_TYPE, source.c_str(), entry);
        return;
    }
    LOG_INFO("playerbots", "[Route] {} aboard {}: deck {:.1f}, {:.1f}, {:.1f}", bot->GetName(), entry, deck->X, deck->Y,
             deck->Z);
    bot->NearTeleportTo(deck->X, deck->Y, deck->Z + HOP_LIFT_YARDS, bot->GetOrientation());
    botAI->BeginAboard(entry);
}

std::optional<Outcome> TakePassengerOutcome(Player& bot, NewRpgInfo::Travel& data)
{
    if (!data.passengerMotion)
        return std::nullopt;
    auto const result =
        TravelLifecycle::PassengerOutcome(*data.passengerMotion, TransportGeometry::ReadMembership(bot));
    if (!result)
        return std::nullopt;
    TransportGeometry::DiscardMotion(bot, data.passengerMotion);
    return result;
}

// The crossing checks' words (TravelLifecycle::CrossingCheck), for the debug log
constexpr std::array<char const*, TravelLifecycle::CrossingTally::REASONS> CROSSING_NAMES = {
    "accepted", "no_shore", "no_deck",  "on_hull",  "height",   "distance",
    "ground",   "onward",   "crossing", "corridor", "deck_walk"};

// A failed exit or quay search, for the debug log: the hull box, the count of each failed check and its first sample
void LogCrossingSearch(Player const& bot, NewRpgInfo::Travel const& data, Transport const& transport, char const* kind,
                       TravelLifecycle::CrossingTally const& tally)
{
    TravelLifecycle::Footprint const hull = TransportGeometry::Hull(bot, transport);
    TransportGeometry::GroundGeometry const geometry{bot, &transport};
    std::string reasons;
    for (std::size_t i = 0; i < CROSSING_NAMES.size(); ++i)
    {
        if (!tally.Counts[i])
            continue;
        auto const& sample = *tally.First[i];
        reasons +=
            Acore::StringFormat(" {}={}@{:.2f},{:.2f},{:.2f}>{:.2f},{:.2f},{:.2f}", CROSSING_NAMES[i], tally.Counts[i],
                                sample.From.X, sample.From.Y, sample.From.Z, sample.To.X, sample.To.Y, sample.To.Z);
        if (i == static_cast<std::size_t>(TravelLifecycle::CrossingCheck::Crossing))
            reasons += Acore::StringFormat(
                "/{}", TransportGeometry::GroundGeometry::BlockName(geometry.CrossingBlock(sample.From, sample.To)));
        if (i == static_cast<std::size_t>(TravelLifecycle::CrossingCheck::Corridor) && tally.Onward)
            reasons += Acore::StringFormat(
                "/{}", TransportGeometry::GroundGeometry::ReachName(geometry.GroundReach(sample.To, *tally.Onward)));
    }
    std::string const onward =
        tally.Onward ? Acore::StringFormat("{:.2f},{:.2f},{:.2f}", tally.Onward->X, tally.Onward->Y, tally.Onward->Z)
                     : std::string("none");
    Point const centre = TransportGeometry::HullCentre(transport);
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] crossing_search bot={} trip={} leg={} kind={} entry={} map={} at={:.2f},{:.2f},{:.2f} "
              "origin={:.2f},{:.2f},{:.2f} o={:.3f} hull={:.1f},{:.1f},{:.1f}..{:.1f},{:.1f},{:.1f} "
              "centre={:.2f},{:.2f} onward={} deck_cells={}{}",
              bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, kind, transport.GetEntry(), bot.GetMapId(),
              bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ(), transport.GetPositionX(),
              transport.GetPositionY(), transport.GetPositionZ(), transport.GetOrientation(), hull.Low.X, hull.Low.Y,
              hull.Low.Z, hull.High.X, hull.High.Y, hull.High.Z, centre.X, centre.Y, onward, tally.DeckCells, reasons);
    // The exit probe: per bearing (degrees from the route's side), the candidate that got furthest through the checks
    for (std::size_t turn = 0; turn < tally.Bearings.size(); ++turn)
    {
        auto const& furthest = tally.Bearings[turn];
        if (!furthest)
            continue;
        int const side = static_cast<int>((turn + 1) / 2) * (turn % 2 ? 1 : -1);
        std::string block;
        if (furthest->Check == TravelLifecycle::CrossingCheck::Crossing)
            block = Acore::StringFormat("/{}", TransportGeometry::GroundGeometry::BlockName(
                                                   geometry.CrossingBlock(furthest->From, furthest->To)));
        else if (furthest->Check == TravelLifecycle::CrossingCheck::Corridor && tally.Onward)
            block = Acore::StringFormat(
                "/{}", TransportGeometry::GroundGeometry::ReachName(geometry.GroundReach(furthest->To, *tally.Onward)));
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] exit_probe bot={} trip={} bearing={} at={:.0f} check={}{} "
                  "from={:.2f},{:.2f},{:.2f} to={:.2f},{:.2f},{:.2f}",
                  bot.GetGUID().GetCounter(), data.tripId,
                  side * static_cast<int>(360 / TravelLifecycle::CROSSING_BEARINGS), furthest->Distance,
                  CROSSING_NAMES[static_cast<std::size_t>(furthest->Check)], block, furthest->From.X, furthest->From.Y,
                  furthest->From.Z, furthest->To.X, furthest->To.Y, furthest->To.Z);
    }
}

// WT43b: the boarding crossing walked back where the transport stands as it did at the boarding (the passenger
// observer confirms it as any exit, ConfirmExit). Its shore may touch the hull (a zeppelin's gondola beside its tower):
// then the walk goes on from the shore, along the crossing's line, to the first point off the hull up to
// BACK_OUT_YARDS out, and that step is the exit's crossing (CheckExit from the shore; the deck-to-shore part is the
// boarding's, checked then at this pose). Onward ground: a step beyond
struct BackPath
{
    Point Deck;
    Point From;  // where the exit's crossing starts: the deck, or the shore when the walk goes on from it
    Point Ground;
    Point Onward;
    std::vector<Point> Walk;  // the deck walk's corners, ending at Deck
};

std::optional<BackPath> BackCrossing(Player const& bot, NewRpgInfo::Travel const& data, Transport const& ridden,
                                     bool trace)
{
    constexpr float ONWARD_YARDS = 4.0f;
    constexpr float BACK_OUT_YARDS = 3.0f;
    auto const walk = TravelLegs::CrossingBack(data.trip.boardCrossing, ridden.GetGUID().GetRawValue(),
                                               TransportGeometry::PoseOf(ridden));
    if (!walk)
        return std::nullopt;
    float const dx = walk->shore.X - walk->deck.X;
    float const dy = walk->shore.Y - walk->deck.Y;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.1f)
        return std::nullopt;
    TransportGeometry::GroundGeometry const geometry{bot, &ridden};
    auto const out = [&](Point const& from, float yards)
    {
        return geometry.SnapGround({from.X + dx / length * yards, from.Y + dy / length * yards, from.Z}, 1.5f,
                                   TravelLifecycle::DOCK_FLOOR_YARDS);
    };
    TravelLifecycle::CrossingCheck check = TravelLifecycle::CrossingCheck::NoShore;
    std::optional<BackPath> path;
    float extra = 0.0f;
    for (; extra <= BACK_OUT_YARDS && !path; extra += 1.0f)
    {
        Point const from = extra > 0.0f ? walk->shore : walk->deck;
        auto const ground = extra > 0.0f ? out(walk->shore, extra) : std::optional<Point>{walk->shore};
        auto const onward = ground ? out(*ground, ONWARD_YARDS) : std::nullopt;
        if (!ground || !onward)
        {
            check = ground ? TravelLifecycle::CrossingCheck::Onward : TravelLifecycle::CrossingCheck::NoShore;
            continue;
        }
        check = TravelLifecycle::CheckExit(geometry, from, *ground, *onward);
        if (check == TravelLifecycle::CrossingCheck::Accepted)
            path = BackPath{walk->deck, from, *ground, *onward};
    }
    if (path)
    {
        std::uint32_t expanded = 0;
        auto deckWalk = geometry.DeckPath(TransportGeometry::PositionOf(bot), walk->deck,
                                          TravelLifecycle::DECK_PATH_CELLS, expanded);
        if (deckWalk)
            path->Walk = std::move(*deckWalk);
        else
        {
            check = TravelLifecycle::CrossingCheck::DeckWalk;
            path.reset();
        }
    }
    if (trace)
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] exit_back bot={} trip={} leg={} entry={} check={} out={:.0f} "
                  "deck={:.2f},{:.2f},{:.2f} shore={:.2f},{:.2f},{:.2f}",
                  bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, ridden.GetEntry(),
                  CROSSING_NAMES[static_cast<std::size_t>(check)], extra - 1.0f, walk->deck.X, walk->deck.Y,
                  walk->deck.Z, walk->shore.X, walk->shore.Y, walk->shore.Z);
    return path;
}

// The exit to a stop's wait point, for the debug log (WT60): the first gate that held it (the crossing checks' words),
// the deck point, and whether the wait point lies off the hull and on ground by the old search's tests
void LogExitGate(Player const& bot, NewRpgInfo::Travel const& data, Transport const& ridden, Point const& wait,
                 std::optional<TravelLifecycle::ExitPath> const& exit, TravelLifecycle::CrossingCheck gate)
{
    TransportGeometry::GroundGeometry const geometry{bot, &ridden};
    std::string deck("none");
    if (exit)
        deck = Acore::StringFormat("{:.2f},{:.2f},{:.2f}", exit->Deck.X, exit->Deck.Y, exit->Deck.Z);
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] exit_gate bot={} trip={} leg={} entry={} at={:.2f},{:.2f},{:.2f} gate={} "
              "wait={:.2f},{:.2f},{:.2f} deck={} wait_off_hull={} wait_ground={} corners={}",
              bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, ridden.GetEntry(), bot.GetPositionX(),
              bot.GetPositionY(), bot.GetPositionZ(), CROSSING_NAMES[static_cast<std::size_t>(gate)], wait.X, wait.Y,
              wait.Z, deck, geometry.OffHull(wait) ? 1 : 0, geometry.OnGround(wait) ? 1 : 0,
              exit ? exit->Walk.size() : 0);
}

// Leaving is boarding in reverse (WT60): at a stop with a wait point the exit crosses from the deck point beside it
// (FindWaitExit). Where it has none, or that crossing fails (the model between the wait point and the deck point
// toward the hull's centre, as at Ratchet and Auberdine's pier), the old search finds ground off the hull toward the
// route (FindExit; GroundOffHull only where no wait point and no route point gives the onward point). A failed exit
// tries the boarding crossing walked back (WT43b), else it is a no_exit and the leg rides back
void WalkOff(Player& bot, NewRpgInfo::Travel& data, Transport const& ridden, std::optional<Point> const& waitPoint,
             std::vector<Point> const& route)
{
    if (data.passengerMotion || data.trip.exitSought || !PrepareTransportMove(bot))
        return;
    bool const trace = !data.place.empty() && sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG);
    TravelLifecycle::CrossingTally tally;
    std::optional<TravelLifecycle::ExitPath> exit;
    if (waitPoint)
    {
        TravelLifecycle::CrossingCheck gate = TravelLifecycle::CrossingCheck::NoDeck;
        if (TransportGeometry::Available(bot, ridden))
            exit = TravelLifecycle::FindWaitExit(TransportGeometry::GroundGeometry{bot, &ridden},
                                                 TransportGeometry::PositionOf(bot), *waitPoint,
                                                 TransportGeometry::HullCentre(ridden), gate);
        if (trace)
            LogExitGate(bot, data, ridden, *waitPoint, exit, gate);
    }
    bool const toWaitPoint = exit.has_value();
    if (!exit)
    {
        std::vector<Point> onward = route;
        if (!waitPoint && !TravelLifecycle::OnwardPoint(TransportGeometry::GroundGeometry{bot, &ridden}, route))
            if (auto const ground = GroundOffHull(bot, ridden, trace))
                onward.push_back(*ground);
        exit = TransportGeometry::FindExit(bot, ridden, onward, trace ? &tally : nullptr);
        if (!exit && trace)
            LogCrossingSearch(bot, data, ridden, "exit", tally);
    }
    if (exit)
    {
        if (trace)
        {
            std::string walk;
            for (std::size_t i = 0; i + 1 < exit->Walk.size(); ++i)
                walk += Acore::StringFormat("{}{:.1f},{:.1f},{:.1f}", walk.empty() ? "" : "/", exit->Walk[i].X,
                                            exit->Walk[i].Y, exit->Walk[i].Z);
            LOG_DEBUG("playerbots",
                      "[TravelLifecycle] exit_path bot={} trip={} leg={} entry={} map={} at={:.2f},{:.2f},{:.2f} "
                      "deck={:.2f},{:.2f},{:.2f} ground={:.2f},{:.2f},{:.2f} onward={:.2f},{:.2f},{:.2f} "
                      "corners={} cells={}",
                      bot.GetGUID().GetCounter(), data.tripId, data.trip.legIdx, ridden.GetEntry(), bot.GetMapId(),
                      bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ(), exit->Deck.X, exit->Deck.Y,
                      exit->Deck.Z, exit->Ground.X, exit->Ground.Y, exit->Ground.Z, exit->Onward.X, exit->Onward.Y,
                      exit->Onward.Z, walk.empty() ? std::string("none") : walk, tally.DeckCells);
        }
        PassengerMotion motion = Intent(bot, ridden, MotionKind::Exit, exit->Ground);
        motion.ExitFrom = exit->Deck;
        motion.Onward = exit->Onward;
        motion.ToWaitPoint = toWaitPoint;
        IssueTransportMove(bot, data, motion, exit->Walk);
        return;
    }
    if (auto const back = BackCrossing(bot, data, ridden, trace))
    {
        PassengerMotion motion = Intent(bot, ridden, MotionKind::Exit, back->Ground);
        motion.ExitFrom = back->From;
        motion.Onward = back->Onward;
        std::vector<Point> via = back->Walk;
        via.push_back(back->From);
        IssueTransportMove(bot, data, motion, via);
        return;
    }
    data.trip.exitSought = true;
    data.trip.exitless = true;
    EventLog::Emit(&bot, TravelLegs::TRANSPORT_EVENT_TYPE,
                   TravelLegs::TransportSource(TravelLegs::TransportEvent::NoExit), ridden.GetEntry(), 0);
}

// WT55b, data for the navigation plan's NV18 (WT55c removes it with the searches): at a stop with a wait point the
// old searches decide nothing. Once per vessel, stop and role in a server run they still run for a traced bot, and a
// dock_foot line says where they would have put the crossing's foot. A bounded set (vessels x stops x 2 roles) the
// map threads share; the board role runs in the rate-limited transport scan, the exit role at the exit search
std::mutex dockFootLock;
std::unordered_set<uint64> dockFeet;

bool TracedDockFoot(NewRpgInfo::Travel const& data, uint32 entry, uint32 stopNode, bool exit)
{
    if (data.place.empty() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return false;
    uint64 const key = (uint64(entry) << 32) | (uint64(stopNode) << 1) | (exit ? 1u : 0u);
    std::lock_guard<std::mutex> guard(dockFootLock);
    return dockFeet.insert(key).second;
}

std::string DockFootPoint(std::optional<Point> const& at)
{
    return at ? Acore::StringFormat("{:.2f},{:.2f},{:.2f}", at->X, at->Y, at->Z) : std::string("none");
}

void LogDockFoot(TravelRoute::Leg const& leg, bool exit, std::optional<Point> const& foot,
                 std::optional<Point> const& deck, char const* firstFail)
{
    TravelGraph const& graph = TravelGraph::Instance();
    uint32 const stopNode = leg.nodes[exit ? leg.rideAt + 1 : leg.rideAt];
    uint32 const waitNode = exit ? leg.nodes.back() : leg.nodes.front();
    TravelRoute::Point const& waitAt = exit ? leg.to : leg.from;
    TravelRoute::Point const& stopAt = exit ? TravelLegs::StopTo(leg) : TravelLegs::StopFrom(leg);
    Point const wait{waitAt.x, waitAt.y, waitAt.z};
    auto const flat = [](Point const& a, Point const& b) { return std::hypot(a.X - b.X, a.Y - b.Y); };
    auto const full = [](Point const& a, Point const& b)
    { return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z)); };
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] dock_foot entry={} stop={} stopname='{}' wait={} role={} wait_at={} foot={} deck={} "
              "d_wait_foot={:.1f} d_foot_deck={:.1f} d_wait_stop={:.1f} first_fail={}",
              leg.object, graph.GetNode(stopNode).id, graph.GetName(stopNode), graph.GetNode(waitNode).id,
              exit ? "exit" : "board", DockFootPoint(wait), DockFootPoint(foot), DockFootPoint(deck),
              foot ? flat(wait, *foot) : -1.0f, foot && deck ? full(*foot, *deck) : -1.0f,
              flat(wait, Point{stopAt.x, stopAt.y, stopAt.z}), firstFail);
}

// The exit role: the ground off the hull the rider would have stepped onto, from the deck at the far stop
void LogExitDockFoot(Player& bot, NewRpgInfo::Travel const& data, Transport const& ridden)
{
    TravelRoute::Leg const& leg = data.trip.legs[data.trip.legIdx];
    if (!leg.waitTo || leg.rideAt + 1 >= leg.nodes.size() || data.passengerMotion || data.trip.exitSought ||
        !TracedDockFoot(data, leg.object, leg.nodes[leg.rideAt + 1], true))
        return;
    std::optional<Point> const foot = GroundOffHull(bot, ridden, true);
    LogDockFoot(leg, true, foot, TransportGeometry::PositionOf(bot), "none");
}

// Where the bot goes after a transport's quay, for the exit search: the far stop's wait point where the leg has one,
// then the next walk leg's waypoints after its start (its graph nodes, then its end), or the start of the next leg of
// another kind; the trip's destination when the next leg lies on another map or there is none. deckZ: the rider's
// floor, the level the ground beside the stop is looked for at
std::vector<Point> OnwardRoute(TravelLegs::Trip const& trip, Map* map, uint32 mapId, Point const& dest, float deckZ)
{
    std::vector<Point> route;
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    if (leg.waitTo && leg.to.mapId == mapId)
        route.push_back({leg.to.x, leg.to.y, leg.to.z});
    if (trip.legIdx + 1 < trip.legs.size())
    {
        TravelRoute::Leg const& onward = trip.legs[trip.legIdx + 1];
        if (onward.type == TravelRoute::LegType::Walk)
        {
            std::vector<TravelLegs::Waypoint> const points = TravelLegs::Waypoints(onward, GraphNodeAt);
            for (std::size_t i = 1; i < points.size(); ++i)
                if (points[i].at.mapId == mapId)
                    route.push_back({points[i].at.x, points[i].at.y, points[i].at.z});
        }
        else if (onward.from.mapId == mapId)
            route.push_back({onward.from.x, onward.from.y, onward.from.z});
    }
    if (route.empty())
        route.push_back(dest);
    // A point at the leg's end node is the vessel's own stop (in the berth's water, or in the air at a zeppelin's
    // origin), with no ground to walk on: the ground beside it instead, as for the approach walk (SeekQuay). A leg
    // that ends at a wait point ends on walkable ground
    TravelRoute::Point const& end = leg.to;
    for (Point& point : route)
        if (!leg.waitTo && end.mapId == mapId &&
            TravelRoute::Distance(end, {mapId, point.X, point.Y, point.Z}) <= TravelLegs::SAME_POINT_YARDS)
            point = GroundBesideStop(map, point, deckZ).value_or(point);
    return route;
}

// An aboard scan short of the leave, for the debug log: what the dock test at the leg's end saw
void LogAboardScan(Player* bot, TravelLegs::Trip const& trip, float moved)
{
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    TravelLegs::TransportSight const& sight = trip.sight;
    TravelRoute::Point const& stop = TravelLegs::StopTo(leg);
    float const dx = sight.at.x - stop.x;
    float const dy = sight.at.y - stop.y;
    LOG_DEBUG("playerbots",
              "[Route] {} aboard {} on map {}: {:.1f} yd (2D) from the leg's end on map {}, still {}, moving {}, "
              "static {}, {:.1f} yd moved since the last scan (-1: none)",
              bot->GetName(), leg.object, sight.at.mapId, std::sqrt(dx * dx + dy * dy), leg.to.mapId, trip.still,
              sight.moving, sight.isStatic, moved);
}

float SeeTransport(Player* bot, Transport* transport, TravelLegs::Trip const& trip, TravelLegs::TransportSight& sight);

uint32 MotionQuayLeg(TravelLegs::Trip const& trip)
{
    uint32 index = trip.legIdx;
    if (index >= trip.legs.size())
        return TravelLegs::NO_NODE;
    TravelRoute::Leg const& current = trip.legs[index];
    if (current.type == TravelRoute::LegType::Walk && index + 1 < trip.legs.size())
    {
        TravelRoute::Point const& dock = trip.legs[++index].from;
        if (current.to.mapId != dock.mapId || TravelRoute::Distance(current.to, dock) > 1.0f)
            return TravelLegs::NO_NODE;
    }
    TravelRoute::Leg const& leg = trip.legs[index];
    // A stop with a wait point is waited at there (WT.42): no quay to look for
    if (leg.type != TravelRoute::LegType::Transport || leg.waitFrom)
        return TravelLegs::NO_NODE;
    GameObjectTemplate const* const info = sObjectMgr->GetGameObjectTemplate(leg.object);
    return info && info->type == GAMEOBJECT_TYPE_MO_TRANSPORT ? index : TravelLegs::NO_NODE;
}

void SeekQuay(Player* bot, NewRpgInfo::Travel& data, TravelLegs::Observation& seen)
{
    TravelLegs::Trip& trip = data.trip;
    if (trip.legIdx >= trip.legs.size() || trip.boarding != TravelLegs::Boarding::Approach)
        return;
    // At its transport's dock the bot waits on foot (WT54-7; TravelKeepsOnFoot keeps it so): Thunder Bluff's zeppelin
    // left while the bot spent the scan after its docking on the dismount. A flying mount's flag stays set on the
    // ground (WT54-11: Ditlis at Valiance Keep, held as "flying" for 800 s): only a bot in the air keeps its mount
    auto const airborne = [bot]
    {
        float const ground = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        return bot->IsFlying() && bot->GetPositionZ() - ground > TravelLifecycle::DOCK_FLOOR_YARDS;
    };
    if (trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport && bot->IsMounted() && !bot->IsInCombat() &&
        !airborne())
    {
        WorldPacket emptyPacket;
        bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    }

    // A stop with a wait point: StepTransport waits and crosses there, no search
    if (trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport && trip.legs[trip.legIdx].waitFrom)
        return;
    if (trip.hasQuay || getMSTimeDiff(data.transportScanMs, seen.nowMs) < TravelLegs::TRANSPORT_SCAN_MS)
        return;
    if (seen.quayLeg == TravelLegs::NO_NODE)
    {
        if (trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport && !trip.quaySought)
            TravelLegs::SetQuay(trip, nullptr);
        return;
    }

    bool const approachWalk = seen.quayLeg != trip.legIdx;
    TravelRoute::Leg const& leg = trip.legs[seen.quayLeg];
    if (bot->GetMapId() != leg.from.mapId)
        return;

    data.transportScanMs = seen.nowMs;
    // The approach walk ends on the ground beside the stop (AimApproachEnd); once docked, the quay
    if (approachWalk && !trip.waypoints.empty())
        AimApproachEnd(bot, trip.waypoints.back().at, leg, trip.sight.guid);
    if (approachWalk && (trip.waypoints.empty() || trip.next + 1 < trip.waypoints.size()))
        return;
    TravelLegs::TransportSight sight;
    Transport* const transport = FindLegTransport(bot, leg, trip.sight.guid);
    if (transport)
        SeeTransport(bot, transport, trip, sight);
    TravelLegs::NoteSight(trip, sight);
    // A traced bot at its transport leg that does not look for its quay this scan, for the debug log (WT54-11: walk
    // done, no quay in 800 s at Valiance Keep): the reason, the scan's still verdict and the vessel's yards to the dock
    auto const holdQuay = [&](char const* reason)
    {
        if (approachWalk || data.place.empty() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
            return;
        float const dock =
            transport ? transport->GetExactDist2d(TravelLegs::StopFrom(leg).x, TravelLegs::StopFrom(leg).y) : -1.0f;
        if (transport && !trip.still && dock > TravelLegs::SHIP_DOCK_YARDS)
            return;  // under way far off: the common case of a wait
        LOG_DEBUG("playerbots",
                  "[TravelLifecycle] quay_hold bot={} trip={} leg={} entry={} reason={} still={} dock={:.1f} "
                  "at={:.2f},{:.2f},{:.2f}",
                  bot->GetGUID().GetCounter(), data.tripId, trip.legIdx, leg.object, reason, trip.still ? 1 : 0, dock,
                  bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    };
    if (!transport || !TravelLegs::DockedAt(trip, TravelLegs::StopFrom(leg)))
    {
        trip.quaySought = false;
        holdQuay(transport ? "undocked" : "no_transport");
        return;
    }
    if (trip.quaySought || !ReadyForTransportMove(*bot))
    {
        char const* const hold = TransportMoveHold(*bot);
        holdQuay(trip.quaySought ? "sought" : (hold ? hold : "hold"));
        return;
    }

    TransportGeometry::GroundGeometry const geometry{*bot, transport};
    Point from = TransportGeometry::PositionOf(*bot);
    if (!approachWalk)
    {
        // A bot whose approach walk ended before the deck's level was known waits off that level (WT54-10:
        // Orgrimmar's upper ramp 62.41, the platform 52.0): its quay is looked for from the ground beside the stop at
        // the deck's level, and the walk to the quay takes it there. Once per docking, so no cost per scan
        TravelRoute::Point seed{leg.from.mapId, from.X, from.Y, from.Z};
        AimApproachEnd(bot, seed, leg, trip.sight.guid);
        from = Point{seed.x, seed.y, seed.z};
    }
    bool const trace = !data.place.empty() && sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG);
    TravelLifecycle::CrossingTally tally;
    auto const shore = TravelLifecycle::FindQuay(geometry, from, TransportGeometry::PositionOf(*transport),
                                                 TransportGeometry::HullCentre(*transport), trace ? &tally : nullptr);
    if (!shore)
    {
        TravelLegs::SetQuay(trip, nullptr);
        LOG_DEBUG("playerbots", "[Route] {} found no reachable boarding floor for {}", bot->GetName(), leg.object);
        if (trace)
        {
            LogCrossingSearch(*bot, data, *transport, "quay", tally);
            // The first quay that failed only on the walk to it, and how the ground mesh joins the bot to it (WT54-7)
            auto const& corridor = tally.First[static_cast<std::size_t>(TravelLifecycle::CrossingCheck::Corridor)];
            if (corridor)
                LOG_DEBUG("playerbots",
                          "[TravelLifecycle] quay_reach bot={} at={:.2f},{:.2f},{:.2f} shore={:.2f},{:.2f},{:.2f} "
                          "reach={}",
                          bot->GetGUID().GetCounter(), from.X, from.Y, from.Z, corridor->From.X, corridor->From.Y,
                          corridor->From.Z,
                          TransportGeometry::GroundGeometry::ReachName(geometry.GroundReach(from, corridor->From)));
        }
        return;
    }

    TravelRoute::Point const quay{leg.from.mapId, shore->X, shore->Y, shore->Z};
    TravelLegs::SetQuay(trip, &quay);
    if (approachWalk)
        trip.waypoints.back().at = quay;
    LOG_DEBUG("playerbots", "[Route] {} waits for transport {} on ground at {:.2f}, {:.2f}, {:.2f}", bot->GetName(),
              leg.object, quay.x, quay.y, quay.z);
}

// The board role of the dock_foot line (LogDockFoot): the quay FindQuay finds from the wait point, with the vessel
// docked, and the deck point from there
void LogBoardDockFoot(Player* bot, NewRpgInfo::Travel const& data, Transport& transport)
{
    TravelLegs::Trip const& trip = data.trip;
    TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
    if (!leg.waitFrom || leg.rideAt + 1 >= leg.nodes.size() ||
        !TracedDockFoot(data, leg.object, leg.nodes[leg.rideAt], false))
        return;
    TravelRoute::Point seed = leg.from;
    AimApproachEnd(bot, seed, leg, trip.sight.guid);
    Point const from{seed.x, seed.y, seed.z};
    TransportGeometry::GroundGeometry const geometry{*bot, &transport};
    TravelLifecycle::CrossingTally tally;
    auto const foot = TravelLifecycle::FindQuay(geometry, from, TransportGeometry::PositionOf(transport),
                                                TransportGeometry::HullCentre(transport), &tally);
    char const* firstFail = "none";
    for (std::size_t i = 0; !foot && i < CROSSING_NAMES.size(); ++i)
        if (tally.Counts[i])
        {
            firstFail = CROSSING_NAMES[i];
            break;
        }
    LogDockFoot(leg, false, foot, geometry.DeckFrom(foot ? *foot : from), firstFail);
}

// Where a boarding starts (WT54-10): the quay the bot waits at, whose crossing FindQuay accepted, else where it
// stands. A bot within QUAY_REACH_YARDS of its quay does not walk to it, and from its own spot the crossing can differ
// (run 12, Orgrimmar's platform: accepted from the quay, blocked 0.6 yd beside it), so the board check, its deck point
// and the walk all start at the quay
Point BoardStart(Player const& bot, TravelLegs::Trip const& trip)
{
    if (trip.hasQuay && trip.quay.mapId == bot.GetMapId())
        return Point{trip.quay.x, trip.quay.y, trip.quay.z};
    return TransportGeometry::PositionOf(bot);
}

// A Board walk, for the debug log: the deck point, the bot's height and the distance
void LogBoardMove(Player* bot, uint32 entry, Point const& deck)
{
    if (!sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    LOG_DEBUG(
        "playerbots", "[Route] {} boards transport {}: deck point ({:.1f}, {:.1f}, {:.1f}), bot z {:.1f}, {:.1f} yd",
        bot->GetName(), entry, deck.X, deck.Y, deck.Z, bot->GetPositionZ(), bot->GetExactDist(deck.X, deck.Y, deck.Z));
}

// A scan at a still transport that does not board a traced bot, for the debug log: the first gate that holds it back.
// wait_z: off the wait point's floor; no_deck: no deck point; deck_z: the deck point beyond a floor's step from the
// bot; ground: not on ground; prepare: PrepareTransportMove (mounted, casting); accept: the crossing check.
void LogBoardGate(Player* bot, NewRpgInfo::Travel const& data, Transport const& transport,
                  std::optional<Point> const& deck, bool ready, bool accept)
{
    TravelLegs::Trip const& trip = data.trip;
    if (data.place.empty() || trip.legIdx >= trip.legs.size() || !sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
        return;
    float const waitDz = std::fabs(bot->GetPositionZ() - TravelLegs::WaitPoint(trip, trip.legs[trip.legIdx]).z);
    char const* gate = "accept";
    if (!deck)
        gate = waitDz > TravelLifecycle::DOCK_FLOOR_YARDS ? "wait_z" : "no_deck";
    else if (!ready)
        gate = std::fabs(deck->Z - bot->GetPositionZ()) > TravelLifecycle::DOCK_FLOOR_YARDS ? "deck_z" : "ground";
    else if (!accept)
        gate = "prepare";
    std::string const point =
        deck ? Acore::StringFormat("{:.2f},{:.2f},{:.2f}", deck->X, deck->Y, deck->Z) : std::string("none");
    char const* const hold = TransportMoveHold(*bot);
    LOG_DEBUG("playerbots",
              "[TravelLifecycle] board_gate bot={} trip={} leg={} entry={} at={:.2f},{:.2f},{:.2f} gate={} "
              "wait_dz={:.2f} deck={} mounted={} hold={}",
              bot->GetGUID().GetCounter(), data.tripId, trip.legIdx, transport.GetEntry(), bot->GetPositionX(),
              bot->GetPositionY(), bot->GetPositionZ(), gate, waitDz, point, bot->IsMounted() ? 1 : 0,
              hold ? hold : "none");
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
        Transport* const transport = FindLegTransport(bot, trip.legs[trip.legIdx], trip.sight.guid);
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
    sight.guid = transport->GetGUID().GetRawValue();
    sight.isStatic = transport->IsStaticTransport();
    sight.at = {bot->GetMapId(), transport->GetPositionX(), transport->GetPositionY(), transport->GetPositionZ()};
    TravelLegs::TransportSight const& before = trip.sight;
    if (before.found && before.guid == sight.guid && before.at.mapId == sight.at.mapId)
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
bool OffFirstBegins(Player* bot, NewRpgInfo::Travel& data, TravelLegs::OffFirstEnd end = TravelLegs::OffFirstEnd::None,
                    char const* why = nullptr, char const* source = nullptr)
{
    Transport const* const ridden = bot->GetTransport();
    if (!ridden)
        return false;
    TravelLegs::Trip& trip = data.trip;
    if (trip.offFirst.sinceMs)
        return true;
    uint32 const period = TransportGeometry::Period(*ridden);
    if (!TravelLegs::BeginOffFirst(trip, ridden->GetEntry(), ridden->GetGUID().GetRawValue(), period, getMSTime(), end,
                                   why, source))
        return false;
    TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
    SendTransportNotes(bot, trip);
    char const* const waiting = end == TravelLegs::OffFirstEnd::Arrive  ? "arrival"
                                : end == TravelLegs::OffFirstEnd::Abort ? "abort"
                                                                        : "walk or route";
    LOG_DEBUG("playerbots", "[Route] {} gets off transport {} first (the {} waits)", bot->GetName(), ridden->GetEntry(),
              waiting);
    return true;
}

// OffFirst shares the normal local-frame exit, including membership acknowledgment.
TravelLegs::OffStep GetOffFirst(Player* bot, NewRpgInfo::Travel& data)
{
    TravelLegs::Trip& trip = data.trip;
    Transport* const ridden = bot->GetTransport();
    TravelLegs::Observation seen;
    seen.nowMs = getMSTime();
    seen.bot = {bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    auto const membership = TransportGeometry::ReadMembership(*bot);
    seen.aboardGuid = membership.Guid;
    seen.membershipAvailable = membership.Available;
    if (membership.Available)
        seen.passengerOutcome = TakePassengerOutcome(*bot, data);
    if (getMSTimeDiff(data.transportScanMs, seen.nowMs) >= TravelLegs::TRANSPORT_SCAN_MS)
    {
        data.transportScanMs = seen.nowMs;
        seen.transport.scanned = true;
        if (ridden && membership.Available && membership.Guid == trip.offFirst.guid)
            SeeTransport(bot, ridden, trip, seen.transport);
    }
    bool const wasStill = trip.still;
    TravelLegs::OffStep const step = TravelLegs::StepOffFirst(trip, seen);
    if (seen.transport.scanned && ridden && membership.Available)
    {
        if (step == TravelLegs::OffStep::Disembark)
        {
            // Off where it stands, onto the ground beside the vessel: the trip's goal may be another vessel stop
            // (no ground) or on another map, and the pending operation plans from the shore
            Point const onward =
                GroundBesideStop(bot->GetMap(), TransportGeometry::PositionOf(*ridden), bot->GetPositionZ())
                    .value_or(TransportGeometry::PositionOf(*bot));
            WalkOff(*bot, data, *ridden, std::nullopt, std::vector<Point>{onward});
        }
        else if (step == TravelLegs::OffStep::Wait && wasStill)
        {
            trip.transportNotes.push_back({TravelLegs::TransportEvent::StuckAboard, ridden->GetEntry(), 0});
            trip.exitSought = false;
            TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
        }
    }
    SendTransportNotes(bot, trip);
    return step;
}

// The last-resort hop (WT.14): a few yards onto the docked deck, only unseen (the scan's sight check), counted (its
// Transport hop event went with the trip's notes; the honesty row for `wlog report honesty`)
void HopOnto(Player* bot, NewRpgInfo::Travel& data, Transport const& transport, Point deck, uint32 misses)
{
    deck.Z += HOP_LIFT_YARDS;
    PassengerMotion motion = Intent(*bot, transport, MotionKind::Board, deck);
    motion.ContactOnly = true;
    bot->GetMotionMaster()->Clear();
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    if (bot->TeleportTo(bot->GetMapId(), deck.X, deck.Y, deck.Z, bot->GetOrientation()))
    {
        data.passengerMotion = motion;
        EventLog::Honesty(bot, TravelLegs::HOP_HONESTY_KIND, TravelLegs::HOP_HONESTY_SOURCE, motion.Entry, misses);
    }
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

// A graph link's walk (WT58): its trip decision row, walk_link once the walk moved on past it or walk_link_failed when
// the leg failed on it: how (stored, nodes, fallback, refused), its moves, its ends' node ids and its hops
void LogWalkLink(Player* bot, NewRpgInfo::Travel const& data, TravelWalk::LinkWalk const& walk, bool done)
{
    if (!walk.Active())
        return;
    TravelGraph const& graph = TravelGraph::Instance();
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Trip, done ? "walk_link" : "walk_link_failed",
                       TravelWalk::RowWord(walk), static_cast<float>(walk.moves),
                       {{"from", static_cast<float>(graph.GetNode(walk.from).id)},
                        {"to", static_cast<float>(graph.GetNode(walk.to).id)},
                        {"hops", static_cast<float>(walk.hops)}},
                       data.tripId);
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
bool PassengerOwnsTravel(Player const& bot, NewRpgInfo::Travel const& data)
{
    return data.PassengerOwnsProgress() || bot.GetTransport() || bot.HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) ||
           bot.IsInFlight() || bot.GetVehicle();
}
}  // namespace

bool TravelKeepsOnFoot(PlayerbotAI* botAI)
{
    auto const* const data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (!data)
        return false;
    TravelLegs::Trip const& trip = data->trip;
    if (trip.offFirst.sinceMs)
        return true;
    // A transport leg goes on foot from its dock on (WT54-7: also while it waits there, SeekQuay)
    return trip.legIdx < trip.legs.size() && trip.legs[trip.legIdx].type == TravelRoute::LegType::Transport;
}

bool NewRpgTravelAction::ConsumeScheduledWork(PlayerbotAI* botAI)
{
    using TravelProgress::Overdue;
    using TravelProgress::Work;
    auto* const data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    Overdue const reason = data ? data->progress.Reason() : Overdue::None;
    bool const lootExpired = botAI->LootInteractionExpired();
    if (reason == Overdue::None && !lootExpired)
        return false;

    Player* const bot = botAI->GetBot();
    bool const walkFailure = reason == Overdue::Walk || reason == Overdue::OffMesh || reason == Overdue::RouteDeadline;
    if (reason != Overdue::None &&
        ((walkFailure && PassengerOwnsTravel(*bot, *data)) || !TransportGeometry::ReadMembership(*bot).Available))
        return false;

    bool const canMove = bot->IsAlive() && !bot->IsInCombat() && botAI->CanMove();
    Work const work = TravelProgress::ScheduledWork(canMove, reason, botAI->HasLootInteraction(), lootExpired);
    if (work == Work::Recover)
        return NewRpgTravelAction(botAI).StepTravel(data, data->progress.TakeReason());
    return work == Work::DeferLoot && botAI->ConsumeLootExpiry();
}

bool NewRpgTravelAction::Execute(Event /*event*/)
{
    auto* const data = std::get_if<NewRpgInfo::Travel>(&botAI->rpgInfo.data);
    if (!data || !bot->IsAlive() || bot->IsInCombat() || (!bot->IsInFlight() && !botAI->CanMove()))
        return false;
    if (ConsumeScheduledWork(botAI))
        return true;
    if (botAI->HasLootInteraction() && !PassengerOwnsTravel(*bot, *data))
        return false;
    return StepTravel(data, TravelProgress::Overdue::None);
}

bool NewRpgTravelAction::StepTravel(NewRpgInfo::Travel* data, TravelProgress::Overdue overdue)
{
    using TravelProgress::Overdue;
    auto const membership = TransportGeometry::ReadMembership(*bot);
    if (!membership.Available && !bot->IsInFlight())
        return true;
    // A trip operation requested aboard waits for an acknowledged exit within one period.
    if (data->trip.offFirst.sinceMs)
    {
        TravelLegs::OffStep const step = GetOffFirst(bot, *data);
        if (step == TravelLegs::OffStep::Wait || step == TravelLegs::OffStep::Disembark)
            return true;
        if (step == TravelLegs::OffStep::GiveUp || step == TravelLegs::OffStep::Lost)
        {
            bool const expired = step == TravelLegs::OffStep::GiveUp;
            botAI->TraceTravelInteraction("route_recovery", expired ? "stuck_aboard" : "transport_lost");
            EndTrip(*data, expired ? "stuck aboard" : "transport lost",
                    expired ? "abort:stuck_aboard" : "abort:transport_lost");
            return true;
        }
        TravelLegs::OffFirst const& off = data->trip.offFirst;
        if (off.end == TravelLegs::OffFirstEnd::Arrive)
            return Arrive(*data);
        if (off.end == TravelLegs::OffFirstEnd::Abort)
            EndTrip(*data, off.why, off.source);
        return true;
    }
    Transport* const ridden = bot->GetTransport();
    TravelLegs::Trip const& trip = data->trip;
    // A route may have been superseded aboard. Only the route with this ride or its own boarding result may use it.
    bool const ownsBoard = data->passengerMotion && data->passengerMotion->Kind == MotionKind::Board &&
                           data->passengerMotion->Transport == membership.Guid;
    // Taken aboard by the leg's vessel at the leg's start (its deck docked under the waiting bot) is the boarding
    TravelRoute::Point const standing{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    // or on the walk to that leg's start (the walk is done, WT54-6); not by an elevator or the tram
    bool const takenAboard =
        ridden && (TravelLegs::TakenAboard(trip, standing, membership.Entry) ||
                   (!ridden->IsStaticTransport() && TravelLegs::WalkTakenAboard(trip, standing, membership.Entry)));
    if (ridden && !takenAboard && (!AtTransportLeg(trip) || (!trip.passengerGuid && !ownsBoard)))
    {
        OffFirstBegins(bot, *data);
        return true;
    }
    uint32 const quayLeg = trip.boarding == TravelLegs::Boarding::Approach ? MotionQuayLeg(trip) : TravelLegs::NO_NODE;
    if (overdue == Overdue::RouteDeadline)
    {
        botAI->TraceTravelInteraction("route_recovery", "deadline");
        EndTrip(*data, "deadline", "abort:deadline");
        return true;
    }
    if (overdue == Overdue::Walk || overdue == Overdue::OffMesh)
    {
        if (!data->progress.AttemptedWalk())
        {
            botAI->TraceTravelInteraction("route_recovery", "stalled");
            EndTrip(*data, "stalled", "abort:stalled");
            return true;
        }

        if (data->progress.Owns(bot->movespline->GetId()) && !bot->movespline->Finalized())
            bot->StopMoving();
        bool const offMesh = GiveUpMove(botAI->rpgInfo.moveFarPos);
        if (!offMesh && !data->CurrentLeg() && data->bindInn)
        {
            botAI->TraceTravelInteraction("route_recovery", "stuck");
            return Arrive(*data, true);
        }
        return LegFailed(*data, offMesh ? TravelLegs::FailReason::OffMesh : TravelLegs::FailReason::Stuck);
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
        return RequestRoute(*data);
    }
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
    seen.nowMs = getMSTime();
    seen.activeLegMs = data->progress.LegMs();
    seen.aboard = membership.Entry;
    seen.aboardGuid = membership.Guid;
    seen.membershipAvailable = membership.Available;
    seen.boardMotion = ownsBoard;
    seen.passengerOutcome = TakePassengerOutcome(*bot, *data);
    seen.quayLeg = quayLeg;
    SeekQuay(bot, *data, seen);
    // The transport scan (WT19): once a TRANSPORT_SCAN_MS while a transport leg is past its approach, on the bot's
    // map only. The deck point and the Transport* live for this tick only
    std::optional<Point> deck;
    Transport* transport = nullptr;
    float moved = -1.0f;  // the transport's yards since the scan before on this map; -1 none
    if (AtTransportLeg(trip) && getMSTimeDiff(data->transportScanMs, seen.nowMs) >= TravelLegs::TRANSPORT_SCAN_MS)
    {
        data->transportScanMs = seen.nowMs;
        TravelLegs::TransportSight& sight = seen.transport;
        sight.scanned = true;
        uint64 const expected = data->passengerMotion ? data->passengerMotion->Transport : trip.sight.guid;
        transport = FindLegTransport(bot, trip.legs[trip.legIdx], expected);
        if (transport)
        {
            moved = SeeTransport(bot, transport, trip, sight);
            TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
            if (!ridden && !sight.moving && !data->passengerMotion &&
                std::fabs(seen.bot.z - TravelLegs::WaitPoint(trip, leg).z) <= TravelLifecycle::DOCK_FLOOR_YARDS)
            {
                Point const from = BoardStart(*bot, trip);
                deck = TransportGeometry::GroundGeometry{*bot, transport}.DeckFrom(from);
                if (deck)
                {
                    seen.boardingReady = std::fabs(deck->Z - from.Z) <= TravelLifecycle::DOCK_FLOOR_YARDS &&
                                         TransportGeometry::GroundGeometry{*bot}.OnGround(from);
                    if (seen.boardingReady && TravelLegs::WantsSightCheck(trip) && !bot->IsMounted() &&
                        ReadyForTransportMove(*bot))
                        sight.unseen = !sRandomPlayerbotMgr.IsTeleportSeen(
                            bot, WorldPosition(bot->GetMapId(), deck->X, deck->Y, deck->Z));
                }
            }
        }
    }
    TravelLegs::NoteTick(data->trip, bot->IsMounted());
    TravelLegs::LegRecord const recordBefore = trip.record;  // Advance starts a new leg's record
    uint32 const legBefore = trip.legIdx;
    uint32 const legStartBefore = trip.legStartMs;
    uint32 const waypointBefore = trip.next;
    uint16 const climbsBefore = trip.climbs;
    float const peakBefore = trip.peakZ;
    bool const wasWalk = data->CurrentLeg() && data->CurrentLeg()->type == TravelRoute::LegType::Walk &&
                         !TravelLegs::IsNoOp(*data->CurrentLeg());
    TravelWalk::LinkWalk const walkBefore = trip.walk;
    TravelLegs::Decision next = TravelLegs::Advance(data->trip, seen, GraphNodeAt);
    bool const legChanged = trip.legIdx != legBefore;
    // The graph link walked until now ends with its row once the walk moved on past it (WT58)
    if (walkBefore.Active())
    {
        auto const [linkFrom, linkTo] = WalkLinkEnds(trip);
        if (legChanged || linkFrom != walkBefore.from || linkTo != walkBefore.to)
        {
            LogWalkLink(bot, *data, walkBefore, true);
            data->trip.walk = {};
        }
    }
    bool const waypointReached =
        wasWalk && (legChanged || trip.next > waypointBefore) && data->progress.WaypointReached();
    if (waypointReached)
        botAI->TraceTravelInteraction("route_progress", "waypoint", ObjectGuid::Empty, 0, "none", 0.0f,
                                      std::pair{legBefore, legStartBefore});
    if (legChanged)
        data->progress.StartLeg(!wasWalk || waypointReached);
    if (data->passengerMotion &&
        (trip.legIdx != legBefore || (next.act != TravelLegs::Act::Board && next.act != TravelLegs::Act::Disembark)))
        TransportGeometry::DiscardMotion(*bot, data->passengerMotion);
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
    }
    // A test's flyfail or flyabort row: past the leg's middle and in the air the fly leg fails as its timeout would
    // (the way down after a failure in the air, upkeep WU26b); flyabort spends the trip's replans first. One bit test
    // outside such a test
    uint16 const constraints = botAI->rpgInfo.constraints;
    if (next.act == TravelLegs::Act::Fly && (constraints & TestConstraints::FLY_FAILS))
    {
        TravelRoute::Leg const& leg = trip.legs[trip.legIdx];
        auto const [groundZ, waterZ] = CheckMountStateAction::FloorBelow(bot);
        bool const inTheAir = TravelLegs::BodyZ(seen.bot.z, groundZ, waterZ) < seen.bot.z;
        if (TestConstraints::FlyLegFails(constraints, std::hypot(seen.bot.x - leg.from.x, seen.bot.y - leg.from.y),
                                         std::hypot(leg.to.x - leg.from.x, leg.to.y - leg.from.y), inTheAir))
        {
            if (TestConstraints::FlyFailAborts(constraints))
                data->trip.replans = TravelLegs::MAX_REPLANS;
            LOG_INFO("playerbots", "[Route] {} fly leg {}/{} fails in the air by the test constraint {}",
                     bot->GetName(), trip.legIdx + 1, trip.legs.size(),
                     TestConstraints::Format(constraints & TestConstraints::FLY_FAILS));
            return LegFailed(*data, TravelLegs::FailReason::Timeout);
        }
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
            if (trip.legIdx != legBefore && seen.quayLeg != trip.legIdx && MotionQuayLeg(trip) != TravelLegs::NO_NODE)
                return true;  // the new approach gets a quay observation before walking
            // Along a graph link: its stored points (WT58); the engine's "no path" at its far node refused it
            if (trip.walk.mode == TravelWalk::Mode::Refused)
                return LegFailed(*data, TravelLegs::FailReason::NoPath);
            next.target = WalkLinkTarget(data->trip, seen.bot, next.target);
            data->trip.lastTarget = next.target;
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
            if (transport)
                LogBoardDockFoot(bot, *data, *transport);
            if (transport && deck && seen.boardingReady && !data->passengerMotion && PrepareTransportMove(*bot))
            {
                Point const from = BoardStart(*bot, data->trip);
                if (TravelLifecycle::AcceptBoard(TransportGeometry::GroundGeometry{*bot, transport}, from, *deck))
                {
                    LogBoardMove(bot, transport->GetEntry(), *deck);
                    IssueTransportMove(*bot, *data, Intent(*bot, *transport, MotionKind::Board, *deck), {from});
                    if (data->passengerMotion)
                        data->trip.boardCrossing = TravelLegs::StoreCrossing(
                            transport->GetGUID().GetRawValue(), TransportGeometry::PoseOf(*transport), *deck, from);
                }
                else
                    LogBoardGate(bot, *data, *transport, deck, true, true);
            }
            else if (transport && !data->passengerMotion && !seen.transport.moving)
                LogBoardGate(bot, *data, *transport, deck, seen.boardingReady, false);
            return true;
        case TravelLegs::Act::Hop:
            data->trip.boardCrossing = {};  // a hop is no crossing to walk back
            if (transport && deck)
                HopOnto(bot, *data, *transport, *deck, trip.misses);
            return true;
        case TravelLegs::Act::Disembark:
            if (ridden && seen.transport.scanned && seen.transport.guid == membership.Guid)
            {
                Point const dest{data->dest.GetPositionX(), data->dest.GetPositionY(), data->dest.GetPositionZ()};
                TravelRoute::Leg const& rideLeg = trip.legs[trip.legIdx];
                // Off at the stop's wait point where it has one: the far one, or after no exit the one it waited on
                Point const target{next.target.x, next.target.y, next.target.z};
                bool const back = trip.boarding == TravelLegs::Boarding::Back;
                bool const atWaitPoint =
                    (back ? rideLeg.waitFrom : rideLeg.waitTo) && next.target.mapId == bot->GetMapId();
                std::optional<Point> const waitPoint = atWaitPoint ? std::optional<Point>{target} : std::nullopt;
                if (back)  // the quay or wait point it waited on
                    WalkOff(*bot, *data, *ridden, waitPoint, std::vector<Point>{target});
                else
                {
                    LogExitDockFoot(*bot, *data, *ridden);
                    WalkOff(*bot, *data, *ridden, waitPoint,
                            OnwardRoute(trip, bot->GetMap(), bot->GetMapId(), dest, bot->GetPositionZ()));
                }
            }
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
    if (!TestConstraints::TaxiAllowed(botAI->rpgInfo.constraints))
        snapshot.knownTaxi.fill(0);
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
    TravelLegs::RouteWait const waiting{snapshot.requestId, snapshot.position.mapId, snapshot.takenMs, true,
                                        snapshot.position};
    auto const submit = [&]
    {
        return TravelRoute::RouteService::Instance().Submit(bot->GetGUID().GetRawValue(), request,
                                                            TravelRoute::ConfiguredParams());
    };
    if (!TravelProgress::SubmitRoute(data.route, waiting, submit))
    {
        EndTrip(data, "no route service", "abort:no_service");  // the graph is not loaded yet: never wait
        return true;
    }
    botAI->TraceTravelInteraction("route_request", "issued");
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
        botAI->TraceTravelInteraction("route_recovery", "resubmit");
        LOG_DEBUG("playerbots", "[Route] {} {} ({} ms since the request), asked again", bot->GetName(), why,
                  now - data.route.requestedMs);
        return RequestRoute(data);
    };
    if (!answer)
        return TravelLegs::WaitExpired(data.route, now) ? askAgain("got no answer") : true;
    TravelRoute::Point const current{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    switch (TravelLegs::Judge(data.route, answer->requestId, current, now))
    {
        case TravelLegs::Verdict::Drop:
            return TravelLegs::WaitExpired(data.route, now) ? askAgain("got only a stale answer") : true;
        case TravelLegs::Verdict::Refresh:
            return askAgain(bot->GetMapId() != data.route.mapId ? "changed maps" : "moved or took the answer too late");
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
    data.progress.AcceptRoute();
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
    botAI->TraceTravelInteraction("route_recovery", TravelLegs::FailWord(reason));
    TransportGeometry::DiscardMotion(*bot, data.passengerMotion);
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

    // The graph link the leg failed on (WT58): its row; a refusal's line names the link's ends
    if (trip.walk.Active())
    {
        if (reason == TravelLegs::FailReason::NoPath)
        {
            TravelGraph const& graph = TravelGraph::Instance();
            LOG_DEBUG("playerbots", "[Route] {} walk leg {}/{} link {} ({}) -> {} ({}) refused: no path",
                      bot->GetName(), trip.legIdx + 1, trip.legs.size(), graph.GetNode(trip.walk.from).id,
                      graph.GetName(trip.walk.from), graph.GetNode(trip.walk.to).id, graph.GetName(trip.walk.to));
        }
        LogWalkLink(bot, data, trip.walk, false);
        trip.walk = {};
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
    data.progress.Recover();
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

bool NewRpgTravelAction::Arrive(NewRpgInfo::Travel& data, bool stuck)
{
    // Not on a ship's deck: the bot gets off first, then arrives (WT19-d)
    if (OffFirstBegins(bot, data, TravelLegs::OffFirstEnd::Arrive))
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
        WorldTravel::InnStep const step = WorldTravel::InnNext(yards, stuck, data.bindTries);
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
    if (OffFirstBegins(bot, data, TravelLegs::OffFirstEnd::Abort, why, source))
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
