/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_ITEMSITUATION_H
#define _PLAYERBOT_ITEMSITUATION_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "ItemUseKinds.h"
#include "ItemUseTypes.h"

// Item use plan, INTERFACE 1.9: what the bot carries that it could use, and the situation it is in. Pure and std
// only, any thread. Written by WI13 (the spend rule's Danger reads the Situation); WI14's ItemOfferRules.h includes
// it. Fields may be appended at the end of a struct, never reordered.
namespace ItemUse
{
struct UsableEntry  // one use effect of something the bot carries or wears
{
    std::uint32_t record{0};  // the index's id (2.1)
    ItemRef item;
    Kind kind{Kind::Unclassified};
    std::uint8_t sub{0};
    std::int32_t rank{0};
    std::uint32_t count{0};  // uses left: the stacks' total, or the charges
    bool consumed{false};
    bool potion{false};
    std::uint32_t replaceCopper{0};
    bool ready{false};  // CheapCheck's ready part, refreshed when the situation is sampled
    // WI14: the Record's target facts the offer reads (filled from the Record when the list is built)
    Shape shape{Shape::None};
    float radiusYards{0.0f};  // 0: no area
    float rangeYards{0.0f};   // against an enemy or a spot
    float minRangeYards{0.0f};
    float rangeFriendYards{0.0f};
    bool pvpTrinket{false};  // spell 42292 or 65547: the PvP trinket, its tactic's (CcTacticStrategy), never offered
};

enum class Rank : std::uint8_t
{
    Normal,
    Elite,
    Rare,
    RareElite,
    Boss,
    Player
};

enum class Role : std::uint8_t
{
    Tank,
    Healer,
    Melee,
    Ranged,
    Caster
};

enum class Stake : std::uint8_t
{
    Ordinary,
    Player,
    Objective
};  // what is lost with the fight

struct NearUnit
{
    std::uint64_t guid{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    std::uint8_t level{1};
    Rank rank{Rank::Normal};
    float healthPct{100.0f};
    std::uint32_t health{0};
    bool inFight{false};
    bool onBot{false};
    bool controlled{false};
    bool flees{false};
    bool casts{false};
    bool friendly{false};
};

constexpr std::size_t MAX_NEAR = 16;

struct Situation
{
    std::uint8_t level{1};
    Role role{Role::Melee};
    float healthPct{100.0f};
    float manaPct{100.0f};
    bool usesMana{false};
    std::uint32_t maxHealth{0};
    float healthLossPerSecond{0.0f};  // over the last seconds of the fight
    std::array<NearUnit, MAX_NEAR> units{};
    std::uint8_t unitCount{0};
    std::int8_t target{-1};  // index into units, -1: none
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    bool inFight{false};
    bool fightEnded{false};
    bool bursts{false};
    bool holdsBurst{false};
    bool flees{false};
    bool cornered{false};
    bool chases{false};
    bool inArena{false};
    Stake stake{Stake::Ordinary};
    std::uint8_t groupSize{1};
    std::uint8_t groupHurt{0};
    std::uint8_t groupDead{0};
    std::uint32_t dispelMask{0};
    bool rooted{false};
    bool snared{false};
    bool gearBroken{false};
    bool vendorNear{false};
    bool potionDrunk{false};
    float thrift{0.0f};  // -1..1 (family rule 9)
    float daring{0.0f};  // -1..1: minus the Caution trait's unit (Q3, settled by WI13; ItemSpendRules.h)
    std::uint32_t earnCopperPerMinute{0};
    std::uint32_t repairCopper{0};
    bool control{false};  // control cohort or the test's nospend: nothing consumed is spent
};
}  // namespace ItemUse

#endif
