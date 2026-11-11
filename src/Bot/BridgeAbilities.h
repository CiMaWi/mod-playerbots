/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEABILITIES_H
#define PLAYERBOTS_BRIDGEABILITIES_H

#include <cstdint>
#include <span>
#include <string_view>

#include "BridgeIntentEffect.h"

// The abilities a conversation partner can ask a bot for, one row each: the one list Execute runs. A new verb is one
// Intent value and one row (with its check and effect) in BridgeAbilities.cpp. Plain data and no game type, so a
// selftest or a module wire can read the table.
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
    Shape,    // FormationShape: the index of the choice plus one
    Enabled,  // SaveMana's yes or no
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

struct ParamRow
{
    char const* Name;
    ParamKind Kind;
    OrderField Field;
    char const* Sentence;
    std::span<char const* const> Choices;
};

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
    // 7. The selftest proves each row applies and gives each listed refusal; the world log has its BridgeAct event.
};

// Null for a verb without a row: the four without an effect (IsUnavailable) and any value outside the enum.
AbilityRow const* FindRow(Intent operation);
AbilityRow const* FindRow(std::string_view name);
std::span<AbilityRow const> AbilityRows();

// Regroup, Pull, AssignRole and DungeonClear: wire verbs with no safe playerbots effect; Execute answers Unavailable.
bool IsUnavailable(Intent operation);

// The refusals every row may give, in check order (Battleground, Travelling, Dead).
std::span<Refusal const> CommonRefusals();

// The word and sentence of a refusal; None's sentence is empty, and an unknown value reads as None.
RefusalText const& RefusalWords(Refusal reason);

bool Takes(AbilityRow const& row, OrderField field);
}  // namespace PlayerbotBridge

#endif
