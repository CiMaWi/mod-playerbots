/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTLOGRULES_H
#define _PLAYERBOT_QUESTLOGRULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "ClassQuestRules.h"
#include "PursuitRules.h"
#include "QuestFailBook.h"
#include "QuestTakeRules.h"

// What a bot on an order does with its quest log (questing WQ31, decision WQ.40): it accepts every quest the one take
// test allows (its log has no limit: WQ40, WQ.46 item 1), abandons only a quest it outlevelled, gave up or cannot do
// (a ready one only when its hand-in failed at its ender), and its plan is due once the givers in reach offer nothing
// more or PLAN_DUE_MS after the round's first accept. Pure; tests/quest-log-selftest.cpp checks it
namespace Planner
{
namespace QuestLog
{
constexpr std::uint32_t PLAN_DUE_MS = 30 * 1000;  // from the round's first accept
// The failure row of a planned take that ended without the accept (WQ18c, WQ.44 item 2): one give-up, as any cause
constexpr std::string_view CANNOT_TAKE = "cannot_take";

// A quest a giver offers is accepted when the take test allows it: no rule for room (WQ40)
inline bool MayAccept(QuestRecord const& quest, Take::Facts const& bot, QuestIndex const& quests)
{
    return !Take::May(quest, bot, quests).has_value();
}

// The bot's failure record as the accept and the log upkeep read it
struct Record
{
    std::vector<std::uint32_t> givenUp;       // sorted; Take::Facts::givenUp
    std::vector<std::uint32_t> handInFailed;  // sorted; last given up for no ender
};

// Given up: GIVE_UP_LIMIT give-ups of any cause (a refused take too, WQ.44 item 2), or one whose last cause is a
// failed hand-in (a quest that cannot be handed in is not taken again). A later give-up overwrites the last cause
inline Record Read(std::vector<std::pair<std::uint32_t, QuestFail::Row>> const& rows)
{
    Record record;
    for (auto const& [quest, row] : rows)
    {
        bool const noEnder = row.giveUps > 0 && row.lastCause == Pursuit::NO_ENDER;
        if (row.giveUps >= ClassQuests::GIVE_UP_LIMIT || noEnder)
            record.givenUp.push_back(quest);
        if (noEnder)
            record.handInFailed.push_back(quest);
    }
    std::sort(record.givenUp.begin(), record.givenUp.end());
    std::sort(record.handInFailed.begin(), record.handInFailed.end());
    return record;
}

// A quest in the bot's log, as Abandon reads it
struct Held
{
    std::uint32_t id{0};
    std::int32_t level{0};     // the quest's level; 0 or below: the bot's
    bool ready{false};         // complete, to be handed in
    bool handInFailed{false};  // Record::handInFailed
    bool givenUp{false};       // Record::givenUp
    bool cannotDo{false};      // the capability table refuses it for the bot's side
};

inline Held HeldOf(QuestRecord const& quest, Record const& record, std::uint8_t side, bool ready)
{
    Held held;
    held.id = quest.id;
    held.level = quest.level;
    held.ready = ready;
    held.handInFailed = std::binary_search(record.handInFailed.begin(), record.handInFailed.end(), quest.id);
    held.givenUp = std::binary_search(record.givenUp.begin(), record.givenUp.end(), quest.id);
    held.cannotDo = RefusedFor(quest, side);
    return held;
}

enum class Drop : std::uint8_t
{
    Keep,
    Outlevelled,  // grey for the bot
    GivenUp,
    CannotDo,
    NoEnder,  // ready, its hand-in failed at its ender
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Drop::Count)> WORDS = {"keep", "outlevelled", "given_up",
                                                                                  "cannot_do", "no_ender"};

inline char const* Word(Drop drop) { return WORDS[static_cast<std::size_t>(drop)]; }

// Whether the bot abandons a quest of its log: the first rule that holds. Never for room
inline Drop Abandon(Held const& quest, std::uint8_t botLevel, std::uint32_t greyDiff)
{
    if (quest.ready)
        return quest.handInFailed ? Drop::NoEnder : Drop::Keep;
    std::int32_t const level = quest.level > 0 ? quest.level : static_cast<std::int32_t>(botLevel);
    if (static_cast<std::int32_t>(botLevel) > level + static_cast<std::int32_t>(greyDiff))
        return Drop::Outlevelled;
    if (quest.givenUp)
        return Drop::GivenUp;
    if (quest.cannotDo)
        return Drop::CannotDo;
    return Drop::Keep;
}

// The accepts of one round: open from the first accept until the plan is due
struct Round
{
    bool open{false};
    std::uint32_t firstAcceptMs{0};
};

inline void NoteAccept(Round& round, std::uint32_t nowMs)
{
    if (round.open)
        return;
    round.open = true;
    round.firstAcceptMs = nowMs;
}

// After an accept the plan is due when no giver in reach still offers a quest MayAccept passes, or PLAN_DUE_MS after
// the round's first accept. Clock wrap-safe (getMSTime)
inline bool PlanDue(Round const& round, std::uint32_t nowMs, bool giverStillOffers)
{
    return round.open && (!giverStillOffers || nowMs - round.firstAcceptMs >= PLAN_DUE_MS);
}
}  // namespace QuestLog
}  // namespace Planner

#endif
