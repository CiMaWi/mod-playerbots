/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTUSERULES_H
#define _PLAYERBOT_QUESTUSERULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Quest items and spells, what to use where (upkeep plan WU20b, PLAN 3.11): the world thread gathers each quest's
// source items with an on-use spell, what each spell can target, the creatures and objects named for it (conditions,
// spell focus, lock keys), the spell-hit credits of objective creatures and the objects whose scripts summon one; the
// rules here turn that into which item or spell to use on which target. No game header: tests/quest-use-selftest.cpp
// builds it from synthetic input. A quest without a named target gets no rule and behaves as before
namespace Planner
{
// What a spell can take, from its implicit and explicit targets
namespace UseTarget
{
constexpr std::uint8_t UNIT = 0x01;
constexpr std::uint8_t DEAD = 0x02;    // dead units or corpses
constexpr std::uint8_t OBJECT = 0x04;  // gameobjects
constexpr std::uint8_t SELF = 0x08;    // the caster or a place around it
}  // namespace UseTarget

enum class UseKind : std::uint8_t
{
    UnitAlive,
    UnitDead,
    Object,  // use the item or spell on the object, or use the object itself
    Focus,   // stand at the spell focus object and use the item
    Count
};
constexpr std::size_t USE_KINDS = static_cast<std::size_t>(UseKind::Count);
constexpr std::array<char const*, USE_KINDS> USE_KIND_NAMES = {"unit", "dead", "object", "focus"};

inline char const* UseKindName(UseKind kind) { return USE_KIND_NAMES[static_cast<std::size_t>(kind)]; }

constexpr std::uint32_t USE_COMPLETES_QUEST = 0xFFFFFFFFu;  // the rule's objective: the use completes the quest

// A creature or gameobject named for a spell by conditions (source types 13 and 17, condition types 31 and 29)
struct UseEntry
{
    std::uint32_t entry{0};
    bool isObject{false};
    bool dead{false};  // condition 29 asks for a dead creature
};

// One spell (an item's on-use spell or a spell an objective creature reacts to), gathered once
struct UseSpell
{
    std::uint32_t spellId{0};
    std::uint8_t targets{0};  // UseTarget
    bool requiresDead{false};
    bool classFamily{false};  // a class's spell family, not generic
    std::uint32_t focusId{0};
    float rangeYards{0.0f};
    float focusYards{0.0f};                   // the focus objects' smallest radius
    std::vector<UseEntry> named;              // ascending
    std::vector<std::uint32_t> focusObjects;  // gameobject templates of the focus, ascending
};

// A quest's source item (StartItem, ItemDrop1-4) with an on-use spell
struct UseItem
{
    std::uint32_t questId{0};
    std::uint32_t itemId{0};
    std::uint32_t spellId{0};
    std::vector<std::uint32_t> lockObjects;  // gameobject templates whose lock takes the item as its key, ascending
};

// smart_scripts event 8 on an objective creature
struct SpellHit
{
    std::uint32_t creature{0};
    std::uint32_t spellId{0};
};

// A gameobject whose smart_scripts (directly or through an action list) summon or credit an objective creature
struct CreatorObject
{
    std::uint32_t object{0};
    std::uint32_t creature{0};
};

struct QuestUseGather
{
    std::vector<UseSpell> spells;
    std::vector<UseItem> items;
    std::vector<SpellHit> hits;
    std::vector<CreatorObject> creators;
};

enum class UseGoalKind : std::uint8_t
{
    Kill,
    Object,
    Item,
    Other
};

// The part of a quest the rules read: its id, class mask and objectives in the index's order
struct UseQuest
{
    std::uint32_t id{0};
    std::uint32_t classMask{0};
    std::vector<std::pair<UseGoalKind, std::uint32_t>> goals;
};

struct QuestUseRule
{
    std::uint32_t questId{0};
    std::uint32_t objective{USE_COMPLETES_QUEST};  // index into the quest's objectives
    std::uint32_t itemId{0};                       // 0: a class spell, or the object itself is used
    std::uint32_t spellId{0};                      // 0: the object itself is used
    UseKind kind{UseKind::UnitAlive};
    std::vector<std::uint32_t> entries;  // creature or gameobject templates, ascending
    float rangeYards{0.0f};
    bool mustBeDead{false};
    bool standAtFocus{false};
    std::uint32_t focusId{0};
    bool classSpell{false};  // the bot must know the spell; WU20c checks it at use time
};

struct QuestUseStats
{
    std::uint32_t items{0};  // source items with an on-use spell
    std::uint32_t rules{0};
    std::uint32_t quests{0};                        // quests with a rule
    std::array<std::uint32_t, USE_KINDS> byKind{};  // quests with a rule of the kind
    std::uint32_t unnamed{0};                       // source items with a spell and no named target: no rule
    std::uint32_t violations{0};
};

// Whether a spell with these targets can be used in the kind of rule
inline bool UseFits(UseKind kind, UseSpell const& spell)
{
    switch (kind)
    {
        case UseKind::UnitAlive:
            return (spell.targets & UseTarget::UNIT) != 0;
        case UseKind::UnitDead:
            return (spell.targets & (UseTarget::UNIT | UseTarget::DEAD)) != 0;
        case UseKind::Object:
            return (spell.targets & UseTarget::OBJECT) != 0;
        case UseKind::Focus:
            return spell.focusId != 0;
        default:
            return false;
    }
}

// Immutable once built
class QuestUseTable
{
public:
    std::vector<QuestUseRule> rules;  // by quest, then objective
    QuestUseStats stats;

