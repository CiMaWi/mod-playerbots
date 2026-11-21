/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the quest executor's item use (questing WT35b, QuestUseExecRules.h and the use rows of
// QuestExecRules.h): a use rule wins over the objective's kind (an item on a unit, a corpse, an object or at a focus:
// use; a class spell: cast; the object itself used stays the kind's, WT35g), the index places a rule's objective at the
// spawns of the rule's targets with their height (creatures, or gameobjects for the object and focus kinds; an entry
// without a spawn leaves the objective's places alone), a rule that completes the quest lands on the objective no spawn
// places or on the quest's event, which the step function then works; the verdict at a target (wrong life state skips,
// out of reach walks, a pending use waits), the rule pick (own objective first, held item, known rank, no_item and
// no_spell), the cause table (every framework cause classified, end words, bounded retries), one quest_use row per
// request at the request (used, pending, refused:<cause>), the resume invariant, the hold of the AI through a pending
// quest channel (part 2: the rod's channel cut by the bot's own fight) and the place restart rule of a stalled walk
// (WQL2b note 2). Mutants that break each rule must fail. Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4
// /WX /I src/Ai/World/Rpg /I src/Bot /I src/Ai/Item tests/quest-use-exec-selftest.cpp
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "QuestExecRules.h"
#include "QuestUseExecRules.h"

using namespace Planner;
using namespace Planner::QuestExec;

namespace
{
constexpr std::uint32_t MAP = 530;
constexpr std::uint32_t ZONE = 12;
constexpr std::uint8_t ALLIANCE = 0;

int passed = 0;
int failed = 0;
bool quiet = false;

bool Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        if (!quiet)
            std::printf("FAIL: %s\n", what.c_str());
    }
    return ok;
}

QuestRecord Quest(std::uint32_t id)
{
    QuestRecord quest;
    quest.id = id;
    quest.level = 10;
    quest.minLevel = 8;
    quest.zoneOrSort = static_cast<std::int32_t>(ZONE);
    quest.xpBase = 100 * id;
    return quest;
}

QuestObjective Objective(ObjectiveKind kind, std::uint32_t target, std::uint32_t count, std::int32_t poiIndex)
{
    return {kind, target, count, poiIndex, {}};
}

UseSpell Spell(std::uint32_t id, std::uint8_t targets, std::vector<UseEntry> named = {})
{
    UseSpell spell;
    spell.spellId = id;
    spell.targets = targets;
    spell.rangeYards = 30.0f;
    spell.named = std::move(named);
    return spell;
}

// The census shapes of part 1 as one synthetic gather
constexpr std::uint32_t Q_DEAD = 9600;      // Symbol of Life on the dead furbolg shaman: the kill objective's target
constexpr std::uint32_t Q_CREDIT = 9303;    // the crystal on owlkin: credit 16534, which no creature spawns as
constexpr std::uint32_t Q_EVENT = 6082;     // the taming rod: a use completes the quest, by its event flag
constexpr std::uint32_t Q_OBJECT = 90010;   // an item on a named object: places at the object, not a creature twin
constexpr std::uint32_t Q_NOSPAWN = 90011;  // the rule's creature has no spawn: the objective keeps its places
constexpr std::uint32_t SHAMAN = 17542;
constexpr std::uint32_t OWLKIN = 16518;
constexpr std::uint32_t OWLKIN_CREDIT = 16534;
constexpr std::uint32_t SCORPID = 3126;
constexpr std::uint32_t PILLAR = 181433;
constexpr std::uint32_t GHOST = 90500;
constexpr std::uint32_t Q_SUMMON = 9174;  // WT35g: the item at the altar summons the objective's creature
constexpr std::uint32_t AQUANTION = 16292;
constexpr std::uint32_t ALTAR = 181157;

