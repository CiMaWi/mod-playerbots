/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RECORDRULES_H
#define PLAYERBOTS_RECORDRULES_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// A bot's record (living-conversation LC6, BotRecordMgr): the major events of its real play, one row per bot and event.
// Pure rules without game types, so tests/bot-record-selftest.cpp proves them offline: the kinds and their words, one
// impact score per event, the marks (below the low mark nothing is kept, above the high mark a row is kept for good),
// the lifetime of a recent deed, the bounds, the merge of older small rows of one zone into a stay, and one id for an
// event that several bots share. All numbers are tuning values.
namespace RecordRules
{
enum class Kind : uint8_t
{
    Kill,
    Quest,
    Equip,
    Level,
    Skill,
    Spell,
    Flight,
    Death,
    Resurrect,
    Fight,
    Dungeon,
    // Declared with their score for the plans that own them; nothing emits them yet (a merged stay excepted)
    Group,
    Stay,
    DungeonRun,
    Guild,
    Duel
};

constexpr std::size_t KIND_COUNT = 16;
// The table's kind column is the index; the words are stable (world-log.md, Record)
inline constexpr char const* KIND_WORDS[KIND_COUNT] = {
    "kill",      "quest", "equip",   "level", "skill", "spell",       "flight", "death",
    "resurrect", "fight", "dungeon", "group", "stay",  "dungeon_run", "guild",  "duel"};

inline char const* Word(Kind kind)
{
    std::size_t const index = static_cast<std::size_t>(kind);
    return index < KIND_COUNT ? KIND_WORDS[index] : "other";
}

inline std::optional<Kind> KindOf(uint32_t index)
{
    return index < KIND_COUNT ? std::optional<Kind>(static_cast<Kind>(index)) : std::nullopt;
}

constexpr uint8_t LOW_MARK = 20;
constexpr uint8_t HIGH_MARK = 60;
constexpr uint8_t MAX_SCORE = 100;
// A recent deed lives this long per score point: 10 h at the low mark, almost 30 h just below the high mark
constexpr uint32_t SECONDS_PER_POINT = 1800;
constexpr std::size_t MAX_PERMANENT = 100;
constexpr std::size_t MAX_RECENT = 100;
// Recent rows below SMALL_SCORE and older than MERGE_AGE merge into one stay when a zone has MERGE_MIN of them
constexpr uint8_t SMALL_SCORE = 40;
constexpr uint32_t MERGE_AGE = 3600;
constexpr std::size_t MERGE_MIN = 3;
// The participants of one event (kind and key) within this many seconds share its id
constexpr uint32_t SHARE_WINDOW = 60;
constexpr std::size_t SHARE_PRUNE = 256;
// Creature ranks as the core's CREATURE_ELITE_* values
constexpr uint8_t RANK_NORMAL = 0;
constexpr uint8_t RANK_ELITE = 1;
constexpr uint8_t RANK_RARE_ELITE = 2;
constexpr uint8_t RANK_WORLD_BOSS = 3;
constexpr uint8_t RANK_RARE = 4;
// A normal creature this many levels above the bot is a deed
constexpr int32_t HARD_LEVELS = 3;
constexpr uint8_t QUALITY_RARE = 3;
constexpr uint8_t QUALITY_EPIC = 4;
constexpr int32_t ITEM_LEVEL_GAIN = 5;
constexpr uint32_t LEVEL_MILESTONE = 10;
constexpr uint32_t CRAFT_RANK_STEP = 75;
constexpr uint32_t CRAFT_RANK_TOP = 450;

// The checkable facts a score depends on; the manager fills those of its kind
struct Facts
{
    uint8_t Rank = RANK_NORMAL;  // kill: the creature's rank
    int32_t LevelDiff = 0;       // kill: the creature's level minus the bot's
    uint32_t Level = 0;          // level: the new level
    uint32_t Skill = 0;          // skill: the new value
    bool Craft = false;          // skill: a profession or secondary skill
    bool Mount = false;          // spell: a mount or a riding skill
    uint8_t Quality = 0;         // equip: the item's quality
    int32_t ItemLevelGain = 0;   // equip: the new item's level minus the old one's
    bool Raid = false;           // dungeon: a raid
    bool Mattered = false;       // fight, duel: the owning plan's judgement
};

inline bool CraftRank(uint32_t skill)
{
    return skill >= CRAFT_RANK_STEP && skill <= CRAFT_RANK_TOP && skill % CRAFT_RANK_STEP == 0;
}

// One impact score per event, 0..MAX_SCORE
inline uint8_t Score(Kind kind, Facts const& facts)
{
    switch (kind)
    {
        case Kind::Kill:
            switch (facts.Rank)
            {
                case RANK_WORLD_BOSS:
                    return 70;
                case RANK_RARE_ELITE:
                    return 50;
                case RANK_ELITE:
                    return 40;
                case RANK_RARE:
                    return 30;
                default:
                    return facts.LevelDiff >= HARD_LEVELS ? 25 : 5;
            }
        case Kind::Quest:
            return 35;
        case Kind::Equip:
            if (facts.Quality >= QUALITY_EPIC)
                return 45;
            return facts.Quality >= QUALITY_RARE || facts.ItemLevelGain >= ITEM_LEVEL_GAIN ? 25 : 5;
        case Kind::Level:
            return facts.Level % LEVEL_MILESTONE == 0 ? 70 : 25;
        case Kind::Skill:
            return facts.Craft && CraftRank(facts.Skill) ? 65 : 5;
        case Kind::Spell:
            return facts.Mount ? 65 : 5;
        case Kind::Flight:
            return 25;
        case Kind::Death:
            return 30;
        case Kind::Resurrect:
            return 25;
        case Kind::Fight:
            return facts.Mattered ? 65 : 40;
        case Kind::Dungeon:
            return facts.Raid ? 45 : 30;
        case Kind::Group:
            return 60;
        case Kind::Stay:
            return 60;
        case Kind::DungeonRun:
            return 70;
        case Kind::Guild:
            return 60;
        case Kind::Duel:
            return facts.Mattered ? 65 : 30;
    }

    return 0;
}

inline bool Kept(uint8_t score) { return score >= LOW_MARK; }

inline bool Permanent(uint8_t score) { return score >= HIGH_MARK; }

// A recent deed's lifetime in seconds, growing with its score; 0 for a permanent row
inline uint32_t Lifetime(uint8_t score)
{
    return Permanent(score) ? 0 : static_cast<uint32_t>(std::min(score, MAX_SCORE)) * SECONDS_PER_POINT;
}

// The expiry time (unix seconds) of a row recorded at now; 0 = kept for good
inline uint32_t Expires(uint8_t score, uint32_t now)
{
    uint32_t const lifetime = Lifetime(score);
    return lifetime ? now + lifetime : 0;
}

// Sources whose items, levels and spells the server hands out (factory, manager), not the bot's play
inline bool HandedOut(std::string_view source)
{
    return source.substr(0, 4) == "mgr_" || source.find("maintenance") != std::string_view::npos ||
           source.find("autogear") != std::string_view::npos || source.find("auto gear") != std::string_view::npos;
}

// The value of key in a facts text ("key=value;key=value;"), or none
inline std::optional<uint32_t> FactValue(std::string_view facts, std::string_view key)
{
    std::size_t start = 0;
    while (start < facts.size())
    {
        std::size_t const end = std::min(facts.find(';', start), facts.size());
        std::string_view const pair = facts.substr(start, end - start);
        std::size_t const equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == key)
        {
            uint32_t value = 0;
            for (char const c : pair.substr(equals + 1))
            {
                if (c < '0' || c > '9')
                    return std::nullopt;
                value = value * 10 + static_cast<uint32_t>(c - '0');
            }
            return value;
        }
        start = end + 1;
    }

