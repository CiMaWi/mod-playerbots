/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Standalone check of the upkeep rules (src/Ai/World/Rpg/UpkeepRules.h): the grant (only a bot that keeps no
// possessions), the guid mix of ShareSlot.h that world PvP's start-cost share uses (stable, monotone, near its
// percent, the SQL form), the kind table, the sick gate (a sick bot rests: the status pick, the status update and the
// rest timeout over a table of statuses) and the errand start.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

#include "AwarenessRules.h"
#include "ProfessionConfig.h"
#include "ShareSlot.h"
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

static_assert(Upkeep::KIND_RULES.size() == static_cast<size_t>(UpkeepKind::Profession) + 1, "a rule per kind");

// Every persistent bot pays its own way (needs-and-errands NE.2): a handout only for a bot that keeps no possessions
// (a master's bot, a bot that is not persistent), never for a bot that keeps them, whatever its guid
void CheckPredicate()
{
    static_assert(Upkeep::Granted(false), "a bot that keeps no possessions gets a handout");
    static_assert(!Upkeep::Granted(true), "a persistent bot gets no handout");
    Check(Upkeep::Granted(false), "a master's bot or a non-keeper gets a handout");
    Check(!Upkeep::Granted(true), "a persistent bot gets no handout");
}

// The guid mix of ShareSlot.h (world PvP's start-cost share; its only test)
void CheckShare()
{
    using BotShare::InShare;
    using BotShare::SHARE_PERCENT_MAX;
    using BotShare::ShareSlot;
    bool stable = true;
    bool monotone = true;
    bool inRange = true;
    for (uint32_t guid = 0; guid < 10000; ++guid)
    {
        uint32_t const slot = ShareSlot(guid);
        stable = stable && slot == ShareSlot(guid);
        inRange = inRange && slot < SHARE_PERCENT_MAX;
        for (uint32_t share = 0; share < SHARE_PERCENT_MAX; ++share)
            if (InShare(guid, share) && !InShare(guid, share + 1))
                monotone = false;
    }
    Check(stable, "the same guid always lands in the same slot");
    Check(inRange, "every slot is 0..99");
    Check(monotone, "a guid in the share at p% stays in at any larger p");
    Check(!InShare(12345, 0), "share 0 holds nobody");
    Check(InShare(12345, 100), "share 100 holds everybody");

    // The formula as conf.dist and server-tools.md give it for SQL, with 64-bit arithmetic
    bool sqlMatches = true;
    for (uint64_t guid : {0ull, 1ull, 2ull, 1000ull, 65535ull, 123456ull, 4000000ull, 4294967295ull})
    {
        uint64_t const sql = (((guid * 2654435761ull) & 0xFFFFFFFFull) >> 16) % 100;
        sqlMatches = sqlMatches && sql == ShareSlot(static_cast<uint32_t>(guid));
    }
    Check(sqlMatches, "the SQL formula gives the same slot");

    // Over 10000 consecutive guids (bot guids are dense) the share lands within 1.5 points of p
    for (uint32_t share : {1u, 5u, 10u, 25u, 50u, 90u})
    {
        uint32_t count = 0;
        for (uint32_t guid = 1; guid <= 10000; ++guid)
            count += InShare(guid, share) ? 1 : 0;
        int32_t const deviation = static_cast<int32_t>(count) - static_cast<int32_t>(share * 100);
        Check(std::abs(deviation) <= 150,
              "share " + std::to_string(share) + "% holds " + std::to_string(count) + " of 10000 guids");
    }
}

void CheckKinds()
{
    std::set<std::string_view> words;
    for (Upkeep::KindRule const& rule : Upkeep::KIND_RULES)
        words.insert(EventLog::HonestyKindWord(rule.kind));
    Check(words.size() == Upkeep::KIND_COUNT, "every kind has its own honesty word");
    Check(!words.count("Item") && !words.count("Spell") && !words.count("Skill"),
          "no kind reuses the gear row Item or the old mixed rows Spell and Skill");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Food)) == "Food", "Food keeps its old row word");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Repair)) == "Repair", "Repair keeps its old row word");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Resurrect)) == "Resurrect", "Resurrect keeps its word");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Gold)) == "Gold", "Gold keeps its old row word");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Talent)) == "Talent", "Talent keeps its old row word");
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::Profession)) == "Profession" &&
              EventLog::Word(EventLog::TypeOf(Upkeep::HonestyKindOf(UpkeepKind::Profession))) == "HonestyProfession",
          "Profession's honesty row is the professions plan's HonestyProfession");
    Check(std::string_view(Upkeep::KindWord(static_cast<UpkeepKind>(Upkeep::KIND_COUNT))) == "Other",
          "a kind out of range is Other");

    // The class-quest kind keeps its row word: the completion (WU21a) writes it; no handout is left (WU40)
    Check(std::string_view(Upkeep::KindWord(UpkeepKind::ClassQuest)) == "ClassQuest",
          "ClassQuest keeps its row word for the class-quest completion");
}

