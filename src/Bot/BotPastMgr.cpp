/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotPastMgr.h"

#include <sstream>

#include "BotRecordMgr.h"
#include "BridgeBotState.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotsDatabase.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "ThreadCheck.h"

using PastRules::Colour;
using PastRules::Refusal;
using PastRules::Statement;

namespace
{
// A text for a quoted SQL literal (the pool keeps the connection's escape to itself; the texts are one line of UTF-8)
std::string Escaped(std::string const& text)
{
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (char const c : text)
    {
        if (c == '\\' || c == '\'' || c == '"')
            escaped.push_back('\\');
        if (c == '\0')
            escaped += "\\0";
        else if (c == '\x1a')
            escaped += "\\Z";
        else
            escaped.push_back(c);
    }
    return escaped;
}

std::string StatementWrite(uint32 guid, Statement const& statement)
{
    return Acore::StringFormat(
        "REPLACE INTO playerbots_bot_background (guid, ordinal, text, version, shared_id, source) VALUES ({}, {}, "
        "'{}', {}, {}, {})",
        guid, static_cast<uint32>(statement.Ordinal), Escaped(statement.Text), static_cast<uint32>(statement.Version),
        statement.SharedId, static_cast<uint32>(statement.From));
}

std::string ColourWrite(uint32 guid, Colour const& colour)
{
    return Acore::StringFormat(
        "REPLACE INTO playerbots_bot_colour (event_id, guid, text, version) VALUES ({}, {}, '{}', {})", colour.EventId,
        guid, Escaped(colour.Text), static_cast<uint32>(colour.Version));
}

// The bot's row of the event: its score; nullopt for none
std::optional<uint8> RowScore(uint32 guid, uint32 eventId)
{
    for (RecordRules::Row const& row : BotRecordMgr::Instance().Rows(guid))
        if (row.EventId == eventId)
            return row.Score;
    return std::nullopt;
}

// The console's event id 0: the bot's newest row at or above the high mark (0: none)
uint32 NewestPermanent(uint32 guid)
{
    uint32 eventId = 0;
    uint32 time = 0;
    for (RecordRules::Row const& row : BotRecordMgr::Instance().Rows(guid))
        if (RecordRules::Permanent(row.Score) && (!eventId || row.Time >= time))
        {
            eventId = row.EventId;
            time = row.Time;
        }
    return eventId;
}

// The rest of the stream after the words read so far, without its leading blanks
std::string Rest(std::istringstream& in)
{
    std::string rest;
    std::getline(in, rest);
    std::size_t const first = rest.find_first_not_of(' ');
    return first == std::string::npos ? std::string() : rest.substr(first);
}

std::string Joined(std::vector<std::string> const& words)
{
    std::string joined;
    for (std::string const& word : words)
        joined += (joined.empty() ? "" : ",") + word;
    return joined.empty() ? "none" : joined;
}
}  // namespace

void BotPastMgr::Load()
{
    std::size_t statements = 0;
    std::size_t colours = 0;
    PastRules::Book book;
    // One query per table for all bots (startup, world thread)
    if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT guid, ordinal, text, version, shared_id, source FROM playerbots_bot_background ORDER BY guid, "
            "ordinal"))
    {
        do
        {
            Field* fields = result->Fetch();
            std::optional<PastRules::Source> const source = PastRules::SourceOf(fields[5].Get<uint8>());
            Statement statement{fields[1].Get<uint8>(), fields[2].Get<std::string>(), fields[3].Get<uint8>(),
                                fields[4].Get<uint32>(), source.value_or(PastRules::Source::Writer)};
            if (PastRules::Set(book.Statements[fields[0].Get<uint32>()], std::move(statement)) == Refusal::None)
                ++statements;
        } while (result->NextRow());
    }

    if (QueryResult result =
            PlayerbotsDatabase.Query("SELECT guid, event_id, text, version FROM playerbots_bot_colour ORDER BY guid"))
    {
        do
        {
            Field* fields = result->Fetch();
            std::vector<Colour>& list = book.Colours[fields[0].Get<uint32>()];
            Colour colour{fields[1].Get<uint32>(), fields[2].Get<std::string>(), fields[3].Get<uint8>()};
            // Loaded as stored: the rows passed their checks when they were written
            auto const at = std::lower_bound(list.begin(), list.end(), colour.EventId,
                                             [](Colour const& held, uint32 id) { return held.EventId < id; });
            list.insert(at, std::move(colour));
            ++colours;
        } while (result->NextRow());
    }

    std::lock_guard<std::mutex> guard(_lock);
    _book = std::move(book);
    LOG_INFO("playerbots", "Bot past loaded: {} statements of {} bots, {} colour lines of {} bots", statements,
             _book.Statements.size(), colours, _book.Colours.size());
}

