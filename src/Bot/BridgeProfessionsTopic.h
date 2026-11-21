/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEPROFESSIONSTOPIC_H
#define PLAYERBOTS_BRIDGEPROFESSIONSTOPIC_H

// WR18: the `professions` topic (BridgeBotState.h), Player-free. BridgeBotState.cpp reads the bot's professions and its
// kept skill path on the world thread (where the `needs` topic reads the path too) into the plain input below, and
// this header words it: which professions the bot has and how far it is, what it works towards, what it lacks. Words
// only: a rank word, no skill number, no copper. What it can make is the recipes topic's. Only BridgeBotState.cpp and
// the selftest include this header. tests/bridge-bot-topics-selftest.cpp checks it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "BridgeBotState.h"
#include "BridgeTexts.h"

namespace PlayerbotBridge::Topics
{
// The row kinds (BotStateItem::code) of the professions topic
enum class ProfessionRow : std::uint8_t
{
    Provided,    // a bot the rules do not govern: it follows no path of its own (the only entry)
    Nothing,     // no profession
    Profession,  // name = the profession, text = its rank and what it works towards
    Lacks        // what the path still needs before its next stretch
};

inline char const* RowWord(ProfessionRow row)
{
    namespace L = BridgeTexts::Lookups;
    switch (row)
    {
        case ProfessionRow::Provided:
            return L::ROW_PROF_PROVIDED;
        case ProfessionRow::Nothing:
            return L::ROW_PROF_NOTHING;
        case ProfessionRow::Profession:
            return L::ROW_PROF_PROFESSION;
        case ProfessionRow::Lacks:
            return L::ROW_PROF_LACKS;
    }
    return "";
}

// What a profession's chosen stretch reaches for
enum class ProfessionGoal : std::uint8_t
{
    None,       // the path chose nothing for this profession
    Recipe,     // a recipe that becomes makeable
    Node,       // a kind of node it can gather
    Skinnable,  // creatures it can skin
    Rank        // a higher rank from a trainer
};

// What the chosen stretch still needs
enum class ProfessionLack : std::uint8_t
{
    Recipe,
    Rank,
    Tool,
    Materials
};

// The most rows: the professions are cut so that the lacks row stays (the topic row's limit)
constexpr std::size_t PROFESSIONS_SHOWN = 8;

struct ProfessionLine
{
    std::string name;
    std::string rank;  // a rank word
    ProfessionGoal goal{ProfessionGoal::None};
    bool making{false};  // its crafts for skill are castable from the bags now
};

struct ProfessionsInput
{
    bool governed{false};  // a persistent bot (BotKind::KeepsPossessions); else it follows no path of its own
    std::vector<ProfessionLine> lines;
    std::vector<ProfessionLack> lacks;  // from the chosen stretch; each kind once
};

inline char const* GoalWord(ProfessionGoal goal)
{
    namespace L = BridgeTexts::Lookups;
    switch (goal)
    {
        case ProfessionGoal::Recipe:
            return L::PROF_GOAL_RECIPE;
        case ProfessionGoal::Node:
            return L::PROF_GOAL_NODE;
        case ProfessionGoal::Skinnable:
            return L::PROF_GOAL_SKINNABLE;
        case ProfessionGoal::Rank:
            return L::PROF_GOAL_RANK;
        case ProfessionGoal::None:
            break;
    }
    return "";
}

inline char const* LackWord(ProfessionLack lack)
{
    namespace L = BridgeTexts::Lookups;
    switch (lack)
    {
        case ProfessionLack::Recipe:
            return L::PROF_LACK_RECIPE;
        case ProfessionLack::Rank:
            return L::PROF_LACK_RANK;
        case ProfessionLack::Tool:
            return L::PROF_LACK_TOOL;
        case ProfessionLack::Materials:
            return L::PROF_LACK_MATERIALS;
    }
    return "";
}

inline BotStateItem ProfessionLineItem(ProfessionRow row, std::string name, std::string text)
{
    return {std::move(name), 0, 0, static_cast<std::uint8_t>(row), std::move(text), RowWord(row)};
}

// One row per profession (cut to leave room for the lacks row), then one lacks row when the path lacks anything; a bot
// the rules do not govern, or one with no profession: one row saying so
inline void ProfessionsItems(ProfessionsInput const& in, std::vector<BotStateItem>& items)
{
    namespace L = BridgeTexts::Lookups;
    items.clear();
    if (!in.governed)
    {
        items.push_back(ProfessionLineItem(ProfessionRow::Provided, L::ROW_PROF_PROVIDED, L::PROF_PROVIDED));
        return;
    }
    if (in.lines.empty())
    {
        items.push_back(ProfessionLineItem(ProfessionRow::Nothing, L::ROW_PROF_NOTHING, L::PROF_NOTHING));
        return;
    }
    std::size_t const room = PROFESSIONS_SHOWN - (in.lacks.empty() ? 0 : 1);
    for (ProfessionLine const& line : in.lines)
    {
        if (items.size() >= room)
            break;

        std::string text = line.name + L::LOOKUP_SEP + line.rank;
        if (line.goal != ProfessionGoal::None)
            text.append(line.making ? L::PROF_MAKING : L::PROF_TOWARDS).append(GoalWord(line.goal));
        else
            text.append(L::PROF_NO_GOAL);
        text.append(L::LOOKUP_END);
        items.push_back(ProfessionLineItem(ProfessionRow::Profession, line.name, std::move(text)));
    }
    if (!in.lacks.empty())
    {
        std::string text = L::PROF_LACKS_LEAD;
        for (std::size_t at = 0; at < in.lacks.size(); ++at)
            text.append(at ? L::LOOKUP_SEP : "").append(LackWord(in.lacks[at]));
        text.append(L::LOOKUP_END);
        items.push_back(ProfessionLineItem(ProfessionRow::Lacks, L::ROW_PROF_LACKS, std::move(text)));
    }
}
}  // namespace PlayerbotBridge::Topics

#endif