QuestGather Gather()
{
    QuestGather in;
    QuestRecord dead = Quest(Q_DEAD);
    dead.objectives.push_back(Objective(ObjectiveKind::Kill, SHAMAN, 1, 0));
    in.quests.push_back(dead);
    in.uses.spells.push_back(Spell(8593, UseTarget::UNIT | UseTarget::DEAD, {{SHAMAN, false, true}}));
    in.uses.items.push_back({Q_DEAD, 6866, 8593, {}});
    in.uses.hits.push_back({SHAMAN, 8593});
    in.targets.push_back({SHAMAN, false, MAP, -2445.0f, -12205.0f, 33.5f});

    QuestRecord credit = Quest(Q_CREDIT);
    credit.objectives.push_back(Objective(ObjectiveKind::Kill, OWLKIN_CREDIT, 6, 0));
    in.quests.push_back(credit);
    in.uses.spells.push_back(Spell(29528, UseTarget::UNIT, {{OWLKIN, false, false}}));
    in.uses.items.push_back({Q_CREDIT, 22962, 29528, {}});
    for (int i = 0; i < 4; ++i)
        in.targets.push_back({OWLKIN, false, MAP, -4508.0f + 6.0f * static_cast<float>(i), -13901.0f, 84.25f});

    QuestRecord event = Quest(Q_EVENT);
    event.event = QuestEvent::Script;
    event.objectives.push_back(Objective(ObjectiveKind::Item, 15920, 1, 0));
    in.quests.push_back(event);
    in.uses.spells.push_back(Spell(19697, UseTarget::UNIT, {{SCORPID, false, false}}));
    in.uses.items.push_back({Q_EVENT, 15920, 19697, {}});
    in.targets.push_back({SCORPID, false, 1, 400.0f, -4700.0f, 21.0f});

    QuestRecord object = Quest(Q_OBJECT);
    object.objectives.push_back(Objective(ObjectiveKind::Item, 5000, 1, 0));
    in.quests.push_back(object);
    in.uses.spells.push_back(Spell(29529, UseTarget::OBJECT, {{PILLAR, true, false}}));
    in.uses.items.push_back({Q_OBJECT, 22955, 29529, {}});
    in.targets.push_back({PILLAR, true, MAP, -4395.0f, -13644.0f, 33.0f});
    in.targets.push_back({PILLAR, false, MAP, 1000.0f, 1000.0f, 5.0f});  // a creature with the object's number

    QuestRecord nospawn = Quest(Q_NOSPAWN);
    nospawn.objectives.push_back(Objective(ObjectiveKind::Kill, GHOST + 1, 1, 0));
    in.quests.push_back(nospawn);
    in.uses.spells.push_back(Spell(29530, UseTarget::UNIT, {{GHOST, false, false}}));
    in.uses.items.push_back({Q_NOSPAWN, 22956, 29530, {}});

    QuestRecord summon = Quest(Q_SUMMON);
    summon.objectives.push_back(Objective(ObjectiveKind::Kill, AQUANTION, 1, 0));
    in.quests.push_back(summon);
    UseSpell altar = Spell(28226, UseTarget::SELF);
    altar.focusId = 50;
    altar.focusObjects = {ALTAR};
    altar.focusYards = 10.0f;
    altar.summons = {AQUANTION};
    in.uses.spells.push_back(altar);
    in.uses.items.push_back({Q_SUMMON, 22675, 28226, {}});
    in.targets.push_back({ALTAR, true, MAP, 7938.0f, -7637.0f, 112.0f});
    return in;
}

QuestPlaces::Skip const NO_SKIP{};

template <typename Open>
Step StepOf(QuestRecord const& quest, Open open, std::vector<PlaceKey> const& tried, float x = 0.0f, float y = 0.0f,
            std::uint32_t mapId = MAP)
{
    return Next(
        quest, LogState{true, false}, open, mapId, x, y, ALLIANCE, [](std::uint16_t) { return true; }, tried, NO_SKIP,
        [](std::vector<Candidate> const&) { return -1; });
}

using WorkFn = std::function<Work(QuestObjective const&)>;
using VerdictFn = std::function<UseAct(UseSeen const&)>;
using RefusalFn = std::function<Refusal(ItemUse::Cause, std::uint8_t)>;
using RowFn = std::function<std::string(ItemUse::Outcome, ItemUse::Cause)>;
using HoldFn = std::function<bool(bool, bool, bool)>;
using PassFn = std::function<bool(UseBrings, std::uint32_t, std::uint32_t)>;
using ChainFn = std::function<bool(QuestObjective const&)>;
using StallFn = std::function<bool(QuestRecord const&, std::vector<PlaceKey> const&, PlaceKey)>;

