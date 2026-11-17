/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMUSABLERULES_H
#define _PLAYERBOT_ITEMUSABLERULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ItemOfferRules.h"
#include "ItemSituation.h"
#include "ItemUseKinds.h"
#include "ItemUseTypes.h"

// Item use plan WI15a: the pure parts of the bot's usable list (ItemUsable.h) and of the situation's sampling. Std
// only, so the selftest (item-usable-selftest) builds it alone.
namespace ItemUse
{
constexpr std::uint32_t PVP_TRINKET_SPELL = 42292;        // Every Man for Himself / the medallions' use
constexpr std::uint32_t PVP_TRINKET_SPELL_OTHER = 65547;  // the later medallions' use
constexpr std::uint32_t HEALTH_LOSS_WINDOW_MS = 5000;     // healthLossPerSecond reads this far back
constexpr std::size_t HEALTH_SAMPLES = 8;                 // samples kept for it (at most one per AI tick)

using KindMask = std::uint32_t;
static_assert(static_cast<std::size_t>(Kind::Count) <= 32, "a bit per kind");

constexpr KindMask KindBit(Kind kind) { return static_cast<KindMask>(1u << static_cast<unsigned>(kind)); }

constexpr bool PvpTrinketSpell(std::uint32_t spellId)
{
    return spellId == PVP_TRINKET_SPELL || spellId == PVP_TRINKET_SPELL_OTHER;
}

// The kinds the list has an entry of: a trigger's O(1) gate (HasEntryFor)
inline KindMask MaskOf(std::span<UsableEntry const> list)
{
    KindMask mask = 0;
    for (UsableEntry const& entry : list)
        mask |= KindBit(entry.kind);
    return mask;
}

// The kinds whose entries answer one of the needs
inline KindMask KindsOf(NeedMask needs)
{
    KindMask mask = 0;
    for (std::size_t n = 0; n < NEED_COUNT; ++n)
        if (needs & NeedBit(static_cast<Need>(n)))
            mask |= KindBit(NeedKind(static_cast<Need>(n)));
    return mask;
}

constexpr bool HasEntryFor(KindMask mask, Need need) { return (mask & KindBit(NeedKind(need))) != 0; }

// WI18: an effect of the record stops a cast: a stun, a silence, a fear, a confusion, a pacify-silence or an
// interrupt (a root or a slow does not: the caster stands and casts on)
constexpr bool StopsCast(Record const& record)
{
    using namespace KindIds;
    std::size_t const count = std::min<std::size_t>(record.effectCount, record.effects.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        EffectFact const& effect = record.effects[i];
        if (effect.effect == INTERRUPT_CAST)
            return true;
        std::uint16_t const aura = effect.aura;
        if (aura == AURA_STUN || aura == AURA_SILENCE || aura == AURA_FEAR || aura == AURA_CONFUSE ||
            aura == AURA_PACIFY_SILENCE)
            return true;
    }
    return false;
}

// The record's entry as the list keeps it; count and pvpTrinket from the bot's item, ready from the sampling
inline UsableEntry EntryOf(Record const& record, Classified const& classified, std::uint32_t recordId, ItemRef ref,
                           std::uint32_t count, bool pvpTrinket)
{
    UsableEntry entry;
    entry.record = recordId;
    entry.item = ref;
    entry.kind = classified.kind;
    entry.sub = classified.sub;
    entry.rank = classified.rank;
    entry.count = count;
    entry.consumed = record.consumed;
    entry.potion = record.potion;
    entry.replaceCopper = record.consumed ? record.replaceCopper : 0;
    entry.shape = record.shape;
    entry.radiusYards = record.radiusYards;
    entry.rangeYards = record.rangeYards;
    entry.minRangeYards = record.minRangeYards;
    entry.rangeFriendYards = record.rangeFriendYards;
    entry.pvpTrinket = pvpTrinket || PvpTrinketSpell(record.spellId);
    entry.channeled = record.channeled;
    entry.stopsCast = classified.kind == Kind::Control && StopsCast(record);
    return entry;
}

// A record the list keeps: classified, and not an equip effect that leaks into a use (the mixed trigger spells)
constexpr bool Listed(Classified const& classified, bool mixedTrigger)
{
    return classified.kind != Kind::Unclassified && !mixedTrigger;
}

// The fight offer's list in WI15: the free entries only (consumed ones arrive with WI16)
inline std::vector<UsableEntry> KeepFree(std::span<UsableEntry const> list)
{
    std::vector<UsableEntry> free;
    for (UsableEntry const& entry : list)
        if (!entry.consumed)
            free.push_back(entry);
    return free;
}

// WI16: the consumed kinds the fight offer reads; WI17 adds the area items (explosives), WI18 the gadgets (nets and
// stuns, decoys, helpers and repair bots)
constexpr KindMask CONSUMED_FIGHT_KINDS = KindBit(Kind::Heal) | KindBit(Kind::Mana) | KindBit(Kind::Protect) |
                                          KindBit(Kind::Dispel) | KindBit(Kind::Move) | KindBit(Kind::DamageArea) |
                                          KindBit(Kind::Control) | KindBit(Kind::Summon);

// WI18: a summon without a role (a game object: a feast, a portal; a creature none of the needs asks for: a vanity
// pet) is never the offer's, so a trigger's kind gate on Summon holds only the gadgets
constexpr bool RolelessSummon(UsableEntry const& entry)
{
    return entry.kind == Kind::Summon &&
           (entry.sub == static_cast<std::uint8_t>(Sub::None) || entry.sub == static_cast<std::uint8_t>(Sub::Object));
}

// The fight offer's list (WI16): every free entry, and the consumed ones of the kinds above; no roleless summon
constexpr bool FightListed(UsableEntry const& entry)
{
    return !RolelessSummon(entry) && (!entry.consumed || (CONSUMED_FIGHT_KINDS & KindBit(entry.kind)) != 0);
}

inline std::vector<UsableEntry> KeepFight(std::span<UsableEntry const> list)
{
    std::vector<UsableEntry> fight;
    for (UsableEntry const& entry : list)
        if (FightListed(entry))
            fight.push_back(entry);
    return fight;
}

// The sampler's flight facts (WI16), from world PvP's flight (FleeRules::Flight): a flight under way, or the bot
// stood at its end (cornered or bound) in this PvP combat
inline void SetFlight(Situation& situation, bool active, bool cornered)
{
    situation.flees = active;
    situation.cornered = cornered;
}

// CheapCheck's ready part over a self target with busy left out (the action's Check asks busy): the record's holds,
// may-use, ready and state causes pass; what is left is the target's, which the offer answers
constexpr bool ReadyCause(Cause cause) { return cause == Cause::None || cause >= Cause::NoTarget; }

// How fast the bot loses health: the samples of the fight's last seconds, at most one per AI tick
struct HealthRing
{
    std::array<std::uint32_t, HEALTH_SAMPLES> ms{};
    std::array<std::uint32_t, HEALTH_SAMPLES> health{};
    std::size_t next{0};
    std::size_t count{0};

