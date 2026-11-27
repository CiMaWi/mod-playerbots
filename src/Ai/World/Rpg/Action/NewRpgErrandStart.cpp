/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "AuctionBuyList.h"
#include "AuctionIndex.h"
#include "BankMailConfig.h"
#include "BankMailService.h"
#include "BankMailState.h"
#include "BankRulesInput.h"
#include "BankVisit.h"
#include "BotTraitMgr.h"
#include "BotTravel.h"
#include "CheckMountStateAction.h"
#include "DBCStores.h"
#include "ErrandRules.h"
#include "InterludeInputs.h"
#include "InventorySlots.h"
#include "ItemUsageValue.h"
#include "ItemWorthRules.h"
#include "MailVisit.h"
#include "MountRules.h"
#include "NewRpgAction.h"
#include "NewRpgErrandInternal.h"
#include "NewRpgInfo.h"
#include "NewRpgInternal.h"
#include "NewRpgStatusEnd.h"
#include "NewRpgTriggers.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "PlannerIndexes.h"
#include "PlannerService.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestIndex.h"
#include "RouteService.h"
#include "SkillPathLive.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "StartingHistoryRules.h"
#include "TestConstraints.h"
#include "TestProbe.h"
#include "TraitRules.h"
#include "TravelGraph.h"
#include "TravelMgr.h"
#include "UpkeepIndex.h"
#include "UpkeepNeedRules.h"
#include "UpkeepRules.h"
#include "WorldLog.h"
#include "WorldLogFormat.h"
#include "WorldLogStatus.h"
#include "WorldTravelConfig.h"
#include "WorldTravelRules.h"

using namespace NewRpgInternal;

namespace
{
// The stable masters on the bot's map that trade with it (WU21c), the nearest STABLE_STOPS within reach
std::vector<Planner::Service const*> StableMasterSpawns(Player* bot, Planner::PlannerIndexes const& indexes,
                                                        float reachYards)
{
    constexpr std::size_t STABLE_STOPS = 3;
    std::vector<Planner::Service const*> result;
    for (Planner::Nearby const& found :
         indexes.NearestAll(Planner::ServiceKind::StableMaster, bot->GetMapId(), bot->GetPositionX(),
                            bot->GetPositionY(), bot->GetPositionZ(), reachYards))
    {
        Planner::Service const& spawn = indexes.services[found.index];
        if (bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(spawn.factionTemplate), bot) <= REP_UNFRIENDLY)
            continue;
        result.push_back(&spawn);
        if (result.size() == STABLE_STOPS)
            break;
    }
    return result;
}

// Mailboxes and bankers (each kind's nearest few) that join the trip's stops by position
constexpr std::size_t BANK_MAIL_STOPS = 4;

// What the buy list's lines save at their typical prices (WA4b, AuctionBuyList.h WantedWorth; WA.4: no current offer
// read here) for the gold the market may spend: the needs list's (cached per trip state, ErrandNeeds)
uint32 WantedWorthOf(PlayerbotAI* botAI, Player* bot)
{
    std::shared_ptr<Planner::UpkeepIndex const> const upkeep =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    std::shared_ptr<AuctionIndex::Snapshot const> const market = AuctionIndex::Current();
    std::shared_ptr<UpkeepNeeds::VisitNeeds const> const needs =
        upkeep && market ? NewRpgInternal::ErrandNeeds(botAI, upkeep) : nullptr;
    if (!needs)
        return 0;
    uint64 const spendable = AuctionRules::Spendable(bot->GetMoney(), UpkeepNeeds::MarketKept(needs->needs), 0);
    if (!spendable)
        return 0;
    uint64 worth = 0;
    for (AuctionRules::BuyLine const& line : Planner::BuyListOf(botAI))
        if (AuctionIndex::ItemEntry const* entry = market->Find(line.item))
            worth += AuctionRules::WantedWorth(line, entry->typical.unitPrice, spendable);
    return static_cast<uint32>(std::min<uint64>(worth, std::numeric_limits<uint32>::max()));
}

