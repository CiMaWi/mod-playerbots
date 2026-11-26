/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionVisit.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "AuctionBuyList.h"
#include "AuctionConfig.h"
#include "AuctionHouseMgr.h"
#include "AuctionIndex.h"
#include "AuctionPricing.h"
#include "AuctionReference.h"
#include "AuctionRules.h"
#include "Bag.h"
#include "BankMailService.h"
#include "BankMailState.h"
#include "BankRules.h"
#include "BankRulesInput.h"
#include "BankVisit.h"
#include "BotKind.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Creature.h"
#include "EventLog.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "NewRpgErrandInternal.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SkillPathLive.h"
#include "Timer.h"
#include "Transport.h"
#include "UpkeepNeedRules.h"
#include "VisitLifecycle.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AuctionMemory
{
namespace
{
// One memory per seller that listed while online, by the guid's counter. Written by the auction hooks on the world
// thread, read by the bots' map threads (their stock reads and asks)
std::shared_mutex sellersLock;
std::unordered_map<uint32, SellerMemory> sellers;
std::unordered_map<uint32, Taken> takenByBot;  // by the guid's counter, under sellersLock too

std::int64_t GameNow() { return static_cast<std::int64_t>(GameTime::GetGameTime().count()); }

Player* OnlineSeller(AuctionEntry const* auction) { return ObjectAccessor::FindConnectedPlayer(auction->owner); }
}  // namespace

ItemMemory Of(uint32 sellerLow, uint32 entry)
{
    std::shared_lock<std::shared_mutex> const guard(sellersLock);
    auto const found = sellers.find(sellerLow);
    return found != sellers.end() ? found->second.Of(entry) : ItemMemory{entry};
}

Taken TakenOf(uint32 botLow)
{
    std::shared_lock<std::shared_mutex> const guard(sellersLock);
    auto const found = takenByBot.find(botLow);
    return found != takenByBot.end() ? found->second : Taken{};
}

// AuctionPost: src post, or relist after an expiry; amount = buyout. Only a bot's listings are remembered
void Listed(AuctionEntry const* auction)
{
    Player* seller = OnlineSeller(auction);
    if (!seller)
        return;
    uint32 steps = 0;
    if (GET_PLAYERBOT_AI(seller))
    {
        std::unique_lock<std::shared_mutex> const guard(sellersLock);
        SellerMemory& memory = sellers[auction->owner.GetCounter()];
        memory.Listed(auction->Id, GameNow(), static_cast<std::int64_t>(auction->expire_time));
        steps = memory.Of(auction->item_template).expiredSteps;
        Taken& taken = takenByBot[auction->owner.GetCounter()];
        ++taken.listings;
        taken.listed = auction->item_template;
    }
    EventLog::Emit(seller, EventLog::EventType::AuctionPost, steps ? "relist" : "post", auction->item_template,
                   auction->buyout);
}

// AuctionSale: src buyout or bid; amount = what the sale's mail brings (the price and the deposit, less the cut)
void Sold(AuctionEntry const* auction)
{
    {
        std::unique_lock<std::shared_mutex> const guard(sellersLock);
        auto const found = sellers.find(auction->owner.GetCounter());
        if (found != sellers.end())
            found->second.Sold(auction->Id, auction->item_template, GameNow());
    }
    if (Player* seller = OnlineSeller(auction))
        EventLog::Emit(seller, EventLog::EventType::AuctionSale,
                       auction->buyout && auction->bid >= auction->buyout ? "buyout" : "bid", auction->item_template,
                       static_cast<int64>(auction->bid) + auction->deposit - auction->GetAuctionCut());
}

// AuctionExpire: src expired; amount = the deposit lost
void Expired(AuctionEntry const* auction)
{
    {
        std::unique_lock<std::shared_mutex> const guard(sellersLock);
        auto const found = sellers.find(auction->owner.GetCounter());
        if (found != sellers.end())
            found->second.Expired(auction->Id, auction->item_template);
    }
    if (Player* seller = OnlineSeller(auction))
        EventLog::Emit(seller, EventLog::EventType::AuctionExpire, "expired", auction->item_template, auction->deposit);
}

// AuctionBuy: src buyout, amount = the price; AuctionWon: src bid, amount = the winning bid. The core sets the bid to
// the buyout before a buyout's mail
void Won(AuctionEntry const* auction, Player* buyer)
{
    bool const buyout = auction->buyout && auction->bid >= auction->buyout;
    // Buyouts alone: only the bot's own packet buys out, while a bid won at its auction's end may follow in one tick
    if (buyout && GET_PLAYERBOT_AI(buyer))
    {
        std::unique_lock<std::shared_mutex> const guard(sellersLock);
        takenByBot[buyer->GetGUID().GetCounter()].boughtOut = auction->Id;
    }
    EventLog::Emit(buyer, buyout ? EventLog::EventType::AuctionBuy : EventLog::EventType::AuctionWon,
                   buyout ? "buyout" : "bid", auction->item_template, auction->bid);
}

// AuctionOutbid: src outbid, amount = the new bid (the old one comes back by mail)
void Outbid(AuctionEntry const* auction, Player* bidder, uint32 newPrice)
{
    EventLog::Emit(bidder, EventLog::EventType::AuctionOutbid, "outbid", auction->item_template, newPrice);
}

void Held(uint32 sellerLow, uint32 entry)
{
    std::unique_lock<std::shared_mutex> const guard(sellersLock);
    sellers[sellerLow].Held(entry);
}

void Removed(AuctionEntry const* auction)
{
    std::unique_lock<std::shared_mutex> const guard(sellersLock);
    auto const found = sellers.find(auction->owner.GetCounter());
    if (found == sellers.end())
        return;
    found->second.Drop(auction->Id);
    if (found->second.Empty())
        sellers.erase(found);
}
}  // namespace AuctionMemory

