/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTEXECRULES_H
#define _PLAYERBOT_QUESTEXECRULES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "QuestIndex.h"
#include "QuestPlaceRules.h"

// What the quest executor does next (questing WT35a, PLAN 3.2): one step function over the bot's quest log, the
// index's places, the capability table and the places tried in this run. The log is the state: the function keeps
// nothing, so a fresh start after a fight, a death, an errand or a logout returns what the run would have done next.
// No game header: tests/quest-exec-selftest.cpp checks it over generated quests and logs
namespace Planner
{
namespace QuestExec
{
// The work an objective asks for, as the capability table names it
enum class Work : std::uint8_t
{
    Kill,
    Loot,
    Object,  // a gameobject the objective names, used once (a goober)
    Chest,   // an item only containers hold, looted through the loot path (WT35e)
    Explore,
    Escort,
    Event,  // a script's event (QuestEvent::Script): a spell, a gossip, a scripted scene
    Item,   // an item no creature or object of the world drops: a vendor's, a made or a used item's
    Talk,   // a kill objective's friendly creature that gives its credit through its gossip (WT35f)
    Use,    // a quest item used at the objective's use rule target, through the item-use framework (WT35b)
    Cast,   // a class spell cast on the objective's use rule target (WT35b)
    Count
};
constexpr std::size_t WORKS = static_cast<std::size_t>(Work::Count);

// A row of the capability table: the work's name, the Status word that refuses it, and whether the executor does it
struct Capability
{
    char const* name;
    char const* refusal;
    bool done;
};

constexpr std::array<Capability, WORKS> CAPABILITIES = {{
    {"kill", "unsupported:kill", true},         // attacks the objective's creature it sought, not whatever stands near
    {"loot", "unsupported:loot", true},         // kills a source creature of the item and loots it
    {"object", "unsupported:object", true},     // uses the objective's object once through its handler (WT35e)
    {"chest", "unsupported:chest", true},       // walks to a container of the item and hands it to the loot path
    {"explore", "unsupported:explore", true},   // walks into the area trigger, its message through its handler
    {"escort", "unsupported:escort", false},    // unsupported by name (PLAN 3.2)
    {"event", "unsupported:event", false},      // unsupported by name (PLAN 3.2: script events)
    {"item", "unsupported:item", false},        // unsupported by name (PLAN 3.2); made and used items WT35g/WT35b
    {"talk", "unsupported:talk_option", true},  // opens the gossip and picks the option the gather ties to the credit
    {"use", "unsupported:use", true},           // seeks the rule's target, walks into its range, uses the item
    {"cast", "unsupported:cast", true},         // casts its own class spell on the rule's target (WT35b)
}};

inline Capability const& CapabilityOf(Work work) { return CAPABILITIES[static_cast<std::size_t>(work)]; }

constexpr std::uint8_t ANY_TEAM = 2;  // WorkOf without a side: a creature friendly to either is talked to

// The work of an objective of the index; Count for one the executor does not work toward (the turn-in). An item is
// loot when a creature drops it (its corpse or its skin), a chest's when only objects hold it; a kill objective's
// creature friendly to the bot's side with a talk credit is talked to (WT35f)
inline Work WorkOf(QuestObjective const& objective, std::uint8_t team = ANY_TEAM)
{
    std::uint8_t const sides = team < ANY_TEAM ? static_cast<std::uint8_t>(1u << team) : std::uint8_t{3};
    // WT35b: a use rule wins over the objective's kind (QuestUseRules.h is the one source); the object itself used
    // (a rule without a spell) is WT35g's
    if (objective.use == UseHow::Item)
        return Work::Use;
    if (objective.use == UseHow::Spell)
        return Work::Cast;
    switch (objective.kind)
    {
        case ObjectiveKind::Kill:
            return objective.talk.how != TalkHow::None && (objective.talk.friendlyTo & sides) ? Work::Talk : Work::Kill;
        case ObjectiveKind::Item:
            if (objective.sourceKinds & (SourceBit(SourceKind::Corpse) | SourceBit(SourceKind::Skin)))
                return Work::Loot;
            return (objective.sourceKinds & SourceBit(SourceKind::Object)) ? Work::Chest : Work::Item;
        case ObjectiveKind::Object:
            return Work::Object;
        case ObjectiveKind::Explore:
            return Work::Explore;
        default:
            return Work::Count;
    }
}

// Whether the executor refuses an objective of this work: a row it does not do, or a talk whose option the gather
// could not tell (WT35f)
inline bool Refuses(QuestObjective const& objective, Work work)
{
    return work != Work::Count &&
           (!CapabilityOf(work).done || (work == Work::Talk && objective.talk.how == TalkHow::Unknown));
}

// WT35g: the chain "use, then kill" of a kill objective whose use brings its creature forth (summoned at the anchor,
// the target turned into it). It keeps no memory: a hostile creature of the objective in sight is held and killed,
// else the use is the step; a friendly or neutral one the use made (a cured beast) is left. A missing link ends as the
// use does (no_item, no_target at the places)
inline bool KillsAfterUse(QuestObjective const& objective)
{
    return objective.kind == ObjectiveKind::Kill && objective.brings != UseBrings::Credit &&
           (objective.use == UseHow::Item || objective.use == UseHow::Spell);
}

// WT35b: the quest's objectives, then its event when a use rule completes it (QuestRecord::eventUse, POI_NONE)
inline std::uint32_t ObjectiveCount(QuestRecord const& quest)
{
    return static_cast<std::uint32_t>(quest.objectives.size()) + (quest.eventUse.use != UseHow::None ? 1 : 0);
}

inline QuestObjective const& ObjectiveAt(QuestRecord const& quest, std::uint32_t i)
{
    return i < quest.objectives.size() ? quest.objectives[i] : quest.eventUse;
}

enum class Act : std::uint8_t
{
    Work,      // work this objective at this place
    TurnIn,    // every objective is met
    Refused,   // an open objective has no capability row: the order ends with its refusal word
    NoPlace,   // no open objective has a usable place on this map
    NoTarget,  // every place of the open objectives was tried in this run without a target
    Done,      // the quest is not in the log
    NoEnder    // WT35h: complete, and every ender on this map the bot can use was tried in this run
};

struct Step
{
    Act act{Act::Done};
    Work work{Work::Count};
    std::uint32_t objective{0};  // index in QuestRecord::objectives
    std::uint32_t area{0};       // index in that objective's areas
    AreaPoint const* place{nullptr};