    return std::nullopt;
}

constexpr std::string_view ROWS_FACT = "rows";

struct Row
{
    uint32_t EventId = 0;
    Kind EventKind = Kind::Kill;
    uint32_t Time = 0;
    uint32_t Map = 0;
    uint32_t Zone = 0;
    uint32_t Area = 0;
    uint8_t Score = 0;
    std::string Others;
    std::string Facts;
    uint32_t Expires = 0;
};

// The events a row stands for: a merged stay counts its rows, any other row 1
inline uint32_t Count(Row const& row)
{
    return row.EventKind == Kind::Stay ? FactValue(row.Facts, ROWS_FACT).value_or(1) : 1;
}

inline bool Expired(Row const& row, uint32_t now) { return row.Expires && row.Expires <= now; }

// Removes the expired rows and, over a bound, the weakest permanent row (lowest score, then oldest) or the recent row
// that would expire first; returns the removed rows
inline std::vector<Row> Prune(std::vector<Row>& rows, uint32_t now)
{
    std::vector<Row> removed;
    auto const expired =
        std::stable_partition(rows.begin(), rows.end(), [now](Row const& row) { return !Expired(row, now); });
    removed.assign(std::make_move_iterator(expired), std::make_move_iterator(rows.end()));
    rows.erase(expired, rows.end());
    for (bool permanent : {true, false})
    {
        std::size_t const bound = permanent ? MAX_PERMANENT : MAX_RECENT;
        auto const counted = [permanent](Row const& row) { return (row.Expires == 0) == permanent; };
        while (static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), counted)) > bound)
        {
            auto weakest = rows.end();
            for (auto it = rows.begin(); it != rows.end(); ++it)
            {
                if (!counted(*it))
                    continue;
                if (weakest == rows.end() || (permanent ? (it->Score < weakest->Score ||
                                                           (it->Score == weakest->Score && it->Time < weakest->Time))
                                                        : it->Expires < weakest->Expires))
                    weakest = it;
            }
            removed.push_back(std::move(*weakest));
            rows.erase(weakest);
        }
    }

    return removed;
}

