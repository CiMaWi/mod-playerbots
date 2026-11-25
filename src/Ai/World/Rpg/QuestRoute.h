/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTROUTE_H
#define _PLAYERBOT_QUESTROUTE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "FieldMap.h"
#include "PlannerSnapshot.h"
#include "QuestCircuit.h"
#include "QuestFields.h"
#include "QuestIndex.h"
#include "QuestOrder.h"

// The quest route (questing plan WQ17c, PLAN 3.10 part 4, WQ.40 to WQ.43): from the log's needs and the takes
// (QuestFields.h) and the field map, a short list of orders for the bot's next half hour with its worth and minutes.
// Each stop is the field with the most worth per minute of walk and stay from where the bot then is, with the log as
// it then will be: a stop's expected credits are taken off the needs, a quest whose credits are then all expected
// gets its hand-in need, a quest a stop takes its objective needs. Near stops: straight yards at run speed within the
// ask's radius of the stop before. WQ17d: with a walk function, far stops too: per need no near stop serves, its fields
// beyond the radius on the bot's map, priced by that function within a budget of calls over the whole route; a field
// it finds no way to is a gap of the route. Pure rules for a planner worker: no game header, no config, no state (the
// walk function is injected). tests/quest-route-selftest.cpp proves them on generated logs
namespace Planner
{
namespace Route
{
static_assert(ORDER_HAND_IN == QuestFields::HAND_IN && ORDER_TAKE == QuestFields::TAKE);

// Tuning values
constexpr float HORIZON_MINUTES = 30.0f;         // a stop is added while the route before it is shorter
constexpr std::size_t MAX_STOPS = 5;             // a handful
constexpr float ALLOWANCE_YARDS = 184.0f;        // the median of the user's detours (WQ.37's research)
constexpr float STAY_MINUTES = 5.0f;             // a field's planned stay (PLAN 3.10 part 1)
constexpr float NPC_STAY_FLOOR_MINUTES = 0.25f;  // the shortest stay at hand-ins and takes
constexpr std::uint32_t WALK_BUDGET = 24;  // walk calls per route: PlannerRouteCost.h's ROUTE_SEARCHES_PER_JOB, copied
constexpr float FAR_RADIUS = 1.0e6f;       // far candidates: TopFields over the whole map

// The minutes of walk from a point to another on the bot's map, or none: no way (the planner's route search)
using Walk = std::function<std::optional<float>(float fromX, float fromY, float toX, float toY)>;

// What ends a route besides the horizon and the stop limit: a stop worth less per minute than the route (WQ17c), or
// nothing (StopRule::HorizonOnly, the replay's alternative of WQ17d)
enum class StopRule : std::uint8_t
{
    RateHolds,
    HorizonOnly
};

// What a route is asked for: the bot, the route's tuning values, and what the plan needs to see the log as it will be
struct Ask
{
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
    std::uint8_t level{1};
    std::uint8_t team{0};
    float runSpeed{0.0f};  // yards per second; 0: Circuit::DEFAULT_RUN_SPEED
    float caution{1.0f};   // the planner's caution weight; 1 is neutral
    std::uint8_t groupSize{0};
    float stayMinutes{STAY_MINUTES};
    float radius{QuestFields::FIELD_WALK_YARDS};  // a stop lies within this many straight yards of the one before
    float horizonMinutes{HORIZON_MINUTES};
    std::size_t maxStops{MAX_STOPS};
    float allowanceYards{ALLOWANCE_YARDS};
    std::size_t freeSlots{QUEST_LOG_SLOTS};  // the log's free slots: a route takes no more quests than fit
    std::span<std::uint64_t const> closed;   // cells closed to the bot, sorted (FieldMap.h's keys)
    QuestFields::Pricing pricing{};
    Walk walk{};                            // prices far stops; none: near stops only
    std::uint32_t walkBudget{WALK_BUDGET};  // walk calls over the whole route
    StopRule stopRule{StopRule::RateHolds};
};

// The log as the plan expects it to be: the needs (those a stop opens are appended) and the free slots
struct Log
{
    std::vector<QuestFields::Need> needs;
    std::size_t freeSlots{0};
};

struct Stop
{
    QuestOrder order;
    float worth{0.0f};  // the expected credits' worth, scaled by the planner's risk term as TopFields ranks it
};

// A far field the walk function found no way to (the circuit's no_route): its cell and the needs it would serve
struct FarGap
{
    std::uint64_t cell{0};
    std::vector<QuestOrderNeed> needs;
};

struct Plan
{
    std::vector<Stop> stops;
    float worth{0.0f};           // the sum of the stops' worth
    float minutes{0.0f};         // the sum of the walks and the stays
    std::vector<FarGap> gaps;    // each refused far field once
    std::uint32_t walkCalls{0};  // the walk function's calls, at most the ask's budget
    std::uint32_t capped{0};     // needs whose far fields the spent budget left unpriced, summed over the stops
};

// A field as a stop from where the bot then is
struct Candidate
{
    QuestOrder order;                  // without leaveBelow, which the stop after it sets
    float worth{0.0f};                 // Stop::worth
    float value{0.0f};                 // the worth per minute of walk and stay
    std::vector<std::uint32_t> needs;  // the expected needs, into the log's, ascending
};

inline float StopValue(float worth, float walkMinutes, float stayMinutes)
{
    float const minutes = walkMinutes + stayMinutes;
    return minutes > 0.0f ? worth / minutes : 0.0f;
}

// The route's worth and minutes from its stops
inline void Price(Plan& plan)
{
    plan.worth = 0.0f;
    plan.minutes = 0.0f;
    for (Stop const& stop : plan.stops)
    {
        plan.worth += stop.worth;
        plan.minutes += stop.order.walkMinutes + stop.order.minutes;
    }
}

inline float WorthPerMinute(Plan const& plan) { return plan.minutes > 0.0f ? plan.worth / plan.minutes : 0.0f; }

namespace Detail
{
inline QuestFields::FieldAsk AskAt(Ask const& ask, float x, float y)
{
    QuestFields::FieldAsk from;
    from.mapId = ask.mapId;
    from.x = x;
    from.y = y;
    from.radius = ask.radius;
    from.level = ask.level;
    from.stayMinutes = ask.stayMinutes;
    from.runSpeed = ask.runSpeed;
    from.caution = ask.caution;
    from.groupSize = ask.groupSize;
    return from;
}

// The same cell or one of its eight neighbours
inline bool Beside(std::uint64_t a, std::uint64_t b)
{
    return FieldMapOf(a) == FieldMapOf(b) && std::abs(FieldCellXOf(a) - FieldCellXOf(b)) <= 1 &&
           std::abs(FieldCellYOf(a) - FieldCellYOf(b)) <= 1;
}

inline bool NearClosed(std::span<std::uint64_t const> closed, std::uint64_t key)
{
    if (closed.empty())
        return false;
    for (std::int32_t dx = -1; dx <= 1; ++dx)
        for (std::int32_t dy = -1; dy <= 1; ++dy)
            if (std::binary_search(closed.begin(), closed.end(),
                                   FieldCellKey(FieldMapOf(key), FieldCellXOf(key) + dx, FieldCellYOf(key) + dy)))
                return true;
    return false;
}

// The credits the plan takes off a need for a stop: the field's expected credits, rounded
inline std::uint32_t Taken(QuestFields::Need const& need, QuestFields::NeedOffer const& one)
{
    return std::min(need.left, static_cast<std::uint32_t>(std::lround(one.credits)));
}

// A ranked field as a stop: its expected needs are those with credits there, a take only while the log has a free
// slot (the stop's hand-ins free theirs first). A stop of hand-ins and takes alone lasts their seconds, not the
// field's stay
inline Candidate CandidateOf(QuestFields::Ranked const& field, Log const& log, Ask const& ask)
{
    using QuestFields::NeedKind;
    Candidate candidate;
    QuestOrder& order = candidate.order;
    std::size_t free = log.freeSlots;
    for (QuestFields::NeedOffer const& one : field.offer.needs)
        if (log.needs[one.need].kind == NeedKind::HandIn && Taken(log.needs[one.need], one))
            ++free;
    std::vector<QuestOrderNeed> rest;
    float worth = 0.0f;
    float seconds = 0.0f;
    bool atNpc = true;
    for (QuestFields::NeedOffer const& one : field.offer.needs)
    {
        QuestFields::Need const& need = log.needs[one.need];
        if (!(one.credits > 0.0f))
            continue;
        std::uint32_t const taken = Taken(need, one);
        if (need.kind == NeedKind::Take)
        {
            if (!free)
                continue;
            free -= taken ? 1 : 0;
        }
        atNpc = atNpc && (need.kind == NeedKind::HandIn || need.kind == NeedKind::Take);
        worth += one.credits * need.worthPerCredit;
        seconds += one.credits * need.seconds;
        candidate.needs.push_back(one.need);
        (need.kind == NeedKind::HandIn ? order.expected : rest).push_back({need.quest, need.objective, taken});
    }
    order.expected.insert(order.expected.end(), rest.begin(), rest.end());
    order.word = atNpc ? QuestOrderWord::HandIn : QuestOrderWord::Field;
    order.end = QuestOrderEnd::Done;
    order.mapId = ask.mapId;
    order.x = field.offer.x;
    order.y = field.offer.y;
    order.cell = field.offer.centre;
    order.allowanceYards = ask.allowanceYards;
    order.walkMinutes = field.walkMinutes;
    order.minutes =
        atNpc ? std::min(ask.stayMinutes, std::max(NPC_STAY_FLOOR_MINUTES, seconds / QuestFields::SECONDS_PER_MINUTE))
              : ask.stayMinutes;
    candidate.worth = worth * field.riskFactor;
    order.worthPerMinute = StopValue(candidate.worth, 0.0f, order.minutes);
    candidate.value = StopValue(candidate.worth, order.walkMinutes, order.minutes);
    return candidate;
}

// The needs of one quest as a slot of the log would give them
inline std::vector<QuestFields::Need> NeedsOf(QuestIndex const& index, Ask const& ask, std::uint32_t quest, bool ready)
{
    QuestSlot slot;
    slot.quest = quest;
    slot.state = ready ? Circuit::SLOT_COMPLETE : 0;
    return QuestFields::Needs(index, std::span<QuestSlot const>(&slot, 1), ask.team, ask.pricing);
}

// Register row G344: the best candidate around the bot's own cell that hands in a ready quest whose ender stands in
// that cell, or none. Its field holds the cell, so it hands in every such quest
inline Candidate const* AtHand(FieldMap const& map, Log const& log, Ask const& ask,
                               std::vector<Candidate> const& priced)
{
    std::uint64_t const here = FieldCellKeyAt(ask.mapId, ask.x, ask.y);
    for (Candidate const& candidate : priced)
    {
        if (!Beside(candidate.order.cell, here))
            continue;
        for (std::uint32_t const at : candidate.needs)
        {
            QuestFields::Need const& need = log.needs[at];
            if (need.kind != QuestFields::NeedKind::HandIn)
                continue;
            for (QuestFields::NeedSource const& source : need.sources)
            {
                std::span<std::uint64_t const> const cells = map.CellsOf(source.kind, source.entry);
                if (std::find(cells.begin(), cells.end(), here) != cells.end())
                    return &candidate;
            }
        }
    }
    return nullptr;
}

// The walk function's answers in one route: a pair asked once, no call beyond the budget, a field it finds no way to
// refused for the rest of the route
struct Walks
{
    struct Answer
    {
        float fromX{0.0f};
        float fromY{0.0f};
        std::uint64_t to{0};
        std::optional<float> minutes;
    };