std::vector<Statement> BotPastMgr::Statements(uint32 guid)
{
    std::lock_guard<std::mutex> guard(_lock);
    auto const found = _book.Statements.find(guid);
    return found == _book.Statements.end() ? std::vector<Statement>() : found->second;
}

std::vector<Colour> BotPastMgr::Colours(uint32 guid)
{
    std::lock_guard<std::mutex> guard(_lock);
    auto const found = _book.Colours.find(guid);
    return found == _book.Colours.end() ? std::vector<Colour>() : found->second;
}

Refusal BotPastMgr::SetStatement(uint32 guid, Statement statement)
{
    std::string const write = StatementWrite(guid, statement);
    Refusal refusal = Refusal::None;
    {
        std::lock_guard<std::mutex> guard(_lock);
        refusal = PastRules::Set(_book.Statements[guid], std::move(statement));
    }
    if (refusal == Refusal::None)
        PlayerbotsDatabase.Execute(write);
    return refusal;
}

Refusal BotPastMgr::Revise(uint32 guid, uint8 ordinal, uint8 version, std::string text)
{
    std::string const write = Acore::StringFormat(
        "UPDATE playerbots_bot_background SET text = '{}', version = {} WHERE guid = {} AND "
        "ordinal = {}",
        Escaped(text), static_cast<uint32>(version), guid, static_cast<uint32>(ordinal));
    Refusal refusal = Refusal::None;
    {
        std::lock_guard<std::mutex> guard(_lock);
        auto const found = _book.Statements.find(guid);
        refusal = found == _book.Statements.end() ? Refusal::Missing
                                                  : PastRules::Revise(found->second, ordinal, version, std::move(text));
    }
    if (refusal == Refusal::None)
        PlayerbotsDatabase.Execute(write);
    return refusal;
}

Refusal BotPastMgr::SetColour(uint32 guid, Colour colour)
{
    PLAYERBOT_CHECK_WORLD_THREAD("BotPastMgr::SetColour");
    std::optional<uint8> const score = RowScore(guid, colour.EventId);
    std::string const write = ColourWrite(guid, colour);
    Refusal refusal = Refusal::None;
    {
        std::lock_guard<std::mutex> guard(_lock);
        refusal = PastRules::SetColour(_book.Colours[guid], std::move(colour), score);
    }
    if (refusal == Refusal::None)
        PlayerbotsDatabase.Execute(write);
    return refusal;
}

void BotPastMgr::Clear(uint32 guid)
{
    PLAYERBOT_CHECK_WORLD_THREAD("BotPastMgr::Clear");
    {
        std::lock_guard<std::mutex> guard(_lock);
        _book.Forget(guid);
    }
    PlayerbotsDatabase.Execute(Acore::StringFormat("DELETE FROM playerbots_bot_background WHERE guid = {}", guid));
    PlayerbotsDatabase.Execute(Acore::StringFormat("DELETE FROM playerbots_bot_colour WHERE guid = {}", guid));
}

void BotPastMgr::Forget(uint32 guid)
{
    PLAYERBOT_CHECK_WORLD_THREAD("BotPastMgr::Forget");
    std::lock_guard<std::mutex> guard(_lock);
    _book.Forget(guid);
}

