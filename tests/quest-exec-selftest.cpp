/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the quest executor's step function (questing WT35a, QuestExecRules.h): dispatch by kind through
// the capability table (kill, loot; object, explore, escort, a script's event and an item without a world source
// refused by name), a loot objective's seek among its item's sources (QuestIndex.h SeekEntries), a met objective
// never worked, a place without a target leading to the objective's next place, places running out as no_target (none
// on the map: no_place), the place a stay left passed over, a planner's pick; and the resume invariant, searched over
// generated quests and logs: at every point of a run (progress, an empty place, an interruption) a fresh start over the
// same log and the same tried places returns the step the run itself takes next; the end at the order's start (an
// escort refused before any work, no place, no target); the pursuit and walk clocks (a fight or a gap holds them, never
// restarts them, and they still run out; a walk gives up before travel's 90 s) and the set of targets passed over.
// Mutants that break each rule must fail. Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I
// src/Ai/World/Rpg /I src/Bot
//   tests/quest-exec-selftest.cpp
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "PursuitRules.h"
#include "QuestExecRules.h"

using namespace Planner;
using namespace Planner::QuestExec;

namespace
{
constexpr std::uint32_t MAP_A = 0;
constexpr std::uint32_t MAP_B = 1;
constexpr std::uint8_t ALLIANCE = 0;
constexpr std::uint8_t HORDE = 1;
constexpr std::size_t SLOTS = 10;  // 4 creature or object slots, 6 item slots
constexpr int RUNS = 600;
constexpr int RUN_STEPS = 200;

struct Tally
{
    int passed{0};
    int failed{0};
    bool print{true};
};

Tally total;
Tally* tally = &total;

void Check(bool pass, std::string const& text)
{
    if (pass)
        ++tally->passed;
    else
    {
        ++tally->failed;
        if (tally->print)
            std::printf("FAIL: %s\n", text.c_str());
    }
}

// The bot's quest and its log: the counts it has per quest-log slot, its position, the places tried in this run
struct Situation
{
    QuestRecord quest;
    std::array<std::uint32_t, SLOTS> have{};
    bool inLog{true};
    std::uint32_t mapId{MAP_A};
    float x{0.0f};
    float y{0.0f};
    std::uint8_t team{ALLIANCE};
    std::vector<PlaceKey> tried;
    bool explored{false};  // the log's one flag for the quest's exploration or event
    QuestPlaces::Skip skip;
    bool plan{false};  // a planner bot: it prefers the candidate nearest its plan's point
    float planX{0.0f};
    float planY{0.0f};
};

bool SlotOpen(Situation const& s, std::int32_t slot)
{
    if (slot == POI_NONE)
    {
        bool flagged = s.quest.event != QuestEvent::None;
        for (QuestObjective const& objective : s.quest.objectives)
            flagged = flagged || objective.kind == ObjectiveKind::Explore;
        return flagged && !s.explored;
    }
    if (slot < 0 || static_cast<std::size_t>(slot) >= SLOTS)
        return false;
    for (QuestObjective const& objective : s.quest.objectives)
        if (objective.poiIndex == slot)
            return s.have[static_cast<std::size_t>(slot)] < objective.count;
    return false;
}

bool AnyOpen(Situation const& s)
{
    for (QuestObjective const& objective : s.quest.objectives)
        if (objective.kind != ObjectiveKind::Turnin && SlotOpen(s, objective.poiIndex))
            return true;
    return SlotOpen(s, POI_NONE);
}

LogState Log(Situation const& s) { return {s.inLog, s.inLog && !AnyOpen(s)}; }

using StepFn = std::function<Step(Situation const&)>;
using Maker = std::function<StepFn()>;

int PlanPick(Situation const& s, std::vector<Candidate> const& candidates)
{
    if (!s.plan)
        return -1;
    int best = -1;
    float bestYards = 0.0f;
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        float const yards = std::hypot(candidates[i].place->x - s.planX, candidates[i].place->y - s.planY);
        if (best < 0 || yards < bestYards)
        {
            best = static_cast<int>(i);
            bestYards = yards;
        }
    }
    return best;
}

template <typename Open>
Step RealStep(Situation const& s, Open open, std::vector<PlaceKey> const& tried)
{
    return Next(
        s.quest, Log(s), open, s.mapId, s.x, s.y, s.team, [](std::uint16_t) { return true; }, tried, s.skip,
        [&s](std::vector<Candidate> const& candidates) { return PlanPick(s, candidates); });
}

Step Real(Situation const& s)
{
    return RealStep(s, [&s](std::int32_t slot) { return SlotOpen(s, slot); }, s.tried);
}

Maker const REAL = [] { return StepFn(Real); };

// Mutants: each breaks one rule the checks below must see
Maker const WORKS_MET = []
{
    return StepFn([](Situation const& s) { return RealStep(s, [](std::int32_t slot) { return slot >= 0; }, s.tried); });
};
Maker const FORGETS_TRIED = []
{
    return StepFn([](Situation const& s)
                  { return RealStep(s, [&s](std::int32_t slot) { return SlotOpen(s, slot); }, {}); });
};
Maker const GRINDS_UNSUPPORTED = []
{
    return StepFn(
        [](Situation const& s)
        {
            auto const supported = [&s](std::int32_t slot)
            {
                if (slot == POI_NONE)
                    return false;
                for (QuestObjective const& objective : s.quest.objectives)
                    if (objective.poiIndex == slot && Refuses(objective, WorkOf(objective, s.team)))
                        return false;
                return SlotOpen(s, slot);
            };
            return RealStep(s, supported, s.tried);
        });
};
Maker const NO_PLACE_FOR_NO_TARGET = []
{
    return StepFn(
        [](Situation const& s)
        {
            Step step = Real(s);
            if (step.act == Act::NoTarget)
                step.act = Act::NoPlace;
            return step;
        });
};
// Hidden state: keeps to the objective it worked last while it has a place, which a fresh start does not know
Maker const STICKY = []
{
    auto last = std::make_shared<std::uint32_t>(UINT32_MAX);
    return StepFn(
        [last](Situation const& s)
        {
            Step const step = Next(
                s.quest, Log(s), [&s](std::int32_t slot) { return SlotOpen(s, slot); }, s.mapId, s.x, s.y, s.team,
                [](std::uint16_t) { return true; }, s.tried, s.skip,
                [&s, last](std::vector<Candidate> const& candidates)
                {
                    for (std::size_t i = 0; i < candidates.size(); ++i)
                        if (candidates[i].objective == *last)
                            return static_cast<int>(i);
                    return PlanPick(s, candidates);
                });
            if (step.act == Act::Work)
                *last = step.objective;
            return step;
        });
};

