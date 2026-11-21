/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_WORLDLOGTYPES_H
#define _PLAYERBOT_WORLDLOGTYPES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

// The world log's types its callers pass (a decision's kind and alternatives, an event row's place); game-free and
// without the row formats (WorldLogFormat.h), so WorldLog.h and EventLog.h hold no formatter.
namespace WorldLogFormat
{
struct Place
{
    uint32_t map = 0;
    uint32_t zone = 0;
    uint32_t area = 0;
    float x = 0.0f;
    float y = 0.0f;
};

// WL7: the decision stream. kind is one of these words (later plans add theirs, WL.9); value and the alternatives'
// values are scores or seconds as the kind says; ref is the trip id the decision belongs to, or 0.
enum class DecisionKind : uint8_t
{
    RpgStatus,
    Route,
    Trip,
    Errand,
    Inn,
    Upkeep,
    Death,
    Circuit,
    Goal,
    FightStart,  // world PvP (WP1): one row per meeting
    FightJoin,   // world PvP (WP5): one row per bystander and fight
    Mail,
    Bank,
    QuestUse,    // upkeep batch G (WU20b-WU20c): a quest item or spell used where its objective needs it
    FightLeave,  // world PvP (WP24): one row per engaged episode a bot leaves by its enthusiasm
    ItemSpend,   // item use (WI2): the spend rule's verdict on one item
    ItemBuff,    // item use (WI2): the buff picker's choice
    Profession,  // professions (WR1): learn, rank, recipe, tool, craft, gather, fish, keep, skip (ProfessionConfig.h)
    QuestExec    // questing WT35a: the quest executor's holds, pass-overs, walks and places tried
};

constexpr size_t DECISION_KIND_COUNT = 19;
constexpr std::array<std::string_view, DECISION_KIND_COUNT> DECISION_KIND_WORDS = {
    "rpg_status",  "route",      "trip",        "errand",     "inn",       "upkeep", "death",
    "circuit",     "goal",       "fight_start", "fight_join", "mail",      "bank",   "quest_use",
    "fight_leave", "item_spend", "item_buff",   "profession", "quest_exec"};

constexpr size_t DECISION_ALTERNATIVES = 3;

// One option the decision weighed: its word (a literal or a view that outlives the Decision call) and its value
struct Alternative
{
    std::string_view word;
    float value = 0.0f;
};

// Up to three alternatives in a fixed array, no allocation. Add appends while there is room; Offer keeps the three
// best seen so far, best first (the highest values, or the lowest when lowestFirst); ties keep their order.
class Alternatives
{
public:
    Alternatives() = default;
    Alternatives(std::initializer_list<Alternative> alternatives)
    {
        for (Alternative const& alternative : alternatives)
            Add(alternative);
    }

    void Add(Alternative const& alternative)
    {
        if (_count < DECISION_ALTERNATIVES)
            _items[_count++] = alternative;
    }

    void Offer(Alternative const& alternative, bool lowestFirst)
    {
        size_t at = _count;
        while (at > 0 && Better(alternative.value, _items[at - 1].value, lowestFirst))
            --at;
        if (at >= DECISION_ALTERNATIVES)
            return;

        for (size_t i = std::min(_count, DECISION_ALTERNATIVES - 1); i > at; --i)
            _items[i] = _items[i - 1];
        _items[at] = alternative;
        _count = std::min(_count + 1, DECISION_ALTERNATIVES);
    }

    size_t Count() const { return _count; }
    Alternative const& operator[](size_t index) const { return _items[index]; }

private:
    static bool Better(float value, float than, bool lowestFirst) { return lowestFirst ? value < than : value > than; }

    std::array<Alternative, DECISION_ALTERNATIVES> _items{};
    size_t _count = 0;
};
}  // namespace WorldLogFormat

#endif
