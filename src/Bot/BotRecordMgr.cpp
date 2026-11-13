/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotRecordMgr.h"

#include <algorithm>
#include <sstream>

#include "CensusMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "EventLogFormat.h"
#include "EventWanted.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "ThreadCheck.h"
#include "World.h"

using RecordRules::Kind;
using RecordRules::Row;

namespace
{
constexpr char const* COLUMNS = "guid, event_id, kind, time, map, zone, area, score, others, facts, expires";
constexpr std::size_t TEXT_LIMIT = 255;
// The world log's own event type for a written row; the record never takes it back
constexpr std::string_view RECORD_TYPE = "Record";

uint32 Now() { return static_cast<uint32>(GameTime::GetGameTime().count()); }

bool IsBot(Player* player)
{
    PlayerbotAI* const botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
    return botAI && botAI->IsBotAI();
}

// "b<guid>" for a bot, "p<guid>" for a real player
std::string PersonToken(Player* player)
{
    return Acore::StringFormat("{}{}", IsBot(player) ? 'b' : 'p', player->GetGUID().GetCounter());
}

void AppendToken(std::string& out, std::string const& token)
{
    if (out.size() + token.size() + 1 > TEXT_LIMIT)
        return;
    if (!out.empty())
        out.push_back(' ');
    out += token;
}

uint8 RankOf(std::string_view word)
{
    for (uint32 rank = 0; rank < EventLog::RANK_COUNT; ++rank)
        if (word == EventLog::RANK_WORDS[rank])
            return static_cast<uint8>(rank);

    return RecordRules::RANK_NORMAL;
}

bool TeachesMount(uint32 spellId)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return false;

    for (SpellEffectInfo const& effect : info->Effects)
        if (effect.ApplyAuraName == SPELL_AURA_MOUNTED ||
            (effect.Effect == SPELL_EFFECT_SKILL && effect.MiscValue == SKILL_RIDING))
            return true;

    return false;
}

bool IsCraft(uint32 skillId)
{
    SkillLineEntry const* line = sSkillLineStore.LookupEntry(skillId);
    return line && (line->categoryId == SKILL_CATEGORY_PROFESSION || line->categoryId == SKILL_CATEGORY_SECONDARY);
}

std::string ZoneName(uint32 zone)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
    return area ? area->area_name[sWorld->GetDefaultDbcLocale()] : "an unknown place";
}

std::string InsertStatement(uint32 guid, Row const& row)
{
    return Acore::StringFormat(
        "INSERT IGNORE INTO playerbots_bot_record ({}) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, "
        "'{}', '{}', {})",
        COLUMNS, guid, row.EventId, static_cast<uint32>(row.EventKind), row.Time, row.Map, row.Zone, row.Area,
        static_cast<uint32>(row.Score), row.Others, row.Facts, row.Expires);
}

std::string DeleteStatement(uint32 guid, Row const& row)
{
    return Acore::StringFormat("DELETE FROM playerbots_bot_record WHERE guid = {} AND event_id = {} AND kind = {}",
                               guid, row.EventId, static_cast<uint32>(row.EventKind));
}
}  // namespace

bool BotRecord::Active() { return BotRecordMgr::Instance().Active(); }

