/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEBOTTOPICS_H
#define PLAYERBOTS_BRIDGEBOTTOPICS_H

// WT18a: the planner topics of BridgeBotState.h as rows, Player-free. BridgeBotState.cpp gathers the inputs on the
// world thread (the published plan view, the bags, the taxi mask and bind point, one route search); these functions
// only format and bound them. tests/bridge-bot-topics-selftest.cpp checks the limits, the order and the word forms.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "BridgeAbilities.h"
#include "BridgeBotState.h"
#include "GoalInterface.h"
#include "RouteRules.h"

namespace PlayerbotBridge::Topics
{
namespace Goals = Planner::Goals;
namespace GoalApply = Planner::GoalApply;

constexpr float SECONDS_CAP = 1.0e9f;
constexpr std::uint32_t MS_PER_SECOND = 1000;

// The topic row's limit (BridgeAbilities.h)
constexpr std::size_t Limit(BotStateTopic topic)
{
    TopicRow const* row = FindTopic(topic);
    return row ? row->Limit : BotStateTopicMaxItems;
}

// The row kinds (BotStateItem::code) of each topic
enum class PlanRow : std::uint8_t
{
    Goal,
    Source,
    Reason,
    Stop
};
enum class QuestRow : std::uint8_t
{
    Take,
    Task,
    TurnIn,
    Carried,
    Deferred
};
enum class BagRow : std::uint8_t
{
    Free,
    Sell,
    Bag,
    MailWaiting,   // value = unopened mails
    MailSeen,      // value = copper worth the bot saw and left in the mail
    MailExpected,  // value = copper the bot expects
    Kept,          // name = the item, value = the level it is due at, extra = 1 in the bank, 0 in the bags
    BankFree       // value = free bank slots, extra = all of them
};
enum class PlaceRow : std::uint8_t
{
    Hearth,
    Flight,      // on the bot's map: extra = yards
    FlightAway,  // on another map: extra = its map id
};
enum class RouteRow : std::uint8_t
{
    Total,
    Leg,
    Failed
};

template <typename Row>
BotStateItem Item(std::string name, std::uint32_t value, std::uint32_t extra, Row row)
{
    return {std::move(name), value, extra, static_cast<std::uint8_t>(row)};
}

// Lower-case words of [a-z_] for the goal, reason and trait fields: digits, '.', '+' and '%' are dropped, so no trait
// or weight number reaches a model (C4); any other character separates words
inline std::string Words(std::string_view text)
{
    std::string words;
    bool gap = false;
    for (char const c : text)
    {
        char const lower = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if ((lower >= 'a' && lower <= 'z') || lower == '_')
        {
            if (gap && !words.empty())
                words.push_back(' ');
            gap = false;
            words.push_back(lower);
        }
        else if (!(lower >= '0' && lower <= '9') && lower != '.' && lower != '+' && lower != '%')
            gap = true;
    }
    return words;
}

// One word: the words joined by '_'
inline std::string Word(std::string_view text)
{
    std::string word = Words(text);
    std::replace(word.begin(), word.end(), ' ', '_');
    return word;
}

inline std::uint32_t RoundUp(float seconds)
{
    return std::isfinite(seconds) && seconds > 0.0f
               ? static_cast<std::uint32_t>(std::ceil(std::min(seconds, SECONDS_CAP)))
               : 0;
}

inline std::uint32_t Rounded(float value)
{
    return std::isfinite(value) && value > 0.0f ? static_cast<std::uint32_t>(std::lround(std::min(value, SECONDS_CAP)))
                                                : 0;
}

// What the plan, options and quest_plan topics read: the published view, only for a bot in the planner's scope
struct PlanInput
{
    bool inScope{false};  // no real master, persistent and Planner.Goals on: otherwise the planner sets no goal
    bool seen{false};     // the planner published the bot
    GoalApply::PlanView view;
    std::uint32_t nowMs{0};
    float x{0.0f};  // the bot, for the stops' yards (a circuit lies on its map)
    float y{0.0f};
    float z{0.0f};
    std::function<bool(std::uint32_t)> open;          // the quest is in the log and not complete
    std::function<std::string(std::uint32_t)> title;  // the quest's title
};

inline bool Planned(PlanInput const& in) { return in.inScope && in.seen && in.view.plan; }

inline bool IsCurrent(Goals::Option const& option, GoalApply::Active const& goal)
{
    return goal.Is() && option.kind == goal.kind && option.target == goal.target &&
           option.targetType == goal.targetType;
}

// Goal (kind word, seconds in, seconds planned: left = extra - value), source, reason, then the circuit's next stops
// with a task of an incomplete log quest (first such quest's title, its open tasks there, yards from the bot). Without
// a goal: "none" and why (not_planner, paused, planning, the last fallback word or idle)
inline void PlanItems(PlanInput const& in, std::vector<BotStateItem>& items)
{
    items.clear();
    GoalApply::Active const& goal = in.view.goal;
    if (!in.inScope || !in.seen || !goal.Is())
    {
        items.push_back(Item("none", 0, 0, PlanRow::Goal));
        char const* why = !in.inScope || !in.seen ? "not_planner"
                          : in.view.paused        ? "paused"
                          : in.view.jobOut        ? "planning"
                          : in.view.lastFallback != GoalApply::Fallback::None
                              ? GoalApply::FallbackWord(in.view.lastFallback)
                              : "idle";
        items.push_back(Item(why, 0, 0, PlanRow::Reason));
        return;
    }
    std::uint32_t const since = goal.startedMs ? (in.nowMs - goal.startedMs) / MS_PER_SECOND : 0;
    items.push_back(
        Item(Goals::KindWord(goal.kind), since, RoundUp(goal.minutes * Goals::SECONDS_PER_MINUTE), PlanRow::Goal));
    items.push_back(Item(GoalApply::SourceWord(goal.source), 0, 0, PlanRow::Source));
    items.push_back(Item(Words(goal.reason), 0, 0, PlanRow::Reason));
    if (goal.kind == Goals::GoalKind::Quest && in.view.plan && in.open)
        for (Planner::CircuitStop const& stop : in.view.plan->circuit.stops)
        {
            if (items.size() >= Limit(BotStateTopic::Plan))
                break;
            std::uint32_t openTasks = 0;
            std::uint32_t first = 0;
            for (Planner::CircuitTask const& task : stop.tasks)
                if (in.open(task.quest) && !openTasks++)
                    first = task.quest;
            if (!openTasks)
                continue;
            float const dx = stop.x - in.x;
            float const dy = stop.y - in.y;
            float const dz = stop.z - in.z;
            items.push_back(Item(in.title ? in.title(first) : std::string(), openTasks,
                                 Rounded(std::sqrt(dx * dx + dy * dy + dz * dz)), PlanRow::Stop));
        }
}

// The ranking's top options: kind, reason and trait words; value per minute (rounded), seconds, code 1 = the goal the
// bot pursues now. Empty out of scope or before a plan (the live ranking may be short)
inline void OptionsItems(PlanInput const& in, std::vector<BotStateItem>& items)
{
    items.clear();
    if (!Planned(in))
        return;
    for (Goals::Option const& option : in.view.plan->ranked.options)
    {
        if (items.size() >= Limit(BotStateTopic::Options))
            break;
        std::string text = std::string(Goals::KindWord(option.kind)) + " " + option.reason;
        for (std::string const& note : option.traitNotes)
            text += " " + note;
        items.push_back({Words(text), Rounded(option.valuePerMinute),
                         RoundUp(option.minutes * Goals::SECONDS_PER_MINUTE),
                         static_cast<std::uint8_t>(IsCurrent(option, in.view.goal) ? 1 : 0)});
    }
}

// The circuit's quests, one row each in walking order: taken at a giver, with tasks (extra = task points), turned in,
// carried to another hub, deferred; name = title, value = quest id. Empty out of scope or without a circuit
inline void QuestPlanItems(PlanInput const& in, std::vector<BotStateItem>& items)
{
    items.clear();
    if (!Planned(in) || !in.view.plan->circuit.ok)
        return;
    Planner::CircuitPlan const& circuit = in.view.plan->circuit;
    std::vector<std::uint32_t> listed;
    auto const add = [&](std::uint32_t quest, QuestRow row, std::uint32_t extra)
    {
        if (!quest || items.size() >= Limit(BotStateTopic::QuestPlan) ||
            std::find(listed.begin(), listed.end(), quest) != listed.end())
            return;
        listed.push_back(quest);
        items.push_back(Item(in.title ? in.title(quest) : std::string(), quest, extra, row));
    };
    for (Planner::CircuitTake const& take : circuit.take)
        add(take.quest, take.carried ? QuestRow::Carried : QuestRow::Take, 0);
    for (Planner::CircuitStop const& stop : circuit.stops)
        for (Planner::CircuitTask const& task : stop.tasks)
        {
            std::uint32_t points = 0;
            for (Planner::CircuitStop const& other : circuit.stops)
                points += static_cast<std::uint32_t>(std::count_if(other.tasks.begin(), other.tasks.end(),
                                                                   [&task](Planner::CircuitTask const& t)
                                                                   { return t.quest == task.quest; }));
            add(task.quest, QuestRow::Task, points);
        }
    for (Planner::CircuitTurnIn const& turnIn : circuit.turnIns)
        add(turnIn.quest, QuestRow::TurnIn, 0);
    for (std::uint32_t const quest : circuit.carried)
        add(quest, QuestRow::Carried, 0);
    for (std::uint32_t const quest : circuit.deferred)
        add(quest, QuestRow::Deferred, 0);
}

// One item the bank rule keeps for a later level, its name resolved by the gather
struct KeptLine
{
    std::string name;
    std::uint32_t dueLevel{0};
    bool inBank{false};
    bool due{false};  // due at the bot's level now
};

// The kept items shown at most (about four)
constexpr std::size_t KEPT_SHOWN = 4;

struct BagsInput
{
    std::uint32_t freeSlots{0};
    std::uint32_t slots{0};  // backpack and bags together
    PlanInput const* plan{nullptr};
    std::uint32_t mailWaiting{0};         // unopened mails (a count only; what is inside stays unknown, WB.4)
    std::uint32_t mailSeenCopper{0};      // worth the bot saw and left in the mail
    std::uint32_t mailExpectedCopper{0};  // gold the bot expects there
    bool keepsPossessions{false};         // the bank rule governs this bot
    std::vector<KeptLine> kept;           // bank items, then bag items, each by later worth
    std::uint32_t bankFree{0};
    std::uint32_t bankSlots{0};
};

// free_slots (value free, extra all), then the plan's first errand (the next sell stop) and bag purchase: reason word,
// seconds, extra = the option's target (vendor or item); then the waiting mail (any bot: mail_waiting, mail_seen,
// mail_expected, each when above 0) and, for a bot the bank rule governs, up to KEPT_SHOWN kept items (due first) and
// bank_free
inline void BagsItems(BagsInput const& in, std::vector<BotStateItem>& items)
{
    items.clear();
    items.push_back(Item("free_slots", in.freeSlots, in.slots, BagRow::Free));
    if (in.plan && Planned(*in.plan))
    {
        bool sell = false;
        bool bag = false;
        for (Goals::Option const& option : in.plan->view.plan->ranked.options)
        {
            bool& seen = option.kind == Goals::GoalKind::Errand ? sell : bag;
            if ((option.kind != Goals::GoalKind::Errand && option.kind != Goals::GoalKind::BuyBag) || seen)
                continue;
            seen = true;
            items.push_back(Item(Word(option.reason), RoundUp(option.minutes * Goals::SECONDS_PER_MINUTE),
                                 option.target, option.kind == Goals::GoalKind::Errand ? BagRow::Sell : BagRow::Bag));
        }
    }
    if (in.mailWaiting)
        items.push_back(Item("mail_waiting", in.mailWaiting, 0, BagRow::MailWaiting));
    if (in.mailSeenCopper)
        items.push_back(Item("mail_seen", in.mailSeenCopper, 0, BagRow::MailSeen));
    if (in.mailExpectedCopper)
        items.push_back(Item("mail_expected", in.mailExpectedCopper, 0, BagRow::MailExpected));
    if (in.keepsPossessions)
    {
        std::size_t shown = 0;
        for (bool const dueNow : {true, false})
            for (KeptLine const& line : in.kept)
                if (line.due == dueNow && shown < KEPT_SHOWN)
                {
                    items.push_back(Item(line.name, line.dueLevel, line.inBank ? 1 : 0, BagRow::Kept));
                    ++shown;
                }
        items.push_back(Item("bank_free", in.bankFree, in.bankSlots, BagRow::BankFree));
    }
    if (items.size() > Limit(BotStateTopic::Bags))
        items.resize(Limit(BotStateTopic::Bags));
}

struct TaxiPlace
{
    std::uint32_t node{0};
    std::string name;
    TravelRoute::Point at;
};

struct PlacesInput
{
    TravelRoute::Point bot;
    bool hasHearthstone{false};
    std::uint32_t hearthCooldownMs{0};
    std::string hearthArea;
    std::vector<TaxiPlace> known;  // the flight points the bot knows
};

// The hearth (bind area, seconds of cooldown left, extra 1 = carries a hearthstone), then the known flight points:
// those on the bot's map nearest first (extra = yards), then the others by node (extra = map id); value = taxi node
inline void PlacesItems(PlacesInput const& in, std::vector<BotStateItem>& items)
{
    items.clear();
    items.push_back(Item(in.hearthArea, (in.hearthCooldownMs + MS_PER_SECOND - 1) / MS_PER_SECOND,
                         in.hasHearthstone ? 1 : 0, PlaceRow::Hearth));
    std::vector<std::pair<float, TaxiPlace const*>> order;
    order.reserve(in.known.size());
    for (TaxiPlace const& place : in.known)
        order.push_back(
            {place.at.mapId == in.bot.mapId ? TravelRoute::Distance(place.at, in.bot) : TravelRoute::UNREACHED,
             &place});
    std::stable_sort(order.begin(), order.end(), [](auto const& a, auto const& b)
                     { return a.first != b.first ? a.first < b.first : a.second->node < b.second->node; });
    for (auto const& [yards, place] : order)
    {
        if (items.size() >= Limit(BotStateTopic::Places))
            break;
        bool const here = place->at.mapId == in.bot.mapId;
        items.push_back(Item(place->name, place->node, here ? Rounded(yards) : place->at.mapId,
                             here ? PlaceRow::Flight : PlaceRow::FlightAway));
    }
}

struct RouteInput
{
    bool known{false};  // the argument named a place or gave "x,y,z,map"
    std::string place;  // the place's word ("position" for a literal one)
    TravelRoute::Result route;
};

// total (place word, seconds, extra = fare copper), then the legs (leg word, seconds, fare); failed: unknown_place or
// the search's failure as one word (graph_not_ready, no_route, ...)
inline void RouteItems(RouteInput const& in, std::vector<BotStateItem>& items)
{
    items.clear();
    if (!in.known)
    {
        items.push_back(Item("unknown_place", 0, 0, RouteRow::Failed));
        return;
    }
    if (!in.route.ok)
    {
        items.push_back(Item(Word(TravelRoute::FailureWord(in.route.failure)), 0, 0, RouteRow::Failed));
        return;
    }
    items.push_back(
        Item(Word(in.place), RoundUp(in.route.totalSeconds), TravelRoute::RouteFare(in.route), RouteRow::Total));
    for (TravelRoute::Leg const& leg : in.route.legs)
    {
        if (items.size() >= Limit(BotStateTopic::Route))
            break;
        items.push_back(Item(TravelRoute::LegTypeWord(leg.type), RoundUp(leg.seconds), leg.fareCopper, RouteRow::Leg));
    }
}
}  // namespace PlayerbotBridge::Topics

#endif
