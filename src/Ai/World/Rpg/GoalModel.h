/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_GOALMODEL_H
#define _PLAYERBOT_GOALMODEL_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "AuctionBuyList.h"
#include "ErrandRules.h"
#include "ItemWorthRules.h"
#include "QuestCircuit.h"
#include "QuestOrderTypes.h"
#include "TraitRules.h"
#include "UpkeepNeedRules.h"
#include "WorldTravelRules.h"

// The planner's value model (travel plan WT14, 3.10): every goal a bot could follow now gets a value per minute,
// (XP + copper x need factor + item worth) / minutes, adjusted for risk and crowding and weighted by the bot's traits;
// the planner keeps the best few with a reason word each, and ReplanDecision says when to leave the current goal.
// Pure and Player-free: a function of the Snapshot and the Situation the caller fills (WT16 fills the live inputs).
// Values are in XP-equivalent units. The tuning constants are estimates [E] until the nights measure them.
namespace Planner
{
namespace Goals
{
constexpr std::size_t RANK_KEEP = 5;
constexpr float MIN_OPTION_MINUTES = 0.5f;
constexpr float MIN_FACTOR = 0.1f;  // no trait weight drops a value below this share
constexpr float NOTICEABLE_UNIT = static_cast<float>(TraitRules::NOTICEABLE) / TraitRules::MAX_VALUE;
constexpr float PERCENT_F = 100.0f;
constexpr float SECONDS_PER_MINUTE = 60.0f;

// A minute of the band's usual play in XP; with the band's earning rate e it gives a copper's neutral worth
constexpr std::array<float, WorldTravel::LEVEL_BAND_COUNT> BAND_XP_PER_MINUTE = {150.0f, 450.0f, 900.0f, 1400.0f,
                                                                                 2200.0f};

// The need factor: what a copper is worth in XP, the neutral exchange times a factor of gold against the reserve
constexpr float NEED_BROKE = 4.0f;       // no money at all
constexpr float NEED_AT_RESERVE = 1.0f;  // money equal to the reserve
constexpr float NEED_FLUSH = 0.1f;       // money at FLUSH_RATIO x the reserve or more
constexpr float FLUSH_RATIO = 2.0f;
constexpr float GREED_NEED_SPAN = 1.0f;      // a greedy bot keeps valuing money when flush
constexpr float THRIFT_RESERVE_SPAN = 0.5f;  // a thrifty bot keeps a larger reserve

// Trait weights: factor = 1 + span x trait (-1..+1)
constexpr float CAUTION_SPAN = 0.75f;           // risk weight and the circuit's caution
constexpr float THOROUGH_CIRCUIT_SPAN = 0.75f;  // the circuit's thoroughness
constexpr float THOROUGH_QUEST_SPAN = 0.2f;     // questing against grinding
constexpr float CURIOSITY_SPAN = 0.3f;          // unexplored places
constexpr float SOCIAL_SPAN = 0.15f;            // a busy hub: mild attraction or mild dislike
constexpr std::uint32_t HUB_CROWD_CAP = 20;     // bots at a hub beyond this change nothing more
constexpr float CROWD_PER_BOT = 0.05f;          // value lost per bot competing for the same kills or nodes
constexpr float COURTESY_CROWD_SPAN = 0.3f;     // a courteous bot leaves others' mobs alone: crowding costs more
constexpr float PATIENCE_CROWD_SPAN = 0.3f;     // a patient bot waits for contested mobs: crowding costs less
constexpr float GREED_GATHER_SPAN = 0.4f;
constexpr float DRIVE_SESSION_SPAN = 0.5f;  // how long a grinding session lasts

// Risk: raw risk x caution weight; value x 1 / (1 + weighted); refused above RISK_REFUSE
constexpr float RISK_PER_LEVEL_ABOVE = 0.15f;
constexpr float RISK_ELITE = 1.0f;         // per share of elite mobs, solo
constexpr float GROUP_ELITE_SHARE = 0.4f;  // the elite risk left in a group
constexpr float RISK_PER_DEATH = 1.0f;     // per expected death
constexpr float RISK_ENEMY = 0.3f;         // per share of enemy faction presence
constexpr float RISK_REFUSE = 1.0f;

constexpr float GRIND_MINUTES = 20.0f;
constexpr float MOVE_HORIZON_MINUTES = 60.0f;        // a hub's rate counts this long after the travel
constexpr float MOVE_UNKNOWN_SHARE = 0.8f;           // a move target without an estimate: this share of the band's rate
constexpr std::size_t NEXT_ZONE_HUBS = 4;            // hubs of other zones offered to an outlevelled bot, its map
constexpr std::size_t NEXT_ZONE_OTHER_MAP_HUBS = 2;  // and on other maps (with a travel cost only)
constexpr std::size_t NEXT_ZONE_HUB_TRIES = 2;       // hubs of one zone asked before the zone is left out
constexpr std::size_t NEXT_ZONE_ASKS = 8;            // ways the next-zone hubs ask in one plan (searches, cap order)
constexpr float TURN_IN_MINUTES = 0.5f;
constexpr float GATHER_MINUTES = 0.25f;
constexpr std::uint16_t SKILLUP_RANGE = 25;  // a node this close above its requirement still raises the skill
constexpr float SKILLUP_MINUTES = 1.0f;      // a skill point is worth about a minute of the band's play

constexpr float NEAR_VENDOR_MINUTES = 2.0f;
constexpr float VISIT_MINUTES = 1.0f;
constexpr std::uint32_t ORDER_PRESSURE_PERCENT = 15;  // an orderly bot sells this much earlier, an untidy one later
constexpr std::uint32_t PRESSURE_MIN_PERCENT = 50;
// A due errand's worth: the minutes a bot with broken or badly worn gear would lose, or one that waits for a stop
// upkeep makes worth a trip of its own (its rules are upkeep's: UpkeepNeeds::RepairErrandDue, OwnStop); the due
// option ranks first, whatever its worth
constexpr float DUE_ERRAND_MINUTES_SAVED = 10.0f;
constexpr std::uint32_t BAG_BUDGET_PERCENT = 50;

constexpr float REST_BASE = 0.5f;  // rests below this share of health or mana, caution raises it, drive lowers it
constexpr float REST_CAUTION_SPAN = 0.25f;
constexpr float REST_DRIVE_SPAN = 0.15f;
constexpr float REST_MIN = 0.2f;
constexpr float REST_MAX = 0.85f;
constexpr float DEATH_CHANCE_AT_ZERO = 0.5f;
constexpr float DEATH_MINUTES = 5.0f;  // a corpse run
constexpr float REST_MINUTES = 0.5f;
constexpr float MINUTE_MS = 60000.0f;
constexpr char const* SICK_REASON = "sick";  // a sick bot rests; the sick gate drops what it may not take (NR3)
// NR3 (NR.4): a sick bot outside every safe area with an errand wanted, its hearthstone ready and bound in a safe area:
// it uses the stone, then runs its errand at the inn (the errand option's place is the bind)
constexpr char const* SICK_HEARTH_REASON = "sick_hearth";
// NR14 (NR.14): an errand to a stop of its own (OwnStop), named by the stop it starts with
constexpr char const* TRAINING_REASON = "training";
constexpr char const* RIDING_REASON = "riding";
constexpr char const* PET_REASON = "pet";
constexpr char const* NOFIGHT_REASON = "nofight";  // a test's nofight (Situation::fightsHeld) leaves the tame out

inline bool OwnStopReason(std::string_view reason)
{
    return reason == TRAINING_REASON || reason == RIDING_REASON || reason == PET_REASON;
}
constexpr float HEARTH_CAST_MINUTES = 10.0f / 60.0f;  // the hearthstone's cast
// The questing test's setting (Playerbots.Test.QuestingOnly, questing WQ37): an option that does not serve levelling
constexpr char const* QUESTING_ONLY_REASON = "questing_only";

constexpr float HEARTH_USES = 1.0f;
constexpr float BIND_MINUTES = 0.5f;

constexpr std::uint32_t REPLAN_INTERVAL_MS = 3 * 60 * 1000;
constexpr float REPLAN_MARGIN = 0.25f;  // a new goal must beat the current one by this share
constexpr float PATIENCE_MARGIN_SPAN = 0.6f;
constexpr float DRIVE_MARGIN_SPAN = 0.3f;
constexpr float MARGIN_MIN = 0.1f;
constexpr float MARGIN_MAX = 0.6f;
constexpr float BREAK_OFF_MINUTES = 2.0f;         // what leaving a goal costs, at the current rate
constexpr float OPPORTUNITY_MARGIN_SHARE = 0.5f;  // a group doing the same quest halves the margin

// Stored in Planner::Goal::kind (WT16): append only
enum class GoalKind : std::uint8_t
{
    None,
    Quest,
    Grind,
    MoveZone,
    Money,
    Errand,
    BuyBag,
    Rest,
    Gather,
    Hearth,
    TravelElsewhere,
    Fish,  // professions WR16b: a fishing trip (GatherNode waters)
    Count
};
// One word per line (clang-format would pack them)
// clang-format off
constexpr std::array<char const*, static_cast<std::size_t>(GoalKind::Count)> KIND_WORDS = {
    "none",
    "quest",
    "grind",
    "move_zone",
    "money",
    "errand",
    "buy_bag",
    "rest",
    "gather",
    "hearth",
    "travel",
    "fish"};
// clang-format on

inline char const* KindWord(GoalKind kind)
{
    return kind < GoalKind::Count ? KIND_WORDS[static_cast<std::size_t>(kind)] : "none";
}

// What a target id names where the kind alone does not say it: a money goal's spot or node, a move's camp
enum class TargetType : std::uint8_t
{
    Default,  // the kind's own: a hub, spot, node, vendor, item or inn id
    Camp,     // a quest camp without a hub
    Spot,
    Node,
    Quest,  // a delivery: the carried quest
    Field,  // a circuit outside every hub and camp (no giver near its start): target 0 (planning WN4)
    Route,  // the quest route's orders on the bot's map (questing WQ18a): target 0
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(TargetType::Count)> TARGET_WORDS = {
    "", "camp", "spot", "node", "quest", "field", "route"};

// Short stops after which the current goal goes on
inline bool IsInterlude(GoalKind kind)
{
    return kind == GoalKind::Errand || kind == GoalKind::BuyBag || kind == GoalKind::Rest || kind == GoalKind::Hearth;
}

// A log word: [a-z0-9_:], anything else becomes '_'
inline std::string LogWord(std::string_view text)
{
    std::string word;
    word.reserve(text.size());
    for (char const c : text)
    {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':')
            word.push_back(c);
        else if (c >= 'A' && c <= 'Z')
            word.push_back(static_cast<char>(c - 'A' + 'a'));
        else
            word.push_back('_');
    }
    return word;
}

inline bool Finite(Place const& place)
{
    return std::isfinite(place.x) && std::isfinite(place.y) && std::isfinite(place.z);
}

inline float Factor(float unit, float span) { return std::max(MIN_FACTOR, 1.0f + span * unit); }

// The traits the model reads (-1..+1); a bot without traits is neutral (0, every factor 1)
struct Weights
{
    float drive{0.0f};
    float curiosity{0.0f};
    float caution{0.0f};
    float sociability{0.0f};
    float thrift{0.0f};
    float greed{0.0f};
    float orderliness{0.0f};
    float thoroughness{0.0f};
    float patience{0.0f};
    float courtesy{0.0f};
};

inline Weights FromTraits(Snapshot const& bot)
{
    Weights weights;
    if (!bot.traits)
        return weights;

    TraitRules::BotTraits const& traits = *bot.traits;
    using TraitRules::Trait;
    weights.drive = TraitRules::Unit(traits, Trait::Drive);
    weights.curiosity = TraitRules::Unit(traits, Trait::Curiosity);
    weights.caution = TraitRules::Unit(traits, Trait::Caution);
    weights.sociability = TraitRules::Unit(traits, Trait::Sociability);
    weights.thrift = TraitRules::Unit(traits, Trait::Thrift);
    weights.greed = -TraitRules::Unit(traits, Trait::Generosity);  // LC.47: greed is generosity's low pole
    weights.orderliness = TraitRules::Unit(traits, Trait::Orderliness);
    weights.thoroughness = weights.orderliness;  // LC.47: one trait, the orderly bot is the thorough one
    weights.patience = TraitRules::Unit(traits, Trait::Patience);
    weights.courtesy = TraitRules::Unit(traits, Trait::Courtesy);
    return weights;
}

inline float CautionWeight(Weights const& weights) { return Factor(weights.caution, CAUTION_SPAN); }

// The core's grey level (Acore::XP::GetGrayLevel): mobs at or below it give no XP
inline std::uint32_t GreyLevel(std::uint32_t level)
{
    if (level <= 5)
        return 0;
    if (level <= 39)
        return level - 5 - level / 10;
    if (level <= 59)
        return level - 1 - level / 5;
    return level - 9;
}

struct Need
{
    float exchange{0.0f};  // a copper's neutral worth in XP for the bot's band
    float factor{0.0f};    // the copper weight: exchange x the share for money against the reserve
    std::uint64_t baseReserve{0};
    std::uint64_t reserve{0};    // scaled by thrift
    bool shortOfReserve{false};  // money below the reserve
    float flush{0.0f};           // the share from FLUSH_RATIO x the reserve on (greed's)
};

// The share for money against the reserve (scaled; 0 a real zero, the flush share at any money): NEED_BROKE without
// money, NEED_AT_RESERVE at the reserve, the flush share from FLUSH_RATIO x the reserve on, linear between
inline float NeedShare(std::uint64_t money, std::uint64_t reserve, float flush)
{
    float share = flush;
    if (reserve)
    {
        float const ratio = static_cast<float>(static_cast<double>(money) / static_cast<double>(reserve));
        if (ratio < 1.0f)
            share = NEED_BROKE + (NEED_AT_RESERVE - NEED_BROKE) * ratio;
        else if (ratio < FLUSH_RATIO)
            share = NEED_AT_RESERVE + (flush - NEED_AT_RESERVE) * (ratio - 1.0f) / (FLUSH_RATIO - 1.0f);
    }
    return std::max(share, flush);
}

// reserveCopper is upkeep's reserve (NeedGoalBridge), 0 a real zero
inline Need NeedFor(Snapshot const& bot, Weights const& weights, std::uint64_t reserveCopper)
{
    Need need;
    std::uint32_t const band = WorldTravel::LevelBand(bot.level);
    need.exchange =
        BAND_XP_PER_MINUTE[band] / static_cast<float>(WorldTravel::DEFAULT_SLOT_BANDS[band].EarnCopperPerMinute);
    need.baseReserve = reserveCopper;
    need.reserve =
        static_cast<std::uint64_t>(static_cast<double>(need.baseReserve) * Factor(weights.thrift, THRIFT_RESERVE_SPAN));
    need.shortOfReserve = bot.money < need.reserve;
    need.flush = NEED_FLUSH * Factor(weights.greed, GREED_NEED_SPAN);
    need.factor = need.exchange * NeedShare(bot.money, need.reserve, need.flush);
    return need;
}

// The need at another purse (auction house AH9): the same reserve and traits, the factor for the money the bot holds
// now, so a purchase weighs the gold it has rather than the gold it had when its plan was made
inline Need NeedAt(Need need, std::uint64_t money)
{
    need.shortOfReserve = money < need.reserve;
    need.factor = need.exchange * NeedShare(money, need.reserve, need.flush);
    return need;
}

// Everything the model derives from the bot before it values options
struct Model
{
    Weights weights;
    Need need;
    std::uint32_t band{0};
    float cautionWeight{1.0f};
    float runSpeed{Circuit::DEFAULT_RUN_SPEED};
};

// The circuit's weights from the traits and the need factor; set them on the CircuitContext before PlanCircuit
inline CircuitWeights CircuitWeightsFor(Model const& model)
{
    CircuitWeights weights;
    weights.xp = 1.0f;
    weights.copper = model.need.factor;
    weights.thoroughness = Factor(model.weights.thoroughness, THOROUGH_CIRCUIT_SPAN);
    weights.caution = model.cautionWeight;
    return weights;
}

// The bot's hub as the circuit sees it
struct HereInfo
{
    // NO_HUB: PlanGoals finds it from the circuit's home camp (CircuitHub; the live bots)
    std::uint32_t hub{0};
    std::uint32_t zoneLow{0};  // the zone's level bracket, 0/0 = unknown
    std::uint32_t zoneHigh{0};
    std::uint32_t botsAtHub{0};
    std::uint32_t competingBots{0};  // at the circuit's kill spots
    float enemyShare{0.0f};
};

// Another hub the bot could go to; valuePerMinute is the expected circuit there (a PlanCircuit run or an estimate)
struct HubOption
{
    std::uint32_t hub{0};
    std::uint32_t zone{0};
    Place place;
    float travelMinutes{0.0f};
    float valuePerMinute{0.0f};
    bool explored{true};
    std::uint32_t botsAtHub{0};
    std::uint32_t competingBots{0};
    float levelsAbove{0.0f};  // its quests' mobs over the bot's level
    float eliteShare{0.0f};
    float enemyShare{0.0f};
};

struct GrindSpot
{
    std::uint32_t id{0};
    std::uint32_t zone{0};
    Place place;
    std::uint32_t minLevel{0};
    std::uint32_t maxLevel{0};
    float xpPerKill{0.0f};      // at the bot's level
    float copperPerKill{0.0f};  // coins and vendor loot
    float killsPerMinute{0.0f};
    float eliteShare{0.0f};
    float deathsPerHour{0.0f};
    float enemyShare{0.0f};
    float travelMinutes{0.0f};
    std::uint32_t competingBots{0};
    bool explored{true};
};

// One node, or a gather circuit (professions WR13b, GatherRules.h CircuitOption): its first stop is the target
struct GatherNode
{
    std::uint32_t id{0};
    std::uint16_t skill{0};  // herbalism, mining, skinning ...
    std::uint16_t required{0};
    Place place;
    float walkMinutes{0.0f};
    float gatherMinutes{GATHER_MINUTES};  // a circuit: its laps' minutes
    float copperWorth{0.0f};              // what its yield sells for
    float itemWorth{0.0f};                // copper the skill path pays for its yield beyond a vendor's price
    float skillPoints{1.0f};              // a circuit: the skill points expected; 0: none
    float levelsAbove{0.0f};              // the place's risk (RiskInput)
    float eliteShare{0.0f};
    std::uint32_t competingBots{0};
    bool explored{true};
    std::vector<std::uint32_t> stops;  // a circuit's spawn ids in walk order; empty: one node
    std::uint32_t herbStops{0};        // of those, the herb nodes (professions WPC13a: the circuit row's way)
    std::uint32_t laps{1};
    float pleasureMinutes{0.0f};  // a fishing trip (Fish::TripOption): the fishing interest's minutes of play
    float xpWorth{0.0f};          // a circuit of creature stops (WR20b): its kills' XP, none for grey
    // The plan's estimate of the trip or circuit (professions WR25a): expected takes (a node harvested, a creature
    // killed, a fish caught), the units those takes give and the fight minutes of the kills, all over the laps
    float takes{0.0f};
    float units{0.0f};
    float fightMinutes{0.0f};
    bool observed{false};  // the circuit's minutes per take are an observed estimate (WPC13b), not the static prior
};

struct Vendor
{
    std::uint32_t id{0};
    float walkMinutes{-1.0f};  // < 0: none known
    bool repairs{false};
    bool safe{
        false};  // NR3, a sick bot's only: the walk to it stays in a safe area the bot stands in (Upkeep::SafeLeg)
};

struct Bags
{
    bool ratesSet{false};  // else the band's defaults
    WorldTravel::SlotRates rates;
    std::uint32_t sellableSlots{0};
    float minutesToNextStop{30.0f};  // until the bot passes a vendor anyway
    Vendor vendor;
    Vendor repairer;  // the nearest that repairs (planning WT63c); none known: the vendor's own flag decides
};

// The skill path's craft queue at a station (professions WR14b): what the queue is worth and the walk to the nearest
// station of its first doable craft; a craft without a station brings none here (it joins a trip made anyway)
struct CraftQueue
{
    float worth{0.0f};         // value units (SkillPath::QueueWorth); 0: no queued craft the bags allow now
    float walkMinutes{-1.0f};  // to the station; < 0: none within the errand walk
    float minutes{0.0f};       // the visit's casts
    bool safe{false};          // NR3, a sick bot's only: the station lies in the bot's safe area (Vendor::safe)
};

// NR14 (NR.14): a stop upkeep's need rules make worth a trip of its own once the gold pays it (UpkeepNeedRules.h
// TrainingErrandDue, RidingErrandDue, PetErrandDue). NeedBridge sets due, target and place from the needs list; the
// walk and its safety come from the map thread's facts. The goal model keeps no threshold of its own
struct OwnStop
{
    bool due{false};
    std::uint32_t target{0};   // the need's trainer key, else its mount vendor; the pet's beast entry
    Place place;               // the beast's spawn; unset for a trainer or vendor
    float walkMinutes{-1.0f};  // < 0: none known
    bool safe{false};          // NR3, a sick bot's only: the walk stays in the bot's safe area (Vendor::safe)
};

// A trainer's or mount vendor's spawn within the errand walk that trades with the bot (the map thread's)
struct StopSeen
{
    std::uint32_t entry{0};  // the creature entry
    float walkMinutes{0.0f};
    bool safe{false};  // NR3, a sick bot's only (Vendor::safe)
};

struct BagOffer
{
    std::uint32_t item{0};
    std::uint32_t vendor{0};
    float walkMinutes{0.0f};
    std::uint32_t price{0};
    std::uint32_t slots{0};
    std::uint32_t replacedSlots{0};  // the smallest bag it replaces, 0 for an empty bag slot
};

struct Inn
{
    std::uint32_t id{0};
    Place place;
    float yards{0.0f};
    bool fits{false};      // its zone fits the bot's level
    bool usual{false};     // the bind is not far from it: a bind there saves nothing (InterludeInputs.h UsualInn)
    bool unstayed{false};  // the bot has not stayed near it for the dwell (InterludeInputs.h MarkUnstayed, NR.5)
    bool safe{false};      // NR3, a sick bot's only: the inn lies in the bot's safe area (Vendor::safe)
};

// A carried quest's turn-in at another hub or map (CircuitPlan::carried): its reward over the journey
struct Delivery
{
    std::uint32_t quest{0};
    Place place;  // the nearest ender
    float travelMinutes{0.0f};
    float xp{0.0f};
    float copper{0.0f};
    bool otherMap{false};
};

// A lone trip to the giver of a class quest with the unlock bonus (upkeep WU20a)
struct ClassTrip
{
    std::uint32_t quest{0};
    std::uint32_t camp{NO_CAMP};  // the giver's camp: the goal's target
    Place place;                  // the giver
    float travelMinutes{0.0f};
    float xp{0.0f};
    float copper{0.0f};
    float bonus{0.0f};  // XP
};

struct RoutePlan;

// The inputs the Snapshot lacks; the harness fills them, WT16 fills them live
struct Situation
{
    CircuitPlan const* circuit{nullptr};  // the questing circuit at the bot's hub (PlanCircuit)
    // The quest route (questing WQ18a, WQ.43 point 4). onCircuit: the bot is held on the circuit, so no route is
    // built or offered; the service clears it for the bots it puts on a route (WQ18b), until then every bot is held
    bool onCircuit{true};
    RoutePlan const* route{nullptr};         // PlanGoals builds it (its build function); a harness hands one in
    std::vector<std::uint64_t> closedCells;  // FieldMap.h's keys of the cells closed to the bot (a dry order)
    HereInfo here;
    std::vector<HubOption> hubs;
    std::vector<Delivery> deliveries;  // empty: PlanGoals fills them from the circuit's carried quests
    std::vector<GrindSpot> spots;
    std::vector<GatherNode> nodes;
    std::vector<GatherNode> waters;      // fishing trips (professions WR16b, Fish::TripOption): id the water's area
    std::uint32_t gatherRefusedItem{0};  // a wanted material only creatures the bot may not fight give (WR20c)
    float gatherRefusedLevels{0.0f};     // the least levels those creatures are above the bot
    std::uint32_t gatherBagsFull{0};     // the bag fill in percent that refused the gather circuit (WPC9); 0: none
    std::uint32_t gatherUnarmed{0};      // creature stops a bot without a weapon was not offered (WPC10)
    bool questsHeld{false};              // a test's noquest (TestConstraints): no quest goal is offered
    bool fightsHeld{false};              // a test's nofight (TestConstraints): no creature stop is offered
    // The questing test's setting (questing WQ37, WQ.50 item 5), copied from the config on the map thread: only the
    // options that serve levelling are kept (GoalBuilder::Add)
    bool questingOnly{false};
    bool trainingUnpayable{false};  // a due trainer spell the gold cannot pay (NeedBridge): money may be earned
    Bags bags;
    CraftQueue craft;
    OwnStop training;  // NR14: the class trainer
    OwnStop riding;    // the riding trainer, or the mount vendor for a known rank
    OwnStop pet;       // a hunter's beast to tame
    // The stops the own stops' walks come from (NeedBridge::PlaceOwnStops), nearest first: the class trainers of the
    // bot's class and the riding trainers, and the vendors that sell a mount for gold
    std::vector<StopSeen> trainersSeen;
    std::vector<StopSeen> mountVendorsSeen;
    // The map thread left the list out: the last plan found none of its stops due (Planner FillOwnStopsSeen)
    bool trainersSkipped{false};
    bool mountVendorsSkipped{false};
    std::vector<BagOffer> bagOffers;
    std::vector<Inn> inns;
    // NR3 (NR.4), a sick bot's only (Upkeep::SafeAreas on the map thread): it stands in a safe area; its bind lies in
    // one
    bool sickSafeHere{false};
    bool sickHearthSafe{false};
    float healthPercent{100.0f};
    float manaPercent{-1.0f};        // < 0: no mana
    std::uint64_t reserveCopper{0};  // upkeep's reserve (NeedGoalBridge, WU16), unscaled: the model scales it by thrift
    std::uint64_t shortfallCopper{0};  // upkeep's shortfall against its reserve; 0 = none
    std::string shortKind;             // the largest unpaid need's kind word (NeedShort's src)
    std::string shortText;             // "12g 0s 0c short for journeyman riding", for the goal view
    std::uint32_t bagBudgetPercent{BAG_BUDGET_PERCENT};
    // Class quests (upkeep WU20a, §3.11): the governed rule's inputs, from the map thread and the job
    bool governed{false};                                     // the bot keeps its possessions (GoalInterface.h)
    std::uint32_t levelXp{0};                                 // XP from the bot's level to the next
    std::vector<std::uint32_t> knownClassQuestSpells;         // sorted: the table's spells the bot knows
    std::vector<std::uint32_t> givenUpQuests;                 // sorted: QuestLog::Record::givenUp (WU21a, WQ34)
    std::vector<std::uint32_t> trainerTaught;                 // sorted: the table's spells the class trainer teaches
    std::vector<std::pair<std::uint32_t, float>> classBonus;  // PlanGoals fills: (quest, bonus XP), sorted
    std::vector<ClassTrip> classTrips;                        // PlanGoals fills: one per bonus quest not in the log
    std::vector<ClassQuests::Due> classFailed;  // PlanGoals fills: the failed doable quests to complete (WU21a)
    // The way back from a far bind by the travel cost (PlanGoals asks it): negative = no route; unset without a cost
    // (straight yards on the bot's map, no saving across maps)
    std::optional<float> bindSeconds;
    RouteGaps routeGaps;  // PlanGoals: the candidates its fills left out for want of a route
};

// What a bot sets out to do: the one value an option, a goal in a slot and a bridge order all carry
struct Goal
{
    GoalKind kind{GoalKind::None};
    std::uint32_t target{0};
    TargetType targetType{TargetType::Default};
    Place place;
    float minutes{0.0f};  // travel and action
    float valuePerMinute{0.0f};
    std::string reason;
    float takes{0.0f};  // a gather circuit's or fishing trip's estimate (GatherNode): the row WR25a writes at its start
    float units{0.0f};
    float fightMinutes{0.0f};
    bool observed{false};  // GatherNode::observed: the estimate's minutes from the observed rate
};

// A goal with the numbers that rank it
struct Option : Goal
{
    float xp{0.0f};
    float copper{0.0f};     // valued by the need factor
    float itemWorth{0.0f};  // copper, valued at the neutral exchange
    float expected{0.0f};   // already in value units: a circuit's value, saved time, avoided risk
    float risk{1.0f};       // multiplier
    float crowd{1.0f};      // multiplier
    float traitFactor{1.0f};
    float oneOff{0.0f};  // the part of the raw value a single turn-in pays (a delivery's reward), in value units
    float value{0.0f};
    float steadyPerMinute{0.0f};  // valuePerMinute without the one-off part: a rate the bot keeps up
    float bonus{0.0f};   // the class-quest unlock bonus in valuePerMinute (AddClassQuests); the stay test takes it off
    bool forced{false};  // ranks above everything and replaces any goal: a sick bot's rest (AddSickRest)
    // ranks right after forced and replaces any goal but itself: the errand for a due repair (NE.3) or a stop of its
    // own (NR14); AddErrand makes one errand option, so one option at most is due
    bool due{false};
    // The sick gate's facts (NR3, NR.4): the option starts no fight, and every stop of its route keeps to the safe area
    // (Upkeep::SafeLeg). Only the options that set both pass while the bot is sick; a healthy bot's ignore them
    bool peaceful{false};
    bool safeStops{false};
    std::vector<std::string> traitNotes;
};

// The reason of the option that stands for the circuit at the bot's own hub; a quest option with another reason
// names a delivery or a move to another hub or camp in the zone (GoalApply::RoleOf)
constexpr char const* CIRCUIT_REASON = "circuit";
constexpr char const* ROUTE_REASON = "route";  // the option of the quest route (its target type is its own)

// <kind>:<target>, the target with its type where the kind does not say it (money:spot_7, move_zone:camp_21)
inline std::string ChoiceWord(Goal const& option)
{
    std::string word = std::string(KindWord(option.kind)) + ":";
    if (option.targetType != TargetType::Default && option.targetType < TargetType::Count)
        word += std::string(TARGET_WORDS[static_cast<std::size_t>(option.targetType)]) + "_";
    return word + std::to_string(option.target);
}

inline bool SameTarget(Goal const& a, Goal const& b)
{
    return a.kind == b.kind && a.target == b.target && a.targetType == b.targetType;
}

struct Dropped
{
    GoalKind kind{GoalKind::None};
    std::uint32_t target{0};
    std::string reason;
};

struct Ranked
{
    bool ok{false};
    std::string failure;
    Need need;
    std::uint64_t shortfallCopper{0};  // the situation's, carried for NeedShort and the goal view
    std::string shortKind;
    std::string shortText;
    float referenceRate{0.0f};  // the best activity's value per minute
    std::uint32_t considered{0};
    std::vector<Option> options;  // the best RANK_KEEP, best first
    std::vector<Dropped> dropped;
    // A sick bot's plan whose best option passed the sick gate (NR3): the sickness rest yields to it
    // (GoalApply::OnFreshPlan)
    bool sickGoal{false};
};

struct RiskInput
{
    float levelsAbove{0.0f};
    float eliteShare{0.0f};
    float deaths{0.0f};
    float enemyShare{0.0f};
};

// The quest route beside the circuit (questing WQ18a): QuestRoute.h's plan as a plan of goals carries it, plain data
// (QuestRoute.h includes this header, so GoalRoute.h makes the one from the other). Its orders in walk order; none:
// no route, and the circuit plans as before
struct RoutePlan
{
    std::vector<QuestOrder> orders;
    float worth{0.0f};           // the stops' worth, in the circuit's value units
    float minutes{0.0f};         // the walks and the stays
    RiskInput risk;              // of the first stop's field
    std::uint32_t walkCalls{0};  // the route searches it asked
    std::uint32_t gaps{0};       // far fields the search found no way to
    std::uint32_t capped{0};     // needs whose far fields the job's searches left unpriced
};

// What builds the route for PlanGoals: GoalRoute.h's over the job's field map
using RouteBuild = std::function<RoutePlan(Snapshot const&, CircuitContext const&, Model const&, Situation const&)>;

inline bool Finite(RiskInput const& in)
{
    return std::isfinite(in.levelsAbove) && std::isfinite(in.eliteShare) && std::isfinite(in.deaths) &&
           std::isfinite(in.enemyShare);
}

inline float RawRisk(RiskInput const& in, std::uint8_t groupSize)
{
    float const elite = std::clamp(in.eliteShare, 0.0f, 1.0f) * RISK_ELITE * (groupSize > 1 ? GROUP_ELITE_SHARE : 1.0f);
    return std::max(in.levelsAbove, 0.0f) * RISK_PER_LEVEL_ABOVE + elite + std::max(in.deaths, 0.0f) * RISK_PER_DEATH +
           std::clamp(in.enemyShare, 0.0f, 1.0f) * RISK_ENEMY;
}

inline bool RankBefore(Option const& a, Option const& b)
{
    if (a.forced != b.forced)
        return a.forced;
    if (a.due != b.due)
        return a.due;
    if (a.valuePerMinute != b.valuePerMinute)
        return a.valuePerMinute > b.valuePerMinute;
    if (a.kind != b.kind)
        return a.kind < b.kind;
    if (a.target != b.target)
        return a.target < b.target;
    return a.targetType < b.targetType;
}

inline float RestThreshold(Weights const& weights)
{
    return std::clamp(REST_BASE + REST_CAUTION_SPAN * weights.caution - REST_DRIVE_SPAN * weights.drive, REST_MIN,
                      REST_MAX);
}

inline std::uint32_t ErrandPressurePercent(Weights const& weights)
{
    float const percent =
        static_cast<float>(WorldTravel::ERRAND_PRESSURE_PERCENT) - ORDER_PRESSURE_PERCENT * weights.orderliness;
    return static_cast<std::uint32_t>(
        std::clamp(percent, static_cast<float>(PRESSURE_MIN_PERCENT), static_cast<float>(WorldTravel::PERCENT)));
}

inline Model Prepare(Snapshot const& bot, Situation const& situation)
{
    Model model;
    model.weights = FromTraits(bot);
    model.need = NeedFor(bot, model.weights, situation.reserveCopper);
    model.band = WorldTravel::LevelBand(bot.level);
    model.cautionWeight = CautionWeight(model.weights);
    if (std::isfinite(bot.runSpeed) && bot.runSpeed > 0.0f)
        model.runSpeed = bot.runSpeed;
    return model;
}

class GoalBuilder
{
public:
    GoalBuilder(Snapshot const& bot, Situation const& situation)
        : _bot(bot), _situation(situation), _model(Prepare(bot, situation))
    {
    }