AreaPoint Area(std::uint32_t mapId, float x, float y)
{
    AreaPoint area;
    area.mapId = mapId;
    area.x = x;
    area.y = y;
    area.radius = 20.0f;
    area.points = 5;
    return area;
}

QuestObjective Objective(ObjectiveKind kind, std::int32_t slot, std::uint32_t count, std::vector<AreaPoint> areas)
{
    QuestObjective objective;
    objective.kind = kind;
    objective.target = 100u + static_cast<std::uint32_t>(slot);
    objective.count = count;
    objective.poiIndex = slot;
    objective.areas = std::move(areas);
    if (kind == ObjectiveKind::Item)
        objective.sourceKinds = SourceBit(SourceKind::Corpse);
    return objective;
}

Situation OneQuest(std::vector<QuestObjective> objectives)
{
    Situation s;
    s.quest.id = 7;
    s.quest.objectives = std::move(objectives);
    return s;
}

bool IsWork(Step const& step, Work work, std::uint32_t objective, std::uint32_t area)
{
    return step.act == Act::Work && step.work == work && step.objective == objective && step.area == area;
}

void Dispatch(Maker const& make)
{
    StepFn const step = make();
    Situation kill = OneQuest({Objective(ObjectiveKind::Kill, 0, 10, {Area(MAP_A, 50, 0)})});
    Check(IsWork(step(kill), Work::Kill, 0, 0), "a kill objective is worked by kill");

    Situation loot = OneQuest({Objective(ObjectiveKind::Item, POI_FIRST_ITEM, 4, {Area(MAP_A, 50, 0)})});
    Check(IsWork(step(loot), Work::Loot, 0, 0), "an item objective is worked by loot");

    Situation object = OneQuest({Objective(ObjectiveKind::Kill, 0, 10, {Area(MAP_A, 5, 0)}),
                                 Objective(ObjectiveKind::Object, 1, 1, {Area(MAP_A, 500, 0)})});
    Check(IsWork(step(object), Work::Kill, 0, 0), "an open object objective refuses nothing: the nearer kill first");
    object.have[0] = 10;
    Check(IsWork(step(object), Work::Object, 1, 0), "an object objective is worked by object (WT35e)");
    object.have[1] = 1;
    Check(step(object).act == Act::TurnIn, "the object used and the kills made: the turn-in");

    // An area trigger's objective has no quest-log slot: the log's exploration flag says whether it is open (WT35f)
    Situation explore = OneQuest({Objective(ObjectiveKind::Kill, 0, 10, {Area(MAP_A, 5, 0)}),
                                  Objective(ObjectiveKind::Explore, POI_NONE, 1, {Area(MAP_A, 50, 0)})});
    Check(IsWork(step(explore), Work::Kill, 0, 0), "an unexplored trigger refuses nothing: the nearer kill first");
    explore.have[0] = 10;
    Check(IsWork(step(explore), Work::Explore, 1, 0), "an unexplored trigger is worked by explore");
    explore.explored = true;
    Check(step(explore).act == Act::TurnIn, "explored and killed: the turn-in");

    // A kill objective's creature with a talk credit is talked to by the sides it is friendly to; one whose option
    // the gather could not tell is refused before any walk, as talk_option; a foe's stays a kill (WT35f)
    std::uint8_t const alliance = 1u << ALLIANCE;
    std::uint8_t const horde = 1u << (ALLIANCE + 1);
    Situation talk = OneQuest({Objective(ObjectiveKind::Kill, 0, 1, {Area(MAP_A, 50, 0)})});
    talk.quest.objectives[0].talk = {TalkHow::Hello, 0, 0, alliance};
    Check(IsWork(step(talk), Work::Talk, 0, 0), "a hello credit of a friendly creature is worked by talk");
    talk.quest.objectives[0].talk = {TalkHow::Select, 2871, 0, alliance};
    Check(IsWork(step(talk), Work::Talk, 0, 0), "a select credit too");
    talk.quest.objectives[0].talk.friendlyTo = horde;
    Check(IsWork(step(talk), Work::Kill, 0, 0), "a talk creature hostile to the bot's side stays a kill");
    talk.quest.objectives[0].talk = {TalkHow::Unknown, 0, 0, alliance};
    Step const optionStep = step(talk);
    Check(optionStep.act == Act::Refused && optionStep.work == Work::Talk &&
              std::string(EndWord(optionStep)) == "unsupported:talk_option",
          "a friendly gossip creature without a credit row is refused as unsupported:talk_option");
    talk.have[0] = 1;
    Check(step(talk).act == Act::TurnIn, "a talk met refuses nothing: the turn-in");
    talk.quest.objectives[0].talk.friendlyTo = horde;
    talk.have[0] = 0;
    Check(IsWork(step(talk), Work::Kill, 0, 0), "an unknown credit of a foe's creature stays a kill");

    // An escort or a script's event (the exploration flag without a trigger) is refused before any walk
    Situation escort = OneQuest({Objective(ObjectiveKind::Kill, 0, 10, {Area(MAP_A, 5, 0)})});
    escort.quest.event = QuestEvent::Escort;
    Step const escortStep = step(escort);
    Check(escortStep.act == Act::Refused && std::string(CapabilityOf(escortStep.work).refusal) == "unsupported:escort",
          "an escort is refused as unsupported:escort, though a kill is open");
    escort.quest.event = QuestEvent::Script;
    Check(std::string(CapabilityOf(step(escort).work).refusal) == "unsupported:event", "a script's event by its name");
    escort.explored = true;
    Check(IsWork(step(escort), Work::Kill, 0, 0), "a happened event refuses nothing");
    Situation onlyEscort = OneQuest({});
    onlyEscort.quest.event = QuestEvent::Escort;
    Check(step(onlyEscort).act == Act::Refused, "an escort without another objective is refused, not turned in");

    // An item's work follows its sources: a creature's corpse or skin is loot, an object's is object, none is item
    Situation skin = loot;
    skin.quest.objectives[0].sourceKinds = SourceBit(SourceKind::Skin);
    Check(IsWork(step(skin), Work::Loot, 0, 0), "a skinned item is loot");
    Situation chest = loot;
    chest.quest.objectives[0].sourceKinds = SourceBit(SourceKind::Object);
    Check(IsWork(step(chest), Work::Chest, 0, 0), "an item only objects hold is worked by chest (WT35e)");
    chest.quest.objectives[0].sourceKinds |= SourceBit(SourceKind::Corpse);
    Check(IsWork(step(chest), Work::Loot, 0, 0), "an item creatures drop too is loot");
    Situation vendor = loot;
    vendor.quest.objectives[0].sourceKinds = 0;
    Step const vendorStep = step(vendor);
    Check(vendorStep.act == Act::Refused && std::string(CapabilityOf(vendorStep.work).refusal) == "unsupported:item",
          "an item without a world source is refused as item");

    Check(std::string(CapabilityOf(Work::Escort).name) == "escort" &&
              std::string(CapabilityOf(Work::Escort).refusal) == "unsupported:escort" &&
              !CapabilityOf(Work::Escort).done,
          "escort is a row refused by name");
    Check(CapabilityOf(Work::Kill).done && CapabilityOf(Work::Loot).done, "kill and loot are rows the executor does");
    Check(CapabilityOf(Work::Explore).done && CapabilityOf(Work::Talk).done &&
              std::string(CapabilityOf(Work::Talk).name) == "talk" &&
              std::string(CapabilityOf(Work::Talk).refusal) == "unsupported:talk_option" &&
              std::string(CapabilityOf(Work::Explore).refusal) == "unsupported:explore",
          "explore and talk are rows the executor does (WT35f)");
    Check(CapabilityOf(Work::Object).done && CapabilityOf(Work::Chest).done &&
              std::string(CapabilityOf(Work::Object).refusal) == "unsupported:object" &&
              std::string(CapabilityOf(Work::Chest).refusal) == "unsupported:chest",
          "object and chest are rows the executor does (WT35e)");

    Situation complete = kill;
    complete.have[0] = 10;
    Check(step(complete).act == Act::TurnIn, "every objective met: the turn-in");
    Situation gone = kill;
    gone.inLog = false;
    Check(step(gone).act == Act::Done, "not in the log: done");
}

