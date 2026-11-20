/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_QUESTFAILBOOK_H
#define _PLAYERBOT_QUESTFAILBOOK_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The failure record per bot and quest (questing WT35c; table playerbots_bot_quest_fail): how often the bot gave the
// quest up, the last cause, the minutes it pursued it and when. The book is the live copy: a give-up on a map thread
// is O(1) under one mutex (give-ups are rare), the world thread loads it once and writes the changed rows
// (QuestFailRecord.cpp). Game-free, for the selftest
namespace QuestFail
{
constexpr std::size_t CAUSE_MAX = 32;  // the last_cause column's width

struct Row
{
    std::uint32_t giveUps{0};
    std::string lastCause;
    std::uint32_t minutes{0};   // pursued over every give-up
    std::uint32_t lastTime{0};  // the last give-up, unix seconds

    bool operator==(Row const& other) const
    {
        return giveUps == other.giveUps && lastCause == other.lastCause && minutes == other.minutes &&
               lastTime == other.lastTime;
    }
};

struct Written
{
    std::uint32_t guid;
    std::uint32_t quest;
    Row row;
};

// A cause word as stored: lower-case letters, digits, '_' and ':' only, at most CAUSE_MAX long
inline std::string CleanCause(std::string_view word)
{
    std::string clean;
    for (char const c : word)
    {
        if (clean.size() >= CAUSE_MAX)
            break;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':')
            clean += c;
    }
    return clean;
}

// One give-up added to a row
inline void Merge(Row& row, std::string_view cause, std::uint32_t minutes, std::uint32_t now)
{
    ++row.giveUps;
    row.lastCause = CleanCause(cause);
    row.minutes += minutes;
    row.lastTime = now;
}

// The row's write with explicit totals: a repeated or late write leaves the same row
inline std::string UpsertText(Written const& written)
{
    std::string const giveUps = std::to_string(written.row.giveUps);
    std::string const cause = "'" + CleanCause(written.row.lastCause) + "'";
    std::string const minutes = std::to_string(written.row.minutes);
    std::string const time = std::to_string(written.row.lastTime);
    return "INSERT INTO playerbots_bot_quest_fail (guid, quest_id, give_ups, last_cause, minutes, last_time) VALUES (" +
           std::to_string(written.guid) + ", " + std::to_string(written.quest) + ", " + giveUps + ", " + cause + ", " +
           minutes + ", " + time + ") ON DUPLICATE KEY UPDATE give_ups = " + giveUps + ", last_cause = " + cause +
           ", minutes = " + minutes + ", last_time = " + time;
}

// The bot's quests given up `limit` times or more, sorted: the planner's givenUpQuests and the quests the picker skips
inline std::vector<std::uint32_t> GivenUp(std::vector<std::pair<std::uint32_t, Row>> const& rows, std::uint32_t limit)
{
    std::vector<std::uint32_t> quests;
    for (auto const& [quest, row] : rows)
        if (row.giveUps >= limit)
            quests.push_back(quest);
    std::sort(quests.begin(), quests.end());
    return quests;
}

inline bool IsGivenUp(std::vector<std::uint32_t> const& givenUp, std::uint32_t quest)
{
    return std::binary_search(givenUp.begin(), givenUp.end(), quest);
}

class Book
{
public:
    // A give-up (map threads)
    void Note(std::uint32_t guid, std::uint32_t quest, std::string_view cause, std::uint32_t minutes, std::uint32_t now)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        Merge(_rows[guid][quest], cause, minutes, now);
        _dirty.insert(KeyOf(guid, quest));
    }

    // Startup: a stored row (not dirty)
    void Restore(std::uint32_t guid, std::uint32_t quest, Row row)
    {
        row.lastCause = CleanCause(row.lastCause);
        std::lock_guard<std::mutex> lock(_mutex);
        _rows[guid][quest] = std::move(row);
    }

    // The flush: copies of the rows changed since the last call
    std::vector<Written> TakeDirty()
    {
        std::vector<Written> written;
        std::lock_guard<std::mutex> lock(_mutex);
        written.reserve(_dirty.size());
        for (std::uint64_t const key : _dirty)
        {
            std::uint32_t const guid = static_cast<std::uint32_t>(key >> 32);
            std::uint32_t const quest = static_cast<std::uint32_t>(key & 0xFFFFFFFFu);
            auto const bot = _rows.find(guid);
            if (bot == _rows.end())
                continue;
            auto const row = bot->second.find(quest);
            if (row != bot->second.end())
                written.push_back({guid, quest, row->second});
        }
        _dirty.clear();
        return written;
    }

    // A deleted bot: its rows and their pending writes go, so nothing writes them back
    void Forget(std::uint32_t guid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto const bot = _rows.find(guid);
        if (bot == _rows.end())
            return;
        for (auto const& entry : bot->second)
            _dirty.erase(KeyOf(guid, entry.first));
        _rows.erase(bot);
    }

    std::optional<Row> Find(std::uint32_t guid, std::uint32_t quest) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto const bot = _rows.find(guid);
        if (bot == _rows.end())
            return std::nullopt;
        auto const row = bot->second.find(quest);
        if (row == bot->second.end())
            return std::nullopt;
        return row->second;
    }

    // A bot's rows, by quest (copies)
    std::vector<std::pair<std::uint32_t, Row>> RowsOf(std::uint32_t guid) const
    {
        std::vector<std::pair<std::uint32_t, Row>> rows;
        std::lock_guard<std::mutex> lock(_mutex);
        auto const bot = _rows.find(guid);
        if (bot != _rows.end())
            rows.assign(bot->second.begin(), bot->second.end());
        return rows;
    }

    std::pair<std::size_t, std::size_t> Size() const  // bots, rows
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::size_t rows = 0;
        for (auto const& bot : _rows)
            rows += bot.second.size();
        return {_rows.size(), rows};
    }

private:
    static std::uint64_t KeyOf(std::uint32_t guid, std::uint32_t quest)
    {
        return (static_cast<std::uint64_t>(guid) << 32) | quest;
    }

    mutable std::mutex _mutex;
    std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, Row>> _rows;
    std::unordered_set<std::uint64_t> _dirty;
};
}  // namespace QuestFail

#endif