    Ranked Run()
    {
        _ranked.need = _model.need;
        _ranked.shortfallCopper = _situation.shortfallCopper;
        _ranked.shortKind = _situation.shortKind;
        _ranked.shortText = _situation.shortText;
        if (!Finite(_bot.position))
        {
            _ranked.failure = "bad position";
            return _ranked;
        }

        AddRoute();
        AddCircuit();
        AddHubs();
        AddDeliveries();
        AddSpots();
        AddNodes();
        AddWaters();
        _ranked.referenceRate = ReferenceRate();
        AddErrand();
        AddBagOffers();
        if (!Sick())
            AddRest();
        AddHearth();
        AddClassQuests();
        if (Sick())
            AddSickRest();
        return Finish();
    }

private:
    Weights const& W() const { return _model.weights; }

    // Resurrection sickness: no goal that fights or walks off is taken until it ends, the status update would end it
    bool Sick() const { return _bot.sickMs > 0; }

    bool Outlevelled() const { return _situation.here.zoneHigh && _bot.level > _situation.here.zoneHigh; }

    void Apply(Option& option, float factor, float unit, char const* high, char const* low) const
    {
        if (factor == 1.0f)
            return;
        option.traitFactor *= factor;
        if (std::abs(unit) >= NOTICEABLE_UNIT)
            option.traitNotes.emplace_back(unit > 0.0f ? high : low);
    }

