/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRIDGEBOTFACTSRULES_H
#define PLAYERBOTS_BRIDGEBOTFACTSRULES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "BridgeBotFacts.h"
#include "BridgeTexts.h"
#include "RecordRules.h"

// LC9: the words of a bot's lasting facts (P §3.3), as plain data without a game type: how it fights as a phrase per
// class and talent tree, the rank word of a craft, what a craft is useful for on the road, the servants a class can
// call, the standing with a faction. Header-only so tests/bridge-bot-facts-selftest.cpp checks what the server runs.
// No word holds a digit, an id or a hidden skill (C4).
namespace PlayerbotBridge::FactsRules
{
// Class ids are the game's (1 warrior .. 11 druid; 6 death knight); tabs are the talent trees in the order of the
// class's talent tab pages (a class without a row, id 0 and 10, falls back)
constexpr std::size_t CLASS_SLOTS = 12;
constexpr std::size_t TREES = 3;

using BridgeTexts::Facts::FALLBACK_PHRASE;
using BridgeTexts::Facts::PHRASES;
using BridgeTexts::Facts::TreePhrases;
static_assert(PHRASES.size() == CLASS_SLOTS && TREES == 3, "BridgeTexts.h holds a row per class and three trees");

constexpr std::string_view Phrase(uint8_t classId, uint8_t tree)
{
    if (tree >= TREES)
        return FALLBACK_PHRASE;

    if (classId >= CLASS_SLOTS || PHRASES[classId].Phrase[tree].empty())
        return FALLBACK_PHRASE;

    return PHRASES[classId].Phrase[tree];
}

// A craft rank in a word: apprentice below 75, journeyman below 150, expert below 225, artisan below 300, master below
// 375, grand master from there
constexpr uint32_t BAND_WIDTH = 75;
constexpr std::size_t RANK_BANDS = 6;
using BridgeTexts::Facts::RANK_WORDS;
static_assert(RANK_WORDS.size() == RANK_BANDS);

constexpr std::string_view RankWord(uint32_t skillValue)
{
    std::size_t const band = skillValue / BAND_WIDTH;
    return RANK_WORDS[band < RANK_BANDS ? band : RANK_BANDS - 1];
}

using BridgeTexts::Facts::Craft;
using BridgeTexts::Facts::CRAFTS;

using BridgeTexts::Facts::Servant;
using BridgeTexts::Facts::SERVANTS;

// The standing with a faction (the game's ReputationRank order, hated to exalted)
constexpr std::size_t STANDINGS = 8;
using BridgeTexts::Facts::STANDING_WORDS;
static_assert(STANDING_WORDS.size() == STANDINGS);
constexpr std::size_t NEUTRAL_STANDING = 3;
constexpr std::size_t MAX_STANDINGS = 6;  // [E] factions in a read, the most marked first

constexpr std::string_view StandingWord(std::size_t rank)
{
    return STANDING_WORDS[rank < STANDINGS ? rank : NEUTRAL_STANDING];
}

// LC18e: the places in a bot's facts, from its Visit rows: newest first (the later row first at one time), at most
// MAX_VISITS of those the game names. placeOf(row, place) gives {name, land}: the place's name (empty: left out) and
// the continent of a zone or dungeon (a continent has none); ago counts the seconds since the row's time.
constexpr std::size_t MAX_VISITS = 24;

template <typename PlaceOf>
std::vector<BotVisit> VisitFacts(std::vector<RecordRules::Row> rows, std::uint32_t now, PlaceOf const& placeOf)
{
    std::sort(rows.begin(), rows.end(), [](RecordRules::Row const& a, RecordRules::Row const& b)
              { return a.Time != b.Time ? a.Time > b.Time : a.EventId > b.EventId; });
    std::vector<BotVisit> visits;
    for (RecordRules::Row const& row : rows)
    {
        if (visits.size() >= MAX_VISITS)
            break;
        std::optional<RecordRules::VisitPlace> const place = RecordRules::VisitPlaceOf(row);
        if (!place)
            continue;
        auto [name, land] = placeOf(row, *place);
        if (name.empty())
            continue;
        if (*place == RecordRules::VisitPlace::Continent)
            land.clear();
        visits.push_back(
            {std::move(name), RecordRules::VisitWord(*place), std::move(land), now > row.Time ? now - row.Time : 0});
    }
    return visits;
}
}  // namespace PlayerbotBridge::FactsRules

#endif