    bool operator==(Step const& other) const
    {
        return act == other.act && work == other.work && objective == other.objective && area == other.area;
    }
};

// A place of the run, by its objective and area; the places tried are kept only while the run lasts
using PlaceKey = std::uint16_t;
constexpr PlaceKey Key(std::uint32_t objective, std::uint32_t area)
{
    return static_cast<PlaceKey>((objective << 8) | (area & 0xFFu));
}
constexpr PlaceKey KeyOf(Step const& step) { return Key(step.objective, step.area); }
constexpr std::uint32_t ObjectiveOf(PlaceKey key) { return static_cast<std::uint32_t>(key >> 8); }

inline bool WasTried(std::vector<PlaceKey> const& tried, PlaceKey key)
{
    return std::find(tried.begin(), tried.end(), key) != tried.end();
}

// What the log says of the quest as a whole
struct LogState
{
    bool inLog{false};
    bool complete{false};
};

// A place the run may go to next
struct Candidate
{
    std::uint32_t objective{0};
    std::uint32_t area{0};
    AreaPoint const* place{nullptr};
    float yards{0.0f};  // from the bot, in the plane
};

// The next step. `open(poiIndex)` says whether the objective in that quest-log slot still needs progress, and
// open(POI_NONE) whether the quest's exploration or event has not happened yet (one flag in the log for both);
// `hasSkill(id)` whether the bot has a skill; `prefer(candidates)` is a planner's pick among the candidates (nearest
// first), -1 for the nearest. `skip` is the place a stay with progress left (WT33b), passed over while the objective
// has another; `tried` the places tried in this run without a target
template <typename Open, typename HasSkill, typename Prefer>
Step Next(QuestRecord const& quest, LogState log, Open open, std::uint32_t mapId, float x, float y, std::uint8_t team,
          HasSkill hasSkill, std::vector<PlaceKey> const& tried, QuestPlaces::Skip const& skip, Prefer prefer)
{
    if (!log.inLog)
        return {Act::Done};
    if (log.complete)
        return {Act::TurnIn};

    // Refused at the order's start, before any walk: an event or an open objective the executor has no interaction for
    Work const eventWork = quest.event == QuestEvent::Script ? WorkOf(quest.eventUse, team) : Work::Count;
    if (quest.event != QuestEvent::None && open(POI_NONE) &&
        (eventWork == Work::Count || !CapabilityOf(eventWork).done))
        return {Act::Refused,
                quest.event == QuestEvent::Escort ? Work::Escort : (eventWork == Work::Count ? Work::Event : eventWork),
                static_cast<std::uint32_t>(quest.objectives.size())};
    for (std::uint32_t i = 0; i < ObjectiveCount(quest); ++i)
    {
        QuestObjective const& objective = ObjectiveAt(quest, i);
        Work const work = WorkOf(objective, team);
        if (Refuses(objective, work) && open(objective.poiIndex))
            return {Act::Refused, work, i};
    }

    std::vector<Candidate> candidates;
    bool placed = false;  // an open objective has a usable place on this map, tried or not
    for (std::uint32_t i = 0; i < ObjectiveCount(quest); ++i)
    {
        QuestObjective const& objective = ObjectiveAt(quest, i);
        if (WorkOf(objective, team) == Work::Count || !open(objective.poiIndex))
            continue;
        std::size_t const first = candidates.size();
        std::size_t left = SIZE_MAX;
        for (std::uint32_t a = 0; a < objective.areas.size(); ++a)
        {
            AreaPoint const& area = objective.areas[a];
            if (area.mapId != mapId || !AreaUsable(area, team, hasSkill))
                continue;
            placed = true;
            if (WasTried(tried, Key(i, a)))
                continue;
            if (skip.on && std::fabs(area.x - skip.x) < 1.0f && std::fabs(area.y - skip.y) < 1.0f)
                left = candidates.size();
            candidates.push_back({i, a, &area, std::hypot(area.x - x, area.y - y)});
        }
        // The place a stay left is passed over while the objective has another (QuestPlaces::PlaceOf)
        if (left != SIZE_MAX && candidates.size() - first > 1)
            candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(left));
    }
    if (candidates.empty())
        return {placed ? Act::NoTarget : Act::NoPlace};

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](Candidate const& a, Candidate const& b) { return a.yards < b.yards; });
    int const preferred = prefer(static_cast<std::vector<Candidate> const&>(candidates));
    Candidate const& chosen = candidates[preferred >= 0 && static_cast<std::size_t>(preferred) < candidates.size()
                                             ? static_cast<std::size_t>(preferred)
                                             : 0];
    return {Act::Work, WorkOf(ObjectiveAt(quest, chosen.objective), team), chosen.objective, chosen.area, chosen.place};
}

