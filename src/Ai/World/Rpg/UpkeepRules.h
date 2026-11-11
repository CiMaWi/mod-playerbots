/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_UPKEEPRULES_H
#define _PLAYERBOT_UPKEEPRULES_H

#include <array>
#include <cstddef>
#include <cstdint>

// Honest upkeep of persistent bots: the kinds of free handouts, which of them are honest already and who keeps every
// handout (the handout share). Player-free for tests/upkeep-selftest.cpp; Upkeep.h applies it to a bot. Kept out of
// PlayerbotAIConfig.h so that flipping a kind rebuilds only the handout sites.
enum class UpkeepKind : uint8_t
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
    Talent
};

namespace Upkeep
{
// word: the kind's honesty row is Honesty<word> (EventLog::Honesty) and the kind's name in NeedShort events.
// honest: the handout is off for persistent bots outside the handout share. The step that makes a kind honest flips
// its entry here; no option does. Gold is honest since honest travel (a bot that keeps its possessions earns it).
struct KindRule
{
    char const* word;
    bool honest;
};

constexpr size_t KIND_COUNT = 20;
constexpr std::array<KindRule, KIND_COUNT> KIND_RULES = {{
    {"Food", true},  // food and water
    {"Ammo", true},
    {"Reagent", true},
    {"Poison", true},      // a rogue's InitConsumables
    {"Consumable", true},  // potions, weapon oils and stones
    {"Repair", true},
    {"Resurrect", true},      // in place, or at the spirit healer without sickness and durability loss
    {"TrainerSpell", false},  // class and trainer spells
    {"WeaponSkill", false},   // InitSkills
    {"Riding", false},        // the riding ranks by level (InitSkills)
    {"Mount", false},
    {"SpecialSpell", false},
    {"ClassQuest", false},  // spells of class quests the bot never did
    {"Pet", false},         // pet and pet talents
    {"Glyph", false},
    {"Key", false},  // keyring
    {"Reputation", false},
    {"Attunement", false},  // completed attunement quests
    {"Gold", true},
    {"Talent", false},  // the talent reset below the persistence level
}};

constexpr char const* KindWord(UpkeepKind kind)
{
    size_t const index = static_cast<size_t>(kind);
    return index < KIND_COUNT ? KIND_RULES[index].word : "Other";
}

constexpr bool IsHonest(UpkeepKind kind)
{
    size_t const index = static_cast<size_t>(kind);
    return index < KIND_COUNT && KIND_RULES[index].honest;
}

// The handout share (Playerbots.Upkeep.HandoutShare): a bot's slot 0..99 is a fixed mix of its guid counter, so it
// stays in one cohort across builds and restarts and SQL names the same bots:
// ((guid * 2654435761) & 0xFFFFFFFF) >> 16) % 100 < share
constexpr uint32_t SHARE_MIX = 2654435761u;
constexpr uint32_t SHARE_SHIFT = 16;
constexpr uint32_t SHARE_PERCENT_MAX = 100;

constexpr uint32_t ShareSlot(uint32_t guidCounter)
{
    return ((guidCounter * SHARE_MIX) >> SHARE_SHIFT) % SHARE_PERCENT_MAX;
}

constexpr bool InShare(uint32_t guidCounter, uint32_t sharePercent) { return ShareSlot(guidCounter) < sharePercent; }

// A bot gets a handout unless it keeps its possessions, the kind is honest and the bot is outside the share
constexpr bool Granted(bool keepsPossessions, bool honest, uint32_t guidCounter, uint32_t sharePercent)
{
    return !keepsPossessions || !honest || InShare(guidCounter, sharePercent);
}

// Resurrection sickness, which the core's spirit healer path casts (Player::ResurrectPlayer): while it lasts a bot does
// nothing that needs fighting; it rests or runs errands (upkeep plan §3.5, one gate in the RPG status pick until WU16
// gives the planner "sick until")
constexpr uint32_t SPELL_RESURRECTION_SICKNESS = 15007;

// The NewRpgStatus values whose activity fights (PlayerbotAIConfig.h; NewRpgBaseAction.cpp asserts them): grinding and
// the wander that picks a grind target, quests, outdoor PvP and gathering among mobs. Rest, travel, flights, errands
// and the npc wander (and the camp walk that ends in it) stay open to a sick bot
constexpr uint32_t RPG_STATUS_GO_GRIND = 1;
constexpr uint32_t RPG_STATUS_WANDER_RANDOM = 3;
constexpr uint32_t RPG_STATUS_DO_QUEST = 5;
constexpr uint32_t RPG_STATUS_OUTDOOR_PVP = 8;
constexpr uint32_t RPG_STATUS_DO_GATHER = 9;
constexpr uint32_t RPG_STATUS_FIGHT_MASK = (1u << RPG_STATUS_GO_GRIND) | (1u << RPG_STATUS_WANDER_RANDOM) |
                                           (1u << RPG_STATUS_DO_QUEST) | (1u << RPG_STATUS_OUTDOOR_PVP) |
                                           (1u << RPG_STATUS_DO_GATHER);

constexpr bool StatusFights(uint32_t status) { return status < 32 && ((RPG_STATUS_FIGHT_MASK >> status) & 1u) != 0; }

// The status pick's sick gate: a sick bot never enters a status that fights
constexpr bool StatusAllowed(uint32_t status, bool sick) { return !sick || !StatusFights(status); }

// Event types of honest upkeep (.claude/docs/world-log.md in the hub)
constexpr char const* EVENT_TRAINER_VISIT = "TrainerVisit";  // spells learned, cost
constexpr char const* EVENT_RIDING_BOUGHT = "RidingBought";  // rank, cost, mount
constexpr char const* EVENT_SICK = "Sick";                   // src spirit_healer, id the spell, amount its duration ms
constexpr char const* EVENT_NEED_SHORT = "NeedShort";  // src = the kind word, amount; when a shortfall begins and ends
constexpr char const* EVENT_FLIGHT_FALL = "FlightFall";
}  // namespace Upkeep

#endif
