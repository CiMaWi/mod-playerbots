/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Quest items and spells (upkeep plan WU20b): the census cases from the world DB as synthetic gathers, then a seeded
// search over random gathers for the invariant (K1-K3). Standalone:
//   clang-cl /std:c++20 /EHsc /W4 /I src/Ai/World/Rpg tests/quest-use-selftest.cpp

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "QuestUseRules.h"

using namespace Planner;

namespace
{
int passed = 0;
int failed = 0;

void Check(bool ok, std::string const& what)
{
    if (ok)
        ++passed;
    else
    {
        ++failed;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

UseSpell Spell(std::uint32_t id, std::uint8_t targets, std::vector<UseEntry> named = {}, std::uint32_t focus = 0,
               std::vector<std::uint32_t> focusObjects = {})
{
    UseSpell spell;
    spell.spellId = id;
    spell.targets = targets;
    spell.rangeYards = 30.0f;
    spell.named = std::move(named);
    spell.focusId = focus;
    spell.focusObjects = std::move(focusObjects);
    spell.focusYards = focus ? 10.0f : 0.0f;
    return spell;
}

UseQuest Quest(std::uint32_t id, std::vector<std::pair<UseGoalKind, std::uint32_t>> goals, std::uint32_t classMask = 0)
{
    return {id, classMask, std::move(goals)};
}

std::vector<QuestUseRule> RulesOf(QuestUseTable const& table, std::uint32_t questId)
{
    auto const [first, last] = table.RulesFor(questId);
    return {first, last};
}

void CensusCases()
{
    QuestUseGather gather;
    std::vector<UseQuest> quests;
    // Taming rod 15920 (spell 19697) names beast 3126 through source type 17, condition 31; the rod is the counter
    gather.spells.push_back(Spell(19697, UseTarget::UNIT, {{3126, false, false}}));
    gather.items.push_back({6082, 15920, 19697, {}});
    quests.push_back(Quest(6082, {{UseGoalKind::Item, 15920}}, 4));
    // Symbol of Life (8593) on the dead Young Furbolg Shaman 17542: types 13 (31) and 17 (29), plus two other quests'
    // creatures; 17542 reacts to it (event 8)
    gather.spells.push_back(Spell(8593, UseTarget::UNIT | UseTarget::DEAD,
                                  {{6172, false, true}, {6177, false, true}, {17542, false, true}}));
    gather.items.push_back({9600, 6866, 8593, {}});
    gather.hits.push_back({17542, 8593});
    quests.push_back(Quest(9600, {{UseGoalKind::Kill, 17542}}, 2));
    // Runes of Summoning (9221) name nothing: the summoning circle comes from the spell focus 43
    gather.spells.push_back(Spell(9221, UseTarget::SELF, {}, 43, {37097}));
    gather.items.push_back({1471, 6284, 9221, {}});
    quests.push_back(Quest(1471, {{UseGoalKind::Kill, 5676}, {UseGoalKind::Item, 6284}}, 256));
    // Ravager Cage Key (30767) opens the cage whose lock takes the key
    gather.spells.push_back(Spell(30767, UseTarget::OBJECT));
    gather.items.push_back({9375, 23925, 30767, {181849}});
    quests.push_back(Quest(9375, {}));
    // Redemption: the Filled Shimmering Vessel (31225) on Blood Knight Stillblade 17768 (type 13; event 8)
    gather.spells.push_back(Spell(31225, UseTarget::UNIT | UseTarget::DEAD, {{17768, false, true}}));
    gather.items.push_back({9685, 24184, 31225, {}});
    gather.hits.push_back({17768, 31225});
    quests.push_back(Quest(9685, {{UseGoalKind::Kill, 17768}}, 2));
    // A class spell a creature reacts to, without an item: kept for a class quest, not for an open one
    UseSpell redemption = Spell(7328, UseTarget::UNIT | UseTarget::DEAD);
    redemption.requiresDead = true;
    redemption.classFamily = true;
    gather.spells.push_back(redemption);
    gather.hits.push_back({90100, 7328});
    quests.push_back(Quest(90001, {{UseGoalKind::Kill, 90100}}, 2));
    quests.push_back(Quest(90002, {{UseGoalKind::Kill, 90100}}, 0));
    // The Flask of Purest Water (9509) has no on-use spell: nothing is gathered for it
    quests.push_back(Quest(9509, {{UseGoalKind::Item, 23752}}, 64));
    // Fel Fire (23054) at Kroshius' Remains (focus 1243): Kroshius drops the core the quest counts
    gather.spells.push_back(Spell(23054, UseTarget::SELF, {}, 1243, {179677}));
    gather.items.push_back({7603, 18626, 23054, {}});
    quests.push_back(Quest(7603, {{UseGoalKind::Item, 18625}}, 256));
    // Cenarion Moondust (18974) at the Moonkin Stone (focus 943); the quest has no counter
    gather.spells.push_back(Spell(18974, UseTarget::SELF, {}, 943, {177525}));
    gather.items.push_back({6001, 15208, 18974, {}});
    quests.push_back(Quest(6001, {}, 1024));
    // 1819: the Mausoleum Trigger's action list summons Ulag the Cleaver
    gather.creators.push_back({104593, 6390});
    quests.push_back(Quest(1819, {{UseGoalKind::Kill, 6390}}, 1));
    // No source item, and a source item whose spell names nothing
    quests.push_back(Quest(90003, {{UseGoalKind::Kill, 123}}));
    gather.spells.push_back(Spell(90200, UseTarget::UNIT));
    gather.items.push_back({90004, 90201, 90200, {}});
    quests.push_back(Quest(90004, {{UseGoalKind::Kill, 124}}));

    QuestUseTable const table = QuestUseTable::Build(gather, quests);
    auto const one = [&table](std::uint32_t quest, char const* name) -> QuestUseRule
    {
        std::vector<QuestUseRule> const found = RulesOf(table, quest);
        Check(found.size() == 1, std::string(name) + ": one rule, got " + std::to_string(found.size()));
        return found.empty() ? QuestUseRule{} : found.front();
    };

    QuestUseRule rule = one(6082, "taming rod");
    Check(rule.kind == UseKind::UnitAlive && rule.entries == std::vector<std::uint32_t>{3126} && rule.itemId == 15920 &&
              rule.objective == USE_COMPLETES_QUEST && !rule.mustBeDead,
          "taming rod 15920 on the living beast 3126, completing the quest");
    rule = one(9600, "symbol of life");
    Check(rule.kind == UseKind::UnitDead && rule.entries == std::vector<std::uint32_t>{17542} && rule.objective == 0 &&
              rule.mustBeDead && rule.spellId == 8593,
          "Symbol of Life on the dead 17542 only (the other quests' creatures dropped), the kill counter");
    rule = one(1471, "runes of summoning");
    Check(rule.kind == UseKind::Focus && rule.entries == std::vector<std::uint32_t>{37097} && rule.standAtFocus &&
              rule.focusId == 43 && rule.objective == 0 && rule.rangeYards == 10.0f,
          "Runes of Summoning at the summoning circle (focus 43), for the summoned voidwalker");
    rule = one(9375, "ravager cage key");
    Check(rule.kind == UseKind::Object && rule.entries == std::vector<std::uint32_t>{181849} && rule.itemId == 23925,
          "Ravager Cage Key at the cage it unlocks");
    rule = one(9685, "redemption");
    Check(rule.kind == UseKind::UnitDead && rule.entries == std::vector<std::uint32_t>{17768} && rule.objective == 0 &&
              rule.itemId == 24184 && !rule.classSpell,
          "the vessel's Redemption spell on Blood Knight Stillblade 17768");
    rule = one(90001, "class spell");
    Check(rule.classSpell && rule.itemId == 0 && rule.spellId == 7328 && rule.kind == UseKind::UnitDead &&
              rule.entries == std::vector<std::uint32_t>{90100},
          "a class quest's creature reacting to a class spell: a class-spell rule");
    Check(RulesOf(table, 90002).empty(), "the same creature in an open quest: no class-spell rule");
    Check(RulesOf(table, 9509).empty(), "the Flask of Purest Water has no on-use spell: no rule");
    rule = one(7603, "fel fire");
    Check(rule.kind == UseKind::Focus && rule.entries == std::vector<std::uint32_t>{179677} && rule.objective == 0,
          "Fel Fire at Kroshius' Remains, for the core Kroshius drops");
    rule = one(6001, "moondust");
    Check(rule.kind == UseKind::Focus && rule.entries == std::vector<std::uint32_t>{177525} &&
              rule.objective == USE_COMPLETES_QUEST,
          "Cenarion Moondust at the Moonkin Stone");
    rule = one(1819, "mausoleum");
    Check(rule.kind == UseKind::Object && rule.spellId == 0 && rule.itemId == 0 &&
              rule.entries == std::vector<std::uint32_t>{104593} && rule.objective == 0,
          "1819: use the Mausoleum Trigger that summons Ulag");
    Check(RulesOf(table, 90003).empty(), "a quest without a source item gets no rule");
    Check(RulesOf(table, 90004).empty() && table.stats.unnamed == 1, "a spell that names nothing: no rule, counted");
    Check(table.stats.violations == 0, "the census table holds the invariant");
    Check(table.stats.quests == 9 && table.stats.byKind[0] == 1 && table.stats.byKind[1] == 3 &&
              table.stats.byKind[2] == 2 && table.stats.byKind[3] == 3,
          "the counts by kind: 1 unit, 3 dead, 2 object, 3 focus");

    // The checker catches broken rules
    QuestUseTable broken = table;
    broken.rules.push_back({9600, 0, 6866, 8593, UseKind::Object, {17542}, 0.0f, false, false, 0, false});
    broken.rules.push_back({90003, 0, 0, 0, UseKind::Object, {}, 0.0f, false, false, 0, false});
    broken.rules.push_back({9685, 0, 6866, 8593, UseKind::UnitDead, {17542}, 0.0f, false, false, 0, false});
    broken.rules.push_back({90002, 0, 0, 7328, UseKind::UnitDead, {90100}, 0.0f, true, false, 0, true});
    broken.rules.push_back({1819, 0, 0, 0, UseKind::Object, {37097}, 0.0f, false, false, 0, false});
    broken.rules.push_back(
        {6082, USE_COMPLETES_QUEST, 15920, 19697, UseKind::UnitAlive, {3127}, 0.0f, false, false, 0, false});
    Check(broken.Violations().size() == 6,
          "six broken rules, six violations, got " + std::to_string(broken.Violations().size()));
}

// Random gathers: every rule holds K1-K3, and a quest whose items name nothing and whose creatures neither react to a
// class spell nor have a creator gets no rule
void Search(std::uint32_t seed, int rounds)
{
    std::mt19937 rng(seed);
    auto const pick = [&rng](std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); };
    int rules = 0;
    int quests = 0;
    for (int round = 0; round < rounds; ++round)
    {
        QuestUseGather gather;
        std::vector<UseQuest> list;
        std::uint32_t const spells = 1 + pick(12);
        for (std::uint32_t s = 1; s <= spells; ++s)
        {
            UseSpell spell = Spell(s, static_cast<std::uint8_t>(pick(16)));
            spell.classFamily = pick(2) == 0;
            spell.requiresDead = pick(4) == 0;
            for (std::uint32_t n = pick(3); n; --n)
                spell.named.push_back({1 + pick(8), pick(3) == 0, pick(3) == 0});
            if (pick(4) == 0)
            {
                spell.focusId = 1 + pick(3);
                if (pick(3))
                    spell.focusObjects.push_back(100 + spell.focusId);
            }
            gather.spells.push_back(spell);
        }
        for (std::uint32_t n = pick(6); n; --n)
            gather.hits.push_back({1 + pick(8), 1 + pick(spells + 2)});
        for (std::uint32_t n = pick(3); n; --n)
            gather.creators.push_back({200 + pick(4), 1 + pick(8)});
        std::uint32_t const count = 1 + pick(6);
        for (std::uint32_t q = 1; q <= count; ++q)
        {
            UseQuest quest = Quest(q, {}, pick(2) ? 1u << pick(10) : 0u);
            for (std::uint32_t n = pick(4); n; --n)
                quest.goals.push_back({static_cast<UseGoalKind>(pick(4)), 1 + pick(8)});
            for (std::uint32_t n = pick(3); n; --n)
            {
                UseItem item{q, 50 + pick(4), 1 + pick(spells + 1), {}};
                if (pick(4) == 0)
                    item.lockObjects.push_back(300 + pick(2));
                gather.items.push_back(item);
            }
            list.push_back(quest);
        }
        if (pick(5) == 0)
            gather.items.push_back({count + 7, 60, 1, {}});  // an item of a quest the index does not hold
        QuestUseTable const table = QuestUseTable::Build(gather, list);
        std::vector<std::string> const violations = table.Violations();
        if (!violations.empty())
        {
            Check(false,
                  "seed " + std::to_string(seed) + " round " + std::to_string(round) + ": " + violations.front());
            return;
        }
        for (UseQuest const& quest : list)
        {
            bool named = false;
            for (UseItem const& item : gather.items)
                if (item.questId == quest.id)
                    for (UseSpell const& spell : gather.spells)
                        named = named ||
                                (spell.spellId == item.spellId &&
                                 (!spell.named.empty() || !spell.focusObjects.empty() || !item.lockObjects.empty() ||
                                  std::any_of(gather.hits.begin(), gather.hits.end(),
                                              [&spell](SpellHit const& hit) { return hit.spellId == spell.spellId; })));
            for (auto const& [kind, target] : quest.goals)
                for (SpellHit const& hit : gather.hits)
                    named = named || (kind == UseGoalKind::Kill && hit.creature == target);
            for (auto const& [kind, target] : quest.goals)
                for (CreatorObject const& creator : gather.creators)
                    named = named || (kind == UseGoalKind::Kill && creator.creature == target);
            std::vector<QuestUseRule> const found = RulesOf(table, quest.id);
            if (!named && !found.empty())
            {
                Check(false, "seed " + std::to_string(seed) + " round " + std::to_string(round) + ": quest " +
                                 std::to_string(quest.id) + " names no target but has a rule");
                return;
            }
            rules += static_cast<int>(found.size());
            quests += found.empty() ? 0 : 1;
        }
    }
    std::printf("search: seed %u, %d rounds, %d quests with %d rules, no counterexample\n", seed, rounds, quests,
                rules);
    Check(rules > 0, "the search produced rules");
}
}  // namespace

int main()
{
    CensusCases();
    Search(20261114u, 20000);
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
