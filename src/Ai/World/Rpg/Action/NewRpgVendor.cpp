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
#include "Creature.h"
#include "ErrandRules.h"
#include "EventLog.h"
#include "EventLogFormat.h"
#include "InventorySlots.h"
#include "ItemDemandShop.h"
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
#include "ProfessionConfig.h"
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

// The riding need's mount at the vendor that sells it (upkeep §3.8, WU14): the trip's needs (new if shopping spent
// gold), the rank bought by worth whose mount this vendor sells, bought through the core's purchase at its gold-only
// row within the need's funded share and learned as the item's own use learns it (the learning spell in its second
// spell; the item is used up). A rank not known yet waits, as the core's item test wants the riding skill: the next
// errand check sends the bot for the mount alone. A mount item the bot carries already is learned, not bought again.
// planned: the rank whose mount the trip planned at this stop (0 none): that purchase always ends with a word
void MountAtVendor(PlayerbotAI* botAI, Creature* vendor, std::shared_ptr<Planner::UpkeepIndex const> const& shared,
                   uint32 planned)
{
    Player* bot = botAI->GetBot();
    Planner::UpkeepIndex const& index = *shared;
    std::shared_ptr<UpkeepNeeds::VisitNeeds const> const visit = ErrandNeeds(botAI, shared);
    Planner::Snapshot const& snapshot = visit->snapshot;
    UpkeepNeeds::NeedList const& needs = visit->needs;
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
    {
        // Nothing to buy here: only the stop planned for a mount says why (MountStopEnd)
        UpkeepNeeds::RankValue const* const rank = UpkeepNeeds::RankOf(needs, planned);
        if (!rank)
            return;
        uint32 const sold = UpkeepNeeds::MountSoldBy(index, snapshot.race, snapshot.cls, *rank, entry);
        uint64 const price =
            static_cast<uint64>(std::floor(static_cast<float>(rank->mount) * bot->GetReputationPriceDiscount(vendor)));
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "skip",
                           UpkeepNeeds::MountStopEnd(*rank, sold != 0, price, bot->GetMoney()),
                           static_cast<float>(price), {{"gold", static_cast<float>(bot->GetMoney())}},
                           sold ? sold : rank->mountItem);
        return;
    }

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
    {
        // The mount's price goes to its SpellLearn row as a lesson's (none when the item was bought before)
        EventLog::LessonScope const lesson(bot, paid);
        bot->learnSpell(learn);
    }
    if (!bot->HasSpell(learn))
    {
        // The item stays in the bags: the next visit learns it without paying again
        skip("not_learned");
        return;
    }

    bot->DestroyItemCount(item, 1, true);
    EventLog::Emit(bot, EventLog::EventType::RidingBought, "mount", item, int64(paid));
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "buy",
                       static_cast<float>(value->Price()) <= value->worth ? "worth" : "want", static_cast<float>(paid),
                       {{"mount", static_cast<float>(value->rank.skill)}, {"funded", static_cast<float>(need->funded)}},
                       item);
}
}  // namespace

