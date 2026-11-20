/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the quest executor's step function (questing WT35a, QuestExecRules.h): dispatch by kind through
// the capability table (kill, loot; object, explore and escort refused by name), a met objective never worked, a
// place without a target leading to the objective's next place, places running out as no_target (none on the map:
// no_place), the place a stay left passed over, a planner's pick; and the resume invariant, searched over generated
// quests and logs: at every point of a run (progress, an empty place, an interruption) a fresh start over the same log
// and the same tried places returns the step the run itself takes next. Mutants that break each rule must fail.
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Ai/World/Rpg /I src/Bot
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
    QuestPlaces::Skip skip;
    bool plan{false};  // a planner bot: it prefers the candidate nearest its plan's point
    float planX{0.0f};
    float planY{0.0f};
};

bool SlotOpen(Situation const& s, std::int32_t slot)
{
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
    return false;
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
                for (QuestObjective const& objective : s.quest.objectives)
                    if (objective.poiIndex == slot && WorkOf(objective.kind) != Work::Count &&
                        !CapabilityOf(WorkOf(objective.kind)).done)
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
    Step const refused = step(object);
    Check(refused.act == Act::Refused && refused.work == Work::Object && refused.objective == 1,
          "an open object objective refuses the quest at the start, though a kill is nearer");
    Check(std::string(CapabilityOf(refused.work).refusal) == "unsupported:object", "the refusal word names the kind");

    object.have[1] = 1;
    Check(IsWork(step(object), Work::Kill, 0, 0), "a met objective without a row refuses nothing");

    Situation explore = OneQuest({Objective(ObjectiveKind::Explore, 2, 1, {Area(MAP_A, 5, 0)})});
    Step const exploreStep = step(explore);
    Check(exploreStep.act == Act::Refused && exploreStep.work == Work::Explore, "an open explore objective is refused");

    Check(std::string(CapabilityOf(Work::Escort).name) == "escort" &&
              std::string(CapabilityOf(Work::Escort).refusal) == "unsupported:escort" &&
              !CapabilityOf(Work::Escort).done,
          "escort is a row refused by name");
    Check(CapabilityOf(Work::Kill).done && CapabilityOf(Work::Loot).done, "kill and loot are rows the executor does");

    Situation complete = kill;
    complete.have[0] = 10;
    Check(step(complete).act == Act::TurnIn, "every objective met: the turn-in");
    Situation gone = kill;
    gone.inLog = false;
    Check(step(gone).act == Act::Done, "not in the log: done");
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
        int const kind = roll(10);
        ObjectiveKind const k = kind < 5 ? ObjectiveKind::Kill : kind < 9 ? ObjectiveKind::Item : ObjectiveKind::Object;
        std::int32_t const slot = k == ObjectiveKind::Item ? itemSlot++ : creatureSlot++;
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
        s.quest.objectives.push_back(Objective(k, slot, 1 + static_cast<std::uint32_t>(roll(6)), std::move(areas)));
        s.have[static_cast<std::size_t>(slot)] = static_cast<std::uint32_t>(roll(3));
    }
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

void All(Maker const& make)
{
    Dispatch(make);
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
}  // namespace

int main()
{
    All(REAL);
    Mutant("works a met objective", WORKS_MET);
    Mutant("forgets the tried places", FORGETS_TRIED);
    Mutant("grinds an unsupported objective", GRINDS_UNSUPPORTED);
    Mutant("calls no_target no_place", NO_PLACE_FOR_NO_TARGET);
    Mutant("keeps hidden state", STICKY);
    std::printf("%d passed, %d failed\n", total.passed, total.failed);
    return total.failed ? 1 : 0;
}
