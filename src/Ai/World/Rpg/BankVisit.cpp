/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankVisit.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "BankMailConfig.h"
#include "BankMailService.h"
#include "BankMailState.h"
#include "BankRules.h"
#include "BankRulesInput.h"
#include "Creature.h"
#include "DBCStores.h"
#include "EventLog.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Visit.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"

namespace BankMail
{
namespace
{
// The bot looks for the banker once it is this close to the spawn (the spawn is the banker's own position)
constexpr float BANKER_SEARCH_YARDS = 20.0f;
// The nearest banker spawns on the map looked through for one whose faction serves the bot
constexpr size_t BANKER_CANDIDATES = 32;

bool Contains(std::vector<uint32> const& guids, uint32 guid)
{
    return std::find(guids.begin(), guids.end(), guid) != guids.end();
}

template <typename Verdicts>
auto VerdictOf(Verdicts& verdicts, uint32 itemGuid) -> decltype(&verdicts.front())
{
    auto verdict = std::find_if(verdicts.begin(), verdicts.end(),
                                [itemGuid](BankVerdict const& known) { return known.itemGuid == itemGuid; });
    return verdict == verdicts.end() ? nullptr : &*verdict;
}

// The first plan's verdicts as they stand before any move: a planned withdrawal leaves the item in the bank, a
// planned deposit keeps it in the bags, until the move really happens
std::vector<BankVerdict> Unmoved(std::vector<BankVerdict> verdicts)
{
    for (BankVerdict& verdict : verdicts)
    {
        if (verdict.choice == BankChoice::Withdraw)
            verdict.choice = BankChoice::Leave;
        else if (verdict.choice == BankChoice::Deposit)
            verdict.choice = BankChoice::Keep;
    }
    return verdicts;
}

}  // namespace

// Item moves: the bot's own map thread inside its AI update only (the core runs the bank opcodes in place,
// Opcodes.cpp); never on a planner worker, never for another bot. The bodies of the core's handlers
// (WorldSession::HandleAutoStoreBankItemOpcode, HandleAutoBankItemOpcode); the item may be merged and gone after.
InventoryResult Withdraw(Player* bot, Item* item)
{
    ItemPosCountVec dest;
    InventoryResult const result = bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
    if (result != EQUIP_ERR_OK)
        return result;

    bot->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
    if (Item const* stored = bot->StoreItem(dest, item, true))
        bot->ItemAddedQuestCheck(stored->GetEntry(), stored->GetCount());
    return EQUIP_ERR_OK;
}

InventoryResult Deposit(Player* bot, Item* item)
{
    ItemPosCountVec dest;
    InventoryResult const result = bot->CanBankItem(NULL_BAG, NULL_SLOT, dest, item, false);
    if (result != EQUIP_ERR_OK)
        return result;
    if (dest.size() == 1 && dest[0].pos == item->GetPos())
        return EQUIP_ERR_NONE;

    bot->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
    bot->ItemRemovedQuestCheck(item->GetEntry(), item->GetCount());
    bot->BankItem(dest, item, true);
    bot->UpdateTitansGrip();
    return EQUIP_ERR_OK;
}

namespace
{
// Professions WR17: the body of WorldSession::HandleBuyBankSlotOpcode (the visit's reach is the handler's CanUseBank):
// the next bank bag slot at its price in the core's table, never beyond the engine's slots
bool BuyBankSlot(Player* bot)
{
    uint32 const slot = bot->GetBankBagSlotCount() + 1;
    if (slot > BANK_BAG_SLOTS)
        return false;
    BankBagSlotPricesEntry const* entry = sBankBagSlotPricesStore.LookupEntry(slot);
    if (!entry || !bot->HasEnoughMoney(entry->price))
        return false;

    bot->SetBankBagSlotCount(static_cast<uint8>(slot));
    bot->ModifyMoney(-int32(entry->price));
    bot->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BUY_BANK_SLOT);
    return true;
}

// Professions WR17: a carried empty bag into the first bought bank bag slot that holds none, as Player::SwapItem moves
// an item to an empty bank slot (CanBankItem checks the slot is bought and the bag usable)
InventoryResult PlaceBankBag(Player* bot, CarriedBag const& carried)
{
    Item* bag = bot->GetItemByPos(carried.bag, carried.slot);
    if (!bag || bag->GetGUID().GetCounter() != carried.itemGuid)
        return EQUIP_ERR_ITEM_NOT_FOUND;

    for (uint32 i = 0; i < bot->GetBankBagSlotCount() && i < BANK_BAG_SLOTS; ++i)
    {
        uint8 const slot = static_cast<uint8>(BANK_SLOT_BAG_START + i);
        if (bot->GetBagByPos(slot))
            continue;
        ItemPosCountVec dest;
        InventoryResult const result = bot->CanBankItem(INVENTORY_SLOT_BAG_0, slot, dest, bag, false);
        if (result != EQUIP_ERR_OK)
            return result;
        bot->RemoveItem(carried.bag, carried.slot, true);
        bot->ItemRemovedQuestCheck(bag->GetEntry(), bag->GetCount());
        bot->BankItem(dest, bag, true);
        return EQUIP_ERR_OK;
    }
    return EQUIP_ERR_MUST_PURCHASE_THAT_BAG_SLOT;
}

// Professions WR17: one bank-space step before the item moves. True when the bot bought a slot or put a bag in one
// (the visit goes on in a later step); else the rule is done for the visit, and a decision that left something out,
// or a refused step, is kept for the visit's rows
bool SpaceStep(Player* bot, BankVisitData& visit, BankIn const& in, BankPlan const& plan)
{
    SpaceAnswer answer = DecideBankSpace(in, plan);
    if (answer.choice == SpaceChoice::BuySlot || answer.choice == SpaceChoice::PlaceBag)
    {
        bool const buy = answer.choice == SpaceChoice::BuySlot;
        InventoryResult const result =
            buy ? (BuyBankSlot(bot) ? EQUIP_ERR_OK : EQUIP_ERR_NOT_ENOUGH_MONEY) : PlaceBankBag(bot, in.space.bag);
        answer.slots = bot->GetBankBagSlotCount();
        if (result == EQUIP_ERR_OK)
        {
            visit.space.push_back(answer);
            return true;
        }
        LOG_DEBUG("playerbots", "[Bank mail] {} bank visit: {} refused, error {}", bot->GetName(),
                  SpaceChoiceWord(answer.choice), static_cast<uint32>(result));
        answer.choice = SpaceChoice::NoSlot;
        answer.reason = SpaceReason::Refused;
    }
    visit.spaceDone = true;
    if (answer.choice == SpaceChoice::NoSlot)
        visit.space.push_back(answer);
    return false;
}

// One move of the plan, resolved from the item's live place; a refused move is never tried again on this visit
void MoveItem(Player* bot, BankVisitData& visit, BankPlan const& plan, BankStep const& next)
{
    Item* item = bot->GetItemByPos(next.bag, next.slot);
    if (!item || item->GetGUID().GetCounter() != next.itemGuid)
    {
        visit.refused.push_back(next.itemGuid);  // not where the plan saw it this tick: left quietly
        return;
    }

    bool const withdraw = next.kind == BankChoice::Withdraw;
    InventoryResult const result = withdraw ? Withdraw(bot, item) : Deposit(bot, item);
    BankVerdict* verdict = VerdictOf(visit.verdicts, next.itemGuid);
    if (result != EQUIP_ERR_OK)
    {
        visit.refused.push_back(next.itemGuid);
        if (verdict)
            verdict->reason = BankReason::Refused;
        LOG_DEBUG("playerbots", "[Bank mail] {} bank visit: {} of item {} refused, error {}", bot->GetName(),
                  ChoiceWord(next.kind), next.entry, static_cast<uint32>(result));
        return;
    }

    visit.moved.push_back(next.itemGuid);
    if (withdraw)
        ++visit.withdrawn;
    else
        ++visit.deposited;
    BankVerdict const* planned = VerdictOf(plan.verdicts, next.itemGuid);
    if (!verdict)
    {
        if (planned)
            visit.verdicts.push_back(*planned);
        return;
    }
    verdict->choice = next.kind;
    if (planned)
        verdict->reason = planned->reason;
}

// One bank decision row per item of the visit's first plan, with what was really done with it
void WriteBankRows(Player* bot, BankVisitData const& visit)
{
    for (BankVerdict const& row : visit.verdicts)
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Bank, ChoiceWord(row.choice), ReasonWord(row.reason),
                           static_cast<float>(row.laterWorth),
                           {{"sell_now", static_cast<float>(row.sellNow)},
                            {"count", static_cast<float>(row.count)},
                            {"due_level", static_cast<float>(row.dueLevel)}},
                           row.entry);
    // Professions WR17: the bank-space steps, and the decision that left something out
    for (SpaceAnswer const& row : visit.space)
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Bank, SpaceChoiceWord(row.choice),
                           SpaceReasonWord(row.reason), static_cast<float>(row.worth),
                           {{"price", static_cast<float>(row.price)},
                            {"left_out", static_cast<float>(row.overflow)},
                            {"slots", static_cast<float>(row.slots)}},
                           row.bagEntry);
}