ItemSource Source(SourceKind kind, std::uint32_t entry, float chance, std::uint32_t spawns = 3)
{
    ItemSource source;
    source.kind = kind;
    source.entry = entry;
    source.chance = chance;
    source.spawns = spawns;
    return source;
}

// The seek of a loot objective: the item's corpse sources a place is built from, the likeliest first
void SeekSources()
{
    std::vector<ItemSource> const mixed = {
        Source(SourceKind::Corpse, 11, 0.3f),    Source(SourceKind::Object, 12, 0.9f),
        Source(SourceKind::Corpse, 13, 0.8f),    Source(SourceKind::Corpse, 14, 0.01f),
        Source(SourceKind::Corpse, 15, 0.9f, 0), Source(SourceKind::Skin, 16, 0.9f),
        Source(SourceKind::Corpse, 13, 0.5f)};
    Check(SeekEntries(mixed) == std::vector<std::uint32_t>{13, 11},
          "the seek takes the corpses above the floor with spawns, the likeliest first, each once");
    Check(SourceKindsOf(mixed) ==
              (SourceBit(SourceKind::Corpse) | SourceBit(SourceKind::Object) | SourceBit(SourceKind::Skin)),
          "the source kinds are those a place is built from");
    std::vector<ItemSource> const skinned = {Source(SourceKind::Skin, 21, 0.9f), Source(SourceKind::Object, 22, 0.9f)};
    Check(SeekEntries(skinned) == std::vector<std::uint32_t>{21}, "without a corpse source the skinned creatures");
    Check(SeekEntries({Source(SourceKind::Object, 31, 0.9f), Source(SourceKind::Corpse, 32, 0.001f)}).empty(),
          "no creature to seek for an item only objects hold");
    std::vector<ItemSource> many;
    for (std::uint32_t i = 0; i < 20; ++i)
        many.push_back(Source(SourceKind::Corpse, 100 + i, 0.05f + 0.01f * static_cast<float>(i)));
    std::vector<std::uint32_t> const capped = SeekEntries(many);
    Check(capped.size() == SEEK_ENTRIES && capped.front() == 119, "at most SEEK_ENTRIES, the likeliest kept");
}