void PrintKinds()
{
    std::cout << "Kinds (honesty words):\n";
    for (Upkeep::KindRule const& rule : Upkeep::KIND_RULES)
        std::cout << "  " << EventLog::HonestyKindWord(rule.kind) << "\n";
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
    {12, "do_fish", false, false, false},
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
        bool const ground = Upkeep::SickEndsStatus(row.status, true, false, false);
        bool const air = Upkeep::SickEndsStatus(row.status, true, true, false);
        // NE11: an errand that started after the rise stays; for every other status that fact changes nothing
        bool const sinceRise = Upkeep::SickEndsStatus(row.status, true, false, true);
        std::cout << "  " << row.word << ": " << healthy << " / " << needsRest << " / " << sick << "; " << ground
                  << " / " << air << "\n";
        std::string const word(row.word);
        Check(healthy, word + " may be picked by a healthy bot");
        Check(needsRest == !row.fights, word + (row.fights ? " is not" : " is") + " picked by a bot that needs rest");
        Check(sick == row.sickOpen && !(row.sickOpen && row.fights), word + " picked by a sick bot only if open");
        Check(Upkeep::StatusPickable(row.status, true, true) == row.sickOpen, word + ": sick and needing rest");
        Check(ground == !row.sickOpen, word + (row.sickOpen ? " stays" : " ends") + " while the bot is sick");
        Check(!air && !Upkeep::SickEndsStatus(row.status, true, true, true),
              word + ": a flight in the air lands first");
        Check(sinceRise == (ground && row.status != 11),
              word + (row.status == 11 ? ": a sick bot keeps the errand it started after the rise"
                                       : ": the errand's start time does not keep it"));
        Check(!Upkeep::SickEndsStatus(row.status, false, false, false) &&
                  !Upkeep::SickEndsStatus(row.status, false, false, true),
              word + " never ends for a healthy bot");
    }
    // Rest: its duration ends it, a sick bot's only once the sickness has ended
    Check(Upkeep::RestEnds(true, false) && !Upkeep::RestEnds(false, false), "rest ends after its duration");
    Check(!Upkeep::RestEnds(true, true) && !Upkeep::RestEnds(false, true), "a sick bot's rest does not time out");
    // The WU8b deaths while sick by the bot's status (review of WU8b-fix): the errand (914), go camp and wander npc
    // picks after a 13-35 s rest (168: go camp, killed, healer, go camp), an inn trip kept through death (636, 904)
    Check(Upkeep::SickEndsStatus(11, true, false, false), "914: an errand from before the death ends at the healer");
    Check(!Upkeep::SickEndsStatus(11, true, false, true),
          "NE11: the errand recover handed over goes on while sick (mutant: it ends at once, errand_sick)");
    Check(!Upkeep::RestEnds(true, true) && !Upkeep::StatusPickable(2, true, false) &&
              !Upkeep::StatusPickable(4, true, false),
          "168: the rest outlasts its 35 s and the pick offers no go camp or wander npc");
    Check(Upkeep::SickEndsStatus(10, true, false, false) && Upkeep::SickEndsStatus(10, true, false, true),
          "636: the inn trip kept from before the death ends");
    Check(Upkeep::SickEndsStatus(6, true, false, false) && !Upkeep::SickEndsStatus(6, true, true, false),
          "a flight not yet in the air ends, one in the air lands");
    // The sickness over: rest times out and the pick offers every status again, the errand start too
    bool everyStatus = true;
    for (StatusRow const& row : STATUS_ROWS)
        everyStatus = everyStatus && Upkeep::StatusPickable(row.status, false, false);
    Check(Upkeep::RestEnds(true, false) && everyStatus && Upkeep::ErrandMayStart(7, false, false),
          "the sickness over, the normal pick and the errand start resume");
}