    void ApplyCuriosity(Option& option, bool explored) const
    {
        if (!explored)
            Apply(option, Factor(W().curiosity, CURIOSITY_SPAN), W().curiosity, "curious", "incurious");
    }

    // Bots competing for the same kills or nodes lower the value (family rule 10); bots merely at the hub do not
    float CompetingCrowd(std::uint32_t competing) const
    {
        float const weight = Factor(W().courtesy, COURTESY_CROWD_SPAN) * Factor(-W().patience, PATIENCE_CROWD_SPAN);
        return 1.0f / (1.0f + static_cast<float>(competing) * CROWD_PER_BOT * weight);
    }

    void ApplyHubCrowd(Option& option, std::uint32_t botsAtHub) const
    {
        if (!botsAtHub)
            return;
        float const busy = static_cast<float>(std::min(botsAtHub, HUB_CROWD_CAP)) / static_cast<float>(HUB_CROWD_CAP);
        Apply(option, Factor(W().sociability * busy, SOCIAL_SPAN), W().sociability, "gregarious", "solitary");
    }

    void Drop(Option const& option, char const* reason)
    {
        _ranked.dropped.push_back({option.kind, option.target, reason});
    }

    // The questing test's keep list (questing WQ37, WQ.50 item 5): quests, a move to another zone or hub, rest, the
    // hearthstone, a bag, an errand for full bags, a repair or a stop of its own (training, riding, the pet: NR14);
    // money only while a due repair or training is beyond the gold. A grind, a gather, a fishing trip, a craft trip and
    // travel elsewhere are not kept. O(1)
    bool ServesLevelling(Option const& option) const
    {
        switch (option.kind)
        {
            case GoalKind::Quest:
            case GoalKind::MoveZone:
            case GoalKind::Rest:
            case GoalKind::Hearth:
            case GoalKind::BuyBag:
                return true;
            case GoalKind::Errand:
                return option.reason == "bags_full" || option.reason == "bags_full_vendor_near" ||
                       option.reason == "repair" || OwnStopReason(option.reason);
            case GoalKind::Money:
                return _situation.trainingUnpayable ||
                       (UpkeepNeeds::RepairDue(_bot.brokenItems, _bot.lowestDurabilityPercent) &&
                        _bot.repairCheapest > _bot.money);
            default:
                return false;
        }
    }