namespace BankMail
{
namespace
{
// The bot looks for the auctioneer once it is this close to the spawn
constexpr float AUCTIONEER_SEARCH_YARDS = 20.0f;
// The planner's auctioneers near a stop looked at for one serving the bot
constexpr std::size_t AUCTIONEER_CANDIDATES = 4;
// The visit's limit (AH.9): first the walk from the stop's spot and the first step, then one step's time from each
// listing or purchase the core took (each is answered in the next world tick)
constexpr uint32 AUCTION_VISIT_SECONDS = 120;
constexpr uint32 MINUTES_PER_HOUR = 60;
// The sell packet: the auctioneer, the item count, one item guid and its count, bid, buyout, minutes
constexpr size_t SELL_PACKET_BYTES = 8 + 4 + 8 + 4 + 4 + 4 + 4;
// The bid packet: the auctioneer, the auction id, the price (a buyout's price is the buyout)
constexpr size_t BID_PACKET_BYTES = 8 + 4 + 4;
// The AuctionVisit row's id: the purchases (buyouts and bids the core took), capped, above the three counts
constexpr uint32 BUY_COUNT_SHIFT = 30;
constexpr uint32 BUY_COUNT_MAX = 3;

uint32 LimitSeconds() { return AUCTION_VISIT_SECONDS; }

uint32 GameTick() { return static_cast<uint32>(GameTime::GetGameTimeMS().count()); }

// An auctioneer serves the bot as a banker does: its reaction above Unfriendly (the core's own interaction check)
struct AuctioneerCheck
{
    Player* bot;
    bool operator()(Creature* creature) const
    {
        return creature->IsAlive() && creature->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER) &&
               BankerServes(bot, creature->GetFaction()) && bot->IsWithinDistInMap(creature, AUCTIONEER_SEARCH_YARDS);
    }
};

ObjectGuid FindAuctioneer(Player* bot, VisitState const& /*visit*/)
{
    std::vector<Creature*> found;
    AuctioneerCheck check{bot};
    Acore::CreatureListSearcher<AuctioneerCheck> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, AUCTIONEER_SEARCH_YARDS);
    Creature const* nearest = nullptr;
    for (Creature const* creature : found)
        if (!nearest || bot->GetExactDist(creature) < bot->GetExactDist(nearest))
            nearest = creature;
    return nearest ? nearest->GetGUID() : ObjectGuid::Empty;
}

WorldObject const* ResolveAuctioneer(Player* bot, ObjectGuid auctioneer)
{
    Creature const* found = bot->GetMap()->GetCreature(auctioneer);
    if (!found || !found->IsAlive() || found->GetReactionTo(bot) <= REP_UNFRIENDLY)
        return nullptr;
    return found;
}

// The sell handler's own check (GetNPCIfCanInteractWith): out of reach it ignores the packet
bool AuctioneerInReach(Player* bot, WorldObject const& auctioneer, float& /*range*/)
{
    return bot->GetNPCIfCanInteractWith(auctioneer.GetGUID(), UNIT_NPC_FLAG_AUCTIONEER);
}

