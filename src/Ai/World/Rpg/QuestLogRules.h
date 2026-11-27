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
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ClassQuestRules.h"
#include "PursuitRules.h"
#include "QuestFailBook.h"
#include "QuestTakeRules.h"

// The quest log's one accept and one drop (questing WQ34, decision WQ.46 items 2-5; before it WQ31, WQ.40), asked
// for every bot the plan covers, on an order or not, on every way a quest enters its log (a giver, a start item, a
// shared quest, a party's escort). The accept: the game lets the bot take the quest, it is not grey by the game's
// colour line, not daily, weekly or repeatable, its holiday has not ended and the bot has not given it up. Nothing
// else is asked: not the quest's kind, not whether the executor can do it, not room (WQ40). The drop: a quest that is
// not complete leaves the log when it is grey, its holiday has ended or the game marks it failed; a complete one never
// does. The two cannot disagree: whatever the drop reads, the accept refuses. What a held quest is worth is the
// plan's valuation, not the log's. The plan is due once the givers in reach offer nothing more or PLAN_DUE_MS after
// the round's first accept. Pure; tests/quest-log-selftest.cpp checks it
namespace Planner
{
namespace QuestLog
{
constexpr std::uint32_t PLAN_DUE_MS = 30 * 1000;  // from the round's first accept
// The failure row of a planned take that ended without the accept (WQ18c, WQ.44 item 2): one give-up, as any cause
constexpr std::string_view CANNOT_TAKE = "cannot_take";
// The quests the accept refuses by their kind: daily, weekly, repeatable (WQ.46 item 5). A holiday's quest is not one
constexpr std::uint16_t REPEATS = QuestFlag::DAILY | QuestFlag::WEEKLY | QuestFlag::REPEATABLE;

// Grey by the game's colour line: at or below the bot's grey level (Acore::XP::GetGrayLevel, which the caller reads).
// A quest without a level (0 or below) is of the bot's own level and never grey
inline bool IsGrey(std::int32_t questLevel, std::int32_t grayLevel)
{
    return questLevel > 0 && questLevel <= grayLevel;
}

// The bot's failure record as the accept and the plan read it
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

// A quest offered to the bot, as the accept reads it
struct Offer
{
    std::uint32_t id{0};
    std::int32_t level{0};     // the quest's level; 0 or below: the bot's
    bool repeats{false};       // daily, weekly or repeatable (REPEATS)
    bool holidayEnded{false};  // a holiday's quest whose holiday is not running
    bool gameAllows{false};    // the game's answer: the core's CanTakeQuest on the bot's thread (WQ41)
};

inline Offer OfferOf(QuestRecord const& quest, bool gameAllows, bool holidayEnded)
{
    return {.id = quest.id,
            .level = quest.level,
            .repeats = (quest.flags & REPEATS) != 0,
            .holidayEnded = holidayEnded,
            .gameAllows = gameAllows};
}

// The bot's facts for the accept
struct Facts
{
    std::int32_t grayLevel{0};               // Acore::XP::GetGrayLevel of the bot's level
    std::span<std::uint32_t const> givenUp;  // sorted: Record::givenUp
};

// Why the accept refuses a quest, in the order it asks: the first that holds
enum class Refusal : std::uint8_t
{
    None,
    Game,
    Grey,
    Repeats,
    HolidayEnded,
    GivenUp,  // the give-up count bounds the retakes (WQ.33 item 3, WQ.46 item 5)
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Refusal::Count)> REFUSALS = {
    "none", "game", "grey", "repeats", "holiday_ended", "given_up"};

inline char const* Word(Refusal refusal) { return REFUSALS[static_cast<std::size_t>(refusal)]; }

inline Refusal Refuses(Offer const& quest, Facts const& bot)
{
    if (!quest.gameAllows)
        return Refusal::Game;
    if (IsGrey(quest.level, bot.grayLevel))
        return Refusal::Grey;
    if (quest.repeats)
        return Refusal::Repeats;
    if (quest.holidayEnded)
        return Refusal::HolidayEnded;
    if (std::binary_search(bot.givenUp.begin(), bot.givenUp.end(), quest.id))
        return Refusal::GivenUp;
    return Refusal::None;
}

// The one accept: every way into the log asks it
inline bool Accepts(Offer const& quest, Facts const& bot) { return Refuses(quest, bot) == Refusal::None; }

// A quest in the bot's log, as Abandon reads it
struct Held
{
    std::uint32_t id{0};
    std::int32_t level{0};     // the quest's level; 0 or below: the bot's
    bool ready{false};         // complete, to be handed in
    bool failed{false};        // the game marks it failed (QUEST_STATUS_FAILED)
    bool holidayEnded{false};  // a holiday's quest whose holiday is not running
};

enum class Drop : std::uint8_t
{
    Keep,
    Grey,
    HolidayEnded,
    Failed,
    Count
};
constexpr std::array<char const*, static_cast<std::size_t>(Drop::Count)> WORDS = {"keep", "grey", "holiday_ended",
                                                                                  "failed"};

inline char const* Word(Drop drop) { return WORDS[static_cast<std::size_t>(drop)]; }

// The one drop: a quest that is not complete leaves the log when it is grey, its holiday has ended or the game marks
// it failed; the first that holds names the drop. A complete quest is handed in, even a grey one
inline Drop Abandon(Held const& quest, std::int32_t grayLevel)
{
    if (quest.ready)
        return Drop::Keep;
    if (IsGrey(quest.level, grayLevel))
        return Drop::Grey;
    if (quest.holidayEnded)
        return Drop::HolidayEnded;
    if (quest.failed)
        return Drop::Failed;
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

// After an accept the plan is due when no giver in reach still offers a quest Accepts passes, or PLAN_DUE_MS after
// the round's first accept. Clock wrap-safe (getMSTime)
inline bool PlanDue(Round const& round, std::uint32_t nowMs, bool giverStillOffers)
{
    return round.open && (!giverStillOffers || nowMs - round.firstAcceptMs >= PLAN_DUE_MS);
}
}  // namespace QuestLog
}  // namespace Planner

#endif
