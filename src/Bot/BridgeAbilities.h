/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEABILITIES_H
#define PLAYERBOTS_BRIDGEABILITIES_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "BridgeBotState.h"
#include "BridgeIntentEffect.h"

// The abilities a conversation partner can ask a bot for, one row each: the acts Execute runs and the topics
// ReadBotStateTopic reads. A new verb is one Intent value and one row (with its check and effect) in
// BridgeAbilities.cpp; a new topic is one BotStateTopic value, one row below and its reader in BridgeBotState.cpp.
// A parameter has a kind and the Order field that carries it, at most one per field (OneParamPerField): a unit
// (Target or TargetName; its Filter says whom a name may mean), a place (PlaceName or Position), a quest (QuestId or
// QuestName; its Quests say among which quests: the bot's log, or the one a group member shared), an item or a spell
// (ItemName, SpellName: RunRow looks the name up among what the bot carries or knows and hands the row ActFacts::ItemId
// or SpellId), a choice (Choice, the index into its Choices; Icon and Shape are the older rows' own), a flag (Enabled)
// or a count (Count). A row taking a unit, quest, item or spell name lists UnknownName. A topic takes at most one
// parameter, of a name kind: its text is ReadBotStateTopic's argument and the topic's reader looks it up. A topic
// marked Lines answers in-world sentences, each item's text one (the record topics' form); every topic beyond the first
// fourteen does (TopicRowsValid). A row marked OldFormOnly keeps an old wire verb working (FindRow, IntentWord) but is
// not in the catalogue: such rows close the table, and AbilityRows lists the others, the catalogue's acts in order
// (mod-ollama-chat pairs them by position). Plain data and no game type, so a selftest or a module wire can read the
// table.
namespace PlayerbotBridge
{
enum class ParamKind : uint8_t
{
    Unit,
    Place,
    Item,
    Spell,
    Quest,
    Choice,  // One of the parameter's Choices
    Flag,    // Yes or no
    Count
};

// The Order field that carries a parameter.
enum class OrderField : uint8_t
{
    Target,
    Icon,       // The index of the choice
    PlaceName,  // A known place's name or "x,y,z,map"
    Position,   // MapId, X, Y, Z
    QuestId,
    Shape,      // FormationShape: the index of the choice plus one
    Enabled,    // SaveMana's yes or no
    ItemName,   // An item the bot carries, by name
    SpellName,  // A spell the bot knows, by name
    Choice,     // The index of the choice, for a row other than Mark and Formation
    Count,      // At least 1
};

// Whom a unit parameter's name may mean, among the units the bot perceives and its group. An enemy is a creature no
// player controls that is not friendly to the bot (hostile or neutral); a friend is a unit friendly to the bot, its
// group's members or the bot itself; a group member is one of the group's members; anyone is all of these.
enum class UnitFilter : uint8_t
{
    Anyone,
    Enemy,
    Friend,
    GroupMember
};

// Who must share a group before the row's own check: nobody, the bot (any group), or the partner (the bot's group).
enum class GroupRule : uint8_t
{
    None,
    BotGroup,
    PartnerInGroup
};

// When an applied order is done, for a caller that waits on it.
enum class Completion : uint8_t
{
    AtOnce,      // Done when applied
    Holds,       // A lasting setting or stance: done when applied, holds until changed or restored
    Arrived,     // A move: done when the bot stops moving
    TargetGone,  // An attack: done when the target is dead or out of reach
    GoalEnds     // A planner goal: done when the goal ends
};

// Which quests a quest parameter's title or id may mean: the bot's log, or the quests of the group member that shared
// one with the bot (the core keeps only who shared: Player::GetDivider).
enum class QuestFrom : uint8_t
{
    Log,
    Shared
};

struct ParamRow
{
    char const* Name;
    ParamKind Kind;
    OrderField Field;
    char const* Sentence;
    std::span<char const* const> Choices;
    UnitFilter Filter = UnitFilter::Anyone;  // A unit's; not in the catalogue
    QuestFrom Quests = QuestFrom::Log;       // A quest's; not in the catalogue
};

// At most one parameter per Order field, so a field never carries two parameters' values.
constexpr bool OneParamPerField(std::span<ParamRow const> params)
{
    for (std::size_t index = 0; index < params.size(); ++index)
        for (std::size_t other = index + 1; other < params.size(); ++other)
            if (params[index].Field == params[other].Field)
                return false;

    return true;
}

// A refusal's stable word (snake_case, for the wire and the world log) and the bot's in-world sentence.
struct RefusalText
{
    Refusal Code;
    char const* Word;
    char const* Sentence;
};

// What Execute read for one admitted order; defined in BridgeAbilities.cpp.
struct ActFacts;
using CheckFn = Refusal (*)(ActFacts const& act);
using EffectFn = Result (*)(ActFacts const& act, Refusal& why);

struct AbilityRow
{
    Intent Operation;
    // 1. The name (the wire's verb and the BridgeAct event's source word) and one sentence for the model.
    char const* Name;
    char const* Sentence;
    // 2. The parameters, in the wire's order.
    std::span<ParamRow const> Params;
    // 3. The plausibility check after the common one (CommonRefusals): the group rule, a unit parameter seen by the
    //    bot (else TargetNotVisible), then Check when there is one. Refusals lists every other reason the row gives.
    GroupRule Group;
    Refusal GroupRefusal;
    CheckFn Check;
    std::span<Refusal const> Refusals;
    // 4. The effect: one playerbots action, setting or planner goal; Lasting is the part Capture and Restore keep.
    EffectFn Effect;
    Part Lasting;
    // 5. When it is done.
    Completion Done;
    // 6. How doing it reads in the bot's state; {name} stands for the parameter of that name.
    char const* StatePhrase;
    // 7. The selftest proves each row applies and gives each listed refusal. Each applied, failed or refused order
    //    writes one ActEventType event to the world log: source word Name, id the Refusal (0 when none), amount the
    //    Result.
    // 8. A verb of the old wire form only (LC17a: attack_own_target, replaced by attack): out of the catalogue and of
    //    AbilityRows, not part of the hash.
    bool OldFormOnly = false;
};

// The world log's event type for an order (CensusMgr.h EventLog).
constexpr char const* ActEventType = "BridgeAct";

// Null for a verb without a row: the three without an effect (IsUnavailable) and any value outside the enum. Both
// find an OldFormOnly row too.
AbilityRow const* FindRow(Intent operation);
AbilityRow const* FindRow(std::string_view name);
// The rows the catalogue lists, in its order: every row but the OldFormOnly ones.
std::span<AbilityRow const> AbilityRows();

// Regroup, AssignRole and DungeonClear: wire verbs with no safe playerbots effect; Execute answers Unavailable.
bool IsUnavailable(Intent operation);

// The number of Intent values: the rows' verbs and the three without one (BridgeAbilities.cpp checks it against the
// table). A new verb is appended to Intent, given its row and counted here; mod-ollama-chat asserts its own count.
constexpr std::size_t IntentCount = static_cast<std::size_t>(Intent::Attack) + 1;

// The ready checks a bot was sent and has not answered, for the ready row: the core keeps no open check
// (WorldSession::HandleRaidReadyCheckOpcode only broadcasts), so the bot's packet handler notes each request it gets
// (another member's), and a finished check or the row's answer ends it. A check is open for ReadyCheckOpenFor after
// its request. Any thread.
constexpr std::chrono::seconds ReadyCheckOpenFor{35};
void NoteReadyCheck(uint64_t bot, std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now());
void EndReadyCheck(uint64_t bot);
bool ReadyCheckOpen(uint64_t bot, std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now());

// The wire's verb of an operation: its row's name, else the word the three without a row have on the wire; empty for
// a value outside the enum. FindIntent is the inverse: a row by name, then the three; none for any other word.
char const* IntentWord(Intent operation);
std::optional<Intent> FindIntent(std::string_view word);

// The refusals every row may give, in check order (Battleground, Travelling, Dead).
std::span<Refusal const> CommonRefusals();

// The word and sentence of a refusal; None's sentence is empty, and an unknown value reads as None.
RefusalText const& RefusalWords(Refusal reason);

bool Takes(AbilityRow const& row, OrderField field);
// The row's parameter carried in the field; null for none.
ParamRow const* ParamOf(AbilityRow const& row, OrderField field);

// A topic is a read: beyond an online bot (else ReadBotStateTopic answers false) it has no check and no refusal,
// changes nothing, is done at once, has no state phrase and writes no world-log event. BridgeBotState.h describes
// its items.
struct TopicRow
{
    BotStateTopic Topic;
    // 1. The name (the wire's topic word) and one sentence for the model.
    char const* Name;
    char const* Sentence;
    // 2. The argument, for a topic that takes one.
    std::span<ParamRow const> Params;
    // The most items the topic returns.
    std::size_t Limit;
    // Its items carry an in-world sentence in text; not in the catalogue.
    bool Lines = false;
};

constexpr bool IsNameKind(ParamKind kind)
{
    return kind == ParamKind::Unit || kind == ParamKind::Place || kind == ParamKind::Item || kind == ParamKind::Spell ||
           kind == ParamKind::Quest;
}

// A topic takes at most one parameter, a name, and one beyond the first fourteen (Known's) answers in lines.
constexpr bool TopicRowsValid(std::span<TopicRow const> rows)
{
    for (TopicRow const& row : rows)
        if (row.Params.size() > 1 || (row.Params.size() == 1 && !IsNameKind(row.Params[0].Kind)) ||
            (row.Topic > BotStateTopic::Known && !row.Lines))
            return false;

    return true;
}

inline constexpr ParamRow RouteParams[] = {
    {"place", ParamKind::Place, OrderField::PlaceName, "A known place's name, or x,y,z,map.", {}}};

// The topics in the wire's order (BotStateTopic's).
inline constexpr TopicRow TopicTable[] = {
    {.Topic = BotStateTopic::Inventory,
     .Name = "inventory",
     .Sentence = "The items you wear and carry, equipped ones first.",
     .Params = {},
     .Limit = BotStateTopicMaxItems},
    {.Topic = BotStateTopic::Quests,
     .Name = "quests",
     .Sentence = "The quests in your log and how far along each one is.",
     .Params = {},
     .Limit = BotStateTopicMaxItems},
    {.Topic = BotStateTopic::Cooldowns,
     .Name = "cooldowns",
     .Sentence = "Your spells still cooling down, longest first.",
     .Params = {},
     .Limit = BotStateTopicMaxItems},
    {.Topic = BotStateTopic::Strategies,
     .Name = "strategies",
     .Sentence = "The habits you follow in and out of a fight.",
     .Params = {},
     .Limit = BotStateTopicMaxItems},
    {.Topic = BotStateTopic::Spells,
     .Name = "spells",
     .Sentence = "The class spells you know, each at its highest rank.",
     .Params = {},
     .Limit = BotStateTopicMaxItems},
    {.Topic = BotStateTopic::Plan,
     .Name = "plan",
     .Sentence = "What you are doing now, why, and your next stops.",
     .Params = {},
     .Limit = 8},
    {.Topic = BotStateTopic::Options,
     .Name = "options",
     .Sentence = "The choices you weighed for what to do next.",
     .Params = {},
     .Limit = 5},
    {.Topic = BotStateTopic::Route,
     .Name = "route",
     .Sentence = "How you would travel to a place: the whole trip, then each leg.",
     .Params = RouteParams,
     .Limit = 12},
    {.Topic = BotStateTopic::QuestPlan,
     .Name = "quest_plan",
     .Sentence = "The quests you plan to take, do and turn in, in walking order.",
     .Params = {},
     .Limit = 12},
    {.Topic = BotStateTopic::Bags,
     .Name = "bags",
     .Sentence = "Your bags, what you keep in the bank and the mail that waits for you.",
     .Params = {},
     .Limit = 12},
    {.Topic = BotStateTopic::Places,
     .Name = "places",
     .Sentence = "Where your hearthstone is bound and the flight points you know.",
     .Params = {},
     .Limit = 24},
    {.Topic = BotStateTopic::History,
     .Name = "history",
     .Sentence = "What you did that mattered, in order, and the lands, dungeons and foes you know.",
     .Params = {},
     .Limit = 24,
     .Lines = true},
    {.Topic = BotStateTopic::Lately,
     .Name = "lately",
     .Sentence = "What you did of note lately, newest first.",
     .Params = {},
     .Limit = 12,
     .Lines = true},
    {.Topic = BotStateTopic::Known,
     .Name = "known",
     .Sentence = "The others you have shared deeds with, and what you shared.",
     .Params = {},
     .Limit = 12,
     .Lines = true}};

constexpr bool TopicTableInOrder()
{
    for (std::size_t index = 0; index < std::size(TopicTable); ++index)
        if (static_cast<std::size_t>(TopicTable[index].Topic) != index)
            return false;

    return static_cast<std::size_t>(BotStateTopic::Known) + 1 == std::size(TopicTable);
}
static_assert(TopicTableInOrder(), "one row per BotStateTopic, in the enum's order");
static_assert(TopicRowsValid(TopicTable), "a topic takes at most one parameter, a name; a new one answers lines");

// Null for a value outside the enum.
constexpr TopicRow const* FindTopic(BotStateTopic topic)
{
    std::size_t const index = static_cast<std::size_t>(topic);
    return index < std::size(TopicTable) ? &TopicTable[index] : nullptr;
}

constexpr TopicRow const* FindTopic(std::string_view name)
{
    for (TopicRow const& row : TopicTable)
        if (name == row.Name)
            return &row;

    return nullptr;
}

constexpr std::span<TopicRow const> TopicRows() { return TopicTable; }

// The catalogue: the table as plain values, for the module's wire and the bridge.
enum class AbilityKind : uint8_t
{
    Act,
    Topic
};

struct ParamData
{
    std::string Name;
    ParamKind Kind = ParamKind::Unit;
    std::string Sentence;
    std::vector<std::string> Choices;
};

struct RefusalData
{
    Refusal Code = Refusal::None;
    std::string Word;
    std::string Sentence;
};

// One row, its parts in the order of AbilityRow; a topic leaves 3 to 7 empty (Lasting None, Done AtOnce).
struct AbilityData
{
    AbilityKind Kind = AbilityKind::Act;
    std::string Name;
    std::string Sentence;
    std::vector<ParamData> Params;
    std::vector<RefusalData> Refusals;  // An act's: the common ones, then its own, in check order
    Part Lasting = Part::None;
    Completion Done = Completion::AtOnce;
    std::string StatePhrase;
    std::string Event;      // ActEventType for an act (source word: Name)
    std::size_t Limit = 0;  // A topic's most items
};

// The acts in table order, then the topics in table order. Allocates: call it on a request, not per tick.
std::vector<AbilityData> Catalogue();

// The canonical text of a catalogue: the format line (CatalogueVersion), the steps line (MaxSteps, the most steps one
// order takes), then per row one line per part in AbilityData's order, the fields tab-separated and enums as words.
// CatalogueHash is its FNV-1a 64 (TextHash). Bump CatalogueVersion when the layout changes.
constexpr unsigned CatalogueVersion = 1;
std::string CatalogueText(std::vector<AbilityData> const& catalogue);
uint64_t CatalogueHash(std::vector<AbilityData> const& catalogue);
uint64_t TextHash(std::string_view text);

// The snake_case words of the catalogue's enums; empty for a value outside the enum.
char const* KindWord(AbilityKind kind);
char const* KindWord(ParamKind kind);
char const* PartWord(Part part);
char const* CompletionWord(Completion done);
}  // namespace PlayerbotBridge

#endif