uint32 LimitSeconds() { return sPlayerbotAIConfig.BankMail.BankVisitDeadlineSeconds; }

ObjectGuid FindBanker(Player* bot, VisitState const& visit)
{
    // The indexed banker's template near its spawn
    Creature const* found = bot->FindNearestCreature(visit.entry, BANKER_SEARCH_YARDS);
    return found ? found->GetGUID() : ObjectGuid::Empty;
}

WorldObject const* ResolveBanker(Player* bot, ObjectGuid banker)
{
    Creature const* found = bot->GetMap()->GetCreature(banker);
    if (!found || !found->IsAlive() || found->GetReactionTo(bot) <= REP_UNFRIENDLY)
        return nullptr;
    return found;
}

// The bank handlers' own check (WorldSession::CanUseBank)
bool BankerInReach(Player* bot, WorldObject const& banker, float& /*range*/)
{
    return bot->GetNPCIfCanInteractWith(banker.GetGUID(), UNIT_NPC_FLAG_BANKER);
}

// Bags and bank reads and the move: the bot's own map thread inside its AI update (see Withdraw). Planned again before
// every move, so the bag slot a deposit frees serves a withdrawal that had no room
VisitTurn MoveNext(PlayerbotAI* botAI, Player* bot)
{
    BankVisitData& visit = botAI->bankMail.bankData;
    BankIn const in = BuildBankIn(botAI);
    BankPlan const plan = PlanBankVisit(in, visit.refused);
    if (!visit.planned)
    {
        visit.planned = true;
        visit.verdicts = Unmoved(plan.verdicts);
    }
    // Room first (professions WR17): the deposits then fill a bank bag bought on this visit
    if (!visit.spaceDone && SpaceStep(bot, visit, in, plan))
        return VisitTurn{VisitNext::Pending};

    auto next = std::find_if(plan.steps.begin(), plan.steps.end(),
                             [&visit](BankStep const& planned) { return !Contains(visit.moved, planned.itemGuid); });
    if (next == plan.steps.end())
        return VisitTurn{VisitNext::End, VisitEnd::Done};

    MoveItem(bot, visit, plan, *next);
    return VisitTurn{VisitNext::Pending};
}