// The bank and mail part of the trip planner's view (WB8): only a bot the bank rule governs (keeps its possessions).
// With an errand to offer, the nearest mailboxes and bankers serving the bot within the walk limit go onto the vendor
// stops within STOP_JOIN_YARDS, or stand as stops of their own (MergeServiceStops)
void AddBankMail(PlayerbotAI* botAI, Player* bot, float reachYards, WorldTravel::ErrandSnapshot& snapshot)
{
    snapshot.Governed = botAI->KeepsPossessions();
    if (!snapshot.Governed)
        return;

    BankMail::BotBankMail& known = botAI->bankMail;
    BankMail::RefreshMailKnowledge(bot, known.mail);
    BankMail::DueTotal const due = BankMail::DueAt(known.bank.kept, bot->GetLevel());
    uint32 carried = 0;
    for (BankMail::KeptEntry const& item : known.bank.carried.items)
        // A due stack stays (auction stock, WA7b), so does one the core will not bank
        carried += item.dueLevel > bot->GetLevel() && !item.blocked ? item.count : 0;
    snapshot.Orderliness = BotTraitMgr::instance().Trait(bot->GetGUID().GetCounter(), TraitRules::Trait::Orderliness);
    snapshot.MailWaiting = known.mail.waiting;
    snapshot.MailDefaultWorth = sPlayerbotAIConfig.BankMail.MailDefaultWorthCopper;
    snapshot.MailSeenCopper = known.mail.seenCopper;
    snapshot.MailExpectedCopper = known.mail.expectedCopper;
    snapshot.BankDueCount = due.count;
    snapshot.BankDueWorth = due.worth;
    snapshot.BankDepositSlots = std::min<uint32>(carried, known.bank.freeSlots);
    // The bag stock's gain over a vendor, as SellableSlots' refresh just decided it at this level (WA3b)
    if (known.bank.carried.level == bot->GetLevel())
        snapshot.AuctionStockWorth = known.bank.carried.auctionGain;
    snapshot.AuctionWantedWorth = WantedWorthOf(botAI, bot);
    // NR12b: the banked sale stock's gain draws an auctioneer and the banker before it, only together (PlanTrip)
    snapshot.BankStockWorth = known.bank.stockGain;

    bool const mail = snapshot.MailWaiting || snapshot.MailSeenCopper || snapshot.MailExpectedCopper;
    bool const bank = snapshot.BankDueCount || snapshot.BankDepositSlots || snapshot.BankStockWorth;
    // The auction plan's worths (WA3b stock; WA4b wanted lines; NR12b banked stock): an auctioneer joins the stops only
    // for them
    bool const auction = snapshot.AuctionStockWorth || snapshot.AuctionWantedWorth || snapshot.BankStockWorth;
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!indexes || (!mail && !bank && !auction))
        return;

    std::vector<WorldTravel::ServiceStop> extras;
    auto add = [&](Planner::ServiceKind kind, uint32 service)
    {
        for (Planner::Nearby const& found :
             indexes->Nearest(kind, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                              BANK_MAIL_STOPS, reachYards))
        {
            Planner::Service const& spot = indexes->services[found.index];
            if ((kind == Planner::ServiceKind::Banker || kind == Planner::ServiceKind::Auctioneer) &&
                !BankMail::BankerServes(bot, spot.factionTemplate))
                continue;
            extras.push_back({kind == Planner::ServiceKind::Mailbox ? 0 : spot.entry,
                              {spot.mapId, spot.x, spot.y, spot.z},
                              service});
        }
    };
    if (mail)
        add(Planner::ServiceKind::Mailbox, WorldTravel::SERVICE_MAILBOX);
    if (bank)
        add(Planner::ServiceKind::Banker, WorldTravel::SERVICE_BANKER);
    if (auction)
        add(Planner::ServiceKind::Auctioneer, WorldTravel::SERVICE_AUCTIONEER);
    WorldTravel::MergeServiceStops(snapshot.Stops, extras);
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
// One PerfMonitor sample of a block's time; nothing while PerfMonitor is off
class ErrandTimer
{
public:
    explicit ErrandTimer(char const* name) : _row(sPerfMonitor.acquire(PERF_MON_TOTAL, name))
    {
        if (_row)
            _started = std::chrono::steady_clock::now();
    }
    ~ErrandTimer()
    {
        if (_row)
            _row->Record(static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _started)
                    .count()));
    }
    ErrandTimer(ErrandTimer const&) = delete;
    ErrandTimer& operator=(ErrandTimer const&) = delete;

private:
    PerformanceData* _row;
    std::chrono::steady_clock::time_point _started;
};

// The trip planned with its vendor stops held to the walk limit by their route (NR13): one search each from the bot,
// asked as the trip's walk asks it (NewRpgTravelAction::RequestRoute) with the flight maps off (no flight search on a
// map thread, contract C3). Before the travel graph is ready the straight distance stands. The PerfMonitor rows
// "errand trip plan" (one per start) and "errand vendor route" (one per search) give the searches per start
WorldTravel::TripPlan PlanErrandTrip(PlayerbotAI* botAI, WorldTravel::TripRequest const& request,
                                     std::vector<WorldTravel::ServiceStop> const& stops,
                                     std::vector<WorldTravel::ErrandOffer> const& offers,
                                     std::vector<WorldTravel::DroppedVendor>& dropped)
{
    ErrandTimer const timer("errand trip plan");
    if (!TravelGraph::Instance().IsReady())
        return WorldTravel::PlanTrip(request, stops, offers);

    Player* bot = botAI->GetBot();
    std::optional<TravelRoute::Snapshot> snapshot;
    std::shared_ptr<TravelRoute::StuckEdges const> stuck;
    TravelRoute::Params params;
    auto const routeSeconds = [&](WorldTravel::ErrandPlace const& place)
    {
        ErrandTimer const searchTimer("errand vendor route");
        if (!snapshot)
        {
            snapshot = TravelRoute::FillSnapshot(bot, 0);
            snapshot->allowTransport = true;
            snapshot->hasHearthstone =
                snapshot->hasHearthstone && TestConstraints::HearthAllowed(botAI->testConstraints);
            if (!TestConstraints::TaxiAllowed(botAI->testConstraints))
                snapshot->knownTaxi.fill(0);
            if (!TestConstraints::OwnFlyingAllowed(botAI->testConstraints))
                snapshot->flyOutland = snapshot->flyNorthrend = false;
            stuck = TravelRoute::StuckBook::Instance().Current();
            params = TravelRoute::ConfiguredParams();
            params.flightMaps = nullptr;
        }
        TravelRoute::Request ask(*snapshot, {place.MapId, place.X, place.Y, place.Z});
        ask.freeRides = TestConstraints::RidesFree(botAI->testConstraints);
        TravelRoute::Result const route = TravelRoute::Search(TravelGraph::Instance(), ask, *stuck, params);
        float const seconds = route.ok ? route.totalSeconds : -1.0f;
        LOG_DEBUG("playerbots", "[New RPG] {} errand vendor at ({:.0f}, {:.0f}, {:.0f}): route {:.0f} s",
                  bot->GetName(), place.X, place.Y, place.Z, seconds);
        return seconds;
    };
    return WorldTravel::PlanTripByRoute(request, stops, offers, routeSeconds, dropped);
}
}  // namespace

