/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionFixture.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "AuctionHouseMgr.h"
#include "AuctionHouseSearcher.h"
#include "AuctionIndex.h"
#include "AuctionPricing.h"
#include "AuctionVisit.h"
#include "Bag.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "ThreadCheck.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AuctionFixture
{
namespace
{
// The bot stands at its auctioneer; the verb looks this far for one (the core's own interaction range is checked too)
constexpr float AUCTIONEER_SEARCH_YARDS = 20.0f;
// The stand-in players: guids no character has, so their mails are lost (the auctions themselves are ordinary). The
// seller and the bidder are two, so the stand-in can outbid a bot on the stand-in's own listing (WA1c run 1)
constexpr uint32 STANDIN_GUID_LOW = 2000000001;
constexpr uint32 STANDIN_BIDDER_GUID_LOW = 2000000002;
constexpr uint32 MINUTES_PER_HOUR = 60;

ObjectGuid StandIn() { return ObjectGuid::Create<HighGuid::Player>(STANDIN_GUID_LOW); }
ObjectGuid StandInBidder() { return ObjectGuid::Create<HighGuid::Player>(STANDIN_BIDDER_GUID_LOW); }

struct Scene
{
    std::string verb;
    std::string botName;
    Player* bot = nullptr;
    uint32 item = 0;
    Creature* auctioneer = nullptr;
    AuctionHouseObject* house = nullptr;
};

bool Fail(ChatHandler* handler, Scene const& scene, std::string const& why)
{
    handler->SendErrorMessage("AUCTION fixture {} {} {}: error, {}", scene.verb, scene.botName, scene.item, why);
    return false;
}

bool Ok(ChatHandler* handler, Scene const& scene, std::string const& what)
{
    handler->PSendSysMessage("AUCTION fixture {} {} {}: ok, {}", scene.verb, scene.botName, scene.item, what);
    return true;
}

struct AuctioneerCheck
{
    Player* bot;
    bool operator()(Creature* creature) const
    {
        return creature->IsAlive() && creature->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER) &&
               bot->IsWithinDistInMap(creature, AUCTIONEER_SEARCH_YARDS);
    }
};

Creature* NearestAuctioneer(Player* bot)
{
    std::vector<Creature*> found;
    AuctioneerCheck check{bot};
    Acore::CreatureListSearcher<AuctioneerCheck> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, AUCTIONEER_SEARCH_YARDS);
    Creature* nearest = nullptr;
    for (Creature* creature : found)
        if (!nearest || bot->GetExactDist(creature) < bot->GetExactDist(nearest))
            nearest = creature;
    return nearest;
}

// The newest listing of the item in the house (the scene's own item: one bot per item)
AuctionEntry* Newest(AuctionHouseObject* house, uint32 item)
{
    AuctionEntry* newest = nullptr;
    for (auto const& [id, entry] : house->GetAuctions())
        if (entry && entry->item_template == item && (!newest || entry->Id > newest->Id))
            newest = entry;
    return newest;
}

