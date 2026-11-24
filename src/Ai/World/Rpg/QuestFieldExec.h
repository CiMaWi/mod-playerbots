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
#include <vector>

#include "FieldMap.h"
#include "QuestFields.h"
#include "QuestOrder.h"
#include "StatusEndTypes.h"

// The executor on an order (questing plan WQ19a, PLAN 3.10 part 4, WQ.43 points 5 and 6): at the order's place the
// bot works every open need of its log that it finds, and the order ends by its own end. Pick chooses among the
// candidates the looks found (one look per kind covers every open need's entries), End names the end a tick finds.
// An order writes no failure row for a target that is not there: its ends map to outcomes other than Abandoned.
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

// The bot as Pick asks of it: where it stands, its level and its caution. WQ20's detour adds the way's allowance
struct PickAsk
{
    float x{0.0f};
    float y{0.0f};
    std::uint8_t level{1};
    float caution{NEUTRAL_CAUTION};
};

// A game time as the run keeps it: 0 means "not yet", so a stamp taken at 0 is 1
inline std::uint32_t Stamp(std::uint32_t now) { return now ? now : 1; }

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

// Whether a candidate is one the order takes: its need open, within reach, in the field
inline bool Takes(Candidate const& candidate, std::span<QuestOrderOpen const> open, QuestOrder const& order,
                  PickAsk const& ask)
{
    return candidate.need < open.size() && open[candidate.need].left > 0 &&
           (candidate.kind == FieldKind::Object || LevelWithinReach(candidate.level, ask.level, ask.caution)) &&
           InField(order.cell, candidate.mapId, candidate.x, candidate.y);
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
// A newer order first; then the plan's end (done: an order that expects needs and has none open; arrived: a way at its
// place); then the stay (from the arrival; without one, twice the planned walk plus the stay from the start); then dry
inline std::optional<QuestOrderEnd> End(QuestOrderRun const& run, std::uint32_t now, bool finding)
{
    if (run.replaced)
        return QuestOrderEnd::Replaced;
    QuestOrder const& order = run.order;
    if (order.end == QuestOrderEnd::Done && !order.expected.empty() && !ExpectedOpen(order, run.open))
        return QuestOrderEnd::Done;
    if (order.end == QuestOrderEnd::Arrived && run.arrivedAt)
        return QuestOrderEnd::Arrived;
    if (!run.arrivedAt)
    {
        float const bound = std::min(WALK_SLACK * order.walkMinutes + order.minutes, MAX_MINUTES);
        if (run.startedAt && now - run.startedAt >= MinutesMs(bound))
            return QuestOrderEnd::Time;
        return std::nullopt;
    }
    std::uint32_t const atPlace = now - run.arrivedAt;
    if (atPlace >= MinutesMs(order.minutes))
        return QuestOrderEnd::Time;
    std::uint32_t const quiet = run.lastFound ? std::min(atPlace, now - run.lastFound) : atPlace;
    if (!finding && quiet >= LOOK_MS)
        return QuestOrderEnd::Dry;
    return std::nullopt;
}

// The status end of an order's end: never Abandoned, which would write a failure row (NewRpgStatusEnd.cpp's door).
// done finishes; time and dry end by the clock; a way arrives; a newer order ends it as an order does
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
            return StatusEnd::Outcome::TimedOut;
        case QuestOrderEnd::Replaced:
            return StatusEnd::Outcome::Ordered;
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

// What an order at a point expects: every open objective of the log (no hand-in, no take: WQ19b's) with a source that
// has a row in the field around the point, all its credits
inline std::vector<QuestOrderNeed> Expect(FieldMap const& map, std::span<QuestFields::Need const> needs,
                                          std::uint32_t mapId, float x, float y)
{
    std::vector<std::uint64_t> const cells = map.Around(mapId, x, y);
    std::vector<QuestOrderNeed> expected;
    for (QuestFields::Need const& need : needs)
    {
        if (!need.left || need.objective == QuestFields::HAND_IN || need.objective == QuestFields::TAKE)
            continue;
        bool const there = std::any_of(
            need.sources.begin(), need.sources.end(),
            [&map, &cells](QuestFields::NeedSource const& source)
            {
                return std::any_of(
                    cells.begin(), cells.end(), [&map, &source](std::uint64_t cell)
                    { return QuestFields::Detail::FindRow(map, cell, source.kind, source.entry) != nullptr; });
            });
        if (there)
            expected.push_back({need.quest, need.objective, need.left});
    }
    return expected;
}

// A field order at a point (the test order `route field`): the point is the place, its cell the field, the expected
// needs those Expect finds; done when they are, else the stay's minutes. The caller sets the walk
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
    order.expected = Expect(map, needs, mapId, x, y);
    order.minutes = minutes;
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

// The QuestOrder row's source: the order's word and its end, "field:done"
inline char const* RowSource(QuestOrderWord word, QuestOrderEnd end)
{
    static constexpr char const* SOURCES[3][5] = {
        {"field:done", "field:arrived", "field:time", "field:dry", "field:replaced"},
        {"hand_in:done", "hand_in:arrived", "hand_in:time", "hand_in:dry", "hand_in:replaced"},
        {"way:done", "way:arrived", "way:time", "way:dry", "way:replaced"}};
    std::size_t const w = std::min<std::size_t>(static_cast<std::size_t>(word), 2);
    std::size_t const e = std::min<std::size_t>(static_cast<std::size_t>(end), 4);
    return SOURCES[w][e];
}
}  // namespace QuestFieldExec
}  // namespace Planner

#endif