// The merge of older small recent rows of one zone into one stay row
struct MergePlan
{
    std::vector<std::size_t> Merged;  // indexes into the rows, ascending
    Row Stay;                         // without its event id
};

// The zone of the oldest mergeable row with at least MERGE_MIN mergeable rows: one stay at its first row's place and
// time that keeps the highest score, the latest expiry and the sum of the counts; none when no zone qualifies
inline std::optional<MergePlan> PlanMerge(std::vector<Row> const& rows, uint32_t now)
{
    auto const mergeable = [now](Row const& row)
    { return row.Expires && row.Score < SMALL_SCORE && row.Time + MERGE_AGE <= now; };
    std::unordered_map<uint32_t, std::size_t> perZone;
    for (Row const& row : rows)
        if (mergeable(row))
            ++perZone[row.Zone];

    std::optional<std::size_t> oldest;
    for (std::size_t index = 0; index < rows.size(); ++index)
        if (mergeable(rows[index]) && perZone[rows[index].Zone] >= MERGE_MIN &&
            (!oldest || rows[index].Time < rows[*oldest].Time))
            oldest = index;

    if (!oldest)
        return std::nullopt;

    Row const& first = rows[*oldest];
    MergePlan plan;
    uint32_t count = 0;
    plan.Stay.EventKind = Kind::Stay;
    plan.Stay.Time = first.Time;
    plan.Stay.Map = first.Map;
    plan.Stay.Zone = first.Zone;
    plan.Stay.Area = first.Area;
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        Row const& row = rows[index];
        if (!mergeable(row) || row.Zone != first.Zone)
            continue;

        plan.Merged.push_back(index);
        count += Count(row);
        plan.Stay.Score = std::max(plan.Stay.Score, row.Score);
        plan.Stay.Expires = std::max(plan.Stay.Expires, row.Expires);
    }
    plan.Stay.Facts = std::string(ROWS_FACT) + "=" + std::to_string(count) + ";";
    return plan;
}

// One id per shared event: the first participant's row allocates it, the others of the same kind and key (a creature,
// a fight) within SHARE_WINDOW reuse it. Not thread-safe: the manager holds its lock.
class ShareBook
{
public:
    // The id noted for (kind, key) within the window, else 0
    uint32_t Find(Kind kind, uint64_t key, uint32_t now) const
    {
        auto const found = _notes.find(KeyOf(kind, key));
        return found != _notes.end() && found->second.Time + SHARE_WINDOW >= now ? found->second.EventId : 0;
    }

    void Note(Kind kind, uint64_t key, uint32_t eventId, uint32_t now)
    {
        if (_notes.size() >= SHARE_PRUNE)
            for (auto it = _notes.begin(); it != _notes.end();)
                it = it->second.Time + SHARE_WINDOW < now ? _notes.erase(it) : std::next(it);

        _notes[KeyOf(kind, key)] = {eventId, now};
    }

    std::size_t Size() const { return _notes.size(); }

private:
    struct Entry
    {
        uint32_t EventId = 0;
        uint32_t Time = 0;
    };

    static uint64_t KeyOf(Kind kind, uint64_t key) { return (key << 8) ^ static_cast<uint64_t>(kind); }

    std::unordered_map<uint64_t, Entry> _notes;
};
}  // namespace RecordRules

#endif