struct Impl
{
    WorkFn work;
    VerdictFn verdict;
    RefusalFn after;
    RowFn row;
    HoldFn hold;
    StallFn stall;
    PassFn passes;
    ChainFn chains;
};

bool RealStall(QuestRecord const& quest, std::vector<PlaceKey> const& tried, PlaceKey key)
{
    return PlaceStallTries(quest, MAP, ALLIANCE, [](std::uint16_t) { return true; }, tried, key);
}

Impl const REAL{[](QuestObjective const& objective) { return WorkOf(objective); },
                UseVerdict,
                AfterRefusal,
                UseRowWord,
                HoldsUseChannel,
                RealStall,
                PassesUsed,
                KillsAfterUse};

QuestObjective WithUse(ObjectiveKind kind, UseHow how, UseKind useKind)
{
    QuestObjective objective = Objective(kind, 1, 1, 0);
    objective.use = how;
    objective.useKind = useKind;
    return objective;
}

// WT35g: the chain "use, then kill": the index says which kill objective's creature the use brings forth; the
// executor kills it when it is there (KillsAfterUse), and does not pass over a target the use turned into it
void Chains(Impl const& impl)
{
    QuestIndex const idx = QuestIndex::Build(Gather());
    QuestRecord const* summon = idx.Find(Q_SUMMON);
    QuestRecord const* credit = idx.Find(Q_CREDIT);
    QuestRecord const* dead = idx.Find(Q_DEAD);
    if (!Check(summon && credit && dead, "the chain quests are indexed"))
        return;
    QuestObjective const& aquantion = summon->objectives[0];
    Check(aquantion.brings == UseBrings::Summon && aquantion.use == UseHow::Item &&
              aquantion.placeFrom == PlaceFrom::UseRule && impl.chains(aquantion),
          "q9174: the altar's use brings the creature forth, placed at the altar; the kill follows the use");
    Step const first = StepOf(*summon, [](std::int32_t) { return true; }, {});
    Check(first.act == Act::Work && first.work == Work::Use && first.objective == 0 && first.area == 0,
          "with no creature there the step is the use at the altar");
    Check(StepOf(*summon, [](std::int32_t) { return true; }, {Key(0, 0)}).act == Act::NoTarget,
          "a use that brought nothing forth at its only place ends no_target");
    Check(credit->objectives[0].brings == UseBrings::Transform && impl.chains(credit->objectives[0]),
          "q9303: the owlkin used on turns into the unspawned credit (left by the kill step while not hostile)");
    Check(dead->objectives[0].brings == UseBrings::Credit && !impl.chains(dead->objectives[0]),
          "a use on the objective's own creature has no kill step");
    QuestObjective item = aquantion;
    item.kind = ObjectiveKind::Item;
    Check(!impl.chains(item), "an item objective has no kill step");
    QuestObjective object = aquantion;
    object.use = UseHow::Object;
    Check(!impl.chains(object), "the object itself used has no kill step");
    Check(impl.passes(UseBrings::Credit, 1200, 1200) && impl.passes(UseBrings::Transform, 1200, 0),
          "a used target that stayed or went is passed over");
    Check(!impl.passes(UseBrings::Transform, 1200, 24782),
          "q55: a target the use turned into the objective's creature is not passed over");
    Check(!impl.passes(UseBrings::Summon, ALTAR, ALTAR) && !impl.passes(UseBrings::Summon, ALTAR, 0),
          "q9174: the summon's anchor serves the next use (three bots, one altar)");
}

