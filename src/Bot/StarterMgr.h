/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_STARTERMGR_H
#define PLAYERBOTS_STARTERMGR_H

#include <atomic>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"
#include "StarterRules.h"

class Player;

// Conversation starters (living-conversation LC28a): an event of a bot reaches StarterRules where a real player
// perceives the bot or shares its group. The real players online are a short list kept at login and logout, so no
// bot is scanned; with none online an event costs one atomic load. Offered starters are a world-log row `Starter` and
// an entry in the listener's bounded queue (read by the chat module from LC28b on); a dropped one is a `StarterDrop`
// row with its reason. Thread-safe: events come from the map threads, the list changes on the world thread.
class StarterMgr
{
public:
    static StarterMgr& Instance()
    {
        static StarterMgr instance;
        return instance;
    }

    void OnLogin(Player* player);
    void OnLogout(Player* player);
    // An event of a starter type (EventTypes.h), source, id and amount as the event row has them; a first visit comes
    // as EventType::Visit
    void OnEvent(Player* bot, EventLog::EventType type, std::string_view source, uint32 id, int64 amount);
    // Any thread: a real player is online (one atomic load)
    bool Online() const { return _online.load(std::memory_order_acquire) != 0; }
    // A copy of the starters offered to one real player, oldest first
    std::vector<StarterRules::Entry> Queued(ObjectGuid listener) const;

private:
    StarterMgr() = default;

    std::atomic<uint32> _online{0};
    mutable std::mutex _lock;
    std::vector<ObjectGuid> _listeners;
    StarterRules::Book _book;
    std::unordered_map<ObjectGuid::LowType, StarterRules::Queue> _queues;
};

#endif
