/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSETYPES_H
#define _PLAYERBOT_ITEMUSETYPES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

// The shared types of the item use framework (item-use plan, INTERFACE.md 1.1 to 1.6): the fact record of one use
// effect, the request and its results, the causes of a refusal. Std only, so the selftests
// build them alone.
namespace ItemUse
{
constexpr std::size_t MAX_EFFECTS = 6;      // a use spell's three effects plus those of spells it triggers
constexpr std::size_t MAX_GROUPS = 4;       // spell groups kept per record
constexpr std::size_t ATTRIBUTE_WORDS = 8;  // Attributes, AttributesEx .. AttributesEx7
constexpr std::uint8_t SLOT_BAGS = 255;     // the item is in the bags, not worn

enum class Source : std::uint8_t
{
    ItemSpell,
    EnchantSpell
};

// What the packet's target must be. Words: none self friend enemy unit ground item object
enum class Shape : std::uint8_t
{
    None,
    Self,
    Friend,
    Enemy,
    AnyUnit,
    Ground,
    OwnItem,
    Object,
    Count
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Shape::Count)> SHAPE_WORDS = {
    "none", "self", "friend", "enemy", "unit", "ground", "item", "object"};

constexpr std::string_view ShapeWord(Shape shape)
{
    std::size_t const index = static_cast<std::size_t>(shape);
    return index < SHAPE_WORDS.size() ? SHAPE_WORDS[index] : std::string_view("none");
}

enum class Life : std::uint8_t
{
    Alive,
    Dead,
    Either
};  // what a unit target must be

enum class FightNeed : std::uint8_t
{
    Any,
    Peace,
    Fight
};  // what the item needs of the bot

enum class PriceSource : std::uint8_t
{
    None,
    Vendor,
    Derived
};  // where replaceCopper comes from

// What a long effect excludes by its kind. Words: none battle_elixir guardian_elixir flask well_fed scroll coating
// food drink
enum class Exclude : std::uint8_t
{
    None,
    BattleElixir,
    GuardianElixir,
    Flask,
    WellFed,
    Scroll,
    Coating,
    Food,
    Drink,
    Count
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Exclude::Count)> EXCLUDE_WORDS = {
    "none", "battle_elixir", "guardian_elixir", "flask", "well_fed", "scroll", "coating", "food", "drink"};

constexpr std::string_view ExcludeWord(Exclude exclude)
{
    std::size_t const index = static_cast<std::size_t>(exclude);
    return index < EXCLUDE_WORDS.size() ? EXCLUDE_WORDS[index] : std::string_view("none");
}

struct EffectFact
{
    std::uint16_t effect{0};      // the game's SpellEffects id
    std::uint16_t aura{0};        // the game's AuraType id, 0: no aura
    std::int32_t amount{0};       // the average of the effect's base points, as a tooltip shows it
    std::int32_t misc{0};         // MiscValue (stat, school mask, creature entry, enchantment id, ...)
    std::int32_t miscB{0};        // MiscValueB
    std::uint32_t periodMs{0};    // 0: at once; else the amount is per tick
    std::uint32_t trigger{0};     // the spell this effect triggers, 0: none
    std::uint32_t createItem{0};  // ItemType
    std::uint8_t targetA{0};      // the game's Targets ids
    std::uint8_t targetB{0};
    std::uint8_t mechanic{0};
    float radiusYards{0.0f};
    bool triggered{false};  // true: an effect of a spell the use spell triggers (one level deep)
};

struct Needs
{
    std::uint8_t level{0};   // the item's RequiredLevel, or the enchantment's requiredLevel
    std::uint16_t skill{0};  // RequiredSkill of the item, or requiredSkill of the enchantment
    std::uint16_t skillRank{0};
    std::uint32_t classMask{0};  // AllowableClass, 0: every class
    std::uint32_t raceMask{0};   // AllowableRace, 0: every race
    std::uint32_t requiredSpell{0};
    std::uint32_t areaGroup{0};  // the spell's AreaGroupId, 0: anywhere ("a place")
    std::uint32_t map{0};        // the item template's Map and Area, 0: anywhere
    std::uint32_t area{0};
    std::uint32_t spellFocus{0};  // RequiresSpellFocus, 0: none
    FightNeed fight{FightNeed::Any};
    bool notInArena{false};  // the item's or the spell's arena rule
    bool outdoors{false};
    bool indoors{false};
    bool notShapeshifted{false};
};

// One record per use effect: an on-use spell of an item template (ITEM_SPELLTRIGGER_ON_USE), or a use spell of an
// enchantment (ITEM_ENCHANTMENT_TYPE_USE_SPELL). Built once, immutable, trivially copyable.
struct Record
{
    // where it comes from
    Source source{Source::ItemSpell};
    std::uint32_t itemEntry{0};  // 0 for an enchantment's record: the piece that carries it is the item
    std::uint32_t enchantId{0};  // 0 for an item's own spell
    std::uint8_t spellIndex{0};  // index among the item's five spells, or among the enchantment's three effects
    std::uint32_t spellId{0};
    // the item (zero for an enchantment's record)
    std::uint8_t itemClass{0};
    std::uint8_t itemSubClass{0};
    std::uint8_t inventoryType{0};  // not 0: usable only while equipped
    std::uint16_t itemLevel{0};
    bool conjured{false};
    // the target
    Shape shape{Shape::None};
    Life life{Life::Alive};
    bool positive{true};                      // SpellInfo::IsPositive
    std::int32_t targetItemClass{-1};         // OwnItem: EquippedItemClass, -1: any
    std::uint32_t targetItemSubClassMask{0};  // 0: any
    std::uint32_t targetItemInvTypeMask{0};   // 0: any
    std::uint16_t targetItemMinLevel{0};      // the spell's BaseLevel against the target item's level
    std::uint16_t targetItemMaxLevel{0};      // the spell's MaxLevel, 0: none
    // the cast
    float minRangeYards{0.0f};
    float rangeYards{0.0f};  // against an enemy or a spot
    float rangeFriendYards{0.0f};
    float radiusYards{0.0f};  // the largest radius of its effects, 0: no area
    std::uint32_t castMs{0};
    std::uint32_t durationMs{0};
    bool channeled{false};
    bool moveInterrupts{false};
    // readiness
    std::uint32_t cooldownMs{0};
    std::uint32_t category{0};
    std::uint32_t categoryCooldownMs{0};
    bool potion{false};  // one per fight: the cooldown starts when the fight ends
    // consumption
    std::int32_t charges{0};  // 0: unlimited; below 0: the item is destroyed when they run out; above: stays
    bool consumed{false};     // a use costs an item or a charge that does not come back
    Needs needs;
    // strength, and what a classifier reads
    std::array<EffectFact, MAX_EFFECTS> effects{};
    std::uint8_t effectCount{0};
    bool effectsCut{false};  // more effects than MAX_EFFECTS existed; the census counts these
    std::array<std::uint32_t, ATTRIBUTE_WORDS> attributes{};
    std::uint32_t schoolMask{0};
    std::uint8_t dispelType{0};
    std::uint8_t spellSpecific{0};  // the game's SpellSpecificType
    // what it excludes
    std::array<std::uint32_t, MAX_GROUPS> groups{};  // spell_group ids of its first rank, ascending
    std::uint8_t groupCount{0};
    Exclude exclude{Exclude::None};
    // what it costs to replace
    std::uint32_t replaceCopper{0};  // per use; 0 for a record that is not consumed
    std::uint32_t sellCopper{0};
    PriceSource price{PriceSource::None};
};
static_assert(std::is_trivially_copyable_v<Record>, "a record is copied freely between threads");

// Who asks. Words: fight buff upkeep quest script pvp errand chat ability test
enum class Caller : std::uint8_t
{
    Fight,
    Buff,
    Upkeep,
    Quest,
    Script,
    Pvp,
    Errand,
    Chat,
    Ability,
    Test,
    Count
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Caller::Count)> CALLER_WORDS = {
    "fight", "buff", "upkeep", "quest", "script", "pvp", "errand", "chat", "ability", "test"};

constexpr std::string_view CallerWord(Caller caller)
{
    std::size_t const index = static_cast<std::size_t>(caller);
    return index < CALLER_WORDS.size() ? CALLER_WORDS[index] : std::string_view("test");
}

enum class TargetKind : std::uint8_t
{
    None,
    Self,
    Unit,
    Spot,
    OwnItem,
    Object,
    TradeSlot  // the trade window's slot of the item the other player does not trade (an enchant on it)
};

struct Target
{
    TargetKind kind{TargetKind::None};
    std::uint64_t guid{0};  // Unit, OwnItem, Object: the raw guid
    float x{0.0f};          // Spot
    float y{0.0f};
    float z{0.0f};
    bool dead{false};  // Unit: the caller means a dead unit (a quest's corpse, a dead group member)
};

struct ItemRef
{
    std::uint32_t entry{0};
    std::uint64_t guid{0};         // 0: any stack of the entry
    std::uint8_t slot{SLOT_BAGS};  // an equipment slot (0..18) for a worn piece
};

struct Request
{
    ItemRef item;
    Target target;
    Caller caller{Caller::Test};
    std::uint32_t spellId{0};  // 0: the item's first use effect; else the one meant (parity with the old packets)
};

// Causes of a refusal, in the order Check asks (INTERFACE.md 1.4). The word is the row's refused:<word>
enum class Cause : std::uint8_t
{
    None,
    NoEffect,
    NoItem,
    NotEquipped,
    Level,
    Skill,
    Class,
    CantUse,
    NotReady,
    Potion,
    NoCharges,
    Busy,
    Dead,
    NoControl,
    Flight,
    InFight,
    NotInFight,
    Arena,
    Place,
    Form,
    NoTarget,
    BadTarget,
    Full,
    Range,
    TooClose,
    Sight,
    Focus,
    Engine,
    Handler
};

constexpr std::size_t CAUSE_COUNT = 29;
constexpr std::array<std::string_view, CAUSE_COUNT> CAUSE_WORDS = {
    "none",         "no_effect", "no_item",    "not_equipped", "level",     "skill",      "class",     "cant_use",
    "not_ready",    "potion",    "no_charges", "busy",         "dead",      "no_control", "in_flight", "in_fight",
    "not_in_fight", "arena",     "place",      "form",         "no_target", "target",     "full",      "range",
    "too_close",    "sight",     "focus",      "engine",       "handler"};
static_assert(static_cast<std::size_t>(Cause::Handler) + 1 == CAUSE_COUNT, "a word per cause");

constexpr std::string_view CauseWord(Cause cause)
{
    std::size_t const index = static_cast<std::size_t>(cause);
    return index < CAUSE_COUNT ? CAUSE_WORDS[index] : std::string_view("none");
}

enum class Outcome : std::uint8_t
{
    Used,
    Pending,
    Interrupted,
    Refused
};

// Why a timed use ended early. Words: moved control death logout reset bound other
enum class Interrupt : std::uint8_t
{
    None,
    Moved,
    Control,
    Death,
    Logout,
    Reset,
    Bound,
    Other,
    Count
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Interrupt::Count)> INTERRUPT_WORDS = {
    "none", "moved", "control", "death", "logout", "reset", "bound", "other"};

constexpr std::string_view InterruptWord(Interrupt interrupt)
{
    std::size_t const index = static_cast<std::size_t>(interrupt);
    return index < INTERRUPT_WORDS.size() ? INTERRUPT_WORDS[index] : std::string_view("other");
}

struct UseResult
{
    Outcome outcome{Outcome::Refused};
    Cause cause{Cause::None};  // Refused
    Interrupt interrupt{Interrupt::None};
    std::uint32_t engine{0};   // Cause::Engine: the SpellCastResult
    std::uint32_t spellId{0};  // the use effect meant
};
}  // namespace ItemUse

#endif