// WT35e: what the executor does with an object it sees, and the locked end. The seek of a chest objective takes its
// item's object sources, the likeliest first
void ObjectVerdicts()
{
    ObjectSeen goober;
    goober.present = true;
    goober.ready = true;
    Check(ObjectVerdict(goober) == ObjectAct::Walk, "an object out of reach is walked to");
    goober.inReach = true;
    Check(ObjectVerdict(goober) == ObjectAct::Use, "an object in reach is used once through its handler");
    ObjectSeen inUse = goober;
    inUse.ready = false;
    Check(ObjectVerdict(inUse) == ObjectAct::Wait, "an object in use is waited for, not passed over");
    ObjectSeen chestInUse = inUse;
    chestInUse.chest = true;
    Check(ObjectVerdict(chestInUse) == ObjectAct::Wait, "a container being opened by another is waited for");
    ObjectSeen gone = goober;
    gone.present = false;
    Check(ObjectVerdict(gone) == ObjectAct::Skip, "an object gone leads to the next");
    ObjectSeen passed = goober;
    passed.passed = true;
    Check(ObjectVerdict(passed) == ObjectAct::Skip, "an object passed over in this run is not taken again");

    ObjectSeen chest = goober;
    chest.chest = true;
    chest.needed = true;
    chest.opens = true;
    Check(ObjectVerdict(chest) == ObjectAct::Loot, "a container the bot can open goes to the loot path");
    ObjectSeen locked = chest;
    locked.opens = false;
    Check(ObjectVerdict(locked) == ObjectAct::Locked, "a container without its key or skill is locked");
    ObjectSeen far = locked;
    far.inReach = false;
    Check(ObjectVerdict(far) == ObjectAct::Walk, "a container's lock is read in reach: walked to first");
    ObjectSeen spent = chest;
    spent.needed = false;
    Check(ObjectVerdict(spent) == ObjectAct::Skip, "a container with nothing the bot needs leads to the next");

    Step const noTarget{Act::NoTarget};
    Check(std::string(EndWord(noTarget, true)) == "locked" && std::string(EndWord(noTarget)) == "no_target",
          "places run out after a locked container: locked, else no_target");
    Step const refused{Act::Refused, Work::Escort};
    Check(std::string(EndWord(refused, true)) == "unsupported:escort", "a refusal keeps its word in a locked run");
    Check(!EndWord(Step{Act::Work}, true) && !EndWord(Step{Act::TurnIn}, true), "a locked run goes on while it works");

    std::vector<ItemSource> const sources = {
        Source(SourceKind::Corpse, 11, 0.9f),    Source(SourceKind::Object, 12, 0.3f),
        Source(SourceKind::Object, 13, 0.8f),    Source(SourceKind::Object, 14, 0.01f),
        Source(SourceKind::Object, 15, 0.9f, 0), Source(SourceKind::Object, 12, 0.2f)};
    Check(SeekObjects(sources) == std::vector<std::uint32_t>{13, 12},
          "the container seek takes the object sources above the floor with spawns, the likeliest first, each once");
    Check(SeekObjects({Source(SourceKind::Corpse, 21, 0.9f)}).empty(), "no container to seek for a corpse's item");
}

void MetNeverWorked(Maker const& make)
{
    StepFn const step = make();
    Situation s = OneQuest({Objective(ObjectiveKind::Kill, 0, 8, {Area(MAP_A, 10, 0)}),
                            Objective(ObjectiveKind::Item, POI_FIRST_ITEM, 3, {Area(MAP_A, 300, 0)})});
    Check(IsWork(step(s), Work::Kill, 0, 0), "the nearest open objective first");
    s.have[0] = 8;
    Check(IsWork(step(s), Work::Loot, 1, 0), "the met kill objective is passed over for the farther item");
    s.have[0] = 7;
    s.have[POI_FIRST_ITEM] = 3;
    Check(IsWork(step(s), Work::Kill, 0, 0), "the met item objective is passed over");
}

void LostTargetNextPlace(Maker const& make)
{
    StepFn const step = make();
    Situation s = OneQuest({Objective(ObjectiveKind::Kill, 0, 8, {Area(MAP_A, 10, 0), Area(MAP_A, 90, 0)}),
                            Objective(ObjectiveKind::Kill, 1, 8, {Area(MAP_A, 60, 0)})});
    Check(IsWork(step(s), Work::Kill, 0, 0), "the nearest place first");
    s.tried.push_back(Key(0, 0));
    Check(IsWork(step(s), Work::Kill, 1, 0), "a place without a target leads to the nearest untried place");
    s.tried.push_back(Key(1, 0));
    Check(IsWork(step(s), Work::Kill, 0, 1), "then the objective's next place");
}

void PlacesRunOut(Maker const& make)
{
    StepFn const step = make();
    Situation s = OneQuest({Objective(ObjectiveKind::Kill, 0, 8, {Area(MAP_A, 10, 0), Area(MAP_A, 90, 0)}),
                            Objective(ObjectiveKind::Item, POI_FIRST_ITEM, 2, {Area(MAP_B, 60, 0)})});
    s.tried = {Key(0, 0), Key(0, 1)};
    Check(step(s).act == Act::NoTarget, "every place tried: no_target");
    s.have[0] = 8;
    Check(step(s).act == Act::NoPlace, "the open objective has no place on this map: no_place");

    Situation side = OneQuest({Objective(ObjectiveKind::Kill, 0, 8, {Area(MAP_A, 10, 0)})});
    side.quest.objectives[0].areas[0].notTeams = 1u << ALLIANCE;
    Check(step(side).act == Act::NoPlace, "an area the bot's side cannot use is no place");
    side.team = HORDE;
    Check(IsWork(step(side), Work::Kill, 0, 0), "the other side uses it");
}

// The start asks the step function once (EndWord): an escort ends before any work, a quest without a place or with
// every place tried ends by that word, a workable quest and a turn-in go on
void RefusalAtStart(Maker const& make)
{
    StepFn const step = make();
    Situation escort = OneQuest({Objective(ObjectiveKind::Kill, 0, 10, {Area(MAP_A, 5, 0)})});
    escort.quest.event = QuestEvent::Escort;
    char const* const refused = EndWord(step(escort));
    Check(refused && std::string(refused) == "unsupported:escort",
          "an escort ends at the order's start as unsupported:escort, before its kill is worked");
    Situation kill = OneQuest({Objective(ObjectiveKind::Kill, 0, 10, {Area(MAP_A, 5, 0)})});
    Check(!EndWord(step(kill)), "a kill quest goes on at the start");
    Situation away = OneQuest({Objective(ObjectiveKind::Kill, 0, 10, {Area(MAP_B, 5, 0)})});
    char const* const noPlace = EndWord(step(away));
    Check(noPlace && std::string(noPlace) == "no_place", "no place on the bot's map ends the start as no_place");
    kill.tried = {Key(0, 0)};
    char const* const noTarget = EndWord(step(kill));
    Check(noTarget && std::string(noTarget) == "no_target", "every place tried ends as no_target");
    kill.have[0] = 10;
    Check(!EndWord(step(kill)), "a met quest's turn-in ends nothing");
}