// The first bag item (backpack, then the bags) the test accepts
template <typename Test>
Item* FirstBagItem(Player* bot, Test const& test)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); item && test(item))
            return item;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (Item* item = bag->GetItemByPos(static_cast<uint8>(slot)); item && test(item))
                    return item;
    return nullptr;
}

bool Tried(std::vector<uint32> const& tried, Item const* item)
{
    return std::find(tried.begin(), tried.end(), item->GetGUID().GetCounter()) != tried.end();
}

// The next bag stack of the bot's auction stock not looked at on this visit: a stack whose own carried keep is auction
// stock (AuctionStock, refreshed at the visit's first step; ListsStack). A stack another provider keeps is never
// listed, whatever its entry's other stacks are
Item* NextStock(PlayerbotAI* botAI, Player* bot, std::vector<uint32> const& tried)
{
    CarriedKeeps const& carried = botAI->bankMail.bank.carried;
    return FirstBagItem(bot, [&](Item const* item)
                        { return !Tried(tried, item) && ListsStack(carried, item->GetGUID().GetCounter()); });
}

// A held entry is held for every stack of it: its other stacks count as looked at (one row and one memory step per
// entry and visit)
void MarkEntryTried(Player* bot, uint32 entry, std::vector<uint32>& tried)
{
    auto const untried = [&](Item const* item) { return item->GetEntry() == entry && !Tried(tried, item); };
    while (Item* same = FirstBagItem(bot, untried))
        tried.push_back(same->GetGUID().GetCounter());
}

// The units of a bag stack the visit lists (StockUnits): those beyond the units the kept skill path uses of its entry,
// the whole stack of an entry it does not use (auction house WA7b). Read now, so a stack a craft used since the keeps'
// refresh never gives up the path's units
uint32 ListUnits(PlayerbotAI* botAI, Item const* item)
{
    KeepItemIn keep;
    keep.count = item->GetCount();
    SkillPath::Path const* const path = Planner::KeptPath(botAI);
    if (SkillPath::Material const* material = path ? SkillPath::MaterialOf(*path, item->GetEntry()) : nullptr)
        keep.materialUsed = material->used;
    return StockUnits(keep);
}

// The ask for the stack's listed units at this house (PLAN 3.4): its fees, the index's evidence (the cheapest competing
// buyout it saw, the item's and its kind's records), the seller's expiry memory
AuctionRules::AskResult AskFor(Player* bot, Item const* item, uint32 units, uint8 houseId)
{
    ItemTemplate const* proto = item->GetTemplate();
    AuctionRules::AskInput in;
    in.count = std::max<uint32>(units, 1);
    in.sellPrice = proto->SellPrice;
    in.vendorPayout = AuctionVendorPayout(item, in.count);
    if (std::shared_ptr<AuctionRules::Table const> const reference = AuctionReference::Current())
        if (AuctionRules::ItemReference const* row = reference->Find(proto->ItemId))
            in.reference = row->copper;
    if (std::shared_ptr<AuctionIndex::Snapshot const> const index = AuctionIndex::Current())
    {
        AuctionIndex::Evidence const evidence = index->EvidenceOf(
            proto->ItemId, AuctionIndex::KindOf(proto->Class, proto->Quality, proto->RequiredLevel, proto->ItemLevel));
        in.typical = evidence.typical;
        in.cheapestUnitBuyout = evidence.cheapestUnitBuyout;
        in.itemRecord = evidence.item;
        in.kindRecord = evidence.kind;
    }
    AuctionMemory::ItemMemory const memory = AuctionMemory::Of(bot->GetGUID().GetCounter(), proto->ItemId);
    in.expiredSteps = memory.expiredSteps;
    in.quickSale = memory.quickSale;
    in.fees = AuctionReference::HouseFees(houseId);
    return AuctionRules::Ask(in, sPlayerbotAIConfig.Auction);
}

// The `auction` decision row of a stack: value = its ask, the floor and the deposit beside it, and for a stack posted
// in part the units still to list (left); ref = the item entry
void WriteRow(Player* bot, AuctionWords::Choice choice, AuctionWords::Reason reason, uint64 ask, uint64 floor,
              uint32 deposit, uint32 entry, uint32 left = 0)
{
    if (!WorldLog::Active())
        return;
    WorldLogFormat::Alternatives amounts{{"floor", static_cast<float>(floor)},
                                         {"deposit", static_cast<float>(deposit)}};
    if (left)
        amounts.Add({"left", static_cast<float>(left)});
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Auction, AuctionWords::ChoiceWord(choice),
                       AuctionWords::ReasonWord(reason), static_cast<float>(ask), amounts, entry);
}