    // Values the option and keeps it, or drops it for bad input or too much risk
    void Add(Option option, RiskInput const& risk)
    {
        ++_ranked.considered;
        if (_situation.questsHeld && option.kind == GoalKind::Quest)
        {
            Drop(option, "noquest");
            return;
        }
        if (_situation.questingOnly && !ServesLevelling(option))
        {
            Drop(option, QUESTING_ONLY_REASON);
            return;
        }
        // The sick gate (NR3, NR.4): once recover has handed the bot over, an option passes when it starts no fight and
        // its stops keep to the safe area; every other option of a sick bot is dropped
        if (Sick() && !(_bot.sickErrandOpen && option.peaceful && option.safeStops))
        {
            Drop(option, SICK_REASON);
            return;
        }
        if (!std::isfinite(option.xp) || !std::isfinite(option.copper) || !std::isfinite(option.itemWorth) ||
            !std::isfinite(option.expected) || !std::isfinite(option.minutes) || !std::isfinite(option.crowd) ||
            !Finite(risk) || !Finite(option.place))
        {
            Drop(option, "bad_input");
            return;
        }

        float const weighted = RawRisk(risk, _bot.groupSize) * _model.cautionWeight;
        if (weighted > RISK_REFUSE)
        {
            Drop(option, _model.weights.caution >= NOTICEABLE_UNIT ? "too_risky:careful" : "too_risky");
            return;
        }
        option.risk = 1.0f / (1.0f + weighted);
        if (weighted > 0.0f && std::abs(W().caution) >= NOTICEABLE_UNIT)
            option.traitNotes.emplace_back(W().caution > 0.0f ? "careful" : "daring");

        float const raw = std::max(option.xp, 0.0f) + std::max(option.copper, 0.0f) * _model.need.factor +
                          std::max(option.itemWorth, 0.0f) * _model.need.exchange + std::max(option.expected, 0.0f);
        option.value = raw * option.risk * std::clamp(option.crowd, 0.0f, 1.0f) * option.traitFactor;
        option.valuePerMinute = option.value / std::max(option.minutes, MIN_OPTION_MINUTES);
        if (!std::isfinite(option.valuePerMinute) || option.value <= 0.0f)
        {
            Drop(option, option.value <= 0.0f ? "no_value" : "bad_input");
            return;
        }
        option.steadyPerMinute = option.valuePerMinute * (1.0f - std::clamp(option.oneOff / raw, 0.0f, 1.0f));
        _options.push_back(std::move(option));
    }

    // The money a spot or node earns drives it more than the rest while the bot is short of its reserve; a bot that
    // is not short keeps the option's kind (it still gathers to sell, weighed by the lowered need factor)
    GoalKind MoneyOr(GoalKind kind, Option const& option) const
    {
        float const money = option.copper * _model.need.factor;
        float const rest = option.xp + option.itemWorth * _model.need.exchange + option.expected;
        return _model.need.shortOfReserve && money > 0.0f && money >= rest ? GoalKind::Money : kind;
    }

    float BandRate() const { return BAND_XP_PER_MINUTE[_model.band]; }

    HubOption const* FindHub(std::uint32_t hub) const
    {
        for (HubOption const& option : _situation.hubs)
            if (option.hub == hub)
                return &option;
        return nullptr;
    }

    Delivery const* FindDelivery(std::uint32_t quest) const
    {
        for (Delivery const& delivery : _situation.deliveries)
            if (delivery.quest == quest)
                return &delivery;
        return nullptr;
    }

    // Another hub: its rate over the horizon after the travel
    Option HubTo(std::uint32_t hub, std::uint32_t zone, Place const& place, float travelMinutes, float rate) const
    {
        Option option;
        option.kind = zone && zone != _bot.zone ? GoalKind::MoveZone : GoalKind::Quest;
        option.target = hub;
        option.place = place;
        option.minutes = travelMinutes + MOVE_HORIZON_MINUTES;
        option.expected = rate * MOVE_HORIZON_MINUTES;
        return option;
    }

    void AddCircuit()
    {
        CircuitPlan const* plan = _situation.circuit;
        if (!plan || !plan->ok)
            return;

        HereInfo const& here = _situation.here;
        if (plan->value > 0.0f && plan->minutes > 0.0f)
        {
            if (_routed)
                return;  // the route owns the bot's map
            Option option;
            option.kind = GoalKind::Quest;
            option.target = here.hub;
            if (here.hub == NO_HUB && plan->homeCamp != NO_CAMP)
            {
                option.target = plan->homeCamp;  // a circuit outside every hub's core: its home camp names it
                option.targetType = TargetType::Camp;
            }
            else if (here.hub == NO_HUB)
            {
                // Neither (no giver near the circuit's start): the circuit runs from its own plan (its key is
                // CAMP_KEY | NO_CAMP), so it is offered, named field_0, never the no-hub marker (WN4)
                option.target = 0;
                option.targetType = TargetType::Field;
            }
            option.place = plan->start;
            option.minutes = plan->minutes;
            option.expected = plan->value;
            option.oneOff = plan->oneOffValue;
            option.reason = CIRCUIT_REASON;
            option.crowd = CompetingCrowd(here.competingBots);
            Apply(option, Factor(W().thoroughness, THOROUGH_QUEST_SPAN), W().thoroughness, "thorough", "hasty");
            ApplyHubCrowd(option, here.botsAtHub);
            Add(std::move(option), {0.0f, 0.0f, 0.0f, here.enemyShare});
            return;
        }

        // A dry circuit: its move becomes the way on, to the target camp's hub or, without one, to the camp itself
        CircuitMove const& move = plan->moveOn;
        if (move.reason == MoveReason::None || move.reason == MoveReason::Nowhere)
            return;
        bool const hasHub = move.hub != NO_HUB;
        HubOption const* known = hasHub ? FindHub(move.hub) : nullptr;
        float const rate = known ? known->valuePerMinute : BandRate() * MOVE_UNKNOWN_SHARE;
        Place const place = known ? known->place : move.place;
        Option option = HubTo(hasHub ? move.hub : move.camp, move.zone, place, move.seconds / SECONDS_PER_MINUTE, rate);
        if (!hasHub)
            option.targetType = TargetType::Camp;
        option.reason = Outlevelled() ? "outlevelled" : "circuit_dry";
        if (!hasHub && move.camp == NO_CAMP)
        {
            ++_ranked.considered;
            Drop(option, "no_target");
            return;
        }
        // A delivery move earns the carried quest's reward as well
        if (Delivery const* delivery = move.reason == MoveReason::Delivery ? FindDelivery(move.quest) : nullptr)
        {
            option.xp = delivery->xp;
            option.copper = delivery->copper;
            option.oneOff = delivery->xp + delivery->copper * _model.need.factor;
            if (!Outlevelled())
                option.reason = "delivery";
        }
        MarkOtherMap(option);
        if (known)
        {
            ApplyCuriosity(option, known->explored);
            ApplyHubCrowd(option, known->botsAtHub);
            option.crowd = CompetingCrowd(known->competingBots);
            Add(std::move(option), {known->levelsAbove, known->eliteShare, 0.0f, known->enemyShare});
        }
        else
            Add(std::move(option), {});
    }

    // A class quest a governed bot lacks (upkeep WU20a, §3.11): worth what the best other activity earns over its
    // minutes, plus its reward and unlock bonus, so the spell is worth the way. A circuit that takes the quest stands
    // for it, with the quests near it; otherwise a lone trip to the giver's camp. Sick bots drop both (Add)
    void AddClassQuests()
    {
        if (_situation.classBonus.empty())
            return;
        auto const bonusOf = [this](std::uint32_t quest)
        {
            for (auto const& [id, bonus] : _situation.classBonus)
                if (id == quest)
                    return bonus;
            return 0.0f;
        };
        CircuitPlan const* plan = _situation.circuit;
        auto const takes = [plan](std::uint32_t quest)
        {
            return plan && std::any_of(plan->take.begin(), plan->take.end(),
                                       [quest](CircuitTake const& take) { return take.quest == quest; });
        };
        Option* circuit = nullptr;
        float base = 0.0f;
        for (Option& option : _options)
        {
            if (option.kind == GoalKind::Quest && option.reason == CIRCUIT_REASON)
                circuit = &option;
            else if (!IsInterlude(option.kind))
                base = std::max(base, option.valuePerMinute);
        }
        if (base <= 0.0f)
            base = BandRate();
        float taken = 0.0f;
        if (plan)
            for (CircuitTake const& take : plan->take)
                if (take.classQuest)
                    taken += bonusOf(take.quest);
        if (circuit && (taken > 0.0f || (plan && plan->unlockValue > 0.0f)))
        {
            float const minutes = std::max(circuit->minutes, MIN_OPTION_MINUTES);
            float const floor = base + taken / minutes;
            float const unlock = plan ? plan->unlockValue : 0.0f;
            float const share =
                unlock * circuit->risk * std::clamp(circuit->crowd, 0.0f, 1.0f) * circuit->traitFactor / minutes;
            float const withoutRate = std::max(circuit->valuePerMinute - share, 0.0f);
            if (taken > 0.0f && circuit->valuePerMinute < floor)
            {
                circuit->valuePerMinute = floor;
                circuit->value = floor * minutes;
            }
            circuit->bonus = circuit->valuePerMinute - withoutRate;
        }
        std::vector<std::uint32_t> camps;
        for (ClassTrip const& trip : _situation.classTrips)
        {
            if (takes(trip.quest) || (plan && plan->ok && trip.camp == plan->homeCamp) ||
                std::find(camps.begin(), camps.end(), trip.camp) != camps.end())
                continue;
            camps.push_back(trip.camp);
            Option option;
            option.kind = GoalKind::Quest;
            option.target = trip.camp;
            option.targetType = TargetType::Camp;
            option.place = trip.place;
            option.minutes = std::max(trip.travelMinutes, MIN_OPTION_MINUTES);
            option.xp = trip.xp + trip.bonus;
            option.copper = trip.copper;
            option.expected = base * option.minutes;
            option.reason = ClassQuests::REASON_WORD;
            MarkOtherMap(option);
            std::size_t const before = _options.size();
            Add(std::move(option), {});
            if (_options.size() > before)
                _options.back().bonus =
                    _options.back().valuePerMinute;  // a trip is all bonus: no plan without it has one
        }
    }

    // A place on another map says so in the option's reason (planning WN1b; delivery_other_map is the delivery's)
    void MarkOtherMap(Option& option) const
    {
        if (option.place.map != _bot.position.map)
            option.reason += "_other_map";
    }

    void AddHubs()
    {
        for (HubOption const& hub : _situation.hubs)
        {
            if (hub.hub == _situation.here.hub && _situation.circuit)
                continue;  // the circuit stands for the bot's own hub
            Option option = HubTo(hub.hub, hub.zone, hub.place, hub.travelMinutes, hub.valuePerMinute);
            if (Outlevelled() && option.kind == GoalKind::MoveZone)
                option.reason = "outlevelled";
            else if (!hub.explored)
                option.reason = "unexplored";
            else
                option.reason = option.kind == GoalKind::MoveZone ? "next_zone" : "hub";
            MarkOtherMap(option);
            option.crowd = CompetingCrowd(hub.competingBots);
            Apply(option, Factor(W().thoroughness, THOROUGH_QUEST_SPAN), W().thoroughness, "thorough", "hasty");
            ApplyCuriosity(option, hub.explored);
            ApplyHubCrowd(option, hub.botsAtHub);
            Add(std::move(option), {hub.levelsAbove, hub.eliteShare, 0.0f, hub.enemyShare});
        }
    }

    // A carried quest turned in elsewhere: its reward over the journey, so a rich delivery close by goes first and a
    // poor one far away waits until the bot moves there anyway
    void AddDeliveries()
    {
        for (Delivery const& delivery : _situation.deliveries)
        {
            if (_routed && !delivery.otherMap)
                continue;  // the route hands in on the bot's map
            Option option;
            option.kind = GoalKind::Quest;
            option.target = delivery.quest;
            option.targetType = TargetType::Quest;
            option.place = delivery.place;
            option.minutes = delivery.travelMinutes + TURN_IN_MINUTES;
            option.xp = delivery.xp;
            option.copper = delivery.copper;
            option.reason = delivery.otherMap ? "delivery_other_map" : "delivery";
            Add(std::move(option), {});
        }
    }

