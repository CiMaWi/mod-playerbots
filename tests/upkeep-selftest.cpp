/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the upkeep rules (src/Ai/World/Rpg/UpkeepRules.h): the predicate over a table of bots, kinds
// and shares, the handout share by guid (stable, monotone, near its percent), the kind table, the sick gate (a sick bot
// rests: the status pick, the status update and the rest timeout over a table of statuses) and the errand start.
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

    // Gold (honest travel), Repair and Resurrect (WU5), the five consumable kinds (WU7), Attunement, Key, Reputation,
    // Glyph and Talent (WU18), trainer spells, weapon skills, riding, mounts and special spells (WU15b) are honest;
    // every other kind is still handed out
    size_t honest = 0;
    for (Upkeep::KindRule const& rule : Upkeep::KIND_RULES)
        honest += rule.honest ? 1 : 0;
    Check(honest == 18 && Upkeep::IsHonest(UpkeepKind::Gold) && Upkeep::IsHonest(UpkeepKind::Repair) &&
              Upkeep::IsHonest(UpkeepKind::Resurrect),
          "Gold, Repair, Resurrect and the five consumable kinds are honest");
    Check(Upkeep::IsHonest(UpkeepKind::Attunement) && Upkeep::IsHonest(UpkeepKind::Key) &&
              Upkeep::IsHonest(UpkeepKind::Reputation) && Upkeep::IsHonest(UpkeepKind::Glyph) &&
              Upkeep::IsHonest(UpkeepKind::Talent),
          "attunements, keys, reputation, glyphs and the talent reset are honest");
    Check(Upkeep::IsHonest(UpkeepKind::TrainerSpell) && Upkeep::IsHonest(UpkeepKind::WeaponSkill) &&
              Upkeep::IsHonest(UpkeepKind::Riding) && Upkeep::IsHonest(UpkeepKind::Mount) &&
              Upkeep::IsHonest(UpkeepKind::SpecialSpell),
          "trainer spells, weapon skills, riding, mounts and special spells are honest");
    Check(!Upkeep::IsHonest(UpkeepKind::Pet) && !Upkeep::IsHonest(UpkeepKind::ClassQuest),
          "pets and class-quest spells are still handed out");
    Check(!Upkeep::IsHonest(static_cast<UpkeepKind>(Upkeep::KIND_COUNT)), "a kind out of range is not honest");

    // The predicate over the flipped kinds' table entries
    using Upkeep::Granted;
    for (UpkeepKind const kind :
         {UpkeepKind::Repair, UpkeepKind::Resurrect, UpkeepKind::Food, UpkeepKind::Ammo, UpkeepKind::Reagent,
          UpkeepKind::Poison, UpkeepKind::Consumable, UpkeepKind::Attunement, UpkeepKind::Key, UpkeepKind::Reputation,
          UpkeepKind::Glyph, UpkeepKind::Talent, UpkeepKind::TrainerSpell, UpkeepKind::WeaponSkill, UpkeepKind::Riding,
          UpkeepKind::Mount, UpkeepKind::SpecialSpell})
    {
        std::string const word = Upkeep::KindWord(kind);
        bool const honestKind = Upkeep::IsHonest(kind);
        Check(!Granted(true, honestKind, 7, 0), word + ": a persistent bot outside the share pays");
        Check(Granted(true, honestKind, 7, 100), word + ": share 100 keeps the handout");
        Check(Granted(false, honestKind, 7, 0), word + ": a bot that does not keep its possessions keeps it");
    }
}

// NewRpgStatus as PlayerbotAIConfig.h numbers it (NewRpgBaseAction.cpp asserts the ones UpkeepRules.h names).
// fights: the activity fights; sickOpen: a sick bot may enter or stay in it (only idle and rest); interrupts: an
// errand may end it
// (questing only for a due need)
struct StatusRow
{
    uint32_t status;
    char const* word;
    bool fights;
    bool sickOpen;
    bool interrupts;
};

constexpr StatusRow STATUS_ROWS[] = {
    {0, "idle", false, true, true},         {1, "go_grind", true, false, true},
    {2, "go_camp", false, false, true},     {3, "wander_random", true, false, true},
    {4, "wander_npc", false, false, true},  {5, "do_quest", true, false, false},
    {6, "flight", false, false, false},     {7, "rest", false, true, true},
    {8, "outdoor_pvp", true, false, false}, {9, "do_gather", true, false, true},
    {10, "travel", false, false, false},    {11, "errands", false, false, false},
};

void CheckSickGate()
{
    std::cout << "Sick gate (status: healthy / sick):\n";
    for (StatusRow const& row : STATUS_ROWS)
    {
        bool const healthy = Upkeep::StatusAllowed(row.status, false);
        bool const sick = Upkeep::StatusAllowed(row.status, true);
        std::cout << "  " << row.word << ": " << (healthy ? "allowed" : "refused") << " / "
                  << (sick ? "allowed" : "refused") << "\n";
        Check(Upkeep::StatusFights(row.status) == row.fights,
              std::string(row.word) + (row.fights ? " fights" : " does not fight"));
        Check(healthy, std::string(row.word) + " is open to a healthy bot");
        Check(sick == row.sickOpen,
              std::string(row.word) + (row.sickOpen ? " stays open" : " is closed") + " to a sick bot");
    }
    Check(Upkeep::StatusAllowed(7, true), "a sick bot can always rest");
    Check(!Upkeep::StatusAllowed(11, true), "a sick bot leaves its errand (WU8b: the death loop at the healer)");
    Check(!Upkeep::StatusFights(32) && !Upkeep::StatusFights(1000), "a status out of range never fights");
    Check(!Upkeep::StatusAllowed(32, true) && Upkeep::StatusAllowed(32, false),
          "a status out of range is closed to a sick bot only");
}