bool NewRpgInternal::StartErrand(PlayerbotAI* botAI, ErrandOrder const& order)
{
    Player* bot = botAI->GetBot();
    AiObjectContext* context = botAI->GetAiObjectContext();
    // A held errand row refuses the trigger's trip, also one fired before the hold was taken (WU15a); an order passes
    // it. An order skips the trigger's worth rules, not its refusals of what cannot be done (ErrandMayStart); a sick
    // bot's order passes only once recover has handed it over (NR3: the goal model's sick gate chose the errand)
    if (order.order ? (BankMail::VisitRuns(botAI->bankMail) || bot->IsInCombat() || !bot->IsAlive() ||
                       (bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS) && !WorldTravel::SickErrandOpen(botAI)) ||
                       bot->GetGroup() || bot->GetMap()->Instanceable())
                    : !TestConstraints::ErrandAllowed(botAI->testConstraints))
        return false;

    WorldTravel::SlotMeasure& measure = context->GetValue<WorldTravel::SlotMeasure>("slot measure")->RefGet();
    // The repair need the trigger saw (its wear test first)
    bool const repairWanted = WorldTravel::RepairErrandWanted(bot);
    WorldTravel::StampErrandCheck(measure, getMSTime());
    measure.HasVendor = false;
    // The errand stops come from the planner's index; while it is built (a worker) the check finds no vendor
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    if (!indexes)
        return false;

    // The needs (upkeep §3.3): the consumable stocks the gold pays a lot of, the due ones and the others, and the
    // vendors that cover them (the immutable index; one snapshot on the bot's map thread)
    std::shared_ptr<Planner::UpkeepIndex const> const index =
        Planner::UpkeepIndexReady() ? Planner::CurrentUpkeepIndex() : nullptr;
    std::shared_ptr<UpkeepNeeds::VisitNeeds const> const visit = index ? ErrandNeeds(botAI, index) : nullptr;
    UpkeepNeeds::NeedList needs;
    std::unordered_map<uint32, Planner::StockMask> cover;
    Planner::StockMask dueKinds = 0;
    Planner::StockMask unmetKinds = 0;
    if (visit)
    {
        needs = visit->needs;
        dueKinds = UpkeepNeeds::PayableKinds(needs, true);
        unmetKinds = UpkeepNeeds::PayableKinds(needs, false);
        if (dueKinds | unmetKinds)
            cover = UpkeepNeeds::VendorCover(*index, visit->snapshot, needs, dueKinds | unmetKinds);
    }

    // The vendors on the bot's map that trade with the bot (the index; no route to other maps yet): each one an
    // upkeep candidate, the nearest one for the destroy rule, the nearest VENDOR_STOPS within the errand walk limit as
    // the trip's service stops
    constexpr std::size_t VENDOR_STOPS = 8;
    uint32 const mapId = bot->GetMapId();
    float const runSpeed = bot->GetSpeed(MOVE_RUN);
    float const reachYards = WorldTravel::ERRAND_MAX_WALK_MINUTES * MINUTE * runSpeed;
    std::vector<Planner::Nearby> const vendors = indexes->NearestAll(
        Planner::ServiceKind::Vendor, mapId, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    std::vector<UpkeepNeeds::VendorCandidate> candidates;
    Planner::Service const* nearest = nullptr;
    float nearestYards = std::numeric_limits<float>::max();
    std::vector<std::pair<float, Planner::Service const*>> inReach;
    uint32 refused = 0;
    for (Planner::Nearby const& found : vendors)
    {
        Planner::Service const& vendor = indexes->services[found.index];
        // The core's interact test (Player::GetNPCIfCanInteractWith): the vendor's reaction must be above Unfriendly.
        // A template without a reputation faction is judged by the template relation, not refused as Neutral.
        if (bot->GetFactionReactionTo(sFactionTemplateStore.LookupEntry(vendor.factionTemplate), bot) <= REP_UNFRIENDLY)
        {
            ++refused;
            continue;
        }

        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(vendor.entry);
        float const yards = found.distance;
        auto const covered = cover.find(vendor.entry);
        candidates.push_back({vendor.entry, WorldTravel::WalkMinutes(yards, runSpeed),
                              proto && (proto->npcflag & UNIT_NPC_FLAG_REPAIR),
                              covered != cover.end() ? covered->second : Planner::StockMask(0)});
        if (!nearest)
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

    Planner::StockMask dueInReach = 0;
    for (UpkeepNeeds::VendorCandidate const& candidate : candidates)
        if (candidate.walkMinutes <= WorldTravel::ERRAND_MAX_WALK_MINUTES)
            dueInReach |= static_cast<Planner::StockMask>(candidate.covers & dueKinds);
    UpkeepNeeds::Need const* training = nullptr;
    UpkeepNeeds::Need const* riding = nullptr;  // one rank per trip, the lowest the funded gold pays
    UpkeepNeeds::Need const* weapon = nullptr;  // one weapon skill per trip, the first the funded gold pays
    UpkeepNeeds::Need const* pet = nullptr;     // a hunter without a pet: the beast to tame (WU21b)
    bool const buysAtVendors = BuysAtVendors(botAI);
    // One profession learn or rank per trip (WR4b); a tool is a stock need, bought by the shop errand (WR5b)
    UpkeepNeeds::Need const* const profession = ProfessionErrand(needs, buysAtVendors);
    bool const professionDue = profession && UpkeepNeeds::ProfessionErrandDue(*profession);
    // A test bot's profession inputs at its check: the needs, and the kept path's ranks (UpkeepNeeds::ComputeNeeds
    // skips a capped skill's rank that a known path does not buy)
    if (botAI->testConstraints && visit && sLog->ShouldLog("playerbots", LOG_LEVEL_DEBUG))
    {
        std::string text;
        for (UpkeepNeeds::Need const& need : needs.needs)
            if (need.kind == UpkeepKind::Profession)
                text += " need " + std::to_string(need.skill) + " step " + std::to_string(uint32(need.step)) +
                        " copper " + std::to_string(need.copper) + " funded " + std::to_string(need.funded) +
                        (need.due ? " due" : "");
        SkillPath::Path const* const path = Planner::KeptPath(botAI);
        text += path ? " path chosen " + std::to_string(path->chosen) : std::string(" no path");
        if (path)
            for (SkillPath::Purchase const& purchase : path->needs)
                text += " buys " + std::to_string(purchase.spell);
        for (Planner::Skill const& held : visit->snapshot.skills)
            text +=
                " skill " + std::to_string(held.id) + " " + std::to_string(held.value) + "/" + std::to_string(held.max);
        LOG_DEBUG("playerbots", "[Professions] {} errand check:{}", bot->GetName(), text);
    }
    for (UpkeepNeeds::Need const& need : needs.needs)
    {
        if (need.kind == UpkeepKind::TrainerSpell)
            training = &need;
        else if (!riding && buysAtVendors && UpkeepNeeds::RidingErrandDue(need))
            riding = &need;
        else if (!weapon && buysAtVendors && UpkeepNeeds::WeaponErrandDue(need))
            weapon = &need;
        else if (!pet && buysAtVendors && UpkeepNeeds::PetErrandDue(need))
            pet = &need;
        Planner::StockMask const bit = Planner::StockBit(need.stock);
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

    // A map without a vendor that trades with the bot still has its trainers (WU12 NOTE 4): a due class training, a
    // riding rank or a weapon skill to learn plans its trainer trip
    bool const trainerWanted = index && ((trainingDue && training->trainer) || (riding && riding->trainer) ||
                                         (weapon && weapon->trainer) || (professionDue && profession->trainer));
    // A trip for the mail or the bank needs no vendor either (WB8b)
    bool const bankMailWanted =
        !botAI->HasGameClientMaster() && BankMail::ErrandWaits(botAI->bankMail, bot->GetLevel());
    if (!WorldTravel::ErrandCheckPlans(nearest != nullptr, trainerWanted || bankMailWanted || pet))
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
        measure.VendorX = nearest->x;
        measure.VendorY = nearest->y;
        measure.VendorZ = nearest->z;
    }

    uint32 const sellableSlots = NewRpgInternal::SellableSlots(botAI);
    uint32 const freeSlots = WorldTravel::FreeGeneralSlots(bot);

    // The trip planner's view of the bot (ErrandRules.h): what each errand is worth now and where it can be done
    WorldTravelConfig const& config = sPlayerbotAIConfig.WorldTravel;
    WorldTravel::SlotBand const& band = config.SlotBands[WorldTravel::LevelBand(bot->GetLevel())];
    WorldTravel::ErrandSnapshot snapshot;
    snapshot.Level = bot->GetLevel();
    snapshot.Money = bot->GetMoney();
    snapshot.Rates = WorldTravel::CurrentRates(measure, band);
    snapshot.BagHorizonHours = band.BagHorizonHours;
    snapshot.Weight = Planner::CopperWeightOf(botAI);
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
            for (std::pair<float, Planner::Service const*> const& vendor : inReach)
                if (vendor.first < mountYards &&
                    UpkeepNeeds::MountSoldBy(*index, bot->getRace(), bot->getClass(), *rank, vendor.second->entry))
                {
                    mountYards = vendor.first;
                    mountVendor = vendor.second->entry;
                }
        snapshot.Riding = {riding->item, mountVendor, riding->trainer != 0};
    }
    if (weapon)
        snapshot.Weapon = {weapon->item};
    if (profession)
        snapshot.Profession = {profession->item, professionDue};
    if (pet && pet->swap)
        snapshot.Stable = {pet->beast, {pet->beastAt.map, pet->beastAt.x, pet->beastAt.y, pet->beastAt.z}};
    else if (pet)
        snapshot.Pet = {pet->beast, {pet->beastAt.map, pet->beastAt.x, pet->beastAt.y, pet->beastAt.z}};
    // The nearest VENDOR_STOPS vendors within the walk limit (inReach is nearest first), and as many more of those
    // that answer a need; past that cap a stop still joins when it answers what no nearer stop does (a city's many
    // food and tool vendors do not crowd out its one poison or reagent vendor)
    std::size_t plainStops = 0;
    std::size_t needStops = 0;
    Planner::StockMask dueTaken = 0;
    bool repairTaken = false;
    bool mountTaken = false;
    for (std::pair<float, Planner::Service const*> const& vendor : inReach)
    {
        uint32 const entry = vendor.second->entry;
        auto const covered = cover.find(entry);
        Planner::StockMask const covers = covered != cover.end() ? covered->second : Planner::StockMask(0);
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
        bool const repairs = creature && (creature->npcflag & UNIT_NPC_FLAG_REPAIR);
        bool const repairStop = repairing && repairs;
        bool const mountStop = snapshot.Riding.MountVendor && entry == snapshot.Riding.MountVendor;
        // A stop answers a need only when it covers a due kind, repairs while a repair is due, or sells the mount
        bool const answers = (covers & dueKinds) || repairStop || mountStop;
        bool const answersNew =
            (covers & dueKinds & ~dueTaken) || (repairStop && !repairTaken) || (mountStop && !mountTaken);
        std::size_t& count = answers ? needStops : plainStops;
        if (count >= VENDOR_STOPS && !answersNew)
            continue;

        dueTaken |= static_cast<Planner::StockMask>(covers & dueKinds);
        repairTaken = repairTaken || repairStop;
        mountTaken = mountTaken || mountStop;
        ++count;
        snapshot.Stops.push_back(
            {entry, ErrandPlaceOf(*vendor.second), WorldTravel::VendorServices(snapshot, entry, repairs, covers)});
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
                AddTrainerStops(TrainerSpawns(bot, *indexes, *index, trainer.key, reachYards),
                                WorldTravel::SERVICE_TRAINER, trainerStops);
        WorldTravel::JoinTrainers(snapshot.Stops, trainerStops, WorldTravel::SERVICE_TRAINER);
        snapshot.Stops.insert(snapshot.Stops.end(), trainerStops.begin(), trainerStops.end());
    }
    // Every riding trainer that teaches the rank: each race's riding trainer is a trainer of its own
    if (index && riding && riding->trainer)
        for (Planner::UpkeepTrainer const& trainer : index->trainers)
            if (trainer.kind == Planner::TrainerKind::Mount && UpkeepNeeds::Teaches(*index, trainer, riding->item))
                AddTrainerStops(TrainerSpawns(bot, *indexes, *index, trainer.key, reachYards),
                                WorldTravel::SERVICE_RIDING_TRAINER, snapshot.Stops);
    // Every weapon master that teaches the skill (WU15a); a vendor stop within STOP_JOIN_YARDS of one that has no
    // trainer yet takes it too
    if (index && weapon)
    {
        std::vector<WorldTravel::ServiceStop> masterStops;
        for (Planner::WeaponSkill const& skill : index->weaponSkills)
            if (skill.spellId == weapon->item)
                for (uint32 const key : skill.trainers)
                    AddTrainerStops(TrainerSpawns(bot, *indexes, *index, key, reachYards),
                                    WorldTravel::SERVICE_WEAPON_MASTER, masterStops);
        WorldTravel::JoinTrainers(snapshot.Stops, masterStops, WorldTravel::SERVICE_WEAPON_MASTER);
        snapshot.Stops.insert(snapshot.Stops.end(), masterStops.begin(), masterStops.end());
    }
    // Every tradeskill trainer that teaches the profession spell (WR4b)
    if (index && profession)
        AddProfessionTeachers(bot, *indexes, *index, profession->item, reachYards, snapshot.Stops);
    // The stable masters for a swap of the hunter's pet (WU21c)
    if (pet && pet->swap)
        AddTrainerStops(StableMasterSpawns(bot, *indexes, reachYards), WorldTravel::SERVICE_STABLE_MASTER,
                        snapshot.Stops);
    AddCompletedQuests(bot, snapshot.Completed);
    AddBankMail(botAI, bot, reachYards, snapshot);
    AddCraft(botAI, bot, *indexes, reachYards, snapshot);

    // No stop the bot makes anyway yet (WT16's goals bring the route): every stop is a detour that must pay
    WorldTravel::TripRequest request;
    request.Start = {mapId, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    request.YardsPerSecond = runSpeed;
    request.Rates = snapshot.Rates;
    std::vector<WorldTravel::ErrandOffer> offers = WorldTravel::CollectOffers(snapshot, WorldTravel::TripProviders());
    // A sick bot's trip (NR3, NR.4) starts no fight and keeps to the safe area it stands in: the tame and the stable's
    // swap are left out, and so is every stop its walk would leave that area for
    if (bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS))
    {
        Upkeep::SafeAreas const here = Planner::SafeAreasHere(bot, *indexes);
        WorldTravel::KeepSickErrands(
            snapshot.Stops, offers, [bot, &indexes, &here](WorldTravel::ErrandPlace const& place)
            { return Planner::SafeWalkTo(bot, *indexes, here, place.MapId, place.X, place.Y, place.Z); });
    }
    // NR9: no stop at a place the record of failed attempts refuses (a trip's third death there, a route none): the
    // next stop that serves is planned instead (the ordered stop below is not filtered)
    std::vector<WorldTravel::Refusal> placeRefused;
    FailedAttempts::Record const& failures = botAI->GetTravel().Failures();
    if (failures.count)
    {
        uint32 const now = getMSTime();
        placeRefused = WorldTravel::KeepUnrefusedErrands(
            snapshot.Stops, offers,
            [&failures, now](WorldTravel::ErrandPlace const& place)
            {
                return !FailedAttempts::IsPlaceRefused(
                            failures, WorldTravel::SightPoint{place.MapId, place.X, place.Y, place.Z}, now)
                            .empty();
            });
    }
    std::vector<WorldTravel::DroppedVendor> dropped;
    WorldTravel::TripPlan const plan = PlanErrandTrip(botAI, request, snapshot.Stops, offers, dropped);
    if (WorldLog::Active())
        for (WorldTravel::DroppedVendor const& vendor : dropped)
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "vendor_drop", vendor.Reason,
                               vendor.RouteSeconds, {{"straight_yd", WorldTravel::Yards(request.Start, vendor.Place)}},
                               vendor.Entry);

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
                case WorldTravel::ErrandKind::Auction:
                    stop.auction = true;
                    continue;
                case WorldTravel::ErrandKind::Craft:
                    stop.craft = true;
                    break;
                case WorldTravel::ErrandKind::TurnIn:
                    stop.turnIns.push_back({errand.Id, errand.Target, errand.TargetIsObject});
                    break;
                case WorldTravel::ErrandKind::Ride:
                    if (errand.Service == WorldTravel::SERVICE_MOUNT_VENDOR)
                        stop.mountRank = errand.Id;
                    break;
                case WorldTravel::ErrandKind::Tame:
                    stop.tameEntry = errand.Target;
                    break;
                case WorldTravel::ErrandKind::Stable:
                    stop.stableEntry = tripStop.Id;
                    stop.stableBeast = errand.Target;
                    stop.stableBeastAt = WorldPosition(snapshot.Stable.Place.MapId, snapshot.Stable.Place.X,
                                                       snapshot.Stable.Place.Y, snapshot.Stable.Place.Z);
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
        // The trainer part of the stop (class, weapon, profession or riding trainer)
        anyTrainer = TrainerPart(tripStop, stop) || anyTrainer;
        if (WorldLog::Active())
        {
            bool const joined =
                tripStop.OnRoute || otherWorth > WorldTravel::DetourCost(request.Rates, tripStop.DetourMinutes);
            for (WorldTravel::ErrandOffer const& errand : tripStop.Errands)
                if (errand.Kind == WorldTravel::ErrandKind::Mail ||
                    errand.Kind == WorldTravel::ErrandKind::BankWithdraw ||
                    errand.Kind == WorldTravel::ErrandKind::BankDeposit ||
                    errand.Kind == WorldTravel::ErrandKind::Auction)
                    bankMail.emplace_back(errand, joined ? "joined" : "detour");
        }
        tripMinutes += tripStop.DetourMinutes;
        errandCount += tripStop.Errands.size();
        stops.push_back(std::move(stop));
    }

    // An order is not refused by worth (planning 3.19 (7)): the vendor it names is on the trip
    anyVendor = OrderedStop(botAI, *indexes, order, stops) || anyVendor;
    // The world log's decision row: the sellable stacks and free slots, the trip's stops and errands it weighed
    WorldLogFormat::Alternatives weighed;
    if (WorldLog::Active())
        weighed = {{"sellable", static_cast<float>(sellableSlots)},
                   {"free_slots", static_cast<float>(snapshot.FreeSlots)},
                   {"stops", static_cast<float>(stops.size())},
                   {"errands", static_cast<float>(errandCount)}};
    if (stops.empty())
    {
        LOG_DEBUG("playerbots",
                  "[New RPG] {} errands: trip not worth it (nearest vendor {} at {:.0f} yd, {:.1f} walk min, {} "
                  "sellable, {} free slots, {} errands refused)",
                  bot->GetName(), nearest ? nearest->entry : 0, nearest ? nearestYards : 0.0f, walkMinutes,
                  sellableSlots, snapshot.FreeSlots, plan.Refused.size());
        // Without a vendor walkMinutes is 0
        // A trip that only refused places left without a stop says so (NR9), not not_worth
        char const* const skip = !nearest                                        ? "no_vendor"
                                 : !placeRefused.empty() && plan.Refused.empty() ? WorldTravel::PLACE_REFUSED
                                                                                 : "not_worth";
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, "skip", skip, walkMinutes * MINUTE, weighed);
        return false;
    }

    // The trip's row names the vendor, else the trainer, else its first errand (turn_in, mail, bank_withdraw)
    char const* tripWord = anyVendor ? "vendor" : "trainer";
    if (!anyVendor && !anyTrainer)
        tripWord = plan.Stops.front().Errands.empty()
                       ? "turn_in"
                       : WorldTravel::ErrandKindName(plan.Stops.front().Errands.front().Kind);
    RpgStatus::Start(botAI, RpgStatus::ErrandTrip(std::move(stops), bot->GetMoney()), order.word);
    // The trip keeps the list: a first stop in reach reads it (a walk forgets it)
    if (auto* errands = std::get_if<NewRpgInfo::Errands>(&botAI->rpgInfo.data))
        errands->needs = visit;
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
    char const* const reason = dueOnTrip(WorldTravel::ErrandKind::Repair)       ? "repair_due"
                               : dueOnTrip(WorldTravel::ErrandKind::Shop)       ? "stock_due"
                               : dueOnTrip(WorldTravel::ErrandKind::Train)      ? "train_due"
                               : dueOnTrip(WorldTravel::ErrandKind::Weapon)     ? "weapon_due"
                               : dueOnTrip(WorldTravel::ErrandKind::Profession) ? "profession_due"
                               : dueOnTrip(WorldTravel::ErrandKind::Ride)       ? "ride_due"
                               : dueOnTrip(WorldTravel::ErrandKind::Stable)     ? "pet_swap"
                               : dueOnTrip(WorldTravel::ErrandKind::Tame)       ? "pet_due"
                               : order.reason                                   ? order.reason
                                                                                : "worth_it";
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand, tripWord, reason, tripMinutes * MINUTE, weighed);
    // The bank and mail errands it takes, then the errands it leaves out: those at a place the record refuses
    // (place_refused), the detour rule's refusals (detour_not_worth, too_far, no_stop)
    if (WorldLog::Active())
        for (std::pair<WorldTravel::ErrandOffer, char const*> const& errand : bankMail)
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand,
                               WorldTravel::ErrandKindName(errand.first.Kind), errand.second, 0.0f,
                               {{"worth", errand.first.WorthCopper}});
    if (WorldLog::Active())
        for (WorldTravel::Refusal const& refusal : placeRefused)
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand,
                               WorldTravel::ErrandKindName(refusal.Offer.Kind), refusal.Reason, 0.0f,
                               {{"worth", refusal.Offer.WorthCopper}});
    if (WorldLog::Active())
        for (WorldTravel::Refusal const& refusal : plan.Refused)
            WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Errand,
                               WorldTravel::ErrandKindName(refusal.Offer.Kind), refusal.Reason,
                               refusal.WalkMinutes * MINUTE,
                               {{"worth", refusal.Offer.WorthCopper}, {"cost", refusal.CostCopper}});
    return true;
}

