/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankVisit.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "BankMailService.h"
#include "BankMailState.h"
#include "BankRules.h"
#include "BankRulesInput.h"
#include "CensusMgr.h"
#include "Creature.h"
#include "DBCStores.h"
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
    BankPlan const plan = PlanBankVisit(BuildBankIn(botAI), visit.refused);
    if (!visit.planned)
    {
        visit.planned = true;
        visit.verdicts = Unmoved(plan.verdicts);
    }

    auto next = std::find_if(plan.steps.begin(), plan.steps.end(),
                             [&visit](BankStep const& planned) { return !Contains(visit.moved, planned.itemGuid); });
    if (next == plan.steps.end())
        return VisitTurn{VisitNext::End, VisitEnd::Done};

    MoveItem(bot, visit, plan, *next);
    RefreshBankKnowledge(botAI);
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
    EventLog::Emit(bot, WorldLogFormat::EVENT_BANK_VISIT, how,
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
        // The core's interact test (Player::GetNPCIfCanInteractWith): the banker's reaction must be above Unfriendly
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(service.factionTemplate);
        if (bot->GetFactionReactionTo(faction, bot) <= REP_UNFRIENDLY)
            continue;
        return BankerSpot{service.mapId, service.x, service.y, service.z, service.entry, candidate.distance};
    }
    refusal = "no banker near the bot serves its faction";
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

BankVisitStep StepBankVisit(PlayerbotAI* botAI)
{
    BankVisitStep step;
    if (!botAI || !VisitRunsOf(botAI->bankMail.visit, VisitKind::Bank))
        return step;

    VisitStep const driven = StepVisit(botAI);
    step.result = driven.result;
    step.mapId = driven.mapId;
    step.x = driven.x;
    step.y = driven.y;
    step.z = driven.z;
    step.banker = driven.object;
    return step;
}

std::string BankVisitCounters() { return VisitCounters(VisitKind::Bank); }
}  // namespace BankMail