bool WriteBankEnd(PlayerbotAI* botAI, Player* bot, VisitState const& state, VisitEnd end)
{
    BankVisitData& visit = botAI->bankMail.bankData;
    char const* how = VisitEndWord(VisitKind::Bank, end);
    // Bank read: the bot's own map thread inside its AI update (BankRulesInput.h)
    RefreshBankKnowledge(botAI);
    uint32 const freeAfter = botAI->bankMail.bank.freeSlots;
    if (WorldLog::Active())
        WriteBankRows(bot, visit);
    EventLog::Emit(bot, EventLog::EventType::BankVisit, how,
                   WorldLogFormat::PackVisitCounts(visit.deposited, visit.withdrawn), freeAfter);
    LOG_DEBUG("playerbots", "[Bank mail] {} bank visit {}: {} deposited, {} withdrawn, {} refused, {} free, {} s",
              bot->GetName(), how, visit.deposited, visit.withdrawn, visit.refused.size(), freeAfter,
              getMSTimeDiff(state.startedMs, getMSTime()) / IN_MILLISECONDS);

    // A withdrawn upgrade is put on; the item it replaces stays in the bags for the sell action
    bool const withdrew = visit.withdrawn > 0;
    visit = BankVisitData{};
    return withdrew;
}

constexpr VisitKindRow BANK_VISIT{.startWord = "bank",
                                  .searchYards = BANKER_SEARCH_YARDS,
                                  .missing = VisitEnd::NoSpot,
                                  .limitSeconds = &LimitSeconds,
                                  .find = &FindBanker,
                                  .resolve = &ResolveBanker,
                                  .reach = &BankerInReach,
                                  .approach = nullptr,
                                  .awaited = nullptr,
                                  .act = &MoveNext,
                                  .end = &WriteBankEnd};
}  // namespace

