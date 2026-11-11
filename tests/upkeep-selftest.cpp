/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the upkeep rules (src/Ai/World/Rpg/UpkeepRules.h): the predicate over a table of bots, kinds
// and shares, the handout share by guid (stable, monotone, near its percent), the kind table and the sick gate of the
// RPG status pick (a table of status x sick).
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

#include "UpkeepRules.h"

namespace
{
int passed = 0;
int failed = 0;

void Check(bool condition, std::string const& what)
{
    if (condition)
    {
        ++passed;
        return;
    }

    ++failed;
    std::cout << "FAIL: " << what << "\n";
}

static_assert(Upkeep::KIND_RULES.size() == static_cast<size_t>(UpkeepKind::Talent) + 1, "a rule per kind");

void CheckPredicate()
{
    using Upkeep::Granted;
    struct Case
    {
        bool keepsPossessions;
        bool honest;
        uint32_t guid;
        uint32_t share;
        bool expected;
        char const* what;
    };
    Case const cases[] = {
        {false, false, 7, 0, true, "a bot that does not keep its possessions gets a handout of a kind not honest"},
        {false, true, 7, 0, true, "a bot that does not keep its possessions gets a handout of an honest kind"},
        {true, false, 7, 0, true, "a persistent bot gets a handout of a kind not honest yet"},
        {true, true, 7, 0, false, "a persistent bot outside the share gets no handout of an honest kind"},
        {true, true, 7, 100, true, "share 100 keeps every persistent bot on every handout"},
        {true, false, 7, 100, true, "share 100, a kind not honest"},
        {false, true, 7, 100, true, "share 100, a bot that does not keep its possessions"},
    };
    for (Case const& c : cases)
        Check(Granted(c.keepsPossessions, c.honest, c.guid, c.share) == c.expected, c.what);

    // A guid inside a share of 50 keeps the handout, one outside does not
    uint32_t inside = 0;
    uint32_t outside = 0;
    for (uint32_t guid = 1; guid < 1000 && (!inside || !outside); ++guid)
    {
        if (Upkeep::ShareSlot(guid) < 50)
            inside = inside ? inside : guid;
        else
            outside = outside ? outside : guid;
    }
    Check(inside && Granted(true, true, inside, 50), "a persistent bot inside the share keeps an honest handout");
    Check(outside && !Granted(true, true, outside, 50), "a persistent bot outside the share loses it");
}

void CheckShare()
{
    bool stable = true;
    bool monotone = true;
    bool inRange = true;
    for (uint32_t guid = 0; guid < 10000; ++guid)
    {
        uint32_t const slot = Upkeep::ShareSlot(guid);
        stable = stable && slot == Upkeep::ShareSlot(guid);
        inRange = inRange && slot < Upkeep::SHARE_PERCENT_MAX;
        for (uint32_t share = 0; share < Upkeep::SHARE_PERCENT_MAX; ++share)
            if (Upkeep::InShare(guid, share) && !Upkeep::InShare(guid, share + 1))
                monotone = false;
    }
    Check(stable, "the same guid always lands in the same slot");
    Check(inRange, "every slot is 0..99");
    Check(monotone, "a guid in the share at p% stays in at any larger p");
    Check(!Upkeep::InShare(12345, 0), "share 0 holds nobody");
    Check(Upkeep::InShare(12345, 100), "share 100 holds everybody");

    // The formula as conf.dist and server-tools.md give it for SQL, with 64-bit arithmetic
    bool sqlMatches = true;
    for (uint64_t guid : {0ull, 1ull, 2ull, 1000ull, 65535ull, 123456ull, 4000000ull, 4294967295ull})
    {
        uint64_t const sql = (((guid * 2654435761ull) & 0xFFFFFFFFull) >> 16) % 100;
        sqlMatches = sqlMatches && sql == Upkeep::ShareSlot(static_cast<uint32_t>(guid));
    }
    Check(sqlMatches, "the SQL formula gives the same slot");

    // Over 10000 consecutive guids (bot guids are dense) the share lands within 1.5 points of p
    for (uint32_t share : {1u, 5u, 10u, 25u, 50u, 90u})
    {
        uint32_t count = 0;
        for (uint32_t guid = 1; guid <= 10000; ++guid)
            count += Upkeep::InShare(guid, share) ? 1 : 0;
        int32_t const deviation = static_cast<int32_t>(count) - static_cast<int32_t>(share * 100);
        Check(std::abs(deviation) <= 150,
              "share " + std::to_string(share) + "% holds " + std::to_string(count) + " of 10000 guids");
    }
}

void CheckKinds()
{
    std::set<std::string_view> words;
    for (Upkeep::KindRule const& rule : Upkeep::KIND_RULES)
        words.insert(rule.word);
    Check(words.size() == Upkeep::KIND_COUNT, "every kind has its own honesty word");
    Check(!words.count("Item") && !words.count("Spell") && !words.count("Skill"),
          "no kind reuses the gear row Item or the old mixed rows Spell and Skill");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Food)) == "Food", "Food keeps its old row word");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Repair)) == "Repair", "Repair keeps its old row word");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Resurrect)) == "Resurrect", "Resurrect keeps its word");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Gold)) == "Gold", "Gold keeps its old row word");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Talent)) == "Talent", "Talent keeps its old row word");
    Check(std::string_view(Upkeep::KindWord(static_cast<UpkeepKind>(Upkeep::KIND_COUNT))) == "Other",
          "a kind out of range is Other");

    // Gold (honest travel), Repair and Resurrect (WU5), the five consumable kinds (WU7) are honest; every other kind
    // is still handed out
    size_t honest = 0;
    for (Upkeep::KindRule const& rule : Upkeep::KIND_RULES)
        honest += rule.honest ? 1 : 0;
    Check(honest == 8 && Upkeep::IsHonest(UpkeepKind::Gold) && Upkeep::IsHonest(UpkeepKind::Repair) &&
              Upkeep::IsHonest(UpkeepKind::Resurrect),
          "Gold, Repair, Resurrect and the five consumable kinds are honest");
    Check(!Upkeep::IsHonest(UpkeepKind::TrainerSpell) && !Upkeep::IsHonest(UpkeepKind::Pet),
          "trainer spells and pets are still handed out");
    Check(!Upkeep::IsHonest(static_cast<UpkeepKind>(Upkeep::KIND_COUNT)), "a kind out of range is not honest");

    // The predicate over the flipped kinds' table entries
    using Upkeep::Granted;
    for (UpkeepKind const kind : {UpkeepKind::Repair, UpkeepKind::Resurrect, UpkeepKind::Food, UpkeepKind::Ammo,
                                  UpkeepKind::Reagent, UpkeepKind::Poison, UpkeepKind::Consumable})
    {
        std::string const word = Upkeep::KindWord(kind);
        bool const honestKind = Upkeep::IsHonest(kind);
        Check(!Granted(true, honestKind, 7, 0), word + ": a persistent bot outside the share pays");
        Check(Granted(true, honestKind, 7, 100), word + ": share 100 keeps the handout");
        Check(Granted(false, honestKind, 7, 0), word + ": a bot that does not keep its possessions keeps it");
    }
}