// Whether a step being worked still stands: its objective is open and its place not tried. A step that does not
// stand is taken again by Next
template <typename Open>
bool Stands(QuestRecord const& quest, Step const& step, Open open, std::vector<PlaceKey> const& tried)
{
    return step.act == Act::Work && step.objective < ObjectiveCount(quest) &&
           open(ObjectiveAt(quest, step.objective).poiIndex) && !WasTried(tried, KeyOf(step));
}

// The word a step ends the order with (refused, no place, no target), nullptr for a step that does not end it. The
// order's start and a status pick ask it right after the start, so a refusal comes before any walk or fight.
// `locked`: the run passed over a container it could not open (WT35e), so places that ran out end as locked
constexpr char const* LOCKED = "locked";
inline char const* EndWord(Step const& step, bool locked = false)
{
    switch (step.act)
    {
        case Act::Refused:
            return CapabilityOf(step.work).refusal;
        case Act::NoPlace:
            return "no_place";
        case Act::NoTarget:
            return locked ? LOCKED : "no_target";
        case Act::NoEnder:
            return "no_ender";
        default:
            return nullptr;
    }
}

// WT35h: the turn-in's ender, nearest first: a Turnin area on the map the bot can use, not tried in this run (TurnIn
// with its key); every one tried or unusable is NoEnder, none on the map NoPlace
inline Step NextEnder(QuestRecord const& quest, std::uint32_t mapId, float x, float y, std::uint8_t team,
                      std::vector<PlaceKey> const& tried)
{
    Step next{Act::NoPlace};
    float best = 0.0f;
    for (std::uint32_t i = 0; i < quest.objectives.size(); ++i)
        for (std::uint32_t a = 0;
             quest.objectives[i].kind == ObjectiveKind::Turnin && a < quest.objectives[i].areas.size(); ++a)
        {
            AreaPoint const& area = quest.objectives[i].areas[a];
            if (area.mapId != mapId)
                continue;
            float const yards = std::hypot(area.x - x, area.y - y);
            if (next.act == Act::NoPlace)
                next.act = Act::NoEnder;
            if (AreaUsable(area, team, [](std::uint16_t) { return true; }) && !WasTried(tried, Key(i, a)) &&
                (!next.place || yards < best))
            {
                next = {Act::TurnIn, Work::Count, i, a, &area};
                best = yards;
            }
        }
    return next;
}

