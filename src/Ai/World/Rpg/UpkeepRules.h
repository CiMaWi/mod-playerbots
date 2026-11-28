/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_UPKEEPRULES_H
#define _PLAYERBOT_UPKEEPRULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../../Bot/Debug/EventTypes.h"

// Honest upkeep of persistent bots: the kinds of free handouts, which of them are honest already and who still gets
// them (a bot that keeps no possessions). Player-free for tests/upkeep-selftest.cpp; Upkeep.h applies it to a bot. Kept
// out of PlayerbotAIConfig.h so that flipping a kind rebuilds only the handout sites.
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
// honest since upkeep WU40 (the class quests are the completion's, ClassQuestCompletion.h): a handout is off for every
// bot that keeps its possessions (Granted)
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

// A bot gets a handout only when it keeps no possessions (not persistent, or a master at a game client leads it):
// every persistent bot pays its own way (needs-and-errands NE.2)
constexpr bool Granted(bool keepsPossessions) { return !keepsPossessions; }

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
constexpr uint32_t RPG_STATUS_TRAVEL = 10;
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

// The status update ends every status a sick bot may not stay in, except a flight in the air (the bot lands, goes
// idle and then rests) and a goal recover handed over after the bot's last rise (SickErrandOpen let it start; it runs
// to its end, NR3): an errand, or the trip to the inn it binds at (`goalSinceRise`: the caller's, only for those). An
// errand or a trip from before the death ends as given up, as every other status does (WU8b)
constexpr bool SickEndsStatus(uint32_t status, bool sick, bool inFlight, bool goalSinceRise)
{
    return !inFlight && !StatusAllowed(status, sick) &&
           !((status == RPG_STATUS_ERRANDS || status == RPG_STATUS_TRAVEL) && goalSinceRise);
}

// Rest ends after its duration (persisted), a sick bot's rest only once the sickness has ended
constexpr bool RestEnds(bool persisted, bool sick) { return persisted && !sick; }

// The RPG statuses the errand trigger may interrupt (WT3b): those without a goal of their own, never questing
// (planning 3.19 (3.6): a planner bot's planner weighs a due need against its quest; a questing bot the planner does
// not lead destroys by worth instead, ItemWorthRules.h LootWaitsForTrip). A sick bot starts one only once recover has
// handed it over (SickErrandOpen)
constexpr uint32_t RPG_STATUS_ERRAND_INTERRUPTS_MASK = (1u << RPG_STATUS_IDLE) | (1u << RPG_STATUS_GO_GRIND) |
                                                       (1u << RPG_STATUS_GO_CAMP) | (1u << RPG_STATUS_WANDER_RANDOM) |
                                                       (1u << RPG_STATUS_WANDER_NPC) | (1u << RPG_STATUS_REST) |
                                                       (1u << RPG_STATUS_DO_GATHER);

// NR3 (NR.4): a sick bot takes a goal of its own only once awareness's recover is done with it: it stands on a clear
// point and its health and mana are restored (AwarenessRules::Restored, what recover waits for in a bot that is not
// sick). Which goal then is the goal model's sick gate (a goal that starts no fight and keeps to the safe area,
// SafeLeg); without one the bot rests. The repair is one errand among others, no longer the condition (NE11's was)
constexpr bool SickErrandOpen(bool sick, bool restored, bool clear) { return sick && restored && clear; }

constexpr bool ErrandMayStart(uint32_t status, bool sick, bool sickErrandOpen)
{
    bool const interrupts = status < 32 && ((RPG_STATUS_ERRAND_INTERRUPTS_MASK >> status) & 1u) != 0;
    return interrupts && (StatusAllowed(RPG_STATUS_ERRANDS, sick) || sickErrandOpen);
}

// NR3 (NR.4, stage 1): where a sick bot is safe. A place is safe in its own side's capital or a sanctuary city, inside
// the circle of a planner hub that is not the other side's, has a flight master or an innkeeper and no creature member
// hostile to the bot by its own reputation, or in its own side's zone whose level range ends at or below the bot's grey
// level. Contested zones and the other side's are never low. The map thread fills the facts (PlannerService), the
// planner job and the trip read the answers
struct SafeAreas
{
    uint32_t city = 0;           // the own capital or sanctuary the place lies in: the zone's id, or a sanctuary area's
    uint32_t lowZone = 0;        // the own side's low zone the place lies in
    std::vector<uint32_t> hubs;  // the safe hubs whose circle holds the place (planner hub indices)
    // Awareness AW38c: the safe hubs a quiet way on foot joins to the place (planner hub indices). Only the bot's own
    // place has any (Planner::SafeAreasHere, Awareness::QuietWalk)
    std::vector<uint32_t> roads;
};