// A bag item of the entry with at least count units, one with exactly count first (the core then moves it whole)
Item* BagItem(Player* bot, uint32 entry, uint32 count)
{
    Item* best = nullptr;
    auto consider = [&](Item* item)
    {
        if (!item || item->GetEntry() != entry || item->GetCount() < count)
            return;
        if (!best || (item->GetCount() == count && best->GetCount() != count))
            best = item;
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                consider(bag->GetItemByPos(static_cast<uint8>(slot)));
    return best;
}

void Queue(Player* bot, WorldPacket* packet)
{
    // QueuePacket takes ownership; the world thread runs the handler when it drains the bot's session
    bot->GetSession()->QueuePacket(packet);
}

// What the core's bid handler refuses without an answer (AuctionHouseHandler.cpp HandleAuctionPlaceBid), said here
std::string BidRefusal(AuctionEntry const* auction, ObjectGuid const& bidder, uint32 price)
{
    if (auction->owner == bidder)
        return "the bidder owns the auction";
    if (price <= auction->bid || price < auction->startbid)
        return "price " + std::to_string(price) + " not above the bid " + std::to_string(auction->bid) +
               " or below the start " + std::to_string(auction->startbid);
    if ((price < auction->buyout || !auction->buyout) &&
        price < auction->bid + AuctionEntry::CalculateAuctionOutBid(auction->bid))
        return "price " + std::to_string(price) + " below the next bid " +
               std::to_string(auction->bid + AuctionEntry::CalculateAuctionOutBid(auction->bid));
    return "";
}

bool CanInteract(ChatHandler* handler, Scene const& scene)
{
    if (scene.bot->GetNPCIfCanInteractWith(scene.auctioneer->GetGUID(), UNIT_NPC_FLAG_AUCTIONEER))
        return true;
    std::ostringstream why;
    why << "the bot cannot interact with " << scene.auctioneer->GetName() << " at " << std::fixed
        << std::setprecision(1) << scene.bot->GetExactDist(scene.auctioneer) << " yd";
    Fail(handler, scene, why.str());
    return false;
}

bool Post(ChatHandler* handler, Scene const& scene, std::vector<uint32> const& n)
{
    if (n.size() < 4)
        return Fail(handler, scene, "usage: post <bot> <item> <count> <bid> <buyout> <hours 12|24|48>");
    uint32 const count = n[0];
    uint32 const minutes = n[3] * MINUTES_PER_HOUR;
    if (!CanInteract(handler, scene))
        return false;
    Item* item = BagItem(scene.bot, scene.item, count);
    if (!item)
        return Fail(handler, scene, "no stack of " + std::to_string(count) + " in the bags");
    AuctionHouseEntry const* houseEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(scene.auctioneer->GetFaction());
    uint32 const deposit =
        houseEntry ? AuctionHouseMgr::GetAuctionDeposit(houseEntry, minutes * MINUTE, item, count) : 0;
    if (!scene.bot->HasEnoughMoney(deposit))
        return Fail(handler, scene, "the deposit " + std::to_string(deposit) + " is more than the purse");

    WorldPacket* packet = new WorldPacket(CMSG_AUCTION_SELL_ITEM, 8 + 4 + 8 + 4 + 4 + 4 + 4);
    *packet << scene.auctioneer->GetGUID() << uint32(1) << item->GetGUID() << count << n[1] << n[2] << minutes;
    Queue(scene.bot, packet);
    return Ok(handler, scene,
              "queued x" + std::to_string(count) + " of a stack of " + std::to_string(item->GetCount()) + ", bid " +
                  std::to_string(n[1]) + " buyout " + std::to_string(n[2]) + ", " + std::to_string(minutes) +
                  " min, deposit " + std::to_string(deposit) + " at " + scene.auctioneer->GetName());
}

bool Bid(ChatHandler* handler, Scene const& scene, std::vector<uint32> const& n, bool buyout)
{
    AuctionEntry* auction = Newest(scene.house, scene.item);
    if (!auction)
        return Fail(handler, scene, "no auction of the item");
    if (buyout && !auction->buyout)
        return Fail(handler, scene, "auction " + std::to_string(auction->Id) + " has no buyout");
    if (!buyout && n.empty())
        return Fail(handler, scene, "usage: bid <bot> <item> <price>");
    uint32 const price = buyout ? auction->buyout : n[0];
    std::string const refusal = BidRefusal(auction, scene.bot->GetGUID(), price);
    if (!refusal.empty())
        return Fail(handler, scene, refusal);
    if (!scene.bot->HasEnoughMoney(price))
        return Fail(handler, scene, "the price " + std::to_string(price) + " is more than the purse");
    if (!CanInteract(handler, scene))
        return false;

    WorldPacket* packet = new WorldPacket(CMSG_AUCTION_PLACE_BID, 8 + 4 + 4);
    *packet << scene.auctioneer->GetGUID() << auction->Id << price;
    Queue(scene.bot, packet);
    return Ok(handler, scene, "queued " + std::to_string(price) + " on auction " + std::to_string(auction->Id));
}

bool Cancel(ChatHandler* handler, Scene const& scene)
{
    AuctionEntry* auction = Newest(scene.house, scene.item);
    if (!auction)
        return Fail(handler, scene, "no auction of the item");
    if (auction->owner != scene.bot->GetGUID())
        return Fail(handler, scene, "auction " + std::to_string(auction->Id) + " is not the bot's");
    if (!CanInteract(handler, scene))
        return false;

    WorldPacket* packet = new WorldPacket(CMSG_AUCTION_REMOVE_ITEM, 8 + 4);
    *packet << scene.auctioneer->GetGUID() << auction->Id;
    Queue(scene.bot, packet);
    return Ok(handler, scene, "queued the cancel of auction " + std::to_string(auction->Id));
}

// The auction's end time is set to the past and the house updated: the core's own end path (expiry, or a sale to
// the bidder) runs now instead of at its next 60 s update
bool End(ChatHandler* handler, Scene const& scene)
{
    AuctionEntry* auction = Newest(scene.house, scene.item);
    if (!auction)
        return Fail(handler, scene, "no auction of the item");
    uint32 const id = auction->Id;
    bool const bidder = !auction->bidder.IsEmpty();
    auction->expire_time = static_cast<time_t>(GameTime::GetGameTime().count()) - 1;
    scene.house->Update();
    if (scene.house->GetAuction(id))
        return Fail(handler, scene, "auction " + std::to_string(id) + " still listed after the update");
    return Ok(handler, scene,
              "auction " + std::to_string(id) + (bidder ? " ended with a bid" : " ended without a bid"));
}

bool StandInPost(ChatHandler* handler, Scene const& scene, std::vector<uint32> const& n)
{
    if (n.size() < 4)
        return Fail(handler, scene, "usage: standin post <bot> <item> <count> <bid> <buyout> <hours 12|24|48>");
    AuctionHouseEntry const* houseEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(scene.auctioneer->GetFaction());
    Item* item = Item::CreateItem(scene.item, n[0]);
    if (!houseEntry || !item)
        return Fail(handler, scene, "no item or house entry");
    item->SetOwnerGUID(StandIn());

    AuctionEntry* auction = new AuctionEntry();
    auction->Id = sObjectMgr->GenerateAuctionID();
    auction->houseId = AuctionHouseId(houseEntry->houseId);
    auction->item_guid = item->GetGUID();
    auction->item_template = item->GetEntry();
    auction->itemCount = item->GetCount();
    auction->owner = StandIn();
    auction->startbid = n[1];
    auction->bidder = ObjectGuid::Empty;
    auction->bid = 0;
    auction->buyout = n[2];
    auction->expire_time = static_cast<time_t>(GameTime::GetGameTime().count()) + n[3] * HOUR;
    auction->deposit = 0;
    auction->auctionHouseEntry = houseEntry;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    item->SaveToDB(trans);
    sAuctionMgr->AddAItem(item);
    scene.house->AddAuction(auction);  // fires OnAuctionAdd
    auction->SaveToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    return Ok(handler, scene,
              "auction " + std::to_string(auction->Id) + " x" + std::to_string(auction->itemCount) + " bid " +
                  std::to_string(n[1]) + " buyout " + std::to_string(n[2]) + " of the stand-in");
}

// The stand-in's bid or buyout: what HandleAuctionPlaceBid does, without a session (the stand-in has no purse)
bool StandInBid(ChatHandler* handler, Scene const& scene, std::vector<uint32> const& n, bool buyout)
{
    AuctionEntry* auction = Newest(scene.house, scene.item);
    if (!auction)
        return Fail(handler, scene, "no auction of the item");
    if (buyout && !auction->buyout)
        return Fail(handler, scene, "auction " + std::to_string(auction->Id) + " has no buyout");
    if (!buyout && n.empty())
        return Fail(handler, scene, "usage: standin bid <bot> <item> <price>");
    uint32 const price = buyout ? auction->buyout : n[0];
    std::string const refusal = BidRefusal(auction, StandInBidder(), price);
    if (!refusal.empty())
        return Fail(handler, scene, refusal);

    uint32 const id = auction->Id;
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    if (price < auction->buyout || !auction->buyout)
    {
        if (auction->bidder && auction->bidder != StandInBidder())
            sAuctionMgr->SendAuctionOutbiddedMail(auction, price, nullptr, trans);
        auction->bidder = StandInBidder();
        auction->bid = price;
        sAuctionMgr->GetAuctionHouseSearcher()->UpdateBid(auction);
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_AUCTION_BID);
        stmt->SetData(0, auction->bidder.GetCounter());
        stmt->SetData(1, auction->bid);
        stmt->SetData(2, auction->Id);
        trans->Append(stmt);
        CharacterDatabase.CommitTransaction(trans);
        return Ok(handler, scene, "bid " + std::to_string(price) + " on auction " + std::to_string(id));
    }

    if (auction->bidder && auction->bidder != StandInBidder())
        sAuctionMgr->SendAuctionOutbiddedMail(auction, auction->buyout, nullptr, trans);
    auction->bidder = StandInBidder();
    auction->bid = auction->buyout;
    sAuctionMgr->SendAuctionSalePendingMail(auction, trans);
    sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
    sAuctionMgr->SendAuctionWonMail(auction, trans);
    sScriptMgr->OnAuctionSuccessful(scene.house, auction);
    auction->DeleteFromDB(trans);
    sAuctionMgr->RemoveAItem(auction->item_guid);
    scene.house->RemoveAuction(auction);  // fires OnAuctionRemove and deletes the entry
    CharacterDatabase.CommitTransaction(trans);
    return Ok(handler, scene, "bought out auction " + std::to_string(id) + " for " + std::to_string(price));
}