// WT35h: whether a walk to the ender `key` that gained nothing tries that ender: only while another usable ender on the
// map is left untried. The last one's walk starts again instead (travel's refusals mid-route pass: WT35h's matrix,
// q791 and q86), bounded by the order's give-up clock (Pursuit NO_ENDER); a look at the ender still tries it for good
inline bool StallTries(QuestRecord const& quest, std::uint32_t mapId, float x, float y, std::uint8_t team,
                       std::vector<PlaceKey> tried, PlaceKey key)
{
    tried.push_back(key);
    return NextEnder(quest, mapId, x, y, team, tried).act == Act::TurnIn;
}

// What a seek or an engagement sees of an object (WT35e): spawned and ready (not in use, selectable, active for the
// bot's quests), passed over in this run, a container and whether it holds an item the bot needs, in reach, and for
// a container in reach whether the loot path can open it (its key or skill, LootObject::IsLootPossible)
struct ObjectSeen
{
    bool present{false};
    bool ready{false};
    bool passed{false};
    bool chest{false};
    bool needed{false};
    bool inReach{false};
    bool opens{false};
};

enum class ObjectAct : std::uint8_t
{
    Skip,    // gone, passed over, or a container with nothing the bot needs: the next object
    Wait,    // in use (another's use, a goober's close time): not held, but the place is not tried while it lasts
    Walk,    // into reach
    Locked,  // a container the bot cannot open: passed over, and the run remembers it
    Loot,    // a container: the loot path opens and loots it
    Use      // any other object: one use through its handler
};

inline ObjectAct ObjectVerdict(ObjectSeen const& seen)
{
    if (!seen.present || seen.passed)
        return ObjectAct::Skip;
    if (!seen.ready)
        return ObjectAct::Wait;
    if (seen.chest && !seen.needed)
        return ObjectAct::Skip;
    if (!seen.inReach)
        return ObjectAct::Walk;
    if (seen.chest)
        return seen.opens ? ObjectAct::Loot : ObjectAct::Locked;
    return ObjectAct::Use;
}

// What a seek or an engagement sees of a talk creature (WT35f): alive and in the world, passed over in this run, one
// the bot can talk to (an npc flag, not hostile), in reach and in sight; for a held one whether the credit needs an
// option, whether its hello and its select were sent, whether the open menu shows the option and the hold is over
struct TalkSeen
{
    bool present{false};
    bool passed{false};
    bool usable{false};
    bool inReach{false};
    bool select{false};
    bool helloSent{false};
    bool selected{false};
    bool optionShown{false};
    bool holdOver{false};
};

enum class TalkAct : std::uint8_t
{
    Skip,        // gone or passed over: the next creature
    NoInteract,  // no talk now (no npc flag, hostile, a script's phase): not held, the place's look clock runs
    Walk,        // into reach
    Hello,       // the gossip opened through its handler
    Select,      // the option picked through its handler
    NoOption,    // the open menu lacks the option (a condition hides it): passed over
    Hold,        // the handler's credit: the log decides
    Done         // the hold is over: written off
};

inline TalkAct TalkVerdict(TalkSeen const& seen)
{
    if (seen.helloSent)
    {
        if (seen.select && !seen.selected)
            return seen.optionShown ? TalkAct::Select : TalkAct::NoOption;
        return seen.holdOver ? TalkAct::Done : TalkAct::Hold;
    }
    if (!seen.present || seen.passed)
        return TalkAct::Skip;
    if (!seen.usable)
        return TalkAct::NoInteract;
    return seen.inReach ? TalkAct::Hello : TalkAct::Walk;
}