// (a) the rule wins over the kind, as far as part 1 goes
void WorkRows(Impl const& impl)
{
    Check(impl.work(WithUse(ObjectiveKind::Kill, UseHow::Item, UseKind::UnitAlive)) == Work::Use,
          "an item on a living unit is a use");
    Check(impl.work(WithUse(ObjectiveKind::Kill, UseHow::Item, UseKind::UnitDead)) == Work::Use,
          "an item on a dead unit is a use, not a kill");
    Check(impl.work(WithUse(ObjectiveKind::Item, UseHow::Item, UseKind::UnitAlive)) == Work::Use,
          "an item objective done by a use is a use, not unsupported:item");
    Check(impl.work(WithUse(ObjectiveKind::Kill, UseHow::Spell, UseKind::UnitAlive)) == Work::Cast,
          "a class spell rule is a cast");
    Check(impl.work(WithUse(ObjectiveKind::Item, UseHow::Item, UseKind::Object)) == Work::Use,
          "an item on an object is a use, not unsupported:item");
    Check(impl.work(WithUse(ObjectiveKind::Kill, UseHow::Item, UseKind::Object)) == Work::Use,
          "a key on a cage is a use, not a kill of the beast inside");
    Check(impl.work(WithUse(ObjectiveKind::Kill, UseHow::Item, UseKind::Focus)) == Work::Use,
          "an item at a focus is a use");
}

void Capabilities()
{
    Check(CapabilityOf(Work::Use).done && std::string(CapabilityOf(Work::Use).refusal) == "unsupported:use",
          "the use row is done");
    Check(CapabilityOf(Work::Cast).done && std::string(CapabilityOf(Work::Cast).refusal) == "unsupported:cast",
          "the cast row is done");
    QuestObjective const cast = WithUse(ObjectiveKind::Kill, UseHow::Spell, UseKind::UnitAlive);
    Check(!Refuses(cast, WorkOf(cast)), "a cast objective is not refused");
    Check(WorkOf(WithUse(ObjectiveKind::Item, UseHow::Object, UseKind::Object)) == Work::Item,
          "the object itself used keeps the kind's row (WT35g)");
    Check(UsesObject(UseKind::Object) && UsesObject(UseKind::Focus) && !UsesObject(UseKind::UnitAlive) &&
              !UsesObject(UseKind::UnitDead),
          "object and focus rules seek gameobjects, unit rules creatures");
    QuestObjective const plain = Objective(ObjectiveKind::Item, 1, 1, 0);
    Check(WorkOf(plain) == Work::Item && std::string(CapabilityOf(Work::Item).refusal) == "unsupported:item",
          "without a rule an item stays unsupported:item");
    Check(WorkOf(Objective(ObjectiveKind::Kill, 1, 1, 0)) == Work::Kill, "without a rule a kill stays a kill");
}

