/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Offline proof of the bot record's rules (living-conversation LC6, RecordRules.h): the kind words, the score of each
// kind against the marks, nothing kept below the low mark, the lifetime by score, the bounds, the merge of one zone's
// older small rows into a stay, one id for a shared event, and one fight key for both sides of a PvP fight (LC6b).
// Build (Git Bash, module root): clang-cl /std:c++20 /EHsc /W4 /WX /I src/Bot tests/bot-record-selftest.cpp

#include <cstdio>
#include <set>
#include <string>

#include "RecordRules.h"

namespace
{
using namespace RecordRules;

int passed = 0;
int failed = 0;

void Check(bool pass, char const* name)
{
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    (pass ? passed : failed)++;
}

bool Recent(uint8_t score) { return Kept(score) && !Permanent(score); }

Facts KillOf(uint8_t rank, int32_t levelDiff)
{
    Facts facts;
    facts.Rank = rank;
    facts.LevelDiff = levelDiff;
    return facts;
}

Row RowOf(uint32_t id, uint32_t zone, uint32_t time, uint8_t score, uint32_t expires)
{
    Row row;
    row.EventId = id;
    row.EventKind = Kind::Quest;
    row.Zone = zone;
    row.Map = 0;
    row.Time = time;
    row.Score = score;
    row.Expires = expires;
    return row;
}

void Words()
{
    std::set<std::string> seen;
    bool complete = true;
    for (char const* word : KIND_WORDS)
    {
        complete = complete && word && *word;
        if (word)
            seen.insert(word);
    }
    Check(complete, "every kind has a word");
    Check(seen.size() == KIND_COUNT, "the kind words are unique");
    Check(std::string(Word(Kind::Kill)) == "kill" && std::string(Word(Kind::Quest)) == "quest" &&
              std::string(Word(Kind::Equip)) == "equip" && std::string(Word(Kind::Level)) == "level" &&
              std::string(Word(Kind::Skill)) == "skill" && std::string(Word(Kind::Spell)) == "spell" &&
              std::string(Word(Kind::Flight)) == "flight" && std::string(Word(Kind::Death)) == "death" &&
              std::string(Word(Kind::Resurrect)) == "resurrect" && std::string(Word(Kind::Fight)) == "fight" &&
              std::string(Word(Kind::Dungeon)) == "dungeon" && std::string(Word(Kind::Group)) == "group" &&
              std::string(Word(Kind::Stay)) == "stay" && std::string(Word(Kind::DungeonRun)) == "dungeon_run" &&
              std::string(Word(Kind::Guild)) == "guild" && std::string(Word(Kind::Duel)) == "duel",
          "each kind keeps its stable word");
    Check(KindOf(static_cast<uint32_t>(Kind::Duel)) == Kind::Duel && !KindOf(KIND_COUNT), "the kind column maps back");
}

void Scores()
{
    Check(!Kept(Score(Kind::Kill, KillOf(RANK_NORMAL, 0))), "a normal kill at the bot's level is below the low mark");
    Check(!Kept(Score(Kind::Kill, KillOf(RANK_NORMAL, HARD_LEVELS - 1))), "a normal kill two levels up is below it");
    Check(Recent(Score(Kind::Kill, KillOf(RANK_NORMAL, HARD_LEVELS))), "a normal kill three levels up is a deed");
    Check(Recent(Score(Kind::Kill, KillOf(RANK_ELITE, -5))), "an elite kill is a deed at any level");
    Check(Recent(Score(Kind::Kill, KillOf(RANK_RARE_ELITE, 0))), "a rare elite kill is a deed");
    Check(Recent(Score(Kind::Kill, KillOf(RANK_RARE, 0))), "a rare kill is a deed");
    Check(Permanent(Score(Kind::Kill, KillOf(RANK_WORLD_BOSS, 0))), "a world boss kill is kept for good");
    Check(Score(Kind::Kill, KillOf(RANK_RARE_ELITE, 0)) > Score(Kind::Kill, KillOf(RANK_ELITE, 0)) &&
              Score(Kind::Kill, KillOf(RANK_ELITE, 0)) > Score(Kind::Kill, KillOf(RANK_NORMAL, HARD_LEVELS)),
          "kill scores rise with the rank");
    Check(Recent(Score(Kind::Quest, {})), "a quest done is a deed");

    Facts equip;
    Check(!Kept(Score(Kind::Equip, equip)), "an ordinary item at no gain is below the low mark");
    equip.ItemLevelGain = ITEM_LEVEL_GAIN;
    Check(Recent(Score(Kind::Equip, equip)), "five item levels more is a deed");
    equip.ItemLevelGain = 0;
    equip.Quality = QUALITY_RARE;
    Check(Recent(Score(Kind::Equip, equip)), "a rare item is a deed");
    equip.Quality = QUALITY_EPIC;
    Check(Recent(Score(Kind::Equip, equip)) && Score(Kind::Equip, equip) > 25, "an epic item scores above a rare one");

    Facts level;
    level.Level = 11;
    Check(Recent(Score(Kind::Level, level)), "a level gained is a deed");
    level.Level = 20;
    Check(Permanent(Score(Kind::Level, level)), "every tenth level is kept for good");

    Facts skill;
    skill.Craft = true;
    skill.Skill = 150;
    Check(Permanent(Score(Kind::Skill, skill)), "a craft rank is kept for good");
    skill.Skill = 151;
    Check(!Kept(Score(Kind::Skill, skill)), "a craft point between ranks is below the low mark");
    skill.Craft = false;
    skill.Skill = 150;
    Check(!Kept(Score(Kind::Skill, skill)), "a weapon skill at 150 is below the low mark");
    Check(CraftRank(75) && CraftRank(450) && !CraftRank(0) && !CraftRank(525) && !CraftRank(74), "the craft ranks");

    Facts spell;
    Check(!Kept(Score(Kind::Spell, spell)), "a class spell is below the low mark");
    spell.Mount = true;
    Check(Permanent(Score(Kind::Spell, spell)), "a mount or riding is kept for good");

    Check(Recent(Score(Kind::Flight, {})), "a flight point is a deed");
    Check(Recent(Score(Kind::Death, {})), "a death is a deed");
    Check(Recent(Score(Kind::Resurrect, {})), "a resurrection is a deed");
    Check(Recent(Score(Kind::Fight, {})), "an ended fight is a deed");
    Facts mattered;
    mattered.Mattered = true;
    Check(Permanent(Score(Kind::Fight, mattered)) && Permanent(Score(Kind::Duel, mattered)),
          "a fight or duel that mattered is kept for good");
    Check(Recent(Score(Kind::Dungeon, {})), "a dungeon entered is a deed");
    Facts raid;
    raid.Raid = true;
    Check(Recent(Score(Kind::Dungeon, raid)) && Score(Kind::Dungeon, raid) > Score(Kind::Dungeon, {}),
          "a raid entered scores above a dungeon");
    Check(Permanent(Score(Kind::Group, {})) && Permanent(Score(Kind::Stay, {})) &&
              Permanent(Score(Kind::DungeonRun, {})) && Permanent(Score(Kind::Guild, {})),
          "the later plans' major kinds are declared for good");

    bool bounded = true;
    for (std::size_t index = 0; index < KIND_COUNT; ++index)
        bounded = bounded && Score(static_cast<Kind>(index), mattered) <= MAX_SCORE &&
                  Score(static_cast<Kind>(index), {}) <= MAX_SCORE;
    Check(bounded, "every score is within 0..100");
    Check(LOW_MARK < SMALL_SCORE && SMALL_SCORE < HIGH_MARK && HIGH_MARK <= MAX_SCORE, "the marks are ordered");
}

void Marks()
{
    Check(!Kept(0) && !Kept(LOW_MARK - 1), "nothing below the low mark is kept");
    Check(Kept(LOW_MARK) && !Permanent(LOW_MARK), "the low mark is a recent deed");
    Check(!Permanent(HIGH_MARK - 1) && Permanent(HIGH_MARK), "the high mark keeps a row for good");
}

void Expiry()
{
    bool monotone = true;
    bool positive = true;
    for (uint8_t score = LOW_MARK; score + 1 < HIGH_MARK; ++score)
    {
        monotone = monotone && Lifetime(score + 1) > Lifetime(score);
        positive = positive && Lifetime(score) > 0;
    }
    Check(monotone, "a higher score lives longer");
    Check(positive, "every recent deed expires after a while");
    Check(Lifetime(HIGH_MARK) == 0 && Expires(MAX_SCORE, 1000) == 0, "a permanent row never expires");
    uint32_t const now = 1000000;
    uint32_t const expires = Expires(LOW_MARK, now);
    Check(expires == now + Lifetime(LOW_MARK), "the expiry is the time plus the lifetime");
    Row row = RowOf(1, 1, now, LOW_MARK, expires);
    Check(!Expired(row, expires - 1) && Expired(row, expires), "a row expires at its expiry time");
    row.Expires = 0;
    Check(!Expired(row, 0xFFFFFFFF), "a permanent row is never expired");
}

void Bounds()
{
    uint32_t const now = 5000000;
    std::vector<Row> rows;
    for (uint32_t index = 0; index < MAX_PERMANENT + 5; ++index)
        rows.push_back(RowOf(index + 1, 1, now - index, static_cast<uint8_t>(HIGH_MARK + index % 30), 0));
    rows.push_back(RowOf(500, 1, now - 10, LOW_MARK, now - 1));
    rows.push_back(RowOf(501, 1, now - 10, LOW_MARK, now + 100));
    std::vector<Row> const removed = Prune(rows, now);
    std::size_t permanent = 0;
    uint8_t lowestKept = MAX_SCORE;
    for (Row const& row : rows)
        if (!row.Expires)
        {
            ++permanent;
            lowestKept = std::min(lowestKept, row.Score);
        }
    uint8_t highestRemoved = 0;
    bool expiredRemoved = false;
    for (Row const& row : removed)
    {
        if (!row.Expires)
            highestRemoved = std::max(highestRemoved, row.Score);
        expiredRemoved = expiredRemoved || row.EventId == 500;
    }
    Check(permanent == MAX_PERMANENT, "the permanent rows are bounded");
    Check(highestRemoved <= lowestKept, "the bound drops the weakest permanent rows");
    Check(expiredRemoved && removed.size() == 6, "an expired row is removed, a living one kept");

    std::vector<Row> recent;
    for (uint32_t index = 0; index < MAX_RECENT + 3; ++index)
        recent.push_back(RowOf(index + 1, index, now, LOW_MARK, now + 1000 + index));
    std::vector<Row> const dropped = Prune(recent, now);
    Check(recent.size() == MAX_RECENT && dropped.size() == 3 && dropped.front().EventId == 1,
          "the recent rows are bounded, the first to expire goes");
}

void Merge()
{
    uint32_t const now = 9000000;
    uint32_t const old = now - MERGE_AGE;
    std::vector<Row> rows = {RowOf(1, 10, old - 50, 25, now + 10), RowOf(2, 20, old - 40, 25, now + 20),
                             RowOf(3, 10, old - 30, 30, now + 30), RowOf(4, 20, old - 20, 25, now + 40),
                             RowOf(5, 10, old - 10, 22, now + 50)};
    std::optional<MergePlan> plan = PlanMerge(rows, now);
    Check(plan && plan->Merged.size() == 3 && plan->Merged[0] == 0 && plan->Merged[1] == 2 && plan->Merged[2] == 4,
          "the merge takes the small rows of one zone only");
    Check(plan && plan->Stay.EventKind == Kind::Stay && plan->Stay.Zone == 10 && plan->Stay.Time == old - 50,
          "the stay sits at the zone's first row");
    Check(plan && plan->Stay.Score == 30 && plan->Stay.Expires == now + 50 && !Permanent(plan->Stay.Score),
          "the stay keeps the highest score and the latest expiry and stays recent");
    Check(plan && Count(plan->Stay) == 3, "the stay counts its rows");

    rows.push_back(RowOf(6, 20, old - 5, 25, now + 60));
    rows[0].Zone = 30;
    plan = PlanMerge(rows, now);
    Check(plan && plan->Merged.size() == 3 && plan->Stay.Zone == 20, "the oldest qualifying zone merges first");

    std::vector<Row> twice = {RowOf(1, 10, old - 50, 25, now + 10), RowOf(2, 10, old - 40, 25, now + 20)};
    twice.front().EventKind = Kind::Stay;
    twice.front().Facts = "rows=4;";
    twice.push_back(RowOf(3, 10, old - 30, 25, now + 30));
    plan = PlanMerge(twice, now);
    Check(plan && Count(plan->Stay) == 6, "a merge of a stay conserves the counts");

    std::vector<Row> young = {RowOf(1, 10, now - 10, 25, now + 10), RowOf(2, 10, now - 20, 25, now + 20),
                              RowOf(3, 10, now - 30, 25, now + 30)};
    Check(!PlanMerge(young, now), "young rows are not merged");
    std::vector<Row> big = {RowOf(1, 10, old, SMALL_SCORE, now + 10), RowOf(2, 10, old, 50, now + 20),
                            RowOf(3, 10, old, 25, now + 30)};
    Check(!PlanMerge(big, now), "rows at or above the small score are not merged");
    std::vector<Row> lasting = {RowOf(1, 10, old, 25, 0), RowOf(2, 10, old, 25, now + 20),
                                RowOf(3, 10, old, 25, now + 30)};
    Check(!PlanMerge(lasting, now), "a permanent row is never merged");
    Check(!PlanMerge({RowOf(1, 10, old, 25, now + 10), RowOf(2, 10, old, 25, now + 20)}, now),
          "fewer than three rows of a zone are not merged");
}

void Sharing()
{
    ShareBook book;
    uint32_t const now = 7000000;
    Check(!book.Find(Kind::Kill, 42, now), "an unnoted event has no id");
    book.Note(Kind::Kill, 42, 900, now);
    Check(book.Find(Kind::Kill, 42, now + 1) == 900, "a second participant reuses the event's id");
    Check(book.Find(Kind::Kill, 42, now + SHARE_WINDOW) == 900, "the id holds through the window");
    Check(!book.Find(Kind::Kill, 42, now + SHARE_WINDOW + 1), "after the window the event is a new one");
    Check(!book.Find(Kind::Fight, 42, now), "another kind with the same key is another event");
    Check(!book.Find(Kind::Kill, 43, now), "another key is another event");
    for (uint32_t key = 0; key < SHARE_PRUNE + 10; ++key)
        book.Note(Kind::Fight, key, key + 1, now + SHARE_WINDOW * 2 + key);
    Check(book.Size() <= SHARE_PRUNE + 10 && !book.Find(Kind::Kill, 42, now + SHARE_WINDOW * 3),
          "old notes are pruned");
}

FightNotes::Person Bot(uint32_t guid) { return {guid, "b" + std::to_string(guid)}; }

bool Lists(std::string const& others, std::string const& token)
{
    return (" " + others + " ").find(" " + token + " ") != std::string::npos;
}

void Fights()
{
    uint32_t const now = 8000000;
    {
        FightNotes notes;
        Check(!notes.Enter(Bot(1), {}, now) && !notes.Size(), "a combat edge without an enemy player notes nothing");
        Check(!notes.Leave(1, now + 5), "a combat end without a fight is no fight");
    }
    {
        FightNotes notes;
        uint64_t const key = notes.Enter(Bot(1), {Bot(2)}, now);
        Check(key && notes.Enter(Bot(2), {Bot(1)}, now) == key, "a pair adopts one key");
        std::optional<FightNotes::Ended> const first = notes.Leave(1, now + 8);
        Check(first && first->Key == key && first->Seconds == 8 && first->Others == "b2",
              "the end gives the key, the seconds and the enemy");
        Check(!notes.Leave(1, now + 9), "a second end of the same bot is a no-op");
        std::optional<FightNotes::Ended> const second = notes.Leave(2, now + 10);
        Check(second && second->Key == key && second->Seconds == 10 && second->Others == "b1" && !notes.Size() &&
                  !notes.Fights(),
              "the last end closes the fight");
        Check(notes.Enter(Bot(1), {Bot(2)}, now + 100) != key, "a later fight of the pair is a new one");
    }
    {
        // lcfix fight: each pair of the two teams in turn; the first edge of each bot sees one enemy
        FightNotes notes;
        uint64_t const key = notes.Enter(Bot(11), {Bot(21)}, now);
        bool const one = notes.Enter(Bot(21), {Bot(11)}, now) == key && notes.Enter(Bot(22), {Bot(11)}, now) == key &&
                         notes.Enter(Bot(12), {Bot(21)}, now) == key;
        Check(one && notes.Size() == 4 && notes.Fights() == 1, "four bots of two teams adopt one key");
        std::optional<FightNotes::Ended> const ally = notes.Leave(11, now + 6);
        Check(ally && Lists(ally->Others, "b21") && Lists(ally->Others, "b22") && Lists(ally->Others, "b12") &&
                  !Lists(ally->Others, "b11"),
              "the others name the enemies and the partner, never the bot");
        std::optional<FightNotes::Ended> const foe = notes.Leave(22, now + 11);
        Check(foe && foe->Key == key && Lists(foe->Others, "b11") && Lists(foe->Others, "b12"),
              "a bot that ends later keeps the fight's key and its bots");
    }
    {
        FightNotes notes;
        uint64_t const key = notes.Enter(Bot(1), {{2, "p2"}}, now);
        Check(notes.Enter(Bot(1), {{3, "p3"}}, now + 2) == key, "a missed end: the next edge continues the fight");
        std::optional<FightNotes::Ended> const ended = notes.Leave(1, now + 4);
        Check(ended && ended->Others == "p2 p3" && ended->Seconds == 4, "real players are named as players");
        uint64_t const other = notes.Enter(Bot(5), {{6, "p6"}}, now);
        Check(other && other != key, "a player's fight without bots gets its own key");
    }
}

// LC18e: one Visit row per place, a later entry moves its time and adds no row; Visit rows are never pruned or merged
void Visits()
{
    uint32_t const now = 5000000;
    Check(std::string(Word(Kind::Visit)) == "visit" && KindOf(16) == Kind::Visit && KIND_COUNT == 17,
          "visit is the kind after duel (stored kinds keep their numbers)");
    Check(Permanent(Score(Kind::Visit, {})) && Lifetime(Score(Kind::Visit, {})) == 0, "a visit is kept for good");

    VisitBook book;
    uint32_t lastId = 40;
    VisitAt elwynn;
    elwynn.Map = 0;
    elwynn.Zone = 12;
    elwynn.Area = 87;
    elwynn.Continent = true;
    std::vector<Visited> first = book.Enter(elwynn, now, lastId);
    Check(first.size() == 2 && first[0].Added && first[1].Added && book.Size() == 2 && lastId == 42,
          "a first entry on a continent adds the zone and the continent, each with a new event id");
    Check(first[0].Visit.EventId == 41 && first[0].Visit.EventKind == Kind::Visit &&
              first[0].Visit.Facts == "visit=zone;" && first[0].Visit.Zone == 12 && first[0].Visit.Map == 0 &&
              first[0].Visit.Time == now && first[0].Visit.Expires == 0 && first[1].Visit.Facts == "visit=continent;",
          "a visit row names its place and its time and never expires");
    Check(VisitPlaceOf(first[0].Visit) == VisitPlace::Zone && VisitPlaceOf(first[1].Visit) == VisitPlace::Continent,
          "a visit row's place reads back from its facts");

    std::vector<Visited> again = book.Enter(elwynn, now + 600, lastId);
    Check(again.size() == 2 && !again[0].Added && !again[1].Added && book.Size() == 2 && lastId == 42,
          "a second entry adds no row and takes no event id");
    Check(again[0].Visit.EventId == 41 && again[0].Visit.Time == now + 600, "a second entry moves the row's time");
    Check(book.Enter(elwynn, now + 600, lastId).empty(), "an entry at the row's time writes nothing");
    bool moved = false;
    for (Row const& row : book.Rows())
        moved = moved || (row.EventId == 41 && row.Time == now + 600);
    Check(moved && book.Rows().size() == 2, "the book keeps the moved time");

    VisitAt westfall = elwynn;
    westfall.Zone = 40;
    std::vector<Visited> next = book.Enter(westfall, now + 700, lastId);
    Check(next.size() == 2 && next[0].Added && !next[1].Added && next[0].Visit.EventId == 43 && book.Size() == 3,
          "a new zone on a known continent: one new row, the continent's time moves");

    VisitAt deadmines;
    deadmines.Map = 36;
    deadmines.Zone = 1581;
    deadmines.Instance = true;
    std::vector<Visited> inside = book.Enter(deadmines, now + 800, lastId);
    Check(
        inside.size() == 1 && inside[0].Added && inside[0].Visit.Facts == "visit=dungeon;" && inside[0].Visit.Map == 36,
        "a dungeon is one row by its map, no zone row");
    deadmines.Zone = 1582;
    Check(book.Enter(deadmines, now + 900, lastId).size() == 1 && book.Size() == 4,
          "another zone of the same dungeon moves the dungeon's row");

    VisitAt ebonHold;
    ebonHold.Map = 609;
    ebonHold.Zone = 4298;
    Check(book.Enter(ebonHold, now, lastId).size() == 1 && book.Size() == 5,
          "a zone off the continents: no continent row");
    VisitAt nowhere;
    Check(book.Enter(nowhere, now, lastId).empty(), "an entry without a zone writes nothing");

    // Load: the stored rows come back by place; a second row of one place, another kind or a broken fact is left out
    VisitBook loaded;
    Row stored = first[0].Visit;
    Row twin = stored;
    twin.EventId = 99;
    Row quest = stored;
    quest.EventKind = Kind::Quest;
    quest.Zone = 13;
    Row broken = stored;
    broken.Zone = 14;
    broken.Facts = "visit=town;";
    Check(
        loaded.Load(stored) && !loaded.Load(twin) && !loaded.Load(quest) && !loaded.Load(broken) && loaded.Size() == 1,
        "loading keeps one row per place and only visit rows");
    uint32_t loadedId = 500;
    VisitAt zoneOnly = elwynn;
    zoneOnly.Continent = false;
    std::vector<Visited> back = loaded.Enter(zoneOnly, now + 1000, loadedId);
    Check(back.size() == 1 && !back[0].Added && back[0].Visit.EventId == 41 && loadedId == 500,
          "an entry finds a loaded row: its time moves, no new id");

    // Prune and merge: visit rows stand outside the bounds, never merged, even over MAX_PERMANENT of them
    std::vector<Row> rows;
    for (uint32_t index = 0; index < MAX_PERMANENT + 5; ++index)
        rows.push_back(RowOf(index + 1, 1, now - index, HIGH_MARK, 0));
    VisitBook many;
    uint32_t manyId = 1000;
    for (uint32_t zone = 1; zone <= MAX_PERMANENT + 20; ++zone)
    {
        VisitAt at;
        at.Zone = zone;
        many.Enter(at, now - 2 * MERGE_AGE, manyId);
    }
    for (Row const& row : many.Rows())
        rows.push_back(row);
    std::vector<Row> const removed = Prune(rows, now);
    std::size_t visits = 0;
    std::size_t deeds = 0;
    for (Row const& row : rows)
        (row.EventKind == Kind::Visit ? visits : deeds)++;
    bool visitRemoved = false;
    for (Row const& row : removed)
        visitRemoved = visitRemoved || row.EventKind == Kind::Visit;
    Check(visits == MAX_PERMANENT + 20 && !visitRemoved && deeds == MAX_PERMANENT && removed.size() == 5,
          "no visit row is pruned; the permanent deeds keep their own bound beside them");

    std::vector<Row> zoneRows = many.Rows();
    for (Row& row : zoneRows)
        row.Zone = 7;
    Check(!PlanMerge(zoneRows, now), "old visit rows of one zone are never merged");
    std::vector<Row> mixed = zoneRows;
    for (uint32_t id = 1; id <= MERGE_MIN; ++id)
        mixed.push_back(RowOf(id, 7, now - 2 * MERGE_AGE, LOW_MARK, now + 1000));
    std::optional<MergePlan> const plan = PlanMerge(mixed, now);
    bool onlyDeeds = plan.has_value();
    if (plan)
        for (std::size_t index : plan->Merged)
            onlyDeeds = onlyDeeds && mixed[index].EventKind != Kind::Visit;
    Check(onlyDeeds && plan && plan->Merged.size() == MERGE_MIN, "a merge beside visit rows takes only the deeds");
}

void Helpers()
{
    Check(FactValue("rows=12;rank=1;", "rows") == 12u && FactValue("a=1;rank=3;", "rank") == 3u, "a fact's value");
    Check(!FactValue("rows=12;", "rank") && !FactValue("rows=x;", "rows") && !FactValue("", "rows"),
          "a missing or broken fact has no value");
    Check(HandedOut("mgr_level") && HandedOut("auto gear") && HandedOut("maintenance") && !HandedOut("loot") &&
              !HandedOut("other") && !HandedOut("equip upgrades"),
          "items, levels and spells handed out by the server are not the bot's play");
}
}  // namespace

int main()
{
    Words();
    Scores();
    Marks();
    Expiry();
    Bounds();
    Merge();
    Sharing();
    Fights();
    Visits();
    Helpers();
    std::printf("%s: %d passed, %d failed\n", failed ? "FAIL" : "PASS", passed, failed);
    return failed ? 1 : 0;
}