// An area trigger (WT35f): outside it the bot walks to its centre (a sphere counts its height, a box is reached at
// its centre); inside it the client's message goes once through the handler; then a hold, and the place is tried
// when the log's flag stays unset (a script swallowed the message)
enum class ExploreAct : std::uint8_t
{
    Walk,
    Send,
    Hold,
    Tried
};

inline ExploreAct ExploreVerdict(bool inside, bool sent, bool holdOver)
{
    if (sent)
        return holdOver ? ExploreAct::Tried : ExploreAct::Hold;
    return inside ? ExploreAct::Send : ExploreAct::Walk;
}

// The executor's clocks (WT35a; WT35c owns the pursuit later). Each counts the time between the executor's ticks, a
// pause longer than CLOCK_GAP_MS (a fight, the loot, another action) as CLOCK_GAP_MS: a pause holds a clock, never
// restarts it, and every clock still runs out
constexpr std::uint32_t CLOCK_GAP_MS = 5000;
constexpr std::uint32_t PURSUIT_MS = 45000;     // the engage time a held target gets to confirm, then it is passed over
constexpr std::uint32_t WALK_STALL_MS = 30000;  // a walk that gains nothing this long is given up (travel's own: 90 s)
constexpr float WALK_GAIN_YARDS = 5.0f;         // a gain: this much closer than the walk's nearest so far
constexpr std::size_t PASSED_TARGETS = 4;       // the targets passed over last, which a seek skips

// What a tick saw between it and the one before
enum class Pause : std::uint8_t
{
    None,
    Combat,  // the tick is in a fight: the pursuit holds
    Gap      // the ticks lie more than CLOCK_GAP_MS apart
};

inline std::uint32_t Counted(bool first, std::uint32_t sinceLastMs)
{
    return first ? 0 : std::min(sinceLastMs, CLOCK_GAP_MS);
}

// One tick of the pursuit of a held target. `ticking`: the tick before counted (false before the first and in a fight)
inline Pause Pursue(std::uint32_t& engagedMs, bool& ticking, bool inCombat, std::uint32_t sinceLastMs)
{
    if (inCombat)
    {
        bool const was = ticking;
        ticking = false;
        return was ? Pause::Combat : Pause::None;
    }
    Pause const pause = ticking && sinceLastMs > CLOCK_GAP_MS ? Pause::Gap : Pause::None;
    engagedMs += Counted(!ticking, sinceLastMs);
    ticking = true;
    return pause;
}

inline bool PursuitOver(std::uint32_t engagedMs) { return engagedMs >= PURSUIT_MS; }

// One tick of a walk toward a held target or a place, `yards` from its end; `nearest` < 0 before its first tick.
// A gain restarts its watch; after a pause the walk is measured from where the bot stands (the pause moved it), its
// time held. True: the walk is given up (the target passed over, the place tried), before travel's give-up would end
// the order
inline bool WalkStalled(float& nearest, std::uint32_t& stalledMs, bool first, std::uint32_t sinceLastMs, float yards)
{
    if (!first && sinceLastMs > CLOCK_GAP_MS)
        nearest = -1.0f;
    stalledMs += Counted(first, sinceLastMs);
    if (nearest < 0.0f)
        nearest = yards;
    else if (yards + WALK_GAIN_YARDS < nearest)
    {
        nearest = yards;
        stalledMs = 0;
    }
    return stalledMs >= WALK_STALL_MS;
}

// The targets passed over last, a ring of PASSED_TARGETS (an empty id is none)
template <typename Id, std::size_t N>
void Remember(std::array<Id, N>& passed, std::uint8_t& next, Id const& id)
{
    static_assert(N == PASSED_TARGETS, "the passed set holds PASSED_TARGETS");
    passed[next % N] = id;
    next = static_cast<std::uint8_t>((next + 1) % N);
}

template <typename Id, std::size_t N>
bool Remembered(std::array<Id, N> const& passed, Id const& id)
{
    return id != Id{} && std::find(passed.begin(), passed.end(), id) != passed.end();
}
}  // namespace QuestExec
}  // namespace Planner

#endif