// (b) the index places the rules' objectives; the step function works them
void IndexPlaces(Impl const& impl)
{
    QuestIndex const idx = QuestIndex::Build(Gather());
    QuestRecord const* dead = idx.Find(Q_DEAD);
    QuestRecord const* credit = idx.Find(Q_CREDIT);
    QuestRecord const* event = idx.Find(Q_EVENT);
    QuestRecord const* object = idx.Find(Q_OBJECT);
    QuestRecord const* nospawn = idx.Find(Q_NOSPAWN);
    if (!Check(dead && credit && event && object && nospawn, "the synthetic quests are indexed"))
        return;

    QuestObjective const& shaman = dead->objectives[0];
    Check(shaman.use == UseHow::Item && shaman.useKind == UseKind::UnitDead, "the shaman's kill is a dead use");
    Check(shaman.placeFrom == PlaceFrom::UseRule && shaman.areas.size() == 1 &&
              std::fabs(shaman.areas[0].z - 33.5f) < 0.01f,
          "the dead use is placed at the shaman's spawn with its height");
    Check(impl.work(shaman) == Work::Use, "the shaman's objective is worked as a use");

    QuestObjective const& owlkin = credit->objectives[0];
    Check(owlkin.use == UseHow::Item && owlkin.placeFrom == PlaceFrom::UseRule && !owlkin.areas.empty() &&
              std::fabs(owlkin.areas[0].z - 84.25f) < 0.01f,
          "a completing rule lands on the credit no spawn places, at the owlkin's spawns");
    Check(impl.work(owlkin) == Work::Use, "the credit objective is worked as a use");

    Check(event->eventUse.use == UseHow::Item && event->eventUse.placeFrom == PlaceFrom::UseRule &&
              event->eventUse.poiIndex == POI_NONE && !event->eventUse.areas.empty(),
          "a completing rule of an event quest is the event's, at the beast's spawn");
    Check(event->objectives[0].use == UseHow::None, "the rod's own counter gets no rule");
    Check(ObjectiveCount(*event) == event->objectives.size() + 1, "the event counts as an objective");

    QuestObjective const& pillar = object->objectives[0];
    Check(pillar.use == UseHow::Item && pillar.useKind == UseKind::Object && pillar.placeFrom == PlaceFrom::UseRule &&
              pillar.areas.size() == 1 && std::fabs(pillar.areas[0].x + 4395.0f) < 0.01f,
          "an object rule takes the gameobject's spawns, not the creature with its number");
    Check(impl.work(pillar) == Work::Use, "the object's objective is worked as a use");

    QuestObjective const& ghost = nospawn->objectives[0];
    Check(ghost.use == UseHow::Item && ghost.placeFrom == PlaceFrom::None && ghost.areas.empty(),
          "a rule's entry without a spawn leaves the objective without a place");

    // Steps: the event of the rod quest is worked once the rod's counter is met; the shaman's objective at its spawn
    auto const eventOpen = [](std::int32_t slot) { return slot == POI_NONE; };
    Step const rod = StepOf(*event, eventOpen, {}, 400.0f, -4700.0f, 1);
    Check(rod.act == Act::Work && rod.work == Work::Use && rod.objective == event->objectives.size() && rod.place &&
              std::fabs(rod.place->z - 21.0f) < 0.01f,
          "the event's use is a step at the beast's place");
    QuestRecord noRule = *event;
    noRule.eventUse = QuestObjective{ObjectiveKind::Turnin, 0, 1, POI_NONE, {}};
    Step const refused = StepOf(noRule, eventOpen, {}, 400.0f, -4700.0f, 1);
    Check(refused.act == Act::Refused && refused.work == Work::Event, "an event without a rule stays refused");
    QuestRecord escort = *event;
    escort.event = QuestEvent::Escort;
    Step const walked = StepOf(escort, eventOpen, {}, 400.0f, -4700.0f, 1);
    Check(walked.act == Act::Refused && walked.work == Work::Escort, "an escort stays refused even with a rule");

    auto const open = [](std::int32_t slot) { return slot == 0; };
    Step const first = StepOf(*dead, open, {});
    Check(first.act == Act::Work && first.work == Work::Use && first.place && first.place->z > 33.0f,
          "the shaman's use is a step at its place");
    // (e) resume: a fresh start with the same log and tried places takes the same step
    Check(StepOf(*dead, open, {}) == first, "a fresh start takes the same step");
    std::vector<PlaceKey> const tried{KeyOf(first)};
    Step const after = StepOf(*dead, open, tried);
    Check(after.act == Act::NoTarget && StepOf(*dead, open, tried) == after,
          "the only place tried ends as no_target, again on a fresh start");
}

// (c) the verdict at a target
void Verdicts(Impl const& impl)
{
    UseSeen living{true, false, false, false, true, false};
    Check(impl.verdict(living) == UseAct::Use, "a living target in reach is used");
    UseSeen far = living;
    far.inReach = false;
    Check(impl.verdict(far) == UseAct::Walk, "out of reach walks");
    UseSeen wantDead = living;
    wantDead.wantDead = true;
    Check(impl.verdict(wantDead) == UseAct::Skip, "a living target for a dead rule is skipped");
    UseSeen corpse = wantDead;
    corpse.dead = true;
    Check(impl.verdict(corpse) == UseAct::Use, "a dead target for a dead rule is used");
    UseSeen killed = living;
    killed.dead = true;
    Check(impl.verdict(killed) == UseAct::Skip, "a dead target for a living rule is skipped");
    UseSeen gone = living;
    gone.present = false;
    Check(impl.verdict(gone) == UseAct::Skip, "a target gone is skipped");
    UseSeen passedOver = living;
    passedOver.passed = true;
    Check(impl.verdict(passedOver) == UseAct::Skip, "a target passed over is skipped");
    UseSeen taken = living;
    taken.taken = true;
    Check(impl.verdict(taken) == UseAct::Skip, "a target another player's use works on is skipped");
    taken.inReach = false;
    Check(impl.verdict(taken) == UseAct::Skip, "a taken target out of reach is not walked to");
    UseSeen pending = living;
    pending.pending = true;
    Check(impl.verdict(pending) == UseAct::Wait, "a pending use waits, no second request");
    pending.present = false;
    Check(impl.verdict(pending) == UseAct::Wait, "a pending use waits even when its target is out of sight");
}