uint32 Clamped(uint64 copper)
{
    return static_cast<uint32>(std::min<uint64>(copper, std::numeric_limits<uint32>::max()));
}

// Test probes and seam (auction house AH3), bots under a test constraint only. The fixture verb `grow` arms a bot's
// next listing of an item: right after its sell packet is queued the stack grows, as a stack the bot loots into while
// the packet is out. The world thread arms, the bot's map thread reads: under the lock
struct GrowProbe
{
    uint32 entry{0};
    uint32 units{0};
};
std::mutex probeLock;
std::unordered_map<uint32, GrowProbe> probes;

uint32 TransportEntry(Player* bot) { return bot->GetTransport() ? bot->GetTransport()->GetEntry() : 0; }

void ProbeQueued(Player* bot, Item* item)
{
    std::lock_guard<std::mutex> const guard(probeLock);
    auto const found = probes.find(bot->GetGUID().GetCounter());
    if (found == probes.end() || found->second.entry != item->GetEntry())
        return;
    uint32 const grown = std::min(item->GetCount() + found->second.units, item->GetMaxStackCount());
    LOG_INFO("playerbots", "[Probe] {} auction grow item {} from {} to {} units while its sell packet is out",
             bot->GetName(), item->GetEntry(), item->GetCount(), grown);
    item->SetCount(grown);
    item->SetState(ITEM_CHANGED, bot);
    probes.erase(found);
}

// The phase the visit ended in, and the bot's distance to what it walked to: the auctioneer once found, else the
// spawn the visit started for; transport = the entry of the transport the bot rides, 0 none
void ProbeEnd(Player* bot, VisitState const& state, char const* how)
{
    Creature const* auctioneer = state.object.IsEmpty() ? nullptr : bot->GetMap()->GetCreature(state.object);
    char const* phase = "none";
    if (state.phase == VisitPhase::Walk)
        phase = state.object.IsEmpty() ? "walk_spot" : "reach";
    else if (state.phase == VisitPhase::Act)
        phase = "act";
    else if (state.phase == VisitPhase::Await)
        phase = "await";
    float const x = auctioneer ? auctioneer->GetPositionX() : state.x;
    float const y = auctioneer ? auctioneer->GetPositionY() : state.y;
    float const z = auctioneer ? auctioneer->GetPositionZ() : state.z;
    bool const inReach = auctioneer && bot->GetNPCIfCanInteractWith(state.object, UNIT_NPC_FLAG_AUCTIONEER);
    LOG_INFO("playerbots",
             "[Probe] {} auction visit end {} phase {} auctioneer {} yd {:.1f} dz {:.1f} in_reach {} transport {} at "
             "({:.1f}, {:.1f}, {:.1f}) after {} s",
             bot->GetName(), how, phase, auctioneer ? auctioneer->GetEntry() : state.entry, bot->GetExactDist(x, y, z),
             bot->GetPositionZ() - z, inReach ? 1 : 0, TransportEntry(bot), bot->GetPositionX(), bot->GetPositionY(),
             bot->GetPositionZ(), getMSTimeDiff(state.startedMs, getMSTime()) / IN_MILLISECONDS);
}

// The `auction` decision row of a buy-list line (WA4b): value = the price (a skip: its nearest offer's, 0 with none);
// the ceiling for the units the lot covers, the line cost and the gold the market could spend beside it; ref = the item
void WriteBuyRow(Player* bot, AuctionWords::Choice choice, AuctionWords::Reason reason, uint64 price, uint64 ceiling,
                 uint64 cost, uint64 spendable, uint32 entry)
{
    if (WorldLog::Active())
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Auction, AuctionWords::ChoiceWord(choice),
                           AuctionWords::ReasonWord(reason), static_cast<float>(price),
                           {{"ceiling", static_cast<float>(ceiling)},
                            {"cost", static_cast<float>(cost)},
                            {"spendable", static_cast<float>(spendable)}},
                           entry);
}

// A listing or a purchase the core took renews the visit's limit
void RenewLimit(PlayerbotAI* botAI)
{
    VisitState& visit = botAI->bankMail.visit;
    visit.limitMs = RenewedLimitMs(getMSTimeDiff(visit.startedMs, getMSTime()), LimitSeconds() * IN_MILLISECONDS);
}