    Walk const* walk{nullptr};
    std::uint32_t budget{0};
    std::uint32_t calls{0};
    std::uint32_t capped{0};
    std::vector<Answer> asked;
    std::vector<FarGap> gaps;

    bool On() const { return walk && *walk; }

    bool Refused(std::uint64_t cell) const
    {
        return std::any_of(gaps.begin(), gaps.end(), [cell](FarGap const& gap) { return gap.cell == cell; });
    }

    // The minutes to a field's centre from a point, asked before or by one call while the budget lasts; false: the
    // budget is spent. A negative or not-a-number answer is no way
    bool To(float x, float y, Candidate const& candidate, std::optional<float>& minutes)
    {
        std::uint64_t const cell = candidate.order.cell;
        for (Answer const& answer : asked)
            if (answer.fromX == x && answer.fromY == y && answer.to == cell)
            {
                minutes = answer.minutes;
                return true;
            }
        if (calls >= budget)
            return false;
        ++calls;
        minutes = (*walk)(x, y, candidate.order.x, candidate.order.y);
        if (minutes && !(*minutes >= 0.0f))
            minutes.reset();
        asked.push_back({x, y, cell, minutes});
        return true;
    }
};

// Per need no near candidate serves, in the log's order: its far candidates by worth, then straight yards, then cell;
// the first the walk function finds a way to joins the candidates at its minutes, one without a way becomes a gap
inline void PriceFar(Walks& walks, Log const& log, float x, float y, std::vector<std::pair<Candidate, float>>& beyond,
                     std::vector<std::uint8_t>& served, std::vector<Candidate>& priced)
{
    std::sort(beyond.begin(), beyond.end(),
              [](std::pair<Candidate, float> const& a, std::pair<Candidate, float> const& b)
              {
                  if (a.first.worth != b.first.worth)
                      return a.first.worth > b.first.worth;
                  if (a.second != b.second)
                      return a.second < b.second;
                  return a.first.order.cell < b.first.order.cell;
              });
    std::vector<std::uint8_t> asked(beyond.size(), 0);
    for (std::uint32_t need = 0; need < log.needs.size(); ++need)
        for (std::size_t i = 0; i < beyond.size() && !served[need]; ++i)
        {
            Candidate& candidate = beyond[i].first;
            if (asked[i] || std::find(candidate.needs.begin(), candidate.needs.end(), need) == candidate.needs.end())
                continue;
            std::optional<float> minutes;
            if (!walks.To(x, y, candidate, minutes))
            {
                ++walks.capped;
                break;
            }
            asked[i] = 1;
            if (!minutes)
            {
                walks.gaps.push_back({candidate.order.cell, candidate.order.expected});
                continue;
            }
            candidate.order.walkMinutes = *minutes;
            candidate.value = StopValue(candidate.worth, *minutes, candidate.order.minutes);
            for (std::uint32_t const at : candidate.needs)
                served[at] = 1;
            priced.push_back(candidate);
        }
}
}  // namespace Detail

// The candidates for the next stop from a point: TopFields' fields of the log as it then is, priced as stops, best
// first (ties by centre cell). None whose centre is a closed cell or a neighbour of one, or a cell the route used.
// With the route's walks (WQ17d): the fields beyond the radius join for the needs no near candidate serves
// (Detail::PriceFar), and no field the walk function found no way to
inline std::vector<Candidate> Priced(FieldMap const& map, Log const& log, Ask const& ask, float x, float y,
                                     std::span<std::uint64_t const> used = {}, Detail::Walks* walks = nullptr)
{
    bool const withFar = walks && walks->On();
    QuestFields::FieldAsk from = Detail::AskAt(ask, x, y);
    if (withFar)
        from.radius = std::max(FAR_RADIUS, ask.radius);
    std::vector<Candidate> priced;
    std::vector<std::pair<Candidate, float>> beyond;  // with their straight yards
    std::vector<std::uint8_t> served(log.needs.size(), 0);
    Circuit::Point const here{x, y, 0.0f};
    QuestFields::Ranking const ranking = QuestFields::TopFields(map, log.needs, from);
    for (QuestFields::Ranked const& field : ranking.fields)
    {
        std::uint64_t const centre = field.offer.centre;
        if (std::find(used.begin(), used.end(), centre) != used.end() || Detail::NearClosed(ask.closed, centre) ||
            (walks && walks->Refused(centre)))
            continue;
        Candidate candidate = Detail::CandidateOf(field, log, ask);
        if (!(candidate.worth > 0.0f))
            continue;
        float const yards = withFar ? Circuit::Yards(here, {field.offer.x, field.offer.y, 0.0f}) : 0.0f;
        if (yards > ask.radius)
        {
            beyond.emplace_back(std::move(candidate), yards);
            continue;
        }
        for (std::uint32_t const need : candidate.needs)
            served[need] = 1;
        priced.push_back(std::move(candidate));
    }
    if (!beyond.empty())
        Detail::PriceFar(*walks, log, x, y, beyond, served, priced);
    std::sort(priced.begin(), priced.end(), [](Candidate const& a, Candidate const& b)
              { return a.value != b.value ? a.value > b.value : a.order.cell < b.order.cell; });
    return priced;
}

// Neighbouring fields thinned: a candidate whose centre lies within one cell of a better one that holds the same
// needs goes, so the shifts of one area are one candidate. The best candidate always stays, so the route's choice of
// the next stop is the same with and without the thinning; Build skips it. For who lists the candidates
inline std::vector<Candidate> Thinned(std::vector<Candidate> const& priced)
{
    std::vector<Candidate> kept;
    for (std::size_t i = 0; i < priced.size(); ++i)
    {
        bool beaten = false;
        for (std::size_t j = 0; j < i && !beaten; ++j)
            beaten = Detail::Beside(priced[j].order.cell, priced[i].order.cell) && priced[j].needs == priced[i].needs;
        if (!beaten)
            kept.push_back(priced[i]);
    }
    return kept;
}

// The log after an order as the plan expects it: the expected credits off their needs; a handed-in quest frees its
// slot; a taken quest fills one and gets its objective needs (its hand-in at once when it has none); a quest whose
// credits are then all expected gets its hand-in need
inline void Advance(QuestIndex const& index, Ask const& ask, Log& log, QuestOrder const& order)
{
    std::vector<std::uint32_t> worked;
    for (QuestOrderNeed const& expected : order.expected)
    {
        auto const it =
            std::find_if(log.needs.begin(), log.needs.end(), [&expected](QuestFields::Need const& need)
                         { return need.left && need.quest == expected.quest && need.objective == expected.objective; });
        if (it == log.needs.end())
            continue;
        std::uint32_t const taken = std::min(it->left, expected.credits);
        if (!taken)
            continue;
        it->left -= taken;
        if (expected.objective == ORDER_HAND_IN)
            ++log.freeSlots;
        else if (expected.objective == ORDER_TAKE)
        {
            log.freeSlots -= log.freeSlots ? 1 : 0;
            std::vector<QuestFields::Need> opened = Detail::NeedsOf(index, ask, expected.quest, false);
            if (opened.empty())
                opened = Detail::NeedsOf(index, ask, expected.quest, true);
            log.needs.insert(log.needs.end(), opened.begin(), opened.end());
        }
        else
            worked.push_back(expected.quest);
    }
    for (std::uint32_t const quest : worked)
    {
        bool open = false;
        bool handIn = false;
        for (QuestFields::Need const& need : log.needs)
            if (need.quest == quest)
            {
                handIn = handIn || need.objective == ORDER_HAND_IN;
                open = open || (need.objective < ORDER_TAKE && need.left);
            }
        if (open || handIn)
            continue;
        std::vector<QuestFields::Need> const ready = Detail::NeedsOf(index, ask, quest, true);
        log.needs.insert(log.needs.end(), ready.begin(), ready.end());
    }
}

// The route. The first stop is the best candidate, or the one that hands in what stands in the bot's cell
// (Detail::AtHand). A further stop is added while the route before it is shorter than the horizon, the stops are
// fewer than the limit and the stop's worth per minute of walk and stay is not below the route's, so the route's
// worth per minute does not fall (StopRule::HorizonOnly drops this test). No two stops share a cell. Each stop becomes
// an order: it ends when its expected needs are done or its stay has passed, and leaving it pays below the next
// stop's worth per minute (0 at the last; batch F3 sets the rule). With a walk function far stops join as Priced
// says, its calls within the ask's budget over the whole route; the plan names the fields it found no way to
inline Plan Build(FieldMap const& map, QuestIndex const& index, std::span<QuestFields::Need const> needs,
                  Ask const& ask)
{
    Plan plan;
    Log log{std::vector<QuestFields::Need>(needs.begin(), needs.end()), ask.freeSlots};
    Detail::Walks walks;
    walks.walk = &ask.walk;
    walks.budget = ask.walkBudget;
    std::vector<std::uint64_t> used;
    float x = ask.x;
    float y = ask.y;
    while (plan.stops.size() < ask.maxStops && plan.minutes < ask.horizonMinutes)
    {
        std::vector<Candidate> const priced = Priced(map, log, ask, x, y, used, &walks);
        if (priced.empty())
            break;
        Candidate const* next = plan.stops.empty() ? Detail::AtHand(map, log, ask, priced) : nullptr;
        if (!next)
            next = &priced.front();
        if (!plan.stops.empty())
        {
            if (ask.stopRule == StopRule::RateHolds && next->value < WorthPerMinute(plan))
                break;
            plan.stops.back().order.leaveBelow = next->value;
        }
        Advance(index, ask, log, next->order);
        used.push_back(next->order.cell);
        x = next->order.x;
        y = next->order.y;
        plan.stops.push_back({next->order, next->worth});
        Price(plan);
    }
    plan.gaps = std::move(walks.gaps);
    plan.walkCalls = walks.calls;
    plan.capped = walks.capped;
    return plan;
}
}  // namespace Route
}  // namespace Planner

#endif