std::string BidderWord(uint64 raw, Player const* bot)
{
    if (!raw)
        return "none";
    if (raw == StandInBidder().GetRawValue())
        return "standin";
    if (raw == bot->GetGUID().GetRawValue())
        return "bot";
    return ObjectGuid(raw).ToString();
}

// The record and chance keys of `check` (AH6): the value and how far a wanted one may lie from it, since every publish
// adds the seconds a lot stood and the records decay. Sales in lots, hours in lot hours, p12/p24/p48 the sale chance at
// that duration in permille through the one rule (AuctionRules::SaleChance)
bool RecordKey(std::string const& key, AuctionIndex::Evidence const& evidence, double& value, double& tolerance)
{
    AuctionConfig const& config = sPlayerbotAIConfig.Auction;
    if (key == "itemsales" || key == "kindsales")
    {
        value = key == "itemsales" ? evidence.item.sales : evidence.kind.sales;
        tolerance = 0.01;
    }
    else if (key == "itemhours" || key == "kindhours")
    {
        value = key == "itemhours" ? evidence.item.hours : evidence.kind.hours;
        tolerance = 0.1;
    }
    else if (key == "p12" || key == "p24" || key == "p48")
    {
        double const hours = key == "p12" ? 12.0 : key == "p24" ? 24.0 : 48.0;
        value = 1000.0 * AuctionRules::SaleChance(evidence.item, evidence.kind, hours, config);
        tolerance = 1.0;
    }
    else
        return false;
    return true;
}

