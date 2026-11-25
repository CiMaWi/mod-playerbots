/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionVisit.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Timer.h"
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

std::int64_t GameNow() { return static_cast<std::int64_t>(GameTime::GetGameTime().count()); }

Player* OnlineSeller(AuctionEntry const* auction) { return ObjectAccessor::FindConnectedPlayer(auction->owner); }
}  // namespace

ItemMemory Of(uint32 sellerLow, uint32 entry)
{
    std::shared_lock<std::shared_mutex> const guard(sellersLock);
    auto const found = sellers.find(sellerLow);
    return found != sellers.end() ? found->second.Of(entry) : ItemMemory{entry};
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
// The visit's deadline: the walk from the stop's spot and a few listings, each answered in the next world tick
constexpr uint32 AUCTION_VISIT_SECONDS = 120;
constexpr uint32 MINUTES_PER_HOUR = 60;
// The sell packet: the auctioneer, the item count, one item guid and its count, bid, buyout, minutes
constexpr size_t SELL_PACKET_BYTES = 8 + 4 + 8 + 4 + 4 + 4 + 4;

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

// The next bag stack of the bot's auction stock not looked at on this visit: an item of an entry the carried keeps
// hold as auction stock (AuctionStock, refreshed at the visit's first step)
Item* NextStock(PlayerbotAI* botAI, Player* bot, std::vector<uint32> const& tried)
{
    std::vector<KeptEntry> const& kept = botAI->bankMail.bank.carried.items;
    auto const stock = [&kept](Item const* item)
    {
        return std::any_of(
            kept.begin(), kept.end(), [item](KeptEntry const& keep)
            { return keep.entry == item->GetEntry() && std::string_view(keep.provider) == AUCTION_STOCK_PROVIDER; });
    };
    return FirstBagItem(bot, [&](Item const* item) { return !Tried(tried, item) && stock(item); });
}

// A held entry is held for every stack of it: its other stacks count as looked at (one row and one memory step per
// entry and visit)
void MarkEntryTried(Player* bot, uint32 entry, std::vector<uint32>& tried)
{
    auto const untried = [&](Item const* item) { return item->GetEntry() == entry && !Tried(tried, item); };
    while (Item* same = FirstBagItem(bot, untried))
        tried.push_back(same->GetGUID().GetCounter());
}

// The ask for the whole stack at this house (PLAN 3.4): its fees, the cheapest competing buyout the index saw, the
// seller's expiry memory
AuctionRules::AskResult AskFor(Player* bot, Item const* item, uint8 houseId)
{
    ItemTemplate const* proto = item->GetTemplate();
    AuctionRules::AskInput in;
    in.count = std::max<uint32>(item->GetCount(), 1);
    in.sellPrice = proto->SellPrice;
    in.vendorPayout = AuctionVendorPayout(item, in.count);
    if (std::shared_ptr<AuctionRules::Table const> const reference = AuctionReference::Current())
        if (AuctionRules::ItemReference const* row = reference->Find(proto->ItemId))
            in.reference = row->copper;
    if (std::shared_ptr<AuctionIndex::Snapshot const> const index = AuctionIndex::Current())
        if (AuctionIndex::ItemEntry const* entry = index->Find(proto->ItemId))
        {
            in.typical = entry->typical;
            in.cheapestUnitBuyout = entry->offers.cheapestUnitBuyout;
        }
    AuctionMemory::ItemMemory const memory = AuctionMemory::Of(bot->GetGUID().GetCounter(), proto->ItemId);
    in.expiredSteps = memory.expiredSteps;
    in.quickSale = memory.quickSale;
    in.fees = AuctionReference::HouseFees(houseId);
    return AuctionRules::Ask(in, sPlayerbotAIConfig.Auction);
}

// The `auction` decision row of a stack: value = its ask, the floor and the deposit beside it; ref = the item entry
void WriteRow(Player* bot, AuctionWords::Choice choice, AuctionWords::Reason reason, uint64 ask, uint64 floor,
              uint32 deposit, uint32 entry)
{
    if (WorldLog::Active())
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Auction, AuctionWords::ChoiceWord(choice),
                           AuctionWords::ReasonWord(reason), static_cast<float>(ask),
                           {{"floor", static_cast<float>(floor)}, {"deposit", static_cast<float>(deposit)}}, entry);
}