// The core answers a refused bid with no packet and no hook (the handler returns silently): a buyout is taken when the
// won mail's hook saw it, a bid when exactly its price left the purse (a bot never bids over its own bid). A bid's row
// is the visit's; a buyout's AuctionBuy comes from that hook
VisitTurn AwaitPurchase(PlayerbotAI* botAI, Player* bot)
{
    AuctionVisitData& visit = botAI->bankMail.auctionData;
    uint32 const paid = visit.moneyBefore > bot->GetMoney() ? visit.moneyBefore - bot->GetMoney() : 0;
    bool const buyout = visit.awaitKind == static_cast<uint8>(AuctionRules::BuyKind::Buyout);
    bool const taken = buyout ? AuctionMemory::TakenOf(bot->GetGUID().GetCounter()).boughtOut == visit.awaitAuction
                              : paid == visit.awaitPrice;
    if (taken)
    {
        RenewLimit(botAI);
        visit.spent += paid;
        visit.bagTaken = visit.bagTaken || visit.awaitBag;
        if (buyout)
            ++visit.bought;
        else
        {
            ++visit.bids;
            ++visit.openBids;
            EventLog::Emit(bot, EventLog::EventType::AuctionBid, "bid", visit.awaitingEntry, visit.awaitPrice);
        }
        WriteBuyRow(bot, buyout ? AuctionWords::Choice::Buy : AuctionWords::Choice::Bid,
                    buyout ? AuctionWords::Reason::UnderCeiling : static_cast<AuctionWords::Reason>(visit.awaitReason),
                    visit.awaitPrice, visit.awaitCeiling, visit.awaitCost, visit.awaitSpendable, visit.awaitingEntry);
    }
    else
    {
        ++visit.refused;
        LOG_DEBUG("playerbots", "[Auction] {} {} of item {} at {} copper on auction {} not taken by the core",
                  bot->GetName(), buyout ? "buyout" : "bid", visit.awaitingEntry, visit.awaitPrice, visit.awaitAuction);
    }
    visit.awaitAuction = 0;
    visit.awaitingEntry = 0;
    return VisitTurn{VisitNext::Continue};
}

// The core took the stack when its add hook saw a listing of the item since the packet went out (the AuctionPost row
// comes from that hook); a stack listed in part is a post, with the units still to list beside it. The handler runs in
// the world thread's drain after this tick's map update: the answer shows from the next world tick on
VisitTurn AwaitListing(PlayerbotAI* botAI, Player* bot)
{
    AuctionVisitData& visit = botAI->bankMail.auctionData;
    if (GameTick() == visit.stepTick || bot->IsBeingTeleported())
        return VisitTurn{VisitNext::Pending};
    if (visit.awaitAuction)
        return AwaitPurchase(botAI, bot);
    AuctionMemory::Taken const taken = AuctionMemory::TakenOf(bot->GetGUID().GetCounter());
    if (taken.listings != visit.listingsBefore && taken.listed == visit.awaitingEntry)
    {
        RenewLimit(botAI);
        ++visit.posted;
        uint32 const deposit = visit.moneyBefore > bot->GetMoney() ? visit.moneyBefore - bot->GetMoney() : 0;
        visit.deposits += deposit;
        Item const* const left = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(visit.awaiting));
        WriteRow(bot, visit.relist ? AuctionWords::Choice::Relist : AuctionWords::Choice::Post,
                 visit.relist ? AuctionWords::Reason::Expired : AuctionWords::Reason::AboveVendor, visit.ask,
                 visit.floor, deposit, visit.awaitingEntry, left ? ListUnits(botAI, left) : 0);
    }
    else
    {
        // A refusal steps the expiry memory as a hold does, so a stack the core always refuses stops being stock
        // (RelistLimit) instead of drawing a trip at every plan
        ++visit.refused;
        MarkEntryTried(bot, visit.awaitingEntry, visit.tried);
        AuctionMemory::Held(bot->GetGUID().GetCounter(), visit.awaitingEntry);
        LOG_DEBUG("playerbots", "[Auction] {} listing of item {} at {} copper not taken by the core", bot->GetName(),
                  visit.awaitingEntry, visit.ask);
    }
    visit.awaiting = 0;
    return VisitTurn{VisitNext::Continue};
}

// The copper the market leaves in the purse, from the trip's needs list (ErrandNeeds, cached per trip state); without
// a needs list nothing is spendable
uint64 MarketKeptNow(PlayerbotAI* botAI)
{
    std::shared_ptr<Planner::UpkeepIndex const> const upkeep =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    std::shared_ptr<UpkeepNeeds::VisitNeeds const> const needs =
        upkeep ? NewRpgInternal::ErrandNeeds(botAI, upkeep) : nullptr;
    return needs ? UpkeepNeeds::MarketKept(needs->needs) : std::numeric_limits<uint64>::max();
}