bool BotPastMgr::HandlePastCommand(ChatHandler* handler, char const* args)
{
    std::istringstream in(args ? args : "");
    std::string target;
    in >> target;
    BotPastMgr& mgr = Instance();
    if (target.empty())
    {
        std::size_t statements = 0;
        std::size_t colours = 0;
        std::size_t bots = 0;
        {
            std::lock_guard<std::mutex> guard(mgr._lock);
            bots = mgr._book.Statements.size();
            for (auto const& [guid, list] : mgr._book.Statements)
                statements += list.size();
            for (auto const& [guid, list] : mgr._book.Colours)
                colours += list.size();
        }
        handler->PSendSysMessage("Bot past: {} statements of {} bots, {} colour lines", statements, bots, colours);
        handler->PSendSysMessage(
            "Usage: .playerbots past <bot name|guid> [set <n> <text> | revise <n> <version> "
            "<text> | colour <event id> <text> | event <event id> | clear]; event id 0: the newest permanent row");
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

    uint32 const counter = guid.GetCounter();
    std::string verb;
    in >> verb;
    auto const answer = [handler, character, &verb](Refusal refusal, std::string const& what)
    {
        if (refusal == Refusal::None)
            handler->PSendSysMessage("PAST {} {} {}", character->Name, verb, what);
        else
            handler->PSendSysMessage("PAST {} {} {} refused {}", character->Name, verb, what,
                                     PastRules::RefusalWord(refusal));
        return refusal == Refusal::None;
    };
    if (verb == "set" || verb == "revise" || verb == "colour" || verb == "event")
    {
        uint32 number = 0;
        uint32 version = PastRules::RULES_VERSION;
        in >> number;
        if (verb == "revise")
            in >> version;
        bool const ordinal = verb == "set" || verb == "revise";
        if (!in || (ordinal && number > UINT8_MAX) || version > UINT8_MAX)
        {
            handler->PSendSysMessage("PAST {} {} refused number", character->Name, verb);
            return false;
        }

        if (!ordinal && !number)
            number = NewestPermanent(counter);  // tests: the row a fixture just made

        std::string const text = Rest(in);
        if (verb == "set")
            return answer(mgr.SetStatement(counter, {static_cast<uint8>(number), text, PastRules::RULES_VERSION, 0,
                                                     PastRules::Source::Writer}),
                          std::to_string(number));
        if (verb == "revise")
            return answer(mgr.Revise(counter, static_cast<uint8>(number), static_cast<uint8>(version), text),
                          std::to_string(number));
        if (verb == "colour")
            return answer(mgr.SetColour(counter, {number, text, PastRules::RULES_VERSION}), std::to_string(number));

        // past.event (LC22a's input), as the bridge reads it
        PlayerbotBridge::PastEvent event;
        if (!PlayerbotBridge::ReadPastEvent(guid, number, event))
        {
            handler->PSendSysMessage("PAST {} event {} refused missing", character->Name, number);
            return false;
        }
        handler->PSendSysMessage("EVENT {} {} {} {} {}", character->Name, event.eventId, event.kind, event.daysAgo,
                                 event.text);
        for (PlayerbotBridge::PastParticipant const& who : event.participants)
            handler->PSendSysMessage("PARTICIPANT {} {} traits {} colour {}", event.eventId, who.name,
                                     Joined(who.traits), who.colour.empty() ? "none" : who.colour);
        return true;
    }
    if (verb == "clear")
    {
        mgr.Clear(counter);
        handler->PSendSysMessage("PAST {} clear", character->Name);
        return true;
    }
    if (!verb.empty())
    {
        handler->PSendSysMessage("Unknown verb {}.", verb);
        return false;
    }

    // "PAST <name> <n> v<version> <source> shared <id> <text>", then the history topic's major rows:
    // "HISTORY <name> <event id> <kind word> coloured <line>" or "HISTORY <name> <event id> <kind word> uncoloured"
    std::vector<Statement> const statements = mgr.Statements(counter);
    std::vector<Colour> const colours = mgr.Colours(counter);
    handler->PSendSysMessage("Past of {}: {} statements, {} colour lines", character->Name, statements.size(),
                             colours.size());
    for (Statement const& statement : statements)
        handler->PSendSysMessage("PAST {} {} v{} {} shared {} {}", character->Name,
                                 static_cast<uint32>(statement.Ordinal), static_cast<uint32>(statement.Version),
                                 PastRules::SourceWord(statement.From), statement.SharedId, statement.Text);

    std::vector<PlayerbotBridge::BotStateItem> items;
    if (!PlayerbotBridge::ReadBotStateTopic(guid, PlayerbotBridge::BotStateTopic::History, items))
    {
        handler->PSendSysMessage("HISTORY {} offline", character->Name);
        return true;
    }
    for (PlayerbotBridge::BotStateItem const& item : items)
        if (item.event)
            handler->PSendSysMessage("HISTORY {} {} {} {}{}", character->Name, item.event, item.name,
                                     item.colour.empty() ? "uncoloured" : "coloured ", item.colour);
    return true;
}
