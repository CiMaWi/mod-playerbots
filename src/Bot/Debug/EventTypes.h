/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_EVENTTYPES_H
#define _PLAYERBOT_EVENTTYPES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// The world log's event types: one row per type with its word and the readers that take it (game-free; selftests
// compile it). A new type is a row here; the readers find their types by the row, not by a list of their own.
namespace EventLog
{
enum class EventType : uint8_t
{
    Aboard,
    BankVisit,
    BridgeAct,
    BridgeOwnLine,
    CannedLine,
    Constraint,
    CraftVisit,
    Death,
    DeathReturn,
    Equip,
    FishTrip,
    Flee,
    Flight,
    FlightFall,
    GhostWait,
    GoalStalled,
    GoldIn,
    GoldOut,
    Hearth,
    Honesty,  // the family of "Honesty" + kind rows; the row carries the whole word
    Inventory,
    ItemIn,
    ItemOut,
    ItemUse,
    Kill,
    Level,
    Logout,
    MailVisit,
    MapChange,
    MeshMiss,
    NeedShort,
    PetTamed,
    ProfessionHeld,
    ProfessionVisit,
    PvpDeath,
    PvpEnthusiasm,
    PvpKill,
    QuestAbandon,
    QuestAccept,
    QuestDone,
    QuestFail,
    QuestReady,
    Record,
    Resurrect,
    ResumeNotStarted,
    RidingBought,
    Sick,
    SkillUp,
    SpellLearn,
    Starter,
    StarterDrop,
    Status,
    Stuck,
    SwitchNotStarted,
    TaxiNode,
    TrainerVisit,
    Transport,
    Travel,
    TravelLootDefer,
    TravelLootRelease,
    Visit,  // a starter key, not a row: the record hands a bot's first entry to the starters under it
    VisitStart,
    XpGain,
    Unknown  // a word the table lacks (the word overloads): written as a row, read by no one
};

// Who takes a type: the event row (world log on), the bots' record, the starters, the bot's recorder ring (through the
// event row, so only with the world log on)
using ReaderMask = uint8_t;
constexpr ReaderMask READ_ROW = 1 << 0;
constexpr ReaderMask READ_RECORD = 1 << 1;
constexpr ReaderMask READ_STARTER = 1 << 2;
constexpr ReaderMask READ_RECORDER = 1 << 3;

struct TypeRow
{
    EventType Type;
    std::string_view Word;
    ReaderMask Readers;
};

constexpr size_t EVENT_TYPE_COUNT = static_cast<size_t>(EventType::Unknown) + 1;
constexpr ReaderMask ROW_ONLY = READ_ROW;
constexpr ReaderMask RECORD_ROW = READ_ROW | READ_RECORD;
constexpr ReaderMask STARTER_RECORD_ROW = READ_ROW | READ_RECORD | READ_STARTER;
constexpr ReaderMask RECORDER_ROW = READ_ROW | READ_RECORDER;

// In enum order
constexpr std::array<TypeRow, EVENT_TYPE_COUNT> TYPES = {{
    {EventType::Aboard, "Aboard", ROW_ONLY},
    {EventType::BankVisit, "BankVisit", ROW_ONLY},
    {EventType::BridgeAct, "BridgeAct", ROW_ONLY},
    {EventType::BridgeOwnLine, "BridgeOwnLine", ROW_ONLY},
    {EventType::CannedLine, "CannedLine", ROW_ONLY},
    {EventType::Constraint, "Constraint", ROW_ONLY},
    {EventType::CraftVisit, "CraftVisit", ROW_ONLY},
    {EventType::Death, "Death", RECORD_ROW | READ_RECORDER},
    {EventType::DeathReturn, "DeathReturn", ROW_ONLY},
    {EventType::Equip, "Equip", STARTER_RECORD_ROW},
    {EventType::FishTrip, "FishTrip", ROW_ONLY},
    {EventType::Flee, "Flee", ROW_ONLY},
    {EventType::Flight, "Flight", ROW_ONLY},
    {EventType::FlightFall, "FlightFall", ROW_ONLY},
    {EventType::GhostWait, "GhostWait", ROW_ONLY},
    {EventType::GoalStalled, "GoalStalled", ROW_ONLY},
    {EventType::GoldIn, "GoldIn", ROW_ONLY},
    {EventType::GoldOut, "GoldOut", ROW_ONLY},
    {EventType::Hearth, "Hearth", ROW_ONLY},
    {EventType::Honesty, "Honesty", RECORDER_ROW},
    {EventType::Inventory, "Inventory", ROW_ONLY},
    {EventType::ItemIn, "ItemIn", ROW_ONLY},
    {EventType::ItemOut, "ItemOut", ROW_ONLY},
    {EventType::ItemUse, "ItemUse", ROW_ONLY},
    {EventType::Kill, "Kill", STARTER_RECORD_ROW | READ_RECORDER},
    {EventType::Level, "Level", STARTER_RECORD_ROW},
    {EventType::Logout, "Logout", ROW_ONLY},
    {EventType::MailVisit, "MailVisit", ROW_ONLY},
    {EventType::MapChange, "MapChange", RECORD_ROW},
    {EventType::MeshMiss, "MeshMiss", ROW_ONLY},
    {EventType::NeedShort, "NeedShort", ROW_ONLY},
    {EventType::PetTamed, "PetTamed", ROW_ONLY},
    {EventType::ProfessionHeld, "ProfessionHeld", ROW_ONLY},
    {EventType::ProfessionVisit, "ProfessionVisit", ROW_ONLY},
    {EventType::PvpDeath, "PvpDeath", ROW_ONLY},
    {EventType::PvpEnthusiasm, "PvpEnthusiasm", ROW_ONLY},
    {EventType::PvpKill, "PvpKill", ROW_ONLY},
    {EventType::QuestAbandon, "QuestAbandon", ROW_ONLY},
    {EventType::QuestAccept, "QuestAccept", ROW_ONLY},
    {EventType::QuestDone, "QuestDone", STARTER_RECORD_ROW},
    {EventType::QuestFail, "QuestFail", ROW_ONLY},
    {EventType::QuestReady, "QuestReady", ROW_ONLY},
    {EventType::Record, "Record", ROW_ONLY},
    {EventType::Resurrect, "Resurrect", RECORD_ROW},
    {EventType::ResumeNotStarted, "ResumeNotStarted", ROW_ONLY},
    {EventType::RidingBought, "RidingBought", ROW_ONLY},
    {EventType::Sick, "Sick", ROW_ONLY},
    {EventType::SkillUp, "SkillUp", RECORD_ROW},
    {EventType::SpellLearn, "SpellLearn", RECORD_ROW},
    {EventType::Starter, "Starter", ROW_ONLY},
    {EventType::StarterDrop, "StarterDrop", ROW_ONLY},
    {EventType::Status, "Status", ROW_ONLY},
    {EventType::Stuck, "Stuck", RECORDER_ROW},
    {EventType::SwitchNotStarted, "SwitchNotStarted", ROW_ONLY},
    {EventType::TaxiNode, "TaxiNode", RECORD_ROW},
    {EventType::TrainerVisit, "TrainerVisit", ROW_ONLY},
    {EventType::Transport, "Transport", ROW_ONLY},
    {EventType::Travel, "Travel", RECORDER_ROW},
    {EventType::TravelLootDefer, "TravelLootDefer", ROW_ONLY},
    {EventType::TravelLootRelease, "TravelLootRelease", ROW_ONLY},
    {EventType::Visit, "Visit", READ_STARTER},
    {EventType::VisitStart, "VisitStart", ROW_ONLY},
    {EventType::XpGain, "XpGain", RECORDER_ROW},
    {EventType::Unknown, "", ROW_ONLY},
}};

constexpr std::string_view HONESTY_WORD = "Honesty";

constexpr TypeRow const& RowOf(EventType type)
{
    size_t const index = static_cast<size_t>(type);
    return TYPES[index < EVENT_TYPE_COUNT ? index : EVENT_TYPE_COUNT - 1];
}

// The type's word ("" for Unknown)
constexpr std::string_view Word(EventType type) { return RowOf(type).Word; }

constexpr bool ReadBy(EventType type, ReaderMask reader) { return (RowOf(type).Readers & reader) != 0; }

// The type of a word (the word overloads only; a linear scan): an "Honesty" + kind word is the Honesty family, a word
// the table lacks is Unknown
constexpr EventType FromWord(std::string_view word)
{
    if (word.empty())
        return EventType::Unknown;

    for (TypeRow const& row : TYPES)
        if (row.Word == word)
            return row.Type;

    if (word.substr(0, HONESTY_WORD.size()) == HONESTY_WORD)
        return EventType::Honesty;

    return EventType::Unknown;
}
}  // namespace EventLog

#endif
