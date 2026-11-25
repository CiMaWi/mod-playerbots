/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_DEATHEDGERULES_H
#define _PLAYERBOT_DEATHEDGERULES_H

#include <cstdint>

// AW31: one death, one row. The kill hooks name the killer of a player's current life (the first one: a priest's
// Spirit of Redemption passes Unit::Kill twice in one life), and the rows of the death are written once, at the
// death's edge (OnPlayerJustDied) or at a resurrect that came before the edge, with that killer or the word unknown.
// Header-only so tests/awareness-rules-selftest.cpp checks the same rules the world log's hooks run
namespace DeathEdge
{
// Who killed: None while no kill hook named a killer in this life (the Death row's word unknown)
enum class Kind : std::uint8_t
{
    None,
    Environment,
    Player,
    Creature,
    PlayerPet
};

struct Killer
{
    Kind kind = Kind::None;
    std::uint32_t id = 0;     // the Death row's ref: the killing player's or the pet owner's guid counter, an entry
    std::uint32_t entry = 0;  // the killing unit's entry, the DeathScene's ref
    std::uint64_t unit = 0;   // the killing unit's raw guid, found again at the edge (0: none)
};

// The killer of a kill hook: OnPlayerPVPKill (the player itself: environmental damage) or OnPlayerKilledByCreature
// (a player's pet, totem or guardian: its owner)
constexpr Kind KindOf(bool pvpHook, bool self, bool ownerIsPlayer)
{
    if (pvpHook)
        return self ? Kind::Environment : Kind::Player;
    return ownerIsPlayer ? Kind::PlayerPet : Kind::Creature;
}

// The first kill hook of a life names its killer and writes the killer's side; false when one is named already (the
// life's second Unit::Kill), so that side is written once per life
constexpr bool Name(Killer& life, Killer const& killer)
{
    if (life.kind != Kind::None || killer.kind == Kind::None)
        return false;
    life = killer;
    return true;
}

// The rows of the death take the life's killer and end the name, so the next life starts unnamed
constexpr Killer Take(Killer& life)
{
    Killer const named = life;
    life = Killer{};
    return named;
}

// A resurrect writes the rows of a death whose edge has not come: only a named killer shows that one happened
constexpr bool RowsAtResurrect(Killer const& life) { return life.kind != Kind::None; }

// The Death row's word
constexpr char const* DeathWord(Kind kind)
{
    switch (kind)
    {
        case Kind::Environment:
            return "environment";
        case Kind::Player:
            return "player";
        case Kind::Creature:
            return "creature";
        case Kind::PlayerPet:
            return "player_pet";
        default:
            return "unknown";
    }
}

// A player's or a pet's kill also writes the victim's PvpDeath row
constexpr bool IsPvp(Kind kind) { return kind == Kind::Player || kind == Kind::PlayerPet; }
}  // namespace DeathEdge

#endif