    void Clear()
    {
        next = 0;
        count = 0;
    }

    void Push(std::uint32_t nowMs, std::uint32_t nowHealth)
    {
        ms[next] = nowMs;
        health[next] = nowHealth;
        next = (next + 1) % HEALTH_SAMPLES;
        if (count < HEALTH_SAMPLES)
            ++count;
    }

    // Health lost per second from the oldest sample within the window to the newest; 0 with fewer than two or a gain
    float LossPerSecond() const
    {
        if (count < 2)
            return 0.0f;
        std::size_t const newest = (next + HEALTH_SAMPLES - 1) % HEALTH_SAMPLES;
        std::size_t oldest = newest;
        for (std::size_t i = 1; i < count; ++i)
        {
            std::size_t const at = (newest + HEALTH_SAMPLES - i) % HEALTH_SAMPLES;
            if (ms[newest] - ms[at] > HEALTH_LOSS_WINDOW_MS)
                break;
            oldest = at;
        }
        std::uint32_t const spanMs = ms[newest] - ms[oldest];
        if (oldest == newest || spanMs == 0 || health[oldest] <= health[newest])
            return 0.0f;
        return static_cast<float>(health[oldest] - health[newest]) * 1000.0f / static_cast<float>(spanMs);
    }
};
}  // namespace ItemUse

#endif
