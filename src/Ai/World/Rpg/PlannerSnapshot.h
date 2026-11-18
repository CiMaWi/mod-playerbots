/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_PLANNERSNAPSHOT_H
#define _PLAYERBOT_PLANNERSNAPSHOT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "TraitRules.h"

// The bot as the planner sees it (PlannerPool.h): Planner::Snapshot and the plain types it holds. The
// snapshot contract: a step that gives the planner a new fact adds its field here.
namespace Planner
{
constexpr std::size_t QUEST_LOG_SLOTS = 25;
constexpr std::size_t QUEST_COUNTERS = 4;
constexpr std::size_t TAXI_WORDS = 14;  // PlayerTaxi's mask
constexpr std::size_t RECENT_GOALS = 4;

struct Place
{
    std::uint32_t map{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct QuestSlot
{
    std::uint32_t quest{0};
    std::uint32_t state{0};  // the quest log slot's state flags
    std::array<std::uint16_t, QUEST_COUNTERS> counters{};
};

struct Skill
{
    std::uint16_t id{0};
    std::uint16_t value{0};
    std::uint16_t max{0};
};

// The skills a snapshot carries (FillSnapshot reads exactly these, the core's SkillType ids): riding, then the
// professions. One the snapshot lacks is at 0
constexpr std::array<std::uint16_t, 15> SNAPSHOT_SKILLS = {762, 129, 185, 356, 171, 164, 333, 202,
                                                           182, 773, 755, 165, 186, 393, 197};

// A weapon the bot wields or carries, by the weapon skill line it needs (upkeep WU15a)
struct OwnedWeapon
{
    std::uint16_t skill{0};
    bool known{false};  // the skill's value is above 0: Player::CanUseItem's weapon test passes
};

// A carried item that counts for an upkeep stock kind (upkeep plan WU2, UpkeepIndex.h)
struct StockItem
{
    std::uint32_t item{0};
    std::uint32_t count{0};
    std::uint8_t kinds{0};     // a bit per UpkeepStock
    std::uint8_t diet{0};      // the pet food bit (FoodType - 1); 0 = no pet eats it
    std::uint8_t subClass{0};  // the item's subclass: ammo counts only the ranged weapon's (arrow 2, bullet 3)
    std::uint8_t level{0};     // the item's required level: poison counts only the grade the bot keeps
    std::uint32_t damage{0};   // ammo: its damage x 1000 as the item usage compares it (UpkeepItemFacts::ammoDamage)
    std::uint32_t lot{1};      // the item's vendor lot (BuyCount): the stock rule keeps a lot less one above the target
};

struct Goal
{
    std::uint32_t kind{0};
    std::uint32_t target{0};
    std::uint32_t startedMs{0};
};

// The bot as the planner sees it (WT.11), filled on its map thread; plain data, a few KB
struct Snapshot
{
    std::uint64_t bot{0};  // the guid's raw value
    std::uint32_t stateVersion{0};
    std::uint32_t takenMs{0};
    std::uint8_t level{0};
    std::uint8_t race{0};
    std::uint8_t cls{0};
    std::uint8_t team{0};  // 0 Alliance, 1 Horde
    std::uint32_t xp{0};
    Place position;
    std::uint32_t zone{0};
    std::uint32_t area{0};
    std::uint32_t money{0};
    std::uint16_t bagSlots{0};  // general slots: the backpack and plain bags
    std::uint16_t bagFree{0};
    std::uint32_t lowestItemWorth{0};  // not filled yet: worth per item is not cheap on the map thread
    std::uint8_t durabilityPercent{100};
    std::uint8_t lowestDurabilityPercent{100};  // the most worn equipped item
    std::uint8_t brokenItems{0};                // equipped items at durability 0
    std::uint32_t repairCost{0};                // copper to repair every worn item, the core's formula, no discount
    std::vector<Skill> skills;                  // riding and the professions the bot has
    std::vector<QuestSlot> quests;
    std::vector<std::uint32_t> rewardedQuests;  // sorted
    bool hasHearthstone{false};
    Place hearth;
    std::uint32_t hearthCooldownMs{0};
    std::array<std::uint32_t, TAXI_WORDS> taxiMask{};
    std::optional<TraitRules::BotTraits> traits;
    float runSpeed{0.0f};
    bool mounted{false};
    std::uint8_t groupSize{0};  // 0 = no group
    std::uint64_t groupLeader{0};
    std::uint32_t deaths{0};  // not filled yet: no cheap per-bot death count exists
    std::uint32_t sickMs{0};  // resurrection sickness left at takenMs (upkeep WU16); 0 = not sick
    Goal currentGoal;         // WT16 fills the goals
    std::vector<Goal> recentGoals;
    // What the upkeep needs rules read (upkeep plan WU2, FillUpkeep in PlannerUpkeep.cpp)
    std::vector<StockItem> stock;  // the backpack and every bag, one entry per item id
    std::uint8_t petDiet{0};       // the hunter pet's diet (bit FoodType - 1); 0 = unknown
    // The hunter's pet (WU21b): a current pet, summoned or not (dismissed or dead: PetStable::CurrentPet), or the
    // unslotted one; any of them makes the core's Tame Beast tame nothing
    bool hasPet{false};
    std::uint8_t petFamily{0};  // its creature family; 0 = unknown
    std::uint8_t petLevel{0};
    bool knowsTameBeast{false};                       // Tame Beast (1515)
    bool tamesExotic{false};                          // Player::CanTameExoticPets: Beast Mastery's aura
    bool stableFree{false};                           // a bought stable slot is empty (WU21c)
    std::uint32_t stableSlotPrice{0};                 // the next slot's price; 0 = every slot bought
    std::uint8_t ammoSubClass{0};                     // the equipped ranged weapon's ammo: arrow 2, bullet 3; 0 = none
    std::uint32_t ammoDamage{0};                      // the equipped ammo's (PLAYER_AMMO_ID) damage x 1000; 0 = none
    std::uint32_t upkeepVersion{0};                   // the upkeep index the bit sets align to; 0 = none yet
    std::vector<std::uint64_t> knownClassSpells;      // bit i: the class trainer list's spell i is known
    std::vector<std::uint64_t> knownRidingSpells;     // the same over the riding trainer list
    std::vector<std::uint64_t> canLearnClassSpells;   // bit i: the class list's spell i fits the bot's class and race
    std::vector<std::uint64_t> canLearnRidingSpells;  // the same over the riding trainer list
    // Bit i: the abilities the class list's spell i needs that are on neither list (talents, racials, quest spells
    // such as Summon Warhorse) are all known
    std::vector<std::uint64_t> offListMetClassSpells;
    // Bit i: the class list's spell i is known and cast without its reagents (Player::CanNoReagentCast: Glyph of
    // Renewed Life frees Reincarnation's Ankh); only the reagent spells' positions are filled
    std::vector<std::uint64_t> reagentFreeClassSpells;
    // The riding skill the bot's best known mount of each kind needs; a riding rank is complete with a mount of its
    // skill or better (WU14). The travel shares are no snapshot fields: the needs read them from the slot measure
    std::uint16_t groundMountSkill{0};
    std::uint16_t flyingMountSkill{0};
    // The owned-weapon-skills field (WU15a): the main hand, off hand and ranged weapons and those in the bags, a
    // skill line once each, sorted
    std::vector<OwnedWeapon> ownedWeaponSkills;

    // What the bot knows of its mail and bank (BankMail::BotBankMail); the mail count is renewed with each snapshot
    std::uint16_t mailWaiting{0};
    std::uint32_t mailSeenCopper{0};
    std::uint32_t mailExpectedCopper{0};
    std::uint32_t bankKeptCount{0};
    std::uint32_t bankDueCount{0};
    std::uint32_t bankDueWorth{0};
    std::uint16_t bankFreeSlots{0};

    // What the professions read (professions plan WR1, FillUpkeep): the spells the bot knows of its profession skill
    // lines (recipes, gathering and conversion spells, not the rank spells that teach a profession), sorted; and the
    // profession tools it holds, equipped or in the bags (an item a cast asks for by its tool category, or a fishing
    // pole), item ids once each, sorted
    std::vector<std::uint32_t> knownRecipes;
    std::vector<std::uint32_t> toolsHeld;

    std::uint32_t StockCount(std::uint8_t kindBit) const
    {
        std::uint32_t total = 0;
        for (StockItem const& entry : stock)
            if (entry.kinds & kindBit)
                total += entry.count;
        return total;
    }

    std::size_t ApproxBytes() const
    {
        return sizeof(Snapshot) + skills.capacity() * sizeof(Skill) + quests.capacity() * sizeof(QuestSlot) +
               rewardedQuests.capacity() * sizeof(std::uint32_t) + recentGoals.capacity() * sizeof(Goal) +
               stock.capacity() * sizeof(StockItem) + ownedWeaponSkills.capacity() * sizeof(OwnedWeapon) +
               (knownRecipes.capacity() + toolsHeld.capacity()) * sizeof(std::uint32_t) +
               (knownClassSpells.capacity() + reagentFreeClassSpells.capacity() + knownRidingSpells.capacity() +
                canLearnClassSpells.capacity() + canLearnRidingSpells.capacity() + offListMetClassSpells.capacity()) *
                   sizeof(std::uint64_t);
    }
};
}  // namespace Planner

#endif