// The city a place lies in: its zone when that is an own capital or a sanctuary, else its area when that alone is one
constexpr uint32_t SafeCity(uint32_t zoneId, uint32_t areaId, bool ownCapital, bool zoneSanctuary, bool areaSanctuary)
{
    return ownCapital || zoneSanctuary ? zoneId : areaSanctuary ? areaId : 0;
}

// A low zone: the bot's own side's, its range known and ending at or below the bot's grey level
constexpr bool LowZoneSafe(uint32_t zoneHigh, bool ownSide, uint32_t greyLevel)
{
    return ownSide && zoneHigh > 0 && zoneHigh <= greyLevel;
}

// A hub member counts as hostile at the reputation rank hostile or below (ReputationRank: hated 0, hostile 1)
constexpr int32_t HUB_HOSTILE_RANK = 1;
constexpr bool HostileMember(int32_t reaction) { return reaction <= HUB_HOSTILE_RANK; }

// A hub is safe when it is not the other side's (its own, neutral or of no side), offers a flight master or an
// innkeeper, and none of its creature members is hostile to the bot
constexpr bool HubSafe(bool otherSide, bool flightOrInn, bool hostileMember)
{
    return !otherSide && flightOrInn && !hostileMember;
}

// Awareness AW38c: a hub asked for a road from the bot's place is safe for it, does not hold the bot, lies outside the
// bot's own city and low zone (there it adds nothing) and has its circle within the errand walk's reach. The bot's
// sickness is the caller's. The nearest SICK_ROAD_HUBS of those are asked (tuning: one route search each)
constexpr bool RoadAsked(bool hubSafe, bool holdsBot, bool inOwnArea, bool withinReach)
{
    return hubSafe && !holdsBot && !inOwnArea && withinReach;
}
constexpr uint32_t SICK_ROAD_HUBS = 3;

inline bool IsSafe(SafeAreas const& place)
{
    return place.city || place.lowZone || !place.hubs.empty() || !place.roads.empty();
}

// The world log's word for where a sick bot was handed over (the upkeep row sick_area): the city first, then a hub,
// then a low zone, then a road; `outside` in none
inline char const* SafeAreaWord(SafeAreas const& place)
{
    return place.city             ? "city"
           : !place.hubs.empty()  ? "hub"
           : place.lowZone        ? "low_zone"
           : !place.roads.empty() ? "road"
                                  : "outside";
}

// Two places lie in one safe area: the same city, the same low zone or a hub circle holding both, or a hub circle
// holding one that a road joins to the other. Two places that each have a road to one hub share nothing
inline bool ShareArea(SafeAreas const& a, SafeAreas const& b)
{
    if ((a.city && a.city == b.city) || (a.lowZone && a.lowZone == b.lowZone))
        return true;
    auto const anyIn = [](std::vector<uint32_t> const& some, std::vector<uint32_t> const& other)
    {
        return std::any_of(some.begin(), some.end(), [&other](uint32_t hub)
                           { return std::find(other.begin(), other.end(), hub) != other.end(); });
    };
    return anyIn(a.hubs, b.hubs) || anyIn(a.roads, b.hubs) || anyIn(a.hubs, b.roads);
}

// The legs of a sick bot's route. A leg on foot starts and ends in one safe area; a flight or a transport joins two
// safe places; the hearthstone needs only its end safe: the cast moves nobody and it is the way in for a bot that rose
// outside
enum class SafeLegKind : uint8_t
{
    Foot,
    Flight,
    Transport,
    Hearthstone
};

inline bool SafeLeg(SafeLegKind kind, SafeAreas const& from, SafeAreas const& to)
{
    switch (kind)
    {
        case SafeLegKind::Foot:
            return ShareArea(from, to);
        case SafeLegKind::Hearthstone:
            return IsSafe(to);
        case SafeLegKind::Flight:
        case SafeLegKind::Transport:
            return IsSafe(from) && IsSafe(to);
    }
    return false;
}

// Event types of honest upkeep (.claude/docs/world-log.md in the hub): Sick (src spirit_healer, id the spell, amount
// its duration ms), FlightFall
// Batch G (class quests, WU20a-WU21c), registered at once so later steps touch no wide header
constexpr char const* SOURCE_CLASS_QUEST_FAILED = "class_quest_failed";  // a grant after the bot failed the quest
constexpr char const* REASON_CLASS_QUEST = "class_quest";  // a goal or circuit row the class-quest bonus decided
}  // namespace Upkeep

#endif
