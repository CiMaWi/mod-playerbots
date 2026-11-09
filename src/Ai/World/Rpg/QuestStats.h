/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTSTATS_H
#define _PLAYERBOT_QUESTSTATS_H

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "PlannerIndexes.h"

// The population's quest statistics (travel plan WT13a, PLAN 3.9, WT.12): per quest and level band, how many bots
// accepted, turned in and abandoned it, how often they died on it and how long accept to turn-in took. Recorded from
// the bots' quest events (map threads, O(1) under one short lock), written as totals by the world thread and read by
// planners through an immutable view, like a guide a player reads. No game header: tests/quest-index-selftest.cpp
// proves the rules and the book
namespace Planner
{
constexpr std::uint32_t LEVEL_BAND_LEVELS = 5;  // a band is five levels: band 0 = levels 0-4, band 1 = 5-9, ...
constexpr std::size_t MINUTE_BUCKETS = 16;
// The top of each accept-to-turn-in bucket in minutes (log scale); a sample above the last is not recorded
constexpr std::array<std::uint32_t, MINUTE_BUCKETS> MINUTE_BUCKET_TOPS = {1,  2,  3,  5,  7,   10,  15,  20,
                                                                          30, 45, 60, 90, 120, 150, 180, 240};
constexpr std::uint64_t MS_PER_MINUTE = 60000;
// A longer accept-to-turn-in (a logout between, a lost start) counts the turn-in but adds no minutes sample
constexpr std::uint64_t SAMPLE_CAP_MS = MINUTE_BUCKET_TOPS[MINUTE_BUCKETS - 1] * MS_PER_MINUTE;
// Open quests (bot, quest) older than this are forgotten at the next flush: retired or logged-out bots
constexpr std::uint64_t OPEN_QUEST_MAX_AGE_MS = SAMPLE_CAP_MS;
// Open quests kept at most; beyond, an accept counts but keeps no start
constexpr std::size_t OPEN_QUESTS_MAX = 200000;

inline std::uint8_t LevelBandOf(std::uint32_t level)
{
    std::uint32_t const band = level / LEVEL_BAND_LEVELS;
    return static_cast<std::uint8_t>(band > 0xFFu ? 0xFFu : band);
}

inline std::uint64_t StatKey(std::uint32_t questId, std::uint8_t band)
{
    return (static_cast<std::uint64_t>(questId) << 8) | band;
}

inline std::uint32_t StatQuest(std::uint64_t key) { return static_cast<std::uint32_t>(key >> 8); }

inline std::uint8_t StatBand(std::uint64_t key) { return static_cast<std::uint8_t>(key & 0xFFu); }

struct QuestStatCell
{
    std::uint32_t accepted{0};
    std::uint32_t completed{0};  // turned in
    std::uint32_t abandoned{0};  // given up (no progress, cannot turn in) or dropped from the log
    std::uint32_t deaths{0};     // while the bot's rpg status was doing the quest
    std::array<std::uint32_t, MINUTE_BUCKETS> minutes{};

