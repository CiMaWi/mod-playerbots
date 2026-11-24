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

#include "../../../Bot/Debug/EventTypes.h"
#include "../ShareSlot.h"

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
    Talent,
    Profession  // the professions plan (WR1): professions, their ranks and recipes the factory hands out
};

namespace Upkeep
{
// word: the kind's honesty row is Honesty<word> (EventTypes.h) and the kind's name in NeedShort events. Every kind is
// honest since upkeep WU40 (the class quests are the completion's, ClassQuestCompletion.h): a handout is off for a
// persistent bot outside the handout share (Granted)
struct KindRule
{
    EventLog::HonestyKind kind;
};

constexpr size_t KIND_COUNT = 21;
// One kind per row (clang-format would pack the short rows into columns)
// clang-format off
constexpr std::array<KindRule, KIND_COUNT> KIND_RULES = {{
    {EventLog::HonestyKind::Food},  // food and water
    {EventLog::HonestyKind::Ammo},
    {EventLog::HonestyKind::Reagent},
    {EventLog::HonestyKind::Poison},      // a rogue's InitConsumables
    {EventLog::HonestyKind::Consumable},  // potions, weapon oils and stones
    {EventLog::HonestyKind::Repair},
    {EventLog::HonestyKind::Resurrect},     // in place, or at the spirit healer without sickness and durability loss
    {EventLog::HonestyKind::TrainerSpell},  // class and trainer spells; WU15b: bought at the class trainer (WU13)
    {EventLog::HonestyKind::WeaponSkill},   // InitSkills; WU15b: bought at weapon masters (WU15a)
    {EventLog::HonestyKind::Riding},        // the riding ranks by level (InitSkills); WU15b: bought (WU14)
    {EventLog::HonestyKind::Mount},
    {EventLog::HonestyKind::SpecialSpell},
    {EventLog::HonestyKind::ClassQuest},  // the class-quest completion's rows (WU21a); no handout since WU40
    {EventLog::HonestyKind::Pet},         // the pet; WU21b: tamed at a beast's spawn (pet talents are free points, no handout)
    {EventLog::HonestyKind::Glyph},
    {EventLog::HonestyKind::Key},  // keyring
    {EventLog::HonestyKind::Reputation},
    {EventLog::HonestyKind::Attunement},  // completed attunement quests
    {EventLog::HonestyKind::Gold},
    {EventLog::HonestyKind::Talent},  // the talent reset below the persistence level
    {EventLog::HonestyKind::Profession},  // professions, ranks, tools (the factory, the fishing pole); honest since professions WR5
}};
// clang-format on

// The upkeep's kinds are the first honesty kinds, in the same order (EventTypes.h owns the words)
static_assert(static_cast<size_t>(UpkeepKind::Profession) + 1 == KIND_COUNT &&
                  static_cast<size_t>(UpkeepKind::Profession) ==
                      static_cast<size_t>(EventLog::HonestyKind::Profession) &&
                  static_cast<size_t>(UpkeepKind::Gold) == static_cast<size_t>(EventLog::HonestyKind::Gold),
              "UpkeepKind is the first run of HonestyKind");

constexpr EventLog::HonestyKind HonestyKindOf(UpkeepKind kind)
{
    return KIND_RULES[static_cast<size_t>(kind) < KIND_COUNT ? static_cast<size_t>(kind) : 0].kind;
}

// The kind's word (a NeedShort source; the honesty row is Honesty + the word); "Other" out of range. The words are
// literals: the view's data ends with a NUL
constexpr char const* KindWord(UpkeepKind kind)
{
    size_t const index = static_cast<size_t>(kind);
    return index < KIND_COUNT ? EventLog::HonestyKindWord(KIND_RULES[index].kind).data() : "Other";
}

// The handout share (Playerbots.Upkeep.HandoutShare): the guid mix of ShareSlot.h, which world PvP's start-cost
// share uses too, so SQL names the same bots
using BotShare::InShare;
using BotShare::SHARE_MIX;
using BotShare::SHARE_PERCENT_MAX;
using BotShare::SHARE_SHIFT;
using BotShare::ShareSlot;

// A bot gets a handout unless it keeps its possessions and is outside the share
constexpr bool Granted(bool keepsPossessions, uint32_t guidCounter, uint32_t sharePercent)
{
    return !keepsPossessions || InShare(guidCounter, sharePercent);
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

// The RPG statuses the errand trigger may interrupt (WT3b): those without a goal of their own, never questing
// (planning 3.19 (3.6): a planner bot's planner weighs a due need against its quest; a questing bot the planner does
// not lead destroys by worth instead, ItemWorthRules.h LootWaitsForTrip). A sick bot starts no errand
constexpr uint32_t RPG_STATUS_ERRAND_INTERRUPTS_MASK = (1u << RPG_STATUS_IDLE) | (1u << RPG_STATUS_GO_GRIND) |
                                                       (1u << RPG_STATUS_GO_CAMP) | (1u << RPG_STATUS_WANDER_RANDOM) |
                                                       (1u << RPG_STATUS_WANDER_NPC) | (1u << RPG_STATUS_REST) |
                                                       (1u << RPG_STATUS_DO_GATHER);

constexpr bool ErrandMayStart(uint32_t status, bool sick)
{
    bool const interrupts = status < 32 && ((RPG_STATUS_ERRAND_INTERRUPTS_MASK >> status) & 1u) != 0;
    return interrupts && StatusAllowed(RPG_STATUS_ERRANDS, sick);
}

// Event types of honest upkeep (.claude/docs/world-log.md in the hub): Sick (src spirit_healer, id the spell, amount
// its duration ms), FlightFall
// Batch G (class quests, WU20a-WU21c), registered at once so later steps touch no wide header
constexpr char const* SOURCE_CLASS_QUEST_FAILED = "class_quest_failed";  // a grant after the bot failed the quest
constexpr char const* REASON_CLASS_QUEST = "class_quest";  // a goal or circuit row the class-quest bonus decided
}  // namespace Upkeep

#endif