// The buy step after the stock (WA4b, PLAN 3.6): each line of the buy list once per visit, at the house's current
// offers (WA.4: read only here, at the auctioneer): ChooseLine's buyout or bid goes out as the core's bid packet and
// waits for its answer, a skip writes its row and the next line is decided. One auction per line and one bag per
// visit; the purse is read at every line, so the gold spent counts. The bot's own auctions are not left out by id
// (the index has no owner): the core refuses a bid on them, which counts as refused
VisitTurn BuyNext(PlayerbotAI* botAI, Player* bot, Creature const* auctioneer, uint8 houseId)
{
    AuctionVisitData& visit = botAI->bankMail.auctionData;
    std::shared_ptr<AuctionIndex::Snapshot const> const index = AuctionIndex::Current();
    if (!index)
        return VisitTurn{VisitNext::End, VisitEnd::Done};
    uint64 const ownGuid = bot->GetGUID().GetRawValue();
    if (!visit.buying)
    {
        visit.buying = true;
        visit.openBids = AuctionRules::OpenBids(*index, ownGuid);
        visit.kept = MarketKeptNow(botAI);
    }

    AuctionIndex::CurrentOffers const none;
    for (AuctionRules::BuyLine const& line : Planner::BuyListOf(botAI))
    {
        if ((visit.only && line.item != visit.only) ||
            std::find(visit.decided.begin(), visit.decided.end(), line.item) != visit.decided.end())
            continue;
        visit.decided.push_back(line.item);
        bool const bag = line.owner == AuctionRules::LineOwner::Bag;
        if (bag && visit.bagTaken)
            continue;

        uint64 const spendable = AuctionRules::Spendable(bot->GetMoney(), visit.kept, 0);
        AuctionRules::BuyInput buyer;
        buyer.spendable = spendable;
        buyer.openBids = visit.openBids;
        buyer.openBidsMax = sPlayerbotAIConfig.Auction.OpenBidsMax;
        buyer.now = static_cast<std::int64_t>(GameTime::GetGameTime().count());
        buyer.ownGuid = ownGuid;
        AuctionIndex::ItemEntry const* entry = index->Find(line.item);
        AuctionIndex::CurrentOffers const& offers = entry ? entry->offers : none;
        AuctionRules::BuyChoice const choice = AuctionRules::ChooseLine(offers, line, houseId, {}, buyer);

        // The chosen offer, or a skip's nearest, for the row's amounts
        AuctionRules::OfferInput offer;
        bool found = false;
        for (AuctionIndex::OfferLine const& candidate : offers.cheapest)
            if (choice.auctionId && candidate.id == choice.auctionId)
            {
                offer = {candidate, line.units, line.excessUnitWorth};
                found = true;
                break;
            }
        uint64 const price = choice.kind != AuctionRules::BuyKind::Skip ? choice.price
                             : found                                    ? AuctionRules::RankPrice(offer.line)
                                                                        : 0;
        uint64 const ceiling = AuctionRules::CeilingOf(line) * (found ? AuctionRules::Covered(offer) : line.units);
        uint64 const cost = found ? AuctionRules::LineCost(offer, price) : 0;
        if (choice.kind == AuctionRules::BuyKind::Skip)
        {
            WriteBuyRow(bot, AuctionWords::Choice::Skip, choice.reason, price, ceiling, cost, spendable, line.item);
            continue;
        }

        // QueuePacket takes ownership; the world thread runs the handler when it drains the bot's session
        WorldPacket* packet = new WorldPacket(CMSG_AUCTION_PLACE_BID, BID_PACKET_BYTES);
        *packet << auctioneer->GetGUID() << choice.auctionId << Clamped(choice.price);
        bot->GetSession()->QueuePacket(packet);
        visit.awaitAuction = choice.auctionId;
        visit.awaitingEntry = line.item;
        visit.awaitPrice = Clamped(choice.price);
        visit.awaitCeiling = Clamped(ceiling);
        visit.awaitCost = Clamped(cost);
        visit.awaitSpendable = Clamped(spendable);
        visit.awaitKind = static_cast<uint8>(choice.kind);
        visit.awaitReason = static_cast<uint8>(choice.reason);
        visit.awaitBag = bag;
        visit.moneyBefore = bot->GetMoney();
        visit.stepTick = GameTick();
        return VisitTurn{VisitNext::Await};
    }
    return VisitTurn{VisitNext::End, VisitEnd::Done};
}