// (d) the cause table and its end words
void Causes(Impl const& impl)
{
    for (std::size_t i = 0; i < ItemUse::CAUSE_COUNT; ++i)
    {
        ItemUse::Cause const cause = static_cast<ItemUse::Cause>(i);
        Refusal const refusal = RefusalOf(cause);
        std::string const word(ItemUse::CAUSE_WORDS[i]);
        Check(refusal == Refusal::Retry || refusal == Refusal::PassOver || refusal == Refusal::GiveUp,
              "cause " + word + " is classified");
        std::string const end = GiveUpWord(cause);
        Check(cause == ItemUse::Cause::NoItem ? end == NO_ITEM : end == "refused:" + word,
              "cause " + word + " ends as " + end);
        // every retriable cause is passed over within the bound
        std::uint8_t tries = 0;
        Refusal now = impl.after(cause, tries);
        while (now == Refusal::Retry && tries < 50)
            now = impl.after(cause, ++tries);
        Check(refusal != Refusal::Retry || (now == Refusal::PassOver && tries + 1 == USE_RETRIES),
              "cause " + word + " is retried at most " + std::to_string(USE_RETRIES) + " times");
    }
    Check(RefusalOf(ItemUse::Cause::NoItem) == Refusal::GiveUp, "no_item ends the order");
    Check(RefusalOf(ItemUse::Cause::Level) == Refusal::GiveUp, "level ends the order");
    Check(RefusalOf(ItemUse::Cause::BadTarget) == Refusal::PassOver, "a refused target is passed over");
    Check(RefusalOf(ItemUse::Cause::Busy) == Refusal::Retry, "busy is waited for");
    Check(RefusalOf(static_cast<ItemUse::Cause>(200)) == Refusal::GiveUp, "an unknown cause ends the order");
    Check(AimOf(ItemUse::Shape::Enemy) == UseAim::Unit && AimOf(ItemUse::Shape::Friend) == UseAim::Unit &&
              AimOf(ItemUse::Shape::Self) == UseAim::Self && AimOf(ItemUse::Shape::Ground) == UseAim::Spot &&
              AimOf(ItemUse::Shape::Object) == UseAim::Object && AimOf(ItemUse::Shape::None) == UseAim::Self,
          "the record's shape aims the request");
    std::uint8_t casts = 0;
    while (AfterCast(casts) == Refusal::Retry && casts < 50)
        ++casts;
    Check(casts + 1 == USE_RETRIES, "a refused cast is tried at most " + std::to_string(USE_RETRIES) + " times");
    Check(UseReach(30.0f) == USE_REACH_MAX_YARDS && UseReach(5.0f) == USE_REACH_MIN_YARDS &&
              std::fabs(UseReach(10.0f) - 8.0f) < 0.01f,
          "the walk-in distance stays inside the range");
}

// One quest_use row per request, at the request: a timed use's row is written when it is asked (its end is the
// framework's ItemUse row), so a use that completes the quest before the next tick still has its row
void Rows(Impl const& impl)
{
    Check(impl.row(ItemUse::Outcome::Used, ItemUse::Cause::None) == "used", "an instant use's row says used");
    Check(impl.row(ItemUse::Outcome::Pending, ItemUse::Cause::None) == "pending",
          "a timed use's row is written at the request: pending");
    for (std::size_t i = 0; i < ItemUse::CAUSE_COUNT; ++i)
    {
        ItemUse::Cause const cause = static_cast<ItemUse::Cause>(i);
        std::string const word(ItemUse::CAUSE_WORDS[i]);
        Check(impl.row(ItemUse::Outcome::Refused, cause) == "refused:" + word, "a refusal's row says refused:" + word);
    }
}

