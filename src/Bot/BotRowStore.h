/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTROWSTORE_H
#define PLAYERBOTS_BOTROWSTORE_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

#include "BotStores.h"

// A bot store's rows in memory, one per bot (the traits, the profile; tests/bot-row-store-selftest.cpp is the proof).
// The owner loads every row at startup (world thread). Reads take the shared lock from any thread and never load or
// query. Every write takes the unique lock and comes from the world thread (the callers check it); a write that changes
// a row issues the Policy's asynchronous save under that lock, so the saves of one bot reach the DB queue in write
// order. Policy: static char const* Name() (the forget's name), static void Save(std::uint32_t guid, Row const& row)
// and, optionally, static void ForgetHook(std::uint32_t guid) for the owner's further memory of a forgotten bot.
template <typename Row, typename Policy>
class BotRowStore
{
public:
    static BotRowStore& Instance()
    {
        static BotRowStore instance;
        return instance;
    }

    // Any thread: a copy of the bot's row; none when it has none
    std::optional<Row> Find(std::uint32_t guid) const
    {
        std::shared_lock<std::shared_mutex> guard(_lock);
        auto const it = _rows.find(guid);
        if (it == _rows.end())
            return std::nullopt;

        return it->second;
    }

    // Any thread: read(row) under the shared lock, without a copy; fallback when the bot has no row
    template <typename Result, typename Reader>
    Result Read(std::uint32_t guid, Result fallback, Reader&& read) const
    {
        std::shared_lock<std::shared_mutex> guard(_lock);
        auto const it = _rows.find(guid);
        if (it == _rows.end())
            return fallback;

        return read(it->second);
    }

    std::size_t Size() const
    {
        std::shared_lock<std::shared_mutex> guard(_lock);
        return _rows.size();
    }

    // The bot's row; when it has none, make() is stored and saved
    template <typename Make>
    Row GetOrCreate(std::uint32_t guid, Make&& make)
    {
        std::unique_lock<std::shared_mutex> guard(_lock);
        auto const it = _rows.find(guid);
        if (it != _rows.end())
            return it->second;

        Row const& row = _rows.emplace(guid, make()).first->second;
        Policy::Save(guid, row);
        return row;
    }

    // mutate(row) changes the bot's row, which is saved; none and no save when the bot has no row
    template <typename Mutate>
    std::optional<Row> Update(std::uint32_t guid, Mutate&& mutate)
    {
        std::unique_lock<std::shared_mutex> guard(_lock);
        auto const it = _rows.find(guid);
        if (it == _rows.end())
            return std::nullopt;

        mutate(it->second);
        Policy::Save(guid, it->second);
        return it->second;
    }

    // next(the bot's row, nullptr when it has none) gives its new row, which is stored and saved
    template <typename Next>
    Row Replace(std::uint32_t guid, Next&& next)
    {
        std::unique_lock<std::shared_mutex> guard(_lock);
        auto const it = _rows.find(guid);
        Row const* current = it == _rows.end() ? nullptr : &it->second;
        Row row = next(current);
        Row const& stored = _rows.insert_or_assign(guid, std::move(row)).first->second;
        Policy::Save(guid, stored);
        return stored;
    }

    // Startup load: one row as the DB holds it, not saved
    void LoadRow(std::uint32_t guid, Row row)
    {
        std::unique_lock<std::shared_mutex> guard(_lock);
        _rows.insert_or_assign(guid, std::move(row));
    }

    // Drops the bot's row from memory; the DB row is the caller's
    void Erase(std::uint32_t guid)
    {
        std::unique_lock<std::shared_mutex> guard(_lock);
        _rows.erase(guid);
    }

    // After the load: a bot's delete drops its row, then runs the Policy's ForgetHook (BotStores, in load order)
    void RegisterForget() { BotStores::RegisterForget(Policy::Name(), &Forget); }

private:
    BotRowStore() = default;

    static void Forget(std::uint32_t guid)
    {
        Instance().Erase(guid);
        if constexpr (requires { Policy::ForgetHook(guid); })
            Policy::ForgetHook(guid);
    }

    mutable std::shared_mutex _lock;
    std::unordered_map<std::uint32_t, Row> _rows;
};

#endif
