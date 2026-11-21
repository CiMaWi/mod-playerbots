/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "StarterMgr.h"

#include <algorithm>
#include <chrono>
#include <iterator>

#include "BridgePerception.h"
#include "BridgeTexts.h"
#include "EventLog.h"
#include "EventLogFormat.h"
#include "EventWanted.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"

namespace
{
using StarterRules::Kind;

static_assert(std::size(BridgeTexts::Starters::SENTENCES) == StarterRules::KIND_COUNT, "a starter sentence per kind");

uint64 NowMs()
{
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint8 RankOf(std::string_view word)
{
    for (uint32 rank = 0; rank < EventLog::RANK_COUNT; ++rank)
        if (word == EventLog::RANK_WORDS[rank])
            return static_cast<uint8>(rank);

    return RecordRules::RANK_NORMAL;
}

StarterRules::Facts FactsOf(Kind kind, std::string_view source, uint32 id, int64 amount)
{
    StarterRules::Facts facts;
    switch (kind)
    {
        case Kind::Loot:
            // An Equip event is an item the bot now wears; one in the bag is no starter
            facts.Worn = true;
            facts.HandedOut = RecordRules::HandedOut(EventLog::SourceWordOf(EventLog::CurrentSource()));
            facts.Record.ItemLevelGain = static_cast<int32>(std::clamp<int64>(amount, -1000, 1000));
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id))
                facts.Record.Quality = static_cast<uint8>(proto->Quality);
            break;
        case Kind::Kill:
            facts.Record.Rank = RankOf(source);
            facts.Record.LevelDiff = static_cast<int32>(std::clamp<int64>(amount, -100, 100));
            break;
        case Kind::Level:
            facts.HandedOut = RecordRules::HandedOut(EventLog::SourceWordOf(EventLog::CurrentSource()));
            facts.Record.Level = id;
            facts.LevelUp = static_cast<int64>(id) > amount;
            break;
        case Kind::Visit:
            facts.First = true;
            break;
        case Kind::Need:
            facts.Begun = id == 1;
            break;
        default:
            break;
    }

    return facts;
}
}  // namespace

void StarterMgr::OnLogin(Player* player)
{
    if (!player || !IsRealPlayer(player))
        return;

    std::lock_guard<std::mutex> guard(_lock);
    if (std::find(_listeners.begin(), _listeners.end(), player->GetGUID()) == _listeners.end())
        _listeners.push_back(player->GetGUID());
    _online.store(static_cast<uint32>(_listeners.size()), std::memory_order_release);
}

void StarterMgr::OnLogout(Player* player)
{
    if (!player)
        return;

    std::lock_guard<std::mutex> guard(_lock);
    _listeners.erase(std::remove(_listeners.begin(), _listeners.end(), player->GetGUID()), _listeners.end());
    _queues.erase(player->GetGUID().GetCounter());
    _online.store(static_cast<uint32>(_listeners.size()), std::memory_order_release);
}

bool Starters::Online() { return StarterMgr::Instance().Online(); }

void StarterMgr::OnEvent(Player* bot, EventLog::EventType type, std::string_view source, uint32 id, int64 amount)
{
    if (!Online() || !bot)
        return;

    StarterRules::Row const* row = StarterRules::RowFor(type);
    if (!row || IsRealPlayer(bot) || !bot->IsInWorld())
        return;

    std::vector<ObjectGuid> listeners;
    {
        std::lock_guard<std::mutex> guard(_lock);
        listeners = _listeners;
    }

    // A real player in the bot's group hears it in the party wherever it is; else the first one that perceives it
    StarterRules::Seen seen;
    ObjectGuid heard;
    Group* const group = bot->GetGroup();
    for (ObjectGuid const& guid : listeners)
    {
        Player* const player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player || !player->IsInWorld())
            continue;

        if (group && group->IsMember(guid))
        {
            seen.Perceived = true;
            seen.Where = StarterRules::Scope::Party;
            seen.ScopeKey = group->GetGUID().GetCounter();
            heard = guid;
            break;
        }

        if (!seen.Perceived && PlayerbotBridge::Perceives(player, bot))
        {
            seen.Perceived = true;
            seen.Where = StarterRules::Scope::Say;
            seen.ScopeKey = bot->GetGUID().GetCounter();
            heard = guid;
        }
    }

    StarterRules::Facts const facts = FactsOf(row->StarterKind, source, id, amount);
    uint32 const botId = bot->GetGUID().GetCounter();
    uint64 const now = NowMs();
    StarterRules::Verdict verdict;
    {
        std::lock_guard<std::mutex> guard(_lock);
        verdict = StarterRules::Decide(*row, facts, botId, seen, now, _book);
        if (verdict.Result == StarterRules::Outcome::Offer)
        {
            _book.Note(botId, seen, row->StarterKind, now);
            // The listener may have logged out since the copy: its queue then waits for nobody
            if (std::find(_listeners.begin(), _listeners.end(), heard) != _listeners.end())
                _queues[heard.GetCounter()].Push({botId, row->StarterKind, verdict.Salience, seen.Where, now});
        }
    }

    // The rows go out after the lock: Emit comes back through WorldLogWriter::OnEvent, which finds no row for them
    if (verdict.Result == StarterRules::Outcome::Offer)
        EventLog::Emit(bot, EventLog::EventType::Starter, StarterRules::Word(row->StarterKind), verdict.Salience,
                       static_cast<int64>(seen.Where));
    else if (verdict.Result == StarterRules::Outcome::Drop)
        EventLog::Emit(bot, EventLog::EventType::StarterDrop, StarterRules::Word(verdict.Why), verdict.Salience,
                       static_cast<int64>(row->StarterKind));
}

std::vector<StarterRules::Entry> StarterMgr::Queued(ObjectGuid listener) const
{
    std::lock_guard<std::mutex> guard(_lock);
    auto const found = _queues.find(listener.GetCounter());
    if (found == _queues.end())
        return {};

    return {found->second.Entries().begin(), found->second.Entries().end()};
}