// Part 2: the AI is held through the bot's own pending quest channel, not through another's, not at low health
void Holds(Impl const& impl)
{
    Check(impl.hold(true, true, false), "a pending quest channel holds the fight AI");
    Check(!impl.hold(false, true, false), "a channel that is no quest use does not hold");
    Check(!impl.hold(true, false, false), "no channel (the cast still prepares, or it ended): no hold");
    Check(!impl.hold(true, true, true), "at low health the bot breaks off and fights");
}

// WQL2b note 2: a stalled walk tries a place only while its objective has another untried usable place on the map
void Stalls(Impl const& impl)
{
    QuestRecord quest = Quest(90020);
    QuestObjective objective = Objective(ObjectiveKind::Kill, 1, 1, 0);
    AreaPoint here;
    here.mapId = MAP;
    here.x = 10.0f;
    objective.areas = {here};
    quest.objectives.push_back(objective);
    Check(!impl.stall(quest, {}, Key(0, 0)), "the objective's only place: the walk starts again");
    AreaPoint elsewhere = here;
    elsewhere.mapId = MAP + 1;
    quest.objectives[0].areas.push_back(elsewhere);
    Check(!impl.stall(quest, {}, Key(0, 0)), "a place on another map is none left");
    AreaPoint next = here;
    next.x = 200.0f;
    quest.objectives[0].areas.push_back(next);
    Check(impl.stall(quest, {}, Key(0, 0)), "another place left: the stalled one is tried");
    Check(!impl.stall(quest, {Key(0, 2)}, Key(0, 0)), "the other place tried: the walk starts again");
    quest.objectives.push_back(objective);
    Check(!impl.stall(quest, {Key(0, 2)}, Key(0, 0)), "another objective's place does not count");
}

void Picks()
{
    auto const rule = [](std::uint32_t objective, std::uint32_t item, std::uint32_t spell, UseKind kind)
    {
        QuestUseRule r;
        r.objective = objective;
        r.itemId = item;
        r.spellId = spell;
        r.kind = kind;
        return r;
    };
    std::vector<QuestUseRule> const rules = {rule(USE_COMPLETES_QUEST, 101, 11, UseKind::UnitAlive),
                                             rule(0, 100, 10, UseKind::UnitAlive),
                                             rule(0, 102, 12, UseKind::Object),
                                             rule(0, 103, 0, UseKind::UnitAlive),
                                             rule(1, 0, 20, UseKind::UnitAlive),
                                             rule(1, 0, 21, UseKind::UnitAlive)};
    auto const pick = [&rules](std::uint32_t objective, UseKind kind, std::vector<std::uint32_t> const& held,
                               std::vector<std::uint32_t> const& known)
    {
        return PickRule(
            rules.begin(), rules.end(), objective, kind,
            [&held](std::uint32_t item) { return std::find(held.begin(), held.end(), item) != held.end(); },
            [&known](std::uint32_t spell) { return std::find(known.begin(), known.end(), spell) != known.end(); });
    };
    UsePick const own = pick(0, UseKind::UnitAlive, {100, 101}, {});
    Check(own.rule && own.rule->itemId == 100, "the objective's own rule comes before a completing one");
    UsePick const completes = pick(0, UseKind::UnitAlive, {101}, {});
    Check(completes.rule && completes.rule->itemId == 101, "a completing rule when the own item is not held");
    UsePick const none = pick(0, UseKind::UnitAlive, {103}, {});
    Check(!none.rule && std::string(none.word) == NO_ITEM, "no held item: no_item (the click rule is not an item)");
    UsePick const kind = pick(0, UseKind::Object, {100}, {});
    Check(!kind.rule && std::string(kind.word) == NO_ITEM, "a rule of another kind is not picked");
    UsePick const rank = pick(1, UseKind::UnitAlive, {}, {21});
    Check(rank.rule && rank.rule->spellId == 21, "the known rank of a class spell");
    UsePick const unknown = pick(1, UseKind::UnitAlive, {}, {});
    Check(!unknown.rule && std::string(unknown.word) == NO_ITEM, "the completing item rule's absence is no_item");
    std::vector<QuestUseRule> const spells = {rule(0, 0, 20, UseKind::UnitAlive)};
    UsePick const noSpell = PickRule(
        spells.begin(), spells.end(), 0, UseKind::UnitAlive, [](std::uint32_t) { return true; },
        [](std::uint32_t) { return false; });
    Check(!noSpell.rule && std::string(noSpell.word) == NO_SPELL, "an unknown class spell: no_spell");
}