    static QuestUseTable Build(QuestUseGather input, std::vector<UseQuest> const& quests)
    {
        QuestUseTable table;
        table.Index(std::move(input));
        for (UseQuest const& quest : quests)
        {
            table._quests[quest.id] = quest;
            std::size_t const before = table.rules.size();
            table.AddRules(quest);
            if (table.rules.size() == before)
                continue;
            ++table.stats.quests;
            std::array<bool, USE_KINDS> seen{};
            for (std::size_t i = before; i < table.rules.size(); ++i)
                seen[static_cast<std::size_t>(table.rules[i].kind)] = true;
            for (std::size_t k = 0; k < USE_KINDS; ++k)
                table.stats.byKind[k] += seen[k] ? 1 : 0;
        }
        std::stable_sort(table.rules.begin(), table.rules.end(), [](QuestUseRule const& a, QuestUseRule const& b)
                         { return std::tie(a.questId, a.objective) < std::tie(b.questId, b.objective); });
        table.stats.rules = static_cast<std::uint32_t>(table.rules.size());
        table.stats.violations = static_cast<std::uint32_t>(table.Violations().size());
        return table;
    }

    std::pair<std::vector<QuestUseRule>::const_iterator, std::vector<QuestUseRule>::const_iterator> RulesFor(
        std::uint32_t questId) const
    {
        auto const first =
            std::lower_bound(rules.begin(), rules.end(), questId,
                             [](QuestUseRule const& rule, std::uint32_t id) { return rule.questId < id; });
        auto const last = std::upper_bound(
            first, rules.end(), questId, [](std::uint32_t id, QuestUseRule const& rule) { return id < rule.questId; });
        return {first, last};
    }

    UseSpell const* Spell(std::uint32_t spellId) const
    {
        auto const it = _spells.find(spellId);
        return it != _spells.end() ? &it->second : nullptr;
    }

    // The quest's source items with a spell, also those without a rule
    std::vector<UseItem const*> ItemsOf(std::uint32_t questId) const
    {
        std::vector<UseItem const*> found;
        auto const range = _itemsByQuest.equal_range(questId);
        for (auto it = range.first; it != range.second; ++it)
            found.push_back(&_items[it->second]);
        return found;
    }