std::optional<BankerSpot> FindBankerSpot(Player* bot, char const*& refusal)
{
    refusal = nullptr;
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!bot || !indexes)
    {
        refusal = "the planner's indexes are not ready";
        return std::nullopt;
    }

    std::vector<Planner::Nearby> const nearest =
        indexes->Nearest(Planner::ServiceKind::Banker, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                         bot->GetPositionZ(), BANKER_CANDIDATES);
    if (nearest.empty())
    {
        refusal = "no banker on the map";
        return std::nullopt;
    }

    for (Planner::Nearby const& candidate : nearest)
    {
        Planner::Service const& service = indexes->services[candidate.index];
        if (!BankerServes(bot, service.factionTemplate))
            continue;
        return BankerSpot{service.mapId, service.x, service.y, service.z, service.entry, candidate.distance};
    }
    refusal = "no banker near the bot serves its faction";
    return std::nullopt;
}

bool BankerServes(Player const* bot, uint32 factionTemplate)
{
    // The banker's reaction must be above Unfriendly
    return bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(factionTemplate), bot) > REP_UNFRIENDLY;
}

std::optional<BankerSpot> BankerSpotNear(Player* bot, uint32 mapId, float x, float y, float z, float yards)
{
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!bot || !indexes)
        return std::nullopt;

    for (Planner::Nearby const& candidate :
         indexes->Nearest(Planner::ServiceKind::Banker, mapId, x, y, z, BANKER_CANDIDATES, yards))
    {
        Planner::Service const& service = indexes->services[candidate.index];
        if (BankerServes(bot, service.factionTemplate))
            return BankerSpot{service.mapId, service.x,     service.y,
                              service.z,     service.entry, bot->GetExactDist(service.x, service.y, service.z)};
    }
    return std::nullopt;
}

VisitKindRow const& BankVisitRow() { return BANK_VISIT; }

bool BeginBankVisit(PlayerbotAI* botAI, BankerSpot const& spot)
{
    if (!BeginVisit(botAI, VisitKind::Bank, &spot))
        return false;
    botAI->bankMail.bankData = BankVisitData{};
    return true;
}

std::string BankVisitCounters() { return VisitCounters(VisitKind::Bank); }
}  // namespace BankMail
