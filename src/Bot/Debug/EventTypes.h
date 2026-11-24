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
    DeathScene,  // beside the Death row of a bot (awareness AW5)
    Equip,
    Fight,
    FightEnemy,  // a unit that entered a bot's fight (awareness AW5)
    FightMore,   // directly after its Fight row, the same src (combat-foundation MEASURES §2.1)
    FightScene,  // a bot's fight began (awareness AW5)
    FishTrip,
    Flee,
    Flight,
    FlightFall,
    GhostWait,
    GoalStalled,
    GoldIn,
    GoldOut,
    Hearth,
    Inventory,
    ItemIn,
    ItemOut,
    ItemUse,
    Kill,
    Level,
    Logout,
    MailVisit,
    MapChange,
    Mark,  // a test's console mark (combat-foundation CFM3b)
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
    // One row per honesty kind (HonestyKind, below): the word is "Honesty" + the kind's word. A shortcut site passes
    // the kind to EventLog::Honesty, never a type of these.
    HonestyFood,
    HonestyAmmo,
    HonestyReagent,
    HonestyPoison,
    HonestyConsumable,
    HonestyRepair,
    HonestyResurrect,
    HonestyTrainerSpell,
    HonestyWeaponSkill,
    HonestyRiding,
    HonestyMount,
    HonestySpecialSpell,
    HonestyClassQuest,
    HonestyPet,
    HonestyGlyph,
    HonestyKey,
    HonestyReputation,
    HonestyAttunement,
    HonestyGold,
    HonestyTalent,
    HonestyProfession,
    HonestyItem,
    HonestyTeleport,
    HonestyFlight,
    HonestyItemUse,
    Unknown  // no type: a word the table lacks (the word overloads) is written as a row and read by no one; a typed
             // Emit of it writes nothing
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
    {EventType::DeathScene, "DeathScene", ROW_ONLY},
    {EventType::Equip, "Equip", STARTER_RECORD_ROW},
    {EventType::Fight, "Fight", ROW_ONLY},
    {EventType::FightEnemy, "FightEnemy", ROW_ONLY},
    {EventType::FightMore, "FightMore", ROW_ONLY},
    {EventType::FightScene, "FightScene", ROW_ONLY},
    {EventType::FishTrip, "FishTrip", ROW_ONLY},
    {EventType::Flee, "Flee", ROW_ONLY},
    {EventType::Flight, "Flight", ROW_ONLY},
    {EventType::FlightFall, "FlightFall", ROW_ONLY},
    {EventType::GhostWait, "GhostWait", ROW_ONLY},
    {EventType::GoalStalled, "GoalStalled", ROW_ONLY},
    {EventType::GoldIn, "GoldIn", ROW_ONLY},
    {EventType::GoldOut, "GoldOut", ROW_ONLY},
    {EventType::Hearth, "Hearth", ROW_ONLY},
    {EventType::Inventory, "Inventory", ROW_ONLY},
    {EventType::ItemIn, "ItemIn", ROW_ONLY},
    {EventType::ItemOut, "ItemOut", ROW_ONLY},
    {EventType::ItemUse, "ItemUse", ROW_ONLY},
    {EventType::Kill, "Kill", STARTER_RECORD_ROW | READ_RECORDER},
    {EventType::Level, "Level", STARTER_RECORD_ROW},
    {EventType::Logout, "Logout", ROW_ONLY},
    {EventType::MailVisit, "MailVisit", ROW_ONLY},
    {EventType::MapChange, "MapChange", RECORD_ROW},
    {EventType::Mark, "Mark", ROW_ONLY},
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
    {EventType::HonestyFood, "HonestyFood", RECORDER_ROW},
    {EventType::HonestyAmmo, "HonestyAmmo", RECORDER_ROW},
    {EventType::HonestyReagent, "HonestyReagent", RECORDER_ROW},
    {EventType::HonestyPoison, "HonestyPoison", RECORDER_ROW},
    {EventType::HonestyConsumable, "HonestyConsumable", RECORDER_ROW},
    {EventType::HonestyRepair, "HonestyRepair", RECORDER_ROW},
    {EventType::HonestyResurrect, "HonestyResurrect", RECORDER_ROW},
    {EventType::HonestyTrainerSpell, "HonestyTrainerSpell", RECORDER_ROW},
    {EventType::HonestyWeaponSkill, "HonestyWeaponSkill", RECORDER_ROW},
    {EventType::HonestyRiding, "HonestyRiding", RECORDER_ROW},
    {EventType::HonestyMount, "HonestyMount", RECORDER_ROW},
    {EventType::HonestySpecialSpell, "HonestySpecialSpell", RECORDER_ROW},
    {EventType::HonestyClassQuest, "HonestyClassQuest", RECORDER_ROW},
    {EventType::HonestyPet, "HonestyPet", RECORDER_ROW},
    {EventType::HonestyGlyph, "HonestyGlyph", RECORDER_ROW},
    {EventType::HonestyKey, "HonestyKey", RECORDER_ROW},
    {EventType::HonestyReputation, "HonestyReputation", RECORDER_ROW},
    {EventType::HonestyAttunement, "HonestyAttunement", RECORDER_ROW},
    {EventType::HonestyGold, "HonestyGold", RECORDER_ROW},
    {EventType::HonestyTalent, "HonestyTalent", RECORDER_ROW},
    {EventType::HonestyProfession, "HonestyProfession", RECORDER_ROW},
    {EventType::HonestyItem, "HonestyItem", RECORDER_ROW},
    {EventType::HonestyTeleport, "HonestyTeleport", RECORDER_ROW},
    {EventType::HonestyFlight, "HonestyFlight", RECORDER_ROW},
    {EventType::HonestyItemUse, "HonestyItemUse", RECORDER_ROW},
    {EventType::Unknown, "", ROW_ONLY},
}};