    bool operator==(QuestStatCell const& other) const = default;
};

// The bucket of an accept-to-turn-in time; MINUTE_BUCKETS when it is above the cap
inline std::size_t MinuteBucket(std::uint64_t ms)
{
    for (std::size_t i = 0; i < MINUTE_BUCKETS; ++i)
        if (ms <= MINUTE_BUCKET_TOPS[i] * MS_PER_MINUTE)
            return i;
    return MINUTE_BUCKETS;
}

// False (and nothing recorded) when the sample is above the cap
inline bool AddSample(QuestStatCell& cell, std::uint64_t ms)
{
    std::size_t const bucket = MinuteBucket(ms);
    if (bucket >= MINUTE_BUCKETS)
        return false;
    ++cell.minutes[bucket];
    return true;
}

inline std::uint32_t Samples(QuestStatCell const& cell)
{
    std::uint32_t n = 0;
    for (std::uint32_t const count : cell.minutes)
        n += count;
    return n;
}

// The median accept-to-turn-in time, linear inside its bucket; none without a sample
inline std::optional<float> MedianMinutes(QuestStatCell const& cell)
{
    std::uint32_t const n = Samples(cell);
    if (!n)
        return std::nullopt;
    float const half = 0.5f * static_cast<float>(n);
    float before = 0.0f;
    for (std::size_t i = 0; i < MINUTE_BUCKETS; ++i)
    {
        float const count = static_cast<float>(cell.minutes[i]);
        if (count > 0.0f && before + count >= half)
        {
            float const low = i ? static_cast<float>(MINUTE_BUCKET_TOPS[i - 1]) : 0.0f;
            float const high = static_cast<float>(MINUTE_BUCKET_TOPS[i]);
            return low + (high - low) * (half - before) / count;
        }
        before += count;
    }
    return static_cast<float>(MINUTE_BUCKET_TOPS[MINUTE_BUCKETS - 1]);
}

// Deaths per turn-in; none before the first turn-in
inline std::optional<float> DeathsPerCompletion(QuestStatCell const& cell)
{
    if (!cell.completed)
        return std::nullopt;
    return static_cast<float>(cell.deaths) / static_cast<float>(cell.completed);
}

inline void Merge(QuestStatCell& into, QuestStatCell const& from)
{
    into.accepted += from.accepted;
    into.completed += from.completed;
    into.abandoned += from.abandoned;
    into.deaths += from.deaths;
    for (std::size_t i = 0; i < MINUTE_BUCKETS; ++i)
        into.minutes[i] += from.minutes[i];
}

// The `minutes` column: the bucket counts separated by commas
inline std::string EncodeMinutes(QuestStatCell const& cell)
{
    std::string text;
    for (std::size_t i = 0; i < MINUTE_BUCKETS; ++i)
    {
        if (i)
            text += ',';
        text += std::to_string(cell.minutes[i]);
    }
    return text;
}

// False (cell untouched) unless the text holds MINUTE_BUCKETS counts; an empty text is no sample
inline bool DecodeMinutes(std::string_view text, QuestStatCell& cell)
{
    std::array<std::uint32_t, MINUTE_BUCKETS> counts{};
    if (text.empty())
    {
        cell.minutes = counts;
        return true;
    }
    char const* at = text.data();
    char const* const end = text.data() + text.size();
    for (std::size_t i = 0; i < MINUTE_BUCKETS; ++i)
    {
        if (i)
        {
            if (at >= end || *at != ',')
                return false;
            ++at;
        }
        auto const [next, error] = std::from_chars(at, end, counts[i]);
        if (error != std::errc())
            return false;
        at = next;
    }
    if (at != end)
        return false;
    cell.minutes = counts;
    return true;
}

// What a planner reads per quest and band
struct QuestStatSummary
{
    std::uint32_t accepted{0};
    std::uint32_t completed{0};
    std::uint32_t abandoned{0};
    std::uint32_t deaths{0};
    std::uint32_t samples{0};
    float medianMinutes{-1.0f};        // -1: no sample
    float deathsPerCompletion{-1.0f};  // -1: no turn-in
};

inline QuestStatSummary Summarize(QuestStatCell const& cell)
{
    QuestStatSummary s;
    s.accepted = cell.accepted;
    s.completed = cell.completed;
    s.abandoned = cell.abandoned;
    s.deaths = cell.deaths;
    s.samples = Samples(cell);
    s.medianMinutes = MedianMinutes(cell).value_or(-1.0f);
    s.deathsPerCompletion = DeathsPerCompletion(cell).value_or(-1.0f);
    return s;
}

// Immutable, published after the load and after each flush; workers read it through QuestStats()
class QuestStatsView
{
public:
    std::unordered_map<std::uint64_t, QuestStatSummary> rows;  // StatKey

    QuestStatSummary const* Find(std::uint32_t questId, std::uint8_t band) const
    {
        auto const it = rows.find(StatKey(questId, band));
        return it == rows.end() ? nullptr : &it->second;
    }
};

using QuestStatsSlot = SharedSlot<QuestStatsView>;

struct QuestBookCounts
{
    std::size_t cells{0};
    std::size_t dirty{0};
    std::size_t open{0};
    std::uint64_t accepts{0};
    std::uint64_t turnIns{0};
    std::uint64_t abandons{0};
    std::uint64_t drops{0};
    std::uint64_t deaths{0};
    std::uint64_t samples{0};
    std::uint64_t unsampled{0};  // turn-ins without a start or above the cap
    std::uint64_t pruned{0};     // open quests forgotten by age
    std::uint64_t full{0};       // accepts that kept no start (OPEN_QUESTS_MAX)
};

// The live book: every event is O(1) under one mutex (events are rare: a bot accepts or ends a quest every few
// minutes); the world thread takes the dirty cells to write and builds the view
class QuestStatsBook
{
public:
    void Accept(std::uint32_t bot, std::uint32_t questId, std::uint32_t level, std::uint64_t nowMs)
    {
        std::uint8_t const band = LevelBandOf(level);
        std::lock_guard<std::mutex> lock(_mutex);
        ++_counts.accepts;
        ++Touch(questId, band).accepted;
        std::uint64_t const key = OpenKey(bot, questId);
        auto const it = _open.find(key);
        if (it != _open.end())
            it->second = {nowMs, band, false};
        else if (_open.size() < OPEN_QUESTS_MAX)
            _open.emplace(key, OpenQuest{nowMs, band, false});
        else
            ++_counts.full;
    }

    // A turn-in: a completion in the band of the accept, with a minutes sample when the start is known and recent
    void Reward(std::uint32_t bot, std::uint32_t questId, std::uint32_t level, std::uint64_t nowMs)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_counts.turnIns;
        auto const it = _open.find(OpenKey(bot, questId));
        std::uint8_t const band = it != _open.end() ? it->second.band : LevelBandOf(level);
        QuestStatCell& cell = Touch(questId, band);
        ++cell.completed;
        bool const sampled = it != _open.end() && it->second.started && nowMs >= it->second.startMs &&
                             AddSample(cell, nowMs - it->second.startMs);
        ++(sampled ? _counts.samples : _counts.unsampled);
        if (it != _open.end())
            _open.erase(it);
    }

