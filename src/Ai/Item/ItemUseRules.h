/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSERULES_H
#define _PLAYERBOT_ITEMUSERULES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "ItemUseTypes.h"

// The use path's pure rules (item-use plan WI3, INTERFACE.md 1.8): the cheap checks of one request over a snapshot
// the bot's map thread takes, the engine results that have a cause of their own, and the lifecycle of a timed use.
// Std only, so the selftest builds it alone.
namespace ItemUse
{
// The states that take a bot's control (UNIT_STATE_LOST_CONTROL), as bits of BotView::lostControl. The first three
// are UNIT_STATE_CONTROLLED, which a spell lets through by its ALLOW_WHILE attributes (Spell::CheckCasterAuras)
namespace LostControl
{
constexpr std::uint8_t Stunned = 1;
constexpr std::uint8_t Fleeing = 2;
constexpr std::uint8_t Confused = 4;
constexpr std::uint8_t Other = 8;  // jumping, charging: no attribute allows a use
}  // namespace LostControl

struct BotView  // what the bot's map thread reads for one request
{
    std::uint8_t level{1};
    std::uint32_t classBit{0};  // 1 << (class - 1)
    std::uint32_t raceBit{0};
    std::uint16_t skillValue{0};  // of the record's needs.skill
    bool hasRequiredSpell{true};
    std::uint32_t held{0};  // how many of the item it holds, the worn piece counts 1
    bool equipped{false};
    bool canUse{true};  // Player::CanUseItem, with the refusal kept in canUseCause
    Cause canUseCause{Cause::None};
    std::uint32_t cooldownLeftMs{0};  // of the spell, the shared category included
    std::int32_t chargesLeft{0};
    bool potionDrunk{false};  // Player::GetLastPotionId() != 0
    bool busy{false};         // casts a non-melee spell, or has a pending use
    bool alive{true};
    std::uint8_t lostControl{0};  // LostControl bits of the states the bot is under
    bool inFight{false};
    bool inArena{false};
    bool shapeshifted{false};
    bool outdoors{true};
    bool placeFits{true};  // the spell's CheckLocation and the item's map and area
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct TargetView  // the request's target, as found on the bot's map
{
    TargetKind kind{TargetKind::None};
    bool found{false};
    bool isBot{false};  // the unit is the bot itself
    bool alive{true};
    bool friendly{false};
    bool hostile{false};
    bool healthFull{false};
    bool powerFull{false};
    bool inSight{true};          // the engine's line-of-sight answer, taken once for the request
    float yards{0.0f};           // from the bot; for a spot, to the spot
    std::int32_t itemClass{-1};  // OwnItem
    std::uint32_t itemSubClass{0};
    std::uint32_t itemInvType{0};
    std::uint16_t itemLevel{0};
    std::uint16_t requiredLevel{0};  // the item's RequiredLevel: the engine's floor reads it, ItemLevel when it is 0
    bool itemHasCoating{false};
};

// The game's ids the rules read (SharedDefines.h, SpellInfo.h): this header includes no game header
constexpr std::uint16_t RULE_EFFECT_HEAL = 10;      // SPELL_EFFECT_HEAL
constexpr std::uint16_t RULE_EFFECT_ENERGIZE = 30;  // SPELL_EFFECT_ENERGIZE
constexpr std::uint8_t RULE_TARGET_UNIT_PET = 5;    // TARGET_UNIT_PET: Spell::CheckItems skips its effects
constexpr std::uint8_t RULE_CLASS_CONSUMABLE = 0;   // ITEM_CLASS_CONSUMABLE
// AttributesEx5 (attribute word 5)
constexpr std::uint32_t RULE_ATTR5_ALLOW_WHILE_STUNNED = 0x00000008;   // SPELL_ATTR5_ALLOW_WHILE_STUNNED
constexpr std::uint32_t RULE_ATTR5_ALLOW_WHILE_FLEEING = 0x00020000;   // SPELL_ATTR5_ALLOW_WHILE_FLEEING
constexpr std::uint32_t RULE_ATTR5_ALLOW_WHILE_CONFUSED = 0x00040000;  // SPELL_ATTR5_ALLOW_WHILE_CONFUSED

// SpellCastResult values of the engine's CheckCast (SharedDefines.h)
constexpr std::uint32_t CAST_FULL_HEALTH = 2;         // SPELL_FAILED_ALREADY_AT_FULL_HEALTH
constexpr std::uint32_t CAST_FULL_POWER = 4;          // SPELL_FAILED_ALREADY_AT_FULL_POWER
constexpr std::uint32_t CAST_BAD_TARGETS = 12;        // SPELL_FAILED_BAD_TARGETS
constexpr std::uint32_t CAST_LINE_OF_SIGHT = 47;      // SPELL_FAILED_LINE_OF_SIGHT
constexpr std::uint32_t CAST_NOT_READY = 67;          // SPELL_FAILED_NOT_READY
constexpr std::uint32_t CAST_NO_CHARGES = 76;         // SPELL_FAILED_NO_CHARGES_REMAIN
constexpr std::uint32_t CAST_OUT_OF_RANGE = 97;       // SPELL_FAILED_OUT_OF_RANGE
constexpr std::uint32_t CAST_SPELL_FOCUS = 102;       // SPELL_FAILED_REQUIRES_SPELL_FOCUS
constexpr std::uint32_t CAST_TOO_CLOSE = 128;         // SPELL_FAILED_TOO_CLOSE
constexpr std::uint32_t CAST_UNIT_NOT_INFRONT = 134;  // SPELL_FAILED_UNIT_NOT_INFRONT
constexpr std::uint32_t CAST_NOT_IN_ARENA = 151;      // SPELL_FAILED_NOT_IN_ARENA
constexpr std::uint32_t CAST_OK = 255;                // SPELL_CAST_OK

struct EngineCause
{
    std::uint32_t result;
    Cause cause;
};

// The engine results with a cause of their own; any other result is Cause::Engine with its number. Facing is not a
// refusal: Use turns the bot to its target before it sends, as a client does
constexpr std::array<EngineCause, 12> ENGINE_CAUSES = {{{CAST_OK, Cause::None},
                                                        {CAST_UNIT_NOT_INFRONT, Cause::None},
                                                        {CAST_NOT_READY, Cause::NotReady},
                                                        {CAST_NO_CHARGES, Cause::NoCharges},
                                                        {CAST_NOT_IN_ARENA, Cause::Arena},
                                                        {CAST_BAD_TARGETS, Cause::BadTarget},
                                                        {CAST_FULL_HEALTH, Cause::Full},
                                                        {CAST_FULL_POWER, Cause::Full},
                                                        {CAST_OUT_OF_RANGE, Cause::Range},
                                                        {CAST_TOO_CLOSE, Cause::TooClose},
                                                        {CAST_LINE_OF_SIGHT, Cause::Sight},
                                                        {CAST_SPELL_FOCUS, Cause::Focus}}};

constexpr Cause CauseOfEngine(std::uint32_t result)
{
    for (EngineCause const& mapped : ENGINE_CAUSES)
        if (mapped.result == result)
            return mapped.cause;
    return Cause::Engine;
}

// The record a request means: its spell, or the item's first use effect; null when there is none (no_effect)
inline Record const* SelectRecord(std::span<Record const> records, std::uint32_t spellId)
{
    for (Record const& record : records)
        if (!spellId || record.spellId == spellId)
            return &record;
    return nullptr;
}

namespace Detail
{
constexpr bool IsUnitShape(Shape shape)
{
    return shape == Shape::Friend || shape == Shape::Enemy || shape == Shape::AnyUnit;
}

// The request's target kind against the record's shape (INTERFACE.md 1.3); a Self request counts as the bot as unit
constexpr bool KindFits(Shape shape, TargetKind kind)
{
    switch (shape)
    {
        case Shape::None:
        case Shape::Self:
            return kind == TargetKind::None || kind == TargetKind::Self || kind == TargetKind::Unit;
        case Shape::Friend:
        case Shape::Enemy:
        case Shape::AnyUnit:
            return kind == TargetKind::Unit || kind == TargetKind::Self;
        case Shape::Ground:
            return kind == TargetKind::Spot;
        case Shape::OwnItem:
            return kind == TargetKind::OwnItem || kind == TargetKind::TradeSlot;
        case Shape::Object:
            return kind == TargetKind::Object;
        default:
            return false;
    }
}

// The lost-control states the record's spell may be used in: each controlling state needs its own attribute (the PvP
// trinket carries all three); the other states allow none. The engine may still refuse a stun of another mechanic
constexpr std::uint8_t ControlAllowed(Record const& record)
{
    std::uint32_t const ex5 = record.attributes[5];
    return static_cast<std::uint8_t>(((ex5 & RULE_ATTR5_ALLOW_WHILE_STUNNED) ? LostControl::Stunned : 0) |
                                     ((ex5 & RULE_ATTR5_ALLOW_WHILE_FLEEING) ? LostControl::Fleeing : 0) |
                                     ((ex5 & RULE_ATTR5_ALLOW_WHILE_CONFUSED) ? LostControl::Confused : 0));
}

constexpr bool UnitFits(Record const& record, Request const& request, TargetView const& target)
{
    if (record.shape == Shape::None || record.shape == Shape::Self)
        return target.isBot;
    if (record.shape == Shape::Friend && !target.isBot && !target.friendly)
        return false;
    if (record.shape == Shape::Enemy && (target.isBot || !target.hostile))
        return false;
    if (!target.alive)
        return record.life != Life::Alive || request.target.dead;
    return record.life != Life::Dead;
}

constexpr bool ItemFits(Record const& record, TargetView const& target)
{
    if (record.targetItemClass >= 0 && target.itemClass != record.targetItemClass)
        return false;
    if (record.targetItemSubClassMask && target.itemSubClass < 32 &&
        !(record.targetItemSubClassMask & (1u << target.itemSubClass)))
        return false;
    if (record.targetItemInvTypeMask && target.itemInvType < 32 &&
        !(record.targetItemInvTypeMask & (1u << target.itemInvType)))
        return false;
    // Spell::CheckItems: the spell's BaseLevel against the item's RequiredLevel (ItemLevel when that is 0), its
    // MaxLevel against the ItemLevel
    std::uint16_t const floorLevel = target.requiredLevel ? target.requiredLevel : target.itemLevel;
    if (floorLevel < record.targetItemMinLevel)
        return false;
    if (record.targetItemMaxLevel && target.itemLevel > record.targetItemMaxLevel)
        return false;
    return !(record.exclude == Exclude::Coating && target.itemHasCoating);
}

// Spell::CheckItems' rule for a consumable on a unit: refused only when every heal or energize effect finds the
// target full
constexpr bool Full(Record const& record, TargetView const& target)
{
    if (record.itemClass != RULE_CLASS_CONSUMABLE)
        return false;
    bool full = false;
    for (std::size_t i = 0; i < record.effectCount && i < record.effects.size(); ++i)
    {
        EffectFact const& effect = record.effects[i];
        if (effect.triggered || effect.targetA == RULE_TARGET_UNIT_PET)
            continue;
        if (effect.effect == RULE_EFFECT_HEAL)
        {
            if (!target.healthFull)
                return false;
            full = true;
        }
        else if (effect.effect == RULE_EFFECT_ENERGIZE)
        {
            if (!target.powerFull)
                return false;
            full = true;
        }
    }
    return full;
}
}  // namespace Detail

// Causes 1 to 24 in their order (INTERFACE.md 1.4): the first that holds, Cause::None when none does. Pure
inline Cause CheapCheck(Record const& record, Request const& request, BotView const& bot, TargetView const& target)
{
    Needs const& needs = record.needs;
    // holds and may use
    if (!record.spellId || (request.spellId && request.spellId != record.spellId))
        return Cause::NoEffect;
    if (!bot.held)
        return Cause::NoItem;
    if (record.inventoryType && !bot.equipped)
        return Cause::NotEquipped;
    if (bot.level < needs.level)
        return Cause::Level;
    if ((needs.skill && bot.skillValue < needs.skillRank) || !bot.hasRequiredSpell)
        return Cause::Skill;
    if ((needs.classMask && !(needs.classMask & bot.classBit)) || (needs.raceMask && !(needs.raceMask & bot.raceBit)))
        return Cause::Class;
    if (!bot.canUse)
        return bot.canUseCause != Cause::None ? bot.canUseCause : Cause::CantUse;
    // ready
    if (bot.cooldownLeftMs)
        return Cause::NotReady;
    if (record.potion && bot.potionDrunk)
        return Cause::Potion;
    if (record.charges && !bot.chargesLeft)
        return Cause::NoCharges;
    if (bot.busy)
        return Cause::Busy;
    // state
    if (!bot.alive)
        return Cause::Dead;
    if (bot.lostControl & ~Detail::ControlAllowed(record))
        return Cause::NoControl;
    if (needs.fight == FightNeed::Peace && bot.inFight)
        return Cause::InFight;
    if (needs.fight == FightNeed::Fight && !bot.inFight)
        return Cause::NotInFight;
    if (needs.notInArena && bot.inArena)
        return Cause::Arena;
    if (!bot.placeFits || (needs.outdoors && !bot.outdoors) || (needs.indoors && bot.outdoors))
        return Cause::Place;
    if (needs.notShapeshifted && bot.shapeshifted)
        return Cause::Form;
    // target fit
    if (!Detail::KindFits(record.shape, request.target.kind) || !target.found)
        return Cause::NoTarget;
    bool const onUnit = target.kind == TargetKind::Unit || target.kind == TargetKind::Self;
    if (onUnit && !Detail::UnitFits(record, request, target))
        return Cause::BadTarget;
    if (target.kind == TargetKind::OwnItem && !Detail::ItemFits(record, target))
        return Cause::BadTarget;
    if (onUnit && Detail::Full(record, target))
        return Cause::Full;
    // range and sight: units other than the bot, spots and objects
    bool const away = (onUnit && !target.isBot) || target.kind == TargetKind::Spot || target.kind == TargetKind::Object;
    if (!away)
        return Cause::None;
    float const range = target.friendly ? record.rangeFriendYards : record.rangeYards;
    if (range > 0.0f && target.yards > range)
        return Cause::Range;
    if (record.minRangeYards > 0.0f && target.yards < record.minRangeYards)
        return Cause::TooClose;
    if (!target.inSight)
        return Cause::Sight;
    return Cause::None;
}

// A timed use (a cast time or a channel) between its start and its end; at most one per bot
struct PendingUse
{
    std::uint32_t itemEntry{0};
    std::uint32_t spellId{0};
    Caller caller{Caller::Test};
    std::uint32_t startedMs{0};
    std::uint32_t boundMs{0};  // castMs or the channel's duration, plus the grace (3.3)
};

// What can happen to a bot between the start and the end of a use
enum class UseEvent : std::uint8_t
{
    CastDone,
    CastFailed,
    Moved,
    LostControl,
    Died,
    LoggedOut,
    Reset,
    Tick
};

// The end the event brings, if it ends the use; Tick ends it only past the bound (boundMs after startedMs)
inline std::optional<UseResult> Advance(PendingUse const& pending, UseEvent event, std::uint32_t nowMs)
{
    UseResult result;
    result.spellId = pending.spellId;
    result.outcome = Outcome::Interrupted;
    switch (event)
    {
        case UseEvent::CastDone:
            result.outcome = Outcome::Used;
            return result;
        case UseEvent::CastFailed:
            result.interrupt = Interrupt::Other;
            return result;
        case UseEvent::Moved:
            result.interrupt = Interrupt::Moved;
            return result;
        case UseEvent::LostControl:
            result.interrupt = Interrupt::Control;
            return result;
        case UseEvent::Died:
            result.interrupt = Interrupt::Death;
            return result;
        case UseEvent::LoggedOut:
            result.interrupt = Interrupt::Logout;
            return result;
        case UseEvent::Reset:
            result.interrupt = Interrupt::Reset;
            return result;
        case UseEvent::Tick:
            if (nowMs - pending.startedMs <= pending.boundMs)
                return std::nullopt;
            result.interrupt = Interrupt::Bound;
            return result;
    }
    return std::nullopt;
}

// What the bot's map thread saw of its pending use since the start (ItemUse::Update and the engine's spell hooks)
struct PendingSigns
{
    bool channeled{false};
    bool castSeen{false};    // the engine cast the spell (Spell::_cast: a cast time's end, a channel's start)
    bool cancelSeen{false};  // the engine cancelled it (Spell::cancel: a move, a stun, a damage interrupt, a death)
    bool movedAtCancel{false};
    bool controlLostAtCancel{false};
    bool alive{true};
    bool runs{false};  // the item's spell is the bot's current generic or channelled spell
};

// The event the signs give at one AI update: a cast time that ended in the cast is used; a death, then a cancel
// (moved, stunned, else other); while the spell runs a tick (the bound); a spell gone without a cancel is used for a
// channel that had started (it ran out) and failed otherwise (the engine refused it at the cast's end)
constexpr UseEvent EventOf(PendingSigns const& signs)
{
    if (signs.castSeen && !signs.channeled)
        return UseEvent::CastDone;
    if (!signs.alive)
        return UseEvent::Died;
    if (signs.cancelSeen)
    {
        if (signs.movedAtCancel)
            return UseEvent::Moved;
        return signs.controlLostAtCancel ? UseEvent::LostControl : UseEvent::CastFailed;
    }
    if (signs.runs)
        return UseEvent::Tick;
    return signs.channeled && signs.castSeen ? UseEvent::CastDone : UseEvent::CastFailed;
}

// The bound of a timed use: its cast time, its channel's duration, plus the grace (PLAN 3.3)
constexpr std::uint32_t BoundOf(Record const& record, std::uint32_t graceMs)
{
    return record.castMs + (record.channeled ? record.durationMs : 0) + graceMs;
}

// A worn piece's enchantment record (a tinker) as the piece's own use: Player::CastItemUseSpell casts an
// enchantment's use spell with the piece, so the row names the piece and the piece must be worn
inline Record PieceRecord(Record record, std::uint32_t pieceEntry, std::uint8_t inventoryType)
{
    record.itemEntry = pieceEntry;
    record.inventoryType = inventoryType;
    return record;
}
}  // namespace ItemUse

#endif