// NewRpgStatus as PlayerbotAIConfig.h numbers it (NewRpgBaseAction.cpp asserts the fighting ones)
void CheckSickGate()
{
    struct Row
    {
        uint32_t status;
        char const* word;
        bool fights;
    };
    Row const rows[] = {
        {0, "idle", false},       {1, "go_grind", true},  {2, "go_camp", false}, {3, "wander_random", true},
        {4, "wander_npc", false}, {5, "do_quest", true},  {6, "flight", false},  {7, "rest", false},
        {8, "outdoor_pvp", true}, {9, "do_gather", true}, {10, "travel", false}, {11, "errands", false},
    };
    std::cout << "Sick gate (status: healthy / sick):\n";
    for (Row const& row : rows)
    {
        bool const healthy = Upkeep::StatusAllowed(row.status, false);
        bool const sick = Upkeep::StatusAllowed(row.status, true);
        std::cout << "  " << row.word << ": " << (healthy ? "allowed" : "refused") << " / "
                  << (sick ? "allowed" : "refused") << "\n";
        Check(Upkeep::StatusFights(row.status) == row.fights,
              std::string(row.word) + (row.fights ? " fights" : " does not fight"));
        Check(healthy, std::string(row.word) + " is open to a healthy bot");
        Check(sick == !row.fights,
              std::string(row.word) + (row.fights ? " is closed" : " stays open") + " to a sick bot");
    }
    Check(Upkeep::StatusAllowed(7, true) && Upkeep::StatusAllowed(11, true),
          "a sick bot can always rest or run an errand");
    Check(!Upkeep::StatusFights(32) && !Upkeep::StatusFights(1000), "a status out of range never fights");
}
}  // namespace

int main()
{
    CheckPredicate();
    CheckShare();
    CheckKinds();
    CheckSickGate();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