// Bag reads: the bot's own map thread inside its AI update. Each stock stack is asked for once: a hold writes its
// row and the next stack is asked; a listing queues the core's sell packet and waits for its answer
VisitTurn ListNext(PlayerbotAI* botAI, Player* bot)
{
    AuctionVisitData& visit = botAI->bankMail.auctionData;
    Creature const* auctioneer = bot->GetMap()->GetCreature(botAI->bankMail.visit.object);
    AuctionHouseEntry const* house =
        auctioneer ? AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction()) : nullptr;
    if (!house)
        return VisitTurn{VisitNext::End, VisitEnd::NoSpot};
    // The bags may have changed since the trip was planned (or no trip planned it: the GM command); once per visit
    if (visit.tried.empty() && !visit.buying)
        RefreshCarriedKeeps(botAI);

    while (Item* item = NextStock(botAI, bot, visit.tried))
    {
        visit.tried.push_back(item->GetGUID().GetCounter());
        uint32 const units = ListUnits(botAI, item);
        AuctionRules::AskResult const ask = AskFor(bot, item, units, static_cast<uint8>(house->houseId));
        bool const lists = units && ask.kind == AuctionRules::AskResult::Kind::List;
        uint32 const minutes = ask.hours * MINUTES_PER_HOUR;
        uint32 const deposit = lists ? AuctionHouseMgr::GetAuctionDeposit(house, minutes * MINUTE, item, units) : 0;
        if (!lists || !bot->HasEnoughMoney(deposit))
        {
            ++visit.held;
            MarkEntryTried(bot, item->GetEntry(), visit.tried);
            AuctionMemory::Held(bot->GetGUID().GetCounter(), item->GetEntry());
            WriteRow(bot, AuctionWords::Choice::Hold, lists ? AuctionWords::Reason::Budget : ask.reason, ask.ask,
                     ask.floor, deposit, item->GetEntry());
            continue;
        }

        // QueuePacket takes ownership; the world thread runs the handler when it drains the bot's session
        WorldPacket* packet = new WorldPacket(CMSG_AUCTION_SELL_ITEM, SELL_PACKET_BYTES);
        // Clamped as the buy side: an ask above the core's money cap is refused, never sold at a truncated price. The
        // count lists those units: the core lists a clone of them and keeps the rest of the stack (WA7b)
        *packet << auctioneer->GetGUID() << uint32(1) << item->GetGUID() << units << Clamped(ask.startBid)
                << Clamped(ask.ask) << minutes;
        visit.listingsBefore = AuctionMemory::TakenOf(bot->GetGUID().GetCounter()).listings;
        bot->GetSession()->QueuePacket(packet);
        if (botAI->testConstraints)
            ProbeQueued(bot, item);
        visit.awaiting = item->GetGUID().GetCounter();
        visit.awaitingEntry = item->GetEntry();
        visit.ask = Clamped(ask.ask);
        visit.floor = Clamped(ask.floor);
        visit.relist = ask.reason == AuctionWords::Reason::Expired;
        visit.moneyBefore = bot->GetMoney();
        visit.stepTick = GameTick();
        return VisitTurn{VisitNext::Await};
    }
    return BuyNext(botAI, bot, auctioneer, static_cast<uint8>(house->houseId));
}

// AuctionVisit: src the end word; id = posted | held << 10 | refused << 20 | min(purchases, 3) << 30 (buyouts and
// bids the core took, WA4b); amount = the deposits paid
bool WriteAuctionEnd(PlayerbotAI* botAI, Player* bot, VisitState const& state, VisitEnd end)
{
    AuctionVisitData& visit = botAI->bankMail.auctionData;
    char const* how = VisitEndWord(VisitKind::Auction, end);
    uint32 const purchases = std::min(visit.bought + visit.bids, BUY_COUNT_MAX);
    EventLog::Emit(
        bot, EventLog::EventType::AuctionVisit, how,
        WorldLogFormat::PackVisitCounts(visit.posted, visit.held, visit.refused) | purchases << BUY_COUNT_SHIFT,
        visit.deposits);
    LOG_DEBUG("playerbots",
              "[Auction] {} auctioneer visit {}: {} listed, {} held, {} refused, {} copper deposits, {} bought, {} "
              "bids, {} copper spent, {} s",
              bot->GetName(), how, visit.posted, visit.held, visit.refused, visit.deposits, visit.bought, visit.bids,
              visit.spent, getMSTimeDiff(state.startedMs, getMSTime()) / IN_MILLISECONDS);
    if (botAI->testConstraints)
        ProbeEnd(bot, state, how);
    visit = AuctionVisitData{};
    return false;
}