bool Check(ChatHandler* handler, Scene const& scene, std::vector<std::string> const& words)
{
    AuctionIndex::PublishNow();
    std::shared_ptr<AuctionIndex::Snapshot const> const snapshot = AuctionIndex::Current();
    if (!snapshot)
        return Fail(handler, scene, "nothing published");
    static AuctionIndex::ItemEntry const EMPTY;
    AuctionIndex::ItemEntry const* found = snapshot->Find(scene.item);
    AuctionIndex::ItemEntry const& entry = found ? *found : EMPTY;
    AuctionIndex::OfferLine const* first = entry.offers.cheapest.empty() ? nullptr : &entry.offers.cheapest.front();
    std::string const bidder = first ? BidderWord(first->bidder, scene.bot) : std::string("none");
    uint32 const bid = first ? first->bid : 0;
    AuctionIndex::Evidence const evidence = snapshot->EvidenceOf(scene.item, AuctionIndex::KindOfEntry(scene.item));
    AuctionConfig const& config = sPlayerbotAIConfig.Auction;

    std::ostringstream have;
    have << "offers " << entry.offers.auctions << " sales " << entry.typical.sample << " expiries "
         << entry.typical.expiries << " unit " << entry.typical.unitPrice << " bid " << bid << " bidder " << bidder
         << " (seq " << snapshot->sequence << ", since start " << snapshot->sales << " sales " << snapshot->expiries
         << " expiries " << snapshot->cancels << " cancels)" << std::fixed << std::setprecision(3) << "; records item "
         << evidence.item.sales << " sales " << evidence.item.hours << " hours, kind " << evidence.kind.sales
         << " sales " << evidence.kind.hours << " hours; p12 "
         << 1000.0 * AuctionRules::SaleChance(evidence.item, evidence.kind, 12.0, config) << " p24 "
         << 1000.0 * AuctionRules::SaleChance(evidence.item, evidence.kind, 24.0, config) << " p48 "
         << 1000.0 * AuctionRules::SaleChance(evidence.item, evidence.kind, 48.0, config);
    std::string differs;
    for (size_t i = 0; i + 1 < words.size(); i += 2)
    {
        std::string const& key = words[i];
        std::string const& want = words[i + 1];
        double value = 0.0;
        double tolerance = 0.0;
        if (RecordKey(key, evidence, value, tolerance))
        {
            char* end = nullptr;
            double const wanted = std::strtod(want.c_str(), &end);
            if (end == want.c_str() || std::abs(value - wanted) > tolerance)
            {
                std::ostringstream off;
                off << std::fixed << std::setprecision(3) << key << " " << value << " not " << want << " (within "
                    << tolerance << ")";
                differs += (differs.empty() ? "" : "; ") + off.str();
            }
            continue;
        }
        std::string got;
        if (key == "offers")
            got = std::to_string(entry.offers.auctions);
        else if (key == "sales")
            got = std::to_string(entry.typical.sample);
        else if (key == "expiries")
            got = std::to_string(entry.typical.expiries);
        else if (key == "unit")
            got = std::to_string(entry.typical.unitPrice);
        else if (key == "bid")
            got = std::to_string(bid);
        else if (key == "bidder")
            got = bidder;
        else
            return Fail(handler, scene,
                        "no key '" + key +
                            "' (offers, sales, expiries, unit, bid, bidder, itemsales, itemhours, kindsales, "
                            "kindhours, p12, p24, p48)");
        if (got != want)
            differs += (differs.empty() ? "" : "; ") + key + " " + got + " not " + want;
    }
    if (words.size() % 2)
        return Fail(handler, scene, "a key without a value");
    if (!differs.empty())
        return Fail(handler, scene, differs + "; have " + have.str());
    return Ok(handler, scene, have.str());
}