// A sick bot rests (WU8b-fix 2): the status pick, the end of a status in the status update and the rest timeout
void CheckSickRests()
{
    std::cout << "Sick bot rests (status: pick healthy / needs rest / sick; ended while sick on the ground / in the "
                 "air):\n";
    for (StatusRow const& row : STATUS_ROWS)
    {
        bool const healthy = Upkeep::StatusPickable(row.status, false, false);
        bool const needsRest = Upkeep::StatusPickable(row.status, false, true);
        bool const sick = Upkeep::StatusPickable(row.status, true, false);
        bool const ground = Upkeep::SickEndsStatus(row.status, true, false);
        bool const air = Upkeep::SickEndsStatus(row.status, true, true);
        std::cout << "  " << row.word << ": " << healthy << " / " << needsRest << " / " << sick << "; " << ground
                  << " / " << air << "\n";
        std::string const word(row.word);
        Check(healthy, word + " may be picked by a healthy bot");
        Check(needsRest == !row.fights, word + (row.fights ? " is not" : " is") + " picked by a bot that needs rest");
        Check(sick == row.sickOpen && !(row.sickOpen && row.fights), word + " picked by a sick bot only if open");
        Check(Upkeep::StatusPickable(row.status, true, true) == row.sickOpen, word + ": sick and needing rest");
        Check(ground == !row.sickOpen, word + (row.sickOpen ? " stays" : " ends") + " while the bot is sick");
        Check(!air, word + ": a flight in the air lands first");
        Check(!Upkeep::SickEndsStatus(row.status, false, false), word + " never ends for a healthy bot");
    }
    // Rest: its duration ends it, a sick bot's only once the sickness has ended
    Check(Upkeep::RestEnds(true, false) && !Upkeep::RestEnds(false, false), "rest ends after its duration");
    Check(!Upkeep::RestEnds(true, true) && !Upkeep::RestEnds(false, true), "a sick bot's rest does not time out");
    // The WU8b deaths while sick by the bot's status (review of WU8b-fix): the errand (914), go camp and wander npc
    // picks after a 13-35 s rest (168: go camp, killed, healer, go camp), an inn trip kept through death (636, 904)
    Check(Upkeep::SickEndsStatus(11, true, false), "914: the errand ends at the healer");
    Check(!Upkeep::RestEnds(true, true) && !Upkeep::StatusPickable(2, true, false) &&
              !Upkeep::StatusPickable(4, true, false),
          "168: the rest outlasts its 35 s and the pick offers no go camp or wander npc");
    Check(Upkeep::SickEndsStatus(10, true, false), "636: the inn trip kept from before the death ends");
    Check(Upkeep::SickEndsStatus(6, true, false) && !Upkeep::SickEndsStatus(6, true, true),
          "a flight not yet in the air ends, one in the air lands");
    // The sickness over: rest times out and the pick offers every status again, the errand start too
    bool everyStatus = true;
    for (StatusRow const& row : STATUS_ROWS)
        everyStatus = everyStatus && Upkeep::StatusPickable(row.status, false, false);
    Check(Upkeep::RestEnds(true, false) && everyStatus && Upkeep::ErrandMayStart(7, true, false),
          "the sickness over, the normal pick and the errand start resume");
}

// The errand start (WT3b's interruptible statuses, WU8b's due need while questing and the sick gate)
void CheckErrandStart()
{
    std::cout << "Errand start (status: bags / due need / due need while sick):\n";
    for (StatusRow const& row : STATUS_ROWS)
    {
        bool const bags = Upkeep::ErrandMayStart(row.status, false, false);
        bool const need = Upkeep::ErrandMayStart(row.status, true, false);
        bool const sick = Upkeep::ErrandMayStart(row.status, true, true);
        std::cout << "  " << row.word << ": " << bags << " / " << need << " / " << sick << "\n";
        Check(bags == row.interrupts,
              std::string(row.word) + (row.interrupts ? " may" : " may not") + " be ended by a bag errand");
        Check(need == (row.interrupts || row.status == 5),
              std::string(row.word) + (need ? " may" : " may not") + " be ended by a due need");
        Check(!sick, std::string(row.word) + ": a sick bot starts no errand");
        Check(!Upkeep::ErrandMayStart(row.status, false, true), std::string(row.word) + ": nor for full bags");
    }
    // The WU8b cases: the healer revive in rest, the hunter without arrows on a quest, a bot on a planned trip
    Check(!Upkeep::ErrandMayStart(7, true, true) && Upkeep::ErrandMayStart(7, true, false),
          "revived sick at the healer the bot rests; the errand waits until the sickness ends");
    Check(Upkeep::ErrandMayStart(5, true, false) && !Upkeep::ErrandMayStart(5, false, false),
          "a questing hunter without arrows (a due stock) goes to the vendor; full bags alone keep it questing");
    Check(!Upkeep::ErrandMayStart(10, true, false) && !Upkeep::ErrandMayStart(6, true, false),
          "travel and flights are not ended by an errand");
    Check(!Upkeep::ErrandMayStart(32, true, false), "a status out of range is never ended");
}
}  // namespace

int main()
{
    CheckPredicate();
    CheckShare();
    CheckKinds();
    CheckSickGate();
    CheckSickRests();
    CheckErrandStart();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