namespace NewRpgInternal
{

// The shopping list at the errand's vendor (upkeep WU6, §3.6): the needs from a fresh snapshot (the sell and the
// repair changed the gold), each stock in Fund's order bought in whole lots within its funded share (PlanShopping),
// the vendor's own rows rechecked (gold only, a limited row's live count). What it buys, the sell keeps: one stock rule
// (UpkeepNeeds::KeepOf, WU28). A repair paid at this stop already spent what the gold pays; the gear left worn costs
// more than the rest, so it holds no gold back here. A use item's line names demand's slot (ItemDemandShop.h). A
// profession tool's rows are the profession's (WR5b): `tool` when bought, else `skip` with gold_short, no_source (this
// vendor does not sell it) or access (the core refused). A failed purchase skips the item; the bot goes on. At most
// one row per stock kind
void ShopAtVendor(PlayerbotAI* botAI, Creature* vendor, bool repaired, uint32 mountRank)
{
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    VendorItemData const* rows = vendor->GetVendorItems();
    if (!index || !rows || !rows->GetItemCount())
        return;

    Player* bot = botAI->GetBot();
    std::shared_ptr<UpkeepNeeds::VisitNeeds const> const visit = ErrandNeeds(botAI, index);
    // A part repair leaves worn gear priced in the list: the shopping is planned without it (its own list, not kept)
    std::optional<Planner::Snapshot> unworn;
    std::optional<UpkeepNeeds::NeedList> unwornNeeds;
    if (repaired && visit->snapshot.repairCost)
    {
        unworn = visit->snapshot;
        unworn->repairCost = 0;
        unwornNeeds = Planner::ComputeErrandNeeds(botAI, *unworn, *index);
    }
    Planner::Snapshot const& snapshot = unworn ? *unworn : visit->snapshot;
    UpkeepNeeds::NeedList const& needs = unwornNeeds ? *unwornNeeds : visit->needs;
    std::vector<UpkeepNeeds::ShopLine> const lines =
        UpkeepNeeds::PlanShopping(needs, *index, snapshot, vendor->GetEntry());
    for (UpkeepNeeds::ShopLine const& line : lines)
    {
        auto const [word, fundWord, fund] = ItemUse::ViewShopLine(line);
        bool const tool = line.kind == Planner::UpkeepStock::Tool;
        // A tool line of a recipe item is the skill path's recipe (WR14b): its rows say recipe
        ItemTemplate const* const lineProto = tool ? sObjectMgr->GetItemTemplate(line.item) : nullptr;
        Professions::Choice const toolChoice = lineProto && lineProto->Class == ITEM_CLASS_RECIPE
                                                   ? Professions::Choice::Recipe
                                                   : Professions::Choice::Tool;
        auto const toolRow = [bot, &line](Professions::Choice choice, Professions::Reason reason, uint64 copper)
        {
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Profession, Professions::ChoiceWord(choice),
                               Professions::ReasonWord(reason), static_cast<float>(copper),
                               {{"price", static_cast<float>(line.wanted)},
                                {"funded", static_cast<float>(line.funded)},
                                {"gold", static_cast<float>(bot->GetMoney())}},
                               line.item);
        };
        if (line.outcome == UpkeepNeeds::ShopOutcome::Budget && tool)
        {
            toolRow(Professions::Choice::Skip, Professions::Reason::GoldShort, line.wanted);
            continue;
        }
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
        if ((slot == rows->GetItemCount() || !lots) && tool)
        {
            toolRow(Professions::Choice::Skip, Professions::Reason::NoSource, line.wanted);
            continue;
        }
        if (slot == rows->GetItemCount() || !lots)
        {
            char const* const reason = slot == rows->GetItemCount() ? "not_sold" : "sold_out";  // ItemDemandShop.h
            WorldLog::Decision(
                bot, WorldLogFormat::DecisionKind::Upkeep, "skip", reason, static_cast<float>(line.wanted),
                {{word, static_cast<float>(line.units)}, {"target", static_cast<float>(line.target)}}, line.item);
            continue;
        }

        uint32 const moneyBefore = bot->GetMoney();
        uint32 const bought = BuyLots(bot, vendor, slot, line.item, lots);
        uint32 const paid = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0;
        if (!bought && tool)
        {
            toolRow(Professions::Choice::Skip, Professions::Reason::Access, line.copper);
            continue;
        }
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

        if (tool)
        {
            toolRow(toolChoice, Professions::Reason::Access, paid);
            continue;
        }
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "buy", line.due ? "stock_low" : "top_up",
                           static_cast<float>(paid),
                           {{word, static_cast<float>(bought * line.lotSize)},
                            {"target", static_cast<float>(line.target)},
                            {fundWord, static_cast<float>(fund)}},
                           line.item);
    }

    // The riding need's mount, when this vendor sells it (WU14), with the gold the stock left: MountAtVendor takes
    // this vendor's own mount for the bought rank, so any vendor of the errand may be the one
    // A planned stop runs it without a need too: the money may be spent since the trip was planned
    if (mountRank || std::any_of(needs.needs.begin(), needs.needs.end(),
                                 [](UpkeepNeeds::Need const& need) { return need.kind == UpkeepKind::Riding; }))
        MountAtVendor(botAI, vendor, index, mountRank);
}
}  // namespace NewRpgInternal