constexpr std::string_view HONESTY_WORD = "Honesty";

// The kinds of honesty tags: what a shortcut site (a handout, a cheat, a teleport) gave a bot. One vocabulary: the
// upkeep's kinds (UpkeepKind, in its order) and the shortcuts that are no upkeep. A kind's row type is
// TypeOf(kind), the word of that row "Honesty" + the kind's word.
enum class HonestyKind : uint8_t
{
    Food,
    Ammo,
    Reagent,
    Poison,
    Consumable,
    Repair,
    Resurrect,
    TrainerSpell,
    WeaponSkill,
    Riding,
    Mount,
    SpecialSpell,
    ClassQuest,
    Pet,
    Glyph,
    Key,
    Reputation,
    Attunement,
    Gold,
    Talent,
    Profession,
    Item,
    Teleport,
    Flight,
    ItemUse,
};

struct HonestyRow
{
    HonestyKind Kind;
    std::string_view Word;
};

constexpr size_t HONESTY_KIND_COUNT = static_cast<size_t>(HonestyKind::ItemUse) + 1;
constexpr size_t FIRST_HONESTY_TYPE = static_cast<size_t>(EventType::HonestyFood);

// In enum order
constexpr std::array<HonestyRow, HONESTY_KIND_COUNT> HONESTY_KINDS = {{
    {HonestyKind::Food, "Food"},
    {HonestyKind::Ammo, "Ammo"},
    {HonestyKind::Reagent, "Reagent"},
    {HonestyKind::Poison, "Poison"},
    {HonestyKind::Consumable, "Consumable"},
    {HonestyKind::Repair, "Repair"},
    {HonestyKind::Resurrect, "Resurrect"},
    {HonestyKind::TrainerSpell, "TrainerSpell"},
    {HonestyKind::WeaponSkill, "WeaponSkill"},
    {HonestyKind::Riding, "Riding"},
    {HonestyKind::Mount, "Mount"},
    {HonestyKind::SpecialSpell, "SpecialSpell"},
    {HonestyKind::ClassQuest, "ClassQuest"},
    {HonestyKind::Pet, "Pet"},
    {HonestyKind::Glyph, "Glyph"},
    {HonestyKind::Key, "Key"},
    {HonestyKind::Reputation, "Reputation"},
    {HonestyKind::Attunement, "Attunement"},
    {HonestyKind::Gold, "Gold"},
    {HonestyKind::Talent, "Talent"},
    {HonestyKind::Profession, "Profession"},
    {HonestyKind::Item, "Item"},
    {HonestyKind::Teleport, "Teleport"},
    {HonestyKind::Flight, "Flight"},
    {HonestyKind::ItemUse, "ItemUse"},
}};

constexpr EventType TypeOf(HonestyKind kind)
{
    return static_cast<EventType>(FIRST_HONESTY_TYPE + static_cast<size_t>(kind));
}

// The kind's word ("Other" for a value out of range)
constexpr std::string_view HonestyKindWord(HonestyKind kind)
{
    size_t const index = static_cast<size_t>(kind);
    return index < HONESTY_KIND_COUNT ? HONESTY_KINDS[index].Word : std::string_view("Other");
}

constexpr TypeRow const& RowOf(EventType type)
{
    size_t const index = static_cast<size_t>(type);
    return TYPES[index < EVENT_TYPE_COUNT ? index : EVENT_TYPE_COUNT - 1];
}

// The type's word ("" for Unknown)
constexpr std::string_view Word(EventType type) { return RowOf(type).Word; }

constexpr bool ReadBy(EventType type, ReaderMask reader) { return (RowOf(type).Readers & reader) != 0; }

// The type of a word (the word overloads only; a linear scan): a word the table lacks is Unknown
constexpr EventType FromWord(std::string_view word)
{
    if (word.empty())
        return EventType::Unknown;

    for (TypeRow const& row : TYPES)
        if (row.Word == word)
            return row.Type;

    return EventType::Unknown;
}

// The kind of a word (the word overload of EventLog::Honesty only)
constexpr bool HonestyKindFromWord(std::string_view word, HonestyKind& kind)
{
    for (HonestyRow const& row : HONESTY_KINDS)
        if (row.Word == word)
        {
            kind = row.Kind;
            return true;
        }

    return false;
}

// Each kind's row is in the type table under its word
constexpr bool HonestyRowsMatch()
{
    for (size_t index = 0; index < HONESTY_KIND_COUNT; ++index)
    {
        HonestyRow const& kind = HONESTY_KINDS[index];
        TypeRow const& row = TYPES[FIRST_HONESTY_TYPE + index];
        if (static_cast<size_t>(kind.Kind) != index || row.Type != TypeOf(kind.Kind) ||
            row.Word.substr(0, HONESTY_WORD.size()) != HONESTY_WORD ||
            row.Word.substr(HONESTY_WORD.size()) != kind.Word)
            return false;
    }

    return FIRST_HONESTY_TYPE + HONESTY_KIND_COUNT + 1 == EVENT_TYPE_COUNT;
}

static_assert(HonestyRowsMatch(), "a type row per honesty kind, \"Honesty\" + the kind's word, in order");
}  // namespace EventLog

#endif