void RealRules()
{
    WorkRows(REAL);
    Capabilities();
    IndexPlaces(REAL);
    Verdicts(REAL);
    Causes(REAL);
    Rows(REAL);
    Holds(REAL);
    Stalls(REAL);
    Chains(REAL);
    Picks();
}

// (f) mutants: each breaks one rule; the checks above must see it
void Mutants()
{
    struct Mutant
    {
        char const* name;
        Impl impl;
        std::function<void(Impl const&)> run;
    };
    Impl byKind = REAL;
    byKind.work = [](QuestObjective const& objective)
    {
        QuestObjective plain = objective;
        plain.use = UseHow::None;
        return WorkOf(plain);
    };
    Impl anyLife = REAL;
    anyLife.verdict = [](UseSeen const& seen)
    {
        UseSeen alive = seen;
        alive.dead = alive.wantDead;
        return UseVerdict(alive);
    };
    Impl noWait = REAL;
    noWait.verdict = [](UseSeen const& seen)
    {
        UseSeen now = seen;
        now.pending = false;
        return UseVerdict(now);
    };
    Impl shared = REAL;
    shared.verdict = [](UseSeen const& seen)
    {
        UseSeen free = seen;
        free.taken = false;
        return UseVerdict(free);
    };
    Impl unbounded = REAL;
    unbounded.after = [](ItemUse::Cause cause, std::uint8_t) { return RefusalOf(cause); };
    Impl rowAtEnd = REAL;
    rowAtEnd.row = [](ItemUse::Outcome outcome, ItemUse::Cause cause)
    { return outcome == ItemUse::Outcome::Pending ? std::string() : UseRowWord(outcome, cause); };
    Impl fightsOn = REAL;
    fightsOn.hold = [](bool, bool, bool) { return false; };
    Impl passesTurned = REAL;
    passesTurned.passes = [](UseBrings, std::uint32_t, std::uint32_t) { return true; };
    Impl noChain = REAL;
    noChain.chains = [](QuestObjective const&) { return false; };
    Impl stallTries = REAL;
    stallTries.stall = [](QuestRecord const&, std::vector<PlaceKey> const&, PlaceKey) { return true; };
    std::vector<Mutant> const mutants = {
        {"a rule ignored (WorkOf by kind)", byKind, [](Impl const& i) { IndexPlaces(i); }},
        {"a dead rule accepting a living target", anyLife, [](Impl const& i) { Verdicts(i); }},
        {"no pending wait", noWait, [](Impl const& i) { Verdicts(i); }},
        {"another player's target used", shared, [](Impl const& i) { Verdicts(i); }},
        {"unbounded retries", unbounded, [](Impl const& i) { Causes(i); }},
        {"a timed use's row at its end", rowAtEnd, [](Impl const& i) { Rows(i); }},
        {"a quest channel left to the fight AI", fightsOn, [](Impl const& i) { Holds(i); }},
        {"a stall tries the only place", stallTries, [](Impl const& i) { Stalls(i); }},
        {"a transformed target passed over", passesTurned, [](Impl const& i) { Chains(i); }},
        {"no kill after a summoning use", noChain, [](Impl const& i) { Chains(i); }},
    };
    for (Mutant const& mutant : mutants)
    {
        int const before = failed;
        int const passedBefore = passed;
        quiet = true;
        mutant.run(mutant.impl);
        quiet = false;
        bool const caught = failed > before;
        failed = before;
        passed = passedBefore;
        Check(caught, std::string("mutant caught: ") + mutant.name);
    }
}
}  // namespace

int main()
{
    RealRules();
    Mutants();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