void SkipAndPlan(Maker const& make)
{
    StepFn const step = make();
    Situation s = OneQuest({Objective(ObjectiveKind::Kill, 0, 8, {Area(MAP_A, 10, 0), Area(MAP_A, 90, 0)})});
    s.skip = {true, 10.0f, 0.0f};
    Check(IsWork(step(s), Work::Kill, 0, 1), "the place a stay left is passed over while the objective has another");
    s.tried.push_back(Key(0, 1));
    Check(IsWork(step(s), Work::Kill, 0, 0), "and taken again when it is the only one left");

    Situation p = OneQuest({Objective(ObjectiveKind::Kill, 0, 8, {Area(MAP_A, 10, 0), Area(MAP_A, 90, 0)})});
    p.plan = true;
    p.planX = 100.0f;
    Check(IsWork(step(p), Work::Kill, 0, 1), "a planner bot takes the place its plan picks");
}

Situation Generate(std::mt19937& rng)
{
    auto const roll = [&rng](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    Situation s;
    s.quest.id = 1;
    int const objectives = 1 + roll(4);
    std::int32_t creatureSlot = 0;
    std::int32_t itemSlot = POI_FIRST_ITEM;
    for (int i = 0; i < objectives; ++i)
    {
        int const kind = roll(12);
        ObjectiveKind const k = kind < 5    ? ObjectiveKind::Kill
                                : kind < 9  ? ObjectiveKind::Item
                                : kind < 11 ? ObjectiveKind::Object
                                            : ObjectiveKind::Explore;
        std::int32_t const slot = k == ObjectiveKind::Explore ? POI_NONE
                                  : k == ObjectiveKind::Item  ? itemSlot++
                                                              : creatureSlot++;
        std::vector<AreaPoint> areas;
        int const count = roll(5);
        for (int a = 0; a < count; ++a)
        {
            AreaPoint area = Area(roll(6) == 0 ? MAP_B : MAP_A, static_cast<float>(roll(800)) - 400.0f,
                                  static_cast<float>(roll(800)) - 400.0f);
            if (roll(8) == 0)
                area.notTeams = static_cast<std::uint8_t>(1u << roll(2));
            areas.push_back(area);
        }
        QuestObjective objective = Objective(k, slot, 1 + static_cast<std::uint32_t>(roll(6)), std::move(areas));
        if (k == ObjectiveKind::Item && roll(8) == 0)
            objective.sourceKinds = static_cast<std::uint8_t>(roll(2) ? SourceBit(SourceKind::Object) : 0);
        s.quest.objectives.push_back(std::move(objective));
        if (slot != POI_NONE)
            s.have[static_cast<std::size_t>(slot)] = static_cast<std::uint32_t>(roll(3));
    }
    if (roll(12) == 0)
        s.quest.event = roll(2) ? QuestEvent::Escort : QuestEvent::Script;
    s.explored = roll(2) == 0;
    s.team = static_cast<std::uint8_t>(roll(2));
    s.x = static_cast<float>(roll(600)) - 300.0f;
    s.y = static_cast<float>(roll(600)) - 300.0f;
    s.plan = roll(3) == 0;
    s.planX = static_cast<float>(roll(800)) - 400.0f;
    s.planY = static_cast<float>(roll(800)) - 400.0f;
    return s;
}

// A run as the executor does it: it takes a step and keeps it while it stands; progress, an empty place and an
// interruption (nothing changes, as a fight) happen at random. After each event a fresh start must agree
void Invariant(Maker const& make)
{
    std::mt19937 rng(20261120u);
    int mismatches = 0;
    int workedMet = 0;
    int unended = 0;
    int refusedLate = 0;
    for (int run = 0; run < RUNS; ++run)
    {
        Situation s = Generate(rng);
        StepFn const runner = make();
        auto const open = [&s](std::int32_t slot) { return SlotOpen(s, slot); };
        Step held = runner(s);
        bool first = true;
        int steps = 0;
        for (; steps < RUN_STEPS && held.act == Act::Work; ++steps)
        {
            if (!open(s.quest.objectives[held.objective].poiIndex))
                ++workedMet;
            first = false;
            switch (rng() % 3)
            {
                case 0:
                    // Progress: a kill, an item or an object counts in its slot; an area trigger sets the log's flag
                    if (s.quest.objectives[held.objective].poiIndex == POI_NONE)
                        s.explored = true;
                    else
                        ++s.have[static_cast<std::size_t>(s.quest.objectives[held.objective].poiIndex)];
                    break;
                case 1:
                    s.tried.push_back(KeyOf(held));
                    break;
                default:
                    break;
            }
            if (!Stands(s.quest, held, open, s.tried))
                held = runner(s);
            if (!(make()(s) == held))
                ++mismatches;
        }
        if (held.act == Act::Work)
            ++unended;
        if (held.act == Act::Refused && !first)
            ++refusedLate;
    }
    Check(mismatches == 0, "resume invariant: a fresh start returns the run's next step (" +
                               std::to_string(mismatches) + " mismatches)");
    Check(workedMet == 0, "no run works a met objective (" + std::to_string(workedMet) + ")");
    Check(unended == 0, "every run ends: turn-in, no_target, no_place or refused (" + std::to_string(unended) + ")");
    Check(refusedLate == 0, "a refusal comes only at the order's start (" + std::to_string(refusedLate) + ")");
}

// The executor's clocks and its passed set, with rules a mutant may replace
using PursueFn = std::function<Pause(std::uint32_t&, bool&, bool, std::uint32_t)>;
using StalledFn = std::function<bool(float&, std::uint32_t&, bool, std::uint32_t, float)>;
using Ring = std::array<std::uint64_t, PASSED_TARGETS>;
using RememberFn = std::function<void(Ring&, std::uint8_t&, std::uint64_t)>;

struct ClockRules
{
    PursueFn pursue = [](std::uint32_t& engaged, bool& ticking, bool combat, std::uint32_t since)
    { return Pursue(engaged, ticking, combat, since); };
    StalledFn stalled = [](float& nearest, std::uint32_t& stalledMs, bool first, std::uint32_t since, float yards)
    { return WalkStalled(nearest, stalledMs, first, since, yards); };
    RememberFn remember = [](Ring& ring, std::uint8_t& next, std::uint64_t id) { Remember(ring, next, id); };
    std::function<Step(QuestRecord const&, std::uint8_t, std::vector<PlaceKey> const&)> ender =
        [](QuestRecord const& quest, std::uint8_t team, std::vector<PlaceKey> const& tried)
    { return NextEnder(quest, MAP_A, 0.0f, 0.0f, team, tried); };
    std::function<bool(QuestRecord const&, std::uint8_t, std::vector<PlaceKey> const&, PlaceKey)> stallTries =
        [](QuestRecord const& quest, std::uint8_t team, std::vector<PlaceKey> const& tried, PlaceKey key)
    { return StallTries(quest, MAP_A, 0.0f, 0.0f, team, tried, key); };
};

// WT35h: the turn-in's enders. Nearest usable first, an unusable or tried one passed over, every one tried no_ender
// (none on the map no_place), a fresh start over the same tried set the same ender; a walk that gains nothing tries
// its ender while another is left, the last one's walk starts again and the order's give-up clock ends it no_ender
// (mutants: no walk watch, the tried enders forgotten, the last ender tried on a stall)
void TurnIns(ClockRules const& rules)
{
    QuestRecord quest;
    quest.objectives.resize(2);
    quest.objectives[1].kind = ObjectiveKind::Turnin;
    quest.objectives[1].areas = {
        {MAP_A, 300.0f, 0.0f}, {MAP_A, 100.0f, 0.0f}, {MAP_B, 10.0f, 0.0f}, {MAP_A, 200.0f, 0.0f}};
    quest.objectives[1].areas[1].notTeams = 1u << HORDE;
    Check(rules.ender(quest, ALLIANCE, {}) == Step{Act::TurnIn, Work::Count, 1, 1}, "the nearest ender first");
    Check(rules.ender(quest, HORDE, {}).area == 3, "an ender unusable for the side is passed over");
    Check(rules.ender(quest, ALLIANCE, {Key(1, 1)}).area == 3, "a tried ender is passed over");
    Check(rules.ender(quest, ALLIANCE, {Key(1, 1)}) == rules.ender(quest, ALLIANCE, {Key(1, 1)}),
          "a fresh start over the same tried set takes the same ender");
    Step const none = rules.ender(quest, ALLIANCE, {Key(1, 0), Key(1, 1), Key(1, 3)});
    Check(none.act == Act::NoEnder && std::string(EndWord(none)) == "no_ender", "every ender tried: no_ender");
    Check(std::string(EndWord(NextEnder(quest, 9, 0.0f, 0.0f, ALLIANCE, {}))) == "no_place",
          "none on the map: no_place");

    Check(rules.stallTries(quest, ALLIANCE, {}, Key(1, 1)), "a stall tries the ender while others are left");
    Check(!rules.stallTries(quest, ALLIANCE, {Key(1, 1), Key(1, 3)}, Key(1, 0)), "a stall keeps the last ender");
    Check(!rules.stallTries(quest, HORDE, {Key(1, 3)}, Key(1, 0)),
          "an ender unusable for the side does not count as left");

    // Every walk gains nothing, 1 s ticks: the first two enders are tried at their stalls, the last one's walk starts
    // again until the order's give-up clock (10 minutes without progress) ends the turn-in no_ender
    std::vector<PlaceKey> tried;
    Pursuit::Clock clock;
    std::uint32_t const limit = Pursuit::LimitMs(10, false);
    char const* cause = nullptr;
    int ticks = 0;
    int restarts = 0;
    Step step = rules.ender(quest, ALLIANCE, tried);
    float nearest = -1.0f;
    std::uint32_t stalledMs = 0;
    bool first = true;
    for (; step.act == Act::TurnIn && !cause && ticks < 10000; ++ticks)
    {
        Pursuit::Tick(clock, true, 1, ticks ? 1000 : 0, 0);
        cause = Pursuit::Decide(clock, limit, nullptr, Pursuit::NO_ENDER);
        if (cause || !rules.stalled(nearest, stalledMs, first, first ? 0 : 1000, 90.0f))
        {
            first = false;
            continue;
        }
        if (rules.stallTries(quest, ALLIANCE, tried, KeyOf(step)))
        {
            tried.push_back(KeyOf(step));
            step = rules.ender(quest, ALLIANCE, tried);
        }
        else
            ++restarts;
        nearest = -1.0f;
        stalledMs = 0;
        first = true;
    }
    Check(cause && std::string(cause) == "no_ender" && step.act == Act::TurnIn && KeyOf(step) == Key(1, 0) &&
              tried == std::vector<PlaceKey>{Key(1, 1), Key(1, 3)} && restarts >= 10 &&
              ticks == static_cast<int>(limit / 1000) + 1,
          "stalled turn-in walks try the enders nearest first, restart the last and end no_ender on the order clock (" +
              std::to_string(ticks) + " ticks, " + std::to_string(restarts) + " restarts)");
    // At the last ender a look without the hand-in still tries it: nothing is left
    tried.push_back(KeyOf(step));
    Check(rules.ender(quest, ALLIANCE, tried).act == Act::NoEnder, "the last ender looked at: no_ender");
}

void Clocks(ClockRules const& rules)
{
    TurnIns(rules);
    // Steady ticks of 1 s: the pursuit runs out at PURSUIT_MS of engage time
    std::uint32_t engaged = 0;
    bool ticking = false;
    int ticks = 0;
    while (!PursuitOver(engaged) && ticks < 1000)
    {
        rules.pursue(engaged, ticking, false, ticks ? 1000 : 0);
        ++ticks;
    }
    Check(ticks == 46, "1 s engage ticks pass a target over after 45 s (" + std::to_string(ticks) + " ticks)");

    // A fight holds it: no time counts in the fight, nor across it, and the pause is seen once
    engaged = 0;
    ticking = false;
    for (int i = 0; i < 21; ++i)
        rules.pursue(engaged, ticking, false, i ? 1000 : 0);
    std::uint32_t const before = engaged;
    int combats = 0;
    for (int i = 0; i < 30; ++i)
        if (rules.pursue(engaged, ticking, true, 1000) == Pause::Combat)
            ++combats;
    rules.pursue(engaged, ticking, false, 30000);
    Check(before == 20000 && engaged == before && combats == 1,
          "a fight holds the pursuit, never restarts it (" + std::to_string(engaged) + " ms)");

    // A gap holds it too: a pause counts as CLOCK_GAP_MS
    Pause const gap = rules.pursue(engaged, ticking, false, 60000);
    Check(gap == Pause::Gap && engaged == before + CLOCK_GAP_MS,
          "a 60 s gap counts 5 s and is seen (" + std::to_string(engaged) + " ms)");

    // Sparse ticks still run the pursuit out
    engaged = 0;
    ticking = false;
    ticks = 0;
    while (!PursuitOver(engaged) && ticks < 1000)
    {
        rules.pursue(engaged, ticking, false, ticks ? 20000 : 0);
        ++ticks;
    }
    Check(ticks <= 10, "ticks 20 s apart pass a target over within 10 ticks (" + std::to_string(ticks) + ")");

    // A walk that gains goes on; one that gains nothing stops at WALK_STALL_MS, before travel's 90 s give-up
    float nearest = -1.0f;
    std::uint32_t stalledMs = 0;
    bool stall = false;
    for (int i = 0; i < 100 && !stall; ++i)
        stall = rules.stalled(nearest, stalledMs, i == 0, i ? 1000 : 0, 800.0f - 7.0f * static_cast<float>(i));
    Check(!stall, "a walk that gains 7 yd/s is never given up");
    nearest = -1.0f;
    stalledMs = 0;
    ticks = 0;
    for (stall = false; !stall && ticks < 1000; ++ticks)
        stall = rules.stalled(nearest, stalledMs, ticks == 0, ticks ? 1000 : 0, 120.0f);
    Check(ticks == 31, "a walk without gain is given up after 30 s (" + std::to_string(ticks) + " ticks), not 90 s");

    // After a pause the walk is measured from where the bot stands, its time held
    nearest = -1.0f;
    stalledMs = 0;
    for (int i = 0; i < 21; ++i)
        rules.stalled(nearest, stalledMs, i == 0, i ? 1000 : 0, 120.0f);
    rules.stalled(nearest, stalledMs, false, 40000, 140.0f);
    Check(stalledMs == 20000 + CLOCK_GAP_MS && nearest == 140.0f,
          "a pause moves the walk's base, holds its time (" + std::to_string(stalledMs) + " ms)");
    bool const gained = !rules.stalled(nearest, stalledMs, false, 1000, 130.0f) && stalledMs == 0;
    Check(gained, "a gain after the pause restarts the watch");

    // Sparse ticks without gain still stop the walk
    nearest = -1.0f;
    stalledMs = 0;
    ticks = 0;
    for (stall = false; !stall && ticks < 1000; ++ticks)
        stall = rules.stalled(nearest, stalledMs, ticks == 0, ticks ? 20000 : 0, 120.0f);
    Check(ticks <= 8, "walk ticks 20 s apart are given up within 8 ticks (" + std::to_string(ticks) + ")");

    // The passed set keeps the last PASSED_TARGETS and never an empty id
    Ring ring{};
    std::uint8_t next = 0;
    for (std::uint64_t id = 1; id <= 5; ++id)
        rules.remember(ring, next, id);
    Check(
        !Remembered(ring, std::uint64_t{1}) && Remembered(ring, std::uint64_t{2}) && Remembered(ring, std::uint64_t{5}),
        "the passed set holds the last four targets");
    Check(!Remembered(ring, std::uint64_t{0}), "an empty id is never passed over");
}

// WT35f: the talk engagement's and the trigger's decisions
void TalkVerdicts()
{
    TalkSeen npc;
    npc.present = true;
    npc.usable = true;
    Check(TalkVerdict(npc) == TalkAct::Walk, "a talk creature out of reach is walked to");
    npc.inReach = true;
    Check(TalkVerdict(npc) == TalkAct::Hello, "in reach and in sight: the hello through its handler");
    TalkSeen busy = npc;
    busy.usable = false;
    Check(TalkVerdict(busy) == TalkAct::NoInteract, "a creature the bot cannot talk to now is not held");
    TalkSeen passed = npc;
    passed.passed = true;
    Check(TalkVerdict(passed) == TalkAct::Skip, "a creature passed over in this run is not taken again");
    TalkSeen gone = npc;
    gone.present = false;
    Check(TalkVerdict(gone) == TalkAct::Skip, "a creature gone leads to the next");

    TalkSeen hello = npc;
    hello.helloSent = true;
    Check(TalkVerdict(hello) == TalkAct::Hold, "a hello credit: the hello alone, then the hold");
    hello.holdOver = true;
    Check(TalkVerdict(hello) == TalkAct::Done, "the hold over: written off, the log decides");
    TalkSeen select = npc;
    select.select = true;
    Check(TalkVerdict(select) == TalkAct::Hello, "a select credit opens the gossip first");
    select.helloSent = true;
    select.optionShown = true;
    Check(TalkVerdict(select) == TalkAct::Select, "then picks the option the open menu shows");
    TalkSeen hidden = select;
    hidden.optionShown = false;
    Check(TalkVerdict(hidden) == TalkAct::NoOption, "an option the menu hides: passed over");
    select.selected = true;
    Check(TalkVerdict(select) == TalkAct::Hold, "picked: the hold");
    select.holdOver = true;
    Check(TalkVerdict(select) == TalkAct::Done, "the hold over after the select: written off");

    Check(ExploreVerdict(false, false, false) == ExploreAct::Walk, "outside the trigger: the walk to its centre");
    Check(ExploreVerdict(true, false, false) == ExploreAct::Send, "inside: the message once through its handler");
    Check(ExploreVerdict(true, true, false) == ExploreAct::Hold, "sent: the hold");
    Check(ExploreVerdict(true, true, true) == ExploreAct::Tried, "the hold over, the flag unset: the place is tried");
}

void All(Maker const& make)
{
    Dispatch(make);
    RefusalAtStart(make);
    SeekSources();
    ObjectVerdicts();
    TalkVerdicts();
    MetNeverWorked(make);
    LostTargetNextPlace(make);
    PlacesRunOut(make);
    SkipAndPlan(make);
    Invariant(make);
}

void Mutant(char const* name, Maker const& make)
{
    Tally local;
    local.print = false;
    tally = &local;
    All(make);
    tally = &total;
    Check(local.failed > 0, std::string("mutant ") + name + " is caught");
}

void ClockMutant(char const* name, ClockRules const& rules)
{
    Tally local;
    local.print = false;
    tally = &local;
    Clocks(rules);
    tally = &total;
    Check(local.failed > 0, std::string("mutant ") + name + " is caught");
}
}  // namespace