    // The invariant (K1-K3), one line per broken rule: every rule names a target entry, from a source that names it,
    // and a kind its spell can take; its quest is indexed and its item is one of the quest's source items with the
    // spell, or the spell is a class spell the creature reacts to, or the object summons or credits the creature
    std::vector<std::string> Violations() const
    {
        std::vector<std::string> found;
        for (QuestUseRule const& rule : rules)
        {
            std::string const what = Check(rule);
            if (!what.empty())
                found.push_back("quest " + std::to_string(rule.questId) + " item " + std::to_string(rule.itemId) +
                                " spell " + std::to_string(rule.spellId) + ": " + what);
        }
        return found;
    }

private:
    std::unordered_map<std::uint32_t, UseSpell> _spells;
    std::vector<UseItem> _items;
    std::unordered_multimap<std::uint32_t, std::size_t> _itemsByQuest;
    std::unordered_set<std::uint64_t> _hits;                                       // creature << 32 | spell
    std::unordered_set<std::uint64_t> _creators;                                   // object << 32 | creature
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> _spellsHitting;  // per creature, ascending
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> _creatorsOf;     // per creature, ascending
    std::unordered_map<std::uint32_t, UseQuest> _quests;

    static std::uint64_t Pair(std::uint32_t a, std::uint32_t b) { return (static_cast<std::uint64_t>(a) << 32) | b; }

    static void SortUnique(std::vector<std::uint32_t>& list)
    {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }

    void Index(QuestUseGather input)
    {
        for (UseSpell& spell : input.spells)
        {
            std::uint32_t const id = spell.spellId;
            _spells[id] = std::move(spell);
        }
        _items = std::move(input.items);
        std::sort(_items.begin(), _items.end(), [](UseItem const& a, UseItem const& b)
                  { return std::tie(a.questId, a.itemId, a.spellId) < std::tie(b.questId, b.itemId, b.spellId); });
        _items.erase(std::unique(_items.begin(), _items.end(), [](UseItem const& a, UseItem const& b)
                                 { return a.questId == b.questId && a.itemId == b.itemId && a.spellId == b.spellId; }),
                     _items.end());
        for (std::size_t i = 0; i < _items.size(); ++i)
            _itemsByQuest.emplace(_items[i].questId, i);
        stats.items = static_cast<std::uint32_t>(_items.size());
        for (SpellHit const& hit : input.hits)
        {
            _hits.insert(Pair(hit.creature, hit.spellId));
            _spellsHitting[hit.creature].push_back(hit.spellId);
        }
        for (CreatorObject const& creator : input.creators)
        {
            _creators.insert(Pair(creator.object, creator.creature));
            _creatorsOf[creator.creature].push_back(creator.object);
        }
        for (auto& [creature, list] : _spellsHitting)
            SortUnique(list);
        for (auto& [creature, list] : _creatorsOf)
            SortUnique(list);
    }

    static std::uint32_t GoalOf(UseQuest const& quest, UseGoalKind kind, std::vector<std::uint32_t> const& entries)
    {
        for (std::size_t i = 0; i < quest.goals.size(); ++i)
            if (quest.goals[i].first == kind &&
                std::binary_search(entries.begin(), entries.end(), quest.goals[i].second))
                return static_cast<std::uint32_t>(i);
        return USE_COMPLETES_QUEST;
    }

    // The objective a use advances when its target is not an objective's: a drop of what it summons or frees (an item
    // counter other than the item used), else the creature it summons (a kill counter), else the quest itself
    static std::uint32_t FollowingGoal(UseQuest const& quest, std::uint32_t itemId)
    {
        for (std::size_t i = 0; i < quest.goals.size(); ++i)
            if (quest.goals[i].first == UseGoalKind::Item && quest.goals[i].second != itemId)
                return static_cast<std::uint32_t>(i);
        for (std::size_t i = 0; i < quest.goals.size(); ++i)
            if (quest.goals[i].first == UseGoalKind::Kill || quest.goals[i].first == UseGoalKind::Object)
                return static_cast<std::uint32_t>(i);
        return USE_COMPLETES_QUEST;
    }

