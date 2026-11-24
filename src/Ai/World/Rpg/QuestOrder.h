/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTORDER_H
#define _PLAYERBOT_QUESTORDER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// The order a quest route gives a bot (questing plan WQ17c, PLAN 3.10 part 4, WQ.40 and WQ.43): a place, the needs
// the plan expects there, an allowance in yards for the way, and an end. The executor works every open need of the
// bot's log it finds at the place or within the allowance on the way; the expected needs only decide when the order
// is done. Its cell and its word are its identity (the block of a dry field is keyed by the cell). The planner fills
// it (QuestRoute.h), the executor and the tests read it. No game header and no other planner header: NewRpgInfo.h
// includes this file
namespace Planner
{
constexpr std::uint32_t ORDER_HAND_IN = 0xFFFFFFFFu;  // QuestOrderNeed::objective of a hand-in (QuestFields::HAND_IN)
constexpr std::uint32_t ORDER_TAKE = 0xFFFFFFFEu;     // and of a quest taken at its giver (QuestFields::TAKE)

// What the plan expects at the place, as the log's word
enum class QuestOrderWord : std::uint8_t
{
    Field = 0,   // credits of objectives, with whatever is handed in or taken there
    HandIn = 1,  // only hand-ins and takes at their NPCs
    Way = 2      // a way to walk: nothing expected but the arrival
};

// How an order ends. A plan sets Done or Arrived: what ends the order before its minutes pass. The executor names
// the end it found with any of them
enum class QuestOrderEnd : std::uint8_t
{
    Done = 0,      // the expected needs are done
    Arrived = 1,   // the bot is at the place (a way)
    Time = 2,      // the minutes passed
    Dry = 3,       // the place gave nothing
    Replaced = 4,  // a newer route gave another order
    NoEnder = 5    // WQ19b: at the place the look time passed with an expected hand-in not handed in (its failure row)
};

constexpr char const* QuestOrderWordName(QuestOrderWord word)
{
    switch (word)
    {
        case QuestOrderWord::HandIn:
            return "hand_in";
        case QuestOrderWord::Way:
            return "way";
        default:
            return "field";
    }
}

constexpr char const* QuestOrderEndName(QuestOrderEnd end)
{
    switch (end)
    {
        case QuestOrderEnd::Arrived:
            return "arrived";
        case QuestOrderEnd::Time:
            return "time";
        case QuestOrderEnd::Dry:
            return "dry";
        case QuestOrderEnd::Replaced:
            return "replaced";
        case QuestOrderEnd::NoEnder:
            return "no_ender";
        default:
            return "done";
    }
}

// One need the plan expects at the place: the quest and its objective (QuestExec::ObjectiveAt's index), or the
// hand-in or take mark
struct QuestOrderNeed
{
    std::uint32_t quest{0};
    std::uint32_t objective{ORDER_HAND_IN};
    std::uint32_t credits{0};  // the credits the plan took off the need for this stop; 0: less than one expected

    bool operator==(QuestOrderNeed const& other) const { return quest == other.quest && objective == other.objective; }
};

struct QuestOrder
{
    QuestOrderWord word{QuestOrderWord::Field};
    QuestOrderEnd end{QuestOrderEnd::Done};
    std::uint32_t mapId{0};
    float x{0.0f};  // the place: the middle of its cell
    float y{0.0f};
    std::uint64_t cell{0};                 // FieldMap.h's cell key of the place
    std::vector<QuestOrderNeed> expected;  // hand-ins first
    float allowanceYards{0.0f};            // how far off the way and the place the bot works what it finds
    float walkMinutes{0.0f};               // the plan's walk to the place from where the order before it ends
    float minutes{0.0f};                   // the stay: the order ends when it has been at the place this long
    float worthPerMinute{0.0f};            // what the plan expects of the stay
    float leaveBelow{0.0f};                // the worth per minute below which leaving for the next order pays
};

// Whether a refreshed route's order is the running one: the same place and the same word
inline bool SameOrder(QuestOrder const& a, QuestOrder const& b) { return a.cell == b.cell && a.word == b.word; }

// WQ19a: one open need of the bot's log as the executor counted it (QuestFields::Need): the quest, the objective
// (QuestExec::ObjectiveAt's index, or the hand-in or take mark) and the credits left
struct QuestOrderOpen
{
    std::uint32_t quest{0};
    std::uint32_t objective{ORDER_HAND_IN};
    std::uint32_t left{0};
};

// WQ19a: one entry the executor's look seeks for an open need; one look per kind (creatures, objects) covers the
// entries of every open need
struct QuestOrderSeek
{
    std::uint32_t entry{0};
    std::uint32_t need{0};  // into QuestOrderRun::open
    bool object{false};
};

// WQ19a: a quest's progress (the sum of its counts) when last counted, and the credits it gained over the order
struct QuestOrderMark
{
    std::uint32_t quest{0};
    std::uint32_t progress{0};
    std::uint32_t gained{0};
};

constexpr std::size_t ORDER_CREDIT_STAMPS = 16;  // WQ23: the last credits' times kept for the rate

// WQ19a: the executor's state of a running order (NewRpgInfo::DoQuest::order; QuestFieldExec.h reads it). Times are
// the game's millisecond stamps (getMSTime: they wrap, compared as differences), 0 = not yet. The fields of batches F2
// and F3 are all here, so that NewRpgInfo.h is not touched again; a field a later step fills names it and stays zero
// until then
struct QuestOrderRun
{
    QuestOrder order;
    std::uint32_t startedAt{0};
    std::uint32_t arrivedAt{0};         // the bot came within the arrival yards of the place
    std::uint32_t lastFound{0};         // a candidate it may take was seen or a credit came: the dry clock restarts
    std::uint32_t lastCount{0};         // the open needs were counted: the start and after each credit
    bool replaced{false};               // a newer order asks for the bot: the next tick ends this one
    std::vector<QuestOrderOpen> open;   // the log's open needs at the last count
    std::vector<QuestOrderSeek> seeks;  // what the looks seek, for those needs
    std::vector<QuestOrderMark> marks;  // per quest of the log: the credits are the rises of its progress
    std::uint32_t placeCredits{0};      // credits after the arrival
    std::uint32_t wayCredits{0};        // WQ20: credits before it, within the allowance on the way
    std::uint16_t handIns{0};           // WQ19b: quests handed in on the order
    std::uint16_t takes{0};             // WQ31: quests taken on the order
    std::uint16_t refused{0};           // WQ25: candidates left by the judgement of a target
    std::uint32_t lastLook{0};          // WQ22: the far look's last time and counts
    std::uint16_t lookAlive{0};
    std::uint16_t lookTapped{0};
    std::uint16_t lookObjects{0};
    std::uint16_t lookPlayers{0};
    std::array<std::uint32_t, ORDER_CREDIT_STAMPS> creditAt{};  // WQ23: the last credits' times, a ring
    std::uint8_t creditNext{0};
};
}  // namespace Planner

#endif