    // The quest route as one option (questing WQ18a, WQ.43 point 4): its worth, its minutes and the risk of its
    // first stop; no crowd factor until batch F3. Kept, it owns the bot's map: the circuit's option and the
    // deliveries on that map are not offered beside it (AddCircuit, AddDeliveries). Dropped, without an order or
    // for a bot held on the circuit, they are offered as before
    void AddRoute()
    {
        RoutePlan const* route = _situation.onCircuit ? nullptr : _situation.route;
        if (!route || route->orders.empty())
            return;
        QuestOrder const& first = route->orders.front();
        Option option;
        option.kind = GoalKind::Quest;
        option.targetType = TargetType::Route;
        option.place = {first.mapId, first.x, first.y, _bot.position.z};
        option.minutes = route->minutes;
        option.expected = route->worth;
        option.reason = ROUTE_REASON;
        std::size_t const before = _options.size();
        Add(std::move(option), route->risk);
        _routed = _options.size() > before;
    }

    void AddSpots()
    {
        std::uint32_t const grey = GreyLevel(_bot.level);
        float const session = GRIND_MINUTES * Factor(W().drive, DRIVE_SESSION_SPAN);
        for (GrindSpot const& spot : _situation.spots)
        {
            bool const greyMobs = spot.maxLevel <= grey;
            float const kills = std::max(spot.killsPerMinute, 0.0f) * session;
            Option option;
            option.kind = GoalKind::Grind;
            option.target = spot.id;
            option.place = spot.place;
            option.minutes = spot.travelMinutes + session;
            option.xp = greyMobs ? 0.0f : kills * spot.xpPerKill;
            option.copper = kills * spot.copperPerKill;
            option.crowd = CompetingCrowd(spot.competingBots);
            option.kind = MoneyOr(GoalKind::Grind, option);
            if (option.kind == GoalKind::Money)
            {
                option.targetType = TargetType::Spot;
                option.reason = "gold_short";
            }
            else
                option.reason = greyMobs ? "grey_mobs" : "xp";
            ApplyCuriosity(option, spot.explored);
            float const midLevel = static_cast<float>(spot.minLevel + spot.maxLevel) / 2.0f;
            Add(std::move(option), {midLevel - static_cast<float>(_bot.level), spot.eliteShare,
                                    spot.deathsPerHour * session / WorldTravel::MINUTES_PER_HOUR, spot.enemyShare});
        }
    }

    Skill const* FindSkill(std::uint16_t id) const
    {
        for (Skill const& skill : _bot.skills)
            if (skill.id == id)
                return &skill;
        return nullptr;
    }

    void AddNodes()
    {
        for (GatherNode const& node : _situation.nodes)
        {
            Option option;
            option.kind = GoalKind::Gather;
            option.target = node.id;
            option.place = node.place;
            option.minutes = node.walkMinutes + node.gatherMinutes;
            // A circuit of kills without a skin asks no skill (skill 0)
            Skill const* skill = node.skill ? FindSkill(node.skill) : nullptr;
            if (node.skill && (!skill || skill->value < node.required))
            {
                ++_ranked.considered;
                Drop(option, "no_skill");
                continue;
            }
            option.copper = node.copperWorth;
            option.itemWorth = node.itemWorth;
            option.xp = node.xpWorth;
            option.takes = node.takes;
            option.units = node.units;
            option.fightMinutes = node.fightMinutes;
            option.observed = node.observed;
            // A circuit counts its skill points by the engine's colours; one node by the range above its requirement
            bool const skillUp = skill && skill->value < skill->max && node.skillPoints > 0.0f &&
                                 (!node.stops.empty() || skill->value < node.required + SKILLUP_RANGE);
            if (skillUp)
                option.expected = BandRate() * SKILLUP_MINUTES * node.skillPoints;
            option.crowd = CompetingCrowd(node.competingBots);
            option.kind = MoneyOr(GoalKind::Gather, option);
            if (option.kind == GoalKind::Money)
            {
                option.targetType = TargetType::Node;
                option.reason = "gold_short";
            }
            else
                option.reason = skillUp ? "skill_up" : "node";
            Apply(option, Factor(W().greed, GREED_GATHER_SPAN), W().greed, "greedy", "content");
            ApplyCuriosity(option, node.explored);
            Add(std::move(option), {node.levelsAbove, node.eliteShare, 0.0f, 0.0f});
        }
    }

    // A fishing trip (professions WR16b): the fish's worth, the casts' skill points and the interest's minutes of play.
    // Never a money goal: its executor is the trip, not a gather (the fill offers only waters that pay, FishRules.h)
    void AddWaters()
    {
        for (GatherNode const& water : _situation.waters)
        {
            Option option;
            option.kind = GoalKind::Fish;
            option.target = water.id;
            option.place = water.place;
            option.minutes = water.walkMinutes + water.gatherMinutes;
            option.takes = water.takes;
            option.units = water.units;
            option.copper = water.copperWorth;
            option.itemWorth = water.itemWorth;
            option.expected = BandRate() * (SKILLUP_MINUTES * water.skillPoints + water.pleasureMinutes);
            option.crowd = CompetingCrowd(water.competingBots);
            float const money = option.copper * _model.need.factor;
            float const goods = option.itemWorth * _model.need.exchange;
            if (water.skillPoints > 0.0f && BandRate() * SKILLUP_MINUTES * water.skillPoints >= std::max(money, goods))
                option.reason = "skill_up";
            else if (water.pleasureMinutes > 0.0f && BandRate() * water.pleasureMinutes >= std::max(money, goods))
                option.reason = "interest";
            else
                option.reason = goods > money ? "use_soon" : "income";
            Apply(option, Factor(W().greed, GREED_GATHER_SPAN), W().greed, "greedy", "content");
            ApplyCuriosity(option, water.explored);
            Add(std::move(option), {water.levelsAbove, water.eliteShare, 0.0f, 0.0f});
        }
    }

    // The best activity's rate: what a minute spent on a stop costs. A delivery is a one-off job of a few minutes,
    // no rate the bot keeps up, so it sets no bar, nor does its reward inside the circuit at hand or a move (WN1c: a
    // turn-in 70 yd away made the circuit's rate the bar, and the bind outranked that circuit)
    float ReferenceRate() const
    {
        float best = 0.0f;
        for (Option const& option : _options)
            if (!IsInterlude(option.kind) && option.targetType != TargetType::Quest)
                best = std::max(best, option.steadyPerMinute);
        return best > 0.0f ? best : BandRate();
    }

    WorldTravel::SlotRates Rates() const
    {
        if (_situation.bags.ratesSet)
            return _situation.bags.rates;
        return WorldTravel::CurrentRates(WorldTravel::SlotMeasure{}, WorldTravel::DEFAULT_SLOT_BANDS[_model.band]);
    }

    // Selling at a vendor (WT2's trip rule) and repairing at the nearest repairer (the vendor when none is known):
    // worth the saved slot time or the avoided breakage. A known vendor costs its walk and the visit, as a bag offer
    // does: the band's average trip time already holds a walk and is no price for this one
    void AddErrand()
    {
        Bags const& bags = _situation.bags;
        // A sick bot (NR3, NR.4) walks only to the vendors, the repairer and the station in the safe area it stands in
        // (their `safe`); outside every safe area its errand goes by the hearthstone (AddSickHearthErrand)
        bool const sick = Sick();
        auto const known = [sick](Vendor const& vendor)
        { return vendor.walkMinutes >= 0.0f && std::isfinite(vendor.walkMinutes) && (!sick || vendor.safe); };
        Vendor const& repairer = known(bags.repairer) ? bags.repairer : bags.vendor;
        bool const repairWanted = UpkeepNeeds::RepairErrandDue(_bot.brokenItems, _bot.lowestDurabilityPercent,
                                                               _bot.repairCheapest, _bot.money);
        bool const repair = known(repairer) && repairer.repairs && repairWanted &&
                            repairer.walkMinutes <= WorldTravel::ERRAND_MAX_WALK_MINUTES;
        // The craft queue counts only when the errand rules would take its station stop: its worth in copper above the
        // detour of a trip of its own (WorldTravel::CollectOffers' test; WR14b run 2: a chosen craft errand whose stop
        // the errand plan then refused as detour_not_worth)
        WorldTravel::SlotRates const rates = Rates();
        CraftQueue const& queue = _situation.craft;
        bool const craft = queue.worth > 0.0f && queue.walkMinutes >= 0.0f &&
                           queue.walkMinutes <= WorldTravel::ERRAND_MAX_WALK_MINUTES && _model.need.exchange > 0.0f &&
                           queue.worth / _model.need.exchange > WorldTravel::DetourCost(rates, queue.walkMinutes) &&
                           (!sick || queue.safe);
        std::uint32_t const usedPercent = _bot.bagSlots ? (_bot.bagSlots - std::min(_bot.bagFree, _bot.bagSlots)) *
                                                              WorldTravel::PERCENT / _bot.bagSlots
                                                        : 0;
        bool const bagsPressed = _bot.bagSlots && usedPercent >= ErrandPressurePercent(W());
        if (sick && !_situation.sickSafeHere)
        {
            AddSickHearthErrand(repairWanted, bagsPressed, rates);
            return;
        }
        // NR14 (NR.14): the stops of their own upkeep makes due, in the needs' fixed order (training, riding, the pet);
        // the first known one names the trip when nothing above does, the trip takes the others as any trip does. The
        // tame fights: a sick bot or one under a test's nofight is not offered it (NR.4)
        OwnStop const* const owns[] = {&_situation.training, &_situation.riding, &_situation.pet};
        char const* const ownReasons[] = {TRAINING_REASON, RIDING_REASON, PET_REASON};
        std::size_t first = std::size(owns);
        std::uint32_t ownDue = 0;
        for (std::size_t i = 0; i < std::size(owns); ++i)
        {
            OwnStop const& stop = *owns[i];
            if (!stop.due)
                continue;
            if (owns[i] == &_situation.pet && (sick || _situation.fightsHeld))
            {
                ++_ranked.considered;
                _ranked.dropped.push_back({GoalKind::Errand, stop.target, sick ? SICK_REASON : NOFIGHT_REASON});
                continue;
            }
            if (stop.walkMinutes < 0.0f || !std::isfinite(stop.walkMinutes) || (sick && !stop.safe))
                continue;
            ++ownDue;
            first = std::min(first, i);
        }
        if (!known(bags.vendor) && !repair && !craft && !ownDue)
            return;

        Option option;
        option.kind = GoalKind::Errand;
        option.target = bags.vendor.id;
        option.minutes = bags.vendor.walkMinutes + VISIT_MINUTES;
        // The trip starts no fight while the bot is sick (WorldTravel::KeepSickErrands leaves the tame out) and its
        // stops are the safe vendors above
        option.peaceful = true;
        option.safeStops = true;
        if (bagsPressed && known(bags.vendor))
        {
            WorldTravel::TripInput in;
            in.Rates = rates;
            in.FreeSlots = _bot.bagFree;
            in.SellableSlots = bags.sellableSlots;
            in.WalkMinutes = bags.vendor.walkMinutes;
            in.MinutesToNextStop = bags.minutesToNextStop;
            if (WorldTravel::TripWorthIt(in))
            {
                option.expected += BagsSaved(rates) * _model.need.exchange;
                option.reason = bags.vendor.walkMinutes <= NEAR_VENDOR_MINUTES ? "bags_full_vendor_near" : "bags_full";
                if (std::abs(W().orderliness) >= NOTICEABLE_UNIT)
                    option.traitNotes.emplace_back(W().orderliness > 0.0f ? "orderly" : "untidy");
            }
        }
        if (repair)
        {
            option.expected += DUE_ERRAND_MINUTES_SAVED * _ranked.referenceRate;
            option.due = true;
            if (option.reason.empty())
            {
                option.reason = "repair";
                option.target = repairer.id;
                option.minutes = repairer.walkMinutes + VISIT_MINUTES;
            }
        }
        // The craft queue (professions WR14b): its net worth; a trip of its own names no vendor, the errand entry plans
        // the station stop (the craft errand's own detour rule)
        if (craft)
        {
            option.expected += queue.worth;
            if (option.reason.empty())
            {
                option.reason = "craft";
                option.target = 0;
                option.minutes = queue.walkMinutes + VISIT_MINUTES + queue.minutes;
            }
        }
        // A stop of its own is due as a repair is: upkeep's rule said it is worth the trip, it ranks first
        if (ownDue)
        {
            option.expected += static_cast<float>(ownDue) * DUE_ERRAND_MINUTES_SAVED * _ranked.referenceRate;
            option.due = true;
            if (option.reason.empty())
            {
                OwnStop const& stop = *owns[first];
                option.reason = ownReasons[first];
                option.target = stop.target;
                option.place = stop.place;
                option.minutes = stop.walkMinutes + VISIT_MINUTES;
                option.peaceful = owns[first] != &_situation.pet;
            }
        }
        if (!option.reason.empty())
            Add(std::move(option), {});
    }