    void AddRules(UseQuest const& quest)
    {
        std::vector<bool> covered(quest.goals.size(), false);
        std::vector<std::uint32_t> itemSpells;
        auto const range = _itemsByQuest.equal_range(quest.id);
        std::vector<std::size_t> order;
        for (auto it = range.first; it != range.second; ++it)
            order.push_back(it->second);
        std::sort(order.begin(), order.end());
        for (std::size_t const i : order)
        {
            UseItem const& item = _items[i];
            itemSpells.push_back(item.spellId);
            UseSpell const* spell = Spell(item.spellId);
            if (!spell)
                continue;
            QuestUseRule rule;
            if (!ItemRule(quest, item, *spell, rule))
            {
                ++stats.unnamed;
                continue;
            }
            if (rule.objective < covered.size())
                covered[rule.objective] = true;
            rules.push_back(std::move(rule));
        }
        for (std::size_t g = 0; g < quest.goals.size(); ++g)
        {
            if (covered[g] || quest.goals[g].first != UseGoalKind::Kill)
                continue;
            std::uint32_t const creature = quest.goals[g].second;
            if (quest.classMask)
                if (auto const it = _spellsHitting.find(creature); it != _spellsHitting.end())
                    for (std::uint32_t const spellId : it->second)
                    {
                        UseSpell const* spell = Spell(spellId);
                        if (!spell || !spell->classFamily ||
                            std::find(itemSpells.begin(), itemSpells.end(), spellId) != itemSpells.end())
                            continue;
                        QuestUseRule rule = BaseRule(quest.id, 0, *spell);
                        rule.kind = spell->requiresDead ? UseKind::UnitDead : UseKind::UnitAlive;
                        rule.mustBeDead = spell->requiresDead;
                        rule.entries = {creature};
                        rule.objective = static_cast<std::uint32_t>(g);
                        rule.classSpell = true;
                        if (!UseFits(rule.kind, *spell))
                            continue;
                        rules.push_back(std::move(rule));
                        covered[g] = true;
                        break;
                    }
            if (covered[g])
                continue;
            if (auto const it = _creatorsOf.find(creature); it != _creatorsOf.end())
            {
                QuestUseRule rule;
                rule.questId = quest.id;
                rule.objective = static_cast<std::uint32_t>(g);
                rule.kind = UseKind::Object;
                rule.entries = it->second;
                rules.push_back(std::move(rule));
                covered[g] = true;
            }
        }
    }

    static QuestUseRule BaseRule(std::uint32_t questId, std::uint32_t itemId, UseSpell const& spell)
    {
        QuestUseRule rule;
        rule.questId = questId;
        rule.itemId = itemId;
        rule.spellId = spell.spellId;
        rule.rangeYards = spell.rangeYards;
        return rule;
    }

    // Named units first (conditions, then the creatures reacting to the spell), then named objects, the spell focus
    // and the objects the item unlocks; none of them: no rule
    bool ItemRule(UseQuest const& quest, UseItem const& item, UseSpell const& spell, QuestUseRule& rule) const
    {
        rule = BaseRule(quest.id, item.itemId, spell);
        std::vector<std::uint32_t> units;
        std::vector<std::uint32_t> objects;
        bool dead = spell.requiresDead;
        for (UseEntry const& named : spell.named)
        {
            (named.isObject ? objects : units).push_back(named.entry);
            dead = dead || (!named.isObject && named.dead);
        }
        for (auto const& [kind, target] : quest.goals)
            if (kind == UseGoalKind::Kill && _hits.count(Pair(target, spell.spellId)))
                units.push_back(target);
        SortUnique(units);
        SortUnique(objects);
        UseKind const unitKind = dead ? UseKind::UnitDead : UseKind::UnitAlive;
        if (!units.empty() && UseFits(unitKind, spell))
            return Named(quest, UseGoalKind::Kill, unitKind, std::move(units), dead, rule);
        if (!objects.empty() && UseFits(UseKind::Object, spell))
            return Named(quest, UseGoalKind::Object, UseKind::Object, std::move(objects), false, rule);
        if (spell.focusId && !spell.focusObjects.empty())
        {
            rule.kind = UseKind::Focus;
            rule.entries = spell.focusObjects;
            rule.standAtFocus = true;
            rule.focusId = spell.focusId;
            rule.rangeYards = spell.focusYards;
            rule.objective = FollowingGoal(quest, item.itemId);
            return true;
        }
        if (!item.lockObjects.empty() && UseFits(UseKind::Object, spell))
        {
            rule.kind = UseKind::Object;
            rule.entries = item.lockObjects;
            rule.objective = GoalOf(quest, UseGoalKind::Object, rule.entries);
            if (rule.objective == USE_COMPLETES_QUEST)
                rule.objective = FollowingGoal(quest, item.itemId);
            return true;
        }
        return false;
    }