// The core took the stack when the bags no longer hold it (the AuctionPost row comes from its add hook). The handler
// runs in the world thread's drain after this tick's map update: the answer shows from the next world tick on
VisitTurn AwaitListing(PlayerbotAI* botAI, Player* bot)
{
    AuctionVisitData& visit = botAI->bankMail.auctionData;
    if (GameTick() == visit.stepTick || bot->IsBeingTeleported())
        return VisitTurn{VisitNext::Pending};
    if (!bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(visit.awaiting)))
    {
        ++visit.posted;
        uint32 const deposit = visit.moneyBefore > bot->GetMoney() ? visit.moneyBefore - bot->GetMoney() : 0;
        visit.deposits += deposit;
        WriteRow(bot, visit.relist ? AuctionWords::Choice::Relist : AuctionWords::Choice::Post,
                 visit.relist ? AuctionWords::Reason::Expired : AuctionWords::Reason::AboveVendor, visit.ask,
                 visit.floor, deposit, visit.awaitingEntry);
    }
    else
    {
        ++visit.refused;
        LOG_DEBUG("playerbots", "[Auction] {} listing of item {} at {} copper not taken by the core", bot->GetName(),
                  visit.awaitingEntry, visit.ask);
    }
    visit.awaiting = 0;
    return VisitTurn{VisitNext::Continue};
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
    // The bags may have changed since the trip was planned (or no trip planned it: the GM command)
    if (visit.tried.empty())
        RefreshCarriedKeeps(botAI);

    while (Item* item = NextStock(botAI, bot, visit.tried))
    {
        visit.tried.push_back(item->GetGUID().GetCounter());
        AuctionRules::AskResult const ask = AskFor(bot, item, static_cast<uint8>(house->houseId));
        bool const lists = ask.kind == AuctionRules::AskResult::Kind::List;
        uint32 const minutes = ask.hours * MINUTES_PER_HOUR;
        uint32 const deposit =
            lists ? AuctionHouseMgr::GetAuctionDeposit(house, minutes * MINUTE, item, item->GetCount()) : 0;
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
        *packet << auctioneer->GetGUID() << uint32(1) << item->GetGUID() << item->GetCount()
                << static_cast<uint32>(ask.startBid) << static_cast<uint32>(ask.ask) << minutes;
        bot->GetSession()->QueuePacket(packet);
        visit.awaiting = item->GetGUID().GetCounter();
        visit.awaitingEntry = item->GetEntry();
        visit.ask = static_cast<uint32>(ask.ask);
        visit.floor = static_cast<uint32>(std::min<uint64>(ask.floor, std::numeric_limits<uint32>::max()));
        visit.relist = ask.reason == AuctionWords::Reason::Expired;
        visit.moneyBefore = bot->GetMoney();
        visit.stepTick = GameTick();
        return VisitTurn{VisitNext::Await};
    }
    return VisitTurn{VisitNext::End, VisitEnd::Done};
}

// AuctionVisit: src the end word; id = posted | held << 10 | refused << 20; amount = the deposits paid
bool WriteAuctionEnd(PlayerbotAI* botAI, Player* bot, VisitState const& state, VisitEnd end)
{
    AuctionVisitData& visit = botAI->bankMail.auctionData;
    char const* how = VisitEndWord(VisitKind::Auction, end);
    EventLog::Emit(bot, EventLog::EventType::AuctionVisit, how,
                   WorldLogFormat::PackVisitCounts(visit.posted, visit.held, visit.refused), visit.deposits);
    LOG_DEBUG("playerbots",
              "[Auction] {} auctioneer visit {}: {} listed, {} held, {} refused, {} copper deposits, {} s",
              bot->GetName(), how, visit.posted, visit.held, visit.refused, visit.deposits,
              getMSTimeDiff(state.startedMs, getMSTime()) / IN_MILLISECONDS);
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

bool HandleAuctionVisitCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string name;
    in >> name;
    if (name.empty())
    {
        handler->SendErrorMessage("Usage: playerbots auction visit <bot name>");
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
    if (spot && !WithinVisitReach(spot->distance))
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

    handler->PSendSysMessage("AUCTION visit {}: started at ({:.0f}, {:.0f}) on map {}, {:.0f} yd away", bot->GetName(),
                             spot->x, spot->y, spot->mapId, spot->distance);
    return true;
}
}  // namespace BankMail