bool NewRpgStartErrandsAction::Execute(Event /*event*/) { return StartErrand(botAI, {}); }

bool NewRpgStartInnAction::Execute(Event /*event*/) { return StartBind(botAI, {}); }

bool NewRpgInternal::StartSickHearth(PlayerbotAI* botAI, char const* word)
{
    constexpr uint32 HEARTHSTONE_SPELL = 8690;  // the stone's spell (NewRpgBaseAction.cpp's HEARTHSTONE_CAST_SPELL)
    Player* bot = botAI->GetBot();
    if (!bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS) || !TestConstraints::HearthAllowed(botAI->testConstraints) ||
        !bot->HasItemCount(StartingHistory::ITEM_HEARTHSTONE, 1, false) || bot->HasSpellCooldown(HEARTHSTONE_SPELL) ||
        bot->IsInCombat() || bot->IsFlying() || bot->IsInFlight() || bot->IsNonMeleeSpellCast(false) ||
        bot->GetGroup() || bot->GetMap()->Instanceable() || !WorldTravel::SickErrandOpen(botAI))
        return false;
    // The core refuses a mounted cast (as NewRpgBaseAction::CastHearth)
    Unit::AuraEffectList const& mounts = bot->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    uint32 const mountSpell = mounts.empty() ? 0 : mounts.front()->GetId();
    if (bot->IsMounted())
        CheckMountStateAction::Dismount(bot, MountRules::How::Hearth);
    bool const cast = botAI->DoSpecificAction("hearthstone", Event(), true);
    // NR3c's trace (run 1: two paladins' casts ended interrupted:other within 0.1 s): the mount before, what the bot
    // casts after the use; one line per start
    Spell const* casting = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    LOG_DEBUG("playerbots", "[NR3] {} sick hearth: use {}, mount spell {}, mounted after {}, stand {}, casting {}",
              bot->GetName(), cast, mountSpell, bot->IsMounted(), static_cast<uint32>(bot->getStandState()),
              casting ? casting->GetSpellInfo()->Id : 0);
    if (!cast)
        return false;
    // The cast runs in a rest that ends at the bind (GoalApply::PlannedEnd, hearth_bind); the bot stands while it casts
    RpgStatus::Start(botAI, NewRpgInfo::Rest{}, word);
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Upkeep, "sick_hearth", "cast",
                       static_cast<float>(bot->GetExactDist(bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ)));
    return true;
}