    // Entries that are an objective's targets narrow the rule to that objective
    static bool Named(UseQuest const& quest, UseGoalKind goal, UseKind kind, std::vector<std::uint32_t> entries,
                      bool dead, QuestUseRule& rule)
    {
        rule.kind = kind;
        rule.mustBeDead = dead;
        rule.objective = GoalOf(quest, goal, entries);
        if (rule.objective != USE_COMPLETES_QUEST)
            rule.entries = {quest.goals[rule.objective].second};
        else
            rule.entries = std::move(entries);
        return true;
    }

    std::string Check(QuestUseRule const& rule) const
    {
        auto const quest = _quests.find(rule.questId);
        if (quest == _quests.end())
            return "the quest is not indexed";
        std::vector<std::pair<UseGoalKind, std::uint32_t>> const& goals = quest->second.goals;
        if (rule.entries.empty())
            return "no target entry";
        if (!rule.spellId)
        {
            if (rule.itemId || rule.kind != UseKind::Object)
                return "an object rule with an item or a kind other than object";
            if (rule.objective >= goals.size())
                return "an object rule without an objective";
            for (std::uint32_t const object : rule.entries)
                if (!_creators.count(Pair(object, goals[rule.objective].second)))
                    return "object " + std::to_string(object) + " neither summons nor credits the objective";
            return {};
        }
        UseSpell const* spell = Spell(rule.spellId);
        if (!spell)
            return "the spell is not gathered";
        if (!UseFits(rule.kind, *spell))
            return std::string("the spell cannot take a target of kind ") + UseKindName(rule.kind);
        if (!rule.itemId)
        {
            if (!rule.classSpell || !spell->classFamily || !quest->second.classMask)
                return "a spell without an item that is not a class spell of a class quest";
            if (rule.objective >= goals.size() || rule.entries.size() != 1 ||
                rule.entries.front() != goals[rule.objective].second)
                return "a class spell rule whose target is not its objective's creature";
            for (std::uint32_t const creature : rule.entries)
                if (!_hits.count(Pair(creature, rule.spellId)))
                    return "creature " + std::to_string(creature) + " does not react to the spell";
            return {};
        }
        UseItem const* item = nullptr;
        auto const range = _itemsByQuest.equal_range(rule.questId);
        for (auto it = range.first; it != range.second; ++it)
            if (_items[it->second].itemId == rule.itemId && _items[it->second].spellId == rule.spellId)
                item = &_items[it->second];
        if (!item)
            return "the item is not a source item of the quest with this spell";
        for (std::uint32_t const entry : rule.entries)
            if (!NamesFor(*spell, *item, rule.kind, entry))
                return "entry " + std::to_string(entry) + " is not named for the spell";
        return {};
    }

    bool NamesFor(UseSpell const& spell, UseItem const& item, UseKind kind, std::uint32_t entry) const
    {
        switch (kind)
        {
            case UseKind::UnitAlive:
            case UseKind::UnitDead:
                return _hits.count(Pair(entry, spell.spellId)) ||
                       std::any_of(spell.named.begin(), spell.named.end(),
                                   [entry](UseEntry const& named) { return !named.isObject && named.entry == entry; });
            case UseKind::Object:
                return std::binary_search(item.lockObjects.begin(), item.lockObjects.end(), entry) ||
                       std::any_of(spell.named.begin(), spell.named.end(),
                                   [entry](UseEntry const& named) { return named.isObject && named.entry == entry; });
            case UseKind::Focus:
                return std::binary_search(spell.focusObjects.begin(), spell.focusObjects.end(), entry);
            default:
                return false;
        }
    }
};
}  // namespace Planner

#endif