constexpr VisitKindRow AUCTION_VISIT{.startWord = "auction",
                                     .searchYards = AUCTIONEER_SEARCH_YARDS,
                                     .missing = VisitEnd::NoSpot,
                                     .limitSeconds = &LimitSeconds,
                                     .find = &FindAuctioneer,
                                     .resolve = &ResolveAuctioneer,
                                     .reach = &AuctioneerInReach,
                                     .approach = nullptr,
                                     .awaited = &AwaitListing,
                                     .act = &ListNext,
                                     .end = &WriteAuctionEnd};
}  // namespace

std::optional<AuctioneerSpot> AuctioneerSpotNear(Player* bot, uint32 mapId, float x, float y, float z, float yards)
{
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!bot || !indexes)
        return std::nullopt;

    for (Planner::Nearby const& candidate :
         indexes->Nearest(Planner::ServiceKind::Auctioneer, mapId, x, y, z, AUCTIONEER_CANDIDATES, yards))
    {
        Planner::Service const& service = indexes->services[candidate.index];
        if (BankerServes(bot, service.factionTemplate))
            return AuctioneerSpot{service.mapId, service.x,     service.y,
                                  service.z,     service.entry, bot->GetExactDist(service.x, service.y, service.z)};
    }
    return std::nullopt;
}

VisitKindRow const& AuctionVisitRow() { return AUCTION_VISIT; }

bool BeginAuctionVisit(PlayerbotAI* botAI, AuctioneerSpot const& spot)
{
    if (!BeginVisit(botAI, VisitKind::Auction, &spot))
        return false;
    botAI->bankMail.auctionData = AuctionVisitData{};
    return true;
}

void ArmAuctionGrow(Player* bot, uint32 entry, uint32 units)
{
    std::lock_guard<std::mutex> const guard(probeLock);
    probes[bot->GetGUID().GetCounter()] = GrowProbe{entry, units};
}

bool HandleAuctionVisitCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    uint32 only = 0;
    bool anyDistance = false;
    for (std::string word; in >> word;)
    {
        if (word == "far")
            anyDistance = true;
        else
            only = static_cast<uint32>(std::strtoul(word.c_str(), nullptr, 10));
    }
    if (name.empty())
    {
        handler->SendErrorMessage("Usage: playerbots auction visit <bot name> [<item>] [far]");
        return false;
    }
    std::string const asked = name;
    Player* bot = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name) : nullptr;
    PlayerbotAI* botAI = bot && bot->IsInWorld() ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
    {
        handler->SendErrorMessage("AUCTION visit: no bot {} online", asked);
        return false;
    }

    // As the mail and bank visit commands: persistent bots only, one visit at a time
    char const* refusal = nullptr;
    if (!BotKind::KeepsPossessions(bot))
        refusal = "the bot does not keep its possessions (not a persistent bot)";
    else if (!bot->IsAlive())
        refusal = "the bot is dead";
    else if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
        refusal = "the bot has no new rpg strategy";
    else
        refusal = VisitStartRefusal(botAI->bankMail, VisitKind::Auction);
    // World thread, between the map updates. A test command (WB.12): the bot stands near an auctioneer; the search
    // looks twice the reach out, so a near miss is named too_far
    std::optional<AuctioneerSpot> const spot =
        refusal ? std::nullopt
                : AuctioneerSpotNear(bot, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                     bot->GetPositionZ(), 2.0f * VISIT_COMMAND_REACH_YARDS);
    if (!refusal && !spot)
        refusal = "no auctioneer serving the bot near it";
    if (spot && !anyDistance && !WithinVisitReach(spot->distance))
    {
        handler->SendErrorMessage(
            "AUCTION visit {}: refused, {} (the nearest auctioneer is {:.0f} yd away, reach {:.0f} yd)", bot->GetName(),
            VISIT_TOO_FAR, spot->distance, VISIT_COMMAND_REACH_YARDS);
        return false;
    }
    if (!spot || !BeginAuctionVisit(botAI, *spot))
    {
        handler->SendErrorMessage("AUCTION visit {}: refused, {}", bot->GetName(), refusal ? refusal : "no visit");
        return false;
    }
    botAI->bankMail.auctionData.only = only;

    handler->PSendSysMessage("AUCTION visit {}: started at ({:.0f}, {:.0f}) on map {}, {:.0f} yd away", bot->GetName(),
                             spot->x, spot->y, spot->mapId, spot->distance);
    return true;
}
}  // namespace BankMail