struct MailKind
{
    char const* word;
    MailAuctionAnswers answer;
    bool item;   // the mail carries the auction's item
    bool money;  // the mail carries money
};

constexpr MailKind MAIL_KINDS[] = {
    {"outbid", AUCTION_OUTBIDDED, false, true},           {"won", AUCTION_WON, true, false},
    {"successful", AUCTION_SUCCESSFUL, false, true},      {"expired", AUCTION_EXPIRED, true, false},
    {"refund", AUCTION_CANCELLED_TO_BIDDER, false, true}, {"cancelled", AUCTION_CANCELED, true, false},
    {"pending", AUCTION_SALE_PENDING, false, false},
};

// The subject the core writes: <item>:0:<answer>:<auction id>:<count>
bool ParseSubject(std::string const& subject, uint32& item, uint32& answer)
{
    std::istringstream in(subject);
    std::string part;
    std::vector<uint32> parts;
    while (std::getline(in, part, ':'))
        parts.push_back(static_cast<uint32>(std::strtoul(part.c_str(), nullptr, 10)));
    if (parts.size() < 3)
        return false;
    item = parts[0];
    answer = parts[2];
    return true;
}

// The bot's delivered auction mails of the scene's item, by kind (a mail of a kind that carries the item or money
// counts only with it); `money C` compares the money of the named kinds' mails
bool MailCount(ChatHandler* handler, Scene const& scene, std::vector<std::string> const& words)
{
    time_t const now = static_cast<time_t>(GameTime::GetGameTime().count());
    uint32 counts[std::size(MAIL_KINDS)] = {};
    uint32 money[std::size(MAIL_KINDS)] = {};
    uint32 waiting = 0;
    for (Mail const* mail : scene.bot->GetMails())
    {
        uint32 item = 0;
        uint32 answer = 0;
        if (!mail || mail->state == MAIL_STATE_DELETED || mail->messageType != MAIL_AUCTION ||
            !ParseSubject(mail->subject, item, answer) || item != scene.item)
            continue;
        if (mail->deliver_time > now)
        {
            ++waiting;
            continue;
        }
        for (size_t k = 0; k < std::size(MAIL_KINDS); ++k)
            if (MAIL_KINDS[k].answer == answer && (!MAIL_KINDS[k].item || !mail->items.empty()) &&
                (!MAIL_KINDS[k].money || mail->money))
            {
                ++counts[k];
                money[k] += mail->money;
            }
    }

    std::ostringstream have;
    for (size_t k = 0; k < std::size(MAIL_KINDS); ++k)
        have << MAIL_KINDS[k].word << " " << counts[k] << " ";
    have << "(" << waiting << " not delivered yet)";
    std::string differs;
    uint32 namedMoney = 0;
    for (size_t i = 0; i + 1 < words.size(); i += 2)
    {
        uint32 const want = static_cast<uint32>(std::strtoul(words[i + 1].c_str(), nullptr, 10));
        if (words[i] == "money")
        {
            if (namedMoney != want)
                differs += (differs.empty() ? "" : "; ") + std::string("money ") + std::to_string(namedMoney) +
                           " not " + words[i + 1];
            continue;
        }
        size_t k = 0;
        while (k < std::size(MAIL_KINDS) && words[i] != MAIL_KINDS[k].word)
            ++k;
        if (k == std::size(MAIL_KINDS))
            return Fail(handler, scene, "no mail kind '" + words[i] + "'");
        namedMoney += money[k];
        if (counts[k] != want)
            differs +=
                (differs.empty() ? "" : "; ") + words[i] + " " + std::to_string(counts[k]) + " not " + words[i + 1];
    }
    if (words.size() % 2)
        return Fail(handler, scene, "a kind without a count");
    if (!differs.empty())
        return Fail(handler, scene, differs + "; have " + have.str());
    return Ok(handler, scene, have.str() + ", money of the named kinds " + std::to_string(namedMoney));
}
}  // namespace