    // The worth of the bag slots a vendor stop frees until the bot passes a vendor anyway, in copper
    float BagsSaved(WorldTravel::SlotRates const& rates) const
    {
        Bags const& bags = _situation.bags;
        return WorldTravel::SlotsWorthPerHour(rates, _bot.bagFree, bags.sellableSlots) * bags.minutesToNextStop /
               WorldTravel::MINUTES_PER_HOUR;
    }

    // NR3 (NR.4): a sick bot outside every safe area takes the one safe way in, its hearthstone, when the stone is
    // ready and bound in a safe area and an errand is wanted (the repair rule, or bags at the pressure); its errand
    // then runs at the inn, planned there from the new place. No stone: no option, the bot rests at a clear point.
    // Never a walk to the nearest safe area
    void AddSickHearthErrand(bool repairWanted, bool bagsPressed, WorldTravel::SlotRates const& rates)
    {
        if (!_bot.hasHearthstone || _bot.hearthCooldownMs || !_situation.sickHearthSafe ||
            !(repairWanted || bagsPressed))
            return;
        Option option;
        option.kind = GoalKind::Errand;
        option.place = _bot.hearth;
        option.minutes = HEARTH_CAST_MINUTES + VISIT_MINUTES;
        option.expected =
            repairWanted ? DUE_ERRAND_MINUTES_SAVED * _ranked.referenceRate : BagsSaved(rates) * _model.need.exchange;
        option.reason = SICK_HEARTH_REASON;
        option.peaceful = true;
        option.safeStops = true;
        Add(std::move(option), {});
    }

    // A bag bought by WT2's rule: price within its worth to the bot (AH.5: the slots' time worth at its need factor, as
    // the auction's bag line) and the budget after the reserve; the price is paid either way
    void AddBagOffers()
    {
        if (_situation.bagOffers.empty())
            return;

        WorldTravel::SlotRates const rates = Rates();
        std::uint32_t const horizon = WorldTravel::DEFAULT_SLOT_BANDS[_model.band].BagHorizonHours;
        for (BagOffer const& offer : _situation.bagOffers)
        {
            Option option;
            option.kind = GoalKind::BuyBag;
            option.target = offer.item;
            option.minutes = offer.walkMinutes + VISIT_MINUTES;
            std::uint32_t const worth = AuctionRules::BagCeiling(
                WorldTravel::BagWorth(rates, _bot.bagFree, offer.slots, offer.replacedSlots, horizon), 0,
                {_model.need.exchange, _model.need.factor});
            WorldTravel::BagBuyInput in;
            in.Price = offer.price;
            in.Worth = worth;
            in.Gold = _bot.money;
            in.Reserve = _model.need.baseReserve;
            in.ThriftPercent = static_cast<std::uint32_t>(Factor(W().thrift, THRIFT_RESERVE_SPAN) * PERCENT_F);
            in.BudgetPercent = _situation.bagBudgetPercent;
            if (!WorldTravel::ShouldBuyBag(in))
            {
                ++_ranked.considered;
                Drop(option, worth < offer.price ? "bag_not_worth_price" : "bag_over_budget");
                continue;
            }
            // Buying now gains the slots until the next vendor stop, where the bot could buy it anyway
            float const saved = WorldTravel::SlotsWorthPerHour(rates, _bot.bagFree, offer.slots - offer.replacedSlots) *
                                _situation.bags.minutesToNextStop / WorldTravel::MINUTES_PER_HOUR;
            option.expected = saved * _model.need.exchange;
            option.reason = "bag_worth_price";
            Add(std::move(option), {});
        }
    }

    // Resting is worth the death it avoids: the chance grows as health or mana fall below the caution's threshold
    void AddRest()
    {
        float lowest = std::isfinite(_situation.healthPercent) ? _situation.healthPercent : PERCENT_F;
        char const* reason = "low_health";
        if (std::isfinite(_situation.manaPercent) && _situation.manaPercent >= 0.0f && _situation.manaPercent < lowest)
        {
            lowest = _situation.manaPercent;
            reason = "low_mana";
        }
        float const threshold = RestThreshold(W()) * PERCENT_F;
        if (lowest >= threshold)
            return;

        float const chance = (1.0f - std::max(lowest, 0.0f) / threshold) * DEATH_CHANCE_AT_ZERO * _model.cautionWeight;
        Option option;
        option.kind = GoalKind::Rest;
        option.place = _bot.position;
        option.minutes = REST_MINUTES;
        option.expected = chance * DEATH_MINUTES * _ranked.referenceRate;
        option.reason = reason;
        if (std::abs(W().caution) >= NOTICEABLE_UNIT)
            option.traitNotes.emplace_back(W().caution > 0.0f ? "careful" : "daring");
        Add(std::move(option), {});
    }

    // A sick bot rests at a clear point until the sickness ends (upkeep §3.5, NR3): the one recover walks it to
    // (Snapshot::sickRest), else where it stands. Added last: with no option the sick gate passed it is forced, so it
    // ranks above everything, a bridge goal too, its worth the time at the reference rate the sickness would cost in a
    // fight. With one (NR.4: an errand, the bind, the stone in) it is the plan's fallback and ranks below them
    void AddSickRest()
    {
        Option option;
        option.kind = GoalKind::Rest;
        option.place = _bot.sickRest.value_or(_bot.position);
        option.minutes = std::max(static_cast<float>(_bot.sickMs) / MINUTE_MS, REST_MINUTES);
        option.forced = _options.empty();
        option.expected = option.forced ? option.minutes * _ranked.referenceRate : 0.0f;
        option.value = option.expected;
        option.valuePerMinute = option.forced ? _ranked.referenceRate : 0.0f;
        option.reason = SICK_REASON;
        ++_ranked.considered;
        _options.push_back(std::move(option));
    }

    // Where the next planned goal lies: the route's first order, else where the circuit moves on
    std::optional<Place> NextPlannedPlace()
    {
        RoutePlan const* route = _situation.onCircuit ? nullptr : _situation.route;
        if (route && !route->orders.empty())
        {
            QuestOrder const& first = route->orders.front();
            return Place{first.mapId, first.x, first.y, 0.0f};
        }
        if (!_situation.circuit)
            return std::nullopt;
        CircuitMove const& move = _situation.circuit->moveOn;
        if (move.reason == MoveReason::None || move.reason == MoveReason::Nowhere)
            return std::nullopt;
        if (HubOption const* known = move.hub != NO_HUB ? FindHub(move.hub) : nullptr)
            return known->place;
        if (move.camp == NO_CAMP)
            return std::nullopt;
        return move.place;
    }

    // Binding at an inn when the bind is far (WT9's rule): worth the way back it saves on the next hearth. The inn
    // the bot is bound at saves nothing (WN1c: a bot 1010 yd from its bind was sent to bind there again)
    void AddHearth()
    {
        if (!_bot.hasHearthstone || _situation.inns.empty())
            return;

        bool const sameMap = _bot.hearth.map == _bot.position.map;
        float const bindYards = std::hypot(_bot.hearth.x - _bot.position.x, _bot.hearth.y - _bot.position.y);
        if (!std::isfinite(bindYards) || !WorldTravel::BindIsFar(sameMap, bindYards))
            return;

        float const maxYards = WorldTravel::INN_MAX_WALK_SECONDS * _model.runSpeed;
        std::optional<Place> const next = NextPlannedPlace();
        std::vector<WorldTravel::InnCandidate> candidates;
        candidates.reserve(_situation.inns.size());
        for (Inn const& inn : _situation.inns)
        {
            // NR.5: no bind at an inn the bot has neither stayed near nor goes to next
            bool const settles =
                !inn.unstayed ||
                (next && !WorldTravel::BindIsFar(next->map == inn.place.map,
                                                 std::hypot(next->x - inn.place.x, next->y - inn.place.y)));
            candidates.push_back(
                {inn.yards, settles && !inn.usual && inn.fits && std::isfinite(inn.yards) && inn.yards <= maxYards});
        }
        int const pick = WorldTravel::PickInn(candidates, std::numeric_limits<float>::max());
        if (pick < 0)
            return;

        // The way back from the bind is the route's; no route there, or no travel cost on another map: nothing saved
        float savedMinutes = 0.0f;
        if (_situation.bindSeconds)
            savedMinutes = *_situation.bindSeconds / SECONDS_PER_MINUTE;
        else if (sameMap)
            savedMinutes = WorldTravel::WalkMinutes(bindYards, _model.runSpeed);
        if (!std::isfinite(savedMinutes) || savedMinutes <= 0.0f)
            return;
        Inn const& inn = _situation.inns[static_cast<std::size_t>(pick)];
        Option option;
        option.kind = GoalKind::Hearth;
        option.target = inn.id;
        option.place = inn.place;
        option.minutes = WorldTravel::WalkMinutes(inn.yards, _model.runSpeed) + BIND_MINUTES;
        option.expected = savedMinutes * HEARTH_USES * _ranked.referenceRate;
        option.reason = "bind_far";
        // The walk to the inn and the bind start no fight; a sick bot takes it only for an inn in its safe area
        option.peaceful = true;
        option.safeStops = inn.safe;
        Add(std::move(option), {});
    }

    Ranked Finish()
    {
        std::sort(_options.begin(), _options.end(), RankBefore);
        for (Option& option : _options)
        {
            if (_ranked.options.size() >= RANK_KEEP)
                break;
            bool const seen = std::any_of(_ranked.options.begin(), _ranked.options.end(),
                                          [&option](Option const& kept) { return SameTarget(kept, option); });
            if (!seen)
                _ranked.options.push_back(std::move(option));
        }
        // While sick the only rest is the sickness rest (AddSickRest): any other best option passed the sick gate
        _ranked.sickGoal = Sick() && !_ranked.options.empty() && _ranked.options.front().kind != GoalKind::Rest;
        _ranked.ok = true;
        return std::move(_ranked);
    }