// WT35g: the chain "use, then kill" is a kill objective's whose use brings its creature forth; the step function
// works it as the use (the kill step is the action's, by the held creature), and a missing link ends as the use does
void UseChains()
{
    QuestObjective summon = Objective(ObjectiveKind::Kill, 0, 1, {Area(MAP_A, 10, 0)});
    summon.use = UseHow::Item;
    summon.useKind = UseKind::Focus;
    summon.brings = UseBrings::Summon;
    Check(KillsAfterUse(summon), "a summoning use is followed by the kill");
    QuestObjective transform = summon;
    transform.useKind = UseKind::UnitAlive;
    transform.brings = UseBrings::Transform;
    Check(KillsAfterUse(transform), "a transforming use is followed by the kill");
    QuestObjective plain = summon;
    plain.brings = UseBrings::Credit;
    Check(!KillsAfterUse(plain), "a use that credits has no kill step");
    QuestObjective cast = transform;
    cast.use = UseHow::Spell;
    Check(KillsAfterUse(cast), "a class spell that transforms is followed by the kill");
    Situation s = OneQuest({summon});
    Check(IsWork(REAL()(s), Work::Use, 0, 0), "the chain's step is the use at the anchor");
    s.tried = {Key(0, 0)};
    Check(REAL()(s).act == Act::NoTarget, "the anchor tried without the creature: no_target");
    Situation nowhere = OneQuest({summon});
    nowhere.quest.objectives[0].areas.clear();
    Check(REAL()(nowhere).act == Act::NoPlace, "a summon without an anchor's place: no_place");
}

