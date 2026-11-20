/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the words of a bot's lasting facts (living-conversation LC9, BridgeBotFactsRules.h): a phrase for
// every class and talent tree, the craft rank bands, a word for every standing, what each craft is useful for, the
// servants of a class; no word holds a digit or a hidden-skill term.
// Build (Git Bash, module root, inside vcvars64): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot
// tests/bridge-bot-facts-selftest.cpp

#include <cstdio>
#include <string>
#include <string_view>

#include "BridgeBotFactsRules.h"

namespace
{
using namespace PlayerbotBridge::FactsRules;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

bool Clean(std::string_view word)
{
    if (word.empty())
        return false;
    for (char const c : word)
        if (c >= '0' && c <= '9')
            return false;
    for (std::string_view const banned : {"skill", "pvp", "stat", "%"})
        if (word.find(banned) != std::string_view::npos)
            return false;
    return true;
}
}  // namespace

int main()
{
    // every pool class and the death knight, every tree: a clean phrase of its own
    bool phrases = true;
    bool distinct = true;
    bool clean = true;
    for (uint8_t const cls : {1, 2, 3, 4, 5, 6, 7, 8, 9, 11})
        for (uint8_t tree = 0; tree < TREES; ++tree)
        {
            std::string_view const phrase = Phrase(cls, tree);
            phrases &= !phrase.empty() && phrase != FALLBACK_PHRASE;
            clean &= Clean(phrase);
            for (uint8_t other = 0; other < tree; ++other)
                distinct &= Phrase(cls, other) != phrase;
        }
    Check(phrases, "every class and tree has a phrase of its own");
    Check(distinct, "the three trees of a class differ");
    Check(clean, "no phrase holds a digit or a hidden-skill term");
    Check(Phrase(0, 0) == FALLBACK_PHRASE && Phrase(10, 1) == FALLBACK_PHRASE && Phrase(3, 3) == FALLBACK_PHRASE,
          "an unknown class or tree falls back, never empty");

    // rank bands
    Check(RankWord(0) == "apprentice" && RankWord(74) == "apprentice", "0-74 apprentice");
    Check(RankWord(75) == "journeyman" && RankWord(149) == "journeyman", "75-149 journeyman");
    Check(RankWord(150) == "expert" && RankWord(224) == "expert", "150-224 expert");
    Check(RankWord(225) == "artisan" && RankWord(299) == "artisan", "225-299 artisan");
    Check(RankWord(300) == "master" && RankWord(374) == "master", "300-374 master");
    Check(RankWord(375) == "grand master" && RankWord(450) == "grand master" && RankWord(9999) == "grand master",
          "375 and up grand master");
    bool rankClean = true;
    for (std::string_view const word : RANK_WORDS)
        rankClean &= Clean(word);
    Check(rankClean, "rank words are clean");

    // standings
    bool standings = true;
    for (std::size_t rank = 0; rank < STANDINGS; ++rank)
        standings &= Clean(StandingWord(rank));
    Check(standings, "every reputation rank has a word");
    Check(StandingWord(0) == "hated" && StandingWord(4) == "friendly" && StandingWord(7) == "exalted" &&
              StandingWord(99) == "neutral",
          "standing words in the game's order");

    // crafts and servants
    bool crafts = true;
    for (Craft const& craft : CRAFTS)
        crafts &= craft.Skill != 0 && Clean(craft.Name) && Clean(craft.Useful);
    Check(crafts, "every craft has a name and a use on the road");
    bool servants = true;
    for (Servant const& servant : SERVANTS)
        servants &= servant.Spell != 0 && Clean(servant.Name);
    Check(servants, "every servant has a name");

    // LC18e: visits, newest first (the later row at one time), at most MAX_VISITS of the named ones; land for a zone or
    // a dungeon only; ago in seconds
    uint32_t const now = 2000000;
    RecordRules::VisitBook book;
    uint32_t lastId = 0;
    RecordRules::VisitAt elwynn;
    elwynn.Zone = 12;
    elwynn.Continent = true;
    book.Enter(elwynn, now - 3600, lastId);
    RecordRules::VisitAt deadmines;
    deadmines.Map = 36;
    deadmines.Zone = 1581;
    deadmines.Instance = true;
    book.Enter(deadmines, now - 60, lastId);
    RecordRules::VisitAt unnamed;
    unnamed.Map = 13;
    unnamed.Zone = 999;
    book.Enter(unnamed, now - 10, lastId);
    std::vector<RecordRules::Row> rows = book.Rows();
    RecordRules::Row deed;
    deed.EventKind = RecordRules::Kind::Kill;
    deed.Time = now;
    rows.push_back(deed);
    auto const names = [](RecordRules::Row const& row, RecordRules::VisitPlace place)
    {
        if (row.Zone == 999)
            return std::pair<std::string, std::string>();
        if (place == RecordRules::VisitPlace::Dungeon)
            return std::pair<std::string, std::string>("The Deadmines", "Eastern Kingdoms");
        if (place == RecordRules::VisitPlace::Continent)
            return std::pair<std::string, std::string>("Eastern Kingdoms", "Eastern Kingdoms");
        return std::pair<std::string, std::string>("Elwynn Forest", "Eastern Kingdoms");
    };
    std::vector<PlayerbotBridge::BotVisit> const visits = VisitFacts(rows, now, names);
    Check(visits.size() == 3 && visits[0].name == "The Deadmines" && visits[0].kind == "dungeon" &&
              visits[0].land == "Eastern Kingdoms" && visits[0].ago == 60,
          "visits: newest first, a dungeon with its land and age in seconds");
    Check(visits[1].kind == "continent" && visits[1].name == "Eastern Kingdoms" && visits[1].land.empty() &&
              visits[2].kind == "zone" && visits[2].name == "Elwynn Forest" && visits[2].land == "Eastern Kingdoms" &&
              visits[2].ago == 3600,
          "visits: the later row first at one time, a continent without a land, a zone with one");
    Check(VisitFacts({}, now, names).empty(), "visits: none without visit rows");

    RecordRules::VisitBook many;
    for (uint32_t zone = 1; zone <= MAX_VISITS + 6; ++zone)
    {
        RecordRules::VisitAt at;
        at.Zone = zone;
        many.Enter(at, now - zone, lastId);
    }
    std::vector<PlayerbotBridge::BotVisit> const capped = VisitFacts(many.Rows(), now, names);
    Check(capped.size() == 24 && capped.front().ago == 1 && capped.back().ago == 24,
          "visits: at most 24 (the wire's bound), the newest kept");

    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