void BotRecordMgr::Load()
{
    std::lock_guard<std::mutex> guard(_lock);
    if (_active.load(std::memory_order_relaxed))
        return;

    // One query for all bots (startup, world thread); a server with Playerbots.Updates.EnableDatabases = 0 may lack the
    // update: then the record lives in memory only
    _table = PlayerbotsDatabase.Query(
                 "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND "
                 "TABLE_NAME = 'playerbots_bot_record'") != nullptr;
    std::size_t count = 0;
    if (!_table)
        LOG_WARN("playerbots",
                 "Bot record: no table playerbots_bot_record (update 2026_11_14_00 not applied): the "
                 "record is kept in memory only");
    else
    {
        if (QueryResult result = PlayerbotsDatabase.Query("SELECT MAX(event_id) FROM playerbots_bot_record"))
            _lastEventId = result->Fetch()[0].Get<uint32>();

        uint32 const now = Now();
        if (QueryResult result = PlayerbotsDatabase.Query(Acore::StringFormat(
                "SELECT {} FROM playerbots_bot_record WHERE expires = 0 OR expires > {}", COLUMNS, now)))
        {
            do
            {
                Field* fields = result->Fetch();
                std::optional<Kind> const kind = RecordRules::KindOf(fields[2].Get<uint8>());
                if (!kind)
                    continue;

                Row row;
                row.EventId = fields[1].Get<uint32>();
                row.EventKind = *kind;
                row.Time = fields[3].Get<uint32>();
                row.Map = fields[4].Get<uint32>();
                row.Zone = fields[5].Get<uint32>();
                row.Area = fields[6].Get<uint32>();
                row.Score = fields[7].Get<uint8>();
                row.Others = fields[8].Get<std::string>();
                row.Facts = fields[9].Get<std::string>();
                row.Expires = fields[10].Get<uint32>();
                _rows[fields[0].Get<uint32>()].push_back(std::move(row));
                ++count;
            } while (result->NextRow());
        }

        PlayerbotsDatabase.Execute(
            Acore::StringFormat("DELETE FROM playerbots_bot_record WHERE expires <> 0 AND expires <= {}", now));
    }

    _active.store(true, std::memory_order_release);
    LOG_INFO("playerbots", "Bot record loaded: {} rows of {} bots, last event id {}", count, _rows.size(),
             _lastEventId);
}

void BotRecordMgr::OnEvent(Player* player, std::string_view type, std::string_view source, uint32 id, int64 amount)
{
    if (!player || !Active() || type.empty() || type == RECORD_TYPE)
        return;

    Kind kind = Kind::Kill;
    if (type == "Kill")
        kind = Kind::Kill;
    else if (type == "QuestDone")
        kind = Kind::Quest;
    else if (type == "Equip")
        kind = Kind::Equip;
    else if (type == "Level")
        kind = Kind::Level;
    else if (type == "SkillUp")
        kind = Kind::Skill;
    else if (type == "SpellLearn")
        kind = Kind::Spell;
    else if (type == "TaxiNode")
        kind = Kind::Flight;
    else if (type == "Death")
        kind = Kind::Death;
    else if (type == "Resurrect")
        kind = Kind::Resurrect;
    else if (type == "MapChange" && (source == "dungeon" || source == "raid"))
        kind = Kind::Dungeon;
    else
        return;

    if (!IsBot(player))
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "Record event");
    RecordRules::Facts facts;
    Event event;
    bool group = false;
    bool const handedOut = RecordRules::HandedOut(EventLog::SourceWordOf(EventLog::CurrentSource()));
    switch (kind)
    {
        case Kind::Kill:
            facts.Rank = RankOf(source);
            facts.LevelDiff = static_cast<int32>(std::clamp<int64>(amount, -100, 100));
            event.Facts = Acore::StringFormat("entry={};rank={};above={};", id, static_cast<uint32>(facts.Rank),
                                              std::max(facts.LevelDiff, 0));
            event.Others = Acore::StringFormat("n{}", id);
            group = true;
            break;
        case Kind::Quest:
            event.Facts = Acore::StringFormat("quest={};", id);
            break;
        case Kind::Equip:
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id))
                facts.Quality = static_cast<uint8>(proto->Quality);
            facts.ItemLevelGain = static_cast<int32>(std::clamp<int64>(amount, -1000, 1000));
            event.Facts = Acore::StringFormat("item={};quality={};gain={};", id, static_cast<uint32>(facts.Quality),
                                              std::max(facts.ItemLevelGain, 0));
            break;
        case Kind::Level:
            facts.Level = id;
            event.Facts = Acore::StringFormat("level={};", id);
            break;
        case Kind::Skill:
            facts.Craft = IsCraft(id);
            facts.Skill = static_cast<uint32>(std::max<int64>(amount, 0));
            event.Facts = Acore::StringFormat("skill={};value={};", id, facts.Skill);
            break;
        case Kind::Spell:
            facts.Mount = TeachesMount(id);
            event.Facts = Acore::StringFormat("spell={};", id);
            break;
        case Kind::Flight:
            event.Facts = Acore::StringFormat("node={};", id);
            break;
        case Kind::Death:
            event.Facts = Acore::StringFormat("by={};", source);
            if (source == "creature")
                event.Others = Acore::StringFormat("n{}", id);
            else if (source == "player" || source == "player_pet")
            {
                Player* const killer = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(id));
                event.Others = Acore::StringFormat("{}{}", killer && IsBot(killer) ? 'b' : 'p', id);
            }
            break;
        case Kind::Resurrect:
            event.Facts = Acore::StringFormat("how={};", source);
            break;
        case Kind::Dungeon:
            facts.Raid = source == "raid";
            event.Facts = Acore::StringFormat("map={};", id);
            break;
        default:
            break;
    }

    bool const playOnly = kind == Kind::Equip || kind == Kind::Level || kind == Kind::Skill || kind == Kind::Spell;
    event.EventKind = kind;
    event.Score = RecordRules::Score(kind, facts);
    if (!RecordRules::Kept(event.Score) || (playOnly && handedOut))
    {
        if (pmo)
            pmo->finish();
        return;
    }

    // A kill above the low mark is shared by the killer's group: its bots alive on the map within reward distance
    std::vector<Player*> participants = {player};
    if (Group* const party = group ? player->GetGroup() : nullptr)
        for (GroupReference* ref = party->GetFirstMember(); ref; ref = ref->next())
        {
            Player* const member = ref->GetSource();
            if (!member || member == player || !member->IsInWorld() || member->GetMap() != player->GetMap() ||
                !member->IsAlive() || !member->IsAtGroupRewardDistance(player))
                continue;

            AppendToken(event.Others, PersonToken(member));
            if (IsBot(member))
                participants.push_back(member);
        }

    Record(participants, event);
    if (pmo)
        pmo->finish();
}