int main()
{
    UseChains();
    All(REAL);
    Mutant("works a met objective", WORKS_MET);
    Mutant("forgets the tried places", FORGETS_TRIED);
    Mutant("grinds an unsupported objective", GRINDS_UNSUPPORTED);
    Mutant("calls no_target no_place", NO_PLACE_FOR_NO_TARGET);
    Mutant("keeps hidden state", STICKY);
    Clocks(ClockRules{});
    ClockRules restartsAfterFight;
    restartsAfterFight.pursue = [](std::uint32_t& engaged, bool& ticking, bool combat, std::uint32_t since)
    {
        if (combat)
            engaged = 0;
        return Pursue(engaged, ticking, combat, since);
    };
    ClockMutant("restarts the pursuit after a fight", restartsAfterFight);
    ClockRules restartsAfterGap;
    restartsAfterGap.pursue = [](std::uint32_t& engaged, bool& ticking, bool combat, std::uint32_t since)
    {
        if (since > CLOCK_GAP_MS)
            engaged = 0;
        return Pursue(engaged, ticking, combat, since);
    };
    ClockMutant("restarts the pursuit after a gap", restartsAfterGap);
    ClockRules walkRestarts;
    walkRestarts.stalled = [](float& nearest, std::uint32_t& stalledMs, bool first, std::uint32_t since, float yards)
    {
        if (since > CLOCK_GAP_MS)
            stalledMs = 0;
        return WalkStalled(nearest, stalledMs, first, since, yards);
    };
    ClockMutant("restarts the walk after a pause", walkRestarts);
    ClockRules holdsOne;
    holdsOne.remember = [](Ring& ring, std::uint8_t&, std::uint64_t id) { ring[0] = id; };
    ClockMutant("passes over only one target", holdsOne);
    ClockRules noWatch;
    noWatch.stalled = [](float&, std::uint32_t&, bool, std::uint32_t, float) { return false; };
    ClockMutant("walks to an ender without a watch (the q791 loop)", noWatch);
    ClockRules forgetsEnders;
    forgetsEnders.ender = [](QuestRecord const& quest, std::uint8_t team, std::vector<PlaceKey> const&)
    { return NextEnder(quest, MAP_A, 0.0f, 0.0f, team, {}); };
    ClockMutant("forgets the tried enders", forgetsEnders);
    ClockRules triesLast;
    triesLast.stallTries = [](QuestRecord const&, std::uint8_t, std::vector<PlaceKey> const&, PlaceKey)
    { return true; };
    ClockMutant("tries the last ender on a stall (WT35h's q791, q86)", triesLast);
    std::printf("%d passed, %d failed\n", total.passed, total.failed);
    return total.failed ? 1 : 0;
}