bool HandleCommand(ChatHandler* handler, char const* args)
{
    PLAYERBOT_CHECK_WORLD_THREAD("AuctionFixture command");
    std::istringstream in(args ? args : "");
    Scene scene;
    in >> scene.verb;
    if (scene.verb == "standin")
    {
        std::string act;
        in >> act;
        scene.verb += " " + act;
    }
    std::string itemWord;
    in >> scene.botName >> itemWord;
    std::vector<std::string> words;
    for (std::string word; in >> word;)
        words.push_back(word);
    scene.item = static_cast<uint32>(std::strtoul(itemWord.c_str(), nullptr, 10));

    std::string asked = scene.botName;
    Player* bot = normalizePlayerName(asked) ? ObjectAccessor::FindPlayerByName(asked) : nullptr;
    if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        return Fail(handler, scene,
                    "no bot online by that name; usage: playerbots auction fixture post|bid|buyout|"
                    "cancel|end|standin post|standin bid|standin buyout|check|mail|grow|listed <bot> <item> ..");
    scene.bot = bot;
    if (!sObjectMgr->GetItemTemplate(scene.item))
        return Fail(handler, scene, "no item " + itemWord);
    if (scene.verb == "grow")
    {
        uint32 const units = words.empty() ? 0 : static_cast<uint32>(std::strtoul(words[0].c_str(), nullptr, 10));
        if (!units)
            return Fail(handler, scene, "no units; usage: grow <bot> <item> <units>");
        BankMail::ArmAuctionGrow(bot, scene.item, units);
        return Ok(handler, scene, "its next listing grows by " + std::to_string(units) + " units after the packet");
    }
    if (scene.verb == "listed")
    {
        double const hours = words.empty() ? 0.0 : std::strtod(words[0].c_str(), nullptr);
        if (!(hours > 0.0))
            return Fail(handler, scene, "no hours; usage: listed <bot> <item> <hours>");
        if (!AuctionIndex::CreditListing(scene.item, hours))
            return Fail(handler, scene, "the item has no kind");
        return Ok(handler, scene, "credited " + words[0] + " listing hours to the item's record and its kind's");
    }
    scene.auctioneer = NearestAuctioneer(bot);
    if (!scene.auctioneer)
        return Fail(handler, scene,
                    "no auctioneer within " + std::to_string(static_cast<int>(AUCTIONEER_SEARCH_YARDS)) + " yd");
    scene.house = sAuctionMgr->GetAuctionsMap(scene.auctioneer->GetFaction());
    if (!scene.house)
        return Fail(handler, scene, "no auction house for the auctioneer");

    std::vector<uint32> numbers;
    for (std::string const& word : words)
        numbers.push_back(static_cast<uint32>(std::strtoul(word.c_str(), nullptr, 10)));
    if (scene.verb == "post")
        return Post(handler, scene, numbers);
    if (scene.verb == "bid" || scene.verb == "buyout")
        return Bid(handler, scene, numbers, scene.verb == "buyout");
    if (scene.verb == "cancel")
        return Cancel(handler, scene);
    if (scene.verb == "end")
        return End(handler, scene);
    if (scene.verb == "standin post")
        return StandInPost(handler, scene, numbers);
    if (scene.verb == "standin bid" || scene.verb == "standin buyout")
        return StandInBid(handler, scene, numbers, scene.verb == "standin buyout");
    if (scene.verb == "check")
        return Check(handler, scene, words);
    if (scene.verb == "mail")
        return MailCount(handler, scene, words);
    return Fail(handler, scene, "no verb '" + scene.verb + "'");
}
}  // namespace AuctionFixture