bool NewRpgInternal::StartBind(PlayerbotAI* botAI, BindOrder const& order)
{
    Player* bot = botAI->GetBot();
    auto const probe = [botAI, bot](char const* word)  // a test bot's trace (WT63d)
    {
        if (TestProbe::On(botAI->testConstraints))
            LOG_INFO("playerbots", "[Probe] {} bind start {}", bot->GetName(), word);
    };
    // A sick bot's order passes once recover has handed it over (NR3: the sick gate chose an inn in its safe area)
    if (order.order ? (BankMail::VisitRuns(botAI->bankMail) || bot->IsInCombat() || !bot->IsAlive() ||
                       (bot->HasAura(Upkeep::SPELL_RESURRECTION_SICKNESS) && !WorldTravel::SickErrandOpen(botAI)) ||
                       bot->GetGroup() || bot->GetMap()->Instanceable())
                    : !WorldTravel::ErrandMayStart(botAI))
    {
        probe("refused");
        return false;
    }

    // The trigger checks at most every INN_RETRY_MS, whatever comes of it; an order leaves its time alone (a planner
    // bot's retry after a failed bind is the planner's)
    RpgMemos& memo = botAI->rpgMemos;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (!order.order)
        memo.lastInnMs = now;

    // The inns of the planner's index the bot can use on its map within the inn walk (straight line at run speed): the
    // order's, else the nearest whose zone fits the bot's level (the planner weighs the same list, PlannerService)
    std::shared_ptr<Planner::PlannerIndexes const> const indexes =
        Planner::IndexesReady() ? Planner::Indexes() : nullptr;
    float const runSpeed = bot->GetSpeed(MOVE_RUN);
    float const maxYards = WorldTravel::INN_MAX_WALK_SECONDS * std::max(runSpeed, 1.0f);
    Planner::Place const here{bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    std::vector<Planner::Goals::Inn> const inns =
        indexes
            ? Planner::Goals::InnInputs(Planner::InnsNear(bot, *indexes, here, maxYards),
                                        {bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ},
                                        FailedAttempts::LiveEntries(botAI->GetTravel().Failures(), now))
            : std::vector<Planner::Goals::Inn>{};
    int const pick = Planner::Goals::PickBindInn(inns, order.innkeeper, maxYards);
    // The world log's decision row: the three nearest other inns in walk seconds (inn_usual: the bind is not far from
    // it, no bind target; inn_unfit: not for the bot's level)
    WorldLogFormat::Alternatives others;
    if (WorldLog::Active())
        for (size_t i = 0; i < inns.size(); ++i)
            if (static_cast<int>(i) != pick)
            {
                char const* word = inns[i].usual ? "inn_usual" : (inns[i].fits ? "inn" : "inn_unfit");
                others.Offer({word, WorldLogFormat::WalkSeconds(inns[i].yards, runSpeed)}, true);
            }
    if (pick < 0)
    {
        probe("no_inn");
        WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "none", "no_fitting_inn", 0.0f, others);
        return false;
    }

    Planner::Goals::Inn const& inn = inns[static_cast<size_t>(pick)];
    RpgStatus::Start(botAI, RpgStatus::TripTo(WorldPosition(inn.place.map, inn.place.x, inn.place.y, inn.place.z)),
                     order.word);
    uint32 tripId = 0;
    if (auto* travel = botAI->GetTravel().Trip())
    {
        travel->bindInn = true;
        tripId = travel->tripId;
    }
    probe(order.innkeeper ? "ordered" : "picked");
    WorldLog::Decision(bot, WorldLogFormat::DecisionKind::Inn, "bind_far",
                       order.innkeeper ? "inn_ordered" : "inn_picked", WorldLogFormat::WalkSeconds(inn.yards, runSpeed),
                       others, tripId);
    return true;
}
