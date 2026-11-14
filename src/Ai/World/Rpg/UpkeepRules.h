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
// One kind per row (clang-format would pack the short rows into columns)
// clang-format off
constexpr std::array<KindRule, KIND_COUNT> KIND_RULES = {{
    {"Food", true},  // food and water
    {"Ammo", true},
    {"Reagent", true},
    {"Poison", true},      // a rogue's InitConsumables
    {"Consumable", true},  // potions, weapon oils and stones
    {"Repair", true},
    {"Resurrect", true},     // in place, or at the spirit healer without sickness and durability loss
    {"TrainerSpell", true},  // class and trainer spells; WU15b: bought at the class trainer (WU13)
    {"WeaponSkill", true},   // InitSkills; WU15b: bought at weapon masters (WU15a), professions stay
    {"Riding", true},        // the riding ranks by level (InitSkills); WU15b: bought (WU14)
    {"Mount", true},
    {"SpecialSpell", true},
    {"ClassQuest", false},  // spells of class quests the bot never did
    {"Pet", false},         // pet and pet talents
    {"Glyph", true},
    {"Key", true},  // keyring
    {"Reputation", true},
    {"Attunement", true},  // completed attunement quests
    {"Gold", true},
    {"Talent", true},  // the talent reset below the persistence level
}};
// clang-format on

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

// Resurrection sickness, which the core's spirit healer path casts (Player::ResurrectPlayer): while it lasts a bot
// rests (upkeep plan §3.5; the status pick, the status update and the errand start until WU16 gives the planner "sick
// until")
constexpr uint32_t SPELL_RESURRECTION_SICKNESS = 15007;

// The NewRpgStatus values whose activity fights (PlayerbotAIConfig.h; NewRpgBaseAction.cpp asserts them): grinding and
// the wander that picks a grind target, quests, outdoor PvP and gathering among mobs. A bot that needs rest (low on
// health or mana with nothing to eat or drink, §3.6) picks none of them
constexpr uint32_t RPG_STATUS_GO_GRIND = 1;
constexpr uint32_t RPG_STATUS_WANDER_RANDOM = 3;
constexpr uint32_t RPG_STATUS_DO_QUEST = 5;
constexpr uint32_t RPG_STATUS_OUTDOOR_PVP = 8;
constexpr uint32_t RPG_STATUS_DO_GATHER = 9;
constexpr uint32_t RPG_STATUS_FIGHT_MASK = (1u << RPG_STATUS_GO_GRIND) | (1u << RPG_STATUS_WANDER_RANDOM) |
                                           (1u << RPG_STATUS_DO_QUEST) | (1u << RPG_STATUS_OUTDOOR_PVP) |
                                           (1u << RPG_STATUS_DO_GATHER);

constexpr bool StatusFights(uint32_t status) { return status < 32 && ((RPG_STATUS_FIGHT_MASK >> status) & 1u) != 0; }

// A sick bot rests (WU8b-fix 2): revived at the spirit healer, a bot that walked off again (to a vendor, a camp, an
// npc, on a trip) went back through the mobs that had killed it, again and again (WU8b: about 40 deaths while sick in
// each session). Only rest and the idle status that picks it stay open; rest lasts until the sickness ends, as players
// wait at the graveyard
constexpr uint32_t RPG_STATUS_IDLE = 0;
constexpr uint32_t RPG_STATUS_GO_CAMP = 2;
constexpr uint32_t RPG_STATUS_WANDER_NPC = 4;
constexpr uint32_t RPG_STATUS_REST = 7;
constexpr uint32_t RPG_STATUS_ERRANDS = 11;
constexpr uint32_t RPG_STATUS_SICK_OPEN_MASK = (1u << RPG_STATUS_IDLE) | (1u << RPG_STATUS_REST);

// The sick gate of the status pick, the status update and the errand start: a sick bot is only idle or rests
constexpr bool StatusAllowed(uint32_t status, bool sick)
{
    return !sick || (status < 32 && ((RPG_STATUS_SICK_OPEN_MASK >> status) & 1u) != 0);
}

// The status pick: a sick bot rests, a bot that needs rest picks no status that fights
constexpr bool StatusPickable(uint32_t status, bool sick, bool needsRest)
{
    return StatusAllowed(status, sick) && !(needsRest && StatusFights(status));
}

// The status update ends every status a sick bot may not stay in (an errand or a trip as given up), except a flight in
// the air: the bot lands, goes idle and then rests
constexpr bool SickEndsStatus(uint32_t status, bool sick, bool inFlight)
{
    return !inFlight && !StatusAllowed(status, sick);
}

// Rest ends after its duration (persisted), a sick bot's rest only once the sickness has ended
constexpr bool RestEnds(bool persisted, bool sick) { return persisted && !sick; }

// The RPG statuses an errand may interrupt (WT3b): those without a goal of their own. A due upkeep need (gear due for
// repair, a due stock the gold pays) also ends questing: a hunter without arrows goes and buys them, as a player
// would; full bags alone do not (a questing bot destroys by worth instead, ItemWorthRules.h LootWaitsForTrip). A sick
// bot starts no errand
constexpr uint32_t RPG_STATUS_ERRAND_INTERRUPTS_MASK = (1u << RPG_STATUS_IDLE) | (1u << RPG_STATUS_GO_GRIND) |
                                                       (1u << RPG_STATUS_GO_CAMP) | (1u << RPG_STATUS_WANDER_RANDOM) |
                                                       (1u << RPG_STATUS_WANDER_NPC) | (1u << RPG_STATUS_REST) |
                                                       (1u << RPG_STATUS_DO_GATHER);

constexpr bool ErrandMayStart(uint32_t status, bool needDue, bool sick)
{
    bool const interrupts = status < 32 && (((RPG_STATUS_ERRAND_INTERRUPTS_MASK >> status) & 1u) != 0 ||
                                            (needDue && status == RPG_STATUS_DO_QUEST));
    return interrupts && StatusAllowed(RPG_STATUS_ERRANDS, sick);
}

// Event types of honest upkeep (.claude/docs/world-log.md in the hub)
constexpr char const* EVENT_TRAINER_VISIT = "TrainerVisit";  // spells learned, cost
constexpr char const* EVENT_RIDING_BOUGHT = "RidingBought";  // rank, cost, mount
constexpr char const* EVENT_SICK = "Sick";                   // src spirit_healer, id the spell, amount its duration ms
constexpr char const* EVENT_NEED_SHORT = "NeedShort";  // src = the kind word, amount; when a shortfall begins and ends
constexpr char const* EVENT_FLIGHT_FALL = "FlightFall";
// Batch G (class quests, WU20a-WU21c), registered at once so later steps touch no wide header
constexpr char const* EVENT_PET_TAMED = "PetTamed";                          // a hunter tamed a beast: id the creature
constexpr char const* SOURCE_CLASS_QUEST_GIVEN_UP = "class_quest_given_up";  // a grant after the bot gave the quest up
constexpr char const* REASON_CLASS_QUEST = "class_quest";  // a goal or circuit row the class-quest bonus decided
}  // namespace Upkeep

#endif
