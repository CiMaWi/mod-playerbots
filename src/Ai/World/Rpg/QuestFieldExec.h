/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTFIELDEXEC_H
#define _PLAYERBOT_QUESTFIELDEXEC_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "FieldMap.h"
#include "QuestFields.h"
#include "QuestOrderTypes.h"
#include "StatusEndTypes.h"

// The executor on an order (questing plan WQ19a, PLAN 3.10 part 4, WQ.43 points 5 and 6): at the order's place the
// bot works every open need of its log that it finds, and the order ends by its own end. Pick chooses among the
// candidates the looks found (one look per kind covers every open need's entries), End names the end a tick finds.
// An order writes no failure row for a target that is not there: its ends map to outcomes other than Abandoned, but
// for no_ender (WQ19b: a ready quest it expected to hand in at the place, not handed in after the look time).
// Pure: the field map, the log's needs (QuestFields::Needs) and the order's state (QuestOrderRun); no game object.
// tests/quest-field-exec-selftest.cpp checks it over generated orders and logs
namespace Planner
{
namespace QuestFieldExec
{
constexpr std::uint32_t LOOK_MS = 60 * 1000;  // at the place this long with no candidate and no credit: dry
constexpr float NEUTRAL_CAUTION = 1.0f;       // the planner's caution weight; the executor has none until WQ18a
constexpr float WALK_SLACK = 2.0f;            // not at the place after this many times the planned walk plus the
                                              // stay: the order ends `time` (every order ends)
constexpr float MAX_MINUTES = 24.0f * 60.0f;  // a stay or walk longer than this counts as this long
constexpr std::uint32_t MS_PER_MINUTE = 60 * 1000;
constexpr std::uint32_t ROW_COUNT_MAX = 0xFFF;  // the QuestOrder row's credits and minutes, 12 bits each
constexpr std::uint64_t ROW_CELL_MASK = 0x3FF;  // and its cell, 10 bits a coordinate
constexpr float WAY_MINUTES = 5.0f;    // WQ20: a way's stay, the slack for the work on the way: a way that does not
                                       // arrive ends `time` after twice its walk plus this
constexpr float CLOSER_YARDS = 10.0f;  // WQ43: nearer the aim than ever by this much restarts the walk's bound

// What a look found for an open need: a creature (its own level) or an object, where it stands
struct Candidate
{
    std::uint32_t need{0};  // into QuestOrderRun::open
    FieldKind kind{FieldKind::Creature};
    std::uint8_t level{0};  // a creature's; objects 0
    std::uint32_t mapId{0};
    float x{0.0f};
    float y{0.0f};
};

// The bot as Pick asks of it: where it stands, its level and its caution; WQ20: whether it walks to the place (not
// arrived), when the way's allowance applies
struct PickAsk
{
    float x{0.0f};
    float y{0.0f};
    std::uint8_t level{1};
    float caution{NEUTRAL_CAUTION};
    bool onWay{false};
};

// A game time as the run keeps it: 0 means "not yet", so a stamp taken at 0 is 1
inline std::uint32_t Stamp(std::uint32_t now) { return now ? now : 1; }

// WQ43 (L4): the bot's distance to its aim before the arrival, each executor tick (DoOrder). The first distance, and
// one at least CLOSER_YARDS below the best so far, becomes the best and restarts the walk's bound (End): a bot that
// still closes on its aim between its fights is not stuck. The best falls by a step at each restart and only a new aim
// (at a count of the log) forgets it, so a bot that never arrives restarts the bound a bounded number of times
inline void NoteCloser(QuestOrderRun& run, float yards, std::uint32_t now)
{
    if (run.arrivedAt || (run.closerAt && yards > run.bestYards - CLOSER_YARDS))
        return;
    run.bestYards = yards;
    run.closerAt = Stamp(now);
}

// Whether the order counts the log again (the executor's tick, NoteOrderCredits): a quest it works turned complete or
// left the log (worked(quest): its status is neither incomplete nor failed; a failed one keeps its objectives open, so
// it is not counted each tick). A take's need closes at the counts the takes start. WQ43 (L2): an open hand-in whose
// quest left the log without its reward (gone(quest)) counts again too, so its stale need closes: the quest is in no
// NoEnderQuests and the order gives up no quest the bot no longer holds
template <typename Worked, typename Gone>
bool CountAgain(std::span<QuestOrderOpen const> open, Worked const& worked, Gone const& gone)
{
    return std::any_of(open.begin(), open.end(),
                       [&](QuestOrderOpen const& need)
                       {
                           if (need.objective == QuestFields::TAKE)
                               return false;
                           if (need.objective == QuestFields::HAND_IN)
                               return need.left > 0 && gone(need.quest);
                           return worked(need.quest);
                       });
}

inline std::uint32_t MinutesMs(float minutes)
{
    return static_cast<std::uint32_t>(std::clamp(minutes, 0.0f, MAX_MINUTES) * static_cast<float>(MS_PER_MINUTE));
}

// Whether a creature of this level is one the bot takes: QuestFields' reach rule (the weighted risk of the levels
// above the bot at or below the refusal), asked of the creature seen rather than of a field's row
inline bool LevelWithinReach(int creatureLevel, int botLevel, float caution = NEUTRAL_CAUTION)
{
    QuestFields::FieldAsk ask;
    ask.caution = caution;
    return QuestFields::Detail::LevelWithinReach(static_cast<float>(creatureLevel - botLevel), ask);
}

// Whether a point lies in the field of the order's cell: the cell and its eight neighbours, on its map
inline bool InField(std::uint64_t cell, std::uint32_t mapId, float x, float y)
{
    return mapId == FieldMapOf(cell) && std::abs(FieldCellOf(x) - FieldCellXOf(cell)) <= 1 &&
           std::abs(FieldCellOf(y) - FieldCellYOf(cell)) <= 1;
}

// WQ20: whether a point is on the way: walking to it and on to the place is at most the allowance longer than
// walking straight there. Straight yards: the route's real walk is travel's. An allowance of 0 or less takes nothing
inline bool Detour(float botX, float botY, float x, float y, float placeX, float placeY, float allowance)
{
    if (allowance <= 0.0f)
        return false;
    float const extra =
        std::hypot(x - botX, y - botY) + std::hypot(placeX - x, placeY - y) - std::hypot(placeX - botX, placeY - botY);
    return extra <= allowance;
}

// WQ20: whether the order takes a point: in the field of its cell, or, while the bot walks to the place, on the way
// within the allowance (on the order's map). After the arrival only the field rule (the detour there would be a
// circle of half the allowance around the place, which the field already covers). The executor's target choice
// (ValidTarget, the seeks) asks it too, so a nearer target off the way is not the one held
inline bool OnOrder(QuestOrder const& order, PickAsk const& ask, std::uint32_t mapId, float x, float y)
{
    return InField(order.cell, mapId, x, y) ||
           (ask.onWay && mapId == order.mapId && Detour(ask.x, ask.y, x, y, order.x, order.y, order.allowanceYards));
}

// questing WQ33: a held target whose stand point or walk is refused (its pass-over `walk`: no stand point the mesh
// carries, or a walk that gains nothing) counts against its need, not the target: the STAND_REFUSALS-th closes the
// need, which no target is held for again, and that need's quest is given up with NO_STAND (one failure row). A lone
// quest's executor (no order) counts its one need so and ends its status. A credit of the need (CarryRefusals) or an
// act at one of its targets (StandWorked) clears the count; a closed need stays closed while its order runs
constexpr std::uint8_t STAND_REFUSALS = 3;
constexpr char const* NO_STAND = "no_stand";

// One refusal of a need's count: true when it is the one that closes the need (once; a closed count stays closed)
inline bool StandRefused(std::uint8_t& refusals)
{
    if (refusals >= STAND_REFUSALS)
        return false;
    return ++refusals == STAND_REFUSALS;
}

inline bool StandClosed(std::uint8_t refusals) { return refusals >= STAND_REFUSALS; }

// An act at a target of the need (an attack, a use, a loot): the count starts again, unless the need is closed
inline void StandWorked(std::uint8_t& refusals)
{
    if (!StandClosed(refusals))
        refusals = 0;
}

// A refusal on an order: true when it closes the need and no other need of its quest closed before (the failure row
// is the quest's, once per order)
inline bool RefuseNeed(std::span<QuestOrderOpen> open, std::size_t need)
{
    if (need >= open.size() || !StandRefused(open[need].refusals))
        return false;
    for (std::size_t at = 0; at < open.size(); ++at)
        if (at != need && open[at].quest == open[need].quest && StandClosed(open[at].refusals))
            return false;
    return true;
}

// Whether a candidate is one the order takes: its need open and not closed (WQ33), within reach, in the field or on
// the way (OnOrder)
inline bool Takes(Candidate const& candidate, std::span<QuestOrderOpen const> open, QuestOrder const& order,
                  PickAsk const& ask)
{
    return candidate.need < open.size() && open[candidate.need].left > 0 &&
           !StandClosed(open[candidate.need].refusals) &&
           (candidate.kind == FieldKind::Object || LevelWithinReach(candidate.level, ask.level, ask.caution)) &&
           OnOrder(order, ask, candidate.mapId, candidate.x, candidate.y);
}

// The nearest candidate the order takes (ties: the first); none when it takes none
inline std::optional<std::size_t> Pick(std::span<Candidate const> candidates, std::span<QuestOrderOpen const> open,
                                       QuestOrder const& order, PickAsk const& ask)
{
    std::optional<std::size_t> best;
    float bestSquare = 0.0f;
    for (std::size_t at = 0; at < candidates.size(); ++at)
    {
        Candidate const& candidate = candidates[at];
        if (!Takes(candidate, open, order, ask))
            continue;
        float const dx = candidate.x - ask.x;
        float const dy = candidate.y - ask.y;
        float const square = dx * dx + dy * dy;
        if (!best || square < bestSquare)
        {
            best = at;
            bestSquare = square;
        }
    }
    return best;
}

// Whether a need the order expects still has credits left in the log
inline bool ExpectedOpen(QuestOrder const& order, std::span<QuestOrderOpen const> open)
{
    for (QuestOrderNeed const& expected : order.expected)
        for (QuestOrderOpen const& need : open)
            if (need.left && need.quest == expected.quest && need.objective == expected.objective)
                return true;
    return false;
}

// The end a tick finds, if any. finding: a candidate the order takes is in reach this tick (or the bot works one).
// WQ19b: whether an expected need is a hand-in still open: the bot has not handed that quest in
inline bool HandInOpen(QuestOrderNeed const& expected, std::span<QuestOrderOpen const> open)
{
    return expected.objective == QuestFields::HAND_IN &&
           std::any_of(open.begin(), open.end(), [&expected](QuestOrderOpen const& need)
                       { return need.left && need.quest == expected.quest && need.objective == QuestFields::HAND_IN; });
}

// Whether a hand-in the order expects is still open
inline bool HandInOpen(QuestOrder const& order, std::span<QuestOrderOpen const> open)
{
    return std::any_of(order.expected.begin(), order.expected.end(),
                       [open](QuestOrderNeed const& expected) { return HandInOpen(expected, open); });
}

// An order at NPCs (word hand_in; questing WQ18b part 3): its planned stay is its hand-ins' and takes' seconds, too
// short for the walk between two NPCs of a hub and shorter than the look time. Both its bounds last the look time
// longer, so at the NPC the look's verdict (no_ender, dry) comes before the clock's, and an NPC that gives nothing
// ends the order by a word that bounds its return (a failure row, a closed cell)
inline std::uint32_t NpcSlackMs(QuestOrder const& order) { return order.word == QuestOrderWord::HandIn ? LOOK_MS : 0; }

// A newer order first; then the plan's end (done: an order that expects needs and has none open; arrived: a way at its
// place once it holds no target, WQ20b); then the stay (from the arrival; without one, twice the planned walk plus the
// stay from the start, the last count of the log or (WQ43, L4) the last new best distance to the aim, NoteCloser: a bot
// that earns on the way or still closes on its aim is working, not stuck; the counts are finite and each new best is
// CLOSER_YARDS nearer, so every order ends, one bound after it last counted or came nearer); then the look time
// without a find at the place: no_ender while an expected hand-in is open (WQ19b), else dry
inline std::optional<QuestOrderEnd> End(QuestOrderRun const& run, std::uint32_t now, bool finding)
{
    if (run.replaced)
        return QuestOrderEnd::Replaced;
    QuestOrder const& order = run.order;
    if (order.end == QuestOrderEnd::Done && !order.expected.empty() && !ExpectedOpen(order, run.open))
        return QuestOrderEnd::Done;
    if (order.end == QuestOrderEnd::Arrived && run.arrivedAt && !finding)
        return QuestOrderEnd::Arrived;
    if (!run.arrivedAt)
    {
        if (!run.startedAt)
            return std::nullopt;
        float const bound = std::min(WALK_SLACK * order.walkMinutes + order.minutes, MAX_MINUTES);
        // the start counts the log, so its count is the start's stamp until a credit comes
        std::uint32_t walked = now - (run.lastCount ? run.lastCount : run.startedAt);
        if (run.closerAt)
            walked = std::min(walked, now - run.closerAt);
        if (walked >= MinutesMs(bound) + NpcSlackMs(order))
            return QuestOrderEnd::Time;
        return std::nullopt;
    }
    std::uint32_t const atPlace = now - run.arrivedAt;
    // WQ18b part 3: an order at NPCs counts its stay from the last count of the log too (each hand-in and each take
    // restarts it, as a credit restarts the walk's bound); a field's stay is the planned one, from the arrival
    bool const atNpc = order.word == QuestOrderWord::HandIn;
    std::uint32_t const stayed = atNpc && run.lastCount ? std::min(atPlace, now - run.lastCount) : atPlace;
    if (stayed >= MinutesMs(order.minutes) + NpcSlackMs(order))
        return QuestOrderEnd::Time;
    std::uint32_t const quiet = run.lastFound ? std::min(atPlace, now - run.lastFound) : atPlace;
    if (!finding && quiet >= LOOK_MS)
        return HandInOpen(order, run.open) ? QuestOrderEnd::NoEnder : QuestOrderEnd::Dry;
    return std::nullopt;
}

// The status end of an order's end: done finishes; time and dry end by the clock; a way arrives; a newer order ends
// it as an order does. Only no_ender (WQ19b, WQ.43 point 6: the failed hand-in) is Abandoned, which writes the
// failure row at the status door (NewRpgStatusEnd.cpp). The planned takes an end leaves open at their giver get
// theirs from the order's end, whatever its outcome (RefusedTakes, WQ18c)
constexpr StatusEnd::Outcome OutcomeOf(QuestOrderEnd end)
{
    switch (end)
    {
        case QuestOrderEnd::Done:
            return StatusEnd::Outcome::Finished;
        case QuestOrderEnd::Arrived:
            return StatusEnd::Outcome::Arrived;
        case QuestOrderEnd::Time:
        case QuestOrderEnd::Dry:
        case QuestOrderEnd::Bags:  // WQ18b: the bags refused the hand-ins; no failure row either
            return StatusEnd::Outcome::TimedOut;
        case QuestOrderEnd::Replaced:
            return StatusEnd::Outcome::Ordered;
        case QuestOrderEnd::NoEnder:
            return StatusEnd::Outcome::Abandoned;
    }
    return StatusEnd::Outcome::TimedOut;
}

// The log's open needs as the order counts them: every need with credits left, in the needs' order
inline std::vector<QuestOrderOpen> Open(std::span<QuestFields::Need const> needs)
{
    std::vector<QuestOrderOpen> open;
    for (QuestFields::Need const& need : needs)
        if (need.left)
            open.push_back({need.quest, need.objective, need.left});
    return open;
}

// WQ33: a recount of the needs keeps each need's refusals from the count before while it gained no credit (its credits
// left the same); a credit clears them
inline void CarryRefusals(std::span<QuestOrderOpen const> before, std::span<QuestOrderOpen> after)
{
    for (QuestOrderOpen& need : after)
        for (QuestOrderOpen const& was : before)
            if (was.quest == need.quest && was.objective == need.objective)
                need.refusals = was.left == need.left ? was.refusals : 0;
}

// Whether one of a need's sources (a creature, an object; a hand-in's enders) has a row in the given cells
inline bool SourceInField(FieldMap const& map, std::span<std::uint64_t const> cells, QuestFields::Need const& need)
{
    return std::any_of(
        need.sources.begin(), need.sources.end(),
        [&map, &cells](QuestFields::NeedSource const& source)
        {
            return std::any_of(
                cells.begin(), cells.end(), [&map, &source](std::uint64_t cell)
                { return QuestFields::Detail::FindRow(map, cell, source.kind, source.entry) != nullptr; });
        });
}

// What an order at a point expects of the objectives: every open objective of the log (no hand-in: ExpectHandIns; no
// take: WQ31's) with a source that has a row in the field around the point, all its credits
inline std::vector<QuestOrderNeed> Expect(FieldMap const& map, std::span<QuestFields::Need const> needs,
                                          std::uint32_t mapId, float x, float y)
{
    std::vector<std::uint64_t> const cells = map.Around(mapId, x, y);
    std::vector<QuestOrderNeed> expected;
    for (QuestFields::Need const& need : needs)
        if (need.left && need.objective != QuestFields::HAND_IN && need.objective != QuestFields::TAKE &&
            SourceInField(map, cells, need))
            expected.push_back({need.quest, need.objective, need.left});
    return expected;
}

// WQ19b: the hand-ins an order at a point expects: every ready quest of the log (an open hand-in need) with an ender
// that has a row in the field around the point, one credit each
inline std::vector<QuestOrderNeed> ExpectHandIns(FieldMap const& map, std::span<QuestFields::Need const> needs,
                                                 std::uint32_t mapId, float x, float y)
{
    std::vector<std::uint64_t> const cells = map.Around(mapId, x, y);
    std::vector<QuestOrderNeed> expected;
    for (QuestFields::Need const& need : needs)
        if (need.left && need.objective == QuestFields::HAND_IN && SourceInField(map, cells, need))
            expected.push_back({need.quest, need.objective, 1});
    return expected;
}

// WQ19b: the quests of the order's open hand-in needs the bot has handed in now (rewarded(quest): the game's reward
// status), each once
template <typename Rewarded>
std::vector<std::uint32_t> HandedIn(std::span<QuestOrderOpen const> open, Rewarded const& rewarded)
{
    std::vector<std::uint32_t> quests;
    for (QuestOrderOpen const& need : open)
        if (need.objective == QuestFields::HAND_IN && need.left && rewarded(need.quest) &&
            std::find(quests.begin(), quests.end(), need.quest) == quests.end())
            quests.push_back(need.quest);
    return quests;
}

// WQ19b: a hand-in on the order: each quest HandedIn finds counts once in handIns, its needs close and the dry clock
// restarts (a hand-in is the place giving something). Returns how many; the caller counts the log again when any
template <typename Rewarded>
std::uint32_t NoteHandIns(QuestOrderRun& run, std::uint32_t now, Rewarded const& rewarded)
{
    std::vector<std::uint32_t> const quests = HandedIn(std::span<QuestOrderOpen const>(run.open), rewarded);
    for (QuestOrderOpen& need : run.open)
        if (need.objective == QuestFields::HAND_IN &&
            std::find(quests.begin(), quests.end(), need.quest) != quests.end())
            need.left = 0;
    run.handIns = static_cast<std::uint16_t>(std::min<std::size_t>(run.handIns + quests.size(), 0xFFFF));
    if (!quests.empty())
        run.lastFound = Stamp(now);
    return static_cast<std::uint32_t>(quests.size());
}

// WQ19b: the quests an order's no_ender end gives up, one failure row each: the hand-ins it expected that are still
// open (none for a quest handed in, none for a ready quest it did not expect)
inline std::vector<std::uint32_t> NoEnderQuests(QuestOrderRun const& run)
{
    std::vector<std::uint32_t> quests;
    for (QuestOrderNeed const& expected : run.order.expected)
        if (HandInOpen(expected, run.open) && std::find(quests.begin(), quests.end(), expected.quest) == quests.end())
            quests.push_back(expected.quest);
    return quests;
}

// WQ18b: the takes an order expects are open needs while the bot can still take them (takeable(quest): not in its
// log, not handed in, not given up). The log's needs (QuestFields::Needs) hold none, because a quest not yet taken is
// in no log: an order of takes only found nothing open and ended done at its first tick. Appended after the log's
// needs, so the seeks' indexes into `open` stay
template <typename Takeable>
void OpenTakes(std::vector<QuestOrderOpen>& open, QuestOrder const& order, Takeable const& takeable)
{
    for (QuestOrderNeed const& expected : order.expected)
        if (expected.objective == QuestFields::TAKE && takeable(expected.quest) &&
            std::none_of(open.begin(), open.end(), [&expected](QuestOrderOpen const& need)
                         { return need.quest == expected.quest && need.objective == QuestFields::TAKE; }))
            open.push_back({expected.quest, QuestFields::TAKE, 1});
}

// WQ18b: whether a need an order expects is open for the bot now (GoalApply::AdvanceRoute's test, over the same count
// of the log as the order's own): a take while its quest can still be taken, any other need while the log's need has
// credits left
template <typename Takeable>
bool NeedOpen(std::span<QuestOrderOpen const> open, QuestOrderNeed const& need, Takeable const& takeable)
{
    if (need.objective == QuestFields::TAKE)
        return takeable(need.quest);
    return std::any_of(open.begin(), open.end(), [&need](QuestOrderOpen const& one)
                       { return one.left && one.quest == need.quest && one.objective == need.objective; });
}

// WQ18b: the end the executor names when End finds one. A hand-in the bags refuse (bagsRefuse(quest): the quest is
// ready and only the room for its reward is missing; since WQ43 the bot makes room at the ender, so this is left only
// when nothing it may destroy covers the reward) is no failed hand-in: its need closes, so it gets no failure row
// (NoEnderQuests) and the log upkeep keeps the ready quest (QuestLogRules' Abandon drops a ready quest only for a
// failed hand-in on record). When every expected hand-in still open is such a one, no_ender becomes `bags` (ends by
// the clock, no row: the bot comes back with room); one its ender failed keeps no_ender, with a row for it alone
// WQ18b part 4: no_ender is said at the ender. A hand-in whose ender stands elsewhere in the order's field
// (elsewhere(quest): EnderElsewhere; the order looked at another NPC) did not fail either: its need closes, it gets no
// row and the quest stays for the route's next order. With none left the look found nothing where the bot stands: dry
template <typename BagsRefuse, typename Elsewhere>
QuestOrderEnd NameEnd(QuestOrderRun& run, QuestOrderEnd end, BagsRefuse const& bagsRefuse, Elsewhere const& elsewhere)
{
    if (end != QuestOrderEnd::NoEnder)
        return end;
    auto const close = [&run](std::uint32_t quest)
    {
        for (QuestOrderOpen& need : run.open)
            if (need.quest == quest && need.objective == QuestFields::HAND_IN)
                need.left = 0;
    };
    for (std::uint32_t const quest : NoEnderQuests(run))
        if (elsewhere(quest))
            close(quest);
    std::vector<std::uint32_t> const quests = NoEnderQuests(run);
    if (quests.empty())
        return QuestOrderEnd::Dry;
    std::size_t refused = 0;
    for (std::uint32_t const quest : quests)
    {
        if (!bagsRefuse(quest))
            continue;
        ++refused;
        close(quest);
    }
    return refused > 0 && refused == quests.size() ? QuestOrderEnd::Bags : QuestOrderEnd::NoEnder;
}

template <typename BagsRefuse>
QuestOrderEnd NameEnd(QuestOrderRun& run, QuestOrderEnd end, BagsRefuse const& bagsRefuse)
{
    return NameEnd(run, end, bagsRefuse, [](std::uint32_t) { return false; });
}

// WQ18b: the order's own end behind a status end (the status door, NewRpgStatusEnd.cpp). The executor ends its order
// with the end's word and outcome (DoOrder); any other pair is something that took the bot over (an interlude, a
// switch, a test's order): the QuestOrder row counts that as replaced, and the route does not take it for the order's
// end (GoalApply::EndRouteOrder), so the order is given again
inline std::optional<QuestOrderEnd> OwnEnd(char const* word, StatusEnd::Outcome outcome)
{
    if (!word)
        return std::nullopt;
    for (QuestOrderEnd const own :
         {QuestOrderEnd::Done, QuestOrderEnd::Arrived, QuestOrderEnd::Time, QuestOrderEnd::Dry, QuestOrderEnd::Replaced,
          QuestOrderEnd::NoEnder, QuestOrderEnd::Bags})
        if (std::string_view(word) == QuestOrderEndName(own) && OutcomeOf(own) == outcome)
            return own;
    return std::nullopt;
}

// WQ18b part 3: a place an order at NPCs can walk to: an ender of a hand-in or a giver of a take, as the quest index
// has it (the spawn)
struct NpcSpot
{
    std::uint32_t quest{0};
    std::uint32_t objective{ORDER_HAND_IN};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// The NPC an order sends the bot to (WQ18b parts 3 and 4): the nearest spot of an expected hand-in that is still open,
// within the order's field (its cell and the eight round it); with no hand-in open, the nearest of an expected take
// (the route counts the log's free slots after a stop's hand-ins). Whatever the order's word: an order at NPCs names
// the middle of its cell, where no NPC stands (lab13, 2026-11-25: 86 `hand_in:time` rows of 12 bots), and a field
// order's place can lie farther from its field's ender than the giver search reaches (part 4, the same lab: quest 313
// ready, its ender 122 yards from the place, `field:no_ender` twice and the quest abandoned). None: no such spot. The
// executor asks at the order's start and after each count of the log, so the bot goes from one NPC to the next (AimOf)
inline std::optional<std::size_t> NearestSpot(QuestOrder const& order, std::span<QuestOrderOpen const> open,
                                              std::span<NpcSpot const> spots, float x, float y)
{
    auto const beside = [](std::int32_t a, std::int32_t b) { return a - b <= 1 && b - a <= 1; };
    std::optional<std::size_t> best;
    float bestYards = 0.0f;
    for (std::size_t i = 0; i < spots.size(); ++i)
    {
        NpcSpot const& spot = spots[i];
        QuestOrderNeed const need{spot.quest, spot.objective, 0};
        if (std::find(order.expected.begin(), order.expected.end(), need) == order.expected.end() ||
            std::none_of(open.begin(), open.end(), [&spot](QuestOrderOpen const& one)
                         { return one.left && one.quest == spot.quest && one.objective == spot.objective; }))
            continue;
        std::uint64_t const cell = FieldCellKeyAt(order.mapId, spot.x, spot.y);
        if (!beside(FieldCellXOf(cell), FieldCellXOf(order.cell)) ||
            !beside(FieldCellYOf(cell), FieldCellYOf(order.cell)))
            continue;
        float const yards = std::hypot(spot.x - x, spot.y - y);
        bool const handIn = spot.objective == ORDER_HAND_IN;
        bool const bestHandIn = best && spots[*best].objective == ORDER_HAND_IN;
        if (!best || (handIn && !bestHandIn) || (handIn == bestHandIn && yards < bestYards))
        {
            best = i;
            bestYards = yards;
        }
    }
    return best;
}

// Where an order walks (WQ18b part 4): to its NPC while NearestSpot has one, else to its place. None: an order at NPCs
// (word hand_in) with no spot left stays where it is, its place being the middle of its cell. A new aim is a new walk:
// the executor takes the arrival again there
struct OrderAim
{
    bool npc{false};  // an NPC's spot, where the bot stays beside it; else the order's place
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};  // an NPC's spawn height; the place's is the ground's, which the executor reads
};

inline std::optional<OrderAim> AimOf(QuestOrder const& order, std::span<QuestOrderOpen const> open,
                                     std::span<NpcSpot const> spots, float x, float y)
{
    if (std::optional<std::size_t> const nearest = NearestSpot(order, open, spots, x, y))
        return OrderAim{true, spots[*nearest].x, spots[*nearest].y, spots[*nearest].z};
    if (order.word == QuestOrderWord::HandIn)
        return std::nullopt;
    return OrderAim{false, order.x, order.y, 0.0f};
}

constexpr float ENDER_YARDS = 20.0f;  // an ender this near the bot is one the order looked at (its arrival, its roam)

// WQ18b part 4: whether a ready quest's ender stands elsewhere: the order's field holds a spot of its hand-in and the
// bot stands at none of them. Such a hand-in did not fail (NameEnd). One with no spot in the field has nowhere to walk
// to, so the look where the bot stands speaks for it, as before
inline bool EnderElsewhere(QuestOrder const& order, std::span<NpcSpot const> spots, std::uint32_t quest, float x,
                           float y)
{
    bool inField = false;
    for (NpcSpot const& spot : spots)
    {
        if (spot.quest != quest || spot.objective != ORDER_HAND_IN || !InField(order.cell, order.mapId, spot.x, spot.y))
            continue;
        if (std::hypot(spot.x - x, spot.y - y) <= ENDER_YARDS)
            return false;
        inField = true;
    }
    return inField;
}

// WQ18c (WQ.44 item 2): the ends that give up the planned takes they leave open at the giver: the look found nothing
// to take (dry), the stay ran out (time) or a hand-in failed there (no_ender). A take-over (replaced) did not finish
// the order; done leaves none open; bags ends for the bot to make room and come back, its takes with it
constexpr bool GivesUpTakes(QuestOrderEnd end)
{
    return end == QuestOrderEnd::Dry || end == QuestOrderEnd::Time || end == QuestOrderEnd::NoEnder;
}

// WQ18c: whether the bot stands at a giver of a quest (a spot of its take within the yards of an NPC the order looked
// at)
inline bool AtGiver(std::span<NpcSpot const> spots, std::uint32_t quest, float x, float y)
{
    return std::any_of(spots.begin(), spots.end(),
                       [quest, x, y](NpcSpot const& spot)
                       {
                           return spot.quest == quest && spot.objective == QuestFields::TAKE &&
                                  std::hypot(spot.x - x, spot.y - y) <= ENDER_YARDS;
                       });
}

// WQ18c (WQ.44 item 2): the planned takes an order's end gives up, one cannot_take give-up each, written in one place
// (QuestGiveUp::OrderEnded): the expected takes still open when an end that gives them up (GivesUpTakes) comes after
// the arrival, while the bot stands at the quest's giver (atGiver) and the take test still allows the quest
// (mayTake: a full log, a grey quest, one given up or one the game refuses, WQ41, is no refusal of the giver). A giver
// that does not offer the quest leaves the take open and the order ends there. Once per quest
template <typename AtGiverOf, typename MayTake>
std::vector<std::uint32_t> RefusedTakes(QuestOrderRun const& run, QuestOrderEnd end, AtGiverOf const& atGiver,
                                        MayTake const& mayTake)
{
    std::vector<std::uint32_t> quests;
    if (!GivesUpTakes(end) || !run.arrivedAt)
        return quests;
    for (QuestOrderNeed const& expected : run.order.expected)
    {
        if (expected.objective != QuestFields::TAKE ||
            std::find(quests.begin(), quests.end(), expected.quest) != quests.end() ||
            std::none_of(run.open.begin(), run.open.end(), [&expected](QuestOrderOpen const& need)
                         { return need.left && need.quest == expected.quest && need.objective == QuestFields::TAKE; }))
            continue;
        if (atGiver(expected.quest) && mayTake(expected.quest))
            quests.push_back(expected.quest);
    }
    return quests;
}

// A field order at a point (the test order `route field`): the point is the place, its cell the field, the expected
// needs those ExpectHandIns and Expect find (hand-ins first); done when they are, else the stay's minutes. The caller
// sets the walk
inline QuestOrder FieldOrder(FieldMap const& map, std::span<QuestFields::Need const> needs, std::uint32_t mapId,
                             float x, float y, float minutes)
{
    QuestOrder order;
    order.word = QuestOrderWord::Field;
    order.end = QuestOrderEnd::Done;
    order.mapId = mapId;
    order.x = x;
    order.y = y;
    order.cell = FieldCellKeyAt(mapId, x, y);
    order.expected = ExpectHandIns(map, needs, mapId, x, y);
    std::vector<QuestOrderNeed> const objectives = Expect(map, needs, mapId, x, y);
    order.expected.insert(order.expected.end(), objectives.begin(), objectives.end());
    order.minutes = minutes;
    return order;
}

// WQ20: a way to a point (the test order `route way`): nothing expected, the end its arrival, the allowance in yards
// for what the bot's log needs on the way (OnOrder). The caller sets the walk
inline QuestOrder WayOrder(std::uint32_t mapId, float x, float y, float allowance)
{
    QuestOrder order;
    order.word = QuestOrderWord::Way;
    order.end = QuestOrderEnd::Arrived;
    order.mapId = mapId;
    order.x = x;
    order.y = y;
    order.cell = FieldCellKeyAt(mapId, x, y);
    order.allowanceYards = std::max(allowance, 0.0f);
    order.minutes = WAY_MINUTES;
    return order;
}

// A quest's progress counted again: the rise since its mark is credits (returned and kept as gained); a fall (the
// quest left the log, its counts reset) only moves the mark
inline std::uint32_t Note(std::vector<QuestOrderMark>& marks, std::uint32_t quest, std::uint32_t progress)
{
    auto it =
        std::find_if(marks.begin(), marks.end(), [quest](QuestOrderMark const& mark) { return mark.quest == quest; });
    if (it == marks.end())
    {
        marks.push_back({quest, progress, 0});
        return 0;
    }
    std::uint32_t const rise = progress > it->progress ? progress - it->progress : 0;
    it->progress = progress;
    it->gained += rise;
    return rise;
}

// How many quests the order's credits belong to
inline std::uint32_t QuestsCredited(std::span<QuestOrderMark const> marks)
{
    return static_cast<std::uint32_t>(
        std::count_if(marks.begin(), marks.end(), [](QuestOrderMark const& mark) { return mark.gained > 0; }));
}

// The QuestOrder row's amount: place credits, way credits and minutes (12 bits each, capped) and the place's cell
// (10 bits a coordinate, two's complement); never negative
inline std::int64_t RowAmount(std::uint32_t placeCredits, std::uint32_t wayCredits, std::uint32_t minutes,
                              std::uint64_t cell)
{
    auto const count = [](std::uint32_t n) { return static_cast<std::uint64_t>(std::min(n, ROW_COUNT_MAX)); };
    auto const coordinate = [](std::int32_t c)
    { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(c)) & ROW_CELL_MASK; };
    return static_cast<std::int64_t>(count(placeCredits) | count(wayCredits) << 12 | count(minutes) << 24 |
                                     coordinate(FieldCellXOf(cell)) << 36 | coordinate(FieldCellYOf(cell)) << 46);
}

// WQ38: the QuestStall row's amount: the stall sum's whole minutes (12 bits, capped) and the order's cell (10 bits a
// coordinate, two's complement, as RowAmount's), minutes | x << 12 | y << 22; never negative
inline std::int64_t StallRowAmount(std::uint32_t minutes, std::uint64_t cell)
{
    auto const coordinate = [](std::int32_t c)
    { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(c)) & ROW_CELL_MASK; };
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(std::min(minutes, ROW_COUNT_MAX)) |
                                     coordinate(FieldCellXOf(cell)) << 12 | coordinate(FieldCellYOf(cell)) << 22);
}

// The QuestOrder row's source: the order's word and its end, "field:done"
inline char const* RowSource(QuestOrderWord word, QuestOrderEnd end)
{
    static constexpr char const* SOURCES[3][7] = {
        {"field:done", "field:arrived", "field:time", "field:dry", "field:replaced", "field:no_ender", "field:bags"},
        {"hand_in:done", "hand_in:arrived", "hand_in:time", "hand_in:dry", "hand_in:replaced", "hand_in:no_ender",
         "hand_in:bags"},
        {"way:done", "way:arrived", "way:time", "way:dry", "way:replaced", "way:no_ender", "way:bags"}};
    std::size_t const w = std::min<std::size_t>(static_cast<std::size_t>(word), 2);
    std::size_t const e = std::min<std::size_t>(static_cast<std::size_t>(end), 6);
    return SOURCES[w][e];
}
}  // namespace QuestFieldExec
}  // namespace Planner

#endif