    // Given up while still in the log: counts once; the quest stays open, marked, so the drop that usually follows
    // does not count again
    void Abandon(std::uint32_t bot, std::uint32_t questId, std::uint32_t level, std::uint64_t nowMs)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::uint64_t const key = OpenKey(bot, questId);
        auto it = _open.find(key);
        if (it != _open.end() && it->second.abandoned)
            return;
        ++_counts.abandons;
        if (it == _open.end())
        {
            if (_open.size() >= OPEN_QUESTS_MAX)
            {
                ++Touch(questId, LevelBandOf(level)).abandoned;
                return;
            }
            // No start known (accepted before this server start): keep the mark without a start
            it = _open.emplace(key, OpenQuest{nowMs, LevelBandOf(level), true, false}).first;
        }
        it->second.abandoned = true;
        ++Touch(questId, it->second.band).abandoned;
    }

    // Removed from the log: an abandon unless Abandon counted it already; closes the open quest
    void Drop(std::uint32_t bot, std::uint32_t questId, std::uint32_t level)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_counts.drops;
        auto const it = _open.find(OpenKey(bot, questId));
        if (it == _open.end())
        {
            ++Touch(questId, LevelBandOf(level)).abandoned;
            return;
        }
        if (!it->second.abandoned)
            ++Touch(questId, it->second.band).abandoned;
        _open.erase(it);
    }

    void Death(std::uint32_t bot, std::uint32_t questId, std::uint32_t level)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        ++_counts.deaths;
        auto const it = _open.find(OpenKey(bot, questId));
        ++Touch(questId, it != _open.end() ? it->second.band : LevelBandOf(level)).deaths;
    }

    // Startup: the stored totals (not dirty)
    void Restore(std::uint32_t questId, std::uint8_t band, QuestStatCell const& cell)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _cells[StatKey(questId, band)] = cell;
    }

    // The flush: copies of the cells changed since the last call (their totals), and forgets open quests older
    // than OPEN_QUEST_MAX_AGE_MS
    std::vector<std::pair<std::uint64_t, QuestStatCell>> TakeDirty(std::uint64_t nowMs)
    {
        std::vector<std::pair<std::uint64_t, QuestStatCell>> rows;
        std::lock_guard<std::mutex> lock(_mutex);
        rows.reserve(_dirty.size());
        for (std::uint64_t const key : _dirty)
            rows.emplace_back(key, _cells[key]);
        _dirty.clear();
        for (auto it = _open.begin(); it != _open.end();)
            if (nowMs > it->second.startMs + OPEN_QUEST_MAX_AGE_MS)
            {
                it = _open.erase(it);
                ++_counts.pruned;
            }
            else
                ++it;
        return rows;
    }

    // A copy of every cell taken under the lock, summarised outside it
    std::shared_ptr<QuestStatsView const> View() const
    {
        std::vector<std::pair<std::uint64_t, QuestStatCell>> cells;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            cells.assign(_cells.begin(), _cells.end());
        }
        auto view = std::make_shared<QuestStatsView>();
        view->rows.reserve(cells.size());
        for (auto const& [key, cell] : cells)
            view->rows.emplace(key, Summarize(cell));
        return view;
    }

    // The cells of one quest, by band (the GM query)
    std::vector<std::pair<std::uint8_t, QuestStatCell>> CellsOf(std::uint32_t questId) const
    {
        std::vector<std::pair<std::uint8_t, QuestStatCell>> found;
        std::lock_guard<std::mutex> lock(_mutex);
        for (std::uint32_t band = 0; band <= 0xFFu; ++band)
        {
            auto const it = _cells.find(StatKey(questId, static_cast<std::uint8_t>(band)));
            if (it != _cells.end())
                found.emplace_back(static_cast<std::uint8_t>(band), it->second);
        }
        return found;
    }

    QuestBookCounts Counts() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        QuestBookCounts counts = _counts;
        counts.cells = _cells.size();
        counts.dirty = _dirty.size();
        counts.open = _open.size();
        return counts;
    }

private:
    struct OpenQuest
    {
        std::uint64_t startMs{0};
        std::uint8_t band{0};
        bool abandoned{false};
        bool started{true};  // false: the mark of an abandon whose accept this server did not see
    };

    static std::uint64_t OpenKey(std::uint32_t bot, std::uint32_t questId)
    {
        return (static_cast<std::uint64_t>(bot) << 32) | questId;
    }

    // Caller holds the lock
    QuestStatCell& Touch(std::uint32_t questId, std::uint8_t band)
    {
        std::uint64_t const key = StatKey(questId, band);
        _dirty.insert(key);
        return _cells[key];
    }

    mutable std::mutex _mutex;
    std::unordered_map<std::uint64_t, QuestStatCell> _cells;
    std::unordered_set<std::uint64_t> _dirty;
    std::unordered_map<std::uint64_t, OpenQuest> _open;
    QuestBookCounts _counts;
};
}  // namespace Planner

#endif