    Snapshot const& _bot;
    Situation const& _situation;
    Model const _model;
    Ranked _ranked;
    std::vector<Option> _options;
    bool _routed{false};  // the route's option is kept (AddRoute)
};

// The ranked options for the bot. Pure and deterministic: the same input gives the same list, in any input order
inline Ranked Rank(Snapshot const& bot, Situation const& situation) { return GoalBuilder(bot, situation).Run(); }

struct GoalPlan
{
    CircuitPlan circuit;
    Ranked ranked;
    std::uint32_t hub{NO_HUB};                                // the bot's hub as the plan saw it (Situation::here.hub)
    std::vector<std::pair<std::uint32_t, float>> classBonus;  // the class quests with the unlock bonus (WU20a)
    std::vector<ClassQuests::Due> classFailed;  // failed doable class quests: the map thread completes them (WU21a)
    RoutePlan route;  // the quest route (questing WQ18a); without an order the circuit is the plan
    // The gather circuit offered (professions WR13b): its stops in walk order, herb stops among them and laps
    std::vector<std::uint32_t> gatherStops;
    std::uint32_t gatherHerbStops{0};
    std::uint32_t gatherLaps{1};
    std::uint32_t gatherRefusedItem{0};  // Situation::gatherRefusedItem: the map thread's skip row (WR20c)
    float gatherRefusedLevels{0.0f};
    std::uint32_t gatherBagsFull{0};  // Situation::gatherBagsFull: the map thread's skip row (WPC9)
    std::uint32_t gatherUnarmed{0};   // Situation::gatherUnarmed: the map thread's skip row (WPC10)
    RouteGaps routeGaps;  // the candidates the plan left out for want of a route (the circuit's and the goals')
    // NR14: the own stops the plan found due, whose spawns the next snapshot seeks (Planner FillOwnStopsSeen): the
    // trainers for due class training or riding, the mount vendors for due riding. A wanted list its snapshot skipped
    // (ownStopsSkipped) left the due stop without a walk: the map thread asks for the next plan at once
    bool trainersWanted{false};
    bool mountVendorsWanted{false};
    bool ownStopsSkipped{false};
};

// The hub of a circuit: its home camp's (CampHub); NO_HUB for a camp outside every hub's core or without givers
inline std::uint32_t CircuitHub(CircuitPlan const& circuit, CircuitContext const& context)
{
    if (circuit.homeCamp == NO_CAMP || !context.quests || !context.indexes ||
        circuit.homeCamp >= context.quests->camps.size())
        return NO_HUB;
    std::optional<Nearby> const hub = CampHub(context.quests->camps[circuit.homeCamp], *context.indexes);
    return hub ? hub->index : NO_HUB;
}

// A carried quest the bot can turn in now: complete in its log, or one with no objective but the turn-in (a take
// carried to its ender). One with objectives left is no delivery yet: its ender is no goal until they are done
inline bool DeliverableNow(Snapshot const& bot, QuestRecord const& record)
{
    for (QuestSlot const& slot : bot.quests)
        if (slot.quest == record.id && (slot.state & Circuit::SLOT_COMPLETE))
            return true;
    return std::none_of(record.objectives.begin(), record.objectives.end(),
                        [](QuestObjective const& objective) { return objective.kind != ObjectiveKind::Turnin; });
}

// The carried quests' turn-ins that are due now (DeliverableNow): the nearest ender by the route's seconds, an ender
// without a route left out (counted in gaps); without a travel cost straight yards on the bot's map, none on another
inline std::vector<Delivery> Deliveries(Snapshot const& bot, CircuitContext const& context, CircuitPlan const& plan,
                                        float runSpeed, RouteGaps* gaps = nullptr)
{
    std::vector<Delivery> deliveries;
    if (!context.quests)
        return deliveries;
    QuestIndex const& quests = *context.quests;
    for (std::uint32_t const id : plan.carried)
    {
        QuestRecord const* record = quests.Find(id);
        if (!record || !DeliverableNow(bot, *record))
            continue;
        std::optional<Delivery> best;
        for (std::uint32_t const ender : record->enders)
        {
            if (ender >= quests.places.size())
                continue;
            QuestPlace const& at = quests.places[ender];
            Delivery delivery;
            delivery.quest = id;
            delivery.place = {at.mapId, at.x, at.y, at.z};
            delivery.otherMap = at.mapId != bot.position.map;
            float const seconds = context.travel ? context.travel(bot.position, delivery.place) : -1.0f;
            if (std::isfinite(seconds) && seconds >= 0.0f)
                delivery.travelMinutes = seconds / SECONDS_PER_MINUTE;
            else if (context.travel)
            {
                if (gaps)
                    gaps->Note(RouteCaller::Delivery, seconds);
                continue;  // no way there, or past the job's searches: never priced by yards (planning WN1a)
            }
            else if (!delivery.otherMap)
                delivery.travelMinutes =
                    WorldTravel::WalkMinutes(std::hypot(at.x - bot.position.x, at.y - bot.position.y), runSpeed);
            else
                continue;  // no price across maps without a route
            if (!best || delivery.travelMinutes < best->travelMinutes)
                best = delivery;
        }
        if (!best)
            continue;
        best->xp = static_cast<float>(record->xpBase);
        best->copper = static_cast<float>(std::max(record->copperReward, 0));
        deliveries.push_back(*best);
    }
    return deliveries;
}

// A camp gives at least the bracket move's few quests the bot may take now: the take test with the game's answer
// (Take::May, WQ41), as the circuit asks it. The game's answer also keeps the bot out of the enemy's towns in a
// contested zone (their quests' race masks)
inline bool OffersQuests(Snapshot const& bot, QuestIndex const& quests, QuestCamp const& camp)
{
    std::vector<std::uint32_t> inLog;
    for (QuestSlot const& slot : bot.quests)
        if (slot.quest)
            inLog.push_back(slot.quest);
    std::sort(inLog.begin(), inLog.end());
    Take::Facts const facts{.race = bot.race,
                            .level = bot.level,
                            .eliteGroup = bot.groupSize > 1,
                            .rewarded = bot.rewardedQuests,
                            .inLog = inLog,
                            .givenUp = {},
                            .takeable = bot.takeable};
    std::uint32_t offered = 0;
    for (std::uint32_t const id : camp.quests)
    {
        QuestRecord const* quest = quests.Find(id);
        if (!quest || Take::May(*quest, facts))
            continue;
        if (++offered >= Circuit::MOVE_MIN_QUESTS)
            return true;
    }
    return false;
}

// What the live caller leaves unset, from the immutable indexes on the worker: the zone's bracket and side, and for a
// bot past its zone's bracket the hubs of other zones it may go to (ZoneOffered), one per zone, on any map: on its own
// map the nearest first (NEXT_ZONE_HUBS), on the others the first ones with a route (NEXT_ZONE_OTHER_MAP_HUBS), each
// priced by its route. A hub without a route is left out (counted) and its zone's next hub tried, up to
// NEXT_ZONE_HUB_TRIES; NEXT_ZONE_ASKS bounds the ways asked. Without a travel cost straight yards on the bot's map
// only. A bot within its bracket moves on when its circuit runs dry (AddCircuit). The hubs' value is the dry move's
// estimate for an unknown hub: a PlanCircuit per hub would cost too much
inline void FillFromIndexes(Snapshot const& bot, CircuitContext const& context, Model const& model,
                            Situation& situation)
{
    if (!context.indexes)
        return;
    PlannerIndexes const& indexes = *context.indexes;
    HereInfo& here = situation.here;
    if (!here.zoneLow && !here.zoneHigh)
        if (ZoneRecord const* zone = indexes.FindZone(bot.zone))
        {
            here.zoneLow = zone->low;
            here.zoneHigh = zone->high;
        }
    if (!situation.hubs.empty() || !context.quests || !here.zoneHigh || bot.level <= here.zoneHigh)
        return;

    QuestIndex const& quests = *context.quests;
    Place const& at = bot.position;
    std::vector<std::uint32_t> zones;  // offered
    std::vector<std::uint32_t> tried;  // a zone once per hub of it asked without a route
    std::size_t asks = 0;
    std::size_t ownMap = 0;
    std::size_t otherMap = 0;
    // One camp's hub as an option; false once no more ways may be asked
    auto const offer = [&](std::uint32_t index)
    {
        QuestCamp const& camp = quests.camps[index];
        bool const away = camp.mapId != at.map;
        if ((away && !context.travel) || std::find(zones.begin(), zones.end(), camp.zoneId) != zones.end() ||
            static_cast<std::size_t>(std::count(tried.begin(), tried.end(), camp.zoneId)) >= NEXT_ZONE_HUB_TRIES)
            return true;
        ZoneRecord const* zone = indexes.FindZone(camp.zoneId);
        if (!ZoneOffered(zone, bot.level, bot.team) || !OffersQuests(bot, quests, camp))
            return true;  // the bot's own zone fails the bracket
        std::optional<Nearby> const hub = CampHub(camp, indexes);
        if (!hub || std::any_of(situation.hubs.begin(), situation.hubs.end(),
                                [&hub](HubOption const& listed) { return listed.hub == hub->index; }))
            return true;
        Hub const& record = indexes.hubs[hub->index];
        HubOption option;
        option.hub = hub->index;
        option.zone = camp.zoneId;
        option.place = {record.mapId, record.x, record.y, record.z};
        if (context.travel)
        {
            if (asks >= NEXT_ZONE_ASKS)
                return false;
            ++asks;
            float const seconds = context.travel(at, option.place);
            if (!std::isfinite(seconds) || seconds < 0.0f)
            {
                situation.routeGaps.Note(RouteCaller::NextZone, seconds);
                tried.push_back(camp.zoneId);
                return true;  // no way there
            }
            option.travelMinutes = seconds / SECONDS_PER_MINUTE;
        }
        else
            option.travelMinutes =
                WorldTravel::WalkMinutes(std::hypot(record.x - at.x, record.y - at.y), model.runSpeed);
        option.valuePerMinute = BAND_XP_PER_MINUTE[model.band] * MOVE_UNKNOWN_SHARE;
        if (context.crowding)
        {
            auto const crowd = context.crowding->byArea.find(AreaKey(record.mapId, record.areaId));
            if (crowd != context.crowding->byArea.end())
                option.botsAtHub = crowd->second.bots;
        }
        situation.hubs.push_back(option);
        zones.push_back(camp.zoneId);
        ++(away ? otherMap : ownMap);
        return true;
    };
    for (Nearby const& nearby : quests.NearestCamps(at.map, at.x, at.y, at.z, quests.camps.size()))
        if (ownMap >= NEXT_ZONE_HUBS || !offer(nearby.index))
            break;
    for (std::uint32_t index = 0; context.travel && index < quests.camps.size(); ++index)
    {
        if (otherMap >= NEXT_ZONE_OTHER_MAP_HUBS || asks >= NEXT_ZONE_ASKS)
            break;
        if (quests.camps[index].mapId != at.map && !offer(index))
            break;
    }
    situation.routeGaps.noNextZone = situation.hubs.empty();
}

// The way to a class quest's giver (upkeep WU39): Yes with the trip to the nearest indexed giver in a camp the travel
// cost reaches (straight walking on the bot's map without one), No when the quest has no such giver or every search
// found no way (TRAVEL_NO_ROUTE), Unknown when a search was cut short (TRAVEL_CAPPED) or, with no travel cost (the
// graph not ready), the giver is on another map: no verdict, so the quest is not failed for it
inline ClassQuests::Route ClassQuestRoute(Snapshot const& bot, CircuitContext const& context, Model const& model,
                                          std::uint32_t quest, RouteGaps& gaps, std::optional<ClassTrip>& best)
{
    best.reset();
    QuestRecord const* record = context.quests ? context.quests->Find(quest) : nullptr;
    if (!record)
        return ClassQuests::Route::No;
    bool unknown = false;
    for (std::uint32_t const index : record->givers)
    {
        QuestPlace const& giver = context.quests->places[index];
        if (giver.camp == NO_CAMP)
            continue;
        Place const place{giver.mapId, giver.x, giver.y, giver.z};
        float minutes = 0.0f;
        if (context.travel)
        {
            float const seconds = context.travel(bot.position, place);
            if (!std::isfinite(seconds) || seconds < 0.0f)
            {
                gaps.Note(RouteCaller::ClassQuest, seconds);
                unknown = unknown || !std::isfinite(seconds) || seconds == TRAVEL_CAPPED;
                continue;
            }
            minutes = seconds / SECONDS_PER_MINUTE;
        }
        else if (giver.mapId == bot.position.map)
            minutes = WorldTravel::WalkMinutes(std::hypot(giver.x - bot.position.x, giver.y - bot.position.y),
                                               model.runSpeed);
        else
        {
            unknown = true;
            continue;
        }
        if (!best || minutes < best->travelMinutes)
            best = ClassTrip{quest,
                             giver.camp,
                             place,
                             minutes,
                             static_cast<float>(record->xpBase),
                             static_cast<float>(std::max(record->copperReward, 0)),
                             0.0f};
    }
    if (best)
        return ClassQuests::Route::Yes;
    return unknown ? ClassQuests::Route::Unknown : ClassQuests::Route::No;
}

// The class quests of a governed bot (upkeep WU20a, §3.11): the unlock bonus on the circuit's values, the quests in
// its log protected from the circuit's abandon, and a lone trip per bonus quest to its nearest reachable giver
inline void FillClassQuests(Snapshot const& bot, CircuitContext& context, Model const& model, Situation& situation)
{
    situation.classBonus.clear();
    situation.classTrips.clear();
    situation.classFailed.clear();
    if (!situation.governed || !context.quests)
        return;
    QuestIndex const& quests = *context.quests;
    ClassQuests::BotFacts facts;
    facts.cls = bot.cls;
    facts.race = bot.race;
    facts.level = bot.level;
    facts.governed = true;
    facts.levelXp = situation.levelXp;
    facts.rewarded = bot.rewardedQuests;
    if (!std::is_sorted(facts.rewarded.begin(), facts.rewarded.end()))
        std::sort(facts.rewarded.begin(), facts.rewarded.end());
    facts.givenUp = situation.givenUpQuests;
    facts.knownSpells = situation.knownClassQuestSpells;
    std::vector<std::uint32_t> log;
    for (QuestSlot const& slot : bot.quests)
        log.push_back(slot.quest);
    std::sort(log.begin(), log.end());

    auto const teaches = [&situation](std::uint32_t spell)
    { return std::binary_search(situation.trainerTaught.begin(), situation.trainerTaught.end(), spell); };
    std::vector<ClassTrip> trips;
    std::vector<std::pair<std::uint32_t, ClassQuests::Route>> asked;  // one search per quest for both rules
    auto const route = [&](std::uint32_t quest)
    {
        for (auto const& [id, known] : asked)
            if (id == quest)
                return known;
        std::optional<ClassTrip> best;
        ClassQuests::Route const answer = ClassQuestRoute(bot, context, model, quest, situation.routeGaps, best);
        if (best && !std::binary_search(log.begin(), log.end(), quest))
            trips.push_back(*best);
        asked.emplace_back(quest, answer);
        return answer;
    };
    situation.classBonus = ClassQuests::Bonuses(quests.classRows, facts, teaches, route);
    // Failed (given up twice, or no way to a giver): the job only names them, the map thread completes them (WU21a)
    situation.classFailed = ClassQuests::DueQuests(quests.classRows, facts, ClassQuests::When::Check, teaches, route);
    for (ClassTrip& trip : trips)
        for (auto const& [quest, bonus] : situation.classBonus)
            if (quest == trip.quest)
                trip.bonus = bonus;
    std::sort(trips.begin(), trips.end(), [](ClassTrip const& a, ClassTrip const& b) { return a.quest < b.quest; });
    trips.erase(std::unique(trips.begin(), trips.end(),
                            [](ClassTrip const& a, ClassTrip const& b) { return a.quest == b.quest; }),
                trips.end());
    trips.erase(std::remove_if(trips.begin(), trips.end(), [](ClassTrip const& trip) { return trip.bonus <= 0.0f; }),
                trips.end());
    situation.classTrips = std::move(trips);
    context.unlockBonus = situation.classBonus;
    for (std::uint32_t const quest : ClassQuests::Protected(quests.classRows, log, facts, teaches))
        context.protectedQuests.push_back(quest);
}

// A bind far enough that binding here saves its way back (WT9's rule)
inline bool BindFar(Snapshot const& bot)
{
    float const yards = std::hypot(bot.hearth.x - bot.position.x, bot.hearth.y - bot.position.y);
    return std::isfinite(yards) && WorldTravel::BindIsFar(bot.hearth.map == bot.position.map, yards);
}

// The planner job: the circuit with the bot's own weights, then every goal beside it. The ways are asked near before
// far, so the job's search cap (PlannerRouteCost.h) cuts the farthest first: the class quests' givers, the circuit's
// moves, the turn-ins, the way back from the bind, then the next zones' hubs
inline GoalPlan PlanGoals(Snapshot const& bot, CircuitContext context, Situation situation,
                          RouteBuild const& buildRoute = {})
{
    GoalPlan plan;
    Model const model = Prepare(bot, situation);
    FillClassQuests(bot, context, model, situation);
    plan.classBonus = situation.classBonus;
    plan.classFailed = situation.classFailed;
    context.weights = CircuitWeightsFor(model);
    // The route before the circuit: its searches are the job's first after the class quests' (whose bonus prices
    // it); the circuit's moves, the deliveries, the bind and the hubs keep what the job's cap then leaves them
    if (!situation.onCircuit)
    {
        if (situation.route)
            plan.route = *situation.route;
        else if (buildRoute)
            plan.route = buildRoute(bot, context, model, situation);
    }
    situation.route = situation.onCircuit ? nullptr : &plan.route;
    context.givenUp = situation.givenUpQuests;
    plan.circuit = PlanCircuit(bot, context);
    if (situation.here.hub == NO_HUB)
        situation.here.hub = CircuitHub(plan.circuit, context);
    plan.hub = situation.here.hub;
    situation.circuit = &plan.circuit;
    if (situation.deliveries.empty())
        situation.deliveries = Deliveries(bot, context, plan.circuit, model.runSpeed, &situation.routeGaps);
    if (context.travel && !situation.bindSeconds && bot.hasHearthstone && !situation.inns.empty() && BindFar(bot))
    {
        float const seconds = context.travel(bot.hearth, bot.position);
        situation.bindSeconds = std::isfinite(seconds) && seconds >= 0.0f ? seconds : TRAVEL_NO_ROUTE;
        if (*situation.bindSeconds < 0.0f)
            situation.routeGaps.Note(RouteCaller::Bind, seconds);
    }
    FillFromIndexes(bot, context, model, situation);
    plan.routeGaps = plan.circuit.routeGaps;
    plan.routeGaps.Add(situation.routeGaps);
    plan.ranked = Rank(bot, situation);
    for (GatherNode const& node : situation.nodes)
        if (!node.stops.empty())
        {
            plan.gatherStops = node.stops;
            plan.gatherHerbStops = node.herbStops;
            plan.gatherLaps = std::max<std::uint32_t>(node.laps, 1);
            break;
        }
    plan.gatherRefusedItem = situation.gatherRefusedItem;
    plan.gatherRefusedLevels = situation.gatherRefusedLevels;
    plan.gatherBagsFull = situation.gatherBagsFull;
    plan.gatherUnarmed = situation.gatherUnarmed;
    plan.trainersWanted = situation.training.due || situation.riding.due;
    plan.mountVendorsWanted = situation.riding.due;
    plan.ownStopsSkipped = (plan.trainersWanted && situation.trainersSkipped) ||
                           (plan.mountVendorsWanted && situation.mountVendorsSkipped);
    return plan;
}

// Re-planning (3.10): when a goal ends, the situation changes, every few minutes, and on an opportunity
enum class ReplanVerdict : std::uint8_t
{
    Stay,
    Switch,  // leave the current goal
    PickUp   // a short stop (a passing giver, a vendor, a rest), then the current goal goes on
};

inline char const* ReplanWord(ReplanVerdict verdict)
{
    switch (verdict)
    {
        case ReplanVerdict::Switch:
            return "switch";
        case ReplanVerdict::PickUp:
            return "pick_up";
        default:
            return "stay";
    }
}

struct CurrentGoal
{
    GoalKind kind{GoalKind::None};
    std::uint32_t target{0};
    float valuePerMinute{0.0f};  // what is left of it, as last valued
    bool ended{false};
    bool travelling{false};  // on the way to it
    TargetType targetType{TargetType::Default};
};

inline bool IsCurrent(Option const& option, CurrentGoal const& current)
{
    return option.kind == current.kind && option.target == current.target && option.targetType == current.targetType;
}

struct PassingGiver
{
    std::uint32_t giver{0};
    std::uint32_t quest{0};
    float detourMinutes{0.0f};
    float value{0.0f};      // the quest's value (the circuit's XP + copper x need)
    bool wouldTake{false};  // the circuit's WouldTake holds for it
};

struct Delta
{
    bool situation{false};  // level, death, zone, health
    bool bagsFullNearVendor{false};
    bool group{false};  // a group doing the same quest
    std::optional<PassingGiver> giver;
};

struct ReplanInput
{
    CurrentGoal current;
    Ranked const* fresh{nullptr};
    Delta delta;
    std::uint32_t nowMs{0};
    std::uint32_t lastPlanMs{0};
    Weights weights;
};

struct Replan
{
    ReplanVerdict verdict{ReplanVerdict::Stay};
    std::string reason;
    std::optional<Option> chosen;
    std::optional<PassingGiver> giver;
};

// The share a new goal must beat the current one by: patience raises it, drive lowers it
inline float ReplanMargin(Weights const& weights)
{
    return std::clamp(
        REPLAN_MARGIN * Factor(weights.patience, PATIENCE_MARGIN_SPAN) * Factor(-weights.drive, DRIVE_MARGIN_SPAN),
        MARGIN_MIN, MARGIN_MAX);
}

// The plan as it stood without the class-quest bonus: each option's rate less its bonus, an option left with no rate
// gone (a lone class trip is all bonus), re-ranked
inline Ranked WithoutBonus(Ranked const& ranked)
{
    Ranked without = ranked;
    without.options.clear();
    for (Option option : ranked.options)
    {
        if (option.bonus > 0.0f)
        {
            option.valuePerMinute -= option.bonus;
            option.value = std::max(option.value - option.bonus * std::max(option.minutes, MIN_OPTION_MINUTES), 0.0f);
            option.bonus = 0.0f;
            if (option.valuePerMinute <= 0.0f)
                continue;
        }
        without.options.push_back(std::move(option));
    }
    std::sort(without.options.begin(), without.options.end(), RankBefore);
    return without;
}

inline Replan DecideReplan(ReplanInput const& in)
{
    Replan replan;
    if (!in.fresh || !in.fresh->ok)
    {
        replan.reason = "no_plan";
        return replan;
    }

    std::vector<Option> const& options = in.fresh->options;
    CurrentGoal const& current = in.current;
    if (current.kind == GoalKind::None || current.ended)
    {
        replan.reason = "goal_ended";
        if (!options.empty())
        {
            replan.verdict = ReplanVerdict::Switch;
            replan.chosen = options.front();
        }
        return replan;
    }

    char const* trigger = nullptr;
    if (in.delta.bagsFullNearVendor)
        trigger = "opportunity_bags_full";
    else if (in.delta.giver)
        trigger = "opportunity_giver";
    else if (in.delta.group)
        trigger = "opportunity_group";
    else if (in.delta.situation)
        trigger = "situation";
    else if (in.nowMs - in.lastPlanMs >= REPLAN_INTERVAL_MS)
        trigger = "interval";
    if (!trigger)
    {
        replan.reason = "no_trigger";
        return replan;
    }

    // The current goal's rate, and the best other goal and the best short stop by rank, each on its own: a goal that
    // fails the margin must not hide a stop worth more than the current goal
    float currentRate = std::isfinite(current.valuePerMinute) ? std::max(current.valuePerMinute, 0.0f) : 0.0f;
    bool currentForced = false;
    bool currentDue = false;
    std::size_t const none = options.size();
    std::size_t best = none;
    std::size_t goal = none;
    std::size_t stop = none;
    for (std::size_t i = 0; i < options.size(); ++i)
    {
        if (IsCurrent(options[i], current))
        {
            currentRate = options[i].valuePerMinute;
            currentForced = options[i].forced;
            currentDue = options[i].due;
            continue;
        }
        best = std::min(best, i);
        if (IsInterlude(options[i].kind))
            stop = std::min(stop, i);
        else
            goal = std::min(goal, i);
    }

    // A forced goal (a sick bot's rest) replaces any other
    if (best != none && options[best].forced)
    {
        replan.verdict = ReplanVerdict::Switch;
        replan.reason = options[best].reason;
        replan.chosen = options[best];
        return replan;
    }

    // A due errand replaces any goal but itself (it is the only due option, so a due current is it)
    if (best != none && options[best].due)
    {
        replan.verdict = ReplanVerdict::Switch;
        replan.reason = options[best].reason;
        replan.chosen = options[best];
        return replan;
    }

    // A forced goal (or a due repair already running) stays until it ends: it counts as the best activity's rate, so
    // only a stop worth more breaks in
    if (currentForced || currentDue)
        currentRate = std::max(currentRate, in.fresh->referenceRate);

    bool const stopWorth = stop != none && options[stop].valuePerMinute > currentRate;
    auto const pickUp = [&](Option const& option)
    {
        replan.verdict = ReplanVerdict::PickUp;
        replan.reason = option.kind == GoalKind::Errand && in.delta.bagsFullNearVendor ? "opportunity_bags_full"
                        : option.kind == GoalKind::Rest                                ? "situation"
                                                                                       : trigger;
        replan.chosen = option;
        return replan;
    };
    if (stopWorth && stop < goal)
        return pickUp(options[stop]);

    // Hysteresis: a clear margin plus the cost of breaking off, so near-equal goals never flap
    if (goal != none && !currentForced && !currentDue)
    {
        Option const& next = options[goal];
        float margin = ReplanMargin(in.weights);
        if (in.delta.group)
            margin *= OPPORTUNITY_MARGIN_SHARE;
        float const required = currentRate * (1.0f + margin) +
                               currentRate * BREAK_OFF_MINUTES / std::max(next.minutes, MIN_OPTION_MINUTES);
        if (next.valuePerMinute > required)
        {
            replan.verdict = ReplanVerdict::Switch;
            if (in.delta.group)
                replan.reason = "opportunity_group";
            else if (in.delta.giver || current.travelling)
                replan.reason = "opportunity_better";
            else
                replan.reason = trigger;
            replan.chosen = next;
            return replan;
        }
    }
    if (stopWorth)
        return pickUp(options[stop]);

    // A giver on the way: picked up when its quest is worth more than the detour costs at the current rate
    if (PassingGiver const* giver = in.delta.giver ? &*in.delta.giver : nullptr;
        giver && giver->wouldTake && std::isfinite(giver->value) && std::isfinite(giver->detourMinutes) &&
        giver->value > std::max(giver->detourMinutes, 0.0f) * currentRate)
    {
        replan.verdict = ReplanVerdict::PickUp;
        replan.reason = "opportunity_giver";
        replan.giver = *giver;
        return replan;
    }

    replan.reason = currentForced ? "outside_goal" : "current_best";
    return replan;
}

inline Replan ReplanDecision(ReplanInput const& in)
{
    // The class-quest bonus never makes the bot leave its goal: what the plan without it keeps, the bot keeps (WU20a)
    bool const running = in.current.kind != GoalKind::None && !in.current.ended;
    if (running && in.fresh && in.fresh->ok &&
        std::any_of(in.fresh->options.begin(), in.fresh->options.end(),
                    [](Option const& option) { return option.bonus > 0.0f; }))
    {
        Ranked const without = WithoutBonus(*in.fresh);
        ReplanInput stay = in;
        stay.fresh = &without;
        Replan plain = DecideReplan(stay);
        if (plain.verdict == ReplanVerdict::Stay)
            return plain;
    }
    return DecideReplan(in);
}
}  // namespace Goals
}  // namespace Planner

#endif