// The errand trigger's start (WT3b's interruptible statuses and the sick gate; planning 3.19 (3.6): never questing)
void CheckErrandStart()
{
    std::cout << "Errand start (status: well / sick / sick with the repair handed over):\n";
    for (StatusRow const& row : STATUS_ROWS)
    {
        bool const well = Upkeep::ErrandMayStart(row.status, false, false);
        bool const sick = Upkeep::ErrandMayStart(row.status, true, false);
        bool const open = Upkeep::ErrandMayStart(row.status, true, true);
        std::cout << "  " << row.word << ": " << well << " / " << sick << " / " << open << "\n";
        Check(well == row.interrupts && Upkeep::ErrandMayStart(row.status, false, true) == well,
              std::string(row.word) + (row.interrupts ? " may" : " may not") + " be ended by the errand trigger");
        Check(!sick, std::string(row.word) + ": a sick bot starts no errand until recover hands a due repair over");
        Check(open == row.interrupts,
              std::string(row.word) + (row.interrupts ? ": the handed-over repair may end it" : ": never ended"));
    }
    // The healer revive in rest, a questing bot (the planner weighs its needs, WT63c), a bot on a planned trip
    Check(!Upkeep::ErrandMayStart(7, true, false) && Upkeep::ErrandMayStart(7, false, false),
          "revived sick at the healer the bot rests; the errand waits for recover's hand-over");
    Check(Upkeep::ErrandMayStart(7, true, true) && Upkeep::ErrandMayStart(0, true, true),
          "NE11: the sick rest (and idle) give way to the due repair recover handed over (mutant: open is ignored)");
    Check(!Upkeep::ErrandMayStart(5, false, false) && !Upkeep::ErrandMayStart(5, true, true),
          "the errand trigger never ends questing, a due need neither");
    Check(!Upkeep::ErrandMayStart(10, false, false) && !Upkeep::ErrandMayStart(6, false, false),
          "travel and flights are not ended by an errand");
    Check(!Upkeep::ErrandMayStart(32, false, false), "a status out of range is never ended");
}

// NE11 (NE.3): the hand-over of recover to the due repair. Open only when sick, a repair due, the bot restored (the
// rule recover waits for, AwarenessRules::Restored) and on a clear point; every other case rests as before, and the
// rest ends with the sickness (RestEnds), so the wait has its exit
void CheckSickErrandOpen()
{
    AwarenessRules::RecoverFacts facts;
    facts.sick = true;
    facts.healthPct = 100;
    facts.powerPct = 100;
    facts.usesMana = true;
    facts.own.clear = true;
    auto const open = [](AwarenessRules::RecoverFacts const& f, bool repairDue)
    { return Upkeep::SickErrandOpen(f.sick, repairDue, AwarenessRules::Restored(f), f.own.clear); };
    Check(open(facts, true), "sick, a repair due, restored on a clear point: the errand opens");
    AwarenessRules::RecoverFacts well = facts;
    well.sick = false;
    Check(!open(well, true), "a bot that is not sick is not this rule's (the errand rules of a healthy bot)");
    AwarenessRules::RecoverFacts unclear = facts;
    unclear.own.clear = false;
    Check(!open(unclear, true) && AwarenessRules::RecoverStep(unclear) == AwarenessRules::RecoverAct::Move,
          "on a place someone reaches recover moves the bot first; the errand waits");
    AwarenessRules::RecoverFacts low = facts;
    low.healthPct = AwarenessRules::RECOVER_HEALTH_PCT - 1;
    Check(!open(low, true), "health at 79 %: not restored, the bot rests");
    AwarenessRules::RecoverFacts dry = facts;
    dry.powerPct = AwarenessRules::RECOVER_POWER_PCT - 1;
    Check(!open(dry, true), "a mana user at 79 % mana: not restored, the bot rests");
    Check(!open(facts, false), "nothing due (no repair): the bot rests, no stock or bag errand opens alone");
    Check(!Upkeep::ErrandMayStart(7, true, open(low, true)) && Upkeep::RestEnds(true, false) &&
              !Upkeep::RestEnds(true, true),
          "a closed hand-over leaves the old rule: the sick bot rests until the sickness ends");
}
}  // namespace

int main()
{
    CheckPredicate();
    CheckShare();
    CheckKinds();
    PrintKinds();
    CheckSickGate();
    CheckSickRests();
    CheckErrandStart();
    CheckSickErrandOpen();

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