void BotRecordMgr::OnFightEnd(Player* player, uint32 fightId, uint32 seconds, std::string_view cause,
                              std::string others)
{
    if (!player || !fightId || !Active() || !IsBot(player))
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_TOTAL, "Record event");
    Event event;
    event.EventKind = Kind::Fight;
    event.Score = RecordRules::Score(Kind::Fight, {});
    event.Facts = Acore::StringFormat("fight={};cause={};seconds={};", fightId, cause, seconds);
    event.Others = std::move(others);
    if (event.Others.size() > TEXT_LIMIT)
        event.Others.resize(event.Others.rfind(' ', TEXT_LIMIT));
    event.ShareKey = fightId;
    Record({player}, event);
    if (pmo)
        pmo->finish();
}

void BotRecordMgr::Record(std::vector<Player*> const& participants, Event const& event)
{
    uint32 const now = Now();
    std::vector<std::string> statements;
    uint32 eventId = 0;
    {
        std::lock_guard<std::mutex> guard(_lock);
        eventId = event.ShareKey ? _share.Find(event.EventKind, event.ShareKey, now) : 0;
        if (!eventId)
        {
            eventId = ++_lastEventId;
            if (event.ShareKey)
                _share.Note(event.EventKind, event.ShareKey, eventId, now);
        }

        for (Player* const participant : participants)
        {
            Row row;
            row.EventId = eventId;
            row.EventKind = event.EventKind;
            row.Time = now;
            row.Map = participant->GetMapId();
            row.Zone = participant->GetZoneId();
            row.Area = participant->GetAreaId();
            row.Score = event.Score;
            row.Others = event.Others;
            row.Facts = event.Facts;
            row.Expires = RecordRules::Expires(event.Score, now);
            InsertLocked(participant->GetGUID().GetCounter(), std::move(row), now, statements);
        }
    }

    if (_table)
        for (std::string const& statement : statements)
            PlayerbotsDatabase.Execute(statement);

    char const* const word = RecordRules::Word(event.EventKind);
    for (Player* const participant : participants)
        EventLog::Emit(participant, "Record", word, eventId, event.Score);
}

void BotRecordMgr::InsertLocked(uint32 guid, Row row, uint32 now, std::vector<std::string>& statements)
{
    std::vector<Row>& rows = _rows[guid];
    for (Row const& existing : rows)
        if (existing.EventId == row.EventId && existing.EventKind == row.EventKind)
            return;

    statements.push_back(InsertStatement(guid, row));
    rows.push_back(std::move(row));
    for (Row const& removed : RecordRules::Prune(rows, now))
        statements.push_back(DeleteStatement(guid, removed));

    std::optional<RecordRules::MergePlan> plan = RecordRules::PlanMerge(rows, now);
    if (!plan)
        return;

    for (auto it = plan->Merged.rbegin(); it != plan->Merged.rend(); ++it)
    {
        statements.push_back(DeleteStatement(guid, rows[*it]));
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(*it));
    }

    plan->Stay.EventId = ++_lastEventId;
    statements.push_back(InsertStatement(guid, plan->Stay));
    rows.push_back(std::move(plan->Stay));
}

std::vector<Row> BotRecordMgr::Rows(uint32 guid)
{
    std::lock_guard<std::mutex> guard(_lock);
    auto const found = _rows.find(guid);
    return found == _rows.end() ? std::vector<Row>() : found->second;
}

void BotRecordMgr::Forget(uint32 guid)
{
    PLAYERBOT_CHECK_WORLD_THREAD("BotRecordMgr::Forget");
    std::lock_guard<std::mutex> guard(_lock);
    _rows.erase(guid);
}

bool BotRecordMgr::HandleRecordCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string target;
    in >> target;
    BotRecordMgr& mgr = Instance();
    if (target.empty())
    {
        std::size_t rows = 0;
        std::size_t bots = 0;
        bool table = false;
        {
            std::lock_guard<std::mutex> guard(mgr._lock);
            bots = mgr._rows.size();
            for (auto const& [guid, list] : mgr._rows)
                rows += list.size();
            table = mgr._table;
        }

        handler->PSendSysMessage("Bot record: {} rows of {} bots, table {}, {}", rows, bots,
                                 table ? "present" : "missing", mgr.Active() ? "taking events" : "not loaded");
        handler->PSendSysMessage("Usage: .playerbots record <bot name|guid>");
        return true;
    }

    ObjectGuid guid = ObjectGuid::Empty;
    std::string name = target;
    if (std::optional<uint32> const number = Acore::StringTo<uint32>(target))
        guid = ObjectGuid::Create<HighGuid::Player>(*number);
    else if (normalizePlayerName(name))
        guid = sCharacterCache->GetCharacterGuidByName(name);

    CharacterCacheEntry const* character = guid.IsEmpty() ? nullptr : sCharacterCache->GetCharacterCacheByGuid(guid);
    if (!character)
    {
        handler->PSendSysMessage("No character {}.", target);
        return false;
    }

    std::vector<Row> rows = mgr.Rows(guid.GetCounter());
    std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) { return a.Time < b.Time; });
    std::size_t const permanent =
        static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), [](Row const& row) { return !row.Expires; }));
    handler->PSendSysMessage("Record of {}: {} rows, {} permanent", character->Name, rows.size(), permanent);
    for (Row const& row : rows)
        handler->PSendSysMessage("RECORD {} {} {} score {} {} {}", character->Name, row.EventId,
                                 RecordRules::Word(row.EventKind), static_cast<uint32>(row.Score),
                                 row.Expires ? "recent" : "permanent", ZoneName(row.Zone));

    return true;
}
